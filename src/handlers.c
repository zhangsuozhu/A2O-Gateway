/**
 * @file handlers.c
 * @brief HTTP 路由处理实现（含登录认证）
 *
 * 本文件实现了网关的所有 HTTP 请求处理逻辑。
 * 核心设计：
 *   - 所有请求先经过 handle_root 进行 URI 和方法路由；
 *   - 需要认证的接口统一调用 gateway_auth_ok，支持 x-api-key 头或
 *     Authorization: Bearer 令牌，并与配置文件中的 gateway_api_key 比对；
 *   - /v1/messages 接收 Anthropic 格式请求，转换为 OpenAI 格式后通过
 *     enqueue_job 投递到工作线程池异步处理；
 *   - 管理接口（/admin/api/ *）提供运行时配置查看与热更新能力。
 */

#include "handlers.h"
#include "config.h"
#include "convert.h"
#include "worker.h"
#include "stats.h"
#include "log.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief 从 HTTP 请求头中查找指定名称的字段值
 * @param req  libevent HTTP 请求对象
 * @param name 头部名称（大小写不敏感）
 * @return 头部值的指针，若不存在则返回 NULL
 */
static const char *header_get(struct evhttp_request *req, const char *name) {
    return evhttp_find_header(evhttp_request_get_input_headers(req), name);
}

/**
 * @brief 常量时间字符串比较，防止时序攻击
 * @param a 字符串 a
 * @param b 字符串 b
 * @return 两字符串完全相等返回 true，否则 false
 *
 * 无论字符串内容如何，执行时间都只与长度有关：
 *   1. 先比较长度；若不同直接返回 false；
 *   2. 逐字节按位异或累加到 diff；
 *   3. 最后检查 diff 是否为 0。
 * 这样避免了通过测量比较时间来猜测密钥前缀。
 */
static bool constant_time_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* Session management for admin UI */
typedef struct session_node {
    char token[65];
    struct session_node *next;
} session_node_t;

static session_node_t *g_sessions = NULL;
static pthread_mutex_t g_sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

static void generate_session_token(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    srand((unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ getpid()));
    for (size_t i = 0; i < 64 && i < len - 1; i++) {
        buf[i] = hex[rand() % 16];
    }
    buf[64 < len - 1 ? 64 : len - 1] = 0;
}

static bool session_valid(const char *token) {
    if (!token || !*token) return false;
    pthread_mutex_lock(&g_sessions_mutex);
    session_node_t *n = g_sessions;
    bool ok = false;
    while (n) {
        if (constant_time_eq(n->token, token)) { ok = true; break; }
        n = n->next;
    }
    pthread_mutex_unlock(&g_sessions_mutex);
    return ok;
}

