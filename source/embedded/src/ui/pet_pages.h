/**
 * @file pet_pages.h
 * @brief The per-page builders, shared between pet_ui.c's dispatcher and the page
 *        files themselves.
 *
 * Internal to the app: not under include/, because nothing outside src/ builds a
 * page. A quoted include resolves against the including file's directory, so the
 * page files reach this as `"pet_pages.h"`.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_PAGES_H__
#define __PET_PAGES_H__

#include "lvgl.h"

#include "pet_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Build one page into the host.
 *
 * @param[in] panel  The panel column, already on screen and already faded in by the
 *                   caller. The page owns everything inside it and is responsible for
 *                   setting the pet's line with pet_say().
 */
void pet_page_pet_build(lv_obj_t *panel);
void pet_page_home_build(lv_obj_t *panel);
void pet_page_camera_build(lv_obj_t *panel);
void pet_page_gallery_build(lv_obj_t *panel);
void pet_page_memos_build(lv_obj_t *panel);
void pet_page_notes_build(lv_obj_t *panel);
void pet_page_settings_build(lv_obj_t *panel);

/**
 * @brief Give a page the standard header for its title, with a back arrow unless it
 *        is the root.
 *
 * Every page but the home one wants the same thing, and the back arrow has to be
 * moved to index 0 so it precedes the title — easy to forget.
 *
 * @param[in] back  TRUE to prepend a back arrow that calls pet_ui_back().
 * @return The header container; append action buttons to it.
 */
lv_obj_t *pet_page_header(lv_obj_t *panel, const char *title, BOOL_T back);

/**
 * @brief Push the header's action buttons to the right edge.
 *
 * Call it after pet_page_header() and before adding the buttons; without it they
 * queue up against the title.
 */
void pet_page_header_push(lv_obj_t *header);

/**
 * @brief A transparent container that fills its parent and lines its children up in a
 *        row (or a column).
 *
 * Already passthrough, already growing, already unpadded — the three things a plain
 * `lv_obj_create` gets wrong for this job.
 */
lv_obj_t *pet_page_row(lv_obj_t *parent, lv_coord_t gap);
lv_obj_t *pet_page_col(lv_obj_t *parent, lv_coord_t gap);

/**
 * @brief The scrolling content area between the header and the action bar.
 *
 * Sized to whatever vertical space is left, so a page only has to fill it.
 */
lv_obj_t *pet_page_content(lv_obj_t *panel);

/**
 * @brief The action bar along the foot of the page.
 *
 * @param[in] gap  Spacing between the buttons.
 */
lv_obj_t *pet_page_action_bar(lv_obj_t *panel, lv_coord_t gap);

/**
 * @brief Raise a full-height scrim over the page, for a modal or a sheet.
 *
 * The returned object is the scrim: build the card into it and align it, then
 * delete the scrim to dismiss. It swallows the tap that dismisses it, so a caller
 * that wants tap-to-close only has to attach a CLICKED handler.
 *
 * @return NULL when the page is not on screen any more.
 */
lv_obj_t *pet_page_sheet(void);

/**
 * @brief Dismiss the sheet raised by pet_page_sheet().
 *
 * Safe to call from inside the sheet's own handlers, and safe to call twice.
 */
void pet_page_sheet_close(void);

/**
 * @brief A destructive-action confirmation, raised over the current page.
 *
 * The sheet is the one shared delete pattern for the memo and note lists: a title,
 * a line of body copy, and 取消 / <action>. There is no text input and no keyboard;
 * the action is the only thing that can be confirmed.
 *
 * @param[in] title       Sheet heading, e.g. 删除备忘录.
 * @param[in] body        Body copy, may contain newlines.
 * @param[in] action      Label of the destructive button, e.g. 删除 / 清空.
 * @param[in] on_confirm  Called after the sheet closes. May rebuild the page.
 * @param[in] user        Passed through to @p on_confirm.
 */
typedef void (*PET_CONFIRM_CB)(void *user);
void pet_page_confirm(const char *title, const char *body, const char *action, PET_CONFIRM_CB on_confirm, void *user);

/**
 * @brief Whether a sheet is currently up.
 *
 * The camera page needs this: its live preview is composited straight into the
 * display framebuffer after LVGL has drawn, so it has to stand down while a sheet
 * is on screen or it would paint over it.
 */
BOOL_T pet_page_sheet_open(void);

#ifdef __cplusplus
}
#endif

#endif /* __PET_PAGES_H__ */
