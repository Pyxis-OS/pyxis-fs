/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void
npfs_memory_copy(void *destination, const void *source, size_t length)
{
  memcpy(destination, source, length);
}

void
npfs_memory_zero(void *destination, size_t length)
{
  memset(destination, 0, length);
}

static enum npfs_status
io_error(struct npfs_image *image, const char *operation, uint64_t block)
{
  snprintf(image->error, sizeof(image->error), "%s block %" PRIu64 ": %s",
           operation, block, strerror(errno));
  return NPFS_IO;
}

static enum npfs_status
transfer(struct npfs_image *image, uint64_t first, uint32_t count,
         void *bytes, bool writing)
{
  if (image == NULL || image->fd < 0 || bytes == NULL || count == 0 ||
      first >= image->block_count || count > image->block_count - first ||
      (uint64_t)count * NPFS_BLOCK_SIZE > SIZE_MAX || (writing && !image->writable)) {
    return NPFS_INVALID;
  }
  uint64_t offset = first * NPFS_BLOCK_SIZE;
  size_t remaining = (size_t)count * NPFS_BLOCK_SIZE;
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
  return NPFS_OK;
}

enum npfs_status
npfs_read_blocks(struct npfs_image *image, uint64_t first, uint32_t count, void *bytes)
{
  return transfer(image, first, count, bytes, false);
}

enum npfs_status
npfs_write_blocks(struct npfs_image *image, uint64_t first, uint32_t count,
                      const void *bytes)
{
  return transfer(image, first, count, (void *)bytes, true);
}

enum npfs_status
npfs_flush(struct npfs_image *image)
{
  if (image == NULL || image->fd < 0 || !image->writable) {
    return NPFS_INVALID;
  }
  if (fsync(image->fd) != 0) {
    return io_error(image, "flush", 0);
  }
  return NPFS_OK;
}

enum npfs_status
npfs_image_close(struct npfs_image *image)
{
  if (image == NULL) {
    return NPFS_INVALID;
  }
  if (image->fd < 0) {
    return NPFS_OK;
  }
  int fd = image->fd;
  image->fd = -1;
  if (close(fd) != 0) {
    return io_error(image, "close", 0);
  }
  return NPFS_OK;
}

static enum npfs_status
open_source(struct npfs_image *image, const char *path,
            bool writable, bool allow_committed, bool allow_device)
{
  if (image == NULL || path == NULL) {
    return NPFS_INVALID;
  }
  *image = (struct npfs_image){ .fd = -1, .writable = writable };
  int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return io_error(image, "open", 0);
  }
  image->fd = fd;
  struct stat info;
  enum npfs_status status = NPFS_IO;
  if (fstat(fd, &info) != 0) {
    status = io_error(image, "stat", 0);
    goto fail;
  }
  uint64_t bytes;
  if (S_ISREG(info.st_mode)) {
    if (flock(fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
      status = io_error(image, "lock", 0);
      goto fail;
    }
    bytes = info.st_size < 0 ? 0 : (uint64_t)info.st_size;
  } else if (allow_device && S_ISBLK(info.st_mode)) {
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      status = io_error(image, "device size", 0);
      goto fail;
    }
  } else {
    status = NPFS_INVALID;
    goto fail;
  }
  if (bytes < 2 * NPFS_BLOCK_SIZE || bytes > INT64_MAX) {
    status = NPFS_INVALID;
    goto fail;
  }
  image->block_count = bytes / NPFS_BLOCK_SIZE;
  uint8_t blocks[2][NPFS_BLOCK_SIZE];
  struct npfs_header headers[2];
  enum npfs_status states[2];
  for (unsigned i = 0; i < 2; ++i) {
    status = npfs_read_blocks(image, i == 0 ? 0 : image->block_count - 1, 1, blocks[i]);
    if (status != NPFS_OK) {
      goto fail;
    }
    states[i] = npfs_header_decode(blocks[i], &headers[i]);
    if (states[i] == NPFS_UNSUPPORTED) {
      status = states[i];
      goto fail;
    }
    if (states[i] == NPFS_OK && headers[i].pool_blocks != image->block_count) {
      states[i] = NPFS_CORRUPT;
    }
  }
  if (states[0] != NPFS_OK && states[1] != NPFS_OK) {
    status = NPFS_CORRUPT;
    goto fail;
  }
  if (states[0] == NPFS_OK && states[1] == NPFS_OK && memcmp(blocks[0], blocks[1], NPFS_BLOCK_SIZE) != 0) {
    status = NPFS_CORRUPT;
    goto fail;
  }
  image->degraded_header = states[0] != NPFS_OK || states[1] != NPFS_OK;
  image->header = headers[states[0] == NPFS_OK ? 0 : 1];
  status = npfs_features_check(&image->header, writable);
  if (status != NPFS_OK) {
    goto fail;
  }
  struct npfs_control controls[2];
  for (unsigned i = 0; i < 2; ++i) {
    status = npfs_read_blocks(image, image->header.journal_start + i, 1, blocks[i]);
    if (status != NPFS_OK) {
      goto fail;
    }
    states[i] = npfs_control_decode(&image->header, blocks[i], &controls[i]);
    if (states[i] != NPFS_OK && npfs_control_checksum_valid(blocks[i])) {
      status = states[i];
      goto fail;
    }
  }
  if (states[0] != NPFS_OK && states[1] != NPFS_OK) {
    status = NPFS_CORRUPT;
    goto fail;
  }
  unsigned selected;
  if (states[0] != NPFS_OK) {
    selected = 1;
  } else if (states[1] != NPFS_OK) {
    selected = 0;
  } else {
    if (controls[0].sequence == controls[1].sequence && memcmp(blocks[0], blocks[1], NPFS_BLOCK_SIZE) != 0) {
      status = NPFS_CORRUPT;
      goto fail;
    }
    selected = controls[1].sequence > controls[0].sequence ? 1 : 0;
  }
  image->control = controls[selected];
  image->control_slot = selected;
  if (image->control.state == NPFS_JOURNAL_COMMITTED && (!writable || !allow_committed)) {
    status = NPFS_RECOVERY_REQUIRED;
    goto fail;
  }
  return NPFS_OK;

