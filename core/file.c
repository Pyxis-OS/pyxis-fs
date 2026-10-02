/* SPDX-License-Identifier: MPL-2.0 */
#include "file.h"
#include "canonical.h"

#define MUTATION_SCRATCH_OFFSET (3 * PFS_PLAN_SCRATCH_BYTES / 4)
#define MUTATION_MAPPINGS_MAX (2 * PFS_PLAN_VOLUME_RETIRED)
#define SHRINK_MAPPINGS_MAX 6u
#define SHRINK_DATA_MAX 128u

struct mutation_state {
  struct pfs_edit_slot slots[PFS_PLAN_VOLUME_NEW];
  bool data_slot[PFS_PLAN_VOLUME_NEW];
  struct check_claim data_claim[PFS_PLAN_VOLUME_NEW];
  struct pfs_reference retired[PFS_PLAN_VOLUME_RETIRED];
  size_t retired_count;
  struct check_claim data_removed[PFS_PLAN_VOLUME_RETIRED];
  size_t data_removed_count;
  struct pfs_extent_mapping mappings[MUTATION_MAPPINGS_MAX];
  size_t mapping_count;
  uint64_t replaced[MUTATION_MAPPINGS_MAX];
  size_t replaced_count;
  uint64_t extent_count;
  bool orphan_object;
  struct pfs_admit_volume volume;
};

struct pfs_mutation {
  struct pfs_pool *pool;
  struct pfs_batch *batch;
  const struct pfs_admit_state *source;
  struct mutation_state state;
  struct mutation_state saved;
  struct pfs_edit_workspace editor;
  struct pfs_batch_block blocks[PFS_PLAN_VOLUME_NEW];
  struct check_claim add[PFS_PLAN_VOLUME_NEW];
  struct check_claim remove[PFS_PLAN_VOLUME_RETIRED];
  uint8_t read_block[PFS_BLOCK_SIZE];
  struct pfs_tree read_tree;
};

_Static_assert(sizeof(struct pfs_mutation) <= PFS_PLAN_SCRATCH_BYTES / 4,
  "file staging must fit the reserved final quarter of scratch");

static const struct check_claim *source_claim(const struct pfs_mutation *mutation,
  uint64_t block);

static bool
same_id(const void *left, const void *right)
{
  return !pfs_bytes_compare(left, PFS_ID_SIZE, right, PFS_ID_SIZE);
}

static struct pfs_record_context
record_context(const struct pfs_mutation *mutation)
{
  return (struct pfs_record_context){
    .block_count = mutation->pool->reader->geometry.block_count,
    .selected_generation = mutation->batch->generation,
    .containing_birth = mutation->batch->generation,
    .features = mutation->state.volume.record.features,
  };
}

static enum pfs_status
mutation_begin(struct pfs_pool *pool, const struct pfs_volume_id *id,
               struct pfs_batch *batch, struct pfs_mutation **out)
{
  struct pfs_writer *writer = pool->writer;
  const struct pfs_admit_state *source = &writer->states[writer->selected];
  size_t index = 0;
  while (index < source->volume_count &&
         !same_id(source->volumes[index].record.id.bytes, id->bytes)) {
    index++;
  }
  if (index == source->volume_count) {
    return PFS_NOT_FOUND;
  }
  struct pfs_mutation *mutation = (void *)(writer->arena.scratch + MUTATION_SCRATCH_OFFSET);
  pfs_bytes_zero(mutation, sizeof(*mutation));
  mutation->pool = pool;
  mutation->batch = batch;
  mutation->source = source;
  mutation->state.volume = source->volumes[index];
  for (size_t i = 0; i < PFS_PLAN_VOLUME_NEW; i++) {
    mutation->state.slots[i].block = batch->available[PFS_PLAN_VOLUME_NEW - 1 - i];
  }
  *out = mutation;
  return PFS_OK;
}

enum pfs_status
pfs_mutation_begin(struct pfs_volume *volume, struct pfs_batch *batch,
                   struct pfs_mutation **out)
{
  if (!volume || !volume->pool || !volume->pool->writer || !batch || !out ||
      !volume->pool->writer->prepared || batch != &volume->pool->writer->batch) {
    return PFS_INVALID;
  }
  struct pfs_volume_record record;
  enum pfs_status status = pfs_volume_metadata(volume, &record);
  return status == PFS_OK ? mutation_begin(volume->pool, &record.id, batch, out) : status;
}

static bool
file_claim(const struct pfs_mutation *mutation, const struct check_claim *claim,
           const struct pfs_object_id *id)
{
  return !claim->type && same_id(claim->volume.bytes, mutation->state.volume.record.id.bytes) &&
    same_id(claim->object.bytes, id->bytes);
}

static bool
mapping_replaced(const struct mutation_state *state, uint64_t logical)
{
  for (size_t i = 0; i < state->replaced_count; i++) {
    if (state->replaced[i] == logical) {
      return true;
    }
  }
  return false;
}

/* Source claims describe exactly the admitted mapping records. Only mappings
 * edited in this batch enter the small overlay; no whole-file vector is needed. */
static bool
mapping_find(const struct pfs_mutation *mutation, const struct pfs_object_id *id,
             uint64_t logical, bool high, struct pfs_extent_mapping *out)
{
  bool found = false;
  for (size_t i = 0; i < mutation->source->claim_count; i++) {
    const struct check_claim *claim = &mutation->source->claims[i];
    if (!file_claim(mutation, claim, id) || mapping_replaced(&mutation->state, claim->logical)) {
      continue;
    }
    bool match = high || (logical >= claim->logical && logical - claim->logical < claim->count);
    if (match && (!found || claim->logical > out->logical_first)) {
      *out = (struct pfs_extent_mapping){claim->logical, claim->count, claim->first, claim->birth};
      found = true;
    }
  }
  for (size_t i = 0; i < mutation->state.mapping_count; i++) {
    const struct pfs_extent_mapping *mapping = &mutation->state.mappings[i];
    bool match = high || (logical >= mapping->logical_first && logical - mapping->logical_first < mapping->count);
    if (match && (!found || mapping->logical_first > out->logical_first)) {
      *out = *mapping;
      found = true;
    }
  }
  return found;
}

static enum pfs_status
extent_tree_edit(struct pfs_mutation *mutation, struct pfs_object_record *object,
                 enum pfs_edit_operation operation, const struct pfs_extent_mapping *mapping)
{
  uint8_t bytes[PFS_EXTENT_RECORD_SIZE];
  size_t written = 0;
  struct pfs_record_context context = record_context(mutation);
  struct pfs_extent_record extent = {.mapping = *mapping};
  enum pfs_status status = PFS_OK;
  if (operation != PFS_EDIT_DELETE) {
    status = pfs_extent_record_encode(bytes, sizeof(bytes), &context, &extent, &written);
  }
  struct pfs_key key = {.length = 8};
  pfs_put_u64(key.bytes, mapping->logical_first);
  struct pfs_encoded_record encoded = {bytes, written};
  if (status == PFS_OK) {
    status = pfs_mutation_tree(mutation, PFS_INDEX_EXTENTS, &object->id, &object->tree_root,
      operation, &key, operation == PFS_EDIT_DELETE ? NULL : &encoded);
  }
  return status;
}

