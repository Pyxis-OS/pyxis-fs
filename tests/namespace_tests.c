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
#define NS_VOLUMES 2u
#define NS_FILE_GRANTS 7u
#define NS_GRANTS (NS_VOLUMES + NS_FILE_GRANTS)
#define NS_ORPHAN_LEAF_RECORDS ((PFS_BLOCK_SIZE - PFS_TREE_HEADER_SIZE) / \
  (PFS_ORPHAN_RECORD_SIZE + PFS_TREE_SLOT_SIZE))
#define NS_RETAINED_VICTIMS (NS_ORPHAN_LEAF_RECORDS + 12u)
#define NS_VIEWS (NS_RETAINED_VICTIMS + 12u)

static struct test_fixture seed;
static struct test_failure device, snapshot;
static struct pfs_pool pool;
static struct pfs_volume volumes[NS_VOLUMES];
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
  .admin = PFS_ADMIN_INSPECT,
};
static const struct pfs_rights directory_rights = {
  .directory = PFS_DIR_RIGHTS_ALL,
  .admin = PFS_ADMIN_INSPECT,
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

/* Add explicit file grants alongside the first volume's owner grant. */
static void
add_file_grants(const struct pfs_volume_record *volume)
{
  uint8_t block[PFS_BLOCK_SIZE], encoded[NS_FILE_GRANTS + 1][PFS_GRANT_RECORD_SIZE];
  struct pfs_encoded_record records[NS_FILE_GRANTS + 1];
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
  for (size_t i = 0; i < NS_FILE_GRANTS + 1; i++) {
    if (i) {
      grant = (struct pfs_grant_record){.object = {{4}},
        .principal = {{(uint8_t)(30 + i)}}, .scope = PFS_SCOPE_OBJECT,
        .file_rights = PFS_FILE_READ};
    }
    records[i].data = encoded[i];
    TEST_ASSERT_EQUAL(PFS_OK, pfs_grant_record_encode(encoded[i], sizeof(encoded[i]),
      &record_context, &grant, &records[i].length));
  }
  tree.count = NS_FILE_GRANTS + 1;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_encode(block, sizeof(block), &context, &tree, records));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder,
    volume->grant_root.block, 1, block, sizeof(block)));
}

static void
build_seed(uint64_t quota, uint64_t cow, uint64_t recovery)
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
      .quota_set = true, .quota = quota, .guarantee_set = true,
      .object_count = 4, .objects = objects},
    {.id = {{12}}, .root_object = {{13}}, .name = {5, "other"}, .owner = {{5}},
      .quota_set = true, .quota = 1024, .guarantee_set = true,
      .object_count = 1, .objects = &second_root},
  };
  struct pfs_build_spec specification = {
    .block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}}, .volume_count = NS_VOLUMES,
    .volumes = specifications,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = cow, .migration_reserve = 1024, .recovery_reserve = recovery,
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
open_seed(uint64_t extents, uint64_t metadata)
{
  memset(&pool, 0, sizeof(pool));
  memset(volumes, 0, sizeof(volumes));
  memset(views, 0, sizeof(views));
  options = (struct pfs_write_options){.extent_limit = extents, .metadata_limit = metadata,
    .random = random_bytes, .random_context = &random_sequence};
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, extents, metadata, 2, &limits));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &seed.builder.reader));
  open_writer();
}

static void
open_fixture_config(uint64_t quota, uint64_t extents, uint64_t metadata,
                    uint64_t cow, uint64_t recovery)
{
  build_seed(quota, cow, recovery);
  open_seed(extents, metadata);
}

static void
open_fixture(void)
{
  open_fixture_config(1024, 64, 64, 1024, 1024);
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
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
  if (result->maintenance_completion == PFS_MAINTENANCE_PENDING) {
    TEST_ASSERT_TRUE(status.drain_pending);
  }
  TEST_ASSERT_FALSE(status.invariant_failure);
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
  }
  /* Logical mutation expectations describe the selected durable state; the
   * older retained state may still protect the preceding namespace/cleanup. */
  unsigned selected = check.state[1].generation > check.state[0].generation;
  TEST_ASSERT_EQUAL_UINT64(orphans, check.state[selected].orphans);
  TEST_ASSERT_EQUAL_UINT64(grants, check.state[selected].grants);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
}

static bool publication_observed;
static size_t publication_allocations;

static void
observe_first_publication(struct test_failure *adapter,
  const struct test_failure_event *event, bool before, void *context)
{
  (void)context;
  if (before && event->kind == TEST_FAILURE_WRITE && !publication_observed) {
    publication_observed = true;
    publication_allocations = adapter->backing.allocation_calls;
  }
}

static void
observe_funded_operation(void)
{
  publication_observed = false;
  device.observer = observe_first_publication;
}

static void
expect_funded_operation(void)
{
  device.observer = NULL;
  TEST_ASSERT_TRUE(publication_observed);
  TEST_ASSERT_EQUAL_UINT(publication_allocations, device.backing.allocation_calls);
}

static void
directories_inherit_creation_policy_and_require_empty_removal(void)
{
  open_fixture();
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_directory(views[0],
    (const uint8_t *)"notes", 5, &directory_rights, &views[2], &identity, &result));
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
  check_durable(1, NS_GRANTS);
  close_view(2);
  check_durable(0, NS_VOLUMES);
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
    (const uint8_t *)"src", 3, &directory_rights, &views[3], &replacement, &result));
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
  check_durable(0, NS_GRANTS);
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
  const struct pfs_rights rights = {
    .directory = PFS_DIR_CREATE | PFS_DIR_METADATA | PFS_DIR_LIST,
  };
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_acquire(&volumes[1], &second, &second.root,
    PFS_SCOPE_OBJECT, &rights, &views[2]));
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
final_orphan_release_finishes_deletion_and_fences_without_more_memory(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  check_durable(1, NS_GRANTS);
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  test_failure_memory_fail_after(&device, 0);
  struct pfs_view_close_result closing;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&views[1], &closing));
  TEST_ASSERT_TRUE(closing.released);
  TEST_ASSERT_NULL(views[1]);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, closing.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_OK, closing.maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, closing.health);
  TEST_ASSERT_GREATER_THAN_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  check_durable(0, NS_VOLUMES);
  size_t allocations = device.backing.allocation_calls;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(views[0], &result));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
  TEST_ASSERT_EQUAL_UINT64(allocations, device.backing.allocation_calls);
  struct pfs_writer_status health;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &health));
  TEST_ASSERT_FALSE(health.drain_pending);
  device.backing.fail_after = SIZE_MAX;
  check_durable(0, NS_VOLUMES);
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
  check_durable(1, NS_GRANTS);
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
  TEST_ASSERT_EQUAL(PFS_OK, opening.recovery.maintenance_status);
  TEST_ASSERT_GREATER_THAN_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  check_durable(0, NS_VOLUMES);
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
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 7, 2, &limits));
  uint64_t recovery = limits.recovery_blocks > 256 ? limits.recovery_blocks : 256;
  open_fixture_config(9, 1, 7, 1024, recovery);
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
  TEST_ASSERT_EQUAL_UINT(NS_FILE_GRANTS, grants);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  TEST_ASSERT_EQUAL_UINT64(0, snapshot.ordinals[TEST_FAILURE_WRITE]);

  struct pfs_write_open_result opening;
  publication_observed = false;
  snapshot.observer = observe_first_publication;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
    snapshot.memory, &options, &opening));
  snapshot.observer = NULL;
  TEST_ASSERT_TRUE(publication_observed);
  TEST_ASSERT_EQUAL_UINT(publication_allocations, snapshot.backing.allocation_calls);
  TEST_ASSERT_EQUAL_UINT64(limits.recovery_blocks, opening.required_recovery_blocks);
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
    TEST_ASSERT_EQUAL_UINT64(NS_VOLUMES, check.state[i].grants);
  }
  TEST_ASSERT_FALSE(snapshot.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
minimum_quota_profile_and_computed_recovery_fund_final_release(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 1, 7, 2, &limits));
  uint64_t recovery = limits.recovery_blocks > 256 ? limits.recovery_blocks : 256;
  /* The first volume has four data blocks and five promised metadata blocks;
   * the empty second volume needs two metadata blocks. */
  open_fixture_config(9, 1, 7, 1024, recovery);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_QUOTA, pfs_view_create_file(views[0],
    (const uint8_t *)"growth", 6, NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  observe_funded_operation();
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_funded_operation();
  expect_complete(&result, true);
  expect_bytes(views[1], original, sizeof(original));
  test_failure_memory_fail_after(&device, 0);
  close_view(1);
  device.backing.fail_after = SIZE_MAX;
  check_durable(0, NS_VOLUMES);
  close_fixture();
}