fail:
  npfs_image_close(image);
  return status;
}

enum npfs_status
npfs_image_open(struct npfs_image *image, const char *path,
                bool writable, bool allow_committed)
{
  return open_source(image, path, writable, allow_committed, false);
}

enum npfs_status
npfs_source_open(struct npfs_image *image, const char *path)
{
  return open_source(image, path, false, false, true);
}

enum npfs_status
npfs_read_volumes(struct npfs_image *image, struct npfs_volume volumes[NPFS_VOLUME_COUNT])
{
  if (image == NULL || volumes == NULL) {
    return NPFS_INVALID;
  }
  if (image->control.state != NPFS_JOURNAL_EMPTY) {
    return NPFS_RECOVERY_REQUIRED;
  }
  uint8_t block[NPFS_BLOCK_SIZE];
  for (unsigned page = 0; page < NPFS_VOLUME_TABLE_BLOCKS; ++page) {
    enum npfs_status status = npfs_read_blocks(image, image->header.volume_start + page, 1, block);
    if (status != NPFS_OK) {
      return status;
    }
    for (unsigned slot = 0; slot < NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE; ++slot) {
      unsigned number = page * (NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE) + slot;
      status = npfs_volume_decode(&image->header, block + slot * NPFS_VOLUME_SIZE, &volumes[number]);
      if (status != NPFS_OK) {
        return status;
      }
    }
  }
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; ++i) {
    if (volumes[i].state != NPFS_VOLUME_LIVE) {
      continue;
    }
    for (unsigned j = 0; j < i; ++j) {
      if (volumes[j].state == NPFS_VOLUME_LIVE &&
          (memcmp(volumes[i].id, volumes[j].id, NPFS_ID_SIZE) == 0 ||
           (volumes[i].name_length == volumes[j].name_length &&
            memcmp(volumes[i].name, volumes[j].name, volumes[i].name_length) == 0))) {
        return NPFS_CORRUPT;
      }
    }
  }
  return NPFS_OK;
}

enum npfs_status
npfs_map_block(struct npfs_image *image, const uint64_t pointers[NPFS_POINTER_COUNT],
                   uint64_t logical, uint64_t *physical)
{
  if (image == NULL || pointers == NULL || physical == NULL) {
    return NPFS_INVALID;
  }
  struct npfs_map_path path;
  enum npfs_status status = npfs_map_path(logical, &path);
  if (status != NPFS_OK) {
    return status;
  }
  uint64_t block = pointers[path.slot];
  uint8_t bytes[NPFS_BLOCK_SIZE];
  for (unsigned level = 0; level < path.depth && block != 0; ++level) {
    if (!npfs_data_block_valid(&image->header, block)) {
      return NPFS_CORRUPT;
    }
    status = npfs_read_blocks(image, block, 1, bytes);
    if (status != NPFS_OK) {
      return status;
    }
    block = npfs_get_u64(bytes + path.index[level] * 8);
  }
  if (block != 0 && !npfs_data_block_valid(&image->header, block)) {
    return NPFS_CORRUPT;
  }
  *physical = block;
  return NPFS_OK;
}

