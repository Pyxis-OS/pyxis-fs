/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_EDIT_H
#define PFS_EDIT_H

#include <pyxis_fs/platform.h>
#include <pyxis_fs/tree.h>

#define PFS_EDIT_NODES_MAX 15u
#define PFS_EDIT_RECORDS_MAX (2 * PFS_TREE_SLOTS_MAX + 2)
#define PFS_EDIT_INTERNAL_BYTES 304u

/* Private planning only. The caller proves source ownership, canonical encoding,
 * exact subtree minima and the namespace occupancy profile before entering.
 * Slot block IDs are distinct, reusable and disjoint from every published claim.
 * All arrays and workspace are disjoint, caller-owned and reserved in advance. */
struct pfs_edit_slot {
  uint64_t block;
  bool active;
  uint8_t data[PFS_BLOCK_SIZE];
};

struct pfs_edit_candidate {
  const struct pfs_block_reader *reader;
  struct pfs_tree_context context;
  uint64_t birth;
  struct pfs_reference root;
  struct pfs_edit_slot *slots;
  size_t slot_capacity;
  struct pfs_reference *retired;
  size_t retired_count;
  size_t retired_capacity;
};

struct pfs_edit_path {
  uint8_t data[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  uint16_t child;
};

struct pfs_edit_output {
  size_t slot;
  uint8_t data[PFS_BLOCK_SIZE];
};

struct pfs_edit_workspace {
  struct pfs_edit_path path[PFS_TREE_DEPTH_MAX];
  uint8_t sibling[PFS_BLOCK_SIZE];
  struct pfs_encoded_record records[2][PFS_EDIT_RECORDS_MAX];
  uint8_t internal[2][PFS_EDIT_INTERNAL_BYTES];
  struct pfs_reference consumed[PFS_EDIT_NODES_MAX];
  struct pfs_edit_output outputs[PFS_EDIT_NODES_MAX];
  size_t consumed_count;
  size_t output_count;
};

enum pfs_edit_operation {
  PFS_EDIT_INSERT,
  PFS_EDIT_UPDATE,
  PFS_EDIT_DELETE,
};

/* Candidate birth exceeds every source birth; context supplies the index owners,
 * geometry, pool and understood features. Empty indexes have a zero root.
 * UPDATE requires identical key and encoded length. Extent ranges cannot overlap
 * the next leaf; a range ending exactly at its minimum remains valid.
 * VOLUMES supports UPDATE only.
 * Success replaces the candidate root, staging buffers and retirement list;
 * failure leaves all candidate state unchanged. No device writes or allocation.
 * Workspace is scratch and may change on either outcome. */
enum pfs_status pfs_edit_tree(struct pfs_edit_candidate *candidate,
                              struct pfs_edit_workspace *workspace,
                              enum pfs_edit_operation operation,
                              const struct pfs_key *key,
                              const struct pfs_encoded_record *record);

#endif
