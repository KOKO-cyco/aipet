/**
 * @file app_net.h
 * @brief 网络服务：链路状态、WiFi 连接、已保存凭据、云连接标志。
 *
 * 状态机
 * ------
 *   APP_NET_DOWN        未连接（或上一次连接尝试超时失败）
 *   APP_NET_CONNECTING  已下发凭据，20 秒内等链路起来
 *   APP_NET_UP          链路已就绪
 *
 * 变化时广播 APP_EVT_NET_STATUS（data 指向 int 形态的 APP_NET_STATUS_E）。
 * 连接是异步的：app_net_connect() 立刻返回，页面轮询 app_net_status() 或订阅事件
 * 来更新提示文字（主页的 WiFi 设置窗就是这么做的）。
 *
 * 凭据来自 SDK：链路建立成功时 netmgr 会把 ssid/pwd 写进 KV "netinfo"
 * （JSON {"s":..,"p":..}），app_net_get_saved() 负责解析出来给页面预填。
 */

#ifndef __APP_NET_H__
#define __APP_NET_H__

#include <stddef.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次 WiFi 连接尝试的超时时间（毫秒）。 */
#define APP_NET_CONNECT_TIMEOUT_MS   20000

typedef enum {
    APP_NET_DOWN = 0,
    APP_NET_CONNECTING,
    APP_NET_UP
} APP_NET_STATUS_E;

/**
 * @brief 初始化网络管理（netmgr + 配网方式），并启动 1 秒状态巡检定时器。
 *        必须在 tuya_iot_start() 之前调用。
 */
OPERATE_RET app_net_init(void);

/** @brief 链路是否可用。给 tuya_iot 的 network_check 回调用（只读，无副作用）。 */
BOOL_T app_net_is_up(void);

/** @brief 当前网络状态。 */
APP_NET_STATUS_E app_net_status(void);

/**
 * @brief 用明文凭据发起连接（异步）。凭据会走 netmgr 的 SSID_PSWD 通道，
 *        与 AP / BLE 配网完全同一条路径，所以两种配网方式可以共存。
 */
OPERATE_RET app_net_connect(const char *ssid, const char *pwd);

/** @brief 读出 SDK 上次保存的凭据（用于页面预填）。没有则输出空串。 */
OPERATE_RET app_net_get_saved(char *ssid, size_t ssid_sz, char *pwd, size_t pwd_sz);

/** @brief 标记云端(MQTT)连接状态（由用户主流程的事件回调调用）。 */
void app_net_set_cloud_up(BOOL_T up);

/** @brief 云端(MQTT)是否已连接。 */
BOOL_T app_net_cloud_up(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NET_H__ */
