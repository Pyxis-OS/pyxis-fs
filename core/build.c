#include <pyxis_fs/build.h>
#include <pyxis_fs/tree.h>

#include "internal.h"

#define BUILD_POOL_TREES 3u
#define BUILD_POOL_NODES 192u
#define COW_FLOOR UINT64_C(1024)
#define MIGRATION_FLOOR UINT64_C(1024)
#define RECOVERY_FLOOR UINT64_C(256)
#define BUILD_ANCESTRY_MAX 256u
#define BUILD_NAMESPACE_MIN 6u

struct build_node {
  uint64_t block;
  size_t first;
  uint16_t count;
  uint16_t level;
};

struct build_tree {
  size_t first_node;
  size_t node_count;
  size_t volume;
  size_t object;
  uint16_t kind;
};

struct build_object {
  struct pfs_build_object spec;
  size_t child_first;
  size_t child_count;
  size_t tree;
  uint64_t data_first;
  uint64_t data_count;
  uint16_t depth;
};

struct build_volume {
  size_t first;
  size_t object_tree;
  size_t grant_tree;
  uint64_t first_block;
};

struct build_state {
  struct pfs_build_volume specs[PFS_VOLUME_MAX];
  struct pfs_volume_record volumes[PFS_VOLUME_MAX];
  struct build_volume layout[PFS_VOLUME_MAX];
  uint16_t id_order[PFS_VOLUME_MAX];
  uint16_t name_order[PFS_VOLUME_MAX];
  struct build_object *objects;
  size_t *object_order;
  size_t *child_order;
  struct build_tree *trees;
  struct build_node *nodes;
  size_t object_count;
  size_t tree_count;
  size_t tree_capacity;
  size_t node_count;
  size_t node_capacity;
  bool has_data;
  uint8_t records[PFS_BLOCK_SIZE];
  uint8_t output[PFS_IO_BLOCKS_MAX * PFS_BLOCK_SIZE];
  struct pfs_encoded_record encoded[PFS_TREE_SLOTS_MAX];
  struct pfs_tree tree;
};

static size_t
aligned_size(size_t value)
{
  return (value + 7) & ~(size_t)7;
}

static bool
append_size(size_t *total, size_t count, size_t size)
{
  if (*total > SIZE_MAX - 7) {
    return false;
  }
  *total = aligned_size(*total);
  if (count > (SIZE_MAX - *total) / size) {
    return false;
  }
  *total += count * size;
  return true;
}

static void *
array_at(struct build_state *state, size_t *offset, size_t count, size_t size)
{
  *offset = aligned_size(*offset);
  void *array = (uint8_t *)state + *offset;
  *offset += count * size;
  return array;
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
header(const struct pfs_build_plan *plan, uint64_t block, uint16_t type)
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
record_context(const struct pfs_build_plan *plan)
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

static int
object_compare(const struct build_state *state, bool children, size_t a, size_t b)
{
  const struct pfs_build_object *left = &state->objects[a].spec;
  const struct pfs_build_object *right = &state->objects[b].spec;
  if (!children) {
    return pfs_bytes_compare(left->id.bytes, PFS_ID_SIZE, right->id.bytes, PFS_ID_SIZE);
  }
  if (left->parent != right->parent) {
    return left->parent < right->parent ? -1 : 1;
  }
  return pfs_bytes_compare(left->name.bytes, left->name.length,
                           right->name.bytes, right->name.length);
}

static void
sift_objects(const struct build_state *state, bool children, size_t *order,
             size_t root, size_t count)
{
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        object_compare(state, children, order[child], order[child + 1]) < 0) {
      ++child;
    }
    if (object_compare(state, children, order[root], order[child]) >= 0) {
      return;
    }
    size_t swap = order[root];
    order[root] = order[child];
    order[child] = swap;
    root = child;
  }
}

static void
sort_objects(const struct build_state *state, bool children, size_t *order, size_t count)
{
  for (size_t i = count / 2; i; --i) {
    sift_objects(state, children, order, i - 1, count);
  }
  for (size_t end = count; end > 1; --end) {
    size_t swap = order[0];
    order[0] = order[end - 1];
    order[end - 1] = swap;
    sift_objects(state, children, order, 0, end - 1);
  }
}

