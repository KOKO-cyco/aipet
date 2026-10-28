/**
 * @file app_avatar_tools.c
 * @brief 对话驱动动作：MCP 工具实现。设计说明见 app_avatar_tools.h。
 *
 * 三个工具把大模型的意图翻译成**事件**，落地目标由页面自己决定：
 *
 *   avatar_play_animation   -> APP_EVT_PET_ACTION（data = 动作名）
 *   avatar_set_emotion      -> APP_EVT_AI_EMOTION（data = 情绪名）
 *   avatar_composite_action -> 两者合一（动作优先，情绪兜底）
 *
 * 线程：MCP 工具回调跑在系统工作队列（ai_mcp_server.c 用 tal_workq_schedule 派发），
 * 所以回调里只能做"置标志位"这种轻活 —— app_event_publish 正好就是（同步分发，
 * 订阅方 page_pet.c 只写一个 volatile 请求位，真正的切动作发生在渲染 tick 的
 * 帧边界上）。
 */

#include <string.h>
#include <strings.h>

#include "tal_api.h"

#include "tuya_kconfig.h"

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)

#include "ai_mcp.h"
#include "ai_mcp_server.h"
#include "tuya_ai_agent.h"

#include "app_ai_chat.h"
#include "app_event.h"
#include "app_pages.h"          /* PAGE_PET */
#include "app_router.h"         /* app_router_on_top() */
#include "app_avatar_tools.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* 板子上真实存在的动作 —— **就是 fish_elf.spf 名字表里的那些名字**（见
 * third_party/sprite/build_fish.sh 的 ELF_ORDER 和 app_sprite2d_clip_name()）。这份清单同时用于
 * 两处：给大模型看的工具描述、回调里的名字合法性校验 —— 名字拼错就直接回 false，
 * 不让 LLM 的幻觉变成一次"宠物没反应"。
 *
 * 改动作集（fish_extract.py 的 CLIPS / build_fish.sh 的 --order）时**这里要跟着改**；
 * 漏改的后果是"模型要求演某个新动作，被这层挡下来"。
 *
 * **只列当前动作集真实存在的组**（fish_elf.spf 的 15 组 + idle）。旧套的
 * click_wave / rnd_photo 之类已经随素材没了，列了只会让大模型点名演不存在的动作。 */
#define AVATAR_ANIM_NAMES                                                      \
    "idle, tap_proud, tap_shy, tap_happy, tap_tickle, "                        \
    "fn_photo, fn_note, emo_sad, emo_happy, emo_think, "                       \
    "rnd_gomoku, rnd_pigeon, rnd_yawn, rnd_cat, rnd_lion, rnd_scare"

/* 情绪名 = skill_emotion.h 里的 EMOJI_*（page_pet.c 的情绪表按这些键映射动作）。
 * 大模型给大小写都认（页面里用 strcasecmp 比对），这里列全便于它挑。 */
#define AVATAR_EMOTION_NAMES                                                   \
    "NEUTRAL, HAPPY, LAUGHING, FUNNY, DELICIOUS, SAD, DISAPPOINTED, ANGRY, "   \
    "ANNOYED, FEARFUL, LOVING, KISSY, WAKEUP, EMBARRASSED, CONFUSED, TOUCH, "  \
    "LEFT, RIGHT, SURPRISE, SHOCKED, THINKING, WINK, SILLY, COOL, CONFIDENT, " \
    "RELAXED, SLEEP"

static const char *const sg_anim_whitelist[] = {
    "idle",
    "tap_proud", "tap_shy", "tap_happy", "tap_tickle",
    "fn_photo", "fn_note",
    "emo_sad", "emo_happy", "emo_think",
    "rnd_gomoku", "rnd_pigeon", "rnd_yawn", "rnd_cat", "rnd_lion", "rnd_scare",
};

/* MCP 服务器是否已经建好（= 工具已登记）。MQTT 没连上时是 false。 */
static bool sg_server_up = false;

/***********************************************************
***********************function define**********************
***********************************************************/
static bool prv_anim_known(const char *name)
{
    if (NULL == name || '\0' == name[0]) {
        return false;
    }
    for (uint32_t i = 0; i < sizeof(sg_anim_whitelist) / sizeof(sg_anim_whitelist[0]); i++) {
        if (0 == strcasecmp(name, sg_anim_whitelist[i])) {
            return true;
        }
    }
    return false;
}

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

/* 动作只能演在桌宠页上：页面不在栈顶时请求会被 page_pet.c 丢掉，在这里就回一个
 * FALSE 更诚实（云端知道这次没演成）。
 *
 * **判据是"桌宠页在不在栈顶"，不是"语音栈活没活"。** 以前拿 app_ai_chat_active()
 * 当代理是成立的（语音栈页面级，只有角色页开着它才活）；现在语音栈开机常驻
 * （见 app_ai_chat.h），那个从云端连上起就一直是 TRUE，用它当代理会永远放行。
 *
 * 桌宠页是开机就进的唯一页面，所以正常情况下恒为 TRUE —— 只有 SD 卡不在、
 * 门页（page_nosd）压在栈顶那段时间会返回 false。 */
