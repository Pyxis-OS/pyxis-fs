/* SPDX-License-Identifier: MPL-2.0 */
#include "publication_tests.h"
#include "failure.h"
#include "writer.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <string.h>

static struct test_fixture seed;
static struct test_failure device, recovered;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *view;
static struct pfs_edit_workspace editor;
static struct pfs_edit_slot object_slot;
static struct pfs_reference retired[1];
static uint8_t data[PFS_BLOCK_SIZE], encoded[PFS_BLOCK_SIZE], source_block[PFS_BLOCK_SIZE];
static struct pfs_batch_block replacements[2];
static struct check_claim removed[2], added[2];
static unsigned payload_checks;
static bool payload_violation;
static struct pfs_write_options options = {16, 16};

static enum pfs_status
source_read(void *context, size_t v, size_t object, uint64_t offset, void *buffer, size_t length)
{
  (void)context;
  if (v || object != 1 || offset + length > PFS_BLOCK_SIZE) {
    return PFS_INVALID;
  }
  memset(buffer, 'A', length);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
build_seed(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE},
  };
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .guarantee_set = true, .quota_set = true, .quota = 64,
    .object_count = 2, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &v,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
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
  view = NULL;
  struct pfs_write_open_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, &result));
  TEST_ASSERT_EQUAL_UINT64(1, result.confirmed_generation);
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volume));
  struct pfs_trusted_context context = {.principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
    .ceiling = {PFS_FILE_READ | PFS_FILE_CHECKPOINT, PFS_DIR_LOOKUP, 0}};
  struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_CHECKPOINT};
  const struct pfs_object_id file = {{4}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &file, PFS_SCOPE_OBJECT, &rights, &view));
}

static void
close_device(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
}

static void
expect_read(uint8_t expected)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  size_t count = 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL_UINT(PFS_BLOCK_SIZE, count);
  for (size_t i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_UINT8(expected, bytes[i]);
  }
}

/* The fixture editor changes an existing one-block file through the actual COW
 * editor and private publisher. Public write/resize semantics arrive separately. */
static struct pfs_batch *
overwrite(uint8_t byte)
{
  struct pfs_batch *batch = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_prepare(&pool, &batch));
  struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
  batch->volume = state->volumes[0];
  struct pfs_reference object_root = batch->volume.record.object_root;
  struct pfs_tree_context context = {.kind = PFS_INDEX_OBJECTS, .volume = {{2}},
    .block = {.block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = batch->generation,
      .referring_birth = batch->generation, .reference = object_root,
      .pool = {{1}}, .features = state->candidate.superblock.features}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&device.builder.reader, object_root.block, 1,
    source_block, sizeof(source_block)));
  struct pfs_tree tree;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(source_block, sizeof(source_block), &context, &tree));
  TEST_ASSERT_EQUAL_UINT(0, tree.level);
  struct pfs_record_context record_context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = batch->generation, .containing_birth = tree.header.birth,
    .features = context.block.features};
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_decode(source_block + tree.slots[1].offset,
    tree.slots[1].length, &record_context, &object));
  TEST_ASSERT_EQUAL_UINT8(4, object.id.bytes[0]);
  uint64_t old_data = object.inline_extent.physical_first;
  size_t found = 0;
  for (size_t i = 0; i < state->claim_count; i++) {
    if (state->claims[i].first == old_data || state->claims[i].first == object_root.block) {
      TEST_ASSERT_LESS_THAN_UINT(2, found);
      removed[found++] = state->claims[i];
    }
  }
  TEST_ASSERT_EQUAL_UINT(2, found);
  memset(data, byte, sizeof(data));
  object.inline_extent.physical_first = batch->available[0];
  object.inline_extent.birth = batch->generation;
  record_context.containing_birth = batch->generation;
  size_t length;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_encode(encoded, sizeof(encoded), &record_context, &object, &length));
  object_slot = (struct pfs_edit_slot){.block = batch->available[1]};
  struct pfs_edit_candidate candidate = {.reader = &device.builder.reader, .context = context,
    .birth = batch->generation, .root = object_root, .slots = &object_slot, .slot_capacity = 1,
    .retired = retired, .retired_capacity = 1};
  struct pfs_key key = {.length = PFS_ID_SIZE, .bytes = {4}};
  struct pfs_encoded_record record = {encoded, length};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &editor, PFS_EDIT_UPDATE, &key, &record));
  batch->volume.record.object_root = candidate.root;
  replacements[0] = (struct pfs_batch_block){batch->available[0], data};
  replacements[1] = (struct pfs_batch_block){batch->available[1], object_slot.data};
  added[0] = (struct check_claim){.first = batch->available[0], .count = 1,
    .birth = batch->generation, .volume = {{2}}, .object = {{4}}};
  added[1] = (struct check_claim){.first = batch->available[1], .count = 1,
    .birth = batch->generation, .volume = {{2}}, .type = PFS_BLOCK_TREE, .kind = PFS_INDEX_OBJECTS};
  batch->blocks = replacements;
  batch->block_count = 2;
  batch->remove = removed;
  batch->remove_count = 2;
  batch->add = added;
  batch->add_count = 2;
  return batch;
}