static enum pfs_status
mapping_overlay_remove(struct pfs_mutation *mutation,
                       const struct pfs_extent_mapping *mapping)
{
  struct mutation_state *state = &mutation->state;
  for (size_t i = 0; i < state->mapping_count; i++) {
    if (state->mappings[i].logical_first == mapping->logical_first) {
      state->mappings[i] = state->mappings[--state->mapping_count];
      state->extent_count--;
      return PFS_OK;
    }
  }
  if (state->replaced_count == MUTATION_MAPPINGS_MAX) {
    return PFS_LIMIT;
  }
  state->replaced[state->replaced_count++] = mapping->logical_first;
  state->extent_count--;
  return PFS_OK;
}

static enum pfs_status
mapping_delete(struct pfs_mutation *mutation, struct pfs_object_record *object,
               const struct pfs_extent_mapping *mapping)
{
  enum pfs_status status = PFS_OK;
  if (object->storage_kind == PFS_STORAGE_TREE) {
    status = extent_tree_edit(mutation, object, PFS_EDIT_DELETE, mapping);
  } else {
    object->storage_kind = PFS_STORAGE_NONE;
    object->inline_extent = (struct pfs_extent_mapping){0};
  }
  return status == PFS_OK ? mapping_overlay_remove(mutation, mapping) : status;
}

static enum pfs_status
mapping_trim(struct pfs_mutation *mutation, struct pfs_object_record *object,
             const struct pfs_extent_mapping *mapping)
{
  if (mutation->state.mapping_count == MUTATION_MAPPINGS_MAX) {
    return PFS_LIMIT;
  }
  enum pfs_status status = PFS_OK;
  if (object->storage_kind == PFS_STORAGE_TREE) {
    status = extent_tree_edit(mutation, object, PFS_EDIT_UPDATE, mapping);
  } else {
    object->inline_extent = *mapping;
  }
  if (status == PFS_OK) {
    status = mapping_overlay_remove(mutation, mapping);
  }
  if (status == PFS_OK) {
    mutation->state.mappings[mutation->state.mapping_count++] = *mapping;
    mutation->state.extent_count++;
  }
  return status;
}

static enum pfs_status
mapping_insert(struct pfs_mutation *mutation, struct pfs_object_record *object,
               const struct pfs_extent_mapping *mapping)
{
  if (mutation->state.mapping_count == MUTATION_MAPPINGS_MAX) {
    return PFS_LIMIT;
  }
  enum pfs_status status = PFS_OK;
  if (object->storage_kind == PFS_STORAGE_NONE) {
    object->storage_kind = PFS_STORAGE_INLINE;
    object->inline_extent = *mapping;
  } else {
    if (object->storage_kind == PFS_STORAGE_INLINE) {
      struct pfs_extent_mapping previous = object->inline_extent;
      object->tree_root = (struct pfs_reference){0};
      status = extent_tree_edit(mutation, object, PFS_EDIT_INSERT, &previous);
      if (status == PFS_OK) {
        object->storage_kind = PFS_STORAGE_TREE;
        object->inline_extent = (struct pfs_extent_mapping){0};
      }
    }
    if (status == PFS_OK) {
      status = extent_tree_edit(mutation, object, PFS_EDIT_INSERT, mapping);
    }
  }
  if (status == PFS_OK) {
    mutation->state.mappings[mutation->state.mapping_count++] = *mapping;
    mutation->state.extent_count++;
  }
  return status;
}

static enum pfs_status
mapping_reduce(struct pfs_mutation *mutation, struct pfs_object_record *object)
{
  if (object->storage_kind != PFS_STORAGE_TREE || mutation->state.extent_count > 1) {
    return PFS_OK;
  }
  if (!mutation->state.extent_count) {
    if (object->tree_root.block) {
      return PFS_CORRUPT;
    }
    object->storage_kind = PFS_STORAGE_NONE;
    return PFS_OK;
  }
  struct pfs_extent_mapping mapping;
  if (!mapping_find(mutation, &object->id, 0, true, &mapping)) {
    return PFS_CORRUPT;
  }
  enum pfs_status status = extent_tree_edit(mutation, object, PFS_EDIT_DELETE, &mapping);
  if (status == PFS_OK) {
    object->storage_kind = PFS_STORAGE_INLINE;
    object->inline_extent = mapping;
    object->tree_root = (struct pfs_reference){0};
  }
  return status;
}

static bool
mapping_compatible(const struct pfs_extent_mapping *left, const struct pfs_extent_mapping *right)
{
  return left->birth == right->birth && left->logical_first + left->count == right->logical_first &&
    left->physical_first + left->count == right->physical_first;
}

static enum pfs_status
mapping_add_coalesced(struct pfs_mutation *mutation, struct pfs_object_record *object,
                      struct pfs_extent_mapping mapping)
{
  struct pfs_extent_mapping neighbor;
  enum pfs_status status = PFS_OK;
  if (mapping.logical_first && mapping_find(mutation, &object->id, mapping.logical_first - 1, false, &neighbor) &&
      mapping_compatible(&neighbor, &mapping)) {
    status = mapping_delete(mutation, object, &neighbor);
    mapping.logical_first = neighbor.logical_first;
    mapping.physical_first = neighbor.physical_first;
    mapping.count += neighbor.count;
  }
  if (status == PFS_OK && mapping_find(mutation, &object->id, mapping.logical_first + mapping.count, false, &neighbor) &&
      mapping_compatible(&mapping, &neighbor)) {
    status = mapping_delete(mutation, object, &neighbor);
    mapping.count += neighbor.count;
  }
  if (status == PFS_OK) {
    status = mapping_insert(mutation, object, &mapping);
  }
  return status;
}

static enum pfs_status
remove_data(struct pfs_mutation *mutation, const struct pfs_object_id *id,
            const struct pfs_extent_mapping *mapping, uint64_t logical, uint64_t count)
{
  if (mapping->birth == mutation->batch->generation) {
    /* Current request slices never overwrite private data twice. */
    return PFS_INVALID;
  }
  struct mutation_state *state = &mutation->state;
  if (state->data_removed_count == PFS_PLAN_VOLUME_RETIRED) {
    return PFS_LIMIT;
  }
  struct check_claim claim = {.first = mapping->physical_first + logical - mapping->logical_first,
    .count = count, .birth = mapping->birth, .logical = logical,
    .volume = state->volume.record.id, .object = *id};
  /* Adjacent removals are combined without requiring physical allocation-map
   * boundaries to match the extent's boundaries. */
  for (size_t i = 0; i < state->data_removed_count; i++) {
    struct check_claim *previous = &state->data_removed[i];
    const struct check_claim *source = source_claim(mutation, claim.first);
    if (source && previous->birth == claim.birth &&
        previous->first >= source->first && previous->first < source->first + source->count &&
        previous->first + previous->count == claim.first &&
        previous->logical + previous->count == claim.logical) {
      previous->count += count;
      return PFS_OK;
    }
  }
  state->data_removed[state->data_removed_count++] = claim;
  return PFS_OK;
}

