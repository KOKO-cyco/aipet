/**
 * @file app_cam_test.h
 * @brief 摄像头（GC2145 / DVP）bring-up 的开关与入口。
 *
 * **这是一个独立的测试模式**：打开之后开机就跑它，拍一张照片铺满 LCD，然后停在
 * 那儿不动（不进桌宠页）。排查完把 APP_CAM_TEST 改回 0，代码留在树里不影响构建。
 *
 * 为什么先扫 I2C 再拍照
 * --------------------
 * 2026-10-16 真机：DVP 起来之后 SCCB 全在 `wait ack timeout` / `gc2145 i2c write err`，
 * 一帧都没有。那说明**传感器根本没应答**，跟分辨率、格式、YUV 顺序都没关系。
 * 所以第一步是扫一遍 SCCB 总线：总线上有没有设备、0x3C（GC2145）在不在、
 * 0x14（触摸屏 GT1151）在不在 —— 一次就能把"总线坏了"和"模组没插/没起来"分开。
 */

#ifndef __APP_CAM_TEST_H__
#define __APP_CAM_TEST_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = 开机进摄像头测试模式（阻塞，拍完停在照片上）；0 = 正常进桌宠页。
 *
 * 位置在 `app_disp_start()` 之后、桌宠页之前（见 tuya_app_main.c）：
 * LVGL 得先跑起来才谈得上"显示到 LCD 上"。 */
#define APP_CAM_TEST    0

/**
 * @brief 跑摄像头 bring-up：扫 SCCB -> 拍照 -> 铺满 LCD -> **停住不返回**。
 *
 * 流程里每一步都打日志，卡在哪一步看串口就知道：
 *   1) `[camt] SCCB 扫描` —— 总线上谁在应答
 *   2) `[camt] 设备信息` —— 最大分辨率 / 支持的格式
 *   3) `[camt] 开流` / `首帧` —— 回调有没有被调
 *   4) `[camt] 已上屏` —— 照片铺上去了
 *
 * 拍照失败会**直接返回**（调用方照常往下启动），不会把设备卡死在测试模式。
 */
void app_cam_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAM_TEST_H__ */
