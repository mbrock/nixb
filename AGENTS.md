# Repository Guidelines

## Project Structure & Module Organization

- `src/` holds the C++20 sources.
- `build/` is the Meson build directory; binaries like `build/nixb` land here after compilation.
- `testdata/` contains sample Nix log JSON used by the smoke test.
- `subprojects/` stores Meson wrap dependencies.
- `docs/` hosts generated API docs for the Nix library dependencies; leave them untouched unless updating the pinned nix API and Doxygen config.

## Build, Test, and Development Commands

- If the Makefile complains about `IN_NIX_SHELL`, run through a one-liner: `nix develop -c make build` (or `-c make test`) so you do not have to drop into a shell manually.
- Configure once (creates `build/`): `make setup` or `meson setup build --wrap-mode=nodownload`; no need to rerun unless you change Meson options or dependencies.
- Build: `make build` or `meson compile -C build` to produce `build/nixb`.
- Smoke test: `./test.sh` runs `build/nixb` against `testdata/nom.json`; this is the current regression check.
- Docs: `make docs` regenerates `docs/nix-api/`; only needed when updating the nix API reference.
- Cleaning is rarely necessary; use `make clean` only after significant Meson/build layout changes.

## Testing Guidelines

- Existing coverage is a smoke test. If you extend parser behavior, add focused tests alongside new fixtures in `testdata/` (keep fixtures small and reproducible).
- Before pushing, rerun `./test.sh` to ensure canonical `@nix` JSON still parses cleanly.
