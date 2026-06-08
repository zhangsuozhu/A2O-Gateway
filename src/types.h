/**
 * @file types.h
 * @brief 核心类型定义头文件，包含全局常量、结构体、内联工具函数及外部变量声明
 *
 * 本文件是 Claude Code OpenAI-Compatible Gateway 的基础头文件，
 * 定义了配置管理、流式处理、任务调度等核心模块所需的数据类型和工具函数。
 */

#ifndef TYPES_H
#define TYPES_H

/* 标准库头文件引入 */
#include <stdbool.h>   /* bool 类型定义 */
#include <stddef.h>    /* size_t、NULL 等基础类型 */
#include <stdint.h>    /* 固定宽度整数类型 */
#include <stdlib.h>    /* malloc、free、realloc 等内存管理函数 */
#include <string.h>    /* 字符串操作函数（memcpy、strlen 等） */
#include <signal.h>    /* 信号处理相关定义 */
#include <pthread.h>   /* POSIX 线程支持（互斥锁、条件变量、读写锁） */
#include <cjson/cJSON.h> /* cJSON 库，用于 JSON 解析与构造 */

/* ====================================================================
 * libcurl 前向声明
 * ==================================================================== */

/**
 * @brief libcurl 单句柄（CURL easy handle）的不透明前向声明
 *
 * 使用 void 指针隐藏 libcurl 内部结构，避免在头文件中引入 curl/curl.h，
 * 从而减少编译依赖并加快编译速度。
 */
typedef void CURL;

/**
 * @brief libcurl 多句柄（CURL multi handle）的不透明前向声明
 *
 * CURLM 用于同时管理多个并发的 HTTP 请求，实现非阻塞式 I/O 复用。
 */
typedef void CURLM;

/**
 * @brief libcurl 字符串链表结构的前向声明
 *
 * curl_slist 用于构建 HTTP 请求头链表，以 key: value 形式组织头部字段。
 */
struct curl_slist;

/* ====================================================================
 * 全局常量宏定义
 * ==================================================================== */

#ifndef PATH_MAX
/**
 * @brief 最大文件路径长度
 *
 * 当系统未定义 PATH_MAX 时（如某些 Linux 系统限制不同），
 * 使用 4096 字节作为统一的最大路径长度，足以覆盖绝大多数文件系统路径需求。
 */
#define PATH_MAX 4096
#endif

/**
 * @brief 默认配置文件路径
 *
 * 网关在启动时若未显式指定配置文件，将尝试加载此路径下的 JSON 配置文件。
 */
#define DEFAULT_CONFIG_PATH "./config/gateway.json"

/**
 * @brief 最大并发工具流（tool stream）数量
 *
 * 限制单次请求中可同时跟踪的工具调用流数量，防止内存无限增长。
 * 当超过此数量时，新的工具流将被丢弃或报错。
 */
#define MAX_TOOL_STREAMS 64

/**
 * @brief 最大工作线程（worker thread）数量
 *
 * 定义 libcurl multi 工作线程池的上限。每个工作线程独立管理一个 CURLM 句柄，
 * 负责一组 HTTP 连接的并发执行。设置为 8 可在并发性能与资源占用间取得平衡。
 */
#define MAX_WORKERS 8

/**
 * @brief 单次请求的最大请求体字节数（64 MiB）
 *
 * 防止客户端上传过大的 JSON 请求体导致内存耗尽。
 * 64 MiB 足以容纳包含大量对话历史的请求。
 */
#define MAX_BODY_BYTES (64 * 1024 * 1024)

/**
 * @brief 敏感信息脱敏后的占位字符串
 *
 * 在日志输出或 Web 管理界面中，将 API 密钥等敏感字段替换为此字符串，
 * 避免密钥泄露。
 */
#define MASKED_KEY "***MASKED***"

/* ====================================================================
 * 核心数据结构定义
 * ==================================================================== */

