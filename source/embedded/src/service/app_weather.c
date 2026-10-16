/**
 * @file app_weather.c
 * @brief 连上 WiFi 后拉取 3 日天气，串口打印。
 *
 * 流程：
 *   1) APP_EVT_NET_STATUS 变 APP_NET_UP 时，起一个独立线程（HTTP 是阻塞调用，
 *      不能在事件回调 / 定时器线程里直接发）。
 *   2) 逐个尝试 ipwho.is / ip-api / ipinfo，**每个都把结果打印出来**，坐标取
 *      第一个成功的那家（开关：WEATHER_PROVIDER_* / WEATHER_TRY_ALL_PROVIDERS）。
 *      全失败则回退到 location=ip，让心知按请求 IP 自己识别。
 *      ⚠ IP 定位只能到"运营商注册地"：手机热点走运营商 CGNAT 大池，库里记的
 *      是省会/注册地，必然飘。换服务商解决不了（实测三家结果完全一致）。
 *      真要准就用下面的 wcity 命令直接指定城市。
 *   3) GET 心知天气 daily.json (days=3) -> 解析 results[0].daily，串口打印。
 *
 * HTTP 客户端用的是 SDK 的 http_client_request()（src/libhttp）。它内部自己做
 * DNS + TCP + 收包，返回的 body 由它自己分配，用完后 http_client_free() 释放。
 *
 * 【失败策略：纯尽力而为】
 * 天气只是个附加功能，网络不通/超时/DNS 失败/JSON 解析失败都只影响它自己：
 *   · 全部吞掉，只打 WARN 日志，不置错误状态、不重试、不通知任何人；
 *   · 失败后线程正常退出，等下一次 NET_UP 事件（重连）自然再试一次；
 *   · 整个定位阶段有总预算（WEATHER_LOC_BUDGET_MS），超了就放弃坐标直接用
 *     location=ip，绝不让线程无限期挂着；
 *   · 任何路径都不阻塞主流程：HTTP 全在自己的线程里，事件回调只起线程。
 *
 * 【给界面用】
 * 解析成功后把结果写进带锁快照（sg_info），再 app_event_publish(
 * APP_EVT_WEATHER_UPDATED)。界面用 app_weather_get() 取数据 —— 它只拷贝数据、
 * 不碰 LVGL；刷界面必须在 LVGL 线程里做（主页 = 事件只置脏位，lv_timer 消费）。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "tal_api.h"
#include "tal_system.h"
#include "tal_memory.h"
#include "tal_mutex.h"
#include "tal_cli.h"
#include "http_client_interface.h"
#include "cJSON.h"

#include "app_event.h"
#include "app_net.h"
#include "app_settings.h"
#include "app_storage.h"
#include "app_weather.h"

/* 心知天气私钥（用户提供的免费版私钥） */
#define WEATHER_PRIVATE_KEY   "Sk_SyG7w7r08lkJX_"

#define WEATHER_HTTP_TIMEOUT  8000    /* 心知请求单次超时 8s */
#define WEATHER_LOC_TIMEOUT   4000    /* 每个定位源单次超时 4s（响应很小） */
#define WEATHER_LOC_BUDGET_MS 15000   /* 定位阶段总预算（安全网：全试一遍约 12s） */

#define WEATHER_THREAD_STACK  10240
#define WEATHER_THREAD_PRIO   THREAD_PRIO_4

#define WEATHER_DAYS_MAX      3

/* ==================== 定位源开关（要调就调这里） ====================
 * 1 = 启用 / 0 = 停用（停用的源会被跳过，日志里显示"已关闭"）。
 * 数组顺序 = 优先级：日志按顺序往下打印，最终坐标取"第一个成功的"那家。
 * 增删源：在 sg_loc_providers[] 里增删一行 + 在下面加一个宏。
 *
 * 另有两个"开关"不在宏里，而在 KV 配置里（运行期生效、不用重编译）：
 *   APP_SET_CITY（键 `city`）/ 串口命令 wcity —— 填了城市就完全跳过 IP 定位。
 */
