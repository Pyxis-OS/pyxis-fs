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
  fputs("usage: pyxisfs-native-inspect --image PATH info|volumes\n"
        "       pyxisfs-native-inspect --image PATH list|stat|cat --volume NAME [--path PATH]\n"
        "       pyxisfs-native-inspect --image PATH extract --volume NAME --path PATH --output FILE\n",
        stderr);
  return 2;
}

static enum pnf_status
inspect_error(struct native_image *image, const char *message)
{
  snprintf(image->error, sizeof(image->error), "%s", message);
  return PNF_CORRUPT;
}

static enum pnf_status
inspect_inode(struct native_image *image, const struct pnf_volume *volume,
              uint64_t number, struct pnf_inode *inode)
{
  enum pnf_status status = native_read_inode(image, volume, number, inode);
  if (status != PNF_OK)
    return status;
  if (inode->kind == PNF_INODE_FREE || inode->cleanup & PNF_CLEANUP_DETACHED)
    return inspect_error(image, "namespace references free or detached inode");
  if (number == 1 && (inode->kind != PNF_INODE_DIRECTORY || inode->parent != 1))
    return inspect_error(image, "invalid root inode");
  if (inode->kind == PNF_INODE_FILE && inode->parent)
    return inspect_error(image, "regular inode has directory parent");
  return PNF_OK;
}

typedef enum pnf_status (*inspect_entry)(struct native_image *, const struct pnf_dirent *,
                                        const struct pnf_inode *, void *);

static enum pnf_status
inspect_directory(struct native_image *image, const struct pnf_volume *volume,
                  uint64_t number, const struct pnf_inode *directory,
                  inspect_entry visit, void *context)
{
  if (directory->kind != PNF_INODE_DIRECTORY)
    return PNF_INVALID;
  uint8_t block[PNF_BLOCK_SIZE];
  for (uint64_t logical = 0; logical < directory->size / PNF_BLOCK_SIZE; logical++) {
    uint64_t physical;
    enum pnf_status status = native_map_block(image, directory->pointers, logical, &physical);
    if (status != PNF_OK)
      return status;
    if (!physical)
      return inspect_error(image, "hole in directory mapping");
    status = native_read_blocks(image, physical, 1, block);
    if (status != PNF_OK)
      return status;
    size_t offset = 0;
    while (offset < PNF_BLOCK_SIZE) {
      struct pnf_dirent entry;
      status = pnf_dirent_decode(block + offset, PNF_BLOCK_SIZE - offset, &entry);
      if (status != PNF_OK)
        return status;
      offset += entry.record_length;
      if (!entry.inode)
        continue;
      struct pnf_inode inode;
      status = inspect_inode(image, volume, entry.inode, &inode);
      if (status != PNF_OK)
        return status;
      if (entry.inode == 1 || (inode.kind == PNF_INODE_DIRECTORY && inode.parent != number))
        return inspect_error(image, "invalid directory backlink");
      status = visit(image, &entry, &inode, context);
      if (status != PNF_OK)
        return status;
    }
  }
  return PNF_OK;
}

struct lookup_context {
  const uint8_t *name;
  size_t length;
  uint64_t number;
  struct pnf_inode inode;
};

static enum pnf_status
lookup_entry(struct native_image *image, const struct pnf_dirent *entry,
             const struct pnf_inode *inode, void *context)
{
  struct lookup_context *lookup = context;
  if (entry->name_length != lookup->length || memcmp(entry->name, lookup->name, lookup->length))
    return PNF_OK;
  if (lookup->number)
    return inspect_error(image, "duplicate name during lookup");
  lookup->number = entry->inode;
  lookup->inode = *inode;
  return PNF_OK;
}

static enum pnf_status
resolve_path(struct native_image *image, const struct pnf_volume *volume, const char *path,
             uint64_t *number, struct pnf_inode *inode)
{
  *number = volume->root_inode;
  enum pnf_status status = inspect_inode(image, volume, *number, inode);
  if (status != PNF_OK)
    return status;
  if (!strcmp(path, ".") || !*path)
    return PNF_OK;
  if (*path == '/')
    return PNF_INVALID;
  const char *component = path;
  while (*component) {
    const char *end = strchr(component, '/');
    size_t length = end ? (size_t)(end - component) : strlen(component);
    if (!pnf_name_valid((const uint8_t *)component, length))
      return PNF_INVALID;
    struct lookup_context lookup = { .name = (const uint8_t *)component, .length = length };
    status = inspect_directory(image, volume, *number, inode, lookup_entry, &lookup);
    if (status != PNF_OK)
      return status;
    if (!lookup.number)
      return PNF_NOT_FOUND;
    *number = lookup.number;
    *inode = lookup.inode;
    if (!end)
      break;
    component = end + 1;
    if (!*component)
      return PNF_INVALID;
  }
  return PNF_OK;
}

