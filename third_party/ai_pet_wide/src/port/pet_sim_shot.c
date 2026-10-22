/**
 * @file pet_sim_shot.c
 * @brief PC-simulator entry point, with an optional screenshot mode.
 *
 * The simulator calls `SIM_ENTRY_FUNC`, which `sim_config.cmake` points at
 * `pet_sim_main` here rather than at `pet_ui_init` directly. That keeps the screenshot
 * plumbing out of the UI code: `pet_ui.c` has no idea it can be captured.
 *
 * Setting `PET_SIM_SHOT` to a file path builds the UI, lets LVGL settle, renders the
 * screen to an RGB888 buffer and writes it as a binary PPM (P6) before exiting.
 * Without the variable this is just `pet_ui_init()` and the normal interactive
 * simulator runs.
 *
 *     PET_SIM_SHOT=/tmp/pet_home.ppm ./dist/lvgl_sim
 *
 * A headless build has no pointer, so the pages behind a tap cannot be reached.
 * `PET_SIM_CLICK` supplies a semicolon-separated list of taps to replay before the
 * capture. An entry with a third number is a drag, which is how a scrolling page is
 * read to the end; a bare number waits that many cycles; a *negative* third number
 * presses and holds without letting go, which is how a pressed state is captured:
 *
 *     # The 拍照 tile on the home page, then the shutter.
 *     PET_SIM_SHOT=/tmp/pet_camera.ppm PET_SIM_CLICK="257,70;330,245" ./dist/lvgl_sim
 *     # The 设置 tile, then a drag up the settings list.
 *     PET_SIM_SHOT=/tmp/pet_settings.ppm PET_SIM_CLICK="420,70;340,290,120" ./dist/lvgl_sim
 *     # The camera page settled, then the shutter held down for 40 cycles.
 *     PET_SIM_SETTLE=1 PET_SIM_CLICK="90;257,70;90;330,245,-40" ./dist/lvgl_sim
 *
 * The taps go to a second, synthetic pointer indev; the SDL mouse is removed in shot
 * mode so the two cannot fight over the active object.
 *
 * PPM is used because it needs no encoder: it is a three-line header followed by raw
 * pixels, so the whole thing is a few lines of C.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_ui.h"

#ifdef LVGL_PC_SIMULATOR

#include "lvgl.h"
#include "pui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/***********************************************************
************************macro define************************
***********************************************************/

/* Number of LVGL cycles to run before capturing. Layout and the first draw both
   happen inside lv_timer_handler(), so a capture taken immediately after
   pet_ui_init() would come back blank. PET_SIM_SETTLE raises it, which is what
   lets a fade finish before the shot is taken. */
#define PET_SHOT_SETTLE_CYCLES 30

/* One cycle is one pass of the LVGL timer handler. The sleep keeps the real-time
   tick moving so refresh and animation timers actually come due. */
#define PET_SIM_CYCLE_US 4000

/* A tap is a few cycles held down followed by a few released. LVGL needs the
   press to be read before the release to see a click at all. */
#define PET_SIM_PRESS_CYCLES   3
#define PET_SIM_RELEASE_CYCLES 4

/* A drag is a press, a run of steps, and a release. LVGL only turns a press into a
   scroll once the pointer has travelled past the scroll limit, so the steps have to
   be read one at a time rather than jumped in one go. */
#define PET_SIM_DRAG_STEPS 10

#define PET_SIM_MAX_TAPS 16

/* Cap on a single wait entry, so a typo cannot hang a headless run. */
#define PET_SIM_MAX_WAIT 3000

/***********************************************************
***********************variable define**********************
***********************************************************/

static lv_point_t sg_sim_point;
static bool       sg_sim_pressed;

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief Write a snapshot buffer as a binary PPM.
 *
 * LVGL stores RGB888 as B,G,R bytes, while PPM wants R,G,B, so the two outer
 * bytes are swapped on the way out.
 */
