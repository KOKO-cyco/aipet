/**
 * @file app_mic_test.c
 * @brief Microphone loopback test: record 2s, wait 1s, play back.
 *
 * Flow:
 *   1. app_speaker_stop()            — release the audio device
 *   2. tdl_audio_find / open(mic_cb) — re-open with mic capture
 *   3. Wait 2 seconds while the callback fills sg_mic_buf
 *   4. tdl_audio_close()             — stop capture
 *   5. tal_system_sleep(1000)        — 1-second gap
 *   6. tdl_audio_open(NULL)          — re-open for playback only
 *   7. tdl_audio_play() in 20ms chunks — play back recorded PCM
 *   8. tdl_audio_close()
 *   9. app_speaker_init()            — restore normal speaker path
 *
 * Dependencies: tal_api.h, tdl_audio_manage.h, app_speaker.h.
 * The audio device name comes from the Kconfig (AUDIO_CODEC_NAME).
 *
 * NOTE (bring-up debug): every 20 frames the callback prints a peak level so the
 * log can tell "mic captures real audio" from "mic delivers silence". See
 * app_mic_probe.h. Apart from those prints the test is unchanged.
 */

#include "tal_api.h"
#include "tdl_audio_manage.h"

#include "app_speaker.h"
#include "app_mic_test.h"
#include "app_mic_probe.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define MIC_SAMPLE_RATE     16000
#define MIC_CHANNELS        1           /* mono */
#define MIC_BYTES_PER_SAMP  2           /* 16-bit */
#define MIC_RECORD_MS       2000
#define MIC_PAUSE_MS        1000
#define MIC_CHUNK_MS        20          /* tdd_audio PCM frame period */

/* Total buffer length in bytes */
#define MIC_BUF_BYTES       ((uint32_t)(MIC_SAMPLE_RATE * MIC_CHANNELS * \
                                        MIC_BYTES_PER_SAMP * MIC_RECORD_MS / 1000))

/* Bytes per callback invocation */
#define MIC_CHUNK_BYTES     ((uint32_t)(MIC_SAMPLE_RATE * MIC_CHANNELS * \
                                        MIC_BYTES_PER_SAMP * MIC_CHUNK_MS / 1000))

/* 每多少帧打一条电平日志（20ms/帧 -> 20 帧 = 400ms） */
#define MIC_PROBE_EVERY     20

/***********************************************************
***********************variable define**********************
***********************************************************/
/* PCM 缓冲必须放 PSRAM，不能用静态数组。
 *
 * 这里原来是 `static int16_t sg_mic_buf[MIC_BUF_BYTES / 2]`（64 KB）。
 * 本工程开机后 SRAM 只剩 ~120 KB，静态吃掉 64 KB 后只剩 ~57 KB，
 * 后果是音频管线建线程直接失败：
 *     [aec_alg] audio_thread_create failed
 *     -> tkl_ai_start voice start fail -> tdl_audio_open() 返回 -1
 * 连应用自己的 app_audio_init() 都打不开扬声器。
 * 判据：日志里 "app startup, left sram" 从 123024 掉到 58952。 */
static int16_t         *sg_mic_buf = NULL;       /* 32000 samples = 64 KB @ PSRAM */
static volatile uint32_t sg_mic_written = 0;     /* bytes written by callback */
static APP_MIC_PROBE_T   sg_probe = {0};

/***********************************************************
***********************function define**********************
***********************************************************/

/* Mic data callback — runs in (or under) the TKL audio ISR/thread.
 * Just copy the PCM frame and advance the write cursor. Stop recording
 * when the buffer is full. */
static void prv_mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                       uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;

    uint32_t pos = sg_mic_written;

    if (pos + len > MIC_BUF_BYTES) {
        len = MIC_BUF_BYTES - pos;
    }
    if (len > 0) {
        memcpy((uint8_t *)sg_mic_buf + pos, data, len);
        sg_mic_written = pos + len;
    }

    /* --- bring-up probe: 电平 --- */
    {
        int32_t pk = app_mic_probe_peak(data, len);

        sg_probe.frames++;
        sg_probe.bytes += len;
        sg_probe.last_peak = pk;
        if (pk > sg_probe.peak) {
            sg_probe.peak = pk;
        }
        if ((sg_probe.frames % MIC_PROBE_EVERY) == 0) {
            PR_NOTICE("[mic_test] cb#%u peak=%d max=%d bytes=%u",
                      (unsigned)sg_probe.frames, (int)sg_probe.last_peak,
                      (int)sg_probe.peak, (unsigned)sg_mic_written);
        }
    }
}

