/**
 * @file app_storage.c
 * @brief 存储服务实现（SD 挂载 + 双码页目录枚举）。
 *
 * 双码页探测的背景见 app_storage.h。这里的实现刻意保持"先原样、再 OEM 重编码"
 * 的顺序：ASCII 名在两种码页下字节相同，永远走第一条路径；只有 CJK 名才会走到
 * 第二条。两条都不行才算不可打开。
 */

#include <string.h>
#include <strings.h>

#include "tal_api.h"
#include "tkl_fs.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#include "driver/sd_card.h"
/* sdio_host_clock_freq_t / SDIO_HOST_CLK_xxM：sd_card.h 自己不带，要单独引
 * （sdio_host_types.h → hal/hal_sdio_host_types.h）。降时钟扫描用。 */
#include "driver/sdio_host_types.h"

#include "app_event.h"
#include "app_storage.h"

/* FatFs 代码页转换表（ffunicode.c / cc936.c 里的公共符号）。app_storage 是唯一
 * 直接依赖它的地方 —— 前提是 SDK 的 ffconf.h 里 FF_CODE_PAGE == 936。 */
extern unsigned short ff_uni2oem(unsigned int uni, unsigned short cp);
extern unsigned short ff_oem2uni(unsigned int oem, unsigned short cp);

#define STORAGE_OEM_CP      936

/* 链路重建的"整条重来"次数，以及每轮里"真读一次"验证的重试次数 / 间隔。
 * 见 app_storage_recover() 里的说明。 */
#define STORAGE_RECOVER_ATTEMPT   2
#define STORAGE_VERIFY_RETRY      3
#define STORAGE_VERIFY_RETRY_MS   20

/* app_storage_recover_reopen() 里"重建后重开句柄"的重试次数与间隔。 */
#define STORAGE_REOPEN_RETRY      4
#define STORAGE_REOPEN_RETRY_MS   30

/* 判死（APP_SD_DAMAGED）之后，隔多久才允许再试一次完整重建。
 *
 * 没有它会很难看：读者是"每攒够 N 帧坏读就重建一次"，而浮层这类常驻读者每秒
 * 都在读 —— 卡真坏了的时候它会每几百毫秒 unmount/remount 一次，每次卡住一百
 * 多毫秒。既然刚刚试过、结论一样，就该歇一会儿。 */
#define STORAGE_DAMAGED_RETRY_MS  5000

static BOOL_T sg_mounted = FALSE;

/* 当前故障。**"卡不在"和"读错"两条路唯一的汇总点**（见 app_storage.h）：
 * CD 轮询只写 ABSENT/OK，链路重建只写 DAMAGED/OK。读者、路由门控、故障页
 * 文案全读它。 */
static APP_SD_FAULT_E sg_fault      = APP_SD_OK;
static uint32_t       sg_damaged_ms = 0;    /* 最近一次判死的时刻（退避用） */

/* 目录索引代次（见 app_storage.h）。**只在 CD 报出拔插时 +1** —— 链路重建刻意
 * 不碰它（卡没换，内容就没变）。"还没扫过"在各模块那边用 0 表示，所以这里从 1 起。 */
static uint32_t       sg_gen        = 1;

/* 两次链路重建之间至少隔这么久。
 *
 * 重建要 unmount + 重新起卡（150ms 起），期间所有句柄作废。读坏如果是"偶发"的，
 * 重建根本救不回来 —— 但每个读者都会各自触发一次，于是 A 的重建让 B 读坏、B 的
 * 重建又让 A 读坏，滚成一整屏的"重建 SD 链路"，最后卡都起不来（实测还会触发
 * FreeRTOS 断言重启）。限制频率是最直接的一道闸：真要坏了，慢一点重建也不会
 * 更坏；只是偶发读错，等下一次重建的窗口里读往往就自己好了。 */
#define STORAGE_RECOVER_MIN_MS   3000u
static uint32_t sg_recover_ms = 0;

/* 链路代次（见 app_storage.h）。**每次重挂都 +1**：切读写档、链路重建都算。
 * 常开的读者（精灵 spf、字库 bin）读之前比一下，变了就先重开自己的文件。 */
static volatile uint32_t sg_media_gen = 1;
/* 预热标志（见 app_storage.h）。跟 sg_fault 是两回事：链路重建成功 ≠ 预热完了。 */
static BOOL_T         sg_warming    = FALSE;

/* "正在长时间占用 SD"的临界区（见 app_storage.h）。目录扫描和链路重建靠它互斥。 */
static MUTEX_HANDLE   sg_busy_lock  = NULL;

void app_storage_busy_enter(void)
{
    if (NULL != sg_busy_lock) {
        (void)tal_mutex_lock(sg_busy_lock);
    }
}

void app_storage_busy_leave(void)
{
    if (NULL != sg_busy_lock) {
        (void)tal_mutex_unlock(sg_busy_lock);
    }
}

/* 卡在位检测（CD 引脚）的状态。**声明必须放在 app_storage_ready() 之前** ——
 * 那个函数要看 sg_cd_present，而它的定义在文件靠前的位置。 */
static BOOL_T   sg_cd_inited  = FALSE;
static BOOL_T   sg_cd_present = TRUE;   /* 开机先当"在"，第一次读立刻纠正 */
static uint8_t  sg_cd_same    = 0;      /* 连续读到同一电平的次数 */
static TIMER_ID sg_cd_timer   = 0;

/* 卡"挂载了"和"能读"是两件事：tkl_fs_mount 对已挂载的卷也返回非 0，所以判据
 * 只能是真读一次。用根目录 opendir —— 它要走一遍 FAT 链和首个目录簇，元数据
 * 通路坏了这里就失败，比看 mount 返回值可靠得多。
 *
 * 两处都要用它：4 线协商（判断"这个线宽能不能起卡"）和 app_storage_recover()
 * 重建之后的验证 —— 所以放在 #if 之外。 */
static BOOL_T prv_readable(void)
{
    TUYA_DIR d = NULL;

    if (0 != tkl_dir_open(APP_STORAGE_MOUNT, &d)) {
        return FALSE;
    }
    tkl_dir_close(d);
    return TRUE;
}

#if APP_STORAGE_SDIO_4BIT
static BOOL_T  sg_sdio_done = FALSE;
static int32_t sg_sdio_lines = 0;       /* 4 / 1；0 = 没起来 */

/* **"能写"的那一档**（固定常量，不再开机扫）。
 *
 *   sg_write_lines  能写的线宽；**0 = 卡没起来**（照片一定存不上）
 *   sg_write_clk    能写的时钟档
 *   sg_fast_lines   读最快的线宽（平时停在这一档）
 *
 * 为什么要把"能写"和"读最快"分开记（2026-10-20 实测，方案 B 的由来）：
 * 这块板 4 线在 13M/6.5M/3.2M/1.6M **每一档都写不进**（`wr_status = 0x7`），
 * 只有 1 线 @13M 一次就通过；可 1 线的读带宽只有 4 线的 1/6~1/10（190~400 vs
 * 1000~2700 KB/s），常驻 1 线会让精灵预取从 22~60ms/批 掉到 150~320ms/批，
 * 桌宠肉眼可见地掉帧。**照片能存和动画流畅本来是二选一。**
 *
 * 解法是分时使用：平时停在 sg_fast_lines（4 线）跑动画，只在按快门那一刻由
 * app_storage_write_begin() 把卡整条重起成 sg_write_lines 那一档，写完
 * app_storage_write_end() 再重起回 4 线。一次 1~1.5 秒，而抓图本身就要等 2 秒，
 * 用户感知不到。 */
