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
 *  双缓冲 2 × 240×240×2 = 230KB PSRAM，另有一块 240×240 的旋转暂存 115KB。 */
#define APP_CAM_PREVIEW_W   240
#define APP_CAM_PREVIEW_H   240

/** 1 = 预览顺时针转 90°；0 = 逆时针。摄像头模组相对屏幕是转着装的，
 *  画面不转就是横躺的。转反了（上下颠倒）就把这个改 0。 */
#define APP_CAM_ROTATE_CW   1

/** 向摄像头请求的分辨率 / 帧率。
 *
 * **240×240**：驱动里有这个 PPI 的传感器寄存器表（cGC2145_240_240_TAB），
 * 取景框本身只有 204×240，这个分辨率已经够用，而且数据量只有 VGA 的 1/4
 * （240×240×2×15fps ≈ 1.7MB/s，VGA 是 9.2MB/s）——真机上 VGA 会把 PSRAM
 * 带宽和 CPU 一起顶满，DVP 一直刷 `sensor fifo is full`，界面和精灵全被拖慢。
 *
 * 帧率选 15：驱动对 240×240 没有单独的帧率表，set_ppi 会保留初始化时的
 * 帧长度寄存器，也就是 15fps 这一档。
 *
 * 注意：别的分辨率（比如 320×240）驱动里没有对应的 PPI 表，传感器仍按 VGA
 * 输出，硬件 YUV-BUF 的分辨率校验对不上，会不停报
 * `sensor's yuyv data resoltion is not right` 并丢帧。 */
#define APP_CAM_WANT_W      240
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
 * @brief 把最新一帧按 cover 方式采到 @a dst（取景框自己的尺寸）。
 *
 * 源图已经是转好 90° 的竖图（240×320）。
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
