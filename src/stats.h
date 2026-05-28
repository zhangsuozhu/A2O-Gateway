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

typedef struct model_stat_entry {
    char model_name[64];
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
    uint64_t http_4xx;
    uint64_t http_5xx;
    uint64_t curl_errors;
    time_t first_seen;
    time_t last_seen;
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
    
    /* 错误统计 */
    uint64_t http_4xx_count;
    uint64_t http_5xx_count;
    uint64_t curl_error_count;
    uint64_t upstream_timeout_count;
    
    /* 当前活跃请求 */
    uint64_t active_requests;
    uint64_t peak_active_requests;
    
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
void stats_request_begin(const char *model, bool stream, size_t request_bytes);

/* 请求结束 */
void stats_request_end(const char *model, bool stream, int http_status, 
                       CURLcode curl_code, size_t response_bytes,
                       long input_tokens, long output_tokens,
                       double latency_ms);

/* 获取统计 JSON（调用者负责 free） */
cJSON *stats_get_json(void);

/* 重置统计 */
void stats_reset(void);

#endif
