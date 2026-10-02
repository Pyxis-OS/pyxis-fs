/* SPDX-License-Identifier: MPL-2.0 */
#include "access_internal.h"
#include "file.h"
#include "read_internal.h"
#include "writer.h"
#include "canonical.h"

_Static_assert(sizeof(struct pfs_directory_token) == 64, "live directory token size");

static bool
same_id(const void *a, const void *b)
{
  return pfs_bytes_compare(a, PFS_ID_SIZE, b, PFS_ID_SIZE) == 0;
}

static void
result_init(struct pfs_pool *pool, struct pfs_write_result *result)
{
  *result = (struct pfs_write_result){.completion = PFS_STOPPED,
    .health = pool->writer ? pool->writer->status.health : PFS_WRITER_READY};
}

static enum pfs_status
result_error(struct pfs_pool *pool, struct pfs_write_result *result, enum pfs_status status)
{
  result->operation_status = status;
  result->health = pool->writer ? pool->writer->status.health : PFS_WRITER_READY;
  return status;
}

static enum pfs_status
file_authorize(struct pfs_view *view, uint64_t right, struct pfs_object_record *object)
{
  struct pfs_pool *pool = view->volume->pool;
  enum pfs_status status = pfs_volume_begin(view->volume);
  if (status != PFS_OK) {
    return status;
  }
  if (!pool->writer) {
    status = PFS_READ_ONLY;
  } else if (pool->writer->status.health != PFS_WRITER_READY) {
    status = PFS_RECOVERY_REQUIRED;
  } else if (!(view->rights.file & right)) {
    status = PFS_DENIED;
  } else {
    status = pfs_volume_object(view->volume, &view->object, object);
  }
  pfs_writer_end(pool, status);
  return status;
}

static enum pfs_status
view_write(struct pfs_view *view, uint64_t offset, const void *buffer,
               size_t length, struct pfs_write_result *result)
{
  struct pfs_pool *pool = view->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  result_init(pool, result);
  struct pfs_object_record object;
  enum pfs_status status = file_authorize(view, PFS_FILE_WRITE, &object);
  if (status == PFS_OK && length && offset + length > object.file_length &&
      !(view->rights.file & PFS_FILE_RESIZE)) {
    status = PFS_DENIED;
  }
  if (status == PFS_OK && length && offset + length > PFS_FILE_SIZE_MAX) {
    status = PFS_LIMIT;
  }
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  if (!length) {
    result->completion = PFS_COMPLETE;
    return PFS_OK;
  }
  return pfs_file_write(view->volume, &view->object, offset, buffer, length, result);
}

static enum pfs_status
view_resize(struct pfs_view *view, uint64_t length, struct pfs_write_result *result)
{
  struct pfs_pool *pool = view->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  result_init(pool, result);
  struct pfs_object_record object;
  enum pfs_status status = file_authorize(view, PFS_FILE_RESIZE, &object);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  result->confirmed_length_valid = true;
  result->confirmed_length = object.file_length;
  if (length > PFS_FILE_SIZE_MAX) {
    return result_error(pool, result, PFS_LIMIT);
  }
  if (length == object.file_length) {
    result->completion = PFS_COMPLETE;
    return PFS_OK;
  }
  return pfs_file_resize(view->volume, &view->object, length, result);
}

static size_t
directory_index(struct pfs_writer *writer, const struct pfs_volume_id *volume,
                const struct pfs_object_id *object)
{
  const struct pfs_changed_directory *table = (const void *)writer->arena.directories;
  for (size_t i = 0; i < writer->changed_directories; i++) {
    if (same_id(table[i].volume.bytes, volume->bytes) &&
        same_id(table[i].object.bytes, object->bytes)) {
      return i;
    }
  }
  return writer->changed_directories;
}

