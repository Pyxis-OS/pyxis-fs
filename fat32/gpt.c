/* SPDX-License-Identifier: MPL-2.0 */
/* GPT encoding/CRC conventions derive from pyxis-userland installer/gpt.c,
 * revision d2437266d45989c1e73e1ceba995600e4d2f9787, under MPL-2.0.
 * Validation follows the existing Caelum GPT discovery profile. */
#include "internal.h"

#define GPT_HEADER_BYTES 92u
#define GPT_ENTRY_BYTES 128u
#define GPT_ARRAY_RESERVE_SECTORS 32u
#define GPT_REVISION UINT32_C(0x00010000)
#define GPT_RESERVED_ATTRIBUTES UINT64_C(0x0000fffffffffff8)

struct gpt_header {
  uint64_t first_usable, last_usable, array_first;
  uint32_t header_bytes, entry_count, entry_bytes, array_bytes, array_crc;
  uint8_t disk_guid[16];
};

static enum fat32_status protective_mbr(struct fat32_context *context)
{
  enum fat32_status status = fat32_disk_read(context, 0, 1, context->data);
  if (status != FAT32_OK) {
    return status;
  }
  if (fat32_get16(context->data + 510) != UINT16_C(0xaa55)) {
    return FAT32_UNSUPPORTED;
  }
  unsigned protective = 0;
  uint32_t expected = context->source->sectors - 1 > UINT32_MAX ? UINT32_MAX :
      (uint32_t)(context->source->sectors - 1);
  for (unsigned i = 0; i < 4; ++i) {
    const uint8_t *entry = context->data + 446 + 16 * i;
    if (entry[4] == 0xee) {
      ++protective;
      if (entry[0] || fat32_get32(entry + 8) != 1 || fat32_get32(entry + 12) != expected) {
        return FAT32_CORRUPT;
      }
    } else if (entry[4]) {
      return FAT32_UNSUPPORTED;
    } else if (!fat32_all_zero(entry, 16)) {
      return FAT32_CORRUPT;
    }
  }
  return protective == 1 ? FAT32_OK : protective ? FAT32_CORRUPT : FAT32_UNSUPPORTED;
}

static enum fat32_status read_header(struct fat32_context *context, bool backup,
    struct gpt_header *header)
{
  uint64_t last = context->source->sectors - 1;
  enum fat32_status status = fat32_disk_read(context, backup ? last : 1, 1, context->data);
  if (status != FAT32_OK) {
    return status;
  }
  uint8_t *bytes = context->data;
  uint32_t header_bytes = fat32_get32(bytes + 12), expected_crc = fat32_get32(bytes + 16);
  if (!fat32_equal(bytes, "EFI PART", 8) ||
      header_bytes < GPT_HEADER_BYTES || header_bytes > FAT32_SECTOR_BYTES) {
    return FAT32_CORRUPT;
  }
  fat32_zero(bytes + 16, 4);
  if (fat32_crc(bytes, header_bytes) != expected_crc) {
    return FAT32_CORRUPT;
  }
  if (fat32_get32(bytes + 8) != GPT_REVISION) {
    return FAT32_UNSUPPORTED;
  }
  if (fat32_get32(bytes + 20) ||
      !fat32_all_zero(bytes + GPT_HEADER_BYTES, FAT32_SECTOR_BYTES - GPT_HEADER_BYTES) ||
      fat32_get64(bytes + 24) != (backup ? last : 1) ||
      fat32_get64(bytes + 32) != (backup ? 1 : last)) {
    return FAT32_CORRUPT;
  }
  header->first_usable = fat32_get64(bytes + 40);
  header->last_usable = fat32_get64(bytes + 48);
  header->array_first = fat32_get64(bytes + 72);
  header->header_bytes = header_bytes;
  header->entry_count = fat32_get32(bytes + 80);
  header->entry_bytes = fat32_get32(bytes + 84);
  header->array_crc = fat32_get32(bytes + 88);
  fat32_copy(header->disk_guid, bytes + 56, 16);
  if (fat32_all_zero(header->disk_guid, 16) || !header->entry_count ||
      header->entry_bytes < GPT_ENTRY_BYTES ||
      (header->entry_bytes & (header->entry_bytes - 1))) {
    return FAT32_CORRUPT;
  }
  uint64_t array_bytes = (uint64_t)header->entry_count * header->entry_bytes;
  if (header->entry_count > FAT32_GPT_ENTRY_LIMIT || array_bytes > FAT32_GPT_ARRAY_LIMIT) {
    return FAT32_UNSUPPORTED;
  }
  header->array_bytes = (uint32_t)array_bytes;
  uint64_t array_sectors = (array_bytes + FAT32_SECTOR_BYTES - 1) / FAT32_SECTOR_BYTES;
  uint64_t reserved = array_sectors > GPT_ARRAY_RESERVE_SECTORS ?
      array_sectors : GPT_ARRAY_RESERVE_SECTORS;
  if (reserved >= last || header->first_usable < 2 + reserved ||
      header->first_usable > header->last_usable || header->last_usable >= last - reserved ||
      header->array_first >= last || array_sectors > last - header->array_first ||
      (backup ? header->array_first <= header->last_usable :
       (header->array_first < 2 || header->array_first + array_sectors > header->first_usable))) {
    return FAT32_CORRUPT;
  }
  return FAT32_OK;
}

