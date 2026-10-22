# aipet 开发手册

> 本文管**平台/环境/流程**：编译烧录监控、SDK 陷阱、常见任务的固定动作。
> 代码分层与接口细节看 [`ARCHITECTURE.md`](ARCHITECTURE.md)，
> 上手与裁剪记录看 [`../../START-HERE.md`](../../START-HERE.md)。

---

## 1. 三件套：编译 / 烧录 / 串口监控

统一前缀（本机 git bash 的 PATH 是坏的，命令前必加）：

```bash
export PATH="/c/Windows/System32:/usr/bin:/bin:/usr/local/bin:$PATH"
SDK="C:/Users/liyi/TuyaOpenIDE/TuyaOpenSDK"
PY="$SDK/.venv/Scripts/python.exe"
PROJ="C:/Users/liyi/TuyaOpenIDE/projects/aipet"
```

### 1.1 编译

```bash
cd "$PROJ/source/embedded"        # ← 必须在 source/embedded 下，否则报 Not project root
"$PY" "$SDK/tos.py" build         # 约 3.5 分钟（首次/清缓存后更久）
```

- 产物：`source/embedded/dist/aipet_1.0.0/`（`aipet_QIO_1.0.0.bin` 是整包，另有 `UA`/`UG` 和 `debug/`）
- **加源文件不用改 CMake**：`CMakeLists.txt` 递归 GLOB `src/**/*.c` + `CONFIGURE_DEPENDS`，新文件自动收进编译。
- 编译开了 `-Werror`：**未使用的 static 函数**、**注释里出现 `/*` 序列**都会直接挂。
  （删代码时这个很有用 —— 它会把浮层那种"没调用者的遗留函数"直接点出来。）
- `CMakeLists.txt` 里 `SDK_ROOT_DIR` 靠 `APP_PATH/../../../../TuyaOpenSDK`（**四级**）
  推导 —— 目录层级动了就会找不到 SDK（或加 `-DSDK_ROOT_DIR=...` 覆盖）。

### 1.2 烧录

```bash
cd "$PROJ/source/embedded"
"$PY" "$SDK/tos.py" flash -p COM19        # 端口按实际改
```

烧完**拔插一次 USB 冷启动**（见 START-HERE.md 的说明）。

### 1.3 串口监控

```bash
"$PY" "$SDK/tos.py" monitor -p COM3              # 默认 460800
"$PY" "$SDK/tos.py" monitor -p COM3 -l D:/x.log  # 落盘
```

⚠ 用 115200 读日志口会得到 **0 字节**，别据此判断"串口/驱动坏了" —— **先换波特率**。
⚠ `monitor` 是阻塞进程，后台跑完必须关掉，否则占着串口，下次烧录会失败。

**怎么判断"真的好了"**（不看工具自述，看证据）：

```bash
cat "$PROJ/.tuyaopen/status.json"      # build/flash.lastResult + 时间戳（UTC）
```

启动日志里应能看到：`mount fatfs, /sdcard 0`、`[font] … 16px SD … 21px SD …`、
`[bar] 状态栏就绪`、`[pet] 页面就绪`、`[pages] 已注册 2 个页面`、
`lv_vendor_start complete`。

### 1.4 常见故障 → 处置

| 症状 | 根因 | 处置 |
|---|---|---|
| `[ERROR]: Not project root` | 在工程根目录跑了 `tos.py` | `cd source/embedded` |
| build 报 `InvalidGitRepositoryError: …\platform\T5AI` | `platform/T5AI` 目录存在但**空**，`tos.py` 只校验 commit、永不重下 | 见 §2.1 |
| flash 报 `Cannot fetch tyutool version info` | tyutool 二进制缺失 + 代理拖死请求 | 重装 tyutool（`tos.py update -t`） |
| monitor 无输出 | 波特率不对（不是 115200） | 换 460800 |
| `cmake: uv trampoline failed to canonicalize` | `.venv/pyvenv.cfg` 的 `home` 路径失效 | 把 `home` 改回真实路径 |

---

## 2. 环境陷阱速查（按"会咬人"排序）

### 2.1 ⚠ `platform/T5AI` 是 gitlink —— 空了会让构建**死锁**

它不在 `.gitmodules` 里，是 `tos.py` 按 `platform/platform_config.yaml` 自动 clone 的
平台仓库，父仓库只记一行 `160000`。

