#include "convert.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

static int expect_key_count(const char *json, const char *quoted_key, int expected) {
    int actual = count_occurrences(json, quoted_key);
    if (actual != expected) {
        fprintf(stderr, "expected %s to appear %d time(s), got %d in: %s\n",
                quoted_key, expected, actual, json);
        return 1;
    }
    return 0;
}

static int expect_number(cJSON *obj, const char *key, double expected) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(value) || value->valuedouble != expected) {
        fprintf(stderr, "expected %s to be %.0f\n", key, expected);
        return 1;
    }
    return 0;
}

static int expect_bool(cJSON *obj, const char *key, int expected_true) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(value) || cJSON_IsTrue(value) != expected_true) {
        fprintf(stderr, "expected %s to be %s\n", key, expected_true ? "true" : "false");
        return 1;
    }
    return 0;
}

static int test_overlapping_request_fields_are_replaced(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"max_tokens\":1024,"
        "\"stream\":true,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"
        "}";
    const char *cfg_json =
        "{"
        "\"upstream_model\":\"upstream-model\","
        "\"params\":{\"max_tokens\":2048,\"stream\":false},"
        "\"extra_body\":{\"max_tokens\":4096,\"custom\":true}"
        "}";

    cJSON *anth = cJSON_Parse(anth_json);
    cJSON *cfg = cJSON_Parse(cfg_json);
    if (!anth || !cfg) {
        fprintf(stderr, "failed to parse test JSON\n");
        cJSON_Delete(anth);
        cJSON_Delete(cfg);
        return 1;
    }

    cJSON *out = build_openai_request(anth, cfg);
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!out || !printed) {
        fprintf(stderr, "failed to build request\n");
        failed = 1;
    } else {
        failed |= expect_key_count(printed, "\"max_tokens\"", 1);
        failed |= expect_key_count(printed, "\"stream\"", 1);
        failed |= expect_number(out, "max_tokens", 4096);
        failed |= expect_bool(out, "stream", 1);
    }

    free(printed);
    cJSON_Delete(out);
    cJSON_Delete(anth);
    cJSON_Delete(cfg);
    return failed;
}

static int test_openai_reasoning_alias_converts_to_thinking_block(void) {
    const char *oai_json =
        "{"
        "\"choices\":[{\"message\":{\"reasoning\":\"alias think\",\"content\":\"answer\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}"
        "}";
    cJSON *out = convert_openai_response_to_anthropic(oai_json, "model");
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!printed) {
        fprintf(stderr, "failed to print converted response\n");
        failed = 1;
    } else {
        if (!strstr(printed, "\"type\":\"thinking\"") || !strstr(printed, "\"thinking\":\"alias think\"")) {
            fprintf(stderr, "expected reasoning alias to convert to thinking block, got: %s\n", printed);
            failed = 1;
        }
    }

    free(printed);
    cJSON_Delete(out);
    return failed;
}

static int test_system_cch_is_stripped_in_openai_conversion(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"system\":\"anthropic-attribution: cch=abc123-def456\\n\\nYou are a helpful assistant.\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"
        "}";
    const char *cfg_json = "{\"upstream_model\":\"upstream-model\"}";

    cJSON *anth = cJSON_Parse(anth_json);
    cJSON *cfg = cJSON_Parse(cfg_json);
    cJSON *out = build_openai_request(anth, cfg);
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!printed) {
        failed = 1;
    } else {
        if (strstr(printed, "anthropic-attribution")) {
            fprintf(stderr, "CCH should be stripped from system prompt in OpenAI conversion, got: %s\n", printed);
            failed = 1;
        }
        if (!strstr(printed, "You are a helpful assistant")) {
            fprintf(stderr, "system content after CCH should be preserved in OpenAI conversion, got: %s\n", printed);
            failed = 1;
        }
    }

    free(printed);
    cJSON_Delete(out);
    cJSON_Delete(anth);
    cJSON_Delete(cfg);
    return failed;
}

