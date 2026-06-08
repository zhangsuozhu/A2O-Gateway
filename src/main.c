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
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <event2/bufferevent_ssl.h>
#endif
#include <cjson/cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "types.h"
#include "log.h"
#include "config.h"
#include "convert.h"
#include "stream.h"
#include "worker.h"
#include "handlers.h"
#include "stats.h"
#include "db.h"
#include "generated/ca_embedded.h"

/* ====================================================================
 * 精灵模式（daemon）相关常量
 * ==================================================================== */

/** @brief PID 文件路径 */
#define DAEMON_PID_FILE "/var/run/cc-oai-gateway.pid"

/** @brief 默认日志文件路径 */
#define DEFAULT_LOG_FILE "/var/log/cc-oai-gateway.log"

/*
 * 以下全局变量在本文件定义，在 types.h 中声明为 extern，
 * 供 worker.c、stream.c 等其他编译单元共享访问。
 */

/** @brief libevent 主事件循环基座，所有事件（定时器、信号、HTTP）均注册于此 */
struct event_base *BASE = NULL;

/** @brief libevent HTTP 服务器对象，负责接收和分发客户端请求 */
struct evhttp *HTTP = NULL;
#ifdef HAVE_SSL
/** @brief libevent HTTPS 服务器对象，仅当 SSL 配置启用时创建 */
static struct evhttp *G_HTTPS = NULL;
/** @brief OpenSSL SSL/TLS 上下文，仅当 SSL 配置启用时创建 */
static SSL_CTX *G_SSL_CTX = NULL;
/** @brief CA 证书 PEM 文本，供管理员页面下载安装 */
char *g_ca_cert_pem = NULL;
long g_ca_cert_pem_len = 0;
#endif

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
 * @brief SIGHUP 信号处理回调 — 用于日志轮转
 * @param sig     信号编号
 * @param events  libevent 事件标志（未使用）
 * @param arg     用户参数（未使用）
 *
 * 收到 SIGHUP 后调用 log_rotate() 重新打开日志文件。
 * 配合 logrotate 使用：logrotate 移动旧日志后发送 SIGHUP，
 * 程序自动打开新的日志文件，无需重启。
 */
static void on_sighup(evutil_socket_t sig, short events, void *arg) {
    (void)sig; (void)events; (void)arg;
    log_rotate();
    log_msg("INFO", "log file rotated (SIGHUP)");
}

/* ====================================================================
 * PID 文件管理
 * ==================================================================== */

/**
 * @brief 写入 PID 文件
 * @param path PID 文件路径
 * @return 成功返回 0，失败返回 -1
 *
 * 以原子方式写入当前进程 PID。若文件已存在，先检查是否为僵尸进程，
 * 若是则覆盖；若不是则返回错误避免误杀其他进程。
 */
static int write_pid_file(const char *path) {
    /* 检查是否已有进程持有 PID 文件 */
    FILE *f = fopen(path, "r");
    if (f) {
        long existing_pid = 0;
        if (fscanf(f, "%ld", &existing_pid) == 1 && existing_pid > 0) {
            /* 检查该 PID 是否仍在运行（不能 kill 则说明已退出） */
            if (kill((pid_t)existing_pid, 0) == 0) {
                fprintf(stderr, "pid file %s exists and process %ld is running\n", path, existing_pid);
                fclose(f);
                return -1;
            }
        }
        fclose(f);
    }

    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}

/**
 * @brief 删除 PID 文件
 * @param path PID 文件路径
 */
static void remove_pid_file(const char *path) {
    unlink(path);
}

/* ====================================================================
 * 精灵模式（daemon）
 * ==================================================================== */

/**
 * @brief 将当前进程转为后台精灵进程
 *
 * 采用标准双 fork 方式实现 daemon 化：
 *   1. 第一次 fork：子进程继续，父进程退出（脱离终端控制）
 *   2. setsid()：创建新会话，成为会话首进程（脱离原控制终端）
 *   3. 第二次 fork：进一步确保进程不能打开控制终端
 *   4. 重定向 stdin/stdout/stderr 到 /dev/null
 *   5. .umask 设为 0，避免文件权限限制
 *   6. 写入 PID 文件
 *
 * 父进程直接 exit(0)，子进程继续执行后续初始化。
 */
