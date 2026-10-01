/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_ACCESS_INTERNAL_H
#define PFS_ACCESS_INTERNAL_H

#include <pyxis_fs/access.h>

struct pfs_view {
  struct pfs_allocation allocation;
  struct pfs_volume *volume;
  struct pfs_object_id object;
  enum pfs_grant_scope scope;
  struct pfs_rights rights;
  struct pfs_view_identity identity;
};


#endif
