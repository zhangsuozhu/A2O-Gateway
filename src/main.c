#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/thread.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_CONFIG_PATH "./config/gateway.json"
#define MAX_TOOL_STREAMS 64
#define MAX_WORKERS 8
#define MAX_BODY_BYTES (64 * 1024 * 1024)
#define MASKED_KEY "***MASKED***"

typedef struct membuf {
    char *ptr;
    size_t len;
    size_t cap;
} membuf_t;

typedef struct app_config {
    pthread_rwlock_t lock;
    cJSON *root;
    char path[PATH_MAX];
} app_config_t;

typedef struct tool_stream_state {
    int openai_index;
    int block_index;
    bool started;
    char *id;
    char *name;
} tool_stream_state_t;

typedef struct stream_state {
    bool reply_started;
    bool message_started;
    bool text_started;
    bool ended;
    int text_block_index;
    int next_block_index;
    char *linebuf;
    size_t linebuf_len;
    size_t linebuf_cap;
    char *message_id;
    char *client_model;
    char *finish_reason;
    long prompt_tokens;
    long completion_tokens;
    tool_stream_state_t tools[MAX_TOOL_STREAMS];
} stream_state_t;

typedef struct gateway_job gateway_job_t;

typedef struct worker {
    pthread_t tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    CURLM *multi;
    gateway_job_t *pending_head;
    gateway_job_t *pending_tail;
    int still_running;
    bool stop;
    int id;
} worker_t;

struct gateway_job {
    struct evhttp_request *client_req;
    char *request_body;
    char *upstream_url;
    char *api_key;
    char *provider_name;
    char *client_model;
    char *upstream_model;
    bool stream;
    bool upstream_headers_done;
    bool response_sent;
    long upstream_status;
    membuf_t upstream_body;
    stream_state_t stream_state;
    CURL *easy;
    struct curl_slist *headers;
    worker_t *worker;
    gateway_job_t *next;
};

static app_config_t G;
static struct event_base *BASE = NULL;
static struct evhttp *HTTP = NULL;
static worker_t WORKERS[MAX_WORKERS];
static int WORKER_COUNT = 4;
static unsigned long RR = 0;
static volatile sig_atomic_t STOP = 0;

static void log_msg(const char *level, const char *fmt, ...) {
    char ts[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    fprintf(stderr, "[%s] %-5s ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { perror("strdup"); abort(); }
    return p;
}

static char *trim_slashes(const char *s) {
    if (!s) return xstrdup("");
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') n--;
    char *out = (char *)calloc(1, n + 1);
    if (!out) abort();
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static void membuf_init(membuf_t *b) {
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

static void membuf_free(membuf_t *b) {
    free(b->ptr);
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

static int membuf_append(membuf_t *b, const char *data, size_t n) {
    if (!data || n == 0) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = (char *)realloc(b->ptr, nc);
        if (!p) return -1;
        b->ptr = p;
        b->cap = nc;
    }
    memcpy(b->ptr + b->len, data, n);
    b->len += n;
    b->ptr[b->len] = 0;
    return 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}

static int write_file_atomic(const char *path, const char *data) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, path);
}

static const char *json_get_str(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool json_get_bool(cJSON *o, const char *key, bool defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return defv;
}

static long json_get_long(cJSON *o, const char *key, long defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return (long)v->valuedouble;
    return defv;
}

static void json_add_dup(cJSON *o, const char *key, cJSON *v) {
    if (!v) return;
    cJSON_AddItemToObject(o, key, cJSON_Duplicate(v, 1));
}

static char *json_print(cJSON *j) {
    return cJSON_PrintUnformatted(j);
}

static cJSON *default_config(void) {
    const char *s =
        "{"
        "\"listen_host\":\"0.0.0.0\","
        "\"listen_port\":8080,"
        "\"gateway_api_key\":\"cc-local-token\","
        "\"admin_token\":\"admin-local-token\","
        "\"worker_threads\":4,"
        "\"active_model\":\"qwen-coder\","
        "\"models\":[{"
          "\"id\":\"qwen-coder\","
          "\"provider\":\"aliyun-bailian\","
          "\"display_name\":\"Qwen Coder via OpenAI Compatible\","
          "\"interface\":\"openai_chat_completions\","
          "\"base_url\":\"https://dashscope.aliyuncs.com/compatible-mode/v1\","
          "\"endpoint\":\"\","
          "\"api_key\":\"YOUR_OPENAI_COMPATIBLE_API_KEY\","
          "\"upstream_model\":\"qwen3-coder-plus\","
          "\"enabled\":true,"
          "\"params\":{\"temperature\":0.2,\"top_p\":0.95},"
          "\"extra_body\":{}"
        "}]"
        "}";
    return cJSON_Parse(s);
}

static cJSON *mask_config(cJSON *root) {
    cJSON *copy = cJSON_Duplicate(root, 1);
    cJSON *models = cJSON_GetObjectItemCaseSensitive(copy, "models");
    if (cJSON_IsArray(models)) {
        cJSON *m;
        cJSON_ArrayForEach(m, models) {
            cJSON *k = cJSON_GetObjectItemCaseSensitive(m, "api_key");
            if (cJSON_IsString(k) && k->valuestring && strlen(k->valuestring) > 0) {
                cJSON_DeleteItemFromObject(m, "api_key");
                cJSON_AddStringToObject(m, "api_key", MASKED_KEY);
            }
        }
    }
    return copy;
}

static cJSON *find_model_by_id(cJSON *root, const char *id) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(models) || !id) return NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *mid = json_get_str(m, "id");
        if (mid && strcmp(mid, id) == 0) return m;
    }
    return NULL;
}

static void preserve_masked_keys(cJSON *incoming, cJSON *oldroot) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(incoming, "models");
    if (!cJSON_IsArray(models) || !oldroot) return;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *id = json_get_str(m, "id");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(m, "api_key");
        if (!id || !cJSON_IsString(k) || strcmp(k->valuestring, MASKED_KEY) != 0) continue;
        cJSON *oldm = find_model_by_id(oldroot, id);
        const char *oldk = oldm ? json_get_str(oldm, "api_key") : NULL;
        if (oldk) {
            cJSON_DeleteItemFromObject(m, "api_key");
            cJSON_AddStringToObject(m, "api_key", oldk);
        }
    }
}

