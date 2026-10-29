/**
 * @file app_memo_tools.c
 * @brief 备忘录/笔记的 MCP 工具实现。设计说明见 app_memo_tools.h。
 *
 * 两个工具，各自覆盖增/改/删：
 *
 *   memo_ops(action=add|update|delete, id, text, when, done)
 *   note_ops(action=add|update|delete, id, title, body)
 *
 * 状态同步不走 MCP：改完模型后 app_memo_state_push() 把带 id 的快照塞进
 * 会话上下文，云端下一轮直接看得见 id 和文字，update/delete 不用先 list。
 *
 * 线程：MCP 工具回调跑在系统工作队列（ai_mcp_server.c 用 tal_workq_schedule 派发）。
 * 模型和 LVGL 对象只在显示线程安全，所以回调里用 app_disp_lock() 包住
 * "改模型 + 刷新页面"这一小段。操作是低频的（用户说一句话最多几次），占锁
 * 时间可接受；add 必须同步拿到新 id 返回给云端，所以不做异步投递。
 * 状态推送（网络发送）放在锁外，不占着显示锁做慢事。
 */

#include <string.h>

#include "tal_api.h"

#include "tuya_kconfig.h"

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)

#include "ai_mcp.h"
#include "ai_mcp_server.h"
#include "tuya_ai_agent.h"
#include "tuya_ai_event.h"

#include "app_disp.h"
#include "app_memo_tools.h"
#include "pet_demo.h"
#include "pet_types.h"
#include "pet_ui.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* 快照单条文本上限。32 条备忘录每条约 40 字节，768 足够放满且截断有度。 */
#define MEMO_STATE_MAX 768
#define NOTE_STATE_MAX 768
#define MEMO_JSON_MAX  (MEMO_STATE_MAX + NOTE_STATE_MAX + 128)

/***********************************************************
***********************variable define**********************
***********************************************************/

/* 快照 JSON 的骨架，两个 key 是给云端大模型看的固定字段名。 */
static const char *const MEMO_STATE_FMT = "{\"device_memo_state\":\"%s\",\"device_note_state\":\"%s\"}";

/* SDK 头文件里的 MCP_PROP_INT_DEF 是坏的：形参叫 name，宏体里 ".name = name"
 * 展开时 .name 的 name 也被替换成实参，产生 ". \"id\" = ..." 这种非法序列。
 * MCP_PROP_STR 没踩坑是因为它的形参叫 prop_name。这里定义自己的正确版本。 */
#define APP_MCP_PROP_INT_DEF(prop_name, desc, def_val)                                                                 \
    &(MCP_PROPERTY_DEF_T)                                                                                              \
    {                                                                                                                  \
        .name = prop_name, .type = MCP_PROPERTY_TYPE_INTEGER, .description = desc, .has_default = TRUE,                \
        .default_val.int_val = def_val, .has_range = FALSE                                                             \
    }

/***********************************************************
***********************function define**********************
***********************************************************/

/* 从属性表里取一个字符串参数（ai_mcp_server 已经把 arguments 填进 default_val）。 */
static const char *prv_prop_str(const MCP_PROPERTY_LIST_T *properties, const char *key)
{
    if (NULL == properties || NULL == key) {
        return NULL;
    }
    for (int i = 0; i < properties->count; i++) {
        const MCP_PROPERTY_T *prop = properties->properties[i];
        if (NULL == prop || NULL == prop->name) {
            continue;
        }
        if (0 == strcmp(prop->name, key) && MCP_PROPERTY_TYPE_STRING == prop->type) {
            return prop->default_val.str_val;
        }
    }
    return NULL;
}

/* 取一个 int 参数；没带或类型不符时回默认值（done 用 -1 表示"不改"）。 */
static int prv_prop_int(const MCP_PROPERTY_LIST_T *properties, const char *key, int def_val)
{
    if (NULL == properties || NULL == key) {
        return def_val;
    }
    for (int i = 0; i < properties->count; i++) {
        const MCP_PROPERTY_T *prop = properties->properties[i];
        if (NULL == prop || NULL == prop->name) {
            continue;
        }
        if (0 == strcmp(prop->name, key) && MCP_PROPERTY_TYPE_INTEGER == prop->type) {
            return prop->default_val.int_val;
        }
    }
    return def_val;
}

