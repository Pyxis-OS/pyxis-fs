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
#include <limits.h>
#include <linux/btrfs.h>
#include <linux/loop.h>
#include <linux/magic.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define IMAGE_BLOCKS (UINT64_C(1024) * 1024 * 1024 / PFS_BLOCK_SIZE)
#define REQUEST_MAX (256u * 1024u)
#define CORE_MEMORY_MAX (128u * 1024u * 1024u)
#define MOUNT_LINE_BYTES 16384u
#define EXT4_SUPERBLOCK_OFFSET 1024u
#define EXT4_JOURNAL_UUID_OFFSET 0xd0u
#define EXT4_JOURNAL_UUID_BYTES 16u
#define EXT4_JOURNAL_INODE_BYTES 4u
#define EXT4_JOURNAL_DEVICE_BYTES 4u

/* Independent application history, with no core metadata or read-back inputs. */
struct oracle_file {
  char name[32];
  uint64_t length;
  size_t capacity;
  uint8_t *bytes;
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

/* Bounded aggregates owned by one workload phase. Timing is
 * the observed first publisher read through first replacement-write interval;
 * it includes adapter reads and optional reference counting, not an NVMe model. */
struct map_stats {
  uint64_t publications, local, bulk, source_sum, source_max, closure_sum, closure_max;
  uint64_t passes, largest_addition, redistribution, retired, emitted, bulk_reference;
  struct pfs_incremental_repair_metrics repair;
  uint64_t split_opportunities, split_trials, split_selected, closure_evaluations;
  uint64_t split_discarded_nodes, split_discarded_growth;
  uint64_t split_skips[4], split_misses[8];
  uint64_t local_emitted, bulk_emitted;
  uint64_t fallback[4], local_cost[3], failed_leaves, failed_records;
  int64_t local_node_difference;
  double planning_seconds, planning_max;
  double local_planning_seconds, bulk_planning_seconds;
};
struct phase_stats {
  uint64_t operations, application_bytes, closes, barriers;
  uint64_t reads, planning_reads;
  uint64_t maximum_boundary_volume_debt, final_volume_debt, final_pool_debt;
  uint64_t initial_flush_ordinal, slot_writes;
  struct callback_stats user, orphan, drain;
  struct map_stats maps;
  char name[32];
  double elapsed_seconds;
  uint64_t extents, metadata_blocks, map_records, map_nodes, live_blocks, reusable_blocks, generation;
};

static double planning_started;
static uint64_t planning_generation, observed_generation;
static struct phase_stats *planning_phase;

static struct test_failure device;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *parent;
static struct oracle_file *files;
static size_t file_count;
static struct phase_stats preparation, measurement;
static struct phase_stats *sustained_phases;
static size_t sustained_phase_count;
static struct phase_stats *phase = &preparation;
static uint8_t payload[REQUEST_MAX], actual[REQUEST_MAX];
static int root_fd = -1;
static int native_root_fd = -1, native_loop_fd = -1, native_backing_fd = -1;
static bool native, batch, callback_failed, phase_stops, verifying, sustained, incomplete;
static unsigned population;
static unsigned background_blocks, window_count, operations_per_window, cycles_per_window;
static unsigned target_bytes, overwrite_bytes, seed;
static enum pfs_status refusal_status;
static unsigned refusal_errno;
static uint64_t refusal_bytes;
static bool refusal_namespace;
static const char *refusal_phase, *refusal_operation;
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
sample_debt(void)
{
  if (native || !pool.writer) {
    return;
  }
  const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
  phase->final_volume_debt = state->retired_volume;
  phase->final_pool_debt = state->retired_pool;
  if (state->retired_volume > phase->maximum_boundary_volume_debt) {
    phase->maximum_boundary_volume_debt = state->retired_volume;
  }
}

static void
require_result(const struct pfs_write_result *result)
{
  require(result->completion == PFS_COMPLETE && result->health == PFS_WRITER_READY &&
    result->maintenance_status == PFS_OK && result->operation_status == PFS_OK &&
    (result->maintenance_completion == PFS_MAINTENANCE_NONE ||
     result->maintenance_completion == PFS_MAINTENANCE_PENDING ||
     result->maintenance_completion == PFS_MAINTENANCE_COMPLETE),
    "incomplete operation or maintenance");
}

static bool
accept_operation(enum pfs_status status, const struct pfs_write_result *result,
                 const char *operation)
{
  if (status == PFS_OK) {
    require_result(result);
    return true;
  }
  bool capacity = status == PFS_QUOTA || status == PFS_NO_SPACE || status == PFS_LIMIT;
  if (sustained && capacity && result->health == PFS_WRITER_READY &&
      result->maintenance_status == PFS_OK && result->operation_status == status &&
      result->completion == PFS_STOPPED) {
    incomplete = true;
    refusal_status = status;
    refusal_bytes = result->confirmed_bytes;
    refusal_namespace = result->namespace_confirmed;
    refusal_phase = phase->name;
    refusal_operation = operation;
    planning_generation = 0;
    planning_phase = NULL;
    return false;
  }
  fprintf(stderr, "comparison: %s failed (status=%s, completion=%u, health=%u, "
    "confirmed_bytes=%" PRIu64 ", namespace_confirmed=%u, maintenance=%u, maintenance_status=%s)\n",
    operation, pfs_status_string(status), (unsigned)result->completion, (unsigned)result->health,
    result->confirmed_bytes, result->namespace_confirmed, (unsigned)result->maintenance_completion,
    pfs_status_string(result->maintenance_status));
  exit(EXIT_FAILURE);
}

static bool
native_refusal(int error, const char *operation, uint64_t confirmed)
{
  if (sustained && (error == ENOSPC || error == EDQUOT)) {
    incomplete = true;
    refusal_errno = (unsigned)error;
    refusal_bytes = confirmed;
    refusal_phase = phase->name;
    refusal_operation = operation;
    return true;
  }
  return false;
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
observe_plan(const struct test_failure_event *event)
{
  const struct pfs_writer *writer = pool.writer;
  if (!writer || !writer->collect_map_metrics) {
    return;
  }
  const struct pfs_map_metrics *metrics = &writer->map_metrics;
  if (event->kind == TEST_FAILURE_READ && writer->map_planning &&
      planning_generation != metrics->generation) {
    planning_generation = metrics->generation;
    planning_started = now();
    planning_phase = phase;
  }
  if (event->kind != TEST_FAILURE_WRITE || observed_generation == metrics->generation) {
    return;
  }
  require(!writer->map_planning && planning_generation == metrics->generation &&
    planning_phase == phase,
    "publication timing boundary missing");
  observed_generation = metrics->generation;
  double elapsed = now() - planning_started;
  struct map_stats *stats = &phase->maps;
  stats->publications++;
  stats->local += metrics->local;
  stats->bulk += !metrics->local;
  stats->source_sum += metrics->source_nodes;
  stats->closure_sum += metrics->closure_nodes;
  if (metrics->source_nodes > stats->source_max) {
    stats->source_max = metrics->source_nodes;
  }
  if (metrics->closure_nodes > stats->closure_max) {
    stats->closure_max = metrics->closure_nodes;
  }
  stats->passes += metrics->growth_passes;
  if (metrics->largest_addition > stats->largest_addition) {
    stats->largest_addition = metrics->largest_addition;
  }
  stats->redistribution += metrics->redistribution_additions;
  stats->repair.underflow += metrics->repair.underflow;
  stats->repair.overflow += metrics->repair.overflow;
  stats->repair.compared += metrics->repair.compared;
  stats->repair.right_preferred += metrics->repair.right_preferred;
  stats->repair.ties += metrics->repair.ties;
  stats->repair.fitting += metrics->repair.fitting;
  stats->repair.left_bridges += metrics->repair.left_bridges;
  stats->repair.right_bridges += metrics->repair.right_bridges;
  stats->repair.left_deficit_sum += metrics->repair.left_deficit_sum;
  stats->repair.right_deficit_sum += metrics->repair.right_deficit_sum;
  stats->repair.chosen_deficit_sum += metrics->repair.chosen_deficit_sum;
  stats->split_opportunities += metrics->split.opportunities;
  stats->split_trials += metrics->split.trials;
  stats->split_selected += metrics->split_selected;
  stats->closure_evaluations += metrics->split.closure_evaluations;
  stats->split_discarded_nodes += metrics->split.discarded_nodes;
  stats->split_discarded_growth += metrics->split.discarded_growth;
  for (size_t i = 0; i < 4; i++) {
    stats->split_skips[i] += (metrics->split.skips & (1u << i)) != 0;
  }
  for (size_t i = 0; i < 8; i++) {
    stats->split_misses[i] += (metrics->split.misses & (1u << i)) != 0;
  }
  stats->emitted += metrics->replacement_nodes;
  stats->retired += metrics->local ? metrics->closure_nodes : metrics->source_nodes;
  if (metrics->local) {
    stats->local_emitted += metrics->replacement_nodes;
    stats->local_planning_seconds += elapsed;
  } else {
    stats->bulk_emitted += metrics->replacement_nodes;
    stats->bulk_planning_seconds += elapsed;
  }
  stats->bulk_reference += metrics->bulk_reference_nodes;
  for (size_t i = 0; i < 4; i++) {
    stats->fallback[i] += (metrics->fallback & (1u << i)) != 0;
  }
  if (metrics->failed_run_leaves) {
    stats->failed_leaves = metrics->failed_run_leaves;
    stats->failed_records = metrics->failed_run_records;
  }
  if (metrics->local) {
    size_t cost = metrics->replacement_nodes < metrics->bulk_reference_nodes ? 0 :
      metrics->replacement_nodes == metrics->bulk_reference_nodes ? 1 : 2;
    stats->local_cost[cost]++;
    stats->local_node_difference += (int64_t)metrics->bulk_reference_nodes -
                                  (int64_t)metrics->replacement_nodes;
  }
  stats->planning_seconds += elapsed;
  if (elapsed > stats->planning_max) {
    stats->planning_max = elapsed;
  }
}

static void
observe(struct test_failure *adapter, const struct test_failure_event *event,
        bool before, void *context)
{
  (void)adapter;
  (void)context;
  if (before) {
    observe_plan(event);
    const struct pfs_batch *active = pool.writer ? pool.writer->active_batch : NULL;
    callback_phase = !pool.writer ? &phase->user : !active ? &phase->drain :
      active->orphan_cleanup ? &phase->orphan : &phase->user;
    return;
  }
  callback_failed |= event->status != PFS_OK;
  if (event->kind == TEST_FAILURE_READ) {
    phase->reads += event->count;
    if (pool.writer && pool.writer->map_planning) {
      phase->planning_reads += event->count;
    }
  }
  if (event->kind == TEST_FAILURE_FLUSH) {
    callback_phase->flushes++;
  }
  if (event->kind != TEST_FAILURE_WRITE) {
    return;
  }
  if (event->first == 0 || event->first == device.builder.reader.geometry.block_count - 1) {
    require(event->count == 1, "publication slot must be one block");
    phase->slot_writes++;
  }
  for (uint64_t block = event->first; block < event->first + event->count; block++) {
    bool data = false;
    if (pool.writer && callback_phase != &phase->drain) {
      const struct pfs_batch *writer_batch = pool.writer->active_batch;
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
  sample_debt();
  phase->operations++;
  if (native && batch && phase->operations % 16 == 0) {
    require(syncfs(root_fd) == 0, "batch filesystem sync");
    phase->barriers++;
  }
}

static void
close_file(struct comparison_file *handle)
{
  if (handle->fd < 0 && !handle->view) {
    return;
  }
  if (native) {
    require(close(handle->fd) == 0, "close file");
    handle->fd = -1;
  } else {
    reset_trace();
    struct pfs_view_close_result result;
    require_status(pfs_view_close(&handle->view, &result), "close core file");
    require(result.released, "core close did not release view");
  }
  sample_debt();
  phase->closes++;
}

static struct comparison_file
create_file(struct oracle_file *oracle, const char *name)
{
  require(strlen(name) < sizeof(oracle->name), "file name too long");
  struct comparison_file handle = {.fd = -1, .oracle = oracle};
  if (native) {
    handle.fd = openat(root_fd, name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (handle.fd < 0 && native_refusal(errno, "create", 0)) {
      return handle;
    }
    require(handle.fd >= 0, "create native file");
    if (!batch) {
      require(fsync(handle.fd) == 0 && fsync(root_fd) == 0, "sync creation and parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    struct pfs_view_identity identity;
    enum pfs_status status = pfs_view_create_file(parent, (const uint8_t *)name, strlen(name),
      &file_rights, &handle.view, &identity, &result);
    if (!accept_operation(status, &result, "create")) {
      require(!result.namespace_confirmed && !handle.view, "refused creation changed namespace");
      return handle;
    }
    require(result.namespace_confirmed, "unconfirmed creation");
  }
  oracle->length = 0;
  oracle->present = true;
  strcpy(oracle->name, name);
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

static bool
write_file(struct comparison_file *handle, uint64_t offset, size_t length, unsigned tag)
{
  require(length <= sizeof(payload), "oversized request");
  struct oracle_file *oracle = handle->oracle;
  require(offset <= oracle->capacity && length <= oracle->capacity - offset,
    "write exceeds configured oracle capacity");
  for (size_t i = 0; i < length; i++) {
    payload[i] = (uint8_t)(1u + tag * 31u + i * 7u + (i >> 8));
  }
  size_t confirmed = length;
  bool complete = true;
  if (native) {
    size_t done = 0;
    while (done < length) {
      ssize_t count = pwrite(handle->fd, payload + done, length - done, (off_t)(offset + done));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && native_refusal(errno, "write", done)) {
        complete = false;
        break;
      }
      require(count > 0, "write native file");
      done += (size_t)count;
    }
    confirmed = done;
    if (!batch) {
      require(fsync(handle->fd) == 0, "sync written file");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    enum pfs_status status = pfs_view_write(handle->view, offset, payload, length, &result);
    complete = accept_operation(status, &result, "write");
    require(result.confirmed_bytes <= length, "invalid confirmed write prefix");
    confirmed = (size_t)result.confirmed_bytes;
    require(!complete || confirmed == length, "short complete core write");
  }
  if (confirmed) {
    if (offset > oracle->length) {
      memset(oracle->bytes + oracle->length, 0, (size_t)(offset - oracle->length));
    }
    for (size_t i = 0; i < confirmed; i++) {
      oracle->bytes[offset + i] = (uint8_t)(1 + (uint64_t)tag * 31 + i * 7 + i / 256);
    }
    if (offset + confirmed > oracle->length) {
      oracle->length = offset + confirmed;
    }
  }
  phase->application_bytes += confirmed;
  if (complete) {
    completed_operation();
  } else {
    sample_debt();
  }
  return complete;
}

static bool
rename_file(struct oracle_file *source, const char *name, struct oracle_file *victim)
{
  if (native) {
    int status = renameat(root_fd, source->name, root_fd, name);
    if (status < 0 && native_refusal(errno, "rename", 0)) {
      return false;
    }
    require(status == 0, "rename native file");
    if (!batch) {
      /* Both affected parents are the same held root in this bounded case. */
      require(fsync(root_fd) == 0, "sync renamed parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    enum pfs_status status = pfs_view_rename(parent, (const uint8_t *)source->name, strlen(source->name),
      parent, (const uint8_t *)name, strlen(name), victim != NULL, &result);
    if (!accept_operation(status, &result, "rename")) {
      require(!result.namespace_confirmed, "refused rename changed namespace");
      return false;
    }
    require(result.namespace_confirmed, "unconfirmed rename");
  }
  strcpy(source->name, name);
  if (victim) {
    victim->present = false;
  }
  completed_operation();
  return true;
}

static bool
remove_file(struct oracle_file *oracle)
{
  if (native) {
    int status = unlinkat(root_fd, oracle->name, 0);
    if (status < 0 && native_refusal(errno, "remove", 0)) {
      return false;
    }
    require(status == 0, "remove native file");
    if (!batch) {
      require(fsync(root_fd) == 0, "sync removed parent");
    }
  } else {
    reset_trace();
    struct pfs_write_result result;
    enum pfs_status status = pfs_view_remove(parent, (const uint8_t *)oracle->name,
      strlen(oracle->name), &result);
    if (!accept_operation(status, &result, "remove")) {
      require(!result.namespace_confirmed, "refused removal changed namespace");
      return false;
    }
    require(result.namespace_confirmed, "unconfirmed removal");
  }
  oracle->present = false;
  completed_operation();
  return true;
}

static uint64_t
mount_id(int fd)
{
  struct statx attributes;
  require(statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &attributes) == 0 &&
    (attributes.stx_mask & STATX_MNT_ID), "read held descriptor mount ID");
  return attributes.stx_mnt_id;
}

static void
require_read_only(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  require(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && !(flags & O_PATH),
    "native descriptors must be open read-only");
}

static void
require_loop_node(const char *path, dev_t loop_device)
{
  int fd = open(path, O_PATH | O_CLOEXEC | O_NOFOLLOW);
  struct stat attributes;
  require(fd >= 0 && fstat(fd, &attributes) == 0 &&
    S_ISBLK(attributes.st_mode) && attributes.st_rdev == loop_device,
    "native filesystem device must be the held loop");
  require(close(fd) == 0, "close inspected device node");
}

static void
verify_ext4_journal(void)
{
  /* Linux ext4_super_block: external UUID, internal inode, external device.
   * An ext4 root on RAM can otherwise send journal writes to another device. */
  uint8_t journal[EXT4_JOURNAL_UUID_BYTES + EXT4_JOURNAL_INODE_BYTES + EXT4_JOURNAL_DEVICE_BYTES];
  require(pread(native_backing_fd, journal, sizeof(journal),
    EXT4_SUPERBLOCK_OFFSET + EXT4_JOURNAL_UUID_OFFSET) == (ssize_t)sizeof(journal),
    "read held ext4 journal fields");
  for (size_t i = 0; i < EXT4_JOURNAL_UUID_BYTES; i++) {
    require(journal[i] == 0, "native ext4 must not use an external journal UUID");
  }
  bool internal = false;
  for (size_t i = EXT4_JOURNAL_UUID_BYTES;
       i < EXT4_JOURNAL_UUID_BYTES + EXT4_JOURNAL_INODE_BYTES; i++) {
    internal = internal || journal[i] != 0;
  }
  require(internal, "native ext4 must have an internal journal inode");
  for (size_t i = EXT4_JOURNAL_UUID_BYTES + EXT4_JOURNAL_INODE_BYTES;
       i < sizeof(journal); i++) {
    require(journal[i] == 0, "native ext4 must not use an external journal device");
  }
}

static void
verify_native_mount(uint64_t id, dev_t loop_device)
{
  struct statfs filesystem;
  require(fstatfs(root_fd, &filesystem) == 0 &&
    (filesystem.f_type == EXT4_SUPER_MAGIC || filesystem.f_type == BTRFS_SUPER_MAGIC),
    "native root must be ext4 or Btrfs");
  FILE *mounts = fopen("/proc/self/mountinfo", "re");
  require(mounts != NULL, "open native mount information");
  char line[MOUNT_LINE_BYTES];
  bool found = false;
  while (fgets(line, sizeof(line), mounts)) {
    require(strchr(line, '\n') != NULL, "native mount information exceeds buffer");
    unsigned long long candidate;
    require(sscanf(line, "%llu", &candidate) == 1, "parse native mount ID");
    if (candidate != id) {
      continue;
    }
    char *separator = strstr(line, " - ");
    char type[32], source[PATH_MAX];
    require(separator && sscanf(separator + 3, "%31s %4095s", type, source) == 2,
      "parse native mount source");
    require((filesystem.f_type == EXT4_SUPER_MAGIC && !strcmp(type, "ext4")) ||
      (filesystem.f_type == BTRFS_SUPER_MAGIC && !strcmp(type, "btrfs")),
      "native mount filesystem type mismatch");
    require_loop_node(source, loop_device);
    found = true;
    break;
  }
  require(found && !ferror(mounts), "held native mount is missing");
  require(fclose(mounts) == 0, "close native mount information");
  if (filesystem.f_type == EXT4_SUPER_MAGIC) {
    struct stat attributes;
    require(fstat(root_fd, &attributes) == 0 && attributes.st_dev == loop_device,
      "ext4 root must use the held loop");
    verify_ext4_journal();
  } else {
    /* Btrfs has an anonymous st_dev; inspect its sole device explicitly. */
    struct btrfs_ioctl_fs_info_args information = {0};
    require(ioctl(root_fd, BTRFS_IOC_FS_INFO, &information) == 0 &&
      information.num_devices == 1, "native Btrfs must have exactly one device");
    struct btrfs_ioctl_dev_info_args backing = {.devid = information.max_id};
    require(ioctl(root_fd, BTRFS_IOC_DEV_INFO, &backing) == 0 &&
      memchr(backing.path, '\0', sizeof(backing.path)), "inspect native Btrfs device");
    require_loop_node((const char *)backing.path, loop_device);
  }
}

static void
open_native_root(void)
{
  require_read_only(native_root_fd);
  require_read_only(native_loop_fd);
  require_read_only(native_backing_fd);
  struct stat root, loop, backing, scratch;
  require(fstat(native_root_fd, &root) == 0 && S_ISDIR(root.st_mode) &&
    fstat(native_loop_fd, &loop) == 0 && S_ISBLK(loop.st_mode) &&
    fstat(native_backing_fd, &backing) == 0 && S_ISREG(backing.st_mode) &&
    fstat(pfs_test_ram_directory_fd(), &scratch) == 0,
    "inspect held native descriptors");
  require(backing.st_dev == scratch.st_dev &&
    mount_id(native_backing_fd) == mount_id(pfs_test_ram_directory_fd()),
    "native loop backing must use the validated RAM mount");
  struct loop_info64 status;
  require(ioctl(native_loop_fd, LOOP_GET_STATUS64, &status) == 0 &&
    status.lo_device == (uint64_t)backing.st_dev && status.lo_inode == backing.st_ino &&
    status.lo_offset == 0 && status.lo_sizelimit == 0 && status.lo_flags == LO_FLAGS_AUTOCLEAR,
    "held loop must cover the verified RAM backing");
  uint64_t id = mount_id(native_root_fd);
  int supplied = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat current;
  require(supplied >= 0 && fstat(supplied, &current) == 0 &&
    current.st_dev == root.st_dev && current.st_ino == root.st_ino && mount_id(supplied) == id,
    "native root must match the held directory");
  require(close(supplied) == 0, "close supplied native root");
  /* All workload paths resolve from this held directory, never from --root. */
  root_fd = fcntl(native_root_fd, F_DUPFD_CLOEXEC, 3);
  require(root_fd >= 0, "hold verified native root");
  verify_native_mount(id, loop.st_rdev);
}

static void
open_backend(void)
{
  if (native) {
    open_native_root();
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
  root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  require(root_fd >= 0, "open supplied root");
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
  if (sustained) {
    device.observer = observe;
  }
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
  pool.writer->collect_map_metrics = true;
  if (!sustained) {
    phase->initial_flush_ordinal = device.ordinals[TEST_FAILURE_FLUSH];
  }
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
    size_t anchor = 0;
    while (anchor < file_count && !files[anchor].present) {
      anchor++;
    }
    if (anchor == file_count) {
      require(incomplete && !pool.writer->status.drain_pending,
        "pending cleanup has no confirmed checkpoint grant");
      return;
    }
    struct comparison_file handle = open_file(&files[anchor]);
    reset_trace();
    struct pfs_write_result result;
    require_status(pfs_view_checkpoint(handle.view, &result), "checkpoint");
    require_result(&result);
    require(result.maintenance_completion == PFS_MAINTENANCE_COMPLETE,
      "checkpoint did not complete retirement fence");
    sample_debt();
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
    struct oracle_file *temporary = &files[(size_t)population + 1];
    struct comparison_file handle = create_file(temporary, "prehistory.tmp");
    write_file(&handle, 0, PFS_BLOCK_SIZE, 3000u + i);
    close_file(&handle);
    char destination[32];
    strcpy(destination, files[i].name);
    rename_file(temporary, destination, &files[i]);
    require(temporary->length <= files[i].capacity, "replacement exceeds oracle capacity");
    memcpy(files[i].bytes, temporary->bytes, (size_t)temporary->length);
    strcpy(files[i].name, temporary->name);
    files[i].length = temporary->length;
    files[i].present = true;
    temporary->present = false;
  }
  if (strcmp(case_name, "compiler")) {
    struct comparison_file handle = create_file(&files[population], "work");
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
      struct oracle_file *temporary = &files[population];
      struct comparison_file handle = create_file(temporary, "compiler.tmp");
      write_file(&handle, 0, PFS_BLOCK_SIZE, 5000u + i);
      close_file(&handle);
      rename_file(temporary, "compiler.o", NULL);
      remove_file(temporary);
    }
    return;
  }
  struct comparison_file handle = open_file(&files[population]);
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

static void
prepare_sustained(void)
{
  uint32_t tag = seed;
  for (unsigned i = 0; i < population && !incomplete; i++) {
    char name[32];
    snprintf(name, sizeof(name), "pop-%u", i);
    struct comparison_file handle = create_file(&files[i], name);
    unsigned blocks = background_blocks / population + (i < background_blocks % population);
    for (unsigned j = 0; j < blocks && !incomplete; j++) {
      write_file(&handle, (uint64_t)j * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE, tag++);
    }
    close_file(&handle);
  }
  if (!incomplete && strcmp(case_name, "compiler")) {
    struct comparison_file handle = create_file(&files[population], "work");
    if (!strcmp(case_name, "overwrite")) {
      for (uint64_t offset = 0; offset < target_bytes && !incomplete;) {
        size_t length = target_bytes - offset < REQUEST_MAX ?
          (size_t)(target_bytes - offset) : REQUEST_MAX;
        write_file(&handle, offset, length, tag++);
        offset += length;
      }
    }
    close_file(&handle);
  }
  final_boundary();
}

static void
run_sustained_window(unsigned window, uint32_t *random)
{
  if (incomplete) {
    return;
  }
  if (!strcmp(case_name, "compiler")) {
    for (unsigned i = 0; i < cycles_per_window && !incomplete; i++) {
      struct oracle_file *temporary = &files[population];
      struct comparison_file handle = create_file(temporary, "compiler.tmp");
      if (!incomplete) {
        write_file(&handle, 0, PFS_BLOCK_SIZE,
          seed + (uint32_t)((uint64_t)window * cycles_per_window + i));
      }
      close_file(&handle);
      if (!incomplete && rename_file(temporary, "compiler.o", NULL)) {
        remove_file(temporary);
      }
    }
    return;
  }
  struct comparison_file handle = open_file(&files[population]);
  for (unsigned i = 0; i < operations_per_window && !incomplete; i++) {
    uint64_t operation = (uint64_t)window * operations_per_window + i;
    uint64_t offset = operation * PFS_BLOCK_SIZE;
    size_t length = PFS_BLOCK_SIZE;
    if (!strcmp(case_name, "overwrite")) {
      *random = *random * 1664525u + 1013904223u;
      length = overwrite_bytes;
      offset = (operation % 2) ?
        (uint64_t)(*random % (target_bytes / PFS_BLOCK_SIZE - 1)) * PFS_BLOCK_SIZE +
          PFS_BLOCK_SIZE - overwrite_bytes / 2 :
        (uint64_t)(*random % (target_bytes / PFS_BLOCK_SIZE)) * PFS_BLOCK_SIZE +
          (*random / (target_bytes / PFS_BLOCK_SIZE)) % (PFS_BLOCK_SIZE - overwrite_bytes + 1);
    }
    write_file(&handle, offset, length, seed + (uint32_t)operation);
  }
  close_file(&handle);
}

static void
snapshot_state(struct phase_stats *stats)
{
  sample_debt();
  if (!native && pool.writer) {
    const struct pfs_admit_state *state = &pool.writer->states[pool.writer->selected];
    stats->extents = state->file_extents;
    stats->metadata_blocks = state->metadata_blocks;
    stats->map_records = state->map_count;
    stats->map_nodes = state->map_nodes;
    stats->live_blocks = state->volumes[0].record.live_blocks;
    stats->reusable_blocks = state->reusable_blocks;
    stats->generation = state->candidate.superblock.header.birth;
  }
}

static void
verify(uint64_t *verified_files, uint64_t *verified_bytes)
{
  for (size_t i = 0; i < file_count; i++) {
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
        require(actual[j] == oracle->bytes[offset + j], "independent content oracle mismatch");
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
    sample_debt();
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
  if (sustained) {
    printf("{\"name\":\"%s\",", name);
  } else {
    printf("\"%s\":{", name);
  }
  printf("\"operations\":%" PRIu64 ",\"application_bytes\":%" PRIu64
    ",\"closes\":%" PRIu64 ",\"barriers\":%" PRIu64, stats->operations,
    stats->application_bytes, stats->closes, stats->barriers);
  if (sustained) {
    printf(",\"elapsed_seconds\":%.9f", stats->elapsed_seconds);
  }
  if (native) {
    printf(",\"core_callbacks\":null}");
    return;
  }
  printf(",\"maximum_boundary_volume_debt_blocks\":%" PRIu64
    ",\"final_volume_debt_blocks\":%" PRIu64 ",\"final_pool_debt_blocks\":%" PRIu64,
    stats->maximum_boundary_volume_debt, stats->final_volume_debt, stats->final_pool_debt);
  const struct callback_stats *groups[] = {&stats->user, &stats->orphan, &stats->drain};
  const char *names[] = {"user", "orphan", "drain"};
  for (size_t i = 0; i < 3; i++) {
    printf(",\"%s\":{\"data_blocks\":%" PRIu64 ",\"metadata_blocks\":%" PRIu64
      ",\"flushes\":%" PRIu64 "}", names[i], groups[i]->data_blocks,
      groups[i]->metadata_blocks, groups[i]->flushes);
  }
  if (sustained) {
    printf(",\"read_blocks\":%" PRIu64 ",\"planning_read_blocks\":%" PRIu64
      ",\"state\":{\"extents\":%" PRIu64 ",\"metadata_blocks\":%" PRIu64
      ",\"map_records\":%" PRIu64 ",\"map_nodes\":%" PRIu64
      ",\"live_blocks\":%" PRIu64 ",\"reusable_blocks\":%" PRIu64
      ",\"generation\":%" PRIu64 "}", stats->reads, stats->planning_reads,
      stats->extents, stats->metadata_blocks, stats->map_records, stats->map_nodes,
      stats->live_blocks, stats->reusable_blocks, stats->generation);
  }
  printf("}");
}

static const struct phase_stats *
map_phase(size_t index)
{
  return sustained ? &sustained_phases[index] : index ? &measurement : &preparation;
}

static void
print_map_stats(void)
{
  size_t count = sustained ? sustained_phase_count : 2;
  printf(",\"map_plans\":{\"phase_order\":[");
  for (size_t i = 0; i < count; i++) {
    printf("%s\"%s\"", i ? "," : "", sustained ? sustained_phases[i].name :
      i ? "measurement" : "preparation");
  }
  printf("]");
#define MAP_FIELD(label, member, format) do { \
  printf(",\"" label "\":["); \
  for (size_t i = 0; i < count; i++) { \
    printf("%s" format, i ? "," : "", map_phase(i)->maps.member); \
  } \
  printf("]"); \
} while (0)
  MAP_FIELD("publications", publications, "%" PRIu64);
  MAP_FIELD("local", local, "%" PRIu64);
  MAP_FIELD("bulk", bulk, "%" PRIu64);
  MAP_FIELD("source_sum", source_sum, "%" PRIu64);
  MAP_FIELD("source_max", source_max, "%" PRIu64);
  MAP_FIELD("closure_sum", closure_sum, "%" PRIu64);
  MAP_FIELD("retired_sum", retired, "%" PRIu64);
  MAP_FIELD("closure_max", closure_max, "%" PRIu64);
  MAP_FIELD("growth_passes", passes, "%" PRIu64);
  MAP_FIELD("largest_addition", largest_addition, "%" PRIu64);
  MAP_FIELD("redistribution_additions", redistribution, "%" PRIu64);
  MAP_FIELD("repair_underflow", repair.underflow, "%" PRIu64);
  MAP_FIELD("repair_overflow", repair.overflow, "%" PRIu64);
  MAP_FIELD("repair_compared", repair.compared, "%" PRIu64);
  MAP_FIELD("repair_right_preferred", repair.right_preferred, "%" PRIu64);
  MAP_FIELD("repair_ties", repair.ties, "%" PRIu64);
  MAP_FIELD("repair_fitting", repair.fitting, "%" PRIu64);
  MAP_FIELD("repair_left_bridges", repair.left_bridges, "%" PRIu64);
  MAP_FIELD("repair_right_bridges", repair.right_bridges, "%" PRIu64);
  MAP_FIELD("repair_left_deficit_sum", repair.left_deficit_sum, "%" PRIu64);
  MAP_FIELD("repair_right_deficit_sum", repair.right_deficit_sum, "%" PRIu64);
  MAP_FIELD("repair_chosen_deficit_sum", repair.chosen_deficit_sum, "%" PRIu64);
  MAP_FIELD("split_opportunities", split_opportunities, "%" PRIu64);
  MAP_FIELD("split_trials", split_trials, "%" PRIu64);
  MAP_FIELD("split_selected", split_selected, "%" PRIu64);
  MAP_FIELD("closure_evaluations", closure_evaluations, "%" PRIu64);
  MAP_FIELD("split_discarded_nodes", split_discarded_nodes, "%" PRIu64);
  MAP_FIELD("split_discarded_growth", split_discarded_growth, "%" PRIu64);
  MAP_FIELD("split_skip_root", split_skips[0], "%" PRIu64);
  MAP_FIELD("split_skip_parent", split_skips[1], "%" PRIu64);
  MAP_FIELD("split_skip_live_cap", split_skips[2], "%" PRIu64);
  MAP_FIELD("split_skip_global", split_skips[3], "%" PRIu64);
  MAP_FIELD("split_miss_global", split_misses[3], "%" PRIu64);
  MAP_FIELD("split_miss_unneeded", split_misses[4], "%" PRIu64);
  MAP_FIELD("split_miss_insufficient", split_misses[5], "%" PRIu64);
  MAP_FIELD("split_miss_other_run", split_misses[6], "%" PRIu64);
  MAP_FIELD("split_miss_resource", split_misses[7], "%" PRIu64);
  MAP_FIELD("emitted_nodes", emitted, "%" PRIu64);
  MAP_FIELD("bulk_reference_nodes", bulk_reference, "%" PRIu64);
  MAP_FIELD("local_node_difference", local_node_difference, "%" PRId64);
  MAP_FIELD("planning_seconds", planning_seconds, "%.9f");
  MAP_FIELD("planning_max_seconds", planning_max, "%.9f");
  if (sustained) {
    MAP_FIELD("local_emitted_nodes", local_emitted, "%" PRIu64);
    MAP_FIELD("bulk_emitted_nodes", bulk_emitted, "%" PRIu64);
    MAP_FIELD("local_planning_seconds", local_planning_seconds, "%.9f");
    MAP_FIELD("bulk_planning_seconds", bulk_planning_seconds, "%.9f");
  }
#undef MAP_FIELD
  printf(",\"fallback_counts\":[");
  for (size_t i = 0; i < count; i++) {
    const struct map_stats *stats = &map_phase(i)->maps;
    printf("%s[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", i ? "," : "",
      stats->fallback[0], stats->fallback[1], stats->fallback[2], stats->fallback[3]);
  }
  printf("],\"last_failed_run\":[");
  for (size_t i = 0; i < count; i++) {
    const struct map_stats *stats = &map_phase(i)->maps;
    printf("%s[%" PRIu64 ",%" PRIu64 "]", i ? "," : "", stats->failed_leaves, stats->failed_records);
  }
  printf("],\"local_cost_counts\":[");
  for (size_t i = 0; i < count; i++) {
    const struct map_stats *stats = &map_phase(i)->maps;
    printf("%s[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", i ? "," : "",
      stats->local_cost[0], stats->local_cost[1], stats->local_cost[2]);
  }
  printf("]}");
}

static void
check_map_phase(const struct phase_stats *stats)
{
  if (native) {
    return;
  }
  uint64_t flushes = stats->user.flushes + stats->orphan.flushes + stats->drain.flushes;
  require(flushes == device.ordinals[TEST_FAILURE_FLUSH] - stats->initial_flush_ordinal,
    "phase flush accounting mismatch");
  if (sustained && stats == &sustained_phases[0]) {
    return;
  }
  /* Fixed slot locations and two flushes per healthy publication are format
   * and durability contracts; the workload's publication count is not fixed. */
  require(stats->maps.publications == stats->slot_writes && !(flushes % 2) &&
    stats->maps.publications == flushes / 2, "phase publication accounting mismatch");
}

static int
parse_descriptor(const char *text)
{
  require(text && *text, "missing native descriptor");
  unsigned value = 0;
  for (; *text; text++) {
    require(*text >= '0' && *text <= '9' &&
      value <= ((unsigned)INT_MAX - (unsigned)(*text - '0')) / 10,
      "invalid native descriptor");
    value = value * 10 + (unsigned)(*text - '0');
  }
  require(value >= 3, "native descriptors must not use standard streams");
  return (int)value;
}

static unsigned
parse_unsigned(const char *text, bool positive)
{
  require(text && *text, "missing numeric option");
  unsigned value = 0;
  for (; *text; text++) {
    require(*text >= '0' && *text <= '9' &&
      value <= (UINT_MAX - (unsigned)(*text - '0')) / 10, "invalid numeric option");
    value = value * 10 + (unsigned)(*text - '0');
  }
  require(!positive || value, "numeric option must be positive");
  return value;
}

static void
parse_arguments(int argc, char **argv)
{
  const char *filesystem = NULL, *durability = NULL, *population_text = NULL, *stops = NULL;
  const char *root_descriptor = NULL, *loop_descriptor = NULL, *backing_descriptor = NULL;
  const char *workload = NULL, *background = NULL, *windows = NULL, *operations = NULL;
  const char *cycles = NULL, *seed_text = NULL, *target = NULL, *overwrite = NULL;
  require(argc >= 11 && argc % 2 == 1, "expected comparison option/value pairs");
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
    } else if (!strcmp(argv[i], "--native-root-fd")) {
      destination = &root_descriptor;
    } else if (!strcmp(argv[i], "--native-loop-fd")) {
      destination = &loop_descriptor;
    } else if (!strcmp(argv[i], "--native-backing-fd")) {
      destination = &backing_descriptor;
    } else if (!strcmp(argv[i], "--workload")) {
      destination = &workload;
    } else if (!strcmp(argv[i], "--background-blocks")) {
      destination = &background;
    } else if (!strcmp(argv[i], "--windows")) {
      destination = &windows;
    } else if (!strcmp(argv[i], "--operations")) {
      destination = &operations;
    } else if (!strcmp(argv[i], "--cycles")) {
      destination = &cycles;
    } else if (!strcmp(argv[i], "--seed")) {
      destination = &seed_text;
    } else if (!strcmp(argv[i], "--target-bytes")) {
      destination = &target;
    } else if (!strcmp(argv[i], "--overwrite-bytes")) {
      destination = &overwrite;
    }
    require(destination && !*destination, "unknown or repeated option");
    *destination = argv[i + 1];
  }
  require(filesystem && root_path && population_text && case_name && durability, "missing option");
  require(!strcmp(filesystem, "pyxis") || !strcmp(filesystem, "native"), "unknown filesystem");
  sustained = workload && !strcmp(workload, "sustained");
  require(!workload || sustained, "unknown workload");
  population = parse_unsigned(population_text, true);
  if (sustained) {
    require(background && windows && operations && cycles && seed_text && target && overwrite,
      "missing sustained settings");
    background_blocks = parse_unsigned(background, true);
    window_count = parse_unsigned(windows, true);
    operations_per_window = parse_unsigned(operations, true);
    cycles_per_window = parse_unsigned(cycles, true);
    target_bytes = parse_unsigned(target, true);
    overwrite_bytes = parse_unsigned(overwrite, true);
    seed = parse_unsigned(seed_text, false);
    require(background_blocks >= population, "each background file needs data");
    require(target_bytes >= 2 * PFS_BLOCK_SIZE && !(target_bytes % PFS_BLOCK_SIZE) &&
      overwrite_bytes >= 2 && overwrite_bytes <= PFS_BLOCK_SIZE,
      "overwrite shape needs a block-aligned target and a small cross-block request");
    require(!strcmp(case_name, "append") || !strcmp(case_name, "overwrite") ||
      !strcmp(case_name, "compiler"), "unknown sustained case");
    require(!strcmp(durability, "operation"), "sustained operations must be individually durable");
  } else {
    require(!background && !windows && !operations && !cycles && !seed_text && !target && !overwrite,
      "sustained options require sustained workload");
    require(population == 32 || population == 256, "baseline population must be 32 or 256");
    require(!strcmp(case_name, "small") || !strcmp(case_name, "large") ||
      !strcmp(case_name, "overwrite") || !strcmp(case_name, "compiler"), "unknown case");
  }
  require(!strcmp(durability, "operation") || !strcmp(durability, "batch"), "unknown durability");
  require(!stops || !strcmp(stops, "yes") || !strcmp(stops, "no"), "phase stops must be yes or no");
  phase_stops = stops && !strcmp(stops, "yes");
  native = !strcmp(filesystem, "native");
  if (native) {
    require(root_descriptor && loop_descriptor && backing_descriptor,
      "native mode requires launcher-held root, loop and RAM backing descriptors");
    native_root_fd = parse_descriptor(root_descriptor);
    native_loop_fd = parse_descriptor(loop_descriptor);
    native_backing_fd = parse_descriptor(backing_descriptor);
  } else {
    require(!root_descriptor && !loop_descriptor && !backing_descriptor,
      "native descriptors are not valid for Pyxis mode");
  }
  batch = !strcmp(durability, "batch");
  require(native || !batch, "Pyxis has operation durability only");
}

static void
allocate_oracles(void)
{
  uint64_t count = (uint64_t)population + 2;
  require(count <= SIZE_MAX / sizeof(*files), "oracle file count overflow");
  file_count = (size_t)count;
  files = calloc(file_count, sizeof(*files));
  require(files != NULL, "allocate expected namespace");
  for (size_t i = 0; i < file_count; i++) {
    uint64_t bytes = PFS_BLOCK_SIZE;
    if (i < population && sustained) {
      bytes *= background_blocks / population + (i < background_blocks % population);
    } else if (i == population && strcmp(case_name, "compiler")) {
      bytes = sustained ? !strcmp(case_name, "append") ?
        (uint64_t)window_count * operations_per_window : target_bytes :
        !strcmp(case_name, "small") ? 64 * PFS_BLOCK_SIZE :
        !strcmp(case_name, "large") ? 16 * REQUEST_MAX : REQUEST_MAX;
      if (sustained && !strcmp(case_name, "append")) {
        require(bytes <= SIZE_MAX / PFS_BLOCK_SIZE, "append oracle size overflow");
        bytes *= PFS_BLOCK_SIZE;
      }
    }
    require(bytes && bytes <= SIZE_MAX, "oracle size overflow");
    files[i].capacity = (size_t)bytes;
    files[i].bytes = calloc(1, files[i].capacity);
    require(files[i].bytes != NULL, "allocate expected payload");
  }
  if (sustained) {
    count = (uint64_t)window_count + 3;
    require(count <= SIZE_MAX / sizeof(*sustained_phases), "phase count overflow");
    sustained_phase_count = (size_t)count;
    sustained_phases = calloc(sustained_phase_count, sizeof(*sustained_phases));
    require(sustained_phases != NULL, "allocate phase observations");
    strcpy(sustained_phases[0].name, "setup");
    strcpy(sustained_phases[1].name, "preparation");
    for (unsigned i = 0; i < window_count; i++) {
      snprintf(sustained_phases[(size_t)i + 2].name, sizeof(sustained_phases[(size_t)i + 2].name),
        "window_%u", i + 1);
    }
    strcpy(sustained_phases[sustained_phase_count - 1].name, "final");
    phase = sustained_phases;
  }
}

static void
finish_sustained_phase(double started)
{
  phase->elapsed_seconds = now() - started;
  snapshot_state(phase);
  check_map_phase(phase);
  char marker[48];
  snprintf(marker, sizeof(marker), "%s_end", phase->name);
  phase_marker(marker);
}

static double
begin_sustained_phase(size_t index)
{
  phase = &sustained_phases[index];
  phase->initial_flush_ordinal = device.ordinals[TEST_FAILURE_FLUSH];
  planning_generation = 0;
  planning_phase = NULL;
  return now();
}
static void
release_oracles(void)
{
  for (size_t i = 0; i < file_count; i++) {
    free(files[i].bytes);
  }
  free(files);
  free(sustained_phases);
}

static int
run_sustained(void)
{
  double started = now();
  open_backend();
  finish_sustained_phase(started);
  started = begin_sustained_phase(1);
  prepare_sustained();
  finish_sustained_phase(started);
  uint32_t random = seed;
  for (unsigned i = 0; i < window_count; i++) {
    started = begin_sustained_phase((size_t)i + 2);
    run_sustained_window(i, &random);
    finish_sustained_phase(started);
  }
  started = begin_sustained_phase(sustained_phase_count - 1);
  final_boundary();
  snapshot_state(phase);
  close_core();
  finish_sustained_phase(started);
  struct phase_stats verification = {0};
  phase = &verification;
  reopen_verification();
  uint64_t verified_files = 0, verified_bytes = 0;
  verify(&verified_files, &verified_bytes);
  close_core();
  uint64_t core_peak = 0, backing_bytes = 0;
  if (!native) {
    require(!callback_failed && !device.infrastructure_failure, "callback observation failure");
    struct stat attributes;
    require(fstat(fileno(device.backing.file), &attributes) == 0, "inspect durable backing allocation");
    backing_bytes = (uint64_t)attributes.st_blocks * 512;
    core_peak = device.backing.peak_memory_bytes;
    require(test_failure_close(&device), "close adapter");
  }
  require(close(root_fd) == 0, "close root directory");
  printf("{\"filesystem\":\"%s\",\"workload\":\"sustained\",\"population\":%u,"
    "\"case\":\"%s\",\"durability\":\"operation\",\"complete\":%s,"
    "\"seed\":%u,\"geometry_bytes\":%" PRIu64 ",\"block_bytes\":%u,"
    "\"background_blocks\":%u,\"windows\":%u,\"operations\":%u,\"cycles\":%u,"
    "\"target_bytes\":%u,\"overwrite_bytes\":%u,\"phases\":[",
    native ? "native" : "pyxis", population, case_name, incomplete ? "false" : "true", seed,
    IMAGE_BLOCKS * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE, background_blocks, window_count,
    operations_per_window, cycles_per_window, target_bytes, overwrite_bytes);
  for (size_t i = 0; i < sustained_phase_count; i++) {
    if (i) {
      printf(",");
    }
    print_phase(sustained_phases[i].name, &sustained_phases[i]);
  }
  printf("]");
  if (!native) {
    printf(",\"extent_limit\":%" PRIu64 ",\"metadata_limit\":%" PRIu64
      ",\"core_memory_peak_bytes\":%" PRIu64
      ",\"backing_allocated_bytes_at_case_end\":%" PRIu64,
      options.extent_limit, options.metadata_limit, core_peak, backing_bytes);
    print_map_stats();
  }
  uint64_t expected_capacity = 0;
  for (size_t i = 0; i < file_count; i++) {
    require(files[i].capacity <= UINT64_MAX - expected_capacity, "oracle accounting overflow");
    expected_capacity += files[i].capacity;
  }
  if (incomplete) {
    printf(",\"refusal\":{\"status_code\":%u,\"status\":\"%s\",\"errno\":%u,"
      "\"phase\":\"%s\",\"operation\":\"%s\",\"confirmed_bytes\":%" PRIu64
      ",\"namespace_confirmed\":%s}", (unsigned)refusal_status,
      refusal_errno ? "native-capacity" : pfs_status_string(refusal_status), refusal_errno,
      refusal_phase, refusal_operation, refusal_bytes, refusal_namespace ? "true" : "false");
  }
  printf(",\"expected_capacity_bytes\":%" PRIu64
    ",\"verification\":{\"oracle\":\"independent-byte-mirrors\","
    "\"contents_verified\":true,\"namespace_verified\":true,\"files\":%" PRIu64
    ",\"bytes\":%" PRIu64 "}}\n", expected_capacity, verified_files, verified_bytes);
  release_oracles();
  return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
  parse_arguments(argc, argv);
  pfs_test_require_ram();
  allocate_oracles();
  if (sustained) {
    return run_sustained();
  }
  open_backend();
  prepare();
  check_map_phase(&preparation);
  phase = &measurement;
  measurement.initial_flush_ordinal = device.ordinals[TEST_FAILURE_FLUSH];
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
  check_map_phase(&measurement);
  phase_marker("measurement_end");
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
    "\"durability\":\"%s\",\"seed\":1,\"geometry_bytes\":%" PRIu64 ","
    "\"block_bytes\":%u,\"batch_unit\":%s,\"elapsed_seconds\":%.9f,",
    native ? "native" : "pyxis", population, case_name, batch ? "batch" : "operation",
    IMAGE_BLOCKS * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE,
    batch ? "\"16 create/write/rename/remove calls\"" : "null", elapsed);
  if (!native) {
    printf("\"extent_limit\":%" PRIu64 ",\"metadata_limit\":%" PRIu64 ",",
      options.extent_limit, options.metadata_limit);
  }
  print_phase("preparation", &preparation);
  printf(",");
  print_phase("measurement", &measurement);
  if (!native) {
    printf(",\"selected_state\":{\"extents\":%" PRIu64 ",\"metadata_blocks\":%" PRIu64
      ",\"map_records\":%zu}", extents, metadata, map_records);
    print_map_stats();
  }
  printf(",\"verification\":{\"oracle\":\"independent-byte-mirrors\",\"contents_verified\":true,"
    "\"files\":%" PRIu64 ",\"bytes\":%" PRIu64 "}}\n", verified_files, verified_bytes);
  release_oracles();
  return EXIT_SUCCESS;
}
