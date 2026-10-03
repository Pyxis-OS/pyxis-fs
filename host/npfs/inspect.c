/* SPDX-License-Identifier: MPL-2.0 */
#include "host.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct inspect_options {
  const char *image;
  const char *command;
  const char *volume;
  const char *path;
  const char *output;
};

static int
usage(void)
{
  fputs("usage: npfs-inspect --image PATH info|volumes\n"
        "       npfs-inspect --image PATH list|stat|cat --volume NAME [--path PATH]\n"
        "       npfs-inspect --image PATH extract --volume NAME --path PATH --output FILE\n",
        stderr);
  return 2;
}

static enum npfs_status
inspect_error(struct npfs_image *image, const char *message)
{
  snprintf(image->error, sizeof(image->error), "%s", message);
  return NPFS_CORRUPT;
}

static enum npfs_status
inspect_inode(struct npfs_image *image, const struct npfs_volume *volume,
              uint64_t number, struct npfs_inode *inode)
{
  enum npfs_status status = npfs_read_inode(image, volume, number, inode);
  if (status != NPFS_OK)
    return status;
  if (inode->kind == NPFS_INODE_FREE || inode->cleanup & NPFS_CLEANUP_DETACHED)
    return inspect_error(image, "namespace references free or detached inode");
  if (number == 1 && (inode->kind != NPFS_INODE_DIRECTORY || inode->parent != 1))
    return inspect_error(image, "invalid root inode");
  if (inode->kind == NPFS_INODE_FILE && inode->parent)
    return inspect_error(image, "regular inode has directory parent");
  return NPFS_OK;
}

typedef enum npfs_status (*inspect_entry)(struct npfs_image *, const struct npfs_dirent *,
                                        const struct npfs_inode *, void *);

static enum npfs_status
inspect_directory(struct npfs_image *image, const struct npfs_volume *volume,
                  uint64_t number, const struct npfs_inode *directory,
                  inspect_entry visit, void *context)
{
  if (directory->kind != NPFS_INODE_DIRECTORY)
    return NPFS_INVALID;
  uint8_t block[NPFS_BLOCK_SIZE];
  for (uint64_t logical = 0; logical < directory->size / NPFS_BLOCK_SIZE; logical++) {
    uint64_t physical;
    enum npfs_status status = npfs_map_block(image, directory->pointers, logical, &physical);
    if (status != NPFS_OK)
      return status;
    if (!physical)
      return inspect_error(image, "hole in directory mapping");
    status = npfs_read_blocks(image, physical, 1, block);
    if (status != NPFS_OK)
      return status;
    size_t offset = 0;
    while (offset < NPFS_BLOCK_SIZE) {
      struct npfs_dirent entry;
      status = npfs_dirent_decode(&image->header, block + offset,
                                 NPFS_BLOCK_SIZE - offset, &entry);
      if (status != NPFS_OK)
        return status;
      offset += entry.record_length;
      if (!entry.inode)
        continue;
      struct npfs_inode inode;
      status = inspect_inode(image, volume, entry.inode, &inode);
      if (status != NPFS_OK)
        return status;
      if (entry.inode == 1 || (inode.kind == NPFS_INODE_DIRECTORY && inode.parent != number))
        return inspect_error(image, "invalid directory backlink");
      status = visit(image, &entry, &inode, context);
      if (status != NPFS_OK)
        return status;
    }
  }
  return NPFS_OK;
}

struct lookup_context {
  const uint8_t *name;
  size_t length;
  uint64_t number;
  struct npfs_inode inode;
};

static enum npfs_status
lookup_entry(struct npfs_image *image, const struct npfs_dirent *entry,
             const struct npfs_inode *inode, void *context)
{
  struct lookup_context *lookup = context;
  if (entry->name_length != lookup->length || memcmp(entry->name, lookup->name, lookup->length))
    return NPFS_OK;
  if (lookup->number)
    return inspect_error(image, "duplicate name during lookup");
  lookup->number = entry->inode;
  lookup->inode = *inode;
  return NPFS_OK;
}

