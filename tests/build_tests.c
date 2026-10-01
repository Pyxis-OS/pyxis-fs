/* SPDX-License-Identifier: MPL-2.0 */
#include "build_tests.h"
#include "support.h"
#include "unity.h"

#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/read.h>
#include <pyxis_fs/tree.h>

#include <string.h>

enum name_profile {
  NAME_MINIMUM,
  NAME_MAXIMUM,
  NAME_VARIABLE,
};

static struct test_fixture fixture;
static const uint8_t payload[] = "formatter contents";

struct namespace_oracle {
  const struct pfs_build_plan *plan;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  enum name_profile profile;
  size_t entries;
  size_t visited;
  uint16_t leaf_level;
};

static struct pfs_object_id
object_id(size_t ordinal)
{
  struct pfs_object_id id = {0};
  id.bytes[0] = 0x41;
  test_put_u64(id.bytes + 8, ordinal + 1);
  return id;
}

static struct pfs_name
expected_name(enum name_profile profile, size_t ordinal)
{
  struct pfs_name name = {0};
  if (profile == NAME_MINIMUM) {
    /* One-byte UTF-8 names in ascending order, skipping the two invalid names. */
    name.length = 1;
    name.bytes[0] = (uint8_t)(ordinal + 1);
    if (name.bytes[0] >= '.') {
      name.bytes[0] += 2;
    }
    return name;
  }
  name.length = profile == NAME_MAXIMUM || ordinal % 3 == 0 ? PFS_NAME_MAX : 4;
  memset(name.bytes, 'x', name.length);
  for (size_t i = 4; i; --i) {
    name.bytes[i - 1] = (uint8_t)('0' + ordinal % 10);
    ordinal /= 10;
  }
  return name;
}

static size_t
align8(size_t value)
{
  return (value + 7) & ~(size_t)7;
}

static size_t
object_nodes(size_t records)
{
  size_t total = 0;
  size_t count = (records + 28) / 29;
  for (;;) {
    total += count;
    if (count == 1) {
      return total;
    }
    count = (count + 56) / 57;
  }
}

static void
walk_namespace(struct namespace_oracle *oracle, struct pfs_reference reference,
               uint16_t parent_level, uint16_t depth, struct pfs_key *minimum)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  struct pfs_tree_context context = {
    .block = {
      .block_count = oracle->plan->block_count,
      .selected_generation = 1,
      .referring_birth = 1,
      .pool = oracle->plan->pool,
      .reference = reference,
    },
    .kind = PFS_INDEX_DIRECTORY,
    .parent_level = parent_level,
    .volume = oracle->volume,
    .object = oracle->object,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, reference.block,
                                         1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, sizeof(bytes), &context, &tree));
  ++oracle->visited;
  TEST_ASSERT_GREATER_OR_EQUAL(parent_level ? 6 : tree.level ? 2 : 1, tree.count);
  size_t record_bytes = 0;
  struct pfs_record_context record_context = {
    .block_count = oracle->plan->block_count,
    .selected_generation = 1,
    .containing_birth = 1,
  };
  for (size_t i = 0; i < tree.count; ++i) {
    const struct pfs_tree_slot *slot = &tree.slots[i];
    record_bytes += slot->length;
    if (tree.level) {
      struct pfs_internal_record record;
      struct pfs_key child_minimum;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(bytes + slot->offset,
        slot->length, PFS_INDEX_DIRECTORY, &record_context, &record));
      walk_namespace(oracle, record.child, tree.level, depth + 1, &child_minimum);
      TEST_ASSERT_EQUAL_UINT16(child_minimum.length, record.minimum.length);
      TEST_ASSERT_EQUAL_MEMORY(child_minimum.bytes, record.minimum.bytes, child_minimum.length);
      TEST_ASSERT_EQUAL_UINT16(align8(48 + child_minimum.length), slot->length);
    } else {
      if (oracle->leaf_level == UINT16_MAX) {
        oracle->leaf_level = depth;
      }
      TEST_ASSERT_EQUAL_UINT16(oracle->leaf_level, depth);
      struct pfs_dirent_record record;
      struct pfs_name name = expected_name(oracle->profile, oracle->entries);
      struct pfs_object_id id = object_id(oracle->entries + 1);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_decode(bytes + slot->offset,
        slot->length, &record_context, &record));
      TEST_ASSERT_EQUAL_UINT16(name.length, record.name.length);
      TEST_ASSERT_EQUAL_MEMORY(name.bytes, record.name.bytes, name.length);
      TEST_ASSERT_EQUAL_MEMORY(id.bytes, record.object.bytes, PFS_ID_SIZE);
      TEST_ASSERT_EQUAL_UINT16(PFS_OBJECT_FILE, record.child_kind);
      TEST_ASSERT_EQUAL_UINT16(align8(40 + name.length), slot->length);
      ++oracle->entries;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(align8(192 + tree.count * 4) + record_bytes, tree.header.used);
  TEST_ASSERT_LESS_OR_EQUAL(PFS_BLOCK_SIZE, tree.header.used);
  *minimum = tree.minimum;
}

