/* SPDX-License-Identifier: MPL-2.0 */
#include "check_internal.h"

static bool
same_volume(const struct pfs_volume_id *first, const struct pfs_volume_id *second)
{
  return pfs_bytes_compare(first->bytes, PFS_ID_SIZE, second->bytes, PFS_ID_SIZE) == 0;
}

static bool
same_object(const struct pfs_object_id *first, const struct pfs_object_id *second)
{
  return pfs_bytes_compare(first->bytes, PFS_ID_SIZE, second->bytes, PFS_ID_SIZE) == 0;
}

static uint64_t
claim_end(const struct check_claim *claim)
{
  return claim->first + claim->count;
}

static uint64_t
allocation_end(const struct pfs_allocation_record *record)
{
  return record->first + record->count;
}

static bool
claim_after(const struct check_claim *first, const struct check_claim *second)
{
  return first->first > second->first ||
         (first->first == second->first && first->count > second->count);
}

static void
swap_claims(struct check_claim *first, struct check_claim *second)
{
  struct check_claim temporary;
  pfs_bytes_copy(&temporary, first, sizeof(temporary));
  pfs_bytes_copy(first, second, sizeof(*first));
  pfs_bytes_copy(second, &temporary, sizeof(*second));
}

static void
sift_claims(struct check_claim *claims, size_t root, size_t count)
{
  while (root < count / 2) {
    size_t child = 2 * root + 1;
    if (child + 1 < count && claim_after(&claims[child + 1], &claims[child])) {
      ++child;
    }
    if (!claim_after(&claims[child], &claims[root])) {
      return;
    }
    swap_claims(&claims[root], &claims[child]);
    root = child;
  }
}

static void
sort_claims(struct check_state *state)
{
  if (state->claims_sorted) {
    return;
  }
  struct check_claim *claims = state->claims.data;
  size_t count = state->claim_count;
  for (size_t i = count / 2; i > 0; --i) {
    sift_claims(claims, i - 1, count);
  }
  for (size_t i = count; i > 1; --i) {
    swap_claims(&claims[0], &claims[i - 1]);
    sift_claims(claims, 0, i - 1);
  }
  state->claims_sorted = true;
}

static bool
same_allocation(const struct pfs_allocation_record *first,
                const struct pfs_allocation_record *second)
{
  return first->state == second->state && first->charge == second->charge &&
         first->birth == second->birth && first->retirement == second->retirement &&
         same_volume(&first->owner, &second->owner);
}

/* Incomplete maps can still prove contradictions in the records collected so
 * far. Only a complete map can prove that a missing interval is corrupt. */
static bool
check_map_order(struct check_state *state, bool diagnose)
{
  const struct pfs_allocation_record *records = state->allocations.data;
  uint64_t next = 1;
  bool ordered = true;
  for (size_t i = 0; i < state->allocation_count; ++i) {
    const struct pfs_allocation_record *record = &records[i];
    if (!pfs_allocatable_range(record->first, record->count,
                               state->candidate->superblock.block_count)) {
      if (diagnose) {
        check_problem(state, PFS_CORRUPT, "allocation range", record->first,
                      &record->owner, NULL);
      }
      ordered = false;
      continue;
    }
    if (record->first < next) {
      if (diagnose) {
        check_problem(state, PFS_CORRUPT, "allocation map overlap/order", record->first,
                      &record->owner, NULL);
      }
      ordered = false;
    } else if (state->map_complete && record->first != next && diagnose) {
      check_problem(state, PFS_CORRUPT, "allocation map gap", next, NULL, NULL);
    }
    if (i && records[i - 1].first + records[i - 1].count == record->first &&
        same_allocation(&records[i - 1], record) && diagnose) {
      check_problem(state, PFS_CORRUPT, "allocation map not coalesced", record->first,
                    &record->owner, NULL);
    }
    next = allocation_end(record);
  }
  if (state->map_complete && next != state->candidate->superblock.block_count - 1 && diagnose) {
    check_problem(state, PFS_CORRUPT, "allocation map final gap", next, NULL, NULL);
  }
  return ordered;
}

