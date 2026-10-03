/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NPFS_INTERNAL_H
#define PYXIS_FS_NPFS_INTERNAL_H

#include <pyxis_fs/npfs.h>

bool npfs_bytes_zero(const void *bytes, size_t length);
bool npfs_bytes_equal(const void *left, const void *right, size_t length);
bool npfs_flags_valid(const struct npfs_header *header, uint32_t flags, uint32_t known);
uint32_t npfs_block_crc(const uint8_t *block, unsigned checksum_offset);
bool npfs_pointers_valid(const struct npfs_header *header,
                        const uint64_t pointers[NPFS_POINTER_COUNT]);

#endif