static int32_t                sg_write_lines = 0;
static sdio_host_clock_freq_t sg_write_clk   = SDIO_HOST_CLK_13M;
static int32_t                sg_fast_lines  = 4;

/* tkl_sdio_init() 在 SDK 里只有 .c 定义、没有对外头文件，只能自己 extern。
 * 它是唯一能改运行期线宽的入口（理由见 app_storage.h 里的说明）。 */
extern OPERATE_RET tkl_sdio_init(int port, const TUYA_SDIO_BASE_CFG_T *cfg);

/* 读通路是 GDMA 还是 CPU 轮询，由编译期 CONFIG_SDIO_GDMA_EN 决定（见
 * sd_card_driver.c）。这里只读出来打一条日志 —— 换卡/换板排查时第一眼要知道
 * 数据是走哪条路进来的。 */
extern int g_sd_card_dma_rx_compiled;

/* 按给定线宽起一次 SD 链路。
 *
 * 顺序不能变：tkl_sdio_init() 只改运行期变量，真正生效在 bk_sd_card_init()
 * （ACMD6 + sdio host 配置），而它是在 mount 内部的 disk_initialize 里触发的。
 * 已经起来的卡不会自己换线宽 ⇒ 换线宽之前必须把卡整个拆掉。
 *
 * 开头的 unmount / deinit 是无条件的：没挂载、没初始化过时它们各返回非 0、无害，
 * 换来的是"上一轮 4 线起了个半死状态"也能被清干净。 */
/* 按给定线宽起一次卡（单次尝试）。
 *
 * 顺序不能变：tkl_sdio_init() 只改运行期变量，真正生效在 bk_sd_card_init()
 * （ACMD6 + sdio host 配置），而它是在 mount 内部的 disk_initialize 里触发的。
 * 已经起来的卡不会自己换线宽 ⇒ 换线宽之前必须把卡整个拆掉。
 *
 * 开头的 unmount / deinit 是无条件的：没挂载、没初始化过时它们各返回非 0、无害，
 * 换来的是"上一轮 4 线起了个半死状态"也能被清干净。 */
static BOOL_T prv_try_lines_once(int lines)
{
    TUYA_SDIO_BASE_CFG_T cfg = {
        .bus_width  = (4 == lines) ? TUYA_SDIO_BUS_WIDTH_4BIT
                                   : TUYA_SDIO_BUS_WIDTH_1BIT,
        .speed_mode = TUYA_SDIO_SPEED_DEFAULT,
        .voltage    = TUYA_SDIO_VOLTAGE_3V3,
        /* clock_hz / speed_mode 都是空参数：tkl_sdio_init() 只读 bus_width，
         * 全 SDK 没有任何代码读 clock_hz。真正生效的时钟是建卡时
         * CONFIG_SDCARD_DEFAULT_CLOCK_FREQ（BK7258 上是 0 = 13M），运行期要改
         * 只能走 bk_sd_card_set_clock() —— 见 app_storage_write_begin()。 */
        .clock_hz   = 0,
        .flags      = 0,
    };
    bk_err_t sdret;
    int32_t  fsret;

    if (OPRT_OK != tkl_sdio_init(0, &cfg)) {
        PR_ERR("[sto] tkl_sdio_init(%d 线) 失败", lines);
        return FALSE;
    }

    (void)tkl_fs_unmount(APP_STORAGE_MOUNT);
    sdret = bk_sd_card_deinit();
    if (0 != sdret) {
        PR_DEBUG("[sto] sd deinit 返回 %d（继续）", (int)sdret);
    }
    sg_mounted = FALSE;

    fsret = tkl_fs_mount(APP_STORAGE_MOUNT, DEV_SDCARD);
    if (0 != fsret) {
        PR_DEBUG("[sto] %d 线 mount 返回 %d（可能已挂载）", lines, (int)fsret);
    }

    if (FALSE == prv_readable()) {
        PR_WARN("[sto] %d 线起卡失败（读不到 %s）", lines, APP_STORAGE_MOUNT);
        return FALSE;
    }

    sg_mounted = TRUE;
    sg_media_gen++;                 /* 重挂过了：所有已打开的句柄作废 */
    return TRUE;
}

/* 起卡**必须重试**（2026-10-21 真机：拍照切 1 线写时一次就失败，卷被拆掉，
 * 紧接着字库的 fopen 全部失败）。
 *
 * 连续 deinit -> init 本身就是会偶发失败的：`sd card unsupport voltage` /
 * CMD55 超时，SDK 自己的 disk_initialize 也是靠重试才起来的。每次重试前再
 * deinit 一次 + 退让，把上一次"起了个半死状态"清干净。
 *
 * **AI 回复时命中率最高**（现场：AI 说话 + 按快门必现）：AEC 常驻吃满一个核、
 * TTS 音频 DMA 同时在跑，建卡那几条命令（CMD0/8/55/41/2/3/7）的响应容易超时。
 * 所以退让做成递增的，给卡和总线都留出恢复时间。 */
#define STORAGE_LINE_TRY_TIMES  5
static BOOL_T prv_try_lines(int lines)
{
    static const uint32_t backoff_ms[STORAGE_LINE_TRY_TIMES] = { 0, 50, 100, 150, 200 };

    for (int i = 0; i < STORAGE_LINE_TRY_TIMES; i++) {
        if (i > 0) {
            (void)bk_sd_card_deinit();
            tal_system_sleep(backoff_ms[i]);
        }
        if (TRUE == prv_try_lines_once(lines)) {
            if (i > 0) {
                PR_NOTICE("[sto] %d 线第 %d 次起卡成功（退了 %ums）",
                          lines, i + 1, (unsigned)backoff_ms[i]);
            }
            return TRUE;
        }
    }
    PR_WARN("[sto] %d 线连续 %d 次起卡失败", lines, STORAGE_LINE_TRY_TIMES);
    return FALSE;
}

/* 写档是**固定**的：1 线 @13M，不再开机扫。
 *
 * 2026-10-20 把这几种组合都试过一遍（4/1 线 × 13M/6.5M/3.2M/1.6M）：4 线每一档
 * 都写不进（卡返回的 `wr_status = 0x7`），只有 1 线 @13M 通过。结论定下来之后，
 * "开机逐个写探针"就只剩坏处了 —— 它每次开机要往卡上写/读/删 4KB × 最多 8 轮，
 * 还要反复 unmount + 重挂，卡在这种反复起卡/写删之后容易进半死状态（读失败、
 * 自己掉线、CRC 错），是我们当时"隔一阵 SD 链路就出事"的最大噪声源。
 * 所以 2026-10-21 整段删掉，直接采用实测档。
 *
 * 代价很小：换一张 4 线也能写的卡时，会在按快门那一下白切一次档（1~1.5 秒），
 * 而抓图本身要等 2 秒，用户感知不到。要重开扫描：见提交 83bc5c7 里的
 * prv_write_probe() / prv_scan_clock_write()。 */
