/* SPDX-License-Identifier: MPL-2.0 */
#include "plan_tests.h"
#include "plan.h"
#include "support.h"
#include "unity.h"
#include <string.h>

static struct test_fixture fixture;
static struct pfs_plan_arena arena;

static void
limits_and_arena(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(UINT64_C(4) * 1024 * 1024 / 4,
    8192, 4096, 16, &limits));
  TEST_ASSERT_EQUAL_UINT64(726, limits.pool_blocks);
  TEST_ASSERT_EQUAL_UINT64(32407, limits.records);
  TEST_ASSERT_EQUAL_UINT64(2562, limits.recovery_blocks);
  TEST_ASSERT_EQUAL_UINT64(788, limits.permanent_pool);
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, limits.arena_bytes - 1));
  arena = (struct pfs_plan_arena){0};
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_EQUAL_UINT64(0, fixture.allocation_calls);
  fixture.memory.limit++;
  fixture.fail_after = 0;
  TEST_ASSERT_EQUAL(PFS_NO_MEMORY, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  TEST_ASSERT_NULL(arena.allocation.data);
  fixture.fail_after = SIZE_MAX;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  TEST_ASSERT_EQUAL_UINT64(limits.arena_bytes, fixture.memory.used);
  TEST_ASSERT_EQUAL_PTR((uint8_t *)arena.allocation.data + limits.arena_bytes,
    arena.scratch + PFS_PLAN_SCRATCH_BYTES);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(UINT64_C(64) * 1024 * 1024 / 4,
    262144, 65536, 16, &limits));
  TEST_ASSERT_EQUAL_UINT64(18570, limits.pool_blocks);
  TEST_ASSERT_EQUAL_UINT64(840527, limits.records);
  TEST_ASSERT_EQUAL_UINT64(56094, limits.recovery_blocks);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(UINT64_C(256) * 1024 * 1024 / 4,
    524288, 131072, 16, &limits));
  TEST_ASSERT_EQUAL_UINT64(37105, limits.pool_blocks);
  TEST_ASSERT_EQUAL_UINT64(1680103, limits.records);
  TEST_ASSERT_EQUAL_UINT64(111699, limits.recovery_blocks);
  struct pfs_plan_limits saved = limits;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_limits(PFS_POOL_BLOCKS_MAX,
    PFS_RECORD_COUNT_MAX, PFS_POOL_BLOCKS_MAX - 2, 16, &limits));
  TEST_ASSERT_EQUAL_MEMORY(&saved, &limits, sizeof(limits));
}

static void
interval_changes_are_private_and_canonical(void)
{
  struct pfs_record_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 3, .containing_birth = 3,
  };
  struct pfs_allocation_record base[] = {
    {.first = 1, .count = 10, .state = PFS_ALLOCATION_VOLUME, .owner = {{9}}, .birth = 1},
    {.first = 11, .count = PFS_POOL_BLOCKS_MIN - 12},
  };
  struct pfs_allocation_record original[2];
  memcpy(original, base, sizeof(base));
  struct pfs_map_change changes[] = {
    {.before = {.first = 3, .count = 4, .state = PFS_ALLOCATION_VOLUME, .owner = {{9}}, .birth = 1},
     .after = {.first = 3, .count = 4, .state = PFS_ALLOCATION_RETIRED,
       .charge = PFS_CHARGE_ORDINARY, .owner = {{9}}, .birth = 1, .retirement = 3}},
    {.before = {.first = 11, .count = 5},
     .after = {.first = 11, .count = 5, .state = PFS_ALLOCATION_VOLUME, .owner = {{9}}, .birth = 3}},
  };
  struct pfs_allocation_record output[6];
  size_t count = 99;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_map_apply(&context, base, 2, changes, 2, output, 2, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_apply(&context, base, 2, changes, 2, output, 6, &count));
  TEST_ASSERT_EQUAL_UINT(5, count);
  const uint64_t first[] = {1, 3, 7, 11, 16};
  const uint64_t length[] = {2, 4, 4, 5, PFS_POOL_BLOCKS_MIN - 17};
  const uint8_t states[] = {PFS_ALLOCATION_VOLUME, PFS_ALLOCATION_RETIRED,
    PFS_ALLOCATION_VOLUME, PFS_ALLOCATION_VOLUME, PFS_ALLOCATION_FREE};
  for (size_t i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_UINT64(first[i], output[i].first);
    TEST_ASSERT_EQUAL_UINT64(length[i], output[i].count);
    TEST_ASSERT_EQUAL_UINT(states[i], output[i].state);
  }
  TEST_ASSERT_EQUAL_UINT64(3, output[1].retirement);
  TEST_ASSERT_EQUAL_UINT64(1, output[2].birth);
  TEST_ASSERT_EQUAL_UINT64(3, output[3].birth);
  /* A free transition coalesces a fully freed map; no padding record remains. */
  struct pfs_map_change free_change = {
    .before = base[0], .after = {.first = 1, .count = 10},
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 1, &count));
  TEST_ASSERT_EQUAL_UINT(1, count);
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, output[0].count);
  TEST_ASSERT_EQUAL_UINT(PFS_ALLOCATION_FREE, output[0].state);
  free_change.before.owner.bytes[0] = 8;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 6, &count));
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
}

