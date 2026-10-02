/* SPDX-License-Identifier: MPL-2.0 */
#include "incremental_tests.h"
#include "incremental_map.h"
#include "failure.h"
#include "support.h"
#include "unity.h"
#include <string.h>

#define SOURCE_LEAVES 9u
#define SOURCE_PARENTS 3u
#define SOURCE_NODES (SOURCE_LEAVES + SOURCE_PARENTS + 1)
#define SOURCE_FIRST 5u
#define OLD_POOL_ROOT (SOURCE_FIRST + SOURCE_NODES)
#define OLD_CATALOG (OLD_POOL_ROOT + 1)
#define FREE_FIRST (OLD_CATALOG + 1)
#define FREE_LENGTH 4096u
#define RECORDS_MAX 512u

static struct test_fixture fixture;
static struct test_failure failed_reader;
static struct pfs_plan_arena arena;
static struct pfs_incremental_map incremental;
static struct pfs_edit_workspace workspace;
static struct pfs_allocation_record source[RECORDS_MAX];
static struct pfs_allocation_record preserved_source[RECORDS_MAX];
static struct pfs_map_change logical[16];
static size_t source_count, logical_count;
static size_t leaf_first[SOURCE_LEAVES], leaf_count[SOURCE_LEAVES];
static uint8_t original[SOURCE_NODES][PFS_BLOCK_SIZE];
static uint8_t record_bytes[46][PFS_ALLOCATION_RECORD_SIZE];
static struct pfs_encoded_record encoded[46];
static uint8_t record_buffer[PFS_BLOCK_SIZE];
static uint8_t walk_bytes[PFS_TREE_DEPTH_MAX][PFS_BLOCK_SIZE];
static struct pfs_tree walk_trees[PFS_TREE_DEPTH_MAX];
static bool emitted_seen[RECORDS_MAX], shared_seen[SOURCE_NODES];
static size_t walked_records, walked_emitted;
static struct pfs_map_plan plan;
static size_t candidate_count;

static struct pfs_block_context
block_context(uint64_t generation)
{
  return (struct pfs_block_context){
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = generation,
    .referring_birth = generation, .pool = {{1}},
  };
}

static bool
same_state(const struct pfs_allocation_record *a,
           const struct pfs_allocation_record *b)
{
  return a->state == b->state && a->charge == b->charge &&
    a->birth == b->birth && a->retirement == b->retirement &&
    memcmp(a->owner.bytes, b->owner.bytes, PFS_ID_SIZE) == 0;
}

static void
check_record(const struct pfs_allocation_record *expected,
             const struct pfs_allocation_record *actual)
{
  TEST_ASSERT_EQUAL_UINT64(expected->first, actual->first);
  TEST_ASSERT_EQUAL_UINT64(expected->count, actual->count);
  TEST_ASSERT_TRUE(same_state(expected, actual));
}

static struct pfs_reference
source_reference(size_t index)
{
  return (struct pfs_reference){SOURCE_FIRST + index, 1,
    PFS_BLOCK_TREE, PFS_FORMAT_VERSION};
}

static void
encode_source(size_t index, uint16_t level, size_t first, size_t count)
{
  struct pfs_reference ref = source_reference(index);
  struct pfs_tree_context context = {
    .block = block_context(1), .kind = PFS_INDEX_ALLOCATION,
  };
  context.block.reference = ref;
  struct pfs_record_context records = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1,
    .containing_birth = 1,
  };
  struct pfs_tree tree = {
    .header = {.type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION,
      .pool = {{1}}, .block = ref.block, .birth = 1},
    .kind = PFS_INDEX_ALLOCATION, .level = level, .count = count,
  };
  for (size_t i = 0; i < count; i++) {
    size_t length;
    if (!level) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_encode(record_bytes[i],
        sizeof(record_bytes[i]), &records, &source[first + i], &length));
    } else {
      size_t child = first + i;
      size_t leaf = level == 1 ? child : (child - SOURCE_LEAVES) * 3;
      struct pfs_internal_record internal = {
        .child = source_reference(child), .minimum = {.length = 8},
      };
      test_put_u64(internal.minimum.bytes, source[leaf_first[leaf]].first);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_encode(record_bytes[i],
        sizeof(record_bytes[i]), PFS_INDEX_ALLOCATION, &records, &internal, &length));
    }
    encoded[i] = (struct pfs_encoded_record){record_bytes[i], length};
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(original[index], PFS_BLOCK_SIZE,
    &context, &tree, encoded));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, ref.block, 1,
    original[index], PFS_BLOCK_SIZE));
}

