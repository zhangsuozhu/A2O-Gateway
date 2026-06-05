#ifndef STREAM_H
#define STREAM_H

#include <event2/util.h>
#include "types.h"

/* SSE 流输出：向客户端发送一块 SSE 数据
 * 
 * 工作原理：
 * - 检查 job 和 client_req 是否有效，以及流是否已结束/响应已发送
 * - 使用 send_mu 互斥锁保护 send_buf
 * - 如果 send_buf 为空则创建新的 evbuffer
 * - 将数据追加到 send_buf
 * - 通过 event_base_once 触发 deferred_send 回调，在 libevent 事件循环中异步发送
 * 
 * 注意：此函数本身不直接发送数据，而是将数据缓冲后安排异步发送
 */
void stream_send_chunk(gateway_job_t *job, const char *data);

/* 构造并发送一个 SSE JSON 事件
 * 
 * 用途：将 cJSON 对象包装为 SSE 格式并发送
 * 工作原理：
 * - 将 cJSON 对象序列化为无格式 JSON 字符串
 * - 构造 SSE 消息格式：event: <event>\ndata: <json>\n\n
 * - 调用 stream_send_chunk 发送
 * 
 * 参数：
 *   job - 网关任务上下文
 *   event - SSE 事件名称（如 "message_start", "content_block_delta"）
 *   obj - 要发送的 JSON 对象（cJSON 格式）
 */
void stream_emit_json(gateway_job_t *job, const char *event, cJSON *obj);

/* 启动 SSE 响应
 * 
 * 用途：发送 HTTP 响应头，开始 SSE 流
 * 工作原理：
 * - 设置 send_start 标志
 * - 触发 deferred_send 回调，发送 HTTP 200 和 SSE 响应头
 * - 确保只启动一次（通过 reply_started 标志）
 */
void stream_start_reply(gateway_job_t *job);

/* 发送 message_start 事件
 * 
 * 用途：发送 Anthropic Messages API 流式响应的 message_start 事件
 * 包含内容：
 * - message ID（自动生成）
 * - 消息类型、角色、模型
 * - 空的 content 数组
 * - usage 信息（input_tokens, output_tokens=0）
 * 
 * 确保只发送一次（通过 message_started 标志）
 */
void stream_message_start(gateway_job_t *job);

/* 发送 text 内容块开始事件
 * 
 * 用途：在首次发送文本增量之前，发送 content_block_start 事件
 * 标记一个新的文本内容块的开始
 * - 分配 text_block_index（递增的块索引）
 * - 发送类型为 "text" 的内容块开始事件
 * 
 * 确保只发送一次（通过 text_started 标志）
 */
void stream_text_start(gateway_job_t *job);

/* 发送文本增量
 * 
 * 用途：累积文本增量，定期刷新到 SSE 流
 * 工作原理：
 * - 将文本追加到 response_text（完整响应文本，用于日志）
 * - 将文本累积到 text_pending 缓冲区
 * - 当缓冲区大小超过 TEXT_FLUSH_THRESHOLD（4096 字节）时
 *   调用 stream_flush_text 发送 content_block_delta 事件
 * 
 * 这种缓冲机制可以减少 SSE 事件数量，提高传输效率
 */
void stream_text_delta(gateway_job_t *job, const char *text);

/* 获取或创建工具调用流状态
 * 
 * 用途：管理多个并发的工具调用流状态
 * 工作原理：
 * - 首先查找是否已存在对应 openai_index 的工具状态
 * - 如果不存在，找一个未使用的槽位（started=false）
 * - 初始化新槽位：设置 openai_index、block_index
 * 
 * 参数：
 *   s - 流状态结构体
 *   openai_index - OpenAI API 返回的工具调用索引
 * 
 * 返回：工具流状态指针，如果槽位已满返回 NULL
 */
tool_stream_state_t *get_tool_state(stream_state_t *s, int openai_index);

/* 发送工具调用内容块开始事件（如果需要）
 * 
 * 用途：在首次发送工具调用增量之前，发送 content_block_start 事件
 * 确保每个工具调用只发送一次开始事件
 * - 生成工具调用 ID（toolu_xxx）
 * - 发送类型为 "tool_use" 的内容块开始事件
 */
void stream_tool_start_if_needed(gateway_job_t *job, tool_stream_state_t *ts);

