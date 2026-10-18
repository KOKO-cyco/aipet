/**
 * @file page_pet.c
 * @brief 桌宠主界面：一只宠物 + 一块"它拿给你看的东西"（480x320 横屏摆件）。
 *
 * 这一页是**开机就进的唯一主界面**（`app_router_prepare(PAGE_PET)`）。
 * 顶上那条状态栏是它的一个控件（见 app_statusbar.h），不是页面。
 *
 * 布局
 * ----
 *   **顶部 480x32  状态栏**  WiFi / 天气 / 时间 / AI 音量 / 电量
 *   **左下 240x288 宠物**   —— 按**原点**摆：脚底中点落在内容区中心再下移一点
 *   **右下 240x288 备忘录** —— 待机那行淡字 / 记忆列表 / 确认卡 / 想起的问句
 *
 * 宠物只在自己的半屏里居中，**不去占整屏的中心** —— 那是备忘录的地方。
 *
 * 设计立场（决定了下面每一个像素）
 * --------------------------------
 *   别人做的是"带宠物的备忘录"，这里做的是"**一个有记性的活物**"：
 *     备忘录是"你查它"，宠物是"**它记着你**"。
 *
 *   推论三条：
 *     ① **没有"页面"，只有"它拿给你看的东西"** —— 宠物一直在场，右半屏
 *        换内容。所以四个视图共用同一个壳，没有任何导航栈。
 *     ② **每条记忆都有"口气"** —— 不是 `2026-10-16 15:00 交房租`，而是
 *        `交房租 / 今天 15:00 · 你 10月12日 跟我说的`。后半句是整个产品的灵魂：
 *        它把"一条记录"变成"它记得这件事"，成本只是一个字段。
 *     ③ **它会"想起"，不只是"提醒"** —— 提醒是你设了时间到点它说；想起是你
 *        **没设**它也记着。两者界面上必须长得不一样：提醒是陈述，想起**带问号**。
 *
 * ⚠️ 备忘录那半边**现在是原型**
 * ---------------------------
 * `sg_memos[]` 是写死的三条假数据，视图（列表 / 捕获卡 / 想起）是**设计验证**，
 * 不是产品代码。**备忘录由别人接**，接的时候换掉数据层、保留这套视图即可。
 * 交互上现在全用触摸触发（没有语音入口）—— 真接上 ASR 时把捕获流程挂到
 * `APP_EVT_AI_*` 上，见下面的动作状态机。
 *
 * 怎么演
 * ------
 *   点宠物        -> **AI 在思考/说话时 = 打断**；其余时候演一个"点击回应" + 弹捕获卡
 *   （**不做拖拽**：这一页的宠物是画布里的，本来就拖不动）
 *   点那行淡字    -> 打开记忆列表
 *   点列表里的行  -> 切换完成状态（看"还给你"的两种样子）
 *   待机 12 秒    -> 自动演一次"想起"（**这是最该看的一段**）
 *   P12 短按      -> 打断 AI（和点宠物同一个动作，见 prv_pet_key）
 *
 * 动作状态机（**不是**原型那套循环轮播）
 * --------------------------------------
 *   待机   ：循环播 idle 组；每 6 秒随机插播一个小动作
 *   持续   ：AI 思考时一直演 think、说话时一直演 talk（"碎碎念"），状态一变就跟着换
 *   插播中 ：播完这一轮就回"当前该演的"（有持续动作就是它，否则待机）
 *
 * 三条路触发，**回调里只写请求位**，真正的切换发生在 tick 的帧边界上：
 *   APP_EVT_PET_ACTION   大模型点名动作（MCP 工具）      —— 一次性
 *   APP_EVT_AI_EMOTION   AI 情绪（云端 NLG / MCP）      —— 一次性
 *   APP_EVT_AI_STATE     AI 状态（THINK / SPEAK）       —— **持续**
 *   点宠物                                               —— 一次性
 *
 * "持续"优先于"一次性"：AI 一开始说话就立刻切过去，正在演的随机动作让位。
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>            /* strncasecmp —— 情绪名比对用（云端大小写不保证） */

#include "tal_api.h"
#include "lvgl.h"

#include "ai_manage_mode.h"     /* AI_MODE_STATE_* —— AI 状态驱动动作 */

#include "app_ai_chat.h"
#include "app_disp.h"
#include "app_event.h"
#include "app_font.h"
#include "app_pages.h"
#include "app_router.h"
#include "app_sprite2d.h"
#include "app_statusbar.h"
#include "app_theme.h"

/***********************************************************
************************* 配色 *****************************
***********************************************************/
/* 整体偏暗、低对比。**故意不鲜艳** —— 一个摆在桌上的活物不该抢眼，
 * 抢眼的是工具。 */
#define PET_COL_BG        0x121822      /* 页面底色（也是宠物画布的底色） */
#define PET_COL_PANEL     0x1B232F      /* 卡片底 */
#define PET_COL_TEXT      0xE4EBF5      /* 主文字 */
#define PET_COL_SUB       0x8C9AAC      /* 次要文字 */
#define PET_COL_FAINT     0x55637A      /* "口气"那行 / 待机那行淡字 */
#define PET_COL_ACCENT    0x4DA3FF      /* 强调（时间、主按钮） */
#define PET_COL_DONE      0x44516A      /* 已完成 */
#define PET_COL_BTN       0x27313F      /* 次按钮底 */
#define PET_COL_BTN_HI    0x2F6EA8      /* 主按钮底 */

/* PET_COL_BG 的 **RGB565** 版本（0x121822 -> R2/G6/B4）。
 * 宠物画布的底色参数要的是 uint16 的 565，不是 24 位色值。 */
#define PET_COL_BG565     0x10C4

/***********************************************************
************************* 几何 *****************************
***********************************************************/
/* 逻辑画布 480x320（面板 320x480 转 90°）。
 *
 * **顶上一条 32px 是状态栏**（app_statusbar），内容区从它下面开始：
 *   左半 240x288  宠物
 *   右半 240x288  备忘录 / 对话
 *
 * 宠物只在自己的半屏里居中，**不去占整屏的中心** —— 那是备忘录的地方。 */
#define PET_W             480
#define PET_H             320
#define PET_HALF_W        (PET_W / 2)

#define PET_BODY_Y        APP_STATUSBAR_H            /* 32 */
#define PET_BODY_H        (PET_H - PET_BODY_Y)       /* 288 */

#define PET_INFO_X        PET_HALF_W
#define PET_INFO_W        (PET_W - PET_HALF_W)
#define PET_PAD           10

/* 每组动作播多久。固定 24fps（素材原速）；1 帧的组给个最短时长，
 * 否则 snap / drag_hold 这种一闪而过根本看不见。 */
#define PET_FRAME_MS      42
#define PET_CLIP_MIN_MS   900

/* **原点**（脚底中点）落在屏幕上的位置：左半屏的中心，垂直方向再**下移**一点。
 *
 * 摆宠物按**原点**摆，不按锚点框摆 —— 锚点框是"所有动作的并集"，它的中心
 * 和角色的原点差了十几像素（并集右边比左边空得多），按框摆角色会偏。
 *
 * PET_ORIGIN_DROP 就是"下移多少"这个旋钮：视觉上宠物偏高时调大它，调多少看
 * 屏幕就行（0 = 回到内容区的垂直居中）。注意它是**加在内容区中心之上**的。 */
#define PET_ORIGIN_DROP   24
#define PET_ORIGIN_X      (PET_HALF_W / 2)
#define PET_ORIGIN_Y      (PET_BODY_Y + PET_BODY_H / 2 + PET_ORIGIN_DROP)

