/**
 * @file app_speaker.c
 * @brief Onboard speaker playback: ring buffer + dedicated drain task.
 *
 * See app_speaker.h for the contract. In short:
 *   - the producer side (app_speaker_write) never blocks, so it is safe to
 *     call from a real-time emulator audio callback;
 *   - tdl_audio_play() blocks, so it only ever runs in the drain task.
 *
 * Audio device: registered by the board (tuya_t5ai_board.c
 * __board_register_audio() -> tdd_audio_register(AUDIO_CODEC_NAME, cfg),
 * speaker at 16kHz, spk_en on GPIO28 handled by the driver). This module must
 * NOT touch spk_en itself -- doing so fights the board driver.
 */

#include "tal_api.h"
#include "tdl_audio_manage.h"
#include "tuya_ringbuf.h"

/* BK 底层 DAC 驱动（t5_os/ap/include/driver/aud_dac.h）。和 app_storage.c 引
 * driver/sd_card.h 是同一套路：应用侧可以直连 BK 的板级驱动。 */
#include "driver/aud_dac.h"

#include "app_speaker.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* Fallback if tdl_audio_get_info() reports nothing. */
#define SPK_FALLBACK_RATE  16000

#define SPK_VOLUME         60      /* codec 音量（0..100）。只当"非静音"的档位用
                                    * —— 真正的响度见下面的 DAC 增益。 */

/* 响度旋钮 = **DAC 增益寄存器**（bk_aud_dac_set_gain，0x00..0x3F）。
 *
 * 它是 **dB 刻度，不是线性幅度** —— 寄存器注释在
 * ap/middleware/soc/bk7258_ap/hal/aud_ll_macro_def.h：
 *     "dac_set_gain:0x7[23:18], DAC 设置的增益 -45db ~18db  0x2d = 0db"
 * 即 **1 个计数 = 1dB**：0x00 = -45dB，0x2D(45) = 0dB，0x3F(63) = +18dB。
 *
 * 所以老代码的 `SPK_DAC_GAIN = 54` 并不是注释里写的"86% 量程"，而是 **+9dB 的
 * 硬件放大** —— 这正是"音量拨到 1 还是吵"的根因：软件那边已经压到 1%(-40dB)，
 * 硬件又补回来 9dB，净下来还有 -31dB。
 *
 * 现在整个响度交给这一个寄存器（它本来就是干这个的），滑块直接映射成 dB：
 *     滑块 1   -> 0x00 (-45dB，几乎听不见)
 *     滑块 100 -> 0x36 (+9dB，与改之前的最大响度**完全一致**)
 * 满量程 54dB、每格 0.54dB。**均匀的 dB 步进正是音量旋钮该有的手感** —— 老曲线
 * 是"顶部 2%/格"，所以 38->50 只差 3dB，真机上听不出区别。
 *
 * 于是**软件增益整个不做**了（原来那条 `prv_apply_gain` 已删）：响度全在硬件上，
 * PCM 一路 bit-exact 直通，小音量下"16bit 只剩 7bit 有效精度、沙沙的"那个毛病
 * 一并消失。音质只升不降。
 *
 * 觉得最大音量破音就把它降到 45（0x2D = 0dB，不再做任何放大）。 */
#define SPK_DAC_GAIN_MAX   54      /* 0x36 = +9dB（= 0x2D 的 0dB 再加 9） */

/* Ring buffer depth in ms of mono PCM.
 *
 * The producer (the emulator) delivers one frame's worth of PCM in a burst once
 * per frame, while the DAC drains steadily, so this buffer is what absorbs the
 * frame-to-frame jitter. At 16kHz mono that is 32 bytes per ms, and the
 * emulator's frame period is ~16.6ms (~530 bytes), so 400ms costs only 12.8KB
 * of PSRAM for a lot of slack. */
#define SPK_RB_MS          400

/* One tdl_audio_play() call; matches the driver's frame_time_ms. */
#define SPK_CHUNK_MS       20

