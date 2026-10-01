#ifndef PYXIS_FS_CHECK_H
#define PYXIS_FS_CHECK_H

#include <pyxis_fs/pool.h>

struct pfs_check_event {
  /* NO_SELECTION denotes a global or cross-state failure. UINT64_MAX means that
   * no physical block is available; zero IDs mean no known volume/object. */
  uint16_t slot;
  enum pfs_status status;
  const char *operation;
  uint64_t block;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
};

/* Called synchronously with borrowed event storage. The reporter must not
 * reenter the checker or change its reader, memory owner or underlying image. */
typedef void (*pfs_check_report_fn)(void *context, const struct pfs_check_event *event);

struct pfs_check_state_result {
  enum pfs_status status;
  /* One bit (1u << status) for every failure observed in this state. */
  uint32_t failures;
  bool complete;
  uint64_t generation;
  uint64_t volumes;
  uint64_t objects;
  uint64_t directory_entries;
  uint64_t file_extents;
  uint64_t grants;
  uint64_t orphans;
  uint64_t metadata_blocks;
  uint64_t file_blocks;
  uint64_t unsupported_volumes;
};

struct pfs_check_result {
  struct pfs_pool_diagnostic pool;
  struct pfs_check_state_result state[2];
  enum pfs_status cross_status;
  bool cross_complete;
};

/* Diagnostic authority over an unchanged image. Both candidates are examined
 * independently, including a retained state not selected for ordinary reads.
 * All workspace is charged to memory and released before return. Media/resource
 * failures publish partial results; invalid arguments leave result unchanged.
 * OK means both states and their overlap were completely checked. File payloads
 * are not read or integrity-verified. No repair or reclamation is performed.
 * report is optional; when supplied it receives individual failure contexts. */
enum pfs_status pfs_check(const struct pfs_block_reader *reader,
                           struct pfs_memory *memory, pfs_check_report_fn report,
                           void *context, struct pfs_check_result *result);

#endif
