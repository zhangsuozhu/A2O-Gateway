#include "stream.h"
#include "convert.h"
#include "stats.h"
#include "log.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

/* 文本刷新阈值：当待发送文本缓冲区累积超过此大小时，
 * 立即刷新并发送一个 SSE content_block_delta 事件
 * 
 * 目的：减少 SSE 事件数量，避免频繁发送小数据包
 * 4096 字节是一个平衡点，既能减少事件数，又不会让客户端等待太久
 */
#define TEXT_FLUSH_THRESHOLD 4096

static void stream_flush_thinking(gateway_job_t *job);

/* 刷新待发送的文本缓冲区
 * 
 * 内部辅助函数，将 text_pending 缓冲区中的累积文本
 * 作为 content_block_delta 事件发送，然后清空缓冲区
 * 
 * 注意：如果 text_pending 为空，直接返回不做任何操作
 */
static void stream_flush_text(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (!s->text_pending || s->text_pending_len == 0) return;
    stream_text_start(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", s->text_block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "text_delta");
    cJSON_AddStringToObject(delta, "text", s->text_pending);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
    /* 重置待发送缓冲区：清空内容和长度计数 */
    s->text_pending[0] = 0;
    s->text_pending_len = 0;
}

/* 向客户端发送一块 SSE 数据
 * 
 * 工作原理：
 * 1. 安全检查：确保 job 和 client_req 有效，且流未结束
 * 2. 加锁保护 send_buf，防止多线程并发访问
 * 3. 如果 send_buf 为空，创建新的 evbuffer
 * 4. 将数据追加到 send_buf
 * 5. 解锁
 * 6. 通过 event_base_once 安排 deferred_send 回调
 *    这样数据会在 libevent 主事件循环中发送，避免跨线程直接操作 evhttp
 */
void stream_send_chunk(gateway_job_t *job, const char *data) {
    if (!job || !job->client_req || !data) return;
    if (job->stream_state.ended || job->response_sent) return;
    pthread_mutex_lock(&job->send_mu);
    if (!job->send_buf) job->send_buf = evbuffer_new();
    evbuffer_add(job->send_buf, data, strlen(data));
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
}

/* 构造并发送一个 SSE JSON 事件
 * 
 * 工作流程：
 * 1. 将 cJSON 对象序列化为紧凑的 JSON 字符串
 * 2. 构造 SSE 消息格式：
 *    event: <event_name>\n
 *    data: <json_string>\n\n
 * 3. 记录 DEBUG 日志
 * 4. 调用 stream_send_chunk 发送数据
 * 5. 释放临时分配的内存
 */
void stream_emit_json(gateway_job_t *job, const char *event, cJSON *obj) {
    char *txt = cJSON_PrintUnformatted(obj);
    if (!txt) return;
    membuf_t b; membuf_init(&b);
    membuf_append(&b, "event: ", 7);
    membuf_append(&b, event, strlen(event));
    membuf_append(&b, "\n", 1);
    membuf_append(&b, "data: ", 6);
    membuf_append(&b, txt, strlen(txt));
    membuf_append(&b, "\n\n", 2);
    log_msg("DEBUG", "SSE_EMIT model=%s event=%s data=%s", job->client_model, event, txt);
    stream_send_chunk(job, b.ptr);
    membuf_free(&b);
    free(txt);
}

/* 启动 SSE HTTP 响应
 * 
 * 功能：发送 HTTP 响应头，开始 SSE 流式传输
 * 确保只启动一次：通过 reply_started 标志防止重复发送
 * 
 * 操作：
 * - 设置 send_start = true
 * - 触发 deferred_send 发送响应头
 */
void stream_start_reply(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->reply_started) return;
    pthread_mutex_lock(&job->send_mu);
    job->send_start = true;
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
    s->reply_started = true;
}

/* 发送 message_start SSE 事件
 * 
 * 构造 Anthropic Messages API 格式的 message_start 事件：
 * {
 *   "type": "message_start",
 *   "message": {
 *     "id": "msg_xxx",
 *     "type": "message",
 *     "role": "assistant",
 *     "model": "...",
 *     "content": [],
 *     "stop_reason": null,
 *     "stop_sequence": null,
 *     "usage": {
 *       "input_tokens": <prompt_tokens>,
 *       "output_tokens": 0
 *     }
 *   }
 * }
 * 
 * 确保只发送一次（通过 message_started 标志）
 */
void stream_message_start(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->message_started) return;
    if (!s->message_id) s->message_id = make_id("msg");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "message_start");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id", s->message_id);
    cJSON_AddStringToObject(msg, "type", "message");
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "model", s->client_model ? s->client_model : "claude-code-gateway");
    cJSON_AddItemToObject(msg, "content", cJSON_CreateArray());
    cJSON_AddNullToObject(msg, "stop_reason");
    cJSON_AddNullToObject(msg, "stop_sequence");
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "input_tokens", s->prompt_tokens);
    cJSON_AddNumberToObject(usage, "output_tokens", 0);
    cJSON_AddItemToObject(msg, "usage", usage);
    cJSON_AddItemToObject(data, "message", msg);
    stream_emit_json(job, "message_start", data);
    cJSON_Delete(data);
    s->message_started = true;
}

/* 发送 text 内容块开始事件
 * 
 * 构造 content_block_start 事件，标记新的文本块：
 * {
 *   "type": "content_block_start",
 *   "index": <block_index>,
 *   "content_block": {
 *     "type": "text",
 *     "text": ""
 *   }
 * }
 * 
 * 分配递增的 block_index（通过 next_block_index）
 * 确保只发送一次（通过 text_started 标志）
 */
