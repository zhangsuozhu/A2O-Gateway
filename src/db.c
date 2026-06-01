#include "db.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static sqlite3 *g_db = NULL;
static char *g_db_path = NULL;

/* 建表 SQL */
static const char *CREATE_TABLES_SQL =
    "CREATE TABLE IF NOT EXISTS requests ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    timestamp INTEGER NOT NULL,"
    "    model TEXT NOT NULL,"
    "    provider TEXT NOT NULL,"
    "    stream INTEGER NOT NULL DEFAULT 0,"
    "    http_status INTEGER,"
    "    curl_code INTEGER,"
    "    input_tokens INTEGER DEFAULT 0,"
    "    output_tokens INTEGER DEFAULT 0,"
    "    latency_ms REAL,"
    "    request_bytes INTEGER DEFAULT 0,"
    "    response_bytes INTEGER DEFAULT 0,"
    "    client_model TEXT,"
    "    upstream_url TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_req_time ON requests(timestamp);"
    "CREATE INDEX IF NOT EXISTS idx_req_model ON requests(model);"

    "CREATE TABLE IF NOT EXISTS hourly_stats ("
    "    hour TEXT NOT NULL,"
    "    model TEXT NOT NULL,"
    "    provider TEXT NOT NULL,"
    "    requests INTEGER DEFAULT 0,"
    "    success INTEGER DEFAULT 0,"
    "    failed INTEGER DEFAULT 0,"
    "    input_tokens INTEGER DEFAULT 0,"
    "    output_tokens INTEGER DEFAULT 0,"
    "    total_latency_ms REAL DEFAULT 0,"
    "    latency_count INTEGER DEFAULT 0,"
    "    PRIMARY KEY (hour, model, provider)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_hourly_time ON hourly_stats(hour);"

    "CREATE TABLE IF NOT EXISTS daily_stats ("
    "    day TEXT NOT NULL,"
    "    model TEXT NOT NULL,"
    "    provider TEXT NOT NULL,"
    "    requests INTEGER DEFAULT 0,"
    "    success INTEGER DEFAULT 0,"
    "    failed INTEGER DEFAULT 0,"
    "    input_tokens INTEGER DEFAULT 0,"
    "    output_tokens INTEGER DEFAULT 0,"
    "    total_latency_ms REAL DEFAULT 0,"
    "    latency_count INTEGER DEFAULT 0,"
    "    PRIMARY KEY (day, model, provider)"
    ");"

    "CREATE TABLE IF NOT EXISTS model_stats ("
    "    model TEXT NOT NULL PRIMARY KEY,"
    "    provider TEXT NOT NULL,"
    "    total_requests INTEGER DEFAULT 0,"
    "    total_success INTEGER DEFAULT 0,"
    "    total_failed INTEGER DEFAULT 0,"
    "    total_input_tokens INTEGER DEFAULT 0,"
    "    total_output_tokens INTEGER DEFAULT 0,"
    "    total_latency_ms REAL DEFAULT 0,"
    "    latency_count INTEGER DEFAULT 0,"
    "    min_latency_ms REAL,"
    "    max_latency_ms REAL,"
    "    first_seen INTEGER,"
    "    last_seen INTEGER"
    ");"
    ;

bool db_init(const char *db_path) {
    if (g_db) return true;

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        log_msg("ERROR", "db_init failed: %s", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }

    /* 启用 WAL 模式提升并发性能 */
    char *err = NULL;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
    if (err) sqlite3_free(err);

    /* 建表 */
    rc = sqlite3_exec(g_db, CREATE_TABLES_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        log_msg("ERROR", "db_create_tables failed: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }

    free(g_db_path);
    g_db_path = db_path ? strdup(db_path) : strdup("gateway.db");
    log_msg("INFO", "db_init ok: %s", g_db_path);
    return true;
}

bool db_reset(void) {
    if (!g_db_path) return false;
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    unlink(g_db_path);
    log_msg("INFO", "db_reset: removed %s", g_db_path);
    return db_init(g_db_path);
}

void db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        log_msg("INFO", "db_close");
    }
}

