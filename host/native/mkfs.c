/* SPDX-License-Identifier: MPL-2.0 */
#define _FILE_OFFSET_BITS 64
#include "mkfs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void
usage(FILE *stream)
{
  fputs("usage: mkpyxisfs-native --image PATH --size SIZE --journal SIZE\n"
        "       --volume NAME [--source DIRECTORY] [--volume NAME ...]\n"
        "SIZE accepts bytes or binary KiB, MiB, GiB and TiB suffixes.\n"
        "The image must be new; --source binds to the preceding volume.\n", stream);
}

/* Allocation never reuses a block: the exclusive sparse image supplies zeros
 * for new inode/indirect storage until it is explicitly written. */
enum pnf_status
mkfs_allocate(struct mkfs_context *context, uint64_t *block)
{
  while (context->next_block < context->image.header.pool_blocks &&
         !pnf_data_block_valid(&context->image.header, context->next_block)) {
    context->next_block++;
  }
  if (context->next_block == context->image.header.pool_blocks) {
    return PNF_NO_SPACE;
  }
  *block = context->next_block++;
  return PNF_OK;
}

enum pnf_status
mkfs_map_insert(struct mkfs_context *context,
                uint64_t pointers[PNF_POINTER_COUNT], uint64_t logical,
                uint64_t physical)
{
  struct pnf_map_path path;
  enum pnf_status status = pnf_map_path(logical, &path);
  if (status != PNF_OK) {
    return status;
  }
  if (path.depth == 0) {
    if (pointers[path.slot] != 0) {
      return PNF_CORRUPT;
    }
    pointers[path.slot] = physical;
    return PNF_OK;
  }
  if (pointers[path.slot] == 0) {
    status = mkfs_allocate(context, &pointers[path.slot]);
    if (status != PNF_OK) {
      return status;
    }
  }
  uint64_t block = pointers[path.slot];
  for (unsigned level = 0; level < path.depth; level++) {
    uint8_t bytes[PNF_BLOCK_SIZE];
    status = native_read_blocks(&context->image, block, 1, bytes);
    if (status != PNF_OK) {
      return status;
    }
    uint8_t *slot = bytes + path.index[level] * sizeof(uint64_t);
    uint64_t next = pnf_get_u64(slot);
    if (level + 1 == path.depth) {
      if (next != 0) {
        return PNF_CORRUPT;
      }
      pnf_put_u64(slot, physical);
      return native_write_blocks(&context->image, block, 1, bytes);
    }
    if (next == 0) {
      status = mkfs_allocate(context, &next);
      if (status != PNF_OK) {
        return status;
      }
      pnf_put_u64(slot, next);
      status = native_write_blocks(&context->image, block, 1, bytes);
      if (status != PNF_OK) {
        return status;
      }
    }
    block = next;
  }
  return PNF_CORRUPT;
}

static void
bitmap_mark(uint8_t bytes[PNF_BLOCK_SIZE], uint64_t base,
            uint64_t begin, uint64_t end)
{
  uint64_t limit = base + PNF_BITMAP_BITS;
  if (begin < base) {
    begin = base;
  }
  if (end > limit) {
    end = limit;
  }
  while (begin < end && begin % 8 != 0) {
    bytes[(begin - base) / 8] |= (uint8_t)(1u << (begin % 8));
    begin++;
  }
  uint64_t whole_end = end - end % 8;
  if (begin < whole_end) {
    memset(bytes + (begin - base) / 8, 0xff, (size_t)((whole_end - begin) / 8));
    begin = whole_end;
  }
  while (begin < end) {
    bytes[(begin - base) / 8] |= (uint8_t)(1u << (begin % 8));
    begin++;
  }
}

