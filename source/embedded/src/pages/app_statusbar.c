/**
 * @file app_statusbar.c
 * @brief 顶部状态栏 + 三个弹窗。设计说明见 app_statusbar.h。
 *
 * 这一份是从老主页（heikesong 的 page_home.c）搬过来的，但**尺寸全部重排过**：
 * 老主页是竖屏 320x480（不旋转），这里跑在横屏 480x320 上（桌宠页把显示旋转了
 * 90°，见 page_pet.c 的 prv_rot_set）。所以：
 *
 *   - 状态栏 480x32 横跨全屏
 *   - WiFi 窗铺满 480x320，键盘 480x170 贴底，输入框并排一行
 *   - 音量窗只留 AI 一路，从 280x300 缩到 280x140
 *   - 天气详情窗 280x224 居中（这个尺寸横竖都放得下，没动）
 *
 * 三个弹窗**互相排斥**（开一个就关掉另外两个），共用一个模态遮罩。这套关系在
 * 老主页里就是对的，照搬过来 —— 拆开写三份会立刻出现"两个窗叠在一起"。
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tkl_wifi.h"           /* WIFI_SSID_LEN / WIFI_PASSWD_LEN */

#include "lvgl.h"

#include "app_audio.h"
#include "app_battery.h"
#include "app_disp.h"
#include "app_event.h"
#include "app_net.h"
#include "app_statusbar.h"
#include "app_theme.h"
#include "app_time.h"
#include "app_volui.h"
#include "app_weather.h"
#include "weather_icons.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* 横屏几何。**不要用 app_theme.h 的 UI_PANEL_W/H** —— 那是竖屏（320x480）的，
 * 这块屏旋转之后逻辑坐标是 480x320。 */
#define SB_W                480
#define SB_H                320

/* 配色：与桌宠页（page_pet.c 的 PET_COL_*）同一套低对比调子，只把状态栏底色
 * 提亮一档，让它读起来是"一条栏"而不是背景。 */
#define SB_COL_BG           0x1B232F
#define SB_COL_LINE         0x2A3442      /* 栏底那条分隔线 */
#define SB_COL_ICON         0xE4EBF5
#define SB_COL_DIM          0x8C9AAC
#define SB_COL_ACCENT       0x4DA3FF

/** 状态栏刷新周期（ms）。时钟是分钟级的，但电量/网络状态要跟得上。 */
#define SB_TICK_MS          1000

/** 天气图标在栏里缩小到这个比例（256 = 100%）。素材是 48x48，缩一半 = 24x24。 */
#define SB_WICON_SCALE      128

/** 缩完之后图标的**对象尺寸**。⚠️ `lv_image_set_scale()` 只改绘制缩放，**不改
 * 对象尺寸** —— 不显式设的话对象还是 48x48，在 32px 高的状态栏里会被裁掉下半截，
 * 旁边的温度文字也被挤下去（这就是"天气看不全"的原因）。 */
#define SB_WICON_PX         24

/***********************************************************
*************************variable define********************
***********************************************************/
static lv_obj_t *sg_root;          /* 弹窗的父对象（桌宠页的屏幕） */
static lv_obj_t *sg_bar;

static lv_obj_t *sg_clock;
static lv_obj_t *sg_wifi_icon;
static lv_obj_t *sg_wea_icon;
static lv_obj_t *sg_wea_text;
static lv_obj_t *sg_bat_icon;
static lv_obj_t *sg_bat_text;

static lv_timer_t *sg_tick;
static char        sg_clock_last[16];
static int32_t     sg_wea_code = -1;      /* 上次画过的天气码，用来跳过重设图片 */

/* 天气快照更新了。**只被事件回调写、只被 LVGL 定时器读** —— 跨线程写一个
 * BOOL_T 是原子的，不需要锁（老主页就是这么干的，见 app_weather.h）。 */
static volatile BOOL_T sg_wea_dirty = FALSE;

/* ---- 弹窗 ---- */
static lv_obj_t *sg_mask;

static lv_obj_t *sg_vol_win;

static lv_obj_t *sg_wea_win;

static lv_obj_t *sg_set_win;

static lv_obj_t *sg_wifi_win;
static lv_obj_t *sg_wifi_kb;
static lv_obj_t *sg_wifi_ta_ssid;
static lv_obj_t *sg_wifi_ta_pwd;
static lv_obj_t *sg_wifi_status;
static lv_timer_t *sg_wifi_poll;
static BOOL_T      sg_wifi_attempted;
static int32_t     sg_wifi_last_status;

/***********************************************************
***********************前向声明*****************************
***********************************************************/
static void prv_vol_close(void);
static void prv_wea_close(void);
static void prv_set_close(void);
static void prv_wifi_close(void);

