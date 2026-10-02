/* SPDX-License-Identifier: MPL-2.0 */
#include "plan_tests.h"
#include "plan.h"
#include "support.h"
#include "unity.h"
#include <string.h>

static struct test_fixture fixture;
static struct pfs_plan_arena arena;

static void
check_sorted_changes(const uint64_t *keys, size_t count)
{
  struct pfs_map_change changes[32], original[32];
  bool seen[32] = {0};
  TEST_ASSERT_LESS_OR_EQUAL_UINT(32, count);
  memset(changes, 0, sizeof(changes));
  for (size_t i = 0; i < count; i++) {
    changes[i].before.first = keys[i];
    changes[i].before.count = i + 1;
    changes[i].before.state = i % 4;
    changes[i].before.charge = i % 3;
    changes[i].before.birth = 100 + i;
    changes[i].before.retirement = 200 + i;
    memset(changes[i].before.owner.bytes, 1 + i, PFS_ID_SIZE);
    changes[i].after.first = UINT64_MAX - i;
    changes[i].after.count = 300 + i;
    changes[i].after.state = (i + 1) % 4;
    changes[i].after.charge = (i + 1) % 3;
    changes[i].after.birth = 400 + i;
    changes[i].after.retirement = 500 + i;
    memset(changes[i].after.owner.bytes, 33 + i, PFS_ID_SIZE);
  }
  memcpy(original, changes, sizeof(changes));
  pfs_plan_sort_changes(changes, count);
  for (size_t i = 0; i < count; i++) {
    if (i) {
      TEST_ASSERT_TRUE(changes[i - 1].before.first <= changes[i].before.first);
    }
    size_t source = 0;
    while (source < count && memcmp(&changes[i], &original[source], sizeof(changes[i]))) {
      source++;
    }
    TEST_ASSERT_LESS_THAN_UINT(count, source);
    TEST_ASSERT_FALSE(seen[source]);
    seen[source] = true;
  }
  /* Sorting only the requested entries preserves the unused tail as well. */
  TEST_ASSERT_EQUAL_MEMORY(original + count, changes + count,
    (32 - count) * sizeof(*changes));
}

static void
allocation_deltas_sort_without_changing_contents(void)
{
  pfs_plan_sort_changes(NULL, 0);
  check_sorted_changes(NULL, 0);
  const uint64_t single[] = {UINT64_MAX};
  check_sorted_changes(single, 1);
  const uint64_t sorted[] = {0, 1, 2, 3, 4, 5, 6, 7, UINT64_MAX};
  check_sorted_changes(sorted, 9);
  const uint64_t reverse[] = {UINT64_MAX, 7, 6, 5, 4, 3, 2, 1, 0};
  check_sorted_changes(reverse, 9);
  const uint64_t mixed[] = {4, UINT64_MAX, 0, 4, 1, 7, 1, 0, 3, UINT64_MAX, 4};
  check_sorted_changes(mixed, 11);
  const uint64_t equal[] = {3, 3, 3, 3, 3, 3};
  check_sorted_changes(equal, 6);
  /* Include even-length heaps and short tails without assuming stable ties. */
  for (size_t count = 2; count <= 8; count++) {
    check_sorted_changes(reverse, count);
    check_sorted_changes(mixed, count);
  }
}

