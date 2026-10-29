/**
 * @file pet_page_notes.c
 * @brief 全部笔记：带彩色图标分类的只读列表，可删除。
 *
 *     +---------------------------------+
 *     | <  全部笔记                     |  header
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | (check) 购物清单  10:20 [x] | |
 *     | | (chat)  会议记录  昨天  [x] | |
 *     | | (pen)   灵感记录  昨天  [x] | |
 *     | | (pin)   旅行计划  上周  [x] | |
 *     +---------------------------------+
 *
 * The leading square is the old 全部笔记 page's categorisation: a tone and a glyph
 * per note, so the list reads as different kinds of note at a glance rather than as
 * four identical rows. The note model has no category field, so the mark is chosen
 * from the note's own sense — a list to tick, a meeting, a thought, a trip.
 *
 * There is no editor and no add button: notes are seeded demo data, and the only
 * action here is the trash at the end of the row.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include <string.h>

#include "pet_demo.h"
#include "pet_pet.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

#define NOTE_CONFIRM_MAX 128

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The category marks, cycled by index. They are the old 全部笔记 page's order:
   a checklist, a conversation, a written thought, a place. */
static const PUI_TONE_E  sg_tones[]  = {PUI_TONE_AMBER, PUI_TONE_INDIGO, PUI_TONE_GREEN, PUI_TONE_ACCENT};
static const char *const sg_glyphs[] = {FONT_AWESOME_CHECK, FONT_AWESOME_COMMENT, FONT_AWESOME_EDIT,
                                        FONT_AWESOME_LOCATION};

/* The title of the note waiting for delete confirmation. Matched at confirm time:
   a model change can land while the sheet is up and shift the list. */
static char sg_pending_title[PET_TEXT_MAX];

/***********************************************************
********************function define**************************
***********************************************************/

static void __delete_do_cb(void *user)
{
    (void)user;

    for (uint32_t i = 0; i < pet_demo_note_count(); i++) {
        const PET_NOTE_T *note = pet_demo_note_at(i);

        if (note != NULL && strcmp(note->title, sg_pending_title) == 0) {
            pet_demo_note_delete(i);
            break;
        }
    }

    pet_say("笔记删掉了");
    pet_ui_refresh();
}

static void __delete_cb(lv_event_t *e)
{
    const PET_NOTE_T *note = pet_demo_note_at((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    char              body[NOTE_CONFIRM_MAX];

    lv_event_stop_bubbling(e);

    if (note == NULL) {
        return;
    }

    lv_snprintf(sg_pending_title, sizeof(sg_pending_title), "%s", note->title);
    lv_snprintf(body, sizeof(body), "确定删除「%s」？\n删除后无法恢复。", sg_pending_title);
    pet_page_confirm("删除笔记", body, "删除", __delete_do_cb, NULL);
}

static void __build_empty(lv_obj_t *body)
{
    lv_obj_t *card = pui_card_create(body, NULL, PUI_TONE_NEUTRAL);
    lv_obj_t *box  = pui_card_body(card);
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *hint;

    lv_obj_set_height(card, 0);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(box, 0);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    icon = pui_label_create(box, FONT_AWESOME_EDIT, PET_FONT_ICON_LG, PET_COLOR_TEXT_HINT);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    title = pui_text_create(box, "还没有笔记", PUI_TEXT_CARD);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    hint = pui_text_create(box, "笔记会出现在这里", PUI_TEXT_SUB);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_notes_build(lv_obj_t *panel)
{
    lv_obj_t *header = pet_page_header(panel, "全部笔记", TRUE);
    lv_obj_t *body;
    lv_obj_t *card;
    lv_obj_t *rows;
    uint32_t  marks = sizeof(sg_tones) / sizeof(sg_tones[0]);

    pet_page_header_push(header);

    body = pet_page_content(panel);
    pui_screen_scrollable(body);

    if (pet_demo_note_count() == 0) {
        __build_empty(body);
        return;
    }

    card = pui_card_create(body, NULL, PUI_TONE_NEUTRAL);
    rows = pui_card_body(card);

    for (uint32_t i = 0; i < pet_demo_note_count(); i++) {
        const PET_NOTE_T *note = pet_demo_note_at(i);
        lv_obj_t         *row;
        lv_obj_t         *trash;

        if (note == NULL) {
            continue;
        }

        row = pui_list_row_create(rows, sg_glyphs[i % marks], sg_tones[i % marks], note->title);
        pui_list_row_set_trailing(row, note->time);

        trash = pui_icon_button_create(row, FONT_AWESOME_TRASH, PUI_TONE_RED, PUI_FILL_GHOST);
        lv_obj_add_event_cb(trash, __delete_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}