```python
if os.path.exists(platform_root):     # ← 空目录也算"存在"
    check_platform_commit(...)        # → Repo() 抛异常，直接炸
else:
    git clone ...                     # ← 只有这里才会重下
```

**空了就永远自愈不了**，而且 `git status` 照样显示干净。搬迁/回滚大工程后必须对账：

```bash
du -sh "$SDK/platform/T5AI"                    # 应约 2.0G
git -C "$SDK/platform/T5AI" rev-parse HEAD     # 应等于 platform_config.yaml 的 commit
```

### 2.2 三个平台级硬约束

1. **`tkl_fs` 的 API 混着"收路径"和"收句柄"**：`tkl_fgetsize/faccess` 收 `const char*`，
   `fread/fseek/ftell/fclose` 收 `TUYA_FILE`（`void*`）。传错**编译期不报错**、运行时拿 0。
   取文件长度一律 `fseek(END)+ftell`。
2. **`tdl_audio` 设备是共享的**（AI 采集同句柄）。"立刻静音"用
   `tdl_audio_volume_set(hdl, 0)`，**别 `tdl_audio_close()`** —— 拆 DMA/ISR 会在真机上
   触发 `Assert at: xQueueGiveFromISR`。
3. **LVGL 跨线程刷新别用 `lv_async_call()`**：本 SDK 里它每次都 `lv_timer_create()`
   一个一次性定时器且**不加锁**，yield 线程调用会踩坏定时器链表。正确做法：事件回调
   只置 volatile 脏位，常驻 `lv_timer` 在 LVGL 线程消费。且事件回调里**别**
   `app_disp_lock()`（`lv_task_handler()` 本身持显示锁 ⇒ 自锁）。
   → `app_statusbar.c` 和 `page_pet.c` 的动作状态机都是照这个写的。

### 2.3 其它高频坑

- **中文文件名**依赖 SDK 的 `fatfs/ffconf.h` 里 `FF_CODE_PAGE == 936`。
  **重装 SDK 会丢**，丢了中文名文件全部打不开（表现为"看得见、打不开"）。
- **挂载只走 `app_storage_init()`**（幂等）。页面里别再挂 —— 二次 SDIO 会
  `CMD13 err:-3902`。别用 `app_storage_ready()` 判"卡可用"（它只是标志位，
  init 失败也置 TRUE）。
- **断言打在与业务无关的 ISR 上 = 内存被踩了**，别去 ISR 那条线找。常见踩踏源：
  任务栈不够、给三方库传错缓冲。
- **LVGL 版本按 v9 写**（`src/liblvgl/v9`）。旧笔记里 v8 的结论（`/:/sdcard/...`
  双斜杠之类）**已作废**；v9 字体走 `app_font`（`/sdcard/font/…`）。

### 2.4 本机（Windows / 沙箱）注意事项

- **git bash 的 PATH 是坏的**：每条命令前加
  `export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"`（要调 WSL 再带上 `/c/Windows/System32`）。
- **打包/删临时产物那类脚本，跑之前清一下 `PYTHONPATH`**，否则会被环境的
  safe-delete shim 拦。

---

## 3. 迭代工作流（常见任务固定动作）

### 3.1 加一个功能页面

1. 写 `src/pages/page_xxx.c`，导出 `const APP_PAGE_T app_page_xxx = {...}`；
2. `src/pages/app_pages.h` 加 `#define PAGE_XXX "xxx"` + `extern` 声明；
3. `src/pages/app_pages.c` 的 `pages[]` 加一行 `&app_page_xxx`。

按键返回、LVGL 挂起/恢复、事件转发**全部自动接上**。完整范例见 `ARCHITECTURE.md` §4。
页面名**必须是字符串字面量**（导航槽只存指针）。

### 3.2 加一个配置项

`APP_SET_ID_E` 末尾追加 + `app_settings.c` 的 `sg_desc[]` 补一行。完事。

### 3.3 加一个事件

`APP_EVT_E` 加一项 → 发布方 `app_event_publish()` → 订阅方 `app_event_subscribe()`。
`on_event` 在**发布者线程**同步执行，只做轻活（见 §2.2 第 3 条）。

### 3.4 改宠物动作集

一次要改**四处**，漏一处就静默不生效：

| 文件 | 改什么 |
|---|---|
| `third_party/sprite/fish_extract.py` | `CLIPS` 表（源素材 webm → 动作名） |
| `third_party/sprite/build_fish.sh` | `ELF_ORDER`（**顺序即组号，第 0 组必须是 idle**） |
| `src/modules/app_avatar_tools.c` | 动作白名单（给大模型看的可选值） |
| `src/pages/page_pet.c` | 情绪表（`prv_emotion_clip_name`）+ 待机小动作池（`sg_idle_acts`） |