#define APP_STORAGE_WRITE_LINES  1
#define APP_STORAGE_WRITE_CLK    SDIO_HOST_CLK_13M

/* 读档线宽：**固定 1 线**。
 *
 * 这块板的 4 线读一直不稳：2026-10-28 真机 4 线试 5 次、1 线再试 5 次，38 秒
 * 全败，屏幕一直黑；退到 1 线后第一次挂载就成功。4 线从来没有在这块板上稳定
 * 兑现过带宽，所以开机不再探测它。
 *
 * 代价：1 线读带宽只有 4 线的 1/6~1/10，桌宠动画可能掉帧。用确定性换流畅度。 */
#define APP_STORAGE_READ_LINES   1

/* 起卡：**只挂一次**。
 *
 * 以前是 4 线 x5 + 1 线 x5，每次 mount 内部还要重读 3 遍，真机最坏 38 秒 ——
 * 这段时间排在显示初始化之前，就是"开机黑屏"。卡挂不上时反复 deinit/mount
 * 也没有帮助：日志里 10 次全败，反而是后面的恢复路径一次就挂上了。
 * 所以开机只试一次，失败就交给 prv_recover_locked()（由 preload 触发）。
 * 不试写 —— 见上面固定写档的理由。 */
static BOOL_T prv_bring_up(void)
{
    if (prv_try_lines_once(APP_STORAGE_READ_LINES)) {
        sg_sdio_lines  = APP_STORAGE_READ_LINES;
        sg_fast_lines  = APP_STORAGE_READ_LINES;
        sg_write_lines = APP_STORAGE_WRITE_LINES;
        sg_write_clk   = APP_STORAGE_WRITE_CLK;
        PR_NOTICE("[sto] 卡就绪：平时 %d 线读，拍照时切到 %d 线写；读通路 %s",
                  sg_fast_lines, sg_write_lines,
                  g_sd_card_dma_rx_compiled ? "GDMA（硬件读，单次 ≤64KB）"
                                            : "CPU 轮询 FIFO");
        return TRUE;
    }

    /* tkl_sdio_init(1 线) 已经生效，恢复路径会沿用这个线宽重挂。
     * 写档等真正挂上之后由 prv_recover_locked() 补回来。 */
    sg_sdio_lines  = 0;
    sg_fast_lines  = APP_STORAGE_READ_LINES;
    sg_write_lines = 0;
    PR_ERR("[sto] 1 线起卡失败（开机只试一次），等恢复路径重挂");
    return FALSE;
}

OPERATE_RET app_storage_sdio_setup(void)
{
    if (sg_sdio_done) {
        return OPRT_OK;
    }
    sg_sdio_done = TRUE;

    if (!prv_bring_up()) {
        return OPRT_COM_ERROR;
    }
    (void)bk_sd_card_set_clock(SDIO_HOST_CLK_13M);
    return OPRT_OK;
}

int32_t app_storage_sdio_lines(void)
{
    return sg_sdio_lines;
}

OPERATE_RET app_storage_write_begin(void)
{
    if (FALSE == sg_sdio_done) {
        /* 有模块跑在启动序列的协商之前就要写：先补一次协商，
         * 否则 sg_write_lines 还是 0、写会被无理由地拒掉。 */
        (void)app_storage_sdio_setup();
    }
    if (0 == sg_write_lines) {
        PR_ERR("[sto] 没有能写的档位（开机扫描全败），写操作放弃");
        return OPRT_COM_ERROR;
    }

    /* 整段关进 SD 临界区：切档要把卷 unmount 掉，绝不能和别人的读叠在一起。
     * 精灵预取会在下一次 spf6_read_at() 上排队等锁 —— 那正是我们要的：切档期间
     * 谁都不许碰卡。（锁不可重入，所以调用方**不能**自己再包一层。） */
    app_storage_busy_enter();

    if (sg_sdio_lines != sg_write_lines) {
        const int32_t prev = sg_sdio_lines;

        if (!prv_try_lines(sg_write_lines)) {
            /* **切档失败必须切回去。** 切档的过程是 unmount -> deinit -> init，
             * 失败时卷是"拆了没挂上"的状态：后面所有读（字库按需读字形、精灵）
             * 都会对着一个死卷 fopen 失败，直到下一次写档或链路重建才恢复。
             * 这里把卡重新起回原来那一档，读能力立刻回来。 */
            PR_ERR("[sto] 切到写档（%d 线）失败，切回 %d 线", sg_write_lines, (int)prev);
            if (0 != prev && prv_try_lines(prev)) {
                sg_sdio_lines = prev;
                PR_NOTICE("[sto] 已切回 %d 线（读恢复）", (int)prev);
            } else {
                sg_sdio_lines = 0;
                PR_ERR("[sto] 切回 %d 线也失败，卡不可用", (int)prev);
            }
            app_storage_busy_leave();
            return OPRT_COM_ERROR;
        }
        sg_sdio_lines = sg_write_lines;
    }

    /* 时钟也要跟着换：写档可能是靠降时钟才通的，而重起卡之后 clock_freq 会回到
     * 默认档。对"13M 就能写"的板子这一句是原地赋同一个值，没有副作用。 */
    if (BK_OK != bk_sd_card_set_clock(sg_write_clk)) {
        PR_WARN("[sto] 写档时钟设置失败（按当前时钟继续）");
    }
    tal_system_sleep(20);               /* 等新分频稳定 */

    app_storage_busy_leave();
    return OPRT_OK;
}

void app_storage_write_end(void)
{
    if (0 == sg_write_lines) {
        return;                         /* 没有写档，也就没有"切回"这回事 */
    }

    app_storage_busy_enter();

    if (sg_sdio_lines != sg_fast_lines) {
        if (prv_try_lines(sg_fast_lines)) {
            sg_sdio_lines = sg_fast_lines;
        } else {
            PR_WARN("[sto] 切回 %d 线失败，停在 %d 线（读会变慢）",
                    sg_fast_lines, sg_sdio_lines);
        }
    }
    /* 切回读档一律用最高档：写档可能为换余量降过时钟，读不需要那份余量。 */
    (void)bk_sd_card_set_clock(SDIO_HOST_CLK_13M);
    tal_system_sleep(20);

    app_storage_busy_leave();
}

int32_t app_storage_write_lines(void)
{
    return sg_write_lines;
}
#else   /* APP_STORAGE_SDIO_4BIT == 0：协商与降级整段不参与编译 */

/* 接口退化成"空操作 / 恒 1"，调用点因此不需要条件编译；挂载时机也保持
 * 原样 —— 继续由 app_storage_init() 懒加载，不在开机序列里提前碰卡。 */
OPERATE_RET app_storage_sdio_setup(void)
{
    return OPRT_OK;
}

