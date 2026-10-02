/* SPDX-License-Identifier: MPL-2.0 */
#include "edit_tests.h"
#include "edit.h"
#include "internal.h"
#include "support.h"
#include "unity.h"
#include <pyxis_fs/record.h>

#include <stdio.h>
#include <string.h>

#define EDIT_TEST_ITEMS 240u
#define EDIT_TEST_SLOTS 256u
#define EXTENT_SPLIT_ITEMS 57u
#define SPARSE_DEPTH_LEAVES 128u
#define GROWTH_LEAVES 13u
#define GROWTH_ITEMS_PER_LEAF 6u
#define ORPHAN_TEST_ITEMS 120u

static struct test_fixture fixture;
static struct pfs_edit_slot slots[EDIT_TEST_SLOTS];
static struct pfs_edit_slot saved_slots[EDIT_TEST_SLOTS];
static struct pfs_reference retired[EDIT_TEST_SLOTS];
static struct pfs_edit_workspace workspace;
static struct pfs_edit_candidate candidate;
static uint8_t walk_data[PFS_TREE_DEPTH_MAX][PFS_BLOCK_SIZE];
static struct pfs_tree walk_tree[PFS_TREE_DEPTH_MAX];
static bool present[EDIT_TEST_ITEMS];
static size_t expected_position;
static size_t reachable_private;

static void
start_case(uint16_t kind)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  memset(slots, 0, sizeof(slots));
  memset(retired, 0, sizeof(retired));
  memset(present, 0, sizeof(present));
  candidate = (struct pfs_edit_candidate) {
    .reader = &fixture.builder.reader,
    .context = {
      .block = { .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}} },
      .kind = kind,
      .volume = {{2}},
      .object = {{3}},
    },
    .birth = 1,
    .slots = slots,
    .slot_capacity = EDIT_TEST_SLOTS,
    .retired = retired,
    .retired_capacity = EDIT_TEST_SLOTS,
  };
  for (size_t i = 0; i < EDIT_TEST_SLOTS; ++i) {
    slots[i].block = 100 + i;
  }
}

static struct pfs_record_context
context_for(uint64_t birth)
{
  return (struct pfs_record_context) {
    .block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = candidate.birth,
    .containing_birth = birth,
    .features = candidate.context.block.features,
  };
}

static struct pfs_key
name_key(unsigned item)
{
  struct pfs_key key = {0};
  key.length = item % 3 ? PFS_NAME_MAX : 4;
  char prefix[11];
  snprintf(prefix, sizeof(prefix), "%04u", item);
  memcpy(key.bytes, prefix, 4);
  memset(key.bytes + 4, 'x', key.length - 4);
  return key;
}

static struct pfs_encoded_record
name_record(unsigned item, const struct pfs_key *key, uint8_t *data)
{
  struct pfs_dirent_record value = {
    .name = { .length = key->length },
    .child_kind = PFS_OBJECT_FILE,
  };
  memcpy(value.name.bytes, key->bytes, key->length);
  value.object.bytes[0] = (uint8_t)(item + 1);
  value.object.bytes[1] = (uint8_t)((item + 1) >> 8);
  struct pfs_record_context context = context_for(candidate.birth);
  struct pfs_encoded_record encoded = { .data = data };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_encode(data, 304, &context, &value, &encoded.length));
  return encoded;
}

static void
get_node(struct pfs_reference reference, size_t depth)
{
  TEST_ASSERT_LESS_THAN(PFS_TREE_DEPTH_MAX, depth);
  bool found = false;
  if (reference.birth == candidate.birth) {
    for (size_t i = 0; i < candidate.slot_capacity; ++i) {
      if (slots[i].active && slots[i].block == reference.block) {
        memcpy(walk_data[depth], slots[i].data, PFS_BLOCK_SIZE);
        ++reachable_private;
        found = true;
        break;
      }
    }
    TEST_ASSERT_TRUE(found);
  } else {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(candidate.reader, reference.block, 1,
                                           walk_data[depth], PFS_BLOCK_SIZE));
  }
  struct pfs_tree_context context = candidate.context;
  context.block.reference = reference;
  context.block.selected_generation = candidate.birth;
  context.block.referring_birth = candidate.birth;
  context.parent_level = depth ? walk_tree[depth - 1].level : 0;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(walk_data[depth], PFS_BLOCK_SIZE,
                                          &context, &walk_tree[depth]));
}

static void
walk_names(struct pfs_reference reference, size_t depth)
{
  get_node(reference, depth);
  struct pfs_tree *tree = &walk_tree[depth];
  if (depth) {
    TEST_ASSERT_GREATER_OR_EQUAL(6, tree->count);
  }
  struct pfs_record_context context = context_for(tree->header.birth);
  for (uint16_t i = 0; i < tree->count; ++i) {
    const uint8_t *data = walk_data[depth] + tree->slots[i].offset;
    if (tree->level) {
      struct pfs_internal_record child;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(data, tree->slots[i].length,
                            PFS_INDEX_DIRECTORY, &context, &child));
      walk_names(child.child, depth + 1);
      TEST_ASSERT_EQUAL(0, pfs_key_compare(PFS_INDEX_DIRECTORY, &child.minimum,
                                          &walk_tree[depth + 1].minimum));
    } else {
      while (expected_position < EDIT_TEST_ITEMS && !present[expected_position]) {
        ++expected_position;
      }
      TEST_ASSERT_LESS_THAN(EDIT_TEST_ITEMS, expected_position);
      struct pfs_dirent_record value;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_decode(data, tree->slots[i].length, &context, &value));
      struct pfs_key expected = name_key((unsigned)expected_position);
      TEST_ASSERT_EQUAL(expected.length, value.name.length);
      TEST_ASSERT_EQUAL_MEMORY(expected.bytes, value.name.bytes, expected.length);
      TEST_ASSERT_EQUAL(expected_position + 1, (unsigned)value.object.bytes[0] |
                                             ((unsigned)value.object.bytes[1] << 8));
      ++expected_position;
    }
  }
}

static void
check_names(void)
{
  expected_position = 0;
  reachable_private = 0;
  if (candidate.root.block) {
    walk_names(candidate.root, 0);
  }
  while (expected_position < EDIT_TEST_ITEMS && !present[expected_position]) {
    ++expected_position;
  }
  TEST_ASSERT_EQUAL(EDIT_TEST_ITEMS, expected_position);
  size_t active = 0;
  for (size_t i = 0; i < candidate.slot_capacity; ++i) {
    active += slots[i].active;
  }
  TEST_ASSERT_EQUAL(active, reachable_private);
}

