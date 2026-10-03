# npfs host tools

`make -j16` builds the format-only `build/libnpfs-format.a` and Linux tools
`mkfs.npfs`, `fsck.npfs`, `npfs-inspect`. From Pyxis use `make -j16 fs-tools`
(`build/fs-tools/`). No compiler-container rebuild is needed. See the
[encoding contract](npfs-format.md).
The existing filesystem CI job builds these outputs only. Structural/recovery
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

All tools use standalone regular pool image files, not GPT selection or raw disks.
Open validates both headers, features and control selection before reading homes.
Shared read/exclusive write nonblocking `flock` coordinates cooperating tools.
Files must remain unchanged/exclusive externally: locks do not constrain programs
that ignore them. No read-only RAM recovery overlay is provided. Unknown required
features refuse all use; unknown read-only-compatible features prevent replay.
The private host read APIs may partially fill buffers on failure; callers discard
those results. The public format decoders publish only validated copied results.

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
