# Native format

Implemented by `include/pyxis_fs/native.h` and `format/`. This is the simple native
format from Pyxis task 2, beside the old `pfs_*` core. Caelum still mounts the old
format. The new archive contains codecs, CRC32C, geometry, timestamp conversion and
mapping arithmetic; it contains no I/O, allocator, cache, mount state or writer.
Consumers provide `pnf_memory_copy` and `pnf_memory_zero` at link time. There are
no environment callback tables. GNU C23 and compiler support for `__int128` are
required. Decoded C structures are not disk layouts.

## Geometry and compatibility

Blocks are 4,096 bytes. Integers are little-endian; 64-bit block pointers are
relative to the pool. Zero is the absent pointer. Pool and volume IDs are nonzero
16-byte values, not access authority. No users, permissions or grants are encoded.

Block zero and the last complete block hold identical headers, immutable while
mounted. A trailing partial block is unused. The bitmap has one bit per pool
block, least-significant bit first, rounded to whole blocks. One means allocated;
fixed regions and out-of-pool padding bits stay allocated. The table is 64 records
of 512 bytes, eight blocks. Header, bitmap, table and journal cannot overlap.
Formatting places bitmap, table and journal consecutively after block zero.
Ordinary storage follows, shared among all volumes.

The three feature masks are 64 bits each. Unknown compatible bits are ignored;
unknown read-only-compatible bits prohibit writes, including replay; unknown
required bits prohibit opening. V1 defines no extension bits. Reserved bytes are
written zero and ignored by decoders. New semantics require a feature bit whose
class accounts for both reading and updates. Codec output creates fresh records;
it does not preserve opaque extension bytes for future read/modify/write consumers.

With no compatible or read-only-compatible extension declared, undefined record
flag bits are corrupt: inode flags permit only the two timestamp-valid bits;
volume, directory-entry and journal-descriptor flags are zero. Unknown ignorable
pool features may define further flags, so older decoders tolerate unknown flag
bits when either of those feature masks is nonzero. They cannot associate unknown
flags with a particular unknown feature. Required features still refuse opening.
Free inodes and unused volume records still require zero flags in all cases.
The directory-entry codecs take the pool header for this contextual validation.
Reserved byte areas remain ignored, distinct from undeclared flag bits.

Headers and journal controls/payload have CRC32C. Home metadata and file contents
have no checksums in v1. Structural checking cannot detect arbitrary content damage.
CRC32C uses reflected polynomial `0x82f63b78`, initial and final XOR `0xffffffff`.
Block checksums cover all 4,096 bytes with their four-byte checksum field zeroed.

## Record encoding

Offsets below are byte offsets, independent of C alignment. Reserved areas remain
available for future features. Kind, state and mapping constants are in the header.
Codecs validate record shape and pointer ranges, but whole-image ownership and
reachability belong to the checker or future writer. Accessible disjoint buffers
are required; decoders publish output only on success and failed encodes leave the
destination unchanged. Directory callers supply at least the entry's encoded length.

### Pool header

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `PYXISNFS` |
| 8 | 16 | Pool ID |
| 24 | 8 | Complete pool blocks |
| 32, 40 | 8 each | Bitmap start, count |
| 48, 56 | 8 each | Volume-table start, count |
| 64, 72 | 8 each | Journal start, count |
| 80, 88, 96 | 8 each | Compatible, read-only-compatible, required masks |
| 104 | 4 | Block size, 4096 |
| 108 | 4 | Full-block CRC32C |
| 112 | 16 | Unassigned current fields |
| 128 | 3968 | Future-feature reserve |

One valid header copy suffices; two valid copies differing anywhere in their full
blocks are rejected. No valid copy is an error. Unsupported checked encodings
are rejected. The host also verifies block count against the opened regular file.

### Volume record

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 16 | Volume ID |
| 16, 24, 32 | 8 each | Root inode, cleanup head, inode-file byte length |
| 40, 44, 48 | 4 each | State, flags, mapping type |
| 52 | 2 | Name length |
| 54 | 2 | Padding |
| 56 | 256 | Counted name and unused trailing storage |
| 312 | 120 | Twelve direct and three indirect pointers |
| 432 | 80 | Future-feature reserve |

State 0 is unused, 1 live. Unused records have zero current fields. Live IDs and
names must be unique in the pool. Root is inode 1. Inode-file length is a multiple
of 256, at least 512; its mapping is dense. It can grow without a fixed volume
partition. No runtime volume-management API or quotas are supplied.

### Inode

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0, 2 | 2 each | Kind, mapping type |
| 4 | 4 | Flags |
| 8, 16, 24 | 8 each | Byte length, next cleanup inode, shrink target |
| 32 | 4 | Cleanup flags |
| 36, 44 | 8 each | Creation, modification Unix nanoseconds, signed |
| 52 | 8 | Directory parent inode |
| 60 | 12 | Unassigned current fields |
| 72 | 120 | Twelve direct and three indirect pointers |
| 192 | 64 | Separate future-feature reserve |

Kind 0 is free, 1 regular file, 2 directory. Free inodes have zero current fields.
Inode-file slot 0 is reserved/free; slot 1 is the root directory. Mapping type 1
is block pointers. Each indirect block holds 512 raw 64-bit pointers. Slots 12,
13 and 14 are single, double and triple indirect roots. Maximum mapped length is
550,831,702,016 bytes, including 12 direct blocks. Regular-file holes read zero;
linked directories and inode files are dense. Detached directory cleanup may
leave holes as blocks are reclaimed while retaining the old length. An empty directory may have length zero.

