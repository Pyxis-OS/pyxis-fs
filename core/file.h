/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_FILE_H
#define PFS_FILE_H

#include "writer.h"
#include "read_internal.h"

/* Shared, serial staging in the reserved arena's final quarter. The caller
 * refreshes the volume, then prepares the writer before begin. All pointers
 * expire at abort/commit or the next begin. No allocation or device writes. */
struct pfs_mutation;
enum pfs_status pfs_mutation_begin(struct pfs_volume *volume,
  struct pfs_batch *batch, struct pfs_mutation **out);
struct pfs_admit_volume *pfs_mutation_volume(struct pfs_mutation *mutation);
enum pfs_status pfs_mutation_tree(struct pfs_mutation *mutation, uint16_t kind,
  const struct pfs_object_id *object, struct pfs_reference *root,
  enum pfs_edit_operation operation, const struct pfs_key *key,
  const struct pfs_encoded_record *record);
enum pfs_status pfs_mutation_finish(struct pfs_mutation *mutation);

/* Authority and argument checks belong to the policy-view wrapper. No writer
 * gate is held on entry. Each batch uses a fresh confirmed object record.
 * The wrapper initializes the result, including the starting confirmed length
 * for resize, and handles no-ops before entry. Results retain confirmed user
 * progress independently of maintenance failure. */
enum pfs_status pfs_file_write(struct pfs_volume *volume,
  const struct pfs_object_id *id, uint64_t offset, const void *data, size_t length,
  struct pfs_write_result *result);
enum pfs_status pfs_file_resize(struct pfs_volume *volume,
  const struct pfs_object_id *id, uint64_t length, struct pfs_write_result *result);

#endif
