#ifndef PFS_READ_INTERNAL_H
#define PFS_READ_INTERNAL_H

#include <pyxis_fs/read.h>

/* Internal lifetime retention only; these helpers confer no object rights. */
enum pfs_status pfs_volume_hold(struct pfs_volume *volume);
enum pfs_status pfs_volume_drop(struct pfs_volume *volume);
uint64_t pfs_volume_generation(const struct pfs_volume *volume);
const struct pfs_pool_id *pfs_volume_pool_id(const struct pfs_volume *volume);

#endif
