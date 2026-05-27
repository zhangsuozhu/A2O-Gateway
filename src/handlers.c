#include "handlers.h"
#include "config.h"
#include "convert.h"
#include "worker.h"
#include "log.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

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

static bool admin_auth_ok(struct evhttp_request *req) {
    const char *token = header_get(req, "x-session-token");
    return session_valid(token);
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
}static char *read_request_body(struct evhttp_request *req, size_t max_bytes) {
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

static long estimate_tokens_from_json(cJSON *root) {
    char *txt = cJSON_PrintUnformatted(root);
    long n = txt ? (long)strlen(txt) : 0;
    free(txt);
    long tok = n / 4 + 1;
    return tok < 1 ? 1 : tok;
}

static void handle_health(struct evhttp_request *req) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    send_json(req, 200, j);
    cJSON_Delete(j);
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

static void handle_config_get(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    char *txt = config_masked_json();
    send_text(req, 200, "application/json; charset=utf-8", txt ? txt : "{}");
    free(txt);
}

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

static void handle_change_password(struct evhttp_request *req) {
    if (!admin_auth_ok(req)) { send_error_json(req, 401, "authentication_error", "admin login required"); return; }
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) { send_error_json(req, 405, "method_not_allowed", "POST required"); return; }
    char *body = read_request_body(req, MAX_BODY_BYTES);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *old_pw = j ? json_get_str(j, "old_password") : NULL;
    const char *new_pw = j ? json_get_str(j, "new_password") : NULL;
    if (!old_pw || !new_pw || !*new_pw) {
        send_error_json(req, 400, "invalid_request_error", "old_password and new_password required");
        cJSON_Delete(j); free(body); return;
    }
    char *expected = config_get_string_copy("admin_password");
    bool ok = false;
    if (expected && constant_time_eq(old_pw, expected)) ok = true;
    free(expected);
    if (!ok) {
        send_error_json(req, 403, "authentication_error", "old password mismatch");
        cJSON_Delete(j); free(body); return;
    }
    char *err = NULL;
    int rc = config_set_string("admin_password", new_pw, &err);
    if (rc != 0) { send_error_json(req, 400, "configuration_error", err ? err : "failed"); free(err); }
    else {
        /* Also update admin_token to match for backwards compat */
        config_set_string("admin_token", new_pw, NULL);
        cJSON *out = cJSON_CreateObject(); cJSON_AddBoolToObject(out, "ok", true); send_json(req, 200, out); cJSON_Delete(out);
    }
    cJSON_Delete(j); free(body);
}

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
    if (URI_IS("/admin/api/change-password") && evhttp_request_get_command(req) == EVHTTP_REQ_POST) { handle_change_password(req); return; }
#undef URI_IS
    send_error_json(req, 404, "not_found", "not found");
}