int32_t app_storage_sdio_lines(void)
{
    return 1;                           /* SDK 默认就是 1 线 */
}

/* 没有协商也就没有"写档/读档"之分：卡从起卡到卸载就一种线宽，
 * begin/end 退化成空操作，write_lines 恒为 1（能不能写由落盘校验说了算）。 */
OPERATE_RET app_storage_write_begin(void)
{
    return OPRT_OK;
}

void app_storage_write_end(void)
{
}

int32_t app_storage_write_lines(void)
{
    return 1;
}
#endif  /* APP_STORAGE_SDIO_4BIT */

OPERATE_RET app_storage_init(void)
{
#if !APP_STORAGE_SDIO_4BIT
    int32_t ret;
#endif

    if (sg_mounted) {
        /* **"每次重进都加载 SD 卡"就落在这一句上。**
         *
         * 挂载标志在拔卡之后是不可信的：FatFs 不知道卡没了，`sg_mounted` 还是真，
         * 光看它会让所有页面拿着一个死卷继续读。所以这里每次被调用都真验一次 ——
         * 卡在位、且根目录真能打开，才算"已挂载"直接返回；否则清掉标志重挂。
         *
         * 代价是一次 opendir（毫秒级），换的是"任何页面 on_enter 都重新确认一遍
         * 卡到底能不能用"。 */
        if (app_storage_card_present() && (TRUE == prv_readable())) {
            return OPRT_OK;
        }
        sg_mounted = FALSE;

        if (!app_storage_card_present()) {
            /* 卡不在位：**不重挂** —— tkl_fs_mount 会走 disk_initialize ->
             * bk_sd_card_init，对着一张拔掉的卡折腾几百毫秒然后失败。
             *
             * 只拦"重挂"，第一次挂载（sg_mounted 本来是假）照旧往下走：CD 引脚
             * 万一判错了，也不至于把整个存储服务锁死。 */
            PR_DEBUG("[sto] 卡不在位，不重挂 %s", APP_STORAGE_MOUNT);
            return OPRT_COM_ERROR;
        }
        PR_NOTICE("[sto] 卷在位但读不动，重新挂载 %s", APP_STORAGE_MOUNT);
    }

#if APP_STORAGE_SDIO_4BIT
    if (FALSE == sg_sdio_done) {
        /* 有模块跑在启动序列的协商之前就直接调了 init —— 这里补一次，
         * 内部含 4 线 -> 1 线降级。 */
        (void)app_storage_sdio_setup();
    } else if (FALSE == sg_mounted) {
        /* 上面把 sg_mounted 清掉了（卷读不动）：按当前线宽把卡重起一次。
         * 少了这一句，一次瞬时读错就会让这个分支**永远**返回失败 —— 线宽协商
         * 已经做过（sg_sdio_done 为真），再也不会有人去起卡。 */
        (void)prv_try_lines((0 != sg_sdio_lines) ? sg_sdio_lines : 4);
    }
    return (TRUE == sg_mounted) ? OPRT_OK : OPRT_COM_ERROR;
#else
    /* 挂载是重活：tkl_fs_mount -> disk_initialize -> bk_sd_card_init()，会把 SDIO
     * 引脚和卡整个重新识别一遍，顺带重新分配 SDIO 的 DMA 通道，并作废所有已经
     * 打开的 SD 文件句柄。所以每次**真**挂载都留一条 NOTICE —— 一次上电应该只有
     * 一条；出现第二条就说明还有人绕过存储服务自己挂。 */
    PR_NOTICE("[sto] 挂载 %s", APP_STORAGE_MOUNT);

    /* 已经挂载过会返回非 0，这不算错误 —— 后面的目录打开才是真正的检验 */
    ret = tkl_fs_mount(APP_STORAGE_MOUNT, DEV_SDCARD);
    if (0 != ret) {
        PR_DEBUG("[sto] mount %s 返回 %d（可能已挂载）", APP_STORAGE_MOUNT, ret);
    }

    sg_mounted = TRUE;
    return OPRT_OK;
#endif
}

APP_SD_FAULT_E app_storage_fault(void)
{
    return sg_fault;
}

uint32_t app_storage_gen(void)
{
    return sg_gen;
}

uint32_t app_storage_media_gen(void)
{
    return sg_media_gen;
}

BOOL_T app_storage_warming(void)
{
    return sg_warming;
}

void app_storage_warm_done(void)
{
    if (sg_warming) {
        sg_warming = FALSE;
        PR_NOTICE("[sto] 预热完成，SD 页面放行");
    }
}

BOOL_T app_storage_wait_ready(uint32_t timeout_ms)
{
    uint32_t t0      = tal_system_get_millisecond();
    uint32_t last_log = t0;
    BOOL_T   told    = FALSE;

    for (;;) {
        if (app_storage_ready() && !sg_warming) {
            return TRUE;
        }

        /* 卡没就绪时只报一次，别每 5 秒刷屏；后面要看进度把日志级别开到 DEBUG。
         * 真卡住时这行会停在屏幕上，说明卡在哪一步没过。 */
        if (!told) {
            told = TRUE;
            PR_WARN("[sto] 等 SD 就绪：卡%s", app_storage_card_present()
                                                  ? "在位但读不到，后台重建中"
                                                  : "不在位，请插入");
        }

        /* **timeout_ms == 0 = 永久等待，不超时。**
         *
         * 卡对本产品相当于外挂 flash：精灵、字库、备忘录全在它上面，"卡不在"
         * 不是一种可降级的状态。所以开机必须等到底 —— 以前给 10 秒超时、超时就
         * 弹状态页，那条路在真机上是**必然崩溃**的：首页进不去 -> 路由没初始化
         * -> splash 还是活动屏幕却被删掉 -> LVGL 没有屏幕（见 app_splash.c）。 */
        if (0 != timeout_ms &&
            (tal_system_get_millisecond() - t0) >= timeout_ms) {
            PR_WARN("[sto] 等 SD 就绪超时（%ums）：卡%s", (unsigned)timeout_ms,
                    app_storage_card_present() ? "在位但没起来" : "不在位");
            return FALSE;
        }

        /* 之后的进度只走 DEBUG，默认 INFO 下不再刷屏。 */
        if (0 == timeout_ms &&
            (tal_system_get_millisecond() - last_log) >= 5000u) {
            last_log = tal_system_get_millisecond();
            PR_DEBUG("[sto] 还在等 SD：卡%s", app_storage_card_present()
                                                ? "在位（重建/预热中）"
                                                : "不在位");
        }

        tal_system_sleep(50);
    }
}

BOOL_T app_storage_ready(void)
{
    /* 卡不在位（CD 引脚）或链路已经判死（重建救不回来）都算不能用 ——
     * 判据统一收在 app_storage_fault() 里，这里只是它的一个谓词形式。 */
    return (APP_SD_OK == sg_fault) ? TRUE : FALSE;
}

/***********************************************************
******************** 卡在位检测（CD） ***********************
***********************************************************/
/* 板级实测极性：TF 座的 CD 触点插卡时短到 GND，引脚内部上拉 —— 所以
 * **拔出 = 高 / 插入 = 低**。（board.json 的 sd-card-1 里 role=cd → GPIO 8） */