void stream_text_start(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    stream_start_reply(job);
    stream_message_start(job);
    if (s->text_started) return;
    s->text_block_index = s->next_block_index++;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_start");
    cJSON_AddNumberToObject(data, "index", s->text_block_index);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", "");
    cJSON_AddItemToObject(data, "content_block", blk);
    stream_emit_json(job, "content_block_start", data);
    cJSON_Delete(data);
    s->text_started = true;
}

/* 将文本追加到待发送缓冲区
 * 
 * 内部辅助函数：动态管理 text_pending 缓冲区
 * - 如果当前容量不足，按 2 倍扩容（初始 8192 字节）
 * - 使用 memcpy 追加文本（包括结尾的 \0）
 */
static void accum_pending(stream_state_t *s, const char *text) {
    size_t len = strlen(text);
    if (s->text_pending_len + len + 1 > s->text_pending_cap) {
        size_t nc = s->text_pending_cap ? s->text_pending_cap * 2 : 8192;
        while (nc < s->text_pending_len + len + 1) nc *= 2;
        char *p = (char *)realloc(s->text_pending, nc);
        if (!p) return;
        s->text_pending = p;
        s->text_pending_cap = nc;
    }
    memcpy(s->text_pending + s->text_pending_len, text, len + 1);
    s->text_pending_len += len;
}

/* 接收文本增量并缓冲
 * 
 * 双重用途：
 * 1. 累积到 response_text：保存完整响应文本（用于日志和后续处理）
 * 2. 累积到 text_pending：缓冲待发送的文本
 * 
 * 缓冲策略：
 * - 文本先进入 text_pending 缓冲区
 * - 当缓冲区大小达到 TEXT_FLUSH_THRESHOLD（4096 字节）时
 *   调用 stream_flush_text 发送 content_block_delta 事件
 * - 这样可以合并多个小文本增量，减少 SSE 事件数量
 */
void stream_text_delta(gateway_job_t *job, const char *text) {
    if (!text || !*text) return;
    stream_state_t *s = &job->stream_state;
    if (s->response_text) {
        size_t old = strlen(s->response_text);
        size_t newlen = old + strlen(text);
        char *p = (char *)realloc(s->response_text, newlen + 1);
        if (p) { s->response_text = p; memcpy(s->response_text + old, text, newlen - old + 1); }
    } else {
        s->response_text = xstrdup(text);
    }
    /* 累积到缓冲区，仅在缓冲区足够大时刷新 */
    accum_pending(s, text);
    if (s->text_pending_len >= TEXT_FLUSH_THRESHOLD) {
        stream_flush_text(job);
    }
}

/* 获取或创建工具调用流状态
 * 
 * 实现一个固定大小的工具状态池（MAX_TOOL_STREAMS 个槽位）
 * 
 * 查找逻辑：
 * 1. 第一轮遍历：查找已存在且 openai_index 匹配的槽位
 * 2. 第二轮遍历：查找未使用的槽位（started=false），初始化后返回
 * 
 * 每个槽位包含：
 * - openai_index：OpenAI API 返回的工具调用索引
 * - block_index：在 Anthropic 响应中的内容块索引（递增分配）
 * - started：标记此槽位是否已使用
 */
tool_stream_state_t *get_tool_state(stream_state_t *s, int openai_index) {
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (s->tools[i].started && s->tools[i].openai_index == openai_index) return &s->tools[i];
    }
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (!s->tools[i].started) {
            s->tools[i].openai_index = openai_index;
            s->tools[i].block_index = s->next_block_index++;
            s->tools[i].started = true;
            return &s->tools[i];
        }
    }
    return NULL;
}

/* 发送工具调用开始事件（按需）
 * 
 * 功能：确保每个工具调用只发送一次 content_block_start 事件
 * 
 * 构造内容：
 * {
 *   "type": "content_block_start",
 *   "index": <block_index>,
 *   "content_block": {
 *     "type": "tool_use",
 *     "id": "toolu_xxx",
 *     "name": "...",
 *     "input": {}
 *   }
 * }
 * 
 * 延迟初始化：如果 id 或 name 未设置，使用默认值
 * 通过 start_emitted 标志确保只发送一次
 */
void stream_tool_start_if_needed(gateway_job_t *job, tool_stream_state_t *ts) {
    stream_start_reply(job);
    stream_message_start(job);
    if (!ts) return;
    if (!ts->id) ts->id = make_id("toolu");
    if (!ts->name) ts->name = xstrdup("tool");
    if (ts->start_emitted) return;
    ts->start_emitted = true;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_start");
    cJSON_AddNumberToObject(data, "index", ts->block_index);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "tool_use");
    cJSON_AddStringToObject(blk, "id", ts->id);
    cJSON_AddStringToObject(blk, "name", ts->name);
    cJSON_AddItemToObject(blk, "input", cJSON_CreateObject());
    cJSON_AddItemToObject(data, "content_block", blk);
    stream_emit_json(job, "content_block_start", data);
    cJSON_Delete(data);
}

/* 发送工具调用 JSON 参数增量
 * 
 * 构造 input_json_delta 事件：
 * {
 *   "type": "content_block_delta",
 *   "index": <block_index>,
 *   "delta": {
 *     "type": "input_json_delta",
 *     "partial_json": "..."
 *   }
 * }
 * 
 * 将 OpenAI 的 function.arguments 片段转换为 Anthropic 的 input_json_delta
 */
void stream_tool_json_delta(gateway_job_t *job, tool_stream_state_t *ts, const char *partial) {
    if (!ts || !partial || !*partial) return;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", ts->block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "input_json_delta");
    cJSON_AddStringToObject(delta, "partial_json", partial);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
}

/* 发送流式错误事件
 * 
 * 构造 error 事件：
 * {
 *   "type": "error",
 *   "error": {
 *     "type": "api_error",
 *     "message": "..."
 *   }
 * }
 * 
 * 先启动 SSE 响应（如果尚未启动），然后发送错误
 */