static enum npfs_status
resolve_path(struct npfs_image *image, const struct npfs_volume *volume, const char *path,
             uint64_t *number, struct npfs_inode *inode)
{
  *number = volume->root_inode;
  enum npfs_status status = inspect_inode(image, volume, *number, inode);
  if (status != NPFS_OK)
    return status;
  if (!strcmp(path, ".") || !*path)
    return NPFS_OK;
  if (*path == '/') {
    snprintf(image->error, sizeof(image->error), "paths are relative to the volume root");
    return NPFS_INVALID;
  }
  const char *component = path;
  while (*component) {
    const char *end = strchr(component, '/');
    size_t length = end ? (size_t)(end - component) : strlen(component);
    if (!npfs_name_valid((const uint8_t *)component, length))
      return NPFS_INVALID;
    struct lookup_context lookup = { .name = (const uint8_t *)component, .length = length };
    status = inspect_directory(image, volume, *number, inode, lookup_entry, &lookup);
    if (status != NPFS_OK)
      return status;
    if (!lookup.number)
      return NPFS_NOT_FOUND;
    *number = lookup.number;
    *inode = lookup.inode;
    if (!end)
      break;
    component = end + 1;
    if (!*component)
      return NPFS_INVALID;
  }
  return NPFS_OK;
}

static void
print_info(const struct npfs_image *image)
{
  fputs("pool ", stdout);
  npfs_id_print(stdout, image->header.pool_id);
  printf("\nblocks %llu\nblock_size %u\nbitmap %llu %llu\nvolume_table %llu %llu\n"
         "journal %llu %llu\njournal_sequence %llu\njournal_state empty\n"
         "compatible 0x%llx\nread_only_compatible 0x%llx\nrequired 0x%llx\n"
         "header_copies %s\n",
         (unsigned long long)image->header.pool_blocks, NPFS_BLOCK_SIZE,
         (unsigned long long)image->header.bitmap_start,
         (unsigned long long)image->header.bitmap_blocks,
         (unsigned long long)image->header.volume_start,
         (unsigned long long)image->header.volume_blocks,
         (unsigned long long)image->header.journal_start,
         (unsigned long long)image->header.journal_blocks,
         (unsigned long long)image->control.sequence,
         (unsigned long long)image->header.compatible,
         (unsigned long long)image->header.read_only_compatible,
         (unsigned long long)image->header.required,
         image->degraded_header ? "one valid" : "both valid");
}

static void
print_volumes(const struct npfs_volume volumes[NPFS_VOLUME_COUNT])
{
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; i++) {
    const struct npfs_volume *volume = &volumes[i];
    if (volume->state != NPFS_VOLUME_LIVE)
      continue;
    printf("slot %u name ", i);
    npfs_name_print(stdout, volume->name, volume->name_length);
    fputs(" id ", stdout);
    npfs_id_print(stdout, volume->id);
    printf(" inode_bytes %llu cleanup_head %llu\n",
           (unsigned long long)volume->inode_bytes, (unsigned long long)volume->cleanup_head);
  }
}

static void
print_stat(uint64_t number, const struct npfs_inode *inode)
{
  printf("inode %llu\nkind %s\nsize %llu\n",
         (unsigned long long)number,
         inode->kind == NPFS_INODE_DIRECTORY ? "directory" : "file",
         (unsigned long long)inode->size);
  if (inode->kind == NPFS_INODE_DIRECTORY)
    printf("parent %llu\n", (unsigned long long)inode->parent);
  if (inode->flags & NPFS_TIME_CREATED_VALID)
    printf("created_ns %lld\n", (long long)inode->created_ns);
  else
    puts("created_ns unknown");
  if (inode->flags & NPFS_TIME_MODIFIED_VALID)
    printf("modified_ns %lld\n", (long long)inode->modified_ns);
  else
    puts("modified_ns unknown");
  printf("cleanup 0x%x\ncleanup_next %llu\nshrink_target %llu\n", inode->cleanup,
         (unsigned long long)inode->cleanup_next, (unsigned long long)inode->shrink_target);
}

static enum npfs_status
list_entry(struct npfs_image *image, const struct npfs_dirent *entry,
           const struct npfs_inode *inode, void *context)
{
  (void)image;
  (void)context;
  printf("%s %llu %llu ", inode->kind == NPFS_INODE_DIRECTORY ? "directory" : "file",
         (unsigned long long)entry->inode, (unsigned long long)inode->size);
  npfs_name_print(stdout, entry->name, entry->name_length);
  putchar('\n');
  return ferror(stdout) ? NPFS_IO : NPFS_OK;
}