/* 删除前的安全校验：id 对得上，文字也得对得上（模型给不了就空着跳过校验）。
 * 快照是给模型拿 id 的唯一来源，id 过期（用户手动删过）时靠这层挡住误删。 */
static BOOL_T prv_memo_matches(uint32_t id, const char *text)
{
    PET_TODO_LIST_T *list = pet_demo_todo();

    for (uint32_t i = 0; i < list->count; i++) {
        if (list->items[i].id == id) {
            return (NULL == text || '\0' == text[0] || 0 == strcmp(list->items[i].text, text)) ? TRUE : FALSE;
        }
    }
    return FALSE;
}

static BOOL_T prv_note_matches(uint32_t id, const char *title)
{
    uint32_t count = pet_demo_note_count();

    for (uint32_t i = 0; i < count; i++) {
        const PET_NOTE_T *note = pet_demo_note_at(i);
        if (NULL != note && note->id == id) {
            return (NULL == title || '\0' == title[0] || 0 == strcmp(note->title, title)) ? TRUE : FALSE;
        }
    }
    return FALSE;
}

/* 模型变了之后的统一收尾。必须在 app_disp_lock() 里调：
 *   - 徽标（状态栏"备忘录 N"）任何时候都要刷；
 *   - 整页重建只限备忘录/笔记/主页 —— 拍照、相册、设置正开着时不该被一条
 *     新备忘录顶掉；用户在哪个页面，回主页/列表时自然看到新数据。 */
static void prv_after_model_change(void)
{
    PET_PAGE_E page = pet_ui_current();

    pet_ui_badge_refresh();

    if (PET_PAGE_HOME == page || PET_PAGE_MEMOS == page || PET_PAGE_NOTES == page) {
        pet_ui_refresh();
    }
}

/* ---- 快照 ----------------------------------------------------------------- */

/* 快照内容是 JSON 字符串值，文本里的引号和反斜杠必须抹掉，不然整个 JSON 报废。
 * 备忘录文本来自模型，基本不会有，但防线要立在写入侧。 */
static void prv_state_copy(char *dst, size_t cap, const char *src)
{
    size_t used = 0;

    if (0 == cap) {
        return;
    }
    dst[0] = '\0';

    if (NULL == src) {
        return;
    }

    for (const char *p = src; *p != '\0'; p++) {
        if (used + 2 >= cap) {
            break;
        }
        dst[used++] = ('"' == *p || '\\' == *p) ? ' ' : *p;
    }
    dst[used] = '\0';
}

/* 备忘录快照：一条一行，id 在最前面 —— 模型 update/delete 就照这个 id 调。 */
static void prv_build_memo_state(char *buf, size_t cap)
{
    PET_TODO_LIST_T *list    = pet_demo_todo();
    uint32_t         pending = pet_demo_todo_pending();
    size_t           used    = 0;

    used = (size_t)snprintf(buf, cap, "共%u条，%u条未完成。", (unsigned)list->count, (unsigned)pending);

    for (uint32_t i = 0; i < list->count && used < cap; i++) {
        const PET_TODO_T *item = &list->items[i];

        used += (size_t)snprintf(buf + used, cap - used, "%sid=%u %s%s%s，%s", (i > 0) ? "；" : "", (unsigned)item->id,
                                 item->text, (item->when[0] != '\0') ? " " : "",
                                 (item->when[0] != '\0') ? item->when : "", item->done ? "已完成" : "未完成");
    }

    if (used >= cap) {
        buf[cap - 1] = '\0';
    }
}

