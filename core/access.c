#include <pyxis_fs/access.h>

#include "internal.h"
#include "read_internal.h"

struct pfs_view {
  struct pfs_allocation allocation;
  struct pfs_volume *volume;
  struct pfs_object_id object;
  enum pfs_grant_scope scope;
  struct pfs_rights rights;
  struct pfs_view_identity identity;
};

struct pfs_view_directory {
  struct pfs_allocation allocation;
  struct pfs_directory_cursor cursor;
};

static bool
scope_valid(enum pfs_grant_scope scope)
{
  return scope == PFS_SCOPE_OBJECT || scope == PFS_SCOPE_SUBTREE;
}

static bool
rights_valid(const struct pfs_rights *rights)
{
  return rights != NULL && (rights->file & ~PFS_FILE_RIGHTS_ALL) == 0 &&
    (rights->directory & ~PFS_DIR_RIGHTS_ALL) == 0 &&
    (rights->admin & ~PFS_ADMIN_RIGHTS_ALL) == 0;
}

static bool
rights_contained(const struct pfs_rights *requested,
                 const struct pfs_rights *available)
{
  return (requested->file & ~available->file) == 0 &&
    (requested->directory & ~available->directory) == 0 &&
    (requested->admin & ~available->admin) == 0;
}

static bool
rights_read_only(const struct pfs_rights *rights)
{
  return (rights->file & ~(PFS_FILE_METADATA | PFS_FILE_READ)) != 0 ||
    (rights->directory & ~(PFS_DIR_METADATA | PFS_DIR_LIST | PFS_DIR_LOOKUP)) != 0 ||
    (rights->admin & ~PFS_ADMIN_INSPECT) != 0;
}

static bool
id_equal(const struct pfs_object_id *a, const struct pfs_object_id *b)
{
  return pfs_bytes_compare(a->bytes, PFS_ID_SIZE, b->bytes, PFS_ID_SIZE) == 0;
}

static enum pfs_status
request_kind_validate(uint16_t kind, enum pfs_grant_scope scope)
{
  if (scope == PFS_SCOPE_SUBTREE) {
    return kind == PFS_OBJECT_DIRECTORY ? PFS_OK : PFS_INVALID;
  }
  return PFS_OK;
}

static void
rights_union_grant(struct pfs_rights *rights,
                   const struct pfs_grant_record *grant)
{
  rights->file |= grant->file_rights;
  rights->directory |= grant->directory_rights;
  rights->admin |= grant->admin_rights;
}

static enum pfs_status
chain_allocate(struct pfs_volume *volume, const struct pfs_object_id *target,
               struct pfs_allocation *allocation, size_t *count)
{
  enum pfs_status status = pfs_volume_diagnostic_ancestry(volume, target, NULL,
                                                        0, count);
  if (status != PFS_OK) {
    return status;
  }
  if (*count == 0 || *count > PFS_ANCESTRY_MAX ||
      *count > SIZE_MAX / sizeof(struct pfs_object_record)) {
    return PFS_CORRUPT;
  }
  status = pfs_memory_allocate(volume->pool->memory,
                              *count * sizeof(struct pfs_object_record),
                              _Alignof(struct pfs_object_record), allocation);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_diagnostic_ancestry(volume, target, allocation->data,
                                         *count, count);
  if (status != PFS_OK) {
    pfs_memory_free(volume->pool->memory, allocation);
  }
  return status;
}