static char *session_create(void) {
    session_node_t *n = (session_node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    generate_session_token(n->token, sizeof(n->token));
    pthread_mutex_lock(&g_sessions_mutex);
    n->next = g_sessions;
    g_sessions = n;
    pthread_mutex_unlock(&g_sessions_mutex);
    return xstrdup(n->token);
}

static void session_destroy(const char *token) {
    if (!token || !*token) return;
    pthread_mutex_lock(&g_sessions_mutex);
    session_node_t **pp = &g_sessions;
    while (*pp) {
        if (constant_time_eq((*pp)->token, token)) {
            session_node_t *d = *pp;
            *pp = d->next;
            free(d);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

/**
 * @brief 通用认证检查
 * @param req      当前 HTTP 请求
 * @param required 期望的密钥（若为空字符串则直接放行）
 * @return 认证通过返回 true
 *
 * 支持两种认证方式：
 *   1. X-Api-Key 头部直接比对；
 *   2. Authorization: Bearer <token> 中的 token 比对。
 * 两者均使用 constant_time_eq 防止时序侧信道。
 */
static bool auth_ok(struct evhttp_request *req, const char *required) {
    if (!required || !*required) return true;
    const char *x = header_get(req, "x-api-key");
    if (constant_time_eq(x, required)) return true;
    const char *a = header_get(req, "authorization");
    if (a && strncasecmp(a, "Bearer ", 7) == 0 && constant_time_eq(a + 7, required)) return true;
    return false;
}

/**
 * @brief 管理员认证检查（用于 /admin/ 路由）
 * @param req 当前 HTTP 请求
 * @return 认证通过返回 true
 *
 * 优先检查会话令牌（x-session-token），然后回退到配置中的 admin_token。
 */
static bool admin_auth_ok(struct evhttp_request *req) {
    const char *token = header_get(req, "x-session-token");
    if (session_valid(token)) return true;
    char *k = config_get_string_copy("admin_token");
    bool ok = auth_ok(req, k);
    free(k);
    return ok;
}

/**
 * @brief 网关级认证检查
 * @param req 当前 HTTP 请求
 * @return 认证通过返回 true
 *
 * 从配置中读取 gateway_api_key，调用 auth_ok 进行比对。
 * 若认证失败，记录 WARN 日志，包含客户端提供的 authorization 和 x-api-key
 * （注意：生产环境中应谨慎记录完整密钥，此处仅用于调试）。
 */
static bool gateway_auth_ok(struct evhttp_request *req) {
    char *k = config_get_string_copy("gateway_api_key");
    bool ok = auth_ok(req, k);
    if (!ok) {
        const char *auth_hdr = header_get(req, "authorization");
        const char *xkey = header_get(req, "x-api-key");
        log_msg("WARN", "gateway auth failed: auth_header=%s x-api-key=%s required_key=%s",
            auth_hdr ? auth_hdr : "(none)", xkey ? xkey : "(none)", k ? k : "(empty)");
    }
    free(k);
    return ok;
}

/**
 * @brief 读取 HTTP 请求体到以 \0 结尾的 C 字符串
 * @param req      libevent HTTP 请求对象
 * @param max_bytes 最大允许长度（超过则返回 NULL）
 * @return 动态分配的字符串指针，调用者负责 free；出错返回 NULL
 *
 * 使用 calloc 分配 len+1 字节，确保末尾有 \0，方便后续字符串处理。
 */
static char *read_request_body(struct evhttp_request *req, size_t max_bytes) {
    struct evbuffer *in = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in);
    if (len > max_bytes) return NULL;
    char *body = (char *)calloc(1, len + 1);
    if (!body) return NULL;
    evbuffer_remove(in, body, len);
    body[len] = 0;
    return body;
}

/**
 * @brief 发送 JSON 响应
 * @param req  HTTP 请求对象
 * @param code HTTP 状态码
 * @param obj  cJSON 对象（会被格式化为紧凑 JSON）
 *
 * 自动设置 Content-Type: application/json; charset=utf-8，
 * 状态短语为 "OK"（200）或 "Error"（非 200）。
 */
static void send_json(struct evhttp_request *req, int code, cJSON *obj) {
    char *txt = cJSON_PrintUnformatted(obj);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", txt ? txt : "{}");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", out);
    evbuffer_free(out);
    free(txt);
}

/**
 * @brief 发送纯文本响应
 * @param req  HTTP 请求对象
 * @param code HTTP 状态码
 * @param ct   Content-Type（NULL 则默认 text/plain; charset=utf-8）
 * @param body 响应正文（NULL 则发送空字符串）
 */
static void send_text(struct evhttp_request *req, int code, const char *ct, const char *body) {
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", body ? body : "");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", ct ? ct : "text/plain; charset=utf-8");
    evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", out);
    evbuffer_free(out);
}

/**
 * @brief 发送标准错误 JSON 响应
 * @param req  HTTP 请求对象
 * @param code HTTP 状态码
 * @param type 错误类型字符串（如 authentication_error）
 * @param msg  人类可读的错误信息
 *
 * 构造 { "error": { "type": ..., "message": ... } } 结构并发送。
 */
static void send_error_json(struct evhttp_request *req, int code, const char *type, const char *msg) {
    cJSON *j = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "type", type ? type : "api_error");
    cJSON_AddStringToObject(err, "message", msg ? msg : "error");
    cJSON_AddItemToObject(j, "error", err);
    send_json(req, code, j);
    cJSON_Delete(j);
}

/**
 * @brief 从 JSON 对象估算 token 数量
 * @param root cJSON 对象
 * @return 估算的 token 数（至少为 1）
 *
 * 本实现采用极度简化的启发式规则：将 JSON 序列化后的字符串长度除以 4 再加 1。
 * 这不是真正的 tokenizer，仅用于 /v1/messages/count_tokens 的本地快速估算。
 */
