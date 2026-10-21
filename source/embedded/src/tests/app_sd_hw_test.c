/**
 * @file app_sd_hw_test.c
 * @brief SD 卡读通路 A/B：**CPU 轮询 FIFO** vs **GDMA**（硬件读）。
 *
 * 背景
 * ----
 * SDK 的 SD 读有两套实现，选哪套只看建卡时的 `dma_rx_en`：
 *
 *   `bk_sdio_host_read_blks_fifo()`
 *     ├─ dma_rx_en = 0 → `sdio_host_cpu_read_blks_fifo()`   CPU 一个个 32bit 搬
 *     └─ dma_rx_en = 1 → `sdio_dma_read_fifo()`             GDMA 分段搬 + 信号量等
 *
 * 本工程一直用 CPU 那条。GDMA 那条在 2026-10-12 试过一次就被关了，记录在
 * `projects/tuya_app/ap/config/bk7258_ap/config`：读了 11 秒就出 -6722（DMA 完成
 * 信号量 5.12 秒超时），而且**调用者缓冲区的尾部 canary 被踩** —— DMA 写到
 * 外面去了。怀疑是分段链式重启（sdio_dma_rx_finish / sdio_dma_rx_start）的记账
 * 在多段传输时跑飞。
 *
 * 现在重新确认一遍。这个测试把范围压到最小：
 *   - 不挂 FatFs、不碰任何业务代码，直接调块层 `bk_sd_card_read_blocks()`；
 *   - 同一批用例两条通路各跑一遍，比**带宽**、**短读次数**、**最长单次耗时**
 *     （5.12 秒死等会直接显示在"最大"那一列）；
 *   - 缓冲区两端放 canary，专盯"多搬一段"的越界写；
 *   - 两条通路读同一段块，**逐字节比对**，确认数据是否一致。
 *
 * 结论拿到后把 APP_SD_HW_TEST 改回 0。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"

#include "driver/sd_card.h"
#include "driver/sdio_host_types.h"

#include "app_sd_hw_test.h"

#if APP_SD_HW_TEST

#define SDHT_TAG            "[sdht]"

#define SDHT_BLOCK          512u
/* 大块：792576 B = 1548 块 —— 就是精灵 idle 常驻那一次读的量级 */
#define SDHT_BIG_BLKS       1548u
#define SDHT_BIG_N          4u
/* 中块：32KB = 预取一批的量级 */
#define SDHT_MID_BLKS       64u
#define SDHT_MID_N          128u
/* 单扇区：走 FAT 链时 FAT 扇区的量级 */
#define SDHT_SMALL_N        256u
/* 散布：8 块一组、随机地址，最像"跳着读" */
#define SDHT_SCAT_BLKS      8u
#define SDHT_SCAT_N         64u
/* 内部 SRAM 缓冲：确认"缓冲区在 PSRAM 还是 SRAM"会不会改变 GDMA 的行为 */
#define SDHT_SRAM_BLKS      128u
#define SDHT_SRAM_N         8u

#define SDHT_CANARY         0x5A5AA5A5u
#define SDHT_BASE_BLK       4096u           /* 从 2MB 处开始，避开分区表/FAT */
/* 卡头 256 块（128KB）：MBR + 引导扇区 + FAT 开头。
 * **这段一定有非零数据** —— 测试区如果落在空闲区（全 0），两条通路比出来
 * "一致"毫无意义（2026-10-21 就这么被坑过一轮）。 */
#define SDHT_HDR_BLKS       256u
/* 扫描范围：卡的前 64MB，每 64KB 一段。不挑固定偏移 —— 卡上哪里有数据就扫到
 * 哪里（FAT、目录、文件数据都在这个范围内），避免又撞进空闲区比一堆 0。
 * 64MB 也覆盖了 16~32GB 卡上 FAT32/exFAT 的默认布局：簇堆通常在前几 MB，
 * 55MB 的精灵文件跟在后面。 */
#define SDHT_SCAN_CHUNKS    1024u
#define SDHT_SCAN_BLKS      128u                        /* 每段 128 块 = 64KB */

/* tkl_sdio_init() 只有 .c 定义、没有对外头文件，和 app_storage.c 一样自己 extern */
extern OPERATE_RET tkl_sdio_init(int port, const TUYA_SDIO_BASE_CFG_T *cfg);

/* SDK 里读通路 DMA 的全局覆盖，见 sd_card_driver.c 的定义。 */
extern int g_sd_card_dma_rx_override;
extern int g_sd_card_dma_rx_compiled;

