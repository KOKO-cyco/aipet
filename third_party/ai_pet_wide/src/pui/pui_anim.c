/**
 * @file pui_anim.c
 * @brief The library's motion, and the one rule that shapes all of it: a band rising, a
 *        badge popping, a sheet opening, a page sliding, a field shaking off a wrong press.
 *
 * The rule is a cost rule, and it is in LVGL rather than here. An object whose
 * transform_scale is not 256 — or whose rotation or skew is not zero — is drawn through
 * LV_LAYER_TYPE_TRANSFORM: LVGL re-renders its whole subtree into an offscreen band buffer
 * on every frame of the animation (lv_obj_style.c, calculate_layer_type(), and the layer
 * draw in lv_refr.c). Opacity and translate take no such path; the only opacity that does
 * is opa_layered, which is the next line of that same function and which nothing here
 * uses. And LV_DRAW_SW_SHADOW_CACHE_SIZE is 0 in this build, so a card on the layer path
 * re-blurs its own shadow per frame as well.
 *
 * So: **scale is for leaves, and anything with children moves by translating and fading.**
 * That is not only the cheap choice, it is the better-looking one — scaling a card scales
 * the text inside it, and a surface that shrinks under a finger moves the thing the finger
 * was aiming at. What a container wants is to arrive, and what a button wants is to give.
 *
 * Two smaller things are also load-bearing here:
 *
 *   - The transform pivot defaults to (0, 0). Nothing in LVGL sets one — the default for
 *     the prop is 0, unlike transform_scale whose default is LV_SCALE_NONE
 *     (lv_style_prop_get_default()) — so a squash written without a pivot shrinks a button
 *     toward its own top-left corner and reads as the label sliding rather than as the
 *     button pressing. Every scale set here names its pivot.
 *   - Every animation is set on the object it moves (lv_anim_set_var). lv_obj_delete()
 *     calls lv_anim_delete(obj, NULL) in its destructor, so an animation whose object the
 *     page stack trashes mid-flight dies with it. That is why no call site has to hold a
 *     handle to cancel, and why the staggered arrivals below are safe on a page the user
 *     can leave 40 ms after it opened.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

/***********************************************************
********************function define**************************
***********************************************************/

static void __opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void __translate_y_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void __translate_x_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

static void __bg_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void __scale_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, v, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, v, 0);
}

/**
 * @brief Bring an object up from below and out of nothing.
 *
 * The two ends are written onto the object *before* the animations start rather than left
 * to the first frame the animation runs on. An animation with a delay does not touch its
 * variable until the delay is up, so a band that only set its end state inside the
 * callback would sit fully opaque and in place for its whole stagger and then jump to
 * transparent to begin — the one frame of the whole sequence that would be seen.
 */
static void __rise(lv_obj_t *obj, lv_coord_t px, uint32_t ms, uint32_t delay)
{
    lv_anim_t a;

    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(obj, px, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, __opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* Two animations rather than one callback deriving both values from a single progress
       value: this way each property gets LVGL's own path over its own range, and the two
       are independent of each other rather than coupled through an arithmetic the reader
       has to check. */
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, __translate_y_cb);
    lv_anim_set_values(&a, px, 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void pui_anim_rise_in(lv_obj_t *obj, uint8_t index)
{
    if (obj == NULL) {
        return;
    }

    __rise(obj, PUI_RISE_PX, PUI_RISE_MS, (uint32_t)index * PUI_RISE_STEP);
}

void pui_anim_pop(lv_obj_t *obj)
{
    lv_anim_t a;

    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, __scale_cb);
    lv_anim_set_values(&a, LV_SCALE_NONE, PUI_POP_SCALE);
    lv_anim_set_duration(&a, PUI_POP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    /* The return trip, and the reason there is a playback at all rather than a second
       animation: it ends at exactly LV_SCALE_NONE, so the object is left with no scale
       and calculate_layer_type() stops putting it on the offscreen path the moment the
       animation is over. A pop that ended anywhere else would leave the badge permanently
       more expensive to draw than the text beside it. */
    lv_anim_set_playback_duration(&a, PUI_POP_MS);
    lv_anim_start(&a);
}

void pui_anim_sheet_in(lv_obj_t *scrim)
{
    lv_obj_t *sheet;
    lv_opa_t  target;
    lv_anim_t a;

    if (scrim == NULL) {
        return;
    }

    /* Read off the scrim rather than named here: how dark a scrim is belongs to
       pui_screen_scrim(), and a second copy of that number in this file is a number that
       can disagree with it. */
    target = lv_obj_get_style_bg_opa(scrim, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_TRANSP, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, scrim);
    lv_anim_set_exec_cb(&a, __bg_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, target);
    lv_anim_set_duration(&a, PUI_SHEET_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* The scrim is a centred flex column whose only child is the sheet, so the first child
       is the card — there is no handle to the sheet to pass in, and a caller that had to
       keep one would be keeping a second name for the thing it just built. */
    sheet = lv_obj_get_child(scrim, 0);
    if (sheet != NULL) {
        __rise(sheet, PUI_SHEET_RISE, PUI_SHEET_MS, 0);
    }
}

void pui_anim_translate_x(lv_obj_t *obj, lv_coord_t from, lv_coord_t to, uint32_t ms, BOOL_T ease_in,
                          lv_anim_completed_cb_t done)
{
    lv_anim_t a;

    if (obj == NULL) {
        return;
    }

    /* Written before the animation rather than left to its first frame, so the page is
       already off the edge on the frame it is created and does not flash in place first. */
    lv_obj_set_style_translate_x(obj, from, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    /* The object carried on the animation, so a completed callback has it without a
       file-static to read and can be sure it retired the page it was watching. */
    lv_anim_set_user_data(&a, obj);
    lv_anim_set_exec_cb(&a, __translate_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_completed_cb(&a, done);

    /* A named curve rather than ease_in/ease_out: those two are the standard 0.42 set and
       neither has the long settling tail a page wants. The enter curve is expo-out —
       cubic-bezier(0.16, 1, 0.3, 1), the fastest-moving, flattest-ending curve Material
       uses for exactly this — and the exit is cubic-bezier(0.7, 0, 0.84, 0), a soft start
       that keeps the leaving page from outrunning the arriving one. Both are set as raw
       control points, of 1024, because the bezier path is generic and takes per-animation
       parameters. */
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    if (ease_in) {
        lv_anim_set_bezier3_param(&a, 717, 0, 860, 0);
    } else {
        lv_anim_set_bezier3_param(&a, 164, 1024, 307, 1024);
    }
    lv_anim_start(&a);
}

void pui_anim_shake(lv_obj_t *obj, lv_coord_t px, uint32_t ms)
{
    lv_anim_t a;

    if (obj == NULL || px <= 0) {
        return;
    }

    /* One animation with a playback and a repeat, rather than a chain of short ones: it
       ends back at 0 on its own, so nothing has to be cleared afterwards and a second shake
       started mid-wobble replaces this one (same variable, same callback). */
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, __translate_x_cb);
    lv_anim_set_values(&a, 0, px);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_playback_duration(&a, ms);
    lv_anim_set_repeat_count(&a, 2);
    lv_anim_start(&a);
}
