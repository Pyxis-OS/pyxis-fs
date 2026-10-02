/* SPDX-License-Identifier: MPL-2.0 */
#include "admit.h"
#include "canonical.h"

_Static_assert(sizeof(struct check_claim) <= 96, "admission claim slot");
_Static_assert(2 * PFS_VOLUME_MAX * sizeof(struct check_volume) <= PFS_PLAN_SCRATCH_BYTES,
               "admission reconciliation scratch");

static bool
same_id(const void *first, const void *second)
{
  return pfs_bytes_compare(first, PFS_ID_SIZE, second, PFS_ID_SIZE) == 0;
}

static struct pfs_admit_volume *
find_volume(struct pfs_admit_state *state, const struct pfs_volume_id *id)
{
  size_t first = 0, last = state->volume_count;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    int order = pfs_bytes_compare(id->bytes, PFS_ID_SIZE,
      state->volumes[middle].record.id.bytes, PFS_ID_SIZE);
    if (!order) {
      return &state->volumes[middle];
    }
    if (order < 0) {
      last = middle;
    } else {
      first = middle + 1;
    }
  }
  return NULL;
}

static bool
same_promises(const struct pfs_admit_state *first, const struct pfs_admit_state *second)
{
  const struct pfs_pool_root *left = &first->candidate.root;
  const struct pfs_pool_root *right = &second->candidate.root;
  if (first->volume_count != second->volume_count ||
      first->candidate.superblock.block_count != second->candidate.superblock.block_count ||
      !same_id(first->candidate.superblock.header.pool.bytes,
               second->candidate.superblock.header.pool.bytes) ||
      left->cow.capacity != right->cow.capacity ||
      left->migration.capacity != right->migration.capacity ||
      left->recovery.capacity != right->recovery.capacity) {
    return false;
  }
  for (size_t i = 0; i < first->volume_count; i++) {
    const struct pfs_volume_record *a = &first->volumes[i].record;
    const struct pfs_volume_record *b = &second->volumes[i].record;
    if (!same_id(a->id.bytes, b->id.bytes) ||
        pfs_bytes_compare(a->name.bytes, a->name.length, b->name.bytes, b->name.length) ||
        !same_id(a->root_object.bytes, b->root_object.bytes) ||
        a->quota != b->quota || a->guarantee != b->guarantee ||
        ((a->features.read_required ^ b->features.read_required) & ~PFS_FEATURE_ORPHANS) ||
        a->features.write_required != b->features.write_required ||
        a->features.optional != b->features.optional) {
      return false;
    }
  }
  return true;
}

