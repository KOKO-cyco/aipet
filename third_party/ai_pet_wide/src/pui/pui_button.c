/**
 * @file pui_button.c
 * @brief The capsule button and the round icon button.
 *
 * One component replaces the three the theme used to carry — primary, ghost and pill —
 * because the difference between them was never structural. `pui_button_create(p,
 * "删除", PUI_TONE_RED, PUI_FILL_OUTLINE, PUI_SIZE_MD)` and `pui_button_create(p, "保存",
 * PUI_TONE_ACCENT, PUI_FILL_SOLID, PUI_SIZE_MD)` are the same widget in two costumes, not
 * two widgets; the tone says which hue, the fill says how much of it, the size says how
 * big. That is also why a new button on a new page is one line rather than a new
 * constructor.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_BTN      (&pui_button_class)
#define MY_ICON_BTN (&pui_icon_button_class)

/* The tool button: the glyph beside the label, or over it. 34 is one line of the 14 px
   face plus the glyph's line box and enough air to read as a button; 46 is the stacked
   variant's, which is two line boxes and a hairline between them. Both are shorter than
   the 36 px capsule on purpose — a tool button is furniture beside a page's own action,
   not the action. */
#define TOOL_H         34
#define TOOL_H_STACK   46
#define TOOL_RADIUS    10
#define TOOL_GAP       3
#define TOOL_GAP_STACK 1

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief The height and the horizontal padding for a size step.
 *
 * The radius is not in the table: it is half the height, so that the ends come out as
 * semicircles whatever the label's width. That is what makes a 28 px pill and a 44 px
 * capsule read as the same family.
 */
static lv_coord_t __size_h(PUI_SIZE_E size)
{
    switch (size) {
    case PUI_SIZE_SM:
        return PUI_BTN_H_SM;
    case PUI_SIZE_LG:
        return PUI_BTN_H_LG;
    case PUI_SIZE_MD:
    default:
        return PUI_BTN_H_MD;
    }
}

static lv_coord_t __size_pad(PUI_SIZE_E size)
{
    switch (size) {
    case PUI_SIZE_SM:
        return 10;
    case PUI_SIZE_LG:
        return 18;
    case PUI_SIZE_MD:
    default:
        return 14;
    }
}

static void __button_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_button_t *btn = (pui_button_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);
    /* A button is the one container that must keep the flag: the press has to land
       somewhere, and this is the object it is meant to land on. */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    btn->size = PUI_SIZE_MD;

    lv_obj_set_width(obj, LV_SIZE_CONTENT);
    lv_obj_set_height(obj, __size_h(PUI_SIZE_MD));
    lv_obj_set_style_radius(obj, __size_h(PUI_SIZE_MD) / 2, 0);
    lv_obj_set_style_pad_hor(obj, __size_pad(PUI_SIZE_MD), 0);
    lv_obj_set_style_pad_ver(obj, 0, 0);

    btn->label = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_ON_ACCENT);
    lv_obj_center(btn->label);

    pui_priv_pressable(obj);
}

