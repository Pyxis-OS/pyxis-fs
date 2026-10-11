/* SPDX-License-Identifier: MPL-2.0 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <pyxis_fs/fat32.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

static const size_t default_memory_budget = 32u * 1024u * 1024u;
static const uint64_t default_step_budget = 64000000;
static const uint64_t default_sector_budget = 4000000;
static const uint32_t default_depth = 64;
static const uint32_t host_max_read_sectors = 128;

struct inspect_options {
  const char *image, *command, *path;
  size_t memory_budget;
  uint64_t step_budget, sector_budget;
  uint32_t gpt_partition, depth;
};

struct inspect_source {
  int fd, error;
  uint64_t bytes, failed_sector;
  uint32_t failed_count;
  bool failed;
};

static int
usage(void)
{
  fputs("usage: fat32-inspect --image PATH --gpt-partition N\n"
        "       [--memory-budget SIZE --step-budget N --sector-budget N --depth N]\n"
        "       info|check|list /PATH|read /PATH\n"
        "SIZE is bytes or a value suffixed KiB, MiB, GiB, KB, MB or GB.\n", stderr);
  return 2;
}

static bool
parse_number(const char *text, bool size, uint64_t *value)
{
  if (*text < '0' || *text > '9')
    return false;
  errno = 0;
  char *end;
  unsigned long long number = strtoull(text, &end, 10);
  if (errno == ERANGE || number > UINT64_MAX)
    return false;
  uint64_t multiplier = 1;
  if (*end) {
    if (!size)
      return false;
    if (!strcmp(end, "KiB"))
      multiplier = 1024;
    else if (!strcmp(end, "MiB"))
      multiplier = 1024 * 1024;
    else if (!strcmp(end, "GiB"))
      multiplier = 1024ull * 1024 * 1024;
    else if (!strcmp(end, "KB"))
      multiplier = 1000;
    else if (!strcmp(end, "MB"))
      multiplier = 1000 * 1000;
    else if (!strcmp(end, "GB"))
      multiplier = 1000ull * 1000 * 1000;
    else
      return false;
  }
  if (!number || number > UINT64_MAX / multiplier)
    return false;
  *value = number * multiplier;
  return true;
}

static bool
parse_options(int argc, char **argv, struct inspect_options *options)
{
  *options = (struct inspect_options) {
    .memory_budget = default_memory_budget,
    .step_budget = default_step_budget,
    .sector_budget = default_sector_budget,
    .depth = default_depth,
  };
  int i = 1;
  for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
    const char *option = argv[i];
    if (++i == argc)
      return false;
    if (!strcmp(option, "--image")) {
      if (options->image)
        return false;
      options->image = argv[i];
      continue;
    }
    uint64_t value;
    if (!parse_number(argv[i], !strcmp(option, "--memory-budget"), &value))
      return false;
    if (!strcmp(option, "--gpt-partition")) {
      if (options->gpt_partition || value > UINT32_MAX)
        return false;
      options->gpt_partition = (uint32_t)value;
    } else if (!strcmp(option, "--memory-budget")) {
      if (value > SIZE_MAX)
        return false;
      options->memory_budget = (size_t)value;
    } else if (!strcmp(option, "--step-budget")) {
      options->step_budget = value;
    } else if (!strcmp(option, "--sector-budget")) {
      options->sector_budget = value;
    } else if (!strcmp(option, "--depth")) {
      if (value > UINT32_MAX)
        return false;
      options->depth = (uint32_t)value;
    } else {
      return false;
    }
  }
  if (!options->image || !options->gpt_partition || i == argc)
    return false;
  options->command = argv[i++];
  if (!strcmp(options->command, "list") || !strcmp(options->command, "read")) {
    if (i == argc || argv[i][0] != '/')
      return false;
    options->path = argv[i++];
  } else if (strcmp(options->command, "info") && strcmp(options->command, "check")) {
    return false;
  }
  return i == argc;
}

static bool
open_source(const char *path, struct inspect_source *source)
{
  *source = (struct inspect_source) { .fd = -1 };
  source->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (source->fd < 0) {
    fprintf(stderr, "fat32-inspect: open %s: %s\n", path, strerror(errno));
    return false;
  }
  struct stat stat;
  if (fstat(source->fd, &stat)) {
    fprintf(stderr, "fat32-inspect: stat %s: %s\n", path, strerror(errno));
    return false;
  }
  if (S_ISREG(stat.st_mode)) {
    if (stat.st_size < 0) {
      fputs("fat32-inspect: negative image size\n", stderr);
      return false;
    }
    source->bytes = (uint64_t)stat.st_size;
  } else if (S_ISBLK(stat.st_mode)) {
#ifdef __linux__
    int sector_bytes;
    if (ioctl(source->fd, BLKGETSIZE64, &source->bytes) ||
        ioctl(source->fd, BLKSSZGET, &sector_bytes)) {
      fprintf(stderr, "fat32-inspect: block geometry %s: %s\n", path, strerror(errno));
      return false;
    }
    if (sector_bytes != FAT32_SECTOR_BYTES) {
      fputs("fat32-inspect: unsupported device logical sector size\n", stderr);
      return false;
    }
#else
    fputs("fat32-inspect: block devices require Linux geometry ioctls\n", stderr);
    return false;
#endif
  } else {
    fputs("fat32-inspect: source must be a regular image or block device\n", stderr);
    return false;
  }
  if (!source->bytes || source->bytes % FAT32_SECTOR_BYTES || source->bytes > INT64_MAX) {
    fputs("fat32-inspect: source size must be nonzero, 512-byte aligned and fit host offsets\n",
          stderr);
    return false;
  }
  return true;
}

static bool
read_sectors(void *context, uint64_t first_sector, uint32_t sector_count, void *bytes)
{
  struct inspect_source *source = context;
  uint64_t sectors = source->bytes / FAT32_SECTOR_BYTES;
  if (!sector_count || sector_count > host_max_read_sectors || first_sector > sectors ||
      sector_count > sectors - first_sector) {
    source->error = EINVAL;
    goto fail;
  }
  size_t remaining = (size_t)sector_count * FAT32_SECTOR_BYTES;
  uint64_t offset = first_sector * FAT32_SECTOR_BYTES;
  unsigned char *cursor = bytes;
  while (remaining) {
    ssize_t got = pread(source->fd, cursor, remaining, (off_t)offset);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      source->error = errno;
      goto fail;
    }
    if (!got) {
      source->error = 0;
      goto fail;
    }
    cursor += got;
    offset += (uint64_t)got;
    remaining -= (size_t)got;
  }
  return true;
fail:
  source->failed = true;
  source->failed_sector = first_sector;
  source->failed_count = sector_count;
  return false;
}

static const char *
status_name(enum fat32_status status)
{
  switch (status) {
  case FAT32_OK: return "ok";
  case FAT32_INVALID: return "invalid argument or path";
  case FAT32_CORRUPT: return "corrupt metadata";
  case FAT32_UNSUPPORTED: return "unsupported format, geometry or name";
  case FAT32_IO: return "source read failed";
  case FAT32_NO_MEMORY: return "workspace is too small";
  case FAT32_LIMIT: return "work or depth budget exhausted";
  case FAT32_NAME_TOO_LONG: return "name exceeds the supported byte bound";
  case FAT32_NOT_FOUND: return "path not found";
  case FAT32_NOT_DIRECTORY: return "path requires a directory";
  case FAT32_IS_DIRECTORY: return "read requires a regular file";
  case FAT32_OUTPUT: return "output failed";
  }
  return "unknown reader status";
}

static bool
write_bytes(void *context, const void *bytes, size_t length)
{
  FILE *output = context;
  return fwrite(bytes, 1, length, output) == length;
}

static bool
write_entry(void *context, const struct fat32_entry *entry)
{
  FILE *output = context;
  if (fprintf(output, "%s\t%" PRIu32 "\t",
              entry->attributes & FAT32_ATTR_DIRECTORY ? "directory" : "file",
              entry->file_bytes) < 0 ||
      fwrite(entry->name, 1, entry->name_bytes, output) != entry->name_bytes)
    return false;
  if (entry->alias_ascii && entry->alias_bytes) {
    if (fputs("\talias=", output) == EOF ||
        fwrite(entry->alias, 1, entry->alias_bytes, output) != entry->alias_bytes)
      return false;
  }
  return fputc('\n', output) != EOF;
}

static bool
write_info(const struct fat32_volume *volume)
{
  return printf("gpt_partition=%" PRIu32 "\npartition_first_sector=%" PRIu64
                "\npartition_sectors=%" PRIu64 "\nvolume_sectors=%" PRIu32
                "\nsector_bytes=%u\nsectors_per_cluster=%u\nfat_count=%u"
                "\nfat_sectors=%" PRIu32 "\nclusters=%" PRIu32
                "\nroot_cluster=%" PRIu32 "\nclean_shutdown=%s\nno_hard_error=%s\n",
                volume->gpt_entry, volume->partition_first, volume->partition_sectors,
                volume->volume_sectors, FAT32_SECTOR_BYTES, volume->sectors_per_cluster,
                volume->fat_count, volume->fat_sectors, volume->cluster_count,
                volume->root_cluster, volume->clean_shutdown ? "yes" : "no",
                volume->no_hard_error ? "yes" : "no") >= 0;
}

static bool
write_report(const struct fat32_check_report *report)
{
  return printf("files=%" PRIu64 "\ndirectories=%" PRIu64 "\nallocated_clusters=%" PRIu64
                "\nfree_clusters=%" PRIu64 "\nbad_clusters=%" PRIu64
                "\nall_names_ascii=%s\n", report->files, report->directories,
                report->allocated_clusters, report->free_clusters, report->bad_clusters,
                report->all_names_ascii ? "yes" : "no") >= 0;
}

int
main(int argc, char **argv)
{
  struct inspect_options options;
  if (!parse_options(argc, argv, &options))
    return usage();
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fprintf(stderr, "fat32-inspect: configure stdout signal handling: %s\n", strerror(errno));
    return 1;
  }
  struct inspect_source source;
  if (!open_source(options.image, &source)) {
    if (source.fd >= 0)
      close(source.fd);
    return 1;
  }
  struct fat32_source input = {
    .read = read_sectors, .context = &source,
    .sectors = source.bytes / FAT32_SECTOR_BYTES,
    .sector_bytes = FAT32_SECTOR_BYTES, .max_read_sectors = host_max_read_sectors,
  };
  struct fat32_workspace workspace = {
    .bytes = fat32_open_workspace_size(),
    .step_limit = options.step_budget, .sector_limit = options.sector_budget,
    .directory_depth_limit = options.depth,
  };
  enum fat32_status status = FAT32_NO_MEMORY;
  if (workspace.bytes > options.memory_budget) {
    fprintf(stderr, "fat32-inspect: open requires %zu workspace bytes; budget is %zu\n",
            workspace.bytes, options.memory_budget);
    goto finish;
  }
  workspace.memory = malloc(workspace.bytes);
  if (!workspace.memory)
    goto finish;
  struct fat32_volume volume;
  status = fat32_open(&input, options.gpt_partition, &workspace, &volume);
  if (status != FAT32_OK)
    goto finish;
  if (!strcmp(options.command, "info")) {
    if (!write_info(&volume))
      status = FAT32_OUTPUT;
    goto finish;
  }
  size_t bytes;
  status = fat32_workspace_size(&volume, options.depth, &bytes);
  if (status != FAT32_OK)
    goto finish;
  if (bytes > options.memory_budget) {
    fprintf(stderr, "fat32-inspect: operation requires %zu workspace bytes; budget is %zu\n",
            bytes, options.memory_budget);
    status = FAT32_NO_MEMORY;
    goto finish;
  }
  if (bytes > workspace.bytes) {
    free(workspace.memory);
    workspace.memory = malloc(bytes);
    if (!workspace.memory) {
      status = FAT32_NO_MEMORY;
      goto finish;
    }
    workspace.bytes = bytes;
  }
  if (!strcmp(options.command, "check")) {
    struct fat32_check_report report;
    status = fat32_check(&volume, &workspace, &report);
    if (status == FAT32_OK && !write_report(&report))
      status = FAT32_OUTPUT;
  } else if (!strcmp(options.command, "list")) {
    status = fat32_list(&volume, options.path, strlen(options.path), &workspace,
                        write_entry, stdout);
  } else {
    status = fat32_read(&volume, options.path, strlen(options.path), &workspace,
                        write_bytes, stdout);
  }
finish:
  free(workspace.memory);
  int output_error = 0;
  if (fclose(stdout)) {
    output_error = errno;
    if (status == FAT32_OK)
      status = FAT32_OUTPUT;
  }
  if (close(source.fd)) {
    fprintf(stderr, "fat32-inspect: source close: %s\n", strerror(errno));
    if (status == FAT32_OK)
      status = FAT32_IO;
  }
  if (status != FAT32_OK) {
    fprintf(stderr, "fat32-inspect: %s: %s\n", options.command, status_name(status));
    if (source.failed)
      fprintf(stderr, "fat32-inspect: source sectors %" PRIu64 " + %" PRIu32 ": %s\n",
              source.failed_sector, source.failed_count,
              source.error ? strerror(source.error) : "unexpected end of source");
    if (output_error)
      fprintf(stderr, "fat32-inspect: stdout: %s\n", strerror(output_error));
  }
  return status == FAT32_OK ? 0 : 1;
}
