/**
 * @file app_ai_chat.h
 * @brief AI 语音对话模块：**常驻**的语音栈，以及它和扬声器之间的 codec 交接。
 *
 * 常驻
 * ----
 * 语音栈（麦克风、AEC、Opus、播放器、若干线程）**开机就活着**，不再跟着页面起落：
 * 云端一连上就 `app_ai_chat_net_ready()`，之后一直听（FREE 模式 + VAD 人声触发，
 * 不需要唤醒词）。好处是"对着设备说话"不用先进任何页面 —— 以前必须进角色页。
 *
 * 谁持有 codec
 * ------------
 * 板载 codec 是**单实例**：`tdl_audio_open()` 会无条件重建整条语音管线，所以本模块
 * 和扬声器（`app_speaker`）**同一时刻只能有一个持有它**。默认给语音栈；要放音的
 * 页面进来时 `app_ai_chat_hold()` 把它要回去，退出时 `app_ai_chat_release()` 还回来。
 *
 * **当前固件里没有调用者** —— 老产品的音乐 / 电影 / 游戏三个页面是唯一的用户，
 * 都删了，所以语音栈现在是"连上云就一直听"。留着这一对是因为它是**单实例 codec
 * 的交接钩子**：以后要有别的音源（比如备忘录到点播提醒），进来 hold、出去 release
 * 就是现成的。
 *
 * 交接的**两个方向都只能整套拆/拉**：SDK 没有"只放开 codec 不拆栈"的 API
 * （`ai_audio_input_wakeup_set(false)` 只是不转发采集，codec 还占着）。所以
 * hold/release 是有代价的（建/拆 ~4 条线程 + codec 开关 + 一次 KV 写），
 * **别把它挂在会被高频调用的地方**。
 *
 * 对外只说事件
 * ------------
 *   APP_EVT_AI_STATE    data = (intptr_t)AI_MODE_STATE_*（THINK 等）
 *   APP_EVT_AI_EMOTION  data = const char *（大写情绪名，UTF-8）
 *
 * 谁来把情绪变成动作？**页面自己**（`page_pet.c` 的 `prv_emotion_clip_name()` 表）。
 * 模块之间不互相 include，动作策略改了也不用动 AI 模块。
 *
 * ⚠️ 与老产品的一处差别：路由的 `on_event` **只把事件转发给栈顶页**，而桌宠页是
 * 唯一的主界面 —— 所以它直接用 `app_event_subscribe()` 全局订阅，不走 `on_event`。
 * 另外 MCP 工具（`app_avatar_tools.c`）也发同一条情绪事件，两条路共用一张映射表。
 *
 * 音量
 * ----
 * AI 音量存在配置中心里。本模块订阅 APP_EVT_SETTING_CHANGED，看到 AI 音量变化就
 * 应用到 AI 播放器 —— 所以音频服务不需要知道 AI 模块的存在。进栈时会把配置中心的
 * AI 音量强制压回播放器（SDK 自己那份 KV 里的旧音量不可信）。
 */

#ifndef __APP_AI_CHAT_H__
#define __APP_AI_CHAT_H__

#include <stdbool.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建内部锁。**开机时调用一次**（tuya_app_main.c 里挨着 app_audio_init()）。
 *
 * 状态机有两个调用者线程：`app_ai_chat_net_ready()` 跑在一次性的 MQTT 就绪线程上，
 * `hold/release` 跑在路由线程上，两者会撞，所以必须串行化。锁在这里建，是为了
 * 让"建锁"这件事只有一个线程做得到。
 */
void app_ai_chat_prepare(void);

/**
 * @brief 云端连上后调用一次：从此语音常听（幂等）。
 *
 * **不要在 yield 线程上直接调** —— `ai_chat_init()` 要建线程、开 codec、写 KV，
 * 而那条线程栈只有 4KB（tuya_app_main.c 的 yield_cfg）。本工程的调用点另起了一条
 * 一次性线程。
 */
void app_ai_chat_net_ready(void);

/**
 * @brief 要独占 codec 的音源进来时调用：暂停常听。与 release 成对，可嵌套。
 *
 * **当前没有调用者**（见上面的"谁持有 codec"）。没网 / 云端没连上时是空操作 ——
 * 那种情况下语音栈本来就没起来，扬声器照旧持有。
 */
void app_ai_chat_hold(void);

/** @brief 音源还回 codec：恢复常听。与 hold 成对。多调一次只打一条 WARN。 */
void app_ai_chat_release(void);

/**
 * @brief 立刻掐掉 AI 正在说的那句话（手动打断，接在 P12 短按上）。
 *
 * 和"说话打断"（VAD barge-in，在 SDK 的 ai_mode_free.c 里）是两条独立的路：
 * 这条不依赖 AEC 与 VAD 阈值，所以必然生效，是打断的兜底手段。
 * 没在播报时是空操作（也不会白发一条 CHAT_BREAK 把会话打断在半路）。
 */
void app_ai_chat_interrupt(void);

/** @brief 语音栈当前是否活着（= codec 是不是在语音栈手上）。 */
bool app_ai_chat_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_AI_CHAT_H__ */