typedef struct {
    uint32_t pre;
    uint8_t  buf[SDHT_BIG_BLKS * SDHT_BLOCK];
    uint32_t post;
} SDHT_BUF_T;

/* 内部 SRAM 那份只要 64KB，单独一种结构 —— 不能拿 SDHT_BUF_T 强转，
 * 那样 canary 会落在分配区外面。 */
typedef struct {
    uint32_t pre;
    uint8_t  buf[SDHT_SRAM_BLKS * SDHT_BLOCK];
    uint32_t post;
} SDHT_SBUF_T;

/* 卡头 128KB 的区域，两条通路各占一份，用来比对"有真实数据"的地方。 */
typedef struct {
    uint32_t pre;
    uint8_t  buf[SDHT_HDR_BLKS * SDHT_BLOCK];
    uint32_t post;
} SDHT_HBUF_T;

typedef struct {
    const char *name;
    uint32_t    n;          /* 请求次数 */
    uint32_t    bad;        /* 失败次数（错误码非 0） */
    uint32_t    canary;     /* canary 被踩次数 */
    uint32_t    ms;         /* 累计耗时 */
    uint32_t    max_ms;     /* 单次最大耗时 */
    uint32_t    bytes;      /* 累计字节 */
    int32_t     last_err;   /* 最后一次错误码 */
} SDHT_STAT_T;

static SDHT_BUF_T  *sg_ps;      /* PSRAM 大缓冲（业务里的字形/精灵缓冲都在 PSRAM） */
static SDHT_BUF_T  *sg_chk;     /* 同一通路重复读的第二份，查"单条通路自己稳不稳" */
static SDHT_SBUF_T *sg_sr;      /* 内部 SRAM 缓冲 */
static SDHT_HBUF_T *sg_h_cpu;   /* 卡头区域：CPU 通路读到的那份 */
static SDHT_HBUF_T *sg_h_dma;   /* 卡头区域：GDMA 通路读到的那份 */
static uint8_t     *sg_ref;     /* CPU 通路大块读的副本 */
static uint8_t     *sg_ref_dma; /* GDMA 通路大块读的副本（扫描会覆盖 sg_ps，必须另存） */
static uint32_t     sg_blocks;  /* 卡的总块数 */

/* 扫描结果：每 64KB 一段的 32bit 校验和。只存校验和是因为要来回切通路 ——
 * CPU 跑一遍存和，GDMA 跑一遍比和，不需要同时留两份 16MB 数据。 */
static uint32_t     sg_sum_cpu[SDHT_SCAN_CHUNKS];
static uint32_t     sg_sum_dma[SDHT_SCAN_CHUNKS];
static uint8_t      sg_has_data[SDHT_SCAN_CHUNKS];   /* 这一段是否含非零字节 */

/* 真机实测这个函数给的是**字节数**（不是注释里说的扇区数）：
 * 卡 62410752 字节 = 121896 块。 */
extern uint32_t bk_sd_card_get_card_size(void);

/* D-cache 维护：GDMA 绕开 CPU cache 直接写内存，比对/查 canary 之前必须让
 * cache 里的旧行失效，否则看到的可能是上一次 CPU 写进去的残留。 */
extern void flush_dcache(void *va, long size);

static uint32_t prv_ms(void)
{
    return (uint32_t)tal_system_get_millisecond();
}

static uint32_t prv_zero_blocks(const uint8_t *buf, uint32_t nblk);

/* FNV-1a：够快也够敏感，用来比两段 64KB 是否一致。 */
static uint32_t prv_fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* 一次块读 + 记账。返回 TRUE = 成功且 canary 完好。 */
static BOOL_T prv_read(SDHT_STAT_T *st, uint8_t *buf, uint32_t *pre, uint32_t *post,
                       uint32_t blk, uint32_t nblk)
{
    uint32_t t0;
    uint32_t dt;
    uint32_t len = nblk * SDHT_BLOCK;
    bk_err_t rc;

    /* 先写回/失效：别让残留的脏行在 DMA 之后把新数据盖回去（这本身就是
     * 一个独立的坑：GDMA 写内存、CPU cache 里还有旧数据）。 */
    flush_dcache(buf, (long)len);

    t0 = prv_ms();
    rc = bk_sd_card_read_blocks(buf, blk, nblk);
    dt = prv_ms() - t0;

    flush_dcache(buf, (long)len);   /* 再失效一次：下面按内存里的真实内容判断 */

    st->n++;
    st->ms    += dt;
    st->bytes += len;
    if (dt > st->max_ms) {
        st->max_ms = dt;
    }

    if (0 != rc) {
        st->bad++;
        st->last_err = (int32_t)rc;
        return FALSE;
    }
    if (SDHT_CANARY != *pre || SDHT_CANARY != *post) {
        st->canary++;
        st->bad++;
        PR_ERR(SDHT_TAG " ****** 缓冲区被踩：canary pre=0x%08X post=0x%08X"
              "（块 %u +%u）—— DMA 写到缓冲区外面了 ******",
              (unsigned)*pre, (unsigned)*post, (unsigned)blk, (unsigned)nblk);
        *pre = *post = SDHT_CANARY;
        return FALSE;
    }
    return TRUE;
}

