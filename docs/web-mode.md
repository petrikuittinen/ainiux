# Embedded browser controller

Ainiux v1.30 serves a responsive browser controller from `/ui/` while
`ainiux server` is running. It is embedded in the executable and uses only
vanilla HTML, CSS, and JavaScript ES modules: there is no Node.js runtime,
framework, npm bundle, CDN, hosted font, or external script.

## Start and connect

Start the browser-oriented server from the workspace you want to control:

```sh
ainiux webserver --workspace .
# equivalent: ainiux server --webui --workspace .
```

The command creates or reuses a private 256-bit token in
`~/.ainiux/server-secret`, prints the managed token and all detected `/ui/`
links, and tries to open the loopback link with the default graphical browser.
On SSH or a text-only system, copy one of the printed links into a browser.
Interface links are local hints; NAT, DNS, VPNs, and firewalls can change which
address another machine can reach.

Open a printed link and enter the controller token. Static boot assets do not
require authentication, but every JSON and event-stream request does. After the
server validates the token, the WUI stores it in origin-scoped HTML5
`localStorage`. A later visit reuses it automatically. “Sign out / Forget
authentication” removes it. HTTP 401 means the secret is no longer valid, so
the WUI removes it and shows “Invalid authentication”; network failures,
timeouts, and server-side 5xx failures do not erase it. The controller retries
transient disconnects with bounded exponential backoff and restores known jobs,
threads, sessions, workspace state, and event streams after reconnecting. The
token never appears in cookies, URLs, logs, or rendered output.

`webserver` defaults to `0.0.0.0` for browser access from another device and
warns prominently when that means plaintext HTTP. Prefer TLS for an untrusted
network, or add `--bind 127.0.0.1` for local-only use. Plain `ainiux server`
keeps its loopback default, does not launch a browser, and never prints a token.
An explicit `--server-secret-file` takes precedence over
`AINIUX_SERVER_SECRET`, which takes precedence over the managed file; explicit
secrets are never echoed. `--quiet` suppresses the managed token but not the
browser URLs.

The controller capability-detects the server before enabling features. It
provides:

- concurrency-safe ordinary chat threads with live streamed model responses;
- provider model suggestions for chat, run/plan, thread creation, and the
  workspace agent, with manual model entry retained as a fallback;
- focused run/plan job progress, replay/reconnect, and cancellation;
- one human-facing workspace Agent with inline provider, model, reasoning,
  Act/Plan, and Confirm/Smart/Yolo controls, live response/reasoning/tool
  activity, correlated turn cancellation, and Guard review/allow/deny;
- a dedicated Image tab with generation controls, an in-page preview, the
  collision-safe server filename, and browser-local download;
- workspace review and dired navigation, revision-checked create, copy, move,
  and confirmed delete operations;
- a bounded UTF-8 editor with optimistic saves, conflict recovery, and AI
  proposals that modify only the browser draft until Save is selected;
- safe status and capability data already exposed by the control API.

Provider selection uses the server/provider API default. The WUI does not carry
a Chat Completions/Responses override between providers: OpenAI may use its
configured Responses default, while DeepSeek and other OpenAI-compatible
providers use Chat Completions unless the server configuration says otherwise.

Chat submission first persists the user message, runs the shared asynchronous
chat job, and appends the assistant result only if the thread revision still
matches. On a conflict, the completed result remains visible in Chat and the UI asks
the user to reload. File drafts likewise remain visible until the user chooses
whether to keep the draft or reload the current server copy.

An unnamed thread initially appears as “New chat”; its first non-empty user
prompt supplies the stored title. Thread rows show the locally formatted
modified date and message count rather than internal concurrency values.
Completed chat and agent turns show compact context, input/output token, elapsed,
TTFT, cache, and decode-rate measurements when the provider/runtime supplies
them. A `~` marker identifies estimated token values.

## Responsive and accessible behavior

The compact, terminal-inspired layout combines the brand and Chat, Jobs, Agent,
Image, Video, Workspace, Settings, and Logout navigation in one top bar. It
keeps metadata, transcripts, and composers visible without wrapping each event
in oversized application cards. Wide screens show navigation and work areas side by side; tablet and
narrow-mobile breakpoints stack them without horizontal page scrolling. Controls work
with pointer, touch, or keyboard input. Semantic headings, labels, live regions,
native labelled dialogs, a skip link, and visible focus indicators support
assistive and keyboard-only use. Reduced-motion and forced-color preferences are
respected.

Chat and Agent keep their headings and composers inside the dynamic viewport;
thread, message, and event lists scroll within their panels. The WUI does not
present a general session manager: opening Agent attaches the newest live
workspace agent or creates one using the project's `.ainiux-pr` settings. In the
chat composer, Enter sends the message. Shift+Enter and Alt+Enter insert a
newline; Ctrl+Enter and Command+Enter remain multiline editing input and do not
submit.

Chat provides Regenerate, Reasoning, and Thinking controls beside the transcript.
Ctrl+R, Ctrl+T, and Ctrl+W are handled when the browser forwards those events,
but ordinary browser tabs reserve Ctrl+T and Ctrl+W and may never deliver them
to a page. Alt+T and Alt+W are therefore provided as keyboard fallbacks, while
the visible buttons work in every supported browser. Esc interrupts the active
response. Agent provides the same visible reasoning control, Alt+T fallback,
and Esc interruption.

The default theme follows `prefers-color-scheme`. The explicit System, Dark,
and Light selector lives in Settings and is stored for the browser origin.
Chat and Agent also accept `/theme light`, `/theme dark`, and `/theme auto`
locally without sending those commands to a model. The palettes use the same
color codes as the built-in Ainiux TUI themes, including dark
`#0B0F14`/`#E6EDF3` and light
`#FFFFFF`/`#000000` foundations.

## Security model

The WUI is a same-origin client of `/ainiux/v1`; it does not contact providers
or third parties directly. Browser responses apply a restrictive CSP with
same-origin scripts, styles, and connections, `data:` only for returned image
jobs, no framing, no referrers, and disabled sensitive browser features.
Versioned CSS/JavaScript assets are immutable-cacheable; the HTML shell is
`no-store`. Only exact embedded asset paths are served—there is no filesystem
or directory-backed static serving.

All model, tool, file, and error text is inserted through DOM nodes and
`textContent`; it is never interpreted as HTML. Provider credentials, the
server's secret source/file, environment variables, database paths, TLS material,
absolute workspace paths, and hidden project state remain server-side. See
[Security](security.md) and the [control API](api.md) for the complete trust and
network boundary.

## Testing

`make test-unit` checks asset routing, authentication separation, managed-secret
permissions and stability, CLI forms, URL reporting, CSP/cache and browser
hardening headers, theme/responsive markers, strict query decoding, and the
absence of external resource URLs, raw-HTML sinks, cookie/query-string token
handling, and third-party JavaScript.
`scripts/test-control-server.sh --build` exercises the embedded assets and API
through the real loopback listener with `curl`. JavaScript syntax can also be
checked with `node --check src/web/js/app-v8.js` when Node.js happens to be
installed; Node.js is not a build or runtime dependency.
