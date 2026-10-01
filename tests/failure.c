/* SPDX-License-Identifier: MPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "failure.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_HEADER_SIZE 16u
#define LOG_TRAILER_SIZE 8u
#define LOG_RECORD_SIZE (LOG_HEADER_SIZE + PFS_BLOCK_SIZE + LOG_TRAILER_SIZE)

enum log_state {
  LOG_CACHE_ONLY,
  LOG_PENDING,
  LOG_REMOVE,
};

struct test_failure_log_record {
  uint64_t block;
  enum log_state state;
};

static struct test_failure *adapters;

static enum pfs_status
infrastructure_error(struct test_failure *adapter)
{
  adapter->infrastructure_failure = true;
  return PFS_IO;
}

static bool
file_read(FILE *file, uint64_t offset, void *buffer, size_t length)
{
  return pread(fileno(file), buffer, length, (off_t)offset) == (ssize_t)length;
}

static bool
file_write(FILE *file, uint64_t offset, const void *buffer, size_t length)
{
  return pwrite(fileno(file), buffer, length, (off_t)offset) == (ssize_t)length;
}

static bool
log_write(struct test_failure *adapter, size_t index, uint64_t block,
          enum log_state state, const void *bytes)
{
  uint8_t header[LOG_HEADER_SIZE] = {0};
  uint8_t trailer[LOG_TRAILER_SIZE];
  test_put_u64(header, block);
  test_put_u32(header + 8, 1);
  test_put_u32(header + 12, (uint32_t)state);
  test_put_u64(trailer, LOG_RECORD_SIZE);
  uint64_t offset = (uint64_t)index * LOG_RECORD_SIZE;
  return file_write(adapter->log, offset, header, sizeof(header)) &&
    file_write(adapter->log, offset + sizeof(header), bytes, PFS_BLOCK_SIZE) &&
    file_write(adapter->log, offset + sizeof(header) + PFS_BLOCK_SIZE, trailer, sizeof(trailer));
}

static bool
log_read(struct test_failure *adapter, size_t index, void *bytes)
{
  return file_read(adapter->log, (uint64_t)index * LOG_RECORD_SIZE + LOG_HEADER_SIZE,
                   bytes, PFS_BLOCK_SIZE);
}

static enum pfs_status
append_pending(struct test_failure *adapter, uint64_t first, uint32_t count,
               const void *buffer)
{
  if (count > adapter->dirty_capacity - adapter->record_count) {
    return infrastructure_error(adapter);
  }
  const uint8_t *bytes = buffer;
  for (uint32_t i = 0; i < count; ++i) {
    size_t index = adapter->record_count;
    if (!log_write(adapter, index, first + i, LOG_PENDING, bytes + i * PFS_BLOCK_SIZE)) {
      return infrastructure_error(adapter);
    }
    adapter->records[index] = (struct test_failure_log_record){first + i, LOG_PENDING};
    ++adapter->record_count;
    ++adapter->pending_count;
  }
  return PFS_OK;
}

static enum pfs_status
compact_log(struct test_failure *adapter)
{
  size_t kept = 0, pending = 0;
  for (size_t i = 0; i < adapter->record_count; ++i) {
    struct test_failure_log_record record = adapter->records[i];
    if (record.state == LOG_REMOVE) {
      continue;
    }
    if (kept != i) {
      if (!log_read(adapter, i, adapter->transfer) ||
          !log_write(adapter, kept, record.block, record.state, adapter->transfer)) {
        return infrastructure_error(adapter);
      }
    }
    adapter->records[kept++] = record;
    pending += record.state == LOG_PENDING;
  }
  if (ftruncate(fileno(adapter->log), (off_t)(kept * LOG_RECORD_SIZE)) != 0) {
    return infrastructure_error(adapter);
  }
  adapter->record_count = kept;
  adapter->pending_count = pending;
  return PFS_OK;
}

static enum pfs_status
promote(struct test_failure *adapter, size_t prefix, bool selected_block, uint64_t block)
{
  size_t promoted = 0;
  for (size_t i = 0; i < adapter->record_count; ++i) {
    struct test_failure_log_record *record = &adapter->records[i];
    if (record->state != LOG_PENDING || promoted == prefix ||
        (selected_block && record->block != block)) {
      continue;
    }
    if (!log_read(adapter, i, adapter->transfer) ||
        !file_write(adapter->backing.file, record->block * PFS_BLOCK_SIZE,
                     adapter->transfer, PFS_BLOCK_SIZE)) {
      return infrastructure_error(adapter);
    }
    /* Earlier overlays of this block cannot hide its newer durable contents. */
    for (size_t j = 0; j <= i; ++j) {
      if (adapter->records[j].block == record->block) {
        adapter->records[j].state = LOG_REMOVE;
      }
    }
    ++promoted;
  }
  if (fsync(fileno(adapter->backing.file)) != 0) {
    return infrastructure_error(adapter);
  }
  return compact_log(adapter);
}

