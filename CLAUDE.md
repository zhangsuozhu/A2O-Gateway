# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# Build
make build

# Run (with local config)
./build/cc-oai-gateway ./config/gateway.local.json

# Clean
make clean

# Docker
make docker
```

### Dependencies (Debian/Ubuntu)
```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libevent-dev libcurl4-openssl-dev libcjson-dev ca-certificates
```

### Dependencies (macOS)
```bash
brew install cmake pkg-config libevent curl cjson
```

### Run with env var (alternative to CLI arg)
```bash
export GATEWAY_CONFIG="./config/gateway.local.json"
./build/cc-oai-gateway
```

### Test Commands (when gateway is running)
```bash
# Health check
curl http://127.0.0.1:8080/healthz

# Non-streaming message
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# Streaming
curl -N http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-stream.json

# Token estimation
curl http://127.0.0.1:8080/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# Run with valgrind
valgrind --leak-check=full ./build/cc-oai-gateway ./config/gateway.local.json
```

## Architecture

This is a C11 gateway that translates Anthropic Messages API ↔ OpenAI Chat Completions protocol.

### Data Flow

```
Claude Code → HTTP POST /v1/messages (Anthropic format)
                → main.c (libevent HTTP server)
                → handlers.c (route handler, auth check)
                → convert.c (Anthropic → OpenAI request conversion)
                → enqueue_job() → worker.c (libcurl multi thread pool)
                → upstream HTTP POST /v1/chat/completions (OpenAI format)
                ← response: stream.c (SSE parse + OpenAI → Anthropic conversion)
                ← handlers.c (send response to Claude Code)
```

### Source Layout

| File | Responsibility |
|------|---------------|
| `main.c` | Entry point, libevent setup, signal handling, global vars |
| `types.h` | Shared structs (membuf_t, app_config_t, stream_state_t, gateway_job, worker_t), inline utilities |
| `log.h/c` | Logging: level filtering, file output, realtime terminal print |
| `config.h/c` | JSON config load/save, masked output (API keys masked with ***MASKED***), model selection |
| `convert.h/c` | Bidirectional protocol conversion (Anthropic ↔ OpenAI), URL construction |
| `stream.h/c` | SSE streaming parser, chunk-by-chunk conversion, client response streaming |
| `worker.h/c` | libcurl multi worker thread pool, job queue, CURL easy handle lifecycle |
| `stats.h/c` | Request statistics: per-model breakdown, time windows, latency tracking |
| `handlers.h/c` | HTTP route dispatch: /v1/messages, /v1/messages/count_tokens, /healthz, /admin, etc. |
| `web/admin.html` | Single-file Web UI for config management (served at /admin) |

### Key Design Decisions

- **Threading**: libevent on main thread (HTTP accept + event dispatch), N worker threads (configurable via `worker_threads`) running libcurl multi handles. Job queue with mutex+condvar for handoff.
- **Global state**: `BASE`, `HTTP`, `WORKERS[]`, `RR` declared in `main.c` as extern in `types.h`. Config is RWLock-protected for hot-reload from Web UI.
- **Streaming**: `stream_state_t` tracks conversion state across chunks. SSE lines parsed incrementally. Multiple concurrent tool streams tracked via `tool_stream_state_t[MAX_TOOL_STREAMS]`.
- **Memory**: `membuf_t` for dynamic body accumulation. `gateway_job` holds full lifecycle of one upstream request. Jobs freed after response is sent.
- **Config hot-reload**: Web UI can update config at runtime via `PUT /admin/config`. `config_replace_from_json()` replaces root under write lock.

### Protocol Mapping

| Anthropic | OpenAI |
|-----------|--------|
| `POST /v1/messages` | `POST /v1/chat/completions` |
| `system` param | `messages[0].role=system` |
| `tools[].input_schema` | `tools[].function.parameters` |
| `tool_choice` | `tool_choice` |
| `content[type=tool_use]` | `tool_calls` |
| `stop_reason=end_turn` | `finish_reason=stop` |
| `stop_reason=max_tokens` | `finish_reason=length` |
| `stop_reason=tool_use` | `finish_reason=tool_calls` |
| `usage.input_tokens` | `usage.prompt_tokens` |
| `usage.output_tokens` | `usage.completion_tokens` |

### API Endpoints

| Method | Path | Handler | Auth |
|--------|------|---------|------|
| POST | /v1/messages | handle_messages | gateway_api_key |
| POST | /v1/messages/count_tokens | handle_count_tokens | gateway_api_key |
| GET | /v1/models | handle_models | gateway_api_key |
| GET | /admin | Web UI (single-file HTML) | session (admin_password login) |
| POST | /admin/api/login | login | admin_password |
| POST | /admin/api/logout | logout | session |
| GET | /admin/api/config | get config | session |
| PUT/POST | /admin/api/config | update config | session |
| POST | /admin/api/switch | switch default model | session |
| POST | /admin/api/change-password | change admin password | session + old_password |
| GET | /admin/api/stats | get request statistics | session |
| POST | /admin/api/stats/reset | reset statistics | session |
| GET | /healthz | handle_healthz | none |
| GET | /readyz | handle_healthz | none |

## Common Tasks

- **Add a new model route**: Add entry to `config/gateway.local.json` under `models[]`. No code change needed.
- **Add a new endpoint**: Define handler in `handlers.c`, register in `handle_root()`.
- **Modify protocol conversion**: Edit `convert.c` (request side) or `stream.c` (response/streaming side).
- **Change config schema**: Update `config.h/c` accessors and `web/admin.html` form fields.
- **Enable HTTP/2 multiplexing**: libcurl multi already handles connection reuse; confirm at build time that libcurl was built with HTTP/2 support.

## Key Configuration

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 8080,
  "worker_threads": 4,
  "gateway_api_key": "cc-local-token",
  "admin_token": "admin-local-token",
  "active_model": "qwen-coder",
  "models": [{
    "id": "qwen-coder",
    "provider": "aliyun-bailian",
    "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
    "api_key": "real-key",
    "upstream_model": "qwen3-coder-plus"
  }]
}
```

Use `config/gateway.local.json` at runtime (copy from `config/gateway.json`, never commit with real keys).