/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

static bool
pointers_zero(const uint64_t pointers[PNF_POINTER_COUNT])
{
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    if (pointers[i] != 0) {
      return false;
    }
  }
  return true;
}

static enum pnf_status
volume_validate(const struct pnf_header *header, const struct pnf_volume *volume)
{
  if (header == NULL || volume == NULL) {
    return PNF_INVALID;
  }
  if (volume->state == PNF_VOLUME_UNUSED) {
    return pnf_bytes_zero(volume->id, PNF_ID_SIZE) && volume->flags == 0 &&
           volume->mapping == 0 && volume->name_length == 0 && volume->root_inode == 0 &&
           volume->cleanup_head == 0 && volume->inode_bytes == 0 &&
           pointers_zero(volume->pointers) ? PNF_OK : PNF_CORRUPT;
  }
  if (volume->state != PNF_VOLUME_LIVE || volume->mapping != PNF_MAPPING_POINTERS) {
    return PNF_UNSUPPORTED;
  }
  if (!pnf_id_valid(volume->id) || !pnf_name_valid(volume->name, volume->name_length) ||
      volume->root_inode != 1 || volume->inode_bytes < 2 * PNF_INODE_SIZE ||
      volume->inode_bytes > PNF_FILE_SIZE_MAX || volume->inode_bytes % PNF_INODE_SIZE != 0 ||
      volume->cleanup_head >= volume->inode_bytes / PNF_INODE_SIZE ||
      !pnf_pointers_valid(header, volume->pointers)) {
    return PNF_CORRUPT;
  }
  return PNF_OK;
}

static enum pnf_status
inode_validate(const struct pnf_header *header, const struct pnf_inode *inode)
{
  if (header == NULL || inode == NULL) {
    return PNF_INVALID;
  }
  if (inode->kind == PNF_INODE_FREE) {
    return inode->mapping == 0 && inode->flags == 0 && inode->size == 0 &&
           inode->cleanup_next == 0 && inode->shrink_target == 0 && inode->cleanup == 0 &&
           inode->created_ns == 0 && inode->modified_ns == 0 && inode->parent == 0 &&
           pointers_zero(inode->pointers) ? PNF_OK : PNF_CORRUPT;
  }
  if ((inode->kind != PNF_INODE_FILE && inode->kind != PNF_INODE_DIRECTORY) ||
      inode->mapping != PNF_MAPPING_POINTERS) {
    return PNF_UNSUPPORTED;
  }
  if (inode->size > PNF_FILE_SIZE_MAX ||
      (inode->cleanup & ~PNF_CLEANUP_FLAGS) != 0 ||
      (inode->cleanup == 0 && inode->cleanup_next != 0) ||
      ((inode->cleanup & PNF_CLEANUP_SHRINK) != 0 && inode->shrink_target != inode->size) ||
      ((inode->cleanup & PNF_CLEANUP_SHRINK) == 0 && inode->shrink_target != 0) ||
      ((inode->flags & PNF_TIME_CREATED_VALID) == 0 && inode->created_ns != 0) ||
      ((inode->flags & PNF_TIME_MODIFIED_VALID) == 0 && inode->modified_ns != 0) ||
      (inode->kind == PNF_INODE_FILE && inode->parent != 0) ||
      (inode->kind == PNF_INODE_DIRECTORY &&
       (inode->size % PNF_BLOCK_SIZE != 0 ||
        ((inode->cleanup & PNF_CLEANUP_DETACHED) != 0 ? inode->parent != 0 : inode->parent == 0))) ||
      !pnf_pointers_valid(header, inode->pointers)) {
    return PNF_CORRUPT;
  }
  return PNF_OK;
}

enum pnf_status
pnf_volume_encode(const struct pnf_header *header, const struct pnf_volume *volume,
                    void *record)
{
  if (record == NULL || volume_validate(header, volume) != PNF_OK) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_VOLUME_SIZE];
  pnf_memory_zero(bytes, sizeof(bytes));
  pnf_memory_copy(bytes, volume->id, PNF_ID_SIZE);
  pnf_put_u64(bytes + 16, volume->root_inode);
  pnf_put_u64(bytes + 24, volume->cleanup_head);
  pnf_put_u64(bytes + 32, volume->inode_bytes);
  pnf_put_u32(bytes + 40, volume->state);
  pnf_put_u32(bytes + 44, volume->flags);
  pnf_put_u32(bytes + 48, volume->mapping);
  pnf_put_u16(bytes + 52, volume->name_length);
  pnf_memory_copy(bytes + 56, volume->name, volume->name_length);
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    pnf_put_u64(bytes + 312 + i * 8, volume->pointers[i]);
  }
  pnf_memory_copy(record, bytes, sizeof(bytes));
  return PNF_OK;
}

enum pnf_status
pnf_volume_decode(const struct pnf_header *header, const void *record,
                    struct pnf_volume *volume)
{
  if (header == NULL || record == NULL || volume == NULL) {
    return PNF_INVALID;
  }
  const uint8_t *bytes = record;
  struct pnf_volume result;
  pnf_memory_zero(&result, sizeof(result));
  pnf_memory_copy(result.id, bytes, PNF_ID_SIZE);
  result.root_inode = pnf_get_u64(bytes + 16);
  result.cleanup_head = pnf_get_u64(bytes + 24);
  result.inode_bytes = pnf_get_u64(bytes + 32);
  result.state = pnf_get_u32(bytes + 40);
  result.flags = pnf_get_u32(bytes + 44);
  result.mapping = pnf_get_u32(bytes + 48);
  result.name_length = pnf_get_u16(bytes + 52);
  if (result.name_length > PNF_NAME_MAX) {
    return PNF_CORRUPT;
  }
  pnf_memory_copy(result.name, bytes + 56, result.name_length);
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    result.pointers[i] = pnf_get_u64(bytes + 312 + i * 8);
  }
  enum pnf_status status = volume_validate(header, &result);
  if (status != PNF_OK) {
    return status;
  }
  pnf_memory_copy(volume, &result, sizeof(result));
  return PNF_OK;
}

