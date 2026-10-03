/* SPDX-License-Identifier: MPL-2.0 */
#include "check.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct check_state {
  struct npfs_image *image;
  uint8_t *owned;
};

struct check_volume {
  struct check_state *state;
  struct npfs_volume *volume;
  struct npfs_inode *inodes;
  uint64_t *references;
  uint8_t *listed;
  uint8_t *colors;
  uint64_t count;
};

struct check_name {
  uint16_t length;
  uint8_t bytes[NPFS_NAME_MAX];
};

struct check_directory {
  struct check_volume *volume;
  uint64_t number;
  struct check_name *names;
  size_t count;
  size_t capacity;
};

static enum npfs_status
check_error(struct npfs_image *image, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(image->error, sizeof(image->error), format, arguments);
  va_end(arguments);
  return NPFS_CORRUPT;
}

static bool
bit_get(const uint8_t *bits, uint64_t number)
{
  return (bits[number / 8] & (1u << (number % 8))) != 0;
}

static void
bit_set(uint8_t *bits, uint64_t number)
{
  bits[number / 8] |= 1u << (number % 8);
}

static enum npfs_status
claim_block(struct check_state *state, uint64_t number)
{
  if (!npfs_data_block_valid(&state->image->header, number))
    return check_error(state->image, "mapping points outside ordinary storage: %llu",
                       (unsigned long long)number);
  if (bit_get(state->owned, number))
    return check_error(state->image, "block %llu has more than one owner",
                       (unsigned long long)number);
  bit_set(state->owned, number);
  return NPFS_OK;
}

typedef enum npfs_status (*check_leaf)(void *context, uint64_t logical, uint64_t physical);

/* Three fixed frames cover the format's maximum indirection depth. Claim before
 * reading an indirect block, so aliases and cycles never get followed twice. */
static enum npfs_status
check_mapping(struct check_state *state, const uint64_t pointers[NPFS_POINTER_COUNT],
              uint64_t size, bool dense, bool allow_extra, check_leaf leaf, void *context)
{
  struct map_frame {
    uint8_t bytes[NPFS_BLOCK_SIZE];
    uint64_t base;
    uint64_t span;
    unsigned level;
    unsigned index;
  } frames[3];
  uint64_t limit = size / NPFS_BLOCK_SIZE + (size % NPFS_BLOCK_SIZE != 0);
  uint64_t found = 0;
  enum npfs_status status;
  for (unsigned i = 0; i < NPFS_DIRECT_COUNT; i++) {
    if (!pointers[i])
      continue;
    status = claim_block(state, pointers[i]);
    if (status != NPFS_OK)
      return status;
    if (i >= limit && !allow_extra)
      return check_error(state->image, "mapping beyond file length");
    if (i < limit)
      found++;
    if (leaf && (status = leaf(context, i, pointers[i])) != NPFS_OK)
      return status;
  }
  uint64_t base = NPFS_DIRECT_COUNT;
  uint64_t coverage = NPFS_INDIRECT_COUNT;
  for (unsigned level = 1; level <= 3; level++) {
    uint64_t root = pointers[NPFS_DIRECT_COUNT + level - 1];
    if (root) {
      if (base >= limit && !allow_extra)
        return check_error(state->image, "indirect mapping beyond file length");
      status = claim_block(state, root);
      if (status != NPFS_OK)
        return status;
      frames[0].base = base;
      frames[0].span = coverage / NPFS_INDIRECT_COUNT;
      frames[0].level = level;
      frames[0].index = 0;
      status = npfs_read_blocks(state->image, root, 1, frames[0].bytes);
      if (status != NPFS_OK)
        return status;
      unsigned depth = 1;
      while (depth) {
        struct map_frame *frame = &frames[depth - 1];
        if (frame->index == NPFS_INDIRECT_COUNT) {
          depth--;
          continue;
        }
        unsigned index = frame->index++;
        uint64_t physical = npfs_get_u64(frame->bytes + index * 8);
        if (!physical)
          continue;
        uint64_t logical = frame->base + index * frame->span;
        if (logical >= limit && !allow_extra)
          return check_error(state->image, "mapping beyond file length");
        status = claim_block(state, physical);
        if (status != NPFS_OK)
          return status;
        if (frame->level == 1) {
          if (logical < limit)
            found++;
          if (leaf && (status = leaf(context, logical, physical)) != NPFS_OK)
            return status;
        } else {
          struct map_frame *child = &frames[depth++];
          child->base = logical;
          child->span = frame->span / NPFS_INDIRECT_COUNT;
          child->level = frame->level - 1;
          child->index = 0;
          status = npfs_read_blocks(state->image, physical, 1, child->bytes);
          if (status != NPFS_OK)
            return status;
        }
      }
    }
    base += coverage;
    coverage *= NPFS_INDIRECT_COUNT;
  }
  if (dense && found != limit)
    return check_error(state->image, "hole in dense metadata file");
  return NPFS_OK;
}

