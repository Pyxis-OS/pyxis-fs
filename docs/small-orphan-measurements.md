# Small-orphan cleanup: matched RAM measurements

Two serial unchanged baseline matrices per revision completed on 2026-10-02:
before `87d2f20`, after production commit `9d9a347`. All 40 cases per sample
completed and every final payload/namespace matched the independent ledger.
The [bounded summary](measurements/small-orphan-cleanup.json) records four samples
in before1/before2/after1/after2 order, all submitted-byte totals, phase counters,
preparation, final counts, timings, native profiles and storage evidence. Later
test/document commits change no measured production or comparison code.

Each run used `sudo python3 tests/ram_run.py --suite baseline`, without changes
to the runner or matrix. Configuration: Linux 6.19.10-300.fc44.x86_64, KVM VM,
16 vCPUs, 32 GiB RAM, GCC 16.2.1 `-O2 -g3`, ext4 tools 1.47.3 and Btrfs tools
6.19.1. CPU affinity was not pinned and caches were not dropped; no other build,
test or workload job overlapped measured operations. Images, seed, populations,
operation history, E/M, core-owner cap and reserves match the
[existing comparison contract](ram-validation.md#small-comparison-contract).
These settings are recorded observations, not filesystem invariants.

## Total writes through maintenance

KiB below includes final synchronization, all Pyxis maintenance and native clean
unmount. Preparation is excluded here and recorded separately; Pyxis preparation
does not include formatter writes. Both Pyxis samples reproduced each byte count.

| Population | Case | Before | After | Change |
| ---: | --- | ---: | ---: | ---: |
| 32 | small 4 KiB appends | 6,088 | 6,104 | +16 |
| 32 | large 256 KiB appends | 5,432 | 5,432 | 0 |
| 32 | small overwrites | 3,084 | 3,088 | +4 |
| 32 | compiler history | 7,744 | 6,528 | −1,216 (15.70%) |
| 256 | small 4 KiB appends | 11,096 | 11,076 | −20 |
| 256 | large 256 KiB appends | 6,716 | 6,716 | 0 |
| 256 | small overwrites | 5,624 | 5,624 | 0 |
| 256 | compiler history | 15,708 | 12,756 | −2,952 (18.79%) |

The optimization changes cleanup during preparation too: the same logical
history produces a different physical map. The small ±4–20 KiB changes are
observed layout/history effects, not reductions in the per-call append/overwrite
publication sequence. At 32 files the compiler saving equals the earlier phase
proxy; at 256 it exceeds the estimated 2,534 KiB. Neither estimate was a test or
acceptance threshold.

Compiler breakdown, KiB:

| Population / revision | Data | User metadata | Orphan metadata | Drain metadata | Metadata/useful byte |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 before | 64 | 2,432 | 1,024 | 4,224 | 120 |
| 32 after | 64 | 2,432 | 512 | 3,520 | 101 |
| 256 before | 64 | 4,280 | 1,920 | 9,444 | 244.4375 |
| 256 after | 64 | 4,232 | 920 | 7,540 | 198.3125 |

Observed compiler publications drop from 288 to 240, inferred from the deliberate
two-flush protocol: 64 user remain, orphan publications fall 32→16 and drains
192→160. That is 4.5→3.75 publications per logical user operation for this
64-operation case. These are workload measurements, not exact test requirements.
Cleanup reduction also changes later map writes, which explains why a phase
average is not a precise prediction.
Final compiler extent counts remain 32 and 256 respectively in all samples;
the byte reduction does not come from omitting file operations or payloads.

Compiler preparation/combined totals are 6,280/14,024→5,976/12,504 KiB at 32 files
and 62,480/78,188→61,860/74,616 KiB at 256. Preparation and measured totals are
not interchangeable denominators.

The matched native compiler operation rows remain ext4 1,772 KiB / Btrfs 5,864
KiB at 32 files and ext4 1,848 KiB / Btrfs 5,896 KiB at 256, in all four samples.
Native batch rows remain separate: ext4 240 KiB / Btrfs 1,152 KiB at 32 and
ext4 240–272 KiB / Btrfs 1,280 KiB at 256. Batch acknowledgments have weaker
intermediate durability. Native journals/COW recovery and Pyxis's retained-state
contract differ as documented; these are submitted filesystem writes, not NAND
amplification or real-host crash qualification.

## Elapsed observations and validation

Primary compiler timing, milliseconds including measured calls, final
synchronization/core close, excluding independent verification:

| Population | Before | After |
| ---: | ---: | ---: |
| 32 | 495.916–496.310 | 438.818–439.356 |
| 256 | 1,328.688–1,331.246 | 1,202.973–1,203.912 |

These are two observations per revision through RAM/simulator adapters, not
NVMe latency/throughput or a statistical performance guarantee. The summary
retains other rows' timings. Native `after_end_seconds` includes verification,
handoff, unmount and trace accounting; it is not isolated unmount time.

All four jobs verified their actual 2 GiB bounded `tmpfs,noswap` scratch,
65,536-inode limit, 4 GiB cgroup cap, zero swap allowance/current use and disabled
core dumps. Builds/comparison children ran as UID/GID 1000; privileged work was
limited to supervision/setup. These are the unchanged provisioned local amounts,
not duplicated validator limits. Peaks were 327,282,688 / 305,348,608 bytes before
and 330,436,608 / 305,532,928 after, with zero max/OOM events. Native traces had
no lost records and stayed below 903,000 bytes per case, within the unchanged
trace budget. Submitted totals matched completed-sector cross-checks after
quiescence. Workload files, volatile/durable logs and native images stayed in
RAM; only bounded summaries were saved. Loop/mount/trace teardown completed.

The final code/tests passed all 118 quick groups and six seed-1 extended groups
through the same strict launcher, with no failures/ignored groups, swap or OOM.
Those jobs peaked at 302,194,688 and 494,002,176 bytes. New tests verify independent
retained bytes before maintenance slot writes, atomic eligible deletion, trace-
discovered flush and planning-read failures, durable-but-unconfirmed deletion,
sticky failure/consumed handles, recovery charges, excluded shapes and funded
last release under quota/profile pressure with no new memory allocation.

This is a measured intermediate improvement. Small append/overwrite whole-map
costs remain, compiler totals still exceed ext4, and the writable-deployment
blocker and task 7 remain open. No allocator, admission, memory-cap, reserve or
durability policy changed. Any next efficiency implementation needs a separate
assignment.