void stream_emit_error(gateway_job_t *job, const char *message) {
    stream_start_reply(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "error");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", "api_error");
    cJSON_AddStringToObject(e, "message", message ? message : "upstream error");
    cJSON_AddItemToObject(data, "error", e);
    stream_emit_json(job, "error", data);
    cJSON_Delete(data);
}

/* 完成并结束 SSE 流
 * 
 * 发送完整的结束序列（严格按顺序）：
 * 
 * 1. 刷新剩余文本（stream_flush_text）
 *    - 发送最后一批累积的文本增量
 * 
 * 2. 发送 text 内容块的 content_block_stop（如果文本块已启动）
 * 
 * 3. 发送所有工具调用的 content_block_stop
 *    - 遍历 tools 数组，为每个已启动的工具调用发送停止事件
 * 
 * 4. 发送 message_delta
 *    - 包含 stop_reason（映射自 OpenAI 的 finish_reason）
 *    - 包含 usage.output_tokens
 * 
 * 5. 发送 message_stop
 *    - 标记消息流正式结束
 * 
 * 6. 结束 HTTP 响应
 *    - 设置 send_end = true
 *    - 触发 deferred_send 发送结束信号
 *    - 标记 response_sent 和 ended
 * 
 * 7. 记录实时日志（响应摘要和响应体）
 */
void stream_finish(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (s->ended) return;
    if (job->passthrough != PT_NONE) {
        /* 透传模式：上游已原样转发所有事件，不再额外发送结束事件 */
        pthread_mutex_lock(&job->send_mu);
        job->send_end = true;
        pthread_mutex_unlock(&job->send_mu);
        event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
        job->response_sent = true;
        s->ended = true;
        rt_print("[RES] model=%s type=stream(passthrough=%d) upstream_status=%ld prompt_tokens=%ld completion_tokens=%ld",
            job->client_model, (int)job->passthrough, job->upstream_status, s->prompt_tokens, s->completion_tokens);
        return;
    }
    stream_start_reply(job);
    stream_message_start(job);
    /* 刷新剩余的 thinking/text 缓冲区，保持 reasoning_content 先于正文输出。 */
    stream_flush_thinking(job);
    stream_flush_text(job);
    if (s->text_started) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "type", "content_block_stop");
        cJSON_AddNumberToObject(data, "index", s->text_block_index);
        stream_emit_json(job, "content_block_stop", data);
        cJSON_Delete(data);
    }
    if (s->thinking_started) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "type", "content_block_stop");
        cJSON_AddNumberToObject(data, "index", s->thinking_block_index);
        stream_emit_json(job, "content_block_stop", data);
        cJSON_Delete(data);
    }
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        if (s->tools[i].started) {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "type", "content_block_stop");
            cJSON_AddNumberToObject(data, "index", s->tools[i].block_index);
            stream_emit_json(job, "content_block_stop", data);
            cJSON_Delete(data);
        }
    }
    cJSON *delta_ev = cJSON_CreateObject();
    cJSON_AddStringToObject(delta_ev, "type", "message_delta");
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "stop_reason", map_finish_reason(s->finish_reason));
    cJSON_AddNullToObject(delta, "stop_sequence");
    cJSON_AddItemToObject(delta_ev, "delta", delta);
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "output_tokens", s->completion_tokens > 0 ? s->completion_tokens : 0);
    cJSON_AddItemToObject(delta_ev, "usage", usage);
    stream_emit_json(job, "message_delta", delta_ev);
    cJSON_Delete(delta_ev);

    cJSON *stop = cJSON_CreateObject();
    cJSON_AddStringToObject(stop, "type", "message_stop");
    stream_emit_json(job, "message_stop", stop);
    cJSON_Delete(stop);
    pthread_mutex_lock(&job->send_mu);
    job->send_end = true;
    pthread_mutex_unlock(&job->send_mu);
    event_base_once(BASE, -1, EV_TIMEOUT, deferred_send, job, NULL);
    job->response_sent = true;
    s->ended = true;

    /* 流完成后的实时打印日志 */
    rt_print("[RES] model=%s type=stream upstream_status=%ld prompt_tokens=%ld completion_tokens=%ld",
        job->client_model, job->upstream_status, s->prompt_tokens, s->completion_tokens);
    if (job->upstream_body.ptr && job->upstream_body.len > 0) {
        if (rt_get_mode() == RT_TXT) {
            if (s->reasoning_content && s->reasoning_content[0]) {
                rt_print("[RES_THINK] model=%s %s", job->client_model, s->reasoning_content);
            }
            rt_print("[RES_BODY] model=%s %s%s", job->client_model,
                s->response_text ? s->response_text : "",
                s->finish_reason ? s->finish_reason : "");
        } else {
            rt_print("[RES_BODY] model=%s %s", job->client_model, job->upstream_body.ptr);
        }
    }
}

/* 释放流状态内存
 * 
 * 清理所有动态分配的资源，防止内存泄漏：
 * - linebuf：SSE 行解析缓冲区
 * - message_id：自动生成的消息 ID
 * - client_model：客户端指定的模型名称
 * - finish_reason：上游返回的完成原因
 * - response_text：完整响应文本（用于日志）
 * - text_pending：待发送文本缓冲区
 * - tools[].id：工具调用 ID
 * - tools[].name：工具调用名称
 * 
 * 注意：stream_state 结构体本身由调用者管理，此函数只释放内部指针
 */
void stream_state_free(stream_state_t *s) {
    free(s->linebuf);
    free(s->message_id);
    free(s->client_model);
    free(s->finish_reason);
    free(s->response_text);
    free(s->text_pending);
    free(s->reasoning_content);
    free(s->thinking_pending);
    for (int i = 0; i < MAX_TOOL_STREAMS; i++) {
        free(s->tools[i].id);
        free(s->tools[i].name);
    }
}

