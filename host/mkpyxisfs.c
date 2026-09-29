/* SPDX-License-Identifier: MPL-2.0 */
#include "source.h"
#include <pyxis_fs/build.h>

#include <string.h>

static void
usage(FILE *stream)
{
  fputs("Usage: mkpyxisfs --image PATH --size SIZE\n"
        "  --volume NAME [--source DIRECTORY] --owner ID [--guarantee SIZE] [--quota SIZE] ...\n"
        "  [--cow-reserve SIZE] [--migration-reserve SIZE]\n"
        "  [--recovery-reserve SIZE] [--memory-limit SIZE] [--plan]\n"
        "Sizes: decimal bytes or KiB/MiB/GiB/TiB; capacities are whole blocks.\n"
        "Creates a new sparse regular image from selected source directories.\n", stream);
}

static bool
value_option(const char *option)
{
  return strcmp(option, "--image") == 0 || strcmp(option, "--size") == 0 ||
    strcmp(option, "--volume") == 0 || strcmp(option, "--owner") == 0 ||
    strcmp(option, "--source") == 0 ||
    strcmp(option, "--guarantee") == 0 || strcmp(option, "--quota") == 0 ||
    strcmp(option, "--cow-reserve") == 0 ||
    strcmp(option, "--migration-reserve") == 0 ||
    strcmp(option, "--recovery-reserve") == 0 ||
    strcmp(option, "--memory-limit") == 0;
}

static enum pfs_status
count_options(int argc, char **argv, size_t *volume_count, uint64_t *limit)
{
  bool memory_set = false;
  *volume_count = 0;
  *limit = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--plan") == 0) {
      continue;
    }
    if (!value_option(argv[i]) || i + 1 == argc) {
      return PFS_INVALID;
    }
    const char *option = argv[i++];
    if (strcmp(option, "--volume") == 0) {
      if (++*volume_count > PFS_VOLUME_MAX) {
        return PFS_LIMIT;
      }
    } else if (strcmp(option, "--memory-limit") == 0) {
      if (memory_set || host_size_parse(argv[i], limit) != PFS_OK || *limit == 0) {
        return PFS_INVALID;
      }
      memory_set = true;
    }
  }
  return *volume_count == 0 ? PFS_INVALID : PFS_OK;
}

static bool
id_zero(const uint8_t *id)
{
  static const uint8_t zero[PFS_ID_SIZE];
  return memcmp(id, zero, sizeof(zero)) == 0;
}

static enum pfs_status
new_id(uint8_t *target, struct pfs_build_spec *spec, struct pfs_build_volume *volumes)
{
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    enum pfs_status status = host_random_id(target);
    if (status != PFS_OK) {
      return status;
    }
    bool collision = id_zero(target);
    if (target != spec->pool.bytes && memcmp(target, spec->pool.bytes, PFS_ID_SIZE) == 0) {
      collision = true;
    }
    for (size_t i = 0; i < spec->volume_count; ++i) {
      const uint8_t *ids[] = {volumes[i].id.bytes, volumes[i].root_object.bytes,
                              volumes[i].owner.bytes};
      for (size_t j = 0; j < sizeof(ids) / sizeof(ids[0]); ++j) {
        if (ids[j] != target && memcmp(target, ids[j], PFS_ID_SIZE) == 0) {
          collision = true;
        }
      }
    }
    if (!collision) {
      return PFS_OK;
    }
  }
  return PFS_IO;
}

static enum pfs_status
block_option(const char *text, uint64_t *blocks)
{
  uint64_t bytes;
  if (host_size_parse(text, &bytes) != PFS_OK || bytes % PFS_BLOCK_SIZE != 0) {
    return PFS_INVALID;
  }
  *blocks = bytes / PFS_BLOCK_SIZE;
  return PFS_OK;
}