static void
check_claim_overlap(struct check_state *state)
{
  const struct check_claim *claims = state->claims.data;
  uint64_t end = 0;
  for (size_t i = 0; i < state->claim_count; ++i) {
    const struct check_claim *claim = &claims[i];
    if (claim->first < end) {
      check_problem(state, PFS_CORRUPT, "conflicting physical claims", claim->first,
                    &claim->volume, &claim->object);
    }
    if (claim_end(claim) > end) {
      end = claim_end(claim);
    }
  }
}

static size_t
allocation_at_or_after(const struct check_state *state, uint64_t block)
{
  const struct pfs_allocation_record *records = state->allocations.data;
  size_t first = 0;
  size_t last = state->allocation_count;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    if (allocation_end(&records[middle]) <= block) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first;
}

static void
check_claim_allocations(struct check_state *state)
{
  const struct check_claim *claims = state->claims.data;
  const struct pfs_allocation_record *records = state->allocations.data;
  for (size_t i = 0; i < state->claim_count; ++i) {
    const struct check_claim *claim = &claims[i];
    size_t index = allocation_at_or_after(state, claim->first);
    uint64_t first = claim->first;
    uint64_t end = claim_end(claim);
    while (first < end) {
      if (index == state->allocation_count || records[index].first > first) {
        if (state->map_complete) {
          check_problem(state, PFS_CORRUPT, "claim missing allocation", first,
                        &claim->volume, &claim->object);
        }
        if (index == state->allocation_count || records[index].first >= end) {
          break;
        }
        first = records[index].first;
      }
      const struct pfs_allocation_record *record = &records[index];
      uint8_t expected = pfs_bytes_are_zero(claim->volume.bytes, PFS_ID_SIZE) ?
                         PFS_ALLOCATION_POOL : PFS_ALLOCATION_VOLUME;
      if (record->state != expected || record->birth != claim->birth ||
          !same_volume(&record->owner, &claim->volume)) {
        check_problem(state, PFS_CORRUPT, "claim allocation state/owner/birth", first,
                      &claim->volume, &claim->object);
      }
      first = allocation_end(record) < end ? allocation_end(record) : end;
      ++index;
    }
  }
}

static bool
owner_complete(struct check_state *state, const struct pfs_allocation_record *record)
{
  if (pfs_features_check(&state->candidate->superblock.features) != PFS_OK) {
    return false;
  }
  if (record->state == PFS_ALLOCATION_POOL) {
    return state->pool_complete;
  }
  struct check_volume *volume = check_find_volume(state, &record->owner);
  return volume && volume->supported && volume->complete;
}

static void
check_live_coverage(struct check_state *state)
{
  const struct pfs_allocation_record *records = state->allocations.data;
  const struct check_claim *claims = state->claims.data;
  size_t index = 0;
  for (size_t i = 0; i < state->allocation_count; ++i) {
    const struct pfs_allocation_record *record = &records[i];
    uint64_t first = record->first;
    uint64_t end = allocation_end(record);
    while (index < state->claim_count && claim_end(&claims[index]) <= first) {
      ++index;
    }
    if ((record->state != PFS_ALLOCATION_POOL && record->state != PFS_ALLOCATION_VOLUME) ||
        !owner_complete(state, record)) {
      continue;
    }
    size_t cursor = index;
    while (first < end) {
      while (cursor < state->claim_count && claim_end(&claims[cursor]) <= first) {
        ++cursor;
      }
      if (cursor == state->claim_count || claims[cursor].first > first) {
        check_problem(state, PFS_CORRUPT, "unexplained live allocation", first,
                      &record->owner, NULL);
        if (cursor == state->claim_count || claims[cursor].first >= end) {
          break;
        }
        first = claims[cursor].first;
      }
      uint64_t claim_limit = claim_end(&claims[cursor]);
      first = claim_limit < end ? claim_limit : end;
    }
  }
}

