#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

static volatile sig_atomic_t running = 1;

static void handle_chat_completions(struct evhttp_request *req, void *arg)
{
    (void)arg;
    struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, 500, "Internal Error");
        return;
    }

    const char *json =
        "{\"id\":\"mock-123\",\"object\":\"chat.completion\","
        "\"created\":1700000000,\"model\":\"mock-model\","
        "\"choices\":[{\"index\":0,\"message\":{"
        "\"role\":\"assistant\",\"content\":\"Hello from mock server.\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";

    evbuffer_add(buf, json, strlen(json));
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

static void sigint_handler(evutil_socket_t fd, short events, void *arg)
{
    (void)fd;
    (void)events;
    struct event_base *base = arg;
    event_base_loopbreak(base);
    running = 0;
}

int main(int argc, char *argv[])
{
    int port = 9001;
    if (argc > 1) port = atoi(argv[1]);

    struct event_base *base = event_base_new();
    if (!base) {
        fprintf(stderr, "Failed to create event base\n");
        return 1;
    }

    struct evhttp *http = evhttp_new(base);
    if (!http) {
        fprintf(stderr, "Failed to create evhttp\n");
        event_base_free(base);
        return 1;
    }

    evhttp_set_cb(http, "/v1/chat/completions", handle_chat_completions, NULL);

    if (evhttp_bind_socket(http, "0.0.0.0", port) != 0) {
        fprintf(stderr, "Failed to bind to port %d\n", port);
        evhttp_free(http);
        event_base_free(base);
        return 1;
    }

    struct event *sig_ev = evsignal_new(base, SIGINT, sigint_handler, base);
    struct event *sigterm_ev = evsignal_new(base, SIGTERM, sigint_handler, base);
    event_add(sig_ev, NULL);
    event_add(sigterm_ev, NULL);

    printf("Mock OpenAI server listening on 0.0.0.0:%d\n", port);
    event_base_dispatch(base);

    printf("Shutting down mock server...\n");
    event_free(sig_ev);
    event_free(sigterm_ev);
    evhttp_free(http);
    event_base_free(base);
    return 0;
}
