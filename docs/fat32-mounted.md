# FAT32 mounted engine

`pyxis_fs/fat32_mounted.h` and `libfat32-mounted.a` supply retained objects,
ordinary offset writes, resize, empty-file/directory creation, removal and
same-volume regular-file rename. Link the mounted archive before
`libfat32-read.a`; the mounted archive does not require the raw-writer archive.
`make fat32-mounted` builds both required archives. The existing
[reader](fat32-read.md), [raw receipts](fat32-write.md) and host tools keep their
separate interfaces and behavior.

The caller exclusively owns the selected extent, serializes all operations and
keeps source/I/O callbacks, mount state, object records and backing alive. There
is no heap, kernel ABI, ID/refcount allocator, capability authority, scheduling,
publication lease, mount registry or retirement controller here. These belong
to the native adapter. Caller data pointers must remain stable and all argument,
object, chain, output and workspace storage must be disjoint. Callbacks cannot
mutate or reenter the engine. The library confines filesystem I/O to the admitted
partition and never infers existing-file authority from raw creation receipts.

## Admission and retained objects

Admit GPT/geometry through `fat32_open` for strict mirror validation or
`fat32_open_mounted_view` for native FAT1 authority, then call
`fat32_mounted_open` with a fresh zero-initialized mount record, separate allocation
backing and full-check workspace. `fat32_mounted_allocation_size` gives the retained
bitmap size. Admission performs one complete mounted-volume check, refusing dirty,
error, corrupt and unsupported media, and builds the exact free bitmap/count.
FSInfo is a hint, never allocation authority. Full checks use FAT windows of up to 128
512-byte sectors per copy, reduced to the callback's read limit, with at most
128 KiB of additional caller-owned scratch. Local mutations keep eight-sector
windows; valid entries are compared before use under the selected mirror policy.

The mounted-view opener borrows a fresh zero-initialized `fat32_mirror_state`.
It validates FAT1's own geometry, allocation headers and flags; a stale or invalid
FAT2 value never replaces FAT1. FAT2 backing reads must still succeed. Geometry
may set `mismatch_seen` before bitmap binding. After open, obtain the byte count
with `fat32_mirror_size`, allocate separate zeroed backing, and call
`fat32_mirror_bind` before any reader, CHECK or mounted operation. One-FAT views
need zero bytes and accept a NULL bitmap. The bitmap has one bit per sector
containing valid FAT entries, including entries zero and one, independent of
unused overprovisioned FAT sectors. The library owns its fields; every copied
volume borrows the same state and backing for subsequent engine operations.

Admission/CHECK fills pending bits while validating all FAT1 allocation and
reachability. `fat32_mirror_pending` and successful CHECK's
`mirror_mismatch_sectors` report the retained count. Comparison ignores reserved
upper nibbles and entries outside the volume's valid range. A failed scan may
conservatively add pending bits, but cannot clear them. `mismatch_seen` stays
true after successful repair so the adapter can log at most once per mount.
No logging, authority or callback is added here. Mounted RO views never repair;
the standalone inspector and raw writer remain strict, and the raw writer
rejects mounted-view volumes.

The retained allocation map borrows caller storage for the mount lifetime. Confirmed
mutation/reclamation transitions update it only after all ordered phases flush.
Detached held chains remain allocated through durable final-reference reclamation.
The caller excludes every other media writer throughout this lifetime. Opening a
fresh record does not accept another instance's dirty mark or held chains, and
opening an existing or stopped record never resets it.

`fat32_mounted_check(mounted, allocation, allocation_bytes, workspace, report)`
explicitly revalidates the complete namespace, allocation, held objects and detached
roots using the mount's current owned dirty snapshot. It needs full-check workspace
from `fat32_workspace_size` and caller-owned allocation backing disjoint from the
workspace. CHECK converts its reachability scratch into free bits after each
cluster's final validation, then copies the completed map only after held-object
validation also succeeds. The destination may be the retained map; no third
bitmap is required. Success publishes map/count and restores valid proof.
Backing/capacity refusal before validation preserves existing proof; any failure
after validation starts leaves proof invalid and allocation bytes unchanged.
Recovery backing remains retained. CHECK never
writes media, repairs damage or resets a stopped writer. Native CHECK authority,
publication intervals and leases belong to the adapter.

