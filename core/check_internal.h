#ifndef PYXIS_FS_CHECK_INTERNAL_H
#define PYXIS_FS_CHECK_INTERNAL_H

#include <pyxis_fs/check.h>
#include <pyxis_fs/tree.h>

#include "internal.h"

#define CHECK_ANCESTRY_MAX 256u
#define CHECK_METADATA_MAX (2 * (2 * PFS_VOLUME_MAX + PFS_ALLOCATION_COUNT_MAX + \
                                  5 * PFS_RECORD_COUNT_MAX) + 1)

struct check_object {
  struct pfs_object_record record;
  uint64_t birth;
  uint32_t incoming;
  uint32_t orphans;
  uint32_t depth;
  uint32_t grants;
};

struct check_volume {
  struct pfs_volume_record record;
  struct pfs_allocation objects;
  size_t object_count;
  bool supported;
  bool complete;
  uint64_t live;
  uint64_t retired;
  uint64_t permanent;
};

/* type zero is file data; metadata uses PFS_BLOCK_POOL or PFS_BLOCK_TREE.
 * Data logical is the logical block corresponding to first. Tree kind/level
 * and both owner IDs identify the interpretation of shared metadata. */
struct check_claim {
  uint64_t first;
  uint64_t count;
  uint64_t birth;
  uint64_t logical;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  uint16_t type;
  uint16_t kind;
  uint16_t level;
};

struct check_state {
  const struct pfs_block_reader *reader;
  struct pfs_memory *memory;
  const struct pfs_pool_candidate *candidate;
  struct pfs_check_state_result *result;
  pfs_check_report_fn report;
  void *context;
  uint16_t slot;
  struct pfs_allocation volumes;
  struct pfs_allocation names;
  struct pfs_allocation allocations;
  struct pfs_allocation claims;
  struct pfs_allocation seen;
  struct pfs_allocation frames;
  size_t volume_count;
  size_t name_count;
  size_t allocation_count;
  size_t claim_count;
  size_t seen_count;
  bool catalog_complete;
  bool map_complete;
  bool pool_complete;
  bool claims_sorted;
};

void check_problem(struct check_state *state, enum pfs_status status,
                    const char *operation, uint64_t block,
                    const struct pfs_volume_id *volume,
                    const struct pfs_object_id *object);
enum pfs_status check_primary(enum pfs_status left, enum pfs_status right);
/* Arrays contain values, not live allocation handles. Count is the number of
 * initialized elements; capacity is derived from allocation size. */
enum pfs_status check_grow(struct pfs_memory *memory, struct pfs_allocation *array,
                            size_t count, size_t item_size, size_t alignment,
                            size_t maximum);
enum pfs_status check_add_claim(struct check_state *state,
                                 const struct check_claim *claim);
struct check_volume *check_find_volume(struct check_state *state,
                                       const struct pfs_volume_id *id);
void check_walk_state(struct check_state *state);
void check_sort_claims(struct check_claim *claims, size_t count);
void check_reconcile_state(struct check_state *state);
enum pfs_status check_compare_states(struct check_state *first,
                                      struct check_state *second,
                                      bool *complete);

/* Called only after complete successful two-state validation, with borrowed
 * sorted tables. Tables remain owned by the checker and expire on return. */
typedef enum pfs_status (*check_capture_fn)(void *context,
                                          const struct check_state states[2]);
enum pfs_status check_capture(const struct pfs_block_reader *reader,
  struct pfs_memory *memory, pfs_check_report_fn report, void *context,
  check_capture_fn capture, void *capture_context, struct pfs_check_result *result);

#endif
