#ifndef PYXIS_FS_READ_H
#define PYXIS_FS_READ_H

#include <pyxis_fs/pool.h>

#define PFS_ANCESTRY_MAX 256u
#define PFS_OBJECT_GRANTS_MAX 64u

/* Zero initialize; never copy a live handle. The unchanged selected pool and
 * its memory/reader remain alive until close. These APIs are diagnostic access
 * over an authorized image; object selectors do not confer policy authority.
 * Caller buffers, output controls, selectors and handles must be disjoint.
 * Copied metadata/counts are published only on success unless stated otherwise. */
struct pfs_volume {
  struct pfs_pool *pool;
  struct pfs_allocation state;
};

struct pfs_directory_cursor {
  struct pfs_volume *volume;
  struct pfs_allocation state;
};

enum pfs_status pfs_pool_diagnostic_volume_open(struct pfs_pool *pool,
  const struct pfs_volume_id *id, struct pfs_volume *volume);
/* BUSY leaves the handle unchanged while policy views/cursors retain it. */
enum pfs_status pfs_volume_close(struct pfs_volume *volume);
enum pfs_status pfs_volume_diagnostic_metadata(struct pfs_volume *volume,
  struct pfs_volume_record *out);
enum pfs_status pfs_volume_diagnostic_object(struct pfs_volume *volume,
  const struct pfs_object_id *id, struct pfs_object_record *out);
/* Relative UTF-8 name components separated by '/'. Empty path selects root;
 * leading/trailing '/', empty components, '.' and '..' are invalid.
 * The supplied root must itself have a validated chain to the volume root. */
enum pfs_status pfs_volume_diagnostic_resolve(struct pfs_volume *volume,
  const struct pfs_object_id *root, const uint8_t *path, size_t length,
  struct pfs_object_record *out);
/* Copied chain includes volume root and target. NULL/zero performs a validated
 * sizing query. Otherwise capacity below the chain count returns LIMIT.
 * Metadata outputs/count remain unchanged on failure. */
enum pfs_status pfs_volume_diagnostic_ancestry(struct pfs_volume *volume,
  const struct pfs_object_id *id, struct pfs_object_record *out,
  size_t capacity, size_t *count);
enum pfs_status pfs_volume_diagnostic_grants(struct pfs_volume *volume,
  const struct pfs_object_id *id, struct pfs_grant_record *out,
  size_t capacity, size_t *count);
/* Clamp at EOF. On a valid call count is set to the proved prefix, including
 * on operational/media failure. Bytes outside that prefix are unchanged.
 * Every published prefix has completed allocation proof closure. */
enum pfs_status pfs_volume_diagnostic_read(struct pfs_volume *volume,
  const struct pfs_object_id *id, uint64_t offset, void *buffer,
  size_t length, size_t *count);
enum pfs_status pfs_volume_diagnostic_directory_open(struct pfs_volume *volume,
  const struct pfs_object_id *id, struct pfs_directory_cursor *cursor);
/* Bounded page in unsigned name-byte order. Capacity must be nonzero. On
 * failure output/count/done and cursor position are unchanged; retry is valid.
 * The cursor retains its volume and selected generation until close. */
enum pfs_status pfs_directory_next(struct pfs_directory_cursor *cursor,
  struct pfs_dirent_record *out, size_t capacity, size_t *count, bool *done);
enum pfs_status pfs_directory_close(struct pfs_directory_cursor *cursor);

#endif