/* 发送 thinking 内容块开始事件
 *
 * 构造 content_block_start 事件，标记新的 thinking 块：
 * {
 *   "type": "content_block_start",
 *   "index": <block_index>,
 *   "content_block": {
 *     "type": "thinking",
 *     "thinking": ""
 *   }
 * }
 *
 * 分配递增的 block_index（通过 next_block_index）
 * 确保只发送一次（通过 thinking_started 标志）
 */
static void stream_thinking_start(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    stream_start_reply(job);
    stream_message_start(job);
    if (s->thinking_started) return;
    s->thinking_block_index = s->next_block_index++;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_start");
    cJSON_AddNumberToObject(data, "index", s->thinking_block_index);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "thinking");
    cJSON_AddStringToObject(blk, "thinking", "");
    cJSON_AddItemToObject(data, "content_block", blk);
    stream_emit_json(job, "content_block_start", data);
    cJSON_Delete(data);
    s->thinking_started = true;
}

/* 累积 reasoning_content 到 thinking_pending 缓冲区（带阈值刷新）
 * 
 * 功能：减少 SSE 事件数量，避免每个 reasoning_content delta 都触发网络写
 * - 初始容量 8192 字节，按 2 倍扩容
 * - 达到 THINKING_FLUSH_THRESHOLD（4096）时发送 thinking_delta
 */
static void accum_thinking(stream_state_t *s, const char *text) {
    size_t len = strlen(text);
    if (s->thinking_pending_len + len + 1 > s->thinking_pending_cap) {
        size_t nc = s->thinking_pending_cap ? s->thinking_pending_cap * 2 : 8192;
        while (nc < s->thinking_pending_len + len + 1) nc *= 2;
        char *p = (char *)realloc(s->thinking_pending, nc);
        if (!p) return;
        s->thinking_pending = p;
        s->thinking_pending_cap = nc;
    }
    memcpy(s->thinking_pending + s->thinking_pending_len, text, len + 1);
    s->thinking_pending_len += len;
}

/* 刷新 thinking_pending 缓冲区并发送 content_block_delta */
static void stream_flush_thinking(gateway_job_t *job) {
    stream_state_t *s = &job->stream_state;
    if (!s->thinking_pending || s->thinking_pending_len == 0) return;
    stream_thinking_start(job);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "content_block_delta");
    cJSON_AddNumberToObject(data, "index", s->thinking_block_index);
    cJSON *delta = cJSON_CreateObject();
    cJSON_AddStringToObject(delta, "type", "thinking_delta");
    cJSON_AddStringToObject(delta, "thinking", s->thinking_pending);
    cJSON_AddItemToObject(data, "delta", delta);
    stream_emit_json(job, "content_block_delta", data);
    cJSON_Delete(data);
    s->thinking_pending[0] = 0;
    s->thinking_pending_len = 0;
}

/* 解析 OpenAI 流式响应 JSON
 * 
 * 核心转换函数：将 OpenAI Chat Completions 流式格式
 * 转换为内部状态，触发相应的 Anthropic 格式事件
 * 
 * 解析字段：
 * 1. usage：token 使用量（prompt_tokens, completion_tokens）
 * 2. choices[0].finish_reason：完成原因
 * 3. choices[0].delta.content：文本增量 -> stream_text_delta
 * 4. choices[0].delta.reasoning_content：thinking 增量 -> stream_thinking_delta
 * 5. choices[0].delta.tool_calls：工具调用
 *    - index：工具调用索引（可能多个工具并行）
 *    - id：工具调用 ID
 *    - function.name：工具名称
 *    - function.arguments：工具参数（JSON 字符串或对象）
 *      -> stream_tool_json_delta
 * 
 * 参数：
 *   json - OpenAI API 返回的单行 JSON 字符串
 */
/* 从一个已解析的 OpenAI 流式响应 cJSON 对象中提取 usage 与缓存 token，
 * 更新 job 的统计字段并写入 stats 系统。
 * 既被 handle_openai_stream_json（协议转换路径）使用，
 * 也被 extract_openai_usage_only（OpenAI 透传路径）使用，
 * 后者无需触发任何 Anthropic SSE 事件。 */
static void extract_openai_usage_from_obj(gateway_job_t *job, cJSON *root) {
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (!cJSON_IsObject(usage)) return;
    long pt = json_get_long(usage, "prompt_tokens", -1);
    long ct = json_get_long(usage, "completion_tokens", -1);
    /* 提示词缓存：跨 provider 字段名兼容
     * Anthropic: cache_read_input_tokens, cache_creation_input_tokens
     * DeepSeek:  prompt_cache_hit_tokens
     * OpenAI/Moonshot: usage.prompt_tokens_details.cached_tokens
     * 注意：prompt_cache_miss_tokens 是 prompt_tokens 的子集（未命中部分），
     * 不是 cache_creation，不应映射到 cache_creation_input_tokens */
    long cache_read = 0, cache_creation = 0;
    cache_read = json_get_long(usage, "cache_read_input_tokens", 0);
    if (cache_read == 0) cache_read = json_get_long(usage, "prompt_cache_hit_tokens", 0);
    if (cache_read == 0) {
        cJSON *details = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
        if (cJSON_IsObject(details)) {
            cache_read = json_get_long(details, "cached_tokens", 0);
        }
    }
    cache_creation = json_get_long(usage, "cache_creation_input_tokens", 0);
    /* provider 的 prompt_tokens 不包含缓存 tokens，需合并 */
    log_msg("DEBUG", "STREAM_CACHE_FIX model=%s includes=%s pt_before=%ld cr=%ld cc=%ld",
            job->client_model, job->prompt_tokens_includes_cache ? "true" : "false", pt, cache_read, cache_creation);
    if (!job->prompt_tokens_includes_cache && pt > 0) {
        pt = pt + cache_read + cache_creation;
        log_msg("DEBUG", "STREAM_CACHE_FIX model=%s pt_after=%ld", job->client_model, pt);
    }
    if (pt >= 0) {
        job->stream_state.prompt_tokens = pt;
        log_msg("DEBUG", "STREAM_USAGE model=%s prompt_tokens=%ld", job->client_model, pt);
    }
    if (ct >= 0) {
        job->stream_state.completion_tokens = ct;
        log_msg("DEBUG", "STREAM_USAGE model=%s completion_tokens=%ld", job->client_model, ct);
    }
    job->stream_state.cache_read_input_tokens = cache_read;
    job->stream_state.cache_creation_input_tokens = cache_creation;
    if (cache_read > 0 || cache_creation > 0) {
        const char *m = job->upstream_model ? job->upstream_model : job->client_model;
        const char *p = job->provider_name ? job->provider_name : "unknown";
        if (cache_read > 0)
            stats_record_cache_read(m, p, (unsigned long)cache_read);
        if (cache_creation > 0)
            stats_record_cache_creation(m, p, (unsigned long)cache_creation);
        log_msg("DEBUG", "STREAM_CACHE model=%s cache_read=%ld cache_creation=%ld",
                job->client_model, cache_read, cache_creation);
    }
}

