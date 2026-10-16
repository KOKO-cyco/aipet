/**
 * @file app_disp.h
 * @brief 显示 / GUI 服务：统一管理面板与 LVGL 的生命周期、跨线程访问和屏幕亮度。
 *
 * 谁拥有面板
 * ----------
 * 同一时刻只有一个"面板所有者"：
 *   - 菜单类页面（LVGL）：由 LVGL 任务刷新，页面在 app_disp_lock() 保护下建 UI。
 *   - 独占类页面（模拟器 / 角色动画）：LVGL 任务被挂起，页面自己往 framebuffer
 *     里写并 flush。页面通过 APP_PAGE_OWN_DISPLAY 标志声明这一点，路由会自动
 *     在进入前 suspend、离开后 resume（见 app_router.h）。
 *
 * 为什么要集中
 * ------------
 * lv_vendor_suspend()/resume() 会 join/重启 LVGL 任务，**绝不能在 LVGL 任务自己
 * 的线程里调用**（会死锁）。把 suspend 状态收在服务层里，路由只需按顺序调用，
 * 页面完全不用关心。
 */

#ifndef __APP_DISP_H__
#define __APP_DISP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 到面板上（调用 lv_vendor_init），但不启动任务。
 *        必须在 app_disp_start() 之前、且在任何页面创建之前调用。
 */
OPERATE_RET app_disp_init(void *device_name);

/**
 * @brief 启动 LVGL 任务。启动后 LVGL 开始刷新当前屏幕。
 * @param[in] prio  LVGL 任务优先级
 * @param[in] stack 任务栈大小（字节）
 */
OPERATE_RET app_disp_start(uint32_t prio, uint32_t stack);

/**
 * @brief 取显示锁。所有"非 LVGL 任务里操作的 LVGL 对象"都必须包在这一对里。
 *        典型场景：路由线程建页面、时间回调更新 label。
 */
void app_disp_lock(void);

/** @brief 释放显示锁。 */
void app_disp_unlock(void);

/** @brief 挂起 LVGL 任务（幂等）。路由在进入独占型页面前调用。 */
void app_disp_suspend(void);

/** @brief 恢复 LVGL 任务（幂等）。路由在离开独占型页面后调用。 */
void app_disp_resume(void);

/** @brief LVGL 任务当前是否被挂起。 */
BOOL_T app_disp_suspended(void);

/** @brief 设置背光亮度 0-100（同时落盘到配置中心）。 */
OPERATE_RET app_disp_set_brightness(int pct);

/** @brief 读取当前背光亮度 0-100。 */
int app_disp_get_brightness(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_DISP_H__ */
