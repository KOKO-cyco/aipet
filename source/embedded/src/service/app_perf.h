/**
 * @file app_perf.h
 * @brief 串口性能诊断（CPU 占用 / 任务列表）。
 *
 * 为什么要自己起一个自动采样线程：串口命令行在本机上不一定敲得响
 * （CLI 线程优先级低，整机被挤爆时可能几秒才轮到它；而且那个 shell 的
 * 提示符会被持续不断的日志淹没，看不出到底进没进去）。所以这里改成
 * **开机后自动采 6 次**，用户什么都不用敲，直接从日志里读。
 *
 * 采样内容 = 每个线程自开机以来的 CPU 占用百分比（bk 的
 * rtos_dump_task_runtime_stats，内部是 FreeRTOS 的运行时统计，
 * configGENERATE_RUN_TIME_STATS=1 已开）。
 *
 * 命令行仍然保留（万一能敲响）：
 *   cpuload            采一次
 *   tasklist           任务列表（名字/优先级/剩余栈）
 *   perf now|off|<秒>  立即采样 / 关掉自动采样 / 改自动周期并重开 6 次
 *
 * 注意：采样内部会短暂关中断做统计（任务数 ~30，几毫秒），所以采样那一
 * 拍本身可能掉帧 —— 看数据时把这一下排除掉，别当成问题。
 */
#ifndef APP_PERF_H
#define APP_PERF_H

#include "tuya_cloud_types.h"

/**
 * @brief 注册诊断命令并启动自动采样线程。
 * @note  只会生效一次，重复调用无副作用。
 */
void app_perf_init(void);

#endif /* APP_PERF_H */
