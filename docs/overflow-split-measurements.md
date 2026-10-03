# Bounded one-leaf overflow split: matched RAM observations

**Historical: retired COW filesystem.** The implementation, host tools and test
suite described here were removed when Caelum adopted the native format. Commands
and APIs below are obsolete; they are not current validation or interfaces. The
[source snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9)
preserves the original implementation. Use the [native format](native-format.md)
and [native host tools](native-host-tools.md) for current behavior.

After Pyxis #327 merged, the owner accepted and assigned one extra leaf under an
existing parent with room, before neighbour expansion. The
[implemented proof](incremental-map.md) separates source retirement p from emitted
n=p+1, requires final J+1<=m, renews accounting/seams and restores the seed once
on a specified miss. The restored path retains ordinary repair and funded bulk.
Read, integrity and guaranteed-resource failures remain errors. Both retained
states, individually durable calls, authority/lifetimes and failure provenance remain.

No internal split, root growth, merge, placement heuristic, bulk fill, reserve/
admission, memory ceiling, caller budget or runner change is included. This is an
intermediate correction, not deployment qualification or an optimal allocator.

## Baseline and matched method

The baseline is merged filesystem `8ddcdb7`, containing published `4b1e81d`.
The [neighbour-repair record](measurements/neighbour-repair.json) contains two
serial repetitions of each population/case/backend. Its measured `0f808dd`
contains the same production code; it supplies the full before matrix.
Before editing production, a fresh Pyxis-only largest-overwrite control completed
and independently verified at exactly 948,514,816 submitted bytes, matching the
recorded before result. This is not a new full before rerun or native control.
The fresh control and unchanged recorded matrix are retained in the
[bounded record](measurements/overflow-split.json).

After uses `d6175e8`, containing production/tests `64d4473`. All 54 backend cases
complete and independently verify names, lengths and bytes. The unchanged matrix
uses 64 background files with 256/2048/5120 separately durable 4 KiB histories,
three equal append/overwrite/compiler windows, two serial repetitions, fresh 1 GiB
images and unchanged E/M/reserves/caller budgets. Setup, preparation, windows and
final fencing remain separate. Total submitted bytes include all maintenance and
native verification/result-handoff/clean-unmount submissions. No window fence,
delayed acknowledgment or batching across completed calls is introduced.

```sh
sudo -n python3 tests/ram_run.py --suite sustained \
  --background-blocks 5120 --case overwrite
```

Run each existing population/case combination twice serially. These parameters,
observed counters and timings are configuration/results, not filesystem invariants
or numerical acceptance thresholds. Compiler commands, native mkfs/mount profiles
and actual RAM controls remain in the record. No other local build/test/sanitizer
job overlaps the matrix; no affinity pinning or cache dropping is introduced.
Equivalent operations and individual synchronization boundaries are compared;
recovery contracts are not identical. Native byte totals come from owned-loop
block-write submissions and match completed-loop accounting. Pyxis counts actual
data/metadata callbacks. See the [synchronization and storage contract](ram-validation.md).
Instrumented planning includes adapter reads and optional bulk-reference counting,
including failed trial/restoration work; it is not isolated CPU time or NVMe performance.

## Matched complete-history submitted writes

MiB includes every phase and native post-end submission. Pyxis totals repeat
exactly in these two samples; native ranges preserve their variation. Neither a
universal amplification ratio nor deployment acceptance follows.

