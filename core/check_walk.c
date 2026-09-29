#include "check_internal.h"

/* Each retained frame owns its input bytes until all of its children finish. */
struct check_frame {
  uint8_t data[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  struct pfs_tree_context context;
  struct pfs_key lower;
  struct pfs_key upper;
  bool bounded_lower;
  bool bounded_upper;
  uint16_t next;
};

struct check_walk {
  struct check_state *state;
  struct check_volume *volume;
  struct check_object *object;
  uint16_t kind;
  bool objects_complete;
  uint64_t records;
  uint64_t previous_end;
};

static bool
id_equal(const void *left, const void *right)
{
  return !pfs_bytes_compare(left, PFS_ID_SIZE, right, PFS_ID_SIZE);
}

static struct check_object *
find_object(struct check_volume *volume, const struct pfs_object_id *id)
{
  struct check_object *objects = volume->objects.data;
  size_t first = 0;
  size_t end = volume->object_count;
  while (first < end) {
    size_t middle = first + (end - first) / 2;
    int order = pfs_bytes_compare(objects[middle].record.id.bytes, PFS_ID_SIZE,
                                  id->bytes, PFS_ID_SIZE);
    if (order < 0) {
      first = middle + 1;
    } else if (order > 0) {
      end = middle;
    } else {
      return &objects[middle];
    }
  }
  return NULL;
}

static void
walk_problem(struct check_walk *walk, enum pfs_status status,
             const char *operation, uint64_t block)
{
  check_problem(walk->state, status, operation, block,
                walk->volume ? &walk->volume->record.id : NULL,
                walk->object ? &walk->object->record.id : NULL);
}

static size_t
seen_slot(uint64_t block, size_t capacity)
{
  block ^= block >> 30;
  block *= UINT64_C(0xbf58476d1ce4e5b9);
  block ^= block >> 27;
  block *= UINT64_C(0x94d049bb133111eb);
  block ^= block >> 31;
  return (size_t)block & (capacity - 1);
}

static enum pfs_status
remember_block(struct check_state *state, uint64_t block)
{
  size_t capacity = state->seen.size / sizeof(uint64_t);
  uint64_t *seen = state->seen.data;
  if (capacity) {
    size_t slot = seen_slot(block, capacity);
    while (seen[slot]) {
      if (seen[slot] == block) {
        return PFS_CORRUPT;
      }
      slot = (slot + 1) & (capacity - 1);
    }
  }
  if (state->seen_count >= CHECK_METADATA_MAX) {
    return PFS_LIMIT;
  }
  if (!capacity || state->seen_count >= capacity / 2) {
    size_t next_capacity = capacity ? capacity * 2 : 64;
    struct pfs_allocation next = {0};
    enum pfs_status status = pfs_memory_allocate(state->memory,
                                                 next_capacity * sizeof(uint64_t),
                                                 _Alignof(uint64_t), &next);
    if (status != PFS_OK) {
      return status;
    }
    pfs_bytes_zero(next.data, next.size);
    uint64_t *old = state->seen.data;
    uint64_t *fresh = next.data;
    for (size_t i = 0; i < capacity; ++i) {
      if (!old[i]) {
        continue;
      }
      size_t slot = seen_slot(old[i], next_capacity);
      while (fresh[slot]) {
        slot = (slot + 1) & (next_capacity - 1);
      }
      fresh[slot] = old[i];
    }
    pfs_memory_free(state->memory, &state->seen);
    pfs_memory_move(state->memory, &next, &state->seen);
    capacity = next_capacity;
  }
  seen = state->seen.data;
  size_t slot = seen_slot(block, capacity);
  while (seen[slot]) {
    slot = (slot + 1) & (capacity - 1);
  }
  seen[slot] = block;
  state->seen_count++;
  return PFS_OK;
}

static enum pfs_status
metadata_claim(struct check_state *state, const struct pfs_block_header *header,
               uint16_t kind, uint16_t level, const struct pfs_volume_id *volume,
               const struct pfs_object_id *object)
{
  struct check_claim claim = {
    .first = header->block,
    .count = 1,
    .birth = header->birth,
    .type = header->type,
    .kind = kind,
    .level = level,
  };
  if (volume) {
    claim.volume = *volume;
  }
  if (object) {
    claim.object = *object;
  }
  return check_add_claim(state, &claim);
}

static enum pfs_status
extent_claim(struct check_walk *walk, const struct pfs_extent_mapping *mapping)
{
  enum pfs_status status = pfs_extent_file_validate(mapping, walk->object->record.file_length);
  if (status != PFS_OK) {
    return status;
  }
  if (walk->records && mapping->logical_first < walk->previous_end) {
    return PFS_CORRUPT;
  }
  if (walk->state->result->file_extents >= PFS_RECORD_COUNT_MAX) {
    return PFS_LIMIT;
  }
  struct check_claim claim = {
    .first = mapping->physical_first,
    .count = mapping->count,
    .birth = mapping->birth,
    .logical = mapping->logical_first,
    .volume = walk->volume->record.id,
    .object = walk->object->record.id,
  };
  status = check_add_claim(walk->state, &claim);
  if (status == PFS_OK) {
    walk->previous_end = mapping->logical_first + mapping->count;
    walk->state->result->file_extents++;
  }
  return status;
}

static enum pfs_status
visit_record(struct check_walk *walk, const uint8_t *data, size_t length,
             const struct pfs_record_context *context)
{
  struct check_state *state = walk->state;
  enum pfs_status status;
  switch (walk->kind) {
  case PFS_INDEX_VOLUMES: {
    if (state->volume_count >= PFS_VOLUME_MAX) {
      return PFS_LIMIT;
    }
    struct check_volume *volumes = state->volumes.data;
    struct check_volume *volume = &volumes[state->volume_count];
    status = pfs_volume_record_decode(data, length, context, &volume->record);
    if (status != PFS_OK) {
      return status;
    }
    state->volume_count++;
    state->result->volumes++;
    volume->supported = pfs_features_check(&volume->record.features) == PFS_OK &&
                         volume->record.object_root.version == PFS_FORMAT_VERSION &&
                         (!volume->record.grant_root.block ||
                          volume->record.grant_root.version == PFS_FORMAT_VERSION);
    if (!volume->supported) {
      state->result->unsupported_volumes++;
      check_problem(state, PFS_UNSUPPORTED, "volume features or tree versions",
                    UINT64_MAX, &volume->record.id, NULL);
    }
    return PFS_OK;
  }
  case PFS_INDEX_VOLUME_NAMES: {
    if (state->name_count >= PFS_VOLUME_MAX) {
      return PFS_LIMIT;
    }
    struct pfs_volume_name_record *names = state->names.data;
    status = pfs_volume_name_record_decode(data, length, context, &names[state->name_count]);
    if (status == PFS_OK) {
      state->name_count++;
    }
    return status;
  }
  case PFS_INDEX_ALLOCATION: {
    struct pfs_allocation_record record;
    status = pfs_allocation_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    status = check_grow(state->memory, &state->allocations, state->allocation_count,
                         sizeof(record), _Alignof(struct pfs_allocation_record),
                         PFS_ALLOCATION_COUNT_MAX);
    if (status == PFS_OK) {
      struct pfs_allocation_record *records = state->allocations.data;
      records[state->allocation_count++] = record;
    }
    return status;
  }
  case PFS_INDEX_OBJECTS: {
    if (state->result->objects >= PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    struct check_object object = { .birth = context->containing_birth };
    status = pfs_object_record_decode(data, length, context, &object.record);
    if (status != PFS_OK) {
      return status;
    }
    status = check_grow(state->memory, &walk->volume->objects, walk->volume->object_count,
                         sizeof(object), _Alignof(struct check_object), PFS_RECORD_COUNT_MAX);
    if (status == PFS_OK) {
      struct check_object *objects = walk->volume->objects.data;
      objects[walk->volume->object_count++] = object;
      state->result->objects++;
    }
    return status;
  }
  case PFS_INDEX_DIRECTORY: {
    struct pfs_dirent_record record;
    status = pfs_dirent_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    if (state->result->directory_entries >= PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    state->result->directory_entries++;
    struct check_object *child = find_object(walk->volume, &record.object);
    if (!child) {
      return walk->objects_complete ? PFS_CORRUPT : PFS_OK;
    }
    if (child->incoming != UINT32_MAX) {
      child->incoming++;
    }
    if (child->record.kind != record.child_kind ||
        !id_equal(child->record.parent.bytes, walk->object->record.id.bytes)) {
      return PFS_CORRUPT;
    }
    return PFS_OK;
  }
  case PFS_INDEX_EXTENTS: {
    struct pfs_extent_record record;
    status = pfs_extent_record_decode(data, length, context, &record);
    return status == PFS_OK ? extent_claim(walk, &record.mapping) : status;
  }
  case PFS_INDEX_GRANTS: {
    struct pfs_grant_record record;
    status = pfs_grant_record_decode(data, length, context, &record);
    if (status != PFS_OK) {
      return status;
    }
    if (state->result->grants >= PFS_RECORD_COUNT_MAX) {
      return PFS_LIMIT;
    }
    state->result->grants++;
    struct check_object *target = find_object(walk->volume, &record.object);
    if (!target) {
      return walk->objects_complete ? PFS_CORRUPT : PFS_OK;
    }
    status = pfs_grant_target_validate(&record, target->record.kind);
    if (status == PFS_OK && ++target->grants > 64) {
      status = PFS_LIMIT;
    }
    return status;
  }
  default:
    return PFS_UNSUPPORTED;
  }
}

static struct pfs_record_context
frame_records(const struct check_frame *frame)
{
  return (struct pfs_record_context) {
    .block_count = frame->context.block.block_count,
    .selected_generation = frame->context.block.selected_generation,
    .containing_birth = frame->tree.header.birth,
    .features = frame->context.block.features,
  };
}

static enum pfs_status
read_frame(struct check_walk *walk, struct check_frame *frame)
{
  struct check_state *state = walk->state;
  enum pfs_status status = remember_block(state, frame->context.block.reference.block);
  if (status == PFS_OK) {
    status = pfs_block_read(state->reader, frame->context.block.reference.block,
                            1, frame->data, sizeof(frame->data));
  }
  if (status == PFS_OK) {
    status = pfs_tree_decode(frame->data, sizeof(frame->data), &frame->context, &frame->tree);
  }
  if (status != PFS_OK) {
    return status;
  }
  status = metadata_claim(state, &frame->tree.header, frame->tree.kind, frame->tree.level,
                            &frame->tree.volume, &frame->tree.object);
  if (status != PFS_OK) {
    return status;
  }
  if ((frame->bounded_lower &&
       pfs_key_compare(walk->kind, &frame->tree.minimum, &frame->lower)) ||
      (frame->bounded_upper &&
       pfs_key_compare(walk->kind, &frame->tree.maximum, &frame->upper) >= 0)) {
    return PFS_CORRUPT;
  }
  frame->next = 0;
  return PFS_OK;
}

static bool
walk_tree(struct check_walk *walk, const struct pfs_reference *root, uint64_t referring_birth)
{
  if (!root->block) {
    return true;
  }
  struct check_state *state = walk->state;
  struct check_frame *frames = state->frames.data;
  struct check_frame *frame = &frames[0];
  pfs_bytes_zero(frame, sizeof(*frame));
  frame->context = (struct pfs_tree_context) {
    .block = {
      .block_count = state->reader->geometry.block_count,
      .selected_generation = state->candidate->superblock.header.birth,
      .referring_birth = referring_birth,
      .pool = state->candidate->superblock.header.pool,
      .reference = *root,
      .features = walk->volume ? walk->volume->record.features :
                                state->candidate->superblock.features,
    },
    .kind = walk->kind,
  };
  if (walk->volume) {
    frame->context.volume = walk->volume->record.id;
  }
  if (walk->object) {
    frame->context.object = walk->object->record.id;
  }
  enum pfs_status status = read_frame(walk, frame);
  size_t depth = 1;
  while (status == PFS_OK && depth) {
    frame = &frames[depth - 1];
    if (frame->next == frame->tree.count) {
      depth--;
      continue;
    }
    struct pfs_tree_slot slot = frame->tree.slots[frame->next++];
    struct pfs_record_context context = frame_records(frame);
    if (!frame->tree.level) {
      status = visit_record(walk, frame->data + slot.offset, slot.length, &context);
      if (status == PFS_OK) {
        walk->records++;
      }
      continue;
    }
    struct pfs_internal_record record;
    status = pfs_internal_record_decode(frame->data + slot.offset, slot.length,
                                        walk->kind, &context, &record);
    if (status != PFS_OK) {
      break;
    }
    if (depth >= PFS_TREE_DEPTH_MAX) {
      status = PFS_LIMIT;
      break;
    }
    struct check_frame *child = &frames[depth];
    pfs_bytes_zero(child, sizeof(*child));
    child->context = frame->context;
    child->context.block.reference = record.child;
    child->context.block.referring_birth = frame->tree.header.birth;
    child->context.parent_level = frame->tree.level;
    child->bounded_lower = true;
    pfs_bytes_copy(&child->lower, &record.minimum, sizeof(child->lower));
    child->bounded_upper = frame->bounded_upper;
    pfs_bytes_copy(&child->upper, &frame->upper, sizeof(child->upper));
    if (frame->next < frame->tree.count) {
      struct pfs_tree_slot next_slot = frame->tree.slots[frame->next];
      struct pfs_internal_record next;
      status = pfs_internal_record_decode(frame->data + next_slot.offset, next_slot.length,
                                          walk->kind, &context, &next);
      if (status != PFS_OK) {
        break;
      }
      child->bounded_upper = true;
      pfs_bytes_copy(&child->upper, &next.minimum, sizeof(child->upper));
    }
    status = read_frame(walk, child);
    depth++;
  }
  if (status != PFS_OK) {
    walk_problem(walk, status, "walk tree", frames[depth - 1].context.block.reference.block);
    return false;
  }
  return true;
}

static bool
check_catalogs(struct check_state *state, bool volumes_complete, bool names_complete)
{
  if (!volumes_complete || !names_complete) {
    return false;
  }
  if (state->volume_count != state->name_count ||
      state->volume_count != state->candidate->root.volume_count) {
    check_problem(state, PFS_CORRUPT, "volume catalog count", UINT64_MAX, NULL, NULL);
    return false;
  }
  struct check_volume *volumes = state->volumes.data;
  struct pfs_volume_name_record *names = state->names.data;
  bool valid = true;
  for (size_t i = 0; i < state->volume_count; ++i) {
    size_t matches = 0;
    for (size_t j = 0; j < state->name_count; ++j) {
      if (id_equal(volumes[i].record.id.bytes, names[j].volume.bytes) &&
          !pfs_bytes_compare(volumes[i].record.name.bytes, volumes[i].record.name.length,
                             names[j].name.bytes, names[j].name.length)) {
        matches++;
      }
    }
    if (matches != 1) {
      check_problem(state, PFS_CORRUPT, "volume catalogs disagree", UINT64_MAX,
                    &volumes[i].record.id, NULL);
      valid = false;
    }
  }
  return valid;
}

static struct check_object *
object_parent(struct check_volume *volume, struct check_object *object)
{
  return object ? find_object(volume, &object->record.parent) : NULL;
}

static bool
check_namespace(struct check_state *state, struct check_volume *volume, bool directories_complete)
{
  struct check_object *objects = volume->objects.data;
  struct check_object *root = find_object(volume, &volume->record.root_object);
  bool valid = true;
  if (!root || root->record.kind != PFS_OBJECT_DIRECTORY ||
      !pfs_bytes_are_zero(root->record.parent.bytes, PFS_ID_SIZE) || root->incoming) {
    check_problem(state, PFS_CORRUPT, "volume root object", UINT64_MAX,
                  &volume->record.id, &volume->record.root_object);
    valid = false;
  }
  if (root) {
    root->depth = 1;
  }
  for (size_t i = 0; i < volume->object_count; ++i) {
    struct check_object *object = &objects[i];
    bool is_root = id_equal(object->record.id.bytes, volume->record.root_object.bytes);
    if (!is_root && (pfs_bytes_are_zero(object->record.parent.bytes, PFS_ID_SIZE) ||
                     object->incoming > 1 || (directories_complete && object->incoming != 1))) {
      check_problem(state, PFS_CORRUPT, "object namespace entry", UINT64_MAX,
                    &volume->record.id, &object->record.id);
      valid = false;
    }
    if (is_root) {
      object->depth = 1;
      continue;
    }
    struct check_object *slow = object;
    struct check_object *fast = object;
    struct check_object *ancestor = object;
    uint32_t depth = 1;
    enum pfs_status status = PFS_OK;
    while (ancestor && ancestor != root) {
      if (ancestor->depth) {
        depth += ancestor->depth - 1;
        if (depth > CHECK_ANCESTRY_MAX) {
          status = PFS_LIMIT;
        }
        break;
      }
      ancestor = object_parent(volume, ancestor);
      if (!ancestor || ancestor->record.kind != PFS_OBJECT_DIRECTORY) {
        status = PFS_CORRUPT;
        break;
      }
      slow = object_parent(volume, slow);
      fast = object_parent(volume, object_parent(volume, fast));
      if (slow && fast && slow == fast) {
        status = PFS_CORRUPT;
        break;
      }
      if (++depth > CHECK_ANCESTRY_MAX) {
        status = PFS_LIMIT;
        break;
      }
    }
    if (status != PFS_OK) {
      check_problem(state, status, "object ancestry", UINT64_MAX,
                    &volume->record.id, &object->record.id);
      valid = false;
    } else {
      object->depth = depth;
    }
  }
  return valid;
}

static void
walk_volume(struct check_state *state, struct check_volume *volume)
{
  if (!volume->supported) {
    return;
  }
  struct check_walk walk = {
    .state = state,
    .volume = volume,
    .kind = PFS_INDEX_OBJECTS,
  };
  /* Catalog decoding has already bounded these references by their containing
   * birth. No containing catalog block remains needed for this traversal. */
  bool objects_complete = walk_tree(&walk, &volume->record.object_root,
                                    state->candidate->superblock.header.birth);
  bool valid = objects_complete;
  if (objects_complete && volume->object_count != volume->record.object_count) {
    check_problem(state, PFS_CORRUPT, "volume object count", UINT64_MAX,
                  &volume->record.id, NULL);
    valid = false;
  }
  bool directories_complete = objects_complete;
  struct check_object *objects = volume->objects.data;
  for (size_t i = 0; i < volume->object_count; ++i) {
    struct check_object *object = &objects[i];
    walk = (struct check_walk) {
      .state = state,
      .volume = volume,
      .object = object,
      .objects_complete = objects_complete,
      .kind = object->record.kind == PFS_OBJECT_DIRECTORY ? PFS_INDEX_DIRECTORY :
                                                          PFS_INDEX_EXTENTS,
    };
    if (object->record.storage_kind == PFS_STORAGE_INLINE) {
      enum pfs_status status = extent_claim(&walk, &object->record.inline_extent);
      if (status != PFS_OK) {
        walk_problem(&walk, status, "inline extent", object->record.inline_extent.physical_first);
        valid = false;
      }
    } else if (object->record.storage_kind == PFS_STORAGE_TREE) {
      bool complete = walk_tree(&walk, &object->record.tree_root, object->birth);
      if (!complete) {
        valid = false;
        if (object->record.kind == PFS_OBJECT_DIRECTORY) {
          directories_complete = false;
        }
      } else if ((object->record.kind == PFS_OBJECT_DIRECTORY &&
                  walk.records != object->record.directory_count) ||
                 (object->record.kind == PFS_OBJECT_FILE && walk.records < 2)) {
        walk_problem(&walk, PFS_CORRUPT, "object storage count", object->record.tree_root.block);
        valid = false;
      }
    }
  }
  if (objects_complete && !check_namespace(state, volume, directories_complete)) {
    valid = false;
  }
  walk = (struct check_walk) {
    .state = state,
    .volume = volume,
    .objects_complete = objects_complete,
    .kind = PFS_INDEX_GRANTS,
  };
  if (!walk_tree(&walk, &volume->record.grant_root, state->candidate->superblock.header.birth)) {
    valid = false;
  }
  volume->complete = valid;
}

void
check_walk_state(struct check_state *state)
{
  enum pfs_status status = remember_block(state, state->candidate->root.header.block);
  if (status == PFS_OK) {
    status = metadata_claim(state, &state->candidate->root.header, 0, 0, NULL, NULL);
  }
  if (status == PFS_OK) {
    status = pfs_memory_allocate(state->memory, PFS_VOLUME_MAX * sizeof(struct check_volume),
                                 _Alignof(struct check_volume), &state->volumes);
  }
  if (status == PFS_OK) {
    pfs_bytes_zero(state->volumes.data, state->volumes.size);
    status = pfs_memory_allocate(state->memory, PFS_VOLUME_MAX * sizeof(struct pfs_volume_name_record),
                                 _Alignof(struct pfs_volume_name_record), &state->names);
  }
  if (status == PFS_OK) {
    pfs_bytes_zero(state->names.data, state->names.size);
    status = pfs_memory_allocate(state->memory, PFS_TREE_DEPTH_MAX * sizeof(struct check_frame),
                                 _Alignof(struct check_frame), &state->frames);
  }
  if (status != PFS_OK) {
    check_problem(state, status, "checker traversal workspace", UINT64_MAX, NULL, NULL);
    return;
  }
  struct check_walk walk = { .state = state, .kind = PFS_INDEX_ALLOCATION };
  state->map_complete = walk_tree(&walk, &state->candidate->root.allocation,
                                  state->candidate->root.header.birth);
  walk = (struct check_walk) { .state = state, .kind = PFS_INDEX_VOLUMES };
  bool volumes_complete = walk_tree(&walk, &state->candidate->root.volumes,
                                    state->candidate->root.header.birth);
  walk = (struct check_walk) { .state = state, .kind = PFS_INDEX_VOLUME_NAMES };
  bool names_complete = walk_tree(&walk, &state->candidate->root.volume_names,
                                  state->candidate->root.header.birth);
  state->catalog_complete = check_catalogs(state, volumes_complete, names_complete);
  state->pool_complete = state->map_complete && state->catalog_complete;
  struct check_volume *volumes = state->volumes.data;
  for (size_t i = 0; i < state->volume_count; ++i) {
    walk_volume(state, &volumes[i]);
  }
}