/* 驱动侧延迟的**兜底**值（毫秒）：tkl_ao_put_frame 每次最多往
 * bk_voice_write_frame_data 递 DRIVER_SPEAK_FIFO_FRAME_NUM(2) x TIME_SAMPLE_MS(20)
 * = 40ms 一块（platform/T5AI/tuyaos/tuyaos_adapter/src/driver/tkl_audio.c）。
 *
 * 注意：**这只是分包粒度，不是驱动侧的真实延迟**。2026-09-24 真机实测，平台在
 * tkl_ao_put_frame 之下还压着约 1.0 秒的播放队列（voice write pool +
 * onboard speaker stream 的 pool，pool_length 默认值在预编译库里，头文件看不到），
 * 40ms 连零头都不够 —— 当初就是把这个常量当成延迟，才让画面恒定领先声音 1 秒。
 * 真实值由 prv_drv_q_tick()/prv_drv_q_push() 在运行期实测，这个常量只在
 * 一次都没递交过时兜底。 */
#define SPK_DRV_FIFO_FRAMES 2

/* Silence written at init so the drain task has a cushion from the start; see
 * the priming block in app_speaker_init(). Half the ring buffer. */
#define SPK_PRIME_MS       200

/* 静音后到还原之间的余量（毫秒）。水位折算本身有误差，多等一拍再恢复，
 * 免得在尾巴上放出一小段上一首的声音。 */
#define SPK_UNMUTE_MARGIN_MS 80

#define SPK_TASK_STACK     4096
#define SPK_TASK_PRIO      THREAD_PRIO_3

/***********************************************************
***********************variable define**********************
***********************************************************/
static TDL_AUDIO_HANDLE_T sg_hdl   = NULL;
static TDL_AUDIO_INFO_T   sg_info  = {0};
static TUYA_RINGBUFF_T    sg_rb    = NULL;
static THREAD_HANDLE      sg_task  = NULL;
static BOOL_T             sg_ready = FALSE;

/* 生命周期代数：init/stop 各 +1。drain 任务开工时抄一份，循环条件里比对，
 * 代数不同即自退 —— tal_thread_delete 只是异步标记（日志里"still running"
 * 会一直挂着），线程自己必须能退出，否则就是僵尸：suspend 后它还每 2ms
 * 轮询，resume 后更会变成第二个 drain 同时啃同一个环形缓冲。 */
static volatile uint32_t  sg_gen   = 0;
/* drain 任务退出握手：stop 里等它落地后再关设备/释放缓冲 */
static volatile BOOL_T    sg_task_gone = TRUE;   /* 还没起过任务 = 已经是"退出"状态 */

static uint32_t sg_dropped  = 0;
static uint32_t sg_underrun = 0;

/* 音量滑块（0..100）。**注意它只是"档位"，不参与混音** —— 响度全部由
 * prv_codec_volume() 折算成 DAC 增益寄存器写下去（见 SPK_DAC_GAIN_MAX）。
 *
 * 以前这里是软件增益（在 drain 路径上逐样本乘），已经删掉：那个做法在低音量下
 * 把 16bit 样本压成 7bit 有效精度，代价全落在音质上，而 DAC 增益本来就是为这件
 * 事准备的。 */
static uint8_t sg_vol = 100;

/* "立刻静音"的状态。
 *
 * 关设备（tdl_audio_close）也能立刻掐掉声音，但**这个音频设备是共享的**：AI 侧
 * 的采集（voice_read / 麦克风）挂在同一个 tdd_audio 上，close 会把 DMA 和 ISR
 * 的资源一起拆掉 —— 真机上就是一句 `Assert at: xQueueGiveFromISR`（ISR 往已经
 * 不存在的对象里 give）。所以"退出要马上没声音"改用硬件静音：
 *
 *   tdl_audio_volume_set(hdl, 0)   —— 已经递交到驱动 FIFO 的那约 1 秒也立刻哑
 *   sg_mute_until_ms               —— 到点再把音量还原成 SPK_VOLUME
 *
 * 还原时机按"静音那一刻的驱动侧水位"折算：那坨放完了才准恢复，否则会在恢复的
 * 瞬间把上一首的尾巴放出来。水位归零（sg_drv_q_ms == 0）也走同一条路。 */
