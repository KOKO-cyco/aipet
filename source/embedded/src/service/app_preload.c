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

/***********************************************************
************************macro define************************
***********************************************************/
#define PRE_STACK       8192
/* **不能取 4096。** 这条线程走的是 FatFs 读盘调用链
 * （app_storage_prepare -> tkl_fread -> disk_read）。老产品的音乐解码任务
 * 4KB 栈真机上被踩穿过，表现是一句毫不相干的 `Assert at: xQueueGiveFromISR`
 * —— 同一个调用链，所以照旧给 8KB。线程栈在 PSRAM 里，不心疼。 */
#define PRE_PRIO        THREAD_PRIO_4   /* 最低档：谁也不许被它挤到（见 .h） */
#define PRE_TICK_MS     250

/* 开机后先让路多久再开始。
 *
 * 开机的头几秒 SD 上挤着两拨人：SD 字库异步加载（睡 1.5 秒再读）和这一轮。
 * 字库那拨最要紧（缺字是肉眼可见的），让它先跑完 —— 它 1.5 秒起步、读两三百 KB，
 * 3 秒差不多。 */
#define PRE_START_DELAY_MS  3000

/***********************************************************
***********************variable define**********************
***********************************************************/
static THREAD_HANDLE sg_thrd = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/
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
                PR_NOTICE("[pre] SD 就绪（第 %u 代），放行", (unsigned)gen);
                app_storage_warm_done();
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