Flags bit 0 and bit 1 mark creation and modification times valid. Unknown payloads
are zero; a known epoch-zero or clamped endpoint remains valid. `pnf_timestamp`
converts signed Unix seconds plus a normalized 0–999,999,999 nanosecond fraction
using wide arithmetic and saturates at `INT64_MIN`/`INT64_MAX`. An unavailable
clock leaves the affected timestamp unknown. Times are wall time, not unique
change counters. Creation is set on allocation; modification changes for content,
size or directory edits, not checkpointing.

Regular-file parent is zero. A linked directory records its containing inode;
root records itself (1). Detached directories have parent zero and no live entries.
Parents are internal metadata, not `..` entries or capability authority. Directory
moves must update both entries and parent atomically and preserve acyclic ancestry.

Cleanup bit 0 is DETACHED, bit 1 SHRINK; both may coexist. SHRINK target equals
the current smaller size; otherwise target is zero. Non-cleanup inodes have next
zero. Each cleanup inode appears exactly once in its volume's persistent list.
Unlinked inodes have no namespace reference; every other live nonroot inode has
exactly one. Only completed reclamation with no surviving references permits reuse.

### Directory entry

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Inode number; zero means free space |
| 8, 10 | 2 each | Record length, name length |
| 12 | 4 | Flags |
| 16 | 16 | Future-feature reserve |
| 32 | Variable | Counted name, padding to record length |

Records cover each directory block, never cross a block boundary, are at least
32 bytes and multiples of eight. A free record has zero name length. Names are
1–255 UTF-8 bytes, excluding NUL, slash, `.` and `..`; overlong sequences,
surrogates and values above U+10FFFF are invalid. Names compare exact bytes with
no normalization and are unique within a directory. Lists are unsorted; lookup is
linear. There are no symbolic links or hard links.

## Journal encoding and recovery

The journal starts with two controls. Descriptor blocks follow, then metadata
images in descriptor order. Each transaction replaces complete metadata blocks;
file contents are ordered data, outside the journal. For N images the region needs
`2 + ceil(N / 128) + N` blocks. A 128 MiB journal holds up to 32,512 images.
Journal size is chosen at formatting; the 256 GB target starts at at least 128 MiB.
There is no resize operation. The codec minimum fits one image; this is not a
claim that a future kernel writer's largest indivisible operation fits.

| Control offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `PYXISJNL` |
| 8 | 16 | Pool ID |
| 24 | 8 | Sequence |
| 32, 36, 40, 44, 48 | 4 each | State, image count, descriptor-block count, payload CRC32C, full-block CRC32C |
| 52 | 76 | Unassigned current fields |
| 128 | 3968 | Future-feature reserve |

EMPTY state 0 has zero counts/payload CRC; COMMITTED state 1 has positive counts
within capacity. Formatting writes EMPTY sequences 0 and 1. Select the highest
valid sequence. Only a damaged magic/checksum envelope may be ignored as torn;
a checksum-valid malformed/unknown control, no valid control or differing blocks
with equal sequences is rejected. Sequence exhaustion stops; it never wraps.

Each 32-byte descriptor contains home address at 0 (8 bytes), metadata kind at 8
(4), flags at 12 (4), and 16 reserved bytes at 16. Kinds 1–5 are bitmap, volume
table, inode file, directory and indirect. Bitmap/table destinations must lie in
the corresponding fixed region; other destinations must be ordinary storage.
Headers/journal are forbidden. Unused descriptor slots are zero. Payload CRC32C
covers a 32-byte context (pool ID, LE sequence, LE image count, LE descriptor count)
followed by all descriptor blocks and all images, including padding.

A writer flushes ordered data and payload, publishes/flushed COMMITTED in the
older control slot, checkpoints/flushed homes, then publishes/flushed next EMPTY
in the older slot. No uncommitted metadata reaches homes. Journal space and freed
blocks remain unavailable until EMPTY is durable. `fsync`/`sync` may complete at
durable COMMITTED after required ordered data; background checkpoint finishes
before another commit. Work spanning batches waits for every covering commit.

Read-only opening requires EMPTY. Writable `fsck --replay` validates all counts,
pool binding, checksums, descriptors, unique targets and local image encodings
before the first home write. It stages the payload in memory, replays it, flushes
homes, publishes next EMPTY, flushes, then checks the complete resulting image.
It does not traverse partially checkpointed homes to authenticate log ownership.
Interrupted replay is repeatable. Invalid committed payload is an error; it is
never silently discarded. An uncertain write/flush failure stops the operation.

The future writer must provide admission bounds, free-inode state, bounded cleanup,
reference lifetime and cache policy. Cleanup removes pointers and bitmap bits in
one transaction without allocating new blocks, and frees highest mappings first.
Shrink stalls further writes/resizes of that inode; reads use the smaller length.
Growth orders newly exposed data/tail zeros before size publication. No whole-file
content atomicity is promised. The codecs encode cleanup state and fsck checks
its structural consistency; ordering, tail-zeroing and live-reference rules remain
contracts for task 3. Task 2 supplies no running writer, cleanup worker or kernel
mount for this format.
