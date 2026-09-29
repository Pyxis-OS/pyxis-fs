#include <pyxis_fs/pool.h>
#include <pyxis_fs/tree.h>

#include "internal.h"
#include "read_internal.h"

struct pool_state {
  struct pfs_pool_diagnostic diagnostic;
  size_t volumes;
};

struct candidate_workspace {
  uint8_t raw[2][PFS_BLOCK_SIZE];
  uint8_t data[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
};

static void
block_context(struct pfs_block_context *context, const struct pfs_superblock *superblock,
              const struct pfs_reference *reference, uint64_t birth)
{
  pfs_bytes_zero(context, sizeof(*context));
  context->block_count = superblock->block_count;
  context->selected_generation = superblock->header.birth;
  context->referring_birth = birth;
  pfs_bytes_copy(&context->pool, &superblock->header.pool, sizeof(context->pool));
  pfs_bytes_copy(&context->reference, reference, sizeof(context->reference));
  pfs_bytes_copy(&context->features, &superblock->features, sizeof(context->features));
}

static void
record_context(struct pfs_record_context *context,
               const struct pfs_superblock *superblock, uint64_t birth)
{
  pfs_bytes_zero(context, sizeof(*context));
  context->block_count = superblock->block_count;
  context->selected_generation = superblock->header.birth;
  context->containing_birth = birth;
  pfs_bytes_copy(&context->features, &superblock->features, sizeof(context->features));
}

static enum pfs_status
candidate_read(const struct pfs_block_reader *reader, uint64_t slot,
               uint8_t *raw, struct candidate_workspace *workspace,
               struct pfs_pool_candidate *candidate)
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
  uint8_t *data = workspace->data;
  status = pfs_block_read(reader, superblock.root.block, 1, data, PFS_BLOCK_SIZE);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_block_context context;
  block_context(&context, &superblock, &superblock.root, superblock.header.birth);
  struct pfs_pool_root root;
  status = pfs_pool_root_decode(data, PFS_BLOCK_SIZE, &context, &root);
  if (status != PFS_OK) {
    return status;
  }
  const struct pfs_reference *references[] = { &root.volumes, &root.volume_names, &root.allocation };
  for (uint16_t i = 0; i < 3; ++i) {
    status = pfs_block_read(reader, references[i]->block, 1, data, PFS_BLOCK_SIZE);
    if (status != PFS_OK) {
      return status;
    }
    struct pfs_tree_context tree_context;
    pfs_bytes_zero(&tree_context, sizeof(tree_context));
    block_context(&tree_context.block, &superblock, references[i], root.header.birth);
    tree_context.kind = (uint16_t)(PFS_INDEX_VOLUMES + i);
    status = pfs_tree_decode(data, PFS_BLOCK_SIZE, &tree_context, &workspace->tree);
    if (status != PFS_OK) {
      return status;
    }
  }
  pfs_bytes_copy(&candidate->superblock, &superblock, sizeof(superblock));
  pfs_bytes_copy(&candidate->root, &root, sizeof(root));
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
  struct pfs_pool_diagnostic result;
  pfs_bytes_zero(&result, sizeof(result));
  result.selected = PFS_POOL_NO_SELECTION;
  struct pfs_allocation scratch;
  pfs_bytes_zero(&scratch, sizeof(scratch));
  enum pfs_status status = pfs_memory_allocate(memory, sizeof(struct candidate_workspace),
    _Alignof(struct candidate_workspace), &scratch);
  if (status != PFS_OK) {
    result.candidate[0].status = status;
    result.candidate[1].status = status;
    pfs_bytes_copy(diagnostic, &result, sizeof(result));
    return status;
  }
  struct candidate_workspace *workspace = scratch.data;
  uint8_t (*raw)[PFS_BLOCK_SIZE] = workspace->raw;
  result.candidate[0].status = candidate_read(reader, 0, raw[0], workspace, &result.candidate[0]);
  result.candidate[1].status = candidate_read(reader, reader->geometry.block_count - 1,
                                             raw[1], workspace, &result.candidate[1]);
  enum pfs_status first = result.candidate[0].status;
  enum pfs_status second = result.candidate[1].status;
  status = failure_priority(first) >= failure_priority(second) ? first : second;
  if (failure_priority(status) > failure_priority(PFS_CORRUPT) ||
      (first != PFS_OK && second != PFS_OK)) {
    goto done;
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
      status = PFS_CORRUPT;
      goto done;
    }
    result.selected = a->header.birth >= b->header.birth ? 0 : 1;
  } else {
    result.selected = first == PFS_OK ? 0 : 1;
    result.degraded = true;
  }
  pfs_memory_free(memory, &scratch);
  status = pfs_memory_allocate(memory, sizeof(struct pool_state), _Alignof(struct pool_state),
                               &pool->state);
  if (status != PFS_OK) {
    result.selected = PFS_POOL_NO_SELECTION;
    result.degraded = false;
    goto done;
  }
  struct pool_state *state = pool->state.data;
  pfs_bytes_copy(&state->diagnostic, &result, sizeof(result));
  state->volumes = 0;
  pool->reader = reader;
  pool->memory = memory;

