# Claude Code OpenAI-Compatible Gateway 用户手册

## 目录

1. [项目简介](#1-项目简介)
2. [快速开始](#2-快速开始)
3. [配置文件详解](#3-配置文件详解)
4. [Web 管理界面](#4-web-管理界面)
5. [连接 Claude Code](#5-连接-claude-code)
6. [命令行测试](#6-命令行测试)
7. [Docker 部署](#7-docker-部署)
8. [生产部署建议](#8-生产部署建议)
9. [常见问题](#9-常见问题)

---

## 1. 项目简介

本项目是一个 **本地 API 网关**，架设在 Claude Code 和任意兼容 OpenAI 格式的模型厂商之间：

```text
Claude Code
  → Anthropic Messages API (POST /v1/messages)
  → 本网关（协议转换）
  → OpenAI Chat Completions (POST /v1/chat/completions)
  → 任意兼容 OpenAI 接口的模型厂商
```

**适用场景**：你手上有 OpenAI 兼容格式的接口（阿里百炼 compatible-mode、DeepSeek、Moonshot、SiliconFlow、Ollama/OpenWebUI/vLLM 等），但 Claude Code 只支持 Anthropic 协议。本网关在中间做协议转换，让 Claude Code 能直接使用这些模型。

### 核心能力

| 能力 | 说明 |
|------|------|
| 协议转换 | Anthropic Messages API → OpenAI Chat Completions |
| 流式支持 | SSE 流式转换（OpenAI chunk → Anthropic stream events） |
| 工具调用转换 | Anthropic tools/tool_use ↔ OpenAI tools/tool_calls |
| 多模型支持 | 可配置多个模型/厂商 |
| 模型切换 | Web 界面一键切换默认模型 |
| 多线程 | libcurl multi + worker 线程池，连接复用 |

---

## 2. 快速开始

### 2.1 安装依赖

**Debian / Ubuntu：**

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  libevent-dev libcurl4-openssl-dev libcjson-dev ca-certificates
```

**macOS：**

```bash
brew install cmake pkg-config libevent curl cjson
```

### 2.2 编译

```bash
cd /path/to/claude-code-openai-gateway
make build
```

如果遇到 `libcjson` 找不到的问题（pkg-config `.pc` 文件缺失），手动创建：

```bash
# 检查 libcjson 是否已安装
ls /usr/lib/x86_64-linux-gnu/libcjson.so  # Debian/Ubuntu

# 如果已安装但 pkg-config 找不到，创建 .pc 文件：
sudo tee /usr/lib/x86_64-linux-gnu/pkgconfig/libcjson.pc << 'EOF'
prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib/x86_64-linux-gnu
includedir=${prefix}/include/cjson
Name: libcjson
Description: Ultralightweight JSON parser in ANSI C
Version: 1.7.10
Libs: -L${libdir} -lcjson
Cflags: -I${includedir}
EOF
```

或者编译时指定 `PKG_CONFIG_PATH`：

```bash
export PKG_CONFIG_PATH=/path/to/dir/containing/libcjson.pc:$PKG_CONFIG_PATH
make build
```

### 2.3 首次运行

```bash
# 创建本地配置文件（不要直接改 gateway.json）
cp config/gateway.json config/gateway.local.json

# 编辑配置文件，填入真实 API Key
vim config/gateway.local.json

# 启动
./build/cc-oai-gateway ./config/gateway.local.json
```

启动后访问 Web 管理界面：http://127.0.0.1:8080/admin

> **提示**：如果 8080 端口被占用，修改配置文件的 `listen_port` 后重启即可。

---

## 3. 配置文件详解

配置文件是一个 JSON 文件，默认路径为 `config/gateway.json`。建议复制为 `gateway.local.json` 使用。

### 完整结构

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 8080,
  "gateway_api_key": "cc-local-token",
  "admin_token": "admin-local-token",
  "worker_threads": 4,
  "active_model": "qwen-coder",
  "models": [
    {
      "id": "qwen-coder",
      "provider": "aliyun-bailian",
      "display_name": "Qwen Coder via OpenAI Compatible",
      "interface": "openai_chat_completions",
      "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "endpoint": "",
      "api_key": "sk-xxxxxxxxxxxxxxxx",
      "upstream_model": "qwen3-coder-plus",
      "enabled": true,
      "params": {
        "temperature": 0.2,
        "top_p": 0.95
      },
      "extra_body": {
        "enable_search": true
      }
    }
  ]
}
```

### 字段说明

#### 全局字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `listen_host` | string | `"0.0.0.0"` | 监听地址。公网部署建议改为内网 IP 或 `127.0.0.1` |
| `listen_port` | number | `8080` | 监听端口 |
| `gateway_api_key` | string | — | Claude Code 连接本网关时使用的认证 Token |
| `admin_token` | string | — | Web 管理界面的登录密码 |
| `worker_threads` | number | `4` | 异步 worker 线程数 |
| `active_model` | string | — | 默认使用的模型 ID（对应 models[].id） |

#### 模型字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | ✅ | Claude Code 侧看到的模型名（`--model` 参数传入的值） |
| `provider` | string | — | 模型厂商名，仅用于标识和日志 |
| `display_name` | string | — | Web 界面展示名称 |
| `interface` | string | — | 接口类型，当前仅支持 `openai_chat_completions` |
| `base_url` | string | ✅ | OpenAI 兼容接口的基础地址，如 `https://api.deepseek.com/v1` |
| `endpoint` | string | — | 完整接口地址（可选，非空时优先于 base_url） |
| `api_key` | string | ✅ | 上游模型厂商的 API Key |
| `upstream_model` | string | ✅ | 上游真实的模型名称，如 `deepseek-chat` |
| `enabled` | boolean | — | 是否启用，默认为 `true`。禁用后该模型不可用 |
| `params` | object | — | 请求参数，见下方 |
| `extra_body` | object | — | 厂商私有扩展参数，透传到上游请求体 |

**params 支持的常用参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `temperature` | number | 采样温度 (0-2)，默认 1.0 |
| `top_p` | number | 核采样 (0-1)，默认 1.0 |
| `max_tokens` | number | 最大输出 token 数 |
| `frequency_penalty` | number | 频率惩罚 (-2 到 2) |
| `presence_penalty` | number | 存在惩罚 (-2 到 2) |
| `stop` | array / string | 停止序列 |

**extra_body 示例：**

```json
// 阿里百炼：开启联网搜索
{ "enable_search": true }

// DeepSeek：设置前缀
{ "prefix": "You are a helpful assistant" }

// 自定义参数
{ "custom_param": "value" }
```

---

## 4. Web 管理界面

启动网关后，在浏览器打开：

```
http://127.0.0.1:8080/admin
```

（如果修改了端口或地址，替换为实际值）

### 4.1 登录

输入配置文件中的 `admin_token`，点击 **加载配置**。

![登录界面](https://via.placeholder.com/800x60?text=输入+Admin+Token+后点击加载配置)

### 4.2 全局设置

登录后第一个卡片是全局设置：

- **监听地址** — 网关监听 IP
- **监听端口** — 网关端口号
- **Gateway API Key** — Claude Code 连接时使用的 token
- **Admin Token** — Web 管理界面登录密码
- **Worker 线程数** — 异步 worker 数量
- **默认模型** — 下拉选择默认使用的模型

修改后点击 **保存全部配置**（或按 `Ctrl+S`）持久化。

### 4.3 模型管理

每个模型显示为一个独立卡片，包含：

#### 基本信息

| 字段 | 说明 |
|------|------|
| **模型 ID** | Claude Code 用 `--model` 参数传入的值 |
| **模型厂商** | 仅用于标识，如 `aliyun-bailian`、`deepseek` |
| **显示名称** | Web 界面上的友好名称 |
| **状态** | 启用 / 禁用 |

#### 接口信息

| 字段 | 说明 |
|------|------|
| **Base URL** | OpenAI 兼容接口地址，例如 `https://api.deepseek.com/v1` |
| **完整 Endpoint** | 可选，填写后优先使用（如厂商有特殊路径） |
| **接口类型** | 当前固定为 `OpenAI Chat Completions` |
| **上游真实模型名** | 实际请求上游时使用的模型名 |
| **API Key** | 以 `***MASKED***` 显示，修改才写入，保存时保留原值 |

#### 参数设置

不再需要手写 JSON！常用参数有独立输入框：

| 参数 | 输入方式 | 范围 |
|------|---------|------|
| **Temperature** | 数字步进 | 0 ~ 2 |
| **Top P** | 数字步进 | 0 ~ 1 |
| **Max Tokens** | 数字输入 | ≥ 1 |
| **频率惩罚** | 数字步进 | -2 ~ 2 |
| **存在惩罚** | 数字步进 | -2 ~ 2 |
| **Stop Sequences** | 逗号分隔 | — |

#### 厂商扩展参数

`extra_body` 以 JSON 文本域输入，例如：

```
阿里百炼开启搜索: {"enable_search": true}
DeepSeek 前缀:    {"prefix": "You are a coder"}
```

#### 模型操作

每个模型卡片右上角有三个操作按钮：

| 按钮 | 说明 |
|------|------|
| **★ 设为默认** | 立即切换默认模型（绿色按钮，仅非默认模型显示） |
| **📋 复制** | 复制当前模型配置为新模型（ID 自动加 `-copy` 后缀） |
| **🗑 删除** | 删除模型（有确认弹窗） |

**快速切换模型**：直接点击非默认模型上的 **★ 设为默认**，无需手动保存。

### 4.4 底部状态栏

页面底部固定显示：

- 当前连接状态
- 模型统计（总数 / 启用数）
- 服务器地址（`http://127.0.0.1:8081`）

### 4.5 Claude Code 配置生成

页面自动生成 Claude Code 连接所需的环境变量配置。点击 **📋 复制** 即可粘贴到终端或 `.claude/settings.json`。

### 4.6 原始 JSON 编辑

点击 **📝 编辑原始 JSON** 展开 JSON 编辑器：

- **从 JSON 刷新表单** — 修改 JSON 后回填到表单
- **表单写回 JSON** — 表单修改后同步到 JSON
- 直接修改 JSON 后保存

### 4.7 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+S` / `Cmd+S` | 保存全部配置 |
| 在 Admin Token 框按 `Enter` | 加载配置 |

### 4.8 安全提示

- 不要把 Web 管理界面直接暴露到公网
- 放在内网或反向代理后面
- `admin_token` 和 `gateway_api_key` 使用高强度随机值
- 配置文件权限建议 `chmod 600 config/gateway.local.json`

---

## 5. 连接 Claude Code

### 5.1 环境变量方式

```bash
export ANTHROPIC_BASE_URL="http://127.0.0.1:8080"
export ANTHROPIC_AUTH_TOKEN="cc-local-token"
export ANTHROPIC_API_KEY="cc-local-token"
export ANTHROPIC_MODEL="qwen-coder"
export CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS=1
export CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY=1

claude --model qwen-coder
```

### 5.2 项目级配置

在项目根目录创建 `.claude/settings.json`：

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

### 5.3 环境变量说明

| 环境变量 | 说明 |
|---------|------|
| `ANTHROPIC_BASE_URL` | 网关地址，设为 `http://127.0.0.1:8080` |
| `ANTHROPIC_AUTH_TOKEN` | 等于配置文件中的 `gateway_api_key` |
| `ANTHROPIC_API_KEY` | 同上（Claude Code 某些版本用此变量） |
| `ANTHROPIC_MODEL` | 默认模型 ID，对应配置中的 models[].id |
| `CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS` | 设为 `1` 避免 beta header 问题 |
| `CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY` | 设为 `1` 启用模型发现（可选） |

---

## 6. 命令行测试

### 6.1 健康检查

```bash
curl http://127.0.0.1:8080/healthz
# → {"status":"ok"}

curl http://127.0.0.1:8080/readyz
# → {"status":"ok"}
```

### 6.2 查看可用模型

```bash
curl http://127.0.0.1:8080/v1/models \
  -H 'Authorization: Bearer cc-local-token'
```

### 6.3 发送消息（非流式）

```bash
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json
```

### 6.4 发送消息（流式）

```bash
curl -N http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-stream.json
```

### 6.5 Token 估算

```bash
curl http://127.0.0.1:8080/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json
```

> 注意：`count_tokens` 是本地近似估算，非精确分词。

### 6.6 例文件内容

`examples/anthropic-message.json`：

```json
{
  "model": "qwen-coder",
  "messages": [
    {"role": "user", "content": "用 Python 写一个快速排序"}
  ],
  "max_tokens": 500
}
```

`examples/anthropic-stream.json`：

```json
{
  "model": "qwen-coder",
  "messages": [
    {"role": "user", "content": "1+1=?"}
  ],
  "max_tokens": 200,
  "stream": true
}
```

---

## 7. Docker 部署

### 7.1 构建镜像

```bash
docker build -t cc-oai-gateway:latest .
```

### 7.2 运行

```bash
docker run --rm -p 8080:8080 \
  -v "$PWD/config/gateway.local.json:/app/config/gateway.json" \
  cc-oai-gateway:latest
```

### 7.3 Docker Compose（示例）

```yaml
# docker-compose.yml
version: '3'
services:
  gateway:
    build: .
    ports:
      - "8080:8080"
    volumes:
      - ./config/gateway.local.json:/app/config/gateway.json
    restart: unless-stopped
```

---

## 8. 生产部署建议

### 8.1 安全

- 不要把 Web 管理界面直接暴露到公网；放在内网或反向代理后面
- `admin_token` 和 `gateway_api_key` 使用高强度随机值（可用 `openssl rand -hex 32` 生成）
- 配置文件权限：`chmod 600 config/gateway.local.json`
- 在反向代理层启用 TLS、访问日志、限流、IP 白名单
- 如需团队使用，建议把 API Key 加密存储，或改接 Vault/KMS
- 如需审计，建议只记录请求元信息，不落盘完整 prompt/code
- 对工具调用参数必须在实际执行工具前进行业务级校验

### 8.2 性能

- `worker_threads` 建议设置为 CPU 核心数的 2~4 倍
- libcurl multi 会自动复用 HTTP 连接
- 如果上游延迟高，可考虑增加 worker 线程数

### 8.3 监控

- 使用 `/healthz` 和 `/readyz` 做健康检查
- 配合 Prometheus + Grafana 可采集 metrics（需自行扩展）
- 日志格式为 `[时间] 级别  消息`，可对接标准日志收集系统

---

## 9. 常见问题

### Q: 启动时提示 "bind failed"

端口被占用。解决方法：
1. 修改配置文件中的 `listen_port`
2. 或用 `lsof -i :8080` 查看占用进程并终止

### Q: 403 / 401 认证错误

- 检查请求头 `Authorization: Bearer xxx` 中的 token 是否正确
- Claude Code 用 `gateway_api_key`
- Web 管理用 `admin_token`

### Q: 模型返回 502 Bad Gateway

上游接口不可达或返回错误。排查：
1. 检查 `base_url` 和 `api_key` 是否正确
2. 用 curl 直接测试上游接口
3. 查看网关日志中的具体错误信息

### Q: 流式输出卡住或不完整

- 确认上游支持 SSE 流式输出
- 某些厂商对流式参数有特殊要求，可通过 `extra_body` 适配
- 检查网络代理设置

### Q: Web 界面修改后不生效

- 点击 **保存全部配置**（`Ctrl+S`）后配置持久化到文件
- 模型/Key/参数修改即时生效
- worker 线程数修改需要重启进程

### Q: 如何添加自定义请求参数？

两种方式：
1. 在 Web 界面的 **参数设置** 中输入 temperature、top_p 等（推荐）
2. 在 Web 界面的 **extra_body** 中填写任意 JSON，透传到上游

### Q: 支持图像输入吗？

取决于上游厂商是否支持多模态输入。网关会透传 image 内容块。

### Q: Claude Code 提示 "model not found"

- 检查请求中的 `model` 参数是否匹配配置中的 `models[].id`
- 检查该模型是否 `enabled: true`
- 检查网关启动日志是否有模型加载错误

### Q: 如何优雅关闭？

发送 `SIGINT` 或 `SIGTERM` 信号即可：

```bash
kill -TERM <pid>
# 或
pkill cc-oai-gateway
```

---

## 附录：转换规则

### Anthropic → OpenAI（请求方向）

| Anthropic 字段 | OpenAI 字段 |
|---------------|-------------|
| 顶层 `system` | `messages[0].role = system` |
| `messages[].content[].type=text` | `messages[].content` |
| `messages[].content[].type=image` | `content parts` |
| `tools[].input_schema` | `tools[].function.parameters` |
| `tool_choice` | `tool_choice` |
| `max_tokens` | `max_tokens` |
| `stop_sequences` | `stop` |
| `stream` | `stream` |

### OpenAI → Anthropic（响应方向）

| OpenAI 字段 | Anthropic 字段 |
|-------------|---------------|
| `choices[0].message.content` | `content[{type:text}]` |
| `tool_calls` | `content[{type:tool_use}]` |
| `finish_reason=stop` | `stop_reason=end_turn` |
| `finish_reason=length` | `stop_reason=max_tokens` |
| `finish_reason=tool_calls` | `stop_reason=tool_use` |
| `usage.prompt_tokens` | `usage.input_tokens` |
| `usage.completion_tokens` | `usage.output_tokens` |

---

*最后更新：2026-05-27*
