/**
 * @file app_cam_snapshot.c
 * @brief 见 app_cam_snapshot.h。
 *
 * 实现上几个决定（都是从 app_cam_test.c 那轮 bring-up 里带过来的，别再踩）：
 *
 * 1) **YUV422 自己转、自己缩**，不用 SDK 的 `tal_image_convert_yuv422_to_rgb565`
 *    —— 它内部只按 `min(in_w,out_w) × min(in_h,out_h)` 逐点拷，640x480 传进去
 *    只会得到左上角一块。这里单趟做完"裁成 3:2 + 最近邻缩到 480x320"。
 * 2) **按 cover 裁，不拉伸**：摄像头 4:3、源图要 3:2，先把上下各切一点。
 * 3) **存卡在主线程做，不在相机回调里**：回调是相机自己的线程，在里面写 FatFs
 *    会阻塞 SDIO、把出帧队列顶爆。回调只做内存拷贝。
 * 4) 拍完**立刻关流**：不关的话 DVP 一直出帧，白占 PSRAM 带宽和 CPU。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"

#include "tdl_camera_manage.h"

#include "app_cam_snapshot.h"
#include "app_storage.h"
#include "app_time.h"

#if APP_CAM_SNAPSHOT

/* 板级 Kconfig 定的设备名（src/peripherals/camera/Kconfig，默认 "camera"）。 */
#ifndef CAMERA_NAME
#define CAMERA_NAME         "camera"
#endif

/* 向摄像头要的分辨率：够 480x320 用，又不用把整幅 1600x1200 搬来搬去。 */
#define CAM_SNAP_WANT_W     640
#define CAM_SNAP_WANT_H     480
#define CAM_SNAP_FPS        15

/* 开机后等界面（字库、精灵首帧、预热）先跑完再动摄像头。 */
#define CAM_SNAP_DELAY_MS   2000

/* 等首帧的上限。 */
#define CAM_SNAP_WAIT_MS    6000

/* JPEG 暂存上限（`encoded_quality.jpeg_cfg.max_size` 要的是 25KB，留足余量）。 */
#define CAM_SNAP_JPEG_MAX   (256 * 1024)

/* 存卡的子目录（APP_STORAGE_MOUNT 下面）。 */
#define CAM_SNAP_DIR        "pictures"

#define CAM_SNAP_TAG        "[camt]"

/* ---- 状态 ---------------------------------------------------------------- */

static uint16_t          *sg_rgb  = NULL;   /* 480x320 RGB565，拍完常驻给 UI 用 */
static uint8_t           *sg_jpeg = NULL;   /* 抓到的 JPEG，写完卡就释放 */
static uint32_t           sg_jpeg_len;
static TUYA_YUV422_ORDER_E sg_order = TUYA_YUV422_UYVY;
static volatile BOOL_T    sg_got_raw = FALSE;
static volatile BOOL_T    sg_got_jpg = FALSE;
static volatile uint32_t  sg_raw_cnt;
static volatile uint32_t  sg_jpg_cnt;
static volatile uint32_t  sg_gen;           /* 每成功一张 +1，UI 据此重画 */
static volatile BOOL_T    sg_ready = FALSE;
static THREAD_HANDLE      sg_thrd;

/* ---- YUV422 -> RGB565（cover 裁切 + 最近邻缩放） -------------------------- */

/**
 * @brief 一帧 YUV422 裁成 3:2、最近邻缩到 @a dw × @a dh。
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

    const BOOL_T yuyv   = (order == TUYA_YUV422_YUYV);
    const int    stride = sw * 2;

    for (int y = 0; y < dh; y++) {
        const uint8_t *row = src + (size_t)(y0 + y * src_h / dh) * stride;
        uint16_t      *out = dst + (size_t)y * dw;

        for (int x = 0; x < dw; x++) {
            int sx = x0 + x * src_w / dw;
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

/* ---- 相机回调（跑在相机自己的线程里） ------------------------------------ */