enum pnf_status
pnf_inode_encode(const struct pnf_header *header, const struct pnf_inode *inode,
                   void *record)
{
  if (record == NULL || inode_validate(header, inode) != PNF_OK) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_INODE_SIZE];
  pnf_memory_zero(bytes, sizeof(bytes));
  pnf_put_u16(bytes, inode->kind);
  pnf_put_u16(bytes + 2, inode->mapping);
  pnf_put_u32(bytes + 4, inode->flags);
  pnf_put_u64(bytes + 8, inode->size);
  pnf_put_u64(bytes + 16, inode->cleanup_next);
  pnf_put_u64(bytes + 24, inode->shrink_target);
  pnf_put_u32(bytes + 32, inode->cleanup);
  pnf_put_u64(bytes + 36, (uint64_t)inode->created_ns);
  pnf_put_u64(bytes + 44, (uint64_t)inode->modified_ns);
  pnf_put_u64(bytes + 52, inode->parent);
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    pnf_put_u64(bytes + PNF_INODE_FIELD_SIZE + i * 8, inode->pointers[i]);
  }
  pnf_memory_copy(record, bytes, sizeof(bytes));
  return PNF_OK;
}

enum pnf_status
pnf_inode_decode(const struct pnf_header *header, const void *record,
                   struct pnf_inode *inode)
{
  if (header == NULL || record == NULL || inode == NULL) {
    return PNF_INVALID;
  }
  const uint8_t *bytes = record;
  struct pnf_inode result;
  pnf_memory_zero(&result, sizeof(result));
  result.kind = pnf_get_u16(bytes);
  result.mapping = pnf_get_u16(bytes + 2);
  result.flags = pnf_get_u32(bytes + 4);
  result.size = pnf_get_u64(bytes + 8);
  result.cleanup_next = pnf_get_u64(bytes + 16);
  result.shrink_target = pnf_get_u64(bytes + 24);
  result.cleanup = pnf_get_u32(bytes + 32);
  result.created_ns = (int64_t)pnf_get_u64(bytes + 36);
  result.modified_ns = (int64_t)pnf_get_u64(bytes + 44);
  result.parent = pnf_get_u64(bytes + 52);
  for (unsigned i = 0; i < PNF_POINTER_COUNT; ++i) {
    result.pointers[i] = pnf_get_u64(bytes + PNF_INODE_FIELD_SIZE + i * 8);
  }
  enum pnf_status status = inode_validate(header, &result);
  if (status != PNF_OK) {
    return status;
  }
  pnf_memory_copy(inode, &result, sizeof(result));
  return PNF_OK;
}

static bool
entry_valid(const struct pnf_dirent *entry)
{
  if (entry == NULL || entry->record_length < PNF_DIRENT_HEADER_SIZE ||
      entry->record_length > PNF_BLOCK_SIZE || entry->record_length % 8 != 0 ||
      entry->name_length > entry->record_length - PNF_DIRENT_HEADER_SIZE) {
    return false;
  }
  return entry->inode == 0 ? entry->name_length == 0 :
                            pnf_name_valid(entry->name, entry->name_length);
}

enum pnf_status
pnf_dirent_encode(const struct pnf_dirent *entry, void *record)
{
  if (record == NULL || !entry_valid(entry)) {
    return PNF_INVALID;
  }
  uint8_t bytes[PNF_BLOCK_SIZE];
  pnf_memory_zero(bytes, entry->record_length);
  pnf_put_u64(bytes, entry->inode);
  pnf_put_u16(bytes + 8, entry->record_length);
  pnf_put_u16(bytes + 10, entry->name_length);
  pnf_put_u32(bytes + 12, entry->flags);
  pnf_memory_copy(bytes + PNF_DIRENT_HEADER_SIZE, entry->name, entry->name_length);
  pnf_memory_copy(record, bytes, entry->record_length);
  return PNF_OK;
}

enum pnf_status
pnf_dirent_decode(const void *record, size_t available, struct pnf_dirent *entry)
{
  if (record == NULL || entry == NULL) {
    return PNF_INVALID;
  }
  if (available < PNF_DIRENT_HEADER_SIZE) {
    return PNF_CORRUPT;
  }
  const uint8_t *bytes = record;
  struct pnf_dirent result;
  pnf_memory_zero(&result, sizeof(result));
  result.inode = pnf_get_u64(bytes);
  result.record_length = pnf_get_u16(bytes + 8);
  result.name_length = pnf_get_u16(bytes + 10);
  result.flags = pnf_get_u32(bytes + 12);
  if (result.record_length > available || result.name_length > PNF_NAME_MAX ||
      result.record_length < PNF_DIRENT_HEADER_SIZE ||
      result.name_length > result.record_length - PNF_DIRENT_HEADER_SIZE) {
    return PNF_CORRUPT;
  }
  pnf_memory_copy(result.name, bytes + PNF_DIRENT_HEADER_SIZE, result.name_length);
  if (!entry_valid(&result)) {
    return PNF_CORRUPT;
  }
  pnf_memory_copy(entry, &result, sizeof(result));
  return PNF_OK;
}
