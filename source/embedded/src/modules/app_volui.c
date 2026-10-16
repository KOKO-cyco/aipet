/**
 * @file app_volui.c
 * @brief 音量控件实现。设计说明见 app_volui.h。
 *
 * 三个状态标志是这里唯一容易搞错的地方，先说清楚：
 *
 *   sg_updating  程序化 set_value 期间置位。lv_slider_set_value() 会触发
 *                VALUE_CHANGED -> tune_volume -> publish -> 我们的事件回调
 *                -> 又一次 set_value —— 没有它就会来回递归。
 *   dragging     用户手指还按在这一条上。此时不接外部更新（云端下发 / 别的页面
 *                改了同一路），否则手指会被拽走。
 *   删除         控件被删时靠 LV_EVENT_DELETE 注销（LVGL 递归删子对象时每个
 *                对象都会收到这个事件），调用方不用管。
 */

#include <string.h>

#include "tal_api.h"

#include "lvgl.h"

#include "app_audio.h"
#include "app_event.h"
#include "app_settings.h"
#include "app_theme.h"
#include "app_volui.h"

/* 同时在世的控件上限：主页窗 4 + 音乐页 1 + 游戏菜单 1，留一倍余量。 */
#define VOLUI_ITEM_MAX      8
/* 外部改了音量之后，最多隔这么久把界面刷过去。云端下发是秒级的事，200ms
 * 完全够"看着是实时的"，又不至于让 LVGL 线程被轮询占住。 */
#define VOLUI_POLL_MS       200

typedef struct {
    BOOL_T         used;
    lv_obj_t      *sl;
    lv_obj_t      *lbl;
    APP_AUDIO_CH_E ch;
    BOOL_T         dragging;
} VOLUI_ITEM_T;

static VOLUI_ITEM_T sg_items[VOLUI_ITEM_MAX];
static BOOL_T       sg_updating = FALSE;
/* 哪几路的界面待刷新（bit = APP_AUDIO_CH_E）。只被事件回调置位（可能在 yield
 * 线程），只被 LVGL 定时器清 —— 两边都不碰对方的链表，所以跨线程是安全的。 */
static volatile uint32_t sg_dirty = 0;

/***********************************************************
************************* internal *************************
***********************************************************/
static const char *prv_name(APP_AUDIO_CH_E ch)
{
    (void)ch;
    return "AI音量";        /* 现在只剩这一路，见 app_audio.h */
}

/* 配置项 -> 通道。不是音量项就返回 APP_AUDIO_CH_COUNT。 */
static APP_AUDIO_CH_E prv_ch_of(APP_SET_ID_E id)
{
    return (APP_SET_VOL_AI == id) ? APP_AUDIO_CH_AI : APP_AUDIO_CH_COUNT;
}

static VOLUI_ITEM_T *prv_find(lv_obj_t *sl)
{
    uint32_t i;

    for (i = 0; i < VOLUI_ITEM_MAX; i++) {
        if (sg_items[i].used && sl == sg_items[i].sl) {
            return &sg_items[i];
        }
    }
    return NULL;
}

/* 把某一路的当前值刷到所有控件上。必须在 LVGL 线程调用。 */
static void prv_sync_ch(APP_AUDIO_CH_E ch)
{
    int      v = app_audio_get_volume(ch);
    uint32_t i;

    for (i = 0; i < VOLUI_ITEM_MAX; i++) {
        VOLUI_ITEM_T *it = &sg_items[i];

        if (!it->used || it->ch != ch) {
            continue;
        }
        /* 手指正按在这条上：别抢。用户此刻的输入就是权威。 */
        if (it->dragging) {
            continue;
        }
        if (lv_slider_get_value(it->sl) != v) {
            sg_updating = TRUE;
            lv_slider_set_value(it->sl, v, LV_ANIM_OFF);
            sg_updating = FALSE;
        }
        if (NULL != it->lbl) {
            lv_label_set_text_fmt(it->lbl, "%d", v);
        }
    }
}

/* LVGL 定时器：把待刷新的几路刷到界面上。跑在 LVGL 线程，可以随便碰控件。 */
static void prv_poll_cb(lv_timer_t *t)
{
    uint32_t m = sg_dirty;
    uint32_t o;

    (void)t;

    if (0 == m) {
        return;
    }
    sg_dirty = 0;
    for (o = 0; o < (uint32_t)APP_AUDIO_CH_COUNT; o++) {
        if (m & (1u << o)) {
            prv_sync_ch((APP_AUDIO_CH_E)o);
        }
    }
}

/* 音量配置变了。发布者可能是任意线程（云端 DP 下发就在 yield 线程），所以这里
 * 只置一个脏位 —— LVGL 只能在 LVGL 线程碰，而 LVGL 任务跑 lv_task_handler() 时
 * 本身持有显示锁，在事件回调里 app_disp_lock() 会把自己锁死。
 *
 * 不用 lv_async_call()：本 SDK 的 LVGL 里它是"每次建一个 timer"的实现，
 * lv_timer_create() 不加锁，跨线程调会踩坏定时器链表。 */
static void prv_evt_cb(APP_EVT_E evt, void *data, void *user)
{
    APP_AUDIO_CH_E ch;

    (void)user;

    if (APP_EVT_SETTING_CHANGED != evt) {
        return;
    }
    ch = prv_ch_of((APP_SET_ID_E)(intptr_t)data);
    if (ch >= APP_AUDIO_CH_COUNT) {
        return;
    }
    sg_dirty |= (1u << (uint32_t)ch);
}