static OPERATE_RET prv_raw_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    (void)hdl;

    if (NULL == frame || NULL == frame->data || 0 == frame->data_len) {
        return OPRT_INVALID_PARM;
    }
    sg_raw_cnt++;
    if (sg_got_raw || NULL == sg_rgb) {
        return OPRT_OK;                     /* 只要第一帧 */
    }
    if (TUYA_FRAME_FMT_YUV422 != frame->fmt) {
        PR_WARN(CAM_SNAP_TAG " 原始帧格式不是 YUV422（fmt=%d），跳过", (int)frame->fmt);
        return OPRT_OK;
    }

    prv_yuv422_cover_to_rgb565(frame->data, frame->width, frame->height,
                               sg_rgb, CAM_SNAP_W, CAM_SNAP_H, sg_order);
    sg_got_raw = TRUE;
    PR_NOTICE(CAM_SNAP_TAG " 首帧 %dx%d -> %dx%d", frame->width, frame->height,
              CAM_SNAP_W, CAM_SNAP_H);
    return OPRT_OK;
}

static OPERATE_RET prv_jpg_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    uint32_t n;

    (void)hdl;

    if (NULL == frame || NULL == frame->data || 0 == frame->data_len) {
        return OPRT_INVALID_PARM;
    }
    sg_jpg_cnt++;
    /* JPEG 可能分片给，只有 is_complete 那次是完整图。 */
    if (!frame->is_complete || sg_got_jpg || NULL == sg_jpeg) {
        return OPRT_OK;
    }

    n = frame->data_len;
    if (n > CAM_SNAP_JPEG_MAX) {
        PR_WARN(CAM_SNAP_TAG " JPEG 超上限 %u > %d，截断", (unsigned)n, CAM_SNAP_JPEG_MAX);
        n = CAM_SNAP_JPEG_MAX;
    }
    memcpy(sg_jpeg, frame->data, n);
    sg_jpeg_len = n;
    sg_got_jpg  = TRUE;
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
        snprintf(out, out_sz, "%s/%s/cam_%s.jpg", APP_STORAGE_MOUNT, CAM_SNAP_DIR, stamp);
        return;
    }

    for (int i = 1; i <= 999; i++) {
        char    name[32];
        char    path[128];
        BOOL_T  ex = FALSE;

        if (1 == i) {
            snprintf(name, sizeof(name), "cam.jpg");
        } else {
            snprintf(name, sizeof(name), "cam_%d.jpg", i);
        }
        snprintf(path, sizeof(path), "%s/%s/%s", APP_STORAGE_MOUNT, CAM_SNAP_DIR, name);
        if (OPRT_OK != tkl_fs_is_exist(path, &ex) || !ex) {
            snprintf(out, out_sz, "%s", path);
            return;
        }
    }
    snprintf(out, out_sz, "%s/%s/cam_999.jpg", APP_STORAGE_MOUNT, CAM_SNAP_DIR);
}

static void prv_save_jpeg(void)
{
    char      dir[64];
    char      path[160];
    TUYA_FILE f;

    if (0 == sg_jpeg_len || NULL == sg_jpeg) {
        PR_WARN(CAM_SNAP_TAG " 没抓到 JPEG，跳过存卡");
        return;
    }

    snprintf(dir, sizeof(dir), "%s/%s", APP_STORAGE_MOUNT, CAM_SNAP_DIR);
    (void)tkl_fs_mkdir(dir);        /* 已存在时返回非 0，是预期内的 */

    prv_make_path(path, sizeof(path));

    f = tkl_fopen(path, "w");
    if (NULL == f) {
        PR_ERR(CAM_SNAP_TAG " 打不开 %s（卡在位吗？）", path);
        return;
    }
    if ((int)sg_jpeg_len != tkl_fwrite(sg_jpeg, (int)sg_jpeg_len, f)) {
        PR_ERR(CAM_SNAP_TAG " 写 %s 失败（要 %u 字节）", path, (unsigned)sg_jpeg_len);
        (void)tkl_fclose(f);
        return;
    }
    (void)tkl_fclose(f);
    PR_NOTICE(CAM_SNAP_TAG " 照片已存 %s（%u 字节）", path, (unsigned)sg_jpeg_len);
}

