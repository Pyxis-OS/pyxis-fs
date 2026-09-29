#include <pyxis_fs/build.h>
#include <pyxis_fs/tree.h>

#include "internal.h"

#define BUILD_TREE_COUNT 3u
#define BUILD_NODES_MAX 64u
#define BUILD_VOLUME_BLOCKS 2u
#define COW_FLOOR UINT64_C(1024)
#define MIGRATION_FLOOR UINT64_C(1024)
#define RECOVERY_FLOOR UINT64_C(256)

struct build_node {
  uint64_t block;
  uint16_t first;
  uint16_t count;
  uint16_t level;
  struct pfs_key minimum;
};

struct build_tree {
  uint16_t kind;
  uint16_t count;
  struct build_node nodes[BUILD_NODES_MAX];
};

struct build_state {
  struct pfs_empty_volume specs[PFS_VOLUME_MAX];
  struct pfs_volume_record volumes[PFS_VOLUME_MAX];
  uint16_t id_order[PFS_VOLUME_MAX];
  uint16_t name_order[PFS_VOLUME_MAX];
  struct build_tree trees[BUILD_TREE_COUNT];
  uint8_t records[PFS_BLOCK_SIZE];
  uint8_t output[PFS_BLOCK_SIZE];
  struct pfs_encoded_record encoded[PFS_TREE_SLOTS_MAX];
  struct pfs_tree tree;
};

static size_t
aligned_size(size_t value)
{
  return (value + 7) & ~(size_t)7;
}

static struct pfs_reference
reference(uint64_t block, uint16_t type)
{
  return (struct pfs_reference){
    .block = block,
    .birth = 1,
    .type = type,
    .version = PFS_FORMAT_VERSION,
  };
}

static struct pfs_block_header
header(const struct pfs_empty_plan *plan, uint64_t block, uint16_t type)
{
  return (struct pfs_block_header){
    .type = type,
    .version = PFS_FORMAT_VERSION,
    .pool = plan->pool,
    .block = block,
    .birth = 1,
  };
}

static struct pfs_record_context
record_context(const struct pfs_empty_plan *plan)
{
  return (struct pfs_record_context){
    .block_count = plan->block_count,
    .selected_generation = 1,
    .containing_birth = 1,
  };
}

static int
volume_compare(const struct build_state *state, uint16_t kind,
               uint16_t left, uint16_t right)
{
  if (kind == PFS_INDEX_VOLUMES) {
    return pfs_bytes_compare(state->specs[left].id.bytes, PFS_ID_SIZE,
                             state->specs[right].id.bytes, PFS_ID_SIZE);
  }
  const struct pfs_name *a = &state->specs[left].name;
  const struct pfs_name *b = &state->specs[right].name;
  return pfs_bytes_compare(a->bytes, a->length, b->bytes, b->length);
}

static void
sort_volumes(struct build_state *state, uint16_t kind, uint16_t *order, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    uint16_t value = (uint16_t)i;
    size_t at = i;
    while (at && volume_compare(state, kind, order[at - 1], value) > 0) {
      order[at] = order[at - 1];
      --at;
    }
    order[at] = value;
  }
}

static size_t
leaf_size(const struct build_state *state, uint16_t kind, size_t index)
{
  if (kind == PFS_INDEX_VOLUMES) {
    return PFS_VOLUME_RECORD_SIZE;
  }
  if (kind == PFS_INDEX_VOLUME_NAMES) {
    return aligned_size(PFS_NAME_RECORD_PREFIX_SIZE +
                        state->specs[state->name_order[index]].name.length);
  }
  return PFS_ALLOCATION_RECORD_SIZE;
}

static size_t
internal_size(const struct build_state *state, const struct build_tree *tree,
              size_t child)
{
  size_t key_size = sizeof(uint64_t);
  if (tree->kind == PFS_INDEX_VOLUMES) {
    key_size = PFS_ID_SIZE;
  } else if (tree->kind == PFS_INDEX_VOLUME_NAMES) {
    const struct build_node *node = &tree->nodes[child];
    while (node->level) {
      node = &tree->nodes[node->first];
    }
    key_size = state->specs[state->name_order[node->first]].name.length;
  }
  return aligned_size(48 + key_size);
}

