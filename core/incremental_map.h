/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_INCREMENTAL_MAP_H
#define PFS_INCREMENTAL_MAP_H

#include "plan.h"
#include "edit.h"

#define PFS_INCREMENTAL_LEAF_RECORDS \
  ((PFS_BLOCK_SIZE - PFS_TREE_HEADER_SIZE) / \
   (PFS_ALLOCATION_RECORD_SIZE + PFS_TREE_SLOT_SIZE))

/* Overflow/underflow reasons mean the failing run has no clean neighbour. */
enum pfs_incremental_fallback {
  PFS_INCREMENTAL_GLOBAL = 1u,
  PFS_INCREMENTAL_OVERFLOW = 2u,
  PFS_INCREMENTAL_UNDERFLOW = 4u,
};

struct pfs_incremental_source {
  struct pfs_reference reference;
  uint64_t minimum;
  uint64_t end;
  size_t parent;
  size_t first_child;
  size_t first_record;
  size_t record_count;
  size_t replacement;
  uint16_t level;
  uint16_t child_count;
  bool marked;
};

struct pfs_incremental_run {
  size_t first_leaf;
  size_t leaf_count;
  size_t first_record;
  size_t record_count;
};

struct pfs_incremental_map {
  struct pfs_plan_arena *arena;
  struct pfs_map_change *changes;
  size_t change_capacity;
  struct pfs_incremental_source *source;
  size_t source_capacity;
  size_t source_count;
  size_t marked_count;
  size_t *leaves;
  size_t leaf_count;
  struct pfs_incremental_run *runs;
  size_t run_count;
  struct pfs_map_node *nodes;
  uint64_t *ids;
  struct pfs_reusable_range *ranges;
  const struct pfs_allocation_record *source_records;
  size_t source_record_count;
  struct pfs_block_context source_context;
  size_t growth_passes;
  size_t largest_addition;
  size_t redistribution_additions;
  size_t redistribution_leaves;
  size_t failed_run_leaves;
  size_t failed_run_records;
  unsigned fallback;
  bool loaded;
  bool stable;
};

/* Partitions only the existing delta region. Caller owns raw changes and the
 * H ascending reusable IDs/ranges; no allocation or reservation occurs here.
 * All planner pointers expire when bulk planning reuses deltas or arena dies. */
enum pfs_status pfs_incremental_map_init(struct pfs_plan_arena *arena,
  struct pfs_incremental_map *map);

/* Context names the selected source root at its original generation. The flat
 * source map is immutable, validated retained-state storage and remains borrowed
 * through encode. Reads validate tree identity, ancestry, exact child minima and
 * coverage, and equality with that flat map. Workspace borrows only path[0].
 * Failure is an actual source/read error, never an optimization fallback. */
enum pfs_status pfs_incremental_map_load(struct pfs_incremental_map *map,
  const struct pfs_block_reader *reader, const struct pfs_block_context *context,
  const struct pfs_allocation_record *source, size_t source_count,
  struct pfs_edit_workspace *workspace);

bool pfs_incremental_map_retired(const struct pfs_incremental_map *map,
  uint64_t block);

/* Candidate is a complete canonical map applied afresh from immutable source,
 * including exactly the CURRENT marked-node retirements and q+c+1 pool claims.
 * A growth result requires that accounting to be regenerated before calling
 * again. Marks never shrink. Final stable runs align with canonical boundaries.
 * Fallback reasons are inspected only after final renewed accounting. */
enum pfs_status pfs_incremental_map_close(struct pfs_incremental_map *map,
  const struct pfs_allocation_record *candidate, size_t count, bool *again);

/* Requires a stable local decision and its unchanged candidate. Uses the fixed
 * ascending ID prefix, encoding exactly q marked source positions. Unmarked
 * subtree references retain their birth. No I/O, allocation or publication.
 * Borrows workspace.path[0].tree and records[0], plus the caller's disjoint
 * one-block record buffer. Output is empty on failure. Internal node.first is
 * unused and zero; leaf node.first indexes candidate records. Other node fields
 * describe emitted inventory. Sealed bytes and inventory belong to the arena. */
enum pfs_status pfs_incremental_map_encode(struct pfs_incremental_map *map,
  const struct pfs_block_context *context,
  const struct pfs_allocation_record *candidate, size_t count,
  size_t catalog_count, struct pfs_edit_workspace *workspace,
  uint8_t record[PFS_BLOCK_SIZE], struct pfs_map_plan *out);

#endif