static void prv_stat_print(const char *mode, const SDHT_STAT_T *st)
{
    uint32_t kbs = st->ms ? (uint32_t)(((uint64_t)st->bytes * 1000u) / st->ms / 1024u) : 0;

    PR_NOTICE(SDHT_TAG " [%s] %-14s %3u 次，失败 %u（越界 %u）"
              "，均 %u ms 最大 %u ms，%u KB/s",
              mode, st->name, (unsigned)st->n, (unsigned)st->bad,
              (unsigned)st->canary, (unsigned)(st->ms / (st->n ? st->n : 1u)),
              (unsigned)st->max_ms, (unsigned)kbs);
    if (0 != st->bad) {
        PR_ERR(SDHT_TAG " [%s] %s 最后一次错误码 0x%X",
               mode, st->name, (unsigned)st->last_err);
    }
}

/* 逐字节比，返回不同字节数；不同则打印首个位置和那儿前后各 16 字节。
 * 连续大段都不同 = 错位 / 整段没搬；零星几个 = 局部坏块或 cache 竞争。 */
static uint32_t prv_diff(const uint8_t *a, const uint8_t *b, uint32_t len,
                         const char *tag_a, const char *tag_b)
{
    uint32_t n = 0;
    uint32_t first = len;
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            if (first == len) {
                first = i;
            }
            n++;
        }
    }
    if (0 == n) {
        return 0;
    }

    PR_ERR(SDHT_TAG " 不同字节 %u / %u，首个在 %u", (unsigned)n, (unsigned)len,
           (unsigned)first);
    if ((first + 16u) <= len) {
        PR_ERR(SDHT_TAG " %s: %02X %02X %02X %02X %02X %02X %02X %02X"
               " | %02X %02X %02X %02X %02X %02X %02X %02X",
               tag_a,
               a[first + 0], a[first + 1], a[first + 2], a[first + 3],
               a[first + 4], a[first + 5], a[first + 6], a[first + 7],
               a[first + 8], a[first + 9], a[first + 10], a[first + 11],
               a[first + 12], a[first + 13], a[first + 14], a[first + 15]);
        PR_ERR(SDHT_TAG " %s: %02X %02X %02X %02X %02X %02X %02X %02X"
               " | %02X %02X %02X %02X %02X %02X %02X %02X",
               tag_b,
               b[first + 0], b[first + 1], b[first + 2], b[first + 3],
               b[first + 4], b[first + 5], b[first + 6], b[first + 7],
               b[first + 8], b[first + 9], b[first + 10], b[first + 11],
               b[first + 12], b[first + 13], b[first + 14], b[first + 15]);
    }
    return n;
}

/* 一整套用例。
 *  save_to 非 NULL 时把大块读的结果另存一份（两条通路各存各的，扫描会复用
 *  sg_ps->buf，必须先存走）；hb 是卡头区域本阶段要填的那一份缓冲 ——
 *  **显式传进来**，别再靠别的参数猜（靠猜的后果是有一条路径永远没被写过，
 *  比对时拿未初始化的 PSRAM 当数据，白判一轮）。 */
