# Filesystem host tools

`make -j16` builds `build/mkpyxisfs`, `build/pyxisfs-inspect` and the freestanding
core archive. Linux supplies file descriptors, advisory locks and `getrandom`;
host libc is confined to the command/adapter layer. The Pyxis parent target
`make -j16 fs-tools` places these outputs in `build/fs-tools/`.

## Create and reopen

Supply an actual owner principal as exactly 32 hexadecimal digits. The principal
below is illustrative; the tool does not provision identities.

```sh
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --source /tmp/source-home \
  --owner 0a32efc079ed4c7bab58e224cf119315 --plan
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --source /tmp/source-home \
  --owner 0a32efc079ed4c7bab58e224cf119315
build/pyxisfs-inspect --image /tmp/pool.raw info
build/pyxisfs-inspect --image /tmp/pool.raw volumes
```

Repeat `--volume NAME [--source DIRECTORY] --owner ID` for up to 256 volumes.
Each group imports its source directory's contents into the volume root; omitting
`--source` creates an empty volume. Nested and empty directories and regular files
are supported. Every object receives the selected owner; the root's explicit
owner subtree grant covers all defined rights. Pool, volume and object IDs use OS
strong randomness, with at most 16 attempts per identity to avoid zero/collisions.

`--guarantee SIZE` and `--quota SIZE` apply to the preceding volume. Global
`--cow-reserve`, `--migration-reserve` and `--recovery-reserve` override the agreed
reserve defaults, subject to their floors. Sizes are unsigned decimal bytes with
optional `KiB`, `MiB`, `GiB` or `TiB`; capacity values must be multiples of 4096.
The [format contract](format.md#accounting-and-defaults) defines the
capacity policy. The [construction layout](empty-layout.md) accounts for every
metadata and data block, including the allocation map itself, before creating
output.

The plan reports allocations, guarantees, unused guarantees, quotas, reserves and
unpromised space in blocks. `--plan` creates no file; a real invocation also prints
its plan before creating anything. Separate invocations generate different IDs.
Output is a new sparse regular file, mode 0600 before any stricter caller umask.
Its logical size is pool capacity; host disk space is not preallocated. Host space
exhaustion remains an I/O failure. Existing output is never overwritten.

Creation traverses parent directories through retained descriptors without
following symlinks, rejects `..`, and uses exclusive creation. An output parent
inside any imported source is refused by directory identity. It holds an
exclusive advisory lock, writes metadata and file contents, revalidates sources,
flushes, writes both generation-1 superblocks, flushes the file and containing
directory, then closes. Failure after creation leaves an explicitly reported
incomplete image for the owner to remove.
Readable slots alone do not establish that formatting completed successfully.

## Source import

The formatter retains source-root descriptors and bounded manifests of object
metadata and host identities. It scans entries relative to retained directory
descriptors, without following symlinks, including in the selected root's path.
Reject `..`, invalid UTF-8 names, symlinks and all entries other than regular files
or directories. Hard links become independent files with separate IDs and data
allocations. Host ownership, permissions and timestamps are not imported.

The scan records device/inode, type, size, mtime and ctime. Reopening checks each
ancestor against that manifest; copying checks the opened file after each read.
All source objects are revalidated before output creation and again after data
copying, before publishing slots. Detected changes, replacements, short reads or
host I/O failures abort construction. Sources must be quiescent; this is change
detection rather than an atomic source snapshot. Source-root descriptors remain
open until cleanup. Other open source descriptors are bounded by directory depth
and one streamed file, rather than the total manifest size.

Each nonempty file receives one contiguous inline extent; host holes are read as
zeros and materialized as allocated data. Empty files allocate no data, and final
block padding is zero. Planning charges all copied metadata, sorts and node
layouts; file data streams through a bounded buffer instead of being retained in
memory. A plan includes every occupied block and checks capacities, reserves,
guarantees and quotas before creating output. A separate `--plan` invocation is
not a source snapshot or authorization for a later build.

## Inspect images

Inspection opens an existing regular image read-only and takes a shared advisory
lock. Lock contention fails immediately. Detectable extent-size changes fail the
operation. Writers ignoring advisory locks are outside the consistency contract;
the adapter does not promise a snapshot of concurrent changes. Trailing bytes
short of a filesystem block are ignored by the core.

`info` reports both slot classifications, selection, features and recorded counts.
Its candidate validation checks envelopes and the three tree roots, without an
allocation walk. `volumes` lists copied envelopes in unsigned name-byte order
after catalog agreement and allocation proof for every consulted metadata block.
Unsupported volume contents remain visible in that listing. Neither command
claims globally verified accounting or traverses root-object/grant trees. Output
quotes names and escapes control and non-ASCII bytes as hexadecimal bytes.

## Objects and policy

```sh
build/pyxisfs-inspect --image /tmp/pool.raw list --volume home --path .
build/pyxisfs-inspect --image /tmp/pool.raw stat --volume home --path .
build/pyxisfs-inspect --image /tmp/pool.raw access --volume home \
  --root . --principal 0a32efc079ed4c7bab58e224cf119315 --target . \
  --scope subtree --rights dir.metadata,dir.list,dir.lookup,admin.inspect \
  --ceiling dir.metadata,dir.list,dir.lookup,admin.inspect
```

Select exactly one `--volume NAME` or `--volume-id ID`. `list` and `stat` paths
are relative to the volume root. For `access`, `--root` is relative to the volume
root and `--target` is relative to that selected directory. Exactly `.` selects
the respective root. Otherwise paths contain UTF-8 name components separated by
single slashes; leading/trailing slashes, empty components, `.` and `..` are
invalid. There is no symlink following or parent traversal.

`list` publishes pages of names, kinds and object IDs in bytewise name order.
If a later page fails, preceding output remains and is reported as partial.
`stat` reports identity, kind, size/count, owner and explicit grants on the object.
Inherited grants are evaluated by `access`, not duplicated in `stat` output.
These commands use diagnostic authority over the selected image. Their metadata
disclosure does not represent ordinary policy-authorized application access.

`access` simulates a trusted principal, directory subtree root and authority
ceiling. `--scope object|subtree` selects the requested view scope. Supply both
`--rights` and `--ceiling` as comma-separated exact tokens or `none`:

- `file.metadata`, `file.read`, `file.write`, `file.resize`, `file.checkpoint`
- `dir.metadata`, `dir.list`, `dir.lookup`, `dir.create`, `dir.remove`, `dir.replace`
- `admin.inspect`, `admin.grants`, `admin.owner`

The command prints the policy decision and effective rights within the ceiling.
Success requires every requested right and intervening directory lookup, then
acquires and releases the requested readonly view. Policy-approved mutation
rights report read-only and create no view. Ownership alone grants no authority.
The command does not authenticate the supplied principal; diagnostic path
selection can reveal a missing target before policy evaluation. The ordinary
core acquisition APIs enforce lookup before resolving each path component.
See [core interfaces](core.md#policy-acquisition-and-ordinary-views).

## Extract files and subtrees

```sh
build/pyxisfs-inspect --image /tmp/pool.raw \
  extract --volume home --path . --output /tmp/extracted-home
build/pyxisfs-inspect --image /tmp/pool.raw \
  extract --volume home --path notes/today.txt --output /tmp/today.txt
```

Extraction uses diagnostic authority and the shared reader. Select one file or a
complete directory subtree with the same volume/path syntax as `stat`. `--output`
is a fresh file or top-level directory; existing paths are refused, and trees are
never merged. Output parent components are opened relative to retained directory
descriptors without following symlinks or accepting `..`. Files use mode 0600 and
directories 0700 before a stricter caller umask. Principal ownership and original
host permissions are not restored.

File reads stop at logical length and zero-fill holes. Iterative directory frames
and 64 KiB copying buffers are charged to the memory cap. Each file is flushed
before close, each directory after its children, and the output parent before
success. A failure after creation leaves clearly reported partial output for
explicit removal. Nothing is recursively deleted on failure. The inspector
continues to hold its readonly image descriptor and shared lock until cleanup;
extraction does not modify the image. Explicit GPT selection applies to this
command too.

## Explicit GPT image selection

```sh
build/pyxisfs-inspect --image /tmp/disk.raw \
  --gpt-partition 1 --sector-size 512 stat --volume home --path .
```

All inspector commands accept both options together. Entry numbers are one-based
GPT entry indices, including unused slots; an unused selected entry is not found.
The sector size must be explicitly 512 or 4096. The adapter accepts only regular
image files, validates the protective MBR and both bounded GPT copies, and reports
GPT health separately from filesystem slot diagnostics. It allows a selectable
degraded GPT without repair. It does not search for a pool or infer its partition
type, sector size or location.

The supported GPT profile is revision 1, at most 256 entries, power-of-two entry
sizes of at least 128 bytes and an entry array of at most 64 KiB. CRCs, copy
agreement, usable ranges, partition overlap, identities, attributes and reserved
fields follow the agreed bounded profile. Unsupported or operational failures
prevent fallback. The selected extent becomes the core's partition-relative
4096-byte reader; its start need only be sector-aligned. Inspection never writes
GPT or pool bytes. Formatting still creates standalone images only.

## Bounds and status

All commands accept `--memory-limit SIZE`, default 128 MiB and maximum 1 GiB.
The cap charges input/planning state, construction buffers, candidate buffers,
catalog staging, traversal frames, node caches, proof bookkeeping, live views,
directory pages and GPT scratch. Fixed codec
stack frames, caller handles, argv and host allocator/libc overhead are outside
that payload counter. Source manifests grow within the same cap; their old and
new arrays are both charged during growth. The bulk planner copies the manifest
and reserves conservative node-descriptor space proportional to its object
count, plus a fixed bounded volume/work-buffer area. These simultaneous planning
allocations can exhaust the cap below the format's record-count maximum. A
formatter limit failure occurs before file creation. An inspector limit failure
publishes no volume records for a failed catalog call, though preceding selection diagnostics or
completed directory pages can already have been printed.

Exit codes are 0 for success, 1 for policy denial, 2 for invalid arguments or a
missing selector, 3 for corrupt/ambiguous media, 4 for unsupported meaning,
read-only operations, busy resources or a resource/capacity limit, and 5 for I/O or
allocation failure. An absent/corrupt peer beside a valid selected slot is a
warning for these commands; a successful operation still returns 0. Unsupported,
limit or operational failures in either candidate prevent selection. If multiple
failures exist, command aggregation prioritizes I/O/allocation, proved corruption,
then unsupported/limit, retaining both candidate diagnostics.

Whole-image checking remains task 6. These tools do not mount, mutate or repair
existing images, or establish that recorded reserves suffice for writes.
Imported images exercise contiguous inline extents; multi-extent and sparse-image
runtime coverage remains limited.

## Task-3 validation

Native GCC 16.2 and the Pyxis cross compiler built this slice. Relocatable linkage
of all cross-compiled core objects has no unresolved symbols. Manual formatting
and reopening covered a 64 MiB single-volume image, a 64 MiB image with 256
maximum-length names in reverse input order, and a 256 MiB two-volume image with
Unicode/space names, explicit reserve overrides and a zero guarantee/two-block
quota. The largest case exercises multi-level catalogs and allocation lookup;
its recorded accounting is still not a complete map reconciliation. A 1 TiB plan
reported 8+8+4 GiB reserves and created no file. Debugger inspection observed the
empty root's owner and matching subtree grant with all defined rights.

Manual refusals covered an existing destination, impossible guarantees, low
planner/open/listing memory caps, a zero owner ID and `..` in the output path.
The single-volume image used 32 KiB host allocation for 64 MiB logical size and
mode 0600 on the validation host. Corrupt/unsupported/degraded or differing-
generation selection and actual I/O/flush failure remain code-inspection coverage;
no damaged-image fixtures or fault injection were used. No kernel mount or QEMU
validation is claimed by this host-only task.

## Task-4 validation

Native and Pyxis-cross builds passed; combining all cross-compiled core objects
left no unresolved symbols. Manual use of a formatter-created 64 MiB empty pool
covered root stat and listing, selection by volume ID, owner acquisition,
non-owner and ceiling denial, mutation refusal after policy allowance, and a
missing path. Debugger inspection observed busy volume/pool closes while a view
was live and zero charged memory after cleanup.

Healthy 96 MiB GPT images made with ordinary `sfdisk` and `dd` contained the same
64 MiB pool. Inspection succeeded with 512-byte sectors at partition LBA 2049
(a start not aligned to 4096 bytes), and 4096-byte sectors at LBA 256. Whole-image
SHA-256 values were unchanged after inspection. An unused GPT entry and a low
memory cap were refused.

Task 4 did not exercise populated namespaces, nested acquisition or actual file
contents. Its inline/tree extent, sparse-hole, malformed-metadata, grant-free
volume, GPT degradation/ambiguity and I/O-failure coverage was source review;
task 5 adds the populated round trips below. No custom image writer, damaged
fixture, fault injection, kernel mount or QEMU validation was introduced.

## Task-5 validation

The native `make -j16 fs-tools` and Pyxis-cross archive builds passed; the combined
cross core has no unresolved symbols. A 128 MiB image imported the current
documentation and kernel source directories into separate volumes (88 and 104
objects), alongside an empty volume. Reopening and extraction of both source
trees completed, and ordinary recursive `diff` reported identical contents.
These object counts exercise object trees with multiple levels.

A separate 64 MiB image imported a nine-object source containing Unicode/space
names, nested and empty directories, an empty file, binary files of 4097 and
476776 bytes, a sparse host file of 8193 bytes, and a hard-linked source copy.
Extraction matched the source with recursive `diff`. The hard-linked inputs
received different object IDs and extracted host inodes. A single 4097-byte file
also extracted byte-for-byte. The source hole was materialized as zero data;
this does not exercise a sparse filesystem extent layout.

Manual refusals covered source/output containment, a source symlink, insufficient
planning memory and a quota below the import's allocation, all before image
creation. Existing extraction output was left untouched. Nested owner acquisition
succeeded; missing lookup rights and a different principal were denied. Debugger
inspection derived a file view under held subtree `file.read` and `dir.lookup`
authority, read 64 matching bytes and refused metadata without `file.metadata`.
Plan-only and file-extraction cleanup left zero charged memory.

A healthy GPT image with 512-byte sectors containing a populated pool extracted a binary
file with identical contents; its whole-image SHA-256 was unchanged. Whole-image
checking, file-data integrity checksums, multi-extent runtime coverage and
kernel/QEMU validation are outside this task's validation.