static long estimate_tokens_from_json(cJSON *root) {
    char *txt = cJSON_PrintUnformatted(root);
    long n = txt ? (long)strlen(txt) : 0;
    free(txt);
    long tok = n / 4 + 1;
    return tok < 1 ? 1 : tok;
}

/**
 * @brief 健康检查接口
 * @param req HTTP 请求对象
 *
 * 返回 { "status": "ok" }，用于 Kubernetes / Docker / 负载均衡器的存活探测。
 */
static void handle_health(struct evhttp_request *req) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    send_json(req, 200, j);
    cJSON_Delete(j);
}

/**
 * @brief 核心消息处理接口（/v1/messages）
 * @param req HTTP 请求对象
 *
 * 这是网关最重要的接口，负责接收 Anthropic Messages API 请求，
 * 转换为 OpenAI Chat Completions 格式，并投递到 worker 线程池。
 *
 * 处理流程：
 *   1. 方法校验：仅接受 POST；
 *   2. 网关认证：调用 gateway_auth_ok；
 *   3. 读取并解析请求体（最大 MAX_BODY_BYTES = 64MB）；
 *   4. 提取 model 字段，通过 config_select_model_copy 选择目标上游模型；
 *   5. 调用 build_openai_request 将 Anthropic JSON 转换为 OpenAI JSON；
 *   6. 提取 stream、api_key、provider、upstream_model 等元数据；
 *   7. 构造 gateway_job_t，填充所有字段，初始化 upstream_body 缓冲区和 send_mu；
 *   8. evhttp_request_own(req) 接管请求所有权，防止 libevent 在当前函数返回时自动释放；
 *   9. enqueue_job(job) 将任务投递到工作线程池；
 *  10. 记录请求日志与实时打印（rt_print）。
 *
 * 此后 worker 线程负责上游网络 I/O，主线程继续处理其他 HTTP 请求。
 */
static void handle_messages(struct evhttp_request *req) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        log_msg("WARN", "messages non-POST method");
        send_error_json(req, 405, "method_not_allowed", "POST required");
        return;
    }
    if (!gateway_auth_ok(req)) {
        log_msg("WARN", "messages auth failed");
        send_error_json(req, 401, "authentication_error", "invalid gateway api key");
        return;
    }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    if (!body) { log_msg("WARN", "messages body too large"); send_error_json(req, 413, "request_too_large", "request body too large"); return; }
    cJSON *anth = cJSON_Parse(body);
    if (!anth) { log_msg("WARN", "messages invalid JSON body"); free(body); send_error_json(req, 400, "invalid_request_error", "invalid JSON"); return; }
    const char *requested_model = json_get_str(anth, "model");
    cJSON *model = config_select_model_copy(requested_model);
    if (!model) {
        log_msg("WARN", "no enabled model found for requested_model=%s", requested_model ? requested_model : "(null)");
        cJSON_Delete(anth); free(body); send_error_json(req, 503, "configuration_error", "no enabled model configured"); return;
    }
    cJSON *oai = build_openai_request(anth, model);
    char *oai_body = cJSON_PrintUnformatted(oai);
    bool stream = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(anth, "stream"));
    const char *client_model = requested_model && *requested_model ? requested_model : json_get_str(model, "id");
    const char *upstream_model = json_get_str(model, "upstream_model");
    const char *api_key = json_get_str(model, "api_key");
    const char *provider = json_get_str(model, "provider");
    char *url = make_upstream_url(model);

    log_msg("INFO", "RECV model=%s body_len=%zu stream=%d", client_model, body ? strlen(body) : 0, stream ? 1 : 0);
    if (body) {
        size_t bl = strlen(body);
        if (bl > 4096) bl = 4096;
        log_msg("DEBUG", "REQ_BODY %.*s", (int)bl, body);
    }

    gateway_job_t *job = (gateway_job_t *)calloc(1, sizeof(*job));
    membuf_init(&job->upstream_body);
    pthread_mutex_init(&job->send_mu, NULL);
    job->client_req = req;
    job->request_body = oai_body;
    job->upstream_url = url;
    job->api_key = xstrdup(api_key ? api_key : "");
    job->provider_name = xstrdup(provider ? provider : "openai-compatible");
    job->client_model = xstrdup(client_model ? client_model : "claude-code-gateway");
    job->upstream_model = xstrdup(upstream_model ? upstream_model : "model");
    job->stream = stream;
    job->stream_state.client_model = xstrdup(job->client_model);
    clock_gettime(CLOCK_MONOTONIC, &job->start_time);
    stats_request_begin(job->upstream_model, job->provider_name, stream, body ? strlen(body) : 0);
    evhttp_request_own(req);
    enqueue_job(job);

    if (body) {
        if (rt_get_mode() != RT_TXT) {
            rt_print("[REQ] model=%s provider=%s stream=%d body_len=%zu",
                client_model, provider ? provider : "openai-compatible", stream ? 1 : 0, strlen(body));
            rt_print_json("[REQ]", body);
        }
    } else {
        rt_print("[REQ] model=%s provider=%s stream=%d body_len=0",
            client_model, provider ? provider : "openai-compatible", stream ? 1 : 0);
    }

    log_msg("INFO", "SEND model=%s upstream_model=%s provider=%s stream=%d url=%s oai_body_len=%zu",
        job->client_model, job->upstream_model, job->provider_name, stream ? 1 : 0, job->upstream_url, oai_body ? strlen(oai_body) : 0);
    if (oai_body) {
        size_t ol = strlen(oai_body);
        if (ol > 4096) ol = 4096;
        log_msg("DEBUG", "UP_REQ %.*s", (int)ol, oai_body);
        if (rt_get_mode() == RT_TXT) {
            rt_print("[UP_REQ] model=%s oai_body_len=%zu\n%s", client_model, strlen(oai_body), oai_body);
        } else {
            rt_print("[UP_REQ] model=%s oai_body_len=%zu", client_model, strlen(oai_body));
            rt_print_json("[UP_REQ]", oai_body);
        }
    }
    cJSON_Delete(model);
    cJSON_Delete(oai);
    cJSON_Delete(anth);
    free(body);
}

