/**
 * @file app_splash.c
 * @brief 开机 splash 实现。语义与时序见 app_splash.h。
 */

#include "tal_api.h"

#include "lvgl.h"

#include "app_boot_logo.h"
#include "app_disp.h"
#include "app_splash.h"

/* 底色 —— **必须和生成 logo 时用的 --bg 一致**（见 app_boot_logo.h）。
 *
 * 为什么不能只靠图片自己的透明：LVGL 的 RGB565 没有 alpha 通道，透明像素在
 * 生成那一步就已经按这个颜色合成掉了（app_boot_logo.c 里是一张不透明的位图）。
 * 两边对不上就会在角色周围看到一圈底色方块。 */
#define SPLASH_BG   0x9393C2

static lv_obj_t *sg_scr = NULL;

OPERATE_RET app_splash_show(void)
{
    if (NULL != sg_scr) {
        return OPRT_OK;                     /* 幂等 */
    }

    /* 没有素材就别铺白底：一块什么都没有的白屏比黑屏更像"坏了"。
     * 调用方也不用特判 —— 返回错误就当作没有开机画面。 */
    if (NULL == app_boot_logo_dsc()) {
        PR_WARN("[boot] 没有 logo 素材，跳过开机画面");
        return OPRT_COM_ERROR;
    }

    app_disp_lock();

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, lv_color_hex(SPLASH_BG), 0);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(sg_scr);

    lv_image_set_src(img, app_boot_logo_dsc());
    lv_obj_center(img);                     /* **居中，别的什么都不放** */

    lv_screen_load(sg_scr);

    app_disp_unlock();

    PR_NOTICE("[boot] 开机画面已上屏");
    return OPRT_OK;
}

void app_splash_hide(void)
{
    app_disp_lock();

    if (NULL != sg_scr) {
        /* **只有它已经不是活动屏幕时才删。**
         *
         * 正常路径下 app_router_prepare() 已经把首页顶上去、splash 退居二线，
         * 删它安全。但首页没进去的时候（页面没注册、被 SD 门控挡下……）splash
         * **还是活动屏幕** —— 这时删掉它，LVGL 下一帧就没有活动屏幕了，直接崩：
         *
         *     Fault on thread lvgl / MemFault / fault address 0x2e
         *
         * 2026-10-16 真机上就是这么崩的（开机没插卡 -> 门控挡下桌宠页 ->
         * 路由没初始化 -> 这句把唯一的屏幕删了）。
         *
         * 宁可把开机画面留着（屏幕上至少有东西），也不要留一个没有屏幕的 LVGL。 */
        if (lv_screen_active() == sg_scr) {
            PR_WARN("[boot] 首页没顶上来，开机画面继续留着（删了 LVGL 就没屏幕了）");
            app_disp_unlock();
            return;
        }
        lv_obj_del(sg_scr);
        sg_scr = NULL;
        PR_NOTICE("[boot] 开机画面退场");
    }

    app_disp_unlock();
}
