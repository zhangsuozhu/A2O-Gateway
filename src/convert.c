#include "convert.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <cjson/cJSON.h>

/**
 * @brief 安全的字符串复制函数
 * @param s 源字符串指针，允许为 NULL
 * @return 返回新分配的字符串副本，如果输入为 NULL 则返回 NULL
 * @note 内部使用 strdup 实现，分配失败时会打印错误并调用 abort() 终止程序
 *         用于替代标准 strdup，提供空指针安全检查
 */
char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { perror("strdup"); abort(); }
    return p;
}

/**
 * @brief 生成带前缀的唯一标识符
 * @param prefix ID 前缀字符串（如 "msg"、"toolu"）
 * @return 返回新分配的字符串，格式为 "prefix_秒_纳秒_随机数"
 * @note 使用 CLOCK_REALTIME 获取纳秒级时间戳，结合 16 位随机数确保 ID 唯一性
 *         生成的 ID 用于消息、工具调用等实体的标识
 */
char *make_id(const char *prefix) {
    char buf[128];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%s_%ld%09ld_%04x", prefix, (long)ts.tv_sec, ts.tv_nsec, rand() & 0xffff);
    return xstrdup(buf);
}

/**
 * @brief 将字符串解析为 JSON 对象，失败时返回空对象
 * @param s JSON 字符串，允许为 NULL 或空字符串
 * @return 解析成功返回对应的 cJSON 对象，失败或输入无效时返回新创建的空对象
 * @note 安全包装函数，确保始终返回有效的 JSON 对象而非 NULL
 *         常用于解析上游 API 返回的可能为空的参数字符串
 */
cJSON *json_from_string_or_empty_object(const char *s) {
    if (s && *s) {
        cJSON *j = cJSON_Parse(s);
        if (j) return j;
    }
    return cJSON_CreateObject();
}

/**
 * @brief 从 JSON 内容中提取纯文本字符串
 * @param content Anthropic 格式的内容，可以是字符串或内容块数组
 * @return 返回 cJSON 字符串对象。如果是数组，会提取所有 type="text" 块的文本并用换行连接
 * @note 用于将 Anthropic 的复杂内容格式（支持多模态块）简化为纯文本字符串
 *         当内容仅为文本时，此函数可以提取出所有文本并用换行符拼接
 */
cJSON *string_or_content_text(cJSON *content) {
    if (cJSON_IsString(content)) return cJSON_CreateString(content->valuestring ? content->valuestring : "");
    membuf_t b; membuf_init(&b);
    if (cJSON_IsArray(content)) {
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (type && strcmp(type, "text") == 0) {
                const char *t = json_get_str(blk, "text");
                if (t) {
                    if (b.len) membuf_append(&b, "\n", 1);
                    membuf_append(&b, t, strlen(t));
                }
            }
        }
    }
    cJSON *s = cJSON_CreateString(b.ptr ? b.ptr : "");
    membuf_free(&b);
    return s;
}

/**
 * @brief 将 Anthropic 内容格式转换为 OpenAI 内容格式
 * @param content Anthropic 格式的内容（字符串或内容块数组）
 * @param has_non_text 输出参数，标记是否包含非文本内容（如图片）
 * @return 返回 OpenAI 格式的内容：纯文本字符串或内容块数组
 * @note 处理 text、image 两种类型：
 *         - text 块直接映射为 OpenAI 的 text 类型
 *         - image 块支持两种来源：
 *           1. url 类型：提取 URL 直接映射为 image_url
 *           2. base64 类型：拼接 media_type 和 data 为 data URL 格式
 *         如果内容中只有纯文本（无图片），返回简化字符串而非数组
 */