#define STORAGE_CD_GPIO         TUYA_GPIO_NUM_8
/* 轮询 20ms、连续 5 次同电平才算数 —— **去抖窗口 100ms**。
 *
 * 这个值不是随便定的：所有"读错→重建"的阈值都必须**大于它**（见各模块的
 * *_READ_RECOVER_AFTER），否则卡拔掉之后、CD 还没确认的那段时间里，
 * card_present() 还是 TRUE，读者会误以为"卡在、只是读坏了"而白重建一趟。
 * 各模块的阈值按 200ms 取，留一倍余量。 */
#define STORAGE_CD_TICK_MS      20      /* 轮询周期 */
#define STORAGE_CD_DEBOUNCE     5       /* 连续读到同一电平几次才算数（=100ms） */

static BOOL_T prv_cd_read(void)
{
    TUYA_GPIO_LEVEL_E lv = TUYA_GPIO_LEVEL_HIGH;

    tkl_gpio_read(STORAGE_CD_GPIO, &lv);

    return (TUYA_GPIO_LEVEL_LOW == lv) ? TRUE : FALSE;      /* 低 = 插着 */
}

static void prv_cd_poll(TIMER_ID id, void *arg)
{
    BOOL_T now = prv_cd_read();

    (void)id;
    (void)arg;

    if (now == sg_cd_present) {
        sg_cd_same = 0;
        return;
    }
    if (++sg_cd_same < STORAGE_CD_DEBOUNCE) {
        return;                             /* 抖一下不算 */
    }

    sg_cd_same    = 0;
    sg_cd_present = now;

    /* 掉线 = 故障；插回来 = 故障解除（链路重建判的 DAMAGED 也一并清掉 ——
     * 卡都换过一张了，旧的判定没有意义）。 */
    sg_fault = now ? APP_SD_OK : APP_SD_ABSENT;
    sg_gen++;               /* 目录索引全部失效（各模块按代次判断要不要重扫） */

    /* 插回来的卡要**重新预热**：预热线程跑完一轮才解除。拔掉时顺手清掉
     * （那期间 fault=ABSENT，页面本来就进不去，留着没意义）。 */
    sg_warming = now ? TRUE : FALSE;

    PR_NOTICE("[sto] SD 卡%s", now ? "已插入" : "已拔出");
    /* 只在**变化**时发一次 —— 订阅方（路由推"请插卡"页、浮层重开句柄）不需要
     * 周期性提醒。 */
    app_event_publish(APP_EVT_SD_STATUS, (void *)(intptr_t)(now ? 1 : 0));
}

OPERATE_RET app_storage_cd_init(void)
{
    TUYA_GPIO_BASE_CFG_T gc = {0};
    OPERATE_RET          rt;

    if (sg_cd_inited) {
        return OPRT_OK;
    }

    /* "扫描 vs 重建"那把互斥先建出来（见 app_storage.h 的 app_storage_busy_enter）。
     * 这里是最早的单线程时机：预热线程还没起、页面也还没进，之后两个并发者才
     * 可能出现。放在函数最前面，免得下面任何一条早退把它漏掉。 */
    if (NULL == sg_busy_lock) {
        OPERATE_RET lrt = tal_mutex_create_init(&sg_busy_lock);

        if (OPRT_OK != lrt) {
            PR_ERR("[sto] 扫描/重建互斥量创建失败: %d", lrt);
        }
    }

    gc.direct = TUYA_GPIO_INPUT;
    gc.mode   = TUYA_GPIO_PULLUP;           /* 没卡时靠它拉到高 */
    tkl_gpio_init(STORAGE_CD_GPIO, &gc);

    sg_cd_present = prv_cd_read();          /* 先读一次，别让头 300ms 状态是猜的 */
    sg_cd_same    = 0;
    sg_fault      = sg_cd_present ? APP_SD_OK : APP_SD_ABSENT;
    /* 开机就标预热中（有卡才标）：三个列表还没扫，这期间 SD 页面先挡着，
     * 免得用户点进去对着冻屏等 3 秒。见 app_storage_warming()。 */
    sg_warming    = sg_cd_present;

    rt = tal_sw_timer_create(prv_cd_poll, NULL, &sg_cd_timer);
    if (OPRT_OK != rt) {
        PR_ERR("[sto] CD 轮询定时器创建失败: %d", rt);
        return rt;
    }
    tal_sw_timer_start(sg_cd_timer, STORAGE_CD_TICK_MS, TAL_TIMER_CYCLE);

    sg_cd_inited = TRUE;
    PR_NOTICE("[sto] 卡在位检测就绪（CD=P8，当前%s）",
              sg_cd_present ? "有卡" : "无卡");
    return OPRT_OK;
}

BOOL_T app_storage_card_present(void)
{
    return sg_cd_present;
}

/***********************************************************
******************* 读错恢复（链路重建） *******************
***********************************************************/
/* **这是"读错"那条路，跟"卡在不在"（上面的 CD 检测）是两回事。**
 *
 * 卡还插着、但读出来的数据是坏的 —— 背景（2026-09-25 真机）：播放中 SDIO 一次
 * FIFO 读超时（-14599）后，FatFs disk_read 的失败处理只做轻量复位 + 重试一次，
 * 重试"成功"返回 BK_OK，但卡/host FIFO 已进入"每次读都成功、数据却是坏的"死态
 * —— 表现为 JPEG 全部 JpegdecInit error 6（JDR_FMT1 = 数据损坏），无限刷屏、
 * 画面定格。轻量复位救不回来，只有完整重建（host 反初始化 + 重新识别卡）有效。
 *
 * **前提：调用方必须已关闭 SD 上的全部文件句柄**，否则 unmount 行为未定义。
 * 典型调用者是动画播放循环：角色页上它唯一常开的句柄就是它自己那个
 * （背景图 bg.rgb 是读完即关的）。 */

/* 按当前线宽重来一遍：unmount -> 卡 deinit/init -> mount。只负责"起卡"，
 * 能不能读由调用者验证（mount 的返回值对"已挂载的卷"也是非 0，不能当判据）。 */
static OPERATE_RET prv_remount(void)
{
    bk_err_t sdret;

    (void)tkl_fs_unmount(APP_STORAGE_MOUNT);
    sg_mounted = FALSE;

    sdret = bk_sd_card_deinit();
    if (0 != sdret) {
        /* "没初始化过"也返回非 0，不算致命 —— 反正后面会 init */
        PR_DEBUG("[sto] sd deinit 返回 %d（继续）", (int)sdret);
    }

    sdret = bk_sd_card_init();
    if (0 != sdret) {
        PR_ERR("[sto] SD 重新初始化失败: %d", (int)sdret);
        return OPRT_COM_ERROR;
    }

    (void)tkl_fs_mount(APP_STORAGE_MOUNT, DEV_SDCARD);
    sg_media_gen++;                 /* 重挂过了：所有已打开的句柄作废 */
    return OPRT_OK;
}

