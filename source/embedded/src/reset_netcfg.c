/**
 * @file reset_netcfg.c
 * @brief Reset network configuration: power-cycle 3 times within 5 seconds
 *        and the device clears its WiFi pairing (re-provision with the app).
 *
 * Ported from the 3D_test/chat-bot projects (weixin-channel parts dropped).
 */

#include "tal_api.h"
#include "tal_sw_timer.h"
#include "tuya_iot.h"

#include "reset_netcfg.h"

#define RESET_NETCNT_NAME "rst_cnt"
#define RESET_NETCNT_MAX  3

static int reset_count_read(uint8_t *count)
{
    int rt;
    uint8_t *read_buf = NULL;
    size_t read_len;

    rt = tal_kv_get(RESET_NETCNT_NAME, &read_buf, &read_len);
    if (OPRT_OK != rt) {
        *count = 0;
        return rt;
    }

    *count = read_buf[0];
    tal_kv_free(read_buf);

    return rt;
}

static int reset_count_write(uint8_t count)
{
    return tal_kv_set(RESET_NETCNT_NAME, &count, 1);
}

static void reset_netconfig_timer(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    reset_count_write(0);
}

static OPERATE_RET __reset_netconfig_clear(void *data)
{
    (void)data;
    reset_count_write(0);
    return OPRT_OK;
}

int reset_netconfig_check(void)
{
    uint8_t rst_cnt = 0;

    reset_count_read(&rst_cnt);
    if (rst_cnt < RESET_NETCNT_MAX) {
        return OPRT_OK;
    }

    /* 先把计数清零，再触发复位。
     *
     * 不清零的话这里是个死锁：复位后 KV 里的计数仍是 >= 阈值，而这轮复位
     * 本身又发生在 5 秒计时器归零之前（check() 在启动末尾跑，约 1.7 秒），
     * 于是下一次开机 check() 又触发一次复位 …… 无限重启、无任何崩溃打印，
     * 现场只能靠擦掉 tuya_data 分区恢复（实测踩过）。 */
    reset_count_write(0);

    tal_event_subscribe(EVENT_RESET, "reset_netconfig", __reset_netconfig_clear,
                        SUBSCRIBE_TYPE_NORMAL);
    tuya_iot_reset(tuya_iot_client_get());

    return OPRT_OK;
}

int reset_netconfig_start(void)
{
    uint8_t rst_cnt = 0;
    TIMER_ID rst_config_timer = NULL;

    reset_count_read(&rst_cnt);
    reset_count_write(++rst_cnt);

    tal_sw_timer_create(reset_netconfig_timer, NULL, &rst_config_timer);
    tal_sw_timer_start(rst_config_timer, 5000, TAL_TIMER_ONCE);

    return OPRT_OK;
}
