# Combined final cleanup of a small orphan

Scope accepted on 2026-10-02: an unreferenced, parentless non-root regular orphan
with exactly one inline one-block mapping and no object-specific grants. Select
this path from validated state before edits. Retire the whole data claim and
delete the object and orphan marker in one private candidate, without publishing
an intermediate empty object. Other shapes keep the existing bounded cleanup.
Grant lookup errors are funded failures, not permission to fall back.

This proof was reviewed against `87d2f20` before implementation. It applies the
existing editor, publisher and admission bounds; it changes none of their limits,
allocator design, reserve policy, memory ceiling or durability semantics.

## Complete transaction envelope

The inline removal requires no extent-tree edit. Sparse object deletion creates
and retires at most 14 nodes; six-entry orphan-index deletion at most 15, including
sibling repair and root collapse. These fixed-key deletions do not increase live
node counts. They edit independent indexes using the same reserved workspace.

| Resource | Combined maximum | Existing reservation |
| --- | ---: | ---: |
| New volume metadata blocks / claims | 29 | V = 128 |
| Retired volume metadata blocks / claims | 29 | D = 256 |
| Retired data blocks / claims | 1 | Within D |
| Total volume retirements | 30 | D = 256 |
| New pool/map/catalog/root blocks per publication | H | H |
| Retired pool/map/catalog/root blocks per publication | H | H |
| Combined-publication interval deltas | `3H + 59` | `8(H + 384)` slots of 128 bytes |
| Each drain's interval deltas | `3H + 30` | Same delta storage |
| Candidate live claims | `E − 1 + M + Pmax` | `E + M + Pmax` |

Preparation requires no pending volume drain. The combined publication can free
at most `2H` eligible pool retirements, retire `H` old pool nodes, retire 30 volume
blocks and allocate 29 replacements: at most `3H+59` interval deltas without
assuming adjacent changes merge. Each drain allocates/retires no volume blocks
and can free at most 30 volume retirements. Catalog replacement is at most eight
nodes, already included with the map and pool root in H.

The candidate removes old live claims before adding replacements. The entire
single data claim disappears; it produces no surviving fragments. E decreases
by one and actual metadata does not increase. Thus the unchanged canonical map
closure covers the combined publication and both drains:

```
Pcat = 4N − 2
Pmax = Pcat + H
K = 1 + 2(E + M + 256)
Rbase(H) = K + 2Pcat + 6H
S(H) = ceil(23(Rbase(H) + 20) / 21)
F(S(H)) + 9 <= H
```

H/S are the existing computed profile ceilings, not blocks to allocate as padding.
The whole-map publisher still accounts for every replacement and retirement,
including its own storage. The optimization removes a publication; it does not
replace that allocation strategy or defer its costs elsewhere.

## Accounting and funded resources

Mapping and object counts each decrease by one; directory and grant counts stay
unchanged. Live blocks change by `metadata_added − metadata_removed − 1`.
The finish pass updates namespace-node accounting from actual tree claims.
Orphan work T decreases by two: one data-block unit in the finish pass and one
object/marker unit in paired deletion. Both contributions must be charged once.
Cleanup volume retirements keep the recovery charge.

The permanent namespace promise B is monotone in object population O for fixed
directory population J. O decreases and J stays fixed; non-namespace metadata
and data cannot increase. Therefore `Aeff = A − Z + B`, effective M and quota
requirements cannot increase. This does not spend deletion headroom or borrow
migration reservations.

Preparation still selects 128 volume IDs and requires H+128 already-reusable
blocks; its policy is unchanged. Recovery reservation `3H+384` covers the overlap
of `2H` old pool debt, H new pool metadata, 29 replacement volume nodes and 30
volume retirements. Later selected recovery retirement occupancy is bounded by
`2H+30`. No allocation uses same-publication frees or blocks protected by either
retained state.

The combined publication and at most two drains consume three generations while
reducing T by two. Existing admission funds `g+3T`; the remaining requirement
`g+3+3(T−2)` is three generations smaller. Startup's initial retirement allowance
and intermediate-crash debt retain their existing funding.

## Memory, scratch and failures

The arena stays:

```
3*80*S + 3*96*(E+M+Pmax) + 48*objects
+ 4096*(H+128) + 128*8*(H+384) + 8 MiB
```

Map planning reuses delta storage for at most H node descriptors, H IDs and H
reusable ranges after applying interval deltas. Three existing map/claim vectors
are sufficient. The unchanged mutation structure and editor occupy the final
2 MiB of reserved scratch, the sealed publication the preceding 2 MiB, and
admission the lower 4 MiB. Existing compile-time size checks enforce these
disjoint regions. Sequential tree deletions reuse the same editor; no new arrays,
snapshots, heap allocations or state vectors are needed. The grant probe uses
the existing bounded tree cursor and read buffer.

There is no speculative multi-edit fallback: once this path is selected, a
subsequent failure follows the existing funded-failure path. Confirmed namespace
progress survives cleanup failure. Read errors stop access; replacement-write
and pre-slot-flush errors stop mutation while preserving readable confirmed
state; uncertain slot publication stops access. Final close consumes its accepted
handle even on failure; startup withholds the instance until recovery completes.
The common publisher retains both flush boundaries and both retained payloads.

Tests use independent contents and healthy-trace failure cuts, verify retained
payloads after replacement writes before slot publication, and exercise funded
completion without new memory. Publication counts and savings are measurement
observations, not exact correctness assertions. See the [maintained test contract](testing.md)
and [RAM comparison contract](ram-validation.md#small-comparison-contract).