static enum pfs_status
replace_block(struct pfs_mutation *mutation, struct pfs_object_record *object,
              uint64_t logical, size_t start, const uint8_t *data, size_t length,
              bool zero_suffix, size_t suffix)
{
  struct mutation_state *state = &mutation->state;
  size_t slot_index = PFS_PLAN_VOLUME_NEW;
  while (slot_index && state->slots[slot_index - 1].active) {
    slot_index--;
  }
  if (!slot_index) {
    return PFS_LIMIT;
  }
  slot_index--;
  struct pfs_edit_slot *slot = &state->slots[slot_index];
  struct pfs_extent_mapping old;
  bool mapped = mapping_find(mutation, &object->id, logical, false, &old);
  enum pfs_status status = PFS_OK;
  if (mapped && (start || length != PFS_BLOCK_SIZE)) {
    status = pfs_block_read(mutation->pool->reader,
      old.physical_first + logical - old.logical_first, 1, slot->data, PFS_BLOCK_SIZE);
  } else {
    pfs_bytes_zero(slot->data, PFS_BLOCK_SIZE);
  }
  if (status != PFS_OK) {
    return status;
  }
  if (zero_suffix) {
    pfs_bytes_zero(slot->data + suffix, PFS_BLOCK_SIZE - suffix);
  }
  if (length) {
    pfs_bytes_copy(slot->data + start, data, length);
  }
  slot->active = true;
  state->data_slot[slot_index] = true;
  state->data_claim[slot_index] = (struct check_claim){.first = slot->block, .count = 1,
    .birth = mutation->batch->generation, .logical = logical,
    .volume = state->volume.record.id, .object = object->id};
  if (mapped) {
    status = mapping_delete(mutation, object, &old);
    if (status == PFS_OK) {
      status = remove_data(mutation, &object->id, &old, logical, 1);
    }
    if (status == PFS_OK && old.logical_first < logical) {
      struct pfs_extent_mapping prefix = old;
      prefix.count = logical - old.logical_first;
      status = mapping_insert(mutation, object, &prefix);
    }
    if (status == PFS_OK && logical + 1 < old.logical_first + old.count) {
      struct pfs_extent_mapping tail = old;
      tail.logical_first = logical + 1;
      tail.physical_first += logical + 1 - old.logical_first;
      tail.count -= logical + 1 - old.logical_first;
      status = mapping_insert(mutation, object, &tail);
    }
  }
  if (status == PFS_OK) {
    struct pfs_extent_mapping replacement = {logical, 1, slot->block, mutation->batch->generation};
    status = mapping_add_coalesced(mutation, object, replacement);
  }
  return status == PFS_OK ? mapping_reduce(mutation, object) : status;
}

static enum pfs_status
object_update(struct pfs_mutation *mutation, const struct pfs_object_record *object)
{
  uint8_t bytes[PFS_OBJECT_RECORD_SIZE];
  struct pfs_record_context context = record_context(mutation);
  size_t written;
  enum pfs_status status = pfs_object_record_encode(bytes, sizeof(bytes), &context, object, &written);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, object->id.bytes, PFS_ID_SIZE);
  struct pfs_encoded_record encoded = {bytes, written};
  return pfs_mutation_tree(mutation, PFS_INDEX_OBJECTS, NULL,
    &mutation->state.volume.record.object_root, PFS_EDIT_UPDATE, &key, &encoded);
}

static enum pfs_status
begin_file(struct pfs_volume *volume, const struct pfs_object_id *id,
           struct pfs_mutation **out, struct pfs_object_record *object)
{
  enum pfs_status status = pfs_volume_begin(volume);
  if (status != PFS_OK) {
    return status;
  }
  pfs_writer_end(volume->pool, PFS_OK);
  struct pfs_batch *batch;
  status = pfs_writer_prepare(volume->pool, &batch);
  if (status != PFS_OK) {
    return status;
  }
  if (status == PFS_OK) {
    status = pfs_mutation_begin(volume, batch, out);
  }
  if (status == PFS_OK) {
    status = pfs_volume_object(volume, id, object);
  }
  if (status == PFS_OK && object->kind != PFS_OBJECT_FILE) {
    status = PFS_INVALID;
  }
  if (status == PFS_OK) {
    (*out)->state.orphan_object = pfs_bytes_are_zero(object->parent.bytes, PFS_ID_SIZE) &&
      !same_id(object->id.bytes, (*out)->state.volume.record.root_object.bytes);
  }
  if (status == PFS_OK) {
    for (size_t i = 0; i < (*out)->source->claim_count; i++) {
      (*out)->state.extent_count += file_claim(*out, &(*out)->source->claims[i], id);
    }
  } else {
    pfs_writer_abort(volume->pool);
    pfs_writer_end(volume->pool, status);
  }
  return status;
}

static enum pfs_status
report_failure(struct pfs_volume *volume, enum pfs_status status,
               struct pfs_write_result *result)
{
  result->completion = PFS_STOPPED;
  result->operation_status = status;
  if (volume->pool->writer) {
    result->health = volume->pool->writer->status.health;
  }
  return status;
}

static enum pfs_status
planning_failure(struct pfs_volume *volume, enum pfs_status status,
                 struct pfs_write_result *result)
{
  pfs_writer_abort(volume->pool);
  pfs_writer_end(volume->pool, status);
  return report_failure(volume, status, result);
}

static bool
accept_batch(struct pfs_write_result *result, const struct pfs_write_result *batch,
             bool last)
{
  result->completion = batch->completion == PFS_COMPLETE && !last ? PFS_STOPPED : batch->completion;
  result->operation_status = batch->operation_status;
  result->maintenance_completion = batch->maintenance_completion;
  result->maintenance_status = batch->maintenance_status;
  result->health = batch->health;
  return batch->completion == PFS_COMPLETE;
}