static void prv_run_suite(const char *mode, uint8_t *save_to, SDHT_HBUF_T *hb,
                          SDHT_STAT_T st[5], BOOL_T sram_ok)
{
    uint32_t i;
    uint32_t base = SDHT_BASE_BLK;
    uint32_t seed = 0x1234ABCDu;
    const uint32_t big_len = SDHT_BIG_BLKS * SDHT_BLOCK;

    /* 1) 大块顺序。读之前先把缓冲填成 0xEE：万一 DMA 没覆盖整段，剩下的 0xEE
     * 会直接暴露"少搬了"，不会被上一轮的旧数据蒙混过去。 */
    st[0].name = "大块 1548块";
    for (i = 0; i < SDHT_BIG_N; i++) {
        memset(sg_ps->buf, 0xEE, big_len);
        flush_dcache(sg_ps->buf, (long)big_len);
        (void)prv_read(&st[0], sg_ps->buf, &sg_ps->pre, &sg_ps->post,
                       base, SDHT_BIG_BLKS);
        if (NULL != save_to && 0 == st[0].bad) {
            memcpy(save_to, sg_ps->buf, big_len);
        }
    }
    prv_stat_print(mode, &st[0]);

    /* 同一通路再读一遍同样的块，和上面那份比 —— 只看"这条通路自己稳不稳"。 */
    if (NULL != sg_chk) {
        uint32_t dn;

        memset(sg_chk->buf, 0xEE, big_len);
        flush_dcache(sg_chk->buf, (long)big_len);
        (void)prv_read(&st[0], sg_chk->buf, &sg_chk->pre, &sg_chk->post,
                       base, SDHT_BIG_BLKS);
        dn = prv_diff(sg_ps->buf, sg_chk->buf, big_len, mode, "重复");
        PR_NOTICE(SDHT_TAG " [%s] 同通路重复读：%s（%u 字节不同）",
                  mode, dn ? "**不一致**" : "一致", (unsigned)dn);
    }

    /* 卡头 256 块（MBR + 引导扇区 + FAT 开头）—— **一定有非零数据**。
     * 大块测试区如果落在空闲区（全 0），两条通路比出来"一致"没有区分度
     * （2026-10-21 被坑过一轮）；这一段才是真正的 A/B 战场。 */
    {
        SDHT_STAT_T  hst = {0};
        uint32_t     zero;

        hst.name = "卡头 128KB";
        memset(hb->buf, 0xEE, sizeof(hb->buf));
        flush_dcache(hb->buf, (long)sizeof(hb->buf));
        (void)prv_read(&hst, hb->buf, &hb->pre, &hb->post, 0, SDHT_HDR_BLKS);
        prv_stat_print(mode, &hst);

        zero = prv_zero_blocks(hb->buf, SDHT_HDR_BLKS);
        PR_NOTICE(SDHT_TAG " [%s] 卡头全 0 块 %u/%u%s", mode, (unsigned)zero,
                  (unsigned)SDHT_HDR_BLKS,
                  (SDHT_HDR_BLKS == zero) ? "（**全 0，读错位置**）" : "");
    }

    /* 2) 中块顺序（32KB ×128 = 4MB） */
    st[1].name = "中块 64块";
    for (i = 0; i < SDHT_MID_N; i++) {
        (void)prv_read(&st[1], sg_ps->buf, &sg_ps->pre, &sg_ps->post,
                       base + 2048u + i * SDHT_MID_BLKS, SDHT_MID_BLKS);
    }
    prv_stat_print(mode, &st[1]);

    /* 3) 单扇区顺序 */
    st[2].name = "单扇区";
    for (i = 0; i < SDHT_SMALL_N; i++) {
        (void)prv_read(&st[2], sg_ps->buf, &sg_ps->pre, &sg_ps->post,
                       base + 4096u + i, 1u);
    }
    prv_stat_print(mode, &st[2]);

    /* 4) 散布（随机地址，8 块一组） */
    st[3].name = "散布 8块";
    for (i = 0; i < SDHT_SCAT_N; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t blk = base + 8192u + (seed % (sg_blocks - base - 16384u));
        (void)prv_read(&st[3], sg_ps->buf, &sg_ps->pre, &sg_ps->post,
                       blk, SDHT_SCAT_BLKS);
    }
    prv_stat_print(mode, &st[3]);

    /* 5) 内部 SRAM 缓冲（64KB ×8） */
    st[4].name = "SRAM 128块";
    if (!sram_ok || NULL == sg_sr) {
        PR_WARN(SDHT_TAG " [%s] 内部 SRAM 缓冲分配失败，跳过", mode);
        return;
    }
    for (i = 0; i < SDHT_SRAM_N; i++) {
        (void)prv_read(&st[4], sg_sr->buf, &sg_sr->pre, &sg_sr->post,
                       base + 16384u + i * SDHT_SRAM_BLKS, SDHT_SRAM_BLKS);
    }
    prv_stat_print(mode, &st[4]);
}

