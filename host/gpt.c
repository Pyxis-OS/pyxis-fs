/* SPDX-License-Identifier: MPL-2.0 */
#include "host.h"

#include <string.h>

#define GPT_ARRAY_LIMIT 65536u
#define GPT_ARRAY_RESERVE 16384u
#define GPT_SECTOR_LIMIT 4096u
#define GPT_HEADER_BYTES 92u
#define GPT_ENTRY_BYTES 128u
#define GPT_ENTRY_LIMIT 256u
#define GPT_REVISION UINT32_C(0x00010000)
#define GPT_CRC_POLYNOMIAL UINT32_C(0xedb88320)
#define GPT_RESERVED_ATTRIBUTES UINT64_C(0x0000fffffffffff8)
#define MBR_SIGNATURE UINT16_C(0xaa55)
#define MBR_SIGNATURE_OFFSET 510u
#define MBR_ENTRIES_OFFSET 446u
#define MBR_ENTRY_BYTES 16u
#define MBR_ENTRY_COUNT 4u
#define MBR_PROTECTIVE_TYPE 0xeeu

struct gpt_header {
  uint8_t disk_guid[16];
  uint64_t first_usable;
  uint64_t last_usable;
  uint64_t array_lba;
  uint32_t header_bytes;
  uint32_t entry_count;
  uint32_t entry_bytes;
  uint32_t array_bytes;
  uint32_t array_crc;
};

struct gpt_scratch {
  uint8_t sector[GPT_SECTOR_LIMIT];
  uint8_t arrays[2][GPT_ARRAY_LIMIT];
  struct gpt_header headers[2];
};

struct gpt_scan {
  struct host_image *image;
  struct gpt_scratch *scratch;
  uint64_t sector_count;
  uint32_t sector_size;
};

static uint16_t
read_le16(const uint8_t *bytes)
{
  return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t
read_le32(const uint8_t *bytes)
{
  return (uint32_t)read_le16(bytes) | (uint32_t)read_le16(bytes + 2) << 16;
}

static uint64_t
read_le64(const uint8_t *bytes)
{
  return (uint64_t)read_le32(bytes) | (uint64_t)read_le32(bytes + 4) << 32;
}

static bool
zero_bytes(const uint8_t *bytes, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

/* GPT uses IEEE CRC32, independently of the filesystem's CRC32C. */
static uint32_t
gpt_crc32(const uint8_t *bytes, size_t count)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? GPT_CRC_POLYNOMIAL : 0);
    }
  }
  return ~crc;
}

static enum host_gpt_mbr_status
validate_mbr(const struct gpt_scan *scan)
{
  const uint8_t *sector = scan->scratch->sector;
  if (read_le16(sector + MBR_SIGNATURE_OFFSET) != MBR_SIGNATURE) {
    return HOST_GPT_MBR_ABSENT;
  }
  unsigned protective = 0;
  bool invalid = false;
  uint32_t expected = scan->sector_count - 1 > UINT32_MAX ? UINT32_MAX :
      (uint32_t)(scan->sector_count - 1);
  for (unsigned i = 0; i < MBR_ENTRY_COUNT; ++i) {
    const uint8_t *entry = sector + MBR_ENTRIES_OFFSET + i * MBR_ENTRY_BYTES;
    if (entry[4] == MBR_PROTECTIVE_TYPE) {
      ++protective;
      if (read_le32(entry + 8) != 1 || read_le32(entry + 12) != expected) {
        invalid = true;
      }
    } else if (entry[4] != 0) {
      return HOST_GPT_MBR_UNSUPPORTED;
    } else if (!zero_bytes(entry, MBR_ENTRY_BYTES)) {
      invalid = true;
    }
  }
  if (invalid || protective > 1 ||
      !zero_bytes(sector + 512, scan->sector_size - 512)) {
    return HOST_GPT_MBR_INVALID;
  }
  return protective == 1 ? HOST_GPT_MBR_VALID : HOST_GPT_MBR_ABSENT;
}