enum pfs_status
pfs_file_write(struct pfs_volume *volume, const struct pfs_object_id *id,
               uint64_t offset, const void *data, size_t length,
               struct pfs_write_result *result)
{
  *result = (struct pfs_write_result){.completion = PFS_STOPPED};
  size_t completed = 0;
  do {
    struct pfs_mutation *mutation;
    struct pfs_object_record object;
    enum pfs_status status = begin_file(volume, id, &mutation, &object);
    if (status != PFS_OK) {
      return report_failure(volume, status, result);
    }
    if (!length) {
      pfs_writer_abort(volume->pool);
      result->completion = PFS_COMPLETE;
      result->health = volume->pool->writer->status.health;
      return PFS_OK;
    }
    uint64_t original_length = object.file_length;
    uint64_t position = offset + completed;
    /* A distant extending write must first discard the old partial EOF suffix.
     * The payload case in that same block combines both edits below. */
    if (offset + length > original_length && original_length % PFS_BLOCK_SIZE &&
        position / PFS_BLOCK_SIZE > original_length / PFS_BLOCK_SIZE) {
      struct pfs_extent_mapping old;
      uint64_t eof_block = original_length / PFS_BLOCK_SIZE;
      if (mapping_find(mutation, id, eof_block, false, &old)) {
        status = replace_block(mutation, &object, eof_block, 0, NULL, 0, true,
          (size_t)(original_length % PFS_BLOCK_SIZE));
      }
    }
    size_t planned = 0;
    while (status == PFS_OK && completed + planned < length) {
      pfs_bytes_copy(&mutation->saved, &mutation->state, sizeof(mutation->saved));
      struct pfs_object_record saved_object = object;
      position = offset + completed + planned;
      size_t start = (size_t)(position % PFS_BLOCK_SIZE);
      size_t take = PFS_BLOCK_SIZE - start;
      if (take > length - completed - planned) {
        take = length - completed - planned;
      }
      bool zero = position / PFS_BLOCK_SIZE == original_length / PFS_BLOCK_SIZE &&
        offset + length > original_length && original_length % PFS_BLOCK_SIZE;
      status = replace_block(mutation, &object, position / PFS_BLOCK_SIZE,
        start, (const uint8_t *)data + completed + planned, take, zero,
        (size_t)(original_length % PFS_BLOCK_SIZE));
      if (status == PFS_OK) {
        uint64_t end = position + take;
        if (end > object.file_length) {
          object.file_length = end;
        }
        status = object_update(mutation, &object);
      }
      if (status == PFS_OK) {
        status = pfs_mutation_finish(mutation);
      }
      if (status == PFS_LIMIT && planned) {
        pfs_bytes_copy(&mutation->state, &mutation->saved, sizeof(mutation->state));
        object = saved_object;
        status = pfs_mutation_finish(mutation);
        break;
      }
      if (status == PFS_OK) {
        planned += take;
      }
    }
    if (status != PFS_OK) {
      return planning_failure(volume, status, result);
    }
    struct pfs_write_result batch_result;
    status = pfs_writer_commit(volume->pool, mutation->batch, &batch_result);
    bool confirmed = accept_batch(result, &batch_result, completed + planned == length);
    if (confirmed) {
      completed += planned;
      result->confirmed_bytes = completed;
    }
    if (status != PFS_OK) {
      return status;
    }
  } while (completed < length);
  return PFS_OK;
}