#define WEATHER_PROVIDER_IPWHOIS   1   /* ipwho.is  ：明文 HTTP、免 key、无频率硬限制 */
#define WEATHER_PROVIDER_IPAPI     1   /* ip-api.com：明文 HTTP、免 key、45 次/分、lang=zh-CN 出中文 */
#define WEATHER_PROVIDER_IPINFO    1   /* ipinfo.io ：明文 HTTP、免 token、字段精简 */

/* 1 = 依次问遍所有启用的源，每个都打印结果（慢但一眼看全）
 * 0 = 第一个成功的就用，后面的不再问（快、省流量） */
#define WEATHER_TRY_ALL_PROVIDERS  1

static THREAD_HANDLE sg_thread     = NULL;
static volatile BOOL_T sg_inflight = FALSE;  /* 正在请求，避免重复起线程 */

/* 给界面用的天气快照。写方是天气线程、读方可以是任意线程（包括 LVGL 任务），
 * 所以一律在 sg_info_lock 里整体拷贝 —— 不跨锁做别的事，也不在锁里 publish。 */
static app_weather_info_t sg_info      = {0};
static MUTEX_HANDLE       sg_info_lock = NULL;

/* ---------- 工具 ---------- */

/* 把一段可能是非 NUL 结尾的 body 复制成 NUL 结尾串，方便 cJSON_Parse。
 * 返回 tal_malloc 的指针（调方负责 tal_free），失败返回 NULL。 */
static char *prv_dup_body(const uint8_t *body, size_t len)
{
    char *s = (char *)tal_malloc(len + 1);

    if (NULL == s) {
        return NULL;
    }
    memcpy(s, body, len);
    s[len] = '\0';
    return s;
}

/* ---------- 取位置（多源：ipwho.is → ip-api → ipinfo） ---------- */

/* 一个定位源能给出的展示信息。真正驱动天气请求的只有经纬度，其余字段
 * 只用于日志展示，方便判断"这个出口 IP 被算到了哪"。 */
typedef struct {
    double lat;
    double lon;
    char   city[48];
    char   region[48];
    char   country[32];
    char   isp[64];
} weather_loc_t;

/* 把 parent 下的字符串字段安全拷进定长缓冲；字段缺失/类型不对则保持 dst 不动 */
static void prv_json_str(cJSON *parent, const char *key, char *dst, size_t cap)
{
    if (NULL == parent) {
        return;
    }

    cJSON *v = cJSON_GetObjectItem(parent, key);

    if (NULL != v && cJSON_IsString(v) && NULL != v->valuestring) {
        (void)snprintf(dst, cap, "%s", v->valuestring);
    }
}

/* 发一次 GET、把 body 复制成 NUL 串再 cJSON_Parse，返回 root（调方 cJSON_Delete）。
 * 任何一步失败都返回 NULL（超时/断网/DNS/解析失败一视同仁，调用方只管降级）。
 * resp 由本函数内部释放。失败只打 DEBUG 级日志——这是预期内的、已被处理的情况。 */
static cJSON *prv_get_json(const char *host, const char *path, uint32_t timeout_ms)
{
    http_client_response_t resp = {0};
    http_client_status_t st = http_client_request(
        &(const http_client_request_t){
            .host = host,
            .path = path,
            .method = "GET",
            .timeout_ms = timeout_ms,
        },
        &resp);

    if (HTTP_CLIENT_SUCCESS != st || NULL == resp.body || 0 == resp.body_length) {
        PR_DEBUG("[weather] GET %s%s 未成功: %d（忽略）", host, path, (int)st);
        http_client_free(&resp);
        return NULL;
    }

    char *js = prv_dup_body(resp.body, resp.body_length);
    http_client_free(&resp);          /* body 就在 resp 里，复制完立刻还掉 */

    if (NULL == js) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(js);
    tal_free(js);
    return root;
}

/* ipwho.is：http://ipwho.is/   （明文 HTTP、免 key）
 * {"success":true,"latitude":30.29,"longitude":120.16,"city":"Hangzhou",
 *  "region":"Zhejiang Sheng","country":"China","connection":{"isp":"..."}} */
