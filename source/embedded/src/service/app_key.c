/**
 * @file app_key.c
 * @brief 按键服务实现（消抖 + 长短按 + 边沿事件队列）。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_gpio.h"

#include "app_key.h"

/* 每个按键一个 4 深度的小 FIFO：够覆盖"按下 + 短按 + 松开"这种连发，
 * 又不会让迟钝的消费者堆积过期事件。 */
#define APP_KEY_QUEUE_LEN   4

typedef struct {
    BOOL_T         used;
    TUYA_GPIO_NUM_E gpio;
    uint8_t        low_cnt;      /* 连续低电平采样数（消抖） */
    BOOL_T         down;         /* 已确认按下 */
    BOOL_T         fired;        /* 本次按下已经报过 SHORT/LONG */
    uint64_t       down_ms;      /* 按下时刻，算长按用 */
    uint8_t        q[APP_KEY_QUEUE_LEN];
    uint8_t        q_head, q_tail;
} APP_KEY_ST_T;

static APP_KEY_ST_T sg_keys[APP_KEY_COUNT] = {
    /* BACK: 板级 P12，按下短路到 GND（内部上拉） */
    [APP_KEY_BACK] = { .used = TRUE, .gpio = TUYA_GPIO_NUM_12 },
};

static BOOL_T sg_inited = FALSE;

static void prv_push(APP_KEY_ST_T *st, APP_KEY_EVT_E evt)
{
    uint8_t next = (uint8_t)((st->q_tail + 1u) % APP_KEY_QUEUE_LEN);

    if (next == st->q_head) {
        /* 队列满 = 消费者太慢。长按事件被丢掉的结果就是"长按没反应"，所以这条
         * 必须喊出来，别静默丢。 */
        PR_WARN("[key] 事件队列满，丢弃 evt=%d", (int)evt);
        return;
    }
    /* 只记边沿（DOWN / SHORT / LONG），UP 太吵不记。
     * 有这条线索，"长按没反应"就能一眼分清是：
     *   没有 DOWN      -> GPIO 根本没看到按下（引脚配置被别的模块改了）
     *   有 DOWN 没 LONG-> 中间被判成松手了（引脚电平抖）
     *   有 DOWN+LONG 却没退 -> 问题不在按键，在页面的退出路径 */
    if (APP_KEY_EVT_UP != evt) {
        PR_DEBUG("[key] gpio%d evt=%d", (int)st->gpio, (int)evt);
    }
    st->q[st->q_tail] = (uint8_t)evt;
    st->q_tail = next;
}

OPERATE_RET app_key_init(void)
{
    if (sg_inited) {
        return OPRT_OK;
    }

    for (int32_t i = 0; i < APP_KEY_COUNT; i++) {
        TUYA_GPIO_BASE_CFG_T gc = {0};

        if (!sg_keys[i].used) {
            continue;
        }
        gc.direct = TUYA_GPIO_INPUT;
        gc.mode   = TUYA_GPIO_PULLUP;       /* 按键另一端接 GND */
        tkl_gpio_init(sg_keys[i].gpio, &gc);
    }

    sg_inited = TRUE;
    PR_NOTICE("[key] 按键服务就绪（%d 个）", (int)APP_KEY_COUNT);
    return OPRT_OK;
}

void app_key_poll(void)
{
    uint64_t now = tal_system_get_millisecond();

    if (!sg_inited) {
        return;
    }

    for (int32_t i = 0; i < APP_KEY_COUNT; i++) {
        APP_KEY_ST_T *st = &sg_keys[i];
        TUYA_GPIO_LEVEL_E lv = TUYA_GPIO_LEVEL_HIGH;

        if (!st->used) {
            continue;
        }
        tkl_gpio_read(st->gpio, &lv);

        if (TUYA_GPIO_LEVEL_LOW == lv) {
            if (0 == st->low_cnt) {
                st->down_ms = now;
            }
            if (st->low_cnt < APP_KEY_DEBOUNCE) {
                st->low_cnt++;
            }
            if (st->low_cnt >= APP_KEY_DEBOUNCE) {
                if (!st->down) {
                    st->down = TRUE;
                    prv_push(st, APP_KEY_EVT_DOWN);
                }
                /* 长按在阈值那一刻报一次，之后按住不再重复 */
                if (!st->fired && (now - st->down_ms) >= APP_KEY_HOLD_MS) {
                    st->fired = TRUE;
                    prv_push(st, APP_KEY_EVT_LONG);
                }
            }
        } else {
            if (st->down) {
                if (!st->fired) {
                    prv_push(st, APP_KEY_EVT_SHORT);
                }
                prv_push(st, APP_KEY_EVT_UP);
                st->down = FALSE;
            }
            /* 松手才复位，天然实现"必须松开才能再次触发" */
            st->low_cnt = 0;
            st->fired   = FALSE;
        }
    }
}

APP_KEY_EVT_E app_key_take(APP_KEY_E key)
{
    APP_KEY_ST_T *st;

    if ((int)key < 0 || (int)key >= (int)APP_KEY_COUNT) {
        return APP_KEY_EVT_NONE;
    }
    st = &sg_keys[key];
    if (st->q_head == st->q_tail) {
        return APP_KEY_EVT_NONE;
    }

    uint8_t evt = st->q[st->q_head];
    st->q_head = (uint8_t)((st->q_head + 1u) % APP_KEY_QUEUE_LEN);
    return (APP_KEY_EVT_E)evt;
}

BOOL_T app_key_is_down(APP_KEY_E key)
{
    if (key < 0 || key >= APP_KEY_COUNT) {
        return FALSE;
    }
    return sg_keys[key].down;
}
