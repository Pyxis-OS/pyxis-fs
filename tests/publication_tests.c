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
static struct pfs_write_options options = {.extent_limit = 16, .metadata_limit = 16, .random = test_random};
static uint64_t initial_generation = 1;
static size_t seed_objects, seed_directories;
/* Configured near-minimum fixture inputs, not independent formatter defaults. */
static const struct pfs_build_spec seed_reserves = {
  .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
  .cow_reserve = 1024, .migration_reserve = 1024,
};
static uint64_t seed_workspace_capacity;
static uint64_t middle_payload_generation = UINT64_MAX;
static struct test_failure_flush_cut commit_cuts[TEST_FAILURE_EVENTS_MAX];

static struct pfs_writer_status
writer_status(void)
{
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
  return status;
}

static bool
read_durable_generation(const struct test_failure *adapter, uint64_t *generation)
{
  *generation = 0;
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    uint8_t bytes[PFS_BLOCK_SIZE];
    struct pfs_superblock super;
    if (test_failure_durable_read(adapter, block, 1, bytes) != PFS_OK ||
        pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, block, &super) != PFS_OK) {
      return false;
    }
    if (super.header.birth > *generation) {
      *generation = super.header.birth;
    }
  }
  return true;
}

static uint64_t
durable_generation(const struct test_failure *adapter)
{
  uint64_t generation;
  TEST_ASSERT_TRUE(read_durable_generation(adapter, &generation));
  return generation;
}

static size_t
slot_writes(const struct test_failure *adapter)
{
  size_t count = 0;
  for (size_t i = 0; i < adapter->event_count; i++) {
    const struct test_failure_event *event = &adapter->events[i];
    count += event->kind == TEST_FAILURE_WRITE && event->count == 1 &&
      (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1);
  }
  return count;
}

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
  seed_objects = sizeof(objects) / sizeof(objects[0]);
  seed_directories = 0;
  for (size_t i = 0; i < seed_objects; i++) {
    seed_directories += objects[i].kind == PFS_OBJECT_DIRECTORY;
  }
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .guarantee_set = true, .quota_set = true, .quota = quota,
    .guarantee = guarantee,
    .object_count = seed_objects, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &v,
    .reserve_set = seed_reserves.reserve_set,
    .cow_reserve = seed_reserves.cow_reserve,
    .migration_reserve = seed_reserves.migration_reserve, .recovery_reserve = recovery};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL_UINT64(spec.cow_reserve, plan.pool_root.cow.capacity);
  TEST_ASSERT_EQUAL_UINT64(spec.migration_reserve, plan.pool_root.migration.capacity);
  seed_workspace_capacity = plan.pool_root.cow.capacity + plan.pool_root.migration.capacity;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  initial_generation = 1;
  middle_payload_generation = UINT64_MAX;
}

static void
build_seed(void)
{
  options = (struct pfs_write_options){.extent_limit = 16, .metadata_limit = 16, .random = test_random};
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
    .ceiling = {PFS_FILE_READ | PFS_FILE_CHECKPOINT | PFS_FILE_RESIZE, PFS_DIR_LOOKUP, 0}};
  struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_CHECKPOINT | PFS_FILE_RESIZE};
  const struct pfs_object_id file = {{4}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &context, &file, PFS_SCOPE_OBJECT, &rights, &view));
}

static void
close_device(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
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

static bool
selection_block_eligible(uint64_t block)
{
  for (size_t retained = 0; retained < 2; retained++) {
    const struct pfs_admit_state *state = &pool.writer->states[retained];
    bool free = false;
    for (size_t i = 0; i < state->map_count; i++) {
      const struct pfs_allocation_record *record = &state->maps[i];
      if (block >= record->first && block - record->first < record->count) {
        free = record->state == PFS_ALLOCATION_FREE ||
          (retained != pool.writer->selected && record->state == PFS_ALLOCATION_RETIRED);
      }
    }
    if (!free) {
      return false;
    }
    for (size_t i = 0; i < state->claim_count; i++) {
      const struct check_claim *claim = &state->claims[i];
      if (block >= claim->first && block - claim->first < claim->count) {
        return false;
      }
    }
  }
  return true;
}

static bool
selection_run_available(size_t demand)
{
  size_t length = 0;
  for (uint64_t block = 1; block < PFS_POOL_BLOCKS_MIN - 1; block++) {
    length = selection_block_eligible(block) ? length + 1 : 0;
    if (length == demand) {
      return true;
    }
  }
  return false;
}

static void
expect_selection(const struct pfs_batch *batch, bool contiguous)
{
  TEST_ASSERT_EQUAL_UINT(PFS_PLAN_VOLUME_NEW, batch->available_count);
  bool run_available = selection_run_available(batch->available_count);
  TEST_ASSERT_EQUAL(contiguous, run_available);
  bool selected_contiguous = true;
  for (size_t i = 0; i < batch->available_count; i++) {
    TEST_ASSERT_TRUE(selection_block_eligible(batch->available[i]));
    if (i) {
      TEST_ASSERT_TRUE(batch->available[i] > batch->available[i - 1]);
      selected_contiguous &= batch->available[i] == batch->available[i - 1] + 1;
    }
  }
  TEST_ASSERT_EQUAL(run_available, selected_contiguous);
}

static struct pfs_batch *
prepare_selection(void)
{
  uint64_t reads = device.ordinals[TEST_FAILURE_READ];
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  uint64_t memory = device.memory->used;
  struct pfs_batch *batch = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_prepare(&pool, &batch));
  TEST_ASSERT_EQUAL_UINT64(reads, device.ordinals[TEST_FAILURE_READ]);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  TEST_ASSERT_EQUAL_UINT64(memory, device.memory->used);
  return batch;
}

static void
contiguous_prepare_preserves_committed_contents(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  expect_read('B');
  batch = prepare_selection();
  expect_selection(batch, true);
  pfs_writer_abort(&pool);
  expect_read('B');
  close_device();
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
}

/* These overlays isolate selection over sorted retained summaries. They are not
 * admitted filesystem images: only prepare/abort run while installed, and the
 * real admitted summaries are restored before reads, close or publication. */
static struct pfs_admit_state selection_saved[2];
static struct pfs_allocation_record selection_maps[2][6];
static struct check_claim selection_claims[2][2];

static void
install_selection_maps(void)
{
  for (size_t retained = 0; retained < 2; retained++) {
    selection_saved[retained] = pool.writer->states[retained];
    pool.writer->states[retained].maps = selection_maps[retained];
    pool.writer->states[retained].map_count = 1;
    pool.writer->states[retained].claims = selection_claims[retained];
    pool.writer->states[retained].claim_count = 0;
    selection_maps[retained][0] = (struct pfs_allocation_record){.first = 1,
      .count = PFS_POOL_BLOCKS_MIN - 2, .state = PFS_ALLOCATION_FREE};
  }
}

static void
restore_selection_maps(void)
{
  pfs_writer_abort(&pool);
  for (size_t retained = 0; retained < 2; retained++) {
    pool.writer->states[retained] = selection_saved[retained];
  }
}

static void
contiguous_prepare_joins_eligible_retained_map_boundaries(void)
{
  build_seed();
  open_device();
  install_selection_maps();
  size_t older = 1 - pool.writer->selected;
  struct pfs_admit_state *state = &pool.writer->states[older];
  uint64_t first = state->maps[0].first, demand = PFS_PLAN_VOLUME_NEW;
  uint64_t half = demand / 2;
  state->map_count = 3;
  state->maps[0].count = half;
  state->maps[1] = (struct pfs_allocation_record){.first = first + half,
    .count = demand - half, .state = PFS_ALLOCATION_RETIRED};
  /* Only the run crossing the eligible map boundary can meet this demand. */
  state->maps[2] = (struct pfs_allocation_record){.first = first + demand,
    .count = PFS_POOL_BLOCKS_MIN - 2 - demand, .state = PFS_ALLOCATION_POOL};
  struct pfs_batch *batch = prepare_selection();
  expect_selection(batch, true);
  restore_selection_maps();
  expect_read('A');
  close_device();
}

