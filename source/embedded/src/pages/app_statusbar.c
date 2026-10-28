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
#include "app_weather_icon.h"

/* 新 UI 的图标字体和配色 token。状态栏现在长在新界面里，用它的字号和颜色。 */
#include "pet_theme.h"
#include "pet_ui.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* 横屏几何。**不要用 app_theme.h 的 UI_PANEL_W/H** —— 那是竖屏（320x480）的，
 * 这块屏旋转之后逻辑坐标是 480x320。 */
#define SB_W                480
#define SB_H                320

/* 配色：跟着新 UI 的**暗色**主题走（参考 ai_pet_wide/4页面暗色UI.png）——
 * 状态栏本身不铺底色，直接坐在深色页面底色上；弹窗用略亮的深色卡片 + 细边。
 * 图标和文字全部用浅色（weather 图标就是 SD 上 white/1x 那套原色，不再重着色）。 */
#define SB_COL_BG           0x151C2A      /* 弹窗底（深色卡片） */
#define SB_COL_LINE         0x263146      /* 细边 */
#define SB_COL_ICON         0xE9EEF8      /* 主文字 / 图标 */
#define SB_COL_DIM          0x9AA6BC      /* 次要文字 / 未连接 */
#define SB_COL_ACCENT       0x3D7BFF      /* 强调（标题、主按钮） */

/** 状态栏刷新周期（ms）。时钟是分钟级的，但电量/网络状态要跟得上。 */
#define SB_TICK_MS          1000

/** 天气图标在栏里缩小到这个比例（256 = 100%）。素材是 48x48，缩一半 = 24x24。 */
#define SB_WICON_SCALE      128

/** 缩完之后图标的**对象尺寸**。⚠️ `lv_image_set_scale()` 只改绘制缩放，**不改
 * 对象尺寸** —— 不显式设的话对象还是 48x48，在 24px 高的状态栏里会被裁掉下半截，
 * 旁边的温度文字也被挤下去（这就是"天气看不全"的原因）。 */
#define SB_WICON_PX         24

/***********************************************************
*************************variable define********************
***********************************************************/
static lv_obj_t *sg_root;          /* 弹窗的父对象（桌宠页的屏幕） */
static lv_obj_t *sg_bar;

static lv_obj_t *sg_badge;
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

/* SD 字库换进来了（APP_EVT_FONT_READY）：栏里的标签是按内置字体量过尺寸的，
 * 而且每个 refresh 都有"内容没变就不动"的短路。置上这一位让下一拍**强制**
 * 重写一遍，标签就用新字体重新量尺寸。 */
static volatile BOOL_T sg_font_dirty = FALSE;

/* 遮罩在不在。给宠物 tick 用的**免锁**只读标志 —— tick 跑在 LVGL 线程里，
 * 已经持有显示锁，再去 app_disp_lock() 会死锁。 */
static volatile BOOL_T sg_popup_on = FALSE;

/* ---- 弹窗 ---- */
static lv_obj_t *sg_mask;

static lv_obj_t *sg_vol_win;

static lv_obj_t *sg_wea_win;

static lv_obj_t *sg_set_win;

