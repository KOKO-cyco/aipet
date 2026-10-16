/**
 * @file app_cam_test.c
 * @brief 摄像头（GC2145 / DVP）bring-up：扫 SCCB -> 拍照 -> 存卡。
 *
 * **2026-10-16：改成"只有摄像头"模式** —— 屏幕（LCD + 触摸）在 app_default.config
 * 里关掉了，因为这个测试要排除"LCD 抢了 P51（板级当 DVP_RST、平台默认表里是
 * GPIO_DEV_LCD_G1）"这条嫌疑。没有屏，所以结果**只看串口日志**，测试跑完停住
 * 不再往下启动（见 tuya_app_main.c 的调用点）。
 *
 * 语义和开关见 app_cam_test.h。这里只记实现上的几个决定：
 *
 * 1) **先扫 I2C 再谈别的。** 2026-10-16 真机上 SCCB 全在 `wait ack timeout`，
 *    一帧都没有。那种情况下分辨率/格式/YUV 顺序全是噪声，唯一有用的信息是
 *    "总线上的地址谁在应答"。扫描用 `tkl_i2c_master_send()` 往每个 7 位地址发
 *    一个字节，看有没有 ACK —— 这是 I2C 的标准探活法。
 *
 * 2) **YUV422 自己转、自己缩，不用 SDK 的 `tal_image_convert_yuv422_to_rgb565`。**
 *    那个函数**不缩放**：内部取 `min(in_w,out_w) × min(in_h,out_h)` 逐点拷贝
 *    （见 tal_image_yuv422_to_rgb.c 的 `__sw_convert_yuv422_to_rgb565`）。640x480 传进去、
 *    480x320 传出来，得到的是左上角那一块。所以这里单趟做完"裁成 3:2 + 最近邻缩
 *    到 480x320"，顺带省掉一张 600KB 的中间图。
 *
 * 3) **按"填满"裁（cover），不拉伸。** 摄像头是 4:3，屏是 3:2，直接缩会把画面横向
 *    拉长 12.5%。先裁成和屏一样的宽高比（640x480 -> 640x426，上下各切 27 行）再缩。
 *
 * 4) **卡上的写入放在主线程，不放在相机回调里。** 回调是相机自己的线程，在里面
 *    写 FatFs 会在 SDIO 上阻塞，把出帧队列顶爆。回调只把 JPEG 拷进 PSRAM 暂存。
 */

#include <string.h>

#include "tal_api.h"

#include "app_cam_test.h"

#if APP_CAM_TEST

#include "tdl_camera_manage.h"
#include "tdd_camera_dvp_i2c.h" /* tdd_dvp_i2c_init —— 扫描前把 SCCB 初始化起来 */
#include "tkl_i2c.h"
#include "tkl_gpio.h"           /* 手动补复位脉冲用（tal_api.h 不带） */
#include "tal_fs.h"             /* tal_fopen/tal_fwrite —— tal_api.h 不带它 */

#include "app_storage.h"        /* APP_STORAGE_MOUNT */
#include "app_time.h"           /* app_time_valid / app_time_format */

/* 板级 Kconfig 定的设备名（src/peripherals/camera/Kconfig，默认 "camera"）。 */
#ifndef CAMERA_NAME
#define CAMERA_NAME             "camera"
#endif

/* 背景/显示缓冲的尺寸 —— **必须等于屏的逻辑尺寸**（横屏 480x320）。 */
#define CAM_BG_W                480
#define CAM_BG_H                320

/* 向摄像头要的分辨率。GC2145 支持到 1600x1200，但显示只需要 480x320。 */
#define CAM_WANT_W              640
#define CAM_WANT_H              480
#define CAM_FPS                 15

/* 等第一帧的上限。 */
#define CAM_WAIT_MS             5000

/* JPEG 暂存上限。`encoded_quality.jpeg_cfg.max_size` 要的是 25KB，留足余量。 */
#define CAM_JPEG_MAX            (256 * 1024)

