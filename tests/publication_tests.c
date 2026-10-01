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
static uint8_t extent_data[PFS_BLOCK_SIZE], extent_records[2][PFS_EXTENT_RECORD_SIZE];
static struct pfs_batch_block replacements[4];
static struct check_claim removed[2], added[4];
static unsigned payload_checks;
static bool payload_violation;
static bool reentry_checked, reentry_violation;
static struct pfs_write_options options = {16, 16};
static uint64_t initial_generation = 1;

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
build_seed_with_budgets(uint64_t quota, uint64_t guarantee, uint64_t recovery)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE},
  };
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .guarantee_set = true, .quota_set = true, .quota = quota,
    .guarantee = guarantee,
    .object_count = 2, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &v,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = recovery};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  initial_generation = 1;
}

static void
build_seed(void)
{
  options = (struct pfs_write_options){16, 16};
  build_seed_with_budgets(64, 0, 1024);
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
  TEST_ASSERT_EQUAL_UINT64(initial_generation, result.confirmed_generation);
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
edit_file(uint8_t byte, bool grow)
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
  if (grow) {
    /* The object replacement lies between the two data blocks, so distinct
     * physical mappings require two canonical extents in a new extent root. */
    TEST_ASSERT_NOT_EQUAL(batch->available[0] + 1, batch->available[2]);
    object.file_length = 2 * PFS_BLOCK_SIZE;
    object.storage_kind = PFS_STORAGE_TREE;
    object.tree_root = (struct pfs_reference){batch->available[3], batch->generation,
      PFS_BLOCK_TREE, PFS_FORMAT_VERSION};
    struct pfs_encoded_record records[2];
    for (size_t i = 0; i < 2; i++) {
      struct pfs_extent_record extent = {.mapping = {.logical_first = i, .count = 1,
        .physical_first = batch->available[2 * i], .birth = batch->generation}};
      size_t length;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(extent_records[i],
        sizeof(extent_records[i]), &record_context, &extent, &length));
      records[i] = (struct pfs_encoded_record){extent_records[i], length};
    }
    struct pfs_tree_context extent_context = context;
    extent_context.kind = PFS_INDEX_EXTENTS;
    extent_context.object = object.id;
    extent_context.block.reference = object.tree_root;
    struct pfs_tree extents = {.header = {.version = PFS_FORMAT_VERSION,
      .type = PFS_BLOCK_TREE, .pool = {{1}}, .block = object.tree_root.block,
      .birth = batch->generation}, .kind = PFS_INDEX_EXTENTS,
      .volume = {{2}}, .object = {{4}}, .count = 2};
    TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(extent_data, sizeof(extent_data),
      &extent_context, &extents, records));
    batch->volume.metadata_blocks++;
    batch->volume.file_extents = 2;
  }
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
  if (grow) {
    replacements[2] = (struct pfs_batch_block){batch->available[2], data};
    replacements[3] = (struct pfs_batch_block){batch->available[3], extent_data};
    added[2] = (struct check_claim){.first = batch->available[2], .count = 1,
      .birth = batch->generation, .volume = {{2}}, .object = {{4}}, .logical = 1};
    added[3] = (struct check_claim){.first = batch->available[3], .count = 1,
      .birth = batch->generation, .volume = {{2}}, .object = {{4}},
      .type = PFS_BLOCK_TREE, .kind = PFS_INDEX_EXTENTS};
  }
  batch->blocks = replacements;
  batch->block_count = grow ? 4 : 2;
  batch->remove = removed;
  batch->remove_count = 2;
  batch->add = added;
  batch->add_count = grow ? 4 : 2;
  return batch;
}

static struct pfs_batch *
overwrite(uint8_t byte)
{
  return edit_file(byte, false);
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
  uint64_t first_retired[] = {removed[0].first, removed[1].first};
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
  bool reused = false;
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_WRITE && event->status == PFS_OK) {
      for (size_t j = 0; j < 2; j++) {
        reused |= event->first <= first_retired[j] &&
                  first_retired[j] < event->first + event->count;
      }
    }
  }
  TEST_ASSERT_TRUE(reused);
  expect_read('C');
  close_device();
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
}

