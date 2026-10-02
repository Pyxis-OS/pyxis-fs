# Maintained host contract tests

Run the deterministic PR suite in the [bounded RAM setup](ram-validation.md):

```sh
sudo python3 tests/ram_run.py --suite check
```

Historical commands and observations below do not waive that storage boundary.
Bare `make check` now refuses unless its actual scratch/cgroup/core limits pass
the same guard and the process has a nonzero UID. Scratch and job capacity are
provisioned inputs; the guard requires finite positive limits rather than the
local launcher's particular defaults. The larger recovery workload is not
currently authorized. The [initial small RAM baseline](ram-baseline.md) records
the initial scoped validation
and the Pyxis/ext4/Btrfs comparison; historical disk-backed timings below remain
historical evidence.

The launcher gives RAM temporary/build directories to its selected non-root
worker, clears supplementary groups, permanently drops UIDs/GIDs and sets
`no_new_privs` before quick, extended or preflight work and result reporting.
Compilation and comparison children also run unprivileged in the native safety
suite, baseline and sustained comparison; their supervisor retains the loop,
mount and trace duties. The
[launcher contract](ram-validation.md#local-execution) describes identity selection
and result-socket authentication, and the
[comparison contract](ram-validation.md#small-comparison-contract) describes the
held descriptors that independently verify native RAM backing.

Run `sudo python3 tests/ram_run.py --suite safety` for the bounded native launch
check: one 32-file compiler operation case each on ext4 and Btrfs, plus five
descriptor/backing refusal cases. This checks the launch boundary without a full
comparative matrix; it adds no timing evidence to the initial baseline report.

The [sustained comparison](ram-validation.md#sustained-comparison) uses
`sudo python3 tests/ram_run.py --suite sustained --background-blocks 256 --case append`
for one configurable experiment, with serial Pyxis/ext4/Btrfs operation-durable
runs. Select `overwrite` or `compiler`, or a single `--filesystem`, explicitly.
Independent byte mirrors and namespace expectations verify the resulting history;
`complete: false` reports a verified healthy capacity-refused prefix, not a full
workload pass. Setup, preparation, successive windows and final maintenance have
separate diagnostics, and native verification/handoff/unmount writes remain in
the total. The existing small baseline and quick/extended suites retain their
coverage and storage guards. This adds comparison instrumentation, not new quick
or extended groups or authorization for the larger recovery command.

`make check` builds `build/pyxis-fs-tests` and runs `--suite pr`. It links the same
freestanding `libpyxis-fs.a` as the ordinary tools, with no alternate core or
production test switches. Unity v2.7.0 is pinned under `third_party/unity` with
its upstream revision and MIT license. The runner fails on any failure, ignored
test or empty suite. An unknown suite is an argument error.

Tests validate behavior, documented invariants, corruption handling and recovery
semantics as they land. They must not duplicate incidental implementation details
or current fixture layouts. Prefer small synthetic fixtures and contract
properties so implementation refactors and harmless layout changes do not require
broad test rewrites. Exact bytes/offsets are appropriate where the disk format
specifies them. Expected results must be independently defined: agreement between
our encoder, reader and checker is not sufficient evidence.

Configuration, authored or draft data, benchmark parameters, machine properties
and convenient fixtures do not become contracts by appearing in tests or
validators. Before adding an assertion, identify the deliberate contract that
changing the value would violate. A setting has one authority: configure a chosen
input and verify its propagation or behavior, rather than asserting the current
default independently in multiple layers. A synthetic fixture may deliberately
choose exact inputs and independent expected results for its scenario; its current
contents do not define which other inputs the filesystem accepts.

The same rule applies to structure and ownership. Investigate duplicate mutable
authorities instead of adding copies and consistency assertions around them.
When replacing an implementation, reassess which old expectations remain real
contracts. Passing tests and matching documentation do not establish that the
underlying architecture is appropriate. Execution-profile checks belong to the
[launcher boundary](ram-validation.md#contracts-and-execution-profiles), separate
from filesystem correctness.

Publication cuts follow healthy callback roles rather than saved operation
ordinals. Trace discovery still enforces the deliberate replacement-write/flush/
slot-write/flush protocol; it does not prescribe a maintenance-publication count.
Retained payload comparisons cover every observed slot write, using independently
expected bytes for each retained state. Editor assertions preserve mappings,
ordering, exact minima, occupancy, reachability and new/retired-node bounds without
requiring one split or repair shape. Deliberately constructed maximum-depth
worst-case witnesses remain explicit. Stopped-state behavior is exercised through
real adapter failures and reported through the public status interface.

## Current coverage

The suite includes the following test groups. The runner prints each group and the
executed total; several groups contain boundary tables or repeated edit histories.

| Area | Contract assertions |
| --- | --- |
| Read-only baseline | Known CRC32C result; independent payload, name and kind expectations; held rights and denial; pool/volume/view lifetimes; failed-open allocation cleanup |
| Encoding and corruption | Independently assembled orphan/volume vectors; feature scope; checkpoint and unknown rights; reserved bytes, padding and compatible extensions; canonical block/tree encoding |
| Orphan recognition | Named-or-orphan exclusivity, missing/root/parent conflicts, empty orphan directories, retained file data/grants, diagnostic reads and refusal of fresh ordinary acquisition |
| Formatter | Root exceptions, short leaf/internal tails and mixed/minimum/maximum names; occupancy, exact minima, payloads, grants and metadata/allocation accounting across 30 populated fixtures |
| Private tree edits | Independently maintained expected entries through insert/update/delete histories; exact minima, ordering, byte fit, namespace occupancy, split/merge/redistribution/root collapse; maximum-depth sparse deletion, separator growth during namespace deletion, unpublished buffer reuse and atomic refusal; overlapping newly split extents rejected while touching ranges remain valid |
| Private map planning | Independent interval/ownership expectations, canonical coalescing, unchanged inputs and refusal outputs; inconsistent deltas distinguished from corrupt bases; fragmented interior allocations accounting for every replacement map/catalog/root block, exact finite bounds and reachable nonempty nodes |
| Publication and recovery | Real private COW overwrite through user and maintenance publications; each replacement/slot write and every publication flush discovered from a healthy trace; durable recovery, sticky health and cache-visible/non-pending-slot counterexample; both retained payloads compared before every observed slot write |
| Writable admission and views | Both-state canonical validation, quota/deletion/profile/reserve/memory boundaries; ordinary live reads and checkpoint authority without read/lookup; callback reentry refusal; funded startup orphan cleanup and refusal before writes when recovery admission fails |
| Reserved memory | Accepted profile arithmetic, capped arena reservation, allocation failure, release and no further allocation/I/O during private map planning |

On 2026-10-01 the 30 task-2 groups passed with native GCC 16.2.1 in 2.22 seconds
(`make check`, including incremental runner rebuild work). They also passed with
ASan/UBSan under GCC 14.2.0 in the existing builder container. Pyxis GCC 16.2.0
compiled the freestanding archive, and the parent kernel and host tools built.
A fresh 64 MiB source import passed both-state checking and byte-for-byte
extraction comparison. These are bounded host observations; no QEMU/guest or
large-workload validation is claimed.

The combined review adds two focused regression groups (32 total): split-boundary
extent adjacency and map-delta error classification. All 32 pass in the native
host build and with ASan/UBSan in the existing builder container; the Pyxis target
archive also compiles. The split regression fails
against the previous editor, which incorrectly accepts `[111,113)` into a leaf
of 57 one-block extents at `0,2,...,112`. The corrected editor rejects it without
changing the candidate and accepts `[111,112)`. These tests do not establish
suitability for Caelum's kernel-task stack; the native integration prerequisite
is recorded in [private candidate planning](core.md#private-candidate-planning).

Fixture storage and allocator controls live in `tests/support.c`, behind the
existing platform callbacks. Temporary sparse files satisfy the format geometry
floor without allocating a large empty-image payload. Allocation callbacks track
ownership and allow deterministic failure; teardown reports leaked core resources.
Test-controlled buffer editing is confined to the test sources. The bounded failure adapter in `tests/failure.c` uses the same callback boundary;
there is no production test hook or claim of physical host preallocation.

## Tasks 3/4 validation observations

At `f2f817b` (2026-10-01), all 68 groups pass with native GCC 16.2.1 and
with ASan/UBSan under GCC 14.2.0 in the existing builder container. The Pyxis GCC
16.2.0 freestanding archive compiles. An uninstrumented suite invocation took
2.46 seconds on the development Linux host; this is an observation, not a CI
latency guarantee. The ordinary host command opened a 64 MiB source-populated
image and exercised authorized checkpoint, grant denial and lock conflict.

The combined-review follow-up adds the conservative planning/maintenance read-error
cases and allocation-delta sorting coverage described below. All 71 groups pass
natively and under ASan/UBSan with the same compilers; ordinary host tools and the
Pyxis freestanding archive also build. The earlier timings above were not rerun
and are not measurements of the sorting change. Linking the revised test runner
against the pre-review writer (`e768b6c`) fails exactly the two new planning and
maintenance read-error groups: it incorrectly reports `READABLE_STOPPED`.

The initial synchronous-drain qualification used 16,384 blocks, two objects and
one data mapping. Its then-current E=1/M=3 profile computed H=27, recovery=465, Pmax=29 and an arena of
9,644,032 bytes. Ordinary and migration reserves stay at their 1,024-block
formatter floors; quota=4 and a separate exact pool-promise boundary are covered.
The same small reachable fixture with E=4096/M=2048 computes H=375,
recovery=1509, Pmax=377 and a 19,917,616-byte arena. Exact computed reserves
succeed and one block below refuses before writes. Capped reserved-memory drains,
occupied retirement tails, generation MAX-3 through MAX, and later reuse are
exercised. The larger declared profile is a resource-boundary test, not evidence
of a populated 4/64/256 GiB workload or maximum-density map history.

A matched read-only comparison used the unchanged task-2 binary at `642ad5f`
(the same source tree as merged `e1536f3`) and the task-3/4 binary at `f2f817b`.
The comparison was collected during integration using the preserved prior
revision, rather than captured before coding. The image imports that revision's
`core`, `include`, `host`, `docs` and `tests` directories: 68 objects, 221 volume
blocks, guarantee=0, quota=8192 and 1024 blocks in each reserve. It is populated
source data, with no writable history. Both tools used their default 128 MiB
memory cap, 4096-byte blocks and a 16-block transfer maximum, native `-O2 -g3`,
on Linux 6.19.10/glibc 2.43. These are host process timings, not guest/device
power-loss or throughput results.

Each sample below averages 20 separate invocations timed with Python
`time.monotonic`; five alternating baseline/current samples used the same warm
image. No compiler or sanitizer job ran during the paired comparison.

| Command | Five sample means, milliseconds per call |
| --- | --- |
| Previous `pyxisfs-inspect --image IMAGE check` | 2.721, 2.623, 2.675, 2.666, 2.615 |
| Current same command | 2.503, 2.499, 2.538, 2.624, 2.580 |
| Current `pyxisfs-write --image IMAGE --extents 512 --metadata 256 open` | 7.822, 7.567, 7.745, 7.885, 7.618 |

This small comparison shows no material read-only regression; it is not a speedup
claim. Writable opening has no prior implementation baseline and includes full
retained validation, canonical rereads, arena reservation and a healthy flush.
The returned arena is 10,918,224 bytes with recovery=594 and Pmax=72. Useful write
throughput and metadata amplification on large sequential and separately committed
small-write workloads remain task-5/7 measurements. A protocol's six-flush count
is not a latency measurement; simulator backing/log overhead is not host writer
throughput.

## CI and scope limits

The `Filesystem` workflow builds and runs the quick suite on each PR and push
to main in the verified RAM boundary, using the existing Pyxis builder image. The `host-contract` job has a
five-minute timeout; the quick suite is intended to stay comfortably below one
minute on the normal runner. The emitted status is `Filesystem / host-contract (pull_request)`; the repository
owner must add that exact pattern to required status checks in branch protection. This repository owns the check; the parent
Pyxis build does not substitute for it.

Passing establishes the exercised core contracts for these bounded inputs. It
does not prove all possible admission/drain states, actual-device
power-loss/recovery safety, host durability qualification, whole-filesystem scale,
performance targets or guest behavior. Private planners trust documented caller
proofs for canonical source closure, ownership, cross-state reusable ranges and
ended pins; their tests do not manufacture those proofs. Calculated envelopes
are distinct from measured memory and performance.

Tasks 3/4 integrate a bounded failure adapter into this runner. It separates
visible, pending and explicitly durable bytes using sparse temporary files, a
bounded pending log and one bounded transfer buffer. Fault controls and event
observers remain test-only. The core receives its ordinary synchronous callbacks;
there is no alternate publisher. Log/trace exhaustion is a failed experiment,
not a simulated success.

The quick publication fixture changes an actual file's data and object-tree path
through the real COW editor/admitted publisher. Expected contents are supplied by
the workload. A separate retained-root traversal compares both durable payloads
while they differ, after replacement writes and before the user and each
maintenance slot write. The observer does not prescribe allocator placement.
The suite then checks later reuse. Retirement checkpoints preserve held authority
and classify replacement/pre-slot failure separately from slot uncertainty. A
checkpoint with no volume debt completes without publication; it supplies no
publication evidence.

The bounded matrix covers each replacement write and slot write in that fixture,
publication flushes with none/all pending writes durable, actual slot tears, and
cleanup/fence failure after confirmed user progress. Cold recovery uses a
clone of the simulator's explicitly durable image. The cache-only slot case
shows cached structural validation plus a successful later flush disagreeing
with durable storage, while the failed core remains stopped. It does not assert
that a fresh core can detect adapter history absent from its inputs.

Focused read-error cases discover every backing-read boundary during the small
fixture's user publication planning and requested maintenance publications. A
transient read failure must stop all ordinary access, preserve confirmed progress and remain
sticky until close. Fresh validated recovery uses the explicitly durable clone.
Existing write/flush cases separately require readable pre-slot failure and
access-stopping publication uncertainty.

Allocation deltas use in-place heapsort within reserved storage. Tests assert
ordering and preservation of complete delta contents for empty, single, ordered,
reversed, mixed and equal-key inputs. Replacing insertion sort removes its
calculated quadratic worst-case sorting cost; no sorting benchmark or measured
throughput improvement is claimed.

Unexpected resource failure during funded cleanup/fencing is an invariant failure,
not successful safe refusal. Resource cases deny further allocation after admission
and require the funded work to finish. Ordinary refusal before admission leaves the
writer usable. Task 6 adds allocation-free final orphan release and funded startup
cleanup, including their resource and interruption cases below.

Task 5 adds public file mutation and the separate populated write-history command
below. Task 6 adds namespace/orphan cases. Larger pressure and extended
failure campaigns remain task 7; the file-workload command is not those campaigns. Task 7 records combined results rather than first adding tests.
Host tests remain maintained after milestone closure. Ordinary native builds,
freestanding target compilation and later guest validation remain separate checks.

Optional local memory/undefined-behavior instrumentation uses a separate build
and requires the host compiler's sanitizer runtimes:

```sh
make -j16 BUILD=/tmp/pyxis-fs-sanitize \
  CFLAGS='-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' check
```


## Task 5 file mutation validation

At implementation `fa78682` with workload reporting `0245e12`, the ordinary host
build and Pyxis GCC 16.2.0 freestanding archive pass. The maintained quick suite
now has 86 groups (the final allocation-failure campaign is `25760f9`). It covers
public create/write/resize, complete-request authority checks, sparse and partial
blocks, crossing the old EOF, shrink/regrow zeroing, live reads, partial progress,
quota/profile/generation refusal, and publication/cleanup failure provenance.
Every discovered allocation point in a create-with-child request is denied in
turn and must fail before namespace publication. Long-name history creates 120
files across tree splits, independently checks names, ownership, empty contents
and grants, then validates/reopens. Live tokens check local invalidation and
instance binding; ordinary writer identities suppress the pool generation.

The public overwrite/reuse/shrink scenario compares both retained durable payloads
after replacement writes and before each user and maintenance slot write. Its
expected bytes/lengths come from the workload. It complements the broader private
publication boundary matrix; it is not an exhaustive public-operation crash matrix.
The same 86 groups and the separate workload below pass ASan/UBSan in the existing
GCC 14.2.0 builder container. Healthy Linux command/extraction observations are
recorded in [host validation](host-tools.md#file-command-validation).

### Separate populated file workload

```sh
make -j16 all check
build/pyxis-fs-tests --suite file-workloads
# Optional instrumentation, including the same larger scenario:
make -j16 BUILD=/tmp/pyxis-fs-sanitize \
  CFLAGS='-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' check
/tmp/pyxis-fs-sanitize/pyxis-fs-tests --suite file-workloads
```

`make check` remains the deterministic per-PR gate. `file-workloads` is one
separate deterministic scenario using the same real core and bounded failure
adapter; no second writer or failure framework is used. It snapshots 12 actual
core/header source files (154978 bytes at the measured revision), in two source
directories, into a 4 GiB sparse image, then creates two output files. E=8192,
M=4096, a 128 MiB core cap, and 8192 blocks in each persisted reserve are explicit.
It writes 4 MiB sequentially in sixteen 256 KiB calls, appends sixty-four separately
committed 4 KiB blocks to the other file, performs sixteen deterministic partial
overwrites (seed 1), and shrinks/regrows. After an explicitly durable cold cut,
it independently compares both output files and every imported source byte.
Source/layout changes are allowed; no current fixture bytes or allocator placements
are golden test expectations. The binary locates source inputs beside its compiled
source path, so retain that source checkout when running this optional workload.

On Linux 6.19.10 x86_64, native GCC 16.2.1, `-O2 -g3`, three sequential uninstrumented
samples at `0245e12` gave the following observations. No build or sanitizer job
ran concurrently. These timings include core planning and synchronous simulator
I/O/drain, not real-device flush latency or guest/NVMe performance. Metadata bytes
include slots and both volume/pool metadata; useful bytes count supplied write
payloads, not zero-filled sparse growth or logical bytes removed.

| Phase | Useful bytes | Elapsed seconds, three samples | Mean call, sample 1 | Throughput range | User / drain metadata bytes per useful byte |
| --- | ---: | --- | ---: | ---: | --- |
| Sixteen 256 KiB writes | 4194304 | 1.099903, 1.099280, 1.098652 | 68.744 ms | 3.637–3.641 MiB/s | 0.124 / 0.162 |
| Sixty-four separate 4 KiB appends | 262144 | 0.343110, 0.342810, 0.343171 | 5.361 ms | 0.728–0.729 MiB/s | 10.281 / 15.344 |
| Sixteen partial overwrites | 131088 | 0.206778, 0.206403, 0.206301 | 12.924 ms | 0.605–0.606 MiB/s | 6.812 / 9.249 |
| Shrink and regrow | 0 | 0.093596, 0.093294, 0.093493 | 46.798 ms | Not a useful-byte throughput measure | 598016 / 868352 metadata bytes total |

Sample 1 maximum call latencies were 89.325, 6.119, 14.522 and 87.931 ms in phase
order. Sequential writing used 16 user and 32 maintenance publications; separate
appends used 64 and 128. The sequential file had 84 extents after its 16 calls,
reflecting actual fragmented reuse rather than an assumed contiguous allocation;
the independently appended file had 64 extents. After overwrites the sequential
file had 116 extents, and after shrink/regrow it had 51. The final total included
12 imported extents: 127 file extents, 9 volume metadata blocks and 153 allocation
records. Charged core memory after reopen with the two views was 30,585,592 bytes;
this is not a peak RSS measurement and excludes simulator files/logs and workload
buffers. The live file editor occupies 1,315,288 bytes of already reserved scratch
(`sizeof`, not an extra allocation).

These are small populated/history examples, not validation of full E/M occupancy,
64/256 GiB workload capacity, all possible fragmentation or performance targets.
The image's sparse 4 GiB size alone establishes none of those properties. Whole-map
rebuilding remains provisional. Code inspection also identifies full admitted-claim
scans and a 581,272-byte staging checkpoint copy per attempted write block; the
measured latency is not a profiler attribution to either. Revisit these costs with
the larger task-7 workloads before selecting allocation/planning optimizations.

### Matched unchanged host commands

A separate 64 MiB image imported `342e92d`'s `core`, `include`, `host`, `docs` and
`tests` via `git archive`: 86 objects, 295 allocated volume blocks, guarantee=0,
quota=8192 blocks, and 1024 blocks in each reserve. Prior `e768b6c` (same filesystem
source as merged `342e92d`) and task-5 `fa78682` host tools used the same unchanged
warm image and default memory cap. `open` used E=512/M=256. Each table entry is the
mean of twenty separate invocations measured with Python `time.monotonic`; five
samples alternated prior/current order. No concurrent build/instrumentation ran.

| Command | Prior mean milliseconds per call, five samples | Task 5 mean milliseconds per call, five samples |
| --- | --- | --- |
| `pyxisfs-inspect --image IMAGE check` | 2.840, 2.855, 2.765, 2.879, 2.688 | 3.128, 3.030, 2.726, 2.690, 3.035 |
| `pyxisfs-write --image IMAGE --extents 512 --metadata 256 open` | 8.355, 8.327, 7.998, 8.113, 8.051 | 8.321, 8.188, 8.214, 7.971, 8.127 |

The check sample ranges overlap, with the task-5 average roughly 4% higher; this
small noisy observation does not isolate a regression or establish equal cost.
Writable-open timings are similar. Public mutation has no prior implementation
baseline; the workload table measures its first implementation without claiming
an improvement over absent behavior. No QEMU/build defaults changed.

## Task 6 namespace and orphan validation

At implementation `4d64eeb` on 2026-10-01, all 106 quick groups pass with native
GCC 16.2.1 and under ASan/UBSan with GCC 14.2.0 in the existing builder container.
The separate populated `file-workloads` scenario also passes both. Ordinary host
tools, the Pyxis GCC 16.2.0 freestanding archive and the parent read-only kernel
build pass. No QEMU/guest writer validation or new performance measurement is
claimed by these correctness runs.

The review correction adds one persisted-accounting regression (107 groups total,
all passing natively and under ASan/UBSan). It traverses the durable allocation
map after successful flushes: orphan cleanup on final release and startup must
charge retired volume blocks to recovery workspace, while named writes, unlink
and retained-orphan writes use ordinary workspace. Recorded workspace occupancy
must equal the corresponding map charges. Relinking against the prior publisher
from `044c5e2` fails exactly this new group on the incorrect retirement charge.

The maintained quick suite now covers directory creation/removal, same-volume
regular-file rename/replacement, explicit replacement intent, exact held rights,
parent ownership, retained file identity/content, narrowed delegation, detached
directories and recreation of a removed name. Fresh acquisition cannot obtain an
orphan. Live directory tokens check local invalidation, including both rename
parents; retained file writes do not invalidate unrelated directories. Maximum
255-byte names exercise creation and cross-directory rename.

Resource cases use two volumes at the formatter's minimum pool geometry. One
fixture has four file data blocks and seven explicit grants: quota=9, E=1, M=7,
ordinary/migration reserves at their 1024-block floors and recovery at the larger
of the computed requirement and formatter floor. Ordinary growth refuses before
writes, while unlink and final cleanup must succeed. A separate exact pool-promise
boundary is checked. Startup recovery uses the same minimum admitted profile;
allocation counters from its first write through return must remain unchanged.
Final release runs with every subsequent allocation denied. Refusal during these
funded paths fails the test; it is not counted as successful safe handling.

The larger quick pressure case creates 120 retained empty victims with maximum
length names, crossing the orphan index's single-leaf capacity. It fills quota=48
with actual file writes until ordinary growth refuses, then unlinks every victim
and requires all final releases to complete with allocation disabled. This also
exercises final root collapse where deletion retires metadata without producing
new volume nodes. Generation-boundary coverage funds four data blocks, seven
grants and final paired deletion at MAX-39; one generation less headroom refuses
the unlink before publication. These are bounded histories, not all possible
namespace heights, occupancy patterns or full-profile populations.

The shared failure adapter discovers replacement publication flush boundaries
from a healthy execution trace and checks every observed phase, including uncertainty after a failed flush has made pending writes
durable. Expected names, source/victim identities and different payloads are
supplied independently. Planning and maintenance read failures require
`ACCESS_STOPPED`, with confirmed namespace progress retained. Focused final-close
and startup cases interrupt each observed cleanup publication phase, check
remaining paired object/orphan records, original payload prefixes and bounded grants,
then require a qualified cold
reopen of the explicitly durable image to finish. The consumed close reference
and failed-open resource release are checked separately from cleanup completion.
Existing overwrite/reuse tests still compare both retained payloads during each
maintenance publication; structural agreement alone is not the content oracle.

The [healthy host command observations](host-tools.md#namespace-command-validation)
cover explicit replacement, long names, refusal/no-op behavior and extraction.
Real-host post-error or interrupted-session recovery is still unqualified. The
larger combined pressure and failure campaigns remain task 7; no native guest
writer or kernel-stack suitability is established here.

## Extended campaigns and recovery workload

The longer scenarios are explicitly invoked in the same Unity runner. `make check`
continues to run the quick contract suite and remains the per-PR CI gate:

```sh
make -j16 all check
TMPDIR=/path/to/test-storage make check-extended
# A different reproducible operation history:
TMPDIR=/path/to/test-storage build/pyxis-fs-tests --suite extended --seed 2
TMPDIR=/path/to/test-storage build/pyxis-fs-tests \
  --suite workload --profile recovery --seed 1 --source /path/to/census
```

`check-extended` selects seed 1. Seeds must be positive unsigned 64-bit integers.
Unknown/duplicate arguments, missing inputs and unsupported profiles fail before
running tests. Only the recovery workload profile is implemented in this delivery;
the development/physical profiles and milestone closure remain separate work.
The smaller `--suite file-workloads` command remains available with its existing
scope. Extended/workload failures return a failing process status, including
infrastructure exhaustion and a profile that cannot finish its declared history.
No resource refusal during an admitted drain counts as success.

`TMPDIR` selects an existing directory for unlinked fixture files and the bounded
failure log; when unset, the runner uses the host's ordinary temporary-file
location. Provision storage explicitly for long campaigns. The depth-eight editor
fixture alone writes about 437.4 MiB of metadata; the recovery workload needs space
for its image, independently expected output files and source snapshots. A sparse
4 GiB geometry is not physical preallocation. Test allocator high-water reporting
counts bytes charged through the capped memory owner, not process RSS, allocator
bookkeeping, the host page cache or all harness storage.

### Reproducing the agreed source census

The workload takes an explicit existing directory. It neither fetches repositories
nor reads an implicit checkout's changing contents. The accepted census uses four
historical revisions; from a Pyxis checkout with those objects available locally,
create a new snapshot directory on storage with enough room:

```sh
pfs_census=$(mktemp -d /path/to/storage/pyxis-census-XXXXXX)
set -o pipefail
git archive --prefix=pyxis/ e4a83ba8b318bebeb4f15550f240eb1ac484521e | tar -xf - -C "$pfs_census"
git -C fs archive --prefix=fs/ 82cc242b3d9773d21c0f7e7a71ec9ca9ccb937ed | tar -xf - -C "$pfs_census"
git -C userspace archive --prefix=userspace/ f24e9d9f8523b1c1571f8b3034ef1513c3705bb3 | tar -xf - -C "$pfs_census"
git -C ports archive --prefix=ports/ fc728f7643f1e99712c8b2523bd7148fa943b782 | tar -xf - -C "$pfs_census"
# Gitlinks can leave empty directories; the census counts regular file blobs.
find "$pfs_census" -depth -type d -empty -delete
```

This produces 790 nonempty files, 132 directories including the common root,
922 objects and 12,460,032 source bytes. The archive instructions were independently
compared byte-for-byte with a `git ls-tree`/`git cat-file` blob snapshot. These
counts describe this input, not hard-coded content/layout expectations for all
source trees. The workload snapshots supplied contents, normalizes path order and
fixture identities, and uses independently expected files for streaming comparisons.

### Bounded extended coverage

The six extended groups reuse the quick suite's fixture callbacks and failure
adapter. A seeded variable-name editor history mixes one-byte and 255-byte names,
separator growth, removal and reinsertion. A separate valid depth-eight namespace
contains 559,872 entries in 111,975 tree nodes and exercises extremal deletion and
reinsertion. Independent key/incarnation expectations, occupancy, exact minima,
ordering and reachability are checked across the whole tree. The editor asserts
upper bounds of 15 new/retired nodes, rather than a particular repair layout.
This private-editor fixture is not full-volume admission at that population.

A retained namespace history covers 32 incarnations, 23 replacements and eight
unlink/name-reuse cycles, ending with independently expected names, identities and
payloads after cold reopen. Three campaigns first discover a healthy publication
trace, then apply 432 selected cuts: 39 for cross-directory replacement, 195 for
multi-batch final release and 198 for startup cleanup. The cleanup victim has 131
data blocks and seven explicit grants; healthy cleanup uses 15 publications.
Each publication tests its first replacement write and slot write, and every
flush: failures before/pending/durable writes, no/first/all pending promotion,
32/64-byte slot tears and cache-only non-pending slots. These are not all possible
replacement writes, tear lengths, subsets, read failures or combinations.

Before each publication's first replacement write, the observer freezes the
durable slots' generation and victim presence/length. Before the slot write it
requires those exact retained states and independently expected payload bytes,
including the remaining victim prefix during cleanup. Early replacement-write
or first-flush failures also compare both frozen states before recovery. Thus
overwritten metadata cannot shorten the expected payload after the fact. This
checks preservation without allocator placements or a fixed cleanup partition.
Sticky health, consumed final-close references, withheld startup handles,
degraded-slot refusal and qualified cold recovery are separate assertions. A
cache-visible image is never substituted for the explicitly durable recovery
input. This does not qualify real-host post-error recovery.

### Recovery workload configuration and outcomes

The recovery run uses a chosen measurement configuration of one 4 GiB volume,
E=8192, M=4096 and a 128 MiB charged memory cap. It uses ordinary/migration
formatter floors of 1024 blocks and the larger of the computed recovery
requirement and its 256-block formatter floor.
For this geometry/profile, H=723, S=32,256, Pmax=725, the reserved arena is
30,198,688 bytes and recovery requires 2553 blocks. The bounded simulator log
holds H+V+1 = 852 replacement/slot block records. Initial source formatting writes
directly to the durable fixture; subsequent operations use the failure adapter.

The requested history is 4096 separately completed 256 KiB sequential calls
(1 GiB), 2000 separately completed 4 KiB appends, 32 partial overwrite/rename
pairs and eight retained unlink/name-reuse rounds. No durability batching spans
completed calls. File-backed expected bytes are updated only for confirmed
progress. Cold reopen compares all source/output contents, object identities and
directory counts, then runs both-state and cross-state checking. The retained
payload campaign above provides the separate older-state content oracle.

If this history exceeds the profile, the runner records the first refusal and
confirmed prefix, independently verifies that prefix after qualified durable
reopen when the writer remains healthy, and reports a verified healthy capacity
refusal separately from a correctness failure. Correctness assertions pass only
if confirmed contents and both retained states still verify; the command still
exits nonzero because the requested history is incomplete. Completed history with
successful verification exits zero. Refusal during admitted maintenance remains
a correctness failure. The runner does not increase limits or shrink/retry requests.
Phase reports separate user, orphan and maintenance metadata/data writes, flushes,
publications, write/flush callback time and useful-byte throughput. Writes/flushes
follow the active publication batch, not the presence of carried debt; standalone
fences have no active user/orphan batch. Healthy pending retirement is accepted
without treating it as failure or a verified capacity refusal. Debt summaries
report the maximum selected volume debt sampled at completed-call boundaries,
selected volume/pool debt at phase end, and the existing map/extent counts. These
sampled maxima do not claim to cover private intermediate batches. Read-callback
time is reported separately: planning reads can precede batch initialization or
follow a completed orphan batch, so its cleanup flag cannot classify those reads.
These are simulator costs,
not physical NVMe write amplification or flush latency. Whole-map rebuilding
remains the first correctness implementation, not the desired allocation strategy.

### Task 7 step 1 observations

The 107 quick groups pass natively (GCC 16.2.1) and with ASan/UBSan in the
existing GCC 14.2.0 builder container. All six extended groups pass in both
builds with seed 1; separate native editor seeds 1/2 and failure-history seeds
1/17 also pass. Seed 1's mixed namespace history used 3600 operations, reaching
five new/seven retired nodes; depth-eight repair reached seven new/fifteen retired
nodes. The integrated native extended run took 117.68 seconds and 6820 KiB peak
RSS while other validation ran. These are correctness-run observations, not an
isolated performance comparison. Ordinary host tools and the Pyxis GCC 16.2.0
freestanding archive build; the parent read-only kernel also builds.

The initial populated recovery run exposed a capacity shortfall. With seed 1 and the exact
census above, sequential call 1258 returns `PFS_LIMIT`, zero confirmed bytes for
that call and a healthy writer. The preceding 1257 calls confirmed 329,515,008
bytes (314.25 MiB). The selected state has 8187 extents against E=8192, 300 volume
metadata blocks, 7572 allocation records, 8675 claims and generation 3778. The
new sequential file accounts for 7397 extents, versus 1257 in the contiguous
model. `PFS_LIMIT` does not identify the refused candidate bound; the observed
state strongly suggests extent pressure but does not prove E was the sole cause.
The 2000-appends and churn phases are not reached. No profile increase,
smaller-prefix retry or allocator change was made in step 1. Durable cold reopening
independently matched the entire confirmed output and all 12,460,032 imported
source bytes, identities and directory counts; both retained states and the
cross-state checker completed successfully. The command then exited 1 as required,
after 431.30 seconds total with 11,908 KiB peak RSS. Preservation of confirmed
progress does not turn the refused workload into a pass.

Through that sequential prefix, user publications wrote 512,024,576 metadata
bytes and maintenance wrote 971,857,920: respectively 1.553873 and 2.949359 bytes
per useful byte (4.503232 combined). There were 1257 user and 2514 maintenance
publications, each with two flushes. The run's 290.658 seconds of mutation calls
reported 1.081 MiB/s, 231.048 ms mean and 426.363 ms maximum call latency. Builds,
other tests and sanitizer work overlapped this run; these single-sample timings
are not a stable baseline or a hardware throughput claim. Byte/record counts
establish the observed history cost independently of those scheduling effects.

The charged owner peaked at 31,551,024 bytes before reopen and 34,684,976 during
reopen, below the 128 MiB cap. Physical fixture storage at refusal was 348,237,824
bytes, with 341,975,040 bytes for independent expectations; this does not reserve
space for any future operation. The calculated arena bound did not promise that
this history would fit E. This failure prompted the focused allocation investigation
and unchanged-profile rerun below.

### Contiguous volume selection follow-up

Debugging the unchanged recovery workload at the rejected generation (3779)
identified the exact failed bound: the candidate had E=8194 against 8192.
The other first-envelope counts fit: M=300/4096, allocation records=7576/32256,
claims=8682/13013, map nodes=185/714 and live pool blocks=188/725. Admission
returned `PFS_LIMIT` before replacement writes. The 64 new data blocks were
split into seven same-birth extents of lengths 57, 1, 1, 1, 1, 1 and 2.
Independent intersection of both retained maps, subtracting both claim sets,
found 963,749 eligible blocks, including a 963,557-block contiguous run. The
old reservation exactly matched the lowest 128 eligible blocks: small reclaimed
holes fragmented data even though a sufficient run existed. This is debugger
and independent interval-analysis evidence, separate from the timings below.

Volume preparation now prefers the first eligible contiguous 128-block run,
joining adjacent eligible map intervals and excluding either retained state's
live claims. If none exists it uses the original fragmented selection. Pool
metadata keeps its existing lowest-eligible-first selection policy; physical
locations can consequently differ when the volume candidate uses other blocks.
Reservation size, admission bounds, publication and maintenance are unchanged.
A run does not permit reuse of protected storage. Different birth generations
still prevent extent coalescing, including separately completed small appends.

Four additional quick groups bring `make check` to 111 groups. A real
publication/reclamation history checks selection past reclaimed holes, file
contents, eligibility and the cross-state checker. Small private-selection
fixtures check FREE/RETIRED boundary joining, interruptions by either retained
map or claim list, and fragmented fallback when aggregate free space exists but
no complete run does. Expected eligibility is independently enumerated; assertions
use adjacency and protection rather than fixed block numbers. These synthetic
summary overlays exercise prepare/abort only and are not evidence that such an
overlay constitutes an admissible on-disk filesystem. Existing publication and
extended campaigns continue checking older payloads through maintenance writes.

The comparison uses baseline `23be37b` (tree-identical to merged `14b24ca`) and
implementation/tests `680cb63`. The recovery runner, source census, seed, profile,
reservation and caps are byte-for-byte unchanged. Both native builds use GCC
16.2.1, `-O2 -g3`, on the same Linux 6.19.10 KVM development guest (16 vCPUs,
32 GiB RAM, reported i9-12900K). Each measured process is pinned to CPU 2, runs
serially with builds/other validation stopped, and uses the same Btrfs-backed
`TMPDIR` and existing census. Caches are not dropped. This is a host-simulator
observation, not an 8 GiB QEMU qualification or NVMe/guest filesystem benchmark.

```sh
TMPDIR=/path/to/test-storage /usr/bin/time -f 'wall_seconds=%e max_rss_kib=%M' \
  taskset -c 2 build/pyxis-fs-tests --suite workload --profile recovery \
  --seed 1 --source /path/to/census
```

Equal-progress time is the runner's cumulative sequential-call time at 1152
completed calls (288 MiB), a checkpoint reached by both implementations. Whole
command time includes formatting and independent cold verification and covers
different amounts of work when a version refuses early. Metadata amplification
is callback metadata bytes divided by confirmed useful bytes in the stated
phase; comparing different completed histories is not an equal-work speed ratio.

One complete command per revision establishes the completion outcomes below.
For timing variation, a second unchanged process per revision repeats the common
prefix. The 1152-call timing line is recorded before any debugger attachment;
these diagnostic processes are later terminated and do not supply additional
full-workload passes. No time after attachment is used. Baseline counters are
sampled from its repeat before call 1258; changed counters come from a separate
GDB run stopped at that same point (1257 calls, 329,515,008 useful bytes). The
counter-only run uses an `offset == 329515008` entry breakpoint in
`recovery_workload.c:write_expected` and file-qualified `stats`/`pool` symbols,
avoiding the smaller file workload's identically named statics. Read-only
snapshots supply equal-progress metadata and extent/map counts, then the process
is terminated. Neither a shortened history nor resource refusal substitutes for
the full changed command's successful completion.

| Equal-progress measurement | Baseline | Contiguous selection |
| --- | ---: | ---: |
| 288 MiB cumulative mutation seconds, samples 1 / 2 | 256.390 / 255.264 | 184.981 / 184.554 |
| Two-sample mean seconds | 255.827 | 184.768 |
| Sample spread / mean | 0.440% | 0.231% |
| 314.25 MiB: sequential-file extents (excluding 790 imported) | 7397 | 1257 |
| 314.25 MiB: allocation-map records | 7572 | 2812 |
| 314.25 MiB: user metadata bytes | 512,024,576 | 214,421,504 |
| 314.25 MiB: maintenance metadata bytes | 971,857,920 | 382,631,936 |
| 314.25 MiB: total metadata bytes/useful byte | 4.503232 | 1.811916 |

At that checkpoint the mean is about 27.8% lower with contiguous selection.
Two samples show local repeatability, not a confidence interval or a general
hardware speedup; they include the simulator and host callback costs. At the
same 314.25 MiB prefix, metadata bytes decrease by about 59.8%. Publication and
flush counts remain identical: 1257 user plus 2514 maintenance publications,
each with two flushes. The improvement comes from placement and fewer map/extent
records, without changing durability granularity.

The full changed run passes every requested mutation and durable verification:
all 1 GiB of sequential output, 8,192,000 appended bytes, all 790 imported files
(12,460,032 bytes), object identities and directory counts match independent
expectations. Both retained states and the cross-state checker complete. Final
generation is 18751, with E=6947/8192, M=360/4096, 13,762 allocation records and
7639 claims. Charged memory peaks at 34,439,216 bytes, below the unchanged 128 MiB
cap. The baseline complete command exits 1 at its original refusal; the changed
complete command exits 0. Full command wall times are respectively 429.65 and
1608.77 seconds, covering substantially different completed histories.

| Changed full-run phase | Confirmed useful bytes | Total extents after phase | Metadata bytes/useful byte, including maintenance | Mutation-call seconds |
| --- | ---: | ---: | ---: | ---: |
| Sequential, 4096 calls | 1,073,741,824 | 4886 (790 imported + 4096 new) | 5.355392 | 906.313 |
| Independent appends, 2000 calls | 8,192,000 | 6886 | 826.107500 | 345.863 |
| 32 partial overwrites and 32 renames | 262,176 | 6947 | 992.988161 | 13.467 |
| Eight retained unlink/name-reuse rounds | 114,696 | 6947 | 3136.352654 | 18.057 |

These phase denominators count confirmed written bytes, including overwrites;
rename, unlink and cleanup metadata remain in their associated phase. The larger
sequential history can have a higher average metadata ratio than the baseline's
shorter prefix despite better placement at equal progress. The small-append phase
retains 2000 distinct birth generations, and each synchronous call still rebuilds
the whole allocation map and drains maintenance. Those measured costs remain a
follow-up limitation, not a finalized allocation strategy or acceptable device
performance target.

All 111 quick groups and six seed-1 extended groups pass natively and with
ASan/UBSan in the existing GCC 14.2.0 builder container. A focused old-writer
regression run fails the new reclaimed-hole and protected-run adjacency checks;
the new writer passes all four groups. Ordinary host tools and the Pyxis GCC
16.2.0 freestanding archive build. The full populated workload above is native,
not an additional full-history sanitizer run. This closes the observed placement
failure for this exact 4 GiB history; it does not establish arbitrary fragmentation,
all histories at E/M, real-host post-error recovery, native writable integration,
8 GiB guest memory qualification or the outstanding 64/256 GiB profiles. Task 7
remains incomplete.

## Bounded retirement carryover coverage

The maintained quick suite now exercises consecutive ordinary and orphan batches
with healthy pending retirement, rather than inserting implicit fences between
operations. It verifies derived record/workspace/generation bounds, refusal
before admission, allocation-free private publications and complete funded orphan
cleanup/terminal fences. Ordinary public authorization and reads can allocate
before admission; the no-additional-allocation promise applies to admitted work,
not every reader call.

The cross-volume fixture discovers a shared catalog leaf pair and another leaf
from its encoded image, then exercises three volumes without restricting allocator
placement. An independent per-generation payload ledger checks every retained
file before slot writes, after replacement writes, in both rolling and startup
fences. It also imports a fully checked nonadjacent pair protecting two owners;
opening must use its broader profile and establish an empty volume-debt seed.
A durable allocation-tree walk verifies old cohorts are free at the freeing
publication boundary; later publications may legitimately reuse those ranges.

Checkpoint-only held rights settle pool-wide debt without read/lookup widening or
orphan deletion. Tests cover no-publication idempotence, complete final orphan
release with pending retirement, I/O-free volume/pool disposal, independently
confirmed prefixes, and actual planning/replacement/slot failures with sticky
provenance. Healthy-trace cuts identify initial startup fences, object cleanup and
trailing fences by independently observed logical progress. Publication counts,
physical numbers, fixture shape and benchmark configuration are not acceptance
assertions. The deliberate two-flush protocol and proved resource bounds remain.

See [matched RAM measurements](retirement-carryover-measurements.md) for the
unchanged comparison through all trailing maintenance. This bounded suite does
not close task 7, qualify real-host post-error recovery or enable the Caelum
writer; the kernel-stack integration prerequisite remains deferred.


## Topology-preserving allocation-map correction

Nine new quick groups bring the suite to 134. Synthetic source trees define
independent interval histories and decode the sealed mixed shared/new tree against
an independently generated per-block oracle. Cases cover remote subtree sharing,
fixed input prefix excluding same-publication frees, both canonical seam endpoints,
overflow/underflow redistribution across parents, successor-only expansion and
exhausted/global funded bulk construction. Emitted inventory must be reachable;
no unused pool allocation is permitted. Configured fixture identities establish
inputs, not required allocator placement in ordinary filesystems.

Source-summary mismatch, noncanonical checksum-valid bytes and an adapter read
failure remain errors. The read cut is discovered from a healthy trace. Existing
publisher tests preserve retained payload checks during replacement writes,
cross-volume/cohort histories, ordinary refusals, funded cleanup with further
allocation denied, checkpoint/final-release and interrupted startup semantics.
The [implementation](incremental-map.md) and
[matched RAM observations](incremental-map-measurements.md) distinguish proven
bounds, measurements and remaining deployment qualification.

The focused planning follow-up adds shuffled physical source IDs, retirement
membership against independently expected changed ancestry, absent IDs and shared
topology preservation. The source-read failure case now interrupts after partial
traversal and establishes that incomplete loading exposes no retirement result;
only the standalone planner is reused, without making a stopped writer healthy.
These cases bring the quick suite to 135 groups. The unchanged comparison checks
each phase's diagnostic attribution against actual slot writes, the deliberate
two-flush protocol and the adapter's monotonic flush counts. It retains the
independent application ledger and read-only content/namespace verification.
The [matched planning report](map-planning-measurements.md) records unchanged
submitted bytes, phase-specific fallback costs, instrumented timing and storage
evidence. Larger-map and writable-deployment qualification remain open.
