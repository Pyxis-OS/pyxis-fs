# FAT32 raw writer

`include/pyxis_fs/fat32_write.h` and `libfat32-write.a` add synchronous raw
creation, directory creation, append, zero extension and staged replacement.
Link the writer archive before `libfat32-read.a`. `make fat32-write` builds those
archives without host tools. The existing inspector and installer remain separate.

The trusted caller first admits GPT/geometry with `fat32_open` after obtaining
exclusive backing authority. That source, authority, callback contexts, copied
volume, writer, workspace, receipt array and disjoint receipt chain arrays remain
alive throughout the session. There is no heap, kernel ABI, mount, exclusion or
persistent ownership service in this library. Operations cannot run concurrently
or reentrantly. Payload callbacks supply stable bytes and cannot mutate the source,
writer, receipts or workspace. All argument/output storage is disjoint from the
workspace. Ending the caller's session releases its storage; no close API claims
additional durability.

## Receipts and supported operations

The caller initializes each receipt's chain pointer/capacity, supplies the array
to `fat32_writer_open`, and obtains a pointer into that array only after a durable
create. Opening a session invalidates every supplied receipt. An existing FAT
name, alias, marker or first cluster never grants update permission. Receipt
copies, receipts from other arrays/sessions, and restarted-session objects refuse.
Library-maintained receipt fields cannot be edited by the caller.

A receipt binds the session/volume, parent, exact physical LFN and short-entry
slots/bytes, original ASCII path and complete cluster chain. Before append,
extend or replacement, the whole volume is checked and the current path,
parent, entry bytes and every chain link must match. Successful updates refresh
the same receipt and its full chain. Directories receive creation receipts but
file updates on them refuse. Children can be created through their supported
ASCII paths. Directory growth can change a created directory's chain; its receipt
is not a file-update handle and does not authorize adoption of its children.

Paths are rooted ASCII, at most 1024 bytes, with components at most 255 bytes;
empty components, dot components, control characters, FAT-forbidden characters,
and trailing dots/spaces refuse. New names retain spelling using validated LFNs;
an available `P` plus seven hexadecimal digits provides an ASCII short alias.
Collision checks cover every sibling long name and alias with ASCII folding.
They refuse non-ASCII siblings in the changed directory, and refuse a parent
whose own name/alias is outside the ASCII profile. Valid Unicode objects elsewhere
remain supported by the reader. Content updates to an ASCII receipt do not
require all siblings to be ASCII. No Unicode folding is inferred.

`fat32_create` takes a directory flag, a payload callback and a byte count.
Directories require zero payload bytes. `fat32_append` adds bytes;
`fat32_extend` takes the final length and fills its new range with zeros.
`fat32_replace` stages a complete separate chain, publishes the existing object's
new first cluster/length, then releases its old chain. It needs enough free space
for the complete new chain while the old chain remains allocated. No temporary
namespace name is created. New cluster slack is zeroed. Zero-length files have
no chain. Size arithmetic enforces the FAT 32-bit length limit, caller file-size
bound and receipt chain capacity. Newly created metadata stores DOS's minimum
representable date and zero times because this interface has no clock authority;
updates preserve existing timestamp fields.

## Preflight, effects and uncertainty

Every operation revalidates BPB geometry/flags and runs the complete reachability,
allocation, chain, directory-tail and label check before effects. Dirty/error flags,
orphan/cross-linked allocation, corrupt and unsupported metadata refuse. The
caller has already reselected matching GPT under its exclusive admission; mutation
reads remain partition-confined. GPT is not rediscovered on each operation.

`fat32_write_workspace_size(volume, depth, max_file_bytes)` reserves reader storage,
a free-cluster vector and bounded complete sector images. The operation checks
namespace, allocation, input payload, receipt capacity, memory, depth, work and
sector budgets before the first write. Payload input is copied entirely during
preflight. There are no reads, allocations or input callbacks during effects.
The same finite context budget covers volume checking and all subsequent
preflight work; effects and barriers are reserved against its remainder. Large
volumes can exceed the caller's memory/work budget and refuse.