static void
publish_fixture(void)
{
  memcpy(saved_slots, slots, sizeof(slots));
  for (size_t i = 0; i < EDIT_TEST_SLOTS; ++i) {
    if (slots[i].active) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, slots[i].block, 1,
                                               slots[i].data, PFS_BLOCK_SIZE));
    }
    slots[i].active = false;
    slots[i].block += 1024;
  }
  ++candidate.birth;
  candidate.retired_count = 0;
}

static void
check_published_unchanged(uint64_t writes)
{
  uint8_t block[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  for (size_t i = 0; i < EDIT_TEST_SLOTS; ++i) {
    if (saved_slots[i].active) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(candidate.reader, saved_slots[i].block,
                                             1, block, PFS_BLOCK_SIZE));
      TEST_ASSERT_EQUAL_MEMORY(saved_slots[i].data, block, PFS_BLOCK_SIZE);
    }
  }
}

static void
namespace_edits_preserve_order_profile_and_published_blocks(void)
{
  start_case(PFS_INDEX_DIRECTORY);
  uint8_t data[304];
  for (unsigned i = 0; i < EDIT_TEST_ITEMS; ++i) {
    unsigned item = (i * 137) % EDIT_TEST_ITEMS;
    struct pfs_key key = name_key(item);
    struct pfs_encoded_record record = name_record(item, &key, data);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
    present[item] = true;
    TEST_ASSERT_LESS_OR_EQUAL(15, workspace.output_count);
    TEST_ASSERT_LESS_OR_EQUAL(8, workspace.consumed_count);
    TEST_ASSERT_EQUAL(0, candidate.retired_count);
    check_names();
  }
  publish_fixture();
  uint64_t writes = fixture.writes;
  for (unsigned i = 0; i < EDIT_TEST_ITEMS; ++i) {
    unsigned item = (i * 137) % EDIT_TEST_ITEMS;
    struct pfs_key key = name_key(item);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &key, NULL));
    present[item] = false;
    TEST_ASSERT_LESS_OR_EQUAL(15, workspace.output_count);
    TEST_ASSERT_LESS_OR_EQUAL(15, workspace.consumed_count);
    check_names();
  }
  TEST_ASSERT_EQUAL_UINT64(0, candidate.root.block);
  size_t published_count = 0;
  for (size_t i = 0; i < EDIT_TEST_SLOTS; ++i) {
    published_count += saved_slots[i].active;
  }
  TEST_ASSERT_EQUAL(published_count, candidate.retired_count);
  check_published_unchanged(writes);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
repeated_updates_reuse_private_buffers_and_fail_atomically(void)
{
  start_case(PFS_INDEX_DIRECTORY);
  candidate.slot_capacity = 1;
  uint8_t data[304];
  struct pfs_key key = name_key(0);
  struct pfs_encoded_record record = name_record(0, &key, data);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  for (unsigned i = 0; i < 100; ++i) {
    record = name_record(i, &key, data);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_UPDATE, &key, &record));
    TEST_ASSERT_EQUAL(0, candidate.retired_count);
    TEST_ASSERT_TRUE(slots[0].active);
  }
  for (unsigned i = 1; i < 13; ++i) {
    struct pfs_key added = name_key(i);
    added.length = PFS_NAME_MAX;
    memset(added.bytes + 4, 'x', PFS_NAME_MAX - 4);
    record = name_record(i, &added, data);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &added, &record));
  }
  struct pfs_key overflow = name_key(13);
  record = name_record(13, &overflow, data);
  struct pfs_edit_slot private_before = slots[0];
  struct pfs_reference private_root = candidate.root;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &overflow, &record));
  TEST_ASSERT_EQUAL_MEMORY(&private_before, &slots[0], sizeof(private_before));
  TEST_ASSERT_EQUAL_MEMORY(&private_root, &candidate.root, sizeof(private_root));
  TEST_ASSERT_EQUAL(0, candidate.retired_count);
  record = name_record(99, &key, data);
  publish_fixture();
  uint64_t writes = fixture.writes;
  struct pfs_reference root = candidate.root;
  candidate.retired_capacity = 0;
  struct pfs_edit_slot before = slots[0];
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_UPDATE, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(&root, &candidate.root, sizeof(root));
  TEST_ASSERT_EQUAL_MEMORY(&before, &slots[0], sizeof(before));
  TEST_ASSERT_EQUAL(0, candidate.retired_count);
  check_published_unchanged(writes);
  candidate.retired_capacity = EDIT_TEST_SLOTS;
  candidate.slot_capacity = 0;
  TEST_ASSERT_EQUAL(PFS_LIMIT, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_UPDATE, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(&root, &candidate.root, sizeof(root));
  TEST_ASSERT_EQUAL_MEMORY(&before, &slots[0], sizeof(before));
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static struct pfs_reference
write_tree(uint64_t block, uint16_t level, struct pfs_encoded_record *records, uint16_t count)
{
  struct pfs_tree_context context = candidate.context;
  context.block.selected_generation = 1;
  context.block.referring_birth = 1;
  context.block.reference = (struct pfs_reference) { block, 1, PFS_BLOCK_TREE, PFS_FORMAT_VERSION };
  struct pfs_tree tree = {
    .header = { .type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION,
                .pool = context.block.pool, .block = block, .birth = 1 },
    .kind = context.kind,
    .level = level,
    .count = count,
    .volume = context.volume,
    .object = context.object,
  };
  uint8_t data[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(data, sizeof(data), &context, &tree, records));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, block, 1, data, sizeof(data)));
  return context.block.reference;
}

static struct pfs_key
extent_key(uint64_t first)
{
  struct pfs_key key = { .length = 8 };
  test_put_u64(key.bytes, first);
  return key;
}

static struct pfs_encoded_record
internal_record(uint8_t *data, struct pfs_reference reference, struct pfs_key minimum)
{
  struct pfs_internal_record value = { .child = reference, .minimum = minimum };
  struct pfs_record_context context = context_for(1);
  struct pfs_encoded_record encoded = { .data = data };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_encode(data, 304, candidate.context.kind,
                                                     &context, &value, &encoded.length));
  return encoded;
}

