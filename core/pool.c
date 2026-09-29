#include <pyxis_fs/pool.h>
#include <pyxis_fs/tree.h>

#include "internal.h"

struct pool_state {
  struct pfs_pool_diagnostic diagnostic;
};

static struct pfs_block_context
block_context(const struct pfs_superblock *superblock,
              const struct pfs_reference *reference, uint64_t birth)
{
  return (struct pfs_block_context){
    .block_count = superblock->block_count,
    .selected_generation = superblock->header.birth,
    .referring_birth = birth,
    .pool = superblock->header.pool,
    .reference = *reference,
    .features = superblock->features,
  };
}

static struct pfs_record_context
record_context(const struct pfs_superblock *superblock, uint64_t birth)
{
  return (struct pfs_record_context){
    .block_count = superblock->block_count,
    .selected_generation = superblock->header.birth,
    .containing_birth = birth,
    .features = superblock->features,
  };
}

static enum pfs_status
candidate_read(const struct pfs_block_reader *reader, uint64_t slot,
               uint8_t *raw, struct pfs_pool_candidate *candidate)
{
  enum pfs_status status = pfs_block_read(reader, slot, 1, raw, PFS_BLOCK_SIZE);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_superblock superblock;
  status = pfs_superblock_decode(raw, PFS_BLOCK_SIZE,
                                reader->geometry.block_count, slot, &superblock);
  if (status != PFS_OK) {
    return status;
  }
  uint8_t data[PFS_BLOCK_SIZE];
  status = pfs_block_read(reader, superblock.root.block, 1, data, sizeof(data));
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_block_context context = block_context(&superblock, &superblock.root,
                                                  superblock.header.birth);
  struct pfs_pool_root root;
  status = pfs_pool_root_decode(data, sizeof(data), &context, &root);
  if (status != PFS_OK) {
    return status;
  }
  const struct pfs_reference references[] = { root.volumes, root.volume_names, root.allocation };
  for (uint16_t i = 0; i < 3; ++i) {
    status = pfs_block_read(reader, references[i].block, 1, data, sizeof(data));
    if (status != PFS_OK) {
      return status;
    }
    struct pfs_tree_context tree_context = {
      .block = block_context(&superblock, &references[i], root.header.birth),
      .kind = (uint16_t)(PFS_INDEX_VOLUMES + i),
    };
    struct pfs_tree tree;
    status = pfs_tree_decode(data, sizeof(data), &tree_context, &tree);
    if (status != PFS_OK) {
      return status;
    }
  }
  candidate->superblock = superblock;
  candidate->root = root;
  return PFS_OK;
}

static unsigned
failure_priority(enum pfs_status status)
{
  switch (status) {
  case PFS_IO: return 6;
  case PFS_NO_MEMORY: return 5;
  case PFS_LIMIT: return 4;
  case PFS_UNSUPPORTED: return 3;
  case PFS_CORRUPT: return 2;
  case PFS_ABSENT: return 1;
  case PFS_OK: return 0;
  default: return 7;
  }
}

