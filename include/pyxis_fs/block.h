#ifndef PYXIS_FS_BLOCK_H
#define PYXIS_FS_BLOCK_H

#include <pyxis_fs/base.h>

struct pfs_block_header {
  uint16_t type;
  uint16_t version;
  uint32_t used;
  struct pfs_pool_id pool;
  uint64_t block;
  uint64_t birth;
};

struct pfs_block_context {
  uint64_t block_count;
  uint64_t selected_generation;
  uint64_t referring_birth;
  struct pfs_pool_id pool;
  struct pfs_reference reference;
  struct pfs_features features;
};

struct pfs_superblock {
  struct pfs_block_header header;
  uint64_t block_count;
  struct pfs_features features;
  struct pfs_reference root;
};

struct pfs_budget {
  uint64_t capacity;
  uint64_t occupied;
};

struct pfs_pool_root {
  struct pfs_block_header header;
  struct pfs_reference volumes;
  struct pfs_reference volume_names;
  struct pfs_reference allocation;
  uint64_t volume_count;
  uint64_t live_pool;
  uint64_t live_volume;
  uint64_t retired;
  uint64_t free;
  struct pfs_budget cow;
  struct pfs_budget migration;
  struct pfs_budget recovery;
};

/* Outputs are copied only on success. Input/output objects must not overlap.
 * These functions validate one block, not a candidate state or allocation proof.
 * Encoders construct initial-version blocks and refuse unknown feature semantics. */
enum pfs_status pfs_block_header_decode(const uint8_t *data, size_t length,
                                       uint64_t blocks, uint64_t physical_block,
                                       uint64_t generation, struct pfs_block_header *out);
enum pfs_status pfs_block_match(const struct pfs_block_header *header,
                               const struct pfs_block_context *context, uint16_t type);
enum pfs_status pfs_superblock_decode(const uint8_t *data, size_t length,
                                     uint64_t blocks, uint64_t slot,
                                     struct pfs_superblock *out);
enum pfs_status pfs_superblock_encode(uint8_t *data, size_t capacity,
                                     const struct pfs_superblock *value);
enum pfs_status pfs_pool_root_decode(const uint8_t *data, size_t length,
                                    const struct pfs_block_context *context,
                                    struct pfs_pool_root *out);
enum pfs_status pfs_pool_root_encode(uint8_t *data, size_t capacity,
                                    const struct pfs_block_context *context,
                                    const struct pfs_pool_root *value);

#endif
