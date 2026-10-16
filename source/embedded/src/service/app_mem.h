/**
 * @file app_mem.h
 * @brief 统一内存账本：一行打齐 SRAM / SRAM 历史最低 / PSRAM / PSRAM 最大连续块。
 *
 * 为什么要有它
 * ------------
 * 之前每个模块各打各的（`[pool]` / `[font]` / `[mus]` / `[spk]` / `[nes]`），格式不一、
 * 时机不一，想回答"现在还剩多少"要翻五个文件。而且有两个更根本的缺口：
 *
 *   1. **测量点几乎全在 PSRAM，而真正紧的是内部 SRAM** —— 开机日志 `left sram:
 *      144176`，只剩 140KB；PSRAM 空闲 6330KB（字库再吃 2542KB 还剩 ~3.8MB）。
 *   2. **没有"最大连续块"这个指标** —— PSRAM 空闲 3.8MB 不等于"能分配出 300KB 连续
 *      块"。`app_sprite2d.c` 已经踩过这个坑：idle 组分配失败（PSRAM 碎片）时整组
 *      改走流式读，帧率掉了但没人知道为什么。
 *
 * 用法
 * ----
 * 在**关键节点**各打一条（开机各阶段 / AI 栈拉起与释放 / 精灵加载），然后对比
 * "进出前后的数值是否回到同一基线" —— 这就是"占用不漂移"的验收点。
 * 沿用老产品 `app_pool_log()` 已验证的思路（那个池子随电影/音乐页一起删了）。
 *
 * 输出格式固定，方便 grep 与逐行对比：
 * @code
 * [mem] movie exit: sram=118KB(min 96KB) psram=3812KB(largest 512KB)
 * @endcode
 */

#ifndef __APP_MEM_H__
#define __APP_MEM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打一行内存账本，带场景标签。
 *
 * @param[in] tag 场景标签，例如 "movie exit"；NULL 用 "-"。
 *
 * @warning **别放进热路径**。内部会做"最大连续块"探测（真去 malloc/free），
 *          几十到几百微秒量级。只在进页/退页/开关这类一次性节点调用。
 *
 * 最大连续块低于阈值时自动升级成 `PR_WARN`，一眼能看出碎片。
 */
void app_mem_stat(const char *tag);

/**
 * @brief 只报内部 SRAM（当前空闲 + 历史最低），**不探 PSRAM 连续块**。
 *
 * 启动流程里要密集打点（网络 / 显示 / 音频 / LVGL 各一条），`app_mem_stat()` 的
 * 探针每次会真去试 ~6 次 `tal_psram_malloc()`，失败的还会让 SDK 打红色 ERR，
 * 放这儿只会把日志淹掉。需要看 PSRAM 时再用 `app_mem_stat()`。
 *
 * 输出格式：
 * @code
 * [mem] net up: sram=106KB(min 106KB)
 * @endcode
 */
void app_mem_sram(const char *tag);

/**
 * @brief 探测当前 PSRAM 能分配出的**最大连续块**（字节）。
 *
 * 二分探测：从 1MB 往下试 `tal_psram_malloc()`，成功就立刻 `free`，不占住内存。
 * 分辨率 64KB（与池子 64 字节对齐同量级，够看趋势）。
 *
 * 这是碎片唯一的直接指标 —— 空闲总量看不出来，`app_sprite2d` 的 idle 组分配失败
 * 就是被它卡住的。
 */
uint32_t app_mem_psram_largest(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MEM_H__ */
