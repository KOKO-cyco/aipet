/**
 * @file pui_screen.c
 * @brief Page furniture: the header, the content well, the action bar, the scrim.
 *
 * A page is a flex column of exactly these, in this order, and every page in the app is
 * the same four pieces. They were in pet_ui.c as loose functions that a page had to be
 * trusted to call correctly; they are here so that the header's height, the page's margin
 * and the action bar's padding are stated once instead of once per page.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The gap between a header's leading arrow and its title. */
#define HEADER_GAP 8

/* The action bar's vertical padding. Its buttons are PUI_BTN_H_MD, so the bar comes to
   PUI_ACTION_H. */
#define ACTION_PAD_V 5

/* The scrollbar, and how far it is held off the object's edges. */
#define SCROLLBAR_W 4

/***********************************************************
********************function define**************************
***********************************************************/

void pui_screen_apply(lv_obj_t *scr)
{
    if (scr == NULL) {
        return;
    }

    /* The same strip the containers get, so a page shell and a card cannot disagree
       about what "transparent" means. */
    pui_priv_container(scr);

    lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scr, PET_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* The page is the column a header, a content well and an action bar go in, in that
       order. Nothing sets a height: each of the three owns its own. */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

lv_obj_t *pui_screen_header(lv_obj_t *page, const char *title, BOOL_T back)
{
    lv_obj_t *header;

    if (page == NULL) {
        return NULL;
    }

    header = lv_obj_create(page);
    pui_priv_container(header);

    lv_obj_set_size(header, LV_PCT(100), PUI_HEADER_H);
    /* No fill, and no rule under it. The header is a row of chrome laid on the page's own
       background rather than a bar across it: it used to be a white strip with a hairline,
       and a white strip on a cream page is a second surface the page never asked for —
       it made the arrow and the title look like a separate screen sitting on top of the
       page rather than the page's own first line. The cards are the only white things in
       the app, and the title row is what says so.
       The height stays: a page's chrome is a fixed height so the content below it does not
       jump between pages, and that is as true of a transparent row as of a white one. */
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    /* Two pixels wider than the page margin, so the arrow's glyph lines up with the
       content below it rather than with the arrow's own circular fill. */
    lv_obj_set_style_pad_hor(header, PET_PAD_PAGE + 2, 0);
    lv_obj_set_style_pad_column(header, HEADER_GAP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (back) {
        /* Built first so it lands at child 0 without a reorder afterwards. Its handler is
           the caller's to attach — the library has no idea what going back means. */
        pui_icon_button_create(header, FONT_AWESOME_MICARROW_LEFT, PUI_TONE_INDIGO, PUI_FILL_GHOST);
    }

    /* The page's own name, in the title face and at full strength: it is the one piece of
       chrome a page gets to be loud with, and the cards below are what it names. */
    lv_obj_set_style_text_letter_space(pui_label_create(header, title, PET_FONT_CJK_TITLE, PET_COLOR_TEXT), 0, 0);

    return header;
}

lv_obj_t *pui_screen_header_back(lv_obj_t *header)
{
    lv_obj_t *first;

    if (header == NULL) {
        return NULL;
    }

    first = lv_obj_get_child(header, 0);
    if (first == NULL || !lv_obj_check_type(first, &pui_icon_button_class)) {
        return NULL;
    }

    return first;
}

void pui_screen_header_push(lv_obj_t *header)
{
    if (header == NULL) {
        return;
    }

    pui_spacer(header);
}

lv_obj_t *pui_screen_content(lv_obj_t *page)
{
    lv_obj_t *body;

    if (page == NULL) {
        return NULL;
    }

    body = lv_obj_create(page);
    pui_priv_container(body);

    /* Whatever vertical space the header and the action bar did not take. A page only has
       to fill it; nothing here needs to know how tall it came out. */
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_style_pad_row(body, PET_GAP_CARD, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    return body;
}

lv_obj_t *pui_screen_actions(lv_obj_t *page, lv_coord_t gap)
{
    lv_obj_t *bar;

    if (page == NULL) {
        return NULL;
    }

    bar = lv_obj_create(page);
    pui_priv_container(bar);

    lv_obj_set_size(bar, LV_PCT(100), PUI_ACTION_H);
    lv_obj_set_style_bg_color(bar, PET_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, PET_PAD_PAGE, 0);
    lv_obj_set_style_pad_ver(bar, ACTION_PAD_V, 0);
    lv_obj_set_style_pad_column(bar, gap, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    return bar;
}

lv_obj_t *pui_screen_scrim(lv_obj_t *page)
{
    lv_obj_t *scrim;

    if (page == NULL) {
        return NULL;
    }

    scrim = lv_obj_create(page);

    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, PET_COLOR_SCRIM, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);

    /* A sheet is inset from the panel rather than spanning it: a card flush to both edges
       reads as a page rather than as something raised over one. The padding is what does
       the insetting, so a sheet's card can be LV_PCT(100) and still not touch the sides —
       which is why no caller writes a sheet width. */
    lv_obj_set_style_pad_all(scrim, PET_PAD_PAGE, 0);
    lv_obj_set_flex_flow(scrim, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scrim, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* The scrim is the one thing here that must keep CLICKABLE: it is what swallows the
       tap that dismisses the sheet, and the tap has to land on it rather than on the page
       underneath. */
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    /* The page lays out in a flex column, and without this the scrim becomes the next item
       in it — placed below the action bar and scrolled off the bottom, instead of covering
       the page. IGNORE_LAYOUT takes it out of the flow and leaves it at its own
       coordinates, which are the page's origin. */
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_move_foreground(scrim);

    return scrim;
}

/**
 * @brief Make @p obj a scroll region, and give it the bar that says so.
 *
 * Both flags, because a scroll region needs both and forgetting the second is invisible
 * until someone drags it.
 *
 * `pui_priv_container()` takes CLICKABLE off every container in this library, and that is
 * right for a decoration: a container that took presses would shield whatever is behind
 * it. A scroll region is not a decoration — it is the ground of the page, and there is
 * nothing behind it — and LVGL resolves a drag by hit-testing for a *clickable* object
 * first, then walking up from it looking for a scrollable one. A region that is
 * scrollable but not clickable is therefore unreachable: a press that lands on the region
 * itself, or on any of the text and padding inside it, matches no object at all, no press
 * is ever delivered, and the page does not move.
 *
 * The symptom is a list that scrolls only when the finger happens to land on one of its
 * controls — the settings page scrolled under the sliders and nowhere else, because the
 * sliders were the only clickable things on it.
 */
void pui_screen_scrollable(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    pui_screen_scrollbar(obj);
}

/**
 * @brief Give a scrollable container the app's slim scrollbar.
 *
 * Styling only — it does not make anything scroll. Call pui_screen_scrollable() for that.
 */
void pui_screen_scrollbar(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_width(obj, SCROLLBAR_W, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, PET_COLOR_TEXT_HINT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);

    /* Padding around the bar keeps it off the card edges and off the screen edge. */
    lv_obj_set_style_pad_right(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(obj, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(obj, 8, LV_PART_SCROLLBAR);
}