/* 存卡的子目录（APP_STORAGE_MOUNT 下面）。 */
#define CAM_PIC_DIR             "pictures"

/* SCCB 上两个已知设备的 7 位地址 —— 扫描时标出来，一眼看出谁在谁不在。 */
#define CAM_ADDR_GC2145         0x3C
#define CAM_ADDR_GT1151         0x14

/* ---- 状态 ---------------------------------------------------------------- */

static uint16_t        *sg_bg       = NULL;     /* 480x320 RGB565 */
static uint8_t         *sg_jpeg     = NULL;     /* 抓到的 JPEG，写完就释放 */
static uint32_t         sg_jpeg_len = 0;
static TUYA_YUV422_ORDER_E sg_order = TUYA_YUV422_UYVY;
static volatile BOOL_T  sg_got_raw  = FALSE;
static volatile BOOL_T  sg_got_jpg  = FALSE;
static volatile uint32_t sg_raw_cnt = 0;        /* 回调被调了几次（诊断用） */
static volatile uint32_t sg_jpg_cnt = 0;

/* ---- SCCB 扫描 ----------------------------------------------------------- */

/** 把这条 SCCB 总线按摄像头的方式初始化（P13/P15 + 软件 I2C）。
 *
 * `tkl_i2c_master_send()` 要求 `sg_i2c_cfg[port].mutex` 已经建好
 * （见 tkl_i2c.c:739，否则直接返回 OPRT_INVALID_PARM —— 扫出来"一个都没有"是假象）。
 * 这里调的就是相机驱动自己那套 `tdd_dvp_i2c_init()`，先跑一遍不影响后面。 */
static void prv_sccb_bus_init(void)
{
    DVP_I2C_CFG_T bus = {
        .port = TUYA_I2C_NUM_0,
        .clk  = TUYA_GPIO_NUM_13,
        .sda  = TUYA_GPIO_NUM_15,
    };
    (void)tdd_dvp_i2c_init(&bus);
}

/**
 * @brief 扫一遍 SCCB（I2C0，P13/P15），把应答的 7 位地址打出来。返回应答个数。
 *
 * 往每个地址写 1 个字节（寄存器地址 0x00），有 ACK 就算在。
 * **注意相机和触摸屏共用这条总线** —— 0x14（GT1151）在不在是"总线本身活不活"的
 * 参照：连触摸屏都不应答就是总线/引脚问题，跟摄像头模组无关。
 *
 * @param tag 日志上下文，比如"上电后" / "开流后"
 */
static int prv_sccb_scan(const char *tag)
{
    uint8_t  dummy = 0x00;
    int      found = 0, gc = 0, tp = 0;
    char     line[160];
    int      n = 0;

    n += snprintf(line + n, sizeof(line) - n, "[camt] SCCB 扫描(%s) 应答:", tag);
    for (int a = 0x08; a <= 0x77; a++) {
        if (OPRT_OK == tkl_i2c_master_send(TUYA_I2C_NUM_0, (uint16_t)a,
                                           &dummy, 1, FALSE)) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%02X%s", a,
                          (a == CAM_ADDR_GC2145) ? "(GC2145)" :
                          (a == CAM_ADDR_GT1151) ? "(触摸屏)" : "");
            found++;
            if (a == CAM_ADDR_GC2145) gc = 1;
            if (a == CAM_ADDR_GT1151) tp = 1;
        }
    }
    if (0 == found) {
        n += snprintf(line + n, sizeof(line) - n, " **一个都没有**");
    }
    PR_NOTICE("%s", line);

    if (0 == found) {
        PR_ERR("[camt] %s：SCCB 上没有任何设备应答 —— 总线/引脚问题", tag);
    } else if (!gc) {
        PR_ERR("[camt] %s：总线是活的（%d 个设备%s），但 0x%02X 的 GC2145 不在",
               tag, found, tp ? "，含触摸屏 0x14" : "", CAM_ADDR_GC2145);
    } else {
        PR_NOTICE("[camt] %s：GC2145 在总线上（0x%02X 有应答）", tag, CAM_ADDR_GC2145);
    }
    return found;
}