static enum pfs_status
plan_tree(struct build_state *state, struct build_tree *tree, size_t records)
{
  size_t first = 0;
  size_t end = records;
  uint16_t level = 0;
  for (;;) {
    size_t level_first = tree->count;
    while (first < end) {
      if (tree->count == BUILD_NODES_MAX) {
        return PFS_LIMIT;
      }
      struct build_node *node = &tree->nodes[tree->count++];
      node->first = (uint16_t)first;
      node->level = level;
      size_t bytes = 0;
      while (first < end) {
        size_t length = level ? internal_size(state, tree, first) :
                                leaf_size(state, tree->kind, first);
        size_t count = node->count + 1;
        size_t start = aligned_size(PFS_TREE_HEADER_SIZE + count * PFS_TREE_SLOT_SIZE);
        if (start > PFS_BLOCK_SIZE || bytes > PFS_BLOCK_SIZE - start ||
            length > PFS_BLOCK_SIZE - start - bytes) {
          break;
        }
        bytes += length;
        ++node->count;
        ++first;
      }
      if (!node->count) {
        return PFS_LIMIT;
      }
    }
    size_t level_count = tree->count - level_first;
    if (level && tree->nodes[tree->count - 1].count == 1) {
      /* A full preceding node has at least twelve children, even with the
       * longest allowed name. Moving one keeps both internal nodes valid. */
      if (level_count < 2 || tree->nodes[tree->count - 2].count < 3) {
        return PFS_INVALID;
      }
      --tree->nodes[tree->count - 2].count;
      --tree->nodes[tree->count - 1].first;
      ++tree->nodes[tree->count - 1].count;
    }
    if (level_count == 1) {
      return PFS_OK;
    }
    if (++level >= PFS_TREE_DEPTH_MAX) {
      return PFS_LIMIT;
    }
    first = level_first;
    end = tree->count;
  }
}

static enum pfs_status
validate_spec(const struct pfs_empty_spec *spec)
{
  if (!spec || !spec->volumes || !spec->volume_count ||
      pfs_bytes_are_zero(spec->pool.bytes, PFS_ID_SIZE) ||
      spec->reserve_set & ~(PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY)) {
    return PFS_INVALID;
  }
  if (spec->volume_count > PFS_VOLUME_MAX || spec->block_count < PFS_POOL_BLOCKS_MIN ||
      spec->block_count > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  for (size_t i = 0; i < spec->volume_count; ++i) {
    const struct pfs_empty_volume *volume = &spec->volumes[i];
    if (pfs_bytes_are_zero(volume->id.bytes, PFS_ID_SIZE) ||
        pfs_bytes_are_zero(volume->root_object.bytes, PFS_ID_SIZE) ||
        pfs_bytes_are_zero(volume->owner.bytes, PFS_ID_SIZE) ||
        pfs_name_validate(volume->name.bytes, volume->name.length) != PFS_OK ||
        !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, spec->pool.bytes, PFS_ID_SIZE) ||
        !pfs_bytes_compare(volume->root_object.bytes, PFS_ID_SIZE, spec->pool.bytes, PFS_ID_SIZE) ||
        !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, volume->root_object.bytes, PFS_ID_SIZE)) {
      return PFS_INVALID;
    }
    for (size_t j = 0; j < i; ++j) {
      const struct pfs_empty_volume *other = &spec->volumes[j];
      if (!pfs_bytes_compare(volume->name.bytes, volume->name.length,
                             other->name.bytes, other->name.length) ||
          !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, other->id.bytes, PFS_ID_SIZE) ||
          !pfs_bytes_compare(volume->root_object.bytes, PFS_ID_SIZE,
                             other->root_object.bytes, PFS_ID_SIZE) ||
          !pfs_bytes_compare(volume->root_object.bytes, PFS_ID_SIZE, other->id.bytes, PFS_ID_SIZE) ||
          !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, other->root_object.bytes, PFS_ID_SIZE)) {
        return PFS_INVALID;
      }
    }
  }
  return PFS_OK;
}