static int config_load(const char *path) {
    pthread_rwlock_init(&G.lock, NULL);
    snprintf(G.path, sizeof(G.path), "%s", path ? path : DEFAULT_CONFIG_PATH);
    size_t n = 0;
    char *txt = read_file(G.path, &n);
    cJSON *root = NULL;
    if (txt) root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        root = default_config();
        char *p = cJSON_Print(root);
        write_file_atomic(G.path, p);
        free(p);
        log_msg("WARN", "created default config: %s", G.path);
    }
    G.root = root;
    WORKER_COUNT = (int)json_get_long(root, "worker_threads", 4);
    if (WORKER_COUNT < 1) WORKER_COUNT = 1;
    if (WORKER_COUNT > MAX_WORKERS) WORKER_COUNT = MAX_WORKERS;
    return 0;
}

static char *config_masked_json(void) {
    pthread_rwlock_rdlock(&G.lock);
    cJSON *copy = cJSON_Duplicate(G.root, 1);
    pthread_rwlock_unlock(&G.lock);
    char *out = cJSON_Print(copy);
    cJSON_Delete(copy);
    return out;
}

static int config_replace_from_json(const char *body, char **err) {
    cJSON *newroot = cJSON_Parse(body);
    if (!newroot) {
        if (err) *err = xstrdup("invalid JSON");
        return -1;
    }
    cJSON *models = cJSON_GetObjectItemCaseSensitive(newroot, "models");
    if (!cJSON_IsArray(models) || cJSON_GetArraySize(models) == 0) {
        cJSON_Delete(newroot);
        if (err) *err = xstrdup("models must be a non-empty array");
        return -1;
    }
    pthread_rwlock_wrlock(&G.lock);
    preserve_masked_keys(newroot, G.root);
    char *txt = cJSON_Print(newroot);
    if (write_file_atomic(G.path, txt) != 0) {
        pthread_rwlock_unlock(&G.lock);
        cJSON_Delete(newroot);
        free(txt);
        if (err) *err = xstrdup("failed to persist config");
        return -1;
    }
    cJSON_Delete(G.root);
    G.root = newroot;
    WORKER_COUNT = (int)json_get_long(G.root, "worker_threads", WORKER_COUNT);
    if (WORKER_COUNT < 1) WORKER_COUNT = 1;
    if (WORKER_COUNT > MAX_WORKERS) WORKER_COUNT = MAX_WORKERS;
    pthread_rwlock_unlock(&G.lock);
    free(txt);
    return 0;
}

static cJSON *config_select_model_copy(const char *requested_model) {
    pthread_rwlock_rdlock(&G.lock);
    cJSON *m = NULL;
    if (requested_model && *requested_model) {
        m = find_model_by_id(G.root, requested_model);
        if (!m || !json_get_bool(m, "enabled", true)) {
            log_msg("WARN", "model '%s' not found or disabled, checking active_model", requested_model);
            const char *active = json_get_str(G.root, "active_model");
            if (active) m = find_model_by_id(G.root, active);
            else m = NULL;
        }
    } else {
        const char *active = json_get_str(G.root, "active_model");
        if (active) m = find_model_by_id(G.root, active);
    }
    if (!m || !json_get_bool(m, "enabled", true)) {
        cJSON *models = cJSON_GetObjectItemCaseSensitive(G.root, "models");
        cJSON *it;
        cJSON_ArrayForEach(it, models) {
            if (json_get_bool(it, "enabled", true)) { m = it; break; }
        }
    }
    cJSON *copy = m ? cJSON_Duplicate(m, 1) : NULL;
    pthread_rwlock_unlock(&G.lock);
    return copy;
}

static char *config_get_string_copy(const char *key) {
    pthread_rwlock_rdlock(&G.lock);
    const char *v = json_get_str(G.root, key);
    char *out = v ? xstrdup(v) : NULL;
    pthread_rwlock_unlock(&G.lock);
    return out;
}

static int config_set_active_model(const char *id, char **err) {
    if (!id || !*id) { if (err) *err = xstrdup("missing active_model"); return -1; }
    pthread_rwlock_wrlock(&G.lock);
    cJSON *m = find_model_by_id(G.root, id);
    if (!m) {
        pthread_rwlock_unlock(&G.lock);
        if (err) *err = xstrdup("model id not found");
        return -1;
    }
    cJSON *active = cJSON_GetObjectItemCaseSensitive(G.root, "active_model");
    if (cJSON_IsString(active)) {
        cJSON_DeleteItemFromObject(G.root, "active_model");
    }
    cJSON_AddStringToObject(G.root, "active_model", id);
    char *txt = cJSON_Print(G.root);
    int rc = write_file_atomic(G.path, txt);
    free(txt);
    pthread_rwlock_unlock(&G.lock);
    if (rc != 0 && err) *err = xstrdup("failed to persist active_model");
    return rc;
}

static const char *header_get(struct evhttp_request *req, const char *name) {
    return evhttp_find_header(evhttp_request_get_input_headers(req), name);
}

static bool constant_time_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static bool auth_ok(struct evhttp_request *req, const char *required) {
    if (!required || !*required) return true;
    const char *x = header_get(req, "x-api-key");
    if (constant_time_eq(x, required)) return true;
    const char *a = header_get(req, "authorization");
    if (a && strncasecmp(a, "Bearer ", 7) == 0 && constant_time_eq(a + 7, required)) return true;
    return false;
}

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

static bool admin_auth_ok(struct evhttp_request *req) {
    char *k = config_get_string_copy("admin_token");
    bool ok = auth_ok(req, k);
    free(k);
    return ok;
}

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

static void send_json(struct evhttp_request *req, int code, cJSON *obj) {
    char *txt = cJSON_PrintUnformatted(obj);
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", txt ? txt : "{}");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json; charset=utf-8");
    evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", out);
    evbuffer_free(out);
    free(txt);
}

static void send_text(struct evhttp_request *req, int code, const char *ct, const char *body) {
    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "%s", body ? body : "");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", ct ? ct : "text/plain; charset=utf-8");
    evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", out);
    evbuffer_free(out);
}

static void send_error_json(struct evhttp_request *req, int code, const char *type, const char *msg) {
    cJSON *j = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "type", type ? type : "api_error");
    cJSON_AddStringToObject(err, "message", msg ? msg : "error");
    cJSON_AddItemToObject(j, "error", err);
    send_json(req, code, j);
    cJSON_Delete(j);
}

static char *make_id(const char *prefix) {
    char buf[128];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%s_%ld%09ld_%04x", prefix, (long)ts.tv_sec, ts.tv_nsec, rand() & 0xffff);
    return xstrdup(buf);
}