static BOOL_T prv_loc_from_ipwhois(weather_loc_t *o)
{
    cJSON *root = prv_get_json("ipwho.is", "/", WEATHER_LOC_TIMEOUT);

    if (NULL == root) {
        return FALSE;
    }

    BOOL_T ok = FALSE;
    cJSON *succ = cJSON_GetObjectItem(root, "success");
    cJSON *lat  = cJSON_GetObjectItem(root, "latitude");
    cJSON *lon  = cJSON_GetObjectItem(root, "longitude");

    if (cJSON_IsTrue(succ) && cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        o->lat = lat->valuedouble;
        o->lon = lon->valuedouble;
        prv_json_str(root, "city",    o->city,    sizeof(o->city));
        prv_json_str(root, "region",  o->region,  sizeof(o->region));
        prv_json_str(root, "country", o->country, sizeof(o->country));
        prv_json_str(cJSON_GetObjectItem(root, "connection"), "isp",
                     o->isp, sizeof(o->isp));
        ok = TRUE;
    }
    cJSON_Delete(root);
    return ok;
}

/* ip-api.com：http://ip-api.com/json/?lang=zh-CN  （明文 HTTP、免 key，45 次/分）
 * 加 lang=zh-CN 则直接返回中文省市，省掉 GBK 转换。
 * {"status":"success","country":"中国","regionName":"浙江","city":"杭州",...} */
static BOOL_T prv_loc_from_ipapi(weather_loc_t *o)
{
    cJSON *root = prv_get_json("ip-api.com", "/json/?lang=zh-CN", WEATHER_LOC_TIMEOUT);

    if (NULL == root) {
        return FALSE;
    }

    BOOL_T ok = FALSE;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *lat    = cJSON_GetObjectItem(root, "lat");
    cJSON *lon    = cJSON_GetObjectItem(root, "lon");

    if (NULL != status && cJSON_IsString(status) &&
        strcmp(status->valuestring, "success") == 0 &&
        cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        o->lat = lat->valuedouble;
        o->lon = lon->valuedouble;
        prv_json_str(root, "city",       o->city,    sizeof(o->city));
        prv_json_str(root, "regionName", o->region,  sizeof(o->region));
        prv_json_str(root, "country",    o->country, sizeof(o->country));
        prv_json_str(root, "isp",        o->isp,     sizeof(o->isp));
        ok = TRUE;
    }
    cJSON_Delete(root);
    return ok;
}

/* ipinfo.io：http://ipinfo.io/json  （明文 HTTP；不带 token 字段较精简，
 * 但城市 + loc("lat,lon") 够用）
 * {"city":"Hangzhou","region":"Zhejiang","country":"CN",
 *  "loc":"30.2936,120.1614","org":"AS4837 CHINA UNICOM ..."} */
static BOOL_T prv_loc_from_ipinfo(weather_loc_t *o)
{
    cJSON *root = prv_get_json("ipinfo.io", "/json", WEATHER_LOC_TIMEOUT);

    if (NULL == root) {
        return FALSE;
    }

    BOOL_T ok = FALSE;
    cJSON *loc = cJSON_GetObjectItem(root, "loc");

    if (NULL != loc && cJSON_IsString(loc)) {
        double la = 0, lo = 0;

        if (2 == sscanf(loc->valuestring, "%lf,%lf", &la, &lo)) {
            o->lat = la;
            o->lon = lo;
            prv_json_str(root, "city",    o->city,    sizeof(o->city));
            prv_json_str(root, "region",  o->region,  sizeof(o->region));
            prv_json_str(root, "country", o->country, sizeof(o->country));
            prv_json_str(root, "org",     o->isp,     sizeof(o->isp));
            ok = TRUE;
        }
    }
    cJSON_Delete(root);
    return ok;
}

typedef struct {
    const char *name;
    BOOL_T (*fn)(weather_loc_t *out);
    uint8_t     enabled;      /* 取自 WEATHER_PROVIDER_* 宏 */
} loc_provider_t;

/* 顺序 = 优先级，也是日志打印顺序、以及"第一个成功者胜出"的顺序。
 * ipwho.is 排第一：免 key，且没有 ip-api 那样 45 次/分的硬限制
 * （设备反复重启时 ip-api 更容易被限流）。 */
static const loc_provider_t sg_loc_providers[] = {
    { "ipwho.is", prv_loc_from_ipwhois, WEATHER_PROVIDER_IPWHOIS },
    { "ip-api",   prv_loc_from_ipapi,   WEATHER_PROVIDER_IPAPI   },
    { "ipinfo",   prv_loc_from_ipinfo,  WEATHER_PROVIDER_IPINFO  },
};

