# lm-cli

Minimal C++ CLI/TUI client for OpenAI-compatible chat/completions endpoints, with streaming output and optional reasoning display.

## Features
- CLI mode: one-shot prompts with streaming output and optional reasoning window.
- TUI mode: chat-style interface with scrollback, colored roles, and ephemeral in-memory history.
- Works with OpenAI-compatible APIs (e.g. LM Studio, local proxies).

## Build
- `make lm` builds the `lm` binary (requires `libcurl` and `ncurses`).
- `make clean` removes built binaries.

## Tests
- `make test` builds and runs a small C++ test harness.

## Environment
- `LM_BASE_URL`: base URL, e.g. `http://localhost:1234/v1`.
- `LM_MODEL`: optional model id.
- `LM_API_KEY`: optional bearer token.
- `LM_SYSTEM_PROMPT`: optional system prompt.
- `LM_HISTORY_FILE`: optional history file path (CLI only, default `/tmp/lmstudio-cli.history.txt`).

## CLI Usage
- `./lm "your prompt"`
- `./lm "prompt" --file path/to/file`
- `./lm "prompt" --cmd "git status"`
- `./lm "prompt" --model openai/gpt-oss-20b`
- `./lm "prompt" --new` to reset CLI history for that run.

Notes:
- The CLI streams assistant output and shows reasoning (if provided) in a 20x3 window that clears when content starts.
- `--raw` disables styling; `--no-color` disables ANSI colors.

## TUI Usage
- `./lm` (no args) starts the TUI.
- `./lm --tui` also starts the TUI.
- `./lm --tui --model openai/gpt-oss-20b`

TUI keys:
- `Enter` send prompt
- `PgUp/PgDn` scroll
- `Ctrl+C` quit
- `/clear` clears the current conversation

Notes:
- TUI history is in-memory only and discarded on exit.
- Reasoning output (if present) shows as a blue, labeled block where the assistant reply will appear.

## Example
```sh
export LM_BASE_URL="http://localhost:1234/v1"
export LM_MODEL="openai/gpt-oss-20b"
./lm "Write a haiku about caching"
```
