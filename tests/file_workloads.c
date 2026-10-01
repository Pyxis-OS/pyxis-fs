/* SPDX-License-Identifier: MPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "file_workloads.h"
#include "failure.h"
#include "writer.h"
#include "unity.h"
#include <pyxis_fs/build.h>

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WORKLOAD_BLOCKS (UINT64_C(4) * 1024 * 1024 * 1024 / PFS_BLOCK_SIZE)
#define SEQUENTIAL_BYTES (4u * 1024u * 1024u)
#define REQUEST_BYTES (256u * 1024u)
#define SOURCE_MAX_BYTES (1024u * 1024u)

static const char *source_paths[] = {
  "core/base.c", "core/block.c", "core/record.c", "core/tree.c",
  "core/writer.c", "core/plan.c", "include/pyxis_fs/base.h",
  "include/pyxis_fs/block.h", "include/pyxis_fs/platform.h",
  "include/pyxis_fs/record.h", "include/pyxis_fs/tree.h", "include/pyxis_fs/write.h",
};
#define SOURCE_COUNT (sizeof(source_paths) / sizeof(source_paths[0]))

static struct test_failure device;
static struct test_fixture sources, expected_sequential, expected_small;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *parent, *sequential, *small;
static struct pfs_build_object objects[SOURCE_COUNT + 3];
static uint64_t source_offsets[SOURCE_COUNT], source_lengths[SOURCE_COUNT];
static uint64_t imported_extents, sequential_extents, small_extents;
static uint8_t payload[REQUEST_BYTES], actual[64u * 1024u], expected[64u * 1024u];
static struct pfs_write_options options = {
  .extent_limit = 8192, .metadata_limit = 4096, .random = test_random,
};
static const struct pfs_trusted_context authority = {
  .principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
  .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL},
};

struct workload_stats {
  uint64_t useful_bytes;
  uint64_t user_metadata, user_data, drain_metadata, drain_data;
  uint64_t user_flushes, drain_flushes, user_publications, drain_publications;
  unsigned calls;
  double seconds, maximum_latency, user_callback_seconds, drain_callback_seconds;
  double callback_start;
  bool callback_drain;
  bool violation;
};
static struct workload_stats stats;

static double
now(void)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time)) {
    return -1;
  }
  return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

/* Observe the admitted user batch's data claims, never decode payload bytes as
 * metadata. drain_pending separates the publisher's synchronous maintenance.
 * Observer callbacks inspect copied fields only and cannot reenter the core. */
static void
observe(struct test_failure *adapter, const struct test_failure_event *event,
        bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (before) {
    stats.callback_start = now();
    stats.callback_drain = pool.writer->status.drain_pending;
    stats.violation |= stats.callback_start < 0;
    return;
  }
  double ended = now();
  stats.violation |= ended < stats.callback_start || event->status != PFS_OK;
  if (stats.callback_drain) {
    stats.drain_callback_seconds += ended - stats.callback_start;
  } else {
    stats.user_callback_seconds += ended - stats.callback_start;
  }
  if (event->kind == TEST_FAILURE_FLUSH) {
    if (stats.callback_drain) {
      stats.drain_flushes++;
    } else {
      stats.user_flushes++;
    }
  }
  if (event->kind != TEST_FAILURE_WRITE) {
    return;
  }
  for (uint64_t block = event->first; block < event->first + event->count; block++) {
    bool data = false;
    if (!stats.callback_drain) {
      const struct pfs_batch *batch = &pool.writer->batch;
      for (size_t i = 0; i < batch->add_count; i++) {
        const struct check_claim *claim = &batch->add[i];
        data |= !claim->type && block >= claim->first && block - claim->first < claim->count;
      }
    }
    if (stats.callback_drain) {
      stats.drain_data += data * PFS_BLOCK_SIZE;
      stats.drain_metadata += !data * PFS_BLOCK_SIZE;
      stats.drain_publications += (block == 0 || block == WORKLOAD_BLOCKS - 1);
    } else {
      stats.user_data += data * PFS_BLOCK_SIZE;
      stats.user_metadata += !data * PFS_BLOCK_SIZE;
      stats.user_publications += (block == 0 || block == WORKLOAD_BLOCKS - 1);
    }
  }
}