#define LOC_PROVIDER_NUM  (sizeof(sg_loc_providers) / sizeof(sg_loc_providers[0]))

/* 遍历定位源，逐个打印结果：
 *   WEATHER_TRY_ALL_PROVIDERS=1 → 每个启用的源都问一遍、都打印（默认）
 *                            =0 → 只问第一个成功的，后面不再问
 * 最终坐标取"第一个成功"的那家（顺序即优先级）；全失败返回 NULL，由上层
 * 回退到 location=ip。任何失败都只打印、只跳过，绝不影响其它逻辑。
 * 总预算 WEATHER_LOC_BUDGET_MS 只是安全网（全试一遍约 12s）。 */
static const char *prv_fetch_location(weather_loc_t *out)
{
    SYS_TIME_T    t0     = tal_system_get_millisecond();
    weather_loc_t best   = {0};
    const char   *best_n = NULL;

    PR_NOTICE("[weather] ---- IP 定位开始（try_all=%d） ----",
              (int)WEATHER_TRY_ALL_PROVIDERS);

    for (size_t i = 0; i < LOC_PROVIDER_NUM; i++) {
        const loc_provider_t *p = &sg_loc_providers[i];

        if (!p->enabled) {
            PR_NOTICE("[weather]  %-9s -> 已关闭（WEATHER_PROVIDER_* 宏）", p->name);
            continue;
        }

#if WEATHER_TRY_ALL_PROVIDERS == 0
        if (NULL != best_n) {
            break;   /* 非"全试"模式：拿到一个就够，后面的不问了 */
        }
#endif

        if (i > 0 && (tal_system_get_millisecond() - t0) >= WEATHER_LOC_BUDGET_MS) {
            PR_WARN("[weather] 定位已超 %u ms 预算，停止尝试剩余源",
                    (unsigned)WEATHER_LOC_BUDGET_MS);
            break;
        }

        weather_loc_t one = {0};

        if (p->fn(&one)) {
            PR_NOTICE("[weather]  %-9s -> %s %s %s (%.4f,%.4f) ISP:%s",
                      p->name, one.country, one.region, one.city,
                      one.lat, one.lon, one.isp);

            if (NULL == best_n) {
                best   = one;
                best_n = p->name;
            }
        } else {
            PR_WARN("[weather]  %-9s -> 失败 / 无结果（忽略）", p->name);
        }
    }

    if (NULL == best_n) {
        PR_WARN("[weather] 定位没拿到坐标，回退 location=ip（不影响其它功能）");
        return NULL;
    }

    *out = best;
    PR_NOTICE("[weather] ---- 采用 %s 的坐标 ----", best_n);
    return best_n;
}

/* ---------- 取天气（心知天气） ---------- */

