/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "source.h"
#include <pyxis_fs/read.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct source_volume {
  int root_fd;
  size_t count;
  size_t capacity;
  struct pfs_allocation objects;
  struct pfs_allocation identities;
  struct pfs_allocation id_slots;
};

struct source_frame {
  int fd;
  uint32_t object;
  size_t used;
  size_t next;
  _Alignas(struct dirent64) uint8_t entries[4096];
};

struct source_workspace {
  struct source_frame frames[PFS_ANCESTRY_MAX];
  uint32_t chain[PFS_ANCESTRY_MAX];
};

static enum pfs_status
source_error(struct host_source *source, const char *operation, int error_number,
              size_t volume, size_t object, enum pfs_status status)
{
  source->operation = operation;
  source->error_number = error_number;
  source->error_volume = volume;
  source->error_object = object;
  return status;
}

static bool
same_identity(const struct stat *a, const struct stat *b)
{
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool
unchanged(const struct stat *a, const struct stat *b)
{
  return same_identity(a, b) && (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT) &&
    a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
    a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
    a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static size_t
id_slot(const struct pfs_object_id *id, size_t capacity)
{
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < PFS_ID_SIZE; ++i) {
    hash = (hash ^ id->bytes[i]) * UINT64_C(1099511628211);
  }
  return (size_t)hash & (capacity - 1);
}

static enum pfs_status
manifest_grow(struct host_source *source, struct source_volume *volume)
{
  if (volume->count < volume->capacity) {
    return PFS_OK;
  }
  size_t capacity = volume->capacity ? volume->capacity * 2 : 64;
  if (capacity > PFS_RECORD_COUNT_MAX) {
    return PFS_LIMIT;
  }
  struct pfs_allocation objects = {0};
  struct pfs_allocation identities = {0};
  struct pfs_allocation slots = {0};
  enum pfs_status status = pfs_memory_allocate(source->memory,
    capacity * sizeof(struct pfs_build_object), _Alignof(struct pfs_build_object), &objects);
  if (status == PFS_OK) {
    status = pfs_memory_allocate(source->memory, capacity * sizeof(struct stat),
      _Alignof(struct stat), &identities);
  }
  if (status == PFS_OK) {
    status = pfs_memory_allocate(source->memory, capacity * 2 * sizeof(uint32_t),
      _Alignof(uint32_t), &slots);
  }
  if (status != PFS_OK) {
    pfs_memory_free(source->memory, &objects);
    pfs_memory_free(source->memory, &identities);
    pfs_memory_free(source->memory, &slots);
    return status;
  }
  if (volume->count) {
    memcpy(objects.data, volume->objects.data, volume->count * sizeof(struct pfs_build_object));
    memcpy(identities.data, volume->identities.data, volume->count * sizeof(struct stat));
  }
  memset(slots.data, 0, slots.size);
  const struct pfs_build_object *records = objects.data;
  uint32_t *table = slots.data;
  for (size_t i = 0; i < volume->count; ++i) {
    size_t at = id_slot(&records[i].id, capacity * 2);
    while (table[at]) {
      at = (at + 1) & (capacity * 2 - 1);
    }
    table[at] = (uint32_t)i + 1;
  }
  pfs_memory_free(source->memory, &volume->objects);
  pfs_memory_free(source->memory, &volume->identities);
  pfs_memory_free(source->memory, &volume->id_slots);
  pfs_memory_move(source->memory, &objects, &volume->objects);
  pfs_memory_move(source->memory, &identities, &volume->identities);
  pfs_memory_move(source->memory, &slots, &volume->id_slots);
  volume->capacity = capacity;
  return PFS_OK;
}

static bool
id_exists(const struct host_source *source, const struct pfs_object_id *id)
{
  const uint8_t *reserved = source->reserved_ids.data;
  for (size_t i = 0; i < source->reserved_ids.size; i += PFS_ID_SIZE) {
    if (memcmp(id->bytes, reserved + i, PFS_ID_SIZE) == 0) {
      return true;
    }
  }
  const struct source_volume *volumes = source->volumes.data;
  for (size_t i = 0; i < source->count; ++i) {
    if (!volumes[i].count) {
      continue;
    }
    const struct pfs_build_object *objects = volumes[i].objects.data;
    const uint32_t *slots = volumes[i].id_slots.data;
    size_t at = id_slot(id, volumes[i].capacity * 2);
    while (slots[at]) {
      if (memcmp(id->bytes, objects[slots[at] - 1].id.bytes, PFS_ID_SIZE) == 0) {
        return true;
      }
      at = (at + 1) & (volumes[i].capacity * 2 - 1);
    }
  }
  return false;
}

static enum pfs_status
append_object(struct host_source *source, struct source_volume *volume,
               uint32_t parent, const char *name, const struct stat *info,
               const struct pfs_object_id *root_id)
{
  if (source->object_count == PFS_RECORD_COUNT_MAX) {
    return PFS_LIMIT;
  }
  enum pfs_status status = manifest_grow(source, volume);
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_build_object *objects = volume->objects.data;
  struct pfs_build_object object = {.parent = parent,
    .kind = S_ISDIR(info->st_mode) ? PFS_OBJECT_DIRECTORY : PFS_OBJECT_FILE};
  if (object.kind == PFS_OBJECT_FILE) {
    if (info->st_size < 0 || (uint64_t)info->st_size > PFS_FILE_SIZE_MAX) {
      return PFS_LIMIT;
    }
    object.file_length = (uint64_t)info->st_size;
  }
  if (name) {
    size_t length = strlen(name);
    if (pfs_name_validate((const uint8_t *)name, length) != PFS_OK) {
      return PFS_INVALID;
    }
    object.name.length = (uint16_t)length;
    memcpy(object.name.bytes, name, length);
  }
  uint32_t *slots = volume->id_slots.data;
  size_t slot = 0;
  bool unique = false;
  for (unsigned attempt = 0; attempt < 16 && !unique; ++attempt) {
    if (root_id) {
      object.id = *root_id;
    } else {
      status = host_random_id(object.id.bytes);
      if (status != PFS_OK) {
        return status;
      }
    }
    static const uint8_t zero[PFS_ID_SIZE];
    if (memcmp(object.id.bytes, zero, PFS_ID_SIZE) == 0 ||
        (!root_id && id_exists(source, &object.id))) {
      continue;
    }
    slot = id_slot(&object.id, volume->capacity * 2);
    unique = true;
    while (slots[slot]) {
      if (memcmp(objects[slots[slot] - 1].id.bytes, object.id.bytes, PFS_ID_SIZE) == 0) {
        unique = false;
        break;
      }
      slot = (slot + 1) & (volume->capacity * 2 - 1);
    }
  }
  if (!unique) {
    return PFS_IO;
  }
  objects[volume->count] = object;
  ((struct stat *)volume->identities.data)[volume->count] = *info;
  slots[slot] = (uint32_t)volume->count + 1;
  ++volume->count;
  ++source->object_count;
  return PFS_OK;
}

static enum pfs_status
open_root(struct host_source *source, const char *path, int *out)
{
  struct pfs_allocation scratch = {0};
  size_t length = strlen(path);
  enum pfs_status status = pfs_memory_allocate(source->memory, length + 1, 1, &scratch);
  if (status != PFS_OK) {
    return status;
  }
  memcpy(scratch.data, path, length + 1);
  int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    status = PFS_IO;
  }
  char *part = scratch.data;
  while (status == PFS_OK && *part) {
    char *slash = strchr(part, '/');
    if (slash) {
      *slash = '\0';
    }
    if (strcmp(part, "..") == 0) {
      status = PFS_INVALID;
      break;
    }
    if (*part && strcmp(part, ".") != 0) {
      int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) {
        status = PFS_IO;
        break;
      }
      close(fd);
      fd = next;
    }
    if (!slash) {
      break;
    }
    part = slash + 1;
  }
  int saved = errno;
  pfs_memory_free(source->memory, &scratch);
  if (status == PFS_OK) {
    *out = fd;
  } else if (fd >= 0) {
    close(fd);
  }
  errno = saved;
  return status;
}

