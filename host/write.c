/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "host.h"
#include <pyxis_fs/write.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define WRITE_INPUT_MAX (16u * 1024u * 1024u)

enum write_command {
  WRITE_NONE,
  WRITE_OPEN,
  WRITE_CHECKPOINT,
  WRITE_CREATE_FILE,
  WRITE_FILE,
  WRITE_RESIZE,
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
  const char *name;
  const char *input;
  uint64_t offset;
  uint64_t length;
};

static void
usage(FILE *stream)
{
  fputs("Usage: pyxisfs-write --image PATH --extents E --metadata M\n"
        "  [--memory-limit SIZE] open\n"
        "  [--memory-limit SIZE] checkpoint --volume-id ID --object ID\n"
        "    --principal ID --rights file.checkpoint|dir.checkpoint\n"
        "  create-file --volume-id ID --object PARENT_ID --principal ID\n"
        "    --rights dir.create --name COMPONENT\n"
        "  write --volume-id ID --object FILE_ID --principal ID\n"
        "    --rights file.write[,file.resize] --input PATH --offset BYTES\n"
        "  resize --volume-id ID --object FILE_ID --principal ID\n"
        "    --rights file.resize --length BYTES\n"
        "E is a nonnegative extent-record limit; M is a positive metadata-block limit.\n"
        "The image must be an existing regular file from a healthy session.\n"
        "The host owns image access; principal/object IDs select trusted context\n"
        "and persistent grants still govern the explicitly requested rights.\n"
        "Write input must be a quiescent regular file of at most 16 MiB.\n"
        "Offset and length are unsigned decimal byte counts.\n", stream);
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
  OPT_NAME = 1u << 8,
  OPT_INPUT = 1u << 9,
  OPT_OFFSET = 1u << 10,
  OPT_LENGTH = 1u << 11,
};

