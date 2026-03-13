# lm-cli

Minimal C++ CLI/TUI client for OpenAI-compatible chat/completions endpoints, with streaming output and optional reasoning display.

## Features
- CLI mode: one-shot prompts with streaming output and optional reasoning window.
- TUI mode: chat-style interface with scrollback, colored roles, and ephemeral in-memory history.
- Runtime-configurable tool calling via TOML (`skills.toml`), with shell execution and streamed follow-up responses.
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
- `LM_SKILLS_FILE`: optional tools TOML path (default `~/.local/share/lm/skills.toml`).

## CLI Usage
- `./lm "your prompt"`
- `./lm "prompt" --file path/to/file`
- `./lm "prompt" --cmd "git status"`
- `./lm "prompt" --model openai/gpt-oss-20b`
- `./lm "prompt" --skills /path/to/skills.toml`
- `./lm "prompt" --new` to reset CLI history for that run.

Notes:
- The CLI streams assistant output and shows reasoning (if provided) in a 20x3 window that clears when content starts.
- `--raw` disables styling; `--no-color` disables ANSI colors.
- If tools are used, tool input/output appears in bordered boxes (truncated in UI display to avoid flooding output).

## TUI Usage
- `./lm` (no args) starts the TUI.
- `./lm --tui` also starts the TUI.
- `./lm --tui --model openai/gpt-oss-20b`

TUI keys:
- `Enter` send prompt
- `Shift+Enter` inserts a newline on terminals that expose it distinctly
- `Ctrl+J` inserts a newline fallback without colliding with terminal quote-next behavior
- `PgUp/PgDn` scroll
- `Ctrl+C` quit
- `/clear` clears the current conversation

Notes:
- TUI history is in-memory only and discarded on exit.
- The TUI enables `nonl()` so `Enter` can submit while `Ctrl+J` inserts a literal line feed.
- Bracketed paste is enabled in the TUI, so pasted multi-line text stays in the current prompt instead of sending line-by-line.
- Reasoning output (if present) shows as a blue, labeled block where the assistant reply will appear.
- Tool input/output is shown inline as bordered blocks in the chat area.

## Tools Config (`skills.toml`)
Default path: `~/.local/share/lm/skills.toml`

```toml
[[tools]]
name = "grepFiles"
command = "grep -n $pattern $path"
description = "Search for text in a file path."

[tools.inputs]
pattern = "Text or regex to search for"
path = "File path to search"
```

Notes:
- `name` must use `[A-Za-z0-9_-]` and be 1-64 chars.
- `command` can include `$variable` placeholders; provided tool arguments are shell-quoted before insertion.
- Commands run in a shell with current environment variables.

## Example
```sh
export LM_BASE_URL="http://localhost:1234/v1"
export LM_MODEL="openai/gpt-oss-20b"
./lm "Write a haiku about caching"
```
