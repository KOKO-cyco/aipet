/**
 * @file page_nosd.c
 * @brief SD 状态页：卡不在 / 正在识别 / 卡损坏，三句话一页。
 *
 * 三种状态对应三种来路：
 *
 *   请插入 SD 卡      卡不在位（CD 引脚判出来）→ 路由推本页（prv_sd_cb）
 *   正在识别存储卡…   卡刚插回来、或刚开机，目录索引还在预热（app_storage_warming）
 *   SD 卡损坏        卡在位但链路重建也救不回来 → 读者报 app_router_sd_damaged()
 *
 * 文案在 on_enter 里现取 `app_storage_*()` —— 推送是异步的，页面建立时才知道那
 * 一刻的真实状态，比在推送方传参更准。
 *
 * 「正在识别」是**自动撤掉**的：预热跑完（三个列表都扫好）本页自己的定时器把栈
 * 收回首页。这条路径很关键 —— 预热没完就放用户进电影页，on_enter 要现场扫 3 秒
 * 目录，LVGL 又没挂起，用户看到的就是"点了没反应"。挡在这里、等扫完再放行，
 * 进去时缓存是热的、秒开（见 app_router.c 的 prv_enter）。
 *
 * **返回键 / 返回按钮一律回首页**，不是"弹掉一层"：
 *   - 卡不在时栈底往往是电影目录 / 音乐列表（同样需要卡），弹回去只是对着一个
 *     读不出东西的页面发呆；
 *   - 卡损坏时栈是空的（路由已经收到首页了），弹一层就是回首页，两种情形一致。
 *   回首页 = 一切从头来，和"每次重进都重新加载 SD 卡"是同一套语义。
 *
 * 画面：**全黑底 + 文案 + 返回键**。开机 logo（白底 + 居中图）是另一回事，
 * 见 app_splash.h —— 那个只在开机时出现，本页是运行期插拔卡时才弹的。
 *
 * 版式刻意**避开右下角**：浮层小精灵（约 113x170）钉在那儿，压上去会挡住按钮、
 * 还会抢走触摸。所以按钮不居中，靠左放。
 *
 * @note **不声明 APP_PAGE_NO_OVERLAY**：浮层小精灵在这一页照样显示（它常驻的
 *       idle 组在 PSRAM 里，不依赖 SD）—— 用户要的就是"掉线也一直有角色在"。
 *       所以图和文案都靠上放，别跟右下角的角色打架。
 */

#include "tal_api.h"

#include "lvgl.h"

#include "app_disp.h"
#include "app_pages.h"
#include "app_router.h"
#include "app_storage.h"
#include "app_theme.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* 状态轮询周期。预热是秒级的，200ms 足够跟上；再密只是白刷。 */
#define NOSD_POLL_MS    200

/***********************************************************
***********************variable define**********************
***********************************************************/
static lv_obj_t   *sg_scr   = NULL;
static lv_obj_t   *sg_msg   = NULL;     /* 主文案（会随状态变） */
static lv_obj_t   *sg_hint  = NULL;     /* 副文案（只有"损坏"才有） */
static lv_timer_t *sg_timer = NULL;
static int32_t     sg_shown = -1;       /* 上次画的是哪种状态 */

typedef enum {
    NOSD_ST_ABSENT = 0,     /* 请插入 SD 卡 */
    NOSD_ST_WARMING,        /* 正在识别存储卡… */
    NOSD_ST_DAMAGED,        /* SD 卡损坏 */
} NOSD_ST_E;

/***********************************************************
***********************function define**********************
***********************************************************/
/* 当前该显示哪一种。**顺序不能反**：预热中就算链路是好的也先说"正在识别"，
 * 否则用户会看到"请插入 SD 卡"（卡明明插着）。 */
static NOSD_ST_E prv_state(void)
{
    if (app_storage_warming()) {
        return NOSD_ST_WARMING;
    }
    if (APP_SD_DAMAGED == app_storage_fault()) {
        return NOSD_ST_DAMAGED;
    }
    return NOSD_ST_ABSENT;
}

static const char *prv_msg(NOSD_ST_E st)
{
    switch (st) {
    case NOSD_ST_WARMING: return "正在识别存储卡…";
    case NOSD_ST_DAMAGED: return "SD 卡损坏";
    default:              return "请插入 SD 卡";
    }
}

/* 返回：**右上角那个 ✕**，和主页 WiFi 弹窗 / 音乐页同一套控件（app_ui_close_btn）。
 * 一律回首页（见文件头）。 */
static void prv_close_cb(lv_event_t *e)
{
    (void)e;
    (void)app_router_go_home();
}

static void prv_nosd_key(APP_KEY_E key, APP_KEY_EVT_E evt)
{
    if (APP_KEY_BACK == key && APP_KEY_EVT_DOWN == evt) {
        (void)app_router_go_home();
    }
}

