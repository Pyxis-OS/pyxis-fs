# Indexed map planning and phase-separated RAM observations

**Historical: retired COW filesystem.** The implementation, host tools and test
suite described here were removed when Caelum adopted the native format. Commands
and APIs below are obsolete; they are not current validation or interfaces. The
[source snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9)
preserves the original implementation. Use the [native format](native-format.md)
and [native host tools](native-host-tools.md) for current behavior.

Filesystem #21 and Pyxis #314 were confirmed merged before this follow-up.
Two unchanged 40-case matrices at filesystem `36ff184` precede implementation;
two after matrices use `cf55a44`. Later documentation commits do not change the
measured code. The [bounded record](measurements/map-planning.json) retains all
four launcher summaries, native comparisons, verification and storage evidence.

Command: `sudo -n python3 tests/ram_run.py --suite baseline`. The existing matrix
uses 32/256-file populated histories and separately durable 4 KiB/256 KiB writes,
small overwrites and compiler-style create/write/rename/remove. No workload,
placement, admission, reserve, memory, durability or runner policy changed.
These benchmark parameters are recorded configuration, not filesystem invariants.
Ordinary matrix builds use Make's `HOST_CC=cc` and `CFLAGS=-O2 -g3`, with the
existing GNU C23 warnings and freestanding core flags. No compiler/flag overrides
were used. The host compiler identity was checked after the matrices:
`cc (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)`. Sanitizer builds separately use
`-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer`; they do not overlap
the matrices or supply their timing samples. Native formatter/mount settings
and versions are retained in each recorded native profile.

## Submitted writes

Every Pyxis byte total, phase callback counter and selected-state summary agrees
across all four samples. Recombining the new phase-specific map counters also
reproduces the before aggregate counters. This lookup/diagnostic change saves no
submitted bytes; the earlier topology-preserving savings remain intact.

KiB covers the measurement window through trailing maintenance and final
synchronization, including native post-end submissions. The full-history column
also includes preparation. Native operation ranges use all four current samples.

| Files | Case | Pyxis measurement, before = after | Pyxis full history, before = after | ext4 operation | Btrfs operation |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 | 4 KiB writes | 2,860 | 5,636 | 1,564 | 4,904 |
| 32 | 256 KiB writes | 4,720 | 7,496 | 4,444 | 5,708 |
| 32 | Small overwrite | 1,512 | 4,576 | 248 | 2,720 |
| 32 | Compiler history | 3,056 | 5,792 | 1,772 | 5,864 |
| 256 | 4 KiB writes | 3,044 | 25,192 | 1,568 | 5,000 |
| 256 | 256 KiB writes | 4,808 | 26,956 | 4,448 | 5,804 |
| 256 | Small overwrite | 1,640 | 24,076 | 252–264 | 2,816 |
| 256 | Compiler history | 3,120 | 25,228 | 1,776–1,848 | 5,896 |

Measurement data KiB are 256 / 4,096 / 216 / 64 in case order for both
populations. User metadata KiB are 2,556 / 576 / 1,244 / 2,432 at 32 files and
2,740 / 664 / 1,372 / 2,496 at 256. Compiler orphan publications write 512 KiB;
terminal fence metadata is 48 / 48 / 52 / 48 KiB. These costs include reclamation
carried by active user/orphan publications. Final volume debt is zero; bounded
idle pool debt remains valid under the existing contract.