| Background MiB | Case | Before Pyxis MiB | After Pyxis MiB | Change | After Btrfs MiB | After ext4 MiB |
| --- | --- | --- | --- | --- | --- | --- |
| 1.0 | append | 159.05 | 114.05 | -28.289% | 253.64 | 92.87 |
| 1.0 | overwrite | 187.04 | 144.52 | -22.729% | 243.59 | 70.49–70.53 |
| 1.0 | compiler | 86.89 | 93.06 | 7.098% | 242.54 | 97.36–97.37 |
| 8.0 | append | 360.57 | 211.37 | -41.378% | 470.85–470.88 | 134.88–134.94 |
| 8.0 | overwrite | 401.05 | 296.13 | -26.161% | 471.51–471.86 | 112.54–112.60 |
| 8.0 | compiler | 273.62 | 217.88 | -20.373% | 468.59 | 139.38 |
| 20.0 | append | 855.83 | 579.59 | -32.278% | 851.18–851.87 | 206.90 |
| 20.0 | overwrite | 904.57 | 612.78 | -32.257% | 851.93–852.78 | 184.55–184.66 |
| 20.0 | compiler | 683.77 | 443.04 | -35.207% | 849.26–849.82 | 211.39 |

## Pyxis phase/accounting split

After MiB, one repetition per history. Data+metadata and user+orphan+drain each
sum independently to the complete-history total. Reclamation inside a user
publication stays in the user group. Setup contributes 0.03125 MiB; preparation
and final are explicit. Native data/metadata splits are not inferred.
Useful bytes count completed application writes; physical data includes whole
replacement blocks for partial overwrites.

| Background MiB | Case | Useful MiB | Prep MiB | Final MiB | Data MiB | Metadata MiB | User MiB | Orphan MiB | Drain MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0 | append | 7.00 | 12.03 | 0.07 | 7.00 | 107.05 | 113.93 | 0.00 | 0.12 |
| 1.0 | overwrite | 6.50 | 16.66 | 0.09 | 14.00 | 130.52 | 144.38 | 0.00 | 0.14 |
| 1.0 | compiler | 2.50 | 11.99 | 0.06 | 2.50 | 90.56 | 78.81 | 14.14 | 0.11 |
| 8.0 | append | 14.00 | 117.24 | 0.06 | 14.00 | 197.37 | 211.24 | 0.00 | 0.13 |
| 8.0 | overwrite | 13.50 | 121.96 | 0.18 | 21.00 | 275.13 | 295.90 | 0.00 | 0.23 |
| 8.0 | compiler | 9.50 | 117.18 | 0.07 | 9.50 | 208.38 | 199.64 | 18.11 | 0.13 |
| 20.0 | append | 26.00 | 346.59 | 0.06 | 26.00 | 553.59 | 579.47 | 0.00 | 0.11 |
| 20.0 | overwrite | 25.50 | 351.37 | 0.17 | 33.00 | 579.78 | 612.55 | 0.00 | 0.23 |
| 20.0 | compiler | 21.50 | 346.55 | 0.11 | 21.50 | 421.54 | 425.86 | 17.01 | 0.16 |

## Successive windows and closure

Before → after. Window sums exclude preparation/final, which remain included in
the preceding totals. Source closure p includes ancestors; sum(p)/sum(J) is a
weighted diagnostic, not a locality guarantee or success criterion.

| Background MiB | Case | W1 MiB | W2 MiB | W3 MiB | Window sum(p)/sum(J) |
| --- | --- | --- | --- | --- | --- |
| 1.0 | append | 34.23 → 24.07 | 47.58 → 44.14 | 63.85 → 33.70 | 0.310 → 0.135 |
| 1.0 | overwrite | 44.91 → 37.04 | 52.70 → 43.96 | 71.32 → 46.73 | 0.419 → 0.194 |
| 1.0 | compiler | 23.61 → 26.00 | 24.00 → 26.48 | 26.00 → 28.50 | 0.230 → 0.167 |
| 8.0 | append | 47.49 → 24.88 | 68.36 → 25.33 | 64.18 → 43.83 | 0.188 → 0.062 |
| 8.0 | overwrite | 84.75 → 51.07 | 69.86 → 60.98 | 60.31 → 61.91 | 0.207 → 0.149 |
| 8.0 | compiler | 31.00 → 33.49 | 31.14 → 33.62 | 31.00 → 33.50 | 0.060 → 0.068 |
| 20.0 | append | 100.33 → 25.47 | 68.09 → 105.17 | 91.69 → 102.27 | 0.187 → 0.163 |
| 20.0 | overwrite | 123.30 → 92.98 | 112.03 → 106.37 | 65.65 → 61.86 | 0.203 → 0.173 |
| 20.0 | compiler | 28.88 → 26.00 | 29.20 → 29.85 | 30.00 → 40.50 | 0.031 → 0.038 |

