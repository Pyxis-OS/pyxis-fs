/* SPDX-License-Identifier: MPL-2.0 */
/* Geometry, confined reads and chain handling are generalized from
 * pyxis-userland installer/esp_read.c at
 * d2437266d45989c1e73e1ceba995600e4d2f9787 (MPL-2.0).
 * The installer's recovery reader remains separate and unchanged. */
#include "internal.h"

#define FAT32_BOOT_SIGNATURE UINT16_C(0xaa55)
#define FAT32_FSINFO_LEAD UINT32_C(0x41615252)
#define FAT32_FSINFO_STRUCTURE UINT32_C(0x61417272)
#define FAT32_FSINFO_TRAIL UINT32_C(0xaa550000)
#define FAT32_CLEAN UINT32_C(0x08000000)
#define FAT32_NO_ERROR UINT32_C(0x04000000)

bool fat32_valid_cluster(const struct fat32_volume *volume, uint32_t cluster)
{
  return cluster >= 2 && cluster - 2 < volume->cluster_count;
}

enum fat32_status fat32_fat_entry(struct fat32_context *context, uint32_t cluster,
    uint32_t *value)
{
  const struct fat32_volume *volume = context->volume;
  enum fat32_status status = fat32_step(context);
  if (status != FAT32_OK) {
    return status;
  }
  if (cluster > (uint64_t)volume->cluster_count + 1) {
    return FAT32_CORRUPT;
  }
  uint64_t offset = (uint64_t)cluster * 4;
  uint64_t sector = offset / FAT32_SECTOR_BYTES;
  if (sector >= volume->fat_sectors) {
    return FAT32_CORRUPT;
  }
  if (!context->fat_cached || sector < context->fat_sector ||
      sector - context->fat_sector >= context->fat_sector_count) {
    context->fat_cached = false;
    context->fat_sector_count = 0;
    uint32_t count = context->fat_window_sectors;
    if (count > context->source->max_read_sectors) {
      count = context->source->max_read_sectors;
    }
    if (count > volume->fat_sectors - sector) {
      count = (uint32_t)(volume->fat_sectors - sector);
    }
    status = fat32_partition_read(context, volume->fat_first + sector, count, context->fat_bytes);
    if (status != FAT32_OK) {
      return status;
    }
    if (volume->fat_count == 2) {
      status = fat32_partition_read(context, volume->fat_first + volume->fat_sectors + sector,
          count, context->mirror_bytes);
      if (status != FAT32_OK) {
        return status;
      }
      uint64_t first = sector * (FAT32_SECTOR_BYTES / 4);
      uint32_t entries = count * (FAT32_SECTOR_BYTES / 4);
      for (uint32_t i = 0; i < entries && first + i <= (uint64_t)volume->cluster_count + 1; ++i) {
        if ((fat32_get32(context->fat_bytes + 4 * i) & FAT32_MASK) !=
            (fat32_get32(context->mirror_bytes + 4 * i) & FAT32_MASK)) {
          struct fat32_mirror_state *mirrors = volume->mirrors;
          if (!mirrors) {
            return FAT32_CORRUPT;
          }
          mirrors->mismatch_seen = true;
          if (mirrors->bound) {
            uint64_t different = (first + i) / (FAT32_SECTOR_BYTES / 4);
            if (different >= mirrors->sector_count) {
              return FAT32_CORRUPT;
            }
            uint8_t mask = (uint8_t)(1u << (different % 8));
            if (!(mirrors->pending[different / 8] & mask)) {
              mirrors->pending[different / 8] |= mask;
              ++mirrors->pending_sectors;
            }
          }
        }
      }
    }
    context->fat_sector = sector;
    context->fat_sector_count = count;
    context->fat_cached = true;
  }
  size_t within = (size_t)(sector - context->fat_sector) * FAT32_SECTOR_BYTES +
      offset % FAT32_SECTOR_BYTES;
  *value = fat32_get32(context->fat_bytes + within) & FAT32_MASK;
  return FAT32_OK;
}