/* 城市名必须是 ASCII（拼音 / 心知的城市 ID），否则塞进 URL 请求行会变成非法字节 */
static BOOL_T prv_is_ascii(const char *s)
{
    for (; '\0' != *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 心知 v3 的 code_day/code_night 是**字符串**（"0"），老接口是数字 —— 两种都收，
 * 缺失/类型不对一律当成"未知"（99），这样界面直接拿 code 去查图标即可。 */
static int32_t prv_json_code(cJSON *parent, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(parent, key);

    if (NULL == v) {
        return APP_WEATHER_CODE_UNKNOWN;
    }
    if (cJSON_IsString(v) && NULL != v->valuestring) {
        return (int32_t)atoi(v->valuestring);
    }
    if (cJSON_IsNumber(v)) {
        return (int32_t)v->valuedouble;
    }
    return APP_WEATHER_CODE_UNKNOWN;
}

/* ---------- 快照（写 / 读） ---------- */

/* 整块换掉快照。天气线程调用；加锁动作只有这一次拷贝。 */
static void prv_info_store(const app_weather_info_t *in)
{
    if (NULL == sg_info_lock) {
        return;
    }
    tal_mutex_lock(sg_info_lock);
    sg_info = *in;
    tal_mutex_unlock(sg_info_lock);
}

BOOL_T app_weather_get(app_weather_info_t *out)
{
    if (NULL == out) {
        return FALSE;
    }

    memset(out, 0, sizeof(*out));
    if (NULL == sg_info_lock) {
        return FALSE;                     /* init 还没跑；按"没数据"处理 */
    }

    tal_mutex_lock(sg_info_lock);
    *out = sg_info;
    tal_mutex_unlock(sg_info_lock);

    return out->valid;
}

static void prv_fetch_weather_once(void)
{
    /* 城市优先：IP 定位按的是"公网 IP 的运营商注册地"，实测手机热点会漂到
     * 隔壁城市（被算成绍兴）。所以只要配置里填了城市（拼音，如 "hangzhou"，
     * 见下面的 wcity 命令）就直接用它，完全跳过 IP 定位——这是唯一可靠的路径。 */
    char city_cfg[40] = {0};
    app_settings_get_str(APP_SET_CITY, city_cfg, sizeof(city_cfg));

    char path[320];
    if ('\0' != city_cfg[0] && prv_is_ascii(city_cfg)) {
        (void)snprintf(path, sizeof(path),
                       "/v3/weather/daily.json?key=%s&location=%s"
                       "&language=zh-Hans&unit=c&start=0&days=3",
                       WEATHER_PRIVATE_KEY, city_cfg);
        PR_NOTICE("[weather] 使用配置城市: %s", city_cfg);
    } else {
        if ('\0' != city_cfg[0]) {
            PR_WARN("[weather] 城市 '%s' 含非 ASCII 字符（要拼音），改用 IP 定位", city_cfg);
        }

        weather_loc_t geo = {0};

        /* 构造请求路径。location 用 纬度:经度（心知格式，冒号分隔），
         * 拿不到坐标就回退到 location=ip（心知按请求 IP 自动识别）。 */
        if (NULL != prv_fetch_location(&geo)) {
            (void)snprintf(path, sizeof(path),
                           "/v3/weather/daily.json?key=%s&location=%.4f:%.4f"
                           "&language=zh-Hans&unit=c&start=0&days=3",
                           WEATHER_PRIVATE_KEY, geo.lat, geo.lon);
        } else {
            (void)snprintf(path, sizeof(path),
                           "/v3/weather/daily.json?key=%s&location=ip"
                           "&language=zh-Hans&unit=c&start=0&days=3",
                           WEATHER_PRIVATE_KEY);
        }
    }

    http_client_response_t resp = {0};
    http_client_status_t st = http_client_request(
        &(const http_client_request_t){
            .host = "api.seniverse.com",
            .path = path,
            .method = "GET",
            .timeout_ms = WEATHER_HTTP_TIMEOUT,
        },
        &resp);

    if (HTTP_CLIENT_SUCCESS != st || NULL == resp.body || 0 == resp.body_length) {
        PR_WARN("[weather] 心知天气请求未成功: %d（忽略，等下次重连）", (int)st);
        http_client_free(&resp);
        return;
    }

    char *js = prv_dup_body(resp.body, resp.body_length);
    if (NULL == js) {
        PR_DEBUG("[weather] 复制 body 失败（内存不足，忽略）");
        http_client_free(&resp);
        return;
    }

    cJSON *root = cJSON_Parse(js);
    if (NULL == root) {
        PR_WARN("[weather] 心知天气 JSON 解析失败（忽略）");
        tal_free(js);
        http_client_free(&resp);
        return;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || 0 == cJSON_GetArraySize(results)) {
        cJSON *s = cJSON_GetObjectItem(root, "status");
        PR_WARN("[weather] 心知天气无结果: %s（忽略）",
                (s && cJSON_IsString(s)) ? s->valuestring : "unknown");
        cJSON_Delete(root);
        tal_free(js);
        http_client_free(&resp);
        return;
    }

    cJSON *r0    = cJSON_GetArrayItem(results, 0);
    cJSON *loc   = cJSON_GetObjectItem(r0, "location");
    cJSON *daily = cJSON_GetObjectItem(r0, "daily");

    app_weather_info_t info;
    memset(&info, 0, sizeof(info));
    for (int i = 0; i < APP_WEATHER_DAYS; i++) {
        info.day[i].code = APP_WEATHER_CODE_UNKNOWN;
    }

    const char *city = "未知";
    if (loc) {
        cJSON *name = cJSON_GetObjectItem(loc, "name");
        if (name && cJSON_IsString(name) && '\0' != name->valuestring[0]) {
            city = name->valuestring;
        }
    }
    (void)snprintf(info.city, sizeof(info.city), "%s", city);

    PR_NOTICE("========================================");
    PR_NOTICE("[weather] %s 未来 3 日天气：", city);

    int di = 0;
    if (cJSON_IsArray(daily)) {
        cJSON *day;
        cJSON_ArrayForEach(day, daily) {
            if (di >= WEATHER_DAYS_MAX || di >= APP_WEATHER_DAYS) {
                break;
            }
            const char *date = "", *td = "", *tn = "";
            const char *high = "", *low = "", *wd = "", *ws = "", *hum = "";
            const char *rain = "", *wspd = "";
            cJSON *v;

            if ((v = cJSON_GetObjectItem(day, "date"))       && cJSON_IsString(v)) { date = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "text_day"))   && cJSON_IsString(v)) { td   = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "text_night")) && cJSON_IsString(v)) { tn   = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "high"))       && cJSON_IsString(v)) { high = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "low"))        && cJSON_IsString(v)) { low  = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "wind_direction")) && cJSON_IsString(v)) { wd = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "wind_scale")) && cJSON_IsString(v)) { ws   = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "humidity"))   && cJSON_IsString(v)) { hum  = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "rainfall"))   && cJSON_IsString(v)) { rain = v->valuestring; }
            if ((v = cJSON_GetObjectItem(day, "wind_speed")) && cJSON_IsString(v)) { wspd = v->valuestring; }

            PR_NOTICE("[weather] %s %s(昼)/%s(夜) %s~%s℃ 风:%s%s级 湿度:%s%% 降水:%smm 风速:%skm/h",
                      date, td, tn, low, high, wd, ws, hum, rain, wspd);

            /* ---- 填快照（定长拷贝，全部 NUL 结尾） ---- */
            app_weather_day_t *d = &info.day[di];

            (void)snprintf(d->date,           sizeof(d->date),           "%s", date);
            (void)snprintf(d->text_day,       sizeof(d->text_day),       "%s", td);
            (void)snprintf(d->text_night,     sizeof(d->text_night),     "%s", tn);
            (void)snprintf(d->high,           sizeof(d->high),           "%s", high);
            (void)snprintf(d->low,            sizeof(d->low),            "%s", low);
            (void)snprintf(d->humidity,       sizeof(d->humidity),       "%s", hum);
            (void)snprintf(d->wind_direction, sizeof(d->wind_direction), "%s", wd);
            (void)snprintf(d->wind_scale,     sizeof(d->wind_scale),     "%s", ws);
            (void)snprintf(d->rainfall,       sizeof(d->rainfall),       "%s", rain);
            (void)snprintf(d->wind_speed,     sizeof(d->wind_speed),     "%s", wspd);
            d->code = prv_json_code(day, "code_day");

            di++;
        }
    }
    PR_NOTICE("========================================");

    /* ---- 发布给界面 ----
     * info.days 只在实际解析出几天时非 0；valid 标记"拉到过数据"（哪怕 days=0，
     * 界面也据此把"天气获取中…"换掉）。 */
    info.days  = di;
    info.valid = TRUE;
    prv_info_store(&info);

    /* 顺手把 SD 拉起来（幂等，失败只打日志）：天气 UI 自身不依赖 SD，这一步只是
     * 给将来可能放卡上的资源留个已挂载的接口，顺序在 publish 之前。 */
    (void)app_storage_init();

    /* 刻意在锁**外**发布：订阅方（主页）只置脏位，真正的 LVGL 刷新回到 LVGL 线程。 */
    app_event_publish(APP_EVT_WEATHER_UPDATED, NULL);

    cJSON_Delete(root);
    tal_free(js);
    http_client_free(&resp);
}

