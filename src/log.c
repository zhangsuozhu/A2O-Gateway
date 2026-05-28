/**
 * @file log.c
 * @brief 日志系统实现
 *
 * 实现分级日志记录与实时追踪（realtime trace）功能。
 * - 日志系统：支持 debug/info/warn/error 四级过滤，输出到 stderr 和可选的日志文件。
 * - 实时追踪：在终端实时打印 Claude Code 与上游之间的请求/响应内容，
 *   支持完整 JSON 输出（RT_ALL）和纯文本提取输出（RT_TXT）两种模式。
 *
 * 线程安全：所有日志输出均通过 pthread_mutex_t（LOG_MU）互斥锁保护，
 * 确保多线程并发调用时输出不会交错。
 */

#include "log.h"        /* 日志系统公共接口 */
#include <stdio.h>     /* fopen、fclose、fprintf、fflush、stderr */
#include <time.h>      /* time、localtime_r、strftime */
#include <string.h>    /* strcmp、strlen */
#include <pthread.h>   /* pthread_mutex_lock、pthread_mutex_unlock */
#include <cjson/cJSON.h> /* JSON 解析，用于 RT_TXT 模式的文本提取 */

/* ====================================================================
 * 模块级静态变量
 * ==================================================================== */

/**
 * @brief 日志文件指针
 *
 * 指向以追加模式（"a"）打开的日志文件。为 NULL 时表示未打开日志文件，
 * 此时日志仅输出到标准错误 stderr。
 */
static FILE *LOG_FP = NULL;

/**
 * @brief 日志系统的全局互斥锁
 *
 * 保护 LOG_FP 的文件操作以及日志/实时输出的格式化与写入过程，
 * 防止多线程并发时日志行交错。
 */
static pthread_mutex_t LOG_MU = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 当前日志过滤级别（数值越小越详细）
 *
 * 过滤规则：只输出级别 >= current_level 的日志。
 * - 0 = debug：输出所有日志（最详细）
 * - 1 = info：  输出 info、warn、error（默认）
 * - 2 = warn：  输出 warn、error
 * - 3 = error： 仅输出 error
 */
static int current_level = 1; /* 默认级别为 info，即过滤掉 DEBUG */

/**
 * @brief 实时追踪模式
 *
 * RT_OFF（默认）：不输出实时内容。
 * RT_ALL：        原样输出完整 JSON。
 * RT_TXT：        仅提取并输出文本内容。
 */
static rt_mode_t rt_mode = RT_OFF;

/* ====================================================================
 * 日志文件与级别管理
 * ==================================================================== */

/**
 * @brief 打开或切换日志文件
 *
 * @param path 日志文件路径；若传入 NULL，则关闭当前日志文件
 *
 * 工作流程：
 * 1. 加锁（LOG_MU），防止与其他日志操作并发。
 * 2. 若已有打开的日志文件（LOG_FP != NULL），先关闭并置空。
 * 3. 若 path 非 NULL，以追加模式打开新日志文件。
 * 4. 解锁。
 *
 * 追加模式（"a"）确保多次重启程序后日志不会覆盖历史内容。
 */
void log_open(const char *path) {
    pthread_mutex_lock(&LOG_MU);
    if (LOG_FP) { fclose(LOG_FP); LOG_FP = NULL; }
    if (path) LOG_FP = fopen(path, "a");
    pthread_mutex_unlock(&LOG_MU);
}

/**
 * @brief 设置日志过滤级别
 *
 * @param level 级别字符串："debug"、"info"、"warn"、"error"
 *
 * 将字符串级别映射为内部数值，便于快速比较过滤：
 * - "debug" → 0
 * - "info"  → 1
 * - "warn"  → 2
 * - "error" → 3
 *
 * 若传入 NULL 或无法识别的字符串，函数直接返回，不做任何修改。
 */
void log_set_level(const char *level) {
    if (!level) return;
    if (strcmp(level, "debug") == 0) current_level = 0;
    else if (strcmp(level, "info") == 0) current_level = 1;
    else if (strcmp(level, "warn") == 0) current_level = 2;
    else if (strcmp(level, "error") == 0) current_level = 3;
}

