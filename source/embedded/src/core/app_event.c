/**
 * @file app_event.c
 * @brief 应用内轻量事件总线实现（订阅表 + 快照分发）。
 */

#include <string.h>

#include "tal_api.h"
#include "tal_mutex.h"

#include "app_event.h"

/* 订阅表容量。当前系统事件 + 页面订阅加起来远小于这个数；满了就报错而不是
 * 静默丢弃，方便发现泄漏（页面忘记退订）。 */
/* 订阅槽上限。
 *
 * **路由自身就吃掉 APP_EVT_MAX-1 个槽**（它订阅所有事件再按页面过滤，见
 * app_router_prepare），现在是 16 个；剩下的才是各模块的。2026-10-20 新 UI
 * 加了两路（状态栏 + 宠物页的 FONT_READY），24 个槽爆了 —— 后注册的
 * AI_STACK_UP / SETTING_CHANGED 直接订阅失败，AI 工具全哑。
 *
 * 48 = 16（路由）+ 32 给模块，留足余量。每槽 ~16 字节，不心疼。 */
#define APP_EVT_SUB_MAX     48
/* 一次发布最多同时投递给多少个订阅者（快照缓冲）。 */
#define APP_EVT_FANOUT_MAX  8

typedef struct {
    BOOL_T     used;
    APP_EVT_E  evt;
    APP_EVT_CB cb;
    void      *user;
} APP_EVT_SUB_T;

static APP_EVT_SUB_T sg_subs[APP_EVT_SUB_MAX];
static MUTEX_HANDLE  sg_lock = NULL;

OPERATE_RET app_event_init(void)
{
    if (NULL != sg_lock) {
        return OPRT_OK;                     /* 幂等 */
    }
    memset(sg_subs, 0, sizeof(sg_subs));
    return tal_mutex_create_init(&sg_lock);
}

OPERATE_RET app_event_subscribe(APP_EVT_E evt, APP_EVT_CB cb, void *user)
{
    OPERATE_RET rt = OPRT_OK;

    if (NULL == cb || evt <= APP_EVT_NONE || evt >= APP_EVT_MAX) {
        return OPRT_INVALID_PARM;
    }
    if (NULL == sg_lock) {
        return OPRT_COM_ERROR;
    }

    tal_mutex_lock(sg_lock);

    /* 同一个回调重复订阅同一事件：直接返回，避免页面重建时表被塞满 */
    for (uint32_t i = 0; i < APP_EVT_SUB_MAX; i++) {
        if (sg_subs[i].used && sg_subs[i].evt == evt && sg_subs[i].cb == cb) {
            sg_subs[i].user = user;
            goto out;
        }
    }
    for (uint32_t i = 0; i < APP_EVT_SUB_MAX; i++) {
        if (!sg_subs[i].used) {
            sg_subs[i].used = TRUE;
            sg_subs[i].evt  = evt;
            sg_subs[i].cb   = cb;
            sg_subs[i].user = user;
            goto out;
        }
    }
    PR_ERR("[evt] 订阅表已满(%d)，%d 号事件订阅失败", APP_EVT_SUB_MAX, (int)evt);
    rt = OPRT_COM_ERROR;

out:
    tal_mutex_unlock(sg_lock);
    return rt;
}

OPERATE_RET app_event_unsubscribe(APP_EVT_E evt, APP_EVT_CB cb)
{
    OPERATE_RET rt = OPRT_NOT_FOUND;

    if (NULL == sg_lock) {
        return OPRT_COM_ERROR;
    }

    tal_mutex_lock(sg_lock);
    for (uint32_t i = 0; i < APP_EVT_SUB_MAX; i++) {
        if (sg_subs[i].used && sg_subs[i].evt == evt && sg_subs[i].cb == cb) {
            sg_subs[i].used = FALSE;
            sg_subs[i].cb   = NULL;
            sg_subs[i].user = NULL;
            rt = OPRT_OK;
            break;
        }
    }
    tal_mutex_unlock(sg_lock);
    return rt;
}

void app_event_publish(APP_EVT_E evt, void *data)
{
    APP_EVT_CB cbs[APP_EVT_FANOUT_MAX];
    void      *users[APP_EVT_FANOUT_MAX];
    uint32_t   n = 0;

    if (NULL == sg_lock || evt <= APP_EVT_NONE || evt >= APP_EVT_MAX) {
        return;
    }

    /* 先在锁内做快照，再在锁外回调：订阅者可以安全地增删订阅。 */
    tal_mutex_lock(sg_lock);
    for (uint32_t i = 0; i < APP_EVT_SUB_MAX && n < APP_EVT_FANOUT_MAX; i++) {
        if (sg_subs[i].used && sg_subs[i].evt == evt) {
            cbs[n]   = sg_subs[i].cb;
            users[n] = sg_subs[i].user;
            n++;
        }
    }
    tal_mutex_unlock(sg_lock);

    for (uint32_t i = 0; i < n; i++) {
        cbs[i](evt, data, users[i]);
    }
}
