#include "canonical.h"

#include <pyxis_fs/record.h>

#include "internal.h"

static enum pfs_status
canonical_compare(enum pfs_status status, const uint8_t *data, size_t length,
                  const uint8_t *canonical, size_t canonical_length)
{
  if (status != PFS_OK) {
    return status;
  }
  return pfs_bytes_compare(data, length, canonical, canonical_length) ?
    PFS_CORRUPT : PFS_OK;
}

enum pfs_status
pfs_canonical_record_validate(const uint8_t *data, size_t length,
                              uint16_t kind, uint16_t level,
                              const struct pfs_record_context *context)
{
  if (!data || !context) {
    return PFS_INVALID;
  }
  enum pfs_status status = kind <= PFS_INDEX_ALLOCATION ?
    pfs_features_write(&context->features) :
    pfs_volume_features_write(&context->features);
  if (status != PFS_OK) {
    return status;
  }
  uint8_t canonical[PFS_VOLUME_RECORD_SIZE];
  size_t written = 0;
  if (level) {
    struct pfs_internal_record record;
    status = pfs_internal_record_decode(data, length, kind, context, &record);
    if (status == PFS_OK) {
      status = pfs_internal_record_encode(canonical, sizeof(canonical), kind,
                                          context, &record, &written);
    }
    return canonical_compare(status, data, length, canonical, written);
  }
  switch (kind) {
  case PFS_INDEX_VOLUMES: {
    struct pfs_volume_record record;
    status = pfs_volume_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_volume_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_VOLUME_NAMES: {
    struct pfs_volume_name_record record;
    status = pfs_volume_name_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_volume_name_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_ALLOCATION: {
    struct pfs_allocation_record record;
    status = pfs_allocation_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_allocation_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_OBJECTS: {
    struct pfs_object_record record;
    status = pfs_object_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_object_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_DIRECTORY: {
    struct pfs_dirent_record record;
    status = pfs_dirent_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_dirent_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_EXTENTS: {
    struct pfs_extent_record record;
    status = pfs_extent_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_extent_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_GRANTS: {
    struct pfs_grant_record record;
    status = pfs_grant_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_grant_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }
  case PFS_INDEX_ORPHANS: {
    struct pfs_orphan_record record;
    status = pfs_orphan_record_decode(data, length, context, &record);
    if (status == PFS_OK) {
      status = pfs_orphan_record_encode(canonical, sizeof(canonical), context,
                                        &record, &written);
    }
    break;
  }

  default:
    return PFS_UNSUPPORTED;
  }
  return canonical_compare(status, data, length, canonical, written);
}

enum pfs_status
pfs_canonical_tree_validate(const uint8_t *data, size_t length,
                            const struct pfs_tree_context *context,
                            struct pfs_tree *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  struct pfs_tree tree;
  enum pfs_status status = pfs_tree_decode(data, length, context, &tree);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context records_context = {
    .block_count = context->block.block_count,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth,
    .features = context->block.features,
  };
  struct pfs_encoded_record records[PFS_TREE_SLOTS_MAX];
  for (uint16_t i = 0; i < tree.count; ++i) {
    records[i].data = data + tree.slots[i].offset;
    records[i].length = tree.slots[i].length;
    status = pfs_canonical_record_validate(records[i].data, records[i].length,
                                          tree.kind, tree.level, &records_context);
    if (status != PFS_OK) {
      return status;
    }
  }
  uint8_t canonical[PFS_BLOCK_SIZE];
  status = pfs_tree_encode(canonical, sizeof(canonical), context, &tree, records);
  status = canonical_compare(status, data, length, canonical, sizeof(canonical));
  if (status == PFS_OK) {
    *out = tree;
  }
  return status;
}

enum pfs_status
pfs_canonical_superblock_validate(const uint8_t *data, size_t length,
                                  uint64_t blocks, uint64_t slot,
                                  struct pfs_superblock *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  struct pfs_superblock superblock;
  enum pfs_status status = pfs_superblock_decode(data, length, blocks, slot, &superblock);
  if (status != PFS_OK) {
    return status;
  }
  uint8_t canonical[PFS_BLOCK_SIZE];
  status = pfs_superblock_encode(canonical, sizeof(canonical), &superblock);
  status = canonical_compare(status, data, length, canonical, sizeof(canonical));
  if (status == PFS_OK) {
    *out = superblock;
  }
  return status;
}

enum pfs_status
pfs_canonical_pool_root_validate(const uint8_t *data, size_t length,
                                 const struct pfs_block_context *context,
                                 struct pfs_pool_root *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  struct pfs_pool_root root;
  enum pfs_status status = pfs_pool_root_decode(data, length, context, &root);
  if (status != PFS_OK) {
    return status;
  }
  uint8_t canonical[PFS_BLOCK_SIZE];
  status = pfs_pool_root_encode(canonical, sizeof(canonical), context, &root);
  status = canonical_compare(status, data, length, canonical, sizeof(canonical));
  if (status == PFS_OK) {
    *out = root;
  }
  return status;
}
