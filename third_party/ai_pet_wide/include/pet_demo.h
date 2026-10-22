/**
 * @file pet_demo.h
 * @brief The in-memory model behind the desktop-pet UI.
 *
 * This round ships the interface only: there is no storage, no camera and no
 * network. Every page reads its content from here, and this file is deliberately
 * free of LVGL so it can be exercised on the host (see tools/pet_test).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_DEMO_H__
#define __PET_DEMO_H__

#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Seed the model with the demo content. Call once at boot.
 *
 * Idempotent: calling it again resets everything to the seeded state.
 */
void pet_demo_init(void);

/* ---- To-do ---------------------------------------------------------------- */

/**
 * @brief The to-do list, for the page to render. Never NULL.
 */
PET_TODO_LIST_T *pet_demo_todo(void);

/**
 * @brief How many items are still open. Drives the badge on the home page.
 */
uint32_t pet_demo_todo_pending(void);

/**
 * @brief Flip the done flag of one item. Out-of-range indices are ignored.
 */
void pet_demo_todo_toggle(uint32_t index);

/**
 * @brief Append an item.
 *
 * @return FALSE when the list is already at PET_TODO_MAX, in which case nothing is
 *         added — the caller decides how to say so.
 */
BOOL_T pet_demo_todo_add(const char *text);

/**
 * @brief Label of the to-do page's left rail, e.g. 灵感记录.
 *
 * @return NULL when @p index is past the end, so a caller can loop until NULL.
 */
const char *pet_demo_nav_name(uint32_t index);

/**
 * @brief When a note was last touched, as it should be shown beside the name.
 *
 * A display string rather than a timestamp: the model has no clock, so this is fixed
 * demo copy of the form 10:20 / 昨天 / 周一.
 *
 * @return NULL when @p index is past the end, so a caller can stop at the same place
 *         pet_demo_nav_name() does.
 */
const char *pet_demo_nav_time(uint32_t index);

/* ---- Gallery -------------------------------------------------------------- */

/**
 * @brief The gallery grid's photos. Never NULL.
 */
const PET_SHOT_T *pet_demo_shots(void);

/**
 * @brief How many photos the grid holds.
 */
uint32_t pet_demo_shot_count(void);

/**
 * @brief Flip one photo's selection. Out-of-range indices are ignored.
 */
void pet_demo_shot_toggle(uint32_t index);

/**
 * @brief How many photos are currently selected. Drives the delete button.
 */
uint32_t pet_demo_shot_selected(void);

/**
 * @brief Deselect everything. Called after the grid's action buttons run.
 */
void pet_demo_shot_clear_selection(void);

/* ---- Camera --------------------------------------------------------------- */

/**
 * @brief The capture mode the camera page is showing.
 */
PET_CAMERA_MODE_E pet_demo_mode(void);

/**
 * @brief Select a mode. Out-of-range values are ignored.
 */
void pet_demo_mode_set(PET_CAMERA_MODE_E mode);

/**
 * @brief Advance to the next mode, wrapping at the end.
 */
void pet_demo_mode_next(void);

/**
 * @brief Chinese name of a mode. Out-of-range values read as PET_MODE_COMPOSE.
 */
const char *pet_demo_mode_name(PET_CAMERA_MODE_E mode);

/* ---- Note page ------------------------------------------------------------ */

/**
 * @brief One line of the note body.
 *
 * @return NULL when @p index is past the end, so a caller can loop until NULL.
 */
const char *pet_demo_note_line(uint32_t index);

/* ---- Reminder picker ------------------------------------------------------ */

/**
 * @brief The day / hour / minute the reminder sheet is currently showing.
 */
uint32_t pet_demo_remind_day(void);
uint32_t pet_demo_remind_hour(void);
uint32_t pet_demo_remind_min(void);

/**
 * @brief Step the reminder, wrapping rather than clamping.
 *
 * Wrapping is the point: the sheet is a pair of rollers, and a roller that stops
 * dead at 23:59 feels broken. Day wraps 1..31, hour 0..23, minute 0..59.
 */
void pet_demo_remind_step(int day_delta, int hour_delta, int min_delta);

/* ---- Settings ------------------------------------------------------------- */

/**
 * @brief Read a setting's value. A switch reads 0 or 1; a slider reads 0..100.
 */
int pet_demo_setting_get(PET_SETTING_E id);

/**
 * @brief Write a setting, clamping to the row's own range. Out-of-range ids are
 *        ignored.
 */
void pet_demo_setting_set(PET_SETTING_E id, int value);

/**
 * @brief Flip a switch row. No effect on a slider row.
 */
void pet_demo_setting_toggle(PET_SETTING_E id);

/**
 * @brief Chinese name of a settings row.
 */
const char *pet_demo_setting_name(PET_SETTING_E id);

/**
 * @brief TRUE when the row is an on/off switch rather than a slider.
 */
BOOL_T pet_demo_setting_is_toggle(PET_SETTING_E id);

/**
 * @brief The row's own range, for the slider to map onto.
 */
int pet_demo_setting_min(PET_SETTING_E id);
int pet_demo_setting_max(PET_SETTING_E id);

/* ---- Composite position --------------------------------------------------- */

/**
 * @brief Where the pet stands in a composite photo.
 */
PET_POS_E pet_demo_pos(void);

/**
 * @brief Advance the position, wrapping at the end.
 */
void pet_demo_pos_next(void);

/**
 * @brief Chinese name of a position.
 */
const char *pet_demo_pos_name(PET_POS_E pos);

#ifdef __cplusplus
}
#endif

#endif /* __PET_DEMO_H__ */