/***********************************************************
************************ modal mask ************************
***********************************************************/
/* 任一弹层打开时铺满整屏，吸掉所有落在底下控件（状态栏图标 / 宠物 / 备忘录）
 * 上的触摸 —— 这就是"弹层打开时其它按钮一律禁用"。
 *
 * 两个必须点：
 *  1) 必须在**弹层窗口之前**创建。LVGL 按创建顺序叠层，后创建的在上面，所以
 *     遮罩先建、窗口后建，窗口才在遮罩之上、自己还能点。
 *  2) 遮罩自己点一下 = 收起当前弹层（点空白处关闭，同时满足"点别的按钮没反应"）。
 *
 * 关弹层用 lv_obj_delete_async()：遮罩可能正是在它自己的事件回调里被关掉的，
 * 同步删会在事件分发途中把对象释放掉。
 */
static void prv_mask_click(lv_event_t *e)
{
    (void)e;

    if (NULL != sg_vol_win) {
        prv_vol_close();
    } else if (NULL != sg_wifi_win) {
        prv_wifi_close();
    } else if (NULL != sg_wea_win) {
        prv_wea_close();
    } else if (NULL != sg_set_win) {
        prv_set_close();
    }
}

static void prv_mask_show(void)
{
    if (NULL != sg_mask || NULL == sg_root) {
        return;                     /* 已经铺着了 */
    }
    sg_mask = lv_obj_create(sg_root);
    lv_obj_set_size(sg_mask, SB_W, SB_H);
    lv_obj_set_pos(sg_mask, 0, 0);
    lv_obj_set_style_bg_opa(sg_mask, LV_OPA_TRANSP, 0);   /* 透明，不挡视觉 */
    lv_obj_set_style_border_width(sg_mask, 0, 0);
    lv_obj_set_style_radius(sg_mask, 0, 0);
    lv_obj_set_style_pad_all(sg_mask, 0, 0);
    lv_obj_clear_flag(sg_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sg_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sg_mask, prv_mask_click, LV_EVENT_CLICKED, NULL);
}

static void prv_mask_hide(void)
{
    if (NULL != sg_mask) {
        lv_obj_delete_async(sg_mask);
        sg_mask = NULL;
    }
}

/* 把三个弹窗全关掉（建新弹窗前调，保证一次只有一个）。 */
static void prv_close_all_popups(void)
{
    if (NULL != sg_vol_win) {
        prv_vol_close();
    }
    if (NULL != sg_wea_win) {
        prv_wea_close();
    }
    if (NULL != sg_wifi_win) {
        prv_wifi_close();
    }
    if (NULL != sg_set_win) {
        prv_set_close();
    }
}

/***********************************************************
************************ small helper **********************
***********************************************************/
/* 状态栏图标按钮：透明底，留出指尖余量。 */
static lv_obj_t *prv_icon(lv_obj_t *parent, const char *sym, uint32_t color,
                          int x, int y)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);           /* touch finger-room */
    app_ui_label(btn, sym, UI_FONT_ICON, color);
    return btn;
}

