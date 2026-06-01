/**
 * @file worker.c
 * @brief libcurl multi 工作线程池实现
 *
 * 每个 worker 线程维护一个独立的 CURLM *multi handle，
 * 通过 curl_multi_perform + curl_multi_poll 驱动异步 I/O。
 * 任务以 gateway_job_t 链表形式挂到 worker 的 pending 队列。
 * worker_loop 不断：
 *   1. 从 pending 队列取出新任务，用 curl_easy_init 创建 handle 并配置；
 *   2. 调用 curl_multi_perform 推进所有活跃传输；
 *   3. 用 curl_multi_poll 等待网络事件；
 *   4. 通过 curl_multi_info_read 读取已完成的传输结果，
 *      根据 stream / non-stream 分别调用 complete_stream_job 或 complete_nonstream_job；
 *   5. 将任务通过 event_base_once 投递回 libevent 主线程进行释放或发送响应。
 *
 * 这样 libevent HTTP 线程只负责接收请求和发送响应，
 * 所有上游网络阻塞操作都由 worker 线程承担，避免阻塞主事件循环。
 */

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

/**
 * @brief 延迟释放 evhttp_request 的回调函数
 * @param fd   libevent 套接字（未使用）
 * @param what 事件标志（未使用）
 * @param arg  指向 struct evhttp_request 的指针
 *
 * evhttp_request 属于 libevent 对象，必须在 libevent 主线程释放。
 * 因此 worker 线程通过 event_base_once 将该函数投递到 BASE 事件循环，
 * 在主线程中安全调用 evhttp_request_free。
 */
static void deferred_req_free(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    struct evhttp_request *req = (struct evhttp_request *)arg;
    if (req) evhttp_request_free(req);
}

/**
 * @brief 延迟释放 gateway_job_t 的回调函数
 * @param fd   libevent 套接字（未使用）
 * @param what 事件标志（未使用）
 * @param arg  指向 gateway_job_t 的指针
 *
 * job_free 内部会释放 client_req、send_buf 等 libevent 相关对象，
 * 这些操作必须在 libevent 主线程完成。
 * 因此 worker 线程通过 event_base_once 将本回调投递到 BASE，
 * 由主线程执行实际的资源释放。
 */
static void deferred_job_free(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    gateway_job_t *job = arg;
    job_free(job);
}

/**
 * @brief 处理非流式请求完成后的结果转换与响应
 * @param job 指向当前 gateway_job_t 任务
 * @param rc  libcurl 传输结果码（CURLE_OK 表示成功）
 *
 * 该函数在 worker 线程中执行，负责将上游 OpenAI 格式的 HTTP 响应
 * 转换为 Anthropic Messages API 格式，并把结果写回 job->nonstream_json。
 * 处理流程：
 *   1. 若 rc != CURLE_OK，表示网络层失败，构造 502 upstream_error；
 *   2. 若 rc == CURLE_OK 但 HTTP status 不在 [200,300)，同样返回 502，
 *      并将上游错误 body 透传；
 *   3. 若 HTTP 成功，调用 convert_openai_response_to_anthropic 做协议转换，
 *      提取 usage 中的 input_tokens / output_tokens；
 *   4. 转换成功后，通过 event_base_once 将 deferred_send 投递到 BASE，
 *      由 libevent 主线程把 nonstream_json 发送回客户端；
 *   5. 调用 rt_print / rt_print_json 输出实时日志。
 *
 * 注意：本函数直接操作 job->nonstream_json / nonstream_code，
 *       并通过 send_mu 互斥锁保护写入；最终发送动作发生在主线程。
 */
