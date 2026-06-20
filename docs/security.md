# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and agent mode are not implemented yet.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files are written through a temporary file, fsynced where supported, renamed over the target, and created with mode `0600`.


## Rendered HTML Output

`--output-format html` renders assistant Markdown to HTML, and preserves raw HTML blocks/fragments emitted by the model. It is meant for local rendering and file export, not sanitizing untrusted model output. Do not serve generated HTML to other users or open it in privileged browser contexts unless the content is trusted or sanitized by a separate tool.


## URL Fetching

The first v0.5 input/URL-fetching slice is explicit: `--input PATH` reads supported local `.txt`, `.md`, and `.html` files, and `--fetch-url URL` fetches an HTML page. Used alone, they print converted content according to `--output-format`; used with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, they insert the converted content as a visible user-context message before the final prompt. URL fetching is never triggered implicitly from text inside a prompt.

Defaults:

- response body cap: 1 MiB unless `--max-fetch-bytes N` is set
- connect timeout: existing `--connect-timeout` default
- total timeout for fetch mode: 30 seconds unless `--timeout N` is set
- redirects: not followed in this slice
- request headers: sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers
- content type: accepts empty content type, `text/html`, and `application/xhtml+xml`
- body encoding: validates UTF-8 and rejects invalid legacy-charset bytes with a clear unsupported-feature error
- private/loopback/link-local/multicast/common metadata literal hosts are refused unless `--allow-private-url-fetch` is set

Current limitation: hostname checks are string/IP-literal based. DNS resolution followed by private-address verification is still required before treating arbitrary hostnames as fully protected against local-network probing.

## Local Image Input

Image input is explicit through `--input IMAGE` combined with a prompt. Supported endings are matched case-insensitively and file signatures must match PNG, JPEG, or GIF before data is sent. The default 20 MiB input cap limits both binary reads and subsequent base64 growth; use `--max-image-bytes N` to lower it for constrained environments. WebP input is disabled because tested vision endpoints did not handle it reliably.

Images are embedded in the provider request as data URLs. This sends the complete selected file to the configured model endpoint. Image bytes are held only for request construction and are removed from the in-memory transcript after the call; saved chat files do not contain base64 image data. Image metadata and attachment persistence are deferred to a later schema update.
