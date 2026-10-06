# npfs host tools

`make -j16` builds the format-only `build/libnpfs-format.a` and Linux tools
`mkfs.npfs`, `fsck.npfs`, `npfs-inspect`. From Pyxis use `make -j16 fs-tools`
(`build/fs-tools/`). No compiler-container rebuild is needed. See the
[encoding contract](npfs-format.md).
The optional libfuse3-dependent `npfs-fuse` tool is described below.
The existing filesystem CI job builds the outputs available in its builder. Structural/recovery
behavior is checked through ordinary manual tool use, not by the retired COW
suite; successful compilation is not a behavior proof.

## Formatting

```sh
build/mkfs.npfs --image /tmp/npfs.raw --size 256MiB --journal 8MiB \
  --volume home --source /path/to/unchanging/source --volume scratch
build/fsck.npfs --image /tmp/npfs.raw
build/npfs-inspect --image /tmp/npfs.raw info
build/npfs-inspect --image /tmp/npfs.raw volumes
build/npfs-inspect --image /tmp/npfs.raw list --volume home
build/npfs-inspect --image /tmp/npfs.raw stat --volume home --path file
build/npfs-inspect --image /tmp/npfs.raw cat --volume home --path file
build/npfs-inspect --image /tmp/npfs.raw extract \
  --volume home --path file --output /tmp/extracted-file
```

Size and journal are explicit and block-aligned. Unsuffixed values are bytes;
KiB/MiB/GiB/TiB are binary and KB/MB/GB/TB decimal. Choose at least 128MiB journal
for the 256 GB target. Smaller images choose a size appropriate to their space.
This formatter constructs a complete initial pool, not a journaled runtime writer.
It makes no claim about a future writer's minimum admission capacity.

Repeat `--volume NAME [--source DIRECTORY]` up to 64 times. IDs come from host
randomness. No owner/principal option exists: capability authority belongs to Pyxis.
Without a source, a volume has an empty root. Inode creation times use format time;
source modification times are preserved with saturation to the format's limits.
If format time is unavailable, new affected timestamps are explicitly unknown.

Output must be a new regular file: exclusive creation, no overwrite or device
formatting. It is sparse but imports allocate every file block, including source
holes. Data and metadata are flushed before success. Failed formatting removes
its own incomplete image when that pathname still refers to its created inode.
Sources must remain unchanged. Traversal uses descriptor-relative no-follow opens
and before/after identity, size and timestamp checks; these are best-effort race
checks, not a host snapshot. Directories and regular files only; symlinks, special
files and invalid UTF-8 components fail. Hard-linked regular files are copied
independently. Directory ordering follows host traversal; metadata is deterministic
only apart from that ordering, random identities and creation times.

Traversal is iterative, holding a directory descriptor and block buffer per depth;
host descriptor/memory exhaustion can stop it. File transfer uses bounded 64 KiB
buffers; allocated mappings live on disk. There is no whole-tree buffer or imposed
format depth limit.

## Checking and replay

`fsck.npfs --image PATH` opens read-only, refuses COMMITTED journals,
and reconciles every allocated block with fixed regions and inode-file, indirect,
directory and file ownership. It checks dense metadata, unique block ownership,
bitmap padding, root/slot zero, names, namespace references, parent backlinks,
cycles and exact persistent cleanup membership. Regular-file holes are permitted.
Detached directory cleanup may have holes in its remaining mapping.
Pending cleanup can be structurally valid; fsck does not reclaim it.

`fsck.npfs --image PATH --replay` opens writable and permits COMMITTED.
It validates the entire staged journal before home writes, checkpoints and clears
it durably, then runs structural checking. An EMPTY pool receives no replay writes.
Fsck warns on stderr when only one header is valid, including on a writable
opening. A structurally valid pool still exits zero with that warning, following
the one-valid-copy opening rule. No arbitrary repair, rollback, header-copy repair
or content verification exists.
A structural error after replay remains an error, even though recovery writes have
already happened. Replay memory holds the full payload, a target bitset and
selected descriptors; checking holds a pool ownership bitset and decoded inode/
reference state for one volume, plus names for one directory. Very large/corrupt
images can exhaust host memory and fail, rather than evade checking.

## Inspection and image authority

Inspector paths are relative to a selected volume; omitted path or `.` selects
its root. An absolute `--path` is rejected with `paths are relative to the volume
root`; use `--path directory/file` rather than `--path /directory/file`.
Empty components, leading/trailing slash and `..` are invalid. `extract` copies
one regular file into a new output, without replacement; it does not extract
a whole directory. Names print with byte escapes. `stat` prints signed nanoseconds
or `unknown`, including cleanup fields. Inspector verifies the records traversed
and rejects duplicate lookup names/backlink errors; it does not run whole-pool
fsck on each command. Run fsck separately for global consistency.

`mkfs.npfs`, `fsck.npfs` and `npfs-inspect` use standalone regular pool image
files, not GPT selection or raw disks. The read-only mount also accepts partition
devices, through a separate opening interface.
Open validates both headers, features and control selection before reading homes.
Shared read/exclusive write nonblocking `flock` coordinates cooperating tools.
Files must remain unchanged/exclusive externally: locks do not constrain programs
that ignore them. No read-only RAM recovery overlay is provided. Unknown required
features refuse all use; unknown read-only-compatible features prevent replay.
The private host read APIs may partially fill buffers on failure; callers discard
those results. The public format decoders publish only validated copied results.