static void __write_ppm(const lv_draw_buf_t *buf, const char *path)
{
    FILE          *fp;
    const uint8_t *row;
    uint32_t       x;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "[sim] cannot open %s for writing\n", path);
        return;
    }

    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)buf->header.w, (unsigned)buf->header.h);

    for (uint32_t y = 0; y < buf->header.h; y++) {
        row = buf->data + (size_t)y * buf->header.stride;

        for (x = 0; x < buf->header.w; x++) {
            uint8_t rgb[3] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3 + 0]};
            fwrite(rgb, 1, sizeof(rgb), fp);
        }
    }

    fclose(fp);
}

/**
 * @brief Run one LVGL cycle, optionally reading the synthetic pointer.
 */
static void __sim_cycle(lv_indev_t *indev)
{
    if (indev != NULL) {
        lv_indev_read(indev);
    }

    lv_timer_handler();
    usleep(PET_SIM_CYCLE_US);
}

/**
 * @brief How many cycles to settle for: PET_SIM_SETTLE, or the default.
 */
static int __sim_settle_cycles(void)
{
    const char *env = getenv("PET_SIM_SETTLE");
    long        n;

    if (env == NULL || env[0] == '\0') {
        return PET_SHOT_SETTLE_CYCLES;
    }

    n = strtol(env, NULL, 10);
    if (n <= 0 || n > 10000) {
        fprintf(stderr, "[sim] ignoring out-of-range PET_SIM_SETTLE=%s\n", env);
        return PET_SHOT_SETTLE_CYCLES;
    }

    return (int)n;
}

/**
 * @brief Stand-in for a touch panel: reports whatever __sim_tap() last set.
 */
static void __sim_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);

    data->point = sg_sim_point;
    data->state = sg_sim_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/**
 * @brief Press and release at one point, letting LVGL process both.
 */
static void __sim_tap(lv_indev_t *indev, lv_coord_t x, lv_coord_t y)
{
    sg_sim_point.x = x;
    sg_sim_point.y = y;

    sg_sim_pressed = true;
    for (int i = 0; i < PET_SIM_PRESS_CYCLES; i++) {
        __sim_cycle(indev);
    }

    sg_sim_pressed = false;
    for (int i = 0; i < PET_SIM_RELEASE_CYCLES; i++) {
        __sim_cycle(indev);
    }
}

/**
 * @brief Press at one point, travel to another, and release — a scroll or a swipe.
 *
 * The intermediate points matter: LVGL decides a press is a scroll only after the
 * pointer has moved, and it re-reads the pointer once per cycle, so the travel has
 * to be spread over several cycles rather than set in one step. The same travel is
 * what a gesture is measured from, so this is also how a swipe is replayed.
 */
static void __sim_drag(lv_indev_t *indev, lv_coord_t x_from, lv_coord_t y_from, lv_coord_t x_to, lv_coord_t y_to)
{
    lv_coord_t x;
    lv_coord_t y;

    sg_sim_point.x = x_from;
    sg_sim_point.y = y_from;

    sg_sim_pressed = true;
    for (int i = 0; i < PET_SIM_PRESS_CYCLES; i++) {
        __sim_cycle(indev);
    }

    for (int i = 1; i <= PET_SIM_DRAG_STEPS; i++) {
        x              = x_from + (lv_coord_t)(((int32_t)(x_to - x_from) * i) / PET_SIM_DRAG_STEPS);
        y              = y_from + (lv_coord_t)(((int32_t)(y_to - y_from) * i) / PET_SIM_DRAG_STEPS);
        sg_sim_point.x = x;
        sg_sim_point.y = y;
        __sim_cycle(indev);
    }

    sg_sim_pressed = false;
    for (int i = 0; i < PET_SIM_RELEASE_CYCLES; i++) {
        __sim_cycle(indev);
    }
}

