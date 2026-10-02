/* SPDX-License-Identifier: MPL-2.0 */
#include "plan.h"
#include "internal.h"

#define MAP_LEAF_RECORDS 46u
#define MAP_INTERNAL_CHILDREN 65u
#define MAP_VECTOR_SLOT 80u
#define CLAIM_SLOT 96u
#define DIRECTORY_SLOT 48u
#define DELTA_SLOT 128u

_Static_assert(sizeof(struct pfs_allocation_record) <= MAP_VECTOR_SLOT, "allocation slot");
_Static_assert(sizeof(struct pfs_map_node) <= DELTA_SLOT, "map node slot");
_Static_assert(sizeof(struct pfs_map_change) <= DELTA_SLOT, "map change slot");

static uint64_t
ceil_div(uint64_t n, uint64_t d)
{
  return n / d + (n % d != 0);
}

static enum pfs_status
map_shape(uint64_t records, uint64_t levels[PFS_TREE_DEPTH_MAX],
          size_t *depth, uint64_t *nodes)
{
  if (!records || records > PFS_ALLOCATION_COUNT_MAX) {
    return PFS_LIMIT;
  }
  *nodes = 0;
  *depth = 0;
  uint64_t n = ceil_div(records, MAP_LEAF_RECORDS);
  for (;;) {
    if (*depth == PFS_TREE_DEPTH_MAX) {
      return PFS_LIMIT;
    }
    levels[(*depth)++] = n;
    *nodes += n;
    if (n == 1) {
      return PFS_OK;
    }
    n = ceil_div(n, MAP_INTERNAL_CHILDREN);
  }
}

enum pfs_status
pfs_plan_limits(uint64_t blocks, uint64_t extents, uint64_t metadata,
                uint16_t volumes, struct pfs_plan_limits *out)
{
  if (!out || blocks < PFS_POOL_BLOCKS_MIN || blocks > PFS_POOL_BLOCKS_MAX ||
      !volumes || volumes > PFS_VOLUME_MAX || !metadata || metadata > blocks - 2 ||
      extents > PFS_RECORD_COUNT_MAX) {
    return PFS_INVALID;
  }
  struct pfs_plan_limits limits = {
    .extents = extents, .metadata = metadata, .catalog_blocks = 4u * volumes - 2,
  };
  /* Input maxima bound all arithmetic below to less than 2^48. */
  limits.catalog_union = limits.catalog_blocks < PFS_PLAN_CATALOG_UNION ?
                         limits.catalog_blocks : PFS_PLAN_CATALOG_UNION;
  uint64_t k = 1 + 2 * (extents + metadata + 2 * PFS_PLAN_VOLUME_RETIRED);
  uint64_t upper = ceil_div(k + 2 * limits.catalog_blocks + 23 * limits.catalog_union + 47, 15);
  for (uint64_t h = limits.catalog_union + 2; h <= upper; h++) {
    uint64_t s = ceil_div(23 * (k + 2 * limits.catalog_blocks + 6 * h +
                              2 * limits.catalog_union + 4), 21);
    uint64_t levels[PFS_TREE_DEPTH_MAX], nodes;
    size_t depth;
    if (map_shape(s, levels, &depth, &nodes) != PFS_OK) {
      return PFS_LIMIT;
    }
    if (nodes + limits.catalog_union + 1 <= h) {
      limits.pool_blocks = h;
      limits.records = s;
      break;
    }
  }
  if (!limits.pool_blocks) {
    return PFS_LIMIT;
  }
  limits.permanent_pool = limits.catalog_blocks + limits.pool_blocks;
  limits.recovery_blocks = 3 * limits.pool_blocks + PFS_PLAN_VOLUME_NEW +
                           2 * PFS_PLAN_VOLUME_RETIRED;
  limits.claims = extents + metadata + limits.permanent_pool;
  limits.objects = metadata > PFS_RECORD_COUNT_MAX / 29 ? PFS_RECORD_COUNT_MAX : 29 * metadata;
  limits.deltas = 8 * (limits.pool_blocks + PFS_PLAN_VOLUME_NEW + PFS_PLAN_VOLUME_RETIRED);
  limits.arena_bytes = 3 * MAP_VECTOR_SLOT * limits.records +
    3 * CLAIM_SLOT * limits.claims + DIRECTORY_SLOT * limits.objects +
    PFS_BLOCK_SIZE * (limits.pool_blocks + PFS_PLAN_VOLUME_NEW) +
    DELTA_SLOT * limits.deltas + PFS_PLAN_SCRATCH_BYTES;
  *out = limits;
  return PFS_OK;
}

