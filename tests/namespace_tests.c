/* SPDX-License-Identifier: MPL-2.0 */
#include "failure.h"
#include "plan.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/write.h>
#include <string.h>

void run_namespace_tests(void);

#define NS_BYTES (3u * PFS_BLOCK_SIZE + 19u)
#define NS_VIEWS 12u

static struct test_fixture seed;
static struct test_failure device, snapshot;
static struct pfs_pool pool;
static struct pfs_volume volumes[2];
static struct pfs_view *views[NS_VIEWS];
static struct pfs_write_options options;
static struct pfs_trusted_context authority;
static uint64_t random_sequence;
static uint8_t original[NS_BYTES];
static const struct pfs_object_id root_id = {{3}}, file_id = {{4}};
static const struct pfs_rights file_rights = {
  .file = PFS_FILE_READ | PFS_FILE_METADATA | PFS_FILE_WRITE |
    PFS_FILE_RESIZE | PFS_FILE_CHECKPOINT,
  .admin = PFS_ADMIN_INSPECT,
};
static const struct pfs_rights parent_rights = {
  .file = PFS_FILE_RIGHTS_ALL,
  .directory = PFS_DIR_RIGHTS_ALL,
  .admin = PFS_ADMIN_RIGHTS_ALL,
};

static enum pfs_status
random_bytes(void *context, void *buffer, size_t length)
{
  uint64_t *sequence = context;
  uint8_t *bytes = buffer;
  ++*sequence;
  for (size_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)(*sequence + i * 17u);
  }
  return PFS_OK;
}

static enum pfs_status
source_read(void *context, size_t volume, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  if (volume || object != 1 || offset > sizeof(original) ||
      length > sizeof(original) - offset) {
    return PFS_INVALID;
  }
  memcpy(buffer, original + offset, length);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

/* Add explicit grants without changing block ownership. Seven file grants
 * require cleanup to retain the marker through more than one grant batch. */
static void
add_file_grants(const struct pfs_volume_record *volume)
{
  uint8_t block[PFS_BLOCK_SIZE], encoded[8][PFS_GRANT_RECORD_SIZE];
  struct pfs_encoded_record records[8];
  struct pfs_tree_context context = {
    .block = {.block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1,
      .referring_birth = 1, .pool = {{1}}, .reference = volume->grant_root},
    .kind = PFS_INDEX_GRANTS, .volume = {{2}},
  };
  struct pfs_record_context record_context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .containing_birth = 1,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader,
    volume->grant_root.block, 1, block, sizeof(block)));
  struct pfs_tree tree;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(block, sizeof(block), &context, &tree));
  TEST_ASSERT_EQUAL_UINT(1, tree.count);
  struct pfs_grant_record grant;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_decode(block + tree.slots[0].offset,
    tree.slots[0].length, &record_context, &grant));
  grant.directory_rights |= PFS_DIR_CHECKPOINT;
  for (size_t i = 0; i < 8; i++) {
    if (i) {
      grant = (struct pfs_grant_record){.object = {{4}},
        .principal = {{(uint8_t)(30 + i)}}, .scope = PFS_SCOPE_OBJECT,
        .file_rights = PFS_FILE_READ};
    }
    records[i].data = encoded[i];
    TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_encode(encoded[i], sizeof(encoded[i]),
      &record_context, &grant, &records[i].length));
  }
  tree.count = 8;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(block, sizeof(block), &context, &tree, records));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder,
    volume->grant_root.block, 1, block, sizeof(block)));
}

static void
build_seed(void)
{
  random_sequence = 0;
  for (size_t i = 0; i < sizeof(original); i++) {
    original[i] = (uint8_t)(i * 29u + 7u);
  }
  TEST_ASSERT_EQUAL(PFS_OK, test_fixture_open(&seed, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {4, "file"}, .kind = PFS_OBJECT_FILE,
      .file_length = sizeof(original)},
    {.id = {{6}}, .parent = 0, .name = {3, "src"}, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{7}}, .parent = 0, .name = {3, "dst"}, .kind = PFS_OBJECT_DIRECTORY},
  };
  struct pfs_build_object second_root = {
    .id = {{13}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY,
  };
  struct pfs_build_volume specifications[] = {
    {.id = {{2}}, .root_object = {{3}}, .name = {4, "home"}, .owner = {{5}},
      .quota_set = true, .quota = 1024, .guarantee_set = true,
      .object_count = 4, .objects = objects},
    {.id = {{12}}, .root_object = {{13}}, .name = {5, "other"}, .owner = {{5}},
      .quota_set = true, .quota = 1024, .guarantee_set = true,
      .object_count = 1, .objects = &second_root},
  };
  struct pfs_build_spec specification = {
    .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}}, .volume_count = 2,
    .volumes = specifications,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024,
  };
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_create(&seed.memory, &specification, &plan));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build(&plan, &seed.builder, &source));
  add_file_grants(&plan.volumes[0]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_build_plan_destroy(&plan));
}