static enum pfs_status
canonical_state(const struct check_state *state)
{
  uint8_t data[PFS_BLOCK_SIZE];
  const struct pfs_pool_candidate *candidate = state->candidate;
  uint64_t slot = state->slot ? state->reader->geometry.block_count - 1 : 0;
  enum pfs_status status = pfs_block_read(state->reader, slot, 1, data, sizeof(data));
  struct pfs_superblock superblock;
  if (status == PFS_OK) {
    status = pfs_canonical_superblock_validate(data, sizeof(data),
      state->reader->geometry.block_count, slot, &superblock);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_block_context block_context = {
    .block_count = superblock.block_count,
    .selected_generation = superblock.header.birth,
    .referring_birth = superblock.header.birth,
    .pool = superblock.header.pool,
    .reference = superblock.root,
    .features = superblock.features,
  };
  struct pfs_pool_root root;
  status = pfs_block_read(state->reader, superblock.root.block, 1, data, sizeof(data));
  if (status == PFS_OK) {
    status = pfs_canonical_pool_root_validate(data, sizeof(data), &block_context, &root);
  }
  if (status != PFS_OK) {
    return status;
  }
  const struct check_claim *claims = state->claims.data;
  const struct check_volume *volumes = state->volumes.data;
  for (size_t i = 0; i < state->claim_count; i++) {
    const struct check_claim *claim = &claims[i];
    if (claim->type != PFS_BLOCK_TREE) {
      continue;
    }
    struct pfs_tree_context context = {
      .block = {
        .block_count = candidate->superblock.block_count,
        .selected_generation = candidate->superblock.header.birth,
        .referring_birth = candidate->superblock.header.birth,
        .pool = candidate->superblock.header.pool,
        .reference = {claim->first, claim->birth, claim->type, PFS_FORMAT_VERSION},
        .features = candidate->superblock.features,
      },
      .kind = claim->kind, .volume = claim->volume, .object = claim->object,
    };
    bool namespace_root = false;
    if (!pfs_bytes_are_zero(claim->volume.bytes, PFS_ID_SIZE)) {
      for (size_t v = 0; v < state->volume_count; v++) {
        if (!same_id(volumes[v].record.id.bytes, claim->volume.bytes)) {
          continue;
        }
        context.block.features = volumes[v].record.features;
        namespace_root = claim->kind == PFS_INDEX_ORPHANS &&
          volumes[v].record.orphan_root.block == claim->first;
        const struct check_object *objects = volumes[v].objects.data;
        size_t first = 0, last = volumes[v].object_count;
        while (claim->kind == PFS_INDEX_DIRECTORY && first < last) {
          size_t middle = first + (last - first) / 2;
          int order = pfs_bytes_compare(claim->object.bytes, PFS_ID_SIZE,
            objects[middle].record.id.bytes, PFS_ID_SIZE);
          if (!order) {
            namespace_root = objects[middle].record.tree_root.block == claim->first;
            break;
          }
          if (order < 0) {
            last = middle;
          } else {
            first = middle + 1;
          }
        }
        break;
      }
    }
    struct pfs_tree tree;
    status = pfs_block_read(state->reader, claim->first, 1, data, sizeof(data));
    if (status == PFS_OK) {
      status = pfs_canonical_tree_validate(data, sizeof(data), &context, &tree);
    }
    if (status != PFS_OK) {
      return status;
    }
    if ((claim->kind == PFS_INDEX_DIRECTORY || claim->kind == PFS_INDEX_ORPHANS) &&
        !namespace_root && tree.count < 6) {
      return PFS_LIMIT;
    }
  }
  return PFS_OK;
}

static void
copy_state(const struct check_state *source, struct pfs_admit_state *out,
           struct pfs_plan_arena *arena, size_t slot)
{
  out->candidate = *source->candidate;
  out->maps = arena->maps[slot];
  out->map_count = source->allocation_count;
  out->claims = (struct check_claim *)arena->claims[slot];
  out->claim_count = source->claim_count;
  out->volume_count = source->volume_count;
  pfs_bytes_copy(out->maps, source->allocations.data, out->map_count * sizeof(*out->maps));
  pfs_bytes_copy(out->claims, source->claims.data, out->claim_count * sizeof(*out->claims));
  const struct check_volume *volumes = source->volumes.data;
  for (size_t i = 0; i < out->volume_count; i++) {
    struct pfs_admit_volume *volume = &out->volumes[i];
    volume->record = volumes[i].record;
    const struct check_object *objects = volumes[i].objects.data;
    for (size_t j = 0; j < volumes[i].object_count; j++) {
      volume->directories += objects[j].record.kind == PFS_OBJECT_DIRECTORY;
      volume->grants += objects[j].grants;
      if (objects[j].orphans) {
        volume->orphan_work += 1 + objects[j].grants;
      }
    }
  }
  for (size_t i = 0; i < out->claim_count; i++) {
    const struct check_claim *claim = &out->claims[i];
    struct pfs_admit_volume *volume = find_volume(out, &claim->volume);
    if (!volume) {
      out->live_pool += claim->count;
      out->map_nodes += claim->kind == PFS_INDEX_ALLOCATION;
    } else if (!claim->type) {
      volume->file_extents++;
      out->file_extents++;
      const struct check_volume *checked = &volumes[volume - out->volumes];
      const struct check_object *objects = checked->objects.data;
      size_t first = 0, last = checked->object_count;
      while (first < last) {
        size_t middle = first + (last - first) / 2;
        int order = pfs_bytes_compare(claim->object.bytes, PFS_ID_SIZE,
          objects[middle].record.id.bytes, PFS_ID_SIZE);
        if (!order) {
          if (objects[middle].orphans) {
            volume->orphan_work += claim->count;
          }
          break;
        }
        if (order < 0) {
          last = middle;
        } else {
          first = middle + 1;
        }
      }
    } else {
      volume->metadata_blocks += claim->count;
      out->metadata_blocks += claim->count;
      if (claim->kind == PFS_INDEX_DIRECTORY || claim->kind == PFS_INDEX_ORPHANS) {
        volume->namespace_nodes += claim->count;
      }
    }
  }
}

static void
protected_retirements(struct pfs_admit_state *state, const struct pfs_admit_state *older)
{
  state->protected_pool = 0;
  state->protected_volume = 0;
  size_t j = 0;
  for (size_t i = 0; i < state->map_count; i++) {
    const struct pfs_allocation_record *record = &state->maps[i];
    uint64_t end = record->first + record->count;
    while (j < older->claim_count &&
           older->claims[j].first + older->claims[j].count <= record->first) {
      j++;
    }
    if (record->state != PFS_ALLOCATION_RETIRED) {
      continue;
    }
    for (size_t k = j; k < older->claim_count && older->claims[k].first < end; k++) {
      const struct check_claim *claim = &older->claims[k];
      uint64_t first = claim->first > record->first ? claim->first : record->first;
      uint64_t limit = claim->first + claim->count;
      if (limit > end) {
        limit = end;
      }
      if (same_id(record->owner.bytes, claim->volume.bytes) && record->birth == claim->birth) {
        if (pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE)) {
          state->protected_pool += limit - first;
        } else {
          state->protected_volume += limit - first;
        }
      }
    }
  }
}

