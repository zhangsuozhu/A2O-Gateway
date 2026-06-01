#include "stats.h"
#include "log.h"
#include "db.h"
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
    G_STATS.sliding_last_second = time(NULL);
}

/* 推进滑动窗口环形缓冲区：每秒对应一个槽位，最多 SLIDING_SLOTS 个槽位 */
static void sliding_tick(void) {
    time_t now = time(NULL);
    int diff = (int)(now - G_STATS.sliding_last_second);
    if (diff <= 0) return;
    if (diff > SLIDING_SLOTS) diff = SLIDING_SLOTS;
    G_STATS.sliding_last_second = now;

    for (int i = 0; i < diff; i++) {
        G_STATS.sliding_index = (G_STATS.sliding_index + 1) % SLIDING_SLOTS;
        int idx = G_STATS.sliding_index;
        memset(&G_STATS.global_sliding[idx], 0, sizeof(sliding_bucket_t));
        for (int m = 0; m < G_STATS.model_count; m++) {
            memset(&G_STATS.models[m].sliding[idx], 0, sizeof(sliding_bucket_t));
        }
    }
}

/* 对滑动窗口所有槽位求和 */
static void sum_sliding(const sliding_bucket_t buckets[], uint64_t *in, uint64_t *out,
                        uint64_t *req_b, uint64_t *res_b) {
    *in = *out = *req_b = *res_b = 0;
    for (int i = 0; i < SLIDING_SLOTS; i++) {
        *in += buckets[i].input_tokens;
        *out += buckets[i].output_tokens;
        *req_b += buckets[i].request_bytes;
        *res_b += buckets[i].response_bytes;
    }
}

/* 计算有效滑动窗口秒数 */
static double sliding_window_seconds(double uptime) {
    double sec = (double)(time(NULL) - G_STATS.sliding_last_second + SLIDING_SLOTS);
    if (sec > SLIDING_SLOTS) sec = (double)SLIDING_SLOTS;
    if (sec > uptime) sec = uptime;
    if (sec < 1.0) sec = 1.0;
    return sec;
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

    /* 滑动窗口累计 request_bytes */
    G_STATS.global_sliding[G_STATS.sliding_index].request_bytes += request_bytes;
    entry->sliding[G_STATS.sliding_index].request_bytes += request_bytes;

    pthread_mutex_unlock(&G_STATS.lock);
}

