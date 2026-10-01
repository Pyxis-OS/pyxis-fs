# Bounded RAM validation

The safety boundary is the development VM itself. Outer-hypervisor settings are
not part of this task. Ordinary builds remain available with `make -j16`;
filesystem tests require the verified RAM boundary below. No allocator or
publication semantics change is included.

## Local execution

On Linux with cgroup v2, systemd, tmpfs `noswap`, Python 3 and the ordinary host
compiler, run the reviewed scoped launcher:

```sh
sudo python3 tests/ram_run.py --suite preflight
sudo python3 tests/ram_run.py --suite check
sudo python3 tests/ram_run.py --suite extended
sudo python3 tests/ram_run.py --suite baseline
```

The baseline also requires loop devices, tracefs `block_bio_queue`, and ext4/Btrfs
kernel support and formatter tools. Unsupported evidence or permissions cause
refusal, without a buffered/disk-backed fallback. The launcher does not disable
system-wide swapping. It creates a fresh job before allocating workload storage:
2 GiB `tmpfs,noswap`, 65,536 inodes, 4 GiB `memory.max`, zero `memory.swap.max`,
and hard core limit zero. All fixture files use the verified pinned scratch
mount. The guard verifies actual cgroup membership and effective limits; an
environment variable alone is not evidence. The administrator must not change
these controls during a run.

Source and ordinary build artifacts may live on persistent storage. The scoped
local run builds in RAM and gives its source a read-only bind mount. Workload
storage has no disk fallback. Memory, inode, trace, output or time exhaustion is
a failed experiment; no automatic resource increase or shortened retry is allowed.
Raw diagnostics are bounded to 1 MiB in memory, trace input to 16 MiB per native
case, and the saved JSON summary to 64 KiB. Only that summary or a bounded failure
diagnostic should be saved persistently. The larger populated recovery workload
is not part of this command and remains unassigned under this storage budget.

## Storage audit

| Path or owner | Storage and limit |
| --- | --- |
| Fixture images, including logically large sparse geometries | Unlinked files opened relative to the guard's pinned RAM directory; sparse geometry is not a write budget |
| Failure adapter durable image and volatile/pending log | Same directory; 4,120-byte log records and bounded record count; no alternate production callback path |
| Seed copies and cold-recovery clones | Same directory; independent files, fixed 64 KiB copy buffer |
| Small workload expected payloads and source copies | Same directory; image, logs and all independent expectations share the aggregate scratch/job limits |
| Baseline native images | Fresh 1 GiB logical images in scratch, attached only to exclusively owned loop devices with verified backing identity |
| Extracted payloads | The inspector takes an explicit output path; it is not redirected by TMPDIR. No extraction command is used by this baseline. Any future extraction must explicitly target verified scratch |
| Build temporaries and output | RAM build directory and TMPDIR during scoped runs; CI checkout/source is the only persistent input workspace |
| stdout/stderr, traces and core dumps | Bounded memory capture/streaming counters; core dumps disabled; no raw workload log saved to disk |

Previously, an unset TMPDIR reached libc `tmpfile`, and ordinary CI declared no
RAM or swap controls. A supplied TMPDIR was only a location hint. These are no
longer sufficient: the runner refuses before fixture construction. The existing
per-owner 128 MiB core allocator cap does not bound page cache, multiple owners,
simulator logs or expected contents; the aggregate cgroup and scratch limits do.
Reclaimed filesystem blocks also need not release the backing file's tmpfs pages.

## Small comparison contract

Each case starts fresh at 1 GiB logical geometry, with 32 or 256 populated files.
Each population file receives 4 KiB; preparation then performs eight partial
1 KiB overwrites and four create/write/close/replacement histories. The measured
cases are 64 4-KiB appends, 16 256-KiB appends, 32 1-KiB overwrites (including
16 explicitly crossing a block boundary), and 16 compiler-like
create/write/close/rename/delete sequences. Seed is 1. Expected lengths, namespace
counts and every final payload byte come from an independent operation ledger.
The Pyxis profile remains E=8192, M=4096 with the existing 128 MiB core-owner cap;
ordinary/migration/recovery reserves are each 8192 blocks. This is populated
history coverage, not evidence for the maximum profile or a large empty image.

