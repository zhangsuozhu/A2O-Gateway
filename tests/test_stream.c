#include "stream.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct event_base *BASE = NULL;
struct evhttp *HTTP = NULL;
worker_t WORKERS[MAX_WORKERS];
int WORKER_COUNT = 0;
unsigned long RR = 0;
volatile sig_atomic_t STOP = 0;

static char *copy_send_buf(gateway_job_t *job) {
    pthread_mutex_lock(&job->send_mu);
    size_t len = job->send_buf ? evbuffer_get_length(job->send_buf) : 0;
    char *out = (char *)calloc(1, len + 1);
    if (out && len > 0) evbuffer_copyout(job->send_buf, out, len);
    pthread_mutex_unlock(&job->send_mu);
    return out;
}

static int expect_contains(const char *text, const char *needle) {
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "expected stream output to contain %s, got: %s\n",
                needle, text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int expect_before(const char *text, const char *first, const char *second) {
    const char *a = text ? strstr(text, first) : NULL;
    const char *b = text ? strstr(text, second) : NULL;
    if (!a || !b || a >= b) {
        fprintf(stderr, "expected %s before %s, got: %s\n",
                first, second, text ? text : "(null)");
        return 1;
    }
    return 0;
}

static void init_stream_job(gateway_job_t *job) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->send_mu, NULL);
    job->client_req = (struct evhttp_request *)1;
    job->client_model = xstrdup("deepseek-v4-flash");
    job->stream_state.client_model = xstrdup("deepseek-v4-flash");
    BASE = event_base_new();
}

static void cleanup_stream_job(gateway_job_t *job) {
    stream_state_free(&job->stream_state);
    free(job->client_model);
    if (job->send_buf) evbuffer_free(job->send_buf);
    if (BASE) event_base_free(BASE);
    BASE = NULL;
    pthread_mutex_destroy(&job->send_mu);
}

