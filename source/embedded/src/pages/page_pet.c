/**
 * @file page_pet.c
 * @brief 桌宠主界面：**新 UI（third_party/ai_pet_wide 移植）的宿主页**。
 *
 * 这一页现在只做三件事，界面本身全在 src/ui/ 里：
 *
 *   1) **建壳**：`pet_ui_init(sg_scr)` 建出顶栏 + 左宠物卡 + 右功能面板
 *      （主页 / 拍照 / 相册 / 备忘录 / 全部笔记 / 设置，七页的页面栈）。
 *   2) **把精灵画进左卡**：`pet_pet_stage()` 给出卡里那块舞台，这里在上面建一个
 *      lv_canvas，按 app_sprite2d 的脏矩形方式每 42ms 画一帧。
 *   3) **动作状态机**：AI 思考/说话、大模型点名、情绪、点击、待机随机插播 ——
 *      和界面无关，原样保留在图里。
 *
 * 布局（480x320 横屏，面板 320x480 转 90°）
 * ----------------------------------------
 *   **顶部 480x24  状态栏**  待办徽标 · 天气 · 时间 · WiFi · 音量 · 电量
 *   **左  196x286  宠物卡**  对话气泡 + 实时精灵
 *   **右  250x286  功能面板** 页面栈（主页三入口 + 两张摘要卡）
 *
 * 和旧版的差别
 * ------------
 * 旧版是深色主题、宠物在左半 240、右半是写死的备忘录原型视图；新版整套界面来自
 * 宽屏 demo（米白卡片风），右半是真正的页面栈，备忘录/笔记/设置都是独立页面。
 * **宠物仍然是实时精灵**，不是 demo 里那张 932KB 的静态图。
 *
 * 动作状态机（**不是**循环轮播）
 * ------------------------------
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
#include "app_pages.h"
#include "app_router.h"
#include "app_sprite2d.h"
#include "app_statusbar.h"

#include "pet_pet.h"
#include "pet_theme.h"
#include "pet_ui.h"

/***********************************************************
************************* 几何 *****************************
***********************************************************/
/* 宠物卡底色（PET_COLOR_PET_COLUMN = 0x0E1626）的 RGB565 版本。
 * 精灵画布的底色要的是 uint16 的 565，不是 24 位色值：
 * R=0x0E>>3=1, G=0x16>>2=5, B=0x26>>3=4 -> 0x08A4。 */
#define PET_COL_CARD565   0x08A4

/* 每组动作播多久。固定 24fps（素材原速）；1 帧的组给个最短时长，
 * 否则 snap / drag_hold 这种一闪而过根本看不见。 */
#define PET_FRAME_MS      42
#define PET_CLIP_MIN_MS   900

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

/** 供给不足时的降帧档（2026-10-20）。
 *
 * AI 播报期 SD IO 掉到 ~1MB/s：loader 每 2s 只供 29 帧，贴着 24fps 的 45 拍
 * 在跑、裕度≈0 —— 批间稍有抖动（105~216ms 的坏批）就整拍 miss 复读旧帧，
 * 下一批到达又 exact 命中切回新帧，相邻拍在"已播帧↔下一帧"间弹跳（抽搐）。
 * 拍子拉到 84ms 后同样供给有 ~100% 裕度，坏批也够覆盖 2.5 拍，复读消失。
 * 只由 miss 率触发/恢复，待机不受影响。 */
#define PET_TICK_SLOW_MS      84
#define PET_MISS_DN_PCT       25        /* miss 率 ≥25% -> 降 */
#define PET_MISS_UP_PCT       8         /* miss 率 <8%  -> 升（滞回带防抖）*/

/** 待机组的组号。素材约定第 0 组必须是 idle（见 tools/build_fish.sh 的 --order）。 */
#define PET_CLIP_IDLE         0

/***********************************************************
************************ 动作巡检 *****************************
***********************************************************/
/* **只为核对素材存在**：开机后按「待机、待机、动作、待机、待机、动作……」的节奏
 * 把每个非待机动作各放**一次**，最后一组后面不补待机，然后从头再来。
 * 串口打 `[AUDIT] 3/15 表情-伤心（emo_sad）130 帧`，气泡里同时显示
 * "3/15 + 中文名"，不用回头翻日志对时间。
 *
 * 打开时**整页只剩巡检**：随机插播 / 点击切动作全部让位（点击只打一行
 * 日志），否则动作会被打断，没法逐组核对。核完把这里置 0 就是正常桌宠 ——
 * 状态机代码一行没动，只是在它之前 return。
 *
 * **2026-10-20 起默认 0**：界面换成新 UI 之后，右侧面板不再是巡检信息区
 * （它是真正的功能页），巡检信息改到气泡里显示。要核对素材时再置 1。
 *
 * 加/删素材后**这张表和 ELF_ORDER 要同序**：动作序号是按表里的顺序数的，
 * 错位不影响播放（按名字找组），但屏幕上的 "i/15" 会和实际播放的组对不上。 */