static volatile BOOL_T   sg_muted         = FALSE;
static volatile uint32_t sg_mute_until_ms = 0;

/* 驱动侧（tkl_ao_put_frame 之下）此刻还压着多少毫秒没放出来。
 *
 * 为什么要实测、不能抄常量：那一级是**预编译库**（voice write pool /
 * onboard speaker stream 的 pool），头文件里 pool_length 是 0（"用默认"），
 * 真值只有库里知道；而且平台一开局会**一次吞掉整个队列深度**（表现为"2 秒内
 * 喂进去 3.46 秒"），所以它同时决定了"画面该等多久"。
 *
 * 怎么测（漏桶）：DAC 只要手上有数据就按 1x 往外吐（平台不会自己停），所以
 * 每圈先按流逝时间把水位减掉，递交时再加上去。驱动拒绝再收时水位就停在那儿，
 * 那个稳定水位**就是**驱动侧的真实延迟。这样连"暂停后队列被放空"也自动跟得上
 * ——不需要在任何地方硬编码这个数。 */
static uint32_t sg_drv_q_ms   = 0;      /* 当前水位 */
static uint32_t sg_drv_q_max  = 0;      /* 观测到的最大水位（日志用） */
static uint32_t sg_q_tick_ms  = 0;      /* 上次折算的时刻 */
static uint32_t sg_q_reported = 0;      /* 日志节流：已报到的水位 */

/* 20ms at 16kHz mono, with headroom in case a higher rate is reported. */
static uint8_t sg_chunk[SPK_CHUNK_MS * 48 * 2];

/***********************************************************
***********************function define**********************
***********************************************************/

/* 滑块 0..100 -> DAC 增益寄存器 0x00..SPK_DAC_GAIN_MAX（dB 刻度，见上面的说明）。
 * 0 不走这里：那是静音，由 prv_codec_volume(0) 交给 bk_aud_dac_mute()。
 *
 * 均匀的 dB 步进：每格 0.54dB。听感上"每一格都差不多响一点"正是音量旋钮该有的
 * 样子 —— 老曲线顶部 2%/格（约 0.17dB），所以从 38 拖到 50 只差 3dB。 */
static uint8_t prv_dac_gain(uint8_t vol)
{
    if (vol > 100u) {
        vol = 100u;
    }
    return (uint8_t)(((uint32_t)vol * SPK_DAC_GAIN_MAX) / 100u);
}

/* 设 codec 音量的**唯一入口**：先走 tdl 那层（它会记 play_volume、并在 0 时做硬件
 * mute），再把 DAC 增益按当前滑块顶上去 —— tkl 自己写的那份会被覆盖掉，它的压缩
 * 映射（30 以上只到 44/63）够不着量程两端，见 SPK_DAC_GAIN_MAX 的说明。
 *
 * 只覆盖非 0 的情况：0 是静音，tkl 那边走 bk_aud_dac_mute()，写增益没意义。
 * 反过来，任何一次 unmute / 重设音量都会把增益按压缩映射写回，所以**所有**
 * 设音量的地方都必须走这里，不能再直接调 tdl_audio_volume_set()。 */
static void prv_codec_volume(uint8_t vol)
{
    if (NULL == sg_hdl) {
        return;
    }
    tdl_audio_volume_set(sg_hdl, vol);
    if (0 != vol) {
        bk_aud_dac_set_gain(prv_dac_gain(sg_vol));
    }
}

/* ---- 驱动侧队列水位的漏桶 ---- 见 sg_drv_q_ms 的注释 --------------------- */

/* 每圈调用一次：把"上一次折算到现在"这段时间里 DAC 吃掉的部分减掉。 */
static void prv_drv_q_tick(void)
{
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    uint32_t dt  = now - sg_q_tick_ms;

    sg_q_tick_ms = now;
    if (0 == dt) {
        return;
    }
    sg_drv_q_ms = (sg_drv_q_ms > dt) ? (sg_drv_q_ms - dt) : 0u;

    /* 静音到点 / 驱动那坨放空了：把硬件音量还原。判断放在这里而不是写入路径上，
     * 是因为"新音频开始喂"不等于"旧音频放完了" —— 按写入恢复会把上一首的
     * 尾巴一起放出来。 */
    if (sg_muted && (0 == sg_drv_q_ms || now >= sg_mute_until_ms)) {
        prv_codec_volume(SPK_VOLUME);
        sg_muted = FALSE;
    }
}

