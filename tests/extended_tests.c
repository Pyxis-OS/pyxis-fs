/* SPDX-License-Identifier: MPL-2.0 */
#include "extended_tests.h"
#include "failure.h"
#include "plan.h"
#include "unity.h"
#include <pyxis_fs/build.h>
#include <pyxis_fs/check.h>
#include <pyxis_fs/write.h>
#include <inttypes.h>
#include <string.h>

#define SOURCE_BYTES (2u * PFS_BLOCK_SIZE + 17u)
#define VICTIM_BYTES (130u * PFS_BLOCK_SIZE + 19u)
#define HISTORY_BYTES (2u * PFS_BLOCK_SIZE + 31u)
#define CUTS_MAX 512u

enum extended_operation { EXT_RENAME, EXT_RELEASE, EXT_STARTUP };
enum extended_phase { EXT_ADMISSION, EXT_REPLACEMENT, EXT_SLOT, EXT_FIRST_FLUSH, EXT_SECOND_FLUSH };

struct extended_cut {
  struct test_failure_fault fault;
  enum extended_phase phase;
  size_t publication;
};

static struct test_fixture fixture;
static struct test_failure device, abandoned;
static struct pfs_pool pool;
static struct pfs_volume volume;
static struct pfs_view *root, *source_parent, *destination_parent, *victim, *current, *candidate;
static struct pfs_trusted_context authority;
static struct extended_cut cuts[CUTS_MAX];
static size_t cut_count;
static uint64_t workload_seed, random_sequence, starting_generation;
static char detail[256];
static const char *operation_names[] = {"rename", "final release", "startup"};
static const char *phase_names[] = {"admission", "replacement write", "slot write",
  "replacement flush", "slot flush"};
static const struct pfs_object_id source_id = {{6}}, victim_id = {{7}};
static const struct pfs_volume_id volume_id = {{2}};
static const struct pfs_rights file_rights = {
  .file = PFS_FILE_READ | PFS_FILE_METADATA | PFS_FILE_WRITE | PFS_FILE_RESIZE | PFS_FILE_CHECKPOINT,
};
static const struct pfs_rights parent_rights = {
  .file = PFS_FILE_READ | PFS_FILE_METADATA | PFS_FILE_WRITE | PFS_FILE_RESIZE | PFS_FILE_CHECKPOINT,
  .directory = PFS_DIR_LIST | PFS_DIR_LOOKUP | PFS_DIR_METADATA | PFS_DIR_CREATE |
    PFS_DIR_REMOVE | PFS_DIR_REPLACE,
};
static struct pfs_write_options options;

#define EXPECT_STATUS(expected, expression) TEST_ASSERT_EQUAL_MESSAGE(expected, expression, detail)
#define EXPECT_TRUE(expression) TEST_ASSERT_TRUE_MESSAGE(expression, detail)
#define EXPECT_U64(expected, actual) TEST_ASSERT_EQUAL_UINT64_MESSAGE(expected, actual, detail)

static uint64_t
mix(uint64_t value)
{
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static uint8_t
expected_byte(unsigned salt, uint64_t offset)
{
  return (uint8_t)(mix(workload_seed ^ ((uint64_t)salt << 48) ^ (offset / 8)) >> (offset % 8 * 8));
}

static void
fill_bytes(uint8_t *buffer, size_t length, unsigned salt, uint64_t offset)
{
  for (size_t i = 0; i < length; i++) {
    buffer[i] = expected_byte(salt, offset + i);
  }
}

static enum pfs_status
random_bytes(void *context, void *buffer, size_t length)
{
  (void)context;
  ++random_sequence;
  uint8_t *bytes = buffer;
  for (size_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)(mix(random_sequence + i / 8 + workload_seed) >> (i % 8 * 8));
  }
  return PFS_OK;
}

static enum pfs_status
source_read(void *context, size_t v, size_t object, uint64_t offset,
            void *buffer, size_t length)
{
  (void)context;
  size_t size = object == 3 ? SOURCE_BYTES : VICTIM_BYTES;
  if (v || (object != 3 && object != 4) || offset > size || length > size - offset) {
    return PFS_INVALID;
  }
  fill_bytes(buffer, length, object == 3 ? 1 : 2, offset);
  return PFS_OK;
}

static enum pfs_status
source_validate(void *context)
{
  (void)context;
  return PFS_OK;
}

static void
add_victim_grants(const struct pfs_volume_record *record)
{
  uint8_t bytes[PFS_BLOCK_SIZE], encoded[8][PFS_GRANT_RECORD_SIZE];
  struct pfs_encoded_record records[8];
  struct pfs_tree_context context = {
    .block = {.block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1,
      .referring_birth = 1, .pool = {{1}}, .reference = record->grant_root},
    .kind = PFS_INDEX_GRANTS, .volume = {{2}},
  };
  struct pfs_record_context record_context = {
    .block_count = PFS_POOL_BLOCKS_MIN, .selected_generation = 1, .containing_birth = 1,
  };
  EXPECT_STATUS(PFS_OK, pfs_block_read(&fixture.builder.reader,
    record->grant_root.block, 1, bytes, sizeof(bytes)));
  struct pfs_tree tree;
  EXPECT_STATUS(PFS_OK, pfs_tree_decode(bytes, sizeof(bytes), &context, &tree));
  EXPECT_U64(1, tree.count);
  struct pfs_grant_record grant;
  EXPECT_STATUS(PFS_OK, pfs_grant_record_decode(bytes + tree.slots[0].offset,
    tree.slots[0].length, &record_context, &grant));
  for (size_t i = 0; i < 8; i++) {
    if (i) {
      grant = (struct pfs_grant_record){.object = {{7}}, .principal = {{(uint8_t)(20 + i)}},
        .scope = PFS_SCOPE_OBJECT, .file_rights = PFS_FILE_READ};
    }
    records[i].data = encoded[i];
    EXPECT_STATUS(PFS_OK, pfs_grant_record_encode(encoded[i], sizeof(encoded[i]),
      &record_context, &grant, &records[i].length));
  }
  tree.count = 8;
  EXPECT_STATUS(PFS_OK, pfs_tree_encode(bytes, sizeof(bytes), &context, &tree, records));
  EXPECT_STATUS(PFS_OK, pfs_block_write(&fixture.builder,
    record->grant_root.block, 1, bytes, sizeof(bytes)));
}

