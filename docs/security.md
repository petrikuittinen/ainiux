# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and agent mode are not implemented yet.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files are written through a temporary file, fsynced where supported, renamed over the target, and created with mode `0600`.
