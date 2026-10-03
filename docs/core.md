# Shared core and encoding

**Historical: retired COW filesystem.** The implementation, host tools and test
suite described here were removed when Caelum adopted the native format. Commands
and APIs below are obsolete; they are not current validation or interfaces. The
[source snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9)
preserves the original implementation. Use the [native format](native-format.md)
and [native host tools](native-host-tools.md) for current behavior.

The core implements the local encoding layer of the [format contract](format.md),
bulk image construction, pool selection, namespace and file reads, policy
acquisition and whole-image consistency checking. It builds as `libpyxis-fs.a`
with no host-libc, kernel or Pyxis ABI dependency. Linux host tools populate
standalone sparse images from source directories and provide diagnostic `info`,
`volumes`, `list`, `stat`, `access`, `extract` and `check` commands. Explicit GPT
partition selection belongs to the host adapter. See [host-tool usage](host-tools.md).
Private canonical validators, COW tree editors and allocation-map planners are
also implemented. Explicit writable open adds retained-state validation, bounded
admission, individually durable publication and bounded retirement carryover.
Held-authority file and namespace mutation APIs use the private COW editor and
publisher; checkpoints settle pool-wide volume retirement.

## Build

GNU Make and a GNU C23 compiler are required. From this repository:

```sh
make -j16
make -j16 BUILD=/tmp/pyxis-fs-build HOST_CC=cc HOST_AR=ar
```

The default outputs are `build/libpyxis-fs.a`, `build/mkpyxisfs` and
`build/pyxisfs-inspect` and `build/pyxisfs-write`. `SOURCE` and `BUILD` can be supplied
explicitly when invoking this Makefile from another directory. `HOST_CC` selects
the compiler, `HOST_AR` the archiver; `CPPFLAGS` and `CFLAGS` add build options.
Required language/freestanding/warning flags remain in force. Use a separate
`BUILD` directory when changing compiler or flags. `make clean` removes only the
known object/dependency files, archive and executables in that output directory.
The maintained host contract suite is described in [testing](testing.md).

For a freestanding cross build, select the archive target explicitly so the
compiler is not asked to build the Linux tools:

```sh
make -j16 BUILD=/tmp/pyxis-fs-target HOST_CC=x86_64-unknown-pyxis-gcc \
  /tmp/pyxis-fs-target/libpyxis-fs.a
```

Pyxis OS pins this repository at `fs/`. `make fs-tools` builds the archive,
formatter and inspector in `build/fs-tools/`.
This opt-in target does not participate in kernel, SDK, ports or image builds.
The parent forwards its existing `HOSTCC` setting as this repository's `HOST_CC`.

## Local codec contract

Public headers are under `include/pyxis_fs/`:

| Header | Implemented boundary |
| --- | --- |
| `base.h` | Distinct pool/volume/object/principal IDs, exact hexadecimal conversion, UTF-8 names, CRC32C, feature compatibility and status values |
| `block.h` | One-block header, superblock and pool-root codecs and reference-context checks |
| `record.h` | Eight leaf record codecs and local validators, including none/inline/tree object storage, orphan IDs, allocation transitions and grant-mask domains |
| `tree.h` | Internal records, typed key order, tightly packed tree blocks, slot/record validation and supplied owner/parent-level context |
| `platform.h` | Bounded allocation ownership and synchronous exact block reader/builder interfaces |
| `build.h` | Owned bulk planning and construction into a newly created output |
| `pool.h` | Independent slot diagnostics, selected pool lifetime and copied diagnostic volume catalogs |
| `read.h` | Diagnostic volume/object access, relative paths, ancestry, explicit grants, file reads and directory cursors |
| `access.h` | Trusted acquisition contexts, policy evaluation and opaque views with checked ordinary operations |
| `check.h` | Full diagnostic traversal, per-state reconciliation and retained-state overlap checking |
| `write.h` | Explicit writable open/profile, writer health, ordinary volume opening and authority-checked checkpoint |

The codecs do not allocate. Callers own input and output storage and must supply
the stated accessible lengths; buffers and typed structures must be disjoint.
Decoders publish copied output only on success. Failed encodes leave output and
the `written` count unchanged. Void ID-format functions and CRC32C require valid
buffers for their stated lengths; ID formatting writes 33 bytes including NUL.
Codec calls do not generate IDs. The formatter supplies strong-random IDs; live
file creation uses the writer adapter's strong-random callback and checks both
retained object indexes before accepting an ID.

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
`PFS_NOT_FOUND` identifies a missing selector or path. `PFS_DENIED` is a policy
or held-rights denial. `PFS_READ_ONLY` means policy permits the requested rights,
but this implementation provides no mutation operation. `PFS_BUSY` preserves a
live pool or volume when retained handles prevent close.

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

Pool feature helpers recognize no enabled features. The corresponding
`pfs_volume_features_*` helpers recognize read-required `PFS_FEATURE_ORPHANS`
(bit 0); unknown volume write-required or optional bits still prevent encoding
and complete checking. The volume's nullable orphan root occupies the declared
24 bytes at offset 424. This feature leaves structure versions and record sizes
unchanged. The formatter keeps its feature masks zero and its existing owner
grant: directory bits 0..5, with the new `dir.checkpoint` bit 6 absent.

Codec success proves local structure only. Pool counters are checked for local
arithmetic consistency; they are not reconciled against allocation records.
Tree decoding checks local ordering and ranges but never fetches children. Callers still need
separator/child-minimum agreement, full ancestry/cycle checks, allocation-proof
closure, file bounds for extent trees, grant-target resolution and global counts.
The standalone extent-file and grant-target helpers check those facts once the
caller has resolved the relevant file length or target kind. No decoded view is
an acquired capability or evidence of policy authorization.

## Construction and pool operations