static cJSON *string_or_content_text(cJSON *content) {
    if (cJSON_IsString(content)) return cJSON_CreateString(content->valuestring ? content->valuestring : "");
    membuf_t b; membuf_init(&b);
    if (cJSON_IsArray(content)) {
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (type && strcmp(type, "text") == 0) {
                const char *t = json_get_str(blk, "text");
                if (t) {
                    if (b.len) membuf_append(&b, "\n", 1);
                    membuf_append(&b, t, strlen(t));
                }
            }
        }
    }
    cJSON *s = cJSON_CreateString(b.ptr ? b.ptr : "");
    membuf_free(&b);
    return s;
}

static cJSON *anthropic_content_to_openai_content(cJSON *content, bool *has_non_text) {
    *has_non_text = false;
    if (cJSON_IsString(content)) return cJSON_CreateString(content->valuestring ? content->valuestring : "");
    if (!cJSON_IsArray(content)) return cJSON_CreateString("");

    cJSON *arr = cJSON_CreateArray();
    cJSON *blk;
    cJSON_ArrayForEach(blk, content) {
        const char *type = json_get_str(blk, "type");
        if (!type) continue;
        if (strcmp(type, "text") == 0) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", "text");
            cJSON_AddStringToObject(o, "text", json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
            cJSON_AddItemToArray(arr, o);
        } else if (strcmp(type, "image") == 0) {
            *has_non_text = true;
            cJSON *src = cJSON_GetObjectItemCaseSensitive(blk, "source");
            const char *stype = src ? json_get_str(src, "type") : NULL;
            const char *url = NULL;
            char *data_url = NULL;
            if (stype && strcmp(stype, "url") == 0) {
                url = json_get_str(src, "url");
            } else if (stype && strcmp(stype, "base64") == 0) {
                const char *media = json_get_str(src, "media_type");
                const char *data = json_get_str(src, "data");
                if (media && data) {
                    size_t need = strlen(media) + strlen(data) + 32;
                    data_url = (char *)calloc(1, need);
                    snprintf(data_url, need, "data:%s;base64,%s", media, data);
                    url = data_url;
                }
            }
            if (url) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", "image_url");
                cJSON *iu = cJSON_CreateObject();
                cJSON_AddStringToObject(iu, "url", url);
                cJSON_AddItemToObject(o, "image_url", iu);
                cJSON_AddItemToArray(arr, o);
            }
            free(data_url);
        }
    }
    if (!*has_non_text) {
        cJSON *text = string_or_content_text(content);
        cJSON_Delete(arr);
        return text;
    }
    return arr;
}

static cJSON *json_from_string_or_empty_object(const char *s) {
    if (s && *s) {
        cJSON *j = cJSON_Parse(s);
        if (j) return j;
    }
    return cJSON_CreateObject();
}

static cJSON *anthropic_tools_to_openai(cJSON *tools) {
    if (!cJSON_IsArray(tools)) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const char *name = json_get_str(t, "name");
        if (!name) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", name);
        const char *desc = json_get_str(t, "description");
        if (desc) cJSON_AddStringToObject(fn, "description", desc);
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "input_schema");
        if (schema) cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
        else cJSON_AddItemToObject(fn, "parameters", cJSON_CreateObject());
        cJSON_AddItemToObject(item, "function", fn);
        cJSON_AddItemToArray(out, item);
    }
    if (cJSON_GetArraySize(out) == 0) { cJSON_Delete(out); return NULL; }
    return out;
}

static cJSON *anthropic_tool_choice_to_openai(cJSON *tc) {
    if (!tc) return NULL;
    if (cJSON_IsString(tc)) return cJSON_CreateString(tc->valuestring);
    if (!cJSON_IsObject(tc)) return NULL;
    const char *type = json_get_str(tc, "type");
    if (!type) return NULL;
    if (strcmp(type, "auto") == 0) return cJSON_CreateString("auto");
    if (strcmp(type, "any") == 0) return cJSON_CreateString("required");
    if (strcmp(type, "tool") == 0) {
        const char *name = json_get_str(tc, "name");
        if (!name) return NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "function");
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", name);
        cJSON_AddItemToObject(o, "function", f);
        return o;
    }
    return NULL;
}

static void append_model_params(cJSON *dst, cJSON *params) {
    if (!cJSON_IsObject(params)) return;
    cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (!p->string) continue;
        cJSON_ReplaceItemInObjectCaseSensitive(dst, p->string, cJSON_Duplicate(p, 1));
        if (!cJSON_GetObjectItemCaseSensitive(dst, p->string)) {
            cJSON_AddItemToObject(dst, p->string, cJSON_Duplicate(p, 1));
        }
    }
}

static void merge_extra_body(cJSON *dst, cJSON *extra) {
    if (!cJSON_IsObject(extra)) return;
    cJSON *p;
    cJSON_ArrayForEach(p, extra) {
        if (!p->string) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(dst, p->string);
        cJSON_AddItemToObject(dst, p->string, cJSON_Duplicate(p, 1));
    }
}

static void convert_message_content_blocks(cJSON *openai_messages, const char *role, cJSON *content) {
    if (strcmp(role, "user") == 0 && cJSON_IsArray(content)) {
        cJSON *text_blocks = cJSON_CreateArray();
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (!type) continue;
            if (strcmp(type, "tool_result") == 0) {
                const char *tid = json_get_str(blk, "tool_use_id");
                cJSON *toolmsg = cJSON_CreateObject();
                cJSON_AddStringToObject(toolmsg, "role", "tool");
                if (tid) cJSON_AddStringToObject(toolmsg, "tool_call_id", tid);
                cJSON *trc = cJSON_GetObjectItemCaseSensitive(blk, "content");
                cJSON_AddItemToObject(toolmsg, "content", string_or_content_text(trc));
                cJSON_AddItemToArray(openai_messages, toolmsg);
            } else if (strcmp(type, "text") == 0 || strcmp(type, "image") == 0) {
                cJSON_AddItemToArray(text_blocks, cJSON_Duplicate(blk, 1));
            }
        }
        if (cJSON_GetArraySize(text_blocks) > 0) {
            bool has_non_text = false;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddItemToObject(msg, "content", anthropic_content_to_openai_content(text_blocks, &has_non_text));
            cJSON_AddItemToArray(openai_messages, msg);
        }
        cJSON_Delete(text_blocks);
        return;
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", strcmp(role, "assistant") == 0 ? "assistant" : "user");
    bool has_non_text = false;
    cJSON_AddItemToObject(msg, "content", anthropic_content_to_openai_content(content, &has_non_text));

    if (strcmp(role, "assistant") == 0 && cJSON_IsArray(content)) {
        cJSON *calls = cJSON_CreateArray();
        cJSON *blk;
        int idx = 0;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (type && strcmp(type, "tool_use") == 0) {
                const char *id = json_get_str(blk, "id");
                const char *name = json_get_str(blk, "name");
                if (!name) continue;
                cJSON *call = cJSON_CreateObject();
                if (id) cJSON_AddStringToObject(call, "id", id);
                cJSON_AddStringToObject(call, "type", "function");
                cJSON_AddNumberToObject(call, "index", idx++);
                cJSON *fn = cJSON_CreateObject();
                cJSON_AddStringToObject(fn, "name", name);
                cJSON *input = cJSON_GetObjectItemCaseSensitive(blk, "input");
                char *arg = input ? cJSON_PrintUnformatted(input) : xstrdup("{}");
                cJSON_AddStringToObject(fn, "arguments", arg ? arg : "{}");
                free(arg);
                cJSON_AddItemToObject(call, "function", fn);
                cJSON_AddItemToArray(calls, call);
            }
        }
        if (cJSON_GetArraySize(calls) > 0) cJSON_AddItemToObject(msg, "tool_calls", calls);
        else cJSON_Delete(calls);
    }
    cJSON_AddItemToArray(openai_messages, msg);
}

