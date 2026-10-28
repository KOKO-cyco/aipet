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
 * @brief Delete one item and close the gap. Out-of-range indices are ignored.
 */
void pet_demo_todo_delete(uint32_t index);

/**
 * @brief Remove every item whose done flag is set.
 */
void pet_demo_todo_clear_done(void);

/* ---- Memos / notes by id (the AI's write path) ----------------------------- */

/**
 * @brief Append a memo at the head of the list, assigning it a fresh id.
 *
 * @return The new id, or 0 when the list is full or @p text is empty.
 */
uint32_t pet_demo_memo_add(const char *text, const char *when);

/**
 * @brief Change one memo, addressed by id.
 *
 * An empty @p text or @p when leaves that field alone; @p done is -1 for "no
 * change", otherwise 0 or 1. A memo that does not exist (the AI's snapshot went
 * stale) reads as failure.
 *
 * @return TRUE when something actually changed.
 */
BOOL_T pet_demo_memo_update(uint32_t id, const char *text, const char *when, int done);

/**
 * @brief Delete the memo with this id.
 *
 * @return FALSE when no memo has that id.
 */
BOOL_T pet_demo_memo_delete_id(uint32_t id);

/**
 * @brief Append a note at the head of the list, assigning it a fresh id.
 *
 * @p body is newline-separated text; each non-empty line becomes one row of the
 * note (up to PET_NOTE_LINES_MAX lines).
 *
 * @return The new id, or 0 when the list is full or @p title is empty.
 */
uint32_t pet_demo_note_add(const char *title, const char *body);

/**
 * @brief Change one note, addressed by id.
 *
 * An empty @p title or @p body leaves that field alone.
 *
 * @return TRUE when something actually changed.
 */
BOOL_T pet_demo_note_update(uint32_t id, const char *title, const char *body);

/**
 * @brief Delete the note with this id.
 *
 * @return FALSE when no note has that id.
 */
BOOL_T pet_demo_note_delete_id(uint32_t id);

/* ---- Notes ---------------------------------------------------------------- */

/**
 * @brief How many notes the list page has to show.
 */
uint32_t pet_demo_note_count(void);

/**
 * @brief One note, newest first.
 *
 * @return NULL when @p index is past the end.
 */
const PET_NOTE_T *pet_demo_note_at(uint32_t index);

/**
 * @brief Delete one note and close the gap. Out-of-range indices are ignored.
 */
void pet_demo_note_delete(uint32_t index);

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
