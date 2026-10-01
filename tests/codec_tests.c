/* SPDX-License-Identifier: MPL-2.0 */
#include "codec_tests.h"
#include "support.h"
#include "unity.h"

#include "canonical.h"
#include <pyxis_fs/access.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/record.h>
#include <string.h>

static struct test_fixture fixture;
static const struct pfs_record_context pool_records = {
  .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .containing_birth = 1,
};
static const struct pfs_record_context volume_records = {
  .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .containing_birth = 1,
  .features = {.read_required = PFS_FEATURE_ORPHANS},
};

static void
independent_record(uint8_t *bytes, size_t length, uint16_t type)
{
  test_put_u16(bytes, type);
  test_put_u16(bytes + 2, 1);
  test_put_u32(bytes + 4, (uint32_t)length);
  test_checksum(bytes, length, 8);
}

static void
standalone_block_matching_preserves_pool_feature_scope(void)
{
  struct pfs_block_header header = {
    .type = PFS_BLOCK_TREE, .version = PFS_FORMAT_VERSION,
    .pool = {{1}}, .block = 7, .birth = 1,
  };
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = 1, .referring_birth = 1,
    .pool = {{1}}, .reference = {7, 1, PFS_BLOCK_TREE, PFS_FORMAT_VERSION},
    .features = {.read_required = PFS_FEATURE_ORPHANS},
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_match(&header, &context, PFS_BLOCK_TREE));
  header.type = context.reference.type = PFS_BLOCK_POOL;
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_block_match(&header, &context, PFS_BLOCK_POOL));
  header.type = context.reference.type = PFS_BLOCK_SUPER;
  header.block = context.reference.block = 0;
  /* Pool feature refusal also precedes superblock reference-envelope checking. */
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_block_match(&header, &context, PFS_BLOCK_SUPER));
}

static void
orphan_known_vector_and_feature_scope(void)
{
  uint8_t bytes[32] = {0};
  bytes[16] = 9;
  independent_record(bytes, sizeof(bytes), 8);
  struct pfs_orphan_record orphan;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_orphan_record_decode(bytes, sizeof(bytes), &volume_records, &orphan));
  TEST_ASSERT_EQUAL_UINT8(9, orphan.object.bytes[0]);
  uint8_t encoded[32];
  size_t written;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_orphan_record_encode(encoded, sizeof(encoded), &volume_records, &orphan, &written));
  TEST_ASSERT_EQUAL_UINT(sizeof(bytes), written);
  TEST_ASSERT_EQUAL_MEMORY(bytes, encoded, sizeof(bytes));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_orphan_record_decode(bytes, sizeof(bytes), &pool_records, &orphan));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_features_read(&volume_records.features));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_features_check(&volume_records.features));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_features_read(&volume_records.features));
  struct pfs_features future = {.read_required = 2};
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_volume_features_read(&future));
  future = (struct pfs_features){.write_required = 2};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_features_read(&future));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_volume_features_write(&future));
  memset(bytes + 16, 0, 16);
  independent_record(bytes, sizeof(bytes), 8);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_orphan_record_decode(bytes, sizeof(bytes), &volume_records, &orphan));
}

static void
volume_root_known_vector(void)
{
  uint8_t bytes[448] = {0};
  bytes[16] = 2;
  test_put_u16(bytes + 32, 4);
  memcpy(bytes + 40, "home", 4);
  test_put_u64(bytes + 296, 1);
  bytes[320] = 3;
  test_put_u64(bytes + 336, 5);
  test_put_u64(bytes + 344, 1);
  test_put_u16(bytes + 352, 3);
  test_put_u16(bytes + 354, 1);
  test_put_u64(bytes + 400, 4);
  test_put_u64(bytes + 416, 3);
  test_put_u64(bytes + 424, 7);
  test_put_u64(bytes + 432, 1);
  test_put_u16(bytes + 440, 3);
  test_put_u16(bytes + 442, 1);
  independent_record(bytes, sizeof(bytes), 1);
  struct pfs_volume_record volume;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_record_decode(bytes, sizeof(bytes), &pool_records, &volume));
  TEST_ASSERT_EQUAL_UINT64(7, volume.orphan_root.block);
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_volume_record_decode(bytes, sizeof(bytes), &volume_records, &volume));
  uint8_t encoded[448];
  size_t written;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_record_encode(encoded, sizeof(encoded), &pool_records, &volume, &written));
  TEST_ASSERT_EQUAL_MEMORY(bytes, encoded, sizeof(bytes));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_record_validate(bytes, sizeof(bytes), PFS_INDEX_VOLUMES, 0, &pool_records));
  test_put_u64(bytes + 296, 0);
  independent_record(bytes, sizeof(bytes), 1);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_record_decode(bytes, sizeof(bytes), &pool_records, &volume));
  TEST_ASSERT_EQUAL_UINT64(0, volume.orphan_root.block);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_record_validate(bytes, sizeof(bytes), PFS_INDEX_VOLUMES, 0, &pool_records));
  test_put_u64(bytes + 296, 1);
  test_put_u64(bytes + 432, 2);
  independent_record(bytes, sizeof(bytes), 1);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_volume_record_decode(bytes, sizeof(bytes), &pool_records, &volume));
}