static void
contiguous_prepare_respects_each_retained_map_and_claim(void)
{
  build_seed();
  open_device();
  for (size_t retained = 0; retained < 2; retained++) {
    for (unsigned claim = 0; claim < 2; claim++) {
      install_selection_maps();
      struct pfs_admit_state *selected = &pool.writer->states[pool.writer->selected];
      uint64_t first = selected->maps[0].first, demand = PFS_PLAN_VOLUME_NEW;
      selected->map_count = 4;
      selected->maps[0].count = 2 * demand - 1;
      selected->maps[1] = (struct pfs_allocation_record){.first = first + 2 * demand - 1,
        .count = 1, .state = PFS_ALLOCATION_POOL};
      selected->maps[2] = (struct pfs_allocation_record){.first = first + 2 * demand,
        .count = demand, .state = PFS_ALLOCATION_FREE};
      selected->maps[3] = (struct pfs_allocation_record){.first = first + 3 * demand,
        .count = PFS_POOL_BLOCKS_MIN - 2 - 3 * demand, .state = PFS_ALLOCATION_POOL};
      struct pfs_admit_state *protected = &pool.writer->states[retained];
      uint64_t interrupted = first + demand - 1;
      if (claim) {
        protected->claims[0] = (struct check_claim){.first = first, .count = 1};
        protected->claims[1] = (struct check_claim){.first = interrupted, .count = 1};
        protected->claim_count = 2;
      } else if (retained == pool.writer->selected) {
        memmove(protected->maps + 3, protected->maps + 1, 3 * sizeof(*protected->maps));
        protected->maps[0].count = demand - 1;
        protected->maps[1] = (struct pfs_allocation_record){.first = interrupted,
          .count = 1, .state = PFS_ALLOCATION_VOLUME};
        protected->maps[2] = (struct pfs_allocation_record){.first = interrupted + 1,
          .count = demand - 1, .state = PFS_ALLOCATION_FREE};
        protected->map_count = 6;
      } else {
        protected->maps[0].count = demand - 1;
        protected->maps[1] = (struct pfs_allocation_record){.first = interrupted,
          .count = 1, .state = PFS_ALLOCATION_VOLUME};
        protected->maps[2] = (struct pfs_allocation_record){.first = interrupted + 1,
          .count = PFS_POOL_BLOCKS_MIN - 2 - demand, .state = PFS_ALLOCATION_FREE};
        protected->map_count = 3;
      }
      struct pfs_batch *batch = prepare_selection();
      expect_selection(batch, true);
      TEST_ASSERT_FALSE(selection_block_eligible(interrupted));
      if (claim) {
        TEST_ASSERT_FALSE(selection_block_eligible(first));
      }
      for (size_t i = 0; i < batch->available_count; i++) {
        TEST_ASSERT_NOT_EQUAL(interrupted, batch->available[i]);
        if (claim) {
          TEST_ASSERT_NOT_EQUAL(first, batch->available[i]);
        }
      }
      restore_selection_maps();
    }
  }
  expect_read('A');
  close_device();
}

static void
contiguous_prepare_falls_back_when_only_short_runs_exist(void)
{
  build_seed();
  open_device();
  install_selection_maps();
  struct pfs_admit_state *selected = &pool.writer->states[pool.writer->selected];
  uint64_t first = selected->maps[0].first, demand = PFS_PLAN_VOLUME_NEW;
  selected->map_count = 4;
  selected->maps[0].count = demand - 1;
  selected->maps[1] = (struct pfs_allocation_record){.first = first + demand - 1,
    .count = 1, .state = PFS_ALLOCATION_POOL};
  selected->maps[2] = (struct pfs_allocation_record){.first = first + demand,
    .count = demand - 1, .state = PFS_ALLOCATION_FREE};
  selected->maps[3] = (struct pfs_allocation_record){.first = first + 2 * demand - 1,
    .count = PFS_POOL_BLOCKS_MIN - 1 - 2 * demand, .state = PFS_ALLOCATION_POOL};
  struct pfs_batch *batch = prepare_selection();
  expect_selection(batch, false);
  restore_selection_maps();
  expect_read('A');
  close_device();
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
  uint8_t expected = super.header.birth <= initial_generation ? 'A' :
    super.header.birth <= middle_payload_generation ? 'B' : 'C';
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

static bool
durable_allocation_state(struct test_failure *adapter, const struct pfs_tree_context *context,
                         uint64_t block, enum pfs_allocation_state *state)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  if (test_failure_durable_read(adapter, context->block.reference.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), context, &tree) != PFS_OK) {
    return false;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth, .features = context->block.features};
  for (size_t i = 0; i < tree.count; i++) {
    if (tree.level) {
      struct pfs_internal_record internal;
      if (pfs_internal_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          PFS_INDEX_ALLOCATION, &records, &internal) != PFS_OK) {
        return false;
      }
      struct pfs_tree_context child = *context;
      child.block.reference = internal.child;
      child.block.referring_birth = tree.header.birth;
      child.parent_level = tree.level;
      if (durable_allocation_state(adapter, &child, block, state)) {
        return true;
      }
    } else {
      struct pfs_allocation_record record;
      if (pfs_allocation_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          &records, &record) != PFS_OK) {
        return false;
      }
      if (block >= record.first && block - record.first < record.count) {
        *state = record.state;
        return true;
      }
    }
  }
  return false;
}

static bool
durable_selected_block_is_free(struct test_failure *adapter, uint64_t block)
{
  uint64_t generation = durable_generation(adapter);
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t physical = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    if (test_failure_durable_read(adapter, physical, 1, bytes) != PFS_OK ||
        pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, physical, &super) != PFS_OK) {
      return false;
    }
    if (super.header.birth != generation) {
      continue;
    }
    struct pfs_block_context context = {.block_count = PFS_POOL_BLOCKS_MIN,
      .selected_generation = generation, .referring_birth = generation,
      .pool = super.header.pool, .features = super.features, .reference = super.root};
    struct pfs_pool_root root;
    if (test_failure_durable_read(adapter, super.root.block, 1, bytes) != PFS_OK ||
        pfs_pool_root_decode(bytes, sizeof(bytes), &context, &root) != PFS_OK) {
      return false;
    }
    struct pfs_tree_context allocation = {.block = context, .kind = PFS_INDEX_ALLOCATION};
    allocation.block.reference = root.allocation;
    enum pfs_allocation_state state;
    return durable_allocation_state(adapter, &allocation, block, &state) && state == PFS_ALLOCATION_FREE;
  }
  return false;
}

static void
real_publications_carry_debt_and_preserve_retained_payloads(void)
{
  build_seed();
  open_device();
  expect_read('A');
  device.observer = observe_payloads;
  payload_checks = 0;
  payload_violation = false;
  struct pfs_batch *batch = overwrite('B');
  uint64_t first_retired[] = {removed[0].first, removed[1].first};
  test_failure_memory_fail_after(&device, 0);
  size_t allocations = device.backing.allocation_calls;
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_TRUE(writer_status().drain_pending);
  middle_payload_generation = durable_generation(&device);
  TEST_ASSERT_TRUE(retained_payload(&device, 0));
  TEST_ASSERT_TRUE(retained_payload(&device, 1));
  for (size_t i = 0; i < sizeof(first_retired) / sizeof(first_retired[0]); i++) {
    TEST_ASSERT_FALSE(selection_block_eligible(first_retired[i]));
  }
  for (unsigned step = 0; step < 2; step++) {
    batch = overwrite('C');
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_TRUE(writer_status().drain_pending);
    TEST_ASSERT_TRUE(retained_payload(&device, 0));
    TEST_ASSERT_TRUE(retained_payload(&device, 1));
    if (step == 1) {
      for (size_t i = 0; i < sizeof(first_retired) / sizeof(first_retired[0]); i++) {
        TEST_ASSERT_TRUE(durable_selected_block_is_free(&device, first_retired[i]));
        TEST_ASSERT_TRUE(selection_block_eligible(first_retired[i]));
      }
    }
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_FALSE(writer_status().drain_pending);
  TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
  TEST_ASSERT_FALSE(payload_violation);
  TEST_ASSERT_GREATER_THAN_UINT(0, slot_writes(&device));
  TEST_ASSERT_EQUAL_UINT(slot_writes(&device), payload_checks);
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  uint64_t generation = durable_generation(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_EQUAL_UINT64(generation, durable_generation(&device));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  device.backing.fail_after = SIZE_MAX;
  expect_read('C');
  close_device();
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
}

#define ROLLING_VOLUMES ((PFS_BLOCK_SIZE - PFS_TREE_HEADER_SIZE) / \
  (PFS_VOLUME_RECORD_SIZE + PFS_TREE_SLOT_SIZE) + 3u)
struct rolling_expectation {
  uint64_t generation;
  uint8_t bytes[ROLLING_VOLUMES];
};
static struct rolling_expectation rolling_history[16];
static size_t rolling_history_count, rolling_checks;
static bool rolling_violation;
static struct pfs_volume rolling_volumes[3];
static struct pfs_view *rolling_views[3];

static enum pfs_status
rolling_source_read(void *context, size_t v, size_t object, uint64_t offset,
                    void *buffer, size_t length)
{
  (void)context;
  if (v >= ROLLING_VOLUMES || object != 1 || offset > PFS_BLOCK_SIZE ||
      length > PFS_BLOCK_SIZE - offset) {
    return PFS_INVALID;
  }
  memset(buffer, 'a' + v, length);
  return PFS_OK;
}

static bool
rolling_volume_record(struct test_failure *adapter, struct pfs_tree_context context,
                      size_t index, struct pfs_volume_record *out, uint64_t *leaf)
{
  struct pfs_key key = {.length = PFS_ID_SIZE, .bytes = {(uint8_t)(10 + index)}};
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (;;) {
    struct pfs_tree tree;
    if (test_failure_durable_read(adapter, context.block.reference.block, 1, bytes) != PFS_OK ||
        pfs_tree_decode(bytes, sizeof(bytes), &context, &tree) != PFS_OK) {
      return false;
    }
    struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
      .selected_generation = context.block.selected_generation,
      .containing_birth = tree.header.birth, .features = context.block.features};
    if (!tree.level) {
      for (size_t i = 0; i < tree.count; i++) {
        if (pfs_volume_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
            &records, out) != PFS_OK) {
          return false;
        }
        if (!memcmp(out->id.bytes, key.bytes, PFS_ID_SIZE)) {
          *leaf = context.block.reference.block;
          return true;
        }
      }
      return false;
    }
    struct pfs_internal_record chosen = {0};
    for (size_t i = 0; i < tree.count; i++) {
      struct pfs_internal_record internal;
      if (pfs_internal_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          PFS_INDEX_VOLUMES, &records, &internal) != PFS_OK) {
        return false;
      }
      if (pfs_key_compare(PFS_INDEX_VOLUMES, &internal.minimum, &key) <= 0) {
        chosen = internal;
      }
    }
    if (!chosen.child.block) {
      return false;
    }
    context.block.reference = chosen.child;
    context.block.referring_birth = tree.header.birth;
    context.parent_level = tree.level;
  }
}