const lv_obj_class_t pui_button_class = {
    .constructor_cb = __button_constructor,
    .instance_size  = sizeof(pui_button_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_button",
};

lv_obj_t *pui_button_create(lv_obj_t *parent, const char *text, PUI_TONE_E tone, PUI_FILL_E fill, PUI_SIZE_E size)
{
    lv_obj_t          *obj = lv_obj_class_create_obj(MY_BTN, parent);
    pui_button_t      *btn;
    const PUI_PAINT_T *paint = pui_paint(tone, fill);
    lv_coord_t         h     = __size_h(size);

    lv_obj_class_init_obj(obj);

    btn       = (pui_button_t *)obj;
    btn->size = size;

    lv_obj_set_height(obj, h);
    lv_obj_set_style_radius(obj, h / 2, 0);
    lv_obj_set_style_pad_hor(obj, __size_pad(size), 0);

    pui_priv_surface(obj, paint, h / 2);
    lv_obj_set_style_text_color(btn->label, paint->fg, 0);
    lv_label_set_text(btn->label, text != NULL ? text : "");

    /* A solid button is the page's loudest control, so it gets the one resting shadow in
       the library; the pressed style then collapses it. Everything softer stays flat. */
    if (fill == PUI_FILL_SOLID) {
        pui_priv_press_shadow(obj);
    }

    return obj;
}

void pui_button_set_grow(lv_obj_t *btn, BOOL_T on)
{
    if (btn == NULL || !lv_obj_check_type(btn, MY_BTN)) {
        return;
    }

    lv_obj_set_flex_grow(btn, on ? 1 : 0);
}

void pui_button_set_full(lv_obj_t *btn, BOOL_T on)
{
    if (btn == NULL || !lv_obj_check_type(btn, MY_BTN)) {
        return;
    }

    /* A percentage, not flex_grow: a card stacks its children in a column, so grow would
       spend the free space on the button's *height*. See the note on the declaration. */
    lv_obj_set_width(btn, on ? LV_PCT(100) : LV_SIZE_CONTENT);
}

void pui_button_set_pad(lv_obj_t *btn, lv_coord_t pad)
{
    if (btn == NULL || !lv_obj_check_type(btn, MY_BTN)) {
        return;
    }

    lv_obj_set_style_pad_hor(btn, pad, 0);
}

void pui_button_set_text(lv_obj_t *btn, const char *text)
{
    if (btn == NULL || !lv_obj_check_type(btn, MY_BTN)) {
        return;
    }

    lv_label_set_text(((pui_button_t *)btn)->label, text != NULL ? text : "");
}

lv_obj_t *pui_button_label(lv_obj_t *btn)
{
    if (btn == NULL || !lv_obj_check_type(btn, MY_BTN)) {
        return NULL;
    }

    return ((pui_button_t *)btn)->label;
}

/***********************************************************
******************icon button implementation****************
***********************************************************/

static void __icon_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_icon_button_t *btn = (pui_icon_button_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_size(obj, PUI_ICON_BTN, PUI_ICON_BTN);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);

    btn->glyph = pui_label_create(obj, "", PET_FONT_ICON, PET_COLOR_ON_ACCENT);
    lv_obj_center(btn->glyph);

    pui_priv_pressable(obj);
}

const lv_obj_class_t pui_icon_button_class = {
    .constructor_cb = __icon_constructor,
    .instance_size  = sizeof(pui_icon_button_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_icon_button",
};

lv_obj_t *pui_icon_button_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, PUI_FILL_E fill)
{
    lv_obj_t          *obj = lv_obj_class_create_obj(MY_ICON_BTN, parent);
    pui_icon_button_t *btn;
    const PUI_PAINT_T *paint = pui_paint(tone, fill);

    lv_obj_class_init_obj(obj);

    btn = (pui_icon_button_t *)obj;

    pui_priv_surface(obj, paint, LV_RADIUS_CIRCLE);
    lv_obj_set_style_text_color(btn->glyph, paint->fg, 0);
    lv_label_set_text(btn->glyph, glyph != NULL ? glyph : "");

    if (fill == PUI_FILL_SOLID) {
        pui_priv_press_shadow(obj);
    }

    return obj;
}

/** @brief A compact icon-and-label action with one shared touch target. */
lv_obj_t *pui_tool_button_create(lv_obj_t *parent, const char *glyph, const char *text, PUI_TONE_E tone,
                                 PUI_FILL_E fill)
{
    const PUI_PAINT_T *paint  = pui_paint(tone, fill);
    lv_obj_t          *button = pui_box_create(parent, PUI_FLOW_ROW, TOOL_GAP);
    lv_obj_t          *icon;

    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, 0, TOOL_H);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pui_priv_surface(button, paint, TOOL_RADIUS);
    if (fill != PUI_FILL_SOLID) {
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, PET_COLOR_LINE, 0);
    } else {
        pui_priv_press_shadow(button);
    }
    icon = pui_label_create(button, glyph, PET_FONT_ICON, fill == PUI_FILL_SOLID ? paint->fg : pui_tone_deep(tone));
    LV_UNUSED(icon);
    pui_label_create(button, text, PET_FONT_CJK_SM, fill == PUI_FILL_SOLID ? paint->fg : PET_COLOR_TEXT);
    pui_priv_pressable(button);
    return button;
}

void pui_tool_button_set_stacked(lv_obj_t *btn, BOOL_T on)
{
    if (btn == NULL) {
        return;
    }

    /* The children are built icon-first, so a column flow puts the glyph over the label
       without anything having to be reordered. The gap moves from the column padding to
       the row padding with the flow — both are zeroed on the axis the flow does not use,
       because a stale pad_column on a column flow is a gap the box silently keeps. */
    lv_obj_set_flex_flow(btn, on ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn, on ? 0 : TOOL_GAP, 0);
    lv_obj_set_style_pad_row(btn, on ? TOOL_GAP_STACK : 0, 0);
    lv_obj_set_height(btn, on ? TOOL_H_STACK : TOOL_H);
}
