/* SPDX-License-Identifier: MPL-2.0 */
#include "file_tests.h"
#include "failure.h"
#include "plan.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/write.h>
#include <string.h>

#define FILE_TEST_BYTES (300u * PFS_BLOCK_SIZE)

static struct test_fixture seed;
static struct test_failure device;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *file_view, *parent_view, *other_view;
static struct pfs_write_options options;
static struct pfs_trusted_context authority;
static uint8_t expected[FILE_TEST_BYTES], payload[FILE_TEST_BYTES];
static size_t expected_length;
static uint64_t random_sequence;
static unsigned random_mode;
static const struct pfs_object_id file_id = {{4}};
static const struct pfs_volume_id volume_id = {{2}};
static const struct pfs_rights file_rights = {
  .file = PFS_FILE_READ | PFS_FILE_METADATA | PFS_FILE_WRITE | PFS_FILE_RESIZE | PFS_FILE_CHECKPOINT,
};

static enum pfs_status
random_bytes(void *context, void *buffer, size_t length)
{
  uint64_t *sequence = context;
  uint8_t *bytes = buffer;
  ++*sequence;
  if (random_mode) {
    memset(buffer, 0, length);
    if (random_mode == 2 && length) {
      bytes[0] = 4;
    }
    return PFS_OK;
  }
  for (size_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)(*sequence + i * 17u);
  }
  return PFS_OK;
}

static enum pfs_status
source_read(void *context, size_t v, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (v || object != 1 || offset > expected_length || length > expected_length - offset) {
    return PFS_INVALID;
  }
  memcpy(buffer, expected + offset, length);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
build_seed(size_t length, uint64_t quota, uint64_t extent_limit)
{
  expected_length = length;
  for (size_t i = 0; i < length; i++) {
    expected[i] = (uint8_t)(i * 29u + 7u);
  }
  random_sequence = 0;
  random_mode = 0;
  options = (struct pfs_write_options){.extent_limit = extent_limit, .metadata_limit = 64,
    .random = random_bytes, .random_context = &random_sequence};
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
      .file_length = length},
  };
  struct pfs_build_volume specification = {.id = {{2}}, .root_object = {{3}},
    .name = {4, "home"}, .owner = {{5}}, .quota_set = true, .quota = quota,
    .guarantee_set = true, .guarantee = 0, .object_count = 2, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &specification,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
}

static void
acquire_file(const struct pfs_rights *rights, struct pfs_view **out)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &file_id,
    PFS_SCOPE_OBJECT, rights, out));
}

static void
open_device(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, options.extent_limit,
    options.metadata_limit, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  file_view = parent_view = other_view = NULL;
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  authority = (struct pfs_trusted_context){.principal = {{5}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE,
    .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL}};
  acquire_file(&file_rights, &file_view);
}

static void
close_handles(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&other_view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&parent_view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file_view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
}

static void
close_fixture(void)
{
  close_handles();
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
expect_bytes(struct pfs_view *view, const uint8_t *bytes, size_t length)
{
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(view, &metadata));
  TEST_ASSERT_EQUAL_UINT64(length, metadata.size);
  uint8_t buffer[PFS_BLOCK_SIZE];
  for (size_t offset = 0; offset < length; offset += sizeof(buffer)) {
    size_t wanted = length - offset;
    if (wanted > sizeof(buffer)) {
      wanted = sizeof(buffer);
    }
    size_t count = SIZE_MAX;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, offset, buffer, wanted, &count));
    TEST_ASSERT_EQUAL_UINT(wanted, count);
    TEST_ASSERT_EQUAL_MEMORY(bytes + offset, buffer, wanted);
  }
  size_t count = SIZE_MAX;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, length, buffer, 1, &count));
  TEST_ASSERT_EQUAL_UINT(0, count);
}

static void
expect_complete(const struct pfs_write_result *result)
{
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result->completion);
  TEST_ASSERT_EQUAL(PFS_OK, result->operation_status);
  TEST_ASSERT_EQUAL(PFS_OK, result->maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result->health);
}

static void
write_expected(uint64_t offset, const void *bytes, size_t length)
{
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(file_view, offset, bytes, length, &result));
  expect_complete(&result);
  TEST_ASSERT_EQUAL_UINT64(length, result.confirmed_bytes);
  size_t end = (size_t)offset + length;
  if (end > expected_length) {
    memset(expected + expected_length, 0, end - expected_length);
    expected_length = end;
  }
  memcpy(expected + offset, bytes, length);
  expect_bytes(file_view, expected, expected_length);
}

