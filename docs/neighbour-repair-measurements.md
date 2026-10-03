# Occupancy-aware neighbour repair

**Historical: retired COW filesystem.** The implementation, host tools and test
suite described here were removed when Caelum adopted the native format. Commands
and APIs below are obsolete; they are not current validation or interfaces. The
[source snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9)
preserves the original implementation. Use the [native format](native-format.md)
and [native host tools](native-host-tools.md) for current behavior.

After filesystem #23 and Pyxis #324 merged, the owner assigned the previously
proposed bounded repair correction. The writer scores both immediate clean
neighbours using the complete expanded candidate run, including an already-marked
run bridged through that neighbour. It prefers fit/smaller interval deficit with
left ties, then marks the chosen leaf and ancestors and renews accounting/seams.
The [implemented proof](incremental-map.md#closure-and-accounting) retains the
ascending eligible-ID prefix, both retained states, no same-publication reuse,
funded bulk fallback and individually durable operations.

All 54 after cases complete and independently verify. Matched traffic changes
range from a 6.141% reduction to a 1.934% increase; this is a mixed intermediate
result. The [bounded record](measurements/neighbour-repair.json) retains both
repetitions, fresh before control, validation summaries and storage evidence.

No disk format, topology/fanout, bulk/local packing, allocation placement,
reserve/admission policy, memory ceiling, caller budget or runner setting changes.
The selector adds constant scalars and fixed diagnostic counters. The publication
scratch-size assertion remains in force; no new arena region or allocation after
admission is required. General splits/merges remain outside this correction.

## Baseline and matched method

The baseline is merged filesystem `1e5bb76`, containing published `fff90d8`.
The [sustained record](sustained-map-measurements.md) already contains two serial
repetitions of each population/case on Pyxis, ext4 and Btrfs. Those recorded
comparisons use `3e9f378` / `b3c7ac6`, whose production core matches published
`fff90d8`; they are not a new full before rerun. Before editing the selector,
a fresh largest-append control ran unchanged on all three backends.
Every backend completed and independently verified; Pyxis reproduced exactly
909,623,296 submitted bytes, including all trailing maintenance. Native totals
retain their observed variation rather than being treated as fixed expectations.

The after comparison uses `0f808dd`, containing production correction `9810536`
and its focused tests. It repeats the unchanged configured matrix: 64 background
files with total separately durable 4 KiB histories of 256/2048/5120 blocks,
three equal append/overwrite/compiler windows, two serial repetitions on all
three filesystems, fresh 1 GiB images, unchanged E/M, reserves and core-owner cap.
Setup, preparation, successive windows and final fencing remain separate; total
bytes include all maintenance and native verification/handoff/unmount traffic.
No window checkpoint, delayed durability or batching across completed calls is
introduced. These settings and counters are observations/configuration, not
filesystem invariants or acceptance thresholds.

```sh
sudo -n python3 tests/ram_run.py --suite sustained \
  --background-blocks 5120 --case append
```

Choose each of the existing population/case combinations and run twice serially.
The compiler, actual build flags, native profiles and RAM controls are retained
with the summaries. No other local build/test/sanitizer job overlaps the matrix.
Instrumented timing includes adapter reads and optional bulk-reference counting;
it is not isolated CPU time or NVMe performance. Added diagnostic accumulation
is part of the after timing. Byte totals are actual submitted writes, not estimates.

## Matched complete-history submitted writes

All 54 after cases complete and independently verify. Pyxis byte totals repeat
exactly for each history; native ranges retain both samples. MiB includes setup,
preparation, every window, final fencing and native post-end submissions.
The before column uses the recorded complete baseline matrix; the fresh
largest-append control supports comparability without replacing that record.

| Background MiB | Case | Before Pyxis MiB | After Pyxis MiB | Change | After Btrfs MiB | After ext4 MiB |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | append | 158.03 | 159.05 | +0.645% | 253.64 | 92.87 |
| 1 | overwrite | 199.27 | 187.04 | -6.141% | 243.50 | 70.52–70.56 |
| 1 | compiler | 86.89 | 86.89 | +0.000% | 242.54 | 97.37 |
| 8 | append | 362.60 | 360.57 | -0.560% | 470.88–470.95 | 134.88 |
| 8 | overwrite | 409.18 | 401.05 | -1.985% | 471.45–471.79 | 112.53–112.57 |
| 8 | compiler | 276.72 | 273.62 | -1.119% | 468.52 | 139.38 |
| 20 | append | 867.48 | 855.83 | -1.343% | 851.84–851.90 | 206.90 |
| 20 | overwrite | 887.41 | 904.57 | +1.934% | 852.40–852.43 | 184.56–184.83 |
| 20 | compiler | 686.78 | 683.77 | -0.438% | 849.10–849.76 | 211.39 |

### Pyxis phase/accounting split

After MiB; all submitted writes. Data plus metadata and user plus orphan
plus drain each independently sum to the complete-history total. Reclamation
inside a user publication stays in the user group. Native splits are not
inferred. Setup is 0.03125 MiB; preparation/final are explicit below and
successive windows follow. Useful completed application bytes, before and
after, are 7/6.5/2.5 MiB at 1 MiB background; 14/13.5/9.5 at 8; and
26/25.5/21.5 at 20 for append/overwrite/compiler respectively.

| Background MiB | Case | Prep MiB | Final MiB | Data MiB | Metadata MiB | User MiB | Orphan MiB | Drain MiB |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | append | 13.28 | 0.08 | 7.00 | 152.05 | 158.92 | 0.00 | 0.13 |
| 1 | overwrite | 17.99 | 0.09 | 14.00 | 173.04 | 186.90 | 0.00 | 0.13 |
| 1 | compiler | 13.20 | 0.05 | 2.50 | 84.39 | 74.11 | 12.68 | 0.10 |
| 8 | append | 180.46 | 0.05 | 14.00 | 346.57 | 360.45 | 0.00 | 0.11 |
| 8 | overwrite | 185.93 | 0.18 | 21.00 | 380.05 | 400.66 | 0.00 | 0.39 |
| 8 | compiler | 180.39 | 0.06 | 9.50 | 264.12 | 256.90 | 16.61 | 0.12 |
| 20 | append | 595.64 | 0.05 | 26.00 | 829.83 | 855.72 | 0.00 | 0.11 |
| 20 | overwrite | 603.37 | 0.20 | 33.00 | 871.57 | 904.20 | 0.00 | 0.38 |
| 20 | compiler | 595.60 | 0.07 | 21.50 | 662.27 | 667.88 | 15.77 | 0.12 |

## Successive windows and closure

Each cell shows before → after. Window bytes include active user/orphan/drain
work; the bounded record retains preparation, final phases and all data/metadata
splits. These window comparisons exclude preparation and final fencing; those
costs remain in the preceding complete-history table.

| Background MiB | Case | W1 MiB | W2 MiB | W3 MiB | Window sum(q)/sum(J) |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | append | 34.23 → 34.23 | 47.58 → 47.58 | 62.83 → 63.85 | 0.306 → 0.310 |
| 1 | overwrite | 45.18 → 44.91 | 57.77 → 52.70 | 78.22 → 71.32 | 0.464 → 0.419 |
| 1 | compiler | 23.61 → 23.61 | 24.00 → 24.00 | 26.00 → 26.00 | 0.230 → 0.230 |
| 8 | append | 49.77 → 47.49 | 66.11 → 68.36 | 66.22 → 64.18 | 0.191 → 0.188 |
| 8 | overwrite | 86.36 → 84.75 | 76.73 → 69.86 | 59.75 → 60.31 | 0.218 → 0.207 |
| 8 | compiler | 31.12 → 31.00 | 31.67 → 31.14 | 33.50 → 31.00 | 0.065 → 0.060 |
| 20 | append | 69.54 → 100.33 | 84.56 → 68.09 | 113.80 → 91.69 | 0.194 → 0.187 |
| 20 | overwrite | 111.14 → 123.30 | 109.25 → 112.03 | 61.70 → 65.65 | 0.192 → 0.203 |
| 20 | compiler | 28.50 → 28.88 | 28.72 → 29.20 | 30.00 → 30.00 | 0.029 → 0.031 |

The following after counts cover preparation, windows and final fencing.
They are measured results, not required publication counts. Setup formatter
callbacks stay separate. Bulk reason flags can overlap and are not a partition
of bulk publications; the record retains global/overflow/underflow/resource
flags separately.

| Background MiB | Case | Publications | Publication flushes | Bulk prep / windows / final | Total map-node writes |
| ---: | --- | ---: | ---: | --- | ---: |
| 1 | append | 1861 | 3722 | 48 / 19 / 0 | 26424 |
| 1 | overwrite | 1877 | 3754 | 49 / 11 / 0 | 30117 |
| 1 | compiler | 2244 | 4488 | 48 / 0 / 0 | 7984 |
| 8 | append | 3653 | 7306 | 66 / 3 / 0 | 66253 |
| 8 | overwrite | 3669 | 7338 | 66 / 3 / 0 | 72677 |
| 8 | compiler | 4036 | 8072 | 66 / 0 / 0 | 44027 |
| 20 | append | 6725 | 13450 | 73 / 2 / 0 | 171345 |
| 20 | overwrite | 6741 | 13482 | 73 / 2 / 0 | 179874 |
| 20 | compiler | 7108 | 14216 | 73 / 0 / 0 | 127329 |

## Repair observations

One repetition per history; repeated counters remain observations. All observed
selections are overflow repairs. Compared/right-preferred/ties refer to two-sided
choices; predicted fits include forced one-sided choices and are not successful
publications. Counters include work later discarded for funded bulk fallback.

| Background MiB | Case | Selections | Two-sided | Right preferred | Ties | Predicted fit |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | append | 18196 | 6 | 2 | 3 | 1738 |
| 1 | overwrite | 13736 | 5444 | 448 | 3713 | 1923 |
| 1 | compiler | 444 | 0 | 0 | 0 | 162 |
| 8 | append | 49488 | 8538 | 41 | 8421 | 2346 |
| 8 | overwrite | 35637 | 1854 | 163 | 1300 | 2354 |
| 8 | compiler | 22723 | 7 | 1 | 3 | 1911 |
| 20 | append | 137833 | 66296 | 205 | 65430 | 3407 |
| 20 | overwrite | 123978 | 56319 | 430 | 53090 | 3487 |
| 20 | compiler | 91419 | 32658 | 123 | 32252 | 2961 |

## Instrumented RAM timing and residual cost

Window elapsed includes the operations in all three windows. Two-sample ranges
are observations, not significance estimates. Planning includes adapter reads,
canonical editing, accounting and optional bulk-reference work; these records
do not isolate a routine or establish a local planning bound.

| Background MiB | Case | Before window seconds | After window seconds | After planning seconds | After map / metadata blocks |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | append | 37.67–37.74 | 37.71–37.72 | 29.70–29.71 | 70.02% |
| 1 | overwrite | 39.95–40.13 | 38.96–39.14 | 26.39–26.47 | 69.94% |
| 1 | compiler | 17.58–17.66 | 17.54–17.56 | 10.14 | 35.82% |
| 8 | append | 73.72–73.84 | 72.44–72.46 | 64.16–64.18 | 75.94% |
| 8 | overwrite | 80.14–80.49 | 79.60–79.63 | 65.98–66.02 | 75.79% |
| 8 | compiler | 66.46–66.69 | 66.28–66.40 | 58.04–58.13 | 49.50% |
| 20 | append | 123.34–123.65 | 121.08–121.48 | 112.44–112.76 | 83.52% |
| 20 | overwrite | 125.96–126.95 | 127.97–128.24 | 113.54–113.75 | 82.92% |
| 20 | compiler | 111.81–111.96 | 111.34–111.48 | 102.34–102.45 | 46.54% |

## What the comparisons establish

The fit/deficit selector is bounded and preserves the publisher's existing proof;
its score describes the current expanded run before renewed self-accounting.
It does not predict the lifetime cost of the resulting topology. Changed repair
choices also change later map histories, even in windows with no new repairs.
The measured regressions are retained rather than hidden in an aggregate average.
No uniform write saving, optimal side choice or deployment acceptance follows.

The largest overwrite increases 17,997,824 submitted bytes (1.934%). Emitted map
nodes increase by 4,350, accounting for 17,817,600 bytes, or 99.0% of that net
increase. W1 contributes most; W2 adds a global bulk fallback. W3 selects no
repair neighbours but emits more map nodes with source inventory J=211 instead
of the baseline 196. These observations establish history propagation, not a
causally wrong individual choice. The largest compiler reduces complete-history
bytes while W1/W2 become more expensive, because preparation improves more.

Source maps reach J=211 in the after matrix. Every observed local emission is
smaller than its calculated bulk reference, but map replacements still account
for about 70–84% of append/overwrite window metadata. Broad local replacement
and repeated full-population planning remain significant.
Current local encoding already distributes records evenly across its marked run;
greedy local packing is not an explanation supported by code or these records.
A later improvement should address the measured closure/self-accounting or
planning cost with its own bounded proposal and matched validation. No additional
mechanism, packing/placement change or campaign is assigned here.

## Validation

All 141 quick groups and six extended groups pass, normally and with ASan/UBSan.
The six added cases deliberately configure fit, smaller deficit, tie and bridging
conditions in both failure directions. They check the chosen/shared source
identities at the first repair, before subsequent accounting may legitimately
expand the set. The independently generated per-block candidate and encoded-tree
walk then verify contents, canonical coverage, sharing, fixed eligible prefix,
funding, immutable source bytes and bounded monotone renewal. They do not require
workload publication counts, current formatter tree shapes or incidental physical
allocation placement.

Ordinary unprivileged `make -j16` builds all host tools, and the complete shared
archive cross-compiles with Pyxis GCC 16.2, compiler-only headers and kernel
freestanding/ABI flags. This checks compilation, not native writable runtime or
the deferred kernel-stack prerequisite.

A small sanitized configurable overwrite comparison also completes and verifies
all three backends with the added diagnostic fields. Existing maintained tests
retain recovery, older-payload protection through replacement writes, authority,
lifetime, failure provenance and funded cleanup coverage. The large old recovery
campaign is not repeated; this task does not qualify a native writable core.

## Storage and interpretation limits

All mutation storage uses the existing verified bounded `tmpfs,noswap` setup and
zero-swap member cgroup, exclusive owned loop devices, actual-target identity
guard, bounded trace/output and no disk fallback. Test/compilation workers remain
unprivileged; storage/tracing setup retains scoped privilege. Trace loss or budget
overflow, OOM and execution failure invalidate a sample. Healthy refusal is an
incomplete verified prefix, not workload completion; limits are never raised to
obtain completion.

The configured launcher retains 2 GiB scratch / 65,536 inodes and a 4 GiB
zero-swap member limit for these samples. Observed member peaks range from
289,665,024 to 337,788,928 bytes, with zero memory/OOM events. Pyxis charged core
peak is 31,272,976 bytes, 88 bytes above the old header accounting; arena size and
caller cap are unchanged. Maximum trace input is 4,378,964 bytes for ext4 and
7,662,639 for Btrfs, with no discarded writes or invalidation. Btrfs case-end
backing/scratch maxima are 379,273,216 / 401,436,672 bytes; these are snapshots,
not peaks. No owned loop devices remain after the matrix. Each summary is under
15 KiB; raw storage/traces remain in RAM and only bounded records persist.

Member-cgroup peaks remain incomplete native whole-job RAM evidence, as explained
in the [baseline resource observations](sustained-map-measurements.md#resource-estimate-and-safety-boundary).
Scratch and trace bounds are independent, and conservative native cache/kernel
overhead stays in the estimate. Configuration changes do not require changing
independent validators. Real-host recovery qualification and the native writer's
kernel-stack prerequisite remain deferred. Task 7 and writable deployment stay
open, independently of any measured write savings.
