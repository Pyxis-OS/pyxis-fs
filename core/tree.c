#include <pyxis_fs/record.h>
#include <pyxis_fs/tree.h>

#include "internal.h"

#define INTERNAL_KEY_OFFSET 48u
#define INTERNAL_CHILD_OFFSET 24u
#define GRANT_KEY_SIZE (2 * PFS_ID_SIZE + 1)

static size_t
align_record(size_t size)
{
  return (size + 7) & ~(size_t)7;
}

enum pfs_status
pfs_key_validate(uint16_t kind, const struct pfs_key *key)
{
  if (!key) {
    return PFS_INVALID;
  }
  switch (kind) {
  case PFS_INDEX_VOLUMES:
  case PFS_INDEX_OBJECTS:
    return key->length == PFS_ID_SIZE && !pfs_bytes_are_zero(key->bytes, PFS_ID_SIZE)
             ? PFS_OK : PFS_CORRUPT;
  case PFS_INDEX_VOLUME_NAMES:
  case PFS_INDEX_DIRECTORY:
    return pfs_name_validate(key->bytes, key->length) == PFS_OK ? PFS_OK : PFS_CORRUPT;
  case PFS_INDEX_ALLOCATION:
  case PFS_INDEX_EXTENTS:
    return key->length == sizeof(uint64_t) ? PFS_OK : PFS_CORRUPT;
  case PFS_INDEX_GRANTS:
    if (key->length != GRANT_KEY_SIZE || pfs_bytes_are_zero(key->bytes, PFS_ID_SIZE) ||
        pfs_bytes_are_zero(key->bytes + PFS_ID_SIZE, PFS_ID_SIZE) ||
        key->bytes[2 * PFS_ID_SIZE] > PFS_SCOPE_SUBTREE) {
      return PFS_CORRUPT;
    }
    return PFS_OK;
  default:
    return PFS_UNSUPPORTED;
  }
}

int
pfs_key_compare(uint16_t kind, const struct pfs_key *left, const struct pfs_key *right)
{
  if (kind == PFS_INDEX_ALLOCATION || kind == PFS_INDEX_EXTENTS) {
    uint64_t a = pfs_get_u64(left->bytes);
    uint64_t b = pfs_get_u64(right->bytes);
    return (a > b) - (a < b);
  }
  return pfs_bytes_compare(left->bytes, left->length, right->bytes, right->length);
}

enum pfs_status
pfs_internal_record_decode(const uint8_t *data, size_t slot_length, uint16_t kind,
                           const struct pfs_record_context *context,
                           struct pfs_internal_record *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_record_header_decode(data, slot_length, PFS_INTERNAL_RECORD_TYPE,
                                                   INTERNAL_KEY_OFFSET, context);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_internal_record value;
  pfs_bytes_zero(&value, sizeof(value));
  value.minimum.length = pfs_get_u16(data + 16);
  if (value.minimum.length > PFS_KEY_MAX ||
      value.minimum.length > slot_length - INTERNAL_KEY_OFFSET) {
    return PFS_CORRUPT;
  }
  pfs_bytes_copy(value.minimum.bytes, data + INTERNAL_KEY_OFFSET, value.minimum.length);
  status = pfs_key_validate(kind, &value.minimum);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(slot_length,
                                     align_record(INTERNAL_KEY_OFFSET + value.minimum.length),
                                     &context->features);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_reference_decode(data + INTERNAL_CHILD_OFFSET, context,
                                PFS_BLOCK_TREE, false, &value.child);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_internal_record_encode(uint8_t *data, size_t capacity, uint16_t kind,
                           const struct pfs_record_context *context,
                           const struct pfs_internal_record *value, size_t *written)
{
  if (!data || !value || !written) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status == PFS_OK) {
    status = pfs_features_write(&context->features);
  }
  if (status == PFS_OK) {
    status = pfs_key_validate(kind, &value->minimum);
  }
  if (status == PFS_OK) {
    status = pfs_reference_validate(&value->child, context, PFS_BLOCK_TREE, false);
  }
  if (status != PFS_OK) {
    return status == PFS_CORRUPT ? PFS_INVALID : status;
  }
  size_t length = align_record(INTERNAL_KEY_OFFSET + value->minimum.length);
  if (capacity < length) {
    return PFS_INVALID;
  }
  pfs_bytes_zero(data, length);
  pfs_put_u16(data + 16, value->minimum.length);
  pfs_reference_encode(data + INTERNAL_CHILD_OFFSET, &value->child);
  pfs_bytes_copy(data + INTERNAL_KEY_OFFSET, value->minimum.bytes, value->minimum.length);
  pfs_record_header_encode(data, PFS_INTERNAL_RECORD_TYPE, length);
  *written = length;
  return PFS_OK;
}