static void
walk_extent_records(struct pfs_reference reference, size_t depth,
                    const struct pfs_extent_record *expected, size_t count, size_t *position)
{
  get_node(reference, depth);
  struct pfs_tree *tree = &walk_tree[depth];
  struct pfs_record_context context = context_for(tree->header.birth);
  for (uint16_t i = 0; i < tree->count; ++i) {
    const uint8_t *data = walk_data[depth] + tree->slots[i].offset;
    if (tree->level) {
      struct pfs_internal_record child;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(data, tree->slots[i].length,
        PFS_INDEX_EXTENTS, &context, &child));
      walk_extent_records(child.child, depth + 1, expected, count, position);
      TEST_ASSERT_EQUAL(0, pfs_key_compare(PFS_INDEX_EXTENTS, &child.minimum,
        &walk_tree[depth + 1].minimum));
    } else {
      TEST_ASSERT_LESS_THAN(count, *position);
      struct pfs_extent_record actual;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_decode(data, tree->slots[i].length,
        &context, &actual));
      TEST_ASSERT_EQUAL_UINT64(expected[*position].mapping.logical_first, actual.mapping.logical_first);
      TEST_ASSERT_EQUAL_UINT64(expected[*position].mapping.count, actual.mapping.count);
      TEST_ASSERT_EQUAL_UINT64(expected[*position].mapping.physical_first, actual.mapping.physical_first);
      TEST_ASSERT_EQUAL_UINT64(expected[*position].mapping.birth, actual.mapping.birth);
      ++*position;
    }
  }
}

static void
check_extent_records(const struct pfs_extent_record *expected, size_t count)
{
  size_t position = 0;
  reachable_private = 0;
  walk_extent_records(candidate.root, 0, expected, count, &position);
  TEST_ASSERT_EQUAL(count, position);
  size_t active = 0;
  for (size_t i = 0; i < candidate.slot_capacity; ++i) {
    active += slots[i].active;
  }
  TEST_ASSERT_EQUAL(active, reachable_private);
}

static void
sparse_repair(unsigned sibling_children)
{
  start_case(PFS_INDEX_EXTENTS);
  candidate.birth = 2;
  uint8_t parent_data[5][304];
  struct pfs_encoded_record parent_records[5];
  unsigned total = 2 + sibling_children;
  struct pfs_extent_record expected[5];
  for (unsigned i = 0; i < total; ++i) {
    struct pfs_extent_record extent = {
      .mapping = { .logical_first = 2 * i, .count = 1, .physical_first = 9000 + i, .birth = 1 },
    };
    struct pfs_record_context context = context_for(1);
    uint8_t data[64];
    struct pfs_encoded_record record = { .data = data };
    TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(data, sizeof(data), &context, &extent, &record.length));
    expected[i] = extent;
    struct pfs_reference leaf = write_tree(600 + i, 0, &record, 1);
    parent_records[i] = internal_record(parent_data[i], leaf, extent_key(2 * i));
  }
  struct pfs_reference left = write_tree(700, 1, parent_records, 2);
  struct pfs_reference right = write_tree(701, 1, parent_records + 2, sibling_children);
  uint8_t root_data[2][304];
  struct pfs_encoded_record root_records[2] = {
    internal_record(root_data[0], left, extent_key(0)),
    internal_record(root_data[1], right, extent_key(4)),
  };
  candidate.root = write_tree(800, 2, root_records, 2);
  uint64_t writes = fixture.writes;
  struct pfs_key key = extent_key(0);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &key, NULL));
  TEST_ASSERT_LESS_OR_EQUAL(14, workspace.output_count);
  TEST_ASSERT_LESS_OR_EQUAL(14, workspace.consumed_count);
  check_extent_records(expected + 1, total - 1);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_LESS_OR_EQUAL(14, candidate.retired_count);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