static enum host_gpt_copy_status
validate_header(struct gpt_scan *scan, unsigned copy)
{
  uint8_t *sector = scan->scratch->sector;
  struct gpt_header *header = &scan->scratch->headers[copy];
  uint64_t last = scan->sector_count - 1;
  uint64_t location = copy == 0 ? 1 : last;
  if (memcmp(sector, "EFI PART", 8) != 0) {
    return HOST_GPT_COPY_ABSENT;
  }
  uint32_t bytes = read_le32(sector + 12);
  if (bytes < GPT_HEADER_BYTES || bytes > scan->sector_size) {
    return HOST_GPT_COPY_INVALID;
  }
  uint32_t expected_crc = read_le32(sector + 16);
  memset(sector + 16, 0, sizeof(uint32_t));
  if (gpt_crc32(sector, bytes) != expected_crc) {
    return HOST_GPT_COPY_INVALID;
  }
  if (read_le32(sector + 8) != GPT_REVISION) {
    return HOST_GPT_COPY_UNSUPPORTED;
  }
  if (read_le32(sector + 20) != 0 ||
      !zero_bytes(sector + GPT_HEADER_BYTES, scan->sector_size - GPT_HEADER_BYTES) ||
      read_le64(sector + 24) != location ||
      read_le64(sector + 32) != (copy == 0 ? last : 1)) {
    return HOST_GPT_COPY_INVALID;
  }
  *header = (struct gpt_header){
    .header_bytes = bytes,
    .first_usable = read_le64(sector + 40),
    .last_usable = read_le64(sector + 48),
    .array_lba = read_le64(sector + 72),
    .entry_count = read_le32(sector + 80),
    .entry_bytes = read_le32(sector + 84),
    .array_crc = read_le32(sector + 88),
  };
  memcpy(header->disk_guid, sector + 56, sizeof(header->disk_guid));
  if (zero_bytes(header->disk_guid, sizeof(header->disk_guid)) ||
      header->entry_count == 0 || header->entry_bytes < GPT_ENTRY_BYTES ||
      (header->entry_bytes & (header->entry_bytes - 1)) != 0) {
    return HOST_GPT_COPY_INVALID;
  }
  /* The two u32 factors fit u64. A valid power-of-two stride is at most
   * 2^31, leaving room for the sector-rounding addition. */
  uint64_t array_bytes = (uint64_t)header->entry_count * header->entry_bytes;
  uint64_t array_sectors = (array_bytes + scan->sector_size - 1) / scan->sector_size;
  uint64_t reserved = array_sectors;
  if (reserved < GPT_ARRAY_RESERVE / scan->sector_size) {
    reserved = GPT_ARRAY_RESERVE / scan->sector_size;
  }
  if (reserved >= last || header->first_usable < 2 + reserved ||
      header->first_usable > header->last_usable ||
      header->last_usable >= last - reserved || header->array_lba >= last ||
      array_sectors > last - header->array_lba ||
      (copy == 0 ? (header->array_lba < 2 ||
                    header->array_lba + array_sectors > header->first_usable) :
                   header->array_lba <= header->last_usable)) {
    return HOST_GPT_COPY_INVALID;
  }
  if (header->entry_count > GPT_ENTRY_LIMIT || array_bytes > GPT_ARRAY_LIMIT) {
    return HOST_GPT_COPY_UNSUPPORTED;
  }
  header->array_bytes = (uint32_t)array_bytes;
  return HOST_GPT_COPY_VALID;
}

static enum host_gpt_copy_status
validate_entries(const struct gpt_scan *scan, unsigned copy)
{
  const struct gpt_header *header = &scan->scratch->headers[copy];
  const uint8_t *array = scan->scratch->arrays[copy];
  if (gpt_crc32(array, header->array_bytes) != header->array_crc) {
    return HOST_GPT_COPY_INVALID;
  }
  for (uint32_t i = 0; i < header->entry_count; ++i) {
    const uint8_t *entry = array + i * header->entry_bytes;
    if (!zero_bytes(entry + GPT_ENTRY_BYTES, header->entry_bytes - GPT_ENTRY_BYTES)) {
      return HOST_GPT_COPY_INVALID;
    }
    if (zero_bytes(entry, 16)) {
      continue;
    }
    uint64_t first = read_le64(entry + 32);
    uint64_t last = read_le64(entry + 40);
    if (zero_bytes(entry + 16, 16) || first > last || first < header->first_usable ||
        last > header->last_usable) {
      return HOST_GPT_COPY_INVALID;
    }
    if ((read_le64(entry + 48) & GPT_RESERVED_ATTRIBUTES) != 0) {
      return HOST_GPT_COPY_UNSUPPORTED;
    }
    for (uint32_t j = 0; j < i; ++j) {
      const uint8_t *other = array + j * header->entry_bytes;
      if (!zero_bytes(other, 16) &&
          (memcmp(entry + 16, other + 16, 16) == 0 ||
           (first <= read_le64(other + 40) && read_le64(other + 32) <= last))) {
        return HOST_GPT_COPY_INVALID;
      }
    }
  }
  return HOST_GPT_COPY_VALID;
}

