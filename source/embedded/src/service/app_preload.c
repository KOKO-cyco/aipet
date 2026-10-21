/**
 * @file app_preload.c
 * @brief 开机后台把 SD 链路拉起来，然后放行"需要 SD 的页面"。
 *
 * 这个线程现在只做两件事（老产品里它还顺带扫电影/音乐/游戏目录，那些都删了）：
 *
 *   1) `app_storage_prepare()` —— 把 SD 链路拉起来。**这是全工程唯一的调用点。**
 *   2) `app_storage_warm_done()` —— 解除门控（见 app_storage.h 的 warming）。
 *
 * 为什么门控还得留着
 * ------------------
 * 宠物页声明了 `APP_PAGE_NEEDS_SD`（精灵和字库都在卡上）。开机那一两秒卡还没
 * 热身完，门控挡着它，免得页面进去之后发现读不出东西。**门控只有这一条出口** ——
 * 少了 `app_storage_warm_done()`，宠物页会被永久挡在门外。
 *
 * 换卡也管
 * --------
 * `app_storage_gen()` 在每次拔插 / 链路重建成功时 +1。代次变了就重跑一遍
 * prepare，所以换了卡不用重启。
 */

#include "tal_api.h"

#include "app_preload.h"
#include "app_storage.h"
#include "app_font.h"
#include "app_sprite2d.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define PRE_STACK       12288
/* **不能取 4096。** 这条线程走的是 FatFs 读盘调用链
 * （app_storage_prepare -> tkl_fread -> disk_read）。老产品的音乐解码任务
 * 4KB 栈真机上被踩穿过，表现是一句毫不相干的 `Assert at: xQueueGiveFromISR`
 * —— 同一个调用链。合并开机流水线之后它还调用字库解析（原来在自己的 8KB
 * 线程里）和精灵初始化（原来在 8KB 的路由线程里），取两者的最大值再留点余量。
 * 线程栈在 PSRAM 里，不心疼。 */
#define PRE_PRIO        THREAD_PRIO_4   /* 最低档：谁也不许被它挤到（见 .h） */
#define PRE_TICK_MS     250

/* 开机后先让路多久再开始。
 *
 * 这一轮是全机**唯一**的开机 SD 初始化，跑得越早越好，但要等开机序列本身
 * （显示、状态栏、页面注册）建完，免得它和 LVGL 抢 CPU 导致初始化期间的读被
 * 打断。3 秒是实测"页面已经建好"的时刻。 */
#define PRE_START_DELAY_MS  3000

/***********************************************************
***********************variable define**********************
***********************************************************/
static THREAD_HANDLE sg_thrd = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/
/* 开机初始化：**一条流水线，一个读者**。
 *
 * 以前这里是"三拨人各起一个线程各读各的"：字库任务（睡 1.5s，读 6 个文件约
 * 9MB）、精灵模块（页面进来时读 .spf 头 + 790KB idle 常驻 RLE + 各组首帧）、
 * 再加上 LVGL 线程按需读字形 —— 同一根 SDIO 上三路并发。表现是开机必现的
 * "idle 的 RLE 读失败"、随机短读、CRC 错，运气不好还把链路读死、只能重建
 * （重建又降到 1 线，整机掉帧）。
 *
 * 现在按顺序全在本线程里做完。**顺序是"最怕出错的先做"**：
 *
 *   1) app_storage_prepare()    挂卡 + 验证（由调用方先做）
 *   2) app_sprite2d_init()      .spf 头 + idle 常驻 RLE + 各组首帧
 *                               —— 趁卡最安静的时候读，开机最容易失败的就是它
 *   3) app_storage_warm_done()  放行需要 SD 的页面（宠物页）—— 到这里主界面可用
 *   4) app_font_sd_load()       六档字库（约 9MB，慢；页面已经能用了，不挡人）
 *   5) app_font_sd_attach()     挂上字库 —— **从这里起** LVGL 才会按需读字形
 *
 * 第 4 步跑的时候宠物页已经在动，预取线程会来抢卡 —— 但两边都走 SD 临界区
 * （app_storage_busy_enter），物理上仍然只有一个读者，不会再出现并发读坏。
 * 换卡（代次变了）会重跑：字库和精灵都"已经读过就跳过"，不会重复读。 */
