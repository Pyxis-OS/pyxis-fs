/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_TEST_SUPPORT_H
#define PFS_TEST_SUPPORT_H

#include <pyxis_fs/platform.h>
#include <stdio.h>

struct test_allocation;
/* Fixtures must outlive the test case (use static storage). Teardown also closes
 * them after Unity aborts a case. Allocation failures are adapter-owned. */
struct test_fixture {
  FILE *file;
  struct pfs_block_builder builder;
  struct pfs_memory memory;
  struct test_allocation *allocations;
  struct test_fixture *next;
  size_t allocation_calls;
  size_t fail_after;
  uint64_t reads;
  uint64_t writes;
  uint64_t flushes;
};

enum pfs_status test_fixture_open(struct test_fixture *fixture, uint64_t blocks,
                                  uint64_t memory_limit);
bool test_fixture_close(struct test_fixture *fixture);
void test_fixtures_cleanup(void);
/* Independent test encoding/checksum helpers; never use production codecs to
 * repair a malformed fixture's enclosing checksum or define a known answer. */
void test_put_u16(uint8_t *bytes, uint16_t value);
void test_put_u32(uint8_t *bytes, uint32_t value);
void test_put_u64(uint8_t *bytes, uint64_t value);
uint32_t test_crc32c(const void *bytes, size_t length);
void test_checksum(uint8_t *bytes, size_t length, size_t offset);

#endif