static void
check_limit_closure(uint64_t extents, uint64_t metadata, uint16_t volumes,
                    const struct pfs_plan_limits *limits)
{
  uint64_t catalog = 4u * volumes - 2;
  uint64_t catalog_union = catalog < 16 ? catalog : 16;
  TEST_ASSERT_EQUAL_UINT64(catalog, limits->catalog_blocks);
  TEST_ASSERT_EQUAL_UINT64(catalog_union, limits->catalog_union);
  TEST_ASSERT_EQUAL_UINT64(catalog + limits->pool_blocks, limits->permanent_pool);
  TEST_ASSERT_EQUAL_UINT64(3 * limits->pool_blocks + 2 * 256 + 128, limits->recovery_blocks);
  uint64_t base = 1 + 2 * (extents + metadata + 2 * 256) + 2 * catalog;
  uint64_t numerator = 23 * (base + 6 * limits->pool_blocks + 2 * catalog_union + 4);
  uint64_t records = numerator / 21 + (numerator % 21 != 0);
  TEST_ASSERT_EQUAL_UINT64(records, limits->records);
  uint64_t nodes = 0;
  uint64_t level = records / 46 + (records % 46 != 0);
  for (;;) {
    nodes += level;
    if (level == 1) {
      break;
    }
    level = level / 65 + (level % 65 != 0);
  }
  TEST_ASSERT_TRUE(nodes + catalog_union + 1 <= limits->pool_blocks);
  /* The accepted closure selects the first sufficient H. Its predecessor must
   * fail, rather than freezing the incidental output for a profile example. */
  numerator = 23 * (base + 6 * (limits->pool_blocks - 1) + 2 * catalog_union + 4);
  records = numerator / 21 + (numerator % 21 != 0);
  nodes = 0;
  level = records / 46 + (records % 46 != 0);
  for (;;) {
    nodes += level;
    if (level == 1) {
      break;
    }
    level = level / 65 + (level % 65 != 0);
  }
  TEST_ASSERT_TRUE(nodes + catalog_union + 1 > limits->pool_blocks - 1);
}

static void
limits_and_arena(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(UINT64_C(4) * 1024 * 1024 / 4,
    8192, 4096, 16, &limits));
  check_limit_closure(8192, 4096, 16, &limits);
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
  check_limit_closure(262144, 65536, 16, &limits);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(UINT64_C(256) * 1024 * 1024 / 4,
    524288, 131072, 16, &limits));
  check_limit_closure(524288, 131072, 16, &limits);
  struct pfs_plan_limits saved = limits;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_limits(PFS_POOL_BLOCKS_MAX,
    PFS_RECORD_COUNT_MAX, PFS_POOL_BLOCKS_MAX - 2, 16, &limits));
  TEST_ASSERT_EQUAL_MEMORY(&saved, &limits, sizeof(limits));
  for (uint16_t volumes = 1; volumes <= 5; volumes++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, volumes, &limits));
    check_limit_closure(0, 16, volumes, &limits);
  }
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
  size_t counted = 99;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_count(&context, base, 2, changes, 2, 6, &counted));
  TEST_ASSERT_EQUAL_UINT(5, counted);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_plan_map_count(&context, base, 2, changes, 2, 2, &counted));
  TEST_ASSERT_EQUAL_UINT(5, counted);

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
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_count(&context, base, 2, &free_change, 1, 1, &counted));
  TEST_ASSERT_EQUAL_UINT(1, counted);

  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, output[0].count);
  TEST_ASSERT_EQUAL_UINT(PFS_ALLOCATION_FREE, output[0].state);
}

