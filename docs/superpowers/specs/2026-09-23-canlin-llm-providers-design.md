# CanLin — LLM providers: dialects, presets and custom endpoints

Status: draft for review. Implementation spec for the LLM slice of the
[LLM/UI design](2026-09-22-canlin-llm-ui-design.md) §2–§4 and §8. It fixes how a user
picks a provider, which wire protocols CanLin speaks, the normalised stream a module
sees, and the limits. Conventions are those of the
[core API design](2026-09-23-canlin-core-api-design.md).

## 1. Model: dialect, preset, profile

The structure follows Langertha (`~/dev/langertha`): a few base engines implement a wire
protocol; most named providers are that base with a URL, a default model and a key
source.

- **Dialect** — a trusted adapter for one wire protocol: builds the request, parses the
  streamed response, maps it to the normalised events of §4. Code, in the core.
- **Preset** — pure data: `{id, title, dialect, base_url, auth, default_model,
  capabilities}`. Adding a provider that speaks an existing dialect is a data change,
  never new code.
- **Profile** — what the user configured in trusted settings: a preset (or `custom`),
  optionally overridden base URL and model, a credential handle, options. Modules and
  the assistant refer to profiles by id; they never see URLs or keys.

**Custom** is a profile without a preset: the user picks a dialect, enters base URL,
model and (optional) key, and chooses the auth scheme the dialect allows. It means "this
dialect at another endpoint", never "guess the protocol" (LLM/UI §2).

## 2. Dialects

### 2.1 v1

| dialect | request | stream | covers (via presets) |
|---|---|---|---|
| `openai-chat` | `POST {base}/chat/completions`, `stream: true`, tools as `tools[].function` | SSE `data:` JSON chunks, `data: [DONE]`; `choices[0].delta.content`, `delta.tool_calls[i]` (`id`, `function.name`, `function.arguments` fragments), `finish_reason` | OpenAI, Groq, Mistral, DeepSeek, OpenRouter, xAI, Cerebras, Ollama (`/v1`), LM Studio, vLLM, llama.cpp, SGLang, most gateways |
| `anthropic-messages` | `POST {base}/v1/messages`, `stream: true`, header `anthropic-version` | SSE events `message_start`, `content_block_start/delta/stop` (`text_delta`, `input_json_delta`), `message_delta` (`stop_reason`), `message_stop`, `error` | Anthropic, MiniMax, Moonshot, LM Studio (Anthropic mode), Anthropic-compatible gateways |

`openai-chat` is implemented first; `anthropic-messages` second. Both are C adapters
built on `canlin_sse` and `canlin_json`: parsing stays bounded, and credentials and
request bodies never pass through module Lua.

### 2.2 Later

`gemini` (generateContent streaming), `openai-responses` (Responses API), `ollama`
(native `/api/chat`, NDJSON) — each a separate ticket with its own fixtures. Until a
dialect exists, presets that need it are listed as unavailable, not silently mapped to
another dialect.

### 2.3 Semantic completion