/* ---------- 线程入口 ---------- */

static void prv_weather_thread(void *arg)
{
    (void)arg;

    /* 等 DNS / 链路稍微稳一点再发，避免刚连上就超时 */
    tal_system_sleep(2000);
    prv_fetch_weather_once();

    sg_inflight = FALSE;
    tal_thread_delete(sg_thread);
    sg_thread = NULL;
}

/* ---------- 网络状态回调（事件线程，禁止阻塞） ---------- */

static void prv_net_status_cb(APP_EVT_E evt, void *data, void *user)
{
    (void)evt;
    (void)user;

    int status = (NULL != data) ? *(const int *)data : (int)APP_NET_DOWN;

    if (APP_NET_UP != (APP_NET_STATUS_E)status) {
        return;
    }
    if (sg_inflight) {
        return;  /* 正在跑就不再起线程；失败也不重试，等下一次 NET_UP 自然再试 */
    }

    sg_inflight = TRUE;
    THREAD_CFG_T cfg = {0};
    cfg.stackDepth = WEATHER_THREAD_STACK;
    cfg.priority   = WEATHER_THREAD_PRIO;
    cfg.thrdname   = "weather";
    OPERATE_RET rt = tal_thread_create_and_start(&sg_thread, NULL, NULL,
                                                 prv_weather_thread, NULL, &cfg);
    if (OPRT_OK != rt) {
        PR_WARN("[weather] 线程创建失败: %d（忽略，天气不拉而已）", rt);
        sg_inflight = FALSE;
    }
}