## Read-only Linux mounts

Install `pkg-config` and the libfuse3 development package (`fuse3-devel` on
Fedora, `libfuse3-dev` on Debian). `make -j16` includes `build/npfs-fuse` when
that dependency is available; `make npfs-fuse` explicitly requires it. Other
outputs remain available without libfuse3.

```sh
mkdir /tmp/pyxis-volume
build/npfs-fuse /path/to/pool.raw /tmp/pyxis-volume
ls /tmp/pyxis-volume/system
cp /tmp/pyxis-volume/system/file /path/to/checkout/
fusermount3 -u /tmp/pyxis-volume
```

SOURCE can also be a readable npfs partition device, such as `/dev/sdb2`, with
no GPT selection. Supply the actual partition, not the whole disk. Symlink
sources are refused; resolve a device alias first. The source is opened read-only
and must remain unchanged for the entire mount. Regular images take the same
shared nonblocking `flock` as inspection. Devices do not have a writer-exclusion
protocol: do not mount while Pyxis or another program can change that pool.
`-f` keeps the daemon in the foreground. Normal unmount releases the source and
image lock. The mount is private to its mounting user, owns entries with that
user's UID/GID, and presents directories as 0555 and files as 0444. No write
operations or `allow_other` option are supplied.

Every live volume appears under the synthetic root with its native name.
The host opener validates both headers, features and selected control state.
Catalog, reserved inode zero and attached directory roots are checked before
mounting. A committed journal refuses the mount before reading home metadata:
boot Pyxis once to recover it, or run `fsck.npfs --image COPY --replay` on a copy
of the image. The mount performs no replay, repair or source writes.

Native creation and modification times are readable xattrs
`user.npfs.created_ns` and `user.npfs.modified_ns`: decimal signed nanoseconds
since the Unix epoch, or the literal `unknown`. Known zero is distinct from
unknown. Linux mtime uses native modification time with nanosecond precision;
unknown mtime has a documented zero fallback. Linux atime/ctime also use that
mtime, since npfs has no access/POSIX change timestamps. Creation time is only
exposed through its xattr. The synthetic root has unknown timestamps.
Use `getfattr -d FILE` to display native times. No timestamps change on reads.

File reads support sparse holes, partial reads and EOF. Namespace operations
validate traversed records and directory backlinks. Opening a directory builds
one sorted metadata snapshot, rejects duplicate names and holds it until close;
array-index cookies support resumed listings. Memory use scales with open
directories, not file contents. Callbacks are serialized because the host reader
owns a shared diagnostic buffer. Linux may cache unchanged file data.
The mount is not whole-pool fsck: global allocation ownership, duplicate inode
references and disconnected cycles still require `fsck.npfs`. Allocation-block
counts are not synthesized from logical size, so `du` is not a disk-usage report.

## Task-2 validation

On 2026-10-03, ordinary `make -j16` built both archives and both tool families
with host GCC 16.2.1. Pyxis GCC 16.2.0 compiled the new freestanding archive;
`nm -u` showed only internal format references and its two memory-provider
symbols, without libc dependencies. GDB confirmed CRC32C of `123456789` is
`0xe3069283`, timestamp saturation at both signed limits and negative fractional
conversion, plus double/triple mapping boundaries and rejection beyond the limit.

A 128 MiB sparse pool with a 1 MiB journal imported this repository's `include`
tree and the eight existing Go tool binaries from
`/usr/lib/golang/pkg/tool/linux_amd64`, plus an empty volume. Formatting took
0.20 s wall time, 0.12 s system time and 1,724 KiB peak RSS. Structural checking
passed; extraction of the then-named `native.h` and the 25,779,081-byte `compile` binary matched
sources with `cmp`. Binary extraction took 0.01 s. GDB observed logical block
524 using the double-indirect root. An actual Go bin-tree import containing a
symlink failed and removed its incomplete output.

At the formatter's final flush, GDB read the inferior's `/proc/PID/io` before
stdout output. Successful write syscall bytes (`wchar`) and calls (`syscw`), and
host allocated image bytes (`st_blocks * 512`), were:

| Source, same 128 MiB pool / 1 MiB journal | Write bytes | Calls | Allocated image bytes |
| --- | ---: | ---: | ---: |
| Include tree | 196,608 | 41 | 143,360 |
| One empty volume | 57,344 | 14 | 57,344 |

The combined image allocated 67,801,088 bytes; its syscall bytes were not measured.
All three had 134,217,728 apparent bytes. These are initial host-tool observations
on tmpfs, not distributions, kernel-operation latency or SSD wear. `write_bytes`
was zero on tmpfs. Repeated inode/indirect block updates account for some write
amplification; revisit batching only if formatter workloads justify it.

A 256 GB sparse image with a 128 MiB journal, imported headers and an empty volume
also passed checking (0.15 s wall time, 1,612 KiB peak RSS), inspection and writable
`--replay` on its EMPTY journal. Committed recovery, torn records and interrupted
cleanup were reviewed by inspection; no new writer yet produces those states.
No crash injection, synthetic fixtures, new tests, CI changes or physical-media
claims accompany this work. Kernel mounting of the new format belongs to task 3.