static enum pfs_status
directory_live_page(struct pfs_view *view,
  const struct pfs_directory_token *after, struct pfs_view_entry *out,
  size_t capacity, size_t *count, bool *done, struct pfs_directory_token *next)
{
  struct pfs_pool *pool = view->volume->pool;
  enum pfs_status status = pfs_volume_begin(view->volume);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_writer *writer = pool->writer;
  if (!(view->rights.directory & PFS_DIR_LIST)) {
    status = PFS_DENIED;
  } else if (!writer) {
    status = PFS_READ_ONLY;
  } else if (capacity > PFS_RECORD_COUNT_MAX || capacity > SIZE_MAX / sizeof(struct pfs_dirent_record)) {
    status = PFS_LIMIT;
  }
  struct pfs_directory_token token = {0};
  uint64_t position = 0;
  if (status == PFS_OK) {
    pfs_bytes_copy(token.instance, writer->nonce, PFS_ID_SIZE);
    token.volume = view->identity.volume;
    token.directory = view->object;
    size_t index = directory_index(writer, &token.volume, &token.directory);
    const struct pfs_changed_directory *table = (const void *)writer->arena.directories;
    token.serial = index == writer->changed_directories ? 0 : table[index].serial;
    if (!pfs_bytes_are_zero((const uint8_t *)after, sizeof(*after))) {
      if (!same_id(after->instance, token.instance) ||
          !same_id(after->volume.bytes, token.volume.bytes) ||
          !same_id(after->directory.bytes, token.directory.bytes)) {
        status = PFS_INVALID;
      } else if (after->serial != token.serial) {
        status = PFS_CHANGED;
      } else {
        position = after->position;
      }
    }
  }
  struct pfs_allocation allocation = {0};
  if (status == PFS_OK) {
    status = pfs_memory_allocate(pool->memory, capacity * sizeof(struct pfs_dirent_record),
      _Alignof(struct pfs_dirent_record), &allocation);
  }
  size_t returned = 0;
  bool ended = false;
  if (status == PFS_OK) {
    status = pfs_volume_directory_page(view->volume, &view->object, position,
      allocation.data, capacity, &returned, &ended, &token.position);
  }
  if (status == PFS_OK) {
    const struct pfs_dirent_record *entries = allocation.data;
    for (size_t i = 0; i < returned; i++) {
      out[i].name = entries[i].name;
      out[i].kind = entries[i].child_kind;
    }
    *count = returned;
    *done = ended;
    *next = token;
  } else if (status == PFS_INVALID || status == PFS_CHANGED) {
    *count = 0;
  }
  pfs_memory_free(pool->memory, &allocation);
  pfs_writer_end(pool, status);
  return status;
}

/* Both retained catalogs were fully validated at admission. Re-read the bounded
 * object path so randomness collisions include the older retained state too. */
static enum pfs_status
id_present(struct pfs_writer *writer, const struct pfs_admit_state *state,
           const struct pfs_volume_id *volume, const struct pfs_object_id *id, bool *present)
{
  const struct pfs_volume_record *record = NULL;
  for (size_t i = 0; i < state->volume_count; i++) {
    if (same_id(state->volumes[i].record.id.bytes, volume->bytes)) {
      record = &state->volumes[i].record;
      break;
    }
  }
  if (!record) {
    return PFS_CORRUPT;
  }
  struct pfs_tree_context context = {.kind = PFS_INDEX_OBJECTS, .volume = *volume,
    .block = {.block_count = writer->backing->reader.geometry.block_count,
      .selected_generation = state->candidate.superblock.header.birth,
      .referring_birth = state->candidate.superblock.header.birth,
      .reference = record->object_root, .pool = state->candidate.superblock.header.pool,
      .features = record->features}};
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, id->bytes, PFS_ID_SIZE);
  for (unsigned depth = 0; depth < PFS_TREE_DEPTH_MAX; depth++) {
    uint8_t bytes[PFS_BLOCK_SIZE];
    enum pfs_status status = pfs_block_read(&writer->backing->reader,
      context.block.reference.block, 1, bytes, sizeof(bytes));
    struct pfs_tree tree;
    if (status == PFS_OK) {
      status = pfs_canonical_tree_validate(bytes, sizeof(bytes), &context, &tree);
    }
    if (status != PFS_OK) {
      return status;
    }
    struct pfs_record_context records = {.block_count = context.block.block_count,
      .selected_generation = context.block.selected_generation,
      .containing_birth = tree.header.birth, .features = record->features};
    struct pfs_reference child = {0};
    for (size_t i = 0; i < tree.count; i++) {
      const uint8_t *encoded = bytes + tree.slots[i].offset;
      if (tree.level) {
        struct pfs_internal_record entry;
        status = pfs_internal_record_decode(encoded, tree.slots[i].length,
          PFS_INDEX_OBJECTS, &records, &entry);
        if (status != PFS_OK) {
          return status;
        }
        if (pfs_key_compare(PFS_INDEX_OBJECTS, &entry.minimum, &key) > 0) {
          break;
        }
        child = entry.child;
      } else {
        struct pfs_object_record object;
        status = pfs_object_record_decode(encoded, tree.slots[i].length, &records, &object);
        if (status != PFS_OK) {
          return status;
        }
        if (same_id(object.id.bytes, id->bytes)) {
          *present = true;
          return PFS_OK;
        }
      }
    }
    if (!tree.level || !child.block) {
      *present = false;
      return PFS_OK;
    }
    context.parent_level = tree.level;
    context.block.referring_birth = tree.header.birth;
    context.block.reference = child;
  }
  return PFS_CORRUPT;
}

static enum pfs_status
new_id(struct pfs_writer *writer, const struct pfs_volume_id *volume, struct pfs_object_id *id,
       bool *entropy_failed)
{
  for (unsigned attempt = 0; attempt < 16; attempt++) {
    if (writer->random(writer->random_context, id->bytes, sizeof(id->bytes)) != PFS_OK) {
      *entropy_failed = true;
      return PFS_IO;
    }
    if (pfs_bytes_are_zero(id->bytes, sizeof(id->bytes))) {
      continue;
    }
    bool present = pfs_runtime_references(writer->pool, volume, id);
    for (size_t slot = 0; slot < 2 && !present; slot++) {
      enum pfs_status status = id_present(writer, &writer->states[slot], volume, id, &present);
      if (status != PFS_OK) {
        return status;
      }
    }
    if (!present) {
      return PFS_OK;
    }
  }
  return PFS_LIMIT;
}

