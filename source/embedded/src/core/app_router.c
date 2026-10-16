/**
 * @file app_router.c
 * @brief 页面路由器实现。语义见 app_router.h。
 */

#include <string.h>

#include "tal_api.h"

#include "app_disp.h"
#include "app_router.h"
#include "app_storage.h"        /* app_storage_fault() —— SD 故障判定，见 prv_enter */

#define APP_ROUTE_PAGE_MAX   12

/* ---- 页面注册表 --------------------------------------------------------- */
static const APP_PAGE_T *sg_pages[APP_ROUTE_PAGE_MAX];
static int32_t           sg_page_cnt = 0;

/* ---- 页面栈 ------------------------------------------------------------- */
typedef struct {
    const APP_PAGE_T *page;
    void             *arg;
    BOOL_T            entered;      /* on_enter 已经成功执行过 */
} ROUTE_ENTRY_T;

static ROUTE_ENTRY_T sg_stack[APP_ROUTE_DEPTH_MAX];
static volatile int32_t sg_depth = 0;

/* ---- 导航请求槽（单槽位，跨线程只写不阻塞） ------------------------------ */
typedef enum {
    RR_NONE = 0,
    RR_PUSH,
    RR_POP,
    RR_HOME,
    RR_SD_DAMAGED,      /* 收到首页 + 压 SD 故障页 */
} RR_E;

/* 请求槽：指针本身是 volatile（"内容"是 const），最后写 sg_req 作为有效标志。 */
static volatile RR_E              sg_req       = RR_NONE;
static const APP_PAGE_T *volatile sg_req_page  = NULL;
static void             *volatile sg_req_arg   = NULL;
static volatile int32_t           sg_req_n     = 0;
static volatile BOOL_T            sg_leave     = FALSE;

static THREAD_HANDLE sg_thrd = NULL;

/* "卡不在时的门页"名字（app_router_set_sd_gate 注册）。NULL = 没启用这个机制。 */
static const char *sg_sd_gate = NULL;

/* 卡插拔回调。订阅在 app_router_prepare() 里，定义在文件靠后 —— 先声明。 */
static void prv_sd_cb(APP_EVT_E evt, void *data, void *user);

/***********************************************************
************************* helpers **************************
***********************************************************/
static const APP_PAGE_T *prv_find(const char *name)
{
    if (NULL == name) {
        return NULL;
    }
    for (int32_t i = 0; i < sg_page_cnt; i++) {
        if (0 == strcmp(sg_pages[i]->name, name)) {
            return sg_pages[i];
        }
    }
    return NULL;
}

static const APP_PAGE_T *prv_top(void)
{
    int32_t d = sg_depth;

    return (d > 0) ? sg_stack[d - 1].page : NULL;
}

/* 栈顶**下面**那一页是不是 SD 门页。
 *
 * 为什么不能直接用 prv_top()：prv_enter() 是在 prv_push() 把新页压进去**之后**
 * 才被调用的（sg_depth 已经算上它了），所以要往下看一层。
 *
 * 用途见 prv_enter 的门控：用户对着"请插入 SD 卡"页反复点电影/游戏时，每次都会
 * 走一遍"拦下 -> 推门页"。不判这一下的话，每点一次栈里就多压一层门页
 * （真机日志里 `[route] enter nosd` 连出两条就是这么来的），点够 8 次栈满，
 * 之后就谁也进不去了。 */
static BOOL_T prv_gate_below(void)
{
    if (NULL == sg_sd_gate || sg_depth < 2) {
        return FALSE;
    }
    return (0 == strcmp(sg_stack[sg_depth - 2].page->name, sg_sd_gate)) ? TRUE : FALSE;
}

static OPERATE_RET prv_post(RR_E kind, const APP_PAGE_T *page, void *arg, int32_t n)
{
    if (RR_NONE != sg_req) {
        PR_DEBUG("[route] 上一个导航请求还没处理，本次丢弃");
        return OPRT_COM_ERROR;
    }
    sg_req_page = page;
    sg_req_arg  = arg;
    sg_req_n    = n;
    sg_req      = kind;                 /* 最后写，作为"请求有效"的标志 */
    return OPRT_OK;
}

