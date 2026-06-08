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
#include "db.h"
#include "admin_html_embedded.h"
#include "favicon_embedded.h"
#include "log.h"
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>
#include <sys/queue.h>
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

static void log_request_headers(struct evhttp_request *req) {
    struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
    struct evkeyval *header;
    TAILQ_FOREACH(header, headers, next) {
        log_msg("DEBUG", "REQ_HDR %s=%s", header->key, header->value);
    }
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
 * 检查 x-session-token 会话令牌是否有效。
 */
static bool admin_auth_ok(struct evhttp_request *req) {
    const char *token = header_get(req, "x-session-token");
    if (session_valid(token)) return true;
    return false;
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
static const char *http_method_str(struct evhttp_request *req) {
    switch (evhttp_request_get_command(req)) {
        case EVHTTP_REQ_GET:     return "GET";
        case EVHTTP_REQ_POST:    return "POST";
        case EVHTTP_REQ_PUT:     return "PUT";
        case EVHTTP_REQ_DELETE:  return "DELETE";
        case EVHTTP_REQ_HEAD:    return "HEAD";
        case EVHTTP_REQ_OPTIONS: return "OPTIONS";
        case EVHTTP_REQ_PATCH:   return "PATCH";
        default:                 return "OTHER";
    }
}

static void send_error_json(struct evhttp_request *req, int code, const char *type, const char *msg) {
    const char *uri = evhttp_request_get_uri(req);
    const char *meth = http_method_str(req);
    char *client_addr = NULL;
    ev_uint16_t client_port = 0;
    struct evhttp_connection *conn = evhttp_request_get_connection(req);
    if (conn) {
        evhttp_connection_get_peer(conn, &client_addr, &client_port);
    }

    /* 尝试从 input buffer 读取 body 预览（尚未被 read_request_body 消费时） */
    struct evbuffer *in = evhttp_request_get_input_buffer(req);
    size_t in_len = in ? evbuffer_get_length(in) : 0;
    char body_preview[2049] = {0};
    if (in_len > 0) {
        size_t copy_len = in_len > 2048 ? 2048 : in_len;
        evbuffer_copyout(in, body_preview, copy_len);
        body_preview[copy_len] = 0;
    }

    if (body_preview[0]) {
        log_msg("ERROR", "CLIENT_ERROR client=%s:%d method=%s uri=%s code=%d type=%s msg=%s body_preview=%s",
                client_addr ? client_addr : "unknown", (int)client_port,
                meth, uri ? uri : "(null)", code,
                type ? type : "api_error", msg ? msg : "error",
                body_preview);
    } else {
        log_msg("ERROR", "CLIENT_ERROR client=%s:%d method=%s uri=%s code=%d type=%s msg=%s",
                client_addr ? client_addr : "unknown", (int)client_port,
                meth, uri ? uri : "(null)", code,
                type ? type : "api_error", msg ? msg : "error");
    }

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

/**
 * @brief 从模型配置中解析自定义请求头并填充到 job
 * @param job  网关任务对象
 * @param model 模型配置的 cJSON 对象
 *
 * 读取模型配置中的 "headers" 对象字段，将其键值对提取为 http_header_t 数组。
 * 仅处理值为字符串类型的条目。
 */
static void job_parse_extra_headers(gateway_job_t *job, cJSON *model) {
    cJSON *headers = cJSON_GetObjectItemCaseSensitive(model, "headers");
    if (!cJSON_IsObject(headers)) return;
    int count = cJSON_GetArraySize(headers);
    if (count <= 0) return;
    job->extra_headers = (http_header_t *)calloc((size_t)count, sizeof(http_header_t));
    if (!job->extra_headers) return;
    cJSON *item = NULL;
    int i = 0;
    cJSON_ArrayForEach(item, headers) {
        if (cJSON_IsString(item) && item->string) {
            job->extra_headers[i].key = xstrdup(item->string);
            job->extra_headers[i].value = xstrdup(item->valuestring);
            i++;
        }
    }
    job->extra_headers_count = (size_t)i;
}

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
    log_request_headers(req);
    char *body = read_request_body(req, MAX_BODY_BYTES);
    if (!body) { log_msg("WARN", "messages body too large"); send_error_json(req, 413, "request_too_large", "request body too large"); return; }
    cJSON *anth = cJSON_Parse(body);
    if (!anth) { log_msg("WARN", "messages invalid JSON body: %s", body); free(body); send_error_json(req, 400, "invalid_request_error", "invalid JSON"); return; }
    const char *requested_model = json_get_str(anth, "model");
    cJSON *model = config_select_model_copy(requested_model);
    if (!model) {
        log_msg("WARN", "no enabled model found for requested_model=%s body=%s", requested_model ? requested_model : "(null)", body);
        cJSON_Delete(anth); free(body); send_error_json(req, 503, "configuration_error", "no enabled model configured"); return;
    }
    const char *api_mode = json_get_str(model, "api_mode");
    passthrough_mode_t passthrough = (api_mode && strcmp(api_mode, "passthrough") == 0)
        ? PT_ANTHROPIC : PT_NONE;

    cJSON *oai = NULL;
    char *upstream_body = NULL;
    char *url = NULL;
    if (passthrough == PT_ANTHROPIC) {
        /* 透传模式：替换 model 为 upstream_model 后直接转发 */
        const char *um = json_get_str(model, "upstream_model");
        if (um && *um) {
            cJSON_ReplaceItemInObjectCaseSensitive(anth, "model", cJSON_CreateString(um));
        }
        upstream_body = cJSON_PrintUnformatted(anth);
        url = make_upstream_url_for_messages(model);
        free(body); body = NULL;
    } else {
        oai = build_openai_request(anth, model);
        upstream_body = cJSON_PrintUnformatted(oai);
        url = make_upstream_url(model);
    }
    bool stream = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(anth, "stream"));
    const char *client_model = requested_model && *requested_model ? requested_model : json_get_str(model, "id");
    const char *upstream_model = json_get_str(model, "upstream_model");
    const char *api_key = json_get_str(model, "api_key");
    const char *provider = json_get_str(model, "provider");

    log_msg("INFO", "RECV model=%s body_len=%zu stream=%d passthrough=%d", client_model, body ? strlen(body) : 0, stream ? 1 : 0, (int)passthrough);
    if (body) {
        size_t bl = strlen(body);
        if (bl > 4096) bl = 4096;
        log_msg("DEBUG", "REQ_BODY %.*s", (int)bl, body);
    }

    gateway_job_t *job = (gateway_job_t *)calloc(1, sizeof(*job));
    membuf_init(&job->upstream_body);
    pthread_mutex_init(&job->send_mu, NULL);
    job->client_req = req;
    job->request_body = upstream_body;
    job->upstream_url = url;
    job->api_key = xstrdup(api_key ? api_key : "");
    job->provider_name = xstrdup(provider ? provider : "openai-compatible");
    const char *ua = header_get(req, "User-Agent");
    job->client_user_agent = xstrdup(ua ? ua : "");
    job->client_model = xstrdup(client_model ? client_model : "claude-code-gateway");
    job->upstream_model = xstrdup(upstream_model ? upstream_model : "model");
    job->stream = stream;
    job->passthrough = passthrough;
    /* 透传模式上游是 Anthropic 协议，input_tokens 不含 cache_read */
    job->prompt_tokens_includes_cache = config_get_prompt_tokens_includes_cache(model, passthrough != PT_ANTHROPIC);
    job_parse_extra_headers(job, model);
    job->spoof_claude_code_headers = json_get_bool(model, "spoof_claude_code_headers", false);
    job->stream_state.client_model = xstrdup(job->client_model);
    /* 不估算 prompt tokens，只认上游返回的真实 usage 值 */
    job->stream_state.prompt_tokens = 0;
    clock_gettime(CLOCK_MONOTONIC, &job->start_time);
    size_t req_body_len = upstream_body ? strlen(upstream_body) : 0;
    stats_request_begin(job->upstream_model, job->provider_name, stream, req_body_len);
    evhttp_request_own(req);
    enqueue_job(job);

    rt_print("[REQ] model=%s provider=%s stream=%d passthrough=%d body_len=%zu",
        client_model, provider ? provider : "openai-compatible", stream ? 1 : 0, (int)passthrough, req_body_len);
    if (body) {
        rt_print_json("[REQ]", body);
    } else if (upstream_body) {
        rt_print_json("[REQ]", upstream_body);
    }

    log_msg("INFO", "SEND model=%s upstream_model=%s provider=%s stream=%d passthrough=%d url=%s upstream_body_len=%zu",
        job->client_model, job->upstream_model, job->provider_name, stream ? 1 : 0, (int)passthrough, job->upstream_url, req_body_len);
    if (upstream_body) {
        size_t ol = strlen(upstream_body);
        if (ol > 4096) ol = 4096;
        log_msg("DEBUG", "UP_REQ %.*s", (int)ol, upstream_body);
        if (rt_get_mode() == RT_TXT) {
            rt_print("[UP_REQ] model=%s upstream_body_len=%zu\n%s", client_model, strlen(upstream_body), upstream_body);
        } else {
            rt_print("[UP_REQ] model=%s upstream_body_len=%zu", client_model, strlen(upstream_body));
            rt_print_json("[UP_REQ]", upstream_body);
        }
    }
    if (oai) cJSON_Delete(oai);
    cJSON_Delete(model);
    cJSON_Delete(anth);
    free(body);
}

/**
 * @brief OpenAI Chat Completions 透传接口（/v1/chat/completions）
 * @param req HTTP 请求对象
 *
 * 客户端按 OpenAI 标准发请求，gateway 不做协议转换、原样转发到上游
 * OpenAI 兼容服务，仅记录监控（usage / 缓存 / 延迟）。
 *
 * 处理流程与 handle_messages 类似，差异点：
 *   - 不读取 model 配置中的 api_mode，无条件按 OpenAI 透传处理；
 *   - 上游 URL 使用 make_upstream_url（base_url + /chat/completions）；
 *   - 替换请求体中的 model 字段为 upstream_model 后转发；
 *   - prompt_tokens_includes_cache 默认 true（与 OpenAI 协议惯例一致）。
 */
static void handle_chat_completions(struct evhttp_request *req) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        log_msg("WARN", "chat/completions non-POST method");
        send_error_json(req, 405, "method_not_allowed", "POST required");
        return;
    }
    if (!gateway_auth_ok(req)) {
        log_msg("WARN", "chat/completions auth failed");
        send_error_json(req, 401, "authentication_error", "invalid gateway api key");
        return;
    }
    log_request_headers(req);
    char *body = read_request_body(req, MAX_BODY_BYTES);
    if (!body) { log_msg("WARN", "chat/completions body too large"); send_error_json(req, 413, "request_too_large", "request body too large"); return; }
    cJSON *oai_req = cJSON_Parse(body);
    if (!oai_req) { log_msg("WARN", "chat/completions invalid JSON body: %s", body); free(body); send_error_json(req, 400, "invalid_request_error", "invalid JSON"); return; }
    const char *requested_model = json_get_str(oai_req, "model");
    cJSON *model = config_select_model_copy(requested_model);
    if (!model) {
        log_msg("WARN", "no enabled model found for requested_model=%s body=%s", requested_model ? requested_model : "(null)", body);
        cJSON_Delete(oai_req); free(body); send_error_json(req, 503, "configuration_error", "no enabled model configured"); return;
    }
    /* OpenAI 透传：替换 model 为 upstream_model 后直接转发 */
    const char *um = json_get_str(model, "upstream_model");
    if (um && *um) {
        cJSON_ReplaceItemInObjectCaseSensitive(oai_req, "model", cJSON_CreateString(um));
    }
    char *upstream_body = cJSON_PrintUnformatted(oai_req);
    char *url = make_upstream_url(model);
    bool stream = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(oai_req, "stream"));
    const char *client_model = requested_model && *requested_model ? requested_model : json_get_str(model, "id");
    const char *upstream_model = json_get_str(model, "upstream_model");
    const char *api_key = json_get_str(model, "api_key");
    const char *provider = json_get_str(model, "provider");

    log_msg("INFO", "RECV model=%s body_len=%zu stream=%d passthrough=%d (openai)",
        client_model, body ? strlen(body) : 0, stream ? 1 : 0, (int)PT_OPENAI);
    {
        size_t bl = body ? strlen(body) : 0;
        if (bl > 4096) bl = 4096;
        if (body) log_msg("DEBUG", "REQ_BODY %.*s", (int)bl, body);
    }

    gateway_job_t *job = (gateway_job_t *)calloc(1, sizeof(*job));
    membuf_init(&job->upstream_body);
    pthread_mutex_init(&job->send_mu, NULL);
    job->client_req = req;
    job->request_body = upstream_body;
    job->upstream_url = url;
    job->api_key = xstrdup(api_key ? api_key : "");
    job->provider_name = xstrdup(provider ? provider : "openai-compatible");
    const char *ua = header_get(req, "User-Agent");
    job->client_user_agent = xstrdup(ua ? ua : "");
    job->client_model = xstrdup(client_model ? client_model : "openai-passthrough");
    job->upstream_model = xstrdup(upstream_model ? upstream_model : "model");
    job->stream = stream;
    job->passthrough = PT_OPENAI;
    /* OpenAI 协议惯例：prompt_tokens 已包含缓存 tokens，可被模型配置覆盖 */
    job->prompt_tokens_includes_cache = config_get_prompt_tokens_includes_cache(model, true);
    job_parse_extra_headers(job, model);
    job->spoof_claude_code_headers = json_get_bool(model, "spoof_claude_code_headers", false);
    job->stream_state.client_model = xstrdup(job->client_model);
    job->stream_state.prompt_tokens = 0;
    clock_gettime(CLOCK_MONOTONIC, &job->start_time);
    size_t req_body_len = upstream_body ? strlen(upstream_body) : 0;
    stats_request_begin(job->upstream_model, job->provider_name, stream, req_body_len);
    evhttp_request_own(req);
    enqueue_job(job);

    rt_print("[REQ] model=%s provider=%s stream=%d passthrough=%d body_len=%zu (openai)",
        client_model, provider ? provider : "openai-compatible", stream ? 1 : 0, (int)PT_OPENAI, req_body_len);
    if (body) {
        rt_print_json("[REQ]", body);
    } else if (upstream_body) {
        rt_print_json("[REQ]", upstream_body);
    }

    log_msg("INFO", "SEND model=%s upstream_model=%s provider=%s stream=%d passthrough=%d url=%s upstream_body_len=%zu",
        job->client_model, job->upstream_model, job->provider_name, stream ? 1 : 0, (int)PT_OPENAI, job->upstream_url, req_body_len);
    if (upstream_body) {
        size_t ol = strlen(upstream_body);
        if (ol > 4096) ol = 4096;
        log_msg("DEBUG", "UP_REQ %.*s", (int)ol, upstream_body);
        if (rt_get_mode() == RT_TXT) {
            rt_print("[UP_REQ] model=%s upstream_body_len=%zu\n%s", client_model, strlen(upstream_body), upstream_body);
        } else {
            rt_print("[UP_REQ] model=%s upstream_body_len=%zu", client_model, strlen(upstream_body));
            rt_print_json("[UP_REQ]", upstream_body);
        }
    }
    cJSON_Delete(model);
    cJSON_Delete(oai_req);
    free(body);
}

