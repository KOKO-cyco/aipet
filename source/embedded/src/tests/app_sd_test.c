/**
 * @file app_sd_test.c
 * @brief 纯 SD 读压力测试 —— 只读卡，把视频解码 / 音频 / LVGL / WiFi 全摘掉。
 *
 * 为什么单独测：真机上播放一会儿就出 `-6722`（SDIO DMA 完成信号量 5.12 秒超时），
 * 而这张卡在 PC 上用软件验证过没问题、时钟从 20MHz 降到 13MHz 也没用、跟播什么
 * 内容无关、音乐也崩。在播放器那个上下文里永远分不清是"SD 通路本身"的问题，
 * 还是被别的负载（硬解 / DMA2D / 音频 / 网络）带出来的 —— 这个测试把其他全部去掉。
 *
 * 位置在 app_storage_sdio_setup() 之后、app_disp_init() 之前（见 tuya_app_main.c）：
 * 板级外设已注册（SDIO 引脚映射好了），而字体 / 预热 / 音乐都还没起来，
 * **整个系统只有这一个读者**。
 *
 * 三种读法分开统计，因为失败机制可能不同：
 *   1) 顺序大块读  32KB —— 正常播放的样子
 *   2) 随机跳读    20KB —— seek 的样子（最可疑）
 *   3) 单扇区小读   512B —— 走 FAT 链的样子
 *
 * 判据：`tkl_fread` 短读（读到的字节数 != 请求数）就是一次失败。SDK 自己会打
 * `read data fail ... -6722`，这里只统计次数和耗时，并在**第一次出错时停下** ——
 * 一次 -6722 要冻 5.12 秒，继续跑只会把日志冲烂。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"

#include "app_sd_test.h"

#if APP_SD_TEST

#include "app_storage.h"

#define SDT_TAG             "[sdt]"

#define SDT_SEQ_CHUNK       (32u * 1024u)           /* 顺序大块 */
#define SDT_SEQ_SMALL       512u                    /* 顺序小块（单扇区） */
#define SDT_RAND_CHUNK      (20u * 1024u)           /* 随机大块（约一帧 JPEG 的量级） */
#define SDT_SECTOR          512u                    /* 随机小块 */
#define SDT_SEQ_PER_ROUND   (4u * 1024u * 1024u)    /* 每轮顺序大块读 4MB */
#define SDT_SEQ_SMALL_N     2000u                   /* 每轮顺序小块读次数 */
#define SDT_RAND_N          200u                    /* 每轮随机大块读次数 */
#define SDT_SECTOR_N        400u                    /* 每轮随机小块读次数 */
#define SDT_BUF_MAX         SDT_SEQ_CHUNK

/* 测试缓冲放 .bss（32KB）—— 刻意不走堆：这一步要测的就是"纯读卡"，任何内存
 * 分配失败都会把结论搅浑。SRAM 空闲 110KB+，32KB 拿得起。
 *
 * **两端各放一个 canary**（运行时填，别用静态初值 —— 那会把 32KB 拖进 .data
 * 撑大固件）。理由：真机第一版测试里统计数字全成了天文数字（顺序"695139989
 * 次"，11 秒里读 32KB 一次 = 22TB/s，不可能），而且两次打印的垃圾一模一样，
 * 是内存被踩的典型迹象。SDIO 的 DMA 是**分段链式重启**的
 * （sdio_dma_rx_finish -> sdio_dma_rx_start 再起下一段），链长算错就会写到
 * 缓冲区外面 —— canary 就是用来确认这一点的。 */
#define SDT_CANARY  0xA5A5A5A5u

typedef struct {
    uint32_t pre;
    uint8_t  buf[SDT_BUF_MAX];
    uint32_t post;
} SDT_BUF_T;

static SDT_BUF_T sg_mem;

#define sg_buf  (sg_mem.buf)

static BOOL_T sg_canary_hit = FALSE;        /* 缓冲被踩过（DMA 越界写） */

static char     sg_path[APP_STORAGE_PATH_MAX];
static uint32_t sg_size = 0;