enum pfs_status
pfs_pool_open(struct pfs_pool *pool, const struct pfs_block_reader *reader,
              struct pfs_memory *memory, struct pfs_pool_diagnostic *diagnostic)
{
  if (!pool || !reader || !reader->read || !memory || !diagnostic ||
      pool->reader || pool->memory || pool->state.owner || pool->state.data ||
      pool->state.size || pool->state.alignment || reader->geometry.block_count < 3 ||
      !reader->geometry.max_transfer_blocks || !memory->allocate || !memory->free ||
      !memory->limit || memory->limit > PFS_MEMORY_MAX || memory->used > memory->limit) {
    return PFS_INVALID;
  }
  struct pfs_pool_diagnostic result = { .selected = PFS_POOL_NO_SELECTION };
  uint8_t raw[2][PFS_BLOCK_SIZE];
  result.candidate[0].status = candidate_read(reader, 0, raw[0], &result.candidate[0]);
  result.candidate[1].status = candidate_read(reader, reader->geometry.block_count - 1,
                                             raw[1], &result.candidate[1]);
  enum pfs_status first = result.candidate[0].status;
  enum pfs_status second = result.candidate[1].status;
  enum pfs_status status = failure_priority(first) >= failure_priority(second) ? first : second;
  if (failure_priority(status) > failure_priority(PFS_CORRUPT)) {
    *diagnostic = result;
    return status;
  }
  if (first != PFS_OK && second != PFS_OK) {
    *diagnostic = result;
    return status;
  }
  if (first == PFS_OK && second == PFS_OK) {
    const struct pfs_superblock *a = &result.candidate[0].superblock;
    const struct pfs_superblock *b = &result.candidate[1].superblock;
    bool disagree = a->block_count != b->block_count ||
      pfs_bytes_compare(a->header.pool.bytes, PFS_ID_SIZE, b->header.pool.bytes, PFS_ID_SIZE);
    if (a->header.birth == b->header.birth) {
      /* Compare every byte, including ignored reserved extensions. */
      pfs_bytes_zero(raw[0] + 20, 4);
      pfs_bytes_zero(raw[1] + 20, 4);
      pfs_bytes_zero(raw[0] + 40, 8);
      pfs_bytes_zero(raw[1] + 40, 8);
      disagree = disagree || pfs_bytes_compare(raw[0], PFS_BLOCK_SIZE, raw[1], PFS_BLOCK_SIZE);
    }
    if (disagree) {
      result.ambiguous = true;
      *diagnostic = result;
      return PFS_CORRUPT;
    }
    result.selected = a->header.birth >= b->header.birth ? 0 : 1;
  } else {
    result.selected = first == PFS_OK ? 0 : 1;
    result.degraded = true;
  }
  status = pfs_memory_allocate(memory, sizeof(struct pool_state), _Alignof(struct pool_state),
                               &pool->state);
  if (status != PFS_OK) {
    result.selected = PFS_POOL_NO_SELECTION;
    *diagnostic = result;
    return status;
  }
  struct pool_state *state = pool->state.data;
  state->diagnostic = result;
  pool->reader = reader;
  pool->memory = memory;
  *diagnostic = result;
  return PFS_OK;
}

enum pfs_status
pfs_pool_close(struct pfs_pool *pool)
{
  if (!pool) {
    return PFS_INVALID;
  }
  if (!pool->state.data && !pool->reader && !pool->memory) {
    return PFS_OK;
  }
  enum pfs_status status = pfs_memory_free(pool->memory, &pool->state);
  if (status == PFS_OK) {
    pool->reader = NULL;
    pool->memory = NULL;
  }
  return status;
}

/* Allocation handles stay in their original owner until freed. Nodes form a
 * work queue and are released backwards, before freeing the preceding handle. */
struct consulted_node {
  struct consulted_node *previous;
  struct pfs_allocation next;
  struct pfs_tree tree;
  uint8_t data[PFS_BLOCK_SIZE];
  uint64_t parent;
  struct consulted_node *dependencies[PFS_TREE_DEPTH_MAX];
  uint16_t dependency_count;
  enum { PROOF_STRUCTURAL, PROOF_PENDING, PROOF_COMPLETED } proof;
  bool catalog_visited;
  bool local_match;
};

struct walk_frame {
  struct consulted_node *node;
  struct pfs_key upper;
  bool has_upper;
  uint16_t next;
};

struct catalog_operation {
  struct pfs_pool *pool;
  const struct pfs_superblock *superblock;
  const struct pfs_pool_root *root;
  struct pfs_allocation first;
  struct consulted_node *last;
  struct pfs_allocation tables[2];
  unsigned active_table;
  size_t table_capacity;
  size_t nodes;
  uint64_t allocation_records;
  struct consulted_node *root_dependencies[PFS_TREE_DEPTH_MAX];
  uint16_t root_dependency_count;
  struct walk_frame frames[PFS_TREE_DEPTH_MAX];
  struct pfs_volume_record volumes[PFS_VOLUME_MAX];
  struct pfs_volume_name_record names[PFS_VOLUME_MAX];
  size_t volume_count;
  size_t name_count;
};

