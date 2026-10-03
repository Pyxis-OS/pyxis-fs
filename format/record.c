/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

static bool
pointers_zero(const uint64_t pointers[NPFS_POINTER_COUNT])
{
  for (unsigned i = 0; i < NPFS_POINTER_COUNT; ++i) {
    if (pointers[i] != 0) {
      return false;
    }
  }
  return true;
}

static enum npfs_status
volume_validate(const struct npfs_header *header, const struct npfs_volume *volume)
{
  if (header == NULL || volume == NULL) {
    return NPFS_INVALID;
  }
  if (volume->state == NPFS_VOLUME_UNUSED) {
    return npfs_bytes_zero(volume->id, NPFS_ID_SIZE) && volume->flags == 0 &&
           volume->mapping == 0 && volume->name_length == 0 && volume->root_inode == 0 &&
           volume->cleanup_head == 0 && volume->inode_bytes == 0 &&
           pointers_zero(volume->pointers) ? NPFS_OK : NPFS_CORRUPT;
  }
  if (volume->state != NPFS_VOLUME_LIVE || volume->mapping != NPFS_MAPPING_POINTERS) {
    return NPFS_UNSUPPORTED;
  }
  if (!npfs_flags_valid(header, volume->flags, 0) ||
      !npfs_id_valid(volume->id) || !npfs_name_valid(volume->name, volume->name_length) ||
      volume->root_inode != 1 || volume->inode_bytes < 2 * NPFS_INODE_SIZE ||
      volume->inode_bytes > NPFS_FILE_SIZE_MAX || volume->inode_bytes % NPFS_INODE_SIZE != 0 ||
      volume->cleanup_head >= volume->inode_bytes / NPFS_INODE_SIZE ||
      !npfs_pointers_valid(header, volume->pointers)) {
    return NPFS_CORRUPT;
  }
  return NPFS_OK;
}

static enum npfs_status
inode_validate(const struct npfs_header *header, const struct npfs_inode *inode)
{
  if (header == NULL || inode == NULL) {
    return NPFS_INVALID;
  }
  if (inode->kind == NPFS_INODE_FREE) {
    return inode->mapping == 0 && inode->flags == 0 && inode->size == 0 &&
           inode->cleanup_next == 0 && inode->shrink_target == 0 && inode->cleanup == 0 &&
           inode->created_ns == 0 && inode->modified_ns == 0 && inode->parent == 0 &&
           pointers_zero(inode->pointers) ? NPFS_OK : NPFS_CORRUPT;
  }
  if ((inode->kind != NPFS_INODE_FILE && inode->kind != NPFS_INODE_DIRECTORY) ||
      inode->mapping != NPFS_MAPPING_POINTERS) {
    return NPFS_UNSUPPORTED;
  }
  if (!npfs_flags_valid(header, inode->flags, NPFS_INODE_FLAGS) ||
      inode->size > NPFS_FILE_SIZE_MAX ||
      (inode->cleanup & ~NPFS_CLEANUP_FLAGS) != 0 ||
      (inode->cleanup == 0 && inode->cleanup_next != 0) ||
      ((inode->cleanup & NPFS_CLEANUP_SHRINK) != 0 && inode->shrink_target != inode->size) ||
      ((inode->cleanup & NPFS_CLEANUP_SHRINK) == 0 && inode->shrink_target != 0) ||
      ((inode->flags & NPFS_TIME_CREATED_VALID) == 0 && inode->created_ns != 0) ||
      ((inode->flags & NPFS_TIME_MODIFIED_VALID) == 0 && inode->modified_ns != 0) ||
      (inode->kind == NPFS_INODE_FILE && inode->parent != 0) ||
      (inode->kind == NPFS_INODE_DIRECTORY &&
       (inode->size % NPFS_BLOCK_SIZE != 0 ||
        ((inode->cleanup & NPFS_CLEANUP_DETACHED) != 0 ? inode->parent != 0 : inode->parent == 0))) ||
      !npfs_pointers_valid(header, inode->pointers)) {
    return NPFS_CORRUPT;
  }
  return NPFS_OK;
}

