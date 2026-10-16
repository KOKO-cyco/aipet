# aipet 固件架构

T5AI（BK7258，480MHz Cortex-M33）**桌宠摆件**：一个有记性的 AI 宠物 + 备忘录。

> 分层骨架是从 `heikesong`（掌机）搬过来的，**core / service / pages / modules
> 的职责、路由、事件总线、显示与音频的所有权约定都没变**。
> 老产品的页面和模块（NES 模拟器、3D 角色、电影/音乐）**已经删干净**了，
> 现在只剩两个页面：桌宠页 + SD 门页。见 `../../START-HERE.md` 的裁剪记录。
本文说明代码分层、可复用接口，以及**新增一个功能页面要做什么**。

---

## 1. 分层总览

```
source/embedded/src/
├── tuya_app_main.c        入口：只做"基础设施初始化 + 按顺序启动服务 + 云端事件转服务调用"
│
├── core/                  应用框架（与硬件无关，任何功能都要用）
│   ├── app_event.*        事件总线：模块之间只通过事件解耦
│   ├── app_settings.*     配置中心：所有掉电保存的配置集中在一张描述表里
│   └── app_router.*       页面路由：页面栈 + 导航请求队列 + 系统按键轮询
│
├── service/               能力服务（每个新功能都会复用）
│   ├── app_disp.*         显示 / GUI：LVGL 生命周期、屏锁、背光
│   ├── app_audio.*        音频：AI 音量、提示音、PCM 直通
│   ├── app_speaker.*      扬声器底层（环形缓冲 + 排空任务）—— 单实例 codec 的持有者
│   ├── app_storage.*      存储：SD 挂载、目录枚举、UTF-8↔OEM 文件名互转
│   ├── app_net.*          网络：链路状态、WiFi 连接、已存凭据、云连接标志
│   ├── app_time.*         时间：本地时间、格式化、秒/分/日事件
│   ├── app_key.*          按键：消抖、短按/长按边沿事件
│   ├── app_font.*         中文字体代理（内置子集 + SD 完整字库）
│   ├── app_weather.*      天气：连上 WiFi 后拉 3 日预报，写带锁快照
│   ├── app_battery.*      电量（**当前实现是假值**，接口是真的）
│   └── app_preload.*      开机后台：把 SD 链路拉起来并放行"需要 SD 的页面"
│
├── pages/                 页面（每个界面一个文件；加页面 = 加文件 + 注册一行）
│   ├── app_theme.*        公共配色/字体/控件小工具（所有页面共用）
│   ├── app_pages.*        页面注册表（现在只有 pet + nosd）
│   ├── app_statusbar.*    顶部状态栏 + WiFi/天气/音量三个弹窗（**是控件，不是页面**）
│   ├── page_pet.c         桌宠主界面：宠物 + 备忘录（唯一的主界面）
│   └── page_nosd.c        SD 卡不在位时的门页
│
└── modules/               具体功能实现
    ├── app_sprite2d.*     精灵引擎（帧序列播放 + 待机随机调度）
    ├── app_spf6.*         .spf 容器格式解析（app_sprite2d 的底座）
    ├── app_ai_chat.*      AI 语音栈的拉起/拆掉 + 事件翻译（**开机常驻**）
    ├── app_avatar_tools.* 对话驱动动作（MCP 工具 -> APP_EVT_PET_ACTION）
    └── app_volui.*        音量控件（滑条，自订阅配置变化）
```

依赖方向是**单向**的：`pages → service + modules → core`。
`modules` 之间不互相 include（AI 要控制宠物动作时走事件总线，不直接调 app_sprite2d）。

绝迹的东西：以前 `app_menu.c` 一个文件 898 行，把状态机、LVGL 页面、WiFi 弹窗、
音量弹窗、时钟、按键消抖、ROM 列表全塞在一起。老产品的 NES 模拟器、电影/音乐
播放器、3D 角色页、全屏缓冲池，以及 `app_sprite2d` 里那个约 1000 行的 LVGL
浮层段，**都已经删掉了**。

---

## 2. 三个核心概念

### 2.1 页面（APP_PAGE_T）

一个页面就是一个结构体 + 几个回调：

| 回调 | 何时调用 | 用来做什么 |
|---|---|---|
| `on_enter(arg)` | 首次进入。**可以阻塞** | 建界面 / 申请资源 / 跑主循环 |
| `on_resume()` | 从上层页面返回（NULL 则退回 `on_enter`） | 恢复显示、解 park、补音频垫 |
| `on_pause()` | 被新页面盖住（界面还在，只是看不见） | 关弹窗、park 数据源 |
| `on_exit()` | 真正离开（弹出） | 释放界面和资源、结束会话 |
| `on_key(key,evt)` | 按键（NULL 则返回键=弹出本页） | 页面自己的按键语义 |
| `on_event(evt,data)` | 事件总线转发（只给栈顶页） | 响应时间/网络/AI 事件 |