static void
expect_ready_without_progress(uint64_t generation, uint64_t writes, uint64_t flushes)
{
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, status.health);
  TEST_ASSERT_EQUAL(PFS_OK, status.failure);
  TEST_ASSERT_FALSE(status.drain_pending);
  TEST_ASSERT_FALSE(status.invariant_failure);
  TEST_ASSERT_EQUAL_UINT64(generation, pool.writer->last_confirmed_generation);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
}

static uint64_t
minimum_recovery(const struct pfs_plan_limits *limits)
{
  return limits->recovery_blocks > 256 ? limits->recovery_blocks : 256;
}

static void
near_minimum_profile_quota_pool_and_memory_fund_drain(void)
{
  options = (struct pfs_write_options){1, 3};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 3, 1, &limits));
  uint64_t recovery = minimum_recovery(&limits);
  /* One case fills effective quota; the other fills the logical pool promise.
   * Geometry stays at the format's 64 MiB minimum in both cases. */
  for (unsigned tight_pool = 0; tight_pool < 2; tight_pool++) {
    uint64_t guarantee = tight_pool ? PFS_POOL_BLOCKS_MIN - 2 -
      limits.permanent_pool - 2 * 1024 - recovery : 0;
    build_seed_with_budgets(tight_pool ? guarantee : 4, guarantee, recovery);
    open_device();
    struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
    TEST_ASSERT_EQUAL_UINT64(1, state->file_extents);
    TEST_ASSERT_EQUAL_UINT64(3, state->metadata_blocks);
    TEST_ASSERT_EQUAL_UINT64(2, state->volumes[0].record.object_count);
    TEST_ASSERT_EQUAL_UINT64(1, state->volumes[0].directories);
    TEST_ASSERT_EQUAL_UINT64(1, state->volumes[0].deletion_blocks);
    TEST_ASSERT_EQUAL_UINT64(4, state->volumes[0].effective_blocks);
    TEST_ASSERT_EQUAL_UINT64(1024, state->candidate.root.cow.capacity);
    TEST_ASSERT_EQUAL_UINT64(1024, state->candidate.root.migration.capacity);
    TEST_ASSERT_EQUAL_UINT64(recovery, state->candidate.root.recovery.capacity);
    TEST_ASSERT_EQUAL_UINT64(limits.arena_bytes, pool.writer->arena.allocation.size);
    if (tight_pool) {
      TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, limits.permanent_pool + guarantee +
        state->candidate.root.cow.capacity + state->candidate.root.migration.capacity +
        state->candidate.root.recovery.capacity);
    } else {
      TEST_ASSERT_EQUAL_UINT64(state->volumes[0].effective_blocks, state->volumes[0].record.quota);
    }
    /* Existing handles and the reserved arena are the entire remaining memory
     * budget. A drain may neither raise that cap nor request another allocation. */
    uint64_t memory_limit = device.memory->limit;
    device.memory->limit = device.memory->used;
    struct pfs_batch *batch = overwrite('B');
    size_t allocations = device.backing.allocation_calls;
    uint64_t memory = device.memory->used;
    test_failure_memory_fail_after(&device, 0);
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_EQUAL_UINT64(4, pool.writer->last_confirmed_generation);
    TEST_ASSERT_FALSE(pool.writer->status.drain_pending);
    TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
    TEST_ASSERT_EQUAL_UINT64(memory, device.memory->used);
    device.backing.fail_after = SIZE_MAX;
    device.memory->limit = memory_limit;
    expect_read('B');
    close_device();
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
ordinary_profile_quota_and_memory_refusals_preserve_ready_writer(void)
{
  const struct pfs_write_options profiles[] = {{1, 4}, {2, 3}, {2, 4}};
  const enum pfs_status expected[] = {PFS_LIMIT, PFS_LIMIT, PFS_QUOTA};
  for (size_t i = 0; i < 3; i++) {
    build_seed_with_budgets(i == 2 ? 4 : 64, 0, 1024);
    options = profiles[i];
    open_device();
    struct pfs_batch *batch = edit_file('B', true);
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(expected[i], pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL(expected[i], result.operation_status);
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_NONE, result.maintenance_completion);
    expect_ready_without_progress(1, writes, flushes);
    expect_read('A');

    struct pfs_trusted_context context = {.principal = {{5}}, .root = {{4}},
      .scope = PFS_SCOPE_OBJECT, .ceiling = {PFS_FILE_READ, 0, 0}};
    struct pfs_rights rights = {.file = PFS_FILE_READ};
    struct pfs_view *extra = NULL;
    test_failure_memory_fail_after(&device, 0);
    TEST_ASSERT_EQUAL(PFS_NO_MEMORY, pfs_view_acquire(&volume, &context,
      &context.root, PFS_SCOPE_OBJECT, &rights, &extra));
    TEST_ASSERT_NULL(extra);
    expect_ready_without_progress(1, writes, flushes);
    batch = overwrite('B');
    size_t allocations = device.backing.allocation_calls;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
    device.backing.fail_after = SIZE_MAX;
    expect_read('B');
    close_device();
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
opening_refuses_unfunded_pool_recovery_and_memory(void)
{
  options = (struct pfs_write_options){1, 3};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 3, 1, &limits));
  uint64_t recovery = minimum_recovery(&limits);
  uint64_t guarantee = PFS_POOL_BLOCKS_MIN - 2 - limits.permanent_pool - 2 * 1024 - recovery;
  for (unsigned cause = 0; cause < 4; cause++) {
    uint64_t promised = cause == 0 ? guarantee + 1 : guarantee;
    build_seed_with_budgets(promised, promised, cause == 1 ? recovery - 1 : recovery);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
      (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1,
      cause == 2 ? limits.arena_bytes - 1 : 0, &seed.builder.reader));
    pool = (struct pfs_pool){0};
    struct pfs_write_open_result result;
    if (cause == 3) {
      test_failure_memory_fail_after(&device, 0);
    }
    enum pfs_status expected = cause == 3 ? PFS_NO_MEMORY : cause == 2 ? PFS_LIMIT : PFS_NO_SPACE;
    TEST_ASSERT_EQUAL(expected,
      pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, &result));
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_generation);
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.recovery.completion);
    TEST_ASSERT_EQUAL(expected, result.recovery.operation_status);
    TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.writer.health);
    if (cause == 3) {
      TEST_ASSERT_EQUAL(PFS_POOL_NO_SELECTION, result.pool.selected);
      TEST_ASSERT_EQUAL(PFS_NO_MEMORY, result.pool.candidate[0].status);
      TEST_ASSERT_EQUAL(PFS_NO_MEMORY, result.pool.candidate[1].status);
    } else {
      TEST_ASSERT_EQUAL_UINT64(limits.recovery_blocks, result.required_recovery_blocks);
    }
    TEST_ASSERT_NULL(pool.state.data);
    TEST_ASSERT_NULL(pool.writer);
    TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
    TEST_ASSERT_EQUAL_UINT64(0, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(0, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
computed_recovery_budget_funds_drain_and_refuses_one_below(void)
{
  /* Large declared bounds exercise recovery arithmetic on the same small
   * reachable seed, rather than claiming a populated large-workload result. */
  options = (struct pfs_write_options){4096, 2048};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN,
    options.extent_limit, options.metadata_limit, 1, &limits));
  uint64_t recovery = minimum_recovery(&limits);
  TEST_ASSERT_GREATER_THAN_UINT64(1024, recovery);
  for (unsigned below = 0; below < 2; below++) {
    build_seed_with_budgets(4, 0, recovery - below);
    if (below) {
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
        (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
      pool = (struct pfs_pool){0};
      struct pfs_write_open_result opening;
      TEST_ASSERT_EQUAL(PFS_NO_SPACE, pfs_pool_open_writer(&pool, &device.builder,
        device.memory, &options, &opening));
      TEST_ASSERT_NULL(pool.state.data);
      TEST_ASSERT_NULL(pool.writer);
      TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
      TEST_ASSERT_EQUAL_UINT64(0, device.ordinals[TEST_FAILURE_WRITE]);
      TEST_ASSERT_EQUAL_UINT64(0, device.ordinals[TEST_FAILURE_FLUSH]);
    } else {
      open_device();
      TEST_ASSERT_EQUAL_UINT64(limits.recovery_blocks,
        pool.writer->states[pool.writer->selected].candidate.root.recovery.capacity);
      struct pfs_batch *batch = overwrite('B');
      size_t allocations = device.backing.allocation_calls;
      test_failure_memory_fail_after(&device, 0);
      struct pfs_write_result result;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
      TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
      TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
      device.backing.fail_after = SIZE_MAX;
      expect_read('B');
      close_device();
    }
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
set_seed_generation(uint64_t generation)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock original;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, 0, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
    0, &original));
  struct pfs_block_context context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = 1, .referring_birth = 1, .pool = {{1}},
    .reference = original.root, .features = original.features};
  struct pfs_pool_root root;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, original.root.block, 1,
    bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_decode(bytes, sizeof(bytes), &context, &root));
  struct pfs_reference *refs[] = {&root.volumes, &root.volume_names, &root.allocation};
  /* A slot and its pool root share a birth. Move the complete pool metadata
   * incarnation and map charges together; volume metadata and data stay old. */
  for (size_t i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, refs[i]->block, 1,
      bytes, sizeof(bytes)));
    if (i == 2) {
      struct pfs_tree_context tree_context = {.block = context, .kind = PFS_INDEX_ALLOCATION};
      tree_context.block.reference = *refs[i];
      struct pfs_tree tree;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree));
      struct pfs_record_context record_context = {.block_count = PFS_POOL_BLOCKS_MIN,
        .selected_generation = 1, .containing_birth = 1, .features = original.features};
      for (size_t j = 0; j < tree.count; j++) {
        struct pfs_allocation_record record;
        TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_decode(bytes + tree.slots[j].offset,
          tree.slots[j].length, &record_context, &record));
        if (record.state == PFS_ALLOCATION_POOL) {
          test_put_u64(bytes + tree.slots[j].offset + 56, generation);
          test_checksum(bytes + tree.slots[j].offset, tree.slots[j].length, 8);
        }
      }
    }
    test_put_u64(bytes + 48, generation);
    test_checksum(bytes, sizeof(bytes), 20);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, refs[i]->block, 1,
      bytes, sizeof(bytes)));
    refs[i]->birth = generation;
  }
  context.selected_generation = context.referring_birth = generation;
  context.reference.birth = generation;
  root.header.birth = generation;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_encode(bytes, sizeof(bytes), &context, &root));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, root.header.block, 1,
    bytes, sizeof(bytes)));
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, block, 1, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
      block, &super));
    super.header.birth = generation;
    super.root.birth = generation;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(bytes, sizeof(bytes), &super));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, block, 1, bytes, sizeof(bytes)));
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&seed.builder));
  initial_generation = generation;
}

