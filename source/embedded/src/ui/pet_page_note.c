/**
 * @file pet_page_note.c
 * @brief The note editor: a title, a body, four tools, and the two sheets the tools
 *        raise.
 *
 *     +---------------------------------+
 *     | <  备忘录              [ 保存 ]  |  header
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | 今日待办               [+]  | |  lavender
 *     | | +-------------------------+ | |
 *     | | | (bell) 提醒 15日 19:00  | | |
 *     | | |  · 拍照记录便签         | | |  white body
 *     | | |  · 提取待办事项         | | |
 *     | | |  · 周三买电池           | | |
 *     | | +-------------------------+ | |
 *     | +-----------------------------+ |
 *     +---------------------------------+
 *     | [ 待办 ][ 提醒 ][ 图片 ][AI总结 ] |  toolbar
 *     +---------------------------------+
 *
 * The lavender surface is what tells this page apart from every other white card in
 * the app: writing is the one thing here that is not just a list, and the page should
 * not look like the settings page with different rows in it.
 *
 * The body is read-only this round. Editing it means a textarea, a soft keyboard and a
 * font that can render whatever the user types, and the layout is what this pass is
 * for; the lines come from the model instead.
 *
 * Neither sheet writes down a width. They used to: NOTE_SHEET_W, NOTE_SHEET_INNER,
 * NOTE_ROLLER_W and NOTE_PICK_W were four derived numbers that had to be recomputed
 * together whenever the panel or a card's padding moved, and the border term in the
 * second of them was a fudge that only worked while the third stayed under it. The
 * scrim insets the sheet and the grid divides what is left.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pet_widgets.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

#define NOTE_SAY_MAX 64

/* The roller's height. Three visible rows of the 18 px face plus the roller's own
   padding — lv_roller will not size itself to a visible row count, so this is the one
   number on the page a component cannot own. */
#define NOTE_ROLLER_H 72

/* "31\n30\n..." is at most three bytes an entry. */
#define NOTE_OPTS_MAX 192

/* The picker's thumbnail hues, cycled by index. There is no image data to take a colour
   from, and a sheet of five identical grey blocks says nothing about being a choice
   between five different photos. */
static const PUI_TONE_E sg_pick_tones[] = {PUI_TONE_ACCENT, PUI_TONE_INDIGO, PUI_TONE_AMBER, PUI_TONE_GREEN,
                                           PUI_TONE_NEUTRAL};

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The page's reminder chip, and the sheet's rollers. All three outlive the sheet,
   which is why the rollers are cleared by the sheet's builder rather than assumed. */
static lv_obj_t *sg_remind_label;
static lv_obj_t *sg_rollers[3];

/***********************************************************
********************function define**************************
***********************************************************/

static void __refresh_remind(void)
{
    char text[NOTE_SAY_MAX];

    if (sg_remind_label == NULL) {
        return;
    }

    /* No month in the model — the reminder is a day-of-month plus a time, which is as
       much as the roller sheet edits. */
    lv_snprintf(text, sizeof(text), "提醒 %u日 %02u:%02u", pet_demo_remind_day(), pet_demo_remind_hour(),
                pet_demo_remind_min());
    pui_text_set(sg_remind_label, text);
}

/**
 * @brief Write "lo\nlo+1\n...\nhi" into @p buf, as an lv_roller wants it.
 *
 * Stops at a line boundary if the buffer runs out, so a truncated list never ends in
 * half a number.
 */
static void __fill_range(char *buf, size_t cap, int lo, int hi)
{
    size_t used = 0;

    if (cap == 0) {
        return;
    }

    buf[0] = '\0';

    for (int v = lo; v <= hi; v++) {
        int n;

        /* Four bytes covers the widest entry ("31\n") plus the terminator. */
        if (used + 4 >= cap) {
            buf[used] = '\0';
            return;
        }

        n = lv_snprintf(buf + used, cap - used, "%d%s", v, v < hi ? "\n" : "");
        if (n <= 0) {
            buf[used] = '\0';
            return;
        }

        used += (size_t)n;
    }
}

/* ---- Reminder sheet -------------------------------------------------------- */

static void __sheet_close_cb(lv_event_t *e)
{
    (void)e;

    pet_page_sheet_close();
}

static void __remind_ok_cb(lv_event_t *e)
{
    (void)e;

    /* The model offers a relative step rather than a setter, so hand it the difference
       between what the rollers show and what it holds. Its wraparound is a no-op for a
       delta that lands on the target exactly. */
    pet_demo_remind_step((int)lv_roller_get_selected(sg_rollers[0]) + 1 - (int)pet_demo_remind_day(),
                         (int)lv_roller_get_selected(sg_rollers[1]) - (int)pet_demo_remind_hour(),
                         (int)lv_roller_get_selected(sg_rollers[2]) - (int)pet_demo_remind_min());

    pet_page_sheet_close();
    __refresh_remind();
    pet_say("提醒设好了，到点我会叫你");
}