/* 笔记快照：只带标题和一行摘要，全文不进上下文（8 条笔记全文会把快照撑爆）。 */
static void prv_build_note_state(char *buf, size_t cap)
{
    uint32_t count = pet_demo_note_count();
    size_t   used  = 0;

    used = (size_t)snprintf(buf, cap, "共%u条。", (unsigned)count);

    for (uint32_t i = 0; i < count && used < cap; i++) {
        const PET_NOTE_T *note = pet_demo_note_at(i);

        if (NULL == note) {
            break;
        }

        used += (size_t)snprintf(buf + used, cap - used, "%sid=%u %s，%s%s", (i > 0) ? "；" : "", (unsigned)note->id,
                                 note->title, note->time, (note->line_count > 0) ? note->lines[0] : "");
    }

    if (used >= cap) {
        buf[cap - 1] = '\0';
    }
}

void app_memo_state_push(void)
{
    char memo[MEMO_STATE_MAX];
    char note[NOTE_STATE_MAX];
    char json[MEMO_JSON_MAX];

    prv_build_memo_state(memo, sizeof(memo));
    prv_build_note_state(note, sizeof(note));

    char memo_safe[MEMO_STATE_MAX];
    char note_safe[NOTE_STATE_MAX];

    prv_state_copy(memo_safe, sizeof(memo_safe), memo);
    prv_state_copy(note_safe, sizeof(note_safe), note);
    snprintf(json, sizeof(json), MEMO_STATE_FMT, memo_safe, note_safe);

    AI_AGENT_SESSION_T *session = tuya_ai_agent_get_session(NULL);
    if (NULL != session) {
        OPERATE_RET rt = tuya_ai_event_update_context(session->sid, session->eid, json);
        if (OPRT_OK != rt) {
            PR_WARN("[memo] 会话中推送状态失败: %d", rt);
        } else {
            PR_DEBUG("[memo] 状态已推入活跃会话");
        }
    } else {
        /* 没有活跃会话：存进 session_param，下一次会话创建时自动带上。
         * tuya_ai_agent_set_session_param 内部会拷贝字符串，栈缓冲可以传。 */
        tuya_ai_agent_set_session_param(json);
        PR_DEBUG("[memo] 状态已存入会话参数");
    }
}

/* ---- memo_ops ------------------------------------------------------------- */

static void prv_memo_add(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    const char *text = prv_prop_str(properties, "text");
    const char *when = prv_prop_str(properties, "when");
    char        resp[48];
    uint32_t    new_id = 0;

    if (NULL == text || '\0' == text[0]) {
        ai_mcp_return_value_set_str(ret_val, "error: text is required for add");
        return;
    }

    app_disp_lock();
    new_id = pet_demo_memo_add(text, when);
    if (0 != new_id) {
        prv_after_model_change();
    }
    app_disp_unlock();

    if (0 == new_id) {
        ai_mcp_return_value_set_str(ret_val, "error: memo list is full");
        return;
    }

    app_memo_state_push();
    snprintf(resp, sizeof(resp), "ok id=%u", (unsigned)new_id);
    ai_mcp_return_value_set_str(ret_val, resp);
}

static void prv_memo_update(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    int         id   = prv_prop_int(properties, "id", 0);
    const char *text = prv_prop_str(properties, "text");
    const char *when = prv_prop_str(properties, "when");
    int         done = prv_prop_int(properties, "done", -1);
    BOOL_T      ok;

    if (id <= 0) {
        ai_mcp_return_value_set_str(ret_val, "error: id is required for update");
        return;
    }

    app_disp_lock();
    ok = pet_demo_memo_update((uint32_t)id, text, when, done);
    if (TRUE == ok) {
        prv_after_model_change();
    }
    app_disp_unlock();

    if (TRUE == ok) {
        app_memo_state_push();
        ai_mcp_return_value_set_str(ret_val, "ok");
    } else {
        ai_mcp_return_value_set_str(ret_val, "error: id not found");
    }
}

static void prv_memo_delete(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    int         id   = prv_prop_int(properties, "id", 0);
    const char *text = prv_prop_str(properties, "text");
    BOOL_T      ok   = FALSE;

    if (id <= 0) {
        ai_mcp_return_value_set_str(ret_val, "error: id is required for delete");
        return;
    }

    app_disp_lock();
    if (TRUE == prv_memo_matches((uint32_t)id, text)) {
        ok = pet_demo_memo_delete_id((uint32_t)id);
        if (TRUE == ok) {
            prv_after_model_change();
        }
    }
    app_disp_unlock();

    if (TRUE == ok) {
        app_memo_state_push();
        ai_mcp_return_value_set_str(ret_val, "ok");
    } else {
        /* id 不存在或文字对不上都走这里 —— 让模型知道没删成，别对用户谎报。 */
        ai_mcp_return_value_set_str(ret_val, "error: id not found or text mismatch");
    }
}