/**
 * @brief 动态内存缓冲区
 *
 * 可自动扩容的字符串缓冲区，用于累积 HTTP 响应体、SSE 流数据等变长内容。
 * 内部使用 realloc 实现指数级扩容策略（初始 8192 字节，后续翻倍），
 * 保证追加操作均摊时间复杂度为 O(1)。
 */
typedef struct membuf {
    char *ptr;      /**< 指向堆内存块的指针，可能为 NULL（空缓冲区） */
    size_t len;     /**< 当前已使用的字节数（不含末尾自动添加的 \0） */
    size_t cap;     /**< 当前分配的内存块总容量（字节） */
} membuf_t;

/**
 * @brief 应用全局配置结构体
 *
 * 封装 JSON 配置文件的内存表示（cJSON 根对象）及其文件路径，
 * 使用读写锁（pthread_rwlock_t）保护，支持多线程并发读取与独占写入。
 * 通过 config.c 中的函数进行加载、查询和热重载。
 */
typedef struct app_config {
    pthread_rwlock_t lock;  /**< 读写锁：允许多个线程并发读取配置，写入时独占 */
    cJSON *root;            /**< cJSON 根对象，指向整个配置文件的内存表示 */
    char path[PATH_MAX];    /**< 配置文件的绝对或相对路径，用于热重载时重新读取 */
} app_config_t;

/**
 * @brief 工具流（Tool Stream）状态跟踪结构体
 *
 * 在 SSE 流式响应中，工具调用（function calling）可能跨多个数据块（chunk）传输。
 * 本结构体用于跟踪单个工具调用的状态：
 * - openai_index: 对应 OpenAI 格式中的 tool_calls[] 数组下标
 * - block_index:  对应 Anthropic 格式中的 content block 下标
 * - started:      是否已接收到工具调用的起始标记
 * - start_emitted: 是否已向客户端发送过工具调用的开始事件
 * - id/name:      工具调用的唯一标识和函数名称
 */
typedef enum {
    JOB_SENDING = 0,  /* 正在发送请求给上游 */
    JOB_WAITING = 1,  /* 请求已发完，等上游返回第一个字节 */
    JOB_RECEIVING = 2 /* 正在接收上游响应数据 */
} job_state_t;

/**
 * @brief 透传模式枚举
 *
 * 描述 gateway 对单次请求所采用的协议处理策略：
 *   - PT_NONE: 协议转换（客户端 Anthropic /v1/messages → 上游 OpenAI /chat/completions）
 *   - PT_ANTHROPIC: Anthropic 透传（客户端 Anthropic → 上游 Anthropic，不转换）
 *   - PT_OPENAI: OpenAI 透传（客户端 OpenAI → 上游 OpenAI，不转换，仅监控）
 *
 * 由 HTTP 路由 + 模型 api_mode 共同决定，详见 handlers.c。
 */
typedef enum {
    PT_NONE = 0,
    PT_ANTHROPIC = 1,
    PT_OPENAI = 2,
} passthrough_mode_t;

typedef struct tool_stream_state {
    int openai_index;      /**< OpenAI 格式中 tool_calls 数组的下标 */
    int block_index;       /**< Anthropic 格式中 content block 的下标 */
    bool started;          /**< 标记该工具流是否已经开始接收数据 */
    bool start_emitted;    /**< 标记是否已向客户端发送 tool_use_start 事件 */
    char *id;              /**< 工具调用的唯一标识符（UUID 格式） */
    char *name;            /**< 被调用的函数名称 */
} tool_stream_state_t;

/**
 * @brief SSE 流式响应状态结构体
 *
 * 管理从上游提供商到客户端的 Server-Sent Events（SSE）流的状态机。
 * 负责将上游的 OpenAI/Anthropic 格式流转换为标准的 SSE 事件序列，
 * 同时处理文本块、工具调用、推理内容（reasoning）的缓冲与发送。
 */