生命周期：`on_enter → (on_pause → on_resume)* → on_exit`。

**页面栈语义**：push **不会**调用下层的 `on_exit`。所以"游戏菜单叠在游戏上"这种
关系天然成立 —— 弹回来时游戏会话（ROM、核心、内存、当前帧）原样还在。只有真正
`pop` 掉的层才会 `on_exit`。

### 2.2 显示归属

同一时刻只有一个面板所有者：

- **普通页面**：LVGL 渲染，页面在 `app_disp_lock()` 保护下建对象。
- **独占页面**（`APP_PAGE_OWN_DISPLAY`）：LVGL 任务被挂起，页面自己往 framebuffer
  写并 flush。

> **当前固件里没有独占页面** —— 老产品的模拟器和 3D 角色页是仅有的两个，都删了。
> 桌宠页是普通 LVGL 页面（宠物画在 `lv_canvas` 上，旋转和位置都由 LVGL 管）。
> 这一位留着是因为路由的那套挂起/恢复逻辑还在，加这类页面不用重写。

路由自动处理顺序，页面完全不用管 `lv_vendor_suspend/resume`：

- 进独占页：**先挂起 LVGL**，再 `on_enter`（它才去开自己的显示通道）。
- 进普通页：先在挂起状态下把界面建好，**再恢复 LVGL**（否则会闪出上一页一帧）。
- 出独占页：`on_exit` 之后才恢复 LVGL。

`lv_vendor_suspend()` 会 join 并删除 LVGL 任务，**绝不能在 LVGL 任务自己的线程里调用**
（死锁）。所以所有页面回调都在路由线程里跑。

### 2.3 线程模型

| 线程 | 职责 |
|---|---|
| `tuya_app_main` | 初始化，然后只跑 `tuya_iot_yield`（网络/云事件泵） |
| `ui_router` | 页面状态机 + 按键轮询（`app_router.c`） |
| `lvgl` | LVGL 渲染（桌宠页全程运行） |
| `preload` | 开机后台把 SD 链路拉起来并放行（`app_preload.c`） |
| `ai_boot` | 一次性：云端连上后把语音栈拉起来（`tuya_app_main.c`） |
| AI 语音栈 | SDK 自建若干条（麦克风 / AEC / Opus / 播放器） |
| `sys_timer` | `tal_sw_timer` 回调：时间滴答、网络巡检 |

约定：

- 页面回调（`on_enter/on_resume/on_pause/on_exit/on_key`）只在**路由线程**执行。
- `on_event` 在**事件发布者线程**执行 → 只能做线程安全的事（置标志位、发请求），
  碰 LVGL 必须用 `app_disp_lock()/unlock()` 包住。
- 触摸回调等其它线程要跳页面，用 `app_router_push()/pop()`（内部只写一个请求槽，
  由路由线程执行切换）。页面名必须是字符串字面量。
- **阻塞式页面**（`page_char.c` 是范例）：`on_enter` 里跑主循环时路由线程被占住，
  该页面要自己调 `app_key_poll()`，离开时调 `app_router_leave()`。

---

## 3. 服务接口速查

### 3.1 配置中心 `app_settings.h`

```c
int  v = app_settings_get_int(APP_SET_VOL_SYS);
app_settings_set_int(APP_SET_VOL_SYS, 80);    // 改内存 + 广播事件，不写 flash
app_settings_save(APP_SET_VOL_SYS);           // 需要持久化时再写 flash
app_settings_set_int_now(...);                // 一步到位（低频场景）
app_settings_get_str(APP_SET_CITY, buf, sizeof(buf));
```

现有配置项：`VOL_AI`（AI 音量，KV 键仍叫 `vol_sys`）、`BRIGHTNESS`（背光）、
`TIMEZONE`（时区小时，默认 8）、`CITY`（天气城市，留空按 IP 定位）、`WEATHER_KEY`。

加一项只要两步：`APP_SET_ID_E` 末尾追加 + `app_settings.c` 的 `sg_desc[]` 补一行。

### 3.2 事件总线 `app_event.h`

```c
app_event_subscribe(APP_EVT_TIME_MINUTE, cb, user);   // 重复订阅是幂等的
app_event_publish(APP_EVT_AI_EMOTION, (void *)"HAPPY");
```