static void
pool_promise_boundary_protects_unlink_and_final_cleanup(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 64, 64, 2, &limits));
  uint64_t cow = PFS_POOL_BLOCKS_MIN - 2 - 1024 - 1024 -
    limits.permanent_pool - 11;
  open_fixture_config(1024, 64, 64, cow, 1024);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_NO_SPACE, pfs_view_create_file(views[0],
    (const uint8_t *)"growth", 6, NULL, NULL, NULL, &result));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  observe_funded_operation();
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_funded_operation();
  expect_complete(&result, true);
  test_failure_memory_fail_after(&device, 0);
  close_view(1);
  device.backing.fail_after = SIZE_MAX;
  check_durable(0, NS_VOLUMES);
  close_fixture();
}

static uint8_t pressure_names[NS_RETAINED_VICTIMS][PFS_NAME_MAX];
static uint8_t pressure_bytes[64u * PFS_BLOCK_SIZE];

static void
quota_filled_namespace_preserves_multiple_orphan_nodes_and_funded_release(void)
{
  open_fixture_config(48, 64, 64, 1024, 1024);
  for (size_t i = 0; i < NS_RETAINED_VICTIMS; i++) {
    memset(pressure_names[i], 'n', sizeof(pressure_names[i]));
    pressure_names[i][0] = (uint8_t)('0' + i / 100);
    pressure_names[i][1] = (uint8_t)('0' + i / 10 % 10);
    pressure_names[i][2] = (uint8_t)('0' + i % 10);
    struct pfs_write_result result;
    struct pfs_view_identity identity;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[0], pressure_names[i],
      sizeof(pressure_names[i]), &file_rights, &views[12 + i], &identity, &result));
    expect_complete(&result, true);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    test_failure_trace_reset(&device);
  }
  memcpy(pressure_bytes, original, sizeof(original));
  memset(pressure_bytes + sizeof(original), 'Q', sizeof(pressure_bytes) - sizeof(original));
  size_t length = sizeof(original);
  bool refused = false;
  while (length + PFS_BLOCK_SIZE <= sizeof(pressure_bytes)) {
    struct pfs_write_result result;
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    enum pfs_status status = pfs_view_write(views[1], length,
      pressure_bytes + length, PFS_BLOCK_SIZE, &result);
    if (status == PFS_QUOTA) {
      TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
      TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
      refused = true;
      break;
    }
    TEST_ASSERT_EQUAL(PFS_OK, status);
    expect_complete(&result, false);
    length += PFS_BLOCK_SIZE;
    test_failure_trace_reset(&device);
  }
  TEST_ASSERT_TRUE(refused);
  expect_bytes(views[1], pressure_bytes, length);
  for (size_t i = 0; i < NS_RETAINED_VICTIMS; i++) {
    struct pfs_write_result result;
    observe_funded_operation();
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], pressure_names[i],
      sizeof(pressure_names[i]), &result));
    expect_funded_operation();
    expect_complete(&result, true);
    expect_bytes(views[12 + i], original, 0);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    test_failure_trace_reset(&device);
  }
  device.backing.fail_after = SIZE_MAX;
  check_durable(NS_RETAINED_VICTIMS, NS_GRANTS);
  test_failure_memory_fail_after(&device, 0);
  for (size_t i = 0; i < NS_RETAINED_VICTIMS; i++) {
    close_view(12 + i);
    TEST_ASSERT_FALSE(device.infrastructure_failure);
    test_failure_trace_reset(&device);
  }
  device.backing.fail_after = SIZE_MAX;
  check_durable(0, NS_GRANTS);
  expect_bytes(views[1], pressure_bytes, length);
  close_fixture();
}

static struct test_failure_flush_cut cleanup_cuts[TEST_FAILURE_EVENTS_MAX];
static uint64_t cleanup_generations[TEST_FAILURE_EVENTS_MAX];
struct cleanup_progress {
  uint64_t orphans;
  uint64_t file_blocks;
  uint64_t grants;
};
static struct cleanup_progress cleanup_before[TEST_FAILURE_EVENTS_MAX];
static struct cleanup_progress cleanup_after[TEST_FAILURE_EVENTS_MAX];
static uint64_t cleanup_flush_base;
static enum pfs_status cleanup_trace_status;

static enum pfs_status
cleanup_durable_read(void *context, uint64_t first, uint32_t count, void *buffer)
{
  return test_failure_durable_read(context, first, count, buffer);
}

/* Discover logical cleanup progress from durable media on a separate reader.
 * Checking uses the seed's idle memory owner, never writer allocations/handles.
 * Independent payload comparisons remain the correctness oracle after each cut. */
static void
observe_cleanup_generation(struct test_failure *adapter,
  const struct test_failure_event *event, bool before, void *context)
{
  (void)context;
  if (event->kind != TEST_FAILURE_FLUSH || cleanup_trace_status != PFS_OK) {
    return;
  }
  uint64_t index = event->ordinal - cleanup_flush_base - 1;
  if (index >= TEST_FAILURE_EVENTS_MAX) {
    cleanup_trace_status = PFS_LIMIT;
    return;
  }
  struct pfs_block_reader reader = {
    .context = adapter, .geometry = adapter->builder.reader.geometry,
    .read = cleanup_durable_read,
  };
  struct pfs_check_result check;
  cleanup_trace_status = pfs_check(&reader, &seed.memory, NULL, NULL, &check);
  if (cleanup_trace_status != PFS_OK) {
    return;
  }
  unsigned selected = check.state[1].generation > check.state[0].generation;
  const struct pfs_check_state_result *state = &check.state[selected];
  struct cleanup_progress progress = {state->orphans, state->file_blocks, state->grants};
  if (before) {
    cleanup_generations[index] = state->generation;
    cleanup_before[index] = progress;
  } else {
    cleanup_after[index] = progress;
  }
}

static enum pfs_completion
startup_cut_completion(const struct test_failure_flush_cut *cut)
{
  const struct cleanup_progress *before = &cleanup_before[cut->ordinal - 1];
  const struct cleanup_progress *after = &cleanup_after[cut->ordinal - 1];
  if (!before->orphans) {
    /* Final paired deletion was confirmed; only its retained-root drain fails. */
    return PFS_COMPLETE;
  }
  bool progress = before->orphans != after->orphans ||
    before->file_blocks != after->file_blocks || before->grants != after->grants;
  /* An uncertain cleanup publication can change remaining work. A failed drain
   * after a partial cleanup leaves the overall cleanup stopped/incomplete. */
  return cut->publishes && progress ? PFS_UNKNOWN : PFS_STOPPED;
}