enum pfs_status
pfs_plan_arena_create(struct pfs_memory *memory, const struct pfs_plan_limits *limits,
                     struct pfs_plan_arena *arena)
{
  if (!arena || arena->allocation.data || !limits || !limits->pool_blocks ||
      limits->arena_bytes > SIZE_MAX) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_memory_allocate(memory, (size_t)limits->arena_bytes,
                                               _Alignof(max_align_t), &arena->allocation);
  if (status != PFS_OK) {
    return status;
  }
  arena->limits = *limits;
  uint8_t *cursor = arena->allocation.data;
  for (size_t i = 0; i < 3; i++) {
    arena->maps[i] = (struct pfs_allocation_record *)cursor;
    cursor += MAP_VECTOR_SLOT * limits->records;
  }
  for (size_t i = 0; i < 3; i++) {
    arena->claims[i] = cursor;
    cursor += CLAIM_SLOT * limits->claims;
  }
  arena->directories = cursor;
  cursor += DIRECTORY_SLOT * limits->objects;
  arena->blocks = cursor;
  cursor += PFS_BLOCK_SIZE * (limits->pool_blocks + PFS_PLAN_VOLUME_NEW);
  arena->deltas = cursor;
  cursor += DELTA_SLOT * limits->deltas;
  arena->scratch = cursor;
  return PFS_OK;
}

enum pfs_status
pfs_plan_arena_destroy(struct pfs_plan_arena *arena)
{
  if (!arena) {
    return PFS_INVALID;
  }
  enum pfs_status status = arena->allocation.data ?
    pfs_memory_free(arena->allocation.owner, &arena->allocation) : PFS_OK;
  if (status == PFS_OK) {
    *arena = (struct pfs_plan_arena){0};
  }
  return status;
}

static bool
same_allocation(const struct pfs_allocation_record *a, const struct pfs_allocation_record *b)
{
  return a->state == b->state && a->charge == b->charge && a->birth == b->birth &&
    a->retirement == b->retirement &&
    pfs_bytes_compare(a->owner.bytes, PFS_ID_SIZE, b->owner.bytes, PFS_ID_SIZE) == 0;
}

static void
swap_changes(struct pfs_map_change *first, struct pfs_map_change *second)
{
  struct pfs_map_change temporary;
  pfs_bytes_copy(&temporary, first, sizeof(temporary));
  pfs_bytes_copy(first, second, sizeof(*first));
  pfs_bytes_copy(second, &temporary, sizeof(*second));
}

static void
sift_changes(struct pfs_map_change *changes, size_t root, size_t count)
{
  while (root < count / 2) {
    size_t child = 2 * root + 1;
    if (child + 1 < count && changes[child + 1].before.first > changes[child].before.first) {
      child++;
    }
    if (changes[root].before.first >= changes[child].before.first) {
      return;
    }
    swap_changes(&changes[root], &changes[child]);
    root = child;
  }
}

void
pfs_plan_sort_changes(struct pfs_map_change *changes, size_t count)
{
  for (size_t i = count / 2; i; i--) {
    sift_changes(changes, i - 1, count);
  }
  for (size_t remaining = count; remaining > 1; remaining--) {
    swap_changes(&changes[0], &changes[remaining - 1]);
    sift_changes(changes, 0, remaining - 1);
  }
}

static enum pfs_status
map_validate(const struct pfs_record_context *context,
             const struct pfs_allocation_record *base, size_t count)
{
  if (!base || !count || count > PFS_ALLOCATION_COUNT_MAX) {
    return PFS_INVALID;
  }
  uint64_t next = 1;
  for (size_t i = 0; i < count; i++) {
    enum pfs_status status = pfs_allocation_record_validate(&base[i], context);
    if (status != PFS_OK) {
      return status;
    }
    if (base[i].first != next || (i && same_allocation(&base[i - 1], &base[i]))) {
      return PFS_CORRUPT;
    }
    next += base[i].count;
  }
  return next == context->block_count - 1 ? PFS_OK : PFS_CORRUPT;
}

