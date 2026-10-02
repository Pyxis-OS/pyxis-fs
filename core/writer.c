/* SPDX-License-Identifier: MPL-2.0 */
#include "writer.h"
#include "canonical.h"
#include "file.h"
#include "incremental_map.h"

/* Planning performs backing reads only. Keep its I/O failures distinct from
 * replacement writes/flushes and the slot publication uncertainty window. */
enum publication_phase {
  PUBLICATION_PLANNING,
  PUBLICATION_REPLACEMENTS,
  PUBLICATION_SLOT,
};

struct publication {
  struct pfs_edit_workspace editor;
  struct pfs_edit_slot catalog[PFS_PLAN_CATALOG_UNION];
  struct pfs_reference retired[PFS_PLAN_CATALOG_UNION];
  struct pfs_reference path[PFS_PLAN_CATALOG_UNION];
  size_t path_count;
  size_t volume_indices[2];
  size_t volume_count;
  struct pfs_map_plan map;
  struct pfs_incremental_map incremental;
  bool bulk;
  uint8_t root[PFS_BLOCK_SIZE];
  uint8_t slot[PFS_BLOCK_SIZE];
  uint8_t record[PFS_BLOCK_SIZE];
};

_Static_assert(sizeof(struct publication) <= PFS_PLAN_SCRATCH_BYTES / 4,
  "publication must not overlap the final quarter used by mutation staging");
_Static_assert(2 * PFS_VOLUME_MAX * sizeof(struct check_volume) <= PFS_PLAN_SCRATCH_BYTES / 2,
  "admission owns only the lower half of scratch");

_Static_assert((unsigned)PFS_MAP_FALLBACK_GLOBAL == (unsigned)PFS_INCREMENTAL_GLOBAL &&
  (unsigned)PFS_MAP_FALLBACK_OVERFLOW == (unsigned)PFS_INCREMENTAL_OVERFLOW &&
  (unsigned)PFS_MAP_FALLBACK_UNDERFLOW == (unsigned)PFS_INCREMENTAL_UNDERFLOW,
  "planner and publisher diagnostics use the same fallback reasons");

static bool
same_id(const void *a, const void *b)
{
  return !pfs_bytes_compare(a, PFS_ID_SIZE, b, PFS_ID_SIZE);
}

static struct pfs_reference
reference(uint64_t block, uint64_t birth, uint16_t type)
{
  return (struct pfs_reference){.block = block, .birth = birth, .type = type, .version = PFS_FORMAT_VERSION};
}

static const struct pfs_allocation_record *
allocation_at(const struct pfs_admit_state *state, uint64_t block)
{
  size_t low = 0, high = state->map_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const struct pfs_allocation_record *record = &state->maps[middle];
    if (block < record->first) {
      high = middle;
    } else if (block - record->first >= record->count) {
      low = middle + 1;
    } else {
      return record;
    }
  }
  return NULL;
}

static bool
claimed(const struct pfs_admit_state *state, uint64_t block)
{
  size_t low = 0, high = state->claim_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const struct check_claim *claim = &state->claims[middle];
    if (block < claim->first) {
      high = middle;
    } else if (block - claim->first >= claim->count) {
      low = middle + 1;
    } else {
      return true;
    }
  }
  return false;
}

static bool
can_free(const struct pfs_admit_state *older, uint64_t block)
{
  const struct pfs_allocation_record *other = allocation_at(older, block);
  return other && !claimed(older, block) && (other->state == PFS_ALLOCATION_FREE ||
    other->state == PFS_ALLOCATION_RETIRED);
}

/* Select only the bounded demand. The intersection proof uses both retained
 * maps and live claims; allocation never consumes a same-publication free. */
static enum pfs_status
select_free(struct pfs_writer *writer, uint64_t *blocks, size_t demand,
            const struct pfs_batch *exclude)
{
  const struct pfs_admit_state *a = &writer->states[writer->selected];
  const struct pfs_admit_state *b = &writer->states[1 - writer->selected];
  size_t ai = 0, bi = 0, count = 0;
  while (ai < a->map_count && bi < b->map_count && count < demand) {
    const struct pfs_allocation_record *left = &a->maps[ai], *right = &b->maps[bi];
    uint64_t end_a = left->first + left->count, end_b = right->first + right->count;
    uint64_t first = left->first > right->first ? left->first : right->first;
    uint64_t end = end_a < end_b ? end_a : end_b;
    if (left->state == PFS_ALLOCATION_FREE &&
        (right->state == PFS_ALLOCATION_FREE || right->state == PFS_ALLOCATION_RETIRED)) {
      for (uint64_t block = first; block < end && count < demand; block++) {
        bool used = false;
        if (exclude) {
          for (size_t i = 0; i < exclude->block_count; i++) {
            used |= exclude->blocks[i].block == block;
          }
        }
        if (!used && !claimed(a, block) && !claimed(b, block)) {
          blocks[count++] = block;
        }
      }
    }
    ai += end_a <= end_b;
    bi += end_b <= end_a;
  }
  return count == demand ? PFS_OK : PFS_NO_SPACE;
}

static const struct check_claim *
next_claim(const struct pfs_admit_state *state, size_t *index, uint64_t first)
{
  while (*index < state->claim_count &&
         state->claims[*index].first + state->claims[*index].count <= first) {
    (*index)++;
  }
  return *index < state->claim_count ? &state->claims[*index] : NULL;
}

/* A volume batch prefers one run, but its admission promise only guarantees
 * aggregate capacity. Walk eligible intervals without scanning free disk blocks. */
