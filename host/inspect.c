/* SPDX-License-Identifier: MPL-2.0 */
#include "host.h"
#include <pyxis_fs/pool.h>

#include <string.h>

static void
usage(FILE *stream)
{
  fputs("Usage: pyxisfs-inspect --image PATH [--memory-limit SIZE] info|volumes\n"
        "Inspects a standalone regular pool image under a shared advisory lock.\n", stream);
}

static enum pfs_status
parse_options(int argc, char **argv, const char **path, const char **command,
               uint64_t *limit)
{
  bool memory_set = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--image") == 0) {
      if (*path != NULL || i + 1 == argc || *argv[i + 1] == '\0') {
        return PFS_INVALID;
      }
      *path = argv[++i];
    } else if (strcmp(argv[i], "--memory-limit") == 0) {
      if (memory_set || i + 1 == argc ||
          host_size_parse(argv[++i], limit) != PFS_OK || *limit == 0) {
        return PFS_INVALID;
      }
      memory_set = true;
    } else if (*command == NULL &&
               (strcmp(argv[i], "info") == 0 || strcmp(argv[i], "volumes") == 0)) {
      *command = argv[i];
    } else {
      return PFS_INVALID;
    }
  }
  return *path != NULL && *command != NULL ? PFS_OK : PFS_INVALID;
}

static void
print_selection(const struct pfs_pool_diagnostic *diagnostic, uint64_t blocks)
{
  printf("Geometry: %llu blocks, 4096 bytes/block\n", (unsigned long long)blocks);
  for (unsigned i = 0; i < 2; ++i) {
    const struct pfs_pool_candidate *candidate = &diagnostic->candidate[i];
    printf("Slot %u (block %llu): %s", i,
           (unsigned long long)(i == 0 ? 0 : blocks - 1),
           pfs_status_string(candidate->status));
    if (candidate->status == PFS_OK) {
      char id[PFS_ID_TEXT_SIZE + 1];
      pfs_pool_id_format(&candidate->superblock.header.pool, id);
      printf(" pool=%s generation=%llu root=%llu", id,
             (unsigned long long)candidate->superblock.header.birth,
             (unsigned long long)candidate->superblock.root.block);
    }
    fputc('\n', stdout);
  }
  if (diagnostic->ambiguous) {
    fputs("Selection: ambiguous committed states\n", stdout);
  } else if (diagnostic->selected != PFS_POOL_NO_SELECTION) {
    printf("Selected slot %u%s\n", diagnostic->selected,
           diagnostic->degraded ? " (degraded read-only)" : "");
    if (diagnostic->degraded) {
      fprintf(stderr, "warning: slot %u is %s; using selected state read-only\n",
              diagnostic->selected ^ 1,
              pfs_status_string(diagnostic->candidate[diagnostic->selected ^ 1].status));
    }
  } else {
    fputs("Selection: no open pool\n", stdout);
  }
}

static void
print_info(const struct pfs_pool_candidate *candidate)
{
  const struct pfs_pool_root *root = &candidate->root;
  const struct pfs_features *features = &candidate->superblock.features;
  printf("Pool features: read-required=0x%llx write-required=0x%llx optional=0x%llx\n",
         (unsigned long long)features->read_required,
         (unsigned long long)features->write_required,
         (unsigned long long)features->optional);
  printf("Recorded accounting, not globally verified (4096-byte blocks):\n"
         "  volumes=%llu live-pool=%llu live-volume=%llu retired=%llu free=%llu\n"
         "  cow=%llu occupied=%llu; migration=%llu occupied=%llu; recovery=%llu occupied=%llu\n",
         (unsigned long long)root->volume_count, (unsigned long long)root->live_pool,
         (unsigned long long)root->live_volume, (unsigned long long)root->retired,
         (unsigned long long)root->free, (unsigned long long)root->cow.capacity,
         (unsigned long long)root->cow.occupied,
         (unsigned long long)root->migration.capacity,
         (unsigned long long)root->migration.occupied,
         (unsigned long long)root->recovery.capacity,
         (unsigned long long)root->recovery.occupied);
}

