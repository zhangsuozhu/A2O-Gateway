#include "db.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static sqlite3 *g_db = NULL;
static char *g_db_path = NULL;

/* ====================================================================
 * 异步批量写入队列
 * ==================================================================== */

typedef enum {
    BATCH_INSERT_REQUEST,
    BATCH_UPDATE_HOURLY,
    BATCH_UPDATE_DAILY,
    BATCH_UPDATE_MODEL
} batch_op_t;

typedef struct batch_item {
    batch_op_t op;
    time_t timestamp;
    char model[64];
    char provider[64];
    bool stream;
    int http_status;
    int curl_code;
    long input_tokens;
    long output_tokens;
    long cache_read_input_tokens;
    long cache_creation_input_tokens;
    double latency_ms;
    size_t request_bytes;
    size_t response_bytes;
    char client_model[64];
    char upstream_url[256];
    char time_key[32];
    bool success;
    struct batch_item *next;
} batch_item_t;

static batch_item_t *batch_head = NULL;
static batch_item_t *batch_tail = NULL;
static int batch_count = 0;
static pthread_mutex_t batch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t batch_cv = PTHREAD_COND_INITIALIZER;
static pthread_t batch_thread;
static volatile bool batch_running = false;

#define BATCH_FLUSH_SIZE 500
#define BATCH_FLUSH_MS 100

static void batch_enqueue(batch_item_t *item)
{
    pthread_mutex_lock(&batch_mu);
    item->next = NULL;
    if (batch_tail) {
        batch_tail->next = item;
    } else {
        batch_head = item;
    }
    batch_tail = item;
    batch_count++;
    bool should_signal = (batch_count >= BATCH_FLUSH_SIZE);
    pthread_mutex_unlock(&batch_mu);
    if (should_signal)
        pthread_cond_signal(&batch_cv);
}

static batch_item_t *batch_dequeue_all(int *out_count)
{
    pthread_mutex_lock(&batch_mu);
    batch_item_t *items = batch_head;
    *out_count = batch_count;
    batch_head = batch_tail = NULL;
    batch_count = 0;
    pthread_mutex_unlock(&batch_mu);
    return items;
}

static void db_flush_batch(batch_item_t *items, int count);

static void *batch_worker(void *arg)
{
    (void)arg;
    while (batch_running) {
        pthread_mutex_lock(&batch_mu);
        while (batch_count < BATCH_FLUSH_SIZE && batch_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += BATCH_FLUSH_MS * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            int rc = pthread_cond_timedwait(&batch_cv, &batch_mu, &ts);
            if (rc == ETIMEDOUT)
                break;
        }
        batch_item_t *items = batch_head;
        int count = batch_count;
        batch_head = batch_tail = NULL;
        batch_count = 0;
        pthread_mutex_unlock(&batch_mu);

        if (items)
            db_flush_batch(items, count);

        while (items) {
            batch_item_t *next = items->next;
            free(items);
            items = next;
        }
    }
    return NULL;
}