static enum fat32_status validate_entries(struct fat32_context *context,
    const struct gpt_header *header, const uint8_t *array)
{
  if (fat32_crc(array, header->array_bytes) != header->array_crc) {
    return FAT32_CORRUPT;
  }
  for (uint32_t i = 0; i < header->entry_count; ++i) {
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK) {
      return status;
    }
    const uint8_t *entry = array + (size_t)i * header->entry_bytes;
    if (!fat32_all_zero(entry + GPT_ENTRY_BYTES, header->entry_bytes - GPT_ENTRY_BYTES)) {
      return FAT32_CORRUPT;
    }
    if (fat32_all_zero(entry, 16)) {
      continue;
    }
    uint64_t first = fat32_get64(entry + 32), last = fat32_get64(entry + 40);
    if (fat32_all_zero(entry + 16, 16) || first > last || first < header->first_usable ||
        last > header->last_usable) {
      return FAT32_CORRUPT;
    }
    if (fat32_get64(entry + 48) & GPT_RESERVED_ATTRIBUTES) {
      return FAT32_UNSUPPORTED;
    }
    for (uint32_t j = 0; j < i; ++j) {
      status = fat32_step(context);
      if (status != FAT32_OK) {
        return status;
      }
      const uint8_t *other = array + (size_t)j * header->entry_bytes;
      if (!fat32_all_zero(other, 16) &&
          (fat32_equal(entry + 16, other + 16, 16) ||
           (first <= fat32_get64(other + 40) && fat32_get64(other + 32) <= last))) {
        return FAT32_CORRUPT;
      }
    }
  }
  return FAT32_OK;
}

enum fat32_status fat32_gpt_select(struct fat32_context *context, uint32_t entry,
    uint8_t *array, struct fat32_volume *volume)
{
  if (context->source->sectors < 68 || !entry) {
    return FAT32_INVALID;
  }
  enum fat32_status status = protective_mbr(context);
  struct gpt_header primary, backup;
  if (status != FAT32_OK) {
    return status;
  }
  status = read_header(context, false, &primary);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_disk_read(context, primary.array_first,
      (primary.array_bytes + FAT32_SECTOR_BYTES - 1) / FAT32_SECTOR_BYTES, array);
  if (status != FAT32_OK) {
    return status;
  }
  status = validate_entries(context, &primary, array);
  if (status != FAT32_OK) {
    return status;
  }
  status = read_header(context, true, &backup);
  if (status != FAT32_OK) {
    return status;
  }
  if (primary.header_bytes != backup.header_bytes ||
      primary.first_usable != backup.first_usable || primary.last_usable != backup.last_usable ||
      primary.entry_count != backup.entry_count || primary.entry_bytes != backup.entry_bytes ||
      primary.array_crc != backup.array_crc || !fat32_equal(primary.disk_guid, backup.disk_guid, 16)) {
    return FAT32_CORRUPT;
  }
  for (uint32_t offset = 0; offset < primary.array_bytes; offset += FAT32_SECTOR_BYTES) {
    status = fat32_disk_read(context, backup.array_first + offset / FAT32_SECTOR_BYTES,
        1, context->data);
    if (status != FAT32_OK) {
      return status;
    }
    size_t length = primary.array_bytes - offset;
    if (length > FAT32_SECTOR_BYTES) {
      length = FAT32_SECTOR_BYTES;
    }
    if (!fat32_equal(array + offset, context->data, length)) {
      return FAT32_CORRUPT;
    }
  }
  if (entry > primary.entry_count) {
    return FAT32_NOT_FOUND;
  }
  const uint8_t *selected = array + (size_t)(entry - 1) * primary.entry_bytes;
  if (fat32_all_zero(selected, 16)) {
    return FAT32_NOT_FOUND;
  }
  volume->partition_first = fat32_get64(selected + 32);
  volume->partition_sectors = fat32_get64(selected + 40) - volume->partition_first + 1;
  volume->partition_attributes = fat32_get64(selected + 48);
  volume->gpt_entry = entry;
  fat32_copy(volume->disk_guid, primary.disk_guid, 16);
  fat32_copy(volume->partition_guid, selected + 16, 16);
  fat32_copy(volume->partition_type, selected, 16);
  return FAT32_OK;
}
