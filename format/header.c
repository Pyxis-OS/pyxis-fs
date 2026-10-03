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

enum pnf_status
pnf_header_validate(const struct pnf_header *header)
{
  if (header == NULL || !pnf_id_valid(header->pool_id) ||
      !region_valid(header->pool_blocks, header->bitmap_start, header->bitmap_blocks) ||
      !region_valid(header->pool_blocks, header->volume_start, header->volume_blocks) ||
      !region_valid(header->pool_blocks, header->journal_start, header->journal_blocks) ||
      header->bitmap_blocks != pnf_bitmap_blocks(header->pool_blocks) ||
      header->volume_blocks != PNF_VOLUME_TABLE_BLOCKS ||
      pnf_journal_capacity(header->journal_blocks) == 0) {
    return PNF_INVALID;
  }
  if (regions_overlap(header->bitmap_start, header->bitmap_blocks,
                      header->volume_start, header->volume_blocks) ||
      regions_overlap(header->bitmap_start, header->bitmap_blocks,
                      header->journal_start, header->journal_blocks) ||
      regions_overlap(header->volume_start, header->volume_blocks,
                      header->journal_start, header->journal_blocks)) {
    return PNF_INVALID;
  }
  return PNF_OK;
}

enum pnf_status
pnf_features_check(const struct pnf_header *header, bool writable)
{
  if (header == NULL) {
    return PNF_INVALID;
  }
  if (header->required != 0 || (writable && header->read_only_compatible != 0)) {
    return PNF_UNSUPPORTED;
  }
  return PNF_OK;
}

enum pnf_status
pnf_header_layout(uint64_t blocks, uint64_t journal_blocks,
                    const uint8_t id[PNF_ID_SIZE], struct pnf_header *header)
{
  if (header == NULL || !pnf_id_valid(id) || blocks < 2 ||
      pnf_journal_capacity(journal_blocks) == 0) {
    return PNF_INVALID;
  }
  struct pnf_header result;
  pnf_memory_zero(&result, sizeof(result));
  pnf_memory_copy(result.pool_id, id, PNF_ID_SIZE);
  result.pool_blocks = blocks;
  result.bitmap_start = 1;
  result.bitmap_blocks = pnf_bitmap_blocks(blocks);
  result.volume_start = result.bitmap_start + result.bitmap_blocks;
  result.volume_blocks = PNF_VOLUME_TABLE_BLOCKS;
  if (result.volume_start > UINT64_MAX - result.volume_blocks) {
    return PNF_NO_SPACE;
  }
  result.journal_start = result.volume_start + result.volume_blocks;
  result.journal_blocks = journal_blocks;
  if (pnf_header_validate(&result) != PNF_OK) {
    return PNF_NO_SPACE;
  }
  pnf_memory_copy(header, &result, sizeof(result));
  return PNF_OK;
}

enum pnf_status
pnf_header_encode(const struct pnf_header *header, void *block)
{
  if (block == NULL || pnf_header_validate(header) != PNF_OK) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_BLOCK_SIZE];
  pnf_memory_zero(bytes, sizeof(bytes));
  pnf_memory_copy(bytes, header_magic, sizeof(header_magic));
  pnf_memory_copy(bytes + 8, header->pool_id, PNF_ID_SIZE);
  pnf_put_u64(bytes + 24, header->pool_blocks);
  pnf_put_u64(bytes + 32, header->bitmap_start);
  pnf_put_u64(bytes + 40, header->bitmap_blocks);
  pnf_put_u64(bytes + 48, header->volume_start);
  pnf_put_u64(bytes + 56, header->volume_blocks);
  pnf_put_u64(bytes + 64, header->journal_start);
  pnf_put_u64(bytes + 72, header->journal_blocks);
  pnf_put_u64(bytes + 80, header->compatible);
  pnf_put_u64(bytes + 88, header->read_only_compatible);
  pnf_put_u64(bytes + 96, header->required);
  pnf_put_u32(bytes + 104, PNF_BLOCK_SIZE);
  pnf_put_u32(bytes + HEADER_CHECKSUM_OFFSET, pnf_block_crc(bytes, HEADER_CHECKSUM_OFFSET));
  pnf_memory_copy(block, bytes, sizeof(bytes));
  return PNF_OK;
}

enum pnf_status
pnf_header_decode(const void *block, struct pnf_header *header)
{
  if (block == NULL || header == NULL) {
    return PNF_INVALID;
  }
  const uint8_t *bytes = block;
  if (!pnf_bytes_equal(bytes, header_magic, sizeof(header_magic)) ||
      pnf_get_u32(bytes + HEADER_CHECKSUM_OFFSET) != pnf_block_crc(bytes, HEADER_CHECKSUM_OFFSET)) {
    return PNF_CORRUPT;
  }
  if (pnf_get_u32(bytes + 104) != PNF_BLOCK_SIZE) {
    return PNF_UNSUPPORTED;
  }
  struct pnf_header result;
  pnf_memory_zero(&result, sizeof(result));
  pnf_memory_copy(result.pool_id, bytes + 8, PNF_ID_SIZE);
  result.pool_blocks = pnf_get_u64(bytes + 24);
  result.bitmap_start = pnf_get_u64(bytes + 32);
  result.bitmap_blocks = pnf_get_u64(bytes + 40);
  result.volume_start = pnf_get_u64(bytes + 48);
  result.volume_blocks = pnf_get_u64(bytes + 56);
  result.journal_start = pnf_get_u64(bytes + 64);
  result.journal_blocks = pnf_get_u64(bytes + 72);
  result.compatible = pnf_get_u64(bytes + 80);
  result.read_only_compatible = pnf_get_u64(bytes + 88);
  result.required = pnf_get_u64(bytes + 96);
  if (pnf_header_validate(&result) != PNF_OK) {
    return PNF_CORRUPT;
  }
  pnf_memory_copy(header, &result, sizeof(result));
  return PNF_OK;
}
