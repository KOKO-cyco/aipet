/**
 * @file pet_widgets.c
 * @brief The widgets the reference design needs that the component library does not
 *        already provide. See pet_widgets.h.
 *
 * These are the rows a settings page is made of — a check, a switch, a slider, a select,
 * a segmented control — plus the roller the note page's reminder sheet uses. They are not
 * `pui` classes because they are not composed of components: each one wraps a real LVGL
 * widget (`lv_switch`, `lv_slider`, `lv_roller`) and the widget's own parts are what the
 * styling below is aimed at. What they take from the library is the vocabulary — the
 * text roles, and `pui_passthrough()` on every container that must not swallow a tap.
 *
 * The API in pet_widgets.h is shared verbatim with the portrait build and does not change
 * here.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_widgets.h"

#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* Row heights. Every one of these went up by four: at the old values a column of rows
   had no air between the text and the hairline under it, which is what made nine rows
   read as one paragraph. The slider row is the tallest because the track needs a margin
   above and below it that the switch does not. */
#define PET_ROW_H_CHECK  34
#define PET_ROW_H_SWITCH 34
#define PET_ROW_H_SLIDER 34
#define PET_ROW_H_SELECT 34

/* Width of the label column in a slider row, so the tracks of every row line up down
   the page instead of stepping with the label lengths. Set by the longest name the
   settings page has — 拍照&AI识别, 103 px at 18 px — and the row clips rather than
   widening, so a longer name has to move this number too. */
#define PET_SLIDER_LABEL_W 86
#define PET_SLIDER_VALUE_W 24

#define PET_TICK      20
#define PET_SEGMENT_H 30

/* How long the switch knob takes to cross when it is thrown. The default theme leaves
   this at 200 ms; a hair quicker reads as the tap having thrown the knob, and the small
   squash the row gives back on the same event is what makes it land rather than glide. */
#define PET_SWITCH_MS 170

/* Half-width of the shadow the slider knob grows while it is being dragged. A slider has
   no other way to say "I have hold of the knob" — the knob simply follows the finger —
   so a soft shadow under it is the whole of the feedback. */
#define PET_KNOB_SHADOW_W 6

/***********************************************************
********************function define**************************
***********************************************************/

lv_obj_t *pet_widget_row_control(lv_obj_t *row)
{
    if (row == NULL) {
        return NULL;
    }

    return (lv_obj_t *)lv_obj_get_user_data(row);
}

/**
 * @brief A bare row: full width, transparent, laid out left to right, ruled underneath.
 *
 * The shared skeleton of every widget in this file. Passthrough, because the
 * children are what take the press; a constructor that wants the whole row to be
 * the target adds CLICKABLE back afterwards.
 *
 * The hairline is what turns a column of rows into a list. It is on by default and the
 * caller turns it off on whichever row is last — see pet_page_settings.c, which is the
 * only place that knows where a group ends. Putting it here rather than at the call site
 * means a new row type cannot be added without one, which is the failure that made the
 * settings page read as nine lines of undifferentiated text.
 */
static lv_obj_t *__row(lv_obj_t *parent, lv_coord_t height, lv_coord_t gap)
{
    lv_obj_t *row = lv_obj_create(parent);

    pui_passthrough(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, PET_COLOR_LINE, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);

    return row;
}

/* ---- Check row ------------------------------------------------------------ */

/**
 * @brief Repaint a tick box for @p done.
 *
 * Done is carried in the box's own user data rather than a file-static, so several
 * rows on one page can each hold their own state.
 */
static void __tick_paint(lv_obj_t *tick, BOOL_T done)
{
    lv_obj_t *glyph;

    lv_obj_set_user_data(tick, (void *)(uintptr_t)(done ? 1u : 0u));

    if (done) {
        lv_obj_set_style_bg_color(tick, PET_COLOR_ACCENT, 0);
        lv_obj_set_style_border_color(tick, PET_COLOR_ACCENT, 0);
    } else {
        lv_obj_set_style_bg_color(tick, PET_COLOR_CARD, 0);
        lv_obj_set_style_border_color(tick, PET_COLOR_LINE_STRONG, 0);
    }

    glyph = lv_obj_get_child(tick, 0);
    if (glyph != NULL) {
        lv_obj_set_style_text_color(glyph, done ? PET_COLOR_ON_ACCENT : PET_COLOR_CARD, 0);
    }
}