/* ---- 时间设置窗 ---- */
static lv_obj_t *sg_time_win;
static lv_obj_t *sg_time_big;      /* 大字 "10:34" */
static lv_obj_t *sg_time_date;     /* 日期行（只读） */
static lv_obj_t *sg_time_hv;       /* 时 / 分 的数值 */
static lv_obj_t *sg_time_mv;
static int       sg_time_h;
static int       sg_time_m;

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
static void prv_time_close(void);
static void prv_time_open(void);

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
    } else if (NULL != sg_time_win) {
        prv_time_close();
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
    /* **必须脱离布局**：新 UI 把屏幕本身设成了 flex 列（pui_screen_apply），
     * 不标这一位的话遮罩会被当成第二个 flex 子项排到 shell 下面（y=320），
     * 整块跑到屏幕外 —— 看得出来"弹窗打开了"但既不见遮罩也挡不住触摸。 */
    lv_obj_add_flag(sg_mask, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_opa(sg_mask, LV_OPA_TRANSP, 0);   /* 透明，不挡视觉 */
    lv_obj_set_style_border_width(sg_mask, 0, 0);
    lv_obj_set_style_radius(sg_mask, 0, 0);
    lv_obj_set_style_pad_all(sg_mask, 0, 0);
    lv_obj_clear_flag(sg_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sg_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sg_mask, prv_mask_click, LV_EVENT_CLICKED, NULL);
    sg_popup_on = TRUE;
}

static void prv_mask_hide(void)
{
    if (NULL != sg_mask) {
        lv_obj_delete_async(sg_mask);
        sg_mask = NULL;
    }
    sg_popup_on = FALSE;
}

/* 把三个弹窗全关掉（建新弹窗前调，保证一次只有一个）。
 *
 * 面板上开着模态 sheet（备忘录的图片选择器）时，也把它收掉：弹窗和 sheet 不能
 * 叠在一起，也不允许从 sheet 里点顶栏直接进另一个界面 —— 要么关掉，要么禁用。 */
static void prv_close_all_popups(void)
{
    if (pet_ui_sheet_active()) {
        pet_ui_close_sheet();
    }

    if (NULL != sg_vol_win) {
        prv_vol_close();
    }
    if (NULL != sg_wea_win) {
        prv_wea_close();
    }
    if (NULL != sg_wifi_win) {
        prv_wifi_close();
    }
    if (NULL != sg_time_win) {
        prv_time_close();
    }
    if (NULL != sg_set_win) {
        prv_set_close();
    }
}

/***********************************************************
************************ small helper **********************
***********************************************************/
/* 状态栏里的一个图标格：透明底，留出指尖余量。图标用 FontAwesome 16px，
 * 和新 UI 里 pui 组件的图标同一套字形。 */
static lv_obj_t *prv_glyph(lv_obj_t *parent, const char *sym, uint32_t color)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);           /* touch finger-room */
    app_ui_label(btn, sym, PET_FONT_ICON, color);
    return btn;
}

/* 状态栏里的一个文字格（14px）。 */
static lv_obj_t *prv_text(lv_obj_t *parent, const char *txt, uint32_t color)
{
    return pui_label_create(parent, txt, PET_FONT_CJK_SM, lv_color_hex(color));
}

