/* SPDX-License-Identifier: MPL-2.0 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "mkfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_BLOCKS 16u

struct source_directory {
  struct source_directory *parent;
  DIR *stream;
  struct stat initial;
  uint64_t number;
  struct npfs_inode inode;
  uint64_t block;
  size_t used;
  uint8_t bytes[NPFS_BLOCK_SIZE];
};

static enum npfs_status
source_error(struct mkfs_context *context, const char *operation, const char *name)
{
  snprintf(context->image.error, sizeof(context->image.error), "%s %s: %s",
           operation, name, strerror(errno));
  return NPFS_IO;
}

static bool
source_unchanged(const struct stat *before, const struct stat *after)
{
  return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
         before->st_mode == after->st_mode && before->st_size == after->st_size &&
         before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
         before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
         before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
         before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static void
inode_initialize(struct mkfs_context *context, struct npfs_inode *inode,
                  uint16_t kind, uint64_t parent, const struct stat *source)
{
  memset(inode, 0, sizeof(*inode));
  inode->kind = kind;
  inode->mapping = NPFS_MAPPING_POINTERS;
  inode->parent = parent;
  if (context->created_valid) {
    inode->created_ns = context->created_ns;
    inode->flags |= NPFS_TIME_CREATED_VALID;
  }
  if (source != NULL) {
    inode->modified_ns = npfs_timestamp(source->st_mtim.tv_sec,
                                       (uint32_t)source->st_mtim.tv_nsec);
    inode->flags |= NPFS_TIME_MODIFIED_VALID;
  } else if (context->created_valid) {
    inode->modified_ns = context->created_ns;
    inode->flags |= NPFS_TIME_MODIFIED_VALID;
  }
}

static enum npfs_status
write_inode(struct mkfs_context *context, struct mkfs_volume *volume,
             uint64_t number, const struct npfs_inode *inode)
{
  uint64_t offset = number * NPFS_INODE_SIZE;
  uint64_t logical = offset / NPFS_BLOCK_SIZE;
  uint64_t physical;
  enum npfs_status status = npfs_map_block(&context->image,
                                            volume->record.pointers,
                                            logical, &physical);
  if (status != NPFS_OK) {
    return status;
  }
  if (physical == 0) {
    status = mkfs_allocate(context, &physical);
    if (status == NPFS_OK) {
      status = mkfs_map_insert(context, volume->record.pointers, logical, physical);
    }
    if (status != NPFS_OK) {
      return status;
    }
  }
  uint8_t bytes[NPFS_BLOCK_SIZE];
  status = npfs_read_blocks(&context->image, physical, 1, bytes);
  if (status == NPFS_OK) {
    status = npfs_inode_encode(&context->image.header, inode,
                              bytes + offset % NPFS_BLOCK_SIZE);
  }
  if (status == NPFS_OK) {
    status = npfs_write_blocks(&context->image, physical, 1, bytes);
  }
  return status;
}

static enum npfs_status
copy_file(struct mkfs_context *context, int parent_fd, const char *name,
           const struct stat *expected, struct npfs_inode *inode)
{
  int fd = openat(parent_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return source_error(context, "open source", name);
  }
  struct stat initial;
  struct stat output;
  enum npfs_status status = NPFS_OK;
  if (fstat(fd, &initial) != 0) {
    status = source_error(context, "stat source", name);
  } else if (!S_ISREG(initial.st_mode) || !source_unchanged(expected, &initial)) {
    snprintf(context->image.error, sizeof(context->image.error),
             "source changed before import: %s", name);
    status = NPFS_IO;
  } else if (fstat(context->image.fd, &output) != 0) {
    status = source_error(context, "stat output", name);
  } else if (initial.st_dev == output.st_dev && initial.st_ino == output.st_ino) {
    snprintf(context->image.error, sizeof(context->image.error),
             "source tree contains the output image: %.128s", name);
    status = NPFS_INVALID;
  } else if (initial.st_size < 0 || (uint64_t)initial.st_size > NPFS_FILE_SIZE_MAX) {
    status = NPFS_NO_SPACE;
  }
  uint8_t bytes[COPY_BLOCKS * NPFS_BLOCK_SIZE];
  uint64_t remaining = status == NPFS_OK ? (uint64_t)initial.st_size : 0;
  uint64_t logical = 0;
  while (status == NPFS_OK && remaining != 0) {
    size_t length = remaining < sizeof(bytes) ? (size_t)remaining : sizeof(bytes);
    memset(bytes, 0, sizeof(bytes));
    size_t received = 0;
    while (received < length) {
      ssize_t result = read(fd, bytes + received, length - received);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result < 0) {
        status = source_error(context, "read source", name);
        break;
      }
      if (result == 0) {
        snprintf(context->image.error, sizeof(context->image.error),
                 "source shortened during import: %s", name);
        status = NPFS_IO;
        break;
      }
      received += (size_t)result;
    }
    uint64_t blocks[COPY_BLOCKS];
    unsigned count = (unsigned)((length + NPFS_BLOCK_SIZE - 1) / NPFS_BLOCK_SIZE);
    for (unsigned index = 0; status == NPFS_OK && index < count; index++) {
      status = mkfs_allocate(context, &blocks[index]);
    }
    for (unsigned index = 0; status == NPFS_OK && index < count; index++) {
      status = mkfs_map_insert(context, inode->pointers, logical++, blocks[index]);
    }
    unsigned first = 0;
    while (status == NPFS_OK && first < count) {
      unsigned end = first + 1;
      while (end < count && blocks[end] == blocks[end - 1] + 1) {
        end++;
      }
      status = npfs_write_blocks(&context->image, blocks[first], end - first,
                                    bytes + first * NPFS_BLOCK_SIZE);
      first = end;
    }
    remaining -= length;
  }
  if (status == NPFS_OK) {
    struct stat final;
    if (fstat(fd, &final) != 0) {
      status = source_error(context, "stat source after import", name);
    } else if (!source_unchanged(&initial, &final)) {
      snprintf(context->image.error, sizeof(context->image.error),
               "source changed during import: %s", name);
      status = NPFS_IO;
    }
  }
  if (close(fd) != 0 && status == NPFS_OK) {
    status = source_error(context, "close source", name);
  }
  if (status == NPFS_OK) {
    inode->size = (uint64_t)initial.st_size;
  }
  return status;
}

static enum npfs_status
flush_directory(struct mkfs_context *context, struct source_directory *directory)
{
  if (directory->block == 0) {
    return NPFS_OK;
  }
  return npfs_write_blocks(&context->image, directory->block, 1,
                              directory->bytes);
}

static enum npfs_status
append_entry(struct mkfs_context *context, struct source_directory *directory,
              uint64_t number, const char *name, size_t length)
{
  size_t record_length = (NPFS_DIRENT_HEADER_SIZE + length + 7) & ~(size_t)7;
  if (directory->block == 0 || NPFS_BLOCK_SIZE - directory->used < record_length) {
    enum npfs_status status = flush_directory(context, directory);
    if (status != NPFS_OK) {
      return status;
    }
    if (directory->inode.size == NPFS_FILE_SIZE_MAX) {
      return NPFS_NO_SPACE;
    }
    status = mkfs_allocate(context, &directory->block);
    if (status == NPFS_OK) {
      status = mkfs_map_insert(context, directory->inode.pointers,
                                directory->inode.size / NPFS_BLOCK_SIZE,
                                directory->block);
    }
    if (status != NPFS_OK) {
      return status;
    }
    directory->inode.size += NPFS_BLOCK_SIZE;
    directory->used = 0;
    memset(directory->bytes, 0, sizeof(directory->bytes));
  }
  size_t available = NPFS_BLOCK_SIZE - directory->used;
  if (available - record_length < NPFS_DIRENT_HEADER_SIZE) {
    record_length = available;
  }
  struct npfs_dirent entry = { .inode = number,
                              .record_length = (uint16_t)record_length,
                              .name_length = (uint16_t)length };
  memcpy(entry.name, name, length);
  enum npfs_status status = npfs_dirent_encode(&context->image.header, &entry,
                                             directory->bytes + directory->used);
  if (status != NPFS_OK) {
    return status;
  }
  directory->used += record_length;
  if (directory->used != NPFS_BLOCK_SIZE) {
    struct npfs_dirent free_entry = {
      .record_length = (uint16_t)(NPFS_BLOCK_SIZE - directory->used),
    };
    status = npfs_dirent_encode(&context->image.header, &free_entry,
                               directory->bytes + directory->used);
  }
  return status;
}

static enum npfs_status
open_directory(struct mkfs_context *context, int parent_fd, const char *name,
                const struct stat *expected, uint64_t number, uint64_t parent,
                struct source_directory **result)
{
  int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return source_error(context, "open directory", name);
  }
  struct source_directory *directory = calloc(1, sizeof(*directory));
  if (directory == NULL) {
    close(fd);
    return NPFS_NO_MEMORY;
  }
  enum npfs_status status = NPFS_OK;
  if (fstat(fd, &directory->initial) != 0) {
    status = source_error(context, "stat directory", name);
  } else if (!S_ISDIR(directory->initial.st_mode) ||
             (expected != NULL && !source_unchanged(expected, &directory->initial))) {
    snprintf(context->image.error, sizeof(context->image.error),
             "directory changed before import: %s", name);
    status = NPFS_IO;
  }
  if (status == NPFS_OK) {
    directory->stream = fdopendir(fd);
    if (directory->stream == NULL) {
      status = source_error(context, "read directory", name);
    }
  }
  if (status != NPFS_OK) {
    close(fd);
    free(directory);
    return status;
  }
  directory->number = number;
  inode_initialize(context, &directory->inode, NPFS_INODE_DIRECTORY, parent,
                    &directory->initial);
  *result = directory;
  return NPFS_OK;
}

enum npfs_status
mkfs_populate(struct mkfs_context *context, struct mkfs_volume *volume)
{
  if (volume->source == NULL) {
    struct npfs_inode root;
    inode_initialize(context, &root, NPFS_INODE_DIRECTORY, 1, NULL);
    volume->record.inode_bytes = volume->next_inode * NPFS_INODE_SIZE;
    return write_inode(context, volume, 1, &root);
  }
  struct source_directory *directory = NULL;
  enum npfs_status status = open_directory(context, AT_FDCWD, volume->source, NULL,
                                          1, 1, &directory);
  while (status == NPFS_OK && directory != NULL) {
    errno = 0;
    struct dirent *entry = readdir(directory->stream);
    if (entry == NULL) {
      if (errno != 0) {
        status = source_error(context, "read directory", volume->source);
        break;
      }
      struct stat final;
      if (fstat(dirfd(directory->stream), &final) != 0) {
        status = source_error(context, "stat directory after import", volume->source);
      } else if (!source_unchanged(&directory->initial, &final)) {
        snprintf(context->image.error, sizeof(context->image.error),
                 "directory changed during import: %s", volume->source);
        status = NPFS_IO;
      }
      if (status == NPFS_OK) {
        status = flush_directory(context, directory);
      }
      if (status == NPFS_OK) {
        status = write_inode(context, volume, directory->number, &directory->inode);
      }
      struct source_directory *parent = directory->parent;
      if (closedir(directory->stream) != 0 && status == NPFS_OK) {
        status = source_error(context, "close directory", volume->source);
      }
      free(directory);
      directory = parent;
      continue;
    }
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    size_t length = strlen(name);
    if (!npfs_name_valid((const uint8_t *)name, length)) {
      snprintf(context->image.error, sizeof(context->image.error),
               "invalid UTF-8 filesystem component in %s", volume->source);
      status = NPFS_INVALID;
      break;
    }
    struct stat source;
    int parent_fd = dirfd(directory->stream);
    if (fstatat(parent_fd, name, &source, AT_SYMLINK_NOFOLLOW) != 0) {
      status = source_error(context, "stat source", name);
      break;
    }
    if (!S_ISREG(source.st_mode) && !S_ISDIR(source.st_mode)) {
      snprintf(context->image.error, sizeof(context->image.error),
               "unsupported source entry (only regular files/directories): %.128s", name);
      status = NPFS_UNSUPPORTED;
      break;
    }
    if (volume->next_inode == NPFS_FILE_SIZE_MAX / NPFS_INODE_SIZE) {
      status = NPFS_NO_SPACE;
      break;
    }
    uint64_t number = volume->next_inode++;
    status = append_entry(context, directory, number, name, length);
    if (status != NPFS_OK) {
      break;
    }
    if (S_ISDIR(source.st_mode)) {
      struct source_directory *child = NULL;
      status = open_directory(context, parent_fd, name, &source, number,
                              directory->number, &child);
      if (status == NPFS_OK) {
        child->parent = directory;
        directory = child;
      }
    } else {
      struct npfs_inode inode;
      inode_initialize(context, &inode, NPFS_INODE_FILE, 0, &source);
      status = copy_file(context, parent_fd, name, &source, &inode);
      if (status == NPFS_OK) {
        status = write_inode(context, volume, number, &inode);
      }
    }
  }
  while (directory != NULL) {
    struct source_directory *parent = directory->parent;
    closedir(directory->stream);
    free(directory);
    directory = parent;
  }
  volume->record.inode_bytes = volume->next_inode * NPFS_INODE_SIZE;
  return status;
}
