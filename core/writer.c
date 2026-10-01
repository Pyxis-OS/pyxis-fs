/* SPDX-License-Identifier: MPL-2.0 */
#include "writer.h"
#include "canonical.h"
#include "file.h"

/* Planning performs backing reads only. Keep its I/O failures distinct from
 * replacement writes/flushes and the slot publication uncertainty window. */
enum publication_phase {
  PUBLICATION_PLANNING,
  PUBLICATION_REPLACEMENTS,
  PUBLICATION_SLOT,
};

struct publication {
  struct pfs_edit_workspace editor;
  struct pfs_edit_slot catalog[PFS_PLAN_CATALOG_PATH];
  struct pfs_reference retired[PFS_PLAN_CATALOG_PATH];
  struct pfs_reference path[PFS_PLAN_CATALOG_PATH];
  size_t path_count;
  struct pfs_map_plan map;
  uint8_t root[PFS_BLOCK_SIZE];
  uint8_t slot[PFS_BLOCK_SIZE];
  uint8_t record[PFS_BLOCK_SIZE];
};

_Static_assert(sizeof(struct publication) <= PFS_PLAN_SCRATCH_BYTES / 4,
  "publication must not overlap the final quarter used by mutation staging");
_Static_assert(2 * PFS_VOLUME_MAX * sizeof(struct check_volume) <= PFS_PLAN_SCRATCH_BYTES / 2,
  "admission owns only the lower half of scratch");

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
change_add(struct pfs_writer *writer, size_t *count, uint64_t first, uint64_t length,
           uint8_t state, const struct pfs_volume_id *owner, uint64_t birth, uint8_t charge)
{
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  struct pfs_map_change *changes = (struct pfs_map_change *)writer->arena.deltas;
  const struct pfs_allocation_record *before = allocation_at(source, first);
  if (!length || !before || length > before->first + before->count - first ||
      *count >= writer->arena.limits.deltas * 128 / sizeof(*changes)) {
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
  if (claim->type == PFS_BLOCK_POOL ||
      (claim->type == PFS_BLOCK_TREE && claim->kind == PFS_INDEX_ALLOCATION)) {
    return true;
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
  for (;;) {
    if (publication->path_count == PFS_PLAN_CATALOG_PATH) {
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
    publication->path[publication->path_count++] = current;
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
  publication->path_count = 0;
  size_t volume_index = SIZE_MAX;
  if (batch) {
    for (size_t i = 0; i < candidate->volume_count; i++) {
      if (same_id(candidate->volumes[i].record.id.bytes, batch->volume.record.id.bytes)) {
        volume_index = i;
        candidate->volumes[i] = batch->volume;
      }
    }
    if (volume_index == SIZE_MAX) {
      return PFS_INVALID;
    }
  }
  size_t changes_count = 0;
  enum pfs_status status;
  /* Eligible pool debt is always reclaimed. Volume debt is either all protected
   * (advance) or all unprotected (free); both retained states remain authoritative. */
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
      status = change_add(writer, &changes_count, block, 1, PFS_ALLOCATION_FREE, NULL, 0, 0);
      if (status != PFS_OK) {
        return status;
      }
      if (volume) {
        for (size_t j = 0; j < candidate->volume_count; j++) {
          if (same_id(record->owner.bytes, candidate->volumes[j].record.id.bytes)) {
            if (volume_index != SIZE_MAX && volume_index != j) {
              return PFS_CORRUPT;
            }
            volume_index = j;
          }
        }
      }
    }
  }
  if (volume_index != SIZE_MAX) {
    status = catalog_path(writer, publication, &candidate->volumes[volume_index].record.id);
    if (status != PFS_OK) {
      return status;
    }
  }
  for (size_t i = 0; i < source->claim_count; i++) {
    const struct check_claim *claim = &source->claims[i];
    if (pool_removed(claim, publication)) {
      status = change_add(writer, &changes_count, claim->first, claim->count,
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
      status = change_add(writer, &changes_count, claim->first, claim->count,
        PFS_ALLOCATION_RETIRED, &claim->volume, claim->birth, PFS_CHARGE_ORDINARY);
      if (status != PFS_OK) {
        return status;
      }
    }
    for (size_t i = 0; i < batch->block_count; i++) {
      status = change_add(writer, &changes_count, batch->blocks[i].block, 1,
        PFS_ALLOCATION_VOLUME, &batch->volume.record.id, generation, PFS_CHARGE_PERMANENT);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  struct pfs_map_change *changes = (struct pfs_map_change *)writer->arena.deltas;
  pfs_plan_sort_changes(changes, changes_count);
  struct pfs_block_context context = {.block_count = writer->backing->reader.geometry.block_count,
    .selected_generation = generation, .referring_birth = generation,
    .pool = source->candidate.superblock.header.pool,
    .features = source->candidate.superblock.features};
  struct pfs_record_context record_context = {.block_count = context.block_count,
    .selected_generation = generation, .containing_birth = generation, .features = context.features};
  status = pfs_plan_map_apply(&record_context, source->maps, source->map_count,
    changes, changes_count, candidate->maps, (size_t)writer->arena.limits.records, &candidate->map_count);
  if (status != PFS_OK) {
    return status;
  }
  /* Map construction owns the front of deltas. Bounded reusable IDs/ranges live
   * after its H descriptors, replacing the no-longer-needed interval deltas. */
  size_t demand = (size_t)writer->arena.limits.pool_blocks;
  uint64_t *ids = (uint64_t *)(writer->arena.deltas + demand * sizeof(struct pfs_map_node));
  struct pfs_reusable_range *ranges = (struct pfs_reusable_range *)(ids + demand);
  status = select_free(writer, ids, demand, batch);
  if (status != PFS_OK) {
    return status;
  }
  size_t range_count = 0;
  for (size_t i = 0; i < demand; i++) {
    if (range_count && ranges[range_count - 1].first + ranges[range_count - 1].count == ids[i]) {
      ranges[range_count - 1].count++;
    } else {
      ranges[range_count++] = (struct pfs_reusable_range){ids[i], 1};
    }
  }
  /* Allocation IDs may be contiguous across a base-free boundary introduced by
   * this publication, but never across any newly live or retired allocation. */
  status = pfs_plan_map_build(&writer->arena, &context, candidate->maps, candidate->map_count,
    ranges, range_count, publication->path_count, &publication->map);
  if (status != PFS_OK) {
    return status;
  }
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
  if (volume_index != SIZE_MAX) {
    size_t length;
    status = pfs_volume_record_encode(publication->record, sizeof(publication->record),
      &record_context, &candidate->volumes[volume_index].record, &length);
    if (status != PFS_OK) {
      return status;
    }
    struct pfs_key key = {.length = PFS_ID_SIZE};
    pfs_bytes_copy(key.bytes, candidate->volumes[volume_index].record.id.bytes, PFS_ID_SIZE);
    for (size_t i = 0; i < publication->path_count; i++) {
      publication->catalog[i] = (struct pfs_edit_slot){.block = publication->map.catalog_blocks[i]};
    }
    struct pfs_edit_candidate edit = {.reader = &writer->backing->reader,
      .context = {.block = context, .kind = PFS_INDEX_VOLUMES}, .birth = generation,
      .root = source->candidate.root.volumes, .slots = publication->catalog,
      .slot_capacity = publication->path_count, .retired = publication->retired,
      .retired_capacity = PFS_PLAN_CATALOG_PATH};
    struct pfs_encoded_record encoded = {publication->record, length};
    status = pfs_edit_tree(&edit, &publication->editor, PFS_EDIT_UPDATE, &key, &encoded);
    if (status != PFS_OK || edit.retired_count != publication->path_count) {
      return status != PFS_OK ? status : PFS_CORRUPT;
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
  if (status == PFS_OK) {
    status = pfs_pool_root_encode(publication->root, PFS_BLOCK_SIZE, &context, root);
  }
  if (status == PFS_OK) {
    status = pfs_superblock_encode(publication->slot, PFS_BLOCK_SIZE, &candidate->candidate.superblock);
  }
  return status;
}

static enum pfs_status
publish(struct pfs_writer *writer, const struct pfs_batch *batch, enum publication_phase *phase)
{
  struct publication *publication = (struct publication *)(writer->arena.scratch +
    PFS_PLAN_SCRATCH_BYTES / 2);
  *phase = PUBLICATION_PLANNING;
  enum pfs_status status = build_publication(writer, batch, publication);
  if (status != PFS_OK) {
    return status;
  }
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
    return status;
  }
  struct pfs_admit_state *candidate = &writer->states[2];
  *phase = PUBLICATION_SLOT;
  status = pfs_block_write(writer->backing, candidate->candidate.superblock.header.block, 1,
    publication->slot, PFS_BLOCK_SIZE);
  if (status == PFS_OK) {
    status = pfs_block_flush(writer->backing);
  }
  if (status != PFS_OK) {
    return status;
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
  return PFS_OK;
}

static enum pfs_status
drain(struct pfs_writer *writer, struct pfs_write_result *result)
{
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
  if (writer->status.drain_pending || source->candidate.superblock.header.birth > UINT64_MAX - 3) {
    pfs_writer_end(pool, PFS_OK);
    return PFS_LIMIT;
  }
  writer->batch = (struct pfs_batch){.generation = source->candidate.superblock.header.birth + 1,
    .available_count = PFS_PLAN_VOLUME_NEW};
  status = select_free(writer, writer->batch.available, PFS_PLAN_VOLUME_NEW, NULL);
  if (status != PFS_OK || source->reusable_blocks < writer->arena.limits.pool_blocks + PFS_PLAN_VOLUME_NEW) {
    pfs_writer_end(pool, PFS_OK);
    return status != PFS_OK ? status : PFS_NO_SPACE;
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
  if (batch->available_count != PFS_PLAN_VOLUME_NEW ||
      !batch->block_count || batch->block_count > PFS_PLAN_VOLUME_NEW || !batch->blocks ||
      !batch->add || !batch->add_count || batch->add_count > PFS_PLAN_VOLUME_NEW ||
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
    status = drain(writer, result);
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
        status = drain(writer, &diagnostic->recovery);
        if (status == PFS_OK) {
          pool->writer_busy = false;
          status = pfs_orphan_recover(pool, &diagnostic->recovery);
          pool->writer_busy = true;
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