static enum host_gpt_copy_status
scan_copy(struct gpt_scan *scan, unsigned copy)
{
  uint64_t lba = copy == 0 ? 1 : scan->sector_count - 1;
  const char *operation = copy == 0 ? "read primary GPT header" : "read backup GPT header";
  enum pfs_status result = host_image_read_bytes(scan->image, lba * scan->sector_size,
      scan->sector_size, scan->scratch->sector, operation, lba);
  if (result != PFS_OK) {
    return HOST_GPT_COPY_IO;
  }
  enum host_gpt_copy_status status = validate_header(scan, copy);
  if (status != HOST_GPT_COPY_VALID) {
    return status;
  }
  const struct gpt_header *header = &scan->scratch->headers[copy];
  size_t length = (header->array_bytes + scan->sector_size - 1) /
      scan->sector_size * scan->sector_size;
  operation = copy == 0 ? "read primary GPT array" : "read backup GPT array";
  result = host_image_read_bytes(scan->image, header->array_lba * scan->sector_size,
      length, scan->scratch->arrays[copy], operation, header->array_lba);
  return result == PFS_OK ? validate_entries(scan, copy) : HOST_GPT_COPY_IO;
}

static bool
copies_agree(const struct gpt_scratch *scratch)
{
  const struct gpt_header *primary = &scratch->headers[0];
  const struct gpt_header *backup = &scratch->headers[1];
  return primary->header_bytes == backup->header_bytes &&
      primary->first_usable == backup->first_usable &&
      primary->last_usable == backup->last_usable &&
      primary->entry_count == backup->entry_count &&
      primary->entry_bytes == backup->entry_bytes &&
      memcmp(primary->disk_guid, backup->disk_guid, sizeof(primary->disk_guid)) == 0 &&
      memcmp(scratch->arrays[0], scratch->arrays[1], primary->array_bytes) == 0;
}

static enum pfs_status
choose_map(const struct gpt_scratch *scratch, struct host_gpt_diagnostic *diagnostic)
{
  enum host_gpt_copy_status primary = diagnostic->primary;
  enum host_gpt_copy_status backup = diagnostic->backup;
  if (primary == HOST_GPT_COPY_IO || backup == HOST_GPT_COPY_IO) {
    diagnostic->status = HOST_GPT_IO;
    return PFS_IO;
  }
  if (diagnostic->mbr == HOST_GPT_MBR_UNSUPPORTED ||
      primary == HOST_GPT_COPY_UNSUPPORTED || backup == HOST_GPT_COPY_UNSUPPORTED) {
    diagnostic->status = HOST_GPT_UNSUPPORTED;
    return PFS_UNSUPPORTED;
  }
  if (diagnostic->mbr == HOST_GPT_MBR_ABSENT && primary == HOST_GPT_COPY_ABSENT &&
      backup == HOST_GPT_COPY_ABSENT) {
    diagnostic->status = HOST_GPT_ABSENT;
    return PFS_ABSENT;
  }
  if (diagnostic->mbr != HOST_GPT_MBR_VALID) {
    diagnostic->status = HOST_GPT_INVALID;
    return PFS_CORRUPT;
  }
  if (primary == HOST_GPT_COPY_VALID && backup == HOST_GPT_COPY_VALID) {
    if (!copies_agree(scratch)) {
      diagnostic->status = HOST_GPT_AMBIGUOUS;
      return PFS_CORRUPT;
    }
    diagnostic->status = HOST_GPT_HEALTHY;
    diagnostic->selected_copy = 1;
    return PFS_OK;
  }
  if (primary == HOST_GPT_COPY_VALID || backup == HOST_GPT_COPY_VALID) {
    diagnostic->status = HOST_GPT_DEGRADED;
    diagnostic->degraded = true;
    diagnostic->selected_copy = primary == HOST_GPT_COPY_VALID ? 1 : 2;
    return PFS_OK;
  }
  diagnostic->status = HOST_GPT_INVALID;
  return PFS_CORRUPT;
}

