/* SPDX-License-Identifier: MPL-2.0 */
#include "inspect.h"
#include "extract.h"

#include <string.h>

static const char *
object_kind(uint16_t kind)
{
  return kind == PFS_OBJECT_FILE ? "file" : "directory";
}

static enum pfs_status
select_volume(struct pfs_pool *pool, const struct inspect_options *options,
               uint64_t volume_count, struct pfs_volume *volume)
{
  if (options->volume_id_set) {
    return pfs_pool_diagnostic_volume_open(pool, &options->volume_id, volume);
  }
  struct pfs_allocation records = {0};
  enum pfs_status status = pfs_memory_allocate(pool->memory,
    (size_t)volume_count * sizeof(struct pfs_volume_record),
    _Alignof(struct pfs_volume_record), &records);
  if (status != PFS_OK) {
    return status;
  }
  size_t count = 0;
  status = pfs_pool_diagnostic_volumes(pool, records.data, (size_t)volume_count, &count);
  struct pfs_volume_id id = {0};
  if (status == PFS_OK) {
    status = PFS_NOT_FOUND;
    const struct pfs_volume_record *volumes = records.data;
    size_t length = strlen(options->volume_name);
    for (size_t i = 0; i < count; ++i) {
      if (volumes[i].name.length == length &&
          memcmp(volumes[i].name.bytes, options->volume_name, length) == 0) {
        id = volumes[i].id;
        status = PFS_OK;
        break;
      }
    }
  }
  pfs_memory_free(pool->memory, &records);
  return status == PFS_OK ? pfs_pool_diagnostic_volume_open(pool, &id, volume) : status;
}

static enum pfs_status
resolve(struct pfs_volume *volume, const struct pfs_object_id *root,
         const char *path, struct pfs_object_record *object)
{
  size_t length = strcmp(path, ".") == 0 ? 0 : strlen(path);
  return pfs_volume_diagnostic_resolve(volume, root, (const uint8_t *)path, length, object);
}

static enum pfs_status
print_stat(struct pfs_volume *volume, const struct pfs_object_record *object)
{
  struct pfs_allocation grants = {0};
  enum pfs_status status = pfs_memory_allocate(volume->pool->memory,
    PFS_OBJECT_GRANTS_MAX * sizeof(struct pfs_grant_record),
    _Alignof(struct pfs_grant_record), &grants);
  if (status != PFS_OK) {
    return status;
  }
  size_t count = 0;
  status = pfs_volume_diagnostic_grants(volume, &object->id, grants.data,
                                       PFS_OBJECT_GRANTS_MAX, &count);
  if (status == PFS_OK) {
    char id[PFS_ID_TEXT_SIZE + 1];
    char owner[PFS_ID_TEXT_SIZE + 1];
    pfs_object_id_format(&object->id, id);
    pfs_principal_id_format(&object->owner, owner);
    printf("Object %s: %s, owner=%s\n", id, object_kind(object->kind), owner);
    if (object->kind == PFS_OBJECT_FILE) {
      printf("File bytes: %llu\n", (unsigned long long)object->file_length);
    } else {
      printf("Directory entries: %llu\n", (unsigned long long)object->directory_count);
    }
    printf("Explicit grants: %zu\n", count);
    const struct pfs_grant_record *records = grants.data;
    for (size_t i = 0; i < count; ++i) {
      pfs_principal_id_format(&records[i].principal, owner);
      printf("  principal=%s scope=%s file=0x%llx directory=0x%llx admin=0x%llx\n",
             owner, records[i].scope == PFS_SCOPE_SUBTREE ? "subtree" : "object",
             (unsigned long long)records[i].file_rights,
             (unsigned long long)records[i].directory_rights,
             (unsigned long long)records[i].admin_rights);
    }
  }
  pfs_memory_free(volume->pool->memory, &grants);
  return status;
}