系统事件：`TIME_SECOND / TIME_MINUTE / TIME_DATE / TIME_SYNCED / NET_STATUS /
CLOUD_STATUS / SETTING_CHANGED / VOLUME_COMMITTED / AI_STATE / AI_EMOTION /
AI_STACK_UP / PET_ACTION / WEATHER_UPDATED / PAGE_CHANGED / SD_STATUS`。

**`PET_ACTION` 是"对话驱动动作"的接口**：MCP 工具模块发它（data = 动作名），
桌宠页订阅它。模块之间不互相 include，动作策略全留在页面里。

新增事件类型 = 在 `APP_EVT_E` 加一项，发布方 publish，订阅方 subscribe。

### 3.3 显示 `app_disp.h`

```c
app_disp_lock();  ...LVGL 操作...  app_disp_unlock();
app_disp_set_brightness(60);
```

页面不需要调用 `suspend/resume` —— 那是路由按 `APP_PAGE_OWN_DISPLAY` 做的
（当前没有这类页面，见 2.2）。

⚠️ **`app_disp_lock()` 不可重入**。LVGL 事件回调（按钮点击）已经在 LVGL 线程里，
在那里再调一次会自己锁死 —— 所以 `app_statusbar.c` 里图标点击走的是内部函数
`prv_wifi_open()`，而不是加锁的公开接口 `app_statusbar_open_wifi()`。

### 3.4 音频 `app_audio.h`

```c
app_audio_set_volume(APP_AUDIO_CH_AI, 70);     // 一步到位（云端 DP 下发）
app_audio_tune_volume(APP_AUDIO_CH_AI, 50);    // 拖动中：不写 flash
app_audio_commit_volume(APP_AUDIO_CH_AI);      // 松手：落盘 + 广播上报事件
app_audio_play_alert(APP_AUDIO_ALERT_NETOK);
```

**只剩 AI 一路音量**（游戏/电影/音乐随老产品删了）。`APP_AUDIO_CH_E` 这个参数
保留着是为了不动调用点签名。

`app_audio_suspend()` / `app_audio_resume()` 是**单实例 codec 的交接**：
`app_ai_chat` 拉起语音栈前必须让扬声器松手（见 `app_ai_chat.h` 的"谁持有 codec"）。
`app_speaker.c` 不能删 —— 它就是那个 codec 的持有者。

### 3.5 存储 `app_storage.h`

```c
static BOOL_T on_file(const APP_FILE_T *f, void *user) {
    PR_NOTICE("找到 %s (%u 字节) -> %s", f->name, f->size, f->path);
    return TRUE;                    // FALSE 提前结束
}
int32_t n = app_storage_iterate("/sdcard/font", ".bin", on_file, NULL);
```

`f->name` 是 UTF-8（可直接给 LVGL 中文字体），`f->path` 是**已经验证过能 `tkl_fopen`**
的路径。中文文件名"看得见打不开"的坑（目录返 UTF-8、`f_open` 按 CP936 解释）已经在
这里一次性解决，新功能不用再抄一遍。

**SD 门控**：`app_storage_warming()` 为真时，声明了 `APP_PAGE_NEEDS_SD` 的页面
进不去（路由会推门页挡住）。解除门控的**唯一出口**是 `app_storage_warm_done()`，
由 `app_preload.c` 那条线程调用 —— 它起不来的话 `tuya_app_main.c` 有兜底直接调。

### 3.6 网络 `app_net.h`

```c
app_net_connect(ssid, pwd);              // 异步，20s 超时
APP_NET_STATUS_E st = app_net_status();  // DOWN / CONNECTING / UP
app_net_get_saved(s, sizeof(s), p, sizeof(p));   // 预填上次凭据
app_net_is_up();  app_net_cloud_up();
```

### 3.7 时间 `app_time.h`

```c
APP_DATETIME_T dt;  app_time_now(&dt);            // 本地时间，时区已算好
app_time_format(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S");
app_time_valid();                                  // 未联网时显示 "--:--"
```

自己实现格式化，不依赖 newlib 的 `strftime`：支持 `%Y %m %d %H %M %S %w %a %%`。

### 3.8 按键 `app_key.h`

```c
app_key_poll();                                  // 由 UI 线程每 30ms 调一次
APP_KEY_EVT_E e = app_key_take(APP_KEY_BACK);    // DOWN / UP / SHORT / LONG
```

一次按下产生 `DOWN → (SHORT 或 LONG) → UP`。菜单页用 `DOWN`（按下即响应），
角色动画页用 `SHORT` 切动作、`LONG` 退出。

---