static void __remind_open_cb(lv_event_t *e)
{
    static const char *const titles[] = {"日", "时", "分"};
    static char              opts[3][NOTE_OPTS_MAX];
    lv_obj_t                *sheet = pet_page_sheet();
    lv_obj_t                *card;
    lv_obj_t                *body;
    lv_obj_t                *grid;
    lv_obj_t                *buttons;
    lv_obj_t                *btn;

    (void)e;

    if (sheet == NULL) {
        return;
    }

    lv_obj_add_event_cb(sheet, __sheet_close_cb, LV_EVENT_CLICKED, NULL);

    __fill_range(opts[0], sizeof(opts[0]), 1, 31);
    __fill_range(opts[1], sizeof(opts[1]), 0, 23);
    __fill_range(opts[2], sizeof(opts[2]), 0, 59);

    /* A plain white card, centred by the scrim's own alignment. Its width is the scrim's
       less the scrim's padding, so nothing here names a sheet width. */
    card = pui_card_create(sheet, "设置提醒时间", PUI_TONE_NEUTRAL);
    body = pui_card_body(card);

    /* Three equal columns, each a caption over its roller. The rollers are LV_PCT(100)
       of a cell the grid divides, which is what replaced NOTE_ROLLER_W. */
    grid = pui_grid_create(body, 3, PUI_GRID_GAP);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = pui_box_create(grid, PUI_FLOW_COLUMN, 4);
        uint32_t  sel;

        pui_text_create(col, titles[i], PUI_TEXT_SUB);

        /* The rollers carry zero-based selections; the day's options start at 1. */
        sel = (i == 0) ? pet_demo_remind_day() - 1 : (i == 1) ? pet_demo_remind_hour() : pet_demo_remind_min();

        sg_rollers[i] = pet_widget_roller(col, opts[i], sel, LV_PCT(100));
        lv_obj_set_height(sg_rollers[i], NOTE_ROLLER_H);
    }

    buttons = pui_box_create(body, PUI_FLOW_ROW, PUI_GRID_GAP);
    /* Right-aligned rather than spread: a sheet's buttons are a confirmation, and the
       pair belongs under the thing being confirmed. */
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn = pui_button_create(buttons, "取消", PUI_TONE_NEUTRAL, PUI_FILL_GHOST, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __sheet_close_cb, LV_EVENT_CLICKED, NULL);

    btn = pui_button_create(buttons, "确认", PUI_TONE_ACCENT, PUI_FILL_SOLID, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __remind_ok_cb, LV_EVENT_CLICKED, NULL);

    pui_anim_sheet_in(sheet);
}

/* ---- Image picker sheet ---------------------------------------------------- */

static void __pick_tile_cb(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_current_target(e);
    uint32_t  idx  = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    pet_demo_shot_toggle(idx);
    pui_tile_set_selected(tile, pet_demo_shots()[idx].selected);
}

static void __pick_ok_cb(lv_event_t *e)
{
    char     text[NOTE_SAY_MAX];
    uint32_t picked = pet_demo_shot_selected();

    (void)e;

    pet_page_sheet_close();
    pet_demo_shot_clear_selection();

    if (picked == 0) {
        pet_say_mood("没选图片，笔记先不动", PET_SAY_ALERT);
        return;
    }

    lv_snprintf(text, sizeof(text), "插进笔记了%u张图片", (unsigned)picked);
    pet_say(text);
}

