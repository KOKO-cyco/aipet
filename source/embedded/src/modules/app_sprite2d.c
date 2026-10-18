/**
 * @file app_sprite2d.c
 * @brief 2D 精灵帧序列播放器。见 app_sprite2d.h。
 *
 * 渲染核心只有一件事：**把 ARGB4444 的一帧按 alpha 顺序混合到目标缓冲上**。
 * 没有仿射、没有插值、没有每像素矩阵运算 —— 实测约 2ms/帧（骨骼方案是 8~9ms）。
 *
 * 两级跳过透明区（角色只占整帧面积的 44%，不做这个等于白付 56% 的访存）：
 *   1. 整行透明 → 整行不做（spans 表在加载时算好）
 *   2. 行内 alpha==0 → 不读目标、不写回
 *
 * 两个入口
 * --------
 *   app_sprite2d_render_clip()  调用方点名"第几组、第几帧"，自己控节奏
 *                               （桌宠页就是这么用的：它按自己的状态机选组）
 *   app_sprite2d_render_to()    自动调度：待机随机轮播 + 一次性动作插播，
 *                               调用方只管给时间轴
 *
 * 老产品还有一个"浮层"模式（LVGL pre-flush 钩子，把精灵直接画进面板
 * framebuffer，带拖拽 / 贴边收放）—— 那部分约 1000 行，随老主页一起删掉了：
 * 桌宠页用 `lv_canvas` 画精灵，旋转和位置都由 LVGL 管，不需要浮层那套。
 */

#include "tal_api.h"
#include "tal_system.h"
#include "tkl_fs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "tdl_display_manage.h"

#include "app_sprite2d.h"
#include "app_spf6.h"           /* .spf 容器解析 */
#include "app_storage.h"        /* app_storage_card_present() / app_storage_init() */

/** 连续读坏多少帧才重建链路（**"读错"那条路**，见 app_storage.h）。
 *  取 6（~30fps ≈ 200ms）：**必须大于 CD 去抖窗口（100ms）**，否则卡拔掉
 *  后、CD 还没确认的那段时间里会误以为"卡在、只是读坏了"而白重建一趟。 */
#define SP2D_READ_RECOVER_AFTER  6

/* ======================= 数据 ======================= */

/* `sp2d_span_t` / `sp2d_role_e` / `sp2d_clip_t` 和整个文件格式的解析都搬去了
 * `app_spf6.c` —— 角色页（app_role2d.c）要读同一个格式，解析器不能有两份。
 * 见 app_spf6.h。 */

/** 一帧的完整描述。各组帧尺寸不同，所以这些不能再从全局取。 */
typedef struct {
    const uint16_t    *px;
    const sp2d_span_t *spans;
    int                w, h;
    int                dx, dy;
} sp2d_fr_t;

typedef struct {
    /* 文件与表：头、名字表、偏移表、动作表全在它里面（app_spf6.h） */
    SPF6_FILE_T  file;

    /* 锚点框：所有组内容框的并集（缩放后坐标，从 (0,0) 起）。
     * 各组靠自己的 dx/dy 落在框里的不同位置 —— 所以**切动作时角色不会跳**。
     * 夹紧位置必须用它而不是某一组的帧尺寸，否则角色会停在离屏幕边缘
     * 十几像素的地方，看着像"拖不到边"。 */
    int          bx0, by0, bx1, by1;

    /* 当前这一帧的描述（prv_frame_at 填）。绘制只认它。 */
    sp2d_fr_t    fr;

    /* ---- 常驻 idle：**存 RLE，不存解码帧** ----
     *
     * SPF6 起整组待机以 PackBits **压缩态**常驻。实测：48 帧解码态要 1.96 MB，
     * 而 PSRAM 拿不到那么大的连续块（加载器会报"idle 常驻分配失败"）；压缩态
     * 只有 0.89 MB，稳过。
     *
     * 用的时候解一帧到 idle_px。**同一帧不重复解** —— tick 是 30fps、待机只有
     * 16fps，多数调用命中缓存，解压只发生在换帧那一下。 */
    uint8_t     *idle_rle;      /**< 整组的 RLE 数据 */
    uint32_t    *idle_off;      /**< 帧号 -> idle_rle 内的偏移（frames+1 项） */
    uint32_t     idle_rle_len;
    uint16_t    *idle_px;       /**< **一帧**的解码缓冲（不是整组） */
    sp2d_span_t *idle_spans;
    uint32_t     idle_frames;
    uint32_t     idle_decoded;  /**< idle_px 里解的是第几帧；0xFFFFFFFF = 没有 */

    /* ---- SPF6 帧索引 ---- */
    uint32_t    *offs;          /**< 全局帧号 -> 文件偏移（total_frames+1 项） */
    uint32_t     total_frames;
    uint8_t     *rle_buf;       /**< 流式帧的 RLE 暂存（按最坏情况分配） */
    uint32_t     rle_cap;

    /* ---- 每组首帧常驻（切组那拍不读卡）----
     *
     * 切组那一拍要额外做一次**跨组 f_lseek + 读帧 + inflate**：fatfs 没开
     * FF_USE_FASTSEEK，跨组 seek 要顺着簇链走一段，实测那拍从 42ms 拉到
     * 54~58ms —— 这就是"一切换就卡一下"。待机之所以不卡，是因为它整组都在
     * PSRAM 里（见 idle_rle）。
     *
     * 所以把**每组第 0 帧**在开机时解好常驻：切组永远从第 0 帧进，那一拍就只是
     * blit，完全不碰 SD。约 0.8 MB PSRAM，换掉切换处那一停。
     * 用户 2026-10-18 的要求原话：**"宁可切换不连贯，也不要卡顿"**。
     *
     * 某一组分配/读取失败就留 NULL，照旧走流式读 —— 功能不受影响，只是那组
     * 切进来时还会顿一下。 */
    uint16_t   **head_px;       /**< [clip_count] 每组首帧的解码结果 */
    sp2d_span_t **head_spans;   /**< [clip_count] */

    /* ---- 流式：当前从 SD 读进来的那一帧 ---- */
    uint16_t    *cur_px;
    sp2d_span_t *cur_spans;
    uint32_t     cur_frame;     /**< 缓冲里是全局第几帧；SP2D_NO_FRAME = 无效 */
    uint32_t     fail_count;    /**< 读失败次数（诊断用） */
    uint32_t     fail_log_t;    /**< 上次打印读失败的时刻，限流用 */
    uint32_t     bad_run;       /**< 连续读坏帧数（成功即清零）—— 攒够就重建链路 */

    /* ---- 文件句柄与布局 ---- */
    TUYA_FILE    f;              /**< 精灵文件句柄；NULL = 没开 */
    uint32_t     data_off;      /**< 帧数据在文件里的起始偏移 */

    BOOL_T       loaded;
} sp2d_data_t;

#define SP2D_NO_FRAME   0xFFFFFFFFu

static sp2d_data_t sg_data;
static SP2D_STAT_T sg_stat;

/* 诊断用（2026-10-14）：上次打印过的组号，用来只在"换组"时打一行渲染日志。 */
static uint32_t sg_dbg_clip = 0xFFFFFFFFu;