#define PET_ACT_AUDIT      0

/** 每组放几遍。**1 = 只放一次**（用户核对用，放两遍反而要等）。 */
#define PET_AUDIT_LOOPS    1

/** 两个非待机动作之间夹几个待机 —— 用户要的节奏是
 *  「待机-待机-开心-待机-待机-伤心」，即夹 2 个。 */
#define PET_AUDIT_GAP      2

/** 巡检表：按打包顺序（= tools/build_fish.sh 的 ELF_ORDER）列全部组。
 * 第 0 条必须是 idle（既当夹在中间的空档，又是动作序号 0 的基准）。
 * label 是给人看的中文名，和源视频文件名一一对应。
 *
 * 整张表（连同下面三个计数宏）只在巡检模式里用 —— 平时置 0，不编进固件。 */
#if PET_ACT_AUDIT
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
#endif /* PET_ACT_AUDIT */

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
********************** 点击时说的话 **************************
***********************************************************/
/* 点宠物（AI 没在说话时）插播一句短话。**不超过气泡的两行上限（14 个字）**，
 * 见 pet_pet.h。空串是"让它安静下来"，留一条方便演示气泡的消失。 */
#if !PET_ACT_AUDIT
static const char *const sg_pokes[] = {
    "怎么啦？我一直在这儿呢",
    "今天的待办别忘了哦",
    "要拍张照片吗？我可以帮你看看",
    "想记点什么的话，说给我听",
    "",
};
#endif

/***********************************************************
************************* 状态 *****************************
***********************************************************/
static lv_obj_t   *sg_scr;
static lv_obj_t   *sg_stage;        /* 宠物画布 */
static lv_timer_t *sg_tick;
static uint16_t   *sg_cbuf;         /* 画布像素缓冲（PSRAM） */
static int         sg_cw, sg_ch;    /* 画布尺寸 = 精灵锚点框 */
static int         sg_painted_clip;  /**< 上一拍真正画过的组号；-1 = 没画过 */
static int         sg_painted_frame; /**< 上一拍真正画过的帧号（同帧跳过用）*/

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

/** 请求改气泡文字（跨线程）：0 = 不动，1 = 思考中，2 = 收起。 */
static volatile int sg_req_say;

/** SD 字库换进来了（APP_EVT_FONT_READY）：当前页用旧字体量过尺寸，要重建。 */
static volatile BOOL_T sg_req_font;

/** AI 当前状态（AI_MODE_STATE_E），点击宠物时用来判断"这一下是不是打断"。 */
static volatile int sg_ai_state = AI_MODE_STATE_IDLE;

/***********************************************************
********************** 前向声明 ******************************
***********************************************************/
#if !PET_ACT_AUDIT
static int  prv_pick_tap_act(void);     /* 定义在下面的"动作状态机"一节 */
static int  prv_pick_idle_act(void);
#endif

/***********************************************************
*********************** 小工具 ******************************
***********************************************************/
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

