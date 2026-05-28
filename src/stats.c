#include "stats.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

static gateway_stats_t G_STATS;

/* 获取或创建模型统计条目，按 provider/model 组合作为唯一 key */
static model_stat_entry_t *get_model_entry(const char *model, const char *provider) {
    if (!model || !*model) model = "unknown";
    if (!provider || !*provider) provider = "unknown";

    for (int i = 0; i < G_STATS.model_count; i++) {
        if (strcmp(G_STATS.models[i].model_name, model) == 0 &&
            strcmp(G_STATS.models[i].provider, provider) == 0) {
            return &G_STATS.models[i];
        }
    }

    if (G_STATS.model_count >= MAX_MODEL_STATS) {
        return &G_STATS.models[MAX_MODEL_STATS - 1];
    }

    model_stat_entry_t *entry = &G_STATS.models[G_STATS.model_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->model_name, model, sizeof(entry->model_name) - 1);
    entry->model_name[sizeof(entry->model_name) - 1] = 0;
    strncpy(entry->provider, provider, sizeof(entry->provider) - 1);
    entry->provider[sizeof(entry->provider) - 1] = 0;
    entry->min_latency_ms = -1.0;
    entry->first_seen = time(NULL);
    return entry;
}

/* 获取当前时间窗口 */
static time_window_t *get_current_window(void) {
    time_t now = time(NULL);
    time_t window_start = (now / WINDOW_SECONDS) * WINDOW_SECONDS;
    
    /* 查找现有窗口 */
    for (int i = 0; i < G_STATS.window_count; i++) {
        if (G_STATS.windows[i].start_time == window_start) {
            return &G_STATS.windows[i];
        }
    }
    
    /* 需要新窗口，先滚动 */
    if (G_STATS.window_count >= MAX_TIME_WINDOWS) {
        /* 移除最旧的窗口 */
        memmove(&G_STATS.windows[0], &G_STATS.windows[1], 
                (MAX_TIME_WINDOWS - 1) * sizeof(time_window_t));
        G_STATS.window_count = MAX_TIME_WINDOWS - 1;
    }
    
    time_window_t *window = &G_STATS.windows[G_STATS.window_count++];
    memset(window, 0, sizeof(*window));
    window->start_time = window_start;
    return window;
}

void stats_init(void) {
    memset(&G_STATS, 0, sizeof(G_STATS));
    pthread_mutex_init(&G_STATS.lock, NULL);
    G_STATS.start_time = time(NULL);
    G_STATS.min_latency_ms = -1.0;
}

void stats_request_begin(const char *model, const char *provider, bool stream, size_t request_bytes) {
    pthread_mutex_lock(&G_STATS.lock);

    G_STATS.total_requests++;
    G_STATS.active_requests++;
    if (G_STATS.active_requests > G_STATS.peak_active_requests) {
        G_STATS.peak_active_requests = G_STATS.active_requests;
    }

    if (stream) G_STATS.stream_requests++;
    else G_STATS.nonstream_requests++;

    G_STATS.total_request_bytes += request_bytes;

    model_stat_entry_t *entry = get_model_entry(model, provider);
    entry->requests++;
    entry->last_seen = time(NULL);
    if (stream) entry->stream_requests++;
    else entry->nonstream_requests++;
    entry->total_request_bytes += request_bytes;

    pthread_mutex_unlock(&G_STATS.lock);
}

