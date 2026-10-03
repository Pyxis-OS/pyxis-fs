/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NATIVE_CHECK_H
#define PYXIS_FS_NATIVE_CHECK_H

#include "host.h"

/* Requires a selected EMPTY journal. Read-only structural proof; no repair. */
enum pnf_status native_check_image(struct native_image *image);
/* Requires exclusive writable opening. Validates the complete committed payload
 * before any home write; a write/flush failure leaves recovery required. */
enum pnf_status native_replay(struct native_image *image);

#endif
