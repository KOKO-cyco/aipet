/**
 * @file app_battery.h
 * @brief 电量：状态栏右上角那一格。
 *
 * **现在是个摆设。** 板子上没接电池采样，实现返回一个固定值（78%、未充电）。
 * 但接口是真的：以后接上电池 ADC，只改 `app_battery.c` 里 `prv_read()` 一个函数，
 * 状态栏一行都不用动。
 *
 * 为什么单独一个文件而不是把 78% 写在状态栏里：状态栏那边一旦把"假"焊死，
 * 以后接真电量就得改 UI 代码 —— 而 UI 代码是最不该知道"数据从哪来"的地方。
 */

#ifndef __APP_BATTERY_H__
#define __APP_BATTERY_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取当前电量。
 *
 * @param[out] pct      电量百分比 0-100（两个出参都可为 NULL，只取想要的）
 * @param[out] charging TRUE = 正在充电
 * @return TRUE 有有效读数；FALSE = 没有电池信息（状态栏会画成"--"）。
 *
 * 任意线程可调（纯内存读，没有 IO）。
 */
BOOL_T app_battery_get(int *pct, BOOL_T *charging);

#ifdef __cplusplus
}
#endif

#endif /* __APP_BATTERY_H__ */
