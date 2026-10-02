/* SPDX-License-Identifier: MPL-2.0 */
#include "live_tests.h"
#include "support.h"
#include "unity.h"
#include "failure.h"
#include "plan.h"
#include <pyxis_fs/write.h>
#include <pyxis_fs/build.h>
#include <string.h>

static struct test_fixture fixture;
static struct test_failure device;
static bool failure_device;
static const uint8_t payload[] = "live reads preserve held authority";
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *file_view;
static struct pfs_view *directory_view;
static struct pfs_trusted_context authority;
static pfs_read_blocks_fn original_read;
static pfs_allocate_fn original_allocate;
static pfs_free_fn original_free;
static bool hook_enabled;
static bool hook_closing_pool;
static bool read_failure;
static unsigned hook_calls;

static enum pfs_status
source_read(void *context, size_t volume_index, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (volume_index || object != 1 || offset > sizeof(payload) || length > sizeof(payload) - offset) {
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
add_explicit_directory_checkpoint(const struct pfs_volume_record *volume_record)
{
  uint8_t data[PFS_BLOCK_SIZE];
  const struct pfs_reference *reference = &volume_record->grant_root;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, reference->block,
                                        1, data, sizeof(data)));
  struct pfs_tree_context context = {
    .block = {
      .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
      .pool = {{1}}, .reference = *reference,
    },
    .kind = PFS_INDEX_GRANTS, .volume = {{2}},
  };
  struct pfs_tree tree;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(data, sizeof(data), &context, &tree));
  TEST_ASSERT_EQUAL(1, tree.count);
  struct pfs_record_context records_context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .containing_birth = 1,
  };
  struct pfs_grant_record grant;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_decode(data + tree.slots[0].offset,
                       tree.slots[0].length, &records_context, &grant));
  grant.directory_rights |= PFS_DIR_CHECKPOINT;
  uint8_t encoded[PFS_GRANT_RECORD_SIZE];
  struct pfs_encoded_record record = {.data = encoded};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_encode(encoded, sizeof(encoded),
                                          &records_context, &grant, &record.length));
  uint8_t output[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(output, sizeof(output), &context, &tree, &record));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, reference->block, 1,
                                         output, sizeof(output)));
}

/* Ordinary live access uses the same admitted writer as mutation tests. */
static void
open_bridge_fixture_on(bool use_failure_device)
{
  failure_device = use_failure_device;
  hook_enabled = false;
  hook_closing_pool = false;
  read_failure = false;
  hook_calls = 0;
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  file_view = NULL;
  directory_view = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
     .file_length = sizeof(payload)},
  };
  struct pfs_build_volume specification = {
    .id = {{2}}, .root_object = {{3}}, .name = {4, "home"}, .owner = {{5}},
    .object_count = 2, .objects = objects,
    .quota_set = true, .quota = 128, .guarantee_set = true,
  };
  struct pfs_build_spec spec = {
    .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &specification,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024,
  };
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&fixture.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &fixture.builder, &source));
  add_explicit_directory_checkpoint(&plan.volumes[0]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  struct pfs_write_open_result opening;
  const struct pfs_write_options options = {
    .extent_limit = 16, .metadata_limit = 16, .random = test_random,
  };
  struct pfs_block_builder *builder = &fixture.builder;
  struct pfs_memory *memory = &fixture.memory;
  if (failure_device) {
    struct pfs_plan_limits limits;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, options.extent_limit,
      options.metadata_limit, 1, &limits));
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
      (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &fixture.builder.reader));
    builder = &device.builder;
    memory = device.memory;
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, builder,
    memory, &options, &opening));
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volume));
  authority = (struct pfs_trusted_context) {
    .principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
    .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL},
  };
}

static void
open_bridge_fixture(void)
{
  open_bridge_fixture_on(false);
}

