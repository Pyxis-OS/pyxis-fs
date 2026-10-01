# Initial Pyxis filesystem format and tool contract

Status: implemented initial format, read-only tools and bounded host mutation;
the [shared core](core.md), bounded bulk construction and source import,
candidate selection, readonly traversal/acquisition, extraction and explicit GPT
inspection and whole-image consistency checking are implemented.
The volume ORPHANS representation, directory checkpoint right, canonical private
COW editors and bounded allocation-map planners are implemented. Explicit
[writer admission/publication](core.md#admitted-writer-and-publication) adds the
ordered publisher and synchronous reclamation. Public file creation, writing and
resizing, directory creation, removal, regular-file rename/replacement and orphan
cleanup are implemented. Native writable integration remains a future task.
[Host commands](host-tools.md) cover `info`, `volumes`, `list`, `stat`, `access`,
`extract` and `check`, plus healthy-session `pyxisfs-write open`, `checkpoint`,
`create-file`, `create-directory`, `remove`, `rename`, `write` and `resize`.
The [integration contract](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/devices/filesystem-readonly.md)
and [persistent-storage decisions](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/wip/persistent-storage.md)
remain authoritative. The owner has agreed the standalone-image creation and
read-only GPT inspection boundary. Original project material is covered by the
repository's [MPL-2.0 licensing notice](../LICENSING.md).

The initial format uses an inline extent descriptor, proportional reserve
policies and the ancestor lookup chain for acquisition by object ID. The
publication/reclamation envelope constrains the bounded writer. Its implemented
scope and validation limits are documented separately from the format; neither
implies qualified real-host post-error recovery or FUSE support.

## Ownership and integration

`PyxisOS/pyxis-fs` owns the format specification, shared freestanding
GNU C23 library, formatter, inspector and Linux adapter. This repository owns the
format contract; Pyxis OS links to it rather than maintaining another copy.
The core document records implemented interfaces, validation limits and ownership
contracts. Host usage and current command coverage are in the host-tools document.

Agreed integration is a published revision pinned at `fs/`, with the relative
submodule URL `../pyxis-fs.git`. An opt-in parent `make fs-tools` invokes the host
build with explicit source/output directories and `HOST_CC`. It produces
`build/fs-tools/libpyxis-fs.a`, `build/fs-tools/mkpyxisfs` and
`build/fs-tools/pyxisfs-inspect` and `build/fs-tools/pyxisfs-write`. A standalone build
in pyxis-fs produces the same tools and a freestanding core archive. Host adapters
use host libc; the core uses neither libc services nor Pyxis kernel/ABI headers.
The Pyxis kernel compiles a pinned read-only core subset for native mounts.
Private planners, the publisher, construction, checking, host adapters and Unity stay out of
that subset; this task adds no guest writable interface.
The maintained [host contract suite](testing.md) has a filesystem CI gate.
Normal builds use the existing compiler; no compiler-container build, FUSE
dependency or installation target is introduced.

## Supported profile and units

Disk integers are unsigned little-endian values of the stated width. Addresses
are pool-relative 4096-byte block numbers, never host offsets or C pointers.
The adapter supplies the selected extent's byte offset and length; the core sees
only its block count. Logical sectors are 512 or 4096 bytes. The extent start
must be sector-aligned, not necessarily aligned to a disk-wide 4096-byte boundary.
Ignore a trailing partial filesystem block. Do not read or modify outside the
selected extent.

For `B = floor(extent_bytes / 4096)`, superblock slots occupy blocks `0` and
`B-1`. Allocatable blocks are `[1, B-1)`, with count `U = B-2`. Both slots use the
same layout. The initial formatter writes generation 1 to both, with identical
committed state and different physical-location/checksum fields.

Initial implementation limits, distinct from integer encoding widths:

| Resource | Limit |
| --- | --- |
| Usable pool extent, including superblock slots | 64 MiB through 1 TiB |
| Volumes | 256 |
| Name | 255 UTF-8 bytes |
| Objects, directory entries, file extents, grant records | 1,048,576 of each per committed state, across all volumes |
| Allocation-map records | 4,194,304 per state |
| Tree depth | 8 nodes from root through leaf |
| Directory depth | 256 objects including the volume root |
| File logical length | 1 TiB; holes do not imply physical allocation |
| Explicit grants on one object | 64 |
| One core block-I/O call | At most 16 contiguous blocks, or 64 KiB |
| Tool working-memory budget | 128 MiB by default; explicit override through 1 GiB |

Limits apply independently to both retained states; they cannot be averaged.
Readers return `LIMIT` for an otherwise interpretable image beyond this profile,
not `CORRUPT`. A memory budget is a cap, not a guarantee that every supported-size
image fits it. Formatting fails before creating output if planning cannot fit.
Inspection reports an incomplete check on resource exhaustion. No disk size or
record count is permission to allocate an equally large host buffer.

All additions, products, alignment and range endpoints are checked before use.
Use subtraction-based containment after verifying the start is in range. Never
wrap a generation, convert a disk count to a narrower host size unchecked, or
interpret an overflowing range as empty. Generation zero is invalid; reaching
`UINT64_MAX` requires a later explicit migration rather than wrapping.

## IDs and names

Pool, volume, object and principal IDs each encode 16 opaque bytes, in that order;
they are distinct types despite identical widths. All-zero is invalid for an
actual identity. Zero in an explicitly nullable field means absent, not a special
principal. Host text uses exactly 32 hexadecimal digits without UUID byte-order
conversion; output is lowercase and input accepts either hex case. Generate new
IDs with OS strong randomness and fail if it is unavailable. Detect collisions
within the new pool and retry with a bound of 16 attempts. No time/PID fallback.
Object identity is `(pool_id, volume_id, object_id)`.

Names are valid shortest-form UTF-8, excluding surrogate code points and values
above U+10FFFF. Reject empty names, NUL, slash and exact `.` or `..`. Compare
unsigned encoded bytes lexicographically, with a prefix sorting before its
extension. No normalization, case folding or locale rules. Stored lengths exclude
terminators. Invalid UTF-8 is corruption in media and invalid input to tools.

## Checksums, metadata headers and references

Use CRC32C/Castagnoli: reflected polynomial `0x82f63b78`, initial accumulator
`0xffffffff`, final XOR `0xffffffff`, stored little-endian. Process bytes in
increasing offset order. This is accidental-corruption detection, not authenticity
or file-data integrity. It differs from GPT's IEEE CRC32.

Every metadata block is exactly 4096 bytes. Its CRC32C covers all 4096 bytes,
including reserved bytes and padding, with bytes 20..23 treated as zero.

| Block offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | Magic bytes `50 59 58 49 53 46 53 00` (`PYXISFS` plus NUL) |
| 8 | 2 | Block type: 1 superblock, 2 pool root, 3 tree node |
| 10 | 2 | Incompatible structure version; initially 1 |
| 12 | 2 | Header size; 128 in version 1 |
| 14 | 2 | Reserved |
| 16 | 4 | Used bytes, including header; 128..4096 |
| 20 | 4 | Whole-block CRC32C |
| 24 | 16 | Pool ID |
| 40 | 8 | This block's physical pool-relative number |
| 48 | 8 | Birth generation; nonzero, no greater than selected generation |
| 56 | 72 | Reserved |

The pool ID, location, type, version and birth generation must match the reference
and traversal context. A copied checksummed block at another address is invalid.
Unused tail bytes are initialized to zero. Readers validate checksums over them
but do not mistake nonzero reserved extension bytes for a new incompatible
version. Future interpretation of reserved bytes follows feature declarations.

A metadata reference is 24 bytes: block number at +0 (`u64`), birth generation at
+8 (`u64`), block type at +16 (`u16`), structure version at +18 (`u16`), and four
reserved bytes at +20. All-zero is the only null encoding. A non-null reference
must target an allocatable block, have nonzero birth generation and match the
referenced header exactly. A required root cannot be null. Reusing a physical
address gives it a new birth generation, preventing an old reference from naming
the replacement accidentally. Repeated references must describe the same block.
Every non-null reference also obeys
`child_birth <= referring_block_birth <= selected_generation`, including references
inside packed records. An immutable older block cannot name a child born later.

Packed leaf and internal records have their own header, with offsets relative
to the record: type `u16` at 0, incompatible version `u16` at 2, total length
`u32` at 4, CRC32C `u32` at 8 and four reserved bytes at 12. Length is a multiple
of eight and includes padding. The record checksum covers its full length with
bytes 8..11 zeroed. Block and record checksums are both required. Disk records
are decoded field by field, not by casting a C structure.

All initial versions are 1. Structure versions change only for incompatible
encoding/meaning. Each pool and volume separately carries three `u64` feature
masks: required-to-read, required-to-write and optional-for-safe-read/write.
The formatter emits zero masks. Volume read-required bit 0 is `ORPHANS`,
recognized only in volume masks; pool masks recognize no enabled features.
Unknown read requirements prevent interpretation of that scope. Unknown write
requirements permit read-only access. Optional features may be ignored for reads. Future
writers must preserve unknown extension bytes and their semantics or refuse
mutation. Merely knowing a mask bit is optional does not prove that a particular
writer can preserve it. Unknown required record types in a supported tree are
`UNSUPPORTED`, never skipped as though absent.

Read compatibility does not imply complete-check compatibility. The checker
reports incomplete/unsupported for any unknown enabled pool or volume feature in
any of the three masks. It may validate known structures, but cannot call the image
clean or call unexplained allocation corrupt solely because the feature's semantics
are unknown. No new feature-mask category is needed. Defects independently proved
under known semantics remain corruption and are reported alongside incompleteness.

## Superblock and pool root

Superblock fields following the common header:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 128 | 4 | Filesystem block size; 4096 |
| 132 | 4 | Reserved |
| 136 | 8 | Pool block count `B`, matching the adapter extent |
| 144 | 8 | Pool required-to-read features |
| 152 | 8 | Pool required-to-write features |
| 160 | 8 | Pool optional features |
| 168 | 24 | Pool-root reference, type 2 |
| 192 | 3904 | Reserved |

The header birth generation is this slot's committed generation. Initial used
length is 192. Superblock identity and geometry are not inferred from one slot
to excuse disagreement in the other.

Pool-root fields:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 128 | 24 | Required volume-ID catalog root |
| 152 | 24 | Required volume-name catalog root |
| 176 | 24 | Required allocation-map root |
| 200 | 8 | Volume count |
| 208 | 8 | Live pool-metadata block count |
| 216 | 8 | Live volume block count |
| 224 | 8 | Retired block count |
| 232 | 8 | Free block count |
| 240, 248 | 8 each | Ordinary COW budget and its occupied portion |
| 256, 264 | 8 each | Migration budget and its occupied portion |
| 272, 280 | 8 each | Recovery budget and its occupied portion |
| 288 | 3808 | Reserved |

Initial used length is 288. At least one volume is required. Pool-root generation
equals its superblock generation; child nodes can have earlier birth generations.
The four allocation-state totals sum exactly to `U`. Budget occupancy is an
additional accounting dimension, not another allocation-state total.

## Candidate validation and selection

Validation has three distinct levels:

| Level | What it establishes |
| --- | --- |
| Candidate selection | Supported superblock/pool-root envelopes, three catalog/map root nodes and their local contexts, reference generations, checked arithmetic and internal consistency of recorded counts. No allocation-map or volume walk. |
| Ordinary access | Structure, reference context and allocation ownership for every range consulted by that operation. No claim about unvisited records or global counts. |
| Complete `check` | Both retained states' global reachability, ownership, feature interpretation and accounting reconciled with all allocation records. |

Opening reads both slots independently and applies the selection level before
classifying them. Check recorded pool counts sum to `U` without overflow, and
recorded occupied budgets do not exceed budgets; these local checks do not prove
the counts reflect the map. `info` reports them as recorded, not globally verified.
It does not silently run a complete accounting walk. Only successful `check`
reports globally verified accounting for its validated, unchanged states.

Before ordinary operations expose metadata or file bytes, every consulted range
must also match the selected allocation map: pool metadata is live-pool; volume
metadata and data are live-volume with the expected volume ID and birth
generation. Free, retired, uncovered or differently owned ranges are corruption.
Split ranges across allocation records as necessary and validate every segment.
Superblock slots are outside the map and follow their fixed-location rules.

Allocation-map lookups bootstrap from structurally checked map nodes (header,
checksum, bounds, key range and reference context). Deduplicate nodes by physical
identity and retain every incoming context constraint. Track three states:

- Structurally validated: bytes and traversal context are checked; ownership is
  not yet proved and the node's own allocation lookup has not completed.
- Proof-pending: its allocation lookup is queued or processed, with an explicit
  local-match flag and dependencies on every map node consulted by that lookup.
- Proof-completed: the node belongs to a closed set for which all structural and
  local allocation checks succeeded, with no unresolved dependency or I/O.

Seed the worklist with the operation's consulted metadata and file ranges.
For each range, traverse the selected map to obtain live state, owner and birth;
add every newly consulted map node and schedule its own allocation lookup once.
Work iteratively: a pending dependency is recorded, never recursively awaited or
treated as completed. When the queue is empty, complete the pending set together
only if every member's lookup finished with a matching live allocation and every
dependency is either inside that fully checked set or already completed. This
allows mutual allocation-proof dependencies without using pending status as proof.
An actual cycle in parent/child tree references remains corruption.

Publish no result or read prefix until its proof closure completes. Cache completed
proofs only within the same selected generation. Bound storage by the memory cap;
cap exhaustion returns `LIMIT`, never an unproved result. Bounded memory does not
promise a small I/O count: closure can reach much of the allocation map. The work
is bounded by the finite supported tree/record profile and deduplicated nodes, not
a fixed handful of reads. Only complete checking proves global absence of
overlapping or duplicate claims.

Classify each candidate as absent (no magic), corrupt (bad checksum, invalid
geometry/encoding/context), unsupported (unknown required format), limit, I/O
error, or valid. Allocation failure is a separate operational error. An I/O,
unsupported, limit or allocation failure on either candidate prevents selection,
even if the other candidate is valid. Do not call an unexamined candidate corrupt.

Two valid candidates must agree on pool ID and extent geometry, otherwise return
`AMBIGUOUS`. Different generations select the larger number. At the same
generation require byte-for-byte equality of the superblocks after normalizing
the physical-location and CRC fields; shared root references must also validate.
Do not compare only checksums or choose the slot at the front by preference.
One valid candidate paired with an absent/corrupt candidate is degraded read-only.
Neither valid means no open pool. Report both candidate classifications; for
multiple operational failures the primary result prioritizes I/O, no-memory,
limit, unsupported, then corruption/absence, without suppressing the other detail.
This is the core's primary selection diagnostic. Commands apply the per-command
degraded-peer rules below before aggregating failures; they do not map only the
primary diagnostic to an exit status. For a successful non-`check` operation on a
selectable degraded pool, the tolerated absent/corrupt peer is a warning, not a
failure included in exit aggregation. `check` includes that peer in its result.

Hold the selected root and the validated older root description for the lifetime
of the open pool. Corruption discovered later produces an error; never switch an
open handle to the other generation. Unsupported volume features do not block
listing its envelope or reading supported sibling volumes. Their ownership must
remain visible through the supported pool allocation map.

## Tree nodes

All indexes use the common type-3 block with this node header:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 128 | 2 | Index kind, listed below |
| 130 | 2 | Level: zero leaf, increasing toward root |
| 132 | 2 | Record count |
| 134 | 2 | Slot size; 4 |
| 136 | 16 | Owning volume ID, zero for pool indexes |
| 152 | 16 | Owning object ID for directory/extent indexes; otherwise zero |
| 168 | 24 | Reserved |
| 192 | 4 per record | Slots: record offset `u16`, record length `u16` |

Records follow the slot array, starting at its end rounded up to eight bytes.
Slots and records have the same key order. Records are tightly packed, aligned
to eight bytes, non-overlapping, and entirely inside used length; the last record
ends at used length. Remaining block bytes and alignment padding start at zero.
Each slot length must equal its record header's total length exactly.
There are no sibling links, overflow records, inline file bytes or compression.
Node count is also bounded by what fits in its 4096 bytes; zero-length slots,
duplicate keys are corrupt and unknown index kinds are unsupported. A structurally
consistent tree exceeding the eight-node depth profile returns `LIMIT`; an invalid
parent/child level relationship is `CORRUPT`, regardless of the depth limit.
Empty indexes use a null reference, except required pool catalog/map roots and
the volume object root.
Those roots are nonempty because each image has a volume, allocation map and
root-directory object. Empty non-null nodes are invalid.

Internal nodes have at least two children. An internal record has type 256:
key length `u16` at +16, six reserved bytes at +18, child reference at +24, and
the child's minimum key at +48 followed by zero alignment padding. The reference
must target a node of the same kind/owners and level one lower. Minimum keys are
strictly increasing. Child ranges are `[key_i, key_(i+1))`, with an unbounded
last upper end. A child must be nonempty and have that exact minimum key. Root
construction collapses a single-child root. Lookup chooses the greatest minimum
not exceeding the target; traversal validates the child bounds it visits. Only a
complete walk proves that unvisited children and separators are globally correct.

| Kind | Leaf type | Key and order |
| --- | --- | --- |
| 1 volume catalog | 1 | Volume ID, 16-byte lexicographic order |
| 2 volume names | 2 | Name bytes, unsigned lexicographic order |
| 3 allocation | 3 | First block, `u64` numeric order |
| 4 objects | 4 | Object ID, 16-byte lexicographic order |
| 5 directory | 5 | Name bytes, unsigned lexicographic order |
| 6 file extents | 6 | Logical first block, `u64` numeric order |
| 7 grants | 7 | Object ID, principal ID, scope byte; lexicographic tuple |
| 8 orphans | 8 | Object ID, 16-byte lexicographic order; requires volume ORPHANS |

Internal numeric keys encode eight little-endian bytes but compare numerically.
Name keys have no terminator; ID keys have exactly 16 bytes; grant keys have
33 bytes. Repeated grants for the same key are forbidden; union their rights in
one record. Bounds, key types and owner context are concrete per index, not
application-defined callbacks stored in the image.

Directory and orphan private editors use a stronger occupancy profile within
these encodings: non-root nodes hold at least six records or children; a root
leaf may hold any positive count and an internal root at least two children.
Actual byte fit, exact minima and uniform leaf depth still apply. The formatter
repairs short final directory groups at every level using ordered adjacent
merge/repartition. Ordinary read-only access and complete checking retain the
broader nonempty-leaf/two-child internal shape contract. No occupancy feature
bit or structure-version change is introduced.

## Leaf record layouts

Offsets below include the 16-byte record header. Record type equals leaf type
from the table. Reserved fields and name-storage suffixes are initially zero.
Readers accept reserved extension bytes under the compatibility rules; a shorter
than defined record is corrupt. Version-1 records may grow only through declared
compatible extensions, never by moving existing fields.

### Volume record, 448 bytes

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 16 | 16 | Volume ID/key |
| 32 | 2 | Name length |
| 34 | 6 | Reserved |
| 40 | 256 | Name storage; only the named prefix is interpreted |
| 296, 304, 312 | 8 each | Read-required, write-required, optional feature masks |
| 320 | 16 | Root-directory object ID |
| 336 | 24 | Required object-index root |
| 360 | 24 | Grant-index root, nullable |
| 384, 392 | 8 each | Guarantee and quota, in physical blocks |
| 400, 408 | 8 each | Live and retired owned block counts |
| 416 | 8 | Object count |
| 424 | 24 | Nullable orphan-index root when read-required ORPHANS is set; otherwise reserved |

Volume-name records have name length at +16 (`u16`), six reserved bytes at +18,
volume ID at +24 (16 bytes), then name bytes at +40, rounded to eight-byte record
length. Both catalogs are one-to-one and agree exactly. Volume ownership policy
is represented by its root object and grants, not a second implicit owner bypass.
An unsupported volume's fixed envelope remains readable if its record version
is supported; otherwise pool catalog interpretation is unsupported.

The ORPHANS bit declares the use of offset 424; the volume record remains
448 bytes. Its orphan tree has the owning volume set and owning object zero.
An empty orphan index has a null root, and its entry count cannot exceed the
volume object count. The feature permits a parentless non-root object only with
the named-or-orphan validation described below. The formatter emits no orphan
feature or entries. The first unlink/replacement enables it atomically, preserving
the older state's own mask and requiring the formerly reserved bytes to be zero.
The bit remains enabled after cleanup. Read-only opening never enables it or
writes; admitted writable opening may clean existing orphans under the documented
adapter recovery precondition.

### Object record, 128 bytes

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 16 | 16 | Object ID/key |
| 32 | 2 | Kind: 1 regular file, 2 directory |
| 34 | 2 | Storage kind: 0 none, 1 inline extent, 2 tree |
| 36 | 4 | Reserved |
| 40 | 16 | Policy-owner principal ID |
| 56 | 16 | Parent object ID; zero for the volume root or an ORPHANS-enabled orphan |
| 72 | 8 | File byte length; zero for directories |
| 80 | 32 | Storage payload, interpreted by kind below |
| 112 | 16 | Reserved |

| Storage kind | Payload at +80..111 | Allowed object kinds |
| --- | --- | --- |
| 0 none | All zero; no physical extent or directory index | File with zero extents (including a nonempty all-hole file), or empty directory |
| 1 inline extent | Four `u64`: logical first block at +80, count at +88, physical first block at +96, allocation birth at +104 | File with exactly one extent |
| 2 tree | Non-null 24-byte index-root reference at +80, directory entry count `u64` at +104 (zero for files) | File with at least two extents, or nonempty directory |

An unknown storage kind is `UNSUPPORTED`; a known kind used with an invalid object
kind or payload is `CORRUPT`. Never interpret an unknown mode as a hole. Inline
mapping metadata is covered by the object record checksum, without a nested record
header or checksum. File contents remain in allocated data blocks. Directory count
is implicit zero in none mode and positive in tree mode. A directory cannot use
inline mode. An empty file uses none; a single sparse extent need not begin at zero.

The formatter chooses none/inline/tree for zero/one/multiple file extents. The
reader supports all three and applies identical range and ownership checks to
inline and tree mappings. The checker includes inline claims in allocation and
cross-state comparisons and in the global extent-count limit; it verifies a file
in tree mode has at least two extents. Ordinary lazy access does not claim to have
counted an entire unvisited extent tree. When a file needs a second extent, promote
the inline mapping to ordinary extent records and publish the new discriminator
and root together; the unchanged data need not move. Any later writable reduction
to one or zero extents restores the corresponding canonical mode.

The packed record remains 128 bytes. With a 192-byte node header and four-byte slots,
`192 + align8(4*n) + 128*n` fits 29 objects in 4024 bytes; 30 require 4152 bytes.
A separate 32-byte addition would yield 160-byte records and only 23 objects per
leaf. The tagged payload avoids that density cost.

Representative modeled imports of tracked local source sets at Pyxis OS revision
`f2d3444b09`, excluding submodules and generated/untracked files:

| Source set | Files / nonempty | Directories including root | Packed object leaves | Extent-leaf space saved |
| --- | --- | --- | --- | --- |
| `kernel`, `include`, `lib` | 199 / 199 | 27 | 8 | 796 KiB |
| `docs` | 83 / 83 | 6 | 4 | 332 KiB |
| All parent-repository tracked regular files | 375 / 375 | 51 | 15 | 1500 KiB |

These are source counts and layout arithmetic, not formatted-image measurements.
They assume each nonempty file is allocated contiguously as one extent. Directory
indexes and data allocation are unchanged; internal-tree and allocation-map effects
are not included. At 158,000 tiny nonempty files, inline mappings save 158,000
extent leaves, or 617.1875 MiB, while data still needs at least that much space.

Without ORPHANS, every non-root object has exactly one naming entry and that
directory as parent. With ORPHANS, every non-root object is either named exactly
once with that parent or has zero parent, no naming entry and exactly one orphan
entry. The root is a directory with zero parent, no incoming naming entry and no
orphan entry. No hard links, cycles, cross-volume references or unexplained
objects are valid. Object counts, grants, file mappings and live allocation
claims include orphans; an orphan directory is empty and owns no directory tree.
Full checking proves this rule independently in each retained state. Diagnostic
object/read/grant APIs may inspect validated orphans by ID, but an orphan has no
ordinary acquisition ancestry and returns `NOT_FOUND` there. It cannot be reached
by namespace paths or listing.
Persistent timestamps and host UID/GID/mode fields are absent.

Directory-entry records have name length at +16 (`u16`), child kind at +18
(`u16`), four reserved bytes at +20, object ID at +24, and name bytes at +40,
rounded to eight-byte record length. Kind and parent must agree with the object
record. Names are unique within the directory.

### Orphan record, 32 bytes

Record type 8 contains only its object ID at +16 after the 16-byte record header.
The ID is nonzero and unique in the per-volume orphan index. Its target exists,
is not the volume root and has zero parent. The record requires the volume's
read-required ORPHANS bit. Cleanup progress uses the ordinary object, extent and
grant indexes; there is no hidden progress record. Codecs and checking support
this representation; creation, unlink and cleanup operations remain unimplemented.

### File extent, 64 bytes

Tree extent records contain `u64` logical first block at +16, block count at +24,
physical first block at +32, allocation birth generation at +40, then 16 reserved
bytes. Inline extents encode the same four fields in the object storage payload.
Both forms obey the following rules. Count and generation
are nonzero. Both ranges are checked and physical blocks exclude superblocks.
Allocation birth cannot exceed the birth of the metadata block containing the
mapping, which cannot exceed the selected generation.
Logical extents are ordered and disjoint; gaps read as zero. Extents cannot
extend beyond `ceil(file_length / 4096)`; an empty file has none. The formatter
zeros unused bytes in the final physical block. Reads clamp to byte length and
never expose block padding. Separate objects cannot share data blocks in one
state. Across retained states, split claims at physical interval boundaries and
compare every overlapping live interval. Require identical volume/object identity,
allocation birth and logical-block mapping on the overlap: for physical block `p`,
`logical_first + (p - physical_first)` must agree. Extent-record boundaries may
differ. For example, old logical `[0,8)` at physical `[100,108)` may share unchanged
`[0,4)` at `[100,104)` and `[5,8)` at `[105,108)` around an overwritten middle block.
No file-data checksum is recorded.

### Grant, 80 bytes

Object ID at +16, principal ID at +32, scope byte at +48 (0 object-only, 1 subtree),
seven reserved bytes at +49, then three `u64` masks at +56, +64 and +72 for file,
directory and administration rights respectively. Subtree scope requires a
directory; file grants have zero directory mask. Grant targets must exist.

### Allocation extent, 80 bytes

`u64` first block at +16 and count at +24; state byte at +32; budget-charge byte
at +33; six reserved bytes at +34; owning volume ID at +40; allocation birth
generation at +56 (`u64`); retirement generation at +64 (`u64`); eight reserved
bytes at +72. State values: 0 free, 1 live pool metadata, 2 live volume storage,
3 retired. Charge values: 0 permanent, 1 ordinary workspace, 2 migration,
3 recovery. Free records have zero owner, generations and charge. Live pool
metadata has zero owner; live volume storage has a valid owning volume. Retired
storage retains its former owner (possibly zero for pool metadata), nonzero
birth and retirement generations, and a nonzero budget charge.

The map partitions all of `[1, B-1)` exactly once, including its own nodes and the
pool root. No implicit free gaps. Every non-free record obeys
`0 < birth <= containing_map_leaf_birth <= selected_generation`. Live records have
zero retirement generation. Retired records additionally obey
`birth < retirement <= containing_map_leaf_birth`. An immutable map leaf cannot
record an allocation or retirement introduced after that leaf was born, even when
opening a newer state. These are local record-validation rules for ordinary access
and complete checking. Adjacent extents with
identical state, owner, generations and charge are coalesced. The initial builder
has no retired or budget-charged live extents: all live storage is permanent.
Later transitional allocations may be live while charged to a workspace budget;
they are never also counted as unused budget.

## Rights and trusted acquisition

The masks keep the three rights domains separate:

| Domain | Bit | Meaning |
| --- | --- | --- |
| File | 0 | Read file metadata |
| File | 1 | Read file bytes |
| File | 2 | Write file bytes; later writable milestone |
| File | 3 | Resize; later writable milestone |
| File | 4 | Request a checkpoint; later writable milestone |
| Directory | 0 | Read directory metadata |
| Directory | 1 | List entry names/kinds |
| Directory | 2 | Look up children/derive authority within held scope |
| Directory | 3 | Create entries; later writable milestone |
| Directory | 4 | Remove entries; later writable milestone |
| Directory | 5 | Replace an existing entry; later writable milestone |
| Directory | 6 | Request a volume checkpoint; later writable milestone |
| Administration | 0 | Inspect owner and grants |
| Administration | 1 | Edit grants; later writable milestone |
| Administration | 2 | Change policy owner; later writable milestone |

Grant decoders must reject rights bits outside their supported masks; they must
not silently discard unknown rights. Adding an independent allow right whose
absence grants no authority does not itself require a feature declaration or
structure-version change. New semantics that alter the interpretation of existing
rights, scopes or records require an appropriate feature declaration or format
change when existing validation would otherwise accept an unsafe interpretation.
Writable admission must reject retained states containing rights it cannot
validate. Adding a supported right never widens existing stored grants: their
absent bit remains absent unless explicitly changed with the required authority.
`dir.checkpoint` is an independent bit, with no feature declaration or version
change. Existing stored grants gain no rights. Formatter owner grants retain
directory bits 0..5 (`0x3f`), so they do not acquire checkpoint authority.

The current grant decoder rejects unknown rights with `CORRUPT` before publishing
the decoded record. Policy acquisition propagates the failure without returning
a view, and complete checking cannot report a clean state containing such a
grant. Candidate selection does not walk all grants, so an older reader may open
the pool and inspect unrelated data before encountering and rejecting an unknown
right. This is rejection on encounter, not guaranteed rejection at pool open or
a promise of compatibility with older implementations. Unknown requested rights
or trusted rights ceilings are invalid API inputs.

A subtree record contains file rights for descendant files, directory rights
for the root and descendant directories, and administration rights for either
kind. Object-only grants apply
only to the named object and its relevant domains. Ownership alone adds no rights.
The formatter gives each volume root an explicit owner subtree grant containing
file bits 0..4, directory bits 0..5 and administration bits 0..2; descendants
inherit its owner and need no repeated grants. Directory checkpoint is omitted.
This records intended future policy, not successful mutation operations today.

Policy acquisition takes a trusted principal context, a bounded root
and scope, a rights ceiling in all three domains, a target, requested result scope
and requested rights.
The embedding authority establishes this context; untrusted callers cannot mint
one by supplying an ID. The core is a trusted library, not an authentication
service or a security boundary against arbitrary native calls into its memory.

Prove the target and requested scope fit the supplied authority scope. Object-only
acquisition unions target object-only grants with subtree grants on its ancestor
directories, including the target itself when it is a directory. Subtree
acquisition requires a directory target and uses only those applicable subtree
grants; object-only grants contribute no descendant authority. Intersect the
result with the caller's domain-specific ceilings. An object-only caller cannot
acquire a subtree or acquire another object through that authority.
Grants above a restricted authority root may contribute policy, but can never
enlarge that root or its ceiling. Require the entire requested set; do not
silently return a weaker handle. Denial returns no acquired object. After policy
authorization, requests for unimplemented mutation/administration operations return
`READ_ONLY`; they do not perform policy changes or acquire an operational write
interface.

An acquired result is bound to the open pool generation, volume/object identity,
scope and actual rights. The trusted adapter owns its lifetime and delegation;
Caelum will wrap it in native capabilities. Deriving a child from an existing
subtree result checks held scope/rights, not the principal's policy afresh. Listing
alone does not confer lookup or file-read rights. There is no stored `..`; caller
traversal contexts cannot escape the acquired root. Changes to persistent grants
affect future policy acquisition, not retained subtree authority.

Ordinary operation checks are explicit; none of the metadata operations disclose
policy-owner or grant records without `admin.inspect`:

| Operation | Required held authority | Returned information |
| --- | --- | --- |
| File metadata | `file.metadata` on that file | Pool/volume/object identity, kind and byte length |
| Directory metadata | `dir.metadata` on that directory | Pool/volume/object identity, kind and entry count |
| File read | `file.read` on that file | Requested in-range bytes; no implicit metadata or administration rights |
| List directory | `dir.list` on that directory | Entry names and kinds only; no child capability, identity, size, owner or grants |
| Inspect policy | `admin.inspect` on that object | Policy-owner ID and explicit grants on that object; no implicit read/list rights |
| Look up a child / derive a child view | Subtree scope, `dir.lookup` on the containing directory, requested child rights contained in held domain masks | Child identity/kind and a view restricted to the requested rights and scope |

Multi-component lookup requires `dir.lookup` at every traversed directory, including
the supplied root and excluding the final target. Derivation preserves generation
and volume, cannot expand a subtree boundary or add rights, and cannot derive
subtree scope for a file. Intermediate views retain only rights needed for the
remaining traversal and result. A final object-only directory view cannot derive
children even if its stored mask contains `dir.lookup`. No operation implicitly
performs a fresh policy acquisition to enlarge an existing view.

Policy acquisition by object ID must meet the same lookup checks as acquisition
by path. After proving the target's validated parent chain reaches the supplied
root, require effective `dir.lookup`
for the principal at each intervening directory, intersected with the trusted
context's directory ceiling. The target equal to the supplied root needs no
traversal check. Structural parent walking uses the trusted core without disclosing
those internal records to the caller. An ID is a selector, not extra authority.
Diagnostic inspection remains a separate authorized entry point. Acquiring by ID
cannot bypass ordinary lookup policy or enlarge retained-capability delegation.

Inspection has an explicitly separate diagnostic entry point over an authorized
image. It may reveal all recorded principals, names and bytes without pretending
to authenticate as an owner. A command that evaluates policy is a diagnostic
simulation of supplied trusted inputs, not a login mechanism.

## Accounting and defaults

Complete checking derives counts from all allocation records for each retained
state and reconciles cached totals. Candidate selection and ordinary access make
only the narrower claims defined above. Live volume storage includes data,
object/directory/extent/grant nodes and other volume-owned metadata. Pool catalog/map
nodes and the pool root are pool metadata. Neither superblock slot is in `U` or any volume quota.
Retired blocks retain ownership for reporting but are charged to their recorded
workspace budget, not twice as live permanent allocation.

Let `A_i` be volume i's permanent live allocation, `G_i` its guarantee, `Q_i` its
quota, `O` all non-free blocks, and `R_j`, `C_j` each workspace budget and its
currently occupied charge. Validate `A_i <= Q_i`, `G_i <= Q_i`, `C_j <= R_j`,
all cached counts, and:

```
O + sum(max(G_i - A_i, 0)) + sum(R_j - C_j) <= U
```

Every occupied block contributes once to `O`, regardless of its accounting owner
or budget. Live budget-charged storage is reported separately from `A_i`; quota
checks for a future transaction use the projected permanent result. Retired
blocks do not inflate a volume's unused guarantee. A later allocator must enforce
the same promises at admission; this reader only validates the recorded state.

Let `P` be permanent live pool metadata. Since every other occupied block is
either permanent live volume allocation or charged to exactly one workspace,
`O = P + sum(A_i) + sum(C_j)`. The capacity inequality is equivalently:

```
P + sum(max(A_i, G_i)) + sum(R_j) <= U
```

Keep the separate `C_j <= R_j` checks: cancellation in the aggregate equation
does not permit overrunning any individual budget.

The previous defaults (approximately 104 GiB combined at 1 TiB) were rejected in
follow-up review. The owner requests roughly 16–24 GiB combined at that size, with
32 GiB at most for the default. This is not a global cap on all pool sizes or on
explicit overrides. The owner accepted the following lower proportional defaults
in follow-up review. Units are blocks, with ceiling division:

| Budget | Default |
| --- | --- |
| Ordinary COW workspace | `max(1024, ceil(U / 128))` |
| Migration workspace | `max(1024, ceil(U / 128))` |
| Recovery workspace | `max(256, ceil(U / 256))` |

The proportional terms give equal capacity to ordinary COW and migration, with
half that amount for recovery. This is a prototype capacity policy, not a measured
operation-cost ratio. Floors remain 4 MiB, 4 MiB and 1 MiB respectively.

| Pool extent | Ordinary COW | Migration | Recovery | Combined budgets |
| --- | --- | --- | --- | --- |
| 64 MiB | 4 MiB | 4 MiB | 1 MiB | 9 MiB |
| 256 MiB | 4 MiB | 4 MiB | 1 MiB | 9 MiB |
| 1 GiB | 8 MiB | 8 MiB | 4 MiB | 20 MiB |
| 16 GiB | 128 MiB | 128 MiB | 64 MiB | 320 MiB |
| 64 GiB | 512 MiB | 512 MiB | 256 MiB | 1.25 GiB |
| 256 GiB | 2 GiB | 2 GiB | 1 GiB | 5 GiB |
| 1 TiB | 8 GiB | 8 GiB | 4 GiB | 20 GiB |

The table uses `U = extent_bytes / 4096 - 2` and rounds each budget up to whole
blocks. Totals exclude both occupied permanent storage and unused volume guarantees;
those remain separate terms in the capacity inequality and formatter plan.

A compared capped alternative retains the old fractions (`U/32`, `U/16`, `U/128`)
but caps default budgets at 8 GiB, 12 GiB and 2 GiB. It gives 22 GiB at 1 TiB,
yet retains 26 MiB at 256 MiB, 104 MiB at 1 GiB and 6.5 GiB at 64 GiB. The lower
proportional policy reduces that cost across development image sizes and needs
no arbitrary transition point. Neither policy proves recovery or migration
sufficiency; a cap alone supplies no bound on the work it must fund.

These are capacity-policy defaults and floors for the read-only prototype, not
proven writable recovery minima. Writable admission requires a separately
reviewed worst-case operation cost and may reject images whose recorded budgets
cannot support it. The fields are wide enough to raise budgets without a format
change, subject to available capacity and protected guarantees. Do not call an
image writable merely because these formulas fit.

The formatter first computes exact permanent allocation for the populated image,
including all index/map nodes, then plans promises. After reserving the three
budgets and any explicit unused guarantees, divide half the remaining unpromised
free blocks evenly between volumes without an explicit guarantee; assign any
remainder in volume-name order. Their guarantees are `A_i + share_i`. An explicit
guarantee below existing allocation is permitted, provided quota covers both.
Default quota is `U - live_pool_metadata - sum(R_j)`, raised to neither mask an
error nor fit an oversized import: if it cannot cover `max(A_i, G_i)`, reject the
plan. Quotas may collectively exceed capacity; hard guarantees may not.

Overrides use whole blocks and cannot lower the policy floors. Reject
any plan violating the inequality, count limits or requested quotas. Report all
computed allocations, unused guarantees, reserve budgets and unpromised space
before creating the image. Planning allocation-map space includes the map's own
blocks; no hidden unaccounted metadata reservation is allowed. The implemented
bulk builder uses the [accepted contiguous layout](empty-layout.md#allocation-map-termination-bound):
one pool metadata prefix, one metadata/data range per volume, and one free suffix.
N+2 records require at most seven map blocks, with no iteration, for both empty
and populated images.

## Future publication and reclamation envelope

This describes the constraints the layout must support, not writable code.
One pool writer serializes publication. Admit only a bounded mutation after
reserving its replacement data, changed index paths, changed allocation-map paths
and commit bookkeeping. Bound dirty bytes/nodes and outstanding generations;
large writes need multiple admitted operations. A volume checkpoint freezes a
sequence boundary and cannot succeed until every earlier accepted operation in
that volume is included in the published pool state. Other volumes may be
included as a consequence of pool-wide publication.

Construct a candidate with generation greater than both durable slots. Existing
storage required by either slot, any live reader or outstanding I/O is protected.
Record replaced extents as retired in the candidate, with the original owner and
birth generation, the candidate's retirement generation and a workspace charge.
Write new data/metadata and flush; replace the older slot and flush again. Only
then acknowledge durability. Uncertain write or flush failure stops mutation and
requires recovery. No automatic retry or counter-based guess proves completion.

The allocation map and pool root themselves are COW metadata: their current and
older blocks obey the same protection rule. New map nodes are allocated from
storage already proven free before publication and recorded as live in that
candidate. Old map/root blocks cannot be omitted from ownership or reused while
another retained root can name them. This avoids treating allocator metadata as
an exception to the allocation protocol.

A future reclaim pass may mark a retired extent free only if complete supported
evidence for both currently durable states proves that neither has a live
reference to it, and all older reader/I/O references have ended. Retired entries
alone are ownership records, not live data references. If either retained state's
map calls a range live, protect it even if a partial traversal found no reference.
Compare owner/birth when both states describe the same retained incarnation.
A free entry has no incarnation; an older historical retired entry may differ
from a newer live incarnation. Any live claim still prevents reclamation of the
physical range. Degraded or unsupported state prevents writable reclamation.

Publish the free-map change using the same replacement/flush protocol, allocating
its own metadata from previously reusable space. Do not allocate the newly freed
range during that publication. It becomes reusable only after successful durable
publication and the same retained-state/reader check against the resulting pair.
Crash recovery always recomputes safety from the selected and retained roots;
a retirement generation alone is insufficient evidence. An old root may still
record an extent as retired after the newer root reuses it, but it must not record
it as live. The checker distinguishes that historical incarnation from a new live
allocation instead of summing both state maps as current consumption.

Example for physical range X, omitting unrelated allocations:

| Durable pair | X in older / newer state | Consequence |
| --- | --- | --- |
| 10 / 11 | live / retired | Generation 10 still protects X. |
| 11 / 12 | retired / retired | Neither durable state refers to live X, but a reader or I/O still pinning 10 prevents freeing it. |
| 12 / 13 | retired / free | After the old reader/I/O ends and required evidence holds, a separate free-map publication records free X. That publication cannot itself reuse X. |
| 13 / 14 | free / new-live | Only after the free publication is durable and protection is rechecked may a later publication reuse X with a new birth generation. |

Allocator/map nodes obey the same sequence as data. These are evidence requirements,
not a mandate to rescan the whole pool for every reclaim batch. A future writer
may maintain equivalent validated summaries and reference tracking, but must
specify how they stay complete across publication and recovery before relying
on them. The offline checker's inability to observe runtime pins is unchanged.

The [accepted writable milestone](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/wip/writable-filesystem-core.md)
defines bounded edits, allocator self-accounting, orphan retention and numerical
admission/recovery promises. Task 2 implements the private planning and format
support described in [the core document](core.md#private-candidate-planning).
Writable admission, durable publication, pin tracking and funded reclamation
remain unimplemented. Prototype reserve defaults do not establish those promises.
Migration still needs its own publication and ownership-transfer protocol when
a real second format exists.

## Core platform and object lifetime

Platform operations are synchronous exact block read, builder-only exact
block write and flush, bounded allocation and free, each with an opaque adapter
context. The adapter reports geometry and a maximum transfer of at least one
filesystem block. Short I/O is an error. It borrows buffers only until return;
no operation retains caller memory afterward. Read-only opens have no write
callback path. Close performs no implicit writes, repair or durability operation.

The host adapter obtains a shared lock for inspection and exclusive ownership of
a newly created output for construction. Locks are advisory: external writers
that ignore them are outside the consistency contract. Reject detectable extent
size changes and never promise an atomic snapshot of a concurrently changing
file. One open core instance is used serially; thread safety and concurrent
mutation are not supplied. Separate read-only instances can coexist on unchanged
media; duplicate pool IDs cannot be attached as independent writable pools later.

An open pool owns its cache and allocations, and pins one selected generation.
Volume and object views reference that pool and cannot outlive it. Close while
views or operations remain returns `BUSY`; it does not silently invalidate them.
Read/list calls take caller-owned bounded output storage and return copied data,
never borrowed cache pointers. Reads return an explicit byte count and status;
on failure, only the reported prefix is valid and the rest is unspecified.
Metadata lookups publish no partially initialized result. Directory cursors are
bound to pool generation, volume and directory; no disk pointer is a user cursor.

This is the current read-only view lifetime, not a future live-capability lifetime.
Writable integration must distinguish live file capabilities from explicit
snapshot/read views and in-flight I/O pins. A long-lived live handle must not
implicitly retain its opening generation forever and exhaust COW workspace.
The live-view refresh and pin-release contract is a writable-implementation gate.

An allocation cap includes cache, traversal stacks, copied records and checker/
builder bookkeeping. Cap exhaustion is `LIMIT`; an allocator failure below the
cap is `NO_MEMORY`. No recursive walk may consume an unbounded C stack. Checked
arithmetic or media validation never calls the allocator using an unchecked count.

## Host command contract

`mkpyxisfs` accepts `--image PATH --size SIZE` and one or more volume groups:
`--volume NAME [--source DIRECTORY] --owner PRINCIPAL_ID`, optionally followed by
`--guarantee SIZE --quota SIZE`. Each volume option applies only to its current
group; duplicate/misplaced options are errors. No source means an empty volume.
Pool options are `--cow-reserve SIZE`, `--migration-reserve SIZE`,
`--recovery-reserve SIZE`, `--memory-limit SIZE` and `--plan`. Sizes are unsigned
decimal bytes optionally suffixed `KiB`, `MiB`, `GiB` or `TiB`; reject signs,
fractions, overflow and non-block-multiple capacity values. Quote shell arguments
normally; names or paths containing spaces are not split by the tool.

`--plan` performs bounded source traversal and allocation/accounting planning,
prints the resulting capacity plan and creates no image. A normal invocation
also prints its plan before creation. Random IDs need not match between separate
plan/build invocations. The real build must revalidate sources and capacity; a
previous plan is not an authorization token or a source snapshot.

Example syntax, with an explicitly provisioned illustrative principal ID:

```sh
mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --source /tmp/source-home \
  --owner 0a32efc079ed4c7bab58e224cf119315 --plan
```

Output must be a new regular file created with exclusive creation and restrictive
mode 0600. No force flag, truncation, block-device destination, in-place update,
GPT creation or partition formatting. Resolve output parent directories without
following symlinks, retain their directory descriptors, and create the final
component relative to the retained parent. Reject `..` components. Refuse an
output parent inside any imported source tree, checking directory identities
rather than string prefixes; a source must not ingest the image being built.
Never unlink or replace a path merely because an earlier operation failed.

Traverse sources relative to retained directory descriptors without following
symlinks, including the selected source root. Reject non-regular/non-directory
entries. Hard-linked source files become separate objects and separate extents;
do not import host policy, timestamps or ownership. Compare opened file identity,
size, mtime and ctime before/after copying, and recheck traversed directories for
detected changes. Read errors, growth/shrink, replacement or unsupported names
abort construction. Sources must be quiescent; these checks do not prove an atomic
snapshot against concurrent modification. Initial population materializes source
holes as zero data; the reader supports sparse extents, but no sparse-preservation
or alternate-generation fixture is introduced in this milestone.
Plan contiguous data for each imported nonempty file and use its inline descriptor;
empty files need no data mapping. Account for extent-tree nodes when a supported
multi-extent layout requires them. Initial contiguous population alone does not
demonstrate traversal of multi-extent trees; that validation limit must be reported.

Bound planning memory and retain sufficient identity information to detect changes
between sizing and copying. Construct all roots and allocation ownership before
publishing superblocks. Flush metadata/data, write both generation-1 slots, then
flush the output and its containing directory before success. On error report
the output as incomplete, close it and leave it for explicit owner removal.
Interruption can leave zero, one or two readable slots; this is not a resumable
format operation. Do not infer successful formatting merely from a readable slot.

`pyxisfs-inspect` takes `--image PATH`, optional `--memory-limit SIZE` with the
same bounds as the formatter, and one command:

| Command | Result |
| --- | --- |
| `info` | Geometry, slot classifications/selection, IDs, features and recorded accounting, explicitly not globally verified |
| `volumes` | Catalog order by name, including unsupported-volume status |
| `list --volume NAME --path PATH` | Directory entries, metadata and identities |
| `stat --volume NAME --path PATH` | Object identity, owner, type, size and grants |
| `extract --volume NAME --path PATH --output NEW_PATH` | One file or a complete directory subtree |
| `check` | Complete supported inspection of both retained states |
| `access --volume NAME --root PATH --principal ID --target PATH --scope object\|subtree --rights LIST --ceiling LIST` | Explain read/list/policy-inspection acquisition under an explicit subtree ceiling; no persistent changes |

Rights lists are comma-separated domain-qualified tokens, or `none`. File tokens
are `file.metadata`, `file.read`, `file.write`, `file.resize`, `file.checkpoint`;
directory tokens are `dir.metadata`, `dir.list`, `dir.lookup`, `dir.create`,
`dir.remove`, `dir.replace`, `dir.checkpoint`; administration tokens are `admin.inspect`,
`admin.grants`, `admin.owner`, in the bit order defined above. Reject unknown or
duplicate tokens. `--scope` selects the requested result scope; the diagnostic
caller context is the subtree at `--root`, bounded by `--ceiling`. Requests for
currently unimplemented rights report `READ_ONLY`, even if policy would allow
them. Report policy allowance separately from available operations.

`--volume-id ID` substitutes for `--volume NAME`, never combines with it. Paths
inside a volume are slash-separated relative to its root; `.` denotes the root
as command syntax, not a stored entry. Reject `..`, empty interior components and
invalid names. `access` interprets target relative to its selected root, preventing
an unbounded path shortcut. Output escapes control bytes in names instead of
emitting terminal controls. `info` may report malformed/unsupported slot envelopes
without opening normal access, but must label unvalidated fields accordingly.

Extraction creates a new file or new top-level directory, never merges into an
existing tree. Open each output component relative to a retained directory
descriptor, rejecting symlinks and `..`; use exclusive creation and modes 0600
for files, 0700 for directories, before a stricter caller umask. Flush files and
completed directories, then the containing parent before success. Do not restore
host ownership or modes from principal IDs. A failure may leave a partial output
tree, clearly reported; do
not recursively delete it automatically. File reads clamp to logical length and
zero-fill holes. Extraction is diagnostic authority, not policy-based acquisition.

Standalone pool images are the default, with no probing for a filesystem at
arbitrary offsets. For an existing whole-disk regular image, require explicit
`--gpt-partition N --sector-size 512|4096`; N is a one-based used entry number.
The host adapter validates GPT under the existing
[bounded GPT contract](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/devices/gpt.md),
including both copies and the protective MBR. No automatic partition/type/name
selection, GPT repair, external tool subprocess or raw block-device access.
Selecting a partition explicitly permits inspecting its contents regardless of
its type GUID; a supported Pyxis superblock is still required. No Pyxis GPT type
GUID is assigned by this task. GPT degraded status and pool degraded status are
reported separately. An error in either selection layer prevents normal access.

GPT parsing belongs to the host container adapter, outside the filesystem core;
it must match the documented kernel GPT profile without including kernel headers
or introducing a second filesystem decoder/writer. Future kernel integration
supplies the already selected partition extent. Refactoring shared GPT parsing
is separate work if concrete duplication later warrants it.

Exit status: 0 completed success, 1 denied policy diagnostic, 2 usage/invalid
input, 3 corrupt or ambiguous media, 4 unsupported, `READ_ONLY`, `LIMIT` from any
command, or incomplete checking, 5 I/O or allocation failure. `access` reports
policy denial as 1; if policy allows the request but its operations are unavailable,
`READ_ONLY` is 4.
A complete `check` requires two valid, fully checked slots: one valid with an
absent peer is degraded/incomplete (4), and one valid with a corrupt peer is 3.
For non-`check` diagnostic/read commands on a selectable degraded pool, the tolerated
absent/corrupt peer is reported as a warning and excluded from failure aggregation.
A successful operation returns 0. For example, successful `stat` with a valid
selected slot and a CRC-bad peer returns 0 with the peer warning, while `check`
returns 3 for the same pair. Corruption on the selected operation's traversed path
still fails with 3. An unsupported, limit, I/O or allocation failure in either
candidate still prevents selection; it is never downgraded to a peer warning.

The per-command peer rule takes precedence over generic aggregation. Among
remaining failures, prioritize operational
I/O/allocation failure (5), proved corruption/ambiguity (3), then incomplete (4);
retain all individual reasons in the output. Every non-success diagnostic identifies
the operation, available object/block context and whether partial output exists. Missing volume
or path is an input/selection error, not a successful empty listing. Tool output
is human-readable; stable JSON output is not part of this task.

## Whole-image consistency operation

`check` validates both candidates fully, even when only the newer generation is
selected for ordinary reads. Identical roots can share traversal work, but each
state's ownership and counts are reconciled separately. Report selected and
retained generation results individually. A single valid root with an absent or
corrupt peer is degraded, not a clean two-root image. An unsupported volume,
unvisited subtree, memory cap or I/O failure makes the global result incomplete
or failed, never clean.

Any unknown enabled pool/volume feature makes the full result unsupported and
incomplete, including a feature compatible with ordinary reads. Continue only
checks meaningful under supported semantics. Do not classify unexplained live
allocations as corruption when an unknown feature may own them, and never
suppress independently established corruption elsewhere.

Walk each supported tree with bounded depth, track visited metadata identities
and reject cycles, duplicate parents within a tree, misordered keys, separator
violations and context mismatches. Reconcile both volume catalogs. Check object
IDs, parent/entry agreement, namespace reachability, root rules, depths, entry
counts, owners, grant targets and named-or-orphan relationships in
ORPHANS-enabled volumes. Check storage discriminators and canonical
none/inline/tree modes, file ranges, logical lengths and allocation incarnations.
Inline data claims are checked just like extent-tree claims. Collect extent claims
for all metadata and file data, sort by physical range and compare them with the
allocation map. No conflicting claims, missing owned ranges or unexplained live allocations are allowed. The allocation map and
pool-root blocks must be accounted for too.

Across states, unchanged physical storage may be shared only with compatible
identity and interpretation. A range retired in a newer map can be live in the
older state; a newly allocated incarnation must never overlap a live reference
in either retained state. Historical free/retired map entries are not live block
references. Retired bytes need not be parseable, and the checker cannot establish
the absence of runtime readers/I/O from an offline image. Reconcile budget charges
and ownership per state without summing historical allocations twice.

Read metadata, not every file payload, for structural checking. Report explicitly
that file-data contents have no checksums and were not integrity-verified. Content
comparison is the separate extract/compare workflow. No repair, reclamation,
automatic alternate-root retry or success after skipped required state.

## Writable implementation gates and validation

The initial format and read-only core are implemented within the
1 TiB/one-million-record profile and 128 MiB default tool budget. The
[construction layout](empty-layout.md) establishes the empty and populated
allocation-map construction bound; every metadata and data block is included
before output creation. The diagnostic `pfs_check` API and host `check` command
implement whole-image consistency inspection; interfaces and limits are recorded
in [the core document](core.md#whole-image-consistency-checking).

Private canonical/edit/map planning, finite envelopes and their arena are
implemented. The caller still supplies retained-state, reusable-range and runtime
pin proofs; planning alone establishes no durable state. The admitted publisher
establishes these proofs from checked retained summaries and explicit serial
operation boundaries before issuing writes.
Acceptance of this contract does not prove formatter reserve defaults sufficient
for writable operation. Repository licensing is established as MPL-2.0.

The shared core builds as a freestanding archive. Formatting, diagnostic
reopening, populated extraction round trips and structural checking are recorded
in [historical host validation](host-tools.md#validation). The maintained
[host contract suite and CI gate](testing.md) add synthetic fixtures and
independent expected results for planners, admission and publication, including
simulated recovery from explicitly durable backing. These bounded checks do not
establish actual-device crash recovery or whole-filesystem workload capacity.