void stats_request_end(const char *model, const char *provider, bool stream, int http_status,
                       CURLcode curl_code, size_t response_bytes,
                       long input_tokens, long output_tokens,
                       double latency_ms) {
    pthread_mutex_lock(&G_STATS.lock);

    G_STATS.active_requests--;
    G_STATS.total_response_bytes += response_bytes;

    bool success = (curl_code == CURLE_OK && http_status >= 200 && http_status < 300);

    if (success) {
        G_STATS.total_success++;
    } else {
        G_STATS.total_failed++;
        if (http_status >= 400 && http_status < 500) G_STATS.http_4xx_count++;
        if (http_status >= 500) G_STATS.http_5xx_count++;
        if (curl_code != CURLE_OK) G_STATS.curl_error_count++;
    }

    if (input_tokens > 0) G_STATS.total_input_tokens += (uint64_t)input_tokens;
    if (output_tokens > 0) G_STATS.total_output_tokens += (uint64_t)output_tokens;

    /* 延迟统计 */
    if (latency_ms > 0) {
        G_STATS.total_latency_ms += latency_ms;
        G_STATS.latency_count++;
        if (G_STATS.min_latency_ms < 0 || latency_ms < G_STATS.min_latency_ms) {
            G_STATS.min_latency_ms = latency_ms;
        }
        if (latency_ms > G_STATS.max_latency_ms) {
            G_STATS.max_latency_ms = latency_ms;
        }
    }

    /* 模型统计 */
    model_stat_entry_t *entry = get_model_entry(model, provider);
    entry->total_response_bytes += response_bytes;
    entry->last_seen = time(NULL);

    if (success) {
        entry->success++;
    } else {
        entry->failed++;
        if (http_status >= 400 && http_status < 500) entry->http_4xx++;
        if (http_status >= 500) entry->http_5xx++;
        if (curl_code != CURLE_OK) entry->curl_errors++;
    }

    if (input_tokens > 0) entry->input_tokens += (uint64_t)input_tokens;
    if (output_tokens > 0) entry->output_tokens += (uint64_t)output_tokens;

    if (latency_ms > 0) {
        entry->total_latency_ms += latency_ms;
        entry->latency_count++;
        if (entry->min_latency_ms < 0 || latency_ms < entry->min_latency_ms) {
            entry->min_latency_ms = latency_ms;
        }
        if (latency_ms > entry->max_latency_ms) {
            entry->max_latency_ms = latency_ms;
        }
    }

    /* 时间窗口 */
    time_window_t *window = get_current_window();
    window->requests++;
    if (input_tokens > 0) window->input_tokens += (uint64_t)input_tokens;
    if (output_tokens > 0) window->output_tokens += (uint64_t)output_tokens;
    if (success) window->success++;
    else window->failed++;

    pthread_mutex_unlock(&G_STATS.lock);
}

static cJSON *model_entry_to_json(const model_stat_entry_t *entry, double uptime) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "model", entry->model_name);
    cJSON_AddStringToObject(obj, "provider", entry->provider);
    cJSON_AddNumberToObject(obj, "requests", (double)entry->requests);
    cJSON_AddNumberToObject(obj, "stream_requests", (double)entry->stream_requests);
    cJSON_AddNumberToObject(obj, "nonstream_requests", (double)entry->nonstream_requests);
    cJSON_AddNumberToObject(obj, "success", (double)entry->success);
    cJSON_AddNumberToObject(obj, "failed", (double)entry->failed);
    cJSON_AddNumberToObject(obj, "input_tokens", (double)entry->input_tokens);
    cJSON_AddNumberToObject(obj, "output_tokens", (double)entry->output_tokens);
    cJSON_AddNumberToObject(obj, "total_tokens", (double)(entry->input_tokens + entry->output_tokens));
    cJSON_AddNumberToObject(obj, "request_bytes", (double)entry->total_request_bytes);
    cJSON_AddNumberToObject(obj, "response_bytes", (double)entry->total_response_bytes);
    cJSON_AddNumberToObject(obj, "total_bytes", (double)(entry->total_request_bytes + entry->total_response_bytes));
    
    double avg_latency = (entry->latency_count > 0) ? (entry->total_latency_ms / entry->latency_count) : 0.0;
    cJSON_AddNumberToObject(obj, "avg_latency_ms", avg_latency);
    cJSON_AddNumberToObject(obj, "min_latency_ms", entry->min_latency_ms >= 0 ? entry->min_latency_ms : 0.0);
    cJSON_AddNumberToObject(obj, "max_latency_ms", entry->max_latency_ms);
    cJSON_AddNumberToObject(obj, "http_4xx", (double)entry->http_4xx);
    cJSON_AddNumberToObject(obj, "http_5xx", (double)entry->http_5xx);
    cJSON_AddNumberToObject(obj, "curl_errors", (double)entry->curl_errors);
    cJSON_AddNumberToObject(obj, "first_seen", (double)entry->first_seen);
    cJSON_AddNumberToObject(obj, "last_seen", (double)entry->last_seen);

    /* 每个模型的 token 速度 (tokens/s) */
    cJSON_AddNumberToObject(obj, "input_token_speed", uptime > 0 ? (double)entry->input_tokens / uptime : 0.0);
    cJSON_AddNumberToObject(obj, "output_token_speed", uptime > 0 ? (double)entry->output_tokens / uptime : 0.0);
    cJSON_AddNumberToObject(obj, "traffic_speed", uptime > 0 ? (double)(entry->total_request_bytes + entry->total_response_bytes) / uptime : 0.0);

    return obj;
}

static cJSON *window_to_json(const time_window_t *window) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "start_time", (double)window->start_time);
    
    /* 生成可读的时段标签 */
    char hour_buf[32];
    time_t t = window->start_time;
    struct tm *tm_info = localtime(&t);
    strftime(hour_buf, sizeof(hour_buf), "%m-%d %H:00", tm_info);
    cJSON_AddStringToObject(obj, "hour", hour_buf);
    
    cJSON_AddNumberToObject(obj, "requests", (double)window->requests);
    cJSON_AddNumberToObject(obj, "success", (double)window->success);
    cJSON_AddNumberToObject(obj, "failed", (double)window->failed);
    cJSON_AddNumberToObject(obj, "input_tokens", (double)window->input_tokens);
    cJSON_AddNumberToObject(obj, "output_tokens", (double)window->output_tokens);
    return obj;
}