/**
 * @brief 模型列表接口（/v1/models）
 * @param req HTTP 请求对象
 *
 * 读取配置中所有 enabled=true 的模型，构造 Anthropic 格式的模型列表响应：
 *   { "object": "list", "data": [ { "id": ..., "type": "model", "object": "model", ... } ] }
 * 如果模型配置了 provider，也会一并返回。
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
    if (!j) { log_msg("WARN", "count_tokens invalid JSON body: %s", body); free(body); send_error_json(req, 400, "invalid_request_error", "invalid JSON"); return; }
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
    if (rc != 0) {
        log_msg("WARN", "config_replace failed: %s body=%s", err ? err : "failed", body);
        free(body);
        send_error_json(req, 400, "configuration_error", err ? err : "failed"); free(err); return;
    }
    free(body);
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
    if (rc != 0) {
        log_msg("WARN", "config_set_active_model failed: %s body=%s", err ? err : "failed", body ? body : "(null)");
        send_error_json(req, 400, "configuration_error", err ? err : "failed");
    } else {
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddBoolToObject(ok, "ok", true);
        send_json(req, 200, ok);
        cJSON_Delete(ok);
    }
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
static void handle_db_reset(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) { send_error_json(req, 405, "method_not_allowed", "POST required"); return; }
    bool ok = db_reset();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    send_json(req, 200, out);
    cJSON_Delete(out);
}

static void handle_stats_reset(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) { send_error_json(req, 405, "method_not_allowed", "POST required"); return; }
    stats_reset();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    send_json(req, 200, ok);
    cJSON_Delete(ok);
}

/* 从 URI 查询字符串中提取指定参数值
 * 返回值写入 out 缓冲区，成功返回 out 指针，失败返回 NULL
 */
