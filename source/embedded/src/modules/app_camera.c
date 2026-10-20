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
 *    降到 240×240 后可以再试。
 * 2) **按 cover 裁，不拉伸**：摄像头 4:3、取景框不是，硬缩会把画面拉长。
 * 3) 帧回调里**只做内存操作**：写 SD 会把 SDIO 堵住、把出帧队列顶爆。
 * 4) **双缓冲**：回调写后缓冲、写完再翻 `sg_front`；LVGL 读前缓冲，互不打架。
 * 5) **缓冲关流不释放**：回调可能在关流后还在跑最后一帧，释放会踩空指针。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"
#include "tal_image_yuv422_to_rgb.h"    /* YUV422->RGB565：有 DMA2D 时走硬件 */

#include "tdl_camera_manage.h"

#include "app_camera.h"
#include "app_storage.h"
#include "app_time.h"

/* 板级 Kconfig 定的设备名（src/peripherals/camera/Kconfig，默认 "camera"）。 */
#ifndef CAMERA_NAME
#define CAMERA_NAME         "camera"
#endif

/* JPEG 暂存上限（`encoded_quality.jpeg_cfg.max_size` 要的是 25KB，留足余量）。 */
#define APP_CAM_JPEG_MAX    (256 * 1024)

/* 快门等一张完整 JPEG 的上限。出帧 15fps，正常 100ms 内就到。 */
#define APP_CAM_CAP_WAIT_MS 2000

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

static TUYA_YUV422_ORDER_E sg_order = TUYA_YUV422_UYVY;

/* 快门：主线程置 req，JPEG 回调拷完置 done。 */
static volatile BOOL_T   sg_cap_req;
static volatile BOOL_T   sg_cap_done;
static uint8_t          *sg_jpeg;
static volatile uint32_t sg_jpeg_len;

/* ---- YUV422 -> RGB565（cover 裁切 + 最近邻缩放） -------------------------- */

/** 横图暂存尺寸：和源图一样大（240×240），转 90° 后还是 240×240。 */
#define APP_CAM_TMP_W       240
#define APP_CAM_TMP_H       240

/* 1 = YUV->RGB565 走 DMA2D 硬件（整幅转色后再软件缩）；0 = 纯软件逐像素。
 *
 * 2026-10-20 实测：VGA 下打开 DMA2D 反而更慢（转换 70~112ms、帧率掉到 6fps），
 * 而且每帧 600KB 的整幅写回 EMC 会和 DVP/面板抢总线，FIFO 溢出更凶。降到
 * 240×240 后数据量是原来的 1/4，先用软件路径；要再压 CPU 可以把这个改回 1
 * （此时整幅缓冲只有 115KB，总线压力小得多）。 */
#define APP_CAM_USE_DMA2D   0

/* 列/行映射表：每帧只建一次（320+240 次），内层循环变成查表。
 * 两个方向都会用到（横图 320x240 与竖图 240x320），按最大边开。 */
#define APP_CAM_MAP_MAX     320
static uint16_t s_xmap[APP_CAM_MAP_MAX];
static uint16_t s_ymap[APP_CAM_MAP_MAX];

/**
 * @brief 一帧 YUV422 裁成目标宽高比、最近邻缩到 @a dw × @a dh。
 *
 * YUV422 每像素 2 字节，两个像素共用一组 UV：
 *   UYVY（T5AI DVP 默认）: U0 Y0 V0 Y1
 *   YUYV                 : Y0 U0 Y1 V0
 * 取某个源像素的 Y 要按它自己的奇偶挑字节。
 */