static void
print_info(const struct native_image *image)
{
  fputs("pool ", stdout);
  native_id_print(stdout, image->header.pool_id);
  printf("\nblocks %llu\nblock_size %u\nbitmap %llu %llu\nvolume_table %llu %llu\n"
         "journal %llu %llu\njournal_sequence %llu\njournal_state empty\n"
         "compatible 0x%llx\nread_only_compatible 0x%llx\nrequired 0x%llx\n"
         "header_copies %s\n",
         (unsigned long long)image->header.pool_blocks, PNF_BLOCK_SIZE,
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
print_volumes(const struct pnf_volume volumes[PNF_VOLUME_COUNT])
{
  for (unsigned i = 0; i < PNF_VOLUME_COUNT; i++) {
    const struct pnf_volume *volume = &volumes[i];
    if (volume->state != PNF_VOLUME_LIVE)
      continue;
    printf("slot %u name ", i);
    native_name_print(stdout, volume->name, volume->name_length);
    fputs(" id ", stdout);
    native_id_print(stdout, volume->id);
    printf(" inode_bytes %llu cleanup_head %llu\n",
           (unsigned long long)volume->inode_bytes, (unsigned long long)volume->cleanup_head);
  }
}

static void
print_stat(uint64_t number, const struct pnf_inode *inode)
{
  printf("inode %llu\nkind %s\nsize %llu\n",
         (unsigned long long)number,
         inode->kind == PNF_INODE_DIRECTORY ? "directory" : "file",
         (unsigned long long)inode->size);
  if (inode->kind == PNF_INODE_DIRECTORY)
    printf("parent %llu\n", (unsigned long long)inode->parent);
  if (inode->flags & PNF_TIME_CREATED_VALID)
    printf("created_ns %lld\n", (long long)inode->created_ns);
  else
    puts("created_ns unknown");
  if (inode->flags & PNF_TIME_MODIFIED_VALID)
    printf("modified_ns %lld\n", (long long)inode->modified_ns);
  else
    puts("modified_ns unknown");
  printf("cleanup 0x%x\ncleanup_next %llu\nshrink_target %llu\n", inode->cleanup,
         (unsigned long long)inode->cleanup_next, (unsigned long long)inode->shrink_target);
}

static enum pnf_status
list_entry(struct native_image *image, const struct pnf_dirent *entry,
           const struct pnf_inode *inode, void *context)
{
  (void)image;
  (void)context;
  printf("%s %llu %llu ", inode->kind == PNF_INODE_DIRECTORY ? "directory" : "file",
         (unsigned long long)entry->inode, (unsigned long long)inode->size);
  native_name_print(stdout, entry->name, entry->name_length);
  putchar('\n');
  return ferror(stdout) ? PNF_IO : PNF_OK;
}

static enum pnf_status
copy_file(struct native_image *image, const struct pnf_inode *inode, FILE *output)
{
  if (inode->kind != PNF_INODE_FILE)
    return PNF_INVALID;
  uint8_t bytes[64 * 1024];
  uint64_t offset = 0;
  while (offset < inode->size) {
    uint64_t remaining = inode->size - offset;
    size_t count = remaining < sizeof(bytes) ? (size_t)remaining : sizeof(bytes);
    enum pnf_status status = native_read_file(image, inode->pointers, inode->size,
                                             offset, bytes, count);
    if (status != PNF_OK)
      return status;
    if (fwrite(bytes, 1, count, output) != count) {
      snprintf(image->error, sizeof(image->error), "output write: %s", strerror(errno));
      return PNF_IO;
    }
    offset += count;
  }
  return PNF_OK;
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
  struct native_image image;
  enum pnf_status status = native_image_open(&image, options.image, false, false);
  if (status != PNF_OK)
    return native_report("open", status, &image);
  if (info) {
    print_info(&image);
    goto done;
  }
  struct pnf_volume volumes[PNF_VOLUME_COUNT];
  status = native_read_volumes(&image, volumes);
  if (status != PNF_OK)
    goto done;
  if (volumes_command) {
    print_volumes(volumes);
    goto done;
  }
  struct pnf_volume *volume = NULL;
  size_t name_length = strlen(options.volume);
  for (unsigned i = 0; i < PNF_VOLUME_COUNT; i++) {
    if (volumes[i].state == PNF_VOLUME_LIVE && volumes[i].name_length == name_length &&
        !memcmp(volumes[i].name, options.volume, name_length)) {
      volume = &volumes[i];
      break;
    }
  }
  if (!volume) {
    status = PNF_NOT_FOUND;
    goto done;
  }
  uint64_t number;
  struct pnf_inode inode;
  status = resolve_path(&image, volume, options.path, &number, &inode);
  if (status != PNF_OK)
    goto done;
  if (list)
    status = inspect_directory(&image, volume, number, &inode, list_entry, NULL);
  else if (stat)
    print_stat(number, &inode);
  else if (cat)
    status = copy_file(&image, &inode, stdout);
  else {
    if (inode.kind != PNF_INODE_FILE) {
      status = PNF_INVALID;
      goto done;
    }
    FILE *output = fopen(options.output, "wbx");
    if (!output) {
      snprintf(image.error, sizeof(image.error), "create output: %s", strerror(errno));
      status = PNF_IO;
      goto done;
    }
    status = copy_file(&image, &inode, output);
    if (fclose(output) && status == PNF_OK) {
      snprintf(image.error, sizeof(image.error), "close output: %s", strerror(errno));
      status = PNF_IO;
    }
    if (status != PNF_OK)
      unlink(options.output);
  }
done:
  if (status == PNF_OK && fflush(stdout)) {
    snprintf(image.error, sizeof(image.error), "flush output: %s", strerror(errno));
    status = PNF_IO;
  }
  int result = status == PNF_OK ? 0 : native_report(options.command, status, &image);
  enum pnf_status close_status = native_image_close(&image);
  if (!result && close_status != PNF_OK)
    result = native_report("close", close_status, &image);
  return result;
}