static void
report(const char *name)
{
  TEST_ASSERT_FALSE(stats.violation);
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  printf("workload %s: calls=%u useful-bytes=%llu elapsed=%.6fs throughput=%.3fMiB/s "
         "mean-call=%.6fs max-call=%.6fs\n"
         "  user metadata-bytes=%llu data-bytes=%llu publications=%llu flushes=%llu callback=%.6fs\n"
         "  drain metadata-bytes=%llu data-bytes=%llu publications=%llu flushes=%llu callback=%.6fs\n",
    name, stats.calls, (unsigned long long)stats.useful_bytes, stats.seconds,
    stats.seconds ? (double)stats.useful_bytes / (1024 * 1024) / stats.seconds : 0,
    stats.calls ? stats.seconds / stats.calls : 0, stats.maximum_latency,
    (unsigned long long)stats.user_metadata, (unsigned long long)stats.user_data,
    (unsigned long long)stats.user_publications, (unsigned long long)stats.user_flushes,
    stats.user_callback_seconds, (unsigned long long)stats.drain_metadata,
    (unsigned long long)stats.drain_data, (unsigned long long)stats.drain_publications,
    (unsigned long long)stats.drain_flushes, stats.drain_callback_seconds);
  if (stats.useful_bytes) {
    printf("  metadata/useful-byte user=%.6f drain=%.6f total=%.6f\n",
      (double)stats.user_metadata / stats.useful_bytes,
      (double)stats.drain_metadata / stats.useful_bytes,
      (double)(stats.user_metadata + stats.drain_metadata) / stats.useful_bytes);
  }
  TEST_ASSERT_EQUAL_UINT64(2 * stats.user_publications, stats.user_flushes);
  TEST_ASSERT_EQUAL_UINT64(2 * stats.drain_publications, stats.drain_flushes);
  stats = (struct workload_stats){0};
}

/* Only one output file changes in a phase. The admitted volume total minus
 * the unchanged imported and other output mappings gives its extent count. */
static void
report_extents(bool changed_small)
{
  const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
  uint64_t total = state->volumes[0].file_extents;
  uint64_t unchanged = imported_extents + (changed_small ? sequential_extents : small_extents);
  TEST_ASSERT_TRUE(total >= unchanged);
  if (changed_small) {
    small_extents = total - unchanged;
  } else {
    sequential_extents = total - unchanged;
  }
  printf("  extents imported=%llu sequential=%llu small=%llu metadata-blocks=%llu "
         "allocation-records=%zu\n", (unsigned long long)imported_extents,
    (unsigned long long)sequential_extents, (unsigned long long)small_extents,
    (unsigned long long)state->volumes[0].metadata_blocks, state->map_count);
}

static enum pfs_status
snapshot_source(size_t index)
{
  char path[4096];
  const char *slash = strrchr(__FILE__, '/');
  if (!slash || snprintf(path, sizeof(path), "%.*s/../%s", (int)(slash - __FILE__),
                         __FILE__, source_paths[index]) >= (int)sizeof(path)) {
    return PFS_INVALID;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    return PFS_IO;
  }
  struct stat metadata;
  enum pfs_status status = PFS_OK;
  if (fstat(fileno(file), &metadata) || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || (uint64_t)metadata.st_size > SOURCE_MAX_BYTES) {
    status = PFS_IO;
  }
  uint64_t offset = index ? source_offsets[index - 1] + source_lengths[index - 1] : 0;
  source_offsets[index] = offset;
  if (status == PFS_OK) {
    source_lengths[index] = (uint64_t)metadata.st_size;
    for (uint64_t done = 0; done < source_lengths[index];) {
      size_t length = source_lengths[index] - done < sizeof(expected) ?
        (size_t)(source_lengths[index] - done) : sizeof(expected);
      if (fread(expected, 1, length, file) != length ||
          pwrite(fileno(sources.file), expected, length, (off_t)(offset + done)) != (ssize_t)length) {
        status = PFS_IO;
        break;
      }
      done += length;
    }
  }
  if (fclose(file)) {
    status = PFS_IO;
  }
  return status;
}