static enum pnf_status
write_metadata(struct mkfs_context *context, struct mkfs_volume *volumes,
                unsigned volume_count)
{
  struct pnf_header *header = &context->image.header;
  uint8_t bytes[PNF_BLOCK_SIZE];
  for (uint64_t index = 0; index < header->bitmap_blocks; index++) {
    uint64_t base = index * PNF_BITMAP_BITS;
    memset(bytes, 0, sizeof(bytes));
    bitmap_mark(bytes, base, 0, context->next_block);
    bitmap_mark(bytes, base, header->bitmap_start,
                 header->bitmap_start + header->bitmap_blocks);
    bitmap_mark(bytes, base, header->volume_start,
                 header->volume_start + header->volume_blocks);
    bitmap_mark(bytes, base, header->journal_start,
                 header->journal_start + header->journal_blocks);
    bitmap_mark(bytes, base, header->pool_blocks - 1, base + PNF_BITMAP_BITS);
    enum pnf_status status = native_write_blocks(&context->image,
                                                 header->bitmap_start + index,
                                                 1, bytes);
    if (status != PNF_OK) {
      return status;
    }
  }
  for (unsigned index = 0; index < PNF_VOLUME_TABLE_BLOCKS; index++) {
    memset(bytes, 0, sizeof(bytes));
    for (unsigned slot = 0; slot < PNF_BLOCK_SIZE / PNF_VOLUME_SIZE; slot++) {
      unsigned volume_index = index * (PNF_BLOCK_SIZE / PNF_VOLUME_SIZE) + slot;
      if (volume_index < volume_count) {
        enum pnf_status status = pnf_volume_encode(header,
                                  &volumes[volume_index].record,
                                  bytes + slot * PNF_VOLUME_SIZE);
        if (status != PNF_OK) {
          return status;
        }
      }
    }
    enum pnf_status status = native_write_blocks(&context->image,
                                                 header->volume_start + index,
                                                 1, bytes);
    if (status != PNF_OK) {
      return status;
    }
  }
  struct pnf_control control = {0};
  memcpy(control.pool_id, header->pool_id, PNF_ID_SIZE);
  for (unsigned slot = 0; slot < 2; slot++) {
    control.sequence = slot;
    enum pnf_status status = pnf_control_encode(header, &control, bytes);
    if (status == PNF_OK) {
      status = native_write_blocks(&context->image, header->journal_start + slot,
                                    1, bytes);
    }
    if (status != PNF_OK) {
      return status;
    }
  }
  enum pnf_status status = pnf_header_encode(header, bytes);
  if (status == PNF_OK) {
    status = native_write_blocks(&context->image, header->pool_blocks - 1, 1, bytes);
  }
  if (status == PNF_OK) {
    status = native_write_blocks(&context->image, 0, 1, bytes);
  }
  if (status == PNF_OK) {
    status = native_flush(&context->image);
  }
  return status;
}

static bool
parse_options(int argc, char **argv, const char **image_path, uint64_t *size,
               uint64_t *journal, struct mkfs_volume *volumes, unsigned *count)
{
  for (int index = 1; index < argc; index++) {
    const char *option = argv[index];
    if (index + 1 == argc) {
      return false;
    }
    const char *value = argv[++index];
    if (strcmp(option, "--image") == 0 && *image_path == NULL) {
      *image_path = value;
    } else if (strcmp(option, "--size") == 0 && *size == 0) {
      if (native_size_parse(value, size) != PNF_OK || *size == 0) {
        return false;
      }
    } else if (strcmp(option, "--journal") == 0 && *journal == 0) {
      if (native_size_parse(value, journal) != PNF_OK || *journal == 0 ||
          *journal % PNF_BLOCK_SIZE != 0) {
        return false;
      }
    } else if (strcmp(option, "--volume") == 0 && *count < PNF_VOLUME_COUNT) {
      size_t length = strlen(value);
      if (!pnf_name_valid((const uint8_t *)value, length)) {
        return false;
      }
      for (unsigned previous = 0; previous < *count; previous++) {
        if (strcmp(value, volumes[previous].name) == 0) {
          return false;
        }
      }
      volumes[*count].name = value;
      (*count)++;
    } else if (strcmp(option, "--source") == 0 && *count != 0 &&
               volumes[*count - 1].source == NULL) {
      volumes[*count - 1].source = value;
    } else {
      return false;
    }
  }
  return *image_path != NULL && *size != 0 && *journal != 0 && *count != 0;
}