static size_t
node_hash(uint64_t block, size_t capacity)
{
  return (size_t)((block * UINT64_C(11400714819323198485)) >> 32) & (capacity - 1);
}

static struct consulted_node **
table_slot(struct consulted_node **table, size_t capacity, uint64_t block)
{
  size_t slot = node_hash(block, capacity);
  while (table[slot] && table[slot]->tree.header.block != block) {
    slot = (slot + 1) & (capacity - 1);
  }
  return &table[slot];
}

static enum pfs_status
table_grow(struct catalog_operation *operation)
{
  size_t capacity = operation->table_capacity ? operation->table_capacity * 2 : 64;
  if (capacity < operation->table_capacity || capacity > SIZE_MAX / sizeof(void *)) {
    return PFS_LIMIT;
  }
  unsigned next = 1 - operation->active_table;
  enum pfs_status status = pfs_memory_allocate(operation->pool->memory,
                                               capacity * sizeof(struct consulted_node *),
                                               _Alignof(struct consulted_node *),
                                               &operation->tables[next]);
  if (status != PFS_OK) {
    return status;
  }
  struct consulted_node **table = operation->tables[next].data;
  pfs_bytes_zero(table, operation->tables[next].size);
  struct consulted_node *node = operation->first.data;
  while (node) {
    *table_slot(table, capacity, node->tree.header.block) = node;
    node = node->next.data;
  }
  pfs_memory_free(operation->pool->memory, &operation->tables[operation->active_table]);
  operation->active_table = next;
  operation->table_capacity = capacity;
  return PFS_OK;
}

static enum pfs_status
node_get(struct catalog_operation *operation, const struct pfs_reference *reference,
         uint64_t birth, uint16_t kind, struct consulted_node *parent,
         const struct pfs_key *minimum, const struct pfs_key *upper,
         struct consulted_node **out)
{
  if (!operation->table_capacity || operation->nodes >= operation->table_capacity / 2) {
    enum pfs_status status = table_grow(operation);
    if (status != PFS_OK) {
      return status;
    }
  }
  struct consulted_node **table = operation->tables[operation->active_table].data;
  struct consulted_node **slot = table_slot(table, operation->table_capacity, reference->block);
  struct consulted_node *node = *slot;
  struct pfs_tree_context context = {
    .block = block_context(operation->superblock, reference, birth),
    .kind = kind,
    .parent_level = parent ? parent->tree.level : 0,
  };
  uint64_t parent_block = parent ? parent->tree.header.block : operation->root->header.block;
  if (reference->block == operation->root->header.block) {
    return PFS_CORRUPT;
  }
  if (node) {
    /* Revalidate every incoming identity, generation, owner and level constraint
     * against immutable cached bytes; a previous lookup is no ownership proof. */
    enum pfs_status status = pfs_block_match(&node->tree.header, &context.block, PFS_BLOCK_TREE);
    if (status != PFS_OK) {
      return status;
    }
    if (node->tree.kind != kind || node->parent != parent_block ||
        (parent && node->tree.level != parent->tree.level - 1)) {
      return PFS_CORRUPT;
    }
  } else {
    if (operation->nodes >= operation->superblock->block_count - 2 ||
        operation->nodes >= PFS_ALLOCATION_COUNT_MAX) {
      return PFS_LIMIT;
    }
    struct pfs_allocation *allocation = operation->last ? &operation->last->next : &operation->first;
    enum pfs_status status = pfs_memory_allocate(operation->pool->memory, sizeof(*node),
                                                _Alignof(struct consulted_node), allocation);
    if (status != PFS_OK) {
      return status;
    }
    node = allocation->data;
    pfs_bytes_zero(node, sizeof(*node));
    node->previous = operation->last;
    operation->last = node;
    node->parent = parent_block;
    status = pfs_block_read(operation->pool->reader, reference->block, 1,
                            node->data, sizeof(node->data));
    if (status == PFS_OK) {
      status = pfs_tree_decode(node->data, sizeof(node->data), &context, &node->tree);
    }
    if (status != PFS_OK) {
      return status;
    }
    if (kind == PFS_INDEX_ALLOCATION && !node->tree.level) {
      if (node->tree.count > PFS_ALLOCATION_COUNT_MAX - operation->allocation_records) {
        return PFS_LIMIT;
      }
      operation->allocation_records += node->tree.count;
    }
    *slot = node;
    ++operation->nodes;
  }
  if ((minimum && pfs_key_compare(kind, &node->tree.minimum, minimum)) ||
      (upper && pfs_key_compare(kind, &node->tree.maximum, upper) >= 0)) {
    return PFS_CORRUPT;
  }
  if (upper && kind == PFS_INDEX_ALLOCATION && !node->tree.level) {
    const struct pfs_tree_slot *last = &node->tree.slots[node->tree.count - 1];
    struct pfs_allocation_record record;
    struct pfs_record_context records = record_context(operation->superblock, node->tree.header.birth);
    enum pfs_status status = pfs_allocation_record_decode(node->data + last->offset,
                                                         last->length, &records, &record);
    if (status != PFS_OK) {
      return status;
    }
    if (record.count > pfs_get_u64(upper->bytes) - record.first) {
      return PFS_CORRUPT;
    }
  }
  *out = node;
  return PFS_OK;
}