static cJSON *build_openai_request(cJSON *anth_req, cJSON *model_cfg) {
    const char *upstream_model = json_get_str(model_cfg, "upstream_model");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "model", upstream_model ? upstream_model : "model");

    cJSON *messages = cJSON_CreateArray();
    cJSON *system = cJSON_GetObjectItemCaseSensitive(anth_req, "system");
    if (system) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddItemToObject(sys, "content", string_or_content_text(system));
        cJSON_AddItemToArray(messages, sys);
    }

    cJSON *anth_messages = cJSON_GetObjectItemCaseSensitive(anth_req, "messages");
    if (cJSON_IsArray(anth_messages)) {
        cJSON *m;
        cJSON_ArrayForEach(m, anth_messages) {
            const char *role = json_get_str(m, "role");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (!role || !content) continue;
            convert_message_content_blocks(messages, role, content);
        }
    }
    cJSON_AddItemToObject(out, "messages", messages);

    cJSON *params = cJSON_GetObjectItemCaseSensitive(model_cfg, "params");
    append_model_params(out, params);

    cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(anth_req, "max_tokens");
    if (cJSON_IsNumber(max_tokens)) cJSON_AddNumberToObject(out, "max_tokens", max_tokens->valuedouble);
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(anth_req, "temperature");
    if (cJSON_IsNumber(temperature)) { cJSON_DeleteItemFromObjectCaseSensitive(out, "temperature"); cJSON_AddNumberToObject(out, "temperature", temperature->valuedouble); }
    cJSON *top_p = cJSON_GetObjectItemCaseSensitive(anth_req, "top_p");
    if (cJSON_IsNumber(top_p)) { cJSON_DeleteItemFromObjectCaseSensitive(out, "top_p"); cJSON_AddNumberToObject(out, "top_p", top_p->valuedouble); }
    cJSON *stop = cJSON_GetObjectItemCaseSensitive(anth_req, "stop_sequences");
    if (stop) cJSON_AddItemToObject(out, "stop", cJSON_Duplicate(stop, 1));
    cJSON *stream = cJSON_GetObjectItemCaseSensitive(anth_req, "stream");
    bool is_stream = cJSON_IsTrue(stream);
    cJSON_AddBoolToObject(out, "stream", is_stream);
    if (is_stream) {
        cJSON *so = cJSON_CreateObject();
        cJSON_AddBoolToObject(so, "include_usage", true);
        cJSON_AddItemToObject(out, "stream_options", so);
    }

    cJSON *tools = anthropic_tools_to_openai(cJSON_GetObjectItemCaseSensitive(anth_req, "tools"));
    if (tools) cJSON_AddItemToObject(out, "tools", tools);
    cJSON *tc = anthropic_tool_choice_to_openai(cJSON_GetObjectItemCaseSensitive(anth_req, "tool_choice"));
    if (tc) cJSON_AddItemToObject(out, "tool_choice", tc);

    cJSON *extra = cJSON_GetObjectItemCaseSensitive(model_cfg, "extra_body");
    merge_extra_body(out, extra);
    return out;
}

static char *make_upstream_url(cJSON *model_cfg) {
    const char *endpoint = json_get_str(model_cfg, "endpoint");
    if (endpoint && *endpoint) return xstrdup(endpoint);
    const char *base = json_get_str(model_cfg, "base_url");
    char *b = trim_slashes(base ? base : "");
    size_t n = strlen(b) + strlen("/chat/completions") + 2;
    char *out = (char *)calloc(1, n);
    snprintf(out, n, "%s/chat/completions", b);
    free(b);
    return out;
}

static const char *map_finish_reason(const char *fr) {
    if (!fr) return "end_turn";
    if (strcmp(fr, "stop") == 0) return "end_turn";
    if (strcmp(fr, "length") == 0) return "max_tokens";
    if (strcmp(fr, "tool_calls") == 0) return "tool_use";
    if (strcmp(fr, "function_call") == 0) return "tool_use";
    return "end_turn";
}

static cJSON *openai_message_to_anthropic_content(cJSON *msg) {
    cJSON *content = cJSON_CreateArray();
    cJSON *txt = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0]) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", txt->valuestring);
        cJSON_AddItemToArray(content, b);
    }
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    if (cJSON_IsArray(calls)) {
        cJSON *tc;
        cJSON_ArrayForEach(tc, calls) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
            const char *id = json_get_str(tc, "id");
            const char *name = fn ? json_get_str(fn, "name") : NULL;
            const char *args = fn ? json_get_str(fn, "arguments") : NULL;
            if (!name) continue;
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_use");
            cJSON_AddStringToObject(b, "id", id ? id : make_id("toolu"));
            cJSON_AddStringToObject(b, "name", name);
            cJSON_AddItemToObject(b, "input", json_from_string_or_empty_object(args));
            cJSON_AddItemToArray(content, b);
        }
    }
    if (cJSON_GetArraySize(content) == 0) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", "");
        cJSON_AddItemToArray(content, b);
    }
    return content;
}

