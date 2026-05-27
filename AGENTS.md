# AGENTS.md — Claude Code OpenAI-Compatible Gateway

## Build & Run

- **Build**: `make build` (CMake, produces `./build/cc-oai-gateway`)
- **Run**: `./build/cc-oai-gateway ./config/gateway.local.json`
- **Docker**: `make docker` then mount `config/gateway.local.json` as `/app/config/gateway.json`

## Project Structure

- **Single source**: `src/main.c` (~1600 lines, C11). All gateway logic lives here.
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

## Configuration

1. Copy template: `cp config/gateway.json config/gateway.local.json`
2. Edit `gateway.local.json` — set real `api_key`, `base_url`, and `active_model`
3. Run with the local config path (not `gateway.json`)

Key fields:
- `gateway_api_key` — Claude Code auth token
- `admin_token` — Web UI password
- `active_model` — default model ID when none specified
- `models[].id` — model name Claude Code uses (`--model`)
- `models[].upstream_model` — real model name sent to provider
- `models[].extra_body` — provider-specific params (e.g., `{"enable_search": true}`)

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
```

## Architecture Notes

- **libevent** HTTP server + **libcurl multi** worker threads (configurable via `worker_threads`)
- Protocol conversion: Anthropic Messages API ↔ OpenAI Chat Completions
- Supports streaming SSE, tool calls, and multi-model routing
- `count_tokens` is local estimation, not real tokenization
- Beta headers from Claude Code are accepted but not forwarded upstream

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
