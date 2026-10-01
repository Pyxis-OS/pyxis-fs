/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#include "failure.h"
#include "ram_guard.h"
#include "writer.h"
#include "unity.h"
#include <pyxis_fs/build.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IMAGE_BLOCKS (UINT64_C(1024) * 1024 * 1024 / PFS_BLOCK_SIZE)
#define POPULATION_MAX 256u
#define FILES_MAX (POPULATION_MAX + 2u)
#define PATCHES_MAX 80u
#define REQUEST_MAX (256u * 1024u)
#define CORE_MEMORY_MAX (128u * 1024u * 1024u)

struct patch {
  uint64_t offset;
  size_t length;
  unsigned tag;
};

/* Independent application history, with no core metadata or read-back inputs. */
struct oracle_file {
  char name[32];
  uint64_t length;
  size_t patch_count;
  struct patch patches[PATCHES_MAX];
  bool present;
};

struct comparison_file {
  int fd;
  struct pfs_view *view;
  struct oracle_file *oracle;
};

struct callback_stats {
  uint64_t data_blocks, metadata_blocks, flushes;
};

struct phase_stats {
  uint64_t operations, application_bytes, closes, barriers;
  struct callback_stats user, orphan, drain;
};

static struct test_failure device;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *parent;
static struct oracle_file files[FILES_MAX];
static struct phase_stats preparation, measurement;
static struct phase_stats *phase = &preparation;
static uint8_t payload[REQUEST_MAX], actual[REQUEST_MAX];
static int root_fd = -1;
static bool native, batch, callback_failed, phase_stops, verifying;
static unsigned population;
static const char *case_name, *root_path;
static struct callback_stats *callback_phase;
static const struct pfs_write_options options = {
  .extent_limit = 8192, .metadata_limit = 4096, .random = test_random,
};
static const struct pfs_trusted_context authority = {
  .principal = {{5}}, .root = {{3}}, .scope = PFS_SCOPE_SUBTREE,
  .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL},
};
static const struct pfs_rights file_rights = {
  .file = PFS_FILE_READ | PFS_FILE_WRITE | PFS_FILE_RESIZE | PFS_FILE_METADATA |
    PFS_FILE_CHECKPOINT,
};
static const struct pfs_rights read_rights = {
  .file = PFS_FILE_READ | PFS_FILE_METADATA,
};

/* The reused fixture cleanup helpers link Unity; this executable uses explicit
 * checks and never enters the Unity test runner. */
void setUp(void);
void tearDown(void);
void
setUp(void)
{
}

void
tearDown(void)
{
}

static void
require(bool success, const char *message)
{
  if (!success) {
    fprintf(stderr, "comparison: %s (errno=%d)\n", message, errno);
    exit(EXIT_FAILURE);
  }
}

static void
require_status(enum pfs_status status, const char *message)
{
  if (status != PFS_OK) {
    fprintf(stderr, "comparison: %s (pfs_status=%d)\n", message, status);
    exit(EXIT_FAILURE);
  }
}

static void
require_result(const struct pfs_write_result *result)
{
  require(result->completion == PFS_COMPLETE && result->health == PFS_WRITER_READY &&
    result->maintenance_status == PFS_OK, "incomplete operation or maintenance");
}

static double
now(void)
{
  struct timespec value;
  require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "clock_gettime");
  return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void
reset_trace(void)
{
  if (!native) {
    test_failure_trace_reset(&device);
  }
}

static void
observe(struct test_failure *adapter, const struct test_failure_event *event,
        bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (before) {
    callback_phase = pool.writer->status.drain_pending ? &phase->drain :
      pool.writer->batch.orphan_cleanup ? &phase->orphan : &phase->user;
    return;
  }
  callback_failed |= event->status != PFS_OK;
  if (event->kind == TEST_FAILURE_FLUSH) {
    callback_phase->flushes++;
  }
  if (event->kind != TEST_FAILURE_WRITE) {
    return;
  }
  for (uint64_t block = event->first; block < event->first + event->count; block++) {
    bool data = false;
    if (callback_phase != &phase->drain) {
      const struct pfs_batch *writer_batch = &pool.writer->batch;
      for (size_t i = 0; i < writer_batch->add_count; i++) {
        const struct check_claim *claim = &writer_batch->add[i];
        data |= !claim->type && block >= claim->first && block - claim->first < claim->count;
      }
    }
    callback_phase->data_blocks += data;
    callback_phase->metadata_blocks += !data;
  }
}