/** 常驻的是第几组动作（图集里第 0 行 = 待机+眨眼）。 */
#define SP2D_IDLE_CLIP      0

/* ---- 切组卡顿探针（2026-10-17）--------------------------------------------
 *
 * 现象：**切到另一个动作的那一下会停住**，动作本身播得顺（用户 2026-10-17）。
 * 差别在于 idle 整组常驻 PSRAM、不碰 SD，其余组每帧现读 —— 所以切进动作组
 * 的那一拍要付一次"跨几 MB 的 fseek + 读 40~60KB + 解压"，而 tick 只有 33ms。
 *
 * 打三类日志，定位完把 SP2D_TRACE_SWITCH 置 0 就全关（不影响任何行为）：
 *   ① 切组后前 FOLLOW_N 拍的实际间隔 —— **第一拍那个数就是用户感觉到的停顿时长**
 *   ② **全程**任意一拍间隔超过 SLOW_TICK_MS（基准 42ms 的 ~15%）就报 ——
 *      2026-10-18 复盘：上一版只跟切换后 3 拍，用户报"开心最卡"时日志里
 *      17 次切换全部 49~53ms 无一超时，其实说明卡的动作没被覆盖到；
 *      这一版把"卡在哪一拍"的网撒到动作全程（原慢帧阈值 25ms 太粗，
 *      6~24ms 的微顿感知得到但不记录，是当时的盲区）
 *   ③ 单帧读（seek + read + RLE 解压 + 建 spans）超过 TRACE_SLOW_MS 就报一条 */
#define SP2D_TRACE_SWITCH      1
#define SP2D_TRACE_SLOW_MS     25u
#define SP2D_TRACE_FOLLOW_N    8
#define SP2D_TRACE_SLOW_TICK   48u     /* 大于此值的拍间隔 = 可疑（基准 42ms）*/


/* PackBits 解压搬去了 app_spf6.c（`spf6_inflate`）—— 角色页也要解同一个格式。 */

/** 一帧里**角色**（alpha 非 0）的包围盒 —— 只用来打日志。
 *
 * 为什么需要它：日志里的 "idle 130x165" 是**裁剪框**，不是角色大小。
 * 框 = 角色 + pad + 起跳余量，各动作差很多（吃晚餐那三盘菜把框撑到 271 宽，
 * 点开心跳到最高那帧把框撑到 194 高），单看框会误以为待机角色小一圈。
 * 真正可比的是这个数：角色高。 */
static void prv_char_bbox(const uint16_t *px, int w, int h,
                          int *x0, int *y0, int *x1, int *y1)
{
    *x0 = w; *y0 = h; *x1 = -1; *y1 = -1;

    for (int y = 0; y < h; y++) {
        const uint16_t *row = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            if (0 == (row[x] & 0xF000u)) {
                continue;
            }
            if (x < *x0) { *x0 = x; }
            if (x > *x1) { *x1 = x; }
            if (y < *y0) { *y0 = y; }
            if (y > *y1) { *y1 = y; }
        }
    }
}

/* 行跨度扫描搬去了 app_spf6.c（`spf6_build_spans`）。 */

/** 全局帧下标 -> 它属于哪一组。找不到返回 NULL（表坏了才会）。 */
static const sp2d_clip_t *prv_clip_of(uint32_t idx)
{
    for (uint16_t i = 0; i < sg_data.file.clip_count; i++) {
        const sp2d_clip_t *c = &sg_data.file.clips[i];

        if (idx >= c->first && idx < c->first + c->frames) {
            return c;
        }
    }
    return NULL;
}

/* ---- 播放状态 -------------------------------------------------------------
 *
 * 分三类：
 *   **常态待机** —— 一直循环待机组，直到有事情发生；
 *   **一次性动作** —— 点击回应（演一遍）、拖拽起手/落地（演一遍）；
 *   **循环动作**   —— 随机表演（演 SP2D_RANDOM_REPS 遍）、拖拽悬空 / 说话 / 贴边
 *                     （手按着或 AI 说着就一直循环）。
 *
 * 这三件事都由 LVGL 线程驱动（tick / 触摸回调 / 合成都在同一个任务里），
 * 所以**不需要加锁**。
 */
static struct {
    int      clip;      /**< 正在播的动作组号；-1 = 待机中 */
    uint32_t t0;        /**< 这一遍从哪一刻开始（用 tick 的时间轴，ms） */
    uint16_t reps;      /**< 播几遍。**0 = 无限循环**（拖拽悬空 / 说话 / 贴边），
                             1 = 演一遍（点击 / 起手 / 落地），
                             N = 循环 N 遍（随机表演，见 SP2D_RANDOM_REPS） */
} sg_play = { -1, 0, 1 };

/* 下一次"随机表演"的时刻（tick 时间轴）。0 = 还没排过。 */
static uint32_t sg_rand_at = 0;

/* 一个够用的伪随机源 —— 这里只是挑"播哪一组 / 隔多久演一次"，不需要好随机。
 * 不用 rand() 是因为它在裸机上没有播种来源，而且 libc 的 rand 会拖进一堆东西。 */
static uint32_t sg_rand_state = 0x12345678u;

static uint32_t prv_rand(void)
{
    /* xorshift32 */
    uint32_t x = sg_rand_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sg_rand_state = x;
    return x;
}

/** 每个**随机表演**循环几遍。1 = 只演一遍，2 = 演两遍再回待机。
 *
 *  素材本身只有几秒，演一遍就闪过去了，看着像"抖了一下"；循环两遍才看得出是在
 *  表演。点击 / 拖拽起手 / 落地**不受这个影响**（它们必须演一遍就收）。 */
#define SP2D_RANDOM_REPS    1

/** 两个动作之间隔多久（ms）—— **从上一个动作演完那一刻起算**。
 *
 *  以前是"从动作**开始**起算"（在 prv_play_role 里排的闹钟），动作越长留给待机
 *  的时间越短 —— 长动作演完紧接着就是下一个，短动作却能歇很久，节奏不齐。
 *  改成从**结束**起算才是真的"每两个动作之间隔一个待机"。
 *
 *  同时这个值也用作"用户刚操作完，别马上自己演一个"的冷却。 */
#define SP2D_GAP_MIN_MS     6000u
#define SP2D_GAP_MAX_MS     16000u

/** 按下到松手，位移小于这么多像素算"点击"，否则算"拖拽"（px）。 */
#define SP2D_TAP_SLOP       12

/** 把"下一个动作"的闹钟排到 t_ms 之后 SP2D_GAP_MIN..MAX 毫秒。
 *
 *  两处调它：**动作开始时**（用户刚操作完，别马上自己演一个）和
 *  **动作演完时**（"每两个动作之间隔一个待机"就是靠这一处）。 */
static void prv_arm_rand(uint32_t t_ms)
{
    sg_rand_at = t_ms + SP2D_GAP_MIN_MS
                 + prv_rand() % (SP2D_GAP_MAX_MS - SP2D_GAP_MIN_MS);
}

/* ---- 动作组时长与选帧 ---- */