/* OpenAI 透传模式专用：仅从一条流式 JSON 中提取 usage 用于监控，
 * 不触发任何 Anthropic SSE 事件。所有字节已经在 stream_parse_append
 * 中原样转发给客户端。 */
static void extract_openai_usage_only(gateway_job_t *job, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    extract_openai_usage_from_obj(job, root);
    cJSON_Delete(root);
}

void handle_openai_stream_json(gateway_job_t *job, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    extract_openai_usage_from_obj(job, root);
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *ch = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!ch) { cJSON_Delete(root); return; }
    const char *fr = json_get_str(ch, "finish_reason");
    if (fr) {
        free(job->stream_state.finish_reason);
        job->stream_state.finish_reason = xstrdup(fr);
    }
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(ch, "delta");
    if (cJSON_IsObject(delta)) {
        /* 累积并流式转发上游 reasoning_content/reasoning */
        const char *rc = json_get_str(delta, "reasoning_content");
        if (!rc) rc = json_get_str(delta, "reasoning");
        if (rc && *rc) {
            stream_state_t *s = &job->stream_state;
            size_t old_len = s->reasoning_content ? strlen(s->reasoning_content) : 0;
            size_t add_len = strlen(rc);
            char *new_rc = (char *)realloc(s->reasoning_content, old_len + add_len + 1);
            if (new_rc) {
                s->reasoning_content = new_rc;
                memcpy(s->reasoning_content + old_len, rc, add_len + 1);
            }
            /* 缓冲 thinking delta，达到阈值时批量发送，减少 SSE 事件数 */
            accum_thinking(s, rc);
            if (s->thinking_pending_len >= TEXT_FLUSH_THRESHOLD) {
                stream_flush_thinking(job);
            }
        }
        const char *content = json_get_str(delta, "content");
        if (content) {
            stream_flush_thinking(job);
            stream_text_delta(job, content);
        }
        cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        if (cJSON_IsArray(tool_calls)) {
            stream_flush_thinking(job);
            cJSON *tc;
            cJSON_ArrayForEach(tc, tool_calls) {
                int oi = (int)json_get_long(tc, "index", 0);
                tool_stream_state_t *ts = get_tool_state(&job->stream_state, oi);
                const char *id = json_get_str(tc, "id");
                if (id && !ts->id) ts->id = xstrdup(id);
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
                const char *name = fn ? json_get_str(fn, "name") : NULL;
                if (name && !ts->name) ts->name = xstrdup(name);
                stream_tool_start_if_needed(job, ts);
                cJSON *args_obj = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                if (cJSON_IsString(args_obj) && args_obj->valuestring && args_obj->valuestring[0]) {
                    stream_tool_json_delta(job, ts, args_obj->valuestring);
                } else if (cJSON_IsObject(args_obj)) {
                    char *args_str = cJSON_PrintUnformatted(args_obj);
                    if (args_str) {
                        stream_tool_json_delta(job, ts, args_str);
                        free(args_str);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

/* 从 Anthropic 流式响应中提取 usage 信息（仅用于透传模式统计）
 *
 * Anthropic SSE 中 usage 可能出现在：
 * - message_start 事件：嵌套在 message.usage 中
 * - message_delta 事件：直接出现在 usage 字段中
 * - message_stop 事件：直接出现在 usage 字段中
 */
static void extract_anthropic_cache_stats(gateway_job_t *job, cJSON *usage) {
    if (job->stream_state.cache_stats_recorded) return;
    long cr = json_get_long(usage, "cache_read_input_tokens", 0);
    if (cr == 0) cr = json_get_long(usage, "prompt_cache_hit_tokens", 0);
    if (cr == 0) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
        if (cJSON_IsObject(d)) {
            cr = json_get_long(d, "cached_tokens", 0);
        }
    }
    long cc = json_get_long(usage, "cache_creation_input_tokens", 0);
    job->stream_state.cache_read_input_tokens = cr;
    job->stream_state.cache_creation_input_tokens = cc;
    const char *m = job->upstream_model ? job->upstream_model : job->client_model;
    const char *p = job->provider_name ? job->provider_name : "unknown";
    if (cr > 0) stats_record_cache_read(m, p, (unsigned long)cr);
    if (cc > 0) stats_record_cache_creation(m, p, (unsigned long)cc);
    if (cr > 0 || cc > 0) {
        job->stream_state.cache_stats_recorded = true;
        log_msg("DEBUG", "ANTH_CACHE model=%s cache_read=%ld cache_create=%ld", job->client_model, cr, cc);
    }
}

static void extract_anthropic_usage(gateway_job_t *job, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        /* 先尝试 Anthropic 格式 input_tokens/output_tokens */
        long in_tok = json_get_long(usage, "input_tokens", -1);
        long out_tok = json_get_long(usage, "output_tokens", -1);
        /* 回退到 OpenAI 格式 prompt_tokens/completion_tokens */
        if (in_tok < 0) in_tok = json_get_long(usage, "prompt_tokens", -1);
        if (out_tok < 0) out_tok = json_get_long(usage, "completion_tokens", -1);

        if (in_tok >= 0) {
            job->stream_state.prompt_tokens = in_tok;
            log_msg("DEBUG", "ANTH_USAGE model=%s input_tokens=%ld", job->client_model, in_tok);
        }
        if (out_tok >= 0) {
            job->stream_state.completion_tokens = out_tok;
            log_msg("DEBUG", "ANTH_USAGE model=%s output_tokens=%ld", job->client_model, out_tok);
        }

        /* 缓存：兼容 Anthropic / DeepSeek / OpenAI / Moonshot 格式 */
        long cr = json_get_long(usage, "cache_read_input_tokens", 0);
        if (cr == 0) cr = json_get_long(usage, "prompt_cache_hit_tokens", 0);
        if (cr == 0) {
            cJSON *d = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
            if (cJSON_IsObject(d)) cr = json_get_long(d, "cached_tokens", 0);
        }
        long cc = json_get_long(usage, "cache_creation_input_tokens", 0);
        log_msg("DEBUG", "STREAM_CACHE_FIX model=%s includes=%s pt_before=%ld cr=%ld cc=%ld",
                job->client_model, job->prompt_tokens_includes_cache ? "true" : "false",
                job->stream_state.prompt_tokens, cr, cc);
        if (!job->prompt_tokens_includes_cache && job->stream_state.prompt_tokens > 0) {
            job->stream_state.prompt_tokens = job->stream_state.prompt_tokens + cr + cc;
            log_msg("DEBUG", "STREAM_CACHE_FIX model=%s pt_after=%ld",
                    job->client_model, job->stream_state.prompt_tokens);
        }
        if (cr > 0 || cc > 0) {
            job->stream_state.cache_read_input_tokens = cr;
            job->stream_state.cache_creation_input_tokens = cc;
            const char *m = job->upstream_model ? job->upstream_model : job->client_model;
            const char *p = job->provider_name ? job->provider_name : "unknown";
            if (cr > 0) stats_record_cache_read(m, p, (unsigned long)cr);
            if (cc > 0) stats_record_cache_creation(m, p, (unsigned long)cc);
        }
    }
    /* message_start 事件的 usage 嵌套在 message 对象内
     * 注意：Anthropic 协议中 input_tokens 不包含 cache_read_input_tokens，
     * 因此需在此处也应用 prompt_tokens_includes_cache 修正 */
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsObject(msg)) {
        cJSON *msg_usage = cJSON_GetObjectItemCaseSensitive(msg, "usage");
        if (cJSON_IsObject(msg_usage)) {
            long in_tok = json_get_long(msg_usage, "input_tokens", -1);
            long out_tok = json_get_long(msg_usage, "output_tokens", -1);
            if (in_tok < 0) in_tok = json_get_long(msg_usage, "prompt_tokens", -1);
            if (out_tok < 0) out_tok = json_get_long(msg_usage, "completion_tokens", -1);
            /* 先提取缓存统计，再根据 includes_cache 口径修正 prompt_tokens */
            extract_anthropic_cache_stats(job, msg_usage);
            log_msg("DEBUG", "STREAM_CACHE_FIX model=%s includes=%s pt_before=%ld cr=%ld cc=%ld",
                    job->client_model, job->prompt_tokens_includes_cache ? "true" : "false",
                    in_tok, job->stream_state.cache_read_input_tokens, job->stream_state.cache_creation_input_tokens);
            if (!job->prompt_tokens_includes_cache && in_tok > 0) {
                in_tok = in_tok + job->stream_state.cache_read_input_tokens + job->stream_state.cache_creation_input_tokens;
                log_msg("DEBUG", "STREAM_CACHE_FIX model=%s pt_after=%ld", job->client_model, in_tok);
            }
            if (in_tok >= 0) {
                job->stream_state.prompt_tokens = in_tok;
                log_msg("DEBUG", "ANTH_USAGE model=%s msg.input_tokens=%ld", job->client_model, in_tok);
            }
            if (out_tok >= 0) {
                job->stream_state.completion_tokens = out_tok;
                log_msg("DEBUG", "ANTH_USAGE model=%s msg.output_tokens=%ld", job->client_model, out_tok);
            }
        }
    }
    cJSON_Delete(root);
}

/* 处理单行 SSE 数据
 * 
 * SSE（Server-Sent Events）协议解析：
 * 输入行格式：data: <json>\n 或 data: [DONE]\n
 * 
 * 处理步骤：
 * 1. 跳过前导空白（空格、制表符、回车）
 * 2. 检查是否以 "data:" 开头，否则忽略
 * 3. 提取 data 字段内容（跳过 "data:" 后的空格）
 * 4. 去除尾部 \r 和 \n
 * 5. 如果是 "[DONE]"，调用 stream_finish 结束流
 * 6. 否则将 JSON 数据传给 handle_openai_stream_json 处理
 * 
 * 注意：此函数会修改输入字符串（去除尾部换行符）
 */
void process_sse_line(gateway_job_t *job, char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    if (strncmp(line, "data:", 5) != 0) return;
    char *data = line + 5;
    while (*data == ' ') data++;
    size_t n = strlen(data);
    while (n > 0 && (data[n-1] == '\r' || data[n-1] == '\n')) data[--n] = 0;
    if (strcmp(data, "[DONE]") == 0) {
        stream_finish(job);
        return;
    }
    switch (job->passthrough) {
        case PT_ANTHROPIC:
            extract_anthropic_usage(job, data);
            break;
        case PT_OPENAI:
            extract_openai_usage_only(job, data);
            break;
        case PT_NONE:
        default:
            handle_openai_stream_json(job, data);
            break;
    }
}

/* SSE 透传模式：对单行 SSE 数据按需覆盖 / 注入 model 字段。
 *
 * 返回值：
 *   - 非 NULL：新分配的 "data: {modified_json}\n" 行（由调用方 free）
 *   - NULL：当前行无需修改（调用方原样转发 line）
 *
 * 适用：
 *   - PT_ANTHROPIC：仅当 data 是 message_start 事件时，覆盖 data.message.model
 *   - PT_OPENAI：每个非 [DONE] data chunk 覆盖顶层 .model
 *
 * 非 data 行（event: / id: / retry: / 空行）原样透传，返回 NULL。 */
static char *passthrough_anthropic_sse_override(const char *line, const char *gateway_model) {
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    if (strncmp(line, "data:", 5) != 0) return NULL;
    const char *data = line + 5;
    while (*data == ' ') data++;
    if (*data == 0) return NULL;

    cJSON *root = cJSON_Parse(data);
    if (!root) return NULL;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsString(type) || !cJSON_IsObject(msg) ||
        strcmp(type->valuestring, "message_start") != 0) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *model = cJSON_GetObjectItemCaseSensitive(msg, "model");
    if (cJSON_IsString(model) && model->valuestring && model->valuestring[0]) {
        cJSON_ReplaceItemInObjectCaseSensitive(msg, "model", cJSON_CreateString(gateway_model));
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(msg, "model");
        cJSON_AddStringToObject(msg, "model", gateway_model);
    }

    char *new_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!new_data) return NULL;

    size_t need = strlen("data: ") + strlen(new_data) + strlen("\n") + 1;
    char *out = (char *)malloc(need);
    if (!out) { free(new_data); return NULL; }
    snprintf(out, need, "data: %s\n", new_data);
    free(new_data);
    return out;
}