static bool
rolling_payloads_match(struct test_failure *adapter, unsigned slot,
                       uint64_t leaves[ROLLING_VOLUMES], uint64_t *volume_debt)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock super;
  uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
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
  size_t history = 0;
  while (history + 1 < rolling_history_count &&
         rolling_history[history + 1].generation <= super.header.birth) {
    history++;
  }
  if (volume_debt) {
    *volume_debt = 0;
  }
  for (size_t v = 0; v < ROLLING_VOLUMES; v++) {
    struct pfs_tree_context catalog = {.block = context, .kind = PFS_INDEX_VOLUMES};
    catalog.block.reference = root.volumes;
    struct pfs_volume_record record;
    uint64_t leaf;
    if (!rolling_volume_record(adapter, catalog, v, &record, &leaf)) {
      return false;
    }
    leaves[v] = leaf;
    if (volume_debt) {
      *volume_debt += record.retired_blocks;
    }
    struct pfs_tree_context objects = {.block = context, .kind = PFS_INDEX_OBJECTS,
      .volume = record.id};
    objects.block.reference = record.object_root;
    struct pfs_tree tree;
    if (test_failure_durable_read(adapter, record.object_root.block, 1, bytes) != PFS_OK ||
        pfs_tree_decode(bytes, sizeof(bytes), &objects, &tree) != PFS_OK || tree.level) {
      return false;
    }
    struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
      .selected_generation = super.header.birth, .containing_birth = tree.header.birth,
      .features = super.features};
    struct pfs_object_record object;
    bool found = false;
    for (size_t i = 0; i < tree.count; i++) {
      if (pfs_object_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          &records, &object) != PFS_OK) {
        return false;
      }
      if (object.id.bytes[0] == 65 + 2 * v) {
        found = true;
        break;
      }
    }
    if (!found || object.file_length != PFS_BLOCK_SIZE || object.inline_extent.count != 1 ||
        test_failure_durable_read(adapter, object.inline_extent.physical_first, 1, bytes) != PFS_OK) {
      return false;
    }
    for (size_t i = 0; i < sizeof(bytes); i++) {
      if (bytes[i] != rolling_history[history].bytes[v]) {
        return false;
      }
    }
  }
  return true;
}

static void
observe_rolling_payloads(struct test_failure *adapter, const struct test_failure_event *event,
                         bool before, void *context)
{
  (void)context;
  if (before && event->kind == TEST_FAILURE_WRITE &&
      (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1)) {
    uint64_t leaves[ROLLING_VOLUMES];
    rolling_checks++;
    rolling_violation |= !rolling_payloads_match(adapter, 0, leaves, NULL) ||
                         !rolling_payloads_match(adapter, 1, leaves, NULL);
  }
}

static void
run_cross_volume_rolling_history(bool restore_seed_slot)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[ROLLING_VOLUMES][2];
  struct pfs_build_volume specs[ROLLING_VOLUMES];
  for (size_t i = 0; i < ROLLING_VOLUMES; i++) {
    objects[i][0] = (struct pfs_build_object){.id = {{(uint8_t)(64 + 2 * i)}},
      .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY};
    objects[i][1] = (struct pfs_build_object){.id = {{(uint8_t)(65 + 2 * i)}},
      .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE};
    specs[i] = (struct pfs_build_volume){.id = {{(uint8_t)(10 + i)}},
      .root_object = {{(uint8_t)(64 + 2 * i)}},
      .name = {.length = 1, .bytes = {(uint8_t)('a' + i)}}, .owner = {{5}},
      .guarantee_set = true, .quota_set = true,
      .quota = 64, .object_count = 2, .objects = objects[i]};
  }
  options = (struct pfs_write_options){.extent_limit = ROLLING_VOLUMES,
    .metadata_limit = 4 * ROLLING_VOLUMES, .random = test_random};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, options.extent_limit,
    options.metadata_limit, ROLLING_VOLUMES, &limits));
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = ROLLING_VOLUMES, .volumes = specs,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024,
    .recovery_reserve = limits.recovery_blocks > 256 ? limits.recovery_blocks : 256};
  struct pfs_build_source source = {.read = rolling_source_read, .validate = source_validate};
  struct pfs_build_plan plan = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  pool = (struct pfs_pool){0};
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  struct pfs_superblock saved_seed;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&device, 0, 1, encoded));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(encoded, sizeof(encoded),
    PFS_POOL_BLOCKS_MIN, 0, &saved_seed));
  rolling_history_count = 1;
  rolling_history[0].generation = opening.confirmed_generation;
  for (size_t i = 0; i < ROLLING_VOLUMES; i++) {
    rolling_history[0].bytes[i] = (uint8_t)('a' + i);
  }
  uint64_t leaves[ROLLING_VOLUMES];
  TEST_ASSERT_TRUE(rolling_payloads_match(&device, 0, leaves, NULL));
  size_t indexes[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (size_t i = 0; i < ROLLING_VOLUMES && indexes[0] == SIZE_MAX; i++) {
    for (size_t j = i + 1; j < ROLLING_VOLUMES; j++) {
      if (leaves[i] == leaves[j]) {
        indexes[0] = i;
        indexes[1] = j;
        break;
      }
    }
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, indexes[0]);
  for (size_t i = 0; i < ROLLING_VOLUMES; i++) {
    if (leaves[i] != leaves[indexes[0]]) {
      indexes[2] = i;
      break;
    }
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, indexes[2]);
  const struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_WRITE | PFS_FILE_CHECKPOINT};
  struct pfs_trusted_context authority = {.principal = {{5}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE, .ceiling = rights};
  authority.ceiling.directory = PFS_DIR_LOOKUP;
  for (size_t i = 0; i < 3; i++) {
    rolling_volumes[i] = (struct pfs_volume){0};
    rolling_views[i] = NULL;
    struct pfs_volume_id id = {{(uint8_t)(10 + indexes[i])}};
    const struct pfs_object_id file = {{(uint8_t)(65 + 2 * indexes[i])}};
    authority.root = (struct pfs_object_id){{(uint8_t)(64 + 2 * indexes[i])}};
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &rolling_volumes[i]));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&rolling_volumes[i], &authority,
      &file, PFS_SCOPE_OBJECT, &rights, &rolling_views[i]));
  }
  struct pfs_view *checkpoint_view = NULL;
  const struct pfs_rights checkpoint_rights = {.file = PFS_FILE_CHECKPOINT};
  const struct pfs_object_id file = {{(uint8_t)(65 + 2 * indexes[2])}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&rolling_volumes[2], &authority,
    &file, PFS_SCOPE_OBJECT, &checkpoint_rights, &checkpoint_view));
  rolling_checks = 0;
  rolling_violation = false;
  device.observer = observe_rolling_payloads;
  test_failure_trace_reset(&device);
  /* A/B/C/A/C/B exercises old-owner frees with both catalog relationships.
   * No volume switch is a fence; all grants and identities remain local. */
  const unsigned order[] = {0, 1, 2, 0, 2, 1};
  struct pfs_write_result result;
  size_t steps = restore_seed_slot ? 2 : sizeof(order) / sizeof(order[0]);
  for (size_t step = 0; step < steps; step++) {
    unsigned target = restore_seed_slot && step == 1 ? 2 : order[step];
    struct rolling_expectation *next = &rolling_history[rolling_history_count];
    *next = rolling_history[rolling_history_count - 1];
    next->generation = durable_generation(&device) + 1;
    next->bytes[indexes[target]] = (uint8_t)('K' + step);
    rolling_history_count++;
    memset(data, 'K' + step, sizeof(data));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(rolling_views[target], 0, data, sizeof(data), &result));
    TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_TRUE(writer_status().drain_pending);
    TEST_ASSERT_FALSE(writer_status().invariant_failure);
  }
  TEST_ASSERT_FALSE(rolling_violation);
  TEST_ASSERT_EQUAL_UINT(slot_writes(&device), rolling_checks);
  /* Reopening from these durable bytes must establish its startup fence while
   * independently comparing both preceding volume payload states before slots. */
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
  size_t allocations = device.backing.allocation_calls;
  test_failure_memory_fail_after(&device, 0);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(checkpoint_view, &result));
  TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_FALSE(writer_status().drain_pending);
  TEST_ASSERT_FALSE(rolling_violation);
  TEST_ASSERT_EQUAL_UINT(slot_writes(&device), rolling_checks);
  size_t length = 99;
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_read(checkpoint_view, 0, data, 1, &length));
  TEST_ASSERT_EQUAL_UINT(0, length);
  device.backing.fail_after = SIZE_MAX;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&checkpoint_view, &(struct pfs_view_close_result){0}));
  device.observer = NULL;
  for (size_t i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&rolling_views[i], &(struct pfs_view_close_result){0}));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&rolling_volumes[i]));
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  if (restore_seed_slot) {
    /* The two real mutations leave the seed's allocations protected until the
     * next publication. Restore its encoded root as the older durable state:
     * both owners' cohorts now need startup's broader protection profile. */
    uint64_t generation = durable_generation(&recovered);
    unsigned older = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
      uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
      struct pfs_superblock super;
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&recovered, block, 1, encoded));
      TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(encoded, sizeof(encoded),
        PFS_POOL_BLOCKS_MIN, block, &super));
      if (super.header.birth < generation) {
        older = slot;
      }
    }
    saved_seed.header.block = older ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(encoded, sizeof(encoded), &saved_seed));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&recovered.builder,
      saved_seed.header.block, 1, encoded, sizeof(encoded)));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&recovered.builder));
    struct pfs_check_result checked;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&recovered.builder.reader, recovered.memory,
      NULL, NULL, &checked));
    TEST_ASSERT_TRUE(checked.cross_complete);
    TEST_ASSERT_TRUE(rolling_payloads_match(&recovered, 0, leaves, NULL));
    TEST_ASSERT_TRUE(rolling_payloads_match(&recovered, 1, leaves, NULL));
    struct pfs_plan_arena arena = {0};
    static struct pfs_admit_state admitted[3];
    memset(admitted, 0, sizeof(admitted));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_open(&recovered.builder.reader, recovered.memory,
      options.extent_limit, options.metadata_limit, &arena, admitted, &checked));
    unsigned selected = admitted[1].candidate.superblock.header.birth >
      admitted[0].candidate.superblock.header.birth;
    TEST_ASSERT_GREATER_THAN_UINT64(0, admitted[selected].retired_volume);
    TEST_ASSERT_EQUAL_UINT64(admitted[selected].retired_volume, admitted[selected].protected_volume);
    struct pfs_volume_id owners[2] = {0};
    size_t owner_count = 0;
    for (size_t i = 0; i < admitted[selected].map_count; i++) {
      const struct pfs_allocation_record *record = &admitted[selected].maps[i];
      const struct pfs_volume_id pool_owner = {{0}};
      if (record->state != PFS_ALLOCATION_RETIRED ||
          !memcmp(&record->owner, &pool_owner, sizeof(pool_owner))) {
        continue;
      }
      TEST_ASSERT_EQUAL(PFS_CHARGE_ORDINARY, record->charge);
      size_t owner = 0;
      while (owner < owner_count && memcmp(&record->owner, &owners[owner], sizeof(record->owner))) {
        owner++;
      }
      if (owner == owner_count) {
        TEST_ASSERT_LESS_THAN_UINT(2, owner_count);
        owners[owner_count++] = record->owner;
      }
    }
    TEST_ASSERT_EQUAL_UINT(2, owner_count);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
    TEST_ASSERT_EQUAL_UINT64(0, recovered.memory->used);
  }
  recovered.observer = observe_rolling_payloads;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &recovered.builder,
    recovered.memory, &options, &opening));
  TEST_ASSERT_FALSE(opening.writer.drain_pending);
  TEST_ASSERT_FALSE(rolling_violation);
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t volume_debt;
    TEST_ASSERT_TRUE(rolling_payloads_match(&recovered, slot, leaves, &volume_debt));
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&recovered, block, 1, encoded));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(encoded, sizeof(encoded),
      PFS_POOL_BLOCKS_MIN, block, &super));
    if (super.header.birth == opening.confirmed_generation) {
      TEST_ASSERT_EQUAL_UINT64(0, volume_debt);
    }
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_TRUE(test_failure_close(&recovered));
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
cross_volume_rolling_histories_share_and_separate_catalog_paths(void)
{
  run_cross_volume_rolling_history(false);
}

