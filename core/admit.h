/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_ADMIT_H
#define PFS_ADMIT_H

#include "check_internal.h"
#include "plan.h"

struct pfs_admit_volume {
  struct pfs_volume_record record;
  uint64_t directories;
  uint64_t namespace_nodes;
  uint64_t metadata_blocks;
  uint64_t file_extents;
  uint64_t grants;
  uint64_t orphan_work;
  uint64_t deletion_blocks;
  uint64_t effective_blocks;
};

struct pfs_admit_state {
  struct pfs_pool_candidate candidate;
  struct pfs_allocation_record *maps;
  size_t map_count;
  struct check_claim *claims;
  size_t claim_count;
  struct pfs_admit_volume volumes[PFS_VOLUME_MAX];
  size_t volume_count;
  uint64_t file_extents;
  uint64_t metadata_blocks;
  uint64_t map_nodes;
  uint64_t live_pool;
  uint64_t retired_pool;
  uint64_t retired_volume;
  uint64_t protected_pool;
  uint64_t protected_volume;
  uint64_t reusable_blocks;
};

/* Summary storage belongs to the caller and must be charged to memory. The
 * empty arena is reserved after both-state structural proof, before summaries
 * are copied. Temporary checker storage shares memory and is released on every
 * return. Opening reads metadata only, performs no writes and refuses retained
 * orphans until orphan recovery is implemented. On failure arena and summaries
 * are empty; check diagnostics retain the completed validation result.
 * Successful vectors occupy arena slots 0/1; empty slot 2 is the candidate. */
enum pfs_status pfs_admit_open(const struct pfs_block_reader *reader,
  struct pfs_memory *memory, uint64_t extents, uint64_t metadata,
  struct pfs_plan_arena *arena, struct pfs_admit_state states[3],
  struct pfs_check_result *result);

/* Reconciles sorted maps/claims and checks profile, deletion promises, workspace,
 * retained protection and generation headroom without I/O or allocation. Uses
 * arena scratch. The caller proves
 * namespace/object/grant deltas through the private editors; this does not walk
 * unchanged trees or manufacture semantic proof from aggregate counts.
 * older is the previous selected state, retained alongside this candidate.
 * Generation advances exactly once. A user candidate reserves its two remaining
 * drain generations; maintenance/opening reserve remaining drains. */
enum pfs_status pfs_admit_candidate(struct pfs_plan_arena *arena,
  struct pfs_admit_state *state, const struct pfs_admit_state *older,
  bool user_batch);

/* Enumerates ranges free in the selected map and free or historically retired
 * in the older map, without either state's live claims. Both summaries must
 * describe completely validated durable states. Runtime operation/I/O pins must
 * independently have ended. A private candidate's same-publication free is not
 * eligible for allocation. Output/count publish only on success;
 * ranges are scratch on failure. Does no I/O or allocation. */
enum pfs_status pfs_admit_reusable(const struct pfs_admit_state *first,
  const struct pfs_admit_state *second, struct pfs_reusable_range *ranges,
  size_t capacity, size_t *count);

#endif
