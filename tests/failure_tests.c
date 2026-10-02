/* SPDX-License-Identifier: MPL-2.0 */
#include "failure_tests.h"
#include "failure.h"
#include "unity.h"

#include <pyxis_fs/block.h>
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/pool.h>

#include <string.h>

static struct test_failure adapter;
static struct test_failure snapshot;
static struct test_fixture seed;
static uint8_t old_bytes[3 * PFS_BLOCK_SIZE];
static uint8_t new_bytes[3 * PFS_BLOCK_SIZE];
static uint8_t observed[3 * PFS_BLOCK_SIZE];

static void
open_adapter(size_t capacity)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&adapter, PFS_POOL_BLOCKS_MIN,
                                             capacity, 0, NULL));
  memset(old_bytes, 0x80, sizeof(old_bytes));
  for (size_t i = 0; i < 3; ++i) {
    memset(new_bytes + i * PFS_BLOCK_SIZE, (int)(0x11 * (i + 1)), PFS_BLOCK_SIZE);
  }
  adapter.stable_writes = true;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 3,
                                          old_bytes, sizeof(old_bytes)));
  adapter.stable_writes = false;
  TEST_ASSERT_EQUAL_size_t(0, adapter.record_count);
}

static void
close_adapter(void)
{
  TEST_ASSERT_FALSE(adapter.infrastructure_failure);
  TEST_ASSERT_EQUAL_UINT64(0, adapter.memory->used);
  TEST_ASSERT_TRUE(test_failure_close(&adapter));
}

static void
fail_next(enum test_failure_event_kind kind, enum test_failure_mode mode,
          size_t prefix, uint64_t block)
{
  struct test_failure_fault fault = {
    .enabled = true,
    .kind = kind,
    .ordinal = adapter.ordinals[kind] + 1,
    .mode = mode,
    .prefix = prefix,
    .block = block,
  };
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&adapter, &fault));
}

static void
pending_cache_and_durable_are_separate(void)
{
  open_adapter(8);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 3,
                                          new_bytes, sizeof(new_bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 3,
                                         observed, sizeof(observed)));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, sizeof(observed));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, sizeof(observed));
  TEST_ASSERT_EQUAL_size_t(3, adapter.pending_count);
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &adapter, 0));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&snapshot.builder.reader, 100, 3,
                                         observed, sizeof(observed)));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, sizeof(observed));
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL_size_t(0, adapter.record_count);
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, sizeof(observed));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&adapter));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 3,
                                         observed, sizeof(observed)));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, sizeof(observed));
  close_adapter();
}

static void
replacement_failures_keep_declared_prefix(void)
{
  const enum test_failure_mode modes[] = {
    TEST_FAILURE_BEFORE, TEST_FAILURE_PENDING_PREFIX, TEST_FAILURE_DURABLE_PREFIX,
  };
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    open_adapter(8);
    fail_next(TEST_FAILURE_WRITE, modes[i], 1, 0);
    TEST_ASSERT_EQUAL(PFS_IO, pfs_block_write(&adapter.builder, 100, 3,
                                            new_bytes, sizeof(new_bytes)));
    TEST_ASSERT_TRUE(adapter.triggered);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 3,
                                           observed, sizeof(observed)));
    TEST_ASSERT_EQUAL_MEMORY(modes[i] == TEST_FAILURE_BEFORE ? old_bytes : new_bytes,
                             observed, PFS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(old_bytes + PFS_BLOCK_SIZE, observed + PFS_BLOCK_SIZE,
                             2 * PFS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
    TEST_ASSERT_EQUAL_MEMORY(modes[i] == TEST_FAILURE_DURABLE_PREFIX ? new_bytes : old_bytes,
                             observed, PFS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(old_bytes + PFS_BLOCK_SIZE, observed + PFS_BLOCK_SIZE,
                             2 * PFS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&adapter));
    TEST_ASSERT_EQUAL_size_t(0, adapter.record_count);
    close_adapter();
  }
  /* Failed requests may leave a complete pending write. Backend flush is an
   * adapter control here, not an attempted retry through a stopped core. */
  open_adapter(8);
  fail_next(TEST_FAILURE_WRITE, TEST_FAILURE_PENDING_PREFIX, 3, 0);
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_write(&adapter.builder, 100, 3,
                                          new_bytes, sizeof(new_bytes)));
  TEST_ASSERT_EQUAL_size_t(3, adapter.pending_count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, sizeof(observed));
  close_adapter();
}

