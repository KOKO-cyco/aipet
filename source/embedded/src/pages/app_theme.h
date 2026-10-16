/**
 * @file app_theme.h
 * @brief 页面公共视觉与控件小工具（颜色、中文字体、标题、关闭按钮）。
 *
 * 所有 LVGL 页面共用这一套常量和小工具，新增页面直接拿，不要再各写一份
 * lv_obj_set_style_* 的样板。
 *
 * 线程约定：这些函数**不**自己加锁。调用者必须在 app_disp_lock() 区间内使用
 * （路由线程建页面时天然满足；事件回调里要自己包）。
 */

#ifndef __APP_THEME_H__
#define __APP_THEME_H__

#include "tuya_cloud_types.h"
#include "lvgl.h"
#include "app_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 面板尺寸 ----------------------------------------------------------- */
#define UI_PANEL_W          320
#define UI_PANEL_H          480

/* ---- 配色 --------------------------------------------------------------- */
#define UI_COL_BG           0x000000    /* 页面底色 */
#define UI_COL_TEXT         0xFFFFFF    /* 主文字（深底上） */
#define UI_COL_TEXT_DK      0x222222    /* 主文字（浅底上）—— 白底页专用 */
#define UI_COL_DIM          0x888888    /* 次要文字 / 占位 */
#define UI_COL_ACCENT       0x30A060    /* 强调色（标题、主按钮） */
#define UI_COL_BTN          0x2A2A32    /* 普通按钮底 */
#define UI_COL_DANGER       0x553333    /* 关闭 / 危险按钮底 */

/* ---- 中文字体 ------------------------------------------------------------
 *
 * 固件里编译的是 GUI Guider 生成的手挑子集（4bpp），缺字就渲染空白。所以这两个宏
 * 不直接指向那份 const 字体，而是指向 app_font.c 里的**代理字体**（地址恒定）：
 *
 *   开机时  代理的内容 == 内置子集本体（行为与以前完全一样）
 *   后台把 SD 卡上的完整字库（/sdcard/font/cn16.bin、cn21.bin）读进 PSRAM 之后，
 *   代理的 fallback 指向它 —— 内置子集查不到的码点自动去 SD 字库找。
 *
 * 之所以要绕一圈：内置字体是 const、编译进 rodata（flash），改不了它的 fallback
 * 字段；而代理在 RAM 里、地址不变，改它的字段对所有已建界面立刻生效。
 * 详见 app_font.h。 */
LV_FONT_DECLARE(lv_font_SourceHanSerifSC_Regular_16)
LV_FONT_DECLARE(lv_font_SourceHanSerifSC_Regular_21)

#define UI_FONT_21          app_font_21()      /* 大号标题 / 时钟 */
#define UI_FONT_CN          app_font_cn()      /* 正文 / 列表 */
#define UI_FONT_ICON        (&lv_font_montserrat_16)   /* 内建符号字体 */

/**
 * @brief 造一个带颜色和字体的标签。@a font 传 NULL 用 LVGL 默认字体。
 */
lv_obj_t *app_ui_label(lv_obj_t *parent, const char *txt,
                       const lv_font_t *font, uint32_t color);

/** @brief 把屏幕刷成统一的页面底色。 */
void app_ui_screen_bg(lv_obj_t *scr);

/** @brief 页面标题（左上角，强调色 16px 中文）。 */
lv_obj_t *app_ui_title(lv_obj_t *scr, const char *text);

/**
 * @brief 右上角关闭按钮（红底 ✕）。
 * @param[in] cb   点击回调；为 NULL 时默认删除 @a parent。
 */
lv_obj_t *app_ui_close_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user);

/**
 * @brief 通用按钮：指定尺寸、底色和居中文字。
 * @param[in] cb  点击回调；为 NULL 表示无动作。
 */
lv_obj_t *app_ui_button(lv_obj_t *parent, int32_t w, int32_t h,
                        uint32_t bg, const char *text,
                        const lv_font_t *font, uint32_t text_color,
                        lv_event_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* __APP_THEME_H__ */