static uint64_t
default_reserve(uint64_t usable, uint64_t divisor, uint64_t floor)
{
  uint64_t value = usable / divisor + (usable % divisor != 0);
  return value < floor ? floor : value;
}

static enum pfs_status
plan_promises(const struct pfs_empty_spec *spec, struct pfs_empty_plan *plan,
              struct build_state *state)
{
  uint64_t usable = plan->block_count - 2;
  struct pfs_pool_root *root = &plan->pool_root;
  root->cow.capacity = spec->reserve_set & PFS_RESERVE_COW ? spec->cow_reserve :
                        default_reserve(usable, 128, COW_FLOOR);
  root->migration.capacity = spec->reserve_set & PFS_RESERVE_MIGRATION ?
                              spec->migration_reserve : default_reserve(usable, 128, MIGRATION_FLOOR);
  root->recovery.capacity = spec->reserve_set & PFS_RESERVE_RECOVERY ? spec->recovery_reserve :
                             default_reserve(usable, 256, RECOVERY_FLOOR);
  if (root->cow.capacity < COW_FLOOR || root->migration.capacity < MIGRATION_FLOOR ||
      root->recovery.capacity < RECOVERY_FLOOR) {
    return PFS_INVALID;
  }
  uint64_t remaining = usable - root->live_pool;
  const uint64_t budgets[] = { root->cow.capacity, root->migration.capacity, root->recovery.capacity };
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
    if (budgets[i] > remaining) {
      return PFS_LIMIT;
    }
    remaining -= budgets[i];
  }
  uint64_t default_quota = remaining;
  if (root->live_volume > remaining) {
    return PFS_LIMIT;
  }
  remaining -= root->live_volume;
  size_t automatic = 0;
  for (size_t i = 0; i < plan->volume_count; ++i) {
    struct pfs_volume_record *volume = &state->volumes[i];
    if (!state->specs[i].guarantee_set) {
      ++automatic;
      continue;
    }
    volume->guarantee = state->specs[i].guarantee;
    uint64_t unused = volume->guarantee > BUILD_VOLUME_BLOCKS ?
                        volume->guarantee - BUILD_VOLUME_BLOCKS : 0;
    if (unused > remaining) {
      return PFS_LIMIT;
    }
    remaining -= unused;
  }
  uint64_t shared = automatic ? remaining / 2 : 0;
  uint64_t share = automatic ? shared / automatic : 0;
  uint64_t remainder = automatic ? shared % automatic : 0;
  for (size_t at = 0; at < plan->volume_count; ++at) {
    size_t i = state->name_order[at];
    struct pfs_volume_record *volume = &state->volumes[i];
    if (!state->specs[i].guarantee_set) {
      volume->guarantee = BUILD_VOLUME_BLOCKS + share + (remainder != 0);
      if (remainder) {
        --remainder;
      }
    }
    volume->quota = state->specs[i].quota_set ? state->specs[i].quota : default_quota;
    if (volume->quota < BUILD_VOLUME_BLOCKS || volume->quota < volume->guarantee) {
      return PFS_LIMIT;
    }
  }
  plan->unpromised = remaining - shared;
  return PFS_OK;
}

static void
leaf_minimum(const struct pfs_empty_plan *plan, const struct build_state *state,
             uint16_t kind, size_t first, struct pfs_key *key)
{
  if (kind == PFS_INDEX_VOLUMES) {
    key->length = PFS_ID_SIZE;
    pfs_bytes_copy(key->bytes, state->volumes[state->id_order[first]].id.bytes, key->length);
  } else if (kind == PFS_INDEX_VOLUME_NAMES) {
    const struct pfs_name *name = &state->volumes[state->name_order[first]].name;
    key->length = name->length;
    pfs_bytes_copy(key->bytes, name->bytes, key->length);
  } else {
    uint64_t block = first ? plan->pool_root.live_pool + 1 +
                             (first - 1) * BUILD_VOLUME_BLOCKS : 1;
    key->length = sizeof(block);
    pfs_put_u64(key->bytes, block);
  }
}