extent_cross_leaf_overlap_is_refused(void)
{
  start_case(PFS_INDEX_EXTENTS);
  candidate.birth = 2;
  uint8_t leaf_data[4][64] = {{0}};
  struct pfs_encoded_record leaf_records[4];
  struct pfs_reference leaves[4];
  /* Independent canonical extents at logical blocks zero, four, ten and fourteen. */
  const uint64_t logical[] = {0, 4, 10, 14};
  struct pfs_extent_record expected[5];
  for (unsigned i = 0; i < 4; ++i) {
    test_put_u16(leaf_data[i], PFS_RECORD_EXTENT);
    test_put_u16(leaf_data[i] + 2, 1);
    test_put_u32(leaf_data[i] + 4, 64);
    test_put_u64(leaf_data[i] + 16, logical[i]);
    test_put_u64(leaf_data[i] + 24, 1);
    test_put_u64(leaf_data[i] + 32, 9000 + i);
    test_put_u64(leaf_data[i] + 40, 1);
    test_checksum(leaf_data[i], 64, 8);
    expected[i] = (struct pfs_extent_record) {
      .mapping = {.logical_first = logical[i], .count = 1, .physical_first = 9000 + i, .birth = 1},
    };
    leaf_records[i] = (struct pfs_encoded_record){leaf_data[i], 64};
    leaves[i] = write_tree(600 + i, 0, &leaf_records[i], 1);
  }
  uint8_t parent_data[4][304];
  struct pfs_encoded_record parent_records[4];
  for (unsigned i = 0; i < 4; ++i) {
    parent_records[i] = internal_record(parent_data[i], leaves[i], extent_key(logical[i]));
  }
  struct pfs_reference left = write_tree(700, 1, parent_records, 2);
  struct pfs_reference right = write_tree(701, 1, parent_records + 2, 2);
  uint8_t root_data[2][304];
  struct pfs_encoded_record root_records[2] = {
    internal_record(root_data[0], left, extent_key(0)),
    internal_record(root_data[1], right, extent_key(10)),
  };
  candidate.root = write_tree(800, 2, root_records, 2);
  memcpy(saved_slots, slots, sizeof(slots));
  struct pfs_reference original_root = candidate.root;
  uint64_t writes = fixture.writes;
  uint8_t replacement[64];
  memcpy(replacement, leaf_data[0], sizeof(replacement));
  test_put_u64(replacement + 16, 2);
  test_put_u64(replacement + 24, 3);
  test_checksum(replacement, sizeof(replacement), 8);
  struct pfs_encoded_record record = {replacement, sizeof(replacement)};
  struct pfs_key key = extent_key(2);
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(saved_slots, slots, sizeof(slots));
  TEST_ASSERT_EQUAL_MEMORY(&original_root, &candidate.root, sizeof(original_root));
  TEST_ASSERT_EQUAL(0, candidate.retired_count);
  /* The last leaf under the left parent inherits the root's upper separator. */
  test_put_u64(replacement + 16, 6);
  test_put_u64(replacement + 24, 5);
  test_checksum(replacement, sizeof(replacement), 8);
  key = extent_key(6);
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(saved_slots, slots, sizeof(slots));
  TEST_ASSERT_EQUAL_MEMORY(&original_root, &candidate.root, sizeof(original_root));
  TEST_ASSERT_EQUAL(0, candidate.retired_count);
  test_put_u64(replacement + 16, 4);
  test_put_u64(replacement + 24, 7);
  test_checksum(replacement, sizeof(replacement), 8);
  key = extent_key(4);
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_UPDATE, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(saved_slots, slots, sizeof(slots));
  TEST_ASSERT_EQUAL_MEMORY(&original_root, &candidate.root, sizeof(original_root));
  TEST_ASSERT_EQUAL(0, candidate.retired_count);
  /* Ending at the next leaf's minimum is valid, with no media write. */
  test_put_u64(replacement + 16, 6);
  test_put_u64(replacement + 24, 4);
  test_checksum(replacement, sizeof(replacement), 8);
  key = extent_key(6);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  memmove(expected + 3, expected + 2, 2 * sizeof(*expected));
  expected[2] = (struct pfs_extent_record) {
    .mapping = {.logical_first = 6, .count = 4, .physical_first = 9000, .birth = 1},
  };
  check_extent_records(expected, sizeof(expected) / sizeof(*expected));
  TEST_ASSERT_LESS_OR_EQUAL(15, workspace.output_count);
  TEST_ASSERT_LESS_OR_EQUAL(8, workspace.consumed_count);
  TEST_ASSERT_LESS_OR_EQUAL(8, candidate.retired_count);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
extent_new_split_overlap_is_refused(void)
{
  start_case(PFS_INDEX_EXTENTS);
  candidate.birth = 2;
  uint8_t data[EXTENT_SPLIT_ITEMS][64];
  struct pfs_encoded_record records[EXTENT_SPLIT_ITEMS];
  struct pfs_extent_record expected[EXTENT_SPLIT_ITEMS + 1];
  struct pfs_record_context context = context_for(1);
  for (unsigned i = 0; i < EXTENT_SPLIT_ITEMS; ++i) {
    struct pfs_extent_record extent = {
      .mapping = {.logical_first = 2 * i, .count = 1, .physical_first = 9000 + i, .birth = 1},
    };
    expected[i] = extent;
    records[i].data = data[i];
    TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(data[i], sizeof(data[i]),
      &context, &extent, &records[i].length));
  }
  candidate.root = write_tree(600, 0, records, EXTENT_SPLIT_ITEMS);
  struct pfs_edit_candidate original;
  memcpy(&original, &candidate, sizeof(original));
  memcpy(saved_slots, slots, sizeof(slots));
  static struct pfs_reference saved_retired[EDIT_TEST_SLOTS];
  memcpy(saved_retired, retired, sizeof(retired));
  uint64_t writes = fixture.writes;
  uint8_t replacement[64];
  struct pfs_extent_record extent = {
    .mapping = {.logical_first = 111, .count = 2, .physical_first = 9100, .birth = 2},
  };
  context = context_for(candidate.birth);
  struct pfs_encoded_record record = {.data = replacement};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(replacement, sizeof(replacement),
    &context, &extent, &record.length));
  struct pfs_key key = extent_key(111);
  /* The extra record forces a split; [111,113) overlaps [112,113). */
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  TEST_ASSERT_EQUAL_MEMORY(&original, &candidate, sizeof(candidate));
  TEST_ASSERT_EQUAL_MEMORY(saved_slots, slots, sizeof(slots));
  TEST_ASSERT_EQUAL_MEMORY(saved_retired, retired, sizeof(retired));
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);

  /* [111,112) touches both neighbors and remains valid across the split. */
  extent.mapping.count = 1;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(replacement, sizeof(replacement),
    &context, &extent, &record.length));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
  expected[EXTENT_SPLIT_ITEMS] = expected[EXTENT_SPLIT_ITEMS - 1];
  expected[EXTENT_SPLIT_ITEMS - 1] = extent;
  check_extent_records(expected, EXTENT_SPLIT_ITEMS + 1);
  TEST_ASSERT_LESS_OR_EQUAL(15, workspace.output_count);
  TEST_ASSERT_LESS_OR_EQUAL(8, workspace.consumed_count);
  /* The sole published node is the original leaf; every replacement is private. */
  TEST_ASSERT_EQUAL(1, candidate.retired_count);
  TEST_ASSERT_EQUAL_MEMORY(&original.root, &retired[0], sizeof(retired[0]));
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
sparse_two_child_sibling_repair_preserves_contents(void)
{
  sparse_repair(2);
}

static void
sparse_three_child_sibling_repair_preserves_contents(void)
{
  sparse_repair(3);
}

static void
minimum_and_maximum_component_lengths(void)
{
  start_case(PFS_INDEX_DIRECTORY);
  uint8_t data[304];
  struct pfs_key low = { .length = 1, .bytes = {'a'} };
  struct pfs_key high = { .length = PFS_NAME_MAX };
  memset(high.bytes, 'z', sizeof(high.bytes));
  struct pfs_encoded_record record = name_record(1, &high, data);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &high, &record));
  record = name_record(0, &low, data);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &low, &record));
  get_node(candidate.root, 0);
  TEST_ASSERT_EQUAL(1, walk_tree[0].minimum.length);
  TEST_ASSERT_EQUAL('a', walk_tree[0].minimum.bytes[0]);
  TEST_ASSERT_EQUAL(PFS_NAME_MAX, walk_tree[0].maximum.length);
  TEST_ASSERT_EQUAL_MEMORY(high.bytes, walk_tree[0].maximum.bytes, PFS_NAME_MAX);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &low, NULL));
  get_node(candidate.root, 0);
  TEST_ASSERT_EQUAL(PFS_NAME_MAX, walk_tree[0].minimum.length);
  TEST_ASSERT_EQUAL_MEMORY(high.bytes, walk_tree[0].minimum.bytes, PFS_NAME_MAX);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &high, NULL));
  TEST_ASSERT_EQUAL(0, candidate.root.block);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