static const char *query_get(const char *uri, const char *key, char *out, size_t out_len) {
    const char *q = strchr(uri, '?');
    if (!q) return NULL;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t seg_len = amp ? (size_t)(amp - q) : strlen(q);
        if (seg_len > klen && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            size_t vlen = seg_len - klen - 1;
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, q + klen + 1, vlen);
            out[vlen] = 0;
            /* URL decode：简单处理 + 和 %XX */
            char *p = out;
            while (*p) {
                if (*p == '+') { *p = ' '; }
                else if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = {p[1], p[2], 0};
                    *p = (char)strtol(hex, NULL, 16);
                    memmove(p + 1, p + 3, strlen(p + 3) + 1);
                }
                p++;
            }
            return out;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

/**
 * @brief 请求历史查询（/admin/api/history GET）
 *
 * 查询参数：
 *   - model: 模型名过滤
 *   - from: 起始 Unix 时间戳
 *   - to: 结束 Unix 时间戳
 *   - limit: 最大返回条数（默认 100）
 *   - offset: 偏移量（默认 0）
 */
static void handle_history(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    const char *uri = evhttp_request_get_uri(req);
    char buf_model[128] = {0};
    char buf_from[32] = {0};
    char buf_to[32] = {0};
    char buf_limit[16] = {0};
    char buf_offset[16] = {0};
    const char *model = query_get(uri, "model", buf_model, sizeof(buf_model));
    const char *from_s = query_get(uri, "from", buf_from, sizeof(buf_from));
    const char *to_s = query_get(uri, "to", buf_to, sizeof(buf_to));
    const char *limit_s = query_get(uri, "limit", buf_limit, sizeof(buf_limit));
    const char *offset_s = query_get(uri, "offset", buf_offset, sizeof(buf_offset));

    time_t from_t = from_s ? (time_t)atoll(from_s) : 0;
    time_t to_t = to_s ? (time_t)atoll(to_s) : 0;
    int limit = limit_s ? atoi(limit_s) : 100;
    int offset = offset_s ? atoi(offset_s) : 0;

    cJSON *result = db_query_history(model && *model ? model : NULL, from_t, to_t, limit, offset);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "{}");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 小时聚合统计查询（/admin/api/hourly GET）
 *
 * 查询参数：
 *   - model: 模型名过滤
 *   - from: 起始小时 "YYYY-MM-DD HH:00"
 *   - to: 结束小时 "YYYY-MM-DD HH:00"
 */