/* 起卡。dma_rx：0 = CPU 轮询，1 = GDMA（只对编译期开了 CONFIG_SDIO_GDMA_EN 有效）。
 *
 * **必须重试。** 真机实测：boot 刚 mount 完就把卡 deinit 再 init 时，第二次起卡
 * 有一定概率失败（`sd card unsupport voltage` + CMD55 超时 -0x3902）—— 和业务
 * 侧 recovery 日志里的 `SD 重新初始化失败: -14601` 是同一个现象。SDK 自己的
 * disk_initialize 也是靠重试才起来的，这里照做。 */
static int prv_card_start(int dma_rx)
{
    TUYA_SDIO_BASE_CFG_T cfg = {
        .bus_width  = TUYA_SDIO_BUS_WIDTH_4BIT,
        .speed_mode = TUYA_SDIO_SPEED_DEFAULT,
        .voltage    = TUYA_SDIO_VOLTAGE_3V3,
        .clock_hz   = 0,
        .flags      = 0,
    };
    bk_err_t rc = -1;

    /* 刚 mount 完的卡还在忙，歇一下再拆 */
    tal_system_sleep(100);

    for (int i = 0; i < 5; i++) {
        if (i > 0) {
            (void)bk_sd_card_deinit();
            tal_system_sleep(50);
        }
        (void)tkl_sdio_init(0, &cfg);
        g_sd_card_dma_rx_override = dma_rx;

        rc = bk_sd_card_init();
        if (0 == rc) {
            (void)bk_sd_card_set_clock(SDIO_HOST_CLK_13M);
            if (i > 0) {
                PR_NOTICE(SDHT_TAG " dma_rx=%d 第 %d 次起卡成功", dma_rx, i + 1);
            }
            return 0;
        }
        PR_WARN(SDHT_TAG " dma_rx=%d 起卡失败 0x%X（第 %d 次），重试",
                dma_rx, (unsigned)rc, i + 1);
    }
    PR_ERR(SDHT_TAG " dma_rx=%d 起卡 5 次都失败：0x%X", dma_rx, (unsigned)rc);
    return -1;
}

/* 基准读：**一次只读一块**（CMD17 单块命令）。
 *
 * 单块读不涉及多块传输和 GDMA 分段，是两条被测通路之外最"笨"也最可信的读法。
 * 用它当裁判，判断 CPU 大块读和 GDMA 大块读到底谁给的数据是错的。 */
static uint32_t prv_single_ref(uint8_t *dst, uint32_t blk, uint32_t nblk)
{
    uint32_t bad = 0;
    uint32_t i;

    for (i = 0; i < nblk; i++) {
        flush_dcache(dst + i * SDHT_BLOCK, (long)SDHT_BLOCK);
        if (0 != bk_sd_card_read_blocks(dst + i * SDHT_BLOCK, blk + i, 1u)) {
            bad++;
        }
        flush_dcache(dst + i * SDHT_BLOCK, (long)SDHT_BLOCK);
    }
    return bad;
}

/* 数一数缓冲里有多少个"整块全 0"的块。
 * 卡上的数据里当然也可能有全 0 的块，但**同一段块**三条通路读出来应当一样多；
 * 某一条明显多出来，就是它把丢掉的块用 0 填了（CPU 通路的典型失败模式：
 * 只检查搬够字节数，不检查内容）。 */
static uint32_t prv_zero_blocks(const uint8_t *buf, uint32_t nblk)
{
    uint32_t n = 0;
    uint32_t i, j;

    for (i = 0; i < nblk; i++) {
        for (j = 0; j < SDHT_BLOCK; j++) {
            if (0 != buf[i * SDHT_BLOCK + j]) {
                break;
            }
        }
        if (SDHT_BLOCK == j) {
            n++;
        }
    }
    return n;
}

/* 以单块读为基准，按块统计两条通路各自错了几块，并列出前几个出错的块号。
 * 返回：0 = 都对，1 = CPU 错，2 = GDMA 错，3 = 都有错。 */