/* 拖动中：改内存 + 广播，不写 flash。
 * 不做声音试听：嘀声走扬声器那路软件增益（游戏音量），跟所调的 AI/电影音量
 * 对不上，反而造成"音量没匹配好"的误会 —— 拖动反馈就靠数值和滑条本身。 */
static void prv_val_cb(lv_event_t *e)
{
    VOLUI_ITEM_T *it = prv_find(lv_event_get_target(e));
    int           v;

    if (NULL == it || sg_updating) {
        return;
    }
    v = lv_slider_get_value(it->sl);
    app_audio_tune_volume(it->ch, v);
    if (NULL != it->lbl) {
        lv_label_set_text_fmt(it->lbl, "%d", v);
    }
}

static void prv_press_cb(lv_event_t *e)
{
    VOLUI_ITEM_T *it = prv_find(lv_event_get_target(e));

    if (NULL != it) {
        it->dragging = TRUE;
    }
}

/* 松手：一次交互只落盘一次，避免写坏 flash */
static void prv_release_cb(lv_event_t *e)
{
    VOLUI_ITEM_T *it = prv_find(lv_event_get_target(e));

    if (NULL == it || sg_updating) {
        return;
    }
    it->dragging = FALSE;
    app_audio_commit_volume(it->ch);
}

static void prv_delete_cb(lv_event_t *e)
{
    VOLUI_ITEM_T *it = prv_find(lv_event_get_target(e));

    if (NULL != it) {
        memset(it, 0, sizeof(*it));
    }
}

/* 挂上回调 + 登记。订阅事件放在这里（幂等），调用方不需要初始化本模块。 */
static void prv_attach(lv_obj_t *sl, lv_obj_t *lbl, APP_AUDIO_CH_E ch)
{
    static BOOL_T s_subed = FALSE;
    uint32_t      i;

    for (i = 0; i < VOLUI_ITEM_MAX; i++) {
        if (!sg_items[i].used) {
            break;
        }
    }
    if (i >= VOLUI_ITEM_MAX) {
        PR_WARN("[volui] 控件太多（>%d），这一个不会被同步", VOLUI_ITEM_MAX - 1);
        return;
    }

    sg_items[i].used     = TRUE;
    sg_items[i].sl       = sl;
    sg_items[i].lbl      = lbl;
    sg_items[i].ch       = ch;
    sg_items[i].dragging = FALSE;

    lv_obj_add_event_cb(sl, prv_val_cb,     LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, prv_press_cb,   LV_EVENT_PRESSED,       NULL);
    lv_obj_add_event_cb(sl, prv_release_cb, LV_EVENT_RELEASED,      NULL);
    lv_obj_add_event_cb(sl, prv_delete_cb,  LV_EVENT_DELETE,        NULL);

    if (!s_subed) {
        s_subed = TRUE;
        app_event_subscribe(APP_EVT_SETTING_CHANGED, prv_evt_cb, NULL);
        /* 定时器必须在持锁（或 LVGL 线程）里建：页面建 UI 时正好满足 */
        if (NULL == lv_timer_create(prv_poll_cb, VOLUI_POLL_MS, NULL)) {
            PR_WARN("[volui] 同步定时器没建起来，界面不会自动跟随外部改动");
        }
    }
}

/***********************************************************
************************* public api ***********************
***********************************************************/
lv_obj_t *app_volui_row(lv_obj_t *parent, APP_AUDIO_CH_E ch, int y)
{
    int       v = app_audio_get_volume(ch);
    lv_obj_t *l;
    lv_obj_t *val;
    lv_obj_t *sl;

    l = app_ui_label(parent, prv_name(ch), UI_FONT_CN, UI_COL_TEXT);
    lv_obj_set_pos(l, 12, y);

    val = app_ui_label(parent, "0", UI_FONT_21, UI_COL_TEXT);
    lv_obj_set_pos(val, 86, y - 3);
    lv_label_set_text_fmt(val, "%d", v);

    sl = lv_slider_create(parent);
    lv_obj_set_width(sl, 180);
    lv_obj_set_pos(sl, 14, y + 32);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, v, LV_ANIM_OFF);

    prv_attach(sl, val, ch);
    return sl;
}

lv_obj_t *app_volui_bar(lv_obj_t *parent, APP_AUDIO_CH_E ch, const char *name,
                        int x, int y, int w)
{
    int       v = app_audio_get_volume(ch);
    lv_obj_t *sl;
    lv_obj_t *val;

    if (NULL != name) {
        lv_obj_t *l = app_ui_label(parent, name, UI_FONT_CN, UI_COL_TEXT);

        lv_obj_set_pos(l, x, y - 30);
    }

    sl = lv_slider_create(parent);
    lv_obj_set_pos(sl, x, y);
    lv_obj_set_width(sl, w);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, v, LV_ANIM_OFF);

    val = app_ui_label(parent, "--", UI_FONT_21, UI_COL_TEXT);
    lv_obj_set_pos(val, x + w + 16, y - 8);
    lv_label_set_text_fmt(val, "%d", v);

    prv_attach(sl, val, ch);
    return sl;
}
