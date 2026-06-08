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
  libevent-dev libcurl4-openssl-dev libcjson-dev libssl-dev ca-certificates

### Dependencies (macOS)
```bash
brew install cmake pkg-config libevent curl cjson openssl
```

### Run with env var (alternative to CLI arg)
```bash
export GATEWAY_CONFIG="./config/gateway.local.json"
./build/cc-oai-gateway
```

### Unit Tests

```bash
# Build and run all tests
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure

# Run individual test binaries directly
./build/test_convert
./build/test_stream
./build/test_log
./build/test_cache_control
```

Test coverage: `test_convert` tests protocol conversion (Anthropic ↔ OpenAI), `test_stream` tests SSE parsing and streaming conversion, `test_log` tests realtime print text extraction (`rt_extract_text_from_json`), `test_cache_control` tests cache policy injection (`cache_control` block generation).

Test infrastructure:
- `tests/test_stubs.c` — provides `WORKER_COUNT` as a weak symbol for unit tests that link `config.c` (which references `WORKER_COUNT` from `main.c`). Linked only by `test_cache_control`.
- `tests/mock_server.c` — a lightweight HTTP server for integration testing (listens on a dynamic port, serves predefined responses). Build target `mock_server`.

Test payloads in `examples/` include load testing (`loadtest-payload.json`) and performance testing (`perf-payload.json`) variants.

### Debug & Test Commands
```bash
# Realtime JSON print mode (set realtime_print in config to "all" or "txt")
# "all" = full JSON dump, "txt" = text-only extraction

# Health check
curl http://127.0.0.1:8081/healthz

# Non-streaming (use examples/ for test payloads)
curl http://127.0.0.1:8081/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# Streaming
curl -N http://127.0.0.1:8081/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-stream.json

# Token estimation (approximate, not real tokenizer)
curl http://127.0.0.1:8081/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-message.json

# OpenAI passthrough (monitoring only) — non-streaming
curl http://127.0.0.1:8081/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d '{"model":"qwen-coder","messages":[{"role":"user","content":"hi"}]}'

# OpenAI passthrough — streaming
curl -N http://127.0.0.1:8081/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d '{"model":"qwen-coder","stream":true,"messages":[{"role":"user","content":"count to 3"}]}'

# Memory check
valgrind --leak-check=full ./build/cc-oai-gateway ./config/gateway.local.json
```

## Architecture

This is a C11 gateway that translates Anthropic Messages API ↔ OpenAI Chat Completions protocol. 9 source files + single-file Web UI (approx. 6500+ lines total) + bundled sqlite3 amalgamation. Line counts below are approximate.

### Data Flow

```
Claude Code (Anthropic) → POST /v1/messages
                → main.c (libevent HTTP server)
                → handlers.c (route handler, auth check)
                → convert.c (Anthropic → OpenAI request conversion)
                → enqueue_job() → worker.c (libcurl multi thread pool)
                → upstream HTTP POST /v1/chat/completions (OpenAI format)
                ← response: stream.c (SSE parse + OpenAI → Anthropic conversion)
                ← handlers.c (send response to Claude Code)

OpenAI SDK / curl       → POST /v1/chat/completions
                → handle_chat_completions (handlers.c)
                → enqueue_job → worker.c → upstream /chat/completions (passthrough)
                ← upstream response forwarded as-is, usage extracted for stats only
```

Three handling modes share the same job/worker plumbing, distinguished by `gateway_job.passthrough`:

| Mode (`passthrough_mode_t`) | Client protocol | Upstream URL | Upstream protocol | Body transform | Response transform |
|---|---|---|---|---|---|
| `PT_NONE` | Anthropic `/v1/messages` | `/chat/completions` | OpenAI | Anthropic → OpenAI | OpenAI → Anthropic |
| `PT_ANTHROPIC` (model `api_mode = "passthrough"`) | Anthropic `/v1/messages` | `/v1/messages` | Anthropic | model field swap | as-is |
| `PT_OPENAI` (route `/v1/chat/completions`) | OpenAI `/v1/chat/completions` | `/chat/completions` | OpenAI | model field swap | as-is (monitoring only) |