static void
reserved_record_bytes_and_checkpoint(void)
{
  uint8_t object_bytes[128] = {0};
  object_bytes[16] = 4;
  test_put_u16(object_bytes + 32, PFS_OBJECT_FILE);
  object_bytes[40] = 5;
  independent_record(object_bytes, sizeof(object_bytes), 4);
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_object_record_decode(object_bytes, sizeof(object_bytes), &pool_records, &object));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_decode(object_bytes, sizeof(object_bytes), &volume_records, &object));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_record_validate(object_bytes, sizeof(object_bytes), PFS_INDEX_OBJECTS, 0, &volume_records));
  const size_t reserved[] = {12, 36, 112, 127};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    object_bytes[reserved[i]] = 1;
    independent_record(object_bytes, sizeof(object_bytes), 4);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_decode(object_bytes, sizeof(object_bytes), &volume_records, &object));
    TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_record_validate(object_bytes, sizeof(object_bytes), PFS_INDEX_OBJECTS, 0, &volume_records));
    object_bytes[reserved[i]] = 0;
  }
  uint8_t grant_bytes[80] = {0};
  grant_bytes[16] = 3;
  grant_bytes[32] = 5;
  test_put_u64(grant_bytes + 64, UINT64_C(1) << 6);
  independent_record(grant_bytes, sizeof(grant_bytes), 7);
  struct pfs_grant_record grant;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_decode(grant_bytes, sizeof(grant_bytes), &pool_records, &grant));
  TEST_ASSERT_EQUAL_UINT64(PFS_DIR_CHECKPOINT, grant.directory_rights);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_target_validate(&grant, PFS_OBJECT_DIRECTORY));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_grant_target_validate(&grant, PFS_OBJECT_FILE));
  test_put_u64(grant_bytes + 64, UINT64_C(1) << 7);
  independent_record(grant_bytes, sizeof(grant_bytes), 7);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_grant_record_decode(grant_bytes, sizeof(grant_bytes), &pool_records, &grant));
}

static void
name_padding_and_extension_profile(void)
{
  uint8_t bytes[56] = {0};
  test_put_u16(bytes + 16, 1);
  test_put_u16(bytes + 18, PFS_OBJECT_FILE);
  bytes[24] = 4;
  bytes[40] = 'a';
  independent_record(bytes, 48, PFS_RECORD_DIRENT);
  struct pfs_dirent_record entry;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_record_validate(bytes, 48, PFS_INDEX_DIRECTORY, 0, &pool_records));
  bytes[47] = 1;
  independent_record(bytes, 48, PFS_RECORD_DIRENT);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_decode(bytes, 48, &pool_records, &entry));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_record_validate(bytes, 48, PFS_INDEX_DIRECTORY, 0, &pool_records));
  bytes[47] = 0;
  bytes[48] = 9;
  independent_record(bytes, sizeof(bytes), PFS_RECORD_DIRENT);
  struct pfs_record_context extended = pool_records;
  extended.features.optional = 2;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_dirent_record_decode(bytes, sizeof(bytes), &extended, &entry));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_canonical_record_validate(bytes, sizeof(bytes), PFS_INDEX_DIRECTORY, 0, &extended));
}