static enum npfs_status
copy_file(struct npfs_image *image, const struct npfs_inode *inode, FILE *output)
{
  if (inode->kind != NPFS_INODE_FILE)
    return NPFS_INVALID;
  uint8_t bytes[64 * 1024];
  uint64_t offset = 0;
  while (offset < inode->size) {
    uint64_t remaining = inode->size - offset;
    size_t count = remaining < sizeof(bytes) ? (size_t)remaining : sizeof(bytes);
    enum npfs_status status = npfs_read_file(image, inode->pointers, inode->size,
                                             offset, bytes, count);
    if (status != NPFS_OK)
      return status;
    if (fwrite(bytes, 1, count, output) != count) {
      snprintf(image->error, sizeof(image->error), "output write: %s", strerror(errno));
      return NPFS_IO;
    }
    offset += count;
  }
  return NPFS_OK;
}

int
main(int argc, char **argv)
{
  struct inspect_options options = { .path = "." };
  bool path_given = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--image") && i + 1 < argc && !options.image)
      options.image = argv[++i];
    else if (!strcmp(argv[i], "--volume") && i + 1 < argc && !options.volume)
      options.volume = argv[++i];
    else if (!strcmp(argv[i], "--path") && i + 1 < argc && !path_given) {
      options.path = argv[++i];
      path_given = true;
    } else if (!strcmp(argv[i], "--output") && i + 1 < argc && !options.output)
      options.output = argv[++i];
    else if (argv[i][0] != '-' && !options.command)
      options.command = argv[i];
    else
      return usage();
  }
  if (!options.image || !options.command)
    return usage();
  bool info = !strcmp(options.command, "info");
  bool volumes_command = !strcmp(options.command, "volumes");
  bool list = !strcmp(options.command, "list");
  bool stat = !strcmp(options.command, "stat");
  bool cat = !strcmp(options.command, "cat");
  bool extract = !strcmp(options.command, "extract");
  if ((!info && !volumes_command && !list && !stat && !cat && !extract) ||
      ((info || volumes_command) && (options.volume || path_given || options.output)) ||
      ((!info && !volumes_command) && !options.volume) ||
      (extract && (!options.output || !path_given)) || (!extract && options.output))
    return usage();
  struct npfs_image image;
  enum npfs_status status = npfs_image_open(&image, options.image, false, false);
  if (status != NPFS_OK)
    return npfs_report("open", status, &image);
  if (info) {
    print_info(&image);
    goto done;
  }
  struct npfs_volume volumes[NPFS_VOLUME_COUNT];
  status = npfs_read_volumes(&image, volumes);
  if (status != NPFS_OK)
    goto done;
  if (volumes_command) {
    print_volumes(volumes);
    goto done;
  }
  struct npfs_volume *volume = NULL;
  size_t name_length = strlen(options.volume);
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; i++) {
    if (volumes[i].state == NPFS_VOLUME_LIVE && volumes[i].name_length == name_length &&
        !memcmp(volumes[i].name, options.volume, name_length)) {
      volume = &volumes[i];
      break;
    }
  }
  if (!volume) {
    status = NPFS_NOT_FOUND;
    goto done;
  }
  uint64_t number;
  struct npfs_inode inode;
  status = resolve_path(&image, volume, options.path, &number, &inode);
  if (status != NPFS_OK)
    goto done;
  if (list)
    status = inspect_directory(&image, volume, number, &inode, list_entry, NULL);
  else if (stat)
    print_stat(number, &inode);
  else if (cat)
    status = copy_file(&image, &inode, stdout);
  else {
    if (inode.kind != NPFS_INODE_FILE) {
      status = NPFS_INVALID;
      goto done;
    }
    FILE *output = fopen(options.output, "wbx");
    if (!output) {
      snprintf(image.error, sizeof(image.error), "create output: %s", strerror(errno));
      status = NPFS_IO;
      goto done;
    }
    status = copy_file(&image, &inode, output);
    if (fclose(output) && status == NPFS_OK) {
      snprintf(image.error, sizeof(image.error), "close output: %s", strerror(errno));
      status = NPFS_IO;
    }
    if (status != NPFS_OK)
      unlink(options.output);
  }
done:
  if (status == NPFS_OK && fflush(stdout)) {
    snprintf(image.error, sizeof(image.error), "flush output: %s", strerror(errno));
    status = NPFS_IO;
  }
  int result = status == NPFS_OK ? 0 : npfs_report(options.command, status, &image);
  enum npfs_status close_status = npfs_image_close(&image);
  if (!result && close_status != NPFS_OK)
    result = npfs_report("close", close_status, &image);
  return result;
}