static int test_stream_finish_flushes_pending_reasoning_content(void) {
    gateway_job_t job;
    init_stream_job(&job);

    handle_openai_stream_json(&job,
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"need tool\"},\"finish_reason\":null}]}");
    stream_finish(&job);

    char *out = copy_send_buf(&job);
    int failed = 0;
    failed |= expect_contains(out, "\"type\":\"thinking_delta\"");
    failed |= expect_contains(out, "\"thinking\":\"need tool\"");

    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_tool_calls_flush_pending_reasoning_content_first(void) {
    gateway_job_t job;
    init_stream_job(&job);

    handle_openai_stream_json(&job,
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"need tool\"},\"finish_reason\":null}]}");
    handle_openai_stream_json(&job,
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}");

    char *out = copy_send_buf(&job);
    int failed = 0;
    failed |= expect_contains(out, "\"type\":\"thinking_delta\"");
    failed |= expect_contains(out, "\"type\":\"tool_use\"");
    failed |= expect_before(out, "\"type\":\"thinking_delta\"", "\"type\":\"tool_use\"");

    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_stream_accepts_reasoning_alias(void) {
    gateway_job_t job;
    init_stream_job(&job);

    handle_openai_stream_json(&job,
        "{\"choices\":[{\"delta\":{\"reasoning\":\"alias think\"},\"finish_reason\":null}]}");
    stream_finish(&job);

    char *out = copy_send_buf(&job);
    int failed = 0;
    failed |= expect_contains(out, "\"type\":\"thinking_delta\"");
    failed |= expect_contains(out, "\"thinking\":\"alias think\"");

    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_none_stream_message_start_model_is_gateway_id(void) {
    gateway_job_t job;
    init_stream_job(&job);
    /* init_stream_job sets client_model="deepseek-v4-flash"; assert message_start uses it */

    stream_message_start(&job);

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        const char *p = strstr(out, "data: ");
        if (!p) {
            fprintf(stderr, "expected message_start data line, got: %s\n", out);
            failed = 1;
        } else {
            p += 6;
            cJSON *root = cJSON_Parse(p);
            if (!root) {
                fprintf(stderr, "failed to parse message_start JSON: %.*s\n", 200, p);
                failed = 1;
            } else {
                cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                cJSON *model = msg ? cJSON_GetObjectItemCaseSensitive(msg, "model") : NULL;
                if (!cJSON_IsString(model) || strcmp(model->valuestring, "deepseek-v4-flash") != 0) {
                    fprintf(stderr, "expected message.model == \"deepseek-v4-flash\", got: %s\n",
                            cJSON_IsString(model) ? model->valuestring : "(missing)");
                    failed = 1;
                }
                cJSON_Delete(root);
            }
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_none_stream_other_events_have_no_top_level_model(void) {
    gateway_job_t job;
    init_stream_job(&job);

    /* Trigger text content + finish to emit content_block_start, delta, stop, message_delta, message_stop */
    handle_openai_stream_json(&job,
        "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}");
    stream_finish(&job);

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        /* Iterate over SSE event blocks (separated by \n\n).
         * For each event's data: JSON, assert top-level .model is NULL.
         * (message.model is acceptable, but no event other than message_start
         *  is expected to have a model field at all.) */
        const char *cursor = out;
        int events_checked = 0;
        while (cursor && *cursor) {
            const char *data_prefix = strstr(cursor, "data: ");
            if (!data_prefix) break;
            const char *data_start = data_prefix + 6;
            const char *data_end = strstr(data_start, "\n");
            if (!data_end) data_end = data_start + strlen(data_start);
            size_t data_len = (size_t)(data_end - data_start);
            char *data_str = (char *)calloc(1, data_len + 1);
            if (!data_str) { failed = 1; break; }
            memcpy(data_str, data_start, data_len);
            data_str[data_len] = 0;

            cJSON *root = cJSON_Parse(data_str);
            free(data_str);
            if (root) {
                cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
                if (cJSON_IsString(model)) {
                    fprintf(stderr, "event should not have top-level model, got: %s\n", data_start);
                    failed = 1;
                }
                cJSON_Delete(root);
                events_checked++;
            }
            cursor = data_end + 1;
        }
        if (events_checked < 4) {
            fprintf(stderr, "expected at least 4 events (content_block_start, delta, stop, message_delta, message_stop), got %d\n", events_checked);
            failed = 1;
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static void init_passthru_stream_job(gateway_job_t *job, passthrough_mode_t pt) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->send_mu, NULL);
    job->client_req = (struct evhttp_request *)1;
    job->client_model = xstrdup("qwen-coder-passthru");
    job->passthrough = pt;
    job->stream_state.client_model = xstrdup("qwen-coder-passthru");
    BASE = event_base_new();
}

static int test_pt_anthropic_stream_message_start_model_overridden(void) {
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_ANTHROPIC);

    const char *upstream_sse =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_upstream_1\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"qwen3-coder-plus-2025-01\","
        "\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":3,\"output_tokens\":0}}}\n"
        "\n"
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        const char *data_prefix = strstr(out, "data: ");
        if (!data_prefix) {
            fprintf(stderr, "expected data: line, got: %s\n", out);
            failed = 1;
        } else {
            const char *data_start = data_prefix + 6;
            cJSON *root = cJSON_Parse(data_start);
            if (!root) {
                fprintf(stderr, "failed to parse message_start JSON\n");
                failed = 1;
            } else {
                cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                cJSON *model = msg ? cJSON_GetObjectItemCaseSensitive(msg, "model") : NULL;
                if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
                    fprintf(stderr, "expected message.model overridden, got: %s\n",
                            cJSON_IsString(model) ? model->valuestring : "(missing)");
                    failed = 1;
                }
                /* id should be preserved (passthrough) */
                cJSON *id = msg ? cJSON_GetObjectItemCaseSensitive(msg, "id") : NULL;
                if (!cJSON_IsString(id) || strcmp(id->valuestring, "msg_upstream_1") != 0) {
                    fprintf(stderr, "id should be preserved, got: %s\n",
                            cJSON_IsString(id) ? id->valuestring : "(missing)");
                    failed = 1;
                }
                cJSON_Delete(root);
            }
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_anthropic_stream_other_events_passthrough(void) {
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_ANTHROPIC);

    const char *upstream_sse =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n"
        "\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        /* Each non-message_start event should be byte-identical to upstream */
        if (!strstr(out, "content_block_start")) {
            fprintf(stderr, "expected content_block_start passthrough, got: %s\n", out);
            failed = 1;
        }
        if (!strstr(out, "content_block_delta")) {
            fprintf(stderr, "expected content_block_delta passthrough, got: %s\n", out);
            failed = 1;
        }
        if (!strstr(out, "message_stop")) {
            fprintf(stderr, "expected message_stop passthrough, got: %s\n", out);
            failed = 1;
        }
        /* No model override should have happened on these events */
        if (strstr(out, "qwen3-coder-plus-2025-01")) {
            fprintf(stderr, "upstream model should not appear in non-message_start events, got: %s\n", out);
            failed = 1;
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_openai_stream_each_chunk_model_overridden(void) {
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_OPENAI);

    const char *upstream_sse =
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}\n"
        "\n"
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
        "\n"
        "data: [DONE]\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        /* Each non-[DONE] data chunk should have .model == "qwen-coder-passthru" */
        const char *cursor = out;
        int data_chunks = 0;
        int chunks_with_overridden_model = 0;
        while (cursor && *cursor) {
            const char *data_prefix = strstr(cursor, "data: ");
            if (!data_prefix) break;
            const char *data_start = data_prefix + 6;
            const char *data_end = strstr(data_start, "\n");
            if (!data_end) data_end = data_start + strlen(data_start);
            size_t data_len = (size_t)(data_end - data_start);
            char *data_str = (char *)calloc(1, data_len + 1);
            if (!data_str) { failed = 1; break; }
            memcpy(data_str, data_start, data_len);
            data_str[data_len] = 0;
            if (strcmp(data_str, "[DONE]") != 0) {
                data_chunks++;
                cJSON *root = cJSON_Parse(data_str);
                if (root) {
                    cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
                    if (cJSON_IsString(model) && strcmp(model->valuestring, "qwen-coder-passthru") == 0) {
                        chunks_with_overridden_model++;
                    } else {
                        fprintf(stderr, "chunk model not overridden: %s\n", data_str);
                        failed = 1;
                    }
                    cJSON_Delete(root);
                }
            }
            free(data_str);
            cursor = data_end + 1;
        }
        if (data_chunks != 2) {
            fprintf(stderr, "expected 2 data chunks, got %d\n", data_chunks);
            failed = 1;
        }
        if (chunks_with_overridden_model != 2) {
            fprintf(stderr, "expected 2 chunks with overridden model, got %d\n", chunks_with_overridden_model);
            failed = 1;
        }
        /* [DONE] should be passthrough */
        if (!strstr(out, "data: [DONE]")) {
            fprintf(stderr, "expected [DONE] passthrough, got: %s\n", out);
            failed = 1;
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_anthropic_stream_missing_model_injected(void) {
    /* Scenario 4.1: upstream message_start has no .message.model field.
     * Gateway must inject the gateway model id. */
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_ANTHROPIC);

    const char *upstream_sse =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_no_model\",\"type\":\"message\","
        "\"role\":\"assistant\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) { failed = 1; }
    else {
        const char *data_prefix = strstr(out, "data: ");
        if (!data_prefix) { failed = 1; }
        else {
            cJSON *root = cJSON_Parse(data_prefix + 6);
            if (!root) { failed = 1; }
            else {
                cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                cJSON *model = msg ? cJSON_GetObjectItemCaseSensitive(msg, "model") : NULL;
                if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
                    fprintf(stderr, "missing model should be injected, got: %s\n",
                            cJSON_IsString(model) ? model->valuestring : "(missing)");
                    failed = 1;
                }
                cJSON_Delete(root);
            }
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_openai_stream_empty_model_injected(void) {
    /* Scenario 4.1: upstream chunk has model="" (empty string).
     * Gateway must inject the gateway model id. */
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_OPENAI);

    const char *upstream_sse =
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":null}]}\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) { failed = 1; }
    else {
        const char *data_prefix = strstr(out, "data: ");
        if (!data_prefix) { failed = 1; }
        else {
            cJSON *root = cJSON_Parse(data_prefix + 6);
            if (!root) { failed = 1; }
            else {
                cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
                if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
                    fprintf(stderr, "empty model should be injected, got: %s\n",
                            cJSON_IsString(model) ? model->valuestring : "(missing)");
                    failed = 1;
                }
                cJSON_Delete(root);
            }
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_openai_stream_preserve_newlines(void) {
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_OPENAI);

    const char *upstream_sse =
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}\n"
        "\n"
        "data: [DONE]\n"
        "\n";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        /* 验证每个 data: 行后都有换行符，且空行（消息分隔符）被保留 */
        const char *p = out;
        int newline_after_data_count = 0;
        while ((p = strstr(p, "data: ")) != NULL) {
            p += 6;
            const char *nl = strchr(p, '\n');
            if (!nl) {
                fprintf(stderr, "expected newline after data line, got: %s\n", out);
                failed = 1;
                break;
            }
            newline_after_data_count++;
            p = nl + 1;
        }
        if (newline_after_data_count != 2) {
            fprintf(stderr, "expected 2 data lines with newlines, got %d\n", newline_after_data_count);
            failed = 1;
        }
        /* 空行使得消息之间有两个连续的 \n：data...\n\ndata...\n\n */
        if (!strstr(out, "\n\n")) {
            fprintf(stderr, "expected SSE empty lines (\\n\\n) preserved, got: %s\n", out);
            failed = 1;
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

static int test_pt_openai_stream_flush_linebuf_on_finish(void) {
    gateway_job_t job;
    init_passthru_stream_job(&job, PT_OPENAI);

    /* 模拟上游最后一个包不带 \n：linebuf 中会残留未闭合行 */
    const char *upstream_sse =
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}\n"
        "\n"
        "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}";
    stream_parse_append(&job, upstream_sse, strlen(upstream_sse));

    /* linebuf 中应残留 finish chunk（无末尾 \n） */
    if (job.stream_state.linebuf_len == 0) {
        fprintf(stderr, "expected linebuf residue after incomplete last chunk\n");
        cleanup_stream_job(&job);
        return 1;
    }

    stream_flush_linebuf(&job);
    char *out = copy_send_buf(&job);
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        if (!strstr(out, "finish_reason\":\"stop\"")) {
            fprintf(stderr, "expected finish chunk flushed from linebuf on stream_finish, got: %s\n", out);
            failed = 1;
        }
    }
    free(out);
    cleanup_stream_job(&job);
    return failed;
}

int main(void) {
    int failed = 0;
    failed |= test_stream_finish_flushes_pending_reasoning_content();
    failed |= test_tool_calls_flush_pending_reasoning_content_first();
    failed |= test_stream_accepts_reasoning_alias();
    failed |= test_pt_none_stream_message_start_model_is_gateway_id();
    failed |= test_pt_none_stream_other_events_have_no_top_level_model();
    failed |= test_pt_anthropic_stream_message_start_model_overridden();
    failed |= test_pt_anthropic_stream_other_events_passthrough();
    failed |= test_pt_openai_stream_each_chunk_model_overridden();
    failed |= test_pt_anthropic_stream_missing_model_injected();
    failed |= test_pt_openai_stream_empty_model_injected();
    failed |= test_pt_openai_stream_preserve_newlines();
    failed |= test_pt_openai_stream_flush_linebuf_on_finish();
    return failed ? 1 : 0;
}