/** 待机多久之后自动演一次"想起"。原型里短一点，方便看。 */
#define PET_RECALL_AFTER_MS   12000u

/** 待机时每隔多久随机演一个小动作（真产品里由状态机决定，这里先让它有生气）。 */
#define PET_IDLE_ACT_AFTER_MS 6000u

/** 宠物动画刷新间隔。
 *
 * **必须等于 PET_FRAME_MS（42）**：一拍正好一帧，节奏才是均匀的 24fps。
 *
 * 以前这里是 33（30Hz），而帧长是 42ms —— 帧号按 (now - sg_clip_at) / 42 算，
 * 于是每拍的取帧结果是 0,0,1,2,3,3,4,5,6,7,7…：**每 3~4 帧就有一次 66ms 的
 * 重复帧**。平时看着像轻微抖动，切动作时最明显 —— 新组的第 0 帧正好落在那个
 * 重复位上，先多停一拍才开始动，就是用户反馈的"切换的时候卡一下"。
 * （实测日志：切换后第 1 拍距上一拍 32~36ms，而正常拍 33ms —— 停顿不在读帧，
 *  就在这个节奏量化上。） */
#define PET_TICK_MS           42

/** 待机组的组号。素材约定第 0 组必须是 idle（见 tools/build_fish.sh 的 --order）。 */
#define PET_CLIP_IDLE         0

/***********************************************************
************************ 动作巡检 *****************************
***********************************************************/
/* **只为核对素材存在**：开机后按「待机、待机、动作、待机、待机、动作……」的节奏
 * 把每个非待机动作各放**一次**，最后一组后面不补待机，然后从头再来。
 * 串口打 `[AUDIT] 3/15 表情-伤心（emo_sad）130 帧`，右半屏同时显示
 * "3/15 + 中文名"，不用回头翻日志对时间。
 *
 * 打开时**整页只剩巡检**：随机插播 / 想起 / 点击切动作全部让位（点击只打一行
 * 日志），否则动作会被打断，没法逐组核对。核完把这里置 0 就是正常桌宠 ——
 * 状态机代码一行没动，只是在它之前 return。
 *
 * 加/删素材后**这张表和 ELF_ORDER 要同序**：动作序号是按表里的顺序数的，
 * 错位不影响播放（按名字找组），但屏幕上的 "i/15" 会和实际播放的组对不上。 */
#define PET_ACT_AUDIT      1

/** 每组放几遍。**1 = 只放一次**（用户核对用，放两遍反而要等）。 */
#define PET_AUDIT_LOOPS    1

/** 两个非待机动作之间夹几个待机 —— 用户要的节奏是
 *  「待机-待机-开心-待机-待机-伤心」，即夹 2 个。 */
#define PET_AUDIT_GAP      2

/** 巡检表：按打包顺序（= tools/build_fish.sh 的 ELF_ORDER）列全部组。
 * 第 0 条必须是 idle（既当夹在中间的空档，又是动作序号 0 的基准）。
 * label 是给人看的中文名，和源视频文件名一一对应。 */
typedef struct {
    const char *clip;
    const char *label;
} PET_AUDIT_T;

static const PET_AUDIT_T sg_audit[] = {
    { "idle",       "待机"          },
    { "tap_proud",  "点击-傲娇生气" },
    { "tap_shy",    "点击-害羞惊讶" },
    { "tap_happy",  "点击-开心跃动" },
    { "tap_tickle", "点击-挠痒咯咯笑" },
    { "fn_photo",   "拍照"          },
    { "fn_note",    "记笔记"        },
    { "emo_sad",    "表情-伤心"     },
    { "emo_happy",  "表情-开心"     },
    { "emo_think",  "表情-思考"     },
    { "rnd_gomoku", "随机-下五子棋" },
    { "rnd_pigeon", "随机-变鸽子"   },
    { "rnd_yawn",   "随机-哈欠连天" },
    { "rnd_cat",    "随机-撸猫"     },
    { "rnd_lion",   "随机-舞狮头"   },
    { "rnd_scare",  "随机-被吓一跳" },
};

#define PET_AUDIT_N  (int)(sizeof(sg_audit) / sizeof(sg_audit[0]))

/** 非待机动作数（表里除第 0 条 idle 之外的全部）与整条播放序列的步数。
 *  15 个动作 ×（2 个待机 + 1 个动作）= 45 步，最后一步是最后一个动作，
 *  后面不补待机。 */
#define PET_AUDIT_N_ACT  (PET_AUDIT_N - 1)
#define PET_AUDIT_STEPS  (PET_AUDIT_N_ACT * (PET_AUDIT_GAP + 1))

/***********************************************************
********************** 持续动作的循环区间 ********************
***********************************************************/
/* 持续动作（AI 思考 / 说话）要**循环几十秒**，而整条动作循环回去时首尾姿态
 * 差得远 —— 接缝处会瞬移/抽动。所以只循环其中一段 [first, last]：这一段的首尾
 * 姿态和运动方向都接得上。
 *
 * **帧号由 tools/find_loop.py 算出，不要手调**（它把"接缝跳变"和"正常播下一帧"
 * 做比较，比肉眼靠谱，换素材也能重跑）：
 *
 *     python tools/find_loop.py third_party/resources/fish_seq/talk
 *
 * `frames` 是**打包时这条动作的总帧数**，用来做一致性检查：对不上就说明素材
 * 重打过（改了 --order / fps / 剪辑），帧号已经失效，必须重跑上面那条命令。
 *
 * 没在这里列出的组按**整条**循环（render_clip 自己取模），idle 就是这种。 */
typedef struct {
    const char *clip;
    int         first;      /* 循环起始帧（含），0 基 */
    int         last;       /* 循环结束帧（含） */
    int         frames;     /* 打包时的总帧数（一致性检查用） */
} PET_LOOP_T;

static const PET_LOOP_T sg_loop[] = {
    /* **空表（2026-10-17，第三套动作集）**：5 条动作里没有"要按子区间循环"的
     * 组了 —— think / talk 随旧素材一起没了，剩下的 click_wave / click_happy /
     * rnd_photo / rnd_note 都是一次性表演，整条播完回待机。
     *
     * 表不能是 0 长度（C 不允许），留一条 `strcmp` 永远匹配不上的哨兵行；消费处
     * （prv_loop_frame）本来就是"匹配不上 = 没有循环区间"。以后哪个动作要加循环
     * 段：重跑 tools/find_loop.py，把 { 名, 起, 止, 总帧 } 换成真数据。 */
    { "", 0, 0, 0 },
};

/***********************************************************
************************ 假数据 *****************************
***********************************************************/
/* 原型里的记忆是写死的 —— 真接上后端时这里换成存储层。
 * `source` 就是差异化那条：**它是怎么知道的**。 */
typedef struct {
    const char *what;       /* 事项 */
    const char *when;       /* 时间（人话，不是时间戳） */
    const char *source;     /* 它是怎么知道的 */
    BOOL_T      done;
} PET_MEMO_T;

static PET_MEMO_T sg_memos[] = {
    { "交房租",       "今天 15:00", "你 10月12日 跟我说的", FALSE },
    { "给妈妈打电话",  "明天 09:00", "你让我提醒你的",       FALSE },
    { "买猫粮",       "10月12日",   "你那天顺口提的",        TRUE  },
};

#define PET_MEMO_N  (int)(sizeof(sg_memos) / sizeof(sg_memos[0]))