static void
canonical_orphan_tree_and_internal_keys(void)
{
  uint8_t leaf[32] = {0};
  leaf[16] = 4;
  independent_record(leaf, sizeof(leaf), 8);
  struct pfs_encoded_record record = {leaf, sizeof(leaf)};
  struct pfs_tree_context context = {
    .block = {
      .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
      .pool = {{1}}, .reference = {7, 1, PFS_BLOCK_TREE, 1},
      .features = {.read_required = PFS_FEATURE_ORPHANS},
    },
    .kind = PFS_INDEX_ORPHANS, .volume = {{2}},
  };
  struct pfs_tree tree = {
    .header = {.type = PFS_BLOCK_TREE, .version = 1, .pool = {{1}}, .block = 7, .birth = 1},
    .kind = PFS_INDEX_ORPHANS, .count = 1, .volume = {{2}},
  };
  uint8_t bytes[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(bytes, sizeof(bytes), &context, &tree, &record));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_tree_validate(bytes, sizeof(bytes), &context, &tree));
  const size_t reserved[] = {14, 56, 127, 168, 191, 196, 4095};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    bytes[reserved[i]] = 1;
    test_checksum(bytes, sizeof(bytes), 20);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, sizeof(bytes), &context, &tree));
    TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_tree_validate(bytes, sizeof(bytes), &context, &tree));
    bytes[reserved[i]] = 0;
  }
  test_checksum(bytes, sizeof(bytes), 20);
  context.block.features.read_required = 0;
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_tree_decode(bytes, sizeof(bytes), &context, &tree));
  context.block.features.read_required = 1;
  bytes[152] = 4;
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_tree_decode(bytes, sizeof(bytes), &context, &tree));
  uint8_t internal[64] = {0};
  test_put_u16(internal + 16, 16);
  test_put_u64(internal + 24, 7);
  test_put_u64(internal + 32, 1);
  test_put_u16(internal + 40, PFS_BLOCK_TREE);
  test_put_u16(internal + 42, 1);
  internal[48] = 4;
  independent_record(internal, sizeof(internal), PFS_INTERNAL_RECORD_TYPE);
  struct pfs_internal_record decoded;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(internal, sizeof(internal), PFS_INDEX_ORPHANS, &volume_records, &decoded));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_record_validate(internal, sizeof(internal), PFS_INDEX_ORPHANS, 1, &volume_records));
  internal[44] = 1;
  independent_record(internal, sizeof(internal), PFS_INTERNAL_RECORD_TYPE);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_internal_record_decode(internal, sizeof(internal), PFS_INDEX_ORPHANS, &volume_records, &decoded));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_record_validate(internal, sizeof(internal), PFS_INDEX_ORPHANS, 1, &volume_records));
}

static struct pfs_reference
reference(uint64_t block)
{
  return (struct pfs_reference){block, 1, PFS_BLOCK_TREE, 1};
}

static void
write_tree(uint64_t physical, uint16_t kind, const struct pfs_encoded_record *records, uint16_t count)
{
  struct pfs_tree_context context = {
    .block = {
      .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
      .pool = {{1}}, .reference = reference(physical),
    },
    .kind = kind,
  };
  struct pfs_tree tree = {
    .header = {.type = PFS_BLOCK_TREE, .version = 1, .pool = {{1}}, .block = physical, .birth = 1},
    .kind = kind, .count = count,
  };
  if (kind >= PFS_INDEX_OBJECTS) {
    context.volume = (struct pfs_volume_id){{2}};
    context.block.features = volume_records.features;
    tree.volume = context.volume;
  }
  uint8_t bytes[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(bytes, sizeof(bytes), &context, &tree, records));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, physical, 1, bytes, sizeof(bytes)));
}

