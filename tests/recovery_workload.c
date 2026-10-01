/* SPDX-License-Identifier: MPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "recovery_workload.h"
#include "failure.h"
#include "writer.h"
#include "unity.h"
#include "../host/source.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RECOVERY_BLOCKS (UINT64_C(4) * 1024 * 1024 * 1024 / PFS_BLOCK_SIZE)
#define SEQUENTIAL_BYTES (UINT64_C(1) * 1024 * 1024 * 1024)
#define REQUEST_BYTES (256u * 1024u)
#define SMALL_APPENDS 2000u
#define OVERWRITES 32u
#define RETAINED_ROUNDS 8u
#define FORMAT_COW_FLOOR 1024u
#define FORMAT_MIGRATION_FLOOR 1024u
#define FORMAT_RECOVERY_FLOOR 256u
#define CORE_MEMORY_CAP (128u * 1024u * 1024u)
#define SOURCE_PATH_BYTES (PFS_ANCESTRY_MAX * (PFS_NAME_MAX + 1u))

struct source_record {
  struct pfs_build_object object;
  uint64_t offset;
  uint64_t children;
  size_t source_index;
  char *path;
};

struct phase_counters {
  uint64_t metadata_bytes, data_bytes, publications, flushes;
  double callback_seconds;
};

struct workload_stats {
  struct phase_counters phases[3];
  uint64_t calls, useful_bytes;
  double seconds, maximum_latency, callback_start;
  unsigned callback_phase;
  bool violation;
};

enum workload_phase { PHASE_USER, PHASE_ORPHAN, PHASE_DRAIN };

static struct test_failure device;
static struct test_fixture expected_sources, expected_sequential, expected_small, expected_churn;
static struct pfs_allocation manifest, source_paths;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *parent, *sequential, *small, *retained, *replacement;
static struct pfs_object_id sequential_id, small_id;
static struct pfs_plan_limits limits;
static struct workload_stats stats;
static size_t source_objects;
static uint64_t source_files, source_directories, source_bytes, source_blocks;
static uint64_t workload_seed;
static const char *source_directory;
static const char *active_phase;
static bool workload_failed, healthy_refusal;
static bool sequential_created, small_created;
static uint64_t random_counter;
static uint64_t retained_length, replacement_length, churn_named_length, churn_named_base;
static bool churn_named;
static struct pfs_object_id churn_named_id;
static const char *current_sequential_name;
static uint8_t payload[REQUEST_BYTES], actual[64u * 1024u], expected[64u * 1024u];
static char source_relative[SOURCE_PATH_BYTES];
static const char *sequential_name = "pfs-recovery-sequential";
static const char *renamed_name = "pfs-recovery-renamed";
static const char *small_name = "pfs-recovery-small";
static const char *churn_name = "pfs-recovery-churn";
static struct pfs_write_options options = {
  .extent_limit = 8192, .metadata_limit = 4096, .random = test_random,
};
static const struct pfs_trusted_context authority = {
  .principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
  .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL},
};
static const struct pfs_rights file_rights = {
  .file = PFS_FILE_READ | PFS_FILE_WRITE | PFS_FILE_RESIZE | PFS_FILE_METADATA,
};

static double
now(void)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time)) {
    return -1;
  }
  return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

/* Phase observation copies admitted publisher fields only; no core reentry. */
static void
observe(struct test_failure *adapter, const struct test_failure_event *event,
        bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (before) {
    stats.callback_start = now();
    stats.callback_phase = pool.writer->status.drain_pending ? PHASE_DRAIN :
      pool.writer->batch.orphan_cleanup ? PHASE_ORPHAN : PHASE_USER;
    stats.violation |= stats.callback_start < 0;
    return;
  }
  double ended = now();
  stats.violation |= ended < stats.callback_start || event->status != PFS_OK;
  struct phase_counters *phase = &stats.phases[stats.callback_phase];
  phase->callback_seconds += ended - stats.callback_start;
  if (event->kind == TEST_FAILURE_FLUSH) {
    phase->flushes++;
  }
  if (event->kind != TEST_FAILURE_WRITE) {
    return;
  }
  for (uint64_t block = event->first; block < event->first + event->count; block++) {
    bool data = false;
    if (stats.callback_phase != PHASE_DRAIN) {
      const struct pfs_batch *batch = &pool.writer->batch;
      for (size_t i = 0; i < batch->add_count; i++) {
        const struct check_claim *claim = &batch->add[i];
        data |= !claim->type && block >= claim->first && block - claim->first < claim->count;
      }
    }
    phase->data_bytes += data * PFS_BLOCK_SIZE;
    phase->metadata_bytes += !data * PFS_BLOCK_SIZE;
    phase->publications += block == 0 || block == RECOVERY_BLOCKS - 1;
  }
}