static enum pfs_status
scan_volume(struct host_source *source, size_t index)
{
  struct source_volume *volume = &((struct source_volume *)source->volumes.data)[index];
  struct source_workspace *workspace = source->workspace.data;
  int fd = fcntl(volume->root_fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0) {
    return source_error(source, "duplicate root", errno, index, 0, PFS_IO);
  }
  memset(&workspace->frames[0], 0, sizeof(workspace->frames[0]));
  workspace->frames[0].fd = fd;
  size_t depth = 1;
  enum pfs_status status = PFS_OK;
  while (depth && status == PFS_OK) {
    struct source_frame *frame = &workspace->frames[depth - 1];
    if (frame->next == frame->used) {
      ssize_t count;
      do {
        count = getdents64(frame->fd, frame->entries, sizeof(frame->entries));
      } while (count < 0 && errno == EINTR);
      if (count < 0) {
        status = source_error(source, "read source directory", errno,
                               index, frame->object, PFS_IO);
        break;
      }
      frame->used = (size_t)count;
      frame->next = 0;
      if (count == 0) {
        struct stat current;
        const struct stat *identities = volume->identities.data;
        if (fstat(frame->fd, &current) != 0) {
          status = source_error(source, "stat scanned directory", errno,
                                 index, frame->object, PFS_IO);
        } else if (!unchanged(&identities[frame->object], &current)) {
          status = source_error(source, "source directory changed", 0,
                                 index, frame->object, PFS_IO);
        }
        if (close(frame->fd) != 0 && status == PFS_OK) {
          status = source_error(source, "close source directory", errno,
                                 index, frame->object, PFS_IO);
        }
        --depth;
        continue;
      }
    }
    size_t remaining = frame->used - frame->next;
    const size_t prefix = offsetof(struct dirent64, d_name);
    if (remaining < prefix + 1) {
      status = source_error(source, "short host directory entry", 0, index, frame->object, PFS_IO);
      break;
    }
    const struct dirent64 *entry = (const void *)(frame->entries + frame->next);
    if (entry->d_reclen < prefix + 1 || entry->d_reclen > remaining ||
        entry->d_reclen % _Alignof(struct dirent64) != 0 ||
        memchr(entry->d_name, '\0', entry->d_reclen - prefix) == NULL) {
      status = source_error(source, "invalid host directory entry", 0, index, frame->object, PFS_IO);
      break;
    }
    frame->next += entry->d_reclen;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    struct stat info;
    if (fstatat(frame->fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      status = source_error(source, "stat source entry", errno, index, volume->count, PFS_IO);
      break;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
      status = source_error(source, "require regular file or directory", 0,
                             index, volume->count, PFS_INVALID);
      break;
    }
    if (depth == PFS_ANCESTRY_MAX) {
      status = source_error(source, "source ancestry limit", 0, index, volume->count, PFS_LIMIT);
      break;
    }
    size_t object = volume->count;
    status = append_object(source, volume, frame->object, entry->d_name, &info, NULL);
    if (status != PFS_OK) {
      source_error(source, "record source entry", 0, index, object, status);
      break;
    }
    if (S_ISDIR(info.st_mode)) {
      const struct stat *identities = volume->identities.data;
      for (size_t i = 0; i < depth; ++i) {
        if (same_identity(&info, &identities[workspace->frames[i].object])) {
          status = source_error(source, "source directory cycle", 0, index, object, PFS_INVALID);
          break;
        }
      }
      if (status != PFS_OK) {
        break;
      }
      int child = openat(frame->fd, entry->d_name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat opened;
      if (child < 0 || fstat(child, &opened) != 0) {
        int saved = errno;
        if (child >= 0) {
          close(child);
        }
        status = source_error(source, "open source directory", saved, index, object, PFS_IO);
        break;
      }
      if (!unchanged(&info, &opened)) {
        close(child);
        status = source_error(source, "source directory replaced", 0, index, object, PFS_IO);
        break;
      }
      struct source_frame *next = &workspace->frames[depth++];
      memset(next, 0, sizeof(*next));
      next->fd = child;
      next->object = (uint32_t)object;
    }
  }
  while (depth) {
    close(workspace->frames[--depth].fd);
  }
  return status;
}

/* Reopen through the retained root and validate every traversed component.
 * Only ancestry depth, not total source object count, bounds open descriptors. */
static enum pfs_status
open_object(struct host_source *source, size_t index, size_t object, int *out)
{
  struct source_volume *volume = &((struct source_volume *)source->volumes.data)[index];
  const struct pfs_build_object *objects = volume->objects.data;
  const struct stat *identities = volume->identities.data;
  struct source_workspace *workspace = source->workspace.data;
  size_t depth = 0;
  for (uint32_t at = (uint32_t)object; at != 0; at = objects[at].parent) {
    if (depth == PFS_ANCESTRY_MAX - 1) {
      return PFS_LIMIT;
    }
    workspace->chain[depth++] = at;
  }
  int fd = fcntl(volume->root_fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0) {
    return source_error(source, "duplicate source root", errno, index, object, PFS_IO);
  }
  struct stat current;
  if (fstat(fd, &current) != 0 || !unchanged(&identities[0], &current)) {
    close(fd);
    return source_error(source, "source root changed", 0, index, 0, PFS_IO);
  }
  while (depth) {
    size_t at = workspace->chain[--depth];
    char name[PFS_NAME_MAX + 1];
    memcpy(name, objects[at].name.bytes, objects[at].name.length);
    name[objects[at].name.length] = '\0';
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    if (objects[at].kind == PFS_OBJECT_DIRECTORY) {
      flags |= O_DIRECTORY;
    }
    int next = openat(fd, name, flags);
    int saved = errno;
    close(fd);
    if (next < 0) {
      return source_error(source, "reopen source entry", saved, index, at, PFS_IO);
    }
    fd = next;
    if (fstat(fd, &current) != 0 || !unchanged(&identities[at], &current)) {
      close(fd);
      return source_error(source, "source entry changed", 0, index, at, PFS_IO);
    }
  }
  *out = fd;
  return PFS_OK;
}

enum pfs_status
host_source_open(struct host_source *source, struct pfs_memory *memory,
                  struct pfs_build_volume *volumes, const char *const *paths, size_t count,
                  const struct pfs_pool_id *pool)
{
  *source = (struct host_source){.memory = memory, .file_fd = -1};
  enum pfs_status status = pfs_memory_allocate(memory, count * sizeof(struct source_volume),
    _Alignof(struct source_volume), &source->volumes);
  if (status != PFS_OK) {
    return status;
  }
  memset(source->volumes.data, 0, source->volumes.size);
  struct source_volume *sources = source->volumes.data;
  source->count = count;
  for (size_t i = 0; i < count; ++i) {
    sources[i].root_fd = -1;
  }
  status = pfs_memory_allocate(memory, sizeof(struct source_workspace),
    _Alignof(struct source_workspace), &source->workspace);
  if (status == PFS_OK) {
    status = pfs_memory_allocate(memory, (count * 3 + 1) * PFS_ID_SIZE, 1,
                                 &source->reserved_ids);
  }
  if (status != PFS_OK) {
    return status;
  }
  uint8_t *reserved = source->reserved_ids.data;
  memcpy(reserved, pool->bytes, PFS_ID_SIZE);
  for (size_t i = 0; i < count; ++i) {
    memcpy(reserved + (i * 3 + 1) * PFS_ID_SIZE, volumes[i].id.bytes, PFS_ID_SIZE);
    memcpy(reserved + (i * 3 + 2) * PFS_ID_SIZE, volumes[i].root_object.bytes, PFS_ID_SIZE);
    memcpy(reserved + (i * 3 + 3) * PFS_ID_SIZE, volumes[i].owner.bytes, PFS_ID_SIZE);
  }
  for (size_t i = 0; status == PFS_OK && i < count; ++i) {
    struct stat info = {.st_mode = S_IFDIR};
    if (paths[i]) {
      status = open_root(source, paths[i], &sources[i].root_fd);
      if (status != PFS_OK) {
        source_error(source, "open source root without symlinks", errno, i, 0, status);
        break;
      }
      if (fstat(sources[i].root_fd, &info) != 0) {
        status = source_error(source, "stat source root", errno, i, 0, PFS_IO);
        break;
      }
    }
    status = append_object(source, &sources[i], UINT32_MAX, NULL, &info, &volumes[i].root_object);
    if (status == PFS_OK && paths[i]) {
      status = scan_volume(source, i);
    }
    volumes[i].object_count = sources[i].count;
    volumes[i].objects = sources[i].objects.data;
  }
  return status;
}

enum pfs_status
host_source_validate(void *context)
{
  struct host_source *source = context;
  struct source_volume *volumes = source->volumes.data;
  for (size_t i = 0; i < source->count; ++i) {
    if (volumes[i].root_fd < 0) {
      continue;
    }
    for (size_t j = 0; j < volumes[i].count; ++j) {
      int fd;
      enum pfs_status status = open_object(source, i, j, &fd);
      if (status != PFS_OK) {
        return status;
      }
      if (close(fd) != 0) {
        return source_error(source, "close revalidated source", errno, i, j, PFS_IO);
      }
    }
  }
  return PFS_OK;
}

enum pfs_status
host_source_exclude_output(struct host_source *source, int parent_fd)
{
  struct stat output;
  if (fstat(parent_fd, &output) != 0) {
    return source_error(source, "stat output parent", errno, 0, 0, PFS_IO);
  }
  const struct source_volume *volumes = source->volumes.data;
  for (size_t i = 0; i < source->count; ++i) {
    if (volumes[i].root_fd < 0) {
      continue;
    }
    const struct pfs_build_object *objects = volumes[i].objects.data;
    const struct stat *identities = volumes[i].identities.data;
    for (size_t j = 0; j < volumes[i].count; ++j) {
      if (objects[j].kind == PFS_OBJECT_DIRECTORY && same_identity(&output, &identities[j])) {
        return source_error(source, "output parent is inside an imported source", 0,
                             i, j, PFS_INVALID);
      }
    }
  }
  return PFS_OK;
}

enum pfs_status
host_source_read(void *context, size_t index, size_t object, uint64_t offset,
                  void *buffer, size_t length)
{
  struct host_source *source = context;
  struct source_volume *volumes = source->volumes.data;
  if (index >= source->count || object >= volumes[index].count) {
    return PFS_INVALID;
  }
  const struct pfs_build_object *objects = volumes[index].objects.data;
  const struct stat *identities = volumes[index].identities.data;
  if (objects[object].kind != PFS_OBJECT_FILE || offset > objects[object].file_length ||
      length > objects[object].file_length - offset) {
    return PFS_INVALID;
  }
  if (source->file_fd < 0) {
    enum pfs_status status = open_object(source, index, object, &source->file_fd);
    if (status != PFS_OK) {
      return status;
    }
    source->file_volume = index;
    source->file_object = object;
  }
  if (source->file_volume != index || source->file_object != object) {
    return PFS_INVALID;
  }
  size_t done = 0;
  while (done < length) {
    ssize_t count = pread(source->file_fd, (uint8_t *)buffer + done, length - done,
                           (off_t)(offset + done));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return source_error(source, "read source file", count < 0 ? errno : 0,
                           index, object, PFS_IO);
    }
    done += (size_t)count;
  }
  struct stat current;
  if (fstat(source->file_fd, &current) != 0 || !unchanged(&identities[object], &current)) {
    return source_error(source, "source file changed while copying", 0, index, object, PFS_IO);
  }
  if (offset + length == objects[object].file_length) {
    int fd = source->file_fd;
    source->file_fd = -1;
    if (close(fd) != 0) {
      return source_error(source, "close source file", errno, index, object, PFS_IO);
    }
  }
  return PFS_OK;
}

void
host_source_error(const struct host_source *source)
{
  if (source->operation) {
    fprintf(stderr, "%s (source volume %zu, object %zu)", source->operation,
             source->error_volume, source->error_object);
    if (source->error_number) {
      fprintf(stderr, ": %s", strerror(source->error_number));
    }
    fputc('\n', stderr);
  }
}

enum pfs_status
host_source_close(struct host_source *source)
{
  enum pfs_status status = PFS_OK;
  if (!source->memory) {
    return status;
  }
  if (source->file_fd >= 0 && close(source->file_fd) != 0) {
    status = PFS_IO;
  }
  struct source_volume *volumes = source->volumes.data;
  for (size_t i = 0; i < source->count; ++i) {
    if (volumes[i].root_fd >= 0 && close(volumes[i].root_fd) != 0) {
      status = PFS_IO;
    }
    pfs_memory_free(source->memory, &volumes[i].objects);
    pfs_memory_free(source->memory, &volumes[i].identities);
    pfs_memory_free(source->memory, &volumes[i].id_slots);
  }
  pfs_memory_free(source->memory, &source->reserved_ids);
  pfs_memory_free(source->memory, &source->workspace);
  pfs_memory_free(source->memory, &source->volumes);
  *source = (struct host_source){0};
  return status;
}