/**
 * @brief 模型列表接口（/v1/models）
 * @param req HTTP 请求对象
 *
 * 读取配置中所有 enabled=true 的模型，构造 Anthropic 格式的模型列表响应：
 *   { "object": "list", "data": [ { "id": ..., "type": "model", "object": "model", ... } ] }
 * 如果模型配置了 display_name 或 provider，也会一并返回。
 */
static void handle_models(struct evhttp_request *req) {
    if (!gateway_auth_ok(req)) { log_msg("WARN", "models auth failed"); send_error_json(req, 401, "authentication_error", "invalid gateway api key"); return; }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "object", "list");
    cJSON *data = cJSON_CreateArray();
    char *models_json = config_masked_json();
    cJSON *root = cJSON_Parse(models_json ? models_json : "{}");
    free(models_json);
    cJSON *models = root ? cJSON_GetObjectItemCaseSensitive(root, "models") : NULL;
    if (cJSON_IsArray(models)) {
        cJSON *m;
        cJSON_ArrayForEach(m, models) {
            if (!json_get_bool(m, "enabled", true)) continue;
            const char *id = json_get_str(m, "id");
            if (!id) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", id);
            cJSON_AddStringToObject(item, "type", "model");
            cJSON_AddStringToObject(item, "object", "model");
            const char *dn = json_get_str(m, "display_name");
            if (dn) cJSON_AddStringToObject(item, "display_name", dn);
            const char *provider = json_get_str(m, "provider");
            if (provider) cJSON_AddStringToObject(item, "provider", provider);
            cJSON_AddItemToArray(data, item);
        }
    }
    cJSON_Delete(root);
    cJSON_AddItemToObject(out, "data", data);
    send_json(req, 200, out);
    cJSON_Delete(out);
}

/**
 * @brief Token 计数接口（/v1/messages/count_tokens）
 * @param req HTTP 请求对象
 *
 * 本地快速估算输入 token 数，不调用上游服务。
 * 实现原理见 estimate_tokens_from_json：将请求 JSON 序列化后按长度除以 4 估算。
 * 仅支持 POST 方法，需要网关认证。
 */
