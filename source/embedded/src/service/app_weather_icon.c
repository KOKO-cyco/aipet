/**
 * @file app_weather_icon.c
 * @brief 见 app_weather_icon.h。
 */

#include "app_weather_icon.h"

#include <stdio.h>
#include <string.h>

#include "tal_api.h"

#include "app_storage.h"
#include "app_weather.h"

/***********************************************************
************************macro define************************
***********************************************************/

/** 缓存几张。一屏最多 4 张（今天 + 3 天详情），留一倍余量。 */
#define WIC_CACHE_N     8

/** WXI1 头：'W''X''I''1' + u16 宽 + u16 高。 */
#define WIC_HDR_LEN     8
#define WIC_MAGIC       "WXI1"

/** 单张图的上限：48x48x3 ≈ 6.8KB，给 64KB 防的是"拿错文件去 malloc 一大块"。 */
#define WIC_FILE_MAX    (64u * 1024u)

/***********************************************************
***********************type define*************************
***********************************************************/

typedef struct {
    int32_t          code;      /**< -1 = 空槽 */
    uint8_t         *blob;      /**< 文件原样（头 + 两个平面） */
    uint32_t         age;       /**< LRU 时间戳 */
    lv_image_dsc_t   dsc;       /**< 指向 blob + WIC_HDR_LEN */
} APP_WICON_SLOT_T;

/***********************************************************
***********************variable define**********************
***********************************************************/

static APP_WICON_SLOT_T sg_slot[WIC_CACHE_N];
static uint32_t         sg_age;
static BOOL_T           sg_inited;

/***********************************************************
**********************function define************************
***********************************************************/

static void prv_slot_free(APP_WICON_SLOT_T *s)
{
    if (NULL != s->blob) {
        tal_psram_free(s->blob);
        s->blob = NULL;
    }
    s->code = -1;
}

/**
 * @brief 把一张图标读进槽位。
 *
 * 文件头校验通过之后，整块缓冲直接当图像数据用：WXI1 的两个平面就是 LVGL
 * RGB565A8 的布局（RGB 平面 stride = w*2，A 平面 stride = w，两段连续）。
 */
static BOOL_T prv_load(APP_WICON_SLOT_T *s, int32_t code)
{
    char      path[64];
    uint32_t  size = 0;
    uint8_t  *blob;
    uint16_t  w;
    uint16_t  h;
    uint32_t  need;

    lv_snprintf(path, sizeof(path), APP_WICON_FMT, (int)code);

    if (!app_storage_probe(path, &size) || size < WIC_HDR_LEN || size > WIC_FILE_MAX) {
        return FALSE;
    }

    blob = (uint8_t *)tal_psram_malloc(size);
    if (NULL == blob) {
        PR_WARN("[weather] 图标 %d 分配 %u 字节失败", (int)code, (unsigned)size);
        return FALSE;
    }

    if (OPRT_OK != app_storage_read_all(path, blob, size)) {
        tal_psram_free(blob);
        return FALSE;
    }

    if (0 != memcmp(blob, WIC_MAGIC, 4)) {
        PR_WARN("[weather] %s 不是 WXI1 格式", path);
        tal_psram_free(blob);
        return FALSE;
    }

    w    = (uint16_t)(blob[4] | ((uint16_t)blob[5] << 8));
    h    = (uint16_t)(blob[6] | ((uint16_t)blob[7] << 8));
    need = (uint32_t)w * h * 3u;

    if (w == 0 || h == 0 || size != WIC_HDR_LEN + need) {
        PR_WARN("[weather] %s 尺寸不对：%ux%u，文件 %u（应为 %u）",
                path, w, h, (unsigned)size, (unsigned)(WIC_HDR_LEN + need));
        tal_psram_free(blob);
        return FALSE;
    }

    prv_slot_free(s);

    s->blob = blob;
    s->code = code;

    memset(&s->dsc, 0, sizeof(s->dsc));
    s->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s->dsc.header.cf     = LV_COLOR_FORMAT_RGB565A8;
    s->dsc.header.w      = w;
    s->dsc.header.h      = h;
    s->dsc.header.stride = (uint16_t)(w * 2u);
    s->dsc.data          = blob + WIC_HDR_LEN;
    s->dsc.data_size     = need;
    return TRUE;
}

static APP_WICON_SLOT_T *prv_slot_for(int32_t code)
{
    APP_WICON_SLOT_T *free_slot = NULL;
    APP_WICON_SLOT_T *oldest    = NULL;

    for (int i = 0; i < WIC_CACHE_N; i++) {
        if (sg_slot[i].code == code) {
            return &sg_slot[i];
        }
        if (sg_slot[i].code < 0) {
            free_slot = &sg_slot[i];
        } else if (NULL == oldest || sg_slot[i].age < oldest->age) {
            oldest = &sg_slot[i];
        }
    }
    return (NULL != free_slot) ? free_slot : oldest;
}

/***********************************************************
************************* public ***************************
***********************************************************/

const lv_image_dsc_t *app_weather_icon(int32_t code)
{
    APP_WICON_SLOT_T *s;

    if (!sg_inited) {
        for (int i = 0; i < WIC_CACHE_N; i++) {
            sg_slot[i].code = -1;
        }
        sg_inited = TRUE;
    }

    if (code < 0) {
        code = APP_WEATHER_CODE_UNKNOWN;
    }

    s = prv_slot_for(code);
    if (NULL == s) {
        return NULL;
    }

    if (s->code != code) {
        if (!prv_load(s, code)) {
            /* 这一张没有就退到 99（未知）；99 也没有就放弃。 */
            if (code == APP_WEATHER_CODE_UNKNOWN ||
                !prv_load(s, APP_WEATHER_CODE_UNKNOWN)) {
                return NULL;
            }
        }
    }

    s->age = ++sg_age;
    return &s->dsc;
}

void app_weather_icon_flush(void)
{
    for (int i = 0; i < WIC_CACHE_N; i++) {
        prv_slot_free(&sg_slot[i]);
    }
    sg_inited = TRUE;
}