/* Read each durable root independently of publisher summaries/checker output.
 * Only explicit format codecs locate the payload; expected bytes come from the
 * workload, never from the writer, reader or checker agreeing with each other. */
static bool
retained_payload(struct test_failure *adapter, size_t slot)
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
      pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree) != PFS_OK || tree.level) {
    return false;
  }
  struct pfs_record_context record_context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = super.header.birth, .containing_birth = tree.header.birth,
    .features = super.features};
  struct pfs_volume_record v;
  if (pfs_volume_record_decode(bytes + tree.slots[0].offset, tree.slots[0].length, &record_context, &v) != PFS_OK) {
    return false;
  }
  tree_context.kind = PFS_INDEX_OBJECTS;
  tree_context.volume = v.id;
  tree_context.block.reference = v.object_root;
  if (test_failure_durable_read(adapter, v.object_root.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree) != PFS_OK || tree.level) {
    return false;
  }
  struct pfs_object_record object;
  record_context.containing_birth = tree.header.birth;
  if (pfs_object_record_decode(bytes + tree.slots[1].offset, tree.slots[1].length,
      &record_context, &object) != PFS_OK || object.id.bytes[0] != 4 ||
      object.file_length != PFS_BLOCK_SIZE || object.inline_extent.count != 1 ||
      test_failure_durable_read(adapter, object.inline_extent.physical_first, 1, bytes) != PFS_OK) {
    return false;
  }
  uint8_t expected = super.header.birth == 1 ? 'A' : super.header.birth < 5 ? 'B' : 'C';
  for (size_t i = 0; i < sizeof(bytes); i++) {
    if (bytes[i] != expected) {
      return false;
    }
  }
  return true;
}

static void
observe_payloads(struct test_failure *adapter, const struct test_failure_event *event,
                 bool before, void *context)
{
  (void)context;
  if (before && event->kind == TEST_FAILURE_WRITE &&
      (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1)) {
    payload_checks++;
    payload_violation |= !retained_payload(adapter, 0) || !retained_payload(adapter, 1);
  }
}

static void
real_publication_drains_and_preserves_retained_payloads(void)
{
  build_seed();
  open_device();
  expect_read('A');
  device.observer = observe_payloads;
  payload_checks = 0;
  payload_violation = false;
  struct pfs_batch *batch = overwrite('B');
  /* A funded drain must make no allocation callback, even if all new allocations
   * would fail. Capped reserved storage remains available throughout. */
  test_failure_memory_fail_after(&device, 0);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_EQUAL_UINT64(4, pool.writer->last_confirmed_generation);
  TEST_ASSERT_FALSE(pool.writer->status.drain_pending);
  TEST_ASSERT_FALSE(payload_violation);
  TEST_ASSERT_EQUAL_UINT(3, payload_checks);
  device.backing.fail_after = SIZE_MAX;
  expect_read('B');
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_EQUAL_UINT64(4, pool.writer->last_confirmed_generation);
  batch = overwrite('C');
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL_UINT64(7, pool.writer->last_confirmed_generation);
  TEST_ASSERT_FALSE(payload_violation);
  TEST_ASSERT_EQUAL_UINT(6, payload_checks);
  expect_read('C');
  close_device();
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
}

static void
cold_reopen_and_compare(uint8_t expected)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
  struct pfs_pool cold = {0};
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&cold, &recovered.builder,
    recovered.memory, &options, &opening));
  TEST_ASSERT_FALSE(cold.writer->status.drain_pending);
  struct pfs_volume v = {0};
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&cold, &id, &v));
  struct pfs_trusted_context context = {.principal = {{5}}, .root = {{4}}, .scope = PFS_SCOPE_OBJECT,
    .ceiling = {PFS_FILE_READ, 0, 0}};
  struct pfs_rights rights = {.file = PFS_FILE_READ};
  struct pfs_view *file = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&v, &context, &context.root, PFS_SCOPE_OBJECT, &rights, &file));
  uint8_t bytes[PFS_BLOCK_SIZE];
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(file, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL_UINT(PFS_BLOCK_SIZE, count);
  for (size_t i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_UINT8(expected, bytes[i]);
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&v));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&cold));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&recovered.builder.reader, recovered.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(test_failure_close(&recovered));
}