/** 气泡里显示当前这一拍：动作显示 "i/15 中文名"，待机间隔只写"待机"。 */
static void prv_audit_show(const char *label, int clip, int act_no)
{
    char buf[40];

    if (act_no > 0) {
        snprintf(buf, sizeof(buf), "%d/%d %s", act_no, PET_AUDIT_N_ACT, label);
    } else {
        snprintf(buf, sizeof(buf), "待机");
    }
    pet_say(buf);
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
 * 云端给的是一套固定的 emoji 标签（skill_emotion.h 的 EMOJI_*），这里把它们归到
 * 第三套动作集里能表达的那些组上：
 *
 *   emo_happy  开心 / 得意 / 亲密 / 放松
 *   emo_sad    难过 / 失望 / 烦
 *   emo_think  思考 / 困惑
 *   tap_shy    惊讶 / 害羞
 *   rnd_scare  被吓一跳（震惊 / 害怕）
 *   tap_proud  生气（傲娇生气）
 *   rnd_yawn   困 / 醒来
 *   tap_tickle 被戳
 *
 * NEUTRAL / LEFT / RIGHT 不演，宠物保持待机（"恢复表情"就是回到 idle）。
 * 前缀匹配、大小写不敏感 —— emoji 标签有二十多个，全等表容易漏。 */
static const char *prv_emotion_clip_name(const char *emo)
{
    if (NULL == emo) {
        return NULL;
    }

    struct {
        const char *key;
        const char *clip;
    } map[] = {
        { "HAPPY",       "emo_happy"  },
        { "LAUGHING",    "emo_happy"  },
        { "FUNNY",       "emo_happy"  },
        { "DELICIOUS",   "emo_happy"  },
        { "COOL",        "emo_happy"  },
        { "CONFIDENT",   "emo_happy"  },
        { "LOVING",      "emo_happy"  },
        { "KISSY",       "emo_happy"  },
        { "WINK",        "emo_happy"  },
        { "RELAXED",     "emo_happy"  },
        { "SILLY",       "emo_happy"  },
        { "SAD",         "emo_sad"    },
        { "DISAPPOINTED", "emo_sad"   },
        { "ANNOYED",     "emo_sad"    },
        { "THINKING",    "emo_think"  },
        { "CONFUSED",    "emo_think"  },
        { "SURPRISE",    "tap_shy"    },
        { "EMBARRASSED", "tap_shy"    },
        { "SHOCKED",     "rnd_scare"  },
        { "FEARFUL",     "rnd_scare"  },
        { "ANGRY",       "tap_proud"  },
        { "SLEEP",       "rnd_yawn"   },
        { "WAKEUP",      "rnd_yawn"   },
        { "TOUCH",       "tap_tickle" },
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

    /* AI 状态是**持续**的：思考时一直演 emo_think，说话期间回待机，状态一变
     * 就跟着换 —— 这就是"AI 恢复表情"：话说完（state 回 idle）就回到待机表情。 */
    case APP_EVT_AI_STATE: {
        int state = (int)(intptr_t)data;

        sg_ai_state = state;

        switch (state) {
        case AI_MODE_STATE_THINK:
            clip = prv_clip_by_name("emo_think");   /* 持续：思考表情 */
            sg_req_say = 1;
            break;
        case AI_MODE_STATE_SPEAK:
            clip = -1;      /* 新动作集没有 talk（碎碎念）组：说话期间回待机 */
            sg_req_say = 2;
            break;
        default:
            clip = -1;      /* IDLE / LISTEN / UPLOAD / INIT -> 回待机（恢复表情） */
            sg_req_say = 2;
            break;
        }

        sg_req_hold = clip;
        break;
    }

    case APP_EVT_FONT_READY:
        /* 字库换进代理了：页面是按内置字体量过尺寸的，交给 tick 重建。 */
        sg_req_font = TRUE;
        break;

    default:
        break;
    }
}

/** 点宠物。
 *
 * **AI 在思考 / 说话时，这一下是"打断"**（不换动作）—— 用户要的"点击可打断"。
 * 其余时候随机演一个"点击回应"并说一句短话。
 *
 * 跑在 LVGL 线程；`app_ai_chat_interrupt()` 内部不取本模块的锁（只判状态 + 发
 * 播放器停止和 CHAT_BREAK），所以这里调它是安全的。 */
static void prv_stage_click(lv_event_t *e)
{
#if PET_ACT_AUDIT
    /* 巡检只核对素材：点击不切动作，只把"现在核对的是哪一组"再打一遍。 */
    (void)e;
    PR_NOTICE("[AUDIT] 巡检中（第 %d/%d 步），点击不切动作",
              sg_audit_pos + 1, PET_AUDIT_STEPS);
#else
    static uint32_t s_poke;
    int             state = sg_ai_state;

    (void)e;

    if (AI_MODE_STATE_SPEAK == state || AI_MODE_STATE_THINK == state) {
        PR_NOTICE("[pet] 点击打断（AI 状态 %d）", state);
        app_ai_chat_interrupt();        /* 没在播报时是空操作 */
        return;
    }

    /* 和 MCP / 情绪走同一条路：只写请求位，tick 的帧边界上才切动作。 */
    sg_req_clip = prv_pick_tap_act();

    pet_say(sg_pokes[s_poke]);
    s_poke = (s_poke + 1) % (sizeof(sg_pokes) / sizeof(sg_pokes[0]));
#endif
}

/***********************************************************
*********************** 定时器 *******************************
***********************************************************/
/* 待机时随机插播的小动作池。**不包含 idle 本身**。
 *
 * 名字必须和 fish_elf.spf 的组名一字不差（见 sg_audit 表 / ELF_ORDER）。
 * 2026-10-20 第三套动作集的全部 rnd_* 都进池子 —— 以前这里还是旧套的
 * click_wave 之类，切素材后这些名字查不到组，随机动作其实一直没生效。 */
#if !PET_ACT_AUDIT
static const char *const sg_idle_acts[] = {
    "rnd_gomoku", "rnd_pigeon", "rnd_yawn",
    "rnd_cat",    "rnd_lion",   "rnd_scare",
};

/* 点宠物时插播的动作 —— 四个"点击回应"。名字同上，以素材为准。 */
static const char *const sg_tap_acts[] = {
    "tap_proud", "tap_shy", "tap_happy", "tap_tickle",
};

/* 一个够用的伪随机源 —— 只是挑"演哪个 / 隔多久演一次"，不需要好随机。
 * 不用 rand() 是因为它在裸机上没有播种来源（和 app_sprite2d.c 里同一个理由）。 */
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

/* ---- 调试统计开关 ----------------------------------------------------------
 * 置 1 时每 2 秒打一条 [pet] 刷新统计（次数 / 平均 / 最大耗时）。配合
 * app_sprite2d.c 的 [sp2d] 预取统计，能把"刷新本身慢 / SD 慢 / 线程被抢占"
 * 三者分开。定位完置 0。 */
#define PET_DBG_STAT     1

/** 刷新统计窗口：次数、累计耗时、最大耗时、窗口起点。 */
static uint32_t sg_refr_n;
static uint32_t sg_refr_sum;
static uint32_t sg_refr_max;
static uint32_t sg_refr_at;

/** @brief 记一次 lv_refr_now 的耗时，满 2 秒打一条。 */
static void prv_dbg_refr(uint32_t dt, uint32_t now)
{
#if PET_DBG_STAT
    if (0 == sg_refr_at) {
        sg_refr_at = now;                     /* 第一拍只当窗口起点 */
        return;
    }
    sg_refr_n++;
    sg_refr_sum += dt;
    if (dt > sg_refr_max) {
        sg_refr_max = dt;
    }
    if (now - sg_refr_at >= 2000u) {
        PR_DEBUG("[pet] 刷新 %u 次/2s：平均 %u ms 最大 %u ms",
                 (unsigned)sg_refr_n,
                 (unsigned)(sg_refr_n ? sg_refr_sum / sg_refr_n : 0),
                 (unsigned)sg_refr_max);
        sg_refr_n   = 0;
        sg_refr_sum = 0;
        sg_refr_max = 0;
        sg_refr_at  = now;
    }
#else
    (void)dt;
    (void)now;
#endif
}

/* ---- 帧率自适应：供不上就降拍（2026-10-20）-----------------------------
 * miss 率 = 复读+落空 / 总取帧。SD 被网络/音频 DMA 挤占时（AI 播报期实测
 * 25~50%）按慢档跑，恢复后自动升回 24fps。判定信号来自预取线程的**累计**
 * hit/miss（app_sprite2d_pf_get，永不清零；uint32 回绕时重建基线）。
 *
 * 防抖三件套（2026-10-20 第二轮，治 42/84 反复横跳）：
 *   ① 1s 一个评估窗口（原 500ms 太碎 —— 供货是脉冲式的，窗口间 miss 率
 *      能差出 30%+，采样本身就在制造抖动）；
 *   ② 连续 2 个坏窗口才降、连续 4 个好窗口才升（滞回带里两不沾）；
 *   ③ 换挡后 3s 冷静期，期间只累计不计分。
 * 空窗口（没有取帧需求 —— idle 常驻帧不碰计数器）算"好"：没 miss 可犯。 */
static uint32_t sg_ad_at;                 /* 上次评估时刻 */
static uint32_t sg_ad_sw_at;              /* 上次换挡时刻 */
static uint32_t sg_ad_hit, sg_ad_miss;    /* 上次读到的累计计数 */
static uint8_t  sg_ad_bad, sg_ad_good;    /* 连续坏 / 好窗口数 */
static BOOL_T   sg_ad_slow;               /* 当前是不是慢档 */

static void prv_adapt_ticks(uint32_t now, lv_timer_t *tick)
{
    uint32_t hit, miss, dh, dm, tol;

    if (0 == sg_ad_at) {                  /* 首拍只建基线 */
        sg_ad_at   = now;
        sg_ad_sw_at = now;
        (void)app_sprite2d_pf_get(&sg_ad_hit, &sg_ad_miss);
        return;
    }
    if (now - sg_ad_at < 1000u) {
        return;                           /* 1s 评估一次 */
    }
    sg_ad_at = now;

    (void)app_sprite2d_pf_get(&hit, &miss);

    if (hit < sg_ad_hit || miss < sg_ad_miss) {
        sg_ad_hit  = hit;                 /* 累计计数回绕：重建基线 */
        sg_ad_miss = miss;
        sg_ad_bad = sg_ad_good = 0;
        return;
    }
    dh = hit - sg_ad_hit;
    dm = miss - sg_ad_miss;
    sg_ad_hit  = hit;
    sg_ad_miss = miss;
    tol = dh + dm;

    if (0 == tol) {
        if (sg_ad_good < 200u) {
            sg_ad_good++;
        }
        sg_ad_bad = 0;                    /* 没需求 = 没 miss 可犯 */
    } else if (dm * 100u >= (uint32_t)PET_MISS_DN_PCT * tol) {
        if (sg_ad_bad < 200u) {
            sg_ad_bad++;
        }
        sg_ad_good = 0;
    } else if (dm * 100u < (uint32_t)PET_MISS_UP_PCT * tol) {
        if (sg_ad_good < 200u) {
            sg_ad_good++;
        }
        sg_ad_bad = 0;
    } else {
        sg_ad_bad = sg_ad_good = 0;       /* 滞回带里：两不沾 */
    }

    if (now - sg_ad_sw_at < 3000u) {
        return;                           /* 换挡冷静期：只累计不换挡 */
    }

    if (!sg_ad_slow && sg_ad_bad >= 2u) {
        sg_ad_slow  = TRUE;
        sg_ad_sw_at = now;
        sg_ad_bad   = 0;
        sg_ad_good  = 0;
        lv_timer_set_period(tick, PET_TICK_SLOW_MS);
        PR_NOTICE("[pet] 预取缺帧 %u/%u -> 拍子 %u -> %u ms（供给不足，降帧保流畅）",
                  (unsigned)dm, (unsigned)tol,
                  (unsigned)PET_TICK_MS, (unsigned)PET_TICK_SLOW_MS);
    } else if (sg_ad_slow && sg_ad_good >= 4u) {
        sg_ad_slow  = FALSE;
        sg_ad_sw_at = now;
        sg_ad_bad   = 0;
        sg_ad_good  = 0;
        lv_timer_set_period(tick, PET_TICK_MS);
        PR_NOTICE("[pet] 预取缺帧 %u/%u -> 拍子 %u -> %u ms（供给恢复，升回）",
                  (unsigned)dm, (unsigned)tol,
                  (unsigned)PET_TICK_SLOW_MS, (unsigned)PET_TICK_MS);
    }
}

/***********************************************************
************************* 每拍 *******************************
***********************************************************/
static void prv_tick_cb(lv_timer_t *t)
{
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    int      n   = app_sprite2d_clip_count();

    (void)t;

    /* 弹窗盖住界面时宠物停手。
     *
     * 天气窗 456x240、WiFi 窗整屏，都压在宠物卡上；宠物每 42ms 脏一块画布，
     * LVGL 就要把弹窗覆盖的那一块跟着重绘 —— 一层透明遮罩 + 一层窗口 + 阴影/
     * 圆角。打开弹窗后帧率掉到十几就是这个原因。遮罩在的时候用户也看不见宠物，
     * 直接不画，关掉弹窗自动恢复。 */
    if (app_statusbar_popup_visible()) {
        return;
    }

    /* 供给不足时降拍（见 PET_TICK_SLOW_MS 注释）：1s 评一次，内部只在越档时
     * 调 lv_timer_set_period —— 常态零开销。 */
    prv_adapt_ticks(now, t);

    /* ---- 气泡文字请求（事件回调只写位，这里才碰 LVGL） ---- */
    {
        int say = sg_req_say;

        if (0 != say) {
            sg_req_say = 0;
            if (1 == say) {
                pet_say_mood("让我想想…", PET_SAY_THINK);
            } else {
                pet_say("");
            }
        }
    }

    /* ---- SD 字库换进来了：当前页要按新字体重新量一遍 ---- */
    if (sg_req_font) {
        sg_req_font = FALSE;
        pet_ui_refresh();           /* 重建面板上的当前页 */
        pet_ui_badge_refresh();     /* 顶栏徽标也重排一次 */
        pet_pet_reflow();           /* 气泡按新字体重新折行 */
    }

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
        int    req  = sg_req_clip;
        int    hold = sg_req_hold;

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

        /* ③ 一次性动作演完 —— 判据是**帧号**，不是墙钟（理由见 prv_audit_state
         * 开头那段）。 */
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
         * 帧号超出本组帧数不用管：blit 内部按组帧数取模。 */
        if (sg_clip != sg_frame_clip || sg_clip_at != sg_frame_at) {
            sg_frame_clip = sg_clip;
            sg_frame_at   = sg_clip_at;
            sg_frame_idx  = 0;
        } else {
            sg_frame_idx++;
        }

        {
            int lf = prv_loop_frame(sg_clip, (int)sg_frame_idx);

            /* 同一帧不重画：待机组 / 单帧组这一拍算出的（组, 帧）和上一拍
             * 真正画过的相同，画布内容就没变 —— 重绘 + 标脏都是白工
             * （待机 16fps、tick 42ms，三拍里有一拍会撞上）。 */
            if (sg_clip != sg_painted_clip || lf != sg_painted_frame) {
                SP2D_RECT_T dirty;

                /* 只把上一帧画过的那块铺回底色、只画当前帧、只标两帧并集：
                 * 角色 ~129×152 ≈ 40KB，画布 193×199 ≈ 150KB。SD 读和 RLE
                 * 解压都在预取线程里（见 app_sprite2d.c 的 sp2d_pf），这里
                 * 只做"从槽里取 + 混合"。 */
                if (app_sprite2d_render_clip_dirty(sg_cbuf, sg_cw, sg_ch,
                                                   sg_clip, lf,
                                                   PET_COL_CARD565, &dirty) &&
                    NULL != sg_stage) {
                    /* invalidate_area 吃的是**屏幕坐标**：画布位置由 LVGL 的
                     * 布局决定，每拍取一次（它不会变，但别假设）。 */
                    lv_area_t coords;
                    lv_area_t inv;

                    lv_obj_get_coords(sg_stage, &coords);
                    inv.x1 = coords.x1 + dirty.x0;
                    inv.y1 = coords.y1 + dirty.y0;
                    inv.x2 = coords.x1 + dirty.x1;
                    inv.y2 = coords.y1 + dirty.y1;
                    lv_obj_invalidate_area(sg_stage, &inv);

                    sg_painted_clip  = sg_clip;
                    sg_painted_frame = lf;
                }
            }
        }
    }

    /* ---- 拍末立刻刷一次 ----
     * LVGL 自带的刷新定时器是 32ms、宠物 tick 是 42ms，两个周期不同步，相位
     * 每拍漂 10ms；漂到"刷新压在下一拍上"，就把 42ms 顶成 50~60ms。渲染完
     * 当拍刷掉，节奏就只由 tick 决定。 */
    {
        uint32_t t0 = (uint32_t)tal_system_get_millisecond();

        lv_refr_now(NULL);
        prv_dbg_refr((uint32_t)tal_system_get_millisecond() - t0, now);
    }
}

