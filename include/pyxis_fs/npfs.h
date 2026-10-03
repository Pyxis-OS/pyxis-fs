/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NPFS_H
#define PYXIS_FS_NPFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NPFS_BLOCK_SIZE 4096u
#define NPFS_ID_SIZE 16u
#define NPFS_NAME_MAX 255u
#define NPFS_VOLUME_COUNT 64u
#define NPFS_VOLUME_SIZE 512u
#define NPFS_VOLUME_TABLE_BLOCKS 8u
#define NPFS_INODE_SIZE 256u
#define NPFS_INODE_FIELD_SIZE 72u
#define NPFS_INODE_RESERVE_SIZE 64u
#define NPFS_POINTER_COUNT 15u
#define NPFS_DIRECT_COUNT 12u
#define NPFS_INDIRECT_COUNT 512u
#define NPFS_BITMAP_BITS (NPFS_BLOCK_SIZE * 8u)
#define NPFS_DIRENT_HEADER_SIZE 32u
#define NPFS_DESCRIPTOR_SIZE 32u
#define NPFS_DESCRIPTORS_PER_BLOCK (NPFS_BLOCK_SIZE / NPFS_DESCRIPTOR_SIZE)
#define NPFS_FIELD_SIZE 128u
#define NPFS_FILE_BLOCKS_MAX (UINT64_C(12) + 512 + 512 * UINT64_C(512) + 512 * 512 * UINT64_C(512))
#define NPFS_FILE_SIZE_MAX (NPFS_FILE_BLOCKS_MAX * NPFS_BLOCK_SIZE)

#define NPFS_VOLUME_UNUSED 0u
#define NPFS_VOLUME_LIVE 1u
#define NPFS_INODE_FREE 0u
#define NPFS_INODE_FILE 1u
#define NPFS_INODE_DIRECTORY 2u
#define NPFS_MAPPING_POINTERS 1u
#define NPFS_TIME_CREATED_VALID (UINT32_C(1) << 0)
#define NPFS_TIME_MODIFIED_VALID (UINT32_C(1) << 1)
#define NPFS_INODE_FLAGS (NPFS_TIME_CREATED_VALID | NPFS_TIME_MODIFIED_VALID)
#define NPFS_CLEANUP_DETACHED (UINT32_C(1) << 0)
#define NPFS_CLEANUP_SHRINK (UINT32_C(1) << 1)
#define NPFS_CLEANUP_FLAGS (NPFS_CLEANUP_DETACHED | NPFS_CLEANUP_SHRINK)
#define NPFS_JOURNAL_EMPTY 0u
#define NPFS_JOURNAL_COMMITTED 1u
#define NPFS_METADATA_BITMAP 1u
#define NPFS_METADATA_VOLUMES 2u
#define NPFS_METADATA_INODES 3u
#define NPFS_METADATA_DIRECTORY 4u
#define NPFS_METADATA_INDIRECT 5u

/* This library is format-only. Providers define these symbols at link time.
 * Buffers are disjoint; copy/zero do not allocate or retain storage. */
void npfs_memory_copy(void *destination, const void *source, size_t length);
void npfs_memory_zero(void *destination, size_t length);

enum npfs_status {
  NPFS_OK,
  NPFS_INVALID,
  NPFS_CORRUPT,
  NPFS_UNSUPPORTED,
  NPFS_IO,
  NPFS_NO_MEMORY,
  NPFS_NO_SPACE,
  NPFS_RECOVERY_REQUIRED,
  NPFS_NOT_FOUND,
  NPFS_EXISTS,
};

struct npfs_header {
  uint8_t pool_id[NPFS_ID_SIZE];
  uint64_t pool_blocks;
  uint64_t bitmap_start;
  uint64_t bitmap_blocks;
  uint64_t volume_start;
  uint64_t volume_blocks;
  uint64_t journal_start;
  uint64_t journal_blocks;
  uint64_t compatible;
  uint64_t read_only_compatible;
  uint64_t required;
};

struct npfs_volume {
  uint8_t id[NPFS_ID_SIZE];
  uint32_t state;
  uint32_t flags;
  uint32_t mapping;
  uint16_t name_length;
  uint64_t root_inode;
  uint64_t cleanup_head;
  uint64_t inode_bytes;
  uint8_t name[NPFS_NAME_MAX + 1];
  uint64_t pointers[NPFS_POINTER_COUNT];
};