/** 本组一轮的时长（ms）。fps 缺省按 12 兜底，免得除零。 */
static uint32_t prv_clip_dur_ms(uint32_t i)
{
    const sp2d_clip_t *c = &sg_data.file.clips[i];
    uint32_t step = 1000u / (c->fps ? c->fps : 12);

    return (0 == step) ? c->frames : (uint32_t)c->frames * step;
}

/** 从某个角色里随机挑一组。找不到返回 -1。 */
static int prv_pick_role(sp2d_role_e role)
{
    int cand[SP2D_MAX_CLIPS];
    int n = 0;

    for (uint16_t i = 0; i < sg_data.file.clip_count && n < SP2D_MAX_CLIPS; i++) {
        if (sg_data.file.clips[i].role == (uint16_t)role) {
            cand[n++] = (int)i;
        }
    }
    if (0 == n) {
        return -1;
    }
    return cand[prv_rand() % (uint32_t)n];
}

/**
 * @brief 起一个动作。
 *
 * @param[in] reps   播几遍。**0 = 无限循环**（拖拽悬空 / 说话 / 贴边），
 *                   1 = 演一遍（点击 / 起手 / 落地），
 *                   N = 循环 N 遍（随机表演，见 SP2D_RANDOM_REPS）
 * @param[in] force  TRUE = 打断当前动作；FALSE = 正在演别的就让给它
 * @return TRUE = 真的换成了这一组；FALSE = 素材里没有这个角色（什么都没做）
 */
static BOOL_T prv_play_role(sp2d_role_e role, uint32_t t_ms, uint16_t reps, BOOL_T force)
{
    int c;
    int old;

    if (!force && sg_play.clip >= 0) {
        return FALSE;                       /* 正在演别的：不打断 */
    }

    c = prv_pick_role(role);
    if (c < 0) {
        return FALSE;                       /* 这套素材里没有这个角色 */
    }

    old = sg_play.clip;
    sg_play.clip = c;
    sg_play.t0   = t_ms;
    sg_play.reps = reps;

    /* 用户刚操作完，把"下一个动作"推远，免得刚点完又自己演一个。
     * （动作**演完**时会再排一次 —— 那次才是"动作之间的待机"。） */
    prv_arm_rand(t_ms);

    /* 诊断（2026-10-14）：换组是"起跳那一下抽动"的另一个嫌疑 —— 换组后
     * dx/dy 和夹紧框都跟着变。 */
    PR_NOTICE("[sp2d] 换动作 %d -> %d（角色 %d reps %u 帧数 %u dxdy %d,%d）",
              old, c, (int)role, (unsigned)reps,
              (unsigned)sg_data.file.clips[c].frames,
              sg_data.file.clips[c].dx, sg_data.file.clips[c].dy);
    return TRUE;
}

/** 回待机（松手 / 演完）。 */
static void prv_play_idle(uint32_t t_ms)
{
    /* 诊断（2026-10-14）：回待机会把夹紧框换回待机组那个（105x129），
     * 和拖拽组的框（118x145）差 16px。 */
    if (sg_play.clip >= 0) {
        PR_NOTICE("[sp2d] 回待机（原组 %d）", sg_play.clip);
    }

    sg_play.clip = -1;
    sg_play.t0   = t_ms;
    sg_play.reps = 1;

    /* **每两个动作之间隔一个待机** —— 回待机的这一刻排下一次动作的闹钟，
     * 所以间隔是从"上一个演完"算起的，不是从"上一个开始演"算起的。 */
    prv_arm_rand(t_ms);
}

/**
 * @brief 按当前状态选出"第几组动作 + 组内第几帧"。
 *
 * 三种情形：
 *   一次性动作在演   -> 播它；演满一轮就回待机
 *   拖拽中           -> 循环播 drag 组，直到松手
 *   其它             -> 循环播待机组
 */
static void prv_pick(uint32_t t_ms, uint32_t *clip_out, uint32_t *frame_out)
{
    if (sg_play.clip >= 0) {
        const sp2d_clip_t *c = &sg_data.file.clips[sg_play.clip];
        uint32_t step = 1000u / (c->fps ? c->fps : 12);

        if (0 == step) {
            step = 1;
        }

        uint32_t el = t_ms - sg_play.t0;

        /* reps = 0 是无限循环；否则总时长 = 单遍时长 x 遍数。
         * 帧号照常取模，所以"循环 N 遍"就是同一段连播 N 次。 */
        uint32_t total = prv_clip_dur_ms((uint32_t)sg_play.clip)
                         * (0 == sg_play.reps ? 1u : (uint32_t)sg_play.reps);

        if (0 == sg_play.reps || el < total) {
            *clip_out  = (uint32_t)sg_play.clip;
            *frame_out = c->first + ((el / step) % (c->frames ? c->frames : 1));
            return;
        }

        /* 演完这一轮：**起手有自己的后继**（接着悬空循环），其余回待机。 */
        if ((uint16_t)SP2D_ROLE_DRAG_UP == c->role) {
            if (!prv_play_role(SP2D_ROLE_DRAG, t_ms, 0, TRUE)) {
                prv_play_idle(t_ms);        /* 素材里没有"悬空"那一组 */
            }
        } else {
            /* 演完 -> 回待机（prv_play_idle 里会把下一个动作的闹钟排到
             * "从现在起 + 一个待机"，这就是"每两个动作之间隔一个待机"） */
            prv_play_idle(t_ms);
        }

        /* **换成后继之后必须按新组重新算一遍，然后 return。**
         *
         * 原来这里直接往下掉进下面那段"画待机"，于是**切换后的第一帧画的是待机组**
         * —— 上机表现就是"起跳那一下抽一下 / 沉一下"，而且整个拖动只出现这一次
         * （只在起手演完、接悬空的那一帧）。2026-10-14 用日志抓到的原话：
         *
         *     换动作 8 -> 9（角色 2 reps 0 帧数 1 dxdy 47,3）
         *     渲染 组 8->0 帧=0 锚点=(85,260) 本组dxdy=(50,18) 实画=(135,278)
         *     渲染 组 0->9 帧=582 锚点=(81,252) 本组dxdy=(47,3) 实画=(128,255)
         *
         * 组 8/9 的 dxdy 都是 (47,3)，待机是 (50,18) —— **差 15px 且朝下**。
         * 递归只有一层：后继要么是无限循环组（reps=0，走上面那个 return），
         * 要么是待机（走下面的 idle 分支），都不会再掉到这里。 */
        prv_pick(t_ms, clip_out, frame_out);
        return;
    }

    {
        const sp2d_clip_t *c = &sg_data.file.clips[SP2D_IDLE_CLIP];
        uint32_t step = 1000u / (c->fps ? c->fps : 12);

        if (0 == step) {
            step = 1;
        }

        *clip_out  = SP2D_IDLE_CLIP;
        *frame_out = c->first + (((t_ms - sg_play.t0) / step)
                                 % (c->frames ? c->frames : 1));
    }
}


/* ======================= 像素 ======================= */

#define SP2D_DIV15(x)   (((x) * 4369) >> 16)

/**
 * @brief ARGB4444 源像素按 4bit alpha 混合到 RGB565 目标上。
 *
 * 位宽必须统一：混合要在同一精度里做再转回 RGB565。直接拿 4bit 分量往
 * RGB565 的 5/6 位字段里塞会只填到低位 —— 结果整体暗一半。
 */