enum pfs_status
pfs_file_resize(struct pfs_volume *volume, const struct pfs_object_id *id,
                uint64_t length, struct pfs_write_result *result)
{
  /* The policy wrapper has already read and reported the starting length.
   * Preparation can refuse without reaching another object read. */
  bool length_valid = result->confirmed_length_valid;
  uint64_t confirmed_length = result->confirmed_length;
  *result = (struct pfs_write_result){.completion = PFS_STOPPED,
    .confirmed_length_valid = length_valid, .confirmed_length = confirmed_length};
  for (;;) {
    struct pfs_mutation *mutation;
    struct pfs_object_record object;
    enum pfs_status status = begin_file(volume, id, &mutation, &object);
    if (status != PFS_OK) {
      return report_failure(volume, status, result);
    }
    if (!result->confirmed_length_valid) {
      result->confirmed_length_valid = true;
      result->confirmed_length = object.file_length;
    }
    if (object.file_length == length) {
      pfs_writer_abort(volume->pool);
      result->completion = PFS_COMPLETE;
      result->health = volume->pool->writer->status.health;
      return PFS_OK;
    }
    bool shrinking = length < object.file_length;
    if (length > object.file_length) {
      if (object.file_length % PFS_BLOCK_SIZE) {
        struct pfs_extent_mapping mapping;
        uint64_t logical = object.file_length / PFS_BLOCK_SIZE;
        if (mapping_find(mutation, id, logical, false, &mapping)) {
          status = replace_block(mutation, &object, logical, 0, NULL, 0, true,
            (size_t)(object.file_length % PFS_BLOCK_SIZE));
        }
      }
      object.file_length = length;
    } else {
      uint64_t keep = (length + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE;
      uint64_t retired = 0;
      size_t deletions = 0;
      struct pfs_extent_mapping mapping;
      /* Six tail operations require at most 6*14 changed metadata nodes plus
       * eight for the object path. A trim is an eight-node fixed-key UPDATE,
       * counted as one of those six operations. Inline reduction removes only
       * the sole remaining leaf. Thus retirement stays below 93 metadata plus
       * 128 data blocks, with no new data and no metadata-node growth. */
      while (status == PFS_OK && deletions < SHRINK_MAPPINGS_MAX && retired < SHRINK_DATA_MAX &&
             mapping_find(mutation, id, 0, true, &mapping) && mapping.logical_first + mapping.count > keep) {
        uint64_t first = mapping.logical_first > keep ? mapping.logical_first : keep;
        uint64_t take = mapping.logical_first + mapping.count - first;
        if (take > SHRINK_DATA_MAX - retired) {
          take = SHRINK_DATA_MAX - retired;
          first = mapping.logical_first + mapping.count - take;
        }
        struct pfs_extent_mapping original = mapping;
        if (first > mapping.logical_first) {
          mapping.count = first - mapping.logical_first;
          status = mapping_trim(mutation, &object, &mapping);
        } else {
          status = mapping_delete(mutation, &object, &mapping);
        }
        if (status == PFS_OK) {
          status = remove_data(mutation, id, &original, first, take);
        }
        object.file_length = first * PFS_BLOCK_SIZE;
        retired += take;
        deletions++;
      }
      if (status == PFS_OK) {
        status = mapping_reduce(mutation, &object);
      }
      if (status == PFS_OK && (!mapping_find(mutation, id, 0, true, &mapping) ||
          mapping.logical_first + mapping.count <= keep)) {
        object.file_length = length;
      }
    }
    if (status == PFS_OK) {
      status = object_update(mutation, &object);
    }
    if (status == PFS_OK) {
      status = pfs_mutation_finish(mutation);
    }
    if (status == PFS_OK && shrinking &&
        (mutation->batch->volume.metadata_blocks > mutation->state.volume.metadata_blocks ||
         mutation->batch->volume.file_extents > mutation->state.volume.file_extents)) {
      status = PFS_CORRUPT;
    }
    if (status != PFS_OK) {
      return planning_failure(volume, status, result);
    }
    struct pfs_write_result batch_result;
    status = pfs_writer_commit(volume->pool, mutation->batch, &batch_result);
    bool confirmed = accept_batch(result, &batch_result, object.file_length == length);
    if (confirmed) {
      result->confirmed_length = object.file_length;
    }
    if (status != PFS_OK || object.file_length == length) {
      return status;
    }
  }
}

struct pfs_admit_volume *
pfs_mutation_volume(struct pfs_mutation *mutation)
{
  return &mutation->state.volume;
}

enum pfs_status
pfs_mutation_tree(struct pfs_mutation *mutation, uint16_t kind,
                  const struct pfs_object_id *object, struct pfs_reference *root,
                  enum pfs_edit_operation operation, const struct pfs_key *key,
                  const struct pfs_encoded_record *record)
{
  struct pfs_edit_candidate candidate = {
    .reader = mutation->pool->reader,
    .context = {
      .block = {
        .block_count = mutation->pool->reader->geometry.block_count,
        .selected_generation = mutation->batch->generation,
        .referring_birth = mutation->batch->generation,
        .pool = mutation->source->candidate.superblock.header.pool,
        .features = mutation->state.volume.record.features,
      },
      .kind = kind,
      .volume = mutation->state.volume.record.id,
    },
    .birth = mutation->batch->generation,
    .root = *root,
    .slots = mutation->state.slots,
    .slot_capacity = PFS_PLAN_VOLUME_NEW,
    .retired = mutation->state.retired,
    .retired_count = mutation->state.retired_count,
    .retired_capacity = PFS_PLAN_VOLUME_RETIRED,
  };
  if (object) {
    candidate.context.object = *object;
  }
  enum pfs_status status = pfs_edit_tree(&candidate, &mutation->editor, operation, key, record);
  if (status == PFS_OK) {
    *root = candidate.root;
    mutation->state.retired_count = candidate.retired_count;
    /* Slots consumed by the tree editor can only be metadata slots. */
    for (size_t i = 0; i < mutation->editor.output_count; i++) {
      mutation->state.data_slot[mutation->editor.outputs[i].slot] = false;
    }
  }
  return status;
}

static const struct check_claim *
source_claim(const struct pfs_mutation *mutation, uint64_t block)
{
  for (size_t i = 0; i < mutation->source->claim_count; i++) {
    const struct check_claim *claim = &mutation->source->claims[i];
    if (claim->first <= block && block - claim->first < claim->count) {
      return claim;
    }
  }
  return NULL;
}

static void
sort_claims(struct check_claim *claims, size_t count)
{
  for (size_t i = 1; i < count; i++) {
    struct check_claim value = claims[i];
    size_t j = i;
    while (j && claims[j - 1].first > value.first) {
      claims[j] = claims[j - 1];
      j--;
    }
    claims[j] = value;
  }
}

enum pfs_status
pfs_mutation_finish(struct pfs_mutation *mutation)
{
  struct mutation_state *state = &mutation->state;
  struct pfs_batch *batch = mutation->batch;
  batch->block_count = batch->add_count = batch->remove_count = 0;
  uint64_t metadata_added = 0, metadata_removed = 0;
  uint64_t namespace_added = 0, namespace_removed = 0;
  uint64_t data_added = 0, data_removed = 0;
  int64_t extents_delta = 0;
  for (size_t i = 0; i < PFS_PLAN_VOLUME_NEW; i++) {
    const struct pfs_edit_slot *slot = &state->slots[i];
    if (!slot->active) {
      continue;
    }
    mutation->blocks[batch->block_count++] = (struct pfs_batch_block){slot->block, slot->data};
    struct check_claim claim = {.first = slot->block, .count = 1,
      .birth = batch->generation, .volume = state->volume.record.id};
    if (state->data_slot[i]) {
      /* The engine fills object ownership when staging data. */
      claim = state->data_claim[i];
      data_added++;
    } else {
      struct pfs_block_header header;
      enum pfs_status status = pfs_block_header_decode(slot->data, PFS_BLOCK_SIZE,
        mutation->pool->reader->geometry.block_count, slot->block,
        batch->generation, &header);
      if (status != PFS_OK) {
        return status;
      }
      claim.type = PFS_BLOCK_TREE;
      claim.kind = pfs_get_u16(slot->data + PFS_BLOCK_HEADER_SIZE);
      claim.level = pfs_get_u16(slot->data + PFS_BLOCK_HEADER_SIZE + 2);
      pfs_bytes_copy(claim.object.bytes, slot->data + PFS_BLOCK_HEADER_SIZE + 24, PFS_ID_SIZE);
      metadata_added++;
      namespace_added += claim.kind == PFS_INDEX_DIRECTORY || claim.kind == PFS_INDEX_ORPHANS;
    }
    /* Keep staging-index data descriptions separate from compact output. */
    mutation->remove[PFS_PLAN_VOLUME_RETIRED - 1 - i] = claim;
  }
  /* Compact through a separate array: add[i] still holds staged data owners. */
  for (size_t i = 0; i < PFS_PLAN_VOLUME_NEW; i++) {
    if (state->slots[i].active) {
      mutation->add[batch->add_count++] = mutation->remove[PFS_PLAN_VOLUME_RETIRED - 1 - i];
    }
  }
  sort_claims(mutation->add, batch->add_count);
  size_t compact = 0;
  for (size_t i = 0; i < batch->add_count; i++) {
    struct check_claim claim = mutation->add[i];
    if (compact && !claim.type && !mutation->add[compact - 1].type &&
        same_id(claim.object.bytes, mutation->add[compact - 1].object.bytes) &&
        mutation->add[compact - 1].first + mutation->add[compact - 1].count == claim.first &&
        mutation->add[compact - 1].logical + mutation->add[compact - 1].count == claim.logical) {
      mutation->add[compact - 1].count++;
    } else {
      mutation->add[compact++] = claim;
      extents_delta += !claim.type;
    }
  }
  batch->add_count = compact;
  for (size_t i = 0; i < state->retired_count; i++) {
    const struct check_claim *claim = source_claim(mutation, state->retired[i].block);
    if (!claim || !claim->type || claim->first != state->retired[i].block ||
        claim->birth != state->retired[i].birth ||
        !same_id(claim->volume.bytes, state->volume.record.id.bytes)) {
      return PFS_CORRUPT;
    }
    mutation->remove[batch->remove_count++] = *claim;
    metadata_removed++;
    namespace_removed += claim->kind == PFS_INDEX_DIRECTORY || claim->kind == PFS_INDEX_ORPHANS;
  }
  if (state->data_removed_count > PFS_PLAN_VOLUME_RETIRED - batch->remove_count) {
    return PFS_LIMIT;
  }
  for (size_t i = 0; i < state->data_removed_count; i++) {
    mutation->remove[batch->remove_count++] = state->data_removed[i];
    data_removed += state->data_removed[i].count;
  }
  if (data_removed + metadata_removed > PFS_PLAN_VOLUME_RETIRED) {
    return PFS_LIMIT;
  }
  sort_claims(mutation->remove, batch->remove_count);
  /* Each surviving physical segment is one retained extent claim. */
  for (size_t i = 0; i < mutation->source->claim_count; i++) {
    const struct check_claim *claim = &mutation->source->claims[i];
    if (claim->type || !same_id(claim->volume.bytes, state->volume.record.id.bytes)) {
      continue;
    }
    uint64_t position = claim->first, end = position + claim->count;
    bool touched = false;
    int64_t pieces = 0;
    for (size_t j = 0; j < batch->remove_count; j++) {
      const struct check_claim *remove = &mutation->remove[j];
      if (remove->type || remove->first < claim->first || remove->first >= end) {
        continue;
      }
      if (remove->first < position || remove->first + remove->count > end) {
        return PFS_INVALID;
      }
      pieces += remove->first > position;
      position = remove->first + remove->count;
      touched = true;
    }
    if (touched) {
      pieces += position < end;
      extents_delta += pieces - 1;
    }
  }
  batch->volume = state->volume;
  batch->volume.metadata_blocks += metadata_added;
  batch->volume.metadata_blocks -= metadata_removed;
  batch->volume.namespace_nodes += namespace_added;
  batch->volume.namespace_nodes -= namespace_removed;
  if (extents_delta < 0) {
    batch->volume.file_extents -= (uint64_t)-extents_delta;
  } else {
    batch->volume.file_extents += (uint64_t)extents_delta;
  }
  batch->volume.record.live_blocks += data_added + metadata_added;
  batch->volume.record.live_blocks -= data_removed + metadata_removed;
  if (state->orphan_object) {
    batch->volume.orphan_work += data_added;
    batch->volume.orphan_work -= data_removed;
  }
  batch->blocks = mutation->blocks;
  batch->add = mutation->add;
  batch->remove = mutation->remove;
  return PFS_OK;
}

struct cleanup_frame {
  struct pfs_reference reference;
  uint64_t referring_birth;
  uint16_t parent_level;
  uint16_t child;
  bool entered;
};

static enum pfs_status
cleanup_read(struct pfs_mutation *mutation, uint16_t kind,
             const struct cleanup_frame *frame)
{
  enum pfs_status status = PFS_OK;
  if (frame->reference.birth > mutation->source->candidate.superblock.header.birth) {
    const struct pfs_edit_slot *slot = NULL;
    for (size_t i = 0; i < PFS_PLAN_VOLUME_NEW; i++) {
      if (mutation->state.slots[i].active &&
          mutation->state.slots[i].block == frame->reference.block) {
        slot = &mutation->state.slots[i];
        break;
      }
    }
    if (!slot) {
      return PFS_CORRUPT;
    }
    pfs_bytes_copy(mutation->read_block, slot->data, PFS_BLOCK_SIZE);
  } else {
    const struct check_claim *claim = source_claim(mutation, frame->reference.block);
    if (!claim || claim->first != frame->reference.block || claim->count != 1 ||
        claim->type != PFS_BLOCK_TREE || claim->kind != kind ||
        claim->birth != frame->reference.birth ||
        !same_id(claim->volume.bytes, mutation->state.volume.record.id.bytes) ||
        !pfs_bytes_are_zero(claim->object.bytes, PFS_ID_SIZE)) {
      return PFS_CORRUPT;
    }
    status = pfs_block_read(mutation->pool->reader, frame->reference.block, 1,
      mutation->read_block, PFS_BLOCK_SIZE);
  }
  struct pfs_tree_context context = {.kind = kind,
    .parent_level = frame->parent_level, .volume = mutation->state.volume.record.id,
    .block = {.block_count = mutation->pool->reader->geometry.block_count,
      .selected_generation = mutation->batch->generation,
      .referring_birth = frame->referring_birth, .reference = frame->reference,
      .pool = mutation->source->candidate.superblock.header.pool,
      .features = mutation->state.volume.record.features}};
  if (status == PFS_OK) {
    status = pfs_canonical_tree_validate(mutation->read_block, PFS_BLOCK_SIZE,
      &context, &mutation->read_tree);
  }
  return status;
}

/* Return the first leaf record at or after lower. Parent frames hold references,
 * never pointers into read_block; rereading parents also handles staged roots
 * after a preceding grant deletion. The tree depth bounds the entire stack. */
static enum pfs_status
cleanup_lower(struct pfs_mutation *mutation, uint16_t kind,
              const struct pfs_reference *root, const struct pfs_key *lower,
              struct pfs_key *key, struct pfs_tree_slot *slot)
{
  if (!root->block) {
    return PFS_NOT_FOUND;
  }
  struct cleanup_frame frames[PFS_TREE_DEPTH_MAX] = {{
    .reference = *root, .referring_birth = mutation->batch->generation}};
  size_t depth = 1;
  while (depth) {
    struct cleanup_frame *frame = &frames[depth - 1];
    enum pfs_status status = cleanup_read(mutation, kind, frame);
    if (status != PFS_OK) {
      return status;
    }
    const struct pfs_tree *tree = &mutation->read_tree;
    struct pfs_record_context context = record_context(mutation);
    context.containing_birth = tree->header.birth;
    if (!tree->level) {
      for (size_t i = 0; i < tree->count; i++) {
        uint64_t end;
        status = pfs_leaf_key_decode(mutation->read_block + tree->slots[i].offset,
          tree->slots[i].length, kind, &context, key, &end);
        if (status != PFS_OK) {
          return status;
        }
        if (pfs_key_compare(kind, key, lower) >= 0) {
          *slot = tree->slots[i];
          return PFS_OK;
        }
      }
      depth--;
      continue;
    }
    if (!frame->entered) {
      frame->child = 0;
      for (size_t i = 0; i < tree->count; i++) {
        struct pfs_internal_record record;
        status = pfs_internal_record_decode(mutation->read_block + tree->slots[i].offset,
          tree->slots[i].length, kind, &context, &record);
        if (status != PFS_OK) {
          return status;
        }
        if (pfs_key_compare(kind, &record.minimum, lower) > 0) {
          break;
        }
        frame->child = (uint16_t)i;
      }
      frame->entered = true;
    }
    if (frame->child == tree->count) {
      depth--;
      continue;
    }
    if (depth == PFS_TREE_DEPTH_MAX) {
      return PFS_CORRUPT;
    }
    struct pfs_tree_slot child_slot = tree->slots[frame->child++];
    struct pfs_internal_record child;
    status = pfs_internal_record_decode(mutation->read_block + child_slot.offset,
      child_slot.length, kind, &context, &child);
    if (status != PFS_OK) {
      return status;
    }
    frames[depth++] = (struct cleanup_frame){.reference = child.child,
      .referring_birth = tree->header.birth, .parent_level = tree->level};
  }
  return PFS_NOT_FOUND;
}

static struct pfs_key
cleanup_id_key(const struct pfs_object_id *id)
{
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, id->bytes, PFS_ID_SIZE);
  return key;
}