`pfs_build_plan_create` copies the supplied pool, volume and object specifications
and owns its accounting, sorting arrays, node layouts and construction workspace
through `pfs_memory`. Initialize the plan to zero; do not copy or modify a live
plan. Its public summaries and volume records are borrowed until
`pfs_build_plan_destroy`, and its memory owner must outlive it. Planning performs
no I/O and retains no input-array pointers after returning. Checked count,
geometry and memory bounds are applied before allocation. See the
[bulk construction layout](empty-layout.md) for allocation-map termination and
reserve/guarantee accounting.

Every volume supplies an object array. Index zero is a directory whose ID matches
`root_object`, with an empty name and `UINT32_MAX` parent. Each remaining parent
index precedes its child and identifies a directory in the same array. Names are
unique within a directory; persistent IDs are nonzero and collision-free. The
planner checks the global object-count profile and the 256-object ancestry limit,
builds object and directory trees with exact codec packing, and rejects capacity
or quota excess. All objects use the selected volume owner and inherit the root's
one explicit owner subtree grant. Empty objects need no storage tree. Nonempty
files each have one contiguous inline extent, with zero final-block padding.
Every non-root directory-index node has at least six records or children.
Planning repairs a short final group at each level using its adjacent predecessor,
with actual byte fit and exact minima recomputed before block/quota accounting.
A sole root leaf may have fewer than six records; empty directories have no
index, and a one-child internal root collapses. Other indexes retain their sparse
shape contract. This packing alone does not establish writable admission.

`pfs_build` requires a newly created, exclusively owned empty output. It uses the
plan's allocated buffers without output reads or further allocation. A
`pfs_build_source` borrows an adapter context and supplies synchronous exact-byte
`read` and `validate` callbacks. Reads identify original volume/object indices,
stream each nonempty file from offset zero through EOF before the next file, and
request at most 64 KiB, respecting smaller output-transfer limits. Empty files
produce no read callbacks. After all metadata trees and file data are written,
`validate` checks the sources before pool-root and superblock publication.
Callback failures propagate unchanged. Both callbacks are required for a supplied
source; a null source is permitted only when there are no nonempty files.

After successful validation, construction writes the pool root, flushes metadata
and data, writes both generation-1 slots, then flushes again. The host separately
flushes the containing directory. Failure leaves partial output; retry is not
recovery. Destroy releases the plan without I/O. Empty volumes use the same
builder and accounting path.

`pfs_pool_open` examines both slots independently and publishes their diagnostics
even on media failure. Successful candidate selection validates the superblock,
pool root and three catalog/map root-node contexts; it does not establish map
ownership or globally verify accounting. Equal-generation disagreement is
ambiguous corruption. A selectable absent/corrupt peer is reported as degraded;
unsupported, limit and operational failures prevent selection.

Initialize pool handles to zero and never copy them. A pool borrows its reader,
memory owner and adapter contexts, which remain alive and unchanged until close.
This read-only entry point requires an unchanged image for its lifetime. Failed open leaves
the handle empty. Close releases owned state without I/O. `pfs_pool_close` returns
`PFS_BUSY` while volume handles remain open.
Volume close similarly returns `PFS_BUSY` while views or directory cursors retain
that volume. Close the retained handles first; a busy close leaves its handle live.

`pfs_pool_diagnostic_volumes` returns copied volume envelopes in unsigned name-byte
order, including unsupported-volume features or target versions. It traverses
both selected-state catalogs, checks their agreement and completes the allocation
proof closure for all consulted pool metadata. It does not inspect object/grant
trees or prove global accounting. Caller output capacity must cover the recorded
volume count; output and count are unchanged on failure. Temporary traversal and
proof storage is released before return. This diagnostic grants no object authority.

## Diagnostic traversal and reads

`pfs_pool_diagnostic_volume_open` selects a volume by ID after catalog agreement
and consulted-metadata allocation proof. Traversing its contents requires
`pfs_volume_features_read` compatibility and supported target-root versions. Unknown
write requirements and read-compatible optional features do not prevent reads.
The volume retains the selected pool generation until close. Its metadata,
objects, parent chains and explicit grants are copied into caller storage; no
internal cache pointer is returned.

Diagnostic resolution takes strict relative UTF-8 name components. Empty paths
select the supplied root; leading/trailing slashes, empty components, `.` and
`..` are invalid. The inspector translates its command-line `.` root syntax to
an empty core path. Object access validates ancestry to the volume root, detects
cycles and checks parent kind and unique naming for the consulted chain.
Ancestry queries support a validated sizing call before copying the chain in
root-to-target order. Selectors are not evidence of policy authority.

With ORPHANS enabled, diagnostic object, file-read and grant operations may
inspect a parentless non-root object by ID after proving its orphan-index entry
and consulted allocation ownership. An orphan directory must be empty. Such an
object has no ordinary acquisition ancestry: ancestry and ordinary acquisition
return `PFS_NOT_FOUND`. Path traversal and listings expose the named namespace.

Every incoming tree reference checks physical identity, generation, owner, kind,
parent level and the supplied key bounds, including when its bytes were already
consulted. Allocation proof bootstraps from structurally checked map nodes. Each
consulted metadata block and data range must match live allocation state, owner
and birth; split ranges are checked across map records. The operation records
map dependencies, adds newly consulted nodes to a bounded work queue, and closes
the fully matched pending set before publishing output. Pending dependencies are
never used as completed proof. This establishes consulted ownership, not global
reachability, duplicate-claim absence or accounting.

File reads validate inline or tree extent bounds against file length, clamp at
EOF and return zeros for holes. Bytes are copied only after allocation proof and
successful I/O. Valid calls report a proved byte prefix even on later failure;
the diagnostic interface leaves bytes beyond that prefix unchanged. Directory
cursors return copied pages in unsigned name-byte order, checking child kind and
parent and the recorded count at exhaustion. A failed page leaves caller output,
count, end flag and cursor position unchanged. Cursors retain their volume and
generation independently of other handles.

Traversal and proof buffers are operation-local and charged to `pfs_memory`.
Parent validation scans each consulted ancestor directory to establish its
naming relation. Separate calls repeat those scans and rebuild their proof
storage; policy evaluation also fetches ancestor grants. These costs can grow
with directory size, ancestry depth and allocation-map closure. The memory cap
can stop an operation with `PFS_LIMIT`; the bounded profile does not promise a
small I/O count or constant-time lookup.

