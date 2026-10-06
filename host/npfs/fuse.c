/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _TIME_BITS 64
#define FUSE_USE_VERSION 31
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mount_state {
  struct npfs_image image;
  struct npfs_volume volumes[NPFS_VOLUME_COUNT];
  uid_t uid;
  gid_t gid;
};

struct mount_node {
  int volume;
  uint64_t number;
  struct npfs_inode inode;
};

struct directory_cursor {
  uint64_t offset;
  uint64_t block;
  uint8_t bytes[NPFS_BLOCK_SIZE];
};

struct directory_entry {
  char name[NPFS_NAME_MAX + 1];
  struct stat attributes;
};

struct open_directory {
  struct directory_entry *entries;
  size_t count;
  size_t capacity;
};

static struct mount_state *mount_state(void)
{
  return fuse_get_context()->private_data;
}

static int read_result(enum npfs_status status)
{
  if (status == NPFS_OK) {
    return 0;
  }
  return status == NPFS_NO_MEMORY ? -ENOMEM : -EIO;
}

static bool attached_inode(uint64_t number, const struct npfs_inode *inode)
{
  return inode->kind != NPFS_INODE_FREE && !(inode->cleanup & NPFS_CLEANUP_DETACHED) &&
      (number != 1 || (inode->kind == NPFS_INODE_DIRECTORY && inode->parent == 1)) &&
      (inode->kind != NPFS_INODE_FILE || !inode->parent);
}

static int read_node(struct mount_state *mount, int volume, uint64_t number,
    struct mount_node *node)
{
  *node = (struct mount_node){.volume = volume, .number = number};
  int error = read_result(npfs_read_inode(&mount->image, &mount->volumes[volume],
      number, &node->inode));
  if (error) {
    return error;
  }
  return attached_inode(number, &node->inode) ? 0 : -EIO;
}

/* Cursor storage and decoded records are private to one scan. Namespace reads
 * check traversed records, not global bitmap ownership or reachability. */
static int next_entry(struct mount_state *mount, const struct mount_node *directory,
    struct directory_cursor *cursor, struct npfs_dirent *entry, struct mount_node *node)
{
  while (cursor->offset < directory->inode.size) {
    uint64_t logical = cursor->offset / NPFS_BLOCK_SIZE;
    if (cursor->block != logical) {
      uint64_t physical;
      int error = read_result(npfs_map_block(&mount->image, directory->inode.pointers,
          logical, &physical));
      if (error || !physical) {
        return error ? error : -EIO;
      }
      error = read_result(npfs_read_blocks(&mount->image, physical, 1, cursor->bytes));
      if (error) {
        return error;
      }
      cursor->block = logical;
    }
    size_t within = (size_t)(cursor->offset % NPFS_BLOCK_SIZE);
    int error = read_result(npfs_dirent_decode(&mount->image.header,
        cursor->bytes + within, NPFS_BLOCK_SIZE - within, entry));
    if (error) {
      return error;
    }
    cursor->offset += entry->record_length;
    if (!entry->inode) {
      continue;
    }
    error = read_node(mount, directory->volume, entry->inode, node);
    if (error || entry->inode == 1 ||
        (node->inode.kind == NPFS_INODE_DIRECTORY && node->inode.parent != directory->number)) {
      return error ? error : -EIO;
    }
    return 1;
  }
  return 0;
}

static int lookup_child(struct mount_state *mount, const struct mount_node *directory,
    const char *name, size_t length, struct mount_node *node)
{
  if (directory->inode.kind != NPFS_INODE_DIRECTORY) {
    return -ENOTDIR;
  }
  struct directory_cursor cursor = {.block = UINT64_MAX};
  struct npfs_dirent entry;
  struct mount_node candidate;
  bool found = false;
  int result;
  while ((result = next_entry(mount, directory, &cursor, &entry, &candidate)) > 0) {
    if (entry.name_length != length || memcmp(entry.name, name, length)) {
      continue;
    }
    if (found) {
      return -EIO;
    }
    found = true;
    *node = candidate;
  }
  return result < 0 ? result : found ? 0 : -ENOENT;
}