static char *passthrough_openai_sse_override(const char *line, const char *gateway_model) {
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    if (strncmp(line, "data:", 5) != 0) return NULL;
    const char *data = line + 5;
    while (*data == ' ') data++;
    if (*data == 0) return NULL;
    /* [DONE] 哨兵：原样透传 */
    if (strcmp(data, "[DONE]") == 0) return NULL;

    cJSON *root = cJSON_Parse(data);
    if (!root) return NULL;

    cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsString(model) && model->valuestring && model->valuestring[0]) {
        cJSON_ReplaceItemInObjectCaseSensitive(root, "model", cJSON_CreateString(gateway_model));
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "model");
        cJSON_AddStringToObject(root, "model", gateway_model);
    }

    char *new_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!new_data) return NULL;

    size_t need = strlen("data: ") + strlen(new_data) + strlen("\n") + 1;
    char *out = (char *)malloc(need);
    if (!out) { free(new_data); return NULL; }
    snprintf(out, need, "data: %s\n", new_data);
    free(new_data);
    return out;
}

/* 追加数据到 SSE 解析缓冲区并处理
 *
 * 功能：将接收到的原始数据追加到行缓冲区，按行分割处理
 *
 * 缓冲区管理：
 * - 动态扩容 linebuf（初始 8192 字节，按需 2 倍扩容）
 * - 确保容量足以容纳新数据 + 结尾 \0
 *
 * 行分割处理：
 * 1. 将新数据追加到 linebuf 末尾
 * 2. 查找换行符 \n
 * 3. 对每个完整行：
 *    - 透传模式：按需覆盖 / 注入 model 字段后转发
 *    - 转换模式（PT_NONE）：交由 handle_openai_stream_json 等 emit 函数
 *    - 替换 \n 为 \0（C 字符串结尾）
 *    - 调用 process_sse_line 处理
 *    - 如果流已结束，提前返回
 * 4. 将剩余的不完整行移到缓冲区开头
 * 5. 更新 linebuf_len
 *
 * 这种设计可以处理跨多个数据包的不完整行
 */
