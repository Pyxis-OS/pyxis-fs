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
FSInfo hints may be stale but must be in range. Dirty/error flags are observations,
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