struct directory_changes {
  struct pfs_object_id objects[3];
  size_t count;
};

static void
change_add(struct directory_changes *changes, const struct pfs_object_id *id)
{
  for (size_t i = 0; i < changes->count; i++) {
    if (same_id(changes->objects[i].bytes, id->bytes)) {
      return;
    }
  }
  changes->objects[changes->count++] = *id;
}

static enum pfs_status
changes_reserve(struct pfs_writer *writer, const struct pfs_volume_id *volume,
                const struct directory_changes *changes)
{
  size_t needed = 0;
  for (size_t i = 0; i < changes->count; i++) {
    needed += directory_index(writer, volume, &changes->objects[i]) == writer->changed_directories;
  }
  if (changes->count > UINT64_MAX - writer->directory_serial ||
      needed > writer->arena.limits.objects - writer->changed_directories) {
    return PFS_LIMIT;
  }
  return PFS_OK;
}

static void
changes_confirm(struct pfs_writer *writer, const struct pfs_volume_id *volume,
                const struct directory_changes *changes)
{
  struct pfs_changed_directory *table = (void *)writer->arena.directories;
  for (size_t i = 0; i < changes->count; i++) {
    size_t index = directory_index(writer, volume, &changes->objects[i]);
    table[index] = (struct pfs_changed_directory){.volume = *volume,
      .object = changes->objects[i], .serial = ++writer->directory_serial};
    writer->changed_directories += index == writer->changed_directories;
  }
}

static enum pfs_status
create_plan(struct pfs_volume *volume, struct pfs_batch *batch,
            struct pfs_object_record *parent, const struct pfs_object_record *child,
            const uint8_t *name, size_t length)
{
  struct pfs_mutation *work;
  enum pfs_status status = pfs_mutation_begin(volume, batch, &work);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_admit_volume *candidate = pfs_mutation_volume(work);
  struct pfs_record_context context = {
    .block_count = volume->pool->reader->geometry.block_count,
    .selected_generation = batch->generation, .containing_birth = batch->generation,
    .features = candidate->record.features};
  struct pfs_dirent_record entry = {.child_kind = child->kind, .object = child->id};
  entry.name.length = (uint16_t)length;
  pfs_bytes_copy(entry.name.bytes, name, length);
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_encoded_record encoded = {.data = bytes};
  status = pfs_dirent_record_encode(bytes, sizeof(bytes), &context, &entry, &encoded.length);
  struct pfs_key key = {.length = (uint16_t)length};
  pfs_bytes_copy(key.bytes, name, length);
  if (status == PFS_OK) {
    status = pfs_mutation_tree(work, PFS_INDEX_DIRECTORY, &parent->id, &parent->tree_root,
      PFS_EDIT_INSERT, &key, &encoded);
  }
  parent->directory_count++;
  parent->storage_kind = PFS_STORAGE_TREE;
  for (size_t i = 0; status == PFS_OK && i < 2; i++) {
    const struct pfs_object_record *object = i ? child : parent;
    status = pfs_object_record_encode(bytes, sizeof(bytes), &context, object, &encoded.length);
    key.length = PFS_ID_SIZE;
    pfs_bytes_copy(key.bytes, object->id.bytes, PFS_ID_SIZE);
    if (status == PFS_OK) {
      status = pfs_mutation_tree(work, PFS_INDEX_OBJECTS, NULL, &candidate->record.object_root,
        i ? PFS_EDIT_INSERT : PFS_EDIT_UPDATE, &key, &encoded);
    }
  }
  if (status == PFS_OK) {
    candidate->record.object_count++;
    candidate->directories += child->kind == PFS_OBJECT_DIRECTORY;
    status = pfs_mutation_finish(work);
  }
  return status;
}

