# Shared core and encoding

The core implements the local encoding layer of the [format contract](format.md).
It builds as `libpyxis-fs.a` with no host-libc, kernel or Pyxis ABI dependency.
There are no formatter/inspector executables or pool-opening/traversal operations
yet. The next milestone task constructs and reopens empty images using these codecs.

## Build

GNU Make and a GNU C23 compiler are required. From this repository:

```sh
make -j16
make -j16 BUILD=/tmp/pyxis-fs-build HOST_CC=cc HOST_AR=ar
```

The default output is `build/libpyxis-fs.a`. `SOURCE` and `BUILD` can be supplied
explicitly when invoking this Makefile from another directory. `HOST_CC` selects
the compiler, `HOST_AR` the archiver; `CPPFLAGS` and `CFLAGS` add build options.
Required language/freestanding/warning flags remain in force. Use a separate
`BUILD` directory when changing compiler or flags. `make clean` removes only the
known object/dependency files and archive in that output directory.

Pyxis OS pins this repository at `fs/`. During task 2, `make fs-tools` builds the
archive in `build/fs-tools/`; task 3 adds the real formatter and inspector there.
This opt-in target does not participate in kernel, SDK, ports or image builds.
The parent forwards its existing `HOSTCC` setting as this repository's `HOST_CC`.

## Local codec contract

Public headers are under `include/pyxis_fs/`:

| Header | Implemented boundary |
| --- | --- |
| `base.h` | Distinct pool/volume/object/principal IDs, exact hexadecimal conversion, UTF-8 names, CRC32C, feature compatibility and status values |
| `block.h` | One-block header, superblock and pool-root codecs and reference-context checks |
| `record.h` | Seven leaf record codecs and local validators, including none/inline/tree object storage, allocation transitions and grant-mask domains |
| `tree.h` | Internal records, typed key order, tightly packed tree blocks, slot/record validation and supplied owner/parent-level context |
| `platform.h` | Bounded allocation ownership and synchronous exact block reader/builder interfaces |

The codecs do not allocate. Callers own input and output storage and must supply
the stated accessible lengths; buffers and typed structures must be disjoint.
Decoders publish copied output only on success. Failed encodes leave output and
the `written` count unchanged. Void ID-format functions and CRC32C require valid
buffers for their stated lengths; ID formatting writes 33 bytes including NUL.
IDs are not generated here: strong-random generation and collision checks belong
to the host builder when introduced.

Record lengths passed to decoders are exact slot lengths. A decoded tree owns
slot offsets/lengths, not pointers into retained internal storage. The caller
keeps its block unchanged while using those offsets to decode individual records.
Minimum/maximum tree keys describe the local records only, not all descendants.
Tree encoding takes complete encoded records and computes packing and used length.
Block encoders use bounded stack scratch space to validate before publishing.

Record context supplies trusted pool geometry, selected generation, containing
metadata birth and scope features. Block context additionally identifies the
expected pool/reference and referring birth. A tree's context supplies its kind,
owners and parent level (zero for an index root). These inputs must come from
validated ancestors; codecs cannot authenticate context supplied by a caller.
Every local reference/mapping/transition is checked against its containing birth.

`PFS_INVALID` identifies a bad caller/encoder argument. Malformed supported media
is `PFS_CORRUPT`; unknown required meaning is `PFS_UNSUPPORTED`; profile/cap excess
is `PFS_LIMIT`. A missing block magic is `PFS_ABSENT` when decoding a raw header or
superblock, but corrupt when following a required pool/tree reference. Allocation
failure below the cap is `PFS_NO_MEMORY`; failed adapter I/O is `PFS_IO`.

Readers ignore reserved extension bytes and can interpret known fields under
unknown read-compatible features. Record growth must have a declared compatible
extension. A volume codec exposes its supported fixed envelope even if that
volume's own required features or target-tree versions are unknown; callers must
check both before traversing volume contents. Length extensions of pool catalog records use
the enclosing pool's feature declarations; volume masks govern records within
the volume. Feature checking for full consistency is
stricter than ordinary reads. Encoders construct initial-version records with
zero extension bytes and reject unknown feature semantics. Decoding and encoding
an existing record is not an extension-preserving mutation operation.

Success proves local structure only. Pool counters are checked for local arithmetic
consistency; they are not reconciled against allocation records. Tree decoding
checks local ordering and ranges but never fetches children. Callers still need
separator/child-minimum agreement, full ancestry/cycle checks, allocation-proof
closure, file bounds for extent trees, grant-target resolution and global counts.
The standalone extent-file and grant-target helpers check those facts once the
caller has resolved the relevant file length or target kind. No decoded view is
an acquired capability or evidence of policy authorization.

## Platform ownership

Reader and builder instances borrow trusted adapter contexts. Read-only instances
contain no write or flush callback. Builder writes and flushes are explicit; none
of the codec functions performs I/O. A callback borrows the buffer only until it
returns and may return `PFS_OK` only for a complete transfer. Short I/O must be
reported as failure by the adapter. The wrapper checks geometry/ranges, exact
buffer lengths, the adapter transfer bound and the 16-block core limit.

Memory state tracks requested live allocation bytes against the selected cap.
Each live allocation has one caller-owned handle recording owner, size and
alignment; it cannot be copied, altered or reinitialized until freed. Allocator
callbacks receive those exact values on free. Wrapper objects and fixed codec
stack storage are outside this heap counter, as is allocator-private overhead.
Future caches, traversal/checker bookkeeping and builder buffers must use this
boundary to participate in the cap. Instances are serial and provide no locking.

There is no Linux file-descriptor adapter yet. The host tools will supply actual
file I/O, locking, path protection, randomness and allocator callbacks as they
are implemented; no stub callbacks or placeholder commands are supplied here.