static struct test_failure_event *
begin_event(struct test_failure *adapter, enum test_failure_event_kind kind,
            uint64_t first, uint32_t count)
{
  if (adapter->event_count == TEST_FAILURE_EVENTS_MAX || adapter->infrastructure_failure) {
    infrastructure_error(adapter);
    return NULL;
  }
  struct test_failure_event *event = &adapter->events[adapter->event_count++];
  *event = (struct test_failure_event) {
    .number = ++adapter->event_number,
    .kind = kind,
    .ordinal = ++adapter->ordinals[kind],
    .first = first,
    .count = count,
    .flush_ordinal = adapter->ordinals[TEST_FAILURE_FLUSH],
    .status = PFS_OK,
    .selected = adapter->fault.enabled && adapter->fault.kind == kind &&
                adapter->fault.ordinal == adapter->ordinals[kind],
    .mode = adapter->fault.mode,
    .prefix = adapter->fault.prefix,
  };
  if (event->selected) {
    adapter->fault.enabled = false;
    adapter->triggered = true;
  }
  if (adapter->observer) {
    adapter->observer(adapter, event, true, adapter->observer_context);
  }
  return event;
}

static enum pfs_status
end_event(struct test_failure *adapter, struct test_failure_event *event,
          enum pfs_status status)
{
  event->status = status;
  if (adapter->observer) {
    adapter->observer(adapter, event, false, adapter->observer_context);
  }
  return status;
}

enum pfs_status
test_failure_durable_read(const struct test_failure *adapter, uint64_t first,
                          uint32_t count, void *buffer)
{
  if (!adapter || !adapter->backing.file || !buffer || !count ||
      count > PFS_IO_BLOCKS_MAX || first >= adapter->builder.reader.geometry.block_count ||
      count > adapter->builder.reader.geometry.block_count - first) {
    return PFS_INVALID;
  }
  return file_read(adapter->backing.file, first * PFS_BLOCK_SIZE, buffer,
                   (size_t)count * PFS_BLOCK_SIZE) ? PFS_OK : PFS_IO;
}

static enum pfs_status
read_blocks(void *context, uint64_t first, uint32_t count, void *buffer)
{
  struct test_failure *adapter = context;
  struct test_failure_event *event = begin_event(adapter, TEST_FAILURE_READ, first, count);
  if (!event) {
    return PFS_IO;
  }
  if (event->selected) {
    return end_event(adapter, event, PFS_IO);
  }
  uint8_t *bytes = buffer;
  for (uint32_t i = 0; i < count; ++i) {
    bool found = false;
    for (size_t j = adapter->record_count; j; --j) {
      if (adapter->records[j - 1].block == first + i) {
        if (!log_read(adapter, j - 1, bytes + i * PFS_BLOCK_SIZE)) {
          return end_event(adapter, event, infrastructure_error(adapter));
        }
        found = true;
        break;
      }
    }
    if (!found && test_failure_durable_read(adapter, first + i, 1,
                                            bytes + i * PFS_BLOCK_SIZE) != PFS_OK) {
      return end_event(adapter, event, infrastructure_error(adapter));
    }
  }
  return end_event(adapter, event, PFS_OK);
}