static int prv_judge(const uint8_t *ref, const uint8_t *cpu, const uint8_t *dma,
                     uint32_t blk0, uint32_t nblk)
{
    uint32_t bad_cpu = 0;
    uint32_t bad_dma = 0;
    uint32_t shown   = 0;
    uint32_t i;

    for (i = 0; i < nblk; i++) {
        const uint32_t off = i * SDHT_BLOCK;

        if (0 != memcmp(ref + off, cpu + off, SDHT_BLOCK)) {
            bad_cpu++;
        }
        if (0 != memcmp(ref + off, dma + off, SDHT_BLOCK)) {
            bad_dma++;
        }
        if (shown < 8 &&
            (0 != memcmp(ref + off, cpu + off, SDHT_BLOCK) ||
             0 != memcmp(ref + off, dma + off, SDHT_BLOCK))) {
            PR_ERR(SDHT_TAG " 块 #%u（绝对 %u）：基准 %02X%02X%02X%02X | "
                   "CPU %02X%02X%02X%02X | GDMA %02X%02X%02X%02X",
                   (unsigned)i, (unsigned)(blk0 + i),
                   ref[off + 0], ref[off + 1], ref[off + 2], ref[off + 3],
                   cpu[off + 0], cpu[off + 1], cpu[off + 2], cpu[off + 3],
                   dma[off + 0], dma[off + 1], dma[off + 2], dma[off + 3]);
            shown++;
        }
    }

    PR_NOTICE(SDHT_TAG " 基准（单块读）比对：CPU 错 %u 块 / GDMA 错 %u 块（共 %u 块）",
              (unsigned)bad_cpu, (unsigned)bad_dma, (unsigned)nblk);
    PR_NOTICE(SDHT_TAG " 整块全 0 的块数：基准 %u / CPU %u / GDMA %u",
              (unsigned)prv_zero_blocks(ref, nblk),
              (unsigned)prv_zero_blocks(cpu, nblk),
              (unsigned)prv_zero_blocks(dma, nblk));
    if (0 == bad_cpu && 0 == bad_dma) {
        PR_NOTICE(SDHT_TAG " >>> 两条通路都和基准一致");
        return 0;
    }
    if (0 == bad_dma && bad_cpu > 0) {
        PR_ERR(SDHT_TAG " >>> **CPU 通路读错**，GDMA 是对的");
        return 1;
    }
    if (0 == bad_cpu && bad_dma > 0) {
        PR_ERR(SDHT_TAG " >>> **GDMA 通路读错**，CPU 是对的");
        return 2;
    }
    PR_ERR(SDHT_TAG " >>> 两条通路都有错块");
    return 3;
}

/* 扫前 64MB，每 64KB 算一个校验和；顺便记下哪一段有非零数据。 */
static void prv_scan(const char *mode, uint32_t *out)
{
    uint32_t i;

    for (i = 0; i < SDHT_SCAN_CHUNKS; i++) {
        const uint32_t blk = i * SDHT_SCAN_BLKS;
        const uint32_t len = SDHT_SCAN_BLKS * SDHT_BLOCK;
        uint32_t k;
        BOOL_T   has = FALSE;

        memset(sg_ps->buf, 0xEE, len);
        flush_dcache(sg_ps->buf, (long)len);
        if (0 != bk_sd_card_read_blocks(sg_ps->buf, blk, SDHT_SCAN_BLKS)) {
            out[i] = 0xDEADBEEFu;
            sg_has_data[i] = 2;             /* 读失败 */
            continue;
        }
        flush_dcache(sg_ps->buf, (long)len);

        for (k = 0; k < len; k++) {
            if (0 != sg_ps->buf[k]) {
                has = TRUE;
                break;
            }
        }
        sg_has_data[i] = has ? 1 : 0;
        out[i] = prv_fnv(sg_ps->buf, len);
    }
    PR_NOTICE(SDHT_TAG " [%s] 扫描前 %u MB 完成", mode,
              (unsigned)(SDHT_SCAN_CHUNKS * SDHT_SCAN_BLKS * SDHT_BLOCK / (1024u * 1024u)));
}

/* 打印"哪里有数据"的地图：. = 全 0，'#' = 有数据，'!' = 读失败。
 * 每行 64 个字符 = 4MB。 */
static void prv_scan_map(void)
{
    char     line[65];
    uint32_t i;

    PR_NOTICE(SDHT_TAG " 64MB 数据地图（每字符 = 64KB，每行 = 4MB）：");
    for (i = 0; i < SDHT_SCAN_CHUNKS; i++) {
        line[i % 64u] = (2 == sg_has_data[i]) ? '!' : (sg_has_data[i] ? '#' : '.');
        if (63u == (i % 64u)) {
            line[64] = '\0';
            PR_NOTICE(SDHT_TAG "  %4u MB |%s|", (unsigned)(i / 64u * 4u), line);
        }
    }
}

