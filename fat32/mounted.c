/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <pyxis_fs/fat32_mounted.h>

#define ENTRY_DELETED 0xe5u
#define ENTRY_ARCHIVE 0x20u
#define ENTRY_LONG_LAST 0x40u
#define DOS_FIRST_DATE 0x0021u
#define FAT32_CLEAN UINT32_C(0x08000000)
#define FAT32_ATTR_READ_ONLY 0x01u

struct allocation_change {
  uint32_t cluster, value;
  uint8_t phase;
};

struct write_plan {
  struct fat32_mounted *mounted;
  struct fat32_context *context;
  struct fat32_prepared_sector *sectors;
  size_t count, capacity;
  uint32_t *free_clusters, candidate_capacity, allocated, searched, next_candidate;
  struct allocation_change *changes;
  uint32_t change_capacity, change_count;
  struct fat32_check_report check;
  uint8_t entry_count, entry_slots[21], entries[21][32];
  uint64_t entry_sectors[21];
  uint64_t short_slot;
  struct fat32_mounted_object *grown_directory, *target_parent;
  uint32_t *directory_extension, directory_extension_count;
  bool allow_mirror_repair;
};

static void put16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *bytes, uint32_t value)
{
  put16(bytes, (uint16_t)value);
  put16(bytes + 2, (uint16_t)(value >> 16));
}

static size_t aligned(size_t bytes)
{
  size_t alignment = _Alignof(max_align_t);
  return (bytes + alignment - 1) / alignment * alignment;
}

static uint64_t cluster_sector(const struct fat32_volume *volume, uint32_t cluster)
{
  return volume->data_first + (uint64_t)(cluster - 2) * volume->sectors_per_cluster;
}

static uint32_t clusters_for(const struct fat32_volume *volume, uint32_t bytes)
{
  uint32_t size = (uint32_t)volume->sectors_per_cluster * FAT32_SECTOR_BYTES;
  return (uint32_t)(((uint64_t)bytes + size - 1) / size);
}

static enum fat32_status mounted_result(struct fat32_mounted *mounted, enum fat32_status status)
{
  if (mounted && (status == FAT32_IO || status == FAT32_CORRUPT)) {
    mounted->allocation_valid = false;
  }
  return status;
}

static bool cluster_free(const struct fat32_mounted *mounted, uint32_t cluster)
{
  return (mounted->allocation[cluster / 8] & (1u << (cluster % 8))) != 0;
}

static enum fat32_status sector_image(struct write_plan *plan, uint64_t sector,
    uint8_t phase, bool zero, uint8_t **bytes)
{
  struct fat32_prepared_sector *previous = NULL;
  for (size_t i = 0; i < plan->count; ++i) {
    enum fat32_status status = fat32_step(plan->context);
    if (status != FAT32_OK) {
      return status;
    }
    struct fat32_prepared_sector *record = &plan->sectors[i];
    if (record->sector == sector) {
      if (record->phase == phase) {
        *bytes = record->bytes;
        return FAT32_OK;
      }
      if (record->phase < phase && (!previous || record->phase > previous->phase)) {
        previous = record;
      }
    }
  }
  if (plan->count == plan->capacity) {
    return FAT32_NO_MEMORY;
  }
  struct fat32_prepared_sector *record = &plan->sectors[plan->count++];
  record->sector = sector;
  record->phase = phase;
  if (zero) {
    fat32_zero(record->bytes, sizeof(record->bytes));
  } else if (previous) {
    fat32_copy(record->bytes, previous->bytes, sizeof(record->bytes));
  } else {
    const struct fat32_volume *volume = &plan->mounted->volume;
    enum fat32_status status;
    if (volume->mirrors && volume->fat_count == 2 &&
        sector >= volume->fat_first + volume->fat_sectors &&
        sector - volume->fat_first - volume->fat_sectors < volume->fat_sectors) {
      uint8_t *primary;
      status = sector_image(plan, sector - volume->fat_sectors, phase, false, &primary);
      if (status == FAT32_OK) {
        fat32_copy(record->bytes, primary, sizeof(record->bytes));
      }
    } else {
      status = fat32_partition_read(plan->context, sector, 1, record->bytes);
    }
    if (status != FAT32_OK) {
      return status;
    }
  }
  *bytes = record->bytes;
  return FAT32_OK;
}

