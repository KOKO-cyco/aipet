/**
 * @file pet_snapshot.c
 * @brief 见 pet_snapshot.h。
 */

#include "pet_snapshot.h"

#include "tal_api.h"

#include "app_sprite2d.h"

/***********************************************************
********************function define**************************
***********************************************************/

/** 画布被删掉时释放它后面那块 PSRAM。缓冲不归 lv_canvas 所有。 */
static void prv_canvas_del_cb(lv_event_t *e)
{
    uint16_t *buf = (uint16_t *)lv_event_get_user_data(e);

    if (NULL != buf) {
        tal_psram_free(buf);
    }
}

lv_obj_t *pet_snapshot_create(lv_obj_t *parent, uint16_t bg)
{
    uint16_t *buf;
    lv_obj_t *cv;
    int       w = 0;
    int       h = 0;

    if (NULL == parent) {
        return NULL;
    }

    app_sprite2d_size(&w, &h);
    if (w <= 0 || h <= 0) {
        return NULL;    /* 精灵没起来（卡不在 / 素材缺）—— 不贴图，页面照常 */
    }

    buf = (uint16_t *)tal_psram_malloc((size_t)w * (size_t)h * 2u);
    if (NULL == buf) {
        PR_WARN("[pet] 合影预览缓冲分配失败（%dx%d）", w, h);
        return NULL;
    }

    /* 铺底色 + 直接 blit，不走 render_clip()：那条路径带着"慢拍检测"的静态
     * 计时，快照本来就是隔几十秒调一次的，会在日志里刷一条假的 `[sp2d] 慢拍`。 */
    for (int i = 0; i < w * h; i++) {
        buf[i] = bg;
    }
    app_sprite2d_blit_clip(buf, w, h, 0, 0);

    cv = lv_canvas_create(parent);    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_set_style_radius(cv, 0, 0);
    lv_obj_add_event_cb(cv, prv_canvas_del_cb, LV_EVENT_DELETE, buf);

    return cv;
}
