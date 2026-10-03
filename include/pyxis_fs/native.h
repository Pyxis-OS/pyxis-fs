/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NATIVE_H
#define PYXIS_FS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PNF_BLOCK_SIZE 4096u
#define PNF_ID_SIZE 16u
#define PNF_NAME_MAX 255u
#define PNF_VOLUME_COUNT 64u
#define PNF_VOLUME_SIZE 512u
#define PNF_VOLUME_TABLE_BLOCKS 8u
#define PNF_INODE_SIZE 256u
#define PNF_INODE_FIELD_SIZE 72u
#define PNF_INODE_RESERVE_SIZE 64u
#define PNF_POINTER_COUNT 15u
#define PNF_DIRECT_COUNT 12u
#define PNF_INDIRECT_COUNT 512u
#define PNF_BITMAP_BITS (PNF_BLOCK_SIZE * 8u)
#define PNF_DIRENT_HEADER_SIZE 32u
#define PNF_DESCRIPTOR_SIZE 32u
#define PNF_DESCRIPTORS_PER_BLOCK (PNF_BLOCK_SIZE / PNF_DESCRIPTOR_SIZE)
#define PNF_FIELD_SIZE 128u
#define PNF_FILE_BLOCKS_MAX (UINT64_C(12) + 512 + 512 * UINT64_C(512) + 512 * 512 * UINT64_C(512))
#define PNF_FILE_SIZE_MAX (PNF_FILE_BLOCKS_MAX * PNF_BLOCK_SIZE)

#define PNF_VOLUME_UNUSED 0u
#define PNF_VOLUME_LIVE 1u
#define PNF_INODE_FREE 0u
#define PNF_INODE_FILE 1u
#define PNF_INODE_DIRECTORY 2u
#define PNF_MAPPING_POINTERS 1u
#define PNF_TIME_CREATED_VALID (UINT32_C(1) << 0)
#define PNF_TIME_MODIFIED_VALID (UINT32_C(1) << 1)
#define PNF_CLEANUP_DETACHED (UINT32_C(1) << 0)
#define PNF_CLEANUP_SHRINK (UINT32_C(1) << 1)
#define PNF_CLEANUP_FLAGS (PNF_CLEANUP_DETACHED | PNF_CLEANUP_SHRINK)
#define PNF_JOURNAL_EMPTY 0u
#define PNF_JOURNAL_COMMITTED 1u
#define PNF_METADATA_BITMAP 1u
#define PNF_METADATA_VOLUMES 2u
#define PNF_METADATA_INODES 3u
#define PNF_METADATA_DIRECTORY 4u
#define PNF_METADATA_INDIRECT 5u

/* This library is format-only. Providers define these symbols at link time.
 * Buffers are disjoint; copy/zero do not allocate or retain storage. */
void pnf_memory_copy(void *destination, const void *source, size_t length);
void pnf_memory_zero(void *destination, size_t length);

enum pnf_status {
  PNF_OK,
  PNF_INVALID,
  PNF_CORRUPT,
  PNF_UNSUPPORTED,
  PNF_IO,
  PNF_NO_MEMORY,
  PNF_NO_SPACE,
  PNF_RECOVERY_REQUIRED,
  PNF_NOT_FOUND,
  PNF_EXISTS,
};

