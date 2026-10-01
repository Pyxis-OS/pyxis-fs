#ifndef PFS_INTERNAL_H
#define PFS_INTERNAL_H

#include <pyxis_fs/base.h>

struct pfs_block_header;
struct pfs_key;
/* Leaf codecs validate locally; ownership and reachability remain caller proofs. */
enum pfs_status pfs_leaf_key_decode(const uint8_t *data, size_t length, uint16_t kind,
                                    const struct pfs_record_context *context,
                                    struct pfs_key *key, uint64_t *end);
void pfs_block_header_encode(uint8_t *data, const struct pfs_block_header *header);
void pfs_block_checksum_encode(uint8_t *data);

uint16_t pfs_get_u16(const uint8_t *p);
uint32_t pfs_get_u32(const uint8_t *p);
uint64_t pfs_get_u64(const uint8_t *p);
void pfs_put_u16(uint8_t *p, uint16_t value);
void pfs_put_u32(uint8_t *p, uint32_t value);
void pfs_put_u64(uint8_t *p, uint64_t value);
void pfs_bytes_copy(void *destination, const void *source, size_t length);
void pfs_bytes_zero(void *data, size_t length);
bool pfs_bytes_are_zero(const void *data, size_t length);
int pfs_bytes_compare(const void *a, size_t a_length, const void *b, size_t b_length);
uint32_t pfs_crc32c_zeroed(const uint8_t *data, size_t length, size_t offset, size_t count);
bool pfs_range_valid(uint64_t first, uint64_t count, uint64_t limit);
bool pfs_allocatable_range(uint64_t first, uint64_t count, uint64_t blocks);
enum pfs_status pfs_context_validate(const struct pfs_record_context *context);
enum pfs_status pfs_reference_validate(const struct pfs_reference *reference,
                                     const struct pfs_record_context *context,
                                     uint16_t expected_type, bool nullable);
enum pfs_status pfs_reference_decode(const uint8_t *data,
                                   const struct pfs_record_context *context,
                                   uint16_t expected_type, bool nullable,
                                   struct pfs_reference *out);
void pfs_reference_encode(uint8_t *data, const struct pfs_reference *reference);
/* slot_length must be the bounded enclosing record length, not remaining block space. */
enum pfs_status pfs_record_header_decode(const uint8_t *data, size_t slot_length,
                                       uint16_t type, size_t minimum_length,
                                       const struct pfs_record_context *context);
enum pfs_status pfs_record_length_validate(size_t length, size_t base_length,
                                         const struct pfs_features *features);
void pfs_record_header_encode(uint8_t *data, uint16_t type, size_t length);

#endif
