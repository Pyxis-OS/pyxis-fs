#include <pyxis_fs/check.h>

#include "check_internal.h"

static unsigned
check_priority(enum pfs_status status)
{
  switch (status) {
  case PFS_OK: return 0;
  case PFS_ABSENT: return 1;
  case PFS_UNSUPPORTED: return 2;
  case PFS_LIMIT: return 3;
  case PFS_CORRUPT: return 4;
  case PFS_NO_MEMORY: return 5;
  case PFS_IO: return 6;
  default: return 7;
  }
}

enum pfs_status
check_primary(enum pfs_status left, enum pfs_status right)
{
  return check_priority(left) >= check_priority(right) ? left : right;
}

void
check_problem(struct check_state *state, enum pfs_status status,
              const char *operation, uint64_t block,
              const struct pfs_volume_id *volume, const struct pfs_object_id *object)
{
  if (status == PFS_OK) {
    return;
  }
  state->result->status = check_primary(state->result->status, status);
  state->result->failures |= UINT32_C(1) << status;
  state->result->complete = false;
  if (state->report) {
    struct pfs_check_event event;
    pfs_bytes_zero(&event, sizeof(event));
    event.slot = state->slot;
    event.status = status;
    event.operation = operation;
    event.block = block;
    if (volume) {
      pfs_bytes_copy(&event.volume, volume, sizeof(event.volume));
    }
    if (object) {
      pfs_bytes_copy(&event.object, object, sizeof(event.object));
    }
    state->report(state->context, &event);
  }
}

enum pfs_status
check_grow(struct pfs_memory *memory, struct pfs_allocation *array,
           size_t count, size_t item_size, size_t alignment, size_t maximum)
{
  if (!item_size || count >= maximum || maximum > SIZE_MAX / item_size) {
    return PFS_LIMIT;
  }
  size_t capacity = array->size / item_size;
  if (count < capacity) {
    return PFS_OK;
  }
  size_t next = capacity ? (capacity > maximum / 2 ? maximum : capacity * 2) : 32;
  if (next > maximum) {
    next = maximum;
  }
  if (next <= count) {
    return PFS_LIMIT;
  }
  struct pfs_allocation replacement;
  pfs_bytes_zero(&replacement, sizeof(replacement));
  enum pfs_status status = pfs_memory_allocate(memory, next * item_size,
                                                alignment, &replacement);
  if (status != PFS_OK) {
    return status;
  }
  pfs_bytes_zero(replacement.data, replacement.size);
  if (count) {
    pfs_bytes_copy(replacement.data, array->data, count * item_size);
  }
  pfs_memory_free(memory, array);
  return pfs_memory_move(memory, &replacement, array);
}

enum pfs_status
check_add_claim(struct check_state *state, const struct check_claim *claim)
{
  enum pfs_status status = check_grow(state->memory, &state->claims,
    state->claim_count, sizeof(*claim), _Alignof(struct check_claim),
    CHECK_METADATA_MAX + PFS_RECORD_COUNT_MAX);
  if (status != PFS_OK) {
    return status;
  }
  struct check_claim *claims = state->claims.data;
  pfs_bytes_copy(&claims[state->claim_count++], claim, sizeof(*claim));
  if (claim->type) {
    state->result->metadata_blocks += claim->count;
  } else {
    state->result->file_blocks += claim->count;
  }
  state->claims_sorted = false;
  return PFS_OK;
}

struct check_volume *
check_find_volume(struct check_state *state, const struct pfs_volume_id *id)
{
  struct check_volume *volumes = state->volumes.data;
  size_t first = 0;
  size_t end = state->volume_count;
  while (first < end) {
    size_t middle = first + (end - first) / 2;
    int order = pfs_bytes_compare(id->bytes, PFS_ID_SIZE,
                                  volumes[middle].record.id.bytes, PFS_ID_SIZE);
    if (!order) {
      return &volumes[middle];
    }
    if (order < 0) {
      end = middle;
    } else {
      first = middle + 1;
    }
  }
  return NULL;
}

static void
state_release(struct check_state *state)
{
  struct check_volume *volumes = state->volumes.data;
  for (size_t i = 0; i < state->volume_count; ++i) {
    pfs_memory_free(state->memory, &volumes[i].objects);
  }
  pfs_memory_free(state->memory, &state->frames);
  pfs_memory_free(state->memory, &state->seen);
  pfs_memory_free(state->memory, &state->claims);
  pfs_memory_free(state->memory, &state->allocations);
  pfs_memory_free(state->memory, &state->names);
  pfs_memory_free(state->memory, &state->volumes);
  state->volume_count = 0;
  state->name_count = 0;
  state->allocation_count = 0;
  state->claim_count = 0;
  state->seen_count = 0;
}