static enum pfs_status
cleanup_object(struct pfs_mutation *mutation, const struct pfs_object_id *id,
               struct pfs_object_record *object)
{
  struct pfs_key lower = cleanup_id_key(id), key;
  struct pfs_tree_slot slot;
  enum pfs_status status = cleanup_lower(mutation, PFS_INDEX_OBJECTS,
    &mutation->state.volume.record.object_root, &lower, &key, &slot);
  if (status == PFS_OK && pfs_key_compare(PFS_INDEX_OBJECTS, &key, &lower)) {
    return PFS_NOT_FOUND;
  }
  if (status == PFS_OK) {
    struct pfs_record_context context = record_context(mutation);
    context.containing_birth = mutation->read_tree.header.birth;
    status = pfs_object_record_decode(mutation->read_block + slot.offset,
      slot.length, &context, object);
  }
  return status;
}

static enum pfs_status
cleanup_orphan(struct pfs_mutation *mutation, const struct pfs_object_id *id,
               bool *present)
{
  struct pfs_key lower = cleanup_id_key(id), key;
  struct pfs_tree_slot slot;
  enum pfs_status status = cleanup_lower(mutation, PFS_INDEX_ORPHANS,
    &mutation->state.volume.record.orphan_root, &lower, &key, &slot);
  if (status == PFS_NOT_FOUND) {
    *present = false;
    return PFS_OK;
  }
  if (status == PFS_OK) {
    *present = !pfs_key_compare(PFS_INDEX_ORPHANS, &key, &lower);
  }
  return status;
}