static enum npfs_status
read_inode_block(void *context, uint64_t logical, uint64_t physical)
{
  struct check_volume *volume = context;
  uint8_t block[NPFS_BLOCK_SIZE];
  enum npfs_status status = npfs_read_blocks(volume->state->image, physical, 1, block);
  if (status != NPFS_OK)
    return status;
  for (unsigned i = 0; i < NPFS_BLOCK_SIZE / NPFS_INODE_SIZE; i++) {
    uint64_t number = logical * (NPFS_BLOCK_SIZE / NPFS_INODE_SIZE) + i;
    if (number >= volume->count)
      break;
    status = npfs_inode_decode(&volume->state->image->header,
                              block + i * NPFS_INODE_SIZE, &volume->inodes[number]);
    if (status != NPFS_OK)
      return check_error(volume->state->image, "invalid inode %llu",
                         (unsigned long long)number);
  }
  return NPFS_OK;
}

static int
name_compare(const void *left, const void *right)
{
  const struct check_name *a = left;
  const struct check_name *b = right;
  size_t common = a->length < b->length ? a->length : b->length;
  int order = memcmp(a->bytes, b->bytes, common);
  if (order)
    return order;
  return (a->length > b->length) - (a->length < b->length);
}

static enum npfs_status
read_directory_block(void *context, uint64_t logical, uint64_t physical)
{
  (void)logical;
  struct check_directory *directory = context;
  struct check_volume *volume = directory->volume;
  struct npfs_image *image = volume->state->image;
  uint8_t block[NPFS_BLOCK_SIZE];
  enum npfs_status status = npfs_read_blocks(image, physical, 1, block);
  if (status != NPFS_OK)
    return status;
  size_t offset = 0;
  while (offset < NPFS_BLOCK_SIZE) {
    struct npfs_dirent entry;
    status = npfs_dirent_decode(&image->header, block + offset,
                               NPFS_BLOCK_SIZE - offset, &entry);
    if (status != NPFS_OK)
      return check_error(image, "invalid directory record in inode %llu",
                         (unsigned long long)directory->number);
    offset += entry.record_length;
    if (!entry.inode)
      continue;
    if (entry.inode >= volume->count || volume->inodes[entry.inode].kind == NPFS_INODE_FREE)
      return check_error(image, "directory references absent inode");
    struct npfs_inode *target = &volume->inodes[entry.inode];
    if (target->cleanup & NPFS_CLEANUP_DETACHED)
      return check_error(image, "directory references detached inode");
    if (++volume->references[entry.inode] != 1)
      return check_error(image, "inode has more than one directory entry");
    if (target->kind == NPFS_INODE_DIRECTORY && target->parent != directory->number)
      return check_error(image, "directory parent backlink disagrees");
    if (directory->count == directory->capacity) {
      size_t capacity = directory->capacity ? directory->capacity * 2 : 32;
      if (capacity < directory->capacity || capacity > SIZE_MAX / sizeof(*directory->names))
        return NPFS_NO_MEMORY;
      void *names = realloc(directory->names, capacity * sizeof(*directory->names));
      if (!names)
        return NPFS_NO_MEMORY;
      directory->names = names;
      directory->capacity = capacity;
    }
    struct check_name *name = &directory->names[directory->count++];
    name->length = entry.name_length;
    memcpy(name->bytes, entry.name, entry.name_length);
  }
  return NPFS_OK;
}