/**
 * @brief 获取当前日志级别的字符串表示
 *
 * @return 与 current_level 对应的级别字符串
 *
 * 内部数值到字符串的反向映射：
 * - 0 → "debug"
 * - 1 → "info"
 * - 2 → "warn"
 * - 3 → "error"
 * - 其他（不应出现）→ "info" 作为安全回退
 */
const char *log_get_level(void) {
    switch (current_level) {
        case 0: return "debug";
        case 1: return "info";
        case 2: return "warn";
        case 3: return "error";
        default: return "info";
    }
}

/* ====================================================================
 * 实时追踪模式管理
 * ==================================================================== */

/**
 * @brief 设置实时追踪模式
 * @param mode 新的 rt_mode_t 值（RT_OFF / RT_ALL / RT_TXT）
 */
void rt_set_mode(rt_mode_t mode) {
    rt_mode = mode;
}

/**
 * @brief 获取当前实时追踪模式
 * @return 当前生效的 rt_mode_t 值
 */
rt_mode_t rt_get_mode(void) {
    return rt_mode;
}

/**
 * @brief 获取当前实时追踪模式的名称字符串
 * @return 模式名称，用于 HTTP API 和管理界面展示
 *
 * 映射规则：
 * - RT_ALL → "all"
 * - RT_TXT → "txt"
 * - RT_OFF 或其他 → "false"
 */
const char *rt_mode_name(void) {
    switch (rt_mode) {
        case RT_ALL: return "all";
        case RT_TXT: return "txt";
        default: return "false";
    }
}

/* ====================================================================
 * 核心日志输出函数
 * ==================================================================== */

/**
 * @brief 记录一条分级日志消息（核心实现）
 *
 * @param level 日志级别字符串（"DEBUG"、"INFO"、"WARN"、"ERROR"）
 * @param fmt   printf 格式字符串
 * @param ...   可变参数
 *
 * 工作流程：
 * 1. 将字符串 level 转换为数值 msg_level，用于与 current_level 比较。
 *    - "DEBUG" → 0，"INFO" → 1，"WARN" → 2，"ERROR" → 3
 * 2. 若 msg_level < current_level，说明当前日志级别设置比消息级别更高
 *    （例如 current_level=info 时，DEBUG 消息被丢弃），直接返回。
 * 3. 生成 ISO 8601 格式的时间戳（如 2024-01-15T09:30:00+0800）。
 * 4. 使用 vsnprintf 将格式字符串和可变参数格式化为临时缓冲区 buf。
 *    若消息超过 8192 字节，截断到缓冲区大小 - 1，避免溢出。
 * 5. 加锁（LOG_MU），确保以下输出操作原子化：
 *    a. 输出到 stderr，格式：[时间戳] 级别 消息内容\n
 *    b. 若 LOG_FP 不为 NULL，同步写入日志文件并 fflush，确保断电不丢日志。
 * 6. 解锁。
 */
void log_msg(const char *level, const char *fmt, ...) {
    /* 将字符串级别映射为数值，用于与 current_level 做快速比较 */
    int msg_level = 1; /* 默认假设为 INFO 级别 */
    if (strcmp(level, "DEBUG") == 0) msg_level = 0;
    else if (strcmp(level, "WARN") == 0) msg_level = 2;
    else if (strcmp(level, "ERROR") == 0) msg_level = 3;

    /* 级别过滤：消息级别低于当前设定级别时，直接丢弃 */
    if (msg_level < current_level) return;

    /* 生成带时区的时间戳 */
    char ts[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);                       /* 线程安全的本地时间转换 */
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv); /* ISO 8601 格式 */

    /* 格式化消息内容到临时缓冲区 */
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);                             /* 初始化可变参数列表 */
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);  /* 安全格式化，防止缓冲区溢出 */
    va_end(ap);                                    /* 清理可变参数列表 */

    /* 处理格式化异常或截断 */
    if (n < 0) n = 0;                             /* vsnprintf 出错时置为 0 */
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1; /* 超长消息截断 */

    /* 加锁并输出到 stderr 和日志文件 */
    pthread_mutex_lock(&LOG_MU);
    fprintf(stderr, "[%s] %-5s %.*s\n", ts, level, n, buf);
    if (LOG_FP) {
        fprintf(LOG_FP, "[%s] %-5s %.*s\n", ts, level, n, buf);
        fflush(LOG_FP); /* 立即刷新到磁盘，降低崩溃时的日志丢失风险 */
    }
    pthread_mutex_unlock(&LOG_MU);
}

