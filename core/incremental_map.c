/* SPDX-License-Identifier: MPL-2.0 */
#include "incremental_map.h"
#include "internal.h"
#include "canonical.h"

/* A numeric internal record has an eight-byte key-length/reserved field,
 * a reference and its eight-byte key after the common record header. */
#define MAP_INTERNAL_CHILDREN \
  ((PFS_BLOCK_SIZE - PFS_TREE_HEADER_SIZE) / \
   (PFS_RECORD_HEADER_SIZE + 8u + PFS_REFERENCE_SIZE + 8u + PFS_TREE_SLOT_SIZE))
#define DELTA_SLOT 128u
#define SOURCE_SLOT 128u
#define NODE_SLOT 128u

_Static_assert(sizeof(struct pfs_map_change) <= DELTA_SLOT, "raw delta slot");
_Static_assert(sizeof(struct pfs_incremental_source) <= SOURCE_SLOT, "source slot");
_Static_assert(sizeof(struct pfs_incremental_run) <= 32u, "run slot");
_Static_assert(sizeof(struct pfs_map_node) <= NODE_SLOT, "node slot");
_Static_assert(sizeof(size_t) <= 8u, "source work slot");
_Static_assert(sizeof(struct pfs_reusable_range) <= 16u, "range slot");

static bool
same_state(const struct pfs_allocation_record *a,
           const struct pfs_allocation_record *b)
{
  return a->state == b->state && a->charge == b->charge &&
    a->birth == b->birth && a->retirement == b->retirement &&
    !pfs_bytes_compare(a->owner.bytes, PFS_ID_SIZE, b->owner.bytes, PFS_ID_SIZE);
}

static bool
same_record(const struct pfs_allocation_record *a,
            const struct pfs_allocation_record *b)
{
  return a->first == b->first && a->count == b->count && same_state(a, b);
}

static enum pfs_status
validate_map(const struct pfs_record_context *context,
             const struct pfs_allocation_record *records, size_t count)
{
  if (!records || !count || count > PFS_ALLOCATION_COUNT_MAX) {
    return PFS_INVALID;
  }
  uint64_t next = 1;
  for (size_t i = 0; i < count; i++) {
    enum pfs_status status = pfs_allocation_record_validate(&records[i], context);
    if (status != PFS_OK) {
      return status;
    }
    if (records[i].first != next || (i && same_state(&records[i - 1], &records[i]))) {
      return PFS_CORRUPT;
    }
    next += records[i].count;
  }
  return next == context->block_count - 1 ? PFS_OK : PFS_CORRUPT;
}

static bool
region(size_t capacity, size_t *offset, uint64_t count, size_t slot)
{
  if (count > SIZE_MAX / slot || *offset > capacity ||
      (size_t)count * slot > capacity - *offset) {
    return false;
  }
  *offset += (size_t)count * slot;
  return true;
}

