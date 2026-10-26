# sdk-patches — TuyaOpenSDK 的改动存档

## 为什么有这个东西

`TuyaOpenSDK` 整个 **4.9 GB**（`platform/T5AI` 里全是预编译库和构建中间件），
不可能进 git。但这个工程跑得起来**必须带上这些改动**，所以只把**改动本身**落库。

新机器上的流程：**克隆上游 SDK → 打这里的补丁 → 克隆 aipet 仓库（third_party 素材已全量入库）**。

## 基线（打补丁前必须在这个 commit 上）

| 仓库 | 路径 | 基线 commit | 上游 remote |
|---|---|---|---|
| 外层 | `TuyaOpenSDK/` | `4e3147b`（master，2026-09-14，本机实际 checkout） | `https://gitee.com/tuya-open/TuyaOpen` |
| 嵌套 | `TuyaOpenSDK/platform/T5AI/` | `e4286054eccbfb136faa0f857861aaa3592fac0e`（= origin/master） | `https://github.com/tuya/TuyaOpen-T5AI` |

> ⚠️ 本机 SDK 比上游 master 落后 **25 个提交**（上游已到 `b1d3aec`，2026-09-30）。
> 补丁必须在 **`4e3147b`** 上打，不能直接 checkout 最新 master —— 之前那份补丁就是
> 拿 `b1d3aec` 当基线生成的，结果混进了一堆"把上游改动改回去"的反向噪声
> （例如把 `tuya_sdp.c` 的 `%64s` 安全修复改回 `%s`）。2026-10-26 已按本机真实树重新生成。

`platform/T5AI` 是个 **gitlink**（嵌套仓库，外层没有 `.gitmodules`），所以它的改动
外层提交不进去 —— 这也是为什么它单独出一份补丁。

## 内容

| 文件 | 是什么 |
|---|---|
| `tuyaopensdk.diff` | **外层** `TuyaOpenSDK/` 的改动，26 个文件（`boards/` + `src/`），`git apply -p1` 可重放 |
| `tuyaopensdk.diff.stat` | 上面那份的 `--stat`，一眼看改了哪些文件 |
| `platform-T5AI.diff` | **嵌套** `platform/T5AI/` 的改动，14 个文件，`git apply -p1` 可重放 |
| `platform-T5AI.diff.stat` | 同上 |
| `新文件/` | **新增（未跟踪）的文件**，diff 带不了，按原路径存着直接拷 |

`新文件/` 里的东西：`src/ai_components/ai_ui/src/guided_wechat/`、
4 个 `tone_*.mp3`（提示音）、`src/ai_components/assets/src/local_alert_src_tone.c`。

## 怎么重放

```bash
# 1) 外层
cd <你的 TuyaOpenIDE>/TuyaOpenSDK
git checkout 4e3147b
git apply -p1 <aipet>/sdk-patches/tuyaopensdk.diff
cp -r <aipet>/sdk-patches/新文件/* .

# 2) 嵌套
cd platform/T5AI
git checkout e4286054eccbfb136faa0f857861aaa3592fac0e
git apply -p1 <aipet>/sdk-patches/platform-T5AI.diff
```

> 打完补丁 `git status` 应该正好显示 26 + 14 个 `M`，以及 `新文件/` 里那几个 `??`。
> 对不上就是基线不对。
>
> 补丁由 `git diff HEAD` 生成，**忠实于本机工作树**：两个 `emoticons21.bdf` 只有
> 换行差异（HEAD 里是 CRLF、工作树被编辑器改成了 LF），所以补丁会整文件重写这两份，
> 内容不变、不用管。

## 改动大致是什么

**外层**（`tuyaopensdk.diff`）：

- `boards/T5AI/TUYA_T5AI_BOARD/` —— 板级配置：按钮/相机/显示/触摸的注册，
  以及一个 `AIPET_SKIP_DISPLAY` 开关（**当前 = 0**，摄像头排查时临时置 1 用来
  完全跳过 LCD 注册）。
- `src/ai_components/`（`ai_agent` / `ai_audio` / `ai_main` / `ai_mode`）——
  AI 语音栈的接线：打断、AEC 输入、播放器、`ai_mode_free` 的 VAD 门限
  （待机从 `LOW` 提到 `MID`/`HIGH`，见下）。
- `src/liblvgl/v9/` —— **RGB565 上屏前合成钩子**：
  `lv_port_disp_internal.h` 新增 `LV_PORT_PRE_FLUSH_CB` / `lv_port_set_pre_flush_cb()`，
  `lv_port_disp_full_frame.c` 在整帧 framebuffer 交给显示驱动前调用它（面板无 GRAM，
  没有硬件 overlay 层，相机预览只能在"上屏前"这一刻叠进 RGB565 framebuffer）；
  同时补了 RGB565 格式的 DMA2D 搬运分支。**aipet 的 `app_camera.c` 直接链接这个符号，
  少了它新机器会 undefined reference。** 另有 `lv_conf.h` 调整。
- `src/peripherals/` —— 音频 codec、RGB 显示（ILI9488）、GT1151 触摸的驱动微调；
  相机 GC2145 新增运行期寄存器读写接口（`tdd_camera_gc2145_reg_write/read()`，
  拍照页的白平衡/曝光调参用）和初始化序列、DVP 管线调整。
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

> ⚠️ **`TuyaOpenSDK/patches/` 那个旧目录已经过时**（只归档了 4 个文件，实际改了 14 个）。
> 以本目录为准。旧目录留着做历史参考。

## 素材不用单独拷了

`third_party/` 整个目录（精灵素材源 `my_pet/`、打包产物 `*.spf`、字体源包
`fontpkg-*/`、UI 移植源 `ai_pet_wide/`）都已进 aipet 的 git 仓库，clone 即可。
中间产物（PNG 序列）由 `sh third_party/sprite/build_fish.sh` 重新生成。