## Whole-image consistency checking

`pfs_check` borrows an unchanged block reader and its memory owner for one
synchronous diagnostic operation. It examines both candidate slots, including
the state not selected for ordinary reads, and reports their results separately.
It returns copied slot diagnostics, per-state status/completeness and visited
record/claimed-block counts, plus the cross-state status/completeness. A state's
`failures` bitmap has one bit at each observed `pfs_status` value; its `status`
is the primary failure under the aggregation priority below. Counts from failed
or incomplete traversals describe partial work. Invalid arguments
leave the caller's result unchanged; media and resource failures publish partial
results. The operation owns no handle after return and releases its workspace.

An optional reporter receives individual failures with operation, slot and
available block/volume/object context. Events are borrowed only until the
callback returns. A reporter must not reenter the checker or change the reader,
memory owner or image. `PFS_POOL_NO_SELECTION` identifies a global or cross-state
event; `UINT64_MAX` identifies an unavailable block, and zero IDs an unavailable
volume/object.

Bounded iterative tree walks use the shared codecs to validate keys, separators,
metadata identity and owner context. The checker reconciles both catalogs,
namespace parent/entry relations and reachability, object storage, grants and
extent claims. Sorted physical claims are matched against each state's allocation
map, including the pool root and map nodes, and recorded counts, ownership and
budget charges are reconciled. Cross-state comparison checks compatible shared
storage and allocation incarnations, including older live references protected
by newer retired allocations. Historical free/retired entries are not treated
as live references, and retired contents are not parsed.

In each ORPHANS-enabled retained state, every non-root object is named exactly
once with the matching parent, or has zero parent and exactly one orphan entry.
It cannot be both. The root is never orphaned; orphan directories are empty.
Object counts and allocation claims include orphan storage, and check results
report orphan counts. The checker proves these representation rules but keeps
the broader read-only tree occupancy contract; it does not perform six-entry
writable admission or remove orphans.

Traversal frames, identity tables, copied records and physical claims share the
supplied memory cap across retained states. Profile or cap exhaustion returns
`PFS_LIMIT`; allocator failure below the cap returns `PFS_NO_MEMORY`. A
resource-exhausted partial state is released before attempting its peer, and its
tables cannot support a complete cross-state proof. Independent supported checks
continue where their preconditions remain established.

Full checking rejects every unknown enabled pool/volume feature, including those
compatible with ordinary reads. Unsupported contents are skipped and the result
remains incomplete. Unknown semantics never justify an unexplained-allocation
corruption claim; independently established corruption is still reported. I/O or
allocation failure takes priority over corruption, then unsupported/resource or
other incomplete results. `PFS_OK` requires two fully checked valid states and
a complete cross-state proof. A valid state with an absent peer is incomplete;
with a corrupt peer it is corrupt.

The checker reads metadata and proves file-data allocation claims without reading
payloads. File bytes have no integrity checksums and are not verified. It performs
no writes, repair, reclamation or runtime-reader exclusion proof. Content
validation remains the separate extraction/host-comparison workflow.

## Private candidate planning

`core/canonical.h`, `core/edit.h` and `core/plan.h` are private interfaces. Their
success is a candidate plan, not authorization, retained-state admission or
durability. The caller must establish ownership, exact subtree minima, understood
features and the relevant tree profile before editing. Reusable block IDs must
be distinct and disjoint from every published claim; runtime pins and both
retained states remain caller proof obligations.

Canonical validators decode then compare against exact version-1 encoding.
They require fixed record/header lengths, exact variable payload lengths and
zero reserved, alignment and unused bytes, except the declared orphan root.
Pool masks are zero; volume masks permit only read-required ORPHANS. Ordinary
read-only codecs retain their broader extension compatibility. Local canonical
validation does not prove allocation ownership or global reachability.

`pfs_edit_tree` edits object, extent, grant, directory and orphan indexes, and
supports same-key, fixed-length volume-catalog updates. Insert/delete operations
repair one path bottom-up, preserve exact minima and collapse roots. Directory
and orphan nodes require six items outside the root; object/extent/grant indexes
use the sparse empty-leaf and one-child internal repair. The caller reserves
staging slots, retirement references and disjoint workspace before calling.
Failure leaves the candidate root, slots and retirement list unchanged; workspace
is scratch. Superseded private nodes are discarded, not durably retired.
Extent insert/update also enforces the nearest successor subtree minimum: a
range may end at that boundary but cannot overlap the next leaf. Split plans
also check adjacency between the last extent in the new left leaf and the first
in the new right leaf. Overlap is `PFS_INVALID`, with the candidate unchanged;
touching ranges remain valid. There are no device writes or heap allocations
during an edit.

At maximum depth eight, insert plans create at most 15 nodes and retire eight;
fixed-length updates create/retire eight. Sparse deletes create/retire at most
14, and six-entry namespace deletes at most 15. These are single-record plans,
not complete transaction/admission costs.

`pfs_plan_map_apply` overlays sorted disjoint deltas on a complete canonical map,
checking the source state, owner, birth, retirement and charge before replacing
each range and coalescing the result. Inputs remain unchanged; the result count
is published only on success, while output bytes are scratch on failure.
A delta whose expected before-state disagrees with the validated base is a
caller error (`PFS_INVALID`); malformed base data remains `PFS_CORRUPT`.
Protection and valid retire/free transitions require separate caller evidence.

