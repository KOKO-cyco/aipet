/**
 * @file weather_icons.h
 * @brief 天气图标查表（内嵌 RGB565A8 位图，48x48，带真透明）。
 *
 * 实现在 weather_icons.c —— 那个文件由 tools/gen_weather_icons.py 从
 * third_party/white/1x/<code>.png 自动生成（图标编进固件，不走 SD，原因见生成器）。
 *
 * 用法
 * ----
 * 保留原始 alpha，**不需要** recolor，也不挑背景色：
 *
 *     lv_obj_t *img = lv_image_create(parent);
 *     lv_image_set_src(img, app_weather_icon(day->code));
 *
 * 48x48x3 = 6912 B/张，40 张约 270KB flash。
 */

#ifndef __WEATHER_ICONS_H__
#define __WEATHER_ICONS_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按心知天气代码取内嵌图标。
 * @param[in] code 心知天气代码（0..38 / 99）。
 * @return 图标描述符；未知代码回退到 99；连 99 都没有时返回 NULL
 *         （lv_image_set_src(img, NULL) 是清空，安全）。
 */
const lv_image_dsc_t *app_weather_icon(int32_t code);

#ifdef __cplusplus
}
#endif

#endif /* __WEATHER_ICONS_H__ */