static void handle_count_tokens(struct evhttp_request *req) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) { send_error_json(req, 405, "method_not_allowed", "POST required"); return; }
    if (!gateway_auth_ok(req)) { log_msg("WARN", "count_tokens auth failed"); send_error_json(req, 401, "authentication_error", "invalid gateway api key"); return; }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    if (!body) { send_error_json(req, 413, "request_too_large", "request body too large"); return; }
    cJSON *j = cJSON_Parse(body);
    if (!j) { free(body); send_error_json(req, 400, "invalid_request_error", "invalid JSON"); return; }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "input_tokens", estimate_tokens_from_json(j));
    send_json(req, 200, out);
    cJSON_Delete(out);
    cJSON_Delete(j);
    free(body);
}

static void handle_admin_login(struct evhttp_request *req) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_error_json(req, 405, "method_not_allowed", "POST required");
        return;
    }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *password = j ? json_get_str(j, "password") : NULL;
    char *expected = config_get_string_copy("admin_password");
    bool ok = false;
    if (password && expected && constant_time_eq(password, expected)) {
        ok = true;
    }
    free(expected);
    if (ok) {
        char *token = session_create();
        cJSON *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "token", token ? token : "");
        send_json(req, 200, out);
        cJSON_Delete(out);
        free(token);
    } else {
        send_error_json(req, 401, "authentication_error", "invalid password");
    }
    cJSON_Delete(j); free(body);
}

static void handle_admin_logout(struct evhttp_request *req) {
    const char *token = header_get(req, "x-session-token");
    session_destroy(token);
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    send_json(req, 200, ok);
    cJSON_Delete(ok);
}

/**
 * @brief 获取当前配置（/admin/api/config GET）
 * @param req HTTP 请求对象
 *
 * 返回 config_masked_json() 的结果，其中 api_key 等敏感字段已被掩码为 ***MASKED***。
 */
static void handle_config_get(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    char *txt = config_masked_json();
    send_text(req, 200, "application/json; charset=utf-8", txt ? txt : "{}");
    free(txt);
}

/**
 * @brief 热更新配置（/admin/api/config PUT/POST）
 * @param req HTTP 请求对象
 *
 * 读取请求体作为完整配置 JSON，调用 config_replace_from_json 替换内存中的配置。
 * 若解析失败，返回 400 及错误详情；成功返回 { "ok": true }。
 */
static void handle_config_put(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    if (!body) { send_error_json(req, 413, "request_too_large", "request body too large"); return; }
    char *err = NULL;
    int rc = config_replace_from_json(body, &err);
    free(body);
    if (rc != 0) { send_error_json(req, 400, "configuration_error", err ? err : "failed"); free(err); return; }
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    send_json(req, 200, ok);
    cJSON_Delete(ok);
}

/**
 * @brief 切换默认活跃模型（/admin/api/switch）
 * @param req HTTP 请求对象
 *
 * 从 JSON 体中提取 active_model，调用 config_set_active_model 修改配置。
 * 用于在运行时快速切换默认使用的上游模型，无需重启进程。
 */
static void handle_switch(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *active = j ? json_get_str(j, "active_model") : NULL;
    char *err = NULL;
    int rc = config_set_active_model(active, &err);
    if (rc != 0) send_error_json(req, 400, "configuration_error", err ? err : "failed");
    else { cJSON *ok = cJSON_CreateObject(); cJSON_AddBoolToObject(ok, "ok", true); send_json(req, 200, ok); cJSON_Delete(ok); }
    free(err); cJSON_Delete(j); free(body);
}

/**
 * @brief 获取网关统计信息（/admin/api/stats GET）
 * @param req HTTP 请求对象
 *
 * 返回 JSON 格式的完整统计信息，包含全局概览、模型统计和时间窗口。
 * 需要管理员认证。
 */
static void handle_stats(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    cJSON *stats = stats_get_json();
    if (!stats) { send_error_json(req, 500, "internal_error", "failed to get stats"); return; }
    char *json = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "{}");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 重置网关统计（/admin/api/stats/reset POST）
 * @param req HTTP 请求对象
 *
 * 清空所有统计数据，重置启动时间。
 * 需要管理员认证。
 */
static void handle_stats_reset(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) { send_error_json(req, 405, "method_not_allowed", "POST required"); return; }
    stats_reset();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    send_json(req, 200, ok);
    cJSON_Delete(ok);
}

/**
 * @brief 返回管理后台 HTML 页面
 * @param req HTTP 请求对象
 *
 * 从 ./web/admin.html 读取单文件 HTML/JS 管理界面。
 * 若文件不存在，返回简单的占位提示。
 * 输出头包含强缓存禁用指令（Cache-Control: no-cache 等）。
 */
