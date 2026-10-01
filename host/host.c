/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static void *
host_allocate(void *context, size_t size, size_t alignment)
{
  (void)context;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  void *data = NULL;
  return posix_memalign(&data, alignment, size) == 0 ? data : NULL;
}

static void
host_free(void *context, void *data, size_t size, size_t alignment)
{
  (void)context;
  (void)size;
  (void)alignment;
  free(data);
}

enum pfs_status
host_memory_init(struct pfs_memory *memory, uint64_t limit)
{
  return pfs_memory_init(memory, NULL, host_allocate, host_free, limit);
}

enum pfs_status
host_size_parse(const char *text, uint64_t *bytes)
{
  if (*text < '0' || *text > '9') {
    return PFS_INVALID;
  }
  uint64_t value = 0;
  do {
    unsigned digit = (unsigned)(*text++ - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return PFS_INVALID;
    }
    value = value * 10 + digit;
  } while (*text >= '0' && *text <= '9');

  unsigned shift = 0;
  if (strcmp(text, "KiB") == 0) {
    shift = 10;
  } else if (strcmp(text, "MiB") == 0) {
    shift = 20;
  } else if (strcmp(text, "GiB") == 0) {
    shift = 30;
  } else if (strcmp(text, "TiB") == 0) {
    shift = 40;
  } else if (*text != '\0') {
    return PFS_INVALID;
  }
  if (value > (UINT64_MAX >> shift)) {
    return PFS_INVALID;
  }
  *bytes = value << shift;
  return PFS_OK;
}

enum pfs_status
host_random_id(uint8_t bytes[PFS_ID_SIZE])
{
  size_t done = 0;
  while (done < PFS_ID_SIZE) {
    ssize_t count = getrandom(bytes + done, PFS_ID_SIZE - done, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return PFS_IO;
    }
    done += (size_t)count;
  }
  return PFS_OK;
}

void
host_name_print(FILE *stream, const uint8_t *bytes, size_t length)
{
  fputc('"', stream);
  for (size_t i = 0; i < length; ++i) {
    unsigned byte = bytes[i];
    if (byte == '"' || byte == '\\') {
      fprintf(stream, "\\%c", byte);
    } else if (byte < 0x20 || byte >= 0x7f) {
      fprintf(stream, "\\x%02x", byte);
    } else {
      fputc((int)byte, stream);
    }
  }
  fputc('"', stream);
}

int
host_exit_status(enum pfs_status status)
{
  switch (status) {
    case PFS_OK: return 0;
    case PFS_DENIED: return 1;
    case PFS_EXISTS:
    case PFS_DETACHED:
    case PFS_NOT_FOUND:
    case PFS_INVALID: return 2;
    case PFS_ABSENT:
    case PFS_CORRUPT: return 3;
    case PFS_UNSUPPORTED:
    case PFS_LIMIT:
    case PFS_CHANGED:
    case PFS_BUSY:
    case PFS_READ_ONLY:
    case PFS_NO_SPACE:
    case PFS_QUOTA:
    case PFS_RECOVERY_REQUIRED: return 4;
    case PFS_IO:
    case PFS_NO_MEMORY: return 5;
  }
  return 5;
}

void
host_error(const char *operation, enum pfs_status status,
           const struct host_image *image)
{
  fprintf(stderr, "%s: %s", operation, pfs_status_string(status));
  if (image != NULL && image->operation != NULL) {
    fprintf(stderr, " (%s", image->operation);
    if (image->error_block != UINT64_MAX) {
      fprintf(stderr, ", %s %llu", image->error_sector ? "sector" : "block",
              (unsigned long long)image->error_block);
    }
    if (image->error_number != 0) {
      fprintf(stderr, ": %s", strerror(image->error_number));
    }
    fputc(')', stderr);
  }
  if (image != NULL && image->created) {
    fputs("; output is incomplete and has been left in place", stderr);
  }
  fputc('\n', stderr);
}

