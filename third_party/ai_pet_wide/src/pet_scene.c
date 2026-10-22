/**
 * @file pet_scene.c
 * @brief The room behind the character. See pet_scene.h for what it is for and why it is
 *        kept this cheap.
 *
 * Two things move here, and both are chosen to stay off the expensive paths:
 *
 *   - A soft pale band, the light, drifting a long way up and down on a cycle long enough
 *     that it reads as the room's light rather than as an animation. It is a rounded
 *     rectangle faded by its own low opacity, not a gradient: a gradient over the card
 *     would repaint the whole card every frame, and this build's software renderer has no
 *     shadow cache to soften that (LV_DRAW_SW_SHADOW_CACHE_SIZE is 0). The band is short,
 *     so the area that repaints is the band, not the card.
 *   - A handful of motes, each bobbing on its own slow cycle, offset so they never sync.
 *     They are circles a few pixels across; the area any one of them dirties in a frame is
 *     its own size.
 *
 * Everything is translate or opacity. Nothing scales and nothing rotates — both would put
 * the object on LV_LAYER_TYPE_TRANSFORM and re-render it offscreen every frame, which for
 * a full-height band is exactly the cost this file exists to avoid.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_scene.h"

#include "pet_theme.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* The light band. A horizontal stadium — rounded top and bottom, square sides — rather
   than a rectangle, so its own corners soften it without a gradient. It drifts the travel
   distance around the middle of the card, which is enough to cross the character and come
   back without ever reaching the card's edges. */
#define SCENE_LIGHT_H      88
#define SCENE_LIGHT_TRAVEL 120
#define SCENE_LIGHT_OPA    LV_OPA_20

/* The motes. Four pixels across is a speck; the travel is how far each bobs from its home
   before turning back, so a mote never leaves the card but is never still either. */
#define SCENE_MOTE_D      4
#define SCENE_MOTE_OPA    LV_OPA_30
#define SCENE_MOTE_TRAVEL 26
#define SCENE_MOTE_COUNT  PET_AMBIENT_MOTES

/***********************************************************
***********************variable define**********************
***********************************************************/

static lv_obj_t *sg_scene;

/***********************************************************
********************function define**************************
***********************************************************/

/* The two trampolines the animations ride. lv_obj_set_style_*() take a selector as their
   third argument, so they cannot be handed to lv_anim_set_exec_cb() directly; these pin it
   to the main part. Same shape as the ones in pui_anim.c, kept local because a scene is not
   a library transition. */
static void __translate_y_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void __translate_x_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/**
 * @brief The light: one low band, drifting up and down forever.
 *
 * Playback with an infinite repeat rather than two animations: the band turns around at
 * each end on the ease, so the reversal is a slow settling rather than a bounce, and there
 * is no reset frame to hide because it never leaves its range.
 */
static void __build_light(lv_obj_t *scene)
{
    lv_obj_t *light = lv_obj_create(scene);
    lv_anim_t a;

    pui_passthrough(light);
    lv_obj_set_size(light, LV_PCT(100), SCENE_LIGHT_H);
    lv_obj_set_style_radius(light, SCENE_LIGHT_H / 2, 0);
    lv_obj_set_style_bg_color(light, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(light, SCENE_LIGHT_OPA, 0);
    lv_obj_center(light);

    lv_anim_init(&a);
    lv_anim_set_var(&a, light);
    lv_anim_set_exec_cb(&a, __translate_y_cb);
    lv_anim_set_values(&a, -SCENE_LIGHT_TRAVEL, SCENE_LIGHT_TRAVEL);
    lv_anim_set_duration(&a, PET_AMBIENT_SWEEP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_playback_duration(&a, PET_AMBIENT_SWEEP_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/**
 * @brief One mote: a speck that bobs on its own clock.
 *
 * Where each mote sits and how fast it moves are two fixed tables rather than arithmetic
 * on the index. Six motes do not need a formula, and a hand-set table is what keeps them
 * from looking like a row — the point is that they are scattered and unaligned, and a
 * formula is the thing most likely to accidentally align them.
 */
static void __build_mote(lv_obj_t *scene, uint32_t i)
{
    /* clang-format off */
    static const lv_coord_t home_x[SCENE_MOTE_COUNT] = { -62, -33,  -6,  20,  47,  66 };
    static const lv_coord_t home_y[SCENE_MOTE_COUNT] = { -70,  38, -28,  66, -46,  14 };
    static const uint32_t   period[SCENE_MOTE_COUNT] = { 8200, 6400, 9400, 7200, 8600, 5600 };
    /* clang-format on */

    lv_obj_t *mote = lv_obj_create(scene);
    lv_anim_t a;

    pui_passthrough(mote);
    lv_obj_set_size(mote, SCENE_MOTE_D, SCENE_MOTE_D);
    lv_obj_set_style_radius(mote, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mote, PET_COLOR_SHADOW, 0);
    lv_obj_set_style_bg_opa(mote, SCENE_MOTE_OPA, 0);
    lv_obj_center(mote);
    lv_obj_set_style_translate_x(mote, home_x[i], 0);
    lv_obj_set_style_translate_y(mote, home_y[i], 0);

    /* The bob, and the stagger. A delay spread across the motes is what makes them read
       as dust in the air rather than as one object with six copies. */
    lv_anim_init(&a);
    lv_anim_set_var(&a, mote);
    lv_anim_set_exec_cb(&a, __translate_y_cb);
    lv_anim_set_values(&a, home_y[i] - SCENE_MOTE_TRAVEL, home_y[i] + SCENE_MOTE_TRAVEL);
    lv_anim_set_duration(&a, period[i]);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_playback_duration(&a, period[i]);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, (uint32_t)i * 1200);
    lv_anim_start(&a);
}

void pet_scene_build(lv_obj_t *column)
{
    if (column == NULL) {
        return;
    }

    sg_scene = lv_obj_create(column);
    pui_passthrough(sg_scene);

    /* Out of the column's flex flow, at the column's origin and its size. It is created
       first, so it is the bottom of the draw order and the character stands in front of
       it. The radius and corner clip keep a mote or the light's ends from spilling past
       the card's rounded corner. */
    lv_obj_add_flag(sg_scene, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(sg_scene, 0, 0);
    lv_obj_set_size(sg_scene, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(sg_scene, PET_RADIUS_CARD, 0);
    lv_obj_set_style_clip_corner(sg_scene, true, 0);

    __build_light(sg_scene);

    for (uint32_t i = 0; i < SCENE_MOTE_COUNT; i++) {
        __build_mote(sg_scene, i);
    }
}

void pet_scene_parallax(int dir)
{
    lv_anim_t  a;
    lv_coord_t from;

    if (sg_scene == NULL || dir == 0) {
        return;
    }

    /* Opposite the page, so the room is a plane behind the panel rather than part of it.
       Set immediately as well as animated, so the shift is there on the frame the page
       starts moving rather than a frame later. */
    from = dir > 0 ? -PET_AMBIENT_PARALLAX : PET_AMBIENT_PARALLAX;
    lv_obj_set_style_translate_x(sg_scene, from, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, sg_scene);
    lv_anim_set_exec_cb(&a, __translate_x_cb);
    lv_anim_set_values(&a, from, 0);
    lv_anim_set_duration(&a, PET_NAV_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
