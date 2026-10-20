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
#define APP_STATUSBAR_H     24

/**
 * @brief 在 @a parent 顶上建状态栏（幂等：重复调用只重建一次）。
 *
 * 状态栏是**父容器的一个 flex 子项**（宽 100%、高 APP_STATUSBAR_H），所以它自己
 * 不定位 —— 由父容器的布局决定。三个弹窗建在 @a parent 上、盖满整屏。
 *
 * @param[in] parent 通常是桌宠页的屏幕对象（弹窗也建在它上面）。
 * @return 状态栏对象；失败返回 NULL。
 *
 * @note 必须在 app_disp_lock() 区间内调用。
 */
lv_obj_t *app_statusbar_create(lv_obj_t *parent);

/**
 * @brief 更新左边那个"待办 N"徽标。
 *
 * @param[in] pending 未完成条数。内容没变时不动标签（省一次重排 + 重绘）。
 * @note 必须在 LVGL 线程（或持显示锁）里调。
 */
void app_statusbar_set_todo(uint32_t pending);

/** @brief 弹"设置"小面板（网络 / 音量两项）。接在 P12 长按上。任意线程可调。 */
void app_statusbar_open_settings(void);

/** @brief 弹 WiFi 键盘窗。任意线程可调。 */
void app_statusbar_open_wifi(void);

/** @brief 弹 AI 音量窗。任意线程可调。 */
void app_statusbar_open_volume(void);

/** @brief 弹"时间设置"窗（手动拨时/分）。任意线程可调。 */
void app_statusbar_open_time(void);

/**
 * @brief 当前是否有弹窗（天气 / WiFi / 音量 / 时间 / 设置）开着。任意线程可调。
 */
BOOL_T app_statusbar_popup_active(void);

/**
 * @brief 遮罩在不在 —— **免锁**，专门给 LVGL 定时器（宠物 tick）判断用。
 *
 * popup_active() 内部要拿显示锁，在 LVGL 线程里调会死锁；这个只读一个
 * volatile 标志。宠物 tick 看到它就直接停手：弹窗盖住界面时没必要再每 42ms
 * 重绘一遍弹窗下面的宠物区域。
 */
BOOL_T app_statusbar_popup_visible(void);

/**
 * @brief 关掉当前弹窗（没有就什么都不做）。
 *
 * @return TRUE = 关掉了一个弹窗；FALSE = 本来就没开。
 * @note 任意线程可调（内部加显示锁）。P12 回退键先问它：弹窗优先于页面回退。
 */
BOOL_T app_statusbar_close_popup(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_STATUSBAR_H__ */
