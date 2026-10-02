# Incremental allocation-map publication

The writer first attempts topology-preserving map replacement. It retains the
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

Let J be the source map-node count, P the monotone set of source nodes replaced,
q its size and c the changed catalog-path union. Each pass starts from the
immutable input, reclaims eligible old cohorts, applies the volume edits, retires
exactly P plus the old pool root/catalog union, and claims the first q+c+1 IDs.
Unmarked map claims remain live. Candidate-born pool claims have identical birth,
owner and permanent charge, permitting canonical coalescing.

Comparing full canonical records with each source leaf adds changed leaves and
ancestors. It also adds adjacent leaves when a canonical record straddles an
original coverage endpoint. After accounting and seams stabilize, each maximal
dirty run of l leaves must hold r records with l <= r <= 46l; 46 derives from the
format's block/header/slot/record sizes. For the first failing run in key order,
add its clean predecessor, or successor if no predecessor exists, and all
ancestors, including across parents. Renew all accounting and seam closure.
Expansion does not guarantee success.

Each growth pass adds a previously unmarked source identity. There are at most
J growth passes and one final decision, shared by accounting, seams and
redistribution. A fitting run is partitioned into exactly its source leaf count;
ancestor levels and fanout stay unchanged. Child-first encoding preserves shared
references, while all q emitted replacements are reachable. No live padding or
same-publication reuse is allowed.

Global closure, exhausted overflow/underflow, or a local candidate failing the
record/resource admission guard chooses the legacy bulk builder. It starts again
from original logical inputs, retires the full old map, and uses the same fixed
ID prefix and publication generation. No backing writes precede the sealed,
admitted choice. I/O, integrity errors, invalid deltas and failure to obtain
already-guaranteed workspace are errors, never optimization misses. Ordinary
quota/profile/capacity refusal remains distinct from failed funded work.

## Funding and workspace

The existing carryover envelope is unchanged. J <= m = H-Cmax-1 bounds both source
and sealed candidates, so q+c+1 <= H. Replacements/retirements, two debt cohorts,
catalog union, reusable input capacity, deletion headroom and generation funding
retain their existing admission requirements. No additional application mutation
is needed to complete admitted fencing or orphan cleanup.

Removing pool storage leaves at most K canonical volume/free records. Restoring
at most Pcat+H live pool blocks and 2H retired pool blocks adds at most two
boundaries each: Rfinal <= K+2Pcat+6H = Rbase <= S. This argument applies to sparse
source topology independently of bulk packing. Candidate admission still checks
profile, charges, claims, orphan work and next-step funding before any write.

The existing delta region is partitioned into raw deltas 128(4H+3D+V), source
nodes 128m, worklist 8m, runs 32m, output descriptors 128H, IDs 8H and ranges 16H.
Checked offsets and compile-time descriptor sizes fit the original
1024(H+V+D) reservation. Regeneration reuses raw deltas without touching source
inputs. Bulk construction can overwrite the dead front of this region; its
reusable ranges remain outside that overwrite. The existing publication editor
supplies traversal, canonical re-encoding and record buffers before catalog
editing. Admission uses the lower scratch half; publication the third quarter;
mutation staging the final quarter. No extra allocation occurs after admission.

Both retained states, individually durable completed calls, the two-flush
protocol, pending debt, confirmed progress and sticky failure provenance are
unchanged. Physical-host space is not reserved by these logical guarantees.
The existing host recovery precondition and kernel-stack prerequisite remain.

## Diagnostics and limits

Private per-publication diagnostics report J, q, growth passes, largest addition,
redistribution additions, chosen path and fallback reasons. The comparison
aggregates these over preparation and measurement, including trailing maintenance
and final checkpoints. Fallback reason counts can overlap. It reports the last
exhausted run's l/r, emitted map nodes and local cost categories.
These aggregates do not establish the measurement-window local/bulk split.
The nearly constant fallback totals may reflect preparation, but the current
record cannot attribute them to that phase.

Optional reference counting reconstructs the bulk preclaim base for the same
immutable input and logical work, using only dead raw-delta storage. Canonical
stream counting does not overwrite the sealed map or node inventory. The reference
uses the current bulk builder's shared shape function, not the local final record
count. Local q may equal or exceed that reference: a local hit is not necessarily
an immediate write saving. No cost-based path selector is introduced.

Host timing observes the first backing read inside publication planning through
its first replacement-write callback. It includes adapter reads and optional
reference counting, and excludes work before the first planning read. Bounded
aggregates report its sum and maximum alongside full workload elapsed time; they
are not exact CPU-only planning timings or NVMe performance. There is no production
clock dependency. Source traversal and full map/claim validation remain
population-sized; global closure remains possible. General structural editing,
CPU/locality improvements and deployment qualification require separate tasks.

The current retirement-membership lookup scans all J source descriptors. Each
accounting pass performs it for J allocation-map claims, contributing O(J²) work
per pass and O(J³) over at most J growth passes plus the final decision. Sealing
also reconstructs claims with an O(J²) lookup contribution. These are calculated
worst-case lookup costs, not measured timings or a complete planning-cost bound;
source validation, canonical map editing and admission add other work.

A proposed focused follow-up would separate preparation and measurement plan
diagnostics and replace repeated linear membership searches with a block-sorted
source-node index. Its storage and construction must fit the existing reserved
workspace, preserving source topology and the single authoritative set of marks.
It would change neither placement nor closure, funding or durability policy.
This follow-up remains separate from the current implementation. Larger-map
qualification follows it: the matched histories reach only J=10 and cannot
establish how replacement size or planning time scales with unrelated population.
