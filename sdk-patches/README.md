# sdk-patches — TuyaOpenSDK 的改动存档

## 为什么有这个东西

`TuyaOpenSDK` 整个 **4.9 GB**（`platform/T5AI` 里全是预编译库和构建中间件），
不可能进 git。但这个工程跑得起来**必须带上这些改动**，所以只把**改动本身**落库。

新机器上的流程：**克隆上游 SDK → 打这里的补丁 → 把巨型素材单独拷过去**。

## 基线（打补丁前必须在这个 commit 上）

| 仓库 | 路径 | 基线 commit | 上游 remote |
|---|---|---|---|
| 外层 | `TuyaOpenSDK/` | `b1d3aecc9a8399a436e123d06b081d705b109a65`（master，2026-10-19 起重生成） | `https://gitee.com/tuya-open/TuyaOpen` |
| 嵌套 | `TuyaOpenSDK/platform/T5AI/` | `e4286054eccbfb136faa0f857861aaa3592fac0e` | `https://github.com/tuya/TuyaOpen-T5AI` |

`platform/T5AI` 是个 **gitlink**（嵌套仓库，外层没有 `.gitmodules`），所以它的改动
外层提交不进去 —— 这也是为什么它单独出一份补丁。

## 内容

| 文件 | 是什么 |
|---|---|
| `tuyaopensdk.diff` | **外层** `TuyaOpenSDK/` 的改动，42 个文件（`boards/` + `src/`），`git apply -p1` 可重放 |
| `tuyaopensdk.diff.stat` | 上面那份的 `--stat`，一眼看改了哪些文件 |
| `platform-T5AI.diff` | **嵌套** `platform/T5AI/` 的改动，11 个文件，`git apply -p1` 可重放 |
| `platform-T5AI.diff.stat` | 同上 |
| `新文件/` | **新增（未跟踪）的文件**，diff 带不了，按原路径存着直接拷 |

`新文件/` 里的东西：`src/ai_components/ai_ui/src/guided_wechat/`、
4 个 `tone_*.mp3`（提示音）、`src/ai_components/assets/src/local_alert_src_tone.c`。

## 怎么重放

```bash
# 1) 外层
cd <你的 TuyaOpenIDE>/TuyaOpenSDK
git checkout b1d3aecc9a8399a436e123d06b081d705b109a65
git apply -p1 <aipet>/sdk-patches/tuyaopensdk.diff
cp -r <aipet>/sdk-patches/新文件/* .

# 2) 嵌套
cd platform/T5AI
git checkout e4286054eccbfb136faa0f857861aaa3592fac0e
git apply -p1 <aipet>/sdk-patches/platform-T5AI.diff
```

> 打完补丁 `git status` 应该正好显示 42 + 11 个 `M`，以及 `新文件/` 里那几个 `??`。
> 对不上就是基线不对。

## 改动大致是什么

**外层**（`tuyaopensdk.diff`）：

- `boards/T5AI/TUYA_T5AI_BOARD/` —— 板级配置：按钮/相机/显示/触摸的注册，
  以及一个 `AIPET_SKIP_DISPLAY` 开关（**当前 = 0**，摄像头排查时临时置 1 用来
  完全跳过 LCD 注册）。
- `src/ai_components/`（`ai_agent` / `ai_audio` / `ai_main` / `ai_mode`）——
  AI 语音栈的接线：打断、AEC 输入、播放器、`ai_mode_free` 的 VAD 门限
  （待机从 `LOW` 提到 `MID`/`HIGH`，见下）。
- `src/liblvgl/v9/` —— LVGL 上屏前的合成钩子（`lv_port_disp_full_frame.c`）和 `lv_conf.h`。
- `src/peripherals/` —— 音频 codec、RGB 显示（ILI9488）、GT1151 触摸的驱动微调。
- `src/tuya_ai_service/` / `src/tuya_cloud_service/` —— AI agent 与云端对接。

**嵌套 `platform/T5AI`**（`platform-T5AI.diff`）：

- `fatfs/ffconf.h` —— `FF_CODE_PAGE` 437 → **936 (GBK)**。不改的话中文文件名
  `f_open()` 永远打不开（目录里看得见、就是打不开）。
- `tuyaos_adapter/src/driver/tkl_vad.c` —— 跨线程指令槽（`rnn_vad_start/stop`
  只在 AEC 管线线程的帧边界执行），修"聊着聊着停一会儿再说话就没反应"。
- `tkl_audio.c` / `tkl_kws.c` —— 探针打印用 `AI_VOICE_TRACE` 包住，**默认关**。
- `sd_card_driver.c` / `bk_voice_service.c` / `onboard_mic_stream.c` ——
  SD 卡与语音服务的板级调参。
- 两个 `*.defconfig` 和 `bk7258_ap/config` —— T5AI 平台的 Kconfig 默认值。

> ⚠️ **`TuyaOpenSDK/patches/` 那个旧目录已经过时**（只归档了 4 个文件，实际改了 11 个）。
> 以本目录为准。旧目录留着做历史参考。

## 别忘了单独拷的东西（都在 aipet 的 `.gitignore` 里）

`third_party/blue_big_fat_fish/`（素材源 webm，72M）和 `third_party/fish/`
（打包产物 `*.spf`，44M）**故意不进 git** —— 都能从源视频用
`sh tools/build_fish.sh` 重跑出来。换机器时单独拷这两个目录，否则桌宠页没素材。

另外 `third_party/resources/`（`fish_extract.py` 解出来的 PNG 序列，几百 M）
也是 gitignore 的，跑一次 `sh tools/build_fish.sh` 就会重新生成。