Pyxis completes and drains each mutation under its existing contract. Native
operation mode synchronizes a written file before returning; creation synchronizes
file and parent, rename synchronizes both affected parents (the same directory
in this matrix), and deletion synchronizes the parent. Close alone is not a
durability barrier. Native batch mode synchronizes after every 16 logical
create/write/rename/remove calls and at the final boundary. Its acknowledged
operations between barriers have a weaker recovery contract and are reported
separately. Both native modes finish with parent fsync, filesystem sync and clean
unmount. No cross-call batching is added to Pyxis.

For native filesystems, trace submitted write sectors at the owned loop-device
boundary, multiply by 512, and include final synchronization and clean unmount.
Preparation/format traffic and measured traffic are reported separately. Stop
markers bound the measured phase before subsequent calls can issue I/O. Tracing
must be drained during formatting and teardown too; any missing record, buffer
loss, incomplete record or budget overflow invalidates the measurement. Completed
block statistics alone cannot establish submitted bytes. Discards and flush/FUA
flags are not useful data bytes.

For Pyxis, count the real core's write callbacks separately as data/metadata and
user/orphan/drain traffic, through final checkpoint and close. Simulator duplicate
log writes are infrastructure, not filesystem write requests. Preparation callback
counts exclude initial image construction/open; do not compare these with native
format-inclusive preparation totals. Native on-disk journals/COW trees, Pyxis's
two retained states, and the simulator's explicit durability model are different
recovery mechanisms. This comparison does not qualify real-host post-error
recovery or equate their guarantees.

RAM elapsed times include the respective adapter paths and are not NVMe latency
or throughput. Record exact source revision, compiler/kernel/formatter settings,
completion, useful bytes, total submitted bytes and Pyxis phase counts alongside
timing. Serial matched runs are evidence for a later focused improvement proposal,
not acceptance of writable deployment or authorization for another implementation.

The [initial baseline report](ram-baseline.md) records two completed matrices,
storage evidence and the next proposed bounded investigation.

## CI

The quick filesystem PR job uses an anonymous tmpfs volume capped at 2 GiB and
65,536 inodes, a 4 GiB container memory limit and zero container swap
(`--memory-swap` equals `--memory`), plus hard core limit zero. Podman's `--tmpfs`
parser does not accept `nr_inodes`; the anonymous local-driver tmpfs mount passes
that option without adding container privileges or using a shared named volume.
Actual mount type and limits are verified before building or running fixtures;
an ordinary disk-backed volume cannot substitute for it.

`python3 tests/ram_run.py --inside --suite check --ci-quick` selects the accepted
quick-only exception to mount-level `noswap`. The runner independently accepts
`--ram-mode ci` only with `--suite pr`. Both verify the actual cgroup membership,
memory limit, zero swap allowance/current usage and core limit. The launcher
creates a new scratch directory, the C guard requires it to be empty, and fixture
helpers create/unlink new files through its pinned descriptor. Consequently the
fixture pages are first allocated by tasks already in the verified zero-swap job;
preexisting or shared backing files are not reused. Administrators must not move
tasks, relax limits or share this scratch during the job.

This mode relies on the owning memory cgroup's swap limit, which the kernel's
[tmpfs swap-out path](https://github.com/torvalds/linux/blob/v6.19/mm/shmem.c)
enforces through [swap allocation](https://github.com/torvalds/linux/blob/v6.19/mm/swapfile.c).
It is not a general exception for buffers created outside that cgroup. Default
local runs, extended/file/recovery suites and the native comparison retain
mount-level `noswap`; the CLI refuses the CI mode for those workloads. Runtime
options alone are not proof: missing or unsupported effective controls still
cause refusal, with no disk fallback or automatic resource increase. The quick
job needs neither loop/tracing privileges nor a compiler-container rebuild.

Keep `Filesystem / host-contract (pull_request)` required in pyxis-fs. Pyxis's
existing `Build Pyxis / build (pull_request)` check explicitly requires the
separate bounded filesystem job for its exact gitlink before image building.
The owner configures branch protection. A green quick check establishes only the
maintained bounded contract cases, not this comparative baseline, larger pressure
campaigns, physical durability, performance acceptance or guest integration.

The storage controls follow the kernel's [tmpfs noswap contract](https://docs.kernel.org/filesystems/tmpfs.html)
and [cgroup v2 memory controller](https://docs.kernel.org/admin-guide/cgroup-v2.html).
The trace counter measures submitted requests; Linux [block statistics](https://docs.kernel.org/block/stat.html)
count completed sectors and provide only a quiesced cross-check, not the primary
submission measure.
