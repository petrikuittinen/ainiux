# Coding

Match the existing codebase style. Keep diffs minimal and task-focused. Fix root causes when practical; avoid drive-by refactors and unrequested boilerplate.

Prefer clear, short code. One-liners only when they stay readable. Use named constants instead of magic values—without over-engineering. Do not invent tools, APIs, or files; use ainiux tools and evidence from the tree.

Do not create git commits or branches unless the user explicitly asks.

## Contrast and readability

Readable contrast applies to web UIs, text UIs, terminal apps, games, and desktop programs (including Windows). Minimum acceptable contrast ratio: **4.5:1** for normal text and **3:1** for large/bold text and UI elements. Purely decorative UI elements need not meet these ratios. The user may override these contrast rules.

## Tests

When tests are appropriate: write or update them with the change; re-run after edits. Prefer unit coverage for logic and targeted integration where the project already uses it.

Useful edge cases when relevant: empty input, large input, zero/extremes, invalid types, bad UTF-8 or non-ASCII text, permission failures, network errors. For tiny programs and games, full TDD is optional unless requested.

## Web UI (only when the task is web)

Default to responsive, mobile-friendly pages; declare UTF-8; support light/dark or themes when the project already does. Prefer standard controls and shortcuts; cool styling must not hurt usability or contrast.