All three modes record stats / history / cache tokens identically via `stats.c` + `db.c`.

### Source Layout

| File | Lines | Responsibility |
|------|-------|---------------|
| `types.h` | 542 | Shared structs (membuf_t, app_config_t, stream_state_t, gateway_job, worker_t), inline utilities, extern globals |
| `main.c` | 576 | Entry point, libevent setup, SSL cert generation (CA + server), signal handling, global var declarations |
| `handlers.c` | 1194 | HTTP route dispatch, auth, request lifecycle, admin API, favicon/CA cert serving |
| `stream.c` | 1151 | SSE streaming parser, chunk-by-chunk conversion, tool stream tracking |
| `convert.c` | 833 | Bidirectional protocol conversion (Anthropic ↔ OpenAI), URL construction |
| `worker.c` | 742 | libcurl multi worker thread pool, job queue, CURL easy handle lifecycle (forced HTTP/1.1) |
| `db.c` | 992 | SQLite persistence: request history, hourly/daily/model aggregate stats (WAL mode) |
| `stats.c` | 503 | Request statistics: per-model breakdown, time windows, latency tracking |
| `config.c` | 558 | JSON config load/save, masked output, RWLock-protected hot-reload |
| `log.c` | 452 | Logging: level filtering, file output, realtime terminal print |
| `db.h` | 79 | SQLite schema constants, INSERT/query function declarations, DB path/reset helpers |
| `src/admin_html_embedded.h` | 8 | Declares `EMBEDDED_ADMIN_HTML[]` / `EMBEDDED_ADMIN_HTML_LEN` symbols |
| `src/generated/admin_html.c` | ~6K | CMake-generated byte array from `web/admin.html` (xxd -i), embedded at build time |
| `web/admin.html` | ~850 | Single-file Web UI source (compiled into binary, not served from disk at runtime) |
| `src/sqlite3/sqlite3.c` | bundled | SQLite amalgamation (bundled, not part of project code) |

### Globals (declared in `main.c`, extern in `types.h`)

```c
extern struct event_base *BASE;       // libevent event base
extern struct evhttp *HTTP;           // libevent HTTP server
extern worker_t WORKERS[MAX_WORKERS]; // worker thread pool
extern int WORKER_COUNT;              // actual worker count
extern unsigned long RR;              // round-robin job dispatch counter
extern volatile sig_atomic_t STOP;    // shutdown flag
extern char *g_ca_cert_pem;           // CA certificate PEM (downloadable at /admin/ca.pem)
extern long g_ca_cert_pem_len;        // CA certificate PEM length
```

Config is held in `app_config_t *config_root;` (defined in `config.c`) and is RWLock-protected for concurrent reads from workers and exclusive writes from the admin Web UI hot-reload.

### Key Internal Patterns

- **`gateway_job` lifecycle**: Allocated per-request, holds the full lifecycle (Anthropic request body → upstream response → client response). Freed after response is fully sent.
- **SQLite persistence (`db.c`)**: All request logs and aggregate stats are persisted to SQLite (WAL mode). Tables: `requests`, `hourly_stats`, `daily_stats`, `model_stats`. Each tracks `input_tokens`, `output_tokens`, `cache_read_input_tokens`, `cache_creation_input_tokens`.
- **Cache control (`cache_policy`)**: When set to `"auto"`, `convert_inject_system_cache()` in `convert.c` wraps system prompts with `cache_control: {type: "ephemeral"}` if text length exceeds `min_cache_tokens`.
- **`prompt_tokens_includes_cache`**: Model-level boolean (default `true`). Controls whether `prompt_tokens` returned by the upstream already includes cached tokens. `false` is required for Moonshot, where `prompt_tokens` excludes cache and the gateway must merge `pt + cache_read + cache_creation` for accurate stats.
- **`membuf_t`**: Dynamic buffer (ptr + len + capacity) used for accumulating request/response bodies. Every heap-allocated buffer in the system uses this.
- **`stream_state_t`**: Tracks SSE parsing state across chunks. For tool calls in streaming mode, `tool_stream_state_t[MAX_TOOL_STREAMS]` tracks partial delta accumulations.
- **Threading model**: `handle_messages()` → `enqueue_job()` (wakes a worker via condvar). Workers run `libcurl multi` event loops. When a worker completes an upstream response, it calls `send_response_cb()` back in the main thread's event loop.

