/**
 * @file app_memo_tools.h
 * @brief 备忘录/笔记的 MCP 工具（memo_ops / note_ops）与状态快照推送。
 *
 * 职责划分：
 *   - 写入/修改/删除走 MCP（memo_ops / note_ops），AI 是唯一写路径（没有键盘）。
 *   - AI 需要的当前状态不走 MCP：app_memo_state_push() 把带 id 的快照塞进
 *     会话上下文（custom.param），云端大模型直接看得见，不需要 list 工具。
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __APP_MEMO_TOOLS_H__
#define __APP_MEMO_TOOLS_H__

#include "tal_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief 注册 memo_ops / note_ops 两个 MCP 工具。
 *
 * 必须在 ai_mcp_init() 建好服务器之后调用（app_avatar_tools 的 MQTT 连接回调里
 * 已经满足）。失败只返回错误码，不影响动作工具。
 *
 * @return OPRT_OK on success.
 */
OPERATE_RET app_memo_tools_register(void);

/**
 * @brief 把备忘录/笔记的当前状态推给云端。
 *
 * 有活跃 AI 会话时走 update_context 立即生效；没有会话时存进 session_param，
 * 下一次会话创建时带上。每次 MCP 改动模型之后都要调一次，否则云端按旧快照回答。
 */
void app_memo_state_push(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MEMO_TOOLS_H__ */
