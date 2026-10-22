/**
 * @file pet_page_settings.c
 * @brief The settings page: four groups of sliders, switches and one cycling choice.
 *
 *     +---------------------------------+
 *     | <  设置                          |  header
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | 宠物设置                     | |  group band
 *     | | 动画速度  [====o----]   60  | |
 *     | | 宠物音量  [===o-----]   50  | |
 *     | | 自动提取待办          [ o ] | |
 *     | +-----------------------------+ |
 *     | +-----------------------------+ |
 *     | | 拍照设置                     | |
 *     | | 拍照倒计时            [ o ] | |
 *     | | 拍照&AI识别           [ o ] | |
 *     | | 照片质量  [===o-----]   70  | |
 *     | | 合影角色位置        居中  v | |
 *     | +-----------------------------+ |
 *     |          ... 备忘录设置 / 系统 ... |
 *     +---------------------------------+
 *
 * The group headings are filled bands rather than the plain captions they used to be:
 * nine rows in a 256 px panel read as one undifferentiated list, and a filled band is
 * what makes the groups legible while scrolling.
 *
 * No action bar: every control writes through to the model as it moves, so there is
 * nothing to save. That is also why the page scrolls — the four groups come to about
 * 540 px against the 264 px the header leaves, and a settings page that hides rows is
 * worse than one that scrolls. The scroll region is set up by pui_screen_scrollable(),
 * which is what lets a drag that lands on a label or a card move the page and not just
 * a drag that happens to land on a slider.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pet_theme.h"
#include "pet_widgets.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief One group of rows: a heading, and the settings that belong under it.
 */
typedef struct {
    const char         *title;
    const PET_SETTING_E rows[6];
    uint32_t            count;
    /* The composite position is a cycling choice rather than a value, so it has no
       PET_SETTING_E slot and cannot live in `rows`. It rides along with the group it
       belongs to instead. */
    BOOL_T has_pos;
} SET_GROUP_T;

/***********************************************************
***********************variable define**********************
***********************************************************/

static const SET_GROUP_T sg_groups[] = {
    {"宠物设置", {PET_SET_ANIM_SPEED, PET_SET_VOLUME, PET_SET_AUTO_TODO}, 3, FALSE},
    {"拍照设置", {PET_SET_SHOT_TIMER, PET_SET_SHOT_AI, PET_SET_QUALITY}, 3, TRUE},
    {"备忘录设置", {PET_SET_OCR}, 1, FALSE},
    {"系统", {PET_SET_BRIGHTNESS}, 1, FALSE},
};

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief A slider moved.
 *
 * The setting's id rides on the slider itself, so one handler serves all six sliders.
 * The row is reached through the slider's parent rather than stashed anywhere: a widget
 * row is exactly one level deep.
 */
static void __slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *row    = lv_obj_get_parent(slider);

    pet_demo_setting_set((PET_SETTING_E)(uintptr_t)lv_obj_get_user_data(slider), (int)lv_slider_get_value(slider));
    pet_widget_slider_row_refresh(row);
}

static void __switch_cb(lv_event_t *e)
{
    lv_obj_t     *sw = lv_event_get_target(e);
    BOOL_T        on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? TRUE : FALSE;
    PET_SETTING_E id = (PET_SETTING_E)(uintptr_t)lv_obj_get_user_data(sw);

    pet_demo_setting_set(id, on ? 1 : 0);
}

static void __pos_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);

    pet_demo_pos_next();
    pet_widget_select_row_set(row, pet_demo_pos_name(pet_demo_pos()));

    pet_say(pet_demo_pos() == PET_POS_LEFT    ? "合影时我站左边"
            : pet_demo_pos() == PET_POS_RIGHT ? "合影时我站右边"
                                              : "合影时我站中间");
}

/**
 * @brief Build one settings row, choosing the widget from the setting itself.
 */
static void __build_row(lv_obj_t *card, PET_SETTING_E id)
{
    const char *name  = pet_demo_setting_name(id);
    int         value = pet_demo_setting_get(id);
    lv_obj_t   *row;

    if (pet_demo_setting_is_toggle(id)) {
        lv_obj_t *sw;

        row = pet_widget_switch_row(card, name, value ? TRUE : FALSE);
        sw  = pet_widget_row_control(row);

        lv_obj_set_user_data(sw, (void *)(uintptr_t)id);
        lv_obj_add_event_cb(sw, __switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    row = pet_widget_slider_row(card, name, value, pet_demo_setting_min(id), pet_demo_setting_max(id));

    lv_obj_set_user_data(pet_widget_row_control(row), (void *)(uintptr_t)id);
    lv_obj_add_event_cb(pet_widget_row_control(row), __slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/**
 * @brief A group: its rows in a card under a heading.
 *
 * One component rather than the two it used to be — a filled bar with a chevron, then a
 * separate card of rows below it. The bar said nothing the card does not: it took no
 * press, so its chevron promised an expansion that did not exist, and the heading reads
 * as the group's name whether it sits above the card or inside it.
 */
static lv_obj_t *__build_group(lv_obj_t *parent, const SET_GROUP_T *group)
{
    lv_obj_t *card = pui_card_create(parent, group->title, PUI_TONE_INDIGO);
    lv_obj_t *body = pui_card_body(card);
    lv_obj_t *last;

    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    pui_card_t *group_card = (pui_card_t *)card;
    lv_obj_set_style_bg_color(group_card->band, pui_tone_wash(PUI_TONE_INDIGO, 160), 0);

    for (uint32_t i = 0; i < group->count; i++) {
        __build_row(body, group->rows[i]);
    }

    if (group->has_pos) {
        lv_obj_t *row = pet_widget_select_row(body, "合影角色位置", pet_demo_pos_name(pet_demo_pos()));

        lv_obj_add_event_cb(row, __pos_cb, LV_EVENT_CLICKED, NULL);
    }

    /* The rule under the last row is the group's own bottom border, drawn a few pixels
       inside it. Every row carries one so that a new row type cannot be added without
       one; this is the only place that knows where the group ends. */
    last = lv_obj_get_child(body, lv_obj_get_child_count(body) - 1);
    if (last != NULL) {
        lv_obj_set_style_border_width(last, 0, 0);
    }

    return card;
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_settings_build(lv_obj_t *panel)
{
    lv_obj_t *body;

    pet_page_header(panel, "设置", TRUE);

    body = pet_page_content(panel);
    /* Nine rows plus three headings come to roughly 540 px in a 246 px area. */
    pui_screen_scrollable(body);

    for (size_t i = 0; i < sizeof(sg_groups) / sizeof(sg_groups[0]); i++) {
        /* Each group is a band. The loop index is the stagger, which is what makes the
           settings page read as a list unrolling rather than as one tall surface. */
        pui_anim_rise_in(__build_group(body, &sg_groups[i]), (uint8_t)i);
    }

    pet_say("有需要就告诉我哦");
}
