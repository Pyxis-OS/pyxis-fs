/* SPDX-License-Identifier: MPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "support.h"
#include "unity.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct test_allocation {
  void *bytes;
  struct test_allocation *next;
};

static struct test_fixture *fixtures;

static void *
allocate(void *context, size_t size, size_t alignment)
{
  struct test_fixture *fixture = context;
  if (fixture->allocation_calls++ >= fixture->fail_after) {
    return NULL;
  }
  struct test_allocation *item = malloc(sizeof(*item));
  if (!item) {
    return NULL;
  }
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  if (posix_memalign(&item->bytes, alignment, size) != 0) {
    free(item);
    return NULL;
  }
  item->next = fixture->allocations;
  fixture->allocations = item;
  uint64_t charged = fixture->memory.used + size;
  if (charged > fixture->peak_memory_bytes) {
    fixture->peak_memory_bytes = charged;
  }
  return item->bytes;
}

static void
release(void *context, void *bytes, size_t size, size_t alignment)
{
  (void)size;
  (void)alignment;
  struct test_fixture *fixture = context;
  struct test_allocation **link = &fixture->allocations;
  while (*link && (*link)->bytes != bytes) {
    link = &(*link)->next;
  }
  if (!*link) {
    /* A core ownership violation must fail, even from inside a callback. */
    abort();
  }
  struct test_allocation *item = *link;
  *link = item->next;
  free(item->bytes);
  free(item);
}

static enum pfs_status
read_blocks(void *context, uint64_t first, uint32_t count, void *buffer)
{
  struct test_fixture *fixture = context;
  fixture->reads++;
  size_t bytes = (size_t)count * PFS_BLOCK_SIZE;
  return pread(fileno(fixture->file), buffer, bytes,
               (off_t)(first * PFS_BLOCK_SIZE)) == (ssize_t)bytes ? PFS_OK : PFS_IO;
}

static enum pfs_status
write_blocks(void *context, uint64_t first, uint32_t count, const void *buffer)
{
  struct test_fixture *fixture = context;
  fixture->writes++;
  size_t bytes = (size_t)count * PFS_BLOCK_SIZE;
  return pwrite(fileno(fixture->file), buffer, bytes,
                (off_t)(first * PFS_BLOCK_SIZE)) == (ssize_t)bytes ? PFS_OK : PFS_IO;
}

static enum pfs_status
flush_blocks(void *context)
{
  struct test_fixture *fixture = context;
  fixture->flushes++;
  /* The task-2 fixture models ordinary synchronous storage, not power loss. */
  return fsync(fileno(fixture->file)) == 0 ? PFS_OK : PFS_IO;
}

FILE *
test_temporary_file(void)
{
  const char *directory = getenv("TMPDIR");
  if (!directory || !*directory) {
    return tmpfile();
  }
  const char suffix[] = "/pyxis-fs-test-XXXXXX";
  size_t length = strlen(directory);
  if (length > SIZE_MAX - sizeof(suffix)) {
    return NULL;
  }
  char *path = malloc(length + sizeof(suffix));
  if (!path) {
    return NULL;
  }
  memcpy(path, directory, length);
  memcpy(path + length, suffix, sizeof(suffix));
  int fd = mkstemp(path);
  if (fd >= 0 && unlink(path) != 0) {
    close(fd);
    fd = -1;
  }
  free(path);
  if (fd < 0) {
    return NULL;
  }
  FILE *file = fdopen(fd, "w+b");
  if (!file) {
    close(fd);
  }
  return file;
}

enum pfs_status
test_fixture_open(struct test_fixture *fixture, uint64_t blocks, uint64_t memory_limit)
{
  if (!fixture || fixture->file || blocks < PFS_POOL_BLOCKS_MIN ||
      blocks > PFS_POOL_BLOCKS_MAX) {
    return PFS_INVALID;
  }
  *fixture = (struct test_fixture){.fail_after = SIZE_MAX};
  fixture->file = test_temporary_file();
  if (!fixture->file) {
    return PFS_IO;
  }
  if (ftruncate(fileno(fixture->file), (off_t)(blocks * PFS_BLOCK_SIZE)) != 0) {
    fclose(fixture->file);
    fixture->file = NULL;
    return PFS_IO;
  }
  struct pfs_geometry geometry = {blocks, PFS_IO_BLOCKS_MAX};
  enum pfs_status status = pfs_block_builder_init(&fixture->builder, fixture,
    &geometry, read_blocks, write_blocks, flush_blocks);
  if (status == PFS_OK) {
    status = pfs_memory_init(&fixture->memory, fixture, allocate, release, memory_limit);
  }
  if (status != PFS_OK) {
    fclose(fixture->file);
    fixture->file = NULL;
    return status;
  }
  fixture->next = fixtures;
  fixtures = fixture;
  return PFS_OK;
}

bool
test_fixture_close(struct test_fixture *fixture)
{
  bool clean = fixture->allocations == NULL && fixture->memory.used == 0;
  while (fixture->allocations) {
    struct test_allocation *item = fixture->allocations;
    fixture->allocations = item->next;
    free(item->bytes);
    free(item);
  }
  if (fixture->file) {
    clean = fclose(fixture->file) == 0 && clean;
  }
  struct test_fixture **link = &fixtures;
  while (*link && *link != fixture) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = fixture->next;
  }
  *fixture = (struct test_fixture){0};
  return clean;
}

void
test_fixtures_cleanup(void)
{
  bool clean = true;
  while (fixtures) {
    clean = test_fixture_close(fixtures) && clean;
  }
  if (!Unity.CurrentTestFailed) {
    TEST_ASSERT_TRUE_MESSAGE(clean, "fixture leaked core allocations or failed close");
  }
}

void
test_put_u16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

void
test_put_u32(uint8_t *bytes, uint32_t value)
{
  for (unsigned i = 0; i < 4; i++) {
    bytes[i] = (uint8_t)(value >> (i * 8));
  }
}

void
test_put_u64(uint8_t *bytes, uint64_t value)
{
  for (unsigned i = 0; i < 8; i++) {
    bytes[i] = (uint8_t)(value >> (i * 8));
  }
}

uint32_t
test_crc32c(const void *bytes, size_t length)
{
  /* Normal (MSB-first) Castagnoli polynomial, with explicit reflected input
   * and output: independent from the core's reflected bit recurrence. */
  const uint8_t *data = bytes;
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; i++) {
    uint8_t reflected = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
      reflected |= ((data[i] >> bit) & 1u) << (7 - bit);
    }
    crc ^= (uint32_t)reflected << 24;
    for (unsigned bit = 0; bit < 8; bit++) {
      crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x1edc6f41) : 0);
    }
  }
  uint32_t reflected = 0;
  for (unsigned bit = 0; bit < 32; bit++) {
    reflected |= ((crc >> bit) & 1u) << (31 - bit);
  }
  return ~reflected;
}

void
test_checksum(uint8_t *bytes, size_t length, size_t offset)
{
  memset(bytes + offset, 0, 4);
  test_put_u32(bytes + offset, test_crc32c(bytes, length));
}

/* Deterministic test adapter; each call gives a different nonzero identity. */
enum pfs_status
test_random(void *context, void *buffer, size_t length)
{
  (void)context;
  static uint64_t counter;
  uint8_t *bytes = buffer;
  uint64_t value = ++counter;
  for (size_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)(value >> ((i % 8) * 8));
  }
  return PFS_OK;
}