static void
flush_boundaries_preserve_progress_and_stop_stickily(void)
{
  build_seed();
  /* Six publication flushes: replacement/slot for user, advance, then free.
   * Include none/all durable outcomes of every failed flush. */
  for (uint64_t boundary = 1; boundary <= 6; boundary++) {
    for (unsigned promoted = 0; promoted < 2; promoted++) {
      open_device();
      struct pfs_batch *batch = overwrite('B');
      struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + boundary,
        .mode = TEST_FAILURE_FLUSH_PREFIX, .prefix = promoted ? SIZE_MAX : 0};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
      struct pfs_write_result result;
      TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
      TEST_ASSERT_TRUE(device.triggered);
      TEST_ASSERT_FALSE(device.infrastructure_failure);
      bool uncertain = boundary % 2 == 0;
      bool user_confirmed = boundary > 2;
      TEST_ASSERT_EQUAL(user_confirmed ? PFS_COMPLETE : uncertain ? PFS_UNKNOWN : PFS_STOPPED,
        result.completion);
      TEST_ASSERT_EQUAL(uncertain ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED, result.health);
      if (user_confirmed) {
        TEST_ASSERT_EQUAL(PFS_OK, result.operation_status);
        TEST_ASSERT_EQUAL(uncertain ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
          result.maintenance_completion);
        TEST_ASSERT_TRUE(pool.writer->status.drain_pending);
      }
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_writer_prepare(&pool, &batch));
      uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
      TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(view, &result));
      TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
      if (uncertain) {
        uint8_t byte;
        size_t count = 99;
        TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_read(view, 0, &byte, 1, &count));
        TEST_ASSERT_EQUAL_UINT(99, count);
      } else {
        expect_read(user_confirmed ? 'B' : 'A');
      }
      close_device();
      cold_reopen_and_compare(user_confirmed || (boundary == 2 && promoted) ? 'B' : 'A');
      TEST_ASSERT_TRUE(test_failure_close(&device));
    }
  }
}

static void
cache_only_slot_is_not_recovered_by_later_flush(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
    .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + 2,
    .mode = TEST_FAILURE_CACHE_ONLY_SLOT, .block = 0};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_UNKNOWN, result.completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
  /* Direct adapter exercise, not permission to revive this core instance. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&device.builder));
  uint8_t cached[PFS_BLOCK_SIZE], durable[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&device.builder.reader, 0, 1, cached, sizeof(cached)));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&device, 0, 1, durable));
  struct pfs_superblock warm, cold;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(cached, sizeof(cached), PFS_POOL_BLOCKS_MIN, 0, &warm));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(durable, sizeof(durable), PFS_POOL_BLOCKS_MIN, 0, &cold));
  TEST_ASSERT_EQUAL_UINT64(2, warm.header.birth);
  TEST_ASSERT_EQUAL_UINT64(1, cold.header.birth);
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_writer_prepare(&pool, &batch));
  close_device();
  /* Complete cached structural validation succeeds while storage still differs.
   * A fresh core cannot infer missing adapter/operator history from these bytes. */
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  cold_reopen_and_compare('A');
}

static void
replacement_and_slot_writes_fail_without_retry(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  test_failure_trace_reset(&device);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  uint64_t offsets[256], blocks[256];
  bool slot[256];
  size_t count = 0;
  uint64_t first = 0;
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_WRITE) {
      TEST_ASSERT_LESS_THAN_UINT(256, count);
      if (!first) {
        first = event->ordinal;
      }
      offsets[count] = event->ordinal - first + 1;
      blocks[count] = event->first;
      slot[count++] = event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1;
    }
  }
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  for (size_t i = 0; i < count; i++) {
    open_device();
    batch = overwrite('B');
    struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_WRITE,
      .ordinal = device.ordinals[TEST_FAILURE_WRITE] + offsets[i],
      .mode = slot[i] ? TEST_FAILURE_TORN_SLOT : TEST_FAILURE_DURABLE_PREFIX,
      .prefix = slot[i] ? 64 : 1};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_EQUAL_UINT64(fault.ordinal, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL(slot[i] ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED, result.health);
    close_device();
    /* A torn slot can leave degraded media: read-only inspection may select the
     * other slot, but writable opening requires two fully validated states. */
    if (!slot[i]) {
      bool confirmed = false;
      for (size_t j = 0; j < i; j++) {
        confirmed |= slot[j];
      }
      cold_reopen_and_compare(confirmed ? 'B' : 'A');
    } else {
      uint8_t bytes[PFS_BLOCK_SIZE];
      struct pfs_superblock super;
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&device, blocks[i], 1, bytes));
      TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
        blocks[i], &super));
    }
    TEST_ASSERT_TRUE(test_failure_close(&device));
  }
}

void
run_publication_tests(void)
{
  RUN_TEST(real_publication_drains_and_preserves_retained_payloads);
  RUN_TEST(flush_boundaries_preserve_progress_and_stop_stickily);
  RUN_TEST(cache_only_slot_is_not_recovered_by_later_flush);
  RUN_TEST(replacement_and_slot_writes_fail_without_retry);
}
