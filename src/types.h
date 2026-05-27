#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <cjson/cJSON.h>

/* Forward declarations matching libcurl actual typedefs */
typedef void CURL;
typedef void CURLM;
struct curl_slist;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_CONFIG_PATH "./config/gateway.json"
#define MAX_TOOL_STREAMS 64
#define MAX_WORKERS 8
#define MAX_BODY_BYTES (64 * 1024 * 1024)
#define MASKED_KEY "***MASKED***"

typedef struct membuf {
    char *ptr;
    size_t len;
    size_t cap;
} membuf_t;

typedef struct app_config {
    pthread_rwlock_t lock;
    cJSON *root;
    char path[PATH_MAX];
} app_config_t;

typedef struct tool_stream_state {
    int openai_index;
    int block_index;
    bool started;
    bool start_emitted;
    char *id;
    char *name;
} tool_stream_state_t;

typedef struct stream_state {
    bool reply_started;
    bool message_started;
    bool text_started;
    bool ended;
    int text_block_index;
    int next_block_index;
    char *linebuf;
    size_t linebuf_len;
    size_t linebuf_cap;
    char *message_id;
    char *client_model;
    char *finish_reason;
    char *response_text;
    char *text_pending;
    size_t text_pending_len;
    size_t text_pending_cap;
    long prompt_tokens;
    long completion_tokens;
    tool_stream_state_t tools[MAX_TOOL_STREAMS];
} stream_state_t;

typedef struct gateway_job gateway_job_t;

typedef struct worker {
    pthread_t tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    CURLM *multi;
    gateway_job_t *pending_head;
    gateway_job_t *pending_tail;
    int still_running;
    bool stop;
    int id;
} worker_t;

struct gateway_job {
    struct evhttp_request *client_req;
    char *request_body;
    char *upstream_url;
    char *api_key;
    char *provider_name;
    char *client_model;
    char *upstream_model;
    bool stream;
    bool upstream_headers_done;
    bool response_sent;
    long upstream_status;
    membuf_t upstream_body;
    stream_state_t stream_state;
    CURL *easy;
    struct curl_slist *headers;
    worker_t *worker;
    gateway_job_t *next;
    pthread_mutex_t send_mu;
    struct evbuffer *send_buf;
    bool send_start;
    bool send_end;
    char *nonstream_json;
    int nonstream_code;
};

extern struct event_base *BASE;
extern struct evhttp *HTTP;
extern worker_t WORKERS[MAX_WORKERS];
extern int WORKER_COUNT;
extern unsigned long RR;
extern volatile sig_atomic_t STOP;

/* libevent forward declarations */
struct event_base;
struct evhttp;
struct evbuffer;
struct evhttp_request;
struct evkeyvalq;

static inline const char *json_get_str(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static inline bool json_get_bool(cJSON *o, const char *key, bool defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return defv;
}

static inline long json_get_long(cJSON *o, const char *key, long defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return (long)v->valuedouble;
    return defv;
}

static inline void json_add_dup(cJSON *o, const char *key, cJSON *v) {
    if (!v) return;
    cJSON_AddItemToObject(o, key, cJSON_Duplicate(v, 1));
}

static inline char *json_print(cJSON *j) {
    return cJSON_PrintUnformatted(j);
}

static inline void membuf_init(membuf_t *b) {
    b->ptr = NULL; b->len = 0; b->cap = 0;
}

static inline void membuf_free(membuf_t *b) {
    free(b->ptr); b->ptr = NULL; b->len = 0; b->cap = 0;
}

static inline int membuf_append(membuf_t *b, const char *data, size_t n) {
    if (!data || n == 0) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = (char *)realloc(b->ptr, nc);
        if (!p) return -1;
        b->ptr = p; b->cap = nc;
    }
    memcpy(b->ptr + b->len, data, n);
    b->len += n;
    b->ptr[b->len] = 0;
    return 0;
}

/* Utility functions defined in convert.c */
char *xstrdup(const char *s);

#endif