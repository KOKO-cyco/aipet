# aipet

T5AI 横屏桌宠摆件：**宠物 + 对话 + 备忘录**。

```
┌──────────────────────────────────────────────┐
│  WiFi   14:32    ☀ 26°    🔊      ▮▮▮ 78%  │  状态栏
├──────────────────────┬───────────────────────┤
│                      │                       │
│         🐟           │   备忘录 / 对话        │
│       宠物            │                       │
└──────────────────────┴───────────────────────┘
```

- **宠物** —— 左半屏，SD 卡上的帧序列精灵，有情绪、会对说话内容做动作
- **对话** —— AI 语音栈（VAD → AEC → Opus → 云端 → 播放），开机常驻，
  通过 MCP 工具驱动宠物动作
- **备忘录** —— 右半屏（**别人做**，现在是设计原型）
- **状态栏** —— WiFi 键盘 / 天气 / 时间 / AI 音量 / 电量

上手看 [`START-HERE.md`](START-HERE.md)（编译、烧录、SD 卡要放什么、裁剪记录、踩过的坑）。

## Build

```sh
cd source/embedded
tos.py build
```

纯 C 工程（老产品的 NES 模拟器和 MP3 软解已删，不再需要 C++）。

## Layout

```
aipet/
├── .tuyaopen/              # IDE + AI metadata（见 AGENTS.md 的描述符契约）
├── third_party/            # 素材 + 它们的生成工具（工具和产物放一起）
│   ├── font/               # 字库：源包 + 产物 sdcard_font/ + gen_font*.py
│   ├── sprite/             # 桌宠精灵：素材源(my_pet) + 产物 fish_elf.spf + 打包脚本
│   ├── weather/            # 天气图标：源图 + 产物 icons/*.bin
│   ├── logo/               # 开机 logo：logo.png + img_to_lvgl.py
│   ├── camera/             # 相机样张（调参用）
│   └── ai_pet_wide/        # 新 UI 的移植源（参考，不参与构建）
├── source/
│   ├── embedded/           # 固件（tos.py build 在这里跑）
│   │   └── src/
│   │       ├── core/       # 事件总线 / 配置中心 / 页面路由
│   │       ├── service/    # 显示 / 音频 / 存储 / 网络 / 时间 / 按键 / 字体 / 天气 / 电量
│   │       ├── pages/      # 页面 + 状态栏控件
│   │       ├── modules/    # 精灵引擎 / AI 语音 / 对话驱动动作 / 扬声器
│   │       └── tests/      # 宏门控的 bring-up 测试
│   └── miniapp/            # panel / mini-app 占位
├── AGENTS.md               # AI agent 的项目说明（分层约定、日志规范、硬件规则）
└── START-HERE.md
```

## 分层

`pages → service + modules → core`，**单向**。模块之间不互相 include ——
AI 要控制宠物动作时走事件总线（`APP_EVT_PET_ACTION`），动作策略留在页面里。
详见 [`source/embedded/ARCHITECTURE.md`](source/embedded/ARCHITECTURE.md)。
