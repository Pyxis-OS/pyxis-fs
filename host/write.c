/* SPDX-License-Identifier: MPL-2.0 */
#include "host.h"
#include <pyxis_fs/write.h>

#include <string.h>

enum write_command {
  WRITE_NONE,
  WRITE_OPEN,
  WRITE_CHECKPOINT,
};

struct write_options {
  enum write_command command;
  const char *image;
  uint64_t memory_limit;
  struct pfs_write_options limits;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  struct pfs_principal_id principal;
  struct pfs_rights rights;
};

static void
usage(FILE *stream)
{
  fputs("Usage: pyxisfs-write --image PATH --extents E --metadata M\n"
        "  [--memory-limit SIZE] open\n"
        "  [--memory-limit SIZE] checkpoint --volume-id ID --object ID\n"
        "    --principal ID --rights file.checkpoint|dir.checkpoint\n"
        "E is a nonnegative extent-record limit; M is a positive metadata-block limit.\n"
        "The image must be an existing regular file from a healthy session.\n"
        "The host owns image access; principal/object IDs select trusted context\n"
        "and persistent grants still govern the requested checkpoint right.\n"
        "No public file or namespace mutation is available in this milestone.\n", stream);
}

static enum pfs_status
decimal_count(const char *text, uint64_t *value)
{
  if (!*text) {
    return PFS_INVALID;
  }
  uint64_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' ||
        result > (UINT64_MAX - (unsigned)(*text - '0')) / 10) {
      return PFS_INVALID;
    }
    result = result * 10 + (unsigned)(*text - '0');
  }
  *value = result;
  return PFS_OK;
}

enum write_option_bit {
  OPT_IMAGE = 1u << 0,
  OPT_EXTENTS = 1u << 1,
  OPT_METADATA = 1u << 2,
  OPT_MEMORY = 1u << 3,
  OPT_VOLUME = 1u << 4,
  OPT_OBJECT = 1u << 5,
  OPT_PRINCIPAL = 1u << 6,
  OPT_RIGHTS = 1u << 7,
};

static enum pfs_status
options_parse(int argc, char **argv, struct write_options *options)
{
  *options = (struct write_options){0};
  unsigned seen = 0;
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    enum write_command command = WRITE_NONE;
    if (!strcmp(argument, "open")) {
      command = WRITE_OPEN;
    } else if (!strcmp(argument, "checkpoint")) {
      command = WRITE_CHECKPOINT;
    }
    if (command != WRITE_NONE) {
      if (options->command != WRITE_NONE) {
        return PFS_INVALID;
      }
      options->command = command;
      continue;
    }
    unsigned bit;
    if (!strcmp(argument, "--image")) {
      bit = OPT_IMAGE;
    } else if (!strcmp(argument, "--extents")) {
      bit = OPT_EXTENTS;
    } else if (!strcmp(argument, "--metadata")) {
      bit = OPT_METADATA;
    } else if (!strcmp(argument, "--memory-limit")) {
      bit = OPT_MEMORY;
    } else if (!strcmp(argument, "--volume-id")) {
      bit = OPT_VOLUME;
    } else if (!strcmp(argument, "--object")) {
      bit = OPT_OBJECT;
    } else if (!strcmp(argument, "--principal")) {
      bit = OPT_PRINCIPAL;
    } else if (!strcmp(argument, "--rights")) {
      bit = OPT_RIGHTS;
    } else {
      return PFS_INVALID;
    }
    if ((seen & bit) || i + 1 == argc) {
      return PFS_INVALID;
    }
    seen |= bit;
    const char *value = argv[++i];
    if (!*value) {
      return PFS_INVALID;
    }
    enum pfs_status status = PFS_OK;
    switch (bit) {
    case OPT_IMAGE:
      options->image = value;
      break;
    case OPT_EXTENTS:
      status = decimal_count(value, &options->limits.extent_limit);
      break;
    case OPT_METADATA:
      status = decimal_count(value, &options->limits.metadata_limit);
      if (status == PFS_OK && !options->limits.metadata_limit) {
        status = PFS_INVALID;
      }
      break;
    case OPT_MEMORY:
      status = host_size_parse(value, &options->memory_limit);
      if (status == PFS_OK && !options->memory_limit) {
        status = PFS_INVALID;
      }
      break;
    case OPT_VOLUME:
      status = pfs_volume_id_parse(value, strlen(value), &options->volume);
      break;
    case OPT_OBJECT:
      status = pfs_object_id_parse(value, strlen(value), &options->object);
      break;
    case OPT_PRINCIPAL:
      status = pfs_principal_id_parse(value, strlen(value), &options->principal);
      break;
    case OPT_RIGHTS:
      if (!strcmp(value, "file.checkpoint")) {
        options->rights.file = PFS_FILE_CHECKPOINT;
      } else if (!strcmp(value, "dir.checkpoint")) {
        options->rights.directory = PFS_DIR_CHECKPOINT;
      } else {
        status = PFS_INVALID;
      }
      break;
    }
    if (status != PFS_OK) {
      return status;
    }
  }
  unsigned common = OPT_IMAGE | OPT_EXTENTS | OPT_METADATA;
  unsigned authority = OPT_VOLUME | OPT_OBJECT | OPT_PRINCIPAL | OPT_RIGHTS;
  if ((seen & common) != common || options->command == WRITE_NONE ||
      (options->command == WRITE_OPEN && (seen & authority)) ||
      (options->command == WRITE_CHECKPOINT && (seen & authority) != authority)) {
    return PFS_INVALID;
  }
  return PFS_OK;
}

