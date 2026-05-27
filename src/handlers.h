/**
 * @file handlers.h
 * @brief HTTP 路由处理接口
 *
 * 本文件声明了网关的顶层 HTTP 请求分发入口 handle_root。
 * 所有客户端请求（包括 Anthropic Messages API、模型列表、token 计数、
 * 管理后台、健康检查等）均由 libevent 的 evhttp 模块接收后，
 * 统一交给 handle_root 进行 URI 路由分发。
 */

#ifndef HANDLERS_H
#define HANDLERS_H

#include <event2/http.h>
#include "types.h"

/**
 * @brief 顶层 HTTP 请求回调，负责 URI 路由分发
 * @param req  libevent 的 HTTP 请求对象
 * @param arg  用户参数（当前未使用）
 *
 * handle_root 解析请求方法和 URI，将其分发到对应的内部处理函数，如：
 *   - /healthz、/readyz        -> handle_health
 *   - /v1/messages             -> handle_messages
 *   - /v1/messages/count_tokens -> handle_count_tokens
 *   - /v1/models               -> handle_models
 *   - /admin、/                -> handle_admin_html
 *   - /admin/api/config        -> handle_config_get / handle_config_put
 *   - /admin/api/switch        -> handle_switch
 * 未匹配的 URI 返回 404。
 */
void handle_root(struct evhttp_request *req, void *arg);

#endif