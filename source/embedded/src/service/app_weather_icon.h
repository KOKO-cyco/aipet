/**
 * @file app_weather_icon.h
 * @brief 天气图标：从 SD 卡按天气码加载（/sdcard/weather/<code>.bin）。
 *
 * 为什么从 SD 卡读
 * ----------------
 * 以前这套图标是编进固件的（weather_icons.c，40 张 48x48 RGB565A8 合计 1.1MB
 * flash）。换成 SD 之后 flash 省下 1.1MB，换图标也不用重编固件。
 *
 * 文件格式（`WXI1`，8 字节头 + 两个平面）
 * ---------------------------------------
 *     [0..3]   'W','X','I','1'        魔数
 *     [4..5]   u16 宽（48）
 *     [6..7]   u16 高（48）
 *     [8..]    RGB565 平面，w*h*2 字节，stride = w*2
 *     [..]     A8     平面，w*h   字节，stride = w
 *
 * 这个布局**就是** LVGL 的 LV_COLOR_FORMAT_RGB565A8 要的样子，所以加载之后
 * 直接拿整块缓冲当图像数据用，不用重排。
 *
 * 用法
 * ----
 *     lv_obj_t *img = lv_image_create(parent);
 *     lv_image_set_src(img, app_weather_icon(day->code));
 *
 * 想要**单色**（比如顶栏那条米白底上的暗色图标）就再叠一层重着色：
 *     lv_obj_set_style_image_recolor(img, 深灰, 0);
 *     lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
 *
 * 线程
 * ----
 * `app_weather_icon()` 会读 SD 卡（只在缓存未命中时），**只能在 LVGL 线程里调**。
 * 缓存 8 张（约 55KB PSRAM），一屏最多同时用 4 张（今天 + 3 天详情）。
 */

#ifndef __APP_WEATHER_ICON_H__
#define __APP_WEATHER_ICON_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 图标目录与文件名的格式（code = 心知天气代码，0..38 / 99）。 */
#define APP_WICON_DIR   "/sdcard/weather"
#define APP_WICON_FMT   "/sdcard/weather/%d.bin"

/**
 * @brief 按天气码取图标。
 *
 * @param[in] code 心知天气代码（0..38 / 99）。未知代码回退到 99（未知图标）；
 *                 连 99 都不在卡上就返回 NULL。
 * @return 图像描述符（数据在 PSRAM，随缓存淘汰失效）；NULL 时
 *         `lv_image_set_src(img, NULL)` 是清空，安全。
 */
const lv_image_dsc_t *app_weather_icon(int32_t code);

/** @brief 丢掉全部缓存（SD 卡换过 / 拔插之后调）。只能在 LVGL 线程里调。 */
void app_weather_icon_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEATHER_ICON_H__ */
