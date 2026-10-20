/**
 * @file pui_text.c
 * @brief The text role — what a run of words is for, which decides its face and colour —
 *        and the escape hatch for the few labels a role cannot describe.
 *
 * Without this the library could build a card, a tile and a button but not put a line of
 * text on a page — a page wanting a caption, a chip's label or a note's line had to reach
 * past the components and name a font and a colour itself. Those two are the library's
 * business, and the role is the only part of the decision that belongs to the page.
 *
 * A role is the right answer almost everywhere, which is why there are only six of them.
 * The exceptions are labels that carry their own colour: a widget whose face and fill are
 * a parameter (pet_widgets.c), and the character's speech bubble, which sits on a surface
 * no tone describes. Those go through pui_label_create() and are the reason it exists.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_CLASS (&pui_text_class)

/***********************************************************
********************function define**************************
***********************************************************/

static const lv_font_t *__role_font(PUI_TEXT_E role)
{
    switch (role) {
    case PUI_TEXT_GLYPH:
        return PET_FONT_ICON;
    case PUI_TEXT_GLYPH_LG:
        return PET_FONT_ICON_LG;
    case PUI_TEXT_FIGURE:
        return PET_FONT_CLOCK;
    case PUI_TEXT_TITLE:
        return PET_FONT_CJK_TITLE;
    case PUI_TEXT_CARD:
        return PET_FONT_CJK_CARD;
    case PUI_TEXT_BODY:
    case PUI_TEXT_SUB:
    case PUI_TEXT_HINT:
    default:
        /* The body face, and the only one that carries all of GB2312 — which is why the
           two roles that are not a title are told apart by colour rather than by size.
           See PUI_TEXT_E. */
        return PET_FONT_CJK;
    }
}

static const lv_color_t __role_color(PUI_TEXT_E role)
{
    switch (role) {
    case PUI_TEXT_SUB:
        return PET_COLOR_TEXT_SUB;
    case PUI_TEXT_HINT:
        return PET_COLOR_TEXT_HINT;
    case PUI_TEXT_BODY:
    case PUI_TEXT_GLYPH:
    case PUI_TEXT_GLYPH_LG:
    case PUI_TEXT_FIGURE:
    default:
        return PET_COLOR_TEXT;
    }
}

static void __text_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_text_t *text = (pui_text_t *)obj;

    LV_UNUSED(class_p);

    text->role = PUI_TEXT_BODY;

    /* A label's own defaults are close to what this wants, but not all the way: the
       theme's label style is what a page was fighting before, and saying it once here is
       the point of the component. */
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

const lv_obj_class_t pui_text_class = {
    .constructor_cb = __text_constructor,
    .instance_size  = sizeof(pui_text_t),
    .base_class     = &lv_label_class,
    .name           = "pui_text",
};

lv_obj_t *pui_text_create(lv_obj_t *parent, const char *text, PUI_TEXT_E role)
{
    lv_obj_t   *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    pui_text_t *self;

    lv_obj_class_init_obj(obj);

    self       = (pui_text_t *)obj;
    self->role = (role >= 0 && role < PUI_TEXT_MAX) ? role : PUI_TEXT_BODY;

    lv_obj_set_style_text_font(obj, __role_font(self->role), 0);
    lv_obj_set_style_text_color(obj, __role_color(self->role), 0);
    lv_label_set_text(obj, text != NULL ? text : "");

    return obj;
}

void pui_text_set(lv_obj_t *text, const char *str)
{
    if (text == NULL || !lv_obj_check_type(text, MY_CLASS)) {
        return;
    }

    lv_label_set_text(text, str != NULL ? str : "");
}

void pui_text_set_tone(lv_obj_t *text, PUI_TONE_E tone)
{
    if (text == NULL || !lv_obj_check_type(text, MY_CLASS)) {
        return;
    }

    lv_obj_set_style_text_color(text, pui_tone_hue(tone), 0);
}

lv_obj_t *pui_label_create(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    /* Built as a pui_text so that a label a component makes internally and one a page
       makes through the public API are the same object, then re-dressed: a component
       knows the exact face and colour it wants, which is the part a role cannot say. */
    lv_obj_t *label = pui_text_create(parent, text, PUI_TEXT_BODY);

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);

    return label;
}