static void
startup_fences_two_owners_protected_by_nonadjacent_seed_root(void)
{
  run_cross_volume_rolling_history(true);
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
  TEST_ASSERT_EQUAL_UINT64(generation, durable_generation(&device));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
}

static uint64_t
minimum_recovery(const struct pfs_plan_limits *limits)
{
  return limits->recovery_blocks > 256 ? limits->recovery_blocks : 256;
}

static void
near_minimum_profile_quota_pool_and_memory_fund_rolling_and_fence(void)
{
  options = (struct pfs_write_options){.extent_limit = 1, .metadata_limit = 3, .random = test_random};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 3, 1, &limits));
  uint64_t recovery = minimum_recovery(&limits);
  /* One case fills effective quota; the other fills the logical pool promise.
   * Geometry stays at the format's 64 MiB minimum in both cases. */
  uint64_t guarantee = 0;
  for (unsigned tight_pool = 0; tight_pool < 2; tight_pool++) {
    build_seed_with_budgets(tight_pool ? guarantee : 4, guarantee, recovery);
    open_device();
    struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
    TEST_ASSERT_EQUAL_UINT64(1, state->file_extents);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(options.metadata_limit, state->metadata_blocks);
    TEST_ASSERT_EQUAL_UINT64(seed_objects, state->volumes[0].record.object_count);
    TEST_ASSERT_EQUAL_UINT64(seed_directories, state->volumes[0].directories);
    uint64_t deletion = (seed_objects - 1 + 4 * (seed_directories + 1)) / 5;
    TEST_ASSERT_EQUAL_UINT64(deletion, state->volumes[0].deletion_blocks);
    TEST_ASSERT_EQUAL_UINT64(state->volumes[0].record.live_blocks -
      state->volumes[0].namespace_nodes + deletion, state->volumes[0].effective_blocks);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(state->volumes[0].record.quota,
      state->volumes[0].effective_blocks);
    TEST_ASSERT_EQUAL_UINT64(seed_reserves.cow_reserve, state->candidate.root.cow.capacity);
    TEST_ASSERT_EQUAL_UINT64(seed_reserves.migration_reserve, state->candidate.root.migration.capacity);
    TEST_ASSERT_EQUAL_UINT64(recovery, state->candidate.root.recovery.capacity);
    TEST_ASSERT_EQUAL_UINT64(limits.arena_bytes, pool.writer->arena.allocation.size);
    if (tight_pool) {
      TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, limits.permanent_pool + guarantee +
        state->candidate.root.cow.capacity + state->candidate.root.migration.capacity +
        state->candidate.root.recovery.capacity);
    } else {
      TEST_ASSERT_EQUAL_UINT64(state->volumes[0].effective_blocks, state->volumes[0].record.quota);
      guarantee = PFS_POOL_BLOCKS_MIN - 2 - limits.permanent_pool -
        state->candidate.root.cow.capacity - state->candidate.root.migration.capacity -
        state->candidate.root.recovery.capacity;
    }
    /* Existing handles and the reserved arena are the entire remaining memory
     * budget. Rolling publications and their fence may not request more memory. */
    uint64_t memory_limit = device.memory->limit;
    device.memory->limit = device.memory->used;
    struct pfs_batch *batch = overwrite('B');
    size_t allocations = device.backing.allocation_calls;
    uint64_t memory = device.memory->used;
    test_failure_memory_fail_after(&device, 0);
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_GREATER_THAN_UINT64(initial_generation, durable_generation(&device));
    TEST_ASSERT_TRUE(writer_status().drain_pending);
    for (unsigned step = 0; step < 3; step++) {
      batch = overwrite('B');
      TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    }
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
    TEST_ASSERT_FALSE(writer_status().drain_pending);
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
  const struct pfs_write_options profiles[] = {{1, 4, test_random, NULL}, {2, 3, test_random, NULL}, {2, 4, test_random, NULL}};
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
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
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
  options = (struct pfs_write_options){.extent_limit = 1, .metadata_limit = 3, .random = test_random};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 3, 1, &limits));
  uint64_t recovery = minimum_recovery(&limits);
  /* Establish the fixture's actual recorded workspace before varying promises. */
  build_seed_with_budgets(64, 0, recovery);
  uint64_t guarantee = PFS_POOL_BLOCKS_MIN - 2 - limits.permanent_pool -
    seed_workspace_capacity - recovery;
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
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
computed_recovery_budget_funds_fence_and_refuses_one_below(void)
{
  /* Large declared bounds exercise recovery arithmetic on the same small
   * reachable seed, rather than claiming a populated large-workload result. */
  options = (struct pfs_write_options){.extent_limit = 4096, .metadata_limit = 2048, .random = test_random};
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
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
      TEST_ASSERT_FALSE(writer_status().drain_pending);
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
generation_boundary_funds_fence_and_refuses_next_batch(void)
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
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
      TEST_ASSERT_GREATER_THAN_UINT64(initial_generation, durable_generation(&device));
      TEST_ASSERT_FALSE(writer_status().drain_pending);
      device.backing.fail_after = SIZE_MAX;
    }
    uint64_t generation = durable_generation(&device);
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct pfs_batch *batch = NULL;
    TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_writer_prepare(&pool, &batch));
    TEST_ASSERT_NULL(batch);
    struct pfs_write_result resize;
    TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_view_resize(view, 0, &resize));
    TEST_ASSERT_EQUAL(PFS_STOPPED, resize.completion);
    TEST_ASSERT_EQUAL(PFS_LIMIT, resize.operation_status);
    TEST_ASSERT_TRUE(resize.confirmed_length_valid);
    TEST_ASSERT_EQUAL_UINT64(PFS_BLOCK_SIZE, resize.confirmed_length);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, resize.health);
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
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&cold, &status));
  TEST_ASSERT_FALSE(status.drain_pending);
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
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&file, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&v));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&cold));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&recovered.builder.reader, recovered.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(test_failure_close(&recovered));
}