int
main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    usage(stdout);
    return EXIT_SUCCESS;
  }
  const char *image_path = NULL;
  uint64_t size = 0;
  uint64_t journal = 0;
  struct mkfs_volume volumes[PNF_VOLUME_COUNT] = {0};
  unsigned count = 0;
  if (!parse_options(argc, argv, &image_path, &size, &journal, volumes, &count) ||
      size > INT64_MAX) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  struct mkfs_context context = { .image = { .fd = -1, .writable = true },
                                  .next_block = 1 };
  uint8_t id[PNF_ID_SIZE];
  enum pnf_status status = native_random_id(id);
  if (status == PNF_OK) {
    status = pnf_header_layout(size / PNF_BLOCK_SIZE, journal / PNF_BLOCK_SIZE,
                               id, &context.image.header);
  }
  if (status != PNF_OK) {
    return native_report("format geometry", status, NULL);
  }
  context.image.block_count = context.image.header.pool_blocks;
  context.created_valid = native_time_now(&context.created_ns);
  context.image.fd = open(image_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW |
                                      O_CLOEXEC, 0666);
  if (context.image.fd < 0) {
    fprintf(stderr, "%s: %s\n", image_path, strerror(errno));
    return EXIT_FAILURE;
  }
  struct stat output = {0};
  bool output_known = fstat(context.image.fd, &output) == 0;
  if (!output_known || !S_ISREG(output.st_mode) ||
      flock(context.image.fd, LOCK_EX | LOCK_NB) != 0 ||
      ftruncate(context.image.fd, (off_t)size) != 0) {
    snprintf(context.image.error, sizeof(context.image.error),
             "cannot initialize new regular image: %s", strerror(errno));
    status = PNF_IO;
  }
  for (unsigned index = 0; status == PNF_OK && index < count; index++) {
    status = native_random_id(volumes[index].record.id);
    if (status != PNF_OK) {
      break;
    }
    for (unsigned previous = 0; previous < index; previous++) {
      if (memcmp(volumes[index].record.id, volumes[previous].record.id,
                 PNF_ID_SIZE) == 0) {
        status = PNF_EXISTS;
        break;
      }
    }
    if (status != PNF_OK) {
      break;
    }
    volumes[index].record.state = PNF_VOLUME_LIVE;
    volumes[index].record.mapping = PNF_MAPPING_POINTERS;
    volumes[index].record.root_inode = 1;
    volumes[index].record.name_length = (uint16_t)strlen(volumes[index].name);
    memcpy(volumes[index].record.name, volumes[index].name,
           volumes[index].record.name_length);
    volumes[index].next_inode = 2;
    status = mkfs_populate(&context, &volumes[index]);
    if (status != PNF_OK) {
      fprintf(stderr, "volume %s: import failed\n", volumes[index].name);
    }
  }
  if (status == PNF_OK) {
    status = write_metadata(&context, volumes, count);
  }
  if (status != PNF_OK) {
    native_report("format", status, &context.image);
    struct stat current;
    if (output_known && lstat(image_path, &current) == 0 &&
        output.st_dev == current.st_dev && output.st_ino == current.st_ino &&
        unlink(image_path) != 0) {
      fprintf(stderr, "%s: cannot remove incomplete image: %s\n",
              image_path, strerror(errno));
    }
  }
  enum pnf_status close_status = native_image_close(&context.image);
  if (status != PNF_OK) {
    return EXIT_FAILURE;
  }
  if (close_status != PNF_OK) {
    struct stat current;
    if (output_known && lstat(image_path, &current) == 0 &&
        output.st_dev == current.st_dev && output.st_ino == current.st_ino &&
        unlink(image_path) != 0) {
      fprintf(stderr, "%s: cannot remove image after close failure: %s\n",
              image_path, strerror(errno));
    }
    return native_report("close formatted image", close_status, &context.image);
  }
  printf("formatted %s: %" PRIu64 " blocks, %u volumes, %" PRIu64
         " journal blocks\n", image_path, size / PNF_BLOCK_SIZE, count,
         journal / PNF_BLOCK_SIZE);
  return EXIT_SUCCESS;
}