static bool prv_avatar_ready(void)
{
    if (TRUE != app_router_on_top(PAGE_PET)) {
        PR_WARN("[avatar] 桌宠页不在栈顶，忽略这次动作请求");
        return false;
    }
    return true;
}

/* 把动作名广播出去，由 page_pet.c 查表插播。
 *
 * 这里**不校验这个名字在精灵里存不存在**（那是页面的事，它手上有名字表）——
 * 白名单只挡大模型的幻觉，挡住之后页面按名字查不到会自己打一条 WARN。 */
static void prv_request_action(const char *name)
{
    PR_NOTICE("[avatar] 请求动作 -> %s", name);
    /* 同步分发：name 在这次 publish 期间一直有效，订阅方不要留存指针 */
    app_event_publish(APP_EVT_PET_ACTION, (void *)name);
}

static OPERATE_RET prv_mcp_play_animation(const MCP_PROPERTY_LIST_T *properties,
                                          MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *name = prv_prop_str(properties, "name");

    (void)user_data;

    if (false == prv_anim_known(name) || false == prv_avatar_ready()) {
        ai_mcp_return_value_set_bool(ret_val, FALSE);
        return OPRT_OK;
    }

    prv_request_action(name);
    ai_mcp_return_value_set_bool(ret_val, TRUE);
    return OPRT_OK;
}

static OPERATE_RET prv_mcp_set_emotion(const MCP_PROPERTY_LIST_T *properties,
                                       MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *emotion = prv_prop_str(properties, "emotion");

    (void)user_data;

    if (NULL == emotion || '\0' == emotion[0] || false == prv_avatar_ready()) {
        ai_mcp_return_value_set_bool(ret_val, FALSE);
        return OPRT_OK;
    }

    /* 动作策略留在 page_pet.c 那张情绪表里，这里只把情绪广播出去（与云端的 NLG
     * emoji 标签走的是同一条事件，所以两条触发路径共用一套映射）。 */
    PR_NOTICE("[avatar] 请求情绪 -> %s", emotion);
    app_event_publish(APP_EVT_AI_EMOTION, (void *)emotion);
    ai_mcp_return_value_set_bool(ret_val, TRUE);
    return OPRT_OK;
}

static OPERATE_RET prv_mcp_composite_action(const MCP_PROPERTY_LIST_T *properties,
                                            MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *animation = prv_prop_str(properties, "animation");
    const char *emotion   = prv_prop_str(properties, "emotion");

    (void)user_data;

    if (false == prv_avatar_ready()) {
        ai_mcp_return_value_set_bool(ret_val, FALSE);
        return OPRT_OK;
    }

    /* 画面只有一只宠物：点名了具体动作就用它，没点名（或名字不认识）才退回
     * 情绪表 —— 两条都放会互相抢，先演完情绪映射的动作再被动作覆盖，白闪一下。 */
    if (true == prv_anim_known(animation)) {
        PR_NOTICE("[avatar] 组合动作 -> %s（情绪 %s 忽略）",
                  animation, (NULL != emotion) ? emotion : "-");
        prv_request_action(animation);
    } else if (NULL != emotion && '\0' != emotion[0]) {
        PR_NOTICE("[avatar] 组合动作 -> 情绪 %s（无有效动作名）", emotion);
        app_event_publish(APP_EVT_AI_EMOTION, (void *)emotion);
    } else {
        ai_mcp_return_value_set_bool(ret_val, FALSE);
        return OPRT_OK;
    }

    ai_mcp_return_value_set_bool(ret_val, TRUE);
    return OPRT_OK;
}

/* 工具描述是给大模型看的提示词，所以用英文：列出可选值 + 什么时候该用。
 * 不写清楚可选动作名，模型就会自己编名字（编出来的名字回调里直接被拒）。 */
