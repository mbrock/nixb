# Agent Integration: Nix Build Observability for LLM Agents

## Motivation

When an LLM agent (like Claude Code) spawns a nix build, it needs structured
visibility into build progress to make intelligent decisions:

- When to yield control back to the user
- When to check on a stuck build
- How to report progress meaningfully
- When to intervene (e.g., spin up remote builders)

Currently: "start build in tmux, user says 'ping' when done" - manual
cooperative multitasking.

Goal: programmatic yield/resume with rich build state.

## The Yield Primitive

An agent needs to express:

```python
task = spawn_nix_build(".#rb-inotify")

yield WaitingFor(
    NixBuildComplete(task),
    progress_callback=on_progress,
    on_user_message="parallel"  # Can chat while building
)
```

nixb provides the event stream that makes `NixBuildComplete` and progress
callbacks possible.

## Required Data Model

### Build State Query

```json
{
  "target": ".#rb-inotify",
  "state": "blocked",
  "blocked_by": ["ruby"],
  "dependency_chain": [
    {
      "drv": "/nix/store/...-perl-5.40.0.drv",
      "name": "perl",
      "state": "building",
      "phase": "build",
      "elapsed_sec": 512,
      "log_lines": 4823,
      "last_log": "compiling ext/B/B.c"
    },
    {
      "drv": "/nix/store/...-ruby-3.3.10.drv",
      "name": "ruby",
      "state": "queued",
      "waiting_on": "perl"
    },
    {
      "drv": "/nix/store/...-rb-inotify-0.11.1.drv",
      "name": "rb-inotify",
      "state": "queued",
      "waiting_on": "ruby"
    }
  ],
  "aggregate": {
    "total_derivations": 12,
    "completed": 7,
    "building": 1,
    "queued": 4,
    "failed": 0
  },
  "estimated_remaining_sec": 720
}
```

### Event Stream (for yield/resume)

```json
{"event": "build_started", "drv": "perl", "timestamp": "..."}
{"event": "phase_change", "drv": "perl", "phase": "build"}
{"event": "progress", "aggregate": {"completed": 8, "total": 12}}
{"event": "build_completed", "drv": "perl", "success": true, "duration_sec": 623}
{"event": "build_started", "drv": "ruby", "timestamp": "..."}
...
{"event": "target_completed", "target": ".#rb-inotify", "success": true}
```

### Resource Stats (via cgroups)

```json
{
  "builders": [
    {
      "host": "localhost",
      "drv": "perl",
      "cpu_percent": 94.2,
      "memory_mb": 2134,
      "io_read_mb_sec": 12.3,
      "io_write_mb_sec": 45.6
    }
  ],
  "total_cpu_percent": 94.2,
  "total_memory_mb": 2134
}
```

## Integration with nixb

### Existing Components

nixb already has most building blocks:

1. **drv-graph.hpp**: Full derivation dependency graph
   - `Node` with `inputs`/`outputs` relationships
   - `depth`, `height`, `subtree_size` metrics
   - Topological ordering for build order

2. **cgroups.cpp**: Resource stats via cgroups v2
   - Type-safe units via mp-units (bytes, pages, μs, percent)
   - Parsing of memory.stat, cpu.stat, etc.

3. **nxb-store.hpp**: TrivialStore for simulation/testing
   - In-memory store implementation
   - Useful for testing agent integration without real builds

4. **@nix internal-json parsing**: Activity lifecycle tracking
   - start/result/stop events
   - Build, CopyPath, FileTransfer activity types
   - Progress via Builds aggregate activity

### Needed Extensions

1. **Query Interface**: JSON-RPC or Unix socket for agent queries
2. **Event Streaming**: Push events to subscribers (agent harness)
3. **Bridge drv-graph to live builds**: Match running activities to graph nodes
4. **Time Estimation**: Cache historical build times per derivation

### API Sketch

```
# One-shot query
nixb query --target .#rb-inotify --format json

# Event stream (for agent to subscribe)
nixb events --target .#rb-inotify --format jsonl

# Attach to running build
nixb attach <build-id> --format json
```

## Agent Decision Points

With rich build data, an agent can:

1. **Progress Reporting**: "Building perl (78%), then ruby, then rb-inotify"

2. **Stuck Detection**: "perl has been in 'build' phase for 30m with no log
   output - might be stuck"

3. **Resource Decisions**: "CPU at 100% on one core, build is single-threaded -
   nothing to optimize"

4. **Failure Triage**: "ruby failed in check phase - let me look at test logs"

5. **Intelligent Yielding**: "This will take ~20 minutes, I'll yield and let
   you know when it's done"

## Relation to nom

nom (nix-output-monitor) optimizes for human terminal experience:
- Pretty tree rendering
- Color-coded status
- Animated progress

nixb for agents optimizes for machine consumption:
- Structured JSON
- Query/subscribe API
- No rendering, just data

They can share the same parser for `@nix {...}` but differ in output layer.

## Open Questions

1. **Build ID Stability**: How to identify "the build I started" across
   nix daemon restarts?

2. **Multi-target Builds**: Agent builds multiple things - how to track
   which events belong to which request?

3. **Distributed Builds**: Events from remote builders - how to aggregate?

4. **Failure Context**: When build fails, how much log context to include
   in the event? Summarization?

5. **Interruption**: Agent wants to cancel a build - how to signal through
   nixb to nix daemon?

---

*Written during a long rb-inotify rebuild after fixing inotify_init pizlonation
in filc-glibc.*
