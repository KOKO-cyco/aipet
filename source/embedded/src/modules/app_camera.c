/**
 * @file app_camera.c
 * @brief 见 app_camera.h。
 *
 * 几个从 bring-up 里带过来的决定（别再改回去）：
 *
 * 1) **YUV422 自己转、自己缩**（默认），不用 SDK 的 `tal_image_convert_yuv422_to_rgb565`
 *    —— 它内部只按 `min(in_w,out_w) × min(in_h,out_h)` 逐点拷，不会缩放。
 *    这里是一趟做完"cover 裁切 + 最近邻缩放"。
 *    2026-10-20 加了可选的 DMA2D 路径（APP_CAM_USE_DMA2D），VGA 下实测更慢，
 *    暂不启用。
 * 2) **按 cover 裁，不拉伸**：摄像头 4:3、取景框不是，硬缩会把画面拉长。
 * 3) 帧回调里**只做内存操作**：写 SD 会把 SDIO 堵住、把出帧队列顶爆。
 * 4) **双缓冲**：回调写后缓冲、写完再翻 `sg_front`；LVGL 读前缓冲，互不打架。
 * 5) **缓冲关流不释放**：回调可能在关流后还在跑最后一帧，释放会踩空指针。
 */

#include <string.h>
#include <stdlib.h>                     /* strtol：解析 flash 里存的参数串 */
#include <math.h>                       /* overlay 圆角覆盖率表（sqrtf） */

#include "tal_api.h"
#include "lvgl.h"                       /* overlay：把预览直接写进显示 framebuffer */
#include "tkl_fs.h"
#include "tkl_dvp.h"                    /* 硬件 resize 的输出缓冲 */
#include "tal_image_yuv422_to_rgb.h"    /* YUV422->RGB565：有 DMA2D 时走硬件 */

#include "tdl_camera_manage.h"
#include "tdl_display_manage.h"         /* TDL_DISP_FRAME_BUFF_T（overlay 直写 fb）*/
#include "tdd_camera_gc2145.h"          /* 运行时读写传感器寄存器（调画面参数） */

#include "app_camera.h"
#include "app_cam_pet.h"                /* 合影立绘（绿幕抠像后编进 flash） */
#include "app_settings.h"
#include "app_storage.h"
#include "app_time.h"

/* 板级 Kconfig 定的设备名（src/peripherals/camera/Kconfig，默认 "camera"）。 */
#ifndef CAMERA_NAME
#define CAMERA_NAME         "camera"
#endif

/* 照片（BMP）缓冲：240×320。
 *
 * 高清 = 24 位（行 240×3 = 720 字节，本来就是 4 的倍数，不用补）；
 * 标准 = 16 位 RGB565（BI_BITFIELDS，行 240×2 = 480 字节，也不用补），
 * 文件小三分之一。缓冲按高清的大小开，标准档只用到前面一截。 */
#define APP_CAM_BMP_HDR_24  (14 + 40)
#define APP_CAM_BMP_HDR_16  (14 + 40 + 12)          /* 16 位要带 R/G/B 三个掩码 */
#define APP_CAM_BMP_ROW_24  (APP_CAM_PREVIEW_W * 3)
#define APP_CAM_BMP_ROW_16  (APP_CAM_PREVIEW_W * 2)
#define APP_CAM_BMP_LEN     (APP_CAM_BMP_HDR_24 + APP_CAM_BMP_ROW_24 * APP_CAM_PREVIEW_H)

/* 预览刷新上限（帧/秒）。
 *
 * **锁 10fps 是刻意的**：预览的每一帧都要走 ROTT（YUV→RGB565+旋转）、写一份
 * 150KB 的 PSRAM、再由 overlay 在每次刷屏时写进 framebuffer —— 提到 30fps
 * 只是把这三笔开销乘三倍，换来的流畅度提升有限。刷屏（LVGL）自己按 31Hz 走，
 * 相机帧按 10fps 换，两者互不牵制。
 *
 * 注意闸门是"距上次转换 ≥ 1000/fps"，源帧率不是它的整数倍时会有额外损耗
 * （以前设 10fps、源 15fps，实测只有 7.5fps）。 */
#define APP_CAM_PREVIEW_FPS 10

#define CAM_TAG             "[cam]"

/* ---- 状态 ---------------------------------------------------------------- */

static TDL_CAMERA_HANDLE_T sg_hdl;
static BOOL_T              sg_open;

/* 预览双缓冲：回调写 [1-front]，写完翻 front。 */
static uint16_t *sg_buf[2];
static uint16_t *sg_tmp;                /* 240x240 横图暂存，旋转用 */
#if APP_CAM_USE_DMA2D
static uint16_t *sg_rgb;                /* 240x240 RGB565：DMA2D 转换的整幅中间图 */
#endif
static volatile int sg_front = -1;      /* -1 = 还没有完整帧 */
static volatile uint32_t sg_gen;
static volatile BOOL_T sg_first_frame_log;  /* 每次开流后只打一次"首帧" */

/* 实测出帧率：驱动的帧率表不一定有我们请求的那一档（会静默跳过），
 * 所以每 2 秒打一次真实帧数，确认传感器到底跑多快。 */
static uint32_t sg_fps_cnt;
static uint32_t sg_fps_t0;
static uint32_t sg_conv_ms_sum;             /* 每帧转换耗时累计（2 秒窗口） */
static uint32_t sg_conv_ms_max;
static uint32_t sg_rot_ms_sum;              /* 其中硬件 ROTT 那一拍的耗时 */
static uint32_t sg_rot_ms_max;
static uint32_t sg_last_conv_ms;            /* 预览限帧：上一帧转换的时间 */
static uint32_t sg_ov_draw_cnt;             /* overlay 实际合成次数（2 秒窗口） */
static uint32_t sg_ov_skip;                 /* overlay 跳过合成的次数（2 秒窗口） */

/* 应用层的 YUV422 排列。SDK 的 TUYA_YUV422_ORDER_E 只有 UYVY/YUYV 两种，
 * 但实测这块板子的 GC2145 输出的是 VYUY，所以在应用层补一种。 */
typedef enum {
    APP_YUV_UYVY = 0,       /* U0 Y0 V0 Y1 */
    APP_YUV_YUYV = 1,       /* Y0 U0 Y1 V0 */
    APP_YUV_VYUY = 2,       /* V0 Y0 U0 Y1 —— GC2145 实测排列 */
} APP_YUV_ORDER_E;

static APP_YUV_ORDER_E sg_order = APP_YUV_VYUY;

/* 白平衡微调（像素层红/蓝增益）：定义在参数表旁边，抓图和 overlay 都要用。 */
static uint16_t prv_wb_px(uint16_t px);

/* 快门：把当前预览帧拼成 BMP 放在这块缓冲里，等用户确认再写卡。 */
static uint8_t  *sg_bmp;
static uint32_t  sg_bmp_len;

/* 抓图期间置位：帧回调看到它就跳过这一帧。
 *
 * 双缓冲只挡得住"下一帧"，挡不住"grab 正在读前缓冲时回调翻页又写它"——
 * grab 要 20~40ms（拼 BMP + 合影），刚好一个帧周期。窗口里翻一次页，照片就是
 * 上半张旧帧、下半张新帧。跳过一帧的代价远小于存出一张坏图。 */
static volatile BOOL_T sg_grab_busy;

/* 拍照设置：位深 / 是否把角色叠进照片 + 叠在哪。 */
static APP_CAM_QUALITY_E sg_quality = APP_CAM_QUALITY_HD;
static BOOL_T            sg_comp_on;
static int               sg_comp_pos = APP_CAM_COMPOSE_RIGHT;

/* ---- YUV422 -> RGB565（cover 裁切 + 最近邻缩放） -------------------------- */

/** 横图暂存尺寸：320×240（传感器输出），转 90° 成 240×320。 */
#define APP_CAM_TMP_W       320
#define APP_CAM_TMP_H       240

/* 1 = 用 YUV BUF 模块的硬件 resize（640×480 -> 240×184），应用只读缩小后的
 * 缓冲；0 = 读整幅 640×480 再软件缩。
 *
 * **2026-10-20 实测：硬件 resize 在这块板子上不工作**（缓冲里是未初始化的
 * 粉紫色噪点），而且它的输出寄存器语义存疑（可能是编码模块的读地址）。
 * 先关掉，等拿到 BPS 寄存器说明再试。 */
#define APP_CAM_USE_HW_RESIZE   0

/* 1 = 用 ROTT/PFC 硬件做 YUV->RGB565 + 90° 旋转（单帧测试）。
 * 输入按 YUYV + 字节翻转匹配这颗传感器的 VYUY；输出 480×640 RGB565，
 * 再由软件缩到预览尺寸。0 = 走原来的软件转色 + 软件旋转。 */
#define APP_CAM_USE_HW_ROTATE   1

/* 1 = YUV->RGB565 走 DMA2D 硬件（整幅转色后再软件缩）；0 = 纯软件逐像素。
 *
 * 2026-10-20 实测：VGA 下打开 DMA2D 反而更慢（转换 70~112ms、帧率掉到 6fps），
 * 而且每帧 600KB 的整幅写回 EMC 会和 DVP/面板抢总线，FIFO 溢出更凶。
 * 目前用软件路径；要再压 CPU 可以改回 1，但整幅缓冲 600KB，需要实测。 */
#define APP_CAM_USE_DMA2D   0

/* 列/行映射表：每帧只建一次（320+240 次），内层循环变成查表。
 * 两个方向都会用到（横图 320x240 与竖图 240x320），按最大边开。 */