static void __pick_open_cb(lv_event_t *e)
{
    lv_obj_t *sheet = pet_page_sheet();
    lv_obj_t *card;
    lv_obj_t *body;
    lv_obj_t *grid;
    lv_obj_t *buttons;
    lv_obj_t *btn;
    uint32_t  count;

    (void)e;

    if (sheet == NULL) {
        return;
    }

    lv_obj_add_event_cb(sheet, __sheet_close_cb, LV_EVENT_CLICKED, NULL);

    card = pui_card_create(sheet, "选择图片", PUI_TONE_NEUTRAL);
    body = pui_card_body(card);
    grid = pui_grid_create(body, 3, PUI_GRID_GAP);

    count = pet_demo_shot_count();
    for (uint32_t i = 0; i < count; i++) {
        const PET_SHOT_T *shot = &pet_demo_shots()[i];
        PUI_TONE_E        tone = sg_pick_tones[i % (sizeof(sg_pick_tones) / sizeof(sg_pick_tones[0]))];
        lv_obj_t         *tile = pui_tile_create(grid, FONT_AWESOME_IMAGE, tone, PUI_FILL_SOFT, shot->caption);

        /* The gallery's thumbnail, one size down: a block over its caption, so what the
           sheet offers is recognisably the same thing the gallery showed. */
        pui_tile_set_box_h(tile, PUI_TILE_PICK_H);
        pui_tile_set_wide(tile, TRUE);
        pui_tile_set_graded(tile, TRUE);
        pui_tile_set_selected(tile, shot->selected);

        lv_obj_add_event_cb(tile, __pick_tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    buttons = pui_box_create(body, PUI_FLOW_ROW, PUI_GRID_GAP);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn = pui_button_create(buttons, "取消", PUI_TONE_NEUTRAL, PUI_FILL_GHOST, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __sheet_close_cb, LV_EVENT_CLICKED, NULL);

    btn = pui_button_create(buttons, "确定选择", PUI_TONE_ACCENT, PUI_FILL_SOLID, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __pick_ok_cb, LV_EVENT_CLICKED, NULL);

    pui_anim_sheet_in(sheet);
}

/* ---- Toolbar --------------------------------------------------------------- */

static void __to_todo_cb(lv_event_t *e)
{
    char     text[NOTE_SAY_MAX];
    uint32_t added = 0;

    (void)e;

    for (uint32_t i = 0; pet_demo_note_line(i) != NULL; i++) {
        if (pet_demo_todo_add(pet_demo_note_line(i))) {
            added++;
        }
    }

    /* The badge counts what is open, so it has to follow the additions. */
    pet_ui_badge_refresh();

    if (added == 0) {
        pet_say("待办已经满了，先清掉几条吧");
        return;
    }

    lv_snprintf(text, sizeof(text), "这%u条都加进待办了", (unsigned)added);
    pet_say(text);
}

static void __summary_cb(lv_event_t *e)
{
    (void)e;

    pet_say("总结好了：周三买电池");
}

static void __save_cb(lv_event_t *e)
{
    (void)e;

    pet_say_mood("笔记保存好了", PET_SAY_HAPPY);
}

/* ---- Body ------------------------------------------------------------------ */

/**
 * @brief The note's title and its one action.
 *
 * Transparent rather than a card: it sits on the lavender surface, and a white bar on
 * top of it would cut the page into three strips instead of two.
 */
static lv_obj_t *__build_body(lv_obj_t *parent)
{
    lv_obj_t *paper = pui_card_create(parent, NULL, PUI_TONE_NEUTRAL);
    lv_obj_t *body  = pui_card_body(paper);
    lv_obj_t *chip;
    lv_obj_t *label;

    lv_obj_set_height(paper, 0);
    lv_obj_set_flex_grow(paper, 1);
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_radius(paper, 8, 0);
    lv_obj_set_style_shadow_width(paper, 0, 0);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    pui_screen_scrollable(body);

    label = pui_text_create(body, pet_demo_todo()->title, PUI_TEXT_CARD);
    lv_obj_set_width(label, LV_PCT(100));
    /* Model text must use the complete face, not the chrome subset. */
    lv_obj_set_style_text_font(label, PET_FONT_CJK, 0);
    for (uint32_t i = 0; pet_demo_note_line(i) != NULL; i++) {
        label = pui_text_create(body, pet_demo_note_line(i), PUI_TEXT_BODY);
        lv_obj_set_width(label, LV_PCT(100));
    }
    pui_spacer(body);
    chip = pui_box_create(body, PUI_FLOW_ROW, 5);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pui_label_create(chip, FONT_AWESOME_BELL, PET_FONT_ICON, pui_tone_deep(PUI_TONE_INDIGO));
    sg_remind_label = pui_label_create(chip, "", PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    __refresh_remind();
    return paper;
}

static lv_obj_t *__build_toolbar(lv_obj_t *panel)
{
    lv_obj_t *bar = pet_page_action_bar(panel, 6);
    lv_obj_t *btn;

    btn = pui_tool_button_create(bar, FONT_AWESOME_CHECK, "待办", PUI_TONE_INDIGO, PUI_FILL_OUTLINE);
    lv_obj_add_event_cb(btn, __to_todo_cb, LV_EVENT_CLICKED, NULL);
    btn = pui_tool_button_create(bar, FONT_AWESOME_BELL, "提醒", PUI_TONE_INDIGO, PUI_FILL_OUTLINE);
    lv_obj_add_event_cb(btn, __remind_open_cb, LV_EVENT_CLICKED, NULL);
    btn = pui_tool_button_create(bar, FONT_AWESOME_IMAGE, "图片", PUI_TONE_ACCENT, PUI_FILL_OUTLINE);
    lv_obj_add_event_cb(btn, __pick_open_cb, LV_EVENT_CLICKED, NULL);
    btn = pui_tool_button_create(bar, FONT_AWESOME_AI_CHIP, "AI", PUI_TONE_INDIGO, PUI_FILL_SOLID);
    lv_obj_add_event_cb(btn, __summary_cb, LV_EVENT_CLICKED, NULL);
    return bar;
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_note_build(lv_obj_t *panel)
{
    lv_obj_t *header;
    lv_obj_t *body;
    lv_obj_t *btn;

    sg_remind_label = NULL;
    sg_rollers[0]   = NULL;
    sg_rollers[1]   = NULL;
    sg_rollers[2]   = NULL;

    header = pet_page_header(panel, "备忘录", TRUE);
    pet_page_header_push(header);

    btn = pui_button_create(header, "保存", PUI_TONE_ACCENT, PUI_FILL_OUTLINE, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __save_cb, LV_EVENT_CLICKED, NULL);

    body = pet_page_content(panel);

    /* The paper, then the toolbar under it: the page is read top to bottom and arrives
       that way. */
    pui_anim_rise_in(__build_body(body), 0);
    pui_anim_rise_in(__build_toolbar(panel), 1);

    pet_say("我帮你记下来");
}