maximum_depth_sparse_delete_fits_retirement_bound(void)
{
  start_case(PFS_INDEX_EXTENTS);
  candidate.birth = 2;
  struct pfs_reference children[SPARSE_DEPTH_LEAVES];
  struct pfs_key minima[SPARSE_DEPTH_LEAVES];
  struct pfs_extent_record expected[SPARSE_DEPTH_LEAVES];
  uint64_t block = 1000;
  struct pfs_record_context context = context_for(1);
  for (unsigned i = 0; i < SPARSE_DEPTH_LEAVES; ++i) {
    struct pfs_extent_record extent = {
      .mapping = { .logical_first = 2 * i, .count = 1, .physical_first = 9000 + i, .birth = 1 },
    };
    uint8_t data[64];
    struct pfs_encoded_record record = { .data = data };
    TEST_ASSERT_EQUAL(PFS_OK, pfs_extent_record_encode(data, sizeof(data), &context, &extent, &record.length));
    expected[i] = extent;
    children[i] = write_tree(block++, 0, &record, 1);
    minima[i] = extent_key(2 * i);
  }
  for (uint16_t level = 1, count = SPARSE_DEPTH_LEAVES; count > 1; ++level, count /= 2) {
    for (uint16_t i = 0; i < count / 2; ++i) {
      uint8_t data[2][304];
      struct pfs_encoded_record records[2] = {
        internal_record(data[0], children[2 * i], minima[2 * i]),
        internal_record(data[1], children[2 * i + 1], minima[2 * i + 1]),
      };
      children[i] = write_tree(block++, level, records, 2);
      minima[i] = minima[2 * i];
    }
  }
  candidate.root = children[0];
  get_node(candidate.root, 0);
  TEST_ASSERT_EQUAL(PFS_TREE_DEPTH_MAX - 1, walk_tree[0].level);
  struct pfs_key key = extent_key(0);
  uint64_t writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &key, NULL));
  /* This depth-eight fixture must reach the sparse-delete worst case, not just fit its bound. */
  TEST_ASSERT_EQUAL(14, candidate.retired_count);
  TEST_ASSERT_LESS_OR_EQUAL(14, workspace.output_count);
  TEST_ASSERT_LESS_OR_EQUAL(14, workspace.consumed_count);
  check_extent_records(expected + 1, SPARSE_DEPTH_LEAVES - 1);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static struct pfs_key
growth_key(unsigned item)
{
  struct pfs_key key = { .length = PFS_NAME_MAX };
  char prefix[11];
  snprintf(prefix, sizeof(prefix), "%04u", item / GROWTH_ITEMS_PER_LEAF);
  memcpy(key.bytes, prefix, 4);
  memset(key.bytes + 4, 'x', PFS_NAME_MAX - 4);
  key.bytes[PFS_NAME_MAX - 1] = (uint8_t)('a' + item % GROWTH_ITEMS_PER_LEAF);
  return key;
}

static void
walk_growth(struct pfs_reference reference, size_t depth)
{
  get_node(reference, depth);
  struct pfs_tree *tree = &walk_tree[depth];
  if (depth) {
    TEST_ASSERT_GREATER_OR_EQUAL(6, tree->count);
  }
  struct pfs_record_context context = context_for(tree->header.birth);
  for (uint16_t i = 0; i < tree->count; ++i) {
    const uint8_t *data = walk_data[depth] + tree->slots[i].offset;
    if (tree->level) {
      struct pfs_internal_record child;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(data, tree->slots[i].length,
                            PFS_INDEX_DIRECTORY, &context, &child));
      walk_growth(child.child, depth + 1);
      TEST_ASSERT_EQUAL(0, pfs_key_compare(PFS_INDEX_DIRECTORY, &child.minimum,
                                          &walk_tree[depth + 1].minimum));
    } else {
      struct pfs_dirent_record value;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_decode(data, tree->slots[i].length, &context, &value));
      struct pfs_key expected = growth_key((unsigned)expected_position);
      TEST_ASSERT_EQUAL(expected.length, value.name.length);
      TEST_ASSERT_EQUAL_MEMORY(expected.bytes, value.name.bytes, expected.length);
      TEST_ASSERT_EQUAL(expected_position + 1, value.object.bytes[0]);
      ++expected_position;
    }
  }
}

static void
namespace_minimum_growth_can_split_root_on_delete(void)
{
  start_case(PFS_INDEX_DIRECTORY);
  candidate.birth = 2;
  uint8_t root_data[GROWTH_LEAVES][304];
  struct pfs_encoded_record root_records[GROWTH_LEAVES];
  struct pfs_key short_key = { .length = 4, .bytes = {'0', '0', '0', '0'} };
  for (unsigned i = 0; i < GROWTH_LEAVES; ++i) {
    uint8_t data[GROWTH_ITEMS_PER_LEAF + 1][304];
    struct pfs_encoded_record records[GROWTH_ITEMS_PER_LEAF + 1];
    unsigned count = 0;
    if (!i) {
      records[count++] = name_record(200, &short_key, data[0]);
    }
    for (unsigned j = 0; j < GROWTH_ITEMS_PER_LEAF; ++j) {
      struct pfs_key key = growth_key(i * GROWTH_ITEMS_PER_LEAF + j);
      records[count] = name_record(i * GROWTH_ITEMS_PER_LEAF + j, &key, data[count]);
      ++count;
    }
    struct pfs_reference leaf = write_tree(600 + i, 0, records, count);
    root_records[i] = internal_record(root_data[i], leaf, i ? growth_key(i * GROWTH_ITEMS_PER_LEAF) : short_key);
  }
  candidate.root = write_tree(700, 1, root_records, GROWTH_LEAVES);
  uint64_t writes = fixture.writes;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &short_key, NULL));
  expected_position = 0;
  reachable_private = 0;
  walk_growth(candidate.root, 0);
  TEST_ASSERT_EQUAL(GROWTH_LEAVES * GROWTH_ITEMS_PER_LEAF, expected_position);
  TEST_ASSERT_LESS_OR_EQUAL(15, workspace.output_count);
  TEST_ASSERT_LESS_OR_EQUAL(15, workspace.consumed_count);
  TEST_ASSERT_EQUAL_UINT64(writes, fixture.writes);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static struct pfs_key
orphan_key(unsigned item)
{
  struct pfs_key key = { .length = PFS_ID_SIZE };
  key.bytes[PFS_ID_SIZE - 1] = (uint8_t)(item + 1);
  return key;
}

static void
walk_orphans(struct pfs_reference reference, size_t depth)
{
  get_node(reference, depth);
  struct pfs_tree *tree = &walk_tree[depth];
  if (depth) {
    TEST_ASSERT_GREATER_OR_EQUAL(6, tree->count);
  }
  struct pfs_record_context context = context_for(tree->header.birth);
  for (uint16_t i = 0; i < tree->count; ++i) {
    const uint8_t *data = walk_data[depth] + tree->slots[i].offset;
    if (tree->level) {
      struct pfs_internal_record child;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(data, tree->slots[i].length,
                            PFS_INDEX_ORPHANS, &context, &child));
      walk_orphans(child.child, depth + 1);
      TEST_ASSERT_EQUAL(0, pfs_key_compare(PFS_INDEX_ORPHANS, &child.minimum,
                                          &walk_tree[depth + 1].minimum));
    } else {
      while (expected_position < ORPHAN_TEST_ITEMS && !present[expected_position]) {
        ++expected_position;
      }
      TEST_ASSERT_LESS_THAN(ORPHAN_TEST_ITEMS, expected_position);
      struct pfs_orphan_record value;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_orphan_record_decode(data, tree->slots[i].length, &context, &value));
      struct pfs_key expected = orphan_key((unsigned)expected_position);
      TEST_ASSERT_EQUAL_MEMORY(expected.bytes, value.object.bytes, PFS_ID_SIZE);
      ++expected_position;
    }
  }
}

