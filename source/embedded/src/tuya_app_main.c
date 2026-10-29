/**
 * @file tuya_app_main.c
 * @brief aipet 入口：只负责"初始化基础设施 + 按顺序启动各服务"，不含任何业务。
 *
 * 分层（详见 source/embedded/ARCHITECTURE.md）
 *   core/     事件总线、配置中心、页面路由      —— 与硬件无关的框架
 *   service/  显示 / 音频 / 存储 / 网络 / 时间 / 按键 —— 所有功能共用的能力
 *   pages/    每个界面一个文件，注册进路由（现在只有桌宠页 + SD 门页）
 *   modules/  具体功能实现（精灵引擎、AI 语音对话、对话驱动动作、扬声器）
 *
 * 本文件里剩下的事只有三件：
 *   1) tal / 涂鸦云 基础设施初始化
 *   2) 启动各服务并进入桌宠页
 *   3) 云端事件（DP 下发、MQTT 连接）转成服务调用
 *
 * 线程
 *   tuya_app_main  - 初始化，然后只跑 tuya_iot_yield（网络/云事件泵）
 *   ui_router      - 页面状态机 + 系统按键轮询（core/app_router.c）
 *   lvgl           - LVGL 渲染任务（桌宠页全程运行）
 *   各自的模块线程  - 精灵渲染、AI 语音栈、SD 就绪等
 */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"               /* lv_display_set_rotation —— 开机就转横屏 */

#include "tal_api.h"
#include "tal_kv.h"
#include "tal_sw_timer.h"
#include "tal_time_service.h"
#include "tal_cli.h"
#include "tkl_output.h"
#include "tkl_wifi.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "tuya_authorize.h"
#include "cJSON.h"

#include "board_com_api.h"

#include "tuya_config.h"
#include "reset_netcfg.h"

#include "app_ai_chat.h"
#include "app_audio.h"
#include "app_avatar_tools.h"
#include "app_bringup.h"
#include "app_cam_test.h"
#include "app_cam_snapshot.h"   /* 开机快照测试（src/tests） */
#include "app_sd_test.h"
#include "app_sd_hw_test.h"     /* SD 读通路 A/B（CPU 轮询 vs GDMA），src/tests */
#include "app_disp.h"
#include "app_event.h"
#include "app_font.h"
#include "app_key.h"
#include "app_net.h"
#include "app_perf.h"
#include "app_pages.h"
#include "app_preload.h"
#include "app_splash.h"
#include "app_weather.h"
#include "app_router.h"
#include "app_settings.h"
#include "app_storage.h"
#include "app_mem.h"
#include "app_time.h"

/* 首页 / 各页面栈尺寸 */
#define APP_ROUTER_STACK    8192
#define APP_ROUTER_PRIO     THREAD_PRIO_3

/* 开机画面最多顶多久 —— **现在是永久**（见启动序列的 5.7）。这里不再有超时常量：
 * 卡对本产品相当于外挂 flash，没卡就一直停在开机画面，插上就走。 */
#define APP_LVGL_STACK      8192
#define APP_LVGL_PRIO       THREAD_PRIO_2

/* 产品 schema 里的音量 DP（可读写，0-100）：
 * id1 bool rw / id2 value ro / id3 value rw(=AI 音量) / id5 enum ro */
#define DP_ID_VOLUME        3

/* Tuya device handle */
static tuya_iot_client_t  ai_client;
/* device license (uuid / authkey), read from flash at boot */
static tuya_iot_license_t license;

/***********************************************************
************************* cloud *****************************
***********************************************************/
/* 把当前 AI 音量上报给云端 */
static void prv_report_volume_dp(void)
{
    dp_obj_t dp = {
        .id             = DP_ID_VOLUME,
        .type           = PROP_VALUE,
        .value.dp_value = app_audio_get_volume(APP_AUDIO_CH_AI),
    };

    int ret = tuya_iot_dp_obj_report(&ai_client, NULL, &dp, 1, 0);

    if (OPRT_OK != ret) {
        PR_WARN("[main] 音量上报失败: %d", ret);
    } else {
        PR_NOTICE("[main] 音量已上报: %d", dp.value.dp_value);
    }
}