static void
report_state(const char *name)
{
  const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
  const struct pfs_admit_volume *v = &state->volumes[0];
  const struct pfs_pool_root *root = &state->candidate.root;
  printf("  state %s: generation=%llu objects=%llu directories=%llu maps=%zu claims=%zu "
         "extents=%llu metadata=%llu effective=%llu namespace=%llu deletion=%llu "
         "orphan-work=%llu charged-memory=%llu peak-charged-memory=%llu\n"
         "  budgets cow=%llu/%llu migration=%llu/%llu recovery=%llu/%llu "
         "live=%llu retired=%llu reusable=%llu\n", name,
    (unsigned long long)pool.writer->last_confirmed_generation,
    (unsigned long long)v->record.object_count, (unsigned long long)v->directories,
    state->map_count, state->claim_count, (unsigned long long)v->file_extents,
    (unsigned long long)v->metadata_blocks, (unsigned long long)v->effective_blocks,
    (unsigned long long)v->namespace_nodes, (unsigned long long)v->deletion_blocks,
    (unsigned long long)v->orphan_work, (unsigned long long)device.memory->used,
    (unsigned long long)device.backing.peak_memory_bytes,
    (unsigned long long)root->cow.occupied, (unsigned long long)root->cow.capacity,
    (unsigned long long)root->migration.occupied, (unsigned long long)root->migration.capacity,
    (unsigned long long)root->recovery.occupied, (unsigned long long)root->recovery.capacity,
    (unsigned long long)v->record.live_blocks, (unsigned long long)v->record.retired_blocks,
    (unsigned long long)state->reusable_blocks);
  fflush(stdout);
}

static uint64_t
physical_bytes(FILE *file)
{
  struct stat info;
  TEST_ASSERT_EQUAL_INT(0, fstat(fileno(file), &info));
  return (uint64_t)info.st_blocks * 512;
}

static void
report(const char *name)
{
  printf("recovery-workload %s: calls=%llu confirmed-useful-bytes=%llu elapsed=%.6fs "
         "throughput=%.3fMiB/s mean-call=%.6fs max-call=%.6fs\n", name,
    (unsigned long long)stats.calls, (unsigned long long)stats.useful_bytes, stats.seconds,
    stats.seconds ? (double)stats.useful_bytes / (1024 * 1024) / stats.seconds : 0,
    stats.calls ? stats.seconds / stats.calls : 0, stats.maximum_latency);
  const char *names[] = {"user", "orphan", "drain"};
  uint64_t metadata = 0;
  for (size_t i = 0; i < 3; i++) {
    const struct phase_counters *p = &stats.phases[i];
    metadata += p->metadata_bytes;
    printf("  %s metadata-bytes=%llu data-bytes=%llu publications=%llu flushes=%llu "
           "callback=%.6fs metadata/useful=%.6f\n", names[i],
      (unsigned long long)p->metadata_bytes, (unsigned long long)p->data_bytes,
      (unsigned long long)p->publications, (unsigned long long)p->flushes,
      p->callback_seconds, stats.useful_bytes ? (double)p->metadata_bytes / stats.useful_bytes : 0);
    TEST_ASSERT_EQUAL_UINT64(2 * p->publications, p->flushes);
  }
  printf("  total metadata/useful=%.6f physical-image=%llu physical-log=%llu "
         "physical-expected=%llu\n",
    stats.useful_bytes ? (double)metadata / stats.useful_bytes : 0,
    (unsigned long long)physical_bytes(device.backing.file),
    (unsigned long long)physical_bytes(device.log),
    (unsigned long long)(physical_bytes(expected_sources.file) +
      physical_bytes(expected_sequential.file) + physical_bytes(expected_small.file) +
      physical_bytes(expected_churn.file)));
  report_state(name);
  TEST_ASSERT_FALSE(stats.violation);
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  stats = (struct workload_stats){0};
}

static void
account_call(double start, uint64_t useful)
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