static void
orphan_index_uses_namespace_occupancy_and_private_disposal(void)
{
  start_case(PFS_INDEX_ORPHANS);
  memset(&candidate.context.object, 0, sizeof(candidate.context.object));
  candidate.context.block.features.read_required = PFS_FEATURE_ORPHANS;
  for (unsigned i = 0; i < ORPHAN_TEST_ITEMS; ++i) {
    unsigned item = i * 77 % ORPHAN_TEST_ITEMS;
    struct pfs_key key = orphan_key(item);
    struct pfs_orphan_record orphan;
    memcpy(orphan.object.bytes, key.bytes, PFS_ID_SIZE);
    struct pfs_record_context context = context_for(candidate.birth);
    uint8_t data[PFS_ORPHAN_RECORD_SIZE];
    struct pfs_encoded_record record = { .data = data };
    TEST_ASSERT_EQUAL(PFS_OK, pfs_orphan_record_encode(data, sizeof(data), &context, &orphan, &record.length));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_INSERT, &key, &record));
    present[item] = true;
  }
  for (unsigned i = 0; i < ORPHAN_TEST_ITEMS; ++i) {
    unsigned item = i * 77 % ORPHAN_TEST_ITEMS;
    struct pfs_key key = orphan_key(item);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_edit_tree(&candidate, &workspace, PFS_EDIT_DELETE, &key, NULL));
    present[item] = false;
    expected_position = 0;
    reachable_private = 0;
    if (candidate.root.block) {
      walk_orphans(candidate.root, 0);
    }
    while (expected_position < ORPHAN_TEST_ITEMS && !present[expected_position]) {
      ++expected_position;
    }
    TEST_ASSERT_EQUAL(ORPHAN_TEST_ITEMS, expected_position);
    TEST_ASSERT_EQUAL(0, candidate.retired_count);
    size_t active = 0;
    for (size_t j = 0; j < candidate.slot_capacity; ++j) {
      active += slots[j].active;
    }
    TEST_ASSERT_EQUAL(active, reachable_private);
  }
  TEST_ASSERT_EQUAL(0, candidate.root.block);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

void
run_edit_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(namespace_edits_preserve_order_profile_and_published_blocks);
  RUN_TEST(repeated_updates_reuse_private_buffers_and_fail_atomically);
  RUN_TEST(extent_cross_leaf_overlap_is_refused);
  RUN_TEST(extent_new_split_overlap_is_refused);
  RUN_TEST(sparse_two_child_sibling_repair_preserves_contents);
  RUN_TEST(sparse_three_child_sibling_repair_preserves_contents);
  RUN_TEST(minimum_and_maximum_component_lengths);
  RUN_TEST(maximum_depth_sparse_delete_fits_retirement_bound);
  RUN_TEST(namespace_minimum_growth_can_split_root_on_delete);
  RUN_TEST(orphan_index_uses_namespace_occupancy_and_private_disposal);
}

#define EXTENDED_NAME_GROUPS 64u
#define EXTENDED_NAMES_PER_GROUP 17u
#define EXTENDED_NAME_ITEMS (EXTENDED_NAME_GROUPS * EXTENDED_NAMES_PER_GROUP)
#define EXTENDED_DEEP_ITEMS 559872u
#define EXTENDED_GEOMETRY (UINT64_C(1) << 20)

static uint64_t extended_seed;
static uint64_t extended_random;
static size_t extended_operation;
static bool extended_present[EXTENDED_NAME_ITEMS];
static uint32_t extended_version[EXTENDED_NAME_ITEMS];
static unsigned extended_order[EXTENDED_NAME_ITEMS];
static bool extended_deep;
static bool extended_low_present;
static bool extended_high_present;
static uint32_t extended_low_version;
static uint32_t extended_high_version;
static size_t extended_expected;
static size_t extended_max_new;
static size_t extended_max_retired;
static size_t extended_max_depth;
static char extended_diagnostic[192];
static uint64_t extended_next_block;
static unsigned extended_build_position;
static uint8_t extended_build_data[PFS_TREE_DEPTH_MAX][6][304];
static struct pfs_encoded_record extended_build_records[PFS_TREE_DEPTH_MAX][6];

static uint64_t
extended_next_random(void)
{
  extended_random ^= extended_random << 13;
  extended_random ^= extended_random >> 7;
  extended_random ^= extended_random << 17;
  return extended_random;
}

static struct pfs_record_context
extended_context(uint64_t birth)
{
  struct pfs_record_context context = context_for(birth);
  context.block_count = candidate.context.block.block_count;
  return context;
}

static struct pfs_key
extended_key(unsigned item)
{
  struct pfs_key key = {.length = PFS_NAME_MAX};
  if (extended_deep) {
    char prefix[9];
    snprintf(prefix, sizeof(prefix), "%08u", item);
    memcpy(key.bytes, prefix, 8);
    memset(key.bytes + 8, 'x', PFS_NAME_MAX - 8);
  } else {
    unsigned group = item / EXTENDED_NAMES_PER_GROUP;
    unsigned within = item % EXTENDED_NAMES_PER_GROUP;
    key.bytes[0] = (uint8_t)('0' + group);
    if (!within) {
      key.length = 1;
    } else {
      char prefix[5];
      snprintf(prefix, sizeof(prefix), "%04u", within);
      memcpy(key.bytes + 1, prefix, 4);
      memset(key.bytes + 5, 'a' + (int)((extended_seed + item) % 26), PFS_NAME_MAX - 5);
    }
  }
  return key;
}

static uint32_t
extended_incarnation(unsigned item)
{
  if (!extended_deep) {
    return extended_version[item];
  }
  return item == 0 ? extended_low_version :
    item == EXTENDED_DEEP_ITEMS - 1 ? extended_high_version : 1;
}

static struct pfs_object_id
extended_object(unsigned item)
{
  struct pfs_object_id id = {0};
  test_put_u32(id.bytes, item + 1);
  test_put_u32(id.bytes + 4, extended_incarnation(item));
  return id;
}

