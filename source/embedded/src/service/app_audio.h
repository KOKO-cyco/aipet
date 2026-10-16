/**
 * @file app_audio.h
 * @brief 音频服务：音量管理、提示音、PCM 直通。
 *
 * 只剩一路音量
 * ------------
 *   APP_AUDIO_CH_AI    AI 音量 —— AI 语音 / TTS。由 AI 音频播放器消费（codec 音量）。
 *
 * 老产品还有游戏 / 电影 / 音乐三路（走 app_speaker 的软件增益），随那些模块一起
 * 删掉了。**通道参数保留着**（枚举只剩一个成员），是为了不动 app_volui /
 * tuya_app_main 的调用点签名。
 *
 * AI 音量落在配置中心（app_settings）里，掉电保存。改了以后不会直接去摸 AI 播放器，
 * 而是广播 APP_EVT_SETTING_CHANGED，由 app_ai_chat 订阅后自行应用 —— 这样音频
 * 服务不需要知道 AI 模块的存在。
 *
 * 拖动滑条
 * --------
 *   value_changed -> app_audio_tune_volume()   只改内存 + 广播，不写 flash
 *   released      -> app_audio_commit_volume() 落盘一次
 * 云端 DP 下发、代码里直接设值等低频场景用 app_audio_set_volume() 一步到位。
 *
 * 播放 PCM
 * -------
 * app_audio_write() 是非阻塞的（底层环形缓冲满了就丢块），采样率必须等于
 * app_audio_rate()，可用 app_audio_room() 反压来节流。**现在固件里没有 PCM
 * 写入方了**（音乐/电影音轨都删了）—— 这几个透传接口留着是给 bring-up 测试和
 * 以后的功能用（见 src/tests/）。
 */

#ifndef __APP_AUDIO_H__
#define __APP_AUDIO_H__

#include <stddef.h>
#include <stdint.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_AUDIO_CH_AI = 0,    /* AI 音量（AI 语音 / TTS，走 AI 播放器的 codec 音量） */
    APP_AUDIO_CH_COUNT
} APP_AUDIO_CH_E;

/** 提示音类型（映射到底层 AI 播放器的语音提示）。 */
typedef enum {
    APP_AUDIO_ALERT_NETCFG = 0,   /* 进入配网 */
    APP_AUDIO_ALERT_NETOK,        /* 联网成功 */
    APP_AUDIO_ALERT_ERROR,        /* 失败提示 */
} APP_AUDIO_ALERT_E;

/**
 * @brief 打开板载扬声器（幂等），并把配置中心里的游戏音量下发给底层。
 *        调用者：用户主流程启动时。模拟器也会调用（同样幂等）。
 */
OPERATE_RET app_audio_init(void);

/**
 * @brief 让出音频 codec（幂等）：停 drain 任务 + 关底层语音管线。
 *
 * 板载 codec 是单实例：底层 tdd_audio 的 open 会**无条件重建**整条
 * tkl_ai 语音管线（麦克风 + AEC）。AI 语音栈要打开同一条管线，所以让位前必须
 * 先把我们的 open 关掉，否则二次 open 资源打架会卡死（`xQueueGiveFromISR`）。
 *
 * 与 app_audio_resume() 成对使用。**唯一调用方是 app_ai_chat.c 的 prv_up/prv_down**
 * —— 语音栈常驻，由它按"云端是否就绪 / 媒体页有没有占着"决定谁来持有 codec。
 * 别的模块不要再自己调这一对。
 */
void app_audio_suspend(void);

/** @brief 重新拿回音频 codec（幂等，等于重新 init：开设备 + 重灌静音垫）。 */
OPERATE_RET app_audio_resume(void);

/** @brief 读音量 0-100（内存缓存，无 IO）。 */
int app_audio_get_volume(APP_AUDIO_CH_E ch);

/** @brief 拖动中改音量：改内存 + 广播事件，**不写 flash**。 */
void app_audio_tune_volume(APP_AUDIO_CH_E ch, int vol);

/** @brief 拖动结束：把当前音量落盘。 */
void app_audio_commit_volume(APP_AUDIO_CH_E ch);

/** @brief 改音量并立即落盘（云端 DP 下发、开机恢复等低频场景）。 */
void app_audio_set_volume(APP_AUDIO_CH_E ch, int vol);

/** @brief 播放一条语音提示音。 */
OPERATE_RET app_audio_play_alert(APP_AUDIO_ALERT_E alert);

/**
 * @brief 重新灌满静音垫并清零丢块/欠载统计。
 *        开始或恢复游戏前调用：开机时灌的那一份在菜单期间就被消耗光了，
 *        不补的话整个会话缓冲都躺在零点，一上来就是爆音。
 */
void app_audio_reprime(void);

/** @brief 设备输出采样率 Hz（未初始化返回 0）。写 PCM 前必须拿它做基准。 */
uint32_t app_audio_rate(void);

/** @brief 环形缓冲剩余空间（字节）。用来反压生产端，避免丢块。 */
uint32_t app_audio_room(void);

/**
 * @brief 非阻塞写一段单声道 16bit PCM（采样率必须是 app_audio_rate()）。
 *        缓冲满时整块丢弃并计入丢块统计，不会阻塞调用者。
 *
 * @return 真正进了环形缓冲的字节数（= 帧数 x 2）。要自己算播放位置的生产者
 *         必须用这个值累加，别无脑加"想写的帧数"（见 app_speaker_write 注释）。
 */
uint32_t app_audio_write(const int16_t *mono, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* __APP_AUDIO_H__ */