static enum pfs_status
validate_spec(const struct pfs_build_spec *spec, size_t *objects)
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
  *objects = 0;
  for (size_t i = 0; i < spec->volume_count; ++i) {
    const struct pfs_build_volume *volume = &spec->volumes[i];
    if (!volume->objects || !volume->object_count ||
        pfs_bytes_are_zero(volume->id.bytes, PFS_ID_SIZE) ||
        pfs_bytes_are_zero(volume->root_object.bytes, PFS_ID_SIZE) ||
        pfs_bytes_are_zero(volume->owner.bytes, PFS_ID_SIZE) ||
        pfs_name_validate(volume->name.bytes, volume->name.length) != PFS_OK ||
        !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, spec->pool.bytes, PFS_ID_SIZE)) {
      return PFS_INVALID;
    }
    if (volume->object_count > PFS_RECORD_COUNT_MAX ||
        volume->object_count > SIZE_MAX - *objects) {
      return PFS_LIMIT;
    }
    *objects += volume->object_count;
    if (*objects > PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    for (size_t j = 0; j < i; ++j) {
      const struct pfs_build_volume *other = &spec->volumes[j];
      if (!pfs_bytes_compare(volume->name.bytes, volume->name.length,
                             other->name.bytes, other->name.length) ||
          !pfs_bytes_compare(volume->id.bytes, PFS_ID_SIZE, other->id.bytes, PFS_ID_SIZE)) {
        return PFS_INVALID;
      }
    }
  }
  return PFS_OK;
}

static enum pfs_status
copy_objects(const struct pfs_build_spec *spec, struct build_state *state)
{
  size_t first = 0;
  for (size_t i = 0; i < spec->volume_count; ++i) {
    const struct pfs_build_volume *volume = &spec->volumes[i];
    state->layout[i].first = first;
    for (size_t j = 0; j < volume->object_count; ++j) {
      struct build_object *object = &state->objects[first + j];
      pfs_bytes_copy(&object->spec, &volume->objects[j], sizeof(object->spec));
      object->tree = SIZE_MAX;
      const struct pfs_build_object *input = &object->spec;
      if (pfs_bytes_are_zero(input->id.bytes, PFS_ID_SIZE) ||
          (input->kind != PFS_OBJECT_FILE && input->kind != PFS_OBJECT_DIRECTORY) ||
          (input->kind == PFS_OBJECT_DIRECTORY && input->file_length)) {
        return PFS_INVALID;
      }
      if (input->file_length > PFS_FILE_SIZE_MAX) {
        return PFS_LIMIT;
      }
      if (!j) {
        if (input->kind != PFS_OBJECT_DIRECTORY || input->parent != UINT32_MAX ||
            input->name.length ||
            pfs_bytes_compare(input->id.bytes, PFS_ID_SIZE,
                               volume->root_object.bytes, PFS_ID_SIZE)) {
          return PFS_INVALID;
        }
        object->depth = 1;
      } else {
        if (input->parent >= j ||
            state->objects[first + input->parent].spec.kind != PFS_OBJECT_DIRECTORY ||
            pfs_name_validate(input->name.bytes, input->name.length) != PFS_OK) {
          return PFS_INVALID;
        }
        object->depth = state->objects[first + input->parent].depth + 1;
        if (object->depth > BUILD_ANCESTRY_MAX) {
          return PFS_LIMIT;
        }
      }
      object->data_count = input->file_length / PFS_BLOCK_SIZE +
                            (input->file_length % PFS_BLOCK_SIZE != 0);
      state->has_data |= object->data_count != 0;
      state->object_order[first + j] = first + j;
      state->child_order[first + j] = first + j;
    }
    first += volume->object_count;
  }
  /* Reuse the child-order workspace to reject global ID collisions, then group
   * each volume's directory entries by parent and bytewise name. */
  sort_objects(state, false, state->child_order, state->object_count);
  for (size_t i = 0; i < state->object_count; ++i) {
    const struct pfs_object_id *id = &state->objects[state->child_order[i]].spec.id;
    if (!pfs_bytes_compare(id->bytes, PFS_ID_SIZE, spec->pool.bytes, PFS_ID_SIZE) ||
        (i && !object_compare(state, false,
                            state->child_order[i - 1], state->child_order[i]))) {
      return PFS_INVALID;
    }
    size_t low = 0;
    size_t high = spec->volume_count;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      int comparison = pfs_bytes_compare(id->bytes, PFS_ID_SIZE,
        state->specs[state->id_order[middle]].id.bytes, PFS_ID_SIZE);
      if (!comparison) {
        return PFS_INVALID;
      }
      if (comparison < 0) {
        high = middle;
      } else {
        low = middle + 1;
      }
    }
  }
  for (size_t i = 0; i < spec->volume_count; ++i) {
    first = state->layout[i].first;
    size_t count = state->specs[i].object_count;
    for (size_t j = 0; j < count; ++j) {
      state->child_order[first + j] = first + j;
    }
    sort_objects(state, false, state->object_order + first, count);
    sort_objects(state, true, state->child_order + first + 1, count - 1);
    for (size_t j = 1; j < count; ++j) {
      size_t index = state->child_order[first + j];
      struct build_object *parent =
        &state->objects[first + state->objects[index].spec.parent];
      if (j > 1 && !object_compare(state, true, state->child_order[first + j - 1], index)) {
        return PFS_INVALID;
      }
      if (!parent->child_count) {
        parent->child_first = first + j;
      }
      ++parent->child_count;
    }
  }
  return PFS_OK;
}