void stream_parse_append(gateway_job_t *job, const char *ptr, size_t n) {
    stream_state_t *s = &job->stream_state;
    if (s->ended) return;
    if (s->linebuf_len + n + 1 > s->linebuf_cap) {
        size_t nc = s->linebuf_cap ? s->linebuf_cap * 2 : 8192;
        while (nc < s->linebuf_len + n + 1) nc *= 2;
        char *p = (char *)realloc(s->linebuf, nc);
        if (!p) return;
        s->linebuf = p;
        s->linebuf_cap = nc;
    }
    memcpy(s->linebuf + s->linebuf_len, ptr, n);
    s->linebuf_len += n;
    s->linebuf[s->linebuf_len] = 0;

    char *start = s->linebuf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = 0;
        /* 透传模式：按需覆盖 / 注入 model 字段后按行转发 */
        if (job->passthrough != PT_NONE) {
            char *out_line = NULL;
            if (job->passthrough == PT_ANTHROPIC) {
                out_line = passthrough_anthropic_sse_override(start, job->client_model);
            } else if (job->passthrough == PT_OPENAI) {
                out_line = passthrough_openai_sse_override(start, job->client_model);
            }
            if (out_line) {
                stream_send_chunk(job, out_line);
                free(out_line);
            } else {
                /* 透传 + 非目标行（event: / id: / retry: / 空行）：原样转发 */
                stream_send_chunk(job, start);
            }
        }
        process_sse_line(job, start);
        if (s->ended) return;
        start = nl + 1;
    }
    size_t rem = strlen(start);
    memmove(s->linebuf, start, rem + 1);
    s->linebuf_len = rem;
}

