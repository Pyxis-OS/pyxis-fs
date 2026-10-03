/* SPDX-License-Identifier: MPL-2.0 */
#include "check.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct check_state {
  struct native_image *image;
  uint8_t *owned;
};

struct check_volume {
  struct check_state *state;
  struct pnf_volume *volume;
  struct pnf_inode *inodes;
  uint64_t *references;
  uint8_t *listed;
  uint8_t *colors;
  uint64_t count;
};

struct check_name {
  uint16_t length;
  uint8_t bytes[PNF_NAME_MAX];
};

struct check_directory {
  struct check_volume *volume;
  uint64_t number;
  struct check_name *names;
  size_t count;
  size_t capacity;
};

static enum pnf_status
check_error(struct native_image *image, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(image->error, sizeof(image->error), format, arguments);
  va_end(arguments);
  return PNF_CORRUPT;
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

static enum pnf_status
claim_block(struct check_state *state, uint64_t number)
{
  if (!pnf_data_block_valid(&state->image->header, number))
    return check_error(state->image, "mapping points outside ordinary storage: %llu",
                       (unsigned long long)number);
  if (bit_get(state->owned, number))
    return check_error(state->image, "block %llu has more than one owner",
                       (unsigned long long)number);
  bit_set(state->owned, number);
  return PNF_OK;
}

typedef enum pnf_status (*check_leaf)(void *context, uint64_t logical, uint64_t physical);

/* Three fixed frames cover the format's maximum indirection depth. Claim before
 * reading an indirect block, so aliases and cycles never get followed twice. */
static enum pnf_status
check_mapping(struct check_state *state, const uint64_t pointers[PNF_POINTER_COUNT],
              uint64_t size, bool dense, bool allow_extra, check_leaf leaf, void *context)
{
  struct map_frame {
    uint8_t bytes[PNF_BLOCK_SIZE];
    uint64_t base;
    uint64_t span;
    unsigned level;
    unsigned index;
  } frames[3];
  uint64_t limit = size / PNF_BLOCK_SIZE + (size % PNF_BLOCK_SIZE != 0);
  uint64_t found = 0;
  enum pnf_status status;
  for (unsigned i = 0; i < PNF_DIRECT_COUNT; i++) {
    if (!pointers[i])
      continue;
    status = claim_block(state, pointers[i]);
    if (status != PNF_OK)
      return status;
    if (i >= limit && !allow_extra)
      return check_error(state->image, "mapping beyond file length");
    if (i < limit)
      found++;
    if (leaf && (status = leaf(context, i, pointers[i])) != PNF_OK)
      return status;
  }
  uint64_t base = PNF_DIRECT_COUNT;
  uint64_t coverage = PNF_INDIRECT_COUNT;
  for (unsigned level = 1; level <= 3; level++) {
    uint64_t root = pointers[PNF_DIRECT_COUNT + level - 1];
    if (root) {
      if (base >= limit && !allow_extra)
        return check_error(state->image, "indirect mapping beyond file length");
      status = claim_block(state, root);
      if (status != PNF_OK)
        return status;
      frames[0].base = base;
      frames[0].span = coverage / PNF_INDIRECT_COUNT;
      frames[0].level = level;
      frames[0].index = 0;
      status = native_read_blocks(state->image, root, 1, frames[0].bytes);
      if (status != PNF_OK)
        return status;
      unsigned depth = 1;
      while (depth) {
        struct map_frame *frame = &frames[depth - 1];
        if (frame->index == PNF_INDIRECT_COUNT) {
          depth--;
          continue;
        }
        unsigned index = frame->index++;
        uint64_t physical = pnf_get_u64(frame->bytes + index * 8);
        if (!physical)
          continue;
        uint64_t logical = frame->base + index * frame->span;
        if (logical >= limit && !allow_extra)
          return check_error(state->image, "mapping beyond file length");
        status = claim_block(state, physical);
        if (status != PNF_OK)
          return status;
        if (frame->level == 1) {
          if (logical < limit)
            found++;
          if (leaf && (status = leaf(context, logical, physical)) != PNF_OK)
            return status;
        } else {
          struct map_frame *child = &frames[depth++];
          child->base = logical;
          child->span = frame->span / PNF_INDIRECT_COUNT;
          child->level = frame->level - 1;
          child->index = 0;
          status = native_read_blocks(state->image, physical, 1, child->bytes);
          if (status != PNF_OK)
            return status;
        }
      }
    }
    base += coverage;
    coverage *= PNF_INDIRECT_COUNT;
  }
  if (dense && found != limit)
    return check_error(state->image, "hole in dense metadata file");
  return PNF_OK;
}

static enum pnf_status
read_inode_block(void *context, uint64_t logical, uint64_t physical)
{
  struct check_volume *volume = context;
  uint8_t block[PNF_BLOCK_SIZE];
  enum pnf_status status = native_read_blocks(volume->state->image, physical, 1, block);
  if (status != PNF_OK)
    return status;
  for (unsigned i = 0; i < PNF_BLOCK_SIZE / PNF_INODE_SIZE; i++) {
    uint64_t number = logical * (PNF_BLOCK_SIZE / PNF_INODE_SIZE) + i;
    if (number >= volume->count)
      break;
    status = pnf_inode_decode(&volume->state->image->header,
                              block + i * PNF_INODE_SIZE, &volume->inodes[number]);
    if (status != PNF_OK)
      return check_error(volume->state->image, "invalid inode %llu",
                         (unsigned long long)number);
  }
  return PNF_OK;
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

static enum pnf_status
read_directory_block(void *context, uint64_t logical, uint64_t physical)
{
  (void)logical;
  struct check_directory *directory = context;
  struct check_volume *volume = directory->volume;
  struct native_image *image = volume->state->image;
  uint8_t block[PNF_BLOCK_SIZE];
  enum pnf_status status = native_read_blocks(image, physical, 1, block);
  if (status != PNF_OK)
    return status;
  size_t offset = 0;
  while (offset < PNF_BLOCK_SIZE) {
    struct pnf_dirent entry;
    status = pnf_dirent_decode(&image->header, block + offset,
                               PNF_BLOCK_SIZE - offset, &entry);
    if (status != PNF_OK)
      return check_error(image, "invalid directory record in inode %llu",
                         (unsigned long long)directory->number);
    offset += entry.record_length;
    if (!entry.inode)
      continue;
    if (entry.inode >= volume->count || volume->inodes[entry.inode].kind == PNF_INODE_FREE)
      return check_error(image, "directory references absent inode");
    struct pnf_inode *target = &volume->inodes[entry.inode];
    if (target->cleanup & PNF_CLEANUP_DETACHED)
      return check_error(image, "directory references detached inode");
    if (++volume->references[entry.inode] != 1)
      return check_error(image, "inode has more than one directory entry");
    if (target->kind == PNF_INODE_DIRECTORY && target->parent != directory->number)
      return check_error(image, "directory parent backlink disagrees");
    if (directory->count == directory->capacity) {
      size_t capacity = directory->capacity ? directory->capacity * 2 : 32;
      if (capacity < directory->capacity || capacity > SIZE_MAX / sizeof(*directory->names))
        return PNF_NO_MEMORY;
      void *names = realloc(directory->names, capacity * sizeof(*directory->names));
      if (!names)
        return PNF_NO_MEMORY;
      directory->names = names;
      directory->capacity = capacity;
    }
    struct check_name *name = &directory->names[directory->count++];
    name->length = entry.name_length;
    memcpy(name->bytes, entry.name, entry.name_length);
  }
  return PNF_OK;
}

static enum pnf_status
check_one_volume(struct check_state *state, struct pnf_volume *record)
{
  struct check_volume volume = { .state = state, .volume = record };
  struct native_image *image = state->image;
  volume.count = record->inode_bytes / PNF_INODE_SIZE;
  if (record->root_inode != 1 || volume.count < 2 || record->inode_bytes % PNF_INODE_SIZE)
    return check_error(image, "invalid inode-file geometry");
  if (volume.count > SIZE_MAX / sizeof(*volume.inodes) ||
      volume.count > SIZE_MAX / sizeof(*volume.references))
    return PNF_NO_MEMORY;
  volume.inodes = calloc((size_t)volume.count, sizeof(*volume.inodes));
  volume.references = calloc((size_t)volume.count, sizeof(*volume.references));
  volume.listed = calloc((size_t)volume.count, 1);
  volume.colors = calloc((size_t)volume.count, 1);
  uint64_t *trail = calloc((size_t)volume.count, sizeof(*trail));
  enum pnf_status status = PNF_NO_MEMORY;
  if (!volume.inodes || !volume.references || !volume.listed || !volume.colors || !trail)
    goto done;
  status = check_mapping(state, record->pointers, record->inode_bytes, true, false,
                         read_inode_block, &volume);
  if (status != PNF_OK)
    goto done;
  if (volume.inodes[0].kind != PNF_INODE_FREE ||
      volume.inodes[1].kind != PNF_INODE_DIRECTORY || volume.inodes[1].parent != 1 ||
      volume.inodes[1].cleanup & PNF_CLEANUP_DETACHED) {
    status = check_error(image, "invalid reserved inode or root inode");
    goto done;
  }
  for (uint64_t number = 1; number < volume.count; number++) {
    struct pnf_inode *inode = &volume.inodes[number];
    if (inode->kind == PNF_INODE_FREE)
      continue;
    if (inode->kind == PNF_INODE_FILE && inode->parent != 0) {
      status = check_error(image, "regular inode has directory parent");
      goto done;
    }
    if ((inode->cleanup & PNF_CLEANUP_SHRINK) && inode->shrink_target != inode->size) {
      status = check_error(image, "shrink target disagrees with inode size");
      goto done;
    }
    bool detached = (inode->cleanup & PNF_CLEANUP_DETACHED) != 0;
    if (inode->kind == PNF_INODE_DIRECTORY && detached && inode->parent != 0) {
      status = check_error(image, "detached directory retains parent");
      goto done;
    }
    struct check_directory directory = { .volume = &volume, .number = number };
    bool is_directory = inode->kind == PNF_INODE_DIRECTORY;
    /* Detached directories may already have reclaimed their highest blocks.
     * Their remaining records must still be empty, but size is not progress. */
    status = check_mapping(state, inode->pointers, inode->size, is_directory && !detached,
                           !is_directory && inode->cleanup != 0,
                           is_directory ? read_directory_block : NULL, &directory);
    if (status == PNF_OK && directory.count > 1) {
      qsort(directory.names, directory.count, sizeof(*directory.names), name_compare);
      for (size_t i = 1; i < directory.count; i++) {
        if (name_compare(&directory.names[i - 1], &directory.names[i]) == 0) {
          status = check_error(image, "duplicate directory name");
          break;
        }
      }
    }
    if (status == PNF_OK && detached && directory.count)
      status = check_error(image, "detached directory contains linked entries");
    free(directory.names);
    if (status != PNF_OK)
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
    struct pnf_inode *inode = &volume.inodes[number];
    if ((inode->cleanup != 0) != (volume.listed[number] != 0)) {
      status = check_error(image, "cleanup membership disagrees with inode state");
      goto done;
    }
    if (inode->kind == PNF_INODE_FREE)
      continue;
    uint64_t expected = number == 1 || (inode->cleanup & PNF_CLEANUP_DETACHED) ? 0 : 1;
    if (volume.references[number] != expected) {
      status = check_error(image, "inode reachability disagrees with cleanup state");
      goto done;
    }
    if (inode->kind != PNF_INODE_DIRECTORY || inode->cleanup & PNF_CLEANUP_DETACHED)
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
          volume.inodes[current].kind != PNF_INODE_DIRECTORY ||
          volume.inodes[current].cleanup & PNF_CLEANUP_DETACHED) {
        status = check_error(image, "directory ancestry does not reach root");
        goto done;
      }
    }
    while (length)
      volume.colors[trail[--length]] = 2;
  }
  status = PNF_OK;
done:
  free(trail);
  free(volume.colors);
  free(volume.listed);
  free(volume.references);
  free(volume.inodes);
  return status;
}

