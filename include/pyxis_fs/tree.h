#ifndef PYXIS_FS_TREE_H
#define PYXIS_FS_TREE_H

#include <pyxis_fs/block.h>

#define PFS_KEY_MAX PFS_NAME_MAX
#define PFS_TREE_SLOTS_MAX ((PFS_BLOCK_SIZE - PFS_TREE_HEADER_SIZE) / \
                          (PFS_RECORD_HEADER_SIZE + PFS_TREE_SLOT_SIZE))

struct pfs_key {
  uint16_t length;
  uint8_t bytes[PFS_KEY_MAX];
};

struct pfs_internal_record {
  struct pfs_reference child;
  struct pfs_key minimum;
};

struct pfs_tree_context {
  struct pfs_block_context block;
  uint16_t kind;
  /* Zero for an index root; otherwise the referring node's decoded level. */
  uint16_t parent_level;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
};

struct pfs_tree_slot {
  uint16_t offset;
  uint16_t length;
};

struct pfs_tree {
  struct pfs_block_header header;
  uint16_t kind;
  uint16_t level;
  uint16_t count;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  struct pfs_key minimum;
  struct pfs_key maximum;
  struct pfs_tree_slot slots[PFS_TREE_SLOTS_MAX];
};

/* Encoding borrows each complete, already encoded record for this call only. */
struct pfs_encoded_record {
  const uint8_t *data;
  size_t length;
};

enum pfs_status pfs_key_validate(uint16_t kind, const struct pfs_key *key);
/* Inputs must first pass pfs_key_validate for this kind. */
int pfs_key_compare(uint16_t kind, const struct pfs_key *left, const struct pfs_key *right);
enum pfs_status pfs_internal_record_decode(const uint8_t *data, size_t slot_length,
                                          uint16_t kind,
                                          const struct pfs_record_context *context,
                                          struct pfs_internal_record *out);
enum pfs_status pfs_internal_record_encode(uint8_t *data, size_t capacity, uint16_t kind,
                                          const struct pfs_record_context *context,
                                          const struct pfs_internal_record *value,
                                          size_t *written);
/* Validates every local slot/record and key order. It does not read children,
 * prove separator/child minimum agreement, or prove allocation ownership.
 * Output owns offsets into the caller's unchanged input block, not pointers. */
enum pfs_status pfs_tree_decode(const uint8_t *data, size_t length,
                               const struct pfs_tree_context *context,
                               struct pfs_tree *out);
/* Uses header identity, kind, level, count and owners; derives used/slots/keys.
 * All buffers and structures must be disjoint. Failure leaves output unchanged. */
enum pfs_status pfs_tree_encode(uint8_t *data, size_t capacity,
                               const struct pfs_tree_context *context,
                               const struct pfs_tree *value,
                               const struct pfs_encoded_record *records);

#endif
