/**
 * @file app_splash.h
 * @brief 开机 splash：主题底色 + 居中 logo，系统准备好之前一直显示。
 *
 * 时序（见 tuya_app_main.c）
 * ------------------------
 *     app_disp_init()          面板挂上 LVGL（还没开始渲染）
 *     app_splash_show()        建好 splash 并上屏
 *     app_disp_start()         LVGL 开始渲染 —— 用户看到的第一帧就是 logo
 *     ... 等 SD 就绪（最多 10 秒，见 app_storage_wait_ready）...
 *     app_router_prepare()     首页建好并上屏
 *     app_splash_hide()        拆掉 splash
 *
 * 为什么要"顶在前面"
 * ----------------
 * 原来的做法是首页建好之后才 `app_disp_start()`，所以开机第一帧就是首页 —— 没有
 * 开机画面可言。现在把首页的建立推后到 SD 就绪之后，中间那段（等 SD 挂载、链路
 * 就绪、三个目录索引预热完，约 6 秒）用 logo 顶着，视觉上就是"开机画面"。
 *
 * **图必须编进 flash**（见 app_boot_logo.h）：这段时间 SD 正在被识别，读不了卡。
 *
 * @note 浮层小精灵要等 splash 退场之后再开 —— 否则角色会画在 logo 上面，
 *       而这段时间屏幕上"应该什么也没有"。
 */

#ifndef __APP_SPLASH_H__
#define __APP_SPLASH_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建 splash 并上屏。幂等。
 *
 * @return OPRT_OK 已上屏；OPRT_COM_ERROR 没有 logo 素材 —— 此时**什么都不做**，
 *         让开机保持黑屏。铺一块纯白再配上"什么都没有"比黑屏更像坏了。
 */
OPERATE_RET app_splash_show(void);

/** @brief 拆掉 splash（首页已经上屏了）。幂等。 */
void app_splash_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SPLASH_H__ */
