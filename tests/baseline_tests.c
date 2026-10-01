/* SPDX-License-Identifier: MPL-2.0 */
#include "support.h"
#include "unity.h"
#include <pyxis_fs/access.h>
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <string.h>

void run_baseline_tests(void);
static struct test_fixture fixture;
static const uint8_t payload[] = "independently expected file bytes\n";

static enum pfs_status
source_read(void *context, size_t volume, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (volume || object != 1 || offset > sizeof(payload) || length > sizeof(payload) - offset) {
    return PFS_INVALID;
  }
  memcpy(buffer, payload + offset, length);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
build_image(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
     .file_length = sizeof(payload)},
  };
  struct pfs_build_volume volume = {
    .id = {{2}}, .root_object = {{3}}, .name = {4, "home"}, .owner = {{5}},
    .object_count = 2, .objects = objects,
  };
  struct pfs_build_spec spec = {
    .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}}, .volume_count = 1, .volumes = &volume,
  };
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&fixture.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &fixture.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
}

static void
crc_known_answer(void)
{
  static const char input[] = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xe3069283, pfs_crc32c(input, 9));
  TEST_ASSERT_EQUAL_HEX32(0xe3069283, test_crc32c(input, 9));
  TEST_ASSERT_EQUAL_HEX32(0, pfs_crc32c(input, 0));
}

static void
read_list_authority_and_lifetimes(void)
{
  build_image();
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_EQUAL_UINT64(2, check.state[0].objects);
  struct pfs_pool pool = {0};
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &fixture.builder.reader, &fixture.memory, &diagnostic));
  struct pfs_volume volume = {0};
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &id, &volume));
  struct pfs_trusted_context context = {
    .principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
    .ceiling = {PFS_FILE_READ | PFS_FILE_METADATA, PFS_DIR_LIST | PFS_DIR_LOOKUP, 0},
  };
  struct pfs_rights rights = {.file = PFS_FILE_READ};
  struct pfs_view *file = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &context,
    (const uint8_t *)"file", 4, PFS_SCOPE_OBJECT, &rights, &file));
  uint8_t bytes[sizeof(payload) + 5];
  size_t count = 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(file, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL_UINT(sizeof(payload), count);
  TEST_ASSERT_EQUAL_MEMORY(payload, bytes, sizeof(payload));
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_metadata(file, &metadata));
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file));
  TEST_ASSERT_NULL(file);
  rights.directory = PFS_DIR_LIST;
  rights.file = 0;
  struct pfs_view *root = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &context.root,
    PFS_SCOPE_SUBTREE, &rights, &root));
  struct pfs_view_entry entries[2];
  uint64_t next;
  bool done;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_page(root, 0, entries, 2, &count, &done, &next));
  TEST_ASSERT_EQUAL_UINT(1, count);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_UINT(4, entries[0].name.length);
  TEST_ASSERT_EQUAL_MEMORY("file", entries[0].name.bytes, 4);
  TEST_ASSERT_EQUAL(PFS_OBJECT_FILE, entries[0].kind);
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_lookup(root, (const uint8_t *)"file", 4,
    PFS_SCOPE_OBJECT, &rights, &file, &identity));
  TEST_ASSERT_NULL(file);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&root));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
}

static void
failed_open_releases_memory(void)
{
  build_image();
  fixture.fail_after = fixture.allocation_calls;
  struct pfs_pool pool = {0};
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_NO_MEMORY, pfs_pool_open(&pool, &fixture.builder.reader,
    &fixture.memory, &diagnostic));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
}

void
run_baseline_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(crc_known_answer);
  RUN_TEST(read_list_authority_and_lifetimes);
  RUN_TEST(failed_open_releases_memory);
}
