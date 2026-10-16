/**
 * @file app_router.h
 * @brief 页面路由器：页面栈 + 导航请求队列 + 系统按键轮询。
 *
 * 一个页面 = 一个 APP_PAGE_T 结构 + 一组回调。加新功能（天气 / 音乐 / 电影）只需要
 * 写一个 page_xxx.c 导出 `const APP_PAGE_T app_page_xxx`，再去 app_pages.c 注册一行，
 * 不用碰任何已有代码。
 *
 * 栈语义
 * ------
 * 首页永远在栈底。push 只是压栈，**不会**调用下层的 on_exit —— 所以像"游戏暂停菜单"
 * 这种叠在游戏上的页面，退出后能回到原状态。on_exit 只在 pop（真正离开）时调用。
 *
 * 显示归属
 * --------
 * 带 APP_PAGE_OWN_DISPLAY 的页面自己往面板 framebuffer 里写（模拟器 / 角色动画）。
 * 路由会在进入这类页面**之前**冻结 LVGL、在离开**之后**恢复；普通 LVGL 页面则相反，
 * 先在挂起状态下把界面建好，再恢复 LVGL，避免闪出上一页的内容。
 * 页面自己完全不用关心 lv_vendor_suspend/resume。
 *
 * 线程模型
 * --------
 * - 所有 on_enter/on_resume/on_exit/on_key 都在**路由线程**里执行。这也是唯一
 *   允许调用 lv_vendor_suspend/resume 的地方。
 * - 触摸回调等其它线程要跳页面，用 app_router_push()/pop()，它们只往请求槽里写一个
 *   请求，真正的切换由路由线程执行（单槽位，人手速度足够）。
 * - 有页面阻塞在 on_enter 里（角色动画页就是这个模式）时，路由线程被占住：该页面要
 *   自己在循环里调 app_key_poll()，离开时调 app_router_leave()。
 * - on_event 在**事件发布者线程**里执行：只能做线程安全的事（置标志位），碰 LVGL 必须
 *   用 app_disp_lock()/unlock() 包住。
 *
 * @note 页面名必须是静态生存期的字符串字面量（请求槽只存指针，不拷贝内容）。
 */

#ifndef __APP_ROUTER_H__
#define __APP_ROUTER_H__

#include "tuya_cloud_types.h"

#include "app_event.h"
#include "app_key.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 页面自己刷面板（LVGL 会被挂起）。 */
#define APP_PAGE_OWN_DISPLAY    (1u << 0)

/**
 * 这一页**不要**浮层精灵（app_sprite2d）。三类页面要声明它：
 *
 *   - **独占全屏页**（game / movie / char）：LVGL 被挂起，浮层本来就画不出来，
 *     声明它是为了让浮层**明确地停手** —— 不占 CPU，也不会在切回来的一瞬间
 *     贴一帧旧位置上去。
 *   - **横屏页**（music）：浮层用的是面板坐标，270° 旋转页对不上（见 app_sprite2d.h）。
 *
 * 路由把这一位翻成 `APP_PAGE_EVT_T.overlay` 发给订阅方（见 app_event.h），
 * 所以订阅方不需要认识任何页面名。
 */
#define APP_PAGE_NO_OVERLAY     (1u << 1)

/**
 * 这一页**需要 SD 卡**才能用（电影 / 音乐 / 游戏列表……）。
 *
 * 卡不在位时路由**不会进这一页**：它会把注册的"门页"推上去挡住，并让本次进入
 * 当成失败退回上一层。所以页面自己的 on_enter **一行检查都不用写** —— 加新页面
 * 只要声明这一位。
 *
 * "卡在不在"由 CD 引脚判定（见 app_storage.h），不是看挂载标志。
 */
#define APP_PAGE_NEEDS_SD       (1u << 2)

/** 页面栈最大深度。 */
#define APP_ROUTE_DEPTH_MAX     8