/**
 * @brief 单独探一下 0x3C（不打印整行扫描）。给"要不要补复位"做判断用。
 */
static BOOL_T prv_gc_present(void)
{
    uint8_t dummy = 0x00;

    return (OPRT_OK == tkl_i2c_master_send(TUYA_I2C_NUM_0, CAM_ADDR_GC2145,
                                           &dummy, 1, FALSE)) ? TRUE : FALSE;
}

/**
 * @brief 把复位脚（P51）**钉在某个电平上不动**，等它稳定。
 *
 * 用途见 `app_cam_test_run()` 最后那段：P51 的**极性**在 SDK 里是自相矛盾的 ——
 * 板级定义（`tuya_t5ai_ex_module.h`）把它当"**低有效复位**"，而 SDK 自己的
 * `platform/T5AI/.../test/test_media/test_media_camera.c` 把**同一个 P51** 当
 * "电源脚、**高有效**"。极性反了的话，SDK 那套 `HIGH→LOW→HIGH` 时序最后停在
 * HIGH，就可能把模组**关掉**（或者一直按在复位里）。
 *
 * @param high TRUE = 钉高电平，FALSE = 钉低电平
 */
static void prv_rst_hold(BOOL_T high)
{
    TUYA_GPIO_BASE_CFG_T cfg = { .direct = TUYA_GPIO_OUTPUT };

    cfg.level = high ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    (void)tkl_gpio_init(TUYA_GPIO_NUM_51, &cfg);
    tkl_gpio_write(TUYA_GPIO_NUM_51, cfg.level);
    tal_system_sleep(150);              /* 给模组上电/复位的时间 */
    PR_NOTICE("[camt] P51 钉在 %s", high ? "高" : "低");
}

/**
 * @brief 手动把摄像头的复位脚（板级 `BOARD_CAMERA_RST_PIN` = P51，低有效）拉一遍。
 *
 * 用来分辨"复位没生效"和"模组根本没接"：拉完再扫，出现 0x3C 就说明之前的复位
 * 时序/引脚有问题，仍然没有就是模组侧（没插好 / 没供电）。
 *
 * 引脚号硬编码 51 —— 和 `boards/T5AI/TUYA_T5AI_BOARD/tuya_t5ai_ex_module.h` 的
 * `BOARD_CAMERA_RST_PIN` 一致（PNG 的 `LCD+CIS+TF IO配置` 也是 P51 = DVP_RST）。
 */
static void prv_rst_pulse(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = { .direct = TUYA_GPIO_OUTPUT };

    cfg.level = TUYA_GPIO_LEVEL_HIGH;
    (void)tkl_gpio_init(TUYA_GPIO_NUM_51, &cfg);
    tkl_gpio_write(TUYA_GPIO_NUM_51, TUYA_GPIO_LEVEL_HIGH);
    tal_system_sleep(20);
    tkl_gpio_write(TUYA_GPIO_NUM_51, TUYA_GPIO_LEVEL_LOW);   /* 低 = 复位 */
    tal_system_sleep(100);
    tkl_gpio_write(TUYA_GPIO_NUM_51, TUYA_GPIO_LEVEL_HIGH);  /* 放开 */
    tal_system_sleep(100);
    PR_NOTICE("[camt] 手动复位脉冲已发（P51）");
}

/* ---- YUV422 -> RGB565（cover 裁切 + 最近邻缩放） -------------------------- */

