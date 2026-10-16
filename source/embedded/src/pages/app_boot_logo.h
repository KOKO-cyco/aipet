/**
 * @file app_boot_logo.h
 * @brief SD 状态页上的那张小图 —— **编进固件的常驻位图**。
 *
 * 为什么不放 SD 卡上现读
 * ----------------------
 * 这张图出现的时机恰恰是 SD 读不了的时候：
 *
 *   请插入 SD 卡      卡被拔了
 *   正在识别存储卡…   卡刚插回来，SDIO 链路还是死的，正在重建
 *
 * 所以它必须**先于等待就在内存里**。两条路：编进 flash，或开机读一次进 PSRAM。
 * 这张图很小、内容也固定，编进 flash 最省事 —— 连开机那次 SD 访问都省了。
 *
 * 怎么生成
 * --------
 * 把图丢给 `tools/img_to_lvgl.py`（PNG / JPEG 都吃），它会**整个覆盖**
 * `app_boot_logo.c`：
 *
 *     python tools/img_to_lvgl.py third_party/logo.png --max 100 --trim --bg "#9393C2"
 *
 * 出来的是一段 RGB565 常量数组 + 描述符。**注意 RGB565 没有 alpha 通道**：源图
 * 带透明的话，透明像素会按 `--bg` 指定的底色合成掉，所以底色要跟页面底色一致
 * （开机 splash 的底色，见 app_splash.c 的 SPLASH_BG）。
 *
 * 尺寸按"长边上限"给：**一个像素 2 字节**，当前这张是 100x100 = 19.5KB flash。
 * app 分区只剩约 800KB，别超 150x150。源图换成别的比例也可以，脚本会等比缩。
 *
 * @note 还没有图的时候 `app_boot_logo_dsc()` 返回 NULL，页面只显示文字，功能不受影响。
 */

#ifndef __APP_BOOT_LOGO_H__
#define __APP_BOOT_LOGO_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 那张图的描述符。**没有图时返回 NULL**，调用方必须判空。
 *
 * 返回值指向 flash 里的常量数据，生存期是全程，不需要释放。
 */
const lv_image_dsc_t *app_boot_logo_dsc(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_BOOT_LOGO_H__ */
