# Claude Code OpenAI-Compatible Gateway

用 C 实现的本地 API 网关，将 Anthropic Messages API 转换为 OpenAI Chat Completions 协议，让 Claude Code 可以使用任意兼容 OpenAI 格式的模型服务。

```
Claude Code (Anthropic Protocol)
  → POST /v1/messages
  → 本网关 (协议转换)
  → POST /v1/chat/completions (OpenAI Protocol)
  → 阿里百炼 / DeepSeek / Moonshot / SiliconFlow / Ollama / vLLM / ...
```

适用场景：你正在使用 Claude Code，但手里只有 OpenAI 兼容格式的 API 接口。

<p align="center">
  <a href="https://github.com/zhangsuozhu/A2O-Gateway">⭐ 如果这个项目对你有帮助，请给一个 Star ❤️</a>
</p>

## 快速开始

### 安装依赖

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential cmake pkg-config \
  libevent-dev libcurl4-openssl-dev libcjson-dev ca-certificates

# macOS
brew install cmake pkg-config libevent curl cjson
```

### 构建 & 运行

```bash
make build

# 首次运行会自动生成默认配置
./build/cc-oai-gateway

# 指定配置文件
./build/cc-oai-gateway -f ./config/gateway.local.json

# 指定端口、密码、worker 数
./build/cc-oai-gateway -p 9090 -P mypass -w 6

# 后台守护进程
./build/cc-oai-gateway -d
```

编辑 `config/gateway.local.json`，填入你的上游 API Key：

```json
{
  "gateway_api_key": "cc-local-token",
  "active_model": "qwen-coder",
  "models": [{
    "id": "qwen-coder",
    "provider": "aliyun-bailian",
    "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
    "api_key": "你的真实 API Key",
    "upstream_model": "qwen3-coder-plus"
  }]
}
```

### 配置 Claude Code

```bash
export ANTHROPIC_BASE_URL="http://127.0.0.1:8081"
export ANTHROPIC_AUTH_TOKEN="cc-local-token"
export ANTHROPIC_API_KEY="cc-local-token"
export ANTHROPIC_MODEL="qwen-coder"

claude --model qwen-coder
```

或通过项目级 `.claude/settings.json`：

```json
{
  "env": {
    "ANTHROPIC_BASE_URL": "http://127.0.0.1:8081",
    "ANTHROPIC_AUTH_TOKEN": "cc-local-token",
    "ANTHROPIC_API_KEY": "cc-local-token",
    "ANTHROPIC_MODEL": "qwen-coder",
    "CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY": "1"
  }
}
```

> `CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY=1` 让 Claude Code 通过 `GET /v1/models` 自动发现可用模型列表。

## CLI 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-f, --file PATH` | 配置文件路径 | `./config/gateway.json` |
| `-p, --port PORT` | 监听端口 | `8081` |
| `-P, --password PASS` | Web 管理密码 | 空 |
| `-w, --workers NUM` | Worker 线程数 (1-8) | `4` |
| `-d, --daemon` | 后台守护进程 | 前台运行 |
| `-h, --help` | 显示帮助 | — |

也支持 `GATEWAY_CONFIG` 环境变量指定配置文件路径。

## 三种处理模式

| 模式 | 客户端协议 | 上游协议 | 说明 |
|------|-----------|---------|------|
| **协议转换**（默认） | Anthropic `/v1/messages` | OpenAI `/chat/completions` | Anthropic ↔ OpenAI 双向转换 |
| **Anthropic 透传** | Anthropic `/v1/messages` | Anthropic `/v1/messages` | `api_mode: "passthrough"`，直接转发 |
| **OpenAI 透传** | OpenAI `/v1/chat/completions` | OpenAI `/chat/completions` | 监控模式，不转换 |

所有模式均记录统计和历史。

## API 端点

### 业务接口（需 gateway_api_key 认证）

| Method | Path | 说明 |
|--------|------|------|
| POST | `/v1/messages` | 聊天补全（流式+非流式） |
| POST | `/v1/messages/count_tokens` | Token 近似估算 |
| GET | `/v1/models` | 模型列表 |
| POST | `/v1/chat/completions` | OpenAI 透传接口 |

### 管理接口（需 session 认证）

| Method | Path | 说明 |
|--------|------|------|
| GET | `/admin` | Web 管理界面 |
| POST | `/admin/api/login` | 密码登录 |
| POST | `/admin/api/logout` | 登出 |
| GET | `/admin/api/config` | 查看配置（密钥已脱敏） |
| PUT/POST | `/admin/api/config` | 热更新配置 |
| POST | `/admin/api/switch` | 切换默认模型 |
| POST | `/admin/api/change-password` | 修改管理密码 |
| GET | `/admin/api/stats` | 请求统计 |
| POST | `/admin/api/stats/reset` | 重置统计 |
| POST | `/admin/api/db/reset` | 重置数据库 |
| GET | `/admin/api/history` | 请求历史 |
| GET | `/admin/api/hourly` | 小时聚合统计 |
| GET | `/admin/api/daily` | 日聚合统计 |
| GET | `/admin/api/debug` | 实时调试信息 |
| GET | `/admin/api/error_logs` | 错误日志 |

### 公开接口

| Method | Path | 说明 |
|--------|------|------|
| GET | `/healthz` | 健康检查 |
| GET | `/readyz` | 健康检查别名 |
| GET | `/` | 跳转到 `/admin` |

## 配置字段说明

