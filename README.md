# Claude Code OpenAI-Compatible Gateway

这是一个用 C 实现的本地 API 网关：

```text
Claude Code
  -> Anthropic Messages API: POST /v1/messages
  -> 本网关
  -> OpenAI-compatible Chat Completions: POST /v1/chat/completions
  -> 任意兼容 OpenAI 格式的模型厂商
```

适用场景：你正在使用 Claude Code，但手里只有 OpenAI 兼容格式的接口，例如自建模型服务、阿里百炼 compatible-mode、DeepSeek、Moonshot、SiliconFlow、Ollama/OpenWebUI/vLLM 等。

## 能力清单

- 对 Claude Code 暴露 Anthropic Messages 兼容接口：
  - `POST /v1/messages` — 聊天补全（流式+非流式）
  - `POST /v1/messages/count_tokens` — Token 近似估算
  - `GET /v1/models` — 模型发现
- 向上游请求 OpenAI-compatible Chat Completions：
  - 默认拼接 `{base_url}/chat/completions`
  - 也可配置完整 `endpoint`
- 多模型、多厂商配置（实时生效）：
  - `id`: Claude Code 侧看到的模型名
  - `provider`: 模型厂商标识
  - `base_url` / `endpoint`: 模型接口地址
  - `api_key`: 上游密钥
  - `upstream_model`: 上游真实模型名称
  - `params`: temperature/top_p 等常规参数
  - `extra_body`: 厂商私有扩展参数
- 双认证方式：`Authorization: Bearer` 或 `x-api-key` 请求头
- 健康检查端点：`GET /healthz` 和 `GET /readyz`
- 支持 Web 配置界面：`http://127.0.0.1:8080/admin`（根路径 `/` 也跳转管理页）
  - 运行时查看/修改配置
  - 一键切换默认模型
  - 一键生成 Claude Code 配置
- 支持默认模型切换：Claude Code 没传入匹配模型时使用 `active_model`
- 支持按 `model` 路由：Claude Code 的 `ANTHROPIC_MODEL` 或 `claude --model xxx` 对应配置中的 `models[].id`
- 支持流式 SSE 转换：OpenAI chunk -> Anthropic stream events
- 支持工具调用转换：Anthropic tools/tool_use <-> OpenAI tools/tool_calls，流式+非流式
- 使用 `libcurl multi` + worker 线程池（可配置），连接复用上游
- 使用 `libevent` 提供 HTTP 服务
- 配置文件热加载（通过 Web UI 修改即时生效）

## 依赖

Debian / Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  libevent-dev libcurl4-openssl-dev libcjson-dev ca-certificates
```

macOS:

```bash
brew install cmake pkg-config libevent curl cjson
```

## 构建

```bash
make build
```

## 运行

先编辑配置：

```bash
cp config/gateway.json config/gateway.local.json
vim config/gateway.local.json
```

至少修改：

```json
{
  "gateway_api_key": "cc-local-token",
  "admin_token": "admin-local-token",
  "active_model": "qwen-coder",
  "models": [
    {
      "id": "qwen-coder",
      "provider": "aliyun-bailian",
      "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "api_key": "真实 API Key",
      "upstream_model": "qwen3-coder-plus"
    }
  ]
}
```

启动：

```bash
./build/cc-oai-gateway ./config/gateway.local.json
```

打开 Web 配置界面：

```text
http://127.0.0.1:8080/admin
```

输入 `admin_token` 后加载配置。

## 配置 Claude Code

Shell 环境变量方式：

```bash
export ANTHROPIC_BASE_URL="http://127.0.0.1:8080"
export ANTHROPIC_AUTH_TOKEN="cc-local-token"
export ANTHROPIC_API_KEY="cc-local-token"
export ANTHROPIC_MODEL="qwen-coder"
export CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS=1
# 可选：让新版 Claude Code 启用网关模型发现；若模型 ID 不以 claude/anthropic 开头，建议仍用 ANTHROPIC_MODEL 或 --model 指定。
export CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY=1

claude --model qwen-coder
```

也可以在 Web UI 点击“生成 Claude Code 配置”。

项目级 `.claude/settings.json` 可以这样写，具体字段以你当前 Claude Code 版本支持为准：

```json
{
  "env": {
    "ANTHROPIC_BASE_URL": "http://127.0.0.1:8080",
    "ANTHROPIC_AUTH_TOKEN": "cc-local-token",
    "ANTHROPIC_API_KEY": "cc-local-token",
    "ANTHROPIC_MODEL": "qwen-coder",
    "CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS": "1"
  }
}
```

## 手工测试

健康检查：
```bash
curl http://127.0.0.1:8080/healthz
```

非流式：
```bash
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json
```

流式：
```bash
curl -N http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-stream.json
```

工具调用（非流式）：
```bash
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-tool.json
```

Token 估算：
```bash
curl http://127.0.0.1:8080/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json
```

注意：`count_tokens` 当前是本地估算，不是精确分词。Claude Code 常用它做上下文预估，若你的上游厂商提供 tokenizer API，可以在后续版本中替换为真实实现。

## 配置字段说明

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 8080,
  "log_level": "info",
  "realtime_print": "false",
  "gateway_api_key": "Claude Code 访问本网关的 token",
  "admin_token": "Web 管理界面 token",
  "worker_threads": 4,
  "active_model": "默认模型 id",
  "models": [
    {
      "id": "Claude Code 侧使用的模型名",
      "provider": "厂商名，仅用于标识和日志",
      "display_name": "Web UI 展示名称",
      "interface": "openai_chat_completions",
      "base_url": "OpenAI 兼容 base_url，例如 https://api.example.com/v1",
      "endpoint": "完整接口地址，可空；非空时优先使用",
      "api_key": "上游模型厂商 API Key",
      "upstream_model": "上游真实模型名",
      "enabled": true,
      "params": {
        "temperature": 0.2,
        "top_p": 0.95,
        "max_tokens": 4096
      },
      "extra_body": {
        "enable_search": true
      }
    }
  ]
}
```