### Protocol Mapping

| Anthropic | OpenAI |
|-----------|--------|
| `POST /v1/messages` | `POST /v1/chat/completions` |
| `system` param | `messages[0].role=system` |
| `tools[].input_schema` | `tools[].function.parameters` |
| `tool_choice` | `tool_choice` |
| `content[type=tool_use]` | `tool_calls` |
| `content[type=thinking]` | `reasoning_content` (extra field) |
| `stop_reason=end_turn` | `finish_reason=stop` |
| `stop_reason=max_tokens` | `finish_reason=length` |
| `stop_reason=tool_use` | `finish_reason=tool_calls` |
| `usage.input_tokens` | `usage.prompt_tokens` |
| `usage.output_tokens` | `usage.completion_tokens` |

## Thread Safety Model

| Data | Protection | Details |
|------|-----------|---------|
| `config_root` | `pthread_rwlock_t` in `app_config_t` | Multiple concurrent readers; exclusive write during hot-reload. Workers read config per-request, Web UI admin writes config. |
| Worker pending queues | Per-worker `pthread_mutex_t` + `pthread_cond_t` | `enqueue_job()` locks, inserts, signals. Worker thread locks, dequeues, processes. |
| `job->send_buf` | Per-job `send_mu` | Worker thread writes streaming data, main thread's `deferred_send` reads. Both lock. |
| Stats counters | `pthread_mutex_t` in `stats.c` | All stats mutations lock; reads lock. |
| `RR`, `STOP` | No lock needed | `RR` is `unsigned long` (atomic on x86_64), `STOP` is `volatile sig_atomic_t`. |

## Memory Management

- **`membuf_t`**: Universal dynamic buffer (exponential growth, initial 8KB, double each time). Every heap buffer in the system uses this. Always `membuf_init` then `membuf_free`.
- **`gateway_job` lifecycle**: Allocated in `handlers.c` → `enqueue_job() → worker → curl_write_cb/stream_finish → deferred_send()` frees job after response fully sent. Never freed manually.
- **cJSON ownership**: Objects from `config_select_model_copy()` must be `cJSON_Delete()`'d by caller. Objects from `config_root` must NOT be freed (read-lock-protected shared tree).
- **Helper**: `xstrdup()` is a NULL-safe strdup defined in convert.c, used throughout.
- **Error strings**: Functions like `config_replace_from_json(body, &err)` allocate error strings that the caller must `free()`.

## Resource Embedding

The gateway embeds several resources into the binary at build time:

- **Admin UI** (`web/admin.html`): CMake runs `xxd -i` to generate `src/generated/admin_html.c`, declaring `EMBEDDED_ADMIN_HTML[]` / `EMBEDDED_ADMIN_HTML_LEN`. The admin UI is served directly from memory — no `web/` directory needed at runtime, and `install(DIRECTORY web ...)` has been removed from CMake. If you modify `web/admin.html`, rebuild to regenerate the embedded C array; the `add_custom_command` in `CMakeLists.txt` handles this automatically when the HTML source is newer than the generated C file.
- **Favicon** (`src/generated/favicon.c`): A small `favicon.png` embedded via `xxd -i`, served at `/favicon.ico`.
- **CA certificate** (`src/generated/ca_embedded.h`): Generated once by `scripts/gen-ca.sh` on first CMake configure. This CA is used to auto-sign the server SSL certificate. The PEM text is also exposed at `/admin/ca.pem` for client download.