static bool
add_count(struct check_state *state, uint64_t *count, uint64_t value,
          const char *operation, uint64_t block, const struct pfs_volume_id *volume)
{
  if (value > UINT64_MAX - *count) {
    check_problem(state, PFS_CORRUPT, operation, block, volume, NULL);
    return false;
  }
  *count += value;
  return true;
}

static void
check_allocation_owners(struct check_state *state)
{
  const struct pfs_allocation_record *records = state->allocations.data;
  for (size_t i = 0; i < state->allocation_count; ++i) {
    const struct pfs_allocation_record *record = &records[i];
    if (!pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE) &&
        state->catalog_complete && !check_find_volume(state, &record->owner)) {
      check_problem(state, PFS_CORRUPT, "allocation owner absent from catalog", record->first,
                    &record->owner, NULL);
    }
  }
}

static void
check_accounting(struct check_state *state)
{
  const struct pfs_allocation_record *records = state->allocations.data;
  const struct pfs_pool_root *root = &state->candidate->root;
  struct check_volume *volumes = state->volumes.data;
  uint64_t totals[4] = {0};
  uint64_t charges[4] = {0};
  uint64_t occupied = 0;
  bool valid = true;
  for (size_t i = 0; i < state->volume_count; ++i) {
    volumes[i].live = 0;
    volumes[i].retired = 0;
    volumes[i].permanent = 0;
  }
  for (size_t i = 0; i < state->allocation_count; ++i) {
    const struct pfs_allocation_record *record = &records[i];
    if (record->state > PFS_ALLOCATION_RETIRED || record->charge > PFS_CHARGE_RECOVERY) {
      check_problem(state, PFS_CORRUPT, "allocation accounting discriminator", record->first,
                    &record->owner, NULL);
      valid = false;
      continue;
    }
    valid &= add_count(state, &totals[record->state], record->count,
                       "allocation totals overflow", record->first, &record->owner);
    if (record->state != PFS_ALLOCATION_FREE) {
      valid &= add_count(state, &occupied, record->count,
                         "occupied count overflow", record->first, &record->owner);
      valid &= add_count(state, &charges[record->charge], record->count,
                         "budget count overflow", record->first, &record->owner);
    }
    struct check_volume *volume = check_find_volume(state, &record->owner);
    if (!volume) {
      continue;
    }
    if (record->state == PFS_ALLOCATION_VOLUME) {
      valid &= add_count(state, &volume->live, record->count,
                         "volume live count overflow", record->first, &record->owner);
      if (record->charge == PFS_CHARGE_PERMANENT) {
        valid &= add_count(state, &volume->permanent, record->count,
                           "volume permanent count overflow", record->first, &record->owner);
      }
    } else if (record->state == PFS_ALLOCATION_RETIRED) {
      valid &= add_count(state, &volume->retired, record->count,
                         "volume retired count overflow", record->first, &record->owner);
    }
  }
  const uint64_t cached[] = {root->free, root->live_pool, root->live_volume, root->retired};
  for (size_t i = 0; i < 4; ++i) {
    if (totals[i] != cached[i]) {
      check_problem(state, PFS_CORRUPT, "pool allocation cached total", root->header.block,
                    NULL, NULL);
    }
  }
  const struct pfs_budget budgets[] = {root->cow, root->migration, root->recovery};
  uint64_t promised = occupied;
  for (size_t i = 0; i < 3; ++i) {
    if (charges[i + 1] != budgets[i].occupied) {
      check_problem(state, PFS_CORRUPT, "workspace cached occupancy", root->header.block,
                    NULL, NULL);
    }
    if (charges[i + 1] > budgets[i].capacity) {
      check_problem(state, PFS_CORRUPT, "workspace budget exceeded", root->header.block,
                    NULL, NULL);
    } else {
      valid &= add_count(state, &promised, budgets[i].capacity - charges[i + 1],
                         "capacity promises overflow", root->header.block, NULL);
    }
  }
  for (size_t i = 0; i < state->volume_count; ++i) {
    const struct check_volume *volume = &volumes[i];
    if (volume->live != volume->record.live_blocks ||
        volume->retired != volume->record.retired_blocks) {
      check_problem(state, PFS_CORRUPT, "volume cached allocation counts", UINT64_MAX,
                    &volume->record.id, NULL);
    }
    if (volume->permanent > volume->record.quota ||
        volume->record.guarantee > volume->record.quota) {
      check_problem(state, PFS_CORRUPT, "volume quota exceeded", UINT64_MAX,
                    &volume->record.id, NULL);
    }
    if (volume->record.guarantee > volume->permanent) {
      valid &= add_count(state, &promised, volume->record.guarantee - volume->permanent,
                         "capacity promises overflow", UINT64_MAX, &volume->record.id);
    }
  }
  if (valid && promised > state->candidate->superblock.block_count - 2) {
    check_problem(state, PFS_CORRUPT, "capacity guarantees/workspaces exceeded",
                  root->header.block, NULL, NULL);
  }
}

