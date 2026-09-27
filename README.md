# Capacity Reconciliation

Capacity Reconciliation is a Summon Software Labs runtime for one question:

> When planned, reserved, installed, observed, usable and allocatable facility
> capacity disagree, **what can be reconciled deterministically, what remains
> unexplained, and which evidence is authoritative enough to drive the next
> decision?**

It is repository 16 of the 72-runtime Data Center Control Plane (DCCP), in
Tranche 2: Facility Capacity and Placement. Version 1.0.0. Portable C++20, CMake,
no third-party runtime dependency, no GUI, no telemetry.

The short answer the runtime gives is a *reconciliation run*: an immutable,
generation-bound, digested record of every cell it examined, the exact evidence
it consumed, the conflicts it could not resolve, the exact residuals it
computed, and — for every residual — how much of it an upstream authority
explained and how much nobody did.

## The systems boundary

This runtime owns **reconciliation and discrepancy classification across
capacity views**. It is a consumer and a judge, never a producer and never an
actor.

It owns:

* reduction of many statements about one `(scope, dimension, unit, view)` to one
  value, by an explicit digested precedence policy;
* classification of discrepancies against a fixed, tested precedence order;
* the distinction between an **evidence conflict** (two authorities disagree and
  policy cannot order them) and a **true deficit** (the sources agree and the
  capacity is not there);
* an explanation graph that decomposes every residual into attributed and
  unattributed parts, and **preserves the unattributed part**;
* diffing two runs, revalidating a run against new evidence, closing, reopening
  and the append-only history that links them;
* a versioned, integrity-checked, crash-safe durable store for all of the above,
  with writer fencing by an operating-system lock.

It does **not** own, and does not pretend to own:

* being the source of any capacity figure — Facility Capacity, Rack Capacity,
  Space Capacity, Power Capacity and Cooling Capacity are the sources;
* reservation authority — the reservation register is the authority for what has
  been reserved;
* placement planning — deciding *where* a workload should go;
* observation — collecting measurements;
* actuation — changing anything upstream. This runtime has no code path that
  mutates upstream state. It **never silently fixes upstream state**; it reports
  the discrepancy and stops.

Adjacent runtimes are integrated through explicit typed inputs and outputs:
`EvidenceItem` and `AttributionItem` in, `ReconciliationRun`, `RunDiff` and
canonical JSON out. This runtime depends on none of them as a library, and none
of them depends on it.

## The state and authority model

### Zero is not unknown

`Quantity` is either `known(exact amount)` or `unknown(reason)`. The reasons are
`not_reported`, `source_unavailable`, `unsupported`, `withheld`, `conflicted`,
`superseded`, `partial_rollup`, `expired` and `generation_too_old`. Unknown
propagates through arithmetic as unknown; it is never coerced to zero. Reading
`.value()` on an unknown quantity is a programming defect that terminates with a
diagnostic rather than returning a plausible number.

### Exact units, never mixed

A capacity dimension without its unit is not a quantity. `DimensionKey` binds a
`CapacityDimension` to a `Unit` and validates the pair at construction: a byte
count cannot be constructed as a power figure. Two keys that differ only in unit
are different keys, and the engine records `unit_separated` for the scope rather
than combining them.

All authoritative accounting is exact signed 64-bit integers in the key's own
unit — milliwatts, milliwatt-thermal, rack units, grams, ports, slots,
accelerators, bytes, bits per second. There is no floating point anywhere in the
accounting path, and every operation that can overflow is checked.

### Evidence carries its own authority

An `EvidenceItem` is immutable and self-describing. It carries:

* the producing **source family** (the exact list of consumers in the boundary,
  plus operator declaration) and a source **instance**;
* its **generation**, the producer's own **revision**, the control-plane
  **epoch** and the producing runtime's **incarnation**;
* the **tick** it was observed at and the tick after which it must not be
  treated as current, in a named **clock domain**;
* the exact **unit**;
* the **view** it speaks to: planned, reserved, installed, observed, usable or
  allocatable;
* a **content digest**, so swapping two statements that share an identity but
  not content is detectable.

The runtime refuses evidence stamped beyond the run's generation (evidence from
the future), and refuses to compare ticks across clock domains. It never reads a
wall clock.

### Precedence, and what happens when it runs out

The default policy ranks source families, most authoritative first:

```
observation_stream, reservation_register, asset_registry, rack_registry,
location_registry, facility_capacity, rack_capacity, space_capacity,
power_capacity, cooling_capacity, operator_declaration, unknown
```