static bool
selected(const struct pfs_map_plan *plan, uint64_t block)
{
  for (size_t i = 0; i < plan->allocation_count; i++) {
    if (plan->nodes[i].block == block) {
      return true;
    }
  }
  return false;
}

static void
check_map(const struct pfs_map_plan *plan, const struct pfs_block_context *context,
          const struct pfs_allocation_record *base, size_t base_count)
{
  /* Independently derive per-block ownership from the original map and reserved
   * IDs, without using the candidate encoder/checker as the expected result. */
  size_t source = 0, target = 0;
  for (uint64_t block = 1; block < context->block_count - 1; block++) {
    while (block >= base[source].first + base[source].count) {
      source++;
    }
    while (block >= plan->records[target].first + plan->records[target].count) {
      target++;
      TEST_ASSERT_LESS_THAN(plan->record_count, target);
    }
    bool allocated = selected(plan, block);
    TEST_ASSERT_EQUAL_UINT(allocated ? PFS_ALLOCATION_POOL : base[source].state,
      plan->records[target].state);
    TEST_ASSERT_EQUAL_UINT64(allocated ? context->selected_generation : base[source].birth,
      plan->records[target].birth);
    TEST_ASSERT_EQUAL_UINT64(allocated ? 0 : base[source].retirement,
      plan->records[target].retirement);
    TEST_ASSERT_EQUAL_UINT(allocated ? 0 : base[source].charge, plan->records[target].charge);
    if (allocated) {
      const struct pfs_volume_id zero = {0};
      TEST_ASSERT_EQUAL_MEMORY(zero.bytes, plan->records[target].owner.bytes, PFS_ID_SIZE);
      TEST_ASSERT_EQUAL_UINT(PFS_ALLOCATION_FREE, base[source].state);
    } else {
      TEST_ASSERT_EQUAL_MEMORY(base[source].owner.bytes, plan->records[target].owner.bytes, PFS_ID_SIZE);
    }
  }
  TEST_ASSERT_EQUAL_UINT(base_count - 1, source);
  TEST_ASSERT_EQUAL_UINT(plan->record_count - 1, target);
  size_t records = 0;
  size_t incoming[256] = {0};
  TEST_ASSERT_LESS_OR_EQUAL_UINT(256, plan->node_count);
  for (size_t i = 0; i < plan->node_count; i++) {
    struct pfs_tree_context tree_context = {
      .block = *context, .kind = PFS_INDEX_ALLOCATION,
    };
    tree_context.block.reference = (struct pfs_reference){
      plan->nodes[i].block, context->selected_generation, PFS_BLOCK_TREE, PFS_FORMAT_VERSION};
    tree_context.block.referring_birth = context->selected_generation;
    struct pfs_tree tree;
    const uint8_t *bytes = plan->blocks + i * PFS_BLOCK_SIZE;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, PFS_BLOCK_SIZE, &tree_context, &tree));
    TEST_ASSERT_GREATER_THAN_UINT(0, tree.count);
    struct pfs_record_context record_context = {
      .block_count = context->block_count, .selected_generation = context->selected_generation,
      .containing_birth = context->selected_generation,
    };
    for (size_t j = 0; j < tree.count; j++) {
      if (tree.level) {
        struct pfs_internal_record ref;
        TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(bytes + tree.slots[j].offset,
          tree.slots[j].length, PFS_INDEX_ALLOCATION, &record_context, &ref));
        size_t child = 0;
        while (child < plan->node_count && plan->nodes[child].block != ref.child.block) {
          child++;
        }
        TEST_ASSERT_LESS_THAN(plan->node_count, child);
        incoming[child]++;
        TEST_ASSERT_EQUAL_UINT(tree.level - 1, plan->nodes[child].level);
        uint8_t minimum[8];
        test_put_u64(minimum, plan->nodes[child].minimum);
        TEST_ASSERT_EQUAL_MEMORY(minimum, ref.minimum.bytes, 8);
      } else {
        TEST_ASSERT_LESS_THAN(plan->record_count, records);
        struct pfs_allocation_record record;
        TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_decode(bytes + tree.slots[j].offset,
          tree.slots[j].length, &record_context, &record));
        TEST_ASSERT_EQUAL_UINT64(plan->records[records].first, record.first);
        TEST_ASSERT_EQUAL_UINT64(plan->records[records].count, record.count);
        TEST_ASSERT_EQUAL_UINT(plan->records[records].state, record.state);
        TEST_ASSERT_EQUAL_UINT64(plan->records[records].birth, record.birth);
        TEST_ASSERT_EQUAL_UINT(plan->records[records].charge, record.charge);
        TEST_ASSERT_EQUAL_UINT64(plan->records[records].retirement, record.retirement);
        TEST_ASSERT_EQUAL_MEMORY(plan->records[records].owner.bytes, record.owner.bytes, PFS_ID_SIZE);
        records++;
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT(plan->record_count, records);
  for (size_t i = 0; i < plan->node_count; i++) {
    TEST_ASSERT_EQUAL_UINT(plan->nodes[i].block == plan->root.block ? 0 : 1, incoming[i]);
  }
}