static enum pfs_status
access_evaluate(struct pfs_volume *volume,
                    const struct pfs_trusted_context *context,
                    const struct pfs_object_id *target,
                    enum pfs_grant_scope scope,
                    const struct pfs_rights *requested,
                    struct pfs_access_result *result)
{
  if (volume == NULL || context == NULL || target == NULL || result == NULL ||
      !scope_valid(scope) || !scope_valid(context->scope) ||
      !rights_valid(requested) || !rights_valid(&context->ceiling) ||
      pfs_bytes_are_zero(context->principal.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(context->root.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(target->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }

  struct pfs_allocation allocation = {0};
  struct pfs_allocation grant_allocation = {0};
  size_t count;
  enum pfs_status status = chain_allocate(volume, target, &allocation, &count);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_object_record *chain = allocation.data;
  size_t root = count;
  for (size_t i = 0; i < count; i++) {
    if (id_equal(&chain[i].id, &context->root)) {
      root = i;
      break;
    }
  }

  struct pfs_access_result evaluated = {0};
  if (root == count || (context->scope == PFS_SCOPE_OBJECT &&
                       (root != count - 1 || scope != PFS_SCOPE_OBJECT))) {
    status = PFS_DENIED;
    goto done;
  }
  if (context->scope == PFS_SCOPE_SUBTREE &&
      chain[root].kind != PFS_OBJECT_DIRECTORY) {
    status = PFS_INVALID;
    goto done;
  }
  status = request_kind_validate(chain[count - 1].kind, scope);
  if (status != PFS_OK) {
    goto done;
  }

  status = pfs_memory_allocate(volume->pool->memory,
                              PFS_OBJECT_GRANTS_MAX * sizeof(struct pfs_grant_record),
                              _Alignof(struct pfs_grant_record),
                              &grant_allocation);
  if (status != PFS_OK) {
    goto done;
  }
  struct pfs_grant_record *grants = grant_allocation.data;
  struct pfs_rights inherited = {0};
  bool lookup_allowed = true;
  for (size_t i = 0; i < count; i++) {
    size_t grant_count;
    status = pfs_volume_diagnostic_grants(volume, &chain[i].id, grants,
                                         PFS_OBJECT_GRANTS_MAX, &grant_count);
    if (status != PFS_OK) {
      goto done;
    }
    struct pfs_rights object_only = {0};
    for (size_t j = 0; j < grant_count; j++) {
      if (pfs_bytes_compare(grants[j].principal.bytes, PFS_ID_SIZE,
                            context->principal.bytes, PFS_ID_SIZE) != 0) {
        continue;
      }
      if (grants[j].scope == PFS_SCOPE_SUBTREE) {
        rights_union_grant(&inherited, &grants[j]);
      } else {
        rights_union_grant(&object_only, &grants[j]);
      }
    }
    if (i >= root && i < count - 1 &&
        (((inherited.directory | object_only.directory) &
          context->ceiling.directory & PFS_DIR_LOOKUP) == 0)) {
      lookup_allowed = false;
    }
    if (i == count - 1) {
      pfs_bytes_copy(&evaluated.effective, &inherited, sizeof(inherited));
      if (scope == PFS_SCOPE_OBJECT) {
        evaluated.effective.file |= object_only.file;
        evaluated.effective.directory |= object_only.directory;
        evaluated.effective.admin |= object_only.admin;
        if (chain[i].kind == PFS_OBJECT_FILE) {
          evaluated.effective.directory = 0;
        } else {
          evaluated.effective.file = 0;
        }
      }
    }
  }
  evaluated.effective.file &= context->ceiling.file;
  evaluated.effective.directory &= context->ceiling.directory;
  evaluated.effective.admin &= context->ceiling.admin;
  evaluated.allowed = lookup_allowed &&
    rights_contained(requested, &evaluated.effective);
  status = evaluated.allowed ?
    (rights_read_only(requested) ? PFS_READ_ONLY : PFS_OK) : PFS_DENIED;

done:
  pfs_memory_free(volume->pool->memory, &grant_allocation);
  pfs_memory_free(volume->pool->memory, &allocation);
  if (status == PFS_OK || status == PFS_DENIED || status == PFS_READ_ONLY) {
    pfs_bytes_copy(result, &evaluated, sizeof(evaluated));
  }
  return status;
}

enum pfs_status
pfs_access_evaluate(struct pfs_volume *volume,
                    const struct pfs_trusted_context *context,
                    const struct pfs_object_id *target,
                    enum pfs_grant_scope scope,
                    const struct pfs_rights *requested,
                    struct pfs_access_result *result)
{
  if (volume == NULL || context == NULL || target == NULL || result == NULL ||
      !scope_valid(scope) || !scope_valid(context->scope) ||
      !rights_valid(requested) || !rights_valid(&context->ceiling) ||
      pfs_bytes_are_zero(context->principal.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(context->root.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(target->bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  if (!id_equal(target, &context->root)) {
    struct pfs_access_result root_result = {0};
    enum pfs_status status = PFS_DENIED;
    if (context->scope == PFS_SCOPE_SUBTREE) {
      const struct pfs_rights lookup = {.directory = PFS_DIR_LOOKUP};
      status = access_evaluate(volume, context, &context->root,
                                PFS_SCOPE_OBJECT, &lookup, &root_result);
    }
    if (status != PFS_OK) {
      if (status == PFS_DENIED) {
        pfs_bytes_zero(result, sizeof(*result));
      }
      return status;
    }
  }
  return access_evaluate(volume, context, target, scope, requested, result);
}

static enum pfs_status
view_create(struct pfs_volume *volume, const struct pfs_object_record *object,
            enum pfs_grant_scope scope, const struct pfs_rights *rights,
            struct pfs_view **out)
{
  struct pfs_volume_record metadata;
  enum pfs_status status = pfs_volume_diagnostic_metadata(volume, &metadata);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_allocation allocation = {0};
  status = pfs_memory_allocate(volume->pool->memory, sizeof(struct pfs_view),
                              _Alignof(struct pfs_view), &allocation);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_hold(volume);
  if (status != PFS_OK) {
    pfs_memory_free(volume->pool->memory, &allocation);
    return status;
  }
  struct pfs_view *view = allocation.data;
  pfs_bytes_zero(view, sizeof(*view));
  status = pfs_memory_move(volume->pool->memory, &allocation, &view->allocation);
  if (status != PFS_OK) {
    pfs_volume_drop(volume);
    pfs_memory_free(volume->pool->memory, &allocation);
    return status;
  }
  view->volume = volume;
  pfs_bytes_copy(&view->object, &object->id, sizeof(object->id));
  view->scope = scope;
  pfs_bytes_copy(&view->rights, rights, sizeof(*rights));
  pfs_bytes_copy(&view->identity.pool, pfs_volume_pool_id(volume),
                 sizeof(view->identity.pool));
  pfs_bytes_copy(&view->identity.volume, &metadata.id, sizeof(metadata.id));
  pfs_bytes_copy(&view->identity.object, &object->id, sizeof(object->id));
  view->identity.generation = pfs_volume_generation(volume);
  view->identity.kind = object->kind;
  *out = view;
  return PFS_OK;
}

enum pfs_status
pfs_view_acquire(struct pfs_volume *volume,
                 const struct pfs_trusted_context *context,
                 const struct pfs_object_id *target, enum pfs_grant_scope scope,
                 const struct pfs_rights *requested, struct pfs_view **out)
{
  if (out == NULL || *out != NULL) {
    return PFS_INVALID;
  }
  struct pfs_access_result result;
  enum pfs_status status = pfs_access_evaluate(volume, context, target, scope,
                                             requested, &result);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_object_record object;
  status = pfs_volume_diagnostic_object(volume, target, &object);
  if (status != PFS_OK) {
    return status;
  }
  return view_create(volume, &object, scope, requested, out);
}

static enum pfs_status
path_validate(const uint8_t *path, size_t length)
{
  if (path == NULL && length != 0) {
    return PFS_INVALID;
  }
  size_t offset = 0;
  size_t components = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && path[end] != '/') {
      end++;
    }
    size_t component_length = end - offset;
    if (pfs_name_validate(path + offset, component_length) != PFS_OK ||
        (component_length == 1 && path[offset] == '.') ||
        (component_length == 2 && path[offset] == '.' && path[offset + 1] == '.') ||
        (end < length && end + 1 == length)) {
      return PFS_INVALID;
    }
    if (++components >= PFS_ANCESTRY_MAX) {
      return PFS_LIMIT;
    }
    offset = end < length ? end + 1 : end;
  }
  return PFS_OK;
}

enum pfs_status
pfs_view_acquire_path(struct pfs_volume *volume,
                      const struct pfs_trusted_context *context,
                      const uint8_t *path, size_t length,
                      enum pfs_grant_scope scope,
                      const struct pfs_rights *requested, struct pfs_view **out)
{
  if (volume == NULL || context == NULL || out == NULL || *out != NULL ||
      !scope_valid(scope) || !scope_valid(context->scope) ||
      !rights_valid(requested) || !rights_valid(&context->ceiling) ||
      pfs_bytes_are_zero(context->principal.bytes, PFS_ID_SIZE) ||
      pfs_bytes_are_zero(context->root.bytes, PFS_ID_SIZE)) {
    return PFS_INVALID;
  }
  enum pfs_status status = path_validate(path, length);
  if (status != PFS_OK) {
    return status;
  }
  if (length != 0 && context->scope == PFS_SCOPE_OBJECT) {
    return PFS_DENIED;
  }

  struct pfs_object_id current;
  pfs_bytes_copy(&current, &context->root, sizeof(current));
  const struct pfs_rights lookup = {.directory = PFS_DIR_LOOKUP};
  size_t offset = 0;
  while (offset < length) {
    /* Authorize the containing directory before consulting its child names.
     * Keep the original root/ceiling so object-only lookup grants apply without
     * turning them into retained descendant authority. */
    struct pfs_access_result result;
    status = pfs_access_evaluate(volume, context, &current, PFS_SCOPE_OBJECT,
                                 &lookup, &result);
    if (status != PFS_OK) {
      return status;
    }
    size_t end = offset;
    while (end < length && path[end] != '/') {
      end++;
    }
    struct pfs_object_record object;
    status = pfs_volume_diagnostic_resolve(volume, &current, path + offset,
                                           end - offset, &object);
    if (status != PFS_OK) {
      return status;
    }
    pfs_bytes_copy(&current, &object.id, sizeof(current));
    offset = end < length ? end + 1 : end;
  }
  return pfs_view_acquire(volume, context, &current, scope, requested, out);
}

enum pfs_status
pfs_view_close(struct pfs_view **view)
{
  if (view == NULL) {
    return PFS_INVALID;
  }
  if (*view == NULL) {
    return PFS_OK;
  }
  struct pfs_volume *volume = (*view)->volume;
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_move(volume->pool->memory,
                                          &(*view)->allocation, &allocation);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_drop(volume);
  if (status != PFS_OK) {
    pfs_memory_move(volume->pool->memory, &allocation, &(*view)->allocation);
    return status;
  }
  status = pfs_memory_free(volume->pool->memory, &allocation);
  if (status == PFS_OK) {
    *view = NULL;
  }
  return status;
}

enum pfs_status
pfs_view_metadata(struct pfs_view *view, struct pfs_view_metadata *out)
{
  if (view == NULL || out == NULL) {
    return PFS_INVALID;
  }
  uint64_t mask = view->identity.kind == PFS_OBJECT_FILE ?
    view->rights.file & PFS_FILE_METADATA :
    view->rights.directory & PFS_DIR_METADATA;
  if (mask == 0) {
    return PFS_DENIED;
  }
  struct pfs_object_record object;
  enum pfs_status status = pfs_volume_diagnostic_object(view->volume,
                                                       &view->object, &object);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_view_metadata metadata;
  pfs_bytes_zero(&metadata, sizeof(metadata));
  pfs_bytes_copy(&metadata.identity, &view->identity, sizeof(view->identity));
  metadata.size = object.kind == PFS_OBJECT_FILE ?
    object.file_length : object.directory_count;
  pfs_bytes_copy(out, &metadata, sizeof(metadata));
  return PFS_OK;
}

enum pfs_status
pfs_view_inspect(struct pfs_view *view, struct pfs_principal_id *owner,
                 struct pfs_grant_record *grants, size_t capacity, size_t *count)
{
  if (view == NULL || owner == NULL || count == NULL ||
      (capacity != 0 && grants == NULL)) {
    return PFS_INVALID;
  }
  if ((view->rights.admin & PFS_ADMIN_INSPECT) == 0) {
    return PFS_DENIED;
  }
  struct pfs_object_record object;
  enum pfs_status status = pfs_volume_diagnostic_object(view->volume,
                                                       &view->object, &object);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_volume_diagnostic_grants(view->volume, &view->object,
                                       grants, capacity, count);
  if (status == PFS_OK) {
    pfs_bytes_copy(owner, &object.owner, sizeof(object.owner));
  }
  return status;
}

enum pfs_status
pfs_view_read(struct pfs_view *view, uint64_t offset, void *buffer, size_t length,
              size_t *read_count)
{
  if (view == NULL || read_count == NULL || (length != 0 && buffer == NULL)) {
    return PFS_INVALID;
  }
  if (view->identity.kind != PFS_OBJECT_FILE) {
    return PFS_INVALID;
  }
  *read_count = 0;
  if ((view->rights.file & PFS_FILE_READ) == 0) {
    return PFS_DENIED;
  }
  return pfs_volume_diagnostic_read(view->volume, &view->object, offset,
                                     buffer, length, read_count);
}

enum pfs_status
pfs_view_directory_open(struct pfs_view *view, struct pfs_view_directory **out)
{
  if (view == NULL || out == NULL || *out != NULL) {
    return PFS_INVALID;
  }
  if (view->identity.kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
  if ((view->rights.directory & PFS_DIR_LIST) == 0) {
    return PFS_DENIED;
  }
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(view->volume->pool->memory,
                                              sizeof(struct pfs_view_directory),
                                              _Alignof(struct pfs_view_directory),
                                              &allocation);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_view_directory *cursor = allocation.data;
  pfs_bytes_zero(cursor, sizeof(*cursor));
  status = pfs_volume_diagnostic_directory_open(view->volume, &view->object,
                                                &cursor->cursor);
  if (status != PFS_OK) {
    pfs_memory_free(view->volume->pool->memory, &allocation);
    return status;
  }
  status = pfs_memory_move(view->volume->pool->memory,
                           &allocation, &cursor->allocation);
  if (status != PFS_OK) {
    pfs_directory_close(&cursor->cursor);
    pfs_memory_free(view->volume->pool->memory, &allocation);
    return status;
  }
  *out = cursor;
  return PFS_OK;
}

enum pfs_status
pfs_view_directory_next(struct pfs_view_directory *cursor,
                        struct pfs_view_entry *out, size_t capacity,
                        size_t *count, bool *done)
{
  if (cursor == NULL || out == NULL || capacity == 0 || count == NULL ||
      done == NULL || capacity > SIZE_MAX / sizeof(struct pfs_dirent_record)) {
    return PFS_INVALID;
  }
  struct pfs_memory *memory = cursor->cursor.volume->pool->memory;
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_allocate(memory,
                                              capacity * sizeof(struct pfs_dirent_record),
                                              _Alignof(struct pfs_dirent_record),
                                              &allocation);
  if (status != PFS_OK) {
    return status;
  }
  size_t returned;
  bool ended;
  status = pfs_directory_next(&cursor->cursor, allocation.data, capacity,
                              &returned, &ended);
  if (status == PFS_OK) {
    struct pfs_dirent_record *entries = allocation.data;
    for (size_t i = 0; i < returned; i++) {
      pfs_bytes_copy(&out[i].name, &entries[i].name, sizeof(entries[i].name));
      out[i].kind = entries[i].child_kind;
    }
    *count = returned;
    *done = ended;
  }
  pfs_memory_free(memory, &allocation);
  return status;
}

enum pfs_status
pfs_view_directory_close(struct pfs_view_directory **cursor)
{
  if (cursor == NULL) {
    return PFS_INVALID;
  }
  if (*cursor == NULL) {
    return PFS_OK;
  }
  struct pfs_memory *memory = (*cursor)->cursor.volume->pool->memory;
  struct pfs_allocation allocation = {0};
  enum pfs_status status = pfs_memory_move(memory, &(*cursor)->allocation,
                                          &allocation);
  if (status != PFS_OK) {
    return status;
  }
  status = pfs_directory_close(&(*cursor)->cursor);
  if (status != PFS_OK) {
    pfs_memory_move(memory, &allocation, &(*cursor)->allocation);
    return status;
  }
  status = pfs_memory_free(memory, &allocation);
  if (status == PFS_OK) {
    *cursor = NULL;
  }
  return status;
}

enum pfs_status
pfs_view_lookup(struct pfs_view *view, const uint8_t *path, size_t length,
                enum pfs_grant_scope scope, const struct pfs_rights *requested,
                struct pfs_view **out, struct pfs_view_identity *identity)
{
  if (view == NULL || path == NULL || length == 0 || out == NULL ||
      *out != NULL || identity == NULL || !scope_valid(scope) ||
      !rights_valid(requested)) {
    return PFS_INVALID;
  }
  if (view->scope != PFS_SCOPE_SUBTREE ||
      (view->rights.directory & PFS_DIR_LOOKUP) == 0 ||
      !rights_contained(requested, &view->rights)) {
    return PFS_DENIED;
  }
  struct pfs_object_record object;
  enum pfs_status status = pfs_volume_diagnostic_resolve(view->volume,
                                                        &view->object, path,
                                                        length, &object);
  if (status != PFS_OK) {
    return status;
  }
  if (id_equal(&object.id, &view->object)) {
    return PFS_INVALID;
  }
  status = request_kind_validate(object.kind, scope);
  if (status != PFS_OK) {
    return status;
  }
  if (scope == PFS_SCOPE_OBJECT &&
      ((object.kind == PFS_OBJECT_FILE && requested->directory != 0) ||
       (object.kind == PFS_OBJECT_DIRECTORY && requested->file != 0))) {
    return PFS_DENIED;
  }
  if (rights_read_only(requested)) {
    return PFS_READ_ONLY;
  }
  status = view_create(view->volume, &object, scope, requested, out);
  if (status == PFS_OK) {
    pfs_bytes_copy(identity, &(*out)->identity, sizeof(*identity));
  }
  return status;
}
