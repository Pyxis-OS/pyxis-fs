/* SPDX-License-Identifier: MPL-2.0 */
#include "admit_tests.h"
#include "admit.h"
#include "support.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <string.h>

static struct test_fixture fixture;
static struct pfs_allocation summaries;
static struct pfs_plan_arena arena;
static struct pfs_admit_state *states;
static const struct pfs_build_object image_objects[] = {
  {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
  {.id = {{4}}, .parent = 0, .name = {3, "one"}, .kind = PFS_OBJECT_FILE},
  {.id = {{5}}, .parent = 0, .name = {3, "two"}, .kind = PFS_OBJECT_FILE},
};

static void
build_image(uint64_t quota, uint64_t recovery)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  summaries = (struct pfs_allocation){0};
  arena = (struct pfs_plan_arena){0};
  struct pfs_build_volume volume = {
    .id = {{2}}, .root_object = {{3}}, .name = {4, "home"}, .owner = {{6}},
    .guarantee_set = true, .quota_set = true, .quota = quota,
    .object_count = sizeof(image_objects) / sizeof(*image_objects), .objects = image_objects,
  };
  struct pfs_build_spec spec = {
    .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}}, .volume_count = 1, .volumes = &volume,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = recovery,
  };
  struct pfs_build_plan plan = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&fixture.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &fixture.builder, NULL));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_allocate(&fixture.memory, 3 * sizeof(*states),
    _Alignof(struct pfs_admit_state), &summaries));
  states = summaries.data;
  memset(states, 0, summaries.size);
}

static void
release_opening(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_arena_destroy(&arena));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_free(&fixture.memory, &summaries));
  states = NULL;
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
}

static void
opening_copies_proof_and_funds_deletion(void)
{
  build_image(64, 1024);
  const uint64_t metadata_limit = 16;
  const size_t object_count = sizeof(image_objects) / sizeof(*image_objects);
  uint64_t directories = 0;
  for (size_t i = 0; i < object_count; ++i) {
    directories += image_objects[i].kind == PFS_OBJECT_DIRECTORY;
  }
  uint64_t records = object_count - 1;
  uint64_t trees = directories + 1;
  if (trees > records) {
    trees = records;
  }
  uint64_t deletion_blocks = (records + 4 * trees) / 5;
  struct pfs_check_result check;
  uint64_t writes = fixture.writes, flushes = fixture.flushes;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, metadata_limit, &arena, states, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  const struct pfs_admit_volume *volume = &states[0].volumes[0];
  TEST_ASSERT_EQUAL_UINT64(object_count, volume->record.object_count);
  TEST_ASSERT_EQUAL_UINT64(directories, volume->directories);
  TEST_ASSERT_GREATER_THAN(0, volume->namespace_nodes);
  TEST_ASSERT_LESS_OR_EQUAL(deletion_blocks, volume->namespace_nodes);
  /* Empty fixture files have no data blocks; live storage is all metadata. */
  TEST_ASSERT_EQUAL_UINT64(volume->record.live_blocks, volume->metadata_blocks);
  TEST_ASSERT_LESS_OR_EQUAL(volume->metadata_blocks, volume->namespace_nodes);
  TEST_ASSERT_LESS_OR_EQUAL(metadata_limit, volume->metadata_blocks);
  TEST_ASSERT_EQUAL_UINT64(deletion_blocks, volume->deletion_blocks);
  TEST_ASSERT_EQUAL_UINT64(volume->record.live_blocks - volume->namespace_nodes + deletion_blocks,
    volume->effective_blocks);
  TEST_ASSERT_EQUAL_UINT64(0, states[0].file_extents);
  TEST_ASSERT_EQUAL_PTR(arena.maps[0], states[0].maps);
  TEST_ASSERT_EQUAL_PTR(arena.maps[1], states[1].maps);
  TEST_ASSERT_EQUAL_PTR(arena.maps[2], states[2].maps);
  TEST_ASSERT_EQUAL_UINT(0, states[2].map_count);
  TEST_ASSERT_EQUAL_MEMORY(states[0].maps, states[1].maps,
    states[0].map_count * sizeof(*states[0].maps));
  TEST_ASSERT_EQUAL_UINT64(summaries.size + arena.allocation.size, fixture.memory.used);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(flushes, fixture.flushes);
  struct pfs_reusable_range ranges[32];
  size_t count = 99;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_reusable(&states[0], &states[1], ranges, 32, &count));
  uint64_t blocks = 0;
  for (size_t i = 0; i < count; i++) {
    blocks += ranges[i].count;
  }
  TEST_ASSERT_EQUAL_UINT64(states[0].reusable_blocks, blocks);
  TEST_ASSERT_EQUAL_UINT64(states[0].candidate.root.free, blocks);
  release_opening();
}

