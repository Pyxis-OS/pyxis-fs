/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_WRITER_H
#define PFS_WRITER_H

#include "admit.h"
#include "edit.h"
#include "incremental_map.h"
#include "writer_access.h"

struct pfs_batch_block {
  uint64_t block;
  const uint8_t *data;
};

/* Private output of the trusted mutation editors, not a public transaction API.
 * Editors prove namespace/record semantics and preserve unchanged subtrees.
 * Claims removed here must exactly identify source claims (a data subrange may
 * split one); added claims identify all new reachable storage. Changed volume
 * summaries describe the same edits. All arrays are borrowed until finish.
 * prepare owns the serial operation until commit or abort; even failed commit
 * ends it. No application bytes are acknowledged by this private interface. */
struct pfs_batch {
  uint64_t generation;
  uint64_t available[PFS_PLAN_VOLUME_NEW];
  size_t available_count;
  struct pfs_admit_volume volume;
  const struct pfs_batch_block *blocks;
  size_t block_count;
  const struct check_claim *remove;
  size_t remove_count;
  const struct check_claim *add;
  size_t add_count;
  /* Cleanup volume retirements consume recovery, not ordinary workspace. */
  bool orphan_cleanup;
};

struct pfs_changed_directory {
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  uint64_t serial;
};
_Static_assert(sizeof(struct pfs_changed_directory) <= 48, "directory arena slot");

enum pfs_map_fallback {
  PFS_MAP_FALLBACK_GLOBAL = 1u,
  PFS_MAP_FALLBACK_OVERFLOW = 2u,
  PFS_MAP_FALLBACK_UNDERFLOW = 4u,
  PFS_MAP_FALLBACK_RESOURCE = 8u,
};

/* Private, bounded plan diagnostics. No clock or public policy depends on them.
 * closure_nodes counts the final local closure p; a local path retires p map
 * nodes and bulk retires source_nodes. replacement_nodes counts emitted nodes n,
 * including a selected virtual leaf. The optional bulk reference counts
 * the unchanged builder's preclaim shape. */
struct pfs_map_metrics {
  uint64_t generation;
  size_t source_nodes, closure_nodes, replacement_nodes, bulk_reference_nodes;
  size_t growth_passes, largest_addition, redistribution_additions;
  struct pfs_incremental_repair_metrics repair;
  struct pfs_incremental_split_metrics split;
  size_t failed_run_leaves, failed_run_records;
  unsigned fallback;
  bool local;
  bool split_selected;
};

struct pfs_writer {
  struct pfs_allocation allocation;
  struct pfs_pool *pool;
  const struct pfs_block_builder *backing;
  struct pfs_plan_arena arena;
  struct pfs_admit_state states[3];
  struct pfs_check_result opening;
  struct pfs_pool_diagnostic diagnostic;
  struct pfs_writer_status status;
  struct pfs_runtime_object *runtime_objects;
  enum pfs_status (*cleanup)(struct pfs_volume *volume,
    const struct pfs_object_id *id, struct pfs_write_result *result);
  /* Mode bridge remains linkable without the writable publisher. */
  enum pfs_status (*fence)(struct pfs_pool *pool, struct pfs_write_result *result);
  struct pfs_batch batch;
  /* Borrowed only during the active user/orphan publication; NULL for fences. */
  const struct pfs_batch *active_batch;
  struct pfs_map_metrics map_metrics;
  bool collect_map_metrics;
  bool map_planning;
  pfs_random_fn random;
  void *random_context;
  uint8_t nonce[PFS_ID_SIZE];
  uint64_t directory_serial;
  size_t changed_directories;
  bool prepared;
  bool publishing;
  size_t selected;
  uint64_t last_confirmed_generation;
};

enum pfs_status pfs_writer_prepare(struct pfs_pool *pool, struct pfs_batch **batch);
void pfs_writer_abort(struct pfs_pool *pool);
enum pfs_status pfs_writer_commit(struct pfs_pool *pool, struct pfs_batch *batch,
  struct pfs_write_result *result);
/* Called with the serial writer gate held. Settles volume debt without orphan
 * cleanup; preserves confirmed operation progress in the supplied result. */
enum pfs_status pfs_writer_fence(struct pfs_pool *pool, struct pfs_write_result *result);
/* A reserved orphan sequence cannot refuse resources as ordinary admission. */
void pfs_writer_funded_failure(struct pfs_pool *pool, enum pfs_status status);

#endif