/* 一个不透明的小窗（三个弹窗共用）。建完里面自己排。 */
static lv_obj_t *prv_window(const char *title, int w, int h,
                            lv_event_cb_t close_cb)
{
    lv_obj_t *win = lv_obj_create(sg_root);

    lv_obj_set_size(win, w, h);
    lv_obj_center(win);
    lv_obj_set_style_bg_color(win, lv_color_hex(SB_COL_BG), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(win, lv_color_hex(SB_COL_ACCENT), 0);
    lv_obj_set_style_radius(win, 8, 0);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    if (NULL != title) {
        lv_obj_t *t = app_ui_label(win, title, UI_FONT_CN, SB_COL_ACCENT);

        lv_obj_set_pos(t, 12, 8);
    }
    app_ui_close_btn(win, close_cb, NULL);
    return win;
}

/***********************************************************
*********************** 栏内刷新 ****************************
***********************************************************/
static void prv_clock_refresh(void)
{
    char buf[16];

    if (NULL == sg_clock) {
        return;
    }
    /* 没联网时 app_time 给的是无效时间 —— 显示占位，别显示 1970。 */
    if (!app_time_valid() || OPRT_OK != app_time_format(buf, sizeof(buf), "%H:%M")) {
        snprintf(buf, sizeof(buf), "--:--");
    }
    /* 1s tick 里一直调它，但内容一分钟才变一次 —— 内容没变就不 set_text，
     * 省掉一次标签重排 + 重绘。 */
    if (0 == strcmp(buf, sg_clock_last)) {
        return;
    }
    snprintf(sg_clock_last, sizeof(sg_clock_last), "%s", buf);
    lv_label_set_text(sg_clock, buf);
}

static void prv_wifi_refresh(void)
{
    if (NULL == sg_wifi_icon) {
        return;
    }
    /* 图标底下那个 label 是 prv_icon 建的唯一子对象。连上了就点亮点，没连上压暗。 */
    lv_obj_t *l = lv_obj_get_child(sg_wifi_icon, 0);

    if (NULL == l) {
        return;
    }
    lv_obj_set_style_text_color(l, lv_color_hex((APP_NET_UP == app_net_status())
                                                    ? SB_COL_ICON : SB_COL_DIM), 0);
}

static void prv_battery_refresh(void)
{
    static int s_last_pct = -999;      /* 值没变就不动 label（每秒一次，别白重绘） */
    static int s_last_chg = -1;
    int    pct = 0;
    BOOL_T charging = FALSE;

    if (NULL == sg_bat_text) {
        return;
    }

    if (!app_battery_get(&pct, &charging)) {
        if (-999 != s_last_pct) {
            s_last_pct = -999;
            lv_label_set_text(sg_bat_text, "--");
        }
        return;
    }
    if (pct == s_last_pct && (int)charging == s_last_chg) {
        return;
    }
    s_last_pct = pct;
    s_last_chg = (int)charging;

    lv_label_set_text_fmt(sg_bat_text, "%d%%", pct);

    if (NULL != sg_bat_icon) {
        lv_obj_t *l = lv_obj_get_child(sg_bat_icon, 0);
        const char *sym;

        if (charging) {
            sym = LV_SYMBOL_CHARGE;
        } else if (pct >= 80) {
            sym = LV_SYMBOL_BATTERY_FULL;
        } else if (pct >= 55) {
            sym = LV_SYMBOL_BATTERY_3;
        } else if (pct >= 30) {
            sym = LV_SYMBOL_BATTERY_2;
        } else if (pct >= 10) {
            sym = LV_SYMBOL_BATTERY_1;
        } else {
            sym = LV_SYMBOL_BATTERY_EMPTY;
        }
        if (NULL != l) {
            lv_label_set_text(l, sym);
        }
    }
}

static void prv_weather_refresh(void)
{
    app_weather_info_t info;
    const app_weather_day_t *d = NULL;

    if (NULL == sg_wea_text || NULL == sg_wea_icon) {
        return;
    }

    memset(&info, 0, sizeof(info));
    (void)app_weather_get(&info);
    if (info.days > 0) {
        d = &info.day[0];               /* 栏里只放今天 */
    }

    if (NULL != d && d->high[0]) {
        lv_label_set_text_fmt(sg_wea_text, "%s°", d->high);
    } else {
        lv_label_set_text(sg_wea_text, "--°");
    }

    /* 图标只在天气码变了才重设 —— lv_image_set_src 会触发一次重绘。 */
    {
        int32_t code = (NULL != d) ? d->code : APP_WEATHER_CODE_UNKNOWN;

        if (code != sg_wea_code) {
            sg_wea_code = code;
            lv_image_set_src(sg_wea_icon, app_weather_icon(code));
        }
    }
}

/***********************************************************
************************ 音量窗 *****************************
***********************************************************/
static void prv_vol_close(void)
{
    if (NULL != sg_vol_win) {
        lv_obj_delete_async(sg_vol_win);
    }
    sg_vol_win = NULL;
    prv_mask_hide();
}

static void prv_vol_close_cb(lv_event_t *e)
{
    (void)e;
    prv_vol_close();
}

/* 只有 AI 一路了（见 app_audio.h），所以窗口从老主页的 280x300 缩到 280x140。 */
static void prv_vol_open(void)
{
    if (NULL != sg_vol_win) {
        prv_vol_close();
        return;
    }
    prv_close_all_popups();

    prv_mask_show();                    /* 先铺遮罩，再建窗口 —— 窗口才在遮罩之上 */
    sg_vol_win = prv_window("音量", 280, 140, prv_vol_close_cb);
    app_volui_row(sg_vol_win, APP_AUDIO_CH_AI, 44);
    PR_NOTICE("[bar] 音量窗打开");
}

/***********************************************************
*********************** 天气详情窗 **************************
***********************************************************/
static void prv_wea_close(void)
{
    if (NULL != sg_wea_win) {
        lv_obj_delete_async(sg_wea_win);
    }
    sg_wea_win = NULL;
    prv_mask_hide();
}

static void prv_wea_close_cb(lv_event_t *e)
{
    (void)e;
    prv_wea_close();
}

/* "2026-10-03" -> "10月3日"；解析失败就原样拷贝。 */
static void prv_fmt_date(const char *src, char *out, size_t n)
{
    int y = 0, m = 0, d = 0;

    if ((NULL != src) && (3 == sscanf(src, "%d-%d-%d", &y, &m, &d))) {
        (void)y;                    /* 只取月日；年由弹窗版面省掉 */
        snprintf(out, n, "%d月%d日", m, d);
    } else {
        snprintf(out, n, "%s", (NULL != src) ? src : "--");
    }
}

/* 三天**并排**的详情弹窗。
 *
 * 老主页是竖屏，三列摆得下（320 宽 / 3 列）；横屏更宽，所以这里放宽到
 * 456x240、每列 144 —— 图标能按素材原尺寸（48x48）画，不用缩。
 *
 * 一列里从上到下：今天/明天/后天 → 日期 → 图标 → 天气 → 温度 → 湿度风。
 * 温度用大字号 + 强调色，是这三列里唯一"扫一眼就要拿到"的信息。 */
static void prv_wea_detail_open(void)
{
    static const char *const s_name[APP_WEATHER_DAYS] = { "今天", "明天", "后天" };
    app_weather_info_t info;
    lv_obj_t *row;

    prv_close_all_popups();
    prv_mask_show();

    memset(&info, 0, sizeof(info));
    (void)app_weather_get(&info);

    sg_wea_win = prv_window("天气", 456, 240, prv_wea_close_cb);

    /* 城市跟在标题右边（拿到了才显示）。 */
    if ('\0' != info.city[0]) {
        lv_obj_t *city = app_ui_label(sg_wea_win, info.city, UI_FONT_CN, SB_COL_DIM);

        lv_obj_set_pos(city, 56, 10);
    }

    row = lv_obj_create(sg_wea_win);
    lv_obj_set_size(row, 440, 180);
    lv_obj_set_pos(row, 8, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (int i = 0; i < APP_WEATHER_DAYS; i++) {
        const app_weather_day_t *d = (info.days > i) ? &info.day[i] : NULL;
        lv_obj_t *col = lv_obj_create(row);
        lv_obj_t *l;
        char      buf[40];
        char      date[24];

        lv_obj_set_size(col, 144, 180);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        /* 列头（今天/明天/后天）—— 不依赖数据，永远显示。 */
        l = app_ui_label(col, s_name[i], UI_FONT_CN, SB_COL_ACCENT);
        lv_obj_set_width(l, 144);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, 0);

        /* 日期 */
        prv_fmt_date((NULL != d) ? d->date : NULL, date, sizeof(date));
        l = app_ui_label(col, date, UI_FONT_CN, SB_COL_DIM);
        lv_obj_set_width(l, 144);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, 20);

        /* 图标（素材原尺寸 48x48） */
        lv_obj_t *ic = lv_image_create(col);

        lv_image_set_src(ic, app_weather_icon((NULL != d) ? d->code
                                                          : APP_WEATHER_CODE_UNKNOWN));
        lv_obj_set_pos(ic, 48, 42);

        /* 天气：白天 + 夜间（"晴转多云"）；同名就只留白天 */
        {
            const char *td = (NULL != d && d->text_day[0]) ? d->text_day : "--";
            const char *tn = (NULL != d && d->text_night[0]) ? d->text_night : NULL;

            if ((NULL != tn) && (0 != strcmp(tn, td))) {
                snprintf(buf, sizeof(buf), "%s转%s", td, tn);
            } else {
                snprintf(buf, sizeof(buf), "%s", td);
            }
            l = app_ui_label(col, buf, UI_FONT_CN, SB_COL_ICON);
            lv_obj_set_width(l, 144);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, 0, 96);
        }

        /* 温度：这一列里唯一的大字 */
        if ((NULL != d) && d->high[0] && d->low[0]) {
            snprintf(buf, sizeof(buf), "%s°/%s°", d->high, d->low);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        l = app_ui_label(col, buf, UI_FONT_21, SB_COL_ACCENT);
        lv_obj_set_width(l, 144);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, 118);

        /* 湿度 / 风 —— 有就显示，没有就留空 */
        buf[0] = '\0';
        if ((NULL != d) && d->humidity[0]) {
            snprintf(buf, sizeof(buf), "湿度 %s%%", d->humidity);
        }
        if ((NULL != d) && d->wind_scale[0]) {
            size_t n = strlen(buf);

            snprintf(buf + n, sizeof(buf) - n, "%s%s级",
                     (0 != n) ? " · " : "", d->wind_scale);
        }
        if ('\0' != buf[0]) {
            l = app_ui_label(col, buf, UI_FONT_CN, SB_COL_DIM);
            lv_obj_set_width(l, 144);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, 0, 150);
        }
    }
}

