/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NATIVE_HOST_H
#define PYXIS_FS_NATIVE_HOST_H

#include <pyxis_fs/native.h>
#include <stdio.h>

struct native_image {
  int fd;
  bool writable;
  bool degraded_header;
  uint64_t block_count;
  struct pnf_header header;
  struct pnf_control control;
  unsigned control_slot;
  char error[192];
};

/* New standalone regular-file pools only. Open validates headers/controls and
 * feature compatibility, but does not read possibly half-checkpointed volumes.
 * Committed journals require allow_committed; readonly ordinary opening refuses.
 * Image owns fd until close. File locks exclude cooperating tools, not outsiders. */
enum pnf_status native_image_open(struct native_image *image, const char *path,
                                  bool writable, bool allow_committed);
enum pnf_status native_image_close(struct native_image *image);
enum pnf_status native_read_blocks(struct native_image *image, uint64_t first,
                                   uint32_t count, void *bytes);
enum pnf_status native_write_blocks(struct native_image *image, uint64_t first,
                                    uint32_t count, const void *bytes);
enum pnf_status native_flush(struct native_image *image);
enum pnf_status native_read_volumes(struct native_image *image,
                                    struct pnf_volume volumes[PNF_VOLUME_COUNT]);
enum pnf_status native_map_block(struct native_image *image,
                                 const uint64_t pointers[PNF_POINTER_COUNT],
                                 uint64_t logical, uint64_t *physical);
/* A missing mapping succeeds with physical zero. Metadata callers must reject
 * holes where dense storage is required; regular-file readers synthesize zeros. */
enum pnf_status native_read_inode(struct native_image *image,
                                  const struct pnf_volume *volume, uint64_t number,
                                  struct pnf_inode *inode);
enum pnf_status native_read_file(struct native_image *image,
                                 const uint64_t pointers[PNF_POINTER_COUNT],
                                 uint64_t size, uint64_t offset, void *bytes, size_t length);
enum pnf_status native_size_parse(const char *text, uint64_t *bytes);
enum pnf_status native_random_id(uint8_t id[PNF_ID_SIZE]);
bool native_time_now(int64_t *nanoseconds);
void native_name_print(FILE *stream, const uint8_t *bytes, size_t length);
void native_id_print(FILE *stream, const uint8_t id[PNF_ID_SIZE]);
int native_report(const char *operation, enum pnf_status status,
                  const struct native_image *image);

#endif
