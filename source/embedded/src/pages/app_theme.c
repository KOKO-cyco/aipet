/**
 * @file app_theme.c
 * @brief 页面公共控件小工具实现。
 */

#include <string.h>

#include "app_theme.h"

lv_obj_t *app_ui_label(lv_obj_t *parent, const char *txt,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_label_set_text(l, (NULL != txt) ? txt : "");
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (NULL != font) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    return l;
}

void app_ui_screen_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

lv_obj_t *app_ui_title(lv_obj_t *scr, const char *text)
{
    lv_obj_t *t = app_ui_label(scr, text, UI_FONT_CN, UI_COL_ACCENT);

    lv_obj_set_pos(t, 12, 10);
    return t;
}

/* 默认行为：把父对象删掉（弹窗自己关自己） */
static void prv_default_close_cb(lv_event_t *e)
{
    lv_obj_t *parent = lv_obj_get_parent(lv_event_get_target(e));

    if (NULL != parent) {
        lv_obj_delete_async(parent);
    }
}

lv_obj_t *app_ui_close_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 40, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_DANGER), 0);
    lv_obj_add_event_cb(btn, (NULL != cb) ? cb : prv_default_close_cb,
                        LV_EVENT_CLICKED, user);
    lv_obj_center(app_ui_label(btn, LV_SYMBOL_CLOSE, UI_FONT_ICON, UI_COL_TEXT));
    return btn;
}

lv_obj_t *app_ui_button(lv_obj_t *parent, int32_t w, int32_t h,
                        uint32_t bg, const char *text,
                        const lv_font_t *font, uint32_t text_color,
                        lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    if (NULL != cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    lv_obj_center(app_ui_label(btn, text, font, text_color));
    return btn;
}
