/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_FAT32_H
#define PYXIS_FS_FAT32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAT32_SECTOR_BYTES 512u
#define FAT32_NAME_MAX 255u
#define FAT32_ALIAS_MAX 12u
#define FAT32_ATTR_DIRECTORY 0x10u

enum fat32_status {
  FAT32_OK,
  FAT32_INVALID,
  FAT32_CORRUPT,
  FAT32_UNSUPPORTED,
  FAT32_IO,
  FAT32_NO_MEMORY,
  FAT32_LIMIT,
  FAT32_NAME_TOO_LONG,
  FAT32_NOT_FOUND,
  FAT32_NOT_DIRECTORY,
  FAT32_IS_DIRECTORY,
  FAT32_OUTPUT,
};

/* The callback fills the entire request or returns false. Its source remains
 * quiescent and alive throughout open and every operation. No snapshot or
 * exclusion is supplied by this library. GPT reads inspect disk metadata;
 * subsequent filesystem reads are confined to the selected partition. */
typedef bool (*fat32_read_sectors_fn)(void *context, uint64_t first_sector,
                                    uint32_t sector_count, void *bytes);

struct fat32_source {
  fat32_read_sectors_fn read;
  void *context;
  uint64_t sectors;
  uint32_t sector_bytes, max_read_sectors;
};

/* Memory is aligned for max_align_t, disjoint from arguments/output, and
 * borrowed only for this call. Limits count examined slots, links/comparisons
 * and backing sectors. Zero is a zero budget, never an unlimited budget.
 * The same workspace cannot be used by nested or concurrent operations. */
struct fat32_workspace {
  void *memory;
  size_t bytes;
  uint64_t step_limit, sector_limit;
  uint32_t directory_depth_limit;
};

/* Copied, read-only geometry. Offsets fat_first/data_first are relative to the
 * partition. Do not modify this result or use it with different media. It is
 * an inspection view, not a mount or a persistent file identity. */
struct fat32_volume {
  struct fat32_source source;
  uint64_t partition_first, partition_sectors;
  uint64_t fat_first, data_first;
  uint32_t volume_sectors, fat_sectors, cluster_count, root_cluster, gpt_entry;
  uint8_t sectors_per_cluster, fat_count, media;
  bool clean_shutdown, no_hard_error;
  uint8_t disk_guid[16], partition_guid[16], partition_type[16];
  uint64_t partition_attributes;
};

struct fat32_entry {
  char name[FAT32_NAME_MAX + 1], alias[FAT32_ALIAS_MAX + 1];
  uint8_t short_name[11];
  uint16_t name_bytes;
  uint8_t alias_bytes, attributes;
  uint32_t first_cluster, file_bytes;
  bool has_long_name, name_ascii, alias_ascii;
};

struct fat32_check_report {
  uint64_t files, directories, allocated_clusters, free_clusters, bad_clusters;
  bool all_names_ascii;
};

/* Sink pointers are borrowed until the callback returns. Returning false stops
 * output with FAT32_OUTPUT. Metadata/chains are preflighted before emission;
 * later I/O, budget or sink failure can leave an emitted prefix. This does not establish
 * integrity of metadata outside the operation's checked set. */
typedef bool (*fat32_entry_sink_fn)(void *context, const struct fat32_entry *entry);
typedef bool (*fat32_bytes_sink_fn)(void *context, const void *bytes, size_t length);

size_t fat32_open_workspace_size(void);
enum fat32_status fat32_workspace_size(const struct fat32_volume *volume,
                                      uint32_t directory_depth_limit, size_t *bytes);
/* gpt_entry is one-based. Both GPT copies must validate and agree; legacy MBR,
 * hybrid MBR and bare FAT volumes are unsupported. Results publish on success.
 * Open checks geometry/allocation headers, not whole-volume reachability.
 * Optional backup sectors (0/0xffff) and absent FSInfo (0xffff) are accepted;
 * declared copies must be structurally valid. FSInfo hints may be stale. */
enum fat32_status fat32_open(const struct fat32_source *source, uint32_t gpt_entry,
                            struct fat32_workspace *workspace, struct fat32_volume *volume);
/* Paths are bounded ASCII, rooted at '/'; '.'/'..' and empty interior components
 * refuse. Components are at most 255 bytes. Unicode names are listed intact;
 * ASCII aliases can read those objects. Short-only OEM names are unsupported.
 * List preflights the directory and its immediate children's chains. Read
 * preflights the path directories and the complete selected file chain. */
enum fat32_status fat32_list(const struct fat32_volume *volume, const char *path,
                            size_t path_bytes, struct fat32_workspace *workspace,
                            fat32_entry_sink_fn sink, void *context);
enum fat32_status fat32_read(const struct fat32_volume *volume, const char *path,
                            size_t path_bytes, struct fat32_workspace *workspace,
                            fat32_bytes_sink_fn sink, void *context);
/* Check traces all reachable directories/files and checks all meaningful FAT
 * entries. Repeated ownership and orphan allocations refuse; marked bad clusters
 * are allowed. FSInfo counts/hints are range checked, never treated as a proof.
 * all_names_ascii records whether the supported namespace folding covers every
 * name/alias; Unicode/OEM folding is not inferred. Report publishes on success. */
enum fat32_status fat32_check(const struct fat32_volume *volume,
                             struct fat32_workspace *workspace,
                             struct fat32_check_report *report);

#endif