A stream is successful only on the dialect's terminal marker: `openai-chat` a
`finish_reason` followed by `[DONE]`; `anthropic-messages` `message_stop` after a
`message_delta` with `stop_reason`. EOF without it, an SSE `error`/error JSON, an
incomplete SSE record or a JSON error is a failure; text already delivered stays and is
marked partial by the assistant (LLM/UI §4). Tool calls are delivered only when their
block is complete (`openai-chat`: at `finish_reason = "tool_calls"`; `anthropic-messages`:
at `content_block_stop`) and their arguments parse as one complete JSON object. A tool
call is **acted on only after `done` with `ok = true`**; if the stream fails after a tool
event, the assistant discards that call (decided 2026-09-23, canlin#38).

**Length stop with an unfinished tool call** (decided 2026-09-23, owner): when the
provider ends on its length stop (`openai-chat` `finish_reason = "length"`,
`anthropic-messages` `stop_reason = "max_tokens"`) while a tool call is still
incomplete, that call is dropped, never delivered. The stream still succeeds with
`stop = "length"`; the text delivered so far stays, and `result.dropped_tools` counts the
dropped calls so the assistant can say the answer was cut off. Complete tool calls
delivered before the length stop are delivered as usual. Anything else incomplete at the
end (EOF, error, other stop reasons) stays a failure.

## 3. Presets

Shipped as data in the binary (a C table now; a `providers.bee` pack once libbee is
consumed, canlin#2), with a local override in `system/local/` for adding or correcting
presets without a release.

| field | meaning |
|---|---|
| `id`, `title` | `"groq"`, `"Groq"` |
| `dialect` | one of §2 |
| `base_url` | full base including any version path, e.g. `https://api.groq.com/openai/v1`; the dialect appends only its fixed suffix |
| `auth` | `bearer` (`Authorization: Bearer`), `x-api-key` (+ `anthropic-version`), or `none` |
| `key` | `required`, `optional` or `none` |
| `default_model` | a model id, or empty = the user must choose |
| `capabilities` | `tools`, `streaming` as configured facts for this preset; the connection test confirms them, nothing is inferred from model names |
| `local` | true for localhost presets (Ollama, LM Studio, vLLM, llama.cpp): plain HTTP to loopback allowed, no key required |
| `stream_usage` | `openai-chat` only: send `stream_options.include_usage`; set only where the provider documents it (v1: openai), never for custom profiles (canlin#51) |

v1 list: openai, anthropic, groq, mistral, deepseek, openrouter, xai, cerebras, minimax,
moonshot (both `anthropic-messages`), minimax-openai, moonshot-openai (their
`openai-chat` endpoints), ollama, lmstudio, lmstudio-anthropic, vllm, llamacpp.
`lmstudio-anthropic` (decided 2026-09-23, owner) is LM Studio's Anthropic mode on
loopback: `anthropic-messages`, `x-api-key`, key `optional`, and without a user key the preset
sends the fixed dummy key `lmstudio` (as Langertha does) because the server requires a
non-empty header. The dummy is preset data, not a credential; a key the user sets
replaces it. Exact URLs and default models are copied
from the corresponding Langertha engines at implementation time and recorded with the
Langertha version they came from; they are data and can be updated without code.

## 4. Lua surface

`canlin.llm` is present only for modules granted the `llm` capability (module roles
spec §3); for all others it is absent (`nil`), not a stub. Until grants exist, a trusted
loader flag on the built-in assistant stands in for the grant (canlin#38).

```lua
local profiles = canlin.llm.profiles()        -- { {id=, title=, model=, tools=true}, ... } no URLs, no keys
local h, code, msg = canlin.llm.request(profile_id, {
  messages = {                                 -- normalised conversation
    { role = "system", text = "..." },
    { role = "user", text = "..." },
    { role = "assistant", text = "...", tool_calls = { {id=, name=, args="{...}"} } },
    { role = "tool", tool_call_id = "...", text = "{...}" },
  },
  tools = { "set_view" },                      -- names of core-provided tools only
  max_tokens = 1024,                           -- optional, bounded
})
canlin.cancel(h)
```

Events for the request handle (core API §5.2):

| event | payload |
|---|---|
| `"llm"` | `{handle, kind = "text", text}` — a UTF-8 fragment (validated, D5) |
| `"llm"` | `{handle, kind = "tool", id, name, args}` — one complete tool call, `args` a JSON string |
| `"llm"` | `{handle, kind = "usage", input, output}` — if the provider reports it |
| `"done"` | `{handle, ok = true, result = {stop = "end"\|"tool"\|"length", dropped_tools = n}}` (`dropped_tools` only with `length`, §2.3) or `{handle, ok = false, code, message}` (the generic shape of core API §5.2) |

`tools` names core-provided tools; their JSON schemas come from the core (for `set_view`
the UI spec §7 schema), so a module cannot describe arbitrary tools to the provider in
v1. The dialect serialises the normalised messages into its own format and keeps any
provider-specific continuation data it needs (LLM/UI §4) with the request.

Error codes follow core API §3.1, refined through `message` only: bad credentials and
permission → `denied`; unknown model, missing tool support → `unsupported`; network,
TLS, timeout, provider overload → `unavailable`; limits → `full`; malformed stream →
`unavailable` with a message naming the protocol error.

## 5. Credentials and endpoints

- A credential handle is bound to one profile, its base URL origin and auth scheme
  (LLM/UI §3). The transport injects the header for that origin only; redirects to
  another origin drop it. Lua, logs, crash output and conversation history never see it.
- HTTPS is required, except for `local` presets/custom endpoints on loopback, and an
  explicitly enabled local-development setting.
- v1 stores keys session-only; persistent storage follows the credential-storage spec
  per platform. The browser build defaults to session-only (LLM/UI §3).

## 6. Connection test

Trusted settings can test a profile: `openai-chat` `GET {base}/models`,
`anthropic-messages` `GET {base}/v1/models`, then one minimal streamed request with a
tool if `tools` is claimed. Results distinguish: bad key (401/403), model missing (404 or
not listed), no tool support (provider error or tool not called on the probe), network /
TLS / DNS, browser CORS (browser build only), and success. No fallback to another
endpoint or dialect.

## 7. Limits

In `include/canlin/llm_limits.h`, initial values:

| constant | value |
|---|---|
| `CANLIN_LLM_SSE_LINE_MAX` | 64 KiB |
| `CANLIN_LLM_SSE_DATA_MAX` | 256 KiB |
| `CANLIN_LLM_JSON_DEPTH` | 32 |
| `CANLIN_LLM_JSON_STRING_MAX` | 64 KiB |
| `CANLIN_LLM_TOOL_ARGS_MAX` | 64 KiB (= `CANLIN_UI_TOOL_MAX_BYTES`) |
| `CANLIN_LLM_TOOL_CALLS_MAX` | 8 per response |
| `CANLIN_LLM_TEXT_MAX` | 256 KiB per response |
| `CANLIN_LLM_REQUEST_MAX` | 512 KiB serialised request body |
| `CANLIN_LLM_INFLIGHT_MAX` | 2 per module |
| `CANLIN_LLM_TOOL_ROUNDS_MAX` | 4 automatic tool rounds per user turn (assistant) |
| `CANLIN_LLM_IDLE_TIMEOUT_MS` | 60 000 without a byte |

## 8. Transport

The dialects talk to a transport interface: open a streaming POST/GET with headers and
body, receive status, headers and body chunks as scheduler events, close/cancel. Three
implementations: a **fixture transport** for tests (scripted responses, arbitrary chunk
splits, faults), the **native** HTTP/1.1 client over nonblocking sockets and mbedtls
([HTTP transport design](2026-09-23-canlin-http-transport-design.md), canlin#41), and the
**browser** Fetch/Streams adapter. Dialects are tested completely against the fixture
transport before any network exists.

## 9. Tests

No API key, no real provider (LLM/UI §8). Recorded-style fixtures per dialect, fed at
arbitrary splits: plain text stream; text + tool call with arguments split across many
chunks (`openai-chat` index-based deltas, `anthropic-messages` `input_json_delta`); two
parallel tool calls; `length` stop; provider error event; HTTP 401/404/429/500 bodies;
EOF before terminal marker; oversize text/args/tool count; invalid UTF-8; late events
after cancel; request body serialisation of every message kind per dialect (golden
JSON); credential header present only for the bound origin and absent after a
cross-origin redirect; `canlin.llm` absent for non-assistant modules. Live provider smoke
tests stay separate and explicitly authorised.

## 10. Implementation order

1. Dialect interface, fixture transport, `openai-chat` adapter, `canlin.llm` for the
   assistant, presets/profiles table, session credentials.
2. `anthropic-messages` adapter.
3. Built-in assistant module: conversation, `set_view` tool loop with
   `CANLIN_LLM_TOOL_ROUNDS_MAX`, submit disclosure (UI spec §7).
4. Native HTTP/TLS transport (own spec), then the browser adapter with the WASM slice.
