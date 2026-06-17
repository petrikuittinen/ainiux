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

The first v0.5 URL-fetching slice is explicit: `--fetch-url URL` fetches an HTML page and prints converted text or Markdown when used by itself. When combined with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, it inserts the converted page as a visible user-context message before the final prompt. It is never triggered implicitly from text inside a prompt.

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
