# API Compatibility

Current provider selection, credentials, and CLI examples are in the [README](../README.md#provider-profiles-and-credentials) and [CLI guide](cli.md). This document records protocol details and quirks.

## OpenAI-Compatible Chat Completions

Implemented for built-in OpenAI-compatible profiles:

- `GET /models` under the selected base URL, usually `GET /v1/models`
- `POST /chat/completions` under the selected base URL, usually `POST /v1/chat/completions`
- non-streaming responses with `choices[0].message.content`
- streaming responses with SSE `data:` events and `choices[0].delta.content`
- local PNG/JPEG/GIF input using user content arrays with `text` and `image_url` data-URL parts

If the user does not provide `-m/--model`, `ainiux` calls the models endpoint before chat starts and uses the first returned model id. If the response has no model ids, the chat request omits the model field and user-facing startup status reports `Model: unknown`.

Image request formatting supports multiple input images. In `auto` mode, the client combines registry-level provider capability with recognized vision-model names. Unknown compatible models require `--image-capability allow`, while `deny` disables image input. This is conservative client-side detection rather than a live provider capability protocol; endpoints can still reject formats their active model cannot decode. Responses API image input is not implemented in this slice.

## Responses API

Official `--provider openai` now defaults to Responses (`POST https://api.openai.com/v1/responses`). Keep Chat Completions with `--api chat`, `openai_chat`, or a user `api = chat` setting. `--api responses`, `--responses`, and `openai_responses` still select Responses explicitly. Custom base URLs and `custom_openai_chat` stay on Chat Completions unless the user asks for Responses. Switching to a chat-only profile such as Gemini, Anthropic, or OpenRouter uses Chat Completions even if the previous provider or project session left `api=responses`. An explicit `--api responses` on a chat-only profile still returns `AINIUX_ERR_UNSUPPORTED_FEATURE` unless the user supplies `--responses-url URL`. xAI and DeepSeek now advertise a built-in `/responses` path.

Current Responses support maps `output_text` and streaming `response.output_text.delta` into the same internal assistant message/delta model used by Chat Completions. Reasoning summary deltas are rendered as `<think>...</think>` blocks when providers emit them. User image input uses Responses `input_image` data URLs. Hosted `web_search` tools are catalog-selected (see below). Native and MCP function tools are sent with `strict: false` because their schemas have optional properties; OpenAI strict mode requires every property in `required`. Files, `previous_response_id` / server-side conversations, `file_search`, `code_interpreter`, and live capability probing are not implemented yet.

Interactive Agent credit display currently supports OpenRouter `GET https://openrouter.ai/api/v1/credits` (`data.total_credits - data.total_usage`, displayed as USD), OpenAI `GET https://api.openai.com/v1/dashboard/billing/credit_grants` (`total_available`, displayed as USD), and DeepSeek `GET https://api.deepseek.com/user/balance` (`balance_infos[].total_balance` plus its returned currency). OpenAI's dashboard endpoint can reject project-scoped keys even when those keys can make model requests. These authenticated lookups use the selected provider key, are bounded and cancellable, never persist the response or credential, and do not change inference endpoint compatibility.

## Unified Reasoning Control

`--reasoning auto|off|VALUE|TOKENS` is the public CLI control. Chat and editor expose `/reasoning`, `/reasoning VALUE`, and `/setting reasoning=VALUE`. Auto omits the override. `off` resolves to a matched model's catalog disable choice and otherwise uses the provider protocol's disabled wire shape. A non-negative integer is retained exactly, and other bounded ASCII tokens are retained verbatim; no approximate effort-label/token-budget conversion occurs.

The layered `models.conf` catalog selects one registered protocol after matching the API and a case-insensitive model-family regular expression against only the final slash-separated component of the model ID. Transport or vendor prefixes are ignored. Bundled family records are provider-neutral, although user records may opt into provider scoping for a transport-specific override. The catalog also supplies model-aware selector choices from one pipe-separated `value` list, the documented default, and an optional `context_window` fallback for endpoints whose `/models` entries omit that metadata. Explicit context overrides take precedence, followed by usable endpoint metadata, then the catalog fallback. Direct reasoning values remain accepted when absent from the selector so a newer provider value can work without waiting for a catalog update, but the one-shot CLI warns and interactive chat/editor commands require confirmation before applying one. The endpoint remains authoritative and may reject an unsupported reasoning value.

```text
catalog protocol            outgoing request fields
openai_effort, chat         reasoning_effort: VALUE
openai_effort, responses    reasoning: { effort: VALUE }
openrouter                  reasoning: { effort: VALUE } or { max_tokens: TOKENS }
gemini_effort               reasoning_effort: VALUE
gemini_thinking_level       generation_config: { thinking_level: VALUE }
gemma_thinking_level        chat_template_kwargs: { enable_thinking: BOOLEAN }
anthropic_budget            thinking: { type: enabled, budget_tokens: TOKENS|VALUE }
thinking_toggle             thinking: { type: enabled|disabled|VALUE }
qwen_chat                   chat_template_kwargs: { enable_thinking: BOOLEAN }
qwen_chat_effort            chat_template_kwargs enable_thinking plus reasoning_effort; top-level reasoning_effort when thinking is on
qwen_responses              reasoning: { effort: VALUE }
deepseek, zai               thinking.type plus unmodified reasoning_effort
kimi_effort                 reasoning_effort: VALUE
xai_effort                  reasoning: { effort: VALUE }
minimax_responses           reasoning: { effort: VALUE }
nemotron_template           chat_template_kwargs enable_thinking/reasoning_budget
hy3_template                extra_body.chat_template_kwargs.reasoning_effort
generic_thinking            enable_thinking plus exact thinking_budget
```

Disable spellings such as `none`, `off`, and numeric `0` are recognized where a protocol has an enable/disable shape. Other names—including `minimal`—are not globally rewritten because their meaning is model-specific. Auto is always omitted rather than converted to a guessed default.

Catalog entries currently cover model-specific GPT-5 generations, Gemini/Gemma, Claude token budgets, Grok, DeepSeek, Kimi, GLM, Qwen 3.5/3.6 Chat/Responses, Qwen 3.8 Chat effort, MiniMax Chat/Responses, MiMo Chat/Responses, Stepfun, Nemotron, Hy3, Llama 3.x presets, and both 20B/120B gpt-oss variants. Model matching checks only the final component, so arbitrarily nested prefixes such as `gateway/vendor/GEMINI-...` work without becoming part of the family expression. Native Anthropic Messages is still not implemented; the catalog cannot add an API adapter by itself.

## Hosted web_search

`models.conf` may mark a family `web_search = on` and optionally `web_search_name`. When the matched model has that flag and `web_search.builtin` is on (the default), Ainiux attaches the **provider-hosted** search tool and does not run Tavily/Firecrawl/Exa/Searxng/DuckDuckGo. `--no-builtin-web-search` or user `web_search.builtin = off` restores the client path. `--web-search-provider` does not disable hosted search.

```text
family              name            wire                                         API
GPT-5               web_search      { "type": "web_search" }                     Responses (OpenAI default)
Claude              web_search      { "type": "web_search" }                     Chat (OpenAI-compat; native Messages is not this slice)
Grok 4              web_search      { "type": "web_search" }                     Responses
DeepSeek V4         web_search      { "type": "web_search" }                     Responses
Kimi K2/K3          $web_search     { "type": "builtin_function", "function": { "name": "$web_search" } }  Chat; client echoes arguments
GLM-5               web_search      { "type": "web_search", "web_search": { "enable": true } }  Chat
```

Gemini official grounding is native `google_search` on generateContent/Interactions. The official OpenAI-compat Chat adapter rejects `{ "type": "google_search" }` with HTTP 400, so Ainiux does not attach that hosted tool and keeps the client `web_search` path. Gemini streamed `tool_calls` often omit the OpenAI `index` field and may include `extra_content.google.thought_signature`; Ainiux assigns a stable index and echoes those extras on the next turn. Anthropic hosted search on native Messages (`web_search_20250305`) is not implemented; client search remains the reliable Claude path. Hosted `web_search_call` output items are replayed and not executed locally. Citations from `url_citation` annotations are collected for display.

Temperature metadata is advisory for explicit overrides. Purpose presets omit temperature when the matched model/reasoning combination marks it unsupported. Explicit CLI, configuration, chat, or editor temperature values remain serialized and produce a warning because the provider may reject them. In particular, the bundled catalog distinguishes older GPT-5 models that reject temperature from GPT-5.4/GPT-5.2 models that permit it only with `reasoning=none`; see [OpenAI's current GPT-5 parameter compatibility](https://developers.openai.com/api/docs/guides/latest-model?model=gpt-5.4).

Native agent tool rounds also expose provider-supplied readable reasoning to the interactive agent UI. Chat Completions accepts streamed or non-streamed `reasoning_content`, textual `reasoning` and `reasoning_details`, and `<think>...</think>` traces. Responses accepts readable reasoning summaries/text and their delta events. Encrypted reasoning details and opaque Responses reasoning state are preserved only where protocol continuation requires them and are never rendered as previews. This display path does not synthesize reasoning when the provider supplies none and does not change one-shot agent output.

## Built-In Profiles

```text
provider       aliases                 base URL                                               chat  responses  key default              local
none           offline                 none                                                   no    no         none                   n/a
openrouter                             https://openrouter.ai/api/v1                          yes   no         OPENROUTER_API_KEY     no
openai         openai_chat,            https://api.openai.com/v1                              yes   yes        OPENAI_API_KEY          no
               openai_responses*
deepseek                               https://api.deepseek.com                              yes   yes        DEEPSEEK_API_KEY       no
gemini                                 https://generativelanguage.googleapis.com/v1beta/openai yes  no         GEMINI_API_KEY         no
anthropic                              https://api.anthropic.com/v1                          yes   no         ANTHROPIC_API_KEY      no
xai            grok                    https://api.x.ai/v1                                   yes   yes        XAI_API_KEY            no
moonshot       kimi                    https://api.moonshot.ai/v1                            yes   no         MOONSHOT_API_KEY       no
llamacpp       llama_cpp, llama.cpp    http://localhost:8080/v1                              yes   no         none                   yes
lm_studio      lmstudio, lm-studio     http://localhost:1234/v1                              yes   no         optional               yes
ollama                                 http://localhost:11434/v1                             yes   no         none                   yes
vllm                                   http://localhost:8000/v1                              yes   no         token-abc123           yes
sglang         sg_lang, sg-lang        http://localhost:30000/v1                             yes   no         none                   yes
groq                                   https://api.groq.com/openai/v1                        yes   no         GROQ_API_KEY           no
mistral                                https://api.mistral.ai/v1                             yes   no         MISTRAL_API_KEY        no
together                               https://api.together.ai/v1                            yes   no         TOGETHER_API_KEY       no
perplexity                             https://api.perplexity.ai                             yes   no         PERPLEXITY_API_KEY     no
cerebras                               https://api.cerebras.ai/v1                            yes   no         CEREBRAS_API_KEY       no
fireworks                              https://api.fireworks.ai/inference/v1                 yes   no         FIREWORKS_API_KEY      no
deepinfra                              https://api.deepinfra.com/v1/openai                   yes   no         DEEPINFRA_API_KEY      no
nvidia_nim                             https://integrate.api.nvidia.com/v1                   yes   no         NVIDIA_NIM_API_KEY     no
zai            z.ai, z_ai              https://api.z.ai/api/paas/v4                         yes   no         ZAI_API_KEY            no
qwen           dashscope_intl          https://dashscope-intl.aliyuncs.com/compatible-mode/v1 yes no         DASHSCOPE_API_KEY      no
dashscope                              https://dashscope.aliyuncs.com/compatible-mode/v1    yes   no         DASHSCOPE_API_KEY      no
custom_openai_chat custom              user supplied                                         yes   yes**      AINIUX_API_KEY optional yes/no
```

The `none` profile is an explicit model-offline mode. It accepts no model endpoint, performs no model discovery or chat HTTP requests, and returns `AINIUX_ERR_UNSUPPORTED_FEATURE` for those operations. Standalone editor, local HTML/Markdown/plaintext conversion, URL extraction, and non-model REPL/TUI commands remain available without configuring an endpoint. Explicit URL fetching still performs the requested non-model HTTP operation.

Official `openai` now defaults to Responses. `openai_chat` and `--api chat` select Chat Completions. `openai_responses` still selects the OpenAI profile and Responses.

`custom_openai_chat` can use `/responses` from the supplied base URL when `--api responses` is selected, or any explicit endpoint passed with `--responses-url`.

Anthropic's OpenAI compatibility layer is mainly for testing/comparison. Perplexity's canonical Sonar endpoint is `/v1/sonar`; `/chat/completions` is the OpenAI SDK-compatible alias.

Z.AI uses its general OpenAI-compatible endpoint. Its published API specification does not expose a model-list route, so pass `--model MODEL`; `--list-models` returns an unsupported-feature error without making an HTTP request. Qwen uses Alibaba Cloud Model Studio's Singapore endpoint by default, while `dashscope` retains the China (Beijing) endpoint. Override `--base-url` for another Model Studio region.

## LM Studio

Provider aliases:

- `lm_studio`
- `lmstudio`
- `lm-studio`

Default base URL: `http://localhost:1234/v1`.

LM Studio keys are optional. If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `AINIUX_API_KEY`, or an explicit key option is configured, `ainiux` sends a Bearer token.

If LM Studio or another local server is bound to a LAN-visible address, protect it with appropriate local network controls.
