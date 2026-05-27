#ifndef CONVERT_H
#define CONVERT_H

#include <stdbool.h>
#include "types.h"

char *xstrdup(const char *s);
char *make_id(const char *prefix);
cJSON *string_or_content_text(cJSON *content);
cJSON *json_from_string_or_empty_object(const char *s);
cJSON *anthropic_content_to_openai_content(cJSON *content, bool *has_non_text);
cJSON *anthropic_tools_to_openai(cJSON *tools);
cJSON *anthropic_tool_choice_to_openai(cJSON *tc);
cJSON *convert_message_content_blocks(cJSON *messages, const char *role, cJSON *content);
cJSON *build_openai_request(cJSON *anth_req, cJSON *model_cfg);
char *make_upstream_url(cJSON *model_cfg);
const char *map_finish_reason(const char *fr);
cJSON *openai_message_to_anthropic_content(cJSON *msg);
cJSON *convert_openai_response_to_anthropic(const char *body, const char *client_model);

#endif