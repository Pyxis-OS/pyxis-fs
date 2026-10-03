/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

#define HEADER_CHECKSUM_OFFSET 108u
static const uint8_t header_magic[8] = {'P', 'Y', 'X', 'I', 'S', 'N', 'F', 'S'};

static bool
region_valid(uint64_t blocks, uint64_t start, uint64_t count)
{
  return blocks >= 2 && start > 0 && start < blocks - 1 && count > 0 &&
         count <= blocks - 1 - start;
}

static bool
regions_overlap(uint64_t a, uint64_t a_count, uint64_t b, uint64_t b_count)
{
  return a < b + b_count && b < a + a_count;
}

enum npfs_status
npfs_header_validate(const struct npfs_header *header)
{
  if (header == NULL || !npfs_id_valid(header->pool_id) ||
      !region_valid(header->pool_blocks, header->bitmap_start, header->bitmap_blocks) ||
      !region_valid(header->pool_blocks, header->volume_start, header->volume_blocks) ||
      !region_valid(header->pool_blocks, header->journal_start, header->journal_blocks) ||
      header->bitmap_blocks != npfs_bitmap_blocks(header->pool_blocks) ||
      header->volume_blocks != NPFS_VOLUME_TABLE_BLOCKS ||
      npfs_journal_capacity(header->journal_blocks) == 0) {
    return NPFS_INVALID;
  }
  if (regions_overlap(header->bitmap_start, header->bitmap_blocks,
                      header->volume_start, header->volume_blocks) ||
      regions_overlap(header->bitmap_start, header->bitmap_blocks,
                      header->journal_start, header->journal_blocks) ||
      regions_overlap(header->volume_start, header->volume_blocks,
                      header->journal_start, header->journal_blocks)) {
    return NPFS_INVALID;
  }
  return NPFS_OK;
}

enum npfs_status
npfs_features_check(const struct npfs_header *header, bool writable)
{
  if (header == NULL) {
    return NPFS_INVALID;
  }
  if (header->required != 0 || (writable && header->read_only_compatible != 0)) {
    return NPFS_UNSUPPORTED;
  }
  return NPFS_OK;
}

enum npfs_status
npfs_header_layout(uint64_t blocks, uint64_t journal_blocks,
                    const uint8_t id[NPFS_ID_SIZE], struct npfs_header *header)
{
  if (header == NULL || !npfs_id_valid(id) || blocks < 2 ||
      npfs_journal_capacity(journal_blocks) == 0) {
    return NPFS_INVALID;
  }
  struct npfs_header result;
  npfs_memory_zero(&result, sizeof(result));
  npfs_memory_copy(result.pool_id, id, NPFS_ID_SIZE);
  result.pool_blocks = blocks;
  result.bitmap_start = 1;
  result.bitmap_blocks = npfs_bitmap_blocks(blocks);
  result.volume_start = result.bitmap_start + result.bitmap_blocks;
  result.volume_blocks = NPFS_VOLUME_TABLE_BLOCKS;
  if (result.volume_start > UINT64_MAX - result.volume_blocks) {
    return NPFS_NO_SPACE;
  }
  result.journal_start = result.volume_start + result.volume_blocks;
  result.journal_blocks = journal_blocks;
  if (npfs_header_validate(&result) != NPFS_OK) {
    return NPFS_NO_SPACE;
  }
  npfs_memory_copy(header, &result, sizeof(result));
  return NPFS_OK;
}

enum npfs_status
npfs_header_encode(const struct npfs_header *header, void *block)
{
  if (block == NULL || npfs_header_validate(header) != NPFS_OK) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_BLOCK_SIZE];
  npfs_memory_zero(bytes, sizeof(bytes));
  npfs_memory_copy(bytes, header_magic, sizeof(header_magic));
  npfs_memory_copy(bytes + 8, header->pool_id, NPFS_ID_SIZE);
  npfs_put_u64(bytes + 24, header->pool_blocks);
  npfs_put_u64(bytes + 32, header->bitmap_start);
  npfs_put_u64(bytes + 40, header->bitmap_blocks);
  npfs_put_u64(bytes + 48, header->volume_start);
  npfs_put_u64(bytes + 56, header->volume_blocks);
  npfs_put_u64(bytes + 64, header->journal_start);
  npfs_put_u64(bytes + 72, header->journal_blocks);
  npfs_put_u64(bytes + 80, header->compatible);
  npfs_put_u64(bytes + 88, header->read_only_compatible);
  npfs_put_u64(bytes + 96, header->required);
  npfs_put_u32(bytes + 104, NPFS_BLOCK_SIZE);
  npfs_put_u32(bytes + HEADER_CHECKSUM_OFFSET, npfs_block_crc(bytes, HEADER_CHECKSUM_OFFSET));
  npfs_memory_copy(block, bytes, sizeof(bytes));
  return NPFS_OK;
}

enum npfs_status
npfs_header_decode(const void *block, struct npfs_header *header)
{
  if (block == NULL || header == NULL) {
    return NPFS_INVALID;
  }
  const uint8_t *bytes = block;
  if (!npfs_bytes_equal(bytes, header_magic, sizeof(header_magic)) ||
      npfs_get_u32(bytes + HEADER_CHECKSUM_OFFSET) != npfs_block_crc(bytes, HEADER_CHECKSUM_OFFSET)) {
    return NPFS_CORRUPT;
  }
  if (npfs_get_u32(bytes + 104) != NPFS_BLOCK_SIZE) {
    return NPFS_UNSUPPORTED;
  }
  struct npfs_header result;
  npfs_memory_zero(&result, sizeof(result));
  npfs_memory_copy(result.pool_id, bytes + 8, NPFS_ID_SIZE);
  result.pool_blocks = npfs_get_u64(bytes + 24);
  result.bitmap_start = npfs_get_u64(bytes + 32);
  result.bitmap_blocks = npfs_get_u64(bytes + 40);
  result.volume_start = npfs_get_u64(bytes + 48);
  result.volume_blocks = npfs_get_u64(bytes + 56);
  result.journal_start = npfs_get_u64(bytes + 64);
  result.journal_blocks = npfs_get_u64(bytes + 72);
  result.compatible = npfs_get_u64(bytes + 80);
  result.read_only_compatible = npfs_get_u64(bytes + 88);
  result.required = npfs_get_u64(bytes + 96);
  if (npfs_header_validate(&result) != NPFS_OK) {
    return NPFS_CORRUPT;
  }
  npfs_memory_copy(header, &result, sizeof(result));
  return NPFS_OK;
}