static void
resize_expected(size_t length)
{
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(file_view, length, &result));
  expect_complete(&result);
  TEST_ASSERT_TRUE(result.confirmed_length_valid);
  TEST_ASSERT_EQUAL_UINT64(length, result.confirmed_length);
  if (length > expected_length) {
    memset(expected + expected_length, 0, length - expected_length);
  }
  expected_length = length;
  expect_bytes(file_view, expected, expected_length);
}

static void
cold_reopen_expected(void)
{
  close_handles();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &device.builder.reader, device.memory, &diagnostic));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  const struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  acquire_file(&rights, &file_view);
  expect_bytes(file_view, expected, expected_length);
}

static void
partial_sparse_overwrites_and_old_eof_zeroing_survive_reopen(void)
{
  build_seed(PFS_BLOCK_SIZE + 37, 1024, 64);
  open_device();
  static const uint8_t crossing[] = "partial block crossing";
  write_expected(PFS_BLOCK_SIZE - 9, crossing, sizeof(crossing));
  static const uint8_t tail[] = "sparse tail";
  write_expected(4 * PFS_BLOCK_SIZE + 113, tail, sizeof(tail));
  resize_expected(PFS_BLOCK_SIZE + 11);
  resize_expected(6 * PFS_BLOCK_SIZE + 31);
  write_expected(PFS_BLOCK_SIZE + 13, tail, sizeof(tail));
  resize_expected(3);
  write_expected(29, crossing, sizeof(crossing));
  resize_expected(0);
  resize_expected(2 * PFS_BLOCK_SIZE + 7);
  cold_reopen_expected();
  close_fixture();
}

static void
held_handles_observe_committed_bytes_without_widening_rights(void)
{
  build_seed(PFS_BLOCK_SIZE, 1024, 64);
  open_device();
  struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  acquire_file(&rights, &other_view);
  struct pfs_view_metadata before, after;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(other_view, &before));
  static const uint8_t bytes[] = "coherent held view";
  write_expected(7, bytes, sizeof(bytes));
  resize_expected(2 * PFS_BLOCK_SIZE);
  expect_bytes(other_view, expected, expected_length);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(other_view, &after));
  TEST_ASSERT_EQUAL_MEMORY(&before.identity.object, &after.identity.object,
    sizeof(before.identity.object));
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_write(other_view, 7, bytes, sizeof(bytes), &result));
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_resize(other_view, 0, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  close_fixture();
}

static void
authority_noops_and_invalid_arguments_leave_media_unchanged(void)
{
  build_seed(37, 1024, 64);
  open_device();
  struct pfs_rights rights = {.file = PFS_FILE_WRITE};
  acquire_file(&rights, &other_view);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_write(other_view, 36, "AB", 2, &result));
  TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_resize(other_view, 37, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(other_view, 0, NULL, 0, &result));
  expect_complete(&result);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(file_view, 37, &result));
  expect_complete(&result);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  memset(&result, 0xa5, sizeof(result));
  struct pfs_write_result before = result;
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_write(file_view, 0, NULL, 1, &result));
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_write(file_view, UINT64_MAX, "X", 1, &result));
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_resize(NULL, 0, &result));
  TEST_ASSERT_EQUAL_MEMORY(&before, &result, sizeof(result));
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_view_resize(file_view, PFS_FILE_SIZE_MAX + 1, &result));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_TRUE(result.confirmed_length_valid);
  TEST_ASSERT_EQUAL_UINT64(expected_length, result.confirmed_length);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  expect_bytes(file_view, expected, expected_length);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&other_view));
  rights = (struct pfs_rights){.file = PFS_FILE_RESIZE};
  acquire_file(&rights, &other_view);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(other_view, 7, &result));
  expected_length = 7;
  expect_bytes(file_view, expected, expected_length);
  close_fixture();
}

static void
acquire_parent(enum pfs_grant_scope scope, const struct pfs_rights *rights)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
    scope, rights, &parent_view));
}

