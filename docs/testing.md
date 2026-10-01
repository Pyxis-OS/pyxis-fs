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
| Writable admission and views | Both-state canonical validation, quota/deletion/profile/reserve/memory boundaries; ordinary live reads and checkpoint authority without read/lookup; callback reentry refusal; orphan-index opening refusal before writes |
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

Unexpected resource failure during a funded drain is an invariant failure,
not successful safe refusal. Resource cases deny further allocation after admission
and require the drain to finish. Ordinary refusal before admission leaves the
writer usable. Final orphan release and its resource cases remain with task 6;
nonempty orphan indexes are currently refused before any write.

File and namespace mutation cases follow with tasks 5/6. Larger pressure,
populated-source/write-history and extended failure campaigns receive explicit
commands and coverage limits when delivered; there is no empty extended-suite
command today. Task 7 records combined results rather than first adding tests.
Host tests remain maintained after milestone closure. Ordinary native builds,
freestanding target compilation and later guest validation remain separate checks.

Optional local memory/undefined-behavior instrumentation uses a separate build
and requires the host compiler's sanitizer runtimes:

```sh
make -j16 BUILD=/tmp/pyxis-fs-sanitize \
  CFLAGS='-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' check
```
