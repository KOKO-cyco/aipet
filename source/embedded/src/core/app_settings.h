/**
 * @file app_settings.h
 * @brief 统一配置中心：所有需要掉电保存的标量/字符串都从这里读写。
 *
 * 为什么要有它
 * ------------
 * 以前音量散在 app_menu.c 和 app_ai_chat.c 各自 tal_kv_get/set，键名、范围、
 * 默认值各写一遍，新功能再加一个配置项就要再抄一遍。现在集中成一张描述表：
 * 每一项的音量键名、类型、默认值、取值范围都写在一处，调用者只说"哪个配置项"。
 *
 * 用法
 * ----
 *   int vol = app_settings_get_int(APP_SET_VOL_AI);
 *   app_settings_set_int(APP_SET_VOL_AI, 80);     // 改内存 + 广播事件，不写 flash
 *   app_settings_save(APP_SET_VOL_AI);            // 需要持久化时再写 flash
 *
 * 拖动滑条这类高频场景：value_changed 里 set_int()，released 里 save()，
 * 这样既实时生效又不会把 flash 写坏（一次交互只写一次）。
 *
 * 新增一个配置项
 * --------------
 * 1) 在 APP_SET_ID_E 末尾追加（必须在 APP_SET_COUNT 之前）；
 * 2) 在 app_settings.c 的 sg_desc[] 里补一行 {KV键名, 类型, 默认值, 最小, 最大, 容量}。
 * 就这两步，get/set/save/广播全部自动可用。
 */

#ifndef __APP_SETTINGS_H__
#define __APP_SETTINGS_H__

#include <stddef.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SET_VOL_AI = 0,    /* AI 音量：AI 语音 / TTS（KV 键仍叫 vol_sys，沿用老值） */
    APP_SET_BRIGHTNESS,    /* 背光亮度，10-100                             */
    APP_SET_TIMEZONE,      /* 时区偏移（小时），-12..14，默认 8（北京）      */
    APP_SET_CITY,          /* 天气城市名（留空则按 IP 定位）                 */
    APP_SET_WEATHER_KEY,   /* 天气接口密钥（心知）                          */
    APP_SET_CAM_TUNE,      /* 相机画面参数（"名字=值,名字=值"，空=出厂默认）  */

    APP_SET_COUNT
} APP_SET_ID_E;

/**
 * @brief 初始化：从 flash 读入所有项到内存缓存。必须在 app_settings_get_* 之前调用。
 *        缺失或长度不对的项回落到描述表里的默认值。
 */
OPERATE_RET app_settings_init(void);

/**
 * @brief 读一个整型配置（内存缓存，无 IO）。返回描述表里的默认值当且仅当初始化失败。
 */
int app_settings_get_int(APP_SET_ID_E id);

/**
 * @brief 改一个整型配置：夹到 [min, max]、写内存缓存，并广播 APP_EVT_SETTING_CHANGED。
 *        **不写 flash**，需要持久化请再调用 app_settings_save()。
 */
OPERATE_RET app_settings_set_int(APP_SET_ID_E id, int val);

/** @brief 一步到位：改内存 + 广播 + 写 flash（低频、确定要保存的场景用）。 */
OPERATE_RET app_settings_set_int_now(APP_SET_ID_E id, int val);

/** @brief 把内存里的值写回 flash。 */
OPERATE_RET app_settings_save(APP_SET_ID_E id);

/** @brief 读字符串配置（拷贝进调用者缓冲，永远以 '\0' 结尾）。 */
OPERATE_RET app_settings_get_str(APP_SET_ID_E id, char *out, size_t out_sz);

/**
 * @brief 写字符串配置：写内存缓存 + 广播事件 + 写 flash。超长会被截断。
 */
OPERATE_RET app_settings_set_str(APP_SET_ID_E id, const char *val);

/** @brief 该配置项的 flash 键名（调试打印用，只读，不要改）。 */
const char *app_settings_key(APP_SET_ID_E id);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SETTINGS_H__ */