typedef struct stream_state {
    /* 状态标志位 */
    bool reply_started;      /**< 标记是否已开始向客户端发送 SSE 回复 */
    bool message_started;    /**< 标记 message_start 事件是否已发送 */
    bool text_started;       /**< 标记当前是否处于文本块（text block）的传输中 */
    bool thinking_emitted;   /**< 标记是否已发送 thinking 事件（用于 reasoning 模型） */
    bool ended;              /**< 标记流是否已正常结束（收到 message_stop 或等效事件） */

    /* 块索引计数器 */
    int text_block_index;    /**< 当前文本块在 content blocks 中的下标 */
    int next_block_index;    /**< 下一个待分配的 content block 全局下标 */

    /* 行缓冲区：用于解析 SSE 流中的换行分隔数据 */
    char *linebuf;           /**< 行缓冲区的堆内存指针 */
    size_t linebuf_len;      /**< 行缓冲区当前已累积的字节数 */
    size_t linebuf_cap;      /**< 行缓冲区的总容量 */

    /* 消息元数据 */
    char *message_id;        /**< 当前消息的唯一标识符，由上游响应头或首个事件提供 */
    char *client_model;      /**< 客户端请求的模型 ID，用于构建响应中的 model 字段 */
    char *finish_reason;     /**< 流结束原因，如 "stop"、"max_tokens"、"tool_calls" 等 */

    /* 累积的响应文本（非流式或用于日志记录） */
    char *response_text;     /**< 已接收到的完整响应文本拼接结果 */

    /* 待发送文本缓冲区：用于延迟发送，减少网络包数量 */
    char *text_pending;      /**< 待发送文本的临时缓冲区 */
    size_t text_pending_len; /**< 待发送文本的当前长度 */
    size_t text_pending_cap; /**< 待发送文本缓冲区的容量 */

    /* Token 计数（由上游响应或本地估算提供） */
    long prompt_tokens;      /**< 提示（prompt）的 token 数量 */
    long completion_tokens;  /**< 生成内容（completion）的 token 数量 */

    /* 推理内容（如 DeepSeek-R1 的 reasoning_content） */
    char *reasoning_content; /**< 模型思考过程的原始文本内容 */

    /* thinking content block 流式状态 */
    int thinking_block_index; /**< thinking 块在 content blocks 中的下标 */
    bool thinking_started;    /**< 标记是否已开始发送 thinking content block */
    char *thinking_pending;   /**< 待发送 thinking 的临时缓冲区 */
    size_t thinking_pending_len;
    size_t thinking_pending_cap;

    /* 工具流状态数组：并行跟踪多个工具调用 */
    tool_stream_state_t tools[MAX_TOOL_STREAMS];

    /* 单次请求的缓存 token 计数（用于持久化到 request_log） */
    long cache_read_input_tokens;      /**< 缓存读 token 数 */
    long cache_creation_input_tokens;  /**< 缓存写（创建）token 数 */

    /* 统计防重入标志 */
    bool stats_recorded;     /**< 标记 stats_request_end 是否已调用 */
    bool cache_stats_recorded; /**< 标记透传流式缓存统计是否已调用 */
} stream_state_t;

/* 前向声明：避免 worker 和 gateway_job 之间的循环引用 */
typedef struct gateway_job gateway_job_t;

/**
 * @brief libcurl 工作线程结构体
 *
 * 每个工作线程独立运行一个事件循环，管理一个 CURLM 多句柄，
 * 处理分配给它的 HTTP 请求队列（gateway_job）。
 * 使用生产者-消费者模式：主线程将任务加入 pending 队列，工作线程取出并执行。
 */
typedef struct worker {
    pthread_t tid;              /**< 工作线程的 POSIX 线程标识符 */
    pthread_mutex_t mu;         /**< 保护 pending 队列和状态标志的互斥锁 */
    pthread_cond_t cv;          /**< 条件变量：当有新任务加入时唤醒工作线程 */
    CURLM *multi;               /**< libcurl 多句柄，管理本线程内所有并发 HTTP 请求 */
    gateway_job_t *pending_head; /**< 待处理任务队列的头部指针（链表） */
    gateway_job_t *pending_tail; /**< 待处理任务队列的尾部指针（链表） */
    gateway_job_t *active_head;  /**< 正在 curl 中传输的任务链表（用于实时调试） */
    gateway_job_t *active_tail;  /**< 活跃任务链表尾部 */
    int still_running;          /**< 当前 CURLM 中仍在执行中的 easy handle 数量 */
    bool stop;                  /**< 线程停止标志：设为 true 后线程将在合适时机退出 */
    int id;                     /**< 工作线程的数字 ID，用于日志区分 */
} worker_t;