static OPERATE_RET prv_register_tools(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "avatar_play_animation",
        "Play a body animation on the pet. It plays exactly once (a few seconds) "
        "and then returns to the idle loop by itself, so prefer calling this once "
        "near the START of your reply.\n"
        "Use it to match the emotion or intent of what you are saying "
        "(e.g. tap_happy for good news, emo_sad for sad news, emo_think while "
        "pondering, tap_proud for a boast). Do NOT call it for every sentence.\n"
        "Animations: " AVATAR_ANIM_NAMES ".\n"
        "Response: true if the animation started.",
        prv_mcp_play_animation,
        NULL,
        MCP_PROP_STR("name", "Animation name (exact match, from the list above).")
    ));

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "avatar_set_emotion",
        "Set the pet's mood by name; it plays the action mapped to that emotion. "
        "Use when a mood fits better than a specific animation.\n"
        "Emotions: " AVATAR_EMOTION_NAMES ".\n"
        "Response: true if the mood was applied.",
        prv_mcp_set_emotion,
        NULL,
        MCP_PROP_STR("emotion", "Emotion name (exact match, from the list above).")
    ));

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "avatar_composite_action",
        "Set the pet's body animation and mood in one call. Prefer this over "
        "separate avatar_play_animation + avatar_set_emotion calls when you want the "
        "pet to react at the beginning of your reply.\n"
        "Animations: " AVATAR_ANIM_NAMES ".\n"
        "Response: true if the action was applied.",
        prv_mcp_composite_action,
        NULL,
        MCP_PROP_STR("animation", "Body animation name (exact match, from the list above)."),
        MCP_PROP_STR("emotion", "Emotion name (used only when animation is empty/unknown).")
    ));

    PR_NOTICE("[avatar] 3 个动作工具已登记：play_animation / set_emotion / composite_action");

    sg_server_up = true;
    return OPRT_OK;
}

/* MQTT 连上时才登记工具：MCP 服务器（ai_mcp_init 的回调）先跑，工具必须加在
 * 服务器初始化之后，否则 ai_mcp_server_add_tool 直接返回参数错误。 */
static OPERATE_RET prv_on_mqtt_connected(void *data)
{
    (void)data;
    return prv_register_tools();
}

/* AI 语音栈每次重建都会把 agent 上下文清空 —— tuya_ai_agent_deinit() 里
 * `memset(&ai_agent_ctx, 0, ...)` 会把 mcp_cb 和 enable_mcp 一起抹掉，而 ai_mcp_init()
 * 的 MQTT 订阅是 ONETIME 的，不会再跑第二遍。所以不重挂的话：**第二次拉起语音栈
 * 开始，会话属性里就没有 supportCustomMCP，云端不再把工具喂给大模型，动作通道
 * 静默失效**（日志上只会看到 MCP 请求回 500）。服务器已经在的话，重挂就是一句话。
 *
 * 语音栈现在开机常驻（只在有别的音源 hold 它时才拆），所以这条路径平时不走 ——
 * 留着是因为它便宜，而且"拆过一次就永久失效"这个坑太隐蔽。 */
static void prv_on_ai_stack_up(APP_EVT_E evt, void *data, void *user)
{
    (void)evt;
    (void)data;
    (void)user;

    if (false == sg_server_up) {
        PR_WARN("[avatar] MCP 服务器还没建（MQTT 未连？），跳过重挂");
        return;
    }

    if (OPRT_OK == tuya_ai_agent_mcp_set_cb(ai_mcp_server_parse_message, NULL)) {
        PR_NOTICE("[avatar] AI 栈重建 -> MCP 回调已重新挂上");
    } else {
        PR_ERR("[avatar] MCP 回调重挂失败");
    }
}

/***********************************************************
************************* public api ***********************
***********************************************************/
OPERATE_RET app_avatar_tools_init(void)
{
    OPERATE_RET rt;

    /* 先建 MCP 服务器（内部 tuya_ai_agent_mcp_set_cb -> enable_mcp=TRUE，
     * 会话属性里才会带上 supportCustomMCP） */
    rt = ai_mcp_init();
    if (OPRT_OK != rt) {
        PR_ERR("[avatar] ai_mcp_init 失败: %d", rt);
        return rt;
    }

    /* 再挂自己的工具注册：同一个事件，订阅顺序 = 回调顺序，所以这一句必须在
     * ai_mcp_init() 之后。ONETIME 意味着只此一次 —— 所以整个 init 要在 MQTT
     * 首次连上之前调用（tuya_iot_start 之前）。 */
    rt = tal_event_subscribe(EVENT_MQTT_CONNECTED, "app_avatar_tools",
                             prv_on_mqtt_connected, SUBSCRIBE_TYPE_ONETIME);
    if (OPRT_OK != rt) {
        PR_ERR("[avatar] 订阅 MQTT 事件失败: %d", rt);
        return rt;
    }

    /* 语音栈每次拉起都要把 MCP 回调重挂到 AI agent（原因见 prv_on_ai_stack_up） */
    rt = app_event_subscribe(APP_EVT_AI_STACK_UP, prv_on_ai_stack_up, NULL);
    if (OPRT_OK != rt) {
        PR_ERR("[avatar] 订阅语音栈事件失败: %d", rt);
        return rt;
    }

    PR_NOTICE("[avatar] MCP 动作工具就绪（等 MQTT 连上登记工具）");
    return OPRT_OK;
}

#else /* !ENABLE_COMP_AI_MCP */

OPERATE_RET app_avatar_tools_init(void)
{
    PR_WARN("[avatar] ENABLE_COMP_AI_MCP=0，对话驱动动作未编入本固件");
    return OPRT_OK;
}

#endif /* ENABLE_COMP_AI_MCP */
