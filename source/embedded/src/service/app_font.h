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
 * 本模块把**完整字库**放到 SD 卡（/sdcard/font/puhui14.bin … puhui32.bin），
 * 开机后台读进 PSRAM，做成地址恒定的**代理字体**。新 UI（src/ui）用六个字号：
 * 14/18 是 Regular 正文（全 GB2312 + ASCII），16/20/24 是 Medium chrome，
 * 32 是 SemiBold 数字。旧的两份 cn16.bin / cn21.bin **开机不再加载**
 * （全产品中文都走 puhui14/18 了，那两份完整字库是 2.4MB PSRAM）；要它们时用
 * app_font_load_sync() 单独加载。
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
 * 这里维护六份**RAM 里的代理字体**（地址固定）。UI 建对象时存进 style 的是
 * 代理的地址；SD 字库到了之后把整份 lv_font_t 原地 memcpy 进去，地址不变，
 * 所有已经建好的界面立刻用上新字体。之后发 APP_EVT_FONT_READY，页面据此重建
 * 一次（字号变了，量过的标签尺寸要重排）。
 *
 * 加载时机
 * --------
 * app_font_load_async() 起一个低优先级后台任务：先睡 1.5 秒让开机流程跑完（避免
 * 和开机时的 SD 卡访问抢），再依次加载六档。每档单独成败，缺的就留内置占位；
 * 卡不在 / 文件缺失都只是打日志，界面退回内置子集，功能不受影响。
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
 * @brief 新 UI（third_party/ai_pet_wide 移植）的六个字号，**全部从 SD 卡加载**。
 *
 * 文件名是 `/sdcard/font/puhui<px>.bin`，px ∈ {14,16,18,20,24,32}。demo 里这
 * 六个字号是 C 源，合计 7.5MB —— 比 app 分区还大，所以必须放 SD。
 *
 * 生成（lv_font_conv，`--format bin`）。字重跟 demo 一致：
 *   - **14 / 18**：Regular，正文和工作字号，要**全 GB2312 + ASCII**（模型/用户
 *     带来的文本都可能是这两个字号）。
 *   - **16 / 20 / 24**：Medium，卡片标题 / 页面标题 / 大标题。
 *   - **32**：SemiBold，卡片里的大数字。
 *   - 16/20/24/32 都只出现界面自己写死的字，**建议做子集** —— 具体码点从 demo
 *     的字表里提取好了，见 `tools/puhui_subset.txt`（每个字号一段
 *     `lv_font_conv -r` 用的 range，外加四档并集）。全字库四份会吃掉 3MB+ PSRAM。
 *
 * 缺哪份就退到内置字体（14/16/18 用 16px，20/24/32 用 21px），界面不崩，只是
 * 那几处字号不对。**加载是后台的**：UI 先建出来，字库到了之后代理字体原地换掉，
 * 已建界面自动重绘 —— 调用方在 style 里存的地址永远有效。
 */
#define APP_FONT_PUHUI_14   "/sdcard/font/puhui14.bin"
#define APP_FONT_PUHUI_16   "/sdcard/font/puhui16.bin"
#define APP_FONT_PUHUI_18   "/sdcard/font/puhui18.bin"
#define APP_FONT_PUHUI_20   "/sdcard/font/puhui20.bin"
#define APP_FONT_PUHUI_24   "/sdcard/font/puhui24.bin"
#define APP_FONT_PUHUI_32   "/sdcard/font/puhui32.bin"

/**
 * @brief 取某个字号的代理字体。地址恒定，可以安全地存进 style。
 *
 * @param[in] px 14/16/18/20/24/32；其它值取最接近的一档。
 * @return 永不为 NULL。SD 字库还没加载 / 没有这一档时是内置字体。
 */
const lv_font_t *app_font_puhui(int px);

/**
 * @brief 旧的正文接口 —— 现在等价于 app_font_puhui(14)。
 *
 * 保留是为了不让老页面源码改动；cn16/cn21 不再开机加载（见文件头）。
 */
const lv_font_t *app_font_cn(void);

/** @brief 旧的大号接口 —— 现在等价于 app_font_puhui(20)。 */
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

/** @brief SD 字库是否已经生效（puhui14 / puhui18 两份都加载成功才算）。 */
BOOL_T app_font_ready(void);

/** @brief 打印当前占用（加载了几档、PSRAM 字节数、剩余）。 */
void app_font_stat(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_FONT_H__ */