/***********************************************************
*********************** 页面 *********************************
***********************************************************/
/**
 * @brief 面板换页时放一个"进页面动作"。
 *
 *   进拍照页   -> fn_photo
 *   进备忘录/笔记页 -> fn_note
 *
 * 跑在 LVGL 线程（页面构建期间），只写请求位 —— 真正的切换在 tick 的帧边界上。
 */
static void prv_page_hook(PET_PAGE_E page)
{
    const char *name = NULL;

    switch (page) {
    case PET_PAGE_CAMERA:
        name = "fn_photo";
        break;
    case PET_PAGE_NOTE:
    case PET_PAGE_NOTES:
        name = "fn_note";
        break;
    default:
        break;
    }

    if (NULL != name) {
        int clip = prv_clip_by_name(name);

        if (clip >= 0) {
            sg_req_clip = clip;
            PR_NOTICE("[pet] 进页面动作 -> %s", name);
        }
    }
}

static void prv_rot_set(lv_display_rotation_t rot)
{
    lv_display_t *disp = lv_display_get_default();

    if (NULL != disp && lv_display_get_rotation(disp) != rot) {
        lv_display_set_rotation(disp, rot);
    }
}

/**
 * @brief 把实时精灵画进新 UI 左边那张卡。
 *
 * 舞台（pet_pet_stage()）是卡里气泡下面那块透明容器，尺寸由 demo 的布局给。
 * 画布是精灵的锚点框（app_sprite2d_size），水平居中、贴着舞台顶部 —— 和 demo
 * 原来摆静态图的方式一致。画布比舞台宽 1px 是透明边（角色内容只有 ~160 宽），
 * 被舞台裁掉看不出来；真超了会打一条 WARN。
 */