### 顶层配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `listen_host` | string | `0.0.0.0` | 监听地址 |
| `listen_port` | number | `8081` | 监听端口 |
| `log_level` | string | `info` | 日志级别：`debug`/`info`/`warn`/`error` |
| `realtime_print` | string | `false` | 实时打印：`false`/`all`（完整 JSON）/`txt`（纯文本） |
| `gateway_api_key` | string | — | 网关访问凭据 |
| `admin_password` | string | 空 | Web 管理界面登录密码 |
| `db_path` | string | `/var/log/gateway.db` | SQLite 数据库路径 |
| `log_file` | string | `/var/log/cc-oai-gateway.log` | 日志文件路径 |
| `worker_threads` | number | `4` | Worker 线程数 (1-8) |
| `active_model` | string | — | 默认模型 ID |

### 模型配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | string | — | **必填**，Claude Code 侧模型名 |
| `provider` | string | — | 厂商标识（日志/统计用） |
| `base_url` | string | — | OpenAI 兼容接口地址 |
| `endpoint` | string | — | 完整接口地址，优先级高于 `base_url` |
| `api_key` | string | — | **必填**，上游 API Key |
| `upstream_model` | string | — | **必填**，上游真实模型名 |
| `api_mode` | string | `openai_chat_completions` | `openai_chat_completions`（转换）或 `passthrough`（透传） |
| `priority` | number | `100` | 优先级 0-1000，越小越优先 |
| `enabled` | bool | `true` | 是否启用 |
| `params` | object | — | temperature/top_p/max_tokens 等 |
| `extra_body` | object | — | 厂商私有参数，合并到请求体 |
| `cache_policy` | string | `off` | `off` 或 `auto`（自动注入 cache_control） |
| `min_cache_tokens` | number | `1024` | 自动缓存注入的 token 阈值 |
| `prompt_tokens_includes_cache` | bool | `true` | prompt_tokens 是否已含缓存 token |
| `strip_reasoning_content` | bool | `false` | 剥离 upstream 请求中的 reasoning_content |

### `prompt_tokens_includes_cache` 说明

大多数厂商（OpenAI/Anthropic/DeepSeek）的 `prompt_tokens` 已包含缓存命中/创建的 token，设为 `true`。Moonshot 等厂商的 `prompt_tokens` 不含缓存 token，需设为 `false`，网关会自动合并 `prompt_tokens + cache_read + cache_creation`。

### `strip_reasoning_content` 说明

DeepSeek 等上游会将 assistant message 中的 `reasoning_content` 按普通 prompt input 计费且不参与缓存。开启此选项后，网关在构建上游请求前自动剥离 assistant message 中的 `reasoning_content`，避免重复计费。

## 协议转换

### Anthropic → OpenAI

| Anthropic | OpenAI |
|-----------|--------|
| `system` 参数 | `messages[0].role=system` |
| `tools[].input_schema` | `tools[].function.parameters` |
| `tool_choice` | `tool_choice` |
| `max_tokens` | `max_tokens` |
| `stop_sequences` | `stop` |
| `stream` | `stream` |

### OpenAI → Anthropic

| OpenAI | Anthropic |
|--------|-----------|
| `message.content` | `content[{type:text}]` |
| `tool_calls` | `content[{type:tool_use}]` |
| `reasoning_content` | `content[{type:thinking}]` |
| `finish_reason=stop` | `stop_reason=end_turn` |
| `finish_reason=length` | `stop_reason=max_tokens` |
| `finish_reason=tool_calls` | `stop_reason=tool_use` |
| `usage.prompt_tokens` | `usage.input_tokens` |
| `usage.completion_tokens` | `usage.output_tokens` |

## Web 管理界面

访问 `http://127.0.0.1:8081/admin`，功能包括：

- 查看/修改完整配置（热加载，即时生效）
- 一键切换默认模型
- 修改管理员密码
- 请求统计面板（按模型/时间/状态码聚合）
- 请求历史查询
- Claude Code 连接信息一键生成

配置热加载时，Web UI 中显示为 `***MASKED***` 的 API Key 会在服务端自动保留原值，无需重新输入。

## Docker

```bash
docker build -t cc-oai-gateway:latest .
docker run --rm -p 8081:8081 \
  -v "$PWD/config/gateway.local.json:/app/config/gateway.json" \
  cc-oai-gateway:latest
```

## 手工测试

```bash
# 健康检查
curl http://127.0.0.1:8081/healthz

# 非流式
curl http://127.0.0.1:8081/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# 流式
curl -N http://127.0.0.1:8081/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-stream.json

# Token 估算
curl http://127.0.0.1:8081/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# OpenAI 透传（非流式）
curl http://127.0.0.1:8081/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d '{"model":"qwen-coder","messages":[{"role":"user","content":"hi"}]}'
```

## 技术栈

- **C11** + CMake 构建
- **libevent** — HTTP 服务器
- **libcurl multi** — 上游 HTTP 客户端（worker 线程池，连接复用）
- **cJSON** — JSON 解析
- **SQLite** (WAL 模式) — 请求历史和统计数据持久化
- Admin UI 编译时嵌入二进制（xxd -i），运行时无需静态文件

## 已知限制

- `count_tokens` 为近似估算（字节数/4），非真实 tokenizer
- 强制 HTTP/1.1 连接上游（避免 DeepSeek 等厂商的 HTTP/2 帧错误）
- 不同厂商对 `stream_options`、`max_tokens` 上限、工具调用格式的兼容性不同
- 工具调用流式模式依赖 SSE chunk 拼接，部分厂商格式可能不标准
- 不实现 OpenAI Responses API
