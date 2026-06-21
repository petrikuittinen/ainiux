# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and agent mode are not implemented yet.

## Configuration Files

Automatic system and user configuration files may select a credential environment variable or key-file path, but API key values and arbitrary authorization headers are not accepted by the schema. Files are capped at 1 MiB and must be regular files. Unknown settings and invalid types fail closed before any part of that file is applied.

`url_fetch.allow_private_addresses = true` relaxes SSRF protections for explicit CLI/TUI fetches and should only be enabled when local-network access is intended. `network.insecure_tls = true` prints a warning whenever effective. User configuration normally lives at `~/.config/pkchat/config.conf`; protect it appropriately if it contains a sensitive key-file path or private endpoint URL.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files are written through a temporary file, fsynced where supported, renamed over the target, and created with mode `0600`.


## Rendered HTML Output

`--output-format html` renders assistant Markdown to HTML, and preserves raw HTML blocks/fragments emitted by the model. It is meant for local rendering and file export, not sanitizing untrusted model output. Do not serve generated HTML to other users or open it in privileged browser contexts unless the content is trusted or sanitized by a separate tool.


## URL Fetching

The first v0.5 input/URL-fetching slice is explicit: `--input PATH` reads supported local `.txt`, `.md`, and `.html` files, `--fetch-url URL` fetches an HTML page, and interactive `/fetch URL` inserts a fetched page into context. Used alone, CLI options print converted content according to `--output-format`; used with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, they insert the converted content as a visible user-context message before the final prompt. URL fetching is never triggered implicitly from text inside a prompt.

Defaults:

- response body cap: 1 MiB unless `--max-fetch-bytes N` is set
- connect timeout: existing `--connect-timeout` default
- total timeout for fetch mode: 30 seconds unless `--timeout N` is set
- redirects: not followed in this slice
- request headers: sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers
- content type: accepts empty content type, `text/html`, and `application/xhtml+xml`
- body encoding: validates UTF-8 and rejects invalid legacy-charset bytes with a clear unsupported-feature error
- private/loopback/link-local/multicast/common metadata literal hosts and resolved socket addresses are refused unless `--allow-private-url-fetch` is set

Resolved IPv4 and IPv6 addresses are checked in libcurl's socket-open callback before a connection is created, so a public-looking hostname cannot connect to a private result. URL fetching through `--proxy` is refused without `--allow-private-url-fetch`, because the client cannot verify target DNS performed by a proxy. The override deliberately disables both literal and resolved-address blocking.

## Local Image Input

Image input is explicit through `--input IMAGE` or repeated `--attach IMAGE` combined with a prompt. Supported endings are matched case-insensitively and file signatures must match PNG, JPEG, or GIF before data is sent. The default 20 MiB input cap limits both binary reads and subsequent base64 growth; use `--max-image-bytes N` to lower it for constrained environments. WebP input is disabled because tested vision endpoints did not handle it reliably.

Images are embedded in the provider request as data URLs. This sends the complete selected file to the configured model endpoint. Image bytes are held only for request construction and are removed from the in-memory transcript after the call; saved chat files do not contain base64 image data. Image metadata and attachment persistence are deferred to a later schema update.

The default `--image-capability auto` mode requires both a provider profile whose Chat Completions adapter can carry image parts and a recognized vision model name. `--image-capability allow` is an explicit trust decision for compatible unknown/custom models; it does not make an incompatible provider understand images.

## Text Attachments

`--attach PATH` and REPL/TUI `/insert PATH` or `/attach PATH` send selected local contents to the configured model endpoint. Text files are limited to 1 MiB each by default (`--max-input-bytes N`), rejected when they contain NUL bytes or invalid UTF-8, and converted according to their `.txt`, `.md`, or `.html` extension. Interactive images are queued for the next prompt, checked against provider/model capabilities, bounded by `--max-image-bytes`, and removed from memory after successful use. Converted text and fetched URL context are intentionally part of the chat transcript and can therefore appear in saved chat files; image bytes do not.

PDF and DOCX are not read as text or uploaded in this slice. Their future input and output converters require explicit dependency, safety, and fidelity decisions.
