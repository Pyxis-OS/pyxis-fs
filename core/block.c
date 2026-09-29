#include <pyxis_fs/block.h>

#include "internal.h"

#define BLOCK_CHECKSUM_OFFSET 20u
#define CHECKSUM_SIZE 4u
#define SUPER_USED 192u
#define POOL_USED 288u

static const uint8_t block_magic[8] = { 'P', 'Y', 'X', 'I', 'S', 'F', 'S', 0 };

enum pfs_status
pfs_block_header_decode(const uint8_t *data, size_t length, uint64_t blocks,
                        uint64_t physical_block, uint64_t generation,
                        struct pfs_block_header *out)
{
  if (!data || !out || blocks < 3 || physical_block >= blocks || !generation) {
    return PFS_INVALID;
  }
  if (length != PFS_BLOCK_SIZE) {
    return PFS_CORRUPT;
  }
  if (pfs_bytes_compare(data, sizeof(block_magic), block_magic, sizeof(block_magic))) {
    return PFS_ABSENT;
  }
  if (pfs_get_u32(data + BLOCK_CHECKSUM_OFFSET) !=
      pfs_crc32c_zeroed(data, length, BLOCK_CHECKSUM_OFFSET, CHECKSUM_SIZE)) {
    return PFS_CORRUPT;
  }
  if (pfs_get_u16(data + 10) != PFS_FORMAT_VERSION ||
      pfs_get_u16(data + 12) != PFS_BLOCK_HEADER_SIZE) {
    return PFS_UNSUPPORTED;
  }
  struct pfs_block_header value;
  pfs_bytes_zero(&value, sizeof(value));
  value.type = pfs_get_u16(data + 8);
  value.version = pfs_get_u16(data + 10);
  value.used = pfs_get_u32(data + 16);
  pfs_bytes_copy(value.pool.bytes, data + 24, PFS_ID_SIZE);
  value.block = pfs_get_u64(data + 40);
  value.birth = pfs_get_u64(data + 48);
  if (value.type < PFS_BLOCK_SUPER || value.type > PFS_BLOCK_TREE) {
    return PFS_UNSUPPORTED;
  }
  if (value.used < PFS_BLOCK_HEADER_SIZE || value.used > PFS_BLOCK_SIZE ||
      pfs_bytes_are_zero(value.pool.bytes, PFS_ID_SIZE) || value.block != physical_block ||
      !value.birth || value.birth > generation) {
    return PFS_CORRUPT;
  }
  if ((value.type == PFS_BLOCK_SUPER && physical_block != 0 && physical_block != blocks - 1) ||
      (value.type != PFS_BLOCK_SUPER && !pfs_allocatable_range(physical_block, 1, blocks))) {
    return PFS_CORRUPT;
  }
  if (blocks < PFS_POOL_BLOCKS_MIN || blocks > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_block_match(const struct pfs_block_header *header,
                const struct pfs_block_context *context, uint16_t type)
{
  if (!header || !context || pfs_bytes_are_zero(context->pool.bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block_count,
    .selected_generation = context->selected_generation,
    .containing_birth = context->referring_birth,
    .features = context->features,
  };
  enum pfs_status status = pfs_context_validate(&record_context);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_reference_validate(&context->reference, &record_context, type, false);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_bytes_compare(header->pool.bytes, PFS_ID_SIZE, context->pool.bytes, PFS_ID_SIZE) ||
      header->block != context->reference.block || header->birth != context->reference.birth ||
      header->type != context->reference.type || header->version != context->reference.version) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

void
pfs_block_header_encode(uint8_t *data, const struct pfs_block_header *header)
{
  pfs_bytes_copy(data, block_magic, sizeof(block_magic));
  pfs_put_u16(data + 8, header->type);
  pfs_put_u16(data + 10, PFS_FORMAT_VERSION);
  pfs_put_u16(data + 12, PFS_BLOCK_HEADER_SIZE);
  pfs_put_u32(data + 16, header->used);
  pfs_bytes_copy(data + 24, header->pool.bytes, PFS_ID_SIZE);
  pfs_put_u64(data + 40, header->block);
  pfs_put_u64(data + 48, header->birth);
}

void
pfs_block_checksum_encode(uint8_t *data)
{
  pfs_put_u32(data + BLOCK_CHECKSUM_OFFSET,
              pfs_crc32c_zeroed(data, PFS_BLOCK_SIZE, BLOCK_CHECKSUM_OFFSET, CHECKSUM_SIZE));
}

enum pfs_status
pfs_superblock_decode(const uint8_t *data, size_t length, uint64_t blocks,
                      uint64_t slot, struct pfs_superblock *out)
{
  if (!data || !out || blocks < 3 || (slot != 0 && slot != blocks - 1)) {
    return PFS_INVALID;
  }
  if (length != PFS_BLOCK_SIZE) {
    return PFS_CORRUPT;
  }
  struct pfs_superblock value;
  pfs_bytes_zero(&value, sizeof(value));
  /* An unopened slot supplies its own generation; zero is rejected by the header. */
  enum pfs_status status = pfs_block_header_decode(data, length, blocks, slot,
                                                 UINT64_MAX, &value.header);
  if (status != PFS_OK) {
    return status;
  }
  if (value.header.type != PFS_BLOCK_SUPER || value.header.used < SUPER_USED ||
      pfs_get_u32(data + 128) != PFS_BLOCK_SIZE || pfs_get_u64(data + 136) != blocks) {
    return PFS_CORRUPT;
  }
  value.block_count = blocks;
  value.features.read_required = pfs_get_u64(data + 144);
  value.features.write_required = pfs_get_u64(data + 152);
  value.features.optional = pfs_get_u64(data + 160);
  status = pfs_features_read(&value.features);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(value.header.used, SUPER_USED, &value.features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context context = {
    .block_count = blocks,
    .selected_generation = value.header.birth,
    .containing_birth = value.header.birth,
    .features = value.features,
  };
  status = pfs_reference_decode(data + 168, &context, PFS_BLOCK_POOL, false, &value.root);
  if (status != PFS_OK) {
    return status;
  }
  if (value.root.birth != value.header.birth) {
    return PFS_CORRUPT;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_superblock_encode(uint8_t *data, size_t capacity, const struct pfs_superblock *value)
{
  if (!data || !value || capacity < PFS_BLOCK_SIZE) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_features_write(&value->features);
  if (status != PFS_OK) {
    return status;
  }
  if (value->header.type != PFS_BLOCK_SUPER || value->header.version != PFS_FORMAT_VERSION ||
      value->header.used != SUPER_USED) {
    return PFS_INVALID;
  }
  uint8_t block[PFS_BLOCK_SIZE];
  pfs_bytes_zero(block, sizeof(block));
  pfs_block_header_encode(block, &value->header);
  pfs_put_u32(block + 128, PFS_BLOCK_SIZE);
  pfs_put_u64(block + 136, value->block_count);
  pfs_reference_encode(block + 168, &value->root);
  pfs_block_checksum_encode(block);
  struct pfs_superblock checked;
  status = pfs_superblock_decode(block, sizeof(block), value->block_count,
                                value->header.block, &checked);
  if (status != PFS_OK) {
    return status == PFS_CORRUPT || status == PFS_ABSENT ? PFS_INVALID : status;
  }
  pfs_bytes_copy(data, block, sizeof(block));
  return PFS_OK;
}

enum pfs_status
pfs_pool_root_decode(const uint8_t *data, size_t length,
                     const struct pfs_block_context *context, struct pfs_pool_root *out)
{
  if (!data || !context || !out) {
    return PFS_INVALID;
  }
  struct pfs_pool_root value;
  pfs_bytes_zero(&value, sizeof(value));
  enum pfs_status status = pfs_block_header_decode(data, length, context->block_count,
                                                 context->reference.block,
                                                 context->selected_generation, &value.header);
  if (status != PFS_OK) {
    return status == PFS_ABSENT ? PFS_CORRUPT : status;
  }
  status = pfs_block_match(&value.header, context, PFS_BLOCK_POOL);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(value.header.used, POOL_USED, &context->features);
  if (status != PFS_OK || value.header.birth != context->selected_generation) {
    return status != PFS_OK ? status : PFS_CORRUPT;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block_count,
    .selected_generation = context->selected_generation,
    .containing_birth = value.header.birth,
    .features = context->features,
  };
  status = pfs_reference_decode(data + 128, &record_context, PFS_BLOCK_TREE, false, &value.volumes);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_reference_decode(data + 152, &record_context, PFS_BLOCK_TREE, false, &value.volume_names);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_reference_decode(data + 176, &record_context, PFS_BLOCK_TREE, false, &value.allocation);
  if (status != PFS_OK) {
    return status;
  }
  value.volume_count = pfs_get_u64(data + 200);
  value.live_pool = pfs_get_u64(data + 208);
  value.live_volume = pfs_get_u64(data + 216);
  value.retired = pfs_get_u64(data + 224);
  value.free = pfs_get_u64(data + 232);
  value.cow.capacity = pfs_get_u64(data + 240);
  value.cow.occupied = pfs_get_u64(data + 248);
  value.migration.capacity = pfs_get_u64(data + 256);
  value.migration.occupied = pfs_get_u64(data + 264);
  value.recovery.capacity = pfs_get_u64(data + 272);
  value.recovery.occupied = pfs_get_u64(data + 280);
  uint64_t remaining = context->block_count - 2;
  const uint64_t counts[] = { value.live_pool, value.live_volume, value.retired, value.free };
  for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
    if (counts[i] > remaining) {
      return PFS_CORRUPT;
    }
    remaining -= counts[i];
  }
  if (remaining || !value.volume_count || !value.live_pool || !value.live_volume ||
      value.cow.occupied > value.cow.capacity || value.migration.occupied > value.migration.capacity ||
      value.recovery.occupied > value.recovery.capacity ||
      value.cow.capacity > context->block_count - 2 ||
      value.migration.capacity > context->block_count - 2 ||
      value.recovery.capacity > context->block_count - 2) {
    return PFS_CORRUPT;
  }
  if (value.volume_count > PFS_VOLUME_MAX) {
    return PFS_LIMIT;
  }
  uint64_t available = value.free;
  const struct pfs_budget budgets[] = { value.cow, value.migration, value.recovery };
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
    uint64_t unused = budgets[i].capacity - budgets[i].occupied;
    if (unused > available) {
      return PFS_CORRUPT;
    }
    available -= unused;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_pool_root_encode(uint8_t *data, size_t capacity, const struct pfs_block_context *context,
                     const struct pfs_pool_root *value)
{
  if (!data || !context || !value || capacity < PFS_BLOCK_SIZE) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_features_write(&context->features);
  if (status != PFS_OK) {
    return status;
  }
  if (value->header.type != PFS_BLOCK_POOL || value->header.version != PFS_FORMAT_VERSION ||
      value->header.used != POOL_USED) {
    return PFS_INVALID;
  }
  uint8_t block[PFS_BLOCK_SIZE];
  pfs_bytes_zero(block, sizeof(block));
  pfs_block_header_encode(block, &value->header);
  pfs_reference_encode(block + 128, &value->volumes);
  pfs_reference_encode(block + 152, &value->volume_names);
  pfs_reference_encode(block + 176, &value->allocation);
  pfs_put_u64(block + 200, value->volume_count);
  pfs_put_u64(block + 208, value->live_pool);
  pfs_put_u64(block + 216, value->live_volume);
  pfs_put_u64(block + 224, value->retired);
  pfs_put_u64(block + 232, value->free);
  pfs_put_u64(block + 240, value->cow.capacity);
  pfs_put_u64(block + 248, value->cow.occupied);
  pfs_put_u64(block + 256, value->migration.capacity);
  pfs_put_u64(block + 264, value->migration.occupied);
  pfs_put_u64(block + 272, value->recovery.capacity);
  pfs_put_u64(block + 280, value->recovery.occupied);
  pfs_block_checksum_encode(block);
  struct pfs_pool_root checked;
  status = pfs_pool_root_decode(block, sizeof(block), context, &checked);
  if (status != PFS_OK) {
    return status == PFS_CORRUPT || status == PFS_ABSENT ? PFS_INVALID : status;
  }
  pfs_bytes_copy(data, block, sizeof(block));
  return PFS_OK;
}
