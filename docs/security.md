# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and agent mode are not implemented yet.

## Configuration Files

Automatic system and user configuration files may select a credential environment variable or key-file path, but API key values and arbitrary authorization headers are not accepted by the schema. Files are capped at 1 MiB and must be regular files. Unknown settings and invalid types fail closed before any part of that file is applied.

`--no-config` skips only the automatic user file; system configuration remains effective. `--debug` prints configuration paths and load states to `stderr`, but not parsed values, credential contents, or authorization headers. Paths can still reveal local account or directory names, so avoid debug logs when that metadata is sensitive.

`url_fetch.allow_private_addresses = true` relaxes SSRF protections for explicit CLI/TUI fetches and should only be enabled when local-network access is intended. `network.insecure_tls = true` prints a warning whenever effective. User configuration normally lives at `~/.config/pkchat/config.conf`; protect it appropriately if it contains a sensitive key-file path or private endpoint URL.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files are written through a temporary file, fsynced where supported, renamed over the target, and created with mode `0600`.

The TUI local chat library stores threads in `~/.pkchat/pkchat.db` using SQLite. The directory is created with mode `0700` and the database file with user-only permissions where supported. It stores prompts, responses, provider/base URL/model metadata, attachments, usage JSON, and compaction events, but not API keys, authorization headers, cookies, or configured key-file contents.


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

## Web Search

Web search is explicit through `--search QUERY`, REPL `/search QUERY`, TUI `/search QUERY`, and editor `Esc /search QUERY`. It is never triggered from URLs or search terms found inside prompt text alone.

Defaults:

- result cap: 3 unless `web_search.max_results`, `--max-web-search-results`, or `MAXIMUM_WEB_SEARCH_RESULTS` overrides it
- provider order: configured API providers when keys/base URLs exist, then DuckDuckGo Instant Answer, then Google HTML parsing
- credentials: API keys come from environment variables or config `*_key_env` names; do not store secrets in config files
- network: uses the same libcurl transport, timeouts, and proxy settings as other HTTP features
- local installs: Searxng/Exa on loopback require `--allow-private-url-fetch`, matching URL-fetch private-address policy

Search results are untrusted third-party text. They are inserted as user-context messages and should be treated as external input by both users and models. Google HTML fallback parsing may break when result markup changes.

## Local Image Input

Image input is explicit through `--input IMAGE` or repeated `--attach IMAGE` combined with a prompt. Supported endings are matched case-insensitively and file signatures must match PNG, JPEG, or GIF before data is sent. The default 20 MiB input cap limits both binary reads and subsequent base64 growth; use `--max-image-bytes N` to lower it for constrained environments. WebP input is disabled because tested vision endpoints did not handle it reliably.

Images are embedded in the provider request as data URLs. This sends the complete selected file to the configured model endpoint. Image bytes are held only for request construction and are removed from the in-memory transcript after the call; saved chat files do not contain base64 image data. Image metadata and attachment persistence are deferred to a later schema update.

The default `--image-capability auto` mode requires both a provider profile whose Chat Completions adapter can carry image parts and a recognized vision model name. `--image-capability allow` is an explicit trust decision for compatible unknown/custom models; it does not make an incompatible provider understand images.

## Text Attachments

`--attach PATH` and interactive `/attach PATH` may send selected local contents to the configured model endpoint. Attachment behavior and supported text/image types remain provider-facing. Interactive `/insert FILE_OR_URL` instead places bounded UTF-8 text in the current editor buffer or chat draft; local insertion ignores the file ending, rejects NUL and invalid UTF-8, and is not sent anywhere until the user later sends that chat draft. URL insertion is an explicit network operation: only HTTP(S) is accepted, the normal private-address/DNS/proxy/TLS/timeout/size protections apply, and the response must be UTF-8 HTML. HTML becomes Markdown by default; disabling `input.auto-convert-html-to-md` preserves untrusted raw HTML in the draft. Inserted chat text and attachment context can appear in saved transcripts; temporary image bytes do not.

PDF and DOCX are not read as text or uploaded in this slice. Their future input and output converters require explicit dependency, safety, and fidelity decisions.

## Benchmark Datasets

Benchmark prompts and any fetched reference text are sent to the selected model provider. The built-in 50-case corpus performs no URL fetches. A custom JSONL case may specify `fetch_url`; this is an explicit network operation using the same response-size, timeout, proxy, TLS, private-address, and resolved-socket restrictions as other URL fetching. Benchmark text fetching accepts UTF-8 `text/plain`, `text/html`, or `application/xhtml+xml`; HTML is converted to Markdown before it enters context. The supplied `benchmarks/long-context.jsonl` contacts Project Gutenberg and must be selected explicitly.

Treat third-party datasets as untrusted input. Loading is capped at 16 MiB total and 1 MiB per line, requires unique IDs and a known schema, and validates UTF-8 before any model request. Dataset content can still contain adversarial instructions by design, so do not run benchmarks against providers or tools with privileges beyond ordinary chat.