/* ---- 一次性线程 ---------------------------------------------------------- */

static void prv_snap_task(void *arg)
{
    TDL_CAMERA_HANDLE_T   hdl;
    TDL_CAMERA_DEV_INFO_T info = { 0 };
    TDL_CAMERA_CFG_T      cfg  = { 0 };
    OPERATE_RET           rt;
    uint32_t              t0;

    (void)arg;

    /* 让开机流程（字库、精灵首帧、预热）先跑完，别和它们抢 PSRAM / CPU。 */
    tal_system_sleep(CAM_SNAP_DELAY_MS);

    PR_NOTICE(CAM_SNAP_TAG " ==== 开机快照 ====");

    hdl = tdl_camera_find_dev(CAMERA_NAME);
    if (NULL == hdl) {
        PR_ERR(CAM_SNAP_TAG " 没找到设备 '%s'（CONFIG_TUYA_T5AI_BOARD_CAMERA 开了吗？）",
               CAMERA_NAME);
        goto out;
    }
    if (OPRT_OK == tdl_camera_dev_get_info(hdl, &info)) {
        PR_NOTICE(CAM_SNAP_TAG " 设备：最大 %dx%d @%dfps，格式位图 0x%X，yuv_order=%d",
                  info.max_width, info.max_height, info.max_fps,
                  (unsigned)info.supported_fmts, (int)info.yuv_order);
        sg_order = info.yuv_order;
    }

    sg_rgb = (uint16_t *)tal_psram_malloc((size_t)CAM_SNAP_W * CAM_SNAP_H * 2u);
    if (NULL == sg_rgb) {
        PR_ERR(CAM_SNAP_TAG " 转换缓冲 %dx%d 分配失败", CAM_SNAP_W, CAM_SNAP_H);
        goto out;
    }
    sg_jpeg = (uint8_t *)tal_psram_malloc(CAM_SNAP_JPEG_MAX);
    if (NULL == sg_jpeg) {
        PR_WARN(CAM_SNAP_TAG " JPEG 暂存分配失败：只显示，不存卡");
    }

    sg_got_raw = FALSE;
    sg_got_jpg = FALSE;
    sg_raw_cnt = 0;
    sg_jpg_cnt = 0;

    cfg.fps                  = CAM_SNAP_FPS;
    cfg.width                = CAM_SNAP_WANT_W;
    cfg.height               = CAM_SNAP_WANT_H;
    cfg.out_fmt              = (NULL != sg_jpeg) ? TDL_CAMERA_FMT_JPEG_YUV422_BOTH
                                                 : TDL_CAMERA_FMT_YUV422;
    cfg.get_frame_cb         = prv_raw_cb;
    cfg.get_encoded_frame_cb = (NULL != sg_jpeg) ? prv_jpg_cb : NULL;
    cfg.encoded_quality.jpeg_cfg.enable   = 1;
    cfg.encoded_quality.jpeg_cfg.max_size = 25;     /* KB */
    cfg.encoded_quality.jpeg_cfg.min_size = 10;

    rt = tdl_camera_dev_open(hdl, &cfg);
    PR_NOTICE(CAM_SNAP_TAG " 开流返回 %d（0 = OK）", (int)rt);
    if (OPRT_OK != rt) {
        goto out;
    }

    t0 = tal_system_get_millisecond();
    while (!(sg_got_raw && (sg_got_jpg || NULL == sg_jpeg))) {
        if ((tal_system_get_millisecond() - t0) >= CAM_SNAP_WAIT_MS) {
            PR_WARN(CAM_SNAP_TAG " 等首帧超时 %dms：raw 回调 %u 次，jpg 回调 %u 次",
                    CAM_SNAP_WAIT_MS, (unsigned)sg_raw_cnt, (unsigned)sg_jpg_cnt);
            break;
        }
        tal_system_sleep(20);
    }

    /* 拍完就关：不关的话 DVP 一直出帧，白占 PSRAM 带宽和 CPU。 */
    (void)tdl_camera_dev_close(hdl);

    if (!sg_got_raw) {
        PR_ERR(CAM_SNAP_TAG " 一帧都没拿到（传感器没应答？看上面 SCCB / 开流日志）");
        goto out;
    }

    prv_save_jpeg();

    sg_gen++;
    sg_ready = TRUE;
    PR_NOTICE(CAM_SNAP_TAG " 快照就绪（gen=%u），拍照页取景框会用这张",
              (unsigned)sg_gen);

out:
    if (NULL != sg_jpeg) {
        tal_psram_free(sg_jpeg);
        sg_jpeg = NULL;
    }
    if (!sg_ready && NULL != sg_rgb) {
        tal_psram_free(sg_rgb);
        sg_rgb = NULL;
    }

    /* 一次性线程，干完自退 —— tal_thread_delete 只是打异步标记，放最后一行。 */
    tal_thread_delete(sg_thrd);
    sg_thrd = NULL;
}