`pfs_plan_map_build` takes a canonical base after volume changes, old pool/map/
catalog retirements and eligible frees, with no live pool allocation born in the
candidate. For base count `R` and catalog union node count `c <= 16`, it chooses
`S = ceil(23*(R + 2*c + 4)/21)`, then `ceil(S/46)` map leaves and successive
`ceil(previous/65)` internal levels. If `F(S)` counts those nodes, it reserves
exactly `N = F(S) + c + 1` reusable blocks before overlaying their allocations.
Arbitrary placement adds at most `2*N` records; the selected `S` bounds the final
count without iterating allocation or allocating unused live padding.
Every map node is reachable and nonempty; internal groups have at least two
children. Map blocks are encoded in the arena; catalog and pool-root block IDs
are reserved for the caller to encode. The planner verifies sorted reusable
ranges and base-free containment. Those ranges must already be proven durably free in
the selected state without live protection in either retained state, with runtime
pins ended before this publication; freeing a
range in this candidate does not make it eligible. Planning performs no I/O or
further allocation, and its borrowed output expires on the next plan or destroy.

`pfs_plan_limits` computes envelopes from explicit extent capacity `E`, volume
metadata capacity `M` and volume count `v`; it chooses no defaults or admission
policy. With `V=128`, `D=256`, `Pcat=4*v-2` and `C=min(Pcat,16)`:

```text
K = 1 + 2*(E + M + 2*D)
Rbase(H) = K + 2*Pcat + 6*H
S(H) = ceil(23*(Rbase(H) + 2*C + 4)/21)
Hclosed = ceil((K + 2*Pcat + 23*C + 47)/15)
```

It scans `H=C+2..Hclosed` for the first `F(S(H)) + C + 1 <= H`, subject to
record and depth limits. Both retained opening states and every candidate require
map-node count at most `H-C-1` and live pool blocks at most `Pmax=Pcat+H`;
imported maps need not have the planner's compact shape. C is the conservative
catalog union ceiling; actual shared nodes are counted and replaced once.
Recovery capacity is at least `3*H+2*D+V`, also honoring the formatter's 256-block
floor. Ordinary workspace is at least `max(1024,2*D+V)`; migration remains empty
and keeps its formatter floor. The workspace envelope guarantees `H+V` reusable
input blocks without allocation from same-publication frees.

The normal selected state at generation g carries at most D volume retirement
from each of retirement generations g and g-1, for at most 2D total and at most two owner volumes. Pool retirement
remains bounded by 2H, with at most H protected. Checked writable opening accepts
up to 2D protected volume blocks and two owners without imposing a runtime debt-age
rule. Its initial fence establishes empty volume debt before orphan batching;
normal candidates then preserve the stronger two-cohort shape.

Generation funding from confirmed g requires `g+a+3*T <= UINT64_MAX`, where T is
remaining orphan work and a is zero with no volume debt, one for entirely eligible
debt and two if any is protected. An ordinary candidate additionally funds its one
publication, two terminal fence generations and `3*T_projected`. Each orphan batch
reduces T by at least one; generation increments remain exactly one without wrap.

One capped arena reserves three S-entry map vectors at 80 bytes per
slot, three `(E+M+Pmax)`-entry claim vectors at 96 bytes, 48 bytes per possible
object (`min(1048576,29*M)`), `(H+V)` block buffers, `8*(H+V+D)` delta slots at
128 bytes, and 8 MiB scratch. Opening validation and live handles are additional
charges to the same memory owner. Arena creation owns one allocation; destroy
clears its pointers. These computed envelopes and reserved storage do not prove
physical backing, free capacity, permanent deletion promises or writable
admission. See [maintained contract checks](testing.md) for exercised boundaries.

