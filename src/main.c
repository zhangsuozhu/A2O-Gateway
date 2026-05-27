#define _GNU_SOURCE

#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "types.h"
#include "log.h"
#include "config.h"
#include "convert.h"
#include "stream.h"
#include "worker.h"
#include "handlers.h"
#include "stats.h"

/* Global variables (defined here, declared extern in types.h) */
struct event_base *BASE = NULL;
struct evhttp *HTTP = NULL;
worker_t WORKERS[MAX_WORKERS];
int WORKER_COUNT = 4;
unsigned long RR = 0;
volatile sig_atomic_t STOP = 0;

static void on_signal(evutil_socket_t sig, short events, void *arg) {
    (void)sig; (void)events; (void)arg;
    STOP = 1;
    if (BASE) event_base_loopexit(BASE, NULL);
}

int main(int argc, char **argv) {
    const char *config_path = getenv("GATEWAY_CONFIG");
    if (!config_path) config_path = DEFAULT_CONFIG_PATH;
    if (argc > 1) config_path = argv[1];
    srand((unsigned int)time(NULL));
    signal(SIGPIPE, SIG_IGN);
    if (evthread_use_pthreads() != 0) { fprintf(stderr, "evthread_use_pthreads failed\n"); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    config_load(config_path);
    stats_init();
    log_open("gateway.log");

    char *host = config_get_string_copy("listen_host");
    long port = 8080;
    /* Read listen_port from config via a temp copy */
    char *cfg_json = config_masked_json();
    cJSON *root = cJSON_Parse(cfg_json ? cfg_json : "{}");
    free(cfg_json);
    if (root) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "listen_port");
        if (cJSON_IsNumber(p)) port = (long)p->valuedouble;
        cJSON_Delete(root);
    }

    workers_start();
    BASE = event_base_new();
    HTTP = evhttp_new(BASE);
    evhttp_set_allowed_methods(HTTP, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_PUT | EVHTTP_REQ_OPTIONS);
    evhttp_set_gencb(HTTP, handle_root, NULL);
    if (evhttp_bind_socket(HTTP, host ? host : "0.0.0.0", (uint16_t)port) != 0) {
        log_msg("ERROR", "bind failed on %s:%ld", host ? host : "0.0.0.0", port);
        return 1;
    }
    struct event *sigint_ev = evsignal_new(BASE, SIGINT, on_signal, NULL);
    struct event *sigterm_ev = evsignal_new(BASE, SIGTERM, on_signal, NULL);
    event_add(sigint_ev, NULL);
    event_add(sigterm_ev, NULL);
    log_msg("INFO", "Claude-Code OpenAI-compatible gateway listening on http://%s:%ld", host ? host : "0.0.0.0", port);
    log_msg("INFO", "admin UI: http://%s:%ld/admin", host ? host : "127.0.0.1", port);
    log_msg("INFO", "log_level=%s realtime_print=%s",
        log_get_level(), rt_mode_name());
    free(host);

    event_base_dispatch(BASE);
    log_msg("INFO", "shutting down");
    workers_stop();
    evhttp_free(HTTP);
    event_free(sigint_ev);
    event_free(sigterm_ev);
    event_base_free(BASE);
    /* Cleanup config via a one-shot */
    char *json = config_masked_json();
    free(json);
    curl_global_cleanup();
    log_open(NULL);
    return 0;
}