# my_pet —— 桌宠「图生视频 → 处理 → 导出 WebM」链路文件集

> 从 `dsh-pet-indesktop` 工程中抽出的**动画素材链路**子集（2026-10-17 整理）。
> 原工程是完整桌宠应用（PySide6），本目录只保留与「AI 生成源视频 → 处理/转换 →
> 导出/校验 WebM 素材」相关的文件，按原相对路径组织，可直接对照原工程使用。

## 链路总览

```
① 图生视频（外部 AI 工具：豆包/可灵/Runway，按提示词配方生成 10s 绿幕源视频）
        │  规范见 docs/ASSET-PIPELINE.md §①；本目录 生成任务-8动作循环/ 有一套现成任务包
        ▼
② 处理（绿幕抠像 → 反解白底混合 → 归一化 → 编码）
        │  pet/frameseq_provision.py 是转换核心（webm → WebP 帧序列）
        ▼
③ 导出/校验 WebM 素材（640×360 VP9 alpha、24fps、脚底 y=330 约定）
           tools/convert_frameseq.py        构建期批量转换 CLI
           scripts/check_frameseq_bundle.py 打包前完整性硬门（只读）
           scripts/prepare_frameseq_bundle.py 三端 CI 供给入口（缺则转、转完必验）
           scripts/convert_to_gif.py        GIF 变体同步
```

## 目录结构与文件职责

```
my_pet/
├── README.md                       本文件
├── pytest.ini                      测试配置（tests/ 需要）
├── docs/                           规范与设计文档（改素材前必读）
│   ├── ASSET-PIPELINE.md                   ★主文档：素材位置约定、绿幕生成规范、
│   │                                       素材生成链四步、GIF 重生成、新增角色入库
│   ├── PR-REPORT-FRAMESEQ-V3-PIPELINE-2026-09-29.md  帧序列统一档位 v3 设计与实测
│   ├── PR-REPORT-FRAMESEQ-ALPHA-FLOOR-2026-10-16.md  alpha 底板清理（v5）报告
│   └── PR-REPORT-FRAMESEQ-NO-CONSOLE-2026-10-12.md   转换无控制台窗口报告
├── pet/                            产品代码（保持包结构，import 链完整）
│   ├── frameseq_provision.py             ★转换核心：两段式转换（ffmpeg 无损中间帧 →
│   │                                     Pillow 逐帧反解）、源身份 sha256、世代目录
│   │                                     原子发布、采纳闸门、运行时首跑自动供给
│   ├── frameseq_clip.py                  WebP 帧序列播放器（采纳后的播放路径）
│   ├── webm_clip.py                      WebM 播放器（帧序列不可用时的回退路径；
│   │                                     provision 依赖它的 session_ending）
│   ├── catalog.py                        角色/素材目录扫描、FEET_Y=330、body_box 约定
│   ├── frame_cache.py                    帧缓存（ByteBudgetLru）
│   ├── mem_debug.py / perfstats.py / win_job.py   webm_clip 的依赖（内存/性能/Windows job）
│   └── __init__.py                       包版本号
├── tools/
│   └── convert_frameseq.py          ★构建期转换 CLI：python tools/convert_frameseq.py
│                                     [--character shenshen] [--force] [--include random,events]
├── scripts/
│   ├── prepare_frameseq_bundle.py   构建/CI 供给入口：先验后转、转完必验
│   ├── check_frameseq_bundle.py     素材完整性硬门（零 Qt 零第三方、只读）
│   └── convert_to_gif.py            WebM → 透明 GIF 同步（--force --clean）
├── tests/                          帧序列链路测试（跑测试需 pytest + PySide6 + Pillow）
│   ├── conftest.py                  全局夹具（测试静音、模态弹窗 no-op）
│   ├── test_frameseq_provision.py   采纳闸门/发布回滚/供给预算（主力测试）
│   ├── test_frameseq_unblend.py     反解数学（逐像素口径）
│   ├── test_frameseq_unblend_v3.py  v3 全链：反解 → 收色 → alpha 微缩
│   ├── test_frameseq_unblend_v4.py  v4 镂空还原（默认挂起，锁直通行为）
│   ├── test_frameseq_alpha_floor.py v5 alpha 底板清理
│   ├── test_frameseq_bundle_gate.py 构建硬门与 provision 档串同源校验
│   ├── test_frameseq_no_console.py  转换不弹控制台窗口
│   ├── test_frameseq_clip.py        帧序列播放器
│   └── test_convert_frameseq.py     转换 CLI 行为
└── 生成任务-8动作循环/              ★现成任务包：8 动作 60s 闭环（豆包图生视频）
    ├── 使用说明.md                    任务包工作流与时间轴
    ├── 提示词_8个动作.md              8 条可直接粘贴的豆包提示词
    └── tools/
        ├── 首尾帧参考图_绿幕_1920x1080.png   默认交付：每个动作的首帧=尾帧
        ├── 首尾帧参考图_绿幕_1920x1080_ico源.png  备选（assets/icon.ico 兜底源，
        │                                       与视频源有色差 29.2/255，见使用说明）
        ├── 角色_透明底_640x360.png           原生分辨率抠像
        └── make_reference.py                 参考图生成脚本（--source webm|ico）
```

