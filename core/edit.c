/* SPDX-License-Identifier: MPL-2.0 */
#include "edit.h"
#include "canonical.h"
#include "internal.h"

#include <pyxis_fs/record.h>

struct edit_result {
  uint16_t count;
  struct pfs_reference children[2];
  struct pfs_key minima[2];
};

static bool
same_reference(const struct pfs_reference *a, const struct pfs_reference *b)
{
  return a->block == b->block && a->birth == b->birth &&
         a->type == b->type && a->version == b->version;
}

static struct pfs_record_context
record_context(const struct pfs_edit_candidate *candidate, uint64_t birth)
{
  return (struct pfs_record_context) {
    .block_count = candidate->context.block.block_count,
    .selected_generation = candidate->birth,
    .containing_birth = birth,
    .features = candidate->context.block.features,
  };
}

static enum pfs_status
read_node(const struct pfs_edit_candidate *candidate,
          const struct pfs_reference *reference, uint16_t parent_level,
          uint8_t *data, struct pfs_tree *tree)
{
  bool found = false;
  if (reference->birth == candidate->birth) {
    for (size_t i = 0; i < candidate->slot_capacity; ++i) {
      if (candidate->slots[i].active && candidate->slots[i].block == reference->block) {
        pfs_bytes_copy(data, candidate->slots[i].data, PFS_BLOCK_SIZE);
        found = true;
        break;
      }
    }
    if (!found) {
      return PFS_INVALID;
    }
  } else {
    enum pfs_status status = pfs_block_read(candidate->reader, reference->block, 1,
                                            data, PFS_BLOCK_SIZE);
    if (status != PFS_OK) {
      return status;
    }
  }
  struct pfs_tree_context context = candidate->context;
  context.block.reference = *reference;
  context.block.referring_birth = candidate->birth;
  context.block.selected_generation = candidate->birth;
  context.parent_level = parent_level;
  enum pfs_status status = pfs_canonical_tree_validate(data, PFS_BLOCK_SIZE, &context, tree);
  if (status == PFS_OK && parent_level &&
      (tree->kind == PFS_INDEX_DIRECTORY || tree->kind == PFS_INDEX_ORPHANS) &&
      tree->count < 6) {
    return PFS_LIMIT;
  }
  return status;
}

static enum pfs_status
record_key(const struct pfs_edit_candidate *candidate,
           const struct pfs_encoded_record *record, uint16_t level,
           uint64_t birth, struct pfs_key *key)
{
  struct pfs_record_context context = record_context(candidate, birth);
  if (level) {
    struct pfs_internal_record value;
    enum pfs_status status = pfs_internal_record_decode(record->data, record->length,
                                                        candidate->context.kind, &context, &value);
    if (status == PFS_OK) {
      *key = value.minimum;
    }
    return status;
  }
  uint64_t end;
  return pfs_leaf_key_decode(record->data, record->length, candidate->context.kind,
                             &context, key, &end);
}

static void
node_records(const uint8_t *data, const struct pfs_tree *tree,
             struct pfs_encoded_record *records)
{
  for (uint16_t i = 0; i < tree->count; ++i) {
    records[i] = (struct pfs_encoded_record) {
      .data = data + tree->slots[i].offset,
      .length = tree->slots[i].length,
    };
  }
}

static bool
records_fit(const struct pfs_encoded_record *records, uint16_t count)
{
  if (!count || count > PFS_TREE_SLOTS_MAX) {
    return false;
  }
  size_t used = PFS_TREE_HEADER_SIZE + count * PFS_TREE_SLOT_SIZE + 4 * (count % 2);
  for (uint16_t i = 0; i < count; ++i) {
    if (records[i].length > PFS_BLOCK_SIZE - used) {
      return false;
    }
    used += records[i].length;
  }
  return true;
}

static enum pfs_status
consume(struct pfs_edit_workspace *workspace, const struct pfs_reference *reference)
{
  for (size_t i = 0; i < workspace->consumed_count; ++i) {
    if (same_reference(reference, &workspace->consumed[i])) {
      return PFS_INVALID;
    }
  }
  if (workspace->consumed_count == PFS_EDIT_NODES_MAX) {
    return PFS_LIMIT;
  }
  workspace->consumed[workspace->consumed_count++] = *reference;
  return PFS_OK;
}