static enum pfs_status
create_object(struct pfs_view *parent, const uint8_t *name, size_t length,
                     const struct pfs_rights *requested, struct pfs_view **out,
                     struct pfs_view_identity *identity, struct pfs_write_result *result, uint16_t kind)
{
  struct pfs_pool *pool = parent->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  result_init(pool, result);
  enum pfs_status status = pfs_volume_begin(parent->volume);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  struct pfs_writer *writer = pool->writer;
  if (!writer) {
    status = PFS_READ_ONLY;
  } else if (writer->status.health != PFS_WRITER_READY) {
    status = PFS_RECOVERY_REQUIRED;
  } else if (!(parent->rights.directory & PFS_DIR_CREATE) ||
      (requested && (parent->scope != PFS_SCOPE_SUBTREE ||
        !(parent->rights.directory & PFS_DIR_LOOKUP) ||
        (requested->file & ~parent->rights.file) ||
        (requested->directory & ~parent->rights.directory) || (requested->admin & ~parent->rights.admin)))) {
    status = PFS_DENIED;
  }
  struct pfs_object_record directory, child = {.kind = kind,
    .storage_kind = PFS_STORAGE_NONE, .parent = parent->object};
  struct pfs_view *reserved = NULL;
  struct directory_changes changes = {0};
  bool entropy_failed = false;
  if (status == PFS_OK) {
    status = pfs_volume_object(parent->volume, &parent->object, &directory);
  }
  if (status == PFS_OK) {
    struct pfs_volume_record volume;
    status = pfs_volume_metadata(parent->volume, &volume);
    if (status == PFS_OK && !same_id(parent->object.bytes, volume.root_object.bytes) &&
        pfs_bytes_are_zero(directory.parent.bytes, PFS_ID_SIZE)) {
      status = PFS_DETACHED;
    }
    if (status == PFS_OK && (volume.object_count == PFS_RECORD_COUNT_MAX ||
        directory.directory_count == PFS_RECORD_COUNT_MAX)) {
      status = PFS_LIMIT;
    }
  }
  if (status == PFS_OK) {
    struct pfs_object_record existing;
    status = pfs_volume_resolve(parent->volume, &parent->object, name, length, &existing);
    status = status == PFS_OK ? PFS_EXISTS : status == PFS_NOT_FOUND ? PFS_OK : status;
  }
  if (status == PFS_OK) {
    size_t depth;
    status = pfs_volume_ancestry(parent->volume, &parent->object, NULL, 0, &depth);
    if (status == PFS_OK && depth >= PFS_ANCESTRY_MAX) {
      status = PFS_LIMIT;
    }
  }
  if (status == PFS_OK) {
    child.owner = directory.owner;
    status = new_id(writer, &parent->identity.volume, &child.id, &entropy_failed);
  }
  if (status == PFS_OK) {
    change_add(&changes, &parent->object);
    if (kind == PFS_OBJECT_DIRECTORY) {
      change_add(&changes, &child.id);
    }
    status = changes_reserve(writer, &parent->identity.volume, &changes);
  }
  if (status == PFS_OK && requested) {
    status = pfs_view_reserve(parent, &child.id, kind, requested, PFS_SCOPE_OBJECT, &reserved);
  }
  pfs_writer_end(pool, entropy_failed ? PFS_OK : status);
  if (status != PFS_OK) {
    pfs_view_discard(reserved);
    return result_error(pool, result, status);
  }
  struct pfs_batch *batch;
  status = pfs_writer_prepare(pool, &batch);
  if (status == PFS_OK) {
    status = create_plan(parent->volume, batch, &directory, &child, name, length);
    if (status != PFS_OK) {
      pfs_writer_abort(pool);
      pfs_writer_end(pool, status);
    } else {
      status = pfs_writer_commit(pool, batch, result);
      if (result->completion == PFS_COMPLETE) {
        result->namespace_confirmed = true;
        changes_confirm(writer, &parent->identity.volume, &changes);
        if (reserved) {
          *out = reserved;
          *identity = reserved->identity;
          reserved = NULL;
        }
      }
      pfs_view_discard(reserved);
      return status;
    }
  }
  pfs_view_discard(reserved);
  return result_error(pool, result, status);
}

