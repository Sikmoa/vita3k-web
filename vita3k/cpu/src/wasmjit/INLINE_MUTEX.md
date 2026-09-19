# Inline lightweight-mutex fast paths

The Wasm JIT recognizes the canonical ARM import stubs for:

| Import | NID |
| --- | --- |
| `sceKernelLockLwMutex` | `46e7be7b` |
| `sceKernelUnlockLwMutex` | `91fa6614` |
| `sceKernelUnlockLwMutex2` | `120afc8c` |

A successful uncontended operation executes entirely in generated Wasm. It
returns zero in R0, clears the software exclusive reservation just as the HLE
boundary does, and continues through the real `MOV PC,LR` instruction (including
ARM/Thumb interworking). The warmed region dispatcher can keep chaining without
a JS call, SVC exit, import lookup or fiber switch **for that operation**.
Ordinary instruction accounting and finite scheduler slices still apply.

This is **not** an atomic-CAS implementation. The current browser runtime runs
one guest fiber at a time on one host thread, using unshared linear memory.
Plain loads/stores are sufficient within a non-yielding generated phase. This
protocol must not be reused by a parallel/shared-memory CPU backend unchanged.

## Eligibility and fallback

Only the exact little-endian ARM stub `EF000000 E1A0F00E <NID>` is recognized.
Both single-block and region caches track all 12 bytes, including the NID and
return instruction, so changing either invalidates a recognized stub.

Before changing anything, the emitted probe checks:

- A runtime-owned table and usable fast-memory bases, and a valid current
  guest thread ID.
- An aligned, mapped, read/write guest workarea whose first 20 bytes fit in
  one page. Code pages are excluded, preserving the existing SMC path.
- An enabled slot with the exact workarea address and UID, and matching guest
  owner, count and attributes. Hash collisions decline; they never evict an
  existing live registration.
- A positive request count and a representable signed lock count.
- For lock: free ownership, or self-ownership with recursive attributes and
  no count overflow. For unlock: self-ownership without count underflow.

Contention, waiters, outstanding HLE operations, bad arguments, mismatched
mirrors, inaccessible memory and unsupported cases use the original SVC/HLE
path with unchanged arguments. This patch does not replace waiter queues,
timeouts, cancellation, priority selection, or direct unlock-to-waiter handoff.
It does not add fast paths for heavyweight mutexes or try-lock imports.

## Coherent host/kernel state

The shared layout is in `vita3k/cpu/include/cpu/inline_mutex.h`:

```text
InlineMutexTable:
  dirty_head: u32                 # slot index + 1; zero terminates
  entries[1024]: InlineMutexEntry # 32 bytes each

InlineMutexEntry (all u32):
  workarea, uid, owner, count, attr, enabled, dirty, next_dirty
```

The table occupies **host** linear memory, outside normal guest mappings. A
free owner is `UINT32_MAX`; the guest workarea ABI is independently checked at
compile time (owner +0, count +8, attributes +12, UID +16, size 32).

A generated success updates the slot **and** the guest owner/count words. It
links that slot into the dirty list once per phase. A kernel `Mutex` object may
lag only while generated code is running without kernel observers.

`GuestThreadRuntime::Impl::run_cpu` uses an RAII boundary to call
`mutex_inline_commit` on every return/exception, before HLE, scheduling or
retirement. The commit validates each dirty registration, then updates the
actual lock count and strong `ThreadStatePtr` owner. Cost is proportional to
the number of distinct changed mutexes, not the number of inlined calls or the
table capacity. Broken internal invariants fail closed rather than allowing a
stale kernel owner to proceed.

Both internal kernel lock/unlock implementations use `InlineMutexAccessGuard`.
It disables a slot for the **entire** HLE operation, including a cooperative
park. A nesting count prevents another operation from re-enabling the slot
while the first is suspended. The last guard republishes authoritative state;
only a normalized mutex with no waiters can be enabled. Internal condition-
variable operations use the same guarded implementations. Deletion clears the
registration before reuse; an old guard cannot recreate a deleted lifetime.