static void
create_checks_held_authority_before_publication_and_preserves_policy(void)
{
  build_seed(37, 1024, 64);
  open_device();
  struct pfs_rights rights = {.directory = PFS_DIR_CREATE};
  acquire_parent(PFS_SCOPE_OBJECT, &rights);
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_create_file(parent_view, (const uint8_t *)"denied", 6,
    &file_rights, &other_view, &identity, &result));
  TEST_ASSERT_NULL(other_view);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent_view, (const uint8_t *)"empty", 5,
    NULL, NULL, NULL, &result));
  expect_complete(&result);
  TEST_ASSERT_TRUE(result.namespace_confirmed);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&parent_view));
  rights = (struct pfs_rights){.file = file_rights.file,
    .directory = PFS_DIR_CREATE | PFS_DIR_LOOKUP | PFS_DIR_LIST | PFS_DIR_METADATA,
    .admin = PFS_ADMIN_INSPECT};
  acquire_parent(PFS_SCOPE_SUBTREE, &rights);
  struct pfs_rights child = file_rights;
  child.admin = PFS_ADMIN_INSPECT;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent_view, (const uint8_t *)"new", 3,
    &child, &other_view, &identity, &result));
  expect_complete(&result);
  TEST_ASSERT_TRUE(result.namespace_confirmed);
  TEST_ASSERT_NOT_NULL(other_view);
  TEST_ASSERT_EQUAL(PFS_OBJECT_FILE, identity.kind);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(&file_id, &identity.object, sizeof(file_id)));
  struct pfs_principal_id owner;
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_inspect(other_view, &owner, NULL, 0, &count));
  const struct pfs_principal_id expected_owner = {{5}};
  TEST_ASSERT_EQUAL_MEMORY(&expected_owner, &owner, sizeof(owner));
  TEST_ASSERT_EQUAL_UINT(0, count);
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(other_view, &metadata));
  TEST_ASSERT_EQUAL_UINT64(0, metadata.size);
  writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_EXISTS, pfs_view_create_file(parent_view, (const uint8_t *)"new", 3,
    NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  memset(&result, 0xa5, sizeof(result));
  struct pfs_write_result before = result;
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_create_file(parent_view, (const uint8_t *)"x/y", 3,
    NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_create_file(parent_view, (const uint8_t *)"x", 1,
    &child, NULL, &identity, &result));
  TEST_ASSERT_EQUAL_MEMORY(&before, &result, sizeof(result));
  struct pfs_view_entry entries[4];
  struct pfs_directory_token start = {0}, next;
  bool done;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(parent_view, &start, entries, 4, &count, &done, &next));
  TEST_ASSERT_EQUAL_UINT(3, count);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_MEMORY("empty", entries[0].name.bytes, 5);
  TEST_ASSERT_EQUAL_MEMORY("file", entries[1].name.bytes, 4);
  TEST_ASSERT_EQUAL_MEMORY("new", entries[2].name.bytes, 3);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&other_view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_lookup(parent_view, (const uint8_t *)"empty", 5,
    PFS_SCOPE_OBJECT, &file_rights, &other_view, &identity));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(other_view, &metadata));
  TEST_ASSERT_EQUAL_UINT64(0, metadata.size);
  close_fixture();
}

static void
quota_and_extent_refusal_leave_a_healthy_usable_writer(void)
{
  build_seed(PFS_BLOCK_SIZE, 8, 64);
  open_device();
  memset(payload, 'Q', 8 * PFS_BLOCK_SIZE);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_QUOTA, pfs_view_write(file_view, 0, payload, 8 * PFS_BLOCK_SIZE, &result));
  TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  expect_bytes(file_view, expected, expected_length);
  resize_expected(37);
  close_fixture();
  build_seed(PFS_BLOCK_SIZE, 1024, 1);
  open_device();
  writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_view_write(file_view, 2 * PFS_BLOCK_SIZE, "Q", 1, &result));
  TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  expect_bytes(file_view, expected, expected_length);
  write_expected(0, "Q", 1);
  close_fixture();
}