/**
 * @brief 实时打印一条消息（用于请求/响应追踪）
 *
 * @param fmt printf 格式字符串
 * @param ... 可变参数
 *
 * 与 log_msg 的区别：
 * - 不检查日志级别，只检查 rt_mode 是否为 RT_OFF。
 * - 输出格式不包含日志级别字段，更简洁。
 * - 用于打印请求/响应正文，而非程序运行日志。
 *
 * 工作流程与 log_msg 类似：生成时间戳 → 格式化消息 → 加锁 → 输出到 stderr
 * 和日志文件 → 解锁。
 */
void rt_print(const char *fmt, ...) {
    if (rt_mode == RT_OFF) return; /* 实时追踪关闭时，直接返回 */

    /* 生成时间戳 */
    char ts[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);

    /* 格式化消息 */
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;

    /* 加锁输出 */
    pthread_mutex_lock(&LOG_MU);
    fprintf(stderr, "[%s] %s\n", ts, buf);
    if (LOG_FP) {
        fprintf(LOG_FP, "[%s] %s\n", ts, buf);
        fflush(LOG_FP);
    }
    pthread_mutex_unlock(&LOG_MU);
}

/* ====================================================================
 * JSON 文本提取（用于 RT_TXT 模式）
 * ==================================================================== */

/**
 * @brief 从 JSON 对象中提取纯文本消息内容
 *
 * @param j     已解析的 cJSON 对象（请求或响应体）
 * @param out   输出缓冲区，提取的文本将追加到此 membuf
 * @param label 标签前缀（如 "-->" 或 "<--"），用于区分请求/响应方向
 *
 * 本函数支持提取以下格式的文本：
 * 1. Anthropic 请求格式中的 system 字段（顶级 system 字符串）。
 * 2. messages 数组中的消息内容（兼容 OpenAI 和 Anthropic 格式）：
 *    - OpenAI 风格：每个消息对象有 role 和 content（字符串）。
 *    - Anthropic 风格：content 为数组，遍历提取 type="text" 的文本块。
 * 3. Anthropic 响应格式中的 content 数组（顶级 content block 数组），
 *    将 type="text" 的块标记为 [assistant] 角色后追加。
 *
 * 对于 OpenAI 响应中的 choices 数组，本函数在 RT_TXT 模式下不做提取，
 * 因为响应内容会在 Anthropic↔OpenAI 转换过程中与流式事件混合，直接提取意义不大。
 */