/* 递交一块成功后调用：把这块加回水位，顺带把"涨到的新高"记下来。 */
static void prv_drv_q_push(uint32_t bytes)
{
    uint32_t rate = sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE;

    sg_drv_q_ms += (uint32_t)(((uint64_t)bytes * 1000u) / ((uint64_t)rate * 2u));

    if (sg_drv_q_ms > sg_drv_q_max) {
        sg_drv_q_max = sg_drv_q_ms;
        /* 填充期水位会一路涨到平台的队列深度，每涨 250ms 记一条：既不会刷屏，
         * 又能从日志看出它稳在哪 —— 那个稳态值就是驱动侧的真实延迟。 */
        if (sg_drv_q_max >= (sg_q_reported + 250u)) {
            sg_q_reported = sg_drv_q_max;
            PR_NOTICE("[spk] 驱动侧队列实测 %ums（上层必须按它扣掉，"
                      "否则画面/字幕会领先声音这么多）", (unsigned)sg_drv_q_max);
        }
    }
}

/* Drain task: the only place tdl_audio_play() is called, so blocking here is
 * harmless -- it just parks this task. Exits by itself when the generation
 * moves on (see sg_gen). */
static void prv_spk_task(void *arg)
{
    uint32_t chunk_bytes;
    uint32_t my_gen = sg_gen;

    (void)arg;

    chunk_bytes = (uint32_t)SPK_CHUNK_MS *
                  (sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE) *
                  2 /* 16-bit */ / 1000;
    if (chunk_bytes > sizeof(sg_chunk)) {
        chunk_bytes = sizeof(sg_chunk);
    }
    if (0 == chunk_bytes) {
        chunk_bytes = 640;
    }

    PR_NOTICE("[spk] drain task up, chunk=%u bytes", (unsigned)chunk_bytes);

    while (my_gen == sg_gen) {
        uint32_t used;

        prv_drv_q_tick();       /* 先折算掉这一圈的消耗，再决定喂什么 */
        used = tuya_ring_buff_used_size_get(sg_rb);

        if (used >= chunk_bytes) {
            uint32_t n = tuya_ring_buff_read(sg_rb, sg_chunk, chunk_bytes);

            if (n > 0) {
                tdl_audio_play(sg_hdl, sg_chunk, n);
                prv_drv_q_push(n);
            }
        } else if (used > 0) {
            /* Partial chunk: play what there is rather than waiting for a full
             * one, which would leave audible gaps between chunks. */
            uint32_t n = tuya_ring_buff_read(sg_rb, sg_chunk, used);

            if (n > 0) {
                tdl_audio_play(sg_hdl, sg_chunk, n);
                prv_drv_q_push(n);
            }
        } else {
            /* **环里空 ≠ DAC 断粮。**
             *
             * drain 任务每轮把环里所有数据都搬进驱动队列，所以稳态下环本来就
             * 常空 —— 真正的缓冲垫在驱动侧（sg_drv_q_ms）。尤其 NES：每帧只产
             * 16.6ms 的音频（16000/60.099 ≈ 266 样本），而一块是 20ms，环里
             * 根本攒不满，一取就空。
             *
             * 原来这里只判环空就计数，于是稳态下必然持续上涨（真机 NES 实测
             * ~900/s），看着像"音频一直在饿"，其实一个采样都没丢 —— 上面的
             * "slow steady climb with drop pinned at 0" 说的就是这个现象，
             * 当时按"补静音垫"治了症状，但计数口径没改。
             *
             * 只有**驱动队列也空了**才是真的断粮。 */
            if (0 == sg_drv_q_ms) {
                sg_underrun++;
            }
            tal_system_sleep(2);
        }
    }

    PR_NOTICE("[spk] drain task exit");
    sg_task_gone = TRUE;
}

