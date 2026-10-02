# Bounded RAM validation

The safety boundary is the development VM itself. Outer-hypervisor settings are
not part of this task. Ordinary builds remain available with `make -j16`;
filesystem tests require the verified RAM boundary below. No allocator or
publication semantics change is included.

## Contracts and execution profiles

The local launcher's 2 GiB scratch, 4 GiB job and 65,536-inode defaults are the
selected execution profile for the recorded local runs. They are not filesystem
invariants, machine specifications or product-capacity requirements. The RAM-only
backing, disabled swap, bounded resource use and verified controls implement the
agreed storage-safety policy for these runs. Capacity one is a precondition of the current
shared-volume mechanism, not a permanent filesystem concurrency restriction.

Scratch byte/inode capacity and the job memory budget each have one provisioning
authority: the scoped local launcher selects its local limits; CI uses the owner's
trusted volume and runner configuration. The workflows do not override them.
Both guards verify finite positive scratch byte/inode capacity and a real finite
positive cgroup memory limit, zero swap allowance/current usage and disabled core
dumps, without copying the selected memory amount into another acceptance ceiling. CI records
the actual member-cgroup memory limit; an ancestor may impose a tighter bound.
The guards never raise limits or retry with more memory. Changing the provisioned
budget is an explicit operator action, not a filesystem behavior change.

The guards do not impose the local scratch defaults as independent acceptance
ceilings. The provisioner chooses capacity and each run records the effective
limits; historical measurements retain their original parameters. RAM-only
storage, zero swap and the shared runner's exclusive serial use remain required.

## Local execution

On Linux with cgroup v2, systemd, tmpfs `noswap`, Python 3 and the ordinary host
compiler, run the reviewed scoped launcher:

```sh
sudo python3 tests/ram_run.py --suite preflight
sudo python3 tests/ram_run.py --suite check
sudo python3 tests/ram_run.py --suite extended
sudo python3 tests/ram_run.py --suite safety
sudo python3 tests/ram_run.py --suite baseline
sudo python3 tests/ram_run.py --suite sustained --background-blocks 256 --case append
```

The safety, baseline and sustained suites also require loop devices, tracefs `block_bio_queue`,
and ext4/Btrfs kernel support and formatter tools. Unsupported evidence or
permissions cause refusal, without a buffered/disk-backed fallback. The launcher does not disable
system-wide swapping. It creates a fresh job before allocating workload storage:
2 GiB `tmpfs,noswap`, 65,536 inodes, 4 GiB `memory.max`, zero `memory.swap.max`,
and hard core limit zero. All fixture files use the verified pinned scratch
mount. The guard verifies actual cgroup membership and effective limits; an
environment variable alone is not evidence. The administrator must not change
these controls during a run.

Root sets up the local mount and cgroup boundary, then gives the RAM temporary
and build directories to the sudo caller's nonzero UID/GID. A root invocation
without a sudo caller, including a root CI container, selects `nobody` instead.
Before quick, extended or preflight work and the result-socket connection, the
worker clears supplementary groups, permanently drops its real/effective/saved
UIDs and GIDs, and sets `no_new_privs`. Compilation and test children therefore
run without root authority; the C fixture guard independently rejects UID 0.
The result socket authenticates the selected worker UID. For the safety, baseline
and sustained suites, the root supervisor connects to a socket expecting UID 0 and
retains the loop, mount and trace duties; compilation and comparison children
always use the unprivileged worker identity with `no_new_privs`.

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
| Expected payloads and source copies | Same RAM job; independent in-memory byte mirrors, images, logs and any source copies share the aggregate scratch/job limits |
| Safety/baseline/sustained native images | Fresh 1 GiB logical images in scratch, attached only to exclusively owned loop devices with verified backing identity |
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

`--suite baseline` retains its original workload and, without filters, its
40-case matrix. `--population`, `--case`, `--durability` and `--filesystem` select
a subset; for example, `--population 256 --case compiler --durability operation`
runs the individually durable Pyxis/ext4/Btrfs triplet. Pyxis has no batch profile.
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

Pyxis completes each mutation durably under its existing contract. Reclamation
may carry over until the explicit final checkpoint. Native
operation mode synchronizes a written file before returning; creation synchronizes
file and parent, rename synchronizes both affected parents (the same directory
in this matrix), and deletion synchronizes the parent. Close alone is not a
durability barrier. Native batch mode synchronizes after every 16 logical
create/write/rename/remove calls and at the final boundary. Its acknowledged
operations between barriers have a weaker recovery contract and are reported
separately. Both native modes finish with parent fsync, filesystem sync and clean
unmount. No cross-call batching is added to Pyxis.

