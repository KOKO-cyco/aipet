/**
 * @file app_camera.h
 * @brief 摄像头服务：实时预览 + 拍照存卡（GC2145 / DVP）。
 *
 * 数据流
 * ------
 *   摄像头 --YUV422--> 相机线程里转成 240x320 RGB565（双缓冲，已旋转）
 *                              |
 *                              v
 *   拍照页取景框 <--overlay 直写 framebuffer-- 最新一帧
 *
 *   快门 --拼 BMP--> 内存里一张 240x320 24 位照片 --> 用户确认后写 /sdcard/pictures
 *
 * 为什么不用 JPEG
 * ---------------
 * 硬件 JPEG 只在开流那一刻定档，TDD 的 `is_inited` 让"关流重开成 JPEG"不生效；
 * 开流就带 JPEG 又会把 PSRAM 带宽和 CPU 吃干（VGA 实测预览掉到 2~4fps）。
 * 所以拍照直接用预览帧拼 BMP：所见即所得、不依赖编码器、快门也不黑屏。
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
 *  320×240 经 ROTT 旋转后就是 240×320，正好铺满这块缓冲；取景框再按 cover 采样。
 *  双缓冲 2 × 240×320×2 = 300KB PSRAM，另有一块 320×240 的旋转暂存 150KB。
 *  照片也直接取自这块缓冲（240×320 的 BMP）。 */
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
 * @brief 打开预览 overlay：把相机画面直接合成进显示 framebuffer。
 *
 * 取景框不再用 lv_canvas（LVGL 逐像素画一块 236×264 的画布很贵，实测把
 * `lv_refr_now` 从 11ms 顶到 40~80ms）。改成注册 SDK 显示端口的 pre-flush
 * 钩子：每次上屏前，如果相机有新帧，就把预览按 cover 缩放着写进当前
 * framebuffer 的对应矩形。
 *
 * @param[in] x,y,w,h 取景区域在**逻辑坐标**（480×320）下的矩形。
 * @param[in] radius  四角圆角半径（像素）；0 = 直角。角上的像素按覆盖率与
 *                    framebuffer 里已有的内容（卡片底色）混合，和 LVGL 的抗
 *                    锯齿一致；只影响 4 个 radius² 的小方块，代价可忽略。
 * @return TRUE = 已启用。
 * @note 逻辑矩形会按当前显示旋转（90°）映射到面板坐标，映射由
 *       `lv_display_rotate_area()` 现算，不写死旋转公式。
 */
BOOL_T app_camera_overlay_enable(int x, int y, int w, int h, int radius);

/** @brief 关闭 overlay（离开拍照页时调；关掉后请对原区域 invalidate 让 LVGL 重画）。 */
void app_camera_overlay_disable(void);

/**
 * @brief 暂停/恢复 overlay 的合成。
 *
 * overlay 是在 LVGL 画完之后直接往 framebuffer 里写像素的，它看不见 LVGL 画了
 * 什么 —— 所以任何盖在取景框上的东西（确认卡那种 sheet）都会被预览糊掉。弹窗
 * 打开时暂停，关掉后恢复；恢复时会强制补画一次，不用等下一帧相机数据。
 *
 * @note 暂停期间那一片会保留上一帧的画面（或 LVGL 重画后的底色），这是预期的：
 *       弹窗下面本来就是"定格"的取景画面。
 */
void app_camera_overlay_pause(BOOL_T pause);

/**
 * @brief 拍一张照片到内存（**不写卡**）。
 *
 * 同步，但很快（约 20ms）：把当前预览帧按 24 位 BMP 拼进内存，不切流、不等帧、
 * 不碰 SD，快门按下去界面不卡、预览也不黑屏。
 *
 * 成功后照片一直留在内存里，直到 `app_camera_save()` 或 `app_camera_discard()`
 * —— 快门的语义是"先拍，用户在确认卡上点保存再落盘"。
 *
 * @return TRUE = 内存里已经有一张照片；FALSE = 没开流 / 还没有帧 / 分配失败。
 */
BOOL_T app_camera_grab(void);

/**
 * @brief 把 `app_camera_grab()` 拍到的照片写进 /sdcard/pictures。
 *
 * 同步，且慢（约 1~3 秒）：要切 SD 线宽、写 225KB、再读回来校验。只能在用户
 * 明确点"保存"的路径上调。
 *
 * @param[out] path     存下来的完整路径（失败时为空串）。可以为 NULL。
 * @param[in]  path_sz  @a path 的容量。
 * @return TRUE = 已保存；FALSE = 没有待保存的照片 / 卡打不开 / 校验不过。
 */
BOOL_T app_camera_save(char *path, size_t path_sz);

/** @brief 丢掉抓到的照片（用户关掉确认卡 / 离开页面时调）。 */
void app_camera_discard(void);

