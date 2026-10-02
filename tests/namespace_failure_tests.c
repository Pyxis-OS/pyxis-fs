/* SPDX-License-Identifier: MPL-2.0 */
#include "failure.h"
#include "plan.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/write.h>
#include <string.h>

void run_namespace_failure_tests(void);

static struct test_fixture seed;
static struct test_failure device;
static struct test_failure_flush_cut rename_cuts[TEST_FAILURE_EVENTS_MAX];
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *source_parent, *destination_parent, *victim;
static const struct pfs_volume_id volume_id = {{2}};
static const struct pfs_object_id root_id = {{3}}, source_id = {{6}}, victim_id = {{7}};
static const struct pfs_write_options options = {
  .extent_limit = 16, .metadata_limit = 32, .random = test_random,
};

static void
expected_bytes(uint8_t *bytes, size_t length, uint64_t offset, bool displaced)
{
  for (size_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)((offset + i) * 13u + (displaced ? 191u : 17u));
  }
}

static enum pfs_status
source_read(void *context, size_t v, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (v || (object != 3 && object != 4) || offset > PFS_BLOCK_SIZE ||
      length > PFS_BLOCK_SIZE - offset) {
    return PFS_INVALID;
  }
  expected_bytes(buffer, length, offset, object == 4);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
open_fixture(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {3, "src"}, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{5}}, .parent = 0, .name = {3, "dst"}, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{6}}, .parent = 1, .name = {4, "item"}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE},
    {.id = {{7}}, .parent = 2, .name = {4, "item"}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE},
  };
  struct pfs_build_volume specification = {.id = {{2}}, .root_object = {{3}},
    .name = {4, "home"}, .owner = {{8}}, .quota_set = true, .quota = 128,
    .guarantee_set = true, .object_count = 5, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &specification,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN,
    options.extent_limit, options.metadata_limit, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  source_parent = destination_parent = victim = NULL;
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  struct pfs_trusted_context context = {.principal = {{8}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE, .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, 0}};
  const struct pfs_rights source_rights = {.directory = PFS_DIR_REMOVE};
  const struct pfs_rights destination_rights = {.directory = PFS_DIR_CREATE | PFS_DIR_REPLACE};
  const struct pfs_rights read_rights = {.file = PFS_FILE_READ};
  const struct pfs_object_id source_directory = {{4}}, destination_directory = {{5}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &source_directory,
    PFS_SCOPE_OBJECT, &source_rights, &source_parent));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &destination_directory,
    PFS_SCOPE_OBJECT, &destination_rights, &destination_parent));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &victim_id,
    PFS_SCOPE_OBJECT, &read_rights, &victim));
  test_failure_trace_reset(&device);
}

static enum pfs_status
rename_file(struct pfs_write_result *result)
{
  return pfs_view_rename(source_parent, (const uint8_t *)"item", 4,
    destination_parent, (const uint8_t *)"item", 4, true, result);
}

static void
close_handles(void)
{
  struct pfs_view_close_result closed;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&victim, &closed));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&source_parent, &closed));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&destination_parent, &closed));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
}

static void
expect_payload(const struct pfs_object_id *id, bool displaced)
{
  uint8_t actual[PFS_BLOCK_SIZE], expected[PFS_BLOCK_SIZE];
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&volume, id,
    0, actual, sizeof(actual), &count));
  TEST_ASSERT_EQUAL_UINT(PFS_BLOCK_SIZE, count);
  expected_bytes(expected, sizeof(expected), 0, displaced);
  TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));
}

static void
expect_namespace(bool moved, bool cleaned)
{
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &device.builder.reader, device.memory, &diagnostic));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &volume_id, &volume));
  struct pfs_object_record object;
  enum pfs_status status = pfs_volume_diagnostic_resolve(&volume, &root_id,
    (const uint8_t *)"src/item", 8, &object);
  TEST_ASSERT_EQUAL(moved ? PFS_NOT_FOUND : PFS_OK, status);
  if (!moved) {
    TEST_ASSERT_EQUAL_MEMORY(&source_id, &object.id, sizeof(source_id));
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_resolve(&volume, &root_id,
    (const uint8_t *)"dst/item", 8, &object));
  TEST_ASSERT_EQUAL_MEMORY(moved ? &source_id : &victim_id, &object.id, sizeof(source_id));
  expect_payload(&source_id, false);
  if (moved && cleaned) {
    TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_volume_diagnostic_object(&volume, &victim_id, &object));
  } else {
    expect_payload(&victim_id, true);
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
}