static void complete_nonstream_job(gateway_job_t *job, CURLcode rc) {
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    double worker_total_ms = (ts0.tv_sec - job->start_time.tv_sec) * 1000.0 + (ts0.tv_nsec - job->start_time.tv_nsec) / 1000000.0;
    log_msg("INFO", "WORKER_RECV model=%s worker_latency=%.2fms", job->client_model, worker_total_ms);
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
            if (job->passthrough) {
                /* 透传模式：上游响应已是 Anthropic 格式，无需协议转换 */
                cJSON *anth = cJSON_Parse(job->upstream_body.ptr ? job->upstream_body.ptr : "");
                if (!anth) {
                    log_msg("ERROR", "PASSTHROUGH_PARSE_FAIL model=%s upstream_body=%.*s", job->client_model, (int)(job->upstream_body.len > 500 ? 500 : job->upstream_body.len), job->upstream_body.ptr ? job->upstream_body.ptr : "");
                    cJSON *j = cJSON_CreateObject();
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "conversion_error");
                    cJSON_AddStringToObject(err, "message", "failed to parse passthrough response");
                    cJSON_AddItemToObject(j, "error", err);
                    json_out = cJSON_PrintUnformatted(j);
                    cJSON_Delete(j);
                    code_out = 502;
                } else {
                    /* 检测上游业务错误 */
                    cJSON *upstream_err = cJSON_GetObjectItemCaseSensitive(anth, "error");
                    if (upstream_err) {
                        log_msg("ERROR", "PASSTHROUGH_UPSTREAM_ERR model=%s", job->client_model);
                        json_out = cJSON_PrintUnformatted(anth);
                        cJSON_Delete(anth);
                        code_out = 502;
                    } else {
                        log_msg("INFO", "RESP_OK model=%s (passthrough)", job->client_model);
                        json_out = cJSON_PrintUnformatted(anth);
                        cJSON *usage = cJSON_GetObjectItemCaseSensitive(anth, "usage");
                        if (usage) {
                            input_tokens = (long)json_get_long(usage, "input_tokens", 0);
                            output_tokens = (long)json_get_long(usage, "output_tokens", 0);
                            /* 缓存：兼容 Anthropic / DeepSeek / OpenAI / Moonshot 格式 */
                            long cr = json_get_long(usage, "cache_read_input_tokens", 0);
                            if (cr == 0) cr = json_get_long(usage, "prompt_cache_hit_tokens", 0);
                            if (cr == 0) {
                                cJSON *d = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
                                if (cJSON_IsObject(d)) {
                                    cr = json_get_long(d, "cached_tokens", 0);
                                }
                            }
                            long cc = json_get_long(usage, "cache_creation_input_tokens", 0);
                            if (cc == 0) cc = json_get_long(usage, "prompt_cache_miss_tokens", 0);
                            job->stream_state.cache_read_input_tokens = cr;
                            job->stream_state.cache_creation_input_tokens = cc;
                            const char *m = job->upstream_model ? job->upstream_model : job->client_model;
                            const char *p = job->provider_name ? job->provider_name : "unknown";
                            if (cr > 0) stats_record_cache_read(m, p, (unsigned long)cr);
                            if (cc > 0) stats_record_cache_creation(m, p, (unsigned long)cc);
                        }
                        cJSON_Delete(anth);
                        code_out = 200;
                    }
                }
            } else {
            /* 非流式 / 非透传：直接从原始 OpenAI 响应体中提取 usage 和缓存 token，
             * 不经过 convert_openai_response_to_anthropic 二次转换，避免信息丢失 */
            if (job->upstream_body.ptr) {
                cJSON *raw = cJSON_Parse(job->upstream_body.ptr);
                if (cJSON_IsObject(raw)) {
                    cJSON *u = cJSON_GetObjectItemCaseSensitive(raw, "usage");
                    if (cJSON_IsObject(u)) {
                        long pt = (long)json_get_long(u, "prompt_tokens", 0);
                        output_tokens = (long)json_get_long(u, "completion_tokens", 0);
                        long cr = json_get_long(u, "cache_read_input_tokens", 0);
                        if (cr == 0) cr = json_get_long(u, "prompt_cache_hit_tokens", 0);
                        if (cr == 0) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens_details");
                            if (cJSON_IsObject(d)) {
                                cr = json_get_long(d, "cached_tokens", 0);
                            }
                        }
                        long cc = json_get_long(u, "cache_creation_input_tokens", 0);
                        if (cc == 0) cc = json_get_long(u, "prompt_cache_miss_tokens", 0);
                        /* provider 的 prompt_tokens 不包含缓存 tokens，需合并 */
                        log_msg("DEBUG", "CACHE_FIX model=%s includes=%s pt_before=%ld cr=%ld cc=%ld",
                                job->client_model, job->prompt_tokens_includes_cache ? "true" : "false", pt, cr, cc);
                        if (!job->prompt_tokens_includes_cache && pt > 0) {
                            pt = pt + cr + cc;
                            log_msg("DEBUG", "CACHE_FIX model=%s pt_after=%ld", job->client_model, pt);
                        }
                        input_tokens = pt;
                        job->stream_state.cache_read_input_tokens = cr;
                        job->stream_state.cache_creation_input_tokens = cc;
                        const char *m = job->upstream_model ? job->upstream_model : job->client_model;
                        const char *p = job->provider_name ? job->provider_name : "unknown";
                        if (cr > 0) stats_record_cache_read(m, p, (unsigned long)cr);
                        if (cc > 0) stats_record_cache_creation(m, p, (unsigned long)cc);
                    }
                }
                cJSON_Delete(raw);
            }
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
                bool has_error = false;
                const char *resp_type = json_get_str(anth, "type");
                if (resp_type && strcmp(resp_type, "error") == 0) has_error = true;
                if (has_error) {
                    log_msg("ERROR", "UPSTREAM_ERR model=%s upstream_body=%.*s", job->client_model, (int)(job->upstream_body.len > 500 ? 500 : job->upstream_body.len), job->upstream_body.ptr ? job->upstream_body.ptr : "");
                    json_out = cJSON_PrintUnformatted(anth);
                    cJSON_Delete(anth);
                    code_out = 502;
                } else {
                    log_msg("INFO", "RESP_OK model=%s", job->client_model);
                    json_out = cJSON_PrintUnformatted(anth);
                    cJSON_Delete(anth);
                    code_out = 200;
                }
            }
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

    {
        struct timespec ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        double resp_ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000.0;
        log_msg("INFO", "WORKER_DONE model=%s resp_process=%.2fms", job->client_model, resp_ms);
    }

    rt_print("[RES] model=%s type=nonstream status=%d upstream_status=%ld prompt_tokens=%ld completion_tokens=%ld",
        job->client_model, code_out, job->upstream_status, input_tokens, output_tokens);
    if (json_out) {
        rt_print_json("[RES_BODY]", json_out);
    }

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double latency_ms = (end_time.tv_sec - job->start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - job->start_time.tv_nsec) / 1000000.0;
    size_t req_bytes = job->request_body ? strlen(job->request_body) : 0;

    stats_request_end(job->upstream_model, job->provider_name, false, code_out == 200 ? 200 : (job->upstream_status > 0 ? (int)job->upstream_status : 502),
                      rc, req_bytes, job->upstream_body.len, input_tokens, output_tokens,
                      job->stream_state.cache_read_input_tokens, job->stream_state.cache_creation_input_tokens,
                      latency_ms);
}

