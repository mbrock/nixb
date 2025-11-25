# C++ async I/O reaches an inflection point with P2300's standardization

**C++26 finally standardizes the sender/receiver execution model** after years
of "executor wars," but a critical gap remains: no standard I/O vocabulary
exists. While P2300R10 was formally adopted in June 2024 and NVIDIA's stdexec
provides a mature reference implementation, the bridge between scheduling
primitives and actual io_uring operations remains largely unbuilt. Production
systems today rely on Seastar, libunifex, or raw liburing—not the standardized
abstractions. The fundamental tension between sender composition and
io_uring's batched, completion-based model presents design challenges that
Rust's ecosystem has confronted more directly.

## P2300 standardization establishes the foundation

**P2300R10 was adopted into C++26** at the June 2024 WG21 plenary meeting in
St. Louis. This represents the culmination of work by Michał Dominiak, Lewis
Baker, Lee Howes, Michael Garland, Eric Niebler, and Bryce Adelstein Lelbach
dating back to 2021. The proposal introduces three core abstractions:
**schedulers** (lightweight handles to execution contexts), **senders** (lazy,
composable descriptions of async work), and **receivers** (multi-channel
callbacks handling value, error, and stopped completion signals).

The design separates concerns elegantly: `connect(sender, receiver)` produces
an operation state, and `start(operation_state)` initiates execution. This
split enables zero-allocation composition—operation states can be stored on
the stack, in coroutine frames, or aggregated into parent operations. Key
algorithms include `then()` for continuations, `let_value()` for monadic
composition, `when_all()` for concurrent operations, and
`starts_on()/continues_on()` for scheduler affinity.

NVIDIA's **stdexec** serves as the reference implementation with **2,100+
GitHub stars** and integration into the NVIDIA HPC SDK since version 22.11. It
provides schedulers for CUDA, Intel TBB, and macOS, plus extensions like
`exec::static_thread_pool` and `exec::async_scope`. However, the standard
itself provides only `run_loop`—no system thread pool, no I/O context, and no
file or network operations.

Recent revisions removed `ensure_started` and `start_detached` in favor of
structured concurrency via `async_scope` (P3149). The `tag_invoke`
customization mechanism was replaced with member functions per P2855,
improving compile times. Additional C++26 work includes P2079's system
execution context proposal for `get_system_scheduler()`.

## io_uring integration remains primitive in standard-adjacent implementations

**stdexec's io_uring support is minimal**—it provides `exec::io_uring_context`
with a scheduler and timer operations (`schedule_after()` using
`IORING_OP_TIMEOUT`), but no file I/O, networking, or comprehensive async
operations. GitHub Issue #1062 explicitly notes: "The included io_uring
example doesn't show how to read files or other operations supported by
io_uring."

**libunifex offers the most complete sender/receiver io_uring
implementation**. Developed at Facebook (now Meta), it provides
`io_uring_context` with actual file I/O senders: `async_read_only_file`,
`async_write_only_file`, `async_read_some_at()`, and `async_write_some_at()`.
These return `SenderOf<ssize_t>` representing byte counts. Per P2300R10,
"Unifex is still used in production at Meta... serving video calling to
billions of people every month."

Other projects attempting this integration include **maikel/senders-io**
(experimental networking adaptation with `async_resource` concept),
**beman.net** (implementing P2762 networking proposal, currently poll-based
with io_uring planned), and various thin liburing wrappers. None have achieved
the elegance of a complete, standardized solution.

The technical pattern for wrapping io_uring in senders involves storing a
`this` pointer in the SQE's `user_data` field. When the CQE arrives, the
execution context dispatches to the correct operation state via this pointer.
Error handling maps negative `cqe->res` values to `std::error_code`.
Cancellation uses stop tokens combined with `IORING_OP_ASYNC_CANCEL`, though
this mechanism has caveats—disk I/O that has already started cannot be
cancelled.

## The executor wars resolved in favor of structured concurrency

The path to P2300 was contentious. **P0443** (Unified Executors Proposal)
combined task construction and work submission through six `execute` function
variants. Chris Kohlhoff's Asio model used completion tokens and associated
executors—a fundamentally different architecture. The 2018 Rapperswil meeting
identified P0443's poor support for lazy execution, leading P1055 to propose
the sender/receiver split.

