#include "internal.h"

#define CRC32C_POLYNOMIAL UINT32_C(0x82f63b78)
#define RECORD_CHECKSUM_OFFSET 8u
#define CHECKSUM_SIZE 4u

const char *
pfs_status_string(enum pfs_status status)
{
  switch (status) {
  case PFS_OK: return "success";
  case PFS_INVALID: return "invalid argument";
  case PFS_ABSENT: return "absent";
  case PFS_CORRUPT: return "corrupt";
  case PFS_UNSUPPORTED: return "unsupported";
  case PFS_LIMIT: return "limit exceeded";
  case PFS_IO: return "I/O failure";
  case PFS_NO_MEMORY: return "allocation failure";
  case PFS_READ_ONLY: return "read only";
  case PFS_BUSY: return "busy";
  case PFS_DENIED: return "permission denied";
  case PFS_NOT_FOUND: return "not found";
  case PFS_NO_SPACE: return "no space";
  case PFS_QUOTA: return "quota exceeded";
  case PFS_RECOVERY_REQUIRED: return "recovery required";
  }
  return "unknown status";
}

uint16_t
pfs_get_u16(const uint8_t *p)
{
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

uint32_t
pfs_get_u32(const uint8_t *p)
{
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= (uint32_t)p[i] << (8 * i);
  }
  return value;
}

uint64_t
pfs_get_u64(const uint8_t *p)
{
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= (uint64_t)p[i] << (8 * i);
  }
  return value;
}

void
pfs_put_u16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

void
pfs_put_u32(uint8_t *p, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) {
    p[i] = (uint8_t)(value >> (8 * i));
  }
}

void
pfs_put_u64(uint8_t *p, uint64_t value)
{
  for (unsigned i = 0; i < 8; ++i) {
    p[i] = (uint8_t)(value >> (8 * i));
  }
}

void
pfs_bytes_copy(void *destination, const void *source, size_t length)
{
  uint8_t *out = destination;
  const uint8_t *in = source;
  for (size_t i = 0; i < length; ++i) {
    out[i] = in[i];
  }
}

void
pfs_bytes_zero(void *data, size_t length)
{
  uint8_t *bytes = data;
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = 0;
  }
}

bool
pfs_bytes_are_zero(const void *data, size_t length)
{
  const uint8_t *bytes = data;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

int
pfs_bytes_compare(const void *a, size_t a_length, const void *b, size_t b_length)
{
  const uint8_t *left = a;
  const uint8_t *right = b;
  size_t common = a_length < b_length ? a_length : b_length;
  for (size_t i = 0; i < common; ++i) {
    if (left[i] != right[i]) {
      return left[i] < right[i] ? -1 : 1;
    }
  }
  return (a_length > b_length) - (a_length < b_length);
}

uint32_t
pfs_crc32c_zeroed(const uint8_t *data, size_t length, size_t offset, size_t count)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = i >= offset && i - offset < count ? 0 : data[i];
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? CRC32C_POLYNOMIAL : 0);
    }
  }
  return crc ^ UINT32_MAX;
}

uint32_t
pfs_crc32c(const void *data, size_t length)
{
  return pfs_crc32c_zeroed(data, length, 0, 0);
}

bool
pfs_range_valid(uint64_t first, uint64_t count, uint64_t limit)
{
  return first <= limit && count <= limit - first;
}

bool
pfs_allocatable_range(uint64_t first, uint64_t count, uint64_t blocks)
{
  return blocks >= 2 && first >= 1 && count != 0 &&
         pfs_range_valid(first, count, blocks - 1);
}

enum pfs_status
pfs_features_read(const struct pfs_features *features)
{
  if (!features) {
    return PFS_INVALID;
  }
  return features->read_required ? PFS_UNSUPPORTED : PFS_OK;
}

enum pfs_status
pfs_features_write(const struct pfs_features *features)
{
  if (!features) {
    return PFS_INVALID;
  }
  return features->read_required || features->write_required || features->optional
           ? PFS_UNSUPPORTED : PFS_OK;
}

enum pfs_status
pfs_features_check(const struct pfs_features *features)
{
  return pfs_features_write(features);
}

enum pfs_status
pfs_volume_features_read(const struct pfs_features *features)
{
  if (!features) {
    return PFS_INVALID;
  }
  return features->read_required & ~PFS_FEATURE_ORPHANS ? PFS_UNSUPPORTED : PFS_OK;
}