static void
acquire_read_views(void)
{
  struct pfs_rights file = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)"file", 4, PFS_SCOPE_OBJECT, &file, &file_view));
  struct pfs_rights directory = {
    .file = PFS_FILE_READ | PFS_FILE_METADATA,
    .directory = PFS_DIR_LIST | PFS_DIR_LOOKUP | PFS_DIR_METADATA,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
      PFS_SCOPE_SUBTREE, &directory, &directory_view));
}

static void
close_bridge_fixture(void)
{
  hook_enabled = false;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file_view, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&directory_view, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_FALSE(pool.writer_busy);
  TEST_ASSERT_NULL(pool.writer);
  if (failure_device) {
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_TRUE(test_failure_close(&device));
  }
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
checkpoint_authority_does_not_imply_read_lookup_or_metadata(void)
{
  open_bridge_fixture();
  authority.root = (struct pfs_object_id){{4}};
  authority.scope = PFS_SCOPE_OBJECT;
  authority.ceiling = (struct pfs_rights){.file = PFS_FILE_CHECKPOINT};
  struct pfs_rights rights = {.file = PFS_FILE_CHECKPOINT};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
                                         PFS_SCOPE_OBJECT, &rights, &file_view));
  uint64_t writes = fixture.writes;
  uint64_t flushes = fixture.flushes;
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(file_view, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_FALSE(result.namespace_confirmed);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(flushes, fixture.flushes);
  uint8_t bytes[8];
  size_t count = 123;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL(0, count);
  struct pfs_view_metadata metadata;
  memset(&metadata, 0xa5, sizeof(metadata));
  struct pfs_view_metadata before = metadata;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_metadata(file_view, &metadata));
  TEST_ASSERT_EQUAL_MEMORY(&before, &metadata, sizeof(metadata));
  authority.root = (struct pfs_object_id){{3}};
  authority.ceiling = (struct pfs_rights){.directory = PFS_DIR_CHECKPOINT};
  rights = (struct pfs_rights){.directory = PFS_DIR_CHECKPOINT};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
                      PFS_SCOPE_OBJECT, &rights, &directory_view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(directory_view, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  struct pfs_view *child = NULL;
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_lookup(directory_view, (const uint8_t *)"file", 4,
                      PFS_SCOPE_OBJECT, &rights, &child, &identity));
  TEST_ASSERT_NULL(child);
  rights = (struct pfs_rights){.file = PFS_FILE_WRITE};
  authority.root = (struct pfs_object_id){{4}};
  authority.ceiling = rights;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
                          PFS_SCOPE_OBJECT, &rights, &child));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&child, &(struct pfs_view_close_result){0}));
  close_bridge_fixture();
}

static void
live_mode_separates_immutable_diagnostics(void)
{
  open_bridge_fixture();
  acquire_read_views();
  struct pfs_volume_record record;
  struct pfs_object_record object;
  const struct pfs_object_id file = {{4}};
  TEST_ASSERT_EQUAL(PFS_READ_ONLY, pfs_volume_diagnostic_metadata(&volume, &record));
  TEST_ASSERT_EQUAL(PFS_READ_ONLY, pfs_volume_diagnostic_object(&volume, &file, &object));
  struct pfs_volume diagnostic_volume = {0};
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_READ_ONLY, pfs_pool_diagnostic_volume_open(&pool, &id, &diagnostic_volume));
  struct pfs_view_directory *cursor = NULL;
  TEST_ASSERT_EQUAL(PFS_READ_ONLY, pfs_view_directory_open(directory_view, &cursor));
  TEST_ASSERT_NULL(cursor);
  struct pfs_view_entry entries[2];
  size_t count;
  bool done;
  uint64_t next;
  TEST_ASSERT_EQUAL(PFS_READ_ONLY, pfs_view_directory_page(directory_view, 0, entries, 2, &count, &done, &next));
  struct pfs_directory_token start = {0}, token;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(directory_view, &start, entries, 2, &count, &done, &token));
  TEST_ASSERT_EQUAL(1, count);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_MEMORY("file", entries[0].name.bytes, 4);
  close_bridge_fixture();
}