static const char *
health_name(enum pfs_writer_health health)
{
  switch (health) {
  case PFS_WRITER_READY: return "ready";
  case PFS_WRITER_READABLE_STOPPED: return "readable-stopped";
  case PFS_WRITER_ACCESS_STOPPED: return "access-stopped";
  }
  return "invalid";
}

static const char *
completion_name(enum pfs_completion completion)
{
  switch (completion) {
  case PFS_COMPLETE: return "complete";
  case PFS_STOPPED: return "stopped";
  case PFS_UNKNOWN: return "unknown";
  }
  return "invalid";
}

static const char *
maintenance_name(enum pfs_maintenance_completion completion)
{
  switch (completion) {
  case PFS_MAINTENANCE_NONE: return "none";
  case PFS_MAINTENANCE_COMPLETE: return "complete";
  case PFS_MAINTENANCE_STOPPED: return "stopped";
  case PFS_MAINTENANCE_UNKNOWN: return "unknown";
  }
  return "invalid";
}

static int
report_failure(const char *operation, enum pfs_status status,
               const struct host_image *image, int current)
{
  if (status == PFS_OK) {
    return current;
  }
  host_error(operation, status, image);
  int next = host_exit_status(status);
  return current > next ? current : next;
}

int
main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    usage(stdout);
    return 0;
  }
  struct write_options options;
  enum pfs_status status = options_parse(argc, argv, &options);
  struct pfs_memory memory = {0};
  if (status == PFS_OK) {
    status = host_memory_init(&memory, options.memory_limit);
  }
  if (status != PFS_OK) {
    host_error("arguments", status, NULL);
    usage(stderr);
    return host_exit_status(status);
  }

  struct host_image image;
  struct pfs_block_builder backing;
  struct pfs_pool pool = {0};
  struct pfs_volume volume = {0};
  struct pfs_view *view = NULL;
  struct pfs_write_open_result opened = {.pool = {.selected = PFS_POOL_NO_SELECTION}};
  status = host_image_open_writer(&image, options.image, &backing);
  const char *operation = "open writable image";
  bool attempted_open = false;
  if (status == PFS_OK) {
    operation = "open writer";
    attempted_open = true;
    status = pfs_pool_open_writer(&pool, &backing, &memory, &options.limits, &opened);
  }
  if (attempted_open) {
    printf("Writer open: %s; extents=%llu metadata=%llu", pfs_status_string(status),
      (unsigned long long)options.limits.extent_limit,
      (unsigned long long)options.limits.metadata_limit);
    if (opened.confirmed_generation) {
      printf("; health=%s; failure=%s; confirmed-generation=%llu\n"
             "  startup-completion=%s startup-operation=%s maintenance=%s maintenance-status=%s",
             health_name(opened.writer.health), pfs_status_string(opened.writer.failure),
             (unsigned long long)opened.confirmed_generation,
             completion_name(opened.recovery.completion),
             pfs_status_string(opened.recovery.operation_status),
             maintenance_name(opened.recovery.maintenance_completion),
             pfs_status_string(opened.recovery.maintenance_status));
    }
    if (opened.reserved_arena_bytes) {
      printf("\n  required-recovery-blocks=%llu permanent-pool-blocks=%llu reserved-arena-bytes=%llu",
        (unsigned long long)opened.required_recovery_blocks,
        (unsigned long long)opened.permanent_pool_blocks,
        (unsigned long long)opened.reserved_arena_bytes);
    }
    fputc('\n', stdout);
  }
  if (status == PFS_OK && options.command == WRITE_CHECKPOINT) {
    operation = "open checkpoint volume";
    status = pfs_pool_volume_open(&pool, &options.volume, &volume);
    struct pfs_trusted_context context = {
      .principal = options.principal,
      .root = options.object,
      .scope = PFS_SCOPE_OBJECT,
      .ceiling = options.rights,
    };
    if (status == PFS_OK) {
      operation = "acquire checkpoint authority";
      status = pfs_view_acquire(&volume, &context, &options.object,
                                PFS_SCOPE_OBJECT, &options.rights, &view);
    }
    if (status == PFS_OK) {
      operation = "checkpoint";
      struct pfs_write_result result;
      status = pfs_view_checkpoint(view, &result);
      if (status != PFS_BUSY && status != PFS_INVALID) {
        printf("Checkpoint: completion=%s operation=%s health=%s maintenance=%s maintenance-status=%s\n",
               completion_name(result.completion), pfs_status_string(result.operation_status),
               health_name(result.health), maintenance_name(result.maintenance_completion),
               pfs_status_string(result.maintenance_status));
      }
    }
  }
  int exit_status = report_failure(operation, status, &image, 0);
  exit_status = report_failure("close checkpoint view", pfs_view_close(&view), NULL, exit_status);
  exit_status = report_failure("close volume", pfs_volume_close(&volume), NULL, exit_status);
  exit_status = report_failure("close pool", pfs_pool_close(&pool), NULL, exit_status);
  exit_status = report_failure("close image", host_image_close(&image), &image, exit_status);
  return exit_status;
}