static void
flush_failures_promote_only_selected_blocks(void)
{
  const size_t prefixes[] = {0, 1, SIZE_MAX};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    open_adapter(8);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 3,
                                            new_bytes, sizeof(new_bytes)));
    fail_next(TEST_FAILURE_FLUSH, TEST_FAILURE_FLUSH_PREFIX, prefixes[i], 0);
    TEST_ASSERT_EQUAL(PFS_IO, pfs_block_flush(&adapter.builder));
    TEST_ASSERT_EQUAL_size_t(i == 0 ? 3 : i == 1 ? 2 : 0, adapter.pending_count);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
    size_t promoted = prefixes[i] == SIZE_MAX ? 3 : prefixes[i];
    if (promoted) {
      TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, promoted * PFS_BLOCK_SIZE);
    }
    if (promoted < 3) {
      TEST_ASSERT_EQUAL_MEMORY(old_bytes + promoted * PFS_BLOCK_SIZE,
        observed + promoted * PFS_BLOCK_SIZE, (3 - promoted) * PFS_BLOCK_SIZE);
    }
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&adapter));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 3,
                                           new_bytes, sizeof(new_bytes)));
    TEST_ASSERT_EQUAL_MEMORY(observed, new_bytes, sizeof(observed));
    close_adapter();
  }
}

static void
make_slot(uint8_t bytes[PFS_BLOCK_SIZE], uint64_t block, uint64_t generation)
{
  struct pfs_superblock value = {
    .header = {.type = PFS_BLOCK_SUPER, .version = PFS_FORMAT_VERSION,
      .used = 192, .pool = {{9}}, .block = block, .birth = generation},
    .block_count = PFS_POOL_BLOCKS_MIN,
    .root = {generation + 1, generation, PFS_BLOCK_POOL, PFS_FORMAT_VERSION},
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(bytes, PFS_BLOCK_SIZE, &value));
}

static void
slot_failures_and_actual_byte_tears(void)
{
  const uint64_t slots[] = {0, PFS_POOL_BLOCKS_MIN - 1};
  const size_t tears[] = {32, 64};
  for (size_t slot = 0; slot < 2; ++slot) {
    for (size_t tear = 0; tear < 2; ++tear) {
      open_adapter(8);
      make_slot(old_bytes, slots[slot], 1);
      make_slot(new_bytes, slots[slot], 2);
      adapter.stable_writes = true;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, slots[slot], 1,
                                               old_bytes, PFS_BLOCK_SIZE));
      adapter.stable_writes = false;
      fail_next(TEST_FAILURE_WRITE, TEST_FAILURE_TORN_SLOT, tears[tear], 0);
      TEST_ASSERT_EQUAL(PFS_IO, pfs_block_write(&adapter.builder, slots[slot], 1,
                                               new_bytes, PFS_BLOCK_SIZE));
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, slots[slot], 1, observed));
      TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, tears[tear]);
      TEST_ASSERT_EQUAL_MEMORY(old_bytes + tears[tear], observed + tears[tear],
                               PFS_BLOCK_SIZE - tears[tear]);
      TEST_ASSERT_NOT_EQUAL(0, memcmp(old_bytes, observed, PFS_BLOCK_SIZE));
      TEST_ASSERT_NOT_EQUAL(0, memcmp(new_bytes, observed, PFS_BLOCK_SIZE));
      struct pfs_superblock decoded;
      TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_superblock_decode(observed, PFS_BLOCK_SIZE,
        PFS_POOL_BLOCKS_MIN, slots[slot], &decoded));
      close_adapter();
    }
    const enum test_failure_mode modes[] = {TEST_FAILURE_BEFORE, TEST_FAILURE_DURABLE_PREFIX};
    for (size_t mode = 0; mode < 2; ++mode) {
      open_adapter(8);
      make_slot(old_bytes, slots[slot], 1);
      make_slot(new_bytes, slots[slot], 2);
      adapter.stable_writes = true;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, slots[slot], 1,
                                               old_bytes, PFS_BLOCK_SIZE));
      adapter.stable_writes = false;
      fail_next(TEST_FAILURE_WRITE, modes[mode], 1, 0);
      TEST_ASSERT_EQUAL(PFS_IO, pfs_block_write(&adapter.builder, slots[slot], 1,
                                               new_bytes, PFS_BLOCK_SIZE));
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, slots[slot], 1, observed));
      TEST_ASSERT_EQUAL_MEMORY(mode == 0 ? old_bytes : new_bytes, observed, PFS_BLOCK_SIZE);
      close_adapter();
    }
  }
}

