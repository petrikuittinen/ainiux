# API Compatibility

## OpenAI-Compatible Chat Completions

Implemented:

- `GET /v1/models`
- `POST /v1/chat/completions`
- non-streaming responses with `choices[0].message.content`
- streaming responses with SSE `data:` events and `choices[0].delta.content`
 
If the user does not provide `-m/--model`, `pkchat` calls `GET /v1/models` before chat starts and uses the first returned model id. If the response has no model ids, the chat request omits the model field and user-facing startup status reports `Model: unknown`.

## LM Studio

Provider aliases:

- `lm_studio`
- `lmstudio`
- `lm-studio`

Default base URL: `http://localhost:1234/v1`.

LM Studio keys are optional. If `LMSTUDIO_API_KEY`, `LM_STUDIO_API_KEY`, `PKCHAT_API_KEY`, or an explicit key option is configured, `pkchat` sends a Bearer token.

If LM Studio or another local server is bound to a LAN-visible address, protect it with appropriate local network controls.