/**
 * @brief 处理流式请求完成后的收尾工作
 * @param job 指向当前 gateway_job_t 任务
 * @param rc  libcurl 传输结果码
 *
 * 流式请求在传输过程中已通过 curl_write_cb 实时解析 SSE 并推送给客户端，
 * 因此本函数仅负责最终的状态确认与错误兜底：
 *   1. 若 stream_state.ended 已为 true，说明之前已正常结束，直接返回；
 *   2. 若 rc != CURLE_OK，记录错误并通过 stream_emit_error 向客户端发送
 *      SSE error 事件；
 *   3. 若上游返回 HTTP >= 400，同样发送 SSE error 事件；
 *   4. 若一切正常，记录 STRM_OK 日志；
 *   5. 最后调用 stream_finish 发送最终 SSE 结束标志（如 message_stop 等）。
 */
static void complete_stream_job(gateway_job_t *job, CURLcode rc) {
    if (job->stream_state.stats_recorded) {
        return;
    }
    if (!job->stream_state.ended) {
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

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double latency_ms = (end_time.tv_sec - job->start_time.tv_sec) * 1000.0 +
                        (end_time.tv_nsec - job->start_time.tv_nsec) / 1000000.0;
    int http_status = (rc == CURLE_OK && job->upstream_status < 400) ? 200 :
                      (job->upstream_status > 0 ? (int)job->upstream_status : 502);
    size_t req_bytes = job->request_body ? strlen(job->request_body) : 0;

    stats_request_end(job->upstream_model, job->provider_name, true, http_status, rc,
                      req_bytes, job->upstream_body.len,
                      job->stream_state.prompt_tokens, job->stream_state.completion_tokens,
                      job->stream_state.cache_read_input_tokens, job->stream_state.cache_creation_input_tokens,
                      latency_ms);
    job->stream_state.stats_recorded = true;
}

/**
 * @brief 为单个任务创建并配置 CURL easy handle，然后加入 multi handle
 * @param w   当前 worker 线程的上下文
 * @param job 待执行的任务
 *
 * 配置项说明：
 *   - CURLOPT_URL: 上游 LLM 服务的完整 URL；
 *   - CURLOPT_POST / POSTFIELDS: 发送 OpenAI 格式的 JSON 请求体；
 *   - CURLOPT_WRITEFUNCTION / WRITEDATA: 将响应体写入 job->upstream_body；
 *   - CURLOPT_HEADERFUNCTION / HEADERDATA: 解析上游响应头；
 *   - CURLOPT_PRIVATE: 将 job 指针绑定到 easy handle，便于完成后找回；
 *   - CURLOPT_NOSIGNAL: 避免在多线程环境下触发 SIGALRM；
 *   - CURLOPT_TCP_KEEPALIVE: 保持 TCP 连接复用；
 *   - CURLOPT_CONNECTTIMEOUT_MS: 连接超时 15 秒；
 *   - CURLOPT_TIMEOUT_MS: 非流式请求 600 秒超时，流式请求不设超时；
 *   - CURLOPT_HTTP_VERSION: 优先使用 HTTP/2（TLS 上）；
 *   - CURLOPT_SSL_VERIFYPEER / VERIFYHOST: 强制校验服务器证书；
 *   - HTTPHEADER: 包含 Content-Type、Accept 以及 Authorization Bearer token。
 */
static void worker_add_easy(worker_t *w, gateway_job_t *job) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    job->job_state = JOB_SENDING;
    job->active_next = NULL;
    if (w->active_tail) w->active_tail->active_next = job;
    else w->active_head = job;
    w->active_tail = job;
    job->easy = curl_easy_init();
    { struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      double ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1000000.0;
      if (ms > 5) log_msg("WARN", "CURL_INIT_SLOW model=%s %.2fms", job->client_model, ms); }
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
    if (job->passthrough) {
        /* Anthropic API 使用 x-api-key 和 anthropic-version 头 */
        if (job->api_key && *job->api_key) {
            char xkey[4096];
            snprintf(xkey, sizeof(xkey), "x-api-key: %s", job->api_key);
            job->headers = curl_slist_append(job->headers, xkey);
        }
        job->headers = curl_slist_append(job->headers, "anthropic-version: 2023-06-01");
        job->headers = curl_slist_append(job->headers, "Accept: application/json, text/event-stream");
    } else {
        job->headers = curl_slist_append(job->headers, "Accept: application/json, text/event-stream");
        if (job->api_key && *job->api_key) {
            char auth[4096];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", job->api_key);
            job->headers = curl_slist_append(job->headers, auth);
        }
    }
    if (job->user_agent && *job->user_agent) {
        char ua[512];
        snprintf(ua, sizeof(ua), "User-Agent: %s", job->user_agent);
        job->headers = curl_slist_append(job->headers, ua);
    }
    curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
    curl_multi_add_handle(w->multi, job->easy);
}