static enum npfs_status
check_one_volume(struct check_state *state, struct npfs_volume *record)
{
  struct check_volume volume = { .state = state, .volume = record };
  struct npfs_image *image = state->image;
  volume.count = record->inode_bytes / NPFS_INODE_SIZE;
  if (record->root_inode != 1 || volume.count < 2 || record->inode_bytes % NPFS_INODE_SIZE)
    return check_error(image, "invalid inode-file geometry");
  if (volume.count > SIZE_MAX / sizeof(*volume.inodes) ||
      volume.count > SIZE_MAX / sizeof(*volume.references))
    return NPFS_NO_MEMORY;
  volume.inodes = calloc((size_t)volume.count, sizeof(*volume.inodes));
  volume.references = calloc((size_t)volume.count, sizeof(*volume.references));
  volume.listed = calloc((size_t)volume.count, 1);
  volume.colors = calloc((size_t)volume.count, 1);
  uint64_t *trail = calloc((size_t)volume.count, sizeof(*trail));
  enum npfs_status status = NPFS_NO_MEMORY;
  if (!volume.inodes || !volume.references || !volume.listed || !volume.colors || !trail)
    goto done;
  status = check_mapping(state, record->pointers, record->inode_bytes, true, false,
                         read_inode_block, &volume);
  if (status != NPFS_OK)
    goto done;
  if (volume.inodes[0].kind != NPFS_INODE_FREE ||
      volume.inodes[1].kind != NPFS_INODE_DIRECTORY || volume.inodes[1].parent != 1 ||
      volume.inodes[1].cleanup & NPFS_CLEANUP_DETACHED) {
    status = check_error(image, "invalid reserved inode or root inode");
    goto done;
  }
  for (uint64_t number = 1; number < volume.count; number++) {
    struct npfs_inode *inode = &volume.inodes[number];
    if (inode->kind == NPFS_INODE_FREE)
      continue;
    if (inode->kind == NPFS_INODE_FILE && inode->parent != 0) {
      status = check_error(image, "regular inode has directory parent");
      goto done;
    }
    if ((inode->cleanup & NPFS_CLEANUP_SHRINK) && inode->shrink_target != inode->size) {
      status = check_error(image, "shrink target disagrees with inode size");
      goto done;
    }
    bool detached = (inode->cleanup & NPFS_CLEANUP_DETACHED) != 0;
    if (inode->kind == NPFS_INODE_DIRECTORY && detached && inode->parent != 0) {
      status = check_error(image, "detached directory retains parent");
      goto done;
    }
    struct check_directory directory = { .volume = &volume, .number = number };
    bool is_directory = inode->kind == NPFS_INODE_DIRECTORY;
    /* Detached directories may already have reclaimed their highest blocks.
     * Their remaining records must still be empty, but size is not progress. */
    status = check_mapping(state, inode->pointers, inode->size, is_directory && !detached,
                           !is_directory && inode->cleanup != 0,
                           is_directory ? read_directory_block : NULL, &directory);
    if (status == NPFS_OK && directory.count > 1) {
      qsort(directory.names, directory.count, sizeof(*directory.names), name_compare);
      for (size_t i = 1; i < directory.count; i++) {
        if (name_compare(&directory.names[i - 1], &directory.names[i]) == 0) {
          status = check_error(image, "duplicate directory name");
          break;
        }
      }
    }
    if (status == NPFS_OK && detached && directory.count)
      status = check_error(image, "detached directory contains linked entries");
    free(directory.names);
    if (status != NPFS_OK)
      goto done;
  }
  uint64_t number = record->cleanup_head;
  while (number) {
    if (number >= volume.count || volume.listed[number] || !volume.inodes[number].cleanup) {
      status = check_error(image, "invalid or cyclic cleanup list");
      goto done;
    }
    volume.listed[number] = 1;
    number = volume.inodes[number].cleanup_next;
  }
  volume.colors[1] = 2;
  for (number = 1; number < volume.count; number++) {
    struct npfs_inode *inode = &volume.inodes[number];
    if ((inode->cleanup != 0) != (volume.listed[number] != 0)) {
      status = check_error(image, "cleanup membership disagrees with inode state");
      goto done;
    }
    if (inode->kind == NPFS_INODE_FREE)
      continue;
    uint64_t expected = number == 1 || (inode->cleanup & NPFS_CLEANUP_DETACHED) ? 0 : 1;
    if (volume.references[number] != expected) {
      status = check_error(image, "inode reachability disagrees with cleanup state");
      goto done;
    }
    if (inode->kind != NPFS_INODE_DIRECTORY || inode->cleanup & NPFS_CLEANUP_DETACHED)
      continue;
    uint64_t current = number;
    size_t length = 0;
    while (volume.colors[current] != 2) {
      if (volume.colors[current] == 1) {
        status = check_error(image, "cycle in directory ancestry");
        goto done;
      }
      volume.colors[current] = 1;
      trail[length++] = current;
      current = volume.inodes[current].parent;
      if (!current || current >= volume.count ||
          volume.inodes[current].kind != NPFS_INODE_DIRECTORY ||
          volume.inodes[current].cleanup & NPFS_CLEANUP_DETACHED) {
        status = check_error(image, "directory ancestry does not reach root");
        goto done;
      }
    }
    while (length)
      volume.colors[trail[--length]] = 2;
  }
  status = NPFS_OK;
done:
  free(trail);
  free(volume.colors);
  free(volume.listed);
  free(volume.references);
  free(volume.inodes);
  return status;
}