static enum pfs_status
append(struct pfs_allocation_record *out, size_t capacity, size_t *count,
       struct pfs_allocation_record *last, struct pfs_allocation_record record)
{
  if (!record.count) {
    return PFS_OK;
  }
  if (*count && same_allocation(last, &record) &&
      last->first + last->count == record.first) {
    last->count += record.count;
    if (out) {
      out[*count - 1] = *last;
    }
    return PFS_OK;
  }
  if (*count == capacity) {
    return PFS_LIMIT;
  }
  *last = record;
  if (out) {
    out[*count] = record;
  }
  (*count)++;
  return PFS_OK;
}

static enum pfs_status
map_apply(const struct pfs_record_context *context,
                   const struct pfs_allocation_record *base, size_t base_count,
                   const struct pfs_map_change *changes, size_t change_count,
                   struct pfs_allocation_record *out, size_t capacity, size_t *count)
{
  if (!context || !count || (change_count && !changes) || !capacity ||
      capacity > PFS_ALLOCATION_COUNT_MAX) {
    return PFS_INVALID;
  }
  enum pfs_status status = map_validate(context, base, base_count);
  if (status != PFS_OK) {
    return status;
  }
  uint64_t end = 0;
  for (size_t i = 0; i < change_count; i++) {
    const struct pfs_map_change *change = &changes[i];
    if (pfs_allocation_record_validate(&change->before, context) != PFS_OK ||
        pfs_allocation_record_validate(&change->after, context) != PFS_OK ||
        change->before.first != change->after.first ||
        change->before.count != change->after.count || change->before.first < end) {
      return PFS_INVALID;
    }
    end = change->before.first + change->before.count;
  }
  size_t produced = 0, change_index = 0;
  struct pfs_allocation_record last = {0};
  for (size_t i = 0; i < base_count; i++) {
    uint64_t position = base[i].first;
    uint64_t limit = position + base[i].count;
    while (position < limit) {
      struct pfs_allocation_record record = base[i];
      record.first = position;
      record.count = limit - position;
      if (change_index < change_count) {
        const struct pfs_map_change *change = &changes[change_index];
        uint64_t change_end = change->before.first + change->before.count;
        if (position >= change->before.first) {
          if (!same_allocation(&base[i], &change->before)) {
            return PFS_INVALID;
          }
          record = change->after;
          record.first = position;
          record.count = (change_end < limit ? change_end : limit) - position;
          if (position + record.count == change_end) {
            change_index++;
          }
        } else if (change->before.first < limit) {
          record.count = change->before.first - position;
        }
      }
      status = append(out, capacity, &produced, &last, record);
      if (status != PFS_OK) {
        return status;
      }
      position += record.count;
    }
  }
  if (change_index != change_count) {
    return PFS_INVALID;
  }
  *count = produced;
  return PFS_OK;
}

enum pfs_status
pfs_plan_map_apply(const struct pfs_record_context *context,
                   const struct pfs_allocation_record *base, size_t base_count,
                   const struct pfs_map_change *changes, size_t change_count,
                   struct pfs_allocation_record *out, size_t capacity, size_t *count)
{
  if (!out) {
    return PFS_INVALID;
  }
  return map_apply(context, base, base_count, changes, change_count, out, capacity, count);
}

enum pfs_status
pfs_plan_map_count(const struct pfs_record_context *context,
                   const struct pfs_allocation_record *base, size_t base_count,
                   const struct pfs_map_change *changes, size_t change_count,
                   size_t capacity, size_t *count)
{
  return map_apply(context, base, base_count, changes, change_count, NULL, capacity, count);
}

static enum pfs_status
bulk_shape(size_t base_count, size_t catalog_count, uint64_t levels[PFS_TREE_DEPTH_MAX],
           size_t *depth, uint64_t *nodes, uint64_t *records)
{
  if (!base_count || base_count > PFS_ALLOCATION_COUNT_MAX ||
      catalog_count > PFS_PLAN_CATALOG_UNION) {
    return PFS_INVALID;
  }
  *records = ceil_div(23 * (base_count + 2 * catalog_count + 4), 21);
  return map_shape(*records, levels, depth, nodes);
}

enum pfs_status
pfs_plan_map_bulk_size(size_t base_count, size_t catalog_count, size_t *count)
{
  if (!count) {
    return PFS_INVALID;
  }
  uint64_t levels[PFS_TREE_DEPTH_MAX], nodes, records;
  size_t depth;
  enum pfs_status status = bulk_shape(base_count, catalog_count, levels, &depth, &nodes, &records);
  if (status == PFS_OK) {
    *count = (size_t)nodes;
  }
  return status;
}