### 配置字段明细

| 字段 | 层级 | 类型 | 说明 |
|------|------|------|------|
| `listen_host` | 顶层 | string | 监听地址，默认 `0.0.0.0` |
| `listen_port` | 顶层 | number | 监听端口，默认 `8080` |
| `log_level` | 顶层 | string | 日志级别：`debug`/`info`/`warn`/`error` |
| `realtime_print` | 顶层 | string | 实时输出模式：`"false"` 关闭、`"all"` 完整 JSON、`"txt"` 纯文本（仅提取对话内容） |
| `gateway_api_key` | 顶层 | string | Claude Code 认证凭据 |
| `admin_token` | 顶层 | string | Web 管理界面登录凭据 |
| `worker_threads` | 顶层 | number | 工作线程数，范围 1-8 |
| `active_model` | 顶层 | string | 默认模型 ID |
| `models[].id` | 模型 | string | Claude Code 侧使用的模型名（`--model` 参数） |
| `models[].provider` | 模型 | string | 厂商名，仅用于日志/标识 |
| `models[].display_name` | 模型 | string | Web UI 展示名称 |
| `models[].enabled` | 模型 | bool | 是否启用，默认 `true` |
| `models[].base_url` | 模型 | string | OpenAI 兼容接口地址 |
| `models[].endpoint` | 模型 | string | 完整接口地址；非空时优先于 `base_url` |
| `models[].api_key` | 模型 | string | 上游 API Key |
| `models[].upstream_model` | 模型 | string | 上游真实模型名 |
| `models[].params` | 模型 | object | temperature/top_p/max_tokens 等 |
| `models[].extra_body` | 模型 | object | 厂商私有参数（如 `enable_search`） |

## 转换规则摘要

### Anthropic -> OpenAI

- 顶层 `system` -> OpenAI `messages[0].role = system`
- `messages[].role=user/assistant` -> OpenAI 同名角色
- Anthropic `content: "text"` -> OpenAI `content: "text"`
- Anthropic text/image 内容块 -> OpenAI content parts
- Anthropic `tools[].input_schema` -> OpenAI `tools[].function.parameters`
- Anthropic `tool_choice` -> OpenAI `tool_choice`
- Anthropic `max_tokens` -> OpenAI `max_tokens`
- Anthropic `stop_sequences` -> OpenAI `stop`
- Anthropic `stream` -> OpenAI `stream`

### OpenAI -> Anthropic

- OpenAI `choices[0].message.content` -> Anthropic `content[{type:text}]`
- OpenAI `tool_calls` -> Anthropic `content[{type:tool_use}]`
- OpenAI `finish_reason=stop` -> `stop_reason=end_turn`
- OpenAI `finish_reason=length` -> `stop_reason=max_tokens`
- OpenAI `finish_reason=tool_calls` -> `stop_reason=tool_use`
- OpenAI `usage.prompt_tokens/completion_tokens` -> Anthropic `usage.input_tokens/output_tokens`

## 生产部署建议

- 不要把 Web 管理界面直接暴露到公网；放在内网或反向代理后面。
- `admin_token` 和 `gateway_api_key` 使用高强度随机值。
- 配置文件权限建议 `chmod 600 config/gateway.local.json`。
- 在反向代理层启用 TLS、访问日志、限流、IP 白名单。
- 如需团队使用，建议把 API Key 加密存储，或改接 Vault/KMS。
- 如需审计，建议只记录请求元信息，不落盘完整 prompt/code。
- 对工具调用参数必须在实际执行工具前进行业务级校验。
- 生产环境建议关闭 `realtime_print` 以避免日志重复输出。

## Docker

```bash
docker build -t cc-oai-gateway:latest .
docker run --rm -p 8080:8080 \
  -v "$PWD/config/gateway.local.json:/app/config/gateway.json" \
  cc-oai-gateway:latest
```

使用 Docker 时，配置文件路径固定在容器内 `/app/config/gateway.json`，挂载时无需修改模板，直接挂载你的 `gateway.local.json` 即可。

## 已知限制

- 当前只实现 OpenAI-compatible Chat Completions，不实现 OpenAI Responses API。
- `count_tokens` 是近似估算（按字节/4），非真实 tokenizer。
- 不同厂商对 `stream_options.include_usage`、`max_tokens` 上限、工具调用格式的支持程度不同；可通过 `extra_body` 或修改转换逻辑适配。
- Claude Code 的某些 beta header（如 `anthropic-beta`）会被接收但不会透传到 OpenAI 兼容上游。
- 工具调用在流式模式下依赖 SSE chunk 拼接，部分厂商的 tool_calls chunk 格式可能不标准，需逐个适配。
- 使用 `params.max_tokens` 字段传递给上游，部分厂商可能使用不同字段名（如 `max_tokens` vs `max_new_tokens`）。
