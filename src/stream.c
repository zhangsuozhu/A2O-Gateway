#include "stream.h"
#include "convert.h"
#include "log.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

#define TEXT_FLUSH_THRESHOLD 4096

static void stream_flush_text(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (!s->text_pending || s->text_pending_len == 0) return;
    stream_text_start(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", s->text_block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "text_delta");
    cJSON_AddStringToObject(delta, "text", s->text_pending);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
    /* Reset pending buffer */
    s->text_pending[0] = 0;
    s->text_pending_len = 0;
}

void stream_send_chunk(gateway_job_t *job, const char *data) {
    if (!job || !job->client_req || !data) return;
    if (job->stream_state.ended || job->response_sent) return;
    pthread_mutex_lock(&job->send_mu);
    if (!job->send_buf) job->send_buf = evbuffer_new();
    evbuffer_add(job->send_buf, data, strlen(data));
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
}

void stream_emit_json(gateway_job_t *job, const char *event, cJSON *obj) {
    char *txt = cJSON_PrintUnformatted(obj);
    if (!txt) return;
    membuf_t b; membuf_init(&b);
    membuf_append(&b, "event: ", 7);
    membuf_append(&b, event, strlen(event));
    membuf_append(&b, "\n", 1);
    membuf_append(&b, "data: ", 6);
    membuf_append(&b, txt, strlen(txt));
    membuf_append(&b, "\n\n", 2);
    log_msg("DEBUG", "SSE_EMIT model=%s event=%s data=%s", job->client_model, event, txt);
    stream_send_chunk(job, b.ptr);
    membuf_free(&b);
    free(txt);
}

void stream_start_reply(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->reply_started) return;
    pthread_mutex_lock(&job->send_mu);
    job->send_start = true;
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
    s->reply_started = true;
}