static void
open_writer(void)
{
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  const struct pfs_volume_id ids[] = {{{2}}, {{12}}};
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &ids[i], &volumes[i]));
  }
  authority = (struct pfs_trusted_context){.principal = {{5}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE, .ceiling = {PFS_FILE_RIGHTS_ALL,
      PFS_DIR_RIGHTS_ALL, PFS_ADMIN_RIGHTS_ALL}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_SUBTREE, &parent_rights, &views[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &file_id,
    PFS_SCOPE_OBJECT, &file_rights, &views[1]));
}

static void
open_fixture(void)
{
  build_seed();
  memset(&pool, 0, sizeof(pool));
  memset(volumes, 0, sizeof(volumes));
  memset(views, 0, sizeof(views));
  options = (struct pfs_write_options){.extent_limit = 64, .metadata_limit = 64,
    .random = random_bytes, .random_context = &random_sequence};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 64, 64, 2, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  open_writer();
}

static void
close_view(size_t index)
{
  struct pfs_view_close_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&views[index], &result));
  TEST_ASSERT_TRUE(result.released);
  TEST_ASSERT_NULL(views[index]);
}

static void
close_handles(void)
{
  for (size_t i = 0; i < NS_VIEWS; i++) {
    if (views[i]) {
      close_view(i);
    }
  }
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volumes[i]));
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, device.memory->used);
}

static void
close_fixture(void)
{
  close_handles();
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
expect_complete(const struct pfs_write_result *result, bool namespace_confirmed)
{
  TEST_ASSERT_EQUAL(PFS_COMPLETE, result->completion);
  TEST_ASSERT_EQUAL(PFS_OK, result->operation_status);
  TEST_ASSERT_EQUAL(PFS_OK, result->maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result->health);
  TEST_ASSERT_EQUAL(namespace_confirmed, result->namespace_confirmed);
}

static void
expect_bytes(struct pfs_view *view, const uint8_t *expected, size_t length)
{
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(view, &metadata));
  TEST_ASSERT_EQUAL_UINT64(length, metadata.size);
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (size_t offset = 0; offset < length; offset += sizeof(bytes)) {
    size_t wanted = length - offset;
    if (wanted > sizeof(bytes)) {
      wanted = sizeof(bytes);
    }
    size_t count;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, offset, bytes, wanted, &count));
    TEST_ASSERT_EQUAL_UINT(wanted, count);
    TEST_ASSERT_EQUAL_MEMORY(expected + offset, bytes, wanted);
  }
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_read(view, length, bytes, 1, &count));
  TEST_ASSERT_EQUAL_UINT(0, count);
}

static struct pfs_view_identity
lookup(size_t parent, const char *name, size_t slot, enum pfs_grant_scope scope,
       const struct pfs_rights *rights)
{
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_lookup(views[parent], (const uint8_t *)name,
    strlen(name), scope, rights, &views[slot], &identity));
  return identity;
}

static void
expect_missing(size_t parent, const char *name)
{
  struct pfs_view_identity identity;
  const struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_view_lookup(views[parent],
    (const uint8_t *)name, strlen(name), PFS_SCOPE_OBJECT,
    &rights, &views[11], &identity));
  TEST_ASSERT_NULL(views[11]);
}

static void
expect_fresh_id_denied(const struct pfs_object_id *id)
{
  TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_view_acquire(&volumes[0], &authority, id,
    PFS_SCOPE_OBJECT, &file_rights, &views[11]));
  TEST_ASSERT_NULL(views[11]);
}

static struct pfs_directory_token
first_page(size_t index)
{
  struct pfs_directory_token start = {0}, next;
  struct pfs_view_entry entry;
  size_t count;
  bool done;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(views[index], &start,
    &entry, 1, &count, &done, &next));
  return next;
}