/**
 * @brief 单个 worker 线程的主循环
 * @param arg 指向 worker_t 的指针
 * @return 始终返回 NULL
 *
 * 线程启动后初始化 CURLM multi handle，并设置连接复用与并发上限：
 *   - CURLMOPT_PIPELINING: 启用 HTTP/2 多路复用；
 *   - CURLMOPT_MAX_HOST_CONNECTIONS: 单主机最大 64 条连接；
 *   - CURLMOPT_MAX_TOTAL_CONNECTIONS: 全局最大 256 条连接。
 *
 * 循环体逻辑：
 *   1. 加锁后若 pending 队列为空且没有活跃传输，则 pthread_cond_wait 休眠；
 *   2. 被唤醒后一次性取出整个 pending 链表，解锁；
 *   3. 遍历链表，为每个任务调用 worker_add_easy 加入 multi；
 *   4. curl_multi_perform 推进所有传输，curl_multi_poll 等待网络事件（200ms 超时）；
 *   5. curl_multi_info_read 读取已完成的传输，通过 CURLINFO_PRIVATE 找回 job，
 *      按 stream / non-stream 调用 complete_*，随后用 event_base_once 投递释放；
 *   6. 若 w->stop 为 true，退出循环并 curl_multi_cleanup。
 */
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
                    /* 从活跃列表移除 */
                    pthread_mutex_lock(&w->mu);
                    {
                        gateway_job_t **pp = &w->active_head;
                        while (*pp) {
                            if (*pp == job) {
                                *pp = job->active_next;
                                if (!job->active_next) w->active_tail = NULL;
                                job->active_next = NULL;
                                break;
                            }
                            pp = &(*pp)->active_next;
                        }
                    }
                    pthread_mutex_unlock(&w->mu);
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