enum pfs_status
pfs_volume_features_write(const struct pfs_features *features)
{
  enum pfs_status status = pfs_volume_features_read(features);
  if (status != PFS_OK) {
    return status;
  }
  return features->write_required || features->optional ? PFS_UNSUPPORTED : PFS_OK;
}

enum pfs_status
pfs_volume_features_check(const struct pfs_features *features)
{
  return pfs_volume_features_write(features);
}

enum pfs_status
pfs_context_validate(const struct pfs_record_context *context)
{
  if (!context || context->block_count < 3 || !context->selected_generation ||
      !context->containing_birth || context->containing_birth > context->selected_generation) {
    return PFS_INVALID;
  }
  if (context->block_count < PFS_POOL_BLOCKS_MIN || context->block_count > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  return pfs_volume_features_read(&context->features);
}

enum pfs_status
pfs_name_validate(const uint8_t *bytes, size_t length)
{
  if (!bytes || !length || length > PFS_NAME_MAX ||
      (bytes[0] == '.' && (length == 1 || (length == 2 && bytes[1] == '.')))) {
    return PFS_INVALID;
  }
  for (size_t i = 0; i < length;) {
    uint8_t first = bytes[i++];
    if (!first || first == '/') {
      return PFS_INVALID;
    }
    if (first < 0x80) {
      continue;
    }
    unsigned continuation;
    uint32_t codepoint;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
      codepoint = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      codepoint = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      codepoint = first & 0x07;
      minimum = 0x10000;
    } else {
      return PFS_INVALID;
    }
    if (continuation > length - i) {
      return PFS_INVALID;
    }
    while (continuation--) {
      uint8_t byte = bytes[i++];
      if ((byte & 0xc0) != 0x80) {
        return PFS_INVALID;
      }
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      return PFS_INVALID;
    }
  }
  return PFS_OK;
}

static int
hex_digit(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static enum pfs_status
id_parse(const char *text, size_t length, uint8_t *out)
{
  uint8_t bytes[PFS_ID_SIZE];
  if (!text || !out || length != PFS_ID_TEXT_SIZE) {
    return PFS_INVALID;
  }
  for (size_t i = 0; i < PFS_ID_SIZE; ++i) {
    int high = hex_digit(text[2 * i]);
    int low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) {
      return PFS_INVALID;
    }
    bytes[i] = (uint8_t)((high << 4) | low);
  }
  if (pfs_bytes_are_zero(bytes, sizeof(bytes))) {
    return PFS_INVALID;
  }
  pfs_bytes_copy(out, bytes, sizeof(bytes));
  return PFS_OK;
}

static void
id_format(const uint8_t *id, char *out)
{
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < PFS_ID_SIZE; ++i) {
    out[2 * i] = digits[id[i] >> 4];
    out[2 * i + 1] = digits[id[i] & 15];
  }
  out[PFS_ID_TEXT_SIZE] = '\0';
}

enum pfs_status
pfs_pool_id_parse(const char *text, size_t length, struct pfs_pool_id *out)
{
  return id_parse(text, length, out ? out->bytes : nullptr);
}

enum pfs_status
pfs_volume_id_parse(const char *text, size_t length, struct pfs_volume_id *out)
{
  return id_parse(text, length, out ? out->bytes : nullptr);
}

enum pfs_status
pfs_object_id_parse(const char *text, size_t length, struct pfs_object_id *out)
{
  return id_parse(text, length, out ? out->bytes : nullptr);
}

enum pfs_status
pfs_principal_id_parse(const char *text, size_t length, struct pfs_principal_id *out)
{
  return id_parse(text, length, out ? out->bytes : nullptr);
}

void
pfs_pool_id_format(const struct pfs_pool_id *id, char out[PFS_ID_TEXT_SIZE + 1])
{
  id_format(id->bytes, out);
}

void
pfs_volume_id_format(const struct pfs_volume_id *id, char out[PFS_ID_TEXT_SIZE + 1])
{
  id_format(id->bytes, out);
}

void
pfs_object_id_format(const struct pfs_object_id *id, char out[PFS_ID_TEXT_SIZE + 1])
{
  id_format(id->bytes, out);
}