/**
 * @brief 一帧 YUV422 裁成屏的宽高比、最近邻缩到 @a dw × @a dh。
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

    const BOOL_T yuyv = (order == TUYA_YUV422_YUYV);
    const int    stride = sw * 2;

    for (int y = 0; y < dh; y++) {
        const uint8_t *row = src + (size_t)(y0 + y * src_h / dh) * stride;
        uint16_t      *out = dst + (size_t)y * dw;

        for (int x = 0; x < dw; x++) {
            int sx = x0 + x * src_w / dw;
            const uint8_t *p = row + (sx & ~1) * 2;
            int  u, v, yv, r, g, b;

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
            int c = yv - 16, d = u - 128, e = v - 128;
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
    if (sg_got_raw || NULL == sg_bg) {
        return OPRT_OK;                     /* 只要第一帧 */
    }
    if (TUYA_FRAME_FMT_YUV422 != frame->fmt) {
        PR_WARN("[camt] 原始帧格式不是 YUV422（fmt=%d），跳过", (int)frame->fmt);
        return OPRT_OK;
    }

    prv_yuv422_cover_to_rgb565(frame->data, frame->width, frame->height,
                               sg_bg, CAM_BG_W, CAM_BG_H, sg_order);
    sg_got_raw = TRUE;
    PR_NOTICE("[camt] 首帧 %dx%d -> %dx%d", frame->width, frame->height,
              CAM_BG_W, CAM_BG_H);
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
    if (n > CAM_JPEG_MAX) {
        PR_WARN("[camt] JPEG 超上限 %u > %d，截断", (unsigned)n, CAM_JPEG_MAX);
        n = CAM_JPEG_MAX;
    }
    memcpy(sg_jpeg, frame->data, n);
    sg_jpeg_len = n;
    sg_got_jpg  = TRUE;
    return OPRT_OK;
}

/* ---- 存卡 ---------------------------------------------------------------- */

/** 拼一个还没被占用的路径：时间对上就用时间戳，否则 boot / boot_2 / … */
static void prv_make_path(char *out, size_t out_sz)
{
    char stamp[32] = { 0 };
    char name[48];

    if (app_time_valid() &&
        OPRT_OK == app_time_format(stamp, sizeof(stamp), "%Y%m%d_%H%M%S") &&
        '\0' != stamp[0] && '-' != stamp[0]) {
        snprintf(out, out_sz, "%s/%s/%s.jpg", APP_STORAGE_MOUNT, CAM_PIC_DIR, stamp);
        return;
    }

    /* 开机时大概率还没对时（NTP 要联网之后才回来），用序号兜底。 */
    for (int i = 1; i <= 999; i++) {
        BOOL_T ex = FALSE;

        if (1 == i) {
            snprintf(name, sizeof(name), "boot.jpg");
        } else {
            snprintf(name, sizeof(name), "boot_%d.jpg", i);
        }
        snprintf(out, out_sz, "%s/%s/%s", APP_STORAGE_MOUNT, CAM_PIC_DIR, name);
        if (OPRT_OK != tal_fs_is_exist(out, &ex) || !ex) {
            return;
        }
    }
    snprintf(out, out_sz, "%s/%s/boot_999.jpg", APP_STORAGE_MOUNT, CAM_PIC_DIR);
}

static void prv_save_jpeg(void)
{
    char dir[64];
    char path[128];
    TUYA_FILE f;

    if (0 == sg_jpeg_len || NULL == sg_jpeg) {
        PR_WARN("[camt] 没抓到 JPEG，跳过存卡");
        return;
    }

    snprintf(dir, sizeof(dir), "%s/%s", APP_STORAGE_MOUNT, CAM_PIC_DIR);
    (void)tal_fs_mkdir(dir);        /* 已存在时返回非 0，是预期内的 */

    prv_make_path(path, sizeof(path));

    f = tal_fopen(path, "w");
    if (NULL == f) {
        PR_ERR("[camt] 打不开 %s（卡在位吗？）", path);
        return;
    }
    if ((int)sg_jpeg_len != tal_fwrite(sg_jpeg, (int)sg_jpeg_len, f)) {
        PR_ERR("[camt] 写 %s 失败（要 %u 字节）", path, (unsigned)sg_jpeg_len);
        (void)tal_fclose(f);
        return;
    }
    (void)tal_fclose(f);
    PR_NOTICE("[camt] 照片已存 %s（%u 字节）", path, (unsigned)sg_jpeg_len);
}