static void
start_case(const size_t counts[SOURCE_LEAVES])
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  arena = (struct pfs_plan_arena){0};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 512, 128, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_init(&arena, &incremental));
  source_count = logical_count = 0;
  uint64_t next = 1;
  for (size_t leaf = 0; leaf < SOURCE_LEAVES; leaf++) {
    leaf_first[leaf] = source_count;
    leaf_count[leaf] = counts[leaf];
    TEST_ASSERT_TRUE(counts[leaf] > 0 && counts[leaf] <= 46);
    for (size_t i = 0; i < counts[leaf]; i++) {
      struct pfs_allocation_record value = {.first = next, .count = 4,
        .state = PFS_ALLOCATION_VOLUME, .owner = {{2}}, .birth = 1};
      value.owner.bytes[1] = source_count + 1;
      if (!source_count) {
        value.count = SOURCE_FIRST - 1;
      } else if (source_count == 1) {
        value.count = SOURCE_NODES + 2;
        value.state = PFS_ALLOCATION_POOL;
        value.owner = (struct pfs_volume_id){0};
      } else if (source_count == 2) {
        value.count = FREE_LENGTH;
        value.state = PFS_ALLOCATION_FREE;
        value.owner = (struct pfs_volume_id){0};
        value.birth = 0;
      }
      TEST_ASSERT_LESS_THAN_UINT(RECORDS_MAX, source_count);
      source[source_count++] = value;
      next += value.count;
    }
  }
  source[source_count - 1].count += PFS_POOL_BLOCKS_MIN - 1 - next;
}

static void
load_source(void)
{
  memcpy(preserved_source, source, source_count * sizeof(*source));
  for (size_t i = 0; i < SOURCE_LEAVES; i++) {
    encode_source(i, 0, leaf_first[i], leaf_count[i]);
  }
  for (size_t i = 0; i < SOURCE_PARENTS; i++) {
    encode_source(SOURCE_LEAVES + i, 1, i * 3, 3);
  }
  encode_source(SOURCE_NODES - 1, 2, SOURCE_LEAVES, SOURCE_PARENTS);
  struct pfs_block_context context = block_context(3);
  context.reference = source_reference(SOURCE_NODES - 1);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_load(&incremental,
    &fixture.builder.reader, &context, source, source_count, &workspace));
  TEST_ASSERT_TRUE(arena.limits.pool_blocks <= FREE_LENGTH);
  for (size_t i = 0; i < arena.limits.pool_blocks; i++) {
    incremental.ids[i] = FREE_FIRST + i;
  }
}

static void
change_range(size_t index, uint64_t offset, uint64_t count, bool make_free)
{
  TEST_ASSERT_LESS_THAN_UINT(16, logical_count);
  struct pfs_map_change *change = &logical[logical_count++];
  change->before = source[index];
  change->before.first += offset;
  change->before.count = count;
  change->after = change->before;
  if (make_free) {
    change->after = (struct pfs_allocation_record){
      .first = change->before.first, .count = count,
    };
  } else {
    change->after.owner.bytes[0] = 99;
    change->after.birth = 4;
  }
}

/* Derive canonical intervals by independent per-block state, starting afresh
 * every round. The production interval editor is deliberately not the oracle. */
static void
renew_candidate(bool claim_prefix)
{
  candidate_count = 0;
  size_t input = 0;
  size_t claims = claim_prefix ? incremental.marked_count + 2 : 0;
  for (uint64_t block = 1; block < PFS_POOL_BLOCKS_MIN - 1; block++) {
    while (block >= source[input].first + source[input].count) {
      input++;
    }
    struct pfs_allocation_record value = source[input];
    for (size_t i = 0; i < logical_count; i++) {
      if (block >= logical[i].before.first &&
          block - logical[i].before.first < logical[i].before.count) {
        value = logical[i].after;
      }
    }
    if (block == OLD_POOL_ROOT || block == OLD_CATALOG ||
        pfs_incremental_map_retired(&incremental, block)) {
      value.state = PFS_ALLOCATION_RETIRED;
      value.charge = PFS_CHARGE_RECOVERY;
      value.retirement = 4;
    }
    if (block >= FREE_FIRST && block - FREE_FIRST < claims) {
      TEST_ASSERT_EQUAL(PFS_ALLOCATION_FREE, source[input].state);
      value = (struct pfs_allocation_record){.state = PFS_ALLOCATION_POOL, .birth = 4};
    }
    value.first = block;
    value.count = 1;
    if (candidate_count && same_state(&arena.maps[2][candidate_count - 1], &value)) {
      arena.maps[2][candidate_count - 1].count++;
    } else {
      TEST_ASSERT_TRUE(candidate_count < arena.limits.records);
      arena.maps[2][candidate_count++] = value;
    }
  }
}