static enum pfs_status
source_read(void *context, size_t volume, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  size_t *reads = context;
  if (volume || object != 1 || offset || length != sizeof(payload) - 1) {
    return PFS_INVALID;
  }
  ++*reads;
  memcpy(buffer, payload, length);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  size_t *reads = context;
  return *reads == 1 ? PFS_OK : PFS_INVALID;
}

static void
build_case(enum name_profile profile, size_t children, size_t expected_nodes,
           uint16_t expected_depth)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN,
                                            PFS_MEMORY_DEFAULT));
  struct pfs_allocation input = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_allocate(&fixture.memory,
    (children + 1) * sizeof(struct pfs_build_object), _Alignof(struct pfs_build_object), &input));
  struct pfs_build_object *objects = input.data;
  memset(objects, 0, input.size);
  objects[0].id = object_id(0);
  objects[0].parent = UINT32_MAX;
  objects[0].kind = PFS_OBJECT_DIRECTORY;
  for (size_t i = 1; i <= children; ++i) {
    size_t ordinal = children - i;
    objects[i].id = object_id(ordinal + 1);
    objects[i].parent = 0;
    objects[i].kind = PFS_OBJECT_FILE;
    objects[i].name = expected_name(profile, ordinal);
  }
  if (children) {
    objects[1].file_length = sizeof(payload) - 1;
  }
  struct pfs_build_volume volume = {
    .id = {.bytes = {0x22}},
    .owner = {.bytes = {0x33}},
    .root_object = objects[0].id,
    .name = {.length = 4, .bytes = {'t', 'e', 's', 't'}},
    .objects = objects,
    .object_count = children + 1,
  };
  struct pfs_build_spec spec = {
    .pool = {.bytes = {0x11}},
    .block_count = PFS_POOL_BLOCKS_MIN,
    .volumes = &volume,
    .volume_count = 1,
  };
  struct pfs_build_plan plan = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&fixture.memory, &spec, &plan));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.writes);
  TEST_ASSERT_EQUAL_UINT64(0, fixture.reads);
  TEST_ASSERT_EQUAL_UINT64(4, plan.pool_root.live_pool);
  TEST_ASSERT_EQUAL_UINT64(1024, plan.pool_root.cow.capacity);
  TEST_ASSERT_EQUAL_UINT64(1024, plan.pool_root.migration.capacity);
  TEST_ASSERT_EQUAL_UINT64(256, plan.pool_root.recovery.capacity);
  TEST_ASSERT_EQUAL_UINT64(spec.block_count - 2, plan.pool_root.live_pool +
    plan.pool_root.live_volume + plan.pool_root.free);
  size_t source_reads = 0;
  struct pfs_build_source source = {
    .context = &source_reads,
    .read = source_read,
    .validate = source_validate,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &fixture.builder, children ? &source : NULL));
  TEST_ASSERT_EQUAL_UINT64(2, fixture.flushes);
  TEST_ASSERT_EQUAL_size_t(children ? 1 : 0, source_reads);

  struct pfs_pool pool = {0};
  struct pfs_pool_diagnostic diagnostic;
  struct pfs_volume opened = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &fixture.builder.reader,
                                        &fixture.memory, &diagnostic));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &volume.id, &opened));
  struct pfs_object_record root;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_object(&opened, &volume.root_object, &root));
  TEST_ASSERT_EQUAL_UINT64(children, root.directory_count);
  struct namespace_oracle oracle = {
    .plan = &plan,
    .volume = volume.id,
    .object = volume.root_object,
    .profile = profile,
    .leaf_level = UINT16_MAX,
  };
  if (children) {
    struct pfs_key minimum;
    walk_namespace(&oracle, root.tree_root, 0, 0, &minimum);
    struct pfs_name first = expected_name(profile, 0);
    TEST_ASSERT_EQUAL_UINT16(first.length, minimum.length);
    TEST_ASSERT_EQUAL_MEMORY(first.bytes, minimum.bytes, first.length);
    TEST_ASSERT_EQUAL_UINT16(expected_depth, oracle.leaf_level);
  } else {
    TEST_ASSERT_EQUAL_UINT16(PFS_STORAGE_NONE, root.storage_kind);
  }
  TEST_ASSERT_EQUAL_size_t(children, oracle.entries);
  if (expected_nodes != SIZE_MAX) {
    TEST_ASSERT_EQUAL_size_t(expected_nodes, oracle.visited);
  }
  size_t expected_metadata = 4 + object_nodes(children + 1) + 1 + oracle.visited;
  TEST_ASSERT_EQUAL_UINT64(expected_metadata - 4 + (children != 0), plan.volumes[0].live_blocks);
  TEST_ASSERT_EQUAL_UINT64(plan.volumes[0].live_blocks, plan.pool_root.live_volume);
  TEST_ASSERT_EQUAL_UINT64(expected_metadata + (children != 0) + 2, fixture.writes);

  struct pfs_grant_record grant;
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_grants(&opened, &volume.root_object,
                                                       &grant, 1, &count));
  TEST_ASSERT_EQUAL_size_t(1, count);
  TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x3f), grant.directory_rights);
  if (children) {
    uint8_t bytes[PFS_BLOCK_SIZE];
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&opened, &objects[1].id,
                                                       0, bytes, sizeof(bytes), &count));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload) - 1, count);
    TEST_ASSERT_EQUAL_MEMORY(payload, bytes, count);
    struct pfs_object_record file;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_object(&opened, &objects[1].id, &file));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader,
      file.inline_extent.physical_first, 1, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL_MEMORY(payload, bytes, sizeof(payload) - 1);
    for (size_t i = sizeof(payload) - 1; i < sizeof(bytes); ++i) {
      TEST_ASSERT_EQUAL_UINT8(0, bytes[i]);
    }
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&opened));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  struct pfs_check_result checked;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&fixture.builder.reader, &fixture.memory,
                                    NULL, NULL, &checked));
  for (size_t slot = 0; slot < 2; ++slot) {
    TEST_ASSERT_TRUE(checked.state[slot].complete);
    TEST_ASSERT_EQUAL_UINT64(children, checked.state[slot].directory_entries);
    TEST_ASSERT_EQUAL_UINT64(expected_metadata, checked.state[slot].metadata_blocks);
    TEST_ASSERT_EQUAL_UINT64(children != 0, checked.state[slot].file_blocks);
  }
  TEST_ASSERT_TRUE(checked.cross_complete);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_memory_free(&fixture.memory, &input));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
  TEST_ASSERT_TRUE(test_fixture_close(&fixture));
}