static void
opening_refuses_unfunded_promises_without_writes(void)
{
  build_image(3, 1024);
  struct pfs_check_result check;
  uint64_t writes = fixture.writes, flushes = fixture.flushes;
  TEST_ASSERT_EQUAL(PFS_QUOTA, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_NULL(states[0].maps);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(flushes, fixture.flushes);
  release_opening();
}

static void
opening_profile_includes_namespace_headroom(void)
{
  build_image(64, 1024);
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 3, &arena, states, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  release_opening();
}

static void
opening_requires_computed_recovery_reserve(void)
{
  build_image(64, 256);
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_NO_SPACE, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  release_opening();
}

static void
opening_refuses_noncanonical_retained_bytes(void)
{
  build_image(64, 1024);
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (size_t i = 0; i < 2; i++) {
    uint64_t slot = i ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, slot, 1, bytes, sizeof(bytes)));
    /* Equal-generation slots must preserve identical reserved bytes. */
    bytes[132] = 1;
    test_checksum(bytes, sizeof(bytes), 20);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, slot, 1, bytes, sizeof(bytes)));
  }
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &check));
  uint64_t writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  release_opening();
}

static void
opening_memory_refusal_releases_temporary_proof(void)
{
  build_image(64, 1024);
  struct pfs_check_result check;
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 0, 16, 1, &limits));
  fixture.memory.limit = summaries.size + limits.arena_bytes - 1;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_NULL(states[0].maps);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  fixture.memory.limit = PFS_MEMORY_DEFAULT;
  fixture.fail_after = fixture.allocation_calls;
  TEST_ASSERT_EQUAL(PFS_NO_MEMORY, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  TEST_ASSERT_NULL(arena.allocation.data);
  TEST_ASSERT_EQUAL_UINT64(summaries.size, fixture.memory.used);
  fixture.fail_after = SIZE_MAX;
  release_opening();
}

static void
copy_candidate(void)
{
  states[2] = states[0];
  states[2].maps = arena.maps[2];
  states[2].claims = (struct check_claim *)arena.claims[2];
  uint64_t old_root = states[0].candidate.root.header.block;
  const struct pfs_allocation_record *free_record = NULL;
  for (size_t i = 0; i < states[0].map_count; i++) {
    if (states[0].maps[i].state == PFS_ALLOCATION_FREE) {
      free_record = &states[0].maps[i];
    }
  }
  TEST_ASSERT_NOT_NULL(free_record);
  uint64_t new_root = free_record->first;
  struct pfs_map_change changes[] = {
    {.before = {.first = old_root, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 1},
     .after = {.first = old_root, .count = 1, .state = PFS_ALLOCATION_RETIRED,
       .charge = PFS_CHARGE_RECOVERY, .birth = 1, .retirement = 2}},
    {.before = {.first = new_root, .count = 1},
     .after = {.first = new_root, .count = 1, .state = PFS_ALLOCATION_POOL, .birth = 2}},
  };
  struct pfs_record_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 2, .containing_birth = 2,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_apply(&context, states[0].maps, states[0].map_count,
    changes, 2, states[2].maps, arena.limits.records, &states[2].map_count));
  size_t next = 0;
  for (size_t i = 0; i < states[0].claim_count; i++) {
    if (states[0].claims[i].first != old_root) {
      states[2].claims[next++] = states[0].claims[i];
    }
  }
  states[2].claims[next++] = (struct check_claim){
    .first = new_root, .count = 1, .birth = 2, .type = PFS_BLOCK_POOL,
  };
  states[2].claim_count = next;
  states[2].candidate.superblock.header.birth = 2;
  states[2].candidate.superblock.root.block = new_root;
  states[2].candidate.superblock.root.birth = 2;
  states[2].candidate.root.header.block = new_root;
  states[2].candidate.root.header.birth = 2;
  states[2].candidate.root.retired++;
  states[2].candidate.root.free--;
  states[2].candidate.root.recovery.occupied++;
}

