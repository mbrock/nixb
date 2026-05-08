# LLM Agent Client Reference

This note distills the useful LLM and agent-client patterns from the Elixir
project at `/home/mbrock/froth`. It is meant as a practical reference for
building a multi-provider streaming LLM client in this repo.

## Current Provider Bias

Prefer the first-party modern APIs where possible:

- OpenAI's newer Responses-style API.
- Gemini's native API.
- Anthropic's Messages API.

xAI support can exist, but treat it as secondary. It is useful enough to keep in
mind, especially for built-in search tools, but it should not drive the core
client architecture.

## Useful Froth Sources

- `/home/mbrock/froth/lib/froth/llm.ex`: public LLM facade, provider resolution,
  streaming orchestration, API-key lookup, telemetry hooks.
- `/home/mbrock/froth/lib/froth/llm/request.ex`: provider-neutral request shape.
- `/home/mbrock/froth/lib/froth/llm/provider.ex`: provider behavior contract.
- `/home/mbrock/froth/lib/froth/llm/transport/sse.ex`: HTTP POST plus SSE
  streaming transport.
- `/home/mbrock/froth/lib/froth/llm/edit.ex`: normalized streamed mutation type.
- `/home/mbrock/froth/lib/froth/llm/store.ex`: incremental state store that
  applies edits.
- `/home/mbrock/froth/lib/froth/llm/providers/anthropic.ex`: Anthropic Messages
  request and stream decoder.
- `/home/mbrock/froth/lib/froth/llm/providers/openai_compat.ex`: OpenAI-compatible
  Chat Completions request and stream decoder, reused by OpenAI, Gemini, and
  xAI chat mode.
- `/home/mbrock/froth/lib/froth/llm/providers/xai_responses.ex`: xAI Responses
  request and stream decoder for native built-in tools.
- `/home/mbrock/froth/lib/froth/openai.ex`, `/home/mbrock/froth/lib/froth/gemini.ex`,
  `/home/mbrock/froth/lib/froth/grok.ex`, `/home/mbrock/froth/lib/froth/anthropic.ex`:
  user-facing provider wrappers.
- `/home/mbrock/froth/lib/froth/agent.ex` and
  `/home/mbrock/froth/lib/froth/agent/worker.ex`: persisted agent loop and tool
  execution.

## Shape Worth Copying

Froth separates the problem into four layers:

1. Public API: `stream_single(messages, on_event, opts)` and
   `stream(request, on_event, opts)`.
2. Provider adapters: turn a neutral request into one provider HTTP request,
   then turn provider-specific stream payloads into neutral edits.
3. Transport: POST JSON, read `text/event-stream`, decode each `data:` payload
   as JSON, halt on `[DONE]` or provider stop.
4. Agent loop: store conversation messages, ask the LLM, persist the assistant
   message, execute any tool uses, append tool results, repeat.

The key design choice is the edit-log middle layer. Provider decoders do not
mutate final result structs directly and do not expose provider-specific chunks
to application code. Each provider emits normalized `Edit` values, the shared
`Store` applies them, and `project_event(edit)` emits UI/runtime events.

This normalized representation is useful for UI updates, provider-independent
tool loops, and tests. It should not be the only data we keep. Save the full raw
requests, raw SSE records, raw decoded provider payloads, final raw responses,
HTTP status, headers, timing, and errors in DuckDB without rewriting or
normalizing them. The normalized view is a convenience layer; the actual
objective provider data is the foundation for debugging, replay, audits,
migration work, and comparing provider behavior later.

## Neutral Request

Froth's neutral request has this shape:

```text
provider          module/provider adapter
messages          provider-neutral API message list
model             model id
system            optional system prompt
max_tokens        optional output token cap
tools             neutral tool schemas
thinking          provider reasoning/thinking config
output_config     extra output config
cache_control     prompt caching config
headers           HTTP headers already resolved by wrapper
endpoint          provider endpoint for compatible APIs
provider_options  map for provider-specific options
parent_id         tracing/span parent
```

This is a good boundary. The wrapper resolves credentials, endpoint, model
defaults, and provider-specific options. The provider adapter only receives a
complete request and builds wire JSON.

## Provider Contract

Each provider implements:

```text
build_request(request) -> {url, headers, body}
decode_payload(json_payload, store) -> {edits, done}
finalize(store) -> final_result
project_event(edit) -> public_stream_event | nil
```

Final results use one neutral shape:

```text
text          concatenated visible text
content       neutral content blocks
stop_reason   provider stop reason normalized enough for callers
usage         token/usage map
model         model id when available
message_id    provider message id when available
```

Public stream events are tuples in Froth, but the useful neutral event names are:

```text
message_start
text_delta
thinking_start
thinking_delta
thinking_stop
tool_use_start
tool_use_delta
tool_use_stop
usage
```

For a C++ client, these should likely become a tagged event type with payload
structs rather than untyped maps.

## Edit Model

Froth's edit type is:

```text
op        open | set | append | merge | delete | close
resource  path identifying the object being edited
path      path inside that object
value     value for set/append/merge
attrs     extra object attributes, especially for open/close events
raw       original provider payload for debugging/telemetry
```