static enum pfs_status
parse_options(int argc, char **argv, struct pfs_build_spec *spec,
               struct pfs_build_volume *volumes, const char **sources,
               const char **path, bool *plan_only)
{
  size_t current = 0;
  bool size_set = false;
  struct pfs_build_volume *volume = NULL;
  for (int i = 1; i < argc; ++i) {
    const char *option = argv[i];
    if (strcmp(option, "--plan") == 0) {
      if (*plan_only) {
        return PFS_INVALID;
      }
      *plan_only = true;
      continue;
    }
    const char *value = argv[++i];
    if (strcmp(option, "--image") == 0) {
      if (*path != NULL || *value == '\0') {
        return PFS_INVALID;
      }
      *path = value;
    } else if (strcmp(option, "--size") == 0) {
      if (size_set || block_option(value, &spec->block_count) != PFS_OK) {
        return PFS_INVALID;
      }
      size_set = true;
    } else if (strcmp(option, "--volume") == 0) {
      if (volume != NULL && id_zero(volume->owner.bytes)) {
        return PFS_INVALID;
      }
      size_t length = strlen(value);
      if (pfs_name_validate((const uint8_t *)value, length) != PFS_OK) {
        return PFS_INVALID;
      }
      volume = &volumes[current++];
      volume->name.length = (uint16_t)length;
      memcpy(volume->name.bytes, value, length);
    } else if (strcmp(option, "--source") == 0) {
      if (volume == NULL || sources[current - 1] != NULL || *value == '\0') {
        return PFS_INVALID;
      }
      sources[current - 1] = value;
    } else if (strcmp(option, "--owner") == 0) {
      if (volume == NULL || !id_zero(volume->owner.bytes) ||
          pfs_principal_id_parse(value, strlen(value), &volume->owner) != PFS_OK) {
        return PFS_INVALID;
      }
    } else if (strcmp(option, "--guarantee") == 0) {
      if (volume == NULL || volume->guarantee_set ||
          block_option(value, &volume->guarantee) != PFS_OK) {
        return PFS_INVALID;
      }
      volume->guarantee_set = true;
    } else if (strcmp(option, "--quota") == 0) {
      if (volume == NULL || volume->quota_set ||
          block_option(value, &volume->quota) != PFS_OK) {
        return PFS_INVALID;
      }
      volume->quota_set = true;
    } else if (strcmp(option, "--memory-limit") != 0) {
      unsigned bit;
      uint64_t *reserve;
      if (strcmp(option, "--cow-reserve") == 0) {
        bit = PFS_RESERVE_COW;
        reserve = &spec->cow_reserve;
      } else if (strcmp(option, "--migration-reserve") == 0) {
        bit = PFS_RESERVE_MIGRATION;
        reserve = &spec->migration_reserve;
      } else {
        bit = PFS_RESERVE_RECOVERY;
        reserve = &spec->recovery_reserve;
      }
      if ((spec->reserve_set & bit) != 0 || block_option(value, reserve) != PFS_OK) {
        return PFS_INVALID;
      }
      spec->reserve_set |= bit;
    }
  }
  if (*path == NULL || !size_set || volume == NULL || id_zero(volume->owner.bytes)) {
    return PFS_INVALID;
  }
  enum pfs_status status = new_id(spec->pool.bytes, spec, volumes);
  for (size_t i = 0; status == PFS_OK && i < spec->volume_count; ++i) {
    status = new_id(volumes[i].id.bytes, spec, volumes);
    if (status == PFS_OK) {
      status = new_id(volumes[i].root_object.bytes, spec, volumes);
    }
  }
  return status;
}

static void
print_plan(const struct pfs_build_plan *plan)
{
  char id[PFS_ID_TEXT_SIZE + 1];
  pfs_pool_id_format(&plan->pool, id);
  printf("Pool %s: %llu blocks (%llu bytes); sparse image\n", id,
         (unsigned long long)plan->block_count,
         (unsigned long long)(plan->block_count * PFS_BLOCK_SIZE));
  const struct pfs_pool_root *root = &plan->pool_root;
  printf("Capacity plan (4096-byte blocks): pool metadata=%llu volumes=%llu free=%llu\n"
         "  reserves: cow=%llu migration=%llu recovery=%llu; unpromised=%llu\n",
         (unsigned long long)root->live_pool, (unsigned long long)root->live_volume,
         (unsigned long long)root->free, (unsigned long long)root->cow.capacity,
         (unsigned long long)root->migration.capacity,
         (unsigned long long)root->recovery.capacity,
         (unsigned long long)plan->unpromised);
  for (size_t i = 0; i < plan->volume_count; ++i) {
    const struct pfs_volume_record *volume = &plan->volumes[i];
    fputs("  volume ", stdout);
    host_name_print(stdout, volume->name.bytes, volume->name.length);
    pfs_volume_id_format(&volume->id, id);
    printf(" id=%s objects=%llu allocation=%llu guarantee=%llu unused-guarantee=%llu quota=%llu\n",
           id, (unsigned long long)volume->object_count,
           (unsigned long long)volume->live_blocks,
           (unsigned long long)volume->guarantee,
           (unsigned long long)(volume->guarantee > volume->live_blocks ?
                                volume->guarantee - volume->live_blocks : 0),
           (unsigned long long)volume->quota);
  }
  fputs("Reserve policy does not establish writable-operation sufficiency.\n", stdout);
}