static void db_flush_batch(batch_item_t *items, int count)
{
    if (!g_db || !items || count <= 0)
        return;

    sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

    const char *req_sql =
        "INSERT INTO requests"
        " (timestamp, model, provider, stream, http_status, curl_code,"
        " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
        " latency_ms, request_bytes, response_bytes, client_model, upstream_url)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    const char *hour_sql =
        "INSERT INTO hourly_stats (hour, model, provider, requests, success, failed,"
        " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
        " total_latency_ms, latency_count)"
        " VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(hour, model, provider) DO UPDATE SET"
        " requests = requests + 1,"
        " success = success + excluded.success,"
        " failed = failed + excluded.failed,"
        " input_tokens = input_tokens + excluded.input_tokens,"
        " output_tokens = output_tokens + excluded.output_tokens,"
        " cache_read_input_tokens = cache_read_input_tokens + excluded.cache_read_input_tokens,"
        " cache_creation_input_tokens = cache_creation_input_tokens + excluded.cache_creation_input_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count;";

    const char *day_sql =
        "INSERT INTO daily_stats (day, model, provider, requests, success, failed,"
        " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
        " total_latency_ms, latency_count)"
        " VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(day, model, provider) DO UPDATE SET"
        " requests = requests + 1,"
        " success = success + excluded.success,"
        " failed = failed + excluded.failed,"
        " input_tokens = input_tokens + excluded.input_tokens,"
        " output_tokens = output_tokens + excluded.output_tokens,"
        " cache_read_input_tokens = cache_read_input_tokens + excluded.cache_read_input_tokens,"
        " cache_creation_input_tokens = cache_creation_input_tokens + excluded.cache_creation_input_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count;";

    const char *model_sql =
        "INSERT INTO model_stats (model, provider, total_requests, total_success, total_failed,"
        " total_input_tokens, total_output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
        " total_latency_ms, latency_count,"
        " min_latency_ms, max_latency_ms, first_seen, last_seen)"
        " VALUES (?, ?, 1, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)"
        " ON CONFLICT(model) DO UPDATE SET"
        " provider = excluded.provider,"
        " total_requests = total_requests + 1,"
        " total_success = total_success + excluded.total_success,"
        " total_failed = total_failed + excluded.total_failed,"
        " total_input_tokens = total_input_tokens + excluded.total_input_tokens,"
        " total_output_tokens = total_output_tokens + excluded.total_output_tokens,"
        " cache_read_input_tokens = cache_read_input_tokens + excluded.cache_read_input_tokens,"
        " cache_creation_input_tokens = cache_creation_input_tokens + excluded.cache_creation_input_tokens,"
        " total_latency_ms = total_latency_ms + excluded.total_latency_ms,"
        " latency_count = latency_count + excluded.latency_count,"
        " min_latency_ms = CASE WHEN excluded.min_latency_ms IS NOT NULL AND"
        "  (model_stats.min_latency_ms IS NULL OR excluded.min_latency_ms < model_stats.min_latency_ms)"
        "  THEN excluded.min_latency_ms ELSE model_stats.min_latency_ms END,"
        " max_latency_ms = CASE WHEN excluded.max_latency_ms IS NOT NULL AND"
        "  (model_stats.max_latency_ms IS NULL OR excluded.max_latency_ms > model_stats.max_latency_ms)"
        "  THEN excluded.max_latency_ms ELSE model_stats.max_latency_ms END,"
        " last_seen = excluded.last_seen;";

    sqlite3_stmt *req_stmt = NULL;
    sqlite3_stmt *hour_stmt = NULL;
    sqlite3_stmt *day_stmt = NULL;
    sqlite3_stmt *model_stmt = NULL;

    sqlite3_prepare_v2(g_db, req_sql, -1, &req_stmt, NULL);
    sqlite3_prepare_v2(g_db, hour_sql, -1, &hour_stmt, NULL);
    sqlite3_prepare_v2(g_db, day_sql, -1, &day_stmt, NULL);
    sqlite3_prepare_v2(g_db, model_sql, -1, &model_stmt, NULL);

    for (batch_item_t *it = items; it; it = it->next) {
        sqlite3_stmt *stmt = NULL;
        switch (it->op) {
        case BATCH_INSERT_REQUEST:
            stmt = req_stmt;
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)it->timestamp);
            sqlite3_bind_text(stmt, 2, it->model, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, it->provider, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, it->stream ? 1 : 0);
            sqlite3_bind_int(stmt, 5, it->http_status);
            sqlite3_bind_int(stmt, 6, it->curl_code);
            sqlite3_bind_int64(stmt, 7, (sqlite3_int64)it->input_tokens);
            sqlite3_bind_int64(stmt, 8, (sqlite3_int64)it->output_tokens);
            sqlite3_bind_int64(stmt, 9, (sqlite3_int64)it->cache_read_input_tokens);
            sqlite3_bind_int64(stmt, 10, (sqlite3_int64)it->cache_creation_input_tokens);
            sqlite3_bind_double(stmt, 11, it->latency_ms);
            sqlite3_bind_int64(stmt, 12, (sqlite3_int64)it->request_bytes);
            sqlite3_bind_int64(stmt, 13, (sqlite3_int64)it->response_bytes);
            sqlite3_bind_text(stmt, 14, it->client_model, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 15, it->upstream_url, -1, SQLITE_STATIC);
            break;
        case BATCH_UPDATE_HOURLY:
            stmt = hour_stmt;
            sqlite3_bind_text(stmt, 1, it->time_key, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, it->model, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, it->provider, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, it->success ? 1 : 0);
            sqlite3_bind_int(stmt, 5, it->success ? 0 : 1);
            sqlite3_bind_int64(stmt, 6, (sqlite3_int64)it->input_tokens);
            sqlite3_bind_int64(stmt, 7, (sqlite3_int64)it->output_tokens);
            sqlite3_bind_int64(stmt, 8, (sqlite3_int64)it->cache_read_input_tokens);
            sqlite3_bind_int64(stmt, 9, (sqlite3_int64)it->cache_creation_input_tokens);
            sqlite3_bind_double(stmt, 10, it->latency_ms > 0 ? it->latency_ms : 0);
            break;
        case BATCH_UPDATE_DAILY:
            stmt = day_stmt;
            sqlite3_bind_text(stmt, 1, it->time_key, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, it->model, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, it->provider, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, it->success ? 1 : 0);
            sqlite3_bind_int(stmt, 5, it->success ? 0 : 1);
            sqlite3_bind_int64(stmt, 6, (sqlite3_int64)it->input_tokens);
            sqlite3_bind_int64(stmt, 7, (sqlite3_int64)it->output_tokens);
            sqlite3_bind_int64(stmt, 8, (sqlite3_int64)it->cache_read_input_tokens);
            sqlite3_bind_int64(stmt, 9, (sqlite3_int64)it->cache_creation_input_tokens);
            sqlite3_bind_double(stmt, 10, it->latency_ms > 0 ? it->latency_ms : 0);
            break;
        case BATCH_UPDATE_MODEL:
            stmt = model_stmt;
            time_t now = it->timestamp ? it->timestamp : time(NULL);
            sqlite3_bind_text(stmt, 1, it->model, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, it->provider, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, it->success ? 1 : 0);
            sqlite3_bind_int(stmt, 4, it->success ? 0 : 1);
            sqlite3_bind_int64(stmt, 5, (sqlite3_int64)it->input_tokens);
            sqlite3_bind_int64(stmt, 6, (sqlite3_int64)it->output_tokens);
            sqlite3_bind_int64(stmt, 7, (sqlite3_int64)it->cache_read_input_tokens);
            sqlite3_bind_int64(stmt, 8, (sqlite3_int64)it->cache_creation_input_tokens);
            sqlite3_bind_double(stmt, 9, it->latency_ms > 0 ? it->latency_ms : 0);
            sqlite3_bind_double(stmt, 10, it->latency_ms > 0 ? it->latency_ms : -1);
            sqlite3_bind_double(stmt, 11, it->latency_ms > 0 ? it->latency_ms : -1);
            sqlite3_bind_int64(stmt, 12, (sqlite3_int64)now);
            sqlite3_bind_int64(stmt, 13, (sqlite3_int64)now);
            break;
        }

        if (stmt) {
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                log_msg("ERROR", "batch step op=%d: %s", it->op, sqlite3_errmsg(g_db));
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }

    if (req_stmt) sqlite3_finalize(req_stmt);
    if (hour_stmt) sqlite3_finalize(hour_stmt);
    if (day_stmt) sqlite3_finalize(day_stmt);
    if (model_stmt) sqlite3_finalize(model_stmt);

    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
}