static void prv_stage_build(void)
{
    lv_obj_t *stage = pet_pet_stage();
    lv_coord_t stage_h;

    sg_stage = NULL;

    if (NULL == stage) {
        PR_WARN("[pet] 新 UI 没给出宠物舞台，跳过精灵");
        return;
    }

    if (OPRT_OK != app_sprite2d_init()) {
        PR_WARN("[pet] 精灵没起来（SD 上没有 %s？）", SP2D_PATH_SPF);
        return;
    }
    app_sprite2d_size(&sg_cw, &sg_ch);
    if (sg_cw <= 0 || sg_ch <= 0) {
        return;
    }

    sg_cbuf = (uint16_t *)tal_psram_malloc((size_t)sg_cw * (size_t)sg_ch * 2u);
    if (NULL == sg_cbuf) {
        PR_WARN("[pet] 画布缓冲分配失败（%dx%d）", sg_cw, sg_ch);
        return;
    }

    /* 底色 = 卡的米白（565 的 0xFFFF 就是 0xFFFEFD 量化后的值）。 */
    for (int i = 0; i < sg_cw * sg_ch; i++) {
        sg_cbuf[i] = PET_COL_CARD565;
    }

    sg_stage = lv_canvas_create(stage);
    lv_canvas_set_buffer(sg_stage, sg_cbuf, sg_cw, sg_ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_border_width(sg_stage, 0, 0);
    lv_obj_set_style_pad_all(sg_stage, 0, 0);
    lv_obj_set_style_radius(sg_stage, 0, 0);
    lv_obj_remove_flag(sg_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sg_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sg_stage, prv_stage_click, LV_EVENT_CLICKED, NULL);
    lv_obj_align(sg_stage, LV_ALIGN_TOP_MID, PET_CHAR_DX, PET_CHAR_TOP);

    /* 舞台高度是布局给的（卡高 - 气泡区）。画布比它高的话底部会被裁 —— 记一
     * 条日志，别让它悄悄发生。 */
    lv_obj_update_layout(stage);
    stage_h = lv_obj_get_content_height(stage);
    if (stage_h < sg_ch + PET_CHAR_TOP) {
        PR_WARN("[pet] 宠物舞台只有 %dpx 高，画布 %dpx + 顶部 %dpx 会被裁",
                (int)stage_h, sg_ch, PET_CHAR_TOP);
    }

    PR_NOTICE("[pet] 宠物画布 %dx%d 放进新 UI 左卡（舞台 %dpx 高）",
              sg_cw, sg_ch, (int)stage_h);

    /* 画布是新的：清掉"上一帧画过哪"（下一次从整块重新铺底），同帧跳过
     * 的记账也归零，别把上一次进页面的状态串到这一次。 */
    app_sprite2d_dirty_reset();
    sg_painted_clip  = -1;
    sg_painted_frame = -1;
}

static OPERATE_RET prv_pet_enter(void *arg)
{
    (void)arg;

    app_disp_lock();

    /* 横屏是**全产品**的决定，开机时就已经转好了（见 tuya_app_main.c）。
     * 这里再设一次是幂等的保险：万一以后有人调了启动顺序，这一页也不会竖着出来。 */
    prv_rot_set(LV_DISPLAY_ROTATION_90);

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, PET_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 新 UI：顶栏 + 左宠物卡 + 右功能面板（七页的页面栈） ----
     * 页面切换钩子先挂上：进拍照/备忘录页时它会请求对应动作。 */
    pet_ui_set_page_hook(prv_page_hook);
    pet_ui_init(sg_scr);

    /* ---- 把实时精灵放进左卡 ---- */
    prv_stage_build();

    sg_clip      = PET_CLIP_IDLE;
    sg_clip_at   = (uint32_t)tal_system_get_millisecond();
#if PET_ACT_AUDIT
    sg_clip      = -1;      /* 巡检：第一拍就切到"待机" */
    sg_audit_pos = 0;
#endif
    sg_clip_once = FALSE;
    sg_idle_at   = sg_clip_at;
    sg_hold_clip = -1;
    sg_req_clip  = -1;
    sg_req_hold  = -2;
    sg_req_say   = 0;
    sg_req_font  = FALSE;
    sg_ai_state  = AI_MODE_STATE_IDLE;

    /* 动作由这三条事件驱动（大模型点名 / AI 情绪 / AI 状态），见上面的状态机说明。
     * 订阅是幂等的，页面重进不会叠加。 */
    app_event_subscribe(APP_EVT_PET_ACTION, prv_pet_evt_cb, NULL);
    app_event_subscribe(APP_EVT_AI_EMOTION, prv_pet_evt_cb, NULL);
    app_event_subscribe(APP_EVT_AI_STATE,   prv_pet_evt_cb, NULL);
    app_event_subscribe(APP_EVT_FONT_READY, prv_pet_evt_cb, NULL);

    lv_screen_load(sg_scr);

    sg_tick = lv_timer_create(prv_tick_cb, PET_TICK_MS, NULL);

    app_disp_unlock();

    PR_NOTICE("[pet] 页面就绪（新 UI，宠物 %dx%d，素材 %d 组，%s）",
              sg_cw, sg_ch, app_sprite2d_clip_count(),
              (NULL != sg_cbuf) ? "已加载" : "**没有宠物**");
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
    app_event_unsubscribe(APP_EVT_FONT_READY, prv_pet_evt_cb);

    if (NULL != sg_tick) {
        lv_timer_del(sg_tick);
        sg_tick = NULL;
    }

    /* 先把 UI 拆掉（它会 reset 气泡指针、删掉整棵对象树），再删屏幕。
     * 顺序反了的话，pet_say() 可能在这中间写进已经释放的对象。 */
    pet_ui_set_page_hook(NULL);
    pet_ui_deinit();
    if (NULL != sg_scr) {
        lv_obj_del(sg_scr);
        sg_scr = NULL;
    }
    sg_stage = NULL;

    if (NULL != sg_cbuf) {
        tal_psram_free(sg_cbuf);
        sg_cbuf = NULL;
    }

    /* 放掉预取槽的占用、清掉"上一帧"状态 —— 别串到下一次进页面。 */
    app_sprite2d_dirty_reset();

    /* **不要把旋转调回 0** —— 横屏是全产品的（开机画面、SD 门页都在这个坐标系
     * 里），这一页离开不该把整个产品的方向掰回去。 */

    app_disp_unlock();
}

/***********************************************************
*********************** 系统按键 *****************************
***********************************************************/
/* P12 = **回退键**。
 *
 *   短按：弹窗开着 -> 关弹窗（主页面上的弹窗也是这一条）
 *         有面板 sheet 开着 -> 关 sheet
 *         否则 -> 面板回上一页（主页面本身就是栈底，pet_ui_back() 是空操作）
 *   长按：进"设置"页。
 *
 * 跑在**路由线程**（不是 LVGL 线程），所以碰界面都要拿显示锁 ——
 * app_statusbar_close_popup() 内部自己加锁，pet_ui_* 这几个由这里包住。 */
static void prv_pet_key(APP_KEY_E key, APP_KEY_EVT_E evt)
{
    if (APP_KEY_BACK != key) {
        return;
    }

    if (APP_KEY_EVT_SHORT == evt) {
        if (app_statusbar_close_popup()) {
            return;                     /* 弹窗优先：只关弹窗，不切页 */
        }

        app_disp_lock();
        if (pet_ui_sheet_active()) {
            pet_ui_close_sheet();       /* 模态卡优先于页面回退 */
        } else {
            pet_ui_back();              /* 主页面时是空操作 */
        }
        app_disp_unlock();
    } else if (APP_KEY_EVT_LONG == evt) {
        /* 弹窗开着时长按也只关弹窗，不在弹窗底下偷偷换页。 */
        if (app_statusbar_close_popup()) {
            return;
        }

        app_disp_lock();
        pet_ui_go(PET_PAGE_SETTINGS);
        app_disp_unlock();
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
