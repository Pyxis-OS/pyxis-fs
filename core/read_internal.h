#ifndef PFS_READ_INTERNAL_H
#define PFS_READ_INTERNAL_H

#include <pyxis_fs/read.h>

/* Ordinary calls hold the writer gate before using these private read helpers. */
enum pfs_status pfs_volume_begin(struct pfs_volume *volume);

/* Internal lifetime retention only; these helpers confer no object rights. */
enum pfs_status pfs_volume_hold(struct pfs_volume *volume);
enum pfs_status pfs_volume_drop(struct pfs_volume *volume);
uint64_t pfs_volume_generation(const struct pfs_volume *volume);
const struct pfs_pool_id *pfs_volume_pool_id(const struct pfs_volume *volume);

/* Internal directory paging over an already authorized volume. The policy-view
 * wrapper checks LIST before calling. No state or volume retention escapes. */
enum pfs_status pfs_volume_directory_page(struct pfs_volume *volume,
  const struct pfs_object_id *id, uint64_t after, struct pfs_dirent_record *out,
  size_t capacity, size_t *count, bool *done, uint64_t *next);

enum pfs_status pfs_volume_metadata(struct pfs_volume *volume, struct pfs_volume_record *out);
enum pfs_status pfs_volume_ancestry(struct pfs_volume *volume, const struct pfs_object_id *id,
  struct pfs_object_record *out, size_t capacity, size_t *count);
enum pfs_status pfs_volume_object(struct pfs_volume *volume, const struct pfs_object_id *id,
  struct pfs_object_record *out);
enum pfs_status pfs_volume_resolve(struct pfs_volume *volume, const struct pfs_object_id *root,
  const uint8_t *path, size_t length, struct pfs_object_record *out);
enum pfs_status pfs_volume_grants(struct pfs_volume *volume, const struct pfs_object_id *id,
  struct pfs_grant_record *out, size_t capacity, size_t *count);
enum pfs_status pfs_volume_read(struct pfs_volume *volume, const struct pfs_object_id *id,
  uint64_t offset, void *buffer, size_t length, size_t *count);

#endif
