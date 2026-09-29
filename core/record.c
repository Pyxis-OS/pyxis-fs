#include <pyxis_fs/record.h>

#include "internal.h"

#define FILE_BLOCKS_MAX (PFS_FILE_SIZE_MAX / PFS_BLOCK_SIZE)

static enum pfs_status
name_validate(const struct pfs_name *name)
{
  if (name->length > PFS_NAME_MAX ||
      pfs_name_validate(name->bytes, name->length) != PFS_OK) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

static size_t
name_record_length(uint16_t length)
{
  return (PFS_NAME_RECORD_PREFIX_SIZE + (size_t)length + 7u) & ~(size_t)7u;
}

static enum pfs_status
name_decode(const uint8_t *bytes, uint16_t length, struct pfs_name *out)
{
  if (length > PFS_NAME_MAX || pfs_name_validate(bytes, length) != PFS_OK) {
    return PFS_CORRUPT;
  }
  out->length = length;
  pfs_bytes_copy(out->bytes, bytes, length);
  return PFS_OK;
}

static enum pfs_status
encode_prepare(void *data, size_t capacity, size_t length,
               const struct pfs_record_context *context, size_t *written,
               enum pfs_status validation)
{
  if (!data || !written || !context || capacity < length) {
    return PFS_INVALID;
  }
  if (validation != PFS_OK) {
    return validation == PFS_CORRUPT ? PFS_INVALID : validation;
  }
  return pfs_features_write(&context->features);
}

/* Catalog envelopes retain target versions without interpreting volume trees. */
static enum pfs_status
volume_reference_validate(const struct pfs_reference *reference,
                           const struct pfs_record_context *context, bool nullable)
{
  if (reference->block && !reference->version) {
    return PFS_CORRUPT;
  }
  struct pfs_reference envelope = *reference;
  if (envelope.version) {
    envelope.version = PFS_FORMAT_VERSION;
  }
  return pfs_reference_validate(&envelope, context, PFS_BLOCK_TREE, nullable);
}

static enum pfs_status
volume_reference_decode(const uint8_t *bytes, const struct pfs_record_context *context,
                         bool nullable, struct pfs_reference *out)
{
  struct pfs_reference value;
  pfs_bytes_zero(&value, sizeof(value));
  value.block = pfs_get_u64(bytes);
  value.birth = pfs_get_u64(bytes + 8);
  value.type = pfs_get_u16(bytes + 16);
  value.version = pfs_get_u16(bytes + 18);
  enum pfs_status status = volume_reference_validate(&value, context, nullable);
  if (status != PFS_OK) {
    return status;
  }
  if (!value.block && !pfs_bytes_are_zero(bytes, PFS_REFERENCE_SIZE)) {
    return PFS_CORRUPT;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_volume_record_validate(const struct pfs_volume_record *record,
                           const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  uint64_t usable = context->block_count - 2;
  if (pfs_bytes_are_zero(record->id.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(record->root_object.bytes, PFS_ID_SIZE) ||
      record->guarantee > record->quota || record->guarantee > usable ||
      record->live_blocks == 0 || record->live_blocks > usable ||
      record->retired_blocks > usable - record->live_blocks ||
      record->object_count == 0) {
    return PFS_CORRUPT;
  }
  if (record->object_count > PFS_RECORD_COUNT_MAX) {
    return PFS_LIMIT;
  }
  status = name_validate(&record->name);
  if (status != PFS_OK) {
    return status;
  }
  status = volume_reference_validate(&record->object_root, context, false);
  if (status != PFS_OK) {
    return status;
  }
  return volume_reference_validate(&record->grant_root, context, true);
}

enum pfs_status
pfs_volume_record_decode(const void *data, size_t slot_length,
                         const struct pfs_record_context *context,
                         struct pfs_volume_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_VOLUME, PFS_VOLUME_RECORD_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_volume_record value;
  pfs_bytes_zero(&value, sizeof(value));
  pfs_bytes_copy(value.id.bytes, bytes + 16, PFS_ID_SIZE);
  status = name_decode(bytes + 40, pfs_get_u16(bytes + 32), &value.name);
  if (status != PFS_OK) {
    return status;
  }
  value.features.read_required = pfs_get_u64(bytes + 296);
  value.features.write_required = pfs_get_u64(bytes + 304);
  value.features.optional = pfs_get_u64(bytes + 312);
  pfs_bytes_copy(value.root_object.bytes, bytes + 320, PFS_ID_SIZE);
  status = volume_reference_decode(bytes + 336, context, false, &value.object_root);
  if (status != PFS_OK) {
    return status;
  }
  status = volume_reference_decode(bytes + 360, context, true, &value.grant_root);
  if (status != PFS_OK) {
    return status;
  }
  value.guarantee = pfs_get_u64(bytes + 384);
  value.quota = pfs_get_u64(bytes + 392);
  value.live_blocks = pfs_get_u64(bytes + 400);
  value.retired_blocks = pfs_get_u64(bytes + 408);
  value.object_count = pfs_get_u64(bytes + 416);
  status = pfs_record_length_validate(slot_length, PFS_VOLUME_RECORD_SIZE, &context->features);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_volume_record_encode(void *data, size_t capacity,
                         const struct pfs_record_context *context,
                         const struct pfs_volume_record *record, size_t *written)
{
  enum pfs_status status = encode_prepare(data, capacity, PFS_VOLUME_RECORD_SIZE, context,
    written, pfs_volume_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_features_write(&record->features);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_reference_validate(&record->object_root, context, PFS_BLOCK_TREE, false);
  if (status == PFS_OK) {
    status = pfs_reference_validate(&record->grant_root, context, PFS_BLOCK_TREE, true);
  }
  if (status != PFS_OK) {
    return status == PFS_CORRUPT ? PFS_INVALID : status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, PFS_VOLUME_RECORD_SIZE);
  pfs_bytes_copy(bytes + 16, record->id.bytes, PFS_ID_SIZE);
  pfs_put_u16(bytes + 32, record->name.length);
  pfs_bytes_copy(bytes + 40, record->name.bytes, record->name.length);
  pfs_bytes_copy(bytes + 320, record->root_object.bytes, PFS_ID_SIZE);
  pfs_reference_encode(bytes + 336, &record->object_root);
  pfs_reference_encode(bytes + 360, &record->grant_root);
  pfs_put_u64(bytes + 384, record->guarantee);
  pfs_put_u64(bytes + 392, record->quota);
  pfs_put_u64(bytes + 400, record->live_blocks);
  pfs_put_u64(bytes + 408, record->retired_blocks);
  pfs_put_u64(bytes + 416, record->object_count);
  pfs_record_header_encode(bytes, PFS_RECORD_VOLUME, PFS_VOLUME_RECORD_SIZE);
  *written = PFS_VOLUME_RECORD_SIZE;
  return PFS_OK;
}

enum pfs_status
pfs_volume_name_record_validate(const struct pfs_volume_name_record *record,
                                const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_bytes_are_zero(record->volume.bytes, PFS_ID_SIZE)) {
    return PFS_CORRUPT;
  }
  return name_validate(&record->name);
}

enum pfs_status
pfs_volume_name_record_decode(const void *data, size_t slot_length,
                              const struct pfs_record_context *context,
                              struct pfs_volume_name_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_VOLUME_NAME, PFS_NAME_RECORD_PREFIX_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  uint16_t length = pfs_get_u16(bytes + 16);
  if (length > PFS_NAME_MAX || slot_length < name_record_length(length)) {
    return PFS_CORRUPT;
  }
  status = pfs_record_length_validate(slot_length, name_record_length(length),
                                      &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_volume_name_record value;
  pfs_bytes_zero(&value, sizeof(value));
  pfs_bytes_copy(value.volume.bytes, bytes + 24, PFS_ID_SIZE);
  status = name_decode(bytes + 40, length, &value.name);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_name_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_volume_name_record_encode(void *data, size_t capacity,
                              const struct pfs_record_context *context,
                              const struct pfs_volume_name_record *record,
                              size_t *written)
{
  if (!record) {
    return PFS_INVALID;
  }
  size_t length = name_record_length(record->name.length);
  enum pfs_status status = encode_prepare(data, capacity, length, context, written,
    pfs_volume_name_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, length);
  pfs_put_u16(bytes + 16, record->name.length);
  pfs_bytes_copy(bytes + 24, record->volume.bytes, PFS_ID_SIZE);
  pfs_bytes_copy(bytes + 40, record->name.bytes, record->name.length);
  pfs_record_header_encode(bytes, PFS_RECORD_VOLUME_NAME, length);
  *written = length;
  return PFS_OK;
}

enum pfs_status
pfs_allocation_record_validate(const struct pfs_allocation_record *record,
                               const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (!pfs_allocatable_range(record->first, record->count, context->block_count) ||
      record->state > PFS_ALLOCATION_RETIRED || record->charge > PFS_CHARGE_RECOVERY) {
    return PFS_CORRUPT;
  }
  bool owner_zero = pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE);
  if (record->state == PFS_ALLOCATION_FREE) {
    if (!owner_zero || record->birth || record->retirement || record->charge) {
      return PFS_CORRUPT;
    }
    return PFS_OK;
  }
  if (record->birth == 0 || record->birth > context->containing_birth) {
    return PFS_CORRUPT;
  }
  if (record->state == PFS_ALLOCATION_RETIRED) {
    if (record->charge == PFS_CHARGE_PERMANENT || record->retirement <= record->birth ||
        record->retirement > context->containing_birth) {
      return PFS_CORRUPT;
    }
    return PFS_OK;
  }
  if (record->retirement != 0 ||
      (record->state == PFS_ALLOCATION_POOL && !owner_zero) ||
      (record->state == PFS_ALLOCATION_VOLUME && owner_zero)) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_allocation_record_decode(const void *data, size_t slot_length,
                             const struct pfs_record_context *context,
                             struct pfs_allocation_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_ALLOCATION, PFS_ALLOCATION_RECORD_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(slot_length, PFS_ALLOCATION_RECORD_SIZE, &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_allocation_record value;
  pfs_bytes_zero(&value, sizeof(value));
  value.first = pfs_get_u64(bytes + 16);
  value.count = pfs_get_u64(bytes + 24);
  value.state = bytes[32];
  value.charge = bytes[33];
  pfs_bytes_copy(value.owner.bytes, bytes + 40, PFS_ID_SIZE);
  value.birth = pfs_get_u64(bytes + 56);
  value.retirement = pfs_get_u64(bytes + 64);
  status = pfs_allocation_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_allocation_record_encode(void *data, size_t capacity,
                             const struct pfs_record_context *context,
                             const struct pfs_allocation_record *record,
                             size_t *written)
{
  enum pfs_status status = encode_prepare(data, capacity, PFS_ALLOCATION_RECORD_SIZE, context,
    written, pfs_allocation_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, PFS_ALLOCATION_RECORD_SIZE);
  pfs_put_u64(bytes + 16, record->first);
  pfs_put_u64(bytes + 24, record->count);
  bytes[32] = record->state;
  bytes[33] = record->charge;
  pfs_bytes_copy(bytes + 40, record->owner.bytes, PFS_ID_SIZE);
  pfs_put_u64(bytes + 56, record->birth);
  pfs_put_u64(bytes + 64, record->retirement);
  pfs_record_header_encode(bytes, PFS_RECORD_ALLOCATION, PFS_ALLOCATION_RECORD_SIZE);
  *written = PFS_ALLOCATION_RECORD_SIZE;
  return PFS_OK;
}

enum pfs_status
pfs_extent_mapping_validate(const struct pfs_extent_mapping *mapping,
                            const struct pfs_record_context *context)
{
  if (!mapping) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (mapping->count == 0 || mapping->birth == 0 ||
      mapping->birth > context->containing_birth ||
      mapping->count > UINT64_MAX - mapping->logical_first ||
      !pfs_allocatable_range(mapping->physical_first, mapping->count,
                             context->block_count)) {
    return PFS_CORRUPT;
  }
  if (!pfs_range_valid(mapping->logical_first, mapping->count, FILE_BLOCKS_MAX)) {
    return PFS_LIMIT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_extent_file_validate(const struct pfs_extent_mapping *mapping, uint64_t file_length)
{
  if (!mapping) {
    return PFS_INVALID;
  }
  if (file_length > PFS_FILE_SIZE_MAX) {
    return PFS_LIMIT;
  }
  uint64_t blocks = file_length / PFS_BLOCK_SIZE;
  if (file_length % PFS_BLOCK_SIZE) {
    blocks++;
  }
  if (mapping->count == 0 ||
      !pfs_range_valid(mapping->logical_first, mapping->count, blocks)) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_object_record_validate(const struct pfs_object_record *record,
                           const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_bytes_are_zero(record->id.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE) ||
      (record->kind != PFS_OBJECT_FILE && record->kind != PFS_OBJECT_DIRECTORY) ||
      (record->kind == PFS_OBJECT_FILE &&
       pfs_bytes_are_zero(record->parent.bytes, PFS_ID_SIZE)) ||
      (record->kind == PFS_OBJECT_DIRECTORY && record->file_length != 0)) {
    return PFS_CORRUPT;
  }
  if (record->file_length > PFS_FILE_SIZE_MAX) {
    return PFS_LIMIT;
  }
  switch (record->storage_kind) {
  case PFS_STORAGE_NONE:
    return PFS_OK;
  case PFS_STORAGE_INLINE:
    if (record->kind != PFS_OBJECT_FILE) {
      return PFS_CORRUPT;
    }
    status = pfs_extent_mapping_validate(&record->inline_extent, context);
    if (status != PFS_OK) {
      return status;
    }
    return pfs_extent_file_validate(&record->inline_extent, record->file_length);
  case PFS_STORAGE_TREE:
    if ((record->kind == PFS_OBJECT_FILE &&
         (record->file_length == 0 || record->directory_count != 0)) ||
        (record->kind == PFS_OBJECT_DIRECTORY && record->directory_count == 0)) {
      return PFS_CORRUPT;
    }
    if (record->directory_count > PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    return pfs_reference_validate(&record->tree_root, context, PFS_BLOCK_TREE, false);
  default:
    return PFS_UNSUPPORTED;
  }
}

static void
mapping_decode(const uint8_t *bytes, struct pfs_extent_mapping *mapping)
{
  mapping->logical_first = pfs_get_u64(bytes);
  mapping->count = pfs_get_u64(bytes + 8);
  mapping->physical_first = pfs_get_u64(bytes + 16);
  mapping->birth = pfs_get_u64(bytes + 24);
}

static void
mapping_encode(uint8_t *bytes, const struct pfs_extent_mapping *mapping)
{
  pfs_put_u64(bytes, mapping->logical_first);
  pfs_put_u64(bytes + 8, mapping->count);
  pfs_put_u64(bytes + 16, mapping->physical_first);
  pfs_put_u64(bytes + 24, mapping->birth);
}

enum pfs_status
pfs_object_record_decode(const void *data, size_t slot_length,
                         const struct pfs_record_context *context,
                         struct pfs_object_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_OBJECT, PFS_OBJECT_RECORD_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(slot_length, PFS_OBJECT_RECORD_SIZE, &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_object_record value;
  pfs_bytes_zero(&value, sizeof(value));
  pfs_bytes_copy(value.id.bytes, bytes + 16, PFS_ID_SIZE);
  value.kind = pfs_get_u16(bytes + 32);
  value.storage_kind = pfs_get_u16(bytes + 34);
  pfs_bytes_copy(value.owner.bytes, bytes + 40, PFS_ID_SIZE);
  pfs_bytes_copy(value.parent.bytes, bytes + 56, PFS_ID_SIZE);
  value.file_length = pfs_get_u64(bytes + 72);
  switch (value.storage_kind) {
  case PFS_STORAGE_NONE:
    if (!pfs_bytes_are_zero(bytes + 80, 32)) {
      return PFS_CORRUPT;
    }
    break;
  case PFS_STORAGE_INLINE:
    mapping_decode(bytes + 80, &value.inline_extent);
    break;
  case PFS_STORAGE_TREE:
    status = pfs_reference_decode(bytes + 80, context, PFS_BLOCK_TREE, false,
                                  &value.tree_root);
    if (status != PFS_OK) {
      return status;
    }
    value.directory_count = pfs_get_u64(bytes + 104);
    break;
  default:
    return PFS_UNSUPPORTED;
  }
  status = pfs_object_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_object_record_encode(void *data, size_t capacity,
                         const struct pfs_record_context *context,
                         const struct pfs_object_record *record, size_t *written)
{
  enum pfs_status status = encode_prepare(data, capacity, PFS_OBJECT_RECORD_SIZE, context,
    written, pfs_object_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, PFS_OBJECT_RECORD_SIZE);
  pfs_bytes_copy(bytes + 16, record->id.bytes, PFS_ID_SIZE);
  pfs_put_u16(bytes + 32, record->kind);
  pfs_put_u16(bytes + 34, record->storage_kind);
  pfs_bytes_copy(bytes + 40, record->owner.bytes, PFS_ID_SIZE);
  pfs_bytes_copy(bytes + 56, record->parent.bytes, PFS_ID_SIZE);
  pfs_put_u64(bytes + 72, record->file_length);
  if (record->storage_kind == PFS_STORAGE_INLINE) {
    mapping_encode(bytes + 80, &record->inline_extent);
  } else if (record->storage_kind == PFS_STORAGE_TREE) {
    pfs_reference_encode(bytes + 80, &record->tree_root);
    pfs_put_u64(bytes + 104, record->directory_count);
  }
  pfs_record_header_encode(bytes, PFS_RECORD_OBJECT, PFS_OBJECT_RECORD_SIZE);
  *written = PFS_OBJECT_RECORD_SIZE;
  return PFS_OK;
}

enum pfs_status
pfs_dirent_record_validate(const struct pfs_dirent_record *record,
                           const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_bytes_are_zero(record->object.bytes, PFS_ID_SIZE) ||
      (record->child_kind != PFS_OBJECT_FILE && record->child_kind != PFS_OBJECT_DIRECTORY)) {
    return PFS_CORRUPT;
  }
  return name_validate(&record->name);
}

enum pfs_status
pfs_dirent_record_decode(const void *data, size_t slot_length,
                         const struct pfs_record_context *context,
                         struct pfs_dirent_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_DIRENT, PFS_NAME_RECORD_PREFIX_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  uint16_t length = pfs_get_u16(bytes + 16);
  if (length > PFS_NAME_MAX || slot_length < name_record_length(length)) {
    return PFS_CORRUPT;
  }
  status = pfs_record_length_validate(slot_length, name_record_length(length),
                                      &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_dirent_record value;
  pfs_bytes_zero(&value, sizeof(value));
  value.child_kind = pfs_get_u16(bytes + 18);
  pfs_bytes_copy(value.object.bytes, bytes + 24, PFS_ID_SIZE);
  status = name_decode(bytes + 40, length, &value.name);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_dirent_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_dirent_record_encode(void *data, size_t capacity,
                         const struct pfs_record_context *context,
                         const struct pfs_dirent_record *record, size_t *written)
{
  if (!record) {
    return PFS_INVALID;
  }
  size_t length = name_record_length(record->name.length);
  enum pfs_status status = encode_prepare(data, capacity, length, context, written,
    pfs_dirent_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, length);
  pfs_put_u16(bytes + 16, record->name.length);
  pfs_put_u16(bytes + 18, record->child_kind);
  pfs_bytes_copy(bytes + 24, record->object.bytes, PFS_ID_SIZE);
  pfs_bytes_copy(bytes + 40, record->name.bytes, record->name.length);
  pfs_record_header_encode(bytes, PFS_RECORD_DIRENT, length);
  *written = length;
  return PFS_OK;
}

enum pfs_status
pfs_extent_record_validate(const struct pfs_extent_record *record,
                           const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  return pfs_extent_mapping_validate(&record->mapping, context);
}

enum pfs_status
pfs_extent_record_decode(const void *data, size_t slot_length,
                         const struct pfs_record_context *context,
                         struct pfs_extent_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_EXTENT, PFS_EXTENT_RECORD_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(slot_length, PFS_EXTENT_RECORD_SIZE, &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_extent_record value;
  pfs_bytes_zero(&value, sizeof(value));
  mapping_decode(bytes + 16, &value.mapping);
  status = pfs_extent_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_extent_record_encode(void *data, size_t capacity,
                         const struct pfs_record_context *context,
                         const struct pfs_extent_record *record, size_t *written)
{
  enum pfs_status status = encode_prepare(data, capacity, PFS_EXTENT_RECORD_SIZE, context,
    written, pfs_extent_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, PFS_EXTENT_RECORD_SIZE);
  mapping_encode(bytes + 16, &record->mapping);
  pfs_record_header_encode(bytes, PFS_RECORD_EXTENT, PFS_EXTENT_RECORD_SIZE);
  *written = PFS_EXTENT_RECORD_SIZE;
  return PFS_OK;
}

enum pfs_status
pfs_grant_record_validate(const struct pfs_grant_record *record,
                          const struct pfs_record_context *context)
{
  if (!record) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_context_validate(context);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_bytes_are_zero(record->object.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(record->principal.bytes, PFS_ID_SIZE) ||
      record->scope > PFS_SCOPE_SUBTREE) {
    return PFS_CORRUPT;
  }
  if ((record->file_rights & ~PFS_FILE_RIGHTS_ALL) ||
      (record->directory_rights & ~PFS_DIR_RIGHTS_ALL) ||
      (record->admin_rights & ~PFS_ADMIN_RIGHTS_ALL)) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_grant_target_validate(const struct pfs_grant_record *record, uint16_t target_kind)
{
  if (!record) {
    return PFS_INVALID;
  }
  if (target_kind != PFS_OBJECT_FILE && target_kind != PFS_OBJECT_DIRECTORY) {
    return PFS_CORRUPT;
  }
  if (target_kind == PFS_OBJECT_FILE &&
      (record->scope == PFS_SCOPE_SUBTREE || record->directory_rights != 0)) {
    return PFS_CORRUPT;
  }
  if (target_kind == PFS_OBJECT_DIRECTORY && record->scope == PFS_SCOPE_OBJECT &&
      record->file_rights != 0) {
    return PFS_CORRUPT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_grant_record_decode(const void *data, size_t slot_length,
                        const struct pfs_record_context *context,
                        struct pfs_grant_record *out)
{
  if (!data || !out) {
    return PFS_INVALID;
  }
  const uint8_t *bytes = data;
  enum pfs_status status = pfs_record_header_decode(bytes, slot_length,
    PFS_RECORD_GRANT, PFS_GRANT_RECORD_SIZE, context);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_record_length_validate(slot_length, PFS_GRANT_RECORD_SIZE, &context->features);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_grant_record value;
  pfs_bytes_zero(&value, sizeof(value));
  pfs_bytes_copy(value.object.bytes, bytes + 16, PFS_ID_SIZE);
  pfs_bytes_copy(value.principal.bytes, bytes + 32, PFS_ID_SIZE);
  value.scope = bytes[48];
  value.file_rights = pfs_get_u64(bytes + 56);
  value.directory_rights = pfs_get_u64(bytes + 64);
  value.admin_rights = pfs_get_u64(bytes + 72);
  status = pfs_grant_record_validate(&value, context);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_copy(out, &value, sizeof(value));
  return PFS_OK;
}

enum pfs_status
pfs_grant_record_encode(void *data, size_t capacity,
                        const struct pfs_record_context *context,
                        const struct pfs_grant_record *record, size_t *written)
{
  enum pfs_status status = encode_prepare(data, capacity, PFS_GRANT_RECORD_SIZE, context,
    written, pfs_grant_record_validate(record, context));
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *bytes = data;
  pfs_bytes_zero(bytes, PFS_GRANT_RECORD_SIZE);
  pfs_bytes_copy(bytes + 16, record->object.bytes, PFS_ID_SIZE);
  pfs_bytes_copy(bytes + 32, record->principal.bytes, PFS_ID_SIZE);
  bytes[48] = record->scope;
  pfs_put_u64(bytes + 56, record->file_rights);
  pfs_put_u64(bytes + 64, record->directory_rights);
  pfs_put_u64(bytes + 72, record->admin_rights);
  pfs_record_header_encode(bytes, PFS_RECORD_GRANT, PFS_GRANT_RECORD_SIZE);
  *written = PFS_GRANT_RECORD_SIZE;
  return PFS_OK;
}