static enum pfs_status
print_list(struct pfs_volume *volume, const struct pfs_object_record *object)
{
  struct pfs_directory_cursor cursor = {0};
  enum pfs_status status = pfs_volume_diagnostic_directory_open(volume, &object->id, &cursor);
  if (status != PFS_OK) {
    return status;
  }
  enum { PAGE_ENTRIES = 16 };
  struct pfs_allocation page = {0};
  status = pfs_memory_allocate(volume->pool->memory,
    PAGE_ENTRIES * sizeof(struct pfs_dirent_record), _Alignof(struct pfs_dirent_record), &page);
  bool done = false;
  size_t published = 0;
  while (status == PFS_OK && !done) {
    size_t count = 0;
    status = pfs_directory_next(&cursor, page.data, PAGE_ENTRIES, &count, &done);
    if (status != PFS_OK) {
      break;
    }
    const struct pfs_dirent_record *entries = page.data;
    for (size_t i = 0; i < count; ++i) {
      char id[PFS_ID_TEXT_SIZE + 1];
      pfs_object_id_format(&entries[i].object, id);
      host_name_print(stdout, entries[i].name.bytes, entries[i].name.length);
      printf(" %s id=%s\n", object_kind(entries[i].child_kind), id);
    }
    published += count;
    if (fflush(stdout) != 0) {
      status = PFS_IO;
    }
  }
  if (status == PFS_OK && published == 0) {
    fputs("Directory is empty.\n", stdout);
  } else if (status != PFS_OK && published != 0) {
    fprintf(stderr, "list: partial output (%zu entries)\n", published);
  }
  pfs_memory_free(volume->pool->memory, &page);
  enum pfs_status close_status = pfs_directory_close(&cursor);
  return status != PFS_OK ? status : close_status;
}

static enum pfs_status
print_access(struct pfs_volume *volume, const struct pfs_volume_record *metadata,
              const struct inspect_options *options)
{
  struct pfs_object_record root;
  enum pfs_status status = resolve(volume, &metadata->root_object, options->root, &root);
  if (status != PFS_OK) {
    return status;
  }
  if (root.kind != PFS_OBJECT_DIRECTORY) {
    return PFS_INVALID;
  }
  struct pfs_object_record target;
  status = resolve(volume, &root.id, options->target, &target);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_trusted_context context = {
    .principal = options->principal,
    .root = root.id,
    .scope = PFS_SCOPE_SUBTREE,
    .ceiling = options->ceiling,
  };
  struct pfs_access_result result = {0};
  status = pfs_access_evaluate(volume, &context, &target.id,
                              options->scope, &options->rights, &result);
  if (status != PFS_OK && status != PFS_DENIED && status != PFS_READ_ONLY) {
    return status;
  }
  printf("Policy simulation: %s\n", result.allowed ? "allowed" : "denied");
  printf("Effective rights within ceiling: file=0x%llx directory=0x%llx admin=0x%llx\n",
         (unsigned long long)result.effective.file,
         (unsigned long long)result.effective.directory,
         (unsigned long long)result.effective.admin);
  if (status == PFS_READ_ONLY) {
    fputs("Requested operations: read only; no view acquired.\n", stdout);
  } else if (status == PFS_OK) {
    struct pfs_view *view = NULL;
    status = pfs_view_acquire(volume, &context, &target.id,
                             options->scope, &options->rights, &view);
    if (status == PFS_OK) {
      fputs("Requested view acquired and released.\n", stdout);
      status = pfs_view_close(&view);
    }
  }
  return status;
}

enum pfs_status
inspect_objects(struct pfs_pool *pool, const struct inspect_options *options,
                 uint64_t volume_count)
{
  struct pfs_volume volume = {0};
  enum pfs_status status = select_volume(pool, options, volume_count, &volume);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_volume_record metadata;
  status = pfs_volume_diagnostic_metadata(&volume, &metadata);
  if (status == PFS_OK) {
    fputs("Volume ", stdout);
    host_name_print(stdout, metadata.name.bytes, metadata.name.length);
    fputc('\n', stdout);
    if (options->command == INSPECT_ACCESS) {
      status = print_access(&volume, &metadata, options);
    } else {
      struct pfs_object_record object;
      status = resolve(&volume, &metadata.root_object, options->path, &object);
      if (status == PFS_OK) {
        if (options->command == INSPECT_EXTRACT) {
          status = host_extract(&volume, &object, options->output);
        } else {
          status = options->command == INSPECT_STAT ? print_stat(&volume, &object) :
                                                     print_list(&volume, &object);
        }
      }
    }
  }
  enum pfs_status close_status = pfs_volume_close(&volume);
  return status != PFS_OK ? status : close_status;
}