static void prv_yuv422_cover_to_rgb565(const uint8_t *src, int sw, int sh,
                                       uint16_t *dst, int dw, int dh,
                                       TUYA_YUV422_ORDER_E order)
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

    const BOOL_T yuyv   = (order == TUYA_YUV422_YUYV);
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

            if (yuyv) {
                yv = p[(sx & 1) ? 2 : 0];
                u  = p[1];
                v  = p[3];
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
#if APP_CAM_USE_DMA2D
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
#endif /* APP_CAM_USE_DMA2D */

/**
 * @brief 90° 旋转：src (sw × sh) -> dst (sh × sw)。
 *
 * 行内读写都是连续的（CW：读反向、写正向；CCW：读跨行、写正向），不用逐像素
 * 算坐标。摄像头模组是转着装的，预览必须转这 90°。
 */
static void prv_rotate_90(const uint16_t *src, int sw, int sh, uint16_t *dst, BOOL_T cw)
{
    if (cw) {
        for (int y = 0; y < sw; y++) {          /* dst 行数 = src 宽 */
            const uint16_t *row = src + (size_t)y * sw;
            uint16_t       *out = dst + (size_t)y * sh;

            for (int x = 0; x < sh; x++) {
                out[x] = row[sh - 1 - x];
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

#if APP_CAM_USE_DMA2D
            if (NULL != rgb && frame->width > 0 && frame->height > 0 &&
                frame->width <= APP_CAM_WANT_W && frame->height <= APP_CAM_WANT_H) {
                TAL_IMAGE_YUV422_TO_RGB_T cfg = { 0 };

                cfg.in_buf     = frame->data;
                cfg.in_width   = frame->width;
                cfg.in_height  = frame->height;
                cfg.out_buf    = (uint8_t *)rgb;
                cfg.out_width  = frame->width;
                cfg.out_height = frame->height;
                cfg.in_order   = sg_order;

                if (OPRT_OK == tal_image_convert_yuv422_to_rgb565(&cfg)) {
                    prv_rgb565_cover_scale(rgb, frame->width, frame->height,
                                           tmp, APP_CAM_TMP_W, APP_CAM_TMP_H);
                    converted = TRUE;
                }
            }
#endif

            if (!converted) {
                prv_yuv422_cover_to_rgb565(frame->data, frame->width, frame->height,
                                           tmp, APP_CAM_TMP_W, APP_CAM_TMP_H, sg_order);
            }

            prv_rotate_90(tmp, APP_CAM_TMP_W, APP_CAM_TMP_H, dst,
                          (BOOL_T)APP_CAM_ROTATE_CW);

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

            PR_DEBUG(CAM_TAG " 预览 %u 帧/%ums（约 %u fps），转换 平均 %u ms 最大 %u ms",
                     (unsigned)sg_fps_cnt, (unsigned)ms,
                     (unsigned)(sg_fps_cnt * 1000u / ms),
                     (unsigned)(sg_conv_ms_sum / sg_fps_cnt),
                     (unsigned)sg_conv_ms_max);
            sg_fps_cnt     = 0;
            sg_fps_t0      = now;
            sg_conv_ms_sum = 0;
            sg_conv_ms_max = 0;
        }
    }
    return OPRT_OK;
}

static OPERATE_RET prv_jpg_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    uint32_t n;

    (void)hdl;

    if (NULL == frame || NULL == frame->data || 0 == frame->data_len) {
        return OPRT_INVALID_PARM;
    }
    /* 只有按下快门那次需要 JPEG；平时完整帧也来了，但直接丢掉，省一次拷贝。 */
    if (!sg_cap_req || !frame->is_complete || NULL == sg_jpeg || sg_cap_done) {
        return OPRT_OK;
    }

    n = frame->data_len;
    if (n > APP_CAM_JPEG_MAX) {
        PR_WARN(CAM_TAG " JPEG 超上限 %u > %d，截断", (unsigned)n, APP_CAM_JPEG_MAX);
        n = APP_CAM_JPEG_MAX;
    }
    memcpy(sg_jpeg, frame->data, n);
    sg_jpeg_len = n;
    sg_cap_done = TRUE;
    return OPRT_OK;
}

/* ---- 存卡 ---------------------------------------------------------------- */

/** 拼一个还没被占用的路径：时间对上就用时间戳，否则 cam / cam_2 / … */
static void prv_make_path(char *out, size_t out_sz)
{
    char stamp[32] = { 0 };

    if (app_time_valid() &&
        OPRT_OK == app_time_format(stamp, sizeof(stamp), "%Y%m%d_%H%M%S") &&
        '\0' != stamp[0] && '-' != stamp[0]) {
        snprintf(out, out_sz, "%s/%s/cam_%s.jpg", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR, stamp);
        return;
    }

    for (int i = 1; i <= 999; i++) {
        char   name[32];
        char   path[128];
        BOOL_T ex = FALSE;

        if (1 == i) {
            snprintf(name, sizeof(name), "cam.jpg");
        } else {
            snprintf(name, sizeof(name), "cam_%d.jpg", i);
        }
        snprintf(path, sizeof(path), "%s/%s/%s", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR, name);
        if (OPRT_OK != tkl_fs_is_exist(path, &ex) || !ex) {
            snprintf(out, out_sz, "%s", path);
            return;
        }
    }
    snprintf(out, out_sz, "%s/%s/cam_999.jpg", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR);
}

static BOOL_T prv_save_jpeg(char *path, size_t path_sz)
{
    char      dir[64];
    char      full[160];
    TUYA_FILE f;

    if (0 == sg_jpeg_len || NULL == sg_jpeg) {
        return FALSE;
    }

    snprintf(dir, sizeof(dir), "%s/%s", APP_STORAGE_MOUNT, APP_CAM_PIC_DIR);
    (void)tkl_fs_mkdir(dir);        /* 已存在时返回非 0，是预期内的 */

    prv_make_path(full, sizeof(full));

    f = tkl_fopen(full, "w");
    if (NULL == f) {
        PR_ERR(CAM_TAG " 打不开 %s（卡在位吗？）", full);
        return FALSE;
    }
    if ((int)sg_jpeg_len != tkl_fwrite(sg_jpeg, (int)sg_jpeg_len, f)) {
        PR_ERR(CAM_TAG " 写 %s 失败（要 %u 字节）", full, (unsigned)sg_jpeg_len);
        (void)tkl_fclose(f);
        return FALSE;
    }
    (void)tkl_fclose(f);

    if (NULL != path && path_sz > 0) {
        snprintf(path, path_sz, "%s", full);
    }
    PR_NOTICE(CAM_TAG " 照片已存 %s（%u 字节）", full, (unsigned)sg_jpeg_len);
    return TRUE;
}

/* ---- 开流 ---------------------------------------------------------------- */

/**
 * @brief 打开/重开一路流。
 *
 * @param[in] with_jpeg TRUE = YUV422 + JPEG 双路（只在按快门的那一瞬间用）；
 *                      FALSE = 只出 YUV422（预览用）。
 *
 * **预览绝不能带 JPEG**：VGA@15fps 的 JPEG 编码流会把 PSRAM 带宽和 CPU 吃干
 * （实测精灵预取掉到 100KB/s、界面 2~4fps，还会刷 `size no match`）。
 * 快门那一下重开一次双路、拍完立刻切回 YUV，代价是快门瞬间预览停半秒。
 */
static OPERATE_RET prv_stream_open(BOOL_T with_jpeg)
{
    TDL_CAMERA_CFG_T cfg = { 0 };

    cfg.fps                  = APP_CAM_FPS;
    cfg.width                = APP_CAM_WANT_W;
    cfg.height               = APP_CAM_WANT_H;
    cfg.out_fmt              = with_jpeg ? TDL_CAMERA_FMT_JPEG_YUV422_BOTH
                                         : TDL_CAMERA_FMT_YUV422;
    cfg.get_frame_cb         = prv_raw_cb;
    cfg.get_encoded_frame_cb = with_jpeg ? prv_jpg_cb : NULL;
    cfg.encoded_quality.jpeg_cfg.enable   = with_jpeg ? 1 : 0;
    cfg.encoded_quality.jpeg_cfg.max_size = 25;     /* KB */
    cfg.encoded_quality.jpeg_cfg.min_size = 10;

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
        sg_order = info.yuv_order;
    }

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

    if (NULL == sg_jpeg) {
        sg_jpeg = (uint8_t *)tal_psram_malloc(APP_CAM_JPEG_MAX);
    }
    if (NULL == sg_jpeg) {
        PR_WARN(CAM_TAG " JPEG 暂存分配失败：能预览，不能拍照");
    }

    sg_front           = -1;
    sg_gen             = 0;
    sg_cap_req         = FALSE;
    sg_cap_done        = FALSE;
    sg_jpeg_len        = 0;
    sg_first_frame_log = FALSE;
    sg_fps_cnt         = 0;
    sg_fps_t0          = 0;
    sg_conv_ms_sum     = 0;
    sg_conv_ms_max     = 0;

    rt = prv_stream_open(FALSE);            /* 预览：只 YUV */
    if (OPRT_OK != rt) {
        PR_ERR(CAM_TAG " 开流失败 %d", (int)rt);
        app_camera_close();
        return rt;
    }

    sg_open = TRUE;
    PR_NOTICE(CAM_TAG " 已开流：%dx%d @%dfps（预览 YUV422），PSRAM 空闲 %uKB",
              APP_CAM_WANT_W, APP_CAM_WANT_H, APP_CAM_FPS,
              (unsigned)(tal_psram_get_free_heap_size() / 1024));
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
    sg_cap_req  = FALSE;
    sg_cap_done = FALSE;
    sg_jpeg_len = 0;
    sg_fps_cnt  = 0;
    sg_fps_t0   = 0;
    sg_conv_ms_sum = 0;
    sg_conv_ms_max = 0;

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

    /* cover：从预览缓冲（APP_CAM_PREVIEW_W×H）里取一块和目标同比例的最大中心矩形 */
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

BOOL_T app_camera_grab(void)
{
    uint32_t t0;
    BOOL_T   got = FALSE;

    sg_jpeg_len = 0;
    if (!sg_open || NULL == sg_jpeg) {
        return FALSE;
    }

    /* 预览是 YUV-only：先关掉，临时开双路拍一张，再切回来。 */
    PR_NOTICE(CAM_TAG " 抓图：切到 JPEG 流");
    (void)tdl_camera_dev_close(sg_hdl);

    sg_cap_done = FALSE;
    sg_cap_req  = TRUE;

    if (OPRT_OK == prv_stream_open(TRUE)) {
        t0 = tal_system_get_millisecond();
        while (!sg_cap_done) {
            if ((tal_system_get_millisecond() - t0) >= APP_CAM_CAP_WAIT_MS) {
                PR_WARN(CAM_TAG " 抓图超时：%dms 内没等到完整 JPEG", APP_CAM_CAP_WAIT_MS);
                break;
            }
            tal_system_sleep(10);
        }
        got = sg_cap_done;
        (void)tdl_camera_dev_close(sg_hdl);
    } else {
        PR_ERR(CAM_TAG " 打不开 JPEG 流");
    }
    sg_cap_req = FALSE;

    /* 切回预览流。这一步失败不影响已经抓到的 JPEG。 */
    if (OPRT_OK != prv_stream_open(FALSE)) {
        PR_ERR(CAM_TAG " 预览流恢复失败，退出页面再进会重开");
        sg_open = FALSE;
    }

    if (!got) {
        sg_jpeg_len = 0;
        return FALSE;
    }

    PR_NOTICE(CAM_TAG " 已抓图（%u 字节），等用户决定是否保存", (unsigned)sg_jpeg_len);
    return TRUE;
}

BOOL_T app_camera_save(char *path, size_t path_sz)
{
    if (NULL != path && path_sz > 0) {
        path[0] = '\0';
    }
    if (0 == sg_jpeg_len) {
        return FALSE;
    }
    if (!prv_save_jpeg(path, path_sz)) {
        return FALSE;
    }

    /* 存过就消费掉：同一张照片不能被"保存到相册"按两次写两份。 */
    sg_jpeg_len = 0;
    return TRUE;
}

void app_camera_discard(void)
{
    sg_jpeg_len = 0;
}