static void handle_admin_html(struct evhttp_request *req) {
    size_t n = 0;
    char *html = NULL;
    FILE *f = fopen("./web/admin.html", "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            n = (size_t)ftell(f);
            rewind(f);
            html = (char *)calloc(1, n + 1);
        size_t fread_n = 0;
        if (html && n > 0) { fread_n = fread(html, 1, n, f); (void)fread_n; }
        }
        fclose(f);
    }
    if (!html) html = xstrdup("<html><body><h1>admin.html not found</h1></body></html>");
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", html ? html : "");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "text/html; charset=utf-8");
    evhttp_add_header(h, "Cache-Control", "no-cache, no-store, must-revalidate");
    evhttp_add_header(h, "Pragma", "no-cache");
    evhttp_add_header(h, "Expires", "0");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(html);
}

/**
 * @brief 顶层 HTTP 请求回调与路由分发
 * @param req libevent HTTP 请求对象
 * @param arg 用户参数（当前未使用）
 *
 * 所有到达本网关的 HTTP 请求都会先进入此函数。
 * 处理步骤：
 *   1. 记录请求方法和 URI；
 *   2. 截断查询字符串（? 之后的内容不参与路由）；
 *   3. 使用 URI_IS 宏进行前缀精确匹配，分发到对应处理函数；
 *   4. 未匹配的 URI 返回 404 JSON 错误。
 *
 * 路由表：
 *   - /                              -> handle_admin_html (管理后台)
 *   - /admin、/admin/                -> handle_admin_html
 *   - /healthz、/readyz              -> handle_health
 *   - /v1/messages                   -> handle_messages
 *   - /v1/messages/count_tokens      -> handle_count_tokens
 *   - /v1/models、/v1/models/        -> handle_models
 *   - /admin/api/config (GET)        -> handle_config_get
 *   - /admin/api/config (PUT/POST)   -> handle_config_put
 *   - /admin/api/switch              -> handle_switch
 */
void handle_root(struct evhttp_request *req, void *arg) {
    (void)arg;
    const char *uri = evhttp_request_get_uri(req);
    const char *meth = evhttp_request_get_command(req) == EVHTTP_REQ_GET ? "GET" :
                       evhttp_request_get_command(req) == EVHTTP_REQ_POST ? "POST" :
                       evhttp_request_get_command(req) == EVHTTP_REQ_PUT ? "PUT" : "OTHER";
    log_msg("INFO", "REQ %s %s", meth, uri);
    const char *q = strchr(uri, '?');
    size_t ulen = q ? (size_t)(q - uri) : strlen(uri);
    if (ulen < 1) { send_error_json(req, 400, "bad_request", "empty uri"); return; }
    if (ulen == 1 && uri[0] == '/') { handle_admin_html(req); return; }
#define URI_IS(s) (ulen == strlen(s) && memcmp(uri, s, ulen) == 0)
    if (URI_IS("/admin") || URI_IS("/admin/")) { handle_admin_html(req); return; }
    if (URI_IS("/healthz") || URI_IS("/readyz")) { handle_health(req); return; }
    if (URI_IS("/v1/messages")) { handle_messages(req); return; }
    if (URI_IS("/v1/messages/count_tokens")) { handle_count_tokens(req); return; }
    if (URI_IS("/v1/models") || URI_IS("/v1/models/")) { handle_models(req); return; }
    if (URI_IS("/admin/api/login") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_admin_login(req); return; }
    if (URI_IS("/admin/api/logout") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_admin_logout(req); return; }
    if (URI_IS("/admin/api/config") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_config_get(req); return; }
    if (URI_IS("/admin/api/config") && (evhttp_request_get_command(req) == EVHTTP_REQ_PUT || evhttp_request_get_command(req) == EVHTTP_REQ_POST)) { handle_config_put(req); return; }
    if (URI_IS("/admin/api/switch")) { handle_switch(req); return; }
    if (URI_IS("/admin/api/stats") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_stats(req); return; }
    if (URI_IS("/admin/api/stats/reset") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_stats_reset(req); return; }
#undef URI_IS
    send_error_json(req, 404, "not_found", "not found");
}