static void
expect_token(size_t index, const struct pfs_directory_token *token, enum pfs_status status)
{
  struct pfs_view_entry entry;
  memset(&entry, 0xa5, sizeof(entry));
  struct pfs_view_entry before = entry;
  struct pfs_directory_token next;
  memset(&next, 0xa5, sizeof(next));
  struct pfs_directory_token previous = next;
  size_t count = SIZE_MAX;
  bool done = false;
  TEST_ASSERT_EQUAL(status, pfs_view_directory_live_page(views[index], token,
    &entry, 1, &count, &done, &next));
  if (status != PFS_OK) {
    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_EQUAL_MEMORY(&before, &entry, sizeof(entry));
    TEST_ASSERT_EQUAL_MEMORY(&previous, &next, sizeof(next));
  }
}

static void
expect_empty(size_t index)
{
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(views[index], &metadata));
  TEST_ASSERT_EQUAL(PFS_OBJECT_DIRECTORY, metadata.identity.kind);
  TEST_ASSERT_EQUAL_UINT64(0, metadata.size);
  struct pfs_directory_token start = {0}, next;
  struct pfs_view_entry entry;
  size_t count;
  bool done;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_directory_live_page(views[index], &start,
    &entry, 1, &count, &done, &next));
  TEST_ASSERT_EQUAL_UINT(0, count);
  TEST_ASSERT_TRUE(done);
}

static void
check_durable(uint64_t orphans, uint64_t grants)
{
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &device, 0));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&snapshot.builder.reader, snapshot.memory,
    NULL, NULL, &check));
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_TRUE(check.state[i].complete);
    TEST_ASSERT_EQUAL_UINT64(orphans, check.state[i].orphans);
    TEST_ASSERT_EQUAL_UINT64(grants, check.state[i].grants);
  }
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
}

static void
directories_inherit_creation_policy_and_require_empty_removal(void)
{
  open_fixture();
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_directory(views[0],
    (const uint8_t *)"notes", 5, &parent_rights, &views[2], &identity, &result));
  expect_complete(&result, true);
  TEST_ASSERT_EQUAL(PFS_OBJECT_DIRECTORY, identity.kind);
  expect_empty(2);
  struct pfs_principal_id owner;
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_inspect(views[2], &owner, NULL, 0, &count));
  const struct pfs_principal_id expected_owner = {{5}};
  TEST_ASSERT_EQUAL_MEMORY(&expected_owner, &owner, sizeof(owner));
  TEST_ASSERT_EQUAL_UINT(0, count);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[2], (const uint8_t *)"child", 5,
    NULL, NULL, NULL, &result));
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_NOT_EMPTY, pfs_view_remove(views[0],
    (const uint8_t *)"notes", 5, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_view_rename(views[0],
    (const uint8_t *)"notes", 5, views[0], (const uint8_t *)"moved", 5, false, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[2], (const uint8_t *)"child", 5, &result));
  expect_complete(&result, true);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"notes", 5, &result));
  expect_complete(&result, true);
  expect_missing(0, "notes");
  expect_empty(2);
  close_fixture();
}

static void
unlinked_files_keep_identity_bytes_and_independent_held_rights(void)
{
  open_fixture();
  const struct pfs_rights readonly = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  lookup(0, "file", 2, PFS_SCOPE_OBJECT, &readonly);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_complete(&result, true);
  expect_missing(0, "file");
  expect_fresh_id_denied(&file_id);
  expect_bytes(views[2], original, sizeof(original));
  struct pfs_view_identity replacement;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[0], (const uint8_t *)"file", 4,
    &file_rights, &views[3], &replacement, &result));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(&file_id, &replacement.object, sizeof(file_id)));
  static const uint8_t old_bytes[] = "old identity remains writable";
  static const uint8_t new_bytes[] = "new identity has separate contents";
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(views[1], 0, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[1], 0, old_bytes, sizeof(old_bytes), &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[3], 0, new_bytes, sizeof(new_bytes), &result));
  expect_bytes(views[2], old_bytes, sizeof(old_bytes));
  expect_bytes(views[3], new_bytes, sizeof(new_bytes));
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(views[2], &metadata));
  TEST_ASSERT_EQUAL_MEMORY(&file_id, &metadata.identity.object, sizeof(file_id));
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_write(views[2], 0, "X", 1, &result));
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_resize(views[2], 0, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  close_view(1);
  expect_bytes(views[2], old_bytes, sizeof(old_bytes));
  check_durable(1, 9);
  close_view(2);
  check_durable(0, 2);
  close_handles();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &device.builder.reader,
    device.memory, &diagnostic));
  const struct pfs_volume_id volume_id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volumes[0]));
  const struct pfs_rights read_parent = {
    .file = PFS_FILE_READ | PFS_FILE_METADATA,
    .directory = PFS_DIR_LOOKUP,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_SUBTREE, &read_parent, &views[0]));
  const struct pfs_rights read_rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  struct pfs_view_identity found = lookup(0, "file", 3, PFS_SCOPE_OBJECT, &read_rights);
  TEST_ASSERT_EQUAL_MEMORY(&replacement.object, &found.object, sizeof(found.object));
  expect_bytes(views[3], new_bytes, sizeof(new_bytes));
  expect_fresh_id_denied(&file_id);
  close_fixture();
}

