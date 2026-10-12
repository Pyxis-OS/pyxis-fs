/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <pyxis_fs/fat32_write.h>

#define ENTRY_DELETED 0xe5u
#define ENTRY_ARCHIVE 0x20u
#define ENTRY_LONG_LAST 0x40u
#define DOS_FIRST_DATE 0x0021u

struct write_plan {
  struct fat32_writer *writer;
  struct fat32_context *context;
  struct fat32_prepared_sector *sectors;
  size_t count, capacity;
  uint32_t *free_clusters, free_count, allocated;
  struct fat32_check_report check;
  uint8_t entry_count, entry_slots[21], entries[21][32];
  uint64_t entry_sectors[21];
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

static enum fat32_status plan_capacity(const struct fat32_volume *volume,
    uint32_t max_file_bytes, size_t *capacity)
{
  if (!volume->sectors_per_cluster || volume->sectors_per_cluster > 128 ||
      (volume->sectors_per_cluster & (volume->sectors_per_cluster - 1)) ||
      (volume->fat_count != 1 && volume->fat_count != 2) || !volume->fat_sectors) {
    return FAT32_INVALID;
  }
  uint64_t count = (uint64_t)volume->fat_sectors * volume->fat_count * 2 +
      (uint64_t)clusters_for(volume, max_file_bytes) * volume->sectors_per_cluster +
      (uint64_t)volume->sectors_per_cluster * 3 + 32;
  if (count > SIZE_MAX / sizeof(struct fat32_prepared_sector)) {
    return FAT32_LIMIT;
  }
  *capacity = (size_t)count;
  return FAT32_OK;
}

enum fat32_status fat32_write_workspace_size(const struct fat32_volume *volume,
    uint32_t depth, uint32_t max_file_bytes, size_t *bytes)
{
  size_t reader, capacity;
  if (!bytes) {
    return FAT32_INVALID;
  }
  enum fat32_status status = fat32_workspace_size(volume, depth, &reader);
  if (status != FAT32_OK) {
    return status;
  }
  status = plan_capacity(volume, max_file_bytes, &capacity);
  if (status != FAT32_OK) {
    return status;
  }
  uint64_t allocation = ((uint64_t)volume->cluster_count + 2) * sizeof(uint32_t);
  uint64_t total = (uint64_t)aligned(reader) + aligned((size_t)allocation) +
      (uint64_t)capacity * sizeof(struct fat32_prepared_sector);
  if (allocation > SIZE_MAX || total > SIZE_MAX) {
    return FAT32_LIMIT;
  }
  *bytes = (size_t)total;
  return FAT32_OK;
}

enum fat32_status fat32_writer_open(struct fat32_writer *writer,
    const struct fat32_volume *volume, const struct fat32_write_io *io,
    const struct fat32_workspace *workspace, uint32_t max_file_bytes,
    struct fat32_receipt *receipts, uint32_t receipt_count)
{
  if (!writer || !volume || !io || !io->write || !io->flush || !workspace ||
      !workspace->memory || (uintptr_t)workspace->memory % _Alignof(max_align_t) ||
      (!receipts && receipt_count)) {
    return FAT32_INVALID;
  }
  size_t required;
  enum fat32_status status = fat32_write_workspace_size(volume,
      workspace->directory_depth_limit, max_file_bytes, &required);
  if (status != FAT32_OK) {
    return status;
  }
  if (workspace->bytes < required) {
    return FAT32_NO_MEMORY;
  }
  if (!volume->clean_shutdown || !volume->no_hard_error) {
    return FAT32_CORRUPT;
  }
  for (uint32_t i = 0; i < receipt_count; ++i) {
    if (receipts[i].chain_capacity && !receipts[i].chain) {
      return FAT32_INVALID;
    }
  }
  fat32_zero(writer, sizeof(*writer));
  writer->volume = *volume;
  writer->io = *io;
  writer->workspace = *workspace;
  writer->max_file_bytes = max_file_bytes;
  writer->receipts = receipts;
  writer->receipt_count = receipt_count;
  for (uint32_t i = 0; i < receipt_count; ++i) {
    receipts[i].valid = false;
    receipts[i].session = NULL;
    receipts[i].chain_count = 0;
  }
  return FAT32_OK;
}

static enum fat32_status begin_plan(struct fat32_writer *writer, struct write_plan *plan)
{
  if (!writer) {
    return FAT32_INVALID;
  }
  if (writer->stopped) {
    return FAT32_STOPPED;
  }
  fat32_zero(plan, sizeof(*plan));
  plan->writer = writer;
  enum fat32_status status = fat32_context_volume(&writer->volume,
      &writer->workspace, &plan->context);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat32_check_context(plan->context, &plan->check);
  if (status != FAT32_OK) {
    return status;
  }
  size_t reader;
  status = fat32_workspace_size(&writer->volume,
      writer->workspace.directory_depth_limit, &reader);
  if (status != FAT32_OK) {
    return status;
  }
  status = plan_capacity(&writer->volume, writer->max_file_bytes, &plan->capacity);
  if (status != FAT32_OK) {
    return status;
  }
  plan->free_clusters = (void *)((uint8_t *)writer->workspace.memory + aligned(reader));
  plan->sectors = (void *)((uint8_t *)plan->free_clusters +
      aligned(((size_t)writer->volume.cluster_count + 2) * sizeof(uint32_t)));
  for (uint32_t cluster = 2; cluster - 2 < writer->volume.cluster_count; ++cluster) {
    uint32_t value;
    status = fat32_fat_entry(plan->context, cluster, &value);
    if (status != FAT32_OK) {
      return status;
    }
    if (!value) {
      plan->free_clusters[plan->free_count++] = cluster;
    }
  }
  fat32_zero(plan->context->seen, plan->context->seen_bytes);
  return FAT32_OK;
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
    enum fat32_status status = fat32_partition_read(plan->context, sector, 1, record->bytes);
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
  const struct fat32_volume *volume = &plan->writer->volume;
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
  if (count > plan->free_count - plan->allocated) {
    return FAT32_NO_SPACE;
  }
  *chain = plan->free_clusters + plan->allocated;
  plan->allocated += count;
  for (uint32_t i = 0; i < count; ++i) {
    enum fat32_status status = fat_set(plan, (*chain)[i],
        i + 1 < count ? (*chain)[i + 1] : FAT32_MASK, FAT32_WRITE_FAT);
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
    for (unsigned sector = 0; sector < plan->writer->volume.sectors_per_cluster; ++sector) {
      uint8_t *bytes;
      enum fat32_status status = sector_image(plan,
          cluster_sector(&plan->writer->volume, chain[i]) + sector, FAT32_WRITE_DATA, true, &bytes);
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
  const struct fat32_volume *volume = &plan->writer->volume;
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
    enum fat32_status status = sector_image(plan, sector, FAT32_WRITE_DATA, false, &bytes);
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
    status = sector_image(plan, info + (i ? backup : 0), FAT32_WRITE_FSINFO, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    put32(bytes + 488, (uint32_t)plan->check.free_clusters - plan->allocated + reclaimed);
    put32(bytes + 492, UINT32_MAX);
  }
  return FAT32_OK;
}

static enum fat32_status run_plan(struct write_plan *plan, const char *target,
    uint32_t old_first, const uint32_t *stage, uint32_t stage_count)
{
  struct fat32_writer *writer = plan->writer;
  uint64_t writes = 0;
  for (size_t i = 0; i < plan->count; ++i) {
    if (plan->sectors[i].phase) {
      ++writes;
    }
  }
  /* Every effect and barrier is reserved; execution neither reads nor allocates. */
  if (writes > plan->context->sectors || writes + FAT32_WRITE_FSINFO > plan->context->steps) {
    return FAT32_LIMIT;
  }
  fat32_zero(&writer->recovery, sizeof(writer->recovery));
  size_t length = 0;
  while (target[length] && length < sizeof(writer->recovery.target) - 1) {
    writer->recovery.target[length] = target[length];
    ++length;
  }
  writer->recovery.old_first = old_first;
  writer->recovery.staged_first = stage_count ? stage[0] : 0;
  writer->recovery.staged_clusters = stage_count;
  writer->recovery.staged_chain = stage;
  writer->recovery.allocated_chain = plan->free_clusters;
  writer->recovery.allocated_clusters = plan->allocated;
  writer->recovery.prepared_sectors = plan->sectors;
  writer->recovery.prepared_sector_count = plan->count;
  for (uint8_t phase = 1; phase <= FAT32_WRITE_FSINFO; ++phase) {
    writer->recovery.phase = phase;
    for (size_t i = 0; i < plan->count; ++i) {
      struct fat32_prepared_sector *record = &plan->sectors[i];
      if (record->phase != phase) {
        continue;
      }
      writer->recovery.failed_sector = writer->volume.partition_first + record->sector;
      if (!writer->io.write(writer->io.context, writer->recovery.failed_sector, 1,
          record->bytes)) {
        goto uncertain;
      }
    }
    writer->recovery.failed_sector = UINT64_MAX;
    if (!writer->io.flush(writer->io.context)) {
      goto uncertain;
    }
  }
  fat32_zero(&writer->recovery, sizeof(writer->recovery));
  return FAT32_OK;
uncertain:
  writer->stopped = true;
  writer->recovery.uncertain = true;
  writer->recovery.failure = FAT32_IO;
  return FAT32_IO;
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

static enum fat32_status siblings(struct write_plan *plan, uint32_t directory,
    uint32_t parent, const char *name, size_t length, const char *alias, size_t alias_bytes,
    bool *collision)
{
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, directory);
  *collision = false;
  for (;;) {
    struct fat32_entry entry;
    bool found;
    enum fat32_status status = fat32_next_entry(plan->context, &cursor, parent, &entry, &found);
    if (status != FAT32_OK || !found) {
      return status;
    }
    if (!fat32_ordinary_entry(&entry)) {
      continue;
    }
    if (!entry.name_ascii || !entry.alias_ascii) {
      return FAT32_UNSUPPORTED;
    }
    if (fat32_ascii_equal(entry.name, entry.name_bytes, name, length) ||
        fat32_ascii_equal(entry.alias, entry.alias_bytes, name, length) ||
        (alias && (fat32_ascii_equal(entry.name, entry.name_bytes, alias, alias_bytes) ||
        fat32_ascii_equal(entry.alias, entry.alias_bytes, alias, alias_bytes)))) {
      *collision = true;
    }
  }
}

static enum fat32_status choose_alias(struct write_plan *plan, uint32_t directory,
    uint32_t parent, const char *name, size_t length, uint8_t alias[11])
{
  bool collision;
  enum fat32_status status = siblings(plan, directory, parent, name, length, NULL, 0, &collision);
  if (status != FAT32_OK) {
    return status;
  }
  if (collision) {
    return FAT32_EXISTS;
  }
  static const char digits[] = "0123456789ABCDEF";
  for (uint32_t number = 1; number < UINT32_C(0x10000000); ++number) {
    alias[0] = 'P';
    for (unsigned i = 0; i < 7; ++i) {
      alias[7 - i] = (uint8_t)digits[(number >> (i * 4)) & 15];
    }
    alias[8] = alias[9] = alias[10] = ' ';
    status = siblings(plan, directory, parent, name, length, (const char *)alias, 8, &collision);
    if (status != FAT32_OK || !collision) {
      return status;
    }
  }
  return FAT32_LIMIT;
}

static enum fat32_status find_slots(struct write_plan *plan, uint32_t directory)
{
  const struct fat32_volume *volume = &plan->writer->volume;
  uint32_t cluster = directory;
  unsigned run = 0;
  /* Reserve a spare slot too, retaining the end marker where one existed. */
  unsigned needed = plan->entry_count + 1;
  for (;;) {
    for (unsigned sector = 0; sector < volume->sectors_per_cluster; ++sector) {
      enum fat32_status status = fat32_partition_read(plan->context,
          cluster_sector(volume, cluster) + sector, 1, plan->context->data);
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
        if (first == 0 || first == ENTRY_DELETED) {
          if (run < plan->entry_count) {
            plan->entry_sectors[run] = cluster_sector(volume, cluster) + sector;
            plan->entry_slots[run] = (uint8_t)slot;
          }
          if (++run == needed) {
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
  uint32_t *chain;
  enum fat32_status status = allocate_chain(plan, count, &chain);
  if (status != FAT32_OK) {
    return status;
  }
  status = zero_chain(plan, chain, count);
  if (status != FAT32_OK) {
    return status;
  }
  status = fat_set(plan, cluster, chain[0], FAT32_WRITE_FAT);
  if (status != FAT32_OK) {
    return status;
  }
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
        FAT32_WRITE_DIRECTORY, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    fat32_copy(bytes + plan->entry_slots[i] * 32, plan->entries[i], 32);
  }
  return FAT32_OK;
}

static void issue_receipt(struct fat32_receipt *receipt, const struct write_plan *plan,
    const char *path, size_t path_bytes, uint32_t parent, bool directory,
    const uint32_t *chain, uint32_t chain_count, uint32_t bytes)
{
  receipt->session = plan->writer;
  receipt->parent = parent;
  receipt->directory = directory;
  receipt->chain_count = chain_count;
  receipt->file_bytes = bytes;
  receipt->entry_count = plan->entry_count;
  for (unsigned i = 0; i < plan->entry_count; ++i) {
    fat32_copy(receipt->entries[i], plan->entries[i], 32);
    receipt->entry_sectors[i] = plan->entry_sectors[i];
    receipt->entry_slots[i] = plan->entry_slots[i];
  }
  receipt->entry_sector = plan->entry_sectors[plan->entry_count - 1];
  receipt->entry_slot = plan->entry_slots[plan->entry_count - 1];
  for (uint32_t i = 0; i < chain_count; ++i) {
    receipt->chain[i] = chain[i];
  }
  fat32_copy(receipt->path, path, path_bytes);
  receipt->path[path_bytes] = 0;
  receipt->valid = true;
}

enum fat32_status fat32_create(struct fat32_writer *writer, const char *path,
    size_t path_bytes, bool directory, fat32_payload_fn payload, void *context,
    uint32_t bytes, struct fat32_receipt **receipt)
{
  if (!writer || !path || !receipt || path_bytes < 2 || path_bytes >= 1025 ||
      path[0] != '/' || (directory && bytes) || (bytes && !payload)) {
    return FAT32_INVALID;
  }
  if (writer->stopped) {
    return FAT32_STOPPED;
  }
  if (bytes > writer->max_file_bytes) {
    return FAT32_LIMIT;
  }
  size_t component = path_bytes;
  while (component && path[component - 1] != '/') {
    --component;
  }
  size_t length = path_bytes - component;
  if (!length || length > FAT32_NAME_MAX ||
      (length == 1 && path[component] == '.') ||
      (length == 2 && path[component] == '.' && path[component + 1] == '.') ||
      path[path_bytes - 1] == ' ' || path[path_bytes - 1] == '.') {
    return FAT32_INVALID;
  }
  for (size_t i = component; i < path_bytes; ++i) {
    uint8_t byte = (uint8_t)path[i];
    if (byte >= 128) {
      return FAT32_UNSUPPORTED;
    }
    if (byte < 32 || byte == 127 || byte == '"' || byte == '*' || byte == ':' ||
        byte == '<' || byte == '>' || byte == '?' || byte == '\\' || byte == '|') {
      return FAT32_INVALID;
    }
  }
  uint32_t chain_count = directory ? 1 : clusters_for(&writer->volume, bytes);
  struct fat32_receipt *selected = NULL;
  for (uint32_t i = 0; i < writer->receipt_count; ++i) {
    if (!writer->receipts[i].valid && chain_count <= writer->receipts[i].chain_capacity) {
      selected = &writer->receipts[i];
      break;
    }
  }
  if (!selected) {
    return FAT32_NO_SPACE;
  }
  struct write_plan plan;
  enum fat32_status status = begin_plan(writer, &plan);
  if (status != FAT32_OK) {
    return status;
  }
  struct fat32_entry parent_entry;
  uint32_t grandparent;
  status = fat32_resolve(plan.context, path, component == 1 ? 1 : component - 1,
      &parent_entry, &grandparent);
  if (status != FAT32_OK) {
    return status;
  }
  if (!(parent_entry.attributes & FAT32_ATTR_DIRECTORY)) {
    return FAT32_NOT_DIRECTORY;
  }
  if (component != 1 && (!parent_entry.name_ascii || !parent_entry.alias_ascii)) {
    return FAT32_UNSUPPORTED;
  }
  uint32_t parent = parent_entry.first_cluster;
  uint8_t alias[11];
  status = choose_alias(&plan, parent, grandparent, path + component, length, alias);
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t *chain;
  status = allocate_chain(&plan, chain_count, &chain);
  if (status == FAT32_OK) {
    status = zero_chain(&plan, chain, chain_count);
  }
  if (status != FAT32_OK) {
    return status;
  }
  if (directory) {
    uint8_t *data;
    status = sector_image(&plan, cluster_sector(&writer->volume, chain[0]), FAT32_WRITE_DATA,
        false, &data);
    if (status != FAT32_OK) {
      return status;
    }
    uint8_t dots[11];
    for (unsigned i = 0; i < 11; ++i) {
      dots[i] = ' ';
    }
    dots[0] = '.';
    short_entry(data, dots, true, chain[0], 0);
    dots[1] = '.';
    short_entry(data + 32, dots, true,
        parent == writer->volume.root_cluster ? 0 : parent, 0);
  } else {
    status = content(&plan, NULL, 0, chain, chain_count, 0, bytes, payload, context);
    if (status != FAT32_OK) {
      return status;
    }
  }
  long_entries(&plan, path + component, length, alias, directory,
      chain_count ? chain[0] : 0, bytes);
  status = find_slots(&plan, parent);
  if (status == FAT32_OK) {
    status = publish_entries(&plan);
  }
  if (status == FAT32_OK) {
    status = fsinfo_plan(&plan, 0);
  }
  if (status != FAT32_OK) {
    return status;
  }
  char target[1025];
  fat32_copy(target, path, path_bytes);
  target[path_bytes] = 0;
  status = run_plan(&plan, target, 0, chain, chain_count);
  if (status == FAT32_OK) {
    issue_receipt(selected, &plan, path, path_bytes, parent, directory, chain, chain_count, bytes);
    *receipt = selected;
  }
  return status;
}

static enum fat32_status validate_receipt(struct write_plan *plan,
    struct fat32_receipt *receipt)
{
  struct fat32_writer *writer = plan->writer;
  bool registered = false;
  for (uint32_t i = 0; i < writer->receipt_count; ++i) {
    if (&writer->receipts[i] == receipt) {
      registered = true;
      break;
    }
  }
  if (!registered || !receipt->valid || receipt->session != writer) {
    return FAT32_STALE;
  }
  if (receipt->directory) {
    return FAT32_IS_DIRECTORY;
  }
  struct fat32_entry current;
  uint32_t parent;
  size_t path_bytes = 0;
  while (path_bytes < sizeof(receipt->path) && receipt->path[path_bytes]) {
    ++path_bytes;
  }
  enum fat32_status status = fat32_resolve(plan->context, receipt->path, path_bytes,
      &current, &parent);
  if (status != FAT32_OK) {
    return status == FAT32_NOT_FOUND ? FAT32_STALE : status;
  }
  if (!current.name_ascii || !current.alias_ascii || parent != receipt->parent ||
      current.file_bytes != receipt->file_bytes ||
      current.first_cluster != (receipt->chain_count ? receipt->chain[0] : 0)) {
    return FAT32_STALE;
  }
  for (unsigned i = 0; i < receipt->entry_count; ++i) {
    uint8_t *bytes;
    status = sector_image(plan, receipt->entry_sectors[i], 0, false, &bytes);
    if (status != FAT32_OK) {
      return status;
    }
    if (!fat32_equal(bytes + receipt->entry_slots[i] * 32, receipt->entries[i], 32)) {
      return FAT32_STALE;
    }
  }
  if (receipt->chain_count != clusters_for(&writer->volume, receipt->file_bytes)) {
    return FAT32_STALE;
  }
  for (uint32_t i = 0; i < receipt->chain_count; ++i) {
    uint32_t next;
    status = fat32_fat_entry(plan->context, receipt->chain[i], &next);
    if (status != FAT32_OK) {
      return status;
    }
    if ((i + 1 < receipt->chain_count && next != receipt->chain[i + 1]) ||
        (i + 1 == receipt->chain_count && next < FAT32_EOC)) {
      return FAT32_STALE;
    }
  }
  return FAT32_OK;
}

static enum fat32_status update(struct fat32_writer *writer, struct fat32_receipt *receipt,
    bool replace, fat32_payload_fn payload, void *context, uint32_t bytes)
{
  if (!writer || !receipt) {
    return FAT32_INVALID;
  }
  if (writer->stopped) {
    return FAT32_STOPPED;
  }
  struct write_plan plan;
  enum fat32_status status = begin_plan(writer, &plan);
  if (status != FAT32_OK) {
    return status;
  }
  status = validate_receipt(&plan, receipt);
  if (status != FAT32_OK) {
    return status;
  }
  uint64_t final = replace ? bytes : (uint64_t)receipt->file_bytes + bytes;
  if (final > UINT32_MAX || final > writer->max_file_bytes) {
    return FAT32_LIMIT;
  }
  uint32_t total = clusters_for(&writer->volume, (uint32_t)final);
  if (total > receipt->chain_capacity) {
    return FAT32_NO_MEMORY;
  }
  uint32_t count = replace ? total : total - receipt->chain_count;
  uint32_t *chain;
  status = allocate_chain(&plan, count, &chain);
  if (status == FAT32_OK) {
    status = zero_chain(&plan, chain, count);
  }
  if (status != FAT32_OK) {
    return status;
  }
  if (!replace && count && receipt->chain_count) {
    status = fat_set(&plan, receipt->chain[receipt->chain_count - 1], chain[0], FAT32_WRITE_FAT);
    if (status != FAT32_OK) {
      return status;
    }
  }
  status = content(&plan, replace ? NULL : receipt->chain,
      replace ? 0 : receipt->chain_count, chain, count, replace ? 0 : receipt->file_bytes,
      bytes, payload, context);
  if (status != FAT32_OK) {
    return status;
  }
  plan.entry_count = receipt->entry_count;
  for (unsigned i = 0; i < plan.entry_count; ++i) {
    fat32_copy(plan.entries[i], receipt->entries[i], 32);
    plan.entry_sectors[i] = receipt->entry_sectors[i];
    plan.entry_slots[i] = receipt->entry_slots[i];
  }
  uint8_t *entry = plan.entries[plan.entry_count - 1];
  uint32_t first = replace ? (count ? chain[0] : 0) :
      receipt->chain_count ? receipt->chain[0] : count ? chain[0] : 0;
  put16(entry + 20, (uint16_t)(first >> 16));
  put16(entry + 26, (uint16_t)first);
  put32(entry + 28, (uint32_t)final);
  status = publish_entries(&plan);
  if (status != FAT32_OK) {
    return status;
  }
  if (replace) {
    for (uint32_t i = 0; i < receipt->chain_count; ++i) {
      status = fat_set(&plan, receipt->chain[i], 0, FAT32_WRITE_RECLAIM);
      if (status != FAT32_OK) {
        return status;
      }
    }
  }
  status = fsinfo_plan(&plan, replace ? receipt->chain_count : 0);
  if (status != FAT32_OK) {
    return status;
  }
  status = run_plan(&plan, receipt->path, receipt->chain_count ? receipt->chain[0] : 0,
      chain, count);
  if (status != FAT32_OK) {
    return status;
  }
  uint32_t old_count = replace ? 0 : receipt->chain_count;
  for (uint32_t i = 0; i < count; ++i) {
    receipt->chain[old_count + i] = chain[i];
  }
  receipt->chain_count = total;
  receipt->file_bytes = (uint32_t)final;
  fat32_copy(receipt->entries[receipt->entry_count - 1], entry, 32);
  return FAT32_OK;
}

enum fat32_status fat32_append(struct fat32_writer *writer,
    struct fat32_receipt *receipt, fat32_payload_fn payload, void *context, uint32_t bytes)
{
  if (bytes && !payload) {
    return FAT32_INVALID;
  }
  return update(writer, receipt, false, payload, context, bytes);
}

enum fat32_status fat32_extend(struct fat32_writer *writer,
    struct fat32_receipt *receipt, uint32_t bytes)
{
  if (!receipt || bytes < receipt->file_bytes) {
    return FAT32_INVALID;
  }
  return update(writer, receipt, false, NULL, NULL, bytes - receipt->file_bytes);
}

enum fat32_status fat32_replace(struct fat32_writer *writer,
    struct fat32_receipt *receipt, fat32_payload_fn payload, void *context, uint32_t bytes)
{
  if (bytes && !payload) {
    return FAT32_INVALID;
  }
  return update(writer, receipt, true, payload, context, bytes);
}

const char *fat32_status_string(enum fat32_status status)
{
  switch (status) {
  case FAT32_OK: return "ok";
  case FAT32_INVALID: return "invalid argument or path";
  case FAT32_CORRUPT: return "corrupt or dirty metadata";
  case FAT32_UNSUPPORTED: return "unsupported format, geometry or name";
  case FAT32_IO: return "backing I/O or flush failed";
  case FAT32_NO_MEMORY: return "insufficient workspace or receipt capacity";
  case FAT32_LIMIT: return "work, depth or file-size budget exhausted";
  case FAT32_NAME_TOO_LONG: return "name too long";
  case FAT32_NOT_FOUND: return "path not found";
  case FAT32_NOT_DIRECTORY: return "not a directory";
  case FAT32_IS_DIRECTORY: return "not a regular file";
  case FAT32_OUTPUT: return "output failed";
  case FAT32_EXISTS: return "name or alias collision";
  case FAT32_NO_SPACE: return "volume or receipt storage full";
  case FAT32_STALE: return "creation receipt no longer matches";
  case FAT32_STOPPED: return "writer permanently stopped after uncertain effects";
  case FAT32_INPUT: return "payload input failed";
  case FAT32_READ_ONLY: return "read-only regular file attribute";
  case FAT32_NOT_EMPTY: return "directory is not empty";
  }
  return "unknown status";
}