/* 云端下发的 DP（在 yield 线程里执行）。目前只处理 AI 音量。 */
static void prv_handle_dp_obj(dp_obj_recv_t *dpobj)
{
    for (uint16_t i = 0; i < dpobj->dpscnt; i++) {
        dp_obj_t *dp = &dpobj->dps[i];

        switch (dp->id) {
        case DP_ID_VOLUME:
            if (PROP_VALUE == dp->type) {
                PR_NOTICE("[main] 云端音量 -> %d", dp->value.dp_value);
                /* AI 音量（不是游戏/电影音量） */
                app_audio_set_volume(APP_AUDIO_CH_AI, dp->value.dp_value);
            }
            break;

        default:
            PR_DEBUG("[main] 未处理的 DP %d", dp->id);
            break;
        }
    }
}

/* 音量滑条松手 -> 上报一次。拖动途中不上报（一次拖动会产生几十个
 * VALUE_CHANGED，全发上去既刷屏又没意义），松手一次即可。
 * 产品上只有 AI 音量这一路映射到云端 DP，所以别的一律忽略。
 *
 * 这个回调跑在**发布者线程**（LVGL 线程的滑条回调）里，tuya_iot_dp_obj_report
 * 只是把 DP 塞进发送队列，不阻塞等回包。 */
static void prv_vol_committed_cb(APP_EVT_E evt, void *data, void *user)
{
    (void)evt;
    (void)user;

    if ((int)(intptr_t)data == APP_AUDIO_CH_AI) {
        prv_report_volume_dp();
    }
}

/* ---- 语音常驻：MQTT 一连上就把语音栈拉起来（见 app_ai_chat.h）----
 *
 * **必须另起一条一次性线程，不能在本回调里直接拉。** 本回调跑在 tuya_yield
 * 线程上，那条栈只有 4KB（见下面 yield_cfg）；而 ai_chat_init() 要建 ~4 条线程、
 * 开 codec、写一次 KV。栈给 8KB —— 和路由线程一样，路由线程以前就是在这个栈上
 * 调 app_ai_chat_init() 的（角色页 on_enter）。
 *
 * 自退套路和 app_font.c / app_speaker.c 一样：函数最后一行 tal_thread_delete()
 * —— 它只是打一个异步标记，所以必须放在最后。 */
static THREAD_HANDLE s_ai_boot_thrd = NULL;

static void prv_ai_boot_thread(void *arg)
{
    (void)arg;

    app_ai_chat_net_ready();

    tal_thread_delete(s_ai_boot_thrd);
    s_ai_boot_thrd = NULL;
}

static void prv_ai_boot_start(void)
{
    THREAD_CFG_T cfg = {0};

    if (NULL != s_ai_boot_thrd) {
        return;                 /* 重连不重复拉；net_ready 本身也幂等 */
    }

    cfg.stackDepth = 8192;
    cfg.priority   = THREAD_PRIO_3;
    cfg.thrdname   = "ai_boot";
    if (OPRT_OK != tal_thread_create_and_start(&s_ai_boot_thrd, NULL, NULL,
                                               prv_ai_boot_thread, NULL, &cfg)) {
        PR_ERR("[main] 语音常驻线程起不来，语音用不了");
        s_ai_boot_thrd = NULL;
    }
}

/* 被云端解绑后停住云状态机（见 TUYA_EVENT_RESET 的处理）。 */
static volatile BOOL_T s_cloud_held = FALSE;