static enum pfs_status
print_volumes(struct pfs_pool *pool, struct pfs_memory *memory, size_t capacity)
{
  struct pfs_allocation output = {0};
  enum pfs_status status = pfs_memory_allocate(memory,
    capacity * sizeof(struct pfs_volume_record), _Alignof(struct pfs_volume_record), &output);
  if (status != PFS_OK) {
    return status;
  }
  size_t count = 0;
  status = pfs_pool_diagnostic_volumes(pool, output.data, capacity, &count);
  if (status == PFS_OK) {
    const struct pfs_volume_record *volumes = output.data;
    fputs("Volumes in name order; recorded accounting is not globally verified:\n", stdout);
    for (size_t i = 0; i < count; ++i) {
      const struct pfs_volume_record *volume = &volumes[i];
      char id[PFS_ID_TEXT_SIZE + 1];
      char root_id[PFS_ID_TEXT_SIZE + 1];
      pfs_volume_id_format(&volume->id, id);
      pfs_object_id_format(&volume->root_object, root_id);
      host_name_print(stdout, volume->name.bytes, volume->name.length);
      bool supported = pfs_features_read(&volume->features) == PFS_OK &&
        volume->object_root.version == PFS_FORMAT_VERSION &&
        (volume->grant_root.block == 0 ||
         volume->grant_root.version == PFS_FORMAT_VERSION);
      printf(" id=%s root=%s contents=%s\n"
             "  live=%llu retired=%llu guarantee=%llu quota=%llu objects=%llu\n"
             "  features: read-required=0x%llx write-required=0x%llx optional=0x%llx\n",
             id, root_id, supported ? "supported" : "unsupported",
             (unsigned long long)volume->live_blocks,
             (unsigned long long)volume->retired_blocks,
             (unsigned long long)volume->guarantee, (unsigned long long)volume->quota,
             (unsigned long long)volume->object_count,
             (unsigned long long)volume->features.read_required,
             (unsigned long long)volume->features.write_required,
             (unsigned long long)volume->features.optional);
    }
  }
  pfs_memory_free(memory, &output);
  return status;
}

static int
selection_exit(enum pfs_status status, const struct pfs_pool_diagnostic *diagnostic)
{
  if (status == PFS_OK) {
    return 0;
  }
  bool corrupt = diagnostic->ambiguous;
  for (unsigned i = 0; i < 2; ++i) {
    enum pfs_status candidate = diagnostic->candidate[i].status;
    if (candidate == PFS_IO || candidate == PFS_NO_MEMORY) {
      return 5;
    }
    if (candidate == PFS_CORRUPT) {
      corrupt = true;
    }
  }
  if (status == PFS_IO || status == PFS_NO_MEMORY) {
    return 5;
  }
  return corrupt ? 3 : host_exit_status(status);
}

int
main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    usage(stdout);
    return 0;
  }
  const char *path = NULL;
  const char *command = NULL;
  uint64_t limit = 0;
  struct pfs_memory memory;
  enum pfs_status status = parse_options(argc, argv, &path, &command, &limit);
  if (status == PFS_OK) {
    status = host_memory_init(&memory, limit);
  }
  if (status != PFS_OK) {
    host_error("arguments", status, NULL);
    usage(stderr);
    return host_exit_status(status);
  }

  struct host_image image;
  struct pfs_block_reader reader;
  struct pfs_pool pool = {0};
  struct pfs_pool_diagnostic diagnostic = {.selected = PFS_POOL_NO_SELECTION};
  const char *operation = "open image";
  int result;
  status = host_image_open(&image, &memory, path, &reader);
  if (status != PFS_OK) {
    result = host_exit_status(status);
    goto done;
  }
  operation = "select pool state (slot diagnostics above)";
  status = pfs_pool_open(&pool, &reader, &memory, &diagnostic);
  print_selection(&diagnostic, reader.geometry.block_count);
  result = selection_exit(status, &diagnostic);
  if (status != PFS_OK) {
    goto done;
  }
  const struct pfs_pool_candidate *selected = &diagnostic.candidate[diagnostic.selected];
  if (strcmp(command, "info") == 0) {
    print_info(selected);
  } else {
    operation = "list volumes (no volume records published on failure)";
    status = print_volumes(&pool, &memory, (size_t)selected->root.volume_count);
    result = host_exit_status(status);
  }

done:
  if (status != PFS_OK) {
    host_error(operation, status, &image);
  }
  pfs_pool_close(&pool);
  enum pfs_status close_status = host_image_close(&image);
  if (close_status != PFS_OK) {
    host_error("close input", close_status, &image);
    result = 5;
  }
  if (fflush(stdout) != 0) {
    host_error("write diagnostics (partial output)", PFS_IO, NULL);
    result = 5;
  }
  return result;
}