static void
build_fixture(void)
{
  random_sequence = 0;
  options = (struct pfs_write_options){.extent_limit = 256, .metadata_limit = 256,
    .random = random_bytes};
  EXPECT_STATUS(PFS_OK, test_fixture_open(&fixture, PFS_POOL_BLOCKS_MIN, 0));
  struct pfs_build_object objects[] = {
    {.id = {{3}}, .parent = UINT32_MAX, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{4}}, .parent = 0, .name = {3, "src"}, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{5}}, .parent = 0, .name = {3, "dst"}, .kind = PFS_OBJECT_DIRECTORY},
    {.id = {{6}}, .parent = 1, .name = {4, "item"}, .kind = PFS_OBJECT_FILE,
      .file_length = SOURCE_BYTES},
    {.id = {{7}}, .parent = 2, .name = {4, "item"}, .kind = PFS_OBJECT_FILE,
      .file_length = VICTIM_BYTES},
  };
  struct pfs_build_volume specification = {.id = {{2}}, .root_object = {{3}},
    .name = {4, "home"}, .owner = {{8}}, .quota_set = true, .quota = 4096,
    .guarantee_set = true, .object_count = 5, .objects = objects};
  struct pfs_build_spec spec = {.block_count = PFS_POOL_BLOCKS_MIN, .pool = {{1}},
    .volume_count = 1, .volumes = &specification,
    .reserve_set = PFS_RESERVE_COW | PFS_RESERVE_MIGRATION | PFS_RESERVE_RECOVERY,
    .cow_reserve = 1024, .migration_reserve = 1024, .recovery_reserve = 1024};
  struct pfs_build_plan plan = {0};
  struct pfs_build_source source = {.read = source_read, .validate = source_validate};
  EXPECT_STATUS(PFS_OK, pfs_build_plan_create(&fixture.memory, &spec, &plan));
  EXPECT_STATUS(PFS_OK, pfs_build(&plan, &fixture.builder, &source));
  add_victim_grants(&plan.volumes[0]);
  EXPECT_STATUS(PFS_OK, pfs_build_plan_destroy(&plan));
}

static void
close_view(struct pfs_view **view)
{
  struct pfs_view_close_result closed;
  EXPECT_STATUS(PFS_OK, pfs_view_close(view, &closed));
  EXPECT_TRUE(closed.released && !*view);
}

static void
close_handles(struct test_failure *adapter)
{
  close_view(&candidate);
  close_view(&current);
  close_view(&victim);
  close_view(&root);
  close_view(&source_parent);
  close_view(&destination_parent);
  EXPECT_STATUS(PFS_OK, pfs_volume_close(&volume));
  EXPECT_STATUS(PFS_OK, pfs_pool_close(&pool));
  EXPECT_U64(0, adapter->memory->used);
}

static void
open_device(void)
{
  struct pfs_plan_limits limits;
  EXPECT_STATUS(PFS_OK, pfs_plan_limits(PFS_POOL_BLOCKS_MIN, 256, 256, 1, &limits));
  EXPECT_STATUS(PFS_OK, test_failure_open(&device, PFS_POOL_BLOCKS_MIN,
    (size_t)limits.pool_blocks + PFS_PLAN_VOLUME_NEW + 1, 0, &fixture.builder.reader));
  pool = (struct pfs_pool){0};
  volume = (struct pfs_volume){0};
  root = source_parent = destination_parent = victim = current = candidate = NULL;
  struct pfs_write_open_result opening;
  EXPECT_STATUS(PFS_OK, pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening));
  EXPECT_STATUS(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  authority = (struct pfs_trusted_context){.principal = {{8}}, .root = {{3}},
    .scope = PFS_SCOPE_SUBTREE, .ceiling = {PFS_FILE_RIGHTS_ALL, PFS_DIR_RIGHTS_ALL, 0}};
  const struct pfs_object_id ids[] = {{{3}}, {{4}}, {{5}}};
  struct pfs_view **parents[] = {&root, &source_parent, &destination_parent};
  for (size_t i = 0; i < 3; i++) {
    EXPECT_STATUS(PFS_OK, pfs_view_acquire(&volume, &authority, &ids[i],
      PFS_SCOPE_SUBTREE, &parent_rights, parents[i]));
  }
  EXPECT_STATUS(PFS_OK, pfs_view_acquire(&volume, &authority, &victim_id,
    PFS_SCOPE_OBJECT, &file_rights, &victim));
}

static void
expect_complete(const struct pfs_write_result *result, bool changed)
{
  EXPECT_STATUS(PFS_COMPLETE, result->completion);
  EXPECT_STATUS(PFS_OK, result->operation_status);
  EXPECT_STATUS(PFS_OK, result->maintenance_status);
  EXPECT_STATUS(PFS_WRITER_READY, result->health);
  EXPECT_TRUE(result->namespace_confirmed == changed);
}

static void
expect_file(struct pfs_view *view, const uint8_t *expected, size_t length)
{
  struct pfs_view_metadata metadata;
  EXPECT_STATUS(PFS_OK, pfs_view_metadata(view, &metadata));
  EXPECT_U64(length, metadata.size);
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (size_t offset = 0; offset < length; offset += sizeof(bytes)) {
    size_t wanted = length - offset;
    if (wanted > sizeof(bytes)) {
      wanted = sizeof(bytes);
    }
    size_t count;
    EXPECT_STATUS(PFS_OK, pfs_view_read(view, offset, bytes, wanted, &count));
    EXPECT_U64(wanted, count);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected + offset, bytes, wanted, detail);
  }
}

