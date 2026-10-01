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

enum pfs_status
pfs_view_directory_live_page(struct pfs_view *view,
  const struct pfs_directory_token *after, struct pfs_view_entry *out,
  size_t capacity, size_t *count, bool *done, struct pfs_directory_token *next)
{
  if (!view || !after || !out || !capacity || !count || !done || !next ||
      view->identity.kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
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
    bool present = false;
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

static enum pfs_status
reserve_child(struct pfs_view *parent, const struct pfs_object_id *id,
              const struct pfs_rights *rights, struct pfs_view **out)
{
  struct pfs_memory *memory = parent->volume->pool->memory;
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(memory, sizeof(struct pfs_view),
    _Alignof(struct pfs_view), &allocation);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_hold(parent->volume);
  if (status != PFS_OK) {
    pfs_memory_free(memory, &allocation);
    return status;
  }
  struct pfs_view *view = allocation.data;
  *view = (struct pfs_view){.volume = parent->volume, .object = *id,
    .scope = PFS_SCOPE_OBJECT, .rights = *rights,
    .identity = {.pool = parent->identity.pool, .volume = parent->identity.volume,
      .object = *id, .kind = PFS_OBJECT_FILE}};
  pfs_memory_move(memory, &allocation, &view->allocation);
  *out = view;
  return PFS_OK;
}

static void
discard_child(struct pfs_view *view)
{
  if (view) {
    struct pfs_pool *pool = view->volume->pool;
    struct pfs_memory *memory = pool->memory;
    struct pfs_allocation allocation = {0};
    /* Publication/abort released the gate; guard allocation callbacks during
     * release of the private, never-published view as ordinary close does. */
    pfs_writer_lifetime_begin(pool);
    pfs_volume_drop(view->volume);
    pfs_memory_move(memory, &view->allocation, &allocation);
    pfs_memory_free(memory, &allocation);
    pfs_writer_end(pool, PFS_OK);
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
  struct pfs_dirent_record entry = {.child_kind = PFS_OBJECT_FILE, .object = child->id};
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
    status = pfs_mutation_finish(work);
  }
  return status;
}

enum pfs_status
pfs_view_create_file(struct pfs_view *parent, const uint8_t *name, size_t length,
                     const struct pfs_rights *requested, struct pfs_view **out,
                     struct pfs_view_identity *identity, struct pfs_write_result *result)
{
  if (!parent || !result || parent->identity.kind != PFS_OBJECT_DIRECTORY ||
      pfs_name_validate(name, length) != PFS_OK ||
      (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.') ||
      (requested ? (!out || *out || !identity || requested->directory ||
        (requested->file & ~PFS_FILE_RIGHTS_ALL) || (requested->admin & ~PFS_ADMIN_RIGHTS_ALL)) :
        (out || identity))) {
    return PFS_INVALID;
  }
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
        (requested->file & ~parent->rights.file) || (requested->admin & ~parent->rights.admin)))) {
    status = PFS_DENIED;
  }
  struct pfs_object_record directory, child = {.kind = PFS_OBJECT_FILE,
    .storage_kind = PFS_STORAGE_NONE, .parent = parent->object};
  struct pfs_view *reserved = NULL;
  size_t changed = 0;
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
    changed = directory_index(writer, &parent->identity.volume, &parent->object);
    if (writer->directory_serial == UINT64_MAX ||
        (changed == writer->changed_directories && changed == writer->arena.limits.objects)) {
      status = PFS_LIMIT;
    }
  }
  if (status == PFS_OK) {
    child.owner = directory.owner;
    status = new_id(writer, &parent->identity.volume, &child.id, &entropy_failed);
  }
  if (status == PFS_OK && requested) {
    status = reserve_child(parent, &child.id, requested, &reserved);
  }
  pfs_writer_end(pool, entropy_failed ? PFS_OK : status);
  if (status != PFS_OK) {
    discard_child(reserved);
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
        struct pfs_changed_directory *table = (void *)writer->arena.directories;
        table[changed] = (struct pfs_changed_directory){.volume = parent->identity.volume,
          .object = parent->object, .serial = ++writer->directory_serial};
        writer->changed_directories += changed == writer->changed_directories;
        if (reserved) {
          *out = reserved;
          *identity = reserved->identity;
          reserved = NULL;
        }
      }
      discard_child(reserved);
      return status;
    }
  }
  discard_child(reserved);
  return result_error(pool, result, status);
}