/***********************************************************
*********************** page entering **********************
***********************************************************/
/* 进入/返回一个页面。显示归属规则见 app_router.h。 */
static OPERATE_RET prv_enter(ROUTE_ENTRY_T *e, BOOL_T first)
{
    const APP_PAGE_T *p = e->page;
    OPERATE_RET       rt = OPRT_OK;

    /* 需要 SD 卡的页面：**首次进入**时卡不可用、或目录索引还没预热完，就别进
     * —— 推门页挡上去，并让本次进入当成失败退回上一层。栈于是变成
     * [..., 上一层, 门页]，用户按返回就回到上一层，不会卡死；下次再进这一页还会
     * 被拦，这就是"每次重进都检测"。
     *
     * 判据是 `app_storage_ready() && !app_storage_warming()`：
     *   - 卡不在 / 链路判死   -> 门页显示"请插入 SD 卡"/"SD 卡损坏"
     *   - 卡在、但预热没跑完  -> 门页显示"正在识别存储卡…"，扫完自动撤掉
     * 后一种尤其重要：放进去的话，on_enter 会**现场扫目录 3 秒**，LVGL 又没挂起，
     * 用户看到的就是"点了没反应"。挡在门外反而是最快的路径 —— 等预热完再进，
     * 缓存是热的、秒开。
     *
     * **只在 first 时查**：从门页按返回是 resume 而不是 first，那时再拦一次就会
     * 把用户弹回门页、永远出不来。
     *
     * 请求槽在路由主循环里是**先清后派发**的，所以这里推页是安全的：请求先落槽，
     * 等本次进入彻底收尾之后才被消费。
     *
     * 播到一半卡被拔掉是另一条路：路由线程此刻正被阻塞式播放页占着，推不进去。
     * 那条路由**播放页自己**发现（`app_storage_ready()` 变假）后收摊退出，
     * 请求槽里的门页推送等它退出后自然生效。 */
    if (first && (p->flags & APP_PAGE_NEEDS_SD) && (NULL != sg_sd_gate)
        && (!app_storage_ready() || app_storage_warming())) {
        /* 门页**已经压在下面**了：只把这次进入当失败退回去，别再推一层。
         * 用户对着门页反复点电影/游戏时，每点一次都会走到这里 —— 不判这一下
         * 栈里会一层层堆门页，堆满 8 层之后就谁也进不去了。见 prv_gate_below。 */
        if (prv_gate_below()) {
            PR_DEBUG("[route] %s 已被 %s 挡着，不重复推", p->name, sg_sd_gate);
            return OPRT_COM_ERROR;
        }
        PR_WARN("[route] %s 要 SD 卡而卡%s，改推 %s", p->name,
                app_storage_warming() ? "还在预热" : "不可用", sg_sd_gate);
        (void)app_router_push(sg_sd_gate, NULL);
        return OPRT_COM_ERROR;
    }

    /* **门页本身：SD 其实没问题就别进。**
     *
     * 会走到这里是因为推送和消费之间有延迟：拔卡那一刻推了门页，而它躺在请求槽里
     * 等阻塞式播放页收摊；这期间卡又插回来了（真机日志里就有这种抖动），等请求
     * 被消费时卡已经恢复 —— 这时再把门页推上去，就**没有任何事件能把它撤掉**了
     * （撤掉靠的是"插入"事件，而那个事件早就过去了），用户会卡在一个"请插入 SD 卡"
     * 页上出不来。
     *
     * 判据必须跟上面那条**完全对称**（同样是 ready 且非 warming），否则两条会打架：
     * 上面拦下、这里又拒进，页面就彻底进不去了。 */
    if (first && (NULL != sg_sd_gate) && (0 == strcmp(p->name, sg_sd_gate))
        && app_storage_ready() && !app_storage_warming()) {
        PR_DEBUG("[route] SD 已就绪，不进 %s", p->name);
        return OPRT_COM_ERROR;
    }

    /* 独占面板的页面：先冻结 LVGL，它才去开自己的显示通道 */
    if (p->flags & APP_PAGE_OWN_DISPLAY) {
        app_disp_suspend();
    }

    if (first || NULL == p->on_resume) {
        if (NULL != p->on_enter) {
            rt = p->on_enter(e->arg);
        }
    } else {
        rt = p->on_resume();
    }
    e->entered = (OPRT_OK == rt) ? TRUE : FALSE;

    /* 普通页面：界面建好之后再交还 LVGL，避免闪出上一页 */
    if (0 == (p->flags & APP_PAGE_OWN_DISPLAY)) {
        app_disp_resume();
    }

    PR_NOTICE("[route] %s %s -> %d", first ? "enter" : "resume", p->name, rt);

    /* 通知"换页了"。放在最后、且**只在成功时发**：on_enter 失败时路由会自动弹回
     * 上一层，那次弹回自己会再发一条 —— 订阅方不会收到一个其实没进去过的页。 */
    if (OPRT_OK == rt) {
        APP_PAGE_EVT_T pe = {
            .page    = p->name,
            .overlay = (0 == (p->flags & APP_PAGE_NO_OVERLAY)),
        };

        app_event_publish(APP_EVT_PAGE_CHANGED, &pe);
    }
    return rt;
}

