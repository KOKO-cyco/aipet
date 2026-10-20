# AGENTS.md — aipet

AI agent instructions for the `aipet` TuyaOpen project.

## Project snapshot

- **Name**: `aipet`
- **Platform**: `t5ai`（T5AI / BK7258，480MHz Cortex-M33）
- **Framework**: `base`
- **What it is**: 横屏桌宠摆件 —— **宠物 + 对话 + 备忘录**三件事。
  界面是 `third_party/ai_pet_wide`（宽屏 demo）移植过来的米白卡片风：
  顶栏（待办 · 天气 · 时间 · WiFi · 音量 · 电量）+ 左宠物卡 + 右功能面板。
  宠物是**实时精灵**（app_sprite2d，SD 卡），不是静态图。
- **Source**: `source/embedded/`，`cd source/embedded && tos.py build`。
  纯 C 工程（老产品的 NES 模拟器和 MP3 软解已删，不再需要 C++）。
- **Pages**: 路由层两页 —— `page_pet.c`（唯一主界面，宿主页）和 `page_nosd.c`（SD 门页）。
  **主界面内部**是一套七页的面板栈，源码在 `src/ui/`（移植自 ai_pet_wide）：
  主页 / 拍照 / 相册 / 备忘录 / 全部笔记 / 设置，以及左侧的宠物卡。
  老产品的首页/游戏/电影/音乐/角色页全删了，见 `START-HERE.md`。

## Project layout

```
aipet/
├── .tuyaopen/
│   ├── project.json        # name, version, type, framework; ai.intent only when AI-created
│   ├── status.json         # lifecycle: scaffolded → configured → built → flashed
│   ├── architecture.json   # surfaces, modules, components, dependencies
│   ├── dependencies.lock.json   # ecosystem libs (when installed via IDE)
│   ├── ide/                # IDE-managed runtime files (do not edit)
│   │   ├── bin/tuya-devplat-cli   # wrapper script — use full path outside VSCode
│   │   ├── platform.json          # hardware capability spec for the active platform (chip-level)
│   │   └── board.json             # board-level hardware spec (onboard components, pre-allocated pins)
│   └── platform/           # Tuya Platform product snapshots (IDE)
│       └── product-<pid>.json
├── tuyaopen.project.ini    # human-readable project config (IDE-maintained)
├── tools/                  # 素材管线（webm -> PNG -> fish_elf.spf，见 build_fish.sh）
├── third_party/
│   ├── fish/               # 打包产物 fish_elf.spf + 背景（gitignore，换机器要单独拷）
│   └── blue_big_fat_fish/  # 动作素材源（gitignore）
├── source/
│   ├── embedded/           # firmware — build with tos.py
│   │   ├── include/        # project header files
│   │   └── src/
│   │       ├── core/       # 事件总线 / 配置中心 / 页面路由
│   │       ├── service/    # 显示 / 音频 / 存储 / 网络 / 时间 / 按键 / 字体 / 天气 / 电量
│   │       ├── pages/      # 路由页（page_pet / page_nosd）+ 状态栏控件
│   │       ├── ui/         # 主界面内部：pui 组件库 + 宠物卡 + 七页面板栈
│   │       │   ├── pui/    # 组件库（卡片 / 瓷贴 / 行 / 按钮 / 页面骨架）
│   │       │   ├── fonts/  # FontAwesome 图标字体（中文全部走 SD）
│   │       │   └── pet_page_*.c  # 主页 / 拍照 / 相册 / 备忘录 / 笔记 / 设置
│   │       ├── modules/    # 精灵引擎 / AI 语音 / 对话驱动动作 / 扬声器
│   │       └── tests/      # 宏门控的 bring-up 测试
│   └── miniapp/            # panel/mini-app placeholder
├── .vscode/
│   └── settings.json       # CMake source + build directory
├── .agents/skills/         # AI agent skill catalogue (Agent Skills standard)
├── .gitignore
├── AGENTS.md
├── CLAUDE.md
├── START-HERE.md           # 上手：怎么编译、烧录、SD 卡放什么、裁剪记录
└── README.md
```

## `.tuyaopen/` descriptor contract

**`project.json`**
- `name`, `version`, `type`, `framework` — project identity
- `ai.intent` — user's original intent _(only present for AI-assisted scaffold — absent in normal projects)_
- `ai.assistedScaffold` — true when created via "Create with AI" _(optional — absent in normal projects)_
- `ai.skills` — reserved for future use; **do not rely on it to discover installed skills**. Scan `.agents/skills/` directly — each subdirectory (flat or one level deep) whose `SKILL.md` exists is an installed skill.