enum npfs_status
npfs_volume_encode(const struct npfs_header *header, const struct npfs_volume *volume,
                    void *record)
{
  if (record == NULL || volume_validate(header, volume) != NPFS_OK) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_VOLUME_SIZE];
  npfs_memory_zero(bytes, sizeof(bytes));
  npfs_memory_copy(bytes, volume->id, NPFS_ID_SIZE);
  npfs_put_u64(bytes + 16, volume->root_inode);
  npfs_put_u64(bytes + 24, volume->cleanup_head);
  npfs_put_u64(bytes + 32, volume->inode_bytes);
  npfs_put_u32(bytes + 40, volume->state);
  npfs_put_u32(bytes + 44, volume->flags);
  npfs_put_u32(bytes + 48, volume->mapping);
  npfs_put_u16(bytes + 52, volume->name_length);
  npfs_memory_copy(bytes + 56, volume->name, volume->name_length);
  for (unsigned i = 0; i < NPFS_POINTER_COUNT; ++i) {
    npfs_put_u64(bytes + 312 + i * 8, volume->pointers[i]);
  }
  npfs_memory_copy(record, bytes, sizeof(bytes));
  return NPFS_OK;
}

enum npfs_status
npfs_volume_decode(const struct npfs_header *header, const void *record,
                    struct npfs_volume *volume)
{
  if (header == NULL || record == NULL || volume == NULL) {
    return NPFS_INVALID;
  }
  const uint8_t *bytes = record;
  struct npfs_volume result;
  npfs_memory_zero(&result, sizeof(result));
  npfs_memory_copy(result.id, bytes, NPFS_ID_SIZE);
  result.root_inode = npfs_get_u64(bytes + 16);
  result.cleanup_head = npfs_get_u64(bytes + 24);
  result.inode_bytes = npfs_get_u64(bytes + 32);
  result.state = npfs_get_u32(bytes + 40);
  result.flags = npfs_get_u32(bytes + 44);
  result.mapping = npfs_get_u32(bytes + 48);
  result.name_length = npfs_get_u16(bytes + 52);
  if (result.name_length > NPFS_NAME_MAX) {
    return NPFS_CORRUPT;
  }
  npfs_memory_copy(result.name, bytes + 56, result.name_length);
  for (unsigned i = 0; i < NPFS_POINTER_COUNT; ++i) {
    result.pointers[i] = npfs_get_u64(bytes + 312 + i * 8);
  }
  enum npfs_status status = volume_validate(header, &result);
  if (status != NPFS_OK) {
    return status;
  }
  npfs_memory_copy(volume, &result, sizeof(result));
  return NPFS_OK;
}

enum npfs_status
npfs_inode_encode(const struct npfs_header *header, const struct npfs_inode *inode,
                   void *record)
{
  if (record == NULL || inode_validate(header, inode) != NPFS_OK) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_INODE_SIZE];
  npfs_memory_zero(bytes, sizeof(bytes));
  npfs_put_u16(bytes, inode->kind);
  npfs_put_u16(bytes + 2, inode->mapping);
  npfs_put_u32(bytes + 4, inode->flags);
  npfs_put_u64(bytes + 8, inode->size);
  npfs_put_u64(bytes + 16, inode->cleanup_next);
  npfs_put_u64(bytes + 24, inode->shrink_target);
  npfs_put_u32(bytes + 32, inode->cleanup);
  npfs_put_u64(bytes + 36, (uint64_t)inode->created_ns);
  npfs_put_u64(bytes + 44, (uint64_t)inode->modified_ns);
  npfs_put_u64(bytes + 52, inode->parent);
  for (unsigned i = 0; i < NPFS_POINTER_COUNT; ++i) {
    npfs_put_u64(bytes + NPFS_INODE_FIELD_SIZE + i * 8, inode->pointers[i]);
  }
  npfs_memory_copy(record, bytes, sizeof(bytes));
  return NPFS_OK;
}