static bool
require_complete(const char *operation, enum pfs_status status,
                 const struct pfs_write_result *result)
{
  bool failed = status != PFS_OK || result->completion != PFS_COMPLETE ||
    result->operation_status != PFS_OK || result->maintenance_status != PFS_OK ||
    (result->maintenance_completion != PFS_MAINTENANCE_NONE &&
     result->maintenance_completion != PFS_MAINTENANCE_COMPLETE) ||
    result->health != PFS_WRITER_READY;
  if (failed) {
    printf("UNEXPECTED workload refusal/failure: %s status=%s completion=%u "
           "operation=%s confirmed=%llu maintenance=%u/%s health=%u\n", operation,
      pfs_status_string(status), result->completion, pfs_status_string(result->operation_status),
      (unsigned long long)result->confirmed_bytes, result->maintenance_completion,
      pfs_status_string(result->maintenance_status), result->health);
    report_state("failed-call");
  }
  if (failed) {
    workload_failed = true;
    healthy_refusal = result->health == PFS_WRITER_READY &&
      result->completion != PFS_UNKNOWN && !device.infrastructure_failure &&
      (status == PFS_LIMIT || status == PFS_NO_SPACE || status == PFS_QUOTA ||
       status == PFS_NO_MEMORY);
    printf("  failure phase=%s call=%llu E=%llu M=%llu healthy-resource-refusal=%s\n",
      active_phase, (unsigned long long)stats.calls,
      (unsigned long long)options.extent_limit, (unsigned long long)options.metadata_limit,
      healthy_refusal ? "true" : "false");
    fflush(stdout);
    return false;
  }
  return true;
}

static bool
write_expected(struct pfs_view *view, FILE *host, uint64_t host_base,
               uint64_t offset, size_t length)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result = {0};
  double start = now();
  enum pfs_status status = pfs_view_write(view, offset, payload, length, &result);
  account_call(start, result.confirmed_bytes);
  TEST_ASSERT_TRUE(result.confirmed_bytes <= length);
  if (result.confirmed_bytes) {
    TEST_ASSERT_EQUAL_INT64((int64_t)result.confirmed_bytes,
      pwrite(fileno(host), payload, (size_t)result.confirmed_bytes, (off_t)(host_base + offset)));
  }
  uint64_t confirmed_end = offset + result.confirmed_bytes;
  if (result.confirmed_bytes && view == retained && confirmed_end > retained_length) {
    retained_length = confirmed_end;
  }
  if (result.confirmed_bytes && view == replacement && confirmed_end > replacement_length) {
    replacement_length = confirmed_end;
  }
  if (churn_named && (view == retained || view == replacement)) {
    churn_named_length = view == retained ? retained_length : replacement_length;
  }
  if (!require_complete("write", status, &result)) {
    printf("  failed write offset=%llu requested=%zu confirmed-prefix=%llu\n",
      (unsigned long long)offset, length, (unsigned long long)result.confirmed_bytes);
    fflush(stdout);
    return false;
  }
  TEST_ASSERT_EQUAL_UINT64(length, result.confirmed_bytes);
  return true;
}

static bool
create_file(const char *name, struct pfs_view **out, struct pfs_object_id *id)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result = {0};
  struct pfs_view_identity identity;
  double start = now();
  enum pfs_status status = pfs_view_create_file(parent, (const uint8_t *)name,
    strlen(name), &file_rights, out, &identity, &result);
  account_call(start, 0);
  if (!require_complete("create-file", status, &result)) {
    return false;
  }
  TEST_ASSERT_TRUE(result.namespace_confirmed);
  *id = identity.object;
  return true;
}

static bool
remove_file(const char *name)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result = {0};
  double start = now();
  enum pfs_status status = pfs_view_remove(parent, (const uint8_t *)name, strlen(name), &result);
  account_call(start, 0);
  if (!require_complete("remove", status, &result)) {
    return false;
  }
  TEST_ASSERT_TRUE(result.namespace_confirmed);
  return true;
}

static bool
close_file(struct pfs_view **view)
{
  test_failure_trace_reset(&device);
  struct pfs_view_close_result result = {0};
  double start = now();
  enum pfs_status status = pfs_view_close(view, &result);
  account_call(start, 0);
  if (status != PFS_OK || !result.released || result.health != PFS_WRITER_READY ||
      result.maintenance_status != PFS_OK ||
      (result.maintenance_completion != PFS_MAINTENANCE_NONE &&
       result.maintenance_completion != PFS_MAINTENANCE_COMPLETE)) {
    struct pfs_write_result failure = {.completion = PFS_STOPPED,
      .operation_status = status, .health = result.health,
      .maintenance_completion = result.maintenance_completion,
      .maintenance_status = result.maintenance_status};
    printf("  close released=%s\n", result.released ? "true" : "false");
    require_complete("close-file", status, &failure);
    return false;
  }
  return true;
}

