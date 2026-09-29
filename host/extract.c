/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "extract.h"
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  EXTRACT_PAGE_ENTRIES = 16,
  EXTRACT_COPY_BYTES = 64 * 1024,
};

struct extract_frame {
  struct pfs_directory_cursor cursor;
  struct pfs_dirent_record entries[EXTRACT_PAGE_ENTRIES];
  size_t count;
  size_t next;
  int fd;
  bool done;
};

struct extract_buffer {
  uint8_t bytes[EXTRACT_COPY_BYTES];
  char name[PFS_NAME_MAX + 1];
};

static enum pfs_status
output_error(const char *operation, int error_number)
{
  fprintf(stderr, "extract: %s", operation);
  if (error_number != 0) {
    fprintf(stderr, ": %s", strerror(error_number));
  }
  fputc('\n', stderr);
  return PFS_IO;
}

static enum pfs_status
output_close(int fd, enum pfs_status status)
{
  if (fd >= 0 && close(fd) != 0 && status == PFS_OK) {
    return output_error("close output", errno);
  }
  return status;
}

/* Keep each traversed parent descriptor until its child has been opened. The
 * final parent stays open across exclusive creation and publication. */
static enum pfs_status
output_parent(struct pfs_memory *memory, const char *path,
               struct pfs_allocation *scratch, int *parent, char **name)
{
  size_t length = strlen(path);
  if (length == 0 || length == SIZE_MAX || path[length - 1] == '/') {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_memory_allocate(memory, length + 1, 1, scratch);
  if (status != PFS_OK) {
    return status;
  }
  char *component = scratch->data;
  memcpy(component, path, length + 1);
  *parent = open(path[0] == '/' ? "/" : ".",
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (*parent < 0) {
    return output_error("open output parent", errno);
  }
  for (;;) {
    char *slash = strchr(component, '/');
    if (slash != NULL) {
      *slash = '\0';
    }
    if (strcmp(component, "..") == 0) {
      return PFS_INVALID;
    }
    if (slash == NULL) {
      if (*component == '\0' || strcmp(component, ".") == 0) {
        return PFS_INVALID;
      }
      *name = component;
      return PFS_OK;
    }
    if (*component != '\0' && strcmp(component, ".") != 0) {
      int next = openat(*parent, component,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) {
        return output_error("open output parent component", errno);
      }
      status = output_close(*parent, PFS_OK);
      *parent = next;
      if (status != PFS_OK) {
        return status;
      }
    }
    component = slash + 1;
  }
}

static enum pfs_status
extract_file(struct pfs_volume *volume, const struct pfs_object_record *object,
              int parent, const char *name, struct extract_buffer *buffer,
              bool *created)
{
  int fd = openat(parent, name,
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return output_error("create output file", errno);
  }
  *created = true;
  enum pfs_status status = PFS_OK;
  uint64_t offset = 0;
  while (status == PFS_OK && offset < object->file_length) {
    size_t length = sizeof(buffer->bytes);
    if (object->file_length - offset < length) {
      length = (size_t)(object->file_length - offset);
    }
    size_t count = 0;
    status = pfs_volume_diagnostic_read(volume, &object->id, offset,
                                         buffer->bytes, length, &count);
    if (status != PFS_OK) {
      break;
    }
    if (count != length) {
      status = PFS_CORRUPT;
      break;
    }
    size_t written = 0;
    while (written < count) {
      ssize_t result = write(fd, buffer->bytes + written, count - written);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result <= 0) {
        status = output_error("write output file", result < 0 ? errno : 0);
        break;
      }
      written += (size_t)result;
    }
    offset += count;
  }
  if (status == PFS_OK && fsync(fd) != 0) {
    status = output_error("flush output file", errno);
  }
  return output_close(fd, status);
}

static enum pfs_status
extract_directory(struct pfs_volume *volume, const struct pfs_object_id *id,
                   int parent, const char *name, struct extract_frame *frame,
                   bool *created)
{
  if (mkdirat(parent, name, 0700) != 0) {
    return output_error("create output directory", errno);
  }
  *created = true;
  frame->fd = openat(parent, name,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (frame->fd < 0) {
    return output_error("open output directory", errno);
  }
  return pfs_volume_diagnostic_directory_open(volume, id, &frame->cursor);
}

static enum pfs_status
frame_close(struct extract_frame *frame, enum pfs_status status)
{
  enum pfs_status close_status = pfs_directory_close(&frame->cursor);
  if (status == PFS_OK) {
    status = close_status;
  }
  status = output_close(frame->fd, status);
  *frame = (struct extract_frame){.fd = -1};
  return status;
}

enum pfs_status
host_extract(struct pfs_volume *volume, const struct pfs_object_record *object,
              const char *path)
{
  struct pfs_memory *memory = volume->pool->memory;
  struct pfs_allocation scratch = {0};
  struct pfs_allocation frames_storage = {0};
  struct pfs_allocation buffer_storage = {0};
  int parent = -1;
  char *name = NULL;
  bool created = false;
  size_t depth = 0;
  enum pfs_status status = output_parent(memory, path, &scratch, &parent, &name);
  if (status == PFS_OK) {
    status = pfs_memory_allocate(memory, sizeof(struct extract_buffer),
      _Alignof(struct extract_buffer), &buffer_storage);
  }
  struct extract_buffer *buffer = buffer_storage.data;
  if (status == PFS_OK && object->kind == PFS_OBJECT_FILE) {
    status = extract_file(volume, object, parent, name, buffer, &created);
  } else if (status == PFS_OK) {
    status = pfs_memory_allocate(memory, PFS_ANCESTRY_MAX * sizeof(struct extract_frame),
      _Alignof(struct extract_frame), &frames_storage);
    if (status == PFS_OK) {
      struct extract_frame *frames = frames_storage.data;
      memset(frames, 0, frames_storage.size);
      frames[0].fd = -1;
      depth = 1;
      status = extract_directory(volume, &object->id, parent, name, &frames[0], &created);
    }
  }
  struct extract_frame *frames = frames_storage.data;
  while (status == PFS_OK && depth != 0) {
    struct extract_frame *frame = &frames[depth - 1];
    if (frame->next == frame->count) {
      if (frame->done) {
        if (fsync(frame->fd) != 0) {
          status = output_error("flush output directory", errno);
        }
        status = frame_close(frame, status);
        --depth;
        continue;
      }
      status = pfs_directory_next(&frame->cursor, frame->entries,
        EXTRACT_PAGE_ENTRIES, &frame->count, &frame->done);
      frame->next = 0;
      if (status != PFS_OK || frame->count == 0) {
        continue;
      }
    }
    const struct pfs_dirent_record *entry = &frame->entries[frame->next++];
    struct pfs_object_record child;
    status = pfs_volume_diagnostic_object(volume, &entry->object, &child);
    if (status != PFS_OK) {
      break;
    }
    memcpy(buffer->name, entry->name.bytes, entry->name.length);
    buffer->name[entry->name.length] = '\0';
    if (child.kind == PFS_OBJECT_FILE) {
      status = extract_file(volume, &child, frame->fd, buffer->name, buffer, &created);
    } else if (depth == PFS_ANCESTRY_MAX) {
      status = PFS_LIMIT;
    } else {
      struct extract_frame *next = &frames[depth++];
      next->fd = -1;
      status = extract_directory(volume, &child.id, frame->fd,
                                  buffer->name, next, &created);
    }
  }
  while (depth != 0) {
    status = frame_close(&frames[--depth], status);
  }
  if (status == PFS_OK && fsync(parent) != 0) {
    status = output_error("flush output parent", errno);
  }
  status = output_close(parent, status);
  pfs_memory_free(memory, &buffer_storage);
  pfs_memory_free(memory, &frames_storage);
  pfs_memory_free(memory, &scratch);
  if (status != PFS_OK && created) {
    fputs("extract: partial output at ", stderr);
    host_name_print(stderr, (const uint8_t *)path, strlen(path));
    fputs(" has been left in place for explicit removal\n", stderr);
  }
  return status;
}