static bool
state_complete(const struct check_state *state)
{
  if (state->result->failures || !state->catalog_complete ||
      !state->map_complete || !state->pool_complete) {
    return false;
  }
  const struct check_volume *volumes = state->volumes.data;
  for (size_t i = 0; i < state->volume_count; ++i) {
    if (!volumes[i].supported || !volumes[i].complete) {
      return false;
    }
  }
  return true;
}

enum pfs_status
pfs_check(const struct pfs_block_reader *reader, struct pfs_memory *memory,
          pfs_check_report_fn report, void *context, struct pfs_check_result *result)
{
  if (!reader || !reader->read || reader->geometry.block_count < 3 ||
      !reader->geometry.max_transfer_blocks || !memory || !memory->allocate ||
      !memory->free || !memory->limit || memory->limit > PFS_MEMORY_MAX ||
      memory->used > memory->limit || !result) {
    return PFS_INVALID;
  }
  struct pfs_check_result checked;
  pfs_bytes_zero(&checked, sizeof(checked));
  checked.pool.selected = PFS_POOL_NO_SELECTION;
  struct pfs_pool pool;
  pfs_bytes_zero(&pool, sizeof(pool));
  enum pfs_status opening = pfs_pool_open(&pool, reader, memory, &checked.pool);
  enum pfs_status status = check_primary(opening, pfs_pool_close(&pool));
  if (opening != PFS_OK && !checked.pool.ambiguous &&
      opening != checked.pool.candidate[0].status &&
      opening != checked.pool.candidate[1].status && report) {
    struct pfs_check_event event;
    pfs_bytes_zero(&event, sizeof(event));
    event.slot = PFS_POOL_NO_SELECTION;
    event.status = opening;
    event.operation = "open pool diagnostic workspace";
    event.block = UINT64_MAX;
    report(context, &event);
  }
  struct check_state states[2];
  pfs_bytes_zero(states, sizeof(states));
  bool retained[2] = {true, true};
  for (uint16_t i = 0; i < 2; ++i) {
    struct check_state *state = &states[i];
    state->reader = reader;
    state->memory = memory;
    state->candidate = &checked.pool.candidate[i];
    state->result = &checked.state[i];
    state->report = report;
    state->context = context;
    state->slot = i;
    enum pfs_status candidate_status = state->candidate->status;
    if (candidate_status != PFS_OK) {
      check_problem(state, candidate_status, "open candidate", i ?
        reader->geometry.block_count - 1 : 0, NULL, NULL);
      continue;
    }
    state->result->generation = state->candidate->superblock.header.birth;
    enum pfs_status features = pfs_features_check(&state->candidate->superblock.features);
    if (features != PFS_OK) {
      check_problem(state, features, "unsupported pool features; contents not checked",
                     i ? reader->geometry.block_count - 1 : 0, NULL, NULL);
      continue;
    }
    check_walk_state(state);
    check_reconcile_state(state);
    state->result->complete = state_complete(state);
    if (!state->result->complete && state->result->status == PFS_OK) {
      check_problem(state, PFS_ABSENT, "state check incomplete", UINT64_MAX, NULL, NULL);
    }
    /* Free a resource-exhausted partial traversal before attempting the peer.
     * Its diagnostics remain, but no cross-state proof can rely on those tables. */
    if (state->result->failures & ((UINT32_C(1) << PFS_LIMIT) |
                                   (UINT32_C(1) << PFS_NO_MEMORY))) {
      state_release(state);
      retained[i] = false;
    }
  }
  if (retained[0] && retained[1] &&
      checked.pool.candidate[0].status == PFS_OK &&
      checked.pool.candidate[1].status == PFS_OK && !checked.pool.ambiguous) {
    checked.cross_status = check_compare_states(&states[0], &states[1],
                                                 &checked.cross_complete);
  } else {
    checked.cross_status = checked.pool.ambiguous ? PFS_CORRUPT : PFS_ABSENT;
  }
  if (!checked.cross_complete && checked.cross_status == PFS_OK) {
    checked.cross_status = PFS_ABSENT;
  }
  status = check_primary(status, checked.cross_status);
  for (size_t i = 0; i < 2; ++i) {
    status = check_primary(status, checked.state[i].status);
    state_release(&states[i]);
  }
  pfs_bytes_copy(result, &checked, sizeof(checked));
  return status;
}