static inline uint16_t prv_over565(uint16_t argb, uint16_t dst565)
{
    uint32_t a = (argb >> 12) & 0xF;

    if (0 == a) {
        return dst565;
    }

    uint32_t r, g, b;

    if (15 == a) {
        r = (argb >> 8) & 0xF;
        g = (argb >> 4) & 0xF;
        b =  argb       & 0xF;
    } else {
        uint32_t dr = ((dst565 >> 11) & 0x1F) >> 1;
        uint32_t dg = ((dst565 >>  5) & 0x3F) >> 2;
        uint32_t db = ( dst565        & 0x1F) >> 1;
        uint32_t inv = 15 - a;

        r = SP2D_DIV15(((argb >> 8) & 0xF) * a + dr * inv);
        g = SP2D_DIV15(((argb >> 4) & 0xF) * a + dg * inv);
        b = SP2D_DIV15(( argb       & 0xF) * a + db * inv);
    }

    /* 4bit -> RGB565，补低位，保证 15 映射到满量程 */
    return (uint16_t)(((r * 2 + r / 8) << 11) |
                      ((g * 4 + g / 4) <<  5) |
                       (b * 2 + b / 8));
}

static uint32_t prv_blit_frame(uint16_t *dst, int dstride,
                               const uint16_t *src, int sstride,
                               const sp2d_span_t *spans, int x_ofs,
                               int w, int h)
{
    uint32_t px = 0;

    for (int y = 0; y < h; y++) {
        /* spans 是帧坐标下的范围；src 已按裁剪偏移，所以要减掉 x_ofs */
        int x0 = (int)spans[y].x0 - x_ofs;
        int x1 = (int)spans[y].x1 - x_ofs;

        if (x0 < 0)  x0 = 0;
        if (x1 > w)  x1 = w;
        if (x0 >= x1) {
            continue;
        }

        const uint16_t *s = src + (size_t)y * sstride;
        uint16_t *d = dst + (size_t)y * dstride;

        px += (uint32_t)(x1 - x0);

        for (int x = x0; x < x1; x++) {
            uint16_t v = s[x];

            if (v & 0xF000u) {              /* alpha == 0 直接跳过 */
                d[x] = prv_over565(v, d[x]);
            }
        }
    }

    return px;
}


/* ======================= 从 SD 加载 ======================= */

/* prv_rd16 / prv_rd32 搬去了 app_spf6.c。 */

/**
 * @brief 从文件 @a off 处读 @a len 字节到 @a buf。失败返回 FALSE（调用方退回 idle）。
 *
 * **读不到就退回 idle，不重试、不打日志。** 卡不在位时每一帧都会走到这里，
 * 重试只是把串口刷满（实测每帧两条 seek 失败 + 一条读失败）—— 而"卡没了"路由
 * 早就知道、也已经把"请插入 SD 卡"页弹出来了（见 app_router.h 的
 * APP_PAGE_NEEDS_SD）。退回**常驻在 PSRAM 里的 idle 组**就够：角色不会消失、
 * 也不会花屏，这正是"掉线就一直播 idle"。
 *
 * 卡在位但偶发读到坏数据也一样跳过 —— 下一帧会重新读，不需要链路重建那套。
 *
 * **这一层的策略（判卡在不在、什么时候重建链路）刻意留在本文件**，没跟着格式
 * 解析一起搬进 app_spf6.c：角色页读不到就是不画，浮层读不到要一直兜着 idle，
 * 两边的容错要求根本不同。app_spf6.c 只提供纯 seek+read（spf6_read_at）。
 */
static BOOL_T prv_read_at(uint32_t off, uint32_t len, void *buf)
{
    if (!app_storage_ready()) {
        return FALSE;                       /* 卡没了 / 链路判死：静默退回 idle */
    }
    return spf6_read_at(&sg_data.file, off, len, buf);
}

/**
 * @brief 从 SD 读一帧到流式缓冲。失败返回 FALSE（调用方退回 idle）。
 *
 * 只在"要显示的帧变了"时才读，所以 12fps 下大约每 83ms 读一次。
 *
 * SPF4 之后各组的帧尺寸不同，所以偏移不能再用"下标 x 固定帧长"算 —— 组的
 * 首帧偏移在载入时就记好了（clips[i].data_off），这里只加组内偏移。
 */
static BOOL_T prv_read_frame(uint32_t idx)
{
    const sp2d_clip_t *c = prv_clip_of(idx);
    uint32_t a, b;

#if SP2D_TRACE_SWITCH
    uint32_t t_cost = (uint32_t)tal_system_get_millisecond();
#endif

    if (NULL == c || idx + 1u >= sg_data.file.total_frames + 1u) {
        return FALSE;
    }

    /* SPF6：这一帧的文件区间 = 偏移表的相邻两项。RLE 是变长的，所以不能像
     * 老格式那样"下标 x 固定帧长"算偏移。 */
    a = sg_data.file.offs[idx];
    b = sg_data.file.offs[idx + 1u];

    if (b <= a || (b - a) > sg_data.rle_cap) {
        PR_ERR("[sp2d] 第 %u 帧 RLE 长度异常（%u..%u，暂存上限 %u）",
               (unsigned)idx, (unsigned)a, (unsigned)b,
               (unsigned)sg_data.rle_cap);
        return FALSE;
    }

    if (TRUE != prv_read_at(a, b - a, sg_data.rle_buf)) {
        sg_data.fail_count++;

        /* **先判卡在不在，再判是不是"读错" —— 两条路在这里就分开了。**
         *
         * 卡不在 -> 这是"卡掉线"，走 CD + 路由那条路（弹"请插入 SD 卡"页），
         *           这里**什么都不做**，顺手把连续计数清零 —— 免得卡插回来之后
         *           拿着卡不在时攒的旧计数立刻误触发一次重建。
         * 卡在   -> 才是"读错"（SDIO 死态），攒够 SP2D_READ_RECOVER_AFTER 帧
         *           才重建链路。 */
        if (!app_storage_card_present()) {
            sg_data.bad_run = 0;
            return FALSE;
        }
        if (++sg_data.bad_run >= SP2D_READ_RECOVER_AFTER) {
            sg_data.bad_run = 0;
            PR_ERR("[sp2d] 连续 %u 帧读坏（卡在位），重建 SD 链路",
                   (unsigned)SP2D_READ_RECOVER_AFTER);
            /* **角色不弹页、不报故障。** 这里是浮层，不是页面：卡坏了也一样退回
             * 常驻 PSRAM 的 idle 组，下一帧接着重试 —— 用户看到的就是"角色一直在，
             * 只是没换动作"，这比在他看别的页面时突然弹一张"SD 卡损坏"要合适
             * （见 app_router.h 的 APP_PAGE_NO_OVERLAY 与 page_nosd.c）。 */
            if (APP_SD_OK == app_storage_recover_reopen(&sg_data.file.f, SP2D_PATH_SPF)) {
                return prv_read_at(a, b - a, sg_data.rle_buf);
            }
        }
        return FALSE;
    }

    sg_data.bad_run = 0;
    spf6_inflate(sg_data.rle_buf, b - a, sg_data.cur_px, (uint32_t)c->w * c->h);
    spf6_build_spans(sg_data.cur_px, sg_data.cur_spans, 1, c->w, c->h);

#if SP2D_TRACE_SWITCH
    {
        uint32_t dt = (uint32_t)tal_system_get_millisecond() - t_cost;

        if (dt >= SP2D_TRACE_SLOW_MS) {
            PR_NOTICE("[sp2d] 读帧慢 %u ms：%s 第 %u 帧（全局 %u），RLE %u KB",
                      (unsigned)dt, c->name, (unsigned)(idx - c->first),
                      (unsigned)idx, (unsigned)((b - a) / 1024u));
        }
    }
#endif

    return TRUE;
}

