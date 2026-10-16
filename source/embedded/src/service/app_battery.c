/**
 * @file app_battery.c
 * @brief 电量读数。语义见 app_battery.h。
 *
 * 接真电池时要改的**只有 prv_read() 这一个函数**：
 *
 *   1) 查板子上的电池采样脚 —— `.tuyaopen/ide/board.json` 的
 *      `peripheralPatterns` 里找 ADC / 电池相关的项（没有就是没接）。
 *   2) 按 platform.json 的 `peripherals.adc` 拿 ADC 通道和量程，用 TAL 的 ADC
 *      接口读原始值（**别直接调平台相关函数**，见 AGENTS.md 的约定）。
 *   3) 查表/分段线性换算成百分比，写进下面的 sg_pct / sg_charging。
 *   4) 采样放哪：别在状态栏的 1s lv_timer 里读 ADC（那是 LVGL 线程，读 ADC 会
 *      阻塞渲染）。起一条低频线程或挂到 tal_sw_timer 上，结果写进这两个静态变量。
 */

#include "tal_api.h"

#include "app_battery.h"

/** 假数据：78%，未充电。接上真电池后删掉这两个常量。 */
#define APP_BATTERY_FAKE_PCT        78
#define APP_BATTERY_FAKE_CHARGING   FALSE

static int    sg_pct      = APP_BATTERY_FAKE_PCT;
static BOOL_T sg_charging = APP_BATTERY_FAKE_CHARGING;
static BOOL_T sg_valid    = TRUE;      /* 假数据也算"有读数" —— 状态栏要显示 78% */

/* 读一次电池。现在是常量，接 ADC 时替换这里。 */
static void prv_read(void)
{
    /* TODO(接电池)：读 ADC -> 换算 -> 写 sg_pct / sg_charging / sg_valid */
}

BOOL_T app_battery_get(int *pct, BOOL_T *charging)
{
    prv_read();

    if (NULL != pct) {
        *pct = sg_pct;
    }
    if (NULL != charging) {
        *charging = sg_charging;
    }
    return sg_valid;
}
