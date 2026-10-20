/**
 * @file app_perf.c
 * @brief 串口性能诊断实现。设计说明见 app_perf.h。
 */

#include "tal_api.h"
#include "tal_cli.h"

#include <stdlib.h>
#include <string.h>

#include "app_perf.h"

/* 这两个是 bk_rtos 的调试接口（bk_rtos_debug.h）。那个头不在 app 的 include
 * 路径上，而函数在固件里是有的（app.map 里能搜到 rtos_dump_task_runtime_stats
 * 和 rtos_dump_task_list），所以跟 app_mem.c 处理 xPortGetMinimumEverFreeHeapSize
 * 一个路子：只 extern 用到的符号，不去碰 include 路径。
 *
 * 打印走的是 bk 的 dump 通路，和日志同一个 uart0。 */
extern void rtos_dump_task_list(void);
extern void rtos_dump_task_runtime_stats(void);

/* 自动采样节奏：开机风暴（配网 / 字库 / 精灵加载）先让它过去，再开始采。 */
#define PERF_FIRST_DELAY_MS 15000
#define PERF_PERIOD_MS      20000
#define PERF_AUTO_MAX       6
#define PERF_STACK          4096

/* cli_cmd_t 的 name/help 是 char*（非 const），用可写数组避免字面量告警
 * —— 与 app_weather.c 里 wcity 的写法保持一致。 */
static char sg_name_cpuload[]  = "cpuload";
static char sg_help_cpuload[]  = "per-task CPU load since boot";
static char sg_name_tasklist[] = "tasklist";
static char sg_help_tasklist[] = "task list (name/prio/stack left)";
static char sg_name_perf[]     = "perf";
static char sg_help_perf[]     = "perf now|off|<sec>: sample once / stop / restart auto";

static THREAD_HANDLE sg_thrd    = NULL;
static int           sg_period  = PERF_PERIOD_MS;
static int           sg_left    = PERF_AUTO_MAX;
static BOOL_T        sg_off     = FALSE;

static void prv_dump(const char *why)
{
    PR_NOTICE("[perf] ====== CPU 采样（%s）======", why);
    rtos_dump_task_runtime_stats();
}

/* ------------------------------------------------------------------ 命令 -- */

static void prv_cli_cpuload(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    prv_dump("命令 cpuload");
}

static void prv_cli_tasklist(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    PR_NOTICE("[perf] ====== 任务列表 ======");
    rtos_dump_task_list();
}

static void prv_cli_perf(int argc, char *argv[])
{
    if (argc >= 2 && NULL != argv[1]) {
        if (0 == strcmp(argv[1], "now")) {
            prv_dump("命令 perf now");
            return;
        }
        if (0 == strcmp(argv[1], "off")) {
            sg_off = TRUE;
            PR_NOTICE("[perf] 自动采样已关");
            return;
        }
        int sec = atoi(argv[1]);
        if (sec > 0) {
            sg_period = sec * 1000;
            sg_left   = PERF_AUTO_MAX;
            sg_off    = FALSE;
            PR_NOTICE("[perf] 自动采样：每 %d 秒一次，再采 %d 次", sec, sg_left);
            return;
        }
    }
    PR_NOTICE("[perf] 用法：perf now | perf off | perf <秒>");
}

static const cli_cmd_t sg_cli_cmds[] = {
    { sg_name_cpuload,  sg_help_cpuload,  prv_cli_cpuload  },
    { sg_name_tasklist, sg_help_tasklist, prv_cli_tasklist },
    { sg_name_perf,     sg_help_perf,     prv_cli_perf     },
};

/* -------------------------------------------------------------- 自动采样 -- */

static void prv_task(void *arg)
{
    int n = 0;

    (void)arg;
    PR_NOTICE("[perf] 自动采样线程就绪：%d ms 后开始，每 %d ms 一次，共 %d 次",
              PERF_FIRST_DELAY_MS, PERF_PERIOD_MS, PERF_AUTO_MAX);

    tal_system_sleep(PERF_FIRST_DELAY_MS);

    for (;;) {
        if (sg_off || sg_left <= 0) {
            /* 采完或关掉后不退出：留着给 perf <秒> 重新开。 */
            tal_system_sleep(5000);
            continue;
        }
        sg_left--;
        n++;
        prv_dump("自动");
        PR_NOTICE("[perf] 自动采样 %d/%d，剩余 %d 次（perf off 可停）",
                  n, PERF_AUTO_MAX, sg_left);
        tal_system_sleep(sg_period);
    }
}

void app_perf_init(void)
{
    OPERATE_RET  rt;
    THREAD_CFG_T cfg;

    if (OPRT_OK != (OPERATE_RET)tal_cli_cmd_register(sg_cli_cmds,
            sizeof(sg_cli_cmds) / sizeof(sg_cli_cmds[0]))) {
        PR_WARN("[perf] 命令注册失败（诊断命令不可用，自动采样照旧）");
    } else {
        PR_NOTICE("[perf] 诊断命令就绪：cpuload / tasklist / perf");
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = PERF_STACK;
    cfg.priority   = THREAD_PRIO_4;     /* 最低档，别跟业务抢 */
    cfg.thrdname   = "perf_smp";

    rt = tal_thread_create_and_start(&sg_thrd, NULL, NULL, prv_task, NULL, &cfg);
    if (OPRT_OK != rt) {
        PR_ERR("[perf] 自动采样线程创建失败: %d", rt);
    }
}