/***********************************************************
***********************public api***************************
***********************************************************/
OPERATE_RET app_mic_test_start(void)
{
    OPERATE_RET rt;
    TDL_AUDIO_HANDLE_T hdl = NULL;
    uint32_t wait_ms;
    int32_t i, chunks;

    PR_NOTICE("[mic_test] === start: record %ums, pause %ums, play ===",
              (unsigned)MIC_RECORD_MS, (unsigned)MIC_PAUSE_MS);

    if (NULL == sg_mic_buf) {
        sg_mic_buf = (int16_t *)tal_psram_malloc(MIC_BUF_BYTES);
        if (NULL == sg_mic_buf) {
            PR_ERR("[mic_test] PSRAM 分配 %u 字节失败", (unsigned)MIC_BUF_BYTES);
            return OPRT_MALLOC_FAILED;
        }
    }

    /* ---- Step 1: stop the normal speaker (closes the audio device) ---- */
    app_speaker_stop();
    PR_NOTICE("[mic_test] speaker stopped");

    /* ---- Step 2: find and open the audio device WITH mic callback ---- */
    rt = tdl_audio_find(AUDIO_CODEC_NAME, &hdl);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_test] audio device '%s' not found (%d)", AUDIO_CODEC_NAME, rt);
        goto restart_speaker;
    }

    sg_mic_written  = 0;
    sg_probe.frames = 0;
    sg_probe.bytes  = 0;
    sg_probe.peak   = 0;

    rt = tdl_audio_open(hdl, prv_mic_cb);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_test] open with mic_cb failed (%d)", rt);
        goto restart_speaker;
    }
    PR_NOTICE("[mic_test] mic capture started (%u bytes buffer = %ums @ 16kHz mono)",
              (unsigned)MIC_BUF_BYTES, (unsigned)MIC_RECORD_MS);

    /* ---- Step 3: wait until 2 seconds of PCM have accumulated ---- */
    wait_ms = MIC_RECORD_MS;
    while (wait_ms > 0 && sg_mic_written < MIC_BUF_BYTES) {
        tal_system_sleep(MIC_CHUNK_MS);
        wait_ms -= (wait_ms > MIC_CHUNK_MS) ? MIC_CHUNK_MS : wait_ms;
    }

    PR_NOTICE("[mic_test] recording done: %u bytes captured, %u frames, peak=%d",
              (unsigned)sg_mic_written, (unsigned)sg_probe.frames,
              (int)sg_probe.peak);

    /* ---- Step 4: close (stops AI capture) ---- */
    tdl_audio_close(hdl);
    PR_NOTICE("[mic_test] capture stopped");

    /* ---- Step 5: wait 1 second ---- */
    PR_NOTICE("[mic_test] pause %ums ...", (unsigned)MIC_PAUSE_MS);
    tal_system_sleep(MIC_PAUSE_MS);

    /* ---- Step 6: re-open for playback only (no mic callback) ---- */
    rt = tdl_audio_open(hdl, NULL);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_test] open for playback failed (%d)", rt);
        goto restart_speaker;
    }

    /* Set volume to a comfortable level */
    tdl_audio_volume_set(hdl, 80);
    PR_NOTICE("[mic_test] playback starting: %u bytes ...",
              (unsigned)sg_mic_written);

    /* ---- Step 7: play back in MIC_CHUNK_MS-sized blocks ----
     *
     * tdl_audio_play() is blocking, so we split the buffer into small
     * chunks. This also keeps the audio DMA fed without over-committing
     * a single frame. */
    chunks = (int32_t)(sg_mic_written / MIC_CHUNK_BYTES);
    for (i = 0; i < chunks; i++) {
        tdl_audio_play(hdl, (uint8_t *)sg_mic_buf + (uint32_t)(i * MIC_CHUNK_BYTES),
                       MIC_CHUNK_BYTES);
    }
    /* Trailing partial chunk */
    uint32_t tail = sg_mic_written % MIC_CHUNK_BYTES;
    if (tail > 0) {
        tdl_audio_play(hdl, (uint8_t *)sg_mic_buf + (uint32_t)(chunks * MIC_CHUNK_BYTES),
                       tail);
    }

    PR_NOTICE("[mic_test] playback done");

    /* ---- Step 8: close ---- */
    tdl_audio_close(hdl);

    /* ---- Step 9: restart normal speaker ---- */
restart_speaker:
    rt = app_speaker_init();
    if (OPRT_OK != rt) {
        PR_ERR("[mic_test] app_speaker_init() failed (%d)", rt);
        return rt;
    }
    PR_NOTICE("[mic_test] === finished (peak=%d) ===", (int)sg_probe.peak);
    return OPRT_OK;
}
