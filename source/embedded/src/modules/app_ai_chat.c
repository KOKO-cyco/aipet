/**
 * @file app_ai_chat.c
 * @brief AI 语音对话模块实现。设计说明见 app_ai_chat.h。
 *
 * 本文件是**唯一**知道"codec 现在归谁"的地方：`prv_ai_up()` / `prv_ai_down()`
 * 把 `ai_chat_init/deinit` 和 `app_audio_suspend/resume` 成对做掉。这两个顺序
 * 反了都会在真机上炸（见各自的注释），所以不要再把这个配对拆到调用方去。
 */

#include <string.h>

#include "tal_api.h"

#include "ai_chat_main.h"
#include "ai_user_event.h"
#include "ai_manage_mode.h"
#include "ai_audio_player.h"
#include "skill_emotion.h"
#include "tuya_ai_agent.h"      /* tuya_ai_agent_event() / AI_EVENT_CHAT_BREAK */

#include "app_audio.h"
#include "app_event.h"
#include "app_mem.h"
#include "app_settings.h"
#include "app_ai_chat.h"

/** 语音栈活着 = codec 在我们手上。 */
static BOOL_T sg_ai_up = FALSE;

/** 云端连上后置 TRUE，**不再回退** —— 网络断了也继续"想听"，重连后自动恢复。 */
static BOOL_T sg_net_ready = FALSE;

/** 媒体页占着 codec 的层数。进出成对，所以是计数而不是布尔。
 *
 * **当前固件里没有调用者** —— 老产品的音乐/电影/游戏三个页面是唯一的用户，它们
 * 都删了，语音栈现在是"连上云就一直听"。留着这一对是因为它是**单实例 codec 的
 * 交接钩子**：以后要有别的音源（比如备忘录到点播提醒），进来时 hold、出去时
 * release 就是现成的。prv_ai_down() 那条拆栈路径也靠它才可达。 */
static int sg_media_hold = 0;

/** 串行化状态机：net_ready 在 MQTT 就绪线程上，hold/release 在路由线程上。 */
static MUTEX_HANDLE sg_lock = NULL;

/* AI 音量变化 -> 实时应用到 AI 播放器。
 * 注意只调 ai_audio_player_set_vol()（纯内存，立即生效），**不走**
 * ai_chat_set_volume() —— 那个内部还会写一次 KV，滑条拖动的每个 VALUE_CHANGED
 * 都刷 flash 会又慢又磨 flash。掉电保存由主页滑条的 commit（app_settings_save）
 * 负责，AI 栈自己的那份 ai_toy 配置 KV 与本值无关。 */
static void prv_setting_cb(APP_EVT_E evt, void *data, void *user)
{
    (void)user;

    if (APP_EVT_SETTING_CHANGED != evt ||
        (APP_SET_ID_E)(intptr_t)data != APP_SET_VOL_AI) {
        return;
    }
    if (TRUE == sg_ai_up) {
        int vol = app_settings_get_int(APP_SET_VOL_AI);

        ai_audio_player_set_vol(vol);
        PR_DEBUG("[ai] AI 音量 -> 播放器: %d", vol);
    }
}

static void prv_ai_evt_cb(AI_NOTIFY_EVENT_T *event)
{
    if (NULL == event) {
        return;
    }

    switch (event->type) {
    case AI_USER_EVT_MODE_STATE_UPDATE: {
        int state = (int)(intptr_t)event->data;

        PR_DEBUG("[ai] mode state %d", state);
        app_event_publish(APP_EVT_AI_STATE, (void *)(intptr_t)state);
    } break;

    case AI_USER_EVT_EMOTION: {
        AI_AGENT_EMO_T *emo = (AI_AGENT_EMO_T *)event->data;

        if (NULL != emo && NULL != emo->name) {
            PR_NOTICE("[ai] emotion %s", emo->name);
            /* 同步分发：emo->name 在这次 publish 期间一直有效 */
            app_event_publish(APP_EVT_AI_EMOTION, (void *)emo->name);
        }
    } break;

    case AI_USER_EVT_KEY_WAKEUP:
        PR_NOTICE("[ai] wakeup");
        break;

    case AI_USER_EVT_ASR_OK:
        PR_INFO("[ai] asr ok");
        break;

    default:
        break;
    }
}

