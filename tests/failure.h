/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_TEST_FAILURE_H
#define PFS_TEST_FAILURE_H

#include "support.h"

#define TEST_FAILURE_EVENTS_MAX 8192u

enum test_failure_event_kind {
  TEST_FAILURE_READ,
  TEST_FAILURE_WRITE,
  TEST_FAILURE_FLUSH,
  TEST_FAILURE_CUT,
};

enum test_failure_mode {
  TEST_FAILURE_BEFORE,
  TEST_FAILURE_PENDING_PREFIX,
  TEST_FAILURE_DURABLE_PREFIX,
  TEST_FAILURE_TORN_SLOT,
  TEST_FAILURE_FLUSH_PREFIX,
  TEST_FAILURE_CACHE_ONLY_SLOT,
};

/* One selected callback fails once. Ordinal is one-based within its kind.
 * Prefix counts whole blocks except TORN_SLOT, where it is 32 or 64 bytes.
 * FLUSH_PREFIX with SIZE_MAX promotes all pending writes before returning IO.
 * CACHE_ONLY_SLOT retains the newest complete slot write in cache only while
 * promoting the other pending records; block selects the first/last slot. */
struct test_failure_fault {
  enum test_failure_event_kind kind;
  uint64_t ordinal;
  enum test_failure_mode mode;
  size_t prefix;
  uint64_t block;
  bool enabled;
};

struct test_failure_event {
  uint64_t number;
  enum test_failure_event_kind kind;
  uint64_t ordinal;
  uint64_t first;
  uint32_t count;
  uint64_t flush_ordinal;
  enum pfs_status status;
  bool selected;
  enum test_failure_mode mode;
  size_t prefix;
};

struct test_failure;
/* Called before/after an ordinary callback, never through a Unity assertion.
 * Observers may inspect/clone durable bytes; they must not reenter core handles
 * or change the adapter while its callback is active. Record violations and
 * assert after the core returns normally. */
typedef void (*test_failure_observer_fn)(struct test_failure *adapter,
  const struct test_failure_event *event, bool before, void *context);

struct test_failure_log_record;
/* Use static storage so teardown can reclaim resources after a Unity abort.
 * Controls and the one transfer buffer are outside separately capped core
 * memory. The sparse backing file is the explicit durable plane; the log is
 * volatile pending/cache-only state, not real-host power-loss evidence. */
struct test_failure {
  struct test_fixture backing;
  struct pfs_block_builder builder;
  struct pfs_memory *memory;
  FILE *log;
  struct test_failure_log_record *records;
  size_t dirty_capacity;
  size_t record_count;
  size_t pending_count;
  uint8_t transfer[PFS_IO_BLOCKS_MAX * PFS_BLOCK_SIZE];
  struct test_failure_event events[TEST_FAILURE_EVENTS_MAX];
  size_t event_count;
  uint64_t event_number;
  uint64_t ordinals[4];
  struct test_failure_fault fault;
  bool triggered;
  bool infrastructure_failure;
  bool stable_writes;
  test_failure_observer_fn observer;
  void *observer_context;
  struct test_failure *next;
};

/* dirty_capacity is the admitted H+V+1 block/record allowance. NULL seed starts
 * empty and can be populated directly through builder. A seed reader is copied
 * with bounded memory into sparse durable storage without tracing core events.
 * Successful default writes are pending until flush. Set stable_writes only for
 * a scenario declaring an explicit durable boundary at completed writes. */
enum pfs_status test_failure_open(struct test_failure *adapter, uint64_t blocks,
  size_t dirty_capacity, uint64_t memory_limit, const struct pfs_block_reader *seed);
enum pfs_status test_failure_clone_durable(struct test_failure *destination,
  const struct test_failure *source, uint64_t memory_limit);
enum pfs_status test_failure_set_fault(struct test_failure *adapter,
                                      const struct test_failure_fault *fault);
void test_failure_memory_fail_after(struct test_failure *adapter, size_t successes);
/* Clearing trace storage retains monotonic event/operation ordinals. */
void test_failure_trace_reset(struct test_failure *adapter);
enum pfs_status test_failure_durable_read(const struct test_failure *adapter,
  uint64_t first, uint32_t count, void *buffer);
/* Explicit early whole-block promotion for a selected pre-flush cut. All actual
 * promoted bytes are fsynced before inspection. These test controls do not call
 * the core; cuts require quiescence and callers discard core runtime handles
 * before cold reopening. A cut never implies core retry permission. */
enum pfs_status test_failure_promote_block(struct test_failure *adapter, uint64_t block);
enum pfs_status test_failure_cold_cut(struct test_failure *adapter);
bool test_failure_close(struct test_failure *adapter);
/* Main teardown calls this BEFORE test_fixtures_cleanup(). */
void test_failures_cleanup(void);

#endif
