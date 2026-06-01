#ifndef CONVERT_H
#define CONVERT_H

#include <stdbool.h>
#include "types.h"

/**
 * @brief 安全的字符串复制函数
 * @param s 源字符串指针，允许为 NULL
 * @return 返回新分配的字符串副本，如果输入为 NULL 则返回 NULL
 * @note 内部使用 strdup 实现，分配失败时会调用 abort() 终止程序
 */
char *xstrdup(const char *s);

/**
 * @brief 生成带前缀的唯一标识符
 * @param prefix ID 前缀字符串（如 "msg"、"toolu"）
 * @return 返回新分配的字符串，格式为 "prefix_秒纳秒_随机数"
 * @note 使用 CLOCK_REALTIME 获取纳秒级时间戳，确保 ID 唯一性
 */
char *make_id(const char *prefix);

/**
 * @brief 从 JSON 内容中提取纯文本字符串
 * @param content Anthropic 格式的内容，可以是字符串或内容块数组
 * @return 返回 cJSON 字符串对象。如果是数组，会提取所有 type="text" 块的文本并用换行连接
 * @note 用于将 Anthropic 的复杂内容格式简化为纯文本
 */
cJSON *string_or_content_text(cJSON *content);

/**
 * @brief 将字符串解析为 JSON 对象，失败时返回空对象
 * @param s JSON 字符串，允许为 NULL 或空字符串
 * @return 解析成功返回对应的 cJSON 对象，失败返回新创建的空对象
 * @note 安全包装函数，确保始终返回有效的 JSON 对象
 */
cJSON *json_from_string_or_empty_object(const char *s);

/**
 * @brief 将 Anthropic 内容格式转换为 OpenAI 内容格式
 * @param content Anthropic 格式的内容（字符串或内容块数组）
 * @param has_non_text 输出参数，标记是否包含非文本内容（如图片）
 * @return 返回 OpenAI 格式的内容：纯文本字符串或内容块数组
 * @note 处理 text、image 两种类型；图片支持 URL 和 base64 两种来源
 */
cJSON *anthropic_content_to_openai_content(cJSON *content, bool *has_non_text);

/**
 * @brief 将 Anthropic 工具定义转换为 OpenAI 工具定义
 * @param tools Anthropic 格式的工具数组
 * @return 返回 OpenAI 格式的 tools 数组，每个工具包装为 function 类型
 * @note Anthropic 的 input_schema 映射为 OpenAI 的 parameters
 */
cJSON *anthropic_tools_to_openai(cJSON *tools);

/**
 * @brief 将 Anthropic 的 tool_choice 转换为 OpenAI 格式
 * @param tc Anthropic 的 tool_choice 对象（auto / any / tool 类型）
 * @return 返回 OpenAI 格式的 tool_choice：字符串或 function 对象
 * @note 映射关系：auto→auto, any→required, tool→function 对象
 */
cJSON *anthropic_tool_choice_to_openai(cJSON *tc);

/**
 * @brief 转换单条消息的内容块为 OpenAI 消息格式
 * @param openai_messages 输出数组，转换后的消息会追加到此数组
 * @param role 消息角色（"user" 或 "assistant"）
 * @param content Anthropic 格式的消息内容
 * @return 返回更新后的 openai_messages 数组
 * @note 特殊处理 user 消息的 tool_result 块，将其拆分为独立的 tool 角色消息
 */
cJSON *convert_message_content_blocks(cJSON *messages, const char *role, cJSON *content, const char *reasoning_content);

/**
 * @brief 将完整的 Anthropic 请求转换为 OpenAI 请求
 * @param anth_req 解析后的 Anthropic Messages API 请求体
 * @param model_cfg 模型配置对象（含 upstream_model、params、extra_body 等）
 * @return 返回新创建的 OpenAI Chat Completions 请求体
 * @note 转换内容包括：system 消息、messages 数组、参数、工具、stream 设置等
 */
cJSON *build_openai_request(cJSON *anth_req, cJSON *model_cfg);

/**
 * @brief 从 Anthropic 请求的 system 字段中移除 CCH 行
 * @param anth_req 解析后的 Anthropic Messages API 请求体（会被原地修改）
 * @note 处理 system 为字符串或内容块数组两种情况，保护第三方 API 的 prompt 缓存机制
 */
void filter_cch_from_anthropic_request(cJSON *anth_req);

/**
 * @brief 构建上游 API 的完整请求 URL
 * @param model_cfg 模型配置对象
 * @return 返回新分配的 URL 字符串，以 /chat/completions 结尾
 * @note 优先使用 endpoint 字段，否则拼接 base_url + /chat/completions
 */
char *make_upstream_url(cJSON *model_cfg);

/**
 * @brief 构建透传模式下的上游 API URL（/v1/messages 而非 /chat/completions）
 * @param model_cfg 模型配置对象
 * @return 返回新分配的 URL 字符串，以 /v1/messages 结尾
 * @note 用于 provider=anthropic 的透传模式，直接转发 Anthropic 格式请求
 */
char *make_upstream_url_for_messages(cJSON *model_cfg);

/**
 * @brief 将 OpenAI 的 finish_reason 映射为 Anthropic 的 stop_reason
 * @param fr OpenAI 的 finish_reason 字符串
 * @return 返回对应的 Anthropic stop_reason 常量字符串
 * @note 映射关系：stop→end_turn, length→max_tokens, tool_calls/function_call→tool_use
 */
const char *map_finish_reason(const char *fr);

/**
 * @brief 将 OpenAI 的消息内容转换为 Anthropic 内容块数组
 * @param msg OpenAI 格式的消息对象
 * @return 返回 Anthropic 格式的 content 数组（text 和 tool_use 块）
 * @note 处理文本内容和 tool_calls，确保至少返回一个空的 text 块
 */
cJSON *openai_message_to_anthropic_content(cJSON *msg);

/**
 * @brief 将完整的 OpenAI 响应转换为 Anthropic 响应格式
 * @param body OpenAI API 返回的原始 JSON 字符串
 * @param client_model 客户端请求的模型 ID
 * @return 返回 Anthropic Messages API 格式的响应对象
 * @note 如果 body 解析失败，返回错误对象；否则提取 choices、usage 等字段
 */
cJSON *convert_openai_response_to_anthropic(const char *body, const char *client_model);

#endif