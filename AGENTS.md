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
- There are currently no dedicated test or asset directories.

## Build, Test, and Development Commands
- `make lm`: builds the `lm` binary using C++20 and links against `libcurl`.
- `./lm`: runs the CLI after a successful build.
- `make clean`: removes the generated `lm` binary.

This project assumes `libcurl` headers and libraries are available (see `-I/usr/local/include` and `-L/usr/local/lib` in `Makefile`).

## Coding Style & Naming Conventions
- Indentation: 2 spaces; braces on the same line.
- Naming: functions and variables use `lowerCamelCase` (e.g., `readFileAll`), types use `PascalCase` (e.g., `StreamingMarkdownPrinter`).
- Prefer standard library utilities (`std::string`, `std::optional`, `std::filesystem`).
- Keep code ASCII unless a non-ASCII character is required by output formatting.

## Testing Guidelines
- No automated tests are present.
- If you add tests, place them under a new `tests/` directory and provide a `make test` target.
- Use clear, descriptive test names (e.g., `json_escape_handles_control_chars`).

## Commit & Pull Request Guidelines
- No strict commit convention exists in history; use concise, imperative messages (e.g., "Add streaming parser guard").
- Keep PRs focused. Include a short summary, build/run notes, and any behavior changes.
- If a change affects CLI output, include a sample command and expected output in the description.

## Security & Configuration Tips
- `main.cpp` executes shell commands via `popen`; avoid passing untrusted input.
- Document any new environment variables or flags in `README.md` and in the PR description.