enum pfs_status
pfs_empty_plan_create(struct pfs_memory *memory, const struct pfs_empty_spec *spec,
                       struct pfs_empty_plan *plan)
{
  if (!memory || !plan || plan->state.owner || plan->state.data || plan->state.size ||
      plan->state.alignment || plan->volumes || plan->volume_count || plan->block_count) {
    return PFS_INVALID;
  }
  enum pfs_status status = validate_spec(spec);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_memory_allocate(memory, sizeof(struct build_state), _Alignof(struct build_state),
                               &plan->state);
  if (status != PFS_OK) {
    return status;
  }
  struct build_state *state = plan->state.data;
  pfs_bytes_zero(state, sizeof(*state));
  plan->block_count = spec->block_count;
  plan->pool = spec->pool;
  plan->volume_count = spec->volume_count;
  plan->volumes = state->volumes;
  pfs_bytes_copy(state->specs, spec->volumes, spec->volume_count * sizeof(*spec->volumes));
  sort_volumes(state, PFS_INDEX_VOLUMES, state->id_order, plan->volume_count);
  sort_volumes(state, PFS_INDEX_VOLUME_NAMES, state->name_order, plan->volume_count);
  uint64_t next_block = 2;
  for (size_t i = 0; i < BUILD_TREE_COUNT; ++i) {
    struct build_tree *tree = &state->trees[i];
    tree->kind = (uint16_t)(PFS_INDEX_VOLUMES + i);
    status = plan_tree(state, tree, plan->volume_count + (i == 2 ? 2 : 0));
    if (status != PFS_OK) {
      goto fail;
    }
    for (size_t j = 0; j < tree->count; ++j) {
      tree->nodes[j].block = next_block++;
    }
  }
  struct pfs_pool_root *root = &plan->pool_root;
  root->header = header(plan, 1, PFS_BLOCK_POOL);
  root->header.used = 288;
  root->volumes = reference(state->trees[0].nodes[state->trees[0].count - 1].block, PFS_BLOCK_TREE);
  root->volume_names = reference(state->trees[1].nodes[state->trees[1].count - 1].block, PFS_BLOCK_TREE);
  root->allocation = reference(state->trees[2].nodes[state->trees[2].count - 1].block, PFS_BLOCK_TREE);
  root->volume_count = plan->volume_count;
  root->live_pool = next_block - 1;
  root->live_volume = plan->volume_count * BUILD_VOLUME_BLOCKS;
  if (root->live_pool + root->live_volume >= plan->block_count - 2) {
    status = PFS_LIMIT;
    goto fail;
  }
  root->free = plan->block_count - 2 - root->live_pool - root->live_volume;
  for (size_t i = 0; i < plan->volume_count; ++i) {
    struct pfs_volume_record *volume = &state->volumes[i];
    volume->id = state->specs[i].id;
    pfs_bytes_copy(&volume->name, &state->specs[i].name, sizeof(volume->name));
    volume->root_object = state->specs[i].root_object;
    volume->object_root = reference(next_block++, PFS_BLOCK_TREE);
    volume->grant_root = reference(next_block++, PFS_BLOCK_TREE);
    volume->live_blocks = BUILD_VOLUME_BLOCKS;
    volume->object_count = 1;
  }
  status = plan_promises(spec, plan, state);
  if (status != PFS_OK) {
    goto fail;
  }
  for (size_t i = 0; i < BUILD_TREE_COUNT; ++i) {
    struct build_tree *tree = &state->trees[i];
    for (size_t j = 0; j < tree->count; ++j) {
      struct build_node *node = &tree->nodes[j];
      if (node->level) {
        pfs_bytes_copy(&node->minimum, &tree->nodes[node->first].minimum,
                        sizeof(node->minimum));
      } else {
        leaf_minimum(plan, state, tree->kind, node->first, &node->minimum);
      }
    }
  }
  return PFS_OK;

fail:
  pfs_empty_plan_destroy(plan);
  return status;
}