Rationale: for the *observed* view nothing outranks a measurement; for the
*reserved* view the reservation register is the authority; the registries
outrank the modelling runtimes because they describe installed physical reality;
operator declarations rank last so that a declaration can never silently
overrule machine evidence.

Within one `(scope, dimension, view)`:

1. statements whose producer revision is behind another statement **from the same
   source instance** are dropped as superseded — this is a producer updating its
   own answer, not a disagreement;
2. the most authoritative family present is selected, and only its statements
   decide the value;
3. if an equally authoritative statement could not establish a value, the view is
   unknown with that reason;
4. if the equally authoritative statements agree, that is the value;
5. if they disagree, **the view is unknown and the conflict is recorded**, with
   every participant, its digest and every asserted value. Nothing picks a
   winner.

A disagreement the policy *can* order is still recorded, as a resolved conflict
with the selected value, so a consumer can see that an authority was outranked.
A disagreement it cannot order is `unresolvable_reason = equal_precedence` (or
`intra_source_disagreement` when one producer instance contradicts itself at the
same revision) and it makes the cell conflicted.

### Residuals, and the part that is never fixed

For every cell the kernel computes exact residuals:

| residual | arithmetic | anomaly direction |
| --- | --- | --- |
| planned gap | planned − installed | either |
| observed gap | installed − observed | either |
| derate gap | observed − usable | either |
| headroom gap | usable − allocated | negative only |
| reservation pressure | reserved − installed | positive only |
| reservation overhang | reserved − allocatable | positive only |
| derived allocatable | usable − reserved | (not a residual) |
| allocatable skew | allocatable − derived | either |

Upstream authorities may attach signed **attributions** to a residual. The kernel
subtracts exactly the attributions that name the same scope, dimension key and
residual kind, and **keeps whatever is left**. There is no balancing, clamping,
redistribution or tolerance-based adjustment step anywhere in the code. If 40 kW
of a 100 kW gap is attributed and 60 kW is not, the explanation graph says
40 explained and 60 unexplained, forever, and the cell is
`partially_explained`.

One-sided constraints are handled explicitly: reservations below nameplate and
headroom held back are normal operating states, so their residuals are computed
and recorded but are not reported as unexplained discrepancies. Only the
violating direction is.

### Classification precedence

A cell's headline classification is the first applicable entry in this fixed,
tested order:

```
arithmetic_overflow, evidence_conflict, no_evidence, generation_regression,
stale, unit_separated, incomplete, observed_shortfall,
observation_exceeds_installed, planned_gap, unplanned_install, usable_derated,
usable_exceeds_observed, allocatable_overstated, allocatable_understated,
reservation_exceeds_installed, reservation_exceeds_allocatable,
unexplained_residual, agrees
```

Every finding is retained, not only the headline. A difference recorded *within*
tolerance keeps a finding with `exceeds_tolerance: false` and the exact residual,
but never becomes the headline: a cell inside its tolerance band is classified as
agreeing.

### Identity is derived, never random

A run's content digest depends on the evidence digest, the attribution digest,
the policy digest, the request and every cell — and on nothing else. Node and
conflict identities inside a run are **derived by hashing their own content and
position**, because a random identity embedded in the canonical encoding would
make two computations over identical inputs produce different digests. The
run's own random identity lives in the persisted envelope and is deliberately
excluded from the content, so two runs over identical inputs have equal digests
and different identities.

Canonicalisation is total: evidence, attributions, scope selections and cells are
ordered before they can affect any output. Evidence supplied in any order, and
scopes listed in any order, produce byte-identical runs.

## Persistence, recovery and fencing

### Layout

```
<root>/CAPACITY-RECONCILIATION-STORE   identity marker: magic, format version,
                                       store uuid, self digest
<root>/LOCK                            exclusive OS lock held by the writer
<root>/HEAD                            published manifest: the commit point
<root>/HEAD.prev                       the previous published manifest
<root>/records/record-<gen>.crr        immutable committed records
<root>/staging/                        staging area; any survivor is residue
```

A record is a framed, self-describing, integrity-checked unit: magic, format
version, an explicit little-endian marker, declared payload length, SHA-256 of
the payload, the previous record's digest (a chain link, cross-checked against
the payload), the store identity, the generation and the attempt id. Every
multi-byte field is written byte by byte, so a record from a foreign endianness
or a foreign format is *detected* rather than misread.

### Commit protocol