static enum pfs_status
cleanup_grant(struct pfs_mutation *mutation, const struct pfs_object_id *id,
              struct pfs_key *key, bool *present)
{
  struct pfs_key lower = {.length = 2 * PFS_ID_SIZE + 1};
  pfs_bytes_copy(lower.bytes, id->bytes, PFS_ID_SIZE);
  lower.bytes[2 * PFS_ID_SIZE - 1] = 1;
  lower.bytes[2 * PFS_ID_SIZE] = PFS_SCOPE_OBJECT;
  struct pfs_tree_slot slot;
  enum pfs_status status = cleanup_lower(mutation, PFS_INDEX_GRANTS,
    &mutation->state.volume.record.grant_root, &lower, key, &slot);
  if (status == PFS_NOT_FOUND) {
    *present = false;
    return PFS_OK;
  }
  if (status == PFS_OK) {
    *present = same_id(key->bytes, id->bytes);
  }
  return status;
}

static enum pfs_status
cleanup_final(struct pfs_mutation *mutation, const struct pfs_object_record *object)
{
  struct pfs_key key = cleanup_id_key(&object->id);
  enum pfs_status status = pfs_mutation_tree(mutation, PFS_INDEX_OBJECTS, NULL,
    &mutation->state.volume.record.object_root, PFS_EDIT_DELETE, &key, NULL);
  if (status == PFS_OK) {
    status = pfs_mutation_tree(mutation, PFS_INDEX_ORPHANS, NULL,
      &mutation->state.volume.record.orphan_root, PFS_EDIT_DELETE, &key, NULL);
  }
  if (status == PFS_OK) {
    mutation->state.volume.record.object_count--;
    mutation->state.volume.directories -= object->kind == PFS_OBJECT_DIRECTORY;
    mutation->state.volume.orphan_work--;
  }
  return status;
}

static void
cleanup_directory_forget(struct pfs_writer *writer, const struct pfs_volume_id *volume,
                         const struct pfs_object_id *id)
{
  struct pfs_changed_directory *table = (void *)writer->arena.directories;
  for (size_t i = 0; i < writer->changed_directories; i++) {
    if (same_id(table[i].volume.bytes, volume->bytes) &&
        same_id(table[i].object.bytes, id->bytes)) {
      table[i] = table[--writer->changed_directories];
      return;
    }
  }
}

static enum pfs_status
cleanup_failure(struct pfs_pool *pool, enum pfs_status status,
                struct pfs_write_result *result)
{
  pfs_writer_funded_failure(pool, status);
  result->completion = PFS_STOPPED;
  result->maintenance_completion = PFS_MAINTENANCE_STOPPED;
  result->maintenance_status = status;
  result->health = pool->writer->status.health;
  return status;
}