/***********************************************************
***********************public api***************************
***********************************************************/
OPERATE_RET app_speaker_init(void)
{
    OPERATE_RET rt;
    THREAD_CFG_T tcfg;
    uint32_t rate;
    uint32_t rb_bytes;

    if (sg_ready) {
        return OPRT_OK;
    }

    rt = tdl_audio_find(AUDIO_CODEC_NAME, &sg_hdl);
    if (OPRT_OK != rt) {
        PR_ERR("[spk] audio device '%s' not found (%d)", AUDIO_CODEC_NAME, rt);
        return rt;
    }

    rt = tdl_audio_open(sg_hdl, NULL);      /* playback only */
    if (OPRT_OK != rt) {
        PR_ERR("[spk] open failed (%d)", rt);
        return rt;
    }

    if (OPRT_OK == tdl_audio_get_info(sg_hdl, &sg_info)) {
        PR_NOTICE("[spk] info: rate=%u ch=%u bits=%u tm=%ums frame=%u",
                  (unsigned)sg_info.sample_rate, (unsigned)sg_info.sample_ch_num,
                  (unsigned)sg_info.sample_bits, (unsigned)sg_info.sample_tm_ms,
                  (unsigned)sg_info.frame_size);
    } else {
        PR_WARN("[spk] get_info failed, assuming %uHz", (unsigned)SPK_FALLBACK_RATE);
    }

    rate = sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE;

    rb_bytes = rate * 2 /* mono 16-bit */ * SPK_RB_MS / 1000;
    rt = tuya_ring_buff_create(rb_bytes, OVERFLOW_PSRAM_STOP_TYPE, &sg_rb);
    if (OPRT_OK != rt) {
        PR_ERR("[spk] ringbuf create failed (%u bytes, %d)", (unsigned)rb_bytes, rt);
        return rt;
    }

    prv_codec_volume(SPK_VOLUME);
    PR_DEBUG("[spk] DAC 增益 %u/63（音量 %u -> %d dB；codec 音量 %u 只当非静音档）",
             (unsigned)prv_dac_gain(sg_vol), (unsigned)sg_vol,
             (int)prv_dac_gain(sg_vol) - 45, (unsigned)SPK_VOLUME);
    sg_muted         = FALSE;       /* 设备刚开，音量是自己设的，不存在"被静音" */
    sg_mute_until_ms = 0;

    /* Prime the ring buffer with a cushion of silence, BEFORE the drain task
     * starts.
     *
     * Production and consumption are balanced in steady state -- the emulator
     * hands over exactly one frame's worth of PCM per frame -- so without a
     * reserve the buffer sits at zero and starves on any jitter. That is what
     * the underrun counter reported on the NES build: a slow steady climb with
     * drop pinned at 0.
     *
     * The deeper reason the ring buffer was empty in the first place is that a
     * slow panel was stalling the producer's frame loop (see the clk note in
     * tdd_disp_rgb_ili9488.c), which made the emulator produce audio ~5% slower
     * than the DAC consumed it. With the panel fixed the average rates match,
     * and this cushion covers the remaining frame-to-frame jitter. The only
     * cost is SPK_PRIME_MS of startup latency. */
    {
        static const int16_t silence[256] = {0};
        uint32_t want = (rate * SPK_PRIME_MS / 1000) * 2;   /* 16-bit mono */
        uint32_t done = 0;

        while (done < want) {
            uint32_t n = want - done;
            if (n > sizeof(silence)) {
                n = sizeof(silence);
            }
            n = tuya_ring_buff_write(sg_rb, silence, n);
            if (0 == n) {
                break;
            }
            done += n;
        }
        PR_NOTICE("[spk] primed %u bytes of silence", (unsigned)done);
    }

    /* 驱动侧水位计：计时基准从现在起，免得第一圈把"开机到现在"整段扣掉 */
    sg_q_tick_ms  = (uint32_t)tal_system_get_millisecond();
    sg_drv_q_ms   = 0;
    sg_drv_q_max  = 0;
    sg_q_reported = 0;

    memset(&tcfg, 0, sizeof(tcfg));
    tcfg.stackDepth = SPK_TASK_STACK;
    tcfg.priority   = SPK_TASK_PRIO;
    tcfg.thrdname   = "spk_play";

    sg_task_gone = FALSE;                   /* 新任务还在跑，退出标记要清掉 */
    rt = tal_thread_create_and_start(&sg_task, NULL, NULL, prv_spk_task, NULL, &tcfg);
    if (OPRT_OK != rt) {
        PR_ERR("[spk] drain task create failed (%d)", rt);
        return rt;
    }

    sg_ready = TRUE;
    PR_NOTICE("[spk] ready: %uHz mono, ringbuf %u bytes", (unsigned)rate,
              (unsigned)rb_bytes);
    return OPRT_OK;
}