/***********************************************************
******************** codec 交接 / 状态机 ********************
***********************************************************/

/* 让出 codec 给语音栈，并把它拉起来。
 *
 * **顺序不能反**：扬声器必须先松手（app_audio_suspend() 会 app_speaker_stop()），
 * 否则两边同时 open 同一个单实例 codec，会在 bk_voice_init 里抢 ADC/AEC/DMA
 * —— 真机上就是界面整个卡死。 */
static OPERATE_RET prv_ai_up(void)
{
    AI_CHAT_MODE_CFG_T cfg = {
        /* FREE 模式 + 人声触发（ai_mode_free.c）：VAD 说话唤醒，静音 2 秒结束。
         * 不需要唤醒词。 */
        .default_mode = AI_CHAT_MODE_FREE,
        .default_vol  = app_settings_get_int(APP_SET_VOL_AI),
        .evt_cb       = prv_ai_evt_cb,
    };
    OPERATE_RET rt;

    app_mem_sram("ai: 让出前");
    app_audio_suspend();
    app_mem_sram("ai: codec 让出");

    rt = ai_chat_init(&cfg);
    if (OPRT_OK != rt) {
        /* 起不来就把 codec 还回去 —— 不然扬声器也哑了，连配网提示音都放不出来，
         * 比"语音用不了"严重得多。 */
        PR_ERR("[ai] 语音栈拉起失败: %d，codec 还给扬声器", rt);
        app_audio_resume();
        return rt;
    }

    sg_ai_up = TRUE;
    /* 强制把本工程的 AI 音量压回播放器：ai_chat_init() 会从**它自己的 KV**
     * （模式切换时顺手保存的那份）恢复旧音量，cfg.default_vol 只在 KV 缺失时
     * 才用得上 —— 不压回去的话，主页滑条改完一被拆栈重建就被 KV 打回原值，
     * 表现就是"AI 音量滑条怎么拖都没效果"。 */
    ai_audio_player_set_vol(app_settings_get_int(APP_SET_VOL_AI));
    /* 幂等订阅：重复调用只会刷新 user 上下文 */
    app_event_subscribe(APP_EVT_SETTING_CHANGED, prv_setting_cb, NULL);

    /* 通知别的模块"语音栈活了"。动作工具模块靠这个把 MCP 回调重新挂到 AI agent
     * 上 —— ai_chat_deinit() 会把 agent 上下文整个清空（tuya_ai_agent_deinit 里的
     * memset 连 mcp_cb / enable_mcp 一起抹掉），而本栈每次让位/收回都要重建一次。 */
    app_event_publish(APP_EVT_AI_STACK_UP, NULL);

    app_mem_sram("ai: 栈起来");
    PR_NOTICE("[ai] 语音栈就绪（FREE 模式，人声触发，音量 %d）",
              app_settings_get_int(APP_SET_VOL_AI));
    return OPRT_OK;
}

/* 拆掉语音栈，再把 codec 还给扬声器。
 *
 * **顺序不能反**：栈没拆干净就 app_audio_resume() 的话，app_speaker_stop() 里的
 * tdl_audio_close() 会把 AI 采集还在用的 DMA/ISR 一起拆掉 —— 真机上是
 * `Assert at: xQueueGiveFromISR`。 */
static void prv_ai_down(void)
{
    uint64_t t0, t1;

    /* **退出前先把这一轮掐死，再拆栈。顺序不能反。**
     *
     * 现场 log/测试.txt 19:01:22 -> 19:01:30：长按退出后 ai_chat_deinit() 卡了
     * 整整 8 秒才返回。那 8 秒里云端还在推同一句的 TTS（19:01:24 / 19:01:28 两条
     * ai_skill 文本，19:01:30 才 finish:true），而播放器任务直到 19:01:30 才处理到
     * 拆栈命令 —— 也就是说 deinit 排在"这一轮回复说完"后面：云端说不完，就退不出去。
     *
     * 所以这里先做两件事：
     *   1. ai_audio_player_stop(ALL)  本地立刻放空播放器
     *   2. AI_EVENT_CHAT_BREAK        告诉云端"别再说了"（走 MQTT 发 chat_break，
     *      见 tuya_ai_agent_event_s -> tuya_ai_event_chat_break）
     * 之后 deinit 要等的"在途数据"就不存在了。
     *
     * 注意与 app_ai_chat_interrupt() 的区别：那个是**页面内手动打断**，没在播报时
     * 故意不发 CHAT_BREAK（免得把一轮对话打断在半路，该说的没说完）；这里是
     * **让出 codec**，目的就是终止这一轮，所以无条件发、也不判 is_playing。 */
    ai_audio_player_stop(AI_AUDIO_PLAYER_ALL);
    tuya_ai_agent_event(AI_EVENT_CHAT_BREAK, 0);

    sg_ai_up = FALSE;

    /* deinit 耗时打出来：以后每多一个"要临时占 codec"的音源，这就是"进那一页
     * 会顿多久"的实测值。 */
    t0 = tal_system_get_millisecond();
    ai_chat_deinit();
    t1 = tal_system_get_millisecond();
    PR_NOTICE("[ai] 语音栈已释放（deinit 耗时 %ums）", (unsigned)(t1 - t0));

    app_audio_resume();
    app_mem_sram("ai: codec 收回");
}