## Trial observations

Counts cover preparation, windows and final fencing, one repetition. Opportunities
are first failing overflow runs where the trial can be considered. Named skips
and misses can overlap; the sequence in each cell is shown in its header.
Discarded source marks/growth are planning work, not committed retirements.
These observations do not prescribe publication counts or physical placement.

| Background MiB | Case | Opportunities | Trials | Selected | Skips root/parent/cap/global | Misses unneeded/insufficient/other/global/resource | Discarded source marks | Discarded growth |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0 | append | 743 | 93 | 93 | 0/650/0/0 | 0/0/0/0/0 | 0 | 0 |
| 1.0 | overwrite | 569 | 63 | 62 | 0/506/0/0 | 0/0/1/0/0 | 6 | 0 |
| 1.0 | compiler | 23 | 23 | 23 | 0/0/0/0 | 0/0/0/0/0 | 0 | 0 |
| 8.0 | append | 928 | 123 | 122 | 0/805/0/0 | 1/0/0/0/0 | 4 | 0 |
| 8.0 | overwrite | 738 | 127 | 122 | 0/611/0/0 | 1/0/4/0/0 | 68 | 1 |
| 8.0 | compiler | 681 | 93 | 93 | 0/588/0/0 | 0/0/0/0/0 | 0 | 0 |
| 20.0 | append | 2218 | 159 | 156 | 0/2059/0/0 | 3/0/0/0/0 | 14 | 0 |
| 20.0 | overwrite | 2321 | 159 | 156 | 0/2162/0/0 | 3/0/0/0/0 | 14 | 0 |
| 20.0 | compiler | 1649 | 150 | 147 | 0/1499/0/0 | 3/0/0/0/0 | 14 | 0 |

## Chosen paths, costs and inventory

All non-formatter publications. Bulk reason flags can overlap. Local cost compares
emitted n with the calculated bulk construction for the same original logical work;
retired p cannot stand in for write cost. This is an immediate-node reference, not
an alternative complete history or a path-selection policy. Source max J and
actual retirements remain separate from emissions.

| Background MiB | Case | Publications | Publication flushes | Bulk prep/windows/final | Bulk reasons global/overflow/underflow/resource | Map nodes written | Map nodes retired | Max source J | Local less/equal/more than bulk |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0 | append | 1861 | 3722 | 35 / 1 / 0 | 36/0/0/0 | 14906 | 14802 | 105 | 1824/1/0 |
| 1.0 | overwrite | 1877 | 3754 | 35 / 0 / 0 | 35/0/0/0 | 19234 | 19169 | 66 | 1841/1/0 |
| 1.0 | compiler | 2244 | 4488 | 35 / 0 / 0 | 35/0/0/0 | 9563 | 9537 | 27 | 2208/1/0 |
| 8.0 | append | 3653 | 7306 | 34 / 0 / 0 | 34/0/0/0 | 28059 | 27927 | 133 | 3618/1/0 |
| 8.0 | overwrite | 3669 | 7338 | 34 / 0 / 0 | 34/0/0/0 | 45789 | 45657 | 133 | 3634/1/0 |
| 8.0 | compiler | 4036 | 8072 | 34 / 0 / 0 | 34/0/0/0 | 29756 | 29653 | 104 | 4001/1/0 |
| 20.0 | append | 6725 | 13450 | 35 / 1 / 0 | 36/1/0/0 | 100626 | 100429 | 198 | 6688/1/0 |
| 20.0 | overwrite | 6741 | 13482 | 35 / 1 / 0 | 36/1/0/0 | 105227 | 105030 | 198 | 6704/1/0 |
| 20.0 | compiler | 7108 | 14216 | 35 / 0 / 0 | 35/1/0/0 | 65700 | 65529 | 172 | 7072/1/0 |