static void handle_hourly(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    const char *uri = evhttp_request_get_uri(req);
    char buf_model[128] = {0};
    char buf_from[32] = {0};
    char buf_to[32] = {0};
    const char *model = query_get(uri, "model", buf_model, sizeof(buf_model));
    const char *from_s = query_get(uri, "from", buf_from, sizeof(buf_from));
    const char *to_s = query_get(uri, "to", buf_to, sizeof(buf_to));

    cJSON *arr = db_query_hourly(model && *model ? model : NULL,
                                  from_s && *from_s ? from_s : NULL,
                                  to_s && *to_s ? to_s : NULL);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "[]");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 日聚合统计查询（/admin/api/daily GET）
 *
 * 查询参数：
 *   - model: 模型名过滤
 *   - from: 起始日期 "YYYY-MM-DD"
 *   - to: 结束日期 "YYYY-MM-DD"
 */
static void handle_daily(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    const char *uri = evhttp_request_get_uri(req);
    char buf_model[128] = {0};
    char buf_from[32] = {0};
    char buf_to[32] = {0};
    const char *model = query_get(uri, "model", buf_model, sizeof(buf_model));
    const char *from_s = query_get(uri, "from", buf_from, sizeof(buf_from));
    const char *to_s = query_get(uri, "to", buf_to, sizeof(buf_to));

    cJSON *arr = db_query_daily(model && *model ? model : NULL,
                                 from_s && *from_s ? from_s : NULL,
                                 to_s && *to_s ? to_s : NULL);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "[]");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 实时调试信息（/admin/api/debug GET）
 * @param req HTTP 请求对象
 *
 * 返回所有 worker 的实时状态：
 *   - 每个 worker 的 pending 队列（模型、已等待时间、是否流式）；
 *   - active_jobs：正在 curl 中传输的任务数；
 *   - still_running：libcurl 报告的活动连接数；
 *   - 全局 active_requests（来自 stats 模块）。
 * 需要管理员认证。
 */
