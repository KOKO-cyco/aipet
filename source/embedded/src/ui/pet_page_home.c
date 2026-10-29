/**
 * @file pet_page_home.c
 * @brief The home page: a card of three entries over two summary cards side by side.
 *
 *     +-----------------------------------------+
 *     | +-------------------------------------+ |
 *     | |  (cam)     (pen)      (gear)        | |
 *     | |  拍照      备忘录      设置          | |  entries
 *     | +-------------------------------------+ |
 *     | +------------------+ +----------------+ |
 *     | | 待办           ›  | | 笔记         ›  | |
 *     | |  3 未完成         | |  ▍购物清单      | |
 *     | |  1 已完成         | |  ▍灵感记录      | |
 *     | +------------------+ +----------------+ |
 *     +-----------------------------------------+
 *
 * The layout is the reference design's: one full-width card of entries, then two tinted
 * cards sharing the row below it. The two are a grid, so they are equal whatever the panel
 * turns out to be wide, and neither card knows how wide it is.
 *
 * The two cards are narrow — a 256 px panel split in two, less the card's own padding — so
 * their contents are written to fit it. The headings are the model's own four characters,
 * the stat labels are two rather than four, and the note rows carry a stripe rather than a
 * square plus a gap; the rows elide as a backstop, but eliding a four-character name to
 * "灵感…" is not a design, so the budget is respected up front. See the notes on
 * LIST_STRIPE and PUI_CARD_PAD.
 *
 * This is the root of the panel's stack, so it carries no header and no back arrow: there
 * is nothing behind it to go back to, and the entries are the way out. Dropping the header
 * is what buys them their size — at 286 px of panel there is no room for a title bar that
 * says nothing the entries do not.
 *
 * 相册 is not here. Photos come from the camera, so the gallery hangs off the camera page
 * instead of taking one of the three entries. 笔记 is not an entry either: the 最近笔记
 * card below is its way in, and the notes themselves are not typed on this screen.
 *
 * There is no arithmetic in this file. Every size on the page is a component's own: the
 * tiles report their height, the grid divides the width, the cards own their padding. The
 * version before this one computed `PUI_TILE_ICON + 6 + PET_LINE_H + 2` for a tile and
 * pinned it, because a nested flex container measured inside a row comes back with a stale
 * height — which is the kind of thing that stops being true when a font changes.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* Enough for the longest line the pet says here, which is 14 Chinese characters. */
#define HOME_SAY_MAX 64

/* How many recent notes the card lists before it defers to 全部笔记. Two is what the card
   has room for beside the to-do card without either of them growing taller than the row
   the grid gives them. */
#define HOME_NOTE_ROWS 2

/* The squares at the head of the note rows. The model has no categories, so the colour is
   decoration — but it is what makes the list read as a list rather than as two more lines
   of text, which is what the reference design does with the same two rows. The glyphs are
   a checklist and a pencil, which is as much as the model's two names say about themselves:
   购物清单 is something to tick off and 会议记录 is something written down. */
static const PUI_TONE_E  sg_note_tones[HOME_NOTE_ROWS]  = {PUI_TONE_AMBER, PUI_TONE_GREEN};
static const char *const sg_note_glyphs[HOME_NOTE_ROWS] = {FONT_AWESOME_CHECK, FONT_AWESOME_EDIT};

/***********************************************************
********************function define**************************
***********************************************************/

static void __goto_cb(lv_event_t *e)
{
    pet_ui_go((PET_PAGE_E)(uintptr_t)lv_event_get_user_data(e));
}

/* ---- Entries --------------------------------------------------------------- */

/**
 * @brief One entry: a saturated squircle, a caption, and the page it opens.
 *
 * The square is filled at the tone's full strength with the glyph in white on top — the
 * reference design's app-icon treatment, and the reason these were reading as an app
 * grid's placeholder cells. A wash of the hue with a glyph in the hue's own deep colour
 * is a *chip*: two tones of one colour, no figure and no ground, which is what a greyed
 * out slot looks like. Full strength behind white is what a real icon looks like, and it
 * is the only place on the page where a colour is used at strength rather than as a tint.
 *
 * The camera gets a drawn glyph rather than a font one — the icon face carries no camera —
 * so its square is left empty by the constructor and filled here, in the same white the
 * font glyphs beside it are set in.
 *
 * The three colours are the page's whole colour story: the camera's blue, the memo's
 * amber, the settings' blue. Nothing else on the page is filled at strength.
 */