static struct pfs_encoded_record
extended_record(unsigned item, uint64_t birth, uint8_t *data)
{
  struct pfs_key key = extended_key(item);
  struct pfs_dirent_record value = {.name = {.length = key.length},
    .child_kind = PFS_OBJECT_FILE, .object = extended_object(item)};
  memcpy(value.name.bytes, key.bytes, key.length);
  struct pfs_record_context context = extended_context(birth);
  struct pfs_encoded_record record = {.data = data};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_encode(data, 304, &context, &value, &record.length));
  return record;
}

static bool
extended_item_present(size_t item)
{
  if (!extended_deep) {
    return extended_present[item];
  }
  return item == 0 ? extended_low_present :
    item == EXTENDED_DEEP_ITEMS - 1 ? extended_high_present : true;
}

static void
extended_walk(struct pfs_reference reference, size_t depth)
{
  get_node(reference, depth);
  struct pfs_tree *tree = &walk_tree[depth];
  if (depth) {
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(6, tree->count, extended_diagnostic);
  } else if (tree->level) {
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(2, tree->count, extended_diagnostic);
  }
  if (depth + 1 > extended_max_depth) {
    extended_max_depth = depth + 1;
  }
  struct pfs_record_context context = extended_context(tree->header.birth);
  for (uint16_t i = 0; i < tree->count; i++) {
    const uint8_t *data = walk_data[depth] + tree->slots[i].offset;
    if (tree->level) {
      struct pfs_internal_record child;
      TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, pfs_internal_record_decode(data,
        tree->slots[i].length, PFS_INDEX_DIRECTORY, &context, &child), extended_diagnostic);
      extended_walk(child.child, depth + 1);
      TEST_ASSERT_EQUAL_MESSAGE(0, pfs_key_compare(PFS_INDEX_DIRECTORY,
        &child.minimum, &walk_tree[depth + 1].minimum), extended_diagnostic);
    } else {
      size_t total = extended_deep ? EXTENDED_DEEP_ITEMS : EXTENDED_NAME_ITEMS;
      while (extended_expected < total && !extended_item_present(extended_expected)) {
        extended_expected++;
      }
      TEST_ASSERT_LESS_THAN_MESSAGE(total, extended_expected, extended_diagnostic);
      struct pfs_dirent_record value;
      TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, pfs_dirent_record_decode(data,
        tree->slots[i].length, &context, &value), extended_diagnostic);
      struct pfs_key expected = extended_key((unsigned)extended_expected);
      struct pfs_object_id object = extended_object((unsigned)extended_expected);
      TEST_ASSERT_EQUAL_MESSAGE(expected.length, value.name.length, extended_diagnostic);
      TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected.bytes, value.name.bytes,
        expected.length, extended_diagnostic);
      TEST_ASSERT_EQUAL_MEMORY_MESSAGE(object.bytes, value.object.bytes,
        PFS_ID_SIZE, extended_diagnostic);
      TEST_ASSERT_EQUAL_MESSAGE(PFS_OBJECT_FILE, value.child_kind, extended_diagnostic);
      extended_expected++;
    }
  }
}

static void
extended_check(void)
{
  extended_expected = 0;
  reachable_private = 0;
  if (candidate.root.block) {
    extended_walk(candidate.root, 0);
  }
  size_t total = extended_deep ? EXTENDED_DEEP_ITEMS : EXTENDED_NAME_ITEMS;
  while (extended_expected < total && !extended_item_present(extended_expected)) {
    extended_expected++;
  }
  TEST_ASSERT_EQUAL_MESSAGE(total, extended_expected, extended_diagnostic);
  size_t active = 0;
  for (size_t i = 0; i < candidate.slot_capacity; i++) {
    active += slots[i].active;
  }
  TEST_ASSERT_EQUAL_MESSAGE(active, reachable_private, extended_diagnostic);
}

static void
extended_start(bool deep)
{
  start_case(PFS_INDEX_DIRECTORY);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, EXTENDED_GEOMETRY, 0));
  candidate.context.block.block_count = EXTENDED_GEOMETRY;
  candidate.slot_capacity = 15;
  candidate.retired_capacity = 15;
  extended_random = extended_seed ? extended_seed : UINT64_C(0x92d68ca2f31e7485);
  extended_operation = 0;
  extended_deep = deep;
  extended_max_new = extended_max_retired = extended_max_depth = 0;
  memset(extended_present, 0, sizeof(extended_present));
  memset(extended_version, 0, sizeof(extended_version));
  snprintf(extended_diagnostic, sizeof(extended_diagnostic),
    "seed=%llu case=%s initial tree", (unsigned long long)extended_seed, deep ? "depth8" : "mixed");
}

static void
extended_publish(void)
{
  for (size_t i = 0; i < candidate.slot_capacity; i++) {
    if (slots[i].active) {
      TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, pfs_block_write(&fixture.builder,
        slots[i].block, 1, slots[i].data, PFS_BLOCK_SIZE), extended_diagnostic);
    }
    slots[i].active = false;
    slots[i].block = extended_next_block++;
  }
  candidate.birth++;
  candidate.retired_count = 0;
}

static void
extended_edit(unsigned item, enum pfs_edit_operation operation)
{
  snprintf(extended_diagnostic, sizeof(extended_diagnostic),
    "seed=%llu case=%s operation=%zu %s item=%u generation=%llu",
    (unsigned long long)extended_seed, extended_deep ? "depth8" : "mixed",
    extended_operation, operation == PFS_EDIT_INSERT ? "insert" : "delete", item,
    (unsigned long long)candidate.birth);
  uint64_t writes = fixture.writes;
  uint8_t data[304];
  struct pfs_key key = extended_key(item);
  struct pfs_encoded_record record = {0};
  if (operation == PFS_EDIT_INSERT) {
    record = extended_record(item, candidate.birth, data);
  }
  enum pfs_status status = pfs_edit_tree(&candidate, &workspace, operation,
    &key, operation == PFS_EDIT_INSERT ? &record : NULL);
  TEST_ASSERT_EQUAL_MESSAGE(PFS_OK, status, extended_diagnostic);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(writes, fixture.writes, extended_diagnostic);
  TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(15, workspace.output_count, extended_diagnostic);
  TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(operation == PFS_EDIT_INSERT ? 8 : 15,
    workspace.consumed_count, extended_diagnostic);
  size_t actual_new = 0;
  for (size_t i = 0; i < candidate.slot_capacity; i++) {
    actual_new += slots[i].active;
  }
  TEST_ASSERT_EQUAL_MESSAGE(workspace.output_count, actual_new, extended_diagnostic);
  TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(15, actual_new, extended_diagnostic);
  TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(operation == PFS_EDIT_INSERT ? 8 : 15,
    candidate.retired_count, extended_diagnostic);
  if (actual_new > extended_max_new) {
    extended_max_new = actual_new;
  }
  if (candidate.retired_count > extended_max_retired) {
    extended_max_retired = candidate.retired_count;
  }
  if (extended_deep) {
    if (!item) {
      extended_low_present = operation == PFS_EDIT_INSERT;
    } else {
      extended_high_present = operation == PFS_EDIT_INSERT;
    }
  } else {
    extended_present[item] = operation == PFS_EDIT_INSERT;
  }
  extended_operation++;
}