static OPERATE_RET prv_push(const APP_PAGE_T *p, void *arg)
{
    ROUTE_ENTRY_T *e;
    OPERATE_RET    rt;

    if (NULL == p) {
        return OPRT_INVALID_PARM;
    }
    if (sg_depth >= APP_ROUTE_DEPTH_MAX) {
        PR_ERR("[route] 页面栈已满(%d)，无法进入 %s", APP_ROUTE_DEPTH_MAX, p->name);
        return OPRT_COM_ERROR;
    }

    /* 通知当前页"要被盖住了"：关弹窗、 park 自己的数据源。仍在 LVGL 线程上下文
     * 之外，所以此时拿 app_disp_lock() 是安全的。 */
    if (sg_depth > 0) {
        ROUTE_ENTRY_T *cur = &sg_stack[sg_depth - 1];

        if (cur->entered && NULL != cur->page->on_pause) {
            cur->page->on_pause();
        }
    }

    e = &sg_stack[sg_depth];
    e->page    = p;
    e->arg     = arg;
    e->entered = FALSE;
    sg_depth++;

    rt = prv_enter(e, TRUE);
    if (OPRT_OK != rt) {
        /* 进入失败：自己清理，退回上一层（原来是各页面各自 resume） */
        PR_ERR("[route] 进入 %s 失败(%d)，退回上层", p->name, rt);
        if (e->entered && NULL != p->on_exit) {
            p->on_exit();
        }
        sg_depth--;
        if (sg_depth > 0) {
            prv_enter(&sg_stack[sg_depth - 1], FALSE);
        }
    }
    return rt;
}

/* 弹出 count 层。注意只对**最终露出的那一层**做"返回"处理：中间被跳过的层直接
 * 销毁，不能先恢复再销毁（否则退出游戏时会先跑起来立刻又被停掉）。 */
static void prv_pop(int32_t count)
{
    int32_t target = sg_depth - ((count > 0) ? count : 1);

    if (target < 1) {
        target = 1;                         /* 首页不弹出 */
    }
    if (target >= sg_depth) {
        return;                             /* 已经在目标层上了 */
    }

    while (sg_depth > target) {
        ROUTE_ENTRY_T *top = &sg_stack[sg_depth - 1];

        if (top->entered && NULL != top->page->on_exit) {
            PR_NOTICE("[route] exit %s", top->page->name);
            top->page->on_exit();
        }
        sg_depth--;
    }

    /* 露出下层：不是"首次进入"，走 on_resume */
    prv_enter(&sg_stack[sg_depth - 1], FALSE);
}

/***********************************************************
************************* key / evt ************************
***********************************************************/
static void prv_dispatch_key(APP_KEY_E key, APP_KEY_EVT_E evt)
{
    const APP_PAGE_T *p = prv_top();

    if (NULL != p && NULL != p->on_key) {
        p->on_key(key, evt);
        return;
    }
    /* 默认行为：返回键"按下"即弹出当前页（与改造前的按键手感一致） */
    if (APP_KEY_BACK == key && APP_KEY_EVT_DOWN == evt && sg_depth > 1) {
        prv_post(RR_POP, NULL, NULL, 1);
    }
}