`fat32_mounted_root` selects the root entry and authoritative root locator using
local mounted validation. It checks geometry, root chain and directory, compares
any canonical held root record, and publishes both outputs only on success.
Detected backing/structure errors invalidate the mounted allocation proof; this
selector neither binds another object nor performs full CHECK. RW adapters use
this selector; the raw `fat32_root` reader retains its separate interface.

Initialize a caller-embedded `fat32_mounted_object` to zero, set its chain pointer
and capacity, then bind a library-produced locator. `object_chain_size` validates
and determines the exact number of clusters before allocating backing. Bind
collects the complete chain and rejects another attached record at that locator.
All aliases and repeated lookups must reuse that canonical record. Empty files
have distinct records/locators despite having no chain. The adapter supplies
genuine mount/object IDs and keeps them independent of reusable FAT slots or
cluster numbers. The engine never fabricates IDs, UTC time or Unix permissions.

The engine maintains the linked object list, cached entry, current locator and
attached state. Kernel wrappers can embed records but must not edit/copy library
fields. Before growth, `object_reserve` copies the current chain into a new
caller-owned disjoint array and switches the record; only then may the caller
release the old array. No allocation occurs in the helper. After successful
mutations the engine updates records and any grown parent chain. The adapter
updates directory generations and publishes its coherent native result only
after success.

Unlink detaches the existing record. Replacement detaches a distinct destination
victim while the source record/identity moves to its new locator. Detached FILE
reads, writes and resize use the retained entry/chain, never the reused old slot.
Removing a directory requires no ordinary children; its retained directory view
still validates/enumerates its dot-only media, and all namespace mutations through
it refuse. Root removal is impossible. Rename supports regular files only.
Self/alias rename is a successful no-op retaining current spelling.

`object_forget` is for the final caller reference only. Attached records unlink
from memory without media changes. A detached allocated chain is freed with
ordered durable reclamation before its record unlinks. Any failure leaves the
record/chain retained. Final close is not a sync or retirement promise. The caller
must keep a detached record alive until successful reclamation and keep every
record alive after terminal uncertainty.
Pending mirror repair refuses detached reclamation with `FAT32_READ_ONLY`,
retaining the record. Forgetting and background/retirement cleanup never repair.

## Reads and mutation profile

Locate, component lookup, one-entry enumeration and offset reads accept retained
objects. They revalidate geometry, selected metadata and complete relevant chains;
reads do not require full-volume checking. Unicode LFNs enumerate intact and an
unambiguous ASCII alias can select their content. Ranged reads clip at EOF and
may emit a prefix on a later I/O/budget/sink failure; a native adapter must stage
bytes when its protocol requires no partial publication. Enumeration retains the
reader's opaque continuation/end and success-only output rules.

Regular-file locate and range reads borrow the full-check FAT windows when the
supplied workspace already contains the complete full-check allocation. Windows
remain bounded by 128 sectors per copy and the source callback limit; smaller
workspaces retain the existing local windows. Geometry, selected metadata and
complete relevant chains are still revalidated on every operation. Directories
and mutations retain their existing eight-sector windows. No proof or media bytes
survive between read operations, and no allocation, workspace growth or ownership
bitmap clearing is added.

Range reads batch only physically consecutive payload sectors, including
consecutive clusters, through the existing 4-KiB data buffer. Partition bounds,
callback limits, EOF clipping, operation budgets and prefix-on-failure rules still
apply. The engine still validates the whole file chain for each bounded read, so
sequential small reads remain proportional to file size times request count.

Content mutation requires an ASCII name and alias but permits unrelated valid
Unicode siblings. Namespace edits require every ordinary sibling name/alias in
both changed directories to fit ASCII folding. New components preserve spelling
with LFNs and collision-free `P` plus seven hexadecimal-digit short aliases.
Dot names, forbidden FAT characters, trailing dots/spaces and overlong components
refuse. Read-only regular-file attributes refuse write, resize, removal and
rename/replacement; directory attribute bits are not authority. No attribute
mutation is supplied. `FAT32_READ_ONLY` and `FAT32_NOT_EMPTY` distinguish these
format-policy refusals from corruption and capacity failure.