## 4. 新增一个功能页面（以"设置页"为例）

> 现在的两个页面是桌宠页和 SD 门页。天气**不是**页面 —— 它在状态栏里
> （`app_statusbar.c`）。下面用一个假想的"设置页"演示加页面要做什么。

### 第 1 步：写 `pages/page_set.c`

```c
#include "app_disp.h"
#include "app_pages.h"
#include "app_router.h"
#include "app_settings.h"
#include "app_theme.h"
#include "app_time.h"

static lv_obj_t *sg_scr = NULL;

/* 事件只转发给栈顶页（看不见时不浪费 CPU）。**在发布者线程跑**，
 * 碰 LVGL 必须加锁 —— 或者更稳：只置脏位，让页面自己的 lv_timer 刷。 */
static void prv_set_event(APP_EVT_E evt, void *data)
{
    (void)evt;
    (void)data;

    app_disp_lock();
    /* ... */
    app_disp_unlock();
}

static OPERATE_RET prv_set_enter(void *arg)
{
    (void)arg;
    app_disp_lock();
    if (NULL != sg_scr) { lv_obj_del(sg_scr); sg_scr = NULL; }
    sg_scr = lv_obj_create(NULL);
    app_ui_screen_bg(sg_scr);
    app_ui_title(sg_scr, "设置");

    app_ui_button(sg_scr, 200, 56, UI_COL_BTN, "返回", UI_FONT_CN, UI_COL_TEXT,
                  NULL, NULL);   /* 返回键走路由默认行为 */
    lv_screen_load(sg_scr);
    app_disp_unlock();
    return OPRT_OK;
}

const APP_PAGE_T app_page_set = {
    .name      = "set",
    .flags     = 0,                  /* 用 LVGL；自己刷面板才写 APP_PAGE_OWN_DISPLAY */
    .on_enter  = prv_set_enter,
    .on_event  = prv_set_event,
};
```

### 第 2 步：注册（两个文件各一行）

`pages/app_pages.h`：

```c
#define PAGE_SET        "set"
extern const APP_PAGE_T app_page_set;
```

`pages/app_pages.c` 的 `pages[]` 数组里加 `&app_page_set`。

### 第 3 步：从桌宠页进去

桌宠页加一个按钮，或者接在状态栏的设置面板上（`app_statusbar.c` 的
`prv_set_wifi_cb` 就是范例）：

```c
/* prv_xxx_click 里： */
app_router_push(PAGE_SET, NULL);
```

**完事**。按键返回、LVGL 挂起/恢复、事件订阅转发，路由都已经接好了。

**注意页面名必须是字符串字面量**（见第 5 节第 1 条）。

---

## 5. 注意事项与已知约束

1. **页面名必须是字符串字面量**：导航请求槽只存指针（跨线程免锁），不拷贝内容。
2. **导航请求是单槽位**：连续快速点两个按钮时，第二个会被丢弃（人手速度足够）。
3. **`app_disp_lock()` 不可重入**：不要嵌套调用。**LVGL 事件回调已经在 LVGL 线程
   里**，那里不能再加锁 —— 状态栏的图标点击就是靠内部函数绕开的（见 3.3）。
4. **`on_event` 不要做重活**：它在发布者线程同步执行，阻塞会拖慢发布方（比如
   定时器线程）。刷界面一律"置脏位 + 自己的 lv_timer 消费"。
5. **LVGL 页面退出时不删自己的屏幕**，下次 `on_enter` 时删旧的重建，`on_resume`
   只做 `lv_screen_load()`。返回是瞬时的。
6. **`app_storage_iterate()` 的回调参数是栈上缓冲**，下一次回调会覆盖。要留存请
   自行拷贝。
7. **中文文件名依赖 SDK 的 `FF_CODE_PAGE == 936`**（`fatfs/ffconf.h`）。
   重装 SDK 会丢，需要改回 936，否则中文名文件无法打开。
8. **没有独立的触摸服务**：状态栏弹窗、宠物点击、备忘录列表都是 LVGL 的单点
   indev。老产品那个多点虚拟手柄（`app_ui.c`）删了。将来要做"非 LVGL 全屏页面 +
   多点触摸"，在 `service/` 下加一个 `app_touch.c` 即可。
9. **背光已有配置项和接口**（`APP_SET_BRIGHTNESS` + `app_disp_set_brightness()`），
   启动时会应用 —— 想给用户调，在设置面板里加一根 `app_volui_bar()` 就行。
10. 构建：`cd source/embedded && tos.py build`。**必须在 `source/embedded/` 下跑**，
    在工程根目录跑会报 `[ERROR]: Not project root`。