/* 重建的**本体**（调用方必须已持 busy 锁）。
 *
 * 拆出来是为了让 app_storage_prepare() 能在锁里做"先读一下再决定要不要重建" ——
 * 那一步必须在锁内，否则查完排队等锁的这段时间里别人可能已经把链路修好了，
 * 轮到你就白重建一趟（真机日志里连着两条"链路重建开始"就是这么来的）。 */
static OPERATE_RET prv_recover_locked(void)
{
    /* 刚判死、还没到重试间隔：直接失败。理由见 STORAGE_DAMAGED_RETRY_MS。 */
    if (APP_SD_DAMAGED == sg_fault
        && (tal_system_get_millisecond() - sg_damaged_ms) < STORAGE_DAMAGED_RETRY_MS) {
        PR_DEBUG("[sto] 刚判死，%ums 内不重试", (unsigned)STORAGE_DAMAGED_RETRY_MS);
        return OPRT_COM_ERROR;
    }

    PR_WARN("[sto] SD 链路重建开始（unmount -> 卡复位 -> mount）");

    /* 两轮"整条重来"，每轮之后用**真读一次**来验证，验证本身也重试几次。
     *
     * 为什么不能"mount 返回 0 就算成功"：AI 对话 + 动画同时跑时 SDIO 的读会
     * 偶发超时（平台层是 CPU 轮询收 FIFO，等数据的超时只有 400us），重建完成后
     * 紧跟的第一次 fopen 就可能撞上这个窗口 —— 真机上见过 mount 成功、第一次
     * fopen 失败，隔几十毫秒同样的 fopen 就成功了。老版本（开关=0 的那条分支）
     * 只看 mount 返回值，于是"重建成功"却重开不了文件，整个动作被丢掉。 */
    for (uint32_t attempt = 0; attempt < STORAGE_RECOVER_ATTEMPT; attempt++) {
        if (OPRT_OK != prv_remount()) {
            break;                          /* 卡都起不来，重试没意义 */
        }

        for (uint32_t i = 0; i < STORAGE_VERIFY_RETRY; i++) {
            if (TRUE == prv_readable()) {
                sg_mounted = TRUE;
                sg_fault   = APP_SD_OK;
                /* 开机那次没挂上、靠这条路径挂起来的：把读写档补回来，
                 * 否则 sg_write_lines 永远是 0，照片保存会一直失败。
                 * 线宽沿用 tkl_sdio_init() 最后一次设置（开机固定 1 线）。 */
                if (0 == sg_sdio_lines) {
                    sg_sdio_lines = sg_fast_lines;
                }
                if (0 == sg_write_lines) {
                    sg_write_lines = APP_STORAGE_WRITE_LINES;
                    sg_write_clk   = APP_STORAGE_WRITE_CLK;
                }
                /* **不动 sg_gen**：重建只是把 SDIO 链路重新拉起来，卡还是那张卡、
                 * 目录内容没变。动它会让"插卡 -> 重建"这条每次必走的路把预热的
                 * 结果白作废一遍（见 app_storage_gen 的说明）。 */
                PR_NOTICE("[sto] SD 链路重建完成");
                return OPRT_OK;
            }
            tal_system_sleep(STORAGE_VERIFY_RETRY_MS);
        }

        PR_WARN("[sto] 重建后仍读不到卡，整条再来一次");
    }

#if APP_STORAGE_SDIO_4BIT
    /* 当前线宽重建不起来 —— 4 线的信号完整性问题常常是"跑了很久才暴露"
     * （开机时冷，跑热了才出错），这里退到 1 线再重建一次：慢比不能读强。 */
    if (4 == sg_sdio_lines && prv_try_lines(1)) {
        sg_sdio_lines = 1;
        sg_fault      = APP_SD_OK;
        PR_NOTICE("[sto] 已在 1 线恢复（降级）");
        return OPRT_OK;
    }
#endif
    PR_ERR("[sto] SD 链路重建失败");
    /* **判死**：卡还插着、但连完整重建都救不回来。这是"SD 卡损坏"的唯一来源，
     * 路由据此弹故障页（见 app_router_sd_damaged）。记下时刻用于退避。 */
    sg_fault      = APP_SD_DAMAGED;
    sg_damaged_ms = tal_system_get_millisecond();
    return OPRT_COM_ERROR;
}

OPERATE_RET app_storage_recover(void)
{
    OPERATE_RET rt;
    uint32_t    now = tal_system_get_millisecond();

    /* 频率闸：上一次重建还没过多久就再来一次，直接拒。
     *
     * 重建期间所有句柄作废，读者会跟着读坏、跟着再触发重建 —— 两个读者互相
     * 踢起来就是死循环（实测整屏都是"重建 SD 链路"，最后卡都起不来）。
     * 偶发读错靠重建是治不好的，等下一个窗口往往自己就好了。 */
    if (sg_recover_ms != 0 && (now - sg_recover_ms) < STORAGE_RECOVER_MIN_MS) {
        PR_DEBUG("[sto] 距上次重建 %u ms，跳过", (unsigned)(now - sg_recover_ms));
        return OPRT_COM_ERROR;
    }

    /* **等所有目录扫描跑完再动卷。** 重建会把整个卷 unmount 掉，扫描读到一半卷
     * 就没了 —— 真机上见过预热枚举 NES 目录时被重建打断，只扫出 6 个游戏。
     *
     * 等的是"秒级"的量（预热那一轮扫描），而重建本来就动辄一两秒，代价可以接受。 */
    app_storage_busy_enter();
    sg_recover_ms = tal_system_get_millisecond();
    rt = prv_recover_locked();
    app_storage_busy_leave();
    return rt;
}

OPERATE_RET app_storage_prepare(void)
{
    OPERATE_RET rt;

    if (!app_storage_card_present()) {
        return OPRT_COM_ERROR;
    }

    app_storage_busy_enter();

    /* **"先读一下"这一步必须在锁里。**
     *
     * 放在锁外的话：读一次发现链路是死的 -> 排队等锁 -> 等到了，可这段时间里
     * 别人（浮层）已经把链路重建好了 -> 你再重建一遍。真机日志里连着两条
     * `SD 链路重建开始` 就是这么来的，白等两秒。
     *
     * 开机那次会命中"能读"，直接过 —— 链路是 app_storage_sdio_setup() 刚起好的。 */
    if (TRUE == prv_readable()) {
        app_storage_busy_leave();
        return OPRT_OK;
    }

    PR_WARN("[sto] 链路读不动（刚插卡？），先重建再扫");
    rt = prv_recover_locked();
    app_storage_busy_leave();
    return rt;
}