void
pfs_principal_id_format(const struct pfs_principal_id *id, char out[PFS_ID_TEXT_SIZE + 1])
{
  id_format(id->bytes, out);
}

enum pfs_status
pfs_reference_validate(const struct pfs_reference *reference,
                       const struct pfs_record_context *context,
                       uint16_t expected_type, bool nullable)
{
  if (!reference || !context) {
    return PFS_INVALID;
  }
  if (!reference->block && !reference->birth && !reference->type && !reference->version) {
    return nullable ? PFS_OK : PFS_CORRUPT;
  }
  if (!pfs_allocatable_range(reference->block, 1, context->block_count) ||
      !reference->birth || reference->birth > context->containing_birth ||
      context->containing_birth > context->selected_generation) {
    return PFS_CORRUPT;
  }
  if (reference->type > PFS_BLOCK_TREE) {
    return PFS_UNSUPPORTED;
  }
  if (reference->type != expected_type) {
    return PFS_CORRUPT;
  }
  return reference->version == PFS_FORMAT_VERSION ? PFS_OK : PFS_UNSUPPORTED;
}

enum pfs_status
pfs_reference_decode(const uint8_t *data, const struct pfs_record_context *context,
                     uint16_t expected_type, bool nullable, struct pfs_reference *out)
{
  struct pfs_reference value;
  pfs_bytes_zero(&value, sizeof(value));
  value.block = pfs_get_u64(data);
  value.birth = pfs_get_u64(data + 8);
  value.type = pfs_get_u16(data + 16);
  value.version = pfs_get_u16(data + 18);
  enum pfs_status status = pfs_reference_validate(&value, context, expected_type, nullable);
  if (status != PFS_OK) {
    return status;
  }
  if (!value.block && !pfs_bytes_are_zero(data, PFS_REFERENCE_SIZE)) {
    return PFS_CORRUPT;
  }
  *out = value;
  return PFS_OK;
}

void
pfs_reference_encode(uint8_t *data, const struct pfs_reference *reference)
{
  pfs_bytes_zero(data, PFS_REFERENCE_SIZE);
  pfs_put_u64(data, reference->block);
  pfs_put_u64(data + 8, reference->birth);
  pfs_put_u16(data + 16, reference->type);
  pfs_put_u16(data + 18, reference->version);
}

enum pfs_status
pfs_record_header_decode(const uint8_t *data, size_t slot_length,
                         uint16_t type, size_t minimum_length,
                         const struct pfs_record_context *context)
{
  if (!data) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (slot_length < PFS_RECORD_HEADER_SIZE || slot_length > PFS_BLOCK_SIZE ||
      slot_length % 8 || pfs_get_u32(data + 4) != slot_length ||
      pfs_get_u32(data + RECORD_CHECKSUM_OFFSET) !=
        pfs_crc32c_zeroed(data, slot_length, RECORD_CHECKSUM_OFFSET, CHECKSUM_SIZE)) {
    return PFS_CORRUPT;
  }
  if (pfs_get_u16(data + 2) != PFS_FORMAT_VERSION) {
    return PFS_UNSUPPORTED;
  }
  uint16_t actual_type = pfs_get_u16(data);
  if (actual_type != type) {
    bool known = (actual_type >= PFS_RECORD_VOLUME && actual_type <= PFS_RECORD_ORPHAN) ||
                 actual_type == PFS_INTERNAL_RECORD_TYPE;
    return known ? PFS_CORRUPT : PFS_UNSUPPORTED;
  }
  return slot_length < minimum_length ? PFS_CORRUPT : PFS_OK;
}

enum pfs_status
pfs_record_length_validate(size_t length, size_t base_length,
                           const struct pfs_features *features)
{
  if (length < base_length || (length != base_length &&
      !features->write_required && !features->optional)) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

void
pfs_record_header_encode(uint8_t *data, uint16_t type, size_t length)
{
  pfs_put_u16(data, type);
  pfs_put_u16(data + 2, PFS_FORMAT_VERSION);
  pfs_put_u32(data + 4, (uint32_t)length);
  pfs_put_u32(data + RECORD_CHECKSUM_OFFSET,
              pfs_crc32c_zeroed(data, length, RECORD_CHECKSUM_OFFSET, CHECKSUM_SIZE));
}
