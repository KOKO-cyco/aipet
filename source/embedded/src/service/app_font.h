/**
 * @file app_font.h
 * @brief SD 卡中文字库：开机后台加载，加载完成后自动接上，UI 代码一行都不用改。
 *
 * 解决什么问题
 * ------------
 * 固件里编译的那两份字体是 GUI Guider 生成的手挑子集 —— 16px 只有 243 个码点、
 * 21px 185 个。任何没挑中的字（比如"音乐"的"乐"）都渲染成空白，而每次改文案都要
 * 重新生成字体、重新烧固件。
 *
 * 本模块把**完整字库**放到 SD 卡（/sdcard/font/cn16.bin、cn21.bin），开机后台读
 * 进 PSRAM，做成内置字体的 fallback：内置字库查不到的码点自动去 SD 字库找。
 *
 * 为什么不用 LVGL 自带的 lv_binfont_create()
 * ------------------------------------------
 * 两个硬障碍，都是看源码确认过的：
 *   1) SDK 的 lv_conf.h 里 LV_MEM_CUSTOM=1，lv_malloc 就是 os_malloc（SRAM）。
 *      而 lv_binfont_loader.c 会把整个字形位图 lv_malloc 出来 —— 2MB 级的字库
 *      SRAM 里放不下。
 *   2) 同一份 lv_conf.h 里 LV_USE_FS_* 全是 0，port/lv_port_fs.c 还是 `#if 0`
 *      的空模板，没有能用的文件系统驱动。
 * 所以这里是自己实现的解析：大块一律 tal_psram_malloc，lv_malloc 一次都不用。
 *
 * 怎么做到"UI 代码不用改"
 * ------------------------
 * 内置字体是 `const`，编译进 rodata（flash），**改不了它的 fallback 字段**
 * （写了也没用，flash 是只读的）。所以这里维护两个**RAM 里的代理字体**：
 *
 *   app_font_cn()  ->  固定地址的一份 lv_font_t 副本（内容 = 内置 16px 字体）
 *   app_font_21()  ->  同上，21px
 *
 * app_theme.h 里 UI_FONT_CN / UI_FONT_21 直接展开成这两个函数。UI 创建对象时
 * 存进 style 的是**代理的地址**，而这个地址永远不变 —— 所以加载完成后只要改
 * 代理的 fallback 字段，所有已经建好的界面立刻生效，不用逐个对象去刷字体。
 *
 * 加载时机
 * --------
 * app_font_load_async() 起一个低优先级后台任务：先睡 1.5 秒让开机流程跑完（避免
 * 和开机时的 SD 卡访问抢），再依次加载两份。全部完成后挂 fallback + invalidate
 * 当前屏。卡不在 / 文件缺失都只是打日志，界面退回内置子集，功能不受影响。
 */

#ifndef __APP_FONT_H__
#define __APP_FONT_H__

#include "tuya_cloud_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡上的字库目录与文件名（lv_font_conv --format bin 生成）。 */
#define APP_FONT_DIR        "/sdcard/font"
#define APP_FONT_FILE_16    "/sdcard/font/cn16.bin"
#define APP_FONT_FILE_21    "/sdcard/font/cn21.bin"

/**
 * @brief 正文 16px 字体（代理）。地址恒定，可放心存进 style。
 *        SD 字库没加载完时就是内置子集本体，加载完后自动带上 fallback。
 */
const lv_font_t *app_font_cn(void);

/** @brief 大号 21px 字体（代理），语义同 app_font_cn()。 */
const lv_font_t *app_font_21(void);

/**
 * @brief 建立两个代理字体。**必须在任何 UI 对象创建之前调用**
 *        （放在 app_pages_register() 之前即可）。幂等。
 */
OPERATE_RET app_font_init(void);

/**
 * @brief 起后台任务从 SD 卡加载字库（失败只打日志，不影响其它功能）。
 *        调用前 SD 卡可以还没挂载 —— 任务自己会调 app_storage_init()（幂等）并按
 *        2 秒一次探测字库文件能不能打开，最多等 2.5 分钟。
 */
OPERATE_RET app_font_load_async(void);

/**
 * @brief 同步加载一份字库（调试 / 测试用）。@a path 为 NULL 时用默认路径。
 * @param[in] px 16 或 21，决定挂到哪个代理上。
 */
OPERATE_RET app_font_load_sync(int px, const char *path);

/** @brief SD 字库是否已经生效（两份都加载成功才算）。 */
BOOL_T app_font_ready(void);

/** @brief 打印当前占用（PSRAM 字节数、字形数、是否生效）。 */
void app_font_stat(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_FONT_H__ */