/* 点栏里的天气格：已开着就收起（再点一次 = 关），否则开详情。
 *
 * 用 RELEASED 触发，且做 400ms 防抖：一次物理触摸若被面板/驱动重复上报，
 * 会出现"刚开就被第二个事件关掉"的观感（看着就是点了没反应）。 */
static uint32_t sg_wea_ms = 0;

static void prv_wea_click(lv_event_t *e)
{
    uint32_t now = lv_tick_get();

    (void)e;

    if ((uint32_t)(now - sg_wea_ms) < 400U) {
        return;
    }
    sg_wea_ms = now;

    if (NULL != sg_wea_win) {
        prv_wea_close();
        return;
    }
    prv_wea_detail_open();
}

/***********************************************************
************************* WiFi 窗 ***************************
***********************************************************/
/* 手动输入 SSID/密码连 WiFi。凭据走 netmgr 的 SSID_PSWD 通道，和 AP / BLE 配网
 * 完全同一条路径，所以两种方式可以共存；连上后 SDK 会把凭据存进 KV "netinfo"。 */
static void prv_wifi_close(void)
{
    if (NULL != sg_wifi_poll) {
        lv_timer_del(sg_wifi_poll);
        sg_wifi_poll = NULL;
    }
    if (NULL != sg_wifi_win) {
        lv_obj_delete_async(sg_wifi_win);
    }
    sg_wifi_win         = NULL;
    sg_wifi_kb          = NULL;
    sg_wifi_ta_ssid     = NULL;
    sg_wifi_ta_pwd      = NULL;
    sg_wifi_status      = NULL;
    sg_wifi_attempted   = FALSE;
    sg_wifi_last_status = -1;
    prv_mask_hide();
}