static enum pfs_status
create_held(struct pfs_view *parent, const uint8_t *name, size_t length,
            const struct pfs_rights *requested, struct pfs_view **out,
            struct pfs_view_identity *identity, struct pfs_write_result *result,
            uint16_t kind)
{
  if (!parent || !result || parent->identity.kind != PFS_OBJECT_DIRECTORY ||
      pfs_name_validate(name, length) != PFS_OK ||
      (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.') ||
      (requested ? (!out || *out || !identity ||
        (kind == PFS_OBJECT_FILE && requested->directory) ||
        (kind == PFS_OBJECT_DIRECTORY && requested->file) ||
        (requested->directory & ~PFS_DIR_RIGHTS_ALL) ||
        (requested->file & ~PFS_FILE_RIGHTS_ALL) || (requested->admin & ~PFS_ADMIN_RIGHTS_ALL)) :
        (out || identity))) {
    return PFS_INVALID;
  }
  struct pfs_pool *pool = parent->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(parent);
  if (status != PFS_OK) {
    result_init(pool, result);
    return result_error(pool, result, status);
  }
  status = create_object(parent, name, length, requested, out, identity, result, kind);
  pfs_view_operation_drop(parent);
  return status;
}

enum pfs_status
pfs_view_create_file(struct pfs_view *parent, const uint8_t *name, size_t length,
                     const struct pfs_rights *requested, struct pfs_view **out,
                     struct pfs_view_identity *identity, struct pfs_write_result *result)
{
  return create_held(parent, name, length, requested, out, identity, result, PFS_OBJECT_FILE);
}

enum pfs_status
pfs_view_create_directory(struct pfs_view *parent, const uint8_t *name, size_t length,
                          const struct pfs_rights *requested, struct pfs_view **out,
                          struct pfs_view_identity *identity, struct pfs_write_result *result)
{
  return create_held(parent, name, length, requested, out, identity, result, PFS_OBJECT_DIRECTORY);
}

static bool
valid_name(const uint8_t *name, size_t length)
{
  return pfs_name_validate(name, length) == PFS_OK &&
    !(length == 1 && name[0] == '.') &&
    !(length == 2 && name[0] == '.' && name[1] == '.');
}

static enum pfs_status
namespace_authorize(struct pfs_view *parent, uint64_t rights)
{
  struct pfs_writer *writer = parent->volume->pool->writer;
  if (!writer) {
    return PFS_READ_ONLY;
  }
  if (writer->status.health != PFS_WRITER_READY) {
    return PFS_RECOVERY_REQUIRED;
  }
  return (parent->rights.directory & rights) == rights ? PFS_OK : PFS_DENIED;
}

static enum pfs_status
insertion_parent(struct pfs_volume *volume, const struct pfs_object_record *parent)
{
  struct pfs_volume_record record;
  enum pfs_status status = pfs_volume_metadata(volume, &record);
  if (status == PFS_OK && !same_id(parent->id.bytes, record.root_object.bytes) &&
      pfs_bytes_are_zero(parent->parent.bytes, PFS_ID_SIZE)) {
    return PFS_DETACHED;
  }
  return status;
}

static enum pfs_status
orphan_work(struct pfs_volume *volume, const struct pfs_object_id *id, uint64_t *out)
{
  size_t grants;
  enum pfs_status status = pfs_volume_grants(volume, id, NULL, 0, &grants);
  if (status != PFS_OK) {
    return status;
  }
  uint64_t work = 1 + grants;
  const struct pfs_writer *writer = volume->pool->writer;
  const struct pfs_admit_state *state = &writer->states[writer->selected];
  struct pfs_volume_record record;
  status = pfs_volume_metadata(volume, &record);
  if (status != PFS_OK) {
    return status;
  }
  const struct pfs_volume_id *volume_id = &record.id;
  for (size_t i = 0; i < state->claim_count; i++) {
    const struct check_claim *claim = &state->claims[i];
    if (!claim->type && same_id(claim->volume.bytes, volume_id->bytes) &&
        same_id(claim->object.bytes, id->bytes)) {
      work += claim->count;
    }
  }
  *out = work;
  return PFS_OK;
}

static struct pfs_record_context
namespace_context(struct pfs_volume *volume, struct pfs_batch *batch,
                  const struct pfs_admit_volume *candidate)
{
  return (struct pfs_record_context){
    .block_count = volume->pool->reader->geometry.block_count,
    .selected_generation = batch->generation, .containing_birth = batch->generation,
    .features = candidate->record.features};
}

static enum pfs_status
namespace_object_update(struct pfs_mutation *mutation,
                        const struct pfs_record_context *context,
                        const struct pfs_object_record *object)
{
  uint8_t bytes[PFS_OBJECT_RECORD_SIZE];
  struct pfs_encoded_record encoded = {.data = bytes};
  enum pfs_status status = pfs_object_record_encode(bytes, sizeof(bytes), context,
    object, &encoded.length);
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, object->id.bytes, PFS_ID_SIZE);
  if (status == PFS_OK) {
    status = pfs_mutation_tree(mutation, PFS_INDEX_OBJECTS, NULL,
      &pfs_mutation_volume(mutation)->record.object_root, PFS_EDIT_UPDATE, &key, &encoded);
  }
  return status;
}

static enum pfs_status
namespace_directory_edit(struct pfs_mutation *mutation,
                         const struct pfs_record_context *context,
                         struct pfs_object_record *parent,
                         enum pfs_edit_operation operation,
                         const uint8_t *name, size_t length,
                         const struct pfs_object_record *child)
{
  struct pfs_key key = {.length = (uint16_t)length};
  pfs_bytes_copy(key.bytes, name, length);
  uint8_t bytes[(PFS_NAME_RECORD_PREFIX_SIZE + PFS_NAME_MAX + 7u) & ~7u];
  struct pfs_encoded_record encoded = {.data = bytes};
  enum pfs_status status = PFS_OK;
  if (operation != PFS_EDIT_DELETE) {
    struct pfs_dirent_record entry = {.child_kind = child->kind, .object = child->id};
    entry.name.length = (uint16_t)length;
    pfs_bytes_copy(entry.name.bytes, name, length);
    status = pfs_dirent_record_encode(bytes, sizeof(bytes), context, &entry, &encoded.length);
  }
  if (status == PFS_OK) {
    status = pfs_mutation_tree(mutation, PFS_INDEX_DIRECTORY, &parent->id, &parent->tree_root,
      operation, &key, operation == PFS_EDIT_DELETE ? NULL : &encoded);
  }
  if (status == PFS_OK) {
    if (operation == PFS_EDIT_DELETE) {
      parent->directory_count--;
    } else if (operation == PFS_EDIT_INSERT) {
      parent->directory_count++;
    }
    parent->storage_kind = parent->directory_count ? PFS_STORAGE_TREE : PFS_STORAGE_NONE;
  }
  return status;
}