static uint64_t
selected_generation(struct test_failure *adapter)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  uint64_t generation = 0;
  for (unsigned slot = 0; slot < 2; slot++) {
    uint64_t block = slot ? PFS_POOL_BLOCKS_MIN - 1 : 0;
    struct pfs_superblock super;
    EXPECT_STATUS(PFS_OK, test_failure_durable_read(adapter, block, 1, bytes));
    EXPECT_STATUS(PFS_OK, pfs_superblock_decode(bytes, sizeof(bytes),
      PFS_POOL_BLOCKS_MIN, block, &super));
    if (super.header.birth > generation) {
      generation = super.header.birth;
    }
  }
  return generation;
}

/* These readers inspect durable bytes only, including from observers. They
 * never enter the active pool or allocate through its memory owner. */
struct payload_record {
  struct pfs_volume_record volume;
  struct pfs_object_record object;
};

static enum pfs_status
find_payload_record(struct test_failure *adapter, const struct pfs_tree_context *context,
                    const uint8_t id[PFS_ID_SIZE], struct payload_record *out)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  enum pfs_status status = test_failure_durable_read(adapter, context->block.reference.block, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_tree_decode(bytes, sizeof(bytes), context, &tree);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth, .features = context->block.features};
  for (size_t i = 0; i < tree.count; i++) {
    const uint8_t *record = bytes + tree.slots[i].offset;
    if (tree.level) {
      struct pfs_internal_record internal;
      status = pfs_internal_record_decode(record, tree.slots[i].length, context->kind, &records, &internal);
      if (status == PFS_OK) {
        struct pfs_tree_context child = *context;
        child.block.reference = internal.child;
        child.block.referring_birth = tree.header.birth;
        child.parent_level = tree.level;
        status = find_payload_record(adapter, &child, id, out);
      }
      if (status != PFS_NOT_FOUND) {
        return status;
      }
    } else if (context->kind == PFS_INDEX_VOLUMES) {
      status = pfs_volume_record_decode(record, tree.slots[i].length, &records, &out->volume);
      if (status != PFS_OK || !memcmp(id, out->volume.id.bytes, PFS_ID_SIZE)) {
        return status;
      }
    } else {
      status = pfs_object_record_decode(record, tree.slots[i].length, &records, &out->object);
      if (status != PFS_OK || !memcmp(id, out->object.id.bytes, PFS_ID_SIZE)) {
        return status;
      }
    }
  }
  return PFS_NOT_FOUND;
}

static enum pfs_status
compare_mapping(struct test_failure *adapter, const struct pfs_extent_mapping *mapping,
                size_t length, unsigned salt, uint64_t *offset)
{
  if (mapping->logical_first * PFS_BLOCK_SIZE != *offset || *offset >= length) {
    return PFS_CORRUPT;
  }
  uint8_t bytes[PFS_BLOCK_SIZE];
  for (uint64_t i = 0; i < mapping->count && *offset < length; i++) {
    enum pfs_status status = test_failure_durable_read(adapter, mapping->physical_first + i, 1, bytes);
    if (status != PFS_OK) {
      return status;
    }
    size_t wanted = length - (size_t)*offset;
    if (wanted > sizeof(bytes)) {
      wanted = sizeof(bytes);
    }
    for (size_t j = 0; j < wanted; j++) {
      if (bytes[j] != expected_byte(salt, *offset + j)) {
        return PFS_CORRUPT;
      }
    }
    *offset += wanted;
  }
  return PFS_OK;
}

static enum pfs_status
compare_extent_tree(struct test_failure *adapter, const struct pfs_tree_context *context,
                    size_t length, unsigned salt, uint64_t *offset)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_tree tree;
  enum pfs_status status = test_failure_durable_read(adapter, context->block.reference.block, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_tree_decode(bytes, sizeof(bytes), context, &tree);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_record_context records = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = context->block.selected_generation,
    .containing_birth = tree.header.birth, .features = context->block.features};
  for (size_t i = 0; i < tree.count && status == PFS_OK; i++) {
    const uint8_t *record = bytes + tree.slots[i].offset;
    if (tree.level) {
      struct pfs_internal_record internal;
      status = pfs_internal_record_decode(record, tree.slots[i].length, PFS_INDEX_EXTENTS, &records, &internal);
      if (status == PFS_OK) {
        struct pfs_tree_context child = *context;
        child.block.reference = internal.child;
        child.block.referring_birth = tree.header.birth;
        child.parent_level = tree.level;
        status = compare_extent_tree(adapter, &child, length, salt, offset);
      }
    } else {
      struct pfs_extent_record extent;
      status = pfs_extent_record_decode(record, tree.slots[i].length, &records, &extent);
      if (status == PFS_OK) {
        status = compare_mapping(adapter, &extent.mapping, length, salt, offset);
      }
    }
  }
  return status;
}

static enum pfs_status
compare_retained_file(struct test_failure *adapter, struct pfs_tree_context *context,
                      const struct pfs_object_id *id, size_t length, unsigned salt,
                      bool present)
{
  struct payload_record record;
  enum pfs_status status = find_payload_record(adapter, context, id->bytes, &record);
  if (!present) {
    return status == PFS_NOT_FOUND ? PFS_OK : status == PFS_OK ? PFS_CORRUPT : status;
  }
  if (status != PFS_OK || record.object.file_length != length) {
    return status == PFS_OK ? PFS_CORRUPT : status;
  }
  uint64_t offset = 0;
  if (record.object.storage_kind == PFS_STORAGE_INLINE) {
    status = compare_mapping(adapter, &record.object.inline_extent, length, salt, &offset);
  } else if (record.object.storage_kind == PFS_STORAGE_TREE) {
    struct pfs_tree_context extents = *context;
    extents.kind = PFS_INDEX_EXTENTS;
    extents.object = *id;
    extents.block.reference = record.object.tree_root;
    status = compare_extent_tree(adapter, &extents, length, salt, &offset);
  } else if (length) {
    status = PFS_CORRUPT;
  }
  return status == PFS_OK && offset != length ? PFS_CORRUPT : status;
}

struct retained_slot_expectation {
  uint64_t generation;
  bool victim_present;
  size_t victim_length;
};

static struct retained_slot_expectation frozen_slots[2];
static enum pfs_status retained_status;
static size_t retained_checks;
static bool publication_active, slot_pending;

