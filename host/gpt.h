/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_HOST_GPT_H
#define PYXIS_FS_HOST_GPT_H

#include <pyxis_fs/platform.h>

enum host_gpt_copy_status {
  HOST_GPT_COPY_UNEXAMINED,
  HOST_GPT_COPY_VALID,
  HOST_GPT_COPY_ABSENT,
  HOST_GPT_COPY_INVALID,
  HOST_GPT_COPY_UNSUPPORTED,
  HOST_GPT_COPY_IO,
};

enum host_gpt_mbr_status {
  HOST_GPT_MBR_UNEXAMINED,
  HOST_GPT_MBR_VALID,
  HOST_GPT_MBR_ABSENT,
  HOST_GPT_MBR_INVALID,
  HOST_GPT_MBR_UNSUPPORTED,
  HOST_GPT_MBR_IO,
};

enum host_gpt_status {
  HOST_GPT_UNEXAMINED,
  HOST_GPT_HEALTHY,
  HOST_GPT_DEGRADED,
  HOST_GPT_AMBIGUOUS,
  HOST_GPT_ABSENT,
  HOST_GPT_INVALID,
  HOST_GPT_UNSUPPORTED,
  HOST_GPT_LIMIT,
  HOST_GPT_NO_MEMORY,
  HOST_GPT_IO,
};

struct host_gpt_selection {
  uint32_t partition_entry;
  uint32_t sector_size;
};

/* GPT discovery health is independent of pool selection. A selected GPT copy
 * can remain recorded when the requested entry is unused or outside the pool
 * size profile. Copy numbers are 1 primary, 2 backup, and 0 unselected. */
struct host_gpt_diagnostic {
  enum host_gpt_status status;
  enum host_gpt_mbr_status mbr;
  enum host_gpt_copy_status primary;
  enum host_gpt_copy_status backup;
  uint32_t selected_copy;
  bool degraded;
  uint64_t offset_bytes;
  uint64_t length_bytes;
};

const char *host_gpt_status_string(enum host_gpt_status status);
const char *host_gpt_copy_status_string(enum host_gpt_copy_status status);
const char *host_gpt_mbr_status_string(enum host_gpt_mbr_status status);

struct host_image;
/* Internal host-container interface. The image is already locked read-only;
 * scratch is charged to memory and released before return. No fd ownership is
 * transferred. Diagnostic is initialized even on failure. */
enum pfs_status host_gpt_select(struct host_image *image, struct pfs_memory *memory,
                                const struct host_gpt_selection *selection,
                                struct host_gpt_diagnostic *diagnostic);

#endif
