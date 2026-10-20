/**
 * @file pui_status.c
 * @brief Quiet screen chrome; the readings remain demonstration values.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "pui_priv.h"

#include <string.h>

#define MY_CLASS  (&pui_status_class)
#define BADGE_MAX 20

static void __status_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    pui_priv_container(obj);
    lv_obj_set_size(obj, LV_PCT(100), PUI_STATUS_H);
    lv_obj_set_style_pad_hor(obj, PET_PAD_PAGE + 4, 0);
    lv_obj_set_style_pad_column(obj, 6, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

const lv_obj_class_t pui_status_class = {
    .constructor_cb = __status_constructor,
    .instance_size  = sizeof(pui_status_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_status",
};

lv_obj_t *pui_status_create(lv_obj_t *parent)
{
    lv_obj_t     *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    pui_status_t *status;

    lv_obj_class_init_obj(obj);
    status        = (pui_status_t *)obj;
    status->badge = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    pui_spacer(obj);
    status->clock = pui_label_create(obj, "10:30", PET_FONT_CJK_SM, PET_COLOR_TEXT);

    /* The readings the device actually has, each icon bound to the value that follows it:
       the network it is on, the card in its slot, the volume it will speak at, then its
       charge. Bluetooth is gone — the pet has no radio for it — and 音量 earns a place
       beside the charge because the volume slider two screens away is the thing this
       strip is meant to save a trip to. Demonstration values, like the clock. */
    pui_label_create(obj, FONT_AWESOME_WIFI, PET_FONT_ICON, PET_COLOR_TEXT_SUB);
    pui_label_create(obj, FONT_AWESOME_SD_CARD, PET_FONT_ICON, PET_COLOR_TEXT_SUB);
    pui_label_create(obj, FONT_AWESOME_VOLUME_HIGH, PET_FONT_ICON, PET_COLOR_TEXT_SUB);
    pui_label_create(obj, "60%", PET_FONT_CJK_SM, PET_COLOR_TEXT);
    pui_label_create(obj, FONT_AWESOME_BATTERY_FULL, PET_FONT_ICON, PET_COLOR_TEXT_SUB);
    pui_label_create(obj, "80%", PET_FONT_CJK_SM, PET_COLOR_TEXT);
    return obj;
}

void pui_status_set_badge(lv_obj_t *status, uint32_t pending)
{
    pui_status_t *s;
    char          text[BADGE_MAX];

    if (status == NULL || !lv_obj_check_type(status, MY_CLASS)) {
        return;
    }
    s = (pui_status_t *)status;
    if (s->badge == NULL) {
        return;
    }
    lv_snprintf(text, sizeof(text), "待办 %u", (unsigned)pending);

    /* Only when it actually moves. The badge is written on every page build, so popping it
       unconditionally would make the strip twitch on every navigation instead of on the
       change the pop is there to report. */
    if (strcmp(lv_label_get_text(s->badge), text) == 0) {
        return;
    }

    lv_label_set_text(s->badge, text);
    pui_anim_pop(s->badge);
}