static enum pfs_status
retained_slot_context(struct test_failure *adapter, uint64_t slot,
                      struct pfs_tree_context *out, uint64_t *generation)
{
  uint8_t bytes[PFS_BLOCK_SIZE];
  struct pfs_superblock super;
  enum pfs_status status = test_failure_durable_read(adapter, slot, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_superblock_decode(bytes, sizeof(bytes), PFS_POOL_BLOCKS_MIN, slot, &super);
  }
  if (status != PFS_OK) {
    return status;
  }
  struct pfs_block_context context = {.block_count = PFS_POOL_BLOCKS_MIN,
    .selected_generation = super.header.birth, .referring_birth = super.header.birth,
    .pool = super.header.pool, .reference = super.root, .features = super.features};
  struct pfs_pool_root pool_root;
  status = test_failure_durable_read(adapter, super.root.block, 1, bytes);
  if (status == PFS_OK) {
    status = pfs_pool_root_decode(bytes, sizeof(bytes), &context, &pool_root);
  }
  struct pfs_tree_context tree = {.block = context, .kind = PFS_INDEX_VOLUMES};
  struct payload_record record;
  if (status == PFS_OK) {
    tree.block.reference = pool_root.volumes;
    tree.block.referring_birth = pool_root.header.birth;
    status = find_payload_record(adapter, &tree, volume_id.bytes, &record);
  }
  if (status == PFS_OK) {
    tree.kind = PFS_INDEX_OBJECTS;
    tree.volume = volume_id;
    tree.block.reference = record.volume.object_root;
    tree.block.features = record.volume.features;
    *out = tree;
    *generation = super.header.birth;
  }
  return status;
}

static enum pfs_status
freeze_retained_slot(struct test_failure *adapter, uint64_t slot,
                     struct retained_slot_expectation *expected)
{
  struct pfs_tree_context context;
  enum pfs_status status = retained_slot_context(adapter, slot, &context, &expected->generation);
  struct payload_record record;
  if (status == PFS_OK) {
    status = find_payload_record(adapter, &context, victim_id.bytes, &record);
  }
  if (status == PFS_NOT_FOUND) {
    expected->victim_present = false;
    expected->victim_length = 0;
    return PFS_OK;
  }
  if (status == PFS_OK) {
    if (record.object.file_length > VICTIM_BYTES) {
      return PFS_CORRUPT;
    }
    expected->victim_present = true;
    expected->victim_length = (size_t)record.object.file_length;
  }
  return status;
}

static enum pfs_status
compare_frozen_slot(struct test_failure *adapter, uint64_t slot,
                    const struct retained_slot_expectation *expected)
{
  struct pfs_tree_context context;
  uint64_t generation;
  enum pfs_status status = retained_slot_context(adapter, slot, &context, &generation);
  if (status == PFS_OK && generation != expected->generation) {
    return PFS_CORRUPT;
  }
  if (status == PFS_OK) {
    status = compare_retained_file(adapter, &context, &source_id, SOURCE_BYTES, 1, true);
  }
  if (status == PFS_OK) {
    status = compare_retained_file(adapter, &context, &victim_id,
      expected->victim_length, 2, expected->victim_present);
  }
  return status;
}

static void
freeze_retained_slots(struct test_failure *adapter)
{
  retained_status = freeze_retained_slot(adapter, 0, &frozen_slots[0]);
  if (retained_status == PFS_OK) {
    retained_status = freeze_retained_slot(adapter, PFS_POOL_BLOCKS_MIN - 1, &frozen_slots[1]);
  }
}

static void
compare_frozen_slots(struct test_failure *adapter)
{
  if (retained_status == PFS_OK) {
    retained_status = compare_frozen_slot(adapter, 0, &frozen_slots[0]);
  }
  if (retained_status == PFS_OK) {
    retained_status = compare_frozen_slot(adapter, PFS_POOL_BLOCKS_MIN - 1, &frozen_slots[1]);
  }
  retained_checks += 2;
}

static void
observe_retained_payloads(struct test_failure *adapter, const struct test_failure_event *event,
                          bool before, void *context)
{
  (void)context;
  if (retained_status != PFS_OK) {
    return;
  }
  if (before && event->kind == TEST_FAILURE_WRITE) {
    bool slot = event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1;
    if (!publication_active) {
      /* Capture applicability before any replacement bytes can overwrite a
       * retained tree. Cleanup may choose its batch size at the prior commit. */
      freeze_retained_slots(adapter);
      publication_active = true;
    }
    if (slot) {
      compare_frozen_slots(adapter);
      slot_pending = true;
    }
  } else if (!before && event->kind == TEST_FAILURE_FLUSH &&
             event->status == PFS_OK && slot_pending) {
    /* This slot completed its publication; the next batch may freeze the
     * newly committed presence/length, never a modified retained record. */
    publication_active = slot_pending = false;
  }
}

static void
start_retained_observation(struct test_failure *adapter)
{
  retained_status = PFS_OK;
  retained_checks = 0;
  publication_active = slot_pending = false;
  freeze_retained_slots(adapter);
  for (size_t i = 0; i < 2 && retained_status == PFS_OK; i++) {
    if (!frozen_slots[i].victim_present || frozen_slots[i].victim_length != VICTIM_BYTES) {
      retained_status = PFS_CORRUPT;
    }
  }
  compare_frozen_slots(adapter);
  adapter->observer = observe_retained_payloads;
}

static void
check_retained_observation(struct test_failure *adapter, bool early_failure)
{
  adapter->observer = NULL;
  if (early_failure) {
    /* A failed replacement or first flush never publishes a slot. Check both
     * frozen states even when the pre-slot observer was never reached. */
    compare_frozen_slots(adapter);
  }
  EXPECT_STATUS(PFS_OK, retained_status);
}

static void
unlink_victim(void)
{
  struct pfs_write_result result;
  EXPECT_STATUS(PFS_OK, pfs_view_remove(destination_parent, (const uint8_t *)"item", 4, &result));
  expect_complete(&result, true);
}