static void
compare_view(struct pfs_view *view, FILE *host, uint64_t first, uint64_t length)
{
  /* Verification reads do not enter the timed mutation callback totals. */
  test_failure_observer_fn observer = device.observer;
  device.observer = NULL;
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
    if (length >= 64u * 1024u * 1024u && offset % (64u * 1024u * 1024u) == 0) {
      printf("  verified output bytes=%llu/%llu\n",
        (unsigned long long)offset, (unsigned long long)length);
    }
  }
  device.observer = observer;
}

static enum pfs_status
relative_path(const struct pfs_build_object *objects, size_t count, size_t index,
              char *out, size_t capacity)
{
  uint32_t chain[PFS_ANCESTRY_MAX];
  size_t depth = 0;
  while (index) {
    if (index >= count || depth == PFS_ANCESTRY_MAX || objects[index].parent >= index) {
      return PFS_INVALID;
    }
    chain[depth++] = (uint32_t)index;
    index = objects[index].parent;
  }
  size_t length = 0;
  while (depth) {
    const struct pfs_name *name = &objects[chain[--depth]].name;
    if ((size_t)name->length + (length != 0) >= capacity - length) {
      return PFS_LIMIT;
    }
    if (length) {
      out[length++] = '/';
    }
    memcpy(out + length, name->bytes, name->length);
    length += name->length;
  }
  out[length] = 0;
  return PFS_OK;
}

static void
seed_id(struct pfs_object_id *id, uint8_t tag, uint64_t ordinal)
{
  memset(id, 0, sizeof(*id));
  id->bytes[0] = tag;
  for (size_t i = 0; i < 7; i++) {
    id->bytes[i + 1] = (uint8_t)(ordinal >> (i * 8));
  }
  for (size_t i = 0; i < 8; i++) {
    id->bytes[i + 8] = (uint8_t)(workload_seed >> (i * 8));
  }
}

static enum pfs_status
workload_random(void *context, void *buffer, size_t length)
{
  (void)context;
  if (length != PFS_ID_SIZE || random_counter == UINT64_MAX) {
    return PFS_INVALID;
  }
  struct pfs_object_id id;
  seed_id(&id, 0xa8, ++random_counter);
  memcpy(buffer, id.bytes, sizeof(id.bytes));
  return PFS_OK;
}

static int
source_compare(const void *left, const void *right)
{
  const struct source_record *a = left, *b = right;
  return strcmp(a->path, b->path);
}

struct source_context {
  struct host_source *source;
  struct source_record *records;
};

static enum pfs_status
normalized_source_read(void *context, size_t v, size_t object, uint64_t offset,
                       void *buffer, size_t length)
{
  const struct source_context *source = context;
  if (v || object >= source_objects) {
    return PFS_INVALID;
  }
  return host_source_read(source->source, v, source->records[object].source_index,
    offset, buffer, length);
}

static enum pfs_status
normalized_source_validate(void *context)
{
  const struct source_context *source = context;
  return host_source_validate(source->source);
}

static enum pfs_status
prepare_manifest(struct pfs_build_volume *v)
{
  source_objects = v->object_count;
  enum pfs_status status = pfs_memory_allocate(&expected_sources.memory,
    source_objects * sizeof(struct source_record), _Alignof(struct source_record), &manifest);
  if (status != PFS_OK) {
    return status;
  }
  struct source_record *records = manifest.data;
  memset(records, 0, manifest.size);
  size_t path_bytes = 0;
  for (size_t i = 0; i < source_objects; i++) {
    status = relative_path(v->objects, source_objects, i, source_relative, sizeof(source_relative));
    if (status != PFS_OK) {
      return status;
    }
    if (strlen(source_relative) + 1 > SIZE_MAX - path_bytes) {
      return PFS_LIMIT;
    }
    path_bytes += strlen(source_relative) + 1;
  }
  status = pfs_memory_allocate(&expected_sources.memory, path_bytes, 1, &source_paths);
  if (status != PFS_OK) {
    return status;
  }
  char *path = source_paths.data;
  for (size_t i = 0; i < source_objects; i++) {
    status = relative_path(v->objects, source_objects, i, source_relative, sizeof(source_relative));
    if (status != PFS_OK) {
      return status;
    }
    records[i].object = v->objects[i];
    records[i].source_index = i;
    records[i].path = path;
    strcpy(path, source_relative);
    path += strlen(path) + 1;
  }
  qsort(records, source_objects, sizeof(*records), source_compare);
  for (size_t i = 1; i < source_objects; i++) {
    uint32_t old_parent = records[i].object.parent;
    size_t parent_index = 0;
    while (parent_index < i && records[parent_index].source_index != old_parent) {
      parent_index++;
    }
    if (parent_index == i) {
      return PFS_INVALID;
    }
    records[i].object.parent = (uint32_t)parent_index;
    records[parent_index].children++;
    seed_id(&records[i].object.id, 0xa7, i);
  }
  return PFS_OK;
}

