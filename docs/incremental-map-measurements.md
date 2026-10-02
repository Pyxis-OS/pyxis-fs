# Incremental allocation-map matched RAM observations

Two unchanged 40-case matrices were captured before implementation at published
`5e44d6f` (tree identical to merged filesystem `e5b76c5`), after Pyxis #313 was
confirmed merged. Two serial after matrices used `6b95138`. Later documentation
commits do not change the measured production or workload code. The bounded
[record](measurements/incremental-map.json) preserves counters, configurations,
verification and safety evidence in before-1/before-2/after-1/after-2 order.

Command: `sudo -n python3 tests/ram_run.py --suite baseline`. The unchanged cases
use 32/256-file populated histories, separately durable 4 KiB/256 KiB writes,
small overwrites and compiler-style create/write/rename/remove. Pyxis operations
remain individually durable; native operation and 16-operation batch boundaries
are separate comparisons. This is the existing small matrix, not new sustained,
large-population, fragmented-pressure or real-device qualification.

## Submitted writes

KiB below covers the measurement window through final synchronization and
maintenance, including native post-end submissions. Preparation is separate.
Pyxis byte totals agree across both samples on each revision. Native ranges use
all four contemporaneous samples. These are submitted block bytes, not physical
NVMe traffic, and do not imply a universal filesystem amplification ratio.

| Files | Case | Pyxis before | Pyxis after | Reduction | ext4 operation | Btrfs operation |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 32 | 4 KiB writes | 2,944 | 2,860 | 2.85% | 1,564 | 4,904 |
| 32 | 256 KiB writes | 4,732 | 4,720 | 0.25% | 4,444 | 5,708 |
| 32 | Small overwrite | 1,520 | 1,512 | 0.53% | 248–260 | 2,720 |
| 32 | Compiler history | 3,056 | 3,056 | 0% | 1,772 | 5,864 |
| 256 | 4 KiB writes | 4,564 | 3,044 | 33.30% | 1,568 | 5,000 |
| 256 | 256 KiB writes | 5,208 | 4,808 | 7.68% | 4,448 | 5,804 |
| 256 | Small overwrite | 2,428 | 1,640 | 32.45% | 252–264 | 2,816 |
| 256 | Compiler history | 5,236 | 3,120 | 40.41% | 1,776–1,848 | 5,896 |

Including preparation, full Pyxis histories submit 5,720→5,636, 7,508→7,496,
4,584→4,576 and 5,792→5,792 KiB at 32 files; 31,172→25,192, 31,816→26,956,
29,348→24,076 and 31,780→25,228 KiB at 256 files, in the same case order.
No reclamation work is omitted to obtain these reductions. The independent
operation ledger, final namespace/content comparison and read-only reopen pass
in every case; no capacity refusal counts as completion.

After measurement-phase traffic is separated below. Data is unchanged between
revisions; metadata savings mostly affect user publications in these histories.
The compiler orphan phase includes its active cleanup publications, regardless
of whether earlier debt is present. A final volume fence still writes metadata.
Remaining pool debt is valid bounded idle debt, not a promise of complete reuse.

| Files | Case | User data KiB | User metadata KiB | Orphan metadata KiB | Fence metadata KiB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 | 4 KiB writes | 256 | 2,556 | 0 | 48 |
| 32 | 256 KiB writes | 4,096 | 576 | 0 | 48 |
| 32 | Small overwrite | 216 | 1,244 | 0 | 52 |
| 32 | Compiler history | 64 | 2,432 | 512 | 48 |
| 256 | 4 KiB writes | 256 | 2,740 | 0 | 48 |
| 256 | 256 KiB writes | 4,096 | 664 | 0 | 48 |
| 256 | Small overwrite | 216 | 1,372 | 0 | 52 |
| 256 | Compiler history | 64 | 2,496 | 512 | 48 |

Native batched workloads use a different acknowledgment boundary; Pyxis has no
batched mode in this comparison. Their measurement-window KiB are recorded
separately and are not treated as equivalent to individually durable Pyxis calls.

| Files | Case | ext4 batch | Btrfs batch |
| ---: | --- | ---: | ---: |
| 32 | 4 KiB writes | 440–452 | 1184 |
| 32 | 256 KiB writes | 4160 | 4456 |
| 32 | Small overwrite | 212 | 644 |
| 32 | Compiler history | 240 | 1152 |
| 256 | 4 KiB writes | 440–452 | 1312 |
| 256 | 256 KiB writes | 4160 | 4488 |
| 256 | Small overwrite | 200–212 | 740 |
| 256 | Compiler history | 240 | 1280 |

## Closure and planning

Diagnostics aggregate preparation plus measurement publications, including tail
maintenance. These are observations, never required publication counts. The
reference is the current bulk builder for the same immutable input/logical work,
using its preclaim record count and conservative shape; it is not a second write
execution. Local plans with equal or higher cost remain permitted and reported.
Preparation and measurement plan counters are not separated, so these aggregates
cannot establish steady-state hit rates. The nearly constant 171–172 global
fallbacks at 256 files suggest a preparation contribution; that attribution is
unverified until phase-specific diagnostics exist.

