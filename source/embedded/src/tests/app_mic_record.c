/**
 * @file app_mic_record.c
 * @brief VAD recording: detect voice -> record -> play back (one round).
 *
 * AEC is enabled via CONFIG_ENABLE_AUDIO_AEC=y. The VAD is fed automatically
 * by the T5AI audio driver pipeline (tkl_ai_set_vad_aec_algorithm is called
 * inside tkl_vad_init()), so no explicit feed call is needed.
 *
 * Flow (one round, blocking):
 *   1. tkl_vad_init + tkl_vad_start
 *   2. app_speaker_stop()
 *   3. tdl_audio_open(mic_cb)
 *   4. Poll VAD until TKL_VAD_STATUS_SPEECH
 *   5. Record PCM into sg_mic_buf while VAD == SPEECH
 *   6. When VAD stays NONE for >= 2 s (or max 2 s reached), stop
 *   7. tdl_audio_close + tkl_vad_deinit
 *   8. app_speaker_init() to restore the speaker pipeline
 *   9. Drain the ring-buffer prime silence
 *  10. Feed recorded audio into the speaker ring buffer via app_speaker_write()
 *
 * Bring-up debug additions (see app_mic_probe.h):
 *   - the mic callback reports a peak level, so the log shows whether the
 *     microphone is actually picking up sound while we wait for the VAD;
 *   - the "wait for voice" loop is BOUNDED (MR_VAD_WAIT_TIMEOUT_MS) and prints
 *     the VAD state once a second, so a non-triggering VAD produces evidence
 *     instead of an endless hang.
 */

#include "tal_api.h"
#include "tdl_audio_manage.h"
#include "tkl_vad.h"

#include "app_speaker.h"
#include "app_mic_record.h"
#include "app_mic_probe.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define MR_SAMPLE_RATE      16000
#define MR_CHANNELS         1
#define MR_BYTES_PER_SAMP   2           /* 16-bit */
#define MR_CHUNK_MS         20

/* VAD */
#define MR_VAD_SPEECH_MS    200
#define MR_VAD_NOISE_MS     500
#define MR_VAD_POLL_MS      50

/* Silence timeout: stop recording after 2 seconds of VAD_NONE */
#define MR_SILENCE_TIMEOUT_MS   2000

/* Max clip length: 2 s */
#define MR_MAX_CLIP_MS      2000

/* Bring-up: 等不到人声就放弃，别把整个自检卡死在这里 */
#define MR_VAD_WAIT_TIMEOUT_MS  20000

/* PCM buffer (2 s x 16 kHz x 2 bytes = 64 kB) —— 走 PSRAM，别占 SRAM */
#define MR_BUF_SAMPLES      ((uint32_t)((uint64_t)MR_SAMPLE_RATE * \
                                        MR_CHANNELS * MR_MAX_CLIP_MS / 1000))
#define MR_BUF_BYTES        (MR_BUF_SAMPLES * MR_BYTES_PER_SAMP)
#define MR_CHUNK_BYTES      ((uint32_t)(MR_SAMPLE_RATE * MR_CHANNELS * \
                                        MR_BYTES_PER_SAMP * MR_CHUNK_MS / 1000))

/***********************************************************
***********************variable define**********************
***********************************************************/
static int16_t         *sg_mic_buf    = NULL;   /* MR_BUF_SAMPLES samples, PSRAM */
static volatile uint32_t sg_mic_written = 0;
static APP_MIC_PROBE_T   sg_probe = {0};

/***********************************************************
***********************function define**********************
***********************************************************/

/* Mic data callback — copies PCM into buffer until full.
 *
 * NOTE: VAD is fed automatically by the audio driver pipeline:
 * tkl_vad_init() registers __tkl_aec_vad_process via
 * tkl_ai_set_vad_aec_algorithm(), which is called for every audio
 * frame. No explicit tkl_vad_feed() needed on this platform. */