static int lookup_node(struct mount_state *mount, const char *path, struct mount_node *node)
{
  if (!path || path[0] != '/') {
    return -EINVAL;
  }
  *node = (struct mount_node){.volume = -1};
  if (!path[1]) {
    return 0;
  }
  const char *name = path + 1;
  const char *end = strchr(name, '/');
  size_t length = end ? (size_t)(end - name) : strlen(name);
  int volume = -1;
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; ++i) {
    if (mount->volumes[i].state == NPFS_VOLUME_LIVE &&
        mount->volumes[i].name_length == length && !memcmp(mount->volumes[i].name, name, length)) {
      volume = (int)i;
      break;
    }
  }
  if (volume < 0) {
    return -ENOENT;
  }
  int error = read_node(mount, volume, 1, node);
  while (!error && end && end[1]) {
    name = end + 1;
    end = strchr(name, '/');
    length = end ? (size_t)(end - name) : strlen(name);
    if (!npfs_name_valid((const uint8_t *)name, length)) {
      return -EINVAL;
    }
    struct mount_node child;
    error = lookup_child(mount, node, name, length, &child);
    if (!error) {
      *node = child;
    }
  }
  return error;
}

static struct timespec linux_time(int64_t nanoseconds)
{
  int64_t seconds = nanoseconds / INT64_C(1000000000);
  long fraction = (long)(nanoseconds % INT64_C(1000000000));
  if (fraction < 0) {
    --seconds;
    fraction += 1000000000;
  }
  return (struct timespec){.tv_sec = (time_t)seconds, .tv_nsec = fraction};
}

static void node_attributes(const struct mount_state *mount, const struct mount_node *node,
    struct stat *attributes)
{
  *attributes = (struct stat){.st_uid = mount->uid, .st_gid = mount->gid,
      .st_blksize = NPFS_BLOCK_SIZE};
  if (node->volume < 0) {
    attributes->st_ino = 1;
    attributes->st_mode = S_IFDIR | 0555;
    attributes->st_nlink = 2;
    return;
  }
  attributes->st_ino = 2 + (uint64_t)node->volume + (node->number - 1) * NPFS_VOLUME_COUNT;
  bool directory = node->inode.kind == NPFS_INODE_DIRECTORY;
  attributes->st_mode = directory ? S_IFDIR | 0555 : S_IFREG | 0444;
  attributes->st_nlink = directory ? 2 : 1;
  attributes->st_size = (off_t)node->inode.size;
  if (node->inode.flags & NPFS_TIME_MODIFIED_VALID) {
    attributes->st_mtim = linux_time(node->inode.modified_ns);
  }
  /* npfs stores neither access nor POSIX change time. Native creation time and
   * unknown-time status remain explicit in the read-only xattrs. */
  attributes->st_atim = attributes->st_mtim;
  attributes->st_ctim = attributes->st_mtim;
}

static int mount_getattr(const char *path, struct stat *attributes, struct fuse_file_info *file)
{
  (void)file;
  struct mount_state *mount = mount_state();
  struct mount_node node;
  int error = lookup_node(mount, path, &node);
  if (!error) {
    node_attributes(mount, &node, attributes);
  }
  return error;
}

static int mount_open(const char *path, struct fuse_file_info *file)
{
  if ((file->flags & O_ACCMODE) != O_RDONLY || (file->flags & (O_TRUNC | O_APPEND))) {
    return -EROFS;
  }
  struct mount_node node;
  int error = lookup_node(mount_state(), path, &node);
  if (error) {
    return error;
  }
  if (node.volume < 0 || node.inode.kind == NPFS_INODE_DIRECTORY) {
    return -EISDIR;
  }
  struct mount_node *opened = malloc(sizeof(*opened));
  if (!opened) {
    return -ENOMEM;
  }
  *opened = node;
  file->fh = (uint64_t)(uintptr_t)opened;
  return 0;
}

static int mount_read(const char *path, char *bytes, size_t length, off_t offset,
    struct fuse_file_info *file)
{
  (void)path;
  if (offset < 0 || !file || !file->fh) {
    return -EINVAL;
  }
  const struct mount_node *node = (const void *)(uintptr_t)file->fh;
  if ((uint64_t)offset >= node->inode.size) {
    return 0;
  }
  uint64_t remaining = node->inode.size - (uint64_t)offset;
  if (length > remaining) {
    length = (size_t)remaining;
  }
  if (length > INT_MAX) {
    length = INT_MAX;
  }
  if (!length) {
    return 0;
  }
  int error = read_result(npfs_read_file(&mount_state()->image, node->inode.pointers,
      node->inode.size, (uint64_t)offset, bytes, length));
  return error ? error : (int)length;
}

static int mount_release(const char *path, struct fuse_file_info *file)
{
  (void)path;
  free((void *)(uintptr_t)file->fh);
  file->fh = 0;
  return 0;
}

