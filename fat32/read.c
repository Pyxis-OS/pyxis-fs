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
      if (sectors > FAT32_IO_BYTES / FAT32_SECTOR_BYTES) {
        sectors = FAT32_IO_BYTES / FAT32_SECTOR_BYTES;
      }
      uint64_t needed = (remaining + FAT32_SECTOR_BYTES - 1) / FAT32_SECTOR_BYTES;
      if (sectors > needed) {
        sectors = (uint32_t)needed;
      }
      status = fat32_partition_read(context, volume->data_first +
          (uint64_t)(cluster - 2) * volume->sectors_per_cluster + sector, sectors, context->data);
      context->data_cached = false;
      if (status != FAT32_OK) {
        return status;
      }
      size_t bytes = (size_t)sectors * FAT32_SECTOR_BYTES;
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