/**
 * @brief 将任务以轮询方式加入工作线程队列
 * @param job 已构造好的 gateway_job_t 任务指针
 *
 * 使用原子递增的 RR（round-robin）计数器对 WORKER_COUNT 取模，
 * 选择目标 worker，避免某个 worker 过载。
 * 加锁后将任务挂到 pending 链表尾部，并用 pthread_cond_signal 唤醒
 * 可能正在休眠的 worker 线程。该函数可在任意线程调用（通常由
 * libevent HTTP 请求处理线程调用）。
 */
/* ====================================================================
 * 实时调试信息
 * ==================================================================== */

/**
 * @brief 获取所有 worker 的实时调试信息
 * @return cJSON 数组，每个元素包含一个 worker 的状态（调用者需 cJSON_Delete）
 *
 * 遍历所有 worker，对每个 worker 加锁后收集：
 *   - pending 队列长度及每个待处理任务的模型、已等待时间、是否流式；
 *   - active_jobs：当前正在 curl 中传输的任务数；
 *   - still_running：libcurl multi 报告的活动连接数。
 * 该函数在 libevent 主线程调用（admin API 请求），不会阻塞 worker。
 */
cJSON *workers_get_debug_info(void) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < WORKER_COUNT; i++) {
        cJSON *wobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(wobj, "id", i);
        worker_t *w = &WORKERS[i];
        pthread_mutex_lock(&w->mu);

        /* 遍历 pending 队列 */
        int pending_len = 0;
        cJSON *pending = cJSON_CreateArray();
        gateway_job_t *j = w->pending_head;
        while (j) {
            cJSON *jobj = cJSON_CreateObject();
            cJSON_AddStringToObject(jobj, "model", j->client_model ? j->client_model : "?");
            cJSON_AddStringToObject(jobj, "upstream_model", j->upstream_model ? j->upstream_model : "?");
            cJSON_AddStringToObject(jobj, "provider", j->provider_name ? j->provider_name : "?");
            cJSON_AddBoolToObject(jobj, "stream", j->stream);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double secs = (now.tv_sec - j->start_time.tv_sec) + (now.tv_nsec - j->start_time.tv_nsec) / 1e9;
            cJSON_AddNumberToObject(jobj, "elapsed_ms", secs * 1000.0);
            cJSON_AddItemToArray(pending, jobj);
            pending_len++;
            j = j->next;
        }
        cJSON_AddItemToObject(wobj, "pending", pending);
        cJSON_AddNumberToObject(wobj, "pending_len", pending_len);

        /* 遍历活跃任务，按状态计数 */
        int n_sending = 0, n_waiting = 0, n_receiving = 0;
        cJSON *active = cJSON_CreateArray();
        j = w->active_head;
        while (j) {
            switch (j->job_state) {
                case JOB_SENDING: n_sending++; break;
                case JOB_WAITING: n_waiting++; break;
                case JOB_RECEIVING: n_receiving++; break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double secs = (now.tv_sec - j->start_time.tv_sec) + (now.tv_nsec - j->start_time.tv_nsec) / 1e9;

            cJSON *ajobj = cJSON_CreateObject();
            cJSON_AddStringToObject(ajobj, "model", j->client_model ? j->client_model : "?");
            cJSON_AddStringToObject(ajobj, "upstream_model", j->upstream_model ? j->upstream_model : "?");
            cJSON_AddStringToObject(ajobj, "provider", j->provider_name ? j->provider_name : "?");
            const char *state_str = j->job_state == JOB_SENDING ? "发送中"
                                                  : j->job_state == JOB_WAITING ? "等回复"
                                                                                : "接收中";
            cJSON_AddStringToObject(ajobj, "state", state_str);
            cJSON_AddNumberToObject(ajobj, "elapsed_ms", secs * 1000.0);
            cJSON_AddItemToArray(active, ajobj);
            j = j->active_next;
        }
        cJSON_AddItemToObject(wobj, "active", active);
        cJSON_AddNumberToObject(wobj, "n_sending", n_sending);
        cJSON_AddNumberToObject(wobj, "n_waiting", n_waiting);
        cJSON_AddNumberToObject(wobj, "n_receiving", n_receiving);

        pthread_mutex_unlock(&w->mu);
        cJSON_AddItemToArray(arr, wobj);
    }
    return arr;
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

/**
 * @brief 启动所有工作线程
 *
 * 遍历 WORKERS 数组（长度为 WORKER_COUNT），为每个 worker 设置 id，
 * 初始化互斥锁与条件变量，然后创建 pthread 执行 worker_loop。
 * 创建完成后，所有 worker 即进入等待状态，直到有任务被 enqueue。
 */
void workers_start(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        WORKERS[i].id = i;
        WORKERS[i].active_head = WORKERS[i].active_tail = NULL;
        pthread_mutex_init(&WORKERS[i].mu, NULL);
        pthread_cond_init(&WORKERS[i].cv, NULL);
        pthread_create(&WORKERS[i].tid, NULL, worker_loop, &WORKERS[i]);
    }
}