static void
publication_failures_enforce_real_stopped_read_states(void)
{
  uint64_t write_cuts[2] = {0};
  open_bridge_fixture_on(true);
  struct pfs_view *mutation = NULL;
  const struct pfs_object_id file = {{4}};
  const struct pfs_rights write_rights = {.file = PFS_FILE_WRITE};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &file,
    PFS_SCOPE_OBJECT, &write_rights, &mutation));
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  test_failure_trace_reset(&device);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(mutation, 0, "X", 1, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind != TEST_FAILURE_WRITE) {
      continue;
    }
    bool slot = event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1;
    if (!write_cuts[slot]) {
      write_cuts[slot] = event->ordinal - writes;
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT64(0, write_cuts[0]);
  TEST_ASSERT_GREATER_THAN_UINT64(0, write_cuts[1]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&mutation, &(struct pfs_view_close_result){0}));
  close_bridge_fixture();

  for (unsigned slot_failure = 0; slot_failure < 2; slot_failure++) {
    open_bridge_fixture_on(true);
    acquire_read_views();
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &file,
      PFS_SCOPE_OBJECT, &write_rights, &mutation));
    struct test_failure_fault fault = {.kind = TEST_FAILURE_WRITE,
      .ordinal = device.ordinals[TEST_FAILURE_WRITE] + write_cuts[slot_failure],
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_write(mutation, 0, "X", 1, &result));
    TEST_ASSERT_TRUE(device.triggered);
    enum pfs_writer_health health = slot_failure ? PFS_WRITER_ACCESS_STOPPED :
                                                  PFS_WRITER_READABLE_STOPPED;
    TEST_ASSERT_EQUAL(health, result.health);
    TEST_ASSERT_EQUAL(slot_failure ? PFS_UNKNOWN : PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    struct pfs_writer_status status;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
    TEST_ASSERT_EQUAL(health, status.health);
    TEST_ASSERT_EQUAL(PFS_IO, status.failure);
    /* Removing the adapter fault does not heal this writer instance. */
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &(struct test_failure_fault){0}));
    uint8_t bytes[sizeof(payload)];
    size_t count = 123;
    if (!slot_failure) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
      TEST_ASSERT_EQUAL_UINT(sizeof(payload), count);
      TEST_ASSERT_EQUAL_MEMORY(payload, bytes, sizeof(payload));
    } else {
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED,
        pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
      TEST_ASSERT_EQUAL_UINT(123, count);
      struct pfs_view_metadata metadata;
      memset(&metadata, 0xa5, sizeof(metadata));
      struct pfs_view_metadata before = metadata;
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_metadata(file_view, &metadata));
      TEST_ASSERT_EQUAL_MEMORY(&before, &metadata, sizeof(metadata));
      struct pfs_view *child = NULL;
      struct pfs_view_identity identity;
      struct pfs_rights rights = {.file = PFS_FILE_READ};
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_lookup(directory_view,
        (const uint8_t *)"file", 4, PFS_SCOPE_OBJECT, &rights, &child, &identity));
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_acquire_path(&volume, &authority,
        (const uint8_t *)"file", 4, PFS_SCOPE_OBJECT, &rights, &child));
      TEST_ASSERT_NULL(child);
    }
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(file_view, &result));
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(health, result.health);
    writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&mutation, &(struct pfs_view_close_result){0}));
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    close_bridge_fixture();
  }
}