static void
detached_directories_keep_empty_identity_and_deny_insertion(void)
{
  open_fixture();
  struct pfs_view_identity old = lookup(0, "src", 2, PFS_SCOPE_SUBTREE, &parent_rights);
  struct pfs_directory_token token = first_page(2);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"src", 3, &result));
  expect_token(2, &token, PFS_CHANGED);
  expect_empty(2);
  struct pfs_view_identity replacement;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_directory(views[0],
    (const uint8_t *)"src", 3, &parent_rights, &views[3], &replacement, &result));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(&old.object, &replacement.object, sizeof(old.object)));
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_DETACHED, pfs_view_create_file(views[2],
    (const uint8_t *)"child", 5, NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL(PFS_DETACHED, pfs_view_create_directory(views[2],
    (const uint8_t *)"child", 5, NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL(PFS_DETACHED, pfs_view_rename(views[0],
    (const uint8_t *)"file", 4, views[2], (const uint8_t *)"child", 5, false, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(views[2], &result));
  expect_complete(&result, false);
  struct pfs_view_metadata metadata;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_metadata(views[2], &metadata));
  TEST_ASSERT_EQUAL_MEMORY(&old.object, &metadata.identity.object, sizeof(old.object));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[3],
    (const uint8_t *)"child", 5, NULL, NULL, NULL, &result));
  expect_empty(2);
  close_fixture();
}

static void
rename_moves_identity_and_replacement_preserves_displaced_file(void)
{
  open_fixture();
  lookup(0, "src", 2, PFS_SCOPE_SUBTREE, &parent_rights);
  lookup(0, "dst", 3, PFS_SCOPE_SUBTREE, &parent_rights);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[0], (const uint8_t *)"file", 4,
    views[2], (const uint8_t *)"source", 6, false, &result));
  expect_complete(&result, true);
  expect_missing(0, "file");
  struct pfs_view_identity moved = lookup(2, "source", 4, PFS_SCOPE_OBJECT, &file_rights);
  TEST_ASSERT_EQUAL_MEMORY(&file_id, &moved.object, sizeof(file_id));
  struct pfs_view_identity victim;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[3], (const uint8_t *)"target", 6,
    &file_rights, &views[5], &victim, &result));
  static const uint8_t displaced[] = "displaced destination bytes";
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[5], 0, displaced, sizeof(displaced), &result));
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_EXISTS, pfs_view_rename(views[2], (const uint8_t *)"source", 6,
    views[3], (const uint8_t *)"target", 6, false, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[2], (const uint8_t *)"source", 6,
    views[3], (const uint8_t *)"target", 6, true, &result));
  expect_complete(&result, true);
  expect_missing(2, "source");
  struct pfs_view_identity target = lookup(3, "target", 6, PFS_SCOPE_OBJECT, &file_rights);
  TEST_ASSERT_EQUAL_MEMORY(&file_id, &target.object, sizeof(file_id));
  expect_bytes(views[6], original, sizeof(original));
  expect_bytes(views[5], displaced, sizeof(displaced));
  expect_fresh_id_denied(&victim.object);
  static const uint8_t retained[] = "retained destination can still write";
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(views[5], 0, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[5], 0, retained, sizeof(retained), &result));
  expect_bytes(views[5], retained, sizeof(retained));
  expect_bytes(views[1], original, sizeof(original));
  close_view(5);
  check_durable(0, 9);
  close_fixture();
}