### Embedded Files

| Path | Source | Build step |
|------|--------|------------|
| `src/generated/admin_html.c` | `web/admin.html` | `add_custom_command` (xxd -i) |
| `src/generated/favicon.c` | `src/favicon_embedded.h` (binary) | `add_custom_command` (xxd -i) |
| `src/generated/ca_embedded.h` | `scripts/gen-ca.sh` | `execute_process` on first configure |

If you modify any of the source files, rebuild to regenerate the embedded arrays.

## Config Hot-Reload Mechanics

The `config_replace_from_json()` function in `config.c` handles API key preservation during hot-reload via the Web UI:
1. Parse new config JSON
2. For each model, if `api_key === "***MASKED***"` (the `MASKED_KEY` sentinel), copy the old config's real key for that model
3. Write to temp file, atomically rename to config path
4. Acquire write lock, swap `config_root`, release lock, free old tree
5. Propagate side effects: `WORKER_COUNT`, `log_level`, `realtime_print`

## Thinking Block Conversion (reasoning_content → thinking)

OpenAI models like DeepSeek-R1 return `choices[0].delta.reasoning_content` in streaming chunks. The gateway converts these to Anthropic `thinking` content blocks:
- **Streaming**: `reasoning_content` → `delta.type=thinking` with `thinking` field in SSE `content_block_delta` events. A `content_block_start` (type=thinking) is emitted before the first delta.
- **Non-streaming**: `choices[0].message.reasoning_content` → `content[{type: thinking, thinking: ...}]` block inserted before the text block.
- **`thinking_pending`** buffer accumulates partial thinking content with the same flush threshold as text deltas.

## Security

- **`config/gateway.local.json` is in `.gitignore`** — real keys should never be committed. Copy from `config/gateway.json`.
- API keys in Web UI are masked as `***MASKED***`. The real key is preserved server-side during hot-reload.
- Never expose `/admin` to the public internet. Use a reverse proxy with IP whitelisting for production.
- **Config file permissions**: `chmod 600 config/gateway.local.json`.
- `admin_token` in config is auto-synced with `admin_password` on login — changing the password invalidates all existing sessions.
- Session tokens are generated on login (`POST /admin/api/login`) and verified on each subsequent admin API call. Sessions can be destroyed via logout (`POST /admin/api/logout`).

## Common Tasks

- **Add a new model route**: Add entry to `config/gateway.local.json` under `models[]`. No code change needed.
- **Add a new HTTP endpoint**: Define handler in `handlers.c`, register in `handle_root()` (~line 1166).
- **Use a model from an OpenAI client**: Point the client at `http://<gateway>/v1/chat/completions` — the route is OpenAI passthrough by design, independent of the model's `api_mode`. Any model with a valid `base_url`/`api_key` is reachable both via `/v1/messages` (with protocol conversion or Anthropic passthrough depending on `api_mode`) and via `/v1/chat/completions` (raw OpenAI passthrough).
- **Modify protocol conversion**: Edit `convert.c` (request/build side) or `stream.c` (response/streaming side).
- **Change config schema**: Update `config.h/c` accessors (`config_get_*` / `config_set_*`) and `web/admin.html` form fields.
- **Add vendor-specific extra params**: Use `extra_body` in model config (e.g., `{"enable_search": true}`). No code change needed for simple key-value extras.
- **Enable HTTP/2 multiplexing**: Not applicable — the gateway forces `CURL_HTTP_VERSION_1_1` to avoid framing errors with upstreams like DeepSeek.
### Route Registration Pattern

