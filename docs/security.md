# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and agent mode are not implemented yet.