static enum pfs_status
format_source(void)
{
  struct pfs_build_volume v = {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"},
    .owner = {{5}}, .guarantee_set = true, .guarantee = 0};
  const struct pfs_pool_id pool_id = {{1}};
  const char *paths[] = {source_directory};
  struct host_source source = {0};
  struct pfs_build_plan plan = {0};
  struct pfs_allocation objects = {0};
  enum pfs_status status = host_source_open(&source, device.memory, &v, paths, 1, &pool_id);
  if (status == PFS_OK) {
    status = prepare_manifest(&v);
  }
  struct source_record *records = manifest.data;
  for (size_t i = 0; status == PFS_OK && i < source_objects; i++) {
    records[i].offset = source_bytes;
    if (records[i].object.kind == PFS_OBJECT_DIRECTORY) {
      source_directories++;
      continue;
    }
    source_files++;
    source_blocks += (records[i].object.file_length + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE;
    for (uint64_t done = 0; status == PFS_OK && done < records[i].object.file_length;) {
      size_t length = records[i].object.file_length - done < sizeof(expected) ?
        (size_t)(records[i].object.file_length - done) : sizeof(expected);
      status = host_source_read(&source, 0, records[i].source_index, done, expected, length);
      if (status == PFS_OK && pwrite(fileno(expected_sources.file), expected, length,
          (off_t)(source_bytes + done)) != (ssize_t)length) {
        status = PFS_IO;
      }
      done += length;
    }
    source_bytes += records[i].object.file_length;
  }
  if (status == PFS_OK && ftruncate(fileno(expected_sources.file), (off_t)source_bytes)) {
    status = PFS_IO;
  }
  if (status == PFS_OK) {
    status = pfs_memory_allocate(&expected_sources.memory,
      source_objects * sizeof(struct pfs_build_object), _Alignof(struct pfs_build_object), &objects);
  }
  if (status == PFS_OK) {
    struct pfs_build_object *build_objects = objects.data;
    for (size_t i = 0; i < source_objects; i++) {
      build_objects[i] = records[i].object;
    }
    v.objects = build_objects;
    struct pfs_build_spec spec = {.block_count = RECOVERY_BLOCKS, .pool = pool_id,
      .volume_count = 1, .volumes = &v,
      .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
      .cow_reserve = FORMAT_COW_FLOOR, .migration_reserve = FORMAT_MIGRATION_FLOOR,
      .recovery_reserve = limits.recovery_blocks > FORMAT_RECOVERY_FLOOR ?
        limits.recovery_blocks : FORMAT_RECOVERY_FLOOR};
    status = pfs_build_plan_create(device.memory, &spec, &plan);
    struct source_context context = {&source, records};
    struct pfs_build_source reader = {.context = &context,
      .read = normalized_source_read, .validate = normalized_source_validate};
    if (status == PFS_OK) {
      printf("recovery-workload: seed=%llu image=4GiB N=1 E=8192 M=4096 cap=128MiB "
             "sources=%s objects=%zu files=%llu directories=%llu payload=%llu data-blocks=%llu\n"
             "  profile H=%llu S=%llu Pmax=%llu arena=%llu recovery-min=%llu "
             "reserves=%llu/%llu/%llu log-record-cap=%zu\n",
        (unsigned long long)workload_seed, source_directory, source_objects,
        (unsigned long long)source_files, (unsigned long long)source_directories,
        (unsigned long long)source_bytes, (unsigned long long)source_blocks,
        (unsigned long long)limits.pool_blocks, (unsigned long long)limits.records,
        (unsigned long long)limits.permanent_pool, (unsigned long long)limits.arena_bytes,
        (unsigned long long)limits.recovery_blocks,
        (unsigned long long)plan.pool_root.cow.capacity,
        (unsigned long long)plan.pool_root.migration.capacity,
        (unsigned long long)plan.pool_root.recovery.capacity, device.dirty_capacity);
      fflush(stdout);
      /* The formatter is not a user batch; its complete import cannot fit the
       * H+V+1 volatile log. Its ordinary builder flushes durable fixture storage. */
      status = pfs_build(&plan, &device.backing.builder, &reader);
    }
  }
  if (status != PFS_OK) {
    host_source_error(&source);
  }
  enum pfs_status destroyed = pfs_build_plan_destroy(&plan);
  enum pfs_status closed = host_source_close(&source);
  enum pfs_status released = pfs_memory_free(&expected_sources.memory, &objects);
  return status != PFS_OK ? status : destroyed != PFS_OK ? destroyed :
    closed != PFS_OK ? closed : released;
}

static void
open_writer(void)
{
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  parent = sequential = small = retained = replacement = NULL;
  test_failure_trace_reset(&device);
  struct pfs_write_open_result result;
  enum pfs_status status = pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, &result);
  printf("  writer-open=%s confirmed-generation=%llu required-recovery=%llu "
         "arena=%llu peak-charged-memory=%llu\n", pfs_status_string(status),
    (unsigned long long)result.confirmed_generation,
    (unsigned long long)result.required_recovery_blocks,
    (unsigned long long)result.reserved_arena_bytes,
    (unsigned long long)device.backing.peak_memory_bytes);
  fflush(stdout);
  TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, status, "unexpected workload admission/reopen refusal");
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volume));
}