New regular files start empty. Newly created directories get real dot entries.
Writes accept at most 4080 caller bytes and enforce FAT's 32-bit file-length
limit. Overwrites preserve untouched bytes. Growth zeroes old slack and all newly
allocated clusters, including beyond-EOF gaps and new slack. Shrink zeroes the
retained tail slack, durably publishes the smaller directory length, then cuts
and frees the discarded tail. Regrowth cannot expose discarded content. A
zero-length write validates state, attributes, names and the selected object but
does not grow, dirty or publish metadata.

Rename checks the exact canonical victim supplied by the caller. With no current
destination the victim must be NULL; with a distinct destination REPLACE and its
retained record are required. Both source and victim stay held throughout the
operation, even if no public handle currently names the victim. Prepared namespace
images can reuse deleted source/victim slots without reusing their identities.
There is no directory move, cross-volume move or crash-atomic rename.

## Finite preparation and effects

Mutations and detached reclamation require valid retained allocation proof and
check the affected complete chains, parent directories, names and held records.
Attached file updates also revalidate a canonical held parent when present. Locally
traversed chains cannot contain clusters recorded free; new candidates are checked
against their FAT entries before effects. Searches visit at most the volume's
cluster count and consume the finite step budget. Local chain loop detection uses
constant storage, without a whole-volume bitmap clear per held object. Detached
directories must remain empty before reads or reclamation.

Local checks do not reprove allocation ownership across unrelated objects. Silent
in-session corruption, including a coherent cross-link to an unrelated allocated
chain, may be detected only by explicit full CHECK or remount. A full mounted check
includes detached chains as additional roots, rejects overlaps and unexplained
orphans, and compares all held records. `fat32_check` remains namespace-rooted and
reports allocated held orphans as corruption.

Backing read errors, detected structural inconsistency and uncertain outcomes
invalidate allocation proof. Ordinary authority, capacity, name collision/missing
name and no-space refusals before effects preserve it. Invalid proof prohibits
mutation, detached reclamation and clean-mark clearing. Reads remain available
unless the writer is stopped. Known pre-effect invalidation can recover only through
successful explicit full CHECK; neither sync nor another operation silently checks
or rebuilds it.

Plans, operation-sized allocation candidates, distinct FAT changes and complete
sector-and-phase images occupy caller-provided aligned workspace. No volume-sized
free vector or conservative image of every FAT sector is reserved for a tiny write.
Before effects the engine checks namespace, FAT space, record capacities and finite
work/sector budgets, and reserves every backing write/flush. No payload
callbacks or allocations occur during effects. Pending mirror repair alone uses
bounded streamed backing reads during effects, as described below. Preflight refusal leaves media and
object records unchanged, although detected corruption invalidates proof.

`fat32_mounted_workspace_size(volume, changed_bytes, allocated_clusters,
changed_clusters)` reserves local context, candidate and FAT-change arrays, mirrored
FAT images and changed data/namespace sectors. `changed_bytes` covers content images,
including gaps, cluster slack and sector edges. An overwrite may reserve the payload
span plus sector edge slack; growth may conservatively reserve
`new_size - old_size + 2 * cluster_bytes + payload_bytes`; shrink needs one cluster.
Namespace operations use zero changed bytes because the helper separately includes
new-directory and parent-growth images. Required candidate/change bounds are:

| Operation | allocated_clusters | changed_clusters |
| --- | --- | --- |
| Write or resize | added clusters | added + removed + 1 |
| Create | 3 | 4 |
| Rename | 2 | 3 |
| Remove | 0 | 0 |
| Detached final forget | 0 | retained chain count |
| Seal | 0 | 0 |