static void prv_mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                       uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;

    uint32_t pos = sg_mic_written;

    if (pos + len > MR_BUF_BYTES) {
        len = MR_BUF_BYTES - pos;
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
    }
}

/***********************************************************
************************public api**************************
***********************************************************/
OPERATE_RET app_mic_record_once(void)
{
    OPERATE_RET        rt;
    OPERATE_RET        result = OPRT_OK;
    TDL_AUDIO_HANDLE_T hdl = NULL;
    TKL_VAD_CONFIG_T   vad_cfg;
    TKL_VAD_STATUS_T   vad_state = TKL_VAD_STATUS_NONE;
    uint32_t           silence_ms = 0;
    uint32_t           clip_ms    = 0;
    uint32_t           waited_ms  = 0;
    BOOL_T             capture_open = FALSE;
    BOOL_T             got_speech   = FALSE;

    PR_NOTICE("[mic_vad] === one-round VAD recording ====");

    if (NULL == sg_mic_buf) {
        sg_mic_buf = (int16_t *)tal_psram_malloc(MR_BUF_BYTES);
        if (NULL == sg_mic_buf) {
            PR_ERR("[mic_vad] PSRAM 分配 %u 字节失败", (unsigned)MR_BUF_BYTES);
            return OPRT_MALLOC_FAILED;
        }
    }

    /* ---- Init VAD first (needs to be running before mic opens) ---- */
    memset(&vad_cfg, 0, sizeof(vad_cfg));
    vad_cfg.sample_rate       = MR_SAMPLE_RATE;
    vad_cfg.channel_num       = MR_CHANNELS;
    vad_cfg.speech_min_ms     = MR_VAD_SPEECH_MS;
    vad_cfg.noise_min_ms      = MR_VAD_NOISE_MS;
    vad_cfg.frame_duration_ms = MR_CHUNK_MS;
    vad_cfg.scale             = 1.0f;

    rt = tkl_vad_init(&vad_cfg);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_vad] tkl_vad_init failed (%d)", rt);
        return rt;
    }
    tkl_vad_start();

    /* ---- Stop speaker ---- */
    app_speaker_stop();

    /* ---- Open audio for capture ---- */
    rt = tdl_audio_find(AUDIO_CODEC_NAME, &hdl);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_vad] audio device '%s' not found (%d)",
               AUDIO_CODEC_NAME, rt);
        result = rt;
        goto cleanup;
    }

    sg_mic_written   = 0;
    sg_probe.frames  = 0;
    sg_probe.bytes   = 0;
    sg_probe.peak    = 0;

    rt = tdl_audio_open(hdl, prv_mic_cb);
    if (OPRT_OK != rt) {
        PR_ERR("[mic_vad] open with mic_cb failed (%d)", rt);
        result = rt;
        goto cleanup;
    }
    capture_open = TRUE;

    /* ---- Wait for voice (bounded, with a 1 Hz status line) ---- */
    PR_NOTICE("[mic_vad] waiting for voice ... (timeout %u ms)",
              (unsigned)MR_VAD_WAIT_TIMEOUT_MS);

    while (TKL_VAD_STATUS_SPEECH != vad_state) {
        tal_system_sleep(MR_VAD_POLL_MS);
        waited_ms += MR_VAD_POLL_MS;
        vad_state = tkl_vad_get_status();

        if ((waited_ms % 1000u) == 0u) {
            PR_NOTICE("[mic_vad] 等待人声 %u ms: vad=%u, 最近电平 peak=%d (max=%d)",
                      (unsigned)waited_ms, (unsigned)vad_state,
                      (int)sg_probe.last_peak, (int)sg_probe.peak);
        }

        if (waited_ms >= MR_VAD_WAIT_TIMEOUT_MS) {
            PR_ERR("[mic_vad] 等待人声超时 %u ms，VAD 全程未触发（max peak=%d）",
                   (unsigned)waited_ms, (int)sg_probe.peak);
            result = OPRT_TIMEOUT;
            goto cleanup;
        }
    }

    got_speech = TRUE;
    PR_NOTICE("[mic_vad] VAD 触发（等了 %u ms），开始录音", (unsigned)waited_ms);

    /* ---- Start recording ---- */
    sg_mic_written = 0;
    silence_ms = 0;
    clip_ms    = 0;

    /* ---- Record while speaking, stop after 2 s silence ---- */
    while (clip_ms < MR_MAX_CLIP_MS && sg_mic_written < MR_BUF_BYTES) {
        tal_system_sleep(MR_VAD_POLL_MS);
        clip_ms += MR_VAD_POLL_MS;

        vad_state = tkl_vad_get_status();

        if (TKL_VAD_STATUS_NONE == vad_state) {
            silence_ms += MR_VAD_POLL_MS;
            if (silence_ms >= MR_SILENCE_TIMEOUT_MS) {
                PR_NOTICE("[mic_vad] silence %u ms reached, stopping",
                          (unsigned)silence_ms);
                break;
            }
        } else {
            silence_ms = 0;
        }
    }

    /* ---- Done recording ---- */
    PR_NOTICE("[mic_vad] captured %u bytes (%u ms), peak=%d",
              (unsigned)sg_mic_written, (unsigned)clip_ms, (int)sg_probe.peak);