bool db_insert_request(const char *model, const char *provider, bool stream,
                       int http_status, int curl_code,
                       long input_tokens, long output_tokens,
                       double latency_ms, size_t request_bytes, size_t response_bytes,
                       const char *client_model, const char *upstream_url) {
    if (!g_db) return false;

    const char *sql = "INSERT INTO requests"
        " (timestamp, model, provider, stream, http_status, curl_code,"
        " input_tokens, output_tokens, latency_ms, request_bytes, response_bytes,"
        " client_model, upstream_url)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_insert_request prepare: %s", sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 2, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, provider ? provider : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, stream ? 1 : 0);
    sqlite3_bind_int(stmt, 5, http_status);
    sqlite3_bind_int(stmt, 6, curl_code);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)input_tokens);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)output_tokens);
    sqlite3_bind_double(stmt, 9, latency_ms);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)request_bytes);
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)response_bytes);
    sqlite3_bind_text(stmt, 12, client_model ? client_model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, upstream_url ? upstream_url : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        log_msg("ERROR", "db_insert_request step: %s", sqlite3_errmsg(g_db));
        return false;
    }
    return true;
}

bool db_update_hourly_stats(const char *hour, const char *model, const char *provider,
                            bool success, long input_tokens, long output_tokens,
                            double latency_ms) {
    if (!g_db) return false;

    const char *sql =
        "INSERT INTO hourly_stats (hour, model, provider, requests, success, failed,"
        " input_tokens, output_tokens, total_latency_ms, latency_count)"
        " VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(hour, model, provider) DO UPDATE SET"
        " requests = requests + 1,"
        " success = success + excluded.success,"
        " failed = failed + excluded.failed,"
        " input_tokens = input_tokens + excluded.input_tokens,"
        " output_tokens = output_tokens + excluded.output_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_update_hourly prepare: %s", sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, hour ? hour : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, provider ? provider : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, success ? 1 : 0);
    sqlite3_bind_int(stmt, 5, success ? 0 : 1);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)input_tokens);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)output_tokens);
    sqlite3_bind_double(stmt, 8, latency_ms > 0 ? latency_ms : 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        log_msg("ERROR", "db_update_hourly step: %s", sqlite3_errmsg(g_db));
        return false;
    }
    return true;
}

bool db_update_daily_stats(const char *day, const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           double latency_ms) {
    if (!g_db) return false;

    const char *sql =
        "INSERT INTO daily_stats (day, model, provider, requests, success, failed,"
        " input_tokens, output_tokens, total_latency_ms, latency_count)"
        " VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(day, model, provider) DO UPDATE SET"
        " requests = requests + 1,"
        " success = success + excluded.success,"
        " failed = failed + excluded.failed,"
        " input_tokens = input_tokens + excluded.input_tokens,"
        " output_tokens = output_tokens + excluded.output_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_update_daily prepare: %s", sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, day ? day : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, provider ? provider : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, success ? 1 : 0);
    sqlite3_bind_int(stmt, 5, success ? 0 : 1);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)input_tokens);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)output_tokens);
    sqlite3_bind_double(stmt, 8, latency_ms > 0 ? latency_ms : 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        log_msg("ERROR", "db_update_daily step: %s", sqlite3_errmsg(g_db));
        return false;
    }
    return true;
}