static void
start_cleanup_trace(struct test_failure *adapter)
{
  test_failure_trace_reset(adapter);
  cleanup_flush_base = adapter->ordinals[TEST_FAILURE_FLUSH];
  cleanup_trace_status = PFS_OK;
  adapter->observer = observe_cleanup_generation;
}

static size_t
finish_cleanup_trace(struct test_failure *adapter)
{
  adapter->observer = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, cleanup_trace_status);
  struct pfs_writer_status status;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_writer_status(&pool, &status));
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, status.health);
  TEST_ASSERT_FALSE(status.invariant_failure);
  size_t count;
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_flush_cuts(adapter, cleanup_flush_base,
    cleanup_cuts, TEST_FAILURE_EVENTS_MAX, &count));
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  return count;
}

static void
expect_interrupted_orphan(struct test_failure *adapter, uint64_t generation)
{
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&adapter->builder.reader, adapter->memory,
    NULL, NULL, &check));
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_TRUE(check.state[i].complete);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(1, check.state[i].orphans);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(NS_GRANTS, check.state[i].grants);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(NS_VOLUMES, check.state[i].grants);
  }
  struct pfs_pool_diagnostic diagnostic;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open(&pool, &adapter->builder.reader,
    adapter->memory, &diagnostic));
  const struct pfs_check_state_result *selected = &check.state[diagnostic.selected];
  TEST_ASSERT_EQUAL_UINT64(generation, selected->generation);
  const struct pfs_volume_id id = {{2}};
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &id, &volumes[0]));
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_NOT_FOUND, pfs_volume_diagnostic_resolve(&volumes[0],
    &root_id, (const uint8_t *)"file", 4, &object));
  TEST_ASSERT_EQUAL(selected->orphans ? PFS_OK : PFS_NOT_FOUND,
    pfs_volume_diagnostic_object(&volumes[0], &file_id, &object));
  if (selected->orphans) {
    TEST_ASSERT_EQUAL_MEMORY(&file_id, &object.id, sizeof(file_id));
    const struct pfs_object_id no_parent = {{0}};
    TEST_ASSERT_EQUAL_MEMORY(&no_parent, &object.parent, sizeof(no_parent));
    TEST_ASSERT_EQUAL(PFS_OBJECT_FILE, object.kind);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(sizeof(original), object.file_length);
    uint8_t bytes[PFS_BLOCK_SIZE];
    size_t count;
    for (uint64_t offset = 0; offset < object.file_length; offset += sizeof(bytes)) {
      size_t wanted = object.file_length - offset;
      if (wanted > sizeof(bytes)) {
        wanted = sizeof(bytes);
      }
      TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&volumes[0], &file_id,
        offset, bytes, wanted, &count));
      TEST_ASSERT_EQUAL_UINT(wanted, count);
      TEST_ASSERT_EQUAL_MEMORY(original + offset, bytes, wanted);
    }
    uint8_t byte = 0xa5;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_read(&volumes[0], &file_id,
      object.file_length, &byte, 1, &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_EQUAL_UINT8(0xa5, byte);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_diagnostic_grants(&volumes[0], &file_id,
      NULL, 0, &count));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(NS_FILE_GRANTS, count);
    TEST_ASSERT_EQUAL_UINT64(NS_VOLUMES + count, selected->grants);
  } else {
    TEST_ASSERT_EQUAL_UINT64(NS_VOLUMES, selected->grants);
    TEST_ASSERT_EQUAL_UINT64(0, selected->file_blocks);
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_volume_close(&volumes[0]));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
}

static void
expect_cleaned_orphans(struct test_failure *adapter)
{
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&adapter->builder.reader, adapter->memory,
    NULL, NULL, &check));
  for (size_t i = 0; i < 2; i++) {
    TEST_ASSERT_TRUE(check.state[i].complete);
    TEST_ASSERT_EQUAL_UINT64(0, check.state[i].orphans);
    TEST_ASSERT_EQUAL_UINT64(NS_VOLUMES, check.state[i].grants);
    TEST_ASSERT_EQUAL_UINT64(0, check.state[i].file_blocks);
  }
}

static void
create_abandoned_snapshot(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_complete(&result, true);
  expect_bytes(views[1], original, sizeof(original));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &device, 0));
  close_handles();
  test_failure_trace_reset(&snapshot);
}

static void
close_snapshot_fixture(void)
{
  TEST_ASSERT_FALSE(snapshot.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&snapshot));
  TEST_ASSERT_FALSE(device.infrastructure_failure);
  TEST_ASSERT_TRUE(test_failure_close(&device));
  TEST_ASSERT_TRUE(test_fixture_close(&seed));
}

static void
final_close_maintenance_failures_consume_view_and_resume_paired_orphan(void)
{
  open_fixture();
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  start_cleanup_trace(&device);
  close_view(1);
  size_t cut_count = finish_cleanup_trace(&device);
  close_fixture();

  for (size_t i = 0; i < cut_count; i++) {
    open_fixture();
    TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
    const struct test_failure_flush_cut *cut = &cleanup_cuts[i];
    uint64_t generation = cleanup_generations[cut->ordinal - 1];
    struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
      .ordinal = device.ordinals[TEST_FAILURE_FLUSH] + cut->ordinal,
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    test_failure_memory_fail_after(&device, 0);
    struct pfs_view_close_result closing;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_close(&views[1], &closing));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_TRUE(closing.released);
    TEST_ASSERT_NULL(views[1]);
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
      closing.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_IO, closing.maintenance_status);
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
      closing.health);
    device.backing.fail_after = SIZE_MAX;
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(views[0], &result));
    close_handles();
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
    expect_interrupted_orphan(&device, generation);
    struct pfs_write_open_result opening;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
      device.memory, &options, &opening));
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
    TEST_ASSERT_EQUAL(PFS_OK, opening.recovery.maintenance_status);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
    expect_cleaned_orphans(&device);
    close_fixture();
  }
}

static void
interrupted_startup_preserves_confirmed_generation_and_withholds_instance(void)
{
  create_abandoned_snapshot();
  start_cleanup_trace(&snapshot);
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
    snapshot.memory, &options, &opening));
  size_t cut_count = finish_cleanup_trace(&snapshot);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  close_snapshot_fixture();

  for (size_t i = 0; i < cut_count; i++) {
    create_abandoned_snapshot();
    const struct test_failure_flush_cut *cut = &cleanup_cuts[i];
    uint64_t generation = cleanup_generations[cut->ordinal - 1];
    struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
      .ordinal = snapshot.ordinals[TEST_FAILURE_FLUSH] + cut->ordinal,
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&snapshot, &fault));
    publication_observed = false;
    snapshot.observer = observe_first_publication;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_pool_open_writer(&pool, &snapshot.builder,
      snapshot.memory, &options, &opening));
    snapshot.observer = NULL;
    TEST_ASSERT_TRUE(publication_observed);
    TEST_ASSERT_EQUAL_UINT(publication_allocations, snapshot.backing.allocation_calls);
    TEST_ASSERT_TRUE(snapshot.triggered);
    TEST_ASSERT_NULL(pool.state.data);
    TEST_ASSERT_NULL(pool.writer);
    TEST_ASSERT_NULL(pool.reader);
    TEST_ASSERT_EQUAL_UINT64(0, snapshot.memory->used);
    TEST_ASSERT_EQUAL_UINT64(generation, opening.confirmed_generation);
    enum pfs_completion expected = cut->publication == 0 ?
      cut->publishes ? PFS_UNKNOWN : PFS_STOPPED : startup_cut_completion(cut);
    TEST_ASSERT_EQUAL(expected, opening.recovery.completion);
    TEST_ASSERT_EQUAL(PFS_OK, opening.recovery.operation_status);
    TEST_ASSERT_EQUAL(PFS_IO, opening.recovery.maintenance_status);
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
      opening.recovery.maintenance_completion);
    TEST_ASSERT_EQUAL(cut->publishes ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED,
      opening.writer.health);
    TEST_ASSERT_EQUAL(PFS_IO, opening.writer.failure);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&snapshot));
    expect_interrupted_orphan(&snapshot, generation);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
      snapshot.memory, &options, &opening));
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, opening.recovery.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
    expect_cleaned_orphans(&snapshot);
    close_snapshot_fixture();
  }
}