/* 在关闭/重置前强制 flush 所有待处理数据 */
static void batch_flush_sync(void)
{
    batch_item_t *items;
    int count;
    do {
        items = batch_dequeue_all(&count);
        if (items) {
            db_flush_batch(items, count);
            while (items) {
                batch_item_t *next = items->next;
                free(items);
                items = next;
            }
        }
    } while (items);
}

/* ====================================================================
 * 建表 SQL
 * ==================================================================== */

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
    "    cache_read_input_tokens INTEGER DEFAULT 0,"
    "    cache_creation_input_tokens INTEGER DEFAULT 0,"
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
    "    cache_read_input_tokens INTEGER DEFAULT 0,"
    "    cache_creation_input_tokens INTEGER DEFAULT 0,"
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
    "    cache_read_input_tokens INTEGER DEFAULT 0,"
    "    cache_creation_input_tokens INTEGER DEFAULT 0,"
    "    total_latency_ms REAL DEFAULT 0,"
    "    latency_count INTEGER DEFAULT 0,"
    "    PRIMARY KEY (day, model, provider)"
    ");"

    "CREATE TABLE IF NOT EXISTS error_logs ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    timestamp INTEGER NOT NULL,"
    "    model TEXT NOT NULL,"
    "    provider TEXT NOT NULL,"
    "    http_status INTEGER,"
    "    curl_code INTEGER,"
    "    error_message TEXT,"
    "    request_body TEXT,"
    "    response_body TEXT,"
    "    latency_ms REAL,"
    "    upstream_url TEXT,"
    "    client_model TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_err_time ON error_logs(timestamp);"
    "CREATE INDEX IF NOT EXISTS idx_err_model ON error_logs(model);"

    "CREATE TABLE IF NOT EXISTS model_stats ("
    "    model TEXT NOT NULL PRIMARY KEY,"
    "    provider TEXT NOT NULL,"
    "    total_requests INTEGER DEFAULT 0,"
    "    total_success INTEGER DEFAULT 0,"
    "    total_failed INTEGER DEFAULT 0,"
    "    total_input_tokens INTEGER DEFAULT 0,"
    "    total_output_tokens INTEGER DEFAULT 0,"
    "    cache_read_input_tokens INTEGER DEFAULT 0,"
    "    cache_creation_input_tokens INTEGER DEFAULT 0,"
    "    total_latency_ms REAL DEFAULT 0,"
    "    latency_count INTEGER DEFAULT 0,"
    "    min_latency_ms REAL,"
    "    max_latency_ms REAL,"
    "    first_seen INTEGER,"
    "    last_seen INTEGER"
    ");"
    ;