static enum pfs_status
namespace_orphan(struct pfs_mutation *mutation,
                 const struct pfs_record_context *context,
                 struct pfs_object_record *object, uint64_t work)
{
  struct pfs_admit_volume *candidate = pfs_mutation_volume(mutation);
  uint8_t bytes[PFS_ORPHAN_RECORD_SIZE];
  struct pfs_encoded_record encoded = {.data = bytes};
  struct pfs_orphan_record orphan = {.object = object->id};
  enum pfs_status status = pfs_orphan_record_encode(bytes, sizeof(bytes), context,
    &orphan, &encoded.length);
  struct pfs_key key = {.length = PFS_ID_SIZE};
  pfs_bytes_copy(key.bytes, object->id.bytes, PFS_ID_SIZE);
  if (status == PFS_OK) {
    status = pfs_mutation_tree(mutation, PFS_INDEX_ORPHANS, NULL,
      &candidate->record.orphan_root, PFS_EDIT_INSERT, &key, &encoded);
  }
  if (status == PFS_OK) {
    object->parent = (struct pfs_object_id){0};
    status = namespace_object_update(mutation, context, object);
  }
  if (status == PFS_OK) {
    candidate->orphan_work += work;
  }
  return status;
}

static enum pfs_status
namespace_cleanup(struct pfs_view *parent, const struct pfs_object_id *victim,
                  enum pfs_status status, struct pfs_write_result *result)
{
  struct pfs_volume *volume = parent->volume;
  if (status != PFS_OK || !result->namespace_confirmed ||
      pfs_runtime_references(volume->pool, &parent->identity.volume, victim)) {
    return status;
  }
  struct pfs_write_result cleanup = {.completion = PFS_STOPPED,
    .health = volume->pool->writer->status.health};
  status = pfs_orphan_cleanup(volume, victim, &cleanup);
  result->health = cleanup.health;
  if (status != PFS_OK) {
    result->maintenance_status = cleanup.maintenance_status != PFS_OK ?
      cleanup.maintenance_status : status;
    result->maintenance_completion = cleanup.maintenance_completion == PFS_MAINTENANCE_UNKNOWN ||
      cleanup.completion == PFS_UNKNOWN ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED;
  } else {
    result->maintenance_completion = cleanup.maintenance_completion;
    result->maintenance_status = cleanup.maintenance_status;
  }
  return status;
}

static enum pfs_status
namespace_commit(struct pfs_view *parent, struct pfs_batch *batch,
                 const struct directory_changes *changes,
                 struct pfs_write_result *result)
{
  enum pfs_status status = pfs_writer_commit(parent->volume->pool, batch, result);
  if (result->completion == PFS_COMPLETE) {
    result->namespace_confirmed = true;
    changes_confirm(parent->volume->pool->writer, &parent->identity.volume, changes);
  }
  return status;
}

static enum pfs_status
namespace_failure(struct pfs_pool *pool, struct pfs_write_result *result,
                  enum pfs_status status)
{
  pfs_writer_abort(pool);
  pfs_writer_end(pool, status);
  return result_error(pool, result, status);
}

static enum pfs_status
remove_object(struct pfs_view *parent, const uint8_t *name, size_t length,
                struct pfs_write_result *result)
{
  struct pfs_pool *pool = parent->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  result_init(pool, result);
  enum pfs_status status = pfs_volume_begin(parent->volume);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  status = namespace_authorize(parent, PFS_DIR_REMOVE);
  struct pfs_object_record directory, victim;
  struct directory_changes changes = {0};
  uint64_t work = 0;
  if (status == PFS_OK) {
    status = pfs_volume_object(parent->volume, &parent->object, &directory);
  }
  if (status == PFS_OK) {
    status = pfs_volume_resolve(parent->volume, &parent->object, name, length, &victim);
  }
  if (status == PFS_OK && victim.kind == PFS_OBJECT_DIRECTORY && victim.directory_count) {
    status = PFS_NOT_EMPTY;
  }
  if (status == PFS_OK) {
    struct pfs_volume_record volume;
    status = pfs_volume_metadata(parent->volume, &volume);
    if (status == PFS_OK && same_id(victim.id.bytes, volume.root_object.bytes)) {
      status = PFS_UNSUPPORTED;
    }
  }
  if (status == PFS_OK) {
    status = orphan_work(parent->volume, &victim.id, &work);
  }
  if (status == PFS_OK) {
    change_add(&changes, &parent->object);
    if (victim.kind == PFS_OBJECT_DIRECTORY) {
      change_add(&changes, &victim.id);
    }
    status = changes_reserve(pool->writer, &parent->identity.volume, &changes);
  }
  pfs_writer_end(pool, status);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  struct pfs_batch *batch;
  status = pfs_writer_prepare(pool, &batch);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  struct pfs_mutation *mutation;
  status = pfs_mutation_begin(parent->volume, batch, &mutation);
  if (status == PFS_OK) {
    struct pfs_admit_volume *candidate = pfs_mutation_volume(mutation);
    candidate->record.features.read_required |= PFS_FEATURE_ORPHANS;
    struct pfs_record_context context = namespace_context(parent->volume, batch, candidate);
    status = namespace_directory_edit(mutation, &context, &directory,
      PFS_EDIT_DELETE, name, length, NULL);
    if (status == PFS_OK) {
      status = namespace_object_update(mutation, &context, &directory);
    }
    if (status == PFS_OK) {
      status = namespace_orphan(mutation, &context, &victim, work);
    }
    if (status == PFS_OK) {
      status = pfs_mutation_finish(mutation);
    }
  }
  if (status != PFS_OK) {
    return namespace_failure(pool, result, status);
  }
  status = namespace_commit(parent, batch, &changes, result);
  return namespace_cleanup(parent, &victim.id, status, result);
}

