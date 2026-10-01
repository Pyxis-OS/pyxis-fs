# Maintained host contract tests

Run the deterministic PR suite from this repository:

```sh
make -j16
make check
```

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
| Publication and recovery | Real private COW overwrite through user/advance/free publications; each replacement/slot write and all six flush boundaries; durable recovery, sticky health and cache-visible/non-pending-slot counterexample; both retained payloads compared before every maintenance slot write |
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

The minimum-resource fixture uses 16,384 blocks, two objects and one data mapping.
The E=1/M=3 profile computes H=27, recovery=465, Pmax=29 and an arena of
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

The `Filesystem` workflow runs the ordinary build and `make check` on each PR and
push to main, using the existing Pyxis builder image. The `host-contract` job has a
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
The suite then checks later reuse. A no-op checkpoint is tested for authority and
health only; it supplies no publication evidence.

The bounded matrix covers each replacement write and slot write in that fixture,
all six publication flushes with none/all pending writes durable, actual slot
tears, and cleanup failure after confirmed user progress. Cold recovery uses a
clone of the simulator's explicitly durable image. The cache-only slot case
shows cached structural validation plus a successful later flush disagreeing
with durable storage, while the failed core remains stopped. It does not assert
that a fresh core can detect adapter history absent from its inputs.

Focused read-error cases discover every backing-read boundary during the small
fixture's user publication planning and both maintenance publications. A transient
read failure must stop all ordinary access, preserve confirmed progress and remain
sticky until close. Fresh validated recovery uses the explicitly durable clone.
Existing write/flush cases separately require readable pre-slot failure and
access-stopping publication uncertainty.

Allocation deltas use in-place heapsort within reserved storage. Tests assert
ordering and preservation of complete delta contents for empty, single, ordered,
reversed, mixed and equal-key inputs. Replacing insertion sort removes its
calculated quadratic worst-case sorting cost; no sorting benchmark or measured
throughput improvement is claimed.

Unexpected resource failure during a funded drain is an invariant failure,
not successful safe refusal. Resource cases deny further allocation after admission
and require the drain to finish. Ordinary refusal before admission leaves the
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

The shared failure adapter checks replacement publication at all six flush
boundaries, including uncertainty after a failed flush has made pending writes
durable. Expected names, source/victim identities and different payloads are
supplied independently. Planning and maintenance read failures require
`ACCESS_STOPPED`, with confirmed namespace progress retained. Focused final-close
and startup cases interrupt cleanup after a confirmed data-removal batch, check
remaining paired object/orphan records and grants, then require a qualified cold
reopen of the explicitly durable image to finish. The consumed close reference
and failed-open resource release are checked separately from cleanup completion.
Existing overwrite/reuse tests still compare both retained payloads during each
maintenance publication; structural agreement alone is not the content oracle.

The [healthy host command observations](host-tools.md#namespace-command-validation)
cover explicit replacement, long names, refusal/no-op behavior and extraction.
Real-host post-error or interrupted-session recovery is still unqualified. The
larger combined pressure and failure campaigns remain task 7; no native guest
writer or kernel-stack suitability is established here.