/** 把 sg_data.fr 填成"常驻 idle 里的第 ii 帧"（按需解压）。 */
static void prv_fr_set_idle(uint32_t ii)
{
    const sp2d_clip_t *ic = &sg_data.file.clips[SP2D_IDLE_CLIP];

    /* **只在换帧时解一次。** tick 是 30fps、待机只有 16fps，所以三次调用里有两次
     * 是同一帧 —— 解一帧虽然只要几十微秒（就是个循环），也没必要每帧都做。 */
    if (ii != sg_data.idle_decoded && NULL != sg_data.idle_rle) {
        uint32_t a = sg_data.idle_off[ii];
        uint32_t b = sg_data.idle_off[ii + 1u];

        spf6_inflate(sg_data.idle_rle + a, b - a,
                    sg_data.idle_px, (uint32_t)ic->w * ic->h);
        spf6_build_spans(sg_data.idle_px, sg_data.idle_spans, 1, ic->w, ic->h);
        sg_data.idle_decoded = ii;
    }

    sg_data.fr.px    = sg_data.idle_px;
    sg_data.fr.spans = sg_data.idle_spans;
    sg_data.fr.w     = ic->w;
    sg_data.fr.h     = ic->h;
    sg_data.fr.dx    = ic->dx;
    sg_data.fr.dy    = ic->dy;
}

/**
 * @brief 取第 @a idx 帧的完整描述（像素 / 行范围 / 尺寸 / 偏移）。取不到返回 NULL。
 *
 * idle 组常驻内存，直接返回；其它组从 SD 流式读进单帧缓冲。
 *
 * 读失败一律**切回常驻的 idle**，不拿上一帧顶着。上一帧属于别的动作，定格在
 * 那儿看着像卡死；而 idle 常驻 PSRAM，任何时刻都能立刻拿出来，是唯一"一定画
 * 得对"的画面。cur_frame 不动，所以下一帧还会重试当前动作。
 */
static const sp2d_fr_t *prv_frame_at(uint32_t idx)
{
    const sp2d_clip_t *c = prv_clip_of(idx);

    /* **"连 idle 都没有"不能在这一层否掉**（2026-10-16）：idle 常驻分配失败
     * （PSRAM 不够拿 6.4 MB 的 idle RLE）时会整组退化成流式读，那时
     * idle_frames = 0、每一帧都走下面的 SD 流式读，和别的组一模一样。
     * 以前这里把 "0 == idle_frames" 也一起 return NULL，结果 idle 一退化就
     * **整只宠物消失** —— 日志表现就是 "[sp2d] idle 常驻分配/读取失败"之后
     * 一帧都不画，而点击 / 随机插播的打印照打（渲染被否在 prv_frame_at，
     * 页面调度在更外层，互不影响）。 */
    if (NULL == c) {
        return NULL;                        /* 表坏了：这一帧不画 */
    }

    if (idx < sg_data.idle_frames) {
        prv_fr_set_idle(idx);
        return &sg_data.fr;
    }

    /* 首帧命中常驻（见 head_px 的字段注释）：切组那一拍直接 blit，不跨组 seek、
     * 不 inflate —— 这就是"切换不再顿一下"的来源。
     * 只认**组内第 0 帧**：切组永远从 0 开始，其余帧照常流式读。 */
    if (0 == (idx - c->first) && NULL != sg_data.head_px) {
        uint32_t gi = (uint32_t)(c - sg_data.file.clips);

        if (NULL != sg_data.head_px[gi]) {
            sg_data.fr.px    = sg_data.head_px[gi];
            sg_data.fr.spans = sg_data.head_spans[gi];
            sg_data.fr.w     = c->w;
            sg_data.fr.h     = c->h;
            sg_data.fr.dx    = c->dx;
            sg_data.fr.dy    = c->dy;
            return &sg_data.fr;
        }
    }

    if (idx != sg_data.cur_frame) {
        if (prv_read_frame(idx)) {
            sg_data.cur_frame = idx;
        } else {
            /* 读失败 —— 退到 idle。用 idx 对 idle 帧数取模，兜底期间 idle 照样在动，
             * 而不是冻在一张脸上。 */
            uint32_t now = (uint32_t)tal_system_get_millisecond();

            /* **卡不在位时不报**：那是"路由已经弹了请插卡页"的正常状态，每秒报一条
             * 只会把日志刷满。卡在位却读失败才值得报（那才是真故障）。 */
            if (app_storage_card_present() && (now - sg_data.fail_log_t >= 1000u)) {
                sg_data.fail_log_t = now;
                PR_ERR("[sp2d] 读第 %u 帧失败（累计 %u 次）—— 切 idle 顶上",
                       (unsigned)idx, (unsigned)sg_data.fail_count);
            }

            /* **没有常驻 idle 可退时**（idle 常驻分配失败、整组退化流式）：
             * idle_px 是 NULL，不能解压；idle_frames 是 0，不能取模。
             * 这一帧不画，下一帧 tick 还会重试当前帧。 */
            if (NULL == sg_data.idle_px) {
                return NULL;
            }

            sg_stat.fallback = 1;
            prv_fr_set_idle(idx % sg_data.idle_frames);
            return &sg_data.fr;
        }
    }

    sg_data.fr.px    = sg_data.cur_px;
    sg_data.fr.spans = sg_data.cur_spans;
    sg_data.fr.w     = c->w;
    sg_data.fr.h     = c->h;
    sg_data.fr.dx    = c->dx;
    sg_data.fr.dy    = c->dy;
    return &sg_data.fr;
}