static void
prepare_operation(enum extended_operation operation)
{
  if (operation == EXT_STARTUP) {
    EXPECT_STATUS(PFS_OK, test_failure_clone_durable(&device, &abandoned, 0));
    pool = (struct pfs_pool){0};
    volume = (struct pfs_volume){0};
    root = source_parent = destination_parent = victim = current = candidate = NULL;
  } else {
    random_sequence = 0;
    open_device();
    if (operation == EXT_RELEASE) {
      unlink_victim();
    }
  }
  starting_generation = selected_generation(&device);
  test_failure_trace_reset(&device);
}

static enum pfs_status
execute_operation(enum extended_operation operation, struct pfs_write_result *result,
                  struct pfs_view_close_result *closed, struct pfs_write_open_result *opening)
{
  if (operation == EXT_RENAME) {
    return pfs_view_rename(source_parent, (const uint8_t *)"item", 4,
      destination_parent, (const uint8_t *)"item", 4, true, result);
  }
  if (operation == EXT_RELEASE) {
    return pfs_view_close(&victim, closed);
  }
  return pfs_pool_open_writer(&pool, &device.builder, device.memory, &options, opening);
}

static void
append_cut(const struct test_failure_event *event, uint64_t base, enum extended_phase phase,
           size_t publication, enum test_failure_mode mode, size_t prefix, uint64_t slot)
{
  EXPECT_TRUE(cut_count < CUTS_MAX);
  cuts[cut_count++] = (struct extended_cut){
    .fault = {.kind = event->kind, .ordinal = event->ordinal - base,
      .mode = mode, .prefix = prefix, .block = slot, .enabled = true},
    .phase = phase, .publication = publication,
  };
}

static size_t
discover_cuts(enum extended_operation operation)
{
  prepare_operation(operation);
  uint64_t base_write = device.ordinals[TEST_FAILURE_WRITE];
  uint64_t base_flush = device.ordinals[TEST_FAILURE_FLUSH];
  start_retained_observation(&device);
  struct pfs_write_result result;
  struct pfs_view_close_result closed;
  struct pfs_write_open_result opening;
  EXPECT_STATUS(PFS_OK, execute_operation(operation, &result, &closed, &opening));
  check_retained_observation(&device, false);
  EXPECT_TRUE(retained_checks > 0);
  cut_count = 0;
  size_t publication = 0;
  bool replacement = false, slot_written = false;
  uint64_t slot = 0;
  for (size_t i = 0; i < device.event_count; i++) {
    const struct test_failure_event *event = &device.events[i];
    if (event->kind == TEST_FAILURE_WRITE) {
      if (event->first == 0 || event->first == PFS_POOL_BLOCKS_MIN - 1) {
        slot = event->first;
        slot_written = true;
        append_cut(event, base_write, EXT_SLOT, publication, TEST_FAILURE_BEFORE, 0, slot);
        append_cut(event, base_write, EXT_SLOT, publication, TEST_FAILURE_TORN_SLOT, 32, slot);
        append_cut(event, base_write, EXT_SLOT, publication, TEST_FAILURE_TORN_SLOT, 64, slot);
      } else if (!replacement) {
        replacement = true;
        append_cut(event, base_write, EXT_REPLACEMENT, publication, TEST_FAILURE_BEFORE, 0, 0);
        append_cut(event, base_write, EXT_REPLACEMENT, publication, TEST_FAILURE_PENDING_PREFIX, 1, 0);
        append_cut(event, base_write, EXT_REPLACEMENT, publication, TEST_FAILURE_DURABLE_PREFIX, 1, 0);
      }
    } else if (event->kind == TEST_FAILURE_FLUSH) {
      enum extended_phase phase = slot_written ? EXT_SECOND_FLUSH :
        replacement ? EXT_FIRST_FLUSH : EXT_ADMISSION;
      append_cut(event, base_flush, phase, publication, TEST_FAILURE_FLUSH_PREFIX, 0, 0);
      append_cut(event, base_flush, phase, publication, TEST_FAILURE_FLUSH_PREFIX, 1, 0);
      append_cut(event, base_flush, phase, publication, TEST_FAILURE_FLUSH_PREFIX, SIZE_MAX, 0);
      if (slot_written) {
        append_cut(event, base_flush, phase, publication, TEST_FAILURE_CACHE_ONLY_SLOT, SIZE_MAX, slot);
        publication++;
        replacement = slot_written = false;
      }
    }
  }
  EXPECT_TRUE(publication > 0 && cut_count > 0);
  struct test_failure_flush_cut flush_cuts[CUTS_MAX];
  size_t flush_count;
  EXPECT_STATUS(PFS_OK, test_failure_flush_cuts(&device, base_flush,
    flush_cuts, CUTS_MAX, &flush_count));
  EXPECT_U64(2 * publication, flush_count);
  close_handles(&device);
  EXPECT_TRUE(!device.infrastructure_failure);
  EXPECT_TRUE(test_failure_close(&device));
  return publication;
}

static void
expect_diagnostic_file(const struct pfs_object_id *id, size_t length, unsigned salt)
{
  struct pfs_object_record object;
  EXPECT_STATUS(PFS_OK, pfs_volume_diagnostic_object(&volume, id, &object));
  EXPECT_U64(length, object.file_length);
  uint8_t actual[PFS_BLOCK_SIZE], expected[PFS_BLOCK_SIZE];
  for (size_t offset = 0; offset < length; offset += sizeof(actual)) {
    size_t wanted = length - offset;
    if (wanted > sizeof(actual)) {
      wanted = sizeof(actual);
    }
    size_t count;
    EXPECT_STATUS(PFS_OK, pfs_volume_diagnostic_read(&volume, id, offset,
      actual, wanted, &count));
    EXPECT_U64(wanted, count);
    fill_bytes(expected, wanted, salt, offset);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, actual, wanted, detail);
  }
  size_t count;
  EXPECT_STATUS(PFS_OK, pfs_volume_diagnostic_read(&volume, id, length, actual, 1, &count));
  EXPECT_U64(0, count);
}

