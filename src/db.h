#ifndef DB_H
#define DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include "cJSON.h"

/* 初始化数据库连接，建表（如果不存在）
 * db_path: 数据库文件路径，如 "data/gateway.db"
 * 返回: true=成功, false=失败
 */
bool db_init(const char *db_path);

/* 关闭数据库连接 */
void db_close(void);

/* 写入单条请求记录 */
bool db_insert_request(const char *model, const char *provider, bool stream,
                       int http_status, int curl_code,
                       long input_tokens, long output_tokens,
                       double latency_ms, size_t request_bytes, size_t response_bytes,
                       const char *client_model, const char *upstream_url);

/* 更新小时聚合统计（UPSERT） */
bool db_update_hourly_stats(const char *hour, const char *model, const char *provider,
                            bool success, long input_tokens, long output_tokens,
                            double latency_ms);

/* 更新日聚合统计（UPSERT） */
bool db_update_daily_stats(const char *day, const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           double latency_ms);

/* 更新模型累计统计（UPSERT） */
bool db_update_model_stats(const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           double latency_ms);

/* 查询请求历史
 * model: 模型名过滤（NULL 或空字符串表示不过滤）
 * from/to: Unix 时间戳范围（0 表示不过滤）
 * limit: 最大返回条数
 * offset: 偏移量
 * 返回: cJSON 数组（调用者负责 free），失败返回 NULL
 */
cJSON *db_query_history(const char *model, time_t from, time_t to, int limit, int offset);

/* 查询小时聚合统计 */
cJSON *db_query_hourly(const char *model, const char *from_hour, const char *to_hour);

/* 查询日聚合统计 */
cJSON *db_query_daily(const char *model, const char *from_day, const char *to_day);

/* 清理过期数据
 * request_retention_days: requests 表保留天数
 * hourly_retention_days: hourly_stats 表保留天数
 */
bool db_cleanup_old_data(int request_retention_days, int hourly_retention_days);

#endif