static void
close_candidate(void)
{
  bool again;
  size_t previous = 0;
  do {
    renew_candidate(true);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_close(&incremental,
      arena.maps[2], candidate_count, &again));
    if (again) {
      TEST_ASSERT_TRUE(incremental.marked_count > previous);
      TEST_ASSERT_TRUE(incremental.marked_count <= incremental.source_count);
      previous = incremental.marked_count;
    }
  } while (again);
}

static void
walk_result(struct pfs_reference ref, uint64_t referring_birth,
            uint16_t parent_level, size_t depth)
{
  TEST_ASSERT_LESS_THAN_UINT(PFS_TREE_DEPTH_MAX, depth);
  size_t emitted = 0;
  while (emitted < plan.node_count && plan.nodes[emitted].block != ref.block) {
    emitted++;
  }
  uint8_t *bytes = walk_bytes[depth];
  if (emitted < plan.node_count) {
    TEST_ASSERT_FALSE(emitted_seen[emitted]);
    emitted_seen[emitted] = true;
    walked_emitted++;
    memcpy(bytes, plan.blocks + emitted * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
  } else {
    TEST_ASSERT_TRUE(ref.block >= SOURCE_FIRST && ref.block < SOURCE_FIRST + SOURCE_NODES);
    size_t index = ref.block - SOURCE_FIRST;
    TEST_ASSERT_FALSE(pfs_incremental_map_retired(&incremental, ref.block));
    shared_seen[index] = true;
    TEST_ASSERT_EQUAL_UINT64(1, ref.birth);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, ref.block, 1,
      bytes, PFS_BLOCK_SIZE));
    TEST_ASSERT_EQUAL_MEMORY(original[index], bytes, PFS_BLOCK_SIZE);
  }
  struct pfs_tree_context context = {
    .block = block_context(4), .kind = PFS_INDEX_ALLOCATION,
    .parent_level = parent_level,
  };
  context.block.reference = ref;
  context.block.referring_birth = referring_birth;
  struct pfs_tree *tree = &walk_trees[depth];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, PFS_BLOCK_SIZE, &context, tree));
  struct pfs_record_context records = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 4,
    .containing_birth = tree->header.birth,
  };
  for (size_t i = 0; i < tree->count; i++) {
    const uint8_t *data = bytes + tree->slots[i].offset;
    size_t length = tree->slots[i].length;
    if (tree->level) {
      struct pfs_internal_record internal;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(data, length,
        PFS_INDEX_ALLOCATION, &records, &internal));
      size_t first = walked_records;
      walk_result(internal.child, tree->header.birth, tree->level, depth + 1);
      uint8_t minimum[8];
      test_put_u64(minimum, arena.maps[2][first].first);
      TEST_ASSERT_EQUAL_MEMORY(minimum, internal.minimum.bytes, 8);
    } else {
      struct pfs_allocation_record allocation;
      TEST_ASSERT_LESS_THAN_UINT(candidate_count, walked_records);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_decode(data, length, &records, &allocation));
      check_record(&arena.maps[2][walked_records++], &allocation);
    }
  }
}

static void
check_pool_inventory(void)
{
  TEST_ASSERT_EQUAL_UINT(plan.node_count + 2, plan.allocation_count);
  TEST_ASSERT_TRUE(plan.node_count <= RECORDS_MAX);
  TEST_ASSERT_NOT_EQUAL(plan.pool_root_block, plan.catalog_blocks[0]);
  for (size_t i = 0; i < plan.node_count; i++) {
    TEST_ASSERT_TRUE(plan.nodes[i].block >= FREE_FIRST &&
      plan.nodes[i].block - FREE_FIRST < plan.allocation_count);
    for (size_t j = 0; j < i; j++) {
      TEST_ASSERT_NOT_EQUAL(plan.nodes[i].block, plan.nodes[j].block);
    }
    TEST_ASSERT_NOT_EQUAL(plan.nodes[i].block, plan.pool_root_block);
    TEST_ASSERT_NOT_EQUAL(plan.nodes[i].block, plan.catalog_blocks[0]);
  }
  TEST_ASSERT_TRUE(plan.pool_root_block >= FREE_FIRST &&
    plan.pool_root_block - FREE_FIRST < plan.allocation_count);
  TEST_ASSERT_TRUE(plan.catalog_blocks[0] >= FREE_FIRST &&
    plan.catalog_blocks[0] - FREE_FIRST < plan.allocation_count);
}

