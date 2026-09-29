/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_SOURCE_H
#define PYXIS_FS_SOURCE_H

#include "host.h"
#include <pyxis_fs/build.h>

struct host_source {
  struct pfs_memory *memory;
  struct pfs_allocation volumes;
  struct pfs_allocation workspace;
  struct pfs_allocation reserved_ids;
  size_t count;
  size_t object_count;
  const char *operation;
  int error_number;
  size_t error_volume;
  size_t error_object;
  int file_fd;
  size_t file_volume;
  size_t file_object;
};

/* Zero-initialize. Retains source roots and charged manifests until close.
 * Volume object pointers borrow those manifests until close. */
enum pfs_status host_source_open(struct host_source *source, struct pfs_memory *memory,
                                  struct pfs_build_volume *volumes,
                                  const char *const *paths, size_t count,
                                  const struct pfs_pool_id *pool);
enum pfs_status host_source_validate(void *context);
enum pfs_status host_source_exclude_output(struct host_source *source, int parent_fd);
enum pfs_status host_source_read(void *context, size_t volume, size_t object,
                                  uint64_t offset, void *buffer, size_t length);
void host_source_error(const struct host_source *source);
enum pfs_status host_source_close(struct host_source *source);

#endif