APP_SD_FAULT_E app_storage_recover_reopen(TUYA_FILE *f, const char *path)
{
    /* **先判卡在不在。** 卡不在是"掉线"，不是"读错" —— 对着一张拔掉的卡重建
     * 毫无意义（只会把日志刷满、白等两秒），交给 CD + 路由那条路。 */
    if (!app_storage_card_present()) {
        return APP_SD_ABSENT;
    }

    if (NULL != f && NULL != *f) {
        tkl_fclose(*f);
        *f = NULL;
    }

    if (OPRT_OK != app_storage_recover()) {
        return APP_SD_DAMAGED;              /* 重建就失败了，句柄不用管 */
    }

    if (NULL == f) {
        return APP_SD_OK;                   /* 没有常开句柄的读者（如 NES）：重建完就行 */
    }

    /* 重建"成功"不等于立刻能读：真机上见过 mount 成功、紧接着的第一次 fopen
     * 失败，隔几十毫秒同样的 fopen 就成功了（卡刚 init 完还在忙、或那次读又撞上
     * SDIO 超时）。所以这里退让着重试，别一次失败就放弃整个读者。 */
    for (uint32_t rp = 0; rp < STORAGE_REOPEN_RETRY && NULL == *f; rp++) {
        if (0 != rp) {
            tal_system_sleep(STORAGE_REOPEN_RETRY_MS);
        }
        *f = tkl_fopen(path, "rb");
    }

    if (NULL == *f) {
        /* 链路重建起来了、卷也读得动，偏偏这个文件开不了。虽然严格说可能是
         * "文件被删了"而不是"卡坏了"，但对用户是同一件事：刚才还在放的东西读不
         * 出来了。按损坏处理 —— 至少有一页明确告诉他去看卡，而不是画面莫名其妙
         * 卡住。故障在重新插卡时自动清除。 */
        PR_ERR("[sto] 重建后重开 %s 失败，按 SD 故障处理", path);
        sg_fault = APP_SD_DAMAGED;
        return APP_SD_DAMAGED;
    }
    PR_NOTICE("[sto] 重建后重开 %s 成功", path);
    return APP_SD_OK;
}

int32_t app_storage_utf8_to_oem(const char *in, char *out, int32_t outsz)
{
    const uint8_t *p = (const uint8_t *)in;
    int32_t        o = 0;

    if (NULL == in || NULL == out || outsz <= 2) {
        return 0;
    }

    while ('\0' != *p && o < outsz - 2) {
        uint32_t       cp;
        unsigned short oem;

        if (p[0] < 0x80) {
            cp = p[0];
            p += 1;
        } else if (0xC0 == (p[0] & 0xE0) && 0x80 == (p[1] & 0xC0)) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (0xE0 == (p[0] & 0xF0) && 0x80 == (p[1] & 0xC0) &&
                   0x80 == (p[2] & 0xC0)) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if (0xF0 == (p[0] & 0xF8) && 0x80 == (p[1] & 0xC0) &&
                   0x80 == (p[2] & 0xC0) && 0x80 == (p[3] & 0xC0)) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) |
                 ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            p += 1;
            cp = '?';
        }

        oem = ff_uni2oem(cp, STORAGE_OEM_CP);
        if (0 == oem) {
            oem = '?';
        }
        /* 双字节字符 FF 返回的是"高字节在前"的打包值，正是 f_open 期望的顺序 */
        if (oem > 0xFF) {
            out[o++] = (char)(oem >> 8);
        }
        out[o++] = (char)(oem & 0xFF);
    }

    out[o] = '\0';
    return o;
}

