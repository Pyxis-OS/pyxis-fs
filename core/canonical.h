/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_CANONICAL_H
#define PFS_CANONICAL_H

#include <pyxis_fs/tree.h>

/* Private writable profile: decode without relaxing read-only compatibility,
 * then require exact canonical bytes before a COW editor can preserve meaning. */
enum pfs_status pfs_canonical_record_validate(const uint8_t *data, size_t length,
  uint16_t kind, uint16_t level, const struct pfs_record_context *context);
enum pfs_status pfs_canonical_tree_validate(const uint8_t *data, size_t length,
  const struct pfs_tree_context *context, struct pfs_tree *out);
enum pfs_status pfs_canonical_superblock_validate(const uint8_t *data, size_t length,
  uint64_t blocks, uint64_t slot, struct pfs_superblock *out);
enum pfs_status pfs_canonical_pool_root_validate(const uint8_t *data, size_t length,
  const struct pfs_block_context *context, struct pfs_pool_root *out);

#endif