## Instrumented RAM timing and residual cost

Window elapsed/planning ranges retain both samples; these are observations without
a significance estimate. Metadata shares cover only measurement windows. Chosen
source closure excludes restored trial work; closure evaluations include all phases
of planning and are a different diagnostic. Full source/claim validation remains
population-sized, and global closure remains possible.

| Background MiB | Case | Before window seconds | After window seconds | After planning seconds | After map / window metadata | Window closure evaluations | Max chosen source closure |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0 | append | 37.71–37.72 | 36.84 | 28.83–28.84 | 56.34% | 10098 | 66 |
| 1.0 | overwrite | 38.96–39.14 | 39.18–39.24 | 26.37–26.40 | 59.52% | 4369 | 24 |
| 1.0 | compiler | 17.54–17.56 | 20.61–20.62 | 12.88 | 41.77% | 3841 | 5 |
| 8.0 | append | 72.44–72.46 | 67.57–68.21 | 57.52–58.03 | 52.44% | 8172 | 76 |
| 8.0 | overwrite | 79.60–79.63 | 75.51–75.61 | 61.87–61.92 | 69.71% | 5096 | 66 |
| 8.0 | compiler | 66.28–66.40 | 67.19–67.40 | 58.97–59.10 | 53.30% | 3841 | 66 |
| 20.0 | append | 121.08–121.48 | 119.01–119.37 | 110.25–110.62 | 81.55% | 42916 | 192 |
| 20.0 | overwrite | 127.97–128.24 | 122.37–122.74 | 108.13–108.44 | 80.30% | 27735 | 181 |
| 20.0 | compiler | 111.34–111.48 | 110.83–112.08 | 102.06–103.13 | 51.21% | 4834 | 133 |

### Full phase timing

These sums include setup, preparation, all windows and final fencing. They exclude
Pyxis independent verification/process teardown and launcher compilation/setup;
native auxiliary after-end timings include verification and process handoff as
well as unmount. They are not isolated unmount or disk-device timings.

| Background MiB | Case | Before full phase seconds | After full phase seconds | After preparation seconds | After final seconds |
| --- | --- | --- | --- | --- | --- |
| 1.0 | append | 39.77–39.78 | 39.01–39.02 | 2.09–2.10 | 0.073 |
| 1.0 | overwrite | 42.17–42.36 | 42.54–42.59 | 3.30–3.31 | 0.051 |
| 1.0 | compiler | 19.55–19.57 | 22.74 | 2.08–2.10 | 0.027 |
| 8.0 | append | 118.93 | 111.32–111.90 | 43.58–43.64 | 0.099–0.101 |
| 8.0 | overwrite | 128.10–128.13 | 121.20–121.36 | 45.58–45.63 | 0.105 |
| 8.0 | compiler | 112.72–112.89 | 111.15–111.52 | 43.67–44.25 | 0.076 |
| 20.0 | append | 334.41–334.49 | 312.69–313.28 | 193.14–194.12 | 0.144–0.173 |
| 20.0 | overwrite | 343.45–344.49 | 319.24–319.70 | 196.72–196.81 | 0.148–0.149 |
| 20.0 | compiler | 324.15–324.43 | 305.86–306.03 | 193.66–195.08 | 0.125–0.126 |

## What the comparisons establish

Complete-history submitted writes improve in eight histories by 20.373–41.378%.
The small compiler history increases 7.098%. All nine Pyxis totals are below the
matched Btrfs totals in these samples; ext4 remains useful context and is generally
cheaper. These bounded histories neither define a universal ratio nor satisfy
broader deployment/pressure and recovery qualification.

The small compiler increase is exactly 6,467,584 bytes, matching 1,579 additional
map-node writes. Preparation is cheaper, but its retained map grows from 15 nodes
to 26–27 and later windows replace more nodes. All its local emissions are below
the calculated same-publication bulk reference except one equality. An immediate
local cost win therefore does not establish a complete-history win, and an
immediate-node cost gate alone would not explain away this history effect.

