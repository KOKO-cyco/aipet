# AI 语音整合路线图

> ⚠️ **本文已实施完毕，不再是待办。** 2026-09-21 规划，随后按 §六 的五步做完：
> AI 语音栈现在**开机常驻**（`app_ai_chat.c`，FREE 模式 + VAD 人声触发），
> 事件→动作映射走 MCP 工具（`app_avatar_tools.c` -> `APP_EVT_PET_ACTION` ->
> `page_pet.c`）。
>
> 保留本文是因为 **§一"关键事实"里的平台参数**（PSRAM 分区、线程约束、事件载荷
> 格式、P12 只能有一个主人）现在依然准确，排查时对得上。
> **§二 文件清单 / §六 实施顺序已经过时** —— 当时要移植的 `app_chat_bot.c`、
> `app_anim_player.c` 那些最终没有原样搬，现在的形态看
> `source/embedded/src/modules/app_ai_chat.c` 和 `app_avatar_tools.c`。

---

> 2026-09-21 由规划代理产出。当前固件为"掌机 + 3D 角色播放器"(无 AI)。
> 未来要加 AI 语音对话(豆包式:人声检测→AEC→Opus→云端→Opus→播放)时,
> 按本文档实施。基座 = 本工程(heikesong,TUYA_T5AI_BOARD + 3.5寸 ILI9488)。

## 一、关键事实

- 平台层与 chat-bot 工程(`projects/tuya-ai-your-chat-bot-copy`)逐字节一致,音频栈已就绪,**无需动 platform**
- SDK `src/ai_components/` 由 `CONFIG_ENABLE_AI_COMPONENTS=y` 一个开关整体引入;`ai_ui`(LVGL wechat UI)由 `CONFIG_ENABLE_COMP_AI_DISPLAY` 单独门控,关掉即零 LVGL
- 按键:SDK `ai_chat_main.c` 硬编码按钮名 `"ai_chat_button"`;板级用 `BUTTON_NAME` 宏(CONFIG_BUTTON_NAME)注册 **GPIO12** 按键
- 线程:`tuya_iot_start()` 非阻塞;`tuya_iot_yield()` 必须独占一条线程死循环;AI 状态机由 SDK 自建 `ai_chat_mode` 线程(优先级5、PSRAM栈)驱动
- PSRAM:HEAP 8.625MB(动画 3×fb+YUYV ≈1.65MB 在此)+ STACK_HEAP 1MB;AP 内部 RAM ≈332KB(SRAM 纪律:留给音频 AEC/Opus)
- 事件载荷:`AI_USER_EVT_MODE_STATE_UPDATE` data=`AI_MODE_STATE_E*`;`EMOTION` data=`AI_NOTIFY_EMO_T*{emoji,name}`

## 二、文件清单

**从 chat-bot 移植**:
- `tuya_main.c` → 重写本工程 `tuya_app_main.c`:cJSON PSRAM hooks、tal_kv/time/cli/authorize_init、`tuya_iot_init`(productkey=TUYA_PRODUCT_ID)、`netmgr_init(NETCONN_WIFI)`+BLE/AP 配网、`board_register_hardware()`、`app_anim_player_start()`(非阻塞化)、`app_chat_bot_init()`、`tuya_iot_start`、`tkl_wifi_set_lp_mode(0,0)`、`reset_netconfig_check()`、`for(;;) tuya_iot_yield()`
- `app_chat_bot.c` 裁剪版:仅 `ai_chat_init{AI_CHAT_MODE_HOLD, vol 70, evt_cb=桥接}` + 10 秒空闲堆打印观察点;**删** display/lowpower/printer/mcp 分支
- `reset_netcfg.c/.h` 原样(5 秒内 3 次重启清配网)
- 新建 `include/tuya_config.h`(TUYA_PRODUCT_ID 兜底 + UUID/AUTHKEY 占位,真正授权走 flash `tuya_authorize_read`)