static void daemon_fork(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %m\n");
        exit(1);
    }
    if (pid > 0) {
        /* 父进程直接退出，返回子进程 PID */
        printf("daemon started (pid %d)\n", (int)pid);
        exit(0);
    }

    /* 子进程：创建新会话 */
    if (setsid() < 0) {
        fprintf(stderr, "setsid failed: %m\n");
        exit(1);
    }

    /* 第二次 fork 防止打开控制终端 */
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "second fork failed: %m\n");
        exit(1);
    }
    if (pid > 0) {
        exit(0); /* 第一子进程退出 */
    }

    /* 清理环境 */
    umask(0);
    /* 重定向标准文件描述符到 /dev/null */
    { FILE *f = freopen("/dev/null", "r", stdin); (void)f; }
    { FILE *f = freopen("/dev/null", "w", stdout); (void)f; }
    { FILE *f = freopen("/dev/null", "w", stderr); (void)f; }

    /* 写入 PID 文件 */
    if (write_pid_file(DAEMON_PID_FILE) != 0) {
        fprintf(stderr, "failed to write pid file: %s\n", DAEMON_PID_FILE);
        exit(1);
    }
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
#ifdef HAVE_SSL
/**
 * @brief SSL bufferevent 创建回调，用于 evhttp_set_bevcb
 *
 * libevent 在每次接受新的 HTTPS 连接时调用此回调创建 SSL-wrapped bufferevent。
 * arg 为 SSL_CTX 指针，通过 bufferevent_openssl_socket_new 创建服务器模式 SSL 连接。
 * fd = -1 表示 libevent 会在回调返回后自动设置已接受的 socket fd。
 */
static struct bufferevent *ssl_bev_cb(struct event_base *base, void *arg) {
    (void)base;
    SSL_CTX *ctx = (SSL_CTX *)arg;
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return NULL;
    return bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
}
#endif

#ifdef HAVE_SSL
static int sign_server_cert_with_ca(
    X509 *ca_cert, EVP_PKEY *ca_pkey,
    X509 **out_cert, EVP_PKEY **out_key,
    const char *extra_ip)
{
    EVP_PKEY *server_pkey = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    X509 *server_cert = NULL;

    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) return -1;
    if (EVP_PKEY_keygen_init(ctx) <= 0) goto fail;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) goto fail;
    if (EVP_PKEY_keygen(ctx, &server_pkey) <= 0) goto fail;
    EVP_PKEY_CTX_free(ctx); ctx = NULL;

    server_cert = X509_new();
    if (!server_cert) goto fail;
    X509_set_version(server_cert, 2);
    if (!ASN1_INTEGER_set(X509_get_serialNumber(server_cert), (long)time(NULL))) goto fail;
    if (!X509_gmtime_adj(X509_get_notBefore(server_cert), 0)) goto fail;
    if (!X509_gmtime_adj(X509_get_notAfter(server_cert), 3650 * 86400)) goto fail;
    if (!X509_set_pubkey(server_cert, server_pkey)) goto fail;

    X509_NAME *ca_subject = X509_get_subject_name(ca_cert);
    if (!ca_subject) goto fail;
    if (!X509_set_issuer_name(server_cert, ca_subject)) goto fail;

    X509_NAME *name = X509_NAME_new();
    if (!name) goto fail;
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            (const unsigned char *)"LLM-ROUTER", -1, -1, 0)) goto fail;
    if (!X509_set_subject_name(server_cert, name)) goto fail;
    X509_NAME_free(name);

    /* SAN: IP:127.0.0.1 + optional extra IP (from -c), DNS:localhost */
    {
        unsigned char ip127[] = {127,0,0,1};
        ASN1_OCTET_STRING *oct127 = ASN1_OCTET_STRING_new();
        ASN1_OCTET_STRING_set(oct127, ip127, 4);
        GENERAL_NAME *g127 = GENERAL_NAME_new();
        GENERAL_NAME_set0_value(g127, GEN_IPADD, oct127);

        ASN1_IA5STRING *dns = ASN1_IA5STRING_new();
        ASN1_STRING_set(dns, "localhost", 9);
        GENERAL_NAME *gdns = GENERAL_NAME_new();
        GENERAL_NAME_set0_value(gdns, GEN_DNS, dns);

        STACK_OF(GENERAL_NAME) *sk = sk_GENERAL_NAME_new_null();
        sk_GENERAL_NAME_push(sk, g127);
        sk_GENERAL_NAME_push(sk, gdns);

        if (extra_ip && *extra_ip) {
            unsigned char buf[16];
            if (inet_pton(AF_INET, extra_ip, buf) == 1) {
                ASN1_OCTET_STRING *oct = ASN1_OCTET_STRING_new();
                if (oct && ASN1_OCTET_STRING_set(oct, buf, 4)) {
                    GENERAL_NAME *g = GENERAL_NAME_new();
                    if (g) {
                        GENERAL_NAME_set0_value(g, GEN_IPADD, oct);
                        sk_GENERAL_NAME_push(sk, g);
                    } else { ASN1_OCTET_STRING_free(oct); }
                } else { if (oct) ASN1_OCTET_STRING_free(oct); }
            }
        }

        X509_add1_ext_i2d(server_cert, NID_subject_alt_name, sk, 0, 0);
        sk_GENERAL_NAME_pop_free(sk, GENERAL_NAME_free);
    }

    if (!X509_sign(server_cert, ca_pkey, EVP_sha256())) goto fail;

    *out_cert = server_cert;
    *out_key = server_pkey;
    return 0;