static bool
fixed_block(const struct pnf_header *header, uint64_t number)
{
  return !pnf_data_block_valid(header, number);
}

enum pnf_status
native_check_image(struct native_image *image)
{
  if (image->control.state != PNF_JOURNAL_EMPTY)
    return PNF_RECOVERY_REQUIRED;
  uint64_t bytes = image->header.pool_blocks / 8 + (image->header.pool_blocks % 8 != 0);
  if (bytes > SIZE_MAX)
    return PNF_NO_MEMORY;
  struct check_state state = { .image = image, .owned = calloc((size_t)bytes, 1) };
  if (!state.owned)
    return PNF_NO_MEMORY;
  struct pnf_volume volumes[PNF_VOLUME_COUNT];
  enum pnf_status status = native_read_volumes(image, volumes);
  if (status != PNF_OK)
    goto done;
  for (unsigned i = 0; i < PNF_VOLUME_COUNT; i++) {
    if (volumes[i].state == PNF_VOLUME_UNUSED)
      continue;
    status = check_one_volume(&state, &volumes[i]);
    if (status != PNF_OK)
      goto done;
  }
  uint8_t bitmap[PNF_BLOCK_SIZE];
  for (uint64_t i = 0; i < image->header.bitmap_blocks; i++) {
    status = native_read_blocks(image, image->header.bitmap_start + i, 1, bitmap);
    if (status != PNF_OK)
      goto done;
    for (unsigned bit = 0; bit < PNF_BITMAP_BITS; bit++) {
      uint64_t number = i * PNF_BITMAP_BITS + bit;
      bool expected = number >= image->header.pool_blocks || fixed_block(&image->header, number) ||
                      bit_get(state.owned, number);
      if (bit_get(bitmap, bit) != expected) {
        status = check_error(image, "bitmap ownership mismatch at block %llu",
                             (unsigned long long)number);
        goto done;
      }
    }
  }
  status = PNF_OK;
done:
  free(state.owned);
  return status;
}