static void
namespace_authority_precedes_names_and_noops_do_not_publish(void)
{
  open_fixture();
  const struct pfs_rights remove_only = {.directory = PFS_DIR_REMOVE};
  const struct pfs_rights create_only = {.directory = PFS_DIR_CREATE};
  const struct pfs_rights both = {.directory = PFS_DIR_REMOVE | PFS_DIR_CREATE};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_OBJECT, &remove_only, &views[2]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_OBJECT, &create_only, &views[3]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_OBJECT, &both, &views[4]));
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_remove(views[3], (const uint8_t *)"absent", 6, &result));
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_rename(views[3], (const uint8_t *)"absent", 6,
    views[2], (const uint8_t *)"file", 4, false, &result));
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_rename(views[2], (const uint8_t *)"file", 4,
    views[2], (const uint8_t *)"file", 4, true, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[4], (const uint8_t *)"file", 4,
    views[4], (const uint8_t *)"file", 4, true, &result));
  expect_complete(&result, false);
  TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_view_rename(views[4], (const uint8_t *)"absent", 6,
    views[4], (const uint8_t *)"absent", 6, false, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[3], (const uint8_t *)"target", 6,
    NULL, NULL, NULL, &result));
  writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_EXISTS, pfs_view_rename(views[2], (const uint8_t *)"file", 4,
    views[3], (const uint8_t *)"target", 6, false, &result));
  TEST_ASSERT_EQUAL(PFS_DENIED, pfs_view_rename(views[2], (const uint8_t *)"file", 4,
    views[3], (const uint8_t *)"target", 6, true, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[2], (const uint8_t *)"file", 4,
    views[3], (const uint8_t *)"new", 3, true, &result));
  expect_complete(&result, true);
  expect_bytes(views[1], original, sizeof(original));
  close_fixture();
}

static void
rename_refuses_volume_crossing_and_directory_replacement(void)
{
  open_fixture();
  struct pfs_trusted_context second = authority;
  second.root = (struct pfs_object_id){{13}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[1], &second, &second.root,
    PFS_SCOPE_OBJECT, &parent_rights, &views[2]));
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_view_rename(views[0], (const uint8_t *)"file", 4,
    views[2], (const uint8_t *)"file", 4, false, &result));
  TEST_ASSERT_EQUAL(PFS_UNSUPPORTED, pfs_view_rename(views[0], (const uint8_t *)"file", 4,
    views[0], (const uint8_t *)"src", 3, true, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  expect_bytes(views[1], original, sizeof(original));
  expect_empty(2);
  close_fixture();
}

static void
live_tokens_invalidate_only_directories_whose_entries_change(void)
{
  open_fixture();
  lookup(0, "src", 2, PFS_SCOPE_SUBTREE, &parent_rights);
  lookup(0, "dst", 3, PFS_SCOPE_SUBTREE, &parent_rights);
  struct pfs_directory_token root = first_page(0), source = first_page(2), destination = first_page(3);
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[2], (const uint8_t *)"child", 5,
    NULL, NULL, NULL, &result));
  expect_token(0, &root, PFS_OK);
  expect_token(2, &source, PFS_CHANGED);
  expect_token(3, &destination, PFS_OK);
  source = first_page(2);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[2], (const uint8_t *)"child", 5,
    views[3], (const uint8_t *)"child", 5, false, &result));
  expect_token(0, &root, PFS_OK);
  expect_token(2, &source, PFS_CHANGED);
  expect_token(3, &destination, PFS_CHANGED);
  destination = first_page(3);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[2], (const uint8_t *)"child", 5,
    NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[2], (const uint8_t *)"child", 5,
    views[3], (const uint8_t *)"child", 5, true, &result));
  expect_token(3, &destination, PFS_CHANGED);
  destination = first_page(3);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[3], (const uint8_t *)"child", 5, &result));
  expect_token(3, &destination, PFS_CHANGED);
  expect_token(0, &root, PFS_OK);
  close_fixture();
}

static void
final_orphan_release_drains_data_and_grants_without_more_memory(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  check_durable(1, 9);
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  test_failure_memory_fail_after(&device, 0);
  struct pfs_view_close_result closing;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&views[1], &closing));
  TEST_ASSERT_TRUE(closing.released);
  TEST_ASSERT_NULL(views[1]);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, closing.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_OK, closing.maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, closing.health);
  TEST_ASSERT_GREATER_THAN_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  device.backing.fail_after = SIZE_MAX;
  check_durable(0, 2);
  close_fixture();
}