static void
cache_only_slot_survives_successful_backend_flush(void)
{
  /* These independently compared slot bytes exercise adapter provenance only;
   * publisher tests must establish complete retained states and sticky health. */
  open_adapter(8);
  make_slot(old_bytes, 0, 1);
  make_slot(new_bytes, 0, 2);
  adapter.stable_writes = true;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 0, 1, old_bytes, PFS_BLOCK_SIZE));
  adapter.stable_writes = false;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 0, 1, new_bytes, PFS_BLOCK_SIZE));
  fail_next(TEST_FAILURE_FLUSH, TEST_FAILURE_CACHE_ONLY_SLOT, 0, 0);
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL_size_t(1, adapter.record_count);
  TEST_ASSERT_EQUAL_size_t(0, adapter.pending_count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 0, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, PFS_BLOCK_SIZE);
  struct pfs_superblock warm;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(observed, PFS_BLOCK_SIZE,
    PFS_POOL_BLOCKS_MIN, 0, &warm));
  TEST_ASSERT_EQUAL_UINT64(2, warm.header.birth);
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &adapter, 0));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&snapshot.builder.reader, 0, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, PFS_BLOCK_SIZE);
  struct pfs_superblock cold;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(observed, PFS_BLOCK_SIZE,
    PFS_POOL_BLOCKS_MIN, 0, &cold));
  TEST_ASSERT_EQUAL_UINT64(1, cold.header.birth);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&adapter));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 0, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, PFS_BLOCK_SIZE);
  close_adapter();
}

static void
early_promotion_and_newer_writes_preserve_order(void)
{
  open_adapter(8);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 3,
                                          new_bytes, sizeof(new_bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_promote_block(&adapter, 101));
  TEST_ASSERT_EQUAL_size_t(2, adapter.pending_count);
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&adapter));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 3, observed));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, PFS_BLOCK_SIZE);
  TEST_ASSERT_EQUAL_MEMORY(new_bytes + PFS_BLOCK_SIZE, observed + PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
  TEST_ASSERT_EQUAL_MEMORY(old_bytes + 2 * PFS_BLOCK_SIZE, observed + 2 * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 1, old_bytes, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 1, new_bytes, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, PFS_BLOCK_SIZE);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(&adapter, 100, 1, observed));
  TEST_ASSERT_EQUAL_MEMORY(new_bytes, observed, PFS_BLOCK_SIZE);
  /* A newer promoted write must supersede an older cache-only incarnation. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 0, 1, new_bytes, PFS_BLOCK_SIZE));
  fail_next(TEST_FAILURE_FLUSH, TEST_FAILURE_CACHE_ONLY_SLOT, 0, 0);
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 0, 1, old_bytes, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL_size_t(0, adapter.record_count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 0, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_MEMORY(old_bytes, observed, PFS_BLOCK_SIZE);
  close_adapter();
}

struct observer_state {
  uint64_t before;
  uint64_t after;
  bool bad;
};

static void
observe_event(struct test_failure *instance, const struct test_failure_event *event,
              bool before, void *context)
{
  struct observer_state *state = context;
  if (before) {
    state->before++;
  } else {
    state->after++;
  }
  if (!event->number || event->ordinal != instance->ordinals[event->kind]) {
    state->bad = true;
  }
}

static void
trace_observers_memory_failures_and_bounds(void)
{
  open_adapter(8);
  struct observer_state state = {0};
  adapter.observer = observe_event;
  adapter.observer_context = &state;
  uint64_t last_event = adapter.event_number;
  test_failure_trace_reset(&adapter);
  fail_next(TEST_FAILURE_READ, TEST_FAILURE_BEFORE, 0, 0);
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_read(&adapter.builder.reader, 100, 1, observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL_size_t(1, adapter.event_count);
  TEST_ASSERT_EQUAL_UINT64(last_event + 1, adapter.events[0].number);
  TEST_ASSERT_EQUAL(PFS_IO, adapter.events[0].status);
  TEST_ASSERT_TRUE(adapter.events[0].selected);
  TEST_ASSERT_EQUAL_UINT64(1, state.before);
  TEST_ASSERT_EQUAL_UINT64(1, state.after);
  TEST_ASSERT_FALSE(state.bad);
  struct pfs_allocation allocation = {0};
  test_failure_memory_fail_after(&adapter, 0);
  TEST_ASSERT_EQUAL(PFS_NO_MEMORY, pfs_memory_allocate(adapter.memory, 64, 8, &allocation));
  TEST_ASSERT_NULL(allocation.data);
  test_failure_memory_fail_after(&adapter, SIZE_MAX);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_allocate(adapter.memory, 64, 8, &allocation));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_free(adapter.memory, &allocation));
  close_adapter();

  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&adapter, PFS_POOL_BLOCKS_MIN, 2, 0, NULL));
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_write(&adapter.builder, 100, 3, new_bytes, sizeof(new_bytes)));
  TEST_ASSERT_TRUE(adapter.infrastructure_failure);
  TEST_ASSERT_FALSE(test_failure_close(&adapter));

  /* Trace overflow is also infrastructure failure, never an acknowledged cut. */
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&adapter, PFS_POOL_BLOCKS_MIN, 2, 0, NULL));
  for (size_t i = 0; i < TEST_FAILURE_EVENTS_MAX; ++i) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&adapter.builder.reader, 100, 1,
                                           observed, PFS_BLOCK_SIZE));
  }
  TEST_ASSERT_EQUAL(PFS_IO, pfs_block_read(&adapter.builder.reader, 100, 1,
                                         observed, PFS_BLOCK_SIZE));
  TEST_ASSERT_TRUE(adapter.infrastructure_failure);
  TEST_ASSERT_FALSE(test_failure_close(&adapter));
}