static enum pfs_status
leaf_key(const uint8_t *data, size_t length, uint16_t kind,
         const struct pfs_record_context *context, struct pfs_key *key, uint64_t *end)
{
  enum pfs_status status;
  *end = 0;
  switch (kind) {
  case PFS_INDEX_VOLUMES: {
    struct pfs_volume_record record;
    status = pfs_volume_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = PFS_ID_SIZE;
    pfs_bytes_copy(key->bytes, record.id.bytes, key->length);
    break;
  }
  case PFS_INDEX_VOLUME_NAMES: {
    struct pfs_volume_name_record record;
    status = pfs_volume_name_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = record.name.length;
    pfs_bytes_copy(key->bytes, record.name.bytes, key->length);
    break;
  }
  case PFS_INDEX_ALLOCATION: {
    struct pfs_allocation_record record;
    status = pfs_allocation_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = sizeof(uint64_t);
    pfs_put_u64(key->bytes, record.first);
    *end = record.first + record.count;
    break;
  }
  case PFS_INDEX_OBJECTS: {
    struct pfs_object_record record;
    status = pfs_object_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = PFS_ID_SIZE;
    pfs_bytes_copy(key->bytes, record.id.bytes, key->length);
    break;
  }
  case PFS_INDEX_DIRECTORY: {
    struct pfs_dirent_record record;
    status = pfs_dirent_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = record.name.length;
    pfs_bytes_copy(key->bytes, record.name.bytes, key->length);
    break;
  }
  case PFS_INDEX_EXTENTS: {
    struct pfs_extent_record record;
    status = pfs_extent_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = sizeof(uint64_t);
    pfs_put_u64(key->bytes, record.mapping.logical_first);
    *end = record.mapping.logical_first + record.mapping.count;
    break;
  }
  case PFS_INDEX_GRANTS: {
    struct pfs_grant_record record;
    status = pfs_grant_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    key->length = GRANT_KEY_SIZE;
    pfs_bytes_copy(key->bytes, record.object.bytes, PFS_ID_SIZE);
    pfs_bytes_copy(key->bytes + PFS_ID_SIZE, record.principal.bytes, PFS_ID_SIZE);
    key->bytes[2 * PFS_ID_SIZE] = record.scope;
    break;
  }
  default:
    return PFS_UNSUPPORTED;
  }
  return PFS_OK;
}