/* libcurl 写回调函数
 * 
 * libcurl 在接收到上游 HTTP 响应体数据时调用此函数
 * 
 * 参数（libcurl 约定）：
 *   ptr - 接收到的数据指针
 *   size - 每个数据单元的大小（通常为 1）
 *   nmemb - 数据单元数量
 *   userdata - 用户数据（gateway_job_t 指针）
 * 
 * 返回：处理的数据字节数（必须等于 size*nmemb，否则 libcurl 视为错误）
 * 
 * 处理逻辑：
 * 1. 将数据追加到 upstream_body（完整记录上游响应，用于调试日志）
 * 2. 如果是流式响应：
 *    - 启动 SSE 响应（发送 HTTP 头）
 *    - 将数据追加到 SSE 解析缓冲区（stream_parse_append）
 * 3. 记录 DEBUG 日志
 */
size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nmemb;
    if (job->job_state != JOB_RECEIVING) job->job_state = JOB_RECEIVING;
    /* 始终累积上游响应体，用于日志记录 */
    membuf_append(&job->upstream_body, ptr, n);
    if (job->stream) {
        stream_start_reply(job);
        stream_parse_append(job, ptr, n);
    }
    log_msg("DEBUG", "UP_RESP chunk model=%s len=%zu", job->client_model, n);
    return n;
}

/* libcurl 头回调函数
 * 
 * libcurl 在接收到 HTTP 响应头时调用此函数
 * 
 * 处理逻辑：
 * 1. 标记 upstream_headers_done = true（表示已开始接收响应体）
 * 2. 去除尾部 \r\n，记录响应头到 DEBUG 日志
 * 
 * 注意：此函数会被每个响应头调用一次，包括状态行和所有头字段
 */
size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    gateway_job_t *job = (gateway_job_t *)userdata;
    size_t n = size * nitems;
    job->upstream_headers_done = true;
    job->job_state = JOB_WAITING;
    size_t hn = n;
    while (hn > 0 && (buffer[hn-1] == '\r' || buffer[hn-1] == '\n')) hn--;
    if (hn > 0 && hn < 512)
        log_msg("DEBUG", "UP_HDR model=%s %.*s", job->client_model, (int)hn, buffer);
    return n;
}

/* 延迟发送回调（libevent 定时器事件）
 * 
 * 核心发送函数：在 libevent 主事件循环中异步执行 HTTP 发送
 * 
 * 为什么需要延迟发送：
 * - libcurl 回调在工作线程中执行
 * - evhttp 不是线程安全的，必须在主事件循环线程中操作
 * - 通过 event_base_once 将发送操作调度到主线程
 * 
 * 操作顺序：
 * 1. 加锁保护 send_mu，提取并清空所有待发送状态
 *    - send_buf：SSE 数据缓冲区
 *    - send_start：是否发送响应头
 *    - send_end：是否结束响应
 *    - nonstream_json：非流式 JSON 响应
 *    - nonstream_code：非流式响应状态码
 * 2. 如果 send_start：发送 HTTP 200 和 SSE 响应头
 * 3. 如果 send_buf 有数据：发送数据块（evhttp_send_reply_chunk）
 * 4. 如果 send_end：结束 HTTP 响应（evhttp_send_reply_end）
 * 5. 如果 nonstream_json：发送完整 JSON 响应（非流式模式）
 * 
 * 注意：fd 和 what 参数是 libevent 回调签名要求，实际不使用
 */
void deferred_send(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what;
    gateway_job_t *job = arg;

    if (!job || !job->client_req) return;
    pthread_mutex_lock(&job->send_mu);
    struct evbuffer *buf = job->send_buf;
    job->send_buf = NULL;
    bool do_start = job->send_start;
    job->send_start = false;
    bool do_end = job->send_end;
    job->send_end = false;
    char *json = job->nonstream_json;
    job->nonstream_json = NULL;
    int code = job->nonstream_code;
    pthread_mutex_unlock(&job->send_mu);
    if (do_start) {
        struct evkeyvalq *h = evhttp_request_get_output_headers(job->client_req);
        evhttp_add_header(h, "Content-Type", "text/event-stream; charset=utf-8");
        evhttp_add_header(h, "Cache-Control", "no-cache, no-transform");
        evhttp_add_header(h, "Connection", "keep-alive");
        evhttp_add_header(h, "X-Accel-Buffering", "no");
        evhttp_send_reply_start(job->client_req, 200, "OK");
    }
    if (buf && evbuffer_get_length(buf) > 0) {
        size_t bl = evbuffer_get_length(buf);
        char *preview = (char *)calloc(1, bl + 1);
        if (preview) {
            evbuffer_copyout(buf, preview, bl);
            size_t pl = bl > 4096 ? 4096 : bl;
            log_msg("DEBUG", "SSE_SEND model=%s len=%zu %.*s", job->client_model, bl, (int)pl, preview);
            free(preview);
        }
        evhttp_send_reply_chunk(job->client_req, buf);
    }
    if (buf) evbuffer_free(buf);
    if (do_end) {
        log_msg("DEBUG", "SSE_END model=%s", job->client_model);
        evhttp_send_reply_end(job->client_req);
        job->response_sent = true;
    }
    if (json) {
        size_t jl = strlen(json);
        size_t pl = jl > 4096 ? 4096 : jl;
        log_msg("DEBUG", "RESP_SEND model=%s code=%d %.*s", job->client_model, code, (int)pl, json);
        struct evbuffer *out = evbuffer_new();
        evbuffer_add(out, json, strlen(json));
        evhttp_add_header(evhttp_request_get_output_headers(job->client_req), "Content-Type", "application/json; charset=utf-8");
        evhttp_send_reply(job->client_req, code, code == 200 ? "OK" : "Error", out);
        evbuffer_free(out);
        free(json);
        job->response_sent = true;
    }
}

