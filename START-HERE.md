# 从这里开始 —— AI 宠物 + 备忘录

一个有记性的桌面宠物（摆件形态）。**三件事：宠物 + 对话 + 备忘录。**

```
┌──────────────────────────────────────────────┐
│  WiFi   14:32    ☀ 26°    🔊      ▮▮▮ 78%  │  ← 状态栏 480x32
├──────────────────────┬───────────────────────┤
│                      │                       │
│         🐟           │   备忘录 / 对话        │
│    宠物 240x288       │   240x288             │
│                      │                       │
└──────────────────────┴───────────────────────┘
```

- **宠物**：左半屏，SD 卡上的帧序列精灵（`fish_elf.spf`），由 `page_pet.c` 驱动
- **对话**：AI 语音栈（VAD → AEC → Opus → 云端 → 解压 → 播放），**开机常驻**，
  并且通过 MCP 工具**驱动宠物做动作**
- **备忘录**：右半屏。**别人做** —— 现在里面是设计原型（写死的假数据），
  接的人换掉数据层即可，视图和立场保留
- **状态栏**：WiFi 键盘 / 天气 / 时间 / AI 音量 / 电量（电量现在是假值，见
  `app_battery.c` 的 TODO）

---

## 1. 先跑起来

### 编译

```sh
cd source/embedded
tos.py build
```

`tos.py` 不在 PATH 里的话用全路径（IDE 自带的 venv）：

```sh
cd source/embedded
"C:/Users/liyi/TuyaOpenIDE/TuyaOpenSDK/.venv/Scripts/python.exe" \
  "C:/Users/liyi/TuyaOpenIDE/TuyaOpenSDK/tos.py" build
```

> **必须在 `source/embedded/` 下跑**。在工程根目录跑会报
> `[ERROR]: Not project root`。

### 烧录

```sh
cd source/embedded
tos.py flash -p COM19      # 端口按实际改
```

> **这块板子有个坑**：烧完之后的热复位（DTR/RTS 复位）经常卡在 bootloader
> 的 `jump to 0x2010000`。**拔插一次 USB 冷启动**就好了。别以为是固件坏了。

### SD 卡上要放什么

```
/sdcard/sprite/fish_elf.spf        精灵（待机整组常驻 PSRAM）
/sdcard/font/puhui14.bin           正文/工作字号（14px，全 GB2312）
/sdcard/font/puhui18.bin           正文（18px，全 GB2312）
/sdcard/font/puhui16.bin           卡片标题（16px，全 GB2312）
/sdcard/font/puhui20.bin           页面标题（20px，全 GB2312）
/sdcard/font/puhui24.bin           大标题（24px，全 GB2312）
/sdcard/font/puhui32.bin           卡片大数字（32px，全 GB2312）
/sdcard/weather/<code>.bin         天气图标，code = 0..38 / 99（WXI1 格式）
```

`/sdcard/font/cn16.bin`、`cn21.bin`（老的两份全字库）**开机不再加载**了 ——
全产品中文都走 puhui14/18，留着只是省得删。要它们得自己调
`app_font_load_sync()`。

**puhui 六档是新界面（`src/ui/`）的字体**，全部从 SD 加载（demo 把它们编进
固件，合计 7.5MB，放不下）。**六档都是全 GB2312**（7540 个码点：符号区 +
6763 个汉字 + ASCII）—— 界面里的字有一部分是运行时才出现的（天气描述、城市名、
AI 回复、用户写的备忘录），静态子集永远会缺字，缺字就渲染成空白。

**字形位图不占 PSRAM**：`app_font.c` 只把索引（cmap + 字形描述）读进来，位图
按需从卡上读、读过的进 24 行缓存（一份字库约 15KB）。六档全集的索引约 850KB，
比之前"14/18 全量 + 其余子集"的 1274KB 还少。

改字库：`python third_party/font/gen_font_sd.py`（要 node + `npm install lv_font_conv`），
产物在 `third_party/font/sdcard_font/`，拷到卡上同名覆盖。**换文案不需要重新
生成** —— 全集已经有了。

天气图标是 8 字节头（`WXI1` + 宽 + 高）+ RGB565 平面 + A8 平面的 48x48 数据，
见 `app_weather_icon.h`。顶栏里会把它**重着色成暗色单色**，详情弹窗里用原彩色。

字库可以不放（固件里有内置子集兜底），但内置子集**缺字就渲染空白**，
开发期建议放上。

`fish_elf.spf` 在 `third_party/sprite/my_pet/` 里（已随仓库入库，clone 即可）。**重新打包过就要重新拷到卡上** —— 固件按名字找组号，旧文件里没有
新加的动作组。

---

## 2. 开机跑的是什么

**唯一的主界面就是桌宠页**（`page_pet.c`），开机直接进。

### 交互