static void
extended_shuffle(void)
{
  for (unsigned i = 0; i < EXTENDED_NAME_ITEMS; i++) {
    extended_order[i] = i;
  }
  for (size_t i = EXTENDED_NAME_ITEMS - 1; i; i--) {
    size_t chosen = (size_t)(extended_next_random() % (i + 1));
    unsigned saved = extended_order[i];
    extended_order[i] = extended_order[chosen];
    extended_order[chosen] = saved;
  }
}

static void
seeded_namespace_minima_growth_and_name_reuse(void)
{
  extended_start(false);
  extended_next_block = 1000;
  extended_shuffle();
  for (unsigned i = 0; i < EXTENDED_NAME_ITEMS; i++) {
    unsigned item = extended_order[i];
    extended_version[item]++;
    extended_edit(item, PFS_EDIT_INSERT);
    if (!(i % 32)) {
      extended_check();
    }
    extended_publish();
  }
  extended_check();
  for (unsigned cycle = 0; cycle < 3; cycle++) {
    /* Removing each one-byte group minimum exposes a 255-byte separator.
     * Seeded churn between those removals and reuse changes repair siblings. */
    for (unsigned group = 0; group < EXTENDED_NAME_GROUPS; group++) {
      extended_edit(group * EXTENDED_NAMES_PER_GROUP, PFS_EDIT_DELETE);
      extended_check();
      extended_publish();
    }
    for (unsigned i = 0; i < 512; i++) {
      unsigned group = (unsigned)(extended_next_random() % EXTENDED_NAME_GROUPS);
      unsigned within = 1 + (unsigned)(extended_next_random() % (EXTENDED_NAMES_PER_GROUP - 1));
      unsigned item = group * EXTENDED_NAMES_PER_GROUP + within;
      enum pfs_edit_operation operation = extended_present[item] ? PFS_EDIT_DELETE : PFS_EDIT_INSERT;
      extended_version[item] += operation == PFS_EDIT_INSERT;
      extended_edit(item, operation);
      if (!(i % 16)) {
        extended_check();
      }
      extended_publish();
    }
    for (unsigned group = EXTENDED_NAME_GROUPS; group; group--) {
      unsigned item = (group - 1) * EXTENDED_NAMES_PER_GROUP;
      extended_version[item]++;
      extended_edit(item, PFS_EDIT_INSERT);
      extended_check();
      extended_publish();
    }
  }
  extended_shuffle();
  for (unsigned i = 0; i < EXTENDED_NAME_ITEMS; i++) {
    unsigned item = extended_order[i];
    if (extended_present[item]) {
      extended_edit(item, PFS_EDIT_DELETE);
      if (!(i % 16)) {
        extended_check();
      }
      extended_publish();
    }
  }
  extended_check();
  TEST_ASSERT_EQUAL_UINT64(0, candidate.root.block);
  printf("editor mixed seed=%llu operations=%zu max_new=%zu max_retired=%zu depth=%zu\n",
    (unsigned long long)extended_seed, extended_operation,
    extended_max_new, extended_max_retired, extended_max_depth);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static struct pfs_reference
extended_build_namespace(uint16_t level, bool root)
{
  uint16_t count = root ? 2 : 6;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t *data = extended_build_data[level][i];
    if (!level) {
      extended_build_records[level][i] = extended_record(extended_build_position++, 1, data);
    } else {
      struct pfs_key minimum = extended_key(extended_build_position);
      struct pfs_reference child = extended_build_namespace(level - 1, false);
      struct pfs_internal_record record = {.child = child, .minimum = minimum};
      struct pfs_record_context context = extended_context(1);
      extended_build_records[level][i] = (struct pfs_encoded_record){.data = data};
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_encode(data, 304, PFS_INDEX_DIRECTORY,
        &context, &record, &extended_build_records[level][i].length));
    }
  }
  return write_tree(extended_next_block++, level, extended_build_records[level], count);
}

static void
admissible_depth_eight_namespace_cascade_fits_fifteen_nodes(void)
{
  extended_start(true);
  candidate.birth = 2;
  extended_next_block = 1000;
  extended_build_position = 0;
  extended_low_present = extended_high_present = true;
  extended_low_version = extended_high_version = 1;
  candidate.root = extended_build_namespace(PFS_TREE_DEPTH_MAX - 1, true);
  TEST_ASSERT_EQUAL(EXTENDED_DEEP_ITEMS, extended_build_position);
  /* 2*6^7 entries: every non-root is at the admitted six-item minimum.
   * Unlike a sparse synthetic path, the entire depth-eight tree is present. */
  extended_check();
  TEST_ASSERT_EQUAL(PFS_TREE_DEPTH_MAX, extended_max_depth);
  unsigned first = extended_next_random() & 1 ? 0 : EXTENDED_DEEP_ITEMS - 1;
  unsigned second = first ? 0 : EXTENDED_DEEP_ITEMS - 1;
  extended_edit(first, PFS_EDIT_DELETE);
  extended_publish();
  extended_edit(second, PFS_EDIT_DELETE);
  extended_check();
  extended_publish();
  extended_low_version = extended_high_version = 2;
  extended_edit(second, PFS_EDIT_INSERT);
  extended_publish();
  extended_edit(first, PFS_EDIT_INSERT);
  extended_check();
  printf("editor depth8 seed=%llu entries=%u operations=%zu max_new=%zu max_retired=%zu depth=%zu\n",
    (unsigned long long)extended_seed, EXTENDED_DEEP_ITEMS, extended_operation,
    extended_max_new, extended_max_retired, extended_max_depth);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

void
run_extended_edit_tests(uint64_t seed)
{
  extended_seed = seed;
  Unity.TestFile = __FILE__;
  RUN_TEST(seeded_namespace_minima_growth_and_name_reuse);
  RUN_TEST(admissible_depth_eight_namespace_cascade_fits_fifteen_nodes);
}
