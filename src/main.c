/**
 * @file main.c
 * @brief 程序入口和主循环
 *
 * 本文件负责网关进程的完整生命周期管理：
 *   1. 读取配置文件（支持环境变量 GATEWAY_CONFIG 或命令行参数）；
 *   2. 初始化 libevent pthread 支持、libcurl 全局环境；
 *   3. 加载配置并打开日志文件；
 *   4. 启动 worker 线程池；
 *   5. 创建 libevent event_base 和 evhttp 服务器，绑定监听端口；
 *   6. 注册 SIGINT / SIGTERM 信号事件，实现优雅退出；
 *   7. 进入 event_base_dispatch 主循环，直到收到信号；
 *   8. 依次停止 worker、释放 HTTP 服务器、清理 curl 全局资源。
 */

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

/*
 * 以下全局变量在本文件定义，在 types.h 中声明为 extern，
 * 供 worker.c、stream.c 等其他编译单元共享访问。
 */

/** @brief libevent 主事件循环基座，所有事件（定时器、信号、HTTP）均注册于此 */
struct event_base *BASE = NULL;

/** @brief libevent HTTP 服务器对象，负责接收和分发客户端请求 */
struct evhttp *HTTP = NULL;

/** @brief worker 线程池数组，长度由 WORKER_COUNT 决定（最大 MAX_WORKERS） */
worker_t WORKERS[MAX_WORKERS];

/** @brief 实际启动的 worker 线程数量，默认值为 4 */
int WORKER_COUNT = 4;

/** @brief 轮询计数器，用于 enqueue_job 的 round-robin 负载均衡 */
unsigned long RR = 0;

/** @brief 进程停止标志，由信号处理函数设置，主循环据此判断是否需要退出 */
volatile sig_atomic_t STOP = 0;

/**
 * @brief 信号处理回调（SIGINT / SIGTERM）
 * @param sig     信号编号
 * @param events  libevent 事件标志（未使用）
 * @param arg     用户参数（未使用）
 *
 * 设置 STOP = 1，并调用 event_base_loopexit 请求 event_base_dispatch 立即返回。
 * 使用 sig_atomic_t 保证在信号上下文和主循环之间安全读写。
 */
static void on_signal(evutil_socket_t sig, short events, void *arg) {
    (void)sig; (void)events; (void)arg;
    STOP = 1;
    if (BASE) event_base_loopexit(BASE, NULL);
}

/**
 * @brief 主函数
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 0 表示正常退出，非 0 表示初始化失败
 *
 * 启动流程详解：
 *   1. 配置路径解析：
 *      - 优先读取环境变量 GATEWAY_CONFIG；
 *      - 其次使用默认值 DEFAULT_CONFIG_PATH（"./config/gateway.json"）；
 *      - 命令行参数可覆盖上述两者。
 *   2. 忽略 SIGPIPE：防止向已关闭的 TCP 连接写数据导致进程被终止。
 *   3. evthread_use_pthreads()：启用 libevent 的 pthread 锁支持，
 *      使 evbuffer、evhttp 等结构在多线程环境下线程安全。
 *   4. curl_global_init：初始化 libcurl 全局状态（SSL、DNS 等）。
 *   5. config_load + log_open：加载 JSON 配置，打开 gateway.log 日志文件。
 *   6. 监听端口读取：从配置中解析 listen_port，默认 8080。
 *   7. workers_start()：启动后台 worker 线程池，准备处理上游请求。
 *   8. event_base_new + evhttp_new：创建 libevent 主循环与 HTTP 服务器。
 *   9. evhttp_set_allowed_methods：允许 GET、POST、PUT、OPTIONS。
 *  10. evhttp_set_gencb：设置通用回调为 handle_root，处理所有 URI。
 *  11. evhttp_bind_socket：绑定到 listen_host:listen_port，失败则进程退出。
 *  12. evsignal_new + event_add：注册 SIGINT 和 SIGTERM 信号事件。
 *  13. event_base_dispatch：阻塞进入主循环，处理网络事件直到 on_signal 触发退出。
 *
 * 关闭流程（dispatch 返回后）：
 *   - workers_stop()：等待所有 worker 线程结束；
 *   - evhttp_free / event_free / event_base_free：释放 libevent 对象；
 *   - curl_global_cleanup：清理 libcurl 全局资源；
 *   - log_open(NULL)：关闭日志文件。
 */
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