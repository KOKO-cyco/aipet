/**
 * @file pui_priv.h
 * @brief What the components share with each other, and nothing else.
 *
 * Not in include/: a page must not be able to reach these, because every one of them is
 * a rule that is only a rule while it holds everywhere. `pui_priv_container()` in
 * particular — the "a decorative container must not swallow taps" fix — is only worth
 * anything if a page cannot build a container that skipped it, which is the argument for
 * the whole library.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PUI_PRIV_H__
#define __PUI_PRIV_H__

#include "pui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Strip everything the default theme puts on a bare container.
 *
 * The default theme is applied to every object by lv_theme_apply() before the
 * constructor's own styles, so a class that wants a transparent, unpadded, borderless,
 * unscrollable box has to say so explicitly rather than rely on the defaults being
 * empty. Does not touch size or layout: those are the caller's to set.
 *
 * Also removes LV_OBJ_FLAG_CLICKABLE, so the container passes taps through to whatever
 * it is drawn over. Every container in this library is built on this.
 */
void pui_priv_container(lv_obj_t *obj);

/**
 * @brief Give an object the shared pressed-state feedback.
 *
 * Dims the whole object rather than darkening its fill: one shared style cannot express
 * "one shade darker than whatever this happens to be", and dimming reads as pressed on
 * a solid fill, a wash and an outline alike.
 */
void pui_priv_pressable(lv_obj_t *obj);

/**
 * @brief The same feedback for a component that holds children.
 *
 * Dims and does not squash. Scaling an object puts it on LV_LAYER_TYPE_TRANSFORM, which
 * re-renders its whole subtree offscreen on every frame, and a card is the largest subtree
 * in the app. It is also the better half to keep on a surface: a card that shrinks under a
 * finger moves the text the finger was aiming at.
 */
void pui_priv_pressable_flat(lv_obj_t *obj);

/**
 * @brief Paint an object as a surface: fill, hairline, corner.
 *
 * The one place PUI_PAINT_T is turned into styles, so a fill means the same thing on a
 * button, a card and a tile.
 */
void pui_priv_surface(lv_obj_t *obj, const PUI_PAINT_T *paint, lv_coord_t radius);

/**
 * @brief Give a solid-filled object the resting half of its press.
 *
 * A shallow warm shadow under the fill, so the button sits just above the page and has
 * somewhere to sink when the pressed style collapses it. Solid fills only: a ghost or an
 * outline has no fill for a shadow to gather under.
 */
void pui_priv_press_shadow(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* __PUI_PRIV_H__ */