cleanup:
    /* ---- Stop VAD + close capture ---- */
    tkl_vad_stop();
    tkl_vad_deinit();
    if (capture_open) {
        tdl_audio_close(hdl);
        capture_open = FALSE;
    }
    PR_NOTICE("[mic_vad] capture stopped");

    /* ---- Restore speaker BEFORE playback ----
     *
     * This reuses the same speaker pipeline the game uses, avoiding the
     * extra tdl_audio_open/play/close cycle that was leaving the audio codec
     * in a broken state (bk_voice_write_frame_data returning 0 = DAC stuck). */
    rt = app_speaker_init();
    if (OPRT_OK != rt) {
        PR_ERR("[mic_vad] app_speaker_init() failed (%d)", rt);
        return rt;
    }

    if (!got_speech) {
        PR_ERR("[mic_vad] 本轮没有检测到人声，跳过回放");
        return result;
    }

    /* ---- Drain the prime silence ----
     *
     * app_speaker_init() primes ~200 ms of silence into the ring buffer
     * to cushion game audio jitter.  For one-shot playback that cushion is
     * unwanted latency, so wait until the drain task has flushed it. */
    {
        uint32_t rate = app_speaker_rate();
        if (0 == rate) { rate = 16000; }
        /* Ring-buffer total = rate * 2 * SPK_RB_MS(400) / 1000 */
        uint32_t rb_total = rate * 2 * 400 / 1000;
        uint32_t waited  = 0;
        while (app_speaker_room() < rb_total - rate * 2 * 50 / 1000) {
            tal_system_sleep(5);
            waited += 5;
            if (waited > 500) { break; }   /* safety timeout */
        }
    }

    /* ---- Write recorded audio into the speaker ring buffer ----
     *
     * The ring buffer caps at ~400 ms (12.8 KB), so feed it in chunks and
     * wait for the drain task to make room. */
    if (sg_mic_written > 0) {
        PR_NOTICE("[mic_vad] playback via speaker: %u bytes ...",
                  (unsigned)sg_mic_written);

        uint32_t written = 0;
        while (written < sg_mic_written) {
            uint32_t avail = app_speaker_room();
            if (avail >= MR_CHUNK_BYTES) {
                uint32_t chunk = sg_mic_written - written;
                if (chunk > MR_CHUNK_BYTES) {
                    chunk = MR_CHUNK_BYTES;
                }
                app_speaker_write(
                    (const int16_t *)((const uint8_t *)sg_mic_buf + written),
                    chunk / 2);
                written += chunk;
            } else {
                /* Ring buffer full — yield for the drain task */
                tal_system_sleep(MR_VAD_POLL_MS);
            }
        }
        PR_NOTICE("[mic_vad] playback done (via speaker ringbuf)");
    }

    return OPRT_OK;
}