static void
build_orphan_fixture(void)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  uint8_t storage[3][448];
  size_t lengths[3];
  struct pfs_encoded_record records[3];
  struct pfs_volume_record volume = {
    .id = {{2}}, .name = {4, "home"}, .features = {.read_required = PFS_FEATURE_ORPHANS},
    .root_object = {{3}}, .object_root = reference(5), .grant_root = reference(6),
    .orphan_root = reference(7), .quota = 4, .live_blocks = 4, .object_count = 3,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_record_encode(storage[0], sizeof(storage[0]), &pool_records, &volume, &lengths[0]));
  records[0] = (struct pfs_encoded_record){storage[0], lengths[0]};
  write_tree(2, PFS_INDEX_VOLUMES, records, 1);
  struct pfs_volume_name_record name = {.name = {4, "home"}, .volume = {{2}}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_name_record_encode(storage[0], sizeof(storage[0]), &pool_records, &name, &lengths[0]));
  records[0].length = lengths[0];
  write_tree(3, PFS_INDEX_VOLUME_NAMES, records, 1);
  struct pfs_allocation_record allocations[] = {
    {.first = 1, .count = 4, .state = PFS_ALLOCATION_POOL, .birth = 1},
    {.first = 5, .count = 4, .state = PFS_ALLOCATION_VOLUME, .owner = {{2}}, .birth = 1},
    {.first = 9, .count = PFS_POOL_BLOCKS_MIN - 10, .state = PFS_ALLOCATION_FREE},
  };
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_encode(storage[i], sizeof(storage[i]), &pool_records, &allocations[i], &lengths[i]));
    records[i] = (struct pfs_encoded_record){storage[i], lengths[i]};
  }
  write_tree(4, PFS_INDEX_ALLOCATION, records, 3);
  struct pfs_object_record objects[] = {
    {.id = {{3}}, .kind = PFS_OBJECT_DIRECTORY, .owner = {{5}}},
    {.id = {{4}}, .kind = PFS_OBJECT_FILE, .owner = {{5}}, .storage_kind = PFS_STORAGE_INLINE,
     .file_length = 11, .inline_extent = {.count = 1, .physical_first = 8, .birth = 1}},
    {.id = {{6}}, .kind = PFS_OBJECT_DIRECTORY, .owner = {{5}}},
  };
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_encode(storage[i], sizeof(storage[i]), &volume_records, &objects[i], &lengths[i]));
    records[i] = (struct pfs_encoded_record){storage[i], lengths[i]};
  }
  write_tree(5, PFS_INDEX_OBJECTS, records, 3);
  struct pfs_grant_record grant = {.object = {{4}}, .principal = {{5}}, .file_rights = PFS_FILE_READ};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_encode(storage[0], sizeof(storage[0]), &volume_records, &grant, &lengths[0]));
  records[0] = (struct pfs_encoded_record){storage[0], lengths[0]};
  write_tree(6, PFS_INDEX_GRANTS, records, 1);
  struct pfs_orphan_record orphans[] = {{{{4}}}, {{{6}}}};
  for (size_t i = 0; i < 2; ++i) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_orphan_record_encode(storage[i], sizeof(storage[i]), &volume_records, &orphans[i], &lengths[i]));
    records[i] = (struct pfs_encoded_record){storage[i], lengths[i]};
  }
  write_tree(7, PFS_INDEX_ORPHANS, records, 2);
  uint8_t bytes[PFS_BLOCK_SIZE] = "orphan data";
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 8, 1, bytes, sizeof(bytes)));
  struct pfs_block_context pool_context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
    .pool = {{1}}, .reference = {1, 1, PFS_BLOCK_POOL, 1},
  };
  struct pfs_pool_root root = {
    .header = {.type = PFS_BLOCK_POOL, .version = 1, .used = 288, .pool = {{1}}, .block = 1, .birth = 1},
    .volumes = reference(2), .volume_names = reference(3), .allocation = reference(4),
    .volume_count = 1, .live_pool = 4, .live_volume = 4, .free = PFS_POOL_BLOCKS_MIN - 10,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_encode(bytes, sizeof(bytes), &pool_context, &root));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 1, 1, bytes, sizeof(bytes)));
  struct pfs_superblock superblock = {
    .header = {.type = PFS_BLOCK_SUPER, .version = 1, .used = 192, .pool = {{1}}, .birth = 1},
    .block_count = PFS_POOL_BLOCKS_MIN, .root = {1, 1, PFS_BLOCK_POOL, 1},
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(bytes, sizeof(bytes), &superblock));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 0, 1, bytes, sizeof(bytes)));
  superblock.header.block = PFS_POOL_BLOCKS_MIN - 1;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(bytes, sizeof(bytes), &superblock));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, superblock.header.block, 1, bytes, sizeof(bytes)));
}