static void __build_entry(lv_obj_t *row, const char *glyph, PUI_TONE_E tone, const char *label, PET_PAGE_E page)
{
    lv_obj_t *tile = pui_tile_create(row, NULL, tone, PUI_FILL_SOFT, label);

    if (glyph == NULL) {
        pui_camera_glyph(pui_tile_box(tile), 36, PET_COLOR_ON_ACCENT);
    } else if (page == PET_PAGE_SETTINGS) {
        pui_gear_glyph(pui_tile_box(tile), 36, pui_tone_deep(tone));
    } else {
        lv_obj_t *icon = pui_label_create(pui_tile_box(tile), glyph, PET_FONT_ICON_LG, pui_tone_deep(tone));

        lv_obj_center(icon);
    }

    lv_obj_add_event_cb(tile, __goto_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);
}

static lv_obj_t *__build_entries(lv_obj_t *parent)
{
    lv_obj_t *card = pui_card_create(parent, NULL, PUI_TONE_NEUTRAL);
    lv_obj_t *row  = pui_box_create(pui_card_body(card), PUI_FLOW_ROW, PUI_TILE_GAP_X);

    lv_obj_set_height(card, 88);
    lv_obj_set_height(pui_card_body(card), 0);
    lv_obj_set_flex_grow(pui_card_body(card), 1);

    __build_entry(row, NULL, PUI_TONE_ACCENT, "拍照", PET_PAGE_CAMERA);
    __build_entry(row, FONT_AWESOME_BELL, PUI_TONE_AMBER, "备忘录", PET_PAGE_MEMOS);
    __build_entry(row, FONT_AWESOME_GEAR, PUI_TONE_ACCENT, "设置", PET_PAGE_SETTINGS);

    return card;
}

/* ---- Cards ----------------------------------------------------------------- */

/**
 * @brief The capsule at a summary card's foot.
 *
 * Its own box rather than a child of the card, because LVGL has no margins: the card's
 * body stacks its children with PUI_CARD_GAP between them, and a card whose rows are
 * separated by their own hairlines wants that gap set to zero — which leaves the button
 * touching the last row. The box carries the air instead.
 *
 * Pinned to the foot by a spacer, because the grid stretches both cards to a shared row
 * height and the shorter of the two would otherwise leave its button floating just under
 * its contents with a band of empty card below.
 *
 * The button is grown and unpadded: the card's content is 93 px and 查看全部 is 72 of
 * them, so a 10 px side padding would leave the label 1 px of air in a card that is
 * already the narrowest surface in the app. Grown with no padding the label is centred in
 * the full width, which is what the reference's capsule is.
 */
static void __build_foot(lv_obj_t *body, const char *label, PUI_TONE_E tone, PET_PAGE_E page)
{
    lv_obj_t *foot = pui_box_create(body, PUI_FLOW_ROW, 0);
    lv_obj_t *btn;

    lv_obj_set_style_pad_top(foot, PUI_CARD_GAP, 0);

    btn = pui_button_create(foot, label, tone, PUI_FILL_SOFT, PUI_SIZE_SM);
    pui_button_set_grow(btn, TRUE);
    pui_button_set_pad(btn, 0);

    lv_obj_add_event_cb(btn, __goto_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);
}

/**
 * @brief 待办: what is open, what is done, and a way to the whole list.
 *
 * The two numbers are the model's — the card cannot drift from the badge in the status
 * strip or from the list the card opens. They are also the only place in the app where
 * type size carries meaning: the figures are drawn in the 24 px medium face and their
 * labels in the 18 px regular one, which is what makes a two-line card read as a summary
 * rather than as a paragraph.
 */
