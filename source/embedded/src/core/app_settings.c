/**
 * @file app_settings.c
 * @brief 统一配置中心实现：一张描述表 + 内存缓存 + flash(KV) 读写。
 */

#include <string.h>
#include <stdint.h>

#include "tal_api.h"
#include "tal_kv.h"

#include "app_event.h"
#include "app_settings.h"

/* 字符串配置项的缓冲区长度（含结尾 '\0'）。描述表里的 cap 不能超过它。 */
#define APP_SET_STR_MAX     48

typedef enum {
    APP_SET_TYPE_INT = 0,
    APP_SET_TYPE_STR,
} APP_SET_TYPE_E;

typedef struct {
    const char     *kv_key;   /* flash 键名，改了会丢掉老值 */
    APP_SET_TYPE_E  type;
    const char     *def_str;  /* STR 默认值 */
    int32_t         def;      /* INT 默认值 */
    int32_t         min;
    int32_t         max;
} APP_SET_DESC_T;

/* 顺序必须与 APP_SET_ID_E 一一对应（编译期靠 _Static_assert 兜底）。 */
static const APP_SET_DESC_T sg_desc[APP_SET_COUNT] = {
    /* VOL_AI    */ { "vol_sys", APP_SET_TYPE_INT, NULL,  70,   0, 100 },
    /* BRIGHTNESS*/ { "bright",  APP_SET_TYPE_INT, NULL, 100,  10, 100 },
    /* TIMEZONE  */ { "tz",      APP_SET_TYPE_INT, NULL,   8, -12,  14 },
    /* CITY      */ { "city",    APP_SET_TYPE_STR, "",     0,   0,   0 },
    /* WEATHER...*/ { "wkey",    APP_SET_TYPE_STR, "",     0,   0,   0 },
};

/* 顺序必须与 APP_SET_ID_E 一一对应。这条 typedef 是编译期断言：
 * 表项数量和枚举对不上时直接报"数组大小为负"。 */
typedef char app_set_desc_count_check[
    (sizeof(sg_desc) / sizeof(sg_desc[0]) == (size_t)APP_SET_COUNT) ? 1 : -1];

static int32_t sg_int[APP_SET_COUNT];
static char    sg_str[APP_SET_COUNT][APP_SET_STR_MAX];
static BOOL_T  sg_inited = FALSE;

static BOOL_T prv_id_valid(APP_SET_ID_E id)
{
    return ((int)id >= 0 && (int)id < (int)APP_SET_COUNT) ? TRUE : FALSE;
}

static int32_t prv_clamp(APP_SET_ID_E id, int32_t v)
{
    if (v < sg_desc[(int)id].min) {
        return sg_desc[(int)id].min;
    }
    if (v > sg_desc[(int)id].max) {
        return sg_desc[(int)id].max;
    }
    return v;
}

/* 读一条 KV 原始字节。返回的 buf 需要 tal_kv_free()。 */
static OPERATE_RET prv_kv_read(const char *key, uint8_t **buf, size_t *len)
{
    *buf = NULL;
    *len = 0;
    return tal_kv_get(key, buf, len);
}

/* flash 里整型可能有两种长度：老固件写的 1 字节，或者本文件的 4 字节。
 * 两种都接受，写入统一用 4 字节 —— 这样升级固件不会丢掉用户已有的音量。 */
static int32_t prv_kv_read_int(const char *key, int32_t def)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    int32_t  v   = def;

    if (OPRT_OK != prv_kv_read(key, &buf, &len) || NULL == buf) {
        return def;
    }
    if (1 == len) {
        v = (int32_t)buf[0];
    } else if (len >= sizeof(int32_t)) {
        memcpy(&v, buf, sizeof(v));
    }
    tal_kv_free(buf);
    return v;
}