static bool
slot_available(const struct pfs_edit_candidate *candidate,
               const struct pfs_edit_workspace *workspace, size_t slot)
{
  for (size_t i = 0; i < workspace->output_count; ++i) {
    if (workspace->outputs[i].slot == slot) {
      return false;
    }
  }
  if (!candidate->slots[slot].active) {
    return true;
  }
  for (size_t i = 0; i < workspace->consumed_count; ++i) {
    if (workspace->consumed[i].birth == candidate->birth &&
        workspace->consumed[i].block == candidate->slots[slot].block) {
      return true;
    }
  }
  return false;
}

static enum pfs_status
emit(struct pfs_edit_candidate *candidate, struct pfs_edit_workspace *workspace,
     uint16_t level, const struct pfs_encoded_record *records, uint16_t count,
     struct pfs_reference *reference, struct pfs_key *minimum)
{
  if (workspace->output_count == PFS_EDIT_NODES_MAX) {
    return PFS_LIMIT;
  }
  size_t slot = 0;
  while (slot < candidate->slot_capacity && !slot_available(candidate, workspace, slot)) {
    ++slot;
  }
  if (slot == candidate->slot_capacity) {
    return PFS_LIMIT;
  }
  struct pfs_tree_context context = candidate->context;
  context.block.reference = (struct pfs_reference) {
    .block = candidate->slots[slot].block,
    .birth = candidate->birth,
    .type = PFS_BLOCK_TREE,
    .version = PFS_FORMAT_VERSION,
  };
  context.block.selected_generation = candidate->birth;
  context.block.referring_birth = candidate->birth;
  context.parent_level = 0;
  struct pfs_tree tree = {
    .header = {
      .type = PFS_BLOCK_TREE,
      .version = PFS_FORMAT_VERSION,
      .pool = context.block.pool,
      .block = context.block.reference.block,
      .birth = candidate->birth,
    },
    .kind = context.kind,
    .level = level,
    .count = count,
    .volume = context.volume,
    .object = context.object,
  };
  struct pfs_edit_output *output = &workspace->outputs[workspace->output_count];
  enum pfs_status status = pfs_tree_encode(output->data, PFS_BLOCK_SIZE, &context, &tree, records);
  if (status == PFS_OK) {
    status = record_key(candidate, records, level, candidate->birth, minimum);
  }
  if (status == PFS_OK) {
    output->slot = slot;
    *reference = context.block.reference;
    ++workspace->output_count;
  }
  return status;
}

static enum pfs_status
partition(struct pfs_edit_candidate *candidate, struct pfs_edit_workspace *workspace,
          uint16_t level, const struct pfs_encoded_record *records, uint16_t count,
          uint16_t minimum_count, bool force_two, struct edit_result *result)
{
  result->count = 0;
  if (!count) {
    return PFS_OK;
  }
  if (!force_two && records_fit(records, count)) {
    enum pfs_status status = emit(candidate, workspace, level, records, count,
                                  &result->children[0], &result->minima[0]);
    if (status == PFS_OK) {
      result->count = 1;
    }
    return status;
  }
  uint16_t cut = 0;
  for (uint16_t i = minimum_count; i + minimum_count <= count; ++i) {
    if (records_fit(records, i) && records_fit(records + i, count - i)) {
      cut = i;
    }
  }
  if (!cut) {
    return PFS_LIMIT;
  }
  enum pfs_status status = emit(candidate, workspace, level, records, cut,
                                &result->children[0], &result->minima[0]);
  if (status == PFS_OK) {
    status = emit(candidate, workspace, level, records + cut, count - cut,
                   &result->children[1], &result->minima[1]);
  }
  if (status == PFS_OK) {
    result->count = 2;
  }
  return status;
}

