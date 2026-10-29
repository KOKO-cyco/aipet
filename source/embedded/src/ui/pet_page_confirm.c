/**
 * @file pet_page_confirm.c
 * @brief The shared destructive-action sheet: a title, copy, and 取消 / action.
 *
 * The memo and note lists have no editing UI on this build, so delete is the only
 * thing a row can do that changes the model. Both lists use this sheet so the
 * confirmation reads the same wherever it is raised.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pui.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

/* Only one confirm sheet can be up at a time — pet_page_sheet() enforces that — so
   the pending callback can live here rather than in the sheet's user data. */
static PET_CONFIRM_CB sg_on_confirm;
static void          *sg_user;

/***********************************************************
********************function define**************************
***********************************************************/

static void __cancel_cb(lv_event_t *e)
{
    (void)e;

    pet_page_sheet_close();
}

static void __run_confirm(void *arg)
{
    PET_CONFIRM_CB cb   = sg_on_confirm;
    void          *user = sg_user;

    (void)arg;

    sg_on_confirm = NULL;
    sg_user       = NULL;

    if (cb != NULL) {
        cb(user);
    }
}

static void __confirm_cb(lv_event_t *e)
{
    (void)e;

    /* Close first, then run the callback on the next LVGL turn. The callback usually
       rebuilds the page, and rebuilding empties the trash — which would delete the
       sheet while this very button callback is still on the stack. */
    pet_page_sheet_close();
    lv_async_call(__run_confirm, NULL);
}

void pet_page_confirm(const char *title, const char *body, const char *action, PET_CONFIRM_CB on_confirm, void *user)
{
    lv_obj_t *sheet = pet_page_sheet();
    lv_obj_t *card;
    lv_obj_t *content;
    lv_obj_t *text;
    lv_obj_t *buttons;
    lv_obj_t *btn;

    if (sheet == NULL) {
        return;
    }

    sg_on_confirm = on_confirm;
    sg_user       = user;

    lv_obj_add_event_cb(sheet, __cancel_cb, LV_EVENT_CLICKED, NULL);

    card    = pui_card_create(sheet, title, PUI_TONE_NEUTRAL);
    content = pui_card_body(card);

    text = pui_text_create(content, body, PUI_TEXT_BODY);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    buttons = pui_box_create(content, PUI_FLOW_ROW, PUI_GRID_GAP);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn = pui_button_create(buttons, "取消", PUI_TONE_NEUTRAL, PUI_FILL_GHOST, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __cancel_cb, LV_EVENT_CLICKED, NULL);

    btn = pui_button_create(buttons, action, PUI_TONE_RED, PUI_FILL_SOLID, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __confirm_cb, LV_EVENT_CLICKED, NULL);

    pui_anim_sheet_in(sheet);
}