uint32_t app_speaker_rate(void)
{
    if (!sg_ready) {
        return 0;
    }
    return sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE;
}

/* 立刻静音（退出页面用）。
 *
 * app_speaker_clear() 只 discard 应用侧的 ring；已经递交到驱动的那一段（实测
 * 约 1 秒，见 sg_drv_q_ms）照样会放完，表现就是"退出页面之后还要响一秒"。
 * 驱动没有 flush 接口，而 tdl_audio_close() 会把**共享**音频设备的 DMA/ISR
 * 一起拆掉（AI 侧采集也挂在这上面）——真机上就是一句
 * `Assert at: xQueueGiveFromISR`。所以这里走硬件静音：DAC 增益拨到 0，已经
 * 排队的那一秒照样播，但播出来是静音，听感上就是"立刻停"。
 *
 * 还原由 drain 任务在 prv_drv_q_tick() 里做（到点 / 水位归零），调用方不用管。 */
void app_speaker_silence_now(void)
{
    if (!sg_ready) {
        return;
    }

    app_speaker_clear();                        /* 应用侧还没喂的先丢了 */
    prv_codec_volume(0);                        /* 驱动 FIFO 里那一段也立刻哑 */
    sg_mute_until_ms = (uint32_t)tal_system_get_millisecond()
                       + sg_drv_q_ms + SPK_UNMUTE_MARGIN_MS;
    sg_muted = TRUE;
}

/* Re-fill the ring buffer with a cushion of silence and clear the counters.
 *
 * The boot-time prime is consumed while the menus are up (the drain task plays
 * it at real time), so by the time a game starts producing the buffer would
 * sit at zero: production == consumption long-term means it stays there.
 * Re-priming right before the emulator thread starts restores the cushion.
 * Call before starting the emulator thread / resuming a paused session. */
void app_speaker_reprime(void)
{
    static const int16_t silence[256] = {0};
    uint32_t rate, want, done = 0;

    if (!sg_ready) {
        return;
    }
    rate = sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE;
    want = (rate * SPK_PRIME_MS / 1000) * 2;

    while (done < want) {
        uint32_t n = want - done;
        if (n > sizeof(silence)) {
            n = sizeof(silence);
        }
        n = tuya_ring_buff_write(sg_rb, silence, n);
        if (0 == n) {
            break;
        }
        done += n;
    }
    sg_underrun = 0;
    sg_dropped  = 0;
    PR_NOTICE("[spk] reprime %u bytes, counters cleared", (unsigned)done);
}

void app_speaker_clear(void)
{
    uint32_t used;

    if (!sg_ready || NULL == sg_rb) {
        return;
    }
    used = tuya_ring_buff_used_size_get(sg_rb);
    if (used > 0) {
        tuya_ring_buff_discard(sg_rb, used);
    }
}

uint32_t app_speaker_write(const int16_t *mono, size_t frames)
{
    uint32_t bytes;
    uint32_t written;

    if (!sg_ready || NULL == mono || 0 == frames) {
        return 0u;
    }

    bytes   = (uint32_t)(frames * 2);
    written = tuya_ring_buff_write(sg_rb, mono, bytes);
    if (written < bytes) {
        sg_dropped++;
    }

    /* 返回实际写入量，不返回 bytes —— 调用方（电影音轨）拿它累加"已喂总量"，
     * 按想写的量累加会在丢块时虚高，位置算得比实际靠后。 */
    return written;
}

