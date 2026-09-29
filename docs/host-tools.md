# Empty-pool host tools

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
root directory and an owner subtree grant covering all defined rights. There is
no policy acquisition operation yet. Pool, volume and root-object IDs use OS
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

## Bounds and status

Both commands accept `--memory-limit SIZE`, default 128 MiB and maximum 1 GiB.
The cap charges input/planning state, construction buffers, candidate buffers,
catalog staging, traversal frames, node caches and proof bookkeeping. Fixed codec
stack frames, caller handles, argv and host allocator/libc overhead are outside
that payload counter. The empty planner currently reserves a bounded workspace
sized for the maximum volume count even for a small pool. It fails before file
creation if this does not fit. An inspector limit failure publishes no volume
records, though preceding selection diagnostics can already have been printed.

Exit codes are 0 for success, 2 for invalid arguments, 3 for corrupt/ambiguous
media, 4 for unsupported meaning or a resource/capacity limit, and 5 for I/O or
allocation failure. An absent/corrupt peer beside a valid selected slot is a
warning for these commands; a successful operation still returns 0. Unsupported,
limit or operational failures in either candidate prevent selection. If multiple
failures exist, command aggregation prioritizes I/O/allocation, proved corruption,
then unsupported/limit, retaining both candidate diagnostics.

Only standalone images, empty construction, `info` and `volumes` are implemented.
GPT selection, namespace traversal and acquisition are the next task. Source
import/extraction and whole-image checking remain later tasks. These tools do not
mount, mutate, repair or establish that the recorded reserves suffice for writes.

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
