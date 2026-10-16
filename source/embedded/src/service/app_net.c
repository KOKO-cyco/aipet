/**
 * @file app_net.c
 * @brief 网络服务实现（netmgr 封装 + 状态巡检）。
 */

#include <string.h>

#include "tal_api.h"
#include "tal_kv.h"
#include "tal_sw_timer.h"
#include "tkl_wifi.h"
#include "netmgr.h"
#include "netconn_wifi.h"
#include "netcfg.h"
#include "cJSON.h"

#include "app_event.h"
#include "app_net.h"

static TIMER_ID            sg_poll_timer  = NULL;
static volatile int        sg_status      = APP_NET_DOWN;
static volatile uint64_t   sg_deadline_ms = 0;   /* 0 = 没有正在进行的尝试 */
static volatile BOOL_T     sg_cloud_up    = FALSE;

static APP_NET_STATUS_E prv_link_status(void)
{
    netmgr_status_e st = NETMGR_LINK_DOWN;

    if (OPRT_OK != netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &st)) {
        return APP_NET_DOWN;
    }
    return (NETMGR_LINK_DOWN == st) ? APP_NET_DOWN : APP_NET_UP;
}

/* 把新状态记下来，变化时广播。返回是否发生了变化。 */
static BOOL_T prv_status_update(APP_NET_STATUS_E st)
{
    int old = sg_status;
    int now = (int)st;

    if (old == now) {
        return FALSE;
    }
    sg_status = now;
    PR_NOTICE("[net] 状态 %d -> %d", old, now);
    /* 同步分发，栈变量在这次 publish 期间一直有效 */
    app_event_publish(APP_EVT_NET_STATUS, &now);
    return TRUE;
}

/* 1 秒巡检：链路状态 + 连接超时兜底 */
static void prv_poll_cb(TIMER_ID id, void *arg)
{
    APP_NET_STATUS_E st = prv_link_status();

    (void)id;
    (void)arg;

    if (APP_NET_UP == st) {
        sg_deadline_ms = 0;
        prv_status_update(APP_NET_UP);
        return;
    }

    /* 链路没起来：还在等就是 CONNECTING，超时就判失败 */
    if (0 != sg_deadline_ms) {
        if (tal_system_get_millisecond() < sg_deadline_ms) {
            prv_status_update(APP_NET_CONNECTING);
            return;
        }
        PR_WARN("[net] 连接超时");
        sg_deadline_ms = 0;
    }
    prv_status_update(APP_NET_DOWN);
}

OPERATE_RET app_net_init(void)
{
    OPERATE_RET rt;

    if (NULL != sg_poll_timer) {
        return OPRT_OK;
    }

    netmgr_init(NETCONN_WIFI);
    /* AP + BLE 双配网通道，与主页里的手动输入是同一个 netmgr */
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                    &(netcfg_args_t){ .type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP });

    rt = tal_sw_timer_create(prv_poll_cb, NULL, &sg_poll_timer);
    if (OPRT_OK != rt) {
        PR_ERR("[net] 巡检定时器创建失败: %d", rt);
        return rt;
    }
    tal_sw_timer_start(sg_poll_timer, 1000, TAL_TIMER_CYCLE);

    sg_status = prv_link_status();
    PR_NOTICE("[net] 服务就绪，当前状态 %d", (int)sg_status);
    return OPRT_OK;
}

BOOL_T app_net_is_up(void)
{
    return (APP_NET_UP == prv_link_status()) ? TRUE : FALSE;
}

APP_NET_STATUS_E app_net_status(void)
{
    return (APP_NET_STATUS_E)sg_status;
}

OPERATE_RET app_net_connect(const char *ssid, const char *pwd)
{
    netconn_wifi_info_t info;

    if (NULL == ssid || '\0' == ssid[0]) {
        return OPRT_INVALID_PARM;
    }

    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
    if (NULL != pwd) {
        strncpy(info.pswd, pwd, sizeof(info.pswd) - 1);
    }

    sg_deadline_ms = tal_system_get_millisecond() + APP_NET_CONNECT_TIMEOUT_MS;
    prv_status_update(APP_NET_CONNECTING);

    PR_NOTICE("[net] 连接 %s ...", info.ssid);
    return netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info);
}

OPERATE_RET app_net_get_saved(char *ssid, size_t ssid_sz, char *pwd, size_t pwd_sz)
{
    uint8_t *kv = NULL;
    size_t   kv_len = 0;

    if (NULL == ssid || 0 == ssid_sz || NULL == pwd || 0 == pwd_sz) {
        return OPRT_INVALID_PARM;
    }
    ssid[0] = '\0';
    pwd[0]  = '\0';

    /* KV "netinfo" = {"s":"<ssid>","p":"<pswd>"}，原始字节、无结尾 NUL */
    if (OPRT_OK != tal_kv_get("netinfo", &kv, &kv_len) || NULL == kv) {
        return OPRT_NOT_FOUND;
    }

    if (kv_len > 0 && kv_len < 128) {
        char   json[128];
        cJSON *root;

        memcpy(json, kv, kv_len);
        json[kv_len] = '\0';

        root = cJSON_Parse(json);
        if (NULL != root) {
            cJSON *js = cJSON_GetObjectItem(root, "s");
            cJSON *jp = cJSON_GetObjectItem(root, "p");

            if (NULL != js && NULL != js->valuestring) {
                snprintf(ssid, ssid_sz, "%s", js->valuestring);
            }
            if (NULL != jp && NULL != jp->valuestring) {
                snprintf(pwd, pwd_sz, "%s", jp->valuestring);
            }
            cJSON_Delete(root);
        }
    }
    tal_kv_free(kv);
    return OPRT_OK;
}

void app_net_set_cloud_up(BOOL_T up)
{
    BOOL_T old = sg_cloud_up;
    BOOL_T now = (TRUE == up) ? TRUE : FALSE;

    sg_cloud_up = now;
    if (old != now) {
        PR_NOTICE("[net] 云端连接 %s", now ? "已建立" : "断开");
        app_event_publish(APP_EVT_CLOUD_STATUS, &now);
    }
}

BOOL_T app_net_cloud_up(void)
{
    return sg_cloud_up;
}
