# API Compatibility

## OpenAI-Compatible Chat Completions

Implemented for built-in OpenAI-compatible profiles:

- `GET /models` under the selected base URL, usually `GET /v1/models`
- `POST /chat/completions` under the selected base URL, usually `POST /v1/chat/completions`
- non-streaming responses with `choices[0].message.content`
- streaming responses with SSE `data:` events and `choices[0].delta.content`
- local PNG/JPEG/GIF input using user content arrays with `text` and `image_url` data-URL parts

If the user does not provide `-m/--model`, `pkchat` calls the models endpoint before chat starts and uses the first returned model id. If the response has no model ids, the chat request omits the model field and user-facing startup status reports `Model: unknown`.

Image request formatting supports multiple input images. In `auto` mode, the client combines registry-level provider capability with recognized vision-model names. Unknown compatible models require `--image-capability allow`, while `deny` disables image input. This is conservative client-side detection rather than a live provider capability protocol; endpoints can still reject formats their active model cannot decode. Responses API image input is not implemented in this slice.

## Responses API

Implemented text-only support is available with `--api responses`, `--responses`, or the `openai_responses` profile shortcut. The built-in OpenAI profile maps this to `POST https://api.openai.com/v1/responses`. Custom base URLs can also use the standard `/responses` path. For chat-only provider profiles, `pkchat` returns `PKCHAT_ERR_UNSUPPORTED_FEATURE` unless the user supplies an explicit `--responses-url URL`.

Current Responses support maps `output_text` and streaming `response.output_text.delta` into the same internal assistant message/delta model used by Chat Completions. Reasoning summary deltas are rendered as `<think>...</think>` blocks when providers emit them. Images, files, tools, provider-side context management, and capability probing are not implemented yet.

## Built-In Profiles

```text
provider       aliases                 base URL                                               chat  responses  key default              local
none           offline                 none                                                   no    no         none                   n/a
openai         openai_chat,            https://api.openai.com/v1                              yes   yes        OPENAI_API_KEY          no
               openai_responses*
openrouter                             https://openrouter.ai/api/v1                          yes   no         OPENROUTER_API_KEY     no
deepseek                               https://api.deepseek.com                              yes   no         DEEPSEEK_API_KEY       no
gemini                                 https://generativelanguage.googleapis.com/v1beta/openai yes  no         GEMINI_API_KEY         no
anthropic                              https://api.anthropic.com/v1                          yes   no         ANTHROPIC_API_KEY      no
xai            grok                    https://api.x.ai/v1                                   yes   no         XAI_API_KEY            no
moonshot       kimi                    https://api.moonshot.ai/v1                            yes   no         MOONSHOT_API_KEY       no
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
lm_studio      lmstudio, lm-studio     http://localhost:1234/v1                              yes   no         optional               yes
ollama                                 http://localhost:11434/v1                             yes   no         none                   yes
vllm                                   http://localhost:8000/v1                              yes   no         token-abc123           yes
llamacpp       llama_cpp, llama.cpp    http://localhost:8080/v1                              yes   no         none                   yes
custom_openai_chat custom              user supplied                                         yes   yes**      PKCHAT_API_KEY optional yes/no
```

The `none` profile is an explicit model-offline mode. It accepts no model endpoint, performs no model discovery or chat HTTP requests, and returns `PKCHAT_ERR_UNSUPPORTED_FEATURE` for those operations. Standalone editor, local HTML/Markdown/plaintext conversion, URL extraction, and non-model REPL/TUI commands remain available without configuring an endpoint. Explicit URL fetching still performs the requested non-model HTTP operation.

`openai_responses` selects the OpenAI profile and `--api responses`.

`custom_openai_chat` can use `/responses` from the supplied base URL when `--api responses` is selected, or any explicit endpoint passed with `--responses-url`.

Anthropic's OpenAI compatibility layer is mainly for testing/comparison. Perplexity's canonical Sonar endpoint is `/v1/sonar`; `/chat/completions` is the OpenAI SDK-compatible alias.

Z.AI uses its general OpenAI-compatible endpoint. Its published API specification does not expose a model-list route, so pass `--model MODEL`; `--list-models` returns an unsupported-feature error without making an HTTP request. Qwen uses Alibaba Cloud Model Studio's Singapore endpoint by default, while `dashscope` retains the China (Beijing) endpoint. Override `--base-url` for another Model Studio region.

## LM Studio

Provider aliases:

- `lm_studio`
- `lmstudio`
- `lm-studio`

Default base URL: `http://localhost:1234/v1`.

LM Studio keys are optional. If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `PKCHAT_API_KEY`, or an explicit key option is configured, `pkchat` sends a Bearer token.

If LM Studio or another local server is bound to a LAN-visible address, protect it with appropriate local network controls.