enum pfs_status
pfs_incremental_map_init(struct pfs_plan_arena *arena, struct pfs_incremental_map *map)
{
  if (!map) {
    return PFS_INVALID;
  }
  *map = (struct pfs_incremental_map){0};
  if (!arena || !arena->allocation.data || !arena->deltas ||
      arena->limits.pool_blocks <= arena->limits.catalog_union + 1 ||
      arena->limits.deltas > SIZE_MAX / DELTA_SLOT) {
    return PFS_INVALID;
  }
  uint64_t h = arena->limits.pool_blocks;
  uint64_t m = h - arena->limits.catalog_union - 1;
  if (h > (UINT64_MAX - 3u * PFS_PLAN_VOLUME_RETIRED - PFS_PLAN_VOLUME_NEW) / 4) {
    return PFS_LIMIT;
  }
  uint64_t raw = 4 * h + 3u * PFS_PLAN_VOLUME_RETIRED + PFS_PLAN_VOLUME_NEW;
  size_t capacity = (size_t)arena->limits.deltas * DELTA_SLOT;
  size_t offset = 0;
  struct pfs_incremental_map layout = {.arena = arena};
  layout.changes = (struct pfs_map_change *)(arena->deltas + offset);
  if (!region(capacity, &offset, raw, DELTA_SLOT)) {
    return PFS_LIMIT;
  }
  layout.change_capacity = (size_t)raw;
  layout.source = (struct pfs_incremental_source *)(arena->deltas + offset);
  if (!region(capacity, &offset, m, SOURCE_SLOT)) {
    return PFS_LIMIT;
  }
  layout.source_capacity = (size_t)m;
  layout.leaves = (size_t *)(arena->deltas + offset);
  if (!region(capacity, &offset, m, 8u)) {
    return PFS_LIMIT;
  }
  layout.runs = (struct pfs_incremental_run *)(arena->deltas + offset);
  if (!region(capacity, &offset, m, 32u)) {
    return PFS_LIMIT;
  }
  layout.nodes = (struct pfs_map_node *)(arena->deltas + offset);
  if (!region(capacity, &offset, h, NODE_SLOT)) {
    return PFS_LIMIT;
  }
  layout.ids = (uint64_t *)(arena->deltas + offset);
  if (!region(capacity, &offset, h, 8u)) {
    return PFS_LIMIT;
  }
  layout.ranges = (struct pfs_reusable_range *)(arena->deltas + offset);
  if (!region(capacity, &offset, h, 16u)) {
    return PFS_LIMIT;
  }
  *map = layout;
  return PFS_OK;
}