cJSON *anthropic_content_to_openai_content(cJSON *content, bool *has_non_text) {
    *has_non_text = false;
    if (cJSON_IsString(content)) return cJSON_CreateString(content->valuestring ? content->valuestring : "");
    if (!cJSON_IsArray(content)) return cJSON_CreateString("");

    cJSON *arr = cJSON_CreateArray();
    cJSON *blk;
    cJSON_ArrayForEach(blk, content) {
        const char *type = json_get_str(blk, "type");
        if (!type) continue;
        if (strcmp(type, "text") == 0) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", "text");
            cJSON_AddStringToObject(o, "text", json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
            cJSON_AddItemToArray(arr, o);
        } else if (strcmp(type, "image") == 0) {
            *has_non_text = true;
            cJSON *src = cJSON_GetObjectItemCaseSensitive(blk, "source");
            const char *stype = src ? json_get_str(src, "type") : NULL;
            const char *url = NULL;
            char *data_url = NULL;
            if (stype && strcmp(stype, "url") == 0) {
                url = json_get_str(src, "url");
            } else if (stype && strcmp(stype, "base64") == 0) {
                const char *media = json_get_str(src, "media_type");
                const char *data = json_get_str(src, "data");
                if (media && data) {
                    size_t need = strlen(media) + strlen(data) + 32;
                    data_url = (char *)calloc(1, need);
                    snprintf(data_url, need, "data:%s;base64,%s", media, data);
                    url = data_url;
                }
            }
            if (url) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", "image_url");
                cJSON *iu = cJSON_CreateObject();
                cJSON_AddStringToObject(iu, "url", url);
                cJSON_AddItemToObject(o, "image_url", iu);
                cJSON_AddItemToArray(arr, o);
            }
            free(data_url);
        }
    }
    if (!*has_non_text) {
        cJSON *text = string_or_content_text(content);
        cJSON_Delete(arr);
        return text;
    }
    return arr;
}

/**
 * @brief 将 Anthropic 工具定义转换为 OpenAI 工具定义
 * @param tools Anthropic 格式的工具数组
 * @return 返回 OpenAI 格式的 tools 数组，每个工具包装为 function 类型；空数组返回 NULL
 * @note 字段映射关系：
 *         - Anthropic name → OpenAI function.name
 *         - Anthropic description → OpenAI function.description
 *         - Anthropic input_schema → OpenAI function.parameters
 *         每个工具外层包装为 { "type": "function", "function": {...} }
 */
cJSON *anthropic_tools_to_openai(cJSON *tools) {
    if (!cJSON_IsArray(tools)) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const char *name = json_get_str(t, "name");
        if (!name) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", name);
        const char *desc = json_get_str(t, "description");
        if (desc) cJSON_AddStringToObject(fn, "description", desc);
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "input_schema");
        if (schema) cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
        else cJSON_AddItemToObject(fn, "parameters", cJSON_CreateObject());
        cJSON_AddItemToObject(item, "function", fn);
        cJSON_AddItemToArray(out, item);
    }
    if (cJSON_GetArraySize(out) == 0) { cJSON_Delete(out); return NULL; }
    return out;
}

/**
 * @brief 将 Anthropic 的 tool_choice 转换为 OpenAI 格式
 * @param tc Anthropic 的 tool_choice 对象（auto / any / tool 类型）
 * @return 返回 OpenAI 格式的 tool_choice：字符串或 function 对象；无效输入返回 NULL
 * @note 映射关系：
 *         - "auto" → "auto"
 *         - "any" → "required"
 *         - "tool" + name → { "type": "function", "function": { "name": "..." } }
 */
cJSON *anthropic_tool_choice_to_openai(cJSON *tc) {
    if (!tc) return NULL;
    if (cJSON_IsString(tc)) return cJSON_CreateString(tc->valuestring);
    if (!cJSON_IsObject(tc)) return NULL;
    const char *type = json_get_str(tc, "type");
    if (!type) return NULL;
    if (strcmp(type, "auto") == 0) return cJSON_CreateString("auto");
    if (strcmp(type, "any") == 0) return cJSON_CreateString("required");
    if (strcmp(type, "tool") == 0) {
        const char *name = json_get_str(tc, "name");
        if (!name) return NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "function");
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", name);
        cJSON_AddItemToObject(o, "function", f);
        return o;
    }
    return NULL;
}

