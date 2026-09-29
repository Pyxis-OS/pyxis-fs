# Empty-image construction

The empty builder constructs one through 256 volumes using the shared codecs.
The host supplies nonzero pool, volume and root-object IDs generated with strong
randomness, valid names and explicit owner principals. Planning copies the input
and charges its bookkeeping and construction buffers to `pfs_memory`; it performs
no I/O. No source import or later mutation is implemented.

## Allocation-map termination bound

Blocks 0 and B-1 hold superblocks. Starting at block 1, all pool-owned metadata
forms one contiguous prefix: the pool root, both catalog trees and the allocation
map tree. Each volume then owns exactly two contiguous blocks, its root-object
leaf and owner-grant leaf. All remaining allocatable blocks form one free suffix.
Every metadata birth is 1 and every live allocation has permanent charge.

For N volumes the map therefore has exactly N+2 records: one pool prefix, N
distinct volume ranges and one free suffix. Including the map's own blocks only
changes the pool prefix's length, never the number or encoded lengths of records.
A map leaf holds 46 fixed 80-byte records (including four-byte slots and aligned
packing); an internal node holds 65 fixed 56-byte records. For N <= 256 there are
at most six leaves and one internal root, hence at most seven map blocks and two
levels. One leaf is used when all records fit; a single-child root is never built.
Map sizing is thus one finite calculation, with no fixed-point iteration.

Catalog leaves and internal nodes are greedily packed using their exact shared
codec lengths. Internal levels group at least two children; a trailing singleton
is moved into the preceding group or paired by moving its preceding sibling.
Each level reduces node count, establishing termination. At worst volume-ID
leaves hold eight records, name leaves hold thirteen maximum-length records,
ID internal nodes hold 57 children and name internal nodes hold twelve children.
Each catalog uses fewer than 64 nodes and fewer than eight levels. The planner
checks these bounds and allocatable geometry before assigning physical blocks.

## Promises and publication

Each volume's permanent allocation is two blocks. Reserves follow the format's
proportional defaults and explicit-override floors. After reserving budgets and
explicit unused guarantees, half the remaining space is distributed among
volumes without explicit guarantees, with remainder assigned in name order.
Quotas and the complete capacity inequality are checked before output creation.

Construction uses the already allocated workspace to emit catalog, allocation,
object and grant trees, then the pool root. It flushes before publishing either
superblock, writes both generation-1 slots with the same state, and flushes again.
The host owns exclusive output creation, sparse extent sizing, partial-output
reporting and parent-directory durability. The builder performs no recovery,
source import or incremental update.
