/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_FAT32_WRITE_H
#define PYXIS_FS_FAT32_WRITE_H

#include <pyxis_fs/fat32.h>

/* Complete writes only; false may mean partial effects. Addresses are absolute
 * source sectors. The caller excludes every competing writer for the session. */
typedef bool (*fat32_write_sectors_fn)(void *context, uint64_t first_sector,
                                     uint32_t sector_count, const void *bytes);
typedef bool (*fat32_flush_fn)(void *context);
/* Input is copied entirely during preflight; no callback runs after effects. */
typedef bool (*fat32_payload_fn)(void *context, uint32_t offset, void *bytes,
                               size_t length);
struct fat32_write_io {
  fat32_write_sectors_fn write;
  fat32_flush_fn flush;
  void *context;
  uint32_t max_write_sectors; /* Nonzero callback bound; mounted batches also fit 4 KiB. */
};

enum fat32_write_phase {
  FAT32_WRITE_PREFLIGHT,
  FAT32_WRITE_DATA,
  FAT32_WRITE_FAT,
  FAT32_WRITE_DIRECTORY,
  FAT32_WRITE_RECLAIM,
  FAT32_WRITE_FSINFO,
};
struct fat32_prepared_sector {
  uint64_t sector; /* relative to the admitted partition */
  uint8_t phase;
  uint8_t *bytes; /* source.sector_bytes bytes retained in operation workspace */
};

struct fat32_writer;
/* Caller-owned, disjoint chain arrays and receipts live until the session ends.
 * Library-maintained fields must not be edited/copied. No persistent adoption. */
struct fat32_receipt {
  uint32_t *chain;
  uint32_t chain_capacity, chain_count;
  const struct fat32_writer *session;
  uint32_t parent, file_bytes;
  uint64_t entry_sector;
  uint8_t entry_slot, entry_count;
  uint8_t entries[21][32];
  uint64_t entry_sectors[21];
  uint8_t entry_slots[21];
  char path[1025];
  bool valid, directory;
};

/* After uncertain effects, workspace retains the prepared sector images and
 * recovery retains the target plus old/new chains. Do not reuse either storage
 * until the caller has captured it for external repair. No rollback or replay. */
struct fat32_recovery {
  enum fat32_status failure;
  char target[1025];
  uint32_t old_first, staged_first, staged_clusters;
  const uint32_t *staged_chain, *allocated_chain;
  uint32_t allocated_clusters;
  const struct fat32_prepared_sector *prepared_sectors;
  size_t prepared_sector_count;
  uint64_t failed_sector;
  uint8_t phase;
  bool uncertain;
};
struct fat32_writer {
  struct fat32_volume volume;
  struct fat32_write_io io;
  struct fat32_workspace workspace;
  struct fat32_receipt *receipts;
  uint32_t receipt_count, max_file_bytes;
  bool stopped;
  struct fat32_recovery recovery;
};

/* Operation workspace includes reader checks, complete prepared sector images,
 * allocation storage and up to max_file_bytes of new content (zero extension
 * included). Zero budgets remain zero. Session operations are serialized. */
enum fat32_status fat32_write_workspace_size(const struct fat32_volume *volume,
    uint32_t depth, uint32_t max_file_bytes, size_t *bytes);
/* Only strict fat32_open volumes; mounted-view policy is not raw-write authority. */
enum fat32_status fat32_writer_open(struct fat32_writer *writer,
    const struct fat32_volume *volume, const struct fat32_write_io *io,
    const struct fat32_workspace *workspace, uint32_t max_file_bytes,
    struct fat32_receipt *receipts, uint32_t receipt_count);
enum fat32_status fat32_create(struct fat32_writer *writer, const char *path,
    size_t path_bytes, bool directory, fat32_payload_fn payload, void *context,
    uint32_t bytes, struct fat32_receipt **receipt);
enum fat32_status fat32_append(struct fat32_writer *writer,
    struct fat32_receipt *receipt, fat32_payload_fn payload, void *context,
    uint32_t bytes);
enum fat32_status fat32_extend(struct fat32_writer *writer,
    struct fat32_receipt *receipt, uint32_t bytes);
enum fat32_status fat32_replace(struct fat32_writer *writer,
    struct fat32_receipt *receipt, fat32_payload_fn payload, void *context,
    uint32_t bytes);
const char *fat32_status_string(enum fat32_status status);

#endif
