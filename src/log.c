#include "log.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <cjson/cJSON.h>

static FILE *LOG_FP = NULL;
static pthread_mutex_t LOG_MU = PTHREAD_MUTEX_INITIALIZER;

/* Log level filtering: default to "info" (skip DEBUG) */
static int current_level = 1; /* 0=debug, 1=info, 2=warn, 3=error */
static rt_mode_t rt_mode = RT_OFF;

void log_open(const char *path) {
    pthread_mutex_lock(&LOG_MU);
    if (LOG_FP) { fclose(LOG_FP); LOG_FP = NULL; }
    if (path) LOG_FP = fopen(path, "a");
    pthread_mutex_unlock(&LOG_MU);
}

void log_set_level(const char *level) {
    if (!level) return;
    if (strcmp(level, "debug") == 0) current_level = 0;
    else if (strcmp(level, "info") == 0) current_level = 1;
    else if (strcmp(level, "warn") == 0) current_level = 2;
    else if (strcmp(level, "error") == 0) current_level = 3;
}

const char *log_get_level(void) {
    switch (current_level) {
        case 0: return "debug";
        case 1: return "info";
        case 2: return "warn";
        case 3: return "error";
        default: return "info";
    }
}

void rt_set_mode(rt_mode_t mode) {
    rt_mode = mode;
}

rt_mode_t rt_get_mode(void) {
    return rt_mode;
}

const char *rt_mode_name(void) {
    switch (rt_mode) {
        case RT_ALL: return "all";
        case RT_TXT: return "txt";
        default: return "false";
    }
}

void log_msg(const char *level, const char *fmt, ...) {
    /* Map string level to numeric for filtering */
    int msg_level = 1;
    if (strcmp(level, "DEBUG") == 0) msg_level = 0;
    else if (strcmp(level, "WARN") == 0) msg_level = 2;
    else if (strcmp(level, "ERROR") == 0) msg_level = 3;

    if (msg_level < current_level) return;

    char ts[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    pthread_mutex_lock(&LOG_MU);
    fprintf(stderr, "[%s] %-5s %.*s\n", ts, level, n, buf);
    if (LOG_FP) {
        fprintf(LOG_FP, "[%s] %-5s %.*s\n", ts, level, n, buf);
        fflush(LOG_FP);
    }
    pthread_mutex_unlock(&LOG_MU);
}

void rt_print(const char *fmt, ...) {
    if (rt_mode == RT_OFF) return;
    char ts[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    pthread_mutex_lock(&LOG_MU);
    fprintf(stderr, "[%s] %s\n", ts, buf);
    if (LOG_FP) {
        fprintf(LOG_FP, "[%s] %s\n", ts, buf);
        fflush(LOG_FP);
    }
    pthread_mutex_unlock(&LOG_MU);
}

static void extract_text_from_json(cJSON *j, membuf_t *out, const char *label) {
    if (!j) return;
    /* Anthropic messages format */
    const char *sys = json_get_str(j, "system");
    if (sys) {
        membuf_append(out, label, strlen(label));
        membuf_append(out, "[system]\n", 9);
        membuf_append(out, sys, strlen(sys));
        membuf_append(out, "\n", 1);
    }
    /* messages array (Anthropic or OpenAI) */
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(j, "messages");
    if (cJSON_IsArray(msgs)) {
        cJSON *m;
        cJSON_ArrayForEach(m, msgs) {
            const char *role = json_get_str(m, "role");
            if (!role) continue;
            membuf_append(out, label, strlen(label));
            membuf_append(out, "[", 1);
            membuf_append(out, role, strlen(role));
            membuf_append(out, "]\n", 2);
            /* OpenAI content string */
            const char *content = json_get_str(m, "content");
            if (content) {
                membuf_append(out, content, strlen(content));
                membuf_append(out, "\n", 1);
            } else {
                /* Anthropic content blocks */
                cJSON *blocks = cJSON_GetObjectItemCaseSensitive(m, "content");
                if (cJSON_IsArray(blocks)) {
                    cJSON *blk;
                    cJSON_ArrayForEach(blk, blocks) {
                        const char *type = json_get_str(blk, "type");
                        if (type && strcmp(type, "text") == 0) {
                            const char *txt = json_get_str(blk, "text");
                            if (txt) {
                                membuf_append(out, txt, strlen(txt));
                                membuf_append(out, "\n", 1);
                            }
                        }
                    }
                }
            }
        }
    }
    /* OpenAI response content */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(j, "choices");
    if (cJSON_IsArray(choices) && rt_get_mode() == RT_TXT) {
        /* Don't extract choices content in RT_TXT since it's mixed with
           the OpenAI->Anthropic conversion in responses */
    }
    /* Anthropic response content blocks */
    cJSON *content_arr = cJSON_GetObjectItemCaseSensitive(j, "content");
    if (cJSON_IsArray(content_arr)) {
        cJSON *blk;
        cJSON_ArrayForEach(blk, content_arr) {
            const char *type = json_get_str(blk, "type");
            if (type && strcmp(type, "text") == 0) {
                const char *txt = json_get_str(blk, "text");
                if (txt) {
                    membuf_append(out, label, strlen(label));
                    membuf_append(out, "[assistant]\n", 12);
                    membuf_append(out, txt, strlen(txt));
                    membuf_append(out, "\n", 1);
                }
            }
        }
    }
}

void rt_print_json(const char *tag, const char *body) {
    if (rt_mode == RT_OFF || !body) return;
    if (rt_mode == RT_ALL) {
        rt_print("%s %s", tag, body);
        return;
    }
    /* RT_TXT mode: extract only text content */
    cJSON *j = cJSON_Parse(body);
    if (!j) { rt_print("%s %s", tag, body); return; }
    membuf_t buf;
    membuf_init(&buf);
    extract_text_from_json(j, &buf, tag);
    if (buf.len > 0) {
        rt_print("%s", buf.ptr);
    }
    membuf_free(&buf);
    cJSON_Delete(j);
}