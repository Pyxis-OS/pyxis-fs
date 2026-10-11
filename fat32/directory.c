/* SPDX-License-Identifier: MPL-2.0 */
/* Short-name matching, LFN checksum/ordering and traversal are generalized
 * from pyxis-userland installer/esp_read.c at
 * d2437266d45989c1e73e1ceba995600e4d2f9787 (MPL-2.0). */
#include "internal.h"

#define FAT32_DELETED 0xe5u
#define FAT32_LONG_LAST 0x40u
#define FAT32_LONG_UNITS 13u
#define FAT32_LONG_ENTRIES 20u
#define FAT32_NT_BASE_LOWER 0x08u
#define FAT32_NT_EXT_LOWER 0x10u

static unsigned ascii_upper(unsigned byte)
{
  return byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
}

static unsigned ascii_lower(unsigned byte)
{
  return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

bool fat32_ascii_equal(const char *left, size_t left_bytes,
    const char *right, size_t right_bytes)
{
  if (left_bytes != right_bytes) {
    return false;
  }
  for (size_t i = 0; i < left_bytes; ++i) {
    if (ascii_upper((uint8_t)left[i]) != ascii_upper((uint8_t)right[i])) {
      return false;
    }
  }
  return true;
}

void fat32_cursor_start(struct fat32_cursor *cursor, uint32_t first)
{
  fat32_zero(cursor, sizeof(*cursor));
  cursor->first = first;
  cursor->cluster = first;
}

static enum fat32_status next_slot(struct fat32_context *context,
    struct fat32_cursor *cursor, uint8_t bytes[FAT32_DIRECTORY_ENTRY_BYTES], bool *found)
{
  *found = false;
  if (cursor->done) {
    return FAT32_OK;
  }
  enum fat32_status status = fat32_step(context);
  if (status != FAT32_OK) {
    return status;
  }
  if (!fat32_valid_cluster(context->volume, cursor->cluster)) {
    return FAT32_CORRUPT;
  }
  uint64_t sector = context->volume->data_first +
      (uint64_t)(cursor->cluster - 2) * context->volume->sectors_per_cluster + cursor->sector;
  if (!context->data_cached || context->data_sector != sector) {
    status = fat32_partition_read(context, sector, 1, context->data);
    if (status != FAT32_OK) {
      return status;
    }
    context->data_sector = sector;
    context->data_cached = true;
  }
  fat32_copy(bytes, context->data + (size_t)cursor->slot * FAT32_DIRECTORY_ENTRY_BYTES,
      FAT32_DIRECTORY_ENTRY_BYTES);
  ++cursor->position;
  *found = true;
  if (!bytes[0]) {
    cursor->done = true;
    return FAT32_OK;
  }
  if (++cursor->slot == FAT32_SECTOR_BYTES / FAT32_DIRECTORY_ENTRY_BYTES) {
    cursor->slot = 0;
    if (++cursor->sector == context->volume->sectors_per_cluster) {
      cursor->sector = 0;
      uint32_t next;
      status = fat32_fat_entry(context, cursor->cluster, &next);
      if (status != FAT32_OK) {
        return status;
      }
      if (next >= FAT32_EOC) {
        cursor->done = true;
      } else {
        if (!fat32_valid_cluster(context->volume, next)) {
          return FAT32_CORRUPT;
        }
        cursor->cluster = next;
      }
    }
  }
  return FAT32_OK;
}

static uint8_t alias_checksum(const uint8_t *entry)
{
  uint8_t sum = 0;
  for (unsigned i = 0; i < 11; ++i) {
    sum = ((sum & 1u) << 7) + (sum >> 1) + entry[i];
  }
  return sum;
}

static bool short_character(unsigned byte)
{
  if (byte < 32) {
    return false;
  }
  switch (byte) {
    case '"': case '*': case '+': case ',': case '.': case '/': case ':':
    case ';': case '<': case '=': case '>': case '?': case '[': case '\\':
    case ']': case '|':
      return false;
    default:
      return true;
  }
}

static enum fat32_status decode_alias(const uint8_t *raw, struct fat32_entry *entry)
{
  if (raw[0] == ' ' || (raw[12] & ~(FAT32_NT_BASE_LOWER | FAT32_NT_EXT_LOWER))) {
    return raw[0] == ' ' ? FAT32_CORRUPT : FAT32_UNSUPPORTED;
  }
  entry->alias_ascii = true;
  unsigned base = 8, extension = 3;
  while (base && raw[base - 1] == ' ') {
    --base;
  }
  while (extension && raw[8 + extension - 1] == ' ') {
    --extension;
  }
  if (!base) {
    return FAT32_CORRUPT;
  }
  for (unsigned i = 0; i < 11; ++i) {
    unsigned byte = raw[i];
    if (!i && byte == 0x05) {
      byte = FAT32_DELETED;
    }
    if (!short_character(byte) || (byte >= 'a' && byte <= 'z')) {
      return FAT32_CORRUPT;
    }
    if (byte > 127) {
      entry->alias_ascii = false;
    }
  }
  if (!entry->alias_ascii) {
    return FAT32_OK;
  }
  unsigned offset = 0;
  for (unsigned i = 0; i < base; ++i) {
    entry->alias[offset++] = raw[12] & FAT32_NT_BASE_LOWER ? ascii_lower(raw[i]) : raw[i];
  }
  if (extension) {
    entry->alias[offset++] = '.';
    for (unsigned i = 0; i < extension; ++i) {
      entry->alias[offset++] = raw[12] & FAT32_NT_EXT_LOWER ?
          ascii_lower(raw[8 + i]) : raw[8 + i];
    }
  }
  entry->alias[offset] = 0;
  entry->alias_bytes = (uint8_t)offset;
  return FAT32_OK;
}

static bool long_character(uint32_t point)
{
  if (point < 32) {
    return false;
  }
  switch (point) {
    case '"': case '*': case '/': case ':': case '<': case '>': case '?':
    case '\\': case '|':
      return false;
    default:
      return true;
  }
}

static enum fat32_status decode_long(struct fat32_context *context, unsigned units,
    struct fat32_entry *entry)
{
  unsigned length = 0;
  while (length < units && context->long_units[length]) {
    if (context->long_units[length] == UINT16_MAX) {
      return FAT32_CORRUPT;
    }
    ++length;
  }
  if (!length || length > 255 || (length + FAT32_LONG_UNITS - 1) / FAT32_LONG_UNITS !=
      units / FAT32_LONG_UNITS) {
    return FAT32_CORRUPT;
  }
  if (length < units) {
    for (unsigned i = length + 1; i < units; ++i) {
      if (context->long_units[i] != UINT16_MAX) {
        return FAT32_CORRUPT;
      }
    }
  }
  unsigned bytes = 0;
  entry->name_ascii = true;
  for (unsigned i = 0; i < length; ++i) {
    uint32_t point = context->long_units[i];
    if (point >= 0xd800 && point <= 0xdbff) {
      if (i + 1 >= length || context->long_units[i + 1] < 0xdc00 ||
          context->long_units[i + 1] > 0xdfff) {
        return FAT32_CORRUPT;
      }
      point = UINT32_C(0x10000) + ((point - 0xd800) << 10) +
          (context->long_units[++i] - 0xdc00);
    } else if (point >= 0xdc00 && point <= 0xdfff) {
      return FAT32_CORRUPT;
    }
    if (!long_character(point)) {
      return FAT32_CORRUPT;
    }
    unsigned encoded = point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
    if (bytes + encoded > FAT32_NAME_MAX) {
      return FAT32_NAME_TOO_LONG;
    }
    if (point >= 0x80) {
      entry->name_ascii = false;
    }
    if (encoded == 1) {
      entry->name[bytes++] = (char)point;
    } else {
      entry->name[bytes++] = (char)((encoded == 2 ? 0xc0 : encoded == 3 ? 0xe0 : 0xf0) |
          (point >> (6 * (encoded - 1))));
      for (unsigned remaining = encoded - 1; remaining; --remaining) {
        entry->name[bytes++] = (char)(0x80 | ((point >> (6 * (remaining - 1))) & 0x3f));
      }
    }
  }
  entry->name[bytes] = 0;
  entry->name_bytes = (uint16_t)bytes;
  if ((bytes == 1 && entry->name[0] == '.') ||
      (bytes == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
    return FAT32_CORRUPT;
  }
  return FAT32_OK;
}

static unsigned dot_kind(const uint8_t *raw)
{
  if (raw[0] != '.') {
    return 0;
  }
  unsigned length = raw[1] == '.' ? 2 : 1;
  for (unsigned i = length; i < 11; ++i) {
    if (raw[i] != ' ') {
      return 0;
    }
  }
  return length;
}

enum fat32_status fat32_next_entry(struct fat32_context *context,
    struct fat32_cursor *cursor, uint32_t parent, struct fat32_entry *entry, bool *found)
{
  static const uint8_t offsets[FAT32_LONG_UNITS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
  };
  unsigned remaining = 0, units = 0;
  uint8_t checksum = 0;
  bool active = false;
  *found = false;
  for (;;) {
    uint8_t raw[FAT32_DIRECTORY_ENTRY_BYTES];
    bool have_slot;
    enum fat32_status status = next_slot(context, cursor, raw, &have_slot);
    if (status != FAT32_OK) {
      return status;
    }
    if (!have_slot || !raw[0]) {
      return active || (cursor->first != context->volume->root_cluster && cursor->position <= 2) ?
          FAT32_CORRUPT : FAT32_OK;
    }
    bool require_dot = cursor->first != context->volume->root_cluster && cursor->position <= 2;
    if (raw[0] == FAT32_DELETED) {
      if (active || require_dot) {
        return FAT32_CORRUPT;
      }
      continue;
    }
    if (raw[11] == FAT32_ATTR_LONG) {
      unsigned ordinal = raw[0] & 0x1f;
      if (require_dot || (raw[0] & ~(0x1fu | FAT32_LONG_LAST)) || !ordinal ||
          ordinal > FAT32_LONG_ENTRIES || raw[12] || fat32_get16(raw + 26)) {
        return FAT32_CORRUPT;
      }
      if (raw[0] & FAT32_LONG_LAST) {
        if (active) {
          return FAT32_CORRUPT;
        }
        active = true;
        remaining = ordinal;
        units = ordinal * FAT32_LONG_UNITS;
        checksum = raw[13];
      }
      if (!active || ordinal != remaining || raw[13] != checksum) {
        return FAT32_CORRUPT;
      }
      for (unsigned i = 0; i < FAT32_LONG_UNITS; ++i) {
        context->long_units[(ordinal - 1) * FAT32_LONG_UNITS + i] = fat32_get16(raw + offsets[i]);
      }
      --remaining;
      continue;
    }
    if (active && (remaining || alias_checksum(raw) != checksum)) {
      return FAT32_CORRUPT;
    }
    if (raw[11] & 0xc0) {
      return FAT32_UNSUPPORTED;
    }
    fat32_zero(entry, sizeof(*entry));
    fat32_copy(entry->short_name, raw, 11);
    entry->attributes = raw[11];
    entry->first_cluster = (uint32_t)fat32_get16(raw + 20) << 16 | fat32_get16(raw + 26);
    entry->file_bytes = fat32_get32(raw + 28);
    unsigned dot = dot_kind(raw);
    if (dot) {
      uint32_t expected = dot == 1 ? cursor->first :
          parent == context->volume->root_cluster ? 0 : parent;
      if (active || cursor->first == context->volume->root_cluster ||
          cursor->position != dot || !(raw[11] & FAT32_ATTR_DIRECTORY) ||
          (raw[11] & FAT32_ATTR_VOLUME) || entry->file_bytes || entry->first_cluster != expected) {
        return FAT32_CORRUPT;
      }
      for (unsigned i = 0; i < dot; ++i) {
        entry->name[i] = entry->alias[i] = '.';
      }
      entry->name_bytes = entry->alias_bytes = (uint8_t)dot;
      entry->name_ascii = entry->alias_ascii = true;
    } else if (raw[11] & FAT32_ATTR_VOLUME) {
      if (active || require_dot || cursor->first != context->volume->root_cluster ||
          (raw[11] & FAT32_ATTR_DIRECTORY) || entry->first_cluster || entry->file_bytes) {
        return FAT32_CORRUPT;
      }
    } else {
      if (require_dot || ((raw[11] & FAT32_ATTR_DIRECTORY) && entry->file_bytes) ||
          ((raw[11] & FAT32_ATTR_DIRECTORY) || entry->file_bytes ?
           !fat32_valid_cluster(context->volume, entry->first_cluster) : entry->first_cluster != 0)) {
        return FAT32_CORRUPT;
      }
      status = decode_alias(raw, entry);
      if (status != FAT32_OK) {
        return status;
      }
      entry->has_long_name = active;
      if (active) {
        status = decode_long(context, units, entry);
        if (status != FAT32_OK) {
          return status;
        }
      } else {
        if (!entry->alias_ascii) {
          return FAT32_UNSUPPORTED;
        }
        fat32_copy(entry->name, entry->alias, entry->alias_bytes + 1);
        entry->name_bytes = entry->alias_bytes;
        entry->name_ascii = true;
      }
    }
    *found = true;
    return FAT32_OK;
  }
}

bool fat32_ordinary_entry(const struct fat32_entry *entry)
{
  return !(entry->attributes & FAT32_ATTR_VOLUME) &&
      !(entry->name_bytes == 1 && entry->name[0] == '.') &&
      !(entry->name_bytes == 2 && entry->name[0] == '.' && entry->name[1] == '.');
}

static bool name_collision(const struct fat32_entry *a, const struct fat32_entry *b)
{
  if (fat32_equal(a->short_name, b->short_name, sizeof(a->short_name)) ||
      (a->name_bytes == b->name_bytes && fat32_equal(a->name, b->name, a->name_bytes))) {
    return true;
  }
  if (a->name_ascii && b->name_ascii &&
      fat32_ascii_equal(a->name, a->name_bytes, b->name, b->name_bytes)) {
    return true;
  }
  if (a->alias_ascii && b->alias_ascii &&
      fat32_ascii_equal(a->alias, a->alias_bytes, b->alias, b->alias_bytes)) {
    return true;
  }
  if (a->name_ascii && b->alias_ascii &&
      fat32_ascii_equal(a->name, a->name_bytes, b->alias, b->alias_bytes)) {
    return true;
  }
  return a->alias_ascii && b->name_ascii &&
      fat32_ascii_equal(a->alias, a->alias_bytes, b->name, b->name_bytes);
}

enum fat32_status fat32_validate_directory(struct fat32_context *context, uint32_t first,
    uint32_t parent)
{
  struct fat32_cursor cursor;
  fat32_cursor_start(&cursor, first);
  unsigned labels = 0;
  for (;;) {
    struct fat32_entry entry;
    bool found;
    enum fat32_status status = fat32_next_entry(context, &cursor, parent, &entry, &found);
    if (status != FAT32_OK || !found) {
      return status;
    }
    if (entry.attributes & FAT32_ATTR_VOLUME) {
      if (++labels > 1) {
        return FAT32_CORRUPT;
      }
    }
    if (!fat32_ordinary_entry(&entry)) {
      continue;
    }
    struct fat32_cursor previous;
    fat32_cursor_start(&previous, first);
    while (previous.position < cursor.position) {
      struct fat32_entry other;
      status = fat32_next_entry(context, &previous, parent, &other, &found);
      if (status != FAT32_OK) {
        return status;
      }
      if (!found || previous.position == cursor.position) {
        break;
      }
      status = fat32_step(context);
      if (status != FAT32_OK) {
        return status;
      }
      if (fat32_ordinary_entry(&other) && name_collision(&entry, &other)) {
        return FAT32_CORRUPT;
      }
    }
  }
}

static enum fat32_status validate_path(struct fat32_context *context,
    const char *path, size_t bytes)
{
  if (!path || !bytes || bytes == SIZE_MAX || path[0] != '/') {
    return FAT32_INVALID;
  }
  size_t component = 1;
  for (size_t i = 1; i <= bytes; ++i) {
    enum fat32_status status = fat32_step(context);
    if (status != FAT32_OK) {
      return status;
    }
    if (i - component > FAT32_NAME_MAX) {
      return FAT32_NAME_TOO_LONG;
    }
    if (i < bytes && (uint8_t)path[i] >= 128) {
      return FAT32_UNSUPPORTED;
    }
    if (i < bytes && (uint8_t)path[i] < 32) {
      return FAT32_INVALID;
    }
    if (i == bytes || path[i] == '/') {
      size_t length = i - component;
      if (!length) {
        return bytes == 1 ? FAT32_OK : FAT32_INVALID;
      }
      if (length > FAT32_NAME_MAX) {
        return FAT32_NAME_TOO_LONG;
      }
      if ((length == 1 && path[component] == '.') ||
          (length == 2 && path[component] == '.' && path[component + 1] == '.')) {
        return FAT32_INVALID;
      }
      component = i + 1;
    }
  }
  return FAT32_OK;
}

enum fat32_status fat32_resolve(struct fat32_context *context, const char *path,
    size_t path_bytes, struct fat32_entry *entry, uint32_t *parent)
{
  enum fat32_status status = validate_path(context, path, path_bytes);
  if (status != FAT32_OK) {
    return status;
  }
  if (!context->frame_limit) {
    return FAT32_LIMIT;
  }
  fat32_zero(entry, sizeof(*entry));
  entry->attributes = FAT32_ATTR_DIRECTORY;
  entry->first_cluster = context->volume->root_cluster;
  *parent = 0;
  uint64_t clusters;
  status = fat32_claim_chain(context, entry->first_cluster, 0, true, &clusters);
  if (status != FAT32_OK) {
    return status;
  }
  size_t component = 1;
  uint32_t depth = 1;
  for (;;) {
    status = fat32_validate_directory(context, entry->first_cluster, *parent);
    if (status != FAT32_OK || component == path_bytes) {
      return status;
    }
    size_t end = component;
    while (end < path_bytes && path[end] != '/') {
      ++end;
    }
    struct fat32_cursor cursor;
    fat32_cursor_start(&cursor, entry->first_cluster);
    bool matched = false;
    struct fat32_entry selected;
    for (;;) {
      struct fat32_entry candidate;
      bool found;
      status = fat32_next_entry(context, &cursor, *parent, &candidate, &found);
      if (status != FAT32_OK) {
        return status;
      }
      if (!found) {
        break;
      }
      if (fat32_ordinary_entry(&candidate) &&
          ((candidate.name_ascii && fat32_ascii_equal(candidate.name, candidate.name_bytes,
              path + component, end - component)) ||
           (candidate.alias_ascii && fat32_ascii_equal(candidate.alias, candidate.alias_bytes,
              path + component, end - component)))) {
        if (matched) {
          return FAT32_CORRUPT;
        }
        fat32_copy(&selected, &candidate, sizeof(selected));
        matched = true;
      }
    }
    if (!matched) {
      return FAT32_NOT_FOUND;
    }
    uint32_t directory = entry->first_cluster;
    bool selected_directory = (selected.attributes & FAT32_ATTR_DIRECTORY) != 0;
    if (end != path_bytes && !selected_directory) {
      return FAT32_NOT_DIRECTORY;
    }
    if (selected_directory && ++depth > context->frame_limit) {
      return FAT32_LIMIT;
    }
    status = fat32_claim_chain(context, selected.first_cluster, selected.file_bytes,
        selected_directory, &clusters);
    if (status != FAT32_OK) {
      return status;
    }
    fat32_copy(entry, &selected, sizeof(selected));
    *parent = directory;
    if (end == path_bytes) {
      return selected_directory ? fat32_validate_directory(context, entry->first_cluster, *parent) :
          FAT32_OK;
    }
    component = end + 1;
  }
}