static void
set_seed_generation(uint64_t generation)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock seed_super;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, 0, 1, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
    0, &seed_super));
  struct pfs_block_context context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = 1, .referring_birth = 1, .pool = {{1}},
    .reference = seed_super.root, .features = seed_super.features};
  struct pfs_pool_root root;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, seed_super.root.block, 1,
    bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_decode(bytes, sizeof(bytes), &context, &root));
  struct pfs_reference *refs[] = {&root.volumes, &root.volume_names, &root.allocation};
  /* A slot and its pool root share a birth. Move the complete pool metadata
   * incarnation and map charges together; volume metadata and data stay old. */
  for (size_t i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, refs[i]->block, 1,
      bytes, sizeof(bytes)));
    if (i == 2) {
      struct pfs_tree_context tree_context = {.block = context, .kind = PFS_INDEX_ALLOCATION};
      tree_context.block.reference = *refs[i];
      struct pfs_tree tree;
      TEST_ASSERT_EQUAL(PFS_OK, pfs_tree_decode(bytes, sizeof(bytes), &tree_context, &tree));
      struct pfs_record_context record_context = {.block_count = PFS_POOL_BLOCKS_MIN,
        .selected_generation = 1, .containing_birth = 1, .features = seed_super.features};
      for (size_t j = 0; j < tree.count; j++) {
        struct pfs_allocation_record record;
        TEST_ASSERT_EQUAL(PFS_OK, pfs_allocation_record_decode(bytes + tree.slots[j].offset,
          tree.slots[j].length, &record_context, &record));
        if (record.state == PFS_ALLOCATION_POOL) {
          test_put_u64(bytes + tree.slots[j].offset + 56, generation);
          test_checksum(bytes + tree.slots[j].offset, tree.slots[j].length, 8);
        }
      }
    }
    test_put_u64(bytes + 48, generation);
    test_checksum(bytes, sizeof(bytes), 20);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, refs[i]->block, 1,
      bytes, sizeof(bytes)));
    refs[i]->birth = generation;
  }
  context.selected_generation = context.referring_birth = generation;
  context.reference.birth = generation;
  root.header.birth = generation;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_root_encode(bytes, sizeof(bytes), &context, &root));
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, root.header.block, 1,
    bytes, sizeof(bytes)));
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_read(&seed.builder.reader, block, 1, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
      block, &super));
    super.header.birth = generation;
    super.root.birth = generation;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_encode(bytes, sizeof(bytes), &super));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_block_write(&seed.builder, block, 1, bytes, sizeof(bytes)));
  }
  TEST_ASSERT_EQUAL(PFS_OK, pfs_block_flush(&seed.builder));
}

static void
generation_promise_funds_retained_file_cleanup_at_its_boundary(void)
{
  uint64_t data_blocks = (sizeof(original) + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE;
  uint64_t work = data_blocks + NS_FILE_GRANTS + 1;
  uint64_t generations = 3 + 3 * work;
  /* Conservative funding covers each orphan unit and a terminal fence at
   * every intermediate stop, independent of the healthy publication count. */
  for (unsigned extra = 0; extra < 2; extra++) {
    build_seed(1024, 1024, 1024);
    set_seed_generation(UINT64_MAX - generations + extra);
    open_seed(64, 64);
    struct pfs_write_result result;
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    enum pfs_status status = pfs_view_remove(views[0],
      (const uint8_t *)"file", 4, &result);
    if (extra) {
      TEST_ASSERT_EQUAL(PFS_LIMIT, status);
      TEST_ASSERT_FALSE(result.namespace_confirmed);
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
      TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
      expect_bytes(views[1], original, sizeof(original));
    } else {
      TEST_ASSERT_EQUAL(PFS_OK, status);
      expect_complete(&result, true);
      test_failure_memory_fail_after(&device, 0);
      close_view(1);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(views[0], &result));
      TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, result.maintenance_completion);
      device.backing.fail_after = SIZE_MAX;
      check_durable(0, NS_VOLUMES);
    }
    close_fixture();
  }
}

static void
maximum_length_rename_names_preserve_source_identity_and_bytes(void)
{
  open_fixture();
  lookup(0, "src", 2, PFS_SCOPE_SUBTREE, &parent_rights);
  lookup(0, "dst", 3, PFS_SCOPE_SUBTREE, &parent_rights);
  char source[PFS_NAME_MAX + 1], destination[PFS_NAME_MAX + 1];
  memset(source, 's', PFS_NAME_MAX);
  memset(destination, 'd', PFS_NAME_MAX);
  source[PFS_NAME_MAX] = destination[PFS_NAME_MAX] = 0;
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[0], (const uint8_t *)"file", 4,
    views[2], (const uint8_t *)source, PFS_NAME_MAX, false, &result));
  expect_complete(&result, true);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_rename(views[2], (const uint8_t *)source,
    PFS_NAME_MAX, views[3], (const uint8_t *)destination, PFS_NAME_MAX, false, &result));
  expect_complete(&result, true);
  expect_missing(2, source);
  struct pfs_view_identity found = lookup(3, destination, 4, PFS_SCOPE_OBJECT, &file_rights);
  TEST_ASSERT_EQUAL_MEMORY(&file_id, &found.object, sizeof(file_id));
  expect_bytes(views[4], original, sizeof(original));
  close_fixture();
}

static void
directory_child_handle_file_rights_are_invalid_before_publication(void)
{
  open_fixture();
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  memset(&result, 0xa5, sizeof(result));
  memset(&identity, 0xa5, sizeof(identity));
  struct pfs_write_result previous_result = result;
  struct pfs_view_identity previous_identity = identity;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  TEST_ASSERT_EQUAL(PFS_INVALID, pfs_view_create_directory(views[0],
    (const uint8_t *)"invalid", 7, &parent_rights, &views[2], &identity, &result));
  TEST_ASSERT_NULL(views[2]);
  TEST_ASSERT_EQUAL_MEMORY(&previous_result, &result, sizeof(result));
  TEST_ASSERT_EQUAL_MEMORY(&previous_identity, &identity, sizeof(identity));
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  expect_missing(0, "invalid");
  close_fixture();
}

struct cleanup_charge_observation {
  enum pfs_budget_charge expected;
  uint64_t initial_generation;
  enum pfs_status status;
  size_t retired_states;
  bool wrong_volume_charge;
  bool wrong_occupied_count;
};

static struct cleanup_charge_observation charge_observation;