/**
 * @brief 转换单条消息的内容块为 OpenAI 消息格式
 * @param openai_messages 输出数组，转换后的消息会追加到此数组
 * @param role 消息角色（"user" 或 "assistant"）
 * @param content Anthropic 格式的消息内容
 * @return 返回更新后的 openai_messages 数组
 * @note 核心消息转换函数，处理两种角色的不同逻辑：
 *         - user 角色：
 *           * 识别 tool_result 块，拆分为独立的 role="tool" 消息（含 tool_call_id）
 *           * 提取 text 和 image 块，合并为一条 user 消息
 *         - assistant 角色：
 *           * 转换文本内容
 *           * 识别 tool_use 块，生成 tool_calls 数组（含 id、name、arguments）
 */
cJSON *convert_message_content_blocks(cJSON *openai_messages, const char *role, cJSON *content) {
    if (strcmp(role, "user") == 0 && cJSON_IsArray(content)) {
        cJSON *text_blocks = cJSON_CreateArray();
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (!type) continue;
            if (strcmp(type, "tool_result") == 0) {
                const char *tid = json_get_str(blk, "tool_use_id");
                cJSON *toolmsg = cJSON_CreateObject();
                cJSON_AddStringToObject(toolmsg, "role", "tool");
                if (tid) cJSON_AddStringToObject(toolmsg, "tool_call_id", tid);
                cJSON *trc = cJSON_GetObjectItemCaseSensitive(blk, "content");
                cJSON_AddItemToObject(toolmsg, "content", string_or_content_text(trc));
                cJSON_AddItemToArray(openai_messages, toolmsg);
            } else if (strcmp(type, "text") == 0 || strcmp(type, "image") == 0) {
                cJSON_AddItemToArray(text_blocks, cJSON_Duplicate(blk, 1));
            }
        }
        if (cJSON_GetArraySize(text_blocks) > 0) {
            bool has_non_text = false;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddItemToObject(msg, "content", anthropic_content_to_openai_content(text_blocks, &has_non_text));
            cJSON_AddItemToArray(openai_messages, msg);
        }
        cJSON_Delete(text_blocks);
        return openai_messages;
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", strcmp(role, "assistant") == 0 ? "assistant" : "user");
    bool has_non_text = false;
    cJSON_AddItemToObject(msg, "content", anthropic_content_to_openai_content(content, &has_non_text));

    if (strcmp(role, "assistant") == 0 && cJSON_IsArray(content)) {
        cJSON *calls = cJSON_CreateArray();
        cJSON *blk;
        int idx = 0;
        cJSON_ArrayForEach(blk, content) {
            const char *type = json_get_str(blk, "type");
            if (type && strcmp(type, "tool_use") == 0) {
                const char *id = json_get_str(blk, "id");
                const char *name = json_get_str(blk, "name");
                if (!name) continue;
                cJSON *call = cJSON_CreateObject();
                if (id) cJSON_AddStringToObject(call, "id", id);
                cJSON_AddStringToObject(call, "type", "function");
                cJSON_AddNumberToObject(call, "index", idx++);
                cJSON *fn = cJSON_CreateObject();
                cJSON_AddStringToObject(fn, "name", name);
                cJSON *input = cJSON_GetObjectItemCaseSensitive(blk, "input");
                char *arg = input ? cJSON_PrintUnformatted(input) : xstrdup("{}");
                cJSON_AddStringToObject(fn, "arguments", arg ? arg : "{}");
                free(arg);
                cJSON_AddItemToObject(call, "function", fn);
                cJSON_AddItemToArray(calls, call);
            }
        }
        if (cJSON_GetArraySize(calls) > 0) cJSON_AddItemToObject(msg, "tool_calls", calls);
        else cJSON_Delete(calls);
    }
    cJSON_AddItemToArray(openai_messages, msg);
    return openai_messages;
}

