/**
 * @file app_camera.h
 * @brief 摄像头服务：实时预览 + 拍照存卡（GC2145 / DVP）。
 *
 * 数据流
 * ------
 *   摄像头 --YUV422--> 相机线程里转成 320x240 RGB565（双缓冲）
 *                              |
 *                              v
 *   拍照页的取景框 <--cover采样-- 最新一帧
 *
 *   快门 --请求--> 下一张完整 JPEG（相机线程拷贝到 PSRAM）--> 主线程写 /sdcard/pictures
 *
 * 为什么预览和拍照用两路
 * ----------------------
 * `TDL_CAMERA_FMT_JPEG_YUV422_BOTH` 同时给两路回调：
 *   - YUV422 自己转 RGB565 给 LVGL 画（不依赖 JPEG 解码器 / 文件系统驱动）；
 *   - JPEG 直接存卡（体积小，手机/电脑都能看）。
 *
 * 线程
 * ----
 * 帧回调跑在**相机自己的线程**里，只做内存转换/拷贝，绝不做 SD 读写。
 * `app_camera_preview()` 在 LVGL 线程里调，读的是双缓冲里**已经完成**的那一份。
 * `app_camera_capture()` 会阻塞到 JPEG 到手 + 写完卡（约 30~150ms），
 * 在按键回调里调是可以接受的；不要在别的耗时路径上调。
 */

#ifndef __APP_CAMERA_H__
#define __APP_CAMERA_H__

#include <stddef.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 预览源图尺寸：**竖屏**（摄像头模组是转 90° 装的，见 APP_CAM_ROTATE_CW）。
 *  320×240 经 ROTT 旋转后是 240×320；取景框 236×264 再做一次 cover。
 *  双缓冲 2 × 240×320×2 = 300KB PSRAM，另有一块 320×240 的旋转暂存 150KB。 */
#define APP_CAM_PREVIEW_W   240
#define APP_CAM_PREVIEW_H   320

/** 1 = 预览顺时针转 90°；0 = 逆时针。摄像头模组相对屏幕是转着装的，
 *  画面不转就是横躺的。实测当前模组要顺时针（1）。 */
#define APP_CAM_ROTATE_CW   1

/** 向摄像头请求的分辨率 / 帧率。
 *
 * **320×240（临时性能测试）**：视野是裁切的（GC2145 out_win 只裁不缩），
 * 这里只用它来代表“未来 OV2640 全视野 320×240”的数据量，验证 LVGL/桌宠
 * 性能能否达标。达标后再换 OV2640；不达标就不用买。
 *
 * **15fps**：320×240 没有单独帧率表，沿用默认帧长度（约 15fps）。 */
#define APP_CAM_WANT_W      320
#define APP_CAM_WANT_H      240
#define APP_CAM_FPS         15

/** 存图目录（APP_STORAGE_MOUNT 下面）。 */
#define APP_CAM_PIC_DIR     "pictures"

/**
 * @brief 打开摄像头并开始出帧（幂等）。
 *
 * @return OPRT_OK 成功；找不到设备 / 开流失败 / 内存不足时返回错误，调用方退回静态预览即可。
 */
OPERATE_RET app_camera_open(void);

/** @brief 关流（幂等）。
 *
 * 预览缓冲（约 700KB）**不释放**，下次打开直接复用 —— 帧回调可能还在相机线程
 * 里跑最后一帧，在这里释放会造成 use-after-free（按 P12 退出拍照页崩过）。 */
void app_camera_close(void);

/** @brief 是否已经打开。 */
BOOL_T app_camera_is_open(void);

/** @brief 预览帧代数：每收到一帧 +1，UI 用它判断要不要重画。 */
uint32_t app_camera_gen(void);

/**
 * @brief 把最新一帧按 fit 方式画到 @a dst（取景框自己的尺寸）。
 *
 * 源图已经是转好 90° 的竖图（240×320）。取景框比例不同时完整保留源图、
 * 两侧补底色，不做 cover 裁边 —— 这样预览看到的就是整幅画面，和存图一致。
 *
 * @return TRUE = 画了（有帧）；FALSE = 还没出帧 / 参数非法。
 * @note 只能在 LVGL 线程里调。
 */
BOOL_T app_camera_preview(uint16_t *dst, int w, int h);

/**
 * @brief 抓一张照片到内存（**不写卡**）。
 *
 * 同步：关预览流 → 临时开 YUV+JPEG 双路 → 等一张完整 JPEG（≤2s）→ 关流 →
 * 切回 YUV 预览。整个过程约 0.5~1 秒，期间界面会停一下。
 *
 * 成功后 JPEG 一直留在内存里，直到 `app_camera_save()` 或 `app_camera_discard()`
 * —— 快门的语义是"先拍，用户在确认卡上点保存再落盘"。
 *
 * @return TRUE = 内存里已经有一张照片；FALSE = 没开流 / 等帧超时 / 分配失败。
 */
BOOL_T app_camera_grab(void);

/**
 * @brief 把 `app_camera_grab()` 抓到的照片写进 /sdcard/pictures。
 *
 * @param[out] path     存下来的完整路径（失败时为空串）。可以为 NULL。
 * @param[in]  path_sz  @a path 的容量。
 * @return TRUE = 已保存；FALSE = 没有待保存的照片 / 卡打不开。
 */
BOOL_T app_camera_save(char *path, size_t path_sz);

/** @brief 丢掉抓到的照片（用户关掉确认卡 / 离开页面时调）。 */
void app_camera_discard(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAMERA_H__ */
