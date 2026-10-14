# Read-only FAT32 engine

`include/pyxis_fs/fat32.h` is the public API. `libfat32-read.a` is freestanding:
no allocation, disk/capability calls, libc, mutation or mount state. The caller
owns a synchronous sector source and an aligned workspace; all arguments and
sink buffers are borrowed for the call. Keep the media quiescent and its source
alive for the copied volume's lifetime. Read-only opening supplies neither a
snapshot nor writer exclusion. The engine never repairs or changes dirty flags.
The installer's separate lenient ESP recovery reader is unchanged: refusal here
must never authorize rebuilding a foreign or damaged ESP.

## Admission and supported profile

Open selects a one-based GPT entry on a whole disk. Protective MBR, both GPT
headers and both arrays must validate and agree, including CRCs, unique GUIDs,
nonoverlapping extents and source bounds. Bare FAT, legacy/hybrid MBR and degraded
GPT recovery are unsupported. The bounded GPT profile accepts at most 256 entries
and 64 KiB of array bytes, with power-of-two entry strides of at least 128 bytes.

FAT32 has 512-byte logical sectors, power-of-two clusters from 1 through 128
sectors, and one FAT or two mirrored FATs. Active-FAT/nonmirrored extensions and
new filesystem versions are unsupported. The volume must fit its selected GPT
extent and contain enough clusters to be FAT32; the FAT must cover them. Declared
backup BPB and FSInfo copies are validated. Absent optional copies are accepted;
FSInfo free-count and next-free hints are advisory, including out-of-range values;
they are not allocation authority. Writers rebuild from the FAT and publish
corrected hints with the next confirmed mutation. Dirty/error flags are observations,
not integrity proofs or permission to write. Meaningful FAT values ignore their
reserved upper nibble; mirrored values must agree.

LFN sequences require ordered slots, one final marker, matching checksums,
zero type/cluster fields, valid termination/filler and valid UTF-16 surrogates.
Names decode to at most 255 UTF-8 bytes without truncation. NT lowercase flags and
ASCII aliases preserve spelling. Unicode names list intact; lookup is ASCII
case-insensitive through supported long names or aliases. Short-only OEM names
refuse, and Unicode/OEM case folding is not inferred. Duplicate exact names/aliases
and ASCII folded name/alias collisions refuse. Short names retain interior spaces,
including spaces in the extension; only trailing padding in each 8.3 component
is removed. This tolerant read profile follows the
[Microsoft FAT specification](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf),
page 30, which permits spaces in short names. It does not normalize or collapse
those spaces. This read compatibility does not adopt existing files for mutation.
Paths start at `/`; empty interior
components and `.`/`..` are invalid. Namespace mutation is absent.

## Retained-entry access

`fat32_root` returns the root entry and its canonical locator;
`fat32_lookup` resolves one counted ASCII component from a directory locator.
`fat32_locate` refreshes the selected entry, and `fat32_read_range` streams a
regular file's requested offset/length, clipped to EOF. Even zero-length or
beyond-EOF reads validate the complete file chain. Offset and length are not
added, so a range extending past the integer limit cannot wrap into the file.

Locators record the containing directory cluster, that directory's parent
cluster for dot-entry validation, and the zero-based logical short-entry slot
within the containing chain. Root is the separate `{0, 0, UINT64_MAX}` sentinel.
Long-name and alias lookups produce identical locators; separate empty files
retain different slots despite both having no data chain. Keep only engine-issued
locators with the original quiescent volume. They supply validation coordinates,
not capability authority, a native object ID or persistent identity. External
mutation and slot reuse are unsupported; finding an ordinary replacement in the
same slot cannot prove it is the original object. Mount/object ownership and
identity interning remain the caller's responsibility.

`fat32_enumerate` validates the selected directory and all immediate children's
chains before returning one entry and its child locator. Start with continuation
0, then pass each returned `next` unchanged; its representation is opaque to the
caller. A successful end returns `found=false` and `next=UINT64_MAX` without
changing the entry/locator outputs. Dot entries and volume labels never appear.
Unicode names retain their UTF-8 spelling. This engine supplies no native cursor
generation, short-buffer protocol or mutable directory handle.

Each retained operation revalidates geometry. Nonroot operations validate the
containing directory's full metadata, chain and sibling-name collisions before
selecting its short-entry slot, then validate the selected chain. Selected
directories validate their own metadata as well. No workspace pointers or caches
survive a call, and no root-to-entry path is retained or replayed. The original
depth budget still controls whole-tree/path traversal; retained operations require
a positive depth budget but do not walk ancestors. Whole-volume admission is a
separate `fat32_check`, not implied by a locator operation.

Entry/locator/continuation outputs publish only on success. Missing or
nonordinary retained slots refuse with STALE. Range sinks follow the same prefix
failure rule as path reads; a kernel adapter needing all-or-nothing publication
must stage the requested bytes and publish them only after success.

## Checked sets and failures

Open checks geometry and allocation headers, not the whole tree. Each subsequent
operation revalidates BPB/FSInfo and FAT headers against the copied geometry;
filesystem backing reads stay inside the selected partition. Read validates the
path directories and entire selected file chain before output. List validates
the selected directory and immediate children's chains. A global visited bitmap
within that operation catches cycles and repeated chain ownership in its checked
set. Files require exactly the clusters their declared length needs; empty files
have no chain. Directories require valid dot entries where applicable. An end marker is accepted
only after checking that every later allocated directory slot has a zero first
byte, across sectors and clusters. Root volume labels require a valid 11-byte
short-name character field, only the VOLUME_ID attribute, and no data chain/size.

`fat32_check` walks the whole tree iteratively, checks every meaningful FAT entry,
and refuses orphan allocations and cross-links. Marked bad clusters are counted,
not treated as free. Its report and open's geometry publish only on success.
A successful individual read does not prove unrelated metadata sound.

Use `fat32_open_workspace_size` for admission and `fat32_workspace_size` for
operations. Workspace holds bounded caches, a cluster bitmap and explicit depth
frames; the engine never allocates a tree or complete file. The caller supplies
positive step, sector and depth budgets. Zero is never unlimited. Exhaustion
returns LIMIT or NO_MEMORY without skipping validation. Name collisions use
bounded pairwise comparisons and can exhaust work on a large directory.

Metadata preflight precedes sink calls. Later source I/O, budget or sink failure
can leave a validated output prefix; callers must inspect the result before using
captured bytes. Source callbacks fill the complete request or fail with IO; sink
failure returns OUTPUT. No operation retries a failed source, repairs metadata,
changes admission mode or downgrades validation. The header distinguishes corrupt,
unsupported, lookup, name-size, capacity, input/output and argument failures.
