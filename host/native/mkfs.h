/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NATIVE_MKFS_H
#define PYXIS_FS_NATIVE_MKFS_H

#include "host.h"

struct mkfs_volume {
  const char *name;
  const char *source;
  struct pnf_volume record;
  uint64_t next_inode;
};

struct mkfs_context {
  struct native_image image;
  uint64_t next_block;
  int64_t created_ns;
  bool created_valid;
};

enum pnf_status mkfs_allocate(struct mkfs_context *context, uint64_t *block);
enum pnf_status mkfs_map_insert(struct mkfs_context *context,
                                uint64_t pointers[PNF_POINTER_COUNT],
                                uint64_t logical, uint64_t physical);
enum pnf_status mkfs_populate(struct mkfs_context *context,
                              struct mkfs_volume *volume);

#endif