然后 `sh third_party/sprite/build_fish.sh`，**把新的 `fish_elf.spf` 拷到 SD 卡**。
打完看脚本末尾那张表：**锚点框尺寸变了，角色在屏幕上就跟着变小**（见 START-HERE.md §4.2）。

### 3.5 生成/更新资源

| 要做的事 | 命令 |
|---|---|
| 宠物精灵 `fish_elf.spf` | `sh third_party/sprite/build_fish.sh` |
| 内置中文字体（C 源） | `python third_party/font/gen_font.py` |
| SD 六档字库 `puhui*.bin` | `python third_party/font/gen_font_sd.py` |
| 字库探针 | `python third_party/font/font_bin_probe.py` |

> 天气图标（`/sdcard/weather/*.bin`，WXI1 格式）**生成脚本不在仓库里** —— 当前这批
> 是手工一次性生成的，产物在 `third_party/weather/icons/`。要重做图标得先补一个
> 生成器（8 字节头 + RGB565 平面 + A8 平面，48x48，见 `app_weather_icon.h`）。

---

## 4. 待办与已知问题

### 4.0 ⚠️ 依赖一条 SDK 侧改动（不在本工程里）

**待机人声检测的门限改过**：`TuyaOpenSDK/src/ai_components/ai_mode/src/ai_mode_free.c`
的 `__ai_mode_enter_idle()` 从 `TKL_AUDIO_VAD_LOW`（-80dB，最敏感）改成了
`TKL_AUDIO_VAD_MID`（-60dB）。上游默认档太灵，待机时环境噪声会让它自己进 LISTEN。

- 这个文件在 SDK 仓库里**本来就有未提交的本地改动**（heikesong 时期留下的），
  别整体 checkout 掉。
- **重装 / 更新 SDK 会丢这条改动**，症状是"没叫它它自己动了"。恢复办法见
  `TuyaOpenSDK/patches/README.md` 的"外层仓库的改动"一节。
- 想再钝一档就改成 `TKL_AUDIO_VAD_HIGH`（-40dB），代价是坐远/说话轻可能叫不动。
  三档到 dB 的映射在 `platform/T5AI/.../tkl_vad.c` 的 `tkl_vad_set_threshold()`。

### 4.1 上机必看（没跑过的）

- **状态栏三个弹窗是按横屏重排过、但没上机跑过的** —— WiFi 键盘窗尤其要看一眼
  （老主页是竖屏 320x480，横屏 480x320 的版面是这次新排的）。
- **电量是假值**（`app_battery.c` 返回固定 78%），状态栏那格显示的是它。

### 4.2 代码里的已知遗留

- `app_sprite2d_render_to()` / `render()` **没有调用者**（浮层删掉后就没消费者了）。
  留着是因为背后那套"待机随机轮播"调度器以后可能有用；桌宠页现在用自己的状态机
  + `render_clip()`，没走这条路。
- `app_ai_chat_hold()` / `release()` 也没有调用者 —— 它们是**单实例 codec 的交接
  钩子**，留给以后要放音的功能（比如备忘录到点播提醒）。
- `fish_role.spf`（73MB，老角色页的大角色）不再打包也不再需要，
  可以删。
- `src/tests/` 下的 bring-up 测试（麦克风自检、SD 读压力）默认关着，排查时把
  `app_bringup.h` / `app_sd_test.h` 里的开关改回 1。

### 4.3 文档

老项目（heikesong）的 `CODE_REVIEW.md` 和 `DEV_NOTES.md` 已随裁剪删掉 / 重写 ——
它们复盘的是电影/音乐/游戏页那些**已经不存在**的代码。需要回溯去 `heikesong/` 看。

---

## 5. 提交前自检清单

- [ ] 编译过了，产物时间戳是新的（`dist/aipet_1.0.0/*.bin`）
- [ ] 烧录过了，`.tuyaopen/status.json` 的 `flash.lastResult` 是 `success`
- [ ] 串口日志里没有新增的 `E(` / `Assert` / 异常重启
- [ ] 新增的 `static` 函数都被用到了（`-Werror` 会拦）
- [ ] 改了动作集的话，四处都改了（见 §3.4）且新 `.spf` 已拷到 SD 卡