static void
interval_failures_distinguish_source_and_delta(void)
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
  struct pfs_map_change free_change = {
    .before = base[0], .after = {.first = 1, .count = 10},
  };
  struct pfs_allocation_record output[6];
  size_t count = 99;
  free_change.before.owner.bytes[0] = 8;
  struct pfs_map_change saved_change = free_change;
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 6, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL_MEMORY(&saved_change, &free_change, sizeof(free_change));

  /* A matching prefix does not authorize the rest of a range across states. */
  free_change.before = base[0];
  free_change.before.count++;
  free_change.after.count++;
  saved_change = free_change;
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 6, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL_MEMORY(&saved_change, &free_change, sizeof(free_change));

  /* Incomplete coverage remains malformed source data, even with bad deltas. */
  base[1].count--;
  memcpy(original, base, sizeof(base));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 6, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL_MEMORY(&saved_change, &free_change, sizeof(free_change));

  base[1].count++;
  base[0].owner = (struct pfs_volume_id){0};
  memcpy(original, base, sizeof(base));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_plan_map_apply(&context, base, 2, &free_change, 1, output, 6, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  TEST_ASSERT_EQUAL_MEMORY(original, base, sizeof(base));
  TEST_ASSERT_EQUAL_MEMORY(&saved_change, &free_change, sizeof(free_change));
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
check_exact_map(const struct pfs_map_plan *plan,
                const struct pfs_allocation_record *expected, size_t count)
{
  TEST_ASSERT_EQUAL_UINT(count, plan->record_count);
  for (size_t i = 0; i < count; i++) {
    const struct pfs_allocation_record *record = &plan->records[i];
    TEST_ASSERT_EQUAL_UINT64(expected[i].first, record->first);
    TEST_ASSERT_EQUAL_UINT64(expected[i].count, record->count);
    TEST_ASSERT_EQUAL_UINT(expected[i].state, record->state);
    TEST_ASSERT_EQUAL_UINT(expected[i].charge, record->charge);
    TEST_ASSERT_EQUAL_UINT64(expected[i].birth, record->birth);
    TEST_ASSERT_EQUAL_UINT64(expected[i].retirement, record->retirement);
    TEST_ASSERT_EQUAL_MEMORY(expected[i].owner.bytes, record->owner.bytes, PFS_ID_SIZE);
  }
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
  TEST_ASSERT_EQUAL_UINT(plan.node_count + 8 + 1, plan.allocation_count);
  TEST_ASSERT_TRUE(plan.allocation_count <= limits.pool_blocks);
  TEST_ASSERT_TRUE(plan.record_count <= limits.records);
  check_map(&plan, &context, base, 4096);
  TEST_ASSERT_EQUAL_UINT64(allocation_calls, fixture.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  /* Preserve the separate-input result, then repeat the same multilevel plan
   * with the candidate vector holding the original source intervals. */
  size_t record_count = plan.record_count;
  memcpy(arena.maps[1], plan.records, record_count * sizeof(*plan.records));
  memcpy(arena.maps[2], base, 4096 * sizeof(*base));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, arena.maps[2], 4096,
    ranges, 2048, 8, &plan));
  TEST_ASSERT_EQUAL_PTR(arena.maps[2], plan.records);
  TEST_ASSERT_EQUAL_UINT(plan.node_count + 8 + 1, plan.allocation_count);
  check_exact_map(&plan, arena.maps[1], record_count);
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
in_place_map_claim_boundaries(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 1, &limits));
  arena = (struct pfs_plan_arena){0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 4, .pool = {{8}},
  };
  /* Two reservations consume a free source exactly, at either edge, or inside
   * it. The following live source must survive every backward expansion. */
  const uint64_t lengths[] = {2, 5, 5, 5};
  const uint64_t starts[] = {1, 1, 4, 2};
  for (size_t i = 0; i < 4; i++) {
    struct pfs_allocation_record base[] = {
      {.first = 1, .count = lengths[i]},
      {.first = lengths[i] + 1, .count = PFS_POOL_BLOCKS_MIN - 2 - lengths[i],
       .state = PFS_ALLOCATION_VOLUME, .owner = {{9}}, .birth = 1},
    };
    struct pfs_allocation_record expected[4];
    size_t count = 0;
    if (starts[i] > 1) {
      expected[count++] = (struct pfs_allocation_record){.first = 1, .count = starts[i] - 1};
    }
    expected[count++] = (struct pfs_allocation_record){.first = starts[i], .count = 2,
      .state = PFS_ALLOCATION_POOL, .birth = 4};
    if (starts[i] + 2 < lengths[i] + 1) {
      expected[count++] = (struct pfs_allocation_record){.first = starts[i] + 2,
        .count = lengths[i] + 1 - starts[i] - 2};
    }
    expected[count++] = base[1];
    memcpy(arena.maps[2], base, sizeof(base));
    struct pfs_reusable_range range = {starts[i], 2};
    struct pfs_map_plan plan;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, arena.maps[2], 2,
      &range, 1, 0, &plan));
    TEST_ASSERT_EQUAL_PTR(arena.maps[2], plan.records);
    TEST_ASSERT_EQUAL_UINT(2, plan.allocation_count);
    TEST_ASSERT_EQUAL_UINT(1, plan.node_count);
    check_exact_map(&plan, expected, count);
    check_map(&plan, &context, base, 2);
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
}

