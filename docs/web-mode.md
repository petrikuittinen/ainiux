# Embedded browser controller

Ainiux v1.34 serves a responsive browser controller from `/ui/` while
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

- concurrency-safe ordinary chat threads with live streamed model responses,
  local file attachments (PNG/JPEG/GIF, PDF, DOCX, Markdown, plaintext, HTML;
  documents convert to Markdown), bounded upload warnings, and `/chat-to-pdf` /
  `/last-to-pdf` downloads;
- safe client-side Markdown rendering for Chat and Agent prose, including
  semantic headings, responsive GFM tables, clickable HTTP(S) links, and the
  full TUI set of highlighted fenced-code languages;
- provider model suggestions for chat, run/plan, thread creation, and the
  workspace agent, with manual model entry retained as a fallback;
- focused run/plan job progress, replay/reconnect, and cancellation;
- one human-facing workspace Agent with inline provider, model, reasoning,
  Act/Plan, and Confirm/Smart/Yolo controls, live response/reasoning/tool
  activity, correlated turn cancellation, and Guard review/allow/deny;
- a dedicated Image tab whose provider/model/size/aspect/quality/format controls
  come from the effective system and user `images.conf`, with dependent choices,
  validated custom dimensions, in-page preview, collision-safe server filename,
  and browser-local download;
- ordered PNG/JPEG reference-image selection for catalog models that support
  image editing, including local thumbnails and per-model input limits;
- a dedicated Video tab driven by layered `videos.conf`, with catalog-derived
  scalar controls, ordered image/video/audio references, cancellable jobs,
  HTML5 MP4 playback, and authenticated download;
- workspace review and dired navigation, revision-checked create, copy, move,
  and confirmed delete operations;
- a bounded UTF-8 editor with optimistic saves, conflict recovery, PDF/DOCX-to-
  Markdown conversion when opening `.pdf` or `.docx` (Save writes the sibling
  `.md`; WebUI DOCX export remains deferred), detected
  per-file indentation controls, Tab/Shift+Tab indent and outdent, adaptive
  selection/file reformatting, and bounded undo/redo. Use `Ctrl+U` or `Ctrl+Z` to
  undo and `Ctrl+Y` to redo; `Alt+U`/`Alt+Z` and `Alt+Y` are browser-safe
  fallbacks. AI proposals modify only the browser draft until Save is selected;
- safe status and capability data already exposed by the control API.

Provider selection uses the server/provider API default. The WUI does not carry
a Chat Completions/Responses override between providers: OpenAI may use its
configured Responses default, while DeepSeek and other OpenAI-compatible
providers use Chat Completions unless the server configuration says otherwise.

Reference images are uploaded to authenticated, memory-only server storage when
Generate is selected. Each file is limited to 20 MiB, one image job is limited
to 40 MiB combined and 16 files globally, and each model may advertise a lower
count. Opaque upload IDs expire after one hour; removing a preview or logging out
requests early deletion. A running job retains only the immutable buffers it
needs. Uploads are never exposed as workspace paths or persisted by the input
store.

Video references follow the same opaque-ID lifecycle and stay server-side.
Accepted media and counts depend on the selected `videos.conf` record; global
limits are 30 MiB per image, 15 MiB per audio file, 200 MiB per video file,
1 GiB combined, and 50 files. Successful jobs write a collision-safe
`videoN.mp4` in the fixed workspace. The browser fetches the authenticated
artifact into a local Blob URL for the native `<video controls>` player and
download action, then revokes that URL on reset or sign-out. FAL and Replicate
keys, queue/prediction control URLs, CDN/Files input URLs, and provider output
URLs never enter browser JSON.

Composer attachments belong to the current draft only: opening another thread
or New chat clears them. Send is disabled only for the thread that has an
in-flight turn; other threads stay usable, and Escape cancels a stuck send
even before a job id exists.

Chat submission first persists the user message (and any converted attachments),
runs the shared asynchronous chat job with that thread id, and appends the
assistant result only if the thread revision still matches. The browser does
not resend the transcript as job `content`; the server hydrates stored Markdown
attachments, including converted PDF and DOCX input, so later turns still see the file. On a
conflict, the completed result remains visible in Chat and the UI asks the user
to reload. File drafts likewise remain visible until the user chooses whether to
keep the draft or reload the current server copy.