OPERATE_RET app_sprite2d_init(void)
{
    if (sg_data.loaded) {
        return OPRT_OK;                     /* 幂等 */
    }

    /* 挂载交给存储服务（幂等，只在真没挂载时才动手）。
     *
     * **开机自动开启浮层时必须先有这一步**：`APP_STORAGE_SDIO_4BIT == 0` 时
     * `app_storage_sdio_setup()` 是空操作，挂载全靠各模块懒加载 —— 而开机那一刻
     * （app_disp_start 之后）唯一会挂卡的字库任务还在睡 1.5 秒。不挂就是
     * "文件打不开"，浮层静默起不来。 */
    if (OPRT_OK != app_storage_init()) {
        PR_WARN("[sp2d] 存储服务未就绪，仍继续尝试直接读");
    }

    /* 文件与表（头、名字表、偏移表、动作表）的解析全在 app_spf6.c ——
     * 角色页读同一个格式，解析器只留一份。见 app_spf6.h 的文件头。
     *
     * **老格式不再支持。** 换格式就得连 SD 卡上那个文件一起换 —— 留一条兼容
     * 路径只会让"到底跑的是哪版"更难查；spf6_open 的报错里会写清楚该干什么。 */
    OPERATE_RET rt = spf6_open(&sg_data.file, SP2D_PATH_SPF, SP2D_MAX_W, SP2D_MAX_H);

    if (OPRT_OK != rt) {
        PR_ERR("[sp2d] %s 加载失败（%d）—— 把 fish_elf.spf 拷到 SD 卡的 /sprite/ 目录",
               SP2D_PATH_SPF, (int)rt);
        return rt;
    }

    sg_data.cur_frame  = SP2D_NO_FRAME;
    sg_data.fail_count = 0;
    sg_data.bx0        = 0;
    sg_data.by0        = 0;
    sg_data.bx1        = (int)sg_data.file.gw - 1;
    sg_data.by1        = (int)sg_data.file.gh - 1;

    /* ---- 流式缓冲 ---- */
    sg_data.cur_px    = (uint16_t *)tal_psram_malloc(
        (size_t)sg_data.file.max_w * sg_data.file.max_h * 2u);
    sg_data.cur_spans = (sp2d_span_t *)tal_psram_malloc(
        spf6_spans_bytes(sg_data.file.max_h));

    /* RLE 暂存：PackBits 的**最坏**情况是全部字面量 = 2 字节/像素 + 每 128 像素
     * 一个控制字节（spf6_rle_worst）。按这个分配，读帧时就永远不用担心截断。 */
    sg_data.rle_cap = spf6_rle_worst((uint32_t)sg_data.file.max_w * sg_data.file.max_h);
    sg_data.rle_buf = (uint8_t *)tal_psram_malloc(sg_data.rle_cap);

    if (NULL == sg_data.cur_px || NULL == sg_data.cur_spans ||
        NULL == sg_data.rle_buf) {
        PR_ERR("[sp2d] PSRAM 不足（流式缓冲 %ux%u = %u KB + RLE 暂存 %u KB）",
               (unsigned)sg_data.file.max_w, (unsigned)sg_data.file.max_h,
               (unsigned)((uint32_t)sg_data.file.max_w * sg_data.file.max_h * 2u / 1024u),
               (unsigned)(sg_data.rle_cap / 1024u));
        app_sprite2d_deinit();
        return OPRT_MALLOC_FAILED;
    }

    /* ---- 常驻 idle：**存 RLE，不存解码帧** ----
     *
     * 拿不到就整组退化成流式读（idle_frames = 0）—— 功能一样，只是待机也要读 SD，
     * 而且**丢掉"SD 读失败时角色还在"那层兜底**（prv_frame_at 会返回 NULL）。
     * 压缩态实测 0.89 MB，比解码态 1.96 MB 稳得多，正常情况下拿得到。 */
    {
        /* 待机永远是第 0 组（打包器 `--order` 的第一个），spf6_open 保证至少有一组，
         * 所以这里不可能是 NULL。 */
        const sp2d_clip_t *ic = spf6_clip_of(&sg_data.file, SP2D_IDLE_CLIP);
        uint32_t npx     = (uint32_t)ic->w * ic->h;
        uint32_t rle_len = sg_data.file.offs[ic->frames] - sg_data.file.offs[0];
        BOOL_T   ok      = FALSE;

        sg_data.idle_frames  = ic->frames;
        sg_data.idle_decoded = 0xFFFFFFFFu;
        sg_data.idle_rle_len = rle_len;

        sg_data.idle_px    = (uint16_t *)tal_psram_malloc((size_t)npx * 2u);
        sg_data.idle_spans = (sp2d_span_t *)tal_psram_malloc(
            (size_t)ic->h * sizeof(sp2d_span_t));
        sg_data.idle_off   = (uint32_t *)tal_malloc(
            (ic->frames + 1u) * sizeof(uint32_t));
        sg_data.idle_rle   = (uint8_t *)tal_psram_malloc(rle_len);

        do {
            if (NULL == sg_data.idle_px || NULL == sg_data.idle_spans ||
                NULL == sg_data.idle_off || NULL == sg_data.idle_rle) {
                break;
            }

            /* idle 永远是第 0 组（first = 0），数据就排在数据区最前面 */
            for (uint32_t i = 0; i <= ic->frames; i++) {
                sg_data.idle_off[i] = sg_data.file.offs[i] - sg_data.file.offs[0];
            }

            if (TRUE != spf6_read_at(&sg_data.file, sg_data.file.offs[0],
                                     rle_len, sg_data.idle_rle)) {
                PR_ERR("[sp2d] idle 的 RLE 读失败（%u 字节）", (unsigned)rle_len);
                break;
            }

            /* 先解第 0 帧 —— 后面 prv_fr_set_idle 会按需换帧 */
            spf6_inflate(sg_data.idle_rle, sg_data.idle_off[1],
                        sg_data.idle_px, npx);
            spf6_build_spans(sg_data.idle_px, sg_data.idle_spans, 1, ic->w, ic->h);
            sg_data.idle_decoded = 0;
            ok = TRUE;
        } while (0);

        if (!ok) {
            PR_WARN("[sp2d] idle 常驻分配/读取失败（RLE %u KB），整组改走流式读",
                    (unsigned)(rle_len / 1024u));
            if (NULL != sg_data.idle_px)    tal_psram_free(sg_data.idle_px);
            if (NULL != sg_data.idle_spans) tal_psram_free(sg_data.idle_spans);
            if (NULL != sg_data.idle_rle)   tal_psram_free(sg_data.idle_rle);
            if (NULL != sg_data.idle_off)   tal_free(sg_data.idle_off);
            sg_data.idle_px      = NULL;
            sg_data.idle_spans   = NULL;
            sg_data.idle_rle     = NULL;
            sg_data.idle_off     = NULL;
            sg_data.idle_frames  = 0;
            sg_data.idle_rle_len = 0;
        }

        /* 句柄留在 sg_data.file 里给后面流式读用（卡插回来时 prv_ov_sd_cb 会按
         * SP2D_PATH_SPF 重开）。 */
        sg_data.loaded = TRUE;

        /* ---- 每组首帧常驻（见 head_px 的字段注释）-----------------------
         * 开机时把除 idle 之外的每组第 0 帧解好放 PSRAM；切组那拍直接 blit，
         * 不跨组 seek、不 inflate，切换处就不会多停那一拍。尽力而为：
         * 哪组拿不到内存/读失败就留 NULL，照旧流式读。 */
        {
            uint32_t nclip    = sg_data.file.clip_count;
            uint32_t cached_kb = 0;
            int      cached_n  = 0;

            sg_data.head_px    = (uint16_t **)tal_malloc(nclip * sizeof(uint16_t *));
            sg_data.head_spans = (sp2d_span_t **)tal_malloc(nclip * sizeof(sp2d_span_t *));

            /* 两个数组各自清零：只成功一个时 deinit 也要能安全遍历 */
            if (NULL != sg_data.head_px) {
                memset(sg_data.head_px, 0, nclip * sizeof(uint16_t *));
            }
            if (NULL != sg_data.head_spans) {
                memset(sg_data.head_spans, 0, nclip * sizeof(sp2d_span_t *));
            }

            if (NULL != sg_data.head_px && NULL != sg_data.head_spans) {
                for (uint32_t i = 1; i < nclip; i++) {      /* 0 = idle，已常驻 */
                    const sp2d_clip_t *hc = &sg_data.file.clips[i];
                    uint32_t a = sg_data.file.offs[hc->first];
                    uint32_t b = sg_data.file.offs[hc->first + 1u];
                    uint16_t    *px;
                    sp2d_span_t *sp;

                    if (b <= a || (b - a) > sg_data.rle_cap) {
                        continue;
                    }

                    px = (uint16_t *)tal_psram_malloc((size_t)hc->w * hc->h * 2u);
                    sp = (sp2d_span_t *)tal_psram_malloc(
                        (size_t)hc->h * sizeof(sp2d_span_t));

                    if (NULL == px || NULL == sp ||
                        TRUE != prv_read_at(a, b - a, sg_data.rle_buf)) {
                        if (NULL != px) tal_psram_free(px);
                        if (NULL != sp) tal_psram_free(sp);
                        continue;
                    }

                    spf6_inflate(sg_data.rle_buf, b - a, px,
                                 (uint32_t)hc->w * hc->h);
                    spf6_build_spans(px, sp, 1, hc->w, hc->h);

                    sg_data.head_px[i]    = px;
                    sg_data.head_spans[i] = sp;
                    cached_n++;
                    cached_kb += (uint32_t)((size_t)hc->w * hc->h * 2u / 1024u);
                }
            } else {
                PR_WARN("[sp2d] 首帧常驻表分配失败，切组仍走流式读");
            }

            PR_NOTICE("[sp2d] 首帧常驻 %d/%u 组（%u KB）：切组那一拍不读卡",
                      cached_n, (unsigned)(nclip - 1u), (unsigned)cached_kb);
        }

        /* "角色高"只算 idle 的第 0 帧（常驻里已经解好了那一帧）。待机是匀速摇摆，
         * 各帧高度一致，第 0 帧够代表；真要逐帧算就得把整组解一遍，不值。 */
        int ch_h = 0;
        if (0 != sg_data.idle_frames) {
            int cx0, cy0, cx1, cy1;
            prv_char_bbox(sg_data.idle_px, ic->w, ic->h, &cx0, &cy0, &cx1, &cy1);
            ch_h = (cy1 >= cy0) ? (cy1 - cy0 + 1) : 0;
        }

        /* **别把"框"和"角色高"并排打** —— 框 = 角色 + pad + 起跳余量，各动作差很多
         * （吃晚餐三盘菜 271 宽、点开心起跳那帧 194 高），并排看会误判待机角色小。
         * 各动作的角色高实测只差 1~2px：idle 157 / 点击 156 / 说话 158。 */
        PR_NOTICE("[sp2d] 已加载 SPF7：%u 组动作 / %u 帧，锚点框 %ux%u；"
                  "idle 常驻 %u 帧（框 %ux%u，角色高 %dpx，RLE %u KB）；"
                  "流式单帧缓冲上限 %ux%u",
                  (unsigned)sg_data.file.clip_count,
                  (unsigned)sg_data.file.total_frames,
                  (unsigned)sg_data.file.gw, (unsigned)sg_data.file.gh,
                  (unsigned)sg_data.idle_frames, ic->w, ic->h, ch_h,
                  (unsigned)(rle_len / 1024u),
                  (unsigned)sg_data.file.max_w, (unsigned)sg_data.file.max_h);
    }
    return OPRT_OK;
}

