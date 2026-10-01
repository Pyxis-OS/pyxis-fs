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