static void
large_write_and_shrink_report_confirmed_progress(void)
{
  build_seed(0, 1024, 64);
  open_device();
  for (size_t i = 0; i < sizeof(payload); i++) {
    payload[i] = (uint8_t)(i * 13u + 19u);
  }
  struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
    .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + 8, .mode = TEST_FAILURE_BEFORE, .enabled = true};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_write(file_view, 0, payload, sizeof(payload), &result));
  TEST_ASSERT_TRUE(device.triggered);
  TEST_ASSERT_EQUAL(PFS_UNKNOWN, result.completion);
  TEST_ASSERT_GREATER_THAN_UINT64(0, result.confirmed_bytes);
  TEST_ASSERT_LESS_THAN_UINT64(sizeof(payload), result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
  /* This selected final-flush failure leaves only the earlier complete chunk
   * durable. Cold reconstruction is an explicit simulator recovery boundary. */
  expected_length = (size_t)result.confirmed_bytes;
  memcpy(expected, payload, expected_length);
  cold_reopen_expected();
  close_fixture();
  build_seed(sizeof(expected), 1024, 64);
  open_device();
  fault.ordinal = device.ordinals[TEST_FAILURE_FLUSH] + 8;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_resize(file_view, 37, &result));
  TEST_ASSERT_TRUE(device.triggered);
  TEST_ASSERT_EQUAL(PFS_UNKNOWN, result.completion);
  TEST_ASSERT_TRUE(result.confirmed_length_valid);
  TEST_ASSERT_GREATER_THAN_UINT64(37, result.confirmed_length);
  TEST_ASSERT_LESS_THAN_UINT64(sizeof(expected), result.confirmed_length);
  expected_length = (size_t)result.confirmed_length;
  cold_reopen_expected();
  close_fixture();
}