static struct pfs_reference
map_reference(uint64_t block, uint64_t birth)
{
  return (struct pfs_reference){block, birth, PFS_BLOCK_TREE, PFS_FORMAT_VERSION};
}

static enum pfs_status
encode_nodes(struct pfs_plan_arena *arena, const struct pfs_block_context *context,
             const uint64_t levels[PFS_TREE_DEPTH_MAX], size_t depth,
             struct pfs_map_plan *plan)
{
  struct pfs_map_node *nodes = (struct pfs_map_node *)arena->deltas;
  struct pfs_record_context record_context = {
    .block_count = context->block_count, .selected_generation = context->selected_generation,
    .containing_birth = context->selected_generation,
  };
  uint8_t records[PFS_BLOCK_SIZE];
  struct pfs_encoded_record encoded[MAP_INTERNAL_CHILDREN];
  size_t index = 0, previous_start = 0;
  for (size_t level = 0; level < depth; level++) {
    size_t start = index;
    size_t remaining = level ? (size_t)levels[level - 1] : plan->record_count;
    size_t first = level ? previous_start : 0;
    for (uint64_t n = 0; n < levels[level]; n++, index++) {
      struct pfs_map_node *node = &nodes[index];
      size_t left = (size_t)(levels[level] - n);
      /* Balanced counts preserve nonempty leaves and >=2 internal children,
       * including the previously planned extra leaves of the finite envelope. */
      size_t count = (size_t)ceil_div(remaining, left);
      if (!count || count > (level ? MAP_INTERNAL_CHILDREN : MAP_LEAF_RECORDS) ||
          (level && count < 2)) {
        return PFS_LIMIT;
      }
      node->first = first;
      node->count = (uint16_t)count;
      node->level = (uint16_t)level;
      node->minimum = level ? nodes[first].minimum : plan->records[first].first;
      size_t offset = 0;
      for (size_t j = 0; j < count; j++) {
        size_t length = 0;
        enum pfs_status status;
        if (level) {
          const struct pfs_map_node *child = &nodes[first + j];
          struct pfs_internal_record internal = {
            .child = map_reference(child->block, context->selected_generation),
            .minimum = {.length = 8},
          };
          pfs_put_u64(internal.minimum.bytes, child->minimum);
          status = pfs_internal_record_encode(records + offset, sizeof(records) - offset,
            PFS_INDEX_ALLOCATION, &record_context, &internal, &length);
        } else {
          status = pfs_allocation_record_encode(records + offset, sizeof(records) - offset,
            &record_context, &plan->records[first + j], &length);
        }
        if (status != PFS_OK) {
          return status;
        }
        encoded[j] = (struct pfs_encoded_record){records + offset, length};
        offset += length;
      }
      struct pfs_tree tree = {
        .header = {.type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION,
          .pool = context->pool, .block = node->block, .birth = context->selected_generation},
        .kind = PFS_INDEX_ALLOCATION, .level = (uint16_t)level, .count = (uint16_t)count,
      };
      struct pfs_tree_context tree_context = {
        .block = *context, .kind = PFS_INDEX_ALLOCATION,
      };
      tree_context.block.referring_birth = context->selected_generation;
      tree_context.block.reference = map_reference(node->block, context->selected_generation);
      enum pfs_status status = pfs_tree_encode(arena->blocks + index * PFS_BLOCK_SIZE,
        PFS_BLOCK_SIZE, &tree_context, &tree, encoded);
      if (status != PFS_OK) {
        return status;
      }
      first += count;
      remaining -= count;
    }
    previous_start = start;
  }
  plan->root = map_reference(nodes[index - 1].block, context->selected_generation);
  return PFS_OK;
}