In `handlers.c`, `handle_root()` (~line 1166) uses manual URI dispatch. New endpoints follow the pattern:
1. Create handler function with signature `void handler(struct evhttp_request *req, void *arg)`
2. Register in `handle_root()` with `URI_IS("/path")` matching inside the catch-all handler

Recent additions: `/favicon.ico` returns an embedded PNG; `/admin/ca.pem` returns the CA certificate PEM for HTTPS client configuration.

## Key Configuration

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 8081,
  "worker_threads": 4,
  "gateway_api_key": "cc-local-token",
  "admin_password": "your-admin-password",
  "active_model": "qwen-coder",
  "realtime_print": "false",
  "models": [{
    "id": "qwen-coder",
    "provider": "aliyun-bailian",
    "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
    "api_key": "sk-xxx",
    "upstream_model": "qwen3-coder-plus",
    "params": { "temperature": 0.2, "max_tokens": 4096 },
    "extra_body": { "enable_search": true }
  }]
}
```

Use `config/gateway.local.json` at runtime (copy from `config/gateway.json`, never commit with real keys). Config hot-reload via admin Web UI -> `PUT /admin/api/config` -> `config_replace_from_json()` swaps the config root under write lock.

### Model Config Fields

| Field | Required | Description |
|-------|----------|-------------|
| `id` | yes | Model name visible to Claude Code (`--model` flag) |
| `provider` | yes | Vendor label (logging/stats only) |
| `base_url` | yes* | OpenAI-compatible base URL (`/chat/completions` appended) |
| `endpoint` | no | Full URL override; takes priority over `base_url` |
| `api_key` | yes | Upstream API key |
| `upstream_model` | yes | Real model name sent to upstream |
| `params` | no | temperature/top_p/max_tokens/etc |
| `extra_body` | no | Vendor-specific JSON merged into request body |
| `cache_policy` | no | `"off"` or `"auto"` — auto injects `cache_control: {type: "ephemeral"}` for system prompts above `min_cache_tokens` |
| `min_cache_tokens` | no | Threshold (tokens) for auto cache injection, default 1024 |
| `prompt_tokens_includes_cache` | no | `true` (default) = `prompt_tokens` already includes cache hit/creation (OpenAI/Anthropic/DeepSeek). `false` = cache is separate from `prompt_tokens` (Moonshot); gateway will merge `pt + cr + cc` for stats |
| `api_mode` | no | `"openai_chat_completions"` (default) = Anthropic→OpenAI conversion. `"passthrough"` = skip conversion, forward Anthropic body to `{base_url}/v1/messages` |
| `interface` | no | Legacy alias for `api_mode`. Both accepted, `api_mode` takes priority |
| `priority` | no | Numeric priority (0-1000, default 100). Lower = higher priority for model selection |
| `enabled` | no | Boolean, default `true`. Set `false` to disable a model without removing it from config |

### Real-time Print Modes

| `realtime_print` | Effect |
|---|---|
| `"false"` | Off (default) |
| `"all"` | Print full JSON of each request/response to terminal |
| `"txt"` | Extract and print only conversation text content |

## Test Payloads

The `examples/` directory contains Anthropic-format test payloads:
- `anthropic-message.json` — basic conversation
- `anthropic-stream.json` — streaming request
- `anthropic-tool.json` — tool/function calling request

### CLI Arguments
```bash
./build/cc-oai-gateway [OPTIONS]
  -f, --file PATH      Config file path (default: ./config/gateway.json)
  -p, --port PORT      Listen port (default: 8081)
  -P, --password PASS  Admin web UI password (default: empty)
  -w, --workers NUM    Worker threads (default: 4, max: 8)
  -c, --cert-ip IP     Extra SAN IP for server cert (default: 127.0.0.1)
  -d, --daemon         Run as background daemon
  -h, --help           Show help
```

CLI flags override config file values for port, password, and workers. The config path can also be set via `GATEWAY_CONFIG` env var.