**`status.json`** — advance `lifecycle` as the project progresses:
`"scaffolded"` → `"configured"` → `"built"` → `"flashed"`

**`architecture.json`** — update when structure changes:
- `surfaces.embedded` — platform, chip, board, entrypoint, RTOS config, hardware peripherals
- `surfaces.miniapp` — panel kind and entrypoint (null until Panel SDK runs here)
- `modules` — logical modules with source paths and surface
- `dependencies` — external libraries (TuyaOpen core, etc.)

**`dependencies.lock.json`** — created when the IDE installs a library from Library → Ecosystem. Canonical pinned versions; commit to git. The `[ecosystem]` section of `tuyaopen.project.ini` mirrors this file.

**`.tuyaopen/platform/`** — Tuya Platform product snapshots (IDE-owned, read-only). One file per active PID.

- **Binding** — active PID is `tuyaopen.project.ini` → `[product] pid`; the filename is informational.
- **Envelope** — `schemaVersion` (1 or 2), `pid`, `fetchedAt`, `source`, `detail`, `dpSchema`, `fetchError`.
- **Unwrap** — `detail` and `dpSchema` may be wrapped `{ ok, data }`; always read as `field.data ?? field`.
- **Active DPs** — read `dpSchema.dps[]` where `selected === true`; skip all others.
- **Cloud functions** — `dpSchema.uiConfig.bic[]` lists BIC cloud functions enabled for this product.
- **Never invent** — if missing or `fetchError` is set, ask the developer to sync in the IDE.
- **Mutations** — use `tuya-devplat-cli` commands or the **Tuya IoT Platform** skill (`tuya-iot-platform`); never edit the JSON directly.

Deep reference: `.agents/skills/tuya-iot-platform/ops/product.md` (products) and
`.agents/skills/tuya-iot-platform/ops/manage-dp.md` (DPs).
_(Install skill **Tuya IoT Platform** from the IDE Skills panel if the path does not exist.)_

## `tuyaopen.project.ini`

Human-readable project descriptor at the project root. Maintained by the TuyaOpen IDE; hand-editing is allowed and unknown keys are preserved on round-trips.

| Section | Purpose |
|---|---|
| `[project]` | `name`, `version`, `type`, `schema` — project identity |
| `[platform]` | `target` (`t5` \| `t5ai` \| `esp32` \| `linux` \| `raspberrypi`), `sdk`, `sdk_version` |
| `[build]` | `output`, `toolchain` |
| `[board]` | Present when a board was selected at creation: `id`, `kconfig_id`, `config_file` (optional fields) |
| `[features]` | Optional feature toggles (`key = value`) |
| `[product]` | Present when bound to a Tuya Platform product: `pid = …` |
| `[ecosystem]` | Present when ecosystem libraries are installed: `owner/name = ^x.y.z` (mirrors `dependencies.lock.json`) |

Prefer updating structured JSON under `.tuyaopen/` for AI-facing contract fields (`project.json`, `architecture.json`). Use the INI when a skill or workflow explicitly targets project config, or when mirroring values the IDE already wrote (product PID, ecosystem constraints).

## Hardware / peripheral code (vibe coding)

For ANY request that touches hardware — a peripheral, pin, or bus (LED, display,
camera, button, audio, touch, printer, IR, joystick, LED strip, **serial / UART**,
GPIO, PWM, I2C, SPI, ADC, sensor, "make the hardware do X") — **first load and
follow `.agents/skills/hardware-vibe-coding/SKILL.md`**. It is the authority; do
not improvise hardware code without it.

Non-negotiable rules from that skill:

- **Read the project hardware first**: `.tuyaopen/used-peripherals.json` (the
  already-confirmed set — your INPUT), `.tuyaopen/board-context.md` (device
  **index**: IDs + grouping + registration semantics), `.tuyaopen/ide/board.json`
  (raw per-device pins / driver / Kconfig, looked up by `ID:`),
  `.tuyaopen/ide/platform.json` → `peripherals` (on-chip availability).
- **Confirm before code (hard gate).** Generate only for confirmed hardware. If
  something is not yet confirmed, or there are multiple matching options / a
  `## group —`, ASK the user which one and wait — never auto-pick.
- **Record before code.** Write the confirmed set to `used-peripherals.json`
  BEFORE writing any code (device `ID:`, or `onchip:<type><n>` for on-chip).