The native comparison supports ext4 with an internal journal and single-device
Btrfs; external ext4 journal UUID/device fields and multi-device Btrfs are refused.
It requires held read-only descriptors for its workload root,
loop device and regular backing file, supplied by the launcher. Before workload
writes, it independently ties the actual root mount source to that autoclear
loop, verifies its backing identity, and requires the backing file to be on the
same verified bounded `tmpfs,noswap` mount as scratch. Only fresh single-device
ext4 and Btrfs mounts are supported. These checks establish this launcher's
storage boundary; they are not a general sandbox for arbitrary programs.

`--suite safety` checks this native launch boundary without running the full
comparison matrix. It runs one 32-file compiler case in operation mode for each
of ext4 and Btrfs, with bounded tracing and teardown. Five refusal cases cover
missing descriptors, a mismatched root, the wrong RAM backing file, a writable
loop descriptor and a read-only persistent backing file. It does not replace the
baseline matrix or establish new performance results.

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

## Sustained comparison

`--suite sustained` runs one chosen background population and case serially on
fresh Pyxis, ext4 and Btrfs backends. `--filesystem pyxis|ext4|btrfs` selects one
backend for controls or separate result collection. Each mutation uses operation
durability as described above; batch mode is refused. Compiler cases model
create/write/close/rename/delete histories, rather than invoking a compiler in
each cycle. Repetitions are separate serial launcher invocations.

The following defaults are configurable experiment inputs, not filesystem
contracts or acceptance targets. `--background-blocks` is the total number of
4-KiB background blocks distributed across all `--population` files, with at
least one block per file. Division remainders go to the first files.

| Option | Launcher default and use |
| --- | --- |
| `--population` | 64 background files |
| `--background-blocks` | 256 total background blocks |
| `--case` | `append`; alternatives are `overwrite` and `compiler` |
| `--windows` | 3 consecutive windows on the same populated backend |
| `--operations` | 512 mutations per append/overwrite window |
| `--cycles` | 128 compiler histories per compiler window |
| `--seed` | 1 for deterministic payloads and overwrite offsets; zero is accepted |
| `--target-bytes` | 4 MiB pre-existing overwrite target |
| `--overwrite-bytes` | 1024 bytes per overwrite |

For example, choose total background populations of 256, 2048 or 5120 blocks
with `--population 64`, then select one case per invocation. Append windows issue
4-KiB writes to one growing file. Overwrite preparation fills the configured target
using requests of at most 256 KiB; windows alternate contained and cross-block
writes without extending it. Compiler cycles write 4 KiB before rename/delete.
The background files remain unchanged throughout the windows. There is no extra
checkpoint or filesystem sync between windows. Preparation and the terminal
phase retain their explicit final boundaries. The current Pyxis E/M profile,
reserves and core memory cap are the same as the small comparison.

The summary's `phases` array contains `setup`, `preparation`, `window_1` through
`window_W`, and `final`. The child emits and stops at `setup_end`,
`preparation_end`, each `window_i_end`, and `final_end`: exactly W+3 boundaries.
The launcher checks marker order and stop count. Final checkpoint/synchronization
and core close precede `final_end`; verification follows it. Pyxis phase records
include application bytes, operations, elapsed time, user/orphan/drain data and
metadata writes, flushes, reads/planning reads, and selected extent/metadata/map,
live/reusable-block and generation state. `map_plans.phase_order` uses the same
phase sequence and separates local/bulk planning evidence and fallback reasons.

Sustained totals include the full write history. Pyxis `setup` counts formatter
and writer-open callbacks, and its total sums setup, preparation, all windows and
final maintenance/close. Simulator duplicate log writes remain infrastructure,
not filesystem submitted bytes. Native tracing begins before formatting and
continues through clean unmount. The first setup snapshot includes mkfs/mount
writes; subsequent phase bytes, flush and FUA counts are snapshot deltas. Writes
after `final_end` remain in `after_end_submitted_bytes` and the total, rather than
being dropped or assigned to a window. `after_end_seconds` covers verification,
result handoff and clean unmount; `external_setup_seconds` records external
setup, and native setup elapsed time includes it. Submitted bytes must still
equal the owned loop's completed write bytes after teardown. Historic baseline
Pyxis preparation/measurement counts continue to exclude formatting/open;
its native preparation retains format-inclusive traffic. Those historic phase
denominators must not be substituted for sustained setup-inclusive totals.

An independently updated byte mirror and namespace ledger record requested
payloads and confirmed progress. After the final boundary, Pyxis reopens read-only;
both backends check every surviving byte, file length and expected namespace
against that oracle. Verification does not regenerate expected contents by
replaying the workload's offset/tag decisions. A healthy quota, profile or space
refusal produces `complete: false` with the refusing phase/operation, status,
confirmed bytes and namespace progress, followed by verification of the confirmed
prefix. Unexecuted windows remain empty diagnostic phases. Such a record is a
valid incomplete result, not completion of the requested history. I/O or stopped
writer errors, adapter failures, OOM, trace loss or execution-budget exhaustion invalidate
the experiment and do not produce a successful prefix claim.