/* 网络 / 云事件，跑在 yield 线程 */
static void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    (void)client;

    PR_DEBUG("[main] Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("[main] Device Bind Start!");
        if (app_ai_chat_active()) {
            app_audio_play_alert(APP_AUDIO_ALERT_NETCFG);
        }
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        PR_INFO("[main] MQTT 已连接");
        app_net_set_cloud_up(TRUE);
        prv_report_volume_dp();
        /* 语音常驻：从这一刻起一直听（见 app_ai_chat.h）。细节见 prv_ai_boot_start。 */
        prv_ai_boot_start();
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ:
        prv_handle_dp_obj(event->value.dpobj);
        break;

    /* 云端把设备判成"未激活/被解绑"时，SDK 会发 TUYA_EVENT_RESET。
     * **只有真正的恢复出厂才重启**：以前无脑 tal_system_reset()，MQTT 认证被拒
     * 一次就重启一次（真机 2026-10-28：无限重启，且热复位把 SD 卡留在半初始化
     * 状态，卡也跟着起不来）。普通解绑只停掉云状态机，界面继续可用。 */
    case TUYA_EVENT_RESET: {
        int32_t why = event->value.asInteger;

        if (TUYA_RESET_TYPE_REMOTE_FACTORY == why) {
            /* **不复位。** 发这个事件的地方已经置了 nextstate=STATE_RESET，
             * 状态机会自己跑到 run_state_reset() 清掉本地激活数据再重新开始。
             * 这里直接 tal_system_reset() 反而把它截断了：激活数据没清，重启后
             * 用同样的凭据再连一次，云端再判一次 reset_factory —— 无限重启。 */
            PR_WARN("[main] 云端恢复出厂（%d），不复位：交给 SDK 清激活数据", (int)why);
        } else {
            PR_WARN("[main] 云端解绑（%d），保持运行：界面可用，云功能停用", (int)why);
            app_net_set_cloud_up(FALSE);
            s_cloud_held = TRUE;
        }
    } break;

    default:
        break;
    }
}

/* tuya_iot 的联网判定回调。为 NULL 会让状态机一直睡下去、MQTT 永远不上线。 */
bool user_network_check(void)
{
    return (TRUE == app_net_is_up()) ? true : false;
}

static void tuya_yield_thread(void *arg)
{
    (void)arg;

    for (;;) {
        if (s_cloud_held) {
            /* 被云端解绑：不再驱动云状态机，否则会"认证失败 -> 重置 -> 重试"
             * 无限刷屏。界面和本地功能照常。 */
            tal_system_sleep(1000);
            continue;
        }
        tuya_iot_yield(&ai_client);
    }
}