**新建**:`Kconfig`(工程根):`config APP_CONFIG bool default y select ENABLE_AI_COMPONENTS` + `config TUYA_PRODUCT_ID string default "ha89ld9djelrng6x"`(带 AI 能力的演示 PID)
**新建**:`src/app_ai_anim_bridge.c` — AI 事件→动作映射核心(见 §四)

**app_anim_player.c 改造**:start 非阻塞化(内部建 "anim_player" 线程 4KB/PRIO_2);新增请求式 API;待机改 home 组(idle*)内轮换;一次性动作只由事件触发

## 三、Kconfig / app_default.config

```
CONFIG_TUYA_PRODUCT_ID="ha89ld9djelrng6x"
CONFIG_BUTTON_NAME="ai_chat_button"            # SDK 硬编码匹配,必须
CONFIG_ENABLE_MBEDTLS_SSL_MAX_CONTENT_LEN=4096 # AI 协议需要
CONFIG_ENABLE_COMP_AI_DISPLAY=n                # 关 wechat UI
CONFIG_ENABLE_COMP_AI_MODE_ONESHOT=n           # 只留 HOLD
CONFIG_ENABLE_COMP_AI_MODE_WAKEUP=n
CONFIG_ENABLE_COMP_AI_MODE_FREE=n
CONFIG_ENABLE_COMP_AI_MCP=n
```
改后必须删 `.build/cache/using.*` + `.build/include/tuya_kconfig.h` 重建缓存;验证 tuya_kconfig.h 出现 `TUYA_PRODUCT_ID`/`BUTTON_NAME`/`ENABLE_AI_COMPONENTS`。

## 四、线程架构与事件→动作映射

| 线程 | 来源 | 优先级/栈 | 职责 |
|---|---|---|---|
| tuya_app_main | 本工程 | 4 / 4-6KB | 初始化后专职 `tuya_iot_yield` |
| anim_player | 播放器自建 | 2 / 4KB | SD→硬解→转换→三缓冲 flush;消费动作请求 |
| ai_chat_mode | SDK | 5 / 3KB PSRAM | HOLD 状态机 IDLE/LISTEN/UPLOAD/THINK/SPEAK |

约束:bridge evt_cb 运行在 SDK 线程,只写请求+日志,绝不阻塞;动作请求 API 非阻塞。

**状态映射**:IDLE→home;LISTEN→say_hello(PLAY);UPLOAD→look_around(PLAY);THINK→thinking(HOLD);SPEAK→excited(HOLD)

**情绪映射**(PLAY 一轮):HAPPY/LAUGHING/...→joy;SAD/DISAPPOINTED/...→crying;SURPRISE/SHOCKED→excited;ANGRY→shoot;THINKING/CONFUSED→thinking;NEUTRAL→happy_idle;WAKEUP→say_hello;SILLY/TOUCH→model_pose

**播完去向**(next_query 回调):SPEAK 中→excited、THINK 中→thinking、LISTEN→home、其余→home。MQTT 首连→play(bier) 鞠躬上线;BIND_START→play(say_hello)+配网提示音。

## 五、授权/配网前置

1. PID 必须带 AI 对话能力(ha89ld9djelrng6x = Tuya AI 玩具类演示 PID);平台核对 AI 服务开通
2. 设备授权:该板已跑通 chat-bot ⇒ 授权已在 flash,`tuya_authorize_read` 直接成功,无需重烧;串口日志确认 license 读取成功
3. 配网:NETCFG_TUYA_BLE|WIFI_AP,涂鸦 App;重配网 = 5 秒内 3 次重启

## 六、实施顺序(5 步)

1. 配置 + 动画线程化(不碰 AI)——验证动画照常、tuya_kconfig.h 正确
2. 网络骨架(移植 tuya_main,无 AI)——验证 App 配网、MQTT_CONNECTED
3. AI 音频链(裁剪版 app_chat_bot,evt_cb 只打印)——验证按住说话 LISTEN→UPLOAD→THINK→SPEAK、TTS 出声、双击打断。**此步 AI 对话即完整可用**
4. 事件→动作映射(bridge)——验证全链路角色反应
5. 打磨:音量 DP、断网重连、触摸策略、2 小时 soak、flash 余量核对