static enum pfs_status
cleanup_batches(struct pfs_pool *pool, const struct pfs_volume_id *volume,
                const struct pfs_object_id *id, struct pfs_write_result *result)
{
  *result = (struct pfs_write_result){.completion = PFS_COMPLETE,
    .health = pool->writer->status.health};
  if (pfs_runtime_references(pool, volume, id)) {
    return PFS_BUSY;
  }
  /* A named last close has no admitted cleanup work and must not need mutation
   * generation headroom or reusable blocks merely to release its view. */
  enum pfs_status checked = pfs_writer_begin(pool, true);
  if (checked != PFS_OK) {
    return cleanup_failure(pool, checked, result);
  }
  struct pfs_batch probe = {.generation = pool->writer->last_confirmed_generation};
  struct pfs_mutation *probe_mutation;
  checked = mutation_begin(pool, volume, &probe, &probe_mutation);
  struct pfs_object_record probe_object;
  bool probe_orphan = false;
  if (checked == PFS_OK) {
    checked = cleanup_object(probe_mutation, id, &probe_object);
  }
  if (checked == PFS_OK) {
    checked = cleanup_orphan(probe_mutation, id, &probe_orphan);
  }
  if (checked == PFS_OK && !probe_orphan &&
      pfs_bytes_are_zero(probe_object.parent.bytes, PFS_ID_SIZE) &&
      !same_id(probe_object.id.bytes, probe_mutation->state.volume.record.root_object.bytes)) {
    checked = PFS_CORRUPT;
  }
  pfs_writer_end(pool, checked);
  if (checked != PFS_OK) {
    return cleanup_failure(pool, checked, result);
  }
  if (!probe_orphan) {
    return PFS_OK;
  }
  for (;;) {
    struct pfs_batch *batch;
    enum pfs_status status = pfs_writer_prepare(pool, &batch);
    if (status != PFS_OK) {
      return cleanup_failure(pool, status, result);
    }
    struct pfs_mutation *mutation;
    status = mutation_begin(pool, volume, batch, &mutation);
    struct pfs_object_record object;
    bool orphan = false;
    if (status == PFS_OK) {
      status = cleanup_object(mutation, id, &object);
    }
    if (status == PFS_OK) {
      status = cleanup_orphan(mutation, id, &orphan);
    }
    if (status == PFS_OK && !orphan) {
      status = pfs_bytes_are_zero(object.parent.bytes, PFS_ID_SIZE) &&
        !same_id(object.id.bytes, mutation->state.volume.record.root_object.bytes) ?
        PFS_CORRUPT : PFS_OK;
      if (status == PFS_OK) {
        pfs_writer_abort(pool);
        return PFS_OK;
      }
    }
    if (status == PFS_OK && (!pfs_bytes_are_zero(object.parent.bytes, PFS_ID_SIZE) ||
        same_id(object.id.bytes, mutation->state.volume.record.root_object.bytes) ||
        (object.kind == PFS_OBJECT_DIRECTORY && object.directory_count))) {
      status = PFS_CORRUPT;
    }
    batch->orphan_cleanup = true;
    bool final = false;
    uint64_t initial_work = status == PFS_OK ? mutation->state.volume.orphan_work : 0;
    if (status == PFS_OK) {
      mutation->state.orphan_object = true;
      for (size_t i = 0; i < mutation->source->claim_count; i++) {
        mutation->state.extent_count += file_claim(mutation, &mutation->source->claims[i], id);
      }
      struct pfs_extent_mapping mapping;
      bool combine_final = object.kind == PFS_OBJECT_FILE &&
        object.storage_kind == PFS_STORAGE_INLINE && object.inline_extent.count == 1 &&
        mutation->state.extent_count == 1 &&
        mapping_find(mutation, id, 0, true, &mapping) && mapping.count == 1;
      if (combine_final) {
        struct pfs_key key;
        bool grants = false;
        status = cleanup_grant(mutation, id, &key, &grants);
        combine_final = status == PFS_OK && !grants;
      }
      uint64_t retired = 0;
      size_t edits = 0;
      while (status == PFS_OK && edits < SHRINK_MAPPINGS_MAX && retired < SHRINK_DATA_MAX &&
             mapping_find(mutation, id, 0, true, &mapping)) {
        uint64_t take = mapping.count;
        if (take > SHRINK_DATA_MAX - retired) {
          take = SHRINK_DATA_MAX - retired;
        }
        uint64_t first = mapping.logical_first + mapping.count - take;
        struct pfs_extent_mapping original = mapping;
        if (take < mapping.count) {
          mapping.count -= take;
          status = mapping_trim(mutation, &object, &mapping);
        } else {
          status = mapping_delete(mutation, &object, &mapping);
        }
        if (status == PFS_OK) {
          status = remove_data(mutation, id, &original, first, take);
        }
        object.file_length = first * PFS_BLOCK_SIZE;
        retired += take;
        edits++;
      }
      if (status == PFS_OK && edits) {
        if (combine_final) {
          /* The proved one-block path deletes both records without an empty-object update. */
          status = cleanup_final(mutation, &object);
          final = status == PFS_OK;
        } else {
          status = mapping_reduce(mutation, &object);
          if (status == PFS_OK && !mapping_find(mutation, id, 0, true, &mapping)) {
            object.file_length = 0;
          }
          if (status == PFS_OK) {
            status = object_update(mutation, &object);
          }
        }
      } else if (status == PFS_OK) {
        bool grants = false;
        struct pfs_key key;
        while (status == PFS_OK && edits < SHRINK_MAPPINGS_MAX) {
          status = cleanup_grant(mutation, id, &key, &grants);
          if (status != PFS_OK || !grants) {
            break;
          }
          status = pfs_mutation_tree(mutation, PFS_INDEX_GRANTS, NULL,
            &mutation->state.volume.record.grant_root, PFS_EDIT_DELETE, &key, NULL);
          if (status == PFS_OK) {
            mutation->state.volume.grants--;
            mutation->state.volume.orphan_work--;
            edits++;
          }
        }
        /* Keep final paired deletion separate from six grant edits. */
        if (status == PFS_OK && !edits && !grants) {
          status = cleanup_final(mutation, &object);
          final = status == PFS_OK;
        }
      }
    }
    if (status == PFS_OK) {
      status = pfs_mutation_finish(mutation);
    }
    if (status == PFS_OK &&
        (batch->volume.metadata_blocks > mutation->state.volume.metadata_blocks ||
         batch->volume.file_extents > mutation->state.volume.file_extents ||
         batch->volume.orphan_work >= initial_work)) {
      status = PFS_LIMIT;
    }
    if (status != PFS_OK) {
      pfs_writer_funded_failure(pool, status);
      pfs_writer_abort(pool);
      return cleanup_failure(pool, status, result);
    }
    struct pfs_write_result committed;
    status = pfs_writer_commit(pool, batch, &committed);
    if (final && committed.completion == PFS_COMPLETE) {
      cleanup_directory_forget(pool->writer, volume, id);
    }
    if (status != PFS_OK) {
      pfs_writer_funded_failure(pool, status);
      result->completion = committed.completion == PFS_COMPLETE && !final ?
        PFS_STOPPED : committed.completion;
      result->maintenance_completion = committed.maintenance_status != PFS_OK ?
        committed.maintenance_completion : committed.completion == PFS_UNKNOWN ?
        PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED;
      result->maintenance_status = status;
      result->health = pool->writer->status.health;
      return status;
    }
    result->maintenance_completion = PFS_MAINTENANCE_COMPLETE;
    if (final) {
      return PFS_OK;
    }
  }
}

enum pfs_status
pfs_orphan_cleanup(struct pfs_volume *volume, const struct pfs_object_id *id,
                    struct pfs_write_result *result)
{
  if (!volume || !volume->pool || !volume->pool->writer || !id || !result) {
    return PFS_INVALID;
  }
  if (volume->pool->writer_busy) {
    return PFS_BUSY;
  }
  struct pfs_volume_record record;
  enum pfs_status status = pfs_volume_metadata(volume, &record);
  return status == PFS_OK ? cleanup_batches(volume->pool, &record.id, id, result) : status;
}

enum pfs_status
pfs_orphan_recover(struct pfs_pool *pool, struct pfs_write_result *result)
{
  if (!pool || !pool->writer || !result || pool->writer_busy) {
    return PFS_INVALID;
  }
  for (size_t i = 0; i < pool->writer->states[pool->writer->selected].volume_count; i++) {
    for (;;) {
      const struct pfs_admit_volume *volume =
        &pool->writer->states[pool->writer->selected].volumes[i];
      if (!volume->record.orphan_root.block) {
        break;
      }
      struct pfs_volume_id volume_id = volume->record.id;
      struct pfs_batch *batch;
      enum pfs_status status = pfs_writer_prepare(pool, &batch);
      if (status != PFS_OK) {
        return cleanup_failure(pool, status, result);
      }
      struct pfs_mutation *mutation;
      status = mutation_begin(pool, &volume_id, batch, &mutation);
      struct pfs_key lower = {.length = PFS_ID_SIZE}, key;
      lower.bytes[PFS_ID_SIZE - 1] = 1;
      struct pfs_tree_slot slot;
      if (status == PFS_OK) {
        status = cleanup_lower(mutation, PFS_INDEX_ORPHANS,
          &mutation->state.volume.record.orphan_root, &lower, &key, &slot);
      }
      struct pfs_object_id id;
      if (status == PFS_OK) {
        pfs_bytes_copy(id.bytes, key.bytes, PFS_ID_SIZE);
      }
      pfs_writer_abort(pool);
      if (status != PFS_OK) {
        return cleanup_failure(pool, status == PFS_NOT_FOUND ? PFS_CORRUPT : status, result);
      }
      struct pfs_write_result cleanup;
      status = cleanup_batches(pool, &volume_id, &id, &cleanup);
      result->maintenance_completion = cleanup.maintenance_completion;
      result->maintenance_status = cleanup.maintenance_status;
      result->health = cleanup.health;
      if (status != PFS_OK) {
        result->completion = cleanup.completion;
        return status;
      }
    }
  }
  return PFS_OK;
}
