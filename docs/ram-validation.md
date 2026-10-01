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

The quick filesystem PR job requires tmpfs capped at 2 GiB and 65,536 inodes,
a 4 GiB container memory limit and zero container swap (`--memory-swap` equals
`--memory`), plus hard core limit zero. Actual mount type and limits are verified
before building or running fixtures; a disk-backed volume cannot substitute.
The owner-provisioned named-volume configuration below passed the effective
boundary checks and all 111 quick groups in CI.

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

### Runner provisioning

Forgejo runner 13.2.0 does not apply every workflow `container.options` field:
its [job-option merge](https://code.forgejo.org/forgejo/runner/src/tag/v13.2.0/act/container/docker/run.go)
accepts `--memory` but omits `--mount`, `--memory-swap` and `--ulimit`. The original
`pyxis` job therefore refused with `dedicated scratch mount is missing` before
the build or tests. Passing the same options directly to rootless Podman works
locally; this is not evidence that the CI runner applied them.

The filesystem jobs select the dedicated `pyxis-fs-ram` rootless runner. The
initial trusted anonymous-volume mount also failed: after the owner permitted
its empty source in `valid_volumes`, the mount existed but was not tmpfs, so
both jobs refused before building. A local Podman 5.8.1 Docker-compatible API
probe reproduced disk backing when creating an anonymous mount with
`VolumeOptions.DriverConfig.Options`, with either empty or explicit `local`
driver. Direct `podman run` had honored the same options; that earlier CLI
validation did not establish the API behavior. The API also rejects `nr_inodes`
in `HostConfig.Tmpfs`. No test payloads were written in these failed probes.

### Named-volume provisioning

**Accepted setup:** provision one named tmpfs volume under the runner's exact
rootless user and Podman store, then attach it by name. Reserve it exclusively
for this single validation runner, with capacity one. No other runner, helper
container or manual mount may use it. Complete job-container removal and volume
unmount before the next job; after interruption, the operator must remove stale
attachments before resuming. This adds an administrator-managed volume lifetime,
not a larger resource budget or a general runner framework.

Provision once, as that rootless user, without `--ignore`:

```sh
podman volume create --driver local \
  --opt type=tmpfs --opt device=tmpfs --opt nocopy \
  --opt o=rw,nosuid,nodev,size=2g,nr_inodes=65536 pyxis-fs-ram
```

The trusted runner fragment replaces the anonymous mount and its
empty-source permission. Preserve other necessary settings and existing exact
volume permissions; do not add a wildcard:

```yaml
runner:
  capacity: 1
  labels:
    - pyxis-fs-ram:docker://git.internal/pyxisos/pyxis-builder:pyxis-gcc16.2-binutils2.47
container:
  privileged: false
  options: >-
    --memory=4g --memory-swap=4g --ulimit core=0:0
    --volume pyxis-fs-ram:/run/pyxis-fs-ram:nocopy
  valid_volumes:
    - pyxis-fs-ram
```

`nocopy` prevents image-directory contents being allocated by the runtime before
the job. Podman [unmounts a volume when its last use ends](https://docs.podman.io/en/latest/markdown/podman-volume-unmount.1.html);
unmounting [tmpfs discards its files](https://docs.kernel.org/filesystems/tmpfs.html).
The volume definition persists, while each fully torn-down job loses its scratch
contents. The guard still checks actual backing/limits before building; creating
`scratch/tmp` fails if a prior job retained that directory. It does not establish
exclusive ownership by itself: the operator must maintain the exclusivity and
teardown preconditions. Fresh fixture pages are allocated by the verified
zero-swap job; neither old payloads nor shared backing files may be reused.

Local API validation passed two sequential create/start/remove cycles, each
verifying tmpfs, exact byte/inode caps, memory/swap/core limits and an empty mount.
A zero-byte marker from the first cycle was absent in the second. A separate
API run of the actual launcher passed all 111 quick groups, with a
262,553,600-byte memory peak and zero swap/max/OOM events. This establishes the
observed local runtime behavior, not successful CI provisioning or crash cleanup
on the owner's runner. Probe containers and volumes were removed.

Only filesystem jobs use this dedicated runner; ordinary image builds remain on
`pyxis`. No production/core change or guard relaxation is needed. The workflows
declare only the memory limit; the trusted runner configuration supplies the
mount, swap and core limits. [Pyxis run 577](https://git.internal/PyxisOS/pyxis-os/actions/runs/577)
verified the provisioned runner at parent `6a4525e` / filesystem `2d8ce96` on
2026-10-01: all 111 groups passed with 2,147,483,648 scratch bytes, 65,536 inodes,
4,294,967,296 memory-limit bytes, zero swap and zero max/OOM events. Peak memory
was 314,847,232 bytes on kernel `7.2.6-1-cachyos`. This verifies the effective
boundary for that run; exclusive ownership and teardown remain operator
preconditions, and every subsequent job must pass the guard again.

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