Create/rename reserve their maximum parent-growth needs even when the current parent
has spare slots. Reserve parent `chain_count + 2` before insertion; 21 name/short
entries plus a spare need at most two minimum-size clusters of growth. Insufficient
workspace returns NO_MEMORY or LIMIT before effects. Mounted read operations can
use the zero-change mounted workspace; admission/CHECK require the full checker
bitmap and traversal frames instead. Keep a nonzero directory depth limit for
locator operations and enough traversal depth for full CHECK.

The caller charges allocation backing, operation workspace, held records/chains and
retained failed workspace under its aggregate mount budget. Allocation backing stays
separate from replaceable operation workspace. Reserve and charge old/new overlap
before replacing either; failed allocation preserves existing storage. Size helpers
calculate requirements without allocating or imposing an independent budget.

The mounted phases are distinct from the raw writer phases:

1. MARK clears the owned clean-shutdown flag in every FAT copy, then flushes before
   the first mutation effect. Prepared later FAT images carry that dirty bit.
2. DATA publishes new/changed sector data and zero-filled slack, then flushes.
3. FAT publishes new chains/links and parent directory growth, then flushes.
4. DIRECTORY publishes names/lengths/locations, then flushes.
5. RECLAIM cuts/frees shrink tails or drained detached chains, then flushes.
6. FSINFO publishes checked free counts and unknown next-free hints to declared
   copies, then performs the final flush.

For a mounted view with pending mismatch, only an effective write, resize,
create, remove or rename may enter repair after all ordinary validation and
preparation succeed. MARK writes FAT1 alone and flushes before repair. This can
add sector zero to pending repair because FAT2 still has the old clean bit.
An already owned durable dirty mark needs no new MARK. The appended MIRROR
phase reads each pending FAT1/FAT2 sector through retained context buffers,
writes FAT1's complete sector to FAT2 only when valid entries differ, then
flushes before DATA or any other user effect. Ordinary prepared FAT2 images
derive from the corresponding primary image, including earlier phase changes,
so they cannot restore stale secondary entries. Healthy volumes keep the
ordinary MARK path and do not scan for repair on each mutation.

Preflight reserves the repair bitmap traversal, bounded comparisons, reads,
writes and flush in the operation budgets. Pending bits clear together only
after the repair flush succeeds. A failed write/flush, or conservatively any
post-MARK repair read failure, stops the writer and retains pending bits,
prepared images and the streamed buffer in `recovery.mirror_image`. Failed
reads can leave that buffer partial. `recovery.phase` is MIRROR, and the failed
sector is absolute; a flush failure uses UINT64_MAX. Later refused calls never
overwrite this backing. No retry or rollback is supplied.

Each non-mark phase flushes even when empty. The raw writer still preserves all
clean/error bits. Timestamps stay preserved during mounted updates/rename; new
entries use DOS minimum dates and zero times without claiming UTC validity.

## Sync, sealing and uncertainty

Sync flushes the instance without clearing its owned dirty mark. Seal flushes,
then clears/flushes the owned mark only when allocation proof is valid and no
detached allocated chains or pending mirror repairs remain.
Held attached objects/views and publication intervals remain caller-owned and
intact. Detached chains or invalid proof leave the mark dirty after flush and do not
by themselves refuse sealing/power. Unresolved mutation failure still refuses.
After a successful clean clear, the copied `mounted.volume` snapshot reflects it;
first resumed mutation durably marks dirty again. Always use that current volume
snapshot with subsequent reader operations, not the original admission copy.
Normal retirement must first enforce caller reference/interval/deferred-work
drainage and reclaim detached records, then seal before releasing backing.
Pre-existing dirty/error bits are never admitted or cleared.

Any unconfirmed write or flush returns IO, sets `stopped` permanently and freezes
all data/check/bind/rebind/forget/sync/seal operations. The first recovery record
retains phase, absolute failed sector (UINT64_MAX for flush), source/victim and
target-parent records, affected component names, complete prepared sector images
and the allocated-chain vector. These pointers borrow failed operation workspace:
the caller must retain it unchanged, along with records/chains and extent
exclusion. There is no automatic replay, rollback, repair or reopening route.
Observation/recovery diagnostics read the retained struct directly. FAT does not
promise untorn sectors, atomic namespace publication across power loss or a
multi-call transaction; interrupted operations may need external host repair.