static enum pfs_status
encode_children(const struct pfs_edit_candidate *candidate,
                struct pfs_edit_workspace *workspace, const struct edit_result *result,
                struct pfs_encoded_record *records)
{
  struct pfs_record_context context = record_context(candidate, candidate->birth);
  for (uint16_t i = 0; i < result->count; ++i) {
    struct pfs_internal_record record = {
      .child = result->children[i],
      .minimum = result->minima[i],
    };
    records[i].data = workspace->internal[i];
    enum pfs_status status = pfs_internal_record_encode(workspace->internal[i],
                                                        PFS_EDIT_INTERNAL_BYTES,
                                                        candidate->context.kind,
                                                        &context, &record, &records[i].length);
    if (status != PFS_OK) {
      return status;
    }
  }
  return PFS_OK;
}

static enum pfs_status
commit(struct pfs_edit_candidate *candidate, struct pfs_edit_workspace *workspace,
       const struct pfs_reference *root, enum pfs_edit_operation operation, bool namespace)
{
  size_t published = 0;
  size_t new_limit = operation == PFS_EDIT_UPDATE ? 8 :
                     operation == PFS_EDIT_DELETE && !namespace ? 14 : 15;
  size_t old_limit = operation == PFS_EDIT_DELETE ? (namespace ? 15 : 14) : 8;
  for (size_t i = 0; i < workspace->consumed_count; ++i) {
    published += workspace->consumed[i].birth != candidate->birth;
  }
  if (workspace->output_count > new_limit || workspace->consumed_count > old_limit ||
      published > candidate->retired_capacity - candidate->retired_count) {
    return PFS_LIMIT;
  }
  for (size_t i = 0; i < workspace->consumed_count; ++i) {
    const struct pfs_reference *reference = &workspace->consumed[i];
    if (reference->birth != candidate->birth) {
      candidate->retired[candidate->retired_count++] = *reference;
    } else {
      for (size_t j = 0; j < candidate->slot_capacity; ++j) {
        if (candidate->slots[j].block == reference->block) {
          candidate->slots[j].active = false;
          break;
        }
      }
    }
  }
  for (size_t i = 0; i < workspace->output_count; ++i) {
    struct pfs_edit_slot *slot = &candidate->slots[workspace->outputs[i].slot];
    pfs_bytes_copy(slot->data, workspace->outputs[i].data, PFS_BLOCK_SIZE);
    slot->active = true;
  }
  candidate->root = *root;
  return PFS_OK;
}

