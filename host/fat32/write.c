/* SPDX-License-Identifier: MPL-2.0 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <pyxis_fs/fat32_write.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOST_RECEIPTS 32u
#define HOST_FILE_BYTES (1024u * 1024u)
#define HOST_DEPTH 64u
#define HOST_LINE_BYTES 8192u
#define HOST_MEMORY_BYTES (32u * 1024u * 1024u)
#define HOST_STEPS UINT64_C(64000000)
#define HOST_SECTORS UINT64_C(4000000)

struct host_source {
  int fd, last_error;
  uint64_t sectors;
};

static bool source_read(void *context, uint64_t first, uint32_t count, void *bytes)
{
  struct host_source *source = context;
  if (!count || first >= source->sectors || count > source->sectors - first) {
    source->last_error = EINVAL;
    return false;
  }
  size_t remaining = (size_t)count * FAT32_SECTOR_BYTES;
  uint8_t *cursor = bytes;
  off_t offset = (off_t)(first * FAT32_SECTOR_BYTES);
  while (remaining) {
    ssize_t got = pread(source->fd, cursor, remaining, offset);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      source->last_error = got < 0 ? errno : EIO;
      return false;
    }
    cursor += got;
    offset += got;
    remaining -= (size_t)got;
  }
  return true;
}

static bool source_write(void *context, uint64_t first, uint32_t count, const void *bytes)
{
  struct host_source *source = context;
  if (!count || first >= source->sectors || count > source->sectors - first) {
    source->last_error = EINVAL;
    return false;
  }
  size_t remaining = (size_t)count * FAT32_SECTOR_BYTES;
  const uint8_t *cursor = bytes;
  off_t offset = (off_t)(first * FAT32_SECTOR_BYTES);
  while (remaining) {
    ssize_t got = pwrite(source->fd, cursor, remaining, offset);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      source->last_error = got < 0 ? errno : EIO;
      return false;
    }
    cursor += got;
    offset += got;
    remaining -= (size_t)got;
  }
  return true;
}

static bool source_flush(void *context)
{
  struct host_source *source = context;
  int result;
  do {
    result = fsync(source->fd);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    source->last_error = errno;
  }
  return result == 0;
}

static bool payload(void *context, uint32_t offset, void *bytes, size_t length)
{
  memcpy(bytes, (const uint8_t *)context + offset, length);
  return true;
}

static bool number(const char *text, uint32_t *value)
{
  if (!text || !*text || *text < '0' || *text > '9') {
    return false;
  }
  errno = 0;
  char *end;
  unsigned long parsed = strtoul(text, &end, 10);
  if (errno || *end || parsed > UINT32_MAX) {
    return false;
  }
  *value = (uint32_t)parsed;
  return true;
}

static bool output_bytes(void *context, const void *bytes, size_t length)
{
  (void)context;
  return fwrite(bytes, 1, length, stdout) == length;
}

static bool output_entry(void *context, const struct fat32_entry *entry)
{
  (void)context;
  return printf("%s\t%" PRIu32 "\t%s\talias=%s\n",
      entry->attributes & FAT32_ATTR_DIRECTORY ? "directory" : "file",
      entry->file_bytes, entry->name, entry->alias_ascii ? entry->alias : "<OEM>") >= 0;
}

static void recovery(const struct fat32_writer *writer)
{
  fprintf(stderr, "uncertain target=%s phase=%u failed_sector=%" PRIu64
      " old_first=%" PRIu32 " staged_first=%" PRIu32
      " staged_clusters=%" PRIu32 " allocated_clusters=%" PRIu32 "\n",
      writer->recovery.target, writer->recovery.phase, writer->recovery.failed_sector,
      writer->recovery.old_first, writer->recovery.staged_first,
      writer->recovery.staged_clusters, writer->recovery.allocated_clusters);
  for (uint32_t i = 0; i < writer->receipt_count; ++i) {
    const struct fat32_receipt *receipt = &writer->receipts[i];
    if (!receipt->valid) {
      continue;
    }
    fprintf(stderr, "receipt[%" PRIu32 "] target=%s chain=", i, receipt->path);
    for (uint32_t j = 0; j < receipt->chain_count; ++j) {
      fprintf(stderr, "%s%" PRIu32, j ? "," : "", receipt->chain[j]);
    }
    fputc('\n', stderr);
  }
  fprintf(stderr, "prepared_sectors=%zu\n", writer->recovery.prepared_sector_count);
  for (uint32_t i = 0; i < writer->recovery.allocated_clusters; ++i) {
    fprintf(stderr, "allocation[%" PRIu32 "]=%" PRIu32 "\n", i,
        writer->recovery.allocated_chain[i]);
  }
}

static enum fat32_status command(struct fat32_writer *writer, char *line)
{
  char *argument = strchr(line, ' ');
  if (argument) {
    *argument++ = 0;
  }
  char *data = argument ? strstr(argument, " = ") : NULL;
  if (data) {
    *data = 0;
    data += 3;
  }
  if (!strcmp(line, "check") && !argument) {
    struct fat32_check_report report;
    enum fat32_status status = fat32_check(&writer->volume, &writer->workspace, &report);
    if (status == FAT32_OK) {
      printf("files=%" PRIu64 " directories=%" PRIu64 " allocated=%" PRIu64
          " free=%" PRIu64 " ascii=%s\n", report.files, report.directories,
          report.allocated_clusters, report.free_clusters, report.all_names_ascii ? "yes" : "no");
    }
    return status;
  }
  if (!strcmp(line, "list") && argument && !data) {
    return fat32_list(&writer->volume, argument, strlen(argument), &writer->workspace,
        output_entry, NULL);
  }
  if (!strcmp(line, "read") && argument && !data) {
    return fat32_read(&writer->volume, argument, strlen(argument), &writer->workspace,
        output_bytes, NULL);
  }
  if ((!strcmp(line, "create") || !strcmp(line, "mkdir")) && argument) {
    bool directory = !strcmp(line, "mkdir");
    if ((directory && data) || (!directory && !data)) {
      return FAT32_INVALID;
    }
    struct fat32_receipt *receipt;
    enum fat32_status status = fat32_create(writer, argument, strlen(argument), directory,
        data ? payload : NULL, data, data ? (uint32_t)strlen(data) : 0, &receipt);
    if (status == FAT32_OK) {
      printf("receipt=%zu bytes=%" PRIu32 "\n", (size_t)(receipt - writer->receipts),
          receipt->file_bytes);
    }
    return status;
  }
  if ((!strcmp(line, "append") || !strcmp(line, "replace")) && argument && data) {
    uint32_t id;
    if (!number(argument, &id) || id >= writer->receipt_count) {
      return FAT32_INVALID;
    }
    return !strcmp(line, "append") ?
        fat32_append(writer, &writer->receipts[id], payload, data, (uint32_t)strlen(data)) :
        fat32_replace(writer, &writer->receipts[id], payload, data, (uint32_t)strlen(data));
  }
  if (!strcmp(line, "extend") && argument && !data) {
    char *length = strchr(argument, ' ');
    uint32_t id, bytes;
    if (!length) {
      return FAT32_INVALID;
    }
    *length++ = 0;
    if (!number(argument, &id) || !number(length, &bytes) || id >= writer->receipt_count) {
      return FAT32_INVALID;
    }
    return fat32_extend(writer, &writer->receipts[id], bytes);
  }
  return FAT32_INVALID;
}

int main(int argc, char **argv)
{
  uint32_t partition;
  if (argc != 5 || strcmp(argv[1], "--image") || strcmp(argv[3], "--gpt-partition") ||
      !number(argv[4], &partition) || !partition) {
    fputs("usage: fat32-write --image DISPOSABLE-REGULAR-IMAGE --gpt-partition N\n"
        "stdin session commands: check, list /path, read /path, mkdir /path,\n"
        "create /path = payload, append ID = payload, replace ID = payload,\n"
        "extend ID LENGTH, quit. Paths/payload may contain spaces.\n"
        "Limits: 32 receipts, 1MiB files, 8191-byte command lines, depth64, 32MiB workspace.\n",
        stderr);
    return 2;
  }
  struct host_source source = { .fd = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW) };
  if (source.fd < 0) {
    fprintf(stderr, "fat32-write: open: %s\n", strerror(errno));
    return 1;
  }
  struct stat info;
  int result = 1;
  void *memory = NULL;
  uint32_t *chains = NULL;
  if (fstat(source.fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      info.st_size % FAT32_SECTOR_BYTES || flock(source.fd, LOCK_EX | LOCK_NB)) {
    fputs("fat32-write: requires an exclusively advisory-locked, regular 512-byte image\n",
        stderr);
    goto finish;
  }
  source.sectors = (uint64_t)info.st_size / FAT32_SECTOR_BYTES;
  struct fat32_source input = {
    .read = source_read, .context = &source, .sectors = source.sectors,
    .sector_bytes = FAT32_SECTOR_BYTES, .max_read_sectors = 128,
  };
  struct fat32_workspace workspace = {
    .bytes = fat32_open_workspace_size(), .step_limit = HOST_STEPS,
    .sector_limit = HOST_SECTORS, .directory_depth_limit = HOST_DEPTH,
  };
  memory = malloc(workspace.bytes);
  if (!memory) {
    goto finish;
  }
  workspace.memory = memory;
  struct fat32_volume volume;
  enum fat32_status status = fat32_open(&input, partition, &workspace, &volume);
  if (status != FAT32_OK) {
    fprintf(stderr, "fat32-write: open: %s\n", fat32_status_string(status));
    if (status == FAT32_IO && source.last_error) {
      fprintf(stderr, "fat32-write: backing I/O: %s\n", strerror(source.last_error));
    }
    goto finish;
  }
  size_t bytes;
  status = fat32_write_workspace_size(&volume, HOST_DEPTH, HOST_FILE_BYTES, &bytes);
  if (status != FAT32_OK || bytes > HOST_MEMORY_BYTES) {
    fputs("fat32-write: workspace budget exceeded\n", stderr);
    goto finish;
  }
  free(memory);
  memory = malloc(bytes);
  if (!memory) {
    goto finish;
  }
  workspace.memory = memory;
  workspace.bytes = bytes;
  uint32_t capacity = (HOST_FILE_BYTES + volume.sectors_per_cluster * 512 - 1) /
      (volume.sectors_per_cluster * 512);
  chains = calloc((size_t)HOST_RECEIPTS * capacity, sizeof(*chains));
  if (!chains) {
    goto finish;
  }
  struct fat32_receipt receipts[HOST_RECEIPTS] = {0};
  for (unsigned i = 0; i < HOST_RECEIPTS; ++i) {
    receipts[i].chain = chains + (size_t)i * capacity;
    receipts[i].chain_capacity = capacity;
  }
  struct fat32_write_io io = { .write = source_write, .flush = source_flush, .context = &source };
  struct fat32_writer writer;
  status = fat32_writer_open(&writer, &volume, &io, &workspace, HOST_FILE_BYTES,
      receipts, HOST_RECEIPTS);
  if (status != FAT32_OK) {
    fprintf(stderr, "fat32-write: session: %s\n", fat32_status_string(status));
    goto finish;
  }
  bool interactive = isatty(STDIN_FILENO);
  char line[HOST_LINE_BYTES];
  result = 0;
  for (;;) {
    if (interactive) {
      fputs("fat32-write> ", stdout);
      fflush(stdout);
    }
    if (!fgets(line, sizeof(line), stdin)) {
      if (ferror(stdin)) {
        result = 1;
      }
      break;
    }
    size_t length = strlen(line);
    if (!length || (line[length - 1] != '\n' && !feof(stdin))) {
      fputs("fat32-write: command line too long; session ended\n", stderr);
      result = 1;
      break;
    }
    if (line[length - 1] == '\n') {
      line[--length] = 0;
    }
    if (!strcmp(line, "quit")) {
      break;
    }
    status = command(&writer, line);
    fprintf(status == FAT32_OK ? stdout : stderr, "%s\n", fat32_status_string(status));
    if (status != FAT32_OK) {
      result = 1;
    }
    if (status == FAT32_IO && source.last_error) {
      fprintf(stderr, "fat32-write: backing I/O: %s\n", strerror(source.last_error));
    }
    if (writer.stopped) {
      recovery(&writer);
      break;
    }
  }
finish:
  free(chains);
  free(memory);
  if (close(source.fd)) {
    result = 1;
  }
  return result;
}