#define APP_CAM_MAP_MAX     320
static uint16_t s_xmap[APP_CAM_MAP_MAX];
static uint16_t s_ymap[APP_CAM_MAP_MAX];

/**
 * @brief 一帧 YUV422 裁成目标宽高比、最近邻缩到 @a dw × @a dh。
 *
 * YUV422 每像素 2 字节，两个像素共用一组 UV。支持三种排列：
 *   UYVY: U0 Y0 V0 Y1
 *   YUYV: Y0 U0 Y1 V0
 *   VYUY: V0 Y0 U0 Y1  （GC2145 实测）
 * 取某个源像素的 Y 要按它自己的奇偶挑字节。
 */
static void prv_yuv422_cover_to_rgb565(const uint8_t *src, int sw, int sh,
                                       uint16_t *dst, int dw, int dh,
                                       APP_YUV_ORDER_E order)
{
    int src_w, src_h, x0, y0;

    if (NULL == src || NULL == dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }

    /* 先按"用满宽"算需要的源高；太高就改用满高。保证 src_w/src_h == dw/dh。 */
    src_w = sw;
    src_h = (int)((int64_t)sw * dh / dw);
    if (src_h > sh) {
        src_h = sh;
        src_w = (int)((int64_t)sh * dw / dh);
    }
    src_w &= ~1;                            /* YUV422 成对，宽度取偶数 */
    if (src_w <= 0 || src_h <= 0) {
        return;
    }
    x0 = (sw - src_w) / 2;
    y0 = (sh - src_h) / 2;

    /* 建映射表（尺寸超了就走慢路径，直接除）。 */
    if (dw <= APP_CAM_MAP_MAX && dh <= APP_CAM_MAP_MAX) {
        for (int x = 0; x < dw; x++) {
            s_xmap[x] = (uint16_t)(x0 + x * src_w / dw);
        }
        for (int y = 0; y < dh; y++) {
            s_ymap[y] = (uint16_t)(y0 + y * src_h / dh);
        }
    }

    const int    stride = sw * 2;
    const BOOL_T mapped = (dw <= APP_CAM_MAP_MAX && dh <= APP_CAM_MAP_MAX);

    for (int y = 0; y < dh; y++) {
        int            sy  = mapped ? s_ymap[y] : (y0 + y * src_h / dh);
        const uint8_t *row = src + (size_t)sy * stride;
        uint16_t      *out = dst + (size_t)y * dw;

        for (int x = 0; x < dw; x++) {
            int sx = mapped ? s_xmap[x] : (x0 + x * src_w / dw);
            const uint8_t *p = row + (sx & ~1) * 2;
            int u, v, yv, r, g, b, c, d, e;

            if (APP_YUV_YUYV == order) {
                yv = p[(sx & 1) ? 2 : 0];
                u  = p[1];
                v  = p[3];
            } else if (APP_YUV_VYUY == order) {
                v  = p[0];
                yv = p[(sx & 1) ? 3 : 1];
                u  = p[2];
            } else {
                u  = p[0];
                yv = p[1 + (sx & 1) * 2];
                v  = p[2];
            }

            /* BT.601，和 SDK 里那套整数系数一致（>>8 定点）。 */
            c = yv - 16;
            d = u - 128;
            e = v - 128;
            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            out[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/**
 * @brief RGB565 整幅 -> 目标尺寸（cover 裁切 + 最近邻缩放），只缩不放、不转色。
 *
 * DMA2D 的 YUV->RGB 只会拷贝和转色，**不会缩放**（width_cp/height_cp 是裁剪）。
 * 所以先用硬件把整幅 YUV 转成同尺寸 RGB565，再由这里缩到目标尺寸 ——
 * 这一遍只读 RGB565、没有 YUV 数学，比逐像素算色便宜得多。
 */
static void prv_rgb565_cover_scale(const uint16_t *src, int sw, int sh,
                                   uint16_t *dst, int dw, int dh)
{
    int src_w, src_h, x0, y0;

    if (NULL == src || NULL == dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }

    src_w = sw;
    src_h = (int)((int64_t)sw * dh / dw);
    if (src_h > sh) {
        src_h = sh;
        src_w = (int)((int64_t)sh * dw / dh);
    }
    if (src_w <= 0 || src_h <= 0) {
        return;
    }
    x0 = (sw - src_w) / 2;
    y0 = (sh - src_h) / 2;

    for (int y = 0; y < dh; y++) {
        int             sy  = y0 + y * src_h / dh;
        const uint16_t *row = src + (size_t)sy * sw + x0;
        uint16_t       *out = dst + (size_t)y * dw;

        for (int x = 0; x < dw; x++) {
            out[x] = row[x * src_w / dw];
        }
    }
}

/**
 * @brief 90° 旋转：src (sw × sh) -> dst (sh × sw)。
 *
 * 行内读写都是连续的（CW：读反向、写正向；CCW：读跨行、写正向），不用逐像素
 * 算坐标。摄像头模组是转着装的，预览必须转这 90°。
 */
static void prv_rotate_90(const uint16_t *src, int sw, int sh, uint16_t *dst, BOOL_T cw)
{
    if (cw) {
        /* 顺时针：dst(y,x) = src(row = sh-1-x, col = y)，dst 尺寸 sh×sw。 */
        for (int y = 0; y < sw; y++) {          /* dst 行数 = src 宽 */
            uint16_t *out = dst + (size_t)y * sh;

            for (int x = 0; x < sh; x++) {
                out[x] = src[(size_t)(sh - 1 - x) * sw + y];
            }
        }
    } else {
        for (int y = 0; y < sw; y++) {
            uint16_t *out = dst + (size_t)y * sh;

            for (int x = 0; x < sh; x++) {
                out[x] = src[(size_t)(sw - 1 - y) + (size_t)x * sw];
            }
        }
    }
}

/* ---- 相机回调（跑在相机自己的线程里） ------------------------------------ */

static OPERATE_RET prv_raw_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    int back;

    (void)hdl;

    if (NULL == frame || NULL == frame->data || 0 == frame->data_len) {
        return OPRT_INVALID_PARM;
    }
    if (TUYA_FRAME_FMT_YUV422 != frame->fmt) {
        return OPRT_OK;
    }

    /* 快门正在读前缓冲：这一帧跳过（见 sg_grab_busy）。 */
    if (sg_grab_busy) {
        return OPRT_OK;
    }

    /* 限帧：预览锁 10fps（100ms 一帧），其余帧立刻返回。
     * 320×240 没有帧率表，传感器按默认帧率出图（约 30fps），
     * 用时间闸门比隔帧更稳，也能避免无效的转换吃 CPU。 */
    {
        uint32_t now = tal_system_get_millisecond();

        if ((now - sg_last_conv_ms) < (1000u / APP_CAM_PREVIEW_FPS)) {
            return OPRT_OK;
        }
        sg_last_conv_ms = now;
    }

    /* 写"后缓冲"，写完再翻 front —— LVGL 那边永远读的是一份完整帧。
     * 指针先取到局部变量再判空：缓冲虽然关流时不再释放，但这里少读一次
     * 全局，避免以后改回"可释放"时又踩一次 TOCTOU。 */
    back = (sg_front == 0) ? 1 : 0;
    {
        uint16_t *dst = sg_buf[back];
        uint16_t *tmp = sg_tmp;
#if APP_CAM_USE_DMA2D
        uint16_t *rgb = sg_rgb;
#endif

        if (NULL == dst || NULL == tmp) {
            return OPRT_OK;
        }

        /* 两步：YUV -> 横图 RGB565（裁+缩），再转 90° 成竖图。
         *
         * 第一步优先交给 DMA2D（硬件只转色、不缩放），转完整幅 640x480 后再由
         * 软件缩到 320x240；DMA2D 不可用或帧比缓冲大时退回原来的逐像素路径。
         * 纯软件算色实测要 55ms/帧，正好卡在 15fps 的 66ms 帧周期上 —— 相机
         * 线程的缓冲被占满，DVP 就会刷 `sensor fifo is full`。 */
        {
            uint32_t t0 = tal_system_get_millisecond();
            uint32_t dt;
            BOOL_T   converted = FALSE;
            BOOL_T   rotated   = FALSE;

#if APP_CAM_USE_HW_ROTATE
            /* 硬件 ROTT：YUV -> RGB565 + 90° 一次完成。
             *
             * 旋转后的尺寸**正好等于预览缓冲**时，让 ROTT 直接写进后缓冲 ——
             * 这时 preview 的 cover 映射是恒等映射，原来那趟"硬件写 150KB →
             * CPU 再读 150KB 写 150KB"的整幅拷贝纯属白搬，每帧白占 300KB 的
             * PSRAM 带宽（实测相机开着时整机 flush 从 1ms 涨到 7ms，就是这类
             * 流量挤的）。尺寸对不上（换传感器/换分辨率）才退回拷贝那条路。 */
            {
                uint32_t rot_ms   = 0;
                uint32_t rot_w    = 0;
                uint32_t rot_h    = 0;
                uint16_t *rot_rgb = NULL;
                BOOL_T   direct   = ((uint32_t)APP_CAM_PREVIEW_W == frame->height &&
                                     (uint32_t)APP_CAM_PREVIEW_H == frame->width) ? TRUE : FALSE;

                if (direct) {
                    uint32_t rt0 = tal_system_get_millisecond();

                    if (OPRT_OK == tkl_dvp_rotate_rgb565(frame->data, frame->width,
                                                         frame->height, dst, &rot_w, &rot_h)) {
                        converted = TRUE;
                        rotated   = TRUE;
                    }
                    rot_ms = tal_system_get_millisecond() - rt0;
                } else if (OPRT_OK == tkl_dvp_test_rotate(frame->data, frame->width, frame->height,
                                                          &rot_ms, &rot_w, &rot_h, &rot_rgb) &&
                           NULL != rot_rgb && rot_w > 0 && rot_h > 0) {
                    prv_rgb565_cover_scale(rot_rgb, (int)rot_w, (int)rot_h,
                                           dst, APP_CAM_PREVIEW_W, APP_CAM_PREVIEW_H);
                    converted = TRUE;
                    rotated   = TRUE;
                }

                if (converted) {
                    sg_rot_ms_sum += rot_ms;
                    if (rot_ms > sg_rot_ms_max) {
                        sg_rot_ms_max = rot_ms;
                    }
                }
            }
#endif

#if APP_CAM_USE_DMA2D
            if (!converted && NULL != rgb && frame->width > 0 && frame->height > 0 &&
                frame->width <= APP_CAM_WANT_W && frame->height <= APP_CAM_WANT_H) {
                TAL_IMAGE_YUV422_TO_RGB_T cfg = { 0 };

                cfg.in_buf     = frame->data;
                cfg.in_width   = frame->width;
                cfg.in_height  = frame->height;
                cfg.out_buf    = (uint8_t *)rgb;
                cfg.out_width  = frame->width;
                cfg.out_height = frame->height;
                /* DMA2D 只认 UYVY/YUYV，不支持 VYUY；这个分支默认是关的。 */
                cfg.in_order   = (APP_YUV_YUYV == sg_order) ? TUYA_YUV422_YUYV
                                                            : TUYA_YUV422_UYVY;

                if (OPRT_OK == tal_image_convert_yuv422_to_rgb565(&cfg)) {
                    prv_rgb565_cover_scale(rgb, frame->width, frame->height,
                                           tmp, APP_CAM_TMP_W, APP_CAM_TMP_H);
                    converted = TRUE;
                }
            }
#endif

            if (!converted) {
#if APP_CAM_USE_HW_RESIZE
                uint32_t rw = 0;
                uint32_t rh = 0;
                uint8_t *rbuf = NULL;

                if (OPRT_OK == tkl_dvp_get_resize_info(&rw, &rh, &rbuf) &&
                    NULL != rbuf && rw > 0 && rh > 0) {
                    prv_yuv422_cover_to_rgb565(rbuf, (int)rw, (int)rh,
                                               tmp, APP_CAM_TMP_W, APP_CAM_TMP_H,
                                               sg_order);
                } else
#endif
                {
                    prv_yuv422_cover_to_rgb565(frame->data, frame->width, frame->height,
                                               tmp, APP_CAM_TMP_W, APP_CAM_TMP_H, sg_order);
                }
            }

            if (!rotated) {
                prv_rotate_90(tmp, APP_CAM_TMP_W, APP_CAM_TMP_H, dst,
                              (BOOL_T)APP_CAM_ROTATE_CW);
            }

            /* 转换耗时：TDL 在回调返回前一直占着这一帧缓冲，转换只要超过帧周期
             * （15fps = 66ms）就会把相机线程的缓冲耗光 → DVP FIFO 溢出。 */
            dt = tal_system_get_millisecond() - t0;
            sg_conv_ms_sum += dt;
            if (dt > sg_conv_ms_max) {
                sg_conv_ms_max = dt;
            }
        }
    }
    sg_front = back;
    sg_gen++;

    if (sg_first_frame_log == FALSE) {
        sg_first_frame_log = TRUE;
        PR_NOTICE(CAM_TAG " 预览首帧：源 %dx%d -> %dx%d（旋转 %s）",
                  frame->width, frame->height,
                  APP_CAM_PREVIEW_W, APP_CAM_PREVIEW_H,
                  APP_CAM_ROTATE_CW ? "顺时针90°" : "逆时针90°");
    }

    /* 每 2 秒报一次真实帧率（PR_DEBUG，周期统计的默认档）。 */
    sg_fps_cnt++;
    {
        uint32_t now = tal_system_get_millisecond();

        if (0 == sg_fps_t0) {
            sg_fps_t0 = now;
        } else if ((now - sg_fps_t0) >= 2000u) {
            uint32_t ms = now - sg_fps_t0;

            /* 合成/跳过：overlay 是"有新帧或有人画到取景框上"才重画，这一对比
             * 能直接看出降频有没有生效（相机 10fps 时合成次数应≈帧数）。 */
            PR_DEBUG(CAM_TAG " 预览 %u 帧/%ums（约 %u fps），转换 平均 %u ms 最大 %u ms"
                     "（其中 ROTT %u/%u），overlay 合成 %u 跳过 %u",
                     (unsigned)sg_fps_cnt, (unsigned)ms,
                     (unsigned)(sg_fps_cnt * 1000u / ms),
                     (unsigned)(sg_conv_ms_sum / sg_fps_cnt),
                     (unsigned)sg_conv_ms_max,
                     (unsigned)(sg_rot_ms_sum / sg_fps_cnt),
                     (unsigned)sg_rot_ms_max,
                     (unsigned)sg_ov_draw_cnt, (unsigned)sg_ov_skip);

            sg_fps_cnt     = 0;
            sg_fps_t0      = now;
            sg_conv_ms_sum = 0;
            sg_conv_ms_max = 0;
            sg_rot_ms_sum  = 0;
            sg_rot_ms_max  = 0;
            sg_ov_draw_cnt = 0;
            sg_ov_skip     = 0;
        }
    }
    return OPRT_OK;
}

/* ---- 拍照：把预览帧拼成一张 BMP ------------------------------------------ */

static void prv_put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void prv_put_u32(uint8_t *p, uint32_t v)
{
    prv_put_u16(p, v & 0xFFFFu);
    prv_put_u16(p + 2, (v >> 16) & 0xFFFFu);
}

/**
 * @brief 写 BMP 文件头（BITMAPFILEHEADER + BITMAPINFOHEADER [+ 三个掩码]）。
 *
 * 自下而上（height 为正）、无压缩。16 位用 BI_BITFIELDS 带 R/G/B 掩码，
 * 因为 BMP 的 16 位 BI_RGB 是 5-5-5，和我们的 RGB565 对不上。
 */
static void prv_bmp_header(uint8_t *h, uint32_t data_len, APP_CAM_QUALITY_E q)
{
    const BOOL_T hd   = (APP_CAM_QUALITY_HD == q) ? TRUE : FALSE;
    const int    hdr  = hd ? APP_CAM_BMP_HDR_24 : APP_CAM_BMP_HDR_16;

    memset(h, 0, (size_t)hdr);

    h[0] = 'B';
    h[1] = 'M';
    prv_put_u32(h + 2, (uint32_t)hdr + data_len);        /* 文件总长 */
    prv_put_u32(h + 10, (uint32_t)hdr);                  /* 像素数据偏移 */
    prv_put_u32(h + 14, 40);                             /* DIB 头长 */
    prv_put_u32(h + 18, APP_CAM_PREVIEW_W);
    prv_put_u32(h + 22, APP_CAM_PREVIEW_H);              /* 正数 = 自下而上 */
    prv_put_u16(h + 26, 1);                              /* 平面数 */
    prv_put_u16(h + 28, hd ? 24 : 16);                   /* 位深 */
    prv_put_u32(h + 34, data_len);                       /* 像素数据长 */
    prv_put_u32(h + 38, 2835);                           /* 横向 DPI（72dpi） */
    prv_put_u32(h + 42, 2835);                           /* 纵向 DPI */

    if (!hd) {
        prv_put_u32(h + 30, 3);                          /* BI_BITFIELDS */
        prv_put_u32(h + 54, 0xF800);                     /* R 掩码 */
        prv_put_u32(h + 58, 0x07E0);                     /* G 掩码 */
        prv_put_u32(h + 62, 0x001F);                     /* B 掩码 */
    }
}

/**
 * @brief 合影：算角色贴在哪，返回立绘指针。
 *
 * 立绘是 RGB565A8 的常量表（`third_party/sprite/make_compose_pet.py` 从绿幕 PNG 抠出来、缩到
 * 180px 高），alpha 混合在写 BMP 的那一趟里逐像素做 —— 所以边缘是干净的，不是
 * 一块方图。
 *
 * **贴边不放留白**：原图本身就是在右边缘切过的半身稿，留 8px 反而会把那条
 * 切边悬在画面中间。放在右边就贴右边缘，放在左边就镜像后贴左边缘。
 *
 * @param[out] ox,oy 左上角（照片坐标）
 */
static const APP_CAM_PET_T *prv_compose_layout(int *ox, int *oy)
{
    const APP_CAM_PET_T *pet = app_cam_pet_get();

    if (NULL == pet || NULL == pet->data || pet->w <= 0 || pet->h <= 0) {
        return NULL;
    }
    if (pet->w > APP_CAM_PREVIEW_W || pet->h > APP_CAM_PREVIEW_H) {
        return NULL;
    }

    *ox = (APP_CAM_COMPOSE_LEFT == sg_comp_pos) ? 0
                                                : (APP_CAM_PREVIEW_W - pet->w);
    *oy = APP_CAM_PREVIEW_H - pet->h - 12;

    if (*ox < 0) {
        *ox = 0;
    }
    if (*oy < 0) {
        *oy = 0;
    }

    return pet;
}

/**
 * @brief RGB565 按 alpha 混合（a = 0 全取底，255 全取上）。
 *
 * 在 5/6 位通道里算就够：再展开到 8 位只是把误差摊薄，屏幕上看不出来，反而不如
 * 少两次乘除。
 */
static uint16_t prv_blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    int dr, dg, db;
    int sr, sg, sb;
    int r, g, b;

    if (0 == a) {
        return dst;
    }
    if (255 == a) {
        return src;
    }

    dr = (dst >> 11) & 0x1F;
    dg = (dst >> 5) & 0x3F;
    db = dst & 0x1F;
    sr = (src >> 11) & 0x1F;
    sg = (src >> 5) & 0x3F;
    sb = src & 0x1F;

    r = (sr * a + dr * (255 - a)) / 255;
    g = (sg * a + dg * (255 - a)) / 255;
    b = (sb * a + db * (255 - a)) / 255;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---- 存卡 ---------------------------------------------------------------- */

/** 拼一个还没被占用的路径：时间对上就用时间戳，否则 cam / cam_2 / … */
static void prv_make_path(char *out, size_t out_sz)
{
    char stamp[32] = { 0 };

    if (app_time_valid() &&
        OPRT_OK == app_time_format(stamp, sizeof(stamp), "%Y%m%d_%H%M%S") &&
        '\0' != stamp[0] && '-' != stamp[0]) {
        snprintf(out, out_sz, "%s/%s/cam_%s.bmp", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR, stamp);
        return;
    }

    for (int i = 1; i <= 999; i++) {
        char   name[32];
        char   path[128];
        BOOL_T ex = FALSE;

        if (1 == i) {
            snprintf(name, sizeof(name), "cam.bmp");
        } else {
            snprintf(name, sizeof(name), "cam_%d.bmp", i);
        }
        snprintf(path, sizeof(path), "%s/%s/%s", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR, name);
        if (OPRT_OK != tkl_fs_is_exist(path, &ex) || !ex) {
            snprintf(out, out_sz, "%s", path);
            return;
        }
    }
    snprintf(out, out_sz, "%s/%s/cam_999.bmp", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR);
}

/* 落盘校验：把刚写的文件读回来逐字节比。
 *
 * 为什么非要比：SDIO 驱动的块写失败**只打一行 WARN**
 * （`write data error, wr_status = 0x7!!!`），既不回传给 FatFs、也不影响
 * write() 的返回值，所以 `fwrite` 报满、`fsync` 报 OK，卡上却是坏数据。
 * 2026-10-20 实测就是"日志说照片已存、相册里打不开"。唯一可信的判据是
 * 自己读回来比一遍 —— 写 8KB 的代价，比让用户以为存上了小得多。 */
static BOOL_T prv_verify_shot(const char *path)
{
    static uint8_t s_cmp[512];
    TUYA_FILE      f;
    uint32_t       done = 0;

    f = tkl_fopen(path, "r");
    if (NULL == f) {
        /* 写的时候 fopen("w") 成功、回头 fopen("r") 却失败，是"目录项没落盘"
         * 的典型形态，和"内容写坏"是两种不同的失败，分开报才不误导现场判断。 */
        PR_WARN(CAM_TAG " 落盘校验：文件读不回来（目录项没落盘）");
        return FALSE;
    }

    while (done < sg_bmp_len) {
        uint32_t n = sg_bmp_len - done;

        if (n > sizeof(s_cmp)) {
            n = sizeof(s_cmp);
        }
        if ((int32_t)n != tkl_fread(s_cmp, (int32_t)n, f)) {
            break;
        }
        if (0 != memcmp(s_cmp, sg_bmp + done, n)) {
            break;
        }
        done += n;
    }

    (void)tkl_fclose(f);

    if (done != sg_bmp_len) {
        PR_WARN(CAM_TAG " 落盘校验失败：第 %u/%u 字节对不上（块写被 SD 吞了）",
                (unsigned)done, (unsigned)sg_bmp_len);
        return FALSE;
    }

    return TRUE;
}

static BOOL_T prv_save_shot(char *path, size_t path_sz)
{
    char      dir[64];
    char      full[160];
    BOOL_T    ok = FALSE;

    if (0 == sg_bmp_len || NULL == sg_bmp) {
        return FALSE;
    }

    /* **先把卡切成"能写"的那一档**（方案 B）。
     *
     * 平时卡停在 4 线 —— 读最快（1000~2700 KB/s），但**写不进**（每档时钟全是
     * `wr_status = 0x7`）。能写的是 1 线，所以按快门这一刻整条重起成 1 线，
     * 写完再切回 4 线，动画该多快还是多快。切档约 1~1.5 秒，而抓图本身就要等
     * 2 秒，用户感知不到。
     *
     * mkdir 必须在切档**之后**：建目录本身就是在写卡，放在前面等于用 4 线写一遍。 */
    if (OPRT_OK != app_storage_write_begin()) {
        PR_ERR(CAM_TAG " 切不到写档（1 线），照片没存；看上面 [sto] 的起卡日志");
        return FALSE;
    }

    snprintf(dir, sizeof(dir), "%s/%s", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR);
    (void)tkl_fs_mkdir(dir);        /* 已存在时返回 8(FR_EXIST)，是预期内的 */

    prv_make_path(full, sizeof(full));

    /* 整段写 + 校验关进 SD 临界区。SD 是单总线：精灵预取线程此刻正在同一张卡上
     * 以 1~2.7MB/s 连读（日志里"预取 2690 KB/s"和"write data error"永远在同一
     * 秒），读写叠在一起就是块写成批失败的直接原因。临界区也顺便让预取让路，
     * 写完立刻放行。 */
    app_storage_busy_enter();

    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        TUYA_FILE f;

        if (attempt > 0) {
            tal_system_sleep(40);
        }

        f = tkl_fopen(full, "w");
        if (NULL == f) {
            PR_ERR(CAM_TAG " 建不了 %s（卡满 / 只读 / 目录项写不进）", full);
            break;
        }
        if ((int)sg_bmp_len != tkl_fwrite(sg_bmp, (int)sg_bmp_len, f)) {
            PR_ERR(CAM_TAG " 写 %s 失败（要 %u 字节）", full, (unsigned)sg_bmp_len);
            (void)tkl_fclose(f);
            break;
        }
        (void)tkl_fsync(tkl_fileno(f));
        (void)tkl_fclose(f);

        ok = prv_verify_shot(full);
        if (!ok) {
            PR_WARN(CAM_TAG " 第 %d 次落盘不合格，重写", attempt + 1);
            (void)tkl_fs_remove(full);
        }
    }

    app_storage_busy_leave();

    /* 切回读最快的那一档（4 线），动画立刻恢复满速。成功失败都要切。 */
    app_storage_write_end();

    if (!ok) {
        PR_ERR(CAM_TAG " %s 落盘失败（3 次都校验不过）。写档是固定的 1 线 @13M"
                       "（开机不再试写），所以这基本只剩卡/供电/卡座三种解释",
               full);
        return FALSE;
    }

    if (NULL != path && path_sz > 0) {
        snprintf(path, path_sz, "%s", full);
    }
    PR_NOTICE(CAM_TAG " 照片已存 %s（%u 字节，已校验）", full, (unsigned)sg_bmp_len);
    return TRUE;
}

/* ---- 开流 ---------------------------------------------------------------- */

/**
 * @brief 打开预览流（只 YUV422）。
 *
 * **不带 JPEG**：JPEG 编码流会把 PSRAM 带宽和 CPU 吃干（实测 VGA@15fps 下精灵
 * 预取掉到 100KB/s、界面 2~4fps）。拍照不再需要它 —— 见 app_camera_grab()。
 */
static OPERATE_RET prv_stream_open(void)
{
    TDL_CAMERA_CFG_T cfg = { 0 };

    cfg.fps          = APP_CAM_FPS;
    cfg.width        = APP_CAM_WANT_W;
    cfg.height       = APP_CAM_WANT_H;
    cfg.out_fmt      = TDL_CAMERA_FMT_YUV422;
    cfg.get_frame_cb = prv_raw_cb;

    return tdl_camera_dev_open(sg_hdl, &cfg);
}

/* ---- 对外 ---------------------------------------------------------------- */

OPERATE_RET app_camera_open(void)
{
    TDL_CAMERA_DEV_INFO_T info = { 0 };
    OPERATE_RET           rt;

    if (sg_open) {
        return OPRT_OK;
    }

    sg_hdl = tdl_camera_find_dev(CAMERA_NAME);
    if (NULL == sg_hdl) {
        PR_ERR(CAM_TAG " 没找到设备 '%s'（CONFIG_TUYA_T5AI_BOARD_CAMERA 开了吗？）",
               CAMERA_NAME);
        return OPRT_NOT_FOUND;
    }

    if (OPRT_OK == tdl_camera_dev_get_info(sg_hdl, &info)) {
        PR_NOTICE(CAM_TAG " 设备：最大 %dx%d @%dfps，格式位图 0x%X，yuv_order=%d",
                  info.max_width, info.max_height, info.max_fps,
                  (unsigned)info.supported_fmts, (int)info.yuv_order);
    }

    /* GC2145 数据手册 P0:0x84[4:0]=0x02：传感器输出 Y Cb Y Cr（YUYV）。
     * YUV BUF 的 bus_dat_byte_reve 已在平台层显式关闭（之前默认反转，导致
     * 内存里变成 Cr Y Cb Y），所以这里按 YUYV 解析。 */
    sg_order = APP_YUV_YUYV;

    /* 缓冲只分配一次，关流不释放（原因见 app_camera_close）。 */
    for (int i = 0; i < 2; i++) {
        if (NULL == sg_buf[i]) {
            sg_buf[i] = (uint16_t *)tal_psram_malloc((size_t)APP_CAM_PREVIEW_W *
                                                     APP_CAM_PREVIEW_H * 2u);
        }
        if (NULL == sg_buf[i]) {
            PR_ERR(CAM_TAG " 预览缓冲分配失败");
            app_camera_close();
            return OPRT_MALLOC_FAILED;
        }
    }

    if (NULL == sg_tmp) {
        sg_tmp = (uint16_t *)tal_psram_malloc((size_t)APP_CAM_TMP_W * APP_CAM_TMP_H * 2u);
    }
    if (NULL == sg_tmp) {
        PR_ERR(CAM_TAG " 旋转暂存分配失败");
        app_camera_close();
        return OPRT_MALLOC_FAILED;
    }

#if APP_CAM_USE_DMA2D
    /* DMA2D 转色用的整幅缓冲（RGB565）。分配失败不致命：预览退回逐像素转色。 */
    if (NULL == sg_rgb) {
        sg_rgb = (uint16_t *)tal_psram_malloc((size_t)APP_CAM_WANT_W *
                                              APP_CAM_WANT_H * 2u);
    }
    if (NULL == sg_rgb) {
        PR_WARN(CAM_TAG " DMA2D 整幅缓冲分配失败，预览走软件转色");
    }
#endif

    if (NULL == sg_bmp) {
        sg_bmp = (uint8_t *)tal_psram_malloc(APP_CAM_BMP_LEN);
    }
    if (NULL == sg_bmp) {
        PR_WARN(CAM_TAG " 照片缓冲分配失败：能预览，不能拍照");
    }

    sg_front           = -1;
    sg_gen             = 0;
    sg_bmp_len         = 0;
    sg_first_frame_log = FALSE;
    sg_fps_cnt         = 0;
    sg_fps_t0          = 0;
    sg_conv_ms_sum     = 0;
    sg_conv_ms_max     = 0;
    sg_last_conv_ms    = 0;

    rt = prv_stream_open();
    if (OPRT_OK != rt) {
        PR_ERR(CAM_TAG " 开流失败 %d", (int)rt);
        app_camera_close();
        return rt;
    }

    sg_open = TRUE;
    PR_NOTICE(CAM_TAG " 已开流：%dx%d @%dfps（预览 YUV422），PSRAM 空闲 %uKB",
              APP_CAM_WANT_W, APP_CAM_WANT_H, APP_CAM_FPS,
              (unsigned)(tal_psram_get_free_heap_size() / 1024));

    /* 画面参数：传感器此刻已经初始化完，把（存过 flash 的）参数下发一遍。
     * 每次开流都下发，是因为关流时寄存器不保证保持。 */
    app_camera_param_apply();

#if APP_CAM_USE_HW_RESIZE
    {
        uint32_t rw = 0;
        uint32_t rh = 0;
        uint8_t *rbuf = NULL;

        if (OPRT_OK == tkl_dvp_get_resize_info(&rw, &rh, &rbuf)) {
            PR_NOTICE(CAM_TAG " 硬件 resize：%ux%u -> %p", (unsigned)rw,
                      (unsigned)rh, rbuf);
        } else {
            PR_WARN(CAM_TAG " 硬件 resize 不可用，回退整幅转换");
        }
    }
#endif

    return OPRT_OK;
}

void app_camera_close(void)
{
    if (NULL != sg_hdl) {
        (void)tdl_camera_dev_close(sg_hdl);
    }
    sg_hdl  = NULL;
    sg_open = FALSE;

    /* **预览缓冲不在这里释放。**
     *
     * 帧回调跑在相机的 raw_flow_task 里，`tdl_camera_dev_close()` 只是停流，
     * 不保证队列里最后一帧的回调已经返回。以前这里直接把 sg_buf/sg_tmp 释放
     * 并置 NULL，回调正好走到 `prv_rotate_90()` 时目标指针就成了 NULL ——
     * 2026-10-20 实测：拍照页按 P12 退出，crash 在 prv_rotate_90 写地址 0
     * （MemFault on raw_flow_task）。预览双缓冲 + 旋转暂存 + DMA2D 整幅缓冲
     * 一共约 1.3MB，留着不释放，回调晚到一步也只是写进一块没人看的旧缓冲，
     * 不会崩。
     *
     * 下次 app_camera_open() 只在指针为 NULL 时才分配。 */
    sg_front    = -1;
    sg_bmp_len  = 0;
    sg_fps_cnt  = 0;
    sg_fps_t0   = 0;
    sg_conv_ms_sum = 0;
    sg_conv_ms_max = 0;
    sg_last_conv_ms = 0;

    /* 相机 TDL 的帧池（VGA 原始 2×600KB + 编码 2×120KB）关流后**不释放**，
     * 这里打的是常驻后的余量，用来判断下次进页面还够不够分配。 */
    PR_NOTICE(CAM_TAG " 已关流，PSRAM 空闲 %uKB（相机帧池常驻）",
              (unsigned)(tal_psram_get_free_heap_size() / 1024));
}

BOOL_T app_camera_is_open(void)
{
    return sg_open;
}

uint32_t app_camera_gen(void)
{
    return sg_gen;
}

BOOL_T app_camera_preview(uint16_t *dst, int w, int h)
{
    int           front = sg_front;
    int           src_w, src_h, x0, y0;
    const uint16_t *src;

    if (!sg_open || front < 0 || NULL == sg_buf[front] || NULL == dst || w <= 0 || h <= 0) {
        return FALSE;
    }
    src = sg_buf[front];

    /* cover：填满取景框（裁掉一点源画面边缘），不留黑边。 */
    src_w = APP_CAM_PREVIEW_W;
    src_h = APP_CAM_PREVIEW_H;
    if ((int64_t)w * src_h > (int64_t)h * src_w) {
        src_h = (int)((int64_t)src_w * h / w);      /* 目标更宽：裁高度 */
    } else {
        src_w = (int)((int64_t)src_h * w / h);      /* 目标更高：裁宽度 */
    }
    if (src_w <= 0 || src_h <= 0) {
        return FALSE;
    }
    x0 = (APP_CAM_PREVIEW_W - src_w) / 2;
    y0 = (APP_CAM_PREVIEW_H - src_h) / 2;

    for (int y = 0; y < h; y++) {
        const uint16_t *row = src + (size_t)(y0 + y * src_h / h) * APP_CAM_PREVIEW_W + x0;
        uint16_t       *out = dst + (size_t)y * w;

        for (int x = 0; x < w; x++) {
            out[x] = row[x * src_w / w];
        }
    }
    return TRUE;
}

/**
 * @brief 拍一张：把**当前预览帧**拼成 BMP 放进内存，不碰流、不碰卡。
 *
 * 为什么不用硬件 JPEG
 * -------------------
 * 硬件 JPEG 只在开流那一刻定档（`TDL_CAMERA_FMT_JPEG_YUV422_BOTH`），而 TDD 的
 * `is_inited` 早退让"关流重开成 JPEG"这条路走不通 —— 实测重开后 DVP 仍在 YUV
 * 模式，等 2 秒也等不到一帧 JPEG（只刷 `sensor's yuyv data resoltion is not
 * right`）。开流就带 JPEG 则会把预览和桌宠拖垮（见 prv_stream_open 注释）。
 *
 * 预览帧本来就是"屏上看到的那幅画"（240×320，已旋转、已裁好），把它按 24 位
 * BMP 存下来最直接：所见即所得，不依赖编码器，也不再有快门时那 2 秒黑屏。
 * 代价是文件大（约 225KB），换掉的是"拍了就有"。
 */
BOOL_T app_camera_grab(void)
{
    const BOOL_T         hd  = (APP_CAM_QUALITY_HD == sg_quality) ? TRUE : FALSE;
    const int            hdr = hd ? APP_CAM_BMP_HDR_24 : APP_CAM_BMP_HDR_16;
    const int            rsz = hd ? APP_CAM_BMP_ROW_24 : APP_CAM_BMP_ROW_16;
    uint8_t             *out;
    const uint16_t      *src;
    const APP_CAM_PET_T *pet = NULL;
    const uint8_t       *pet_px = NULL;
    const uint8_t       *pet_a  = NULL;
    int                  front = sg_front;
    int                  cx = 0, cy = 0;
    uint32_t             t0;

    sg_bmp_len = 0;
    if (!sg_open || front < 0 || NULL == sg_buf[front] || NULL == sg_bmp) {
        return FALSE;
    }
    sg_grab_busy = TRUE;                /* 从这一刻起回调不再翻缓冲（见它的注释） */

    t0  = tal_system_get_millisecond();
    src = sg_buf[front];
    out = sg_bmp + hdr;

    if (sg_comp_on) {
        pet = prv_compose_layout(&cx, &cy);
        if (NULL != pet) {
            pet_px = pet->data;
            pet_a  = pet->data + (size_t)pet->w * (size_t)pet->h * 2u;
        }
    }

    /* 放左边要镜像：原图是在右边缘切过的半身稿，镜像之后那条切边才落在左边缘。 */
    const BOOL_T flip = (APP_CAM_COMPOSE_LEFT == sg_comp_pos) ? TRUE : FALSE;

    /* BMP 自下而上：文件里的第一行是画面的最后一行。 */
    for (int y = 0; y < APP_CAM_PREVIEW_H; y++) {
        const int       py  = APP_CAM_PREVIEW_H - 1 - y;    /* 照片行号（自上而下） */
        const uint16_t *row = src + (size_t)py * APP_CAM_PREVIEW_W;
        uint8_t        *dst = out + (size_t)y * rsz;
        const BOOL_T    in_cy = (NULL != pet_px && py >= cy && py < cy + pet->h) ? TRUE : FALSE;

        for (int x = 0; x < APP_CAM_PREVIEW_W; x++) {
            uint16_t px = row[x];

            /* 合影：立绘按 alpha 混进来（不是盖一块方图）。 */
            if (in_cy && x >= cx && x < cx + pet->w) {
                int      sx = x - cx;
                size_t   i;
                uint16_t s;

                if (flip) {
                    sx = pet->w - 1 - sx;
                }
                i = (size_t)(py - cy) * (size_t)pet->w + (size_t)sx;
                s = (uint16_t)pet_px[i * 2] | ((uint16_t)pet_px[i * 2 + 1] << 8);

                px = prv_blend565(px, s, pet_a[i]);
            }

            /* 照片和预览用同一套白平衡增益，存出来的和屏上看到的一致。 */
            px = prv_wb_px(px);

            if (hd) {
                uint8_t r5 = (uint8_t)((px >> 11) & 0x1F);
                uint8_t g6 = (uint8_t)((px >> 5) & 0x3F);
                uint8_t b5 = (uint8_t)(px & 0x1F);

                /* BMP 像素是 BGR；565 拉到 888 用"高位复制"补低位，比左移纯 0 更亮一点。 */
                *dst++ = (uint8_t)((b5 << 3) | (b5 >> 2));
                *dst++ = (uint8_t)((g6 << 2) | (g6 >> 4));
                *dst++ = (uint8_t)((r5 << 3) | (r5 >> 2));
            } else {
                /* 16 位就是 RGB565 本身，小端两个字节。 */
                *dst++ = (uint8_t)(px & 0xFF);
                *dst++ = (uint8_t)(px >> 8);
            }
        }
    }

    prv_bmp_header(sg_bmp, (uint32_t)(rsz * APP_CAM_PREVIEW_H), sg_quality);
    sg_bmp_len = (uint32_t)hdr + (uint32_t)rsz * APP_CAM_PREVIEW_H;

    sg_grab_busy = FALSE;               /* 缓冲不用再护着了 */

    PR_NOTICE(CAM_TAG " 已拍照 %dx%d（BMP %u 位 %uKB%s，%u ms）",
              APP_CAM_PREVIEW_W, APP_CAM_PREVIEW_H,
              hd ? 24u : 16u, (unsigned)(sg_bmp_len / 1024),
              (NULL != pet_px) ? "，含角色合影" : "",
              (unsigned)(tal_system_get_millisecond() - t0));
    return TRUE;
}

void app_camera_set_quality(APP_CAM_QUALITY_E q)
{
    sg_quality = (APP_CAM_QUALITY_HD == q) ? APP_CAM_QUALITY_HD : APP_CAM_QUALITY_STANDARD;
}

void app_camera_set_compose(BOOL_T on, int pos)
{
    sg_comp_on  = (TRUE == on) ? TRUE : FALSE;
    sg_comp_pos = (pos < APP_CAM_COMPOSE_LEFT || pos > APP_CAM_COMPOSE_RIGHT)
                      ? APP_CAM_COMPOSE_RIGHT
                      : pos;
}

BOOL_T app_camera_save(char *path, size_t path_sz)
{
    if (NULL != path && path_sz > 0) {
        path[0] = '\0';
    }
    if (0 == sg_bmp_len) {
        return FALSE;
    }
    if (!prv_save_shot(path, path_sz)) {
        return FALSE;
    }

    /* 存过就消费掉：同一张照片不能被"保存到相册"按两次写两份。 */
    sg_bmp_len = 0;
    return TRUE;
}

void app_camera_discard(void)
{
    sg_bmp_len = 0;
}

/* ---- 画面参数（调色）------------------------------------------------------
 *
 * 参数就是 GC2145 的寄存器原值，表里每一行写明页/地址，改一个数就写一个寄存器。
 * 取值依据是数据手册（`GC2145 CSP DataSheet`）：
 *
 *   P2:0xd5 Luma_offset     "Add offset on luma value. S7."      → 亮度
 *   P2:0xd3 luma_contrast   "2.6bits, 0x40 = 1.0"                → 对比度
 *   P2:0xd1/d2 Cb/Cr sat    "3.5bits, 0x20 = 1.0"                → 饱和度
 *   P1:0x13 AEC_target_Y    "expected luminance value"           → 曝光目标
 *
 * 注意：另一份厂商参考代码（XRADIO 的 drv_gc2145.c）把亮度/对比度写在 P0:0x55/
 * 0x56 上，但数据手册里 P0 根本没有这两个寄存器 —— 那套是别的传感器留下的，
 * 照抄会静默无效。这里按手册的 P2 YCP 块来。
 */

typedef struct {
    const char *name;
    uint8_t     page;
    uint8_t     reg;
    uint8_t     reg2;      /* 0 = 只有一个寄存器（饱和度要写 Cb/Cr 两个） */
    int16_t     def;
    int16_t     min;
    int16_t     max;
} APP_CAM_PARAM_T;

static const APP_CAM_PARAM_T sg_param_tab[APP_CAM_PARAM_COUNT] = {
    /* bright   */ { "bright",   2, 0xd5, 0x00, 0x00, -64,   63  },
    /* contrast */ { "contrast", 2, 0xd3, 0x00, 0x40, 0x20,  0x60 },
    /* sat      */ { "sat",      2, 0xd1, 0xd2, 0x32, 0x10,  0x50 },
    /* aec      */ { "aec",      1, 0x13, 0x00, 0x7b, 0x20,  0xc0 },
    /* wbr      */ { "wbr",      0xFF, 0xFF, 0x00, 100,  50,   200 },  /* 像素层，不写寄存器 */
    /* wbb      */ { "wbb",      0xFF, 0xFF, 0x00, 100,  50,   200 },
};

static int    sg_param_val[APP_CAM_PARAM_COUNT];
static BOOL_T sg_param_ready;

/** 白平衡增益查表：RGB565 的 5 位 R / 5 位 B 各一张（32 项），改参数时重建。 */
static uint16_t sg_wb_lut_r[32];
static uint16_t sg_wb_lut_b[32];

static void prv_wb_lut_build(void)
{
    int rp = sg_param_val[APP_CAM_PARAM_WB_R];
    int bp = sg_param_val[APP_CAM_PARAM_WB_B];

    for (int i = 0; i < 32; i++) {
        int r = (i * rp + 50) / 100;
        int b = (i * bp + 50) / 100;

        sg_wb_lut_r[i] = (uint16_t)((r > 31) ? 31 : r);
        sg_wb_lut_b[i] = (uint16_t)((b > 31) ? 31 : b);
    }
}

/** 把一个 RGB565 像素按白平衡增益修正一次。R/B 都是 100% 时原样返回。 */
static uint16_t prv_wb_px(uint16_t px)
{
    return (uint16_t)((sg_wb_lut_r[(px >> 11) & 0x1F] << 11) |
                      (px & 0x07E0) |
                      sg_wb_lut_b[px & 0x1F]);
}

static void prv_param_load(void);

static void prv_param_ensure(void)
{
    if (sg_param_ready) {
        return;
    }
    for (int i = 0; i < APP_CAM_PARAM_COUNT; i++) {
        sg_param_val[i] = sg_param_tab[i].def;
    }
    sg_param_ready = TRUE;
    prv_param_load();
    prv_wb_lut_build();                 /* 白平衡查表跟着内存值走 */
}

static int prv_param_clamp(APP_CAM_PARAM_E p, int v)
{
    const APP_CAM_PARAM_T *t = &sg_param_tab[p];

    if (v < t->min) {
        return t->min;
    }
    if (v > t->max) {
        return t->max;
    }
    return v;
}

int app_camera_param_get(APP_CAM_PARAM_E p)
{
    if ((int)p < 0 || (int)p >= (int)APP_CAM_PARAM_COUNT) {
        return 0;
    }
    prv_param_ensure();
    return sg_param_val[p];
}

int app_camera_param_min(APP_CAM_PARAM_E p)
{
    return ((int)p >= 0 && (int)p < (int)APP_CAM_PARAM_COUNT) ? sg_param_tab[p].min : 0;
}

int app_camera_param_max(APP_CAM_PARAM_E p)
{
    return ((int)p >= 0 && (int)p < (int)APP_CAM_PARAM_COUNT) ? sg_param_tab[p].max : 0;
}

const char *app_camera_param_name(APP_CAM_PARAM_E p)
{
    return ((int)p >= 0 && (int)p < (int)APP_CAM_PARAM_COUNT) ? sg_param_tab[p].name : "?";
}

BOOL_T app_camera_param_find(const char *name, APP_CAM_PARAM_E *out)
{
    if (NULL == name || NULL == out) {
        return FALSE;
    }
    for (int i = 0; i < APP_CAM_PARAM_COUNT; i++) {
        if (0 == strcmp(name, sg_param_tab[i].name)) {
            *out = (APP_CAM_PARAM_E)i;
            return TRUE;
        }
    }
    return FALSE;
}

OPERATE_RET app_camera_reg_write(uint8_t page, uint8_t reg, uint8_t val)
{
    return tdd_camera_gc2145_reg_write(page, reg, val);
}

OPERATE_RET app_camera_reg_read(uint8_t page, uint8_t reg, uint8_t *val)
{
    return tdd_camera_gc2145_reg_read(page, reg, val);
}

static void prv_param_apply_one(APP_CAM_PARAM_E p)
{
    const APP_CAM_PARAM_T *t = &sg_param_tab[p];
    uint8_t                v = (uint8_t)sg_param_val[p];

    /* 像素层参数（白平衡增益）不写寄存器，只重建查表。 */
    if (0xFF == t->page) {
        prv_wb_lut_build();
        return;
    }

    (void)tdd_camera_gc2145_reg_write(t->page, t->reg, v);
    if (0 != t->reg2) {
        (void)tdd_camera_gc2145_reg_write(t->page, t->reg2, v);
    }
}

void app_camera_param_apply(void)
{
    uint8_t y = 0;

    prv_param_ensure();
    for (int i = 0; i < APP_CAM_PARAM_COUNT; i++) {
        prv_param_apply_one((APP_CAM_PARAM_E)i);
    }
    /* 页复位到 P0：驱动后续按自己的表写寄存器，别让它以为还在 P1/P2。 */
    (void)tdd_camera_gc2145_reg_write(0, 0xfe, 0x00);

    (void)tdd_camera_gc2145_reg_read(1, 0x14, &y);      /* AEC 当前帧平均亮度 */

    PR_NOTICE(CAM_TAG " 画面参数：bright=%d contrast=%d sat=%d aec=%d（实测 Y=%u）",
              sg_param_val[APP_CAM_PARAM_BRIGHTNESS],
              sg_param_val[APP_CAM_PARAM_CONTRAST],
              sg_param_val[APP_CAM_PARAM_SATURATION],
              sg_param_val[APP_CAM_PARAM_AEC_TARGET], (unsigned)y);
}

void app_camera_param_set(APP_CAM_PARAM_E p, int val)
{
    if ((int)p < 0 || (int)p >= (int)APP_CAM_PARAM_COUNT) {
        return;
    }
    prv_param_ensure();

    sg_param_val[p] = prv_param_clamp(p, val);

    /* 传感器初始化过一次之后，I2C 就一直可用（关流只停 DVP，不拆 I2C），
     * 所以这里不看 sg_open —— 在设置页里调，下次开流立刻就是新的。 */
    prv_param_apply_one(p);
}

void app_camera_param_reset(void)
{
    for (int i = 0; i < APP_CAM_PARAM_COUNT; i++) {
        sg_param_val[i] = sg_param_tab[i].def;
    }
    sg_param_ready = TRUE;

    app_camera_param_apply();
    PR_NOTICE(CAM_TAG " 画面参数已恢复默认");
}

void app_camera_param_save(void)
{
    char buf[64];
    int  n = 0;

    prv_param_ensure();

    buf[0] = '\0';
    for (int i = 0; i < APP_CAM_PARAM_COUNT && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s=%d",
                      (0 == i) ? "" : ",", sg_param_tab[i].name, sg_param_val[i]);
    }

    if (OPRT_OK == app_settings_set_str(APP_SET_CAM_TUNE, buf)) {
        PR_NOTICE(CAM_TAG " 画面参数已存：%s", buf);
    } else {
        PR_WARN(CAM_TAG " 画面参数存不进去");
    }
}

static void prv_param_load(void)
{
    char        buf[64];
    const char *s;

    if (OPRT_OK != app_settings_get_str(APP_SET_CAM_TUNE, buf, sizeof(buf))) {
        return;
    }

    s = buf;
    while ('\0' != *s) {
        char name[16];
        int  n = 0;
        int  val;

        while ('\0' != *s && '=' != *s && ',' != *s && n < (int)sizeof(name) - 1) {
            name[n++] = *s++;
        }
        name[n] = '\0';

        if ('=' != *s) {
            break;
        }
        s++;
        {
            char *end = NULL;
            val = (int)strtol(s, &end, 0);          /* 十进制或 0x 都认 */
            s = (NULL != end) ? end : (s + strlen(s));
        }
        while (',' == *s) {
            s++;
        }

        if (n > 0) {
            APP_CAM_PARAM_E p;

            if (app_camera_param_find(name, &p)) {
                sg_param_val[p] = prv_param_clamp(p, val);
            }
        }
    }
}

/* ---- overlay：预览直接写进显示 framebuffer -------------------------------- */

/* SDK 显示端口的 pre-flush 钩子（lv_port_disp_internal.h，应用不直接包含它）。
 * 第二个参数是本次刷新 LVGL 重绘的区域（逻辑坐标）—— 合成用它判断"这次有没有
 * 人画到取景框上"，没画到就跳过重画。 */
typedef void (*APP_CAM_PRE_FLUSH_CB)(TDL_DISP_FRAME_BUFF_T *fb, const lv_area_t *area);
extern void lv_port_set_pre_flush_cb(APP_CAM_PRE_FLUSH_CB cb);

/* 缓存回写：CPU 直接写 fb 之后，必须把这段刷回 PSRAM，DMA/面板才能看到。 */
extern void flush_dcache(void *va, long size);

#define APP_CAM_OV_MAX  320

static BOOL_T   sg_ov_on;
static uint32_t sg_ov_drawn_gen;            /* 上一次画进 fb 的帧代数 */
static int      sg_ov_ox, sg_ov_oy;         /* 逻辑原点对应的面板点 */
static int      sg_ov_ax, sg_ov_ay;         /* 逻辑 +x 在面板上的单位步进 */
static int      sg_ov_bx, sg_ov_by;         /* 逻辑 +y 在面板上的单位步进 */
static int      sg_ov_lx, sg_ov_ly;         /* 取景框的逻辑原点（脏矩形反查用） */
static int      sg_ov_px0, sg_ov_py0;       /* 面板包围盒（未钳制） */
static int      sg_ov_px1, sg_ov_py1;
static int      sg_ov_w, sg_ov_h;           /* 逻辑尺寸 */
static BOOL_T   sg_ov_paused;               /* 弹窗盖在取景框上时暂停合成 */
static uint16_t sg_ov_mx[APP_CAM_OV_MAX];   /* 逻辑 x -> 预览 x */
static uint16_t sg_ov_my[APP_CAM_OV_MAX];   /* 逻辑 y -> 预览 y */

/* 圆角：角方块里的覆盖率表（0..255）。只算一次，合成时查表。
 * 半径上限取 40 —— 卡片圆角是 20（PET_RADIUS_CARD），留一倍余量。 */
#define APP_CAM_OV_RAD_MAX   40
static int     sg_ov_rad;
static uint8_t sg_ov_cov[APP_CAM_OV_RAD_MAX][APP_CAM_OV_RAD_MAX];

/**
 * 本次刷新的脏矩形（逻辑坐标）有没有压到取景框上。
 *
 * 映射和合成用的是同一组仿射基：逻辑点 (lx,ly) -> 面板
 *   px = ox + (lx-lx0)*ax + (ly-ly0)*bx
 *   py = oy + (lx-lx0)*ay + (ly-ly0)*by
 * 取脏矩形两个对角点各映射一次、取包围盒，再和取景框的面板包围盒求交即可。
 * **只是区域判定，不参与像素寻址** —— 合成那块算法一个字没动。
 */
static BOOL_T prv_ov_area_hit(const lv_area_t *area)
{
    const int INF = 0x7FFFFFFF;
    int       lxs[2], lys[2];
    int       px0 = INF, py0 = INF, px1 = -INF, py1 = -INF;
    int       i, j;

    if (NULL == area) {
        return TRUE;                    /* 没给区域：保守当命中 */
    }
    lxs[0] = area->x1;
    lxs[1] = area->x2;
    lys[0] = area->y1;
    lys[1] = area->y2;

    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            int dx = lxs[i] - sg_ov_lx;
            int dy = lys[j] - sg_ov_ly;
            int px = sg_ov_ox + dx * sg_ov_ax + dy * sg_ov_bx;
            int py = sg_ov_oy + dx * sg_ov_ay + dy * sg_ov_by;

            if (px < px0) px0 = px;
            if (px > px1) px1 = px;
            if (py < py0) py0 = py;
            if (py > py1) py1 = py;
        }
    }
    return (px1 < sg_ov_px0 || px0 > sg_ov_px1 ||
            py1 < sg_ov_py0 || py0 > sg_ov_py1) ? FALSE : TRUE;
}