fail:
    if (server_cert) X509_free(server_cert);
    if (server_pkey) EVP_PKEY_free(server_pkey);
    if (ctx) EVP_PKEY_CTX_free(ctx);
    return -1;
}
#endif
int main(int argc, char **argv) {
    const char *config_path = getenv("GATEWAY_CONFIG");
    if (!config_path) config_path = DEFAULT_CONFIG_PATH;
    bool daemon_mode = false;
    long cli_port = -1;
    long cli_workers = -1;
    const char *cli_password = NULL;
    const char *cli_cert_ip = NULL;
    /* 解析命令行参数：第一个非配置路径参数为 --daemon */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0) {
            daemon_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: cc-oai-gateway [OPTIONS]\n");
            fprintf(stderr, "  -f, --file PATH      Config file path (default: %s)\n", DEFAULT_CONFIG_PATH);
            fprintf(stderr, "  -p, --port PORT      Listen port (default: 8081)\n");
            fprintf(stderr, "  -P, --password PASS  Admin web UI password (default: empty)\n");
            fprintf(stderr, "  -w, --workers NUM    Worker threads (default: 4, max: %d)\n", MAX_WORKERS);
            fprintf(stderr, "  -d, --daemon         Run as background daemon\n");
            fprintf(stderr, "  -c, --cert-ip IP    Extra SAN IP for server cert (default: 127.0.0.1)\n");
            fprintf(stderr, "  -h, --help           Show this help\n");
            return 0;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 < argc) config_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) cli_port = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--password") == 0) {
            if (i + 1 < argc) cli_password = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workers") == 0) {
            if (i + 1 < argc) cli_workers = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cert-ip") == 0) {
            if (i + 1 < argc) cli_cert_ip = argv[++i];
        } else {
            config_path = argv[i];
        }
    }
    srand((unsigned int)time(NULL));
    signal(SIGPIPE, SIG_IGN);
    if (evthread_use_pthreads() != 0) { fprintf(stderr, "evthread_use_pthreads failed\n"); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (config_load(config_path, cli_port, cli_password, cli_workers) != 0) {
        return 1;
    }
    stats_init();

    /* 精灵模式：fork 到后台运行（必须在创建任何线程之前执行） */
    if (daemon_mode) {
        daemon_fork();
    }

    /* 初始化 SQLite 数据库（持久化统计和历史）
     * 必须在 daemon_fork() 之后，因为 db_init() 会创建批量写入线程，
     * 而 fork() 不会复制子进程中的其他线程 */
    char *db_path = config_get_string_copy("db_path");
    db_init(db_path ? db_path : "/var/log/gateway.db");
    free(db_path);

    /* 从配置读取 log_file，默认使用 gateway.log（兼容旧版）或 /var/log 路径 */
    char *log_file = config_get_string_copy("log_file");
    const char *log_path = log_file ? log_file : "/var/log/gateway.log";
    log_open(log_path);
    free(log_file);

    char *host = config_get_string_copy("listen_host");
    long port = 8081;
    /* Read listen_port from config via a temp copy */
    char *cfg_json = config_masked_json();
    bool http_enabled = true;
    bool https_enabled = true;
    cJSON *root = cJSON_Parse(cfg_json ? cfg_json : "{}");
    free(cfg_json);
    if (root) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "listen_port");
        if (cJSON_IsNumber(p)) port = (long)p->valuedouble;
        cJSON *hep = cJSON_GetObjectItemCaseSensitive(root, "http_enabled");
        cJSON *hsep = cJSON_GetObjectItemCaseSensitive(root, "https_enabled");
        http_enabled = !cJSON_IsFalse(hep);
        https_enabled = !cJSON_IsFalse(hsep);
        cJSON_Delete(root);
    }
    workers_start();
    BASE = event_base_new();
    event_base_priority_init(BASE, 1);
    if (http_enabled) {
        HTTP = evhttp_new(BASE);
        evhttp_set_allowed_methods(HTTP, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_PUT | EVHTTP_REQ_OPTIONS);
        evhttp_set_gencb(HTTP, handle_root, NULL);
        if (evhttp_bind_socket(HTTP, host ? host : "0.0.0.0", (uint16_t)port) != 0) {
            log_msg("ERROR", "HTTP bind failed on %s:%ld", host ? host : "0.0.0.0", port);
            return 1;
        }
     }