/* ---------- 对外 API ---------- */

/* 串口命令（tal_cli 跑在 uart0，就是看日志的那个口）：
 *   wcity            —— 打印当前城市
 *   wcity hangzhou   —— 设为杭州（拼音），写 KV 持久化，下次拉取生效
 * 定位源"逐个试、逐个打印"已经做进拉取流程本身（见 WEATHER_TRY_ALL_PROVIDERS），
 * 所以不再需要单独的对比命令。 */

static void prv_cli_wcity(int argc, char *argv[])
{
    if (argc >= 2) {
        if (OPRT_OK == app_settings_set_str(APP_SET_CITY, argv[1])) {
            PR_NOTICE("[weather] 城市已设为 %s", argv[1]);
        } else {
            PR_ERR("[weather] 城市保存失败");
        }
    }

    char now[40] = {0};
    app_settings_get_str(APP_SET_CITY, now, sizeof(now));

    char line[96];
    (void)snprintf(line, sizeof(line), "[weather] 当前城市: %s (空 = 按 IP 定位)\r\n",
                   ('\0' != now[0]) ? now : "未设置");
    tal_cli_echo(line);
}

/* cli_cmd_t 的 name/help 是 char*（非 const），用可写数组避免字面量告警 */
static char sg_cli_name_wcity[] = "wcity";
static char sg_cli_help_wcity[] = "weather city: wcity <pinyin> | wcity";

static const cli_cmd_t sg_cli_cmds[] = {
    { sg_cli_name_wcity, sg_cli_help_wcity, prv_cli_wcity },
};

/* 注意：本函数**永不返回失败**。天气是附加功能，它的任何问题（订阅不上、
 * CLI 注册不上）都不该让调用方的启动流程走进错误分支。 */
OPERATE_RET app_weather_init(void)
{
    /* 快照锁：必须在这里建（幂等）。app_weather_get() 与写快照都以 NULL 为
     * "还没就绪"的判据 —— 绝不把 NULL 句柄递给 tal_mutex_lock。 */
    if (NULL == sg_info_lock) {
        if (OPRT_OK != tal_mutex_create_init(&sg_info_lock)) {
            sg_info_lock = NULL;
            PR_WARN("[weather] 快照锁创建失败：天气不显示（功能自身降级，不影响别的）");
        }
    }

    if (OPRT_OK != (OPERATE_RET)tal_cli_cmd_register(sg_cli_cmds,
            sizeof(sg_cli_cmds) / sizeof(sg_cli_cmds[0]))) {
        PR_WARN("[weather] wcity 命令注册失败（串口改城市不可用，忽略）");
    }

    if (OPRT_OK != app_event_subscribe(APP_EVT_NET_STATUS, prv_net_status_cb, NULL)) {
        PR_WARN("[weather] 订阅网络事件失败（忽略，天气不工作而已）");
        return OPRT_OK;
    }

    PR_NOTICE("[weather] 天气服务已注册，等待 WiFi 连接");
    return OPRT_OK;
}
