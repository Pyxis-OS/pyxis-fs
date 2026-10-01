/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_ACCESS_INTERNAL_H
#define PFS_ACCESS_INTERNAL_H

#include <pyxis_fs/access.h>

struct pfs_view {
  struct pfs_allocation allocation;
  struct pfs_volume *volume;
  struct pfs_runtime_object *runtime;
  struct pfs_object_id object;
  enum pfs_grant_scope scope;
  struct pfs_rights rights;
  struct pfs_view_identity identity;
};

/* These helpers run inside an accepted serialized operation. */
enum pfs_status pfs_view_operation_hold(struct pfs_view *view);
void pfs_view_operation_drop(struct pfs_view *view);
enum pfs_status pfs_view_reserve(struct pfs_view *parent,
  const struct pfs_object_id *id, uint16_t kind, const struct pfs_rights *rights,
  enum pfs_grant_scope scope, struct pfs_view **out);
/* The private view was never published; entry is outside the writer gate. */
void pfs_view_discard(struct pfs_view *view);

#endif
