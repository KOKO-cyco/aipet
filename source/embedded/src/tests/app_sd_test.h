/**
 * @file app_sd_test.h
 * @brief 纯 SD 读压力测试的开关与入口。
 *
 * 排查"播放一会儿就出 -6722"时用：把视频解码 / 音频 / LVGL / WiFi 全摘掉，
 * 只读卡，看 SD 通路本身会不会崩。结论拿到后把 APP_SD_TEST 改回 0，
 * 测试代码留在树里不影响构建。
 */

#ifndef __APP_SD_TEST_H__
#define __APP_SD_TEST_H__

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = 开机先跑 SD 读压力测试（阻塞，跑完才继续启动）；0 = 不跑。
 *
 * 位置在 app_storage_sdio_setup() 之后、app_disp_init() 之前（见 tuya_app_main.c）：
 * 那一刻板级外设已注册（SDIO 引脚映射好了），而字体 / 预热 / 音乐都还没起来，
 * **整个系统只有这一个读者** —— 能把"SD 通路本身"和"被别的负载带出来"分开。 */
#define APP_SD_TEST     0

/** 跑多少轮。一轮 = 顺序 8MB + 随机读 200 次 + 单扇区读 400 次。
 *  第一次出错就停（一次 -6722 要冻 5.12 秒，继续跑只会把日志冲烂）。 */
#define APP_SD_TEST_ROUNDS  10

/**
 * @brief 跑 SD 读压力测试。阻塞，直到轮次跑完或第一次出错。
 *
 * 自己会挂载 SD（走 app_storage_init()），找不到合适的测试文件就只打日志返回。
 */
void app_sd_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SD_TEST_H__ */