/* 状态机：想听 = 云端就绪 && 没有别的音源占着 codec。
 *
 * 两个位的写法把三种时序都兜住了：
 *   没网时进占用方   -> want 一直是 FALSE，扬声器照旧，语音栈根本不起来
 *   占用方里连上云   -> hold>0，want 仍是 FALSE，不会把正在放音的 codec 抢走
 *   正常进出占用方   -> 进出各一次拆/拉
 *
 * 当前没有占用方（音乐/电影/游戏都删了），所以连上云之后 want 恒为 TRUE ——
 * 效果就是"语音栈常驻"。这套写法留着是为了以后加音源时不用重写。
 *
 * **调用方必须持锁。** */
static void prv_sync(void)
{
    BOOL_T want = (TRUE == sg_net_ready && 0 == sg_media_hold);

    if (want && TRUE != sg_ai_up) {
        (void)prv_ai_up();
    } else if (!want && TRUE == sg_ai_up) {
        prv_ai_down();
    }
}

static void prv_lock(void)
{
    /* 正常路径开机已经建好了（app_ai_chat_prepare）。这里是兜底：漏调也不至于
     * 拿着 NULL 去 lock。 */
    if (NULL == sg_lock) {
        (void)tal_mutex_create_init(&sg_lock);
    }
    tal_mutex_lock(sg_lock);
}

static void prv_unlock(void)
{
    tal_mutex_unlock(sg_lock);
}

/***********************************************************
************************* public api ***********************
***********************************************************/
void app_ai_chat_prepare(void)
{
    if (NULL == sg_lock) {
        (void)tal_mutex_create_init(&sg_lock);
    }
}

void app_ai_chat_net_ready(void)
{
    prv_lock();
    sg_net_ready = TRUE;
    prv_sync();
    prv_unlock();
}

void app_ai_chat_hold(void)
{
    prv_lock();
    sg_media_hold++;
    prv_sync();
    PR_NOTICE("[ai] 媒体页占住 codec（层数 %d）", sg_media_hold);
    prv_unlock();
}

void app_ai_chat_release(void)
{
    prv_lock();
    if (sg_media_hold > 0) {
        sg_media_hold--;
    } else {
        PR_WARN("[ai] release 比 hold 多了一次，忽略");
    }
    prv_sync();
    prv_unlock();
}

/* 手动打断：立刻掐掉当前这句播报。
 *
 * 与 VAD 打断（说话打断，见 SDK 的 ai_mode_free.c barge-in 分支）是两条独立的
 * 路：这条给 P12 短按用，不看 AEC 也不看 VAD 阈值，所以必然生效。
 *
 * "没在播报"时直接返回、**不发 CHAT_BREAK**：那条事件会把当前会话打断在半路，
 * 结果是该说的没说完、下一句又接不上。 */
void app_ai_chat_interrupt(void)
{
    if (TRUE != sg_ai_up) {
        return;
    }
    if (!ai_audio_player_is_playing()) {
        return;
    }

    PR_NOTICE("[ai] 手动打断：停止播报");
    ai_audio_player_stop(AI_AUDIO_PLAYER_ALL);
    tuya_ai_agent_event(AI_EVENT_CHAT_BREAK, 0);
}

bool app_ai_chat_active(void)
{
    return (TRUE == sg_ai_up);
}
