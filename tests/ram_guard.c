/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
#include "ram_guard.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define MOUNT_LINE_BYTES 16384

static int scratch_directory = -1;

static void
refuse(const char *reason)
{
  fprintf(stderr, "pyxis-fs: RAM guard: %s\n", reason);
  exit(EXIT_FAILURE);
}

static bool
parse_number(const char *text, uint64_t *value)
{
  uint64_t result = 0;
  if (!*text) {
    return false;
  }
  for (; *text; text++) {
    if (*text < '0' || *text > '9' || result > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    result = result * 10 + (*text - '0');
  }
  *value = result;
  return true;
}

static bool
mount_id(int fd, uint64_t *id)
{
  struct statx attributes;
  if (statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &attributes) != 0 ||
      !(attributes.stx_mask & STATX_MNT_ID)) {
    return false;
  }
  *id = attributes.stx_mnt_id;
  return true;
}

static bool
mount_option(const char *options, const char *wanted, bool prefix)
{
  size_t length = strlen(wanted);
  for (const char *option = options; *option;) {
    const char *end = strchr(option, ',');
    size_t size = end ? (size_t)(end - option) : strlen(option);
    if (size >= length && !memcmp(option, wanted, length) && (prefix || size == length)) {
      return true;
    }
    if (!end) {
      break;
    }
    option = end + 1;
  }
  return false;
}

static bool
decode_path(char *path)
{
  char *destination = path;
  for (char *source = path; *source;) {
    if (*source != '\\') {
      *destination++ = *source++;
      continue;
    }
    if (!source[1] || !source[2] || !source[3] ||
        source[1] < '0' || source[1] > '7' || source[2] < '0' || source[2] > '7' ||
        source[3] < '0' || source[3] > '7') {
      return false;
    }
    unsigned value = (unsigned)(source[1] - '0') * 64 +
      (unsigned)(source[2] - '0') * 8 + (unsigned)(source[3] - '0');
    if (!value || value > UCHAR_MAX) {
      return false;
    }
    *destination++ = (char)value;
    source += 4;
  }
  *destination = '\0';
  return true;
}

static bool
read_limit(int directory, const char *name, uint64_t *value, bool allow_max)
{
  int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  char text[64];
  ssize_t size = read(fd, text, sizeof(text) - 1);
  int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  if (size <= 0 || (size_t)size == sizeof(text) - 1 || text[size - 1] != '\n') {
    errno = EINVAL;
    return false;
  }
  text[size - 1] = '\0';
  if (allow_max && !strcmp(text, "max")) {
    *value = UINT64_MAX;
    return true;
  }
  if (!parse_number(text, value)) {
    errno = EINVAL;
    return false;
  }
  return true;
}