static void
close_writer(void)
{
  device.observer = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&retained, &(struct pfs_view_close_result){0}));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&replacement, &(struct pfs_view_close_result){0}));
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
  source_objects = source_files = source_directories = source_bytes = source_blocks = 0;
  stats = (struct workload_stats){0};
  manifest = source_paths = (struct pfs_allocation){0};
  workload_failed = healthy_refusal = sequential_created = small_created = false;
  random_counter = 0;
  retained_length = replacement_length = churn_named_length = churn_named_base = 0;
  churn_named = false;
  current_sequential_name = sequential_name;
  options.random = workload_random;
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_sources, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_sequential, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_small, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&expected_churn, PFS_POOL_BLOCKS_MIN, 0));
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_sequential.file), 0));
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_small.file), 0));
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_churn.file), 0));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(RECOVERY_BLOCKS, options.extent_limit,
    options.metadata_limit, 1, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, RECOVERY_BLOCKS,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, CORE_MEMORY_CAP, NULL));
  TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, format_source(), "source census formatting failed");
  open_writer();
  report_state("imported");
  struct pfs_rights held = {.file = file_rights.file,
    .directory = PFS_DIR_CREATE | PFS_DIR_LOOKUP | PFS_DIR_REMOVE};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volume, &authority, &authority.root,
    PFS_SCOPE_SUBTREE, &held, &parent));
  device.observer = observe;
}

static void
progress(const char *phase, unsigned call, unsigned total)
{
  printf("  progress phase=%s call=%u/%u confirmed=%llu elapsed=%.3fs maps=%zu extents=%llu\n",
    phase, call, total, (unsigned long long)stats.useful_bytes, stats.seconds,
    pool.writer->states[pool.writer->selected].map_count,
    (unsigned long long)pool.writer->states[pool.writer->selected].file_extents);
  fflush(stdout);
}

static void
fill_payload(uint64_t iteration, size_t length, unsigned salt)
{
  for (size_t i = 0; i < length; i++) {
    payload[i] = (uint8_t)(workload_seed + iteration * 17 + i * 29 + (i >> 8) + salt);
  }
}

static bool
rename_sequential(const char *to)
{
  test_failure_trace_reset(&device);
  struct pfs_write_result result = {0};
  double start = now();
  enum pfs_status status = pfs_view_rename(parent, (const uint8_t *)current_sequential_name,
    strlen(current_sequential_name), parent, (const uint8_t *)to, strlen(to), false, &result);
  account_call(start, 0);
  if (!require_complete("rename", status, &result)) {
    return false;
  }
  TEST_ASSERT_TRUE(result.namespace_confirmed);
  current_sequential_name = to;
  return true;
}