static enum pfs_status
scan_durable_charges(struct test_failure *adapter,
  const struct pfs_tree_context *context, uint64_t occupied[4], uint64_t *volume_retired)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  enum pfs_status status = test_failure_durable_read(adapter,
    context->block.reference.block, 1, bytes);
  struct pfs_tree tree;
  if (status == PFS_OK) {
    status = pfs_tree_decode(bytes, sizeof(bytes), context, &tree);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context records = {
    .block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth, .features = context->block.features,
  };
  for (size_t i = 0; i < tree.count; i++) {
    const uint8_t *record = bytes + tree.slots[i].offset;
    if (tree.level) {
      struct pfs_internal_record internal;
      status = pfs_internal_record_decode(record, tree.slots[i].length,
        PFS_INDEX_ALLOCATION, &records, &internal);
      if (status == PFS_OK) {
        struct pfs_tree_context child = *context;
        child.block.reference = internal.child;
        child.block.referring_birth = tree.header.birth;
        child.parent_level = tree.level;
        status = scan_durable_charges(adapter, &child, occupied, volume_retired);
      }
    } else {
      struct pfs_allocation_record allocation;
      status = pfs_allocation_record_decode(record, tree.slots[i].length,
        &records, &allocation);
      if (status == PFS_OK) {
        occupied[allocation.charge] += allocation.count;
        const struct pfs_volume_id pool_owner = {{0}};
        if (allocation.state == PFS_ALLOCATION_RETIRED &&
            memcmp(&allocation.owner, &pool_owner, sizeof(pool_owner))) {
          if (allocation.retirement > charge_observation.initial_generation) {
            *volume_retired += allocation.count;
            charge_observation.wrong_volume_charge |=
              allocation.charge != charge_observation.expected;
          }
        }
      }
    }
    if (status != PFS_OK) {
      return status;
    }
  }
  return PFS_OK;
}

static void
observe_durable_cleanup_charges(struct test_failure *adapter,
  const struct test_failure_event *event, bool before, void *context)
{
  (void)context;
  if (before || event->kind != TEST_FAILURE_FLUSH || event->status != PFS_OK ||
      charge_observation.status != PFS_OK) {
    return;
  }
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock selected = {0};
  enum pfs_status status = PFS_OK;
  for (unsigned slot = 0; slot < 2 && status == PFS_OK; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    status = test_failure_durable_read(adapter, block, 1, bytes);
    if (status == PFS_OK) {
      status = pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN,
        block, &super);
    }
    if (status == PFS_OK && super.header.birth > selected.header.birth) {
      selected = super;
    }
  }
  struct pfs_block_context root_context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = selected.header.birth,
    .referring_birth = selected.header.birth, .pool = selected.header.pool,
    .reference = selected.root, .features = selected.features,
  };
  struct pfs_pool_root root;
  if (status == PFS_OK) {
    status = test_failure_durable_read(adapter, selected.root.block, 1, bytes);
  }
  if (status == PFS_OK) {
    status = pfs_pool_root_decode(bytes, sizeof(bytes), &root_context, &root);
  }
  uint64_t occupied[4] = {0}, volume_retired = 0;
  if (status == PFS_OK) {
    struct pfs_tree_context allocation_context = {
      .block = root_context, .kind = PFS_INDEX_ALLOCATION,
    };
    allocation_context.block.reference = root.allocation;
    allocation_context.block.referring_birth = root.header.birth;
    status = scan_durable_charges(adapter, &allocation_context, occupied, &volume_retired);
  }
  if (status == PFS_OK) {
    charge_observation.retired_states += volume_retired != 0;
    charge_observation.wrong_occupied_count |=
      root.cow.occupied != occupied[PFS_CHARGE_ORDINARY] ||
      root.migration.occupied != occupied[PFS_CHARGE_MIGRATION] ||
      root.recovery.occupied != occupied[PFS_CHARGE_RECOVERY];
  }
  charge_observation.status = status;
}

static void
start_charge_observation(struct test_failure *adapter, enum pfs_budget_charge expected)
{
  charge_observation = (struct cleanup_charge_observation){.expected = expected};
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_durable_read(adapter, block, 1, bytes));
    TEST_ASSERT_EQUAL(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes),
      PFS_POOL_BLOCKS_MIN, block, &super));
    if (super.header.birth > charge_observation.initial_generation) {
      charge_observation.initial_generation = super.header.birth;
    }
  }
  adapter->observer = observe_durable_cleanup_charges;
}

static void
expect_charge_observation(struct test_failure *adapter)
{
  adapter->observer = NULL;
  TEST_ASSERT_EQUAL(PFS_OK, charge_observation.status);
  TEST_ASSERT_GREATER_THAN_UINT(0, charge_observation.retired_states);
  TEST_ASSERT_FALSE_MESSAGE(charge_observation.wrong_volume_charge,
    "durable retired volume record uses the wrong workspace charge");
  TEST_ASSERT_FALSE_MESSAGE(charge_observation.wrong_occupied_count,
    "durable workspace occupancy differs from allocation record charges");
}

static void
cleanup_uses_recovery_while_named_and_retained_orphan_mutations_use_ordinary(void)
{
  open_fixture();
  uint8_t expected[NS_BYTES];
  memcpy(expected, original, sizeof(expected));
  static const uint8_t named[] = "named overwrite";
  static const uint8_t retained[] = "retained overwrite";
  struct pfs_write_result result;
  start_charge_observation(&device, PFS_CHARGE_ORDINARY);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[1], 7, named, sizeof(named), &result));
  expect_charge_observation(&device);
  expect_complete(&result, false);
  memcpy(expected + 7, named, sizeof(named));

  start_charge_observation(&device, PFS_CHARGE_ORDINARY);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
  expect_charge_observation(&device);
  expect_complete(&result, true);
  start_charge_observation(&device, PFS_CHARGE_ORDINARY);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[1], 31,
    retained, sizeof(retained), &result));
  expect_charge_observation(&device);
  expect_complete(&result, false);
  memcpy(expected + 31, retained, sizeof(retained));
  expect_bytes(views[1], expected, sizeof(expected));
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &device, 0));

  start_charge_observation(&device, PFS_CHARGE_RECOVERY);
  close_view(1);
  expect_charge_observation(&device);
  close_handles();
  start_charge_observation(&snapshot, PFS_CHARGE_RECOVERY);
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
    snapshot.memory, &options, &opening));
  expect_charge_observation(&snapshot);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, opening.recovery.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  expect_cleaned_orphans(&snapshot);
  close_snapshot_fixture();
}

static struct pfs_object_id small_id;
static uint8_t small_bytes[2u * PFS_BLOCK_SIZE];
static size_t small_length, small_slot_checks;
static bool small_atomic, small_payload_violation;

/* Locate records through each durable root independently. The fixture supplies
 * IDs and expected bytes, never block placements or editor/checker summaries. */
static enum pfs_status
small_durable_record(struct test_failure *adapter, const struct pfs_tree_context *context,
                     struct pfs_volume_record *volume, struct pfs_object_record *object)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  enum pfs_status status = test_failure_durable_read(adapter,
    context->block.reference.block, 1, bytes);
  struct pfs_tree tree;
  if (status == PFS_OK) {
    status = pfs_tree_decode(bytes, sizeof(bytes), context, &tree);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context records = {
    .block_count = context->block.block_count,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth, .features = context->block.features,
  };
  for (size_t i = 0; i < tree.count; i++) {
    const uint8_t *record = bytes + tree.slots[i].offset;
    if (tree.level) {
      struct pfs_internal_record internal;
      status = pfs_internal_record_decode(record, tree.slots[i].length,
        context->kind, &records, &internal);
      if (status == PFS_OK) {
        struct pfs_tree_context child = *context;
        child.block.reference = internal.child;
        child.block.referring_birth = tree.header.birth;
        child.parent_level = tree.level;
        status = small_durable_record(adapter, &child, volume, object);
      }
      if (status != PFS_NOT_FOUND) {
        return status;
      }
    } else if (context->kind == PFS_INDEX_VOLUMES) {
      status = pfs_volume_record_decode(record, tree.slots[i].length, &records, volume);
      const struct pfs_volume_id id = {{2}};
      if (status != PFS_OK || !memcmp(&volume->id, &id, sizeof(id))) {
        return status;
      }
    } else {
      status = pfs_object_record_decode(record, tree.slots[i].length, &records, object);
      if (status != PFS_OK || !memcmp(&object->id, &small_id, sizeof(small_id))) {
        return status;
      }
    }
  }
  return PFS_NOT_FOUND;
}