/* ---- 对外 ---------------------------------------------------------------- */

void app_cam_snapshot_start(void)
{
    THREAD_CFG_T cfg = { 0 };

    if (NULL != sg_thrd) {
        return;                             /* 幂等 */
    }

    cfg.stackDepth = 8192;
    cfg.priority   = THREAD_PRIO_4;         /* 低于 LVGL(2) 和路由(3)：不抢界面 */
    cfg.thrdname   = "cam_snap";

    if (OPRT_OK != tal_thread_create_and_start(&sg_thrd, NULL, NULL,
                                               prv_snap_task, NULL, &cfg)) {
        PR_ERR(CAM_SNAP_TAG " 快照线程起不来");
        sg_thrd = NULL;
    }
}

BOOL_T app_cam_snapshot_ready(void)
{
    return sg_ready;
}

uint32_t app_cam_snapshot_gen(void)
{
    return sg_gen;
}

BOOL_T app_cam_snapshot_blit(uint16_t *dst, int w, int h)
{
    int src_w, src_h, x0, y0;

    if (!sg_ready || NULL == sg_rgb || NULL == dst || w <= 0 || h <= 0) {
        return FALSE;
    }

    /* cover：从 480x320 里取一块和目标同比例的最大中心矩形 */
    src_w = CAM_SNAP_W;
    src_h = CAM_SNAP_H;
    if ((int64_t)w * src_h > (int64_t)h * src_w) {
        src_h = (int)((int64_t)src_w * h / w);      /* 目标更宽：裁高度 */
    } else {
        src_w = (int)((int64_t)src_h * w / h);      /* 目标更高：裁宽度 */
    }
    if (src_w <= 0 || src_h <= 0) {
        return FALSE;
    }
    x0 = (CAM_SNAP_W - src_w) / 2;
    y0 = (CAM_SNAP_H - src_h) / 2;

    for (int y = 0; y < h; y++) {
        const uint16_t *row = sg_rgb + (size_t)(y0 + y * src_h / h) * CAM_SNAP_W + x0;
        uint16_t       *out = dst + (size_t)y * w;

        for (int x = 0; x < w; x++) {
            out[x] = row[x * src_w / w];
        }
    }
    return TRUE;
}

void app_cam_snapshot_release(void)
{
    if (NULL != sg_rgb) {
        tal_psram_free(sg_rgb);
        sg_rgb = NULL;
    }
    sg_ready = FALSE;
}

#endif /* APP_CAM_SNAPSHOT */