- **"串口 / serial / UART" is ambiguous → ASK first**: the debug/log console
  (`PR_*` — often a USB-serial the PC already sees; not a peripheral, nothing
  recorded) vs a dedicated user UART (`tal_uart` on its own instance + pins).
  Do not assume `PR_*` is what the user wants.

## Build & flash (embedded)

```bash
cd source/embedded
tos.py check     # verify SDK environment
tos.py build     # compile firmware
tos.py flash     # flash to device
```

> **Skill-script path note**: Skill scripts (e.g. `tuyaopen-dev-loop` `build_run.py`,
> `tuyaopen-debug-helper` `monitor_helper.py`) look for `app_default.config` in CWD
> to locate the project root. Since `app_default.config` lives in `source/embedded/`,
> call these scripts from there and adjust the path accordingly:
>
> ```bash
> cd source/embedded
> $OPEN_SDK_PYTHON ../../.agents/skills/tuyaopen-dev-loop/scripts/build_run.py
> $OPEN_SDK_PYTHON ../../.agents/skills/tuyaopen-debug-helper/scripts/monitor_helper.py start -p /dev/ttyACM1
> ```
>
> Skills install one flat directory per id (`.agents/skills/<id>/`). The nested
> `tuyaopen/<name>/` layout these commands used to show is the pre-migration one
> — see `skillsLegacyMigration` in the IDE.


## Developer Platform CLI

The TuyaOpen IDE writes a wrapper script at `.tuyaopen/ide/bin/tuya-devplat-cli`.
The IDE injects this directory into the VSCode integrated terminal PATH, but agent
tools running outside VSCode terminals (Claude Code CLI, shell scripts, etc.) must
use the full path explicitly:

```bash
# Prefer the full path so it works in all execution contexts:
.tuyaopen/ide/bin/tuya-devplat-cli auth status --format json
```

The wrapper sets `TUYA_DEVPLAT_CONFIG_PATH` internally, so credentials resolve
correctly regardless of the caller environment.

### Calling the CLI

```bash
.tuyaopen/ide/bin/tuya-devplat-cli auth status --format json          # check login state
.tuyaopen/ide/bin/tuya-devplat-cli product list --format json          # list products
.tuyaopen/ide/bin/tuya-devplat-cli product detail --pid <PID> --format json
.tuyaopen/ide/bin/tuya-devplat-cli product dp-schema --pid <PID> --format json
```

### Write operations — mandatory two-step flow

**Never call `--confirm` without first running `--dry-run`.**

```bash
# Step 1: dry-run (safe, no side effects)
.tuyaopen/ide/bin/tuya-devplat-cli product update --pid <PID> --name "New Name" --dry-run --format json
# → returns { confirmToken, preview, riskLevel }

# Step 2: confirm with the exact same parameters (token is a hash of args)
.tuyaopen/ide/bin/tuya-devplat-cli product update --pid <PID> --name "New Name" --confirm <confirmToken> --format json
```

The CLI validates the confirm token against the command arguments — any modification
of flags between dry-run and confirm will be rejected.

### Credentials

Credentials are stored by the IDE in the directory pointed to by `TUYA_DEVPLAT_CONFIG_PATH`
(set automatically by the wrapper). If the CLI returns an auth error, ask the developer
to sign in via **TuyaOpen IDE → Developer Platform** sidebar.

**Never attempt `.tuyaopen/ide/bin/tuya-devplat-cli auth login` — it requires interactive browser input.**

### Source of truth for product data

Read `.tuyaopen/platform/product-<pid>.json` for cached product/DP data.
Ask the developer to press **Sync** in the IDE if the snapshot is missing or stale.
Do not invent product IDs or DP codes.

## `tuyaopen` project CLI (local metadata + code generation)

The IDE writes a wrapper at `.tuyaopen/ide/bin/tuyaopen` (same dir as the
devplat CLI wrapper, already on the PATH in VS Code terminals). Outside VS Code
terminals, use the full path. It is a zero-dependency, AI-first CLI for project
metadata and code generation — **prefer it over hand-editing `.tuyaopen/` files.**

```bash
.tuyaopen/ide/bin/tuyaopen dp list --format json                       # list selected DPs
.tuyaopen/ide/bin/tuyaopen dp generate --target embedded,miniapp       # regenerate both sides
.tuyaopen/ide/bin/tuyaopen hardware confirmed-context --format json     # the vibe-prompt context block
.tuyaopen/ide/bin/tuyaopen hardware set-used --ids "LED_1,BTN_1"        # record confirmed peripherals
.tuyaopen/ide/bin/tuyaopen project info --format json                   # project + product + lifecycle
.tuyaopen/ide/bin/tuyaopen dependency list --format json                # ecosystem lockfile
.tuyaopen/ide/bin/tuyaopen product info --format json                   # bound product snapshot + DP count
```

