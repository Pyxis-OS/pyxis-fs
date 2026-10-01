/* SPDX-License-Identifier: MPL-2.0 */
#include "writer.h"

bool
pfs_writer_active(const struct pfs_pool *pool)
{
  return pool && pool->writer_busy;
}

enum pfs_status
pfs_writer_lifetime_begin(struct pfs_pool *pool)
{
  if (!pool) {
    return PFS_INVALID;
  }
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  if (pool->writer) {
    pool->writer_busy = true;
  }
  return PFS_OK;
}

enum pfs_status
pfs_writer_begin(struct pfs_pool *pool, bool mutation)
{
  if (!pool) {
    return PFS_INVALID;
  }
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  if (!pool->writer) {
    return mutation ? PFS_READ_ONLY : PFS_OK;
  }
  enum pfs_writer_health health = pool->writer->status.health;
  if (health == PFS_WRITER_ACCESS_STOPPED || (mutation && health != PFS_WRITER_READY)) {
    return PFS_RECOVERY_REQUIRED;
  }
  pool->writer_busy = true;
  return PFS_OK;
}

void
pfs_writer_end(struct pfs_pool *pool, enum pfs_status status)
{
  if (pool && pool->writer) {
    if (status == PFS_CORRUPT || status == PFS_IO) {
      pool->writer->status.health = PFS_WRITER_ACCESS_STOPPED;
      pool->writer->status.failure = status;
    }
    pool->writer_busy = false;
  }
}

const struct pfs_pool_diagnostic *
pfs_writer_diagnostic(const struct pfs_pool *pool)
{
  return pool && pool->writer ? &pool->writer->diagnostic : NULL;
}

const struct pfs_volume_record *
pfs_writer_volume(const struct pfs_pool *pool, const struct pfs_volume_id *id, uint64_t *birth)
{
  if (!pool || !pool->writer || !id || !birth) {
    return NULL;
  }
  const struct pfs_writer *writer = pool->writer;
  const struct pfs_admit_state *state = &writer->states[writer->selected];
  for (size_t i = 0; i < state->volume_count; i++) {
    if (!pfs_bytes_compare(state->volumes[i].record.id.bytes, PFS_ID_SIZE, id->bytes, PFS_ID_SIZE)) {
      *birth = state->candidate.superblock.header.birth;
      return &state->volumes[i].record;
    }
  }
  return NULL;
}

enum pfs_status
pfs_pool_writer_status(const struct pfs_pool *pool, struct pfs_writer_status *out)
{
  if (!pool || !out || !pool->state.data) {
    return PFS_INVALID;
  }
  if (!pool->writer) {
    return PFS_READ_ONLY;
  }
  *out = pool->writer->status;
  return PFS_OK;
}

enum pfs_status
pfs_writer_checkpoint(struct pfs_pool *pool, struct pfs_write_result *result)
{
  if (!pool || !result || !pool->writer || !pool->writer_busy) {
    return PFS_INVALID;
  }
  *result = (struct pfs_write_result){.completion = PFS_COMPLETE,
    .health = pool->writer->status.health};
  if (pool->writer->status.health != PFS_WRITER_READY) {
    result->completion = PFS_STOPPED;
    result->operation_status = PFS_RECOVERY_REQUIRED;
    return PFS_RECOVERY_REQUIRED;
  }
  return PFS_OK;
}

void
pfs_writer_dispose(struct pfs_pool *pool)
{
  if (!pool || !pool->writer) {
    return;
  }
  struct pfs_writer *writer = pool->writer;
  pool->writer = NULL;
  if (writer->arena.allocation.data) {
    pfs_memory_free(pool->memory, &writer->arena.allocation);
  }
  struct pfs_allocation allocation = {0};
  pfs_memory_move(pool->memory, &writer->allocation, &allocation);
  pfs_memory_free(pool->memory, &allocation);
}

static struct pfs_runtime_object *
runtime_find(const struct pfs_pool *pool, const struct pfs_volume_id *volume,
             const struct pfs_object_id *object)
{
  if (!pool || !pool->writer || !volume || !object) {
    return NULL;
  }
  for (struct pfs_runtime_object *entry = pool->writer->runtime_objects;
       entry; entry = entry->next) {
    if (!pfs_bytes_compare(entry->volume.bytes, PFS_ID_SIZE, volume->bytes, PFS_ID_SIZE) &&
        !pfs_bytes_compare(entry->object.bytes, PFS_ID_SIZE, object->bytes, PFS_ID_SIZE)) {
      return entry;
    }
  }
  return NULL;
}

bool
pfs_runtime_references(const struct pfs_pool *pool, const struct pfs_volume_id *volume,
                       const struct pfs_object_id *object)
{
  const struct pfs_runtime_object *entry = runtime_find(pool, volume, object);
  return entry && (entry->views || entry->operations);
}

enum pfs_status
pfs_runtime_hold(struct pfs_pool *pool, const struct pfs_volume_id *volume,
                 const struct pfs_object_id *object, struct pfs_runtime_object **out)
{
  if (!pool || !pool->writer || !pool->writer_busy || !volume || !object ||
      !out || *out) {
    return PFS_INVALID;
  }
  struct pfs_runtime_object *entry = runtime_find(pool, volume, object);
  if (entry) {
    if (entry->views == SIZE_MAX) {
      return PFS_LIMIT;
    }
    ++entry->views;
    *out = entry;
    return PFS_OK;
  }
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(pool->memory, sizeof(*entry),
    _Alignof(struct pfs_runtime_object), &allocation);
  if (status != PFS_OK) {
    return status;
  }
  entry = allocation.data;
  *entry = (struct pfs_runtime_object){.volume = *volume, .object = *object, .views = 1};
  status = pfs_memory_move(pool->memory, &allocation, &entry->allocation);
  if (status != PFS_OK) {
    pfs_memory_free(pool->memory, &allocation);
    return status;
  }
  entry->next = pool->writer->runtime_objects;
  pool->writer->runtime_objects = entry;
  *out = entry;
  return PFS_OK;
}

void
pfs_runtime_drop(struct pfs_pool *pool, struct pfs_runtime_object *object)
{
  if (!pool || !pool->writer || !object || !object->views) {
    return;
  }
  --object->views;
  if (object->views || object->operations) {
    return;
  }
  struct pfs_runtime_object **link = &pool->writer->runtime_objects;
  while (*link && *link != object) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = object->next;
    struct pfs_allocation allocation = {0};
    pfs_memory_move(pool->memory, &object->allocation, &allocation);
    pfs_memory_free(pool->memory, &allocation);
  }
}