static bool
select_contiguous(struct pfs_writer *writer, uint64_t *blocks, size_t demand)
{
  const struct pfs_admit_state *a = &writer->states[writer->selected];
  const struct pfs_admit_state *b = &writer->states[1 - writer->selected];
  size_t ai = 0, bi = 0, ac = 0, bc = 0;
  uint64_t run_first = 0, run_end = 0;
  while (ai < a->map_count && bi < b->map_count) {
    const struct pfs_allocation_record *left = &a->maps[ai], *right = &b->maps[bi];
    uint64_t end_a = left->first + left->count, end_b = right->first + right->count;
    uint64_t first = left->first > right->first ? left->first : right->first;
    uint64_t end = end_a < end_b ? end_a : end_b;
    if (left->state == PFS_ALLOCATION_FREE &&
        (right->state == PFS_ALLOCATION_FREE || right->state == PFS_ALLOCATION_RETIRED)) {
      while (first < end) {
        const struct check_claim *claims[] = {
          next_claim(a, &ac, first), next_claim(b, &bc, first),
        };
        uint64_t blocked_end = first, free_end = end;
        for (size_t i = 0; i < 2; i++) {
          const struct check_claim *claim = claims[i];
          if (!claim) {
            continue;
          }
          if (claim->first <= first) {
            uint64_t claim_end = claim->first + claim->count;
            if (claim_end > blocked_end) {
              blocked_end = claim_end;
            }
          } else if (claim->first < free_end) {
            free_end = claim->first;
          }
        }
        if (blocked_end > first) {
          first = blocked_end;
          run_end = 0;
          continue;
        }
        if (run_end != first) {
          run_first = first;
        }
        run_end = free_end;
        if (run_end - run_first >= demand) {
          for (size_t i = 0; i < demand; i++) {
            blocks[i] = run_first + i;
          }
          return true;
        }
        first = free_end;
      }
    }
    ai += end_a <= end_b;
    bi += end_b <= end_a;
  }
  return false;
}

static void
stop_writer(struct pfs_writer *writer, enum pfs_status status, enum publication_phase phase,
            bool admitted)
{
  bool read_failed = status == PFS_IO && phase == PUBLICATION_PLANNING;
  writer->status.health = read_failed || phase == PUBLICATION_SLOT || status == PFS_CORRUPT ?
    PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED;
  writer->status.failure = status;
  writer->status.invariant_failure = admitted && status != PFS_IO && status != PFS_CORRUPT;
}

void
pfs_writer_funded_failure(struct pfs_pool *pool, enum pfs_status status)
{
  if (!pool || !pool->writer || status == PFS_OK || status == PFS_BUSY ||
      status == PFS_RECOVERY_REQUIRED) {
    return;
  }
  struct pfs_writer *writer = pool->writer;
  if (writer->status.health == PFS_WRITER_READY) {
    stop_writer(writer, status, PUBLICATION_PLANNING, true);
  }
}

static enum pfs_status
change_add(struct pfs_writer *writer, struct publication *publication, size_t *count, uint64_t first, uint64_t length,
           uint8_t state, const struct pfs_volume_id *owner, uint64_t birth, uint8_t charge)
{
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  struct pfs_map_change *changes = publication->incremental.changes;
  const struct pfs_allocation_record *before = allocation_at(source, first);
  if (!length || !before || length > before->first + before->count - first ||
      *count >= publication->incremental.change_capacity) {
    return PFS_LIMIT;
  }
  struct pfs_map_change *change = &changes[(*count)++];
  change->before = *before;
  change->before.first = first;
  change->before.count = length;
  change->after = (struct pfs_allocation_record){.first = first, .count = length,
    .state = state, .birth = birth, .charge = charge};
  if (owner) {
    change->after.owner = *owner;
  }
  if (state == PFS_ALLOCATION_RETIRED) {
    change->after.retirement = source->candidate.superblock.header.birth + 1;
  }
  return PFS_OK;
}

static bool
pool_removed(const struct check_claim *claim, const struct publication *publication)
{
  if (claim->type == PFS_BLOCK_POOL) {
    return true;
  }
  if (claim->type == PFS_BLOCK_TREE && claim->kind == PFS_INDEX_ALLOCATION) {
    return publication->bulk || pfs_incremental_map_retired(&publication->incremental, claim->first);
  }
  for (size_t i = 0; i < publication->path_count; i++) {
    if (claim->first == publication->path[i].block) {
      return true;
    }
  }
  return false;
}

static enum pfs_status
claim_add(struct pfs_writer *writer, struct check_claim claim)
{
  struct pfs_admit_state *candidate = &writer->states[2];
  if (candidate->claim_count == writer->arena.limits.claims) {
    return PFS_LIMIT;
  }
  candidate->claims[candidate->claim_count++] = claim;
  return PFS_OK;
}

static bool
same_reference(const struct pfs_reference *a, const struct pfs_reference *b)
{
  return a->block == b->block && a->birth == b->birth && a->type == b->type &&
    a->version == b->version;
}

static enum pfs_status
changed_volume(struct pfs_admit_state *candidate, struct publication *publication,
               const struct pfs_volume_id *id)
{
  for (size_t i = 0; i < candidate->volume_count; i++) {
    if (!same_id(candidate->volumes[i].record.id.bytes, id->bytes)) {
      continue;
    }
    for (size_t j = 0; j < publication->volume_count; j++) {
      if (publication->volume_indices[j] == i) {
        return PFS_OK;
      }
    }
    if (publication->volume_count == 2) {
      return PFS_LIMIT;
    }
    publication->volume_indices[publication->volume_count++] = i;
    return PFS_OK;
  }
  return PFS_CORRUPT;
}

static enum pfs_status
catalog_path(struct pfs_writer *writer, struct publication *publication,
             const struct pfs_volume_id *volume)
{
  const struct pfs_pool_candidate *source = &writer->states[writer->selected].candidate;
  struct pfs_reference current = source->root.volumes;
  struct pfs_tree_context context = {.kind = PFS_INDEX_VOLUMES,
    .block = {.block_count = writer->backing->reader.geometry.block_count,
      .pool = source->superblock.header.pool, .features = source->superblock.features,
      .selected_generation = source->superblock.header.birth,
      .referring_birth = source->root.header.birth}};
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, volume->bytes, PFS_ID_SIZE);
  size_t depth = 0;
  for (;;) {
    if (depth++ == PFS_PLAN_CATALOG_PATH) {
      return PFS_LIMIT;
    }
    context.block.reference = current;
    enum pfs_status status = pfs_block_read(&writer->backing->reader, current.block, 1,
      publication->record, PFS_BLOCK_SIZE);
    struct pfs_tree tree;
    if (status == PFS_OK) {
      status = pfs_canonical_tree_validate(publication->record, PFS_BLOCK_SIZE, &context, &tree);
    }
    if (status != PFS_OK) {
      return status;
    }
    bool shared = false;
    for (size_t i = 0; i < publication->path_count; i++) {
      if (publication->path[i].block == current.block) {
        if (!same_reference(&publication->path[i], &current)) {
          return PFS_CORRUPT;
        }
        shared = true;
        break;
      }
    }
    if (!shared) {
      if (publication->path_count == writer->arena.limits.catalog_union) {
        return PFS_LIMIT;
      }
      publication->path[publication->path_count++] = current;
    }
    if (!tree.level) {
      return PFS_OK;
    }
    struct pfs_internal_record child;
    bool found = false;
    for (uint16_t i = 0; i < tree.count; i++) {
      const uint8_t *data = publication->record + tree.slots[i].offset;
      size_t length = tree.slots[i].length;
      status = PFS_OK;
      struct pfs_record_context record_context = {.block_count = context.block.block_count,
        .selected_generation = context.block.selected_generation,
        .containing_birth = tree.header.birth, .features = context.block.features};
      if (status == PFS_OK) {
        status = pfs_internal_record_decode(data, length, PFS_INDEX_VOLUMES,
          &record_context, &child);
      }
      if (status != PFS_OK) {
        return status;
      }
      if (pfs_bytes_compare(child.minimum.bytes, child.minimum.length, key.bytes, key.length) > 0) {
        break;
      }
      current = child.child;
      found = true;
    }
    if (!found) {
      return PFS_CORRUPT;
    }
    context.block.referring_birth = tree.header.birth;
  }
}