/* ---- 铺满 LCD ------------------------------------------------------------ */

/* ---- 对外 ---------------------------------------------------------------- */

void app_cam_test_run(void)
{
    TDL_CAMERA_HANDLE_T hdl;
    TDL_CAMERA_DEV_INFO_T info = { 0 };
    TDL_CAMERA_CFG_T cfg = { 0 };
    OPERATE_RET rt;
    uint32_t t0;

    PR_NOTICE("[camt] ==== 摄像头 bring-up 开始 ====");

    /* 1) 先把总线起起来，扫一遍。
     *    **这一次的结论要打折扣看** —— 此刻 DVP 还没 init，MCLK（P27）没输出，
     *    GC2145 没时钟可能不应答 SCCB。真正有诊断价值的是下面"开流后"那次。 */
    prv_sccb_bus_init();
    prv_sccb_scan("上电后/MCLK 未起");

    /* 2) 找设备。TDD 是板级注册的（board_register_hardware），这里只是查表。 */
    hdl = tdl_camera_find_dev(CAMERA_NAME);
    if (NULL == hdl) {
        PR_ERR("[camt] 没找到设备 '%s'（CONFIG_TUYA_T5AI_BOARD_CAMERA 开了吗？）",
               CAMERA_NAME);
        return;
    }
    if (OPRT_OK == tdl_camera_dev_get_info(hdl, &info)) {
        PR_NOTICE("[camt] 设备信息：最大 %dx%d @%dfps，支持格式位图 0x%X，yuv_order=%d",
                  info.max_width, info.max_height, info.max_fps,
                  (unsigned)info.supported_fmts, (int)info.yuv_order);
        sg_order = info.yuv_order;
    } else {
        PR_WARN("[camt] 读不到设备信息（不影响继续）");
    }

    /* 3) 缓冲 */
    sg_bg = (uint16_t *)tal_psram_malloc((size_t)CAM_BG_W * CAM_BG_H * 2u);
    if (NULL == sg_bg) {
        PR_ERR("[camt] 显示缓冲 %dx%d 分配失败", CAM_BG_W, CAM_BG_H);
        return;
    }
    sg_jpeg = (uint8_t *)tal_psram_malloc(CAM_JPEG_MAX);
    if (NULL == sg_jpeg) {
        PR_WARN("[camt] JPEG 暂存分配失败：只显示，不存卡");
    }

    /* 4) 开流 */
    sg_got_raw = FALSE;
    sg_got_jpg = FALSE;
    sg_raw_cnt = 0;
    sg_jpg_cnt = 0;

    cfg.fps                  = CAM_FPS;
    cfg.width                = CAM_WANT_W;
    cfg.height               = CAM_WANT_H;
    cfg.out_fmt              = (NULL != sg_jpeg) ? TDL_CAMERA_FMT_JPEG_YUV422_BOTH
                                                 : TDL_CAMERA_FMT_YUV422;
    cfg.get_frame_cb         = prv_raw_cb;
    cfg.get_encoded_frame_cb = (NULL != sg_jpeg) ? prv_jpg_cb : NULL;
    cfg.encoded_quality.jpeg_cfg.enable   = 1;
    cfg.encoded_quality.jpeg_cfg.max_size = 25;     /* KB */
    cfg.encoded_quality.jpeg_cfg.min_size = 10;

    rt = tdl_camera_dev_open(hdl, &cfg);
    PR_NOTICE("[camt] 开流返回 %d（0 = OK）", (int)rt);
    if (OPRT_OK != rt) {
        tal_psram_free(sg_bg);
        sg_bg = NULL;
        return;
    }

    /* 5) **再扫一遍 —— 这一次才是准的。**
     *
     * `tdl_camera_dev_open()` 走完 = 复位做过了、`tkl_dvp_start()` 也把 MCLK
     * 打开了（见 tdd_camera_dvp.c 的 `__tdd_camera_dvp_open`）。所以此刻传感器
     * 该有的电和时钟都有了，还不在总线上就是真不在。
     *
     * 三种结果的读法：
     *   这次 0x3C 出现、上次没有  -> MCLK 没起导致的假阴性（SDK 的 init 顺序问题）
     *   两次都没有              -> 模组侧：没插好 / 没供电 / 复位脚
     *   这次 0x3C 出现但仍无帧   -> 传感器在，问题在 DVP 数据线/分辨率
     */
    (void)prv_sccb_scan("开流后/MCLK 已起");
    if (!prv_gc_present()) {
        /* 再给一次机会：手动补一个复位脉冲，然后第三次扫。 */
        prv_rst_pulse();
        (void)prv_sccb_scan("补复位后");
    }
    if (!prv_gc_present()) {
        /* 最后一次：P51 的极性在 SDK 里是自相矛盾的（板级当"低有效复位"，
         * test_media_camera.c 当"高有效电源"）。把它分别钉在两个电平上各扫一遍，
         * **哪次出现 0x3C 就是哪个极性** —— 这样就不用在硬件上瞎猜了。 */
        prv_rst_hold(FALSE);
        (void)prv_sccb_scan("P51 钉低");
        prv_rst_hold(TRUE);
        (void)prv_sccb_scan("P51 钉高");
    }

    /* 6) 等首帧。日志把"回调一次没调"和"调了但没出图"分开。 */
    t0 = tal_system_get_millisecond();
    while (!(sg_got_raw && (sg_got_jpg || NULL == sg_jpeg))) {
        if ((tal_system_get_millisecond() - t0) >= CAM_WAIT_MS) {
            PR_WARN("[camt] 等首帧超时 %dms：raw 回调 %u 次，jpg 回调 %u 次",
                    CAM_WAIT_MS, (unsigned)sg_raw_cnt, (unsigned)sg_jpg_cnt);
            break;
        }
        tal_system_sleep(20);
    }

    /* 拍完就关 —— 不关的话 DVP 会一直出帧，白占 PSRAM 带宽和 CPU。 */
    (void)tdl_camera_dev_close(hdl);

    if (!sg_got_raw) {
        PR_ERR("[camt] 一帧都没拿到 —— 看上面的 SCCB 扫描结果");
        tal_psram_free(sg_bg);
        sg_bg = NULL;
        if (NULL != sg_jpeg) {
            tal_psram_free(sg_jpeg);
            sg_jpeg = NULL;
        }
        PR_WARN("[camt] 没有背景可用，测试模式到此为止（不再往下启动）");
        for (;;) {
            tal_system_sleep(1000);
        }
    }

    /* 6) 存卡（关流之后、主线程里做）。
     *    "只有摄像头"模式下这段代码跑在 app_storage_wait_ready() **之前**，
     *    卡还没挂起来，所以先看 app_storage_ready() —— 没就绪就跳过，
     *    免得日志里冒一条"打不开文件"的假错误。 */
    if (app_storage_ready()) {
        prv_save_jpeg();
    } else {
        PR_NOTICE("[camt] SD 还没就绪，跳过存卡（只验证拍照）");
    }
    if (NULL != sg_jpeg) {
        tal_psram_free(sg_jpeg);
        sg_jpeg = NULL;
    }

    /* 7) **停住** —— 这是"只有摄像头"的测试模式：屏已经关掉了（见
     *    app_default.config），后面的 app_disp_init / 桌宠页都跑不起来。 */
    PR_NOTICE("[camt] 拍照成功（%dx%d 背景已就绪）。测试模式停在这里；"
              "要回正常启动：把 app_cam_test.h 的 APP_CAM_TEST 改 0，"
              "并把 app_default.config 里 CONFIG_TUYA_T5AI_BOARD_LCD_35565 恢复。",
              CAM_BG_W, CAM_BG_H);
    for (;;) {
        tal_system_sleep(1000);
    }
}

#else   /* APP_CAM_TEST == 0 */

void app_cam_test_run(void)
{
    /* 正常启动：什么都不做 */
}

#endif
