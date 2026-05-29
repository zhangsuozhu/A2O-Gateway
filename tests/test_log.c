#include "log.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_contains(const char *text, const char *needle) {
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "expected extracted text to contain %s, got: %s\n",
                needle, text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int test_extracts_user_prompt_from_anthropic_request(void) {
    const char *json =
        "{"
        "\"system\":[{\"type\":\"text\",\"text\":\"follow instructions\"}],"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"explain epoll\"}]}]"
        "}";
    cJSON *j = cJSON_Parse(json);
    membuf_t out;
    membuf_init(&out);

    rt_extract_text_from_json(j, &out, "[REQ]");

    int failed = 0;
    failed |= expect_contains(out.ptr, "[REQ][system]\nfollow instructions");
    failed |= expect_contains(out.ptr, "[REQ][user]\nexplain epoll");

    membuf_free(&out);
    cJSON_Delete(j);
    return failed;
}

static int test_extracts_openai_reasoning_content(void) {
    const char *json =
        "{"
        "\"choices\":[{\"message\":{\"reasoning_content\":\"think step\",\"content\":\"final answer\"}}]"
        "}";
    cJSON *j = cJSON_Parse(json);
    membuf_t out;
    membuf_init(&out);

    rt_extract_text_from_json(j, &out, "[RES]");

    int failed = 0;
    failed |= expect_contains(out.ptr, "[RES][thinking]\nthink step");
    failed |= expect_contains(out.ptr, "[RES][assistant]\nfinal answer");

    membuf_free(&out);
    cJSON_Delete(j);
    return failed;
}

int main(void) {
    int failed = 0;
    failed |= test_extracts_user_prompt_from_anthropic_request();
    failed |= test_extracts_openai_reasoning_content();
    return failed ? 1 : 0;
}
