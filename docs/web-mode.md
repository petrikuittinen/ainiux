# Embedded browser controller

Ainiux v1.30 serves a responsive browser controller from `/ui/` while
`ainiux server` is running. It is embedded in the executable and uses only
vanilla HTML, CSS, and JavaScript ES modules: there is no Node.js runtime,
framework, npm bundle, CDN, hosted font, or external script.

## Start and connect

Use a dedicated full-control secret and the loopback listener unless you have
explicitly configured the server's TLS/direct-access policy:

```sh
export AINIUX_SERVER_SECRET='use-a-long-random-value'
ainiux server --workspace . --port 8766
```

Open `http://127.0.0.1:8766/ui/` and enter that controller token. Static boot
assets do not require authentication, but every JSON and event-stream request
does. The token is held in JavaScript memory by default. “Keep in this tab
only” uses `sessionStorage`; disconnecting removes it. The WUI never puts the
token in `localStorage`, cookies, URLs, logs, or rendered output.

The controller capability-detects the server before enabling features. It
provides:

- revision-safe ordinary chat threads and asynchronous model responses;
- run/plan jobs, progress, replay/reconnect, cancellation, and image results;
- interactive Act/Plan agent sessions, Confirm/Smart/Yolo selection subject to
  server policy, correlated turn cancellation, and Guard review/allow/deny;
- workspace review and dired navigation, revision-checked create, copy, move,
  and confirmed delete operations;
- a bounded UTF-8 editor with optimistic saves, conflict recovery, and AI
  proposals that modify only the browser draft until Save is selected;
- safe status and capability data already exposed by the control API.

Chat submission first persists the user message, runs the shared asynchronous
chat job, and appends the assistant result only if the thread revision still
matches. On a conflict, the completed result remains in Jobs and the UI asks
the user to reload. File drafts likewise remain visible until the user chooses
whether to keep the draft or reload the current server copy.

## Responsive and accessible behavior

The layout uses flexible CSS Grid/Flex regions. Wide screens show navigation
and work areas side by side; tablet and narrow-mobile breakpoints stack them
without horizontal page scrolling. Controls have touch-sized targets and work
with pointer, touch, or keyboard input. Semantic headings, labels, live regions,
native labelled dialogs, a skip link, and visible focus indicators support
assistive and keyboard-only use. Reduced-motion and forced-color preferences are
respected.

The default theme follows `prefers-color-scheme`. The explicit System, Dark,
and Light selector is tab-scoped. Its palettes use the same color codes as the
built-in Ainiux TUI themes, including dark `#0B0F14`/`#E6EDF3` and light
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
`textContent`; it is never interpreted as HTML. Provider credentials, stored
controller secrets, environment variables, database paths, TLS material,
absolute workspace paths, and hidden project state remain server-side. See
[Security](security.md) and the [control API](api.md) for the complete trust and
network boundary.

## Testing

`make test-unit` checks asset routing, authentication separation, CSP/cache and
browser hardening headers, theme/responsive markers, strict query decoding, and
the absence of external resource URLs, raw-HTML sinks, and `localStorage`.
`scripts/test-control-server.sh --build` exercises the embedded assets and API
through the real loopback listener with `curl`. JavaScript syntax can also be
checked with `node --check src/web/js/app-v1.js` when Node.js happens to be
installed; Node.js is not a build or runtime dependency.
