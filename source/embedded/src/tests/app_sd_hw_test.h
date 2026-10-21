/**
 * @file app_sd_hw_test.h
 * @brief SD 卡**硬件读（GDMA）**驱动测试的开关与入口。
 *
 * 和 app_sd_test.c 的区别：那个走 FatFs（文件层），这个直接打**块层**
 * `bk_sd_card_read_blocks()` —— 不挂文件系统、不碰业务代码，只测驱动本身。
 * 目的只有一个：确认读通路到底能不能用硬件（GDMA），还是必须留在 CPU 轮询。
 *
 * 打开之后 **整个应用都不启动**（见 tuya_app_main.c）：跑完测试就停在测试里，
 * 显示 / 网络 / 字库 / 精灵 / AI 一个都不起来。
 */

#ifndef __APP_SD_HW_TEST_H__
#define __APP_SD_HW_TEST_H__

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = 开机只跑 SD 硬件读测试，**不启动任何业务代码**；0 = 不跑（正常启动）。
 *
 * 2026-10-21 结论：GDMA 可用（前提是单次传输 ≤64KB，已在
 * `bk_sd_card_read_blocks()` 里统一拆分）。测试代码留着，下次换卡/换板再开。 */
#define APP_SD_HW_TEST   0

/**
 * @brief 跑 SD 读通路 A/B 测试（CPU 轮询 vs GDMA）。**不返回。**
 *
 * 自己起卡（4 线 @13M，和产品一致），然后用同一批用例分别跑两条通路，
 * 打印耗时 / 带宽 / 短读 / canary 越界 / 数据一致性，最后给结论并停住。
 */
void app_sd_hw_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SD_HW_TEST_H__ */