bool db_init(const char *db_path)
{
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

    /* schema 升级 */
    sqlite3_exec(g_db, "ALTER TABLE requests ADD COLUMN cache_read_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE requests ADD COLUMN cache_creation_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE hourly_stats ADD COLUMN cache_read_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE hourly_stats ADD COLUMN cache_creation_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE daily_stats ADD COLUMN cache_read_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE daily_stats ADD COLUMN cache_creation_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE model_stats ADD COLUMN cache_read_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE model_stats ADD COLUMN cache_creation_input_tokens INTEGER DEFAULT 0;", NULL, NULL, NULL);

    free(g_db_path);
    g_db_path = db_path ? strdup(db_path) : strdup("gateway.db");

    /* 启动后台批量写入线程 */
    batch_running = true;
    pthread_create(&batch_thread, NULL, batch_worker, NULL);

    log_msg("INFO", "db_init ok: %s (async batch flush=%dms/%d)", g_db_path, BATCH_FLUSH_MS, BATCH_FLUSH_SIZE);
    return true;
}

bool db_reset(void)
{
    if (!g_db_path) return false;

    batch_flush_sync();

    if (g_db) {
        batch_running = false;
        pthread_cond_signal(&batch_cv);
        pthread_join(batch_thread, NULL);
        sqlite3_close(g_db);
        g_db = NULL;
    }
    unlink(g_db_path);
    log_msg("INFO", "db_reset: removed %s", g_db_path);
    return db_init(g_db_path);
}

void db_close(void)
{
    if (g_db) {
        batch_flush_sync();
        batch_running = false;
        pthread_cond_signal(&batch_cv);
        pthread_join(batch_thread, NULL);
        sqlite3_close(g_db);
        g_db = NULL;
        log_msg("INFO", "db_close");
    }
}

bool db_insert_request(const char *model, const char *provider, bool stream,
                       int http_status, int curl_code,
                       long input_tokens, long output_tokens,
                       long cache_read_input_tokens, long cache_creation_input_tokens,
                       double latency_ms, size_t request_bytes, size_t response_bytes,
                       const char *client_model, const char *upstream_url)
{
    if (!g_db) return false;

    batch_item_t *item = calloc(1, sizeof(batch_item_t));
    if (!item) return false;

    item->op = BATCH_INSERT_REQUEST;
    item->timestamp = time(NULL);
    strncpy(item->model, model ? model : "", sizeof(item->model) - 1);
    strncpy(item->provider, provider ? provider : "", sizeof(item->provider) - 1);
    item->stream = stream;
    item->http_status = http_status;
    item->curl_code = curl_code;
    item->input_tokens = input_tokens;
    item->output_tokens = output_tokens;
    item->cache_read_input_tokens = cache_read_input_tokens;
    item->cache_creation_input_tokens = cache_creation_input_tokens;
    item->latency_ms = latency_ms;
    item->request_bytes = request_bytes;
    item->response_bytes = response_bytes;
    strncpy(item->client_model, client_model ? client_model : "", sizeof(item->client_model) - 1);
    strncpy(item->upstream_url, upstream_url ? upstream_url : "", sizeof(item->upstream_url) - 1);

    batch_enqueue(item);
    return true;
}