static int test_system_cch_is_stripped_in_passthrough_string(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"system\":\"anthropic-attribution: cch=abc123-def456\\n\\nYou are a helpful assistant.\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"
        "}";

    cJSON *anth = cJSON_Parse(anth_json);
    filter_cch_from_anthropic_request(anth);
    cJSON *system = cJSON_GetObjectItemCaseSensitive(anth, "system");
    int failed = 0;

    if (!cJSON_IsString(system) || !system->valuestring) {
        fprintf(stderr, "system should remain a string after CCH stripping\n");
        failed = 1;
    } else if (strstr(system->valuestring, "anthropic-attribution")) {
        fprintf(stderr, "CCH should be stripped from passthrough string system, got: %s\n", system->valuestring);
        failed = 1;
    } else if (!strstr(system->valuestring, "You are a helpful assistant")) {
        fprintf(stderr, "system content after CCH should be preserved, got: %s\n", system->valuestring);
        failed = 1;
    }

    cJSON_Delete(anth);
    return failed;
}

static int test_system_cch_is_stripped_in_passthrough_array(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"system\":[{\"type\":\"text\",\"text\":\"anthropic-attribution: cch=abc123-def456\\n\\nYou are a helpful assistant.\"}],"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"
        "}";

    cJSON *anth = cJSON_Parse(anth_json);
    filter_cch_from_anthropic_request(anth);
    cJSON *system = cJSON_GetObjectItemCaseSensitive(anth, "system");
    int failed = 0;

    if (!cJSON_IsArray(system)) {
        fprintf(stderr, "system should remain an array after CCH stripping\n");
        failed = 1;
    } else {
        cJSON *blk = cJSON_GetArrayItem(system, 0);
        cJSON *text = cJSON_GetObjectItemCaseSensitive(blk, "text");
        if (!cJSON_IsString(text) || !text->valuestring) {
            fprintf(stderr, "text block should remain after CCH stripping\n");
            failed = 1;
        } else if (strstr(text->valuestring, "anthropic-attribution")) {
            fprintf(stderr, "CCH should be stripped from array system prompt, got: %s\n", text->valuestring);
            failed = 1;
        } else if (!strstr(text->valuestring, "You are a helpful assistant")) {
            fprintf(stderr, "system content after CCH should be preserved, got: %s\n", text->valuestring);
            failed = 1;
        }
    }

    cJSON_Delete(anth);
    return failed;
}

static int test_pt_none_nonstreaming_response_model_is_gateway_id(void) {
    const char *oai_json =
        "{"
        "\"id\":\"chatcmpl-X\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}"
        "}";
    cJSON *out = convert_openai_response_to_anthropic(oai_json, "qwen-coder");
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!out || !printed) {
        fprintf(stderr, "failed to build response\n");
        failed = 1;
    } else {
        cJSON *model = cJSON_GetObjectItemCaseSensitive(out, "model");
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder") != 0) {
            fprintf(stderr, "expected .model == \"qwen-coder\", got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(non-string or missing)");
            failed = 1;
        }
        if (cJSON_IsString(model) && strcmp(model->valuestring, "qwen3-coder-plus-2025-01") == 0) {
            fprintf(stderr, "model leaked upstream name: %s\n", model->valuestring);
            failed = 1;
        }
        if (strstr(printed, "qwen3-coder-plus-2025-01")) {
            fprintf(stderr, "upstream model name should not appear in response: %s\n", printed);
            failed = 1;
        }
    }

    free(printed);
    cJSON_Delete(out);
    return failed;
}

