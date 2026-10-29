/**
 * @file pet_page_memos.c
 * @brief 备忘录列表：勾选完成、删除、清空已完成。
 *
 *     +---------------------------------+
 *     | <  备忘录            清空已完成  |  header
 *     +---------------------------------+
 *     | [ 全部 ] [ 未完成 ] [ 已完成 ]   |  filter
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | [ ] 买咖啡       09:30  [x] | |
 *     | | [x] 回复邮件     10:00  [x] | |
 *     | | [ ] 项目讨论     19:00  [x] | |
 *     | +-----------------------------+ |
 *     +---------------------------------+
 *
 * There is no editing UI and no add button: memos are seeded demo data, and this
 * page only lets the user finish one or delete it. The row is a check row
 * from pet_widgets (the whole row is the touch target) with a time and a trash
 * button added on the right.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include <string.h>

#include "pet_demo.h"
#include "pet_pet.h"
#include "pet_widgets.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* The filter bar sits between the header and the content. The segment itself is
   PET_SEGMENT_H (30), so the bar is 2 px taller than that: one pixel of air above
   and below. */
#define MEMO_FILTER_H 32

#define MEMO_CONFIRM_MAX 128

/***********************************************************
***********************typedef define***********************
***********************************************************/

typedef enum {
    MEMO_FILTER_ALL = 0,
    MEMO_FILTER_OPEN,
    MEMO_FILTER_DONE,
    MEMO_FILTER_MAX,
} MEMO_FILTER_E;

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The filter survives a rebuild: toggling a memo must not send the user back to 全部. */
static MEMO_FILTER_E sg_filter = MEMO_FILTER_ALL;

/* The text of the memo waiting for delete confirmation. The sheet is modal, but a
   model change can still land while it is up and shift the list; matching by text at
   confirm time is safer than remembering an index. */
static char sg_pending_text[PET_TEXT_MAX];

/***********************************************************
********************function define**************************
***********************************************************/

static uint32_t __count_done(const PET_TODO_LIST_T *list)
{
    uint32_t done = 0;

    for (uint32_t i = 0; i < list->count; i++) {
        if (list->items[i].done) {
            done++;
        }
    }

    return done;
}

static BOOL_T __matches(const PET_TODO_T *item)
{
    if (sg_filter == MEMO_FILTER_OPEN) {
        return item->done ? FALSE : TRUE;
    }

    if (sg_filter == MEMO_FILTER_DONE) {
        return item->done ? TRUE : FALSE;
    }

    return TRUE;
}

static void __delete_do_cb(void *user)
{
    PET_TODO_LIST_T *list = pet_demo_todo();

    (void)user;

    for (uint32_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].text, sg_pending_text) == 0) {
            pet_demo_todo_delete(i);
            break;
        }
    }

    pet_ui_badge_refresh();
    pet_say("删掉了");
    pet_ui_refresh();
}

static void __delete_cb(lv_event_t *e)
{
    PET_TODO_LIST_T *list  = pet_demo_todo();
    uint32_t         index = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    char             body[MEMO_CONFIRM_MAX];

    /* The row itself toggles done. A tap on the trash must not also tick the box. */
    lv_event_stop_bubbling(e);

    if (index >= list->count) {
        return;
    }

    lv_snprintf(sg_pending_text, sizeof(sg_pending_text), "%s", list->items[index].text);
    lv_snprintf(body, sizeof(body), "确定删除「%s」？\n删除后无法恢复。", sg_pending_text);
    pet_page_confirm("删除备忘录", body, "删除", __delete_do_cb, NULL);
}

static void __toggle_cb(lv_event_t *e)
{
    PET_TODO_LIST_T *list  = pet_demo_todo();
    uint32_t         index = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    if (index >= list->count) {
        return;
    }

    pet_demo_todo_toggle(index);
    pet_ui_badge_refresh();
    pet_say(list->items[index].done ? "完成啦" : "又变回未完成了");
    pet_ui_refresh();
}

static void __clear_do_cb(void *user)
{
    (void)user;

    pet_demo_todo_clear_done();
    pet_ui_badge_refresh();
    pet_say("清干净啦");
    pet_ui_refresh();
}

static void __clear_cb(lv_event_t *e)
{
    uint32_t done = __count_done(pet_demo_todo());
    char     body[64];

    (void)e;

    if (done == 0) {
        return;
    }

    lv_snprintf(body, sizeof(body), "将删除 %u 条已完成的备忘录。", (unsigned)done);
    pet_page_confirm("清空已完成", body, "清空", __clear_do_cb, NULL);
}