static cJSON *convert_openai_response_to_anthropic(const char *body, const char *client_model) {
    cJSON *oai = cJSON_Parse(body ? body : "");
    if (!oai) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "invalid_upstream_response");
        cJSON_AddStringToObject(e, "message", "upstream did not return valid JSON");
        cJSON_AddItemToObject(err, "error", e);
        return err;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(oai, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    const char *fr = choice ? json_get_str(choice, "finish_reason") : NULL;
    cJSON *out = cJSON_CreateObject();
    char *id = make_id("msg");
    cJSON_AddStringToObject(out, "id", id);
    free(id);
    cJSON_AddStringToObject(out, "type", "message");
    cJSON_AddStringToObject(out, "role", "assistant");
    cJSON_AddStringToObject(out, "model", client_model ? client_model : "claude-code-gateway");
    cJSON_AddItemToObject(out, "content", msg ? openai_message_to_anthropic_content(msg) : cJSON_CreateArray());
    cJSON_AddStringToObject(out, "stop_reason", map_finish_reason(fr));
    cJSON_AddNullToObject(out, "stop_sequence");
    cJSON *usage = cJSON_CreateObject();
    cJSON *u = cJSON_GetObjectItemCaseSensitive(oai, "usage");
    cJSON_AddNumberToObject(usage, "input_tokens", json_get_long(u, "prompt_tokens", 0));
    cJSON_AddNumberToObject(usage, "output_tokens", json_get_long(u, "completion_tokens", 0));
    cJSON_AddItemToObject(out, "usage", usage);
    cJSON_Delete(oai);
    return out;
}

static void stream_send_chunk(gateway_job_t *job, const char *data) {
    if (!job || !job->client_req || !data) return;
    if (job->stream_state.ended || job->response_sent) return;
    struct evbuffer *b = evbuffer_new();
    evbuffer_add(b, data, strlen(data));
    evhttp_send_reply_chunk(job->client_req, b);
    evbuffer_free(b);
}

static void stream_emit_json(gateway_job_t *job, const char *event, cJSON *obj) {
    char *txt = cJSON_PrintUnformatted(obj);
    if (!txt) return;
    membuf_t b; membuf_init(&b);
    membuf_append(&b, "event: ", 7);
    membuf_append(&b, event, strlen(event));
    membuf_append(&b, "\n", 1);
    membuf_append(&b, "data: ", 6);
    membuf_append(&b, txt, strlen(txt));
    membuf_append(&b, "\n\n", 2);
    stream_send_chunk(job, b.ptr);
    membuf_free(&b);
    free(txt);
}

static void stream_start_reply(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->reply_started) return;
    struct evkeyvalq *h = evhttp_request_get_output_headers(job->client_req);
    evhttp_add_header(h, "Content-Type", "text/event-stream; charset=utf-8");
    evhttp_add_header(h, "Cache-Control", "no-cache, no-transform");
    evhttp_add_header(h, "Connection", "keep-alive");
    evhttp_add_header(h, "X-Accel-Buffering", "no");
    evhttp_send_reply_start(job->client_req, 200, "OK");
    s->reply_started = true;
}

static void stream_message_start(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->message_started) return;
    if (!s->message_id) s->message_id = make_id("msg");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "message_start");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id", s->message_id);
    cJSON_AddStringToObject(msg, "type", "message");
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "model", s->client_model ? s->client_model : "claude-code-gateway");
    cJSON_AddItemToObject(msg, "content", cJSON_CreateArray());
    cJSON_AddNullToObject(msg, "stop_reason");
    cJSON_AddNullToObject(msg, "stop_sequence");
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "input_tokens", s->prompt_tokens);
    cJSON_AddNumberToObject(usage, "output_tokens", 0);
    cJSON_AddItemToObject(msg, "usage", usage);
    cJSON_AddItemToObject(data, "message", msg);
    stream_emit_json(job, "message_start", data);
    cJSON_Delete(data);
    s->message_started = true;
}

static void stream_text_start(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    stream_start_reply(job);
    stream_message_start(job);
    if (s->text_started) return;
    s->text_block_index = s->next_block_index++;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_start");
    cJSON_AddNumberToObject(data, "index", s->text_block_index);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", "");
    cJSON_AddItemToObject(data, "content_block", blk);
    stream_emit_json(job, "content_block_start", data);
    cJSON_Delete(data);
    s->text_started = true;
}

static void stream_text_delta(gateway_job_t *job, const char *text) {
    if (!text || !*text) return;
    stream_text_start(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", job->stream_state.text_block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "text_delta");
    cJSON_AddStringToObject(delta, "text", text);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
}

static tool_stream_state_t *get_tool_state(stream_state_t *s, int openai_index) {
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (s->tools[i].started && s->tools[i].openai_index == openai_index) return &s->tools[i];
    }
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (!s->tools[i].started) {
            s->tools[i].openai_index = openai_index;
            s->tools[i].block_index = s->next_block_index++;
            s->tools[i].started = true;
            return &s->tools[i];
        }
    }
    return NULL;
}

static void stream_tool_start_if_needed(gateway_job_t *job, tool_stream_state_t *ts) {
    stream_start_reply(job);
    stream_message_start(job);
    if (!ts) return;
    if (!ts->id) ts->id = make_id("toolu");
    if (!ts->name) ts->name = xstrdup("tool");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_start");
    cJSON_AddNumberToObject(data, "index", ts->block_index);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "tool_use");
    cJSON_AddStringToObject(blk, "id", ts->id);
    cJSON_AddStringToObject(blk, "name", ts->name);
    cJSON_AddItemToObject(blk, "input", cJSON_CreateObject());
    cJSON_AddItemToObject(data, "content_block", blk);
    stream_emit_json(job, "content_block_start", data);
    cJSON_Delete(data);
}

static void stream_tool_json_delta(gateway_job_t *job, tool_stream_state_t *ts, const char *partial) {
    if (!ts || !partial || !*partial) return;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", ts->block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "input_json_delta");
    cJSON_AddStringToObject(delta, "partial_json", partial);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
}

static void stream_emit_error(gateway_job_t *job, const char *message) {
    stream_start_reply(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "error");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", "api_error");
    cJSON_AddStringToObject(e, "message", message ? message : "upstream error");
    cJSON_AddItemToObject(data, "error", e);
    stream_emit_json(job, "error", data);
    cJSON_Delete(data);
}

