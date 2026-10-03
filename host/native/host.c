/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void
pnf_memory_copy(void *destination, const void *source, size_t length)
{
  memcpy(destination, source, length);
}

void
pnf_memory_zero(void *destination, size_t length)
{
  memset(destination, 0, length);
}

static enum pnf_status
io_error(struct native_image *image, const char *operation, uint64_t block)
{
  snprintf(image->error, sizeof(image->error), "%s block %" PRIu64 ": %s",
           operation, block, strerror(errno));
  return PNF_IO;
}

static enum pnf_status
transfer(struct native_image *image, uint64_t first, uint32_t count,
         void *bytes, bool writing)
{
  if (image == NULL || image->fd < 0 || bytes == NULL || count == 0 ||
      first >= image->block_count || count > image->block_count - first ||
      (uint64_t)count * PNF_BLOCK_SIZE > SIZE_MAX || (writing && !image->writable)) {
    return PNF_INVALID;
  }
  uint64_t offset = first * PNF_BLOCK_SIZE;
  size_t remaining = (size_t)count * PNF_BLOCK_SIZE;
  uint8_t *cursor = bytes;
  while (remaining != 0) {
    size_t length = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
    ssize_t done = writing ? pwrite(image->fd, cursor, length, (off_t)offset) :
                             pread(image->fd, cursor, length, (off_t)offset);
    if (done < 0 && errno == EINTR) {
      continue;
    }
    if (done <= 0) {
      if (done == 0) {
        errno = EIO;
      }
      return io_error(image, writing ? "write" : "read", first);
    }
    remaining -= (size_t)done;
    cursor += done;
    offset += (uint64_t)done;
  }
  return PNF_OK;
}

enum pnf_status
native_read_blocks(struct native_image *image, uint64_t first, uint32_t count, void *bytes)
{
  return transfer(image, first, count, bytes, false);
}

enum pnf_status
native_write_blocks(struct native_image *image, uint64_t first, uint32_t count,
                      const void *bytes)
{
  return transfer(image, first, count, (void *)bytes, true);
}

enum pnf_status
native_flush(struct native_image *image)
{
  if (image == NULL || image->fd < 0 || !image->writable) {
    return PNF_INVALID;
  }
  if (fsync(image->fd) != 0) {
    return io_error(image, "flush", 0);
  }
  return PNF_OK;
}

enum pnf_status
native_image_close(struct native_image *image)
{
  if (image == NULL) {
    return PNF_INVALID;
  }
  if (image->fd < 0) {
    return PNF_OK;
  }
  int fd = image->fd;
  image->fd = -1;
  if (close(fd) != 0) {
    return io_error(image, "close", 0);
  }
  return PNF_OK;
}

enum pnf_status
native_image_open(struct native_image *image, const char *path,
                    bool writable, bool allow_committed)
{
  if (image == NULL || path == NULL) {
    return PNF_INVALID;
  }
  *image = (struct native_image){ .fd = -1, .writable = writable };
  int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return io_error(image, "open", 0);
  }
  image->fd = fd;
  struct stat info;
  enum pnf_status status = PNF_IO;
  if (fstat(fd, &info) != 0 || flock(fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
    status = io_error(image, "stat/lock", 0);
    goto fail;
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 2 * PNF_BLOCK_SIZE) {
    status = PNF_INVALID;
    goto fail;
  }
  image->block_count = (uint64_t)info.st_size / PNF_BLOCK_SIZE;
  uint8_t blocks[2][PNF_BLOCK_SIZE];
  struct pnf_header headers[2];
  enum pnf_status states[2];
  for (unsigned i = 0; i < 2; ++i) {
    status = native_read_blocks(image, i == 0 ? 0 : image->block_count - 1, 1, blocks[i]);
    if (status != PNF_OK) {
      goto fail;
    }
    states[i] = pnf_header_decode(blocks[i], &headers[i]);
    if (states[i] == PNF_UNSUPPORTED) {
      status = states[i];
      goto fail;
    }
    if (states[i] == PNF_OK && headers[i].pool_blocks != image->block_count) {
      states[i] = PNF_CORRUPT;
    }
  }
  if (states[0] != PNF_OK && states[1] != PNF_OK) {
    status = PNF_CORRUPT;
    goto fail;
  }
  if (states[0] == PNF_OK && states[1] == PNF_OK && memcmp(blocks[0], blocks[1], PNF_BLOCK_SIZE) != 0) {
    status = PNF_CORRUPT;
    goto fail;
  }
  image->degraded_header = states[0] != PNF_OK || states[1] != PNF_OK;
  image->header = headers[states[0] == PNF_OK ? 0 : 1];
  status = pnf_features_check(&image->header, writable);
  if (status != PNF_OK) {
    goto fail;
  }
  struct pnf_control controls[2];
  for (unsigned i = 0; i < 2; ++i) {
    status = native_read_blocks(image, image->header.journal_start + i, 1, blocks[i]);
    if (status != PNF_OK) {
      goto fail;
    }
    states[i] = pnf_control_decode(&image->header, blocks[i], &controls[i]);
    if (states[i] != PNF_OK && pnf_control_checksum_valid(blocks[i])) {
      status = states[i];
      goto fail;
    }
  }
  if (states[0] != PNF_OK && states[1] != PNF_OK) {
    status = PNF_CORRUPT;
    goto fail;
  }
  unsigned selected;
  if (states[0] != PNF_OK) {
    selected = 1;
  } else if (states[1] != PNF_OK) {
    selected = 0;
  } else {
    if (controls[0].sequence == controls[1].sequence && memcmp(blocks[0], blocks[1], PNF_BLOCK_SIZE) != 0) {
      status = PNF_CORRUPT;
      goto fail;
    }
    selected = controls[1].sequence > controls[0].sequence ? 1 : 0;
  }
  image->control = controls[selected];
  image->control_slot = selected;
  if (image->control.state == PNF_JOURNAL_COMMITTED && (!writable || !allow_committed)) {
    status = PNF_RECOVERY_REQUIRED;
    goto fail;
  }
  return PNF_OK;

fail:
  native_image_close(image);
  return status;
}