static void __filter_cb(lv_event_t *e)
{
    void    *data;
    uint32_t filter;

    /* pet_widget_segment puts the index on the segment itself; the label on top of it
       is built with pui_text_create(), so it does not swallow the press. Index 0 is
       stored as a NULL user data pointer, which is why NULL means 全部 rather than
       "ignore the tap". */
    data   = lv_obj_get_user_data(lv_event_get_target(e));
    filter = (uint32_t)(uintptr_t)data;

    if (filter >= MEMO_FILTER_MAX || (MEMO_FILTER_E)filter == sg_filter) {
        return;
    }

    sg_filter = (MEMO_FILTER_E)filter;
    pet_ui_refresh();
}

/**
 * @brief One memo: check box, text, time, trash.
 *
 * The check row already owns the tick and the label. The time and the trash are
 * appended after it, so the row keeps the library's press behaviour and the text
 * stays the growing middle column.
 */
static void __build_row(lv_obj_t *parent, uint32_t index)
{
    PET_TODO_LIST_T *list  = pet_demo_todo();
    PET_TODO_T      *item  = &list->items[index];
    lv_obj_t        *row   = pet_widget_check_row(parent, item->text, item->done);
    lv_obj_t        *label = lv_obj_get_child(row, 1);

    if (label != NULL) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
    }

    if (item->when[0] != '\0') {
        lv_obj_t *time = pui_box_create(row, PUI_FLOW_ROW, 3);
        lv_obj_t *bell;
        lv_obj_t *when;

        /* pui_box_create() fills its parent across; this one must be as wide as the
           bell and the time, or it would push the memo text off the row. */
        lv_obj_set_width(time, LV_SIZE_CONTENT);
        lv_obj_set_height(time, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(time, 0, 0);

        bell = pui_label_create(time, FONT_AWESOME_BELL, PET_FONT_ICON, PET_COLOR_TEXT_HINT);
        lv_obj_clear_flag(bell, LV_OBJ_FLAG_CLICKABLE);

        when = pui_label_create(time, item->when, PET_FONT_CJK_SM, PET_COLOR_TEXT_HINT);
        lv_obj_clear_flag(when, LV_OBJ_FLAG_CLICKABLE);
    }

    {
        lv_obj_t *trash = pui_icon_button_create(row, FONT_AWESOME_TRASH, PUI_TONE_RED, PUI_FILL_GHOST);

        lv_obj_add_event_cb(trash, __delete_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    }

    lv_obj_add_event_cb(row, __toggle_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
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

    icon = pui_label_create(box, FONT_AWESOME_BELL, PET_FONT_ICON_LG, PET_COLOR_TEXT_HINT);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    title = pui_text_create(box,
                            (sg_filter == MEMO_FILTER_OPEN)   ? "没有未完成的备忘录"
                            : (sg_filter == MEMO_FILTER_DONE) ? "还没有已完成的备忘录"
                                                              : "还没有备忘录",
                            PUI_TEXT_CARD);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    hint = pui_text_create(
        box, (sg_filter == MEMO_FILTER_DONE) ? "完成一条后会出现在这里" : "对宠物说「记一下，明天买电池」",
        PUI_TEXT_SUB);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_memos_build(lv_obj_t *panel)
{
    static const char *const filters[] = {"全部", "未完成", "已完成"};
    PET_TODO_LIST_T         *list      = pet_demo_todo();
    lv_obj_t                *header;
    lv_obj_t                *filter_bar;
    lv_obj_t                *segment;
    lv_obj_t                *body;
    lv_obj_t                *card;
    lv_obj_t                *rows;
    uint32_t                 shown = 0;

    header = pet_page_header(panel, "备忘录", TRUE);
    pet_page_header_push(header);

    if (__count_done(list) > 0) {
        lv_obj_t *clear = pui_button_create(header, "清空已完成", PUI_TONE_NEUTRAL, PUI_FILL_GHOST, PUI_SIZE_SM);

        lv_obj_add_event_cb(clear, __clear_cb, LV_EVENT_CLICKED, NULL);
    }

    filter_bar = pui_box_create(panel, PUI_FLOW_ROW, 0);
    lv_obj_set_size(filter_bar, LV_PCT(100), MEMO_FILTER_H);
    lv_obj_set_style_pad_hor(filter_bar, PET_PAD_PAGE, 0);
    lv_obj_set_style_pad_ver(filter_bar, 1, 0);

    segment = pet_widget_segment(filter_bar, filters, MEMO_FILTER_MAX, (uint32_t)sg_filter);
    lv_obj_set_flex_grow(segment, 1);
    lv_obj_add_event_cb(segment, __filter_cb, LV_EVENT_CLICKED, NULL);

    body = pet_page_content(panel);
    pui_screen_scrollable(body);

    for (uint32_t i = 0; i < list->count; i++) {
        if (__matches(&list->items[i])) {
            shown++;
        }
    }

    if (shown == 0) {
        __build_empty(body);
        return;
    }

    card = pui_card_create(body, NULL, PUI_TONE_NEUTRAL);
    rows = pui_card_body(card);

    for (uint32_t i = 0; i < list->count; i++) {
        if (__matches(&list->items[i])) {
            __build_row(rows, i);
        }
    }
}