static bool
inspect_durable_namespace(enum extended_operation operation, bool torn,
                          bool cleaned, bool expected_moved)
{
  struct pfs_check_result check;
  EXPECT_STATUS(torn ? PFS_CORRUPT : PFS_OK,
    pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  struct pfs_pool_diagnostic diagnostic;
  EXPECT_STATUS(PFS_OK, pfs_pool_open(&pool, &device.builder.reader, device.memory, &diagnostic));
  EXPECT_TRUE(diagnostic.degraded == torn);
  EXPECT_STATUS(PFS_OK, pfs_pool_diagnostic_volume_open(&pool, &volume_id, &volume));
  const struct pfs_object_id root_id = {{3}};
  struct pfs_object_record source, destination, displaced;
  enum pfs_status source_status = pfs_volume_diagnostic_resolve(&volume, &root_id,
    (const uint8_t *)"src/item", 8, &source);
  enum pfs_status destination_status = pfs_volume_diagnostic_resolve(&volume, &root_id,
    (const uint8_t *)"dst/item", 8, &destination);
  bool moved = operation == EXT_RENAME && source_status == PFS_NOT_FOUND;
  if (operation == EXT_RENAME) {
    EXPECT_STATUS(moved ? PFS_NOT_FOUND : PFS_OK, source_status);
    EXPECT_STATUS(PFS_OK, destination_status);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(moved ? &source_id : &victim_id,
      &destination.id, sizeof(destination.id), detail);
    if (!moved) {
      TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&source_id, &source.id, sizeof(source.id), detail);
    }
    if (cleaned) {
      EXPECT_TRUE(moved == expected_moved);
    }
  } else {
    EXPECT_STATUS(PFS_OK, source_status);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&source_id, &source.id, sizeof(source.id), detail);
    EXPECT_STATUS(PFS_NOT_FOUND, destination_status);
  }
  expect_diagnostic_file(&source_id, SOURCE_BYTES, 1);
  enum pfs_status displaced_status = pfs_volume_diagnostic_object(&volume, &victim_id, &displaced);
  if (cleaned && (operation != EXT_RENAME || moved)) {
    EXPECT_STATUS(PFS_NOT_FOUND, displaced_status);
    EXPECT_U64(0, check.state[diagnostic.selected].orphans);
  } else if (operation == EXT_RENAME) {
    EXPECT_STATUS(PFS_OK, displaced_status);
    expect_diagnostic_file(&victim_id, VICTIM_BYTES, 2);
    EXPECT_U64(moved ? 1 : 0, check.state[diagnostic.selected].orphans);
  } else if (displaced_status == PFS_OK) {
    EXPECT_TRUE(displaced.file_length <= VICTIM_BYTES);
    expect_diagnostic_file(&victim_id, (size_t)displaced.file_length, 2);
    EXPECT_U64(1, check.state[diagnostic.selected].orphans);
    const struct pfs_object_id no_parent = {{0}};
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&no_parent, &displaced.parent, sizeof(no_parent), detail);
  } else {
    EXPECT_STATUS(PFS_NOT_FOUND, displaced_status);
    EXPECT_U64(0, check.state[diagnostic.selected].orphans);
  }
  EXPECT_STATUS(PFS_OK, pfs_volume_close(&volume));
  EXPECT_STATUS(PFS_OK, pfs_pool_close(&pool));
  return moved;
}

static void
run_cut(enum extended_operation operation, const struct extended_cut *cut, size_t index)
{
  snprintf(detail, sizeof(detail), "seed=%" PRIu64 " operation=%s cut=%zu/%zu publication=%zu phase=%s mode=%u prefix=%zu",
    workload_seed, operation_names[operation], index + 1, cut_count,
    cut->publication, phase_names[cut->phase], (unsigned)cut->fault.mode, cut->fault.prefix);
  prepare_operation(operation);
  struct test_failure_fault fault = cut->fault;
  fault.ordinal += device.ordinals[fault.kind];
  EXPECT_STATUS(PFS_OK, test_failure_set_fault(&device, &fault));
  start_retained_observation(&device);
  struct pfs_write_result result;
  struct pfs_view_close_result closed;
  struct pfs_write_open_result opening;
  EXPECT_STATUS(PFS_IO, execute_operation(operation, &result, &closed, &opening));
  check_retained_observation(&device, cut->phase == EXT_ADMISSION ||
    cut->phase == EXT_REPLACEMENT || cut->phase == EXT_FIRST_FLUSH);
  EXPECT_TRUE(device.triggered && !device.infrastructure_failure);
  bool uncertain = cut->phase == EXT_SLOT || cut->phase == EXT_SECOND_FLUSH;
  enum pfs_writer_health health = uncertain ? PFS_WRITER_ACCESS_STOPPED : PFS_WRITER_READABLE_STOPPED;
  if (operation == EXT_STARTUP) {
    EXPECT_TRUE(!pool.state.data && !pool.writer && !pool.reader && !pool.memory);
    EXPECT_U64(0, device.memory->used);
    EXPECT_U64(starting_generation + cut->publication, opening.confirmed_generation);
    EXPECT_STATUS(health, opening.writer.health);
    EXPECT_STATUS(PFS_IO, opening.writer.failure);
  } else {
    struct pfs_writer_status status;
    EXPECT_STATUS(PFS_OK, pfs_pool_writer_status(&pool, &status));
    EXPECT_STATUS(health, status.health);
    EXPECT_STATUS(PFS_IO, status.failure);
    EXPECT_TRUE(!status.invariant_failure);
    if (operation == EXT_RENAME) {
      EXPECT_TRUE(result.namespace_confirmed == (cut->publication != 0));
      EXPECT_STATUS(cut->publication ? PFS_COMPLETE : uncertain ? PFS_UNKNOWN : PFS_STOPPED,
        result.completion);
      EXPECT_STATUS(cut->publication ? PFS_OK : PFS_IO, result.operation_status);
    } else {
      EXPECT_TRUE(closed.released && !victim);
      EXPECT_STATUS(uncertain ? PFS_MAINTENANCE_UNKNOWN : PFS_MAINTENANCE_STOPPED,
        closed.maintenance_completion);
      EXPECT_STATUS(PFS_IO, closed.maintenance_status);
    }
    uint64_t writes = device.ordinals[TEST_FAILURE_WRITE], flushes = device.ordinals[TEST_FAILURE_FLUSH];
    EXPECT_STATUS(PFS_RECOVERY_REQUIRED, pfs_view_remove(source_parent,
      (const uint8_t *)"absent", 6, &result));
    struct pfs_view_metadata metadata;
    EXPECT_STATUS(uncertain ? PFS_RECOVERY_REQUIRED : PFS_OK,
      pfs_view_metadata(source_parent, &metadata));
    close_handles(&device);
    EXPECT_U64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    EXPECT_U64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  }
  EXPECT_STATUS(PFS_OK, test_failure_cold_cut(&device));
  bool torn = cut->fault.mode == TEST_FAILURE_TORN_SLOT;
  bool moved = inspect_durable_namespace(operation, torn, false, false);
  if (operation == EXT_RENAME && cut->publication) {
    EXPECT_TRUE(moved);
  }
  uint64_t writes = device.ordinals[TEST_FAILURE_WRITE], flushes = device.ordinals[TEST_FAILURE_FLUSH];
  enum pfs_status recovery_status = pfs_pool_open_writer(&pool, &device.builder,
    device.memory, &options, &opening);
  EXPECT_STATUS(torn ? PFS_CORRUPT : PFS_OK, recovery_status);
  if (torn) {
    EXPECT_TRUE(!pool.state.data && !pool.writer && !pool.reader && !pool.memory);
    EXPECT_U64(0, device.memory->used);
    EXPECT_U64(writes, device.ordinals[TEST_FAILURE_WRITE]);
    EXPECT_U64(flushes, device.ordinals[TEST_FAILURE_FLUSH]);
  } else {
    EXPECT_STATUS(PFS_WRITER_READY, opening.writer.health);
    EXPECT_STATUS(PFS_OK, opening.recovery.maintenance_status);
    EXPECT_STATUS(PFS_OK, pfs_pool_close(&pool));
    inspect_durable_namespace(operation, false, true, moved);
  }
  EXPECT_TRUE(!device.infrastructure_failure);
  EXPECT_TRUE(test_failure_close(&device));
}