## Validation

The retained-map change and its three local-validation corrections were built with
ordinary `make -j16`, including host tools and all FAT archives using freestanding
GNU C23 and warnings as errors. Manual Linux host qualification used existing
ignored disposable-image machinery, the owner-produced FAT fixtures, 512-byte
sectors, two mirrored FATs, callback reads of up to eight sectors, full-check depth
16 and step/sector limits of one billion. Three fresh mutation sequences passed
for each geometry:

| Partition / sectors per cluster | Admission read requests | Empty CREATE read requests | Tiny write read requests | Admission milliseconds, min–max |
| --- | --- | --- | --- | --- |
| 40 MiB / 1 | 163 | 14 | 13 | 1.700–2.019 |
| 128 MiB / 1 | 511 | 14 | 13 | 3.293–4.505 |
| 512 MiB / 1 | 2023 | 14 | 13 | 8.466–11.766 |
| 1 GiB / 1 | 4039 | 14 | 13 | 16.197–17.242 |
| 512 MiB / 8 | 276 | 45 | 37 | 1.854–1.906 |
| 1 GiB / 8 | 532 | 45 | 37 | 3.170–5.999 |

Dense 1-GiB admission retained a 258107-byte free map and used 271664 bytes of
full-check workspace; the manual operation workspace was 90784 bytes for every
one-sector geometry. Host timing includes cached Linux image I/O and shared-host
variation; it is not a native or nested-QEMU storage rate or a speedup comparison.
The original exact-main baseline and parent runtime qualification live in the
Pyxis OS preflight-cost investigation.

Each sequence exercised create, payload/gap writes, zero extension, shrink,
held-victim replacement, full CHECK with detached roots, detached file writes,
durable final-reference reclamation, sealing, resumed mutation and final CHECK.
Extracted final partitions passed `fsck.fat -n` and `mdir`; independent `mtype`
confirmed the final `Beta.txt` bytes were exactly `new`.

Additional existing manual modes confirmed pre-effect truncated backing and
unsupported geometry/touched-chain corruption invalidate proof without stopping
the writer, and restored media permit explicit rebuild. An untouched orphan was
accepted by unrelated local creation and refused by full CHECK. Restored final
images passed fsck and mtools. On a private 256-KiB tmpfs, real ENOSPC stopped the
writer during DATA after 56 completed writes, retained 16 prepared records and
eight candidate clusters, and invalidated proof. CHECK, write, seal, final forget,
bind and reopen subsequently refused STOPPED without I/O or changing the retained
recovery/workspace.

Mounted-root follow-up qualification covered unbound and held root selection on
40-MiB and dense 1-GiB images, with canonical root outputs and unchanged raw RO
helpers. Corrupt root allocation, truncated backing and a mismatching held root
chain refused without writes and invalidated proof; failures preserved both output
records. Restoring media and explicit CHECK restored root selection, while reopen
could not reset invalid proof. Real ENOSPC also confirmed mounted root refuses
STOPPED without touching recovery or failed workspace. Final restored/regular
partitions passed fsck and mtools; these logs and source hashes use the
`root-api-*` prefix under `build/qualification/`.

The review reproductions now refuse before effects: a parent's extension into a
map-free cluster returns CORRUPT; extension into an already allocated cluster
returns STALE when its held chain disagrees; a detached directory containing an
ordinary child returns CORRUPT and stays retained. Restoring the crafted media,
explicitly checking, reclaiming and sealing produced fsck/mtools-clean images.
Allocation-destination and full-check-workspace capacity refusals preserve valid proof
and perform no I/O.

For these manual runs, sources, archives and logs remain under the ignored
`build/qualification/` directory (`matrix-fixed-results.json`, `*-fixed-*.log`,
`review-*.log`, `fixed-source-sha256.json`). No tracked harness, fixture generator,
fault hook, self-test or CI workflow was added. CI establishes builds for its
submitted revision only. The native adapter, CHECK authority/helper, publisher
lease and interactive QEMU qualification are parent integration work; no native
ESP writes or hardware power-loss guarantees are claimed here.