bool db_update_model_stats(const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           double latency_ms) {
    if (!g_db) return false;

    const char *sql =
        "INSERT INTO model_stats (model, provider, total_requests, total_success, total_failed,"
        " total_input_tokens, total_output_tokens, total_latency_ms, latency_count,"
        " min_latency_ms, max_latency_ms, first_seen, last_seen)"
        " VALUES (?, ?, 1, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)"
        " ON CONFLICT(model) DO UPDATE SET"
        " provider = excluded.provider,"
        " total_requests = total_requests + 1,"
        " total_success = total_success + excluded.total_success,"
        " total_failed = total_failed + excluded.total_failed,"
        " total_input_tokens = total_input_tokens + excluded.total_input_tokens,"
        " total_output_tokens = total_output_tokens + excluded.total_output_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count,"
        " min_latency_ms = CASE WHEN excluded.min_latency_ms IS NOT NULL AND"
        "  (model_stats.min_latency_ms IS NULL OR excluded.min_latency_ms < model_stats.min_latency_ms)"
        "  THEN excluded.min_latency_ms ELSE model_stats.min_latency_ms END,"
        " max_latency_ms = CASE WHEN excluded.max_latency_ms IS NOT NULL AND"
        "  (model_stats.max_latency_ms IS NULL OR excluded.max_latency_ms > model_stats.max_latency_ms)"
        "  THEN excluded.max_latency_ms ELSE model_stats.max_latency_ms END,"
        " last_seen = excluded.last_seen;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_update_model prepare: %s", sqlite3_errmsg(g_db));
        return false;
    }

    time_t now = time(NULL);
    sqlite3_bind_text(stmt, 1, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, provider ? provider : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, success ? 1 : 0);
    sqlite3_bind_int(stmt, 4, success ? 0 : 1);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)input_tokens);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)output_tokens);
    sqlite3_bind_double(stmt, 7, latency_ms > 0 ? latency_ms : 0);
    sqlite3_bind_double(stmt, 8, latency_ms > 0 ? latency_ms : -1);
    sqlite3_bind_double(stmt, 9, latency_ms > 0 ? latency_ms : -1);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)now);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        log_msg("ERROR", "db_update_model step: %s", sqlite3_errmsg(g_db));
        return false;
    }
    return true;
}

cJSON *db_query_history(const char *model, time_t from, time_t to, int limit, int offset) {
    if (!g_db) return cJSON_CreateArray();

    /* 动态构建 WHERE 子句 */
    char where[512] = "";
    int wc = 0;
    if (model && *model) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where), " WHERE model = ?");
        wc++;
    }
    if (from > 0) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND timestamp >= ?" : " WHERE timestamp >= ?");
        wc++;
    }
    if (to > 0) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND timestamp <= ?" : " WHERE timestamp <= ?");
        wc++;
    }

    /* 先 COUNT 总数 */
    char count_sql[1024];
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM requests%s;", where);
    sqlite3_stmt *count_stmt = NULL;
    int total = 0;
    if (sqlite3_prepare_v2(g_db, count_sql, -1, &count_stmt, NULL) == SQLITE_OK) {
        int cidx = 1;
        if (model && *model) sqlite3_bind_text(count_stmt, cidx++, model, -1, SQLITE_STATIC);
        if (from > 0) sqlite3_bind_int64(count_stmt, cidx++, (sqlite3_int64)from);
        if (to > 0) sqlite3_bind_int64(count_stmt, cidx++, (sqlite3_int64)to);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            total = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT timestamp, model, provider, stream, http_status, curl_code,"
             " input_tokens, output_tokens, latency_ms, request_bytes, response_bytes,"
             " client_model, upstream_url"
             " FROM requests%s ORDER BY timestamp DESC LIMIT ? OFFSET ?;",
             where);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_query_history prepare: %s", sqlite3_errmsg(g_db));
        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "total", 0);
        cJSON_AddNumberToObject(out, "limit", limit > 0 ? limit : 100);
        cJSON_AddNumberToObject(out, "offset", offset > 0 ? offset : 0);
        cJSON_AddItemToObject(out, "data", cJSON_CreateArray());
        return out;
    }

    int idx = 1;
    if (model && *model) sqlite3_bind_text(stmt, idx++, model, -1, SQLITE_STATIC);
    if (from > 0) sqlite3_bind_int64(stmt, idx++, (sqlite3_int64)from);
    if (to > 0) sqlite3_bind_int64(stmt, idx++, (sqlite3_int64)to);
    sqlite3_bind_int(stmt, idx++, limit > 0 ? limit : 100);
    sqlite3_bind_int(stmt, idx++, offset > 0 ? offset : 0);

    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "timestamp", (double)sqlite3_column_int64(stmt, 0));
        cJSON_AddStringToObject(obj, "model", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddStringToObject(obj, "provider", (const char *)sqlite3_column_text(stmt, 2));
        cJSON_AddNumberToObject(obj, "stream", sqlite3_column_int(stmt, 3));
        cJSON_AddNumberToObject(obj, "http_status", sqlite3_column_int(stmt, 4));
        cJSON_AddNumberToObject(obj, "curl_code", sqlite3_column_int(stmt, 5));
        cJSON_AddNumberToObject(obj, "input_tokens", (double)sqlite3_column_int64(stmt, 6));
        cJSON_AddNumberToObject(obj, "output_tokens", (double)sqlite3_column_int64(stmt, 7));
        cJSON_AddNumberToObject(obj, "latency_ms", sqlite3_column_double(stmt, 8));
        cJSON_AddNumberToObject(obj, "request_bytes", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddNumberToObject(obj, "response_bytes", (double)sqlite3_column_int64(stmt, 10));
        const char *cm = (const char *)sqlite3_column_text(stmt, 11);
        if (cm) cJSON_AddStringToObject(obj, "client_model", cm);
        const char *uu = (const char *)sqlite3_column_text(stmt, 12);
        if (uu) cJSON_AddStringToObject(obj, "upstream_url", uu);
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "total", total);
    cJSON_AddNumberToObject(out, "limit", limit > 0 ? limit : 100);
    cJSON_AddNumberToObject(out, "offset", offset > 0 ? offset : 0);
    cJSON_AddItemToObject(out, "data", arr);
    return out;
}