/**
 * @brief 网关任务（HTTP 请求转发任务）结构体
 *
 * 表示一个从客户端接收到的 API 请求及其转发到上游提供商的全过程状态。
 * 包含请求/响应数据、流式状态、libcurl 句柄、发送缓冲区等完整上下文。
 * 该结构体生命周期从收到客户端请求开始，到响应完全发送后结束。
 */
/**
 * @brief 自定义 HTTP 请求头键值对
 *
 * 用于在模型配置中定义需要额外发送或覆盖的上游请求头。
 * key 为头名称，value 为头值；value 为空字符串表示删除该头。
 */
typedef struct {
    char *key;   /**< 请求头名称 */
    char *value; /**< 请求头值（空字符串表示删除） */
} http_header_t;

struct gateway_job {
    /* 客户端请求相关 */
    struct evhttp_request *client_req;  /**< libevent HTTP 请求对象（来自客户端） */
    char *request_body;                 /**< 从客户端读取到的完整请求体（JSON 字符串） */

    /* 上游连接配置 */
    char *upstream_url;     /**< 上游提供商的 API 端点完整 URL */
    char *api_key;          /**< 用于上游认证的实际 API 密钥 */
    char *provider_name;    /**< 上游提供商名称（如 "anthropic"、"openai"），用于路由 */
    char *client_user_agent; /**< 客户端 User-Agent，透传到上游 */
    bool spoof_claude_code_headers; /**< 是否模拟 Claude Code 请求头发送给上游 */

    /* 模型映射 */
    char *client_model;     /**< 客户端请求的模型 ID（如 "claude-3-sonnet"） */
    char *upstream_model;   /**< 实际发送给上游的模型 ID（可能与 client_model 不同） */

    /* 请求模式标志 */
    bool stream;            /**< 是否为流式请求（SSE）：true=流式，false=非流式 */
    passthrough_mode_t passthrough; /**< 透传模式：见 passthrough_mode_t 定义 */
    bool prompt_tokens_includes_cache; /**< provider 的 prompt_tokens 是否包含缓存 tokens */

    /* 实时状态（用于调试面板） */
    job_state_t job_state;  /**< SENDING / WAITING / RECEIVING */

    /* 上游响应状态 */
    bool upstream_headers_done;  /**< 标记是否已接收并处理完上游响应头 */
    bool response_sent;          /**< 标记是否已完成响应发送（避免重复发送） */
    long upstream_status;        /**< 上游 HTTP 状态码（如 200、401、429） */
    membuf_t upstream_body;      /**< 上游响应体的累积缓冲区（主要用于非流式响应） */

    /* 流式状态机（仅 stream=true 时有效） */
    stream_state_t stream_state; /**< SSE 流解析与转换的状态上下文 */

    /* libcurl 句柄与请求头 */
    CURL *easy;                  /**< 本请求对应的 CURL easy handle */
    struct curl_slist *headers;  /**< 发送给上游的 HTTP 请求头链表 */
    http_header_t *extra_headers; /**< 模型配置中自定义的额外请求头 */
    size_t extra_headers_count;   /**< 自定义请求头数量 */

    /* 所属工作线程与链表指针 */
    worker_t *worker;        /**< 处理本任务的工作线程指针 */
    gateway_job_t *next;     /**< 待处理队列中的下一个任务（链表） */
    gateway_job_t *active_next; /**< 活跃任务链表中的下一个（在 curl 中传输时） */

    /* 发送缓冲区（线程安全） */
    pthread_mutex_t send_mu; /**< 保护 send_buf 的互斥锁（主线程与工作线程可能并发访问） */
    struct evbuffer *send_buf; /**< 待发送给客户端的数据缓冲区（libevent evbuffer） */
    bool send_start;         /**< 标记是否已发送响应起始行/头 */
    bool send_end;           /**< 标记是否已发送响应结束标记 */