static void prv_kv_read_str(const char *key, const char *def, char *out, size_t out_sz)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    snprintf(out, out_sz, "%s", (NULL != def) ? def : "");
    if (OPRT_OK != prv_kv_read(key, &buf, &len) || NULL == buf || 0 == len) {
        if (NULL != buf) {
            tal_kv_free(buf);
        }
        return;
    }
    if (len > out_sz - 1) {
        len = out_sz - 1;
    }
    memcpy(out, buf, len);
    out[len] = '\0';
    tal_kv_free(buf);
}

OPERATE_RET app_settings_init(void)
{
    if (sg_inited) {
        return OPRT_OK;
    }

    for (int32_t i = 0; i < APP_SET_COUNT; i++) {
        if (APP_SET_TYPE_STR == sg_desc[i].type) {
            prv_kv_read_str(sg_desc[i].kv_key, sg_desc[i].def_str,
                            sg_str[i], sizeof(sg_str[i]));
            PR_DEBUG("[set] %s = \"%s\"", sg_desc[i].kv_key, sg_str[i]);
        } else {
            sg_int[i] = prv_clamp(i, prv_kv_read_int(sg_desc[i].kv_key, sg_desc[i].def));
            PR_DEBUG("[set] %s = %d", sg_desc[i].kv_key, (int)sg_int[i]);
        }
    }

    sg_inited = TRUE;
    PR_NOTICE("[set] 配置中心就绪（%d 项）", (int)APP_SET_COUNT);
    return OPRT_OK;
}

int app_settings_get_int(APP_SET_ID_E id)
{
    if (!prv_id_valid(id) || APP_SET_TYPE_INT != sg_desc[id].type) {
        return 0;
    }
    return (int)sg_int[id];
}

OPERATE_RET app_settings_set_int(APP_SET_ID_E id, int val)
{
    if (!prv_id_valid(id) || APP_SET_TYPE_INT != sg_desc[id].type) {
        return OPRT_INVALID_PARM;
    }

    sg_int[id] = prv_clamp(id, (int32_t)val);
    app_event_publish(APP_EVT_SETTING_CHANGED, (void *)(intptr_t)id);
    return OPRT_OK;
}

OPERATE_RET app_settings_save(APP_SET_ID_E id)
{
    if (!prv_id_valid(id)) {
        return OPRT_INVALID_PARM;
    }

    if (APP_SET_TYPE_STR == sg_desc[id].type) {
        uint32_t len = (uint32_t)strlen(sg_str[id]) + 1u;

        return tal_kv_set(sg_desc[id].kv_key, (const uint8_t *)sg_str[id], len);
    }
    return tal_kv_set(sg_desc[id].kv_key, (const uint8_t *)&sg_int[id], sizeof(sg_int[id]));
}

OPERATE_RET app_settings_set_int_now(APP_SET_ID_E id, int val)
{
    OPERATE_RET rt = app_settings_set_int(id, val);

    if (OPRT_OK != rt) {
        return rt;
    }
    return app_settings_save(id);
}

OPERATE_RET app_settings_get_str(APP_SET_ID_E id, char *out, size_t out_sz)
{
    if (!prv_id_valid(id) || NULL == out || 0 == out_sz) {
        return OPRT_INVALID_PARM;
    }
    if (APP_SET_TYPE_STR != sg_desc[id].type) {
        out[0] = '\0';
        return OPRT_INVALID_PARM;
    }
    snprintf(out, out_sz, "%s", sg_str[id]);
    return OPRT_OK;
}

OPERATE_RET app_settings_set_str(APP_SET_ID_E id, const char *val)
{
    if (!prv_id_valid(id) || NULL == val || APP_SET_TYPE_STR != sg_desc[id].type) {
        return OPRT_INVALID_PARM;
    }

    snprintf(sg_str[id], sizeof(sg_str[id]), "%s", val);
    app_event_publish(APP_EVT_SETTING_CHANGED, (void *)(intptr_t)id);
    return app_settings_save(id);
}

const char *app_settings_key(APP_SET_ID_E id)
{
    return prv_id_valid(id) ? sg_desc[id].kv_key : "?";
}