static void
run_campaign(enum extended_operation operation)
{
  snprintf(detail, sizeof(detail), "seed=%" PRIu64 " discover operation=%s", workload_seed,
    operation_names[operation]);
  build_fixture();
  if (operation == EXT_STARTUP) {
    open_device();
    unlink_victim();
    EXPECT_STATUS(PFS_OK, test_failure_clone_durable(&abandoned, &device, 0));
    close_handles(&device);
    EXPECT_TRUE(test_failure_close(&device));
  }
  size_t publications = discover_cuts(operation);
  printf("[extended] seed=%" PRIu64 " %s: %zu publications, %zu selected cuts\n",
    workload_seed, operation_names[operation], publications, cut_count);
  fflush(stdout);
  for (size_t i = 0; i < cut_count; i++) {
    run_cut(operation, &cuts[i], i);
    if ((i + 1) % 50 == 0) {
      printf("[extended] seed=%" PRIu64 " %s: %zu/%zu cuts checked\n",
        workload_seed, operation_names[operation], i + 1, cut_count);
      fflush(stdout);
    }
  }
  if (operation == EXT_STARTUP) {
    EXPECT_TRUE(!abandoned.infrastructure_failure);
    EXPECT_TRUE(test_failure_close(&abandoned));
  }
  EXPECT_TRUE(test_fixture_close(&fixture));
}

static void
extended_namespace_cuts(void)
{
  run_campaign(EXT_RENAME);
}

static void
extended_final_release_cuts(void)
{
  run_campaign(EXT_RELEASE);
}

static void
extended_startup_cuts(void)
{
  run_campaign(EXT_STARTUP);
}

static struct pfs_view_identity
lookup_history_file(struct pfs_view *parent, const char *name, struct pfs_view **out)
{
  const struct pfs_rights rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA};
  struct pfs_view_identity identity;
  EXPECT_STATUS(PFS_OK, pfs_view_lookup(parent, (const uint8_t *)name, strlen(name),
    PFS_SCOPE_OBJECT, &rights, out, &identity));
  return identity;
}

static void
expect_history_names(struct pfs_view *parent, const char *name, bool live)
{
  struct pfs_directory_token start = {0}, next;
  struct pfs_view_entry entries[2];
  size_t count;
  bool done;
  if (live) {
    EXPECT_STATUS(PFS_OK, pfs_view_directory_live_page(parent, &start, entries, 2,
      &count, &done, &next));
  } else {
    uint64_t position;
    EXPECT_STATUS(PFS_OK, pfs_view_directory_page(parent, 0, entries, 2,
      &count, &done, &position));
  }
  EXPECT_U64(1, count);
  EXPECT_TRUE(done);
  EXPECT_U64(strlen(name), entries[0].name.length);
  EXPECT_STATUS(PFS_OBJECT_FILE, entries[0].kind);
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(name, entries[0].name.bytes, strlen(name), detail);
}

