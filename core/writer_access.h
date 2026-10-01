/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_WRITER_ACCESS_H
#define PFS_WRITER_ACCESS_H

#include <pyxis_fs/write.h>

/* Private bridge between ordinary views and serialized writer state. Read-only
 * pools pass begin/end without retaining anything. */
enum pfs_status pfs_writer_begin(struct pfs_pool *pool, bool mutation);
enum pfs_status pfs_writer_lifetime_begin(struct pfs_pool *pool);
void pfs_writer_end(struct pfs_pool *pool, enum pfs_status status);
bool pfs_writer_active(const struct pfs_pool *pool);
void pfs_writer_dispose(struct pfs_pool *pool);
const struct pfs_pool_diagnostic *pfs_writer_diagnostic(const struct pfs_pool *pool);
const struct pfs_volume_record *pfs_writer_volume(const struct pfs_pool *pool,
  const struct pfs_volume_id *id, uint64_t *birth);
enum pfs_status pfs_writer_checkpoint(struct pfs_pool *pool, struct pfs_write_result *result);

/* Shared live identity references do not pin any committed generation. Entry
 * storage is charged to pool memory, separate from the admitted mutation arena. */
struct pfs_runtime_object {
  struct pfs_allocation allocation;
  struct pfs_runtime_object *next;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  size_t views;
  size_t operations;
};
bool pfs_runtime_references(const struct pfs_pool *pool,
  const struct pfs_volume_id *volume, const struct pfs_object_id *object);
enum pfs_status pfs_runtime_hold(struct pfs_pool *pool,
  const struct pfs_volume_id *volume, const struct pfs_object_id *object,
  struct pfs_runtime_object **out);
void pfs_runtime_drop(struct pfs_pool *pool, struct pfs_runtime_object *object);

#endif