enum npfs_status
npfs_inode_decode(const struct npfs_header *header, const void *record,
                   struct npfs_inode *inode)
{
  if (header == NULL || record == NULL || inode == NULL) {
    return NPFS_INVALID;
  }
  const uint8_t *bytes = record;
  struct npfs_inode result;
  npfs_memory_zero(&result, sizeof(result));
  result.kind = npfs_get_u16(bytes);
  result.mapping = npfs_get_u16(bytes + 2);
  result.flags = npfs_get_u32(bytes + 4);
  result.size = npfs_get_u64(bytes + 8);
  result.cleanup_next = npfs_get_u64(bytes + 16);
  result.shrink_target = npfs_get_u64(bytes + 24);
  result.cleanup = npfs_get_u32(bytes + 32);
  result.created_ns = (int64_t)npfs_get_u64(bytes + 36);
  result.modified_ns = (int64_t)npfs_get_u64(bytes + 44);
  result.parent = npfs_get_u64(bytes + 52);
  for (unsigned i = 0; i < NPFS_POINTER_COUNT; ++i) {
    result.pointers[i] = npfs_get_u64(bytes + NPFS_INODE_FIELD_SIZE + i * 8);
  }
  enum npfs_status status = inode_validate(header, &result);
  if (status != NPFS_OK) {
    return status;
  }
  npfs_memory_copy(inode, &result, sizeof(result));
  return NPFS_OK;
}

static bool
entry_valid(const struct npfs_header *header, const struct npfs_dirent *entry)
{
  if (header == NULL || entry == NULL || !npfs_flags_valid(header, entry->flags, 0) ||
      entry->record_length < NPFS_DIRENT_HEADER_SIZE ||
      entry->record_length > NPFS_BLOCK_SIZE || entry->record_length % 8 != 0 ||
      entry->name_length > entry->record_length - NPFS_DIRENT_HEADER_SIZE) {
    return false;
  }
  return entry->inode == 0 ? entry->name_length == 0 :
                            npfs_name_valid(entry->name, entry->name_length);
}

enum npfs_status
npfs_dirent_encode(const struct npfs_header *header, const struct npfs_dirent *entry,
                  void *record)
{
  if (record == NULL || !entry_valid(header, entry)) {
    return NPFS_INVALID;
  }
  uint8_t bytes[NPFS_BLOCK_SIZE];
  npfs_memory_zero(bytes, entry->record_length);
  npfs_put_u64(bytes, entry->inode);
  npfs_put_u16(bytes + 8, entry->record_length);
  npfs_put_u16(bytes + 10, entry->name_length);
  npfs_put_u32(bytes + 12, entry->flags);
  npfs_memory_copy(bytes + NPFS_DIRENT_HEADER_SIZE, entry->name, entry->name_length);
  npfs_memory_copy(record, bytes, entry->record_length);
  return NPFS_OK;
}

enum npfs_status
npfs_dirent_decode(const struct npfs_header *header, const void *record, size_t available,
                  struct npfs_dirent *entry)
{
  if (header == NULL || record == NULL || entry == NULL) {
    return NPFS_INVALID;
  }
  if (available < NPFS_DIRENT_HEADER_SIZE) {
    return NPFS_CORRUPT;
  }
  const uint8_t *bytes = record;
  struct npfs_dirent result;
  npfs_memory_zero(&result, sizeof(result));
  result.inode = npfs_get_u64(bytes);
  result.record_length = npfs_get_u16(bytes + 8);
  result.name_length = npfs_get_u16(bytes + 10);
  result.flags = npfs_get_u32(bytes + 12);
  if (result.record_length > available || result.name_length > NPFS_NAME_MAX ||
      result.record_length < NPFS_DIRENT_HEADER_SIZE ||
      result.name_length > result.record_length - NPFS_DIRENT_HEADER_SIZE) {
    return NPFS_CORRUPT;
  }
  npfs_memory_copy(result.name, bytes + NPFS_DIRENT_HEADER_SIZE, result.name_length);
  if (!entry_valid(header, &result)) {
    return NPFS_CORRUPT;
  }
  npfs_memory_copy(entry, &result, sizeof(result));
  return NPFS_OK;
}