#ifdef HAVE_SSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    if (!https_enabled) {
        log_msg("DEBUG", "HTTPS disabled by config (https_enabled=false)");
    }

    if (https_enabled) {
        X509 *ca_cert = NULL;
        EVP_PKEY *ca_key = NULL;
        X509 *server_cert = NULL;
        EVP_PKEY *server_key = NULL;
        /* 从编译嵌入的 CA 证书解析 */
        {
            BIO *bio = BIO_new_mem_buf(CA_CERT, CA_CERT_LEN);
            if (bio) { ca_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL); BIO_free(bio); }
            bio = BIO_new_mem_buf(CA_KEY, CA_KEY_LEN);
            if (bio) { ca_key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL); BIO_free(bio); }
        }
        if (!ca_cert || !ca_key) {
            log_msg("ERROR", "Failed to parse embedded CA certificate, HTTPS disabled");
            if (ca_cert) X509_free(ca_cert);
            if (ca_key) EVP_PKEY_free(ca_key);
        } else if (sign_server_cert_with_ca(ca_cert, ca_key, &server_cert, &server_key, cli_cert_ip) != 0) {
            log_msg("ERROR", "Failed to sign server certificate, HTTPS disabled");
            X509_free(ca_cert);
            EVP_PKEY_free(ca_key);
        } else {
            G_SSL_CTX = SSL_CTX_new(TLS_server_method());
            if (!G_SSL_CTX) {
                log_msg("ERROR", "Failed to create SSL context");
            } else if (SSL_CTX_use_certificate(G_SSL_CTX, server_cert) <= 0) {
                log_msg("ERROR", "Failed to load server cert into SSL context");
                SSL_CTX_free(G_SSL_CTX);
                G_SSL_CTX = NULL;
            } else if (SSL_CTX_use_PrivateKey(G_SSL_CTX, server_key) <= 0) {
                log_msg("ERROR", "Failed to load server key into SSL context");
                SSL_CTX_free(G_SSL_CTX);
                G_SSL_CTX = NULL;
            } else {
                /* PEM-encode CA cert for admin download */
                BIO *bio = BIO_new(BIO_s_mem());
                if (bio) {
                    if (PEM_write_bio_X509(bio, ca_cert)) {
                        char *pem_data;
                        long pem_len = BIO_get_mem_data(bio, &pem_data);
                        g_ca_cert_pem = malloc(pem_len + 1);
                        if (g_ca_cert_pem) {
                            memcpy(g_ca_cert_pem, pem_data, pem_len);
                            g_ca_cert_pem[pem_len] = '\0';
                            g_ca_cert_pem_len = pem_len;
                        }
                    }
                    BIO_free(bio);
                }

                long ssl_port = 8443;
                char *ssl_json = config_masked_json();
                cJSON *sroot = cJSON_Parse(ssl_json ? ssl_json : "{}");
                free(ssl_json);
                if (sroot) {
                    cJSON *sp = cJSON_GetObjectItemCaseSensitive(sroot, "ssl_port");
                    if (cJSON_IsNumber(sp)) ssl_port = (long)sp->valuedouble;
                    cJSON_Delete(sroot);
                }
                G_HTTPS = evhttp_new(BASE);
                evhttp_set_allowed_methods(G_HTTPS, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_PUT | EVHTTP_REQ_OPTIONS);
                evhttp_set_gencb(G_HTTPS, handle_root, NULL);
                evhttp_set_bevcb(G_HTTPS, ssl_bev_cb, G_SSL_CTX);
                if (evhttp_bind_socket(G_HTTPS, host ? host : "0.0.0.0", (uint16_t)ssl_port) != 0) {
                    log_msg("ERROR", "HTTPS bind failed on %s:%ld", host ? host : "0.0.0.0", ssl_port);
                    evhttp_free(G_HTTPS);
                    G_HTTPS = NULL;
                    SSL_CTX_free(G_SSL_CTX);
                    G_SSL_CTX = NULL;
                } else {
                    log_msg("INFO", "HTTPS listening on https://%s:%ld", host ? host : "0.0.0.0", ssl_port);
                    log_msg("INFO", "admin UI over HTTPS: https://%s:%ld/admin", host ? host : "127.0.0.1", ssl_port);
                }
            }
            /* SSL_CTX holds its own references; safe to free originals */
            X509_free(server_cert);
            EVP_PKEY_free(server_key);
            /* Keep CA cert (via g_ca_cert_pem), free key */
            EVP_PKEY_free(ca_key);
            if (!g_ca_cert_pem) X509_free(ca_cert);
        }
    }