static void
retained_orphans_check_and_diagnostic_access(void)
{
  build_orphan_fixture();
  struct pfs_check_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  for (size_t i = 0; i < 2; ++i) {
    TEST_ASSERT_TRUE(result.state[i].complete);
    TEST_ASSERT_EQUAL_UINT64(3, result.state[i].objects);
    TEST_ASSERT_EQUAL_UINT64(2, result.state[i].orphans);
    TEST_ASSERT_EQUAL_UINT64(1, result.state[i].grants);
    TEST_ASSERT_EQUAL_UINT64(1, result.state[i].file_blocks);
  }
  struct pfs_pool pool = {0};
  struct pfs_volume volume = {0};
  struct pfs_pool_diagnostic diagnostic;
  const struct pfs_volume_id volume_id = {{2}};
  const struct pfs_object_id file_id = {{4}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &fixture.builder.reader, &fixture.memory, &diagnostic));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &volume_id, &volume));
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_object(&volume, &file_id, &object));
  TEST_ASSERT_EQUAL_UINT64(11, object.file_length);
  uint8_t bytes[16];
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&volume, &file_id, 0, bytes, sizeof(bytes), &count));
  TEST_ASSERT_EQUAL_UINT(11, count);
  TEST_ASSERT_EQUAL_MEMORY("orphan data", bytes, 11);
  struct pfs_trusted_context context = {
    .principal = {{5}}, .root = file_id, .scope = PFS_SCOPE_OBJECT, .ceiling = {.file = PFS_FILE_READ},
  };
  const struct pfs_rights rights = {.file = PFS_FILE_READ};
  struct pfs_view *view = NULL;
  TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_view_acquire(&volume, &context, &file_id, PFS_SCOPE_OBJECT, &rights, &view));
  TEST_ASSERT_NULL(view);
  struct pfs_grant_record grant;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_grants(&volume, &file_id, &grant, 1, &count));
  TEST_ASSERT_EQUAL_UINT(1, count);
  TEST_ASSERT_EQUAL_UINT64(PFS_FILE_READ, grant.file_rights);
  const struct pfs_object_id directory_id = {{6}};
  struct pfs_directory_cursor cursor = {0};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_directory_open(&volume, &directory_id, &cursor));
  struct pfs_dirent_record entry;
  bool done = false;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_directory_next(&cursor, &entry, 1, &count, &done));
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_UINT(0, count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_directory_close(&cursor));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volume));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
}

static void
rewrite_byte(uint64_t block, size_t offset, uint8_t value, size_t record_offset, size_t record_length)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, block, 1, bytes, sizeof(bytes)));
  bytes[offset] = value;
  if (record_length) {
    test_checksum(bytes + record_offset, record_length, 8);
  }
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, block, 1, bytes, sizeof(bytes)));
}

static void
malformed_orphan_relations(void)
{
  build_orphan_fixture();
  struct pfs_check_result result;
  /* Orphan marker points at the root, then at a missing same-volume object. */
  const uint8_t targets[] = {3, 9};
  for (size_t i = 0; i < sizeof(targets); ++i) {
    rewrite_byte(7, 216, targets[i], 200, 32);
    TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
    TEST_ASSERT_FALSE(result.state[0].complete);
    TEST_ASSERT_FALSE(result.state[1].complete);
  }
  rewrite_byte(7, 216, 4, 200, 32);
  /* Parentless file has no corresponding orphan marker. */
  rewrite_byte(7, 216, 5, 200, 32);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  rewrite_byte(7, 216, 4, 200, 32);
  /* Named parent association plus orphan marker is invalid. */
  rewrite_byte(5, 392, 3, 336, 128);
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
}

static void
make_named_file(void)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 5, 1, bytes, sizeof(bytes)));
  test_put_u16(bytes + 242, PFS_STORAGE_TREE);
  test_put_u64(bytes + 288, 9);
  test_put_u64(bytes + 296, 1);
  test_put_u16(bytes + 304, PFS_BLOCK_TREE);
  test_put_u16(bytes + 306, 1);
  test_put_u64(bytes + 312, 1);
  bytes[392] = 3;
  test_checksum(bytes + 208, 128, 8);
  test_checksum(bytes + 336, 128, 8);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 5, 1, bytes, sizeof(bytes)));
  uint8_t dirent[48] = {0};
  test_put_u16(dirent + 16, 4);
  test_put_u16(dirent + 18, PFS_OBJECT_FILE);
  dirent[24] = 4;
  memcpy(dirent + 40, "file", 4);
  independent_record(dirent, sizeof(dirent), PFS_RECORD_DIRENT);
  struct pfs_tree_context context = {
    .block = {
      .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
      .pool = {{1}}, .reference = {9, 1, PFS_BLOCK_TREE, 1},
      .features = {.read_required = PFS_FEATURE_ORPHANS},
    },
    .kind = PFS_INDEX_DIRECTORY, .volume = {{2}}, .object = {{3}},
  };
  struct pfs_tree tree = {
    .header = {.type = PFS_BLOCK_TREE, .version = 1, .pool = {{1}}, .block = 9, .birth = 1},
    .kind = PFS_INDEX_DIRECTORY, .count = 1, .volume = {{2}}, .object = {{3}},
  };
  struct pfs_encoded_record record = {dirent, sizeof(dirent)};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(bytes, sizeof(bytes), &context, &tree, &record));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 9, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 2, 1, bytes, sizeof(bytes)));
  test_put_u64(bytes + 592, 5);
  test_put_u64(bytes + 600, 5);
  test_checksum(bytes + 200, 448, 8);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 2, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 4, 1, bytes, sizeof(bytes)));
  test_put_u64(bytes + 312, 5);
  test_put_u64(bytes + 384, 10);
  test_put_u64(bytes + 392, PFS_POOL_BLOCKS_MIN - 11);
  test_checksum(bytes + 288, 80, 8);
  test_checksum(bytes + 368, 80, 8);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 4, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 1, 1, bytes, sizeof(bytes)));
  test_put_u64(bytes + 216, 5);
  test_put_u64(bytes + 232, PFS_POOL_BLOCKS_MIN - 11);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 1, 1, bytes, sizeof(bytes)));
}

