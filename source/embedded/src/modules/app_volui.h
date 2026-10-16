/**
 * @file app_volui.h
 * @brief 音量控件的公共实现：主页弹窗里的"一行"，以及应用页面里的"一根条"。
 *
 * 为什么要单独抽一个模块
 * ----------------------
 * 同一路音量会在两个地方出现（主页音量窗 + 它自己的应用页），两处必须始终显示
 * 同一个值。与其每个页面各写一份"滑条 + 数值 + 拖动/松手回调"，不如让控件自己
 * 订阅 APP_EVT_SETTING_CHANGED：任何一处改了音量（拖动、**云端 DP 下发**、代码
 * 里直接设值），所有活着的控件都会跟上。
 *
 * 线程
 * ----
 * 事件是**同步分发**的，发布者可能是 yield 线程（云端下发）也可能是 LVGL 线程
 * （本地拖动）。LVGL 只能在 LVGL 线程里碰，而 LVGL 任务跑 lv_task_handler() 时
 * 本身持有显示锁 —— 在事件回调里再 app_disp_lock() 会把自己锁死。所以事件回调
 * 只置一个"哪几路变了"的脏位，真正的控件刷新由本模块自己的 LVGL 定时器（200ms）
 * 完成：云端改完音量，界面最多 200ms 就跟上，看着就是实时的。
 *
 * 使用
 * ----
 *   app_volui_row(win, APP_AUDIO_CH_AI, 46);              // 弹窗一行
 *   app_volui_bar(scr, APP_AUDIO_CH_AI, "音量", x, y, w);  // 应用内一根条
 *
 * 控件被删除时（页面销毁、弹窗关闭）自己注销，调用方不用管。
 */

#ifndef __APP_VOLUI_H__
#define __APP_VOLUI_H__

#include "tuya_cloud_types.h"

#include "lvgl.h"

#include "app_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 弹窗里的一行：名称 + 数值 + 滑条。
 *
 * 布局按 280 宽的弹窗写死（状态栏音量窗就是按它排的），@a y 是这一行顶端的 y。
 * @return 滑条对象（一般用不到，传出去方便调用方摆位置）。
 */
lv_obj_t *app_volui_row(lv_obj_t *parent, APP_AUDIO_CH_E ch, int y);

/**
 * @brief 应用内的一根条：可选名称标签（滑条上方 30px，传 NULL 则不要）
 *        + 滑条 + 右侧数值（数值在 x+w+16, y-8）。
 *
 * @return 滑条对象。
 */
lv_obj_t *app_volui_bar(lv_obj_t *parent, APP_AUDIO_CH_E ch, const char *name,
                        int x, int y, int w);

#ifdef __cplusplus
}
#endif

#endif /* __APP_VOLUI_H__ */