/* ---- 画面参数（调色）------------------------------------------------------
 *
 * 传感器是 GC2145，参数就是数据手册里的寄存器，值都按寄存器原值给：
 *
 *   bright    P2:0xd5  Luma_offset     亮度偏移，S7 有符号，-64..63，默认 0
 *   contrast  P2:0xd3  luma_contrast   对比度，0x40 = 1.0，0x20..0x60
 *   sat       P2:0xd1/0xd2  Cb/Cr 饱和度，0x20 = 1.0，0x10..0x50
 *   aec       P1:0x13  AEC_target_Y    自动曝光的目标亮度，越大越亮，0x20..0xc0
 *   wbr/wbb   **不写寄存器**：像素层的红/蓝增益（百分比，100 = 原样），50..200
 *
 * 前四个是传感器参数，能调亮度/反差/饱和度/曝光；**色偏（发青、发红）不是它们
 * 能修的** —— 那是白平衡。AWB 由传感器自己做（P0:0x82[1]=1），缺省是开着的；
 * 场景光本身偏色（比如那颗 LED）或 AWB 被增益上限卡住时，画面就会整体偏；
 * 这时用 wbr/wbb 在预览和照片的像素上乘一个系数，立刻可见、也不影响传感器状态。
 *
 * **背光/点光源太亮（画面里那盏 LED）先动 aec**：目标亮度是 AEC 平均下来的
 * 期望值，调低整幅就压暗，过曝的光斑会缩。默认 0x7b 是驱动出厂值，数据手册
 * 的默认是 0x50。
 *
 * 改完 `app_camera_param_save()` 才写 flash；不保存的话重启回默认。
 */

typedef enum {
    APP_CAM_PARAM_BRIGHTNESS = 0,
    APP_CAM_PARAM_CONTRAST,
    APP_CAM_PARAM_SATURATION,
    APP_CAM_PARAM_AEC_TARGET,
    APP_CAM_PARAM_WB_R,             /* 红增益（百分比，100 = 原样） */
    APP_CAM_PARAM_WB_B,             /* 蓝增益（百分比，100 = 原样） */
    APP_CAM_PARAM_COUNT
} APP_CAM_PARAM_E;

/** @brief 读一个参数当前值（内存里的，未必等于传感器里的）。 */
int app_camera_param_get(APP_CAM_PARAM_E p);

/** @brief 改一个参数：夹到合法范围，相机开着就立刻写进传感器。 */
void app_camera_param_set(APP_CAM_PARAM_E p, int val);

/** @brief 参数名（`cam set <名字> <值>` 用的就是它）。 */
const char *app_camera_param_name(APP_CAM_PARAM_E p);

/** @brief 按名字找参数；找不到返回 FALSE。 */
BOOL_T app_camera_param_find(const char *name, APP_CAM_PARAM_E *out);

int app_camera_param_min(APP_CAM_PARAM_E p);
int app_camera_param_max(APP_CAM_PARAM_E p);

/** @brief 把所有参数写进传感器（开流后自动调一次；也可手动重放）。 */
void app_camera_param_apply(void);

/** @brief 全部恢复出厂默认（不写 flash）。 */
void app_camera_param_reset(void);

/** @brief 把当前参数存进 flash（下次开机自动加载）。 */
void app_camera_param_save(void);

/**
 * @brief 直接读写传感器寄存器（逃逸口：表里没有的都能这么试）。
 * @param[in] page 0..2（P0/P1/P2）
 */
OPERATE_RET app_camera_reg_write(uint8_t page, uint8_t reg, uint8_t val);
OPERATE_RET app_camera_reg_read(uint8_t page, uint8_t reg, uint8_t *val);

/** @brief 照片质量：标准 = 16 位 BMP（约 150KB），高清 = 24 位 BMP（约 225KB）。 */
typedef enum {
    APP_CAM_QUALITY_STANDARD = 0,
    APP_CAM_QUALITY_HD       = 1,
} APP_CAM_QUALITY_E;

void app_camera_set_quality(APP_CAM_QUALITY_E q);

/** @brief 合影时角色站的位置（数值与 PET_POS_E 一致：0 左 / 1 右）。 */
#define APP_CAM_COMPOSE_LEFT  0
#define APP_CAM_COMPOSE_RIGHT 1

/**
 * @brief 设置拍照时是否把角色叠进去、叠在哪一边。
 *
 * 角色用的是 `third_party/my_pet/右侧半身比耶.png` 抠像后编进 flash 的立绘
 * （见 app_cam_pet.h / tools/make_compose_pet.py），**带 alpha 混合** ——
 * 是叠上去的，不是贴一块方图。
 *
 * 站位：原图是在右边缘切过的半身稿，所以**贴边不放留白** —— 右边贴右边缘，
 * 左边镜像后贴左边缘；竖直方向底边离照片底 12px。
 *
 * 只在按快门那一刻合成，预览不受影响。
 */
void app_camera_set_compose(BOOL_T on, int pos);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAMERA_H__ */