static enum pfs_status
image_error(struct host_image *image, const char *operation, int error_number,
            uint64_t block)
{
  image->operation = operation;
  image->error_number = error_number;
  image->error_block = block;
  image->error_sector = false;
  return PFS_IO;
}

static enum pfs_status
image_unchanged(struct host_image *image)
{
  struct stat info;
  if (fstat(image->fd, &info) != 0) {
    return image_error(image, "stat image", errno, UINT64_MAX);
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      (uint64_t)info.st_size != image->bytes) {
    return image_error(image, "image size changed", 0, UINT64_MAX);
  }
  return PFS_OK;
}

static enum pfs_status
image_read(void *context, uint64_t first, uint32_t count, void *buffer)
{
  struct host_image *image = context;
  if (image_unchanged(image) != PFS_OK) {
    return PFS_IO;
  }
  size_t length = (size_t)count * PFS_BLOCK_SIZE;
  uint64_t blocks = image->length_bytes / PFS_BLOCK_SIZE;
  if (first >= blocks || count > blocks - first) {
    return image_error(image, "read outside selected extent", 0, first);
  }
  size_t done = 0;
  while (done < length) {
    ssize_t result = pread(image->fd, (uint8_t *)buffer + done, length - done,
                           (off_t)(image->offset_bytes + first * PFS_BLOCK_SIZE + done));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return image_error(image, "read image", result < 0 ? errno : 0, first);
    }
    done += (size_t)result;
  }
  return image_unchanged(image);
}

enum pfs_status
host_image_read_bytes(struct host_image *image, uint64_t offset, size_t length,
                       void *buffer, const char *operation, uint64_t sector)
{
  if (image_unchanged(image) != PFS_OK) {
    return PFS_IO;
  }
  if (offset > image->bytes || length > image->bytes - offset) {
    image_error(image, operation, 0, sector);
    image->error_sector = true;
    return PFS_IO;
  }
  size_t done = 0;
  while (done < length) {
    ssize_t result = pread(image->fd, (uint8_t *)buffer + done, length - done,
                           (off_t)(offset + done));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      image_error(image, operation, result < 0 ? errno : 0, sector);
      image->error_sector = true;
      return PFS_IO;
    }
    done += (size_t)result;
  }
  return image_unchanged(image);
}

static enum pfs_status
image_write(void *context, uint64_t first, uint32_t count, const void *buffer)
{
  struct host_image *image = context;
  if (image_unchanged(image) != PFS_OK) {
    return PFS_IO;
  }
  size_t length = (size_t)count * PFS_BLOCK_SIZE;
  size_t done = 0;
  while (done < length) {
    ssize_t result = pwrite(image->fd, (const uint8_t *)buffer + done, length - done,
                            (off_t)(first * PFS_BLOCK_SIZE + done));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return image_error(image, "write image", result < 0 ? errno : 0, first);
    }
    done += (size_t)result;
  }
  return image_unchanged(image);
}

static enum pfs_status
image_flush(void *context)
{
  struct host_image *image = context;
  if (image_unchanged(image) != PFS_OK) {
    return PFS_IO;
  }
  if (fsync(image->fd) != 0) {
    return image_error(image, "flush image", errno, UINT64_MAX);
  }
  return image_unchanged(image);
}

/* Retain the final parent across creation and directory fsync. Every directory
 * component is opened relative to the preceding descriptor without symlinks. */
static enum pfs_status
open_parent(struct host_image *image, struct pfs_memory *memory, const char *path,
             struct pfs_allocation *scratch, char **final)
{
  size_t length = strlen(path);
  if (length == 0 || path[length - 1] == '/' || length == SIZE_MAX) {
    return PFS_INVALID;
  }
  enum pfs_status status = pfs_memory_allocate(memory, length + 1, 1, scratch);
  if (status != PFS_OK) {
    return status;
  }
  char *copy = scratch->data;
  memcpy(copy, path, length + 1);
  image->parent_fd = open(path[0] == '/' ? "/" : ".",
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (image->parent_fd < 0) {
    return image_error(image, "open output parent", errno, UINT64_MAX);
  }
  char *component = copy;
  while (*component != '\0') {
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
      *final = component;
      return PFS_OK;
    }
    if (*component != '\0' && strcmp(component, ".") != 0) {
      int next = openat(image->parent_fd, component,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) {
        return image_error(image, "open output parent component", errno, UINT64_MAX);
      }
      close(image->parent_fd);
      image->parent_fd = next;
    }
    component = slash + 1;
  }
  return PFS_INVALID;
}

