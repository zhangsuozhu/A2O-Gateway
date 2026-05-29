/**
 * @file log.h
 * @brief 日志系统公共接口头文件
 *
 * 提供分级日志记录（debug/info/warn/error）和实时追踪（realtime trace）功能。
 * 实时追踪用于在终端实时打印 Claude Code 与上游提供商之间的请求/响应内容，
 * 便于调试协议转换问题。
 */

#ifndef LOG_H
#define LOG_H

/* 标准库头文件 */
#include <stdbool.h>   /* bool 类型定义 */
#include <stdarg.h>    /* 可变参数列表（va_list、va_start、va_end） */

/* 引入本项目的核心类型定义（主要使用 membuf_t） */
#include "types.h"

/* ====================================================================
 * 实时追踪模式枚举
 * ==================================================================== */

/**
 * @brief 实时输出模式枚举
 *
 * 控制 rt_print / rt_print_json 的行为，决定终端实时追踪的输出粒度：
 * - RT_OFF: 关闭实时输出（默认），不打印任何请求/响应内容
 * - RT_ALL: 完整输出模式，原样打印所有 JSON 请求和响应
 * - RT_TXT: 仅文本模式，从 JSON 中提取纯文本内容（system/user/assistant 消息）打印
 */
typedef enum { RT_OFF,  /**< 关闭实时追踪，不输出任何内容 */
               RT_ALL,  /**< 完整模式，输出原始 JSON */
               RT_TXT   /**< 纯文本模式，仅提取并输出消息文本 */
             } rt_mode_t;

/* ====================================================================
 * 日志与实时追踪函数接口
 * ==================================================================== */

/**
 * @brief 打开日志文件
 *
 * @param path 日志文件路径；若为 NULL 则关闭当前日志文件（仅输出到 stderr）
 *
 * 以追加模式（"a"）打开日志文件。若已有打开的日志文件，先关闭旧文件。
 * 线程安全：内部使用互斥锁保护文件指针操作。
 */
void log_open(const char *path);

/**
 * @brief 记录一条分级日志消息
 *
 * @param level 日志级别字符串（"DEBUG"、"INFO"、"WARN"、"ERROR"）
 * @param fmt   printf 格式的消息模板
 * @param ...   可变参数列表，与 fmt 格式说明符对应
 *
 * 根据当前设置的日志级别进行过滤。若 msg_level < current_level，则直接丢弃。
 * 消息会同时输出到 stderr 和日志文件（如果已打开），并附加 ISO 8601 格式的时间戳。
 *
 * __attribute__((format(printf, 2, 3))) 让编译器在编译期检查格式字符串与参数类型是否匹配。
 */
void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief 设置全局日志过滤级别
 *
 * @param level 级别字符串："debug"、"info"、"warn"、"error"
 *
 * 级别从低到高为：debug(0) < info(1) < warn(2) < error(3)。
 * 默认级别为 "info"，即不输出 DEBUG 级别的日志。
 * 若传入 NULL 或无法识别的字符串，保持当前级别不变。
 */
void log_set_level(const char *level);

/**
 * @brief 获取当前日志级别的字符串表示
 *
 * @return 当前级别的字符串："debug"、"info"、"warn" 或 "error"
 *
 * 若内部状态异常（不应发生），默认返回 "info"。
 */
const char *log_get_level(void);

/**
 * @brief 设置实时追踪模式
 *
 * @param mode 目标模式：RT_OFF、RT_ALL 或 RT_TXT
 *
 * 通过管理界面或配置项调用，动态控制是否实时打印请求/响应内容到终端。
 */
void rt_set_mode(rt_mode_t mode);

/**
 * @brief 获取当前实时追踪模式
 *
 * @return 当前生效的 rt_mode_t 枚举值
 */
rt_mode_t rt_get_mode(void);

/**
 * @brief 获取当前实时追踪模式的名称字符串
 *
 * @return 模式名称："false"（RT_OFF）、"all"（RT_ALL）或 "txt"（RT_TXT）
 *
 * 返回的字符串用于 HTTP API 响应（如 /admin/status 接口）和管理界面展示。
 */
const char *rt_mode_name(void);

/**
 * @brief 实时打印一条消息（带时间戳）
 *
 * @param fmt printf 格式的消息模板
 * @param ... 可变参数列表
 *
 * 仅在实时追踪模式不为 RT_OFF 时实际输出。
 * 输出格式与 log_msg 类似，但不包含日志级别字段，适合打印请求/响应正文。
 * 线程安全：使用与日志系统相同的互斥锁。
 */
void rt_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief 从 JSON 对象中提取实时追踪文本。
 *
 * 主要供 rt_print_json(RT_TXT) 使用，也暴露给单元测试覆盖协议格式差异。
 */
void rt_extract_text_from_json(cJSON *j, membuf_t *out, const char *label);

/**
 * @brief 实时打印 JSON 内容（支持纯文本提取）
 *
 * @param tag  标签前缀，如 "<--"（上游响应）或 "-->"（客户端请求）
 * @param body JSON 字符串或原始文本内容
 *
 * 根据当前 rt_mode 决定输出方式：
 * - RT_OFF: 不输出
 * - RT_ALL: 直接调用 rt_print 输出 tag + body（完整 JSON）
 * - RT_TXT: 解析 JSON，提取 system/user/assistant 的文本内容后输出
 *
 * 若 body 不是合法 JSON（如纯文本错误信息），在 RT_TXT 模式下回退到原样输出。
 */
void rt_print_json(const char *tag, const char *body);

#endif /* LOG_H */