static void
check_local_result(void)
{
  TEST_ASSERT_EQUAL_UINT(0, incremental.fallback);
  size_t allocations = fixture.allocation_calls;
  uint64_t reads = fixture.reads, writes = fixture.writes;
  struct pfs_block_context context = block_context(4);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_encode(&incremental, &context,
    arena.maps[2], candidate_count, 1, &workspace, record_buffer, &plan));
  TEST_ASSERT_EQUAL_UINT64(allocations, fixture.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT(incremental.marked_count, plan.node_count);
  check_pool_inventory();
  memset(emitted_seen, 0, sizeof(emitted_seen));
  memset(shared_seen, 0, sizeof(shared_seen));
  walked_records = walked_emitted = 0;
  walk_result(plan.root, 4, 0, 0);
  TEST_ASSERT_EQUAL_UINT(candidate_count, walked_records);
  TEST_ASSERT_EQUAL_UINT(plan.node_count, walked_emitted);
  for (size_t i = 0; i < SOURCE_NODES; i++) {
    TEST_ASSERT_EQUAL(!pfs_incremental_map_retired(&incremental, SOURCE_FIRST + i), shared_seen[i]);
  }
}

static void
finish_case(void)
{
  TEST_ASSERT_EQUAL_MEMORY(preserved_source, source, source_count * sizeof(*source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
check_funded_bulk_result(void)
{
  /* The alternative starts from the same immutable work with every source
   * node retired, dropping the provisional local claims before bulk sealing. */
  renew_candidate(false);
  const struct pfs_reusable_range reusable = {FREE_FIRST, FREE_LENGTH};
  struct pfs_block_context context = block_context(4);
  size_t allocations = fixture.allocation_calls;
  uint64_t reads = fixture.reads, writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context,
    arena.maps[2], candidate_count, &reusable, 1, 1, &plan));
  TEST_ASSERT_EQUAL_UINT64(allocations, fixture.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_TRUE(plan.allocation_count <= arena.limits.pool_blocks);
  candidate_count = plan.record_count;
  size_t input = 0, output = 0;
  for (uint64_t block = 1; block < PFS_POOL_BLOCKS_MIN - 1; block++) {
    while (block >= source[input].first + source[input].count) {
      input++;
    }
    while (block >= plan.records[output].first + plan.records[output].count) {
      output++;
      TEST_ASSERT_LESS_THAN_UINT(plan.record_count, output);
    }
    struct pfs_allocation_record expected = source[input];
    for (size_t i = 0; i < logical_count; i++) {
      if (block >= logical[i].before.first &&
          block - logical[i].before.first < logical[i].before.count) {
        expected = logical[i].after;
      }
    }
    if (block >= SOURCE_FIRST && block <= OLD_CATALOG) {
      expected.state = PFS_ALLOCATION_RETIRED;
      expected.charge = PFS_CHARGE_RECOVERY;
      expected.retirement = 4;
    }
    if (block >= FREE_FIRST && block - FREE_FIRST < plan.allocation_count) {
      expected = (struct pfs_allocation_record){.state = PFS_ALLOCATION_POOL, .birth = 4};
    }
    TEST_ASSERT_TRUE(same_state(&expected, &plan.records[output]));
  }
  for (size_t i = 1; i < plan.record_count; i++) {
    TEST_ASSERT_FALSE(same_state(&plan.records[i - 1], &plan.records[i]));
  }
  check_pool_inventory();
  memset(emitted_seen, 0, sizeof(emitted_seen));
  memset(shared_seen, 0, sizeof(shared_seen));
  walked_records = walked_emitted = 0;
  walk_result(plan.root, 4, 0, 0);
  TEST_ASSERT_EQUAL_UINT(plan.record_count, walked_records);
  TEST_ASSERT_EQUAL_UINT(plan.node_count, walked_emitted);
}

static void
local_change_shares_remote_subtree_and_fixed_input_prefix(void)
{
  const size_t counts[] = {3, 6, 6, 6, 6, 6, 6, 6, 6};
  start_case(counts);
  change_range(leaf_first[4] + 2, 1, 2, false);
  /* These lower IDs become free in this publication and remain ineligible. */
  change_range(0, 0, source[0].count, true);
  load_source();
  close_candidate();
  check_local_result();
  TEST_ASSERT_FALSE(pfs_incremental_map_retired(&incremental,
    source_reference(SOURCE_LEAVES + 2).block));
  finish_case();
}

static void
overflow_redistributes_across_parent_boundary(void)
{
  const size_t counts[] = {3, 6, 6, 46, 6, 6, 6, 6, 6};
  start_case(counts);
  change_range(leaf_first[3] + 20, 1, 2, false);
  load_source();
  close_candidate();
  check_local_result();
  TEST_ASSERT_TRUE(pfs_incremental_map_retired(&incremental, source_reference(2).block));
  TEST_ASSERT_TRUE(incremental.redistribution_leaves > 0);
  finish_case();
}

static void
underflow_redistributes_across_parent_boundary(void)
{
  const size_t counts[] = {3, 6, 6, 1, 1, 1, 6, 6, 6};
  start_case(counts);
  for (size_t leaf = 3; leaf <= 5; leaf++) {
    change_range(leaf_first[leaf], 0, source[leaf_first[leaf]].count, true);
  }
  load_source();
  close_candidate();
  check_local_result();
  TEST_ASSERT_TRUE(pfs_incremental_map_retired(&incremental, source_reference(2).block));
  TEST_ASSERT_TRUE(incremental.redistribution_leaves > 0);
  finish_case();
}

static void
first_leaf_overflow_uses_successor_slack(void)
{
  const size_t counts[] = {46, 6, 6, 6, 6, 6, 6, 6, 6};
  start_case(counts);
  change_range(leaf_first[0] + 20, 1, 2, false);
  load_source();
  close_candidate();
  check_local_result();
  TEST_ASSERT_TRUE(pfs_incremental_map_retired(&incremental, source_reference(1).block));
  TEST_ASSERT_FALSE(pfs_incremental_map_retired(&incremental,
    source_reference(SOURCE_LEAVES + 2).block));
  TEST_ASSERT_TRUE(incremental.redistribution_leaves > 0);
  finish_case();
}

static void
canonical_straddling_repairs_both_run_endpoints(void)
{
  const size_t counts[] = {3, 6, 6, 1, 6, 6, 6, 6, 6};
  start_case(counts);
  size_t left = leaf_first[3] - 1;
  size_t right = leaf_first[4];
  source[left].state = source[right].state = PFS_ALLOCATION_FREE;
  source[left].owner = source[right].owner = (struct pfs_volume_id){0};
  source[left].birth = source[right].birth = 0;
  change_range(leaf_first[3], 0, source[leaf_first[3]].count, true);
  load_source();
  close_candidate();
  check_local_result();
  TEST_ASSERT_TRUE(pfs_incremental_map_retired(&incremental, source_reference(2).block));
  TEST_ASSERT_TRUE(pfs_incremental_map_retired(&incremental, source_reference(4).block));
  finish_case();
}

static void
global_change_requests_bulk_without_encoding_local_padding(void)
{
  const size_t counts[] = {3, 1, 1, 1, 1, 1, 1, 1, 1};
  start_case(counts);
  for (size_t leaf = 1; leaf < SOURCE_LEAVES; leaf++) {
    change_range(leaf_first[leaf], 0, source[leaf_first[leaf]].count, true);
  }
  load_source();
  close_candidate();
  TEST_ASSERT_TRUE(incremental.fallback & PFS_INCREMENTAL_GLOBAL);
  TEST_ASSERT_TRUE(incremental.fallback & PFS_INCREMENTAL_UNDERFLOW);
  TEST_ASSERT_EQUAL_UINT(incremental.source_count, incremental.marked_count);
  TEST_ASSERT_TRUE(incremental.growth_passes <= incremental.source_count);
  check_funded_bulk_result();
  finish_case();
}

static void
exhausted_overflow_requests_bulk(void)
{
  const size_t counts[] = {46, 46, 46, 46, 46, 46, 46, 46, 46};
  start_case(counts);
  for (size_t leaf = 0; leaf < SOURCE_LEAVES; leaf++) {
    change_range(leaf_first[leaf] + 20, 1, 2, false);
  }
  load_source();
  close_candidate();
  TEST_ASSERT_TRUE(incremental.fallback & PFS_INCREMENTAL_GLOBAL);
  TEST_ASSERT_TRUE(incremental.fallback & PFS_INCREMENTAL_OVERFLOW);
  TEST_ASSERT_TRUE(incremental.failed_run_records >
    incremental.failed_run_leaves * PFS_INCREMENTAL_LEAF_RECORDS);
  check_funded_bulk_result();
  finish_case();
}

static void
source_mismatch_is_corruption_without_fallback(void)
{
  const size_t counts[] = {3, 6, 6, 6, 6, 6, 6, 6, 6};
  start_case(counts);
  load_source();
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_init(&arena, &incremental));
  struct pfs_block_context context = block_context(3);
  context.reference = source_reference(SOURCE_NODES - 1);
  /* The encoded source remains healthy; its independently supplied summary
   * describes a different owner and must not be accepted as the same input. */
  source[leaf_first[4]].owner.bytes[0] = 88;
  uint64_t writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_incremental_map_load(&incremental,
    &fixture.builder.reader, &context, source, source_count, &workspace));
  TEST_ASSERT_EQUAL_UINT(0, incremental.fallback);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  source[leaf_first[4]] = preserved_source[leaf_first[4]];
  /* A healthy checksum does not permit a noncanonical reserved byte. */
  memcpy(record_buffer, original[SOURCE_NODES - 1], PFS_BLOCK_SIZE);
  record_buffer[14] = 1;
  test_checksum(record_buffer, PFS_BLOCK_SIZE, 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, context.reference.block,
    1, record_buffer, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_init(&arena, &incremental));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_incremental_map_load(&incremental,
    &fixture.builder.reader, &context, source, source_count, &workspace));
  TEST_ASSERT_EQUAL_UINT(0, incremental.fallback);
  finish_case();
}

