/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_WRITER_H
#define PFS_WRITER_H

#include "admit.h"
#include "edit.h"
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
  struct pfs_batch batch;
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
/* A reserved orphan sequence cannot refuse resources as ordinary admission. */
void pfs_writer_funded_failure(struct pfs_pool *pool, enum pfs_status status);

#endif