- `--pid` defaults to `tuyaopen.project.ini` → `[product] pid`; `--project-root` defaults to CWD.
- Piped/non-TTY output is JSON (a uniform `{ok, data, error, code, suggestion}` envelope); exit 0 on success, 1 on any failure. Run `<group> --help` or `<group> <command> --help` to see flags.
- `dp generate --target miniapp` writes `source/miniapp/src/devices/schema.ts`; `--target embedded` writes `source/embedded/include/tuya_dp_profile.h` (`#define DPID_<CODE> <id>` + value bounds). Both stamp a `// DP-HASH:` first line so you can detect drift.
- `hardware confirmed-context` emits exactly the block the IDE inlines into vibe prompts — fetch it on demand instead of reading `board.json`/`platform.json`/`used-peripherals.json` separately. `hardware set-used` replaces hand-editing `used-peripherals.json`.
- After changing DPs (in the IDE or via the devplat CLI), re-run `dp generate` instead of editing those generated files by hand.

Wired today: `dp`, `hardware`, `project info`, `dependency list`, `product info`. Cloud-side operations (DP add/delete, product/panel sync) still go via `tuya-devplat-cli` below; credentials/login stay in the IDE.

## Platform hardware spec (`.tuyaopen/ide/platform.json`)

**Read before writing any hardware-interfacing code.** Authoritative chip-level capability map.

| Field path | What it tells you |
|---|---|
| `peripherals.<name>.tklHeader` | `#include` this header to use the peripheral |
| `peripherals.<name>.idPrefix` | C enum prefix for port/pin IDs (e.g. `TUYA_GPIO_NUM_`, `TUYA_UART_NUM_`) |
| `peripherals.<name>.enableMacro` | Kconfig symbol that gates this peripheral; null = always enabled |
| `peripherals.<name>.count` | Number of available ports / channels |
| `peripherals.<name>.spec` | Port IDs, valid pin groups, parameter ranges (baud, speed, mode, freq…) |
| `peripherals.uart[].ports[*].logPort` | `true` = SDK debug-log port — do **not** reassign |
| `connectivity.<radio>.enabled` | Whether the radio (wifi/ble/ethernet/cellular) exists on this chip |
| `connectivity.<radio>.enableMacro` | Kconfig symbol to enable the radio stack |
| `flashAndDebug.flash.pins` | UART pins used for firmware download — do **not** reassign |
| `flashAndDebug.debug` | Log UART port and baud (read-only; already claimed by the SDK) |
| `pinout[]` | Physical pin ↔ GPIO number ↔ multiplexed functions |
| `memory` | SRAM / ROM / flash / PSRAM sizes in bytes |
| `kconfig.PLATFORM_CHOICE` | Platform Kconfig value for `app_default.config` |

**Workflow**: before using any peripheral, look up `peripherals.<name>` to get the correct header, ID prefix, port IDs, valid pin groups, and parameter constraints. Choose a pin from `spec` that is not already reserved (see `board.json` below and `flashAndDebug.flash.pins`).

## Board hardware spec (`.tuyaopen/ide/board.json`)

**Read before assigning any GPIO.** Chip-agnostic description of what is physically wired on this development board.

| Field | What it tells you |
|---|---|
| `id` / `kconfigId` | Board identifier; pass `kconfigId` to `tos.py config choice -c <kconfigId>` |
| `platformId` | Matches `platform.json.id` — links the two files |
| `peripheralPatterns.<type>[]` | Onboard components (LED, button, audio, display, …) |
| `peripheralPatterns.<type>[].pins.<if>[].gpio` | GPIO number already occupied by this component |
| `peripheralPatterns.<type>[].note` | Polarity / behaviour (active-high, active-low, AEC enabled…) |
| `peripheralPatterns.<type>[].kconfig` | Kconfig symbol to enable this component (null = always active) |
| `scaffold.baseConfig` | Kconfig lines required in `app_default.config` for this board |

**Workflow**: before wiring a new GPIO, collect all reserved numbers —
`peripheralPatterns.<type>[].pins.<if>[].gpio` across every entry — then pick a free one from `platform.json peripherals.gpio.spec.pins`. Never repurpose a reserved GPIO without developer approval; these are physical PCB traces.

