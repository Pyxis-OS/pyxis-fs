#ifndef PYXIS_FS_BUILD_H
#define PYXIS_FS_BUILD_H

#include <pyxis_fs/block.h>
#include <pyxis_fs/platform.h>
#include <pyxis_fs/record.h>

/* Root is objects[0], a directory with an empty name and UINT32_MAX parent.
 * Every other parent precedes its child. Names are unique within each directory.
 * Object IDs and all persistent pool/volume IDs are supplied by the embedder. */
struct pfs_build_object {
  struct pfs_object_id id;
  uint32_t parent;
  struct pfs_name name;
  uint16_t kind;
  uint64_t file_length;
};

struct pfs_build_volume {
  struct pfs_volume_id id;
  struct pfs_object_id root_object;
  struct pfs_name name;
  struct pfs_principal_id owner;
  uint64_t guarantee;
  uint64_t quota;
  bool guarantee_set;
  bool quota_set;
  size_t object_count;
  const struct pfs_build_object *objects;
};

#define PFS_RESERVE_COW 1u
#define PFS_RESERVE_MIGRATION 2u
#define PFS_RESERVE_RECOVERY 4u

struct pfs_build_spec {
  uint64_t block_count;
  struct pfs_pool_id pool;
  size_t volume_count;
  const struct pfs_build_volume *volumes;
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
struct pfs_build_plan {
  struct pfs_allocation state;
  uint64_t block_count;
  struct pfs_pool_id pool;
  struct pfs_pool_root pool_root;
  size_t volume_count;
  const struct pfs_volume_record *volumes;
  uint64_t unpromised;
};

enum pfs_status pfs_build_plan_create(struct pfs_memory *memory,
                                      const struct pfs_build_spec *spec,
                                      struct pfs_build_plan *plan);
/* Synchronous callbacks borrow buffers only until return. Read transfers exactly
 * length bytes (at most 64 KiB) from the selected input object at offset. Validate
 * checks every source after all data has been copied, before slots are published.
 * Callback failures propagate unchanged. The context outlives the build call. */
struct pfs_build_source {
  void *context;
  enum pfs_status (*read)(void *context, size_t volume, size_t object,
                           uint64_t offset, void *buffer, size_t length);
  enum pfs_status (*validate)(void *context);
};

/* Only a newly created, exclusively owned empty output is permitted. Writes
 * metadata and streamed data, validates sources, flushes, publishes both
 * generation-1 slots, then flushes. No output reads or allocations occur.
 * NULL source is permitted only without nonempty files; a supplied source must
 * have both callbacks. Failure leaves partial output; retry is not recovery.
 * The host separately flushes the parent directory. */
enum pfs_status pfs_build(const struct pfs_build_plan *plan,
                           const struct pfs_block_builder *builder,
                           const struct pfs_build_source *source);
/* Releases memory without I/O and clears the plan. Empty destruction succeeds. */
enum pfs_status pfs_build_plan_destroy(struct pfs_build_plan *plan);

#endif
