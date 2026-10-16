/**
 * @file app_time.c
 * @brief 时间服务实现（UTC + 时区 -> 本地时间，1 秒事件）。
 */

#include <string.h>

#include "tal_api.h"
#include "tal_sw_timer.h"
#include "tal_time_service.h"

#include "app_event.h"
#include "app_settings.h"
#include "app_time.h"

/* 早于这个时刻（2020-01-01 UTC）一律视为"还没同步"：芯片没接电池时 RTC 从 0 开始。 */
#define TIME_VALID_EPOCH    1577836800u

static TIMER_ID sg_tick = NULL;
static BOOL_T   sg_valid = FALSE;
static int      sg_last_min = -1;
static int      sg_last_day = -1;

/* 有电池的 RTC 会一直走，但只有联网后云端下发的时间才可信 */
BOOL_T app_time_valid(void)
{
    return sg_valid;
}

OPERATE_RET app_time_now(APP_DATETIME_T *out)
{
    POSIX_TM_S tm;
    TIME_T     utc;
    TIME_T     local;

    if (NULL == out) {
        return OPRT_INVALID_PARM;
    }
    memset(out, 0, sizeof(*out));

    utc = tal_time_get_posix();
    if (NULL == tal_time_gmtime_r(&utc, &tm)) {
        return OPRT_COM_ERROR;
    }

    /* UTC -> 本地：偏移量来自配置中心，界面显示的永远是本地时间 */
    local = utc + (TIME_T)app_settings_get_int(APP_SET_TIMEZONE) * 3600;
    if (NULL == tal_time_gmtime_r(&local, &tm)) {
        return OPRT_COM_ERROR;
    }

    out->year  = tm.tm_year + 1900;
    out->month = tm.tm_mon + 1;
    out->day   = tm.tm_mday;
    out->hour  = tm.tm_hour;
    out->min   = tm.tm_min;
    out->sec   = tm.tm_sec;
    out->wday  = tm.tm_wday;
    return OPRT_OK;
}

int app_time_timezone(void)
{
    return app_settings_get_int(APP_SET_TIMEZONE);
}

/* 1 秒滴答：只在跨秒/跨分/跨日时广播，订阅者按需选粒度 */
static void prv_tick_cb(TIMER_ID id, void *arg)
{
    APP_DATETIME_T dt;
    BOOL_T         was_valid;

    (void)id;
    (void)arg;

    was_valid = sg_valid;
    sg_valid  = (tal_time_get_posix() > (TIME_T)TIME_VALID_EPOCH) ? TRUE : FALSE;

    if (!sg_valid) {
        return;
    }
    if (!was_valid) {
        PR_NOTICE("[time] 时间已同步");
        app_event_publish(APP_EVT_TIME_SYNCED, NULL);
    }

    if (OPRT_OK != app_time_now(&dt)) {
        return;
    }

    app_event_publish(APP_EVT_TIME_SECOND, NULL);

    if (dt.min != sg_last_min) {
        sg_last_min = dt.min;
        app_event_publish(APP_EVT_TIME_MINUTE, NULL);
    }
    if (dt.day != sg_last_day) {
        sg_last_day = dt.day;
        PR_NOTICE("[time] %04d-%02d-%02d", dt.year, dt.month, dt.day);
        app_event_publish(APP_EVT_TIME_DATE, NULL);
    }
}

OPERATE_RET app_time_init(void)
{
    OPERATE_RET rt;

    if (NULL != sg_tick) {
        return OPRT_OK;
    }

    sg_valid    = (tal_time_get_posix() > (TIME_T)TIME_VALID_EPOCH) ? TRUE : FALSE;
    sg_last_min = -1;
    sg_last_day = -1;

    rt = tal_sw_timer_create(prv_tick_cb, NULL, &sg_tick);
    if (OPRT_OK != rt) {
        PR_ERR("[time] 定时器创建失败: %d", rt);
        return rt;
    }
    tal_sw_timer_start(sg_tick, 1000, TAL_TIMER_CYCLE);

    PR_NOTICE("[time] 服务就绪 (时区 UTC%+d, 已同步=%d)",
              app_time_timezone(), (int)sg_valid);
    return OPRT_OK;
}

/* 极简 strftime 替代：只支持本工程用得到的几个占位符，行为完全确定 */
OPERATE_RET app_time_format(char *out, size_t out_sz, const char *fmt)
{
    static const char *const wday_cn[] = { "日", "一", "二", "三", "四", "五", "六" };
    APP_DATETIME_T dt;
    size_t         o = 0;

    if (NULL == out || 0 == out_sz) {
        return OPRT_INVALID_PARM;
    }
    out[0] = '\0';

    if (!sg_valid || OPRT_OK != app_time_now(&dt)) {
        snprintf(out, out_sz, "--");
        return OPRT_COM_ERROR;
    }
    if (NULL == fmt) {
        fmt = "%Y-%m-%d %H:%M:%S";
    }

    for (const char *p = fmt; '\0' != *p && o + 8 < out_sz; p++) {
        BOOL_T nopad = FALSE;

        if ('%' != *p) {
            out[o++] = *p;
            continue;
        }
        p++;
        /* "%-d" 这种 = 该字段不补零（strftime 的 GNU 扩展写法）。中文习惯写
         * "9月3日" 而不是 "09月03日"，所以需要它。 */
        if ('-' == *p) {
            nopad = TRUE;
            p++;
        }
        switch (*p) {
        case 'Y': o += (size_t)snprintf(out + o, out_sz - o, "%04d", dt.year);        break;
        case 'm': o += (size_t)snprintf(out + o, out_sz - o, nopad ? "%d" : "%02d",
                                        dt.month);                                    break;
        case 'd': o += (size_t)snprintf(out + o, out_sz - o, nopad ? "%d" : "%02d",
                                        dt.day);                                      break;
        case 'H': o += (size_t)snprintf(out + o, out_sz - o, nopad ? "%d" : "%02d",
                                        dt.hour);                                     break;
        case 'M': o += (size_t)snprintf(out + o, out_sz - o, nopad ? "%d" : "%02d",
                                        dt.min);                                      break;
        case 'S': o += (size_t)snprintf(out + o, out_sz - o, nopad ? "%d" : "%02d",
                                        dt.sec);                                      break;
        case 'w': o += (size_t)snprintf(out + o, out_sz - o, "%d", dt.wday);          break;
        case 'a': o += (size_t)snprintf(out + o, out_sz - o, "%s", wday_cn[dt.wday % 7]); break;
        case '%': out[o++] = '%';                                                     break;
        case '\0': p--;                                                               break;
        default:  out[o++] = '%'; out[o++] = *p;                                      break;
        }
    }
    out[o] = '\0';
    return OPRT_OK;
}