static int append_entry(struct mount_state *mount, struct open_directory *directory,
    const char *name, size_t length, const struct mount_node *node)
{
  if (directory->count == directory->capacity) {
    size_t limit = SIZE_MAX / sizeof(*directory->entries);
    if (directory->capacity > limit / 2) {
      return -ENOMEM;
    }
    size_t capacity = directory->capacity ? directory->capacity * 2 : 16;
    void *entries = realloc(directory->entries, capacity * sizeof(*directory->entries));
    if (!entries) {
      return -ENOMEM;
    }
    directory->entries = entries;
    directory->capacity = capacity;
  }
  struct directory_entry *entry = &directory->entries[directory->count++];
  memcpy(entry->name, name, length);
  entry->name[length] = '\0';
  node_attributes(mount, node, &entry->attributes);
  return 0;
}

static int compare_entries(const void *left, const void *right)
{
  const struct directory_entry *a = left, *b = right;
  return strcmp(a->name, b->name);
}

static int mount_opendir(const char *path, struct fuse_file_info *file)
{
  struct mount_state *mount = mount_state();
  struct mount_node node;
  int error = lookup_node(mount, path, &node);
  if (error) {
    return error;
  }
  if (node.volume >= 0 && node.inode.kind != NPFS_INODE_DIRECTORY) {
    return -ENOTDIR;
  }
  struct open_directory *directory = calloc(1, sizeof(*directory));
  if (!directory) {
    return -ENOMEM;
  }
  struct mount_node parent = {.volume = -1};
  if (node.volume >= 0 && node.number != 1) {
    error = read_node(mount, node.volume, node.inode.parent, &parent);
    if (!error && parent.inode.kind != NPFS_INODE_DIRECTORY) {
      error = -EIO;
    }
  }
  if (!error) {
    error = append_entry(mount, directory, ".", 1, &node);
  }
  if (!error) {
    error = append_entry(mount, directory, "..", 2, &parent);
  }
  if (node.volume < 0) {
    for (unsigned i = 0; !error && i < NPFS_VOLUME_COUNT; ++i) {
      if (mount->volumes[i].state != NPFS_VOLUME_LIVE) {
        continue;
      }
      struct mount_node root;
      error = read_node(mount, (int)i, 1, &root);
      if (!error) {
        error = append_entry(mount, directory, (const char *)mount->volumes[i].name,
            mount->volumes[i].name_length, &root);
      }
    }
  } else {
    struct directory_cursor cursor = {.block = UINT64_MAX};
    struct npfs_dirent entry;
    struct mount_node child;
    int result = 0;
    while (!error && (result = next_entry(mount, &node, &cursor, &entry, &child)) > 0) {
      error = append_entry(mount, directory, (const char *)entry.name, entry.name_length, &child);
    }
    if (!error && result < 0) {
      error = result;
    }
  }
  if (!error) {
    qsort(directory->entries + 2, directory->count - 2, sizeof(*directory->entries), compare_entries);
    for (size_t i = 3; i < directory->count; ++i) {
      if (!strcmp(directory->entries[i - 1].name, directory->entries[i].name)) {
        error = -EIO;
        break;
      }
    }
  }
  if (error) {
    free(directory->entries);
    free(directory);
    return error;
  }
  file->fh = (uint64_t)(uintptr_t)directory;
  return 0;
}

static int mount_readdir(const char *path, void *buffer, fuse_fill_dir_t fill,
    off_t offset, struct fuse_file_info *file, enum fuse_readdir_flags flags)
{
  (void)path;
  if (offset < 0 || !file || !file->fh) {
    return -EINVAL;
  }
  const struct open_directory *directory = (const void *)(uintptr_t)file->fh;
  if ((uint64_t)offset > directory->count) {
    return -EINVAL;
  }
  for (size_t i = (size_t)offset; i < directory->count; ++i) {
    const struct directory_entry *entry = &directory->entries[i];
    enum fuse_fill_dir_flags filled = flags & FUSE_READDIR_PLUS ? FUSE_FILL_DIR_PLUS : 0;
    if (fill(buffer, entry->name, &entry->attributes, (off_t)(i + 1), filled)) {
      break;
    }
  }
  return 0;
}

static int mount_releasedir(const char *path, struct fuse_file_info *file)
{
  (void)path;
  struct open_directory *directory = (void *)(uintptr_t)file->fh;
  free(directory->entries);
  free(directory);
  file->fh = 0;
  return 0;
}

