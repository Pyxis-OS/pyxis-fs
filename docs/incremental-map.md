# Incremental allocation-map publication

The writer first attempts local map replacement, including one bounded
leaf-overflow split. It retains the
current disk format, canonical partition and maximal record coalescing. Unchanged
subtrees keep their references and births. General splits/merges, placement
heuristics and changes to bulk fill or admission policy are outside this step.
Whole-map construction remains an explicit funded fallback, including when
self-accounting reaches every source node. This is an intermediate improvement;
it does not establish disk-deployment readiness.

## Closure and accounting

Before closure, the publisher validates the source topology against its admitted
flat map and reserves the existing ascending list of H reusable input IDs,
excluding the batch's new volume blocks. Both retained maps and live claims
establish eligibility. The list remains fixed; blocks declared free by this
publication cannot enter it.

Let J be the source map-node count, P the set of source nodes replaced,
p its size, n the emitted map-node count and c the changed catalog-path union.
Ordinary replacement emits n=p; the one-leaf trial emits n=p+1. Each pass starts from the
immutable input, reclaims eligible old cohorts, applies the volume edits, retires
exactly P plus the old pool root/catalog union, and claims the first n+c+1 IDs.
Unmarked map claims remain live. Candidate-born pool claims have identical birth,
owner and permanent charge, permitting canonical coalescing.

Comparing full canonical records with each source leaf adds changed leaves and
ancestors. It also adds adjacent leaves when a canonical record straddles an
original coverage endpoint. After accounting and seams stabilize, each maximal
dirty run of l leaves must hold r records with l <= r <= 46l; 46 derives from the
format's block/header/slot/record sizes. Before any neighbour expansion, the first
failing run can offer one split trial if it overflows. Require a non-leaf root,
non-global closure, J+1<=m and an existing parent that can encode one more child.
Choose the lowest-key eligible leaf in that run, freeze its source identity and
parent, and save the seed marks/planner state. Regenerate the candidate with the
fixed surplus of one; do not perform neighbour expansion during the trial.

Renew source-retirement and canonical seam closure until stable. The current run
containing the anchor must satisfy 46l<r<=46(l+1); all other runs must fit their
original capacities. An unnecessary/insufficient split, another failing run or
global closure abandons the trial. Restore the seed with further trials disabled,
regenerate all accounting, and resume ordinary repair. A final candidate-local
admission resource miss follows the same restoration, including mutable volume,
claim and catalog state that sealing changed. Read/integrity/encoding and
guaranteed-storage failures are errors, never misses.

For ordinary repair of the first failing run in key order,
score both available immediate clean neighbours. An expansion includes that
neighbour and any already-marked run joined across its one-leaf gap. Its score
is the record distance outside the expanded run's interval
`[expanded_leaves, 46*expanded_leaves]`; a fit scores zero. Prefer the lower
deficit, with a left tie. Both fitting alternatives therefore retain the left
preference even if the right offers more spare slots. This is the accepted
intermediate repair policy, not a new physical placement rule.

Evaluate both alternatives before marking. At this point, the unchanged-leaf
comparison establishes that each clean neighbour has exactly its source record
count in the validated candidate, and canonical seams align. The adjoining
counts therefore add without hidden coalescing. Score the whole expanded run:
bridging can reverse the failure direction, so a neighbour-only score is wrong.
Mark only the chosen clean leaf and all ancestors, including across parents,
then renew accounting and seam closure. A predicted fit cannot seal directly,
and expansion does not guarantee success.