| 动作 | 结果 |
|---|---|
| 点宠物 | 演一个"点击回应" + 弹捕获确认卡 |
| 点右半那行淡字 | 打开记忆列表 |
| 点列表里的行 | 切换完成状态 |
| 待机 12 秒 | 自动演一次"想起"（**这是最该看的一段**） |
| 点状态栏 WiFi 图标 | 弹键盘窗，输 SSID/密码连网 |
| 点状态栏天气格 | 弹 3 日天气详情 |
| 点状态栏音量图标 | 弹 AI 音量条 |
| **P12 短按** | 打断 AI 正在说的那句话 |
| **P12 长按** | 弹设置（网络 / 音量） |

### 动作是怎么来的

宠物**不再循环轮播素材**了。现在是状态机（`page_pet.c` 的 `prv_tick_cb`）：

- **待机**：循环播 `idle` 组，每 6 秒随机插播一个小动作
- **插播**：播完这一轮就回待机

三条路都能触发插播（**回调里只写请求位**，真正的切换在渲染 tick 的帧边界上）：

1. `APP_EVT_PET_ACTION` —— 大模型点名动作（MCP 工具，`app_avatar_tools.c`）
2. `APP_EVT_AI_EMOTION` —— AI 情绪（云端 NLG 标签 / MCP 情绪工具）
3. 点宠物

### 设计立场（改界面之前先看这个）

这个界面验证的不是"好不好看"，是一条立场：

> 别人做的是"带宠物的备忘录"，这里做的是"**一个有记性的活物**" ——
> 备忘录是"你查它"，宠物是"**它记着你**"。

三条推论，每条在 `page_pet.c` 里都有对应代码：

1. **没有"页面"，只有"它拿给你看的东西"** —— 右半屏换内容，没有导航栈
2. **每条记忆都有"口气"** —— 列表第三行是"你 10月12日 跟我说的"，
   把"一条记录"读成"它记得这件事"
3. **它会"想起"，不只是"提醒"** —— 想起**带问号**，提醒是陈述句

---

## 3. 裁剪记录：哪些老产品的东西删掉了

这份工程是从 `heikesong`（掌机：NES 模拟器 + 3D 角色 + 电影/音乐播放器）复制出来的。
**老产品的页面和模块已经全部删干净**，删的是：

| 删掉的东西 | 说明 |
|---|---|
| `app_nes.cpp/.h`、`page_gamelist.c`、`page_game.c`、`page_pause.c`、`app_ui.c/.h` | NES 模拟器 + 虚拟手柄 |
| `app_movie.c/.h`、`app_movie_audio.c/.h`、`page_movie.c` | 电影 |
| `app_music.c/.h`、`page_music.c` | 音乐 |
| `app_role2d.c/.h`、`page_char.c` | 老角色页那套 2D 大角色 |
| `app_weather.c/.h` | **留着了** —— 天气挪进了状态栏 |
| `weather_icons.c/.h` | **删了** —— 40 张图标 1.1MB flash，改成 SD 卡按需加载（`app_weather_icon.c`） |
| `page_home.c` | 老主页 —— 能力（WiFi 键盘 / 天气 / 时间 / 音量）搬进了 `app_statusbar.c` |
| `app_pool.c/.h` | 全屏缓冲池（只有电影/音乐/角色用） |
| `third_party/quicknes/`、`third_party/minimp3/` | NES 核心、MP3 软解 |
| `app_sprite2d.c` 的**浮层段** | 约 1000 行（LVGL pre-flush 钩子 + 拖拽/贴边）—— 桌宠页用 `lv_canvas`，不需要它 |

**删完是纯 C 工程**（`CMakeLists.txt` 里不再 `enable_language(CXX)`）。

新增的：

| 新增 | 说明 |
|---|---|
| `app_battery.c/.h` | 电量接口（**实现是假值**，接 ADC 只改这一个文件） |
| `app_statusbar.c/.h` | 顶部状态栏 + 三个弹窗（从老主页搬来，**尺寸按横屏重排过**） |
| `APP_EVT_PET_ACTION` | 对话驱动动作的事件（模块之间不互相 include） |

`fish_role.spf`（老角色页那个 73MB 的大角色）不再打包，也不再需要 ——
（老素材目录已经清掉了，不用再管。）

---

## 4. 几个必须知道的坑

这些是踩出来的，写在这里免得再踩一遍。

### ① 面板是 320×480 竖屏，横屏靠旋转

`lv_display_set_rotation()`：**90°/270° 才换轴**（320×480 → 480×320），
**180° 不换**。桌宠页用 **90°**（摄像头朝上）。

⚠️ **旋转之后 LVGL 的逻辑坐标是 480×320**，所以 `app_theme.h` 里的
`UI_PANEL_W/H`（320/480）**不能拿来给这一页排版** —— 状态栏那三个弹窗就是
按老主页的竖屏尺寸写的，搬过来时全部重排过（见 `app_statusbar.c` 的文件头）。

### ② 锚点框 ≠ 角色本体 —— 这是"角色能画多大"的真正约束

