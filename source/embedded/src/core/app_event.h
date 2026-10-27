/**
 * @file app_event.h
 * @brief 应用内轻量事件总线：模块之间只通过事件解耦，不互相 include。
 *
 * 设计要点
 * --------
 * - 同步分发：app_event_publish() 在**发布者线程**里依次调用订阅者。
 *   好处是零延迟、零额外线程；代价是订阅者不能阻塞、也不能做重活。
 * - 快照分发：发布前先把订阅表拷贝到栈上再逐个调用，所以订阅者可以在自己的
 *   回调里再次 app_event_subscribe()/publish()，不会破坏遍历。
 * - 线程约定：回调里**禁止直接操作 LVGL**（LVGL 只能在 LVGL 任务或持有显示锁
 *   时被访问）。需要刷 UI 时用 app_disp_lock()/unlock() 包住，或者只置一个标志
 *   位交给页面自己在 on_event 之后处理。
 *
 * 新增一个事件类型：在 APP_EVT_E 里追加一项，然后在发布方 app_event_publish()，
 * 订阅方按需 app_event_subscribe()。系统事件（时间/网络/配置）已经由服务层发布，
 * 业务代码一般只需要订阅。
 */

#ifndef __APP_EVENT_H__
#define __APP_EVENT_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** AI 文本流分片的类型。 */
typedef enum {
    APP_AI_TEXT_START = 0, /**< 一轮回复开始（可能已带第一片文字） */
    APP_AI_TEXT_DATA,      /**< 中间分片 */
    APP_AI_TEXT_STOP,      /**< 本轮最后一片，之后不会再有 */
    APP_AI_TEXT_ABORT,     /**< 被打断：丢弃尚未显示的分页 */
} APP_AI_TEXT_E;

/** AI 文本流分片。
 *
 * `timeindex` 是云端给整段回复打的时间戳，同一轮里单调不减（单位 ms）；
 * **只有相对差值有意义** —— 气泡按"本片 - 本轮首片"相对 TTS 起点排期，
 * 所以云端给绝对时间还是从 0 开始的偏移都能对齐。分片里的字符串只在
 * app_event_publish() 同步分发期间有效，订阅方必须拷贝。 */
typedef struct {
    const char   *data;
    uint16_t      len;
    uint32_t      timeindex;
    APP_AI_TEXT_E kind;
} APP_AI_TEXT_T;

/** TTS 播放时钟。气泡用 START 的本地时刻当"第 0 毫秒"。 */
typedef enum {
    APP_AI_TTS_START = 0,
    APP_AI_TTS_STOP,
    APP_AI_TTS_ABORT,
} APP_AI_TTS_E;