int32_t app_storage_oem_to_utf8(const char *in, char *out, int32_t outsz)
{
    const uint8_t *p = (const uint8_t *)in;
    int32_t        o = 0;

    if (NULL == in || NULL == out || outsz <= 4) {
        return 0;
    }

    while ('\0' != *p && o < outsz - 4) {
        unsigned short uni;

        if (p[0] < 0x80) {
            out[o++] = (char)p[0];          /* ASCII 直通 */
            p += 1;
            continue;
        }
        if ('\0' == p[1]) {
            break;                          /* 落单的引导字节，丢弃 */
        }
        uni = ff_oem2uni(((unsigned int)p[0] << 8) | p[1], STORAGE_OEM_CP);
        if (0 == uni) {
            uni = '?';
        }
        p += 2;

        if (uni < 0x800) {
            out[o++] = (char)(0xC0 | (uni >> 6));
            out[o++] = (char)(0x80 | (uni & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (uni >> 12));
            out[o++] = (char)(0x80 | ((uni >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (uni & 0x3F));
        }
    }

    out[o] = '\0';
    return o;
}

BOOL_T app_storage_probe(const char *path, uint32_t *size_out)
{
    TUYA_FILE f;
    int64_t   size;

    if (NULL == path) {
        return FALSE;
    }

    f = tkl_fopen(path, "rb");
    if (NULL == f) {
        return FALSE;
    }
    tkl_fseek(f, 0, SEEK_END);
    size = tkl_ftell(f);
    tkl_fclose(f);

    /* 能不能打开，判据只有 fopen 成不成功 —— 大小只是给 UI 看的。
     * 平台底层 `typedef long int off_t` 是 32 位有符号（bk_vfs_lseek 的
     * offset 参数、tkl_ftell 的返回都用它），tkl_ftell 对 >= 2GiB 的文件会
     * 回绕成负数。以前这里写 `size <= 0 → FALSE`，于是 2.4GB 的影片在目录
     * 扫描里被判成"打不开"、整个 continue 跳过，列表里根本不出现
     * （2026-09-24 实测定位）。 */
    if (size < 0) {
        size = 0;                       /* 超大文件取不到大小，只丢显示 */
    }
    if (NULL != size_out) {
        *size_out = (uint32_t)size;
    }
    return TRUE;
}

OPERATE_RET app_storage_open(const char *dir, const char *name,
                             char *path_out, int32_t path_sz)
{
    char    cand[APP_STORAGE_PATH_MAX];
    char    oem[APP_STORAGE_NAME_MAX * 2];

    if (NULL == dir || NULL == name || NULL == path_out || path_sz <= 0) {
        return OPRT_INVALID_PARM;
    }

    /* 形式一：目录给出的原样字节（ASCII 名走这条） */
    snprintf(cand, sizeof(cand), "%s/%s", dir, name);
    if (app_storage_probe(cand, NULL)) {
        snprintf(path_out, (size_t)path_sz, "%s", cand);
        return OPRT_OK;
    }

    /* 形式二：重新编码成 FatFs 的 OEM 代码页 */
    if (app_storage_utf8_to_oem(name, oem, (int32_t)sizeof(oem)) > 0) {
        snprintf(cand, sizeof(cand), "%s/%s", dir, oem);
        if (app_storage_probe(cand, NULL)) {
            snprintf(path_out, (size_t)path_sz, "%s", cand);
            return OPRT_OK;
        }
    }

    return OPRT_NOT_FOUND;
}

/* 目录能不能打开，只能靠 opendir 真试一次 —— 和文件那边的 fopen 验证同构。
 * （FatFs 的卷级互斥只在单次调用内进出，不跨调用持有，所以枚举过程中再打开
 * 一个子目录、甚至嵌套枚举，都是安全的。） */
static BOOL_T prv_dir_try(const char *path)
{
    TUYA_DIR d = NULL;

    if (0 != tkl_dir_open(path, &d)) {
        return FALSE;
    }
    tkl_dir_close(d);
    return TRUE;
}

OPERATE_RET app_storage_open_dir(const char *dir, const char *name,
                                 char *path_out, int32_t path_sz)
{
    char cand[APP_STORAGE_PATH_MAX];
    char oem[APP_STORAGE_NAME_MAX * 2];

    if (NULL == dir || NULL == name || NULL == path_out || path_sz <= 0) {
        return OPRT_INVALID_PARM;
    }

    /* 形式一：目录给出的原样字节（本平台的中文名走这条） */
    snprintf(cand, sizeof(cand), "%s/%s", dir, name);
    if (prv_dir_try(cand)) {
        snprintf(path_out, (size_t)path_sz, "%s", cand);
        return OPRT_OK;
    }

    /* 形式二：重新编码成 FatFs 的 OEM 代码页 */
    if (app_storage_utf8_to_oem(name, oem, (int32_t)sizeof(oem)) > 0) {
        snprintf(cand, sizeof(cand), "%s/%s", dir, oem);
        if (prv_dir_try(cand)) {
            snprintf(path_out, (size_t)path_sz, "%s", cand);
            return OPRT_OK;
        }
    }

    return OPRT_NOT_FOUND;
}

int32_t app_storage_iterate_dirs(const char *dir, APP_STORAGE_DIR_CB cb, void *user)
{
    TUYA_DIR      dir_h = NULL;
    TUYA_FILEINFO info = NULL;
    const char   *raw = NULL;
    int32_t       hits = 0;
    char          disp[APP_STORAGE_PATH_MAX];   /* 日志用：OEM 字节转成 UTF-8 */

    if (NULL == dir || NULL == cb) {
        return OPRT_INVALID_PARM;
    }
    if (OPRT_OK != app_storage_init()) {
        return OPRT_COM_ERROR;
    }

    if (0 != tkl_dir_open(dir, &dir_h)) {
        PR_WARN("[sto] 目录打不开: %s", dir);
        return OPRT_COM_ERROR;
    }

    while (0 == tkl_dir_read(dir_h, &info) && info) {
        APP_DIR_T d;
        BOOL_T    is_dir = FALSE;
        BOOL_T    is_reg = FALSE;

        raw = NULL;
        tkl_dir_name(info, &raw);
        if (NULL == raw || '\0' == raw[0]) {
            continue;
        }
        if (0 == strcmp(raw, ".") || 0 == strcmp(raw, "..")) {
            continue;
        }

        /* d_type 先在前面挡一道，只为少打几条 SDK 的 "tkl_dir_open failed"：
         * 它明确说"是常规文件"就一定不是目录。回 DT_UNKNOWN 时（部分 VFS
         * 后端不填 d_type）落到下面的 opendir 探测 —— 那才是可靠判据。 */
        tkl_dir_is_directory(info, &is_dir);
        if (FALSE == is_dir) {
            tkl_dir_is_regular(info, &is_reg);
            if (TRUE == is_reg) {
                continue;
            }
        }

        memset(&d, 0, sizeof(d));
        if (OPRT_OK != app_storage_open_dir(dir, raw, d.path, (int32_t)sizeof(d.path))) {
            continue;
        }
        app_storage_oem_to_utf8(raw, d.name, (int32_t)sizeof(d.name));

        hits++;
        if (FALSE == cb(&d, user)) {
            break;                          /* 调用者要求提前结束 */
        }
    }
    tkl_dir_close(dir_h);

    /* **打日志前把路径转成 UTF-8。** dir 可能是调用方从上层目录项里拿到的 OEM
     * 码页字节（本工程 CP936），那不是合法 UTF-8 —— 串口按 UTF-8 解就成了
     * `ս��1` 这种乱码（`战` 的 CP936 字节 0xD5 0xBD 正好是 U+057D 的 UTF-8）。
     * 纯 ASCII 的路径转不转一样，所以这一句对 "/sdcard/music" 那种没影响。 */
    app_storage_oem_to_utf8(dir, disp, (int32_t)sizeof(disp));
    PR_DEBUG("[sto] %s 命中 %d 个目录", disp, (int)hits);
    return hits;
}

int32_t app_storage_iterate(const char *dir, const char *ext,
                            APP_STORAGE_HIT_CB cb, void *user)
{
    TUYA_DIR    dir_h = NULL;
    TUYA_FILEINFO info = NULL;
    const char *raw = NULL;
    BOOL_T      is_reg = FALSE;
    size_t      ext_len;
    int32_t     hits = 0;
    char        disp[APP_STORAGE_PATH_MAX];     /* 日志用：OEM 字节转成 UTF-8 */

    if (NULL == dir || NULL == cb) {
        return OPRT_INVALID_PARM;
    }
    if (OPRT_OK != app_storage_init()) {
        return OPRT_COM_ERROR;
    }

    ext_len = (NULL != ext) ? strlen(ext) : 0;

    if (0 != tkl_dir_open(dir, &dir_h)) {
        PR_WARN("[sto] 目录打不开: %s", dir);
        return OPRT_COM_ERROR;
    }

    while (0 == tkl_dir_read(dir_h, &info) && info) {
        APP_FILE_T file;                    /* 每次迭代重建，回调可直接读 */

        raw = NULL;
        is_reg = FALSE;
        tkl_dir_name(info, &raw);
        if (NULL == raw || '\0' == raw[0]) {
            continue;
        }
        tkl_dir_is_regular(info, &is_reg);
        if (!is_reg) {
            continue;
        }
        if (ext_len > 0) {
            size_t len = strlen(raw);
            if (len <= ext_len || 0 != strcasecmp(raw + len - ext_len, ext)) {
                continue;
            }
        }

        memset(&file, 0, sizeof(file));
        if (OPRT_OK != app_storage_open(dir, raw, file.path, (int32_t)sizeof(file.path))) {
            PR_DEBUG("[sto] 打不开，跳过: %s/%s", dir, raw);
            continue;
        }
        app_storage_probe(file.path, &file.size);
        /* 目录名是 OEM 字节，转成 UTF-8 再给 UI 用 */
        app_storage_oem_to_utf8(raw, file.name, (int32_t)sizeof(file.name));

        hits++;
        if (FALSE == cb(&file, user)) {
            break;                          /* 调用者要求提前结束 */
        }
    }
    tkl_dir_close(dir_h);

    /* 同 iterate_dirs：dir 可能是 OEM 码页字节，日志里要先转 UTF-8（见那里的说明）。 */
    app_storage_oem_to_utf8(dir, disp, (int32_t)sizeof(disp));
    PR_DEBUG("[sto] %s 命中 %d 个 %s", disp, (int)hits, (NULL != ext) ? ext : "*");
    return hits;
}

OPERATE_RET app_storage_read_all(const char *path, void *dst, uint32_t expect)
{
    TUYA_FILE f;
    int32_t   rd;

    if (NULL == path || NULL == dst || 0 == expect) {
        return OPRT_INVALID_PARM;
    }

    f = tkl_fopen(path, "rb");
    if (NULL == f) {
        return OPRT_NOT_FOUND;
    }
    rd = tkl_fread(dst, (int32_t)expect, f);
    tkl_fclose(f);

    if (rd != (int32_t)expect) {
        PR_ERR("[sto] 短读 %s: %d/%u", path, rd, (unsigned)expect);
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}
