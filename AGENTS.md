This project is a Nix build monitor/prototype.

Current shape:
- The app-specific Nix build monitor code lives under `src/`.
- The reusable terminal UI/runtime code is being extracted under `nxt/`.
- The old implementation lives under `archive/` as reference material only. Much of it has been ported or superseded, though dependency-graph behavior may still be worth comparing.

Namespace convention:
- App-specific code uses the `nxb` namespace.
- Reusable terminal/runtime library code uses the `nxt` namespace.

Build and test:
- The flake provides the compiler and dependency environment Meson uses for setup/reconfigure.
- Once the build directory is configured, `make build` and `make test` may work outside `nix develop` because Meson records resolved dependency paths.
- Use `nix develop` when setting up the build directory, reconfiguring, or if ambient commands cannot find the expected tools/dependencies.
- Build with `make build`.
- Run tests with `make test`.
- The build system is Meson. Do not introduce CMake guidance.

When editing:
- Avoid reviving archived code wholesale; port only the specific behavior needed.
