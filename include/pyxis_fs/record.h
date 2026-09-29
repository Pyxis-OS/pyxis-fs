#ifndef PYXIS_FS_RECORD_H
#define PYXIS_FS_RECORD_H

#include <pyxis_fs/base.h>

#define PFS_VOLUME_RECORD_SIZE 448u
#define PFS_NAME_RECORD_PREFIX_SIZE 40u
#define PFS_ALLOCATION_RECORD_SIZE 80u
#define PFS_OBJECT_RECORD_SIZE 128u
#define PFS_EXTENT_RECORD_SIZE 64u
#define PFS_GRANT_RECORD_SIZE 80u

enum pfs_object_kind {
  PFS_OBJECT_FILE = 1,
  PFS_OBJECT_DIRECTORY = 2,
};

enum pfs_storage_kind {
  PFS_STORAGE_NONE = 0,
  PFS_STORAGE_INLINE = 1,
  PFS_STORAGE_TREE = 2,
};

enum pfs_allocation_state {
  PFS_ALLOCATION_FREE = 0,
  PFS_ALLOCATION_POOL = 1,
  PFS_ALLOCATION_VOLUME = 2,
  PFS_ALLOCATION_RETIRED = 3,
};

enum pfs_budget_charge {
  PFS_CHARGE_PERMANENT = 0,
  PFS_CHARGE_ORDINARY = 1,
  PFS_CHARGE_MIGRATION = 2,
  PFS_CHARGE_RECOVERY = 3,
};

enum pfs_grant_scope {
  PFS_SCOPE_OBJECT = 0,
  PFS_SCOPE_SUBTREE = 1,
};

#define PFS_FILE_METADATA (UINT64_C(1) << 0)
#define PFS_FILE_READ (UINT64_C(1) << 1)
#define PFS_FILE_WRITE (UINT64_C(1) << 2)
#define PFS_FILE_RESIZE (UINT64_C(1) << 3)
#define PFS_FILE_CHECKPOINT (UINT64_C(1) << 4)
#define PFS_FILE_RIGHTS_ALL UINT64_C(0x1f)
#define PFS_DIR_METADATA (UINT64_C(1) << 0)
#define PFS_DIR_LIST (UINT64_C(1) << 1)
#define PFS_DIR_LOOKUP (UINT64_C(1) << 2)
#define PFS_DIR_CREATE (UINT64_C(1) << 3)
#define PFS_DIR_REMOVE (UINT64_C(1) << 4)
#define PFS_DIR_REPLACE (UINT64_C(1) << 5)
#define PFS_DIR_RIGHTS_ALL UINT64_C(0x3f)
#define PFS_ADMIN_INSPECT (UINT64_C(1) << 0)
#define PFS_ADMIN_GRANTS (UINT64_C(1) << 1)
#define PFS_ADMIN_OWNER (UINT64_C(1) << 2)
#define PFS_ADMIN_RIGHTS_ALL UINT64_C(0x07)

struct pfs_volume_record {
  struct pfs_volume_id id;
  struct pfs_name name;
  struct pfs_features features;
  struct pfs_object_id root_object;
  struct pfs_reference object_root;
  struct pfs_reference grant_root;
  uint64_t guarantee;
  uint64_t quota;
  uint64_t live_blocks;
  uint64_t retired_blocks;
  uint64_t object_count;
};

struct pfs_volume_name_record {
  struct pfs_name name;
  struct pfs_volume_id volume;
};

struct pfs_allocation_record {
  uint64_t first;
  uint64_t count;
  uint8_t state;
  uint8_t charge;
  struct pfs_volume_id owner;
  uint64_t birth;
  uint64_t retirement;
};

struct pfs_extent_mapping {
  uint64_t logical_first;
  uint64_t count;
  uint64_t physical_first;
  uint64_t birth;
};

struct pfs_object_record {
  struct pfs_object_id id;
  uint16_t kind;
  uint16_t storage_kind;
  struct pfs_principal_id owner;
  struct pfs_object_id parent;
  uint64_t file_length;
  /* Only fields selected by storage_kind are interpreted. */
  struct pfs_extent_mapping inline_extent;
  struct pfs_reference tree_root;
  uint64_t directory_count;
};

struct pfs_dirent_record {
  struct pfs_name name;
  uint16_t child_kind;
  struct pfs_object_id object;
};

struct pfs_extent_record {
  struct pfs_extent_mapping mapping;
};

struct pfs_grant_record {
  struct pfs_object_id object;
  struct pfs_principal_id principal;
  uint8_t scope;
  uint64_t file_rights;
  uint64_t directory_rights;
  uint64_t admin_rights;
};