static enum pfs_status
image_open_locked(struct host_image *image, const char *path, bool writable)
{
  *image = (struct host_image){.fd = -1, .parent_fd = -1, .error_block = UINT64_MAX};
  image->fd = open(path, (writable ? O_RDWR : O_RDONLY) |
                        O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (image->fd < 0) {
    return image_error(image, "open image", errno, UINT64_MAX);
  }
  struct stat info;
  if (fstat(image->fd, &info) != 0) {
    return image_error(image, "stat image", errno, UINT64_MAX);
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0) {
    image->operation = "require regular image";
    return PFS_INVALID;
  }
  if (flock(image->fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
    int error_number = errno;
    image_error(image, "lock image", error_number, UINT64_MAX);
    return error_number == EWOULDBLOCK ? PFS_BUSY : PFS_IO;
  }
  if (fstat(image->fd, &info) != 0) {
    return image_error(image, "stat locked image", errno, UINT64_MAX);
  }
  image->bytes = (uint64_t)info.st_size;
  return PFS_OK;
}

static enum pfs_status
image_reader_init(struct host_image *image, struct pfs_block_reader *reader)
{
  uint64_t blocks = image->length_bytes / PFS_BLOCK_SIZE;
  if (blocks < PFS_POOL_BLOCKS_MIN || blocks > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  struct pfs_geometry geometry = {blocks, PFS_IO_BLOCKS_MAX};
  return pfs_block_reader_init(reader, image, &geometry, image_read);
}

enum pfs_status
host_image_open(struct host_image *image, struct pfs_memory *memory,
                 const char *path, struct pfs_block_reader *reader)
{
  (void)memory;
  enum pfs_status status = image_open_locked(image, path, false);
  if (status != PFS_OK) {
    return status;
  }
  image->length_bytes = image->bytes;
  return image_reader_init(image, reader);
}

/* Writable sessions never retry a partial or interrupted transfer. The core
 * owns the resulting publication uncertainty and stops ordinary access. */
static enum pfs_status
image_write_exact(void *context, uint64_t first, uint32_t count, const void *buffer)
{
  struct host_image *image = context;
  if (image_unchanged(image) != PFS_OK) {
    return PFS_IO;
  }
  uint64_t blocks = image->length_bytes / PFS_BLOCK_SIZE;
  if (first >= blocks || count > blocks - first) {
    return image_error(image, "write outside selected extent", 0, first);
  }
  size_t length = (size_t)count * PFS_BLOCK_SIZE;
  ssize_t written = pwrite(image->fd, buffer, length,
                           (off_t)(image->offset_bytes + first * PFS_BLOCK_SIZE));
  if (written < 0 || (size_t)written != length) {
    return image_error(image, "write image exactly", written < 0 ? errno : EIO, first);
  }
  return image_unchanged(image);
}

enum pfs_status
host_image_open_writer(struct host_image *image, const char *path,
                        struct pfs_block_builder *builder)
{
  enum pfs_status status = image_open_locked(image, path, true);
  if (status != PFS_OK) {
    return status;
  }
  if (image->bytes % PFS_BLOCK_SIZE) {
    image->operation = "require block-aligned image";
    return PFS_INVALID;
  }
  image->length_bytes = image->bytes;
  uint64_t blocks = image->length_bytes / PFS_BLOCK_SIZE;
  if (blocks < PFS_POOL_BLOCKS_MIN || blocks > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  struct pfs_geometry geometry = {blocks, PFS_IO_BLOCKS_MAX};
  return pfs_block_builder_init(builder, image, &geometry, image_read,
                                image_write_exact, image_flush);
}

enum pfs_status
host_image_open_gpt(struct host_image *image, struct pfs_memory *memory,
                     const char *path, const struct host_gpt_selection *selection,
                     struct host_gpt_diagnostic *diagnostic,
                     struct pfs_block_reader *reader)
{
  *image = (struct host_image){.fd = -1, .parent_fd = -1, .error_block = UINT64_MAX};
  *diagnostic = (struct host_gpt_diagnostic){0};
  if (selection == NULL || selection->partition_entry == 0 ||
      (selection->sector_size != 512 && selection->sector_size != 4096)) {
    return PFS_INVALID;
  }
  enum pfs_status status = image_open_locked(image, path, false);
  if (status == PFS_OK) {
    status = host_gpt_select(image, memory, selection, diagnostic);
  }
  if (status != PFS_OK) {
    return status;
  }
  uint64_t blocks = diagnostic->length_bytes / PFS_BLOCK_SIZE;
  if (blocks < PFS_POOL_BLOCKS_MIN || blocks > PFS_POOL_BLOCKS_MAX) {
    diagnostic->offset_bytes = 0;
    diagnostic->length_bytes = 0;
    image->operation = "selected GPT partition size";
    return PFS_LIMIT;
  }
  image->offset_bytes = diagnostic->offset_bytes;
  image->length_bytes = diagnostic->length_bytes;
  return image_reader_init(image, reader);
}

enum pfs_status
host_image_prepare(struct host_image *image, struct pfs_memory *memory,
                    const char *path, struct pfs_allocation *scratch, char **name)
{
  *image = (struct host_image){.fd = -1, .parent_fd = -1, .error_block = UINT64_MAX};
  return open_parent(image, memory, path, scratch, name);
}

enum pfs_status
host_image_create(struct host_image *image, const char *name, uint64_t bytes,
                   struct pfs_block_builder *builder)
{
  if (image->parent_fd < 0 || image->fd >= 0) {
    return PFS_INVALID;
  }
  if (bytes % PFS_BLOCK_SIZE != 0 || bytes / PFS_BLOCK_SIZE < PFS_POOL_BLOCKS_MIN ||
      bytes / PFS_BLOCK_SIZE > PFS_POOL_BLOCKS_MAX) {
    return PFS_INVALID;
  }
  image->fd = openat(image->parent_fd, name,
                       O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (image->fd < 0) {
    return image_error(image, "create image", errno, UINT64_MAX);
  }
  image->created = true;
  if (flock(image->fd, LOCK_EX | LOCK_NB) != 0) {
    return image_error(image, "lock new image", errno, UINT64_MAX);
  }
  image->bytes = bytes;
  image->length_bytes = bytes;
  if (ftruncate(image->fd, (off_t)bytes) != 0) {
    return image_error(image, "size sparse image", errno, UINT64_MAX);
  }
  struct pfs_geometry geometry = {bytes / PFS_BLOCK_SIZE, PFS_IO_BLOCKS_MAX};
  return pfs_block_builder_init(builder, image, &geometry, image_read,
                                image_write, image_flush);
}

enum pfs_status
host_image_publish(struct host_image *image)
{
  if (image_flush(image) != PFS_OK) {
    return PFS_IO;
  }
  if (fsync(image->parent_fd) != 0) {
    return image_error(image, "flush output directory", errno, UINT64_MAX);
  }
  return PFS_OK;
}

enum pfs_status
host_image_close(struct host_image *image)
{
  enum pfs_status status = PFS_OK;
  if (image->fd >= 0) {
    if (close(image->fd) != 0) {
      status = image_error(image, "close image", errno, UINT64_MAX);
    }
    image->fd = -1;
  }
  if (image->parent_fd >= 0) {
    if (close(image->parent_fd) != 0 && status == PFS_OK) {
      status = image_error(image, "close output directory", errno, UINT64_MAX);
    }
    image->parent_fd = -1;
  }
  return status;
}