static int mount_getxattr(const char *path, const char *name, char *value, size_t size)
{
  bool created;
  if (!strcmp(name, "user.npfs.created_ns")) {
    created = true;
  } else if (!strcmp(name, "user.npfs.modified_ns")) {
    created = false;
  } else {
    return -ENODATA;
  }
  struct mount_node node;
  int error = lookup_node(mount_state(), path, &node);
  if (error) {
    return error;
  }
  uint32_t flag = created ? NPFS_TIME_CREATED_VALID : NPFS_TIME_MODIFIED_VALID;
  char text[32];
  int length = node.volume >= 0 && (node.inode.flags & flag) ?
      snprintf(text, sizeof(text), "%lld", (long long)(created ? node.inode.created_ns : node.inode.modified_ns)) :
      snprintf(text, sizeof(text), "unknown");
  if (!size) {
    return length;
  }
  if (size < (size_t)length) {
    return -ERANGE;
  }
  memcpy(value, text, (size_t)length);
  return length;
}

static int mount_listxattr(const char *path, char *list, size_t size)
{
  struct mount_node node;
  int error = lookup_node(mount_state(), path, &node);
  if (error) {
    return error;
  }
  static const char names[] = "user.npfs.created_ns\0user.npfs.modified_ns\0";
  size_t length = sizeof(names) - 1;
  if (!size) {
    return (int)length;
  }
  if (size < length) {
    return -ERANGE;
  }
  memcpy(list, names, length);
  return (int)length;
}

static void *mount_initialize(struct fuse_conn_info *connection, struct fuse_config *configuration)
{
  (void)connection;
  configuration->use_ino = 1;
  configuration->kernel_cache = 1;
  return mount_state();
}

int main(int argc, char **argv)
{
  bool foreground = argc == 4 && !strcmp(argv[1], "-f");
  if (argc != (foreground ? 4 : 3)) {
    fputs("usage: npfs-fuse [-f] SOURCE MOUNTPOINT\n", stderr);
    return 2;
  }
  const char *source = argv[foreground ? 2 : 1];
  char *mountpoint = argv[foreground ? 3 : 2];
  struct mount_state mount = {.uid = getuid(), .gid = getgid()};
  enum npfs_status status = npfs_source_open(&mount.image, source);
  if (status != NPFS_OK) {
    if (status == NPFS_RECOVERY_REQUIRED) {
      fputs("npfs-fuse: committed journal; boot Pyxis once to recover it, or run\n"
          "fsck.npfs --image COPY --replay on a copy of the image. Nothing was written.\n", stderr);
    } else {
      npfs_report("npfs-fuse open", status, &mount.image);
    }
    return 1;
  }
  if (mount.image.degraded_header) {
    fputs("npfs-fuse: warning: only one pool header is valid\n", stderr);
  }
  status = npfs_read_volumes(&mount.image, mount.volumes);
  bool valid = status == NPFS_OK;
  for (unsigned i = 0; valid && i < NPFS_VOLUME_COUNT; ++i) {
    if (mount.volumes[i].state != NPFS_VOLUME_LIVE) {
      continue;
    }
    struct npfs_inode reserved, root;
    status = npfs_read_inode(&mount.image, &mount.volumes[i], 0, &reserved);
    valid = status == NPFS_OK && reserved.kind == NPFS_INODE_FREE;
    if (valid) {
      status = npfs_read_inode(&mount.image, &mount.volumes[i], 1, &root);
      valid = status == NPFS_OK && attached_inode(1, &root);
    }
  }
  if (!valid) {
    npfs_report("npfs-fuse catalog/root", status == NPFS_OK ? NPFS_CORRUPT : status, &mount.image);
    npfs_image_close(&mount.image);
    return 1;
  }
  static const struct fuse_operations operations = {
    .init = mount_initialize,
    .getattr = mount_getattr,
    .open = mount_open,
    .read = mount_read,
    .release = mount_release,
    .opendir = mount_opendir,
    .readdir = mount_readdir,
    .releasedir = mount_releasedir,
    .getxattr = mount_getxattr,
    .listxattr = mount_listxattr,
  };
  /* Serialize callbacks: the host reader owns one diagnostic buffer. Fixed
   * readonly/private mount options do not grant other users access. */
  char *arguments[] = {argv[0], mountpoint, "-s", "-o", "ro,default_permissions", "-f"};
  int result = fuse_main(foreground ? 6 : 5, arguments, &operations, &mount);
  status = npfs_image_close(&mount.image);
  if (status != NPFS_OK) {
    npfs_report("npfs-fuse close", status, &mount.image);
    return 1;
  }
  return result;
}