static void handle_debug(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    cJSON *root = cJSON_CreateObject();
    cJSON *stats = stats_get_json();
    if (stats) {
        cJSON_AddItemToObject(root, "stats", stats);
    }
    cJSON *workers = workers_get_debug_info();
    cJSON_AddItemToObject(root, "workers", workers);

    /* 计算全局摘要 */
    int total_pending = 0, total_sending = 0, total_waiting = 0, total_receiving = 0;
    cJSON *w;
    cJSON_ArrayForEach(w, workers) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(w, "pending_len");
        if (cJSON_IsNumber(p)) total_pending += (int)p->valuedouble;
        cJSON *s = cJSON_GetObjectItemCaseSensitive(w, "n_sending");
        if (cJSON_IsNumber(s)) total_sending += (int)s->valuedouble;
        cJSON *w2 = cJSON_GetObjectItemCaseSensitive(w, "n_waiting");
        if (cJSON_IsNumber(w2)) total_waiting += (int)w2->valuedouble;
        cJSON *r = cJSON_GetObjectItemCaseSensitive(w, "n_receiving");
        if (cJSON_IsNumber(r)) total_receiving += (int)r->valuedouble;
    }
    cJSON_AddNumberToObject(root, "total_pending", total_pending);
    cJSON_AddNumberToObject(root, "total_sending", total_sending);
    cJSON_AddNumberToObject(root, "total_waiting", total_waiting);
    cJSON_AddNumberToObject(root, "total_receiving", total_receiving);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "{}");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 错误日志查询（/admin/api/error_logs GET）
 *
 * 查询参数同 history：model, from, to, limit, offset
 * 返回包含完整请求体/响应体的错误日志
 */