done:
  pfs_memory_free(memory, &scratch);
  pfs_bytes_copy(diagnostic, &result, sizeof(result));
  return status;
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
  if (!pool->state.data || !pool->reader || !pool->memory ||
      pool->state.owner != pool->memory) {
    return PFS_INVALID;
  }
  if (((struct pool_state *)pool->state.data)->volumes) {
    return PFS_BUSY;
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

struct consulted_range {
  struct consulted_range *previous;
  struct pfs_allocation next;
  uint64_t first;
  uint64_t count;
  uint64_t birth;
  struct pfs_volume_id owner;
  struct consulted_node *dependencies[PFS_TREE_DEPTH_MAX];
  uint16_t dependency_count;
  bool local_match;
  bool completed;
};

struct walk_frame {
  struct consulted_node *node;
  struct pfs_key upper;
  bool has_upper;
  uint16_t next;
};

struct volume_state {
  struct pfs_volume_record record;
  uint64_t birth;
  size_t retained;
};

struct catalog_operation {
  struct pfs_pool *pool;
  const struct pfs_superblock *superblock;
  const struct pfs_pool_root *root;
  struct pfs_allocation first;
  struct consulted_node *last;
  struct pfs_allocation first_range;
  struct consulted_range *last_range;
  struct pfs_allocation tables[2];
  unsigned active_table;
  size_t table_capacity;
  size_t nodes;
  uint64_t allocation_records;
  uint64_t volume_records[8];
  struct consulted_node *root_dependencies[PFS_TREE_DEPTH_MAX];
  uint16_t root_dependency_count;
  struct walk_frame frames[PFS_TREE_DEPTH_MAX];
  struct pfs_volume_record volumes[PFS_VOLUME_MAX];
  struct pfs_volume_name_record names[PFS_VOLUME_MAX];
  size_t volume_count;
  size_t name_count;
  uint64_t volume_births[PFS_VOLUME_MAX];
  struct volume_state *volume;
  struct pfs_object_id object;
  struct pfs_object_record chain[PFS_ANCESTRY_MAX];
  size_t chain_count;
  struct pfs_grant_record grants[PFS_OBJECT_GRANTS_MAX];
  size_t grant_count;
  uint8_t file_data[PFS_BLOCK_SIZE];
  uint64_t file_length;
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
  struct pfs_tree_context context;
  pfs_bytes_zero(&context, sizeof(context));
  block_context(&context.block, operation->superblock, reference, birth);
  context.kind = kind;
  if (kind >= PFS_INDEX_OBJECTS) {
    pfs_bytes_copy(&context.volume, &operation->volume->record.id, sizeof(context.volume));
    pfs_bytes_copy(&context.object, &operation->object, sizeof(context.object));
    pfs_bytes_copy(&context.block.features, &operation->volume->record.features, sizeof(context.block.features));
  }
  context.parent_level = parent ? parent->tree.level : 0;
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
        pfs_bytes_compare(node->tree.volume.bytes, PFS_ID_SIZE, context.volume.bytes, PFS_ID_SIZE) ||
        pfs_bytes_compare(node->tree.object.bytes, PFS_ID_SIZE, context.object.bytes, PFS_ID_SIZE) ||
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
    if (kind >= PFS_INDEX_OBJECTS && !node->tree.level) {
      if (node->tree.count > PFS_RECORD_COUNT_MAX - operation->volume_records[kind]) {
        return PFS_LIMIT;
      }
      operation->volume_records[kind] += node->tree.count;
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
    struct pfs_record_context records;
    record_context(&records, operation->superblock, node->tree.header.birth);
    enum pfs_status status = pfs_allocation_record_decode(node->data + last->offset,
                                                         last->length, &records, &record);
    if (status != PFS_OK) {
      return status;
    }
    if (record.count > pfs_get_u64(upper->bytes) - record.first) {
      return PFS_CORRUPT;
    }
  }
  if (kind == PFS_INDEX_EXTENTS && !node->tree.level) {
    struct pfs_record_context records;
    record_context(&records, operation->superblock, node->tree.header.birth);
    pfs_bytes_copy(&records.features, &operation->volume->record.features, sizeof(records.features));
    for (uint16_t i = 0; i < node->tree.count; ++i) {
      const struct pfs_tree_slot *location = &node->tree.slots[i];
      struct pfs_extent_record record;
      enum pfs_status status = pfs_extent_record_decode(node->data + location->offset,
        location->length, &records, &record);
      if (status == PFS_OK) {
        status = pfs_extent_file_validate(&record.mapping, operation->file_length);
      }
      if (status != PFS_OK) {
        return status;
      }
      if (upper && record.mapping.count > pfs_get_u64(upper->bytes) - record.mapping.logical_first) {
        return PFS_CORRUPT;
      }
    }
  }
  *out = node;
  return PFS_OK;
}

static void
node_records(struct catalog_operation *operation, struct consulted_node *node,
             struct pfs_record_context *context)
{
  record_context(context, operation->superblock, node->tree.header.birth);
  if (node->tree.kind >= PFS_INDEX_OBJECTS) {
    pfs_bytes_copy(&context->features, &operation->volume->record.features,
                   sizeof(context->features));
  }
}

static enum pfs_status
internal_at(struct catalog_operation *operation, struct consulted_node *node,
            uint16_t slot, struct pfs_internal_record *record)
{
  struct pfs_record_context context;
  node_records(operation, node, &context);
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
  pfs_bytes_zero(&operation->frames[0], sizeof(operation->frames[0]));
  operation->frames[0].node = node;
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
        pfs_bytes_copy(&upper, &frame->upper, sizeof(upper));
      }
      if (slot + 1 < node->tree.count) {
        struct pfs_internal_record next;
        status = internal_at(operation, node, slot + 1, &next);
        if (status != PFS_OK) {
          return status;
        }
        pfs_bytes_copy(&upper, &next.minimum, sizeof(upper));
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
      pfs_bytes_zero(&operation->frames[depth], sizeof(operation->frames[depth]));
      operation->frames[depth].node = target;
      operation->frames[depth].has_upper = has_upper;
      if (has_upper) {
        pfs_bytes_copy(&operation->frames[depth].upper, &upper, sizeof(upper));
      }
      ++depth;
    } else {
      struct pfs_record_context context;
      record_context(&context, operation->superblock, node->tree.header.birth);
      const struct pfs_tree_slot *record_slot = &node->tree.slots[slot];
      if (kind == PFS_INDEX_VOLUMES) {
        if (operation->volume_count == PFS_VOLUME_MAX) {
          return PFS_LIMIT;
        }
        status = pfs_volume_record_decode(node->data + record_slot->offset, record_slot->length,
                                           &context, &operation->volumes[operation->volume_count]);
        operation->volume_births[operation->volume_count] = node->tree.header.birth;
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
                 const struct pfs_volume_id *owner, uint64_t *available,
                 struct consulted_node **dependencies, uint16_t *dependency_count)
{
  struct consulted_node *node;
  enum pfs_status status = node_get(operation, &operation->root->allocation,
                                    operation->root->header.birth, PFS_INDEX_ALLOCATION,
                                    NULL, NULL, NULL, &node);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_key key;
  pfs_bytes_zero(&key, sizeof(key));
  key.length = sizeof(uint64_t);
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
      struct pfs_record_context context;
      record_context(&context, operation->superblock, node->tree.header.birth);
      for (uint16_t i = 0; i < node->tree.count; ++i) {
        struct pfs_allocation_record record;
        status = pfs_allocation_record_decode(node->data + node->tree.slots[i].offset,
                                               node->tree.slots[i].length, &context, &record);
        if (status != PFS_OK) {
          return status;
        }
        if (block >= record.first && block - record.first < record.count) {
          if (record.birth != birth || record.state != (owner ? PFS_ALLOCATION_VOLUME : PFS_ALLOCATION_POOL) ||
              (owner && pfs_bytes_compare(record.owner.bytes, PFS_ID_SIZE, owner->bytes, PFS_ID_SIZE))) {
            return PFS_CORRUPT;
          }
          if (available) {
            *available = record.count - (block - record.first);
          }
          return PFS_OK;
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
        pfs_bytes_copy(&upper, &next.minimum, sizeof(upper));
        has_upper = true;
        break;
      }
      pfs_bytes_copy(&chosen, &next, sizeof(chosen));
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
                                            operation->root->header.birth, NULL, NULL,
                                            operation->root_dependencies,
                                            &operation->root_dependency_count);
  if (status != PFS_OK) {
    return status;
  }
  struct consulted_node *node = operation->first.data;
  while (node) {
    node->proof = PROOF_PENDING;
    status = allocation_match(operation, node->tree.header.block, node->tree.header.birth,
                               node->tree.kind >= PFS_INDEX_OBJECTS ? &node->tree.volume : NULL, NULL,
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
  for (struct consulted_range *range = operation->first_range.data;
       range; range = range->next.data) {
    if (!range->local_match) {
      return PFS_CORRUPT;
    }
    for (uint16_t i = 0; i < range->dependency_count; ++i) {
      struct consulted_node *dependency = range->dependencies[i];
      if (dependency->proof != PROOF_PENDING || !dependency->local_match) {
        return PFS_CORRUPT;
      }
    }
  }
  for (struct consulted_range *range = operation->first_range.data;
       range; range = range->next.data) {
    range->completed = true;
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
  struct consulted_range *range = operation->last_range;
  while (range) {
    struct consulted_range *previous = range->previous;
    struct pfs_allocation *allocation = previous ? &previous->next : &operation->first_range;
    pfs_memory_free(operation->pool->memory, allocation);
    range = previous;
  }
  pfs_memory_free(operation->pool->memory, &operation->tables[0]);
  pfs_memory_free(operation->pool->memory, &operation->tables[1]);
}

static enum pfs_status
catalog_read(struct catalog_operation *operation)
{
  enum pfs_status status = catalog_walk(operation, &operation->root->volumes, PFS_INDEX_VOLUMES);
  if (status == PFS_OK) {
    status = catalog_walk(operation, &operation->root->volume_names, PFS_INDEX_VOLUME_NAMES);
  }
  if (status == PFS_OK && (operation->volume_count != operation->root->volume_count ||
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
  return status;
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
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
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
  status = catalog_read(operation);
  if (status == PFS_OK) {
    for (size_t i = 0; i < operation->name_count; ++i) {
      size_t j = 0;
      while (pfs_bytes_compare(operation->names[i].volume.bytes, PFS_ID_SIZE,
                                operation->volumes[j].id.bytes, PFS_ID_SIZE)) {
        ++j;
      }
      pfs_bytes_copy(&out[i], &operation->volumes[j], sizeof(out[i]));
    }
    *count = operation->name_count;
  }
  operation_release(operation);
  pfs_memory_free(pool->memory, &storage);
  return status;
}

static bool
volume_live(const struct pfs_volume *volume)
{
  return volume && volume->pool && volume->state.data && volume->pool->state.data &&
    volume->state.owner == volume->pool->memory;
}

static const struct pfs_pool_candidate *
selected_candidate(const struct pfs_pool *pool)
{
  const struct pool_state *state = pool->state.data;
  return &state->diagnostic.candidate[state->diagnostic.selected];
}

static enum pfs_status
operation_create(struct pfs_volume *volume, struct pfs_allocation *storage,
                 struct catalog_operation **out)
{
  if (!volume_live(volume)) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_memory_allocate(volume->pool->memory,
    sizeof(struct catalog_operation), _Alignof(struct catalog_operation), storage);
  if (status != PFS_OK) {
    return status;
  }
  struct catalog_operation *operation = storage->data;
  pfs_bytes_zero(operation, sizeof(*operation));
  const struct pfs_pool_candidate *selected = selected_candidate(volume->pool);
  operation->pool = volume->pool;
  operation->superblock = &selected->superblock;
  operation->root = &selected->root;
  operation->volume = volume->state.data;
  *out = operation;
  return PFS_OK;
}

static void
operation_destroy(struct catalog_operation *operation, struct pfs_allocation *storage)
{
  struct pfs_memory *memory = operation->pool->memory;
  operation_release(operation);
  pfs_memory_free(memory, storage);
}

static enum pfs_status
operation_result(struct catalog_operation *operation, enum pfs_status status)
{
  if (status != PFS_OK && status != PFS_NOT_FOUND) {
    return status;
  }
  enum pfs_status proof = proof_complete(operation);
  return proof == PFS_OK ? status : proof;
}

static enum pfs_status
record_key(struct catalog_operation *operation, struct consulted_node *node,
           uint16_t slot, struct pfs_key *key)
{
  struct pfs_record_context context;
  node_records(operation, node, &context);
  const struct pfs_tree_slot *location = &node->tree.slots[slot];
  const uint8_t *data = node->data + location->offset;
  enum pfs_status status;
  pfs_bytes_zero(key, sizeof(*key));
  switch (node->tree.kind) {
  case PFS_INDEX_OBJECTS: {
    struct pfs_object_record record;
    status = pfs_object_record_decode(data, location->length, &context, &record);
    if (status == PFS_OK) {
      key->length = PFS_ID_SIZE;
      pfs_bytes_copy(key->bytes, record.id.bytes, key->length);
    }
    return status;
  }
  case PFS_INDEX_DIRECTORY: {
    struct pfs_dirent_record record;
    status = pfs_dirent_record_decode(data, location->length, &context, &record);
    if (status == PFS_OK) {
      key->length = record.name.length;
      pfs_bytes_copy(key->bytes, record.name.bytes, key->length);
    }
    return status;
  }
  case PFS_INDEX_EXTENTS: {
    struct pfs_extent_record record;
    status = pfs_extent_record_decode(data, location->length, &context, &record);
    if (status == PFS_OK) {
      key->length = sizeof(uint64_t);
      pfs_put_u64(key->bytes, record.mapping.logical_first);
    }
    return status;
  }
  case PFS_INDEX_GRANTS: {
    struct pfs_grant_record record;
    status = pfs_grant_record_decode(data, location->length, &context, &record);
    if (status == PFS_OK) {
      key->length = 2 * PFS_ID_SIZE + 1;
      pfs_bytes_copy(key->bytes, record.object.bytes, PFS_ID_SIZE);
      pfs_bytes_copy(key->bytes + PFS_ID_SIZE, record.principal.bytes, PFS_ID_SIZE);
      key->bytes[2 * PFS_ID_SIZE] = record.scope;
    }
    return status;
  }
  default:
    return PFS_INVALID;
  }
}

/* Seek a lower bound, then continue through successors without following a
 * sibling pointer. Every descent retains its inherited exclusive upper bound. */
static enum pfs_status
record_next(struct catalog_operation *operation, const struct pfs_reference *root,
            uint64_t birth, uint16_t kind, const struct pfs_object_id *object,
            const struct pfs_key *lower, bool strict, struct consulted_node **out,
            uint16_t *out_slot, struct pfs_key *out_key)
{
  if (object) {
    pfs_bytes_copy(&operation->object, object, sizeof(operation->object));
  } else {
    pfs_bytes_zero(&operation->object, sizeof(operation->object));
  }
  struct consulted_node *node;
  enum pfs_status status = node_get(operation, root, birth, kind, NULL, NULL, NULL, &node);
  if (status != PFS_OK) {
    return status;
  }
  size_t depth = 1;
  pfs_bytes_zero(&operation->frames[0], sizeof(operation->frames[0]));
  operation->frames[0].node = node;
  bool seeking = lower != NULL;
  uint64_t visited = 0;
  while (depth) {
    struct walk_frame *frame = &operation->frames[depth - 1];
    node = frame->node;
    if (node->tree.level) {
      if (seeking && !frame->next) {
        for (uint16_t i = 1; i < node->tree.count; ++i) {
          struct pfs_internal_record next;
          status = internal_at(operation, node, i, &next);
          if (status != PFS_OK) {
            return status;
          }
          if (pfs_key_compare(kind, lower, &next.minimum) < 0) {
            break;
          }
          frame->next = i;
        }
      }
      if (frame->next == node->tree.count) {
        --depth;
        continue;
      }
      uint16_t slot = frame->next++;
      struct pfs_internal_record child;
      status = internal_at(operation, node, slot, &child);
      if (status != PFS_OK) {
        return status;
      }
      struct pfs_key upper;
      bool has_upper = frame->has_upper;
      if (has_upper) {
        pfs_bytes_copy(&upper, &frame->upper, sizeof(upper));
      }
      if (slot + 1 < node->tree.count) {
        struct pfs_internal_record next;
        status = internal_at(operation, node, slot + 1, &next);
        if (status != PFS_OK) {
          return status;
        }
        pfs_bytes_copy(&upper, &next.minimum, sizeof(upper));
        has_upper = true;
      }
      struct consulted_node *target;
      status = node_get(operation, &child.child, node->tree.header.birth, kind, node,
                        &child.minimum, has_upper ? &upper : NULL, &target);
      if (status != PFS_OK) {
        return status;
      }
      for (size_t i = 0; i < depth; ++i) {
        if (operation->frames[i].node == target) {
          return PFS_CORRUPT;
        }
      }
      if (depth == PFS_TREE_DEPTH_MAX || ++visited > PFS_RECORD_COUNT_MAX) {
        return PFS_LIMIT;
      }
      pfs_bytes_zero(&operation->frames[depth], sizeof(operation->frames[depth]));
      operation->frames[depth].node = target;
      operation->frames[depth].has_upper = has_upper;
      if (has_upper) {
        pfs_bytes_copy(&operation->frames[depth].upper, &upper, sizeof(upper));
      }
      ++depth;
      continue;
    }
    for (uint16_t i = 0; i < node->tree.count; ++i) {
      struct pfs_key key;
      status = record_key(operation, node, i, &key);
      if (status != PFS_OK) {
        return status;
      }
      int comparison = lower ? pfs_key_compare(kind, &key, lower) : 1;
      if (comparison > 0 || (!strict && comparison == 0)) {
        *out = node;
        *out_slot = i;
        pfs_bytes_copy(out_key, &key, sizeof(key));
        return PFS_OK;
      }
    }
    seeking = false;
    --depth;
  }
  return PFS_NOT_FOUND;
}

static enum pfs_status
object_lookup(struct catalog_operation *operation, const struct pfs_object_id *id,
              struct pfs_object_record *out, uint64_t *birth)
{
  struct pfs_key key;
  pfs_bytes_zero(&key, sizeof(key));
  key.length = PFS_ID_SIZE;
  pfs_bytes_copy(key.bytes, id->bytes, PFS_ID_SIZE);
  struct consulted_node *node;
  uint16_t slot;
  struct pfs_key found;
  enum pfs_status status = record_next(operation, &operation->volume->record.object_root,
    operation->volume->birth, PFS_INDEX_OBJECTS, NULL, &key, false, &node, &slot, &found);
  if (status != PFS_OK) {
    return status;
  }
  if (pfs_key_compare(PFS_INDEX_OBJECTS, &key, &found)) {
    return PFS_NOT_FOUND;
  }
  struct pfs_record_context context;
  node_records(operation, node, &context);
  status = pfs_object_record_decode(node->data + node->tree.slots[slot].offset,
    node->tree.slots[slot].length, &context, out);
  if (status == PFS_OK && birth) {
    *birth = node->tree.header.birth;
  }
  return status;
}

static enum pfs_status
entry_next(struct catalog_operation *operation, const struct pfs_object_record *directory,
           uint64_t birth, const struct pfs_key *lower, bool strict,
           struct pfs_dirent_record *entry, struct pfs_key *key)
{
  if (directory->kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
  if (directory->storage_kind == PFS_STORAGE_NONE) {
    return PFS_NOT_FOUND;
  }
  struct consulted_node *node;
  uint16_t slot;
  enum pfs_status status = record_next(operation, &directory->tree_root, birth,
    PFS_INDEX_DIRECTORY, &directory->id, lower, strict, &node, &slot, key);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context context;
  node_records(operation, node, &context);
  return pfs_dirent_record_decode(node->data + node->tree.slots[slot].offset,
    node->tree.slots[slot].length, &context, entry);
}

static enum pfs_status
naming_match(struct catalog_operation *operation, const struct pfs_object_record *parent,
             uint64_t birth, const struct pfs_object_record *child)
{
  struct pfs_key previous;
  bool has_previous = false;
  uint64_t seen = 0;
  size_t matches = 0;
  while (true) {
    struct pfs_dirent_record entry;
    struct pfs_key key;
    enum pfs_status status = entry_next(operation, parent, birth,
      has_previous ? &previous : NULL, has_previous, &entry, &key);
    if (status == PFS_NOT_FOUND) {
      return matches == 1 && seen == parent->directory_count ? PFS_OK : PFS_CORRUPT;
    }
    if (status != PFS_OK) {
      return status;
    }
    if (++seen > PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    if (!pfs_bytes_compare(entry.object.bytes, PFS_ID_SIZE, child->id.bytes, PFS_ID_SIZE)) {
      if (entry.child_kind != child->kind || ++matches > 1) {
        return PFS_CORRUPT;
      }
    }
    pfs_bytes_copy(&previous, &key, sizeof(key));
    has_previous = true;
  }
}

static enum pfs_status
ancestry_collect(struct catalog_operation *operation, const struct pfs_object_id *id)
{
  operation->chain_count = 0;
  struct pfs_object_id current;
  pfs_bytes_copy(&current, id, sizeof(current));
  while (true) {
    for (size_t i = 0; i < operation->chain_count; ++i) {
      if (!pfs_bytes_compare(current.bytes, PFS_ID_SIZE,
                            operation->chain[i].id.bytes, PFS_ID_SIZE)) {
        return PFS_CORRUPT;
      }
    }
    if (operation->chain_count == PFS_ANCESTRY_MAX) {
      return PFS_LIMIT;
    }
    struct pfs_object_record *object = &operation->chain[operation->chain_count];
    uint64_t birth;
    enum pfs_status status = object_lookup(operation, &current, object, &birth);
    if (status != PFS_OK) {
      return operation->chain_count && status == PFS_NOT_FOUND ? PFS_CORRUPT : status;
    }
    if (operation->chain_count) {
      if (object->kind != PFS_OBJECT_DIRECTORY) {
        return PFS_CORRUPT;
      }
      status = naming_match(operation, object, birth,
        &operation->chain[operation->chain_count - 1]);
      if (status != PFS_OK) {
        return status;
      }
    }
    ++operation->chain_count;
    if (!pfs_bytes_compare(object->id.bytes, PFS_ID_SIZE,
                           operation->volume->record.root_object.bytes, PFS_ID_SIZE)) {
      if (object->kind != PFS_OBJECT_DIRECTORY ||
          !pfs_bytes_are_zero(object->parent.bytes, PFS_ID_SIZE)) {
        return PFS_CORRUPT;
      }
      return PFS_OK;
    }
    if (pfs_bytes_are_zero(object->parent.bytes, PFS_ID_SIZE)) {
      return PFS_CORRUPT;
    }
    pfs_bytes_copy(&current, &object->parent, sizeof(current));
  }
}

enum pfs_status
pfs_pool_diagnostic_volume_open(struct pfs_pool *pool, const struct pfs_volume_id *id,
                                struct pfs_volume *volume)
{
  if (!pool || !pool->state.data || !pool->reader || !pool->memory || !id || !volume ||
      pfs_bytes_are_zero(id->bytes, PFS_ID_SIZE) || volume->pool || volume->state.owner ||
      volume->state.data || volume->state.size || volume->state.alignment) {
    return PFS_INVALID;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  enum pfs_status status = pfs_memory_allocate(pool->memory, sizeof(struct catalog_operation),
    _Alignof(struct catalog_operation), &storage);
  if (status != PFS_OK) {
    return status;
  }
  struct catalog_operation *operation = storage.data;
  pfs_bytes_zero(operation, sizeof(*operation));
  const struct pfs_pool_candidate *selected = selected_candidate(pool);
  operation->pool = pool;
  operation->superblock = &selected->superblock;
  operation->root = &selected->root;
  status = catalog_read(operation);
  size_t index = 0;
  if (status == PFS_OK) {
    while (index < operation->volume_count &&
           pfs_bytes_compare(operation->volumes[index].id.bytes, PFS_ID_SIZE,
                             id->bytes, PFS_ID_SIZE)) {
      ++index;
    }
    if (index == operation->volume_count) {
      status = PFS_NOT_FOUND;
    }
  }
  if (status == PFS_OK) {
    const struct pfs_volume_record *record = &operation->volumes[index];
    status = pfs_features_read(&record->features);
    if (status == PFS_OK && (record->object_root.version != PFS_FORMAT_VERSION ||
        (record->grant_root.block && record->grant_root.version != PFS_FORMAT_VERSION))) {
      status = PFS_UNSUPPORTED;
    }
  }
  if (status == PFS_OK) {
    status = pfs_memory_allocate(pool->memory, sizeof(struct volume_state),
      _Alignof(struct volume_state), &volume->state);
  }
  if (status == PFS_OK) {
    struct volume_state *state = volume->state.data;
    pfs_bytes_zero(state, sizeof(*state));
    pfs_bytes_copy(&state->record, &operation->volumes[index], sizeof(state->record));
    state->birth = operation->volume_births[index];
    operation->volume = state;
    status = ancestry_collect(operation, &state->record.root_object);
    if (status == PFS_OK && state->record.grant_root.block) {
      struct consulted_node *node;
      pfs_bytes_zero(&operation->object, sizeof(operation->object));
      status = node_get(operation, &state->record.grant_root, state->birth,
        PFS_INDEX_GRANTS, NULL, NULL, NULL, &node);
    }
    if (status == PFS_OK) {
      status = proof_complete(operation);
    }
    if (status == PFS_OK) {
      volume->pool = pool;
      ++((struct pool_state *)pool->state.data)->volumes;
    } else {
      pfs_memory_free(pool->memory, &volume->state);
    }
  }
  operation_destroy(operation, &storage);
  return status;
}

enum pfs_status
pfs_volume_close(struct pfs_volume *volume)
{
  if (!volume) {
    return PFS_INVALID;
  }
  if (!volume->pool && !volume->state.data && !volume->state.owner &&
      !volume->state.size && !volume->state.alignment) {
    return PFS_OK;
  }
  if (!volume_live(volume)) {
    return PFS_INVALID;
  }
  if (((struct volume_state *)volume->state.data)->retained) {
    return PFS_BUSY;
  }
  struct pfs_pool *pool = volume->pool;
  enum pfs_status status = pfs_memory_free(pool->memory, &volume->state);
  if (status == PFS_OK) {
    --((struct pool_state *)pool->state.data)->volumes;
    volume->pool = NULL;
  }
  return status;
}

enum pfs_status
pfs_volume_hold(struct pfs_volume *volume)
{
  if (!volume_live(volume)) {
    return PFS_INVALID;
  }
  struct volume_state *state = volume->state.data;
  if (state->retained == SIZE_MAX) {
    return PFS_LIMIT;
  }
  ++state->retained;
  return PFS_OK;
}

enum pfs_status
pfs_volume_drop(struct pfs_volume *volume)
{
  if (!volume_live(volume)) {
    return PFS_INVALID;
  }
  struct volume_state *state = volume->state.data;
  if (!state->retained) {
    return PFS_INVALID;
  }
  --state->retained;
  return PFS_OK;
}

uint64_t
pfs_volume_generation(const struct pfs_volume *volume)
{
  return volume_live(volume) ? selected_candidate(volume->pool)->superblock.header.birth : 0;
}

const struct pfs_pool_id *
pfs_volume_pool_id(const struct pfs_volume *volume)
{
  return volume_live(volume) ? &selected_candidate(volume->pool)->superblock.header.pool : NULL;
}

enum pfs_status
pfs_volume_diagnostic_metadata(struct pfs_volume *volume, struct pfs_volume_record *out)
{
  if (!volume_live(volume) || !out) {
    return PFS_INVALID;
  }
  const struct volume_state *state = volume->state.data;
  pfs_bytes_copy(out, &state->record, sizeof(*out));
  return PFS_OK;
}

enum pfs_status
pfs_volume_diagnostic_ancestry(struct pfs_volume *volume, const struct pfs_object_id *id,
                              struct pfs_object_record *out, size_t capacity, size_t *count)
{
  if (!volume_live(volume) || !id || !count || (!out && capacity) ||
      pfs_bytes_are_zero(id->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  status = ancestry_collect(operation, id);
  if (status == PFS_OK && out && capacity < operation->chain_count) {
    status = PFS_LIMIT;
  }
  status = operation_result(operation, status);
  if (status == PFS_OK) {
    if (out) {
      for (size_t i = 0; i < operation->chain_count; ++i) {
        pfs_bytes_copy(&out[i], &operation->chain[operation->chain_count - 1 - i], sizeof(out[i]));
      }
    }
    *count = operation->chain_count;
  }
  operation_destroy(operation, &storage);
  return status;
}

enum pfs_status
pfs_volume_diagnostic_object(struct pfs_volume *volume, const struct pfs_object_id *id,
                            struct pfs_object_record *out)
{
  if (!volume_live(volume) || !id || !out || pfs_bytes_are_zero(id->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  status = ancestry_collect(operation, id);
  status = operation_result(operation, status);
  if (status == PFS_OK) {
    pfs_bytes_copy(out, &operation->chain[0], sizeof(*out));
  }
  operation_destroy(operation, &storage);
  return status;
}

enum pfs_status
pfs_volume_diagnostic_resolve(struct pfs_volume *volume, const struct pfs_object_id *root,
                             const uint8_t *path, size_t length, struct pfs_object_record *out)
{
  if (!volume_live(volume) || !root || !out || (!path && length) ||
      pfs_bytes_are_zero(root->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  /* Validate all caller syntax before consulting media. */
  size_t offset = 0;
  size_t components = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && path[end] != '/') {
      ++end;
    }
    if (pfs_name_validate(path + offset, end - offset) != PFS_OK ||
        end - offset == 0 || (end - offset == 1 && path[offset] == '.') ||
        (end - offset == 2 && path[offset] == '.' && path[offset + 1] == '.') ||
        (end < length && end + 1 == length)) {
      return PFS_INVALID;
    }
    if (++components >= PFS_ANCESTRY_MAX) {
      return PFS_LIMIT;
    }
    offset = end < length ? end + 1 : end;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  status = ancestry_collect(operation, root);
  struct pfs_object_record object;
  uint64_t birth = 0;
  if (status == PFS_OK) {
    status = object_lookup(operation, root, &object, &birth);
  }
  offset = 0;
  while (status == PFS_OK && offset < length) {
    if (object.kind != PFS_OBJECT_DIRECTORY) {
      status = PFS_NOT_FOUND;
      break;
    }
    size_t end = offset;
    while (end < length && path[end] != '/') {
      ++end;
    }
    struct pfs_key key;
    pfs_bytes_zero(&key, sizeof(key));
    key.length = (uint16_t)(end - offset);
    pfs_bytes_copy(key.bytes, path + offset, key.length);
    struct pfs_key found;
    struct pfs_dirent_record entry;
    status = entry_next(operation, &object, birth, &key, false, &entry, &found);
    if (status == PFS_OK && pfs_key_compare(PFS_INDEX_DIRECTORY, &key, &found)) {
      status = PFS_NOT_FOUND;
    }
    if (status == PFS_OK) {
      struct pfs_object_id parent;
      pfs_bytes_copy(&parent, &object.id, sizeof(parent));
      status = object_lookup(operation, &entry.object, &object, &birth);
      if (status == PFS_NOT_FOUND) {
        status = PFS_CORRUPT;
      }
      if (status == PFS_OK && (object.kind != entry.child_kind ||
          pfs_bytes_compare(object.parent.bytes, PFS_ID_SIZE, parent.bytes, PFS_ID_SIZE))) {
        status = PFS_CORRUPT;
      }
    }
    offset = end < length ? end + 1 : end;
  }
  if (status == PFS_OK) {
    status = ancestry_collect(operation, &object.id);
  }
  status = operation_result(operation, status);
  if (status == PFS_OK) {
    pfs_bytes_copy(out, &object, sizeof(*out));
  }
  operation_destroy(operation, &storage);
  return status;
}

enum pfs_status
pfs_volume_diagnostic_grants(struct pfs_volume *volume, const struct pfs_object_id *id,
                            struct pfs_grant_record *out, size_t capacity, size_t *count)
{
  if (!volume_live(volume) || !id || !count || (!out && capacity) ||
      pfs_bytes_are_zero(id->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  status = ancestry_collect(operation, id);
  uint16_t kind = status == PFS_OK ? operation->chain[0].kind : 0;
  struct pfs_key lower;
  pfs_bytes_zero(&lower, sizeof(lower));
  lower.length = 2 * PFS_ID_SIZE + 1;
  pfs_bytes_copy(lower.bytes, id->bytes, PFS_ID_SIZE);
  bool strict = false;
  while (status == PFS_OK && operation->volume->record.grant_root.block) {
    struct consulted_node *node;
    uint16_t slot;
    struct pfs_key key;
    status = record_next(operation, &operation->volume->record.grant_root,
      operation->volume->birth, PFS_INDEX_GRANTS, NULL, &lower, strict, &node, &slot, &key);
    if (status == PFS_NOT_FOUND) {
      status = PFS_OK;
      break;
    }
    if (status != PFS_OK) {
      break;
    }
    if (pfs_bytes_compare(key.bytes, PFS_ID_SIZE, id->bytes, PFS_ID_SIZE)) {
      break;
    }
    if (operation->grant_count == PFS_OBJECT_GRANTS_MAX) {
      status = PFS_LIMIT;
      break;
    }
    struct pfs_record_context context;
    node_records(operation, node, &context);
    struct pfs_grant_record *grant = &operation->grants[operation->grant_count];
    status = pfs_grant_record_decode(node->data + node->tree.slots[slot].offset,
      node->tree.slots[slot].length, &context, grant);
    if (status == PFS_OK) {
      status = pfs_grant_target_validate(grant, kind);
    }
    if (status != PFS_OK) {
      break;
    }
    ++operation->grant_count;
    pfs_bytes_copy(&lower, &key, sizeof(lower));
    strict = true;
  }
  if (status == PFS_OK && out && capacity < operation->grant_count) {
    status = PFS_LIMIT;
  }
  status = operation_result(operation, status);
  if (status == PFS_OK) {
    if (out) {
      pfs_bytes_copy(out, operation->grants, operation->grant_count * sizeof(*out));
    }
    *count = operation->grant_count;
  }
  operation_destroy(operation, &storage);
  return status;
}

static enum pfs_status
extent_lookup(struct catalog_operation *operation, const struct pfs_object_record *file,
              uint64_t birth, uint64_t logical, struct pfs_extent_mapping *mapping, bool *mapped)
{
  *mapped = false;
  if (file->storage_kind == PFS_STORAGE_NONE) {
    return PFS_OK;
  }
  if (file->storage_kind == PFS_STORAGE_INLINE) {
    enum pfs_status status = pfs_extent_file_validate(&file->inline_extent, file->file_length);
    if (status != PFS_OK) {
      return status;
    }
    if (logical >= file->inline_extent.logical_first &&
        logical - file->inline_extent.logical_first < file->inline_extent.count) {
      pfs_bytes_copy(mapping, &file->inline_extent, sizeof(*mapping));
      *mapped = true;
    }
    return PFS_OK;
  }
  pfs_bytes_copy(&operation->object, &file->id, sizeof(operation->object));
  operation->file_length = file->file_length;
  struct consulted_node *node;
  enum pfs_status status = node_get(operation, &file->tree_root, birth,
    PFS_INDEX_EXTENTS, NULL, NULL, NULL, &node);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_key key;
  pfs_bytes_zero(&key, sizeof(key));
  key.length = sizeof(uint64_t);
  pfs_put_u64(key.bytes, logical);
  struct pfs_key upper;
  bool has_upper = false;
  size_t depth = 0;
  while (true) {
    if (pfs_key_compare(PFS_INDEX_EXTENTS, &key, &node->tree.minimum) < 0 || !node->tree.count) {
      return PFS_OK;
    }
    if (!node->tree.level) {
      struct pfs_record_context context;
      node_records(operation, node, &context);
      for (uint16_t i = 0; i < node->tree.count; ++i) {
        struct pfs_extent_record record;
        const struct pfs_tree_slot *location = &node->tree.slots[i];
        status = pfs_extent_record_decode(node->data + location->offset,
          location->length, &context, &record);
        if (status != PFS_OK) {
          return status;
        }
        if (logical >= record.mapping.logical_first &&
            logical - record.mapping.logical_first < record.mapping.count) {
          pfs_bytes_copy(mapping, &record.mapping, sizeof(*mapping));
          *mapped = true;
          return PFS_OK;
        }
      }
      return PFS_OK;
    }
    if (depth == PFS_TREE_DEPTH_MAX) {
      return PFS_LIMIT;
    }
    for (size_t i = 0; i < depth; ++i) {
      if (operation->frames[i].node == node) {
        return PFS_CORRUPT;
      }
    }
    operation->frames[depth++].node = node;
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
      if (pfs_key_compare(PFS_INDEX_EXTENTS, &key, &next.minimum) < 0) {
        pfs_bytes_copy(&upper, &next.minimum, sizeof(upper));
        has_upper = true;
        break;
      }
      pfs_bytes_copy(&chosen, &next, sizeof(chosen));
    }
    struct consulted_node *target;
    status = node_get(operation, &chosen.child, node->tree.header.birth,
      PFS_INDEX_EXTENTS, node, &chosen.minimum, has_upper ? &upper : NULL, &target);
    if (status != PFS_OK) {
      return status;
    }
    node = target;
  }
}

static enum pfs_status
range_match(struct catalog_operation *operation, uint64_t first, uint64_t count,
            uint64_t birth, const struct pfs_volume_id *owner)
{
  while (count) {
    struct consulted_range *range = operation->first_range.data;
    while (range && (range->first != first || range->birth != birth ||
        pfs_bytes_compare(range->owner.bytes, PFS_ID_SIZE, owner->bytes, PFS_ID_SIZE))) {
      range = range->next.data;
    }
    if (!range) {
      struct pfs_allocation *storage = operation->last_range ?
        &operation->last_range->next : &operation->first_range;
      enum pfs_status status = pfs_memory_allocate(operation->pool->memory,
        sizeof(struct consulted_range), _Alignof(struct consulted_range), storage);
      if (status != PFS_OK) {
        return status;
      }
      range = storage->data;
      pfs_bytes_zero(range, sizeof(*range));
      range->previous = operation->last_range;
      operation->last_range = range;
      range->first = first;
      range->birth = birth;
      pfs_bytes_copy(&range->owner, owner, sizeof(range->owner));
      uint64_t available;
      status = allocation_match(operation, first, birth, owner, &available,
        range->dependencies, &range->dependency_count);
      if (status != PFS_OK) {
        return status;
      }
      range->count = available < count ? available : count;
      range->local_match = true;
    }
    uint64_t take = range->count < count ? range->count : count;
    first += take;
    count -= take;
  }
  return PFS_OK;
}

enum pfs_status
pfs_volume_diagnostic_read(struct pfs_volume *volume, const struct pfs_object_id *id,
                          uint64_t offset, void *buffer, size_t length, size_t *count)
{
  if (!volume_live(volume) || !id || !count || (!buffer && length) ||
      pfs_bytes_are_zero(id->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  *count = 0;
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  status = ancestry_collect(operation, id);
  struct pfs_object_record file;
  uint64_t birth = 0;
  if (status == PFS_OK) {
    status = object_lookup(operation, id, &file, &birth);
  }
  if (status == PFS_OK && file.kind != PFS_OBJECT_FILE) {
    status = PFS_INVALID;
  }
  status = operation_result(operation, status);
  size_t remaining = 0;
  if (status == PFS_OK && offset < file.file_length) {
    uint64_t available = file.file_length - offset;
    remaining = available < length ? (size_t)available : length;
  }
  while (status == PFS_OK && remaining) {
    uint64_t logical = offset / PFS_BLOCK_SIZE;
    size_t within = (size_t)(offset % PFS_BLOCK_SIZE);
    size_t take = PFS_BLOCK_SIZE - within;
    if (take > remaining) {
      take = remaining;
    }
    struct pfs_extent_mapping mapping;
    bool mapped;
    status = extent_lookup(operation, &file, birth, logical, &mapping, &mapped);
    if (status == PFS_OK && mapped) {
      status = range_match(operation, mapping.physical_first, mapping.count, mapping.birth,
        &operation->volume->record.id);
    }
    if (status == PFS_OK) {
      status = proof_complete(operation);
    }
    if (status != PFS_OK) {
      break;
    }
    if (mapped) {
      uint64_t physical = mapping.physical_first + logical - mapping.logical_first;
      status = pfs_block_read(operation->pool->reader, physical, 1,
        operation->file_data, sizeof(operation->file_data));
      if (status != PFS_OK) {
        break;
      }
      pfs_bytes_copy((uint8_t *)buffer + *count, operation->file_data + within, take);
    } else {
      pfs_bytes_zero((uint8_t *)buffer + *count, take);
    }
    *count += take;
    offset += take;
    remaining -= take;
  }
  operation_destroy(operation, &storage);
  return status;
}

struct directory_state {
  struct pfs_object_id object;
  struct pfs_key previous;
  uint64_t generation;
  uint64_t emitted;
  bool started;
  bool done;
};

enum pfs_status
pfs_volume_diagnostic_directory_open(struct pfs_volume *volume, const struct pfs_object_id *id,
                                    struct pfs_directory_cursor *cursor)
{
  if (!volume_live(volume) || !id || !cursor || cursor->volume || cursor->state.owner ||
      cursor->state.data || cursor->state.size || cursor->state.alignment) {
    return PFS_INVALID;
  }
  struct pfs_object_record object;
  enum pfs_status status = pfs_volume_diagnostic_object(volume, id, &object);
  if (status != PFS_OK) {
    return status;
  }
  if (object.kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
  status = pfs_memory_allocate(volume->pool->memory, sizeof(struct directory_state),
    _Alignof(struct directory_state), &cursor->state);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_hold(volume);
  if (status != PFS_OK) {
    pfs_memory_free(volume->pool->memory, &cursor->state);
    return status;
  }
  struct directory_state *state = cursor->state.data;
  pfs_bytes_zero(state, sizeof(*state));
  pfs_bytes_copy(&state->object, id, sizeof(state->object));
  state->generation = pfs_volume_generation(volume);
  cursor->volume = volume;
  return PFS_OK;
}

enum pfs_status
pfs_directory_close(struct pfs_directory_cursor *cursor)
{
  if (!cursor) {
    return PFS_INVALID;
  }
  if (!cursor->volume && !cursor->state.data && !cursor->state.owner &&
      !cursor->state.size && !cursor->state.alignment) {
    return PFS_OK;
  }
  if (!volume_live(cursor->volume) || !cursor->state.data) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_memory_free(cursor->volume->pool->memory, &cursor->state);
  if (status == PFS_OK) {
    status = pfs_volume_drop(cursor->volume);
    cursor->volume = NULL;
  }
  return status;
}

enum pfs_status
pfs_directory_next(struct pfs_directory_cursor *cursor, struct pfs_dirent_record *out,
                   size_t capacity, size_t *count, bool *done)
{
  if (!cursor || !volume_live(cursor->volume) || !cursor->state.data || !out ||
      !capacity || !count || !done) {
    return PFS_INVALID;
  }
  if (capacity > PFS_RECORD_COUNT_MAX || capacity > SIZE_MAX / sizeof(*out)) {
    return PFS_LIMIT;
  }
  struct directory_state *state = cursor->state.data;
  if (state->generation != pfs_volume_generation(cursor->volume)) {
    return PFS_INVALID;
  }
  if (state->done) {
    *count = 0;
    *done = true;
    return PFS_OK;
  }
  struct pfs_allocation storage;
  pfs_bytes_zero(&storage, sizeof(storage));
  struct catalog_operation *operation;
  enum pfs_status status = operation_create(cursor->volume, &storage, &operation);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_allocation page;
  pfs_bytes_zero(&page, sizeof(page));
  status = pfs_memory_allocate(cursor->volume->pool->memory, capacity * sizeof(*out),
    _Alignof(struct pfs_dirent_record), &page);
  if (status != PFS_OK) {
    operation_destroy(operation, &storage);
    return status;
  }
  struct pfs_dirent_record *entries = page.data;
  status = ancestry_collect(operation, &state->object);
  struct pfs_object_record directory;
  uint64_t birth;
  if (status == PFS_OK) {
    status = object_lookup(operation, &state->object, &directory, &birth);
  }
  struct pfs_key previous;
  pfs_bytes_copy(&previous, &state->previous, sizeof(previous));
  bool started = state->started;
  bool exhausted = false;
  size_t used = 0;
  while (status == PFS_OK && used < capacity) {
    struct pfs_key key;
    status = entry_next(operation, &directory, birth, started ? &previous : NULL,
      started, &entries[used], &key);
    if (status == PFS_NOT_FOUND) {
      status = PFS_OK;
      exhausted = true;
      break;
    }
    if (status != PFS_OK) {
      break;
    }
    struct pfs_object_record child;
    status = object_lookup(operation, &entries[used].object, &child, NULL);
    if (status == PFS_NOT_FOUND) {
      status = PFS_CORRUPT;
    }
    if (status == PFS_OK && (child.kind != entries[used].child_kind ||
        pfs_bytes_compare(child.parent.bytes, PFS_ID_SIZE, directory.id.bytes, PFS_ID_SIZE))) {
      status = PFS_CORRUPT;
    }
    if (status != PFS_OK) {
      break;
    }
    ++used;
    pfs_bytes_copy(&previous, &key, sizeof(previous));
    started = true;
  }
  if (status == PFS_OK && (state->emitted > directory.directory_count ||
      used > directory.directory_count - state->emitted ||
      (exhausted && state->emitted + used != directory.directory_count))) {
    status = PFS_CORRUPT;
  }
  if (status == PFS_OK) {
    status = proof_complete(operation);
  }
  if (status == PFS_OK) {
    pfs_bytes_copy(out, entries, used * sizeof(*out));
    *count = used;
    *done = exhausted;
    pfs_bytes_copy(&state->previous, &previous, sizeof(state->previous));
    state->started = started;
    state->done = exhausted;
    state->emitted += used;
  }
  pfs_memory_free(cursor->volume->pool->memory, &page);
  operation_destroy(operation, &storage);
  return status;
}