struct pnf_header {
  uint8_t pool_id[PNF_ID_SIZE];
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

struct pnf_volume {
  uint8_t id[PNF_ID_SIZE];
  uint32_t state;
  uint32_t flags;
  uint32_t mapping;
  uint16_t name_length;
  uint64_t root_inode;
  uint64_t cleanup_head;
  uint64_t inode_bytes;
  uint8_t name[PNF_NAME_MAX + 1];
  uint64_t pointers[PNF_POINTER_COUNT];
};

struct pnf_inode {
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
  uint64_t pointers[PNF_POINTER_COUNT];
};

struct pnf_dirent {
  uint64_t inode;
  uint16_t record_length;
  uint16_t name_length;
  uint32_t flags;
  uint8_t name[PNF_NAME_MAX];
};

struct pnf_control {
  uint8_t pool_id[PNF_ID_SIZE];
  uint64_t sequence;
  uint32_t state;
  uint32_t image_count;
  uint32_t descriptor_blocks;
  uint32_t payload_crc;
};

struct pnf_descriptor {
  uint64_t home;
  uint32_t kind;
  uint32_t flags;
};

struct pnf_map_path {
  unsigned slot;
  unsigned depth;
  unsigned index[3];
};

const char *pnf_status_string(enum pnf_status status);
uint16_t pnf_get_u16(const void *bytes);
uint32_t pnf_get_u32(const void *bytes);
uint64_t pnf_get_u64(const void *bytes);
void pnf_put_u16(void *bytes, uint16_t value);
void pnf_put_u32(void *bytes, uint32_t value);
void pnf_put_u64(void *bytes, uint64_t value);
uint32_t pnf_crc_begin(void);
uint32_t pnf_crc_update(uint32_t crc, const void *bytes, size_t length);
uint32_t pnf_crc_finish(uint32_t crc);
uint32_t pnf_crc32c(const void *bytes, size_t length);
uint32_t pnf_payload_begin(const struct pnf_control *control);
bool pnf_id_valid(const uint8_t id[PNF_ID_SIZE]);
bool pnf_name_valid(const uint8_t *name, size_t length);
int64_t pnf_timestamp(int64_t seconds, uint32_t nanoseconds);
uint64_t pnf_bitmap_blocks(uint64_t pool_blocks);
uint64_t pnf_journal_capacity(uint64_t journal_blocks);
bool pnf_data_block_valid(const struct pnf_header *header, uint64_t block);
enum pnf_status pnf_map_path(uint64_t logical, struct pnf_map_path *path);

/* Encoded lengths are format constants, not sizeof the decoded C structures.
 * Callers supply accessible, disjoint buffers of those lengths. Decoders publish
 * copied results only on success; failed encodes leave destination unchanged.
 * Reserved bytes are emitted zero and ignored when decoding for extension.
 * Structural ownership/reachability and bitmap proof belong to fsck/the writer. */
enum pnf_status pnf_header_layout(uint64_t blocks, uint64_t journal_blocks,
                                 const uint8_t id[PNF_ID_SIZE], struct pnf_header *header);
enum pnf_status pnf_header_validate(const struct pnf_header *header);
enum pnf_status pnf_features_check(const struct pnf_header *header, bool writable);
enum pnf_status pnf_header_encode(const struct pnf_header *header, void *block);
enum pnf_status pnf_header_decode(const void *block, struct pnf_header *header);
enum pnf_status pnf_volume_encode(const struct pnf_header *header,
                                 const struct pnf_volume *volume, void *record);
enum pnf_status pnf_volume_decode(const struct pnf_header *header, const void *record,
                                 struct pnf_volume *volume);
enum pnf_status pnf_inode_encode(const struct pnf_header *header,
                                const struct pnf_inode *inode, void *record);
enum pnf_status pnf_inode_decode(const struct pnf_header *header, const void *record,
                                struct pnf_inode *inode);
enum pnf_status pnf_dirent_encode(const struct pnf_dirent *entry, void *record);
enum pnf_status pnf_dirent_decode(const void *record, size_t available,
                                 struct pnf_dirent *entry);
enum pnf_status pnf_control_encode(const struct pnf_header *header,
                                  const struct pnf_control *control, void *block);
enum pnf_status pnf_control_decode(const struct pnf_header *header, const void *block,
                                  struct pnf_control *control);
enum pnf_status pnf_descriptor_encode(const struct pnf_header *header,
                                     const struct pnf_descriptor *descriptor, void *record);
enum pnf_status pnf_descriptor_decode(const struct pnf_header *header, const void *record,
                                     struct pnf_descriptor *descriptor);

#endif