/* 事件总线只转发给栈顶页面：看不见的页面不需要响应 */
static void prv_on_event(APP_EVT_E evt, void *data, void *user)
{
    const APP_PAGE_T *p;

    (void)user;
    p = prv_top();
    if (NULL != p && NULL != p->on_event) {
        p->on_event(evt, data);
    }
}

/***********************************************************
************************ route task ************************
***********************************************************/
static void prv_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 1) 阻塞式页面的离开请求 */
        if (sg_leave) {
            sg_leave = FALSE;
            prv_pop(1);
        }

        /* 2) 消费一个导航请求（先取全部字段，再清标志） */
        RR_E kind = sg_req;
        if (RR_NONE != kind) {
            const APP_PAGE_T *p  = sg_req_page;
            void             *pa = sg_req_arg;
            int32_t           n  = sg_req_n;

            sg_req = RR_NONE;

            switch (kind) {
            case RR_PUSH:
                if (NULL != p) {
                    prv_push(p, pa);
                }
                break;
            case RR_POP:
                prv_pop((n > 0) ? n : 1);
                break;
            case RR_HOME:
                prv_pop(sg_depth - 1);
                break;
            case RR_SD_DAMAGED:
                /* 收到首页再压故障页。栈里那些页面（电影目录 / 音乐列表 / 播放页）
                 * 同样需要卡，留着只会让"返回"落在一个同样读不出东西的页上。 */
                prv_pop(sg_depth - 1);
                if (NULL != sg_sd_gate) {
                    const APP_PAGE_T *gate = prv_find(sg_sd_gate);

                    if (NULL != gate) {
                        prv_push(gate, NULL);
                    }
                }
                break;
            default:
                break;
            }
        }

        /* 3) 系统按键：采样 + 分发 */
        app_key_poll();
        {
            APP_KEY_EVT_E e;

            while ((e = app_key_take(APP_KEY_BACK)) != APP_KEY_EVT_NONE) {
                prv_dispatch_key(APP_KEY_BACK, e);
            }
        }

        tal_system_sleep(APP_KEY_TICK_MS);
    }
}

/***********************************************************
************************* public api ***********************
***********************************************************/
OPERATE_RET app_router_register(const APP_PAGE_T *page)
{
    if (NULL == page || NULL == page->name) {
        return OPRT_INVALID_PARM;
    }
    if (NULL != prv_find(page->name)) {
        PR_WARN("[route] 页面 %s 重复注册，忽略", page->name);
        return OPRT_OK;
    }
    if (sg_page_cnt >= APP_ROUTE_PAGE_MAX) {
        PR_ERR("[route] 页面注册表已满(%d)", APP_ROUTE_PAGE_MAX);
        return OPRT_COM_ERROR;
    }
    sg_pages[sg_page_cnt++] = page;
    return OPRT_OK;
}

OPERATE_RET app_router_prepare(const char *home_page)
{
    const APP_PAGE_T *home = prv_find(home_page);

    if (NULL == home) {
        PR_ERR("[route] 首页 %s 没有注册", (NULL != home_page) ? home_page : "(null)");
        return OPRT_NOT_FOUND;
    }

    /* 事件总线 -> 栈顶页面。所有类型都订阅，由页面自己过滤。 */
    for (int32_t i = APP_EVT_NONE + 1; i < APP_EVT_MAX; i++) {
        app_event_subscribe((APP_EVT_E)i, prv_on_event, NULL);
    }

    /* 卡插拔单独走一路：它不是"转发给页面"，而是路由自己的策略
     * （卡不在时拦下 APP_PAGE_NEEDS_SD 的页面，见 prv_sd_cb）。 */
    app_event_subscribe(APP_EVT_SD_STATUS, prv_sd_cb, NULL);

    sg_depth = 0;
    return prv_push(home, NULL);
}

OPERATE_RET app_router_run(uint32_t stack, uint32_t prio)
{
    THREAD_CFG_T tc = {0};

    if (NULL != sg_thrd) {
        return OPRT_OK;
    }
    if (0 == sg_depth) {
        PR_ERR("[route] 还没调用 app_router_prepare()");
        return OPRT_COM_ERROR;
    }

    tc.stackDepth = stack;
    tc.priority   = prio;
    tc.thrdname   = "ui_router";

    OPERATE_RET rt = tal_thread_create_and_start(&sg_thrd, NULL, NULL,
                                                 prv_task, NULL, &tc);
    if (OPRT_OK != rt) {
        PR_ERR("[route] 路由线程创建失败: %d", rt);
        return rt;
    }

    PR_NOTICE("[route] 路由线程已启动（首页 %s）", sg_stack[0].page->name);
    return OPRT_OK;
}