enum pnf_status
native_read_volumes(struct native_image *image, struct pnf_volume volumes[PNF_VOLUME_COUNT])
{
  if (image == NULL || volumes == NULL) {
    return PNF_INVALID;
  }
  if (image->control.state != PNF_JOURNAL_EMPTY) {
    return PNF_RECOVERY_REQUIRED;
  }
  uint8_t block[PNF_BLOCK_SIZE];
  for (unsigned page = 0; page < PNF_VOLUME_TABLE_BLOCKS; ++page) {
    enum pnf_status status = native_read_blocks(image, image->header.volume_start + page, 1, block);
    if (status != PNF_OK) {
      return status;
    }
    for (unsigned slot = 0; slot < PNF_BLOCK_SIZE / PNF_VOLUME_SIZE; ++slot) {
      unsigned number = page * (PNF_BLOCK_SIZE / PNF_VOLUME_SIZE) + slot;
      status = pnf_volume_decode(&image->header, block + slot * PNF_VOLUME_SIZE, &volumes[number]);
      if (status != PNF_OK) {
        return status;
      }
    }
  }
  for (unsigned i = 0; i < PNF_VOLUME_COUNT; ++i) {
    if (volumes[i].state != PNF_VOLUME_LIVE) {
      continue;
    }
    for (unsigned j = 0; j < i; ++j) {
      if (volumes[j].state == PNF_VOLUME_LIVE &&
          (memcmp(volumes[i].id, volumes[j].id, PNF_ID_SIZE) == 0 ||
           (volumes[i].name_length == volumes[j].name_length &&
            memcmp(volumes[i].name, volumes[j].name, volumes[i].name_length) == 0))) {
        return PNF_CORRUPT;
      }
    }
  }
  return PNF_OK;
}

enum pnf_status
native_map_block(struct native_image *image, const uint64_t pointers[PNF_POINTER_COUNT],
                   uint64_t logical, uint64_t *physical)
{
  if (image == NULL || pointers == NULL || physical == NULL) {
    return PNF_INVALID;
  }
  struct pnf_map_path path;
  enum pnf_status status = pnf_map_path(logical, &path);
  if (status != PNF_OK) {
    return status;
  }
  uint64_t block = pointers[path.slot];
  uint8_t bytes[PNF_BLOCK_SIZE];
  for (unsigned level = 0; level < path.depth && block != 0; ++level) {
    if (!pnf_data_block_valid(&image->header, block)) {
      return PNF_CORRUPT;
    }
    status = native_read_blocks(image, block, 1, bytes);
    if (status != PNF_OK) {
      return status;
    }
    block = pnf_get_u64(bytes + path.index[level] * 8);
  }
  if (block != 0 && !pnf_data_block_valid(&image->header, block)) {
    return PNF_CORRUPT;
  }
  *physical = block;
  return PNF_OK;
}

