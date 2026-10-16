/**
 * @file app_statusbar.h
 * @brief 顶部状态栏：WiFi · 天气 · 时间 · AI 音量 · 电量，以及它们背后的弹窗。
 *
 * 为什么是"控件"而不是"页面"
 * --------------------------
 * 桌宠页是唯一的主界面，状态栏只是它顶上的一条 —— 用页面栈来表达"状态栏 +
 * 内容"会让路由和返回键语义全乱掉（返回键该退到哪？）。所以这里导出的是
 * `app_statusbar_create(parent)`，由桌宠页在 `on_enter` 里挂上去。
 *
 * 谁负责布局
 * ----------
 * 状态栏自己画在 `parent` 的 (0, 0)，占 480x32。**内容区从 y=32 往下**是桌宠页
 * 的事（见 page_pet.c 的 PET_BODY_Y）。三个弹窗建在 `parent` 上、盖满整屏。
 *
 * 线程
 * ----
 * `app_statusbar_create()` 必须在**持显示锁**（或 LVGL 线程）里调。
 * 其余两个 open_* 是给路由线程（按键回调）用的，内部自己加锁。
 * 事件回调（天气/网络/配置变化）只置脏位，刷界面交给自己的 1s lv_timer ——
 * 事件是在发布者线程同步分发的，那里碰 LVGL 会炸（见 app_weather.h）。
 */

#ifndef __APP_STATUSBAR_H__
#define __APP_STATUSBAR_H__

#include "tuya_cloud_types.h"

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态栏高度。桌宠页的内容区从这一行往下排（PET_BODY_Y）。 */
#define APP_STATUSBAR_H     32

/**
 * @brief 在 @a parent 顶上建状态栏（幂等：重复调用只重建一次）。
 *
 * @param[in] parent 通常是桌宠页的屏幕对象；三个弹窗也建在它上面。
 * @return 状态栏对象；失败返回 NULL。
 *
 * @note 必须在 app_disp_lock() 区间内调用。
 */
lv_obj_t *app_statusbar_create(lv_obj_t *parent);

/** @brief 弹"设置"小面板（网络 / 音量两项）。接在 P12 长按上。任意线程可调。 */
void app_statusbar_open_settings(void);

/** @brief 弹 WiFi 键盘窗。任意线程可调。 */
void app_statusbar_open_wifi(void);

/** @brief 弹 AI 音量窗。任意线程可调。 */
void app_statusbar_open_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_STATUSBAR_H__ */
