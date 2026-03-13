 # Top-Level Directives
	- 1. Keep every Markdown document in this repository current at the end of each task.
	- 2. Maintain this file as the primary contributor guide for future code agents.
	- 3. Preserve the changelog below as append-only and date-stamped.
	- 4. Rewrite the handoff section completely after every task so the next agent starts from current reality.
	- 5. Ensure `.gitignore` excludes build artefacts, packaging output, and local-only tool state.
	- 6. Remove build artefacts before returning control to the user.
	- 7. Use `mise` to manage the build environment.

# Repository Guidelines

## Project Structure & Module Organization
- `main.cpp` contains the entire CLI implementation and is the single compilation unit.
- `Makefile` defines build and cleanup targets.
- `README.md` is a minimal project description.
- `tests/test_main.cpp` contains the lightweight C++ test harness for non-interactive behavior.

## Build, Test, and Development Commands
- `make lm`: builds the `lm` binary using C++20 and links against `libcurl`.
- `./lm`: runs the CLI after a successful build.
- `make test`: builds and runs the C++ test harness.
- `make clean`: removes the generated `lm` and `lm_test` binaries.

This project assumes `libcurl` headers and libraries are available (see `-I/usr/local/include` and `-L/usr/local/lib` in `Makefile`).

## Coding Style & Naming Conventions
- Indentation: 2 spaces; braces on the same line.
- Naming: functions and variables use `lowerCamelCase` (e.g., `readFileAll`), types use `PascalCase` (e.g., `StreamingMarkdownPrinter`).
- Prefer standard library utilities (`std::string`, `std::optional`, `std::filesystem`).
- Keep code ASCII unless a non-ASCII character is required by output formatting.

## Testing Guidelines
- Automated coverage lives in `tests/test_main.cpp` and runs through `make test`.
- Keep tests focused on non-interactive helpers unless you add infrastructure for TUI integration tests.
- Use clear, descriptive test names (e.g., `json_escape_handles_control_chars`).

## Commit & Pull Request Guidelines
- No strict commit convention exists in history; use concise, imperative messages (e.g., "Add streaming parser guard").
- Keep PRs focused. Include a short summary, build/run notes, and any behavior changes.
- If a change affects CLI output, include a sample command and expected output in the description.

## Security & Configuration Tips
- `main.cpp` executes shell commands via `popen`; avoid passing untrusted input.
- Document any new environment variables or flags in `README.md` and in the PR description.

## Changelog
- 2026-03-13: Switched the TUI multiline bindings from `Ctrl+V` to `Ctrl+J`, separated submit vs newline handling with `nonl()`, expanded `Shift+Enter` escape-sequence coverage, added key-binding regression tests, and refreshed repo docs/ignore rules.
- 2026-03-13: Installed `libcurl-devel` and `ncurses-devel` in the Fedora container, verified `make test` and `make lm` succeed via `mise`, and removed the generated binaries with `make clean`.
- 2026-03-13: Removed the unused legacy `buildChatRequestJson` overload from `main.cpp`, updated the test harness to call the live `ChatMessage` overload directly, verified a clean `mise exec -- make lm` build no longer emits the unused-function warning, and re-ran `mise exec -- make test`.

## Handoff
- The current tree has been verified in this Fedora container with `mise`: `mise exec -- make lm` performs a clean rebuild without the prior `buildChatRequestJson` unused-function warning, `mise exec -- make test` returns `ok`, and `mise exec -- make clean` removes `lm` and `lm_test`.
- `main.cpp` now keeps only the active `buildChatRequestJson(const std::vector<ChatMessage>&, ...)` implementation; any future request-body tests should construct `ChatMessage` vectors directly instead of relying on a history/system-prompt wrapper.
- The existing TUI binding changes remain in place: newline insertion is split from submit via `nonl()`, `Ctrl+J` is the fallback multiline binding, extra `Shift+Enter` escape sequences are registered in `configureInputSequences()`, and `tests/test_main.cpp` includes regression coverage for those key-path helpers.