enum pfs_status
pfs_edit_tree(struct pfs_edit_candidate *candidate, struct pfs_edit_workspace *workspace,
              enum pfs_edit_operation operation, const struct pfs_key *key,
              const struct pfs_encoded_record *record)
{
  if (!candidate || !workspace || !key || !candidate->reader || !candidate->slots ||
      !candidate->birth || candidate->root.birth > candidate->birth ||
      candidate->retired_count > candidate->retired_capacity ||
      (candidate->retired_capacity && !candidate->retired) ||
      (operation != PFS_EDIT_INSERT && operation != PFS_EDIT_UPDATE &&
       operation != PFS_EDIT_DELETE) ||
      (operation != PFS_EDIT_DELETE && (!record || !record->data))) {
    return PFS_INVALID;
  }
  uint16_t kind = candidate->context.kind;
  bool namespace = kind == PFS_INDEX_DIRECTORY || kind == PFS_INDEX_ORPHANS;
  if (kind != PFS_INDEX_OBJECTS && kind != PFS_INDEX_EXTENTS && kind != PFS_INDEX_GRANTS &&
      !namespace && !(kind == PFS_INDEX_VOLUMES && operation == PFS_EDIT_UPDATE)) {
    return PFS_UNSUPPORTED;
  }
  enum pfs_status status = pfs_key_validate(kind, key);
  if (status != PFS_OK) {
    return status == PFS_CORRUPT ? PFS_INVALID : status;
  }
  uint64_t extent_end = 0;
  if (operation != PFS_EDIT_DELETE) {
    struct pfs_record_context context = record_context(candidate, candidate->birth);
    status = pfs_canonical_record_validate(record->data, record->length, kind, 0, &context);
    struct pfs_key encoded_key;
    if (status == PFS_OK) {
      status = record_key(candidate, record, 0, candidate->birth, &encoded_key);
    }
    if (status != PFS_OK || pfs_key_compare(kind, key, &encoded_key)) {
      return status == PFS_OK || status == PFS_CORRUPT ? PFS_INVALID : status;
    }
    if (kind == PFS_INDEX_EXTENTS) {
      struct pfs_extent_record extent;
      status = pfs_extent_record_decode(record->data, record->length, &context, &extent);
      if (status != PFS_OK) {
        return status;
      }
      extent_end = extent.mapping.logical_first + extent.mapping.count;
    }
  }
  workspace->consumed_count = 0;
  workspace->output_count = 0;
  size_t depth = 0;
  struct pfs_reference reference = candidate->root;
  uint16_t parent_level = 0;
  bool has_extent_upper = false;
  uint64_t extent_upper = 0;
  if (reference.block) {
    while (true) {
      if (depth == PFS_TREE_DEPTH_MAX) {
        return PFS_LIMIT;
      }
      struct pfs_edit_path *path = &workspace->path[depth++];
      status = read_node(candidate, &reference, parent_level, path->data, &path->tree);
      if (status != PFS_OK) {
        return status;
      }
      if (!path->tree.level) {
        break;
      }
      struct pfs_record_context context = record_context(candidate, path->tree.header.birth);
      path->child = 0;
      struct pfs_internal_record child;
      for (uint16_t i = 0; i < path->tree.count; ++i) {
        status = pfs_internal_record_decode(path->data + path->tree.slots[i].offset,
                                             path->tree.slots[i].length, kind, &context, &child);
        if (status != PFS_OK) {
          return status;
        }
        if (i && pfs_key_compare(kind, &child.minimum, key) > 0) {
          if (kind == PFS_INDEX_EXTENTS) {
            extent_upper = pfs_get_u64(child.minimum.bytes);
            has_extent_upper = true;
          }
          break;
        }
        path->child = i;
        reference = child.child;
      }
      parent_level = path->tree.level;
    }
  } else {
    if (operation != PFS_EDIT_INSERT) {
      return PFS_NOT_FOUND;
    }
    depth = 1;
    pfs_bytes_zero(&workspace->path[0].tree, sizeof(workspace->path[0].tree));
  }
  if (operation != PFS_EDIT_DELETE && has_extent_upper && extent_end > extent_upper) {
    return PFS_INVALID;
  }
  struct pfs_edit_path *leaf = &workspace->path[depth - 1];
  unsigned sequence_index = 0;
  struct pfs_encoded_record *sequence = workspace->records[sequence_index];
  node_records(leaf->data, &leaf->tree, sequence);
  uint16_t count = leaf->tree.count;
  uint16_t position = 0;
  bool exists = false;
  for (; position < count; ++position) {
    struct pfs_key current;
    status = record_key(candidate, &sequence[position], 0, leaf->tree.header.birth, &current);
    if (status != PFS_OK) {
      return status;
    }
    int order = pfs_key_compare(kind, &current, key);
    if (order >= 0) {
      exists = !order;
      break;
    }
  }
  if (operation == PFS_EDIT_INSERT && exists) {
    return PFS_INVALID;
  }
  if (operation != PFS_EDIT_INSERT && !exists) {
    return PFS_NOT_FOUND;
  }
  if (operation == PFS_EDIT_UPDATE) {
    if (sequence[position].length != record->length) {
      return PFS_INVALID;
    }
    sequence[position] = *record;
  } else if (operation == PFS_EDIT_INSERT) {
    for (uint16_t i = count; i > position; --i) {
      sequence[i] = sequence[i - 1];
    }
    sequence[position] = *record;
    ++count;
  } else {
    --count;
    for (uint16_t i = position; i < count; ++i) {
      sequence[i] = sequence[i + 1];
    }
  }

  for (size_t remaining = depth; remaining; --remaining) {
    size_t path_index = remaining - 1;
    struct pfs_edit_path *path = &workspace->path[path_index];
    uint16_t level = path->tree.level;
    if (path->tree.header.block) {
      struct pfs_reference old = {
        .block = path->tree.header.block,
        .birth = path->tree.header.birth,
        .type = PFS_BLOCK_TREE,
        .version = PFS_FORMAT_VERSION,
      };
      status = consume(workspace, &old);
      if (status != PFS_OK) {
        return status;
      }
    }
    uint16_t replace_first = path_index ? workspace->path[path_index - 1].child : 0;
    uint16_t replace_count = 1;
    uint16_t minimum_count = namespace ? 6 : level ? 2 : 1;
    bool force_two = false;
    if (path_index && count < minimum_count && (namespace || (level && count))) {
      struct pfs_edit_path *parent = &workspace->path[path_index - 1];
      uint16_t sibling_index = replace_first ? replace_first - 1 : replace_first + 1;
      struct pfs_record_context context = record_context(candidate, parent->tree.header.birth);
      struct pfs_internal_record sibling_reference;
      status = pfs_internal_record_decode(parent->data + parent->tree.slots[sibling_index].offset,
                                           parent->tree.slots[sibling_index].length,
                                           kind, &context, &sibling_reference);
      struct pfs_tree sibling;
      if (status == PFS_OK) {
        status = read_node(candidate, &sibling_reference.child, parent->tree.level,
                            workspace->sibling, &sibling);
      }
      if (status != PFS_OK) {
        return status;
      }
      status = consume(workspace, &sibling_reference.child);
      if (status != PFS_OK) {
        return status;
      }
      struct pfs_encoded_record *combined = workspace->records[sequence_index ^ 1];
      if (sibling_index < replace_first) {
        node_records(workspace->sibling, &sibling, combined);
        for (uint16_t i = 0; i < count; ++i) {
          combined[sibling.count + i] = sequence[i];
        }
        replace_first = sibling_index;
      } else {
        for (uint16_t i = 0; i < count; ++i) {
          combined[i] = sequence[i];
        }
        node_records(workspace->sibling, &sibling, combined + count);
      }
      count += sibling.count;
      replace_count = 2;
      force_two = !namespace && sibling.count > 2;
      sequence_index ^= 1;
      sequence = combined;
    }
    struct edit_result result;
    if (!path_index && level && count == 1) {
      struct pfs_record_context context = record_context(candidate, candidate->birth);
      struct pfs_internal_record child;
      status = pfs_internal_record_decode(sequence[0].data, sequence[0].length,
                                           kind, &context, &child);
      if (status != PFS_OK) {
        return status;
      }
      return commit(candidate, workspace, &child.child, operation, namespace);
    }
    status = partition(candidate, workspace, level, sequence, count,
                        minimum_count, force_two, &result);
    if (status != PFS_OK) {
      return status;
    }
    if (!path_index) {
      struct pfs_reference root = {0};
      if (result.count == 1) {
        root = result.children[0];
      } else if (result.count == 2) {
        if (level + 1u >= PFS_TREE_DEPTH_MAX) {
          return PFS_LIMIT;
        }
        status = encode_children(candidate, workspace, &result, sequence);
        struct pfs_key minimum;
        if (status == PFS_OK) {
          status = emit(candidate, workspace, level + 1, sequence, 2, &root, &minimum);
        }
        if (status != PFS_OK) {
          return status;
        }
      }
      return commit(candidate, workspace, &root, operation, namespace);
    }
    struct pfs_edit_path *parent = &workspace->path[path_index - 1];
    struct pfs_encoded_record *next = workspace->records[sequence_index ^ 1];
    uint16_t next_count = 0;
    for (uint16_t i = 0; i < replace_first; ++i) {
      next[next_count++] = (struct pfs_encoded_record) {
        parent->data + parent->tree.slots[i].offset, parent->tree.slots[i].length,
      };
    }
    status = encode_children(candidate, workspace, &result, next + next_count);
    if (status != PFS_OK) {
      return status;
    }
    next_count += result.count;
    for (uint16_t i = replace_first + replace_count; i < parent->tree.count; ++i) {
      next[next_count++] = (struct pfs_encoded_record) {
        parent->data + parent->tree.slots[i].offset, parent->tree.slots[i].length,
      };
    }
    sequence_index ^= 1;
    sequence = next;
    count = next_count;
  }
  return PFS_INVALID;
}
