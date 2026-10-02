# Sustained populated allocation-map RAM observations

Filesystem #22 and Pyxis #316 were confirmed merged before this follow-up. The
baseline is filesystem merge `933098d`, containing published `fbaf2fd`; the
production core is unchanged throughout this task. The owner discussed and
approved the compact matrix/resource estimate before workload implementation.
This supplies cost evidence for the next discussion, not deployment acceptance
or authorization for another allocator change. All 54 filesystem cases complete
and independently verify. The [bounded record](measurements/sustained-map.json)
retains both repetitions, native settings, build commands, all phases/counters,
verification and storage evidence.

Measured comparison code is `b3c7ac6`; the first 256-block append sample uses
`3e9f378`, differing only in error-result diagnostics. No healthy workload or
production behavior changed between these revisions. Later documentation and
quick-build target changes do not change the sustained path. The ordinary
compiler is GCC 16.2.1, with Make's unchanged `-O2 -g3`, GNU C23 warnings and
freestanding core flags. Kernel is Linux 6.19.10. Formatter commands, feature
flags and versions (mke2fs 1.47.3, Btrfs-progs 6.19.1) remain in each native record.
The observed VM has 16 online CPUs and MemTotal=32,850,516 KiB; these are observed
configuration, not validation requirements. No affinity pinning or cache dropping
is used, and no other build/test/sanitizer job overlaps a matrix.

## Configuration and accounting

Each configuration starts fresh on Pyxis, ext4 and Btrfs and runs twice,
serially. It holds 64 background files constant while varying their total
individually durable 4 KiB append history across 256, 2048 and 5120 blocks
(1, 8 and 20 MiB). After preparation, three equal successive windows perform:

- Append: 512 separately durable 4 KiB writes per window, extending one target
  through 6 MiB.
- Overwrite: 512 separately durable 1 KiB writes per window in an existing 4 MiB
  target, alternating contained and deliberately cross-block requests. Target
  preparation uses requests of at most 256 KiB.
- Compiler history: 128 create/write-4-KiB/close/rename/remove cycles per window.
  This models file operations; it does not invoke a compiler in each cycle.

All backends use fresh 1 GiB geometry. Pyxis retains E=8192, M=4096, existing
8192-block ordinary/migration/recovery reserves and a 128 MiB core-owner cap.
These are recorded experiment settings, not new filesystem invariants or
acceptance thresholds. The launcher exposes the population, total background
blocks, windows, operations, cycles, overwrite shape and seed as configuration.

```sh
sudo -n python3 tests/ram_run.py --suite sustained \
  --background-blocks 2048 --case append
```