static enum pfs_status
source_read(void *context, size_t v, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (v || object < 3 || object >= SOURCE_COUNT + 3) {
    return PFS_INVALID;
  }
  size_t index = object - 3;
  if (offset > source_lengths[index] || length > source_lengths[index] - offset) {
    return PFS_INVALID;
  }
  return pread(fileno(sources.file), buffer, length,
    (off_t)(source_offsets[index] + offset)) == (ssize_t)length ? PFS_OK : PFS_IO;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
open_writer(void)
{
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  parent = sequential = small = NULL;
  test_failure_trace_reset(&device);
  struct pfs_write_open_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder, device.memory,
                                               &options, &result));
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volume));
}

static void
close_writer(void)
{
  device.observer = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&sequential, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&small, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&parent, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
}

static void
setup(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&sources, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_sequential, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_small, PFS_POOL_BLOCKS_MIN, 0));
  objects[0] = (struct pfs_build_object){.id = {{3}}, .parent = UINT32_MAX,
    .kind = PFS_OBJECT_DIRECTORY};
  objects[1] = (struct pfs_build_object){.id = {{4}}, .parent = 0,
    .kind = PFS_OBJECT_DIRECTORY, .name = {4, "core"}};
  objects[2] = (struct pfs_build_object){.id = {{6}}, .parent = 0,
    .kind = PFS_OBJECT_DIRECTORY, .name = {7, "include"}};
  uint64_t payload_bytes = 0;
  for (size_t i = 0; i < SOURCE_COUNT; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, snapshot_source(i));
    const char *name = strrchr(source_paths[i], '/') + 1;
    objects[i + 3] = (struct pfs_build_object){.id = {{(uint8_t)(16 + i)}},
      .parent = i < 6 ? 1 : 2, .kind = PFS_OBJECT_FILE, .file_length = source_lengths[i]};
    objects[i + 3].name.length = (uint16_t)strlen(name);
    memcpy(objects[i + 3].name.bytes, name, strlen(name));
    payload_bytes += source_lengths[i];
  }
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(WORKLOAD_BLOCKS, options.extent_limit,
    options.metadata_limit, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, WORKLOAD_BLOCKS,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 128u * 1024u * 1024u, NULL));
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .guarantee_set = true, .guarantee = 0,
    .object_count = SOURCE_COUNT + 3, .objects = objects};
  struct pfs_build_spec spec = {.block_count = WORKLOAD_BLOCKS, .pool = {{1}},
    .volume_count = 1, .volumes = &v,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 8192, .migration_reserve = 8192, .recovery_reserve = 8192};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(device.memory, &spec, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &device.builder, &source));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  printf("file-workloads: image=4GiB E=8192 M=4096 memory=128MiB sources=%zu "
         "source-bytes=%llu reserves=8192/8192/8192 blocks seed=1\n",
         SOURCE_COUNT, (unsigned long long)payload_bytes);
  open_writer();
  imported_extents = pool.writer->states[pool.writer->selected].volumes[0].file_extents;
  sequential_extents = small_extents = 0;
  struct pfs_rights held = {.file = PFS_FILE_READ | PFS_FILE_WRITE | PFS_FILE_RESIZE |
    PFS_FILE_METADATA, .directory = PFS_DIR_CREATE | PFS_DIR_LOOKUP};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
    PFS_SCOPE_SUBTREE, &held, &parent));
  struct pfs_rights file = {.file = held.file};
  struct pfs_view_identity identity;
  struct pfs_write_result result;
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent, (const uint8_t *)"sequential", 10,
    &file, &sequential, &identity, &result));
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(parent, (const uint8_t *)"small", 5,
    &file, &small, &identity, &result));
  stats = (struct workload_stats){0};
  device.observer = observe;
}

static void
account_call(double start, size_t useful)
{
  double latency = now() - start;
  TEST_ASSERT_TRUE(start >= 0 && latency >= 0);
  stats.calls++;
  stats.seconds += latency;
  stats.useful_bytes += useful;
  if (latency > stats.maximum_latency) {
    stats.maximum_latency = latency;
  }
}

static void
write_expected(struct pfs_view *view, struct test_fixture *host,
               uint64_t offset, size_t length)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result;
  double start = now();
  enum pfs_status status = pfs_view_write(view, offset, payload, length, &result);
  account_call(start, length);
  TEST_ASSERT_EQUAL(PFS_OK, status);
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_EQUAL_UINT64(length, result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_INT64((int64_t)length,
    pwrite(fileno(host->file), payload, length, (off_t)offset));
}