    /* 非流式响应专用 */
    char *nonstream_json;    /**< 非流式模式下，从上游接收到的完整响应 JSON */
    int nonstream_code;      /**< 非流式模式下，返回给客户端的 HTTP 状态码 */

    /* 统计信息 */
    struct timespec start_time; /**< 任务开始时间（用于计算请求处理延迟） */
};

/* ====================================================================
 * 全局外部变量声明
 * ==================================================================== */

/**
 * @brief libevent 全局事件基座（event_base）
 *
 * 整个应用的核心事件循环句柄，所有网络 I/O、定时器事件均注册于此。
 * 在 main.c 中初始化，由主线程运行 event_base_loop() 驱动。
 */
extern struct event_base *BASE;

/**
 * @brief libevent HTTP 服务器句柄
 *
 * 监听客户端 HTTP 请求的 evhttp 对象，绑定到 BASE 事件基座上。
 * 负责接收来自 Claude Code 或其他客户端的 API 请求。
 */
extern struct evhttp *HTTP;

/**
 * @brief 工作线程数组
 *
 * 大小为 MAX_WORKERS 的静态数组，存储所有工作线程的上下文。
 * 实际使用的工作线程数量由 WORKER_COUNT 决定。
 */
extern worker_t WORKERS[MAX_WORKERS];

/**
 * @brief 实际启用的工作线程数量
 *
 * 从配置文件读取 worker_threads 字段后初始化（范围 1~MAX_WORKERS）。
 */
extern int WORKER_COUNT;

/**
 * @brief 轮询（Round-Robin）调度计数器
 *
 * 用于简单地将新任务轮流分配给各个工作线程，实现基本的负载均衡。
 */
extern unsigned long RR;

/**
 * @brief 全局停止标志
 *
 * 当接收到 SIGINT 或 SIGTERM 信号时，主线程将此标志设为 1，
 * 通知所有工作线程开始优雅退出。使用 volatile sig_atomic_t 保证信号安全。
 */
extern volatile sig_atomic_t STOP;

/**
 * @brief CA 证书 PEM 文本，用于管理员页面下载
 *
 * main.c 中生成 CA 证书时将其 PEM 编码到此缓冲区，
 * handlers.c 中 /admin/ca.pem 路由直接从此处读取。
 */
extern char *g_ca_cert_pem;
extern long g_ca_cert_pem_len;

/* ====================================================================
 * libevent 前向声明
 * ==================================================================== */

/* 以下结构体定义在 libevent 内部，此处仅作不透明前向声明，减少编译依赖 */
struct event_base;       /**< libevent 事件基座结构体 */
struct evhttp;           /**< libevent HTTP 服务器结构体 */
struct evbuffer;         /**< libevent 动态数据缓冲区结构体 */
struct evhttp_request;   /**< libevent HTTP 请求结构体 */
struct evkeyvalq;        /**< libevent 键值对队列（HTTP 头解析结果） */

/* ====================================================================
 * JSON 便捷访问内联函数
 * ==================================================================== */

/**
 * @brief 从 JSON 对象中安全获取字符串字段
 *
 * @param o   cJSON 对象指针
 * @param key 字段名称（区分大小写）
 * @return 字段值为字符串时返回其值，否则返回 NULL
 *
 * 本函数是 cJSON_GetObjectItemCaseSensitive + cJSON_IsString 的安全封装，
 * 避免调用方手动检查类型，减少空指针解引用风险。
 */