1. **plan** — compute the successor whole-state image;
2. **validate** — bounds, invariants, generation monotonicity;
3. **reserve** — generation = head.generation + 1, attempt = head.attempt + 1;
4. **stage** — write `staging/record-<gen>`, flush to durable storage
   (`FlushFileBuffers`; `fsync` on POSIX);
5. **verify** — re-read the staged bytes *from the medium* and re-verify framing,
   digest and decodability;
6. **publish** — atomically rename into `records/`, preserve the outgoing
   manifest as `HEAD.prev`, then atomically replace `HEAD` with
   `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`;
7. **cleanup** — remove staging residue and retire unreferenced records.

`HEAD` is the single commit point. A reader sees either the previous whole state
or the new whole state, never a mixture, and a failure before step 6 leaves the
store exactly as it was.

### Recovery

Opening a store verifies the marker, the published manifest, the referenced
record, the chain link, and the agreement of store identity, generation and
attempt between the header, the payload and the manifest. If every check passes,
that whole state is adopted. If the published manifest fails verification and
`HEAD.prev` verifies, the **previous whole commit** is adopted and the recovery
is recorded in the state itself with a stable note naming the kind of failure.
If neither verifies, or if the two disagree about which store they belong to,
the store is refused.

Every failure reports its **original typed error** — malformed, incompatible
version, limit exceeded, corruption or conflict — annotated with the fact that
no fallback was available. Collapsing them into one code would erase exactly the
information an operator needs.

### Fencing and freshness

* Every mutation carries an explicit generation precondition. A mismatch is
  `stale_generation`, never a merge.
* Every durable mutation also carries the **incarnation**. Opening a store takes
  a fresh incarnation, so authority granted before a restart is fenced:
  `stale_authority`.
* An **epoch** can be advanced, which fences all authority granted under the
  previous epoch. It must move forward.
* **Persisted evidence does not become fresh by being read.** A reopened store
  reproduces exactly the stamps that were written. Freshness is re-evaluated by
  the kernel at the caller's evaluation instant, and a run that depends on
  expired evidence is classified `stale` — or, if the policy permits using it,
  is still marked `stale` so it can never be mistaken for a current answer.
* A **reopen ticket** authorises exactly one successor run at the generation it
  was issued for; replaying it is `stale_authority`.
* **Idempotency is bounded and explicit.** Evidence and attributions are
  immutable and identified; appending one twice is `already_exists`. Nothing is
  deduplicated silently.
* A recovered run is never silently promoted: `Engine::attach` adopts the
  committed state as-is and `revalidate` must be asked for explicitly.

## Concurrency and process authority

Full detail, including the manual inspection checklist, is in
[docs/CONCURRENCY-AUDIT.md](docs/CONCURRENCY-AUDIT.md). The essentials:

* **Ownership.** An `Engine` owns its evidence, attributions, runs, history and
  at most one `Store`. A `Store` owns one directory, one OS lock and one whole
  image.
* **Lock order.** `Engine::mutex_` → `Store::mutex_` → the platform lock. Store
  never calls Engine, so the reverse order cannot arise.
* **Snapshots.** Readers copy an immutable `shared_ptr<const EvidenceSet>` under
  the lock and then work outside it, so a long reconciliation never blocks an
  append.
* **Callbacks.** The one observer is invoked *after* every lock is released,
  with an immutable copy of the event, and may safely re-enter the engine.
* **Cancellation.** There is none to get wrong; no operation waits on another
  thread, and the runtime owns no worker threads.
* **Writer authority is proved with real processes**, not threads. A second
  process is refused with `lock_conflict`; a process killed with `abort()` or
  `TerminateProcess` relinquishes the lock and a successor adopts the store and
  verifies it; eight processes contending for one store leave exactly one
  winner.

## Error model

Every fallible entry point returns `Result<T>` or `Status`. The library does not
throw for control flow. The stable codes are:

`invalid_argument`, `stale_generation`, `stale_authority`, `conflict`,
`not_found`, `already_exists`, `incompatible_version`, `corruption`,
`limit_exceeded`, `unsupported`, `unavailable`, `unknown`, `permission_denied`,
`io_failure`, `lock_conflict`, `invariant_violation`, `overflow`, `malformed`,
`path_rejected`.

`stale_generation` and `stale_authority` are deliberately different: the first
means the caller reasoned about an older state, the second means authority that
was valid has been superseded. `unknown` is never `unavailable`, and neither is
ever zero. Errors carry the expected and current generation, or the exact
constraint that was violated, where that helps.

