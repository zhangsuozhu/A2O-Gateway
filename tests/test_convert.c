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

int main(void) {
    int failed = 0;
    failed |= test_overlapping_request_fields_are_replaced();
    failed |= test_openai_reasoning_alias_converts_to_thinking_block();
    failed |= test_system_cch_is_stripped_in_openai_conversion();
    failed |= test_system_cch_is_stripped_in_passthrough_string();
    failed |= test_system_cch_is_stripped_in_passthrough_array();
    return failed ? 1 : 0;
}
