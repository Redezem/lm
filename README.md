# lm-cli

Minimal C++ client for OpenAI-compatible `/chat/completions` endpoints, with a streaming CLI, an ncurses TUI, and optional tool execution from a local TOML file.

## Features
- CLI mode streams assistant output and can splice file contents or command output into the prompt.
- TUI mode provides a scrollable chat view, multiline input, bracketed paste support, and in-memory conversation state.
- Optional reasoning display appears before the assistant starts emitting visible content.
- Tool calls are loaded from `skills.toml`, executed in a shell, and fed back to the model as tool results.

## Build
- `mise exec -- make lm` builds `lm`.
- `mise exec -- make test` builds and runs the C++ test harness.
- `make clean` removes `lm` and `lm_test`.

The current `Makefile` links both `libcurl` and `ncurses`, so both headers and libraries need to be available.

## Environment
- `LM_BASE_URL`: required. Base URL such as `http://localhost:1234/v1`; the client posts to `/chat/completions`.
- `LM_MODEL`: optional model id. Defaults to `local-model`.
- `LM_API_KEY`: optional bearer token.
- `LM_SYSTEM_PROMPT`: optional system prompt injected at the start of each conversation.
- `LM_HISTORY_FILE`: optional CLI history path. Defaults to `/tmp/lmstudio-cli.history.txt`.
- `LM_SKILLS_FILE`: optional tools file path. Defaults to `~/.local/share/lm/skills.toml`.

## CLI Usage
```sh
./lm "your prompt"
./lm explain this code
./lm "summarize this file" --file path/to/file
./lm "debug this output" --cmd "git status"
./lm "use a different model" --model openai/gpt-oss-20b
./lm "run without prior CLI history" --new
./lm "use alternate tools" --skills /path/to/skills.toml
```

Notes:
- CLI mode requires a prompt unless you launch the TUI.
- Extra unflagged arguments after the first prompt token are appended to the prompt with spaces.
- `--file` appends `=== <filename>` plus the file contents to the prompt.
- `--cmd` appends `=== output of command <cmd>` plus combined stdout/stderr to the prompt.
- `--no-color` disables ANSI colors for streamed assistant output.
- `--raw` disables styling entirely, including the transient reasoning window.
- CLI history is loaded from `LM_HISTORY_FILE` on startup and appended only after a final non-empty assistant reply.
- CLI tool execution is capped at 8 model/tool rounds before the request aborts.

When stdout is a TTY and `--raw` is not set, reasoning text is shown in a transient 20x3 window until assistant content starts. Tool activity is printed afterward in bordered blocks that may be truncated for display.

## TUI Usage
```sh
./lm
./lm --tui
./lm --tui --model openai/gpt-oss-20b
./lm --tui --skills /path/to/skills.toml
```

Notes:
- No-argument startup enters TUI mode automatically.
- TUI mode supports only `--tui`, `--model`, and `--skills`. Prompt text, `--file`, `--cmd`, `--new`, `--no-color`, and `--raw` are rejected in TUI mode.
- TUI history is in memory only; it is discarded on exit.
- `/clear` resets the current TUI conversation and keeps `LM_SYSTEM_PROMPT` if one was configured.
- Tool execution in TUI mode is capped at 800 rounds before the session aborts.

TUI keys:
- `Enter`: submit the prompt
- `Shift+Enter`: insert a newline when the terminal reports it distinctly
- `Ctrl+J`: newline fallback
- `PgUp` / `PgDn`: scroll chat history
- `Ctrl+C`: quit
- `/clear`: clear the current conversation

The TUI enables `nonl()` so `Enter` and `Ctrl+J` remain distinct, and it enables bracketed paste so pasted multiline text stays in the input box. User messages are right-aligned, assistant messages are left-aligned, tool output is boxed, and the temporary reasoning block is labeled `thinking...`.

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
- The parser supports `[[tools]]` and `[[tool]]`, plus `[tools.inputs]` and `[tool.inputs]`.
- Values must be basic quoted TOML strings; this is a small custom parser, not a full TOML implementation.
- Tool names must match `[A-Za-z0-9_-]{1,64}`.
- `$variable` placeholders in `command` become required string parameters in the tool schema sent to the model.
- Arguments substituted into the shell command are single-quoted before execution.
- Extra input descriptions declared under `[tools.inputs]` are included in the schema even if a variable is not referenced by `command`.
- Commands execute via `sh -lc` with the current process environment.

## Security Notes
- CLI conversation history is written in plain text to `/tmp/lmstudio-cli.history.txt` by default.
- `--cmd` and tool commands both execute through a shell; do not pass untrusted input.
- Tool definitions should be treated as trusted local config because they inherit the current environment.
- No committed credentials were present in the repository or its git history as of the 2026-03-14 audit.

## Example
```sh
export LM_BASE_URL="http://localhost:1234/v1"
export LM_MODEL="openai/gpt-oss-20b"
./lm "Write a haiku about caching"
```