static enum pfs_status
reusable(const struct pfs_admit_state *first, const struct pfs_admit_state *second,
         struct pfs_reusable_range *ranges, size_t capacity, size_t *count,
         uint64_t *blocks)
{
  size_t i = 0, j = 0, produced = 0;
  uint64_t total = 0;
  bool first_selected = first->candidate.superblock.header.birth >=
                        second->candidate.superblock.header.birth;
  while (i < first->map_count && j < second->map_count) {
    const struct pfs_allocation_record *a = &first->maps[i];
    const struct pfs_allocation_record *b = &second->maps[j];
    uint64_t a_end = a->first + a->count, b_end = b->first + b->count;
    uint64_t begin = a->first > b->first ? a->first : b->first;
    uint64_t end = a_end < b_end ? a_end : b_end;
    const struct pfs_allocation_record *selected = first_selected ? a : b;
    const struct pfs_allocation_record *older = first_selected ? b : a;
    if (begin < end && selected->state == PFS_ALLOCATION_FREE &&
        (older->state == PFS_ALLOCATION_FREE || older->state == PFS_ALLOCATION_RETIRED)) {
      const struct pfs_admit_state *states[] = {first, second};
      for (size_t s = 0; s < 2; s++) {
        const struct pfs_admit_state *state = states[s];
        size_t lo = 0, hi = state->claim_count;
        while (lo < hi) {
          size_t mid = lo + (hi - lo) / 2;
          if (state->claims[mid].first + state->claims[mid].count <= begin) {
            lo = mid + 1;
          } else {
            hi = mid;
          }
        }
        if (lo < state->claim_count && state->claims[lo].first < end) {
          return PFS_CORRUPT;
        }
      }
      if (ranges) {
        if (produced == capacity) {
          return PFS_LIMIT;
        }
        ranges[produced] = (struct pfs_reusable_range){begin, end - begin};
      }
      produced++;
      total += end - begin;
    }
    if (a_end <= b_end) {
      i++;
    }
    if (b_end <= a_end) {
      j++;
    }
  }
  *count = produced;
  *blocks = total;
  return PFS_OK;
}

enum pfs_status
pfs_admit_reusable(const struct pfs_admit_state *first,
                  const struct pfs_admit_state *second,
                  struct pfs_reusable_range *ranges, size_t capacity, size_t *count)
{
  if (!first || !second || !ranges || !capacity || !count ||
      !first->maps || !second->maps || !first->map_count || !second->map_count ||
      (first->claim_count && !first->claims) || (second->claim_count && !second->claims)) {
    return PFS_INVALID;
  }
  uint64_t blocks;
  size_t produced;
  enum pfs_status status = reusable(first, second, ranges, capacity, &produced, &blocks);
  if (status == PFS_OK) {
    *count = produced;
  }
  return status;
}