static enum pfs_status
rename_object(struct pfs_view *source_parent, const uint8_t *source_name,
                size_t source_length, struct pfs_view *destination_parent,
                const uint8_t *destination_name, size_t destination_length,
                bool replace, struct pfs_write_result *result)
{
  struct pfs_pool *pool = source_parent->volume->pool;
  if (pool->writer_busy || destination_parent->volume->pool->writer_busy) {
    return PFS_BUSY;
  }
  result_init(pool, result);
  enum pfs_status status = pfs_volume_begin(source_parent->volume);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  status = namespace_authorize(source_parent, PFS_DIR_REMOVE);
  if (status == PFS_OK) {
    status = namespace_authorize(destination_parent, PFS_DIR_CREATE);
  }
  if (status == PFS_OK && (pool != destination_parent->volume->pool ||
      !same_id(source_parent->identity.volume.bytes, destination_parent->identity.volume.bytes))) {
    status = PFS_UNSUPPORTED;
  }
  struct pfs_object_record source_directory, destination_directory, source, victim;
  bool same_parent = same_id(source_parent->object.bytes, destination_parent->object.bytes);
  bool same_name = same_parent && source_length == destination_length &&
    !pfs_bytes_compare(source_name, source_length, destination_name, destination_length);
  bool displaced = false;
  uint64_t work = 0;
  struct directory_changes changes = {0};
  if (status == PFS_OK) {
    status = pfs_volume_object(source_parent->volume, &source_parent->object, &source_directory);
  }
  if (status == PFS_OK) {
    status = pfs_volume_object(source_parent->volume, &destination_parent->object, &destination_directory);
  }
  if (status == PFS_OK) {
    status = insertion_parent(source_parent->volume, &destination_directory);
  }
  if (status == PFS_OK) {
    status = pfs_volume_resolve(source_parent->volume, &source_parent->object,
      source_name, source_length, &source);
  }
  if (status == PFS_OK && source.kind != PFS_OBJECT_FILE) {
    status = PFS_UNSUPPORTED;
  }
  if (status == PFS_OK && same_name) {
    result->completion = PFS_COMPLETE;
    pfs_writer_end(pool, PFS_OK);
    return PFS_OK;
  }
  if (status == PFS_OK) {
    status = pfs_volume_resolve(source_parent->volume, &destination_parent->object,
      destination_name, destination_length, &victim);
    displaced = status == PFS_OK;
    if (status == PFS_NOT_FOUND) {
      status = PFS_OK;
    } else if (status == PFS_OK && victim.kind != PFS_OBJECT_FILE) {
      status = PFS_UNSUPPORTED;
    } else if (status == PFS_OK && !replace) {
      status = PFS_EXISTS;
    } else if (status == PFS_OK && !(destination_parent->rights.directory & PFS_DIR_REPLACE)) {
      status = PFS_DENIED;
    }
  }
  if (status == PFS_OK && !same_parent) {
    size_t depth;
    status = pfs_volume_ancestry(source_parent->volume, &destination_parent->object, NULL, 0, &depth);
    if (status == PFS_OK && depth >= PFS_ANCESTRY_MAX) {
      status = PFS_LIMIT;
    }
    if (status == PFS_OK && !displaced && destination_directory.directory_count == PFS_RECORD_COUNT_MAX) {
      status = PFS_LIMIT;
    }
  }
  if (status == PFS_OK && displaced) {
    status = orphan_work(source_parent->volume, &victim.id, &work);
  }
  if (status == PFS_OK) {
    change_add(&changes, &source_parent->object);
    change_add(&changes, &destination_parent->object);
    status = changes_reserve(pool->writer, &source_parent->identity.volume, &changes);
  }
  pfs_writer_end(pool, status);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  struct pfs_batch *batch;
  status = pfs_writer_prepare(pool, &batch);
  if (status != PFS_OK) {
    return result_error(pool, result, status);
  }
  struct pfs_mutation *mutation;
  status = pfs_mutation_begin(source_parent->volume, batch, &mutation);
  if (status == PFS_OK) {
    struct pfs_admit_volume *candidate = pfs_mutation_volume(mutation);
    if (displaced) {
      candidate->record.features.read_required |= PFS_FEATURE_ORPHANS;
    }
    struct pfs_record_context context = namespace_context(source_parent->volume, batch, candidate);
    struct pfs_object_record *destination = same_parent ? &source_directory : &destination_directory;
    status = namespace_directory_edit(mutation, &context, &source_directory,
      PFS_EDIT_DELETE, source_name, source_length, NULL);
    if (status == PFS_OK) {
      status = namespace_directory_edit(mutation, &context, destination,
        displaced ? PFS_EDIT_UPDATE : PFS_EDIT_INSERT,
        destination_name, destination_length, &source);
    }
    if (status == PFS_OK) {
      status = namespace_object_update(mutation, &context, &source_directory);
    }
    if (status == PFS_OK && !same_parent) {
      status = namespace_object_update(mutation, &context, destination);
    }
    if (status == PFS_OK && !same_parent) {
      source.parent = destination_parent->object;
      status = namespace_object_update(mutation, &context, &source);
    }
    if (status == PFS_OK && displaced) {
      status = namespace_orphan(mutation, &context, &victim, work);
    }
    if (status == PFS_OK) {
      status = pfs_mutation_finish(mutation);
    }
  }
  if (status != PFS_OK) {
    return namespace_failure(pool, result, status);
  }
  status = namespace_commit(source_parent, batch, &changes, result);
  return displaced ? namespace_cleanup(source_parent, &victim.id, status, result) : status;
}