static enum pfs_status
small_durable_object(struct test_failure *adapter, unsigned slot,
                     struct pfs_object_record *object)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  uint64_t blocks = adapter->builder.reader.geometry.block_count;
  uint64_t block = slot ? blocks - 1 : 0;
  struct pfs_superblock super;
  enum pfs_status status = test_failure_durable_read(adapter, block, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_superblock_decode(bytes, sizeof(bytes), blocks, block, &super);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_tree_context context = {
    .block = {.block_count = blocks, .selected_generation = super.header.birth,
      .referring_birth = super.header.birth, .pool = super.header.pool,
      .features = super.features, .reference = super.root},
    .kind = PFS_INDEX_VOLUMES,
  };
  struct pfs_pool_root root;
  status = test_failure_durable_read(adapter, super.root.block, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_pool_root_decode(bytes, sizeof(bytes), &context.block, &root);
  }
  struct pfs_volume_record volume;
  if (status == PFS_OK) {
    context.block.reference = root.volumes;
    context.block.referring_birth = root.header.birth;
    status = small_durable_record(adapter, &context, &volume, object);
  }
  if (status == PFS_OK) {
    context.kind = PFS_INDEX_OBJECTS;
    context.volume = volume.id;
    context.block.features = volume.features;
    context.block.reference = volume.object_root;
    status = small_durable_record(adapter, &context, &volume, object);
  }
  return status;
}

static bool
small_payload_matches(struct test_failure *adapter, unsigned slot)
{
  struct pfs_object_record object;
  enum pfs_status status = small_durable_object(adapter, slot, &object);
  if (status == PFS_NOT_FOUND) {
    return true;
  }
  if (status != PFS_OK || object.kind != PFS_OBJECT_FILE ||
      object.file_length > small_length ||
      (small_atomic && object.file_length != small_length)) {
    return false;
  }
  if (!object.file_length) {
    return !small_atomic && object.storage_kind == PFS_STORAGE_NONE;
  }
  if (object.storage_kind != PFS_STORAGE_INLINE ||
      object.inline_extent.logical_first ||
      object.inline_extent.count != (object.file_length + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE) {
    return false;
  }
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (uint64_t i = 0; i < object.inline_extent.count; i++) {
    size_t offset = (size_t)i * PFS_BLOCK_SIZE;
    size_t length = object.file_length - offset;
    if (length > sizeof(bytes)) {
      length = sizeof(bytes);
    }
    if (test_failure_durable_read(adapter, object.inline_extent.physical_first + i,
        1, bytes) != PFS_OK || memcmp(bytes, small_bytes + offset, length)) {
      return false;
    }
  }
  return true;
}

static void
observe_small_cleanup(struct test_failure *adapter, const struct test_failure_event *event,
                      bool before, void *context)
{
  observe_cleanup_generation(adapter, event, before, context);
  observe_durable_cleanup_charges(adapter, event, before, context);
  observe_first_publication(adapter, event, before, context);
  if (before && event->kind == TEST_FAILURE_WRITE &&
      (event->first == 0 || event->first == adapter->builder.reader.geometry.block_count - 1)) {
    small_slot_checks++;
    small_payload_violation |= !small_payload_matches(adapter, 0) ||
                               !small_payload_matches(adapter, 1);
  }
}

static void
start_small_cleanup(struct test_failure *adapter, bool atomic)
{
  start_cleanup_trace(adapter);
  small_atomic = atomic;
  small_slot_checks = 0;
  small_payload_violation = false;
  publication_observed = false;
  start_charge_observation(adapter, PFS_CHARGE_RECOVERY);
  adapter->observer = observe_small_cleanup;
}

static size_t
finish_small_cleanup(struct test_failure *adapter)
{
  size_t count = finish_cleanup_trace(adapter);
  TEST_ASSERT_GREATER_THAN_UINT(0, small_slot_checks);
  TEST_ASSERT_FALSE_MESSAGE(small_payload_violation,
    "cleanup changed a still-reachable retained payload or split an eligible deletion");
  TEST_ASSERT_TRUE(publication_observed);
  TEST_ASSERT_EQUAL_UINT(publication_allocations, adapter->backing.allocation_calls);
  expect_charge_observation(adapter);
  return count;
}

static void
create_small_file(size_t length)
{
  TEST_ASSERT_TRUE(length && length <= sizeof(small_bytes));
  small_length = length;
  for (size_t i = 0; i < length; i++) {
    small_bytes[i] = (uint8_t)(i * 43u + 11u);
  }
  struct pfs_write_result result;
  struct pfs_view_identity identity;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_create_file(views[0], (const uint8_t *)"small", 5,
    &file_rights, &views[2], &identity, &result));
  expect_complete(&result, true);
  small_id = identity.object;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_write(views[2], 0, small_bytes, length, &result));
  expect_complete(&result, false);
  expect_bytes(views[2], small_bytes, length);
  struct pfs_object_record object;
  TEST_ASSERT_EQUAL(PFS_OK, small_durable_object(&device, 0, &object));
  TEST_ASSERT_EQUAL(PFS_STORAGE_INLINE, object.storage_kind);
  TEST_ASSERT_EQUAL_UINT64((length + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE,
    object.inline_extent.count);
  size_t grants;
  struct pfs_principal_id owner;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_inspect(views[2], &owner, NULL, 0, &grants));
  TEST_ASSERT_EQUAL_UINT(0, grants);
}

static void
unlink_small_file(void)
{
  struct pfs_write_result result;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"small", 5, &result));
  expect_complete(&result, true);
  expect_missing(0, "small");
  expect_fresh_id_denied(&small_id);
  expect_bytes(views[2], small_bytes, small_length);
}

static void
expect_small_durable(struct test_failure *adapter, uint64_t generation, bool cleaned)
{
  struct pfs_check_result check;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&adapter->builder.reader, adapter->memory,
    NULL, NULL, &check));
  TEST_ASSERT_TRUE(check.cross_complete);
  for (unsigned slot = 0; slot < 2; slot++) {
    struct pfs_object_record object;
    enum pfs_status status = small_durable_object(adapter, slot, &object);
    TEST_ASSERT_TRUE(status == PFS_OK || status == PFS_NOT_FOUND);
    bool present = status == PFS_OK;
    TEST_ASSERT_TRUE(check.state[slot].complete);
    TEST_ASSERT_EQUAL_UINT64(present, check.state[slot].orphans);
    TEST_ASSERT_EQUAL_UINT64((sizeof(original) + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE + present,
      check.state[slot].file_blocks);
    TEST_ASSERT_EQUAL_UINT64(NS_GRANTS, check.state[slot].grants);
    TEST_ASSERT_TRUE(small_payload_matches(adapter, slot));
    if (present) {
      const struct pfs_object_id parent = {{0}};
      TEST_ASSERT_EQUAL_MEMORY(&parent, &object.parent, sizeof(parent));
    }
    if (cleaned) {
      TEST_ASSERT_FALSE(present);
    }
  }
  if (generation) {
    unsigned selected = check.state[1].generation > check.state[0].generation;
    TEST_ASSERT_EQUAL_UINT64(generation, check.state[selected].generation);
  }
}

static void
create_small_abandoned_snapshot(void)
{
  open_fixture();
  create_small_file(PFS_BLOCK_SIZE - 17u);
  unlink_small_file();
  TEST_ASSERT_EQUAL(PFS_OK, test_failure_clone_durable(&snapshot, &device, 0));
  close_handles();
  test_failure_trace_reset(&snapshot);
}

