# AGENTS.md — Claude Code OpenAI-Compatible Gateway

## Build & Run

- **Build**: `make build` (CMake, produces `./build/cc-oai-gateway`)
- **Run**: `./build/cc-oai-gateway ./config/gateway.local.json`
  - Alternative: `export GATEWAY_CONFIG="./config/gateway.local.json"; ./build/cc-oai-gateway`
- **Docker**: `make docker` then mount `config/gateway.local.json` as `/app/config/gateway.json`

> `make run` uses `config/gateway.json` (the template), not `gateway.local.json`.

## Project Structure

- **Source files** (`src/`):
  - `types.h` — Shared types, constants, inline utilities
  - `log.h/c` — Logging (level filtering, file output, realtime print)
  - `config.h/c` — Config load/save/mask/model selection
  - `convert.h/c` — Anthropic↔OpenAI protocol conversion
  - `stream.h/c` — SSE streaming parse & convert
  - `worker.h/c` — libcurl multi worker thread pool
  - `handlers.h/c` — HTTP route handlers
  - `stats.h/c` — Request statistics, per-model latency/throughput tracking
  - `main.c` — Entry point, libevent setup, signal handling
- **Static web UI**: `web/admin.html` (single-file HTML/JS, served at `/admin`)
- **Examples**: `examples/anthropic-{message,stream,tool}.json` for manual curl testing
- **Config**: JSON file; `config/gateway.json` is the template, `config/gateway.local.json` is the runtime file

## Dependencies

Debian/Ubuntu:
```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libevent-dev libcurl4-openssl-dev libcjson-dev ca-certificates
```

macOS:
```bash
brew install cmake pkg-config libevent curl cjson
```

> Note: `libcjson` sometimes lacks a `.pc` file. If `pkg-config` fails, create `/usr/lib/x86_64-linux-gnu/pkgconfig/libcjson.pc` or set `PKG_CONFIG_PATH`.
> Note: `cc2open` in repo root is a stale build artifact; ignore it.

## Configuration

1. Copy template: `cp config/gateway.json config/gateway.local.json`
2. Edit `gateway.local.json` — set real `api_key`, `base_url`, and `active_model`
3. Run with the local config path (not `gateway.json`)

Key fields:
- `http_enabled` — Enable HTTP listener (default true; set false for HTTPS-only mode)
- `https_enabled` — Enable HTTPS listener (default true; requires ssl_cert_file + ssl_key_file or auto-generation)
- `gateway_api_key` — Claude Code auth token
- `admin_password` — Web UI login password (used to obtain session)
- `admin_token` — Web UI bearer token (auto-synced with `admin_password`)
- `active_model` — default model ID when none specified
- `models[].id` — model name Claude Code uses (`--model`)
- `models[].upstream_model` — real model name sent to provider
- `models[].extra_body` — provider-specific params (e.g., `{"enable_search": true}`)
- `ssl_cert_file` — PEM certificate file path (optional; enables HTTPS when set with `ssl_key_file`)
- `ssl_key_file` — PEM private key file path (optional)
- `ssl_port` — HTTPS listen port (default 8443)

## Testing

```bash
# Health
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

# Tool call
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer cc-local-token' \
  -d @examples/anthropic-tool.json
```

## Architecture Notes

- **libevent** HTTP server + **libcurl multi** worker threads (configurable via `worker_threads`)
- Protocol conversion: Anthropic Messages API ↔ OpenAI Chat Completions
- Supports streaming SSE, tool calls, and multi-model routing
- `count_tokens` is local estimation, not real tokenization
- Beta headers from Claude Code are accepted but not forwarded upstream
- See `CLAUDE.md` for detailed data flow and protocol mapping tables

## Claude Code Integration

Set env vars before running `claude`:
```bash
export ANTHROPIC_BASE_URL="http://127.0.0.1:8080"
export ANTHROPIC_AUTH_TOKEN="cc-local-token"
export ANTHROPIC_API_KEY="cc-local-token"
export ANTHROPIC_MODEL="your-model-id"
export CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS=1
```

Or use `.claude/settings.json` with `"env"` section.

## Security

- Don't expose `/admin` to public internet
- Use strong random tokens: `openssl rand -hex 32`
- Set config file permissions: `chmod 600 config/gateway.local.json`
- `gateway.local.json` is NOT gitignored; never commit real API keys
