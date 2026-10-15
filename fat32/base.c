/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

uint16_t fat32_get16(const uint8_t *bytes)
{
  return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

uint32_t fat32_get32(const uint8_t *bytes)
{
  return (uint32_t)fat32_get16(bytes) | (uint32_t)fat32_get16(bytes + 2) << 16;
}

uint64_t fat32_get64(const uint8_t *bytes)
{
  return (uint64_t)fat32_get32(bytes) | (uint64_t)fat32_get32(bytes + 4) << 32;
}

void fat32_copy(void *destination, const void *source, size_t length)
{
  uint8_t *to = destination;
  const uint8_t *from = source;
  for (size_t i = 0; i < length; ++i) {
    to[i] = from[i];
  }
}

void fat32_zero(void *bytes, size_t length)
{
  uint8_t *destination = bytes;
  for (size_t i = 0; i < length; ++i) {
    destination[i] = 0;
  }
}

bool fat32_equal(const void *left, const void *right, size_t length)
{
  const uint8_t *a = left, *b = right;
  for (size_t i = 0; i < length; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

bool fat32_all_zero(const void *bytes, size_t length)
{
  const uint8_t *source = bytes;
  for (size_t i = 0; i < length; ++i) {
    if (source[i]) {
      return false;
    }
  }
  return true;
}

uint32_t fat32_crc(const uint8_t *bytes, size_t length)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
  }
  return ~crc;
}

static size_t align_size(size_t bytes)
{
  size_t alignment = _Alignof(max_align_t);
  return (bytes + alignment - 1) / alignment * alignment;
}

size_t fat32_context_size(void)
{
  return align_size(sizeof(struct fat32_context));
}

size_t fat32_open_workspace_size(void)
{
  return fat32_context_size() + FAT32_GPT_ARRAY_LIMIT;
}

static uint32_t check_window_sectors(const struct fat32_volume *volume)
{
  return volume->source.max_read_sectors < FAT32_CHECK_FAT_WINDOW_SECTORS ?
      volume->source.max_read_sectors : FAT32_CHECK_FAT_WINDOW_SECTORS;
}

enum fat32_status fat32_workspace_size(const struct fat32_volume *volume,
    uint32_t directory_depth_limit, size_t *bytes)
{
  if (!volume || !bytes || volume->cluster_count < FAT32_MIN_CLUSTERS ||
      volume->cluster_count > FAT32_RESERVED_MIN - 2) {
    return FAT32_INVALID;
  }
  size_t bitmap = ((size_t)volume->cluster_count + 2 + 7) / 8;
  size_t fixed = fat32_context_size() + align_size(bitmap);
  if (directory_depth_limit > (SIZE_MAX - fixed) / sizeof(struct fat32_frame)) {
    return FAT32_LIMIT;
  }
  size_t required = fixed + (size_t)directory_depth_limit * sizeof(struct fat32_frame);
  uint32_t window = check_window_sectors(volume);
  if (window > FAT32_FAT_WINDOW_SECTORS) {
    size_t windows = (size_t)window * FAT32_SECTOR_BYTES * 2;
    if (windows > SIZE_MAX - required) {
      return FAT32_LIMIT;
    }
    required += windows;
  }
  *bytes = required;
  return FAT32_OK;
}

enum fat32_status fat32_context_open(const struct fat32_source *source,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  if (!source || !source->read || !source->max_read_sectors || !workspace ||
      !workspace->memory || (uintptr_t)workspace->memory % _Alignof(max_align_t)) {
    return FAT32_INVALID;
  }
  if (source->sector_bytes != FAT32_SECTOR_BYTES) {
    return FAT32_UNSUPPORTED;
  }
  if (workspace->bytes < fat32_context_size()) {
    return FAT32_NO_MEMORY;
  }
  *context = workspace->memory;
  fat32_zero(*context, sizeof(**context));
  (*context)->source = source;
  (*context)->fat_bytes = (*context)->fat;
  (*context)->mirror_bytes = (*context)->mirror;
  (*context)->fat_window_sectors = FAT32_FAT_WINDOW_SECTORS;
  (*context)->steps = workspace->step_limit;
  (*context)->sectors = workspace->sector_limit;
  (*context)->frame_limit = workspace->directory_depth_limit;
  return FAT32_OK;
}

enum fat32_status fat32_context_volume_prepare(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  if (!volume || (volume->mirrors && !volume->mirrors->bound)) {
    return FAT32_INVALID;
  }
  size_t required;
  enum fat32_status status = fat32_workspace_size(volume,
      workspace ? workspace->directory_depth_limit : 0, &required);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_context_open(&volume->source, workspace, context);
  if (status != FAT32_OK) {
    return status;
  }
  if (workspace->bytes < required) {
    return FAT32_NO_MEMORY;
  }
  (*context)->volume = volume;
  (*context)->seen = (uint8_t *)workspace->memory + fat32_context_size();
  (*context)->seen_bytes = ((size_t)volume->cluster_count + 2 + 7) / 8;
  (*context)->frames = (void *)((*context)->seen + align_size((*context)->seen_bytes));
  uint32_t window = check_window_sectors(volume);
  if (window > FAT32_FAT_WINDOW_SECTORS) {
    uint8_t *buffers = (uint8_t *)((*context)->frames + (*context)->frame_limit);
    (*context)->fat_bytes = buffers;
    (*context)->mirror_bytes = buffers + (size_t)window * FAT32_SECTOR_BYTES;
    (*context)->fat_window_sectors = window;
  }
  fat32_zero((*context)->seen, (*context)->seen_bytes);
  return FAT32_OK;
}

enum fat32_status fat32_context_volume(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  enum fat32_status status = fat32_context_volume_prepare(volume, workspace, context);
  return status == FAT32_OK ? fat32_revalidate(*context) : status;
}

enum fat32_status fat32_context_volume_local_prepare(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  if (!volume || (volume->mirrors && !volume->mirrors->bound) ||
      volume->cluster_count < FAT32_MIN_CLUSTERS ||
      volume->cluster_count > FAT32_RESERVED_MIN - 2) {
    return FAT32_INVALID;
  }
  enum fat32_status status = fat32_context_open(&volume->source, workspace, context);
  if (status != FAT32_OK) {
    return status;
  }
  (*context)->volume = volume;
  return FAT32_OK;
}

enum fat32_status fat32_context_volume_local(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  enum fat32_status status = fat32_context_volume_local_prepare(volume, workspace, context);
  return status == FAT32_OK ? fat32_revalidate(*context) : status;
}

void fat32_context_read_windows(struct fat32_context *context,
    struct fat32_workspace *workspace)
{
  uint32_t window = check_window_sectors(context->volume);
  size_t required;
  if (window <= FAT32_FAT_WINDOW_SECTORS ||
      fat32_workspace_size(context->volume, workspace->directory_depth_limit, &required) != FAT32_OK ||
      workspace->bytes < required) {
    return;
  }
  uint8_t *buffers = (uint8_t *)workspace->memory + required -
      (size_t)window * FAT32_SECTOR_BYTES * 2;
  context->fat_bytes = buffers;
  context->mirror_bytes = buffers + (size_t)window * FAT32_SECTOR_BYTES;
  context->fat_window_sectors = window;
  context->fat_cached = false;
}

enum fat32_status fat32_step(struct fat32_context *context)
{
  if (!context->steps) {
    return FAT32_LIMIT;
  }
  --context->steps;
  return FAT32_OK;
}

enum fat32_status fat32_disk_read(struct fat32_context *context, uint64_t first,
    uint32_t count, void *bytes)
{
  if (!count || first >= context->source->sectors ||
      count > context->source->sectors - first) {
    return FAT32_CORRUPT;
  }
  uint8_t *destination = bytes;
  while (count) {
    uint32_t chunk = count < context->source->max_read_sectors ?
        count : context->source->max_read_sectors;
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK || chunk > context->sectors) {
      return FAT32_LIMIT;
    }
    context->sectors -= chunk;
    if (!context->source->read(context->source->context, first, chunk, destination)) {
      return FAT32_IO;
    }
    destination += (size_t)chunk * FAT32_SECTOR_BYTES;
    first += chunk;
    count -= chunk;
  }
  return FAT32_OK;
}

enum fat32_status fat32_partition_read(struct fat32_context *context, uint64_t first,
    uint32_t count, void *bytes)
{
  const struct fat32_volume *volume = context->volume;
  if (!volume || !count || first >= volume->partition_sectors ||
      count > volume->partition_sectors - first ||
      volume->partition_first > UINT64_MAX - first) {
    return FAT32_CORRUPT;
  }
  return fat32_disk_read(context, volume->partition_first + first, count, bytes);
}