static void
healthy_trace_cuts_require_the_publication_protocol(void)
{
  open_adapter(8);
  uint64_t base = adapter.ordinals[TEST_FAILURE_FLUSH];
  test_failure_trace_reset(&adapter);
  /* An opening validation flush is not a publication. The following writes
   * deliberately construct the format's replacement/flush/slot/flush protocol. */
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 100, 3,
    new_bytes, sizeof(new_bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&adapter.builder, 0, 1,
    new_bytes, PFS_BLOCK_SIZE));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&adapter.builder));
  struct test_failure_flush_cut cuts[2];
  size_t count = 99;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  TEST_ASSERT_EQUAL_size_t(2, count);
  TEST_ASSERT_EQUAL_UINT64(2, cuts[0].ordinal);
  TEST_ASSERT_EQUAL_UINT64(3, cuts[1].ordinal);
  TEST_ASSERT_EQUAL_size_t(0, cuts[0].publication);
  TEST_ASSERT_EQUAL_size_t(0, cuts[1].publication);
  TEST_ASSERT_FALSE(cuts[0].publishes);
  TEST_ASSERT_TRUE(cuts[1].publishes);
  TEST_ASSERT_EQUAL(PFS_LIMIT, test_failure_flush_cuts(&adapter, base, cuts, 1, &count));
  adapter.event_count--;
  TEST_ASSERT_EQUAL(PFS_INVALID, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  adapter.event_count++;
  struct test_failure_event original = adapter.events[2];
  adapter.events[2].kind = TEST_FAILURE_READ; /* Missing pre-slot flush. */
  TEST_ASSERT_EQUAL(PFS_INVALID, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  adapter.events[2] = original;
  adapter.events[2].status = PFS_IO;
  TEST_ASSERT_EQUAL(PFS_INVALID, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  adapter.events[2] = original;
  adapter.events[0].kind = TEST_FAILURE_CUT;
  TEST_ASSERT_EQUAL(PFS_INVALID, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  adapter.events[0].kind = TEST_FAILURE_FLUSH;
  adapter.events[1].first = PFS_POOL_BLOCKS_MIN - 2; /* Range crosses the final slot. */
  TEST_ASSERT_EQUAL(PFS_INVALID, test_failure_flush_cuts(&adapter, base, cuts, 2, &count));
  close_adapter();
}

static void
existing_fixture_and_direct_builder_use_real_core(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object object = {
    .id = {{4}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY,
  };
  struct pfs_build_volume volume = {
    .id = {{2}}, .root_object = object.id, .owner = {{3}},
    .name = {.length = 1, .bytes = {'v'}}, .object_count = 1, .objects = &object,
  };
  struct pfs_build_spec spec = {
    .pool = {{1}}, .block_count = PFS_POOL_BLOCKS_MIN,
    .volume_count = 1, .volumes = &volume,
  };
  struct pfs_build_plan plan = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, NULL));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&adapter, PFS_POOL_BLOCKS_MIN, 32, 0,
                                             &seed.builder.reader));
  struct pfs_check_result checked;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&adapter.builder.reader, adapter.memory, NULL, NULL, &checked));
  TEST_ASSERT_TRUE(checked.cross_complete);
  close_adapter();
  TEST_ASSERT_TRUE(test_fixture_close(&seed));

  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&adapter, PFS_POOL_BLOCKS_MIN, 32, 0, NULL));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(adapter.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &adapter.builder, NULL));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &adapter, 0));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&snapshot.builder.reader, snapshot.memory, NULL, NULL, &checked));
  TEST_ASSERT_TRUE(checked.cross_complete);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  close_adapter();
}

void
run_failure_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(pending_cache_and_durable_are_separate);
  RUN_TEST(replacement_failures_keep_declared_prefix);
  RUN_TEST(flush_failures_promote_only_selected_blocks);
  RUN_TEST(slot_failures_and_actual_byte_tears);
  RUN_TEST(cache_only_slot_survives_successful_backend_flush);
  RUN_TEST(early_promotion_and_newer_writes_preserve_order);
  RUN_TEST(trace_observers_memory_failures_and_bounds);
  RUN_TEST(healthy_trace_cuts_require_the_publication_protocol);
  RUN_TEST(existing_fixture_and_direct_builder_use_real_core);
}