static void
generation_boundary_funds_exact_drain_and_refuses_next_batch(void)
{
  for (unsigned extra = 0; extra < 2; extra++) {
    build_seed();
    set_seed_generation(UINT64_MAX - 3 + extra);
    open_device();
    if (!extra) {
      struct pfs_batch *batch = overwrite('B');
      test_failure_memory_fail_after(&device, 0);
      struct pfs_write_result result;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
      TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, pool.writer->last_confirmed_generation);
      device.backing.fail_after = SIZE_MAX;
    }
    uint64_t generation = pool.writer->last_confirmed_generation;
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct pfs_batch *batch = NULL;
    TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_writer_prepare(&pool, &batch));
    TEST_ASSERT_NULL(batch);
    expect_ready_without_progress(generation, writes, flushes);
    expect_read(extra ? 'A' : 'B');
    close_device();
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
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

static void
startup_cleanup_failure_keeps_confirmed_progress_and_releases_handles(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
    .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + 3, .mode = TEST_FAILURE_BEFORE};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL_UINT64(2, pool.writer->last_confirmed_generation);
  close_device();
  /* Clone only durable bytes: the selected user root still owns a funded drain.
   * Opening's initial healthy flush precedes replacement/slot cleanup flushes. */
  for (uint64_t boundary = 2; boundary <= 3; boundary++) {
    for (unsigned promoted = 0; promoted < 2; promoted++) {
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
      fault = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = boundary, .mode = TEST_FAILURE_FLUSH_PREFIX,
        .prefix = promoted ? SIZE_MAX : 0};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&recovered, &fault));
      struct pfs_pool cold = {0};
      struct pfs_write_open_result opening;
      TEST_ASSERT_EQUAL(PFS_IO, pfs_pool_open_writer(&cold, &recovered.builder,
        recovered.memory, &options, &opening));
      TEST_ASSERT_TRUE(recovered.triggered);
      TEST_ASSERT_FALSE(recovered.infrastructure_failure);
      TEST_ASSERT_NULL(cold.state.data);
      TEST_ASSERT_NULL(cold.writer);
      TEST_ASSERT_NULL(cold.memory);
      TEST_ASSERT_NULL(cold.reader);
      TEST_ASSERT_EQUAL_UINT64(0, recovered.memory->used);
      TEST_ASSERT_EQUAL_UINT64(2, opening.confirmed_generation);
      TEST_ASSERT_EQUAL(boundary == 3 ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
        opening.writer.health);
      TEST_ASSERT_TRUE(opening.writer.drain_pending);
      TEST_ASSERT_FALSE(opening.writer.invariant_failure);
      TEST_ASSERT_EQUAL(boundary == 3 ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
        opening.recovery.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_IO, opening.recovery.maintenance_status);
      TEST_ASSERT_TRUE(retained_payload(&recovered, 0));
      TEST_ASSERT_TRUE(retained_payload(&recovered, 1));
      TEST_ASSERT_TRUE(test_failure_close(&recovered));
    }
  }
}