static void
fragmented_map_accounts_for_itself(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 8192, 4096, 16, &limits));
  arena = (struct pfs_plan_arena){0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  struct pfs_allocation_record *base = arena.maps[0];
  static struct pfs_reusable_range ranges[2048];
  for (size_t i = 0; i < 4096; i++) {
    base[i] = (struct pfs_allocation_record){.first = (i / 2) * 4 + 1 + (i & 1), .count = (i & 1) ? 3 : 1};
    if ((i & 1) == 0) {
      base[i].state = PFS_ALLOCATION_VOLUME;
      base[i].owner.bytes[0] = 7;
      base[i].birth = 1;
    } else {
      ranges[i / 2] = (struct pfs_reusable_range){base[i].first + 1, 1};
    }
  }
  base[4095].count = PFS_POOL_BLOCKS_MIN - 1 - base[4095].first;
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 2,
    .referring_birth = 2, .pool = {{3}},
  };
  struct pfs_map_plan plan;
  size_t allocation_calls = fixture.allocation_calls;
  uint64_t reads = fixture.reads, writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, base, 4096,
    ranges, 2048, 8, &plan));
  TEST_ASSERT_EQUAL_UINT(110, plan.allocation_count);
  TEST_ASSERT_EQUAL_UINT(101, plan.node_count);
  TEST_ASSERT_EQUAL_UINT(4316, plan.record_count);
  check_map(&plan, &context, base, 4096);
  TEST_ASSERT_EQUAL_UINT64(allocation_calls, fixture.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  /* Insufficient caller-proven reusable capacity refuses the plan. */
  const struct pfs_reusable_range one = {2, 1};
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_map_build(&arena, &context, base, 4096, &one, 1, 8, &plan));
  TEST_ASSERT_NULL(plan.records);
  const struct pfs_reusable_range live = {1, 1};
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_plan_map_build(&arena, &context, base, 4096, &live, 1, 0, &plan));
  TEST_ASSERT_NULL(plan.records);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
}

static void
single_leaf_map_and_reserved_blocks(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 1, &limits));
  arena = (struct pfs_plan_arena){0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  struct pfs_allocation_record base = {.first = 1, .count = PFS_POOL_BLOCKS_MIN - 2};
  struct pfs_reusable_range range = {100, 100};
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 2, .pool = {{8}},
  };
  struct pfs_map_plan plan;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, &base, 1, &range, 1, 0, &plan));
  check_map(&plan, &context, &base, 1);
  TEST_ASSERT_NOT_EQUAL(plan.root.block, plan.pool_root_block);
  TEST_ASSERT_TRUE(selected(&plan, plan.pool_root_block));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, &base, 1, &range, 1, 8, &plan));
  TEST_ASSERT_EQUAL_UINT(10, plan.allocation_count);
  TEST_ASSERT_EQUAL_UINT(1, plan.node_count);
  check_map(&plan, &context, &base, 1);
  TEST_ASSERT_EQUAL_UINT64(1, base.first);
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, base.count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
}

void
run_plan_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(limits_and_arena);
  RUN_TEST(interval_changes_are_private_and_canonical);
  RUN_TEST(fragmented_map_accounts_for_itself);
  RUN_TEST(single_leaf_map_and_reserved_blocks);
}