/**
 * @brief Press at one point and leave the pointer there.
 *
 * A tap always ends with a release, so it can never capture a state that only exists
 * while the finger is down — and a press transition is one of those: it runs while the
 * object is pressed and reverses the moment it is let go. Written as a negative third
 * field on a tap, `x,y,-N`, which is a value the drag syntax has no other use for.
 */
static void __sim_hold(lv_indev_t *indev, lv_coord_t x, lv_coord_t y, long cycles)
{
    sg_sim_point.x = x;
    sg_sim_point.y = y;
    sg_sim_pressed = true;

    for (long i = 0; i < cycles && i < PET_SIM_MAX_WAIT; i++) {
        __sim_cycle(indev);
    }
}

/**
 * @brief Create the synthetic pointer, replacing the SDL mouse.
 *
 * Two pointer indevs would both be processed every cycle, and whichever is read
 * last owns the active object. Dropping the SDL one keeps a headless capture
 * deterministic.
 */
static lv_indev_t *__sim_pointer_create(void)
{
    lv_indev_t *indev;
    lv_indev_t *next;

    for (indev = lv_indev_get_next(NULL); indev != NULL; indev = next) {
        next = lv_indev_get_next(indev);
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_delete(indev);
        }
    }

    indev = lv_indev_create();
    if (indev == NULL) {
        return NULL;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, __sim_pointer_read_cb);

    /* lv_indev_create() arms a read timer, and lv_timer_handler() — which every
       __sim_cycle() runs — fires it on its own schedule. The timer reads the same
       point the script last set, so once the replay is slower than
       LV_INDEV_DEF_READ_PERIOD the extra read reports no movement, and LVGL treats a
       stationary read as the end of the gesture and clears what it had accumulated.
       The script would then have to finish a swipe inside one 30 ms window. Replay is
       driven by hand here, so the timer has nothing to do. */
    lv_timer_pause(lv_indev_get_read_timer(indev));

    return indev;
}

/**
 * @brief Replay a "x,y;x,y;..." tap script, where "x,y1,y2" is a scroll.
 *
 * Malformed entries are skipped rather than treated as an error: this is a
 * debugging aid, and a typo in the script should still produce a capture.
 */
static void __sim_play_taps(const char *script)
{
    lv_indev_t *indev = __sim_pointer_create();
    const char *cur   = script;

    if (indev == NULL) {
        fprintf(stderr, "[sim] cannot create the synthetic pointer\n");
        return;
    }

    for (int n = 0; n < PET_SIM_MAX_TAPS && *cur != '\0'; n++) {
        char *end;
        long  x = strtol(cur, &end, 10);
        long  y;

        /* A bare number is a wait, in cycles. An animation is driven by elapsed
           time, so a script that only taps cannot reach the state it is heading for. */
        if (end != cur && *end != ',') {
            printf("[sim] wait %ld cycles\n", x);
            for (long i = 0; i < x && i < PET_SIM_MAX_WAIT; i++) {
                __sim_cycle(NULL);
            }

            cur = (*end == ';') ? end + 1 : end;
            continue;
        }

        if (end == cur || *end != ',') {
            fprintf(stderr, "[sim] skipping malformed tap at \"%s\"\n", cur);
            break;
        }

        cur = end + 1;
        y   = strtol(cur, &end, 10);
        if (end == cur) {
            fprintf(stderr, "[sim] skipping malformed tap at \"%s\"\n", cur);
            break;
        }

        /* A third number makes it a drag straight down the column x; a fourth makes it
           a drag to that point, which is how a sideways swipe is replayed; a negative
           third makes it a hold, which is how a pressed state is captured. */
        if (*end == ',') {
            char *drag_end;
            long  third = strtol(end + 1, &drag_end, 10);

            if (drag_end == end + 1) {
                fprintf(stderr, "[sim] skipping malformed drag at \"%s\"\n", cur);
                break;
            }

            if (third < 0) {
                printf("[sim] hold %ld,%ld for %ld cycles\n", x, y, -third);
                __sim_hold(indev, (lv_coord_t)x, (lv_coord_t)y, -third);
                end = drag_end;
            } else if (*drag_end == ',') {
                char *to_end;
                long  x_to = third;
                long  y_to = strtol(drag_end + 1, &to_end, 10);

                if (to_end == drag_end + 1) {
                    fprintf(stderr, "[sim] skipping malformed drag at \"%s\"\n", cur);
                    break;
                }

                printf("[sim] drag %ld,%ld -> %ld,%ld\n", x, y, x_to, y_to);
                __sim_drag(indev, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)x_to, (lv_coord_t)y_to);
                end = to_end;
            } else {
                printf("[sim] drag %ld,%ld -> %ld\n", x, y, third);
                __sim_drag(indev, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)x, (lv_coord_t)third);
                end = drag_end;
            }
        } else {
            printf("[sim] tap %ld,%ld\n", x, y);
            __sim_tap(indev, (lv_coord_t)x, (lv_coord_t)y);
        }

        cur = (*end == ';') ? end + 1 : end;
    }
}