static void
callback_reentry(void)
{
  if (!hook_enabled) {
    return;
  }
  ++hook_calls;
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_pool_open(&pool, &fixture.builder.reader,
                                         &fixture.memory, &diagnostic));
  struct pfs_volume extra = {0};
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_pool_volume_open(&pool, &id, &extra));
  if (hook_closing_pool) {
    return;
  }
  struct pfs_view *target = file_view ? file_view : directory_view;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_volume_close(&volume));
  struct pfs_view_metadata metadata;
  memset(&metadata, 0xa5, sizeof(metadata));
  struct pfs_view_metadata before = metadata;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_metadata(target, &metadata));
  TEST_ASSERT_EQUAL_MEMORY(&before, &metadata, sizeof(metadata));
  uint8_t bytes[8];
  size_t count = 456;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_read(target, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL(456, count);
  struct pfs_write_result result;
  memset(&result, 0xa5, sizeof(result));
  struct pfs_write_result previous = result;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_checkpoint(target, &result));
  TEST_ASSERT_EQUAL_MEMORY(&previous, &result, sizeof(result));
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_close(&target, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_NOT_NULL(target);
  struct pfs_view *child = NULL;
  struct pfs_view_identity identity;
  struct pfs_rights rights = {.file = PFS_FILE_READ};
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_lookup(directory_view, (const uint8_t *)"file", 4,
                            PFS_SCOPE_OBJECT, &rights, &child, &identity));
  TEST_ASSERT_NULL(child);
  struct pfs_access_result evaluated;
  const struct pfs_object_id target_id = {{4}};
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_access_evaluate(&volume, &authority, &target_id,
                                PFS_SCOPE_OBJECT, &rights, &evaluated));
  struct pfs_view_entry entries[2];
  bool done = false;
  uint64_t next = 789;
  TEST_ASSERT_EQUAL(PFS_BUSY, pfs_view_directory_page(directory_view, 0, entries, 2,
                                                  &count, &done, &next));
  TEST_ASSERT_EQUAL(456, count);
  TEST_ASSERT_FALSE(done);
  TEST_ASSERT_EQUAL_UINT64(789, next);
}

static enum pfs_status
hook_read(void *context, uint64_t first, uint32_t count, void *buffer)
{
  callback_reentry();
  return read_failure ? PFS_IO : original_read(context, first, count, buffer);
}

static void *
hook_allocate(void *context, size_t size, size_t alignment)
{
  callback_reentry();
  return original_allocate(context, size, alignment);
}

static void
hook_free(void *context, void *data, size_t size, size_t alignment)
{
  callback_reentry();
  original_free(context, data, size, alignment);
}

static void
install_hooks(void)
{
  original_read = fixture.builder.reader.read;
  original_allocate = fixture.memory.allocate;
  original_free = fixture.memory.free;
  fixture.builder.reader.read = hook_read;
  fixture.memory.allocate = hook_allocate;
  fixture.memory.free = hook_free;
}

static void
callbacks_cannot_reenter_reads_authority_or_lifetime_changes(void)
{
  open_bridge_fixture();
  acquire_read_views();
  install_hooks();
  hook_enabled = true;
  uint8_t bytes[sizeof(payload)];
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL_MEMORY(payload, bytes, sizeof(payload));
  TEST_ASSERT_GREATER_THAN(0, hook_calls);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file_view, &(struct pfs_view_close_result){0}));
  hook_closing_pool = true;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&directory_view, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  hook_enabled = false;
  TEST_ASSERT_FALSE(pool.writer_busy);
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
read_io_failure_stops_access_without_automatic_retry(void)
{
  open_bridge_fixture();
  acquire_read_views();
  install_hooks();
  read_failure = true;
  uint8_t bytes[sizeof(payload)];
  size_t count = 456;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL(0, count);
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, status.health);
  TEST_ASSERT_EQUAL(PFS_IO, status.failure);
  read_failure = false;
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_read(file_view, 0, bytes, sizeof(bytes), &count));
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(file_view, &result));
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
  close_bridge_fixture();
}

void
run_live_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(checkpoint_authority_does_not_imply_read_lookup_or_metadata);
  RUN_TEST(live_mode_separates_immutable_diagnostics);
  RUN_TEST(publication_failures_enforce_real_stopped_read_states);
  RUN_TEST(callbacks_cannot_reenter_reads_authority_or_lifetime_changes);
  RUN_TEST(read_io_failure_stops_access_without_automatic_retry);
}
