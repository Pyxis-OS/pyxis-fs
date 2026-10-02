# Retirement carryover: matched RAM measurements

Two serial unchanged 40-case matrices per revision completed on 2026-10-02.
Before was filesystem `73a4885`; after sample 1 compiled `609b701`, and sample 2
compiled `063d93c`. Their only production difference is defensive classification
of impossible funded selection exhaustion in `7e28ad2`; successful publication
and workload behavior are identical. Later production changes dispatch that same
fence through the existing mode bridge, keeping the read-only kernel link independent
of the publisher. Other later commits change fixtures and documentation.
Every final payload/namespace matched the independent ledger.
The [bounded summary](measurements/retirement-carryover.json) records all four
samples, preparation, submitted bytes through trailing maintenance/synchronization,
phase counters, final state, timings, native profiles and storage evidence.

Command: `sudo python3 tests/ram_run.py --suite baseline`, without matrix, runner,
budget or synchronization changes. Observed configuration: Linux
`6.19.10-300.fc44.x86_64`, KVM VM, 16 vCPUs, 32 GiB RAM, GCC 16.2.1 `-O2 -g3`.
Affinity was not pinned, caches were not dropped, and no other build/test/workload
job overlapped measured operations. The same seed, populated preparation history,
geometry, E/M, core-owner cap and persisted reserves apply before/after; the
computed admission requirements increase under carryover. Configuration and
profile values are observations, not test invariants.

## Writes through final checkpoints

KiB below excludes preparation (reported separately) and includes final checkpoints,
all Pyxis maintenance and native clean teardown. Both Pyxis samples reproduce
each before and after count. Submitted callbacks/bios are RAM write requests,
not NAND amplification or NVMe performance.

| Population | Case | Before KiB | After KiB | Reduction |
| ---: | --- | ---: | ---: | ---: |
| 32 | 4 KiB appends | 6,104 | 2,944 | 51.77% |
| 32 | 256 KiB appends | 5,432 | 4,732 | 12.89% |
| 32 | Small overwrites | 3,088 | 1,520 | 50.78% |
| 32 | Compiler history | 6,528 | 3,056 | 53.19% |
| 256 | 4 KiB appends | 11,076 | 4,564 | 58.79% |
| 256 | 256 KiB appends | 6,716 | 5,208 | 22.45% |
| 256 | Small overwrites | 5,624 | 2,428 | 56.83% |
| 256 | Compiler history | 12,756 | 5,236 | 58.95% |

Compiler phase breakdown, KiB:

| Population / revision | Data | User metadata | Orphan metadata | Standalone drain metadata | Metadata/useful byte |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 before | 64 | 2,432 | 512 | 3,520 | 101.0000 |
| 32 after | 64 | 2,432 | 512 | 48 | 46.7500 |
| 256 before | 64 | 4,232 | 920 | 7,540 | 198.3125 |
| 256 after | 64 | 4,156 | 916 | 100 | 80.8125 |

Classification now follows the active publication, not whether debt exists.
User/orphan metadata includes reclamation performed inside those durable publications;
moving work out of the standalone phase alone is not a saving. Total requests above
include every phase and the terminal checkpoint. Every after case has zero final
volume debt; bounded pool retirement remains (10/24 blocks in compiler cases).
Maximum observed compiler call-boundary volume debt is eight blocks, not a required
threshold. Two retained cohorts, not these measurements, establish the bound.

Final extent counts are unchanged in every matched case. Compiler extents remain
32/256; allocation records change 63→54 and 336→334. The smaller map and different
physical history affect later metadata traffic; no operation or payload was omitted.

Compiler preparation/combined totals fall 5,976/12,504→2,736/5,792 KiB at 32 files
and 61,860/74,616→26,544/31,780 KiB at 256. Preparation is a separate denominator
and excludes Pyxis formatter traffic.

Matched native compiler individually durable rows remain ext4 1,772/1,848 KiB
and Btrfs 5,864/5,896 KiB at 32/256 files in all four samples. Native batch rows
remain separate, with weaker intermediate acknowledgment durability. Their exact
profiles, submitted/completed cross-check and ranges are in the summary and
[existing comparison contract](ram-validation.md#small-comparison-contract).
Pyxis compiler totals still exceed ext4; task 7 and writable deployment remain
blocked. This does not qualify real-host post-error recovery or change durability.

## RAM timings and storage evidence

Compiler elapsed observations, milliseconds through calls, final checkpoint and
core disposal, excluding independent verification:

| Population | Before range | After range |
| ---: | ---: | ---: |
| 32 | 438.374–444.194 | 303.334–305.109 |
| 256 | 1200.434–1202.038 | 887.184–887.427 |

These are two observations per revision through RAM/simulator adapters, not
statistical performance guarantees or NVMe latency/throughput. Native auxiliary
`after_end_seconds` includes verification, handoff, unmount and trace accounting;
it is not isolated unmount time.

All four jobs verified bounded tmpfs,noswap scratch, bounded cgroup memory, zero
swap allowance/current use and disabled core dumps. Actual local provisioning was
2 GiB scratch, 65,536 inodes and 4 GiB job memory, without changing validators or
configuration. Builds/comparison workers ran as UID/GID 1000; privilege remained
scoped to supervision, mounts, owned loop devices and tracing. Job peaks in sample
order were 307,085,312 / 306,745,344 / 317,050,880 / 317,829,120 bytes. All max/OOM
events were zero. Native trace records were complete and stayed below 902,749 bytes
per case within the existing trace budget. Submitted totals equaled completed-sector
cross-checks after quiescence. Images, volatile/durable backing and extracted files
stayed in RAM; only bounded summaries were retained. Owned loop/mount/trace teardown
completed, with no fallback or resource increase.

## Behavioral validation and limits

At `a0c1c75`, all 125 quick groups and six seed-1 extended groups pass using the
same strict launcher, configured budgets and unprivileged build/test worker. Job
memory peaks are 319,090,688 and 493,666,304 bytes, with zero max/OOM events and no
swap. Ordinary host tools build with the suite. The complete shared archive
cross-compiles with Pyxis GCC 16.2.0, kernel flags and compiler headers only; the
kernel's read-only subset plus its existing memory primitives links without any
publisher/planner/checker object or unresolved symbol.

The instrumented build could not link because this VM's GCC sanitizer links refer
to missing `libasan.so.8.0.0` and `libubsan.so.1.0.0`. No sanitizer tests executed;
no packages, runner configuration or budgets were changed. Exact-head repository
CI is reported on the filesystem and dependent Pyxis PRs.

Tests cover independently expected rolling/cross-volume retained payloads, durable
free transitions, nonadjacent opening, interrupted fences/recovery, checkpoint and
final release, provenance and funded progress. Near-minimum fixtures exercise the
calculated reserve floors with small reachable images; they do not establish every
maximal 2D-debt/profile population. No large recovery workload was repeated. These
checks do not qualify real-host post-error recovery, native writable deployment or
the broader write-efficiency acceptance target.
