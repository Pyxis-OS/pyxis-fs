# Filesystem host tools

`make -j16` builds `build/mkpyxisfs`, `build/pyxisfs-inspect`, `build/pyxisfs-write` and the freestanding
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
owner subtree grant carries the original file, directory and administration
rights; it omits the added `dir.checkpoint` bit. Feature masks remain zero.
Pool, volume and object IDs use OS strong randomness, with at most 16 attempts
per identity to avoid zero/collisions.

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
Directory indexes satisfy the private six-entry non-root occupancy profile after
byte-aware tail repair at every level. This does not establish writable
admission or change reserves and quotas; existing images are never repacked.

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

## Check images

```sh
build/pyxisfs-inspect --image /tmp/pool.raw check
```

`check` takes no volume or path selector. It performs a bounded diagnostic walk
of both candidates, reports selected and retained results separately, and checks
their shared physical storage. Output includes individual failures with available
block/volume/object context, visited record counts, claimed metadata/file blocks,
and per-state and cross-state completeness. Counts from incomplete work are
partial observations. Explicit GPT selection uses the same command.

The checker reconciles catalogs, tree ordering and separators, namespaces, object
storage and grants, physical claims, allocation ownership/incarnations, accounting
and budget charges, including ORPHANS-enabled named-or-orphan relationships in
each retained state. Orphan object/storage claims are included in the checked
counts. Six-entry namespace occupancy is a private editor precondition, not a
read-only corruption rule. Traversal and bookkeeping share the memory cap. Unknown
enabled features make the full result unsupported and incomplete, including
read-compatible features; supported independent checks continue where meaningful.
Skipped contents or exhausted resources never establish a clean image.

Success requires two valid, fully checked states and a complete cross-state
comparison. A valid state with an absent peer returns 4, and a corrupt peer
returns 3. I/O or allocation failures take priority and return 5. The command
reads metadata without reading or verifying file payloads, whose contents have
no integrity checksums. It performs no repair, reclamation or image writes.
Use extraction and ordinary host comparison to compare file contents.

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
- `dir.metadata`, `dir.list`, `dir.lookup`, `dir.create`, `dir.remove`, `dir.replace`, `dir.checkpoint`
- `admin.inspect`, `admin.grants`, `admin.owner`