static void handle_error_logs(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    const char *uri = evhttp_request_get_uri(req);
    char buf_model[128] = {0};
    char buf_from[32] = {0};
    char buf_to[32] = {0};
    char buf_limit[16] = {0};
    char buf_offset[16] = {0};
    const char *model = query_get(uri, "model", buf_model, sizeof(buf_model));
    const char *from_s = query_get(uri, "from", buf_from, sizeof(buf_from));
    const char *to_s = query_get(uri, "to", buf_to, sizeof(buf_to));
    const char *limit_s = query_get(uri, "limit", buf_limit, sizeof(buf_limit));
    const char *offset_s = query_get(uri, "offset", buf_offset, sizeof(buf_offset));

    time_t from_t = from_s ? (time_t)atoll(from_s) : 0;
    time_t to_t = to_s ? (time_t)atoll(to_s) : 0;
    int limit = limit_s ? atoi(limit_s) : 100;
    int offset = offset_s ? atoi(offset_s) : 0;

    cJSON *result = db_query_error_logs(model && *model ? model : NULL, from_t, to_t, limit, offset);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", json ? json : "{}");
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
    free(json);
}

/**
 * @brief 返回管理后台 HTML 页面
 * @param req HTTP 请求对象
 *
 * 返回内嵌在二进制中的单文件 HTML/JS 管理界面。
 * 输出头包含强缓存禁用指令（Cache-Control: no-cache 等）。
 */
/**
 * @brief 修改管理员密码（/admin/api/change-password POST）
 * @param req HTTP 请求对象
 *
 * 需提供 {old_password, new_password}，验证旧密码后更新配置并持久化。
 * 空旧密码时允许直接设置新密码（首次设置场景）。
 */
static void handle_admin_change_password(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *old_pwd = j ? json_get_str(j, "old_password") : NULL;
    const char *new_pwd = j ? json_get_str(j, "new_password") : NULL;
    if (!new_pwd || strlen(new_pwd) < 4) {
        send_error_json(req, 400, "bad_request", "new_password must be at least 4 characters");
        cJSON_Delete(j); free(body); return;
    }
    char *expected = config_get_string_copy("admin_password");
    bool ok = false;
    if (expected && old_pwd && constant_time_eq(expected, old_pwd)) ok = true;
    if (!expected || expected[0] == '\0') ok = true; /* empty current password: allow any */
    free(expected);
    if (!ok) {
        send_error_json(req, 401, "authentication_error", "old password incorrect");
        cJSON_Delete(j); free(body); return;
    }
    char *err = NULL;
    int rc = config_set_string("admin_password", new_pwd, &err);
    if (rc != 0) {
        send_error_json(req, 500, "internal_error", err ? err : "failed to persist password");
        free(err);
    } else {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        send_json(req, 200, out);
        cJSON_Delete(out);
    }
    cJSON_Delete(j); free(body);
}
/**
 * @brief 处理 /favicon.ico 请求
 * @param req HTTP 请求对象
 *
 * 返回嵌入的 favicon PNG 图像。
 */
