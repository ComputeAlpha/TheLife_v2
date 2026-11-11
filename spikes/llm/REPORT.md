# Spike: glm-5.3-flash:cloud capabilities (Stage 0)

Date: 2026-11-12 · Endpoint: local Ollama 0.40.11 (`http://127.0.0.1:11434/v1`, OpenAI-compatible) · Model metadata: `capabilities: [completion, thinking, tools, vision]`, FP8.
Script: `spikes/llm/spike.py` (only synthetic PIL-drawn screens were sent). About 40 requests in total.

## Results

| Capability | Result | Notes |
|---|---|---|
| Chat | ✅ | ~0.8 s for a trivial reply |
| Reasoning separation (default) | ✅ | Thinking arrives in `message.reasoning`; `content` is clean |
| `reasoning_effort: "none"` | ❌ **unsafe** | Reasoning stops being separated and **leaks into `content`** ("The user wants me to… pong"). Never send `none`; use the default or `low` |
| Streaming + usage | ✅ | SSE chunks; `stream_options.include_usage` returns usage in the final chunk; TTFT ~1.2 s |
| Tool calling: single | ✅ | Correct tool, schema-valid args |
| Tool calling: parallel | ✅ | 3 calls in one message (2× `activities_log`, `tasks_create`), all schema-valid |
| Tool round trip | ✅ | Tool results with `tool_call_id` → clean final reply |
| Vision: description | ✅ | Read the window title and file name from a 1600×900 JPEG (~2k prompt tokens) |
| **Structured output: `response_format: json_schema`** | ❌ **ignored** | Prose or markdown returned in every reasoning mode (default, low, none) |
| **Structured output: native `/api/chat` `format`** | ❌ **ignored** | Same, with `think` true or false |
| Structured output via **tool-as-output** (single `submit_*` tool) | ✅ 5/6 | Schema-valid and correct on 5 of 6. One `tool_choice`-named run returned invented field names (`areas`), so a repair step is required |
| Structured output via **schema in prompt** | ✅ 3/3 | Pure JSON, valid and correct; one run used 751 output tokens |
| Vision classification (work / skill / entertainment) | ✅ | Correct on every schema-valid response; 1.4–3.2 s each |

## Error fixtures (`out/error_fixtures.json`)

| Case | Status | Body shape | Note |
|---|---|---|---|
| Unknown model | 404 | `{"error":{"message":"model \"…\" not found…","type":"not_found_error"}}` | `Content-Type: text/html` despite a JSON body, so the classifier must parse bodies whatever the content type |
| Malformed tool schema | 400 | `{"error":{"type":"invalid_request_error", "message":"json: cannot unmarshal…"}}` | |
| Invalid role | **200** | `content: ""`, `finish_reason: "load"`, zero usage | **A silent failure.** The gateway must treat empty content without tool calls (or `finish_reason ∉ {stop, tool_calls, length}`) as `InvalidOutput` |
| Weekly/hourly limit | not captured | — | Cannot be triggered on purpose. The gateway gets a **fixture-capture mode**: any unclassified non-2xx body is stored (redacted) on first sight, so the real limit body becomes a fixture the first time it occurs |

## Decisions for `agent_lld.md` §3

1. **Structured output strategy:** tool-as-output first (one `submit_<schema>` tool, `tool_choice` auto, an instruction to call it once). Then validate (valijson), then one repair turn with the validation errors, then fall back to schema-in-prompt with the same validation, then fail as `InvalidOutput`. `Capabilities.json_schema = false` for this model.
2. **Reasoning:** never send `reasoning_effort: "none"`. Use `low` for jobs (vision, summaries) and the default for agent turns. Store `reasoning` only in debug capture, never in `messages.content`.
3. **Silent-failure guard:** a 200 response with empty content, no tool calls, or an unknown `finish_reason` is classified `InvalidOutput` and retried once.
4. **Error parsing:** parse JSON error bodies regardless of `Content-Type`; classify on `error.type`, status and message patterns.
5. **Vision cost:** a 1600 px JPEG costs about 2k prompt tokens and ~150–300 output tokens with tool-as-output. Default `capture_max_px = 1600` is fine.

## Still open

- Weekly-limit body (captured on first occurrence, see above).
- The repair rate over a larger sample: the prompt-eval suite (agent_lld §9) should measure it with 20+ synthetic screens before Stage 4.