The largest append reduces complete-history traffic 32.278%, mostly through
preparation and W1; W2 rises from 68.09 to 105.17 MiB and W3 from 91.69 to
102.27 MiB. The largest compiler improves 35.207% overall while W3 rises from
30.00 to 40.50 MiB. The 8 MiB compiler also has cheaper preparation and more
expensive windows. These exceptions remain visible rather than being hidden in
an overall average. The source inventories, discarded work and callback totals
are observations, not an optimal-placement or causal individual-choice proof.

Full-parent skips dominate opportunities; workload misses are unnecessary splits
after renewed coalescing or another failing run. The other skip/miss classes are
covered by focused tests, not demonstrated by this matrix. General internal
splits/merges and a new fill or placement policy are not introduced.

Largest append/overwrite map writes still contribute 81.55%/80.30% of window
metadata, with broad closures and many renewal evaluations. Planning consumes
most window elapsed time and does not fall proportionally with submitted bytes.
Both full source validation and closure/self-accounting remain significant;
these records do not isolate which routine should be changed next. Any subsequent
optimisation needs a separately assigned bounded proposal and matched evidence.

## Validation and storage evidence

All 153 quick groups and six extended groups pass normally and under ASan/UBSan,
through the existing scoped RAM launcher. Sanitized runs use
`-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer`; a temporary local
Makefile default selects those flags and is restored before measurements. No
launcher, production build default or budget change is committed.
Unprivileged `make -j16` builds host tools. The complete archive cross-compiles
with Pyxis GCC 16.2, compiler-only headers and kernel freestanding/ABI flags.
This is compilation evidence, not native writable runtime or stack qualification.

The [test coverage](testing.md#bounded-one-leaf-overflow-trials) independently
checks canonical intervals, mixed-tree reachability/sharing/minima, selected and
discarded candidates, real late admission refusal with subsequent healthy use,
and discarded-catalog backing read failure without fallback. Healthy-trace cuts
preserve selected-path failure provenance. A generation/payload ledger compares
both retained states after replacement writes and before slot attempts, including
checkpoint/startup maintenance and explicitly durable simulator recovery.
Existing funded near-minimum user/orphan/cross-volume/fence cases remain. No
large old recovery campaign is repeated; tests do not freeze workload counters,
formatter shape, physical placement or machine configuration.

All mutation storage stays in the existing verified bounded `tmpfs,noswap` setup
and zero-swap cgroup, with exclusive loop ownership/actual target identity,
bounded tracing/output and no disk fallback. Compilation and ordinary operations
run unprivileged. Trace loss/budget overflow, OOM or execution failure invalidates
measurements; healthy refusal is an incomplete verified prefix, not completion.
No sample raises limits to finish.

For these runs the launcher reports configured 2 GiB scratch / 65,536 inodes
and a 4 GiB zero-swap member limit. Observed member peaks range from
300,478,464 to 339,738,624 bytes; memory/OOM event counts are zero.
Pyxis charged core peak is 31,273,024 bytes, 48 bytes above the prior header
accounting; the arena reservation and caller cap remain unchanged.
Maximum trace input is 4,378,945 bytes for ext4 and 7,662,378 for Btrfs.
Each persisted summary is at most 15,291 bytes; raw images/copies/payloads
and traces remain in RAM. These are recorded configurations/observations, not
independently duplicated validator limits or required machine values.
No loop devices remain after the matrix.

Member-cgroup peaks do not establish complete native whole-job RAM high-water.
Scratch/trace bounds remain independent, and the existing conservative native
cache/kernel estimate still applies. Backing/scratch case-end values in the JSON
are snapshots, not peaks. See the
[baseline resource analysis](sustained-map-measurements.md#resource-estimate-and-safety-boundary).
Real-host post-error recovery qualification and the native writer's kernel-stack
prerequisite remain deferred. Task 7 and writable deployment stay open; Btrfs-
comparable representative costs remain the initial direction and lower amplification
is the longer-term ambition. Further implementation requires separate assignment.