/* 阶段统计：尝试次数 / 短读次数 / 累计耗时(ms) */
typedef struct {
    uint32_t n;
    uint32_t bad;
    uint32_t ms;
} SDT_STAT_T;

static SDT_STAT_T sg_seq;                   /* 顺序大块 32KB */
static SDT_STAT_T sg_seqs;                  /* 顺序小块 512B */
static SDT_STAT_T sg_rand;                  /* 随机大块 20KB */
static SDT_STAT_T sg_sect;                  /* 随机小块 512B */

/***********************************************************
********************** 找测试文件 ***************************
***********************************************************/
/* 挑一个够大的 .anim（4MB 以上）—— 太小的话随机读没什么空间，
 * 也复现不出"读一会儿才崩"。 */
static BOOL_T prv_file_hit(const APP_FILE_T *f, void *user)
{
    (void)user;

    if (f->size < (4u * 1024u * 1024u)) {
        return TRUE;                        /* 太小，看下一个 */
    }
    strncpy(sg_path, f->path, sizeof(sg_path) - 1u);
    sg_size = f->size;
    return FALSE;                           /* 够了，停 */
}

static BOOL_T prv_dir_hit(const APP_DIR_T *d, void *user)
{
    (void)user;

    (void)app_storage_iterate(d->path, ".anim", prv_file_hit, NULL);
    return (sg_size > 0u) ? FALSE : TRUE;   /* 找到就停 */
}

static BOOL_T prv_pick_file(void)
{
    sg_path[0] = '\0';
    sg_size    = 0;

    if (OPRT_OK != app_storage_init()) {
        PR_ERR(SDT_TAG " SD 挂载失败，测试跳过");
        return FALSE;
    }
    /* 电影都是"一个文件夹 = 一部"，所以先枚举目录再进目录找 .anim */
    (void)app_storage_iterate_dirs("/sdcard/movie", prv_dir_hit, NULL);

    if (0u == sg_size) {
        PR_ERR(SDT_TAG " /sdcard/movie 下没找到 >4MB 的 .anim，测试跳过");
        return FALSE;
    }
    PR_NOTICE(SDT_TAG " 测试文件：%s（%u KB）", sg_path, (unsigned)(sg_size / 1024u));
    return TRUE;
}

/***********************************************************
*********************** 读一次 *****************************
***********************************************************/
/* 读 len 字节并判成败。返回 0 成功、-1 失败。
 * 短读就是一次失败 —— SDK 那边会同时打 "read data fail ... -6722"。
 * 每次读完先验 canary：踩了就是 DMA 越界写，那是比短读更严重的发现。 */
static int prv_read(TUYA_FILE f, uint32_t len)
{
    int32_t rd = tkl_fread(sg_buf, (int32_t)len, f);

    if (SDT_CANARY != sg_mem.pre || SDT_CANARY != sg_mem.post) {
        PR_ERR(SDT_TAG " ****** 缓冲区被踩！canary pre=%u post=%u（请求 %u 字节）"
              "—— SDIO 的 DMA 写到缓冲区外面了 ******",
              (unsigned)sg_mem.pre, (unsigned)sg_mem.post, (unsigned)len);
        sg_canary_hit = TRUE;
        return -1;
    }
    if ((int32_t)len == rd) {
        return 0;
    }
    PR_ERR(SDT_TAG " 短读：请求 %u 字节，实到 %d 字节", (unsigned)len, (int)rd);
    return -1;
}