Classification names, reason tokens, unit and dimension tokens, error codes and
JSON keys are stable public contracts and are tested. Human-readable message text
is not a contract.

## The durable store's bounds

Every externally supplied size is validated against `Limits` **before** anything
is allocated or read: record bytes (64 MiB), manifest bytes, marker bytes, scope
depth and segment length, evidence and attribution counts, cells per run,
evidence per cell, conflicts per cell, explanation nodes and residuals per cell,
retained runs, retained history, retained record files, diff entries and scope
selections per run. Exceeding a bound is `limit_exceeded` naming the bound and
the object, never a silent truncation.

## Build and install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

The build has no third-party dependency. It uses the C++20 standard library and,
on Windows, `bcrypt` for entropy plus the file, locking and durability primitives
of the operating system.

Consuming the installed package:

```cmake
find_package(CapacityReconciliation 1.0 CONFIG REQUIRED)
target_link_libraries(my_target
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)
```

```
cmake -S tests/consumer -B consumer-build \
      -DCapacityReconciliation_DIR=<prefix>/lib/cmake/CapacityReconciliation
```

`tests/consumer` is deliberately a separate project: it reads only installed
headers, the installed library and the installed package files, and drives a real
lifecycle (create, ingest, reconcile, commit, close, reopen, verify, read the
committed run back, remove the store).

`cmake/PackageCheck.cmake` performs the whole proof end to end:

```
cmake -DSOURCE_DIR=. -DWORK_DIR=../capacity-reconciliation-package-check \
      -P cmake/PackageCheck.cmake
```

It configures and builds Release, runs the suite, installs into a scratch prefix,
configures and builds the out-of-tree consumer against that prefix with
`find_package`, runs it, and removes the scratch tree. Every step fails loudly;
nothing is retried and nothing is timed out.

## Examples

* `examples/basic_reconcile.cpp` — one cell in memory, with the residual table
  and the explained/unexplained split printed.
* `examples/rollup_and_conflict.cpp` — the two places a capacity total is easiest
  to fake: a subtree rollup missing a contributor (unknown total, partial sum
  retained) and a disagreement the policy cannot order (both values kept).
* `examples/durable_lifecycle.cpp` — create, ingest, reconcile, commit, close,
  reopen, revalidate, diff, verify, with the store removed at the end.

They are registered with CTest, so the documented output is the tested output.

## The command-line tools

`capacity_reconciliation_cli` is a thin, strict wrapper over the library: it
takes the same locks, applies the same bounds and reports the same typed
outcomes, so the inspection surface is exactly as strict as the operational one.

```
capacity_reconciliation_cli version
capacity_reconciliation_cli policy  [--json]
capacity_reconciliation_cli apply       --store DIR --input FILE [--json]
capacity_reconciliation_cli reconcile   --store DIR --input FILE [--commit] [--json]
capacity_reconciliation_cli inspect     --store DIR [--json]
capacity_reconciliation_cli history     --store DIR [--json]
capacity_reconciliation_cli show        --store DIR --run ID [--cells] [--json]
capacity_reconciliation_cli diff        --store DIR --before ID --after ID [--json]
capacity_reconciliation_cli revalidate  --store DIR --run ID --instant N [--json]
capacity_reconciliation_cli reopen      --store DIR --run ID --instant N [--json]
capacity_reconciliation_cli close       --store DIR --run ID --instant N
capacity_reconciliation_cli verify      --store DIR [--json]
```

The scenario format is line-oriented and deliberately not a configuration
language: `key=value` tokens, a known set of keys, and an error on anything
unknown, duplicated or over-long. `--json` is the machine-readable surface;
everything else is a convenience and is not a contract.

`capacity_reconciliation_worker` exists so that process-level authority is proved
with real processes: `hold`, `try`, `read`, `commit`, `abort`, `verify`.

## Actual validation

Performed on this host, MSVC 19.44.35222 (x64), CMake 4.3.2, Ninja, Windows.

| Configuration | Result |
| --- | --- |
| Release (`/W4 /WX /permissive-`) | builds with zero warnings; suite passes |
| Debug | builds with zero warnings; suite passes |
| RelWithDebInfo + `/fsanitize=address` | builds; suite passes with no AddressSanitizer report |

* **183 tests, 21595 assertions, 0 failures**, identical in all three
  configurations.
* `ctest` runs 4 registered tests (the suite plus the three examples): 100 %
  passed in all three configurations.