static enum pfs_status
encode_leaf(const struct pfs_empty_plan *plan, struct build_state *state,
            uint16_t kind, size_t index, uint8_t *data, size_t capacity, size_t *written)
{
  struct pfs_record_context context = record_context(plan);
  if (kind == PFS_INDEX_VOLUMES) {
    return pfs_volume_record_encode(data, capacity, &context,
                                    &state->volumes[state->id_order[index]], written);
  }
  if (kind == PFS_INDEX_VOLUME_NAMES) {
    const struct pfs_volume_record *volume = &state->volumes[state->name_order[index]];
    struct pfs_volume_name_record record;
    pfs_bytes_zero(&record, sizeof(record));
    pfs_bytes_copy(&record.name, &volume->name, sizeof(record.name));
    record.volume = volume->id;
    return pfs_volume_name_record_encode(data, capacity, &context, &record, written);
  }
  struct pfs_allocation_record record = {0};
  if (!index) {
    record.first = 1;
    record.count = plan->pool_root.live_pool;
    record.state = PFS_ALLOCATION_POOL;
    record.birth = 1;
  } else if (index <= plan->volume_count) {
    const struct pfs_volume_record *volume = &state->volumes[index - 1];
    record.first = volume->object_root.block;
    record.count = BUILD_VOLUME_BLOCKS;
    record.state = PFS_ALLOCATION_VOLUME;
    record.owner = volume->id;
    record.birth = 1;
  } else {
    record.first = plan->pool_root.live_pool + plan->pool_root.live_volume + 1;
    record.count = plan->pool_root.free;
  }
  return pfs_allocation_record_encode(data, capacity, &context, &record, written);
}

static enum pfs_status
write_tree_node(const struct pfs_empty_plan *plan, struct build_state *state,
                const struct pfs_block_builder *builder, uint16_t kind,
                const struct pfs_volume_id *volume, uint64_t block,
                uint16_t level, uint16_t count)
{
  pfs_bytes_zero(&state->tree, sizeof(state->tree));
  state->tree.header = header(plan, block, PFS_BLOCK_TREE);
  state->tree.kind = kind;
  state->tree.level = level;
  state->tree.count = count;
  if (volume) {
    state->tree.volume = *volume;
  }
  struct pfs_tree_context context = {
    .block = {
      .block_count = plan->block_count,
      .selected_generation = 1,
      .referring_birth = 1,
      .pool = plan->pool,
      .reference = reference(block, PFS_BLOCK_TREE),
    },
    .kind = kind,
    .volume = state->tree.volume,
  };
  enum pfs_status status = pfs_tree_encode(state->output, sizeof(state->output),
                                         &context, &state->tree, state->encoded);
  if (status != PFS_OK) {
    return status;
  }
  return pfs_block_write(builder, block, 1, state->output, sizeof(state->output));
}