static void prv_sd_pipeline(void)
{
    /* 精灵先读：idle 常驻 790KB 是开机最脆弱的一次读，放在字体那 9MB 之前。 */
    (void)app_sprite2d_init();

    /* 页面可以进了：宠物此时已经是"待机常驻 + 首帧常驻"，进页面不读卡。 */
    app_storage_warm_done();

    /* 字库慢慢读：失败只影响字体（内置占位），不影响已经起来的宠物。 */
    (void)app_font_sd_load();
    app_font_sd_attach();       /* 最后才让 LVGL 开始按需读卡 */
}

/* 现在能不能碰 SD？
 *
 *   ABSENT  卡不在     -> 碰也碰不出来
 *   DAMAGED 链路判死   -> 同上，等换卡
 *   OK / SETTLING 之前 -> **必须碰**：warming 正是靠这一轮来解除的
 */
static BOOL_T prv_may_prepare(void)
{
    APP_SD_FAULT_E f = app_storage_fault();

    return (APP_SD_ABSENT != f && APP_SD_DAMAGED != f) ? TRUE : FALSE;
}

static void prv_task(void *arg)
{
    uint32_t done_gen = 0;      /* 已经按哪一代处理过；0 = 还没处理过（初值 1 起） */

    (void)arg;
    PR_NOTICE("[pre] SD 就绪线程启动（%ums 后开始）", (unsigned)PRE_START_DELAY_MS);

    tal_system_sleep(PRE_START_DELAY_MS);

    for (;;) {
        uint32_t gen = app_storage_gen();

        if (gen != done_gen) {
            if (!prv_may_prepare()) {
                /* 卡不在 / 链路已判死：**没有"就绪"可言，必须解除门控**。
                 *
                 * 漏了这一步的话，门页会永远停在"正在识别存储卡…" —— 因为
                 * warming 只有这一条路会清掉，而真实原因（请插入 / 损坏）就再也
                 * 说不出来了。 */
                PR_NOTICE("[pre] 卡不可用，放行（门页显示真实原因）");
                app_storage_warm_done();
                done_gen = gen;     /* 这一代没什么可做的；换卡后代次会变，自然重来 */
            } else if (OPRT_OK != app_storage_prepare()) {
                /* 链路起不来（多半刚被判成 APP_SD_DAMAGED）。不在这里解除门控 ——
                 * 下一轮 prv_may_prepare() 会拦住并解除，那时 fault 也已经是
                 * DAMAGED，门页自然显示"SD 卡损坏"。只等一拍。 */
                PR_WARN("[pre] 链路起不来，本轮不处理");
            } else {
                /* 串行做完：精灵 -> 放行页面 -> 字库 -> 挂字库。
                 * warm_done 在流水线**中间**调（见 prv_sd_pipeline 的说明），
                 * 不再在这里补一次。 */
                prv_sd_pipeline();
                PR_NOTICE("[pre] SD 初始化流水线完成（第 %u 代）", (unsigned)gen);
                done_gen = gen;
            }
        }

        tal_system_sleep(PRE_TICK_MS);
    }
}

OPERATE_RET app_preload_init(void)
{
    OPERATE_RET  rt;
    THREAD_CFG_T cfg;

    if (NULL != sg_thrd) {
        return OPRT_OK;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = PRE_STACK;
    cfg.priority   = PRE_PRIO;
    cfg.thrdname   = "preload";

    rt = tal_thread_create_and_start(&sg_thrd, NULL, NULL, prv_task, NULL, &cfg);
    if (OPRT_OK != rt) {
        PR_ERR("[pre] SD 就绪线程创建失败: %d", rt);
        return rt;
    }
    return OPRT_OK;
}
