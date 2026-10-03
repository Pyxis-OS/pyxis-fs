/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NPFS_HOST_H
#define PYXIS_FS_NPFS_HOST_H

#include <pyxis_fs/npfs.h>
#include <stdio.h>

struct npfs_image {
  int fd;
  bool writable;
  bool degraded_header;
  uint64_t block_count;
  struct npfs_header header;
  struct npfs_control control;
  unsigned control_slot;
  char error[192];
};

/* New standalone regular-file pools only. Open validates headers/controls and
 * feature compatibility, but does not read possibly half-checkpointed volumes.
 * Committed journals require allow_committed; readonly ordinary opening refuses.
 * Image owns fd until close. File locks exclude cooperating tools, not outsiders. */
enum npfs_status npfs_image_open(struct npfs_image *image, const char *path,
                                  bool writable, bool allow_committed);
enum npfs_status npfs_image_close(struct npfs_image *image);
enum npfs_status npfs_read_blocks(struct npfs_image *image, uint64_t first,
                                   uint32_t count, void *bytes);
enum npfs_status npfs_write_blocks(struct npfs_image *image, uint64_t first,
                                    uint32_t count, const void *bytes);
enum npfs_status npfs_flush(struct npfs_image *image);
enum npfs_status npfs_read_volumes(struct npfs_image *image,
                                    struct npfs_volume volumes[NPFS_VOLUME_COUNT]);
enum npfs_status npfs_map_block(struct npfs_image *image,
                                 const uint64_t pointers[NPFS_POINTER_COUNT],
                                 uint64_t logical, uint64_t *physical);
/* A missing mapping succeeds with physical zero. Metadata callers must reject
 * holes where dense storage is required; regular-file readers synthesize zeros. */
enum npfs_status npfs_read_inode(struct npfs_image *image,
                                  const struct npfs_volume *volume, uint64_t number,
                                  struct npfs_inode *inode);
enum npfs_status npfs_read_file(struct npfs_image *image,
                                 const uint64_t pointers[NPFS_POINTER_COUNT],
                                 uint64_t size, uint64_t offset, void *bytes, size_t length);
enum npfs_status npfs_size_parse(const char *text, uint64_t *bytes);
enum npfs_status npfs_random_id(uint8_t id[NPFS_ID_SIZE]);
bool npfs_time_now(int64_t *nanoseconds);
void npfs_name_print(FILE *stream, const uint8_t *bytes, size_t length);
void npfs_id_print(FILE *stream, const uint8_t id[NPFS_ID_SIZE]);
int npfs_report(const char *operation, enum npfs_status status,
                  const struct npfs_image *image);

#endif