static void stream_finish(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->ended) return;
    stream_start_reply(job);
    stream_message_start(job);
    if (s->text_started) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "type", "content_block_stop");
        cJSON_AddNumberToObject(data, "index", s->text_block_index);
        stream_emit_json(job, "content_block_stop", data);
        cJSON_Delete(data);
    }
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (s->tools[i].started) {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "type", "content_block_stop");
            cJSON_AddNumberToObject(data, "index", s->tools[i].block_index);
            stream_emit_json(job, "content_block_stop", data);
            cJSON_Delete(data);
        }
    }
    cJSON *delta_ev = cJSON_CreateObject();
    cJSON_AddStringToObject(delta_ev, "type", "message_delta");
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "stop_reason", map_finish_reason(s->finish_reason));
    cJSON_AddNullToObject(delta, "stop_sequence");
    cJSON_AddItemToObject(delta_ev, "delta", delta);
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "output_tokens", s->completion_tokens > 0 ? s->completion_tokens : 0);
    cJSON_AddItemToObject(delta_ev, "usage", usage);
    stream_emit_json(job, "message_delta", delta_ev);
    cJSON_Delete(delta_ev);

    cJSON *stop = cJSON_CreateObject();
    cJSON_AddStringToObject(stop, "type", "message_stop");
    stream_emit_json(job, "message_stop", stop);
    cJSON_Delete(stop);
    evhttp_send_reply_end(job->client_req);
    job->response_sent = true;
    s->ended = true;
}

static void stream_state_free(stream_state_t *s) {
    free(s->linebuf);
    free(s->message_id);
    free(s->client_model);
    free(s->finish_reason);
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        free(s->tools[i].id);
        free(s->tools[i].name);
    }
}

static void handle_openai_stream_json(gateway_job_t *job, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        long pt = json_get_long(usage, "prompt_tokens", -1);
        long ct = json_get_long(usage, "completion_tokens", -1);
        if (pt >= 0) job->stream_state.prompt_tokens = pt;
        if (ct >= 0) job->stream_state.completion_tokens = ct;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *ch = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!ch) { cJSON_Delete(root); return; }
    const char *fr = json_get_str(ch, "finish_reason");
    if (fr) {
        free(job->stream_state.finish_reason);
        job->stream_state.finish_reason = xstrdup(fr);
    }
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(ch, "delta");
    if (cJSON_IsObject(delta)) {
        const char *content = json_get_str(delta, "content");
        if (content) stream_text_delta(job, content);
        cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        if (cJSON_IsArray(tool_calls)) {
            cJSON *tc;
            cJSON_ArrayForEach(tc, tool_calls) {
                int oi = (int)json_get_long(tc, "index", 0);
                tool_stream_state_t *ts = get_tool_state(&job->stream_state, oi);
                const char *id = json_get_str(tc, "id");
                if (id && !ts->id) ts->id = xstrdup(id);
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
                const char *name = fn ? json_get_str(fn, "name") : NULL;
                if (name && !ts->name) ts->name = xstrdup(name);
                stream_tool_start_if_needed(job, ts);
                const char *args = fn ? json_get_str(fn, "arguments") : NULL;
                if (args) stream_tool_json_delta(job, ts, args);
            }
        }
    }
    cJSON_Delete(root);
}

static void process_sse_line(gateway_job_t *job, char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    if (strncmp(line, "data:", 5) != 0) return;
    char *data = line + 5;
    while (*data == ' ') data++;
    size_t n = strlen(data);
    while (n > 0 && (data[n-1] == '\r' || data[n-1] == '\n')) data[--n] = 0;
    if (strcmp(data, "[DONE]") == 0) {
        stream_finish(job);
        return;
    }
    handle_openai_stream_json(job, data);
}

static void stream_parse_append(gateway_job_t *job, const char *ptr, size_t n) {
    stream_state_t *s = &job->stream_state;
    // Don't process more data after stream has ended
    if (s->ended) return;
    if (s->linebuf_len + n + 1 > s->linebuf_cap) {
        size_t nc = s->linebuf_cap ? s->linebuf_cap * 2 : 8192;
        while (nc < s->linebuf_len + n + 1) nc *= 2;
        char *p = (char *)realloc(s->linebuf, nc);
        if (!p) return;
        s->linebuf = p;
        s->linebuf_cap = nc;
    }
    memcpy(s->linebuf + s->linebuf_len, ptr, n);
    s->linebuf_len += n;
    s->linebuf[s->linebuf_len] = 0;

    char *start = s->linebuf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = 0;
        process_sse_line(job, start);
        if (s->ended) return;
        start = nl + 1;
    }
    size_t rem = strlen(start);
    memmove(s->linebuf, start, rem + 1);
    s->linebuf_len = rem;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nmemb;
    if (job->stream) {
        stream_start_reply(job);
        stream_parse_append(job, ptr, n);
    } else {
        membuf_append(&job->upstream_body, ptr, n);
    }
    return n;
}

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nitems;
    (void)buffer;
    job->upstream_headers_done = true;
    return n;
}

static void deferred_req_free(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    struct evhttp_request *req = (struct evhttp_request *)arg;
    if (req) evhttp_request_free(req);
}



static void job_free(gateway_job_t *job) {
    if (!job) return;
    free(job->request_body); job->request_body = NULL;
    free(job->upstream_url); job->upstream_url = NULL;
    free(job->api_key); job->api_key = NULL;
    free(job->provider_name); job->provider_name = NULL;
    free(job->client_model); job->client_model = NULL;
    free(job->upstream_model); job->upstream_model = NULL;
    membuf_free(&job->upstream_body);
    stream_state_free(&job->stream_state);
    if (job->headers) { curl_slist_free_all(job->headers); job->headers = NULL; }
    if (job->easy) { curl_easy_cleanup(job->easy); job->easy = NULL; }
    // If response was already sent via evhttp_send_reply, libevent owns the request
    if (job->client_req && !job->response_sent) {
        if (BASE) {
            event_base_once(BASE, -1, EV_TIMEOUT, deferred_req_free, job->client_req, NULL);
        } else {
            evhttp_request_free(job->client_req);
        }
        job->client_req = NULL;
    }
    job->client_req = NULL;
    free(job);
}

