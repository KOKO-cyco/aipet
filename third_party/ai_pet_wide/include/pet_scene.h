/**
 * @file pet_scene.h
 * @brief The room behind the character: the slow light and the dust, and the little
 *        parallax they give the panel when a page moves.
 *
 * The card on the left used to be a flat cream field with a sprite standing on it. This
 * puts a room behind the sprite — a light that drifts and a few motes that float in it —
 * so the character is *somewhere* rather than on a swatch, and the card is never quite
 * still. The whole thing is decoupled from the art: none of it knows what the character
 * is, so swapping the still for a real animation touches pet_page_pet.c and not this.
 *
 * Nothing here loops anything expensive. The rule from pui_anim.c holds — no scale, no
 * rotation — and the two things that do move are a low-opacity band and a handful of
 * 4 px motes, both on long, slow cycles. The intent is ambience: if the user can tell
 * exactly when a mote resets, it is moving too much.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_SCENE_H__
#define __PET_SCENE_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Build the room into the character's card.
 *
 * Call once, and before anything else is added to @p column: the room is a sibling of the
 * character and has to be behind it, which here means being the first child. It ignores
 * layout, so it neither sets the card's size nor moves anything already in it.
 *
 * @param[in] column  The character's card — the same object pet_page_pet_build() gets.
 */
void pet_scene_build(lv_obj_t *column);

/**
 * @brief Nudge the room the other way while the panel changes pages.
 *
 * The character does not move when the panel does, but if the room behind it is perfectly
 * still the left half of the screen reads as painted on while the right half moves. A few
 * pixels of opposite drift, eased back, is enough to separate the two planes without ever
 * being something the user can point at.
 *
 * @param[in] dir  The direction of the page travel: +1 a push, -1 a pop, 0 nothing. The
 *                 room leans the other way, so it is the *opposite* of the page.
 */
void pet_scene_parallax(int dir);

#ifdef __cplusplus
}
#endif

#endif /* __PET_SCENE_H__ */
