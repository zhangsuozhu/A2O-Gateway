// File: tests/test_cache_control.c
// 红灯阶段：本文件引用的 convert_inject_system_cache / convert_inject_tools_cache
//          / cache_control_ephemeral / approx_token_count / stats_record_cache_*
//          均尚未实现。链接必然失败 → ctest 红。

#include "convert.h"
#include "stats.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

/* ---------- 1. 长 system prompt 自动获得 cache_control ---------- */
static int test_long_system_injects_cache_control(void) {
    char *big = malloc(20000);
    char *p = big;
    for (int i = 0; i < 1600; i++) p += sprintf(p, "word%d ", i);
    *p = 0;

    cJSON *model_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(model_cfg, "cache_policy", "auto");
    cJSON_AddNumberToObject(model_cfg, "min_cache_tokens", 1024);

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", big);

    bool injected = convert_inject_system_cache(sys_msg, NULL, model_cfg);

    int rc = 0;
    if (!injected) rc = fail("injected should be true for 2000-token system");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(sys_msg, "content");
    if (!cJSON_IsArray(content)) rc = fail("content should be array after injection");
    else {
        cJSON *last = cJSON_GetArrayItem(content, cJSON_GetArraySize(content) - 1);
        cJSON *cc = cJSON_GetObjectItemCaseSensitive(last, "cache_control");
        if (!cc) rc = fail("last block should have cache_control");
        else {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(cc, "type");
            if (!cJSON_IsString(t) || strcmp(t->valuestring, "ephemeral") != 0)
                rc = fail("cache_control.type should be \"ephemeral\"");
        }
    }

    free(big);
    cJSON_Delete(model_cfg);
    cJSON_Delete(sys_msg);
    return rc;
}

/* ---------- 2. 短 system prompt 不注入 ---------- */
static int test_short_system_no_inject(void) {
    cJSON *model_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(model_cfg, "cache_policy", "auto");
    cJSON_AddNumberToObject(model_cfg, "min_cache_tokens", 1024);

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "you are helpful.");

    bool injected = convert_inject_system_cache(sys_msg, NULL, model_cfg);

    int rc = 0;
    if (injected) rc = fail("short system should not be injected");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(sys_msg, "content");
    if (!cJSON_IsString(content)) rc = fail("short system content should remain string");

    cJSON_Delete(model_cfg);
    cJSON_Delete(sys_msg);
    return rc;
}

/* ---------- 3. tools 非空，最后一个 tool 获 cache_control ---------- */
static int test_last_tool_gets_cache_control(void) {
    cJSON *model_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(model_cfg, "cache_policy", "auto");

    cJSON *tools = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        char name[32]; snprintf(name, sizeof(name), "fn%d", i);
        cJSON_AddStringToObject(fn, "name", name);
        cJSON_AddItemToObject(t, "function", fn);
        cJSON_AddItemToArray(tools, t);
    }

    bool injected = convert_inject_tools_cache(tools, model_cfg);

    int rc = 0;
    if (!injected) rc = fail("last tool should be injected");
    cJSON *first_fn = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(tools, 0), "function");
    if (cJSON_GetObjectItemCaseSensitive(first_fn, "cache_control"))
        rc = fail("first tool must not have cache_control");
    cJSON *last_fn = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(tools, 2), "function");
    cJSON *cc = cJSON_GetObjectItemCaseSensitive(last_fn, "cache_control");
    if (!cc) rc = fail("last tool should have cache_control");
    else {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(cc, "type");
        if (!cJSON_IsString(t) || strcmp(t->valuestring, "ephemeral") != 0)
            rc = fail("last tool cache_control.type should be ephemeral");
    }

    cJSON_Delete(model_cfg);
    cJSON_Delete(tools);
    return rc;
}

/* ---------- 4. tools 为空数组不注入 ---------- */
static int test_empty_tools_no_inject(void) {
    cJSON *model_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(model_cfg, "cache_policy", "auto");
    cJSON *tools = cJSON_CreateArray();

    bool injected = convert_inject_tools_cache(tools, model_cfg);

    int rc = (injected) ? fail("empty tools should not be injected") : 0;
    cJSON_Delete(model_cfg);
    cJSON_Delete(tools);
    return rc;
}

