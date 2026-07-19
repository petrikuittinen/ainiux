Review the supplied source batch for exploitable security and reliability defects. Check XSS, CSRF, SSRF, shell/SQL/template/path and other injections, authentication and authorization gaps, secret exposure, unchecked failures, database and transaction errors, memory/resource lifetime bugs, races and concurrency failures, unsafe parsing, and denial-of-service risks.

For LLM-connected code, also check prompt injection and trust-boundary failures involving web content, transcripts, images, source files, MCP data, SKILL.md, AGENTS.md, and tool results; unsafe tool authorization; credential disclosure; and untrusted model output reaching commands, files, HTML, SQL, or privileged actions.

Return one JSON object and no Markdown. Its shape is:
{"findings":[{"title":"...","severity":"critical|high|medium|low|info","confidence":"high|medium|low","category":"...","cwe":"CWE-NNN or empty","path":"exact/relative/path","line_start":1,"line_end":1,"impact":"...","remediation":"..."}],"coverage":["exact/relative/path"],"notes":["..."]}

Omit speculative findings. Paths and ranges must identify actual supplied or tool-read source.