cJSON *db_query_hourly(const char *model, const char *from_hour, const char *to_hour) {
    if (!g_db) return cJSON_CreateArray();

    char where[512] = "";
    int wc = 0;
    if (model && *model) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where), " WHERE model = ?");
        wc++;
    }
    if (from_hour && *from_hour) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND hour >= ?" : " WHERE hour >= ?");
        wc++;
    }
    if (to_hour && *to_hour) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND hour <= ?" : " WHERE hour <= ?");
        wc++;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT hour, model, provider, requests, success, failed,"
             " input_tokens, output_tokens, total_latency_ms, latency_count"
             " FROM hourly_stats%s ORDER BY hour;",
             where);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_query_hourly prepare: %s", sqlite3_errmsg(g_db));
        return cJSON_CreateArray();
    }

    int idx = 1;
    if (model && *model) sqlite3_bind_text(stmt, idx++, model, -1, SQLITE_STATIC);
    if (from_hour && *from_hour) sqlite3_bind_text(stmt, idx++, from_hour, -1, SQLITE_STATIC);
    if (to_hour && *to_hour) sqlite3_bind_text(stmt, idx++, to_hour, -1, SQLITE_STATIC);

    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "hour", (const char *)sqlite3_column_text(stmt, 0));
        cJSON_AddStringToObject(obj, "model", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddStringToObject(obj, "provider", (const char *)sqlite3_column_text(stmt, 2));
        cJSON_AddNumberToObject(obj, "requests", (double)sqlite3_column_int64(stmt, 3));
        cJSON_AddNumberToObject(obj, "success", (double)sqlite3_column_int64(stmt, 4));
        cJSON_AddNumberToObject(obj, "failed", (double)sqlite3_column_int64(stmt, 5));
        cJSON_AddNumberToObject(obj, "input_tokens", (double)sqlite3_column_int64(stmt, 6));
        cJSON_AddNumberToObject(obj, "output_tokens", (double)sqlite3_column_int64(stmt, 7));
        cJSON_AddNumberToObject(obj, "total_latency_ms", sqlite3_column_double(stmt, 8));
        cJSON_AddNumberToObject(obj, "latency_count", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

cJSON *db_query_daily(const char *model, const char *from_day, const char *to_day) {
    if (!g_db) return cJSON_CreateArray();

    char where[512] = "";
    int wc = 0;
    if (model && *model) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where), " WHERE model = ?");
        wc++;
    }
    if (from_day && *from_day) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND day >= ?" : " WHERE day >= ?");
        wc++;
    }
    if (to_day && *to_day) {
        snprintf(where + strlen(where), sizeof(where) - strlen(where),
                 wc ? " AND day <= ?" : " WHERE day <= ?");
        wc++;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT day, model, provider, requests, success, failed,"
             " input_tokens, output_tokens, total_latency_ms, latency_count"
             " FROM daily_stats%s ORDER BY day;",
             where);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_query_daily prepare: %s", sqlite3_errmsg(g_db));
        return cJSON_CreateArray();
    }

    int idx = 1;
    if (model && *model) sqlite3_bind_text(stmt, idx++, model, -1, SQLITE_STATIC);
    if (from_day && *from_day) sqlite3_bind_text(stmt, idx++, from_day, -1, SQLITE_STATIC);
    if (to_day && *to_day) sqlite3_bind_text(stmt, idx++, to_day, -1, SQLITE_STATIC);

    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "day", (const char *)sqlite3_column_text(stmt, 0));
        cJSON_AddStringToObject(obj, "model", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddStringToObject(obj, "provider", (const char *)sqlite3_column_text(stmt, 2));
        cJSON_AddNumberToObject(obj, "requests", (double)sqlite3_column_int64(stmt, 3));
        cJSON_AddNumberToObject(obj, "success", (double)sqlite3_column_int64(stmt, 4));
        cJSON_AddNumberToObject(obj, "failed", (double)sqlite3_column_int64(stmt, 5));
        cJSON_AddNumberToObject(obj, "input_tokens", (double)sqlite3_column_int64(stmt, 6));
        cJSON_AddNumberToObject(obj, "output_tokens", (double)sqlite3_column_int64(stmt, 7));
        cJSON_AddNumberToObject(obj, "total_latency_ms", sqlite3_column_double(stmt, 8));
        cJSON_AddNumberToObject(obj, "latency_count", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

bool db_cleanup_old_data(int request_retention_days, int hourly_retention_days) {
    if (!g_db) return false;

    time_t now = time(NULL);
    char *err = NULL;
    bool ok = true;

    if (request_retention_days > 0) {
        time_t cutoff = now - request_retention_days * 86400;
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM requests WHERE timestamp < %ld;", (long)cutoff);
        int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            log_msg("ERROR", "db_cleanup requests: %s", err ? err : "unknown");
            if (err) sqlite3_free(err);
            ok = false;
        } else {
            int changes = sqlite3_changes(g_db);
            if (changes > 0) log_msg("INFO", "db_cleanup requests: deleted %d rows", changes);
        }
    }

    if (hourly_retention_days > 0) {
        time_t cutoff = now - hourly_retention_days * 86400;
        struct tm *tm_info = localtime(&cutoff);
        char hour_str[32];
        strftime(hour_str, sizeof(hour_str), "%Y-%m-%d %H:00", tm_info);
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM hourly_stats WHERE hour < '%s';", hour_str);
        int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            log_msg("ERROR", "db_cleanup hourly: %s", err ? err : "unknown");
            if (err) sqlite3_free(err);
            ok = false;
        } else {
            int changes = sqlite3_changes(g_db);
            if (changes > 0) log_msg("INFO", "db_cleanup hourly: deleted %d rows", changes);
        }
    }

    return ok;
}