static enum pfs_status
rights_parse(const char *text, struct pfs_rights *rights)
{
  static const struct {
    const char *name;
    bool directory;
    uint64_t bit;
  } tokens[] = {
    {"file.write", false, PFS_FILE_WRITE},
    {"file.resize", false, PFS_FILE_RESIZE},
    {"file.checkpoint", false, PFS_FILE_CHECKPOINT},
    {"dir.create", true, PFS_DIR_CREATE},
    {"dir.checkpoint", true, PFS_DIR_CHECKPOINT},
  };
  while (*text) {
    const char *end = strchr(text, ',');
    size_t length = end ? (size_t)(end - text) : strlen(text);
    size_t i = 0;
    while (i < sizeof(tokens) / sizeof(tokens[0]) &&
           (strlen(tokens[i].name) != length || memcmp(tokens[i].name, text, length))) {
      ++i;
    }
    if (i == sizeof(tokens) / sizeof(tokens[0])) {
      return PFS_INVALID;
    }
    uint64_t *mask = tokens[i].directory ? &rights->directory : &rights->file;
    if (*mask & tokens[i].bit) {
      return PFS_INVALID;
    }
    *mask |= tokens[i].bit;
    if (!end) {
      return PFS_OK;
    }
    text = end + 1;
  }
  return PFS_INVALID;
}

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
    } else if (!strcmp(argument, "create-file")) {
      command = WRITE_CREATE_FILE;
    } else if (!strcmp(argument, "write")) {
      command = WRITE_FILE;
    } else if (!strcmp(argument, "resize")) {
      command = WRITE_RESIZE;
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
    } else if (!strcmp(argument, "--name")) {
      bit = OPT_NAME;
    } else if (!strcmp(argument, "--input")) {
      bit = OPT_INPUT;
    } else if (!strcmp(argument, "--offset")) {
      bit = OPT_OFFSET;
    } else if (!strcmp(argument, "--length")) {
      bit = OPT_LENGTH;
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
      status = rights_parse(value, &options->rights);
      break;
    case OPT_NAME:
      options->name = value;
      status = pfs_name_validate((const uint8_t *)value, strlen(value));
      break;
    case OPT_INPUT:
      options->input = value;
      break;
    case OPT_OFFSET:
      status = decimal_count(value, &options->offset);
      if (options->offset > PFS_FILE_SIZE_MAX) {
        status = PFS_INVALID;
      }
      break;
    case OPT_LENGTH:
      status = decimal_count(value, &options->length);
      if (options->length > PFS_FILE_SIZE_MAX) {
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
  unsigned required = common;
  uint64_t file = options->rights.file;
  uint64_t directory = options->rights.directory;
  switch (options->command) {
  case WRITE_OPEN:
    break;
  case WRITE_CHECKPOINT:
    required |= authority;
    if (!((file == PFS_FILE_CHECKPOINT && !directory) ||
          (!file && directory == PFS_DIR_CHECKPOINT))) {
      return PFS_INVALID;
    }
    break;
  case WRITE_CREATE_FILE:
    required |= authority | OPT_NAME;
    if (file || directory != PFS_DIR_CREATE) {
      return PFS_INVALID;
    }
    break;
  case WRITE_FILE:
    required |= authority | OPT_INPUT | OPT_OFFSET;
    if (directory || !(file & PFS_FILE_WRITE) ||
        (file & ~(PFS_FILE_WRITE | PFS_FILE_RESIZE))) {
      return PFS_INVALID;
    }
    break;
  case WRITE_RESIZE:
    required |= authority | OPT_LENGTH;
    if (directory || file != PFS_FILE_RESIZE) {
      return PFS_INVALID;
    }
    break;
  case WRITE_NONE:
    return PFS_INVALID;
  }
  if ((seen & ~OPT_MEMORY) != required) {
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

static enum pfs_status
writer_random(void *context, void *buffer, size_t length)
{
  (void)context;
  uint8_t *bytes = buffer;
  while (length) {
    uint8_t random[PFS_ID_SIZE];
    enum pfs_status status = host_random_id(random);
    if (status != PFS_OK) {
      return status;
    }
    size_t count = length < sizeof(random) ? length : sizeof(random);
    memcpy(bytes, random, count);
    bytes += count;
    length -= count;
  }
  return PFS_OK;
}

static enum pfs_status
input_load(const struct write_options *options, const struct host_image *image,
           struct pfs_memory *memory, struct pfs_allocation *input)
{
  int fd = open(options->input, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "open input: %s\n", strerror(errno));
    return PFS_IO;
  }
  enum pfs_status status = PFS_OK;
  struct stat before;
  struct stat backing;
  if (fstat(fd, &before) || fstat(image->fd, &backing)) {
    fprintf(stderr, "stat input/image: %s\n", strerror(errno));
    status = PFS_IO;
  } else if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
             (before.st_dev == backing.st_dev && before.st_ino == backing.st_ino)) {
    status = PFS_INVALID;
  } else if ((uint64_t)before.st_size > WRITE_INPUT_MAX) {
    status = PFS_LIMIT;
  } else if ((uint64_t)before.st_size > PFS_FILE_SIZE_MAX - options->offset) {
    status = PFS_INVALID;
  } else if (flock(fd, LOCK_SH | LOCK_NB)) {
    status = errno == EWOULDBLOCK ? PFS_BUSY : PFS_IO;
    fprintf(stderr, "lock input: %s\n", strerror(errno));
  }
  if (status == PFS_OK && before.st_size) {
    status = pfs_memory_allocate(memory, (size_t)before.st_size, 1, input);
  }
  size_t done = 0;
  while (status == PFS_OK && done < input->size) {
    ssize_t count = pread(fd, (uint8_t *)input->data + done, input->size - done,
                          (off_t)done);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      if (count < 0) {
        fprintf(stderr, "read input: %s\n", strerror(errno));
      } else {
        fputs("read input: unexpected end of file\n", stderr);
      }
      status = PFS_IO;
    } else {
      done += (size_t)count;
    }
  }
  struct stat after;
  if (status == PFS_OK) {
    if (fstat(fd, &after)) {
      fprintf(stderr, "revalidate input: %s\n", strerror(errno));
      status = PFS_IO;
    } else if (before.st_size != after.st_size ||
               before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
               before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
               before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
               before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
      fputs("read input: source changed\n", stderr);
      status = PFS_BUSY;
    }
  }
  if (close(fd)) {
    fprintf(stderr, "close input: %s\n", strerror(errno));
    status = PFS_IO;
  }
  return status;
}