static enum pfs_status
tree_context_match(const struct pfs_tree *value, const struct pfs_tree_context *context)
{
  bool pool_index = value->kind <= PFS_INDEX_ALLOCATION;
  bool object_index = value->kind == PFS_INDEX_DIRECTORY || value->kind == PFS_INDEX_EXTENTS;
  if (value->kind < PFS_INDEX_VOLUMES || value->kind > PFS_INDEX_GRANTS) {
    return PFS_UNSUPPORTED;
  }
  if (value->kind != context->kind ||
      pfs_bytes_are_zero(value->volume.bytes, PFS_ID_SIZE) != pool_index ||
      pfs_bytes_are_zero(value->object.bytes, PFS_ID_SIZE) == object_index ||
      pfs_bytes_compare(value->volume.bytes, PFS_ID_SIZE, context->volume.bytes, PFS_ID_SIZE) ||
      pfs_bytes_compare(value->object.bytes, PFS_ID_SIZE, context->object.bytes, PFS_ID_SIZE)) {
    return PFS_CORRUPT;
  }
  if (context->parent_level && value->level != context->parent_level - 1) {
    return PFS_CORRUPT;
  }
  if (!value->count || value->count > PFS_TREE_SLOTS_MAX ||
      (value->level && value->count < 2)) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

static bool
allocation_coalescible(const uint8_t *previous, const uint8_t *current)
{
  return previous[32] == current[32] && previous[33] == current[33] &&
         !pfs_bytes_compare(previous + 40, PFS_ID_SIZE, current + 40, PFS_ID_SIZE) &&
         pfs_get_u64(previous + 56) == pfs_get_u64(current + 56) &&
         pfs_get_u64(previous + 64) == pfs_get_u64(current + 64);
}

enum pfs_status
pfs_tree_decode(const uint8_t *data, size_t length,
                 const struct pfs_tree_context *context, struct pfs_tree *out)
{
  if (!data || !context || !out) {
    return PFS_INVALID;
  }
  struct pfs_tree value;
  pfs_bytes_zero(&value, sizeof(value));
  enum pfs_status status = pfs_block_header_decode(data, length, context->block.block_count,
                                                 context->block.reference.block,
                                                 context->block.selected_generation, &value.header);
  if (status != PFS_OK) {
    return status == PFS_ABSENT ? PFS_CORRUPT : status;
  }
  status = pfs_block_match(&value.header, &context->block, PFS_BLOCK_TREE);
  if (status != PFS_OK) {
    return status;
  }
  if (value.header.used < PFS_TREE_HEADER_SIZE || pfs_get_u16(data + 134) != PFS_TREE_SLOT_SIZE) {
    return PFS_CORRUPT;
  }
  value.kind = pfs_get_u16(data + 128);
  value.level = pfs_get_u16(data + 130);
  value.count = pfs_get_u16(data + 132);
  pfs_bytes_copy(value.volume.bytes, data + 136, PFS_ID_SIZE);
  pfs_bytes_copy(value.object.bytes, data + 152, PFS_ID_SIZE);
  status = tree_context_match(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  size_t record_offset = align_record(PFS_TREE_HEADER_SIZE + value.count * PFS_TREE_SLOT_SIZE);
  if (record_offset > value.header.used) {
    return PFS_CORRUPT;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block.block_count,
    .selected_generation = context->block.selected_generation,
    .containing_birth = value.header.birth,
    .features = context->block.features,
  };
  uint64_t previous_end = 0;
  for (uint16_t i = 0; i < value.count; ++i) {
    const uint8_t *slot = data + PFS_TREE_HEADER_SIZE + i * PFS_TREE_SLOT_SIZE;
    value.slots[i].offset = pfs_get_u16(slot);
    value.slots[i].length = pfs_get_u16(slot + 2);
    size_t record_length = value.slots[i].length;
    if (value.slots[i].offset != record_offset || record_length < PFS_RECORD_HEADER_SIZE ||
        record_length % 8 || record_length > value.header.used - record_offset) {
      return PFS_CORRUPT;
    }
    struct pfs_key key;
    pfs_bytes_zero(&key, sizeof(key));
    uint64_t end = 0;
    if (value.level) {
      struct pfs_internal_record record;
      status = pfs_internal_record_decode(data + record_offset, record_length,
                                          value.kind, &record_context, &record);
      if (status == PFS_OK) {
        pfs_bytes_copy(&key, &record.minimum, sizeof(key));
        if (record.child.block == value.header.block) {
          return PFS_CORRUPT;
        }
      }
    } else {
      status = leaf_key(data + record_offset, record_length, value.kind, &record_context, &key, &end);
    }
    if (status != PFS_OK) {
      return status;
    }
    if (i && pfs_key_compare(value.kind, &value.maximum, &key) >= 0) {
      return PFS_CORRUPT;
    }
    if (i && !value.level && (value.kind == PFS_INDEX_EXTENTS || value.kind == PFS_INDEX_ALLOCATION)) {
      uint64_t first = pfs_get_u64(key.bytes);
      if (first < previous_end || (value.kind == PFS_INDEX_ALLOCATION && first != previous_end)) {
        return PFS_CORRUPT;
      }
      if (value.kind == PFS_INDEX_ALLOCATION &&
          allocation_coalescible(data + value.slots[i - 1].offset, data + record_offset)) {
        return PFS_CORRUPT;
      }
    }
    if (!i) {
      pfs_bytes_copy(&value.minimum, &key, sizeof(key));
    }
    pfs_bytes_copy(&value.maximum, &key, sizeof(key));
    previous_end = end;
    record_offset += record_length;
  }
  if (record_offset != value.header.used) {
    return PFS_CORRUPT;
  }
  if (value.level >= PFS_TREE_DEPTH_MAX) {
    return PFS_LIMIT;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_tree_encode(uint8_t *data, size_t capacity, const struct pfs_tree_context *context,
                 const struct pfs_tree *value, const struct pfs_encoded_record *records)
{
  if (!data || !context || !value || !records || capacity < PFS_BLOCK_SIZE ||
      !value->count || value->count > PFS_TREE_SLOTS_MAX ||
      value->header.type != PFS_BLOCK_TREE || value->header.version != PFS_FORMAT_VERSION) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_features_write(&context->block.features);
  if (status != PFS_OK) {
    return status;
  }
  uint8_t block[PFS_BLOCK_SIZE];
  pfs_bytes_zero(block, sizeof(block));
  struct pfs_block_header header = value->header;
  size_t offset = align_record(PFS_TREE_HEADER_SIZE + value->count * PFS_TREE_SLOT_SIZE);
  for (uint16_t i = 0; i < value->count; ++i) {
    if (!records[i].data || records[i].length < PFS_RECORD_HEADER_SIZE ||
        records[i].length % 8 || records[i].length > sizeof(block) - offset) {
      return PFS_INVALID;
    }
    uint8_t *slot = block + PFS_TREE_HEADER_SIZE + i * PFS_TREE_SLOT_SIZE;
    pfs_put_u16(slot, (uint16_t)offset);
    pfs_put_u16(slot + 2, (uint16_t)records[i].length);
    pfs_bytes_copy(block + offset, records[i].data, records[i].length);
    offset += records[i].length;
  }
  header.used = (uint32_t)offset;
  pfs_block_header_encode(block, &header);
  pfs_put_u16(block + 128, value->kind);
  pfs_put_u16(block + 130, value->level);
  pfs_put_u16(block + 132, value->count);
  pfs_put_u16(block + 134, PFS_TREE_SLOT_SIZE);
  pfs_bytes_copy(block + 136, value->volume.bytes, PFS_ID_SIZE);
  pfs_bytes_copy(block + 152, value->object.bytes, PFS_ID_SIZE);
  pfs_block_checksum_encode(block);
  struct pfs_tree checked;
  status = pfs_tree_decode(block, sizeof(block), context, &checked);
  if (status != PFS_OK) {
    return status == PFS_CORRUPT || status == PFS_ABSENT ? PFS_INVALID : status;
  }
  if (!checked.level && checked.kind == PFS_INDEX_VOLUMES) {
    for (uint16_t i = 0; i < checked.count; ++i) {
      const uint8_t *record = block + checked.slots[i].offset;
      struct pfs_features features = {
        .read_required = pfs_get_u64(record + 296),
        .write_required = pfs_get_u64(record + 304),
        .optional = pfs_get_u64(record + 312),
      };
      status = pfs_features_write(&features);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  pfs_bytes_copy(data, block, sizeof(block));
  return PFS_OK;
}