#else
    (void)0;
#endif
    struct event *sigint_ev = evsignal_new(BASE, SIGINT, on_signal, NULL);
    struct event *sigterm_ev = evsignal_new(BASE, SIGTERM, on_signal, NULL);
    struct event *sighup_ev = evsignal_new(BASE, SIGHUP, on_sighup, NULL);
    event_add(sigint_ev, NULL);
    event_add(sigterm_ev, NULL);
    event_add(sighup_ev, NULL);
    if (http_enabled) {
        log_msg("INFO", "Claude-Code gateway HTTP listening on http://%s:%ld", host ? host : "0.0.0.0", port);
        log_msg("INFO", "admin UI: http://%s:%ld/admin", host ? host : "127.0.0.1", port);
    }
    log_msg("INFO", "log_level=%s realtime_print=%s",
        log_get_level(), rt_mode_name());
    free(host);

    event_base_dispatch(BASE);
    log_msg("INFO", "shutting down");
    workers_stop();
    evhttp_free(HTTP);
    event_free(sigint_ev);
    event_free(sigterm_ev);
#ifdef HAVE_SSL
    if (G_HTTPS) { evhttp_free(G_HTTPS); G_HTTPS = NULL; }
    if (G_SSL_CTX) { SSL_CTX_free(G_SSL_CTX); G_SSL_CTX = NULL; }
#endif
    event_free(sighup_ev);
    event_base_free(BASE);
    /* Cleanup config via a one-shot */
    char *json = config_masked_json();
    free(json);
    db_close();
    curl_global_cleanup();
    log_open(NULL);
    remove_pid_file(DAEMON_PID_FILE);
    return 0;
}