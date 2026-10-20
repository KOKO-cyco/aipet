/**
 * @file pet_widgets.h
 * @brief The widgets the reference design needs that the theme layer does not
 *        already provide.
 *
 * ai_memo had cards, buttons, headers and a stepper, and nothing else. This app
 * needs check rows, switches, sliders, a cycling select and a segmented control, so
 * they live here rather than being rebuilt inline on three different pages.
 *
 * The constructors deliberately do not take callbacks. They build the shape and
 * hand back the row; the page attaches its own handler and reads the control out of
 * the row with pet_widget_row_control(). That keeps this file free of page state,
 * and keeps every page's event handling in the page.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_WIDGETS_H__
#define __PET_WIDGETS_H__

#include "lvgl.h"

#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief The child a widget row's state lives in, stashed by its constructor.
 *
 * For a switch row that is the `lv_switch`, for a slider row the `lv_slider`, for a
 * check row the tick box, for a select row the chevron. It is the object a page's
 * handler reaches for when it needs to read or restyle the row.
 *
 * @return NULL when @p row is NULL or was not built by a pet_widget_*() constructor.
 */
lv_obj_t *pet_widget_row_control(lv_obj_t *row);

/**
 * @brief A to-do row: a tick box and a label, with the whole row as the target.
 *
 * Not an `lv_checkbox`: that widget's own layout fights a flex row, and its tick is
 * drawn in the label's font rather than from the icon face.
 *
 * @param[in] done  Whether the tick starts on.
 */
lv_obj_t *pet_widget_check_row(lv_obj_t *parent, const char *text, BOOL_T done);

/**
 * @brief Restyle a check row after its state changed.
 *
 * Done rows dim their text and fill the tick; open rows show an empty outlined box.
 */
void pet_widget_check_row_set(lv_obj_t *row, BOOL_T done);

/**
 * @brief Read a check row's state.
 */
BOOL_T pet_widget_check_row_get(lv_obj_t *row);

/**
 * @brief Flip a check row and restyle it.
 *
 * @return The state after the flip.
 */
BOOL_T pet_widget_check_row_toggle(lv_obj_t *row);

/**
 * @brief A settings row with an on/off switch on the right.
 */
lv_obj_t *pet_widget_switch_row(lv_obj_t *parent, const char *label, BOOL_T on);

/**
 * @brief A settings row with a slider between the label and the value readout.
 *
 * @param[in] min/max  The row's own range. The slider maps onto it directly, so a
 *                     countdown in seconds and a percentage both work.
 */
lv_obj_t *pet_widget_slider_row(lv_obj_t *parent, const char *label, int value, int min, int max);

/**
 * @brief Read a slider row's current value.
 */
int pet_widget_slider_row_value(lv_obj_t *row);

/**
 * @brief Repaint a slider row's value readout. Call from the slider's handler.
 */
void pet_widget_slider_row_refresh(lv_obj_t *row);

/**
 * @brief A settings row that shows a choice: label, current value, chevron.
 *
 * It does not open anything. The page decides whether that means cycling in place
 * or raising a sheet, because the two pages that use it want different behaviour.
 */
lv_obj_t *pet_widget_select_row(lv_obj_t *parent, const char *label, const char *value);

/**
 * @brief Replace a select row's value text.
 */
void pet_widget_select_row_set(lv_obj_t *row, const char *value);

/**
 * @brief A row of mutually exclusive choices, evenly divided across the width.
 *
 * @param[in] labels    Array of @p count UTF-8 labels.
 * @param[in] selected  Index that starts highlighted; out-of-range selects none.
 *
 * Each segment carries its own index in its user data, so a single handler on the
 * container can read `(uint32_t)(uintptr_t)lv_event_get_user_data(e)`.
 */
lv_obj_t *pet_widget_segment(lv_obj_t *parent, const char *const *labels, uint32_t count, uint32_t selected);

/**
 * @brief Move the highlight. Out-of-range @p selected clears every highlight.
 */
void pet_widget_segment_set(lv_obj_t *segment, uint32_t selected);

/**
 * @brief A scroll wheel, for picking a number out of a range.
 *
 * @param[in] options  Newline-separated UTF-8 options, as `lv_roller` expects.
 */
lv_obj_t *pet_widget_roller(lv_obj_t *parent, const char *options, uint32_t selected, lv_coord_t width);

#ifdef __cplusplus
}
#endif

#endif /* __PET_WIDGETS_H__ */