typedef enum {
    APP_EVT_NONE = 0,

    /* ---- 时间服务 app_time 发布 ---------------------------------------- */
    APP_EVT_TIME_SECOND,      /* 每秒一次，data = NULL                          */
    APP_EVT_TIME_MINUTE,      /* 分钟跳变，data = NULL                          */
    APP_EVT_TIME_DATE,        /* 日期跳变，data = NULL                          */
    APP_EVT_TIME_SYNCED,      /* 首次拿到有效时间（云端同步成功），data = NULL    */

    /* ---- 网络服务 app_net 发布 ----------------------------------------- */
    APP_EVT_NET_STATUS,       /* data = (int *)APP_NET_STATUS_E                 */
    APP_EVT_CLOUD_STATUS,     /* 云端(MQTT)连接状态，data = (bool *)             */

    /* ---- 配置中心 app_settings 发布 ------------------------------------ */
    APP_EVT_SETTING_CHANGED,  /* data = (intptr_t)APP_SET_ID_E                  */

    /* ---- 音频服务 app_audio 发布 --------------------------------------- */
    APP_EVT_VOLUME_COMMITTED, /* 音量拖完松手（已落盘），data = (intptr_t)APP_AUDIO_CH_E
                               * 订阅方（云端上报）只在松手时发一次，拖动过程不发 */

    /* ---- AI 对话模块 app_ai_chat 发布 ---------------------------------- */
    APP_EVT_AI_STATE,         /* data = (intptr_t)AI_MODE_STATE_*               */
    APP_EVT_AI_EMOTION,       /* data = const char *（情绪名，大写，UTF-8）      */
    APP_EVT_AI_STACK_UP,      /* 语音栈拉起成功，data = NULL                    */
    APP_EVT_AI_TEXT,          /* data = const APP_AI_TEXT_T *（NLG 文本流分片） */
    APP_EVT_AI_TTS,           /* data = (intptr_t)APP_AI_TTS_E（播放时钟）      */

    /* ---- 对话驱动动作：MCP 工具模块 app_avatar_tools 发布 ----------------
     * data = const char *（精灵动作名，与 app_sprite2d_clip_name() 一致）。
     * 订阅方是桌宠页（page_pet.c）—— 它按自己的状态机决定什么时候插播、
     * 播完回到哪一组。
     *
     * **模块之间不互相 include**（见 ARCHITECTURE.md）：所以工具模块不直接调
     * app_sprite2d_play()，而是发这个事件；"动作策略"全留在页面里。 */
    APP_EVT_PET_ACTION,       /* data = const char *（动作名）                   */

    /* ---- 天气服务 app_weather 发布 ------------------------------------- */
    APP_EVT_WEATHER_UPDATED,  /* 拉到/刷新天气，快照已就绪，data = NULL          */
                              /* 订阅方（状态栏）只置脏位，刷新交给自己的 lv_timer */

    /* ---- 字体服务 app_font 发布 ---------------------------------------- */
    APP_EVT_FONT_READY,       /* SD 字库（puhui 六档）换进代理字体，data = NULL  */
                              /* 字号变了，已经量过尺寸的标签要重排 —— 订阅方    */
                              /* （桌宠页）只置位，在 LVGL 线程里重建当前页      */

    /* ---- 页面路由 app_router 发布 -------------------------------------- */
    APP_EVT_PAGE_CHANGED,     /* 进入了新页面，data = (APP_PAGE_EVT_T *)         */
                              /* 订阅方据此决定"这一页要不要干活" —— 只看       */
                              /* p->overlay，**不认识任何页面名**               */

    /* ---- 存储服务 app_storage 发布 ------------------------------------- */
    APP_EVT_SD_STATUS,        /* SD 卡在位状态变了，data = (intptr_t)1 插入 / 0 拔出 */
                              /* 由 CD 引脚（P8）轮询去抖后发布，见 app_storage.h  */
                              /* 注意：只在**变化**时发一次，不是周期性广播          */

    APP_EVT_MAX
} APP_EVT_E;

/**
 * @brief APP_EVT_PAGE_CHANGED 的负载（栈上，只在回调期间有效，别留存指针）。
 *
 * `overlay` 由**页面自己**在 `APP_PAGE_T.flags` 里声明（`APP_PAGE_NO_OVERLAY`），
 * 路由只负责转发。这样浮层 / 唤醒检测这些订阅方**零页面知识** —— 加一个新页面
 * 只要它自己声明，不用回头改订阅方。
 */
typedef struct {
    const char *page;       /* 新页面的名字（静态生存期）                     */
    BOOL_T      overlay;    /* 这一页要不要画浮层精灵                         */
} APP_PAGE_EVT_T;

/** 事件回调。@param user 订阅时传入的上下文。 */
typedef void (*APP_EVT_CB)(APP_EVT_E evt, void *data, void *user);

/**
 * @brief 初始化事件总线（幂等）。必须在任何 subscribe/publish 之前调用。
 */
OPERATE_RET app_event_init(void);

/**
 * @brief 订阅一个事件类型。同一个回调可以对多个事件类型分别订阅。
 * @return OPRT_OK；订阅表满时返回 OPRT_COM_ERROR。
 */
OPERATE_RET app_event_subscribe(APP_EVT_E evt, APP_EVT_CB cb, void *user);

/** @brief 退订（按 事件类型 + 回调 精确匹配）。 */
OPERATE_RET app_event_unsubscribe(APP_EVT_E evt, APP_EVT_CB cb);

/**
 * @brief 发布事件，同步调用所有订阅者。可在任意线程调用。
 * @param[in] data 事件负载，语义见 APP_EVT_E 各项注释；可为 NULL。
 */
void app_event_publish(APP_EVT_E evt, void *data);

#ifdef __cplusplus
}
#endif

#endif /* __APP_EVENT_H__ */