static void
leaf_minimum(const struct pfs_build_plan *plan, const struct build_state *state,
             const struct build_tree *tree, size_t index, struct pfs_key *key)
{
  if (tree->kind == PFS_INDEX_VOLUMES) {
    key->length = PFS_ID_SIZE;
    pfs_bytes_copy(key->bytes, state->volumes[state->id_order[index]].id.bytes, key->length);
  } else if (tree->kind == PFS_INDEX_VOLUME_NAMES) {
    const struct pfs_name *name = &state->volumes[state->name_order[index]].name;
    key->length = name->length;
    pfs_bytes_copy(key->bytes, name->bytes, key->length);
  } else if (tree->kind == PFS_INDEX_ALLOCATION) {
    uint64_t block = index == 0 ? 1 : index <= plan->volume_count ?
      state->layout[index - 1].first_block : plan->pool_root.live_pool + plan->pool_root.live_volume + 1;
    key->length = sizeof(block);
    pfs_put_u64(key->bytes, block);
  } else if (tree->kind == PFS_INDEX_OBJECTS) {
    key->length = PFS_ID_SIZE;
    pfs_bytes_copy(key->bytes, state->objects[state->object_order[index]].spec.id.bytes, key->length);
  } else if (tree->kind == PFS_INDEX_DIRECTORY) {
    const struct pfs_name *name = &state->objects[state->child_order[index]].spec.name;
    key->length = name->length;
    pfs_bytes_copy(key->bytes, name->bytes, key->length);
  } else {
    key->length = PFS_ID_SIZE * 2 + 1;
    pfs_bytes_copy(key->bytes, state->specs[tree->volume].root_object.bytes, PFS_ID_SIZE);
    pfs_bytes_copy(key->bytes + PFS_ID_SIZE, state->specs[tree->volume].owner.bytes, PFS_ID_SIZE);
    key->bytes[PFS_ID_SIZE * 2] = PFS_SCOPE_SUBTREE;
  }
}

static size_t
leaf_index(const struct build_state *state, const struct build_node *node)
{
  while (node->level) {
    node = &state->nodes[node->first];
  }
  return node->first;
}

static size_t
leaf_size(const struct build_state *state, const struct build_tree *tree, size_t index)
{
  switch (tree->kind) {
  case PFS_INDEX_VOLUMES:
    return PFS_VOLUME_RECORD_SIZE;
  case PFS_INDEX_VOLUME_NAMES:
    return aligned_size(PFS_NAME_RECORD_PREFIX_SIZE +
                          state->specs[state->name_order[index]].name.length);
  case PFS_INDEX_ALLOCATION:
    return PFS_ALLOCATION_RECORD_SIZE;
  case PFS_INDEX_OBJECTS:
    return PFS_OBJECT_RECORD_SIZE;
  case PFS_INDEX_DIRECTORY:
    return aligned_size(PFS_NAME_RECORD_PREFIX_SIZE +
                          state->objects[state->child_order[index]].spec.name.length);
  default:
    return PFS_GRANT_RECORD_SIZE;
  }
}

static size_t
internal_size(const struct build_state *state, const struct build_tree *tree, size_t child)
{
  size_t key_size = PFS_ID_SIZE;
  if (tree->kind == PFS_INDEX_ALLOCATION) {
    key_size = sizeof(uint64_t);
  } else if (tree->kind == PFS_INDEX_VOLUME_NAMES) {
    size_t index = leaf_index(state, &state->nodes[child]);
    key_size = state->specs[state->name_order[index]].name.length;
  } else if (tree->kind == PFS_INDEX_DIRECTORY) {
    size_t index = leaf_index(state, &state->nodes[child]);
    key_size = state->objects[state->child_order[index]].spec.name.length;
  } else if (tree->kind == PFS_INDEX_GRANTS) {
    key_size = PFS_ID_SIZE * 2 + 1;
  }
  return aligned_size(48 + key_size);
}