/***********************************************************
*********************** 三个阶段 ****************************
***********************************************************/
/* 1) 顺序大块读 —— 正常播放的样子。从头往后读，不 seek。 */
static int prv_phase_seq(TUYA_FILE f)
{
    uint32_t done = 0;
    uint32_t t0   = (uint32_t)tal_system_get_millisecond();

    if (0 != tkl_fseek(f, 0, SEEK_SET)) {
        PR_ERR(SDT_TAG " 顺序阶段：定位到 0 失败");
        return -1;
    }
    while (done < SDT_SEQ_PER_ROUND) {
        uint32_t want = SDT_SEQ_PER_ROUND - done;

        if (want > SDT_SEQ_CHUNK) {
            want = SDT_SEQ_CHUNK;
        }
        sg_seq.n++;
        if (0 != prv_read(f, want)) {
            sg_seq.bad++;
            sg_seq.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
        done += want;
    }
    sg_seq.ms += (uint32_t)tal_system_get_millisecond() - t0;
    return 0;
}

/* 1b) 顺序小块读 —— 单扇区，但地址**连续**。
 *
 * 用来把"块小"和"地址跳"两个因素分开：单扇区本身不会让驱动中断多块读
 * （sd_card_check_continious_rw 看的是地址接不接得上），所以这一轮过、
 * 随机小块挂 ⇒ 病在"跳地址"（CMD12 中断 + 重启），不在块大小。 */
static int prv_phase_seq_small(TUYA_FILE f)
{
    uint32_t t0 = (uint32_t)tal_system_get_millisecond();
    uint32_t i;

    if (0 != tkl_fseek(f, 0, SEEK_SET)) {
        PR_ERR(SDT_TAG " 顺序小块阶段：定位到 0 失败");
        return -1;
    }
    for (i = 0; i < SDT_SEQ_SMALL_N; i++) {
        sg_seqs.n++;
        if (0 != prv_read(f, SDT_SEQ_SMALL)) {
            sg_seqs.bad++;
            sg_seqs.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
    }
    sg_seqs.ms += (uint32_t)tal_system_get_millisecond() - t0;
    return 0;
}

/* 2) 随机跳读 —— seek 的样子。这是最可疑的一种：随机读吃不到卡内部的预读。 */
static int prv_phase_rand(TUYA_FILE f)
{
    uint32_t t0 = (uint32_t)tal_system_get_millisecond();
    uint32_t seed = 0x1234ABCDu;
    uint32_t i;

    for (i = 0; i < SDT_RAND_N; i++) {
        uint32_t off;

        seed = seed * 1103515245u + 12345u;             /* 简单 LCG，够用 */
        off  = seed % (sg_size - SDT_RAND_CHUNK);

        sg_rand.n++;
        if (0 != tkl_fseek(f, (int64_t)off, SEEK_SET)) {
            PR_ERR(SDT_TAG " 随机阶段：定位到 %u 失败", (unsigned)off);
            sg_rand.bad++;
            sg_rand.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
        if (0 != prv_read(f, SDT_RAND_CHUNK)) {
            sg_rand.bad++;
            sg_rand.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
    }
    sg_rand.ms += (uint32_t)tal_system_get_millisecond() - t0;
    return 0;
}

/* 3) 单扇区读 —— 走 FAT 链的样子。512 字节，随机偏移。 */
static int prv_phase_sector(TUYA_FILE f)
{
    uint32_t t0 = (uint32_t)tal_system_get_millisecond();
    uint32_t seed = 0x5A5A5A5Au;
    uint32_t i;

    for (i = 0; i < SDT_SECTOR_N; i++) {
        uint32_t off;

        seed = seed * 1103515245u + 12345u;
        off  = (seed % ((sg_size - SDT_SECTOR) / SDT_SECTOR)) * SDT_SECTOR;

        sg_sect.n++;
        if (0 != tkl_fseek(f, (int64_t)off, SEEK_SET)) {
            PR_ERR(SDT_TAG " 扇区阶段：定位到 %u 失败", (unsigned)off);
            sg_sect.bad++;
            sg_sect.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
        if (0 != prv_read(f, SDT_SECTOR)) {
            sg_sect.bad++;
            sg_sect.ms += (uint32_t)tal_system_get_millisecond() - t0;
            return -1;
        }
    }
    sg_sect.ms += (uint32_t)tal_system_get_millisecond() - t0;
    return 0;
}

/***********************************************************
*********************** 入口 ********************************
***********************************************************/
void app_sd_test_run(void)
{
    TUYA_FILE f;
    uint32_t  round;
    uint32_t  t_all;

    PR_NOTICE(SDT_TAG " ====== 纯 SD 读压力测试开始（%u 轮）======",
              (unsigned)APP_SD_TEST_ROUNDS);

    if (!prv_pick_file()) {
        return;
    }

    f = tkl_fopen(sg_path, "rb");
    if (NULL == f) {
        PR_ERR(SDT_TAG " 打不开 %s，测试跳过", sg_path);
        return;
    }

    memset(&sg_seq,  0, sizeof(sg_seq));
    memset(&sg_seqs, 0, sizeof(sg_seqs));
    memset(&sg_rand, 0, sizeof(sg_rand));
    memset(&sg_sect, 0, sizeof(sg_sect));
    sg_mem.pre    = SDT_CANARY;             /* 填缓冲区两端的 canary */
    sg_mem.post   = SDT_CANARY;
    sg_canary_hit = FALSE;
    t_all = (uint32_t)tal_system_get_millisecond();

    /* 阶段顺序是**刻意**排的：从"最不可能出问题"到"最可疑"，每阶段开跑前
     * 单独打一条日志。上一版把随机大块排在随机小块前面，结果两次都死在大块上，
     * 小块那轮根本没跑到 —— 就没法区分"块小"和"地址跳"两个因素。 */
    for (round = 0; round < APP_SD_TEST_ROUNDS; round++) {
        static const char *names[4] = { "顺序大块32KB", "顺序小块512B",
                                        "随机小块512B", "随机大块20KB" };
        int rt;

        rt = 0;
        for (uint32_t ph = 0; ph < 4u; ph++) {
            PR_NOTICE(SDT_TAG " 第 %u 轮 · 阶段 %u/4：%s",
                      (unsigned)(round + 1u), (unsigned)(ph + 1u), names[ph]);

            switch (ph) {
            case 0: rt = prv_phase_seq(f);       break;
            case 1: rt = prv_phase_seq_small(f); break;
            case 2: rt = prv_phase_sector(f);    break;
            default: rt = prv_phase_rand(f);     break;
            }
            if (0 != rt) {
                PR_ERR(SDT_TAG " ****** 挂在「%s」******", names[ph]);
                break;
            }
        }

        PR_NOTICE(SDT_TAG " 第 %u/%u 轮完成",
                  (unsigned)(round + 1u), (unsigned)APP_SD_TEST_ROUNDS);
        PR_NOTICE(SDT_TAG "   顺序大块 %u 次 / %u ms",
                  (unsigned)sg_seq.n,  (unsigned)sg_seq.ms);
        PR_NOTICE(SDT_TAG "   顺序小块 %u 次 / %u ms",
                  (unsigned)sg_seqs.n, (unsigned)sg_seqs.ms);
        PR_NOTICE(SDT_TAG "   随机小块 %u 次 / %u ms",
                  (unsigned)sg_sect.n, (unsigned)sg_sect.ms);
        PR_NOTICE(SDT_TAG "   随机大块 %u 次 / %u ms",
                  (unsigned)sg_rand.n, (unsigned)sg_rand.ms);

        if (0 != rt) {
            PR_ERR(SDT_TAG " ****** 第 %u 轮出错，测试停止 ******", (unsigned)(round + 1u));
            break;
        }
    }

    PR_NOTICE(SDT_TAG " ====== 测试结束，共 %u ms ======",
              (unsigned)((uint32_t)tal_system_get_millisecond() - t_all));
    PR_NOTICE(SDT_TAG " 顺序大块 %u 次（坏 %u）",
              (unsigned)sg_seq.n,  (unsigned)sg_seq.bad);
    PR_NOTICE(SDT_TAG " 顺序小块 %u 次（坏 %u）",
              (unsigned)sg_seqs.n, (unsigned)sg_seqs.bad);
    PR_NOTICE(SDT_TAG " 随机小块 %u 次（坏 %u）",
              (unsigned)sg_sect.n, (unsigned)sg_sect.bad);
    PR_NOTICE(SDT_TAG " 随机大块 %u 次（坏 %u）",
              (unsigned)sg_rand.n, (unsigned)sg_rand.bad);
    PR_NOTICE(SDT_TAG " 缓冲区 canary：%s",
              sg_canary_hit ? "**被踩过**（DMA 越界写）" : "完好");

    tkl_fclose(f);
}

#else   /* APP_SD_TEST == 0：整段不参与编译（32KB 缓冲也不进 .bss） */

void app_sd_test_run(void)
{
}

#endif  /* APP_SD_TEST */
