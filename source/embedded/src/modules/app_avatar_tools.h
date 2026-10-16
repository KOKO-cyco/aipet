/**
 * @file app_avatar_tools.h
 * @brief 对话驱动动作：复刻 VirtuaMate 的 MCP 道具通道。
 *
 * VirtuaMate 让角色"根据对话做动作"靠的不是本地写死的规则，而是把动作做成
 * **MCP 工具**注册给云端大模型，大模型在生成回复时自己决定"这一轮该演哪个动作"，
 * 设备侧回调直接落到渲染层。
 *
 * 本项目复刻的就是这条通道，三个工具的落地：
 *   avatar_play_animation(name)          -> 广播 APP_EVT_PET_ACTION，由 page_pet.c 播
 *   avatar_set_emotion(emotion)          -> 广播 APP_EVT_AI_EMOTION，由 page_pet.c 的情绪表选动作
 *   avatar_composite_action(anim, emo)   -> 两者合一；anim 优先，缺省时退回情绪表
 *
 * **动作策略全留在页面里**（模块之间不互相 include，见 ARCHITECTURE.md）：本模块
 * 只负责"把大模型的意图翻译成事件"，不碰精灵引擎，也不认识动作表。
 *
 * 为什么要在开机时初始化
 * ----------------------
 * 云端会话开始时设备会把 `{"deviceMcp":{"supportCustomMCP":true}}` 塞进 session
 * attributes（见 tuya_ai_agent.c），云端据此才会把本机工具喂给大模型。所以 MCP
 * 服务器 + 工具必须在**第一个会话之前**就绪 —— 必须早于 MQTT 首次连上，否则
 * "MQTT 已连上" 这个一次性事件早就过去了，初始化回调永远不会触发。
 *
 * 与语音栈的关系：MCP 服务器是全局的（开机建一次），与语音栈（常驻，见
 * app_ai_chat.h）无关；只有**执行动作**时才检查桌宠页是不是在栈顶。
 */

#ifndef __APP_AVATAR_TOOLS_H__
#define __APP_AVATAR_TOOLS_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建 MCP 服务器并登记 avatar_* 动作工具（幂等）。
 *
 * 内部先调 ai_mcp_init()（它在 EVENT_MQTT_CONNECTED 上建服务器），再订阅同一个
 * 事件注册自有工具 —— 顺序不能反，工具必须加在服务器初始化之后。调用点要放在
 * tuya_iot_start() 之前。
 *
 * @return OPRT_OK；MCP 组件未编入时打一条 WARN 并返回 OPRT_OK（不影响启动）。
 */
OPERATE_RET app_avatar_tools_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_AVATAR_TOOLS_H__ */