static void
public_write_flush_failures_preserve_progress_and_stop_without_retry(void)
{
  for (uint64_t boundary = 1; boundary <= 6; boundary++) {
    build_seed(PFS_BLOCK_SIZE, 1024, 64);
    open_device();
    memset(payload, 'B', PFS_BLOCK_SIZE);
    struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
      .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + boundary,
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_write(file_view, 0, payload, PFS_BLOCK_SIZE, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL_UINT64(boundary > 2 ? PFS_BLOCK_SIZE : 0, result.confirmed_bytes);
    TEST_ASSERT_EQUAL(boundary == 1 ? PFS_STOPPED : boundary == 2 ? PFS_UNKNOWN : PFS_COMPLETE,
      result.completion);
    TEST_ASSERT_EQUAL(boundary > 2 ? PFS_OK : PFS_IO, result.operation_status);
    TEST_ASSERT_EQUAL(boundary % 2 ? PFS_WRITER_READABLE_STOPPED : PFS_WRITER_ACCESS_STOPPED,
      result.health);
    if (boundary > 2) {
      TEST_ASSERT_EQUAL(PFS_IO, result.maintenance_status);
      memcpy(expected, payload, PFS_BLOCK_SIZE);
    }
    if (boundary % 2) {
      expect_bytes(file_view, expected, expected_length);
    }
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_write(file_view, 0, payload, 1, &result));
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_resize(file_view, expected_length, &result));
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(file_view, &result));
    close_handles();
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
planning_read_and_replacement_write_failures_keep_distinct_health(void)
{
  for (unsigned read_failure = 0; read_failure < 2; read_failure++) {
    build_seed(PFS_BLOCK_SIZE, 1024, 64);
    open_device();
    enum test_failure_event_kind kind = read_failure ? TEST_FAILURE_READ : TEST_FAILURE_WRITE;
    struct test_failure_fault fault = {.kind = kind, .ordinal = device.ordinals[kind] + 1,
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_write(file_view, 13, "R", 1, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(read_failure ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
      result.health);
    if (!read_failure) {
      expect_bytes(file_view, expected, expected_length);
    }
    close_fixture();
  }
}

static void
creation_failures_publish_a_view_only_after_confirmed_namespace_commit(void)
{
  for (uint64_t boundary = 1; boundary <= 6; boundary++) {
    build_seed(37, 1024, 64);
    open_device();
    const struct pfs_rights rights = {.file = file_rights.file,
      .directory = PFS_DIR_CREATE | PFS_DIR_LOOKUP};
    acquire_parent(PFS_SCOPE_SUBTREE, &rights);
    struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
      .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + boundary,
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    struct pfs_write_result result;
    struct pfs_view_identity identity;
    memset(&identity, 0xa5, sizeof(identity));
    struct pfs_view_identity previous = identity;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_create_file(parent_view, (const uint8_t *)"new", 3,
      &file_rights, &other_view, &identity, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL(boundary > 2, result.namespace_confirmed);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    TEST_ASSERT_EQUAL(boundary == 1 ? PFS_STOPPED : boundary == 2 ? PFS_UNKNOWN : PFS_COMPLETE,
      result.completion);
    if (boundary > 2) {
      TEST_ASSERT_NOT_NULL(other_view);
      TEST_ASSERT_EQUAL(PFS_OBJECT_FILE, identity.kind);
      TEST_ASSERT_EQUAL(PFS_OK, result.operation_status);
      TEST_ASSERT_EQUAL(PFS_IO, result.maintenance_status);
      if (boundary % 2) {
        expect_bytes(other_view, expected, 0);
      }
    } else {
      TEST_ASSERT_NULL(other_view);
      TEST_ASSERT_EQUAL_MEMORY(&previous, &identity, sizeof(identity));
    }
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_create_file(parent_view,
      (const uint8_t *)"retry", 5, NULL, NULL, NULL, &result));
    close_handles();
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
token_refusal(const struct pfs_directory_token *token, enum pfs_status status)
{
  struct pfs_view_entry entry, before;
  memset(&entry, 0xa5, sizeof(entry));
  before = entry;
  struct pfs_directory_token next;
  memset(&next, 0xa5, sizeof(next));
  struct pfs_directory_token previous = next;
  size_t count = SIZE_MAX;
  bool done = false;
  TEST_ASSERT_EQUAL(status, pfs_view_directory_live_page(parent_view, token,
    &entry, 1, &count, &done, &next));
  TEST_ASSERT_EQUAL_UINT(0, count);
  TEST_ASSERT_FALSE(done);
  TEST_ASSERT_EQUAL_MEMORY(&before, &entry, sizeof(entry));
  TEST_ASSERT_EQUAL_MEMORY(&previous, &next, sizeof(next));
}

static void
live_directory_tokens_follow_namespace_changes_and_writer_instance(void)
{
  build_seed(37, 1024, 64);
  open_device();
  const struct pfs_rights rights = {.directory = PFS_DIR_CREATE | PFS_DIR_LIST};
  acquire_parent(PFS_SCOPE_OBJECT, &rights);
  struct pfs_directory_token start = {0}, token, next;
  struct pfs_view_entry entry;
  size_t count;
  bool done;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(parent_view, &start,
    &entry, 1, &count, &done, &token));
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_create_file(parent_view,
    (const uint8_t *)"denied", 6, &file_rights, &other_view, &identity, &result));
  TEST_ASSERT_EQUAL(PFS_EXISTS, pfs_view_create_file(parent_view,
    (const uint8_t *)"file", 4, NULL, NULL, NULL, &result));
  write_expected(7, "X", 1);
  resize_expected(128);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(parent_view, &token,
    &entry, 1, &count, &done, &next));
  TEST_ASSERT_EQUAL_UINT(0, count);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent_view,
    (const uint8_t *)"new", 3, NULL, NULL, NULL, &result));
  token_refusal(&token, PFS_CHANGED);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(parent_view, &start,
    &entry, 1, &count, &done, &token));
  struct pfs_directory_token wrong = token;
  wrong.instance[0] ^= 1;
  token_refusal(&wrong, PFS_INVALID);
  wrong = token;
  wrong.directory.bytes[0] ^= 1;
  token_refusal(&wrong, PFS_INVALID);
  wrong = token;
  wrong.volume.bytes[0] ^= 1;
  token_refusal(&wrong, PFS_INVALID);
  close_handles();
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  acquire_file(&file_rights, &file_view);
  acquire_parent(PFS_SCOPE_OBJECT, &rights);
  token_refusal(&token, PFS_INVALID);
  close_fixture();
}

static void
creation_randomness_refuses_zero_and_existing_identity_without_publication(void)
{
  build_seed(37, 1024, 64);
  open_device();
  const struct pfs_rights rights = {.directory = PFS_DIR_CREATE};
  acquire_parent(PFS_SCOPE_OBJECT, &rights);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  for (unsigned mode = 1; mode <= 2; mode++) {
    random_mode = mode;
    uint64_t previous = random_sequence;
    TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_view_create_file(parent_view,
      (const uint8_t *)"new", 3, NULL, NULL, NULL, &result));
    TEST_ASSERT_GREATER_THAN_UINT64(previous, random_sequence);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_FALSE(result.namespace_confirmed);
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  }
  random_mode = 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent_view,
    (const uint8_t *)"new", 3, NULL, NULL, NULL, &result));
  expect_complete(&result);
  close_fixture();
}