void
check_reconcile_state(struct check_state *state)
{
  sort_claims(state);
  check_claim_overlap(state);
  bool ordered = check_map_order(state, true);
  check_allocation_owners(state);
  if (ordered) {
    check_claim_allocations(state);
    if (state->map_complete) {
      check_live_coverage(state);
    }
  }
  if (state->map_complete) {
    check_accounting(state);
  }
}

static bool
state_complete(const struct check_state *state)
{
  if (state->result->failures || !state->map_complete || !state->pool_complete ||
      !state->catalog_complete) {
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

static bool
compatible_claims(const struct check_claim *first, const struct check_claim *second,
                  uint64_t block)
{
  if (first->type != second->type || first->birth != second->birth ||
      !same_volume(&first->volume, &second->volume) ||
      !same_object(&first->object, &second->object)) {
    return false;
  }
  if (first->type != 0) {
    return first->kind == second->kind && first->level == second->level;
  }
  return first->logical + (block - first->first) ==
         second->logical + (block - second->first);
}

static void
compare_claims(struct check_state *diagnostic, const struct check_state *first,
               const struct check_state *second)
{
  const struct check_claim *left = first->claims.data;
  const struct check_claim *right = second->claims.data;
  size_t i = 0;
  size_t j = 0;
  while (i < first->claim_count && j < second->claim_count) {
    uint64_t left_end = claim_end(&left[i]);
    uint64_t right_end = claim_end(&right[j]);
    uint64_t block = left[i].first > right[j].first ? left[i].first : right[j].first;
    if (block < left_end && block < right_end &&
        !compatible_claims(&left[i], &right[j], block)) {
      check_problem(diagnostic, PFS_CORRUPT, "retained live claim identity/interpretation",
                    block, &left[i].volume, &left[i].object);
    }
    if (left_end <= right_end) {
      ++i;
    }
    if (right_end <= left_end) {
      ++j;
    }
  }
}

static void
compare_claim_protection(struct check_state *diagnostic, const struct check_state *older,
                         const struct check_state *newer)
{
  const struct check_claim *claims = older->claims.data;
  const struct pfs_allocation_record *records = newer->allocations.data;
  uint64_t generation = older->candidate->superblock.header.birth;
  bool equal_generation = generation == newer->candidate->superblock.header.birth;
  for (size_t i = 0; i < older->claim_count; ++i) {
    const struct check_claim *claim = &claims[i];
    size_t index = allocation_at_or_after(newer, claim->first);
    uint64_t first = claim->first;
    uint64_t end = claim_end(claim);
    while (first < end) {
      if (index == newer->allocation_count || records[index].first > first) {
        if (newer->map_complete) {
          check_problem(diagnostic, PFS_CORRUPT, "retained claim missing protection", first,
                        &claim->volume, &claim->object);
        }
        if (index == newer->allocation_count || records[index].first >= end) {
          break;
        }
        first = records[index].first;
      }
      const struct pfs_allocation_record *record = &records[index];
      uint8_t expected = pfs_bytes_are_zero(claim->volume.bytes, PFS_ID_SIZE) ?
                         PFS_ALLOCATION_POOL : PFS_ALLOCATION_VOLUME;
      bool live = record->state == expected;
      bool retired = !equal_generation && record->state == PFS_ALLOCATION_RETIRED &&
                     record->retirement > generation;
      if ((!live && !retired) || record->birth != claim->birth ||
          !same_volume(&record->owner, &claim->volume)) {
        check_problem(diagnostic, PFS_CORRUPT, "retained live claim lost protection", first,
                      &claim->volume, &claim->object);
      }
      first = allocation_end(record) < end ? allocation_end(record) : end;
      ++index;
    }
  }
}

static void
compare_allocations(struct check_state *diagnostic, const struct check_state *older,
                    const struct check_state *newer)
{
  const struct pfs_allocation_record *left = older->allocations.data;
  const struct pfs_allocation_record *right = newer->allocations.data;
  uint64_t generation = older->candidate->superblock.header.birth;
  bool equal_generation = generation == newer->candidate->superblock.header.birth;
  size_t i = 0;
  size_t j = 0;
  while (i < older->allocation_count && j < newer->allocation_count) {
    uint64_t left_end = allocation_end(&left[i]);
    uint64_t right_end = allocation_end(&right[j]);
    uint64_t block = left[i].first > right[j].first ? left[i].first : right[j].first;
    if (block < left_end && block < right_end) {
      bool conflict = false;
      if (equal_generation) {
        conflict = !same_allocation(&left[i], &right[j]);
      } else if (left[i].state == PFS_ALLOCATION_POOL ||
                 left[i].state == PFS_ALLOCATION_VOLUME) {
        conflict = right[j].birth != left[i].birth ||
                   !same_volume(&left[i].owner, &right[j].owner) ||
                   (right[j].state != left[i].state &&
                    right[j].state != PFS_ALLOCATION_RETIRED) ||
                   (right[j].state == PFS_ALLOCATION_RETIRED &&
                    right[j].retirement <= generation);
      } else if (right[j].state != PFS_ALLOCATION_FREE) {
        if (left[i].state == PFS_ALLOCATION_RETIRED &&
            left[i].birth == right[j].birth) {
          conflict = !same_volume(&left[i].owner, &right[j].owner) ||
                     right[j].state != PFS_ALLOCATION_RETIRED ||
                     left[i].retirement != right[j].retirement;
        } else {
          conflict = right[j].birth <= generation;
        }
      }
      if (conflict) {
        check_problem(diagnostic, PFS_CORRUPT, "retained allocation incarnation/protection",
                      block, &left[i].owner, NULL);
      }
    }
    if (left_end <= right_end) {
      ++i;
    }
    if (right_end <= left_end) {
      ++j;
    }
  }
}

enum pfs_status
check_compare_states(struct check_state *first, struct check_state *second,
                     bool *complete)
{
  struct pfs_check_state_result result;
  struct check_state diagnostic;
  pfs_bytes_zero(&result, sizeof(result));
  pfs_bytes_zero(&diagnostic, sizeof(diagnostic));
  diagnostic.result = &result;
  diagnostic.report = first->report;
  diagnostic.context = first->context;
  diagnostic.slot = PFS_POOL_NO_SELECTION;
  sort_claims(first);
  sort_claims(second);
  compare_claims(&diagnostic, first, second);
  bool maps_ordered = check_map_order(first, false) && check_map_order(second, false);
  if (maps_ordered) {
    if (first->candidate->superblock.header.birth <= second->candidate->superblock.header.birth) {
      compare_allocations(&diagnostic, first, second);
      compare_claim_protection(&diagnostic, first, second);
    } else {
      compare_allocations(&diagnostic, second, first);
      compare_claim_protection(&diagnostic, second, first);
    }
  }
  *complete = state_complete(first) && state_complete(second) && maps_ordered &&
              result.failures == 0;
  return result.status;
}