static void
resize_expected(uint64_t length)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result;
  double start = now();
  enum pfs_status status = pfs_view_resize(sequential, length, &result);
  account_call(start, 0);
  TEST_ASSERT_EQUAL(PFS_OK, status);
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result.completion);
  TEST_ASSERT_TRUE(result.confirmed_length_valid);
  TEST_ASSERT_EQUAL_UINT64(length, result.confirmed_length);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_sequential.file), (off_t)length));
}

static void
compare_view(struct pfs_view *view, FILE *host, uint64_t first, uint64_t length)
{
  struct pfs_view_metadata metadata;
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(view, &metadata));
  TEST_ASSERT_EQUAL_UINT64(length, metadata.size);
  for (uint64_t offset = 0; offset < length;) {
    size_t count = length - offset < sizeof(actual) ? (size_t)(length - offset) : sizeof(actual);
    TEST_ASSERT_EQUAL_INT64((int64_t)count,
      pread(fileno(host), expected, count, (off_t)(first + offset)));
    size_t got;
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, offset, actual, count, &got));
    TEST_ASSERT_EQUAL_UINT(count, got);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, count);
    offset += count;
  }
}

static void
populated_sequential_small_and_edit_history(void)
{
  setup();
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_sequential.file), 0));
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_small.file), 0));
  for (unsigned i = 0; i < SEQUENTIAL_BYTES / REQUEST_BYTES; i++) {
    for (size_t j = 0; j < sizeof(payload); j++) {
      payload[j] = (uint8_t)(i * 17 + j * 29 + (j >> 8));
    }
    write_expected(sequential, &expected_sequential, (uint64_t)i * REQUEST_BYTES, REQUEST_BYTES);
  }
  report("sequential-256KiB");
  report_extents(false);
  for (unsigned i = 0; i < 64; i++) {
    for (size_t j = 0; j < PFS_BLOCK_SIZE; j++) {
      payload[j] = (uint8_t)(i * 31 + j * 7);
    }
    write_expected(small, &expected_small, (uint64_t)i * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
  }
  report("independent-4KiB-appends");
  report_extents(true);
  uint32_t random = 1;
  for (unsigned i = 0; i < 16; i++) {
    random = random * 1664525u + 1013904223u;
    uint64_t offset = random % (SEQUENTIAL_BYTES - 8193u);
    for (size_t j = 0; j < 8193; j++) {
      payload[j] = (uint8_t)(i * 53 + j * 13);
    }
    write_expected(sequential, &expected_sequential, offset, 8193);
  }
  report("partial-overwrites");
  report_extents(false);
  resize_expected(SEQUENTIAL_BYTES / 2 + 17);
  resize_expected(SEQUENTIAL_BYTES + PFS_BLOCK_SIZE);
  report("shrink-regrow");
  report_extents(false);
  close_writer();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
  open_writer();
  struct pfs_rights read = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
    (const uint8_t *)"sequential", 10, PFS_SCOPE_OBJECT, &read, &sequential));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
    (const uint8_t *)"small", 5, PFS_SCOPE_OBJECT, &read, &small));
  compare_view(sequential, expected_sequential.file, 0, SEQUENTIAL_BYTES + PFS_BLOCK_SIZE);
  compare_view(small, expected_small.file, 0, 64u * PFS_BLOCK_SIZE);
  for (size_t i = 0; i < SOURCE_COUNT; i++) {
    char path[64];
    const char *name = strrchr(source_paths[i], '/') + 1;
    int length = snprintf(path, sizeof(path), "%s/%s", i < 6 ? "core" : "include", name);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(path));
    struct pfs_view *source = NULL;
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)path, (size_t)length, PFS_SCOPE_OBJECT, &read, &source));
    compare_view(source, sources.file, source_offsets[i], source_lengths[i]);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&source, &(struct pfs_view_close_result){0}));
  }
  const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
  printf("  final generation=%llu allocation-records=%zu claims=%zu file-extents=%llu "
         "metadata-blocks=%llu charged-memory=%llu\n",
    (unsigned long long)pool.writer->last_confirmed_generation, state->map_count, state->claim_count,
    (unsigned long long)state->volumes[0].file_extents,
    (unsigned long long)state->volumes[0].metadata_blocks,
    (unsigned long long)device.memory->used);
  close_writer();
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&sources));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_sequential));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_small));
}

void
run_file_workloads(void)
{
  RUN_TEST(populated_sequential_small_and_edit_history);
}
