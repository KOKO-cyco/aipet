/**
 * @file pet_ui.h
 * @brief The desktop-pet user interface: the shell, the page stack, and the entry
 *        point.
 *
 * The screen is a status strip across the top, and under it the character's card on
 * the left and the function panel on the right. The card is built once and the panel
 * swaps its contents.
 *
 *     +-----------------------------------------------+
 *     | 10:30      (天气)        待办 3   (bat) 80%   |  status (480 x 24)
 *     +-----------------+-----------------------------+
 *     |                 |                             |
 *     |  pet card (196) |  page (250 x 286)           |
 *     |                 |                             |
 *     +-----------------+-----------------------------+
 *
 * 主页 is the root of the stack: it carries the three entry tiles that reach 拍照,
 * 备忘录 and 设置. 相册 hangs off the camera page rather than off the home page, which
 * is where the photos come from.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_UI_H__
#define __PET_UI_H__

#include "lvgl.h"

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief The pages that can fill the panel.
 *
 * 主页 is the root; the others are reached from it or from each other. The character
 * is not in here — it has a card of its own that is always on screen, so it is not
 * something the user can navigate to.
 */
typedef enum {
    PET_PAGE_HOME = 0, /* 主页 — three entry tiles, the weather and two summary cards */
    PET_PAGE_CAMERA,   /* 拍照 */
    PET_PAGE_GALLERY,  /* 相册 — reached from the camera page */
    PET_PAGE_NOTES,    /* 全部笔记 — the list of notes, reached from the home card */
    PET_PAGE_NOTE,     /* 备忘录 — the editor for one note */
    PET_PAGE_SETTINGS, /* 设置 */
    PET_PAGE_MAX,
} PET_PAGE_E;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Build the shell and show the home page.
 *
 * @param[in] screen  The screen to build into — normally the page's own
 *                    `lv_obj_create(NULL)`, which the router owns. NULL falls back to
 *                    lv_screen_active().
 *
 * Must be called with the LVGL display lock held when another thread owns the
 * display; the device entry point takes the lock around it.
 */
void pet_ui_init(lv_obj_t *screen);

/**
 * @brief Tear the shell down, freeing every page and the character's card.
 *
 * The screen itself is the caller's to delete. Call this first: it resets the
 * statics that pet_say() and the page stack use, so a line set from another thread
 * between the delete and the next init has nothing to write into.
 */
void pet_ui_deinit(void);

/**
 * @brief Push another page onto the panel.
 *
 * Silently ignored once the stack is full, or when the page asked for is already on
 * top, rather than dropping the current page.
 */
void pet_ui_go(PET_PAGE_E page);

/**
 * @brief Pop back to the previous page. A no-op on the root page.
 */
void pet_ui_back(void);

/**
 * @brief Rebuild the page on screen.
 *
 * For a page whose widgets no longer agree with the model — the gallery after its
 * selection was cleared, say. Navigating to the page it is already showing would push
 * a second copy of it onto the stack instead.
 */
void pet_ui_refresh(void);

/**
 * @brief The page container currently on screen, for a sheet to raise itself over.
 *
 * @return NULL before pet_ui_init() has run.
 */
lv_obj_t *pet_ui_panel(void);

/**
 * @brief Re-read the to-do count into the status strip's badge.
 *
 * Call after anything that can change how many to-dos are open, otherwise the badge
 * keeps advertising the count from boot. The strip belongs to the shell rather than to
 * any page — it is the one thing that must not move when the panel changes — so this
 * lives here and not with the character.
 */
void pet_ui_badge_refresh(void);

/**
 * @brief 每个面板页建好之后回调一次（宿主页用来放"进页面动作"）。
 *
 * 桌宠页把它映射成宠物动作：进拍照页演 `fn_photo`、进备忘录演 `fn_note`。
 * 回调在 LVGL 线程里执行（页面构建期间），只应写请求位。
 *
 * @param[in] cb NULL = 摘掉回调。
 */
typedef void (*PET_UI_PAGE_HOOK)(PET_PAGE_E page);
void pet_ui_set_page_hook(PET_UI_PAGE_HOOK cb);

/** @brief 当前是否有 sheet（图片选择器等模态卡）开着。 */
BOOL_T pet_ui_sheet_active(void);

/** @brief 关掉当前 sheet（没有就什么都不做）。 */
void pet_ui_close_sheet(void);

#ifdef __cplusplus
}
#endif

#endif /* __PET_UI_H__ */
