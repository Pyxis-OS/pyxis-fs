/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_NATIVE_INTERNAL_H
#define PYXIS_FS_NATIVE_INTERNAL_H

#include <pyxis_fs/native.h>

bool pnf_bytes_zero(const void *bytes, size_t length);
bool pnf_bytes_equal(const void *left, const void *right, size_t length);
bool pnf_flags_valid(const struct pnf_header *header, uint32_t flags, uint32_t known);
uint32_t pnf_block_crc(const uint8_t *block, unsigned checksum_offset);
bool pnf_pointers_valid(const struct pnf_header *header,
                        const uint64_t pointers[PNF_POINTER_COUNT]);

#endif