static size_t
discover_overwrite_cuts(uint64_t *first_slot)
{
  open_device();
  struct pfs_batch *batch = overwrite('B');
  uint64_t base = device.ordinals[TEST_FAILURE_FLUSH];
  test_failure_trace_reset(&device);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  size_t count = 0;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, base, commit_cuts,
    TEST_FAILURE_EVENTS_MAX, &count));
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  TEST_ASSERT_EQUAL_UINT(2 * slot_writes(&device), count);
  if (first_slot) {
    for (size_t i = 0; i < device.event_count; i++) {
      const struct test_failure_event *event = &device.events[i];
      if (event->kind == TEST_FAILURE_WRITE &&
          (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1)) {
        *first_slot = event->first;
        break;
      }
    }
  }
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  return count;
}

static void
flush_boundaries_preserve_progress_and_stop_stickily(void)
{
  build_seed();
  size_t cut_count = discover_overwrite_cuts(NULL);
  /* Exercise both durability outcomes at every observed publication phase. */
  for (size_t i = 0; i < cut_count; i++) {
    const struct test_failure_flush_cut cut = commit_cuts[i];
    for (unsigned promoted = 0; promoted < 2; promoted++) {
      open_device();
      struct pfs_batch *batch = overwrite('B');
      struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + cut.ordinal,
        .mode = TEST_FAILURE_FLUSH_PREFIX, .prefix = promoted ? SIZE_MAX : 0};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
      struct pfs_write_result result;
      TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
      TEST_ASSERT_TRUE(device.triggered);
      TEST_ASSERT_FALSE(device.infrastructure_failure);
      bool uncertain = cut.publishes;
      bool user_confirmed = cut.publication > 0;
      TEST_ASSERT_EQUAL(user_confirmed ? PFS_COMPLETE : uncertain ? PFS_UNKNOWN : PFS_STOPPED,
        result.completion);
      TEST_ASSERT_EQUAL(uncertain ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED, result.health);
      if (user_confirmed) {
        TEST_ASSERT_EQUAL(PFS_OK, result.operation_status);
        TEST_ASSERT_EQUAL(uncertain ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
          result.maintenance_completion);
        TEST_ASSERT_TRUE(writer_status().drain_pending);
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
      cold_reopen_and_compare(user_confirmed || (cut.publication == 0 && cut.publishes && promoted) ? 'B' : 'A');
      TEST_ASSERT_TRUE(test_failure_close(&device));
    }
  }
}

static void
failed_next_mutation_preserves_debt_without_maintenance_failure(void)
{
  const enum test_failure_event_kind kinds[] = {
    TEST_FAILURE_READ, TEST_FAILURE_WRITE, TEST_FAILURE_FLUSH,
  };
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    build_seed();
    open_device();
    struct pfs_write_result confirmed, result;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, overwrite('B'), &confirmed));
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, confirmed.maintenance_completion);
    struct pfs_batch *batch = overwrite('C');
    struct test_failure_fault fault = {.enabled = true, .kind = kinds[i],
      .ordinal = device.ordinals[kinds[i]] + 1, .mode = TEST_FAILURE_BEFORE};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_NONE, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_OK, result.maintenance_status);
    TEST_ASSERT_EQUAL(kinds[i] == TEST_FAILURE_READ ?
      PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED, result.health);
    TEST_ASSERT_TRUE(writer_status().drain_pending);
    TEST_ASSERT_EQUAL(PFS_COMPLETE, confirmed.completion);
    if (kinds[i] != TEST_FAILURE_READ) {
      expect_read('B');
    }
    close_device();
    cold_reopen_and_compare('B');
    TEST_ASSERT_TRUE(test_failure_close(&device));
    TEST_ASSERT_TRUE(test_fixture_close(&seed));
  }
}

static void
checkpoint_flush_failures_preserve_confirmed_mutation_and_phase(void)
{
  build_seed();
  open_device();
  struct pfs_write_result confirmed, result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, overwrite('B'), &confirmed));
  uint64_t base = device.ordinals[TEST_FAILURE_FLUSH];
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, base, commit_cuts,
    TEST_FAILURE_EVENTS_MAX, &count));
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  for (size_t i = 0; i < count; i++) {
    struct test_failure_flush_cut cut = commit_cuts[i];
    for (unsigned promoted = 0; promoted < 2; promoted++) {
      open_device();
      TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, overwrite('B'), &confirmed));
      TEST_ASSERT_EQUAL(PFS_COMPLETE, confirmed.completion);
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, confirmed.maintenance_completion);
      struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + cut.ordinal,
        .mode = TEST_FAILURE_FLUSH_PREFIX, .prefix = promoted ? SIZE_MAX : 0};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
      TEST_ASSERT_EQUAL(PFS_IO, pfs_view_checkpoint(view, &result));
      TEST_ASSERT_TRUE(device.triggered);
      TEST_ASSERT_EQUAL(cut.publishes ? PFS_UNKNOWN : PFS_STOPPED, result.completion);
      TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
      TEST_ASSERT_EQUAL(cut.publishes ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
        result.maintenance_completion);
      TEST_ASSERT_EQUAL(PFS_IO, result.maintenance_status);
      TEST_ASSERT_EQUAL(cut.publishes ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
        result.health);
      TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
      TEST_ASSERT_FALSE(result.namespace_confirmed);
      TEST_ASSERT_EQUAL(PFS_COMPLETE, confirmed.completion);
      if (!cut.publishes) {
        expect_read('B');
      }
      uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
      uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
      close_device();
      TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
      TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
      cold_reopen_and_compare('B');
      TEST_ASSERT_TRUE(test_failure_close(&device));
    }
  }
}

static void
read_failures_stop_access(bool maintenance)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct pfs_write_result result;
  if (maintenance) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
  }
  uint64_t reads = device.ordinals[TEST_FAILURE_READ];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, maintenance ? pfs_view_checkpoint(view, &result) :
    pfs_writer_commit(&pool, batch, &result));
  struct read_cut {
    uint64_t ordinal;
    uint64_t flushes;
    uint64_t writes;
  } cuts[TEST_FAILURE_EVENTS_MAX];
  size_t count = 0;
  uint64_t written = 0;
  /* Select every actual read in this operation's healthy trace. */
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_WRITE) {
      written = event->ordinal - writes;
    } else if (event->kind == TEST_FAILURE_READ) {
      TEST_ASSERT_LESS_THAN_UINT(TEST_FAILURE_EVENTS_MAX, count);
      cuts[count++] = (struct read_cut){event->ordinal - reads,
        event->flush_ordinal - flushes, written};
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  for (size_t i = 0; i < count; i++) {
    open_device();
    batch = overwrite('B');
    struct pfs_write_result confirmed;
    if (maintenance) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &confirmed));
      TEST_ASSERT_EQUAL(PFS_COMPLETE, confirmed.completion);
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, confirmed.maintenance_completion);
    }
    writes = device.ordinals[TEST_FAILURE_WRITE];
    flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_READ,
      .ordinal = device.ordinals[TEST_FAILURE_READ] + cuts[i].ordinal,
      .mode = TEST_FAILURE_BEFORE};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, maintenance ? pfs_view_checkpoint(view, &result) :
      pfs_writer_commit(&pool, batch, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
    TEST_ASSERT_EQUAL(maintenance ? PFS_MAINTENANCE_STOPPED : PFS_MAINTENANCE_NONE,
      result.maintenance_completion);
    TEST_ASSERT_EQUAL(maintenance ? PFS_IO : PFS_OK, result.maintenance_status);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    TEST_ASSERT_FALSE(result.namespace_confirmed);
    struct pfs_writer_status health = writer_status();
    TEST_ASSERT_EQUAL(PFS_IO, health.failure);
    TEST_ASSERT_EQUAL(maintenance, health.drain_pending);
    TEST_ASSERT_FALSE(health.invariant_failure);
    TEST_ASSERT_EQUAL_UINT64(writes + cuts[i].writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes + cuts[i].flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    reads = device.ordinals[TEST_FAILURE_READ];
    uint8_t byte = 0xa5;
    size_t length = 99;
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_read(view, 0, &byte, 1, &length));
    TEST_ASSERT_EQUAL_UINT8(0xa5, byte);
    TEST_ASSERT_EQUAL_UINT(99, length);
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_writer_prepare(&pool, &batch));
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(view, &result));
    TEST_ASSERT_EQUAL_UINT64(reads, device.ordinals[TEST_FAILURE_READ]);
    close_device();
    TEST_ASSERT_EQUAL_UINT64(writes + cuts[i].writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes + cuts[i].flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    cold_reopen_and_compare(maintenance ? 'B' : 'A');
    TEST_ASSERT_TRUE(test_failure_close(&device));
  }
}

static void
planning_read_failures_stop_all_access(void)
{
  read_failures_stop_access(false);
}

static void
maintenance_read_failures_preserve_confirmed_progress_and_stop_access(void)
{
  read_failures_stop_access(true);
}