static void
rename_flush_failures_preserve_atomic_namespace_and_confirmed_progress(void)
{
  open_fixture();
  uint64_t first_flush = device.ordinals[TEST_FAILURE_FLUSH];
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, rename_file(&result));
  size_t cut_count;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, first_flush,
    rename_cuts, TEST_FAILURE_EVENTS_MAX, &cut_count));
  TEST_ASSERT_GREATER_THAN_UINT(0, cut_count);
  size_t user_slot_flush = SIZE_MAX;
  for (size_t i = 0; i < cut_count; i++) {
    if (!rename_cuts[i].publication && rename_cuts[i].publishes) {
      user_slot_flush = i;
      break;
    }
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, user_slot_flush);
  close_handles();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));

  for (size_t i = 0; i <= cut_count; i++) {
    open_fixture();
    bool durable_unknown = i == cut_count;
    const struct test_failure_flush_cut *cut = &rename_cuts[durable_unknown ? user_slot_flush : i];
    bool confirmed = cut->publication != 0;
    struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
      .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + cut->ordinal,
      .mode = durable_unknown ? TEST_FAILURE_FLUSH_PREFIX : TEST_FAILURE_BEFORE,
      .prefix = SIZE_MAX, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, rename_file(&result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL(confirmed, result.namespace_confirmed);
    TEST_ASSERT_EQUAL(confirmed ? PFS_COMPLETE : cut->publishes ? PFS_UNKNOWN : PFS_STOPPED,
      result.completion);
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
      result.health);
    TEST_ASSERT_EQUAL(confirmed ? PFS_OK : PFS_IO, result.operation_status);
    if (confirmed) {
      TEST_ASSERT_EQUAL(PFS_IO, result.maintenance_status);
    }
    uint8_t byte = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_RECOVERY_REQUIRED : PFS_OK,
      pfs_view_read(victim, 0, &byte, 1, &count));
    if (!cut->publishes) {
      TEST_ASSERT_EQUAL_UINT8(191, byte);
    }
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, rename_file(&result));
    close_handles();
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
    bool moved = confirmed || durable_unknown;
    expect_namespace(moved, false);
    struct pfs_write_open_result opening;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
      device.memory, &options, &opening));
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
    expect_namespace(moved, true);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
rename_planning_and_maintenance_reads_stop_access_without_erasing_commit(void)
{
  open_fixture();
  uint64_t first_read = device.ordinals[TEST_FAILURE_READ];
  uint64_t first_flush = device.ordinals[TEST_FAILURE_FLUSH];
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, rename_file(&result));
  size_t cut_count;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, first_flush,
    rename_cuts, TEST_FAILURE_EVENTS_MAX, &cut_count));
  uint64_t user_confirmed_flush = 0;
  for (size_t i = 0; i < cut_count; i++) {
    if (!rename_cuts[i].publication && rename_cuts[i].publishes) {
      user_confirmed_flush = first_flush + rename_cuts[i].ordinal;
      break;
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT64(first_flush, user_confirmed_flush);
  uint64_t maintenance_read = 0;
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_READ && event->flush_ordinal >= user_confirmed_flush) {
      maintenance_read = event->ordinal - first_read;
      break;
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT64(0, maintenance_read);
  close_handles();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
  for (unsigned maintenance = 0; maintenance < 2; maintenance++) {
    open_fixture();
    struct test_failure_fault fault = {.kind = TEST_FAILURE_READ,
      .ordinal = device.ordinals[TEST_FAILURE_READ] + (maintenance ? maintenance_read : 1),
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, rename_file(&result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL(maintenance != 0, result.namespace_confirmed);
    TEST_ASSERT_EQUAL(maintenance ? PFS_COMPLETE : PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
    TEST_ASSERT_EQUAL(maintenance ? PFS_OK : PFS_IO, result.operation_status);
    TEST_ASSERT_EQUAL(maintenance ? PFS_IO : PFS_OK, result.maintenance_status);
    close_handles();
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
    expect_namespace(maintenance != 0, false);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

void
run_namespace_failure_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(rename_flush_failures_preserve_atomic_namespace_and_confirmed_progress);
  RUN_TEST(rename_planning_and_maintenance_reads_stop_access_without_erasing_commit);
}
