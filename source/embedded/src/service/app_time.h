/**
 * @file app_time.h
 * @brief 时间服务：本地时间、格式化、秒/分钟/日期事件。
 *
 * 时区
 * ----
 * 芯片 RTC / 云端下发的都是 UTC，界面上要显示的是本地时间。时区偏移取自配置中心
 * （APP_SET_TIMEZONE，小时，默认 8 = 北京），换算在 app_time_now() 里一次做完，
 * 所以调用者拿到的永远是本地时间，不用自己算。
 *
 * 事件
 * ----
 * 服务内部有一个 1 秒周期定时器，会广播：
 *   APP_EVT_TIME_SECOND   每秒
 *   APP_EVT_TIME_MINUTE   分钟跳变（时钟刷新用这个，不用每秒刷）
 *   APP_EVT_TIME_DATE     日期跳变
 *   APP_EVT_TIME_SYNCED   首次拿到有效时间（联网后云端下发）
 *
 * @warning 事件在**定时器线程**里分发。回调里要改 LVGL 时必须用
 *          app_disp_lock()/unlock() 包住，不能直接裸操作。
 */

#ifndef __APP_TIME_H__
#define __APP_TIME_H__

#include <stddef.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int year;    /* 完整年份，如 2026      */
    int month;   /* 1-12                  */
    int day;     /* 1-31                  */
    int hour;    /* 0-23（本地时间）        */
    int min;     /* 0-59                  */
    int sec;     /* 0-59                  */
    int wday;    /* 0=周日 .. 6=周六       */
} APP_DATETIME_T;

/** @brief 启动时间服务（1 秒巡检 + 事件广播）。需在 tal_time_service_init() 之后调用。 */
OPERATE_RET app_time_init(void);

/** @brief 是否已拿到有效时间（联网前是 FALSE，此时界面应显示 "--:--"）。 */
BOOL_T app_time_valid(void);

/** @brief 取当前本地时间。@return OPRT_OK 成功。 */
OPERATE_RET app_time_now(APP_DATETIME_T *out);

/**
 * @brief 按格式串生成字符串。支持 %Y %m %d %H %M %S %w %a（中文星期）%% 。
 *        字段前加 '-' 表示该字段不补零（如 %-m → "9" 而不是 "09"）。
 *
 * 例：app_time_format(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S")
 *     app_time_format(buf, sizeof(buf), "%Y年%-m月%-d日 星期%a %-H:%M")
 *       → "2026年9月3日 星期一 8:04"
 *
 * 非 '%' 的字符原样透传，所以中文年月日直接写在格式串里即可。
 * 注意缓冲区要留够：中文日期串约 30 字节，别用 16 字节的小 buf。
 *
 * 不用 strftime()：裸机 newlib 里它的 locale / 时区依赖不可靠，这里自己实现，
 * 行为完全确定。
 *
 * @return OPRT_OK；时间无效时 out 为 "--"。
 */
OPERATE_RET app_time_format(char *out, size_t out_sz, const char *fmt);

/** @brief 当前时区偏移（小时）。 */
int app_time_timezone(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_TIME_H__ */