## 日志（PR_* 分级规范）

日志走 SDK 的 `tal_log`（`src/tal_system/include/tal_log.h`），6 级，数值越小越严重：

| 级别 | 宏 | 语义 |
|---|---|---|
| 0 | `PR_ERR` | **功能不可用 / 数据可能损坏**。用户会看到异常，必须修 |
| 1 | `PR_WARN` | **降级但继续工作**，或"不该发生但兜住了" |
| 2 | `PR_NOTICE` | **正常生命周期里的关键节点**。日常开着的默认档 |
| 3 | `PR_INFO` | 比 NOTICE 细一档的一次性信息（配置值、能力探测结果） |
| 4 | `PR_DEBUG` | 排查时才关心的细节 |
| 5 | `PR_TRACE` | 逐调用跟踪，基本不用 |

**判据（一条就够）：这条日志出现时，用户还能正常用吗？**

- 不能 → `PR_ERR`
- 能，但功能打了折扣 → `PR_WARN`（静音播放、跳过坏段、目录不存在、触摸不可用）
- 能，一切正常 → `PR_NOTICE` 或更低

推论：**`PR_ERR` 应该罕见。** 它被稀释之后，"缓冲池预留失败"和"某个音轨没声音"看起来一样红，
扫日志时反而会漏掉前者。

**周期性统计一律 `PR_DEBUG`。** 每 2 秒刷一次的 fps / 丢帧 / 欠载 / 耗时分解是排查用的，
不是"发生了什么"。它们留在 `PR_NOTICE` 会让全局级别开关失效——调高连正常生命周期日志一起没了。

**每条日志带 `[tag]` 前缀**，一个 `.c` 文件固定一个 tag。现有：

| 层 | tag |
|---|---|
| core | `[evt]` `[set]` `[route]` `[pages]` |
| service | `[disp]` `[aud]` `[sto]` `[net]` `[time]` `[key]` `[font]` `[mem]` `[weather]` `[bar]` |
| modules | `[sp2d]` `[spf6]` `[ai]` `[avatar]` `[spk]` `[volui]` |
| pages | `[pet]` `[nosd]` `[boot]` |
| tests | `[bringup]` `[sdt]` `[camt]` |
| 入口 | `[main]` `[pre]` |

配合 SDK 自动打的 `[文件.c:行号]` 和级别配色（ERR 红 / WARN 黄 / NOTICE 蓝），一眼能认出
是哪块在说话。

> `[bar]` 是顶部状态栏（`app_statusbar.c`），`[pre]` 是 SD 就绪线程（`app_preload.c`）。
> 老产品的 `[pool]` `[movie]` `[mva]` `[anim]` `[nes]` `[game]` `[char]` `[list]` `[ui]`
> 随那些模块一起删了。

**随时开关**：全局级别用 `tal_log_set_level()`（本项目开机设的是 `TAL_LOG_LEVEL_DEBUG`，
见 `tuya_app_main.c` 的 `tal_log_init()`）。日常看 `TAL_LOG_LEVEL_NOTICE`，排查切 `DEBUG`。

> **不要试图按模块设级别。** `tal_log_add_module_level()` / `tal_log_set_module_level()` 在
> SDK 头文件里有声明，但**没有实现**（全 SDK 搜不到定义，预编译库里也没有符号），调了会链接失败。
> 想按模块过滤只能自己搭一层，成本不划算——先把分类修对，全局级别就够用了。

## Conventions

- Entry point: `tuya_app_main()` in `source/embedded/src/tuya_app_main.c`
- 日志分级见上一节（`PR_*` 规范）
- Use TAL APIs (`tal_system_sleep`, `tal_thread_create_and_start`, etc.) — do not call platform-specific functions directly
- Run `tos.py check` after environment changes before building
- Update `status.json.lifecycle` as the project progresses
- Surface open questions to the developer; do not invent requirements

## Coding Style

Follow POSIX C style — use standard lowercase keywords:

```c
static void   app_led_init(void);       // correct
static int    count = 0;                // correct
const uint8_t *buf;                     // correct

STATIC VOID   app_led_init(VOID);      // WRONG — never use uppercase keyword aliases
```

- C keywords (`static`, `void`, `const`, `int`, `return`, …) are always lowercase
- SDK typedefs that are uppercase are fine: `OPERATE_RET`, `TDL_LED_HANDLE_T`, `TUYA_GPIO_NUM_E`, etc.
- Never use `STATIC`, `VOID`, `CONST`, `INT` even if the SDK headers define them