/**
 * @brief 将完整的 Anthropic 请求转换为 OpenAI 请求
 * @param anth_req 解析后的 Anthropic Messages API 请求体
 * @param model_cfg 模型配置对象（含 upstream_model、params、extra_body 等）
 * @return 返回新创建的 OpenAI Chat Completions 请求体
 * @note 完整的请求转换流程：
 *         1. 设置模型 ID（使用 upstream_model 或默认值）
 *         2. 转换 system 消息为 OpenAI 的 role="system" 消息
 *         3. 逐条转换 messages 数组中的消息
 *         4. 合并模型配置中的 params 参数
 *         5. 提取并设置 max_tokens、temperature、top_p、stop_sequences 等参数
 *         6. 设置 stream 标志，流式请求时添加 stream_options.include_usage
 *         7. 转换 tools 和 tool_choice
 *         8. 合并 extra_body 中的额外参数（覆盖之前的设置）
 */
cJSON *build_openai_request(cJSON *anth_req, cJSON *model_cfg) {
    const char *upstream_model = json_get_str(model_cfg, "upstream_model");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "model", upstream_model ? upstream_model : "model");

    cJSON *messages = cJSON_CreateArray();
    cJSON *system = cJSON_GetObjectItemCaseSensitive(anth_req, "system");
    if (system) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddItemToObject(sys, "content", string_or_content_text(system));
        cJSON_AddItemToArray(messages, sys);
    }

    cJSON *anth_messages = cJSON_GetObjectItemCaseSensitive(anth_req, "messages");
    if (cJSON_IsArray(anth_messages)) {
        cJSON *m;
        cJSON_ArrayForEach(m, anth_messages) {
            const char *role = json_get_str(m, "role");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (!role || !content) continue;
            convert_message_content_blocks(messages, role, content);
        }
    }
    cJSON_AddItemToObject(out, "messages", messages);

    cJSON *params = cJSON_GetObjectItemCaseSensitive(model_cfg, "params");
    if (cJSON_IsObject(params)) {
        cJSON *p;
        cJSON_ArrayForEach(p, params) {
            if (!p->string) continue;
            cJSON *existing = cJSON_GetObjectItemCaseSensitive(out, p->string);
            if (existing) {
                cJSON_ReplaceItemInObjectCaseSensitive(out, p->string, cJSON_Duplicate(p, 1));
            } else {
                cJSON_AddItemToObject(out, p->string, cJSON_Duplicate(p, 1));
            }
        }
    }

    cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(anth_req, "max_tokens");
    if (cJSON_IsNumber(max_tokens)) cJSON_AddNumberToObject(out, "max_tokens", max_tokens->valuedouble);
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(anth_req, "temperature");
    if (cJSON_IsNumber(temperature)) { cJSON_DeleteItemFromObjectCaseSensitive(out, "temperature"); cJSON_AddNumberToObject(out, "temperature", temperature->valuedouble); }
    cJSON *top_p = cJSON_GetObjectItemCaseSensitive(anth_req, "top_p");
    if (cJSON_IsNumber(top_p)) { cJSON_DeleteItemFromObjectCaseSensitive(out, "top_p"); cJSON_AddNumberToObject(out, "top_p", top_p->valuedouble); }
    cJSON *stop = cJSON_GetObjectItemCaseSensitive(anth_req, "stop_sequences");
    if (stop) cJSON_AddItemToObject(out, "stop", cJSON_Duplicate(stop, 1));
    cJSON *stream = cJSON_GetObjectItemCaseSensitive(anth_req, "stream");
    bool is_stream = cJSON_IsTrue(stream);
    cJSON_AddBoolToObject(out, "stream", is_stream);
    if (is_stream) {
        cJSON *so = cJSON_CreateObject();
        cJSON_AddBoolToObject(so, "include_usage", true);
        cJSON_AddItemToObject(out, "stream_options", so);
    }

    cJSON *tools = anthropic_tools_to_openai(cJSON_GetObjectItemCaseSensitive(anth_req, "tools"));
    if (tools) cJSON_AddItemToObject(out, "tools", tools);
    cJSON *tc = anthropic_tool_choice_to_openai(cJSON_GetObjectItemCaseSensitive(anth_req, "tool_choice"));
    if (tc) cJSON_AddItemToObject(out, "tool_choice", tc);

    cJSON *extra = cJSON_GetObjectItemCaseSensitive(model_cfg, "extra_body");
    if (cJSON_IsObject(extra)) {
        cJSON *p;
        cJSON_ArrayForEach(p, extra) {
            if (!p->string) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(out, p->string);
            cJSON_AddItemToObject(out, p->string, cJSON_Duplicate(p, 1));
        }
    }
    return out;
}

