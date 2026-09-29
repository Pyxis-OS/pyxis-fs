#ifndef PYXIS_FS_BUILD_H
#define PYXIS_FS_BUILD_H

#include <pyxis_fs/block.h>
#include <pyxis_fs/platform.h>
#include <pyxis_fs/record.h>

struct pfs_empty_volume {
  struct pfs_volume_id id;
  struct pfs_object_id root_object;
  struct pfs_name name;
  struct pfs_principal_id owner;
  uint64_t guarantee;
  uint64_t quota;
  bool guarantee_set;
  bool quota_set;
};

#define PFS_RESERVE_COW 1u
#define PFS_RESERVE_MIGRATION 2u
#define PFS_RESERVE_RECOVERY 4u

struct pfs_empty_spec {
  uint64_t block_count;
  struct pfs_pool_id pool;
  size_t volume_count;
  const struct pfs_empty_volume *volumes;
  unsigned reserve_set;
  uint64_t cow_reserve;
  uint64_t migration_reserve;
  uint64_t recovery_reserve;
};

/* Initialize to zero. A successful plan owns copied specifications, bookkeeping
 * and construction buffers charged to memory. Do not copy or modify a live plan.
 * Its memory owner outlives it. Public summaries and volumes are borrowed until
 * destroy; volume order matches the input. Creation performs no I/O. IDs must be
 * supplied from host strong randomness; actual owners are explicit nonzero IDs. */
struct pfs_empty_plan {
  struct pfs_allocation state;
  uint64_t block_count;
  struct pfs_pool_id pool;
  struct pfs_pool_root pool_root;
  size_t volume_count;
  const struct pfs_volume_record *volumes;
  uint64_t unpromised;
};

enum pfs_status pfs_empty_plan_create(struct pfs_memory *memory,
                                      const struct pfs_empty_spec *spec,
                                      struct pfs_empty_plan *plan);
/* Only a newly created, exclusively owned empty output is permitted. Writes
 * metadata and pool root, flushes, writes both generation-1 slots, then flushes.
 * No reads or allocations occur. Failure may leave partial output; retry is not
 * a recovery operation. The host separately flushes the parent directory. */
enum pfs_status pfs_empty_build(const struct pfs_empty_plan *plan,
                               const struct pfs_block_builder *builder);
/* Releases memory without I/O and clears the plan. Empty destruction succeeds. */
enum pfs_status pfs_empty_plan_destroy(struct pfs_empty_plan *plan);

#endif
