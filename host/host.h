/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_HOST_H
#define PYXIS_FS_HOST_H

#include <pyxis_fs/platform.h>
#include <stdio.h>

struct host_image {
  int fd;
  int parent_fd;
  int error_number;
  const char *operation;
  uint64_t error_block;
  uint64_t bytes;
  bool created;
};

enum pfs_status host_memory_init(struct pfs_memory *memory, uint64_t limit);
enum pfs_status host_size_parse(const char *text, uint64_t *bytes);
enum pfs_status host_random_id(uint8_t bytes[PFS_ID_SIZE]);
void host_name_print(FILE *stream, const uint8_t *bytes, size_t length);
int host_exit_status(enum pfs_status status);
void host_error(const char *operation, enum pfs_status status,
                const struct host_image *image);

/* Each open initializes image, including on failure. Failed creation leaves its
 * new file in place; close never removes it. Path scratch is charged to memory. */
enum pfs_status host_image_open(struct host_image *image, struct pfs_memory *memory,
                                const char *path, struct pfs_block_reader *reader);
enum pfs_status host_image_create(struct host_image *image, struct pfs_memory *memory,
                                  const char *path, uint64_t bytes,
                                  struct pfs_block_builder *builder);
enum pfs_status host_image_publish(struct host_image *image);
enum pfs_status host_image_close(struct host_image *image);

#endif