/* ---------- 5. cache_policy = "off" 完全跳过 ---------- */
static int test_policy_off_skips_all(void) {
    cJSON *model_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(model_cfg, "cache_policy", "off");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "content", "x");
    bool s = convert_inject_system_cache(sys_msg, NULL, model_cfg);

    cJSON *tools = cJSON_CreateArray();
    cJSON *t = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "f");
    cJSON_AddItemToObject(t, "function", fn);
    cJSON_AddItemToArray(tools, t);
    bool t_inj = convert_inject_tools_cache(tools, model_cfg);

    int rc = 0;
    if (s)     rc = fail("policy=off should not inject system");
    if (t_inj) rc = fail("policy=off should not inject tools");
    if (cJSON_GetObjectItemCaseSensitive(fn, "cache_control"))
        rc = fail("policy=off must not add cache_control to tool");

    cJSON_Delete(model_cfg);
    cJSON_Delete(sys_msg);
    cJSON_Delete(tools);
    return rc;
}

/* ---------- 6. 缺省配置（NULL / 无字段）默认 off，不崩溃 ---------- */
static int test_default_config_is_off(void) {
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "content", "x");
    bool s = convert_inject_system_cache(sys_msg, NULL, NULL);

    cJSON *tools = cJSON_CreateArray();
    bool t_inj = convert_inject_tools_cache(tools, NULL);

    int rc = 0;
    if (s)     rc = fail("NULL model_cfg should default to off");
    if (t_inj) rc = fail("NULL model_cfg should default to off");

    cJSON_Delete(sys_msg);
    cJSON_Delete(tools);
    return rc;
}

/* ---------- 7. approx_token_count 启发式 ---------- */
static int test_approx_token_count_heuristic(void) {
    int rc = 0;
    if (approx_token_count(NULL) != 0)   rc = fail("NULL should give 0");
    if (approx_token_count("")      != 0) rc = fail("empty should give 0");
    int n = approx_token_count("hello world foo");
    if (n < 3 || n > 4)                  rc = fail("3 words should give ~3-4");
    return rc;
}

/* ---------- 8. cache_control_ephemeral 形状 ---------- */
static int test_cache_control_ephemeral_shape(void) {
    cJSON *cc = cache_control_ephemeral();
    int rc = 0;
    if (!cc) return fail("should return non-NULL");
    cJSON *t = cJSON_GetObjectItemCaseSensitive(cc, "type");
    if (!cJSON_IsString(t) || strcmp(t->valuestring, "ephemeral") != 0)
        rc = fail("type should be ephemeral");
    cJSON_Delete(cc);
    return rc;
}

/* ---------- 9. 缓存读 token 被记录 + 节省金额 ---------- */
static int test_stats_record_cache_read(void) {
    stats_reset_for_test();
    stats_set_input_price_for_test("qwen-coder", 1.0);  /* 必须先设价再 record */
    stats_record_cache_read("qwen-coder", "test", 5000);

    unsigned long got = stats_get_cache_read("qwen-coder", "test");
    if (got != 5000) return fail("cache_read tokens should be 5000");

    stats_record_cache_read("qwen-coder", "test", 1000);
    double saved = stats_get_saved_cost("qwen-coder", "test");
    /* saved = 6000 * 1e-6 * 0.9 = 0.0054 */
    if (saved < 0.005 || saved > 0.006)
        return fail("saved_cost_usd miscomputed for cache reads");
    return 0;
}

/* ---------- 10. 缓存写 token 被记录，扣减节省金额 ---------- */
static int test_stats_record_cache_creation(void) {
    stats_reset_for_test();
    stats_set_input_price_for_test("qwen-coder", 1.0);
    stats_record_cache_creation("qwen-coder", "test", 2000);

    unsigned long got = stats_get_cache_creation("qwen-coder", "test");
    if (got != 2000) return fail("cache_creation tokens should be 2000");

    double saved = stats_get_saved_cost("qwen-coder", "test");
    /* saved = -2000 * 1e-6 * 0.25 = -0.0005 */
    if (saved < -0.001 || saved > 0.0001)
        return fail("saved_cost_usd should subtract 0.25x for cache writes");
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= test_long_system_injects_cache_control();
    failed |= test_short_system_no_inject();
    failed |= test_last_tool_gets_cache_control();
    failed |= test_empty_tools_no_inject();
    failed |= test_policy_off_skips_all();
    failed |= test_default_config_is_off();
    failed |= test_approx_token_count_heuristic();
    failed |= test_cache_control_ephemeral_shape();
    failed |= test_stats_record_cache_read();
    failed |= test_stats_record_cache_creation();
    if (failed == 0) printf("ALL CACHE CONTROL TESTS PASSED\n");
    return failed ? 1 : 0;
}