/***********************************************************
************************** boot *****************************
***********************************************************/
static void user_main(void)
{
    int32_t ret;

    /* ---- 1. tal 基础设施（顺序与参考工程一致） ---- */
    cJSON_InitHooks(&(cJSON_Hooks){
        .malloc_fn = tal_psram_malloc,
        .free_fn   = tal_psram_free,
    });
    /* INFO：把 SDK 里高频的 DEBUG 刷屏（AI 上行分片、事件分发、每 2s 的性能
     * 统计等）全部关掉，只留 NOTICE/WARN/ERR。要查底层问题时改回 DEBUG。 */
    tal_log_init(TAL_LOG_LEVEL_INFO, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();
    tal_time_service_init();
    tal_cli_init();
    tuya_authorize_init();

    reset_netconfig_start();

    /* ---- 2. 应用框架：事件总线 + 配置中心（必须最先，其它服务都依赖它们） ---- */
    app_event_init();
    app_settings_init();

    /* 音量滑条松手 -> 上报云端 DP（只上报映射到云端的 AI 音量那一路） */
    app_event_subscribe(APP_EVT_VOLUME_COMMITTED, prv_vol_committed_cb, NULL);

    /* ---- 3. 设备授权 ---- */
    if (OPRT_OK != tuya_authorize_read(&license)) {
        license.uuid    = TUYA_OPENSDK_UUID;
        license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("[main] flash 里没有授权信息：用编译期 UUID/AUTHKEY，或先烧录授权");
    }

    ret = tuya_iot_init(&ai_client, &(const tuya_iot_config_t){
                                        .software_ver  = "1.0.0",
                                        .productkey    = TUYA_PRODUCT_ID,
                                        .uuid          = license.uuid,
                                        .authkey       = license.authkey,
                                        .event_handler = user_event_handler_on,
                                        .network_check = user_network_check,
                                    });
    if (OPRT_OK != ret) {
        PR_ERR("[main] tuya_iot_init 失败: %d", ret);
    }

    /* ---- 4. 服务层 ---- */
    app_net_init();             /* netmgr + AP/BLE 配网 + 状态巡检 */
    app_mem_sram("boot: net");
    app_weather_init();         /* 连上 WiFi 后拉取 3 日天气（订阅 APP_EVT_NET_STATUS） */
    app_perf_init();            /* 串口诊断命令 cpuload / tasklist（抢占排查用） */

    /* 卡在位检测（CD 引脚 P8）。**必须在 board_register_hardware() 之前** ——
     * 开机画面要显示"正在识别存储卡"的状态，得先知道卡在不在。P8 是裸 GPIO 输入，
     * 不依赖板级外设注册。 */
    if (OPRT_OK != app_storage_cd_init()) {
        PR_WARN("[main] 卡在位检测没起来，插拔不会自动响应");
    }

    /* 开机没插卡不再"照常启动"了 —— 卡是外挂 flash，没有它这个产品不成立。
     * 这里只报一条，真正的等待在 5.7（永久等，期间开机画面顶着）。
     * 用户至少知道机器是活的：logo 在屏幕上，串口每 5 秒报一次状态。 */
    if (!app_storage_card_present()) {
        PR_WARN("[main] 开机没有 SD 卡 —— 停在开机画面等插入");
    }

    board_register_hardware();  /* 板级外设：显示、音频、触摸 …… */
    app_mem_sram("boot: board hw");

    /* ---- 摄像头 bring-up（"只有摄像头"模式）------------------------------
     *
     * **放在这儿**：紧贴 board_register_hardware() 之后、任何显示代码之前。
     * 2026-10-16 排查：摄像头在 SCCB 上一直不应答，而 P51（板级当 DVP_RST）在平台
     * 默认 GPIO 表里是 `GPIO_DEV_LCD_G1` —— 要排除"LCD 把复位脚抢了"，就得让 LCD
     * 彻底不初始化。
     *
     * LCD 是在**板级**关掉的：`boards/T5AI/TUYA_T5AI_BOARD/tuya_t5ai_ex_module.c`
     * 里的 `AIPET_SKIP_DISPLAY`（当前 = 1）。不能走 app_default.config ——
     * `ENABLE_LIBLVGL` 会 select `ENABLE_DISPLAY`，注释掉 LCD 模块也没用。
     * 所以这里 board_register_hardware() 只注册了摄像头，没注册面板和触摸。
     *
     * **它不返回** —— 没屏了，后面的 app_disp_init / 桌宠页都跑不起来，
     * 所以拍完（或失败）就停在这儿，只留串口日志。开关在 app_cam_test.h。 */
#if APP_CAM_TEST
    app_cam_test_run();
#endif


    /* SDIO 线宽协商：先按 4 线起卡，起不来自动退 1 线（见 app_storage.h）。
     *
     * 位置不能随便挪：① 必须在**任何**模块碰 SD 卡之前 —— 线宽只在
     * bk_sd_card_init() 里生效一次，卡起来之后再改就晚了，而精灵引擎
     * （app_sprite2d）、字库（app_font）、天气缓存都会自己 mount；② 必须在
     * board_register_hardware() **之后** —— 4 线要把 GPIO 5/10/11 映射成 SDIO，
     * 得先让板级外设把它们注册完。
     * APP_STORAGE_SDIO_4BIT=0 时这里就是个空操作，挂载仍走老路径。
     * 失败只打日志不拦启动：卡不在时状态栏和联网照常。 */
    if (OPRT_OK != app_storage_sdio_setup()) {
        PR_WARN("[main] SD 卡没起来（%d 线），SD 相关功能不可用",
                app_storage_sdio_lines());
    }
    app_mem_sram("boot: sdio");

#if APP_SD_HW_TEST
    /* SD 读通路 A/B（CPU 轮询 vs GDMA）—— 驱动层测试，**不跑任何业务代码**：
     * 这一句不返回，显示 / 网络 / 字库 / 精灵 / AI 全都不启动，串口日志就是结论。
     * 位置同样在 app_storage_sdio_setup() 之后：SDIO 引脚已映射、卡已起来，
     * 测试自己会 deinit/init 切换通路。开关在 app_sd_hw_test.h。 */
    app_sd_hw_test_run();
#endif

#if APP_SD_TEST
    /* 纯 SD 读压力测试：只读卡，不带视频解码 / 音频 / LVGL。
     *
     * 位置是刻意的 —— 板级外设已注册（SDIO 引脚映射好了），而字体 / 预热 /
     * 音乐都还没起来，**全系统只有这一个 SD 读者**。这样才分得清是"SD 通路
     * 本身"的问题，还是被别的负载带出来的。阻塞，跑完才继续启动。
     * 排查结束把 app_sd_test.h 里的开关改回 0。 */
    app_sd_test_run();
#endif

    app_disp_init(NULL);        /* LVGL 挂到面板（还没开始渲染） */
    app_mem_sram("boot: disp");

    /* ---- 全产品横屏：**在开机画面之前**就把显示转 90° ----------------------
     *
     * 90° 是"摄像头朝上"的握持方向，逻辑坐标从 320x480 变成 480x320。
     *
     * **位置是刻意的**：开机画面（5.4）比桌宠页（5.8）先上屏。等桌宠页 on_enter
     * 里才转的话，用户会先看到一个竖着的 logo，然后当着面转一下 —— 很难看。
     * 桌宠页和 SD 门页都按这个坐标系排版（见 page_pet.c 的 PET_BODY_*、
     * app_statusbar.c 的 SB_W/SB_H）。 */
    {
        lv_display_t *disp = lv_display_get_default();

        if (NULL != disp) {
            lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
        }
    }

    app_audio_init();           /* 扬声器 + 恢复保存的音量 */
    app_mem_sram("boot: audio");

    /* 语音栈的内部锁。开机建（而不是用到时才懒建）：状态机有两个调用者线程
     * —— MQTT 就绪线程和路由线程，锁必须在它们可能跑起来之前就存在。 */
    app_ai_chat_prepare();

#if APP_BRINGUP_MIC_TEST
    /* 开机自检：麦克风采集 + VAD 人声判定（阻塞，跑完才继续）。
     *
     * 位置很关键——必须放在 app_disp_init / app_audio_init **之后**：
     * 音频管线（bk_voice）的任务栈走 PSRAM 堆，而 rtos_create_thread 在
     * PSRAM 堆/显示子系统就绪之前调用会直接失败
     * （[aec_alg] audio_thread_create failed -> tkl_ai_start voice start fail）。
     * 放在这里 = 和应用自己打开音频时机的时序完全一致。
     *
     * 结论拿到后把 app_bringup.h 里的开关改回 0。 */
    app_bringup_mic_run();
#endif

    app_time_init();            /* 1 秒滴答 + 秒/分/日事件 */
    app_key_init();             /* 系统按键（P12） */
    app_mem_sram("boot: time+key");

    /* ---- 5. 页面路由 ----
     * app_font_init() 必须在建任何界面**之前**：它建立两个"代理字体"，而
     * UI_FONT_CN / UI_FONT_21 就是这两个代理的地址 —— 界面把地址存进 style 之后，
     * 后台加载完 SD 字库只要改代理的 fallback，界面不用重建就生效。 */
    if (OPRT_OK != app_font_init()) {
        PR_ERR("[main] 字体代理初始化失败，界面将缺字");
    }

    /* ---- 5.4 开机画面 ----
     *
     * **先把 splash 顶上去，再让 LVGL 开跑** —— 用户看到的第一帧就是 logo。
     * 首页的建立被推到了 5.8（等 SD 就绪之后），中间这段（挂卡、链路就绪、三个
     * 目录索引预热完，约 6 秒）由 logo 顶着，视觉上就是"开机画面"。
     *
     * 原来的做法是首页建好之后才 app_disp_start()，所以开机第一帧直接就是首页，
     * 根本没有开机画面可言。现在这个顺序换了，但"不闪黑屏"仍然成立 —— 顶着的是
     * splash。见 app_splash.h。 */
    (void)app_splash_show();
    app_mem_sram("boot: splash");
    app_disp_start(APP_LVGL_PRIO, APP_LVGL_STACK);

    /* ---- 5.5 SD 卡完整字库 ----
     * 界面先用内置子集。字库不再自己起线程：它现在是**开机 SD 初始化流水线**的
     * 第一步，由下面 5.6 那条线程串行执行（读字库 -> 读精灵 -> 挂字库 -> 放行），
     * 理由见 app_preload.c 的 prv_boot_pipeline()：开机的 SD 访问只能有一个读者。 */

    /* ---- 5.6 SD 初始化 + 目录索引后台预热 ----
     * 这条线程现在负责整条开机 SD 初始化流水线（挂卡 -> 字库 -> 精灵 -> 挂字库），
     * 做完才放行需要 SD 的页面；顺带把目录索引扫好，用户点进去时 on_enter 直接
     * 命中缓存。最低优先级，且自己会等卡可用，所以放在最后、不占启动时间。
     * 见 app_preload.h。 */
    if (OPRT_OK != app_preload_init()) {
        PR_WARN("[main] 后台预热没起来，各页面照旧自己扫");
        /* **必须解除门控**：预热是"SD 页面放行"的唯一出口（见 app_storage_warming），
         * 线程没起来还不解锁的话，电影/音乐/游戏列表会被永久挡在门外。 */
        app_storage_warm_done();
    }

    /* ---- 5.7 等 SD 就绪（开机画面顶在这儿） ----
     *
     * 就绪 = 卡在位 + 链路正常 + 目录索引都预热完。**永久等待，不超时。**
     *
     * 卡对本产品相当于外挂 flash：精灵、字库、备忘录全在它上面，"卡不在"不是
     * 一种可降级的状态。以前给 10 秒超时、超时就弹状态页 —— 那条路在真机上
     * **必然崩溃**（2026-10-16 现场：没插卡 -> 门控挡下 pet -> 路由没初始化 ->
     * splash 还是活动屏幕却被删 -> LVGL 没有屏幕，Fault on thread lvgl）。
     * 现在等到底：没插卡就一直停在开机画面，插上就走（每 5 秒串口报一次状态）。 */
    (void)app_storage_wait_ready(0);

    /* ---- 5.8 主界面上屏，开机画面退场 ---- */
    app_pages_register();
    if (OPRT_OK != app_router_prepare(PAGE_PET)) {
        /* 走到这儿说明首页真的进不去（页面没注册之类）。**这时不能退场开机画面**
         * —— 它还是活动屏幕，删了 LVGL 就没屏幕了。app_splash_hide() 自己会判。 */
        PR_ERR("[main] 桌宠页进入失败");
    }
    app_splash_hide();
    app_mem_sram("boot: home ui");

#if APP_CAM_SNAPSHOT
    /* ---- 摄像头快照测试 ----
     *
     * **不阻塞启动**：起一个后台线程，睡 2 秒（让字库/精灵首帧/预热先跑完）再拍
     * 一张，JPEG 存 /sdcard/pictures，RGB565 留在 PSRAM 里给拍照页的取景框。
     * 开关在 app_cam_snapshot.h；测试完改 0 即可。
     *
     * 放在这里是因为：SD 已经就绪（5.7 等过）、首页已经上屏（界面线程在跑），
     * 而摄像头和 LVGL 共用 PSRAM/CPU —— 让启动序列先落地，再动它。 */
    app_cam_snapshot_start();
#endif

    /* 启动序列走完。音频/路由/LVGL 都起来了；字库是异步加载的，它完成时
     * 还会再打一条 —— 对照这一条就能看出"字库吃掉了多少、连续块掉到多少"。 */
    app_mem_sram("boot: ready");
    app_router_run(APP_ROUTER_STACK, APP_ROUTER_PRIO);

    /* 运行期拔卡由路由的 SD 门控接管（推 page_nosd，插回来自己撤掉），
     * 开机路径不再需要"没卡就弹状态页"—— 上面 5.7 已经等到底了。 */

    /* ---- 6. 联网 ----
     *
     * 对话驱动动作（MCP 工具）必须在联网**之前**挂上：MCP 服务器建在
     * EVENT_MQTT_CONNECTED 这个一次性事件上，而云端只有看到会话属性里的
     * supportCustomMCP 才会把本机工具喂给大模型。晚于首次连上再订阅，事件早就
     * 过去了，工具就永远登记不上。（同 ai_chat_main.c 里那个 latch 兜底是同一个坑。） */
    app_avatar_tools_init();

    tuya_iot_start(&ai_client);
    tkl_wifi_set_lp_mode(0, 0);
    reset_netconfig_check();

    /* 网络 / 云事件泵独占一个线程，其余线程各管各的 */
    {
        static THREAD_HANDLE s_yield_thrd = NULL;
        THREAD_CFG_T yield_cfg = {0};

        yield_cfg.stackDepth = 4096;
        yield_cfg.priority   = THREAD_PRIO_4;
        yield_cfg.thrdname   = "tuya_yield";
        tal_thread_create_and_start(&s_yield_thrd, NULL, NULL,
                                    tuya_yield_thread, NULL, &yield_cfg);
    }
}

/**
 * @brief main
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};

    thrd_param.stackDepth = 1024 * 8;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "tuya_app_main";

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