lv_obj_t *pet_widget_check_row(lv_obj_t *parent, const char *text, BOOL_T done)
{
    lv_obj_t *row  = __row(parent, PET_ROW_H_CHECK, 8);
    lv_obj_t *tick = lv_obj_create(row);
    lv_obj_t *label;
    lv_obj_t *glyph;

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* The row is the target, so it takes the library's flat press (dim, no squash — it
       holds the tick and the label). Without it a check row was the one control on the
       page that did not answer a finger. See pet_widget_select_row for the same on a
       select. */
    pui_pressable_flat(row);

    pui_passthrough(tick);
    lv_obj_set_size(tick, PET_TICK, PET_TICK);
    lv_obj_set_style_radius(tick, 6, 0);
    lv_obj_set_style_border_width(tick, 2, 0);

    /* Always present, just painted the same colour as the fill when unchecked. A
       box that adds and removes its glyph would relayout the row on every tap. */
    glyph = pui_text_create(tick, FONT_AWESOME_CHECK, PUI_TEXT_GLYPH);
    lv_obj_set_style_text_color(glyph, PET_COLOR_CARD, 0);
    lv_obj_center(glyph);

    /* pui_text_create() is what keeps the words off the press — an lv_label is
       clickable by default in this LVGL build and the press goes to the deepest
       clickable object under the finger, so without it tapping the words does nothing
       and only the margins around them tick the box. */
    label = pui_text_create(row, text, PUI_TEXT_BODY);
    lv_obj_set_flex_grow(label, 1);

    lv_obj_set_user_data(row, tick);
    __tick_paint(tick, done);

    if (done) {
        lv_obj_set_style_text_color(label, PET_COLOR_TEXT_SUB, 0);
    }

    return row;
}

BOOL_T pet_widget_check_row_get(lv_obj_t *row)
{
    lv_obj_t *tick = pet_widget_row_control(row);

    if (tick == NULL) {
        return FALSE;
    }

    return lv_obj_get_user_data(tick) != NULL ? TRUE : FALSE;
}

void pet_widget_check_row_set(lv_obj_t *row, BOOL_T done)
{
    lv_obj_t *tick = pet_widget_row_control(row);
    lv_obj_t *label;

    if (tick == NULL) {
        return;
    }

    __tick_paint(tick, done);

    label = lv_obj_get_child(row, 1);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, done ? PET_COLOR_TEXT_SUB : PET_COLOR_TEXT, 0);
    }
}

BOOL_T pet_widget_check_row_toggle(lv_obj_t *row)
{
    BOOL_T done = pet_widget_check_row_get(row) ? FALSE : TRUE;

    pet_widget_check_row_set(row, done);

    return done;
}

/* ---- Switch row ----------------------------------------------------------- */

/**
 * @brief Give the switch something of its own on the way through a toggle.
 *
 * The knob's slide is LVGL's, driven by the anim duration set below. This adds the small
 * squash on top of it, on the same event, so the switch lands rather than glides. It is
 * additive: a page's own LV_EVENT_VALUE_CHANGED handler still runs — LVGL calls every
 * handler on the object — so no page has to know this exists.
 */
static void __switch_bounce_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        pui_anim_pop(lv_event_get_target(e));
    }
}