static enum pnf_status
check_log_image(struct native_image *image, const struct pnf_descriptor *descriptor,
                const uint8_t bytes[PNF_BLOCK_SIZE])
{
  enum pnf_status status;
  if (descriptor->kind == PNF_METADATA_BITMAP) {
    uint64_t base = (descriptor->home - image->header.bitmap_start) * PNF_BITMAP_BITS;
    for (unsigned bit = 0; bit < PNF_BITMAP_BITS; bit++) {
      uint64_t number = base + bit;
      if ((number >= image->header.pool_blocks || fixed_block(&image->header, number)) &&
          !bit_get(bytes, bit))
        return check_error(image, "journal bitmap clears permanent allocation bit");
    }
  } else if (descriptor->kind == PNF_METADATA_VOLUMES) {
    for (unsigned i = 0; i < PNF_BLOCK_SIZE / PNF_VOLUME_SIZE; i++) {
      struct pnf_volume volume;
      status = pnf_volume_decode(&image->header, bytes + i * PNF_VOLUME_SIZE, &volume);
      if (status != PNF_OK)
        return status;
    }
  } else if (descriptor->kind == PNF_METADATA_INODES) {
    for (unsigned i = 0; i < PNF_BLOCK_SIZE / PNF_INODE_SIZE; i++) {
      struct pnf_inode inode;
      status = pnf_inode_decode(&image->header, bytes + i * PNF_INODE_SIZE, &inode);
      if (status != PNF_OK)
        return status;
    }
  } else if (descriptor->kind == PNF_METADATA_DIRECTORY) {
    size_t offset = 0;
    while (offset < PNF_BLOCK_SIZE) {
      struct pnf_dirent entry;
      status = pnf_dirent_decode(&image->header, bytes + offset,
                                 PNF_BLOCK_SIZE - offset, &entry);
      if (status != PNF_OK)
        return status;
      offset += entry.record_length;
    }
  } else if (descriptor->kind == PNF_METADATA_INDIRECT) {
    for (unsigned i = 0; i < PNF_INDIRECT_COUNT; i++) {
      uint64_t pointer = pnf_get_u64(bytes + i * 8);
      if (pointer && !pnf_data_block_valid(&image->header, pointer))
        return PNF_CORRUPT;
    }
  }
  return PNF_OK;
}

