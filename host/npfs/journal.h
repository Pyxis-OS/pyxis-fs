/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NPFS_JOURNAL_H
#define PYXIS_FS_NPFS_JOURNAL_H

#include "host.h"

struct npfs_journal {
  size_t count;
  struct npfs_descriptor *descriptors;
  uint8_t *images;
};

/* No source writes. Requires validated open headers/features/control and no
 * active overlay. Shares fsck payload validation; publishes owned buffers only
 * on success. Writable/counter admission belongs to the checkpoint caller. */
enum npfs_status npfs_journal_load(struct npfs_image *image, struct npfs_journal *journal);
void npfs_journal_free(struct npfs_journal *journal);
/* Exclusive writable opening. Validate before home writes and durable clearing. */
enum npfs_status npfs_replay(struct npfs_image *image);
/* Readonly opening. Retain a complete validated overlay until image close,
 * and expose an EMPTY logical control without changing the source control. */
enum npfs_status npfs_memory_replay(struct npfs_image *image);
const uint8_t *npfs_overlay_block(const struct npfs_overlay *overlay, uint64_t home);
void npfs_overlay_free(struct npfs_overlay *overlay);

#endif