void stream_message_start(gateway_job_t *job) {
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

void stream_text_start(gateway_job_t *job) {
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

static void accum_pending(stream_state_t *s, const char *text) {
    size_t len = strlen(text);
    if (s->text_pending_len + len + 1 > s->text_pending_cap) {
        size_t nc = s->text_pending_cap ? s->text_pending_cap * 2 : 8192;
        while (nc < s->text_pending_len + len + 1) nc *= 2;
        char *p = (char *)realloc(s->text_pending, nc);
        if (!p) return;
        s->text_pending = p;
        s->text_pending_cap = nc;
    }
    memcpy(s->text_pending + s->text_pending_len, text, len + 1);
    s->text_pending_len += len;
}

void stream_text_delta(gateway_job_t *job, const char *text) {
    if (!text || !*text) return;
    stream_state_t *s = &job->stream_state;
    if (s->response_text) {
        size_t old = strlen(s->response_text);
        size_t newlen = old + strlen(text);
        char *p = (char *)realloc(s->response_text, newlen + 1);
        if (p) { s->response_text = p; memcpy(s->response_text + old, text, newlen - old + 1); }
    } else {
        s->response_text = xstrdup(text);
    }
    /* Accumulate and flush only when buffer is large enough */
    accum_pending(s, text);
    if (s->text_pending_len >= TEXT_FLUSH_THRESHOLD) {
        stream_flush_text(job);
    }
}

tool_stream_state_t *get_tool_state(stream_state_t *s, int openai_index) {
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

void stream_tool_start_if_needed(gateway_job_t *job, tool_stream_state_t *ts) {
    stream_start_reply(job);
    stream_message_start(job);
    if (!ts) return;
    if (!ts->id) ts->id = make_id("toolu");
    if (!ts->name) ts->name = xstrdup("tool");
    if (ts->start_emitted) return;
    ts->start_emitted = true;
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

void stream_tool_json_delta(gateway_job_t *job, tool_stream_state_t *ts, const char *partial) {
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

void stream_emit_error(gateway_job_t *job, const char *message) {
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

void stream_finish(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->ended) return;
    stream_start_reply(job);
    stream_message_start(job);
    /* Flush any remaining buffered text */
    stream_flush_text(job);
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
    pthread_mutex_lock(&job->send_mu);
    job->send_end = true;
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
    job->response_sent = true;
    s->ended = true;

    /* Realtime print for stream completion */
    rt_print("[RES] model=%s type=stream upstream_status=%ld prompt_tokens=%ld completion_tokens=%ld",
        job->client_model, job->upstream_status, s->prompt_tokens, s->completion_tokens);
    if (job->upstream_body.ptr && job->upstream_body.len > 0) {
        if (rt_get_mode() == RT_TXT) {
            rt_print("[RES_BODY] model=%s %s%s", job->client_model,
                s->response_text ? s->response_text : "",
                s->finish_reason ? s->finish_reason : "");
        } else {
            rt_print("[RES_BODY] model=%s %s", job->client_model, job->upstream_body.ptr);
        }
    }
}

void stream_state_free(stream_state_t *s) {
    free(s->linebuf);
    free(s->message_id);
    free(s->client_model);
    free(s->finish_reason);
    free(s->response_text);
    free(s->text_pending);
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        free(s->tools[i].id);
        free(s->tools[i].name);
    }
}

void handle_openai_stream_json(gateway_job_t *job, const char *json) {
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
                cJSON *args_obj = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                if (cJSON_IsString(args_obj) && args_obj->valuestring && args_obj->valuestring[0]) {
                    stream_tool_json_delta(job, ts, args_obj->valuestring);
                } else if (cJSON_IsObject(args_obj)) {
                    char *args_str = cJSON_PrintUnformatted(args_obj);
                    if (args_str) {
                        stream_tool_json_delta(job, ts, args_str);
                        free(args_str);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

void process_sse_line(gateway_job_t *job, char *line) {
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

void stream_parse_append(gateway_job_t *job, const char *ptr, size_t n) {
    stream_state_t *s = &job->stream_state;
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

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nmemb;
    /* Always accumulate upstream body for logging */
    membuf_append(&job->upstream_body, ptr, n);
    if (job->stream) {
        stream_start_reply(job);
        stream_parse_append(job, ptr, n);
    }
    log_msg("DEBUG", "UP_RESP chunk model=%s len=%zu", job->client_model, n);
    return n;
}

size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nitems;
    job->upstream_headers_done = true;
    size_t hn = n;
    while (hn > 0 && (buffer[hn-1] == '\r' || buffer[hn-1] == '\n')) hn--;
    if (hn > 0 && hn < 512)
        log_msg("DEBUG", "UP_HDR model=%s %.*s", job->client_model, (int)hn, buffer);
    return n;
}

void deferred_send(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    gateway_job_t *job = arg;
    if (!job || !job->client_req) return;
    pthread_mutex_lock(&job->send_mu);
    struct evbuffer *buf = job->send_buf;
    job->send_buf = NULL;
    bool do_start = job->send_start;
    job->send_start = false;
    bool do_end = job->send_end;
    job->send_end = false;
    char *json = job->nonstream_json;
    job->nonstream_json = NULL;
    int code = job->nonstream_code;
    pthread_mutex_unlock(&job->send_mu);
    if (do_start) {
        struct evkeyvalq *h = evhttp_request_get_output_headers(job->client_req);
        evhttp_add_header(h, "Content-Type", "text/event-stream; charset=utf-8");
        evhttp_add_header(h, "Cache-Control", "no-cache, no-transform");
        evhttp_add_header(h, "Connection", "keep-alive");
        evhttp_add_header(h, "X-Accel-Buffering", "no");
        evhttp_send_reply_start(job->client_req, 200, "OK");
    }
    if (buf && evbuffer_get_length(buf) > 0) {
        size_t bl = evbuffer_get_length(buf);
        char *preview = (char *)calloc(1, bl + 1);
        if (preview) {
            evbuffer_copyout(buf, preview, bl);
            size_t pl = bl > 4096 ? 4096 : bl;
            log_msg("DEBUG", "SSE_SEND model=%s len=%zu %.*s", job->client_model, bl, (int)pl, preview);
            free(preview);
        }
        evhttp_send_reply_chunk(job->client_req, buf);
    }
    if (buf) evbuffer_free(buf);
    if (do_end) {
        log_msg("DEBUG", "SSE_END model=%s", job->client_model);
        evhttp_send_reply_end(job->client_req);
        job->response_sent = true;
    }
    if (json) {
        size_t jl = strlen(json);
        size_t pl = jl > 4096 ? 4096 : jl;
        log_msg("DEBUG", "RESP_SEND model=%s code=%d %.*s", job->client_model, code, (int)pl, json);
        struct evbuffer *out = evbuffer_new();
        evbuffer_add(out, json, strlen(json));
        evhttp_add_header(evhttp_request_get_output_headers(job->client_req), "Content-Type", "application/json; charset=utf-8");
        evhttp_send_reply(job->client_req, code, code == 200 ? "OK" : "Error", out);
        evbuffer_free(out);
        free(json);
        job->response_sent = true;
    }
}