Each growth pass adds a previously unmarked source identity. Marks are monotone
within initial closure, the trial and restored repair; they may shrink only at
the single seed restoration. For seed size s, at most s initial growth evaluations,
J-s trial growth and J-s restored growth, plus three terminal evaluations, give
at most 2J-s+3 closure evaluations. No guessed-count search or arbitrary retry
limit is used. Ordinary neighbour selection reads adjacent run descriptors and scalar
counts, without scanning farther neighbours, speculative marks or another map.
Expanded leaf counts never exceed the source leaf inventory. Within either
expansion, its component intervals are disjoint, and its total record count
never exceeds the validated candidate count. The existing
admitted record/node bounds cover their arithmetic. A fitting run is
partitioned into exactly its source leaf count, or l+1 in the anchor run of a
successful trial, using balanced packing. Only the frozen parent gains a child;
all levels and other fanout remain unchanged. The virtual leaf's output index is
p and its physical ID is ids[p], but its logical position is immediately after
the anchor's replacement. Encode it before reverse source traversal reaches its
parent. Child-first encoding preserves shared references, while all n emitted
replacements are reachable. Catalog/root offsets use n, not p; the virtual leaf
never joins the immutable source/retirement index. No live padding or
same-publication reuse is allowed.

After any trial restoration, global closure, exhausted overflow/underflow, or a
split-disabled local candidate failing the
record/resource admission guard chooses the legacy bulk builder. It starts again
from original logical inputs, retires the full old map, and uses the same fixed
ID prefix and publication generation. No backing writes precede the sealed,
admitted choice. I/O, integrity errors, invalid deltas and failure to obtain
already-guaranteed workspace are errors, never optimization misses. Ordinary
quota/profile/capacity refusal remains distinct from failed funded work.

## Funding and workspace

The existing carryover envelope is unchanged. J <= m = H-Cmax-1 bounds the source;
the trial additionally requires J+1<=m, so n+c+1<=H and p+c+1<=H. A successful
split leaves J+1 live map nodes; skipping the trial at the cap does not refuse
the mutation. Replacements/retirements, two debt cohorts,
catalog union, reusable input capacity, deletion headroom and generation funding
retain their existing admission requirements. No additional application mutation
is needed to complete admitted fencing or orphan cleanup.

Removing pool storage leaves at most K canonical volume/free records. Restoring
at most Pcat+H live pool blocks and 2H retired pool blocks adds at most two
boundaries each: Rfinal <= K+2Pcat+6H = Rbase <= S. This argument applies to sparse
source topology independently of bulk packing. Candidate admission still checks
profile, charges, claims, orphan work and next-step funding before any write.

The existing delta region is partitioned into raw deltas 128(4H+3D+V), source
nodes 128m, block-sorted descriptor index 8m, worklist 8m, runs 32m, output
descriptors 128H, IDs 8H and ranges 16H, then seed marks 8m, a 128-byte split
slot and a 256-byte rollback header.
Checked offsets and compile-time descriptor sizes fit the original
1024(H+V+D) reservation. Regeneration reuses raw deltas without touching source
inputs. Bulk construction can overwrite the dead front of this region; its
reusable ranges remain outside that overwrite. The existing publication editor
supplies traversal, canonical re-encoding and record buffers before catalog
editing. Admission uses the lower scratch half; publication the third quarter;
mutation staging the final quarter. No extra allocation occurs after admission.
Neighbour scoring adds only constant local scalars. Fixed diagnostic
counters remain in the existing publication scratch quarter and private writer
header, whose size/charged allocation is computed normally. The publication
size assertion still prevents overlap; no arena allocation, memory ceiling,
caller budget, reserve or admission bound increases.

The index is constructed once after complete source validation, using an in-place
heapsort with constant extra storage. It stores descriptor identities rather than
copies of marks, preserving source topology and its authoritative replacement set.
The complete layout is 664H+184m+384D+128V+384 bytes, at most
848H+384D+128V+384 within the existing 1024(H+V+D) reservation for computed
profiles (H>=4). Checked subregion
offsets still establish nonoverlapping lifetimes; bulk planning discards the index
with the other provisional descriptors. Existing source-load duplicate detection
is unchanged, and an incomplete load exposes no retirement membership.

Both retained states, individually durable completed calls, the two-flush
protocol, pending debt, confirmed progress and sticky failure provenance are
unchanged. Physical-host space is not reserved by these logical guarantees.
The existing host recovery precondition and kernel-stack prerequisite remain.

## Diagnostics and limits

