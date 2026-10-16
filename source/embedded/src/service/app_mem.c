/**
 * @file app_mem.c
 * @brief 统一内存账本实现。设计说明见 app_mem.h。
 */

#include "tal_api.h"
#include "tal_system.h"

#include "app_mem.h"

/* FreeRTOS 堆的历史最低水位。声明在 freertos/portable.h 里，那个头不在 app 的
 * include 路径上（app_nes.cpp 同样是自己 extern 的），所以这里只声明用到的符号。
 * 注意：这是"从开机到现在的最低点"，不是"当前"—— 它的用途是看**曾经**被压到多低，
 * 判断还有多少余量。 */
extern size_t xPortGetMinimumEverFreeHeapSize(void);

/* 最大连续块的探测上界与分辨率。
 * 上界取 8MB（比 PSRAM 总量还大一点）：**不能取 1MB** —— 真机上"最大连续块"
 * 经常就在 1MB 附近，探到上限就会稳定输出 960KB 这种"假精确"的数字，
 * 看着像没变化，其实底下在变。取到比总量还大，失败的那次 malloc 会立刻返回
 * （heap_4 找不到够大的空闲块），代价只有几十微秒。
 * 分辨率 64KB 与池子的对齐粒度同量级，够看趋势。 */
#define MEM_PROBE_MAX       (8u * 1024u * 1024u)
#define MEM_PROBE_STEP      (64u * 1024u)

/* 低于这个"最大连续块"就值得警惕：池子里最大的单块是 300KB 的 framebuffer，
 * 后续阶段（浮层精灵常驻、大精灵）还要再要几块。 */
#define MEM_LARGEST_WARN    (256u * 1024u)

uint32_t app_mem_psram_largest(void)
{
    uint32_t lo = 0;
    /* 上界取"当前空闲量"而不是写死的 8MB：探针每失败一次，SDK 的
     * tal_psram_malloc 就会打一条红色 ERR（`0x... psram malloc failed`）。
     * 从空闲量起步，就不会再有那些注定失败的大块尝试，日志干净得多 ——
     * 免得排查时把探针自己的噪声当成真故障。 */
    uint32_t hi = (uint32_t)tal_psram_get_free_heap_size();

    if (hi > MEM_PROBE_MAX) {
        hi = MEM_PROBE_MAX;
    }

    /* **先打一条，把下面那几条红字认领了。**
     *
     * 二分找上界的过程**必然**要 malloc 失败几次（失败才知道上界在哪），而
     * `tal_psram_malloc` 失败时无条件打一条 `psram malloc failed` 的 ERR ——
     * 那是 SDK 打的，级别和内容都改不了。
     *
     * 结果就是：每次进出全屏页（电影/NES/角色），日志里都会冒出**几条连着的、
     * 尺寸递减的** psram 分配失败。2026-10-13 我照着这个查了半天"谁在要 2MB"，
     * 最后顺着 `__builtin_return_address` 查 map 才发现指向的就是本函数。
     *
     * 所以在这里认领一下：**看到"尺寸递减的连续几条 + 紧跟一条 [mem] 汇总"就是
     * 探针在干活，不是故障。** 真故障长这样：**孤零零一条**、尺寸不递减、
     * 而且后面没有 [mem] 汇总。 */
    PR_DEBUG("[mem] 探测最大连续块（下面几条 psram malloc failed 是探针故意试出来的）");

    /* 二分找"能分配"的上界。每步 malloc 完立刻 free，不会真占住内存 ——
     * 这是唯一能直接量出碎片的办法（空闲总量看不出来）。 */
    while ((hi - lo) > MEM_PROBE_STEP) {
        uint32_t mid = (lo + hi) / 2u;
        void    *p   = tal_psram_malloc(mid);

        if (NULL != p) {
            tal_psram_free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void app_mem_sram(const char *tag)
{
    uint32_t    sram_b  = (uint32_t)tal_system_get_free_heap_size();
    uint32_t    sram_lo = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    const char *t       = (NULL != tag) ? tag : "-";

    PR_NOTICE("[mem] %s: sram=%uKB(min %uKB)",
              t, (unsigned)(sram_b / 1024u), (unsigned)(sram_lo / 1024u));
}

void app_mem_stat(const char *tag)
{
    uint32_t    sram_b  = (uint32_t)tal_system_get_free_heap_size();
    uint32_t    sram_lo = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    uint32_t    psram_b = (uint32_t)tal_psram_get_free_heap_size();
    uint32_t    largest = app_mem_psram_largest();
    const char *t       = (NULL != tag) ? tag : "-";

    if (largest < MEM_LARGEST_WARN) {
        /* 碎片预警：空闲总量可能还很多，但已经拼不出一块像样的连续内存了。 */
        PR_WARN("[mem] %s: sram=%uKB(min %uKB) psram=%uKB(最大连续块仅 %uKB)"
                " —— 连续块偏小，注意碎片",
                t, (unsigned)(sram_b / 1024u), (unsigned)(sram_lo / 1024u),
                (unsigned)(psram_b / 1024u), (unsigned)(largest / 1024u));
        return;
    }

    PR_NOTICE("[mem] %s: sram=%uKB(min %uKB) psram=%uKB(largest %uKB)",
              t, (unsigned)(sram_b / 1024u), (unsigned)(sram_lo / 1024u),
              (unsigned)(psram_b / 1024u), (unsigned)(largest / 1024u));
}
