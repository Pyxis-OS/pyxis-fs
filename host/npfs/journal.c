/* SPDX-License-Identifier: MPL-2.0 */
#include "journal.h"

#include <stdlib.h>
#include <string.h>

static enum npfs_status
journal_error(struct npfs_image *image, const char *message)
{
  snprintf(image->error, sizeof(image->error), "%s", message);
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

void
npfs_journal_free(struct npfs_journal *journal)
{
  free(journal->images);
  free(journal->descriptors);
  *journal = (struct npfs_journal){0};
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
      if ((number >= image->header.pool_blocks || !npfs_data_block_valid(&image->header, number)) &&
          !bit_get(bytes, bit))
        return journal_error(image, "journal bitmap clears permanent allocation bit");
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
npfs_journal_load(struct npfs_image *image, struct npfs_journal *journal)
{
  if (!image || !journal || image->overlay)
    return NPFS_INVALID;
  *journal = (struct npfs_journal){0};
  if (image->control.state == NPFS_JOURNAL_EMPTY)
    return NPFS_OK;
  if (image->control.state != NPFS_JOURNAL_COMMITTED)
    return journal_error(image, "unknown selected journal state");
  size_t count = image->control.image_count;
  size_t descriptor_blocks = image->control.descriptor_blocks;
  uint64_t expected_descriptors = ((uint64_t)count + NPFS_DESCRIPTORS_PER_BLOCK - 1) /
                                  NPFS_DESCRIPTORS_PER_BLOCK;
  if (!count || descriptor_blocks != expected_descriptors ||
      (uint64_t)count > npfs_journal_capacity(image->header.journal_blocks) ||
      2 + expected_descriptors + (uint64_t)count > image->header.journal_blocks)
    return journal_error(image, "invalid committed journal counts");
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
            status = journal_error(image, "nonzero unused journal descriptor");
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
        status = journal_error(image, "journal target outside pool");
        goto done;
      }
      if (bit_get(targets, home)) {
        status = journal_error(image, "duplicate journal home target");
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
    status = journal_error(image, "committed journal payload checksum mismatch");
    goto done;
  }
  free(targets);
  *journal = (struct npfs_journal){.count = count, .descriptors = descriptors, .images = images};
  return NPFS_OK;
done:
  free(targets);
  free(images);
  free(descriptors);
  return status;
}

enum npfs_status
npfs_replay(struct npfs_image *image)
{
  if (!image || !image->writable || image->overlay)
    return NPFS_INVALID;
  if (image->control.state == NPFS_JOURNAL_EMPTY)
    return NPFS_OK;
  if (image->control.sequence == UINT64_MAX)
    return NPFS_UNSUPPORTED;
  struct npfs_journal journal;
  enum npfs_status status = npfs_journal_load(image, &journal);
  if (status != NPFS_OK)
    return status;
  for (size_t i = 0; i < journal.count; ++i) {
    status = npfs_write_blocks(image, journal.descriptors[i].home, 1,
                              journal.images + i * NPFS_BLOCK_SIZE);
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
  uint8_t block[NPFS_BLOCK_SIZE];
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
  npfs_journal_free(&journal);
  return status;
}

struct overlay_entry {
  uint64_t home;
  size_t index;
};

struct npfs_overlay {
  struct npfs_journal journal;
  struct overlay_entry *entries;
};

static int
compare_entries(const void *left, const void *right)
{
  const struct overlay_entry *a = left, *b = right;
  return (a->home > b->home) - (a->home < b->home);
}

const uint8_t *
npfs_overlay_block(const struct npfs_overlay *overlay, uint64_t home)
{
  size_t low = 0, high = overlay->journal.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const struct overlay_entry *entry = &overlay->entries[middle];
    if (home == entry->home)
      return overlay->journal.images + entry->index * NPFS_BLOCK_SIZE;
    if (home < entry->home)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}

void
npfs_overlay_free(struct npfs_overlay *overlay)
{
  if (!overlay)
    return;
  npfs_journal_free(&overlay->journal);
  free(overlay->entries);
  free(overlay);
}

enum npfs_status
npfs_memory_replay(struct npfs_image *image)
{
  if (!image || image->writable || image->overlay)
    return NPFS_INVALID;
  if (image->control.state == NPFS_JOURNAL_EMPTY)
    return NPFS_OK;
  struct npfs_overlay *overlay = calloc(1, sizeof(*overlay));
  if (!overlay)
    return NPFS_NO_MEMORY;
  enum npfs_status status = npfs_journal_load(image, &overlay->journal);
  if (status != NPFS_OK)
    goto fail;
  if (overlay->journal.count > SIZE_MAX / sizeof(*overlay->entries)) {
    status = NPFS_NO_MEMORY;
    goto fail;
  }
  overlay->entries = malloc(overlay->journal.count * sizeof(*overlay->entries));
  if (!overlay->entries) {
    status = NPFS_NO_MEMORY;
    goto fail;
  }
  for (size_t i = 0; i < overlay->journal.count; ++i) {
    overlay->entries[i] = (struct overlay_entry){.home = overlay->journal.descriptors[i].home, .index = i};
  }
  qsort(overlay->entries, overlay->journal.count, sizeof(*overlay->entries), compare_entries);
  /* Publish only after complete validation/indexing. No caller can observe a
   * partially staged view, and this source remains O_RDONLY. */
  image->overlay = overlay;
  image->control.state = NPFS_JOURNAL_EMPTY;
  image->control.image_count = 0;
  image->control.descriptor_blocks = 0;
  image->control.payload_crc = 0;
  return NPFS_OK;
fail:
  npfs_overlay_free(overlay);
  return status;
}