static bool
fixed_block(const struct npfs_header *header, uint64_t number)
{
  return !npfs_data_block_valid(header, number);
}

enum npfs_status
npfs_check_image(struct npfs_image *image)
{
  if (image->control.state != NPFS_JOURNAL_EMPTY)
    return NPFS_RECOVERY_REQUIRED;
  uint64_t bytes = image->header.pool_blocks / 8 + (image->header.pool_blocks % 8 != 0);
  if (bytes > SIZE_MAX)
    return NPFS_NO_MEMORY;
  struct check_state state = { .image = image, .owned = calloc((size_t)bytes, 1) };
  if (!state.owned)
    return NPFS_NO_MEMORY;
  struct npfs_volume volumes[NPFS_VOLUME_COUNT];
  enum npfs_status status = npfs_read_volumes(image, volumes);
  if (status != NPFS_OK)
    goto done;
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; i++) {
    if (volumes[i].state == NPFS_VOLUME_UNUSED)
      continue;
    status = check_one_volume(&state, &volumes[i]);
    if (status != NPFS_OK)
      goto done;
  }
  uint8_t bitmap[NPFS_BLOCK_SIZE];
  for (uint64_t i = 0; i < image->header.bitmap_blocks; i++) {
    status = npfs_read_blocks(image, image->header.bitmap_start + i, 1, bitmap);
    if (status != NPFS_OK)
      goto done;
    for (unsigned bit = 0; bit < NPFS_BITMAP_BITS; bit++) {
      uint64_t number = i * NPFS_BITMAP_BITS + bit;
      bool expected = number >= image->header.pool_blocks || fixed_block(&image->header, number) ||
                      bit_get(state.owned, number);
      if (bit_get(bitmap, bit) != expected) {
        status = check_error(image, "bitmap ownership mismatch at block %llu",
                             (unsigned long long)number);
        goto done;
      }
    }
  }
  status = NPFS_OK;
done:
  free(state.owned);
  return status;
}

static enum npfs_status
check_log_image(struct npfs_image *image, const struct npfs_descriptor *descriptor,
                const uint8_t bytes[NPFS_BLOCK_SIZE])
{
  enum npfs_status status;
  if (descriptor->kind == NPFS_METADATA_BITMAP) {
    uint64_t base = (descriptor->home - image->header.bitmap_start) * NPFS_BITMAP_BITS;
    for (unsigned bit = 0; bit < NPFS_BITMAP_BITS; bit++) {
      uint64_t number = base + bit;
      if ((number >= image->header.pool_blocks || fixed_block(&image->header, number)) &&
          !bit_get(bytes, bit))
        return check_error(image, "journal bitmap clears permanent allocation bit");
    }
  } else if (descriptor->kind == NPFS_METADATA_VOLUMES) {
    for (unsigned i = 0; i < NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE; i++) {
      struct npfs_volume volume;
      status = npfs_volume_decode(&image->header, bytes + i * NPFS_VOLUME_SIZE, &volume);
      if (status != NPFS_OK)
        return status;
    }
  } else if (descriptor->kind == NPFS_METADATA_INODES) {
    for (unsigned i = 0; i < NPFS_BLOCK_SIZE / NPFS_INODE_SIZE; i++) {
      struct npfs_inode inode;
      status = npfs_inode_decode(&image->header, bytes + i * NPFS_INODE_SIZE, &inode);
      if (status != NPFS_OK)
        return status;
    }
  } else if (descriptor->kind == NPFS_METADATA_DIRECTORY) {
    size_t offset = 0;
    while (offset < NPFS_BLOCK_SIZE) {
      struct npfs_dirent entry;
      status = npfs_dirent_decode(&image->header, bytes + offset,
                                 NPFS_BLOCK_SIZE - offset, &entry);
      if (status != NPFS_OK)
        return status;
      offset += entry.record_length;
    }
  } else if (descriptor->kind == NPFS_METADATA_INDIRECT) {
    for (unsigned i = 0; i < NPFS_INDIRECT_COUNT; i++) {
      uint64_t pointer = npfs_get_u64(bytes + i * 8);
      if (pointer && !npfs_data_block_valid(&image->header, pointer))
        return NPFS_CORRUPT;
    }
  }
  return NPFS_OK;
}