int
main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    usage(stdout);
    return 0;
  }
  size_t volume_count;
  uint64_t limit;
  enum pfs_status status = count_options(argc, argv, &volume_count, &limit);
  struct pfs_memory memory;
  if (status == PFS_OK) {
    status = host_memory_init(&memory, limit);
  }
  if (status != PFS_OK) {
    host_error("arguments", status, NULL);
    usage(stderr);
    return host_exit_status(status);
  }

  struct pfs_allocation options = {0};
  struct pfs_allocation paths = {0};
  struct pfs_allocation output_path = {0};
  struct pfs_build_plan plan = {0};
  struct host_source source = {0};
  struct host_image image = {.fd = -1, .parent_fd = -1};
  const char *operation = "allocate volume options";
  const char *path = NULL;
  char *output_name = NULL;
  bool plan_only = false;
  status = pfs_memory_allocate(&memory, volume_count * sizeof(struct pfs_build_volume),
                               _Alignof(struct pfs_build_volume), &options);
  if (status == PFS_OK) {
    status = pfs_memory_allocate(&memory, volume_count * sizeof(const char *),
                                 _Alignof(const char *), &paths);
  }
  if (status != PFS_OK) {
    goto done;
  }
  memset(options.data, 0, options.size);
  memset(paths.data, 0, paths.size);
  struct pfs_build_spec spec = {.volume_count = volume_count, .volumes = options.data};
  operation = "parse options and generate identities";
  status = parse_options(argc, argv, &spec, options.data, paths.data, &path, &plan_only);
  if (status != PFS_OK) {
    usage(stderr);
    goto done;
  }
  operation = "scan source directories";
  status = host_source_open(&source, &memory, options.data, paths.data, volume_count, &spec.pool);
  if (status != PFS_OK) {
    goto done;
  }
  operation = "prepare output parent";
  status = host_image_prepare(&image, &memory, path, &output_path, &output_name);
  if (status == PFS_OK) {
    status = host_source_exclude_output(&source, image.parent_fd);
  }
  if (status != PFS_OK) {
    goto done;
  }
  operation = "plan pool";
  status = pfs_build_plan_create(&memory, &spec, &plan);
  pfs_memory_free(&memory, &options);
  if (status != PFS_OK) {
    goto done;
  }
  operation = "revalidate planned sources";
  status = host_source_validate(&source);
  if (status != PFS_OK) {
    goto done;
  }
  print_plan(&plan);
  if (fflush(stdout) != 0) {
    status = PFS_IO;
    operation = "print capacity plan (no image created)";
    goto done;
  }
  if (plan_only) {
    goto done;
  }
  struct pfs_block_builder builder;
  operation = "create output";
  status = host_image_create(&image, output_name,
                             plan.block_count * PFS_BLOCK_SIZE, &builder);
  if (status != PFS_OK) {
    goto done;
  }
  const struct pfs_build_source input = {
    .context = &source,
    .read = host_source_read,
    .validate = host_source_validate,
  };
  operation = "construct pool";
  status = pfs_build(&plan, &builder, &input);
  if (status != PFS_OK) {
    goto done;
  }
  operation = "publish new image";
  status = host_image_publish(&image);

done:
  if (status != PFS_OK) {
    host_error(operation, status, &image);
    host_source_error(&source);
  }
  enum pfs_status close_status = host_image_close(&image);
  if (close_status != PFS_OK) {
    host_error("close output", close_status, &image);
    status = close_status;
  }
  pfs_build_plan_destroy(&plan);
  close_status = host_source_close(&source);
  if (close_status != PFS_OK) {
    host_error("close sources", close_status, &image);
    status = close_status;
  }
  pfs_memory_free(&memory, &options);
  pfs_memory_free(&memory, &paths);
  pfs_memory_free(&memory, &output_path);
  if (status == PFS_OK && !plan_only) {
    fputs("Created pool; both generation-1 slots flushed.\n", stdout);
    if (fflush(stdout) != 0) {
      host_error("report completion (image creation finished)", PFS_IO, NULL);
      status = PFS_IO;
    }
  }
  return host_exit_status(status);
}
