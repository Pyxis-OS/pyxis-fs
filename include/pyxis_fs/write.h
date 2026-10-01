/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_WRITE_H
#define PYXIS_FS_WRITE_H

#include <pyxis_fs/access.h>

struct pfs_write_options {
  uint64_t extent_limit;
  uint64_t metadata_limit;
};

enum pfs_writer_health {
  PFS_WRITER_READY,
  PFS_WRITER_READABLE_STOPPED,
  PFS_WRITER_ACCESS_STOPPED,
};

enum pfs_completion {
  PFS_COMPLETE,
  PFS_STOPPED,
  PFS_UNKNOWN,
};

enum pfs_maintenance_completion {
  PFS_MAINTENANCE_NONE,
  PFS_MAINTENANCE_COMPLETE,
  PFS_MAINTENANCE_STOPPED,
  PFS_MAINTENANCE_UNKNOWN,
};

struct pfs_write_result {
  enum pfs_completion completion;
  enum pfs_status operation_status;
  uint64_t confirmed_bytes;
  bool confirmed_length_valid;
  uint64_t confirmed_length;
  bool namespace_confirmed;
  enum pfs_maintenance_completion maintenance_completion;
  enum pfs_status maintenance_status;
  enum pfs_writer_health health;
};

struct pfs_writer_status {
  enum pfs_writer_health health;
  enum pfs_status failure;
  bool drain_pending;
  bool invariant_failure;
};

struct pfs_write_open_result {
  struct pfs_pool_diagnostic pool;
  struct pfs_writer_status writer;
  struct pfs_write_result recovery;
  uint64_t confirmed_generation;
  uint64_t required_recovery_blocks;
  uint64_t permanent_pool_blocks;
  uint64_t reserved_arena_bytes;
};

/* The adapter owns exclusive, healthy/durably recovered backing for the complete
 * lifetime. Callbacks are synchronous; no core callback may reenter the pool.
 * Neither valid cached bytes nor a successful flush certifies recovery history.
 * Explicit limits are required. Failure leaves pool empty; diagnostic preserves
 * confirmed startup cleanup and uncertainty. No repair or automatic retry. */
enum pfs_status pfs_pool_open_writer(struct pfs_pool *pool,
  const struct pfs_block_builder *backing, struct pfs_memory *memory,
  const struct pfs_write_options *options, struct pfs_write_open_result *diagnostic);

/* Copies in-memory status only; available while stopped. */
enum pfs_status pfs_pool_writer_status(const struct pfs_pool *pool,
  struct pfs_writer_status *out);

/* Volume handles confer no object authority. Ordinary acquisition still checks
 * the supplied trusted context and persistent grants. Diagnostics remain RO. */
enum pfs_status pfs_pool_volume_open(struct pfs_pool *pool,
  const struct pfs_volume_id *id, struct pfs_volume *volume);

/* Uses the held file.checkpoint or dir.checkpoint right, not lookup/read rights.
 * No-op after prior funded drains; stopped instances cannot retry to health. */
enum pfs_status pfs_view_checkpoint(struct pfs_view *view, struct pfs_write_result *result);

#endif