static void complete_nonstream_job(gateway_job_t *job, CURLcode rc) {
    if (rc != CURLE_OK) {
        log_msg("ERROR", "UPSTREAM_FAIL model=%s url=%s curl_error=%s", job->client_model, job->upstream_url, curl_easy_strerror(rc));
        send_error_json(job->client_req, 502, "upstream_error", curl_easy_strerror(rc));
        job->response_sent = true;
        return;
    }
    curl_easy_getinfo(job->easy, CURLINFO_RESPONSE_CODE, &job->upstream_status);
    log_msg("INFO", "UPS_RESP model=%s upstream_status=%ld response_len=%zu",
        job->client_model, job->upstream_status, job->upstream_body.len);
    if (job->upstream_status < 200 || job->upstream_status >= 300) {
        char *body_preview = job->upstream_body.ptr ? job->upstream_body.ptr : "";
        size_t preview_len = strlen(body_preview);
        if (preview_len > 500) preview_len = 500;
        log_msg("ERROR", "UPSTREAM_HTTP_ERR model=%s status=%ld body=%.*s", job->client_model, job->upstream_status, (int)preview_len, body_preview);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "error");
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "upstream_error");
        cJSON_AddNumberToObject(err, "status", job->upstream_status);
        cJSON_AddStringToObject(err, "message", job->upstream_body.ptr ? job->upstream_body.ptr : "upstream HTTP error");
        cJSON_AddItemToObject(e, "error", err);
        send_json(job->client_req, 502, e);
        cJSON_Delete(e);
        job->response_sent = true;
        return;
    }
    cJSON *anth = convert_openai_response_to_anthropic(job->upstream_body.ptr, job->client_model);
    if (!anth) {
        log_msg("ERROR", "CONV_FAIL model=%s upstream_body=%.*s", job->client_model, (int)(job->upstream_body.len > 500 ? 500 : job->upstream_body.len), job->upstream_body.ptr ? job->upstream_body.ptr : "");
        send_error_json(job->client_req, 502, "conversion_error", "failed to convert upstream response");
        job->response_sent = true;
        return;
    }
    log_msg("INFO", "RESP_OK model=%s", job->client_model);
    send_json(job->client_req, 200, anth);
    cJSON_Delete(anth);
    job->response_sent = true;
}

static void complete_stream_job(gateway_job_t *job, CURLcode rc) {
    if (job->stream_state.ended) {
        log_msg("INFO", "STRM_END model=%s already ended", job->client_model);
        return;
    }
    if (rc != CURLE_OK) {
        log_msg("ERROR", "STRM_FAIL model=%s url=%s curl_error=%s", job->client_model, job->upstream_url, curl_easy_strerror(rc));
        stream_emit_error(job, curl_easy_strerror(rc));
    }
    curl_easy_getinfo(job->easy, CURLINFO_RESPONSE_CODE, &job->upstream_status);
    if (job->upstream_status >= 400) {
        log_msg("ERROR", "STRM_HTTP_ERR model=%s status=%ld body=%.*s", job->client_model, job->upstream_status, (int)(job->upstream_body.len > 500 ? 500 : job->upstream_body.len), job->upstream_body.ptr ? job->upstream_body.ptr : "");
        stream_emit_error(job, "upstream returned HTTP error");
    }
    if (rc == CURLE_OK && job->upstream_status < 400) {
        log_msg("INFO", "STRM_OK model=%s upstream_status=%ld", job->client_model, job->upstream_status);
    }
    stream_finish(job);
}