static void
report_result(const char *operation, const struct pfs_write_result *result)
{
  printf("%s: completion=%s operation=%s confirmed-bytes=%llu",
         operation, completion_name(result->completion),
         pfs_status_string(result->operation_status),
         (unsigned long long)result->confirmed_bytes);
  if (result->confirmed_length_valid) {
    printf(" confirmed-length=%llu", (unsigned long long)result->confirmed_length);
  } else {
    fputs(" confirmed-length=unavailable", stdout);
  }
  printf(" namespace-confirmed=%s health=%s maintenance=%s maintenance-status=%s\n",
         result->namespace_confirmed ? "true" : "false", health_name(result->health),
         maintenance_name(result->maintenance_completion),
         pfs_status_string(result->maintenance_status));
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
  struct pfs_allocation input = {0};
  struct pfs_write_open_result opened = {.pool = {.selected = PFS_POOL_NO_SELECTION}};
  options.limits.random = writer_random;
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
  if (status == PFS_OK && options.command != WRITE_OPEN) {
    operation = "open volume";
    status = pfs_pool_volume_open(&pool, &options.volume, &volume);
    struct pfs_trusted_context context = {
      .principal = options.principal,
      .root = options.object,
      .scope = PFS_SCOPE_OBJECT,
      .ceiling = options.rights,
    };
    if (status == PFS_OK) {
      operation = "acquire operation authority";
      status = pfs_view_acquire(&volume, &context, &options.object,
                                PFS_SCOPE_OBJECT, &options.rights, &view);
    }
    if (status == PFS_OK && options.command == WRITE_FILE) {
      operation = "load write input";
      status = input_load(&options, &image, &memory, &input);
    }
    if (status == PFS_OK) {
      struct pfs_write_result result;
      switch (options.command) {
      case WRITE_CHECKPOINT:
        operation = "checkpoint";
        status = pfs_view_checkpoint(view, &result);
        break;
      case WRITE_CREATE_FILE:
        operation = "create-file";
        status = pfs_view_create_file(view, (const uint8_t *)options.name,
                                     strlen(options.name), NULL, NULL, NULL, &result);
        break;
      case WRITE_FILE: {
        operation = "write";
        uint8_t empty = 0;
        status = pfs_view_write(view, options.offset, input.data ? input.data : &empty,
                               input.size, &result);
        break;
      }
      case WRITE_RESIZE:
        operation = "resize";
        status = pfs_view_resize(view, options.length, &result);
        break;
      case WRITE_OPEN:
      case WRITE_NONE:
        status = PFS_INVALID;
        break;
      }
      if (status != PFS_BUSY && status != PFS_INVALID) {
        report_result(operation, &result);
      }
    }
  }
  int exit_status = report_failure(operation, status, &image, 0);
  exit_status = report_failure("free input", pfs_memory_free(&memory, &input), NULL, exit_status);
  exit_status = report_failure("close view", pfs_view_close(&view), NULL, exit_status);
  exit_status = report_failure("close volume", pfs_volume_close(&volume), NULL, exit_status);
  exit_status = report_failure("close pool", pfs_pool_close(&pool), NULL, exit_status);
  exit_status = report_failure("close image", host_image_close(&image), &image, exit_status);
  return exit_status;
}