static bool
retained_churn(void)
{
  for (unsigned i = 0; i < RETAINED_ROUNDS; i++) {
    struct pfs_object_id old_id, new_id;
    retained_length = replacement_length = 0;
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(expected_churn.file), 0));
    if (!create_file(churn_name, &retained, &old_id)) {
      return false;
    }
    churn_named = true;
    churn_named_id = old_id;
    churn_named_base = churn_named_length = 0;
    fill_payload(i, 2 * PFS_BLOCK_SIZE, 101);
    if (!write_expected(retained, expected_churn.file, 0, 0, 2 * PFS_BLOCK_SIZE) ||
        !remove_file(churn_name)) {
      return false;
    }
    churn_named = false;
    fill_payload(i, 2049, 113);
    if (!write_expected(retained, expected_churn.file, 0, 17, 2049)) {
      return false;
    }
    if (!create_file(churn_name, &replacement, &new_id)) {
      return false;
    }
    churn_named = true;
    churn_named_id = new_id;
    churn_named_length = 0;
    churn_named_base = 2 * PFS_BLOCK_SIZE;
    TEST_ASSERT_TRUE(memcmp(old_id.bytes, new_id.bytes, PFS_ID_SIZE));
    fill_payload(i, PFS_BLOCK_SIZE, 127);
    if (!write_expected(replacement, expected_churn.file, 2 * PFS_BLOCK_SIZE,
        0, PFS_BLOCK_SIZE)) {
      return false;
    }
    compare_view(retained, expected_churn.file, 0, 2 * PFS_BLOCK_SIZE);
    compare_view(replacement, expected_churn.file, 2 * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
    if (!close_file(&retained) || !close_file(&replacement)) {
      return false;
    }
    if (!remove_file(churn_name)) {
      return false;
    }
    churn_named = false;
  }
  return true;
}

static uint64_t
host_length(FILE *file)
{
  struct stat info;
  TEST_ASSERT_EQUAL_INT(0, fstat(fileno(file), &info));
  TEST_ASSERT_TRUE(info.st_size >= 0);
  return (uint64_t)info.st_size;
}

static void
compare_outputs(void)
{
  const struct pfs_rights read = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  struct pfs_view_identity identity;
  if (sequential_created) {
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)current_sequential_name, strlen(current_sequential_name),
      PFS_SCOPE_OBJECT, &read, &sequential));
    struct pfs_view_metadata metadata;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(sequential, &metadata));
    identity = metadata.identity;
    TEST_ASSERT_EQUAL_MEMORY(sequential_id.bytes, identity.object.bytes, PFS_ID_SIZE);
    compare_view(sequential, expected_sequential.file, 0, host_length(expected_sequential.file));
  }
  if (small_created) {
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)small_name, strlen(small_name), PFS_SCOPE_OBJECT, &read, &small));
    struct pfs_view_metadata metadata;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(small, &metadata));
    TEST_ASSERT_EQUAL_MEMORY(small_id.bytes, metadata.identity.object.bytes, PFS_ID_SIZE);
    compare_view(small, expected_small.file, 0, host_length(expected_small.file));
  }
  if (churn_named) {
    struct pfs_view *churn = NULL;
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)churn_name, strlen(churn_name), PFS_SCOPE_OBJECT, &read, &churn));
    struct pfs_view_metadata metadata;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(churn, &metadata));
    TEST_ASSERT_EQUAL_MEMORY(churn_named_id.bytes, metadata.identity.object.bytes, PFS_ID_SIZE);
    compare_view(churn, expected_churn.file, churn_named_base, churn_named_length);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&churn, &(struct pfs_view_close_result){0}));
  }
  struct source_record *records = manifest.data;
  for (size_t i = 0; i < source_objects; i++) {
    struct pfs_view *source = NULL;
    const struct pfs_rights requested = records[i].object.kind == PFS_OBJECT_FILE ? read :
      (struct pfs_rights){.directory = PFS_DIR_METADATA};
    test_failure_trace_reset(&device);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)records[i].path, strlen(records[i].path),
      PFS_SCOPE_OBJECT, &requested, &source));
    struct pfs_view_metadata metadata;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(source, &metadata));
    TEST_ASSERT_EQUAL_MEMORY(records[i].object.id.bytes, metadata.identity.object.bytes, PFS_ID_SIZE);
    TEST_ASSERT_EQUAL(records[i].object.kind, metadata.identity.kind);
    if (records[i].object.kind == PFS_OBJECT_FILE) {
      compare_view(source, expected_sources.file, records[i].offset, records[i].object.file_length);
    } else {
      uint64_t children = records[i].children +
        (!i ? (uint64_t)sequential_created + small_created + churn_named : 0);
      TEST_ASSERT_EQUAL_UINT64(children, metadata.size);
    }
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&source, &(struct pfs_view_close_result){0}));
    if (i && i % 128 == 0) {
      printf("  verified source objects=%zu/%zu\n", i, source_objects);
      fflush(stdout);
    }
  }
  printf("  durable streaming verification: sequential=%llu small=%llu source-files=%llu "
         "source-bytes=%llu identities-and-directory-counts=matched\n",
    (unsigned long long)host_length(expected_sequential.file),
    (unsigned long long)host_length(expected_small.file),
    (unsigned long long)source_files, (unsigned long long)source_bytes);
  fflush(stdout);
}