static void
candidate_generation(uint64_t generation)
{
  states[0].candidate.superblock.header.birth = generation - 1;
  states[2].candidate.superblock.header.birth = generation;
  states[2].candidate.superblock.root.birth = generation;
  states[2].candidate.root.header.birth = generation;
  for (size_t i = 0; i < states[2].map_count; i++) {
    if (states[2].maps[i].state == PFS_ALLOCATION_RETIRED) {
      states[2].maps[i].retirement = generation;
    }
    if (states[2].maps[i].first == states[2].candidate.root.header.block) {
      states[2].maps[i].birth = generation;
    }
  }
  for (size_t i = 0; i < states[2].claim_count; i++) {
    if (states[2].claims[i].type == PFS_BLOCK_POOL) {
      states[2].claims[i].birth = generation;
    }
  }
}

static void
candidate_admission_is_private_and_reserves_exact_generations(void)
{
  build_image(64, 1024);
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  copy_candidate();
  uint64_t reads = fixture.reads, writes = fixture.writes, flushes = fixture.flushes;
  size_t allocations = fixture.allocation_calls;
  fixture.fail_after = allocations;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_candidate(&arena, &states[2], &states[0], true));
  TEST_ASSERT_EQUAL_MEMORY(states[0].maps, states[1].maps,
    states[0].map_count * sizeof(*states[0].maps));
  TEST_ASSERT_EQUAL_MEMORY(states[0].claims, states[1].claims,
    states[0].claim_count * sizeof(*states[0].claims));
  candidate_generation(UINT64_MAX - 2);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_candidate(&arena, &states[2], &states[0], true));
  candidate_generation(UINT64_MAX - 1);
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_admit_candidate(&arena, &states[2], &states[0], true));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_candidate(&arena, &states[2], &states[0], false));
  states[2].candidate.superblock.header.birth--;
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_admit_candidate(&arena, &states[2], &states[0], false));
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(flushes, fixture.flushes);
  TEST_ASSERT_EQUAL_UINT64(allocations, fixture.allocation_calls);
  fixture.fail_after = SIZE_MAX;
  release_opening();
}

static void
candidate_rejects_unproven_claims_and_live_workspace_charges(void)
{
  build_image(64, 1024);
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    0, 16, &arena, states, &check));
  copy_candidate();
  uint64_t reads = fixture.reads, writes = fixture.writes;
  size_t allocations = fixture.allocation_calls;
  states[2].claims[0].birth++;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_admit_candidate(&arena, &states[2], &states[0], false));
  copy_candidate();
  for (size_t i = 0; i < states[2].map_count; i++) {
    if (states[2].maps[i].state == PFS_ALLOCATION_VOLUME) {
      states[2].maps[i].charge = PFS_CHARGE_ORDINARY;
      states[2].candidate.root.cow.occupied += states[2].maps[i].count;
    }
  }
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_admit_candidate(&arena, &states[2], &states[0], false));
  copy_candidate();
  states[2].volumes[0].record.guarantee++;
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_admit_candidate(&arena, &states[2], &states[0], false));
  TEST_ASSERT_EQUAL_MEMORY(states[0].maps, states[1].maps,
    states[0].map_count * sizeof(*states[0].maps));
  TEST_ASSERT_EQUAL_MEMORY(states[0].claims, states[1].claims,
    states[0].claim_count * sizeof(*states[0].claims));
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(allocations, fixture.allocation_calls);
  release_opening();
}