static void
observe_publication_reentry(struct test_failure *adapter,
                           const struct test_failure_event *event, bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (!before || event->kind != TEST_FAILURE_WRITE || reentry_checked) {
    return;
  }
  reentry_checked = true;
  struct pfs_write_result result;
  uint8_t original[sizeof(result)];
  memset(&result, 0xa5, sizeof(result));
  memcpy(original, &result, sizeof(result));
  pfs_writer_abort(&pool);
  reentry_violation |= !pool.writer_busy || !pool.writer->prepared;
  reentry_violation |= pfs_writer_commit(&pool, &pool.writer->batch, &result) != PFS_BUSY;
  reentry_violation |= memcmp(original, &result, sizeof(result)) != 0;
  uint8_t byte = 0xa5;
  size_t count = 99;
  reentry_violation |= pfs_view_read(view, 0, &byte, 1, &count) != PFS_BUSY;
  reentry_violation |= byte != 0xa5 || count != 99;
  reentry_violation |= pfs_pool_close(&pool) != PFS_BUSY;
}

static void
publication_callbacks_cannot_abort_or_commit_recursively(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  reentry_checked = reentry_violation = false;
  device.observer = observe_publication_reentry;
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_TRUE(reentry_checked);
  TEST_ASSERT_FALSE(reentry_violation);
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_UINT64(4, pool.writer->last_confirmed_generation);
  expect_read('B');
  close_device();
}

void
run_publication_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(real_publication_drains_and_preserves_retained_payloads);
  RUN_TEST(flush_boundaries_preserve_progress_and_stop_stickily);
  RUN_TEST(cache_only_slot_is_not_recovered_by_later_flush);
  RUN_TEST(replacement_and_slot_writes_fail_without_retry);
  RUN_TEST(near_minimum_profile_quota_pool_and_memory_fund_drain);
  RUN_TEST(ordinary_profile_quota_and_memory_refusals_preserve_ready_writer);
  RUN_TEST(opening_refuses_unfunded_pool_recovery_and_memory);
  RUN_TEST(computed_recovery_budget_funds_drain_and_refuses_one_below);
  RUN_TEST(generation_boundary_funds_exact_drain_and_refuses_next_batch);
  RUN_TEST(startup_cleanup_failure_keeps_confirmed_progress_and_releases_handles);
  RUN_TEST(publication_callbacks_cannot_abort_or_commit_recursively);
}
