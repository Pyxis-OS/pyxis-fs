/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

#define CRC32C_POLYNOMIAL UINT32_C(0x82f63b78)

const char *
pnf_status_string(enum pnf_status status)
{
  switch (status) {
  case PNF_OK: return "success";
  case PNF_INVALID: return "invalid argument";
  case PNF_CORRUPT: return "corrupt metadata";
  case PNF_UNSUPPORTED: return "unsupported format or feature";
  case PNF_IO: return "I/O failure";
  case PNF_NO_MEMORY: return "allocation failure";
  case PNF_NO_SPACE: return "insufficient space";
  case PNF_RECOVERY_REQUIRED: return "journal replay required";
  case PNF_NOT_FOUND: return "not found";
  case PNF_EXISTS: return "already exists";
  }
  return "unknown status";
}

uint16_t
pnf_get_u16(const void *bytes)
{
  const uint8_t *p = bytes;
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

uint32_t
pnf_get_u32(const void *bytes)
{
  const uint8_t *p = bytes;
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= (uint32_t)p[i] << (8 * i);
  }
  return value;
}

uint64_t
pnf_get_u64(const void *bytes)
{
  const uint8_t *p = bytes;
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= (uint64_t)p[i] << (8 * i);
  }
  return value;
}

void
pnf_put_u16(void *bytes, uint16_t value)
{
  uint8_t *p = bytes;
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

void
pnf_put_u32(void *bytes, uint32_t value)
{
  uint8_t *p = bytes;
  for (unsigned i = 0; i < 4; ++i) {
    p[i] = (uint8_t)(value >> (8 * i));
  }
}

void
pnf_put_u64(void *bytes, uint64_t value)
{
  uint8_t *p = bytes;
  for (unsigned i = 0; i < 8; ++i) {
    p[i] = (uint8_t)(value >> (8 * i));
  }
}

bool
pnf_flags_valid(const struct pnf_header *header, uint32_t flags, uint32_t known)
{
  /* Ignorable pool extensions may define flags unknown to this reader. */
  return (flags & ~known) == 0 || header->compatible != 0 ||
         header->read_only_compatible != 0;
}

bool
pnf_bytes_zero(const void *bytes, size_t length)
{
  const uint8_t *p = bytes;
  for (size_t i = 0; i < length; ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

bool
pnf_bytes_equal(const void *left, const void *right, size_t length)
{
  const uint8_t *a = left;
  const uint8_t *b = right;
  for (size_t i = 0; i < length; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

uint32_t
pnf_crc_begin(void)
{
  return UINT32_MAX;
}

uint32_t
pnf_crc_update(uint32_t crc, const void *bytes, size_t length)
{
  const uint8_t *p = bytes;
  for (size_t i = 0; i < length; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? CRC32C_POLYNOMIAL : 0);
    }
  }
  return crc;
}

uint32_t
pnf_crc_finish(uint32_t crc)
{
  return crc ^ UINT32_MAX;
}

uint32_t
pnf_crc32c(const void *bytes, size_t length)
{
  return pnf_crc_finish(pnf_crc_update(pnf_crc_begin(), bytes, length));
}

uint32_t
pnf_block_crc(const uint8_t *block, unsigned checksum_offset)
{
  static const uint8_t zeros[4];
  uint32_t crc = pnf_crc_update(pnf_crc_begin(), block, checksum_offset);
  crc = pnf_crc_update(crc, zeros, sizeof(zeros));
  crc = pnf_crc_update(crc, block + checksum_offset + sizeof(zeros),
                       PNF_BLOCK_SIZE - checksum_offset - sizeof(zeros));
  return pnf_crc_finish(crc);
}

bool
pnf_id_valid(const uint8_t id[PNF_ID_SIZE])
{
  return id != NULL && !pnf_bytes_zero(id, PNF_ID_SIZE);
}

bool
pnf_name_valid(const uint8_t *name, size_t length)
{
  if (name == NULL || length == 0 || length > PNF_NAME_MAX ||
      (length == 1 && name[0] == '.') ||
      (length == 2 && name[0] == '.' && name[1] == '.')) {
    return false;
  }
  size_t offset = 0;
  while (offset < length) {
    uint8_t first = name[offset++];
    if (first == 0 || first == '/') {
      return false;
    }
    if (first < 0x80) {
      continue;
    }
    unsigned following;
    uint32_t value;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      following = 1;
      value = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      following = 2;
      value = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      following = 3;
      value = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (following > length - offset) {
      return false;
    }
    for (unsigned i = 0; i < following; ++i) {
      uint8_t byte = name[offset++];
      if ((byte & 0xc0) != 0x80) {
        return false;
      }
      value = (value << 6) | (byte & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

int64_t
pnf_timestamp(int64_t seconds, uint32_t nanoseconds)
{
  __int128 value = (__int128)seconds * 1000000000 + nanoseconds;
  if (value < INT64_MIN) {
    return INT64_MIN;
  }
  if (value > INT64_MAX) {
    return INT64_MAX;
  }
  return (int64_t)value;
}

uint64_t
pnf_bitmap_blocks(uint64_t pool_blocks)
{
  return pool_blocks / PNF_BITMAP_BITS + (pool_blocks % PNF_BITMAP_BITS != 0);
}

uint64_t
pnf_journal_capacity(uint64_t journal_blocks)
{
  if (journal_blocks < 4) {
    return 0;
  }
  uint64_t lo = 0;
  uint64_t hi = journal_blocks - 2;
  if (hi > UINT32_MAX) {
    hi = UINT32_MAX;
  }
  while (lo < hi) {
    uint64_t middle = lo + (hi - lo + 1) / 2;
    uint64_t descriptors = middle / PNF_DESCRIPTORS_PER_BLOCK +
                           (middle % PNF_DESCRIPTORS_PER_BLOCK != 0);
    if (middle + descriptors <= journal_blocks - 2) {
      lo = middle;
    } else {
      hi = middle - 1;
    }
  }
  return lo;
}

bool
pnf_data_block_valid(const struct pnf_header *header, uint64_t block)
{
  if (header == NULL || block == 0 || header->pool_blocks < 2 ||
      block >= header->pool_blocks - 1) {
    return false;
  }
  return !(block >= header->bitmap_start &&
           block - header->bitmap_start < header->bitmap_blocks) &&
         !(block >= header->volume_start &&
           block - header->volume_start < header->volume_blocks) &&
         !(block >= header->journal_start &&
           block - header->journal_start < header->journal_blocks);
}

bool
pnf_pointers_valid(const struct pnf_header *header,
                    const uint64_t pointers[PNF_POINTER_COUNT])
{
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    if (pointers[i] != 0 && !pnf_data_block_valid(header, pointers[i])) {
      return false;
    }
  }
  return true;
}

enum pnf_status
pnf_map_path(uint64_t logical, struct pnf_map_path *path)
{
  if (path == NULL || logical >= PNF_FILE_BLOCKS_MAX) {
    return PNF_INVALID;
  }
  struct pnf_map_path result;
  pnf_memory_zero(&result, sizeof(result));
  if (logical < PNF_DIRECT_COUNT) {
    result.slot = (unsigned)logical;
  } else {
    logical -= PNF_DIRECT_COUNT;
    uint64_t capacity = PNF_INDIRECT_COUNT;
    unsigned depth = 1;
    while (logical >= capacity) {
      logical -= capacity;
      capacity *= PNF_INDIRECT_COUNT;
      ++depth;
    }
    result.slot = PNF_DIRECT_COUNT + depth - 1;
    result.depth = depth;
    for (unsigned level = 0; level < depth; ++level) {
      capacity /= PNF_INDIRECT_COUNT;
      result.index[level] = (unsigned)(logical / capacity);
      logical %= capacity;
    }
  }
  pnf_memory_copy(path, &result, sizeof(result));
  return PNF_OK;
}