static void
failed_final_release_consumes_handle_and_startup_drains_abandoned_orphan(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  struct test_failure_fault fault = {.kind = TEST_FAILURE_WRITE,
    .ordinal = device.ordinals[TEST_FAILURE_WRITE] + 1,
    .mode = TEST_FAILURE_BEFORE, .enabled = true};
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
  struct pfs_view_close_result closing;
  TEST_ASSERT_EQUAL(PFS_IO, pfs_view_close(&views[1], &closing));
  TEST_ASSERT_TRUE(device.triggered);
  TEST_ASSERT_TRUE(closing.released);
  TEST_ASSERT_NULL(views[1]);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_STOPPED, closing.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_IO, closing.maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READABLE_STOPPED, closing.health);
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_create_file(views[0],
    (const uint8_t *)"next", 4, NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  close_handles();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
  check_durable(1, 9);
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
  TEST_ASSERT_EQUAL(PFS_OK, opening.recovery.maintenance_status);
  TEST_ASSERT_GREATER_THAN_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  check_durable(0, 2);
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_SUBTREE, &parent_rights, &views[0]));
  expect_missing(0, "file");
  expect_fresh_id_denied(&file_id);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[0], (const uint8_t *)"file", 4,
    NULL, NULL, NULL, &result));
  expect_complete(&result, true);
  close_fixture();
}

static void
durable_restart_loses_runtime_retention_and_cleans_validated_orphans(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_bytes(views[1], original, sizeof(original));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &device, 0));
  close_handles();

  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &snapshot.builder.reader,
    snapshot.memory, &diagnostic));
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &id, &volumes[0]));
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_object(&volumes[0], &file_id, &object));
  TEST_ASSERT_EQUAL_UINT64(sizeof(original), object.file_length);
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (size_t offset = 0; offset < sizeof(original); offset += sizeof(bytes)) {
    size_t wanted = sizeof(original) - offset;
    if (wanted > sizeof(bytes)) {
      wanted = sizeof(bytes);
    }
    size_t count;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&volumes[0], &file_id,
      offset, bytes, wanted, &count));
    TEST_ASSERT_EQUAL_UINT(wanted, count);
    TEST_ASSERT_EQUAL_MEMORY(original + offset, bytes, wanted);
  }
  size_t grants;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_grants(&volumes[0], &file_id,
    NULL, 0, &grants));
  TEST_ASSERT_EQUAL_UINT(7, grants);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, snapshot.ordinals[TEST_FAILURE_WRITE]);

  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
    snapshot.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
  TEST_ASSERT_GREATER_THAN_UINT64(0, snapshot.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_volume_open(&pool, &id, &volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[0], &authority, &root_id,
    PFS_SCOPE_SUBTREE, &parent_rights, &views[0]));
  expect_missing(0, "file");
  expect_fresh_id_denied(&file_id);
  close_view(0);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&snapshot.builder.reader, snapshot.memory,
    NULL, NULL, &check));
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_EQUAL_UINT64(0, check.state[i].orphans);
    TEST_ASSERT_EQUAL_UINT64(2, check.state[i].grants);
  }
  TEST_ASSERT_FALSE(snapshot.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

void
run_namespace_tests(void)
{
  RUN_TEST(directories_inherit_creation_policy_and_require_empty_removal);
  RUN_TEST(unlinked_files_keep_identity_bytes_and_independent_held_rights);
  RUN_TEST(detached_directories_keep_empty_identity_and_deny_insertion);
  RUN_TEST(rename_moves_identity_and_replacement_preserves_displaced_file);
  RUN_TEST(namespace_authority_precedes_names_and_noops_do_not_publish);
  RUN_TEST(rename_refuses_volume_crossing_and_directory_replacement);
  RUN_TEST(live_tokens_invalidate_only_directories_whose_entries_change);
  RUN_TEST(final_orphan_release_drains_data_and_grants_without_more_memory);
  RUN_TEST(failed_final_release_consumes_handle_and_startup_drains_abandoned_orphan);
  RUN_TEST(durable_restart_loses_runtime_retention_and_cleans_validated_orphans);
}
