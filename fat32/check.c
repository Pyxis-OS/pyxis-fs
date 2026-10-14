/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

enum fat32_status fat32_check(const struct fat32_volume *volume,
    struct fat32_workspace *workspace, struct fat32_check_report *report)
{
  if (!report) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_volume(volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  return fat32_check_context(context, report);
}

enum fat32_status fat32_check_context(struct fat32_context *context,
    struct fat32_check_report *report)
{
  return fat32_check_context_extra(context, report, NULL, NULL);
}

enum fat32_status fat32_check_context_extra(struct fat32_context *context,
    struct fat32_check_report *report, fat32_check_extra_fn extra, void *extra_context)
{
  const struct fat32_volume *volume = context->volume;
  enum fat32_status status;
  if (!context->frame_limit) {
    return FAT32_LIMIT;
  }
  struct fat32_check_report result;
  fat32_zero(&result, sizeof(result));
  result.all_names_ascii = true;
  status = fat32_claim_chain(context, volume->root_cluster, 0, true, &result.allocated_clusters);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_validate_directory(context, volume->root_cluster, 0);
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t depth = 1;
  struct fat32_frame *root = &context->frames[0];
  root->first = volume->root_cluster;
  root->parent = 0;
  fat32_cursor_start(&root->cursor, root->first);
  result.directories = 1;
  while (depth) {
    struct fat32_frame *frame = &context->frames[depth - 1];
    struct fat32_entry entry;
    bool found;
    status = fat32_next_entry(context, &frame->cursor, frame->parent, &entry, &found);
    if (status != FAT32_OK) {
      return status;
    }
    if (!found) {
      --depth;
      continue;
    }
    if (!fat32_ordinary_entry(&entry)) {
      continue;
    }
    if (!entry.name_ascii || !entry.alias_ascii) {
      result.all_names_ascii = false;
    }
    bool directory = (entry.attributes & FAT32_ATTR_DIRECTORY) != 0;
    uint64_t clusters;
    status = fat32_claim_chain(context, entry.first_cluster, entry.file_bytes, directory, &clusters);
    if (status != FAT32_OK) {
      return status;
    }
    result.allocated_clusters += clusters;
    if (directory) {
      if (depth == context->frame_limit) {
        return FAT32_LIMIT;
      }
      status = fat32_validate_directory(context, entry.first_cluster, frame->first);
      if (status != FAT32_OK) {
        return status;
      }
      struct fat32_frame *child = &context->frames[depth++];
      child->first = entry.first_cluster;
      child->parent = frame->first;
      fat32_cursor_start(&child->cursor, child->first);
      ++result.directories;
    } else {
      ++result.files;
    }
  }
  if (extra) {
    status = extra(context, extra_context, &result);
    if (status != FAT32_OK) {
      return status;
    }
  }
  for (uint32_t cluster = 2; cluster - 2 < volume->cluster_count; ++cluster) {
    uint32_t value;
    status = fat32_fat_entry(context, cluster, &value);
    if (status != FAT32_OK) {
      return status;
    }
    bool reached = (context->seen[cluster / 8] & (1u << (cluster % 8))) != 0;
    if (!value) {
      if (reached) {
        return FAT32_CORRUPT;
      }
      ++result.free_clusters;
    } else if (value == FAT32_BAD) {
      if (reached) {
        return FAT32_CORRUPT;
      }
      ++result.bad_clusters;
    } else if (!reached || (value < FAT32_EOC && !fat32_valid_cluster(volume, value))) {
      return FAT32_CORRUPT;
    }
    if (context->free_map) {
      /* Reached bits are no longer needed after this cluster's validation,
       * so mounted CHECK can rebuild in the same scratch bitmap. */
      uint8_t mask = 1u << (cluster % 8);
      if (!value) {
        context->free_map[cluster / 8] |= mask;
      } else {
        context->free_map[cluster / 8] &= (uint8_t)~mask;
      }
    }
  }
  result.mirror_mismatch_sectors = fat32_mirror_pending(volume);
  fat32_copy(report, &result, sizeof(result));
  return FAT32_OK;
}