/** RGB565 按覆盖率与已有像素混合（overlay 的圆角边缘用）。
 *  名字带 ov_：文件里已经有一个 prv_blend565（合影用，参数是 0..255 的 alpha）。 */
static uint16_t prv_ov_blend565(uint16_t dst, uint16_t src, int cov)
{
    int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    int r  = (sr * cov + dr * (255 - cov)) / 255;
    int g  = (sg * cov + dg * (255 - cov)) / 255;
    int b  = (sb * cov + db * (255 - cov)) / 255;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/**
 * pre-flush 回调：跑在 LVGL 线程的 flush 里，只做内存拷贝，不碰 LVGL API。
 *
 * **现在是"按需重画"**（2026-10-21）：
 *
 *   1) 有新相机帧（帧代数变了）   -> 整块重画；
 *   2) 没有新帧，但本次刷新 LVGL 确实画到了取景框区域 -> 也整块重画；
 *   3) 两者都不满足               -> 直接返回，framebuffer 里保留上一次的合成结果。
 *
 * 为什么以前是"每次 flush 都重画"：LVGL 只要在取景框里重绘了任何东西（按返回键
 * 时按钮的按下态），fb 就被刷回页面底色，而预览要等下一个相机帧（≤100ms）才补
 * 回来 —— 用户看到"点返回，画面上闪一下黑块"。第 2 条就是治它的，作用和当初
 * 一样，但只在该重画时才付代价。
 *
 * 代价对比：相机 10fps、页面刷新 30+fps，以前 2/3 的合成在重复画同一帧；
 * 每次约 15ms（40K 像素 PSRAM 读+写），显示线程近一半时间耗在这里。现在
 * 常态下每秒只合成 10 次。
 */
static void prv_overlay_cb(TDL_DISP_FRAME_BUFF_T *fb, const lv_area_t *area)
{
    const uint16_t *src;
    uint16_t       *dst;
    int             front;
    int             fw;
    int             px0, px1, py0, py1;

    if (!sg_ov_on || sg_ov_paused || NULL == fb || NULL == fb->frame) {
        return;
    }
    front = sg_front;
    if (front < 0 || NULL == sg_buf[front]) {
        return;
    }

    /* 没有新帧、这次刷新也没画到取景框上：不必重做合成。 */
    if (sg_gen == sg_ov_drawn_gen && !prv_ov_area_hit(area)) {
        sg_ov_skip++;
        return;
    }
    sg_ov_drawn_gen = sg_gen;
    sg_ov_draw_cnt++;

    src = sg_buf[front];
    dst = (uint16_t *)fb->frame;
    fw  = (int)fb->width;

    /* 面板包围盒：启用时按四个角点算好并保存，这里只做屏幕钳制。 */
    px0 = sg_ov_px0;
    py0 = sg_ov_py0;
    px1 = sg_ov_px1;
    py1 = sg_ov_py1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 >= fw) px1 = fw - 1;
    if (px1 < px0 || py1 < py0) {
        return;
    }

    /* 逐面板像素反查逻辑坐标：面板行连续写，源图是跨行读（可接受）。
     *
     * (ax,ay)/(bx,by) 是正交单位基，点积即反解。px 每 +1，lx/ly 就各走一个常量
     * 步进，所以两个乘法提到行首算一次，内层只剩加法和比较。 */
    for (int py = py0; py <= py1; py++) {
        uint16_t *drow = dst + (size_t)py * fw + px0;
        int       dx0  = px0 - sg_ov_ox;
        int       dy   = py - sg_ov_oy;
        int       lx   = dx0 * sg_ov_ax + dy * sg_ov_ay;
        int       ly   = dx0 * sg_ov_bx + dy * sg_ov_by;

        for (int px = px0; px <= px1; px++, lx += sg_ov_ax, ly += sg_ov_bx) {
            if (lx >= 0 && lx < sg_ov_w && ly >= 0 && ly < sg_ov_h) {
                /* 白平衡微调在这里做：写进 fb 的每个像素都过一遍 R/B 增益查表。 */
                uint16_t sp = prv_wb_px(
                    src[(size_t)sg_ov_my[ly] * APP_CAM_PREVIEW_W + sg_ov_mx[lx]]);

                /* 圆角：只处理四个角的 r×r 方块，其余像素一次比较就跳过。 */
                if (sg_ov_rad > 0) {
                    int u = -1;
                    int v = -1;

                    if (px < px0 + sg_ov_rad) {
                        u = px - px0;
                    } else if (px > px1 - sg_ov_rad) {
                        u = px1 - px;
                    }
                    if (py < py0 + sg_ov_rad) {
                        v = py - py0;
                    } else if (py > py1 - sg_ov_rad) {
                        v = py1 - py;
                    }

                    if (u >= 0 && v >= 0) {
                        int cov = sg_ov_cov[v][u];

                        if (0 == cov) {
                            continue;           /* 圆外：保留 fb 里的卡片底色 */
                        }
                        if (cov < 255) {
                            sp = prv_ov_blend565(drow[px - px0], sp, cov);
                        }
                    }
                }
                drow[px - px0] = sp;
            }
        }
        /* 这一行已经写完整，刷回 PSRAM，供 DMA/面板读取。 */
        flush_dcache(drow, (long)(px1 - px0 + 1) * 2);
    }
}