void app_sprite2d_deinit(void)
{
    /* 首帧常驻必须**先于** spf6_close 释放：要按 clip_count 逐项 free，
     * 而 spf6_close 会把那张动作表一起收掉。 */
    if (NULL != sg_data.head_px || NULL != sg_data.head_spans) {
        for (uint32_t i = 0; i < sg_data.file.clip_count; i++) {
            if (NULL != sg_data.head_px && NULL != sg_data.head_px[i]) {
                tal_psram_free(sg_data.head_px[i]);
            }
            if (NULL != sg_data.head_spans && NULL != sg_data.head_spans[i]) {
                tal_psram_free(sg_data.head_spans[i]);
            }
        }
        if (NULL != sg_data.head_px)    tal_free(sg_data.head_px);
        if (NULL != sg_data.head_spans) tal_free(sg_data.head_spans);
        sg_data.head_px    = NULL;
        sg_data.head_spans = NULL;
    }

    spf6_close(&sg_data.file);              /* 句柄 + 动作表 + 偏移表一起收干净 */
    if (NULL != sg_data.idle_px)    tal_psram_free(sg_data.idle_px);
    if (NULL != sg_data.idle_spans) tal_psram_free(sg_data.idle_spans);
    if (NULL != sg_data.idle_rle)   tal_psram_free(sg_data.idle_rle);
    if (NULL != sg_data.idle_off)   tal_free(sg_data.idle_off);
    if (NULL != sg_data.rle_buf)    tal_psram_free(sg_data.rle_buf);
    if (NULL != sg_data.cur_px)     tal_psram_free(sg_data.cur_px);
    if (NULL != sg_data.cur_spans)  tal_psram_free(sg_data.cur_spans);
    memset(&sg_data, 0, sizeof(sg_data));
}

/** @brief 精灵**锚点框**的尺寸（不是某一组的帧尺寸）。
 *
 * SPF4 之后各组帧尺寸不同，所以对外只报锚点框 —— 调用方（脏区标记、触控区）
 * 要的是"这个精灵可能占多大"，锚点框是所有组的最小外接，一定够。 */
void app_sprite2d_size(int *w, int *h)
{
    if (NULL != w) *w = sg_data.bx1 - sg_data.bx0 + 1;
    if (NULL != h) *h = sg_data.by1 - sg_data.by0 + 1;
}

/* ======================= 渲染 ======================= */

/**
 * @brief 把一帧混合到目标。@a ox/@a oy 是**锚点框左上角**，本组的 dx/dy 在这里加。
 *
 * 裁剪到目标范围（精灵可能部分出屏），返回实际混合的像素数。
 * render_to（按时间挑帧）和 render_clip（按组号点名）共用这一段。
 */
static uint32_t prv_blit_anchor(uint16_t *dst, int stride, int dw, int dh,
                                int ox, int oy, const sp2d_fr_t *fr)
{
    int x0 = ox + fr->dx;
    int y0 = oy + fr->dy;
    int x1 = x0 + fr->w;
    int y1 = y0 + fr->h;
    int sx = 0, sy = 0;

    if (x0 < 0) { sx = -x0; x0 = 0; }
    if (y0 < 0) { sy = -y0; y0 = 0; }
    if (x1 > dw) x1 = dw;
    if (y1 > dh) y1 = dh;

    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    return prv_blit_frame(dst + (size_t)y0 * stride + x0, stride,
                          fr->px + (size_t)sy * fr->w + sx, fr->w,
                          fr->spans + sy, sx,
                          x1 - x0, y1 - y0);
}

