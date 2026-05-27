#include "convert.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <cjson/cJSON.h>

char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { perror("strdup"); abort(); }
    return p;
}

char *make_id(const char *prefix) {
    char buf[128];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%s_%ld%09ld_%04x", prefix, (long)ts.tv_sec, ts.tv_nsec, rand() & 0xffff);
    return xstrdup(buf);
}

cJSON *json_from_string_or_empty_object(const char *s) {
    if (s && *s) {
        cJSON *j = cJSON_Parse(s);
        if (j) return j;
    }
    return cJSON_CreateObject();
}

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
            cJSON_ReplaceItemInObjectCaseSensitive(out, p->string, cJSON_Duplicate(p, 1));
            if (!cJSON_GetObjectItemCaseSensitive(out, p->string)) {
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

const char *map_finish_reason(const char *fr) {
    if (!fr) return "end_turn";
    if (strcmp(fr, "stop") == 0) return "end_turn";
    if (strcmp(fr, "length") == 0) return "max_tokens";
    if (strcmp(fr, "tool_calls") == 0) return "tool_use";
    if (strcmp(fr, "function_call") == 0) return "tool_use";
    return "end_turn";
}

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