enum pnf_status
native_replay(struct native_image *image)
{
  if (!image->writable)
    return PNF_INVALID;
  if (image->control.state == PNF_JOURNAL_EMPTY)
    return PNF_OK;
  if (image->control.state != PNF_JOURNAL_COMMITTED)
    return check_error(image, "unknown selected journal state");
  if (image->control.sequence == UINT64_MAX)
    return PNF_UNSUPPORTED;
  size_t count = image->control.image_count;
  size_t descriptor_blocks = image->control.descriptor_blocks;
  uint64_t expected_descriptors = ((uint64_t)count + PNF_DESCRIPTORS_PER_BLOCK - 1) /
                                  PNF_DESCRIPTORS_PER_BLOCK;
  if (!count || descriptor_blocks != expected_descriptors ||
      (uint64_t)count > pnf_journal_capacity(image->header.journal_blocks) ||
      2 + expected_descriptors + (uint64_t)count > image->header.journal_blocks)
    return check_error(image, "invalid committed journal counts");
  if (count > SIZE_MAX / PNF_BLOCK_SIZE ||
      count > SIZE_MAX / sizeof(struct pnf_descriptor))
    return PNF_NO_MEMORY;
  struct pnf_descriptor *descriptors = calloc(count, sizeof(*descriptors));
  uint8_t *images = malloc(count * PNF_BLOCK_SIZE);
  uint64_t owned_bytes = image->header.pool_blocks / 8 + (image->header.pool_blocks % 8 != 0);
  uint8_t *targets = owned_bytes <= SIZE_MAX ? calloc((size_t)owned_bytes, 1) : NULL;
  enum pnf_status status = PNF_NO_MEMORY;
  if (!descriptors || !images || !targets)
    goto done;
  uint32_t crc = pnf_payload_begin(&image->control);
  uint8_t block[PNF_BLOCK_SIZE];
  for (size_t i = 0; i < descriptor_blocks; i++) {
    status = native_read_blocks(image, image->header.journal_start + 2 + i, 1, block);
    if (status != PNF_OK)
      goto done;
    crc = pnf_crc_update(crc, block, sizeof(block));
    for (unsigned j = 0; j < PNF_DESCRIPTORS_PER_BLOCK; j++) {
      size_t index = i * PNF_DESCRIPTORS_PER_BLOCK + j;
      if (index >= count) {
        for (unsigned k = 0; k < PNF_DESCRIPTOR_SIZE; k++) {
          if (block[j * PNF_DESCRIPTOR_SIZE + k]) {
            status = check_error(image, "nonzero unused journal descriptor");
            goto done;
          }
        }
        continue;
      }
      status = pnf_descriptor_decode(&image->header,
                                      block + j * PNF_DESCRIPTOR_SIZE, &descriptors[index]);
      if (status != PNF_OK)
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
    uint8_t *bytes = images + i * PNF_BLOCK_SIZE;
    status = native_read_blocks(image, image->header.journal_start + 2 + descriptor_blocks + i,
                                 1, bytes);
    if (status != PNF_OK)
      goto done;
    crc = pnf_crc_update(crc, bytes, PNF_BLOCK_SIZE);
    status = check_log_image(image, &descriptors[i], bytes);
    if (status != PNF_OK)
      goto done;
  }
  if (pnf_crc_finish(crc) != image->control.payload_crc) {
    status = check_error(image, "committed journal payload checksum mismatch");
    goto done;
  }
  for (size_t i = 0; i < count; i++) {
    status = native_write_blocks(image, descriptors[i].home, 1, images + i * PNF_BLOCK_SIZE);
    if (status != PNF_OK)
      goto done;
  }
  status = native_flush(image);
  if (status != PNF_OK)
    goto done;
  struct pnf_control empty = image->control;
  empty.sequence++;
  empty.state = PNF_JOURNAL_EMPTY;
  empty.image_count = 0;
  empty.descriptor_blocks = 0;
  empty.payload_crc = 0;
  status = pnf_control_encode(&image->header, &empty, block);
  if (status != PNF_OK)
    goto done;
  unsigned slot = image->control_slot ^ 1;
  status = native_write_blocks(image, image->header.journal_start + slot, 1, block);
  if (status != PNF_OK)
    goto done;
  status = native_flush(image);
  if (status != PNF_OK)
    goto done;
  image->control = empty;
  image->control_slot = slot;
done:
  free(targets);
  free(images);
  free(descriptors);
  return status;
}
