# Contributing

Capacity Reconciliation decides *what is true* about facility capacity when
sources disagree. A defect here is not a cosmetic problem: it is a wrong answer
about how much power, cooling, space or accelerator capacity a facility has, and
somebody will commit equipment against that answer. Contributions are therefore
judged first on whether they preserve the invariants below, and only then on
style.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful variants:

```
# Debug, with assertions and iterator debugging enabled
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure

# AddressSanitizer over the whole suite (MSVC: /fsanitize=address)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCAPACITY_RECONCILIATION_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

# Whole-tree packaging proof: build, test, install, then configure, build and
# run an independent out-of-tree consumer against the installed package
cmake -DSOURCE_DIR=. -DWORK_DIR=../capacity-reconciliation-package-check \
      -P cmake/PackageCheck.cmake
```

Warnings are errors by default (`CAPACITY_RECONCILIATION_WARNINGS_AS_ERRORS=ON`,
`/W4 /WX /permissive-` on MSVC). A first-party warning is a defect: fix it rather
than suppressing it. If a suppression is genuinely unavoidable, it must be local,
and it must carry a comment naming the construct that cannot be expressed
otherwise.

There are no test timeouts, and none may be added: not a CTest `TIMEOUT`
property, not a shell timeout wrapper, not a watchdog. A test that hangs is a
defect to diagnose and fix. The same rule applies to the multiprocess suite,
which waits on observable events (a marker in a child's output, a release file,
a process exit) rather than on elapsed time.

## Invariants a change must not weaken

1. **Zero is not unknown.** `Quantity` is either a known exact integer or an
   unknown with a reason. Never introduce a path that turns an unknown into
   zero, and never let an unknown participate in arithmetic as if it were zero.
   If a total cannot be established, it is unknown, and the partial value is
   reported separately in the explanation graph.

2. **A residual is never forced to balance.** The kernel subtracts exactly the
   attributions that name a scope, dimension key and residual kind, and keeps
   whatever is left. There is no balancing, clamping, redistribution or
   tolerance-based adjustment step anywhere. Adding one is a boundary violation,
   not a feature.

3. **Units never mix.** A capacity dimension without its unit is not a quantity.
   Two keys that differ only in unit are different keys, and no code path may
   add, subtract or compare across them. A cross-unit join is reported, never
   coerced.

4. **Identity inside a run is derived, never random.** A run's content digest
   must depend only on its inputs. Node and conflict identities are derived from
   their own content and position; a random identity embedded in the canonical
   encoding would make two computations over identical inputs disagree.

5. **Stale authority is refused, not merged.** Every mutation carries an
   explicit generation precondition, and every durable mutation also carries the
   incarnation. A mismatch is an error, never a merge and never a silent retry.

6. **Recovery yields one whole state.** A store adopts either the published
   commit or the previous commit, verified end to end, or it refuses. There is
   no partial adoption, no repair and no mixing of two records.

7. **Persisted evidence does not become fresh by being read.** A reopened store
   reproduces exactly the stamps that were written. Freshness is re-evaluated by
   the kernel at the caller's evaluation instant, and a recovered run is never
   silently promoted to a current one.

8. **The order of inputs is not part of the answer.** Evidence, attributions,
   scope selections and cells are canonicalised before they affect any digest or
   any output. If a change makes an output depend on insertion order, it is a
   defect.

9. **Bounds are checked before allocation.** Every externally supplied size is
   validated against `Limits` before anything is allocated or read.

10. **A stable token is a contract; prose is not.** Error codes, classification
    names, reason tokens and JSON keys are public and tested. Human-readable
    message text may change freely.

11. **Nothing is hidden to make a result look clean.** A policy cannot be
    configured to suppress conflict sets or unexplained residuals; the kernel
    refuses such a policy outright.

## Tests a change must bring

* a unit test for the new behaviour, in the suite that owns the area;
* a property test when the behaviour is arithmetic or ordering-related, driven
  by a fixed seed so a failure is reproducible;
* a reference-model test when the behaviour computes a number, so the runtime
  and an independently written calculation are compared;
* an adversarial test when the behaviour parses or trusts input;
* an installed-consumer check when the change touches a public header, the
  exported target or the CMake package.

The suites that exist are: `core`, `evidence`, `policy`, `reconcile`,
`reference`, `property`, `diff`/`lifecycle`, `persistence`, `adversarial`,
`concurrency`, `multiprocess`, `scale` and `render`. Put a test in the suite that
owns the property, not in whichever file was open.

## Style

* Portable C++20, standard library only. A new third-party dependency needs a
  correctness justification that outweighs the audit cost, and it will usually
  be refused.
* No floating point in authoritative accounting. Capacities, residuals,
  tolerances and offsets are exact integers with explicit units.
* Checked arithmetic for anything that can overflow. A wrapped capacity is worse
  than an error.
* `Result<T>` / `Status` for fallible operations. The library does not throw for
  control flow, and callers must be able to distinguish invalid argument, stale
  generation, stale authority, conflict, not found, already exists, incompatible
  version, corruption, limit exceeded, unsupported, unavailable, unknown,
  permission, I/O failure, lock conflict and invariant violation.
* Every external or persisted input is untrusted. Validate before use, bound
  before allocating, and reject rather than repair.
* Comments explain *why*, especially where the code deliberately refuses to do
  the obvious thing.

## License and contributions

Contributions are accepted under the Apache License 2.0, the same terms as this
project. There is no Contributor License Agreement and none will be required.
By opening a pull request you confirm that you wrote the contribution, or that
you have the right to submit it under those terms.

Commit messages are public-facing, concise and neutral. Do not add
`Co-authored-by` trailers, AI attribution, or any reference to the tooling used
to produce the change.

## Reporting a defect

A useful capacity-reconciliation defect report contains a minimal scenario file
(the line-oriented format the CLI reads), the clock domain, the evaluation
instant, and either the run digest or a `--json` rendering. If the defect is in
recovery, include the exact bytes of the file that failed to verify and the
error code from `capacity_reconciliation_cli verify`.