static bool
contains_process(int directory)
{
  int fd = openat(directory, "cgroup.procs", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  FILE *file = fdopen(fd, "r");
  if (!file) {
    close(fd);
    return false;
  }
  bool found = false;
  char line[32];
  while (fgets(line, sizeof(line), file)) {
    size_t length = strlen(line);
    uint64_t pid;
    if (!length || line[length - 1] != '\n') {
      found = false;
      break;
    }
    line[length - 1] = '\0';
    if (!parse_number(line, &pid)) {
      found = false;
      break;
    }
    found = found || pid == (uint64_t)getpid();
  }
  bool valid = !ferror(file);
  fclose(file);
  return valid && found;
}

static bool
check_cgroup(char *root, char *point, uint64_t id, const char *path)
{
  if (!decode_path(root) || !decode_path(point) || *root != '/' || *point != '/') {
    return false;
  }
  int mount = open(point, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (mount < 0) {
    return false;
  }
  struct stat root_attributes;
  struct statfs filesystem;
  uint64_t actual_id;
  bool valid = fstat(mount, &root_attributes) == 0 &&
    fstatfs(mount, &filesystem) == 0 && filesystem.f_type == CGROUP2_SUPER_MAGIC &&
    mount_id(mount, &actual_id) && actual_id == id;
  const char *relative = path + 1;
  size_t root_length = strlen(root);
  if (root_length > 1 && !strncmp(path, root, root_length) &&
      (!path[root_length] || path[root_length] == '/')) {
    relative = path + root_length;
    if (*relative == '/') {
      relative++;
    }
  }
  int directory = valid ? openat(mount, *relative ? relative : ".",
    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
  close(mount);
  if (directory < 0) {
    return false;
  }
  uint64_t current_swap;
  valid = mount_id(directory, &actual_id) && actual_id == id &&
    contains_process(directory) &&
    read_limit(directory, "memory.swap.current", &current_swap, false) && !current_swap;
  uint64_t memory_max = UINT64_MAX, swap_max = UINT64_MAX;
  while (valid) {
    struct stat attributes;
    if (fstat(directory, &attributes) != 0) {
      valid = false;
      break;
    }
    bool at_root = attributes.st_dev == root_attributes.st_dev &&
      attributes.st_ino == root_attributes.st_ino;
    uint64_t memory, swap;
    bool memory_present = read_limit(directory, "memory.max", &memory, true);
    int memory_error = errno;
    bool swap_present = read_limit(directory, "memory.swap.max", &swap, true);
    int swap_error = errno;
    /* The real hierarchy root has no controller limit files. A namespace root
     * for a limited job does have them, and is checked like any other cgroup. */
    if (!memory_present || !swap_present) {
      valid = at_root && !memory_present && !swap_present &&
        memory_error == ENOENT && swap_error == ENOENT;
      break;
    }
    if (memory < memory_max) {
      memory_max = memory;
    }
    if (swap < swap_max) {
      swap_max = swap;
    }
    if (at_root) {
      break;
    }
    int parent = openat(directory, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(directory);
    directory = parent;
    valid = directory >= 0 && mount_id(directory, &actual_id) && actual_id == id;
  }
  if (directory >= 0) {
    close(directory);
  }
  return valid && memory_max > 0 && memory_max < UINT64_MAX && swap_max == 0;
}

static bool
read_cgroup_path(char *path, size_t capacity)
{
  FILE *file = fopen("/proc/self/cgroup", "re");
  if (!file) {
    return false;
  }
  bool found = false;
  char line[PATH_MAX + 8];
  while (fgets(line, sizeof(line), file)) {
    size_t length = strlen(line);
    if (!length || line[length - 1] != '\n') {
      found = false;
      break;
    }
    if (strncmp(line, "0::/", 4)) {
      continue;
    }
    line[length - 1] = '\0';
    if (found || length - 3 > capacity) {
      found = false;
      break;
    }
    memcpy(path, line + 3, length - 3);
    found = true;
  }
  bool valid = !ferror(file);
  fclose(file);
  if (!valid || !found) {
    return false;
  }
  /* Reject paths that could escape the controller mount, including the proc
   * representation of membership outside the current cgroup namespace. */
  for (const char *part = path + 1; *part;) {
    const char *end = strchr(part, '/');
    size_t length = end ? (size_t)(end - part) : strlen(part);
    if (!length || (length == 1 && part[0] == '.') ||
        (length == 2 && part[0] == '.' && part[1] == '.')) {
      return false;
    }
    if (!end) {
      break;
    }
    part = end + 1;
  }
  return true;
}

static bool
directory_empty(int directory)
{
  int copy = dup(directory);
  if (copy < 0) {
    return false;
  }
  DIR *stream = fdopendir(copy);
  if (!stream) {
    close(copy);
    return false;
  }
  bool empty = true;
  errno = 0;
  struct dirent *entry;
  while ((entry = readdir(stream))) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      empty = false;
      break;
    }
  }
  bool valid = !errno;
  return closedir(stream) == 0 && valid && empty;
}

static void
require_ram(bool ci_quick)
{
  if (scratch_directory >= 0) {
    return;
  }
  if (getuid() == 0 || geteuid() == 0) {
    refuse("filesystem workloads must run unprivileged");
  }
  struct rlimit core;
  if (getrlimit(RLIMIT_CORE, &core) != 0 || core.rlim_cur != 0 || core.rlim_max != 0 ||
      prctl(PR_SET_DUMPABLE, 0) != 0 || prctl(PR_GET_DUMPABLE) != 0) {
    refuse("launch with hard core limit zero; dumpability must be disabled");
  }
  const char *temporary = getenv("TMPDIR");
  if (!temporary || !*temporary) {
    refuse("TMPDIR is required");
  }
  int directory = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct statfs filesystem;
  struct statvfs capacity;
  uint64_t scratch_id;
  if (directory < 0 || fstatfs(directory, &filesystem) != 0 ||
      filesystem.f_type != TMPFS_MAGIC || fstatvfs(directory, &capacity) != 0 ||
      !capacity.f_frsize || !capacity.f_blocks ||
      capacity.f_blocks >= UINT64_MAX / capacity.f_frsize ||
      !capacity.f_files || capacity.f_files == UINT64_MAX ||
      !mount_id(directory, &scratch_id)) {
    refuse("TMPDIR must be tmpfs with finite positive byte and inode limits");
  }
  char cgroup_path[PATH_MAX];
  if (!read_cgroup_path(cgroup_path, sizeof(cgroup_path))) {
    refuse("cannot resolve cgroup v2 membership");
  }
  FILE *mounts = fopen("/proc/self/mountinfo", "re");
  if (!mounts) {
    refuse("cannot read mountinfo");
  }
  bool scratch_valid = false, cgroup_valid = false;
  char line[MOUNT_LINE_BYTES];
  while (fgets(line, sizeof(line), mounts)) {
    size_t length = strlen(line);
    if (!length || line[length - 1] != '\n') {
      refuse("mountinfo line exceeds guard capacity");
    }
    line[length - 1] = '\0';
    char *state;
    char *id_text = strtok_r(line, " ", &state);
    uint64_t id;
    if (!id_text || !parse_number(id_text, &id) || !strtok_r(NULL, " ", &state) ||
        !strtok_r(NULL, " ", &state)) {
      refuse("malformed mountinfo");
    }
    char *root = strtok_r(NULL, " ", &state);
    char *point = strtok_r(NULL, " ", &state);
    char *options = strtok_r(NULL, " ", &state);
    char *field;
    if (!root || !point || !options) {
      refuse("malformed mountinfo");
    }
    do {
      field = strtok_r(NULL, " ", &state);
    } while (field && strcmp(field, "-"));
    char *type = field ? strtok_r(NULL, " ", &state) : NULL;
    char *source = type ? strtok_r(NULL, " ", &state) : NULL;
    char *super_options = source ? strtok_r(NULL, " ", &state) : NULL;
    if (!super_options) {
      refuse("malformed mountinfo");
    }
    if (id == scratch_id) {
      scratch_valid = !strcmp(type, "tmpfs") &&
        (ci_quick || mount_option(super_options, "noswap", false)) &&
        mount_option(super_options, "nr_inodes=", true);
    }
    if (!cgroup_valid && !strcmp(type, "cgroup2")) {
      cgroup_valid = check_cgroup(root, point, id, cgroup_path);
    }
  }
  bool read_failed = ferror(mounts);
  fclose(mounts);
  if (read_failed || !scratch_valid) {
    refuse(ci_quick ? "TMPDIR mount must use tmpfs and an explicit inode cap" :
      "TMPDIR mount must explicitly use noswap and an inode cap");
  }
  if (!cgroup_valid) {
    refuse("require a finite positive cgroup memory limit, swap.max = 0 and swap.current = 0");
  }
  if (ci_quick && !directory_empty(directory)) {
    refuse("quick CI requires an empty TMPDIR before creating fresh fixtures");
  }
  scratch_directory = directory;
}

void
pfs_test_require_ram(void)
{
  require_ram(false);
}

void
pfs_test_require_ci_ram(void)
{
  require_ram(true);
}

int
pfs_test_ram_directory_fd(void)
{
  pfs_test_require_ram();
  return scratch_directory;
}