static enum pfs_status
internal_at(struct catalog_operation *operation, struct consulted_node *node,
            uint16_t slot, struct pfs_internal_record *record)
{
  struct pfs_record_context context = record_context(operation->superblock, node->tree.header.birth);
  return pfs_internal_record_decode(node->data + node->tree.slots[slot].offset,
                                    node->tree.slots[slot].length, node->tree.kind,
                                    &context, record);
}

static enum pfs_status
catalog_walk(struct catalog_operation *operation, const struct pfs_reference *root, uint16_t kind)
{
  struct consulted_node *node;
  enum pfs_status status = node_get(operation, root, operation->root->header.birth,
                                    kind, NULL, NULL, NULL, &node);
  if (status != PFS_OK) {
    return status;
  }
  size_t depth = 1;
  operation->frames[0] = (struct walk_frame){ .node = node };
  node->catalog_visited = true;
  while (depth) {
    struct walk_frame *frame = &operation->frames[depth - 1];
    node = frame->node;
    if (frame->next == node->tree.count) {
      --depth;
      continue;
    }
    uint16_t slot = frame->next++;
    if (node->tree.level) {
      struct pfs_internal_record child;
      status = internal_at(operation, node, slot, &child);
      if (status != PFS_OK) {
        return status;
      }
      struct pfs_key upper;
      bool has_upper = frame->has_upper;
      if (has_upper) {
        upper = frame->upper;
      }
      if (slot + 1 < node->tree.count) {
        struct pfs_internal_record next;
        status = internal_at(operation, node, slot + 1, &next);
        if (status != PFS_OK) {
          return status;
        }
        upper = next.minimum;
        has_upper = true;
      }
      struct consulted_node *target;
      status = node_get(operation, &child.child, node->tree.header.birth, kind, node,
                         &child.minimum, has_upper ? &upper : NULL, &target);
      if (status != PFS_OK) {
        return status;
      }
      if (target->catalog_visited) {
        return PFS_CORRUPT;
      }
      if (depth == PFS_TREE_DEPTH_MAX) {
        return PFS_LIMIT;
      }
      target->catalog_visited = true;
      operation->frames[depth] = (struct walk_frame){ .node = target, .has_upper = has_upper };
      if (has_upper) {
        operation->frames[depth].upper = upper;
      }
      ++depth;
    } else {
      struct pfs_record_context context = record_context(operation->superblock, node->tree.header.birth);
      const struct pfs_tree_slot *record_slot = &node->tree.slots[slot];
      if (kind == PFS_INDEX_VOLUMES) {
        if (operation->volume_count == PFS_VOLUME_MAX) {
          return PFS_LIMIT;
        }
        status = pfs_volume_record_decode(node->data + record_slot->offset, record_slot->length,
                                           &context, &operation->volumes[operation->volume_count]);
        ++operation->volume_count;
      } else {
        if (operation->name_count == PFS_VOLUME_MAX) {
          return PFS_LIMIT;
        }
        status = pfs_volume_name_record_decode(node->data + record_slot->offset, record_slot->length,
                                                &context, &operation->names[operation->name_count]);
        ++operation->name_count;
      }
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  return PFS_OK;
}

static enum pfs_status
allocation_match(struct catalog_operation *operation, uint64_t block, uint64_t birth,
                 struct consulted_node **dependencies, uint16_t *dependency_count)
{
  struct consulted_node *node;
  enum pfs_status status = node_get(operation, &operation->root->allocation,
                                    operation->root->header.birth, PFS_INDEX_ALLOCATION,
                                    NULL, NULL, NULL, &node);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_key key = { .length = sizeof(uint64_t) };
  pfs_put_u64(key.bytes, block);
  struct pfs_key upper;
  bool has_upper = false;
  *dependency_count = 0;
  while (true) {
    for (uint16_t i = 0; i < *dependency_count; ++i) {
      if (dependencies[i] == node) {
        return PFS_CORRUPT;
      }
    }
    if (*dependency_count == PFS_TREE_DEPTH_MAX) {
      return PFS_LIMIT;
    }
    dependencies[(*dependency_count)++] = node;
    if (pfs_key_compare(PFS_INDEX_ALLOCATION, &key, &node->tree.minimum) < 0) {
      return PFS_CORRUPT;
    }
    if (!node->tree.level) {
      struct pfs_record_context context = record_context(operation->superblock, node->tree.header.birth);
      for (uint16_t i = 0; i < node->tree.count; ++i) {
        struct pfs_allocation_record record;
        status = pfs_allocation_record_decode(node->data + node->tree.slots[i].offset,
                                               node->tree.slots[i].length, &context, &record);
        if (status != PFS_OK) {
          return status;
        }
        if (block >= record.first && block - record.first < record.count) {
          return record.state == PFS_ALLOCATION_POOL && record.birth == birth ? PFS_OK : PFS_CORRUPT;
        }
      }
      return PFS_CORRUPT;
    }
    struct pfs_internal_record chosen;
    status = internal_at(operation, node, 0, &chosen);
    if (status != PFS_OK) {
      return status;
    }
    for (uint16_t i = 1; i < node->tree.count; ++i) {
      struct pfs_internal_record next;
      status = internal_at(operation, node, i, &next);
      if (status != PFS_OK) {
        return status;
      }
      if (pfs_key_compare(PFS_INDEX_ALLOCATION, &key, &next.minimum) < 0) {
        upper = next.minimum;
        has_upper = true;
        break;
      }
      chosen = next;
    }
    struct consulted_node *target;
    status = node_get(operation, &chosen.child, node->tree.header.birth,
                       PFS_INDEX_ALLOCATION, node, &chosen.minimum, has_upper ? &upper : NULL, &target);
    if (status != PFS_OK) {
      return status;
    }
    node = target;
  }
}

static enum pfs_status
proof_complete(struct catalog_operation *operation)
{
  enum pfs_status status = allocation_match(operation, operation->root->header.block,
                                            operation->root->header.birth,
                                            operation->root_dependencies,
                                            &operation->root_dependency_count);
  if (status != PFS_OK) {
    return status;
  }
  struct consulted_node *node = operation->first.data;
  while (node) {
    node->proof = PROOF_PENDING;
    status = allocation_match(operation, node->tree.header.block, node->tree.header.birth,
                               node->dependencies, &node->dependency_count);
    if (status != PFS_OK) {
      return status;
    }
    node->local_match = true;
    /* The lookup can append previously unconsulted map nodes to this queue. */
    node = node->next.data;
  }
  /* Pending dependencies are never treated as proof. First establish that the
   * entire pending set is locally matched and closed, then complete it together. */
  for (node = operation->first.data; node; node = node->next.data) {
    if (node->proof != PROOF_PENDING || !node->local_match) {
      return PFS_CORRUPT;
    }
    for (uint16_t i = 0; i < node->dependency_count; ++i) {
      struct consulted_node *dependency = node->dependencies[i];
      if (dependency->proof != PROOF_PENDING || !dependency->local_match) {
        return PFS_CORRUPT;
      }
    }
  }
  for (uint16_t i = 0; i < operation->root_dependency_count; ++i) {
    struct consulted_node *dependency = operation->root_dependencies[i];
    if (dependency->proof != PROOF_PENDING || !dependency->local_match) {
      return PFS_CORRUPT;
    }
  }
  for (node = operation->first.data; node; node = node->next.data) {
    node->proof = PROOF_COMPLETED;
  }
  return PFS_OK;
}

static void
operation_release(struct catalog_operation *operation)
{
  struct consulted_node *node = operation->last;
  while (node) {
    struct consulted_node *previous = node->previous;
    struct pfs_allocation *allocation = previous ? &previous->next : &operation->first;
    pfs_memory_free(operation->pool->memory, allocation);
    node = previous;
  }
  pfs_memory_free(operation->pool->memory, &operation->tables[0]);
  pfs_memory_free(operation->pool->memory, &operation->tables[1]);
}

enum pfs_status
pfs_pool_diagnostic_volumes(struct pfs_pool *pool, struct pfs_volume_record *out,
                            size_t capacity, size_t *count)
{
  if (!pool || !pool->reader || !pool->memory || !pool->state.data || !out || !count) {
    return PFS_INVALID;
  }
  const struct pool_state *state = pool->state.data;
  const struct pfs_pool_candidate *selected = &state->diagnostic.candidate[state->diagnostic.selected];
  if (capacity < selected->root.volume_count) {
    return PFS_LIMIT;
  }
  struct pfs_allocation storage = {0};
  enum pfs_status status = pfs_memory_allocate(pool->memory, sizeof(struct catalog_operation),
                                               _Alignof(struct catalog_operation), &storage);
  if (status != PFS_OK) {
    return status;
  }
  struct catalog_operation *operation = storage.data;
  pfs_bytes_zero(operation, sizeof(*operation));
  operation->pool = pool;
  operation->superblock = &selected->superblock;
  operation->root = &selected->root;
  status = catalog_walk(operation, &selected->root.volumes, PFS_INDEX_VOLUMES);
  if (status == PFS_OK) {
    status = catalog_walk(operation, &selected->root.volume_names, PFS_INDEX_VOLUME_NAMES);
  }
  if (status == PFS_OK && (operation->volume_count != selected->root.volume_count ||
                           operation->name_count != operation->volume_count)) {
    status = PFS_CORRUPT;
  }
  if (status == PFS_OK) {
    for (size_t i = 0; i < operation->name_count; ++i) {
      const struct pfs_volume_name_record *name = &operation->names[i];
      size_t j = 0;
      while (j < operation->volume_count &&
             pfs_bytes_compare(name->volume.bytes, PFS_ID_SIZE,
                                operation->volumes[j].id.bytes, PFS_ID_SIZE)) {
        ++j;
      }
      if (j == operation->volume_count ||
          pfs_bytes_compare(name->name.bytes, name->name.length,
                             operation->volumes[j].name.bytes, operation->volumes[j].name.length)) {
        status = PFS_CORRUPT;
        break;
      }
    }
  }
  if (status == PFS_OK) {
    status = proof_complete(operation);
  }
  if (status == PFS_OK) {
    for (size_t i = 0; i < operation->name_count; ++i) {
      size_t j = 0;
      while (pfs_bytes_compare(operation->names[i].volume.bytes, PFS_ID_SIZE,
                                operation->volumes[j].id.bytes, PFS_ID_SIZE)) {
        ++j;
      }
      out[i] = operation->volumes[j];
    }
    *count = operation->name_count;
  }
  operation_release(operation);
  pfs_memory_free(pool->memory, &storage);
  return status;
}