The command prints the policy decision and effective rights within the ceiling.
Success requires every requested right and intervening directory lookup, then
acquires and releases the requested readonly view. Policy-approved mutation
rights report read-only and create no view. Ownership alone grants no authority.
The command does not authenticate the supplied principal; diagnostic path
selection can reveal a missing target before policy evaluation. The ordinary
core acquisition APIs enforce lookup before resolving each path component.
See [core interfaces](core.md#policy-acquisition-and-ordinary-views).
The inspector evaluates checkpoint authority but provides no checkpoint operation;
the [writer](#healthy-writer-sessions) uses acquired checkpoint views. Formatter-created
grants omit `dir.checkpoint`. Orphans have no named path or ordinary
acquisition ancestry. Core diagnostic object/read/grant APIs can inspect a
validated orphan by ID; these host commands retain their namespace path selectors.

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
catalog staging, traversal frames, node caches, proof/checker bookkeeping, live
views, directory pages, writer input buffers and GPT scratch. Fixed codec
stack frames, caller handles, argv and host allocator/libc overhead are outside
that payload counter. Source manifests grow within the same cap; their old and
new arrays are both charged during growth. The bulk planner copies the manifest
and reserves conservative node-descriptor space proportional to its object
count, plus a fixed bounded volume/work-buffer area. These simultaneous planning
allocations can exhaust the cap below the format's record-count maximum. A
formatter limit failure occurs before file creation. An inspector limit failure
publishes no volume records for a failed catalog call, though preceding selection
diagnostics or completed directory pages can already have been printed. `check`
reports partial work and incomplete states on exhaustion and releases a partial
resource-exhausted state before attempting its peer. Retained state tables share
the cap; a complete cross-state result requires both states' necessary records.

Exit codes are 0 for success, 1 for policy denial, 2 for invalid arguments or a
missing selector, 3 for corrupt/ambiguous media, 4 for unsupported meaning,
read-only operations, busy resources, a resource/capacity limit or incomplete
checking, and 5 for I/O or allocation failure. For commands other than `check`,
an absent/corrupt peer beside a valid selected slot is a warning; a successful
operation still returns 0. Unsupported, limit or operational failures in either
candidate prevent selection. If multiple
failures exist, command aggregation prioritizes I/O/allocation, proved corruption,
then unsupported/limit, retaining both candidate diagnostics.

These tools do not mount or repair images. The [healthy writer](#healthy-writer-sessions)
validates writable admission before exposing mutation. Formatter imports exercise
contiguous inline extents; the maintained [host contract suite](testing.md) records
core operation, admission and publication coverage separately from manual host runs.

## Validation

This section records the historical read-only milestone and continuation checks.
Its coverage limits describe those manual runs; current maintained-suite scope
and evidence are recorded in [testing](testing.md).

The read-only milestone closed on 2026-09-29 using merged implementation
`144710d` (merge `c379afe`). Validation used ordinary host commands and debugger
inspection on Linux 6.19.10 x86_64, with native GCC 16.2.1 and Pyxis GCC 16.2.0.
The parent `make -j16 fs-tools`, a fresh standalone native build and a fresh
cross-compiled core build passed. Relocatable linkage of the cross core had no
unresolved symbols. No compiler-container rebuild was needed.

### Final merged workflow

A fresh 128 MiB image imported quiescent snapshots of the Pyxis documentation and
kernel trees, the mixed source described below, and an empty volume. The volumes
contained 88, 104, 9 and 1 objects respectively. Separate inspector processes
reopened the image, listed volumes/directories and checked both committed states.
Each state reconciled 202 objects, 198 directory entries, 179 file extents, four
grants, 40 metadata blocks and 638 data blocks. Both slots held the same
generation-1 root and the whole-image check completed successfully. The repository
imports exercise object indexes with multiple levels.

Extraction of all three populated volumes matched their source snapshots with
ordinary recursive `diff`; extraction of the empty volume produced an empty
directory. The mixed source included Unicode/space names, nested and empty
directories, an empty file, binary files of 4097 and 476776 bytes, an 8193-byte
sparse host file, and hard-linked source names. Extracted hard-linked inputs had
different host inodes. Source holes were materialized as zero data, so this does
not establish runtime coverage for sparse filesystem extents.

The three extractions ran concurrently in this Linux workspace. `/usr/bin/time`
reported 0.29 seconds and 3140 KiB maximum RSS for documentation, 0.15 seconds and
3080 KiB for kernel sources, and 0.00 seconds at the tool's displayed precision
and 3140 KiB for the mixed volume. RSS includes host/library overhead and is not
the core's charged-memory counter. These small-input observations are neither
scaling bounds nor measurements on the owner's host.

Nested owner acquisition of `file.read` and `file.metadata` succeeded. Removing
`dir.lookup` from the ceiling or supplying an unrelated principal denied the
request with exit 1. Policy-allowed `file.write` returned read-only/exit 4 without
creating a view. A fresh 64 MiB single-empty-volume image also passed full checking
and empty-directory listing. A 64 KiB checker cap returned incomplete/exit 4;
debugger inspection observed zero charged memory at image cleanup.

### Additional measured coverage

Earlier implementation validation used the same retained contracts:

| Area | Observed behavior |
| --- | --- |
| Catalog and allocation-map depth | A 64 MiB image with 256 maximum-length volume names supplied in reverse order reopened and checked successfully; each state had 256 objects/grants and 576 metadata blocks. |
| Capacity planning | A 256 MiB two-volume image accepted explicit reserves, a zero guarantee and a two-block quota. A 1 TiB plan reported 8+8+4 GiB reserves and created no file. |
| GPT containers | Healthy 96 MiB images containing 64 MiB pools opened with 512-byte sectors at LBA 2049 (not 4 KiB aligned) and 4096-byte sectors at LBA 256. Checking succeeded for both. A populated 512-byte GPT image extracted matching file contents. Image SHA-256 values stayed unchanged during readonly inspection/extraction/checking. |
| Object and view lifetime | ID-based volume selection, root metadata/grants and directory paging succeeded. Debugger inspection observed busy closes with a live view, held-subtree file reads with matching bytes, and metadata denial without `file.metadata`. |
| Source and destination refusals | Existing output, source symlinks, source/output containment, `..` output components, zero owner IDs, impossible guarantees and insufficient quota were refused. Existing extraction output stayed untouched. |
| Limits and cleanup | Low planner, pool-open, listing and checker caps were refused. An unused GPT entry was rejected. Debugger inspection observed zero charged memory after plan-only, extraction and successful/refused checker operations. |

### Directory continuation validation

On 2026-09-30, ordinary `make -j16` with GCC 16.2.1 built the freestanding core
and host tools. The existing formatter imported `/usr/include/linux` into a
64 MiB disposable image alongside an empty volume. Both generation-1 states
passed `pyxisfs-inspect check`: 839 objects, 837 directory entries and 805 file
extents. Existing diagnostic listing and recursive extraction succeeded;
`diff -r` found no difference between the extracted headers and their source.

Interactive GDB calls exercised `pfs_view_directory_page` through an acquired
view in the ordinary `access` inspector command; no diagnostic command, test
harness or image mutation was added:

| Case | Observed result |
| --- | --- |
| Root with 604 entries and an internal directory-tree root | Pages of 300, 300 and 4 entries returned in order, ending at `zorro_ids.h`; continuation crossed leaf boundaries. |
| Independent replay | Reusing the first-entry token twice returned the same next name, `acct.h`, and next token. |
| Repeated end | Reusing the final token returned zero entries, `done=true` and the unchanged token. |
| Nested LIST-only view | The `can` directory returned eight names; a root token with excess bytes was invalid there, while a valid local slot resumed inside that directory. |
| Invalid slot paths | Missing/zero ancestor or leaf slots, out-of-range ancestor/leaf slots and excess bytes returned INVALID without changing output name, count, end flag or next token. |
| Missing LIST | A LOOKUP-only view returned DENIED with outputs unchanged. |
| Empty volume | Zero returned repeatable empty/end results; a nonzero continuation returned INVALID. |
| Budget exhaustion | Under an 8 MiB core cap, a valid 16,000-entry capacity request returned LIMIT with outputs unchanged. |
| Cleanup and abandonment | Charged core memory returned to the 1,200-byte pool/volume/view baseline after inspected calls and to zero after final close; no token close was needed. |

Runtime continuation coverage uses one- and two-level directory trees, unchanged
generation-1 images and the host adapter. Maximum-depth tokens, later generations,
rooted media/format corruption and actual I/O/allocator failures have code review
only. These checks do not measure the future kernel adapter's peak memory or
runtime limits, and do not claim that an arbitrary token records its originating
view. All debugger processes were stopped after inspection.

### Coverage limits

All runtime images have identical generation-1 roots and formatter-produced
inline extents. Multiple-extent trees, sparse filesystem holes, grant-free
volumes, differing generations, retired storage, unknown/malformed metadata,
GPT degradation/ambiguity, concurrent source changes and actual I/O/allocation/
flush failures retain source-review coverage only. No custom filesystem writer,
damaged-image fixture or fault injection was introduced.

Structural checking does not read file payloads; the extraction comparisons above
establish contents only for the exercised inputs. File-data checksums, writable
admission/recovery, FUSE and kernel mounting remain outside the implemented slice.
No QEMU validation or production-data safety claim follows from these host checks.


## Healthy writer sessions

`pyxisfs-write` exposes ordinary writable admission/reopening, checkpointing,
file and directory creation, removal, regular-file rename/replacement, writes
and resize. Supply explicit
extent and metadata limits; profile selection is separate from disk capacity and
per-transaction bounds. For example, on a healthy image formatted with sufficient
unpromised capacity and reserves:

```sh
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 open
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object OBJECT_ID --principal PRINCIPAL_ID \
  --rights file.checkpoint checkpoint
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object PARENT_ID --principal PRINCIPAL_ID \
  --rights dir.create create-file --name notes.txt
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object PARENT_ID --principal PRINCIPAL_ID \
  --rights dir.create create-directory --name drafts
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object PARENT_ID --principal PRINCIPAL_ID \
  --rights dir.remove remove --name obsolete.txt
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object SOURCE_PARENT_ID --principal PRINCIPAL_ID \
  --rights dir.remove rename --name notes.txt \
  --destination-object DESTINATION_PARENT_ID --destination-name saved.txt \
  --destination-rights dir.create
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object SOURCE_PARENT_ID --principal PRINCIPAL_ID \
  --rights dir.remove rename --name newer.txt \
  --destination-object DESTINATION_PARENT_ID --destination-name saved.txt \
  --destination-rights dir.create,dir.replace --replace
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object FILE_ID --principal PRINCIPAL_ID \
  --rights file.write,file.resize write --input /tmp/contents --offset 0
build/pyxisfs-write --image /tmp/pool.raw --extents 512 --metadata 256 \
  --volume-id VOLUME_ID --object FILE_ID --principal PRINCIPAL_ID \
  --rights file.resize resize --length 8193
```

IDs are exactly 32 hexadecimal digits. Each command acquires an object-scoped
view with exactly the requested rights. Checkpoint accepts `file.checkpoint` or
`dir.checkpoint`; both create commands accept `dir.create`; remove and the rename
source accept `dir.remove`; resize accepts `file.resize`; write accepts
`file.write` with optional `file.resize`. Rename independently acquires the
specified destination parent with exactly `--destination-rights dir.create`,
optionally including `dir.replace`, under the same volume and principal. Both
parents receive their own object-scoped trusted context and ceiling. Duplicate
rights, duplicate options and options or rights unrelated to the selected
operation are rejected. The CLI's supplied principal/object
are trusted embedding inputs, not authentication. The object ID must be obtained
under appropriate authority before the exclusive session. Existing formatter
grants have not gained `dir.checkpoint`; they retain the existing file right.
The command reports admission requirements, confirmed generation and writer
health/cleanup outcome. Successful mutation/cleanup may report maintenance
`pending` with ready health: object changes are durable, while bounded volume
retirement remains funded. `none` reports no maintenance outcome and does not
certify debt absence. Checkpoint settles pool-wide volume retirement under the held
checkpoint right, using at most two pure publications and reporting `complete`
even when none is needed. It does not clean retained orphans or widen object rights.
Pool/volume disposal performs no checkpoint; checkpoint before releasing its view
when settled reclamation is required.

`create-file` and `create-directory` take one UTF-8 component of 1–255 bytes,
excluding `/`, `.` and `..`. They create an empty object owned under the parent's
creation policy, with no new grants. The commands request no returned child
handle or child authority; obtain its new ID through a subsequent authorized
lookup or diagnostic listing. Creation authority alone does not grant access to
the new object.

`remove` takes the held parent ID and one name component. It removes a regular
file or empty directory; a nonempty directory returns `not-empty`. The core
records the victim as a persistent orphan in the same transaction and cleans it
when no runtime references remain. Existing held views in an embedding retain
their identity, contents and rights; a removed directory stays empty and rejects
insertion. These single-operation host commands retain only the parent views,
so their unheld victims are cleaned after confirmed namespace publication.

`rename` takes source-parent `--object` and `--name`, plus explicit
`--destination-object`, `--destination-name` and `--destination-rights`. Source
and destination names are single components. Regular-file rename preserves the
source object's identity and contents; directory moves/replacement and
cross-volume moves are unsupported. The optional boolean flag `--replace`
defaults to false. A distinct existing file destination returns `exists` unless
`--replace` is supplied, even when destination authority includes `dir.replace`.
Displacing that file also requires `dir.replace`; an absent destination needs
only `dir.create`. A validated same-parent/same-name operation succeeds without
publication or replacement authority, reporting `namespace-confirmed=false`.
Replacement retains and cleans the displaced object through the same orphan
lifecycle as removal.

`--offset` and `--length` are unsigned decimal byte counts. Resize growth exposes
zeros, and shrinking then regrowing does not expose discarded bytes. Write input
must be a quiescent regular host file, with no final symlink, at most 16 MiB; image
aliases and hard links to the image are refused. The command takes a shared
advisory input lock, reads the entire input into a buffer charged to the memory
cap, checks its size/mtime/ctime before mutation, and closes it. Cooperating input
writers must honor that lock; these checks do not establish an atomic source
snapshot against a noncooperating writer.

The 16 MiB cap is a host command input bound, separate from the core's file-size
and transaction limits. The command submits one core write request for the whole
buffer. This preserves bounded multi-block planning and the core's upfront range
authority check: an extending request without `file.resize` is denied before
changing even its in-range prefix. Large requests can still span committed
transactions; whole-write and whole-shrink atomicity are not promised. Splitting
a larger host input into separate command invocations creates separate requests
and authority checks, rather than one aggregate atomic operation.

Operation output includes completion, original operation status, confirmed byte
prefix, last confirmed length when available, namespace confirmation, writer
health and separate maintenance outcome/status. Consume that output even on a
nonzero exit: an error may follow confirmed progress, and an unknown outcome may
have committed beyond the confirmed prefix or length. A close error fails the
command without erasing earlier confirmed progress. When close reports cleanup
or failure, its output includes whether the handle was consumed (`released`),
writer health and maintenance outcome/status. Both rename parent views are
closed even when acquisition or mutation fails. No automatic retry follows an
error or unknown outcome.

This adapter supports buffered I/O to a Linux local regular file, with exclusive
advisory locking for the session, exact transfers and explicit `fsync`. It does
not support writer access through GPT selection, raw devices or network files.
All access must cooperate with the lock; other processes, mappings or devices
must not modify the image. The host filesystem/device must honor the ordinary
`fsync` durability contract. A single failed/partial write is not retried;
flush/close errors are reported. Logical reserve admission does not reserve
physical host space for the sparse image.

Supported starting points have established healthy backing history, such as
successful fresh formatting or a previously successful healthy writer session.
After actual/suspected writeback failure, or an interrupted mutating session whose
healthy history cannot be established, do not use ordinary reopen as recovery.
The restriction also covers interrupted-session orphan recovery. Cached reads,
close/reopen and a later successful `fsync` cannot certify previously failed
writes. This is an adapter/operator precondition; the program has no automatic
cross-process history detection, persistent registry, special XFS requirement or
force-clear flag. Qualification of a concrete real-host durable recovery boundary
is deferred. There is no supported recovery command for that case.

Within an instance, pre-slot write/flush failure stops mutation while permitting
proved confirmed-state reads. Uncertain slot publication stops all ordinary
access. Any backing read failure during ordinary operations, publication planning
or maintenance also stops all ordinary access pool-wide, even if transient. A
fresh validated reopen must meet the backing-history preconditions above; the
failed instance cannot retry itself healthy. Cleanup failure cannot erase already
confirmed user progress. An unknown outcome may include additional committed
bytes and is not automatically retryable. In a healthy writer, last-reference
closure finishes admitted orphan deletion and may report pending retirement.
Named-object close reports no cleanup outcome. Closure performs no recovery or retry
in a stopped writer. Simulator results are core protocol evidence only, not
qualification of Linux post-error recovery.

### File command validation

On 2026-10-01, the integrated file commands were exercised on Linux 6.19.10
x86_64 with GCC 16.2.1, using a freshly formatted 256 MiB sparse regular image,
E=512, M=256 and a 4 MiB recovery reserve. The default 1 MiB recovery reserve
was refused at opening because this profile required 594 blocks; that refusal
left both generation-1 states unchanged.

An object-scoped `dir.create` view created an empty regular file. An extending
262177-byte write at offset 8193 with only `file.write` returned denied and zero
confirmed bytes. Requesting `file.resize` as well completed the write; extraction
matched an independent host file containing the zero-filled gap and input bytes.
Shrinking to 8209 retained the first sixteen input bytes, and regrowing to 32769
matched an independent expected file with a zero-filled discarded tail.

A hard link to the image as input returned invalid, a 16 MiB plus one-byte input
returned limit, and an exclusive advisory lock on input returned busy. Those
refusals left the selected generation unchanged. The final check completed both
retained generations 22/21 and their cross-state comparison, each with one file
extent and one data block. These are healthy host command observations; they
provide no real-host writeback-failure or power-loss recovery qualification.


### Namespace command validation

On Linux 6.19.10 with native GCC 16.2.1, a fresh 64 MiB source-populated image
(E=512/M=256, 4 MiB recovery reserve) passed directory/file creation, removal and
regular-file rename/replacement. Holding `dir.replace` without `--replace` refused
an existing destination without changing its generation or expected victim bytes.
Explicit replacement preserved the source identity and independently supplied
4391-byte contents. A 255-byte destination name succeeded; same-name rename
reported `namespace_confirmed=false` without changing generation. Nonempty
directory removal refused without publication. Removing empty files/directories
returned a ready writer. Final checking covered both retained generations and
cross-state consistency with no orphans; full extraction matched the independently
defined final tree. Catalog diagnostics recognized the retained ORPHANS feature.
These are ordinary healthy-session observations, not interrupted-session or
post-writeback-error recovery qualification.