static int test_pt_anthropic_nonstreaming_model_overridden(void) {
    const char *anth_json =
        "{\"id\":\"msg_upstream_1\",\"type\":\"message\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
        "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}";
    char *out = passthrough_anthropic_override_model(anth_json, "qwen-coder-passthru");
    int failed = 0;
    if (!out) {
        fprintf(stderr, "passthrough_anthropic_override_model returned NULL\n");
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
            fprintf(stderr, "expected model=\"qwen-coder-passthru\", got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        /* other fields must be preserved (id, type, stop_reason, usage) */
        cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        if (!cJSON_IsString(id) || strcmp(id->valuestring, "msg_upstream_1") != 0) {
            fprintf(stderr, "id should be preserved, got: %s\n", cJSON_IsString(id) ? id->valuestring : "(missing)");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_anthropic_nonstreaming_missing_model_injected(void) {
    const char *anth_json =
        "{\"id\":\"msg_x\",\"type\":\"message\",\"content\":[]}";
    char *out = passthrough_anthropic_override_model(anth_json, "qwen-coder-passthru");
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
            fprintf(stderr, "expected injected model=\"qwen-coder-passthru\" (Scenario 4.1), got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_anthropic_nonstreaming_empty_model_injected(void) {
    const char *anth_json =
        "{\"id\":\"msg_x\",\"type\":\"message\",\"model\":\"\",\"content\":[]}";
    char *out = passthrough_anthropic_override_model(anth_json, "qwen-coder-passthru");
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
            fprintf(stderr, "expected injected model=\"qwen-coder-passthru\" (Scenario 4.1 empty), got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_anthropic_passthrough_error_response_includes_model(void) {
    /* Scenario 4.3: upstream returns 4xx/5xx with error body. worker.c
     * forwards body through the override helper, which must inject model
     * even when the body has no model field. */
    const char *err_body = "{\"type\":\"error\",\"error\":{\"type\":\"overloaded\",\"message\":\"upstream busy\"}}";
    char *out = passthrough_anthropic_override_model(err_body, "qwen-coder-passthru");
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder-passthru") != 0) {
            fprintf(stderr, "passthrough error should include .model, got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        /* other fields must be preserved */
        cJSON *type = root ? cJSON_GetObjectItemCaseSensitive(root, "type") : NULL;
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "error") != 0) {
            fprintf(stderr, "type should be preserved\n");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_openai_passthrough_error_response_includes_model(void) {
    const char *err_body = "{\"error\":{\"message\":\"rate limit\",\"code\":\"429\"}}";
    char *out = passthrough_openai_override_model(err_body, "qwen-coder");
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder") != 0) {
            fprintf(stderr, "PT_OPENAI error should include .model, got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        cJSON *e = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
        cJSON *code = e ? cJSON_GetObjectItemCaseSensitive(e, "code") : NULL;
        if (!cJSON_IsString(code) || strcmp(code->valuestring, "429") != 0) {
            fprintf(stderr, "error.code should be preserved\n");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_openai_nonstreaming_model_overridden(void) {
    const char *oai_json =
        "{\"id\":\"chatcmpl-X\",\"object\":\"chat.completion\","
        "\"model\":\"qwen3-coder-plus-2025-01\","
        "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}}";
    char *out = passthrough_openai_override_model(oai_json, "qwen-coder");
    int failed = 0;
    if (!out) {
        failed = 1;
    } else {
        cJSON *root = cJSON_Parse(out);
        cJSON *model = root ? cJSON_GetObjectItemCaseSensitive(root, "model") : NULL;
        if (!cJSON_IsString(model) || strcmp(model->valuestring, "qwen-coder") != 0) {
            fprintf(stderr, "expected model=\"qwen-coder\", got: %s\n",
                    cJSON_IsString(model) ? model->valuestring : "(missing)");
            failed = 1;
        }
        /* id / object / choices must be preserved */
        cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        if (!cJSON_IsString(id) || strcmp(id->valuestring, "chatcmpl-X") != 0) {
            fprintf(stderr, "id should be preserved\n");
            failed = 1;
        }
        cJSON_Delete(root);
        free(out);
    }
    return failed;
}

static int test_pt_none_error_response_includes_model(void) {
    int failed = 0;
    /* Path 1: invalid upstream JSON -> parse-fail error */
    cJSON *err1 = convert_openai_response_to_anthropic("not valid json", "qwen-coder");
    cJSON *m1 = err1 ? cJSON_GetObjectItemCaseSensitive(err1, "model") : NULL;
    if (!cJSON_IsString(m1) || strcmp(m1->valuestring, "qwen-coder") != 0) {
        fprintf(stderr, "parse-fail error should include .model, got: %s\n",
                cJSON_IsString(m1) ? m1->valuestring : "(missing)");
        failed = 1;
    }
    cJSON *type1 = err1 ? cJSON_GetObjectItemCaseSensitive(err1, "type") : NULL;
    if (!cJSON_IsString(type1) || strcmp(type1->valuestring, "error") != 0) {
        fprintf(stderr, "error type should be \"error\", got: %s\n",
                cJSON_IsString(type1) ? type1->valuestring : "(missing)");
        failed = 1;
    }
    cJSON_Delete(err1);

    /* Path 2: upstream returns OpenAI error object -> business error response */
    cJSON *err2 = convert_openai_response_to_anthropic(
        "{\"error\":{\"message\":\"upstream rate limit\",\"code\":\"rate_limit\"}}",
        "qwen-coder");
    cJSON *m2 = err2 ? cJSON_GetObjectItemCaseSensitive(err2, "model") : NULL;
    if (!cJSON_IsString(m2) || strcmp(m2->valuestring, "qwen-coder") != 0) {
        fprintf(stderr, "upstream-error response should include .model, got: %s\n",
                cJSON_IsString(m2) ? m2->valuestring : "(missing)");
        failed = 1;
    }
    /* upstream error.code should still be passed through */
    cJSON *e2 = err2 ? cJSON_GetObjectItemCaseSensitive(err2, "error") : NULL;
    cJSON *code2 = e2 ? cJSON_GetObjectItemCaseSensitive(e2, "code") : NULL;
    if (!cJSON_IsString(code2) || strcmp(code2->valuestring, "rate_limit") != 0) {
        fprintf(stderr, "upstream error.code should be preserved\n");
        failed = 1;
    }
    cJSON_Delete(err2);
    return failed;
}

static int test_default_preserves_reasoning_content(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"hello\"},"
        "  {\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"deep thought\"},{\"type\":\"text\",\"text\":\"hi\"}]}"
        "]"
        "}";
    const char *cfg_json = "{\"upstream_model\":\"upstream-model\"}";

    cJSON *anth = cJSON_Parse(anth_json);
    cJSON *cfg = cJSON_Parse(cfg_json);
    cJSON *out = build_openai_request(anth, cfg);
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!printed) {
        failed = 1;
    } else if (!strstr(printed, "\"reasoning_content\"")) {
        fprintf(stderr, "default should preserve reasoning_content from thinking block, got: %s\n", printed);
        failed = 1;
    }

    free(printed);
    cJSON_Delete(out);
    cJSON_Delete(anth);
    cJSON_Delete(cfg);
    return failed;
}

static int test_strip_reasoning_content_when_enabled(void) {
    const char *anth_json =
        "{"
        "\"model\":\"gateway-model\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"hello\"},"
        "  {\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"deep thought\"},{\"type\":\"text\",\"text\":\"hi\"}]}"
        "]"
        "}";
    const char *cfg_json = "{\"upstream_model\":\"upstream-model\",\"strip_reasoning_content\":true}";

    cJSON *anth = cJSON_Parse(anth_json);
    cJSON *cfg = cJSON_Parse(cfg_json);
    cJSON *out = build_openai_request(anth, cfg);
    char *printed = cJSON_PrintUnformatted(out);
    int failed = 0;

    if (!printed) {
        failed = 1;
    } else if (strstr(printed, "\"reasoning_content\"")) {
        fprintf(stderr, "strip_reasoning_content=true should remove reasoning_content, got: %s\n", printed);
        failed = 1;
    }

    free(printed);
    cJSON_Delete(out);
    cJSON_Delete(anth);
    cJSON_Delete(cfg);
    return failed;
}

int main(void) {
    int failed = 0;
    failed |= test_overlapping_request_fields_are_replaced();
    failed |= test_openai_reasoning_alias_converts_to_thinking_block();
    failed |= test_system_cch_is_stripped_in_openai_conversion();
    failed |= test_system_cch_is_stripped_in_passthrough_string();
    failed |= test_system_cch_is_stripped_in_passthrough_array();
    failed |= test_pt_none_nonstreaming_response_model_is_gateway_id();
    failed |= test_pt_anthropic_nonstreaming_model_overridden();
    failed |= test_pt_anthropic_nonstreaming_missing_model_injected();
    failed |= test_pt_anthropic_nonstreaming_empty_model_injected();
    failed |= test_pt_openai_nonstreaming_model_overridden();
    failed |= test_pt_anthropic_passthrough_error_response_includes_model();
    failed |= test_pt_openai_passthrough_error_response_includes_model();
    failed |= test_pt_none_error_response_includes_model();
    failed |= test_default_preserves_reasoning_content();
    failed |= test_strip_reasoning_content_when_enabled();
    return failed ? 1 : 0;
}