* The suite is contract-driven unit tests, an independently written reference
  model for every residual and classification, seeded randomized property tests,
  persistence and recovery tests, adversarial tests, a deliberate hardening
  phase, concurrency tests, real multiprocess tests, scale/bound tests and
  renderer tests.
* Real multiprocess proof: second-process refusal, process-death release,
  external termination release, cross-process commit/read, and eight-process
  contention with exactly one winner.
* Real crash/reopen proof: a store is closed and reopened, its published commit
  is re-read and re-verified from disk, and a corrupted `HEAD` falls back to the
  previous whole commit with the recovery recorded in the state.
* Adversarial results: every single-bit flip in a record is detected by the
  framer or by the store; every prefix truncation is detected; wrong magic,
  wrong version, foreign endianness, oversized declared lengths, a swapped
  store's record, a forged marker, a corrupted marker, a directory where a
  manifest, lock, record or staging directory belongs, a missing records
  directory, a manifest pointing at an absent generation, hostile scope paths,
  an unknown reason token in stored bytes, and repeated open/close cycles all
  behave as documented.
* Installed/downstream proof: `cmake/PackageCheck.cmake` builds, tests, installs,
  then configures, builds and runs the out-of-tree consumer against the
  installed package, and removes its scratch tree.

### Defects found and fixed during hardening

The suite and the adversarial phase found real defects, all fixed and re-tested:

1. the record header size constant was 112 while the format is 136 bytes, so
   every staged record failed its own read-back verification;
2. the framed header was published with a zero payload digest, which would have
   made the manifest unable to identify its own record;
3. `HEAD` manifests and store markers were decoded without consuming their field
   tags, and cell and history fields were read without tags — the persisted
   format did not round-trip at all;
4. a run's envelope identity was compared against a placeholder, so no run could
   ever be decoded;
5. `ReconciliationPolicy` had no default constructor that produced the standard
   policy, so a default-constructed `RunRequest` silently had an *empty*
   precedence list and every multi-source agreement became an unresolvable
   conflict;
6. the policy digest was cached while every policy setting is publicly mutable,
   so the digest could silently disagree with the policy it identified;
7. node and conflict identities were random and were part of the canonical
   content, so two runs over identical inputs produced different digests;
8. a subtree rollup that was missing a contributor emitted the *partial sum* as
   the total — the classic silent under-report the runtime exists to prevent;
9. reservation overhang was computed and explained but never classified, so
   reservations exceeding declared headroom could go unreported;
10. a `const char*` passed to the JSON writer bound to the `bool` overload and
    emitted `true` instead of the token, and the writer emitted a stray comma
    before pre-rendered nested values;
11. `checked_mul(x, 1)` reported a spurious overflow for the minimum value;
12. `checked_sub(amount_min, -1)` and `policy_tolerance_bound` with a zero
    percentage tolerance both refused representable results;
13. `Engine::append_history` took its entry by value, so the observer was
    notified with sequence 0;
14. the engine could retire the very run its new run named as its parent,
    leaving a dangling lineage;
15. the store's recorded clock domain was never updated, so a later reopen
    refused the evidence it had itself accepted;
16. a store with a marker but no commits could not be opened read-only, because
    its clock domain was validated before being defaulted;
17. an unguarded `Quantity::value()` in the allocatable-skew explanation
    terminated the process on an unknown quantity;
18. the library was never compiled with `/W4 /WX` at all, because
    `add_compile_options` was placed after `add_library`;
19. scope matching was O(selections × evidence), making runs quadratic; it is
    now indexed by scope;
20. every verification failure was collapsed into one `corruption` code,
    erasing the difference between malformed, wrong-version, oversized and
    conflicting state;
21. a directory where the lock belongs was reported as `lock_conflict`,
    pointing an operator at a second writer that does not exist;
22. retention could delete a record file the store did not recognise as its
    own.

### Benchmark methodology and results

Benchmarks measure **completed operations only**. A reconciliation is timed to
completion; a durable commit is timed from plan through the atomic `HEAD`
replacement, so the durability flush and the publish are *inside* the
measurement; a reopen is timed from close to opened state, so the read, the
framing verification and the decode are inside it.

The harness runs each workload 9 times and reports the **median and
interquartile range**, never a best case. Each workload verifies the state it
created — cell count, committed generation, reopened digest — before its timing
is reported, and the whole benchmark tree is removed at the end. Workloads are
**SYNTHETIC** and generated in-process from a fixed seed; no hardware is
involved and no hardware claim is made. The workloads were not varied between
runs, so these are absolute figures, not a before/after pair.

