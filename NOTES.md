# nix internal-json notes (minimal nixb)

This is a quick reference for the `@nix { … }` internal-json stream as seen in `nix-output-monitor`. It covers what nixb currently parses and what is intentionally missing.

## Actions
Each line is prefixed with `@nix ` and the JSON object has an `"action"` key:
- `start`: begins an activity, with `id`, `type`, `text`, `level` (verbosity), and `fields` depending on the type.
- `result`: update for a prior `start`, keyed by `id`, with `type` and `fields`.
- `stop`: ends an activity, keyed by `id`.
- `msg`: a free-form message (`msg`, `level`); not tied to an activity id.

## Activity types (`start.type`)
Numeric codes mapped to names:
- `100` CopyPath
- `101` FileTransfer
- `102` Realise
- `103` CopyPaths
- `104` Builds (global aggregate “builds” activity)
- `105` Build
- `106` OptimiseStore
- `107` VerifyPaths
- `108` Substitute
- `109` QueryPathInfo
- `110` PostBuildHook
- `111` BuildWaiting
- `112` FetchTree
Any unknown code is treated as `Unknown`.

`start.text` is a human-readable description (often includes store paths/URLs). `start.fields` content depends on the type; nixb currently only uses `type`/`text`.

## Result types (`result.type` and `result.fields`)
Numeric codes mapped to names and field meaning:
- `100` FileLinked: `[bytes_done, bytes_total]`
- `101` BuildLogLine: `[text]`
- `102` UntrustedPath: `[store_path]`
- `103` CorruptedPath: `[store_path]`
- `104` SetPhase: `[phase_text]`
- `105` Progress: `[done, expected, running, failed]`
- `106` SetExpected: `[activity_type_num, expected_count]`
- `107` PostBuildLogLine: `[text]`
- `108` FetchStatus: `[text]`

Unknown codes are labeled `UnknownResult`.

### Progress (result.type 105) details
`Progress` carries an `ActivityProgress{done, expected, running, failed}` (see `nix-output-monitor/lib/NOM/NixMessage/JSON.hs`). nixb and the reference implementation in `~/2025/nix-output-monitor` interpret it differently per activity:
- Builds aggregate (type 104, `id` 3488063200165898 in `testdata/nom.json`): the stream counts down expected builds or downloads (`[0,36,0,0] … [0,1,1,0] … [1,1,0,0]`). The delta of `done` seeds the success-token heuristic in nixb and in upstream `Update.hs`.
- File transfers (type 101): upstream `printTransferProgress` sums `done`/`expected` as bytes for in-flight downloads/uploads. In `testdata/nom.json` narinfo fetches report `[243,0,0,0]` or `[34,0,0,0]` (unknown totals, so `expected` is 0).
- CopyPaths (type 103): still emit `Progress`, but `nom.json` stayed `[0,0,0,0]` because nothing needed copying.

In `testdata/hello.json` (long run that substitutes a lot, then builds GNU hello):
- SetExpected (type 106) arrives for the enclosing Realise activity with `[101,<bytes>]` and `[100,<bytes>]`, then FileTransfer progress ticks up with the same `expected` (e.g. nar downloads climb to `[35783,33057956,0,0]`).
- CopyPaths aggregate (type 103, `id` 72009421684748) counts how many paths will be copied: it rises `[0,1,1,0] → [1,1,0,0]` and finishes at `[22,22,0,0]`.
- Builds aggregate (type 104, `id` 72009421684747) shows queued/running counts: starts `[0,1,0,0]`, briefly `[0,2,0,0]`, then `[0,1,1,0]` while the drv runs, and ends `[0,1,0,1]` because the build was marked non-deterministic.

In `testdata/hello2.json` (offline build, no substitutes, hello succeeds):
- No FileTransfer or CopyPaths progression: aggregates stay `[0,0,0,0]`.
- Builds aggregate (type 104, `id` 151440546856962) counts a single build: `[0,1,1,0]` while running, then `[1,2,0,0]` when finished successfully (expected was bumped to 2 during evaluation).

## Minimal state logic in nixb
- Tracks `id -> {type, text}` from `start`.
- Remembers the global `Builds` activity id (type 104).
- Tracks “success tokens”: when `Progress` arrives for the `Builds` activity, `done` deltas increment a counter. A `stop` for a `Build` consumes a token and marks it as success. This mirrors the Haskell behavior without full dependency info.
- Prints:
  - `[start] <type> <text>`
  - `[result] <kind> …` (decoded fields)
  - `> <log line>` (dim) for BuildLogLine/PostBuildLogLine/FetchStatus
  - `[phase] <phase text>` for SetPhase
  - `[stop] <type> OK` (if success token consumed)
  - `[msg] <text>` for top-level messages

## What we **haven’t** implemented
- `.drv` loading/parsing and dependency graph construction.
- Store-path existence checks or query of outputs.
- Handling of human-readable nix output (only internal-json is parsed).
- Full state summaries, tree rendering, or per-host grouping beyond the token heuristic.
- Caching of build times/reports.
- Robust handling of all `fields` combinations (we only decode the common ones above).

## Sample logs
See `testdata/standard-stderr.json` and `testdata/fail-stderr.json` (copied from `nix-output-monitor` tests) and the larger `../nix-output-monitor/log.json` for richer streams. Pipe any of them into `./build/nixb` to see the current output.
