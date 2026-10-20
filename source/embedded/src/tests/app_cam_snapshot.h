/**
 * @file app_cam_snapshot.h
 * @brief 摄像头快照测试：开机拍一张 -> 存 /sdcard/pictures -> 给拍照页取景框用。
 *
 * 和 `app_cam_test.c` 的区别
 * --------------------------
 * 那个是"只有摄像头"的阻塞式 bring-up（关屏、拍完停住、只看串口）。这个**不
 * 阻塞启动**：后台线程睡两秒再拍，拍完把 RGB565 留在 PSRAM 里给 UI 用，同时把
 * JPEG 写进卡里的 pictures/。桌宠页照常启动。
 *
 * 为什么同时要 YUV 和 JPEG
 * ------------------------
 *   YUV422  : 自己转 RGB565 给 LVGL 画（不依赖 JPEG 解码器，也不需要文件系统驱动）
 *   JPEG    : 存卡（体积小，别的机器/手机能直接看）
 *
 * 内存：转换缓冲 480x320 RGB565 = 300KB（拍完常驻，拍照页要用）；JPEG 暂存 256KB
 * （写完卡就释放）。
 *
 * 开关：`APP_CAM_SNAPSHOT`。测试完改 0 即可，代码留在树里不影响构建。
 */

#ifndef __APP_CAM_SNAPSHOT_H__
#define __APP_CAM_SNAPSHOT_H__

#include "tuya_cloud_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = 开机后台拍一张并接到拍照页；0 = 什么都不做。
 *
 * **2026-10-20 起默认 0**：拍照页已经换成实时预览（src/modules/app_camera.c），
 * 开机快照这条测试路径不再需要。留着代码是因为它验证过"YUV 转换 + 存卡"这条
 * 链路，以后排查摄像头还能单独打开用。 */
#ifndef APP_CAM_SNAPSHOT
#define APP_CAM_SNAPSHOT    0
#endif

/** 快照源图的固定尺寸（横屏 480x320 的 3:2 cover 图）。 */
#define CAM_SNAP_W          480
#define CAM_SNAP_H          320

#if APP_CAM_SNAPSHOT

/**
 * @brief 起一次性后台线程：等界面稳定 -> 拍一张 -> 存卡 -> 置 ready。
 *
 * 由 `tuya_app_main.c` 在 SD 就绪、首页上屏之后调用一次。线程拍完自退；
 * 失败只打日志，不影响任何功能。
 */
void app_cam_snapshot_start(void);

/** @brief 快照是否已经就绪（可以往取景框里画了）。 */
BOOL_T app_cam_snapshot_ready(void);

/**
 * @brief 快照的代数：每成功拍一张 +1。UI 用它判断"要不要重画"。
 */
uint32_t app_cam_snapshot_gen(void);

/**
 * @brief 把快照按 cover 方式采到 @a dst（任意宽高，取景框自己的尺寸）。
 *
 * 源图固定 480x320；目标比例不同就居中裁掉长边，不拉伸。
 *
 * @return TRUE = 画了；FALSE = 还没有快照 / 参数非法。
 * @note 只能在 LVGL 线程里调（读的是常驻 PSRAM，无锁；写只发生在线程启动那一次）。
 */
BOOL_T app_cam_snapshot_blit(uint16_t *dst, int w, int h);

/** @brief 释放快照缓冲（页面退出 / 测试结束时可选调用）。 */
void app_cam_snapshot_release(void);

#else /* APP_CAM_SNAPSHOT == 0 */

static inline void     app_cam_snapshot_start(void) {}
static inline BOOL_T   app_cam_snapshot_ready(void) { return FALSE; }
static inline uint32_t app_cam_snapshot_gen(void) { return 0; }
static inline BOOL_T   app_cam_snapshot_blit(uint16_t *dst, int w, int h)
{
    (void)dst; (void)w; (void)h;
    return FALSE;
}
static inline void     app_cam_snapshot_release(void) {}

#endif /* APP_CAM_SNAPSHOT */

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAM_SNAPSHOT_H__ */