**Critical differences drove the resolution**: P0443 executors couldn't report
errors occurring after submission but before continuation invocation. As P2464
noted, "A P0443 executor is not an executor. It's a work-submitter." The
sender/receiver model's three completion channels (`set_value`, `set_error`,
`set_stopped`) provide structured error handling without escape hatches.

Committee polling in 2021 reached consensus to abandon P0443 in favor of
P2300. Kohlhoff and colleagues responded with P2469, arguing the Networking TS
model is a "superset" of P2300 capabilities with decades of production
experience. The Networking TS remains in limbo—neither standardized as-is nor
abandoned.

**Architectural comparison reveals fundamental tradeoffs**:

| Aspect          | Asio                         | Folly Futures         | P2300/stdexec       |
| --------------- | ---------------------------- | --------------------- | ------------------- |
| Execution model | Completion tokens            | Promise/Future        | Sender/Receiver     |
| Laziness        | Configurable                 | Eager by default      | Always lazy         |
| Type erasure    | Optional (`any_io_executor`) | Heavy                 | Minimal             |
| Error handling  | Exceptions/error_codes       | `Try<T>`              | Completion channels |
| Allocation      | Allocator-aware              | Shared state required | Zero by design      |

Folly's `SemiFuture`/`Future` split separates executor-less results from
executor-bound execution, similar conceptually to senders but with eager
semantics. libunifex bridged these worlds and directly influenced P2300's
design through key contributors including Eric Niebler and Lewis Baker (who
also created cppcoro and Folly's coroutine support).

## I/O vocabulary standardization faces significant gaps

**P2762 (Sender/Receiver Interface for Networking)** by Dietmar Kühl proposes
sender-based async networking: `async_accept`, `async_connect`,
`async_read_some`, `async_write_some`. The design treats these as sender
adaptors (pipeable) rather than factories, obtaining the scheduler from the
receiver's environment rather than as an explicit argument.

**Buffer ownership in async contexts** follows a clear principle: the
operation state owns everything needed for operation lifetime. P2300R9 §5.2
mandates that operation states are neither movable nor copyable, guaranteeing
stable addresses. Callers passing buffer references must ensure those
references remain valid until completion. The `connect()/start()` separation
enables callers to control operation state placement—stack, heap, or
aggregated into parent states.

**Cancellation presents harder challenges**. Stop tokens integrate via
`get_stop_token()` on receivers, but io_uring cancellation isn't guaranteed
synchronous. `IORING_OP_ASYNC_CANCEL` can return `-EALREADY`, meaning
cancellation started but the original request's CQE hasn't arrived yet—the
operation state must remain valid. P2762 notes that networking operations are
"generally inactive after the operation was started but the network operation
hasn't completed yet," requiring active cancellation rather than simple atomic
bool checks.

P3409R1 addresses stop token overhead: current `inplace_stop_token` requires
**16-24 bytes** and mutex-protected linked lists for callback registration.
The proposal introduces `single_inplace_stop_source` allowing only one
callback, reducing synchronization overhead for performance-critical paths.

Structured concurrency via **P3149's `counting_scope`** addresses dynamic work
spawning without reference counting. Operations use `nest(sender, token)` to
associate with a scope, and `join()` waits for all nested work to complete
before returning.

## Batching exposes fundamental tension with sender abstractions

io_uring's power comes from batching multiple operations into single syscalls
via `io_uring_submit_and_wait()`. **Senders represent individual
operations**—the abstraction doesn't natively express "submit N operations
together."

Current approaches defer batching to the scheduler level: operations are
queued via `connect()` but not immediately submitted. The io_context
accumulates SQEs and submits them in batches during its event loop. This
requires type-erasing heterogeneous operations into queues, potentially
requiring allocation. **No one has solved this elegantly in C++**.

**Registered buffers and files** (`IORING_REGISTER_BUFFERS`,
`IORING_REGISTER_FILES`) enable zero-copy I/O by creating persistent kernel
mappings. These require upfront registration and index-based access,
conflicting with sender's dynamic composition model where resources have
operation-scoped lifetimes. Potential solutions include resource pools managed
by schedulers, hybrid APIs distinguishing registered from non-registered
operations, or P2300's domain system transforming senders to use registered
resources.

## Rust's ecosystem confronted these problems more directly

The **buffer ownership problem** is fundamental to completion-based I/O: when
you drop a future, the kernel may still be writing to your buffer.
Readiness-based I/O (epoll) doesn't have this problem—you just ignore the
readiness notification and the buffer was never handed to the kernel.

**tokio-uring's solution: owned buffers**. Instead of borrowing buffers, the
API transfers ownership:

```rust
let (result, buffer) = file.read_at(buffer, 0).await;
// Buffer always returned, regardless of success or failure
```

The `BufResult` type returns `(Result<usize>, Buffer)`, ensuring buffer
ownership returns on completion. `IoBuf`/`IoBufMut` traits guarantee pointer
stability. Resources are `!Sync`, eliminating cross-thread concerns.

**glommio (Datadog) takes thread-per-core further**: each thread has
independent io_uring rings—main, latency, and poll rings—with no cross-thread
sharing. This eliminates synchronization entirely. The latency ring enables
priority-aware scheduling; the poll ring uses polled completions for NVMe,
trading CPU for latency.

**monoio (ByteDance)** provides `AsyncReadRent`/`AsyncWriteRent` traits with
ownership-passing semantics, achieving **2-3x improvement over Tokio** in
multi-core scenarios by eliminating cross-thread synchronization.

**Key lessons applicable to C++**:

- Accept that completion-based I/O requires ownership transfer for safe APIs
- Thread-per-core specialized runtimes outperform general-purpose thread pools
  with io_uring
- Registration should be scheduler configuration, not operation configuration
- Multiple ring strategies enable priority handling without lock contention

## Production usage centers on frameworks, not standards

**What people actually use today**:

- **Seastar** (ScyllaDB, Redpanda): Most mature production io_uring
  integration. io_uring became the default reactor backend in October 2022.
  Thread-per-core, shard-per-core architecture with its own future/promise
  model.

- **PhotonLibOS** (Alibaba): Production at Alibaba Cloud, ByteDance, Xiaomi.
  Stackful coroutines with interchangeable epoll/kqueue/io_uring backends.
  Successfully integrated with RocksDB, doubling performance in heavy I/O
  scenarios with ~200 lines of changes.

- **Boost.Asio**: io_uring backend since v1.78.0, but disabled by default.
  Requires both `BOOST_ASIO_HAS_IO_URING` and `BOOST_ASIO_DISABLE_EPOLL` for
  full io_uring usage. Network sockets still default to epoll.

- **libunifex**: Working io_uring_context with file I/O senders. Production at
  Meta, but development has shifted toward stdexec.

- **Raw liburing**: Many high-performance systems use Jens Axboe's C wrapper
  directly with custom coroutine awaitables.

**stdexec with real I/O is not production-ready**. The gap between P2300's
scheduling primitives and practical I/O vocabulary remains significant. No
production system combines stdexec with comprehensive io_uring I/O.

Community sentiment reflects this fragmentation: "There are lots of in-depth
resources for both io_uring and C++20 coroutines, but practically nothing
showing how to combine both." The lack of a dominant async runtime—unlike
Tokio in Rust—creates friction for new projects.

## Conclusions

The C++ async ecosystem is at an inflection point. **P2300 provides the
theoretical foundation** for structured, composable async programming, but
**years of practical work remain** before standard I/O vocabularies exist. The
timeline: basic sender/receiver in C++26, possibly standard async task types
by C++29, and indefinitely for standard I/O schedulers with io_uring/IOCP
backends.

For new projects requiring io_uring today: **Seastar** offers battle-tested
production code with an opinionated thread-per-core model; **libunifex** stays
closest to P2300's direction with proven Meta deployments; **Boost.Asio**
provides familiar APIs with io_uring as an opt-in backend; **raw liburing**
with custom coroutines offers maximum control.

The deeper insight from both C++ and Rust ecosystems: completion-based I/O
fundamentally conflicts with standard ownership models. Rust's tokio-uring and
glommio explicitly accept kernel buffer ownership; C++ will need similar
adaptations. Thread-per-core architectures—already proven by Seastar, glommio,
and monoio—may represent the only path to extracting io_uring's full
performance potential, rather than bolting io_uring onto general-purpose
thread pool schedulers designed for different paradigms.