/* Local validators use CORRUPT for malformed fields and LIMIT for profile limits.
 * They do not establish allocation ownership, tree kind/owners, or reachability.
 * Decode publishes an owned copy only on success. Encode emits version 1 with
 * zero extension bytes and refuses enabled features; it is for new images only.
 * Encoding inputs must not overlap the destination. */
/* Volume validation exposes the fixed envelope without interpreting its feature
 * masks. Check pfs_features_read before interpreting that volume's trees. */
/* Context features describe the containing index: pool features for catalog
 * envelopes, volume features for records inside that volume. */
enum pfs_status pfs_volume_record_validate(const struct pfs_volume_record *record,
                                          const struct pfs_record_context *context);
enum pfs_status pfs_volume_record_decode(const void *data, size_t slot_length,
                                        const struct pfs_record_context *context,
                                        struct pfs_volume_record *out);
enum pfs_status pfs_volume_record_encode(void *data, size_t capacity,
                                        const struct pfs_record_context *context,
                                        const struct pfs_volume_record *record,
                                        size_t *written);

enum pfs_status pfs_volume_name_record_validate(const struct pfs_volume_name_record *record,
                                               const struct pfs_record_context *context);
enum pfs_status pfs_volume_name_record_decode(const void *data, size_t slot_length,
                                             const struct pfs_record_context *context,
                                             struct pfs_volume_name_record *out);
enum pfs_status pfs_volume_name_record_encode(void *data, size_t capacity,
                                             const struct pfs_record_context *context,
                                             const struct pfs_volume_name_record *record,
                                             size_t *written);

enum pfs_status pfs_allocation_record_validate(const struct pfs_allocation_record *record,
                                              const struct pfs_record_context *context);
enum pfs_status pfs_allocation_record_decode(const void *data, size_t slot_length,
                                            const struct pfs_record_context *context,
                                            struct pfs_allocation_record *out);
enum pfs_status pfs_allocation_record_encode(void *data, size_t capacity,
                                            const struct pfs_record_context *context,
                                            const struct pfs_allocation_record *record,
                                            size_t *written);

enum pfs_status pfs_object_record_validate(const struct pfs_object_record *record,
                                          const struct pfs_record_context *context);
enum pfs_status pfs_object_record_decode(const void *data, size_t slot_length,
                                        const struct pfs_record_context *context,
                                        struct pfs_object_record *out);
enum pfs_status pfs_object_record_encode(void *data, size_t capacity,
                                        const struct pfs_record_context *context,
                                        const struct pfs_object_record *record,
                                        size_t *written);

enum pfs_status pfs_dirent_record_validate(const struct pfs_dirent_record *record,
                                          const struct pfs_record_context *context);
enum pfs_status pfs_dirent_record_decode(const void *data, size_t slot_length,
                                        const struct pfs_record_context *context,
                                        struct pfs_dirent_record *out);
enum pfs_status pfs_dirent_record_encode(void *data, size_t capacity,
                                        const struct pfs_record_context *context,
                                        const struct pfs_dirent_record *record,
                                        size_t *written);

enum pfs_status pfs_extent_record_validate(const struct pfs_extent_record *record,
                                          const struct pfs_record_context *context);
enum pfs_status pfs_extent_record_decode(const void *data, size_t slot_length,
                                        const struct pfs_record_context *context,
                                        struct pfs_extent_record *out);
enum pfs_status pfs_extent_record_encode(void *data, size_t capacity,
                                        const struct pfs_record_context *context,
                                        const struct pfs_extent_record *record,
                                        size_t *written);
enum pfs_status pfs_extent_mapping_validate(const struct pfs_extent_mapping *mapping,
                                           const struct pfs_record_context *context);
/* Apply after local mapping validation, using the owning file's byte length. */
enum pfs_status pfs_extent_file_validate(const struct pfs_extent_mapping *mapping,
                                        uint64_t file_length);

enum pfs_status pfs_grant_record_validate(const struct pfs_grant_record *record,
                                         const struct pfs_record_context *context);
enum pfs_status pfs_grant_record_decode(const void *data, size_t slot_length,
                                       const struct pfs_record_context *context,
                                       struct pfs_grant_record *out);
enum pfs_status pfs_grant_record_encode(void *data, size_t capacity,
                                       const struct pfs_record_context *context,
                                       const struct pfs_grant_record *record,
                                       size_t *written);
/* Apply after local grant validation and resolving the target object. */
enum pfs_status pfs_grant_target_validate(const struct pfs_grant_record *record,
                                         uint16_t target_kind);

#endif