/**
 * @brief 优雅停止所有工作线程
 *
 * 分两步：
 *   1. 广播阶段：向每个 worker 加锁设置 stop=true，并用 cond_signal 唤醒；
 *   2. 等待阶段：依次 pthread_join，确保线程资源完全回收。
 * 该函数应在 event_base_dispatch 返回后、进程退出前调用。
 */
void workers_stop(void) {
    for (int i = 0; i < WORKER_COUNT; i++) {
        pthread_mutex_lock(&WORKERS[i].mu);
        WORKERS[i].stop = true;
        pthread_cond_signal(&WORKERS[i].cv);
        pthread_mutex_unlock(&WORKERS[i].mu);
    }
    for (int i = 0; i < WORKER_COUNT; i++) pthread_join(WORKERS[i].tid, NULL);
}

/**
 * @brief 彻底释放 gateway_job_t 占用的全部内存与句柄
 * @param job 任务指针（NULL 安全）
 *
 * 释放顺序与注意事项：
 *   1. 释放所有动态分配的字符串字段，并将指针置 NULL，防止重复释放；
 *   2. membuf_free 释放上游响应体缓冲区；
 *   3. stream_state_free 释放 SSE 解析过程中积累的临时文本与 linebuf；
 *   4. curl_slist_free_all 释放 HTTP 请求头链表；
 *   5. curl_easy_cleanup 释放 libcurl easy handle（若尚未从 multi 移除，
 *      调用方应先 curl_multi_remove_handle）；
 *   6. client_req 处理：若尚未发送响应且 BASE 存在，
 *      通过 event_base_once 延迟到主线程释放；否则直接释放；
 *   7. pthread_mutex_destroy 销毁 send_mu；
 *   8. evbuffer_free 释放 send_buf（流式发送缓冲区）；
 *   9. 最后 free(job) 本身。
 */
void job_free(gateway_job_t *job) {
    if (!job) return;
    free(job->request_body); job->request_body = NULL;
    free(job->upstream_url); job->upstream_url = NULL;
    free(job->api_key); job->api_key = NULL;
    free(job->user_agent); job->user_agent = NULL;
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