static size_t
record_at(const struct pfs_allocation_record *records, size_t count, uint64_t block)
{
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (records[middle].first + records[middle].count <= block) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

static enum pfs_status
source_allocation(const struct pfs_incremental_map *map,
                  const struct pfs_reference *reference)
{
  if (!pfs_allocatable_range(reference->block, 1, map->source_context.block_count)) {
    return PFS_CORRUPT;
  }
  size_t index = record_at(map->source_records, map->source_record_count, reference->block);
  if (index == map->source_record_count) {
    return PFS_CORRUPT;
  }
  const struct pfs_allocation_record *record = &map->source_records[index];
  return record->state == PFS_ALLOCATION_POOL && record->birth == reference->birth &&
    record->charge == PFS_CHARGE_PERMANENT ?
    PFS_OK : PFS_CORRUPT;
}

static enum pfs_status
source_append(struct pfs_incremental_map *map, struct pfs_reference reference,
              uint64_t minimum, size_t parent)
{
  for (size_t i = 0; i < map->source_count; i++) {
    if (map->source[i].reference.block == reference.block) {
      return PFS_CORRUPT;
    }
  }
  enum pfs_status status = source_allocation(map, &reference);
  if (status != PFS_OK) {
    return status;
  }
  if (map->source_count == map->source_capacity) {
    return PFS_LIMIT;
  }
  map->source[map->source_count++] = (struct pfs_incremental_source){
    .reference = reference, .minimum = minimum, .parent = parent,
    .replacement = SIZE_MAX,
  };
  return PFS_OK;
}

enum pfs_status
pfs_incremental_map_load(struct pfs_incremental_map *map,
  const struct pfs_block_reader *reader, const struct pfs_block_context *context,
  const struct pfs_allocation_record *source, size_t source_count,
  struct pfs_edit_workspace *workspace)
{
  if (!map || !map->arena || map->loaded || !reader || !context || !workspace ||
      !source_count || source_count > map->arena->limits.records ||
      reader->geometry.block_count != context->block_count) {
    return PFS_INVALID;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block_count,
    .selected_generation = context->selected_generation,
    .containing_birth = context->selected_generation, .features = context->features,
  };
  enum pfs_status status = validate_map(&record_context, source, source_count);
  if (status != PFS_OK) {
    return status;
  }
  map->source_context = *context;
  map->source_records = source;
  map->source_record_count = source_count;
  map->source_count = 0;
  map->leaf_count = 0;
  status = source_append(map, context->reference, 1, SIZE_MAX);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_edit_path *path = &workspace->path[0];
  size_t next_record = 0;
  /* Breadth-first children are contiguous; exact descending levels put every
   * source leaf in the same layer and preserve its key order. */
  for (size_t i = 0; i < map->source_count; i++) {
    struct pfs_incremental_source *node = &map->source[i];
    struct pfs_tree_context tree_context = {.block = *context, .kind = PFS_INDEX_ALLOCATION};
    tree_context.block.reference = node->reference;
    if (node->parent != SIZE_MAX) {
      const struct pfs_incremental_source *parent = &map->source[node->parent];
      tree_context.parent_level = parent->level;
      tree_context.block.referring_birth = parent->reference.birth;
    }
    status = pfs_block_read(reader, node->reference.block, 1, path->data, PFS_BLOCK_SIZE);
    if (status != PFS_OK) {
      return status;
    }
    status = pfs_tree_decode(path->data, PFS_BLOCK_SIZE, &tree_context, &path->tree);
    if (status != PFS_OK) {
      return status;
    }
    const struct pfs_tree *tree = &path->tree;
    if (pfs_get_u64(tree->minimum.bytes) != node->minimum ||
        tree->count > (tree->level ? MAP_INTERNAL_CHILDREN : PFS_INCREMENTAL_LEAF_RECORDS)) {
      return PFS_CORRUPT;
    }
    node->level = tree->level;
    record_context.containing_birth = tree->header.birth;
    struct pfs_encoded_record *encoded = workspace->records[0];
    for (size_t j = 0; j < tree->count; j++) {
      const struct pfs_tree_slot *slot = &tree->slots[j];
      encoded[j] = (struct pfs_encoded_record){path->data + slot->offset, slot->length};
      status = pfs_canonical_record_validate(encoded[j].data, encoded[j].length,
        PFS_INDEX_ALLOCATION, tree->level, &record_context);
      if (status != PFS_OK) {
        return status;
      }
    }
    status = pfs_tree_encode(workspace->path[1].data, PFS_BLOCK_SIZE,
      &tree_context, tree, encoded);
    if (status != PFS_OK) {
      return status;
    }
    if (pfs_bytes_compare(path->data, PFS_BLOCK_SIZE,
                         workspace->path[1].data, PFS_BLOCK_SIZE)) {
      return PFS_CORRUPT;
    }
    if (tree->level) {
      node->first_child = map->source_count;
      node->child_count = tree->count;
      for (size_t j = 0; j < tree->count; j++) {
        const struct pfs_tree_slot *slot = &tree->slots[j];
        struct pfs_internal_record child;
        status = pfs_internal_record_decode(path->data + slot->offset, slot->length,
          PFS_INDEX_ALLOCATION, &record_context, &child);
        if (status != PFS_OK) {
          return status;
        }
        status = source_append(map, child.child, pfs_get_u64(child.minimum.bytes), i);
        if (status != PFS_OK) {
          return status;
        }
      }
    } else {
      node->first_record = next_record;
      node->record_count = tree->count;
      if (tree->count > source_count - next_record) {
        return PFS_CORRUPT;
      }
      for (size_t j = 0; j < tree->count; j++) {
        const struct pfs_tree_slot *slot = &tree->slots[j];
        struct pfs_allocation_record decoded;
        status = pfs_allocation_record_decode(path->data + slot->offset, slot->length,
          &record_context, &decoded);
        if (status != PFS_OK) {
          return status;
        }
        if (!same_record(&decoded, &source[next_record++])) {
          return PFS_CORRUPT;
        }
      }
      const struct pfs_allocation_record *last = &source[next_record - 1];
      node->end = last->first + last->count;
      map->leaves[map->leaf_count++] = i;
    }
  }
  if (next_record != source_count) {
    return PFS_CORRUPT;
  }
  for (size_t i = map->source_count; i; i--) {
    struct pfs_incremental_source *node = &map->source[i - 1];
    if (!node->level) {
      continue;
    }
    const struct pfs_incremental_source *first = &map->source[node->first_child];
    node->first_record = first->first_record;
    uint64_t next = node->minimum;
    for (size_t j = 0; j < node->child_count; j++) {
      const struct pfs_incremental_source *child = &map->source[node->first_child + j];
      if (child->minimum != next) {
        return PFS_CORRUPT;
      }
      next = child->end;
      node->record_count += child->record_count;
    }
    node->end = next;
  }
  if (map->source[0].end != context->block_count - 1 ||
      map->source[0].record_count != source_count) {
    return PFS_CORRUPT;
  }
  map->loaded = true;
  return PFS_OK;
}

bool
pfs_incremental_map_retired(const struct pfs_incremental_map *map, uint64_t block)
{
  if (!map || !map->loaded) {
    return false;
  }
  for (size_t i = 0; i < map->source_count; i++) {
    if (map->source[i].reference.block == block) {
      return map->source[i].marked;
    }
  }
  return false;
}

static void
mark_path(struct pfs_incremental_map *map, size_t index)
{
  while (index != SIZE_MAX && !map->source[index].marked) {
    struct pfs_incremental_source *node = &map->source[index];
    node->marked = true;
    map->marked_count++;
    index = node->parent;
  }
}

static void
growth(struct pfs_incremental_map *map, size_t before)
{
  size_t added = map->marked_count - before;
  map->growth_passes++;
  if (added > map->largest_addition) {
    map->largest_addition = added;
  }
}

static enum pfs_status
make_runs(struct pfs_incremental_map *map,
          const struct pfs_allocation_record *candidate, size_t count)
{
  map->run_count = 0;
  size_t record = 0;
  for (size_t i = 0; i < map->leaf_count;) {
    const struct pfs_incremental_source *first = &map->source[map->leaves[i]];
    if (!first->marked) {
      i++;
      continue;
    }
    struct pfs_incremental_run *run = &map->runs[map->run_count++];
    run->first_leaf = i;
    do {
      i++;
    } while (i < map->leaf_count && map->source[map->leaves[i]].marked);
    run->leaf_count = i - run->first_leaf;
    const struct pfs_incremental_source *last = &map->source[map->leaves[i - 1]];
    while (record < count && candidate[record].first < first->minimum) {
      record++;
    }
    run->first_record = record;
    if (record == count || candidate[record].first != first->minimum) {
      return PFS_CORRUPT;
    }
    while (record < count && candidate[record].first < last->end) {
      record++;
    }
    run->record_count = record - run->first_record;
    const struct pfs_allocation_record *end = &candidate[record - 1];
    if (end->first + end->count != last->end) {
      return PFS_CORRUPT;
    }
  }
  return PFS_OK;
}

enum pfs_status
pfs_incremental_map_close(struct pfs_incremental_map *map,
  const struct pfs_allocation_record *candidate, size_t count, bool *again)
{
  if (!map || !map->loaded || !again || count > map->arena->limits.records ||
      map->source_context.selected_generation == UINT64_MAX) {
    return PFS_INVALID;
  }
  *again = false;
  map->stable = false;
  struct pfs_record_context context = {
    .block_count = map->source_context.block_count,
    .selected_generation = map->source_context.selected_generation + 1,
    .containing_birth = map->source_context.selected_generation + 1,
    .features = map->source_context.features,
  };
  enum pfs_status status = validate_map(&context, candidate, count);
  if (status != PFS_OK) {
    return status;
  }
  size_t before = map->marked_count;
  size_t record = 0;
  for (size_t i = 0; i < map->leaf_count; i++) {
    struct pfs_incremental_source *leaf = &map->source[map->leaves[i]];
    while (record < count && candidate[record].first + candidate[record].count <= leaf->minimum) {
      record++;
    }
    size_t first = record, end = record;
    while (end < count && candidate[end].first < leaf->end) {
      end++;
    }
    bool changed = end - first != leaf->record_count;
    for (size_t j = 0; !changed && j < leaf->record_count; j++) {
      changed = !same_record(&map->source_records[leaf->first_record + j],
                            &candidate[first + j]);
    }
    /* Comparing complete records, rather than only their clipped states, marks
     * both leaves at a source seam straddled by a final canonical record. */
    if (changed) {
      mark_path(map, map->leaves[i]);
    }
  }
  if (map->marked_count != before) {
    growth(map, before);
    *again = true;
    return PFS_OK;
  }
  status = make_runs(map, candidate, count);
  if (status != PFS_OK) {
    return status;
  }
  map->fallback = map->marked_count == map->source_count ? PFS_INCREMENTAL_GLOBAL : 0;
  for (size_t i = 0; i < map->run_count; i++) {
    const struct pfs_incremental_run *run = &map->runs[i];
    if (run->record_count >= run->leaf_count &&
        run->record_count <= PFS_INCREMENTAL_LEAF_RECORDS * run->leaf_count) {
      continue;
    }
    size_t neighbour = SIZE_MAX;
    if (run->first_leaf) {
      neighbour = run->first_leaf - 1;
    } else if (run->first_leaf + run->leaf_count < map->leaf_count) {
      neighbour = run->first_leaf + run->leaf_count;
    }
    if (neighbour != SIZE_MAX) {
      mark_path(map, map->leaves[neighbour]);
      map->redistribution_additions += map->marked_count - before;
      map->redistribution_leaves++;
      growth(map, before);
      *again = true;
      return PFS_OK;
    }
    map->failed_run_leaves = run->leaf_count;
    map->failed_run_records = run->record_count;
    map->fallback |= run->record_count < run->leaf_count ?
      PFS_INCREMENTAL_UNDERFLOW : PFS_INCREMENTAL_OVERFLOW;
    break;
  }
  map->stable = true;
  return PFS_OK;
}

static struct pfs_reference
new_reference(uint64_t block, uint64_t birth)
{
  return (struct pfs_reference){.block = block, .birth = birth,
    .type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION};
}

static enum pfs_status
validate_prefix(const struct pfs_incremental_map *map,
                const struct pfs_block_context *context,
                const struct pfs_allocation_record *candidate, size_t count,
                size_t total)
{
  uint64_t previous = 0;
  for (size_t i = 0; i < (size_t)map->arena->limits.pool_blocks; i++) {
    if (!pfs_allocatable_range(map->ids[i], 1, context->block_count) ||
        map->ids[i] <= previous) {
      return PFS_INVALID;
    }
    previous = map->ids[i];
  }
  size_t next = 0;
  for (size_t i = 0; i < count; i++) {
    const struct pfs_allocation_record *record = &candidate[i];
    if (record->state != PFS_ALLOCATION_POOL ||
        record->birth != context->selected_generation) {
      continue;
    }
    if (record->charge != PFS_CHARGE_PERMANENT || record->count > total - next) {
      return PFS_INVALID;
    }
    for (uint64_t j = 0; j < record->count; j++) {
      if (map->ids[next++] != record->first + j) {
        return PFS_INVALID;
      }
    }
  }
  return next == total ? PFS_OK : PFS_INVALID;
}

enum pfs_status
pfs_incremental_map_encode(struct pfs_incremental_map *map,
  const struct pfs_block_context *context,
  const struct pfs_allocation_record *candidate, size_t count,
  size_t catalog_count, struct pfs_edit_workspace *workspace,
  uint8_t record[PFS_BLOCK_SIZE], struct pfs_map_plan *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  *out = (struct pfs_map_plan){0};
  if (!map || !map->loaded || !map->stable || map->fallback || !context ||
      !workspace || !record || !map->marked_count ||
      count > map->arena->limits.records ||
      catalog_count > map->arena->limits.catalog_union ||
      catalog_count > PFS_PLAN_CATALOG_UNION ||
      context->block_count != map->source_context.block_count ||
      map->source_context.selected_generation == UINT64_MAX ||
      context->selected_generation != map->source_context.selected_generation + 1) {
    return PFS_INVALID;
  }
  size_t total = map->marked_count + catalog_count + 1;
  if (total > map->arena->limits.pool_blocks) {
    return PFS_LIMIT;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block_count,
    .selected_generation = context->selected_generation,
    .containing_birth = context->selected_generation, .features = context->features,
  };
  enum pfs_status status = validate_map(&record_context, candidate, count);
  if (status != PFS_OK) {
    return status;
  }
  status = validate_prefix(map, context, candidate, count, total);
  if (status != PFS_OK) {
    return status;
  }
  size_t replaced = 0;
  for (size_t i = 0; i < map->source_count; i++) {
    struct pfs_incremental_source *source = &map->source[i];
    if (source->marked) {
      source->replacement = replaced;
      map->nodes[replaced] = (struct pfs_map_node){
        .block = map->ids[replaced], .count = source->child_count, .level = source->level,
      };
      replaced++;
    }
  }
  for (size_t i = 0; i < map->run_count; i++) {
    const struct pfs_incremental_run *run = &map->runs[i];
    if (run->record_count < run->leaf_count ||
        run->record_count > PFS_INCREMENTAL_LEAF_RECORDS * run->leaf_count) {
      return PFS_INVALID;
    }
    size_t remaining = run->record_count, first = run->first_record;
    for (size_t j = 0; j < run->leaf_count; j++) {
      size_t left = run->leaf_count - j;
      size_t take = remaining / left + (remaining % left != 0);
      const struct pfs_incremental_source *source = &map->source[map->leaves[run->first_leaf + j]];
      struct pfs_map_node *node = &map->nodes[source->replacement];
      node->first = first;
      node->count = (uint16_t)take;
      node->minimum = candidate[first].first;
      first += take;
      remaining -= take;
    }
  }
  struct pfs_encoded_record *encoded = workspace->records[0];
  struct pfs_tree *tree = &workspace->path[0].tree;
  for (size_t i = map->source_count; i; i--) {
    const struct pfs_incremental_source *source = &map->source[i - 1];
    if (!source->marked) {
      continue;
    }
    struct pfs_map_node *node = &map->nodes[source->replacement];
    size_t offset = 0;
    for (size_t j = 0; j < node->count; j++) {
      size_t length;
      if (source->level) {
        const struct pfs_incremental_source *child = &map->source[source->first_child + j];
        struct pfs_internal_record internal = {.child = child->reference, .minimum = {.length = 8}};
        uint64_t minimum = child->minimum;
        if (child->marked) {
          const struct pfs_map_node *replacement = &map->nodes[child->replacement];
          internal.child = new_reference(replacement->block, context->selected_generation);
          minimum = replacement->minimum;
        }
        if (!j) {
          node->minimum = minimum;
        }
        pfs_put_u64(internal.minimum.bytes, minimum);
        status = pfs_internal_record_encode(record + offset, PFS_BLOCK_SIZE - offset,
          PFS_INDEX_ALLOCATION, &record_context, &internal, &length);
      } else {
        status = pfs_allocation_record_encode(record + offset, PFS_BLOCK_SIZE - offset,
          &record_context, &candidate[node->first + j], &length);
      }
      if (status != PFS_OK) {
        return status;
      }
      encoded[j] = (struct pfs_encoded_record){record + offset, length};
      offset += length;
    }
    *tree = (struct pfs_tree){
      .header = {.type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION,
        .pool = context->pool, .block = node->block, .birth = context->selected_generation},
      .kind = PFS_INDEX_ALLOCATION, .level = source->level, .count = node->count,
    };
    struct pfs_tree_context tree_context = {.block = *context, .kind = PFS_INDEX_ALLOCATION};
    tree_context.block.reference = new_reference(node->block, context->selected_generation);
    tree_context.block.referring_birth = context->selected_generation;
    status = pfs_tree_encode(map->arena->blocks + source->replacement * PFS_BLOCK_SIZE,
      PFS_BLOCK_SIZE, &tree_context, tree, encoded);
    if (status != PFS_OK) {
      return status;
    }
  }
  struct pfs_map_plan plan = {
    .root = new_reference(map->nodes[map->source[0].replacement].block, context->selected_generation),
    .pool_root_block = map->ids[map->marked_count + catalog_count],
    .catalog_count = catalog_count, .record_count = count,
    .node_count = map->marked_count, .allocation_count = total,
    .records = candidate, .nodes = map->nodes, .blocks = map->arena->blocks,
  };
  for (size_t i = 0; i < catalog_count; i++) {
    plan.catalog_blocks[i] = map->ids[map->marked_count + i];
  }
  *out = plan;
  return PFS_OK;
}