static enum pfs_status
torn_slot(struct test_failure *adapter, uint64_t block, const void *new_bytes)
{
  uint8_t *mixed = adapter->transfer;
  uint8_t *old = adapter->transfer + PFS_BLOCK_SIZE;
  if (test_failure_durable_read(adapter, block, 1, old) != PFS_OK) {
    return infrastructure_error(adapter);
  }
  memcpy(mixed, old, PFS_BLOCK_SIZE);
  memcpy(mixed, new_bytes, adapter->fault.prefix);
  if (!memcmp(mixed, old, PFS_BLOCK_SIZE) || !memcmp(mixed, new_bytes, PFS_BLOCK_SIZE)) {
    return infrastructure_error(adapter);
  }
  uint8_t checksum[4];
  memcpy(checksum, mixed + 20, sizeof(checksum));
  memset(mixed + 20, 0, sizeof(checksum));
  uint8_t calculated[4];
  test_put_u32(calculated, test_crc32c(mixed, PFS_BLOCK_SIZE));
  memcpy(mixed + 20, checksum, sizeof(checksum));
  if (!memcmp(checksum, calculated, sizeof(checksum)) ||
      append_pending(adapter, block, 1, mixed) != PFS_OK ||
      promote(adapter, SIZE_MAX, true, block) != PFS_OK) {
    return infrastructure_error(adapter);
  }
  return PFS_OK;
}

static enum pfs_status
write_blocks(void *context, uint64_t first, uint32_t count, const void *buffer)
{
  struct test_failure *adapter = context;
  struct test_failure_event *event = begin_event(adapter, TEST_FAILURE_WRITE, first, count);
  if (!event) {
    return PFS_IO;
  }
  enum pfs_status status = PFS_OK;
  if (event->selected) {
    if (adapter->fault.mode == TEST_FAILURE_BEFORE) {
      return end_event(adapter, event, PFS_IO);
    }
    if (adapter->fault.mode == TEST_FAILURE_TORN_SLOT) {
      if (count != 1 || (first != 0 && first != adapter->builder.reader.geometry.block_count - 1)) {
        status = infrastructure_error(adapter);
      } else {
        status = torn_slot(adapter, first, buffer);
      }
    } else if (adapter->fault.prefix > count) {
      status = infrastructure_error(adapter);
    } else {
      status = append_pending(adapter, first, (uint32_t)adapter->fault.prefix, buffer);
      if (status == PFS_OK && adapter->fault.mode == TEST_FAILURE_DURABLE_PREFIX) {
        for (size_t i = 0; i < adapter->fault.prefix && status == PFS_OK; ++i) {
          status = promote(adapter, SIZE_MAX, true, first + i);
        }
      }
    }
    return end_event(adapter, event, status == PFS_OK ? PFS_IO : status);
  }
  status = append_pending(adapter, first, count, buffer);
  if (status == PFS_OK && adapter->stable_writes) {
    status = promote(adapter, SIZE_MAX, false, 0);
  }
  return end_event(adapter, event, status);
}

static enum pfs_status
cache_only_slot(struct test_failure *adapter)
{
  bool found = false;
  for (size_t i = adapter->record_count; i; --i) {
    struct test_failure_log_record *record = &adapter->records[i - 1];
    if (record->block == adapter->fault.block) {
      if (record->state != LOG_PENDING) {
        return infrastructure_error(adapter);
      }
      record->state = LOG_CACHE_ONLY;
      uint8_t state[4];
      test_put_u32(state, LOG_CACHE_ONLY);
      if (!file_write(adapter->log, (uint64_t)(i - 1) * LOG_RECORD_SIZE + 12,
                       state, sizeof(state))) {
        return infrastructure_error(adapter);
      }
      found = true;
      /* Every earlier pending slot incarnation must also stop being pending. */
      for (size_t j = 0; j + 1 < i; ++j) {
        if (adapter->records[j].block == record->block) {
          adapter->records[j].state = LOG_REMOVE;
        }
      }
      break;
    }
  }
  if (!found) {
    return infrastructure_error(adapter);
  }
  return promote(adapter, SIZE_MAX, false, 0);
}