static enum pfs_status
admit_limits(struct pfs_plan_arena *arena, struct pfs_admit_state *state,
             const struct pfs_admit_state *older, bool normal_batch)
{
  const struct pfs_plan_limits *limits = &arena->limits;
  const struct pfs_pool_root *root = &state->candidate.root;
  if (!same_promises(state, older)) {
    return PFS_UNSUPPORTED;
  }
  if (root->cow.capacity < 1024 || root->migration.capacity < 1024 ||
      root->recovery.capacity < limits->recovery_blocks) {
    return PFS_NO_SPACE;
  }
  if (root->migration.occupied) {
    return PFS_UNSUPPORTED;
  }
  if (state->map_count > limits->records || state->claim_count > limits->claims ||
      state->file_extents > limits->extents || state->metadata_blocks > limits->metadata ||
      state->map_nodes > limits->pool_blocks - limits->catalog_union - 1 ||
      state->live_pool > limits->permanent_pool) {
    return PFS_LIMIT;
  }
  state->retired_pool = 0;
  state->retired_volume = 0;
  uint64_t charges[4] = {0};
  struct pfs_volume_id retired_owners[2] = {0};
  size_t retired_owner_count = 0;
  struct {
    struct pfs_volume_id owner;
    uint64_t blocks;
    uint8_t charge;
  } cohorts[2] = {0};
  uint64_t generation = state->candidate.superblock.header.birth;
  for (size_t i = 0; i < state->map_count; i++) {
    const struct pfs_allocation_record *record = &state->maps[i];
    if (record->state != PFS_ALLOCATION_FREE) {
      charges[record->charge] += record->count;
    }
    if ((record->state == PFS_ALLOCATION_POOL || record->state == PFS_ALLOCATION_VOLUME) &&
        record->charge != PFS_CHARGE_PERMANENT) {
      return PFS_UNSUPPORTED;
    }
    if (record->state != PFS_ALLOCATION_RETIRED) {
      continue;
    }
    if (pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE)) {
      if (record->charge != PFS_CHARGE_RECOVERY) {
        return PFS_UNSUPPORTED;
      }
      state->retired_pool += record->count;
    } else {
      if (record->charge != PFS_CHARGE_ORDINARY && record->charge != PFS_CHARGE_RECOVERY) {
        return PFS_UNSUPPORTED;
      }
      size_t owner = 0;
      while (owner < retired_owner_count &&
             !same_id(retired_owners[owner].bytes, record->owner.bytes)) {
        owner++;
      }
      if (owner == retired_owner_count) {
        if (retired_owner_count == 2) {
          return PFS_LIMIT;
        }
        retired_owners[retired_owner_count++] = record->owner;
      }
      if (normal_batch) {
        size_t cohort;
        if (record->retirement == generation) {
          cohort = 0;
        } else if (record->retirement == generation - 1) {
          cohort = 1;
        } else {
          return PFS_LIMIT;
        }
        if (cohorts[cohort].blocks &&
            (!same_id(cohorts[cohort].owner.bytes, record->owner.bytes) ||
             cohorts[cohort].charge != record->charge)) {
          return PFS_LIMIT;
        }
        cohorts[cohort].owner = record->owner;
        cohorts[cohort].charge = record->charge;
        cohorts[cohort].blocks += record->count;
        if (cohorts[cohort].blocks > PFS_PLAN_VOLUME_RETIRED) {
          return PFS_LIMIT;
        }
      }
      state->retired_volume += record->count;
    }
  }
  if (charges[PFS_CHARGE_ORDINARY] > root->cow.capacity ||
      charges[PFS_CHARGE_RECOVERY] > root->recovery.capacity) {
    return PFS_NO_SPACE;
  }
  if (charges[PFS_CHARGE_MIGRATION]) {
    return PFS_UNSUPPORTED;
  }
  protected_retirements(state, older);
  if (state->retired_pool > 2 * limits->pool_blocks ||
      state->protected_pool > limits->pool_blocks ||
      state->retired_volume > 2 * PFS_PLAN_VOLUME_RETIRED ||
      state->protected_volume > (normal_batch ? PFS_PLAN_VOLUME_RETIRED :
                                               2 * PFS_PLAN_VOLUME_RETIRED)) {
    return PFS_LIMIT;
  }
  uint64_t permanent = limits->permanent_pool;
  uint64_t metadata = 0, objects = 0, extents = 0, orphan_work = 0;
  for (size_t i = 0; i < state->volume_count; i++) {
    struct pfs_admit_volume *volume = &state->volumes[i];
    uint64_t population = volume->record.object_count;
    if (!population || population > limits->objects ||
        volume->metadata_blocks > limits->metadata || volume->file_extents > limits->extents ||
        volume->grants > PFS_RECORD_COUNT_MAX ||
        !volume->directories || volume->directories > population ||
        volume->namespace_nodes > volume->metadata_blocks ||
        volume->namespace_nodes > volume->record.live_blocks) {
      return PFS_LIMIT;
    }
    uint64_t records = population - 1;
    uint64_t trees = volume->directories + 1;
    if (trees > records) {
      trees = records;
    }
    volume->deletion_blocks = (records + 4 * trees) / 5;
    if (volume->namespace_nodes > volume->deletion_blocks) {
      return PFS_LIMIT;
    }
    volume->effective_blocks = volume->record.live_blocks - volume->namespace_nodes +
                               volume->deletion_blocks;
    if (volume->effective_blocks > volume->record.quota) {
      return PFS_QUOTA;
    }
    permanent += volume->effective_blocks > volume->record.guarantee ?
                 volume->effective_blocks : volume->record.guarantee;
    metadata += volume->metadata_blocks - volume->namespace_nodes + volume->deletion_blocks;
    objects += population;
    extents += volume->file_extents;
    orphan_work += volume->orphan_work;
  }
  if (metadata > limits->metadata || objects > limits->objects ||
      extents != state->file_extents || extents > limits->extents) {
    return PFS_LIMIT;
  }
  permanent += root->cow.capacity + root->migration.capacity + root->recovery.capacity;
  if (permanent > state->candidate.superblock.block_count - 2) {
    return PFS_NO_SPACE;
  }
  uint64_t remaining = normal_batch ? 2 : state->retired_volume ?
                       (state->protected_volume ? 2 : 1) : 0;
  if (orphan_work > (UINT64_MAX - remaining) / 3 ||
      generation > UINT64_MAX - remaining - 3 * orphan_work) {
    return PFS_LIMIT;
  }
  size_t range_count;
  enum pfs_status status = reusable(state, older, NULL, 0, &range_count,
                                    &state->reusable_blocks);
  if (status != PFS_OK) {
    return status;
  }
  return state->reusable_blocks < limits->pool_blocks + PFS_PLAN_VOLUME_NEW ?
         PFS_NO_SPACE : PFS_OK;
}