void stats_request_end(const char *model, const char *provider, bool stream, int http_status,
                       CURLcode curl_code, size_t request_bytes, size_t response_bytes,
                       long input_tokens, long output_tokens,
                       long cache_read_input_tokens, long cache_creation_input_tokens,
                       double latency_ms) {
    pthread_mutex_lock(&G_STATS.lock);

    sliding_tick();

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

    /* 每次请求的 token 速度累加 */
    if (latency_ms > 0) {
        double sec = latency_ms / 1000.0;
        double inp_speed = (double)input_tokens / sec;
        double out_speed = (double)output_tokens / sec;
        if (inp_speed > 0 || out_speed > 0) {
            G_STATS.total_input_speed += inp_speed;
            G_STATS.total_output_speed += out_speed;
            G_STATS.speed_count++;
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

    /* 每次请求的 token 速度累加 */
    if (latency_ms > 0) {
        double sec = latency_ms / 1000.0;
        double inp_speed = (double)input_tokens / sec;
        double out_speed = (double)output_tokens / sec;
        if (inp_speed > 0 || out_speed > 0) {
            entry->total_input_speed += inp_speed;
            entry->total_output_speed += out_speed;
            entry->speed_count++;
        }
    }

    /* 滑动窗口累计 */
    int si = G_STATS.sliding_index;
    if (input_tokens > 0) {
        G_STATS.global_sliding[si].input_tokens += (uint64_t)input_tokens;
        entry->sliding[si].input_tokens += (uint64_t)input_tokens;
    }
    if (output_tokens > 0) {
        G_STATS.global_sliding[si].output_tokens += (uint64_t)output_tokens;
        entry->sliding[si].output_tokens += (uint64_t)output_tokens;
    }
    G_STATS.global_sliding[si].response_bytes += response_bytes;
    entry->sliding[si].response_bytes += response_bytes;

    /* 时间窗口 */
    time_window_t *window = get_current_window();
    window->requests++;
    if (input_tokens > 0) window->input_tokens += (uint64_t)input_tokens;
    if (output_tokens > 0) window->output_tokens += (uint64_t)output_tokens;
    if (success) window->success++;
    else window->failed++;

    pthread_mutex_unlock(&G_STATS.lock);

    /* 持久化到 SQLite */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char hour_str[32], day_str[32];
    strftime(hour_str, sizeof(hour_str), "%Y-%m-%d %H:00", tm_info);
    strftime(day_str, sizeof(day_str), "%Y-%m-%d", tm_info);

    db_insert_request(model, provider, stream, http_status, (int)curl_code,
                      input_tokens, output_tokens,
                      cache_read_input_tokens, cache_creation_input_tokens,
                      latency_ms,
                      request_bytes, response_bytes, model, "");
    db_update_hourly_stats(hour_str, model, provider, success, input_tokens, output_tokens,
                           cache_read_input_tokens, cache_creation_input_tokens, latency_ms);
    db_update_daily_stats(day_str, model, provider, success, input_tokens, output_tokens,
                          cache_read_input_tokens, cache_creation_input_tokens, latency_ms);
    db_update_model_stats(model, provider, success, input_tokens, output_tokens,
                          cache_read_input_tokens, cache_creation_input_tokens, latency_ms);
}

static cJSON *model_entry_to_json(const model_stat_entry_t *entry, double window_sec, double uptime) {
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

    /* 每次请求 token 速度的平均值 */
    double avg_in_speed = (entry->speed_count > 0) ? (entry->total_input_speed / entry->speed_count) : 0.0;
    double avg_out_speed = (entry->speed_count > 0) ? (entry->total_output_speed / entry->speed_count) : 0.0;
    cJSON_AddNumberToObject(obj, "avg_input_tokens_per_sec", avg_in_speed);
    cJSON_AddNumberToObject(obj, "avg_output_tokens_per_sec", avg_out_speed);
    cJSON_AddNumberToObject(obj, "http_4xx", (double)entry->http_4xx);
    cJSON_AddNumberToObject(obj, "http_5xx", (double)entry->http_5xx);
    cJSON_AddNumberToObject(obj, "curl_errors", (double)entry->curl_errors);
    /* 提示词缓存 */
    cJSON_AddNumberToObject(obj, "cache_read_input_tokens", (double)entry->cache_read_input_tokens);
    cJSON_AddNumberToObject(obj, "cache_creation_input_tokens", (double)entry->cache_creation_input_tokens);
    {
        /* input_tokens 通常已包含 cache tokens（Anthropic/DeepSeek/OpenAI 均如此），
         * 避免重复累加；若 input_tokens 为 0（无 usage），fallback 到 cache tokens 之和 */
        uint64_t total_in = entry->input_tokens;
        uint64_t cache_total = entry->cache_read_input_tokens + entry->cache_creation_input_tokens;
        if (total_in < cache_total) total_in = cache_total;
        double read_rate = (total_in > 0) ? (double)entry->cache_read_input_tokens / (double)total_in : 0.0;
        double write_rate = (total_in > 0) ? (double)entry->cache_creation_input_tokens / (double)total_in : 0.0;
        cJSON_AddNumberToObject(obj, "cache_read_hit_rate", read_rate);
        cJSON_AddNumberToObject(obj, "cache_write_hit_rate", write_rate);
    }
    cJSON_AddNumberToObject(obj, "first_seen", (double)entry->first_seen);
    cJSON_AddNumberToObject(obj, "last_seen", (double)entry->last_seen);

    /* 基于滑动窗口的 token 速度 (tokens/s) */
    uint64_t si, so, srb, sresb;
    sum_sliding(entry->sliding, &si, &so, &srb, &sresb);
    cJSON_AddNumberToObject(obj, "input_token_speed", window_sec > 0 ? (double)si / window_sec : 0.0);
    cJSON_AddNumberToObject(obj, "output_token_speed", window_sec > 0 ? (double)so / window_sec : 0.0);
    cJSON_AddNumberToObject(obj, "traffic_speed", window_sec > 0 ? (double)(srb + sresb) / window_sec : 0.0);

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

    /* 查询前先推进滑动窗口，确保旧数据被清掉 */
    sliding_tick();

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

    /* 每次请求 token 速度的平均值 */
    double avg_in_speed = (G_STATS.speed_count > 0) ? (G_STATS.total_input_speed / G_STATS.speed_count) : 0.0;
    double avg_out_speed = (G_STATS.speed_count > 0) ? (G_STATS.total_output_speed / G_STATS.speed_count) : 0.0;
    cJSON_AddNumberToObject(overview, "avg_input_tokens_per_sec", avg_in_speed);
    cJSON_AddNumberToObject(overview, "avg_output_tokens_per_sec", avg_out_speed);

    /* 速度统计（基于滑动窗口） */
    double uptime = (double)(time(NULL) - G_STATS.start_time);
    if (uptime < 1.0) uptime = 1.0;
    double window_sec = sliding_window_seconds(uptime);
    uint64_t gsi, gso, gsrb, gsresb;
    sum_sliding(G_STATS.global_sliding, &gsi, &gso, &gsrb, &gsresb);
    cJSON_AddNumberToObject(overview, "input_token_speed", window_sec > 0 ? (double)gsi / window_sec : 0.0);
    cJSON_AddNumberToObject(overview, "output_token_speed", window_sec > 0 ? (double)gso / window_sec : 0.0);
    cJSON_AddNumberToObject(overview, "traffic_speed", window_sec > 0 ? (double)(gsrb + gsresb) / window_sec : 0.0);

    cJSON_AddNumberToObject(overview, "http_4xx", (double)G_STATS.http_4xx_count);
    cJSON_AddNumberToObject(overview, "http_5xx", (double)G_STATS.http_5xx_count);
    cJSON_AddNumberToObject(overview, "curl_errors", (double)G_STATS.curl_error_count);
    cJSON_AddItemToObject(root, "overview", overview);
    
    /* 模型统计 */
    cJSON *models = cJSON_CreateArray();
    for (int i = 0; i < G_STATS.model_count; i++) {
        cJSON_AddItemToArray(models, model_entry_to_json(&G_STATS.models[i], window_sec, uptime));
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
    G_STATS.sliding_last_second = time(NULL);
    pthread_mutex_unlock(&G_STATS.lock);
    log_msg("INFO", "stats reset");
}

/* ====================================================================== */
/*  提示词缓存统计                                                       */
/* ====================================================================== */

/* 缓存读：上游按 0.1x 计费 → 节省 0.9x
 * 缓存写：上游按 1.25x 计费 → 净亏 0.25x
 * 阈值常量显式写出，便于调参 */
static const double CACHE_READ_DISCOUNT  = 0.9;
static const double CACHE_WRITE_PREMIUM  = 0.25;

/* 锁 + 拿 entry + 出循环时自动解锁。`entry` 是循环内可见的 model_stat_entry_t*。
 * 用法: WITH_STATS_LOCK(e, m, p) { e->field = ...; }
 * 实现：单趟 for 循环，init 段取锁 + 拿 entry，loop-cond 段判 entry 非 NULL，
 *       post 段把 entry 置 NULL 并解锁。break 会跳过 unlock（避免死锁），但
 *       所有现有调用者都用大括号包住，没有 break。 */
#define WITH_STATS_LOCK(entry, model, provider)                                          \
    for (model_stat_entry_t *entry =                                                    \
             (pthread_mutex_lock(&G_STATS.lock), get_model_entry((model), (provider))); \
         entry;                                                                          \
         entry = NULL, pthread_mutex_unlock(&G_STATS.lock))

void stats_record_cache_read(const char *model, const char *provider, unsigned long tokens) {
    WITH_STATS_LOCK(e, model, provider) {
        e->cache_read_input_tokens += tokens;
        double per_token = e->input_price_per_million / 1e6;
        e->saved_cost_usd += (double)tokens * per_token * CACHE_READ_DISCOUNT;
    }
}

void stats_record_cache_creation(const char *model, const char *provider, unsigned long tokens) {
    WITH_STATS_LOCK(e, model, provider) {
        e->cache_creation_input_tokens += tokens;
        double per_token = e->input_price_per_million / 1e6;
        e->saved_cost_usd -= (double)tokens * per_token * CACHE_WRITE_PREMIUM;
    }
}

unsigned long stats_get_cache_read(const char *model, const char *provider) {
    unsigned long v = 0;
    WITH_STATS_LOCK(e, model, provider) {
        v = (unsigned long)e->cache_read_input_tokens;
    }
    return v;
}

unsigned long stats_get_cache_creation(const char *model, const char *provider) {
    unsigned long v = 0;
    WITH_STATS_LOCK(e, model, provider) {
        v = (unsigned long)e->cache_creation_input_tokens;
    }
    return v;
}

double stats_get_saved_cost(const char *model, const char *provider) {
    double v = 0.0;
    WITH_STATS_LOCK(e, model, provider) {
        v = e->saved_cost_usd;
    }
    return v;
}

void stats_reset_for_test(void) {
    pthread_mutex_lock(&G_STATS.lock);
    memset(&G_STATS, 0, sizeof(G_STATS));
    G_STATS.start_time = time(NULL);
    G_STATS.min_latency_ms = -1.0;
    G_STATS.sliding_last_second = time(NULL);
    for (int i = 0; i < MAX_MODEL_STATS; i++) G_STATS.models[i].min_latency_ms = -1.0;
    pthread_mutex_unlock(&G_STATS.lock);
}

void stats_set_input_price_for_test(const char *model, double price_per_million) {
    pthread_mutex_lock(&G_STATS.lock);
    model_stat_entry_t *e = get_model_entry(model, "test");
    e->input_price_per_million = price_per_million;
    pthread_mutex_unlock(&G_STATS.lock);
}
