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
  --volume home --owner 0a32efc079ed4c7bab58e224cf119315 --plan
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --owner 0a32efc079ed4c7bab58e224cf119315
build/pyxisfs-inspect --image /tmp/pool.raw info
build/pyxisfs-inspect --image /tmp/pool.raw volumes
```

Repeat `--volume NAME --owner ID` for up to 256 volumes. Each starts with an empty
root directory and an owner subtree grant covering all defined rights.
Pool, volume and root-object IDs use OS
strong randomness, with at most 16 attempts per identity to avoid zero/collisions.

`--guarantee SIZE` and `--quota SIZE` apply to the preceding volume. Global
`--cow-reserve`, `--migration-reserve` and `--recovery-reserve` override the agreed
reserve defaults, subject to their floors. Sizes are unsigned decimal bytes with
optional `KiB`, `MiB`, `GiB` or `TiB`; capacity values must be multiples of 4096.
The [format contract](format.md#accounting-and-defaults) defines the
capacity policy. The [construction layout](empty-layout.md) accounts for every
metadata block, including the allocation map itself, before creating output.

The plan reports allocations, guarantees, unused guarantees, quotas, reserves and
unpromised space in blocks. `--plan` creates no file; a real invocation also prints
its plan before creating anything. Separate invocations generate different IDs.
Output is a new sparse regular file, mode 0600 before any stricter caller umask.
Its logical size is pool capacity; host disk space is not preallocated. Host space
exhaustion remains an I/O failure. Existing output is never overwritten.

Creation traverses parent directories through retained descriptors without
following symlinks, rejects `..`, and uses exclusive creation. It holds an
exclusive advisory lock, writes metadata, flushes, writes both generation-1
superblocks, flushes the file and containing directory, then closes. Failure after
creation leaves an explicitly reported incomplete image for the owner to remove.
Readable slots alone do not establish that formatting completed successfully.

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
that payload counter. The empty planner currently reserves a bounded workspace
sized for the maximum volume count even for a small pool. It fails before file
creation if this does not fit. An inspector limit failure publishes no volume
records for a failed catalog call, though preceding selection diagnostics or
completed directory pages can already have been printed.

Exit codes are 0 for success, 1 for policy denial, 2 for invalid arguments or a
missing selector, 3 for corrupt/ambiguous media, 4 for unsupported meaning,
read-only operations, busy resources or a resource/capacity limit, and 5 for I/O or
allocation failure. An absent/corrupt peer beside a valid selected slot is a
warning for these commands; a successful operation still returns 0. Unsupported,
limit or operational failures in either candidate prevent selection. If multiple
failures exist, command aggregation prioritizes I/O/allocation, proved corruption,
then unsupported/limit, retaining both candidate diagnostics.

Source import/extraction and whole-image checking remain later tasks. These tools do not
mount, mutate, repair or establish that the recorded reserves suffice for writes.
The core supports file reads, but this CLI does not yet export file contents.

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

Populated namespace, nested acquisition and actual file-content reads await
task 5's agreed importer. Inline/tree extents, sparse holes, malformed metadata,
grant-free volumes, GPT degradation/ambiguity and I/O failures have source-review
coverage only in this slice. No custom image writer, damaged fixture, fault
injection, kernel mount or QEMU validation was introduced.