/** 原型里"捕获"演示用的假输入 —— 真接上就是 ASR 的识别结果。 */
#define PET_DEMO_UTTER   "我明天下午三点得交房租"

/** 点宠物时插播的动作 —— **第三套动作集（2026-10-17）里除 idle 外的全部 4 条**：
 * 元气挥手 / 开心跃动 / 拍照 / 记笔记
 * （源在 third_party/新动作视频/，名字映射见 tools/fish_extract.py 的 CLIPS 表）。
 *
 * **别写死成一个**：以前这里只有 `"click_happy"`，于是不管怎么点都是同一套
 * 动作，别的永远不会出现。现在随机挑，且**避开上一次刚演过的那一个**。 */
#if !PET_ACT_AUDIT
static const char *const sg_tap_acts[] = {
    "click_wave", "click_happy", "rnd_photo", "rnd_note",
};
#endif

/***********************************************************
************************* 视图 *****************************
***********************************************************/
typedef enum {
    PET_VIEW_IDLE = 0,      /* 宠物 + 一行淡字 */
    PET_VIEW_LIST,          /* 记忆列表 */
    PET_VIEW_CAPTURE,       /* 捕获确认卡 */
    PET_VIEW_RECALL,        /* "它会想起" */
} PET_VIEW_E;

/***********************************************************
************************* 状态 *****************************
***********************************************************/
static lv_obj_t   *sg_scr;
static lv_obj_t   *sg_stage;        /* 宠物画布 */
static lv_obj_t   *sg_info;         /* 顶部信息带（换内容只动它） */
static lv_timer_t *sg_tick;
static uint16_t   *sg_cbuf;         /* 画布像素缓冲（PSRAM） */
static int         sg_cw, sg_ch;    /* 画布尺寸 = 精灵锚点框 */
static PET_VIEW_E  sg_view;
static uint32_t    sg_view_at;      /* 进当前视图的时刻 */
static BOOL_T      sg_recalled;     /* 这一轮待机里"想起"演过了没有 */

/* ---- 动作状态机 ----
 *
 *   待机   ：循环播 idle 组；每隔 PET_IDLE_ACT_AFTER_MS 随机插播一个小动作
 *   插播中 ：播完这一轮就回待机
 *
 * 三条路触发切换，**回调里只写请求位**，真正的切换发生在 tick 的帧边界上
 * （事件是在发布者线程同步分发的，那里碰 LVGL 会炸）：
 *
 *   APP_EVT_PET_ACTION   大模型点名动作（MCP 工具，见 app_avatar_tools.c）—— 一次性
 *   APP_EVT_AI_EMOTION   AI 情绪（云端 NLG 标签 / MCP 情绪工具）        —— 一次性
 *   APP_EVT_AI_STATE     AI 状态（思考 / 说话）                        —— **持续**
 *
 * "持续"和"一次性"的区别是这一页的关键：
 *   思考中 -> 一直演 think，直到状态变了
 *   说话中 -> 一直演 talk（"碎碎念"），直到说完
 * 一次性动作（点名 / 情绪）插播完会**回到当前的持续动作**（有的话），否则回待机。
 */
static int      sg_clip;         /* 当前正在播的组 */
static uint32_t sg_clip_at;      /* 本组开始的时刻 */
static BOOL_T   sg_clip_once;    /* TRUE = 播完这一轮就回"当前该演的" */

/* 本组已经播到第几帧（**计数器，不按墙钟算**，见 prv_tick_cb 里的说明）。
 * sg_frame_clip / sg_frame_at 用来判断"是不是换了组 / 同一组被重新点名"，
 * 两者任一变化就把帧号归零。 */
static uint32_t sg_frame_idx;
static int      sg_frame_clip = -1;
static uint32_t sg_frame_at;
static uint32_t sg_idle_at;      /* 上次回到待机的时刻（决定下次随机插播） */

/** AI 要求"持续演某一组"（思考 / 说话）；-1 = 没有，按待机走。 */
static int sg_hold_clip = -1;

/** 请求插播的组号；-1 = 没有请求。跨线程写一个 int 是原子的，不需要锁。 */
static volatile int sg_req_clip = -1;

/** 请求改"持续动作"：-2 = 没变化，-1 = 清掉（回待机），>=0 = 持续演这一组。 */
static volatile int sg_req_hold = -2;

/** AI 当前状态（AI_MODE_STATE_E），点击宠物时用来判断"这一下是不是打断"。 */
static volatile int sg_ai_state = AI_MODE_STATE_IDLE;

/***********************************************************
********************** 前向声明 ******************************
***********************************************************/
static void prv_set_view(PET_VIEW_E v);
#if !PET_ACT_AUDIT
static int  prv_pick_tap_act(void);     /* 定义在下面的"动作状态机"一节 */
#endif

/***********************************************************
*********************** 小工具 ******************************
***********************************************************/
static int prv_pending(void)
{
    int n = 0;

    for (int i = 0; i < PET_MEMO_N; i++) {
        if (!sg_memos[i].done) {
            n++;
        }
    }
    return n;
}

