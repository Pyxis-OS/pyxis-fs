/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

#define CONTROL_CHECKSUM_OFFSET 48u
static const uint8_t journal_magic[8] = {'P', 'Y', 'X', 'I', 'S', 'J', 'N', 'L'};

bool
npfs_control_checksum_valid(const void *block)
{
  if (block == NULL) {
    return false;
  }
  const uint8_t *bytes = block;
  return npfs_bytes_equal(bytes, journal_magic, sizeof(journal_magic)) &&
         npfs_get_u32(bytes + CONTROL_CHECKSUM_OFFSET) == npfs_block_crc(bytes, CONTROL_CHECKSUM_OFFSET);
}

static enum npfs_status
control_validate(const struct npfs_header *header, const struct npfs_control *control)
{
  if (header == NULL || control == NULL) {
    return NPFS_INVALID;
  }
  if (!npfs_bytes_equal(header->pool_id, control->pool_id, NPFS_ID_SIZE)) {
    return NPFS_CORRUPT;
  }
  if (control->state == NPFS_JOURNAL_EMPTY) {
    return control->image_count == 0 && control->descriptor_blocks == 0 &&
           control->payload_crc == 0 ? NPFS_OK : NPFS_CORRUPT;
  }
  if (control->state != NPFS_JOURNAL_COMMITTED) {
    return NPFS_UNSUPPORTED;
  }
  uint64_t descriptors = (uint64_t)control->image_count / NPFS_DESCRIPTORS_PER_BLOCK +
                        (control->image_count % NPFS_DESCRIPTORS_PER_BLOCK != 0);
  if (control->image_count == 0 || control->descriptor_blocks != descriptors ||
      control->image_count > npfs_journal_capacity(header->journal_blocks)) {
    return NPFS_CORRUPT;
  }
  return NPFS_OK;
}

enum npfs_status
npfs_control_encode(const struct npfs_header *header, const struct npfs_control *control,
                     void *block)
{
  if (block == NULL || control_validate(header, control) != NPFS_OK) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_BLOCK_SIZE];
  npfs_memory_zero(bytes, sizeof(bytes));
  npfs_memory_copy(bytes, journal_magic, sizeof(journal_magic));
  npfs_memory_copy(bytes + 8, control->pool_id, NPFS_ID_SIZE);
  npfs_put_u64(bytes + 24, control->sequence);
  npfs_put_u32(bytes + 32, control->state);
  npfs_put_u32(bytes + 36, control->image_count);
  npfs_put_u32(bytes + 40, control->descriptor_blocks);
  npfs_put_u32(bytes + 44, control->payload_crc);
  npfs_put_u32(bytes + CONTROL_CHECKSUM_OFFSET, npfs_block_crc(bytes, CONTROL_CHECKSUM_OFFSET));
  npfs_memory_copy(block, bytes, sizeof(bytes));
  return NPFS_OK;
}

enum npfs_status
npfs_control_decode(const struct npfs_header *header, const void *block,
                     struct npfs_control *control)
{
  if (header == NULL || block == NULL || control == NULL) {
    return NPFS_INVALID;
  }
  const uint8_t *bytes = block;
  if (!npfs_control_checksum_valid(block)) {
    return NPFS_CORRUPT;
  }
  struct npfs_control result;
  npfs_memory_zero(&result, sizeof(result));
  npfs_memory_copy(result.pool_id, bytes + 8, NPFS_ID_SIZE);
  result.sequence = npfs_get_u64(bytes + 24);
  result.state = npfs_get_u32(bytes + 32);
  result.image_count = npfs_get_u32(bytes + 36);
  result.descriptor_blocks = npfs_get_u32(bytes + 40);
  result.payload_crc = npfs_get_u32(bytes + 44);
  enum npfs_status status = control_validate(header, &result);
  if (status != NPFS_OK) {
    return status;
  }
  npfs_memory_copy(control, &result, sizeof(result));
  return NPFS_OK;
}

uint32_t
npfs_payload_begin(const struct npfs_control *control)
{
  uint8_t context[NPFS_ID_SIZE + 16];
  npfs_memory_copy(context, control->pool_id, NPFS_ID_SIZE);
  npfs_put_u64(context + NPFS_ID_SIZE, control->sequence);
  npfs_put_u32(context + NPFS_ID_SIZE + 8, control->image_count);
  npfs_put_u32(context + NPFS_ID_SIZE + 12, control->descriptor_blocks);
  return npfs_crc_update(npfs_crc_begin(), context, sizeof(context));
}

static enum npfs_status
descriptor_validate(const struct npfs_header *header, const struct npfs_descriptor *descriptor)
{
  if (header == NULL || descriptor == NULL) {
    return NPFS_INVALID;
  }
  if (!npfs_flags_valid(header, descriptor->flags, 0)) {
    return NPFS_CORRUPT;
  }
  switch (descriptor->kind) {
  case NPFS_METADATA_BITMAP:
    return descriptor->home >= header->bitmap_start &&
           descriptor->home - header->bitmap_start < header->bitmap_blocks ? NPFS_OK : NPFS_CORRUPT;
  case NPFS_METADATA_VOLUMES:
    return descriptor->home >= header->volume_start &&
           descriptor->home - header->volume_start < header->volume_blocks ? NPFS_OK : NPFS_CORRUPT;
  case NPFS_METADATA_INODES:
  case NPFS_METADATA_DIRECTORY:
  case NPFS_METADATA_INDIRECT:
    return npfs_data_block_valid(header, descriptor->home) ? NPFS_OK : NPFS_CORRUPT;
  default:
    return NPFS_UNSUPPORTED;
  }
}

enum npfs_status
npfs_descriptor_encode(const struct npfs_header *header, const struct npfs_descriptor *descriptor,
                        void *record)
{
  if (record == NULL || descriptor_validate(header, descriptor) != NPFS_OK) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_DESCRIPTOR_SIZE];
  npfs_memory_zero(bytes, sizeof(bytes));
  npfs_put_u64(bytes, descriptor->home);
  npfs_put_u32(bytes + 8, descriptor->kind);
  npfs_put_u32(bytes + 12, descriptor->flags);
  npfs_memory_copy(record, bytes, sizeof(bytes));
  return NPFS_OK;
}

enum npfs_status
npfs_descriptor_decode(const struct npfs_header *header, const void *record,
                        struct npfs_descriptor *descriptor)
{
  if (header == NULL || record == NULL || descriptor == NULL) {
    return NPFS_INVALID;
  }
  const uint8_t *bytes = record;
  struct npfs_descriptor result = {
    .home = npfs_get_u64(bytes),
    .kind = npfs_get_u32(bytes + 8),
    .flags = npfs_get_u32(bytes + 12),
  };
  enum npfs_status status = descriptor_validate(header, &result);
  if (status != NPFS_OK) {
    return status;
  }
  npfs_memory_copy(descriptor, &result, sizeof(result));
  return NPFS_OK;
}