struct opening {
  struct pfs_memory *memory;
  uint64_t extents;
  uint64_t metadata;
  struct pfs_plan_arena *arena;
  struct pfs_admit_state *states;
};

static enum pfs_status
capture_states(void *context, const struct check_state states[2])
{
  struct opening *opening = context;
  if (states[0].volume_count != states[1].volume_count) {
    return PFS_UNSUPPORTED;
  }
  struct pfs_plan_limits limits;
  enum pfs_status status = pfs_plan_limits(states[0].candidate->superblock.block_count,
    opening->extents, opening->metadata, (uint16_t)states[0].volume_count, &limits);
  if (status != PFS_OK) {
    return status;
  }
  for (size_t i = 0; i < 2; i++) {
    if (states[i].allocation_count > limits.records || states[i].claim_count > limits.claims) {
      return PFS_LIMIT;
    }
    status = canonical_state(&states[i]);
    if (status != PFS_OK) {
      return status;
    }
  }
  status = pfs_plan_arena_create(opening->memory, &limits, opening->arena);
  if (status != PFS_OK) {
    return status;
  }
  for (size_t i = 0; i < 2; i++) {
    copy_state(&states[i], &opening->states[i], opening->arena, i);
  }
  for (size_t i = 0; i < 2; i++) {
    status = admit_limits(opening->arena, &opening->states[i], &opening->states[1 - i], false);
    if (status != PFS_OK) {
      return status;
    }
  }
  opening->states[2].maps = opening->arena->maps[2];
  opening->states[2].claims = (struct check_claim *)opening->arena->claims[2];
  return PFS_OK;
}

enum pfs_status
pfs_admit_open(const struct pfs_block_reader *reader, struct pfs_memory *memory,
              uint64_t extents, uint64_t metadata, struct pfs_plan_arena *arena,
              struct pfs_admit_state states[3], struct pfs_check_result *result)
{
  if (!arena || arena->allocation.data || !states || !result ||
      extents > PFS_RECORD_COUNT_MAX || !metadata) {
    return PFS_INVALID;
  }
  pfs_bytes_zero(states, 3 * sizeof(*states));
  struct opening opening = {
    .memory = memory, .extents = extents, .metadata = metadata,
    .arena = arena, .states = states,
  };
  enum pfs_status status = check_capture(reader, memory, NULL, NULL,
                                         capture_states, &opening, result);
  if (status != PFS_OK) {
    pfs_plan_arena_destroy(arena);
    pfs_bytes_zero(states, 3 * sizeof(*states));
  }
  return status;
}

