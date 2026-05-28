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

int main(void) {
    int failed = 0;
    failed |= test_stream_finish_flushes_pending_reasoning_content();
    failed |= test_tool_calls_flush_pending_reasoning_content_first();
    return failed ? 1 : 0;
}