static void worker_add_easy(worker_t *w, gateway_job_t *job) {
    job->easy = curl_easy_init();
    curl_easy_setopt(job->easy, CURLOPT_URL, job->upstream_url);
    curl_easy_setopt(job->easy, CURLOPT_POST, 1L);
    curl_easy_setopt(job->easy, CURLOPT_POSTFIELDS, job->request_body);
    curl_easy_setopt(job->easy, CURLOPT_POSTFIELDSIZE, (long)strlen(job->request_body));
    curl_easy_setopt(job->easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(job->easy, CURLOPT_WRITEDATA, job);
    curl_easy_setopt(job->easy, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(job->easy, CURLOPT_HEADERDATA, job);
    curl_easy_setopt(job->easy, CURLOPT_PRIVATE, job);
    curl_easy_setopt(job->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(job->easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(job->easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(job->easy, CURLOPT_TIMEOUT_MS, job->stream ? 0L : 600000L);
    curl_easy_setopt(job->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(job->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(job->easy, CURLOPT_SSL_VERIFYHOST, 2L);

    job->headers = NULL;
    job->headers = curl_slist_append(job->headers, "Content-Type: application/json");
    job->headers = curl_slist_append(job->headers, "Accept: application/json, text/event-stream");
    if (job->api_key && *job->api_key) {
        char auth[4096];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", job->api_key);
        job->headers = curl_slist_append(job->headers, auth);
    }
    curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
    curl_multi_add_handle(w->multi, job->easy);
}

static void *worker_loop(void *arg) {
    worker_t *w = (worker_t *)arg;
    w->multi = curl_multi_init();
    curl_multi_setopt(w->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(w->multi, CURLMOPT_MAX_HOST_CONNECTIONS, 64L);
    curl_multi_setopt(w->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 256L);
    log_msg("INFO", "worker %d started", w->id);
    while (!w->stop) {
        pthread_mutex_lock(&w->mu);
        while (!w->pending_head && w->still_running == 0 && !w->stop) {
            pthread_cond_wait(&w->cv, &w->mu);
        }
        gateway_job_t *list = w->pending_head;
        w->pending_head = w->pending_tail = NULL;
        pthread_mutex_unlock(&w->mu);

        while (list) {
            gateway_job_t *next = list->next;
            list->next = NULL;
            worker_add_easy(w, list);
            list = next;
        }

        CURLMcode mc = curl_multi_perform(w->multi, &w->still_running);
        if (mc != CURLM_OK) log_msg("ERROR", "curl_multi_perform: %s", curl_multi_strerror(mc));
        int numfds = 0;
        curl_multi_poll(w->multi, NULL, 0, 200, &numfds);

        int msgs_left = 0;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(w->multi, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE) {
                gateway_job_t *job = NULL;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &job);
                curl_multi_remove_handle(w->multi, msg->easy_handle);
                if (job) {
                    if (job->stream) complete_stream_job(job, msg->data.result);
                    else complete_nonstream_job(job, msg->data.result);
                    job_free(job);
                }
            }
        }
    }
    curl_multi_cleanup(w->multi);
    log_msg("INFO", "worker %d stopped", w->id);
    return NULL;
}

static void enqueue_job(gateway_job_t *job) {
    worker_t *w = &WORKERS[(RR++) % WORKER_COUNT];
    job->worker = w;
    pthread_mutex_lock(&w->mu);
    if (w->pending_tail) w->pending_tail->next = job;
    else w->pending_head = job;
    w->pending_tail = job;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void workers_start(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        WORKERS[i].id = i;
        pthread_mutex_init(&WORKERS[i].mu, NULL);
        pthread_cond_init(&WORKERS[i].cv, NULL);
        pthread_create(&WORKERS[i].tid, NULL, worker_loop, &WORKERS[i]);
    }
}

static void workers_stop(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        pthread_mutex_lock(&WORKERS[i].mu);
        WORKERS[i].stop = true;
        pthread_cond_signal(&WORKERS[i].cv);
        pthread_mutex_unlock(&WORKERS[i].mu);
    }
    for (int i = 0; i < WORKER_COUNT; i++) pthread_join(WORKERS[i].tid, NULL);
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

    gateway_job_t *job = (gateway_job_t *)calloc(1, sizeof(*job));
    membuf_init(&job->upstream_body);
    job->client_req = req;
    job->request_body = oai_body;
    job->upstream_url = url;
    job->api_key = xstrdup(api_key ? api_key : "");
    job->provider_name = xstrdup(provider ? provider : "openai-compatible");
    job->client_model = xstrdup(client_model ? client_model : "claude-code-gateway");
    job->upstream_model = xstrdup(upstream_model ? upstream_model : "model");
    job->stream = stream;
    job->stream_state.client_model = xstrdup(job->client_model);
    evhttp_request_own(req);
    enqueue_job(job);

    log_msg("INFO", "SEND model=%s upstream_model=%s provider=%s stream=%d url=%s oai_body_len=%zu",
        job->client_model, job->upstream_model, job->provider_name, stream ? 1 : 0, job->upstream_url, oai_body ? strlen(oai_body) : 0);
    cJSON_Delete(model);
    cJSON_Delete(oai);
    cJSON_Delete(anth);
    free(body);
}

static long estimate_tokens_from_json(cJSON *root) {
    char *txt = cJSON_PrintUnformatted(root);
    long n = txt ? (long)strlen(txt) : 0;
    free(txt);
    long tok = n / 4 + 1;
    return tok < 1 ? 1 : tok;
}


static void handle_models(struct evhttp_request *req) {
    if (!gateway_auth_ok(req)) { log_msg("WARN", "models auth failed"); send_error_json(req, 401, "authentication_error", "invalid gateway api key"); return; }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "object", "list");
    cJSON *data = cJSON_CreateArray();
    pthread_rwlock_rdlock(&G.lock);
    cJSON *models = cJSON_GetObjectItemCaseSensitive(G.root, "models");
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
    pthread_rwlock_unlock(&G.lock);
    cJSON_AddItemToObject(out, "data", data);
    send_json(req, 200, out);
    cJSON_Delete(out);
}

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

static void handle_health(struct evhttp_request *req) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    send_json(req, 200, j);
    cJSON_Delete(j);
}

static void handle_config_get(struct evhttp_request *req) {
    char *txt = config_masked_json();
    send_text(req, 200, "application/json; charset=utf-8", txt ? txt : "{}");
    free(txt);
}

static void handle_config_put(struct evhttp_request *req) {
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

static void handle_switch(struct evhttp_request *req) {
    char *body = read_request_body(req, MAX_BODY_BYTES);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *active = j ? json_get_str(j, "active_model") : NULL;
    char *err = NULL;
    int rc = config_set_active_model(active, &err);
    if (rc != 0) send_error_json(req, 400, "configuration_error", err ? err : "failed");
    else { cJSON *ok = cJSON_CreateObject(); cJSON_AddBoolToObject(ok, "ok", true); send_json(req, 200, ok); cJSON_Delete(ok); }
    free(err); cJSON_Delete(j); free(body);
}

static void handle_admin_html(struct evhttp_request *req) {
    size_t n = 0;
    char *html = read_file("./web/admin.html", &n);
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

static void handle_root(struct evhttp_request *req, void *arg) {
    (void)arg;
    const char *uri = evhttp_request_get_uri(req);
    const char *meth = evhttp_request_get_command(req) == EVHTTP_REQ_GET ? "GET" :
                       evhttp_request_get_command(req) == EVHTTP_REQ_POST ? "POST" :
                       evhttp_request_get_command(req) == EVHTTP_REQ_PUT ? "PUT" : "OTHER";
    log_msg("INFO", "REQ %s %s", meth, uri);
    // Compare only the path portion (ignore query string)
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
    if (URI_IS("/admin/api/config") && evhttp_request_get_command(req) == EVHTTP_REQ_GET) { handle_config_get(req); return; }
    if (URI_IS("/admin/api/config") && (evhttp_request_get_command(req) == EVHTTP_REQ_PUT || evhttp_request_get_command(req) == EVHTTP_REQ_POST)) { handle_config_put(req); return; }
    if (URI_IS("/admin/api/switch")) { handle_switch(req); return; }
#undef URI_IS
    send_error_json(req, 404, "not_found", "not found");
}

static void on_signal(evutil_socket_t sig, short events, void *arg) {
    (void)sig; (void)events; (void)arg;
    STOP = 1;
    if (BASE) event_base_loopexit(BASE, NULL);
}

int main(int argc, char **argv) {
    const char *config_path = getenv("GATEWAY_CONFIG");
    if (!config_path) config_path = DEFAULT_CONFIG_PATH;
    if (argc > 1) config_path = argv[1];
    srand((unsigned int)time(NULL));
    signal(SIGPIPE, SIG_IGN);
    if (evthread_use_pthreads() != 0) { fprintf(stderr, "evthread_use_pthreads failed\n"); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    config_load(config_path);
    workers_start();

    char *host = config_get_string_copy("listen_host");
    long port = 8080;
    pthread_rwlock_rdlock(&G.lock);
    port = json_get_long(G.root, "listen_port", 8080);
    pthread_rwlock_unlock(&G.lock);

    BASE = event_base_new();
    HTTP = evhttp_new(BASE);
    evhttp_set_allowed_methods(HTTP, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_PUT | EVHTTP_REQ_OPTIONS);
    evhttp_set_gencb(HTTP, handle_root, NULL);
    if (evhttp_bind_socket(HTTP, host ? host : "0.0.0.0", (uint16_t)port) != 0) {
        log_msg("ERROR", "bind failed on %s:%ld", host ? host : "0.0.0.0", port);
        return 1;
    }
    struct event *sigint_ev = evsignal_new(BASE, SIGINT, on_signal, NULL);
    struct event *sigterm_ev = evsignal_new(BASE, SIGTERM, on_signal, NULL);
    event_add(sigint_ev, NULL);
    event_add(sigterm_ev, NULL);
    log_msg("INFO", "Claude-Code OpenAI-compatible gateway listening on http://%s:%ld", host ? host : "0.0.0.0", port);
    log_msg("INFO", "admin UI: http://%s:%ld/admin", host ? host : "127.0.0.1", port);
    free(host);

    event_base_dispatch(BASE);
    log_msg("INFO", "shutting down");
    workers_stop();
    evhttp_free(HTTP);
    event_free(sigint_ev);
    event_free(sigterm_ev);
    event_base_free(BASE);
    pthread_rwlock_wrlock(&G.lock);
    cJSON_Delete(G.root);
    pthread_rwlock_unlock(&G.lock);
    pthread_rwlock_destroy(&G.lock);
    curl_global_cleanup();
    return 0;
}