An unnamed thread initially appears as “New chat”; its first non-empty user
prompt supplies the stored title. Thread rows show the locally formatted
modified date and message count rather than internal concurrency values.
Each writable row also has a small **Delete** control under that metadata.
Empty threads delete immediately; threads with user or assistant content ask
for confirmation first in an in-page dialog (never `alert`/`confirm`/`prompt`).
Read-only threads cannot be deleted.
On the first authenticated browser load, the controller sweeps leftover empty
threads, then creates and selects a new thread even when older conversations
exist. New chat, including that first empty thread, reuses the last used
provider and model when one is available. It prompts for a provider only when
none has been chosen yet. A thread left without
user or assistant content is revision-safely abandoned when another thread is
selected or created; reconnection reloads the active thread instead of creating
another one and keeps that thread while sweeping other empties.
Completed assistant responses on a writable thread can be edited in place
(Save or Cancel). User and assistant messages can be deleted after an in-page
confirmation; deleting a message also removes every message after it, so a
turn can be rewound to an earlier user prompt. System messages stay
append-only. Streaming cards have no edit or delete controls until the turn
finishes.
Completed chat and agent turns show compact context, input/output token, elapsed,
TTFT, cache, and decode-rate measurements when the provider/runtime supplies
them. A `~` marker identifies estimated token values.

## Model selection and remembered settings

Chat and Agent show compact provider/model names that link to the relevant
section of Settings. Choosing a provider immediately starts its `/models`
lookup and continues into the returned model choices, matching the terminal
flow; `none` is not presented as a usable provider, and a sole returned model
is selected automatically. Manual model entry is available through the
explicit **Enter model manually** fallback, never as an empty primary action.
There are no catalog counts or extra picker buttons in either conversation toolbar.
Reasoning stays directly adjustable; Agent also retains Act/Plan and permissions.

**Alt+P** opens the provider picker and **Alt+M** opens the model picker for the
current Chat, Agent, workspace editor, or run/plan job, including from the message
composer. In Settings the shortcuts use the focused section, or the last active
chat/workspace scope; they also work in the new-thread dialog. Other open dialogs,
text composition, and AltGr combinations are left alone. Visible controls remain
available if a browser or operating system reserves a shortcut.

Provider and model buttons in Settings, new threads, and run/plan jobs open the
same picker. Use Up/Down, PageUp/PageDown, or Home/End to move;
Enter selects and Esc cancels. Press `.` or **Sort A–Z** to toggle alphabetical
and original provider order without changing the highlighted model. Press `/`,
type a search term, and Enter to jump to the next case-insensitive substring
match. `/` then Enter repeats the previous search for that list and wraps at
the end. **Search** and **Find next** provide the same pointer/touch actions.
Search navigates the complete list rather than filtering it. Model names may
still be entered manually, including when a provider cannot list models, by
opening **Enter model manually**; its submit action remains disabled until a
non-empty name is entered.
The search, sort, and navigation keys apply inside the picker list, not while
typing a model or message.

Settings begins with **Current chat**, **Workspace agent & editor**, and
**Appearance**, followed by status and capability details. The first two cards
provide one input-like provider picker and one model picker; manual model entry
remains inside the model dialog.

Model settings use two separate scopes: **Current chat** and
**Workspace agent & editor**. Fields use the same request-setting validation as
the TUI: temperature, top_p, top_k, min_p, repeat_penalty, presence_penalty,
max_tokens, reasoning, stream, context_tokens, and auto_convert_html_to_md.
Empty optional fields restore their automatic/unset behavior. Terminal layout
and editor formatting settings are not model settings.

Chat changes save on selection or field commit (Enter or leaving the field),
without waiting for a message. Choosing another thread restores its own values
from the existing chat database. Saves are revision-checked; read-only threads
and stale writes are rejected. Chat model controls are locked during generation.

Agent and editor assist share the workspace configuration, independent of the
selected chat thread. Changes are saved in the existing project database, even
before opening Agent, and are locked while an agent turn runs. Opening Agent
after a restart restores its provider, model, request settings, Act/Plan policy,
and saved transcript, ready for the next instruction. It does not automatically
execute or replay interrupted work. **Load older history** pages backward through
the transcript; request-only compaction summaries are excluded, and explicit
context-reset boundaries are respected. Each page is limited to 100 rows and
4 MiB; a larger individual row reports an error without altering stored history.

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
Agent view, the activity transcript owns the vertical scrollbar while the
instruction composer remains anchored at the bottom of the viewport. New
activity follows the bottom only while the reader is already near it, so
scrolling back through a running transcript does not immediately jump down. In the
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