static inline const char *json_get_str(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/**
 * @brief 从 JSON 对象中安全获取布尔字段
 *
 * @param o     cJSON 对象指针
 * @param key   字段名称（区分大小写）
 * @param defv  当字段不存在或不是布尔类型时的默认值
 * @return 字段的布尔值，或 defv
 *
 * 常用于获取 JSON 配置中的开关选项，如 "stream": true。
 */
static inline bool json_get_bool(cJSON *o, const char *key, bool defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return defv;
}

/**
 * @brief 从 JSON 对象中安全获取整数字段
 *
 * @param o     cJSON 对象指针
 * @param key   字段名称（区分大小写）
 * @param defv  当字段不存在或不是数字类型时的默认值
 * @return 字段的整数值（通过 valuedouble 转换为 long），或 defv
 *
 * 注意：cJSON 内部使用 double 存储数字，对于超大整数可能存在精度损失。
 */
static inline long json_get_long(cJSON *o, const char *key, long defv) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return (long)v->valuedouble;
    return defv;
}

/**
 * @brief 将 JSON 值深度复制后添加到目标对象
 *
 * @param o   目标 cJSON 对象
 * @param key 新增字段的名称
 * @param v   要添加的 cJSON 值（将被深度复制，原指针保持不变）
 *
 * 若 v 为 NULL，则不做任何操作。使用 cJSON_Duplicate(..., 1) 执行递归深拷贝，
 * 确保添加的值为独立副本，避免多个对象共享同一子树导致的意外修改。
 */
static inline void json_add_dup(cJSON *o, const char *key, cJSON *v) {
    if (!v) return;
    cJSON_AddItemToObject(o, key, cJSON_Duplicate(v, 1));
}

/**
 * @brief 将 cJSON 对象序列化为紧凑格式的 JSON 字符串
 *
 * @param j cJSON 对象指针
 * @return 新分配的 JSON 字符串（调用方需使用 free 释放），失败返回 NULL
 *
 * 生成的字符串不含多余空白（与 cJSON_Print 不同），适合网络传输。
 */
static inline char *json_print(cJSON *j) {
    return cJSON_PrintUnformatted(j);
}

/* ====================================================================
 * 动态内存缓冲区内联函数
 * ==================================================================== */

/**
 * @brief 初始化 membuf 缓冲区
 *
 * @param b 指向 membuf_t 的指针
 *
 * 将缓冲区置为空状态（ptr=NULL, len=0, cap=0）。
 * 初始化后可直接调用 membuf_append 进行数据追加。
 */
static inline void membuf_init(membuf_t *b) {
    b->ptr = NULL; b->len = 0; b->cap = 0;
}

/**
 * @brief 释放 membuf 缓冲区占用的内存
 *
 * @param b 指向 membuf_t 的指针
 *
 * 释放 ptr 指向的堆内存，并将 len/cap 清零。
 * 允许对同一 membuf 多次调用（第二次起无实际效果）。
 */
static inline void membuf_free(membuf_t *b) {
    free(b->ptr); b->ptr = NULL; b->len = 0; b->cap = 0;
}

/**
 * @brief 向 membuf 追加数据
 *
 * @param b    指向 membuf_t 的指针
 * @param data 要追加的数据指针
 * @param n    要追加的字节数
 * @return 成功返回 0，内存分配失败返回 -1
 *
 * 工作原理：
 * 1. 若 data 为 NULL 或 n 为 0，直接返回成功（无操作）。
 * 2. 检查当前容量是否足以容纳新数据（预留 1 字节用于末尾 \0）。
 * 3. 容量不足时，按指数增长策略扩容：初始分配 8192 字节，之后每次翻倍，
 *    直到容量足够。指数策略将均摊时间复杂度降至 O(1)。
 * 4. 使用 memcpy 将数据追加到缓冲区尾部。
 * 5. 更新 len，并在末尾添加 null 终止符，使 ptr 可作为 C 字符串安全使用。
 */
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

/* ====================================================================
 * 其他工具函数声明
 * ==================================================================== */

/**
 * @brief 安全字符串复制函数
 *
 * @param s 源字符串指针
 * @return 新分配的字符串副本（调用方需使用 free 释放），s 为 NULL 时返回 NULL
 *
 * 在 convert.c 中实现，是 strdup 的安全封装，处理 NULL 输入。
 */
char *xstrdup(const char *s);

#endif /* TYPES_H */