static void
small_orphan_close_and_startup_preserve_retained_payload_and_recovery_accounting(void)
{
  create_small_abandoned_snapshot();
  start_small_cleanup(&snapshot, true);
  struct pfs_write_open_result opening;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
    snapshot.memory, &options, &opening));
  finish_small_cleanup(&snapshot);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, opening.recovery.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
  expect_small_durable(&snapshot, 0, true);
  close_snapshot_fixture();

  open_fixture();
  create_small_file(PFS_BLOCK_SIZE);
  unlink_small_file();
  start_small_cleanup(&device, true);
  test_failure_memory_fail_after(&device, 0);
  struct pfs_view_close_result closing;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_close(&views[2], &closing));
  TEST_ASSERT_TRUE(closing.released);
  TEST_ASSERT_NULL(views[2]);
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_PENDING, closing.maintenance_completion);
  TEST_ASSERT_EQUAL(PFS_OK, closing.maintenance_status);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, closing.health);
  finish_small_cleanup(&device);
  struct pfs_write_result fence;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(views[0], &fence));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, fence.maintenance_completion);
  device.backing.fail_after = SIZE_MAX;
  close_handles();
  expect_small_durable(&device, 0, true);
  close_fixture();
}

static void
small_orphan_failures_preserve_atomic_progress_and_resume_without_retry(void)
{
  for (unsigned startup = 0; startup < 2; startup++) {
    if (startup) {
      create_small_abandoned_snapshot();
    } else {
      open_fixture();
      create_small_file(PFS_BLOCK_SIZE - 17u);
      unlink_small_file();
    }
    struct test_failure *adapter = startup ? &snapshot : &device;
    start_small_cleanup(adapter, true);
    struct pfs_write_open_result opening;
    if (startup) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &snapshot.builder,
        snapshot.memory, &options, &opening));
    } else {
      close_view(2);
    }
    size_t count = finish_small_cleanup(adapter);
    size_t progress_cut = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
      const struct test_failure_flush_cut *cut = &cleanup_cuts[i];
      if (cut->publishes && cleanup_before[cut->ordinal - 1].orphans &&
          !cleanup_after[cut->ordinal - 1].orphans) {
        progress_cut = i;
        break;
      }
    }
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, progress_cut);
    if (startup) {
      TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
      close_snapshot_fixture();
    } else {
      close_fixture();
    }
    for (size_t i = 0; i <= count; i++) {
      bool durable_unknown = i == count;
      const struct test_failure_flush_cut *cut = &cleanup_cuts[durable_unknown ? progress_cut : i];
      uint64_t generation = cleanup_generations[cut->ordinal - 1];
      if (startup) {
        create_small_abandoned_snapshot();
      } else {
        open_fixture();
        create_small_file(PFS_BLOCK_SIZE - 17u);
        unlink_small_file();
      }
      adapter = startup ? &snapshot : &device;
      struct test_failure_fault fault = {.kind = TEST_FAILURE_FLUSH,
        .ordinal = adapter->ordinals[TEST_FAILURE_FLUSH] + cut->ordinal,
        .mode = durable_unknown ? TEST_FAILURE_FLUSH_PREFIX : TEST_FAILURE_BEFORE,
        .prefix = SIZE_MAX, .enabled = true};
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(adapter, &fault));
      enum pfs_writer_health health = cut->publishes ?
        PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED;
      enum pfs_maintenance_completion maintenance = cut->publishes ?
        PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED;
      if (startup) {
        publication_observed = false;
        adapter->observer = observe_first_publication;
        TEST_ASSERT_EQUAL(PFS_IO, pfs_pool_open_writer(&pool, &snapshot.builder,
          snapshot.memory, &options, &opening));
        adapter->observer = NULL;
        TEST_ASSERT_TRUE(publication_observed);
        TEST_ASSERT_EQUAL_UINT(publication_allocations, adapter->backing.allocation_calls);
        TEST_ASSERT_NULL(pool.state.data);
        TEST_ASSERT_NULL(pool.writer);
        TEST_ASSERT_NULL(pool.reader);
        TEST_ASSERT_EQUAL_UINT64(0, adapter->memory->used);
        TEST_ASSERT_EQUAL_UINT64(generation, opening.confirmed_generation);
        TEST_ASSERT_EQUAL(startup_cut_completion(cut), opening.recovery.completion);
        TEST_ASSERT_EQUAL(PFS_OK, opening.recovery.operation_status);
        TEST_ASSERT_EQUAL(PFS_IO, opening.recovery.maintenance_status);
        TEST_ASSERT_EQUAL(maintenance, opening.recovery.maintenance_completion);
        TEST_ASSERT_EQUAL(health, opening.writer.health);
        TEST_ASSERT_EQUAL(PFS_IO, opening.writer.failure);
      } else {
        test_failure_memory_fail_after(adapter, 0);
        struct pfs_view_close_result closing;
        TEST_ASSERT_EQUAL(PFS_IO, pfs_view_close(&views[2], &closing));
        TEST_ASSERT_TRUE(closing.released);
        TEST_ASSERT_NULL(views[2]);
        TEST_ASSERT_EQUAL(maintenance, closing.maintenance_completion);
        TEST_ASSERT_EQUAL(PFS_IO, closing.maintenance_status);
        TEST_ASSERT_EQUAL(health, closing.health);
        adapter->backing.fail_after = SIZE_MAX;
        uint64_t writes = adapter->ordinals[TEST_FAILURE_WRITE];
        uint64_t flushes = adapter->ordinals[TEST_FAILURE_FLUSH];
        struct pfs_write_result result;
        TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(views[0], &result));
        close_handles();
        TEST_ASSERT_EQUAL_UINT64(writes, adapter->ordinals[TEST_FAILURE_WRITE]);
        TEST_ASSERT_EQUAL_UINT64(flushes, adapter->ordinals[TEST_FAILURE_FLUSH]);
      }
      TEST_ASSERT_TRUE(adapter->triggered);
      TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(adapter));
      small_atomic = true;
      expect_small_durable(adapter, durable_unknown ? 0 : generation, false);
      if (durable_unknown) {
        unsigned selected;
        struct pfs_check_result check;
        TEST_ASSERT_EQUAL(PFS_OK, pfs_check(&adapter->builder.reader, adapter->memory,
          NULL, NULL, &check));
        selected = check.state[1].generation > check.state[0].generation;
        TEST_ASSERT_EQUAL_UINT64(0, check.state[selected].orphans);
      }
      TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &adapter->builder,
        adapter->memory, &options, &opening));
      TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
      expect_small_durable(adapter, 0, true);
      if (startup) {
        close_snapshot_fixture();
      } else {
        close_fixture();
      }
    }
  }
}