enum pfs_status
host_gpt_select(struct host_image *image, struct pfs_memory *memory,
                  const struct host_gpt_selection *selection,
                  struct host_gpt_diagnostic *diagnostic)
{
  *diagnostic = (struct host_gpt_diagnostic){0};
  if (selection == NULL || selection->partition_entry == 0 ||
      (selection->sector_size != 512 && selection->sector_size != 4096)) {
    return PFS_INVALID;
  }
  if (image->bytes % selection->sector_size != 0) {
    diagnostic->status = HOST_GPT_UNSUPPORTED;
    image->operation = "GPT image sector geometry";
    return PFS_UNSUPPORTED;
  }
  uint64_t sectors = image->bytes / selection->sector_size;
  if (sectors < 3) {
    diagnostic->status = HOST_GPT_INVALID;
    image->operation = "GPT image too small";
    return PFS_CORRUPT;
  }
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(memory, sizeof(struct gpt_scratch),
                                               _Alignof(struct gpt_scratch), &allocation);
  if (status != PFS_OK) {
    diagnostic->status = status == PFS_LIMIT ? HOST_GPT_LIMIT : HOST_GPT_NO_MEMORY;
    image->operation = "allocate GPT scratch";
    return status;
  }
  struct gpt_scan scan = {
    .image = image,
    .scratch = allocation.data,
    .sector_count = sectors,
    .sector_size = selection->sector_size,
  };
  status = host_image_read_bytes(image, 0, scan.sector_size, scan.scratch->sector,
                                 "read protective MBR", 0);
  if (status != PFS_OK) {
    diagnostic->mbr = HOST_GPT_MBR_IO;
    diagnostic->status = HOST_GPT_IO;
  } else {
    diagnostic->mbr = validate_mbr(&scan);
    diagnostic->primary = scan_copy(&scan, 0);
    diagnostic->backup = scan_copy(&scan, 1);
    status = choose_map(scan.scratch, diagnostic);
    if (status != PFS_OK && status != PFS_IO) {
      image->operation = diagnostic->mbr != HOST_GPT_MBR_VALID ?
          "protective MBR gate" : "GPT copy selection";
    }
  }
  if (status == PFS_OK) {
    unsigned copy = diagnostic->selected_copy - 1;
    const struct gpt_header *header = &scan.scratch->headers[copy];
    if (selection->partition_entry > header->entry_count) {
      status = PFS_NOT_FOUND;
    } else {
      const uint8_t *entry = scan.scratch->arrays[copy] +
          (selection->partition_entry - 1) * header->entry_bytes;
      if (zero_bytes(entry, 16)) {
        status = PFS_NOT_FOUND;
      } else {
        uint64_t first = read_le64(entry + 32);
        uint64_t count = read_le64(entry + 40) - first + 1;
        /* Validated extents end before the final image sector, so both
         * products and their sum fit the already measured whole-file size. */
        diagnostic->offset_bytes = first * scan.sector_size;
        diagnostic->length_bytes = count * scan.sector_size;
      }
    }
    if (status == PFS_NOT_FOUND) {
      image->operation = "select used GPT partition entry";
    }
  }
  pfs_memory_free(memory, &allocation);
  return status;
}

const char *
host_gpt_status_string(enum host_gpt_status status)
{
  switch (status) {
    case HOST_GPT_UNEXAMINED: return "unexamined";
    case HOST_GPT_HEALTHY: return "healthy";
    case HOST_GPT_DEGRADED: return "degraded (read-only)";
    case HOST_GPT_AMBIGUOUS: return "ambiguous";
    case HOST_GPT_ABSENT: return "absent";
    case HOST_GPT_INVALID: return "invalid";
    case HOST_GPT_UNSUPPORTED: return "unsupported";
    case HOST_GPT_LIMIT: return "limit";
    case HOST_GPT_NO_MEMORY: return "no memory";
    case HOST_GPT_IO: return "I/O error";
  }
  return "unknown";
}

const char *
host_gpt_copy_status_string(enum host_gpt_copy_status status)
{
  switch (status) {
    case HOST_GPT_COPY_UNEXAMINED: return "unexamined";
    case HOST_GPT_COPY_VALID: return "valid";
    case HOST_GPT_COPY_ABSENT: return "absent";
    case HOST_GPT_COPY_INVALID: return "invalid";
    case HOST_GPT_COPY_UNSUPPORTED: return "unsupported";
    case HOST_GPT_COPY_IO: return "I/O error";
  }
  return "unknown";
}

const char *
host_gpt_mbr_status_string(enum host_gpt_mbr_status status)
{
  switch (status) {
    case HOST_GPT_MBR_UNEXAMINED: return "unexamined";
    case HOST_GPT_MBR_VALID: return "valid";
    case HOST_GPT_MBR_ABSENT: return "absent";
    case HOST_GPT_MBR_INVALID: return "invalid";
    case HOST_GPT_MBR_UNSUPPORTED: return "unsupported";
    case HOST_GPT_MBR_IO: return "I/O error";
  }
  return "unknown";
}
