/**
 * @file app_disp.c
 * @brief 显示 / GUI 服务实现。
 */

#include "tal_api.h"
#include "tdl_display_manage.h"
#include "board_com_api.h"      /* DISPLAY_NAME */

#include "lvgl.h"
#include "lv_vendor.h"

#include "app_disp.h"
#include "app_settings.h"

static TDL_DISP_HANDLE_T sg_disp  = NULL;
static BOOL_T          sg_inited  = FALSE;   /* lv_vendor_init 完成 */
static BOOL_T          sg_started = FALSE;   /* lv_vendor_start 完成 */
static BOOL_T          sg_suspend = FALSE;   /* LVGL 任务处于挂起态 */

static TDL_DISP_HANDLE_T prv_dev(void)
{
    if (NULL == sg_disp) {
        sg_disp = tdl_disp_find_dev((char *)DISPLAY_NAME);
    }
    return sg_disp;
}

OPERATE_RET app_disp_init(void *device_name)
{
    if (sg_inited) {
        return OPRT_OK;
    }

    lv_vendor_init((NULL != device_name) ? device_name : (void *)DISPLAY_NAME);
    sg_inited = TRUE;

    /* 应用用户保存的背光亮度 */
    app_disp_set_brightness(app_settings_get_int(APP_SET_BRIGHTNESS));

    PR_NOTICE("[disp] LVGL 已挂到 %s", (char *)DISPLAY_NAME);
    return OPRT_OK;
}

OPERATE_RET app_disp_start(uint32_t prio, uint32_t stack)
{
    if (!sg_inited) {
        return OPRT_COM_ERROR;
    }
    if (sg_started) {
        return OPRT_OK;
    }

    lv_vendor_start(prio, stack);
    sg_started = TRUE;
    sg_suspend = FALSE;
    return OPRT_OK;
}

void app_disp_lock(void)
{
    lv_vendor_disp_lock();
}

void app_disp_unlock(void)
{
    lv_vendor_disp_unlock();
}

void app_disp_suspend(void)
{
    if (!sg_started || sg_suspend) {
        return;
    }
    lv_vendor_suspend();
    sg_suspend = TRUE;
    PR_DEBUG("[disp] LVGL 挂起");
}

void app_disp_resume(void)
{
    if (!sg_started || !sg_suspend) {
        return;
    }
    lv_vendor_resume();
    sg_suspend = FALSE;
    PR_DEBUG("[disp] LVGL 恢复");
}

BOOL_T app_disp_suspended(void)
{
    return sg_suspend;
}

OPERATE_RET app_disp_set_brightness(int pct)
{
    TDL_DISP_HANDLE_T h = prv_dev();

    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    if (NULL == h) {
        PR_WARN("[disp] 没找到显示设备，背光设置忽略");
        return OPRT_COM_ERROR;
    }
    tdl_disp_set_brightness(h, (uint8_t)pct);
    app_settings_set_int(APP_SET_BRIGHTNESS, pct);
    return OPRT_OK;
}

int app_disp_get_brightness(void)
{
    return app_settings_get_int(APP_SET_BRIGHTNESS);
}