void app_sd_hw_test_run(void)
{
    static SDHT_STAT_T cpu_st[5];
    static SDHT_STAT_T dma_st[5];
    BOOL_T sram_ok;
    BOOL_T same;
    int    verdict;

    PR_NOTICE("============================================================");
    PR_NOTICE(SDHT_TAG " SD 读通路 A/B 测试（不挂文件系统、不跑业务代码）");
    PR_NOTICE(SDHT_TAG " 编译期 CONFIG_SDIO_GDMA_EN = %d（0 = GDMA 代码没编进来，"
              "下面的 dma_rx=1 会退化成 CPU 轮询）", g_sd_card_dma_rx_compiled);
    PR_NOTICE("============================================================");

    /* 缓冲区：PSRAM 792KB ×3（测试 / 同通路复读 / CPU 结果副本）
     *         + 卡头 128KB ×2，SRAM 64KB ×1 */
    sg_ps   = (SDHT_BUF_T *)tal_psram_malloc(sizeof(SDHT_BUF_T));
    sg_chk  = (SDHT_BUF_T *)tal_psram_malloc(sizeof(SDHT_BUF_T));
    sg_ref  = (uint8_t *)tal_psram_malloc(SDHT_BIG_BLKS * SDHT_BLOCK);
    sg_ref_dma = (uint8_t *)tal_psram_malloc(SDHT_BIG_BLKS * SDHT_BLOCK);
    sg_h_cpu = (SDHT_HBUF_T *)tal_psram_malloc(sizeof(SDHT_HBUF_T));
    sg_h_dma = (SDHT_HBUF_T *)tal_psram_malloc(sizeof(SDHT_HBUF_T));
    sg_sr   = (SDHT_SBUF_T *)tal_malloc(sizeof(SDHT_SBUF_T));
    if (NULL == sg_ps || NULL == sg_chk || NULL == sg_ref || NULL == sg_ref_dma ||
        NULL == sg_h_cpu || NULL == sg_h_dma) {
        PR_ERR(SDHT_TAG " PSRAM 缓冲分配失败，测试中止");
        goto done;
    }
    sram_ok = (NULL != sg_sr);
    sg_ps->pre   = sg_ps->post   = SDHT_CANARY;
    sg_chk->pre  = sg_chk->post  = SDHT_CANARY;
    sg_h_cpu->pre = sg_h_cpu->post = SDHT_CANARY;
    sg_h_dma->pre = sg_h_dma->post = SDHT_CANARY;
    if (sram_ok) {
        sg_sr->pre = sg_sr->post = SDHT_CANARY;
    }

    /* ---- A：CPU 轮询 ---- */
    if (0 != prv_card_start(0)) {
        goto done;
    }
    /* get_card_size() 返回的是**扇区数**（SDK 注释和 mount 日志都对得上：
     * 62410752 × 512 ≈ 31.9GB）。不要再除 512。 */
    sg_blocks = bk_sd_card_get_card_size();
    PR_NOTICE(SDHT_TAG " 卡 %u 块（%u MB），测试基址 %u 块",
              (unsigned)sg_blocks, (unsigned)(sg_blocks / 2048u), (unsigned)SDHT_BASE_BLK);
    PR_NOTICE(SDHT_TAG " ---------- A：dma_rx=0（CPU 轮询 FIFO） ----------");
    prv_run_suite("CPU", sg_ref, sg_h_cpu, cpu_st, sram_ok);
    prv_scan("CPU", sg_sum_cpu);
    prv_scan_map();

    /* ---- B：GDMA ---- */
    if (0 != prv_card_start(1)) {
        goto done;
    }
    PR_NOTICE(SDHT_TAG " ---------- B：dma_rx=1（GDMA 硬件读） ----------");
    prv_run_suite("GDMA", sg_ref_dma, sg_h_dma, dma_st, sram_ok);
    prv_scan("GDMA", sg_sum_dma);

    /* ---- 16MB 扫描比对：**不依赖卡上恰好有数据的地方** ---- */
    {
        uint32_t bad = 0;
        uint32_t shown = 0;
        uint32_t i;

        for (i = 0; i < SDHT_SCAN_CHUNKS; i++) {
            if (sg_sum_cpu[i] != sg_sum_dma[i]) {
                bad++;
                if (shown < 8) {
                    PR_ERR(SDHT_TAG " 块 %u 起 64KB 不同：CPU %08X / GDMA %08X",
                           (unsigned)(i * SDHT_SCAN_BLKS),
                           (unsigned)sg_sum_cpu[i], (unsigned)sg_sum_dma[i]);
                    shown++;
                }
            }
        }
        PR_NOTICE(SDHT_TAG " 64MB 扫描：%u/%u 段不同%s",
                  (unsigned)bad, (unsigned)SDHT_SCAN_CHUNKS,
                  bad ? "" : "（两条通路读到的内容完全一致）");
        if (bad) {
            PR_ERR(SDHT_TAG " >>> **GDMA 与 CPU 读出的内容不同**，以 CPU 为准（单块基准见下）");
        }
    }

    /* ---- 数据一致性 ----
     * 大块区（2MB 处）可能整个落在空闲 FAT 区、全是 0 —— 那样比出来"一致"
     * 没有区分度，所以只当参考；**判定看卡头 128KB**（MBR/引导扇区/FAT 开头，
     * 一定有非零数据）。 */
    {
        uint32_t dn_big = prv_diff(sg_ref, sg_ref_dma, SDHT_BIG_BLKS * SDHT_BLOCK,
                                   "CPU ", "GDMA");
        uint32_t zero_big = prv_zero_blocks(sg_ref, SDHT_BIG_BLKS);
        uint32_t dn_hdr = prv_diff(sg_h_cpu->buf, sg_h_dma->buf,
                                   SDHT_HDR_BLKS * SDHT_BLOCK, "CPU ", "GDMA");

        PR_NOTICE(SDHT_TAG " 数据比对（1548 块）：%s（%u 字节不同，全 0 块 %u）",
                  dn_big ? "**不一致**" : "一致", (unsigned)dn_big, (unsigned)zero_big);
        if (SDHT_BIG_BLKS == zero_big) {
            PR_WARN(SDHT_TAG " 该区域全 0（空闲区），这一项没有区分度，以卡头为准");
        }
        PR_NOTICE(SDHT_TAG " 数据比对（卡头 128KB）：%s（%u 字节不同）",
                  dn_hdr ? "**不一致**" : "一致", (unsigned)dn_hdr);

        same = (0 == dn_hdr);
    }

    /* ---- 谁错：换回 CPU 模式，用**单块读**把卡头再读一遍做基准 ---- */
    verdict = -1;
    if (0 == prv_card_start(0)) {
        uint32_t rbad = prv_single_ref(sg_chk->buf, 0, SDHT_HDR_BLKS);

        if (0 == rbad) {
            verdict = prv_judge(sg_chk->buf, sg_h_cpu->buf, sg_h_dma->buf,
                                0, SDHT_HDR_BLKS);
        } else {
            PR_ERR(SDHT_TAG " 单块基准读本身就有 %u 块失败，基准不可信",
                   (unsigned)rbad);
        }
    }

    /* ---- 结论 ---- */
    PR_NOTICE("============================================================");
    PR_NOTICE(SDHT_TAG " 结论：CPU 失败 %u 次 / GDMA 失败 %u 次 / 数据%s",
              (unsigned)(cpu_st[0].bad + cpu_st[1].bad + cpu_st[2].bad +
                         cpu_st[3].bad + cpu_st[4].bad),
              (unsigned)(dma_st[0].bad + dma_st[1].bad + dma_st[2].bad +
                         dma_st[3].bad + dma_st[4].bad),
              same ? "一致" : "不一致");
    PR_NOTICE(SDHT_TAG " 大块带宽：CPU %u KB/s，GDMA %u KB/s",
              (unsigned)(cpu_st[0].ms ? ((uint64_t)cpu_st[0].bytes * 1000u / cpu_st[0].ms / 1024u) : 0),
              (unsigned)(dma_st[0].ms ? ((uint64_t)dma_st[0].bytes * 1000u / dma_st[0].ms / 1024u) : 0));
    if (0 == g_sd_card_dma_rx_compiled) {
        PR_WARN(SDHT_TAG " 注意：GDMA 没编进来，B 组实际也是 CPU 轮询 —— 两组数据应当几乎一样");
    } else if (2 == verdict) {
        PR_ERR(SDHT_TAG " >>> GDMA 不可用（基准判定：GDMA 读错）");
    } else if (1 == verdict) {
        PR_ERR(SDHT_TAG " >>> **业务现在用的 CPU 通路读错**，GDMA 反而是对的 —— "
                "这才是真正要修的东西");
    } else if (0 == verdict) {
        PR_NOTICE(SDHT_TAG " >>> 两条通路都与单块基准一致，GDMA 可用");
    } else {
        PR_WARN(SDHT_TAG " >>> 判定不完整（基准不可信或起卡失败），看上面的明细");
    }
    PR_NOTICE("============================================================");

done:
    /* 停住：不启动任何业务代码，串口日志就是结果。 */
    for (;;) {
        tal_system_sleep(5000);
    }
}

#endif /* APP_SD_HW_TEST */
