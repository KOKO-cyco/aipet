/**
 * @file pet_page_notes.c
 * @brief The notes list: every note the app holds, newest first, each with its time.
 *
 *     +---------------------------------+
 *     | <  全部笔记                      |  header
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | (check) 购物清单      10:20 | |
 *     | | (chat) 会议记录       昨天  | |
 *     | | (pen)  灵感记录       昨天  | |  white card
 *     | | (check) 待办事项      周一  | |
 *     | | (pin)  旅行计划       上周  | |
 *     | +-----------------------------+ |
 *     +---------------------------------+
 *
 * The home page's 最近笔记 card previews the first two of these; this page is where
 * 全部笔记 leads, and it exists because "all" is a promise the card's two rows cannot
 * keep. A row opens the editor, which is the same editor the to-do card leads to — there
 * is one note in the model, so there is one thing to edit.
 *
 * This is the one list whose rows carry a time. A note is a thing that happened, and when
 * it happened is half of what tells a note from its neighbours; a setting has no such
 * second fact, which is why the same row component leaves its trailing slot empty there.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pui.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The mark at the head of each row, cycled by index. The model carries no category, so
   the glyph and the tone are read from the name's own sense — a list to tick, a meeting
   written down, a thought to catch, a place to go — and the two that share a sense share
   a mark. */
static const PUI_TONE_E sg_tones[] = {PUI_TONE_AMBER, PUI_TONE_INDIGO, PUI_TONE_GREEN, PUI_TONE_ACCENT, PUI_TONE_AMBER};
static const char *const sg_glyphs[] = {FONT_AWESOME_CHECK, FONT_AWESOME_COMMENT, FONT_AWESOME_EDIT, FONT_AWESOME_CHECK,
                                        FONT_AWESOME_LOCATION};

/***********************************************************
********************function define**************************
***********************************************************/

static void __open_cb(lv_event_t *e)
{
    (void)e;

    pet_ui_go(PET_PAGE_NOTE);
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_notes_build(lv_obj_t *panel)
{
    lv_obj_t *body = pet_page_content(panel);
    lv_obj_t *card = pui_card_create(body, NULL, PUI_TONE_NEUTRAL);
    lv_obj_t *rows = pui_card_body(card);
    uint32_t  n    = sizeof(sg_tones) / sizeof(sg_tones[0]);

    /* Scrollable so a longer list slides rather than clips. Five rows fit the panel, but
       the count is the model's and the card should not be the thing that decides it. */
    pui_screen_scrollable(body);

    for (uint32_t i = 0; pet_demo_nav_name(i) != NULL; i++) {
        lv_obj_t *row = pui_list_row_create(rows, sg_glyphs[i % n], sg_tones[i % n], pet_demo_nav_name(i));

        pui_list_row_set_trailing(row, pet_demo_nav_time(i));

        /* A row is a container, and a container here takes no input — it would shield
           whatever it sits over. This one is the page's whole point, so it is made a
           target again, with the flat press the library gives a surface that holds
           children. See pui_pressable_flat(). */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        pui_pressable_flat(row);
        lv_obj_add_event_cb(row, __open_cb, LV_EVENT_CLICKED, NULL);
    }

    pet_say("这些都帮你收好了");
}