The table is optional and runtime-owned. Desktop/native execution leaves it
null. Browser attachment allocates it; retirement clears borrowed CPU pointers
and shutdown frees it after guest records are retired. Mutexes created before
attachment, and registrations that collided, remain on the safe HLE path.

### JIT ABI additions

`JitState` appends `guest_thread_id`, host-address-sized `mutex_table`, and
`mutex_fast_take`, `mutex_fast_release`, `mutex_fast_fallback` counters. The
wasm32 layout is 456 bytes; emitted accesses always use `offsetof`, including
Memory64's wider host pointers. Synthetic R0/R1 reads and the R0 write are
included in the region register-use/spill masks. No new generated Wasm function
imports are required.

The CPU profile reports accumulated per-CPU `mutex_take`, `mutex_release`, and
`mutex_fallback`. These count generated probes, not nanoseconds. Other existing
memory-profile counters are process-global.

## Switch and tests

Enabled by default. Set `Module.VITA3K_JIT_INLINE_MUTEX = '0'` before creating
the module (or `VITA3K_JIT_INLINE_MUTEX=0` for a Node executable) to disable both
stub recognition and kernel table/bookkeeping. Only the exact string `0`
disables it. Configuration is cached for the module lifetime: start a fresh
Worker to change it.

The retail page forwards `?inlineMutex=0`; the headless Limbo harness accepts
`LIMBO_INLINE_MUTEX=0`. The ready diagnostic names the selection. See
[`SCRIPTS.md`](../../../../SCRIPTS.md) for build/run commands.

Asset-free regression tests:

- `vita3k/cpu/tests/wasmjit_inline_mutex_tests.inc`: real translated stub probes
  across single-block and eight region state/base policies, all three NIDs,
  register/flag/exclusive preservation, guard failures, counts, and dirty
  linkage. CPU tests cover NID-only and return-word patches, interworking,
  real lock/unlock loops, and finite slices.
- The warmed 1,000-pair region loop has exactly **one JS dispatcher call and
  one final SVC** (the test's explicit exit), with 1,000 inline takes/releases.
  This demonstrates crossing elimination, not a predicted latency multiplier.
- `browser/tests/inline_mutex_fixture.h`: real kernel creation, zero-HLE pair
  loops, multiple dirty owners, held-across-slice commit, a parked contender
  and direct handoff, collision/lifetime reuse, recursion and deletion.
- Full backend and guest-thread suites run in Memory64 Chromium and wasm32
  Node. The existing cancellation, timeout, exception and teardown tests remain
  green. Native emitter fixtures additionally validate the extended layout and
  reference/promoted-state modules in Node.

Leave inline mutexes enabled for the acceleration regression suites: they
explicitly require successful fast operations. Use the retail toggle for A/B
measurement instead of interpreting a disabled fast-path assertion as a kernel
correctness failure.

## Initial retail measurement

One sequential on/off pair on this two-CPU host, Chromium **153.0.8010.12**,
SwiftShader WebGPU, wasm64 JIT, the same binary and assets, with a **180-second
harness observation window** for each run:

| Measurement | Inline off | Inline on |
| --- | ---: | ---: |
| Presented frames | 3 | 34 |
| Completed GXM indexed draws | 15 | 185 |
| Last live progress rate | 1.97 MIPS | 6.06 MIPS |
| Last live instruction total | 351,807,469 | 1,077,990,851 |
| Imports reaching HLE at that report | 356,500 | 102,000 |
| First frame after `run-app` | 65.915 s | 71.141 s |

Both runs reached the observation deadline, not a guest exit. Both reported
no worker/page/thread/draw failures. The first-frame PNG MD5 was identical:
`b894f635c1e546208ad21908de97613e`. Raw local records are
`.limbo_work/inline-{off,on}-180.log`; assets and images are not committed.

This pair shows roughly **3.1x the reported instruction rate** and substantially
more post-loading progress, **not** faster initial startup, steady gameplay FPS,
or a general 10–100x improvement. The samples are at slightly different times
(~178 s) and different guest work positions; import counts are not an
apples-to-apples per-call cost benchmark. Inclusive HLE time can overlap parked
fibers and must not be summed as CPU time. No sub-microsecond latency or real
audio playback claim follows from these measurements.