static void
small_cleanup_planning_reads_stop_access_without_fallback(void)
{
  static uint64_t cuts[TEST_FAILURE_EVENTS_MAX];
  open_fixture();
  create_small_file(PFS_BLOCK_SIZE - 17u);
  unlink_small_file();
  uint64_t read_base = device.ordinals[TEST_FAILURE_READ];
  test_failure_trace_reset(&device);
  close_view(2);
  size_t count = 0;
  /* Every healthy pre-write read is a planning cut, including the eligibility
   * grant probe. No physical address or saved callback ordinal is required. */
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_WRITE) {
      break;
    }
    if (event->kind == TEST_FAILURE_READ) {
      cuts[count++] = event->ordinal - read_base;
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT(0, count);
  close_fixture();

  for (size_t i = 0; i < count; i++) {
    open_fixture();
    create_small_file(PFS_BLOCK_SIZE - 17u);
    unlink_small_file();
    struct test_failure_fault fault = {.kind = TEST_FAILURE_READ,
      .ordinal = device.ordinals[TEST_FAILURE_READ] + cuts[i],
      .mode = TEST_FAILURE_BEFORE, .enabled = true};
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_set_fault(&device, &fault));
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
    uint64_t flushes = device.ordinals[TEST_FAILURE_FLUSH];
    struct pfs_view_close_result closing;
    TEST_ASSERT_EQUAL(PFS_IO, pfs_view_close(&views[2], &closing));
    TEST_ASSERT_TRUE(device.triggered);
    TEST_ASSERT_TRUE(closing.released);
    TEST_ASSERT_NULL(views[2]);
    TEST_ASSERT_EQUAL(PFS_MAINTENANCE_STOPPED, closing.maintenance_completion);
    TEST_ASSERT_EQUAL(PFS_IO, closing.maintenance_status);
    TEST_ASSERT_EQUAL(PFS_WRITER_ACCESS_STOPPED, closing.health);
    struct pfs_write_result result;
    TEST_ASSERT_EQUAL(PFS_RECOVERY_REQUIRED, pfs_view_checkpoint(views[0], &result));
    close_handles();
    TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    TEST_ASSERT_EQUAL_UINT64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
    TEST_ASSERT_EQUAL(PFS_OK, test_failure_cold_cut(&device));
    small_atomic = true;
    expect_small_durable(&device, 0, false);
    for (unsigned slot = 0; slot < 2; slot++) {
      struct pfs_object_record object;
      TEST_ASSERT_EQUAL(PFS_OK, small_durable_object(&device, slot, &object));
      TEST_ASSERT_EQUAL_UINT64(small_length, object.file_length);
    }
    struct pfs_write_open_result opening;
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
      device.memory, &options, &opening));
    TEST_ASSERT_EQUAL(PFS_WRITER_READY, opening.writer.health);
    TEST_ASSERT_EQUAL(PFS_OK, pfs_pool_close(&pool));
    expect_small_durable(&device, 0, true);
    close_fixture();
  }
}

static void
small_cleanup_excludes_grants_and_inline_multi_block_mappings(void)
{
  for (unsigned grants = 0; grants < 2; grants++) {
    open_fixture();
    struct pfs_write_result result;
    size_t closing_view;
    if (grants) {
      small_id = file_id;
      small_length = PFS_BLOCK_SIZE;
      memcpy(small_bytes, original, small_length);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_resize(views[1], small_length, &result));
      expect_complete(&result, false);
      TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"file", 4, &result));
      expect_complete(&result, true);
      closing_view = 1;
    } else {
      create_small_file(sizeof(small_bytes));
      unlink_small_file();
      closing_view = 2;
    }
    start_small_cleanup(&device, false);
    test_failure_memory_fail_after(&device, 0);
    close_view(closing_view);
    size_t count = finish_small_cleanup(&device);
    bool data_removed_before_pair = false;
    uint64_t background = grants ? 0 : (sizeof(original) + PFS_BLOCK_SIZE - 1) / PFS_BLOCK_SIZE;
    for (size_t i = 0; i < count; i++) {
      const struct cleanup_progress *after = &cleanup_after[cleanup_cuts[i].ordinal - 1];
      data_removed_before_pair |= after->orphans == 1 && after->file_blocks == background;
    }
    TEST_ASSERT_TRUE(data_removed_before_pair);
    device.backing.fail_after = SIZE_MAX;
    check_durable(0, grants ? NS_VOLUMES : NS_GRANTS);
    close_fixture();
  }
}

static void
small_orphan_near_minimum_profile_and_quota_fund_last_release(void)
{
  struct pfs_plan_limits limits;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 2, 8, 2, &limits));
  uint64_t recovery = limits.recovery_blocks > 256 ? limits.recovery_blocks : 256;
  /* The configured files use two mappings and five data blocks. Five objects
   * and three directories promise four namespace blocks, plus the first
   * volume's object/grant indexes: quota 11 and metadata profile 6 + 2 for
   * the empty second volume. Recovery uses the computed bound/format floor. */
  open_fixture_config(11, 2, 8, 1024, recovery);
  create_small_file(PFS_BLOCK_SIZE);
  struct pfs_write_result result;
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE];
  uint8_t growth = 0x69;
  enum pfs_status status = pfs_view_write(views[2], small_length, &growth, 1, &result);
  /* A new block exceeds quota; a separate mapping can also exceed E first. */
  TEST_ASSERT_TRUE(status == PFS_QUOTA || status == PFS_LIMIT);
  TEST_ASSERT_EQUAL_UINT64(0, result.confirmed_bytes);
  TEST_ASSERT_EQUAL(PFS_WRITER_READY, result.health);
  TEST_ASSERT_EQUAL_UINT64(writes, device.ordinals[TEST_FAILURE_WRITE]);
  observe_funded_operation();
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_remove(views[0], (const uint8_t *)"small", 5, &result));
  expect_funded_operation();
  expect_complete(&result, true);
  expect_bytes(views[2], small_bytes, small_length);
  start_small_cleanup(&device, true);
  test_failure_memory_fail_after(&device, 0);
  close_view(2);
  finish_small_cleanup(&device);
  struct pfs_write_result fence;
  TEST_ASSERT_EQUAL(PFS_OK, pfs_view_checkpoint(views[0], &fence));
  TEST_ASSERT_EQUAL(PFS_MAINTENANCE_COMPLETE, fence.maintenance_completion);
  device.backing.fail_after = SIZE_MAX;
  close_handles();
  expect_small_durable(&device, 0, true);
  close_fixture();
}

void
run_namespace_tests(void)
{
  Unity.TestFile = __FILE__;
  RUN_TEST(directories_inherit_creation_policy_and_require_empty_removal);
  RUN_TEST(unlinked_files_keep_identity_bytes_and_independent_held_rights);
  RUN_TEST(detached_directories_keep_empty_identity_and_deny_insertion);
  RUN_TEST(rename_moves_identity_and_replacement_preserves_displaced_file);
  RUN_TEST(namespace_authority_precedes_names_and_noops_do_not_publish);
  RUN_TEST(rename_refuses_volume_crossing_and_directory_replacement);
  RUN_TEST(live_tokens_invalidate_only_directories_whose_entries_change);
  RUN_TEST(final_orphan_release_finishes_deletion_and_fences_without_more_memory);
  RUN_TEST(failed_final_release_consumes_handle_and_startup_drains_abandoned_orphan);
  RUN_TEST(durable_restart_loses_runtime_retention_and_cleans_validated_orphans);
  RUN_TEST(minimum_quota_profile_and_computed_recovery_fund_final_release);
  RUN_TEST(pool_promise_boundary_protects_unlink_and_final_cleanup);
  RUN_TEST(quota_filled_namespace_preserves_multiple_orphan_nodes_and_funded_release);
  RUN_TEST(generation_promise_funds_retained_file_cleanup_at_its_boundary);
  RUN_TEST(maximum_length_rename_names_preserve_source_identity_and_bytes);
  RUN_TEST(directory_child_handle_file_rights_are_invalid_before_publication);
  RUN_TEST(final_close_maintenance_failures_consume_view_and_resume_paired_orphan);
  RUN_TEST(interrupted_startup_preserves_confirmed_generation_and_withholds_instance);
  RUN_TEST(cleanup_uses_recovery_while_named_and_retained_orphan_mutations_use_ordinary);
  RUN_TEST(small_orphan_close_and_startup_preserve_retained_payload_and_recovery_accounting);
  RUN_TEST(small_orphan_failures_preserve_atomic_progress_and_resume_without_retry);
  RUN_TEST(small_cleanup_planning_reads_stop_access_without_fallback);
  RUN_TEST(small_cleanup_excludes_grants_and_inline_multi_block_mappings);
  RUN_TEST(small_orphan_near_minimum_profile_and_quota_fund_last_release);
}