static void
named_or_orphan_exclusivity(void)
{
  build_orphan_fixture();
  make_named_file();
  struct pfs_check_result result;
  /* The file now has one correct namespace entry while its marker remains. */
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  uint8_t orphan[32] = {0};
  orphan[16] = 6;
  independent_record(orphan, sizeof(orphan), PFS_RECORD_ORPHAN);
  struct pfs_encoded_record record = {orphan, sizeof(orphan)};
  write_tree(7, PFS_INDEX_ORPHANS, &record, 1);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(1, result.state[0].directory_entries);
  TEST_ASSERT_EQUAL_UINT64(1, result.state[0].orphans);
  TEST_ASSERT_EQUAL_UINT64(1, result.state[1].directory_entries);
  TEST_ASSERT_EQUAL_UINT64(1, result.state[1].orphans);
  /* An orphan directory cannot own even a locally valid directory tree. */
  uint8_t bytes[PFS_BLOCK_SIZE];
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 5, 1, bytes, sizeof(bytes)));
  test_put_u16(bytes + 498, PFS_STORAGE_TREE);
  test_put_u64(bytes + 544, 9);
  test_put_u64(bytes + 552, 1);
  test_put_u16(bytes + 560, PFS_BLOCK_TREE);
  test_put_u16(bytes + 562, 1);
  test_put_u64(bytes + 568, 1);
  test_checksum(bytes + 464, 128, 8);
  test_checksum(bytes, sizeof(bytes), 20);
  struct pfs_object_record directory;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_object_record_decode(bytes + 464, 128, &volume_records, &directory));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&fixture.builder, 5, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_check(&fixture.builder.reader, &fixture.memory, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(0, fixture.memory.used);
}

static void
canonical_pool_blocks(void)
{
  build_orphan_fixture();
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock superblock;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 0, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_superblock_validate(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  bytes[132] = 1;
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_superblock_validate(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  bytes[132] = 0;
  test_put_u64(bytes + 160, 2);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_canonical_superblock_validate(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  test_put_u64(bytes + 160, 0);
  test_put_u64(bytes + 144, PFS_FEATURE_ORPHANS);
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, 0, &superblock));
  struct pfs_block_context context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .referring_birth = 1,
    .pool = {{1}}, .reference = {1, 1, PFS_BLOCK_POOL, 1},
  };
  struct pfs_pool_root root;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&fixture.builder.reader, 1, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_canonical_pool_root_validate(bytes, sizeof(bytes), &context, &root));
  bytes[148] = 1;
  test_checksum(bytes, sizeof(bytes), 20);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_decode(bytes, sizeof(bytes), &context, &root));
  TEST_ASSERT_EQUAL(PFS_CORRUPT, pfs_canonical_pool_root_validate(bytes, sizeof(bytes), &context, &root));
}

void
run_codec_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(standalone_block_matching_preserves_pool_feature_scope);
  RUN_TEST(orphan_known_vector_and_feature_scope);
  RUN_TEST(volume_root_known_vector);
  RUN_TEST(reserved_record_bytes_and_checkpoint);
  RUN_TEST(name_padding_and_extension_profile);
  RUN_TEST(canonical_orphan_tree_and_internal_keys);
  RUN_TEST(canonical_pool_blocks);
  RUN_TEST(retained_orphans_check_and_diagnostic_access);
  RUN_TEST(malformed_orphan_relations);
  RUN_TEST(named_or_orphan_exclusivity);
}
