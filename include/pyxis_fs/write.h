/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_WRITE_H
#define PYXIS_FS_WRITE_H

#include <pyxis_fs/access.h>

/* Trusted strong randomness. No fallback; callbacks borrow the buffer only
 * until return and must not reenter the core. */
typedef enum pfs_status (*pfs_random_fn)(void *context, void *buffer, size_t length);

struct pfs_write_options {
  uint64_t extent_limit;
  uint64_t metadata_limit;
  pfs_random_fn random;
  void *random_context;
};

enum pfs_completion {
  PFS_COMPLETE,
  PFS_STOPPED,
  PFS_UNKNOWN,
};

struct pfs_write_result {
  enum pfs_completion completion;
  enum pfs_status operation_status;
  uint64_t confirmed_bytes;
  bool confirmed_length_valid;
  uint64_t confirmed_length;
  bool namespace_confirmed;
  enum pfs_maintenance_completion maintenance_completion;
  enum pfs_status maintenance_status;
  enum pfs_writer_health health;
};

struct pfs_writer_status {
  enum pfs_writer_health health;
  enum pfs_status failure;
  bool drain_pending;
  bool invariant_failure;
};

struct pfs_write_open_result {
  struct pfs_pool_diagnostic pool;
  struct pfs_writer_status writer;
  struct pfs_write_result recovery;
  uint64_t confirmed_generation;
  uint64_t required_recovery_blocks;
  uint64_t permanent_pool_blocks;
  uint64_t reserved_arena_bytes;
};

/* The adapter owns exclusive, healthy/durably recovered backing for the complete
 * lifetime. Callbacks are synchronous; no core callback may reenter the pool.
 * Neither valid cached bytes nor a successful flush certifies recovery history.
 * Explicit limits are required. Failure leaves pool empty; diagnostic preserves
 * confirmed startup cleanup and uncertainty. No repair or automatic retry. */
enum pfs_status pfs_pool_open_writer(struct pfs_pool *pool,
  const struct pfs_block_builder *backing, struct pfs_memory *memory,
  const struct pfs_write_options *options, struct pfs_write_open_result *diagnostic);

/* Copies in-memory status only; available while stopped. */
enum pfs_status pfs_pool_writer_status(const struct pfs_pool *pool,
  struct pfs_writer_status *out);

/* Volume handles confer no object authority. Ordinary acquisition still checks
 * the supplied trusted context and persistent grants. Diagnostics remain RO. */
enum pfs_status pfs_pool_volume_open(struct pfs_pool *pool,
  const struct pfs_volume_id *id, struct pfs_volume *volume);

/* Uses the held file.checkpoint or dir.checkpoint right, not lookup/read rights.
 * No-op after prior funded drains; stopped instances cannot retry to health. */
enum pfs_status pfs_view_checkpoint(struct pfs_view *view, struct pfs_write_result *result);

/* Invalid arguments and rejected callback reentry leave result unchanged.
 * Accepted calls report confirmed user
 * progress independently of maintenance and health, even on non-OK return.
 * Buffers, results and handles are disjoint; bytes are borrowed through return.
 * A write that extends requires both write and resize before any progress. */
enum pfs_status pfs_view_write(struct pfs_view *view, uint64_t offset,
  const void *buffer, size_t length, struct pfs_write_result *result);
enum pfs_status pfs_view_resize(struct pfs_view *view, uint64_t length,
  struct pfs_write_result *result);

/* Create an empty regular file under the parent's policy owner, with no new
 * grants. Supply all of requested/out/identity, or all NULL for no child handle.
 * A returned view has exactly requested rights and object scope; parent subtree
 * lookup authority must contain them. Reserve it before publication. Confirmed
 * creation may return a view despite cleanup error; UNKNOWN returns none. */
enum pfs_status pfs_view_create_file(struct pfs_view *parent,
  const uint8_t *name, size_t length, const struct pfs_rights *requested,
  struct pfs_view **out, struct pfs_view_identity *identity,
  struct pfs_write_result *result);

/* Empty directory creation has the same owner, reservation and result contract
 * as file creation. Its optional returned view has object scope. */
enum pfs_status pfs_view_create_directory(struct pfs_view *parent,
  const uint8_t *name, size_t length, const struct pfs_rights *requested,
  struct pfs_view **out, struct pfs_view_identity *identity,
  struct pfs_write_result *result);

/* REMOVE on the held parent authorizes unlink of a file or empty directory.
 * Every victim becomes a persistent orphan in the namespace transaction;
 * retained views keep identity, contents and rights. An unheld victim is cleaned
 * after confirmed publication and its funded drain. Namespace confirmation stays
 * true even if that cleanup fails, reported in the maintenance fields. */
enum pfs_status pfs_view_remove(struct pfs_view *parent,
  const uint8_t *name, size_t length, struct pfs_write_result *result);

/* Same-volume regular-file rename preserves source identity and contents.
 * Requires source REMOVE and destination CREATE before either name is examined.
 * A distinct existing regular-file destination requires explicit replace=true
 * and destination REPLACE; replace=false returns EXISTS. Directory moves or
 * replacement and cross-volume moves are UNSUPPORTED. A validated same-name
 * rename is a no-op, with COMPLETE and namespace_confirmed=false. Displaced
 * objects follow the same retained-orphan and cleanup contract as remove. */
enum pfs_status pfs_view_rename(struct pfs_view *source_parent,
  const uint8_t *source_name, size_t source_length,
  struct pfs_view *destination_parent, const uint8_t *destination_name,
  size_t destination_length, bool replace, struct pfs_write_result *result);

/* Copied live continuation, no authority or retained resources. Zero starts.
 * Wrong binding/position is INVALID, changed serial is CHANGED; both publish
 * zero count without advancing next. Check LIST and health before token data.
 * File writes/resizes and maintenance do not invalidate these tokens. */
struct pfs_directory_token {
  uint8_t instance[PFS_ID_SIZE];
  struct pfs_volume_id volume;
  struct pfs_object_id directory;
  uint64_t serial;
  uint64_t position;
};
enum pfs_status pfs_view_directory_live_page(struct pfs_view *view,
  const struct pfs_directory_token *after, struct pfs_view_entry *out,
  size_t capacity, size_t *count, bool *done, struct pfs_directory_token *next);

#endif