/* 一个不透明的小窗（三个弹窗共用）。建完里面自己排。 */
static lv_obj_t *prv_window(const char *title, int w, int h,
                            lv_event_cb_t close_cb)
{
    lv_obj_t *win = lv_obj_create(sg_root);

    lv_obj_set_size(win, w, h);
    /* 和遮罩一样：屏幕是 flex 容器，窗口必须脱离布局才能按 lv_obj_center 定位，
     * 否则会被排到 shell 下面（屏幕外）。 */
    lv_obj_add_flag(win, LV_OBJ_FLAG_IGNORE_LAYOUT);
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
     * 省掉一次标签重排 + 重绘。字库换了要强制重写一次（见 sg_font_dirty）。 */
    if (!sg_font_dirty && 0 == strcmp(buf, sg_clock_last)) {
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
    if (!sg_font_dirty && pct == s_last_pct && (int)charging == s_last_chg) {
        return;
    }
    s_last_pct = pct;
    s_last_chg = (int)charging;

    lv_label_set_text_fmt(sg_bat_text, "%d%%", pct);

    if (NULL != sg_bat_icon) {
        lv_obj_t *l = lv_obj_get_child(sg_bat_icon, 0);
        const char *sym;

        if (charging) {
            sym = FONT_AWESOME_BATTERY_CHARGING;
        } else if (pct >= 80) {
            sym = FONT_AWESOME_BATTERY_FULL;
        } else if (pct >= 55) {
            sym = FONT_AWESOME_BATTERY_3;
        } else if (pct >= 30) {
            sym = FONT_AWESOME_BATTERY_2;
        } else if (pct >= 10) {
            sym = FONT_AWESOME_BATTERY_1;
        } else {
            sym = FONT_AWESOME_BATTERY_EMPTY;
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

        if (code != sg_wea_code || sg_font_dirty) {
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

/* 点时钟 = 打开时间设置（弹窗互斥 + 遮罩由 prv_time_open 处理）。 */
static void prv_time_click(lv_event_t *e)
{
    (void)e;
    prv_time_open();
}

/***********************************************************
********************** 时间设置窗 ***************************
***********************************************************/
/* 点顶栏的时钟打开。只拨**时 / 分**，日期不动 —— 日期来自云端，没有网的机器
 * 拨日期也没有意义；时/分错了（时区没同步、云校时失败）是用户唯一需要自己改的。
 *
 * 保存走 app_time_set_local()：按配置里的时区换算成 UTC 写 RTC。 */
static void prv_time_close(void)
{
    if (NULL != sg_time_win) {
        lv_obj_delete_async(sg_time_win);
    }
    sg_time_win  = NULL;
    sg_time_big  = NULL;
    sg_time_date = NULL;
    sg_time_hv   = NULL;
    sg_time_mv   = NULL;
    prv_mask_hide();
}

static void prv_time_close_cb(lv_event_t *e)
{
    (void)e;
    prv_time_close();
}

static void prv_time_show(void)
{
    if (NULL != sg_time_big) {
        lv_label_set_text_fmt(sg_time_big, "%02d:%02d", sg_time_h, sg_time_m);
    }
    if (NULL != sg_time_hv) {
        lv_label_set_text_fmt(sg_time_hv, "%02d", sg_time_h);
    }
    if (NULL != sg_time_mv) {
        lv_label_set_text_fmt(sg_time_mv, "%02d", sg_time_m);
    }
}

static void prv_time_hour_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);

    sg_time_h = (sg_time_h + d + 24) % 24;
    prv_time_show();
}

static void prv_time_min_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);

    sg_time_m = (sg_time_m + d + 60) % 60;
    prv_time_show();
}

static void prv_time_save_cb(lv_event_t *e)
{
    APP_DATETIME_T now;

    (void)e;

    if (OPRT_OK == app_time_now(&now)) {
        now.hour = sg_time_h;
        now.min  = sg_time_m;
        now.sec  = 0;
        if (OPRT_OK == app_time_set_local(&now)) {
            sg_clock_last[0] = '\0';        /* 强制下一次 tick 重写时钟 */
            prv_clock_refresh();
        }
    }
    prv_time_close();
}

/* 一行：标签 + [-] + 数值 + [+] */
static void prv_time_row(lv_obj_t *win, int y, const char *caption,
                         lv_event_cb_t cb, lv_obj_t **out_val)
{
    lv_obj_t *l = app_ui_label(win, caption, UI_FONT_CN, SB_COL_DIM);

    lv_obj_set_pos(l, 46, y + 4);

    lv_obj_t *minus = app_ui_button(win, 32, 28, 0x1B2434, "-", UI_FONT_21,
                                    SB_COL_ICON, cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(minus, 86, y);

    lv_obj_t *plus = app_ui_button(win, 32, 28, 0x1B2434, "+", UI_FONT_21,
                                   SB_COL_ICON, cb, (void *)(intptr_t)1);
    lv_obj_set_pos(plus, 230, y);

    *out_val = app_ui_label(win, "00", UI_FONT_21, SB_COL_ICON);
    lv_obj_set_width(*out_val, 70);
    lv_obj_set_style_text_align(*out_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*out_val, 140, y + 2);
}

static void prv_time_open(void)
{
    APP_DATETIME_T now;
    char           date[40];

    if (NULL != sg_time_win) {          /* 再点一次 = 收起 */
        prv_time_close();
        return;
    }
    prv_close_all_popups();
    prv_mask_show();                    /* 先把遮罩铺上，窗口才在它上面 */

    memset(&now, 0, sizeof(now));
    (void)app_time_now(&now);
    sg_time_h = (now.hour >= 0 && now.hour < 24) ? now.hour : 0;
    sg_time_m = (now.min >= 0 && now.min < 60) ? now.min : 0;

    sg_time_win = prv_window("时间设置", 340, 226, prv_time_close_cb);

    /* 日期：只读。没同步时给一句提示，别显示 1970。 */
    if (app_time_valid() &&
        OPRT_OK == app_time_format(date, sizeof(date), "%Y年%-m月%-d日 星期%a")) {
        sg_time_date = app_ui_label(sg_time_win, date, UI_FONT_CN, SB_COL_DIM);
    } else {
        sg_time_date = app_ui_label(sg_time_win, "时间未同步（联网后自动校准）",
                                    UI_FONT_CN, SB_COL_DIM);
    }
    lv_obj_set_width(sg_time_date, 316);
    lv_obj_set_style_text_align(sg_time_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sg_time_date, 12, 40);

    /* 大字时间 */
    sg_time_big = app_ui_label(sg_time_win, "00:00", UI_FONT_21, SB_COL_ACCENT);
    lv_obj_set_width(sg_time_big, 316);
    lv_obj_set_style_text_align(sg_time_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sg_time_big, 12, 66);

    prv_time_row(sg_time_win, 112, "时", prv_time_hour_cb, &sg_time_hv);
    prv_time_row(sg_time_win, 148, "分", prv_time_min_cb, &sg_time_mv);

    /* 取消 / 保存 */
    lv_obj_t *cancel = app_ui_button(sg_time_win, 92, 34, 0x1B2434, "取消",
                                     UI_FONT_CN, SB_COL_ICON,
                                     prv_time_close_cb, NULL);
    lv_obj_set_pos(cancel, 96, 186);

    lv_obj_t *save = app_ui_button(sg_time_win, 92, 34, SB_COL_ACCENT, "保存",
                                   UI_FONT_CN, 0xFFFFFF,
                                   prv_time_save_cb, NULL);
    lv_obj_set_pos(save, 200, 186);

    prv_time_show();
    PR_NOTICE("[bar] 时间窗打开");
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
    /* 这个窗是直接建在屏幕上的（没走 prv_window），同样要脱离屏幕的 flex 布局，
     * 否则会被排到 shell 下面 —— 屏幕外，看不见。 */
    lv_obj_add_flag(sg_wifi_win, LV_OBJ_FLAG_IGNORE_LAYOUT);
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

/* 左上角的备忘录徽标：点一下直接进备忘录列表。 */
static void prv_memo_badge_cb(lv_event_t *e)
{
    (void)e;
    pet_ui_go(PET_PAGE_MEMOS);
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
    sg_time_win  = NULL;
    sg_time_big  = NULL;
    sg_time_date = NULL;
    sg_time_hv   = NULL;
    sg_time_mv   = NULL;
    sg_wifi_win  = NULL;
    sg_wifi_kb   = NULL;
    sg_wifi_ta_ssid = NULL;
    sg_wifi_ta_pwd  = NULL;
    sg_wifi_status  = NULL;
    sg_clock     = NULL;
    sg_badge     = NULL;
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

    if (sg_wea_dirty || sg_font_dirty) {
        sg_wea_dirty = FALSE;
        prv_weather_refresh();
    }

    /* 强制重写这一拍已经做完，后面的 refresh 恢复"内容没变就不动"。 */
    sg_font_dirty = FALSE;
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
     * 纯内存读，没有 IO），不需要事件驱动 —— 所以这里只接天气和字库。 */
    if (APP_EVT_WEATHER_UPDATED == evt) {
        sg_wea_dirty = TRUE;
    } else if (APP_EVT_FONT_READY == evt) {
        sg_font_dirty = TRUE;
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

    /* 弹窗建在**屏幕**上，不是父容器上：新 UI 的父容器（shell / body）是
     * flex 布局，往里面塞一个 `lv_obj_center()` 的窗口会被 flex 当成普通子项
     * 排进队列，既不会居中也不会盖满屏。屏幕不是布局容器，坐标才有意义。 */
    sg_root = lv_obj_get_screen(parent);

    /* 上一次的状态栏还活着（同一个屏幕重建）就先删掉 —— 它的 DELETE 回调会把
     * 定时器和所有指针清干净，所以下面可以直接重来。**屏幕被删过的话 sg_bar
     * 已经是 NULL**（prv_bar_deleted_cb 清的），不会摸到野指针。 */
    if (NULL != sg_bar) {
        lv_obj_del(sg_bar);
    }
    sg_clock_last[0] = '\0';
    sg_wea_code = -1;

    sg_bar = lv_obj_create(parent);
    lv_obj_set_size(sg_bar, LV_PCT(100), APP_STATUSBAR_H);
    /* **不铺底色、不画分隔线**：新 UI 的顶栏就是一条字，坐在页面底色上。 */
    lv_obj_set_style_bg_opa(sg_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sg_bar, 0, 0);
    lv_obj_set_style_radius(sg_bar, 0, 0);
    lv_obj_set_style_pad_hor(sg_bar, PET_PAD_PAGE + 4, 0);
    lv_obj_set_style_pad_ver(sg_bar, 0, 0);
    lv_obj_set_style_pad_column(sg_bar, 6, 0);
    lv_obj_set_flex_flow(sg_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sg_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(sg_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sg_bar, prv_bar_deleted_cb, LV_EVENT_DELETE, NULL);

    /* ---- 左：备忘录数（可点，进备忘录列表）---- */
    {
        lv_obj_t *memo = lv_button_create(sg_bar);

        lv_obj_set_size(memo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(memo, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(memo, 0, 0);
        lv_obj_set_style_pad_all(memo, 2, 0);
        sg_badge = prv_text(memo, "备忘录 0", SB_COL_DIM);
        lv_obj_add_event_cb(memo, prv_memo_badge_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 弹簧：把下面这些推到右边。 */
    pui_spacer(sg_bar);

    /* ---- 右：天气 · 时间 · WiFi · 音量 · 电量 ----
     *
     * 天气图标用 SD 卡上 white/1x 那套（浅色/彩色），**不重着色** —— 顶栏是深色底，
     * 白色云彩和黄色太阳正是它设计时用的底色。 */
    {
        lv_obj_t *wea = lv_button_create(sg_bar);

        lv_obj_set_size(wea, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(wea, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wea, 0, 0);
        lv_obj_set_style_pad_all(wea, 0, 0);
        lv_obj_set_flex_flow(wea, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wea, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(wea, 2, 0);

        sg_wea_icon = lv_image_create(wea);
        lv_image_set_scale(sg_wea_icon, SB_WICON_SCALE);
        lv_image_set_antialias(sg_wea_icon, true);   /* 48 -> 24 的降采样 */
        lv_obj_set_size(sg_wea_icon, SB_WICON_PX, SB_WICON_PX);   /* 见 SB_WICON_PX 的说明 */

        sg_wea_text = prv_text(wea, "--°", SB_COL_ICON);
        lv_obj_add_event_cb(wea, prv_wea_click, LV_EVENT_RELEASED, NULL);
    }

    {
        lv_obj_t *clk = lv_button_create(sg_bar);

        lv_obj_set_size(clk, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(clk, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clk, 0, 0);
        lv_obj_set_style_pad_all(clk, 2, 0);
        sg_clock = prv_text(clk, "--:--", SB_COL_ICON);
        lv_obj_add_event_cb(clk, prv_time_click, LV_EVENT_CLICKED, NULL);
    }

    sg_wifi_icon = prv_glyph(sg_bar, FONT_AWESOME_WIFI, SB_COL_DIM);
    lv_obj_add_event_cb(sg_wifi_icon, prv_wifi_icon_cb, LV_EVENT_CLICKED, NULL);

    {
        lv_obj_t *vol = prv_glyph(sg_bar, FONT_AWESOME_VOLUME_HIGH, SB_COL_ICON);

        lv_obj_add_event_cb(vol, prv_vol_icon_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 电量：图标 + 百分比贴在一起（它们是一件事的两个部分）。 */
    sg_bat_icon = prv_glyph(sg_bar, FONT_AWESOME_BATTERY_3, SB_COL_ICON);
    sg_bat_text = prv_text(sg_bar, "--", SB_COL_ICON);

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
    app_event_subscribe(APP_EVT_FONT_READY, prv_evt_cb, NULL);

    PR_NOTICE("[bar] 状态栏就绪（%dx%d）", SB_W, APP_STATUSBAR_H);
    return sg_bar;
}

void app_statusbar_set_todo(uint32_t pending)
{
    char text[24];

    if (NULL == sg_badge) {
        return;
    }

    lv_snprintf(text, sizeof(text), "备忘录 %u", (unsigned)pending);
    if (0 == strcmp(lv_label_get_text(sg_badge), text)) {
        return;
    }
    lv_label_set_text(sg_badge, text);
}

void app_statusbar_open_time(void)
{
    app_disp_lock();
    prv_time_open();
    app_disp_unlock();
}

BOOL_T app_statusbar_popup_visible(void)
{
    /* 免锁：只读一个 volatile，LVGL 定时器里也能调。 */
    return sg_popup_on;
}

BOOL_T app_statusbar_popup_active(void)
{
    BOOL_T on;

    app_disp_lock();
    on = (BOOL_T)((NULL != sg_mask) || (NULL != sg_vol_win) ||
                  (NULL != sg_wifi_win) || (NULL != sg_wea_win) ||
                  (NULL != sg_time_win) || (NULL != sg_set_win));
    app_disp_unlock();
    return on;
}

BOOL_T app_statusbar_close_popup(void)
{
    BOOL_T had;

    app_disp_lock();
    had = (BOOL_T)((NULL != sg_vol_win) || (NULL != sg_wifi_win) ||
                   (NULL != sg_wea_win) || (NULL != sg_time_win) ||
                   (NULL != sg_set_win) || (NULL != sg_mask));
    prv_close_all_popups();
    if (NULL != sg_mask) {
        /* 只剩遮罩（窗口已经没了）的边角情况 */
        prv_mask_hide();
    }
    app_disp_unlock();
    return had;
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