static void
extended_retained_replacement_and_name_reuse_history(void)
{
  snprintf(detail, sizeof(detail), "seed=%" PRIu64 " retained history setup", workload_seed);
  build_fixture();
  open_device();
  unlink_victim();
  close_view(&victim);
  uint8_t named[HISTORY_BYTES], next_bytes[HISTORY_BYTES], retained[HISTORY_BYTES];
  size_t named_length = 0;
  struct pfs_view_identity named_identity = {0};
  size_t incarnations = 0;
  for (unsigned step = 0; step < 24; step++) {
    snprintf(detail, sizeof(detail), "seed=%" PRIu64 " retained history operation=%u", workload_seed, step);
    size_t next_length = HISTORY_BYTES - (size_t)(mix(workload_seed + step) % 1000);
    fill_bytes(next_bytes, next_length, 10 + step, 0);
    struct pfs_view_identity next_identity;
    struct pfs_write_result result;
    EXPECT_STATUS(PFS_OK, pfs_view_create_file(source_parent, (const uint8_t *)"cycle", 5,
      &file_rights, &candidate, &next_identity, &result));
    expect_complete(&result, true);
    EXPECT_TRUE(!current || memcmp(&named_identity.object, &next_identity.object, sizeof(next_identity.object)));
    EXPECT_STATUS(PFS_OK, pfs_view_write(candidate, 0, next_bytes, next_length, &result));
    expect_complete(&result, false);
    EXPECT_STATUS(PFS_OK, pfs_view_rename(source_parent, (const uint8_t *)"cycle", 5,
      destination_parent, (const uint8_t *)"cycle", 5, current != NULL, &result));
    expect_complete(&result, true);
    if (current) {
      expect_file(current, named, named_length);
      struct pfs_view_metadata metadata;
      EXPECT_STATUS(PFS_OK, pfs_view_metadata(current, &metadata));
      TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&named_identity.object, &metadata.identity.object,
        sizeof(named_identity.object), detail);
      struct pfs_view *fresh = NULL;
      EXPECT_STATUS(PFS_NOT_FOUND, pfs_view_acquire(&volume, &authority, &named_identity.object,
        PFS_SCOPE_OBJECT, &file_rights, &fresh));
      EXPECT_TRUE(!fresh);
      memcpy(retained, named, named_length);
      fill_bytes(retained + 17, 64, 60 + step, 17);
      EXPECT_STATUS(PFS_OK, pfs_view_write(current, 17, retained + 17, 64, &result));
      expect_file(current, retained, named_length);
      EXPECT_STATUS(PFS_OK, pfs_view_resize(current, 17, &result));
      EXPECT_STATUS(PFS_OK, pfs_view_resize(current, PFS_BLOCK_SIZE + 7, &result));
      memset(retained + 17, 0, PFS_BLOCK_SIZE + 7 - 17);
      expect_file(current, retained, PFS_BLOCK_SIZE + 7);
      close_view(&current);
    }
    current = candidate;
    candidate = NULL;
    named_identity = next_identity;
    named_length = next_length;
    memcpy(named, next_bytes, named_length);
    incarnations++;
    if (step % 3 == 2) {
      EXPECT_STATUS(PFS_OK, pfs_view_remove(destination_parent, (const uint8_t *)"cycle", 5, &result));
      expect_file(current, named, named_length);
      EXPECT_STATUS(PFS_OK, pfs_view_create_file(destination_parent, (const uint8_t *)"cycle", 5,
        &file_rights, &candidate, &next_identity, &result));
      EXPECT_TRUE(memcmp(&named_identity.object, &next_identity.object, sizeof(next_identity.object)));
      fill_bytes(next_bytes, named_length, 100 + step, 0);
      EXPECT_STATUS(PFS_OK, pfs_view_write(candidate, 0, next_bytes, named_length, &result));
      expect_file(current, named, named_length);
      close_view(&current);
      current = candidate;
      candidate = NULL;
      named_identity = next_identity;
      memcpy(named, next_bytes, named_length);
      incarnations++;
    }
    struct pfs_view *read_only = NULL;
    struct pfs_view_identity found = lookup_history_file(destination_parent, "cycle", &read_only);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&named_identity.object, &found.object,
      sizeof(named_identity.object), detail);
    expect_file(read_only, named, named_length);
    EXPECT_STATUS(PFS_DENIED, pfs_view_write(read_only, 0, "X", 1, &result));
    close_view(&read_only);
    expect_history_names(source_parent, "item", true);
    expect_history_names(destination_parent, "cycle", true);
    EXPECT_TRUE(!device.infrastructure_failure);
    test_failure_trace_reset(&device);
  }
  struct pfs_write_result fence;
  EXPECT_STATUS(PFS_OK, pfs_view_checkpoint(current, &fence));
  EXPECT_STATUS(PFS_MAINTENANCE_COMPLETE, fence.maintenance_completion);
  close_handles(&device);
  snprintf(detail, sizeof(detail), "seed=%" PRIu64 " retained history cold manifest", workload_seed);
  EXPECT_STATUS(PFS_OK, test_failure_cold_cut(&device));
  struct pfs_check_result check;
  EXPECT_STATUS(PFS_OK, pfs_check(&device.builder.reader, device.memory, NULL, NULL, &check));
  EXPECT_TRUE(check.state[0].complete && check.state[1].complete && check.cross_complete);
  EXPECT_U64(0, check.state[0].orphans);
  EXPECT_U64(0, check.state[1].orphans);
  struct pfs_pool_diagnostic diagnostic;
  EXPECT_STATUS(PFS_OK, pfs_pool_open(&pool, &device.builder.reader, device.memory, &diagnostic));
  EXPECT_STATUS(PFS_OK, pfs_pool_volume_open(&pool, &volume_id, &volume));
  const struct pfs_object_id destination_id = {{5}};
  const struct pfs_rights directory_rights = {.file = PFS_FILE_READ | PFS_FILE_METADATA,
    .directory = PFS_DIR_LOOKUP | PFS_DIR_LIST};
  EXPECT_STATUS(PFS_OK, pfs_view_acquire(&volume, &authority, &destination_id,
    PFS_SCOPE_SUBTREE, &directory_rights, &destination_parent));
  struct pfs_view_identity found = lookup_history_file(destination_parent, "cycle", &current);
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&named_identity.object, &found.object, sizeof(named_identity.object), detail);
  expect_file(current, named, named_length);
  expect_history_names(destination_parent, "cycle", false);
  expect_diagnostic_file(&source_id, SOURCE_BYTES, 1);
  close_handles(&device);
  EXPECT_TRUE(test_failure_close(&device));
  EXPECT_TRUE(test_fixture_close(&fixture));
  printf("[extended] seed=%" PRIu64 " retained history: 23 replacements, 8 unlinks/name reuses, %zu incarnations, complete cold manifest\n",
    workload_seed, incarnations);
}

void
run_extended_tests(uint64_t seed)
{
  workload_seed = seed;
  Unity.TestFile = __FILE__;
  RUN_TEST(extended_retained_replacement_and_name_reuse_history);
  RUN_TEST(extended_namespace_cuts);
  RUN_TEST(extended_final_release_cuts);
  RUN_TEST(extended_startup_cuts);
}
