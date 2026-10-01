/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_PLAN_H
#define PFS_PLAN_H

#include <pyxis_fs/platform.h>
#include <pyxis_fs/record.h>
#include <pyxis_fs/tree.h>

#define PFS_PLAN_VOLUME_NEW 128u
#define PFS_PLAN_VOLUME_RETIRED 256u
#define PFS_PLAN_CATALOG_PATH 8u
#define PFS_PLAN_SCRATCH_BYTES (8u * 1024u * 1024u)

struct pfs_plan_limits {
  uint64_t extents;
  uint64_t metadata;
  uint64_t pool_blocks;
  uint64_t records;
  uint64_t catalog_blocks;
  uint64_t permanent_pool;
  uint64_t recovery_blocks;
  uint64_t arena_bytes;
  uint64_t claims;
  uint64_t objects;
  uint64_t deltas;
};

/* Computes the accepted finite envelopes, not writable admission. It neither
 * validates retained states nor promises physical host backing or free space.
 * Output remains unchanged on failure. No E/M defaults are selected. */
enum pfs_status pfs_plan_limits(uint64_t blocks, uint64_t extents, uint64_t metadata,
                                 uint16_t volumes, struct pfs_plan_limits *out);

struct pfs_plan_arena {
  struct pfs_allocation allocation;
  struct pfs_plan_limits limits;
  struct pfs_allocation_record *maps[3];
  uint8_t *claims[3];
  uint8_t *directories;
  uint8_t *blocks;
  uint8_t *deltas;
  uint8_t *scratch;
};

/* Empty-initialized, serial, never copied. Reserves one capped allocation before
 * planning; all pointers expire at destroy. Maps/claims reserve the three retained
 * and candidate vectors. The caller assigns scratch to nonoverlapping editors.
 * Limits must be an unmodified successful pfs_plan_limits result. */
enum pfs_status pfs_plan_arena_create(struct pfs_memory *memory,
                                       const struct pfs_plan_limits *limits,
                                       struct pfs_plan_arena *arena);
enum pfs_status pfs_plan_arena_destroy(struct pfs_plan_arena *arena);

struct pfs_map_change {
  struct pfs_allocation_record before;
  struct pfs_allocation_record after;
};

/* Sorted, disjoint deltas replace exactly matching state/owner/birth/charge of
 * their source ranges. Input is a canonical complete map. Output may not alias
 * input/deltas; count is published only on success, output bytes are scratch on
 * failure. Both successful and failed calls leave source/deltas unchanged.
 * Protection/admission evidence for retire/free/claim transitions belongs to the
 * future publisher; this is a bounded private interval editor, not that proof. */
enum pfs_status pfs_plan_map_apply(const struct pfs_record_context *context,
  const struct pfs_allocation_record *base, size_t base_count,
  const struct pfs_map_change *changes, size_t change_count,
  struct pfs_allocation_record *out, size_t capacity, size_t *count);

struct pfs_reusable_range {
  uint64_t first;
  uint64_t count;
};

struct pfs_map_node {
  uint64_t block;
  uint64_t minimum;
  size_t first;
  uint16_t count;
  uint16_t level;
};

struct pfs_map_plan {
  struct pfs_reference root;
  uint64_t pool_root_block;
  uint64_t catalog_blocks[PFS_PLAN_CATALOG_PATH];
  size_t catalog_count;
  size_t record_count;
  size_t node_count;
  size_t allocation_count;
  const struct pfs_allocation_record *records;
  const struct pfs_map_node *nodes;
  const uint8_t *blocks;
};

/* Base already includes volume deltas, old pool/map/catalog retirement and
 * eligible frees; it has no live pool allocation born in this candidate.
 * Reusable ranges must independently have been proven free in both retained
 * states with runtime pins ended BEFORE this publication. A same-publication
 * free is not eligible. This function verifies sorted ranges and base-free
 * containment, but cannot establish their cross-state/lifetime provenance.
 *
 * Produces every map block, with all map/catalog/pool-root allocations accounted
 * for. Catalog/root IDs are reserved; caller encodes those blocks separately.
 * Does no I/O, heap allocation or publication. Uses maps[2], deltas and blocks;
 * invalidates a prior plan in this arena. Input must not alias those regions.
 * Output is empty on failure and owned by arena until next plan/destroy.
 * Catalog count is 0..8. Geometry and generation come from validated context. */
enum pfs_status pfs_plan_map_build(struct pfs_plan_arena *arena,
  const struct pfs_block_context *context,
  const struct pfs_allocation_record *base, size_t base_count,
  const struct pfs_reusable_range *reusable, size_t reusable_count,
  size_t catalog_count, struct pfs_map_plan *out);

#endif
