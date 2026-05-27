#include "worker.h"
#include "stream.h"
#include "convert.h"
#include "stats.h"
#include "log.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void deferred_req_free(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    struct evhttp_request *req = (struct evhttp_request *)arg;
    if (req) evhttp_request_free(req);
}

static void deferred_job_free(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    gateway_job_t *job = arg;
    job_free(job);
}

static void complete_nonstream_job(gateway_job_t *job, CURLcode rc) {
    char *json_out = NULL;
    int code_out = 200;
    long input_tokens = 0, output_tokens = 0;
    if (rc != CURLE_OK) {
        log_msg("ERROR", "UPSTREAM_FAIL model=%s url=%s curl_error=%s", job->client_model, job->upstream_url, curl_easy_strerror(rc));
        if (job->upstream_body.ptr && job->upstream_body.len > 0) {
            size_t pl = job->upstream_body.len > 4096 ? 4096 : job->upstream_body.len;
            log_msg("DEBUG", "UP_BODY_FAIL model=%s %.*s", job->client_model, (int)pl, job->upstream_body.ptr);
        }
        cJSON *j = cJSON_CreateObject();
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "upstream_error");
        cJSON_AddStringToObject(err, "message", curl_easy_strerror(rc));
        cJSON_AddItemToObject(j, "error", err);
        json_out = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        code_out = 502;
    } else {
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
            json_out = cJSON_PrintUnformatted(e);
            cJSON_Delete(e);
            code_out = 502;
        } else {
            cJSON *anth = convert_openai_response_to_anthropic(job->upstream_body.ptr, job->client_model);
            if (!anth) {
                log_msg("ERROR", "CONV_FAIL model=%s upstream_body=%.*s", job->client_model, (int)(job->upstream_body.len > 500 ? 500 : job->upstream_body.len), job->upstream_body.ptr ? job->upstream_body.ptr : "");
                cJSON *j = cJSON_CreateObject();
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "conversion_error");
                cJSON_AddStringToObject(err, "message", "failed to convert upstream response");
                cJSON_AddItemToObject(j, "error", err);
                json_out = cJSON_PrintUnformatted(j);
                cJSON_Delete(j);
                code_out = 502;
            } else {
                log_msg("INFO", "RESP_OK model=%s", job->client_model);
                json_out = cJSON_PrintUnformatted(anth);
                cJSON *usage = cJSON_GetObjectItemCaseSensitive(anth, "usage");
                if (usage) {
                    input_tokens = (long)json_get_long(usage, "input_tokens", 0);
                    output_tokens = (long)json_get_long(usage, "output_tokens", 0);
                }
                cJSON_Delete(anth);
                code_out = 200;
            }
        }
    }
    if (json_out) {
        size_t jl = strlen(json_out);
        if (jl > 4096) jl = 4096;
        log_msg("DEBUG", "RESP model=%s code=%d %.*s", job->client_model, code_out, (int)jl, json_out);
    }
    pthread_mutex_lock(&job->send_mu);
    job->nonstream_code = code_out;
    job->nonstream_json = json_out;
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
    job->response_sent = true;

    rt_print("[RES] model=%s type=nonstream status=%d upstream_status=%ld prompt_tokens=%ld completion_tokens=%ld",
        job->client_model, code_out, job->upstream_status, input_tokens, output_tokens);
    if (json_out) {
        rt_print_json("[RES_BODY]", json_out);
    }
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double latency_ms = (end_time.tv_sec - job->start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - job->start_time.tv_nsec) / 1000000.0;
    stats_request_end(job->client_model, false, code_out == 200 ? 200 : (job->upstream_status > 0 ? (int)job->upstream_status : 502), 
                      rc, job->upstream_body.len, input_tokens, output_tokens, latency_ms);
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
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double latency_ms = (end_time.tv_sec - job->start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - job->start_time.tv_nsec) / 1000000.0;
    stats_request_end(job->client_model, true, 
                      (rc == CURLE_OK && job->upstream_status < 400) ? 200 : (job->upstream_status > 0 ? (int)job->upstream_status : 502),
                      rc, job->upstream_body.len, 
                      job->stream_state.prompt_tokens, job->stream_state.completion_tokens, latency_ms);
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
                    event_base_once(BASE, -1, EV_TIMEOUT, deferred_job_free, job, NULL);
                }
            }
        }
    }
    curl_multi_cleanup(w->multi);
    log_msg("INFO", "worker %d stopped", w->id);
    return NULL;
}

void enqueue_job(gateway_job_t *job) {
    worker_t *w = &WORKERS[(RR++) % WORKER_COUNT];
    job->worker = w;
    pthread_mutex_lock(&w->mu);
    if (w->pending_tail) w->pending_tail->next = job;
    else w->pending_head = job;
    w->pending_tail = job;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

void workers_start(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        WORKERS[i].id = i;
        pthread_mutex_init(&WORKERS[i].mu, NULL);
        pthread_cond_init(&WORKERS[i].cv, NULL);
        pthread_create(&WORKERS[i].tid, NULL, worker_loop, &WORKERS[i]);
    }
}

void workers_stop(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        pthread_mutex_lock(&WORKERS[i].mu);
        WORKERS[i].stop = true;
        pthread_cond_signal(&WORKERS[i].cv);
        pthread_mutex_unlock(&WORKERS[i].mu);
    }
    for (int i = 0; i < WORKER_COUNT; i++) pthread_join(WORKERS[i].tid, NULL);
}

void job_free(gateway_job_t *job) {
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
    if (job->client_req && !job->response_sent) {
        if (BASE) {
            event_base_once(BASE, -1, EV_TIMEOUT, deferred_req_free, job->client_req, NULL);
        } else {
            evhttp_request_free(job->client_req);
        }
        job->client_req = NULL;
    }
    job->client_req = NULL;
    pthread_mutex_destroy(&job->send_mu);
    if (job->send_buf) { evbuffer_free(job->send_buf); job->send_buf = NULL; }
    free(job->nonstream_json); job->nonstream_json = NULL;
    free(job);
}