static void
borrow_state(struct check_state *out, const struct pfs_admit_state *source,
             struct check_volume *volumes, struct pfs_check_state_result *result)
{
  *out = (struct check_state){
    .candidate = &source->candidate, .result = result,
    .volume_count = source->volume_count,
    .allocation_count = source->map_count, .claim_count = source->claim_count,
    .catalog_complete = true, .map_complete = true, .pool_complete = true,
    .volumes = {.data = volumes, .size = source->volume_count * sizeof(*volumes)},
    .allocations = {.data = source->maps, .size = source->map_count * sizeof(*source->maps)},
    .claims = {.data = source->claims, .size = source->claim_count * sizeof(*source->claims)},
    .claims_sorted = true,
  };
  for (size_t i = 0; i < source->volume_count; i++) {
    volumes[i] = (struct check_volume){
      .record = source->volumes[i].record, .supported = true, .complete = true,
    };
  }
}

enum pfs_status
pfs_admit_candidate(struct pfs_plan_arena *arena, struct pfs_admit_state *state,
                    const struct pfs_admit_state *older, bool normal_batch)
{
  if (!arena || !arena->allocation.data || !state || !older || state == older ||
      !state->maps || !state->map_count || !state->claims || !state->claim_count ||
      !older->maps || !older->map_count || !older->claims || !older->claim_count ||
      !state->volume_count || state->volume_count > PFS_VOLUME_MAX ||
      !older->volume_count || older->volume_count > PFS_VOLUME_MAX ||
      state->maps == older->maps || state->claims == older->claims) {
    return PFS_INVALID;
  }
  uint64_t generation = older->candidate.superblock.header.birth;
  if (generation == UINT64_MAX || state->candidate.superblock.header.birth != generation + 1) {
    return PFS_INVALID;
  }
  if (state->map_count > arena->limits.records || state->claim_count > arena->limits.claims) {
    return PFS_LIMIT;
  }
  struct pfs_record_context context = {
    .block_count = state->candidate.superblock.block_count,
    .selected_generation = state->candidate.superblock.header.birth,
    .containing_birth = state->candidate.superblock.header.birth,
    .features = state->candidate.superblock.features,
  };
  for (size_t i = 0; i < state->map_count; i++) {
    enum pfs_status status = pfs_allocation_record_validate(&state->maps[i], &context);
    if (status != PFS_OK) {
      return status;
    }
  }
  uint64_t observed[PFS_VOLUME_MAX][3] = {0};
  uint64_t extents = 0, metadata = 0, pool = 0, map_nodes = 0;
  uint64_t previous_end = 0;
  bool root_claim = false;
  for (size_t i = 0; i < state->volume_count; i++) {
    enum pfs_status status = pfs_volume_record_validate(&state->volumes[i].record, &context);
    if (status != PFS_OK || (i && pfs_bytes_compare(state->volumes[i - 1].record.id.bytes,
        PFS_ID_SIZE, state->volumes[i].record.id.bytes, PFS_ID_SIZE) >= 0)) {
      return status == PFS_OK ? PFS_CORRUPT : status;
    }
  }
  for (size_t i = 0; i < state->claim_count; i++) {
    const struct check_claim *claim = &state->claims[i];
    if (!pfs_allocatable_range(claim->first, claim->count, context.block_count) ||
        !claim->birth || claim->birth > context.selected_generation ||
        claim->first < previous_end ||
        (claim->type && (claim->count != 1 ||
          (claim->type != PFS_BLOCK_POOL && claim->type != PFS_BLOCK_TREE)))) {
      return PFS_CORRUPT;
    }
    previous_end = claim->first + claim->count;
    struct pfs_admit_volume *volume = find_volume(state, &claim->volume);
    if (pfs_bytes_are_zero(claim->volume.bytes, PFS_ID_SIZE)) {
      if (!claim->type || !pfs_bytes_are_zero(claim->object.bytes, PFS_ID_SIZE) ||
          (claim->type == PFS_BLOCK_TREE &&
           (claim->kind < PFS_INDEX_VOLUMES || claim->kind > PFS_INDEX_ALLOCATION ||
            claim->level >= PFS_TREE_DEPTH_MAX)) ||
          (claim->type == PFS_BLOCK_POOL && (claim->kind || claim->level ||
            claim->first != state->candidate.root.header.block))) {
        return PFS_CORRUPT;
      }
      pool += claim->count;
      map_nodes += claim->kind == PFS_INDEX_ALLOCATION;
      if (claim->first == state->candidate.root.header.block &&
          claim->type == PFS_BLOCK_POOL && claim->birth == state->candidate.root.header.birth &&
          !claim->kind && !claim->level) {
        root_claim = true;
      }
    } else if (!volume) {
      return PFS_CORRUPT;
    } else if (!claim->type) {
      if (pfs_bytes_are_zero(claim->object.bytes, PFS_ID_SIZE) ||
          claim->logical > UINT64_MAX - claim->count) {
        return PFS_CORRUPT;
      }
      extents++;
      observed[volume - state->volumes][0]++;
    } else {
      bool object_tree = claim->kind == PFS_INDEX_DIRECTORY || claim->kind == PFS_INDEX_EXTENTS;
      if (claim->type != PFS_BLOCK_TREE || claim->kind < PFS_INDEX_OBJECTS ||
          claim->kind > PFS_INDEX_ORPHANS || claim->level >= PFS_TREE_DEPTH_MAX ||
          object_tree == pfs_bytes_are_zero(claim->object.bytes, PFS_ID_SIZE)) {
        return PFS_CORRUPT;
      }
      metadata += claim->count;
      observed[volume - state->volumes][1] += claim->count;
      if (claim->kind == PFS_INDEX_DIRECTORY || claim->kind == PFS_INDEX_ORPHANS) {
        observed[volume - state->volumes][2] += claim->count;
      }
    }
  }
  uint64_t grants = 0;
  for (size_t i = 0; i < state->volume_count; i++) {
    if (observed[i][0] != state->volumes[i].file_extents ||
        observed[i][1] != state->volumes[i].metadata_blocks ||
        observed[i][2] != state->volumes[i].namespace_nodes) {
      return PFS_CORRUPT;
    }
    if (state->volumes[i].grants > PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    grants += state->volumes[i].grants;
  }
  if (grants > PFS_RECORD_COUNT_MAX) {
    return PFS_LIMIT;
  }
  if (!root_claim || extents != state->file_extents || metadata != state->metadata_blocks ||
      pool != state->live_pool || map_nodes != state->map_nodes) {
    return PFS_CORRUPT;
  }
  struct check_volume *volumes = (struct check_volume *)arena->scratch;
  struct check_state checked[2];
  struct pfs_check_state_result results[2] = {0};
  borrow_state(&checked[0], state, volumes, &results[0]);
  borrow_state(&checked[1], older, volumes + PFS_VOLUME_MAX, &results[1]);
  check_reconcile_storage(&checked[0]);
  if (results[0].status != PFS_OK) {
    return results[0].status;
  }
  enum pfs_status policy = admit_limits(arena, state, older, normal_batch);
  if (policy != PFS_OK) {
    return policy;
  }
  uint8_t encoded[PFS_BLOCK_SIZE];
  enum pfs_status encoded_status = pfs_superblock_encode(encoded, sizeof(encoded),
                                                         &state->candidate.superblock);
  struct pfs_block_context block_context = {
    .block_count = state->candidate.superblock.block_count,
    .selected_generation = state->candidate.superblock.header.birth,
    .referring_birth = state->candidate.superblock.header.birth,
    .pool = state->candidate.superblock.header.pool,
    .reference = state->candidate.superblock.root,
    .features = state->candidate.superblock.features,
  };
  if (encoded_status == PFS_OK) {
    encoded_status = pfs_pool_root_encode(encoded, sizeof(encoded), &block_context,
                                          &state->candidate.root);
  }
  if (encoded_status != PFS_OK) {
    return encoded_status;
  }
  check_reconcile_state(&checked[0]);
  if (results[0].status != PFS_OK) {
    return results[0].status;
  }
  bool complete;
  enum pfs_status status = check_compare_states(&checked[0], &checked[1], &complete);
  if (status != PFS_OK || !complete) {
    return status == PFS_OK ? PFS_CORRUPT : status;
  }
  return PFS_OK;
}