enum npfs_status
npfs_read_inode(struct npfs_image *image, const struct npfs_volume *volume,
                    uint64_t number, struct npfs_inode *inode)
{
  if (image == NULL || volume == NULL || inode == NULL ||
      number >= volume->inode_bytes / NPFS_INODE_SIZE) {
    return NPFS_INVALID;
  }
  uint64_t byte_offset = number * NPFS_INODE_SIZE;
  uint64_t physical;
  enum npfs_status status = npfs_map_block(image, volume->pointers,
                                           byte_offset / NPFS_BLOCK_SIZE, &physical);
  if (status != NPFS_OK) {
    return status;
  }
  if (physical == 0) {
    return NPFS_CORRUPT;
  }
  uint8_t block[NPFS_BLOCK_SIZE];
  status = npfs_read_blocks(image, physical, 1, block);
  if (status != NPFS_OK) {
    return status;
  }
  return npfs_inode_decode(&image->header, block + byte_offset % NPFS_BLOCK_SIZE, inode);
}

enum npfs_status
npfs_read_file(struct npfs_image *image, const uint64_t pointers[NPFS_POINTER_COUNT],
                   uint64_t size, uint64_t offset, void *bytes, size_t length)
{
  if (image == NULL || pointers == NULL || bytes == NULL || size > NPFS_FILE_SIZE_MAX ||
      offset > size || length > size - offset) {
    return NPFS_INVALID;
  }
  uint8_t *out = bytes;
  uint8_t block[NPFS_BLOCK_SIZE];
  while (length != 0) {
    uint64_t physical;
    enum npfs_status status = npfs_map_block(image, pointers, offset / NPFS_BLOCK_SIZE, &physical);
    if (status != NPFS_OK) {
      return status;
    }
    size_t within = (size_t)(offset % NPFS_BLOCK_SIZE);
    size_t take = NPFS_BLOCK_SIZE - within;
    if (take > length) {
      take = length;
    }
    if (physical == 0) {
      memset(out, 0, take);
    } else {
      status = npfs_read_blocks(image, physical, 1, block);
      if (status != NPFS_OK) {
        return status;
      }
      memcpy(out, block + within, take);
    }
    offset += take;
    out += take;
    length -= take;
  }
  return NPFS_OK;
}

enum npfs_status
npfs_size_parse(const char *text, uint64_t *bytes)
{
  if (text == NULL || bytes == NULL || *text < '0' || *text > '9') {
    return NPFS_INVALID;
  }
  uint64_t value = 0;
  do {
    unsigned digit = (unsigned)(*text++ - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return NPFS_INVALID;
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
    return NPFS_INVALID;
  }
  if (value > UINT64_MAX / multiplier) {
    return NPFS_INVALID;
  }
  *bytes = value * multiplier;
  return NPFS_OK;
}

enum npfs_status
npfs_random_id(uint8_t id[NPFS_ID_SIZE])
{
  size_t done = 0;
  while (done < NPFS_ID_SIZE) {
    ssize_t count = getrandom(id + done, NPFS_ID_SIZE - done, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return NPFS_IO;
    }
    done += (size_t)count;
  }
  return npfs_id_valid(id) ? NPFS_OK : NPFS_IO;
}

bool
npfs_time_now(int64_t *nanoseconds)
{
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return false;
  }
  *nanoseconds = npfs_timestamp(now.tv_sec, (uint32_t)now.tv_nsec);
  return true;
}

void
npfs_name_print(FILE *stream, const uint8_t *bytes, size_t length)
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
npfs_id_print(FILE *stream, const uint8_t id[NPFS_ID_SIZE])
{
  for (unsigned i = 0; i < NPFS_ID_SIZE; ++i) {
    fprintf(stream, "%02x", id[i]);
  }
}

int
npfs_report(const char *operation, enum npfs_status status, const struct npfs_image *image)
{
  fprintf(stderr, "%s: %s", operation, npfs_status_string(status));
  if (image != NULL && image->error[0] != '\0') {
    fprintf(stderr, ": %s", image->error);
  }
  fputc('\n', stderr);
  return 1;
}