void app_speaker_set_volume(uint8_t vol)
{
    sg_vol = vol;
    /* 响度在 DAC 增益寄存器上，光改 sg_vol 不会出声（软件增益已经不做了，见
     * SPK_DAC_GAIN_MAX），所以立刻重下一次 codec 音量把增益顶上去。
     *
     * 静音期间不碰：prv_codec_volume(非 0) 会顺手 unmute，把"退出后立刻没声音"
     * 那套（silence_now -> 水位放完再还原）提前打断。还原那一步
     * （prv_drv_q_tick）本来就会按新的 sg_vol 重设增益，不会漏。 */
    if (!sg_muted) {
        /* vol=0 走真静音（tkl 那边会 bk_aud_dac_mute），不是"增益拨到 -45dB" ——
         * 后者在安静环境下还听得见一点点。 */
        prv_codec_volume((0 == vol) ? 0 : SPK_VOLUME);
    }
}

void app_speaker_stop(void)
{
    int wait = 0;

    /* 代数失效 + 异步删除标记：drain 任务最多再跑一轮就自退 */
    sg_gen++;
    if (sg_task) {
        tal_thread_delete(sg_task);
        sg_task = NULL;
    }
    /* 等它真正退出（正常 <=2ms），之后才能关设备/释放缓冲 */
    while (!sg_task_gone && wait++ < 50) {
        tal_system_sleep(1);
    }
    if (!sg_task_gone) {
        PR_WARN("[spk] drain task exit timeout(50ms)");
    }

    if (sg_ready) {
        /* 关设备前先静音并停一拍：否则 DAC 会在半个波形上被硬切，放大器把这个
         * 跳变直接变成"啪"一声（日志里那串 IN-[spk] AEL_IO_ABORT 就是这个过程）。
         * 设备马上要关掉，所以这次改音量不需要还原 —— 下次 init 会重设 SPK_VOLUME。 */
        prv_codec_volume(0);
        tal_system_sleep(10);
        tdl_audio_close(sg_hdl);
        sg_ready = FALSE;
        sg_muted = FALSE;               /* 设备都没了，init 时会重新设音量 */
    }
    /* 设备已关，驱动侧那一坨队列不复存在：水位清零，下次 init 重新实测 */
    sg_drv_q_ms  = 0;
    sg_q_tick_ms = (uint32_t)tal_system_get_millisecond();
    if (sg_rb) {
        tuya_ring_buff_free(sg_rb);
        sg_rb = NULL;
    }
}

uint32_t app_speaker_room(void)
{
    if (!sg_ready || NULL == sg_rb) {
        return 0;
    }
    return tuya_ring_buff_free_size_get(sg_rb);
}

uint32_t app_speaker_buffered_ms(void)
{
    uint32_t rate;

    if (!sg_ready || NULL == sg_rb) {
        return 0;
    }
    rate = sg_info.sample_rate ? sg_info.sample_rate : SPK_FALLBACK_RATE;
    return tuya_ring_buff_used_size_get(sg_rb) * 1000u / (rate * 2u /* mono 16-bit */);
}

uint32_t app_speaker_driver_latency_ms(void)
{
    uint32_t tm;
    uint32_t base;

    if (!sg_ready) {
        return 0u;
    }
    /* 帧长优先用驱动报上来的（tdd_audio.c 是 AUDIO_PCM_FRAME_MS = 20），拿不到
     * 就退回本模块的 chunk 长度 —— 两者本来就对齐（见 SPK_CHUNK_MS 的注释）。 */
    tm   = sg_info.sample_tm_ms ? (uint32_t)sg_info.sample_tm_ms : (uint32_t)SPK_CHUNK_MS;
    base = (uint32_t)SPK_DRV_FIFO_FRAMES * tm;

    /* **实测水位优先**：它把 tkl_ao_put_frame 之下每一级（voice write pool /
     * onboard speaker pool / DMA）都算进去了，而常量只是分包粒度。没数据时
     * 水位是 0，才退回常量兜底。 */
    return (sg_drv_q_ms > base) ? sg_drv_q_ms : base;
}

uint32_t app_speaker_dropped(void)
{
    return sg_dropped;
}

uint32_t app_speaker_underrun(void)
{
    return sg_underrun;
}