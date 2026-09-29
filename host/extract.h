/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_EXTRACT_H
#define PYXIS_FS_EXTRACT_H

#include <pyxis_fs/read.h>

/* Diagnostic copy through the shared reader. Creates a fresh destination,
 * leaves partial output on failure, and charges traversal/copy storage to the
 * pool's memory budget. The selected volume remains alive throughout. */
enum pfs_status host_extract(struct pfs_volume *volume,
                             const struct pfs_object_record *object,
                             const char *path);

#endif