The private edit/validation/codec call path is not yet suitable for Caelum's
16 KiB kernel-task stack. Pyxis GCC 16.2.0 with kernel flags gives a conservative
sum of maximum reported frames of 19,120 bytes along the principal nested path
after the split-boundary correction (19,104 in the reviewed version), before
deeper helpers and outer frames. This is not a runtime high-water measurement. These private objects are
not kernel-linked; host checks and target compilation do not qualify their stack
usage. Resolving this, preferably with caller-reserved temporary workspace, is a
[prerequisite for native writable integration](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/technical-debt.md#writable-filesystem-kernel-stack-prerequisite).
The buffer refactor is deferred beyond this task.

## Policy acquisition and ordinary views

The embedding authority supplies the trusted principal, root, scope and rights
ceiling. The core does not authenticate IDs. `pfs_access_evaluate` checks target
containment, applicable grants, ceilings and effective `dir.lookup` on every
intervening directory. Object-only grants apply to that object; subtree requests
use applicable subtree grants and require a directory target. Grants above a
restricted root can contribute policy but cannot enlarge its boundary or ceiling.
Path acquisition checks lookup policy before resolving the next component;
object-ID acquisition requires the same intervening lookup rights.

Acquisition requires the complete requested set and publishes no weaker view.
Policy denial returns `PFS_DENIED`. Read-only instances refuse mutation rights
with `PFS_READ_ONLY`. Writer instances also support file write/resize/checkpoint
and directory create/remove/replace/checkpoint; unimplemented administration rights
still return `PFS_READ_ONLY` without an operational view. Successful opaque views bind volume,
object, scope and exactly the requested rights. The generation field identifies
the immutable state in read-only mode and is zero in live writer results. They retain the volume
until explicit close and are serial, never copied. Ordinary child derivation
uses held subtree scope and rights, without reacquiring principal policy, and
cannot expand the boundary or create a subtree view of a file.

Ordinary metadata returns identity, kind and size/count only, guarded by the
appropriate metadata right. File reads require `file.read`; listing requires
`dir.list` and exposes names/kinds only, without child IDs or views. Deriving a
child requires `dir.lookup` and publishes its identity/kind with the new view.
Only `admin.inspect` exposes owner and explicit grants. None of these rights
implicitly adds another domain. A view directory cursor captures list authority
and independently retains the volume, so closing its originating view does not
invalidate the cursor.

### Stateless directory continuation

`pfs_view_directory_page(view, after, entries, capacity, count, done, next)`
provides an independent LIST page on a read-only instance without a retained
cursor. Writers use the live token interface below; the immutable page interface
returns `PFS_READ_ONLY` there. Start with `after=0`
and pass the returned `next` to resume. Capacity must be positive and within
`PFS_RECORD_COUNT_MAX`; outputs and the view must be disjoint. Each successful
page publishes copied names/kinds in unsigned name order, its count, end flag
and the last returned continuation. A zero-entry page preserves the input point;
repeating the final point returns zero entries and `done=true`. A full page can
return `done=false` even when its last entry is the directory's final entry;
then the next call establishes exhaustion. All outputs remain unchanged on any
failure, including denial, invalid continuation, media and memory-limit errors.

The opaque 64-bit value contains no address or authority. The current encoding
packs one-based tree slot choices, with the leaf entry in the low byte and its
ancestors in successively higher bytes. Eight levels and at most 195 slots per
node fit in 64 bits; compile-time assertions tie the encoding to these bounds.
The directory root's validated level determines the number of used bytes. Zero
slot choices, out-of-range slots and nonzero excess bytes return `PFS_INVALID`.
Do not decode, increment or persist these values as on-disk identities.

Each nonzero token is interpreted against the held view's immutable selected
pool/volume/directory. Descent begins at its directory root and follows only
validated internal references, preserving parent level, separator and inherited
upper bounds. The token never supplies a block address. The reached entry's key
supplies an exclusive lower bound to the existing successor search; the reader
does not enumerate the returned prefix to recover a position. Errors from rooted
reads, metadata decoding and allocation proofs retain their statuses rather than
being reclassified as bad token bytes. All consulted allocations are proved
before publication, and returned children must match their recorded kind/parent.
Duplicate child IDs within a returned page are rejected.

Tokens are untrusted resume hints, not authenticated records of prior calls. A
forged reachable position can skip entries within the authorized directory, and
the same numeric token may also be valid in another authorized view. Neither
case expands LIST authority or proves token provenance. The embedding OS must
keep its directory/generation binding separate; an old token cannot retain or
reopen its view. No emitted-count value is taken from the caller, so this page
operation makes no full-list count or cross-page uniqueness claim. Existing
stateful diagnostic/view cursors retain end-to-end count reconciliation; the
whole-image checker remains the global structural check.

The call borrows its view and releases all temporary memory before return. It
retains no cursor, extra volume reference or continuation registry: callers may
replay, fork or abandon tokens without close calls. Existing ancestry naming
scans and allocation-proof costs still apply; eliminating prefix reconstruction
does not establish constant-time enumeration or a small I/O bound. Validation
coverage is recorded in the [host-tool guide](host-tools.md#directory-continuation-validation).

The diagnostic APIs and inspector commands operate over an already authorized
image and may disclose identities and policy records. Their results are separate
from ordinary view authority. The host `access` command simulates supplied trusted
inputs; it is not authentication or a login mechanism.

## Platform ownership

Reader and builder instances borrow trusted adapter contexts. Read-only instances
contain no write or flush callback. Builder writes and flushes are explicit; none
of the codec functions performs I/O. A callback borrows the buffer only until it
returns and may return `PFS_OK` only for a complete transfer. Short I/O must be
reported as failure by the adapter. The wrapper checks geometry/ranges, exact
buffer lengths, the adapter transfer bound and the 16-block core limit.

Memory state tracks requested live allocation bytes against the selected cap.
Each live allocation has one caller-owned handle recording owner, size and
alignment; it cannot be copied, altered or reinitialized outside
`pfs_memory_move`. This operation transfers ownership to an empty, disjoint
handle and clears the source without changing the charged bytes. A handle stored
inside its own payload must move out before free. Allocator callbacks receive
those exact values on free. Caller-supplied handle structs and fixed codec stack
storage are outside this heap counter, as is allocator-private overhead.
Opening scratch, retained pool/volume/view/cursor state, traversal and proof
bookkeeping, plan buffers and host path/output buffers use this boundary.
Instances are serial and provide no locking.

The Linux adapter supplies exact file I/O, bounded allocation, strong randomness,
exclusive output creation and advisory locks. Inspection takes a shared lock;
construction owns its newly created output exclusively. Writers that ignore
advisory locks are outside the consistency contract. Detectable extent-size
changes fail I/O; the adapter does not promise a snapshot of changing media.
The default host reader opens a standalone pool. Explicit paired
`--gpt-partition N --sector-size 512|4096` options select a used entry in a regular
whole-disk image after the protective MBR validates and both bounded GPT copies
are inspected. Healthy or selectable degraded GPT metadata supplies the extent;
ambiguous, unsupported or failed discovery supplies none.
GPT diagnostics remain separate from pool-slot diagnostics. The core receives
only the partition-relative 4096-byte reader; a sector-aligned start may have no
disk-wide 4096-byte alignment, and a trailing partial pool block is ignored.
The host reader retains the whole-file size for change detection and never reads
outside the selected extent. No raw device access, GPT repair or writes are
provided by inspection. The host importer retains source-root descriptors and
charged manifests of names, parent indices and file identities. It traverses and
reopens entries relative to those descriptors without following symlinks, rejects
special files, and gives hard-linked files independent objects and data extents.
It checks device/inode, type, size, mtime and ctime while scanning/reopening/copying,
revalidates all source objects before output creation and before slot publication,
and refuses an output parent matching any imported directory identity. Sources
must be quiescent: these checks detect changes but do not establish an atomic
snapshot. Open traversal descriptors are bounded by ancestry, and one source
file descriptor is retained during its streamed copy.

Extraction uses diagnostic volume operations with a charged transfer buffer and
bounded iterative directory frames. Fresh output files and directories use modes
0600 and 0700 before a stricter umask. Output parent components are opened through
retained descriptors without symlinks or `..`; extraction never merges into an
existing tree. Files are flushed after copying, directories after their children,
and the containing parent before successful completion. Failure leaves reported
partial output for explicit removal. The host owns and closes all source/output
descriptors; core readers and builders borrow their adapter contexts. Checking
uses that same read-only adapter and shared lock without modifying the image.


## Admitted writer and publication

`pfs_pool_open_writer` borrows an exclusive block builder and capped memory owner.
It requires explicit E/M limits, a strong-random callback for the instance nonce
and future object IDs, complete supported validation of both retained
states, canonical metadata, the namespace occupancy profile, unchanged volume
identities/promises, permanent deletion headroom and the computed workspace
reserves. It rejects degraded states, live workspace charges, occupied migration
charges. Opening reserves the three map/claim vectors and commit/drain arena,
fences selected volume retirement, cleans abandoned orphans, and performs a final
retirement fence before exposing the writer. Views cannot consume that reservation.
The first unlink may enable
the existing volume ORPHANS feature; each retained state is checked using its own
feature mask, including the older state without that feature.

Opening returns the pool diagnostic, confirmed startup generation, health, cleanup
outcome and computed arena/recovery/permanent-pool requirements. E counts mappings,
including inline mappings; M covers actual metadata plus namespace deletion
headroom. Both retained states must fit. No capacity defaults are inferred from
image size. Admission is logical capacity: it does not preallocate sparse-file
host storage. The existing 1 GiB core memory cap still applies.

The private `core/writer.h` batch interface accepts the output of trusted COW
editors. Those editors establish semantic changes and complete changed-path
claims; the publisher independently reconciles physical maps/claims, counts,
limits, charges and retained protection. It does not rescan unchanged file trees
for each transaction. `prepare` holds the serial operation gate until `commit` or
`abort`; ordinary quota/profile/capacity admission refusal performs no device writes
and leaves a healthy writer usable. Failure to obtain publication workspace already
guaranteed by writable-state admission is an admission/editor invariant failure,
even during preparation or planning before any device write. It stops mutation
with `READABLE_STOPPED`, escalating to `ACCESS_STOPPED` if integrity or publication
certainty is lost; that instance cannot retry itself healthy. There is no public
test transaction interface. File mutation uses this
same private path; it does not introduce an alternate publisher.

Volume preparation prefers the first physically contiguous eligible run covering
the existing 128-block candidate reservation. Eligibility still intersects both
retained maps and excludes either state's live claims; adjacent eligible map
segments may form one run. If no run is large enough, preparation uses the existing
fragmented selection. This preference adds no contiguity admission requirement,
changes no reservation size or E/M limit, and retains the existing
lowest-eligible-first selection policy for pool metadata. Only blocks used by the final candidate become allocations. Physical
fragmentation and different publication births can still prevent extent coalescing.

Every admitted publication writes its complete replacement blocks, flushes,
attempts the older slot once, then flushes again. No short/failed write is retried.
All map nodes, pool root and changed catalog paths are included in allocation
accounting. Only the final successful flush rotates the confirmed in-memory
state. User and orphan batches may carry bounded volume retirement across
publications. Each publication frees eligible input debt, including debt owned by
another volume, while replacements allocate only from already-reusable input
space. A candidate changing one volume and freeing another updates their catalog
paths together. The normal pipeline retains at most two volume cohorts; the most
recent remains protected until the older slot advances. Admission funds terminal
reclamation independently of another application mutation.

An explicit retirement fence uses at most two pure publications to advance retained
protection and durably free selected volume debt. Writable startup fences before
and after abandoned-orphan cleanup. Every publication also frees eligible pool
debt and leaves the bounded, recovery-charged remainder; no fence chases zero
retired pool metadata.

Free/reuse decisions use both complete retained summaries and ended synchronous
operation/I/O borrows. A selected free record must already be durably published
before allocation. Historical retired entries in the older map are not live
protection; either state's live map allocation or live claim still prevents
reuse. A publication cannot allocate a range it frees in that same publication.
Retained object views identify objects, not immutable physical snapshots. Ordinary
operations refresh their volume description and observe the latest confirmed
state under their held authority. Immutable diagnostic calls and cursors remain
read-only-mode facilities. Callback reentry into ordinary/lifetime operations is
`PFS_BUSY`; in-memory health reporting remains available.

`pfs_view_checkpoint` requires the held `file.checkpoint` or `dir.checkpoint`
right independently of read/lookup rights. Existing grants are unchanged. Its
fence settles pool-wide volume debt, including other volumes, without changing
object authority or cleaning retained orphans. It reports maintenance `COMPLETE`
even when no publication is needed. A fence error is both the checkpoint operation
error and a stopped/unknown maintenance outcome; replacement/pre-slot failures
remain `READABLE_STOPPED`. A stopped instance cannot use checkpoint, reopening its
handles or close to retry maintenance.
Pool/volume close releases runtime resources without writes or an implicit flush.
Last-view close on a healthy writer completes funded orphan deletion as described
below and may leave retirement pending; stopped close only releases runtime
references.

| Failure | Confirmed state and subsequent access |
| --- | --- |
| Profile/quota/space/memory refusal before admission | `STOPPED`; healthy writer remains usable |
| Replacement write or pre-slot flush error | `STOPPED`, `READABLE_STOPPED`; confirmed-state reads remain authorized |
| Slot-write attempt or final flush error | `UNKNOWN`, `ACCESS_STOPPED`; no ordinary access |
| Cleanup error after confirmed user publication | User remains `COMPLETE`; cleanup reports stopped/unknown independently with the corresponding health |
| Failure to obtain already-guaranteed publication workspace, or unexpected resource failure during funded cleanup/fence | Admission/editor invariant failure; `READABLE_STOPPED`, escalating if integrity or publication certainty is lost; that instance cannot retry itself healthy |
| Integrity failure or any backing read failure during an ordinary operation, publication planning or maintenance | `ACCESS_STOPPED`, pool-wide, even for a transient read error |

The initial writer deliberately uses this conservative read-error policy. Even a
transient backing read failure requires a fresh validated reopen under the
adapter recovery preconditions; clearing the error does not restore this instance.
A read error during maintenance preserves all already confirmed user progress.

`PFS_NO_SPACE`, `PFS_QUOTA` and `PFS_LIMIT` identify capacity/profile errors, but
the status alone does not establish a healthy admission refusal: failure to obtain
already-guaranteed workspace stops mutation and sets writer `invariant_failure`.
`PFS_RECOVERY_REQUIRED` refuses operations prohibited by stopped health. Unknown
outcomes may include additional committed bytes and are not automatically
retryable. The result's confirmed bytes/length/namespace fields are independent of
health; private batches do not acknowledge application bytes. Public mutation
operations populate these fields according to their confirmed progress.

Maintenance `PENDING` means completed healthy mutation/cleanup left funded selected
volume retirement: maintenance status is `OK` and health is `READY`. `COMPLETE`
confirms an explicitly requested retirement fence. `NONE` reports no maintenance
outcome and does not certify debt absence; rejected calls and named-object or
stopped release-only closes may report it while debt remains. Writer status
`drain_pending` independently reports last-confirmed selected volume debt; it no
longer prohibits the next mutation. Idle healthy writers may retain bounded debt
indefinitely; there is no timer or background worker. After uncertainty it does not certify the
actual durable image. Ordinary successful mutation stays `COMPLETE` with its
confirmed progress even when maintenance is `PENDING`.

After earlier batches confirm partial progress, a later ordinary admission refusal
may leave health `READY` and report maintenance `PENDING` or `NONE`, depending on
the refusal stage. Planning can retain the earlier `PENDING`; commit-time admission
can replace it with `NONE`. Neither changes confirmed progress or establishes that
debt was settled. `NONE` reports no maintenance outcome; `drain_pending` separately
reports last-confirmed selected volume debt.

A later user-batch failure retains the confirmed call prefix and reports its own
operation error/uncertainty and resulting health, without inventing maintenance
failure. If required orphan cleanup instead fails after namespace confirmation,
that confirmation remains true and maintenance reports `STOPPED` or `UNKNOWN`.
The latest actual maintenance failure takes precedence over earlier healthy
pending/complete outcomes.

Recovery is an adapter/operator precondition, not a core history detector. The
adapter must establish an appropriate durable backing state and quiesce I/O;
close/reopen, cached validation and a later successful flush do not establish it
after writeback failure. The simulator supplies explicitly durable images for
recovery tests. The ordinary Linux host adapter has no qualified post-error or
unknown-history interrupted-session recovery route, no registry and no force
clear. See [healthy host operation](host-tools.md#healthy-writer-sessions).

The writer attempts [incremental map publication](incremental-map.md)
with self-accounting, seam closure and neighbouring-leaf redistribution.
Replacement marks grow within each planning phase.
Before expansion it can try one additional leaf under an existing parent with
room, renewing accounting for n=p+1 emissions and checking final live J+1<=m.
A miss restores the seed once, regenerates mutable candidate accounting and
disables further trials before current repair/bulk. Source retirements remain p;
the virtual leaf is not source metadata. No internal split, root growth or merge
is introduced, and all reserve/admission/memory bounds remain unchanged.
The accepted repair policy scores both immediate clean-neighbour expansions,
including any bridged marked run, preferring fit/smaller deficit with left ties.
It still renews accounting and seams before sealing and uses no new reservation.
An explicit funded bulk fallback handles global closure and exhausted packing;
source reads, flat summaries and admission remain population-sized. The two-flush
publication protocol and carryover/fence guarantees are unchanged. Local hits
can equal or exceed bulk replacement cost, so write traffic and planning are
measured through all maintenance and final checkpoints. This intermediate step
does not qualify writable deployment. Private writer/planner/encoder stack use
still needs resolution before native writable integration; the kernel links only
the read-only core and its small mode/health bridge, not the publisher.


## Live file mutation

`write.h` exposes `pfs_view_create_file`, `pfs_view_write` and `pfs_view_resize`.
These require a healthy writer and held `dir.create`, `file.write` or
`file.resize` respectively. A nonempty write extending the current length needs
both file rights, checked for the entire request before its first publication.
Zero-length writes and same-length resizes check authority and health but publish
nothing. Invalid arguments and rejected callback reentry preserve result outputs.
Other refusals return initialized results, without widening authority.

Creation accepts a validated single component name and returns `EXISTS` for an
existing entry. The empty file receives a fresh ID, the parent's policy owner and
no explicit grants. Strong randomness has no fallback; zero/colliding IDs are
retried at most 16 times against both retained object indexes and live runtime
identities, including retained orphans.
Entropy failure refuses creation without publishing or stopping otherwise healthy
backing; an actual backing-read failure retains the pool-wide access-stop policy.

The optional child-handle request requires parent subtree scope, lookup and every
requested right within the held masks. Creation validates these requirements and
reserves the view before publication. No returned view implies no added authority.
A confirmed creation returns its reserved view; an unknown creation returns none.
Identity output does not require separate metadata rights.

Writes edit only affected mappings and object paths, preserving untouched storage.
Within a call, compatible logical/physical runs born in the same transaction
coalesce. A separately committed append has a new birth and cannot merge with
an older run merely because it is adjacent. The planner adds slices until the
actual staged edit reaches its fixed transaction bounds, then publishes an
individually durable batch. Funded retirement can carry into later batches or an
explicit checkpoint. No delayed acknowledgement or batching across completed calls
is introduced. Extent lookup uses the admitted claim summary with a bounded
private overlay rather than a whole-file rewrite.

Sparse growth exposes zeros. Partial-block writes preserve bytes outside their
range; extending writes and growth clear any previously hidden partial-EOF suffix
before exposing it. Shrink publishes valid intermediate lengths from the tail,
retiring at most 128 data blocks and processing at most six tail mappings per
batch. Six fixed-key deletions require at most 84 new/retired tree nodes; the
object update adds at most eight, and canonical reduction can retire the sole
extent leaf. Boundary trimming uses a fixed-key update rather than delete/insert.
Thus this shrink bound needs at most 92 new metadata blocks and 93 retired
metadata blocks plus 128 retired data blocks, within V=128 and D=256. Holes
require no data retirement. An eventual partial EOF is kept hidden
until zeroed on later growth, so discarded bytes cannot reappear. None/inline/tree
representation remains canonical. These transaction bounds do not limit total
file length or replace the separately admitted E/M profile.

Mutation staging uses the final 2 MiB of the already reserved 8 MiB scratch arena;
compile-time bounds separate it from admission and publication storage. Changed
claims, blocks and volume counts feed the existing publisher. Publication and
funded retirement fences need no further heap allocation. Global closure/bulk fallback
and the kernel-stack prerequisite remain limitations; this code is not linked into Caelum.

Results preserve the confirmed write prefix or resize length independently of
retirement maintenance and health. A fully confirmed request remains `COMPLETE`
with healthy `PENDING` retirement. If a later user batch fails, the request retains
its confirmed prefix/length and reports that batch's operation error or uncertainty;
it does not invent a maintenance failure. An uncertain batch may have committed
additional bytes or a shorter length and is not automatically retryable.
Creation sets `namespace_confirmed` only after both user-publication flushes.

### Live directory tokens

`pfs_view_directory_live_page` accepts a copied 64-byte token; pass an all-zero
token to start. Tokens bind the random writer nonce, volume, directory, local
change serial and tree position. They confer no authority. Listing checks held
LIST and pool health before binding/freshness. Wrong binding or malformed position
returns `INVALID`; a changed serial returns `CHANGED`. Both publish zero count
without advancing the token or publishing entries.

Confirmed creation, removal and rename/replacement update each affected directory
once with a fresh never-wrapping serial. A new or detached directory also receives
a fresh serial. Table capacity and counter headroom are checked before publication;
the table is reserved by admission and entries remain until object deletion or
writer close. Failed mutations, file data/length changes and maintenance do not
change these serials. Fresh writable opening generates a new nonce, invalidating
prior-instance tokens.


## Namespace changes and orphan lifetime

`pfs_view_create_directory` creates an empty directory under the parent's owner,
with no explicit grants. Like file creation, it accepts optional requested child
rights and reserves the returned view before publication. Returned creation views
have object scope: a file accepts file rights and a directory accepts directory
rights, plus supported administration rights. Creating a child does not itself
supply lookup or child authority.

`pfs_view_remove(parent, name, length, result)` needs `dir.remove`, independently
of child read/write rights. It removes a file or an empty directory; nonempty
directories return `PFS_NOT_EMPTY`, and the volume root cannot be removed.
`pfs_view_rename` accepts held source/destination parents and single component
names. It supports regular files in the same volume, requiring source `dir.remove`
and destination `dir.create`. Directory moves/replacements and volume crossing
return `PFS_UNSUPPORTED`. Both namespaces and object parent changes publish in
one transaction.

Replacement requires an explicit `replace=true` and destination `dir.replace`.
With `replace=false`, an existing distinct destination returns `PFS_EXISTS` even
if replacement authority is held. An absent destination needs no replacement
right. Same-parent/same-name rename validates the source and base rights, then
returns `COMPLETE` with `namespace_confirmed=false` and no publication. False
means not confirmed; an `UNKNOWN` result may already have changed the namespace
and must not be automatically retried.

Unlink and replacement atomically remove the naming entry, clear the displaced
object's parent and insert its orphan marker. The object remains in the ordinary
object index, and its data/metadata remain charged. Shared runtime entries keyed
by volume/object ID count views and active operations. Held files retain their
identity, current contents and granted read/write/resize authority; fresh path or
ID acquisition cannot find an orphan. `pfs_view_delegate` narrows a held view's
rights/scope without acquiring new policy, including for retained orphans. A
retained empty directory remains readable/listable under its rights but rejects
insertion with `PFS_DETACHED`. Recreating its former name gives a fresh identity.

Each view and distinct live identity is charged to the capped memory owner before
acquisition output or creation publication. On the current x86-64 build these
cost 160 bytes per view plus 88 bytes per distinct live identity; these are
implementation observations, not interface sizes or a guaranteed handle count.
Shared-entry lookup is linear in the number of live identities. Runtime references
retain no old generation, and their storage does not consume the reserved editor
arena.

`pfs_view_close(view, result)` consumes an accepted view and clears its pointer
before final cleanup. `released=true` remains true if cleanup fails; `PFS_BUSY`
leaves the view live with `released=false`. The result reports maintenance
completion/status and pool health separately. Healthy `PENDING` means deletion
finished durably and its retired storage remains to be reclaimed, not that deletion
needs another application mutation. A named-object close performs no cleanup and
reports `NONE`. A stopped instance only releases runtime state. Pool close requires
all volume handles/views/active operations to
end and never retries stopped cleanup.

After the last view/operation ends, cleanup removes at most six tail mappings or
six grants and at most 128 data blocks per publication. Grant cleanup and final
paired object/orphan removal use separate batches. The object and marker remain
paired until final deletion; every intermediate state is valid and resumable.
An unreferenced regular orphan with exactly one inline one-block mapping and no
object-specific grants combines the final data retirement with paired deletion,
without publishing an intermediate empty object. Eligibility is established
before edits; other shapes retain their existing path. The
[complete transaction proof](small-orphan-cleanup.md) covers metadata repair,
accounting and scratch for the combined path. Its volume-edit bounds remain
unchanged; rolling retirement uses the expanded admission/workspace envelope.
Empty sparse files can proceed directly to grant/final cleanup. Each batch reduces
remaining orphan work and may carry funded volume retirement into the next batch.
Final release completes all admitted data, grant and paired-record deletion without
another application mutation. Cleanup volume retirements charge recovery workspace;
user mutations,
including writes through retained orphan handles, charge ordinary workspace.
Last release and startup recovery use the reserved arena for tree reads and edits,
with no further heap allocation. Unexpected resource failure is an admission/editor
invariant failure and stops the instance; I/O/integrity failures
retain their documented phase-specific health and uncertainty.

Protected deletion headroom remains enforced against volume quota, metadata
profile and total pool promises. Ordinary growth cannot consume it. This enables
unlink and replacement even when ordinary growth is refused; retained victim
contents need not be reclaimed immediately. Logical admission does not reserve
physical host space for sparse images. Last release and startup can take many
bounded transactions. Real-host recovery after writeback error or an interrupted
mutating session with unknown backing history remains unqualified; simulated
durable restart does not establish that host precondition.