| Files | Case | Local / all plans | Global fallbacks | Source J max | Closure q max | Growth passes | Redistributed nodes | Local less/equal/more than bulk | Map nodes emitted / bulk reference |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 32 | 4 KiB writes | 17 / 157 | 140 | 5 | 5 | 157 | 0 | 17 / 0 / 0 | 455 / 475 |
| 32 | 256 KiB writes | 3 / 109 | 106 | 4 | 3 | 109 | 0 | 3 / 0 / 0 | 256 / 259 |
| 32 | Small overwrite | 2 / 126 | 124 | 4 | 4 | 126 | 0 | 2 / 0 / 0 | 330 / 332 |
| 32 | Compiler history | 0 / 172 | 172 | 3 | 3 | 172 | 0 | 0 / 0 / 0 | 444 / 444 |
| 256 | 4 KiB writes | 433 / 605 | 172 | 10 | 9 | 722 | 117 | 432 / 1 / 0 | 2,232 / 3,710 |
| 256 | 256 KiB writes | 386 / 557 | 171 | 9 | 8 | 636 | 79 | 385 / 1 / 0 | 2,009 / 3,212 |
| 256 | Small overwrite | 402 / 574 | 172 | 10 | 9 | 656 | 82 | 401 / 1 / 0 | 2,093 / 3,391 |
| 256 | Compiler history | 449 / 620 | 171 | 9 | 8 | 676 | 56 | 448 / 1 / 0 | 2,175 / 3,767 |

Each 256-file history records one coincident exhausted-overflow/global fallback
(last run l=5,r=231). No exhausted-underflow or resource fallback occurs in this
matrix; synthetic tests exercise both occupancy failure directions. The raw
record includes closure/source sums, largest additions, signed local cost and
fallback frequencies; reasons can overlap. The unchanged 32-file compiler
traffic demonstrates that a topology-preserving attempt can provide no write
saving when global closure is routine.

Source maps reach at most ten nodes in this matrix. The reductions do not establish
how closure size grows with larger maps. Qualification must include substantially
larger populated maps, with configuration proposed separately under the existing
RAM/no-swap safeguards; a node-count target is not a filesystem contract. Before
those runs, the [planning follow-up](incremental-map.md#diagnostics-and-limits)
should address repeated linear retirement lookups and phase attribution.

The observed planning interval is first backing read inside the publisher through
first replacement-write callback. It includes source/adapter reads and optional
bulk-reference counting, excludes earlier planning, and is not CPU-only time.
After-only sums over complete histories round to 0.254, 0.152–0.153,
0.188 and 0.248 seconds at 32 files; 1.569–1.570, 1.381, 1.449–1.452
and 1.591–1.593 seconds at 256 files. Per-publication
maxima are at most 0.006 seconds in these samples. No independent timer isolates
reference counting, and there is no uninstrumented after timing sample.

Mean primary measurement-window seconds increase from 0.261→0.316,
1.088→1.093, 0.185→0.210 and 0.305→0.350 at 32 files; 1.052→1.149,
1.482→1.500, 0.613→0.661 and 0.897→0.991 at 256 files. Source traversal,
closure/vector work and optional diagnostics are real costs. These RAM callback
observations establish reduced submitted traffic for the larger small histories,
not improved latency, NVMe throughput or a population-independent cost bound.

## Storage safety and validation

All four matrices used the existing scoped launcher: bounded tmpfs with `noswap`,
a zero-swap memory cgroup, no disk fallback, and unprivileged build/workload
children. The root supervisor exclusively owns loop devices, verifies their
RAM-backed target and cleans up mounts/tracing on normal exit and failure.
Configured budgets stayed at 2 GiB scratch / 4 GiB job; those are recorded
provisioning values, not filesystem or runner invariants. The observed VM was
Linux 6.19.10 x86_64 with 16 vCPUs and 32 GiB RAM; affinity was not pinned and
caches were not dropped. No build/test/sanitizer workload overlapped a matrix.

Memory peaks are 312,037,376 / 329,269,248 before and 312,684,544 / 312,774,656
after bytes. All have zero memory max/OOM events and zero swap. Trace loss or
budget overflow invalidates a run; all cases completed through the existing
validated trace/parser boundary. Each persisted result is below the existing
64 KiB summary cap; trace data remains bounded RAM storage and is not retained.
Native filesystem configuration, mkfs versions, durability boundaries and
operation-vs-batch counters are in the record and unchanged
[comparison contract](ram-validation.md#small-comparison-contract).
`after_end_seconds` includes verification/process handoff/teardown, not isolated
unmount timing.

All 134 quick groups and six extended groups pass, with zero swap/OOM events.
The final production revision also passes all 134 quick and six extended groups
under ASan/UBSan (leak detection and halt-on-error), using the existing temporary
sanitizer environment wrapper and unchanged scoped launcher. Sanitizer peaks are
496,148,480 / 533463040 bytes. The ordinary extended run preceded the final
error-classification correction; the final sanitizer extended run covers it.
The complete shared archive cross-compiles with Pyxis GCC 16.2.0, kernel flags
and compiler freestanding headers only. Native tools build with the suite.
Exact-head filesystem and Pyxis CI are reported on their PRs.

Contract-focused tests retain
independent expected intervals and contents, canonical/corruption checks,
trace-derived faults, retained payload protection during maintenance, cross-volume
carryover, final release/checkpoint/startup and no-extra-allocation funded work.
Calculated envelopes are separate from measured memory peaks. Passing this
bounded suite and comparison does not qualify sustained writes, larger or heavily
fragmented populations, resource pressure, real-host post-error recovery or native
writable deployment. Task 7 and the writable-deployment blocker remain open.