/**
 * @brief 构建上游 API 的完整请求 URL
 * @param model_cfg 模型配置对象，应包含 endpoint 或 base_url 字段
 * @return 返回新分配的 URL 字符串，以 /chat/completions 结尾
 * @note URL 构建逻辑：
 *         1. 如果 model_cfg 中有 endpoint 字段，直接复制返回
 *         2. 否则拼接 base_url + /chat/completions
 *         3. 自动去除 base_url 末尾的斜杠，避免双斜杠
 *         4. 默认使用空字符串作为 base，确保即使无配置也能生成有效路径
 */
char *make_upstream_url(cJSON *model_cfg) {
    const char *endpoint = json_get_str(model_cfg, "endpoint");
    if (endpoint && *endpoint) return xstrdup(endpoint);
    const char *base = json_get_str(model_cfg, "base_url");
    const char *raw = base ? base : "";
    size_t n = strlen(raw);
    while (n > 0 && raw[n - 1] == '/') n--;
    char *b = (char *)calloc(1, n + 1);
    if (!b) abort();
    memcpy(b, raw, n);
    b[n] = 0;
    size_t m = n + strlen("/chat/completions") + 2;
    char *out = (char *)calloc(1, m);
    snprintf(out, m, "%s/chat/completions", b);
    free(b);
    return out;
}

/**
 * @brief 将 OpenAI 的 finish_reason 映射为 Anthropic 的 stop_reason
 * @param fr OpenAI 的 finish_reason 字符串
 * @return 返回对应的 Anthropic stop_reason 常量字符串
 * @note 映射关系表：
 *         - "stop" → "end_turn"（正常结束）
 *         - "length" → "max_tokens"（达到最大 token 限制）
 *         - "tool_calls" → "tool_use"（因工具调用而停止）
 *         - "function_call" → "tool_use"（兼容旧版 function_call）
 *         - NULL 或其他 → "end_turn"（默认）
 */
const char *map_finish_reason(const char *fr) {
    if (!fr) return "end_turn";
    if (strcmp(fr, "stop") == 0) return "end_turn";
    if (strcmp(fr, "length") == 0) return "max_tokens";
    if (strcmp(fr, "tool_calls") == 0) return "tool_use";
    if (strcmp(fr, "function_call") == 0) return "tool_use";
    return "end_turn";
}

/**
 * @brief 将 OpenAI 的消息内容转换为 Anthropic 内容块数组
 * @param msg OpenAI 格式的消息对象（包含 content 和可选的 tool_calls）
 * @return 返回 Anthropic 格式的 content 数组（text 和 tool_use 块）
 * @note 转换逻辑：
 *         1. 提取 content 字段，如果为非空字符串，生成一个 text 块
 *         2. 遍历 tool_calls 数组，每个有效调用生成一个 tool_use 块：
 *            - id → id（无则生成 toolu_ 前缀的 ID）
 *            - function.name → name
 *            - function.arguments 字符串解析为 JSON 对象 → input
 *         3. 如果数组为空，确保至少返回一个空的 text 块（符合 Anthropic API 要求）
 */
