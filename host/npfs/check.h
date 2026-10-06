/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NPFS_CHECK_H
#define PYXIS_FS_NPFS_CHECK_H

#include "journal.h"

/* Requires a selected EMPTY journal. Read-only structural proof; no repair. */
enum npfs_status npfs_check_image(struct npfs_image *image);

#endif