static OPERATE_RET prv_mcp_memo_ops(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *action = prv_prop_str(properties, "action");

    (void)user_data;

    if (NULL == action || '\0' == action[0]) {
        ai_mcp_return_value_set_str(ret_val, "error: action is required");
        return OPRT_OK;
    }

    if (0 == strcmp(action, "add")) {
        prv_memo_add(properties, ret_val);
    } else if (0 == strcmp(action, "update")) {
        prv_memo_update(properties, ret_val);
    } else if (0 == strcmp(action, "delete")) {
        prv_memo_delete(properties, ret_val);
    } else {
        ai_mcp_return_value_set_str(ret_val, "error: unknown action, use add/update/delete");
    }

    return OPRT_OK;
}

/* ---- note_ops ------------------------------------------------------------- */

static void prv_note_add(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    const char *title = prv_prop_str(properties, "title");
    const char *body  = prv_prop_str(properties, "body");
    char        resp[48];
    uint32_t    new_id = 0;

    if (NULL == title || '\0' == title[0]) {
        ai_mcp_return_value_set_str(ret_val, "error: title is required for add");
        return;
    }

    app_disp_lock();
    new_id = pet_demo_note_add(title, body);
    if (0 != new_id) {
        prv_after_model_change();
    }
    app_disp_unlock();

    if (0 == new_id) {
        ai_mcp_return_value_set_str(ret_val, "error: note list is full");
        return;
    }

    app_memo_state_push();
    snprintf(resp, sizeof(resp), "ok id=%u", (unsigned)new_id);
    ai_mcp_return_value_set_str(ret_val, resp);
}

static void prv_note_update(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    int         id    = prv_prop_int(properties, "id", 0);
    const char *title = prv_prop_str(properties, "title");
    const char *body  = prv_prop_str(properties, "body");
    BOOL_T      ok;

    if (id <= 0) {
        ai_mcp_return_value_set_str(ret_val, "error: id is required for update");
        return;
    }

    app_disp_lock();
    ok = pet_demo_note_update((uint32_t)id, title, body);
    if (TRUE == ok) {
        prv_after_model_change();
    }
    app_disp_unlock();

    if (TRUE == ok) {
        app_memo_state_push();
        ai_mcp_return_value_set_str(ret_val, "ok");
    } else {
        ai_mcp_return_value_set_str(ret_val, "error: id not found");
    }
}

static void prv_note_delete(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val)
{
    int         id    = prv_prop_int(properties, "id", 0);
    const char *title = prv_prop_str(properties, "title");
    BOOL_T      ok    = FALSE;

    if (id <= 0) {
        ai_mcp_return_value_set_str(ret_val, "error: id is required for delete");
        return;
    }

    app_disp_lock();
    if (TRUE == prv_note_matches((uint32_t)id, title)) {
        ok = pet_demo_note_delete_id((uint32_t)id);
        if (TRUE == ok) {
            prv_after_model_change();
        }
    }
    app_disp_unlock();

    if (TRUE == ok) {
        app_memo_state_push();
        ai_mcp_return_value_set_str(ret_val, "ok");
    } else {
        ai_mcp_return_value_set_str(ret_val, "error: id not found or title mismatch");
    }
}

static OPERATE_RET prv_mcp_note_ops(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *action = prv_prop_str(properties, "action");

    (void)user_data;

    if (NULL == action || '\0' == action[0]) {
        ai_mcp_return_value_set_str(ret_val, "error: action is required");
        return OPRT_OK;
    }

    if (0 == strcmp(action, "add")) {
        prv_note_add(properties, ret_val);
    } else if (0 == strcmp(action, "update")) {
        prv_note_update(properties, ret_val);
    } else if (0 == strcmp(action, "delete")) {
        prv_note_delete(properties, ret_val);
    } else {
        ai_mcp_return_value_set_str(ret_val, "error: unknown action, use add/update/delete");
    }

    return OPRT_OK;
}