static void extract_text_from_json(cJSON *j, membuf_t *out, const char *label) {
    if (!j) return; /* 防御性编程：空 JSON 对象直接返回 */

    /* --------------------------------------------------------
     * 1. 提取 Anthropic 请求中的 system 字段
     * -------------------------------------------------------- */
    const char *sys = json_get_str(j, "system");
    if (sys) {
        membuf_append(out, label, strlen(label));
        membuf_append(out, "[system]\n", 9);
        membuf_append(out, sys, strlen(sys));
        membuf_append(out, "\n", 1);
    }

    /* --------------------------------------------------------
     * 2. 提取 messages 数组中的消息文本
     * -------------------------------------------------------- */
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(j, "messages");
    if (cJSON_IsArray(msgs)) {
        cJSON *m;
        /* 遍历 messages 数组中的每个消息对象 */
        cJSON_ArrayForEach(m, msgs) {
            const char *role = json_get_str(m, "role");
            if (!role) continue; /* 跳过没有 role 字段的异常消息 */

            /* 输出标签和角色标识，如 "--> [user]\n" */
            membuf_append(out, label, strlen(label));
            membuf_append(out, "[", 1);
            membuf_append(out, role, strlen(role));
            membuf_append(out, "]\n", 2);

            /* --- 2a. OpenAI 风格：content 为直接字符串 --- */
            const char *content = json_get_str(m, "content");
            if (content) {
                membuf_append(out, content, strlen(content));
                membuf_append(out, "\n", 1);
            } else {
                /* --- 2b. Anthropic 风格：content 为 block 数组 --- */
                cJSON *blocks = cJSON_GetObjectItemCaseSensitive(m, "content");
                if (cJSON_IsArray(blocks)) {
                    cJSON *blk;
                    cJSON_ArrayForEach(blk, blocks) {
                        const char *type = json_get_str(blk, "type");
                        /* 仅提取文本类型的块，忽略 image、tool_use 等 */
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

    /* --------------------------------------------------------
     * 3. OpenAI 响应中的 choices 数组（在 RT_TXT 模式下跳过）
     * -------------------------------------------------------- */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(j, "choices");
    if (cJSON_IsArray(choices) && rt_get_mode() == RT_TXT) {
        /* 不提取 choices 中的 content，因为 OpenAI→Anthropic 转换时
         * 响应内容会与转换逻辑混合，直接提取会造成内容重复或混乱。
         * 若需要查看原始响应，可使用 RT_ALL 模式。 */
    }

    /* --------------------------------------------------------
     * 4. Anthropic 响应中的 content 数组
     * -------------------------------------------------------- */
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
            } else if (type && strcmp(type, "thinking") == 0) {
                const char *think = json_get_str(blk, "thinking");
                if (think) {
                    membuf_append(out, label, strlen(label));
                    membuf_append(out, "[thinking]\n", 11);
                    membuf_append(out, think, strlen(think));
                    membuf_append(out, "\n", 1);
                }
            }
        }
    }
}

/**
 * @brief 实时打印 JSON 内容（根据模式自动选择完整输出或文本提取）
 *
 * @param tag  方向标签，如 "-->"（发送给上游的请求）或 "<--"（来自上游的响应）
 * @param body 要打印的 JSON 字符串或原始文本
 *
 * 工作流程：
 * 1. 若实时追踪关闭（RT_OFF）或 body 为 NULL，直接返回。
 * 2. 若模式为 RT_ALL，直接调用 rt_print 输出 tag + body，保持原始 JSON 完整。
 * 3. 若模式为 RT_TXT：
 *    a. 尝试解析 body 为 JSON。
 *    b. 解析失败时（如上游返回纯文本错误），回退到原样输出 tag + body。
 *    c. 解析成功时，初始化 membuf，调用 extract_text_from_json 提取纯文本。
 *    d. 若提取到内容（buf.len > 0），调用 rt_print 输出。
 *    e. 清理 membuf 和 cJSON 对象，防止内存泄漏。
 */
void rt_print_json(const char *tag, const char *body) {
    if (rt_mode == RT_OFF || !body) return;

    /* RT_ALL 模式：直接输出完整 JSON，不做任何解析或转换 */
    if (rt_mode == RT_ALL) {
        rt_print("%s %s", tag, body);
        return;
    }

    /* RT_TXT 模式：提取纯文本后输出 */
    cJSON *j = cJSON_Parse(body);
    /* JSON 解析失败（如收到 HTML 错误页或非 JSON 响应），回退到原样输出 */
    if (!j) { rt_print("%s %s", tag, body); return; }

    membuf_t buf;
    membuf_init(&buf);
    extract_text_from_json(j, &buf, tag);

    /* 仅当提取到有效内容时才输出，避免打印空行 */
    if (buf.len > 0) {
        rt_print("%s", buf.ptr);
    }

    /* 清理资源：先释放 membuf 再释放 cJSON，顺序无关但建议先释放大块内存 */
    membuf_free(&buf);
    cJSON_Delete(j);
}