static enum fat32_status fsinfo(struct fat32_context *context, uint32_t sector)
{
  enum fat32_status status = fat32_partition_read(context, sector, 1, context->data);
  if (status != FAT32_OK) {
    return status;
  }
  const uint8_t *bytes = context->data;
  if (fat32_get32(bytes) != FAT32_FSINFO_LEAD ||
      fat32_get32(bytes + 484) != FAT32_FSINFO_STRUCTURE ||
      fat32_get32(bytes + 508) != FAT32_FSINFO_TRAIL) {
    return FAT32_CORRUPT;
  }
  /* Both hints are advisory, including out-of-range values. Writers rebuild
   * from the FAT and publish corrected hints with a confirmed mutation. */
  return FAT32_OK;
}

enum fat32_status fat32_geometry(struct fat32_context *context, struct fat32_volume *volume)
{
  if (!volume->partition_sectors || volume->partition_first >= volume->source.sectors ||
      volume->partition_sectors > volume->source.sectors - volume->partition_first) {
    return FAT32_CORRUPT;
  }
  context->volume = volume;
  enum fat32_status status = fat32_partition_read(context, 0, 1, context->data);
  if (status != FAT32_OK) {
    return status;
  }
  uint8_t boot[FAT32_SECTOR_BYTES];
  fat32_copy(boot, context->data, sizeof(boot));
  uint32_t reserved = fat32_get16(boot + 14);
  uint32_t fat_sectors = fat32_get32(boot + 36), total = fat32_get32(boot + 32);
  uint32_t info_sector = fat32_get16(boot + 48), backup = fat32_get16(boot + 50);
  uint8_t cluster_sectors = boot[13], fat_count = boot[16];
  if (fat32_get16(boot + 510) != FAT32_BOOT_SIGNATURE || !reserved || !fat_sectors ||
      !total || total > volume->partition_sectors || fat32_get16(boot + 17) ||
      fat32_get16(boot + 19) || fat32_get16(boot + 22)) {
    return FAT32_CORRUPT;
  }
  if (fat32_get16(boot + 11) != FAT32_SECTOR_BYTES ||
      !cluster_sectors || cluster_sectors > 128 || (cluster_sectors & (cluster_sectors - 1)) ||
      (fat_count != 1 && fat_count != 2) || fat32_get16(boot + 40) ||
      fat32_get16(boot + 42) || !fat32_all_zero(boot + 52, 12)) {
    return FAT32_UNSUPPORTED;
  }
  if (boot[21] != 0xf0 && boot[21] < 0xf8) {
    return FAT32_CORRUPT;
  }
  uint64_t data_first = (uint64_t)reserved + (uint64_t)fat_count * fat_sectors;
  if (data_first >= total) {
    return FAT32_CORRUPT;
  }
  uint64_t clusters = (total - data_first) / cluster_sectors;
  if (clusters < FAT32_MIN_CLUSTERS) {
    return FAT32_UNSUPPORTED;
  }
  if (clusters > FAT32_RESERVED_MIN - 2 || (clusters + 2) * 4 > (uint64_t)fat_sectors * 512) {
    return FAT32_CORRUPT;
  }
  volume->fat_first = reserved;
  volume->fat_sectors = fat_sectors;
  volume->data_first = data_first;
  volume->volume_sectors = total;
  volume->cluster_count = (uint32_t)clusters;
  volume->root_cluster = fat32_get32(boot + 44);
  volume->sectors_per_cluster = cluster_sectors;
  volume->fat_count = fat_count;
  volume->media = boot[21];
  if (!fat32_valid_cluster(volume, volume->root_cluster)) {
    return FAT32_CORRUPT;
  }
  bool have_info = info_sector != UINT16_MAX;
  bool have_backup = backup != 0 && backup != UINT16_MAX;
  if ((have_info && (!info_sector || info_sector >= reserved)) ||
      (have_backup && (backup >= reserved || (have_info &&
       (backup == info_sector || info_sector + backup >= reserved))))) {
    return FAT32_CORRUPT;
  }
  if (have_backup) {
    status = fat32_partition_read(context, backup, 1, context->data);
    if (status != FAT32_OK) {
      return status;
    }
    if (fat32_get16(context->data + 510) != FAT32_BOOT_SIGNATURE ||
        !fat32_equal(boot + 11, context->data + 11, 53)) {
      return FAT32_CORRUPT;
    }
  }
  if (have_info) {
    status = fsinfo(context, info_sector);
    if (status != FAT32_OK) {
      return status;
    }
    if (have_backup) {
      status = fsinfo(context, backup + info_sector);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  uint32_t media, flags;
  context->fat_cached = false;
  status = fat32_fat_entry(context, 0, &media);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_fat_entry(context, 1, &flags);
  if (status != FAT32_OK) {
    return status;
  }
  if (media != (UINT32_C(0x0fffff00) | volume->media) ||
      (flags & UINT32_C(0x03ffffff)) != UINT32_C(0x03ffffff)) {
    return FAT32_CORRUPT;
  }
  volume->clean_shutdown = (flags & FAT32_CLEAN) != 0;
  volume->no_hard_error = (flags & FAT32_NO_ERROR) != 0;
  context->data_cached = false;
  return FAT32_OK;
}

enum fat32_status fat32_revalidate(struct fat32_context *context)
{
  const struct fat32_volume *original = context->volume;
  struct fat32_volume current;
  fat32_copy(&current, original, sizeof(current));
  enum fat32_status status = fat32_geometry(context, &current);
  context->volume = original;
  if (status != FAT32_OK) {
    return status;
  }
  if (current.fat_first != original->fat_first || current.data_first != original->data_first ||
      current.volume_sectors != original->volume_sectors ||
      current.fat_sectors != original->fat_sectors || current.cluster_count != original->cluster_count ||
      current.root_cluster != original->root_cluster ||
      current.sectors_per_cluster != original->sectors_per_cluster ||
      current.fat_count != original->fat_count || current.media != original->media ||
      current.clean_shutdown != original->clean_shutdown || current.no_hard_error != original->no_hard_error) {
    return FAT32_CORRUPT;
  }
  return FAT32_OK;
}

static enum fat32_status open_view(const struct fat32_source *source, uint32_t gpt_entry,
    struct fat32_mirror_state *mirrors, struct fat32_workspace *workspace,
    struct fat32_volume *volume)
{
  if (!volume) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = fat32_context_open(source, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  if (workspace->bytes < fat32_open_workspace_size()) {
    return FAT32_NO_MEMORY;
  }
  struct fat32_volume result;
  fat32_zero(&result, sizeof(result));
  result.mirrors = mirrors;
  fat32_copy(&result.source, source, sizeof(*source));
  status = fat32_gpt_select(context, gpt_entry,
      (uint8_t *)workspace->memory + fat32_context_size(), &result);
  if (status == FAT32_OK) {
    status = fat32_geometry(context, &result);
  }
  if (status == FAT32_OK) {
    fat32_copy(volume, &result, sizeof(result));
  }
  return status;
}

enum fat32_status fat32_open(const struct fat32_source *source, uint32_t gpt_entry,
    struct fat32_workspace *workspace, struct fat32_volume *volume)
{
  return open_view(source, gpt_entry, NULL, workspace, volume);
}

enum fat32_status fat32_open_mounted_view(const struct fat32_source *source, uint32_t gpt_entry,
    struct fat32_mirror_state *mirrors, struct fat32_workspace *workspace,
    struct fat32_volume *volume)
{
  if (!mirrors || mirrors->pending || mirrors->pending_bytes || mirrors->sector_count ||
      mirrors->pending_sectors || mirrors->mismatch_seen || mirrors->bound) {
    return FAT32_INVALID;
  }
  return open_view(source, gpt_entry, mirrors, workspace, volume);
}

enum fat32_status fat32_mirror_size(const struct fat32_volume *volume, size_t *bytes)
{
  if (!volume || !volume->mirrors || !bytes ||
      volume->cluster_count < FAT32_MIN_CLUSTERS || volume->cluster_count > FAT32_RESERVED_MIN - 2 ||
      (volume->fat_count != 1 && volume->fat_count != 2)) {
    return FAT32_INVALID;
  }
  uint64_t sectors = ((uint64_t)volume->cluster_count + 2 + 127) / 128;
  if (sectors > volume->fat_sectors) {
    return FAT32_CORRUPT;
  }
  *bytes = volume->fat_count == 2 ? (size_t)((sectors + 7) / 8) : 0;
  return FAT32_OK;
}

enum fat32_status fat32_mirror_bind(const struct fat32_volume *volume,
    uint8_t *pending, size_t bytes)
{
  size_t required;
  enum fat32_status status = fat32_mirror_size(volume, &required);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_mirror_state *mirrors = volume->mirrors;
  if (mirrors->bound || mirrors->pending || mirrors->pending_bytes ||
      mirrors->sector_count || mirrors->pending_sectors) {
    return FAT32_INVALID;
  }
  if (bytes < required || (required && !pending)) {
    return FAT32_NO_MEMORY;
  }
  for (size_t i = 0; i < required; ++i) {
    if (pending[i]) {
      return FAT32_INVALID;
    }
  }
  mirrors->pending = required ? pending : NULL;
  mirrors->pending_bytes = required;
  mirrors->sector_count = volume->fat_count == 2 ?
      (uint32_t)(((uint64_t)volume->cluster_count + 2 + 127) / 128) : 0;
  mirrors->bound = true;
  return FAT32_OK;
}

uint32_t fat32_mirror_pending(const struct fat32_volume *volume)
{
  return volume && volume->mirrors ? volume->mirrors->pending_sectors : 0;
}

static enum fat32_status claim_chain(struct fat32_context *context, uint32_t first,
    uint32_t file_bytes, bool directory, uint64_t *clusters,
    const uint32_t *chain, uint32_t chain_count, bool compare)
{
  *clusters = 0;
  if (!directory && !file_bytes) {
    if (first) {
      return FAT32_CORRUPT;
    }
    return compare && chain_count ? FAT32_STALE : FAT32_OK;
  }
  uint64_t expected = ((uint64_t)file_bytes +
      (uint64_t)context->volume->sectors_per_cluster * FAT32_SECTOR_BYTES - 1) /
      ((uint64_t)context->volume->sectors_per_cluster * FAT32_SECTOR_BYTES);
  bool matches = true;
  uint32_t cluster = first;
  uint32_t checkpoint = first;
  uint64_t power = 1, distance = 0;
  for (;;) {
    if (!fat32_valid_cluster(context->volume, cluster) ||
        *clusters >= context->volume->cluster_count) {
      return FAT32_CORRUPT;
    }
    if (context->allocation && (context->allocation[cluster / 8] & (1u << (cluster % 8)))) {
      return FAT32_CORRUPT;
    }
    if (context->seen) {
      if (context->seen[cluster / 8] & (1u << (cluster % 8))) {
        return FAT32_CORRUPT;
      }
      context->seen[cluster / 8] |= 1u << (cluster % 8);
    }
    if (compare && (*clusters >= chain_count || !chain || chain[*clusters] != cluster)) {
      matches = false;
    }
    ++*clusters;
    uint32_t next;
    enum fat32_status status = fat32_fat_entry(context, cluster, &next);
    if (status != FAT32_OK) {
      return status;
    }
    if (next >= FAT32_EOC) {
      if (!directory && *clusters != expected) {
        return FAT32_CORRUPT;
      }
      return compare && (!matches || *clusters != chain_count) ? FAT32_STALE : FAT32_OK;
    }
    if (!directory && *clusters >= expected) {
      return FAT32_CORRUPT;
    }
    if (!context->seen) {
      /* Brent's checkpoints detect a repeated link with constant storage.
       * The cluster-count ceiling also bounds traversal and power doubling. */
      if (next == checkpoint) {
        return FAT32_CORRUPT;
      }
      if (++distance == power) {
        checkpoint = next;
        power *= 2;
        distance = 0;
      }
    }
    cluster = next;
  }
}

enum fat32_status fat32_claim_chain(struct fat32_context *context, uint32_t first,
    uint32_t file_bytes, bool directory, uint64_t *clusters)
{
  return claim_chain(context, first, file_bytes, directory, clusters, NULL, 0, false);
}

enum fat32_status fat32_match_file_chain(struct fat32_context *context, uint32_t first,
    uint32_t file_bytes, const uint32_t *chain, uint32_t chain_count)
{
  uint64_t clusters;
  return claim_chain(context, first, file_bytes, false, &clusters, chain, chain_count, true);
}
