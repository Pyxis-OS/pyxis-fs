#ifndef PYXIS_FS_POOL_H
#define PYXIS_FS_POOL_H

#include <pyxis_fs/block.h>
#include <pyxis_fs/platform.h>
#include <pyxis_fs/record.h>

#define PFS_POOL_NO_SELECTION UINT16_MAX

struct pfs_pool_candidate {
  enum pfs_status status;
  /* Descriptions are available only when status is PFS_OK. */
  struct pfs_superblock superblock;
  struct pfs_pool_root root;
};

struct pfs_pool_diagnostic {
  struct pfs_pool_candidate candidate[2];
  uint16_t selected;
  bool degraded;
  bool ambiguous;
};

/* Zero initialize handles. Reader, memory and their contexts are borrowed and
 * remain unchanged and alive until close. Handles are serial and never copied.
 * The adapter must keep the underlying image unchanged while the pool is open. */
struct pfs_pool {
  const struct pfs_block_reader *reader;
  struct pfs_memory *memory;
  struct pfs_allocation state;
};

/* Both slots are examined independently. Diagnostic is published even on media
 * failure; selected is NO_SELECTION then. Ambiguity returns CORRUPT with its
 * flag set. Success validates candidate envelopes/root nodes, not map ownership
 * or global accounting. Both valid root descriptions are retained until close.
 * On failure the handle is empty. Caller argument errors leave diagnostic alone. */
enum pfs_status pfs_pool_open(struct pfs_pool *pool,
                             const struct pfs_block_reader *reader,
                             struct pfs_memory *memory,
                             struct pfs_pool_diagnostic *diagnostic);
/* BUSY leaves the pool live while volume handles remain open. */
enum pfs_status pfs_pool_close(struct pfs_pool *pool);

/* Diagnostic access over an authorized image; this grants no object authority.
 * Output contains copied volume envelopes in unsigned name-byte order, including
 * unsupported volume features/target versions. Both catalogs must agree; all
 * consulted pool metadata has completed selected-generation allocation proof.
 * No object trees or global accounting are examined. Capacity below the recorded
 * count returns LIMIT. Output/count remain unchanged on any failure. Buffers,
 * count and handle must not overlap. No allocation survives this operation. */
enum pfs_status pfs_pool_diagnostic_volumes(struct pfs_pool *pool,
                                           struct pfs_volume_record *out,
                                           size_t capacity, size_t *count);

#endif
