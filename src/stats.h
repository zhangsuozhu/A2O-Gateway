#ifndef STATS_H
#define STATS_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <curl/curl.h>
#include "types.h"

/* 最大模型统计条目数 */
#define MAX_MODEL_STATS 64
/* 时间窗口数量（每小时一个窗口，保留24小时） */
#define MAX_TIME_WINDOWS 24
/* 窗口时长（秒） */
#define WINDOW_SECONDS 3600

/* 滑动窗口速度统计：1 秒粒度，6 个槽位 = 覆盖最近 5 秒 */
#define SLIDING_SLOTS 6

typedef struct {
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t request_bytes;
    uint64_t response_bytes;
} sliding_bucket_t;

typedef struct model_stat_entry {
    char model_name[64];
    char provider[64];
    uint64_t requests;
    uint64_t stream_requests;
    uint64_t nonstream_requests;
    uint64_t success;
    uint64_t failed;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t total_request_bytes;
    uint64_t total_response_bytes;
    double total_latency_ms;
    uint64_t latency_count;
    double min_latency_ms;
    double max_latency_ms;
    double total_input_speed;   /* 每次请求 input_tokens/耗时 的累加 */
    double total_output_speed;  /* 每次请求 output_tokens/耗时 的累加 */
    uint64_t speed_count;       /* 参与速度统计的请求数 */
    uint64_t http_4xx;
    uint64_t http_5xx;
    uint64_t curl_errors;
    /* 提示词缓存 */
    uint64_t cache_read_input_tokens;
    uint64_t cache_creation_input_tokens;
    double   saved_cost_usd;
    double   input_price_per_million;  /* USD / 1M tokens */
    time_t first_seen;
    time_t last_seen;
    sliding_bucket_t sliding[SLIDING_SLOTS];
} model_stat_entry_t;

typedef struct time_window {
    time_t start_time;
    uint64_t requests;
    uint64_t success;
    uint64_t failed;
    uint64_t input_tokens;
    uint64_t output_tokens;
} time_window_t;

typedef struct gateway_stats {
    pthread_mutex_t lock;
    time_t start_time;
    
    /* 全局计数 */
    uint64_t total_requests;
    uint64_t stream_requests;
    uint64_t nonstream_requests;
    uint64_t total_success;
    uint64_t total_failed;
    uint64_t total_input_tokens;
    uint64_t total_output_tokens;
    uint64_t total_request_bytes;
    uint64_t total_response_bytes;
    
    /* 延迟统计 */
    double total_latency_ms;
    uint64_t latency_count;
    double min_latency_ms;
    double max_latency_ms;

    /* 每次请求的 token 速度累加（用于计算平均速度） */
    double total_input_speed;
    double total_output_speed;
    uint64_t speed_count;
    
    /* 错误统计 */
    uint64_t http_4xx_count;
    uint64_t http_5xx_count;
    uint64_t curl_error_count;
    uint64_t upstream_timeout_count;
    
    /* 当前活跃请求 */
    uint64_t active_requests;
    uint64_t peak_active_requests;

    /* 滑动窗口速度统计 */
    sliding_bucket_t global_sliding[SLIDING_SLOTS];
    int sliding_index;
    time_t sliding_last_second;

    /* 模型统计 */
    int model_count;
    model_stat_entry_t models[MAX_MODEL_STATS];
    
    /* 时间窗口 */
    int window_count;
    time_window_t windows[MAX_TIME_WINDOWS];
} gateway_stats_t;

/* 初始化统计模块 */
void stats_init(void);

/* 请求开始 */
void stats_request_begin(const char *model, const char *provider, bool stream, size_t request_bytes);

/* 请求结束 */
void stats_request_end(const char *model, const char *provider, bool stream, int http_status,
                       CURLcode curl_code, size_t request_bytes, size_t response_bytes,
                       long input_tokens, long output_tokens,
                       double latency_ms);

/* 获取统计 JSON（调用者负责 free） */
cJSON *stats_get_json(void);

/* 重置统计 */
void stats_reset(void);

/* ---------- 提示词缓存统计 ---------- */
void        stats_record_cache_read    (const char *model, const char *provider, unsigned long tokens);
void        stats_record_cache_creation(const char *model, const char *provider, unsigned long tokens);
unsigned long stats_get_cache_read    (const char *model, const char *provider);
unsigned long stats_get_cache_creation(const char *model, const char *provider);
double        stats_get_saved_cost    (const char *model, const char *provider);

/* test-only */
void stats_reset_for_test(void);
void stats_set_input_price_for_test(const char *model, double price_per_million);

#endif
