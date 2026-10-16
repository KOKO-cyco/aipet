/**
 * @file app_bringup.h
 * @brief 开机自检开关与入口。
 *
 * 用途：把"麦克风采集"和"VAD 人声判定"从 AI 语音栈里剥出来单独验证。
 * 排查"无法唤醒"时，先看这两项是否成立：
 *   测试 A 通过  -> 麦克风硬件、codec、采集链路正常
 *   测试 B 通过  -> BK 的 VAD 能识别人声，问题在 AI 栈的接线（VAD 模式等）
 *   测试 B 失败  -> VAD 层就没出结果，跟 AI 栈无关
 *
 * 排查结束后把 APP_BRINGUP_MIC_TEST 改回 0，测试代码留在树里不影响构建。
 */

#ifndef __APP_BRINGUP_H__
#define __APP_BRINGUP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = 开机先跑自检（阻塞，跑完再进正常应用）；0 = 正常直接进应用。
 *
 * 自检跑在 app_disp_init() 之后、app_disp_start() 之前（见 tuya_app_main.c 的
 * 位置说明，音频线程必须在显示子系统就绪后创建）。那个位置有两个副作用，
 * 都是"预期内"的，不是 bug：
 *
 *   1) 屏幕颜色不对 —— app_disp_init() 只把 LVGL 挂上并开了显示，真正渲染的
 *      lvgl 任务要到 app_disp_start() 才起。自检期间面板扫的是一块从没画过的
 *      帧缓冲，所以满屏彩色噪点；自检结束、LVGL 开始渲染后就恢复正常。
 *   2) 回放像"炸麦" —— 自检会把录到的 2 秒原始麦克风数据从扬声器放一遍
 *      （app_mic_record.c 的 playback via speaker），录到峰值 -2.6dBFS 的东西
 *      直接放出来本来就很响，还夹着 codec 反复开关的爆音。
 *
 * 所以日常开发保持 0；需要复现自检结论时再临时改 1。 */
#define APP_BRINGUP_MIC_TEST    0

/** 自检轮数（每轮 = 测试 A + 测试 B）。 */
#define APP_BRINGUP_ROUNDS      3

/**
 * @brief 跑麦克风 / VAD 自检。阻塞，直到所有轮次结束。
 *
 * 结束时会把扬声器通道停掉，把 codec 干净地交还给后续的 app_audio_init()。
 */
void app_bringup_mic_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_BRINGUP_H__ */
