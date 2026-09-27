# Concurrency audit

This document records the ownership model, the lock order, and the manual
inspection that was performed against the checklist in the project
requirements. It describes what the code does today; the accompanying tests are
named where they exist.

## Ownership

| Object | Owns | Mutated by | Immutable to readers |
| --- | --- | --- | --- |
| `EvidenceSet` | An immutable, canonically ordered set of statements, behind `shared_ptr<const EvidenceSet>` | Nobody, ever. A new set is built and the pointer is replaced | Yes |
| `AttributionSet` | The same, for explanations | Nobody, ever | Yes |
| `Engine` | The evidence pointer, the attribution pointer, the run list, the history, the observer and (optionally) one `Store` | `Engine`'s own mutating methods, under `Engine::mutex_` | Via `evidence_snapshot()` / `attribution_snapshot()` |
| `Store` | One store directory, one exclusive OS lock, one whole `StoreImage`, and a commit mutex | `Store::commit`, under `Store::mutex_` | `image()` returns a reference to state that only changes on commit |
| `ReconciliationRun` | An immutable answer plus a small mutable envelope (resolution, supersession, commit generation) | The engine, on the supersession path | Kernel content is never rewritten; `run_digest()` is fixed at creation |

The single most important consequence: **a reader never holds a lock while it
works**. `Engine::compute` takes the mutex only long enough to copy two
`shared_ptr`s and the policy out of it, then releases the lock and runs the
kernel against those immutable snapshots. A long reconciliation therefore cannot
block an append, and an append cannot observe a half-built run.

## Lock order

One order is defined, and it is honoured everywhere:

```
Engine::mutex_   ->   Store::mutex_   ->   platform::ExclusiveFileLock (OS)
```

* `Store::commit` is the only place that touches the OS lock, and it does so
  inside `Store::mutex_`.
* `Store` never calls `Engine`, so the reverse order cannot arise.
* No lock is ever held across a call that can re-enter the same lock. The one
  callback the runtime offers (the history observer) is invoked *after* every
  lock has been released; see below.

## Checklist results

| Hazard | Finding | Evidence |
| --- | --- | --- |
| Read→write acquisition on the same lock without dropping the read guard | None. `std::lock_guard` is used in a single flat scope per method; no method acquires the same mutex twice or upgrades a guard. | Manual inspection of `src/engine.cpp`, `src/store.cpp` |
| Write lock held while calling code that acquires the same lock | None. `Store::commit` calls only `platform` helpers, which take no `Store` lock. `Engine` mutation methods call `store_->mutate(...)`, which takes `Store::mutex_` — the defined order. | Manual inspection; `concurrency.a_persistent_engine_serialises_commits_under_contention` |
| Mutex re-entry through callbacks | None. The only callback is `Engine::Observer`, and `notify()` explicitly copies the observer under a short lock and then *releases the lock before calling it*. | Manual inspection; `concurrency.an_observer_may_reenter_the_engine_from_many_threads` |
| Event/callback emission beneath internal locks | None. `notify()` is called at the end of each mutating method, after the `lock_guard` scope has closed. | `src/engine.cpp`; the test above re-enters `engine.generation()` from inside the observer |
| Shutdown or join while holding locks needed by workers | Not applicable: the runtime owns no worker threads. Threads that call into it are created and joined by the caller. | `grep` for `std::thread` outside `tests/` returns nothing |
| Reversed nested lock ordering | None. There is exactly one nesting, and `Store` has no path back into `Engine`. | Manual inspection |
| Cancellation paths with reversed ordering | Not applicable: there is no cancellation. Every operation either completes or returns a typed error, and no operation waits on another thread. | Manual inspection |
| Progress/observer callbacks re-entering mutable state | Handled by design: the observer sees an immutable copy of the history entry and runs outside every lock. | The observer test above |
| Shutdown waiting on work while preventing completion | Not applicable: no owned threads, no queues, no worker pools. | Manual inspection |

## Process authority

Threads cannot prove writer exclusion, because two threads share every handle
the process holds. Writer authority is therefore proved with real operating
system processes (`tests/test_multiprocess.cpp`):

* a second **process** attempting to open a held store is refused with
  `lock_conflict`, while a read-only handle in a third process still opens;
* a process that kills itself with `abort()` — no unwinding, no destructors, no
  cooperative release — relinquishes the lock, and a successor process acquires
  it and verifies the store;
* a process terminated from outside (`TerminateProcess`) relinquishes it too;
* eight independent processes contend for one store; exactly one holds it, the
  other seven are refused, and the store verifies afterwards.

On Windows the lock is `CreateFileW` with `dwShareMode == 0`, so no other handle
in any process may open the lock file at all; the kernel releases it when the
process dies. On POSIX it is `flock(LOCK_EX | LOCK_NB)` on a descriptor that
dies with the process. Both are exercised only on Windows on this host; no POSIX
behaviour is claimed.

## Determinism under concurrency

Two threads reconciling the same generation over the same snapshots must produce
byte-identical runs. That is asserted directly
(`concurrency.concurrent_reconciles_produce_one_answer`), and it is the property
that makes the run digest meaningful as a shared reference.