Select each background/case combination and repeat the unchanged command once.
Unspecified experiment options use the documented defaults; no limits are raised
or requests shrunk to obtain completion. The [launcher
contract](ram-validation.md#sustained-comparison)
describes commands, refusal and invalidation behavior.

Setup, preparation, each window and final checkpoint/maintenance are separate
phases. There is no checkpoint or filesystem-wide sync between windows. The
complete-history total includes formatting and every trailing maintenance write.
Native tracing continues through verification, result handoff and clean unmount;
post-end bytes remain in the total. Native file writes fsync before completion;
creation syncs file and parent, rename/removal sync the affected parent, and the
final boundary syncs parent and filesystem. Close alone is not a barrier. Pyxis
preserves individually durable completion, both retained states, two-flush
publication, carried retirement and bounded idle pool debt. Final checkpoint
settles volume debt without requiring pool debt to vanish. These recovery
contracts remain different; RAM/native fsync observations do not qualify
real-host post-error recovery or physical-device durability.

Pyxis submitted bytes are real core callbacks, separated into data/metadata and
active user/orphan/standalone drain work. Reclamation inside a user publication
stays in that publication's group. Simulator duplicate backing/log writes are
infrastructure. Native bytes are submitted sectors at the exclusively owned loop
boundary; they are not split speculatively into native data or metadata. Trace
loss, budget overflow or submitted/completed mismatch invalidates a sample.

An independent byte mirror is updated from requested contents and confirmed
progress, without core/checker metadata or read-back inputs. After final fencing,
Pyxis reopens read-only and all surviving names, lengths and bytes are checked;
native verification uses the same expectations. Expected namespace membership
plus total entry count detects missing and extra names. Healthy quota/profile/
space refusal remains a verified **incomplete** prefix, not completion. I/O,
stopped health, host allocation failure/OOM or execution failure invalidates the
run. Owner-cap exhaustion reported as healthy `PFS_LIMIT` remains an incomplete
profile refusal. No incidental publication count, physical location, map shape
or benchmark setting is a correctness assertion.

## Resource estimate and safety boundary

The approved estimate accounts for the entire job, rather than just useful
payload. At the largest append setting, independent expected capacity is about
26 MiB. A worst-case fully touched image consumes 1 GiB; the bounded volatile
log adds about 3.375 MiB. The unchanged profile's arena is 30,372,016 bytes, within
the 128 MiB charged owner budget. Builds, process buffers and trace capture also
consume RAM. Native backing may touch the full image and native page caches can
add roughly another image's worth of memory. Preparation shares the same image;
there are no recovery clones, extracted payloads or copied source trees in this
comparison. Only one backend runs at a time.

The planning estimate was roughly 1.5–2 GiB for Pyxis and 2.5–3 GiB for native
cases, using the existing configured 2 GiB scratch and 4 GiB member-cgroup
limits. Trace ring capacity is separately bounded, and cumulative
trace input is limited to 16 MiB; diagnostic capture is bounded to 1 MiB and
each persistent job summary to 64 KiB. The existing 600-second child and
1200-second service deadlines remain. These provisioning values are recorded
configuration, not independent validator constants. Sparse geometry or small
payload does not bound write traffic or touched backing pages.

The launcher verifies bounded `tmpfs,noswap`, zero-swap cgroup and actual native
root/loop/backing identity before mutations. Compilation and comparison children
run unprivileged; mount, exclusive loop ownership and tracing remain with the
supervisor. Cleanup handles normal failure/interruption. Raw traces, images,
volatile/durable backing and expectations remain in RAM; only bounded JSON
summaries persist. Case-end backing/scratch allocation snapshots are not peaks.

The cgroup peak is process/member-cgroup evidence, **not a complete native RAM
high-water measurement**. The first largest append sample records a 317,198,336-byte
cgroup peak while its Btrfs case-end backing/scratch allocations are 373,837,824 /
395,927,552 bytes. Those values alone demonstrate incomplete charging to this
cgroup; the responsible kernel/loop-worker charging path is not established by
these records. Scratch pages remain independently capped by tmpfs, and traces
by their own bounds. Native caches/kernel work can fall outside the member limit;
the conservative pre-run estimate includes native cache overhead. Do not sum end
snapshots with cgroup peaks and call that a measured total peak, or claim the
4 GiB cgroup limits every kernel allocation. This task retains the existing
verified storage boundary, rather than claiming general host-memory isolation.

## Complete-history submitted writes

MiB includes setup, preparation, all three windows, final fencing and native
post-end submissions. Useful bytes count completed application writes, including
overwrites and subsequently removed compiler output; they are not final live
size. Ranges retain both serial samples.

| Background MiB | Case | Full-history useful MiB | Pyxis MiB | Btrfs MiB | ext4 MiB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | append | 7 | 158.03 | 253.64 | 92.87 |
| 1 | overwrite | 6.5 | 199.27 | 243.50–243.59 | 70.52 |
| 1 | compiler | 2.5 | 86.89 | 242.54 | 97.36 |
| 8 | append | 14 | 362.60 | 470.82–470.95 | 134.88 |
| 8 | overwrite | 13.5 | 409.18 | 471.54–471.76 | 112.54–112.58 |
| 8 | compiler | 9.5 | 276.72 | 468.52 | 139.38–139.43 |
| 20 | append | 26 | 867.48 | 851.15–851.93 | 206.90–211.90 |
| 20 | overwrite | 25.5 | 887.41 | 852.50–852.90 | 184.56–184.58 |
| 20 | compiler | 21.5 | 686.78 | 849.10–849.29 | 211.39 |

## Preparation, successive windows and final cost

Each append window writes 2 MiB; overwrite/compiler windows each write 0.5 MiB.
Pyxis physical data is respectively 2 / 3 / 0.5 MiB per window:
contained/cross-block partial overwrites replace one/two full data blocks. The
following MiB are **all** submitted writes, not data alone. Window comparisons
exclude setup/preparation; final synchronization and post-end writes are
reported separately and remain in complete-history totals.

| Background MiB | Case | Pyxis prep MiB | Pyxis W1 / W2 / W3 MiB | Btrfs W1 / W2 / W3 MiB | ext4 W1 / W2 / W3 MiB |
| ---: | --- | ---: | --- | --- | --- |
| 1 | append | 13.28 | 34.23 / 47.58 / 62.83 | 62.94 / 67.69 / 79.84 | 12.00 / 12.00 / 12.00 |
| 1 | overwrite | 17.99 | 45.18 / 57.77 / 78.22 | 62.16 / 66.79 / 66.00–66.07 | 3.12 / 3.12 / 3.12 |
| 1 | compiler | 13.20 | 23.61 / 24.00 / 26.00 | 63.50 / 63.54 / 72.84 | 13.50 / 13.51 / 13.50 |
| 8 | append | 180.40 | 49.77 / 66.11 / 66.22 | 63.10 / 79.38–79.48 / 68.38–68.47 | 12.00 / 12.00 / 12.00 |
| 8 | overwrite | 186.14 | 86.36 / 76.73 / 59.75 | 62.97 / 79.73–79.91 / 63.00–63.50 | 3.12–3.16 / 3.12 / 3.13 |
| 8 | compiler | 180.34 | 31.12 / 31.67 / 33.50 | 63.63 / 72.56 / 72.91 | 13.51 / 13.50 / 13.50 |
| 20 | append | 599.50 | 69.54 / 84.56 / 113.80 | 62.94 / 79.23–79.32 / 68.56–68.66 | 12.00–13.00 / 12.00–14.00 / 12.00–14.00 |
| 20 | overwrite | 605.13 | 111.14 / 109.25 / 61.70 | 62.16 / 79.45 / 65.03 | 3.12 / 3.12–3.14 / 3.13 |
| 20 | compiler | 599.46 | 28.50 / 28.72 / 30.00 | 63.50 / 72.78 / 73.03 | 13.50–13.51 / 13.50–13.51 / 13.50–13.51 |

Setup submissions are 32 KiB for Pyxis, 50,192 KiB for ext4 and 5,368 KiB for
Btrfs in these samples. Final values below include the explicit
fence/synchronization plus native post-end submissions; the record preserves
those native components separately.

| Background MiB | Case | Pyxis final KiB | Btrfs final + post-end KiB | ext4 final + post-end KiB |
| ---: | --- | ---: | ---: | ---: |
| 1 | append | 84 | 712 | 28 |
| 1 | overwrite | 80 | 904–936 | 20 |
| 1 | compiler | 52 | 392 | 44 |
| 8 | append | 56 | 776 | 28 |
| 8 | overwrite | 168 | 1,064–1,096 | 20 |
| 8 | compiler | 64 | 392 | 44–48 |
| 20 | append | 56 | 840 | 28 |
| 20 | overwrite | 168 | 1,000–1,096 | 20 |
| 20 | compiler | 72 | 456 | 48–52 |

## Pyxis metadata and publication diagnostics

KiB below cover all three windows together. User/orphan/drain classification
follows the active publication, including reclamation carried by that
publication. All trailing standalone drain costs appear in the final phase.
Map-node emissions identify the allocation-map component of metadata traffic;
other metadata includes volume trees, catalog and pool-root/slot writes.

| Background MiB | Case | User / orphan / standalone-drain metadata KiB | Map share of metadata | Local / bulk publications | Flushes | Redistribution share of closure marks |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | append | 141,964 / 0 / 0 | 69.8% | 1517 / 19 | 3072 | 71.0% |
| 1 | overwrite | 176,304 / 0 / 0 | 72.1% | 1525 / 11 | 3072 | 50.7% |
| 1 | compiler | 60,856 / 12,988 / 0 | 35.8% | 1920 / 0 | 3840 | 0.5% |
| 8 | append | 180,336 / 0 / 0 | 76.2% | 1533 / 3 | 3072 | 80.3% |
| 8 | overwrite | 218,976 / 0 / 0 | 76.8% | 1533 / 3 | 3072 | 35.3% |
| 8 | compiler | 79,456 / 17,612 / 0 | 51.2% | 1920 / 0 | 3840 | 0.5% |
| 20 | append | 268,184 / 0 / 0 | 84.0% | 1534 / 2 | 3072 | 87.5% |
| 20 | overwrite | 279,640 / 0 / 0 | 81.8% | 1535 / 1 | 3072 | 53.3% |
| 20 | compiler | 72,092 / 15,688 / 0 | 46.0% | 1920 / 0 | 3840 | 0.1% |

Counts are observations, not correctness requirements. Redistribution additions
include ancestors and abandoned local attempts, not only repaired leaves. Local
and bulk emitted nodes, costs, every fallback flag, closure sum/max, growth
passes, source sums and complete phase flush counts remain in the record.

| Background MiB | Case | J max prep / W1 / W2 / W3 | q max W1 / W2 / W3 | Mean closure W1 / W2 / W3 | Bulk emissions W1 / W2 / W3 |
| ---: | --- | --- | --- | --- | --- |
| 1 | append | 15 / 40 / 63 / 92 | 37 / 57 / 84 | 9.2 / 15.7 / 23.3 | 277 / 265 / 323 |
| 1 | overwrite | 17 / 46 / 50 / 50 | 42 / 49 / 49 | 13.3 / 19.2 / 29.5 | 307 / 50 / 0 |
| 1 | compiler | 15 / 15 / 15 / 15 | 14 / 6 / 4 | 3.0 / 3.3 / 4.0 | 0 / 0 / 0 |
| 8 | append | 100 / 110 / 121 / 133 | 100 / 110 / 121 | 17.0 / 25.0 / 25.1 | 110 / 121 / 133 |
| 8 | overwrite | 100 / 121 / 133 / 133 | 110 / 121 / 26 | 33.2 / 28.6 / 20.2 | 231 / 133 / 0 |
| 8 | compiler | 100 / 100 / 100 / 100 | 12 / 15 / 7 | 6.0 / 6.4 / 7.0 | 0 / 0 / 0 |
| 20 | append | 179 / 179 / 196 / 216 | 136 / 179 / 196 | 26.9 / 34.2 / 48.8 | 0 / 196 / 216 |
| 20 | overwrite | 179 / 196 / 196 / 196 | 179 / 147 / 149 | 45.6 / 44.9 / 21.2 | 196 / 0 / 0 |
| 20 | compiler | 179 / 179 / 179 / 179 | 9 / 10 / 6 | 5.0 / 5.2 / 5.6 | 0 / 0 / 0 |

## Instrumented RAM elapsed observations

Seconds are two-sample means from the child phase timer. They exclude formatting
from preparation and exclude post-end verification/handoff/unmount from windows.
Native external setup and native post-end elapsed times remain separate in the
record. The planning interval runs from first publisher backing read to first
replacement-write callback; it includes adapter reads and optional
bulk-reference counting, rather than isolated CPU time.

| Background MiB | Case | Pyxis prep seconds | Pyxis W1 / W2 / W3 seconds | Planning W1 / W2 / W3 seconds | Btrfs three-window seconds | ext4 three-window seconds |
| ---: | --- | ---: | --- | --- | ---: | ---: |
| 1 | append | 1.993 | 7.625 / 12.394 / 17.685 | 5.129 / 9.726 / 14.810 | 0.217 | 0.079 |
| 1 | overwrite | 3.175 | 10.836 / 13.827 / 15.380 | 6.561 / 9.530 / 11.240 | 0.198 | 0.028 |
| 1 | compiler | 1.984 | 5.773 / 5.884 / 5.960 | 3.337 / 3.373 / 3.453 | 0.205 | 0.064 |
| 8 | append | 46.479 | 21.422 / 24.784 / 27.577 | 18.727 / 21.987 / 24.633 | 0.217 | 0.099 |
| 8 | overwrite | 48.496 | 26.990 / 27.465 / 25.863 | 22.293 / 22.884 / 21.409 | 0.260 | 0.031 |
| 8 | compiler | 46.671 | 22.083 / 22.198 / 22.296 | 19.327 / 19.396 / 19.494 | 0.237 | 0.065 |
| 20 | append | 213.826 | 35.957 / 40.331 / 47.208 | 33.149 / 37.376 / 44.120 | 0.218 | 0.085 |
| 20 | overwrite | 217.021 | 43.572 / 44.545 / 38.337 | 38.619 / 39.810 / 33.704 | 0.192 | 0.030 |
| 20 | compiler | 213.235 | 37.204 / 37.334 / 37.344 | 34.298 / 34.355 / 34.407 | 0.190 | 0.070 |

## Interpretation

Allocation-map replacement remains the dominant submitted metadata component in
append/overwrite windows. It accounts for roughly 70–84% of their metadata
writes across both samples. Most publications choose the local path, and every
observed local emission remains smaller than its diagnostic bulk reference.
Bulk fallback is consequently not the dominant window traffic source. Global
fallbacks remain material during preparation; their exact flags/costs are retained
separately, including coincident exhausted-overflow flags.

At the largest append setting, equal 2 MiB windows submit approximately 69.5,
84.6 and 113.8 MiB in Pyxis as source maps reach J=179/196/216. Mean closure grows
from about 27 to 49 nodes. Redistribution contributes about 87.5% of window
closure marks, including ancestors. These observations support targeting closure
expansion; they do not identify which neighbour would have been better, prove a
linear population cost, or turn a local hit into an automatic performance win.

Overwrite history can first expand then settle: the largest samples' equal
windows decline from about 111/109 MiB to 62 MiB while J remains 196 in the latter
two windows. Compiler histories keep background/map population approximately
constant and require no window bulk construction. Their write cost is not
monotonic with background size: 20 MiB history has fewer window bytes than 8 MiB,
but much higher planning time. Source validation, canonical vector editing and
admission still scan population-sized structures. The timer establishes their
combined instrumented interval, not an isolated attribution to one algorithm.

The smaller complete-history Pyxis totals stay below Btrfs; the largest append
and overwrite samples exceed Btrfs. Window exceptions also exist where a
complete-history win remains. Ext4 stays useful context, and format costs must
not obscure sustained-operation differences. No universal ratio, numerical
acceptance threshold or disk-deployment qualification follows from this matrix.

## Recommended next bounded improvement

Propose a separately assigned **occupancy-aware immediate-neighbour choice** for
the existing topology-preserving repair, rather than another allocator redesign.
Current repair always chooses the clean left neighbour when one exists. The
measured redistribution marks identify a cost worth investigating, but do not
establish which direction or occupancy caused it: additions include ancestors
and can include attempts subsequently discarded for bulk construction.

For a failing run with l leaves and r records, a clean neighbour with n records
offers n-1 spare records for underflow, or 46-n vacant slots for overflow. Compare
both available immediate neighbours using the validated candidate's **expanded
run**, including any already-marked run bridged by that neighbour. Prefer a
fitting expanded run, otherwise the smaller remaining deficit, with a left tie
break. This is a proposed rule for review, not accepted placement policy. A
leaf-only score can be wrong when it bridges two marked runs. Even a currently
fitting score does not establish final fit after renewed self-accounting.

The correction can remain within the current proof: mark one clean neighbour and
its ancestors, never remove marks, keep the fixed ascending eligible-ID prefix,
then regenerate accounting and canonical seam closure. Each growth pass still
adds a source identity, terminating within the fixed J inventory or choosing the
existing funded bulk fallback. Existing q <= J <= H-Cmax-1 and q+c+1 <= H funding
remain; both retained states, no same-publication reuse and admission/failure
semantics remain intact. Use scalar occupancy counts/current reserved traversal
storage, not another map copy or an unfunded retry. No general splits/merges,
fill-policy, reserve, memory-budget or placement redesign is proposed here.

Record bounded repair-reason/alternative-capacity counters with that next task,
then repeat these unchanged matched histories through all final maintenance.
Compare final closure, growth passes, actual map/total submitted bytes, fallback
and RAM elapsed time. More favourable neighbour capacity is a hypothesis for
smaller closure; the present counters cannot predict its savings. It does not
remove global validation/vector scans, guarantee a better scaling bound, or
resolve every compiler-history cost. Do not implement it without a new assignment.

## Validation and observed resource use

The [bounded record](measurements/sustained-map.json) includes validation results
and the instrumentation controls, not just successful performance samples:

- A focused legacy compiler control before/after instrumentation preserves Pyxis
  submitted bytes and non-timing state/map counters. Native measured-window bytes
  also match; native complete-history totals retain their observed variation.
- Small configurable append, overwrite and compiler histories complete and verify
  on all three filesystems, including a non-default population/window shape and
  seed zero. These check configuration propagation without asserting defaults.
- The maintained quick suite passes 135 groups; the extended suite passes six.
  Both also pass with ASan/UBSan. Scoped contract checks compile the comparison
  binary alongside the ordinary core tools.
- ASan/UBSan small histories pass on all three backends, as does a focused legacy
  compiler case covering the replacement oracle. Existing native safety checks
  pass their five target/descriptor/backing refusals and small native workloads.

All 18 matrix jobs (54 filesystem cases) report complete, independently verified
contents and namespace, no trace loss/budget overrun and no member-cgroup OOM or
swap use. Repeated Pyxis submitted-byte totals agree. Recorded matrix member peaks
range from 289,099,776 to 333,266,944 bytes; these have the native charging
limitation explained above. The maximum cumulative native trace input is
7,662,806 bytes. Each persistent job summary is about 14 KiB, below its configured
64 KiB cap. Peak core-charged memory is 31,272,888 bytes, below the unchanged
128 MiB owner budget. Sanitizer observations remain separate from timings.

These checks protect behavior, recovery and retained-state contracts while the
workload independently checks its actual completed history. This task does not
force a pressure refusal, maximal debt or near-minimum admission scenario; their
existing contract coverage remains, without claiming this matrix qualifies all
profiles. No old large recovery campaign or full legacy comparison matrix is
repeated. Ordinary tests and compilation run unprivileged inside the existing
verified launcher; privileges remain scoped to its storage/tracing setup.

## Scope limits

This is bounded history coverage at a fixed background file count, not a larger
namespace census, near-profile/quota pressure campaign, maximum-cohort stress,
64/256 GiB disk qualification or a native Caelum writer run. RAM timings include
adapter and diagnostic work and are not NVMe performance. Two repetitions show
local repeatability, not a confidence interval. Task 7, broader Btrfs-comparable
acceptance and writable deployment remain open; the kernel-stack prerequisite
and real-host recovery qualification remain deferred.