enum pfs_status
pfs_plan_map_build(struct pfs_plan_arena *arena, const struct pfs_block_context *context,
                   const struct pfs_allocation_record *base, size_t base_count,
                   const struct pfs_reusable_range *reusable, size_t reusable_count,
                   size_t catalog_count, struct pfs_map_plan *out)
{
  if (!out) {
    return PFS_INVALID;
  }
  *out = (struct pfs_map_plan){0};
  if (!arena || !arena->allocation.data || !context || !reusable || !reusable_count ||
      !context->selected_generation || catalog_count > arena->limits.catalog_union ||
      catalog_count > PFS_PLAN_CATALOG_UNION ||
      base_count > arena->limits.records) {
    return PFS_INVALID;
  }
  struct pfs_record_context record_context = {
    .block_count = context->block_count, .selected_generation = context->selected_generation,
    .containing_birth = context->selected_generation,
  };
  enum pfs_status status = map_validate(&record_context, base, base_count);
  if (status != PFS_OK) {
    return status;
  }
  for (size_t i = 0; i < base_count; i++) {
    if (base[i].state == PFS_ALLOCATION_POOL && base[i].birth == context->selected_generation) {
      return PFS_INVALID;
    }
  }
  uint64_t s, levels[PFS_TREE_DEPTH_MAX], nodes;
  size_t depth;
  status = bulk_shape(base_count, catalog_count, levels, &depth, &nodes, &s);
  if (status != PFS_OK) {
    return status;
  }
  size_t total = (size_t)nodes + catalog_count + 1;
  if (total > arena->limits.pool_blocks || s > arena->limits.records) {
    return PFS_LIMIT;
  }
  struct pfs_map_node *allocated = (struct pfs_map_node *)arena->deltas;
  uint64_t prior_end = 1;
  size_t selected = 0, base_index = 0;
  for (size_t i = 0; i < reusable_count; i++) {
    const struct pfs_reusable_range *range = &reusable[i];
    if (!pfs_allocatable_range(range->first, range->count, context->block_count) ||
        range->first < prior_end) {
      return PFS_INVALID;
    }
    prior_end = range->first + range->count;
    while (base_index < base_count && base[base_index].first + base[base_index].count <= range->first) {
      base_index++;
    }
    if (base_index == base_count || base[base_index].state != PFS_ALLOCATION_FREE ||
        prior_end > base[base_index].first + base[base_index].count) {
      return PFS_INVALID;
    }
    uint64_t take = range->count;
    if (take > total - selected) {
      take = total - selected;
    }
    for (uint64_t j = 0; j < take; j++) {
      allocated[selected++] = (struct pfs_map_node){.block = range->first + j};
    }
  }
  if (selected != total) {
    return PFS_LIMIT;
  }
  /* Each canonical source interval expands independently: candidate-born pool
   * claims cannot merge with a source neighbour. Count first, then expand from
   * the end so maps[2] can also be the input without a fourth map vector. */
  size_t produced = 0, next = 0;
  for (size_t i = 0; i < base_count; i++) {
    uint64_t position = base[i].first;
    uint64_t end = position + base[i].count;
    while (next < selected && allocated[next].block < end) {
      produced += allocated[next].block > position;
      position = allocated[next++].block + 1;
      while (next < selected && allocated[next].block == position) {
        position++;
        next++;
      }
      produced++;
    }
    produced += position < end;
  }
  if (produced > arena->limits.records) {
    return PFS_LIMIT;
  }
  size_t output = produced;
  next = selected;
  for (size_t i = base_count; i; i--) {
    struct pfs_allocation_record source = base[i - 1];
    uint64_t end = source.first + source.count;
    while (next && allocated[next - 1].block >= source.first) {
      uint64_t last = allocated[--next].block;
      if (last + 1 < end) {
        struct pfs_allocation_record suffix = source;
        suffix.first = last + 1;
        suffix.count = end - suffix.first;
        arena->maps[2][--output] = suffix;
      }
      uint64_t first = last;
      while (next && allocated[next - 1].block + 1 == first &&
             allocated[next - 1].block >= source.first) {
        first = allocated[--next].block;
      }
      arena->maps[2][--output] = (struct pfs_allocation_record){
        .first = first, .count = last - first + 1,
        .state = PFS_ALLOCATION_POOL, .birth = context->selected_generation,
      };
      end = first;
    }
    if (end > source.first) {
      source.count = end - source.first;
      arena->maps[2][--output] = source;
    }
  }
  if (produced > s || produced < levels[0]) {
    return PFS_LIMIT;
  }
  struct pfs_map_plan plan = {
    .pool_root_block = allocated[total - 1].block,
    .catalog_count = catalog_count, .record_count = produced,
    .node_count = (size_t)nodes, .allocation_count = total,
    .records = arena->maps[2], .nodes = allocated, .blocks = arena->blocks,
  };
  for (size_t i = 0; i < catalog_count; i++) {
    plan.catalog_blocks[i] = allocated[nodes + i].block;
  }
  status = encode_nodes(arena, context, levels, depth, &plan);
  if (status == PFS_OK) {
    *out = plan;
  }
  return status;
}