static void
historical_retirement_does_not_protect_reusable_storage(void)
{
  build_image(64, 1024);
  struct pfs_allocation_record older_map[] = {
    {.first = 1, .count = 10},
    {.first = 11, .count = 2, .state = PFS_ALLOCATION_RETIRED,
     .charge = PFS_CHARGE_RECOVERY, .birth = 1, .retirement = 2},
    {.first = 13, .count = PFS_POOL_BLOCKS_MIN - 14},
  };
  struct pfs_allocation_record selected_map = {.first = 1, .count = PFS_POOL_BLOCKS_MIN - 2};
  states[0].candidate.superblock.header.birth = 2;
  states[0].maps = older_map;
  states[0].map_count = 3;
  states[1].candidate.superblock.header.birth = 3;
  states[1].maps = &selected_map;
  states[1].map_count = 1;
  struct pfs_reusable_range ranges[4];
  size_t count = 99;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_reusable(&states[0], &states[1], ranges, 4, &count));
  uint64_t total = 0;
  bool historical_available = false;
  for (size_t i = 0; i < count; i++) {
    total += ranges[i].count;
    historical_available |= ranges[i].first <= 11 &&
      ranges[i].first + ranges[i].count >= 13;
  }
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 2, total);
  TEST_ASSERT_TRUE(historical_available);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_reusable(&states[1], &states[0], ranges, 4, &count));
  struct check_claim live = {.first = 11, .count = 2, .birth = 1,
    .volume = {{2}}, .object = {{4}}};
  states[0].claims = &live;
  states[0].claim_count = 1;
  count = 99;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_admit_reusable(&states[0], &states[1], ranges, 4, &count));
  TEST_ASSERT_EQUAL_UINT(99, count);
  states[0].claims = NULL;
  states[0].claim_count = 0;

  /* An older live allocation protects the range even without a collected claim. */
  older_map[1].state = PFS_ALLOCATION_POOL;
  older_map[1].charge = PFS_CHARGE_PERMANENT;
  older_map[1].retirement = 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_reusable(&states[1], &states[0], ranges, 4, &count));
  total = 0;
  for (size_t i = 0; i < count; i++) {
    total += ranges[i].count;
    TEST_ASSERT_TRUE(ranges[i].first + ranges[i].count <= 11 || ranges[i].first >= 13);
  }
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 4, total);

  /* A selected retirement remains unavailable until its free change is durable. */
  older_map[1].state = PFS_ALLOCATION_RETIRED;
  older_map[1].charge = PFS_CHARGE_RECOVERY;
  older_map[1].retirement = 3;
  states[0].maps = &selected_map;
  states[0].map_count = 1;
  states[1].maps = older_map;
  states[1].map_count = 3;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_reusable(&states[0], &states[1], ranges, 4, &count));
  total = 0;
  for (size_t i = 0; i < count; i++) {
    total += ranges[i].count;
    TEST_ASSERT_TRUE(ranges[i].first + ranges[i].count <= 11 || ranges[i].first >= 13);
  }
  TEST_ASSERT_EQUAL_UINT64(PFS_POOL_BLOCKS_MIN - 4, total);
  release_opening();
}

static void
projected_growth_refuses_quota_before_media_accounting(void)
{
  build_image(4, 1024);
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_admit_open(&fixture.builder.reader, &fixture.memory,
    1, 16, &arena, states, &check));
  copy_candidate();
  uint64_t block = states[2].candidate.root.header.block + 1;
  struct pfs_map_change add = {
    .before = {.first = block, .count = 1},
    .after = {.first = block, .count = 1, .state = PFS_ALLOCATION_VOLUME,
      .owner = {{2}}, .birth = 2},
  };
  struct pfs_record_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 2, .containing_birth = 2,
  };
  struct pfs_allocation_record projected[32];
  size_t count = 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_map_apply(&context, states[2].maps, states[2].map_count,
    &add, 1, projected, 32, &count));
  memcpy(states[2].maps, projected, count * sizeof(*projected));
  states[2].map_count = count;
  states[2].claims[states[2].claim_count++] = (struct check_claim){
    .first = block, .count = 1, .birth = 2, .volume = {{2}}, .object = {{4}},
  };
  states[2].file_extents++;
  states[2].volumes[0].file_extents++;
  states[2].volumes[0].record.live_blocks++;
  states[2].candidate.root.live_volume++;
  states[2].candidate.root.free--;
  uint64_t reads = fixture.reads, writes = fixture.writes;
  size_t allocations = fixture.allocation_calls;
  TEST_ASSERT_EQUAL(PFS_QUOTA, pfs_admit_candidate(&arena, &states[2], &states[0], true));
  TEST_ASSERT_EQUAL_MEMORY(states[0].maps, states[1].maps,
    states[0].map_count * sizeof(*states[0].maps));
  TEST_ASSERT_EQUAL_MEMORY(states[0].claims, states[1].claims,
    states[0].claim_count * sizeof(*states[0].claims));
  states[2].claims[states[2].claim_count - 1].birth = 1;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_admit_candidate(&arena, &states[2], &states[0], true));
  TEST_ASSERT_EQUAL_UINT64(reads, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(allocations, fixture.allocation_calls);
  release_opening();
}

void
run_admit_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(opening_copies_proof_and_funds_deletion);
  RUN_TEST(opening_refuses_unfunded_promises_without_writes);
  RUN_TEST(opening_profile_includes_namespace_headroom);
  RUN_TEST(opening_requires_computed_recovery_reserve);
  RUN_TEST(opening_refuses_noncanonical_retained_bytes);
  RUN_TEST(opening_memory_refusal_releases_temporary_proof);
  RUN_TEST(candidate_admission_is_private_and_reserves_exact_generations);
  RUN_TEST(candidate_rejects_unproven_claims_and_live_workspace_charges);
  RUN_TEST(historical_retirement_does_not_protect_reusable_storage);
  RUN_TEST(projected_growth_refuses_quota_before_media_accounting);
}