OPERATE_RET app_router_push(const char *page, void *arg)
{
    const APP_PAGE_T *p = prv_find(page);

    if (NULL == p) {
        PR_ERR("[route] 没有这个页面: %s", (NULL != page) ? page : "(null)");
        return OPRT_NOT_FOUND;
    }
    return prv_post(RR_PUSH, p, arg, 0);
}

OPERATE_RET app_router_pop(int32_t count)
{
    if (count <= 0) {
        return OPRT_INVALID_PARM;
    }
    return prv_post(RR_POP, NULL, NULL, count);
}

OPERATE_RET app_router_go_home(void)
{
    return prv_post(RR_HOME, NULL, NULL, 0);
}

void app_router_leave(void)
{
    sg_leave = TRUE;
}

const char *app_router_current(void)
{
    const APP_PAGE_T *p = prv_top();

    return (NULL != p) ? p->name : NULL;
}

int32_t app_router_depth(void)
{
    return sg_depth;
}

BOOL_T app_router_on_top(const char *page)
{
    const APP_PAGE_T *p = prv_top();

    return (NULL != p && NULL != page && 0 == strcmp(p->name, page)) ? TRUE : FALSE;
}

BOOL_T app_router_overlay_ok(void)
{
    const APP_PAGE_T *p = prv_top();

    /* 还没进任何页面时按"允许"算：那是开机建首页的瞬间，浮层也还没起来。 */
    return (NULL == p || 0 == (p->flags & APP_PAGE_NO_OVERLAY)) ? TRUE : FALSE;
}

OPERATE_RET app_router_set_sd_gate(const char *page)
{
    sg_sd_gate = page;
    return OPRT_OK;
}

OPERATE_RET app_router_sd_damaged(void)
{
    return prv_post(RR_SD_DAMAGED, NULL, NULL, 0);
}

/* 卡插拔通知。跑在存储服务的软件定时器线程里，所以只能做线程安全的事 ——
 * app_router_push()/pop() 正是为此设计的（写请求槽，异步生效）。 */
static void prv_sd_cb(APP_EVT_E evt, void *data, void *user)
{
    BOOL_T            present = ((intptr_t)data != 0);
    const APP_PAGE_T *top     = prv_top();

    (void)evt;
    (void)user;

    if (NULL == sg_sd_gate || NULL == top) {
        return;
    }

    if (!present) {
        /* 卡拔了：当前页要卡就把门页推上去。
         * 阻塞式播放页此刻正占着路由线程 —— 请求先落槽，等它收摊退出后生效
         * （播放页自己会发现 app_storage_fault() 变非 OK 然后收摊）。 */
        if (0 != (top->flags & APP_PAGE_NEEDS_SD)) {
            PR_NOTICE("[route] 卡被拔出，当前页 %s 需要卡 -> 推 %s",
                      top->name, sg_sd_gate);
            (void)app_router_push(sg_sd_gate, NULL);
        }
    } else if (app_router_on_top(sg_sd_gate) && !app_storage_warming()) {
        /* 卡插回来了、而且预热也跑完了：撤掉门页，**一路回到首页**。
         *
         * 不回到原来那一页（电影目录 / 音乐列表）：它们的数据是拔卡前扫出来的，
         * 而且回来之后用户还得自己再点一次。回到首页 = 一切从头来 —— 和"每次重进
         * 都加载 SD 卡"是同一套语义，也是故障页上那个返回键的去向。
         *
         * 预热没完就**不撤**：那时页面进去了也要现场扫 3 秒，门页上那句
         * "正在识别存储卡…"才是对的。撤掉由门页自己的定时器在预热结束时发起
         * （见 page_nosd.c）。 */
        PR_NOTICE("[route] 卡已插回且已就绪，撤掉 %s 并回到首页", sg_sd_gate);
        (void)app_router_pop(sg_depth - 1);
    }
}