static void
cache_only_slot_is_not_recovered_by_later_flush(void)
{
  build_seed();
  uint64_t slot = UINT64_MAX;
  size_t cuts = discover_overwrite_cuts(&slot);
  size_t user_slot = 0;
  while (user_slot < cuts && !commit_cuts[user_slot].publishes) {
    user_slot++;
  }
  TEST_ASSERT_LESS_THAN_UINT(cuts, user_slot);
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
    .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + commit_cuts[user_slot].ordinal,
    .mode = TEST_FAILURE_CACHE_ONLY_SLOT, .block = slot};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_UNKNOWN, result.completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
  /* Direct adapter exercise, not permission to revive this core instance. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&device.builder));
  uint8_t cached[PFS_BLOCK_SIZE], durable[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&device.builder.reader, slot, 1, cached, sizeof(cached)));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&device, slot, 1, durable));
  struct pfs_superblock warm, cold;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(cached, sizeof(cached), PFS_POOL_BLOCKS_MIN, slot, &warm));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(durable, sizeof(durable), PFS_POOL_BLOCKS_MIN, slot, &cold));
  TEST_ASSERT_GREATER_THAN_UINT64(initial_generation, warm.header.birth);
  TEST_ASSERT_EQUAL_UINT64(initial_generation, cold.header.birth);
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

static uint64_t startup_generations[TEST_FAILURE_EVENTS_MAX];
static bool startup_observer_failed;

static void
observe_startup_generation(struct test_failure *adapter,
                           const struct test_failure_event *event, bool before, void *context)
{
  (void)context;
  if (before && event->kind == TEST_FAILURE_FLUSH) {
    if (event->ordinal >= TEST_FAILURE_EVENTS_MAX ||
        !read_durable_generation(adapter, &startup_generations[event->ordinal])) {
      startup_observer_failed = true;
    }
  }
}

static void
startup_cleanup_failure_keeps_confirmed_progress_and_releases_handles(void)
{
  build_seed();
  open_device();
  struct pfs_batch *batch = overwrite('B');
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &result));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
  uint64_t base = device.ordinals[TEST_FAILURE_FLUSH];
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, base, commit_cuts,
    TEST_FAILURE_EVENTS_MAX, &count));
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  struct test_failure_flush_cut first = commit_cuts[0];
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  open_device();
  batch = overwrite('B');
  struct pfs_write_result confirmed;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_writer_commit(&pool, batch, &confirmed));
  struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_FLUSH,
    .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + first.ordinal,
    .mode = TEST_FAILURE_BEFORE};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
  TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_STOPPED, result.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READABLE_STOPPED, result.health);
  TEST_ASSERT_EQUAL(PFS_COMPLETE, confirmed.completion);
  TEST_ASSERT_GREATER_THAN_UINT64(initial_generation, durable_generation(&device));
  close_device();

  /* Trace recovery from durable bytes, including its ordinary validation flush.
   * Save the durable generation before each callback independently of the writer. */
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
  recovered.observer = observe_startup_generation;
  startup_observer_failed = false;
  struct pfs_pool cold = {0};
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&cold, &recovered.builder,
    recovered.memory, &options, &opening));
  TEST_ASSERT_FALSE(startup_observer_failed);
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&recovered, 0, commit_cuts,
    TEST_FAILURE_EVENTS_MAX, &count));
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&cold));
  TEST_ASSERT_TRUE(test_failure_close(&recovered));

  for (size_t i = 0; i < count; i++) {
    const struct test_failure_flush_cut cut = commit_cuts[i];
    for (unsigned promoted = 0; promoted < 2; promoted++) {
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
      fault = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = cut.ordinal, .mode = TEST_FAILURE_FLUSH_PREFIX,
        .prefix = promoted ? SIZE_MAX : 0};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&recovered, &fault));
      TEST_ASSERT_EQUAL(PFS_IO, pfs_pool_open_writer(&cold, &recovered.builder,
        recovered.memory, &options, &opening));
      TEST_ASSERT_TRUE(recovered.triggered);
      TEST_ASSERT_FALSE(recovered.infrastructure_failure);
      TEST_ASSERT_NULL(cold.state.data);
      TEST_ASSERT_NULL(cold.writer);
      TEST_ASSERT_NULL(cold.memory);
      TEST_ASSERT_NULL(cold.reader);
      TEST_ASSERT_EQUAL_UINT64(0, recovered.memory->used);
      TEST_ASSERT_EQUAL_UINT64(startup_generations[cut.ordinal], opening.confirmed_generation);
      TEST_ASSERT_EQUAL(cut.publishes ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
        opening.writer.health);
      TEST_ASSERT_TRUE(opening.writer.drain_pending);
      TEST_ASSERT_FALSE(opening.writer.invariant_failure);
      TEST_ASSERT_EQUAL(cut.publishes ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
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
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, result.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_GREATER_THAN_UINT64(initial_generation, durable_generation(&device));
  expect_read('B');
  close_device();
}

/* A bounded configured population grows canonical allocation boundaries through
 * real writes. Its manifest describes application bytes independently of COW
 * claims, map descriptors and physical allocation. */
#define SPLIT_FILES 96u
#define SPLIT_WRITES (2u * SPLIT_FILES)
struct split_expectation {
  uint64_t generation;
  uint8_t bytes[SPLIT_FILES];
};
static struct split_expectation split_history[SPLIT_WRITES + 1];
static size_t split_history_count;
static bool split_payload_violation;
static size_t split_payload_checks;
static uint64_t split_read_cut, split_write_cut;
static bool observe_split_resource;
static uint64_t split_catalog_read_cut;

static enum pfs_status
split_source_read(void *context, size_t v, size_t object, uint64_t offset,
                  void *buffer, size_t length)
{
  (void)context;
  if (v || !object || object > SPLIT_FILES || offset > PFS_BLOCK_SIZE ||
      length > PFS_BLOCK_SIZE - offset) {
    return PFS_INVALID;
  }
  memset(buffer, 1 + (object - 1) % 64, length);
  return PFS_OK;
}

static void
build_split_seed(void)
{
  static struct pfs_build_object objects[SPLIT_FILES + 1];
  static char names[SPLIT_FILES][8];
  memset(objects, 0, sizeof(objects));
  objects[0] = (struct pfs_build_object){.id = {{3}}, .parent = UINT32_MAX,
    .kind = PFS_OBJECT_DIRECTORY};
  for (size_t i = 0; i < SPLIT_FILES; i++) {
    int length = snprintf(names[i], sizeof(names[i]), "f%03zu", i);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(names[i]));
    objects[i + 1] = (struct pfs_build_object){.id = {{(uint8_t)(10 + i)}}, .parent = 0,
      .name = {.length = (size_t)length}, .kind = PFS_OBJECT_FILE,
      .file_length = PFS_BLOCK_SIZE};
    memcpy(objects[i + 1].name.bytes, names[i], (size_t)length);
  }
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .quota_set = true, .quota = 4096, .guarantee_set = true,
    .object_count = SPLIT_FILES + 1, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &v,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 2048, .migration_reserve = 2048, .recovery_reserve = 2048};
  struct pfs_build_source source = {.read = split_source_read, .validate = source_validate};
  struct pfs_build_plan plan = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  options = (struct pfs_write_options){.extent_limit = 512, .metadata_limit = 256,
    .random = test_random};
}

static void
reset_split_history(void)
{
  split_history_count = 1;
  split_history[0].generation = 1;
  for (size_t i = 0; i < SPLIT_FILES; i++) {
    split_history[0].bytes[i] = (uint8_t)(1 + i % 64);
  }
  split_payload_checks = 0;
  split_payload_violation = false;
}

/* The configured file length requires one mapped block; either supported
 * object representation may name it. Decode its mapping before inspecting bytes. */
static bool
split_file_mapping(struct test_failure *adapter, const struct pfs_tree_context *objects,
                   uint64_t containing_birth, const struct pfs_object_record *object,
                   uint64_t *physical)
{
  struct pfs_extent_mapping mapping;
  if (object->storage_kind == PFS_STORAGE_INLINE) {
    mapping = object->inline_extent;
  } else if (object->storage_kind == PFS_STORAGE_TREE) {
    struct pfs_tree_context context = {.block = objects->block, .kind = PFS_INDEX_EXTENTS,
      .volume = objects->volume, .object = object->id};
    context.block.reference = object->tree_root;
    context.block.referring_birth = containing_birth;
    uint8_t bytes[PFS_BLOCK_SIZE];
    struct pfs_tree tree;
    if (test_failure_durable_read(adapter, object->tree_root.block, 1, bytes) != PFS_OK ||
        pfs_tree_decode(bytes, sizeof(bytes), &context, &tree) != PFS_OK || tree.level || tree.count != 1) {
      return false;
    }
    struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
      .selected_generation = context.block.selected_generation,
      .containing_birth = tree.header.birth, .features = context.block.features};
    struct pfs_extent_record extent;
    if (pfs_extent_record_decode(bytes + tree.slots[0].offset, tree.slots[0].length,
        &records, &extent) != PFS_OK) {
      return false;
    }
    mapping = extent.mapping;
  } else {
    return false;
  }
  if (mapping.logical_first || mapping.count != 1) {
    return false;
  }
  *physical = mapping.physical_first;
  return true;
}

