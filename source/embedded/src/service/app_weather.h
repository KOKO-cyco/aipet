/**
 * @file app_weather.h
 * @brief 天气服务：连上 WiFi 后自动拉取 3 日天气，串口打印 + 暴露给 UI。
 *
 * 流程（详见 app_weather.c）：
 *   1) 订阅 APP_EVT_NET_STATUS，链路变 APP_NET_UP 时起一个独立线程
 *      （HTTP 是阻塞调用，不能在事件 / 定时器回调里直接发）。
 *   2) 逐个尝试 ipwho.is / ip-api / ipinfo 拿经纬度（失败则回退到心知天气的
 *      location=ip）；配置了城市（KV `city`，串口 `wcity`）则完全跳过定位。
 *   3) GET 心知天气 daily.json（days=3），解析后：
 *        · 串口打印（==== 分隔块）；
 *        · 写进**带锁的全局快照**；
 *        · app_storage_init() 后 publish APP_EVT_WEATHER_UPDATED。
 *
 * 必须在 app_net_init() 之后调用（要订阅网络事件）。
 *
 * 线程约定
 * --------
 * 取数据用 app_weather_get()：内部加锁拷一份快照，任意线程可调。
 * 但拿到的数组只管数据、不管 LVGL —— 刷界面必须在 LVGL 线程里做（主页的做法是
 * 事件回调只置脏位，常驻 lv_timer 消费）。**不要在事件回调里直接改 LVGL。**
 */

#ifndef __APP_WEATHER_H__
#define __APP_WEATHER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 展示天数（心知请求 days=3）。 */
#define APP_WEATHER_DAYS        3
#define APP_WEATHER_CITY_LEN    32
#define APP_WEATHER_TEXT_LEN    24
#define APP_WEATHER_NUM_LEN     8
#define APP_WEATHER_DATE_LEN    16
#define APP_WEATHER_DIR_LEN     16

/** 未知天气代码（心知用它表示"无数据/未知"，图标集里也有 99）。 */
#define APP_WEATHER_CODE_UNKNOWN   99

/** 一天的数据。字段全部 NUL 结尾；缺失时是空串，不是 NULL。 */
typedef struct {
    char    date[APP_WEATHER_DATE_LEN];       /* "2026-10-03"                  */
    int32_t code;                             /* 心知天气代码（白天），99=未知   */
    char    text_day[APP_WEATHER_TEXT_LEN];    /* 白天天气名，UTF-8             */
    char    text_night[APP_WEATHER_TEXT_LEN];  /* 夜间天气名，UTF-8             */
    char    high[APP_WEATHER_NUM_LEN];         /* 最高温（摄氏，字符串）         */
    char    low[APP_WEATHER_NUM_LEN];          /* 最低温                        */
    char    humidity[APP_WEATHER_NUM_LEN];     /* 相对湿度 %                    */
    char    wind_direction[APP_WEATHER_DIR_LEN]; /* 风向（中文）                */
    char    wind_scale[APP_WEATHER_NUM_LEN];   /* 风力等级                      */
    char    rainfall[APP_WEATHER_NUM_LEN];     /* 降水量 mm                     */
    char    wind_speed[APP_WEATHER_NUM_LEN];   /* 风速 km/h                     */
} app_weather_day_t;

/** 天气快照。 */
typedef struct {
    BOOL_T            valid;                  /* 是否已成功拉到过数据           */
    char              city[APP_WEATHER_CITY_LEN];
    int32_t           days;                   /* 有效天数 0..APP_WEATHER_DAYS   */
    app_weather_day_t day[APP_WEATHER_DAYS];
} app_weather_info_t;

/**
 * @brief 取一份天气快照（内部加锁拷贝，任意线程可调）。
 *
 * @param[out] out 必填；总是被完整填充（没数据时清零、days=0）。
 * @return TRUE 有有效数据（照 @a out 画界面）；FALSE 还没拉到 / 还没初始化。
 *
 * @note 这只是数据的拷贝，不涉及 LVGL。刷界面请回到 LVGL 线程做。
 */
BOOL_T app_weather_get(app_weather_info_t *out);

/**
 * @brief 注册天气服务（订阅网络状态事件，连上网后拉取并刷新快照）。幂等。
 * @return 恒为 OPRT_OK —— 天气是附加功能，它的任何问题都不该拦启动流程。
 */
OPERATE_RET app_weather_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEATHER_H__ */

