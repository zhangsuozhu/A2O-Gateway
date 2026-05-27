#ifndef STREAM_H
#define STREAM_H

#include <event2/util.h>
#include "types.h"

void stream_send_chunk(gateway_job_t *job, const char *data);
void stream_emit_json(gateway_job_t *job, const char *event, cJSON *obj);
void stream_start_reply(gateway_job_t *job);
void stream_message_start(gateway_job_t *job);
void stream_text_start(gateway_job_t *job);
void stream_text_delta(gateway_job_t *job, const char *text);
tool_stream_state_t *get_tool_state(stream_state_t *s, int openai_index);
void stream_tool_start_if_needed(gateway_job_t *job, tool_stream_state_t *ts);
void stream_tool_json_delta(gateway_job_t *job, tool_stream_state_t *ts, const char *partial);
void stream_emit_error(gateway_job_t *job, const char *message);
void stream_finish(gateway_job_t *job);
void stream_state_free(stream_state_t *s);
void handle_openai_stream_json(gateway_job_t *job, const char *json);
void process_sse_line(gateway_job_t *job, char *line);
void stream_parse_append(gateway_job_t *job, const char *ptr, size_t n);
size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata);
void deferred_send(evutil_socket_t fd, short what, void *arg);

#endif