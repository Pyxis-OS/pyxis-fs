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

Admit GPT/geometry through `fat32_open` under exclusive authority, then call
`fat32_mounted_open`. Admission revalidates geometry and the complete namespace,
allocation and chains, refusing dirty/error, corrupt and unsupported media.
Opening a fresh struct does not accept another instance's dirty mark or held
chains. Reacquisition of an existing instance is a caller policy operation;
`fat32_mounted_check` validates that existing state without reopening it.

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

## Reads and mutation profile

Locate, component lookup, one-entry enumeration and offset reads accept retained
objects. They revalidate geometry, selected metadata and complete relevant chains;
reads do not require full-volume checking. Unicode LFNs enumerate intact and an
unambiguous ASCII alias can select their content. Ranged reads clip at EOF and
may emit a prefix on a later I/O/budget/sink failure; a native adapter must stage
bytes when its protocol requires no partial publication. Enumeration retains the
reader's opaque continuation/end and success-only output rules.

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

Every mutation and detached reclamation runs whole-volume structural preflight.
Explicit detached object chains are additional roots; their complete recorded
links must still match, must not overlap namespace/other held chains, and detached
directories must remain empty. Unexplained orphan allocation still refuses.
`fat32_check` remains strictly namespace-rooted and deliberately reports allocated
held orphans as corruption; only `fat32_mounted_check` knows mount-owned roots.

All plans, complete sector images, free-cluster vectors and payload copies occupy
caller-provided aligned operation workspace. No file image is retained or copied
in full merely to perform an ordinary overwrite. Before effects the engine checks
namespace, FAT space, record capacities, finite work/sector/depth budgets and
reserves every backing write/flush. There are no reads, payload callbacks or
allocations during effects. Preflight refusal leaves media and records unchanged.

`fat32_mounted_workspace_size(volume, depth, changed_bytes)` conservatively reserves
reader storage, the volume's free-cluster vector, mirrored FAT images and changed
data/namespace sectors. `changed_bytes` covers changed gaps/slack as well as input.
For ordinary overwrite reserve the payload span plus sector edge slack. For growth,
`new_size - old_size + 2 * cluster_bytes + payload_bytes` is conservative; for
shrink one cluster is sufficient. Namespace operations use zero changed bytes:
the size calculation separately reserves new-directory and parent-growth sectors.
The operation uses only supplied bytes and returns NO_MEMORY or LIMIT before
effects when any bound is insufficient. Admission/read operations need only the
ordinary reader workspace. Reserve parent `chain_count + 2` for an insertion;
21 name/short entries plus a spare fit at most two minimum-size clusters of growth.
The operation charges every array/image through its caller; the native adapter
also accounts for held records and retained failed workspace under its aggregate
mount budgets.

The mounted phases are distinct from the raw writer phases:

1. MARK clears the owned clean-shutdown flag in every FAT copy, then flushes before
   the first mutation effect. Prepared later FAT images carry that dirty bit.
2. DATA publishes new/changed sector data and zero-filled slack, then flushes.
3. FAT publishes new chains/links and parent directory growth, then flushes.
4. DIRECTORY publishes names/lengths/locations, then flushes.
5. RECLAIM cuts/frees shrink tails or drained detached chains, then flushes.
6. FSINFO publishes checked free counts and unknown next-free hints to declared
   copies, then performs the final flush.

Each non-mark phase flushes even when empty. The raw writer still preserves all
clean/error bits. Timestamps stay preserved during mounted updates/rename; new
entries use DOS minimum dates and zero times without claiming UTC validity.

## Sync, sealing and uncertainty

Sync flushes the instance without clearing its owned dirty mark. Seal flushes,
then clears/flushed the owned mark only when no detached allocated chains remain.
Held attached objects/views and publication intervals remain caller-owned and
intact. Detached chains leave the mark dirty but do not refuse sealing/power.
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

The task-4 engine was built with ordinary `make -j16`, including all host tools
and the three FAT archives with freestanding GNU C23 warnings as errors. The
existing raw CLI was manually exercised on a disposable copy of a 64 MiB FAT
partition (512-byte clusters, two FATs): directory/file create, receipt append,
extend to 1025, replacement, read and strict inspector check succeeded. The final
check reported four files, five directories, nine allocated clusters and 129013
free clusters. This validates raw-profile integration, not mounted semantics.
Native mounted runtime, held-victim, clean-power and debugger qualification is
performed by the parent task on disposable VirtIO images; no native ESP writes
or hardware power-loss guarantees are claimed here. No new harness, fault hook,
self-test or CI workflow is added.