static void
test_directory_root_exceptions(void)
{
  build_case(NAME_MINIMUM, 0, 0, 0);
  for (size_t count = 1; count <= 5; ++count) {
    build_case(NAME_MINIMUM, count, 1, 0);
    build_case(NAME_MAXIMUM, count, 1, 0);
  }
  build_case(NAME_MAXIMUM, 13, 1, 0);
}

static void
test_directory_leaf_tails(void)
{
  for (size_t tail = 1; tail <= 5; ++tail) {
    build_case(NAME_MINIMUM, 75 + tail, 3, 1);
    build_case(NAME_MAXIMUM, 13 + tail, 3, 1);
  }
}

static void
test_directory_internal_tails(void)
{
  for (size_t tail = 1; tail <= 5; ++tail) {
    build_case(NAME_MAXIMUM, 13 * (12 + tail), 15 + tail, 2);
  }
  build_case(NAME_MAXIMUM, 1885, 161, 3);
}

static void
test_directory_variable_names(void)
{
  build_case(NAME_VARIABLE, 91, SIZE_MAX, 1);
  build_case(NAME_VARIABLE, 777, SIZE_MAX, 2);
}

void
run_build_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(test_directory_root_exceptions);
  RUN_TEST(test_directory_leaf_tails);
  RUN_TEST(test_directory_internal_tails);
  RUN_TEST(test_directory_variable_names);
}