static bool
split_objects_match(struct test_failure *adapter, struct pfs_tree_context context,
                    const uint8_t expected[SPLIT_FILES], bool seen[SPLIT_FILES])
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  if (test_failure_durable_read(adapter, context.block.reference.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), &context, &tree) != PFS_OK) {
    return false;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = context.block.selected_generation,
    .containing_birth = tree.header.birth, .features = context.block.features};
  for (size_t i = 0; i < tree.count; i++) {
    if (tree.level) {
      struct pfs_internal_record internal;
      if (pfs_internal_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          PFS_INDEX_OBJECTS, &records, &internal) != PFS_OK) {
        return false;
      }
      struct pfs_tree_context child = context;
      child.block.reference = internal.child;
      child.block.referring_birth = tree.header.birth;
      child.parent_level = tree.level;
      if (!split_objects_match(adapter, child, expected, seen)) {
        return false;
      }
    } else {
      struct pfs_object_record object;
      if (pfs_object_record_decode(bytes + tree.slots[i].offset, tree.slots[i].length,
          &records, &object) != PFS_OK) {
        return false;
      }
      if (object.kind == PFS_OBJECT_DIRECTORY) {
        if (object.id.bytes[0] != 3) {
          return false;
        }
        continue;
      }
      size_t file = object.id.bytes[0] - 10u;
      uint64_t physical;
      if (file >= SPLIT_FILES || seen[file] || object.file_length != PFS_BLOCK_SIZE ||
          !split_file_mapping(adapter, &context, tree.header.birth, &object, &physical)) {
        return false;
      }
      uint8_t payload[PFS_BLOCK_SIZE];
      if (test_failure_durable_read(adapter, physical, 1, payload) != PFS_OK) {
        return false;
      }
      for (size_t j = 0; j < sizeof(payload); j++) {
        if (payload[j] != expected[file]) {
          return false;
        }
      }
      seen[file] = true;
    }
  }
  return true;
}

static bool
split_payloads_match(struct test_failure *adapter, unsigned slot)
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
  struct pfs_tree_context catalog = {.block = context, .kind = PFS_INDEX_VOLUMES};
  catalog.block.reference = root.volumes;
  struct pfs_tree tree;
  if (test_failure_durable_read(adapter, root.volumes.block, 1, bytes) != PFS_OK ||
      pfs_tree_decode(bytes, sizeof(bytes), &catalog, &tree) != PFS_OK || tree.level || tree.count != 1) {
    return false;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = super.header.birth, .containing_birth = tree.header.birth,
    .features = super.features};
  struct pfs_volume_record v;
  if (pfs_volume_record_decode(bytes + tree.slots[0].offset, tree.slots[0].length, &records, &v) != PFS_OK) {
    return false;
  }
  size_t history = 0;
  while (history + 1 < split_history_count &&
         split_history[history + 1].generation <= super.header.birth) {
    history++;
  }
  struct pfs_tree_context objects = {.block = context, .kind = PFS_INDEX_OBJECTS, .volume = v.id};
  objects.block.reference = v.object_root;
  bool seen[SPLIT_FILES] = {0};
  if (!split_objects_match(adapter, objects, split_history[history].bytes, seen)) {
    return false;
  }
  for (size_t i = 0; i < SPLIT_FILES; i++) {
    if (!seen[i]) {
      return false;
    }
  }
  return true;
}

static void
observe_split_payloads(struct test_failure *adapter, const struct test_failure_event *event,
                       bool before, void *context)
{
  (void)context;
  bool slot = event->kind == TEST_FAILURE_WRITE &&
    (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1);
  if (before && event->kind == TEST_FAILURE_WRITE && pool.writer &&
      pool.writer->active_batch && !pool.writer->active_batch->orphan_cleanup &&
      split_history[split_history_count - 1].generation == UINT64_MAX) {
    split_history[split_history_count - 1].generation = pool.writer->map_metrics.generation;
  }
  if ((before && slot) || (!before && event->kind == TEST_FAILURE_WRITE && !slot)) {
    split_payload_checks++;
    split_payload_violation |= !split_payloads_match(adapter, 0) || !split_payloads_match(adapter, 1);
  }
  if (!before || !pool.writer) {
    return;
  }
  if (event->kind == TEST_FAILURE_READ && pool.writer->map_planning) {
    split_read_cut = event->ordinal;
  }
  if (observe_split_resource && !split_catalog_read_cut &&
      event->kind == TEST_FAILURE_READ && pool.writer->map_planning &&
      !pool.writer->map_metrics.fallback) {
    const struct pfs_admit_state *source = &pool.writer->states[pool.writer->selected];
    const struct pfs_pool_root *candidate = &pool.writer->states[2].candidate.root;
    /* Fixed-length catalog replacement preserves its inventory. A trial's
     * single extra map leaf is the sole surplus live pool block before reads. */
    if (candidate->header.birth == pool.writer->map_metrics.generation &&
        candidate->live_pool == source->candidate.root.live_pool + 1) {
      for (size_t i = 0; i < source->claim_count; i++) {
        const struct check_claim *claim = &source->claims[i];
        if (claim->first == event->first && claim->kind == PFS_INDEX_VOLUMES) {
          split_catalog_read_cut = event->ordinal;
        }
      }
    }
  }

  if (event->kind == TEST_FAILURE_WRITE && pool.writer->map_metrics.split_selected && !split_write_cut) {
    const struct pfs_admit_state *candidate = &pool.writer->states[2];
    for (size_t i = 0; i < candidate->claim_count; i++) {
      const struct check_claim *claim = &candidate->claims[i];
      if (claim->first == event->first && claim->kind == PFS_INDEX_ALLOCATION) {
        split_write_cut = event->ordinal;
      }
    }
  }
}

static void
open_split_device(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN,
    options.extent_limit, options.metadata_limit, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  struct pfs_write_open_result opening;
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  view = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, &opening));
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volume));
  reset_split_history();
  pool.writer->collect_map_metrics = true;
  device.observer = observe_split_payloads;
}

static void
acquire_split_file(size_t step)
{
  const struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_WRITE |
    PFS_FILE_CHECKPOINT | PFS_FILE_RESIZE};
  const struct pfs_trusted_context authority = {.principal = {{5}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE, .ceiling = {rights.file, PFS_DIR_LOOKUP, 0}};
  const struct pfs_object_id id = {{(uint8_t)(10 + (37 * step) % SPLIT_FILES)}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &id, PFS_SCOPE_OBJECT, &rights, &view));
}

static enum pfs_status
write_split_file(size_t step, struct pfs_write_result *result)
{
  struct split_expectation *next = &split_history[split_history_count];
  *next = split_history[split_history_count - 1];
  next->generation = UINT64_MAX;
  uint8_t byte = (uint8_t)(128 + step % 64);
  next->bytes[(37 * step) % SPLIT_FILES] = byte;
  split_history_count++;
  memset(data, byte, sizeof(data));
  enum pfs_status status = pfs_view_write(view, 0, data, sizeof(data), result);
  if (status == PFS_OK) {
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, next->generation);
    TEST_ASSERT_EQUAL_UINT64(sizeof(data), result->confirmed_bytes);
  }
  return status;
}

static void
replay_split_prefix(size_t stop)
{
  open_split_device();
  for (size_t step = 0; step < stop; step++) {
    acquire_split_file(step);
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_OK, write_split_file(step, &result));
    TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
    test_failure_trace_reset(&device);
  }
  acquire_split_file(stop);
}

