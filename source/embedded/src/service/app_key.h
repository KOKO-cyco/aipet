/**
 * @file app_key.h
 * @brief 按键服务：GPIO 消抖 + 短按/长按边沿事件。
 *
 * 线程模型
 * --------
 * 只有一个线程可以调用 app_key_poll()：普通情况下是路由线程（app_router 每
 * APP_KEY_TICK_MS 调一次）；当某个页面**接管**了主循环（比如角色动画页自己跑
 * 帧循环），就由那个页面在自己的循环里调用。两处不会同时发生，因为接管期间路由
 * 线程正阻塞在该页面的 on_enter 里。
 *
 * 取事件
 * ------
 * app_key_take() 从该按键的 FIFO 里取一个边沿事件，取完返回 APP_KEY_EVT_NONE。
 * 一次按下会产生：DOWN -> (SHORT 或 LONG) -> UP。
 *
 *   菜单返回     用 DOWN（按下即响应，和改造前的行为一致）
 *   角色动画     用 SHORT 切下一个动作，LONG 退出（和中途松手/长按的原逻辑一致）
 *
 * 新增按键
 * --------
 * 在 APP_KEY_E 里加一项，在 app_key.c 的 sg_keys[] 里给它填上 GPIO 并置 used=TRUE。
 * 消抖、长短按判定、事件队列全部自动可用。
 */

#ifndef __APP_KEY_H__
#define __APP_KEY_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 轮询周期（毫秒）。路由线程按这个节奏调 app_key_poll()。 */
#define APP_KEY_TICK_MS     30
/** 消抖：连续 N 次采样为低才认按下。 */
#define APP_KEY_DEBOUNCE    2
/** 长按判定阈值（毫秒）。 */
#define APP_KEY_HOLD_MS     1500

typedef enum {
    APP_KEY_BACK = 0,      /* P12，低电平有效（板级返回键） */
    APP_KEY_COUNT
} APP_KEY_E;

typedef enum {
    APP_KEY_EVT_NONE = 0,
    APP_KEY_EVT_DOWN,      /* 消抖后按下 */
    APP_KEY_EVT_UP,        /* 松开 */
    APP_KEY_EVT_SHORT,     /* 松开且未达长按阈值 */
    APP_KEY_EVT_LONG,      /* 按住达到 APP_KEY_HOLD_MS（只报一次） */
} APP_KEY_EVT_E;

/** @brief 初始化按键 GPIO（幂等）。 */
OPERATE_RET app_key_init(void);

/** @brief 采样一次并产生边沿事件。见文件头的线程约定。 */
void app_key_poll(void);

/** @brief 取该按键下一个待处理事件，没有返回 APP_KEY_EVT_NONE。 */
APP_KEY_EVT_E app_key_take(APP_KEY_E key);

/** @brief 该按键当前是否处于按下状态。 */
BOOL_T app_key_is_down(APP_KEY_E key);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KEY_H__ */