cJSON *openai_message_to_anthropic_content(cJSON *msg) {
    cJSON *content = cJSON_CreateArray();
    cJSON *txt = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0]) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", txt->valuestring);
        cJSON_AddItemToArray(content, b);
    }
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    if (cJSON_IsArray(calls)) {
        cJSON *tc;
        cJSON_ArrayForEach(tc, calls) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
            const char *id = json_get_str(tc, "id");
            const char *name = fn ? json_get_str(fn, "name") : NULL;
            if (!name) continue;
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_use");
            cJSON_AddStringToObject(b, "id", id ? id : make_id("toolu"));
            cJSON_AddStringToObject(b, "name", name);
            cJSON *args_obj = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            if (cJSON_IsString(args_obj)) {
                cJSON_AddItemToObject(b, "input", json_from_string_or_empty_object(args_obj->valuestring));
            } else if (cJSON_IsObject(args_obj)) {
                cJSON_AddItemToObject(b, "input", cJSON_Duplicate(args_obj, 1));
            } else {
                cJSON_AddItemToObject(b, "input", cJSON_CreateObject());
            }
            cJSON_AddItemToArray(content, b);
        }
    }
    if (cJSON_GetArraySize(content) == 0) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", "");
        cJSON_AddItemToArray(content, b);
    }
    return content;
}

/**
 * @brief 将完整的 OpenAI 响应转换为 Anthropic 响应格式
 * @param body OpenAI API 返回的原始 JSON 字符串
 * @param client_model 客户端请求的模型 ID（用于回显）
 * @return 返回 Anthropic Messages API 格式的响应对象
 * @note 完整的响应转换流程：
 *         1. 尝试解析 body 为 JSON，失败返回错误对象
 *         2. 提取 choices[0]、message、finish_reason 等字段
 *         3. 构建 Anthropic 响应骨架：id、type、role、model、content
 *         4. 转换 message 为 Anthropic content 数组
 *         5. 映射 finish_reason 为 stop_reason
 *         6. 提取 usage.prompt_tokens/completion_tokens 映射为 input_tokens/output_tokens
 *         7. 清理临时 JSON 对象，返回结果
 */
cJSON *convert_openai_response_to_anthropic(const char *body, const char *client_model) {
    cJSON *oai = cJSON_Parse(body ? body : "");
    if (!oai) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "invalid_upstream_response");
        cJSON_AddStringToObject(e, "message", "upstream did not return valid JSON");
        cJSON_AddItemToObject(err, "error", e);
        return err;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(oai, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    const char *fr = choice ? json_get_str(choice, "finish_reason") : NULL;
    cJSON *out = cJSON_CreateObject();
    char *id = make_id("msg");
    cJSON_AddStringToObject(out, "id", id);
    free(id);
    cJSON_AddStringToObject(out, "type", "message");
    cJSON_AddStringToObject(out, "role", "assistant");
    cJSON_AddStringToObject(out, "model", client_model ? client_model : "claude-code-gateway");
    cJSON_AddItemToObject(out, "content", msg ? openai_message_to_anthropic_content(msg) : cJSON_CreateArray());
    cJSON_AddStringToObject(out, "stop_reason", map_finish_reason(fr));
    cJSON_AddNullToObject(out, "stop_sequence");
    cJSON *usage = cJSON_CreateObject();
    cJSON *u = cJSON_GetObjectItemCaseSensitive(oai, "usage");
    cJSON_AddNumberToObject(usage, "input_tokens", json_get_long(u, "prompt_tokens", 0));
    cJSON_AddNumberToObject(usage, "output_tokens", json_get_long(u, "completion_tokens", 0));
    cJSON_AddItemToObject(out, "usage", usage);
    cJSON_Delete(oai);
    return out;
}