`.spf` 里有两个尺寸：

- **角色本体**：真正有像素的那块（可见部分）
- **锚点框**：**所有动作内容框的并集**，比角色本体大得多

锚点框和打包参数 `--height` 是**线性关系**，所以它决定了"角色能画多大"。
**想放大角色，先删"撑宽"的动作**（不是"体型大"的，是**并集两边顶得最远**
的那几个），删完再提 `--height`。

当前：锚点框 218×195（最宽的是 `rnd_whale`，199 宽）。

### ③ 素材全部入库

`third_party/` 整个目录（精灵素材源 `my_pet/`、打包产物 `fish_elf.spf`、字体源包、
UI 移植源）都已进 git，**换机器 clone 仓库就有全部素材**，不用单独拷。

`third_party/sprite/` 下的中间产物（PNG 序列）由 `build_fish.sh` 自己生成，不用入库。

### ④ 中文文件名依赖 SDK 的 `FF_CODE_PAGE == 936`

`fatfs/ffconf.h` 里。重装 SDK 会丢，需要改回 936，否则中文名文件无法打开。

### ⑤ `app_disp_lock()` 不可重入

LVGL 事件回调（按钮点击）**已经在 LVGL 线程里**，别再调
`app_statusbar_open_*()` 那层加锁的公开接口 —— 直接调内部函数
（见 `app_statusbar.c` 里 `prv_wifi_icon_cb` 的注释）。

### ⑥ 状态栏的弹窗在事件回调里只能置脏位

`app_weather.h` 明确要求：刷界面必须回 LVGL 线程。事件是**在发布者线程同步
分发**的，所以事件回调只置 `volatile` 脏位，真正的刷新交给状态栏自己的
1s `lv_timer`。

---

## 5. 宠物素材管线

```sh
sh third_party/sprite/build_fish.sh        # webm -> PNG 序列 -> fish_elf.spf，一步到位
```

需要 **Python（numpy + PIL）和 ffmpeg**。本机 `py -3` 有 numpy/PIL
（项目 venv 里没有），要换就 `PY=python3 sh third_party/sprite/build_fish.sh`。

产出是 `third_party/sprite/my_pet/fish_elf.spf`（**要拷到 SD 卡**）。

**改动作集**：编 `third_party/sprite/fish_extract.py` 的 `CLIPS` 表 + `third_party/sprite/build_fish.sh`
的 `ELF_ORDER`（**顺序即组号，第 0 组必须是 idle**），**还要同步**
`app_avatar_tools.c` 的动作白名单（给大模型看的可选值）和
`page_pet.c` 的情绪表。

**关键参数**：

| 参数 | 作用 |
|---|---|
| `--height N` | 角色本体多高（**唯一控制"画多大"的旋钮**） |
| `--ref-action idle` | **千万别省** —— 省了会静默按"所有动作并集"缩放，整套缩小 1.16 倍且不报错 |
| `--shrink-default N` | 其他动作比基准小 N 像素 |
| `--idle-frames N` | 待机抽多少帧（常驻内存的那组） |
| `--max-w/--max-h` | **单帧**尺寸上限（不是锚点框，别搞混） |
| `--verify` | 解回来逐像素对拍，务必带上 |

打包完会打出一张表（每组多大、偏移多少）和**锚点框尺寸** —— 加了新动作就
盯着锚点框，它一变，角色在屏幕上就跟着变小。

---

## 6. 下一步

1. **备忘录接上** —— `page_pet.c` 的 `sg_memos[]` 是写死的三条假数据。
   建议按"**记忆**"建模而不是"备忘录"：一条记忆 = 内容 + 时间 + 来源 +
   可选提醒，备忘录只是"带提醒的那一类"（见 `page_pet.c` 的注释）
2. **提醒要能离线响** —— 现在原型的"想起"是 12 秒定时器演的。真产品里
   提醒调度不能依赖 UI 线程（那 12 秒只是给你看效果）
3. **捕获流程接 ASR** —— 现在点宠物弹的那张确认卡是模拟入口，接上之后
   换成"听到一句话 → 回显确认"
4. **电量接 ADC** —— `app_battery.c` 的 `prv_read()` 一个函数
5. **调宠物位置** —— `page_pet.c` 的 `PET_ORIGIN_DROP`（现在是 24）

## 7. 已知未验证 / 待办

- 状态栏那三个弹窗是**按横屏重排过但没上机跑过**的 —— WiFi 键盘窗尤其要看一眼
  （老主页是竖屏，横屏版面是这次新排的）
- `app_sprite2d_render_to()` / `render()` 现在**没有调用者**（浮层删掉后就没
  消费者了）。留着是因为它们背后那套"待机随机轮播"调度器以后可能有用 ——
  桌宠页现在用的是自己的状态机 + `render_clip()`，没走这条路
- 没做过长时间跑机测试（30fps 画布重绘的 CPU 占用没量过）
