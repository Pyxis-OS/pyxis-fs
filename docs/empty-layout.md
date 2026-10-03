# Bulk-image construction

**Historical: retired COW filesystem.** The implementation, host tools and test
suite described here were removed when Caelum adopted the native format. Commands
and APIs below are obsolete; they are not current validation or interfaces. The
[source snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9)
preserves the original implementation. Use the [native format](native-format.md)
and [native host tools](native-host-tools.md) for current behavior.

The bulk builder constructs one through 256 volumes using the shared codecs.
The host supplies nonzero pool, volume and object IDs generated with strong
randomness, valid names and explicit owner principals. Every volume has at least
its root directory. Planning copies all object metadata and charges bookkeeping,
sorting arrays, node layouts and construction buffers to `pfs_memory`; it performs
no I/O. File bytes remain with the host until construction.

## Allocation-map termination bound

Blocks 0 and B-1 hold superblocks. Starting at block 1, all pool-owned metadata
forms one contiguous prefix: the pool root, both catalog trees and the allocation
map tree. Each volume then owns one contiguous range containing its object and
grant trees, nonempty-directory trees, and file data. All remaining allocatable
blocks form one free suffix. Every metadata birth is 1 and every live allocation
has permanent charge.

For N volumes the map therefore has exactly N+2 records: one pool prefix, N
volume ranges and one free suffix. Including the map's own blocks changes only
the pool prefix's length, never the number or encoded lengths of records. A map
leaf holds 46 fixed 80-byte records (including four-byte slots and aligned
packing); an internal node holds 65 fixed 56-byte records. For N <= 256 there are
at most six leaves and one internal root, hence at most seven map blocks and two
levels. One leaf is used when all records fit; a single-child root is never built.
Map sizing is one finite calculation, with no fixed-point iteration.

Catalog and object leaves are greedily packed using exact shared codec lengths.
Their internal levels group at least two children, repairing a final singleton
by moving one child from its predecessor. Directory levels use greedy byte
packing followed by adjacent merge/repartition of a final group with fewer than
six records or children. Each nonfinal greedy namespace group has at least
twelve items. The ordered combined sequence therefore fits one node or can be
partitioned into two byte-fitting nodes of at least six items. Exact minima are
recomputed before packing each next level. A sole root leaf may hold one through
five records, and a one-child internal root collapses. Other indexes keep their
sparse shape contract. The planner checks the eight-node depth limit and exact
repaired node counts before assigning blocks, data ranges and capacity promises.

For M objects, object-tree leaves require at most M nodes, and directory leaves
at most M-N nodes: every non-root object has exactly one naming entry. A tree
with L leaves and internal fanout at least two has at most 2L-1 nodes. The plan
therefore reserves at most 4M+N+192 node descriptors, including one grant leaf per
volume and fewer than 64 nodes for each of the three pool trees. The descriptor
reservation is a conservative memory bound, not occupied image space. Only
actually packed nodes consume blocks. Checked sizes and the memory cap precede
allocation; there is no recursive construction or unbounded C stack.

## Objects, data and accounting

The supplied root is object index zero, with no parent and an empty name. Each
other object's parent precedes it and names a directory. This establishes one
reachable parent chain without cycles. Directory depth is limited to 256 objects
including the root. Iterative heapsort establishes object-ID order and
parent/name order; duplicate IDs, persistent-ID collisions and duplicate sibling
names are rejected. Total objects are limited to the committed-state profile;
directory entries and initial file extents are bounded by that count.

All objects have the volume's selected owner. Each volume receives exactly one
explicit owner subtree grant on its root, carrying file bits 0..4, directory
bits 0..5 and administration bits 0..2. The added directory checkpoint bit 6
does not widen this grant. Feature masks remain zero, with no orphan index. Empty
files and directories have no storage root. Nonempty directories have their own
name-keyed trees with volume and directory ownership in every node. Every
nonempty file receives one contiguous inline extent; the final block's padding
is zero. Source holes are materialized by the host's ordinary reads.

Each volume's permanent allocation includes all its tree nodes and data blocks.
Reserves follow the proportional defaults and explicit-override floors. After
reserving budgets, existing allocations and explicit unused guarantees, half the
remaining space is distributed among volumes without explicit guarantees, with
remainder assigned in name order. Quotas cover both actual allocations and
guarantees. Capacity and quota failures are reported during planning, before
output creation.
Six-entry directory packing supplies one private editor precondition; construction
does not perform writable admission, validate the permanent deletion envelope or
change E/M, reserve, quota or grant defaults. Existing sparse namespace trees
remain readable and checkable; the formatter does not normalize existing images.

## Publication

Construction uses the allocated workspace to emit every metadata tree and stream
file data in transfers of at most 64 KiB, respecting the output adapter's smaller
transfer limit. The source callbacks identify files by original volume/object
index; planning's sort order does not change these selectors. Each requested
source read is exact. After copying, the source adapter revalidates all sources.
A callback failure stops construction without publishing either superblock.

The builder then writes the pool root, flushes metadata and data, publishes both
generation-1 slots with the same state, and flushes again. The host owns exclusive
output creation, source traversal and change detection, sparse output sizing,
partial-output reporting and parent-directory durability. Construction performs
no output reads, further allocation, recovery or incremental update. Empty
volumes use this same builder without file reads.

The private whole-map candidate planner uses a different bounded shape for
fragmented COW layouts. It reserves exact replacement IDs and accounts for them
before encoding map nodes; its reusable-range proof obligations and fixed arena
formulas are described in [private candidate planning](core.md#private-candidate-planning).
It performs no publication and does not change this generation-1 bulk layout.