BOOL_T app_camera_overlay_enable(int x, int y, int w, int h, int radius)
{
    lv_display_t *disp = lv_display_get_default();
    lv_area_t     a, b, c;
    int           src_w, src_h, x0, y0;

    if (NULL == disp || w <= 0 || h <= 0 || w > APP_CAM_OV_MAX || h > APP_CAM_OV_MAX) {
        return FALSE;
    }
    if (w < 2 || h < 2 || 0 == (w - 1) || 0 == (h - 1)) {
        return FALSE;
    }

    /* 圆角覆盖率表：角方块里每个像素被圆盖住多少（0..255），只建一次。
     * 圆心取 (radius, radius)，像素中心在 (u+0.5, v+0.5)，按"半径 - 距离 + 0.5"
     * 算覆盖率 —— 和 LVGL 自己的抗锯齿同一套做法。 */
    if (radius < 0 || radius > APP_CAM_OV_RAD_MAX || radius * 2 >= w || radius * 2 >= h) {
        radius = 0;
    }
    sg_ov_rad = radius;
    if (radius > 0) {
        for (int v = 0; v < radius; v++) {
            for (int u = 0; u < radius; u++) {
                float dx = (float)radius - ((float)u + 0.5f);
                float dy = (float)radius - ((float)v + 0.5f);
                float d  = sqrtf(dx * dx + dy * dy);
                float cv = (float)radius - d + 0.5f;

                if (cv < 0.0f) {
                    cv = 0.0f;
                } else if (cv > 1.0f) {
                    cv = 1.0f;
                }
                sg_ov_cov[v][u] = (uint8_t)(cv * 255.0f + 0.5f);
            }
        }
    }

    /* 用 LVGL 自己的旋转把三个角点映射到面板坐标，避免手写 90° 公式。 */
    a = (lv_area_t){ .x1 = x,         .y1 = y,         .x2 = x,         .y2 = y         };
    b = (lv_area_t){ .x1 = x + w - 1, .y1 = y,         .x2 = x + w - 1, .y2 = y         };
    c = (lv_area_t){ .x1 = x,         .y1 = y + h - 1, .x2 = x,         .y2 = y + h - 1 };
    lv_display_rotate_area(disp, &a);
    lv_display_rotate_area(disp, &b);
    lv_display_rotate_area(disp, &c);

    sg_ov_ox = a.x1;
    sg_ov_oy = a.y1;
    sg_ov_ax = (b.x1 - a.x1) / (w - 1);
    sg_ov_ay = (b.y1 - a.y1) / (w - 1);
    sg_ov_bx = (c.x1 - a.x1) / (h - 1);
    sg_ov_by = (c.y1 - a.y1) / (h - 1);
    sg_ov_lx = x;
    sg_ov_ly = y;
    sg_ov_w  = w;
    sg_ov_h  = h;
    sg_ov_skip = 0;

    /* 面板包围盒：四个角点映射一遍取 min/max，合成时直接用（每帧都要）。 */
    {
        int cx[4], cy[4];

        cx[0] = sg_ov_ox;
        cy[0] = sg_ov_oy;
        cx[1] = sg_ov_ox + (w - 1) * sg_ov_ax;
        cy[1] = sg_ov_oy + (w - 1) * sg_ov_ay;
        cx[2] = sg_ov_ox + (h - 1) * sg_ov_bx;
        cy[2] = sg_ov_oy + (h - 1) * sg_ov_by;
        cx[3] = cx[1] + (h - 1) * sg_ov_bx;
        cy[3] = cy[1] + (h - 1) * sg_ov_by;

        sg_ov_px0 = sg_ov_px1 = cx[0];
        sg_ov_py0 = sg_ov_py1 = cy[0];
        for (int i = 1; i < 4; i++) {
            if (cx[i] < sg_ov_px0) sg_ov_px0 = cx[i];
            if (cx[i] > sg_ov_px1) sg_ov_px1 = cx[i];
            if (cy[i] < sg_ov_py0) sg_ov_py0 = cy[i];
            if (cy[i] > sg_ov_py1) sg_ov_py1 = cy[i];
        }
    }

    /* cover 映射：逻辑矩形 -> 240×320 预览（比例不同就裁，不拉伸）。 */
    src_w = APP_CAM_PREVIEW_W;
    src_h = (int)((int64_t)src_w * h / w);
    if (src_h > APP_CAM_PREVIEW_H) {
        src_h = APP_CAM_PREVIEW_H;
        src_w = (int)((int64_t)src_h * w / h);
    }
    if (src_w <= 0 || src_h <= 0) {
        return FALSE;
    }
    x0 = (APP_CAM_PREVIEW_W - src_w) / 2;
    y0 = (APP_CAM_PREVIEW_H - src_h) / 2;
    for (int i = 0; i < w; i++) {
        sg_ov_mx[i] = (uint16_t)(x0 + i * src_w / w);
    }
    for (int i = 0; i < h; i++) {
        sg_ov_my[i] = (uint16_t)(y0 + i * src_h / h);
    }

    sg_ov_drawn_gen = sg_gen;
    sg_ov_paused    = FALSE;
    sg_ov_on        = TRUE;
    lv_port_set_pre_flush_cb(prv_overlay_cb);

    PR_NOTICE(CAM_TAG " overlay 已启用：逻辑 %d,%d %dx%d -> 面板 %d,%d 步进(%d,%d)/(%d,%d)",
              x, y, w, h, sg_ov_ox, sg_ov_oy,
              sg_ov_ax, sg_ov_ay, sg_ov_bx, sg_ov_by);
    return TRUE;
}

void app_camera_overlay_pause(BOOL_T pause)
{
    if (!sg_ov_on) {
        return;
    }

    pause = (TRUE == pause) ? TRUE : FALSE;
    if (sg_ov_paused == pause) {
        return;
    }
    sg_ov_paused = pause;

    if (!pause) {
        /* 恢复的第一拍：哪怕暂停期间没有新帧，也要补画一次，否则要等下一次
         * 相机出帧（最长 100ms）才回来，中间那一片是空的。 */
        sg_ov_drawn_gen = sg_gen - 1;
    }
}

void app_camera_overlay_disable(void)
{
    if (!sg_ov_on) {
        return;
    }
    sg_ov_on     = FALSE;
    sg_ov_paused = FALSE;
    lv_port_set_pre_flush_cb(NULL);
}