/* ---- 注册 ------------------------------------------------------------------ */

/* 工具描述是给大模型看的提示词，所以用英文。三条硬规则必须写死：
 *  1. id 只从上下文的 device_memo_state / device_note_state 里拿，不许猜；
 *  2. delete 只在用户明确要求时调；
 *  3. update 的空字段 = 不改，防止模型改一个字段把别的清空。 */
static OPERATE_RET prv_register_memo_tool(void)
{
    return AI_MCP_TOOL_ADD(
        "memo_ops",
        "Manage the pet's 备忘录 list (time-bound tasks the user sees on screen).\n"
        "Actions:\n"
        "- add: create a memo. 'text' required (short Chinese, under 20 chars); "
        "'when' optional display time like 明天 09:00.\n"
        "- update: change one memo. 'id' required; non-empty 'text'/'when' replace "
        "those fields; 'done' 1=completed 0=reopened, omit to leave unchanged.\n"
        "- delete: remove one memo. 'id' required; also pass its current 'text' "
        "from your context as a safety check.\n"
        "Get ids from device_memo_state in your context; never guess an id. "
        "Only delete when the user explicitly asks to delete.\n"
        "Response: 'ok id=N' for add, otherwise 'ok' or 'error: ...'.",
        prv_mcp_memo_ops, NULL, MCP_PROP_STR("action", "add / update / delete."),
        APP_MCP_PROP_INT_DEF("id", "Memo id from device_memo_state. Required for update/delete; ignored for add.", 0),
        MCP_PROP_STR_DEF("text", "Memo text. Required for add; for delete, the memo's current text as a safety check.",
                         ""),
        MCP_PROP_STR_DEF("when", "Display time like 明天 09:00. Optional; empty means no time.", ""),
        APP_MCP_PROP_INT_DEF("done", "1=completed, 0=reopened, omit/-1=unchanged. Only for update.", -1));
}

static OPERATE_RET prv_register_note_tool(void)
{
    return AI_MCP_TOOL_ADD(
        "note_ops",
        "Manage the pet's 笔记 list (free-form notes without a time).\n"
        "Actions:\n"
        "- add: create a note. 'title' required (short Chinese); 'body' optional, "
        "may contain newlines.\n"
        "- update: change one note. 'id' required; non-empty 'title'/'body' replace "
        "those fields; empty means leave unchanged.\n"
        "- delete: remove one note. 'id' required; also pass its current 'title' "
        "from your context as a safety check.\n"
        "Get ids from device_note_state in your context; never guess an id. "
        "Only delete when the user explicitly asks to delete.\n"
        "Response: 'ok id=N' for add, otherwise 'ok' or 'error: ...'.",
        prv_mcp_note_ops, NULL, MCP_PROP_STR("action", "add / update / delete."),
        APP_MCP_PROP_INT_DEF("id", "Note id from device_note_state. Required for update/delete; ignored for add.", 0),
        MCP_PROP_STR_DEF("title",
                         "Note title. Required for add; for delete, the note's current title as a safety "
                         "check.",
                         ""),
        MCP_PROP_STR_DEF("body", "Note body, may contain newlines. Optional.", ""));
}

/***********************************************************
************************* public api ***********************
***********************************************************/

OPERATE_RET app_memo_tools_register(void)
{
    OPERATE_RET rt;

    rt = prv_register_memo_tool();
    TUYA_CALL_ERR_RETURN(rt);

    rt = prv_register_note_tool();
    TUYA_CALL_ERR_RETURN(rt);

    /* 注册完立刻存一份快照：MQTT 刚连上、第一次会话还没建，这份数据会作为
     * session_param 进第一次会话，模型开场就知道设备上有什么。 */
    app_memo_state_push();

    PR_NOTICE("[memo] 2 个备忘录/笔记工具已登记：memo_ops / note_ops");
    return OPRT_OK;
}

#endif /* ENABLE_COMP_AI_MCP */