void app_sprite2d_render_to(uint16_t *dst, int stride, int dw, int dh,
                            int ox, int oy, uint32_t t_ms)
{
    if (!sg_data.loaded || NULL == dst) {
        return;
    }

    uint32_t c0 = (uint32_t)tal_system_get_millisecond();

    uint32_t ci = 0, fi = 0;

    prv_pick(t_ms, &ci, &fi);

    sg_stat.fallback = 0;                   /* prv_frame_at 读失败时会置 1 */

    /* idle 组常驻内存；其它组从 SD 流式读进来，读失败切回 idle */
    const sp2d_fr_t *fr = prv_frame_at(fi);

    if (NULL == fr) {
        return;                             /* 一帧都拿不到（SD 不在）：这一帧不画 */
    }

    /* **本组的 dx/dy 在 prv_blit_anchor 里生效**：ox/oy 是锚点框左上角，这一组的
     * 裁剪框可能比锚点框小、也可能（因为 pad 取整）略微伸出去。 */
    /* 诊断（2026-10-14）：只在**换组**时打。起跳那一下如果角色整个跳了一下，
     * 这一行能直接看出是"换组时 dx/dy 变了"还是"锚点位置变了" ——
     * 实画 = 锚点 + 本组 dxdy，两个数都在。 */
    if (ci != sg_dbg_clip) {
        PR_NOTICE("[sp2d] 渲染 组 %u->%u 帧=%u 锚点=(%d,%d) 本组dxdy=(%d,%d) 实画=(%d,%d)",
                  (unsigned)sg_dbg_clip, (unsigned)ci, (unsigned)fi,
                  ox, oy, fr->dx, fr->dy, ox + fr->dx, oy + fr->dy);
        sg_dbg_clip = ci;
    }

    uint32_t done = prv_blit_anchor(dst, stride, dw, dh, ox, oy, fr);

    sg_stat.us_total  = ((uint32_t)tal_system_get_millisecond() - c0) * 1000u;
    sg_stat.px        = done;
    sg_stat.clip_idx  = (uint16_t)ci;
    sg_stat.clip_cnt  = sg_data.file.clip_count;
    sg_stat.frame_idx = (uint16_t)(fi - sg_data.file.clips[ci].first);
    sg_stat.frame_cnt = sg_data.file.clips[ci].frames;
}

void app_sprite2d_render(uint16_t *dst, int w, int h,
                         uint32_t t_ms, uint16_t bg)
{
    if (NULL == dst) {
        return;
    }

    for (int i = 0; i < w * h; i++) {
        dst[i] = bg;
    }

    app_sprite2d_render_to(dst, w, w, h, 0, 0, t_ms);
}

int app_sprite2d_clip_count(void)
{
    return sg_data.loaded ? (int)sg_data.file.clip_count : 0;
}

int app_sprite2d_clip_frames(int clip)
{
    if (!sg_data.loaded || clip < 0 || clip >= (int)sg_data.file.clip_count) {
        return 0;
    }
    return (int)sg_data.file.clips[clip].frames;
}

const char *app_sprite2d_clip_name(int clip)
{
    if (!sg_data.loaded || clip < 0 || clip >= (int)sg_data.file.clip_count) {
        return "?";
    }
    return sg_data.file.clips[clip].name;
}

void app_sprite2d_clip_box(int clip, int *dx, int *dy, int *w, int *h)
{
    int ldx = 0, ldy = 0, lw = 0, lh = 0;

    if (sg_data.loaded && clip >= 0 && clip < (int)sg_data.file.clip_count) {
        const sp2d_clip_t *c = &sg_data.file.clips[clip];

        ldx = c->dx;
        ldy = c->dy;
        lw  = c->w;
        lh  = c->h;
    }

    if (NULL != dx) *dx = ldx;
    if (NULL != dy) *dy = ldy;
    if (NULL != w)  *w  = lw;
    if (NULL != h)  *h  = lh;
}

void app_sprite2d_blit_clip(uint16_t *dst, int w, int h, int clip, int frame)
{
    if (NULL == dst || w <= 0 || h <= 0) {
        return;
    }
    if (!sg_data.loaded || clip < 0 || clip >= (int)sg_data.file.clip_count) {
        return;
    }

    const sp2d_clip_t *c = &sg_data.file.clips[clip];

    if (0 == c->frames) {
        return;
    }

    /* 帧号按本组帧数取模，调用方不用自己绕回来 */
    uint32_t fi = c->first + (uint32_t)(frame % (int)c->frames);
    const sp2d_fr_t *fr = prv_frame_at(fi);

    if (NULL == fr) {
        return;                             /* SD 读失败：这一帧只剩底色 */
    }

    sg_stat.fallback  = 0;
    sg_stat.px        = prv_blit_anchor(dst, w, w, h, 0, 0, fr);
    sg_stat.clip_idx  = (uint16_t)clip;
    sg_stat.clip_cnt  = sg_data.file.clip_count;
    sg_stat.frame_idx = (uint16_t)(frame % (int)c->frames);
    sg_stat.frame_cnt = c->frames;
}

void app_sprite2d_render_clip(uint16_t *dst, int w, int h,
                              int clip, int frame, uint16_t bg)
{
    if (NULL == dst || w <= 0 || h <= 0) {
        return;
    }

#if SP2D_TRACE_SWITCH
    {
        static int      s_clip   = -2;      /* -2 = 还没跑过第一拍 */
        static uint32_t s_t      = 0;
        static int      s_follow = 0;       /* 切组后还要盯几拍 */

        uint32_t now = (uint32_t)tal_system_get_millisecond();
        uint32_t gap = now - s_t;

        if (-2 == s_clip) {
            s_clip = clip;                  /* 首拍不算"切组" */
        } else if (clip != s_clip) {
            PR_NOTICE("[sp2d] 切组 %d -> %d（%s，%d 帧）", s_clip, clip,
                      app_sprite2d_clip_name(clip), app_sprite2d_clip_frames(clip));
            s_clip   = clip;
            s_follow = SP2D_TRACE_FOLLOW_N;
        } else if (gap > SP2D_TRACE_SLOW_TICK) {
            /* **动作播放全程的慢拍检测**：不限切换后，任何一拍间隔超标就记 */
            PR_NOTICE("[sp2d] 慢拍：%s 帧 %d，距上一拍 %u ms",
                      app_sprite2d_clip_name(clip), frame, (unsigned)gap);
        }
        if (s_follow > 0) {
            /* **这一条才是卡顿的读数**：切组那拍花了多久，下一拍就多隔了多久 */
            PR_NOTICE("[sp2d]   切换后 %d 拍：帧 %d，距上一拍 %u ms",
                      SP2D_TRACE_FOLLOW_N + 1 - s_follow, frame, (unsigned)gap);
            s_follow--;
        }
        s_t = now;
    }
#endif

    for (int i = 0; i < w * h; i++) {
        dst[i] = bg;
    }

    app_sprite2d_blit_clip(dst, w, h, clip, frame);
}

void app_sprite2d_stat(SP2D_STAT_T *out)
{
    if (NULL != out) {
        *out = sg_stat;
    }
}