Prepared writes have these publicly named phases, each followed by a real backing
flush, including empty phases:

1. `FAT32_WRITE_DATA`: zeroed new clusters and content, including an existing
   append tail sector when needed.
2. `FAT32_WRITE_FAT`: complete new chains, append links and directory growth,
   updating every mirrored FAT copy.
3. `FAT32_WRITE_DIRECTORY`: LFN/short entries or the existing file's new chain/size.
4. `FAT32_WRITE_RECLAIM`: release the replaced old chain, after directory flush.
5. `FAT32_WRITE_FSINFO`: exact checked free count and an unknown next-free hint in
   each declared FSInfo copy, followed by the final flush.

Writes use partition-confined absolute sector addresses. FAT read/modify/write
preserves each copy's upper reserved nibble, padding, and entry-1 clean/error bits.
The raw profile never marks dirty or clears a prior flag. Success and the receipt
publish only after the final flush. This is not crash atomicity, untorn-sector
protection, rollback, or a transaction across FAT sectors/files.

Any failed write or flush may have effects and permanently stops the session.
Later operations return `FAT32_STOPPED` without reusing workspace or clearing
recovery. The caller retains `writer.recovery`: target, failed phase/absolute
sector (UINT64_MAX for a flush), old/staged starts, complete staged and allocated
cluster arrays, and public read-only prepared-sector records/count. Record
addresses are partition-relative; phase zero records are read-only preflight
snapshots. The old chain remains in the original receipt. These pointers borrow
workspace and must be captured before storage is released. No automatic cleanup,
retry or replay runs. Old append bytes and namespace/FAT state may be uncertain;
external host repair remains outside Pyxis.

## Disposable host images

`build/fat32-write --image IMAGE --gpt-partition N` opens only a regular image,
with `O_NOFOLLOW` and an exclusive nonblocking advisory lock. The operator must
exclude tools that do not honor that lock. The writer has no physical-device
interface. It uses `pread`/`pwrite` and `fsync`, preserving system error text for
backing failures. The Linux host filesystem remains responsible for its own
physical durability guarantees.

Add `--sector-size 4096` for a 4Kn image; `--sector-size 512` is the default.
For example:

```sh
build/fat32-write --image /tmp/scratch-4kn-gpt.raw --gpt-partition 1 --sector-size 4096
```

The image length must be nonzero and aligned to its selected logical sector
size, and its FAT BPB must agree. The tool never guesses GPT sector geometry
from image contents. Callback requests remain bounded to 64 KiB in bytes;
sector counts and byte offsets use the selected 512-byte or 4096-byte geometry.
Receipt chain capacity uses the actual bytes per cluster and the existing
1 MiB file bound.

Commands are read from stdin in one session:

```text
mkdir /Scratch
create /Scratch/Created file.txt = first payload
append 1 = appended bytes
extend 1 1025
replace 1 = replacement
check
list /Scratch
read /Scratch/Created file.txt
quit
```

Successful creation prints its receipt ID; append/replacement/extend use that ID,
never a name. Inline payload may contain spaces and be empty. The delimiter is
` = `; binary payload and delimiter-containing paths need a separate consumer.
The host limits are 32 receipts, 1 MiB per file, 8191 bytes per command line,
depth 64, 32 MiB workspace, 64 million steps and 4 million backing logical sectors per
operation. An oversized line ends the session before executing a truncated
command. Any refused command makes the process exit nonzero; safe later commands
may continue. Uncertain effects print recovery/held chains and end the session
immediately, preserving on-disk staging without automatic repair. The read-only
`fat32-inspect` remains the standalone byte-output inspector.

See [local qualification](fat32-write-qualification.md) for measured scope and
[reader limits](fat32-read.md) for the admitted format.