static bool reentry_checked, reentry_violation;

static void
observe_public_reentry(struct test_failure *adapter, const struct test_failure_event *event,
                       bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (!before || event->kind != TEST_FAILURE_WRITE || reentry_checked) {
    return;
  }
  reentry_checked = true;
  struct pfs_write_result result;
  memset(&result, 0xa5, sizeof(result));
  struct pfs_write_result previous = result;
  reentry_violation |= pfs_view_write(file_view, 0, "X", 1, &result) != PFS_BUSY;
  reentry_violation |= memcmp(&previous, &result, sizeof(result)) != 0;
  reentry_violation |= pfs_view_resize(file_view, 0, &result) != PFS_BUSY;
  reentry_violation |= memcmp(&previous, &result, sizeof(result)) != 0;
  struct pfs_view *child = NULL;
  struct pfs_view_identity identity;
  memset(&identity, 0xa5, sizeof(identity));
  struct pfs_view_identity prior_identity = identity;
  reentry_violation |= pfs_view_create_file(parent_view, (const uint8_t *)"nested", 6,
    &file_rights, &child, &identity, &result) != PFS_BUSY;
  reentry_violation |= child != NULL || memcmp(&prior_identity, &identity, sizeof(identity)) != 0;
  reentry_violation |= memcmp(&previous, &result, sizeof(result)) != 0;
  reentry_violation |= pfs_pool_close(&pool) != PFS_BUSY;
}

static void
public_mutation_callbacks_cannot_reenter_or_change_outputs(void)
{
  build_seed(PFS_BLOCK_SIZE, 1024, 64);
  open_device();
  const struct pfs_rights rights = {.file = file_rights.file,
    .directory = PFS_DIR_CREATE | PFS_DIR_LOOKUP};
  acquire_parent(PFS_SCOPE_SUBTREE, &rights);
  reentry_checked = reentry_violation = false;
  device.observer = observe_public_reentry;
  write_expected(13, "X", 1);
  TEST_ASSERT_TRUE(reentry_checked);
  TEST_ASSERT_FALSE(reentry_violation);
  device.observer = NULL;
  close_fixture();
}

static uint8_t retained_before[PFS_BLOCK_SIZE], retained_after[PFS_BLOCK_SIZE];
static size_t retained_before_length, retained_after_length;
static uint64_t retained_generation;
static unsigned retained_checks;
static bool retained_violation;

/* Decode each retained durable root separately. These bounded fixtures use one
 * inline mapping; no checker summary or ordinary reader supplies the expected
 * bytes, and no physical allocation location is prescribed by the fixture. */
static bool
retained_payload_matches(struct test_failure *adapter, size_t slot)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
  struct pfs_superblock super;
  if (test_failure_durable_read(adapter, block, 1, bytes) != PFS_OK ||
      pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, block, &super) != PFS_OK) {
    return false;
  }
  struct pfs_block_context context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = super.header.birth, .referring_birth = super.header.birth,
    .pool = super.header.pool, .features = super.features, .reference = super.root};
  struct pfs_pool_root root;
  if (test_failure_durable_read(adapter, super.root.block, 1, bytes) != PFS_OK ||
      pfs_pool_root_decode(bytes, sizeof(bytes), &context, &root) != PFS_OK) {
    return false;
  }
  struct pfs_tree tree;
  struct pfs_tree_context tree_context = {.block = context, .kind = PFS_INDEX_VOLUMES};
  tree_context.block.reference = root.volumes;
  if (test_failure_durable_read(adapter, root.volumes.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree) != PFS_OK || tree.level || tree.count != 1) {
    return false;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = super.header.birth, .containing_birth = tree.header.birth,
    .features = super.features};
  struct pfs_volume_record v;
  if (pfs_volume_record_decode(bytes + tree.slots[0].offset, tree.slots[0].length, &records, &v) != PFS_OK) {
    return false;
  }
  tree_context.kind = PFS_INDEX_OBJECTS;
  tree_context.volume = v.id;
  tree_context.block.reference = v.object_root;
  if (test_failure_durable_read(adapter, v.object_root.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree) != PFS_OK || tree.level) {
    return false;
  }
  records.containing_birth = tree.header.birth;
  struct pfs_object_record object;
  bool found = false;
  for (size_t i = 0; i < tree.count; i++) {
    if (pfs_object_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
        &records, &object) != PFS_OK) {
      return false;
    }
    if (!memcmp(&object.id, &file_id, sizeof(file_id))) {
      found = true;
      break;
    }
  }
  const uint8_t *wanted = super.header.birth <= retained_generation ? retained_before : retained_after;
  size_t length = super.header.birth <= retained_generation ? retained_before_length : retained_after_length;
  if (!found || object.file_length != length || object.inline_extent.count != 1 ||
      test_failure_durable_read(adapter, object.inline_extent.physical_first, 1, bytes) != PFS_OK) {
    return false;
  }
  return !memcmp(bytes, wanted, length);
}

