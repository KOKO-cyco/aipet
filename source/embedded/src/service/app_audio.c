/**
 * @file app_audio.c
 * @brief 音频服务实现：音量配置 + 提示音 + PCM 直通（底层是 app_speaker）。
 */

#include "tal_api.h"

#include "app_speaker.h"
#include "app_audio.h"
#include "app_event.h"
#include "app_settings.h"

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
#include "ai_audio_player.h"
#endif

static BOOL_T sg_inited = FALSE;

/* 只剩 AI 一路音量了（游戏 / 电影 / 音乐那三路随老产品删掉了）。
 *
 * 它**不需要**"下发到扬声器"这一步：AI 音量走 AI 播放器的 codec 音量，由
 * app_ai_chat 订阅 APP_EVT_SETTING_CHANGED 之后自己应用 —— 所以这里只负责
 * 配置中心的读写和事件广播。
 *
 * APP_AUDIO_CH_E 保留（现在只有一个成员 APP_AUDIO_CH_AI），是为了让
 * app_volui / tuya_app_main 的调用点不用改签名。 */
static APP_SET_ID_E prv_set_id(APP_AUDIO_CH_E ch)
{
    (void)ch;
    return APP_SET_VOL_AI;
}

OPERATE_RET app_audio_init(void)
{
    OPERATE_RET rt;

    if (sg_inited) {
        return OPRT_OK;
    }

    rt = app_speaker_init();
    if (OPRT_OK != rt) {
        PR_ERR("[aud] 扬声器初始化失败: %d", rt);
        return rt;
    }

    sg_inited = TRUE;
    PR_NOTICE("[aud] 音频服务就绪 (rate=%uHz, ai=%d)",
              (unsigned)app_speaker_rate(),
              app_settings_get_int(APP_SET_VOL_AI));
    return OPRT_OK;
}

void app_audio_suspend(void)
{
    /* 与 stop 相同的拆法（停任务 + tdl_audio_close），但语义是"临时让出"：
     * 调用方随后会 resume。app_speaker_stop/app_speaker_init 都幂等。 */
    app_speaker_stop();
    sg_inited = FALSE;
    PR_NOTICE("[aud] codec 已让出（AI 语音栈接管）");
}

OPERATE_RET app_audio_resume(void)
{
    OPERATE_RET rt = app_audio_init();

    if (OPRT_OK == rt) {
        PR_NOTICE("[aud] codec 已收回");
    }
    return rt;
}

int app_audio_get_volume(APP_AUDIO_CH_E ch)
{
    return app_settings_get_int(prv_set_id(ch));
}

void app_audio_tune_volume(APP_AUDIO_CH_E ch, int vol)
{
    static uint32_t s_log_ms = 0;       /* 拖动日志节流（全局一把，够用） */
    int old = app_settings_get_int(prv_set_id(ch));

    (void)ch;

    /* set_int 内部会夹范围并广播 APP_EVT_SETTING_CHANGED */
    app_settings_set_int(prv_set_id(ch), vol);
    vol = app_settings_get_int(prv_set_id(ch));
    if (vol != old) {
        /* 拖动时每秒几十次 VALUE_CHANGED，逐条打会把串口灌满 —— 限到 ~3 条/秒；
         * 最终定格值由 commit 那条兜底。 */
        uint32_t now = (uint32_t)tal_system_get_millisecond();
        if (now - s_log_ms >= 300) {
            s_log_ms = now;
            PR_NOTICE("[aud] AI 音量拖动 -> %d", vol);
        }
    }
}

void app_audio_commit_volume(APP_AUDIO_CH_E ch)
{
    int vol = app_settings_get_int(prv_set_id(ch));

    (void)ch;

    app_settings_save(prv_set_id(ch));
    PR_NOTICE("[aud] AI 音量定格 %d（已保存，云端在此时上报）", vol);
    /* 松手才算"用户定下来了"：想上报云端的一方（tuya_app_main）订阅这个事件，
     * 拖动过程中的每次 VALUE_CHANGED 都不惊动云端。 */
    app_event_publish(APP_EVT_VOLUME_COMMITTED, (void *)(intptr_t)ch);
}

void app_audio_set_volume(APP_AUDIO_CH_E ch, int vol)
{
    int old = app_settings_get_int(prv_set_id(ch));

    (void)ch;

    app_settings_set_int_now(prv_set_id(ch), vol);
    vol = app_settings_get_int(prv_set_id(ch));
    if (vol != old) {
        /* 外部直接设置（云端 DP 下发），低频调用，逐条打 */
        PR_NOTICE("[aud] AI 音量外部设置 -> %d", vol);
    }
}

OPERATE_RET app_audio_play_alert(APP_AUDIO_ALERT_E alert)
{
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    AI_AUDIO_ALERT_TYPE_E type;

    switch (alert) {
    case APP_AUDIO_ALERT_NETOK: type = AI_AUDIO_ALERT_NETWORK_CONNECTED; break;
    case APP_AUDIO_ALERT_ERROR: type = AI_AUDIO_ALERT_NETWORK_FAIL;      break;
    case APP_AUDIO_ALERT_NETCFG:
    default:                    type = AI_AUDIO_ALERT_NETWORK_CFG;       break;
    }
    return ai_audio_player_alert(type);
#else
    (void)alert;
    return OPRT_NOT_SUPPORTED;
#endif
}

void app_audio_reprime(void)
{
    app_speaker_reprime();
}

uint32_t app_audio_rate(void)
{
    return app_speaker_rate();
}

uint32_t app_audio_room(void)
{
    return app_speaker_room();
}

uint32_t app_audio_write(const int16_t *mono, size_t frames)
{
    return app_speaker_write(mono, frames);
}