static void
populated_recovery_history(void)
{
  setup();
  active_phase = "create-outputs";
  if (!(sequential_created = create_file(sequential_name, &sequential, &sequential_id)) ||
      !(small_created = create_file(small_name, &small, &small_id))) {
    goto verify;
  }
  report(active_phase);
  active_phase = "sequential-256KiB";
  for (unsigned i = 0; i < SEQUENTIAL_BYTES / REQUEST_BYTES; i++) {
    fill_payload(i, sizeof(payload), 0);
    if (!write_expected(sequential, expected_sequential.file, 0,
        (uint64_t)i * REQUEST_BYTES, REQUEST_BYTES)) {
      goto verify;
    }
    if ((i + 1) % 128 == 0) {
      progress(active_phase, i + 1, (unsigned)(SEQUENTIAL_BYTES / REQUEST_BYTES));
    }
  }
  report(active_phase);
  active_phase = "independent-4KiB-appends";
  for (unsigned i = 0; i < SMALL_APPENDS; i++) {
    fill_payload(i, PFS_BLOCK_SIZE, 37);
    if (!write_expected(small, expected_small.file, 0, (uint64_t)i * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE)) {
      goto verify;
    }
    if ((i + 1) % 128 == 0) {
      progress(active_phase, i + 1, SMALL_APPENDS);
    }
  }
  report(active_phase);
  active_phase = "overwrite-and-rename";
  uint64_t random = workload_seed;
  for (unsigned i = 0; i < OVERWRITES; i++) {
    random = random * UINT64_C(6364136223846793005) + 1;
    uint64_t offset = random % (SEQUENTIAL_BYTES - 8193u);
    fill_payload(i, 8193, 73);
    if (!write_expected(sequential, expected_sequential.file, 0, offset, 8193) ||
        !rename_sequential(i % 2 ? sequential_name : renamed_name)) {
      goto verify;
    }
  }
  report(active_phase);
  active_phase = "retained-unlink-name-reuse";
  if (!retained_churn()) {
    goto verify;
  }
  report(active_phase);
verify:
  if (workload_failed) {
    report("failed-phase-prefix");
    if (!healthy_refusal) {
      TEST_FAIL_MESSAGE("unexpected non-ready workload failure; no safe expected-state reopen claimed");
    }
    /* If a churn call refuses before publication, verify held orphan contents
     * before last-reference cleanup can legitimately erase them. */
    if (retained) {
      compare_view(retained, expected_churn.file, 0, retained_length);
    }
    if (replacement) {
      compare_view(replacement, expected_churn.file, 8192, replacement_length);
    }
  }
  close_writer();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
  open_writer();
  compare_outputs();
  const struct pfs_admit_volume *v = &pool.writer->states[pool.writer->selected].volumes[0];
  if (!workload_failed) {
    TEST_ASSERT_EQUAL_UINT64(source_objects + 2, v->record.object_count);
    TEST_ASSERT_EQUAL_UINT64(source_directories, v->directories);
    TEST_ASSERT_EQUAL_UINT64(0, v->orphan_work);
  }
  report_state("durable-reopen-verified");
  close_writer();
  struct pfs_check_result checked;
  test_failure_trace_reset(&device);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&device.backing.builder.reader, device.memory, NULL, NULL, &checked));
  TEST_ASSERT_TRUE(checked.state[0].complete);
  TEST_ASSERT_TRUE(checked.state[1].complete);
  TEST_ASSERT_TRUE(checked.cross_complete);
  printf("  both retained states and cross-state checker: complete, success; peak-charged-memory=%llu\n",
    (unsigned long long)device.backing.peak_memory_bytes);
  fflush(stdout);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_free(&expected_sources.memory, &source_paths));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_free(&expected_sources.memory, &manifest));
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_sources));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_sequential));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_small));
  TEST_ASSERT_TRUE(test_fixture_close(&expected_churn));
  TEST_ASSERT_FALSE_MESSAGE(workload_failed,
    "planned recovery history refused; confirmed prefix verified without reducing workload/profile");
}

void
run_recovery_workload(uint64_t seed, const char *source_path)
{
  Unity.TestFile = __FILE__;
  workload_seed = seed;
  source_directory = source_path;
  RUN_TEST(populated_recovery_history);
}