static void
adapter_read_error_is_not_an_optimization_miss(void)
{
  const size_t counts[] = {3, 6, 6, 6, 6, 6, 6, 6, 6};
  start_case(counts);
  load_source();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&failed_reader, PFS_POOL_BLOCKS_MIN,
    arena.limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &fixture.builder.reader));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_init(&arena, &incremental));
  struct pfs_block_context context = block_context(3);
  context.reference = source_reference(SOURCE_NODES - 1);
  uint64_t read_base = failed_reader.ordinals[TEST_FAILURE_READ];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_load(&incremental,
    &failed_reader.builder.reader, &context, source, source_count, &workspace));
  uint64_t read_cut = 0;
  for (size_t i = 0; i < failed_reader.event_count; i++) {
    const struct test_failure_event *event = &failed_reader.events[i];
    if (event->kind == TEST_FAILURE_READ && event->status == PFS_OK) {
      read_cut = event->ordinal - read_base;
      break;
    }
  }
  TEST_ASSERT_TRUE(read_cut > 0);
  test_failure_trace_reset(&failed_reader);
  struct test_failure_fault fault = {
    .kind = TEST_FAILURE_READ,
    .ordinal = failed_reader.ordinals[TEST_FAILURE_READ] + read_cut,
    .mode = TEST_FAILURE_BEFORE, .enabled = true,
  };
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&failed_reader, &fault));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_incremental_map_init(&arena, &incremental));
  TEST_ASSERT_EQUAL(PFS_IO, pfs_incremental_map_load(&incremental,
    &failed_reader.builder.reader, &context, source, source_count, &workspace));
  TEST_ASSERT_TRUE(failed_reader.triggered);
  TEST_ASSERT_FALSE(failed_reader.infrastructure_failure);
  TEST_ASSERT_EQUAL_UINT(0, incremental.fallback);
  TEST_ASSERT_TRUE(test_failure_close(&failed_reader));
  finish_case();
}

void
run_incremental_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(local_change_shares_remote_subtree_and_fixed_input_prefix);
  RUN_TEST(overflow_redistributes_across_parent_boundary);
  RUN_TEST(underflow_redistributes_across_parent_boundary);
  RUN_TEST(first_leaf_overflow_uses_successor_slack);
  RUN_TEST(canonical_straddling_repairs_both_run_endpoints);
  RUN_TEST(global_change_requests_bulk_without_encoding_local_padding);
  RUN_TEST(exhausted_overflow_requests_bulk);
  RUN_TEST(source_mismatch_is_corruption_without_fallback);
  RUN_TEST(adapter_read_error_is_not_an_optimization_miss);
}