Chat user/assistant prose and Agent user/response prose use the embedded
dependency-free Markdown renderer, including while a response streams. It
supports headings, paragraphs and hard breaks, emphasis, lists, blockquotes,
rules, inline/fenced code, and GFM tables. Fences receive TUI-role-compatible
client-side coloring for Markdown, Python, C/C++, C#, Java,
JavaScript/TypeScript, HTML/HTML-only, CSS, XML, JSON, Bash, PHP, Perl, Ruby,
Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI. HTML composes the
markup, CSS, and JavaScript lexers for style/script element bodies and inline
style/event attributes. Unknown or unlabelled fences stay plain. Agent
thinking, tool calls, notices, approvals, and errors use distinct TUI-derived
colors; fenced source excerpts in activity rows reuse the same highlighter.
Workspace dired colors directories and executables distinctly, and the file
viewer and live editor detect the native TUI language from its path. Editor
highlighting follows each draft change and stays aligned while scrolling.
Run/plan job output and unstructured activity remain safely literal.

When a file opens, the browser inspects its first 20 physical lines and fills
the editor's **Width** (1–32) and **Indent** (Spaces/Tabs) controls, falling back
to four spaces when the sample is inconclusive. These values belong to the
loaded browser file only; changing them does not write Ainiux configuration.
In edit mode, Tab inserts to the next visual stop (or inserts one literal tab),
and Shift+Tab removes one indentation level. With a selection, both keys act on
all touched physical lines and preserve forward or reverse selection direction.

The reformat button reads **Reformat selection** while a range is selected and
**Reformat file** otherwise. It changes leading whitespace only and uses the
same brace, Ruby, Bash, markup, SQL, topology, and assembly profiles as the
native editor. Comments, strings, heredocs, Markdown fences, YAML scalar bodies,
and other protected multiline regions are ignored; HTML keeps embedded
JavaScript/CSS structure. YAML reformat always emits spaces. Plain-text files
retain manual Tab/Shift+Tab but disable reformat because there is no language
structure to infer. Each operation is one browser undo step, and tab width is
kept in sync across the textarea, live syntax overlay, and read-only viewer.
Reformatting remains synchronous only within the 1 MiB editable-file boundary;
lines beyond the 64 KiB analysis bound, and structurally uncertain content that
follows them, are preserved.

Markdown and bare absolute HTTP(S) links are underlined and open in a new tab
with `noopener`, `noreferrer`, and no referrer. Relative links, URL credentials,
other schemes, image syntax, and raw HTML remain visible inert text. A link is
never opened or fetched until the user activates it.

## Security model

The WUI is a same-origin client of `/ainiux/v1`; it does not contact providers
or third parties directly. Browser responses apply a restrictive CSP with
same-origin scripts, styles, and connections, `data:` only for returned image
jobs, no framing, no referrers, and disabled sensitive browser features.
Versioned CSS/JavaScript and the HTML shell are all `no-store`. Only exact
embedded asset paths are served—there is no filesystem or directory-backed
static serving.

All model, tool, file, and error text is inserted through constructed DOM nodes,
`createTextNode`, and `textContent`; model-provided HTML is never interpreted as
markup. The Markdown renderer uses an allowlisted element vocabulary and does
not use raw-HTML sinks. The programming-language lexer emits only text nodes and
allowlisted semantic `<span>` classes, applies source/line/token work bounds,
and never interprets highlighted HTML as DOM markup. Provider credentials, the
server's secret source/file, environment variables, database paths, TLS material,
absolute workspace paths, and hidden project state remain server-side. See
[Security](security.md) and the [control API](api.md) for the complete trust and
network boundary.

## Testing

`make test-unit` checks asset routing, authentication separation, managed-secret
permissions and stability, CLI forms, URL reporting, CSP/cache and browser
hardening headers, theme/responsive markers, strict query decoding, and the
absence of external resource URLs, raw-HTML sinks, cookie/query-string token
handling, and third-party JavaScript. When Node.js is available it also runs the
dependency-free Markdown DOM and fenced-language behavior tests through
`make test-web-js`; otherwise
that optional browser-source check reports a skip without changing build/runtime
requirements.
`scripts/test-control-server.sh --build` exercises the embedded assets and API
through the real loopback listener with `curl`. JavaScript syntax can also be
checked with `make test-web-js` when Node.js happens to be installed; Node.js is
not a build or runtime dependency.