static enum fat32_status fat_set(struct write_plan *plan, uint32_t cluster,
    uint32_t value, uint8_t phase)
{
  struct allocation_change *change = NULL;
  for (uint32_t i = 0; i < plan->change_count; ++i) {
    enum fat32_status status = fat32_step(plan->context);
    if (status != FAT32_OK) {
      return status;
    }
    if (plan->changes[i].cluster == cluster) {
      change = &plan->changes[i];
      break;
    }
  }
  if (!change) {
    if (plan->change_count == plan->change_capacity) {
      return FAT32_NO_MEMORY;
    }
    change = &plan->changes[plan->change_count++];
    *change = (struct allocation_change){.cluster = cluster};
  }
  if (phase >= change->phase) {
    change->phase = phase;
    change->value = value;
  }
  const struct fat32_volume *volume = &plan->mounted->volume;
  for (unsigned copy = 0; copy < volume->fat_count; ++copy) {
    uint8_t *bytes;
    enum fat32_status status = sector_image(plan, volume->fat_first +
        (uint64_t)copy * volume->fat_sectors + cluster / 128, phase, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    uint8_t *entry = bytes + (cluster % 128) * 4;
    put32(entry, (fat32_get32(entry) & ~FAT32_MASK) | value);
  }
  return FAT32_OK;
}

static enum fat32_status allocate_chain(struct write_plan *plan, uint32_t count,
    uint32_t **chain)
{
  struct fat32_mounted *mounted = plan->mounted;
  if (count > mounted->free_count - plan->allocated) {
    return FAT32_NO_SPACE;
  }
  if (count > plan->candidate_capacity - plan->allocated) {
    return FAT32_NO_MEMORY;
  }
  *chain = plan->free_clusters + plan->allocated;
  uint32_t found = 0;
  while (found < count) {
    if (plan->searched == mounted->volume.cluster_count) {
      return FAT32_CORRUPT;
    }
    uint32_t cluster = plan->next_candidate;
    if (++plan->next_candidate - 2 == mounted->volume.cluster_count) {
      plan->next_candidate = 2;
    }
    ++plan->searched;
    enum fat32_status status = fat32_step(plan->context);
    if (status != FAT32_OK) {
      return status;
    }
    if (!cluster_free(mounted, cluster)) {
      continue;
    }
    uint32_t value;
    status = fat32_fat_entry(plan->context, cluster, &value);
    if (status != FAT32_OK) {
      return status;
    }
    if (value) {
      return FAT32_CORRUPT;
    }
    (*chain)[found++] = cluster;
  }
  plan->allocated += count;
  for (uint32_t i = 0; i < count; ++i) {
    enum fat32_status status = fat_set(plan, (*chain)[i],
        i + 1 < count ? (*chain)[i + 1] : FAT32_MASK, FAT32_MOUNTED_FAT);
    if (status != FAT32_OK) {
      return status;
    }
  }
  return FAT32_OK;
}

static enum fat32_status zero_chain(struct write_plan *plan,
    const uint32_t *chain, uint32_t count)
{
  for (uint32_t i = 0; i < count; ++i) {
    for (unsigned sector = 0; sector < plan->mounted->volume.sectors_per_cluster; ++sector) {
      uint8_t *bytes;
      enum fat32_status status = sector_image(plan,
          cluster_sector(&plan->mounted->volume, chain[i]) + sector, FAT32_MOUNTED_DATA, true, &bytes);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  return FAT32_OK;
}

static enum fat32_status content(struct write_plan *plan, const uint32_t *old_chain,
    uint32_t old_count, const uint32_t *new_chain, uint32_t new_count,
    uint32_t offset, uint32_t length, fat32_payload_fn payload, void *context)
{
  const struct fat32_volume *volume = &plan->mounted->volume;
  uint32_t cluster_bytes = (uint32_t)volume->sectors_per_cluster * FAT32_SECTOR_BYTES;
  uint32_t processed = 0;
  while (processed < length) {
    uint64_t position = (uint64_t)offset + processed;
    uint32_t index = (uint32_t)(position / cluster_bytes);
    if (index >= old_count + new_count) {
      return FAT32_INVALID;
    }
    uint32_t cluster = index < old_count ? old_chain[index] : new_chain[index - old_count];
    uint64_t sector = cluster_sector(volume, cluster) +
        position % cluster_bytes / FAT32_SECTOR_BYTES;
    uint32_t within = (uint32_t)(position % FAT32_SECTOR_BYTES);
    uint32_t amount = FAT32_SECTOR_BYTES - within;
    if (amount > length - processed) {
      amount = length - processed;
    }
    uint8_t *bytes;
    enum fat32_status status = sector_image(plan, sector, FAT32_MOUNTED_DATA, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    if (payload) {
      if (!payload(context, processed, bytes + within, amount)) {
        return FAT32_INPUT;
      }
    } else {
      fat32_zero(bytes + within, amount);
    }
    processed += amount;
  }
  return FAT32_OK;
}

static enum fat32_status fsinfo_plan(struct write_plan *plan, uint32_t reclaimed)
{
  uint8_t *boot;
  enum fat32_status status = sector_image(plan, 0, 0, false, &boot);
  if (status != FAT32_OK) {
    return status;
  }
  /* The boot snapshot is preflight-only; never write it. */
  uint32_t info = fat32_get16(boot + 48), backup = fat32_get16(boot + 50);
  if (info == UINT16_MAX) {
    return FAT32_OK;
  }
  unsigned copies = backup && backup != UINT16_MAX ? 2 : 1;
  for (unsigned i = 0; i < copies; ++i) {
    uint8_t *bytes;
    status = sector_image(plan, info + (i ? backup : 0), FAT32_MOUNTED_FSINFO, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    put32(bytes + 488, plan->mounted->free_count - plan->allocated + reclaimed);
    put32(bytes + 492, UINT32_MAX);
  }
  return FAT32_OK;
}

static void short_entry(uint8_t *raw, const uint8_t alias[11], bool directory,
    uint32_t first, uint32_t bytes)
{
  fat32_zero(raw, 32);
  fat32_copy(raw, alias, 11);
  raw[11] = directory ? FAT32_ATTR_DIRECTORY : ENTRY_ARCHIVE;
  /* No clock authority: valid DOS minimum dates, zero times. */
  put16(raw + 16, DOS_FIRST_DATE);
  put16(raw + 18, DOS_FIRST_DATE);
  put16(raw + 24, DOS_FIRST_DATE);
  put16(raw + 20, (uint16_t)(first >> 16));
  put16(raw + 26, (uint16_t)first);
  put32(raw + 28, bytes);
}

static uint8_t checksum(const uint8_t alias[11])
{
  uint8_t value = 0;
  for (unsigned i = 0; i < 11; ++i) {
    value = (uint8_t)(((value & 1u) << 7) + (value >> 1) + alias[i]);
  }
  return value;
}

static void long_entries(struct write_plan *plan, const char *name,
    size_t length, const uint8_t alias[11], bool directory, uint32_t first, uint32_t bytes)
{
  static const uint8_t positions[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
  unsigned longs = (unsigned)((length + 12) / 13);
  plan->entry_count = (uint8_t)(longs + 1);
  for (unsigned i = 0; i < longs; ++i) {
    uint8_t *raw = plan->entries[i];
    fat32_zero(raw, 32);
    unsigned ordinal = longs - i;
    raw[0] = (uint8_t)ordinal | (!i ? ENTRY_LONG_LAST : 0);
    raw[11] = FAT32_ATTR_LONG;
    raw[13] = checksum(alias);
    for (unsigned j = 0; j < 13; ++j) {
      size_t index = (ordinal - 1) * 13 + j;
      uint16_t unit = index < length ? (uint8_t)name[index] :
          index == length ? 0 : UINT16_MAX;
      put16(raw + positions[j], unit);
    }
  }
  short_entry(plan->entries[longs], alias, directory, first, bytes);
}

static enum fat32_status find_slots(struct write_plan *plan, struct fat32_mounted_object *parent)
{
  const struct fat32_volume *volume = &plan->mounted->volume;
  uint32_t directory = parent->entry.first_cluster;
  uint32_t cluster = directory;
  uint64_t position = 0;
  unsigned run = 0;
  /* Reserve a spare slot too, retaining the end marker where one existed. */
  unsigned needed = plan->entry_count + 1;
  for (;;) {
    for (unsigned sector = 0; sector < volume->sectors_per_cluster; ++sector) {
      uint64_t address = cluster_sector(volume, cluster) + sector;
      const struct fat32_prepared_sector *prepared = NULL;
      for (size_t i = 0; i < plan->count; ++i) {
        enum fat32_status step = fat32_step(plan->context);
        if (step != FAT32_OK) {
          return step;
        }
        const struct fat32_prepared_sector *record = &plan->sectors[i];
        if (record->sector == address && record->phase <= FAT32_MOUNTED_DIRECTORY &&
            (!prepared || record->phase > prepared->phase)) {
          prepared = record;
        }
      }
      enum fat32_status status = FAT32_OK;
      if (prepared) {
        fat32_copy(plan->context->data, prepared->bytes, FAT32_SECTOR_BYTES);
      } else {
        status = fat32_partition_read(plan->context, address, 1, plan->context->data);
      }
      plan->context->data_cached = false;
      if (status != FAT32_OK) {
        return status;
      }
      for (unsigned slot = 0; slot < 16; ++slot) {
        status = fat32_step(plan->context);
        if (status != FAT32_OK) {
          return status;
        }
        uint8_t first = plan->context->data[slot * 32];
        ++position;
        if (first == 0 || first == ENTRY_DELETED) {
          if (run < plan->entry_count) {
            plan->entry_sectors[run] = cluster_sector(volume, cluster) + sector;
            plan->entry_slots[run] = (uint8_t)slot;
          }
          if (++run == needed) {
            plan->short_slot = position - 2;
            return FAT32_OK;
          }
        } else {
          run = 0;
        }
      }
    }
    uint32_t next;
    enum fat32_status status = fat32_fat_entry(plan->context, cluster, &next);
    if (status != FAT32_OK) {
      return status;
    }
    if (next >= FAT32_EOC) {
      break;
    }
    cluster = next;
  }
  uint32_t count = (needed - run + volume->sectors_per_cluster * 16 - 1) /
      (volume->sectors_per_cluster * 16);
  if (count > parent->chain_capacity - parent->chain_count) {
    return FAT32_NO_MEMORY;
  }
  uint32_t *chain;
  enum fat32_status status = allocate_chain(plan, count, &chain);
  if (status != FAT32_OK) {
    return status;
  }
  status = zero_chain(plan, chain, count);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat_set(plan, cluster, chain[0], FAT32_MOUNTED_FAT);
  if (status != FAT32_OK) {
    return status;
  }
  plan->grown_directory = parent;
  plan->directory_extension = chain;
  plan->directory_extension_count = count;
  plan->short_slot = position - run + plan->entry_count - 1;
  /* Continue the existing free tail, moving any end marker past the new group. */
  for (unsigned i = run; i < plan->entry_count; ++i) {
    unsigned index = i - run;
    unsigned cluster_index = index / (volume->sectors_per_cluster * 16);
    unsigned within = index % (volume->sectors_per_cluster * 16);
    plan->entry_sectors[i] = cluster_sector(volume, chain[cluster_index]) + within / 16;
    plan->entry_slots[i] = (uint8_t)(within % 16);
  }
  return FAT32_OK;
}

static enum fat32_status publish_entries(struct write_plan *plan)
{
  for (unsigned i = 0; i < plan->entry_count; ++i) {
    uint8_t *bytes;
    enum fat32_status status = sector_image(plan, plan->entry_sectors[i],
        FAT32_MOUNTED_DIRECTORY, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    fat32_copy(bytes + plan->entry_slots[i] * 32, plan->entries[i], 32);
  }
  return FAT32_OK;
}

static bool same_locator(const struct fat32_locator *a, const struct fat32_locator *b)
{
  return a->parent_cluster == b->parent_cluster &&
      a->parent_parent_cluster == b->parent_parent_cluster &&
      a->short_entry_slot == b->short_entry_slot;
}

static enum fat32_status object_valid(const struct fat32_mounted_object *object)
{
  if (!object || !object->mounted) {
    return FAT32_STALE;
  }
  struct fat32_mounted *mounted = object->mounted;
  if (mounted->stopped) {
    return FAT32_STOPPED;
  }
  for (struct fat32_mounted_object *item = mounted->objects; item; item = item->next) {
    if (item == object) {
      return FAT32_OK;
    }
  }
  return FAT32_STALE;
}

static enum fat32_status chain_collect(struct fat32_context *context,
    const struct fat32_entry *entry, uint32_t *chain, uint32_t capacity, uint32_t *count)
{
  *count = 0;
  if (!entry->first_cluster) {
    return entry->file_bytes ? FAT32_CORRUPT : FAT32_OK;
  }
  uint32_t cluster = entry->first_cluster;
  for (;;) {
    if (!fat32_valid_cluster(context->volume, cluster) ||
        *count >= context->volume->cluster_count) {
      return FAT32_CORRUPT;
    }
    if (chain) {
      if (*count == capacity) {
        return FAT32_NO_MEMORY;
      }
      chain[*count] = cluster;
    }
    ++*count;
    enum fat32_status status = fat32_fat_entry(context, cluster, &cluster);
    if (status != FAT32_OK) {
      return status;
    }
    if (cluster >= FAT32_EOC) {
      return FAT32_OK;
    }
  }
}

static enum fat32_status chain_matches(struct fat32_context *context,
    const struct fat32_mounted_object *object)
{
  if (object->chain_count > object->chain_capacity ||
      (object->chain_count && !object->chain) ||
      object->entry.first_cluster != (object->chain_count ? object->chain[0] : 0)) {
    return FAT32_STALE;
  }
  if (!(object->entry.attributes & FAT32_ATTR_DIRECTORY) &&
      object->chain_count != clusters_for(context->volume, object->entry.file_bytes)) {
    return FAT32_STALE;
  }
  for (uint32_t i = 0; i < object->chain_count; ++i) {
    if (!fat32_valid_cluster(context->volume, object->chain[i]) ||
        (object->mounted->allocation_valid && cluster_free(object->mounted, object->chain[i]))) {
      return FAT32_STALE;
    }
    uint32_t next;
    enum fat32_status status = fat32_fat_entry(context, object->chain[i], &next);
    if (status != FAT32_OK) {
      return status;
    }
    if ((i + 1 < object->chain_count && next != object->chain[i + 1]) ||
        (i + 1 == object->chain_count && next < FAT32_EOC)) {
      return FAT32_STALE;
    }
  }
  return FAT32_OK;
}

static enum fat32_status empty_directory(struct fat32_context *context,
    struct fat32_mounted_object *object)
{
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, object->entry.first_cluster);
  for (;;) {
    struct fat32_entry entry;
    bool found;
    enum fat32_status status = fat32_next_entry(context, &cursor,
        object->locator.parent_cluster, &entry, &found);
    if (status != FAT32_OK || !found) {
      return status;
    }
    if (fat32_ordinary_entry(&entry)) {
      return FAT32_NOT_EMPTY;
    }
  }
}

static enum fat32_status held_roots(struct fat32_context *context, void *extra,
    struct fat32_check_report *report)
{
  struct fat32_mounted *mounted = extra;
  for (struct fat32_mounted_object *object = mounted->objects; object; object = object->next) {
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK) {
      return status;
    }
    if (object->attached) {
      continue;
    }
    status = chain_matches(context, object);
    if (status != FAT32_OK) {
      return status;
    }
    bool directory = (object->entry.attributes & FAT32_ATTR_DIRECTORY) != 0;
    uint64_t clusters;
    status = fat32_claim_chain(context, object->entry.first_cluster,
        object->entry.file_bytes, directory, &clusters);
    if (status != FAT32_OK) {
      return status;
    }
    report->allocated_clusters += clusters;
    if (directory) {
      status = fat32_validate_directory(context, object->entry.first_cluster,
          object->locator.parent_cluster);
      if (status != FAT32_OK) {
        return status;
      }
      status = empty_directory(context, object);
      if (status != FAT32_OK) {
        return status == FAT32_NOT_EMPTY ? FAT32_CORRUPT : status;
      }
    }
  }
  return FAT32_OK;
}

static bool same_entry(const struct fat32_entry *a, const struct fat32_entry *b)
{
  return a->first_cluster == b->first_cluster && a->file_bytes == b->file_bytes &&
      a->attributes == b->attributes && a->name_bytes == b->name_bytes &&
      a->alias_bytes == b->alias_bytes && a->has_long_name == b->has_long_name &&
      fat32_equal(a->name, b->name, a->name_bytes) &&
      fat32_equal(a->alias, b->alias, a->alias_bytes) &&
      fat32_equal(a->short_name, b->short_name, sizeof(a->short_name));
}

static enum fat32_status validate_object(struct fat32_context *context,
    struct fat32_mounted_object *object)
{
  if (object->attached) {
    struct fat32_entry entry;
    enum fat32_status status = fat32_locate_entry(context, &object->locator, &entry);
    if (status != FAT32_OK) {
      if (status == FAT32_STALE || status == FAT32_UNSUPPORTED) {
        object->mounted->allocation_valid = false;
      }
      return mounted_result(object->mounted, status);
    }
    if (!same_entry(&entry, &object->entry)) {
      object->mounted->allocation_valid = false;
      return FAT32_STALE;
    }
  }
  enum fat32_status status = chain_matches(context, object);
  if (status == FAT32_OK && (object->entry.attributes & FAT32_ATTR_DIRECTORY)) {
    status = fat32_validate_directory(context, object->entry.first_cluster,
        object->locator.parent_cluster);
    if (status == FAT32_OK && !object->attached) {
      status = empty_directory(context, object);
      if (status == FAT32_NOT_EMPTY) {
        status = FAT32_CORRUPT;
      }
    }
  }
  if (status == FAT32_STALE || status == FAT32_UNSUPPORTED) {
    object->mounted->allocation_valid = false;
  }
  return mounted_result(object->mounted, status);
}

static enum fat32_status validate_parent(struct fat32_context *context,
    struct fat32_mounted_object *object)
{
  if (!object->attached) {
    return FAT32_OK;
  }
  for (struct fat32_mounted_object *parent = object->mounted->objects; parent; parent = parent->next) {
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK) {
      return status;
    }
    if (parent->attached && (parent->entry.attributes & FAT32_ATTR_DIRECTORY) &&
        parent->entry.first_cluster == object->locator.parent_cluster) {
      return validate_object(context, parent);
    }
  }
  return FAT32_OK;
}

static enum fat32_status mounted_context(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  if (!mounted) {
    return FAT32_INVALID;
  }
  if (mounted->stopped) {
    return FAT32_STOPPED;
  }
  enum fat32_status status = fat32_context_volume_local(&mounted->volume, workspace, context);
  if (status == FAT32_UNSUPPORTED) {
    mounted->allocation_valid = false;
  }
  if (status == FAT32_OK && mounted->allocation_valid) {
    (*context)->allocation = mounted->allocation;
  }
  return mounted_result(mounted, status);
}

static enum fat32_status full_check(struct fat32_mounted *mounted,
    struct fat32_context *context, struct fat32_check_report *report)
{
  enum fat32_status status = fat32_check_context_extra(context, report, held_roots, mounted);
  if (status != FAT32_OK) {
    return status;
  }
  context->seen = NULL;
  context->seen_bytes = 0;
  for (struct fat32_mounted_object *object = mounted->objects; object; object = object->next) {
    status = fat32_step(context);
    if (status == FAT32_OK) {
      status = validate_object(context, object);
    }
    if (status != FAT32_OK) {
      return status;
    }
  }
  return FAT32_OK;
}

enum fat32_status fat32_mounted_allocation_size(const struct fat32_volume *volume, size_t *bytes)
{
  if (!volume || !volume->sectors_per_cluster || volume->sectors_per_cluster > 128 ||
      (volume->sectors_per_cluster & (volume->sectors_per_cluster - 1)) ||
      (volume->fat_count != 1 && volume->fat_count != 2)) {
    return FAT32_INVALID;
  }
  size_t reader;
  enum fat32_status status = fat32_workspace_size(volume, 0, &reader);
  if (status == FAT32_OK && bytes) {
    *bytes = ((size_t)volume->cluster_count + 2 + 7) / 8;
  }
  return bytes ? status : FAT32_INVALID;
}

enum fat32_status fat32_mounted_workspace_size(const struct fat32_volume *volume,
    uint64_t changed_bytes, uint32_t allocated_clusters, uint32_t changed_clusters, size_t *bytes)
{
  size_t allocation;
  enum fat32_status status = fat32_mounted_allocation_size(volume, &allocation);
  if (status != FAT32_OK || !bytes) {
    return status != FAT32_OK ? status : FAT32_INVALID;
  }
  if (changed_bytes > UINT64_MAX - (FAT32_SECTOR_BYTES - 1) ||
      allocated_clusters > volume->cluster_count || changed_clusters > volume->cluster_count) {
    return FAT32_LIMIT;
  }
  uint64_t sectors = changed_clusters < volume->fat_sectors ? changed_clusters : volume->fat_sectors;
  uint64_t capacity = sectors * volume->fat_count * 2 +
      (changed_bytes + FAT32_SECTOR_BYTES - 1) / FAT32_SECTOR_BYTES +
      (uint64_t)volume->sectors_per_cluster * 3 + 64;
  uint64_t candidates = (uint64_t)allocated_clusters * sizeof(uint32_t);
  uint64_t changes = (uint64_t)changed_clusters * sizeof(struct allocation_change);
  if (candidates > SIZE_MAX - _Alignof(max_align_t) ||
      changes > SIZE_MAX - _Alignof(max_align_t)) {
    return FAT32_LIMIT;
  }
  size_t fixed = fat32_context_size();
  if (aligned((size_t)candidates) > SIZE_MAX - fixed) {
    return FAT32_LIMIT;
  }
  fixed += aligned((size_t)candidates);
  if (aligned((size_t)changes) > SIZE_MAX - fixed) {
    return FAT32_LIMIT;
  }
  fixed += aligned((size_t)changes);
  if (capacity > (SIZE_MAX - fixed) / sizeof(struct fat32_prepared_sector)) {
    return FAT32_LIMIT;
  }
  *bytes = fixed + (size_t)capacity * sizeof(struct fat32_prepared_sector);
  return FAT32_OK;
}

enum fat32_status fat32_mounted_open(struct fat32_mounted *mounted,
    const struct fat32_volume *volume, const struct fat32_write_io *io,
    uint8_t *allocation, size_t allocation_bytes, struct fat32_workspace *workspace)
{
  if (!mounted || !volume || !io || !io->write || !io->flush || !workspace) {
    return FAT32_INVALID;
  }
  if (mounted->stopped) {
    return FAT32_STOPPED;
  }
  if (mounted->allocation || mounted->objects || mounted->object_count) {
    return FAT32_INVALID;
  }
  if (!volume->clean_shutdown || !volume->no_hard_error) {
    return FAT32_CORRUPT;
  }
  struct fat32_mounted result;
  fat32_zero(&result, sizeof(result));
  fat32_copy(&result.volume, volume, sizeof(*volume));
  fat32_copy(&result.io, io, sizeof(*io));
  struct fat32_check_report report;
  enum fat32_status status = fat32_mounted_check(&result, allocation, allocation_bytes,
      workspace, &report);
  if (status == FAT32_OK) {
    fat32_copy(mounted, &result, sizeof(result));
  }
  return status;
}

enum fat32_status fat32_mounted_check(struct fat32_mounted *mounted,
    uint8_t *allocation, size_t allocation_bytes, struct fat32_workspace *workspace,
    struct fat32_check_report *report)
{
  if (!mounted || !report) {
    return FAT32_INVALID;
  }
  if (mounted->stopped) {
    return FAT32_STOPPED;
  }
  size_t required;
  enum fat32_status status = fat32_mounted_allocation_size(&mounted->volume, &required);
  if (status != FAT32_OK) {
    return status;
  }
  if (!allocation || allocation_bytes < required) {
    return FAT32_NO_MEMORY;
  }
  struct fat32_context *context;
  status = fat32_context_volume_prepare(&mounted->volume, workspace, &context);
  if (status != FAT32_OK) {
    return status;
  }
  mounted->allocation_valid = false;
  status = fat32_revalidate(context);
  if (status != FAT32_OK) {
    return status;
  }
  uint8_t *rebuilt = context->seen;
  context->free_map = rebuilt;
  struct fat32_check_report result;
  status = full_check(mounted, context, &result);
  if (status == FAT32_OK) {
    fat32_copy(allocation, rebuilt, required);
    mounted->allocation = allocation;
    mounted->allocation_bytes = required;
    mounted->free_count = (uint32_t)result.free_clusters;
    mounted->next_free = 2;
    mounted->allocation_valid = true;
    fat32_copy(report, &result, sizeof(result));
  }
  return status;
}

static enum fat32_status begin_plan(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace, struct write_plan *plan, bool mutation,
    bool allow_mirror_repair, uint32_t candidates, uint32_t changes)
{
  fat32_zero(plan, sizeof(*plan));
  plan->mounted = mounted;
  plan->allow_mirror_repair = allow_mirror_repair;
  if (!mounted->allocation_valid) {
    return mounted->stopped ? FAT32_STOPPED : FAT32_CORRUPT;
  }
  enum fat32_status status = mounted_context(mounted, workspace, &plan->context);
  if (status != FAT32_OK) {
    return status;
  }
  size_t vector = aligned((size_t)candidates * sizeof(uint32_t));
  size_t updates = aligned((size_t)changes * sizeof(struct allocation_change));
  size_t offset = fat32_context_size();
  if (vector > workspace->bytes - offset || updates > workspace->bytes - offset - vector) {
    return FAT32_NO_MEMORY;
  }
  plan->free_clusters = (void *)((uint8_t *)workspace->memory + offset);
  plan->candidate_capacity = candidates;
  offset += vector;
  plan->changes = (void *)((uint8_t *)workspace->memory + offset);
  plan->change_capacity = changes;
  offset += updates;
  plan->sectors = (void *)((uint8_t *)workspace->memory + offset);
  plan->capacity = (workspace->bytes - offset) / sizeof(*plan->sectors);
  plan->next_candidate = mounted->next_free;
  if (mutation && !mounted->dirty_owned) {
    for (unsigned copy = 0; copy < mounted->volume.fat_count; ++copy) {
      uint8_t *bytes;
      status = sector_image(plan, mounted->volume.fat_first +
          (uint64_t)copy * mounted->volume.fat_sectors, FAT32_MOUNTED_MARK, false, &bytes);
      if (status != FAT32_OK) {
        return status;
      }
      put32(bytes + 4, fat32_get32(bytes + 4) & ~FAT32_CLEAN);
    }
  }
  return FAT32_OK;
}

static enum fat32_status uncertain(struct fat32_mounted *mounted)
{
  mounted->allocation_valid = false;
  mounted->stopped = true;
  mounted->recovery.uncertain = true;
  mounted->recovery.failure = FAT32_IO;
  return FAT32_IO;
}

static enum fat32_status repair_mirror(struct write_plan *plan)
{
  struct fat32_mounted *mounted = plan->mounted;
  const struct fat32_volume *volume = &mounted->volume;
  struct fat32_mirror_state *mirrors = volume->mirrors;
  struct fat32_context *context = plan->context;
  mounted->recovery.phase = FAT32_MOUNTED_MIRROR;
  mounted->recovery.mirror_image = context->fat_bytes;
  context->fat_cached = false;
  for (uint32_t sector = 0; sector < mirrors->sector_count; ++sector) {
    if (fat32_step(context) != FAT32_OK) {
      return uncertain(mounted);
    }
    if (!(mirrors->pending[sector / 8] & (1u << (sector % 8)))) {
      continue;
    }
    uint64_t primary = volume->fat_first + sector;
    uint64_t secondary = primary + volume->fat_sectors;
    mounted->recovery.failed_sector = volume->partition_first + primary;
    enum fat32_status status = fat32_partition_read(context, primary, 1, context->fat_bytes);
    if (status != FAT32_OK) {
      return uncertain(mounted);
    }
    mounted->recovery.failed_sector = volume->partition_first + secondary;
    status = fat32_partition_read(context, secondary, 1, context->mirror_bytes);
    if (status != FAT32_OK) {
      return uncertain(mounted);
    }
    uint32_t entries = volume->cluster_count + 2 - sector * 128;
    if (entries > 128) {
      entries = 128;
    }
    bool different = false;
    for (uint32_t i = 0; i < entries; ++i) {
      if (fat32_step(context) != FAT32_OK) {
        return uncertain(mounted);
      }
      if ((fat32_get32(context->fat_bytes + 4 * i) & FAT32_MASK) !=
          (fat32_get32(context->mirror_bytes + 4 * i) & FAT32_MASK)) {
        different = true;
      }
    }
    if (different) {
      if (!context->sectors || fat32_step(context) != FAT32_OK) {
        return uncertain(mounted);
      }
      --context->sectors;
      if (!mounted->io.write(mounted->io.context, mounted->recovery.failed_sector,
          1, context->fat_bytes)) {
        return uncertain(mounted);
      }
    }
  }
  mounted->recovery.failed_sector = UINT64_MAX;
  if (fat32_step(context) != FAT32_OK || !mounted->io.flush(mounted->io.context)) {
    return uncertain(mounted);
  }
  fat32_zero(mirrors->pending, mirrors->pending_bytes);
  mirrors->pending_sectors = 0;
  mounted->recovery.mirror_image = NULL;
  return FAT32_OK;
}

static enum fat32_status run_plan(struct write_plan *plan,
    struct fat32_mounted_object *source, struct fat32_mounted_object *victim,
    const char *target, size_t target_bytes, bool clean)
{
  struct fat32_mounted *mounted = plan->mounted;
  struct fat32_mirror_state *mirrors = mounted->volume.mirrors;
  uint32_t repairs = fat32_mirror_pending(&mounted->volume);
  if (repairs && clean) {
    return FAT32_OK;
  }
  if (repairs && !plan->allow_mirror_repair) {
    return FAT32_READ_ONLY;
  }
  bool repair = repairs != 0;
  if (repair && !mounted->dirty_owned && !(mirrors->pending[0] & 1u)) {
    ++repairs;
  }
  uint64_t writes = 0;
  for (size_t i = 0; i < plan->count; ++i) {
    if (plan->sectors[i].phase) {
      ++writes;
    }
  }
  uint64_t repair_sectors = (uint64_t)repairs * 3;
  uint64_t repair_steps = repair ? mirrors->sector_count + (uint64_t)repairs * (128 + 3) + 1 : 0;
  if (writes > plan->context->sectors || repair_sectors > plan->context->sectors - writes ||
      writes + FAT32_MOUNTED_CLEAN + repair_steps > plan->context->steps) {
    return FAT32_LIMIT;
  }
  fat32_zero(&mounted->recovery, sizeof(mounted->recovery));
  mounted->recovery.source = source;
  mounted->recovery.victim = victim;
  mounted->recovery.target_parent = plan->target_parent;
  if (source) {
    fat32_copy(mounted->recovery.source_name, source->entry.name, source->entry.name_bytes);
  }
  if (target) {
    fat32_copy(mounted->recovery.target_name, target, target_bytes);
  }
  mounted->recovery.prepared_sectors = plan->sectors;
  mounted->recovery.prepared_sector_count = plan->count;
  mounted->recovery.allocated_chain = plan->free_clusters;
  mounted->recovery.allocated_clusters = plan->allocated;
  uint8_t first = clean ? FAT32_MOUNTED_CLEAN : FAT32_MOUNTED_MARK;
  uint8_t last = clean ? FAT32_MOUNTED_CLEAN : FAT32_MOUNTED_FSINFO;
  for (uint8_t phase = first; phase <= last; ++phase) {
    mounted->recovery.phase = phase;
    bool wrote = false;
    for (size_t i = 0; i < plan->count; ++i) {
      struct fat32_prepared_sector *record = &plan->sectors[i];
      if (record->phase != phase) {
        continue;
      }
      if (repair && phase == FAT32_MOUNTED_MARK &&
          record->sector != mounted->volume.fat_first) {
        continue;
      }
      wrote = true;
      mounted->recovery.failed_sector = mounted->volume.partition_first + record->sector;
      if (!mounted->io.write(mounted->io.context, mounted->recovery.failed_sector, 1, record->bytes)) {
        return uncertain(mounted);
      }
    }
    if (phase != FAT32_MOUNTED_MARK || wrote) {
      mounted->recovery.failed_sector = UINT64_MAX;
      if (!mounted->io.flush(mounted->io.context)) {
        return uncertain(mounted);
      }
      if (phase == FAT32_MOUNTED_MARK) {
        mounted->dirty_owned = true;
        mounted->volume.clean_shutdown = false;
      } else if (phase == FAT32_MOUNTED_CLEAN) {
        mounted->dirty_owned = false;
        mounted->volume.clean_shutdown = true;
      }
    }
    if (repair && phase == FAT32_MOUNTED_MARK) {
      /* A primary-only dirty mark may introduce a new sector-zero mismatch. */
      if (wrote && !(mirrors->pending[0] & 1u)) {
        mirrors->pending[0] |= 1u;
        ++mirrors->pending_sectors;
      }
      enum fat32_status status = repair_mirror(plan);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  if (!clean) {
    for (uint32_t i = 0; i < plan->change_count; ++i) {
      const struct allocation_change *change = &plan->changes[i];
      uint8_t mask = (uint8_t)(1u << (change->cluster % 8));
      bool was_free = cluster_free(mounted, change->cluster);
      bool free = change->value == 0;
      if (free) {
        mounted->allocation[change->cluster / 8] |= mask;
      } else {
        mounted->allocation[change->cluster / 8] &= (uint8_t)~mask;
      }
      if (was_free != free) {
        if (free) {
          ++mounted->free_count;
        } else {
          --mounted->free_count;
        }
      }
    }
    mounted->next_free = plan->next_candidate;
    mounted->mutated = true;
  }
  if (plan->grown_directory) {
    struct fat32_mounted_object *directory = plan->grown_directory;
    for (uint32_t i = 0; i < plan->directory_extension_count; ++i) {
      directory->chain[directory->chain_count++] = plan->directory_extension[i];
    }
  }
  fat32_zero(&mounted->recovery, sizeof(mounted->recovery));
  return FAT32_OK;
}

enum fat32_status fat32_mounted_root(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace, struct fat32_entry *entry, struct fat32_locator *locator)
{
  if (!entry || !locator) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = mounted_context(mounted, workspace, &context);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  struct fat32_locator root = {.short_entry_slot = UINT64_MAX};
  struct fat32_entry result;
  status = fat32_locate_entry(context, &root, &result);
  if (status == FAT32_UNSUPPORTED) {
    mounted->allocation_valid = false;
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  for (struct fat32_mounted_object *object = mounted->objects; object; object = object->next) {
    status = fat32_step(context);
    if (status == FAT32_OK && object->attached && same_locator(&object->locator, &root)) {
      status = validate_object(context, object);
      if (status == FAT32_OK) {
        break;
      }
    }
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
  }
  fat32_copy(entry, &result, sizeof(result));
  fat32_copy(locator, &root, sizeof(root));
  return FAT32_OK;
}

static void register_object(struct fat32_mounted *mounted,
    struct fat32_mounted_object *object)
{
  object->mounted = mounted;
  object->previous = NULL;
  object->next = mounted->objects;
  if (object->next) {
    object->next->previous = object;
  }
  mounted->objects = object;
  ++mounted->object_count;
}

enum fat32_status fat32_mounted_object_chain_size(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace, uint32_t *count)
{
  if (!count || !locator) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = mounted_context(mounted, workspace, &context);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  struct fat32_entry entry;
  uint32_t result;
  status = fat32_locate_entry(context, locator, &entry);
  if (status == FAT32_OK) {
    status = chain_collect(context, &entry, NULL, 0, &result);
  }
  if (status == FAT32_OK) {
    *count = result;
  }
  return mounted_result(mounted, status);
}

enum fat32_status fat32_mounted_object_bind(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace,
    struct fat32_mounted_object *object)
{
  if (!object || !locator || object->mounted || (object->chain_capacity && !object->chain)) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = mounted_context(mounted, workspace, &context);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  for (struct fat32_mounted_object *item = mounted->objects; item; item = item->next) {
    status = fat32_step(context);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    if (item->attached && same_locator(&item->locator, locator)) {
      return FAT32_EXISTS;
    }
  }
  if (mounted->object_count == UINT32_MAX) {
    return FAT32_LIMIT;
  }
  struct fat32_entry entry;
  uint32_t count;
  status = fat32_locate_entry(context, locator, &entry);
  if (status == FAT32_OK) {
    status = chain_collect(context, &entry, object->chain, object->chain_capacity, &count);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (count > object->chain_capacity) {
    return FAT32_NO_MEMORY;
  }
  fat32_copy(&object->entry, &entry, sizeof(entry));
  fat32_copy(&object->locator, locator, sizeof(*locator));
  object->chain_count = count;
  object->attached = true;
  register_object(mounted, object);
  return FAT32_OK;
}

enum fat32_status fat32_mounted_object_reserve(struct fat32_mounted_object *object,
    uint32_t *chain, uint32_t capacity)
{
  enum fat32_status status = object_valid(object);
  if (status != FAT32_OK) {
    return status;
  }
  if (capacity < object->chain_count || (capacity && !chain)) {
    return FAT32_NO_MEMORY;
  }
  if (chain != object->chain) {
    fat32_copy(chain, object->chain, (size_t)object->chain_count * sizeof(*chain));
  }
  object->chain = chain;
  object->chain_capacity = capacity;
  return FAT32_OK;
}

static enum fat32_status read_object_context(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace, struct fat32_context **context, bool read_windows)
{
  enum fat32_status status = object_valid(object);
  if (status != FAT32_OK) {
    return status;
  }
  status = mounted_context(object->mounted, workspace, context);
  if (status == FAT32_OK && read_windows && !(object->entry.attributes & FAT32_ATTR_DIRECTORY)) {
    fat32_context_read_windows(*context, workspace);
  }
  return status == FAT32_OK ? validate_object(*context, object) : status;
}

enum fat32_status fat32_mounted_locate(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace, struct fat32_entry *entry)
{
  struct fat32_mounted *mounted = object ? object->mounted : NULL;
  if (!entry) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = read_object_context(object, workspace, &context, true);
  if (status == FAT32_OK) {
    fat32_copy(entry, &object->entry, sizeof(*entry));
  }
  return mounted_result(mounted, status);
}

static enum fat32_status component_valid(const char *name, size_t bytes, bool mutation)
{
  if (!name || !bytes || (bytes == 1 && name[0] == '.') ||
      (bytes == 2 && name[0] == '.' && name[1] == '.')) {
    return FAT32_INVALID;
  }
  if (bytes > FAT32_NAME_MAX) {
    return FAT32_NAME_TOO_LONG;
  }
  if (mutation && (name[bytes - 1] == ' ' || name[bytes - 1] == '.')) {
    return FAT32_INVALID;
  }
  for (size_t i = 0; i < bytes; ++i) {
    uint8_t byte = (uint8_t)name[i];
    if (byte >= 128) {
      return FAT32_UNSUPPORTED;
    }
    if (byte < 32 || byte == '/' || (mutation && (byte == 127 || byte == '"' ||
        byte == '*' || byte == ':' || byte == '<' || byte == '>' || byte == '?' ||
        byte == '\\' || byte == '|'))) {
      return FAT32_INVALID;
    }
  }
  return FAT32_OK;
}

static enum fat32_status directory_context(struct fat32_mounted_object *directory,
    struct fat32_workspace *workspace, struct fat32_context **context)
{
  enum fat32_status status = read_object_context(directory, workspace, context, false);
  if (status == FAT32_OK && !(directory->entry.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  return status;
}

static struct fat32_locator directory_child(struct fat32_mounted_object *directory,
    uint64_t position)
{
  return (struct fat32_locator) {
    .parent_cluster = directory->entry.first_cluster,
    .parent_parent_cluster = directory->locator.parent_cluster,
    .short_entry_slot = position,
  };
}

enum fat32_status fat32_mounted_lookup(struct fat32_mounted_object *directory,
    const char *component, size_t bytes, struct fat32_workspace *workspace,
    struct fat32_entry *entry, struct fat32_locator *child)
{
  struct fat32_mounted *mounted = directory ? directory->mounted : NULL;
  if (!entry || !child) {
    return FAT32_INVALID;
  }
  enum fat32_status status = component_valid(component, bytes, false);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  struct fat32_context *context;
  status = directory_context(directory, workspace, &context);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, directory->entry.first_cluster);
  for (;;) {
    struct fat32_entry candidate;
    bool found;
    status = fat32_next_entry(context, &cursor, directory->locator.parent_cluster, &candidate, &found);
    if (status != FAT32_OK || !found) {
      return mounted_result(mounted, status != FAT32_OK ? status : FAT32_NOT_FOUND);
    }
    if (!fat32_ordinary_entry(&candidate) ||
        !((candidate.name_ascii && fat32_ascii_equal(candidate.name, candidate.name_bytes, component, bytes)) ||
          (candidate.alias_ascii && fat32_ascii_equal(candidate.alias, candidate.alias_bytes, component, bytes)))) {
      continue;
    }
    uint64_t clusters;
    status = fat32_claim_chain(context, candidate.first_cluster, candidate.file_bytes,
        (candidate.attributes & FAT32_ATTR_DIRECTORY) != 0, &clusters);
    if (status == FAT32_OK && (candidate.attributes & FAT32_ATTR_DIRECTORY)) {
      status = fat32_validate_directory(context, candidate.first_cluster, directory->entry.first_cluster);
    }
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    fat32_copy(entry, &candidate, sizeof(*entry));
    *child = directory_child(directory, cursor.position - 1);
    return FAT32_OK;
  }
}

enum fat32_status fat32_mounted_enumerate(struct fat32_mounted_object *directory,
    uint64_t continuation, struct fat32_workspace *workspace,
    struct fat32_entry *entry, struct fat32_locator *child, uint64_t *next, bool *found)
{
  struct fat32_mounted *mounted = directory ? directory->mounted : NULL;
  if (!entry || !child || !next || !found) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = directory_context(directory, workspace, &context);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, directory->entry.first_cluster);
  struct fat32_entry result;
  struct fat32_locator locator;
  bool selected = false;
  uint64_t following = UINT64_MAX;
  for (;;) {
    struct fat32_entry candidate;
    bool have;
    status = fat32_next_entry(context, &cursor, directory->locator.parent_cluster, &candidate, &have);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    if (!have) {
      break;
    }
    if (!fat32_ordinary_entry(&candidate)) {
      continue;
    }
    uint64_t clusters;
    status = fat32_claim_chain(context, candidate.first_cluster, candidate.file_bytes,
        (candidate.attributes & FAT32_ATTR_DIRECTORY) != 0, &clusters);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    if (!selected && continuation != UINT64_MAX && cursor.position - 1 >= continuation) {
      selected = true;
      fat32_copy(&result, &candidate, sizeof(result));
      locator = directory_child(directory, cursor.position - 1);
      following = cursor.position;
    }
  }
  if (selected) {
    fat32_copy(entry, &result, sizeof(*entry));
    *child = locator;
  }
  *next = following;
  *found = selected;
  return FAT32_OK;
}

enum fat32_status fat32_mounted_read_range(struct fat32_mounted_object *object,
    uint64_t offset, size_t bytes, struct fat32_workspace *workspace,
    fat32_bytes_sink_fn sink, void *output)
{
  struct fat32_mounted *mounted = object ? object->mounted : NULL;
  if (!sink) {
    return FAT32_INVALID;
  }
  struct fat32_context *context;
  enum fat32_status status = read_object_context(object, workspace, &context, true);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (object->entry.attributes & FAT32_ATTR_DIRECTORY) {
    return FAT32_IS_DIRECTORY;
  }
  if (offset >= object->entry.file_bytes) {
    return FAT32_OK;
  }
  uint64_t remaining = object->entry.file_bytes - offset;
  if (remaining > bytes) {
    remaining = bytes;
  }
  uint32_t cluster_bytes = (uint32_t)object->mounted->volume.sectors_per_cluster * FAT32_SECTOR_BYTES;
  while (remaining) {
    uint32_t index = (uint32_t)(offset / cluster_bytes);
    uint64_t sector = cluster_sector(&object->mounted->volume, object->chain[index]) +
        offset % cluster_bytes / FAT32_SECTOR_BYTES;
    size_t skip = (size_t)(offset % FAT32_SECTOR_BYTES);
    uint32_t sectors = 1;
    while (sectors < FAT32_IO_BYTES / FAT32_SECTOR_BYTES &&
        (uint64_t)sectors * FAT32_SECTOR_BYTES < remaining + skip) {
      uint64_t next = offset - skip + (uint64_t)sectors * FAT32_SECTOR_BYTES;
      uint32_t next_index = (uint32_t)(next / cluster_bytes);
      uint64_t next_sector = cluster_sector(&object->mounted->volume, object->chain[next_index]) +
          next % cluster_bytes / FAT32_SECTOR_BYTES;
      if (next_sector != sector + sectors) {
        break;
      }
      ++sectors;
    }
    size_t amount = (size_t)sectors * FAT32_SECTOR_BYTES - skip;
    if (amount > remaining) {
      amount = (size_t)remaining;
    }
    status = fat32_partition_read(context, sector, sectors, context->data);
    context->data_cached = false;
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    if (!sink(output, context->data + skip, amount)) {
      return FAT32_OUTPUT;
    }
    offset += amount;
    remaining -= amount;
  }
  return FAT32_OK;
}

static enum fat32_status mutable_file(struct fat32_mounted_object *object)
{
  enum fat32_status status = object_valid(object);
  if (status != FAT32_OK) {
    return status;
  }
  if (object->entry.attributes & FAT32_ATTR_DIRECTORY) {
    return FAT32_IS_DIRECTORY;
  }
  if (object->entry.attributes & FAT32_ATTR_READ_ONLY) {
    return FAT32_READ_ONLY;
  }
  return object->entry.name_ascii && object->entry.alias_ascii ? FAT32_OK : FAT32_UNSUPPORTED;
}

static enum fat32_status mutable_directory(struct fat32_mounted_object *object)
{
  enum fat32_status status = object_valid(object);
  if (status != FAT32_OK) {
    return status;
  }
  if (!(object->entry.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  if (!object->attached) {
    return FAT32_STALE;
  }
  if (object->locator.parent_cluster && (!object->entry.name_ascii || !object->entry.alias_ascii)) {
    return FAT32_UNSUPPORTED;
  }
  return FAT32_OK;
}

static enum fat32_status entry_group(struct write_plan *plan,
    struct fat32_mounted_object *object)
{
  if (!object->attached || !object->locator.parent_cluster ||
      !object->entry.name_ascii || !object->entry.alias_ascii) {
    return FAT32_UNSUPPORTED;
  }
  unsigned count = object->entry.has_long_name ? (object->entry.name_bytes + 12) / 13 + 1 : 1;
  uint64_t short_slot = object->locator.short_entry_slot;
  if (short_slot < count - 1) {
    return FAT32_STALE;
  }
  uint64_t first_slot = short_slot - count + 1;
  uint32_t slots_per_cluster = plan->mounted->volume.sectors_per_cluster * 16;
  uint32_t cluster = object->locator.parent_cluster;
  uint64_t skip = first_slot / slots_per_cluster;
  while (skip--) {
    enum fat32_status status = fat32_fat_entry(plan->context, cluster, &cluster);
    if (status != FAT32_OK) {
      return status;
    }
    if (!fat32_valid_cluster(&plan->mounted->volume, cluster)) {
      return FAT32_STALE;
    }
  }
  unsigned within = (unsigned)(first_slot % slots_per_cluster);
  plan->entry_count = (uint8_t)count;
  for (unsigned i = 0; i < count; ++i) {
    uint64_t sector = cluster_sector(&plan->mounted->volume, cluster) + within / 16;
    uint8_t *bytes;
    enum fat32_status status = sector_image(plan, sector, FAT32_MOUNTED_SNAPSHOT, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    plan->entry_sectors[i] = sector;
    plan->entry_slots[i] = (uint8_t)(within % 16);
    fat32_copy(plan->entries[i], bytes + (within % 16) * 32, 32);
    if (++within == slots_per_cluster && i + 1 < count) {
      within = 0;
      status = fat32_fat_entry(plan->context, cluster, &cluster);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  return FAT32_OK;
}

static enum fat32_status delete_entry(struct write_plan *plan,
    struct fat32_mounted_object *object)
{
  enum fat32_status status = entry_group(plan, object);
  if (status != FAT32_OK) {
    return status;
  }
  for (unsigned i = 0; i < plan->entry_count; ++i) {
    uint8_t *bytes;
    status = sector_image(plan, plan->entry_sectors[i], FAT32_MOUNTED_DIRECTORY, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    bytes[plan->entry_slots[i] * 32] = ENTRY_DELETED;
  }
  return FAT32_OK;
}

static enum fat32_status update_size(struct write_plan *plan,
    struct fat32_mounted_object *object, uint32_t first, uint32_t size)
{
  if (!object->attached) {
    return FAT32_OK;
  }
  enum fat32_status status = entry_group(plan, object);
  if (status != FAT32_OK) {
    return status;
  }
  unsigned index = plan->entry_count - 1;
  uint8_t *bytes;
  status = sector_image(plan, plan->entry_sectors[index], FAT32_MOUNTED_DIRECTORY, false, &bytes);
  if (status != FAT32_OK) {
    return status;
  }
  uint8_t *raw = bytes + plan->entry_slots[index] * 32;
  put16(raw + 20, (uint16_t)(first >> 16));
  put16(raw + 26, (uint16_t)first);
  put32(raw + 28, size);
  return FAT32_OK;
}

struct mounted_payload {
  const uint8_t *bytes;
};

static bool payload_copy(void *context, uint32_t offset, void *bytes, size_t count)
{
  const struct mounted_payload *payload = context;
  fat32_copy(bytes, payload->bytes + offset, count);
  return true;
}

static enum fat32_status file_update(struct fat32_mounted_object *object,
    uint32_t final, uint32_t offset, const void *payload, uint32_t payload_bytes,
    struct fat32_workspace *workspace)
{
  enum fat32_status status = mutable_file(object);
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t old_size = object->entry.file_bytes;
  if (!payload_bytes && final == old_size) {
    struct fat32_context *context;
    return read_object_context(object, workspace, &context, false);
  }
  uint32_t total = clusters_for(&object->mounted->volume, final);
  if (total > object->chain_capacity) {
    return FAT32_NO_MEMORY;
  }
  uint32_t old_count = object->chain_count;
  uint32_t added = total > old_count ? total - old_count : 0;
  uint32_t removed = old_count > total ? old_count - total : 0;
  struct write_plan plan;
  status = begin_plan(object->mounted, workspace, &plan, true, true, added, added + removed + 1);
  if (status == FAT32_OK) {
    status = validate_parent(plan.context, object);
  }
  if (status == FAT32_OK) {
    status = validate_object(plan.context, object);
  }
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t *new_chain;
  status = allocate_chain(&plan, added, &new_chain);
  if (status == FAT32_OK) {
    status = zero_chain(&plan, new_chain, added);
  }
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t cluster_bytes = (uint32_t)object->mounted->volume.sectors_per_cluster * FAT32_SECTOR_BYTES;
  if (final > old_size) {
    /* Zero pre-existing slack too; newly allocated clusters are already zero. */
    uint32_t slack = (uint32_t)((uint64_t)old_count * cluster_bytes - old_size);
    if (slack) {
      status = content(&plan, object->chain, old_count, new_chain, added,
          old_size, slack, NULL, NULL);
      if (status != FAT32_OK) {
        return status;
      }
    }
    if (added && old_count) {
      status = fat_set(&plan, object->chain[old_count - 1], new_chain[0], FAT32_MOUNTED_FAT);
      if (status != FAT32_OK) {
        return status;
      }
    }
  } else if (final < old_size && total) {
    uint32_t slack = (uint32_t)((uint64_t)total * cluster_bytes - final);
    if (slack) {
      status = content(&plan, object->chain, total, NULL, 0, final, slack, NULL, NULL);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  if (payload_bytes) {
    struct mounted_payload input = { payload };
    status = content(&plan, object->chain, old_count, new_chain, added,
        offset, payload_bytes, payload_copy, &input);
    if (status != FAT32_OK) {
      return status;
    }
  }
  uint32_t first = total ? old_count ? object->chain[0] : new_chain[0] : 0;
  status = update_size(&plan, object, first, final);
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t reclaimed = old_count > total ? old_count - total : 0;
  if (reclaimed && total) {
    /* Smaller length is durable before cutting the retained prefix. */
    status = fat_set(&plan, object->chain[total - 1], FAT32_MASK, FAT32_MOUNTED_RECLAIM);
    if (status != FAT32_OK) {
      return status;
    }
  }
  for (uint32_t i = total; i < old_count; ++i) {
    status = fat_set(&plan, object->chain[i], 0, FAT32_MOUNTED_RECLAIM);
    if (status != FAT32_OK) {
      return status;
    }
  }
  status = fsinfo_plan(&plan, reclaimed);
  if (status == FAT32_OK) {
    status = run_plan(&plan, object, NULL, object->entry.name, object->entry.name_bytes, false);
  }
  if (status != FAT32_OK) {
    return status;
  }
  for (uint32_t i = 0; i < added; ++i) {
    object->chain[old_count + i] = new_chain[i];
  }
  object->chain_count = total;
  object->entry.first_cluster = first;
  object->entry.file_bytes = final;
  return FAT32_OK;
}

enum fat32_status fat32_mounted_write(struct fat32_mounted_object *object,
    uint64_t offset, const void *payload, size_t bytes, struct fat32_workspace *workspace)
{
  struct fat32_mounted *mounted = object ? object->mounted : NULL;
  enum fat32_status status = mutable_file(object);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (bytes > FAT32_MOUNTED_PAYLOAD_MAX ||
      (bytes && (offset > UINT32_MAX || bytes > UINT32_MAX - offset))) {
    return FAT32_LIMIT;
  }
  if (bytes && !payload) {
    return FAT32_INVALID;
  }
  uint32_t final = object->entry.file_bytes;
  if (bytes && offset + bytes > final) {
    final = (uint32_t)(offset + bytes);
  }
  return mounted_result(mounted, file_update(object, final, bytes ? (uint32_t)offset : 0, payload, (uint32_t)bytes, workspace));
}

enum fat32_status fat32_mounted_resize(struct fat32_mounted_object *object,
    uint64_t bytes, struct fat32_workspace *workspace)
{
  struct fat32_mounted *mounted = object ? object->mounted : NULL;
  if (bytes > UINT32_MAX) {
    return FAT32_LIMIT;
  }
  return mounted_result(mounted, file_update(object, (uint32_t)bytes, 0, NULL, 0, workspace));
}

static enum fat32_status namespace_scan(struct write_plan *plan,
    struct fat32_mounted_object *directory, const char *name, size_t bytes,
    const char *alias, size_t alias_bytes, const struct fat32_mounted_object *exclude_source,
    const struct fat32_mounted_object *exclude_victim, bool *collision,
    struct fat32_locator *matched)
{
  *collision = false;
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, directory->entry.first_cluster);
  for (;;) {
    struct fat32_entry entry;
    bool found;
    enum fat32_status status = fat32_next_entry(plan->context, &cursor,
        directory->locator.parent_cluster, &entry, &found);
    if (status != FAT32_OK || !found) {
      return status;
    }
    if (!fat32_ordinary_entry(&entry)) {
      continue;
    }
    if (!entry.name_ascii || !entry.alias_ascii) {
      return FAT32_UNSUPPORTED;
    }
    struct fat32_locator locator = directory_child(directory, cursor.position - 1);
    if ((exclude_source && same_locator(&locator, &exclude_source->locator)) ||
        (exclude_victim && same_locator(&locator, &exclude_victim->locator))) {
      continue;
    }
    if (fat32_ascii_equal(entry.name, entry.name_bytes, name, bytes) ||
        fat32_ascii_equal(entry.alias, entry.alias_bytes, name, bytes) ||
        (alias && (fat32_ascii_equal(entry.name, entry.name_bytes, alias, alias_bytes) ||
          fat32_ascii_equal(entry.alias, entry.alias_bytes, alias, alias_bytes)))) {
      if (*collision) {
        return FAT32_CORRUPT;
      }
      *collision = true;
      if (matched) {
        *matched = locator;
      }
    }
  }
}

static enum fat32_status mounted_alias(struct write_plan *plan,
    struct fat32_mounted_object *directory, const char *name, size_t bytes,
    const struct fat32_mounted_object *source, const struct fat32_mounted_object *victim,
    uint8_t alias[11])
{
  bool collision;
  enum fat32_status status = namespace_scan(plan, directory, name, bytes,
      NULL, 0, source, victim, &collision, NULL);
  if (status != FAT32_OK || collision) {
    return status != FAT32_OK ? status : FAT32_EXISTS;
  }
  static const char digits[] = "0123456789ABCDEF";
  for (uint32_t number = 1; number < UINT32_C(0x10000000); ++number) {
    alias[0] = 'P';
    for (unsigned i = 0; i < 7; ++i) {
      alias[7 - i] = (uint8_t)digits[(number >> (i * 4)) & 15];
    }
    alias[8] = alias[9] = alias[10] = ' ';
    status = namespace_scan(plan, directory, name, bytes, (const char *)alias, 8,
        source, victim, &collision, NULL);
    if (status != FAT32_OK || !collision) {
      return status;
    }
  }
  return FAT32_LIMIT;
}

static void created_entry(struct fat32_entry *entry, const struct write_plan *plan,
    const char *name, size_t bytes, bool directory, uint32_t first, uint32_t size)
{
  fat32_zero(entry, sizeof(*entry));
  fat32_copy(entry->name, name, bytes);
  entry->name_bytes = (uint16_t)bytes;
  fat32_copy(entry->short_name, plan->entries[plan->entry_count - 1], 11);
  fat32_copy(entry->alias, entry->short_name, 8);
  entry->alias_bytes = 8;
  entry->name_ascii = entry->alias_ascii = true;
  entry->has_long_name = true;
  entry->attributes = directory ? FAT32_ATTR_DIRECTORY : ENTRY_ARCHIVE;
  entry->first_cluster = first;
  entry->file_bytes = size;
}

enum fat32_status fat32_mounted_create(struct fat32_mounted_object *parent,
    const char *component, size_t bytes, bool directory,
    struct fat32_workspace *workspace, struct fat32_mounted_object *object)
{
  struct fat32_mounted *mounted = parent ? parent->mounted : NULL;
  enum fat32_status status = mutable_directory(parent);
  if (status == FAT32_OK) {
    status = component_valid(component, bytes, true);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (!object || object->mounted || (object->chain_capacity && !object->chain)) {
    return FAT32_INVALID;
  }
  if (parent->mounted->object_count == UINT32_MAX) {
    return FAT32_LIMIT;
  }
  if (directory && !object->chain_capacity) {
    return FAT32_NO_MEMORY;
  }
  struct write_plan plan;
  status = begin_plan(parent->mounted, workspace, &plan, true, true, 3, 4);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  status = validate_object(plan.context, parent);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  uint8_t alias[11];
  status = mounted_alias(&plan, parent, component, bytes, NULL, NULL, alias);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  uint32_t count = directory ? 1 : 0;
  uint32_t *chain;
  status = allocate_chain(&plan, count, &chain);
  if (status == FAT32_OK) {
    status = zero_chain(&plan, chain, count);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (directory) {
    uint8_t *data;
    status = sector_image(&plan, cluster_sector(&parent->mounted->volume, chain[0]),
        FAT32_MOUNTED_DATA, false, &data);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    uint8_t dots[11];
    for (unsigned i = 0; i < 11; ++i) {
      dots[i] = ' ';
    }
    dots[0] = '.';
    short_entry(data, dots, true, chain[0], 0);
    dots[1] = '.';
    short_entry(data + 32, dots, true,
        parent->entry.first_cluster == parent->mounted->volume.root_cluster ? 0 : parent->entry.first_cluster, 0);
  }
  uint32_t first = count ? chain[0] : 0;
  long_entries(&plan, component, bytes, alias, directory, first, 0);
  status = find_slots(&plan, parent);
  if (status == FAT32_OK) {
    status = publish_entries(&plan);
  }
  if (status == FAT32_OK) {
    status = fsinfo_plan(&plan, 0);
  }
  if (status == FAT32_OK) {
    plan.target_parent = parent;
    status = run_plan(&plan, NULL, NULL, component, bytes, false);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  created_entry(&object->entry, &plan, component, bytes, directory, first, 0);
  object->locator = directory_child(parent, plan.short_slot);
  object->chain_count = count;
  if (count) {
    object->chain[0] = chain[0];
  }
  object->attached = true;
  register_object(parent->mounted, object);
  return FAT32_OK;
}

enum fat32_status fat32_mounted_remove(struct fat32_mounted_object *parent,
    struct fat32_mounted_object *object, struct fat32_workspace *workspace)
{
  struct fat32_mounted *mounted = parent ? parent->mounted : NULL;
  enum fat32_status status = mutable_directory(parent);
  if (status == FAT32_OK) {
    status = object_valid(object);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (object->mounted != parent->mounted || !object->attached ||
      object->locator.parent_cluster != parent->entry.first_cluster) {
    return FAT32_STALE;
  }
  if (!object->entry.name_ascii || !object->entry.alias_ascii) {
    return FAT32_UNSUPPORTED;
  }
  if (!(object->entry.attributes & FAT32_ATTR_DIRECTORY) && (object->entry.attributes & FAT32_ATTR_READ_ONLY)) {
    return FAT32_READ_ONLY;
  }
  struct write_plan plan;
  status = begin_plan(parent->mounted, workspace, &plan, true, true, 0, 0);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  status = validate_object(plan.context, parent);
  if (status == FAT32_OK) {
    status = validate_object(plan.context, object);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  bool collision;
  status = namespace_scan(&plan, parent, object->entry.name, object->entry.name_bytes,
      NULL, 0, NULL, NULL, &collision, NULL);
  if (status == FAT32_OK && (object->entry.attributes & FAT32_ATTR_DIRECTORY)) {
    status = empty_directory(plan.context, object);
  }
  if (status == FAT32_OK) {
    status = delete_entry(&plan, object);
  }
  if (status == FAT32_OK) {
    status = fsinfo_plan(&plan, 0);
  }
  if (status == FAT32_OK) {
    plan.target_parent = parent;
    status = run_plan(&plan, object, NULL, object->entry.name, object->entry.name_bytes, false);
  }
  if (status == FAT32_OK) {
    object->attached = false;
  }
  return mounted_result(mounted, status);
}

enum fat32_status fat32_mounted_rename(struct fat32_mounted_object *source_parent,
    struct fat32_mounted_object *source, struct fat32_mounted_object *target_parent,
    const char *component, size_t bytes, bool replace,
    struct fat32_mounted_object *victim, struct fat32_workspace *workspace)
{
  struct fat32_mounted *mounted = source ? source->mounted : NULL;
  enum fat32_status status = mutable_directory(source_parent);
  if (status == FAT32_OK) {
    status = mutable_directory(target_parent);
  }
  if (status == FAT32_OK) {
    status = mutable_file(source);
  }
  if (status == FAT32_OK) {
    status = component_valid(component, bytes, true);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (source->mounted != source_parent->mounted || target_parent->mounted != source->mounted ||
      !source->attached || source->locator.parent_cluster != source_parent->entry.first_cluster) {
    return FAT32_STALE;
  }
  if (victim && victim != source) {
    status = mutable_file(victim);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    if (victim->mounted != source->mounted || !victim->attached ||
        victim->locator.parent_cluster != target_parent->entry.first_cluster) {
      return FAT32_STALE;
    }
  }
  struct write_plan plan;
  status = begin_plan(source->mounted, workspace, &plan, true, true, 2, 3);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  status = validate_object(plan.context, source_parent);
  if (status == FAT32_OK && target_parent != source_parent) {
    status = validate_object(plan.context, target_parent);
  }
  if (status == FAT32_OK) {
    status = validate_object(plan.context, source);
  }
  if (status == FAT32_OK && victim && victim != source) {
    status = validate_object(plan.context, victim);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  bool collision;
  struct fat32_locator matched;
  status = namespace_scan(&plan, source_parent, source->entry.name, source->entry.name_bytes,
      NULL, 0, NULL, NULL, &collision, NULL);
  if (status == FAT32_OK) {
    status = namespace_scan(&plan, target_parent, component, bytes,
        NULL, 0, NULL, NULL, &collision, &matched);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (collision) {
    if (same_locator(&matched, &source->locator)) {
      /* Alias/self rename is a successful no-op, preserving current spelling. */
      return mounted_result(mounted, victim && victim != source ? FAT32_STALE : FAT32_OK);
    }
    if (!replace) {
      return FAT32_EXISTS;
    }
    if (!victim || !same_locator(&matched, &victim->locator)) {
      return FAT32_STALE;
    }
  } else if (victim) {
    return FAT32_STALE;
  }
  status = entry_group(&plan, source);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  uint8_t old_short[32];
  fat32_copy(old_short, plan.entries[plan.entry_count - 1], sizeof(old_short));
  if (victim) {
    status = delete_entry(&plan, victim);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
  }
  status = delete_entry(&plan, source);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  uint8_t alias[11];
  status = mounted_alias(&plan, target_parent, component, bytes, source, victim, alias);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  long_entries(&plan, component, bytes, alias, false, source->entry.first_cluster, source->entry.file_bytes);
  /* Rename preserves attributes/timestamps; the generated alias has no NT case flags. */
  uint8_t *raw = plan.entries[plan.entry_count - 1];
  fat32_copy(raw + 11, old_short + 11, 21);
  raw[12] = 0;
  status = find_slots(&plan, target_parent);
  if (status == FAT32_OK) {
    status = publish_entries(&plan);
  }
  if (status == FAT32_OK) {
    status = fsinfo_plan(&plan, 0);
  }
  if (status == FAT32_OK) {
    plan.target_parent = target_parent;
    status = run_plan(&plan, source, victim, component, bytes, false);
  }
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  uint8_t attributes = source->entry.attributes;
  created_entry(&source->entry, &plan, component, bytes, false,
      source->entry.first_cluster, source->entry.file_bytes);
  source->entry.attributes = attributes;
  source->locator = directory_child(target_parent, plan.short_slot);
  if (victim) {
    victim->attached = false;
  }
  return FAT32_OK;
}

static void unregister_object(struct fat32_mounted_object *object)
{
  struct fat32_mounted *mounted = object->mounted;
  if (object->previous) {
    object->previous->next = object->next;
  } else {
    mounted->objects = object->next;
  }
  if (object->next) {
    object->next->previous = object->previous;
  }
  --mounted->object_count;
  object->mounted = NULL;
  object->previous = object->next = NULL;
  object->chain_count = 0;
  object->attached = false;
}

enum fat32_status fat32_mounted_object_forget(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace)
{
  struct fat32_mounted *mounted = object ? object->mounted : NULL;
  enum fat32_status status = object_valid(object);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  if (object->attached || !object->chain_count) {
    unregister_object(object);
    return FAT32_OK;
  }
  struct write_plan plan;
  status = begin_plan(object->mounted, workspace, &plan, true, false, 0, object->chain_count);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  status = validate_object(plan.context, object);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  for (uint32_t i = 0; i < object->chain_count; ++i) {
    status = fat_set(&plan, object->chain[i], 0, FAT32_MOUNTED_RECLAIM);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
  }
  status = fsinfo_plan(&plan, object->chain_count);
  if (status == FAT32_OK) {
    status = run_plan(&plan, object, NULL, object->entry.name, object->entry.name_bytes, false);
  }
  if (status == FAT32_OK) {
    unregister_object(object);
  }
  return mounted_result(mounted, status);
}

enum fat32_status fat32_mounted_sync(struct fat32_mounted *mounted)
{
  if (!mounted) {
    return FAT32_INVALID;
  }
  if (mounted->stopped) {
    return FAT32_STOPPED;
  }
  fat32_zero(&mounted->recovery, sizeof(mounted->recovery));
  mounted->recovery.failed_sector = UINT64_MAX;
  mounted->recovery.phase = FAT32_MOUNTED_FSINFO;
  if (!mounted->io.flush(mounted->io.context)) {
    return mounted_result(mounted, uncertain(mounted));
  }
  fat32_zero(&mounted->recovery, sizeof(mounted->recovery));
  return FAT32_OK;
}

enum fat32_status fat32_mounted_seal(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace)
{
  enum fat32_status status = fat32_mounted_sync(mounted);
  if (status != FAT32_OK || !mounted->dirty_owned || !mounted->allocation_valid ||
      fat32_mirror_pending(&mounted->volume)) {
    return mounted_result(mounted, status);
  }
  for (struct fat32_mounted_object *object = mounted->objects; object; object = object->next) {
    if (!object->attached && object->chain_count) {
      return FAT32_OK;
    }
  }
  struct write_plan plan;
  status = begin_plan(mounted, workspace, &plan, false, false, 0, 0);
  if (status != FAT32_OK) {
    return mounted_result(mounted, status);
  }
  for (unsigned copy = 0; copy < mounted->volume.fat_count; ++copy) {
    uint8_t *bytes;
    status = sector_image(&plan, mounted->volume.fat_first +
        (uint64_t)copy * mounted->volume.fat_sectors, FAT32_MOUNTED_CLEAN, false, &bytes);
    if (status != FAT32_OK) {
      return mounted_result(mounted, status);
    }
    put32(bytes + 4, fat32_get32(bytes + 4) | FAT32_CLEAN);
  }
  return mounted_result(mounted, run_plan(&plan, NULL, NULL, NULL, 0, true));
}