/**
 * @brief Print every object's class, position, size, transform and flags.
 *
 * `lv_obj_dump_tree()` only prints under LV_USE_LOG, and what is wanted here is
 * geometry rather than the tree shape. A label's text is printed too: the geometry of
 * a page is mostly the same whichever mode is selected, so without it a capture cannot
 * say whether a tap landed.
 *
 * The scale and the opacity are printed because neither is geometry. A press squash and
 * a change pop are both draw-time transforms — the object's box does not move, so a dump
 * of coordinates alone cannot tell a settled frame from one caught at the peak of an
 * animation. These two can, and they are the only way to read a state that exists for a
 * tenth of a second.
 */
static void __sim_dump(lv_obj_t *obj, int depth)
{
    uint32_t n = lv_obj_get_child_count(obj);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t   *child = lv_obj_get_child(obj, i);
        lv_area_t   a;
        const char *text = "";

        lv_obj_get_coords(child, &a);

        if (lv_obj_check_type(child, &lv_label_class) || lv_obj_check_type(child, &pui_text_class)) {
            text = lv_label_get_text(child);
        }

        printf("[dump] %*s%s x=%d..%d y=%d..%d scale=%d opa=%d clickable=%d scrollable=%d hidden=%d \"%s\"\n",
               depth * 2, "", lv_obj_get_class(child)->name, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
               (int)lv_obj_get_style_transform_scale_x(child, 0), (int)lv_obj_get_style_opa(child, 0),
               (int)lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE), (int)lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE),
               (int)lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN), text);

        __sim_dump(child, depth + 1);
    }
}

void pet_sim_main(void)
{
    const char    *path   = getenv("PET_SIM_SHOT");
    const char    *clicks = getenv("PET_SIM_CLICK");
    int            settle;
    lv_draw_buf_t *shot;

    pet_ui_init();

    if (path == NULL || path[0] == '\0') {
        return;
    }

    settle = __sim_settle_cycles();

    /* Let the first page lay out and draw before anything is tapped. */
    for (int i = 0; i < settle; i++) {
        __sim_cycle(NULL);
    }

    if (clicks != NULL && clicks[0] != '\0') {
        __sim_play_taps(clicks);

        for (int i = 0; i < settle; i++) {
            __sim_cycle(NULL);
        }
    }

    if (getenv("PET_SIM_DUMP") != NULL) {
        __sim_dump(lv_screen_active(), 0);
    }

    shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (shot == NULL) {
        fprintf(stderr, "[sim] snapshot failed\n");
        exit(1);
    }

    __write_ppm(shot, path);
    printf("[sim] wrote %s (%ux%u)\n", path, (unsigned)shot->header.w, (unsigned)shot->header.h);

    lv_draw_buf_destroy(shot);

    /* The caller's loop would run forever; a capture is a one-shot job. */
    exit(0);
}

#endif /* LVGL_PC_SIMULATOR */