Native 16-operation batch rows remain in the record with their different
acknowledgment boundary; Pyxis has no matched batched mode. Preserve the
[comparison contracts](ram-validation.md#small-comparison-contract): submitted
block writes are not physical NVMe traffic, and equivalent synchronization does
not make the filesystems' retained-state/recovery contracts identical.

## Phase attribution

After records store one `map_plans` object with
`phase_order = ["preparation", "measurement"]`. Scalar fields are pairs in that
order; existing array fields are pairs of arrays. Every prior diagnostic remains,
without another serialized total or duplicate field names. Add additive fields
and take maxima for maximum fields when reconstructing combined observations;
select the last nonempty failed run rather than adding its dimensions. Fallback
reasons can overlap. Counts below repeat across both after samples and are
observations, never required publication counts.

| Files | Case | Preparation local/all | Global fallbacks prep/measure | Measurement local/all | Measurement J/q max | Measurement emitted/bulk-reference map nodes |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 32 | 4 KiB writes | 0/91 | 91/49 | 17/66 | 5/5 | 254/274 |
| 32 | 256 KiB writes | 0/91 | 91/15 | 3/18 | 4/3 | 55/58 |
| 32 | Small overwrite | 0/92 | 92/32 | 2/34 | 4/4 | 126/128 |
| 32 | Compiler history | 0/90 | 90/82 | 0/82 | 3/3 | 246/246 |
| 256 | 4 KiB writes | 368/539 | 171/1 | 65/66 | 10/9 | 300/672 |
| 256 | 256 KiB writes | 368/539 | 171/0 | 18/18 | 9/8 | 77/174 |
| 256 | Small overwrite | 369/540 | 171/1 | 33/34 | 10/9 | 158/344 |
| 256 | Compiler history | 367/538 | 171/0 | 82/82 | 9/3 | 246/738 |

This confirms preparation accounts for most global fallbacks at 256 files in
these histories. Each case's coincident exhausted-overflow fallback also occurs
in preparation. Measurement includes final maintenance/checkpoints, not just user
calls; high local-hit frequency alone does not prove savings or scaling. The
32-file compiler case still uses bulk for every observed measurement publication.
All source maps remain at most ten nodes; substantially larger populated and
fragmented maps require separate qualification under the existing RAM safeguards.

The comparison checks phase publication attribution against independently
observed fixed-slot writes and the deliberate two-flush protocol. Callback flush
totals also match deltas of the adapter's monotonic flush ordinals. No publication
may span the phase switch. Startup precedes observer installation; each phase
includes its terminal fence, and measurement includes writer close. Verification
disables the observer before read-only reopening. Active publication work, rather
than debt presence, still classifies user/orphan/maintenance bytes.

## Timing and calculated cost

Primary measurement-window seconds below are means of two samples per revision;
the record preserves each sample. No speedup is established on these small maps.

| Files | Case | Before mean seconds | After mean seconds | Change | After measurement planning interval seconds |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 | 4 KiB writes | 0.315202 | 0.315430 | +0.07% | 0.133012–0.133086 |
| 32 | 256 KiB writes | 1.091174 | 1.093819 | +0.24% | 0.031730–0.031885 |
| 32 | Small overwrite | 0.209414 | 0.212239 | +1.35% | 0.065478–0.065869 |
| 32 | Compiler history | 0.349849 | 0.350053 | +0.06% | 0.129261–0.129399 |
| 256 | 4 KiB writes | 1.143648 | 1.152497 | +0.77% | 0.254686–0.255981 |
| 256 | 256 KiB writes | 1.498140 | 1.504530 | +0.43% | 0.068503–0.069300 |
| 256 | Small overwrite | 0.659579 | 0.664226 | +0.70% | 0.131705–0.131870 |
| 256 | Compiler history | 0.986682 | 0.993801 | +0.72% | 0.280536–0.281235 |

These are instrumented RAM observations with modest increases and too few small
samples to establish a significant latency trend. The planning interval starts
at the first publisher backing read and ends at its first replacement-write
callback; source/adapter reads and optional bulk-reference counting are included.
No independent timer isolates index construction or diagnostic overhead. Before
planning counters combine both phases, so they cannot supply a matched
measurement-only planning ratio. `after_end_seconds` includes verification,
handoff and teardown rather than isolated unmount time. No NVMe performance or
population-independent planning bound is claimed.

Calculated membership work changes from O(J²) per accounting pass / O(J³) across
bounded growth to O(J log J) / O(J² log J). The index costs O(J log J) to construct
once with constant extra sorting storage; sealing membership is O(J log J).
Source-load duplicate detection remains O(J²) once, and full traversal, vector
editing and admission retain population costs. This improves a known worst-case
component before larger qualification; these measurements do not establish its
benefit at larger J or eliminate global closure.

## Storage safety and validation

All four matrices used the unchanged scoped launcher: bounded tmpfs with
`noswap`, zero-swap cgroup, no disk fallback, unprivileged compilation/workloads
and root-supervised exclusive loop/mount/tracing setup with cleanup. Trace loss,
output/resource exhaustion or submitted/completed-byte mismatch invalidates a
run. All 40 cases in each matrix completed and independently verified their
ledger-defined namespace/contents after read-only reopening; refusal did not
count as completion. No build/test/sanitizer workload overlapped a matrix.

Recorded provisioning is 2 GiB scratch, 65,536 inodes and 4 GiB job memory, with
zero swap/OOM events. These are selected run settings, not validators' required
constants. Matrix memory peaks are 339,820,544 / 325,197,824 bytes before and
303,804,416 / 304,619,520 after. Summary sizes are 59,604 / 59,613 / 60,922 /
60,919 bytes, all under the unchanged 64 KiB guard. Only bounded summaries persist;
raw traces remain in RAM and are not retained. The observed VM has Linux 6.19.10,
16 online vCPUs and 32 GiB RAM; no affinity pinning or cache dropping was used.

All 135 quick and six extended groups pass, including the same suites under
ASan/UBSan with leak detection and halt-on-error. Ordinary quick/extended peaks
are 318,570,496 / 494,833,664 bytes; sanitizer peaks are 521,703,424 / 534,142,976.
All have zero swap/OOM. The complete shared archive cross-compiles with Pyxis
GCC 16.2.0 and kernel flags, using compiler freestanding headers. Existing suites
retain funded cleanup, cross-volume histories, retained payloads, authority and
sticky recovery failures; the new case covers shuffled source IDs/shared topology
and the expanded source-read failure case covers incomplete source loading.
Exact-head CI is reported on the PRs. No guest run or real-host recovery
qualification is claimed.
Task 7 and writable deployment remain open.