static void handle_favicon(struct evhttp_request *req) {
    struct evbuffer *out = evbuffer_new();
    evbuffer_add(out, EMBEDDED_FAVICON, EMBEDDED_FAVICON_LEN);
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "image/x-icon");
    evhttp_add_header(h, "Cache-Control", "public, max-age=86400");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
}


static void handle_admin_html(struct evhttp_request *req) {
    struct evbuffer *out = evbuffer_new();
    evbuffer_add(out, EMBEDDED_ADMIN_HTML, EMBEDDED_ADMIN_HTML_LEN);
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "text/html; charset=utf-8");
    evhttp_add_header(h, "Cache-Control", "no-cache, no-store, must-revalidate");
    evhttp_add_header(h, "Pragma", "no-cache");
    evhttp_add_header(h, "Expires", "0");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
}

/**
 * @brief 提供 CA 证书下载（/admin/ca.pem）
 */
static void handle_ca_cert_download(struct evhttp_request *req) {
    if (!g_ca_cert_pem || g_ca_cert_pem_len <= 0) {
        send_error_json(req, 404, "not_found", "CA certificate not available");
        return;
    }
    struct evbuffer *out = evbuffer_new();
    evbuffer_add(out, g_ca_cert_pem, g_ca_cert_pem_len);
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/x-pem-file");
    evhttp_add_header(h, "Content-Disposition", "attachment; filename=\"ca-cert.pem\"");
    evhttp_add_header(h, "Cache-Control", "public, max-age=3600");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
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
 *   - /v1/chat/completions           -> handle_chat_completions (OpenAI 透传，仅监控)
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
    if (URI_IS("/favicon.ico")) { handle_favicon(req); return; }
    if (URI_IS("/admin") || URI_IS("/admin/")) { handle_admin_html(req); return; }
    if (URI_IS("/admin/ca.pem") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_ca_cert_download(req); return; }
    if (URI_IS("/healthz") || URI_IS("/readyz")) { handle_health(req); return; }
    if (URI_IS("/v1/messages")) { handle_messages(req); return; }
    if (URI_IS("/v1/chat/completions")) { handle_chat_completions(req); return; }
    if (URI_IS("/chat/completions")) { handle_chat_completions(req); return; }
    if (URI_IS("/v1/messages/count_tokens")) { handle_count_tokens(req); return; }
    if (URI_IS("/v1/models") || URI_IS("/v1/models/")) { handle_models(req); return; }
    if (URI_IS("/admin/api/login") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_admin_login(req); return; }
    if (URI_IS("/admin/api/logout") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_admin_logout(req); return; }
    if (URI_IS("/admin/api/config") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_config_get(req); return; }
    if (URI_IS("/admin/api/config") && (evhttp_request_get_command(req) == EVHTTP_REQ_PUT || evhttp_request_get_command(req) == EVHTTP_REQ_POST)) { handle_config_put(req); return; }
    if (URI_IS("/admin/api/switch")) { handle_switch(req); return; }
    if (URI_IS("/admin/api/stats") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_stats(req); return; }
    if (URI_IS("/admin/api/stats/reset") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_stats_reset(req); return; }
    if (URI_IS("/admin/api/db/reset") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_db_reset(req); return; }
    if (URI_IS("/admin/api/history") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_history(req); return; }
    if (URI_IS("/admin/api/hourly") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_hourly(req); return; }
    if (URI_IS("/admin/api/daily") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_daily(req); return; }
    if (URI_IS("/admin/api/debug") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_debug(req); return; }
    if (URI_IS("/admin/api/error_logs") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_error_logs(req); return; }
    if (URI_IS("/admin/api/change-password") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_admin_change_password(req); return; }
#undef URI_IS
    send_error_json(req, 404, "not_found", "not found");
}