static void
selected_overflow_publication_preserves_payloads_through_failure_and_recovery(void)
{
  build_split_seed();
  open_split_device();
  size_t selected = SIZE_MAX;
  struct test_failure_fault cuts[4] = {0};
  struct pfs_write_result result;
  for (size_t step = 0; step < SPLIT_WRITES; step++) {
    acquire_split_file(step);
    uint64_t bases[3] = {device.ordinals[TEST_FAILURE_READ],
      device.ordinals[TEST_FAILURE_WRITE], device.ordinals[TEST_FAILURE_FLUSH]};
    split_read_cut = split_write_cut = 0;
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, write_split_file(step, &result));
    TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
    if (pool.writer->map_metrics.split_selected) {
      selected = step;
      TEST_ASSERT_GREATER_THAN_UINT64(bases[0], split_read_cut);
      TEST_ASSERT_GREATER_THAN_UINT64(bases[1], split_write_cut);
      cuts[0] = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_READ,
        .ordinal = split_read_cut - bases[0], .mode = TEST_FAILURE_BEFORE};
      cuts[1] = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_WRITE,
        .ordinal = split_write_cut - bases[1], .mode = TEST_FAILURE_DURABLE_PREFIX, .prefix = 1};
      size_t count;
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&device, bases[2], commit_cuts,
        TEST_FAILURE_EVENTS_MAX, &count));
      TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, count);
      cuts[2] = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = commit_cuts[0].ordinal, .mode = TEST_FAILURE_FLUSH_PREFIX};
      cuts[3] = (struct test_failure_fault){.enabled = true, .kind = TEST_FAILURE_FLUSH,
        .ordinal = commit_cuts[1].ordinal, .mode = TEST_FAILURE_FLUSH_PREFIX, .prefix = SIZE_MAX};
      break;
    }
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, selected);
  TEST_ASSERT_FALSE(split_payload_violation);
  TEST_ASSERT_GREATER_THAN_UINT(0, split_payload_checks);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_FALSE(split_payload_violation);
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));

  for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
    replay_split_prefix(selected);
    struct test_failure_fault fault = cuts[i];
    fault.ordinal += device.ordinals[fault.kind];
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    TEST_ASSERT_EQUAL(PFS_IO, write_split_file(selected, &result));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    bool uncertain = i == 3;
    enum pfs_writer_health health = i == 0 || uncertain ?
      PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED;
    TEST_ASSERT_EQUAL(health, result.health);
    TEST_ASSERT_EQUAL(uncertain ? PFS_UNKNOWN : PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_NONE, result.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_OK, result.maintenance_status);
    struct pfs_writer_status stopped = writer_status();
    TEST_ASSERT_EQUAL(health, stopped.health);
    TEST_ASSERT_EQUAL(PFS_IO, stopped.failure);
    TEST_ASSERT_FALSE(stopped.invariant_failure);
    TEST_ASSERT_FALSE(split_payload_violation);
    TEST_ASSERT_TRUE(split_payloads_match(&device, 0));
    TEST_ASSERT_TRUE(split_payloads_match(&device, 1));
    uint64_t reads = device.ordinals[TEST_FAILURE_READ];
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct pfs_batch *batch = NULL;
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_writer_prepare(&pool, &batch));
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(view, &result));
    TEST_ASSERT_EQUAL_UINT64(reads, device.ordinals[TEST_FAILURE_READ]);
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    /* An uncommitted logical update must not become the expectation for a
     * later startup fence merely because that fence reaches its generation. */
    if (durable_generation(&device) < split_history[split_history_count - 1].generation) {
      split_history_count--;
    }
    close_device();
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
    recovered.observer = observe_split_payloads;
    pool = (struct pfs_pool){0};
    struct pfs_write_open_result opening;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &recovered.builder,
      recovered.memory, &options, &opening));
    TEST_ASSERT_FALSE(opening.writer.drain_pending);
    TEST_ASSERT_FALSE(split_payload_violation);
    TEST_ASSERT_TRUE(split_payloads_match(&recovered, 0));
    TEST_ASSERT_TRUE(split_payloads_match(&recovered, 1));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
    struct pfs_check_result check;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&recovered.builder.reader, recovered.memory, NULL, NULL, &check));
    TEST_ASSERT_TRUE(check.cross_complete);
    TEST_ASSERT_TRUE(test_failure_close(&recovered));
    TEST_ASSERT_TRUE(test_failure_close(&device));
  }
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
discarded_overflow_catalog_read_failure_stops_without_fallback(void)
{
  build_split_seed();
  open_split_device();
  size_t prefix = SIZE_MAX;
  uint64_t metadata_limit = 0;
  struct pfs_write_result result;
  /* Discover a configured overflow boundary through the same bounded healthy
   * history. Its metadata shape supplies a valid tight profile for the replay. */
  for (size_t step = 0; step < SPLIT_WRITES; step++) {
    acquire_split_file(step);
    TEST_ASSERT_EQUAL(PFS_OK, write_split_file(step, &result));
    if (pool.writer->map_metrics.split_selected) {
      prefix = step;
      const struct pfs_admit_volume *source =
        &pool.writer->states[pool.writer->selected].volumes[0];
      metadata_limit = source->metadata_blocks - source->namespace_nodes + source->deletion_blocks;
      TEST_ASSERT_GREATER_THAN_UINT64(source->metadata_blocks, metadata_limit);
      break;
    }
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
    test_failure_trace_reset(&device);
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, prefix);
  TEST_ASSERT_FALSE(split_payload_violation);
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  options.metadata_limit = metadata_limit;
  replay_split_prefix(prefix);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
  uint64_t read_cut = 0;
  size_t target = SIZE_MAX;
  observe_split_resource = true;
  memset(data, 0xe7, sizeof(data));
  for (size_t candidate = 0; candidate < SPLIT_FILES; candidate++) {
    acquire_split_file(candidate);
    uint64_t reads = device.ordinals[TEST_FAILURE_READ];
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    split_catalog_read_cut = 0;
    test_failure_trace_reset(&device);
    /* No manifest row is added: a refused append leaves every configured file
     * at its original length and previously confirmed expected bytes. */
    TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_view_write(view, PFS_BLOCK_SIZE, data, sizeof(data), &result));
    TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
    TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    if (pool.writer->map_metrics.split.misses & PFS_INCREMENTAL_SPLIT_RESOURCE) {
      target = candidate;
      TEST_ASSERT_GREATER_THAN_UINT64(reads, split_catalog_read_cut);
      read_cut = split_catalog_read_cut - reads;
      break;
    }
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
  }
  TEST_ASSERT_NOT_EQUAL(SIZE_MAX, target);
  TEST_ASSERT_TRUE(split_payloads_match(&device, 0));
  TEST_ASSERT_TRUE(split_payloads_match(&device, 1));
  observe_split_resource = false;
  /* Refusal must leave this instance usable, with no sealed-candidate state
   * leaking into a compatible overwrite or its funded checkpoint. */
  TEST_ASSERT_EQUAL(PFS_OK, write_split_file(target, &result));
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_FALSE(split_payload_violation);
  TEST_ASSERT_TRUE(split_payloads_match(&device, 0));
  TEST_ASSERT_TRUE(split_payloads_match(&device, 1));
  close_device();
  TEST_ASSERT_TRUE(test_failure_close(&device));

  replay_split_prefix(prefix);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&view, &(struct pfs_view_close_result){0}));
  acquire_split_file(target);
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  struct test_failure_fault fault = {.enabled = true, .kind = TEST_FAILURE_READ,
    .ordinal = device.ordinals[TEST_FAILURE_READ] + read_cut, .mode = TEST_FAILURE_BEFORE};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  memset(data, 0xe7, sizeof(data));
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_write(view, PFS_BLOCK_SIZE, data, sizeof(data), &result));
  TEST_ASSERT_TRUE(device.triggered);
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, result.health);
  TEST_ASSERT_EQUAL(PFS_STOPPED, result.completion);
  TEST_ASSERT_EQUAL(PFS_IO, result.operation_status);
  TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
  struct pfs_writer_status stopped = writer_status();
  TEST_ASSERT_EQUAL(PFS_IO, stopped.failure);
  TEST_ASSERT_FALSE(stopped.invariant_failure);
  TEST_ASSERT_FALSE(split_payload_violation);
  TEST_ASSERT_TRUE(split_payloads_match(&device, 0));
  TEST_ASSERT_TRUE(split_payloads_match(&device, 1));
  uint64_t reads = device.ordinals[TEST_FAILURE_READ];
  struct pfs_batch *batch = NULL;
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_writer_prepare(&pool, &batch));
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(view, &result));
  TEST_ASSERT_EQUAL_UINT64(reads, device.ordinals[TEST_FAILURE_READ]);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  close_device();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&recovered, &device, 0));
  recovered.observer = observe_split_payloads;
  pool = (struct pfs_pool){0};
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &recovered.builder,
    recovered.memory, &options, &opening));
  TEST_ASSERT_FALSE(opening.writer.drain_pending);
  TEST_ASSERT_FALSE(split_payload_violation);
  TEST_ASSERT_TRUE(split_payloads_match(&recovered, 0));
  TEST_ASSERT_TRUE(split_payloads_match(&recovered, 1));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&recovered.builder.reader, recovered.memory, NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_TRUE(test_failure_close(&recovered));
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

void
run_publication_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(selected_overflow_publication_preserves_payloads_through_failure_and_recovery);
  RUN_TEST(discarded_overflow_catalog_read_failure_stops_without_fallback);
  RUN_TEST(real_publications_carry_debt_and_preserve_retained_payloads);
  RUN_TEST(cross_volume_rolling_histories_share_and_separate_catalog_paths);
  RUN_TEST(startup_fences_two_owners_protected_by_nonadjacent_seed_root);
  RUN_TEST(contiguous_prepare_preserves_committed_contents);
  RUN_TEST(contiguous_prepare_joins_eligible_retained_map_boundaries);
  RUN_TEST(contiguous_prepare_respects_each_retained_map_and_claim);
  RUN_TEST(contiguous_prepare_falls_back_when_only_short_runs_exist);
  RUN_TEST(flush_boundaries_preserve_progress_and_stop_stickily);
  RUN_TEST(checkpoint_flush_failures_preserve_confirmed_mutation_and_phase);
  RUN_TEST(failed_next_mutation_preserves_debt_without_maintenance_failure);
  RUN_TEST(planning_read_failures_stop_all_access);
  RUN_TEST(maintenance_read_failures_preserve_confirmed_progress_and_stop_access);
  RUN_TEST(cache_only_slot_is_not_recovered_by_later_flush);
  RUN_TEST(replacement_and_slot_writes_fail_without_retry);
  RUN_TEST(near_minimum_profile_quota_pool_and_memory_fund_rolling_and_fence);
  RUN_TEST(ordinary_profile_quota_and_memory_refusals_preserve_ready_writer);
  RUN_TEST(opening_refuses_unfunded_pool_recovery_and_memory);
  RUN_TEST(computed_recovery_budget_funds_fence_and_refuses_one_below);
  RUN_TEST(generation_boundary_funds_fence_and_refuses_next_batch);
  RUN_TEST(startup_cleanup_failure_keeps_confirmed_progress_and_releases_handles);
  RUN_TEST(publication_callbacks_cannot_abort_or_commit_recursively);
}