static void
observe_retained_payloads(struct test_failure *adapter, const struct test_failure_event *event,
                          bool before, void *context)
{
  (void)context;
  if (before && event->kind == TEST_FAILURE_WRITE &&
      (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1)) {
    ++retained_checks;
    retained_violation |= !retained_payload_matches(adapter, 0) ||
                          !retained_payload_matches(adapter, 1);
  }
}

static void
prepare_retained_comparison(size_t new_length, uint8_t new_byte)
{
  memcpy(retained_before, expected, expected_length);
  retained_before_length = expected_length;
  memset(retained_after, new_byte, sizeof(retained_after));
  retained_after_length = new_length;
  uint8_t bytes[PFS_BLOCK_SIZE];
  retained_generation = 0;
  for (size_t i = 0; i < 2; i++) {
    uint64_t block = i ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&device, block, 1, bytes));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, block, &super));
    if (super.header.birth > retained_generation) {
      retained_generation = super.header.birth;
    }
  }
  retained_checks = 0;
  retained_violation = false;
  device.observer = observe_retained_payloads;
}

static void
public_overwrite_reuse_and_shrink_preserve_both_payloads_through_maintenance(void)
{
  build_seed(PFS_BLOCK_SIZE, 1024, 64);
  open_device();
  for (unsigned i = 0; i < 3; i++) {
    uint8_t byte = (uint8_t)('B' + i);
    prepare_retained_comparison(PFS_BLOCK_SIZE, byte);
    memset(payload, byte, PFS_BLOCK_SIZE);
    write_expected(0, payload, PFS_BLOCK_SIZE);
    TEST_ASSERT_FALSE(retained_violation);
    TEST_ASSERT_EQUAL_UINT(3, retained_checks);
  }
  prepare_retained_comparison(37, 'D');
  resize_expected(37);
  TEST_ASSERT_FALSE(retained_violation);
  TEST_ASSERT_EQUAL_UINT(3, retained_checks);
  device.observer = NULL;
  cold_reopen_expected();
  close_fixture();
}

void
run_file_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(partial_sparse_overwrites_and_old_eof_zeroing_survive_reopen);
  RUN_TEST(held_handles_observe_committed_bytes_without_widening_rights);
  RUN_TEST(authority_noops_and_invalid_arguments_leave_media_unchanged);
  RUN_TEST(create_checks_held_authority_before_publication_and_preserves_policy);
  RUN_TEST(quota_and_extent_refusal_leave_a_healthy_usable_writer);
  RUN_TEST(large_write_and_shrink_report_confirmed_progress);
  RUN_TEST(public_write_flush_failures_preserve_progress_and_stop_without_retry);
  RUN_TEST(planning_read_and_replacement_write_failures_keep_distinct_health);
  RUN_TEST(public_overwrite_reuse_and_shrink_preserve_both_payloads_through_maintenance);
  RUN_TEST(creation_failures_publish_a_view_only_after_confirmed_namespace_commit);
  RUN_TEST(live_directory_tokens_follow_namespace_changes_and_writer_instance);
  RUN_TEST(creation_randomness_refuses_zero_and_existing_identity_without_publication);
  RUN_TEST(public_mutation_callbacks_cannot_reenter_or_change_outputs);
}