static enum pfs_status
flush_blocks(void *context)
{
  struct test_failure *adapter = context;
  struct test_failure_event *event = begin_event(adapter, TEST_FAILURE_FLUSH, 0, 0);
  if (!event) {
    return PFS_IO;
  }
  enum pfs_status status = PFS_OK;
  if (event->selected) {
    if (adapter->fault.mode == TEST_FAILURE_FLUSH_PREFIX) {
      status = promote(adapter, adapter->fault.prefix, false, 0);
    } else if (adapter->fault.mode == TEST_FAILURE_CACHE_ONLY_SLOT) {
      status = cache_only_slot(adapter);
    }
    return end_event(adapter, event, status == PFS_OK ? PFS_IO : status);
  }
  status = promote(adapter, SIZE_MAX, false, 0);
  return end_event(adapter, event, status);
}

static enum pfs_status
seed_copy(struct test_failure *adapter, const struct pfs_block_reader *seed)
{
  if (!seed) {
    return PFS_OK;
  }
  if (seed->geometry.block_count != adapter->builder.reader.geometry.block_count ||
      !seed->geometry.max_transfer_blocks) {
    return PFS_INVALID;
  }
  for (uint64_t first = 0; first < seed->geometry.block_count;) {
    uint32_t count = seed->geometry.max_transfer_blocks;
    if (count > PFS_IO_BLOCKS_MAX) {
      count = PFS_IO_BLOCKS_MAX;
    }
    if (count > seed->geometry.block_count - first) {
      count = (uint32_t)(seed->geometry.block_count - first);
    }
    if (pfs_block_read(seed, first, count, adapter->transfer,
                       (size_t)count * PFS_BLOCK_SIZE) != PFS_OK) {
      return infrastructure_error(adapter);
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *bytes = adapter->transfer + i * PFS_BLOCK_SIZE;
      bool nonzero = false;
      for (size_t j = 0; j < PFS_BLOCK_SIZE; ++j) {
        nonzero |= bytes[j] != 0;
      }
      if (nonzero && !file_write(adapter->backing.file, (first + i) * PFS_BLOCK_SIZE,
                                 bytes, PFS_BLOCK_SIZE)) {
        return infrastructure_error(adapter);
      }
    }
    first += count;
  }
  return fsync(fileno(adapter->backing.file)) == 0 ? PFS_OK : infrastructure_error(adapter);
}

enum pfs_status
test_failure_open(struct test_failure *adapter, uint64_t blocks, size_t dirty_capacity,
                  uint64_t memory_limit, const struct pfs_block_reader *seed)
{
  if (!adapter || adapter->backing.file || !dirty_capacity ||
      dirty_capacity > PFS_ALLOCATION_COUNT_MAX) {
    return PFS_INVALID;
  }
  *adapter = (struct test_failure){0};
  enum pfs_status status = test_fixture_open(&adapter->backing, blocks, memory_limit);
  if (status != PFS_OK) {
    return status;
  }
  adapter->memory = &adapter->backing.memory;
  adapter->log = test_temporary_file();
  adapter->records = calloc(dirty_capacity, sizeof(*adapter->records));
  adapter->dirty_capacity = dirty_capacity;
  if (!adapter->log) {
    status = PFS_IO;
  } else if (!adapter->records) {
    status = PFS_NO_MEMORY;
  } else {
    struct pfs_geometry geometry = {blocks, PFS_IO_BLOCKS_MAX};
    status = pfs_block_builder_init(&adapter->builder, adapter, &geometry,
                                    read_blocks, write_blocks, flush_blocks);
  }
  if (status == PFS_OK) {
    status = seed_copy(adapter, seed);
  }
  if (status != PFS_OK) {
    test_failure_close(adapter);
    return status;
  }
  adapter->next = adapters;
  adapters = adapter;
  return PFS_OK;
}

static enum pfs_status
durable_reader(void *context, uint64_t first, uint32_t count, void *buffer)
{
  return test_failure_durable_read(context, first, count, buffer);
}