static enum pfs_status
write_pool_trees(const struct pfs_empty_plan *plan, struct build_state *state,
                 const struct pfs_block_builder *builder)
{
  struct pfs_record_context context = record_context(plan);
  for (size_t i = 0; i < BUILD_TREE_COUNT; ++i) {
    const struct build_tree *tree = &state->trees[i];
    for (size_t j = 0; j < tree->count; ++j) {
      const struct build_node *node = &tree->nodes[j];
      size_t offset = 0;
      for (size_t k = 0; k < node->count; ++k) {
        size_t written = 0;
        enum pfs_status status;
        if (node->level) {
          const struct build_node *child = &tree->nodes[node->first + k];
          struct pfs_internal_record record;
          pfs_bytes_zero(&record, sizeof(record));
          record.child = reference(child->block, PFS_BLOCK_TREE);
          pfs_bytes_copy(&record.minimum, &child->minimum, sizeof(record.minimum));
          status = pfs_internal_record_encode(state->records + offset,
            sizeof(state->records) - offset, tree->kind, &context, &record, &written);
        } else {
          status = encode_leaf(plan, state, tree->kind, node->first + k,
                                state->records + offset, sizeof(state->records) - offset, &written);
        }
        if (status != PFS_OK) {
          return status;
        }
        state->encoded[k] = (struct pfs_encoded_record){state->records + offset, written};
        offset += written;
      }
      enum pfs_status status = write_tree_node(plan, state, builder, tree->kind,
        NULL, node->block, node->level, node->count);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  return PFS_OK;
}

static enum pfs_status
write_volume_trees(const struct pfs_empty_plan *plan, struct build_state *state,
                   const struct pfs_block_builder *builder)
{
  struct pfs_record_context context = record_context(plan);
  for (size_t i = 0; i < plan->volume_count; ++i) {
    const struct pfs_volume_record *volume = &state->volumes[i];
    struct pfs_object_record object = {
      .id = volume->root_object,
      .kind = PFS_OBJECT_DIRECTORY,
      .storage_kind = PFS_STORAGE_NONE,
      .owner = state->specs[i].owner,
    };
    size_t written = 0;
    enum pfs_status status = pfs_object_record_encode(state->records, sizeof(state->records),
                                                     &context, &object, &written);
    if (status != PFS_OK) {
      return status;
    }
    state->encoded[0] = (struct pfs_encoded_record){state->records, written};
    status = write_tree_node(plan, state, builder, PFS_INDEX_OBJECTS, &volume->id,
                             volume->object_root.block, 0, 1);
    if (status != PFS_OK) {
      return status;
    }
    struct pfs_grant_record grant = {
      .object = volume->root_object,
      .principal = state->specs[i].owner,
      .scope = PFS_SCOPE_SUBTREE,
      .file_rights = PFS_FILE_RIGHTS_ALL,
      .directory_rights = PFS_DIR_RIGHTS_ALL,
      .admin_rights = PFS_ADMIN_RIGHTS_ALL,
    };
    status = pfs_grant_record_encode(state->records, sizeof(state->records), &context, &grant, &written);
    if (status != PFS_OK) {
      return status;
    }
    state->encoded[0] = (struct pfs_encoded_record){state->records, written};
    status = write_tree_node(plan, state, builder, PFS_INDEX_GRANTS, &volume->id,
                             volume->grant_root.block, 0, 1);
    if (status != PFS_OK) {
      return status;
    }
  }
  return PFS_OK;
}

enum pfs_status
pfs_empty_build(const struct pfs_empty_plan *plan, const struct pfs_block_builder *builder)
{
  if (!plan || !plan->state.data || !plan->state.owner ||
      plan->state.size != sizeof(struct build_state) || !builder ||
      !builder->reader.read || !builder->write || !builder->flush ||
      builder->reader.geometry.block_count != plan->block_count ||
      !builder->reader.geometry.max_transfer_blocks) {
    return PFS_INVALID;
  }
  struct build_state *state = plan->state.data;
  enum pfs_status status = write_pool_trees(plan, state, builder);
  if (status == PFS_OK) {
    status = write_volume_trees(plan, state, builder);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_block_context context = {
    .block_count = plan->block_count,
    .selected_generation = 1,
    .referring_birth = 1,
    .pool = plan->pool,
    .reference = reference(1, PFS_BLOCK_POOL),
  };
  status = pfs_pool_root_encode(state->output, sizeof(state->output), &context, &plan->pool_root);
  if (status == PFS_OK) {
    status = pfs_block_write(builder, 1, 1, state->output, sizeof(state->output));
  }
  if (status == PFS_OK) {
    status = pfs_block_flush(builder);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_superblock super = {
    .header = header(plan, 0, PFS_BLOCK_SUPER),
    .block_count = plan->block_count,
    .root = reference(1, PFS_BLOCK_POOL),
  };
  super.header.used = 192;
  for (size_t i = 0; i < 2; ++i) {
    super.header.block = i ? plan->block_count - 1 : 0;
    status = pfs_superblock_encode(state->output, sizeof(state->output), &super);
    if (status == PFS_OK) {
      status = pfs_block_write(builder, super.header.block, 1, state->output, sizeof(state->output));
    }
    if (status != PFS_OK) {
      return status;
    }
  }
  return pfs_block_flush(builder);
}

enum pfs_status
pfs_empty_plan_destroy(struct pfs_empty_plan *plan)
{
  if (!plan) {
    return PFS_INVALID;
  }
  if (plan->state.owner) {
    enum pfs_status status = pfs_memory_free(plan->state.owner, &plan->state);
    if (status != PFS_OK) {
      return status;
    }
  } else if (plan->state.data || plan->state.size || plan->state.alignment) {
    return PFS_INVALID;
  }
  pfs_bytes_zero(plan, sizeof(*plan));
  return PFS_OK;
}