bool db_update_hourly_stats(const char *hour, const char *model, const char *provider,
                            bool success, long input_tokens, long output_tokens,
                            long cache_read_input_tokens, long cache_creation_input_tokens,
                            double latency_ms)
{
    if (!g_db) return false;

    batch_item_t *item = calloc(1, sizeof(batch_item_t));
    if (!item) return false;

    item->op = BATCH_UPDATE_HOURLY;
    strncpy(item->time_key, hour ? hour : "", sizeof(item->time_key) - 1);
    strncpy(item->model, model ? model : "", sizeof(item->model) - 1);
    strncpy(item->provider, provider ? provider : "", sizeof(item->provider) - 1);
    item->success = success;
    item->input_tokens = input_tokens;
    item->output_tokens = output_tokens;
    item->cache_read_input_tokens = cache_read_input_tokens;
    item->cache_creation_input_tokens = cache_creation_input_tokens;
    item->latency_ms = latency_ms;

    batch_enqueue(item);
    return true;
}

bool db_update_daily_stats(const char *day, const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           long cache_read_input_tokens, long cache_creation_input_tokens,
                           double latency_ms)
{
    if (!g_db) return false;

    batch_item_t *item = calloc(1, sizeof(batch_item_t));
    if (!item) return false;

    item->op = BATCH_UPDATE_DAILY;
    strncpy(item->time_key, day ? day : "", sizeof(item->time_key) - 1);
    strncpy(item->model, model ? model : "", sizeof(item->model) - 1);
    strncpy(item->provider, provider ? provider : "", sizeof(item->provider) - 1);
    item->success = success;
    item->input_tokens = input_tokens;
    item->output_tokens = output_tokens;
    item->cache_read_input_tokens = cache_read_input_tokens;
    item->cache_creation_input_tokens = cache_creation_input_tokens;
    item->latency_ms = latency_ms;

    batch_enqueue(item);
    return true;
}

bool db_update_model_stats(const char *model, const char *provider,
                           bool success, long input_tokens, long output_tokens,
                           long cache_read_input_tokens, long cache_creation_input_tokens,
                           double latency_ms)
{
    if (!g_db) return false;

    batch_item_t *item = calloc(1, sizeof(batch_item_t));
    if (!item) return false;

    item->op = BATCH_UPDATE_MODEL;
    item->timestamp = time(NULL);
    strncpy(item->model, model ? model : "", sizeof(item->model) - 1);
    strncpy(item->provider, provider ? provider : "", sizeof(item->provider) - 1);
    item->success = success;
    item->input_tokens = input_tokens;
    item->output_tokens = output_tokens;
    item->cache_read_input_tokens = cache_read_input_tokens;
    item->cache_creation_input_tokens = cache_creation_input_tokens;
    item->latency_ms = latency_ms;

    batch_enqueue(item);
    return true;
}