static lv_obj_t *__build_todo_card(lv_obj_t *parent)
{
    PET_TODO_LIST_T *todo    = pet_demo_todo();
    uint32_t         pending = pet_demo_todo_pending();
    /* The heading is the model's own 今日待办, which these cards are now wide enough to
       take: the 16 px card face sets it in about 64 px of the 98 the heading row gives it,
       and the chevron that used to share the row is gone — the card's footer capsule
       already says the card is a target, and the heading had more right to the space. */
    lv_obj_t *card = pui_card_create(parent, "今日备忘录", PUI_TONE_AMBER);
    lv_obj_t *body = pui_card_body(card);
    char      text[HOME_SAY_MAX];

    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);

    lv_snprintf(text, sizeof(text), "%u", (unsigned)pending);
    pui_stat_row_create(body, text, "未完成", PUI_TONE_AMBER);

    lv_snprintf(text, sizeof(text), "%u", (unsigned)(todo->count - pending));
    pui_stat_row_create(body, text, "已完成", PUI_TONE_GREEN);

    pui_spacer(body);
    __build_foot(body, "查看全部", PUI_TONE_AMBER, PET_PAGE_MEMOS);

    pui_card_set_action(card, __goto_cb, (void *)(uintptr_t)PET_PAGE_MEMOS);

    return card;
}

/**
 * @brief 最近笔记: the last couple of lists the note page holds.
 *
 * The rows are the note page's own navigation entries rather than a second list kept here,
 * so the two can never disagree about what the notes are called.
 */
static lv_obj_t *__build_note_card(lv_obj_t *parent)
{
    /* 笔记 and not 最近笔记, for the same width reason as 待办 above. The card lists the note
       page's own navigation entries rather than a second list kept here, so the two can
       never disagree about what the notes are called — only about how many of them fit. */
    lv_obj_t *card = pui_card_create(parent, "最近笔记", PUI_TONE_ACCENT);
    lv_obj_t *body = pui_card_body(card);

    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);

    /* Names only. The card is 98 px of usable width, and a name plus a time does not fit
       there — the row's own trailing slot is for the full-width list page, where there is
       room for both. Here the time would take a third of the width and force the name to
       wrap, which is worse than not showing it. */
    for (uint32_t i = 0; i < HOME_NOTE_ROWS; i++) {
        const PET_NOTE_T *note = pet_demo_note_at(i);

        if (note == NULL) {
            break;
        }

        pui_list_row_create(body, sg_note_glyphs[i], sg_note_tones[i], note->title);
    }

    pui_spacer(body);
    __build_foot(body, "全部笔记", PUI_TONE_ACCENT, PET_PAGE_NOTES);

    /* The whole card opens the notes list rather than the editor: 最近笔记 is a preview of
       the list, so tapping it should show the rest of it, not drop the user into one
       note's body. The editor is one tap further, from a row on that list. */
    pui_card_set_action(card, __goto_cb, (void *)(uintptr_t)PET_PAGE_NOTES);

    return card;
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_home_build(lv_obj_t *panel)
{
    lv_obj_t *body = pui_screen_content(panel);
    lv_obj_t *grid;
    lv_obj_t *card;

    /* Still scrollable: the layout fits the panel now, but a card that grows a row later
       should scroll rather than clip. */
    pui_screen_scrollable(body);

    /* The page arrives in three bands, in the order it is read. The entries card is one;
       the two summary cards are the second and third, and they stagger against each other
       rather than against the row they share — the row is a track, not a thing. */
    card = __build_entries(body);
    pui_anim_rise_in(card, 0);

    grid = pui_box_create(body, PUI_FLOW_ROW, PUI_GRID_GAP);
    lv_obj_set_height(grid, 0);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    card = __build_todo_card(grid);
    lv_obj_set_width(card, 0);
    lv_obj_set_flex_grow(card, 1);
    pui_anim_rise_in(card, 1);
    card = __build_note_card(grid);
    lv_obj_set_width(card, 0);
    lv_obj_set_flex_grow(card, 1);
    pui_anim_rise_in(card, 2);

    pet_say("今天也一起记录吧");
}