static void prv_wifi_close_cb(lv_event_t *e)
{
    (void)e;
    prv_wifi_close();
}

/* 点文本框把键盘路由过去 */
static void prv_wifi_ta_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (NULL != sg_wifi_kb) {
        lv_keyboard_set_textarea(sg_wifi_kb, ta);
        lv_obj_clear_flag(sg_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 「连接」按钮和键盘回车共用（单行文本框回车会发 LV_EVENT_READY） */
static void prv_wifi_try_connect(void)
{
    const char *ssid;
    const char *pwd;

    if (NULL == sg_wifi_ta_ssid || NULL == sg_wifi_ta_pwd) {
        return;
    }

    ssid = lv_textarea_get_text(sg_wifi_ta_ssid);
    pwd  = lv_textarea_get_text(sg_wifi_ta_pwd);
    if (NULL == ssid || '\0' == ssid[0]) {
        lv_label_set_text(sg_wifi_status, "请输入WiFi名称");
        return;
    }

    if (OPRT_OK == app_net_connect(ssid, pwd)) {
        lv_label_set_text(sg_wifi_status, "连接中...");
        sg_wifi_attempted   = TRUE;
        sg_wifi_last_status = APP_NET_CONNECTING;
        if (NULL != sg_wifi_kb) {
            lv_obj_add_flag(sg_wifi_kb, LV_OBJ_FLAG_HIDDEN);   /* 让出屏幕看状态 */
        }
    } else {
        lv_label_set_text(sg_wifi_status, "下发失败,请重试");
    }
}

static void prv_wifi_connect_cb(lv_event_t *e)
{
    (void)e;
    prv_wifi_try_connect();
}

static void prv_wifi_ready_cb(lv_event_t *e)
{
    (void)e;
    prv_wifi_try_connect();
}

/* 轮询连接状态（跑在 LVGL 任务里）。超时判定在 app_net 内部，这里只管显示。 */
static void prv_wifi_poll_cb(lv_timer_t *timer)
{
    int st;

    (void)timer;
    if (NULL == sg_wifi_status || !sg_wifi_attempted) {
        return;
    }

    st = (int)app_net_status();
    if (st == sg_wifi_last_status) {
        return;
    }
    sg_wifi_last_status = st;

    switch (st) {
    case APP_NET_UP:
        lv_label_set_text(sg_wifi_status, "已连接");
        break;
    case APP_NET_CONNECTING:
        lv_label_set_text(sg_wifi_status, "连接中...");
        break;
    default:
        lv_label_set_text(sg_wifi_status, "连接失败,请检查名称和密码");
        break;
    }
}

/* 横屏版面：标题/状态/两个输入框并排/连接按钮，键盘 480x170 贴底。
 *
 * 老主页是竖屏（输入框上下叠、键盘 200 高），直接搬过来键盘会把按钮盖住 ——
 * 320 高的屏塞不下"标题 + 状态 + 两个竖排输入框 + 按钮 + 200 键盘"。 */
static void prv_wifi_open(void)
{
    if (NULL != sg_wifi_win) {
        prv_wifi_close();
        return;
    }
    prv_close_all_popups();
    prv_mask_show();

    sg_wifi_win = lv_obj_create(sg_root);
    lv_obj_set_size(sg_wifi_win, SB_W, SB_H);
    lv_obj_set_pos(sg_wifi_win, 0, 0);
    lv_obj_set_style_bg_color(sg_wifi_win, lv_color_hex(SB_COL_BG), 0);
    lv_obj_set_style_bg_opa(sg_wifi_win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sg_wifi_win, 0, 0);
    lv_obj_set_style_radius(sg_wifi_win, 0, 0);
    lv_obj_set_style_pad_all(sg_wifi_win, 0, 0);
    lv_obj_clear_flag(sg_wifi_win, LV_OBJ_FLAG_SCROLLABLE);

    app_ui_title(sg_wifi_win, "WiFi设置");
    app_ui_close_btn(sg_wifi_win, prv_wifi_close_cb, NULL);

    sg_wifi_status = app_ui_label(sg_wifi_win, "输入名称和密码", UI_FONT_CN, SB_COL_DIM);
    lv_obj_set_pos(sg_wifi_status, 12, 32);

    /* 两个输入框并排一行 —— 横屏宽度够，省下的高度留给键盘。 */
    lv_obj_t *ls = app_ui_label(sg_wifi_win, "名称", UI_FONT_CN, SB_COL_ICON);
    lv_obj_set_pos(ls, 12, 60);
    sg_wifi_ta_ssid = lv_textarea_create(sg_wifi_win);
    lv_textarea_set_one_line(sg_wifi_ta_ssid, true);
    lv_obj_set_pos(sg_wifi_ta_ssid, 58, 54);
    lv_obj_set_size(sg_wifi_ta_ssid, 180, 32);

    lv_obj_t *lp = app_ui_label(sg_wifi_win, "密码", UI_FONT_CN, SB_COL_ICON);
    lv_obj_set_pos(lp, 250, 60);
    sg_wifi_ta_pwd = lv_textarea_create(sg_wifi_win);
    lv_textarea_set_one_line(sg_wifi_ta_pwd, true);
    lv_textarea_set_password_mode(sg_wifi_ta_pwd, true);
    lv_obj_set_pos(sg_wifi_ta_pwd, 296, 54);
    lv_obj_set_size(sg_wifi_ta_pwd, 172, 32);

    lv_obj_add_event_cb(sg_wifi_ta_ssid, prv_wifi_ta_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sg_wifi_ta_pwd, prv_wifi_ta_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sg_wifi_ta_ssid, prv_wifi_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(sg_wifi_ta_pwd, prv_wifi_ready_cb, LV_EVENT_READY, NULL);

    /* 预填 SDK 上次保存的凭据 */
    {
        char s[WIFI_SSID_LEN + 1] = {0};
        char p[WIFI_PASSWD_LEN + 1] = {0};

        app_net_get_saved(s, sizeof(s), p, sizeof(p));
        lv_textarea_set_text(sg_wifi_ta_ssid, s);
        lv_textarea_set_text(sg_wifi_ta_pwd, p);
    }

    lv_obj_t *conn = app_ui_button(sg_wifi_win, 140, 40, SB_COL_ACCENT, "连接",
                                   UI_FONT_CN, 0x000000, prv_wifi_connect_cb, NULL);
    lv_obj_align(conn, LV_ALIGN_TOP_MID, 0, 96);   /* 两个输入框下面、键盘上面，居中 */

    /* 键盘贴底，480x170 —— 上面那一行输入框正好在它上面。 */
    sg_wifi_kb = lv_keyboard_create(sg_wifi_win);
    lv_obj_set_size(sg_wifi_kb, SB_W, 170);
    /* 键盘创建时自带了 bottom-mid 的样式对齐；set_pos() 会在这个锚点基础上再加
     * 偏移，把键盘挤出屏幕。所以这里保持锚点、只把偏移清零。 */
    lv_obj_align(sg_wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(sg_wifi_kb, sg_wifi_ta_ssid);

    sg_wifi_poll = lv_timer_create(prv_wifi_poll_cb, 500, NULL);
    PR_NOTICE("[bar] WiFi 窗打开");
}

/***********************************************************
************************ 设置小面板 *************************
***********************************************************/
/* P12 长按弹出来的两格面板。**不做成"设置页"** —— 现在只有网络和音量两件事，
 * 一个页面栈的层级换两行按钮不划算（而且桌宠页是唯一页面，加页面就多一套
 * 返回键语义）。以后项多了再升级成页面。 */
static void prv_set_close(void)
{
    if (NULL != sg_set_win) {
        lv_obj_delete_async(sg_set_win);
    }
    sg_set_win = NULL;
    prv_mask_hide();
}

static void prv_set_close_cb(lv_event_t *e)
{
    (void)e;
    prv_set_close();
}

static void prv_set_wifi_cb(lv_event_t *e)
{
    (void)e;
    prv_set_close();
    prv_wifi_open();
}

static void prv_set_vol_cb(lv_event_t *e)
{
    (void)e;
    prv_set_close();
    prv_vol_open();
}

static void prv_set_open(void)
{
    if (NULL != sg_set_win) {
        prv_set_close();
        return;
    }
    prv_close_all_popups();
    prv_mask_show();

    sg_set_win = prv_window("设置", 220, 130, prv_set_close_cb);

    lv_obj_t *b1 = app_ui_button(sg_set_win, 188, 32, SB_COL_BG, "网络设置",
                                 UI_FONT_CN, SB_COL_ICON, prv_set_wifi_cb, NULL);
    lv_obj_set_pos(b1, 16, 42);

    lv_obj_t *b2 = app_ui_button(sg_set_win, 188, 32, SB_COL_BG, "音量",
                                 UI_FONT_CN, SB_COL_ICON, prv_set_vol_cb, NULL);
    lv_obj_set_pos(b2, 16, 84);
}

/***********************************************************
************** 栏里图标的点击（跑在 LVGL 线程） **************
***********************************************************/
/* 这些回调是 LVGL 事件回调，**已经在 LVGL 线程里**（显示锁由 LVGL 任务持有），
 * 所以直接调 prv_*_open()，不要再走 app_statusbar_open_*() 那层加锁的公开接口
 * —— app_disp_lock() 不可重入（见 app_router.h 的约束）。 */
static void prv_wifi_icon_cb(lv_event_t *e)
{
    (void)e;
    prv_wifi_open();
}

static void prv_vol_icon_cb(lv_event_t *e)
{
    (void)e;
    prv_vol_open();
}

/***********************************************************
************************* 定时器 ****************************
***********************************************************/
/* 状态栏被销毁时（页面退出、屏幕重建）**必须把定时器一起收掉**。
 *
 * 少了这一步就是真机上的野指针：屏幕删了，1s 的 tick 还在跑，下一拍就去
 * `lv_label_set_text(sg_clock, ...)` 摸一块已经释放的内存 —— 表现是退出页面后
 * 随机崩溃，而且崩在哪完全看不出来。两个定时器（tick / WiFi 轮询）都要收。
 *
 * 挂在这个回调上而不是让页面显式调：销毁路径只有一条，写在这里不会漏。 */
static void prv_bar_deleted_cb(lv_event_t *e)
{
    (void)e;

    if (NULL != sg_tick) {
        lv_timer_del(sg_tick);
        sg_tick = NULL;
    }
    if (NULL != sg_wifi_poll) {
        lv_timer_del(sg_wifi_poll);
        sg_wifi_poll = NULL;
    }

    /* 屏幕一没，这些对象和指针全部作废 —— 一个不留。 */
    sg_bar       = NULL;
    sg_mask      = NULL;
    sg_vol_win   = NULL;
    sg_wea_win   = NULL;
    sg_set_win   = NULL;
    sg_wifi_win  = NULL;
    sg_wifi_kb   = NULL;
    sg_wifi_ta_ssid = NULL;
    sg_wifi_ta_pwd  = NULL;
    sg_wifi_status  = NULL;
    sg_clock     = NULL;
    sg_wifi_icon = NULL;
    sg_wea_icon  = NULL;
    sg_wea_text  = NULL;
    sg_bat_icon  = NULL;
    sg_bat_text  = NULL;
}

static void prv_tick_cb(lv_timer_t *t)
{
    (void)t;

    prv_clock_refresh();
    prv_wifi_refresh();
    prv_battery_refresh();

    if (sg_wea_dirty) {
        sg_wea_dirty = FALSE;
        prv_weather_refresh();
    }
}

/***********************************************************
************************ 事件回调 ****************************
***********************************************************/
/* **跑在发布者线程**，只能置脏位 —— 碰 LVGL 必须回 LVGL 线程（见 .h）。 */
static void prv_evt_cb(APP_EVT_E evt, void *data, void *user)
{
    (void)data;
    (void)user;

    /* 网络状态和电量都由 1s tick 自己读（app_net_status / app_battery_get 都是
     * 纯内存读，没有 IO），不需要事件驱动 —— 所以这里只接天气。 */
    if (APP_EVT_WEATHER_UPDATED == evt) {
        sg_wea_dirty = TRUE;
    }
}

/***********************************************************
************************* public api ************************
***********************************************************/
lv_obj_t *app_statusbar_create(lv_obj_t *parent)
{
    if (NULL == parent) {
        return NULL;
    }

    sg_root = parent;

    /* 上一次的状态栏还活着（同一个屏幕重建）就先删掉 —— 它的 DELETE 回调会把
     * 定时器和所有指针清干净，所以下面可以直接重来。**屏幕被删过的话 sg_bar
     * 已经是 NULL**（prv_bar_deleted_cb 清的），不会摸到野指针。 */
    if (NULL != sg_bar) {
        lv_obj_del(sg_bar);
    }
    sg_clock_last[0] = '\0';
    sg_wea_code = -1;

    sg_bar = lv_obj_create(parent);
    lv_obj_set_size(sg_bar, SB_W, APP_STATUSBAR_H);
    lv_obj_set_pos(sg_bar, 0, 0);
    lv_obj_set_style_bg_color(sg_bar, lv_color_hex(SB_COL_BG), 0);
    lv_obj_set_style_bg_opa(sg_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sg_bar, 0, 0);
    lv_obj_set_style_pad_all(sg_bar, 0, 0);
    /* 底下一条 1px 分隔线 —— 让状态栏读起来是"一条栏"，不是背景的一部分。 */
    lv_obj_set_style_border_width(sg_bar, 1, 0);
    lv_obj_set_style_border_side(sg_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(sg_bar, lv_color_hex(SB_COL_LINE), 0);
    lv_obj_clear_flag(sg_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sg_bar, prv_bar_deleted_cb, LV_EVENT_DELETE, NULL);

    /* ---- 左：WiFi + 天气 ----
     * 栏里每一格都**垂直居中**（栏 32px、图标 24px，所以 y 偏移一律 0）。 */
    sg_wifi_icon = prv_icon(sg_bar, LV_SYMBOL_WIFI, SB_COL_DIM, 0, 0);
    lv_obj_align(sg_wifi_icon, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(sg_wifi_icon, prv_wifi_icon_cb, LV_EVENT_CLICKED, NULL);

    /* 天气格：图标 + 温度，整格可点 -> 详情弹窗。
     *
     * 整格在栏里**垂直居中**（栏 32px，图标 24px + 上下各 4px 内边距 = 32，正好）。 */
    {
        lv_obj_t *wea = lv_button_create(sg_bar);

        lv_obj_set_size(wea, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(wea, LV_ALIGN_LEFT_MID, 40, 0);
        lv_obj_set_style_bg_opa(wea, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wea, 0, 0);
        lv_obj_set_style_pad_all(wea, 4, 0);
        lv_obj_set_flex_flow(wea, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wea, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(wea, 2, 0);

        sg_wea_icon = lv_image_create(wea);
        lv_image_set_scale(sg_wea_icon, SB_WICON_SCALE);
        lv_obj_set_size(sg_wea_icon, SB_WICON_PX, SB_WICON_PX);   /* 见 SB_WICON_PX 的说明 */
        sg_wea_text = app_ui_label(wea, "--°", UI_FONT_CN, SB_COL_ICON);
        lv_obj_add_event_cb(wea, prv_wea_click, LV_EVENT_RELEASED, NULL);
    }

    /* ---- 中：时间 ---- */
    sg_clock = app_ui_label(sg_bar, "--:--", UI_FONT_CN, SB_COL_ICON);
    lv_obj_align(sg_clock, LV_ALIGN_CENTER, 0, 0);

    /* ---- 右：音量 + 电量 ----
     *
     * 电量那两格（图标 + 百分比）**必须贴在一起** —— 它们是一件事的两个部分，
     * 分开摆会读成两个不相干的图标。所以按"百分比在右、图标紧贴它左边"来摆：
     *   百分比：右对齐到 -4，宽 42
     *   图标  ：右对齐到 -48（图标约 24 宽，正好接在百分比左边，中间留 2px） */
    {
        lv_obj_t *vol = prv_icon(sg_bar, LV_SYMBOL_VOLUME_MAX, SB_COL_ICON, 0, 0);

        lv_obj_align(vol, LV_ALIGN_RIGHT_MID, -124, 0);
        lv_obj_add_event_cb(vol, prv_vol_icon_cb, LV_EVENT_CLICKED, NULL);
    }
    sg_bat_icon = prv_icon(sg_bar, LV_SYMBOL_BATTERY_3, SB_COL_ICON, 0, 0);
    lv_obj_align(sg_bat_icon, LV_ALIGN_RIGHT_MID, -48, 0);

    sg_bat_text = app_ui_label(sg_bar, "--", UI_FONT_CN, SB_COL_ICON);
    lv_obj_set_width(sg_bat_text, 42);
    lv_obj_set_style_text_align(sg_bat_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(sg_bat_text, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ---- 首次填充 + 常驻定时器 ---- */
    prv_clock_refresh();
    prv_wifi_refresh();
    prv_battery_refresh();
    prv_weather_refresh();

    if (NULL == sg_tick) {
        sg_tick = lv_timer_create(prv_tick_cb, SB_TICK_MS, NULL);
    }

    /* 订阅是幂等的（见 app_event.h）——页面重进时重复调不会叠加。 */
    app_event_subscribe(APP_EVT_WEATHER_UPDATED, prv_evt_cb, NULL);

    PR_NOTICE("[bar] 状态栏就绪（%dx%d）", SB_W, APP_STATUSBAR_H);
    return sg_bar;
}

void app_statusbar_open_settings(void)
{
    app_disp_lock();
    prv_set_open();
    app_disp_unlock();
}

void app_statusbar_open_wifi(void)
{
    app_disp_lock();
    prv_wifi_open();
    app_disp_unlock();
}

void app_statusbar_open_volume(void)
{
    app_disp_lock();
    prv_vol_open();
    app_disp_unlock();
}
