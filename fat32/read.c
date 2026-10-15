/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

enum fat32_status fat32_list(const struct fat32_volume *volume, const char *path,
    size_t path_bytes, struct fat32_workspace *workspace, fat32_entry_sink_fn sink, void *output)
{
  if (!sink) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry directory;
  uint32_t parent;
  status = fat32_resolve(context, path, path_bytes, &directory, &parent);
  if (status != FAT32_OK) {
    return status;
  }
  if (!(directory.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, directory.first_cluster);
  for (;;) {
    struct fat32_entry entry;
    bool found;
    status = fat32_next_entry(context, &cursor, parent, &entry, &found);
    if (status != FAT32_OK) {
      return status;
    }
    if (!found) {
      break;
    }
    if (fat32_ordinary_entry(&entry)) {
      uint64_t clusters;
      status = fat32_claim_chain(context, entry.first_cluster, entry.file_bytes,
          (entry.attributes & FAT32_ATTR_DIRECTORY) != 0, &clusters);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  fat32_cursor_start(&cursor, directory.first_cluster);
  for (;;) {
    struct fat32_entry entry;
    bool found;
    status = fat32_next_entry(context, &cursor, parent, &entry, &found);
    if (status != FAT32_OK || !found) {
      return status;
    }
    if (fat32_ordinary_entry(&entry) && !sink(output, &entry)) {
      return FAT32_OUTPUT;
    }
  }
}

enum fat32_status fat32_read(const struct fat32_volume *volume, const char *path,
    size_t path_bytes, struct fat32_workspace *workspace, fat32_bytes_sink_fn sink, void *output)
{
  if (!sink) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry file;
  uint32_t parent;
  status = fat32_resolve(context, path, path_bytes, &file, &parent);
  if (status != FAT32_OK) {
    return status;
  }
  if (file.attributes & FAT32_ATTR_DIRECTORY) {
    return FAT32_IS_DIRECTORY;
  }
  uint64_t remaining = file.file_bytes;
  uint32_t cluster = file.first_cluster;
  while (remaining) {
    if (!fat32_valid_cluster(volume, cluster)) {
      return FAT32_CORRUPT;
    }
    uint32_t sector = 0;
    while (remaining && sector < volume->sectors_per_cluster) {
      uint32_t sectors = volume->sectors_per_cluster - sector;
      if (sectors > FAT32_IO_BYTES / volume->source.sector_bytes) {
        sectors = FAT32_IO_BYTES / volume->source.sector_bytes;
      }
      uint64_t needed = (remaining + volume->source.sector_bytes - 1) / volume->source.sector_bytes;
      if (sectors > needed) {
        sectors = (uint32_t)needed;
      }
      status = fat32_partition_read(context, volume->data_first +
          (uint64_t)(cluster - 2) * volume->sectors_per_cluster + sector, sectors, context->data);
      context->data_cached = false;
      if (status != FAT32_OK) {
        return status;
      }
      size_t bytes = (size_t)sectors * volume->source.sector_bytes;
      if (bytes > remaining) {
        bytes = (size_t)remaining;
      }
      if (!sink(output, context->data, bytes)) {
        return FAT32_OUTPUT;
      }
      remaining -= bytes;
      sector += sectors;
    }
    if (remaining) {
      status = fat32_fat_entry(context, cluster, &cluster);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  return FAT32_OK;
}

static enum fat32_status validate_entry_chain(struct fat32_context *context,
    const struct fat32_entry *entry, uint32_t parent)
{
  bool directory = (entry->attributes & FAT32_ATTR_DIRECTORY) != 0;
  uint64_t clusters;
  enum fat32_status status = fat32_claim_chain(context, entry->first_cluster,
      entry->file_bytes, directory, &clusters);
  if (status != FAT32_OK || !directory) {
    return status;
  }
  return fat32_validate_directory(context, entry->first_cluster, parent);
}

static enum fat32_status locate_entry(struct fat32_context *context,
    const struct fat32_locator *locator, struct fat32_entry *entry, bool validate_chain)
{
  if (!locator) {
    return FAT32_INVALID;
  }
  if (!context->frame_limit) {
    return FAT32_LIMIT;
  }
  const struct fat32_volume *volume = context->volume;
  if (!locator->parent_cluster) {
    if (locator->parent_parent_cluster || locator->short_entry_slot != UINT64_MAX) {
      return FAT32_INVALID;
    }
    fat32_zero(entry, sizeof(*entry));
    entry->attributes = FAT32_ATTR_DIRECTORY;
    entry->first_cluster = volume->root_cluster;
    return validate_chain ? validate_entry_chain(context, entry, 0) : FAT32_OK;
  }
  if (!fat32_valid_cluster(volume, locator->parent_cluster) ||
      (locator->parent_cluster == volume->root_cluster ? locator->parent_parent_cluster != 0 :
       !fat32_valid_cluster(volume, locator->parent_parent_cluster)) ||
      locator->short_entry_slot == UINT64_MAX) {
    return FAT32_INVALID;
  }
  uint64_t clusters;
  enum fat32_status status = fat32_claim_chain(context, locator->parent_cluster, 0, true,
      &clusters);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_validate_directory(context, locator->parent_cluster,
      locator->parent_parent_cluster);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, locator->parent_cluster);
  for (;;) {
    bool found;
    status = fat32_next_entry(context, &cursor, locator->parent_parent_cluster, entry, &found);
    if (status != FAT32_OK) {
      return status;
    }
    if (!found || cursor.position - 1 > locator->short_entry_slot) {
      return FAT32_STALE;
    }
    if (cursor.position - 1 == locator->short_entry_slot) {
      if (!fat32_ordinary_entry(entry)) {
        return FAT32_STALE;
      }
      return validate_chain ? validate_entry_chain(context, entry, locator->parent_cluster) : FAT32_OK;
    }
  }
}

enum fat32_status fat32_locate_entry(struct fat32_context *context,
    const struct fat32_locator *locator, struct fat32_entry *entry)
{
  return locate_entry(context, locator, entry, true);
}

enum fat32_status fat32_locate_entry_metadata(struct fat32_context *context,
    const struct fat32_locator *locator, struct fat32_entry *entry)
{
  return locate_entry(context, locator, entry, false);
}

enum fat32_status fat32_root(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_entry *entry, struct fat32_locator *locator)
{
  if (!entry || !locator) {
    return FAT32_INVALID;
  }
  struct fat32_locator root = { .short_entry_slot = UINT64_MAX };
  enum fat32_status status = fat32_locate(volume, &root, workspace, entry);
  if (status == FAT32_OK) {
    *locator = root;
  }
  return status;
}

enum fat32_status fat32_locate(const struct fat32_volume *volume,
    const struct fat32_locator *locator, struct fat32_workspace *workspace, struct fat32_entry *entry)
{
  if (!locator || !entry) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry result;
  status = fat32_locate_entry(context, locator, &result);
  if (status == FAT32_OK) {
    fat32_copy(entry, &result, sizeof(*entry));
  }
  return status;
}

static struct fat32_locator child_locator(const struct fat32_locator *directory,
    const struct fat32_entry *entry, const struct fat32_cursor *cursor)
{
  return (struct fat32_locator) {
    .parent_cluster = entry->first_cluster,
    .parent_parent_cluster = directory->parent_cluster,
    .short_entry_slot = cursor->position - 1,
  };
}

static enum fat32_status validate_component(struct fat32_context *context,
    const char *component, size_t bytes)
{
  if (!component || !bytes || (bytes == 1 && component[0] == '.') ||
      (bytes == 2 && component[0] == '.' && component[1] == '.')) {
    return FAT32_INVALID;
  }
  if (bytes > FAT32_NAME_MAX) {
    return FAT32_NAME_TOO_LONG;
  }
  for (size_t i = 0; i < bytes; ++i) {
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK) {
      return status;
    }
    if ((uint8_t)component[i] >= 128) {
      return FAT32_UNSUPPORTED;
    }
    if ((uint8_t)component[i] < 32 || component[i] == '/') {
      return FAT32_INVALID;
    }
  }
  return FAT32_OK;
}

enum fat32_status fat32_lookup(const struct fat32_volume *volume,
    const struct fat32_locator *directory, const char *component, size_t component_bytes,
    struct fat32_workspace *workspace, struct fat32_entry *entry, struct fat32_locator *child)
{
  if (!directory || !entry || !child) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  status = validate_component(context, component, component_bytes);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry parent;
  status = fat32_locate_entry(context, directory, &parent);
  if (status != FAT32_OK) {
    return status;
  }
  if (!(parent.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, parent.first_cluster);
  for (;;) {
    struct fat32_entry candidate;
    bool found;
    status = fat32_next_entry(context, &cursor, directory->parent_cluster, &candidate, &found);
    if (status != FAT32_OK || !found) {
      return status != FAT32_OK ? status : FAT32_NOT_FOUND;
    }
    if (!fat32_ordinary_entry(&candidate) ||
        !((candidate.name_ascii && fat32_ascii_equal(candidate.name, candidate.name_bytes,
            component, component_bytes)) ||
          (candidate.alias_ascii && fat32_ascii_equal(candidate.alias, candidate.alias_bytes,
            component, component_bytes)))) {
      continue;
    }
    status = validate_entry_chain(context, &candidate, parent.first_cluster);
    if (status != FAT32_OK) {
      return status;
    }
    fat32_copy(entry, &candidate, sizeof(*entry));
    *child = child_locator(directory, &parent, &cursor);
    return FAT32_OK;
  }
}

enum fat32_status fat32_enumerate(const struct fat32_volume *volume,
    const struct fat32_locator *directory, uint64_t continuation, struct fat32_workspace *workspace,
    struct fat32_entry *entry, struct fat32_locator *child, uint64_t *next, bool *found)
{
  if (!directory || !entry || !child || !next || !found) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry parent;
  status = fat32_locate_entry(context, directory, &parent);
  if (status != FAT32_OK) {
    return status;
  }
  if (!(parent.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, parent.first_cluster);
  for (;;) {
    struct fat32_entry candidate;
    bool have_entry;
    status = fat32_next_entry(context, &cursor, directory->parent_cluster, &candidate, &have_entry);
    if (status != FAT32_OK) {
      return status;
    }
    if (!have_entry) {
      break;
    }
    if (fat32_ordinary_entry(&candidate)) {
      uint64_t clusters;
      status = fat32_claim_chain(context, candidate.first_cluster, candidate.file_bytes,
          (candidate.attributes & FAT32_ATTR_DIRECTORY) != 0, &clusters);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  fat32_cursor_start(&cursor, parent.first_cluster);
  if (continuation != UINT64_MAX) {
    for (;;) {
      struct fat32_entry candidate;
      bool have_entry;
      status = fat32_next_entry(context, &cursor, directory->parent_cluster, &candidate, &have_entry);
      if (status != FAT32_OK) {
        return status;
      }
      if (!have_entry) {
        break;
      }
      if (fat32_ordinary_entry(&candidate) && cursor.position - 1 >= continuation) {
        fat32_copy(entry, &candidate, sizeof(*entry));
        *child = child_locator(directory, &parent, &cursor);
        *next = cursor.position;
        *found = true;
        return FAT32_OK;
      }
    }
  }
  *next = UINT64_MAX;
  *found = false;
  return FAT32_OK;
}

enum fat32_status fat32_read_range(const struct fat32_volume *volume,
    const struct fat32_locator *file, uint64_t offset, size_t length,
    struct fat32_workspace *workspace, fat32_bytes_sink_fn sink, void *output)
{
  if (!file || !sink) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry entry;
  status = fat32_locate_entry(context, file, &entry);
  if (status != FAT32_OK) {
    return status;
  }
  if (entry.attributes & FAT32_ATTR_DIRECTORY) {
    return FAT32_IS_DIRECTORY;
  }
  if (offset >= entry.file_bytes || !length) {
    return FAT32_OK;
  }
  uint64_t remaining = entry.file_bytes - offset;
  if (remaining > length) {
    remaining = length;
  }
  uint64_t cluster_bytes = (uint64_t)volume->sectors_per_cluster * volume->source.sector_bytes;
  uint32_t cluster = entry.first_cluster;
  while (offset >= cluster_bytes) {
    status = fat32_fat_entry(context, cluster, &cluster);
    if (status != FAT32_OK) {
      return status;
    }
    offset -= cluster_bytes;
  }
  uint32_t sector = (uint32_t)(offset / volume->source.sector_bytes);
  size_t skip = (size_t)(offset % volume->source.sector_bytes);
  while (remaining) {
    uint32_t sectors = volume->sectors_per_cluster - sector;
    if (sectors > FAT32_IO_BYTES / volume->source.sector_bytes) {
      sectors = FAT32_IO_BYTES / volume->source.sector_bytes;
    }
    uint64_t needed = (remaining + skip + volume->source.sector_bytes - 1) / volume->source.sector_bytes;
    if (sectors > needed) {
      sectors = (uint32_t)needed;
    }
    status = fat32_partition_read(context, volume->data_first +
        (uint64_t)(cluster - 2) * volume->sectors_per_cluster + sector, sectors, context->data);
    context->data_cached = false;
    if (status != FAT32_OK) {
      return status;
    }
    size_t bytes = (size_t)sectors * volume->source.sector_bytes - skip;
    if (bytes > remaining) {
      bytes = (size_t)remaining;
    }
    if (!sink(output, context->data + skip, bytes)) {
      return FAT32_OUTPUT;
    }
    remaining -= bytes;
    skip = 0;
    sector += sectors;
    if (remaining && sector == volume->sectors_per_cluster) {
      sector = 0;
      status = fat32_fat_entry(context, cluster, &cluster);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  return FAT32_OK;
}
