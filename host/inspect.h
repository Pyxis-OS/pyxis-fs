/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_INSPECT_H
#define PYXIS_FS_INSPECT_H

#include "host.h"
#include <pyxis_fs/access.h>

enum inspect_command {
  INSPECT_NONE,
  INSPECT_INFO,
  INSPECT_VOLUMES,
  INSPECT_LIST,
  INSPECT_STAT,
  INSPECT_ACCESS,
  INSPECT_EXTRACT,
  INSPECT_CHECK,
};

struct inspect_options {
  enum inspect_command command;
  const char *image;
  uint64_t memory_limit;
  const char *volume_name;
  struct pfs_volume_id volume_id;
  bool volume_id_set;
  const char *path;
  const char *output;
  const char *root;
  const char *target;
  struct pfs_principal_id principal;
  uint8_t scope;
  struct pfs_rights rights;
  struct pfs_rights ceiling;
  bool gpt;
  struct host_gpt_selection partition;
};

void inspect_usage(FILE *stream);
void inspect_selection_print(const struct pfs_pool_diagnostic *diagnostic, uint64_t blocks);
enum pfs_status inspect_check(const struct pfs_block_reader *reader,
                               struct pfs_memory *memory);
enum pfs_status inspect_options_parse(int argc, char **argv, struct inspect_options *options);
enum pfs_status inspect_objects(struct pfs_pool *pool,
                                const struct inspect_options *options,
                                uint64_t volume_count);

#endif