typedef struct app_page_s {
    const char *name;                    /* 唯一名字，导航用 */

    uint32_t    flags;                   /* APP_PAGE_OWN_DISPLAY */

    /**
     * @brief 首次进入：建界面 / 申请资源。可以阻塞（阻塞期间该页面自己跑主循环）。
     * @param[in] arg push 时带进来的参数（通常是列表下标），可为 NULL。
     * @return OPRT_OK 进入成功；其它值会被路由当成失败并自动退回上一层。
     */
    OPERATE_RET (*on_enter)(void *arg);

    /**
     * @brief 从上层页面返回。为 NULL 时退回调用 on_enter()。
     *        用来区分"第一次进"和"回来"（例如游戏要判断是重新加载还是恢复）。
     */
    OPERATE_RET (*on_resume)(void);

    /**
     * @brief 被新页面盖住时调用（不是离开，界面还在，只是看不见了）。
     *        用来关掉自己弹出的窗口、暂停自己的动画/定时器。
     *        注意：对 APP_PAGE_OWN_DISPLAY 页面来说，这是"让出面板"的最后时机
     *        （LVGL 会在 on_pause 之后才被恢复），所以模拟器在这里 park。
     */
    void        (*on_pause)(void);

    /** @brief 真正离开本页：释放界面/资源、停掉自己的线程。 */
    void        (*on_exit)(void);

    /** @brief 按键事件。为 NULL 时路由执行默认行为（返回键弹出本页）。 */
    void        (*on_key)(APP_KEY_E key, APP_KEY_EVT_E evt);

    /** @brief 事件总线转发（只转发给栈顶页面）。 */
    void        (*on_event)(APP_EVT_E evt, void *data);
} APP_PAGE_T;

/** @brief 注册一个页面（在 app_router_prepare() 之前调用）。 */
OPERATE_RET app_router_register(const APP_PAGE_T *page);

/**
 * @brief 进入首页。故意放在 LVGL 任务启动**之前**调用，这样首页界面在 LVGL 开始
 *        渲染时就已经存在，开机不会闪一下黑屏。
 *
 * 推荐启动顺序（见 tuya_app_main.c）：
 *   app_disp_init()  ->  app_router_prepare("home")  ->  app_disp_start()
 *   ->  app_router_run()
 */
OPERATE_RET app_router_prepare(const char *home_page);

/** @brief 启动路由线程（按键轮询 + 导航请求消费）。 */
OPERATE_RET app_router_run(uint32_t stack, uint32_t prio);

/** @brief 压入一个页面。任意线程可调用，异步生效。 */
OPERATE_RET app_router_push(const char *page, void *arg);

/** @brief 弹出 count 层（首页不会被弹出）。任意线程可调用，异步生效。 */
OPERATE_RET app_router_pop(int32_t count);

/** @brief 一路弹回首页。 */
OPERATE_RET app_router_go_home(void);

/** @brief 阻塞式页面用它请求离开（on_enter 返回后生效）。 */
void app_router_leave(void);

/** @brief 栈顶页面名（无页面返回 NULL）。 */
const char *app_router_current(void);

/** @brief 当前栈深度（最少 1）。 */
int32_t app_router_depth(void);

/** @brief 栈顶是否是指定页面。 */
BOOL_T app_router_on_top(const char *page);

/**
 * @brief 栈顶页面要不要浮层精灵（等价于 `APP_PAGE_EVT_T.overlay` 的当前值）。
 *
 * 给"开机自动开启浮层"用：那时**不会有** `APP_EVT_PAGE_CHANGED`（首页早就进去了），
 * 订阅方得主动问一次当前页，否则会先画出来再擦掉。
 */
BOOL_T app_router_overlay_ok(void);

/**
 * @brief 注册"卡不在时的门页"（通常是"请插入 SD 卡"页）。
 *
 * 注册之后路由自动做两件事：
 *   - 卡不在时进 `APP_PAGE_NEEDS_SD` 的页面 → 拦下并推这个门页
 *   - 卡插回来时如果正停在门页上 → 自动撤掉它，回到首页
 *
 * 门页要显示哪一句话由 `app_storage_fault()` 决定（卡不在 / 卡损坏），
 * 所以它同时也是"SD 故障页"（见 app_router_sd_damaged）。
 *
 * @param[in] page 页面名（静态生存期）；NULL 取消。
 */
OPERATE_RET app_router_set_sd_gate(const char *page);

/**
 * @brief 报告"SD 卡损坏"：把页面栈收到**首页**，再压上 SD 故障页。
 *
 * 由读者在"卡在位、链路重建也救不回来"（`app_storage_fault() == APP_SD_DAMAGED`）
 * 时调用。和"卡被拔掉"那条路（`APP_EVT_SD_STATUS` → prv_sd_cb 推门页）是两回事：
 * 卡还在，所以不会有人发插拔事件，必须由发现故障的人显式报一次。
 *
 * **不收着栈往回弹，而是收到首页**：栈里那些页面（电影目录、音乐列表）同样需要
 * 卡，退回去也只是对着一个读不出东西的页面发呆。故障页上的返回键也是回首页
 * （见 page_nosd.c），两边口径一致。
 *
 * 任意线程可调用（只写请求槽，异步生效）—— 阻塞式播放页要先自己跳出循环，
 * 请求才会被消费。
 */
OPERATE_RET app_router_sd_damaged(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_ROUTER_H__ */
