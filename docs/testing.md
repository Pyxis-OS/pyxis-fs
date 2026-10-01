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

Task 2 supplies the following test groups. The runner prints each group and the
executed total; several groups contain boundary tables or repeated edit histories.

| Area | Contract assertions |
| --- | --- |
| Read-only baseline | Known CRC32C result; independent payload, name and kind expectations; held rights and denial; pool/volume/view lifetimes; failed-open allocation cleanup |
| Encoding and corruption | Independently assembled orphan/volume vectors; feature scope; checkpoint and unknown rights; reserved bytes, padding and compatible extensions; canonical block/tree encoding |
| Orphan recognition | Named-or-orphan exclusivity, missing/root/parent conflicts, empty orphan directories, retained file data/grants, diagnostic reads and refusal of fresh ordinary acquisition |
| Formatter | Root exceptions, short leaf/internal tails and mixed/minimum/maximum names; occupancy, exact minima, payloads, grants and metadata/allocation accounting across 30 populated fixtures |
| Private tree edits | Independently maintained expected entries through insert/update/delete histories; exact minima, ordering, byte fit, namespace occupancy, split/merge/redistribution/root collapse; maximum-depth sparse deletion, separator growth during namespace deletion, unpublished buffer reuse and atomic refusal |
| Private map planning | Independent interval/ownership expectations, canonical coalescing, unchanged inputs and refusal outputs; fragmented interior allocations accounting for every replacement map/catalog/root block, exact finite bounds and reachable nonempty nodes |
| Reserved memory | Accepted profile arithmetic, capped arena reservation, allocation failure, release and no further allocation/I/O during private map planning |

On 2026-10-01 the 30 task-2 groups passed with native GCC 16.2.1 in 2.22 seconds
(`make check`, including incremental runner rebuild work). They also passed with
ASan/UBSan under GCC 14.2.0 in the existing builder container. Pyxis GCC 16.2.0
compiled the freestanding archive, and the parent kernel and host tools built.
A fresh 64 MiB source import passed both-state checking and byte-for-byte
extraction comparison. These are bounded host observations; no QEMU/guest or
large-workload validation is claimed.

Fixture storage and allocator controls live in `tests/support.c`, behind the
existing platform callbacks. Temporary sparse files satisfy the format geometry
floor without allocating a large empty-image payload. Allocation callbacks track
ownership and allow deterministic failure; teardown reports leaked core resources.
Test-controlled buffer editing is confined to the test sources. There is no
production test hook, crash simulator or claim of physical preallocation here.

## CI and scope limits

The `Filesystem` workflow runs the ordinary build and `make check` on each PR and
push to main, using the existing Pyxis builder image. The `host-contract` job has a
five-minute timeout; the quick suite is intended to stay comfortably below one
minute on the normal runner. The emitted status is `Filesystem / host-contract (pull_request)`; the repository
owner must add that exact pattern to required status checks in branch protection. This repository owns the check; the parent
Pyxis build does not substitute for it.

Passing establishes the exercised core contracts for these bounded inputs. It
does not establish a usable writer, full admission or funded-drain correctness,
power-loss/recovery safety, host durability qualification, whole-filesystem scale,
performance targets or guest behavior. Private planners trust documented caller
proofs for canonical source closure, ownership, cross-state reusable ranges and
ended pins; their tests do not manufacture those proofs. Calculated envelopes
are distinct from measured memory and performance.

Tasks 3/4 add the bounded failure adapter to this same runner and land publication,
recovery, admission and funded-cleanup tests with those behaviors. That adapter
will separately model visible bytes, flush-pending writes and durable bytes; it
will exercise the cache-visible/not-flush-pending counterexample, sticky stopped
instances and recovery from an explicitly durable image. Older retained payloads
must be compared against independent expected contents through the maintenance
replacement/pre-slot cuts. These tests are not implemented or claimed by task 2.

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