This suite retains the existing RAM-only, zero-swap, identity, native-target,
trace-loss and submitted/completed guards. Scratch/job/output/trace budgets remain
unchanged, as do the 600-second child and 1200-second service limits. A selected
configuration must fit those limits; there is no automatic limit increase or
shortened retry. The summary also records compiler/version and actual build
commands, core and job memory evidence, oracle capacity, build allocation and
case-end scratch/backing physical allocation snapshots. End snapshots are not
peak storage measurements. Quick and extended contract coverage remains separate
and unchanged; sustained comparison is a diagnostic experiment, not writable
deployment or real-device durability qualification.

## CI

The quick filesystem PR job requires tmpfs with finite positive byte and inode
limits selected by the provisioner, a finite positive container memory limit
selected by the owner and zero container swap (`--memory-swap` equals `--memory`),
plus hard core limit zero. Actual mount type and limits are verified before
building or running fixtures; a disk-backed volume cannot substitute.
The historical 4 GiB named-volume configuration passed the effective boundary
checks and all 111 quick groups in CI; its evidence is recorded below.

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

The filesystem jobs select the `pyxis-fs-ram` rootless runner label. The
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
for this single runner, with capacity one. It may advertise both `pyxis` and
`pyxis-fs-ram`; its limits and mount then apply to both kinds of job. No other runner, helper
container or manual mount may use it. Complete job-container removal and volume
unmount before the next job; after interruption, the operator must remove stale
attachments before resuming. This adds an administrator-managed volume lifetime,
not a larger resource budget or a general runner framework.

Provision once, as that rootless user, without `--ignore`. The following volume
and runner fragments are an illustrative profile: their 2 GiB scratch,
65,536-inode and 8 GiB job amounts are provisioning choices, not guard
requirements. Select and maintain the intended capacity in these authoritative
settings:

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
    - pyxis:docker://docker.io/library/node:24-bookworm
    - pyxis-fs-ram:docker://git.internal/pyxisos/pyxis-builder:pyxis-gcc16.2-binutils2.47
container:
  privileged: false
  options: >-
    --memory=8g --memory-swap=8g --ulimit core=0:0
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

Filesystem jobs select `pyxis-fs-ram`; ordinary image builds select `pyxis`.
Both may use the shared serial runner. Its trusted configuration supplies memory,
mount, swap and core limits together; the workflows supply no memory override.
Previously overriding only memory to 4 GiB while the runner supplied a 16 GiB
memory-plus-swap limit allowed swap, and preflight correctly refused before tests.
The corrected guards accept the configured finite memory budget and retain the
zero-swap requirement. No production/core change is included.

Historical [Pyxis run 577](https://git.internal/PyxisOS/pyxis-os/actions/runs/577)
verified the provisioned runner at parent `6a4525e` / filesystem `2d8ce96` on
2026-10-01: all 111 groups passed with 2,147,483,648 scratch bytes, 65,536 inodes,
4,294,967,296 memory-limit bytes, zero swap and zero max/OOM events. Peak memory
was 314,847,232 bytes on kernel `7.2.6-1-cachyos`. This verifies the effective
boundary for that run; exclusive ownership and teardown remain operator
preconditions, and every subsequent job must pass the guard again.

The earlier memory-budget correction passed all 111 quick groups in the scoped local
4 GiB launcher, with a 305,643,520-byte peak and zero swap/max/OOM events.
Separate actual-cgroup probes configured 1 GiB and 16 GiB caps: both the Python
preflight and C guard accepted each with zero swap. Both refused an unlimited
memory hierarchy and a 16 GiB job with a 1 MiB swap allowance, before fixture
writes. These are deliberately varied deployment inputs, not accepted filesystem
capacity limits. No large recovery workload or comparative matrix was rerun.

The privilege/scratch review correction passed 111 quick and six extended groups
as worker UID/GID 1000, with zero swap/max/OOM events. Peaks were 307,220,480 and
493,228,032 bytes respectively. Actual scratch probes configured 64 MiB / 2,048
inodes and 3 GiB / 131,072 inodes; both guards accepted these distinct bounded
inputs, and refused unlimited bytes or inodes. The C guard refused a root
workload, and socket probes accepted the selected non-root peer and rejected a
different UID. These tests did not allocate payloads up to the configured caps.

The final native safety run passed both 32-file/compiler cases and ten refusal
checks as UID/GID 1000. Tracing counted 55,422,976 ext4 and 21,716,992 Btrfs submitted
bytes including formatting and final unmount, entirely on RAM-backed loops; no
trace loss/budget failure occurred. Peak job memory was 300,560,384 bytes, with
zero swap/max/OOM events. Four additional in-memory journal-field probes accepted
an internal ext4 journal and refused external UUID/device or missing internal
inode fields. The scoped missing-scratch probe preserved its diagnostic through
the authenticated non-root result socket. These are small safety checks, not a
new performance baseline or real-host durability qualification. The original
comparison measurements and primary timing table are unchanged.

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