static void
in_place_map_preserves_fragmented_intervals(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_plan_limits limits;
  /* Three volumes permit eight nodes in a two-path catalog union. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 3, &limits));
  arena = (struct pfs_plan_arena){0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  const struct pfs_allocation_record base[] = {
    {.first = 1, .count = 8},
    {.first = 9, .count = 3, .state = PFS_ALLOCATION_VOLUME, .owner = {{9}}, .birth = 1},
    {.first = 12, .count = 12},
    {.first = 24, .count = 4, .state = PFS_ALLOCATION_POOL, .birth = 1},
    {.first = 28, .count = 3, .state = PFS_ALLOCATION_RETIRED,
     .charge = PFS_CHARGE_RECOVERY, .owner = {{9}}, .birth = 1, .retirement = 3},
    {.first = 31, .count = 10},
    {.first = 41, .count = PFS_POOL_BLOCKS_MIN - 42,
     .state = PFS_ALLOCATION_VOLUME, .owner = {{8}}, .birth = 2},
  };
  const struct pfs_reusable_range ranges[] = {
    {1, 1}, {2, 1}, {4, 1}, {7, 2}, {12, 1}, {17, 1}, {23, 1}, {31, 1}, {40, 1},
  };
  const struct pfs_allocation_record expected[] = {
    {.first = 1, .count = 2, .state = PFS_ALLOCATION_POOL, .birth = 4},
    {.first = 3, .count = 1},
    {.first = 4, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    {.first = 5, .count = 2},
    {.first = 7, .count = 2, .state = PFS_ALLOCATION_POOL, .birth = 4},
    base[1],
    {.first = 12, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    {.first = 13, .count = 4},
    {.first = 17, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    {.first = 18, .count = 5},
    {.first = 23, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    base[3], base[4],
    {.first = 31, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    {.first = 32, .count = 8},
    {.first = 40, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 4},
    base[6],
  };
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 4, .pool = {{8}},
  };
  memcpy(arena.maps[2], base, sizeof(base));
  struct pfs_map_plan plan;
  size_t allocation_calls = fixture.allocation_calls;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, arena.maps[2], 7,
    ranges, 9, 8, &plan));
  TEST_ASSERT_EQUAL_PTR(arena.maps[2], plan.records);
  TEST_ASSERT_EQUAL_UINT(10, plan.allocation_count);
  TEST_ASSERT_EQUAL_UINT(1, plan.node_count);
  check_exact_map(&plan, expected, 17);
  check_map(&plan, &context, base, 7);
  TEST_ASSERT_EQUAL_UINT64(allocation_calls, fixture.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(0, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(0, fixture.writes);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
}

static void
single_leaf_map_and_reserved_blocks(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 5, &limits));
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
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_build(&arena, &context, &base, 1, &range, 1, 16, &plan));
  TEST_ASSERT_EQUAL_UINT(16, plan.catalog_count);
  TEST_ASSERT_EQUAL_UINT(18, plan.allocation_count);
  TEST_ASSERT_EQUAL_UINT(1, plan.node_count);
  check_map(&plan, &context, &base, 1);
  for (size_t i = 0; i < plan.catalog_count; i++) {
    TEST_ASSERT_TRUE(selected(&plan, plan.catalog_blocks[i]));
    TEST_ASSERT_NOT_EQUAL(plan.pool_root_block, plan.catalog_blocks[i]);
    TEST_ASSERT_NOT_EQUAL(plan.root.block, plan.catalog_blocks[i]);
    for (size_t j = 0; j < i; j++) {
      TEST_ASSERT_NOT_EQUAL(plan.catalog_blocks[j], plan.catalog_blocks[i]);
    }
  }
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_plan_map_build(&arena, &context, &base, 1,
    &range, 1, 17, &plan));
  TEST_ASSERT_NULL(plan.records);
  TEST_ASSERT_EQUAL_UINT64(1, base.first);
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, base.count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
  /* Physical capacity for more IDs cannot override the computed catalog bound. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_create(&fixture.memory, &limits, &arena));
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_plan_map_build(&arena, &context, &base, 1,
    &range, 1, 3, &plan));
  TEST_ASSERT_NULL(plan.records);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
}

void
run_plan_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(allocation_deltas_sort_without_changing_contents);
  RUN_TEST(limits_and_arena);
  RUN_TEST(interval_changes_are_private_and_canonical);
  RUN_TEST(interval_failures_distinguish_source_and_delta);
  RUN_TEST(fragmented_map_accounts_for_itself);
  RUN_TEST(single_leaf_map_and_reserved_blocks);
  RUN_TEST(in_place_map_claim_boundaries);
  RUN_TEST(in_place_map_preserves_fragmented_intervals);
}