Examples:

```text
append ["message"], ["text"], "Hel"
append ["message"], ["text"], "lo"
open   ["message", "tool_calls", 0], attrs={id, name}
append ["message", "tool_calls", 0], ["arguments_json"], "{\"x\":"
append ["message", "tool_calls", 0], ["arguments_json"], "1}"
close  ["message", "tool_calls", 0], attrs={id, name, input={x: 1}}
```

The store applies edits into a JSON-like document. Important behavior:

- `open` creates or deep-merges a resource with attrs.
- `set` writes at `resource + path`.
- `append` string-concats at `resource + path`.
- `merge` deep-merges maps.
- `delete` removes a path.
- `close` marks the resource closed and can merge final attrs.

This keeps each provider decoder small and makes stream replay/testing simple.

## SSE Transport

The transport sends:

```text
POST url
headers + Content-Type: application/json
body: provider request JSON
Accept: text/event-stream where provider wrapper wants it
```

On a 2xx response:

- Iterate the SSE body.
- Ignore non-message items.
- For each SSE message:
  - If `data == "[DONE]"`, halt successfully.
  - If `data` decodes to a JSON object, call the provider payload callback.
  - Ignore malformed JSON payloads in the success stream.
- The provider callback returns `continue` or `halt` with updated store.

On non-2xx:

- Consume the body.
- Try to JSON-decode it.
- Return `http_error(status, decoded_or_text)`.

Timeout defaults are around 60 seconds in Froth. The transport should take a
configurable receive timeout and injected HTTP client/Finch equivalent for tests.

## Provider Details

### Anthropic Messages

Endpoint:

```text
https://api.anthropic.com/v1/messages
```

Headers:

```text
x-api-key: ...
anthropic-version: 2023-06-01
anthropic-beta: context-1m-2025-08-07
accept: text/event-stream
content-type: application/json
```

Request body:

```json
{
  "model": "...",
  "max_tokens": 16384,
  "messages": [],
  "stream": true,
  "system": "...",
  "thinking": {"type": "enabled", "budget_tokens": 1024},
  "output_config": {},
  "tools": [],
  "cache_control": {"type": "ephemeral"}
}
```

Relevant stream payloads:

- `message_start`: open message, set id/model/usage.
- `message_delta`: set stop reason, merge usage.
- `content_block_start` with `text`: open text block at index.
- `content_block_start` with `thinking`: open thinking block with internal
  buffers.
- `content_block_start` with `tool_use`: open tool-use block with internal JSON
  argument buffer.
- `content_block_delta` with text delta: append to block text.
- `content_block_delta` with thinking delta/signature delta: append to internal
  buffers.
- `content_block_delta` with input JSON delta: append to `__input_json_buf`.
- `content_block_stop`: finalize block. Tool-use blocks decode buffered JSON
  into `input`, delete internal buffers, and close the block.
- `message_stop`: done.
- `error`: store error and halt.

Anthropic final content is ordered blocks from `message.blocks`, with internal
buffer keys removed. Text result is the concatenation of text blocks.

### OpenAI-Compatible Chat Completions

Used by OpenAI, Gemini's OpenAI-compatible endpoint, and xAI chat mode.

Endpoints used in Froth:

```text
OpenAI: https://api.openai.com/v1/chat/completions
Gemini: https://generativelanguage.googleapis.com/v1beta/openai/chat/completions
xAI chat: https://api.x.ai/v1/chat/completions
```

Headers:

```text
authorization: Bearer ...
content-type: application/json
```

Request body:

```json
{
  "model": "...",
  "messages": [],
  "stream": true,
  "max_tokens": 16384,
  "tools": [],
  "stream_options": {"include_usage": true},
  "reasoning_effort": "..."
}
```

Message conversion:

- Neutral `system` becomes a leading `{role: "system", content: system}`.
- User string content passes through as `{role: "user", content: text}`.
- User tool results become OpenAI tool messages:
  `{role: "tool", tool_call_id: id, content: text}`.
- Assistant neutral content blocks become one assistant message:
  - Text blocks are concatenated into `content`.
  - Tool-use blocks become `tool_calls`.
  - Tool input is JSON-encoded into `function.arguments`.
  - Extra provider metadata on the neutral tool block is preserved.
- Function tools become:
  `{type: "function", function: {name, description, parameters}}`.
- Built-in non-function tools are passed through unchanged by the shared adapter.

Stream decoding:

- Read `choices[0].delta.content` as text deltas.
- Read `choices[0].delta.tool_calls` as partial tool-call records.
- Open tool-call resources when id/name/metadata appears.
- Append `function.arguments` chunks into `arguments_json`.
- Merge top-level usage.
- Set `stop_reason` from `finish_reason`.
- On finish reason `tool_calls` or `stop`, close all accumulated tool calls and
  JSON-decode their argument buffers into neutral `input` maps.

### xAI Responses

Froth uses xAI Responses when built-in tools are present:

```text
x_search
web_search
code_interpreter
```

Endpoint:

```text
https://api.x.ai/v1/responses
```

Request body:

```json
{
  "model": "...",
  "input": [],
  "stream": true,
  "tools": [],
  "max_output_tokens": 16384,
  "reasoning": {"effort": "..."}
}
```

Message conversion differs from chat completions:

- Tool results become `{type: "function_call_output", call_id, output}`.
- Assistant tool-use blocks become `{type: "function_call", call_id, name,
  arguments}` items.
- Function tool schemas use top-level `name`, `description`, `parameters`.
- Built-in tools pass through as `{type: "x_search"}` etc.

Stream decoding:

- `response.output_text.delta`: append visible text.
- `response.output_item.added` with function call: open tool-call resource.
- `response.function_call_arguments.delta`: append args, keyed by item id.
- `response.output_item.done` with function call: close the tool-call resource.
- `response.completed`: merge usage and set stop reason.

Important edge: xAI Responses can send argument deltas keyed by item id rather
than public call id, so retain both internal resource key and public call id.

## Provider Selection

Froth resolves providers explicitly or by model prefix:

```text
claude*   -> anthropic
grok*     -> grok
gemini*   -> gemini
gpt*      -> openai
chatgpt*  -> openai
o*        -> openai
```

Aliases:

```text
anthropic, claude -> Anthropic
openai, gpt       -> OpenAI
grok, xai         -> xAI/Grok
gemini, google    -> Gemini
```

Provider wrappers resolve API keys, defaults, headers, endpoints, and model
options before constructing the neutral request.

## Agent Loop

Froth's agent loop is:

1. Create a cycle.
2. Persist initial user message as the head.
3. Load message chain ending at current head, oldest first.
4. Convert stored messages to neutral API messages:
   - user -> `{"role": "user", "content": ...}`
   - agent -> `{"role": "assistant", "content": ...}`
5. Stream one LLM response.
6. Broadcast projected stream events while streaming.
7. Persist the final assistant content and metadata.
8. If final content contains no `tool_use` blocks, stop.
9. If tool-use blocks exist:
   - Run each tool concurrently.
   - Convert each result to a neutral `tool_result` block.
   - Persist a user message containing all tool results.
   - Loop back to step 3.

Tool execution contract:

```text
executor receives: {execute, ToolUse{id, name, input}, context}
executor returns:  {:ok, content} | {:error, content} | content
```

Tool result API block:

```json
{
  "type": "tool_result",
  "tool_use_id": "...",
  "content": "...",
  "is_error": true
}
```

The `is_error` key is omitted when false.

## Persistence Model

Froth persists:

- `agent_cycles`: cycle id.
- `agent_messages`: role, JSON content, optional metadata, parent message id.
- `agent_events`: cycle id, current head id, monotonic seq.

The message chain is a linked list through `parent_id`. A cycle event points to
the current head after each append. Loading a conversation means recursively
following parents from the latest head and reversing to oldest-first order.

For a simpler first C++ implementation, persistence can be optional. The useful
concept is still the same: keep an append-only transcript of neutral messages
and a current head.

One Froth persistence pattern to avoid: message chains linked only by
`parent_id`. In practice this has been annoying and not useful. It makes simple
questions like "what happened in this run?" and "what order were these messages
created in?" depend on recursive traversal and extra event bookkeeping. Prefer
an explicit run/session id plus monotonic sequence number for transcript rows,
with optional parent links only if branching is truly needed.

## Testing Patterns

Froth's tests are especially useful:

- Provider request-building tests assert exact wire JSON for messages, tools,
  and tool results.
- Provider decoder tests feed synthetic JSON payloads into `decode_payload`,
  apply edits to a store, and assert final neutral content.
- Agent tests replay recorded SSE fixtures, assert tool execution, and assert
  the second request contains the expected tool result.
- SSE replay loads fixture files and runs the same parser/decoder path as real
  streams.

For this repo, equivalent tests should cover:

- SSE framing with arbitrary chunk boundaries.
- `[DONE]` handling.
- Non-2xx body capture.
- Anthropic text, thinking, and tool-use streams.
- OpenAI-compatible text and partial tool-call argument streams.
- xAI Responses function calls keyed by internal item id.
- Full agent loop: user -> assistant tool use -> tool result -> final assistant.

## C++ Porting Notes

A good C++ shape would be:

```text
llm::Request
llm::Provider interface
llm::Edit
llm::Store
llm::Event
llm::SseTransport
llm::Client
agent::Runner
agent::ToolExecutor
```

Keep provider adapters pure where possible:

```text
build_request(Request) -> HttpRequest
decode_payload(Json, Store const&) -> DecodeResult{edits, done}
finalize(Store const&) -> Response
project_event(Edit const&) -> optional<Event>
```

That makes provider behavior easy to test without a network stack.

Use the `nxt` SSE body mode as the HTTP response body layer, but keep provider
decoding above it. The HTTP client should emit parsed SSE records; the LLM
transport should:

1. Interpret `[DONE]`.
2. JSON-decode `data`.
3. Call provider decoder.
4. Apply edits.
5. Emit projected events.
6. Return finalized response.

Do not let the provider own sockets, retries, or TLS. Do not let the HTTP layer
know about tools. The clean boundary is: HTTP gives SSE records; provider gives
edits; agent gives tool loops.