## 依赖

| 用途 | 依赖 |
|---|---|
| 转换核心 / 播放器 | Python 3.10+、PySide6（QtCore/QtGui）、shiboken6 |
| 逐帧反解 | Pillow ≥ 10.3（`ImageMath.lambda_eval`；10.3 无 cp313 wheel，3.13 上装 ≥10.4） |
| 视频解码/编码 | imageio-ffmpeg（自带 ffmpeg 二进制；**抽 alpha 必须 `-c:v libvpx-vp9`**，原生 VP9 解码器会静默丢 alpha 位流） |
| 跑测试 | pytest、`QT_QPA_PLATFORM=offscreen` |

## 常用命令

```bash
# 构建期：把角色全部 webm 转成帧序列（热集默认，random/events 用 --include 带入）
python tools/convert_frameseq.py --character shenshen --include random,events

# 打包前硬门（只读校验，退出码 0 = 素材完整）
python scripts/check_frameseq_bundle.py

# CI 供给（缺则转、转完必验；--force 全量重转）
python scripts/prepare_frameseq_bundle.py

# GIF 变体同步
python scripts/convert_to_gif.py --force --clean

# 帧序列链路测试
QT_QPA_PLATFORM=offscreen python -m pytest -q tests/test_frameseq_provision.py tests/test_frameseq_unblend_v3.py
```

## 关键约定（改素材前必读，详见 docs/ASSET-PIPELINE.md）

- 源视频：16:9、纯绿幕 `#00FF00`、头顶约画幅垂直 20%、脚底约 85%、左右约 25%/75%、
  禁止平移、首尾帧闭环、道具「无→有→无」
- 成品 WebM：640×360、24fps、VP9 alpha 透明、脚底对齐画布 y=330（`catalog.py` FEET_Y=330、PAD=30）
- 帧序列产物：`assets/characters/<id>/frameseq/<分类>/<动作>.g<源sha256前12位>/`，
  每目录 `meta.json` + `f_%04d.webp`；采纳五条件（目录戳/meta戳/帧数/encoder 档串/源身份）
  缺一即回退 WebM 播放——**档串是身份的一部分，改档等于让全部旧世代失效**
- 转换两段式不可合并成 ffmpeg 一趟：白底抠图素材的半透明像素存值混着白
  （`stored = 角色色×a + 255×(1-a)`），必须在有损编码前按 alpha 反解

## 未收录说明（原工程中与链路无关或不被 import 的部分）

- `pet/` 其余 200+ 文件（窗口/碰撞/聊天/设置/托盘等桌宠应用本体）——不属于素材链路
- `tests/` 中 `test_webm_*`（播放器行为）、`test_frameseq_prefetch_watchdog.py`（播放看门狗）——
  属运行时播放侧；`pet/webm_clip.py`、`pet/frameseq_clip.py` 本身已收录（import 链需要）
- 素材本体（`assets/characters/**` 约 3GB，106 段 webm + png + 三视图）不入本目录，
  按需从原工程拷贝；参考图任务包只收了生成所需的参考帧