static void
sync_native(void)
{
  require(fsync(root_fd) == 0 && syncfs(root_fd) == 0, "final filesystem sync");
  phase->barriers++;
}

/* A logical operation is one create, write, rename or remove call. Close and
 * barriers are reported separately. Native batch flushes after each 16 such
 * operations; the final boundary also flushes an incomplete batch. */
static void
completed_operation(void)
{
  phase->operations++;
  if (native && batch && phase->operations % 16 == 0) {
    require(syncfs(root_fd) == 0, "batch filesystem sync");
    phase->barriers++;
  }
}

static void
close_file(struct comparison_file *handle)
{
  if (native) {
    require(close(handle->fd) == 0, "close file");
    handle->fd = -1;
  } else {
    reset_trace();
    struct pfs_view_close_result result;
    require_status(pfs_view_close(&handle->view, &result), "close core file");
    require(result.released, "core close did not release view");
  }
  phase->closes++;
}

static struct comparison_file
create_file(struct oracle_file *oracle, const char *name)
{
  require(strlen(name) < sizeof(oracle->name), "file name too long");
  *oracle = (struct oracle_file){.present = true};
  strcpy(oracle->name, name);
  struct comparison_file handle = {.fd = -1, .oracle = oracle};
  if (native) {
    handle.fd = openat(root_fd, name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(handle.fd >= 0, "create native file");
    if (!batch) {
      require(fsync(handle.fd) == 0 && fsync(root_fd) == 0, "sync creation and parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    struct pfs_view_identity identity;
    require_status(pfs_view_create_file(parent, (const uint8_t *)name, strlen(name),
      &file_rights, &handle.view, &identity, &result), "create core file");
    require_result(&result);
    require(result.namespace_confirmed, "unconfirmed creation");
  }
  completed_operation();
  return handle;
}

static struct comparison_file
open_file(struct oracle_file *oracle)
{
  struct comparison_file handle = {.fd = -1, .oracle = oracle};
  if (native) {
    handle.fd = openat(root_fd, oracle->name, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    require(handle.fd >= 0, "open native file");
  } else {
    reset_trace();
    require_status(pfs_view_acquire_path(&volume, &authority,
      (const uint8_t *)oracle->name, strlen(oracle->name), PFS_SCOPE_OBJECT,
      verifying ? &read_rights : &file_rights, &handle.view), "open core file");
  }
  return handle;
}

static void
write_file(struct comparison_file *handle, uint64_t offset, size_t length, unsigned tag)
{
  require(length <= sizeof(payload), "oversized request");
  for (size_t i = 0; i < length; i++) {
    payload[i] = (uint8_t)(1u + tag * 31u + i * 7u + (i >> 8));
  }
  if (native) {
    size_t done = 0;
    while (done < length) {
      ssize_t count = pwrite(handle->fd, payload + done, length - done, (off_t)(offset + done));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      require(count > 0, "write native file");
      done += (size_t)count;
    }
    if (!batch) {
      require(fsync(handle->fd) == 0, "sync written file");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    require_status(pfs_view_write(handle->view, offset, payload, length, &result), "write core file");
    require_result(&result);
    require(result.confirmed_bytes == length, "short core write");
  }
  struct oracle_file *oracle = handle->oracle;
  require(oracle->patch_count < PATCHES_MAX, "oracle history overflow");
  oracle->patches[oracle->patch_count++] = (struct patch){offset, length, tag};
  if (offset + length > oracle->length) {
    oracle->length = offset + length;
  }
  phase->application_bytes += length;
  completed_operation();
}

static void
rename_file(struct oracle_file *source, const char *name, struct oracle_file *victim)
{
  if (native) {
    require(renameat(root_fd, source->name, root_fd, name) == 0, "rename native file");
    if (!batch) {
      /* Both affected parents are the same held root in this bounded case. */
      require(fsync(root_fd) == 0, "sync renamed parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    require_status(pfs_view_rename(parent, (const uint8_t *)source->name, strlen(source->name),
      parent, (const uint8_t *)name, strlen(name), victim != NULL, &result), "rename core file");
    require_result(&result);
    require(result.namespace_confirmed, "unconfirmed rename");
  }
  strcpy(source->name, name);
  if (victim) {
    victim->present = false;
  }
  completed_operation();
}

static void
remove_file(struct oracle_file *oracle)
{
  if (native) {
    require(unlinkat(root_fd, oracle->name, 0) == 0, "remove native file");
    if (!batch) {
      require(fsync(root_fd) == 0, "sync removed parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    require_status(pfs_view_remove(parent, (const uint8_t *)oracle->name,
      strlen(oracle->name), &result), "remove core file");
    require_result(&result);
    require(result.namespace_confirmed, "unconfirmed removal");
  }
  oracle->present = false;
  completed_operation();
}

static void
open_backend(void)
{
  root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  require(root_fd >= 0, "open supplied root");
  if (native) {
    DIR *directory = fdopendir(dup(root_fd));
    require(directory != NULL, "inspect native root");
    struct dirent *entry;
    rewinddir(directory);
    errno = 0;
    while ((entry = readdir(directory))) {
      require(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."), "native root must be empty");
    }
    require(errno == 0 && closedir(directory) == 0, "read native root");
    return;
  }
  struct stat supplied, validated;
  require(fstat(root_fd, &supplied) == 0 &&
    fstat(pfs_test_ram_directory_fd(), &validated) == 0 &&
    supplied.st_dev == validated.st_dev && supplied.st_ino == validated.st_ino,
    "Pyxis root must be the validated TMPDIR");
  struct pfs_plan_limits limits;
  require_status(pfs_plan_limits(IMAGE_BLOCKS, options.extent_limit,
    options.metadata_limit, 1, &limits), "plan limits");
  require_status(test_failure_open(&device, IMAGE_BLOCKS,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, CORE_MEMORY_MAX, NULL), "open adapter");
  const struct pfs_build_object object = {
    .id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY,
  };
  const struct pfs_build_volume build_volume = {
    .id = {{2}}, .root_object = {{3}}, .name = {4, "home"}, .owner = {{5}},
    .guarantee_set = true, .object_count = 1, .objects = &object,
  };
  const struct pfs_build_spec spec = {
    .block_count = IMAGE_BLOCKS, .pool = {{1}}, .volume_count = 1, .volumes = &build_volume,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 8192, .migration_reserve = 8192, .recovery_reserve = 8192,
  };
  struct pfs_build_plan plan = {0};
  require_status(pfs_build_plan_create(device.memory, &spec, &plan), "create image plan");
  require_status(pfs_build(&plan, &device.builder, NULL), "build image");
  require_status(pfs_build_plan_destroy(&plan), "destroy image plan");
  reset_trace();
  struct pfs_write_open_result opened;
  require_status(pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, &opened),
    "open writer");
  const struct pfs_volume_id id = {{2}};
  require_status(pfs_pool_volume_open(&pool, &id, &volume), "open volume");
  const struct pfs_rights rights = {
    .file = file_rights.file,
    .directory = PFS_DIR_METADATA | PFS_DIR_LIST | PFS_DIR_LOOKUP |
      PFS_DIR_CREATE | PFS_DIR_REMOVE | PFS_DIR_REPLACE,
  };
  reset_trace();
  require_status(pfs_view_acquire(&volume, &authority, &authority.root,
    PFS_SCOPE_SUBTREE, &rights, &parent), "acquire root");
  device.observer = observe;
}

static void
final_boundary(void)
{
  if (native) {
    sync_native();
  } else {
    /* The formatter's existing grant includes file checkpoint, not directory
     * checkpoint. Exercise that held authority without widening the grant. */
    struct comparison_file handle = open_file(&files[0]);
    reset_trace();
    struct pfs_write_result result;
    require_status(pfs_view_checkpoint(handle.view, &result), "checkpoint");
    require_result(&result);
    close_file(&handle);
    phase->barriers++;
  }
}

static void
prepare(void)
{
  for (unsigned i = 0; i < population; i++) {
    char name[32];
    snprintf(name, sizeof(name), "pop-%03u", i);
    struct comparison_file handle = create_file(&files[i], name);
    write_file(&handle, 0, PFS_BLOCK_SIZE, 1000u + i);
    close_file(&handle);
  }
  /* A fixed overwrite/replacement history precedes every measured case. */
  for (unsigned i = 0; i < 8; i++) {
    struct comparison_file handle = open_file(&files[i]);
    write_file(&handle, 1024, 1024, 2000u + i);
    close_file(&handle);
  }
  for (unsigned i = 0; i < 4; i++) {
    struct oracle_file *temporary = &files[POPULATION_MAX + 1];
    struct comparison_file handle = create_file(temporary, "prehistory.tmp");
    write_file(&handle, 0, PFS_BLOCK_SIZE, 3000u + i);
    close_file(&handle);
    char destination[32];
    strcpy(destination, files[i].name);
    rename_file(temporary, destination, &files[i]);
    files[i] = *temporary;
    temporary->present = false;
  }
  if (strcmp(case_name, "compiler")) {
    struct comparison_file handle = create_file(&files[POPULATION_MAX], "work");
    if (!strcmp(case_name, "overwrite")) {
      write_file(&handle, 0, REQUEST_MAX, 4000);
    }
    close_file(&handle);
  }
  final_boundary();
}

static void
run_case(void)
{
  if (!strcmp(case_name, "compiler")) {
    for (unsigned i = 0; i < 16; i++) {
      struct oracle_file *temporary = &files[POPULATION_MAX];
      struct comparison_file handle = create_file(temporary, "compiler.tmp");
      write_file(&handle, 0, PFS_BLOCK_SIZE, 5000u + i);
      close_file(&handle);
      rename_file(temporary, "compiler.o", NULL);
      remove_file(temporary);
    }
    return;
  }
  struct comparison_file handle = open_file(&files[POPULATION_MAX]);
  unsigned count = !strcmp(case_name, "small") ? 64 : !strcmp(case_name, "large") ? 16 : 32;
  size_t length = !strcmp(case_name, "small") ? PFS_BLOCK_SIZE :
    !strcmp(case_name, "large") ? REQUEST_MAX : 1024;
  uint32_t random = 1;
  for (unsigned i = 0; i < count; i++) {
    uint64_t offset = (uint64_t)i * length;
    if (!strcmp(case_name, "overwrite")) {
      random = random * 1664525u + 1013904223u;
      offset = (i % 2) ? (uint64_t)(random % 63u) * PFS_BLOCK_SIZE + 3584 :
        random % (REQUEST_MAX - 1024u + 1u);
    }
    write_file(&handle, offset, length, 6000u + i);
  }
  close_file(&handle);
}

static uint8_t
expected_byte(const struct oracle_file *oracle, uint64_t offset)
{
  for (size_t i = oracle->patch_count; i; i--) {
    const struct patch *patch = &oracle->patches[i - 1];
    if (offset >= patch->offset && offset - patch->offset < patch->length) {
      uint64_t index = offset - patch->offset;
      return (uint8_t)(1 + (uint64_t)patch->tag * 31 + index * 7 + index / 256);
    }
  }
  return 0;
}

static void
verify(uint64_t *verified_files, uint64_t *verified_bytes)
{
  for (size_t i = 0; i < FILES_MAX; i++) {
    struct oracle_file *oracle = &files[i];
    if (!oracle->present) {
      continue;
    }
    struct comparison_file handle = open_file(oracle);
    uint64_t length;
    if (native) {
      struct stat metadata;
      require(fstat(handle.fd, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_size >= 0,
        "inspect verified file");
      length = (uint64_t)metadata.st_size;
    } else {
      reset_trace();
      struct pfs_view_metadata metadata;
      require_status(pfs_view_metadata(handle.view, &metadata), "inspect verified core file");
      require(metadata.identity.kind == PFS_OBJECT_FILE, "verified object is not a file");
      length = metadata.size;
    }
    require(length == oracle->length, "oracle file length mismatch");
    for (uint64_t offset = 0; offset < length;) {
      size_t count = length - offset < sizeof(actual) ? (size_t)(length - offset) : sizeof(actual);
      if (native) {
        require(pread(handle.fd, actual, count, (off_t)offset) == (ssize_t)count, "read verified file");
      } else {
        size_t got;
        reset_trace();
        require_status(pfs_view_read(handle.view, offset, actual, count, &got), "read verified core file");
        require(got == count, "short verified core read");
      }
      for (size_t j = 0; j < count; j++) {
        require(actual[j] == expected_byte(oracle, offset + j), "independent content oracle mismatch");
      }
      offset += count;
    }
    close_file(&handle);
    (*verified_files)++;
    *verified_bytes += length;
  }
  uint64_t entries = 0;
  if (native) {
    DIR *directory = fdopendir(dup(root_fd));
    require(directory != NULL, "open final directory");
    struct dirent *entry;
    rewinddir(directory);
    errno = 0;
    while ((entry = readdir(directory))) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
        entries++;
      }
    }
    require(errno == 0 && closedir(directory) == 0, "read final directory");
  } else {
    reset_trace();
    struct pfs_view_metadata metadata;
    require_status(pfs_view_metadata(parent, &metadata), "inspect final directory");
    entries = metadata.size;
  }
  require(entries == *verified_files, "unexpected final namespace entries");
}

static void
close_core(void)
{
  if (!native) {
    reset_trace();
    struct pfs_view_close_result result;
    require_status(pfs_view_close(&parent, &result), "close root view");
    require_status(pfs_volume_close(&volume), "close volume");
    require_status(pfs_pool_close(&pool), "close pool");
  }
}

static void
reopen_verification(void)
{
  verifying = true;
  if (!native) {
    device.observer = NULL;
    reset_trace();
    struct pfs_pool_diagnostic diagnostic;
    require_status(pfs_pool_open(&pool, &device.builder.reader, device.memory, &diagnostic),
      "reopen read-only pool");
    const struct pfs_volume_id id = {{2}};
    require_status(pfs_pool_diagnostic_volume_open(&pool, &id, &volume), "reopen read-only volume");
    const struct pfs_rights rights = {
      .directory = PFS_DIR_METADATA | PFS_DIR_LIST | PFS_DIR_LOOKUP,
      .file = PFS_FILE_METADATA | PFS_FILE_READ,
    };
    reset_trace();
    require_status(pfs_view_acquire(&volume, &authority, &authority.root,
      PFS_SCOPE_SUBTREE, &rights, &parent), "reacquire read-only root");
  }
}

static void
phase_marker(const char *name)
{
  printf("{\"phase\":\"%s\"}\n", name);
  require(fflush(stdout) == 0, "flush phase marker");
  if (phase_stops) {
    require(raise(SIGSTOP) == 0, "stop at phase boundary");
  }
}

static void
print_phase(const char *name, const struct phase_stats *stats)
{
  printf("\"%s\":{\"operations\":%" PRIu64 ",\"application_bytes\":%" PRIu64
    ",\"closes\":%" PRIu64 ",\"barriers\":%" PRIu64, name, stats->operations,
    stats->application_bytes, stats->closes, stats->barriers);
  if (native) {
    printf(",\"core_callbacks\":null}");
    return;
  }
  const struct callback_stats *groups[] = {&stats->user, &stats->orphan, &stats->drain};
  const char *names[] = {"user", "orphan", "drain"};
  for (size_t i = 0; i < 3; i++) {
    printf(",\"%s\":{\"data_blocks\":%" PRIu64 ",\"metadata_blocks\":%" PRIu64
      ",\"flushes\":%" PRIu64 "}", names[i], groups[i]->data_blocks,
      groups[i]->metadata_blocks, groups[i]->flushes);
  }
  printf("}");
}

static void
parse_arguments(int argc, char **argv)
{
  const char *filesystem = NULL, *durability = NULL, *population_text = NULL, *stops = NULL;
  require(argc == 11 || argc == 13,
    "expected --filesystem --root --population --case --durability and optional --phase-stops pairs");
  for (int i = 1; i < argc; i += 2) {
    const char **destination = NULL;
    if (!strcmp(argv[i], "--filesystem")) {
      destination = &filesystem;
    } else if (!strcmp(argv[i], "--root")) {
      destination = &root_path;
    } else if (!strcmp(argv[i], "--population")) {
      destination = &population_text;
    } else if (!strcmp(argv[i], "--case")) {
      destination = &case_name;
    } else if (!strcmp(argv[i], "--durability")) {
      destination = &durability;
    } else if (!strcmp(argv[i], "--phase-stops")) {
      destination = &stops;
    }
    require(destination && !*destination, "unknown or repeated option");
    *destination = argv[i + 1];
  }
  require(filesystem && root_path && population_text && case_name && durability, "missing option");
  require(!strcmp(filesystem, "pyxis") || !strcmp(filesystem, "native"), "unknown filesystem");
  require(!strcmp(population_text, "32") || !strcmp(population_text, "256"), "population must be 32 or 256");
  require(!strcmp(case_name, "small") || !strcmp(case_name, "large") ||
    !strcmp(case_name, "overwrite") || !strcmp(case_name, "compiler"), "unknown case");
  require(!strcmp(durability, "operation") || !strcmp(durability, "batch"), "unknown durability");
  require(!stops || !strcmp(stops, "yes") || !strcmp(stops, "no"), "phase stops must be yes or no");
  phase_stops = stops && !strcmp(stops, "yes");
  native = !strcmp(filesystem, "native");
  batch = !strcmp(durability, "batch");
  population = !strcmp(population_text, "32") ? 32 : 256;
  require(native || !batch, "Pyxis has operation durability only");
}

int
main(int argc, char **argv)
{
  pfs_test_require_ram();
  parse_arguments(argc, argv);
  open_backend();
  prepare();
  phase = &measurement;
  phase_marker("measurement_begin");
  double start = now();
  run_case();
  final_boundary();
  uint64_t extents = 0, metadata = 0;
  size_t map_records = 0;
  if (!native) {
    const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
    extents = state->volumes[0].file_extents;
    metadata = state->volumes[0].metadata_blocks;
    map_records = state->map_count;
  }
  close_core();
  double elapsed = now() - start;
  phase_marker("measurement_end");
  /* Verification does not change the measured close/callback counters. */
  struct phase_stats verification = {0};
  phase = &verification;
  reopen_verification();
  uint64_t verified_files = 0, verified_bytes = 0;
  verify(&verified_files, &verified_bytes);
  close_core();
  if (!native) {
    require(!callback_failed && !device.infrastructure_failure, "callback observation failure");
    require(test_failure_close(&device), "close adapter");
  }
  require(close(root_fd) == 0, "close root directory");
  printf("{\"filesystem\":\"%s\",\"population\":%u,\"case\":\"%s\","
    "\"durability\":\"%s\",\"seed\":1,\"geometry_bytes\":1073741824,"
    "\"block_bytes\":4096,\"batch_unit\":%s,\"elapsed_seconds\":%.9f,",
    native ? "native" : "pyxis", population, case_name, batch ? "batch" : "operation",
    batch ? "\"16 create/write/rename/remove calls\"" : "null", elapsed);
  if (!native) {
    printf("\"extent_limit\":8192,\"metadata_limit\":4096,");
  }
  print_phase("preparation", &preparation);
  printf(",");
  print_phase("measurement", &measurement);
  if (!native) {
    printf(",\"selected_state\":{\"extents\":%" PRIu64 ",\"metadata_blocks\":%" PRIu64
      ",\"map_records\":%zu}", extents, metadata, map_records);
  }
  printf(",\"verification\":{\"oracle\":\"seed1-operation-ledger\",\"contents_verified\":true,"
    "\"files\":%" PRIu64 ",\"bytes\":%" PRIu64 "}}\n", verified_files, verified_bytes);
  return EXIT_SUCCESS;
}