static enum pfs_status
repair_namespace_tail(struct build_state *state, const struct build_tree *tree,
                      size_t level_first)
{
  size_t level_count = state->node_count - level_first;
  struct build_node *tail = &state->nodes[state->node_count - 1];
  if (level_count == 1 || tail->count >= BUILD_NAMESPACE_MIN) {
    return PFS_OK;
  }
  struct build_node *left = tail - 1;
  size_t count = (size_t)left->count + tail->count;
  size_t bytes = 0;
  size_t prefix = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t item = left->first + i;
    bytes += left->level ? internal_size(state, tree, item) : leaf_size(state, tree, item);
    size_t start = aligned_size(PFS_TREE_HEADER_SIZE + (i + 1) * PFS_TREE_SLOT_SIZE);
    if (i + 1 <= PFS_TREE_SLOTS_MAX && start <= PFS_BLOCK_SIZE &&
        bytes <= PFS_BLOCK_SIZE - start) {
      prefix = i + 1;
    } else {
      break;
    }
  }
  if (prefix == count) {
    left->count = (uint16_t)count;
    --state->node_count;
    return PFS_OK;
  }

  /* Twelve maximum-size namespace items fit. A short greedy tail and its
   * predecessor therefore merge or admit two fitting groups of at least six. */
  if (prefix > count - BUILD_NAMESPACE_MIN) {
    prefix = count - BUILD_NAMESPACE_MIN;
  }
  if (prefix < BUILD_NAMESPACE_MIN) {
    return PFS_INVALID;
  }
  bytes = 0;
  for (size_t i = prefix; i < count; ++i) {
    size_t item = left->first + i;
    bytes += left->level ? internal_size(state, tree, item) : leaf_size(state, tree, item);
  }
  size_t suffix = count - prefix;
  size_t start = aligned_size(PFS_TREE_HEADER_SIZE + suffix * PFS_TREE_SLOT_SIZE);
  if (suffix > PFS_TREE_SLOTS_MAX || start > PFS_BLOCK_SIZE ||
      bytes > PFS_BLOCK_SIZE - start) {
    return PFS_INVALID;
  }
  left->count = (uint16_t)prefix;
  tail->first = left->first + prefix;
  tail->count = (uint16_t)suffix;
  return PFS_OK;
}