enum npfs_status
npfs_replay(struct npfs_image *image)
{
  if (!image->writable)
    return NPFS_INVALID;
  if (image->control.state == NPFS_JOURNAL_EMPTY)
    return NPFS_OK;
  if (image->control.state != NPFS_JOURNAL_COMMITTED)
    return check_error(image, "unknown selected journal state");
  if (image->control.sequence == UINT64_MAX)
    return NPFS_UNSUPPORTED;
  size_t count = image->control.image_count;
  size_t descriptor_blocks = image->control.descriptor_blocks;
  uint64_t expected_descriptors = ((uint64_t)count + NPFS_DESCRIPTORS_PER_BLOCK - 1) /
                                  NPFS_DESCRIPTORS_PER_BLOCK;
  if (!count || descriptor_blocks != expected_descriptors ||
      (uint64_t)count > npfs_journal_capacity(image->header.journal_blocks) ||
      2 + expected_descriptors + (uint64_t)count > image->header.journal_blocks)
    return check_error(image, "invalid committed journal counts");
  if (count > SIZE_MAX / NPFS_BLOCK_SIZE ||
      count > SIZE_MAX / sizeof(struct npfs_descriptor))
    return NPFS_NO_MEMORY;
  struct npfs_descriptor *descriptors = calloc(count, sizeof(*descriptors));
  uint8_t *images = malloc(count * NPFS_BLOCK_SIZE);
  uint64_t owned_bytes = image->header.pool_blocks / 8 + (image->header.pool_blocks % 8 != 0);
  uint8_t *targets = owned_bytes <= SIZE_MAX ? calloc((size_t)owned_bytes, 1) : NULL;
  enum npfs_status status = NPFS_NO_MEMORY;
  if (!descriptors || !images || !targets)
    goto done;
  uint32_t crc = npfs_payload_begin(&image->control);
  uint8_t block[NPFS_BLOCK_SIZE];
  for (size_t i = 0; i < descriptor_blocks; i++) {
    status = npfs_read_blocks(image, image->header.journal_start + 2 + i, 1, block);
    if (status != NPFS_OK)
      goto done;
    crc = npfs_crc_update(crc, block, sizeof(block));
    for (unsigned j = 0; j < NPFS_DESCRIPTORS_PER_BLOCK; j++) {
      size_t index = i * NPFS_DESCRIPTORS_PER_BLOCK + j;
      if (index >= count) {
        for (unsigned k = 0; k < NPFS_DESCRIPTOR_SIZE; k++) {
          if (block[j * NPFS_DESCRIPTOR_SIZE + k]) {
            status = check_error(image, "nonzero unused journal descriptor");
            goto done;
          }
        }
        continue;
      }
      status = npfs_descriptor_decode(&image->header,
                                      block + j * NPFS_DESCRIPTOR_SIZE, &descriptors[index]);
      if (status != NPFS_OK)
        goto done;
      uint64_t home = descriptors[index].home;
      if (home >= image->header.pool_blocks) {
        status = check_error(image, "journal target outside pool");
        goto done;
      }
      if (bit_get(targets, home)) {
        status = check_error(image, "duplicate journal home target");
        goto done;
      }
      bit_set(targets, home);
    }
  }
  for (size_t i = 0; i < count; i++) {
    uint8_t *bytes = images + i * NPFS_BLOCK_SIZE;
    status = npfs_read_blocks(image, image->header.journal_start + 2 + descriptor_blocks + i,
                                 1, bytes);
    if (status != NPFS_OK)
      goto done;
    crc = npfs_crc_update(crc, bytes, NPFS_BLOCK_SIZE);
    status = check_log_image(image, &descriptors[i], bytes);
    if (status != NPFS_OK)
      goto done;
  }
  if (npfs_crc_finish(crc) != image->control.payload_crc) {
    status = check_error(image, "committed journal payload checksum mismatch");
    goto done;
  }
  for (size_t i = 0; i < count; i++) {
    status = npfs_write_blocks(image, descriptors[i].home, 1, images + i * NPFS_BLOCK_SIZE);
    if (status != NPFS_OK)
      goto done;
  }
  status = npfs_flush(image);
  if (status != NPFS_OK)
    goto done;
  struct npfs_control empty = image->control;
  empty.sequence++;
  empty.state = NPFS_JOURNAL_EMPTY;
  empty.image_count = 0;
  empty.descriptor_blocks = 0;
  empty.payload_crc = 0;
  status = npfs_control_encode(&image->header, &empty, block);
  if (status != NPFS_OK)
    goto done;
  unsigned slot = image->control_slot ^ 1;
  status = npfs_write_blocks(image, image->header.journal_start + slot, 1, block);
  if (status != NPFS_OK)
    goto done;
  status = npfs_flush(image);
  if (status != NPFS_OK)
    goto done;
  image->control = empty;
  image->control_slot = slot;
done:
  free(targets);
  free(images);
  free(descriptors);
  return status;
}