lv_obj_t *pet_widget_switch_row(lv_obj_t *parent, const char *label, BOOL_T on)
{
    lv_obj_t *row = __row(parent, PET_ROW_H_SWITCH, 8);
    lv_obj_t *sw;

    /* The label is built first and grown, so the switch lands on the right edge — the
       side every other row puts its control on, and the side the reference shows. The
       row's control is reached through its user data, not through a child index, so
       the order here is free to follow the layout. */
    lv_obj_set_flex_grow(pui_label_create(row, label, PET_FONT_CJK_SM, PET_COLOR_TEXT), 1);

    sw = lv_switch_create(row);

    lv_obj_set_size(sw, 36, 20);
    /* Faster than the theme's 200 ms; see PET_SWITCH_MS. */
    lv_obj_set_style_anim_duration(sw, PET_SWITCH_MS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, PET_COLOR_NEUTRAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    /* INDICATOR is the part that fills in when checked. */
    lv_obj_set_style_bg_color(sw, PET_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, PET_COLOR_CARD, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);

    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(sw, __switch_bounce_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_set_user_data(row, sw);

    return row;
}

/* ---- Slider row ----------------------------------------------------------- */

static lv_style_t sg_knob_press;
static BOOL_T     sg_knob_press_ready;

/**
 * @brief Give the slider knob a shadow while it is held.
 *
 * The knob has no pressed state of its own to lean on beyond the base LVGL one, and a
 * slider that follows the finger with no other change is hard to tell from one that is
 * merely being tapped past. A soft shadow under the knob while LV_STATE_PRESSED is set is
 * the whole of the feedback: the knob reads as picked up. Built once, on the first slider.
 */
static void __knob_press_style(lv_obj_t *slider)
{
    if (!sg_knob_press_ready) {
        sg_knob_press_ready = TRUE;
        lv_style_init(&sg_knob_press);
        lv_style_set_shadow_width(&sg_knob_press, PET_KNOB_SHADOW_W);
        lv_style_set_shadow_color(&sg_knob_press, PET_COLOR_SHADOW);
        lv_style_set_shadow_opa(&sg_knob_press, LV_OPA_40);
    }

    lv_obj_add_style(slider, &sg_knob_press, LV_PART_KNOB | LV_STATE_PRESSED);
}

lv_obj_t *pet_widget_slider_row(lv_obj_t *parent, const char *label, int value, int min, int max)
{
    lv_obj_t *row    = __row(parent, PET_ROW_H_SLIDER, 8);
    lv_obj_t *name   = pui_label_create(row, label, PET_FONT_CJK_SM, PET_COLOR_TEXT);
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_t *readout;

    lv_obj_set_width(name, PET_SLIDER_LABEL_W);
    /* One line only: a two-line label would make this row taller than the others
       and break the rhythm of the column. */
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);

    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 4);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(slider, PET_COLOR_NEUTRAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, PET_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, PET_COLOR_CARD, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, PET_COLOR_ACCENT, LV_PART_KNOB);

    __knob_press_style(slider);

    /* LVGL 9.1 draws a bar or slider wrong — and can walk off the end of the draw
       buffer — when the main part has top padding but no right padding. Forcing a
       right padding of 1 sidesteps it. */
    lv_obj_set_style_pad_right(slider, 1, LV_PART_MAIN);

    readout = pui_label_create(row, "", PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    lv_obj_set_width(readout, PET_SLIDER_VALUE_W);
    lv_obj_set_style_text_align(readout, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_set_user_data(row, slider);

    pet_widget_slider_row_refresh(row);

    return row;
}

int pet_widget_slider_row_value(lv_obj_t *row)
{
    lv_obj_t *slider = pet_widget_row_control(row);

    if (slider == NULL) {
        return 0;
    }

    return (int)lv_slider_get_value(slider);
}

void pet_widget_slider_row_refresh(lv_obj_t *row)
{
    lv_obj_t *slider = pet_widget_row_control(row);
    /* Child 2 by construction: label, slider, readout. See pet_widget_slider_row. */
    lv_obj_t *readout;
    char      text[16];

    if (slider == NULL) {
        return;
    }

    readout = lv_obj_get_child(row, 2);
    if (readout == NULL) {
        return;
    }

    lv_snprintf(text, sizeof(text), "%d", (int)lv_slider_get_value(slider));
    lv_label_set_text(readout, text);
}

/* ---- Select row ----------------------------------------------------------- */

lv_obj_t *pet_widget_select_row(lv_obj_t *parent, const char *label, const char *value)
{
    lv_obj_t *row = __row(parent, PET_ROW_H_SELECT, 6);
    lv_obj_t *gap;
    lv_obj_t *chevron;

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    pui_pressable_flat(row);

    /* pui_text_create() and pui_passthrough() are what keep the text off the press —
       see the note in pet_widget_check_row. */
    pui_label_create(row, label, PET_FONT_CJK_SM, PET_COLOR_TEXT);

    gap = lv_obj_create(row);
    pui_passthrough(gap);
    lv_obj_set_size(gap, 0, 0);
    lv_obj_set_flex_grow(gap, 1);

    pui_label_create(row, value, PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);

    chevron = pui_text_create(row, FONT_AWESOME_ARROW_DOWN, PUI_TEXT_GLYPH);
    lv_obj_set_style_text_color(chevron, PET_COLOR_TEXT_HINT, 0);

    /* No separate control to read back: the value is the second label. */
    lv_obj_set_user_data(row, chevron);

    return row;
}

void pet_widget_select_row_set(lv_obj_t *row, const char *value)
{
    /* Child 2 by construction: label, spacer, value, chevron. See the constructor. */
    lv_obj_t *label = row != NULL ? lv_obj_get_child(row, 2) : NULL;

    if (label == NULL) {
        return;
    }

    lv_label_set_text(label, value != NULL ? value : "");
}

/* ---- Segmented control ---------------------------------------------------- */

/**
 * @brief Repaint one segment for the selected/unselected look.
 */
static void __segment_paint(lv_obj_t *seg, BOOL_T on)
{
    lv_obj_t *label = lv_obj_get_child(seg, 0);

    lv_obj_set_style_bg_color(seg, on ? PET_COLOR_ACCENT : PET_COLOR_NEUTRAL, 0);

    if (label != NULL) {
        lv_obj_set_style_text_color(label, on ? PET_COLOR_ON_ACCENT : PET_COLOR_TEXT_SUB, 0);
    }
}

lv_obj_t *pet_widget_segment(lv_obj_t *parent, const char *const *labels, uint32_t count, uint32_t selected)
{
    lv_obj_t *row = __row(parent, PET_SEGMENT_H, 4);

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *seg = lv_obj_create(row);
        lv_obj_t *label;

        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(seg, PET_SEGMENT_H);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, PET_RADIUS_PILL, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_set_style_pad_hor(seg, 2, 0);

        /* The segment is the target, so it answers the press the same way a check or a
           select row does. Flat: it is a fill with a label on it, not a leaf. */
        pui_pressable_flat(seg);

        /* The index travels on the segment itself, so the page can put one handler
           on the container and still know which choice was tapped. */
        lv_obj_set_user_data(seg, (void *)(uintptr_t)i);

        /* Four Chinese labels across the panel's 284 px is tight; clipping beats the
           label forcing the segment wider than its share. The segment is the target and
           carries the index, so the text on top of it must not swallow the press —
           pui_text_create() is what guarantees that. */
        label = pui_text_create(seg, labels[i], PUI_TEXT_SUB);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_center(label);

        __segment_paint(seg, i == selected);
    }

    return row;
}

void pet_widget_segment_set(lv_obj_t *segment, uint32_t selected)
{
    uint32_t count;

    if (segment == NULL) {
        return;
    }

    count = lv_obj_get_child_count(segment);

    for (uint32_t i = 0; i < count; i++) {
        __segment_paint(lv_obj_get_child(segment, i), i == selected);
    }
}

/* ---- Roller --------------------------------------------------------------- */

lv_obj_t *pet_widget_roller(lv_obj_t *parent, const char *options, uint32_t selected, lv_coord_t width)
{
    lv_obj_t *roller = lv_roller_create(parent);

    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_width(roller, width);
    lv_obj_set_style_text_font(roller, PET_FONT_CJK, 0);
    lv_obj_set_style_text_font(roller, PET_FONT_CJK, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, PET_COLOR_TEXT_SUB, 0);
    lv_obj_set_style_text_color(roller, PET_COLOR_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, PET_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(roller, PET_COLOR_ACCENT_SOFT, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_border_width(roller, 1, 0);
    lv_obj_set_style_border_color(roller, PET_COLOR_LINE_STRONG, 0);
    lv_obj_set_style_radius(roller, PET_RADIUS_BTN, 0);

    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);

    return roller;
}