enum pnf_status
native_read_inode(struct native_image *image, const struct pnf_volume *volume,
                    uint64_t number, struct pnf_inode *inode)
{
  if (image == NULL || volume == NULL || inode == NULL ||
      number >= volume->inode_bytes / PNF_INODE_SIZE) {
    return PNF_INVALID;
  }
  uint64_t byte_offset = number * PNF_INODE_SIZE;
  uint64_t physical;
  enum pnf_status status = native_map_block(image, volume->pointers,
                                           byte_offset / PNF_BLOCK_SIZE, &physical);
  if (status != PNF_OK) {
    return status;
  }
  if (physical == 0) {
    return PNF_CORRUPT;
  }
  uint8_t block[PNF_BLOCK_SIZE];
  status = native_read_blocks(image, physical, 1, block);
  if (status != PNF_OK) {
    return status;
  }
  return pnf_inode_decode(&image->header, block + byte_offset % PNF_BLOCK_SIZE, inode);
}

enum pnf_status
native_read_file(struct native_image *image, const uint64_t pointers[PNF_POINTER_COUNT],
                   uint64_t size, uint64_t offset, void *bytes, size_t length)
{
  if (image == NULL || pointers == NULL || bytes == NULL || size > PNF_FILE_SIZE_MAX ||
      offset > size || length > size - offset) {
    return PNF_INVALID;
  }
  uint8_t *out = bytes;
  uint8_t block[PNF_BLOCK_SIZE];
  while (length != 0) {
    uint64_t physical;
    enum pnf_status status = native_map_block(image, pointers, offset / PNF_BLOCK_SIZE, &physical);
    if (status != PNF_OK) {
      return status;
    }
    size_t within = (size_t)(offset % PNF_BLOCK_SIZE);
    size_t take = PNF_BLOCK_SIZE - within;
    if (take > length) {
      take = length;
    }
    if (physical == 0) {
      memset(out, 0, take);
    } else {
      status = native_read_blocks(image, physical, 1, block);
      if (status != PNF_OK) {
        return status;
      }
      memcpy(out, block + within, take);
    }
    offset += take;
    out += take;
    length -= take;
  }
  return PNF_OK;
}

enum pnf_status
native_size_parse(const char *text, uint64_t *bytes)
{
  if (text == NULL || bytes == NULL || *text < '0' || *text > '9') {
    return PNF_INVALID;
  }
  uint64_t value = 0;
  do {
    unsigned digit = (unsigned)(*text++ - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return PNF_INVALID;
    }
    value = value * 10 + digit;
  } while (*text >= '0' && *text <= '9');
  uint64_t multiplier;
  if (*text == '\0') {
    multiplier = 1;
  } else if (strcmp(text, "KiB") == 0) {
    multiplier = UINT64_C(1024);
  } else if (strcmp(text, "MiB") == 0) {
    multiplier = UINT64_C(1024) * 1024;
  } else if (strcmp(text, "GiB") == 0) {
    multiplier = UINT64_C(1024) * 1024 * 1024;
  } else if (strcmp(text, "TiB") == 0) {
    multiplier = UINT64_C(1024) * 1024 * 1024 * 1024;
  } else if (strcmp(text, "KB") == 0) {
    multiplier = UINT64_C(1000);
  } else if (strcmp(text, "MB") == 0) {
    multiplier = UINT64_C(1000) * 1000;
  } else if (strcmp(text, "GB") == 0) {
    multiplier = UINT64_C(1000) * 1000 * 1000;
  } else if (strcmp(text, "TB") == 0) {
    multiplier = UINT64_C(1000) * 1000 * 1000 * 1000;
  } else {
    return PNF_INVALID;
  }
  if (value > UINT64_MAX / multiplier) {
    return PNF_INVALID;
  }
  *bytes = value * multiplier;
  return PNF_OK;
}

enum pnf_status
native_random_id(uint8_t id[PNF_ID_SIZE])
{
  size_t done = 0;
  while (done < PNF_ID_SIZE) {
    ssize_t count = getrandom(id + done, PNF_ID_SIZE - done, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return PNF_IO;
    }
    done += (size_t)count;
  }
  return pnf_id_valid(id) ? PNF_OK : PNF_IO;
}

bool
native_time_now(int64_t *nanoseconds)
{
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return false;
  }
  *nanoseconds = pnf_timestamp(now.tv_sec, (uint32_t)now.tv_nsec);
  return true;
}

void
native_name_print(FILE *stream, const uint8_t *bytes, size_t length)
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

void
native_id_print(FILE *stream, const uint8_t id[PNF_ID_SIZE])
{
  for (unsigned i = 0; i < PNF_ID_SIZE; ++i) {
    fprintf(stream, "%02x", id[i]);
  }
}

int
native_report(const char *operation, enum pnf_status status, const struct native_image *image)
{
  fprintf(stderr, "%s: %s", operation, pnf_status_string(status));
  if (image != NULL && image->error[0] != '\0') {
    fprintf(stderr, ": %s", image->error);
  }
  fputc('\n', stderr);
  return 1;
}