enum pfs_status
test_failure_clone_durable(struct test_failure *destination,
                           const struct test_failure *source, uint64_t memory_limit)
{
  if (!source || !source->backing.file || destination == source) {
    return PFS_INVALID;
  }
  struct pfs_block_reader reader = source->builder.reader;
  reader.context = (void *)source;
  reader.read = durable_reader;
  return test_failure_open(destination, reader.geometry.block_count,
                            source->dirty_capacity, memory_limit, &reader);
}

enum pfs_status
test_failure_set_fault(struct test_failure *adapter, const struct test_failure_fault *fault)
{
  if (!adapter || !adapter->backing.file || !fault ||
      (fault->enabled && (!fault->ordinal || fault->kind < TEST_FAILURE_READ ||
                         fault->kind > TEST_FAILURE_FLUSH))) {
    return PFS_INVALID;
  }
  if (fault->enabled) {
    bool valid = fault->mode == TEST_FAILURE_BEFORE;
    if (fault->kind == TEST_FAILURE_WRITE) {
      valid |= fault->mode == TEST_FAILURE_PENDING_PREFIX ||
               fault->mode == TEST_FAILURE_DURABLE_PREFIX ||
               (fault->mode == TEST_FAILURE_TORN_SLOT &&
                (fault->prefix == 32 || fault->prefix == 64));
    } else if (fault->kind == TEST_FAILURE_FLUSH) {
      valid |= fault->mode == TEST_FAILURE_FLUSH_PREFIX ||
               (fault->mode == TEST_FAILURE_CACHE_ONLY_SLOT &&
                (fault->block == 0 || fault->block == adapter->builder.reader.geometry.block_count - 1));
    }
    if (!valid) {
      return PFS_INVALID;
    }
  }
  adapter->fault = *fault;
  adapter->triggered = false;
  return PFS_OK;
}

void
test_failure_memory_fail_after(struct test_failure *adapter, size_t successes)
{
  adapter->backing.fail_after = successes > SIZE_MAX - adapter->backing.allocation_calls ?
    SIZE_MAX : adapter->backing.allocation_calls + successes;
}

void
test_failure_trace_reset(struct test_failure *adapter)
{
  adapter->event_count = 0;
}

enum pfs_status
test_failure_promote_block(struct test_failure *adapter, uint64_t block)
{
  if (!adapter || !adapter->backing.file || block >= adapter->builder.reader.geometry.block_count) {
    return PFS_INVALID;
  }
  return promote(adapter, SIZE_MAX, true, block);
}

enum pfs_status
test_failure_cold_cut(struct test_failure *adapter)
{
  if (!adapter || !adapter->backing.file) {
    return PFS_INVALID;
  }
  struct test_failure_event *event = begin_event(adapter, TEST_FAILURE_CUT, 0, 0);
  if (!event) {
    return PFS_IO;
  }
  enum pfs_status status = PFS_OK;
  if (ftruncate(fileno(adapter->log), 0) != 0) {
    status = infrastructure_error(adapter);
  } else {
    adapter->record_count = 0;
    adapter->pending_count = 0;
  }
  return end_event(adapter, event, status);
}

bool
test_failure_close(struct test_failure *adapter)
{
  if (!adapter) {
    return false;
  }
  bool clean = !adapter->infrastructure_failure;
  if (adapter->log) {
    clean = fclose(adapter->log) == 0 && clean;
  }
  free(adapter->records);
  clean = test_fixture_close(&adapter->backing) && clean;
  struct test_failure **link = &adapters;
  while (*link && *link != adapter) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = adapter->next;
  }
  *adapter = (struct test_failure){0};
  return clean;
}

void
test_failures_cleanup(void)
{
  bool clean = true;
  while (adapters) {
    clean = test_failure_close(adapters) && clean;
  }
  if (!Unity.CurrentTestFailed) {
    TEST_ASSERT_TRUE_MESSAGE(clean, "failure adapter leaked memory or encountered infrastructure failure");
  }
}
