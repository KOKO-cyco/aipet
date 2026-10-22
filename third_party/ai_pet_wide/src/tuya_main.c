/**
 * @file tuya_main.c
 * @brief Device entry point for the desktop-pet UI.
 *
 * This file is the only part of the app that knows about the board. It brings up the
 * display through `lv_vendor`, hands the UI to `pet_ui_init()`, and starts the LVGL
 * task. The simulator has its own `main()` and compiles this file to nothing.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "tuya_cloud_types.h"

#ifndef LVGL_PC_SIMULATOR

#include "board_com_api.h"
#include "lv_vendor.h"
#include "pet_ui.h"
#include "tal_api.h"
#include "tkl_output.h"

#include "lvgl.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* LVGL runs on its own thread; the stack has to cover widget construction, which is
   where the deep call chains are. The settings page builds nine rows in one pass, so
   this is sized for that rather than for a single card. */
#define PET_LVGL_PRIO       5
#define PET_LVGL_STACK_SIZE (1024 * 8)

/***********************************************************
********************function declaration********************
***********************************************************/

void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);

    /* Registers the LCD and the touch panel. The camera and the codec are declared in
       the app's config, but nothing on this page uses them yet. */
    board_register_hardware();

    /* Brings up the panel and registers the touch input device; no separate indev
       setup is needed. */
    lv_vendor_init(DISPLAY_NAME);

    /* Turn the panel on its side, so LVGL's canvas is the 480x320 this UI is drawn
       for. The board registers the 3.5" 35565 at its native 320x480 portrait
       (BOARD_LCD_WIDTH/HEIGHT, TUYA_DISPLAY_ROTATION_0), and the rotation is applied
       in LVGL's own flush — lv_port_disp.c rotates the drawn buffer with
       lv_draw_sw_rotate() on the way to the physical framebuffer, and lv_indev.c
       applies the same transform to touch coordinates. Nothing in the driver or the
       touch panel has to change, and the framebuffer stays at its native size.

       Doing it here rather than by editing BOARD_LCD_ROTATION keeps the change inside
       this app: that macro is the board's, and every other app on it expects portrait.

       Which of 90 and 270 stands the panel the right way up depends on the ILI9488's
       fixed MADCTL and on the touch panel's own axes, neither of which can be settled
       from the source — if the picture comes up inverted, this is the line to swap. */
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);

    /* The UI is built here rather than in the LVGL task, so it is complete before the
       first frame is drawn and the screen never shows an empty flash. */
    lv_vendor_disp_lock();
    pet_ui_init();
    lv_vendor_disp_unlock();

    lv_vendor_start(PET_LVGL_PRIO, PET_LVGL_STACK_SIZE);
}

/***********************************************************
************************thread setup************************
***********************************************************/

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param;

    memset(&thrd_param, 0, sizeof(THREAD_CFG_T));
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "tuya_app_main";

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}

#endif /* LVGL_PC_SIMULATOR */