static enum pfs_status
make_claims(struct pfs_writer *writer, const struct pfs_batch *batch,
            const struct publication *publication)
{
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  struct pfs_admit_state *candidate = &writer->states[2];
  candidate->claim_count = 0;
  for (size_t i = 0; i < source->claim_count; i++) {
    struct check_claim claim = source->claims[i];
    if (pool_removed(&claim, publication)) {
      continue;
    }
    uint64_t position = claim.first, end = position + claim.count;
    if (batch) {
      for (size_t j = 0; j < batch->remove_count; j++) {
        const struct check_claim *remove = &batch->remove[j];
        if (remove->first >= end || remove->first + remove->count <= claim.first) {
          continue;
        }
        if (!remove->count || remove->first < position || remove->first + remove->count > end ||
            remove->birth != claim.birth || remove->type != claim.type ||
            remove->kind != claim.kind || remove->level != claim.level ||
            !same_id(remove->volume.bytes, claim.volume.bytes) ||
            !same_id(remove->object.bytes, claim.object.bytes) ||
            (!claim.type && remove->logical != claim.logical + remove->first - claim.first) ||
            (claim.type && remove->count != claim.count)) {
          return PFS_INVALID;
        }
        if (remove->first > position) {
          struct check_claim prefix = claim;
          prefix.first = position;
          prefix.count = remove->first - position;
          prefix.logical += position - claim.first;
          enum pfs_status status = claim_add(writer, prefix);
          if (status != PFS_OK) {
            return status;
          }
        }
        position = remove->first + remove->count;
      }
    }
    if (position < end) {
      claim.logical += position - claim.first;
      claim.first = position;
      claim.count = end - position;
      enum pfs_status status = claim_add(writer, claim);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  if (batch) {
    for (size_t i = 0; i < batch->add_count; i++) {
      enum pfs_status status = claim_add(writer, batch->add[i]);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  return PFS_OK;
}

static enum pfs_status
gather_changes(struct pfs_writer *writer, const struct pfs_batch *batch,
               struct publication *publication, bool allocate, size_t *count)
{
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  const struct pfs_admit_state *older = &writer->states[1 - writer->selected];
  struct pfs_admit_state *candidate = &writer->states[2];
  uint64_t generation = source->candidate.superblock.header.birth + 1;
  size_t changes_count = 0;
  enum pfs_status status;
  for (size_t i = 0; i < source->map_count; i++) {
    const struct pfs_allocation_record *record = &source->maps[i];
    if (record->state != PFS_ALLOCATION_RETIRED) {
      continue;
    }
    bool volume = !pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE);
    for (uint64_t block = record->first; block < record->first + record->count; block++) {
      if (!can_free(older, block)) {
        continue;
      }
      status = change_add(writer, publication, &changes_count, block, 1, PFS_ALLOCATION_FREE, NULL, 0, 0);
      if (status != PFS_OK) {
        return status;
      }
      if (volume) {
        status = changed_volume(candidate, publication, &record->owner);
        if (status != PFS_OK) {
          return status;
        }
      }
    }
  }
  for (size_t i = 0; i < source->claim_count; i++) {
    const struct check_claim *claim = &source->claims[i];
    if (pool_removed(claim, publication)) {
      status = change_add(writer, publication, &changes_count, claim->first, claim->count,
        PFS_ALLOCATION_RETIRED, NULL, claim->birth, PFS_CHARGE_RECOVERY);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  if (batch) {
    uint64_t retired = 0;
    for (size_t i = 0; i < batch->remove_count; i++) {
      const struct check_claim *claim = &batch->remove[i];
      if (!same_id(claim->volume.bytes, batch->volume.record.id.bytes) ||
          claim->count > PFS_PLAN_VOLUME_RETIRED - retired) {
        return PFS_INVALID;
      }
      retired += claim->count;
      status = change_add(writer, publication, &changes_count, claim->first, claim->count,
        PFS_ALLOCATION_RETIRED, &claim->volume, claim->birth,
        batch->orphan_cleanup ? PFS_CHARGE_RECOVERY : PFS_CHARGE_ORDINARY);
      if (status != PFS_OK) {
        return status;
      }
    }
    for (size_t i = 0; i < batch->block_count; i++) {
      status = change_add(writer, publication, &changes_count, batch->blocks[i].block, 1,
        PFS_ALLOCATION_VOLUME, &batch->volume.record.id, generation, PFS_CHARGE_PERMANENT);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  if (allocate) {
    size_t total = publication->incremental.marked_count + publication->path_count + 1;
    if (total > writer->arena.limits.pool_blocks) {
      return PFS_LIMIT;
    }
    for (size_t i = 0; i < total; i++) {
      status = change_add(writer, publication, &changes_count, publication->incremental.ids[i], 1,
        PFS_ALLOCATION_POOL, NULL, generation, PFS_CHARGE_PERMANENT);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  pfs_plan_sort_changes(publication->incremental.changes, changes_count);
  *count = changes_count;
  return PFS_OK;
}

static enum pfs_status
seal_publication(struct pfs_writer *writer, const struct pfs_batch *batch,
                 struct publication *publication, struct pfs_block_context context,
                 bool *resource_miss)
{
  struct pfs_admit_state *source = &writer->states[writer->selected];
  const struct pfs_admit_state *older = &writer->states[1 - writer->selected];
  struct pfs_admit_state *candidate = &writer->states[2];
  uint64_t generation = context.selected_generation;
  struct pfs_record_context record_context = {.block_count = context.block_count,
    .selected_generation = generation, .containing_birth = generation, .features = context.features};
  enum pfs_status status;
  *resource_miss = false;
  candidate->map_count = publication->map.record_count;
  candidate->candidate.root = source->candidate.root;
  struct pfs_pool_root *root = &candidate->candidate.root;
  root->header.block = publication->map.pool_root_block;
  root->header.birth = generation;
  root->allocation = publication->map.root;
  root->live_pool = root->live_volume = root->retired = root->free = 0;
  root->cow.occupied = root->recovery.occupied = root->migration.occupied = 0;
  for (size_t i = 0; i < candidate->volume_count; i++) {
    candidate->volumes[i].record.live_blocks = 0;
    candidate->volumes[i].record.retired_blocks = 0;
  }
  for (size_t i = 0; i < candidate->map_count; i++) {
    const struct pfs_allocation_record *record = &candidate->maps[i];
    if (record->state == PFS_ALLOCATION_FREE) {
      root->free += record->count;
    } else if (record->state == PFS_ALLOCATION_POOL) {
      root->live_pool += record->count;
    } else if (record->state == PFS_ALLOCATION_VOLUME) {
      root->live_volume += record->count;
    } else {
      root->retired += record->count;
    }
    if (record->charge == PFS_CHARGE_ORDINARY) {
      root->cow.occupied += record->count;
    } else if (record->charge == PFS_CHARGE_RECOVERY) {
      root->recovery.occupied += record->count;
    }
    for (size_t j = 0; j < candidate->volume_count; j++) {
      if (same_id(record->owner.bytes, candidate->volumes[j].record.id.bytes)) {
        if (record->state == PFS_ALLOCATION_VOLUME) {
          candidate->volumes[j].record.live_blocks += record->count;
        } else if (record->state == PFS_ALLOCATION_RETIRED) {
          candidate->volumes[j].record.retired_blocks += record->count;
        }
      }
    }
  }
  status = make_claims(writer, batch, publication);
  if (status != PFS_OK) {
    return status;
  }
  if (publication->volume_count) {
    for (size_t i = 0; i < publication->path_count; i++) {
      publication->catalog[i] = (struct pfs_edit_slot){.block = publication->map.catalog_blocks[i]};
    }
    /* Sequential fixed-length updates share one private candidate. Consumed
     * private nodes are reused by the editor, never added to durable debt. */
    struct pfs_edit_candidate edit = {.reader = &writer->backing->reader,
      .context = {.block = context, .kind = PFS_INDEX_VOLUMES}, .birth = generation,
      .root = source->candidate.root.volumes, .slots = publication->catalog,
      .slot_capacity = publication->path_count, .retired = publication->retired,
      .retired_capacity = publication->path_count};
    for (size_t i = 0; i < publication->volume_count; i++) {
      const struct pfs_volume_record *volume = &candidate->volumes[publication->volume_indices[i]].record;
      size_t length;
      status = pfs_volume_record_encode(publication->record, sizeof(publication->record),
        &record_context, volume, &length);
      if (status != PFS_OK) {
        return status;
      }
      struct pfs_key key = {.length = PFS_ID_SIZE};
      pfs_bytes_copy(key.bytes, volume->id.bytes, PFS_ID_SIZE);
      struct pfs_encoded_record encoded = {publication->record, length};
      status = pfs_edit_tree(&edit, &publication->editor, PFS_EDIT_UPDATE, &key, &encoded);
      if (status != PFS_OK) {
        return status;
      }
    }
    if (edit.retired_count != publication->path_count) {
      return PFS_CORRUPT;
    }
    for (size_t i = 0; i < publication->path_count; i++) {
      bool found = false;
      for (size_t j = 0; j < edit.retired_count; j++) {
        found |= same_reference(&publication->path[i], &edit.retired[j]);
      }
      if (!found || !publication->catalog[i].active ||
          publication->catalog[i].block != publication->map.catalog_blocks[i]) {
        return PFS_CORRUPT;
      }
    }
    root->volumes = edit.root;
    for (size_t i = 0; i < publication->path_count; i++) {
      struct pfs_tree tree;
      struct pfs_tree_context tree_context = {.block = context, .kind = PFS_INDEX_VOLUMES};
      tree_context.block.reference = reference(publication->catalog[i].block, generation, PFS_BLOCK_TREE);
      status = pfs_tree_decode(publication->catalog[i].data, PFS_BLOCK_SIZE, &tree_context, &tree);
      if (status == PFS_OK) {
        status = claim_add(writer, (struct check_claim){.first = tree.header.block, .count = 1,
          .birth = generation, .type = PFS_BLOCK_TREE, .kind = PFS_INDEX_VOLUMES, .level = tree.level});
      }
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  for (size_t i = 0; i < publication->map.node_count; i++) {
    const struct pfs_map_node *node = &publication->map.nodes[i];
    status = claim_add(writer, (struct check_claim){.first = node->block, .count = 1,
      .birth = generation, .type = PFS_BLOCK_TREE, .kind = PFS_INDEX_ALLOCATION, .level = node->level});
    if (status != PFS_OK) {
      return status;
    }
  }
  status = claim_add(writer, (struct check_claim){.first = root->header.block, .count = 1,
    .birth = generation, .type = PFS_BLOCK_POOL});
  if (status != PFS_OK) {
    return status;
  }
  context.reference = reference(root->header.block, generation, PFS_BLOCK_POOL);
  size_t target = source->candidate.superblock.header.birth == older->candidate.superblock.header.birth ?
    0 : 1 - writer->selected;
  candidate->candidate.superblock = source->candidate.superblock;
  candidate->candidate.superblock.header.birth = generation;
  candidate->candidate.superblock.header.block = target ? context.block_count - 1 : 0;
  candidate->candidate.superblock.root = context.reference;
  /* Admission uses the lower half of scratch; the sealed publication is kept
   * in its upper half so validation cannot overwrite encoded output. */
  candidate->file_extents = candidate->metadata_blocks = 0;
  candidate->live_pool = candidate->map_nodes = 0;
  for (size_t i = 0; i < candidate->claim_count; i++) {
    const struct check_claim *claim = &candidate->claims[i];
    if (pfs_bytes_are_zero(claim->volume.bytes, PFS_ID_SIZE)) {
      candidate->live_pool += claim->count;
      candidate->map_nodes += claim->kind == PFS_INDEX_ALLOCATION;
    } else if (claim->type) {
      candidate->metadata_blocks += claim->count;
    } else {
      candidate->file_extents++;
    }
  }
  check_sort_claims(candidate->claims, candidate->claim_count);
  status = pfs_admit_candidate(&writer->arena, candidate, source, batch != NULL);
  *resource_miss = status == PFS_LIMIT || status == PFS_NO_SPACE;
  if (status == PFS_OK) {
    status = pfs_pool_root_encode(publication->root, PFS_BLOCK_SIZE, &context, root);
  }
  if (status == PFS_OK) {
    status = pfs_superblock_encode(publication->slot, PFS_BLOCK_SIZE, &candidate->candidate.superblock);
  }
  return status;
}

static enum pfs_status
build_publication(struct pfs_writer *writer, const struct pfs_batch *batch,
                  struct publication *publication)
{
  struct pfs_admit_state *source = &writer->states[writer->selected];
  const struct pfs_admit_state *older = &writer->states[1 - writer->selected];
  struct pfs_admit_state *candidate = &writer->states[2];
  struct pfs_allocation_record *maps = candidate->maps;
  struct check_claim *claims = candidate->claims;
  *candidate = *source;
  candidate->maps = maps;
  candidate->claims = claims;
  uint64_t generation = source->candidate.superblock.header.birth + 1;
  if (!generation) {
    return PFS_LIMIT;
  }
  publication->path_count = publication->volume_count = 0;
  publication->bulk = false;
  writer->map_metrics = (struct pfs_map_metrics){.generation = generation};
  enum pfs_status status = pfs_incremental_map_init(&writer->arena, &publication->incremental);
  if (status != PFS_OK) {
    stop_writer(writer, status, PUBLICATION_PLANNING, true);
    return status;
  }
  if (batch) {
    status = changed_volume(candidate, publication, &batch->volume.record.id);
    if (status != PFS_OK) {
      return status == PFS_CORRUPT ? PFS_INVALID : status;
    }
    size_t index = publication->volume_indices[0];
    const struct pfs_admit_volume *previous = &source->volumes[index];
    if (previous->file_extents > source->file_extents ||
        previous->metadata_blocks > source->metadata_blocks) {
      return PFS_CORRUPT;
    }
    uint64_t extents = source->file_extents - previous->file_extents;
    uint64_t metadata = source->metadata_blocks - previous->metadata_blocks;
    /* Establish the existing physical E/M guards before relying on the S/H
     * map-storage proof. Final admission still checks actual claims, deletion
     * headroom, quotas, debt and next-step funding. */
    if (batch->volume.file_extents > writer->arena.limits.extents - extents ||
        batch->volume.metadata_blocks > writer->arena.limits.metadata - metadata) {
      return PFS_LIMIT;
    }
    candidate->volumes[index] = batch->volume;
  }
  /* Determine catalog owners once from the immutable input retirement cohorts. */
  for (size_t i = 0; i < source->map_count; i++) {
    const struct pfs_allocation_record *record = &source->maps[i];
    if (record->state != PFS_ALLOCATION_RETIRED ||
        pfs_bytes_are_zero(record->owner.bytes, PFS_ID_SIZE)) {
      continue;
    }
    for (uint64_t block = record->first; block < record->first + record->count; block++) {
      if (can_free(older, block)) {
        status = changed_volume(candidate, publication, &record->owner);
        if (status != PFS_OK) {
          return status;
        }
        break;
      }
    }
  }
  for (size_t i = 0; i < publication->volume_count; i++) {
    status = catalog_path(writer, publication,
      &candidate->volumes[publication->volume_indices[i]].record.id);
    if (status != PFS_OK) {
      return status;
    }
  }
  struct pfs_block_context context = {.block_count = writer->backing->reader.geometry.block_count,
    .selected_generation = generation, .referring_birth = generation,
    .pool = source->candidate.superblock.header.pool,
    .features = source->candidate.superblock.features};
  struct pfs_block_context source_context = context;
  source_context.selected_generation = source->candidate.superblock.header.birth;
  source_context.referring_birth = source->candidate.root.header.birth;
  source_context.reference = source->candidate.root.allocation;
  struct pfs_incremental_map *incremental = &publication->incremental;
  status = pfs_incremental_map_load(incremental, &writer->backing->reader, &source_context,
    source->maps, source->map_count, &publication->editor);
  if (status != PFS_OK || incremental->source_count != source->map_nodes) {
    if (status != PFS_OK && status != PFS_IO && status != PFS_CORRUPT) {
      stop_writer(writer, status, PUBLICATION_PLANNING, true);
    }
    return status != PFS_OK ? status : PFS_CORRUPT;
  }
  size_t demand = (size_t)writer->arena.limits.pool_blocks;
  status = select_free(writer, incremental->ids, demand, batch);
  if (status != PFS_OK) {
    stop_writer(writer, status, PUBLICATION_PLANNING, true);
    return status;
  }
  struct pfs_record_context record_context = {.block_count = context.block_count,
    .selected_generation = generation, .containing_birth = generation, .features = context.features};
  bool again;
  do {
    size_t count;
    status = gather_changes(writer, batch, publication, true, &count);
    if (status != PFS_OK) {
      stop_writer(writer, status, PUBLICATION_PLANNING, true);
      return status;
    }
    status = pfs_plan_map_apply(&record_context, source->maps, source->map_count,
      incremental->changes, count, candidate->maps, (size_t)writer->arena.limits.records,
      &candidate->map_count);
    if (status == PFS_LIMIT) {
      /* Canonical stream storage is guaranteed once physical E/M fits. */
      stop_writer(writer, status, PUBLICATION_PLANNING, true);
      return status;
    }
    if (status != PFS_OK) {
      return status;
    }
    status = pfs_incremental_map_close(incremental, candidate->maps, candidate->map_count, &again);
    if (status != PFS_OK) {
      return status;
    }
  } while (again);
  writer->map_metrics.source_nodes = incremental->source_count;
  writer->map_metrics.closure_nodes = incremental->marked_count;
  writer->map_metrics.growth_passes = incremental->growth_passes;
  writer->map_metrics.largest_addition = incremental->largest_addition;
  writer->map_metrics.redistribution_additions = incremental->redistribution_additions;
  writer->map_metrics.failed_run_leaves = incremental->failed_run_leaves;
  writer->map_metrics.failed_run_records = incremental->failed_run_records;
  writer->map_metrics.fallback |= incremental->fallback;
  bool resource_miss;
  if (!writer->map_metrics.fallback) {
    status = pfs_incremental_map_encode(incremental, &context, candidate->maps, candidate->map_count,
      publication->path_count, &publication->editor, publication->record, &publication->map);
    if (status != PFS_OK) {
      stop_writer(writer, status, PUBLICATION_PLANNING, true);
      return status;
    }
    status = seal_publication(writer, batch, publication, context, &resource_miss);
    if (status == PFS_OK) {
      writer->map_metrics.local = true;
      writer->map_metrics.replacement_nodes = publication->map.node_count;
      if (!writer->collect_map_metrics) {
        return PFS_OK;
      }
      /* Reuse only the dead raw deltas. The sealed map and topology survive. */
      publication->bulk = true;
      size_t changes_count, base_count;
      status = gather_changes(writer, batch, publication, false, &changes_count);
      if (status == PFS_OK) {
        status = pfs_plan_map_count(&record_context, source->maps, source->map_count,
          incremental->changes, changes_count, (size_t)writer->arena.limits.records, &base_count);
      }
      if (status == PFS_OK) {
        status = pfs_plan_map_bulk_size(base_count, publication->path_count,
          &writer->map_metrics.bulk_reference_nodes);
      }
      publication->bulk = false;
      if (status != PFS_OK) {
        stop_writer(writer, status, PUBLICATION_PLANNING, true);
      }
      return status;
    }
    if (!resource_miss) {
      return status;
    }
    writer->map_metrics.fallback |= PFS_MAP_FALLBACK_RESOURCE;
  }
  /* A scratch miss discards all provisional accounting. The funded legacy
   * builder consumes the same fixed eligible prefix, at this generation. */
  publication->bulk = true;
  size_t changes_count;
  status = gather_changes(writer, batch, publication, false, &changes_count);
  if (status == PFS_OK) {
    status = pfs_plan_map_apply(&record_context, source->maps, source->map_count,
      incremental->changes, changes_count, candidate->maps, (size_t)writer->arena.limits.records,
      &candidate->map_count);
  }
  if (status != PFS_OK) {
    if (status == PFS_LIMIT || status == PFS_NO_SPACE || status == PFS_NO_MEMORY) {
      stop_writer(writer, status, PUBLICATION_PLANNING, true);
    }
    return status;
  }
  size_t range_count = 0;
  for (size_t i = 0; i < demand; i++) {
    uint64_t id = incremental->ids[i];
    if (range_count && incremental->ranges[range_count - 1].first +
        incremental->ranges[range_count - 1].count == id) {
      incremental->ranges[range_count - 1].count++;
    } else {
      incremental->ranges[range_count++] = (struct pfs_reusable_range){id, 1};
    }
  }
  status = pfs_plan_map_build(&writer->arena, &context, candidate->maps, candidate->map_count,
    incremental->ranges, range_count, publication->path_count, &publication->map);
  if (status != PFS_OK &&
      (status == PFS_LIMIT || status == PFS_NO_SPACE || status == PFS_NO_MEMORY)) {
    stop_writer(writer, status, PUBLICATION_PLANNING, true);
  }
  if (status == PFS_OK) {
    writer->map_metrics.replacement_nodes = publication->map.node_count;
    writer->map_metrics.bulk_reference_nodes = publication->map.node_count;
    status = seal_publication(writer, batch, publication, context, &resource_miss);
  }
  return status;
}

static enum pfs_status
publish(struct pfs_writer *writer, const struct pfs_batch *batch, enum publication_phase *phase)
{
  struct publication *publication = (struct publication *)(writer->arena.scratch +
    PFS_PLAN_SCRATCH_BYTES / 2);
  writer->active_batch = batch;
  *phase = PUBLICATION_PLANNING;
  writer->map_planning = true;
  enum pfs_status status = build_publication(writer, batch, publication);
  if (status != PFS_OK) {
    goto done;
  }
  writer->map_planning = false;
  *phase = PUBLICATION_REPLACEMENTS;
  if (batch) {
    for (size_t i = 0; status == PFS_OK && i < batch->block_count; i++) {
      status = pfs_block_write(writer->backing, batch->blocks[i].block, 1,
        batch->blocks[i].data, PFS_BLOCK_SIZE);
    }
  }
  for (size_t i = 0; status == PFS_OK && i < publication->map.node_count; i++) {
    status = pfs_block_write(writer->backing, publication->map.nodes[i].block, 1,
      publication->map.blocks + i * PFS_BLOCK_SIZE, PFS_BLOCK_SIZE);
  }
  for (size_t i = 0; status == PFS_OK && i < publication->path_count; i++) {
    status = pfs_block_write(writer->backing, publication->catalog[i].block, 1,
      publication->catalog[i].data, PFS_BLOCK_SIZE);
  }
  if (status == PFS_OK) {
    status = pfs_block_write(writer->backing, publication->map.pool_root_block, 1,
      publication->root, PFS_BLOCK_SIZE);
  }
  if (status == PFS_OK) {
    status = pfs_block_flush(writer->backing);
  }
  if (status != PFS_OK) {
    goto done;
  }
  struct pfs_admit_state *candidate = &writer->states[2];
  *phase = PUBLICATION_SLOT;
  status = pfs_block_write(writer->backing, candidate->candidate.superblock.header.block, 1,
    publication->slot, PFS_BLOCK_SIZE);
  if (status == PFS_OK) {
    status = pfs_block_flush(writer->backing);
  }
  if (status != PFS_OK) {
    goto done;
  }
  size_t target = candidate->candidate.superblock.header.block ? 1 : 0;
  struct pfs_allocation_record *previous_maps = writer->states[target].maps;
  struct check_claim *previous_claims = writer->states[target].claims;
  writer->states[target] = *candidate;
  candidate->maps = previous_maps;
  candidate->claims = previous_claims;
  /* The planner always emits into maps[2]; rotate arena vector identities too. */
  writer->arena.maps[target] = writer->states[target].maps;
  writer->arena.maps[2] = candidate->maps;
  writer->arena.claims[target] = (uint8_t *)writer->states[target].claims;
  writer->arena.claims[2] = (uint8_t *)candidate->claims;
  writer->selected = target;
  writer->last_confirmed_generation = writer->states[target].candidate.superblock.header.birth;
  writer->diagnostic.candidate[target] = writer->states[target].candidate;
  writer->diagnostic.selected = (uint16_t)target;
  writer->status.drain_pending = writer->states[target].retired_volume != 0;
done:
  writer->map_planning = false;
  writer->active_batch = NULL;
  return status;
}

enum pfs_status
pfs_writer_fence(struct pfs_pool *pool, struct pfs_write_result *result)
{
  if (!pool || !pool->writer || !result || !pool->writer_busy) {
    return PFS_INVALID;
  }
  struct pfs_writer *writer = pool->writer;
  if (writer->status.health != PFS_WRITER_READY) {
    result->maintenance_completion = PFS_MAINTENANCE_STOPPED;
    result->maintenance_status = PFS_RECOVERY_REQUIRED;
    result->health = writer->status.health;
    return PFS_RECOVERY_REQUIRED;
  }
  result->maintenance_completion = PFS_MAINTENANCE_COMPLETE;
  result->maintenance_status = PFS_OK;
  for (unsigned step = 0; writer->status.drain_pending && step < 2; step++) {
    enum publication_phase phase;
    enum pfs_status status = publish(writer, NULL, &phase);
    if (status != PFS_OK) {
      stop_writer(writer, status, phase, true);
      result->maintenance_completion = phase == PUBLICATION_SLOT ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED;
      result->maintenance_status = status;
      result->health = writer->status.health;
      return status;
    }
    result->maintenance_completion = PFS_MAINTENANCE_COMPLETE;
  }
  if (writer->status.drain_pending) {
    stop_writer(writer, PFS_LIMIT, PUBLICATION_PLANNING, true);
    result->maintenance_completion = PFS_MAINTENANCE_STOPPED;
    result->maintenance_status = PFS_LIMIT;
    result->health = writer->status.health;
    return PFS_LIMIT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_writer_prepare(struct pfs_pool *pool, struct pfs_batch **batch)
{
  if (!pool || !pool->state.data || !batch) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_writer_begin(pool, true);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_writer *writer = pool->writer;
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  if (source->candidate.superblock.header.birth > UINT64_MAX - 3) {
    pfs_writer_end(pool, PFS_OK);
    return PFS_LIMIT;
  }
  writer->batch = (struct pfs_batch){.generation = source->candidate.superblock.header.birth + 1,
    .available_count = PFS_PLAN_VOLUME_NEW};
  if (!select_contiguous(writer, writer->batch.available, PFS_PLAN_VOLUME_NEW)) {
    status = select_free(writer, writer->batch.available, PFS_PLAN_VOLUME_NEW, NULL);
  }
  if (status != PFS_OK || source->reusable_blocks < writer->arena.limits.pool_blocks + PFS_PLAN_VOLUME_NEW) {
    status = status != PFS_OK ? status : PFS_NO_SPACE;
    stop_writer(writer, status, PUBLICATION_PLANNING, true);
    pfs_writer_end(pool, PFS_OK);
    return status;
  }
  writer->prepared = true;
  *batch = &writer->batch;
  return PFS_OK;
}

void
pfs_writer_abort(struct pfs_pool *pool)
{
  if (pool && pool->writer && pool->writer->prepared && !pool->writer->publishing) {
    pool->writer->prepared = false;
    pfs_writer_end(pool, PFS_OK);
  }
}

enum pfs_status
pfs_writer_commit(struct pfs_pool *pool, struct pfs_batch *batch, struct pfs_write_result *result)
{
  if (pool && pool->writer && pool->writer->publishing) {
    return PFS_BUSY;
  }
  if (!pool || !pool->writer || !pool->writer->prepared || batch != &pool->writer->batch || !result) {
    return PFS_INVALID;
  }
  struct pfs_writer *writer = pool->writer;
  writer->publishing = true;
  *result = (struct pfs_write_result){.completion = PFS_STOPPED, .health = writer->status.health};
  enum pfs_status status = PFS_OK;
  /* Deleting a final leaf may collapse to an unchanged child or an empty
   * index, retiring metadata without emitting replacement volume nodes. */
  if (batch->available_count != PFS_PLAN_VOLUME_NEW ||
      batch->block_count > PFS_PLAN_VOLUME_NEW || (batch->block_count && !batch->blocks) ||
      batch->add_count > PFS_PLAN_VOLUME_NEW || (batch->add_count && !batch->add) ||
      ((!batch->block_count) != (!batch->add_count)) ||
      (!batch->block_count && !batch->remove_count) ||
      batch->remove_count > PFS_PLAN_VOLUME_RETIRED || (batch->remove_count && !batch->remove) ||
      batch->generation != writer->last_confirmed_generation + 1) {
    status = PFS_INVALID;
  }
  for (size_t i = 0; status == PFS_OK && i < batch->block_count; i++) {
    bool available = false;
    for (size_t j = 0; j < batch->available_count; j++) {
      available |= batch->blocks[i].block == batch->available[j];
    }
    for (size_t j = 0; j < i; j++) {
      if (batch->blocks[i].block == batch->blocks[j].block) {
        available = false;
      }
    }
    if (!available || !batch->blocks[i].data) {
      status = PFS_INVALID;
    }
  }
  enum publication_phase phase = PUBLICATION_PLANNING;
  if (status == PFS_OK) {
    status = publish(writer, batch, &phase);
  }
  if (status == PFS_OK) {
    result->completion = PFS_COMPLETE;
    result->maintenance_completion = writer->status.drain_pending ?
      PFS_MAINTENANCE_PENDING : PFS_MAINTENANCE_NONE;
  } else {
    result->operation_status = status;
    if (status == PFS_IO || status == PFS_CORRUPT || phase == PUBLICATION_SLOT) {
      stop_writer(writer, status, phase, false);
    }
    result->completion = phase == PUBLICATION_SLOT ? PFS_UNKNOWN : PFS_STOPPED;
  }
  result->health = writer->status.health;
  writer->publishing = false;
  writer->prepared = false;
  pfs_writer_end(pool, PFS_OK);
  return status;
}

enum pfs_status
pfs_pool_open_writer(struct pfs_pool *pool, const struct pfs_block_builder *backing,
                     struct pfs_memory *memory, const struct pfs_write_options *options,
                     struct pfs_write_open_result *diagnostic)
{
  if (pool && pool->writer_busy) {
    return PFS_BUSY;
  }
  if (!pool || pool->state.owner || pool->state.data || pool->state.size ||
      pool->state.alignment || pool->writer || pool->reader || pool->memory || !backing ||
      !backing->write || !backing->flush || !backing->reader.read ||
      backing->reader.geometry.block_count < PFS_POOL_BLOCKS_MIN ||
      backing->reader.geometry.block_count > PFS_POOL_BLOCKS_MAX ||
      !backing->reader.geometry.max_transfer_blocks || !memory || !memory->allocate ||
      !memory->free || !memory->limit || memory->limit > PFS_MEMORY_MAX || memory->used > memory->limit ||
      !options || !options->random || options->extent_limit > PFS_RECORD_COUNT_MAX || !options->metadata_limit ||
      options->metadata_limit > backing->reader.geometry.block_count - 2 || !diagnostic) {
    return PFS_INVALID;
  }
  *diagnostic = (struct pfs_write_open_result){
    .pool = {.selected = PFS_POOL_NO_SELECTION},
    .recovery = {.completion = PFS_STOPPED},
  };
  pool->writer_busy = true;
  pool->memory = memory;
  pool->reader = &backing->reader;
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(memory, sizeof(struct pfs_writer),
    _Alignof(struct pfs_writer), &allocation);
  if (status == PFS_OK) {
    pool->writer = allocation.data;
    pfs_bytes_zero(pool->writer, sizeof(*pool->writer));
    pfs_memory_move(memory, &allocation, &pool->writer->allocation);
    pool->writer->pool = pool;
    pool->writer->backing = backing;
    pool->writer->cleanup = pfs_orphan_cleanup;
    pool->writer->fence = pfs_writer_fence;
    pool->writer->random = options->random;
    pool->writer->random_context = options->random_context;
    status = pfs_admit_open(&backing->reader, memory, options->extent_limit, options->metadata_limit,
      &pool->writer->arena, pool->writer->states, &pool->writer->opening);
  }
  struct pfs_writer *writer = pool->writer;
  if (writer) {
    diagnostic->pool = writer->opening.pool;
    /* Keep computed requirements reviewable even when admission rejects a
     * fully checked image or cannot reserve its arena. */
    if (writer->opening.cross_complete && writer->opening.state[0].complete &&
        writer->opening.state[1].complete) {
      struct pfs_plan_limits limits;
      if (pfs_plan_limits(backing->reader.geometry.block_count, options->extent_limit,
          options->metadata_limit, (uint16_t)writer->opening.pool.candidate[0].root.volume_count,
          &limits) == PFS_OK) {
        diagnostic->required_recovery_blocks = limits.recovery_blocks;
        diagnostic->permanent_pool_blocks = limits.permanent_pool;
        diagnostic->reserved_arena_bytes = limits.arena_bytes;
      }
    }
  }
  if (status == PFS_OK) {
    status = writer->random(writer->random_context, writer->nonce, sizeof(writer->nonce));
    if (status != PFS_OK || pfs_bytes_are_zero(writer->nonce, sizeof(writer->nonce))) {
      status = PFS_IO;
    }
  }
  if (status == PFS_OK) {
    struct pfs_pool temporary = {0};
    status = pfs_pool_open(&temporary, &backing->reader, memory, &writer->diagnostic);
    if (status == PFS_OK) {
      pfs_memory_move(memory, &temporary.state, &pool->state);
      writer->selected = writer->diagnostic.selected;
      writer->last_confirmed_generation = writer->states[writer->selected].candidate.superblock.header.birth;
      writer->status.drain_pending = writer->states[writer->selected].retired_volume != 0;
      diagnostic->required_recovery_blocks = writer->arena.limits.recovery_blocks;
      diagnostic->permanent_pool_blocks = writer->arena.limits.permanent_pool;
      diagnostic->reserved_arena_bytes = writer->arena.limits.arena_bytes;
      diagnostic->recovery.completion = PFS_COMPLETE;
      status = pfs_block_flush(backing);
      if (status == PFS_OK) {
        status = pfs_writer_fence(pool, &diagnostic->recovery);
        if (status == PFS_OK) {
          pool->writer_busy = false;
          status = pfs_orphan_recover(pool, &diagnostic->recovery);
          pool->writer_busy = true;
          if (status == PFS_OK) {
            status = pfs_writer_fence(pool, &diagnostic->recovery);
          }
        }
      } else {
        stop_writer(writer, status, PUBLICATION_REPLACEMENTS, false);
        diagnostic->recovery.completion = PFS_STOPPED;
        diagnostic->recovery.operation_status = status;
      }
    }
  }
  if (writer) {
    diagnostic->writer = writer->status;
    diagnostic->confirmed_generation = writer->last_confirmed_generation;
    diagnostic->recovery.health = writer->status.health;
    if (writer->last_confirmed_generation) {
      diagnostic->pool = writer->diagnostic;
    }
  }
  if (status != PFS_OK) {
    if (!diagnostic->confirmed_generation) {
      diagnostic->writer = (struct pfs_writer_status){
        .health = PFS_WRITER_ACCESS_STOPPED, .failure = status};
      diagnostic->recovery.completion = PFS_STOPPED;
      diagnostic->recovery.operation_status = status;
      diagnostic->recovery.health = PFS_WRITER_ACCESS_STOPPED;
      if (!writer) {
        diagnostic->pool.candidate[0].status = status;
        diagnostic->pool.candidate[1].status = status;
      }
    }
    if (pool->state.data) {
      pfs_memory_free(memory, &pool->state);
    }
    pfs_writer_dispose(pool);
    *pool = (struct pfs_pool){0};
    return status;
  }
  pool->writer_busy = false;
  return PFS_OK;
}
