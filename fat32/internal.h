/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_FAT32_INTERNAL_H
#define PYXIS_FS_FAT32_INTERNAL_H

#include <pyxis_fs/fat32.h>

#define FAT32_MASK UINT32_C(0x0fffffff)
#define FAT32_RESERVED_MIN UINT32_C(0x0ffffff0)
#define FAT32_BAD UINT32_C(0x0ffffff7)
#define FAT32_EOC UINT32_C(0x0ffffff8)
#define FAT32_MIN_CLUSTERS 65525u
#define FAT32_IO_BYTES 4096u
#define FAT32_GPT_ARRAY_LIMIT 65536u
#define FAT32_GPT_ENTRY_LIMIT 256u
#define FAT32_DIRECTORY_ENTRY_BYTES 32u
#define FAT32_ATTR_VOLUME 0x08u
#define FAT32_ATTR_LONG 0x0fu

struct fat32_cursor {
  uint32_t first, cluster, sector;
  uint8_t slot;
  uint64_t position;
  bool done;
};

struct fat32_frame {
  uint32_t first, parent;
  struct fat32_cursor cursor;
};

struct fat32_context {
  const struct fat32_source *source;
  const struct fat32_volume *volume;
  uint64_t steps, sectors;
  uint8_t *seen;
  size_t seen_bytes;
  struct fat32_frame *frames;
  uint32_t frame_limit;
  uint8_t data[FAT32_IO_BYTES];
  uint8_t fat[FAT32_SECTOR_BYTES], mirror[FAT32_SECTOR_BYTES];
  uint16_t long_units[260];
  uint64_t data_sector, fat_sector;
  bool data_cached, fat_cached;
};

uint16_t fat32_get16(const uint8_t *bytes);
uint32_t fat32_get32(const uint8_t *bytes);
uint64_t fat32_get64(const uint8_t *bytes);
void fat32_copy(void *destination, const void *source, size_t length);
void fat32_zero(void *bytes, size_t length);
bool fat32_equal(const void *left, const void *right, size_t length);
bool fat32_all_zero(const void *bytes, size_t length);
uint32_t fat32_crc(const uint8_t *bytes, size_t length);
size_t fat32_context_size(void);
enum fat32_status fat32_context_open(const struct fat32_source *source,
                                    struct fat32_workspace *workspace,
                                    struct fat32_context **context);
enum fat32_status fat32_context_volume(const struct fat32_volume *volume,
                                      struct fat32_workspace *workspace,
                                      struct fat32_context **context);
enum fat32_status fat32_step(struct fat32_context *context);
enum fat32_status fat32_disk_read(struct fat32_context *context, uint64_t first,
                                uint32_t count, void *bytes);
enum fat32_status fat32_partition_read(struct fat32_context *context, uint64_t first,
                                     uint32_t count, void *bytes);
enum fat32_status fat32_gpt_select(struct fat32_context *context, uint32_t entry,
                                 uint8_t *array, struct fat32_volume *volume);
enum fat32_status fat32_geometry(struct fat32_context *context, struct fat32_volume *volume);
enum fat32_status fat32_revalidate(struct fat32_context *context);
bool fat32_valid_cluster(const struct fat32_volume *volume, uint32_t cluster);
enum fat32_status fat32_fat_entry(struct fat32_context *context, uint32_t cluster,
                                uint32_t *value);
enum fat32_status fat32_claim_chain(struct fat32_context *context, uint32_t first,
                                  uint32_t file_bytes, bool directory, uint64_t *clusters);
void fat32_cursor_start(struct fat32_cursor *cursor, uint32_t first);
enum fat32_status fat32_next_entry(struct fat32_context *context,
                                 struct fat32_cursor *cursor, uint32_t parent,
                                 struct fat32_entry *entry, bool *found);
enum fat32_status fat32_validate_directory(struct fat32_context *context, uint32_t first,
                                         uint32_t parent);
enum fat32_status fat32_resolve(struct fat32_context *context, const char *path,
                              size_t path_bytes, struct fat32_entry *entry,
                              uint32_t *parent);
bool fat32_ascii_equal(const char *left, size_t left_bytes,
                       const char *right, size_t right_bytes);
bool fat32_ordinary_entry(const struct fat32_entry *entry);

#endif