cJSON *stats_get_json(void) {
    pthread_mutex_lock(&G_STATS.lock);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "start_time", (double)G_STATS.start_time);
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(time(NULL) - G_STATS.start_time));
    
    /* 全局概览 */
    cJSON *overview = cJSON_CreateObject();
    cJSON_AddNumberToObject(overview, "total_requests", (double)G_STATS.total_requests);
    cJSON_AddNumberToObject(overview, "active_requests", (double)G_STATS.active_requests);
    cJSON_AddNumberToObject(overview, "peak_active_requests", (double)G_STATS.peak_active_requests);
    cJSON_AddNumberToObject(overview, "stream_requests", (double)G_STATS.stream_requests);
    cJSON_AddNumberToObject(overview, "nonstream_requests", (double)G_STATS.nonstream_requests);
    cJSON_AddNumberToObject(overview, "success", (double)G_STATS.total_success);
    cJSON_AddNumberToObject(overview, "failed", (double)G_STATS.total_failed);
    cJSON_AddNumberToObject(overview, "success_rate", 
        G_STATS.total_requests > 0 ? (double)G_STATS.total_success / G_STATS.total_requests : 0.0);
    cJSON_AddNumberToObject(overview, "total_input_tokens", (double)G_STATS.total_input_tokens);
    cJSON_AddNumberToObject(overview, "total_output_tokens", (double)G_STATS.total_output_tokens);
    cJSON_AddNumberToObject(overview, "total_tokens", (double)(G_STATS.total_input_tokens + G_STATS.total_output_tokens));
    cJSON_AddNumberToObject(overview, "total_request_bytes", (double)G_STATS.total_request_bytes);
    cJSON_AddNumberToObject(overview, "total_response_bytes", (double)G_STATS.total_response_bytes);
    cJSON_AddNumberToObject(overview, "total_bytes", (double)(G_STATS.total_request_bytes + G_STATS.total_response_bytes));
    
    double avg_latency = (G_STATS.latency_count > 0) ? (G_STATS.total_latency_ms / G_STATS.latency_count) : 0.0;
    cJSON_AddNumberToObject(overview, "avg_latency_ms", avg_latency);
    cJSON_AddNumberToObject(overview, "min_latency_ms", G_STATS.min_latency_ms >= 0 ? G_STATS.min_latency_ms : 0.0);
    cJSON_AddNumberToObject(overview, "max_latency_ms", G_STATS.max_latency_ms);

    /* 速度统计 */
    double uptime = (double)(time(NULL) - G_STATS.start_time);
    if (uptime < 1.0) uptime = 1.0;
    cJSON_AddNumberToObject(overview, "input_token_speed", (double)G_STATS.total_input_tokens / uptime);
    cJSON_AddNumberToObject(overview, "output_token_speed", (double)G_STATS.total_output_tokens / uptime);
    cJSON_AddNumberToObject(overview, "traffic_speed", (double)(G_STATS.total_request_bytes + G_STATS.total_response_bytes) / uptime);

    cJSON_AddNumberToObject(overview, "http_4xx", (double)G_STATS.http_4xx_count);
    cJSON_AddNumberToObject(overview, "http_5xx", (double)G_STATS.http_5xx_count);
    cJSON_AddNumberToObject(overview, "curl_errors", (double)G_STATS.curl_error_count);
    cJSON_AddItemToObject(root, "overview", overview);
    
    /* 模型统计 */
    cJSON *models = cJSON_CreateArray();
    for (int i = 0; i < G_STATS.model_count; i++) {
        cJSON_AddItemToArray(models, model_entry_to_json(&G_STATS.models[i], uptime));
    }
    cJSON_AddItemToObject(root, "models", models);
    
    /* 时间窗口 */
    cJSON *windows = cJSON_CreateArray();
    for (int i = 0; i < G_STATS.window_count; i++) {
        cJSON_AddItemToArray(windows, window_to_json(&G_STATS.windows[i]));
    }
    cJSON_AddItemToObject(root, "windows", windows);
    
    pthread_mutex_unlock(&G_STATS.lock);
    return root;
}

void stats_reset(void) {
    pthread_mutex_lock(&G_STATS.lock);
    memset(&G_STATS, 0, sizeof(G_STATS));
    G_STATS.start_time = time(NULL);
    G_STATS.min_latency_ms = -1.0;
    pthread_mutex_unlock(&G_STATS.lock);
    log_msg("INFO", "stats reset");
}