struct npfs_inode {
  uint16_t kind;
  uint16_t mapping;
  uint32_t flags;
  uint64_t size;
  uint64_t cleanup_next;
  uint64_t shrink_target;
  uint32_t cleanup;
  int64_t created_ns;
  int64_t modified_ns;
  uint64_t parent;
  uint64_t pointers[NPFS_POINTER_COUNT];
};

struct npfs_dirent {
  uint64_t inode;
  uint16_t record_length;
  uint16_t name_length;
  uint32_t flags;
  uint8_t name[NPFS_NAME_MAX];
};

struct npfs_control {
  uint8_t pool_id[NPFS_ID_SIZE];
  uint64_t sequence;
  uint32_t state;
  uint32_t image_count;
  uint32_t descriptor_blocks;
  uint32_t payload_crc;
};

struct npfs_descriptor {
  uint64_t home;
  uint32_t kind;
  uint32_t flags;
};

struct npfs_map_path {
  unsigned slot;
  unsigned depth;
  unsigned index[3];
};

const char *npfs_status_string(enum npfs_status status);
uint16_t npfs_get_u16(const void *bytes);
uint32_t npfs_get_u32(const void *bytes);
uint64_t npfs_get_u64(const void *bytes);
void npfs_put_u16(void *bytes, uint16_t value);
void npfs_put_u32(void *bytes, uint32_t value);
void npfs_put_u64(void *bytes, uint64_t value);
uint32_t npfs_crc_begin(void);
uint32_t npfs_crc_update(uint32_t crc, const void *bytes, size_t length);
uint32_t npfs_crc_finish(uint32_t crc);
uint32_t npfs_crc32c(const void *bytes, size_t length);
uint32_t npfs_payload_begin(const struct npfs_control *control);
bool npfs_id_valid(const uint8_t id[NPFS_ID_SIZE]);
bool npfs_name_valid(const uint8_t *name, size_t length);
int64_t npfs_timestamp(int64_t seconds, uint32_t nanoseconds);
uint64_t npfs_bitmap_blocks(uint64_t pool_blocks);
uint64_t npfs_journal_capacity(uint64_t journal_blocks);
bool npfs_data_block_valid(const struct npfs_header *header, uint64_t block);
enum npfs_status npfs_map_path(uint64_t logical, struct npfs_map_path *path);

/* Encoded lengths are format constants, not sizeof the decoded C structures.
 * Callers supply accessible, disjoint buffers of those lengths. Decoders publish
 * copied results only on success; failed encodes leave destination unchanged.
 * Reserved bytes are emitted zero and ignored when decoding for extension.
 * Structural ownership/reachability and bitmap proof belong to fsck/the writer. */
enum npfs_status npfs_header_layout(uint64_t blocks, uint64_t journal_blocks,
                                 const uint8_t id[NPFS_ID_SIZE], struct npfs_header *header);
enum npfs_status npfs_header_validate(const struct npfs_header *header);
enum npfs_status npfs_features_check(const struct npfs_header *header, bool writable);
enum npfs_status npfs_header_encode(const struct npfs_header *header, void *block);
enum npfs_status npfs_header_decode(const void *block, struct npfs_header *header);
enum npfs_status npfs_volume_encode(const struct npfs_header *header,
                                 const struct npfs_volume *volume, void *record);
enum npfs_status npfs_volume_decode(const struct npfs_header *header, const void *record,
                                 struct npfs_volume *volume);
enum npfs_status npfs_inode_encode(const struct npfs_header *header,
                                const struct npfs_inode *inode, void *record);
enum npfs_status npfs_inode_decode(const struct npfs_header *header, const void *record,
                                struct npfs_inode *inode);
enum npfs_status npfs_dirent_encode(const struct npfs_header *header,
                                 const struct npfs_dirent *entry, void *record);
enum npfs_status npfs_dirent_decode(const struct npfs_header *header,
                                 const void *record, size_t available,
                                 struct npfs_dirent *entry);
enum npfs_status npfs_control_encode(const struct npfs_header *header,
                                  const struct npfs_control *control, void *block);
enum npfs_status npfs_control_decode(const struct npfs_header *header, const void *block,
                                  struct npfs_control *control);
/* A checksum-valid but structurally invalid control must not be discarded as a
 * torn copy when selecting journal state. */
bool npfs_control_checksum_valid(const void *block);
enum npfs_status npfs_descriptor_encode(const struct npfs_header *header,
                                     const struct npfs_descriptor *descriptor, void *record);
enum npfs_status npfs_descriptor_decode(const struct npfs_header *header, const void *record,
                                     struct npfs_descriptor *descriptor);

#endif