Private per-publication diagnostics report J, p, n, growth passes, largest addition,
redistribution additions, chosen path and fallback reasons. The comparison
keeps separate phase aggregates, including each phase's
trailing maintenance and final checkpoints. Fallback reason counts can overlap.
It reports each phase's last exhausted run's l/r, actual retired map nodes
(p for local, J for bulk), emitted map nodes and local cost
categories. Earlier combined records cannot establish measurement-window hit rates.

Repair diagnostics count underflow/overflow selections, two-sided comparisons,
right preferences within those comparisons, ties and predicted fitting choices.
Left/right bridge counts describe evaluated alternatives, including unchosen
ones. Left/right deficit sums cover only two-sided comparisons; chosen deficit
sum and fitting cover every selected repair, including one-sided boundary cases.
The scores precede renewed accounting. All repair counters can include attempts
later discarded for funded bulk construction; they are not final leaf occupancy,
successful local repairs or eliminated writes. Per-publication selection counts
are bounded by J and deficit sums by J times the admitted candidate-record
bound, using 64-bit diagnostic accumulators. Split diagnostics separately report
opportunities, trials, admitted selections, skip/miss reasons, all-phase closure
evaluations and discarded trial source marks/growth. Trial observations survive
restoration; chosen-path repair observations restore with the seed. Discarded
marks are planning work, not durable retirements or submitted writes. No policy
depends on the counters.

To retain all fields within the unchanged bounded output budget, `map_plans`
serializes one set of field names with `phase_order` set to
`["preparation", "measurement"]` for the small baseline. The sustained comparison
uses setup, preparation, successive windows and final maintenance in the same
phase-indexed representation. Scalar fields contain one value per phase;
array fields contain one array per phase. Combined totals can be derived by adding
count/sum fields and taking maxima for maximum fields; use the last failed run
from the last applicable phase. The comparison checks publication phases'
observed publications against actual fixed-slot writes and two flushes
per publication, and flush counters against the adapter's monotonic ordinals.
It does not require a particular number of publications or allocation choices.
Sustained setup separately counts formatting/open writes and flushes; it is not
subject to the per-publication count relation.

Optional reference counting reconstructs the bulk preclaim base for the same
immutable input and logical work, using only dead raw-delta storage. Canonical
stream counting does not overwrite the sealed map or node inventory. The reference
uses the current bulk builder's shared shape function, not the local final record
count. Local n may equal or exceed that reference: a local hit is not necessarily
an immediate write saving. No cost-based path selector is introduced.

Host timing observes the first backing read inside publication planning through
its first replacement-write callback. It includes adapter reads and optional
reference counting, and excludes work before the first planning read. Bounded
aggregates report its sum and maximum alongside full workload elapsed time; they
are not exact CPU-only planning timings or NVMe performance. There is no production
clock dependency. Failed trials and restored repair remain inside that interval.
Source traversal and full map/claim validation remain
population-sized; global closure remains possible. Each selected split adds one
live map node, so later replacement/reclamation may cost more and eventually
reach m or fill parents. There is no local worst-case replacement bound or promised
complete-history saving. General structural editing,
CPU/locality improvements and deployment qualification require separate tasks.

Retirement membership now uses O(log J) binary search through the sorted index.
Index construction costs O(J log J), accounting membership O(J log J) per pass and
O(J² log J) over the derived bounded phases; sealing adds
O(J log J) lookup work. This replaces the previous O(J³) repeated-lookup
contribution. These are calculated costs, not measured timings or a complete
planning bound. Source-load duplicate detection remains O(J²) once; full source
validation, canonical editing and admission retain other population-sized costs.
The original matched histories reach only J=10 and cannot establish scaling with
unrelated population. The separately assigned sustained populated-map comparison
adds larger histories; it does not establish a local worst-case bound or settle
larger-profile, pressure and writable-deployment qualification.
The [matched planning report](map-planning-measurements.md) records unchanged
submitted bytes, newly separated fallback costs and modest instrumented timing
increases; it establishes no speedup on these small maps.
