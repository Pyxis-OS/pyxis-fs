/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

#define CONTROL_CHECKSUM_OFFSET 48u
static const uint8_t journal_magic[8] = {'P', 'Y', 'X', 'I', 'S', 'J', 'N', 'L'};

bool
pnf_control_checksum_valid(const void *block)
{
  if (block == NULL) {
    return false;
  }
  const uint8_t *bytes = block;
  return pnf_bytes_equal(bytes, journal_magic, sizeof(journal_magic)) &&
         pnf_get_u32(bytes + CONTROL_CHECKSUM_OFFSET) == pnf_block_crc(bytes, CONTROL_CHECKSUM_OFFSET);
}

static enum pnf_status
control_validate(const struct pnf_header *header, const struct pnf_control *control)
{
  if (header == NULL || control == NULL) {
    return PNF_INVALID;
  }
  if (!pnf_bytes_equal(header->pool_id, control->pool_id, PNF_ID_SIZE)) {
    return PNF_CORRUPT;
  }
  if (control->state == PNF_JOURNAL_EMPTY) {
    return control->image_count == 0 && control->descriptor_blocks == 0 &&
           control->payload_crc == 0 ? PNF_OK : PNF_CORRUPT;
  }
  if (control->state != PNF_JOURNAL_COMMITTED) {
    return PNF_UNSUPPORTED;
  }
  uint64_t descriptors = (uint64_t)control->image_count / PNF_DESCRIPTORS_PER_BLOCK +
                        (control->image_count % PNF_DESCRIPTORS_PER_BLOCK != 0);
  if (control->image_count == 0 || control->descriptor_blocks != descriptors ||
      control->image_count > pnf_journal_capacity(header->journal_blocks)) {
    return PNF_CORRUPT;
  }
  return PNF_OK;
}

enum pnf_status
pnf_control_encode(const struct pnf_header *header, const struct pnf_control *control,
                     void *block)
{
  if (block == NULL || control_validate(header, control) != PNF_OK) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_BLOCK_SIZE];
  pnf_memory_zero(bytes, sizeof(bytes));
  pnf_memory_copy(bytes, journal_magic, sizeof(journal_magic));
  pnf_memory_copy(bytes + 8, control->pool_id, PNF_ID_SIZE);
  pnf_put_u64(bytes + 24, control->sequence);
  pnf_put_u32(bytes + 32, control->state);
  pnf_put_u32(bytes + 36, control->image_count);
  pnf_put_u32(bytes + 40, control->descriptor_blocks);
  pnf_put_u32(bytes + 44, control->payload_crc);
  pnf_put_u32(bytes + CONTROL_CHECKSUM_OFFSET, pnf_block_crc(bytes, CONTROL_CHECKSUM_OFFSET));
  pnf_memory_copy(block, bytes, sizeof(bytes));
  return PNF_OK;
}

enum pnf_status
pnf_control_decode(const struct pnf_header *header, const void *block,
                     struct pnf_control *control)
{
  if (header == NULL || block == NULL || control == NULL) {
    return PNF_INVALID;
  }
  const uint8_t *bytes = block;
  if (!pnf_control_checksum_valid(block)) {
    return PNF_CORRUPT;
  }
  struct pnf_control result;
  pnf_memory_zero(&result, sizeof(result));
  pnf_memory_copy(result.pool_id, bytes + 8, PNF_ID_SIZE);
  result.sequence = pnf_get_u64(bytes + 24);
  result.state = pnf_get_u32(bytes + 32);
  result.image_count = pnf_get_u32(bytes + 36);
  result.descriptor_blocks = pnf_get_u32(bytes + 40);
  result.payload_crc = pnf_get_u32(bytes + 44);
  enum pnf_status status = control_validate(header, &result);
  if (status != PNF_OK) {
    return status;
  }
  pnf_memory_copy(control, &result, sizeof(result));
  return PNF_OK;
}

uint32_t
pnf_payload_begin(const struct pnf_control *control)
{
  uint8_t context[PNF_ID_SIZE + 16];
  pnf_memory_copy(context, control->pool_id, PNF_ID_SIZE);
  pnf_put_u64(context + PNF_ID_SIZE, control->sequence);
  pnf_put_u32(context + PNF_ID_SIZE + 8, control->image_count);
  pnf_put_u32(context + PNF_ID_SIZE + 12, control->descriptor_blocks);
  return pnf_crc_update(pnf_crc_begin(), context, sizeof(context));
}

static enum pnf_status
descriptor_validate(const struct pnf_header *header, const struct pnf_descriptor *descriptor)
{
  if (header == NULL || descriptor == NULL) {
    return PNF_INVALID;
  }
  switch (descriptor->kind) {
  case PNF_METADATA_BITMAP:
    return descriptor->home >= header->bitmap_start &&
           descriptor->home - header->bitmap_start < header->bitmap_blocks ? PNF_OK : PNF_CORRUPT;
  case PNF_METADATA_VOLUMES:
    return descriptor->home >= header->volume_start &&
           descriptor->home - header->volume_start < header->volume_blocks ? PNF_OK : PNF_CORRUPT;
  case PNF_METADATA_INODES:
  case PNF_METADATA_DIRECTORY:
  case PNF_METADATA_INDIRECT:
    return pnf_data_block_valid(header, descriptor->home) ? PNF_OK : PNF_CORRUPT;
  default:
    return PNF_UNSUPPORTED;
  }
}

enum pnf_status
pnf_descriptor_encode(const struct pnf_header *header, const struct pnf_descriptor *descriptor,
                        void *record)
{
  if (record == NULL || descriptor_validate(header, descriptor) != PNF_OK) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_DESCRIPTOR_SIZE];
  pnf_memory_zero(bytes, sizeof(bytes));
  pnf_put_u64(bytes, descriptor->home);
  pnf_put_u32(bytes + 8, descriptor->kind);
  pnf_put_u32(bytes + 12, descriptor->flags);
  pnf_memory_copy(record, bytes, sizeof(bytes));
  return PNF_OK;
}

enum pnf_status
pnf_descriptor_decode(const struct pnf_header *header, const void *record,
                        struct pnf_descriptor *descriptor)
{
  if (header == NULL || record == NULL || descriptor == NULL) {
    return PNF_INVALID;
  }
  const uint8_t *bytes = record;
  struct pnf_descriptor result = {
    .home = pnf_get_u64(bytes),
    .kind = pnf_get_u32(bytes + 8),
    .flags = pnf_get_u32(bytes + 12),
  };
  enum pnf_status status = descriptor_validate(header, &result);
  if (status != PNF_OK) {
    return status;
  }
  pnf_memory_copy(descriptor, &result, sizeof(result));
  return PNF_OK;
}