/* 发送工具调用 JSON 增量
 * 
 * 用途：发送工具调用参数的 JSON 片段
 * 构造 input_json_delta 事件，包含 partial_json 字段
 */
void stream_tool_json_delta(gateway_job_t *job, tool_stream_state_t *ts, const char *partial);

/* 发送错误事件
 * 
 * 用途：在流式响应中发送错误信息
 * 构造 error 事件，类型为 api_error
 */
void stream_emit_error(gateway_job_t *job, const char *message);

/* 完成 SSE 流
 * 
 * 用途：发送流式响应的结束事件
 * 发送顺序：
 * 1. 刷新剩余的文本缓冲区
 * 2. 发送 text 内容块的 content_block_stop（如果有）
 * 3. 发送所有工具调用的 content_block_stop
 * 4. 发送 message_delta（包含 stop_reason 和 usage）
 * 5. 发送 message_stop
 * 6. 设置 send_end 标志，触发 deferred_send 结束 HTTP 响应
 * 
 * 确保只调用一次（通过 ended 标志）
 */
void stream_finish(gateway_job_t *job);

/* 在流结束前刷新 linebuf 中残留的未闭合 SSE 行 */
void stream_flush_linebuf(gateway_job_t *job);

/* 释放流状态占用的内存
 * 
 * 释放以下资源：
 * - linebuf（SSE 行缓冲区）
 * - message_id（消息 ID）
 * - client_model（客户端模型名）
 * - finish_reason（完成原因）
 * - response_text（完整响应文本）
 * - text_pending（待发送文本缓冲区）
 * - 所有工具调用的 id 和 name
 */
void stream_state_free(stream_state_t *s);

/* 处理 OpenAI 流式 JSON 响应
 * 
 * 用途：解析 OpenAI 的流式响应 JSON，转换为 Anthropic 格式
 * 处理内容：
 * - usage（token 使用量）
 * - finish_reason（完成原因）
 * - delta.content（文本增量）-> 调用 stream_text_delta
 * - delta.tool_calls（工具调用）-> 调用 stream_tool_json_delta
 */
void handle_openai_stream_json(gateway_job_t *job, const char *json);

/* 处理单行 SSE 数据
 * 
 * 用途：解析 SSE 协议的单行数据
 * 工作原理：
 * - 跳过前导空白字符
 * - 检查是否以 "data:" 开头
 * - 提取 data 字段内容
 * - 如果数据为 "[DONE]"，调用 stream_finish 结束流
 * - 否则调用 handle_openai_stream_json 处理 JSON 数据
 */
void process_sse_line(gateway_job_t *job, char *line);

/* 追加数据到 SSE 解析缓冲区
 * 
 * 用途：将 libcurl 接收到的数据追加到行缓冲区，按行解析
 * 工作原理：
 * - 动态扩展 linebuf 缓冲区（按需 2 倍扩容）
 * - 查找换行符，逐行提取
 * - 每找到一行，调用 process_sse_line 处理
 * - 保留未完成的行到缓冲区开头
 */
void stream_parse_append(gateway_job_t *job, const char *ptr, size_t n);

/* libcurl 写回调函数
 * 
 * 用途：libcurl 接收到上游响应数据时调用
 * 处理逻辑：
 * - 将数据追加到 upstream_body（用于日志记录完整响应）
 * - 如果是流式响应（job->stream=true）：
 *   - 启动 SSE 响应
 *   - 追加数据到 SSE 解析缓冲区
 * - 返回接收到的字节数（libcurl 要求）
 */
size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);

/* libcurl 头回调函数
 * 
 * 用途：libcurl 接收到 HTTP 响应头时调用
 * 处理逻辑：
 * - 标记 upstream_headers_done = true（表示已收到响应头）
 * - 记录响应头到日志（DEBUG 级别）
 * - 返回接收到的字节数
 */
size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata);

/* 延迟发送回调（libevent 事件）
 * 
 * 用途：在 libevent 事件循环中异步发送缓冲的数据
 * 处理逻辑：
 * - 如果 send_start=true：发送 HTTP 响应头和 SSE 头
 * - 如果 send_buf 有数据：发送 SSE 数据块
 * - 如果 send_end=true：结束 HTTP 响应
 * - 如果 nonstream_json 有数据：发送非流式 JSON 响应
 * 
 * 通过 event_base_once 触发，确保在主事件循环线程中执行
 */
void deferred_send(evutil_socket_t fd, short what, void *arg);

#endif