cJSON *db_query_history(const char *model, time_t from, time_t to, int limit, int offset)
{
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
             " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
             " latency_ms, request_bytes, response_bytes,"
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
        cJSON_AddNumberToObject(obj, "cache_read_input_tokens", (double)sqlite3_column_int64(stmt, 8));
        cJSON_AddNumberToObject(obj, "cache_creation_input_tokens", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddNumberToObject(obj, "latency_ms", sqlite3_column_double(stmt, 10));
        cJSON_AddNumberToObject(obj, "request_bytes", (double)sqlite3_column_int64(stmt, 11));
        cJSON_AddNumberToObject(obj, "response_bytes", (double)sqlite3_column_int64(stmt, 12));
        const char *cm = (const char *)sqlite3_column_text(stmt, 13);
        if (cm) cJSON_AddStringToObject(obj, "client_model", cm);
        const char *uu = (const char *)sqlite3_column_text(stmt, 14);
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

cJSON *db_query_hourly(const char *model, const char *from_hour, const char *to_hour)
{
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
             " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
             " total_latency_ms, latency_count"
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
        cJSON_AddNumberToObject(obj, "cache_read_input_tokens", (double)sqlite3_column_int64(stmt, 8));
        cJSON_AddNumberToObject(obj, "cache_creation_input_tokens", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddNumberToObject(obj, "total_latency_ms", sqlite3_column_double(stmt, 10));
        cJSON_AddNumberToObject(obj, "latency_count", (double)sqlite3_column_int64(stmt, 11));
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

cJSON *db_query_daily(const char *model, const char *from_day, const char *to_day)
{
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
             " input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens,"
             " total_latency_ms, latency_count"
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
        cJSON_AddNumberToObject(obj, "cache_read_input_tokens", (double)sqlite3_column_int64(stmt, 8));
        cJSON_AddNumberToObject(obj, "cache_creation_input_tokens", (double)sqlite3_column_int64(stmt, 9));
        cJSON_AddNumberToObject(obj, "total_latency_ms", sqlite3_column_double(stmt, 10));
        cJSON_AddNumberToObject(obj, "latency_count", (double)sqlite3_column_int64(stmt, 11));
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

/* ====================================================================
 * 错误日志（同步写入，含完整请求/响应体）
 * ==================================================================== */

bool db_insert_error_log(const char *model, const char *provider,
                          int http_status, int curl_code,
                          const char *error_message,
                          const char *request_body, const char *response_body,
                          double latency_ms, const char *upstream_url,
                          const char *client_model)
{
    if (!g_db) return false;

    const char *sql = "INSERT INTO error_logs"
        " (timestamp, model, provider, http_status, curl_code, error_message,"
        " request_body, response_body, latency_ms, upstream_url, client_model)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_insert_error_log prepare: %s", sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 2, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, provider ? provider : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, http_status);
    sqlite3_bind_int(stmt, 5, curl_code);
    sqlite3_bind_text(stmt, 6, error_message ? error_message : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, request_body ? request_body : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, response_body ? response_body : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 9, latency_ms);
    sqlite3_bind_text(stmt, 10, upstream_url ? upstream_url : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, client_model ? client_model : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        log_msg("ERROR", "db_insert_error_log step: %s", sqlite3_errmsg(g_db));
        return false;
    }
    return true;
}

cJSON *db_query_error_logs(const char *model, time_t from, time_t to, int limit, int offset)
{
    if (!g_db) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "total", 0);
        cJSON_AddItemToObject(out, "data", cJSON_CreateArray());
        return out;
    }

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

    /* COUNT 总数 */
    char count_sql[1024];
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM error_logs%s;", where);
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

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT timestamp, model, provider, http_status, curl_code, error_message,"
             " request_body, response_body, latency_ms, upstream_url, client_model"
             " FROM error_logs%s ORDER BY timestamp DESC LIMIT ? OFFSET ?;",
             where);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("ERROR", "db_query_error_logs prepare: %s", sqlite3_errmsg(g_db));
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
        cJSON_AddNumberToObject(obj, "http_status", sqlite3_column_int(stmt, 3));
        cJSON_AddNumberToObject(obj, "curl_code", sqlite3_column_int(stmt, 4));
        const char *err_msg = (const char *)sqlite3_column_text(stmt, 5);
        if (err_msg) cJSON_AddStringToObject(obj, "error_message", err_msg);
        const char *req_body = (const char *)sqlite3_column_text(stmt, 6);
        if (req_body) cJSON_AddStringToObject(obj, "request_body", req_body);
        const char *resp_body = (const char *)sqlite3_column_text(stmt, 7);
        if (resp_body) cJSON_AddStringToObject(obj, "response_body", resp_body);
        cJSON_AddNumberToObject(obj, "latency_ms", sqlite3_column_double(stmt, 8));
        const char *url = (const char *)sqlite3_column_text(stmt, 9);
        if (url) cJSON_AddStringToObject(obj, "upstream_url", url);
        const char *cm = (const char *)sqlite3_column_text(stmt, 10);
        if (cm) cJSON_AddStringToObject(obj, "client_model", cm);
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

bool db_cleanup_old_data(int request_retention_days, int hourly_retention_days)
{
    if (!g_db) return false;

    batch_flush_sync();

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