The benchmark binary was run four times on the same host. The table reports the
**median of those four run-medians** together with the full range of the four
run-medians, because this host is shared and the run-to-run spread is real
(roughly ±10 %, with individual runs reaching 20 %). No run was discarded and no
run was favoured.

Release, MSVC 19.44 (x64), Windows, per completed operation:

| workload | median of run-medians | range of run-medians |
| --- | --- | --- |
| reconcile a run over 100 cells | 4.82 ms | 4.66 – 5.02 ms |
| reconcile a run over 1000 cells | 54.0 ms | 51.3 – 58.4 ms |
| reconcile a run over 2000 cells | 125 ms | 113 – 131 ms |
| durable commit of 192 evidence items | 9.46 ms | 8.22 – 10.1 ms |
| durable commit of 1536 evidence items | 21.2 ms | 19.9 – 22.4 ms |
| reopen a store holding 3000 evidence items | 14.4 ms | 13.1 – 17.4 ms |

Reconciliation scales linearly in the number of cells (about 48–65 µs per cell
across the four runs). The dominant cost is one SHA-256 per explanation node,
used to derive node identities from content; that is the price of the property
that identical inputs produce identical digests, and it is not paid anywhere on
the read path.

## Honest limitations

1. **MSVC on Windows only.** The sources are portable C++20 and no
   platform-specific construct is used outside `src/platform.cpp`, but no other
   toolchain was available on this host, so Linux, macOS, GCC, Clang and UBSan
   are **UNSUPPORTED on this host** and no POSIX behaviour is claimed. In
   particular the POSIX branch of the file lock, the durable flush and the atomic
   replace are written to the same contract but are not built or executed here.
2. **AddressSanitizer only.** MSVC's `/fsanitize=address` provides memory-error
   detection but not the undefined-behaviour checks UBSan provides, and no UBSan
   runtime is available for this toolchain. No UBSan proof is claimed.
3. **The directive durability point on Windows is `MOVEFILE_WRITE_THROUGH`, not a
   directory fsync.** Windows offers no portable way to flush a directory entry
   through the C++ standard library. `sync_directory` reports success there and
   says so, rather than pretending. On POSIX it performs a real directory fsync.
4. **Crash recovery is proved by close/reopen and by damaged-state recovery, not
   by power loss.** `HEAD` corruption, record corruption, truncation and
   bit-flips are all exercised against real files; an actual power cut cannot be
   produced on this host.
5. **The store is not a distributed consensus system.** It is one directory with
   one writer, fenced by an operating-system lock. Replication, quorum and
   split-brain handling are outside the boundary.
6. **Integrity is not authentication.** SHA-256 proves that bytes were not
   corrupted; it does not prove who wrote them. An adversary who can rewrite the
   whole store, its marker and its manifests can construct a self-consistent
   store this runtime will accept.
7. **Rollups are only as complete as their contributors.** A subtree rollup that
   is missing a contributor is reported as an *unknown* total with the
   contributing and missing counts, never as a smaller number. If every
   descendant reports, the rollup is exact.
8. **The reconciliation kernel is single-threaded per run.** Concurrency is the
   caller's: many threads may reconcile the same immutable snapshot, and the
   result is byte-identical, but one run is computed by one thread.
9. **Explain text is a rendering, not a contract.** The stable machine surface
   is the canonical JSON from `--json` and the run digest.
10. **`charge`-style operator weights are not modelled.** The runtime ranks
   *source families*, not individual producers within a family. Two instances of
   one family that disagree at equal precedence produce an unresolvable conflict
   by design; per-instance calibration is outside the boundary.
11. **Record retention is count-based.** The store keeps the last
    `max_records_retained` commits (8 by default) plus whatever the current and
    previous manifests reference. History is bounded by
    `max_history_retained`; oldest entries are retired first, and the sequence
    stays strictly increasing.

## Repository layout

```
include/summon/capacity_reconciliation/   public headers
src/                                      implementation
  reconcile.cpp                           the reconciliation kernel
  engine.cpp  store.cpp  journal.cpp      stateful engine, durable store, framing
  platform.cpp                            files, locks, durability, entropy
tools/                                    CLI and multiprocess worker
examples/                                 three runnable examples
bench/                                    completed-operation benchmarks
tests/                                    the suite, including consumer/
docs/CONCURRENCY-AUDIT.md                 ownership, lock order, checklist results
cmake/                                    package config and the packaging proof
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