static enum pfs_status
plan_tree(struct build_state *state, uint16_t kind, size_t volume, size_t object,
          size_t first, size_t records, size_t *index)
{
  if (!records || state->tree_count == state->tree_capacity) {
    return PFS_LIMIT;
  }
  *index = state->tree_count++;
  struct build_tree *tree = &state->trees[*index];
  tree->kind = kind;
  tree->volume = volume;
  tree->object = object;
  tree->first_node = state->node_count;
  size_t end = first + records;
  uint16_t level = 0;
  for (;;) {
    size_t level_first = state->node_count;
    while (first < end) {
      if (state->node_count == state->node_capacity) {
        return PFS_LIMIT;
      }
      struct build_node *node = &state->nodes[state->node_count++];
      node->first = first;
      node->count = 0;
      node->level = level;
      size_t bytes = 0;
      while (first < end) {
        size_t length = level ? internal_size(state, tree, first) : leaf_size(state, tree, first);
        size_t count = node->count + 1;
        size_t start = aligned_size(PFS_TREE_HEADER_SIZE + count * PFS_TREE_SLOT_SIZE);
        if (count > PFS_TREE_SLOTS_MAX || start > PFS_BLOCK_SIZE || bytes > PFS_BLOCK_SIZE - start ||
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
    if (kind == PFS_INDEX_DIRECTORY) {
      enum pfs_status status = repair_namespace_tail(state, tree, level_first);
      if (status != PFS_OK) {
        return status;
      }
    }
    size_t level_count = state->node_count - level_first;
    if (level_count == 1) {
      if (level && state->nodes[level_first].count == 1) {
        --state->node_count;
      }
      tree->node_count = state->node_count - tree->first_node;
      return PFS_OK;
    }
    if (kind != PFS_INDEX_DIRECTORY && level && state->nodes[state->node_count - 1].count == 1) {
      if (level_count < 2 || state->nodes[state->node_count - 2].count < 3) {
        return PFS_INVALID;
      }
      --state->nodes[state->node_count - 2].count;
      --state->nodes[state->node_count - 1].first;
      ++state->nodes[state->node_count - 1].count;
    }
    if (++level >= PFS_TREE_DEPTH_MAX) {
      return PFS_LIMIT;
    }
    first = level_first;
    end = state->node_count;
  }
}

static uint64_t
default_reserve(uint64_t usable, uint64_t divisor, uint64_t floor)
{
  uint64_t value = usable / divisor + (usable % divisor != 0);
  return value < floor ? floor : value;
}

static enum pfs_status
plan_promises(const struct pfs_build_spec *spec, struct pfs_build_plan *plan,
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
    uint64_t unused = volume->guarantee > volume->live_blocks ?
                        volume->guarantee - volume->live_blocks : 0;
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
      volume->guarantee = volume->live_blocks + share + (remainder != 0);
      if (remainder) {
        --remainder;
      }
    }
    volume->quota = state->specs[i].quota_set ? state->specs[i].quota : default_quota;
    if (volume->quota < volume->live_blocks || volume->quota < volume->guarantee) {
      return PFS_LIMIT;
    }
  }
  plan->unpromised = remaining - shared;
  return PFS_OK;
}

static struct pfs_reference
tree_reference(const struct build_state *state, size_t index)
{
  const struct build_tree *tree = &state->trees[index];
  return reference(state->nodes[tree->first_node + tree->node_count - 1].block, PFS_BLOCK_TREE);
}

static enum pfs_status
plan_layout(const struct pfs_build_spec *spec, struct pfs_build_plan *plan,
            struct build_state *state)
{
  enum pfs_status status;
  size_t tree_index;
  for (size_t i = 0; i < BUILD_POOL_TREES; ++i) {
    status = plan_tree(state, (uint16_t)(PFS_INDEX_VOLUMES + i), SIZE_MAX, SIZE_MAX, 0,
                       plan->volume_count + (i == 2 ? 2 : 0), &tree_index);
    if (status != PFS_OK) {
      return status;
    }
  }
  uint64_t next_block = 2;
  for (size_t i = 0; i < state->node_count; ++i) {
    state->nodes[i].block = next_block++;
  }
  struct pfs_pool_root *root = &plan->pool_root;
  root->header = header(plan, 1, PFS_BLOCK_POOL);
  root->header.used = 288;
  root->volumes = tree_reference(state, 0);
  root->volume_names = tree_reference(state, 1);
  root->allocation = tree_reference(state, 2);
  root->volume_count = plan->volume_count;
  root->live_pool = next_block - 1;
  for (size_t i = 0; i < plan->volume_count; ++i) {
    struct build_volume *layout = &state->layout[i];
    layout->first_block = next_block;
    size_t first_node = state->node_count;
    size_t count = state->specs[i].object_count;
    status = plan_tree(state, PFS_INDEX_OBJECTS, i, SIZE_MAX, layout->first,
                       count, &layout->object_tree);
    if (status == PFS_OK) {
      status = plan_tree(state, PFS_INDEX_GRANTS, i, SIZE_MAX, 0, 1, &layout->grant_tree);
    }
    if (status != PFS_OK) {
      return status;
    }
    for (size_t j = 0; j < count; ++j) {
      struct build_object *object = &state->objects[layout->first + j];
      if (object->child_count) {
        status = plan_tree(state, PFS_INDEX_DIRECTORY, i, layout->first + j,
                           object->child_first, object->child_count, &object->tree);
        if (status != PFS_OK) {
          return status;
        }
      }
    }
    for (size_t j = first_node; j < state->node_count; ++j) {
      if (next_block >= plan->block_count - 1) {
        return PFS_LIMIT;
      }
      state->nodes[j].block = next_block++;
    }
    for (size_t j = 0; j < count; ++j) {
      struct build_object *object = &state->objects[layout->first + j];
      if (object->data_count > plan->block_count - 1 - next_block) {
        return PFS_LIMIT;
      }
      if (object->data_count) {
        object->data_first = next_block;
        next_block += object->data_count;
      }
    }
    struct pfs_volume_record *volume = &state->volumes[i];
    volume->object_root = tree_reference(state, layout->object_tree);
    volume->grant_root = tree_reference(state, layout->grant_tree);
    volume->live_blocks = next_block - layout->first_block;
    volume->object_count = count;
    root->live_volume += volume->live_blocks;
  }
  if (next_block >= plan->block_count - 1) {
    return PFS_LIMIT;
  }
  root->free = plan->block_count - 1 - next_block;
  return plan_promises(spec, plan, state);
}

enum pfs_status
pfs_build_plan_create(struct pfs_memory *memory, const struct pfs_build_spec *spec,
                       struct pfs_build_plan *plan)
{
  if (!memory || !plan || plan->state.owner || plan->state.data || plan->state.size ||
      plan->state.alignment || plan->volumes || plan->volume_count || plan->block_count) {
    return PFS_INVALID;
  }
  size_t objects;
  enum pfs_status status = validate_spec(spec, &objects);
  if (status != PFS_OK) {
    return status;
  }
  if (objects > (SIZE_MAX - spec->volume_count - BUILD_POOL_NODES) / 4 ||
      objects > SIZE_MAX - spec->volume_count * 2 - BUILD_POOL_TREES) {
    return PFS_LIMIT;
  }
  size_t tree_capacity = objects + spec->volume_count * 2 + BUILD_POOL_TREES;
  size_t node_capacity = objects * 4 + spec->volume_count + BUILD_POOL_NODES;
  size_t bytes = sizeof(struct build_state);
  if (!append_size(&bytes, objects, sizeof(struct build_object)) ||
      !append_size(&bytes, objects, sizeof(size_t)) ||
      !append_size(&bytes, objects, sizeof(size_t)) ||
      !append_size(&bytes, tree_capacity, sizeof(struct build_tree)) ||
      !append_size(&bytes, node_capacity, sizeof(struct build_node))) {
    return PFS_LIMIT;
  }
  status = pfs_memory_allocate(memory, bytes, _Alignof(struct build_state), &plan->state);
  if (status != PFS_OK) {
    return status;
  }
  struct build_state *state = plan->state.data;
  pfs_bytes_zero(state, bytes);
  size_t offset = sizeof(*state);
  state->objects = array_at(state, &offset, objects, sizeof(struct build_object));
  state->object_order = array_at(state, &offset, objects, sizeof(size_t));
  state->child_order = array_at(state, &offset, objects, sizeof(size_t));
  state->trees = array_at(state, &offset, tree_capacity, sizeof(struct build_tree));
  state->nodes = array_at(state, &offset, node_capacity, sizeof(struct build_node));
  state->object_count = objects;
  state->tree_capacity = tree_capacity;
  state->node_capacity = node_capacity;
  plan->block_count = spec->block_count;
  plan->pool = spec->pool;
  plan->volume_count = spec->volume_count;
  plan->volumes = state->volumes;
  pfs_bytes_copy(state->specs, spec->volumes, spec->volume_count * sizeof(*spec->volumes));
  for (size_t i = 0; i < plan->volume_count; ++i) {
    state->specs[i].objects = NULL;
    state->volumes[i].id = state->specs[i].id;
    pfs_bytes_copy(&state->volumes[i].name, &state->specs[i].name, sizeof(state->volumes[i].name));
    state->volumes[i].root_object = state->specs[i].root_object;
  }
  sort_volumes(state, PFS_INDEX_VOLUMES, state->id_order, plan->volume_count);
  sort_volumes(state, PFS_INDEX_VOLUME_NAMES, state->name_order, plan->volume_count);
  status = copy_objects(spec, state);
  if (status == PFS_OK) {
    status = plan_layout(spec, plan, state);
  }
  if (status != PFS_OK) {
    pfs_build_plan_destroy(plan);
  }
  return status;
}

static enum pfs_status
encode_leaf(const struct pfs_build_plan *plan, struct build_state *state,
            const struct build_tree *tree, size_t index, uint8_t *data,
            size_t capacity, size_t *written)
{
  struct pfs_record_context context = record_context(plan);
  if (tree->kind == PFS_INDEX_VOLUMES) {
    return pfs_volume_record_encode(data, capacity, &context,
                                    &state->volumes[state->id_order[index]], written);
  }
  if (tree->kind == PFS_INDEX_VOLUME_NAMES) {
    const struct pfs_volume_record *volume = &state->volumes[state->name_order[index]];
    struct pfs_volume_name_record record;
    pfs_bytes_zero(&record, sizeof(record));
    pfs_bytes_copy(&record.name, &volume->name, sizeof(record.name));
    record.volume = volume->id;
    return pfs_volume_name_record_encode(data, capacity, &context, &record, written);
  }
  if (tree->kind == PFS_INDEX_ALLOCATION) {
    struct pfs_allocation_record record = {0};
    if (!index) {
      record.first = 1;
      record.count = plan->pool_root.live_pool;
      record.state = PFS_ALLOCATION_POOL;
      record.birth = 1;
    } else if (index <= plan->volume_count) {
      const struct pfs_volume_record *volume = &state->volumes[index - 1];
      record.first = state->layout[index - 1].first_block;
      record.count = volume->live_blocks;
      record.state = PFS_ALLOCATION_VOLUME;
      record.owner = volume->id;
      record.birth = 1;
    } else {
      record.first = plan->pool_root.live_pool + plan->pool_root.live_volume + 1;
      record.count = plan->pool_root.free;
    }
    return pfs_allocation_record_encode(data, capacity, &context, &record, written);
  }
  if (tree->kind == PFS_INDEX_OBJECTS) {
    const struct build_object *source = &state->objects[state->object_order[index]];
    const struct build_volume *layout = &state->layout[tree->volume];
    struct pfs_object_record record = {
      .id = source->spec.id,
      .kind = source->spec.kind,
      .owner = state->specs[tree->volume].owner,
      .file_length = source->spec.file_length,
      .directory_count = source->child_count,
    };
    if (source->spec.parent != UINT32_MAX) {
      record.parent = state->objects[layout->first + source->spec.parent].spec.id;
    }
    if (source->child_count) {
      record.storage_kind = PFS_STORAGE_TREE;
      record.tree_root = tree_reference(state, source->tree);
    } else if (source->data_count) {
      record.storage_kind = PFS_STORAGE_INLINE;
      record.inline_extent.count = source->data_count;
      record.inline_extent.physical_first = source->data_first;
      record.inline_extent.birth = 1;
    }
    return pfs_object_record_encode(data, capacity, &context, &record, written);
  }
  if (tree->kind == PFS_INDEX_DIRECTORY) {
    const struct pfs_build_object *source = &state->objects[state->child_order[index]].spec;
    struct pfs_dirent_record record;
    pfs_bytes_zero(&record, sizeof(record));
    pfs_bytes_copy(&record.name, &source->name, sizeof(record.name));
    record.object = source->id;
    record.child_kind = source->kind;
    return pfs_dirent_record_encode(data, capacity, &context, &record, written);
  }
  struct pfs_grant_record grant = {
    .object = state->specs[tree->volume].root_object,
    .principal = state->specs[tree->volume].owner,
    .scope = PFS_SCOPE_SUBTREE,
    .file_rights = PFS_FILE_RIGHTS_ALL,
    .directory_rights = PFS_DIR_METADATA | PFS_DIR_LIST | PFS_DIR_LOOKUP |
                        PFS_DIR_CREATE | PFS_DIR_REMOVE | PFS_DIR_REPLACE,
    .admin_rights = PFS_ADMIN_RIGHTS_ALL,
  };
  return pfs_grant_record_encode(data, capacity, &context, &grant, written);
}

static enum pfs_status
write_tree_node(const struct pfs_build_plan *plan, struct build_state *state,
                const struct pfs_block_builder *builder, const struct build_tree *tree,
                const struct build_node *node)
{
  struct pfs_record_context record = record_context(plan);
  size_t offset = 0;
  for (size_t i = 0; i < node->count; ++i) {
    size_t written;
    enum pfs_status status;
    if (node->level) {
      const struct build_node *child = &state->nodes[node->first + i];
      struct pfs_internal_record internal;
      pfs_bytes_zero(&internal, sizeof(internal));
      internal.child = reference(child->block, PFS_BLOCK_TREE);
      leaf_minimum(plan, state, tree, leaf_index(state, child), &internal.minimum);
      status = pfs_internal_record_encode(state->records + offset,
        sizeof(state->records) - offset, tree->kind, &record, &internal, &written);
    } else {
      status = encode_leaf(plan, state, tree, node->first + i,
        state->records + offset, sizeof(state->records) - offset, &written);
    }
    if (status != PFS_OK) {
      return status;
    }
    state->encoded[i] = (struct pfs_encoded_record){state->records + offset, written};
    offset += written;
  }
  pfs_bytes_zero(&state->tree, sizeof(state->tree));
  state->tree.header = header(plan, node->block, PFS_BLOCK_TREE);
  state->tree.kind = tree->kind;
  state->tree.level = node->level;
  state->tree.count = node->count;
  if (tree->volume != SIZE_MAX) {
    state->tree.volume = state->specs[tree->volume].id;
  }
  if (tree->object != SIZE_MAX) {
    state->tree.object = state->objects[tree->object].spec.id;
  }
  struct pfs_tree_context context = {
    .block = {
      .block_count = plan->block_count,
      .selected_generation = 1,
      .referring_birth = 1,
      .pool = plan->pool,
      .reference = reference(node->block, PFS_BLOCK_TREE),
    },
    .kind = tree->kind,
    .volume = state->tree.volume,
    .object = state->tree.object,
  };
  enum pfs_status status = pfs_tree_encode(state->output, PFS_BLOCK_SIZE,
                                           &context, &state->tree, state->encoded);
  if (status != PFS_OK) {
    return status;
  }
  return pfs_block_write(builder, node->block, 1, state->output, PFS_BLOCK_SIZE);
}

static enum pfs_status
write_data(const struct pfs_build_plan *plan, struct build_state *state,
           const struct pfs_block_builder *builder, const struct pfs_build_source *source)
{
  uint32_t maximum = builder->reader.geometry.max_transfer_blocks;
  if (maximum > PFS_IO_BLOCKS_MAX) {
    maximum = PFS_IO_BLOCKS_MAX;
  }
  for (size_t i = 0; i < plan->volume_count; ++i) {
    for (size_t j = 0; j < state->specs[i].object_count; ++j) {
      const struct build_object *object = &state->objects[state->layout[i].first + j];
      uint64_t copied_blocks = 0;
      while (copied_blocks < object->data_count) {
        uint64_t remaining = object->data_count - copied_blocks;
        uint32_t blocks = remaining < maximum ? (uint32_t)remaining : maximum;
        size_t bytes = (size_t)blocks * PFS_BLOCK_SIZE;
        uint64_t offset = copied_blocks * PFS_BLOCK_SIZE;
        uint64_t available = object->spec.file_length - offset;
        size_t length = available < bytes ? (size_t)available : bytes;
        pfs_bytes_zero(state->output, bytes);
        enum pfs_status status = source->read(source->context, i, j, offset, state->output, length);
        if (status != PFS_OK) {
          return status;
        }
        status = pfs_block_write(builder, object->data_first + copied_blocks,
                                   blocks, state->output, bytes);
        if (status != PFS_OK) {
          return status;
        }
        copied_blocks += blocks;
      }
    }
  }
  return PFS_OK;
}

enum pfs_status
pfs_build(const struct pfs_build_plan *plan, const struct pfs_block_builder *builder,
          const struct pfs_build_source *source)
{
  if (!plan || !plan->state.data || !plan->state.owner ||
      plan->state.size < sizeof(struct build_state) || !builder ||
      !builder->reader.read || !builder->write || !builder->flush ||
      builder->reader.geometry.block_count != plan->block_count ||
      !builder->reader.geometry.max_transfer_blocks ||
      (source && (!source->read || !source->validate))) {
    return PFS_INVALID;
  }
  struct build_state *state = plan->state.data;
  if (state->has_data && !source) {
    return PFS_INVALID;
  }
  enum pfs_status status;
  for (size_t i = 0; i < state->tree_count; ++i) {
    const struct build_tree *tree = &state->trees[i];
    for (size_t j = 0; j < tree->node_count; ++j) {
      status = write_tree_node(plan, state, builder, tree, &state->nodes[tree->first_node + j]);
      if (status != PFS_OK) {
        return status;
      }
    }
  }
  status = write_data(plan, state, builder, source);
  if (status == PFS_OK && source) {
    status = source->validate(source->context);
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
  status = pfs_pool_root_encode(state->output, PFS_BLOCK_SIZE, &context, &plan->pool_root);
  if (status == PFS_OK) {
    status = pfs_block_write(builder, 1, 1, state->output, PFS_BLOCK_SIZE);
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
    status = pfs_superblock_encode(state->output, PFS_BLOCK_SIZE, &super);
    if (status == PFS_OK) {
      status = pfs_block_write(builder, super.header.block, 1, state->output, PFS_BLOCK_SIZE);
    }
    if (status != PFS_OK) {
      return status;
    }
  }
  return pfs_block_flush(builder);
}

enum pfs_status
pfs_build_plan_destroy(struct pfs_build_plan *plan)
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
