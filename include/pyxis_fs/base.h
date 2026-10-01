#ifndef PYXIS_FS_BASE_H
#define PYXIS_FS_BASE_H

#include <stddef.h>
#include <stdint.h>

enum pfs_status {
  PFS_OK,
  PFS_INVALID,
  PFS_ABSENT,
  PFS_CORRUPT,
  PFS_UNSUPPORTED,
  PFS_LIMIT,
  PFS_IO,
  PFS_NO_MEMORY,
  PFS_READ_ONLY,
  PFS_BUSY,
  PFS_DENIED,
  PFS_NOT_FOUND,
  PFS_NO_SPACE,
  PFS_QUOTA,
  PFS_RECOVERY_REQUIRED,
  PFS_EXISTS,
  PFS_DETACHED,
  PFS_CHANGED,
};

#define PFS_BLOCK_SIZE 4096u
#define PFS_ID_SIZE 16u
#define PFS_ID_TEXT_SIZE 32u
#define PFS_NAME_MAX 255u
#define PFS_FORMAT_VERSION 1u
#define PFS_BLOCK_HEADER_SIZE 128u
#define PFS_REFERENCE_SIZE 24u
#define PFS_RECORD_HEADER_SIZE 16u
#define PFS_TREE_HEADER_SIZE 192u
#define PFS_TREE_SLOT_SIZE 4u
#define PFS_TREE_DEPTH_MAX 8u
#define PFS_VOLUME_MAX 256u
#define PFS_RECORD_COUNT_MAX UINT64_C(1048576)
#define PFS_ALLOCATION_COUNT_MAX UINT64_C(4194304)
#define PFS_FILE_SIZE_MAX (UINT64_C(1) << 40)
#define PFS_POOL_BLOCKS_MIN (UINT64_C(1) << 14)
#define PFS_POOL_BLOCKS_MAX (UINT64_C(1) << 28)
#define PFS_IO_BLOCKS_MAX 16u
#define PFS_MEMORY_DEFAULT (UINT64_C(128) << 20)
#define PFS_MEMORY_MAX (UINT64_C(1) << 30)

struct pfs_pool_id { uint8_t bytes[PFS_ID_SIZE]; };
struct pfs_volume_id { uint8_t bytes[PFS_ID_SIZE]; };
struct pfs_object_id { uint8_t bytes[PFS_ID_SIZE]; };
struct pfs_principal_id { uint8_t bytes[PFS_ID_SIZE]; };

struct pfs_name {
  uint16_t length;
  uint8_t bytes[PFS_NAME_MAX];
};

#define PFS_FEATURE_ORPHANS (UINT64_C(1) << 0)

struct pfs_features {
  uint64_t read_required;
  uint64_t write_required;
  uint64_t optional;
};

enum pfs_block_type {
  PFS_BLOCK_SUPER = 1,
  PFS_BLOCK_POOL = 2,
  PFS_BLOCK_TREE = 3,
};

enum pfs_index_kind {
  PFS_INDEX_VOLUMES = 1,
  PFS_INDEX_VOLUME_NAMES = 2,
  PFS_INDEX_ALLOCATION = 3,
  PFS_INDEX_OBJECTS = 4,
  PFS_INDEX_DIRECTORY = 5,
  PFS_INDEX_EXTENTS = 6,
  PFS_INDEX_GRANTS = 7,
  PFS_INDEX_ORPHANS = 8,
};

enum pfs_record_type {
  PFS_RECORD_VOLUME = 1,
  PFS_RECORD_VOLUME_NAME = 2,
  PFS_RECORD_ALLOCATION = 3,
  PFS_RECORD_OBJECT = 4,
  PFS_RECORD_DIRENT = 5,
  PFS_RECORD_EXTENT = 6,
  PFS_RECORD_GRANT = 7,
  PFS_RECORD_ORPHAN = 8,
  PFS_INTERNAL_RECORD_TYPE = 256,
};

struct pfs_reference {
  uint64_t block;
  uint64_t birth;
  uint16_t type;
  uint16_t version;
};

/* Caller supplies trusted geometry and the already checked containing header.
 * Decoding establishes local structure only, never reachability or ownership. */
struct pfs_record_context {
  uint64_t block_count;
  uint64_t selected_generation;
  uint64_t containing_birth;
  struct pfs_features features;
};

const char *pfs_status_string(enum pfs_status status);
uint32_t pfs_crc32c(const void *data, size_t length);
enum pfs_status pfs_name_validate(const uint8_t *bytes, size_t length);
enum pfs_status pfs_features_read(const struct pfs_features *features);
enum pfs_status pfs_features_write(const struct pfs_features *features);
enum pfs_status pfs_features_check(const struct pfs_features *features);
/* ORPHANS is recognized only in a volume feature mask. */
enum pfs_status pfs_volume_features_read(const struct pfs_features *features);
enum pfs_status pfs_volume_features_write(const struct pfs_features *features);
enum pfs_status pfs_volume_features_check(const struct pfs_features *features);

enum pfs_status pfs_pool_id_parse(const char *text, size_t length, struct pfs_pool_id *out);
enum pfs_status pfs_volume_id_parse(const char *text, size_t length, struct pfs_volume_id *out);
enum pfs_status pfs_object_id_parse(const char *text, size_t length, struct pfs_object_id *out);
enum pfs_status pfs_principal_id_parse(const char *text, size_t length, struct pfs_principal_id *out);
void pfs_pool_id_format(const struct pfs_pool_id *id, char out[PFS_ID_TEXT_SIZE + 1]);
void pfs_volume_id_format(const struct pfs_volume_id *id, char out[PFS_ID_TEXT_SIZE + 1]);
void pfs_object_id_format(const struct pfs_object_id *id, char out[PFS_ID_TEXT_SIZE + 1]);
void pfs_principal_id_format(const struct pfs_principal_id *id, char out[PFS_ID_TEXT_SIZE + 1]);

#endif