/* 按状态刷新文案。返回 TRUE 表示状态变了。 */
static BOOL_T prv_apply_state(NOSD_ST_E st)
{
    if (st == (NOSD_ST_E)sg_shown) {
        return FALSE;
    }
    sg_shown = (int32_t)st;

    if (NULL != sg_msg) {
        lv_label_set_text(sg_msg, prv_msg(st));
        /* 深底页：正常白字，损坏用暗红（UI_COL_DANGER 在深底上偏暗，够用且不刺眼）。 */
        lv_obj_set_style_text_color(sg_msg, lv_color_hex(
            (NOSD_ST_DAMAGED == st) ? UI_COL_DANGER : UI_COL_TEXT), 0);
    }
    if (NULL != sg_hint) {
        /* 只有"损坏"才给下一步提示 —— 另外两种要么等它自己好，要么用户已经在做了。 */
        if (NOSD_ST_DAMAGED == st) {
            lv_label_set_text(sg_hint, "请换一张卡，或按返回键离开");
            lv_obj_clear_flag(sg_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sg_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return TRUE;
}

/* 定时器（LVGL 上下文，可以放心碰 LVGL）：跟状态 + 预热结束时自动撤页。
 *
 * 为什么撤页在这里做而不是在路由的事件回调里：预热结束没有对应的事件 ——
 * `APP_EVT_SD_STATUS` 只在**拔插**时发，而"预热完了"是预热线程跑完那一刻。
 * 本页自己就是为这件事存在的，自己收尾最直接。 */
static void prv_tick_cb(lv_timer_t *t)
{
    NOSD_ST_E st;

    (void)t;

    /* **先判"该走了没"，再刷新文案。**
     *
     * 顺序反了会闪一下：预热刚结束那一刻 warming 已经是假、fault 又是 OK，
     * `prv_state()` 只能落到 ABSENT，于是先把标签刷成"请插入 SD 卡"，下一句才
     * 撤页 —— 用户看到一帧错误文案（真机日志里就是 `状态 -> 请插入 SD 卡` 紧跟
     * `SD 已就绪，撤掉本页`）。
     *
     * **还要先确认自己还在栈顶**：定时器是页面级的，万一本页被别的页面盖住
     * （现在没有这种路径，但别把"弹掉别人"写进定时器里），这一下会弹错人。 */
    if (app_router_on_top(PAGE_NOSD) && app_storage_ready() && !app_storage_warming()) {
        PR_NOTICE("[nosd] SD 已就绪，撤掉本页");
        (void)app_router_pop(app_router_depth() - 1);
        return;
    }

    st = prv_state();
    if (prv_apply_state(st)) {
        PR_NOTICE("[nosd] 状态 -> %s", prv_msg(st));
    }
}

static OPERATE_RET prv_nosd_enter(void *arg)
{
    (void)arg;

    app_disp_lock();

    if (NULL != sg_scr) {
        lv_obj_del(sg_scr);
        sg_scr = NULL;
    }
    sg_scr   = lv_obj_create(NULL);
    sg_msg   = NULL;
    sg_hint  = NULL;
    sg_shown = -1;
    app_ui_screen_bg(sg_scr);               /* 深色底（开机 logo 才是白底） */
    app_ui_title(sg_scr, "存储");

    sg_msg = app_ui_label(sg_scr, "", UI_FONT_CN, UI_COL_TEXT);
    lv_obj_align(sg_msg, LV_ALIGN_CENTER, 0, -20);

    sg_hint = app_ui_label(sg_scr, "", UI_FONT_CN, UI_COL_DIM);
    lv_obj_align(sg_hint, LV_ALIGN_CENTER, 0, 16);

    /* 右上角 ✕（和 WiFi 弹窗同一个控件）。**不做大按钮**：文案已经说清了，
     * 右下角还是小精灵的地盘，多一颗大按钮只会挤。 */
    app_ui_close_btn(sg_scr, prv_close_cb, NULL);

    (void)prv_apply_state(prv_state());

    lv_screen_load(sg_scr);

    if (NULL == sg_timer) {
        sg_timer = lv_timer_create(prv_tick_cb, NOSD_POLL_MS, NULL);
    }

    app_disp_unlock();

    PR_NOTICE("[nosd] 进入：%s", prv_msg((NOSD_ST_E)sg_shown));
    return OPRT_OK;
}

/* 界面保留（下次进入先删再建，与其它页一致）；只把定时器停掉 —— 页面都离开了
 * 还在轮询状态、甚至调 app_router_pop() 就荒唐了。 */
static void prv_nosd_exit(void)
{
    app_disp_lock();
    if (NULL != sg_timer) {
        lv_timer_del(sg_timer);
        sg_timer = NULL;
    }
    app_disp_unlock();
}

const APP_PAGE_T app_page_nosd = {
    .name     = PAGE_NOSD,
    .flags    = 0,                      /* 普通 LVGL 页；按键自己处理 */
    .on_enter = prv_nosd_enter,
    .on_exit  = prv_nosd_exit,
    .on_key   = prv_nosd_key,           /* 返回键回首页，见文件头 */
};