/** 一个圆角小色块（当"完成"圆点用 —— 字体里没有 ⏰/✓ 这类符号）。 */
static lv_obj_t *prv_dot(lv_obj_t *parent, int x, int y, int d, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *prv_label(lv_obj_t *parent, int x, int y, int w,
                           const char *txt, uint32_t col, const lv_font_t *font)
{
    lv_obj_t *l = app_ui_label(parent, txt, font, col);

    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

/** 软按钮：低对比圆角块 + 居中文字。 */
static lv_obj_t *prv_button(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t bg, const char *txt, uint32_t fg,
                            lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);

    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_center(app_ui_label(b, txt, UI_FONT_CN, fg));

    if (NULL != cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

/** 清空信息带，准备换内容。 */
static void prv_info_reset(void)
{
    if (NULL != sg_info) {
        lv_obj_clean(sg_info);
    }
}

/***********************************************************
*********************** 视图：待机 ***************************
***********************************************************/
/* 待机界面上**只有两样东西**：宠物，和一行极淡的字。
 *
 * 那行字同时做两件事：告诉用户"它有活干"，和强调"**这是它的事，不是你的**"
 * —— 主语是"它"，不是"你"（不是"你有 3 个待办"）。 */
static void prv_idle_click(lv_event_t *e)
{
    (void)e;
    prv_set_view(PET_VIEW_LIST);        /* 点那行字 = "我记了些什么" */
}

static void prv_show_idle(void)
{
    int  n = prv_pending();
    char buf[64];
    lv_obj_t *l;

    prv_info_reset();

    if (n > 0) {
        snprintf(buf, sizeof(buf), "今天它替你记着 %d 件事", n);
    } else {
        snprintf(buf, sizeof(buf), "今天没什么要提醒你的");
    }

    /* 右半屏的**垂直中心** —— 那一半只有这一行字，别把它顶到角落去。
     * （用内容区高度算，不是整屏 —— 顶上还有状态栏。） */
    l = prv_label(sg_info, PET_PAD, PET_BODY_H / 2 - 20, PET_INFO_W - PET_PAD * 2,
                  buf, PET_COL_FAINT, UI_FONT_CN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, prv_idle_click, LV_EVENT_CLICKED, NULL);
}

/***********************************************************
*********************** 视图：列表 ***************************
***********************************************************/
static void prv_back_click(lv_event_t *e)
{
    (void)e;
    prv_set_view(PET_VIEW_IDLE);
}

/* 每条记忆两行：
 *     交房租
 *     今天 15:00 · 你 10月12日 跟我说的
 * 第二行的后半句是差异化 —— 它把"一条记录"读成"它记得这件事"。 */
static void prv_row_click(lv_event_t *e)
{
    PET_MEMO_T *m = (PET_MEMO_T *)lv_event_get_user_data(e);

    if (NULL != m) {
        m->done = (BOOL_T)(!m->done);   /* 原型里点一下就切换，方便看两种样子 */
    }
    prv_set_view(PET_VIEW_LIST);
}

static void prv_show_list(void)
{
    int inner = PET_INFO_W - PET_PAD * 2;
    int y;

    prv_info_reset();

    /* 头部刻意**不做"标题栏"** —— 那太像 App 了。只有一行小字和一个"收起"。 */
    prv_label(sg_info, PET_PAD, 4, 140, "它替你记着", PET_COL_SUB, UI_FONT_CN);
    {
        lv_obj_t *x = prv_label(sg_info, PET_INFO_W - PET_PAD - 40, 4, 40,
                                "收起", PET_COL_FAINT, UI_FONT_CN);

        lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(x, prv_back_click, LV_EVENT_CLICKED, NULL);
    }

    y = 30;

    for (int i = 0; i < PET_MEMO_N; i++) {
        PET_MEMO_T *m = &sg_memos[i];
        lv_obj_t   *row = lv_obj_create(sg_info);

        lv_obj_set_size(row, inner, 62);
        lv_obj_set_pos(row, PET_PAD, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, prv_row_click, LV_EVENT_CLICKED, m);

        /* 左边一个小圆点：亮 = 还没做，暗 = 做完了 */
        prv_dot(row, 2, 8, 8, m->done ? PET_COL_DONE : PET_COL_ACCENT);

        /* 三行：事项 / 时间 / **它是怎么知道的**。
         * 第三行是差异化 —— 它把"一条记录"读成"它记得这件事"。 */
        prv_label(row, 18, 0, inner - 20, m->what,
                  m->done ? PET_COL_DONE : PET_COL_TEXT, UI_FONT_CN);
        prv_label(row, 18, 20, inner - 20, m->when,
                  m->done ? PET_COL_DONE : PET_COL_ACCENT, UI_FONT_CN);
        prv_label(row, 18, 40, inner - 20, m->source, PET_COL_FAINT, UI_FONT_CN);

        y += 66;
    }
}

/***********************************************************
*********************** 视图：捕获 ***************************
***********************************************************/
/* 抽错时间是整条链路里错误率最高的一环，而错的时间是**静默失败**
 * （不会报错，只是永远不响）。所以捕获必须回显确认。
 *
 * 卡片里时间用**大字** —— 最容易抽错的字段最显眼。 */
static void prv_done_click(lv_event_t *e)
{
    (void)e;
    prv_set_view(PET_VIEW_IDLE);
}

static void prv_show_capture(void)
{
    lv_obj_t *card;
    int inner = PET_INFO_W - PET_PAD * 2;

    prv_info_reset();

    prv_label(sg_info, PET_PAD, 4, 140, "你是说——", PET_COL_SUB, UI_FONT_CN);

    card = lv_obj_create(sg_info);
    lv_obj_set_size(card, inner, 150);
    lv_obj_set_pos(card, PET_PAD, 30);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(PET_COL_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间：大字。抽错的就是它。 */
    prv_label(card, 12, 12, inner - 24, "明天 15:00", PET_COL_ACCENT, UI_FONT_21);
    prv_label(card, 12, 50, inner - 24, "交房租", PET_COL_TEXT, UI_FONT_CN);
    prv_label(card, 12, 76, inner - 24, PET_DEMO_UTTER, PET_COL_FAINT, UI_FONT_CN);

    prv_button(card, 12, 108, 92, 32, PET_COL_BTN_HI, "对", 0xFFFFFF,
               prv_done_click, NULL);
    prv_button(card, 114, 108, 92, 32, PET_COL_BTN, "改", PET_COL_SUB,
               NULL, NULL);
}

/***********************************************************
*********************** 视图：想起 ***************************
***********************************************************/
/* **这是整个产品的杀手锏，也是最该看的一段。**
 *
 * 和"提醒"的区别：
 *   提醒 = 你设了时间，到点它**陈述**："该交房租了。"
 *   想起 = 你**没设**，它自己记着，并且**带问号**："你提过三次去看牙医了，
 *          要不要我记着？"
 *
 * 界面上必须长得不一样：想起是**问句 + 两个按钮**，不是通知。 */
static void prv_show_recall(void)
{
    int inner = PET_INFO_W - PET_PAD * 2;

    prv_info_reset();

    prv_label(sg_info, PET_PAD, 80, inner,
              "你提过三次「去看牙医」了，", PET_COL_TEXT, UI_FONT_CN);
    prv_label(sg_info, PET_PAD, 108, inner,
              "要不要我记着？", PET_COL_TEXT, UI_FONT_CN);

    prv_button(sg_info, PET_PAD, 180, 92, 32, PET_COL_BTN_HI,
               "好", 0xFFFFFF, prv_done_click, NULL);
    prv_button(sg_info, PET_PAD + 104, 180, 92, 32, PET_COL_BTN,
               "不用", PET_COL_SUB, prv_done_click, NULL);
}

/***********************************************************
*********************** 视图：切换 ***************************
***********************************************************/
static void prv_set_view(PET_VIEW_E v)
{
    sg_view    = v;
    sg_view_at = (uint32_t)tal_system_get_millisecond();

    switch (v) {
    case PET_VIEW_LIST:    prv_show_list();    break;
    case PET_VIEW_CAPTURE: prv_show_capture(); break;
    case PET_VIEW_RECALL:  prv_show_recall();  break;
    case PET_VIEW_IDLE:
    default:               prv_show_idle();    break;
    }
}

/***********************************************************
*********************** 动作状态机 ***************************
***********************************************************/
/* 注：这里原来有 prv_clip_ms()（"一组播完要多久" = 帧数 × 42ms）。
 * 2026-10-18 动作"演完"和巡检切换都改成看**帧号**（prv_tick_cb 的 ③ 与
 * prv_seq_len）—— 按墙钟算会在末尾多播几拍、回绕到第 0 帧，那正是切换处那一下卡。 */

/** 按名字找组号。找不到返回 -1（名字表就是 app_sprite2d_clip_name() 那份）。 */
static int prv_clip_by_name(const char *name)
{
    int n;

    if (NULL == name) {
        return -1;
    }
    n = app_sprite2d_clip_count();
    for (int i = 0; i < n; i++) {
        if (0 == strcmp(app_sprite2d_clip_name(i), name)) {
            return i;
        }
    }
    return -1;
}

/***********************************************************
************************* 动作巡检 ****************************
***********************************************************/
#if PET_ACT_AUDIT
/* 只推进"该播哪一组"；帧号推进和渲染仍走 tick 末尾那一份，不重复实现。 */

static int sg_audit_pos;    /* 巡检序列走到第几步（0..PET_AUDIT_STEPS-1） */

/** 右半屏显示当前这一拍：动作显示 "i/17 + 中文名 + 帧数"，待机间隔只写"待机"。 */
static void prv_audit_show(const char *label, int clip, int act_no)
{
    char      buf[40];
    lv_obj_t *l;
    uint32_t  ms = (uint32_t)app_sprite2d_clip_frames(clip) * PET_FRAME_MS;

    prv_info_reset();

    l = prv_label(sg_info, PET_PAD, 78, PET_INFO_W - PET_PAD * 2,
                  "动作巡检", PET_COL_FAINT, UI_FONT_CN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    if (act_no > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", act_no, PET_AUDIT_N_ACT);
        l = prv_label(sg_info, PET_PAD, 104, PET_INFO_W - PET_PAD * 2,
                      buf, PET_COL_ACCENT, UI_FONT_21);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

        l = prv_label(sg_info, PET_PAD, 146, PET_INFO_W - PET_PAD * 2,
                      label, PET_COL_TEXT, UI_FONT_CN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        l = prv_label(sg_info, PET_PAD, 116, PET_INFO_W - PET_PAD * 2,
                      "待机", PET_COL_SUB, UI_FONT_21);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }

    snprintf(buf, sizeof(buf), "%d 帧 · %u.%u 秒",
             app_sprite2d_clip_frames(clip),
             (unsigned)(ms / 1000u), (unsigned)((ms % 1000u) / 100u));
    l = prv_label(sg_info, PET_PAD, 176, PET_INFO_W - PET_PAD * 2,
                  buf, PET_COL_FAINT, UI_FONT_CN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

/** 巡检状态：每拍调一次。每一步放完就进下一步，走到头回到第 0 步。
 *
 * 序列（PET_AUDIT_GAP = 2）：
 *   待机、待机、动作1、待机、待机、动作2、…、待机、待机、动作17
 * 即第 pos 步 = pos % 3 == 2 ? 表里第 1+pos/3 条 : 第 0 条（待机）。
 * **最后一个动作后面不补待机** —— 所以总步数正好是 17×3。 */
static void prv_audit_state(uint32_t now)
{
    int n = app_sprite2d_clip_count();
    int entry = 0;
    int act_no;
    int clip;

    if (n <= 0) {
        return;
    }

    /* sg_clip < 0 = 刚进页面还没选组；否则看当前这一组**整条播完没有**。
     *
     * **判据是帧号，不是墙钟**（2026-10-18）。帧号由 tick 的计数器推进，每拍 +1；
     * 而实际拍间隔会因为读帧、重绘、WiFi 扫描抖动成 42~58ms 不等，墙钟比计数器
     * 走得快。按墙钟判，拍数富余时动作末尾会**再回绕到第 0 帧**播一拍才切走
     * （"演完突然跳回开头"），拍数不足时最后一帧又没演 —— 两种都看着像卡一下。
     * 待机→动作之所以不卡，是因为待机本来就在循环，回绕看不出来。
     *
     * 按帧号判：渲染完第 L-1 帧的下一拍就切，永远整条一次、不回绕。
     * sg_frame_idx 此刻还是**上一拍**渲染的帧号（本拍的 ++ 在下面）。 */
    if (sg_clip < 0 || sg_clip >= n ||
        (sg_frame_idx + 1u) >= (uint32_t)app_sprite2d_clip_frames(sg_clip)) {
        if (sg_clip >= 0 && sg_clip < n) {
            sg_audit_pos = (sg_audit_pos + 1) % PET_AUDIT_STEPS;
        }

        /* 素材里缺这一组就跳到下一步（某条视频没打包进去时别卡住） */
        clip = -1;
        for (int k = 0; k < PET_AUDIT_STEPS; k++) {
            int slot = sg_audit_pos % (PET_AUDIT_GAP + 1);

            entry = (PET_AUDIT_GAP == slot)
                    ? (1 + sg_audit_pos / (PET_AUDIT_GAP + 1))
                    : 0;
            clip  = prv_clip_by_name(sg_audit[entry].clip);
            if (clip >= 0) {
                break;
            }
            PR_WARN("[AUDIT] 素材里没有 %s，跳过", sg_audit[entry].clip);
            sg_audit_pos = (sg_audit_pos + 1) % PET_AUDIT_STEPS;
        }
        if (clip < 0) {
            return;
        }

        act_no     = entry;         /* 0 = 待机（夹在中间的间隔） */
        sg_clip    = clip;
        sg_clip_at = now;

        if (act_no > 0) {
            PR_NOTICE("[AUDIT] %d/%d %s（%s）%d 帧",
                      act_no, PET_AUDIT_N_ACT, sg_audit[entry].label,
                      app_sprite2d_clip_name(clip), app_sprite2d_clip_frames(clip));
        } else {
            PR_NOTICE("[AUDIT] · 待机（间隔）");
        }
        prv_audit_show(sg_audit[entry].label, clip, act_no);
    }
}
#endif

/** 情绪名 -> 动作名。**以精灵里真实存在的组为准**（见 app_sprite2d_clip_name()）。
 *
 * 云端给的是一套固定的 emoji 标签（见 skill_emotion.h），这里把它们归到精灵
 * 有的那几组上。**第三套动作集（2026-10-17）只有 click_happy 一组能对上表情**
 * （开心/大笑/调皮这类），sad / think / click_angry 都随旧素材没了 —— 映射到
 * 不存在的组只会刷"素材里没有这一组"的警告，所以这些情绪**不演**（见函数尾），
 * 宠物保持待机。想给伤心/生气配替身动作，就把下面 map[] 里加一行指到现有组。
 * 大小写不敏感 —— 云端偶尔会发小写。 */
static const char *prv_emotion_clip_name(const char *emo)
{
    if (NULL == emo) {
        return NULL;
    }

    /* 用前缀匹配而不是全等：emoji 标签有 27 个，一个个列全容易漏，
     * 而语义相近的（HAPPY/LAUGHING/FUNNY…）本来就该演同一组。 */
    struct {
        const char *key;
        const char *clip;
    } map[] = {
        { "HAPPY",       "click_happy"  },
        { "LAUGHING",    "click_happy"  },
        { "FUNNY",       "click_happy"  },
        { "DELICIOUS",   "click_happy"  },
        { "COOL",        "click_happy"  },
        { "CONFIDENT",   "click_happy"  },
        { "LOVING",      "click_happy"  },
        { "KISSY",       "click_happy"  },
        { "WINK",        "click_happy"  },
        { "RELAXED",     "click_happy"  },
    };

    for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (0 == strncasecmp(emo, map[i].key, strlen(map[i].key))) {
            return map[i].clip;
        }
    }
    return NULL;        /* 认不出来就不演，别硬塞一个不搭的动作 */
}

/* 事件回调：**跑在发布者线程**，只写请求位（见上面的状态机说明）。 */
static void prv_pet_evt_cb(APP_EVT_E evt, void *data, void *user)
{
    int clip = -1;

    (void)user;

    switch (evt) {
    case APP_EVT_PET_ACTION:
        clip = prv_clip_by_name((const char *)data);
        if (clip < 0) {
            PR_WARN("[pet] 素材里没有叫 %s 的动作", (NULL != data) ? (const char *)data : "(null)");
            return;
        }
        sg_req_clip = clip;
        break;

    case APP_EVT_AI_EMOTION: {
        const char *name = prv_emotion_clip_name((const char *)data);

        if (NULL == name) {
            return;     /* 认不出来的情绪：不演，保持当前动作 */
        }
        clip = prv_clip_by_name(name);
        if (clip < 0) {
            PR_WARN("[pet] 情绪 %s 映射到 %s，但素材里没有这一组",
                    (const char *)data, name);
            return;
        }
        sg_req_clip = clip;
        break;
    }

    /* AI 状态是**持续**的：思考时一直演 think，状态一变就跟着换
     * （见上面"持续 vs 一次性"的说明）。
     * **思考和说话都没有专属组了** —— 第三套动作集没有思考/碎碎念素材，
     * 期间回待机；想给它们配替身动作（比如思考 -> rnd_note），把下面的 -1
     * 改成组名即可。 */
    case APP_EVT_AI_STATE: {
        int state = (int)(intptr_t)data;

        sg_ai_state = state;

        switch (state) {
        case AI_MODE_STATE_THINK:
            clip = -1;      /* 第三套动作集没有 think（思考）组：期间回待机 */
        case AI_MODE_STATE_SPEAK:
            clip = -1;      /* 新动作集没有 talk（碎碎念）组：说话期间回待机 */
            break;
        default:
            clip = -1;      /* IDLE / LISTEN / UPLOAD / INIT -> 回待机 */
            break;
        }

        sg_req_hold = clip;
        break;
    }

    default:
        break;
    }
}

/** 点宠物。
 *
 * **AI 在思考 / 说话时，这一下是"打断"**（不弹捕获卡）—— 用户要的"点击可打断"。
 * 其余时候保持原型的"点击回应 + 捕获卡"（那是备忘录的模拟入口，ASR 接上之后
 * 会被真流程替掉，见文件头）。
 *
 * 跑在 LVGL 线程；`app_ai_chat_interrupt()` 内部不取本模块的锁（只判状态 + 发
 * 播放器停止和 CHAT_BREAK），所以这里调它是安全的。 */
static void prv_stage_click(lv_event_t *e)
{
#if PET_ACT_AUDIT
    /* 巡检只核对素材：点击不切动作、不弹捕获卡，只把"现在核对的是哪一组"
     * 再打一遍，省得翻前面的日志。 */
    (void)e;
    PR_NOTICE("[AUDIT] 巡检中（第 %d/%d 步），点击不切动作",
              sg_audit_pos + 1, PET_AUDIT_STEPS);
#else
    int state = sg_ai_state;

    (void)e;

    if (AI_MODE_STATE_SPEAK == state || AI_MODE_STATE_THINK == state) {
        PR_NOTICE("[pet] 点击打断（AI 状态 %d）", state);
        app_ai_chat_interrupt();        /* 没在播报时是空操作 */
        return;
    }

    if (PET_VIEW_IDLE != sg_view && PET_VIEW_RECALL != sg_view) {
        return;                         /* 已经在别的视图里，别打断 */
    }
    /* 和 MCP / 情绪走同一条路：只写请求位，tick 的帧边界上才切动作。
     * 动作从 4 个"点击回应"里随机挑（见 sg_tap_acts）。 */
    sg_req_clip = prv_pick_tap_act();
    prv_set_view(PET_VIEW_CAPTURE);
#endif
}

/***********************************************************
*********************** 定时器 *******************************
***********************************************************/
/* 待机时随机插播的小动作池。**不包含 idle 本身**。
 * 第三套动作集（2026-10-17）只剩 5 条，没有 snap / drag / talk / think 那几组，
 * 所以池子 = 4 条非待机动作（和 sg_tap_acts 同一个口径，与
 * app_avatar_tools.c 的白名单一致）。 */
#if !PET_ACT_AUDIT
static const char *const sg_idle_acts[] = {
    "click_wave", "click_happy", "rnd_photo", "rnd_note",
};
#endif

/* 一个够用的伪随机源 —— 只是挑"演哪个 / 隔多久演一次"，不需要好随机。
 * 不用 rand() 是因为它在裸机上没有播种来源（和 app_sprite2d.c 里同一个理由）。 */
#if !PET_ACT_AUDIT
static uint32_t prv_rand(uint32_t n)
{
    static uint32_t s_seed = 0x2468ACE1u;

    s_seed = s_seed * 1103515245u + 12345u;
    return (s_seed >> 16) % n;
}

/** 挑一个待机小动作。挑不到（素材缺组）返回 -1。 */
static int prv_pick_idle_act(void)
{
    int n = (int)(sizeof(sg_idle_acts) / sizeof(sg_idle_acts[0]));

    return prv_clip_by_name(sg_idle_acts[prv_rand((uint32_t)n)]);
}

/** 挑一个"点击回应"。**避开上一次刚演过的那个** —— 连点两下出一模一样的动作，
 *  看着就像"点了没反应"。 */
static int prv_pick_tap_act(void)
{
    static int s_last = -1;
    int        n = (int)(sizeof(sg_tap_acts) / sizeof(sg_tap_acts[0]));
    int        pick;

    if (n <= 1) {
        pick = 0;
    } else {
        do {
            pick = (int)prv_rand((uint32_t)n);
        } while (pick == s_last);
    }
    s_last = pick;
    return prv_clip_by_name(sg_tap_acts[pick]);
}
#endif /* !PET_ACT_AUDIT —— 巡检时不挑动作，这两个 pick 没人调 */

/* 把"播到第几帧"映射到这条动作的**循环区间**里。
 *
 * 只有持续动作（AI 说话/思考）会走到这里 —— 一次性动作播完就切走了。
 * 没配循环区间的（idle）原样返回，`render_clip` 内部会按整条取模。
 *
 * 结果按组号缓存：查表要 strcmp 名字，而这是 30fps 的渲染路径，别每帧都查。 */
static int prv_loop_frame(int clip, int frame)
{
    static int s_clip  = -1;
    static int s_first = 0;
    static int s_len   = 0;         /* 0 = 整条循环 */

    if (clip != s_clip) {
        const char *name = app_sprite2d_clip_name(clip);
        int         frames = app_sprite2d_clip_frames(clip);

        s_clip  = clip;
        s_first = 0;
        s_len   = 0;

        for (uint32_t i = 0; i < sizeof(sg_loop) / sizeof(sg_loop[0]); i++) {
            if (0 != strcmp(name, sg_loop[i].clip)) {
                continue;
            }
            if (frames != sg_loop[i].frames) {
                /* 素材重打过，帧号已经失效 —— 宁可退回整条循环，也别按错位的
                 * 帧号切一段莫名其妙的动作出来。 */
                PR_WARN("[pet] %s 现在 %d 帧（循环区间是按 %d 帧算的）—— 区间失效，"
                        "按整条循环。重跑 tools/find_loop.py 更新 sg_loop[]",
                        name, frames, sg_loop[i].frames);
                break;
            }
            s_first = sg_loop[i].first;
            s_len   = sg_loop[i].last - sg_loop[i].first + 1;
            break;
        }
    }

    return (s_len > 0) ? (s_first + (frame % s_len)) : frame;
}

/** 这条动作实际会播多少帧：配了循环区间就是区间长度，否则整条。
 *  一次性动作"演完"的判据用它（见 prv_tick_cb 的 ③）—— 用总帧数会在有循环
 *  区间时多播几拍（末尾回绕），正好是切换处那一下卡。 */
#if !PET_ACT_AUDIT     /* 只有正常模式的一次性动作"演完"会问它 */
static int prv_seq_len(int clip)
{
    const char *name   = app_sprite2d_clip_name(clip);
    int         frames = app_sprite2d_clip_frames(clip);

    for (uint32_t i = 0; i < sizeof(sg_loop) / sizeof(sg_loop[0]); i++) {
        if (0 != strcmp(name, sg_loop[i].clip)) {
            continue;
        }
        if (frames != sg_loop[i].frames) {
            break;                      /* 素材重打过，帧号失效，按整条 */
        }
        return sg_loop[i].last - sg_loop[i].first + 1;
    }

    return frames;
}
#endif

static void prv_tick_cb(lv_timer_t *t){
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    int      n   = app_sprite2d_clip_count();

    (void)t;

    /* ---- 动作状态机 ------------------------------------------------------
     * 优先级（从高到低）：
     *   ① 一次性插播请求（MCP 点名 / 情绪 / 点宠物）
     *   ② 持续动作变了（AI 思考 / 说话 / 结束）—— 立刻切，正在演的一次性动作让位
     *   ③ 一次性动作演完 -> 收敛回"当前该演的"
     *   ④ 待机够久 -> 随机插播一个小动作 */
    if (NULL != sg_cbuf && sg_cw > 0 && n > 0) {
#if PET_ACT_AUDIT
        prv_audit_state(now);       /* 巡检接管：下面整块正常状态机不跑 */
#else
        int req  = sg_req_clip;
        int hold = sg_req_hold;

        sg_req_clip = -1;
        if (req < 0 || req >= n) {
            req = -1;
        }

        /* ② 持续动作变了：AI 开始/结束思考或说话。
         * **立刻切过去**，正在演的一次性动作让位 —— 用户要的是"回复时就是碎碎念"，
         * 不是"等这段随机动作演完再说话"。 */
        if (hold != -2) {
            sg_req_hold  = -2;
            sg_hold_clip = (hold >= 0 && hold < n) ? hold : -1;
            sg_clip      = (sg_hold_clip >= 0) ? sg_hold_clip : PET_CLIP_IDLE;
            sg_clip_at   = now;
            sg_clip_once = FALSE;
            if (sg_hold_clip < 0) {
                sg_idle_at = now;       /* 回待机：重新开始"多久之后随机插播" */
            }
            PR_NOTICE("[pet] AI 状态 -> %s", app_sprite2d_clip_name(sg_clip));
        }

        /* ① 一次性插播：优先级最高，会盖掉上面刚切的持续动作，演完再回去 */
        if (req >= 0) {
            sg_clip      = req;
            sg_clip_at   = now;
            sg_clip_once = TRUE;
            PR_NOTICE("[pet] 插播 %s（第 %d/%d 组）",
                      app_sprite2d_clip_name(sg_clip), sg_clip + 1, n);
        }

        if (sg_clip < 0 || sg_clip >= n) {
            sg_clip = PET_CLIP_IDLE;
        }

        /* ③ 一次性动作演完 —— 判据同样是**帧号**，不是墙钟（理由见 prv_audit_state
         * 开头那段）：按墙钟会在末尾多播几拍，正好回绕到第 0 帧，就是切换卡一下。 */
        if (sg_clip_once &&
            (sg_frame_idx + 1u) >= (uint32_t)prv_seq_len(sg_clip)) {
            sg_clip_once = FALSE;
            sg_clip_at   = now;             /* 下一拍按 want 收敛 */
        }

        /* ④ 不在插播中：收敛到"当前该演的"；待机时再考虑随机插播 */
        if (!sg_clip_once) {
            int want = (sg_hold_clip >= 0) ? sg_hold_clip : PET_CLIP_IDLE;

            if (sg_clip != want) {
                sg_clip    = want;
                sg_clip_at = now;
                if (PET_CLIP_IDLE == want) {
                    sg_idle_at = now;
                }
            } else if (PET_CLIP_IDLE == want &&
                       (now - sg_idle_at) >= PET_IDLE_ACT_AFTER_MS) {
                int act = prv_pick_idle_act();

                sg_idle_at = now;           /* 挑不到也重置计时，免得每帧都试 */
                if (act >= 0) {
                    sg_clip      = act;
                    sg_clip_at   = now;
                    sg_clip_once = TRUE;
                    PR_NOTICE("[pet] 待机插播 %s", app_sprite2d_clip_name(act));
                }
            }
        }
#endif /* PET_ACT_AUDIT */

        /* 帧号用**计数器**推进，不用 (now - sg_clip_at) / PET_FRAME_MS：
         * tick 和帧周期都是 42ms，一拍正好一帧；按墙钟除法会在定时器有一两毫秒
         * 抖动时把同一帧算两遍（66ms 不动），下一帧再追回来 —— 那点不均匀就是
         * 眼睛看到的"卡"。计数器只在换组、或同一组被重新点名（sg_clip_at 变了）
         * 时归零，节奏恒定 24fps。
         *
         * 帧号超出本组帧数不用管：app_sprite2d_blit_clip 内部按组帧数取模。 */
        if (sg_clip != sg_frame_clip || sg_clip_at != sg_frame_at) {
            sg_frame_clip = sg_clip;
            sg_frame_at   = sg_clip_at;
            sg_frame_idx  = 0;
        } else {
            sg_frame_idx++;
        }

        app_sprite2d_render_clip(sg_cbuf, sg_cw, sg_ch, sg_clip,
                                 prv_loop_frame(sg_clip, (int)sg_frame_idx),
                                 PET_COL_BG565);
        if (NULL != sg_stage) {
            lv_obj_invalidate(sg_stage);
        }
    }

    /* ---- 待机够久就演一次"想起" ----------------------------------------
     * 原型里自动触发；真产品里由后端 / LLM 决定时机。
     * 巡检时不演 —— 它会占掉右半屏那块"i/n + 中文名"。 */
#if !PET_ACT_AUDIT
    if (PET_VIEW_IDLE == sg_view && !sg_recalled &&
        (now - sg_view_at) >= PET_RECALL_AFTER_MS) {
        sg_recalled = TRUE;
        prv_set_view(PET_VIEW_RECALL);
    }
#endif
}

/***********************************************************
*********************** 页面 *********************************
***********************************************************/
static void prv_rot_set(lv_display_rotation_t rot)
{
    lv_display_t *disp = lv_display_get_default();

    if (NULL != disp && lv_display_get_rotation(disp) != rot) {
        lv_display_set_rotation(disp, rot);
    }
}

static OPERATE_RET prv_pet_enter(void *arg)
{
    (void)arg;

    app_disp_lock();

    /* 横屏是**全产品**的决定，开机时就已经转好了（见 tuya_app_main.c）。
     * 这里再设一次是幂等的保险：万一以后有人调了启动顺序，这一页也不会竖着出来。 */
    prv_rot_set(LV_DISPLAY_ROTATION_90);

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, lv_color_hex(PET_COL_BG), 0);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶部状态栏（WiFi / 天气 / 时间 / 音量 / 电量）--------------------
     * 它是**这一页的一个控件**，不是页面 —— 见 app_statusbar.h。
     * 三个弹窗（WiFi 键盘 / 天气详情 / 音量）也建在这块屏幕上，按需弹出。 */
    (void)app_statusbar_create(sg_scr);

    /* ---- 宠物：水平居中、底对齐 ------------------------------------------
     * 用 lv_canvas 而不是桌面浮层：浮层是直接往面板 framebuffer 上画的，
     * 拿不到 LVGL 的旋转；画布是普通 LVGL 对象，旋转由 LVGL 管，
     * 位置也精确可控。**画布不可拖** —— 这一页的宠物不跟随手指。 */
    if (OPRT_OK == app_sprite2d_init()) {
        app_sprite2d_size(&sg_cw, &sg_ch);
    }
    if (sg_cw > 0 && sg_ch > 0) {
        sg_cbuf = (uint16_t *)tal_psram_malloc((size_t)sg_cw * (size_t)sg_ch * 2u);
    }

    if (NULL != sg_cbuf) {
        int origin_x, origin_y;

        /* **设备只管显示，几何全在打包里。**
         *
         * 打包（tools/pack_seq.py）保证：锚点框是**以待机裁剪框中心为枢轴**的对称框，
         * 而且每一组的角色都已经对齐到同一个站位、同一个尺寸。所以这里只需要把画布
         * 居中摆到原点 —— 不用读第 0 组的裁剪框，也不用对任何动作做偏移补偿。
         *
         * （以前这里是 `clip_box(0,...)` + `odx + ow/2`，等于设备端也在算位置，
         * 而且待机的裁剪框一变宠物就挪窝。2026-10-16 用户要求改成"只保留打包"。）
         *
         * 想整体挪宠物就调 PET_ORIGIN_X / PET_ORIGIN_Y（PET_ORIGIN_DROP 是纵向旋钮）。 */
        origin_x = sg_cw / 2;
        origin_y = sg_ch / 2;

        memset(sg_cbuf, 0, (size_t)sg_cw * (size_t)sg_ch * 2u);
        sg_stage = lv_canvas_create(sg_scr);
        lv_canvas_set_buffer(sg_stage, sg_cbuf, sg_cw, sg_ch,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(sg_stage, PET_ORIGIN_X - origin_x,
                       PET_ORIGIN_Y - origin_y);
        lv_obj_set_style_border_width(sg_stage, 0, 0);
        lv_obj_set_style_pad_all(sg_stage, 0, 0);
        lv_obj_set_style_radius(sg_stage, 0, 0);
        lv_obj_clear_flag(sg_stage, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sg_stage, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sg_stage, prv_stage_click, LV_EVENT_CLICKED, NULL);

        PR_NOTICE("[pet] 画布 %dx%d 居中摆在 (%d,%d)（几何全由打包决定）",
                  sg_cw, sg_ch,
                  PET_ORIGIN_X - origin_x, PET_ORIGIN_Y - origin_y);
    } else {
        PR_WARN("[pet] 精灵没起来（SD 上没有 %s？），只有信息带", SP2D_PATH_SPF);
    }

    /* ---- 右半：备忘录 ----------------------------------------------------
     * 宠物一直在左半，换内容只动这一块 —— 四个视图共用它。
     * **尺寸是内容区的高度**（320 减掉顶上那条状态栏），不是整屏。 */
    sg_info = lv_obj_create(sg_scr);
    lv_obj_set_size(sg_info, PET_INFO_W, PET_BODY_H);
    lv_obj_set_pos(sg_info, PET_INFO_X, PET_BODY_Y);
    lv_obj_set_style_radius(sg_info, 0, 0);
    lv_obj_set_style_bg_color(sg_info, lv_color_hex(PET_COL_BG), 0);
    lv_obj_set_style_bg_opa(sg_info, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sg_info, 0, 0);
    lv_obj_set_style_pad_all(sg_info, 0, 0);
    lv_obj_clear_flag(sg_info, LV_OBJ_FLAG_SCROLLABLE);

    sg_view      = PET_VIEW_IDLE;
    sg_view_at   = (uint32_t)tal_system_get_millisecond();
    sg_recalled  = FALSE;
    sg_clip      = PET_CLIP_IDLE;
    sg_clip_at   = sg_view_at;
#if PET_ACT_AUDIT
    sg_clip      = -1;      /* 巡检：第一拍就切到"待机"，并把右半屏换成巡检信息 */
    sg_audit_pos = 0;
#endif
    sg_clip_once = FALSE;
    sg_idle_at   = sg_view_at;
    sg_hold_clip = -1;
    sg_req_clip  = -1;
    sg_req_hold  = -2;
    sg_ai_state  = AI_MODE_STATE_IDLE;

    /* 动作由这三条事件驱动（大模型点名 / AI 情绪 / AI 状态），见上面的状态机说明。
     * 订阅是幂等的，页面重进不会叠加。 */
    app_event_subscribe(APP_EVT_PET_ACTION, prv_pet_evt_cb, NULL);
    app_event_subscribe(APP_EVT_AI_EMOTION, prv_pet_evt_cb, NULL);
    app_event_subscribe(APP_EVT_AI_STATE,   prv_pet_evt_cb, NULL);

    lv_screen_load(sg_scr);
    prv_show_idle();

    sg_tick = lv_timer_create(prv_tick_cb, PET_TICK_MS, NULL);

    app_disp_unlock();

    PR_NOTICE("[pet] 页面就绪（画布 %dx%d @ (%d,%d)，素材 %d 组，%s）",
              sg_cw, sg_ch,
              PET_ORIGIN_X - sg_cw / 2, PET_ORIGIN_Y - sg_ch / 2,
              app_sprite2d_clip_count(),
              (NULL != sg_cbuf) ? "宠物已加载" : "**没有宠物**");
    return OPRT_OK;
}

static void prv_pet_exit(void)
{
    app_disp_lock();

    /* 动作事件的订阅撤掉。**必须在删屏幕之前** —— 事件回调只写请求位、不碰
     * LVGL，所以顺序上其实无所谓，但撤干净免得下次进页面时旧回调还在。 */
    app_event_unsubscribe(APP_EVT_PET_ACTION, prv_pet_evt_cb);
    app_event_unsubscribe(APP_EVT_AI_EMOTION, prv_pet_evt_cb);
    app_event_unsubscribe(APP_EVT_AI_STATE,   prv_pet_evt_cb);

    if (NULL != sg_tick) {
        lv_timer_del(sg_tick);
        sg_tick = NULL;
    }
    /* 画布缓冲必须在删屏幕**之前**放：lv_canvas 不拥有那块内存，但 LVGL 可能
     * 还在用它渲染，所以先删对象、再释放缓冲。 */
    if (NULL != sg_scr) {
        lv_obj_del(sg_scr);
        sg_scr = NULL;
    }
    sg_stage = NULL;
    sg_info  = NULL;

    if (NULL != sg_cbuf) {
        tal_psram_free(sg_cbuf);
        sg_cbuf = NULL;
    }

    /* **不要把旋转调回 0** —— 横屏是全产品的（开机画面、SD 门页都在这个坐标系
     * 里），这一页离开不该把整个产品的方向掰回去。 */

    app_disp_unlock();
}

/***********************************************************
*********************** 系统按键 *****************************
***********************************************************/
/* P12：短按掐断 AI 正在说的那句话；长按弹设置（网络 / 音量）。
 *
 * 路由只在 `on_key == NULL` 时才把返回键的"按下"当"弹出本页"用（见
 * app_router.c 的 prv_dispatch_key）。桌宠页是唯一页面、栈深恒为 1，返回本来也
 * 没处去 —— 所以接管 on_key 不影响任何已有行为。
 *
 * 跑在**路由线程**（不是 LVGL 线程），所以 app_statusbar_open_settings() 自己
 * 加显示锁是对的。 */
static void prv_pet_key(APP_KEY_E key, APP_KEY_EVT_E evt)
{
    if (APP_KEY_BACK != key) {
        return;
    }

    if (APP_KEY_EVT_SHORT == evt) {
        app_ai_chat_interrupt();
    } else if (APP_KEY_EVT_LONG == evt) {
        app_statusbar_open_settings();
    }
}

const APP_PAGE_T app_page_pet = {
    .name      = PAGE_PET,
    .flags     = APP_PAGE_NO_OVERLAY | APP_PAGE_NEEDS_SD,
    .on_enter  = prv_pet_enter,
    .on_resume = NULL,
    .on_pause  = NULL,
    .on_exit   = prv_pet_exit,
    .on_key    = prv_pet_key,
    .on_event  = NULL,
};