enum pfs_status
pfs_view_write(struct pfs_view *view, uint64_t offset, const void *buffer,
               size_t length, struct pfs_write_result *result)
{
  if (!view || !result || view->identity.kind != PFS_OBJECT_FILE ||
      (length && !buffer) || length > UINT64_MAX - offset) {
    return PFS_INVALID;
  }
  struct pfs_pool *pool = view->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(view);
  if (status != PFS_OK) {
    result_init(pool, result);
    return result_error(pool, result, status);
  }
  status = view_write(view, offset, buffer, length, result);
  pfs_view_operation_drop(view);
  return status;
}

enum pfs_status
pfs_view_resize(struct pfs_view *view, uint64_t length, struct pfs_write_result *result)
{
  if (!view || !result || view->identity.kind != PFS_OBJECT_FILE) {
    return PFS_INVALID;
  }
  struct pfs_pool *pool = view->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(view);
  if (status != PFS_OK) {
    result_init(pool, result);
    return result_error(pool, result, status);
  }
  status = view_resize(view, length, result);
  pfs_view_operation_drop(view);
  return status;
}

enum pfs_status
pfs_view_directory_live_page(struct pfs_view *view,
  const struct pfs_directory_token *after, struct pfs_view_entry *out,
  size_t capacity, size_t *count, bool *done, struct pfs_directory_token *next)
{
  if (!view || !after || !out || !capacity || !count || !done || !next ||
      view->identity.kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
  if (view->volume->pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(view);
  if (status != PFS_OK) {
    return status;
  }
  status = directory_live_page(view, after, out, capacity, count, done, next);
  pfs_view_operation_drop(view);
  return status;
}

enum pfs_status
pfs_view_remove(struct pfs_view *parent, const uint8_t *name, size_t length,
                struct pfs_write_result *result)
{
  if (!parent || !result || parent->identity.kind != PFS_OBJECT_DIRECTORY ||
      !valid_name(name, length)) {
    return PFS_INVALID;
  }
  struct pfs_pool *pool = parent->volume->pool;
  if (pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(parent);
  if (status != PFS_OK) {
    result_init(pool, result);
    return result_error(pool, result, status);
  }
  status = remove_object(parent, name, length, result);
  pfs_view_operation_drop(parent);
  return status;
}

enum pfs_status
pfs_view_rename(struct pfs_view *source_parent, const uint8_t *source_name,
                size_t source_length, struct pfs_view *destination_parent,
                const uint8_t *destination_name, size_t destination_length,
                bool replace, struct pfs_write_result *result)
{
  if (!source_parent || !destination_parent || !result ||
      source_parent->identity.kind != PFS_OBJECT_DIRECTORY ||
      destination_parent->identity.kind != PFS_OBJECT_DIRECTORY ||
      !valid_name(source_name, source_length) || !valid_name(destination_name, destination_length)) {
    return PFS_INVALID;
  }
  struct pfs_pool *pool = source_parent->volume->pool;
  if (pool->writer_busy || destination_parent->volume->pool->writer_busy) {
    return PFS_BUSY;
  }
  enum pfs_status status = pfs_view_operation_hold(source_parent);
  if (status != PFS_OK) {
    result_init(pool, result);
    return result_error(pool, result, status);
  }
  status = pfs_view_operation_hold(destination_parent);
  if (status == PFS_OK) {
    status = rename_object(source_parent, source_name, source_length,
      destination_parent, destination_name, destination_length, replace, result);
    pfs_view_operation_drop(destination_parent);
  } else {
    result_init(pool, result);
    result_error(pool, result, status);
  }
  pfs_view_operation_drop(source_parent);
  return status;
}
