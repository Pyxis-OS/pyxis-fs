#ifndef PYXIS_FS_ACCESS_H
#define PYXIS_FS_ACCESS_H

#include <pyxis_fs/read.h>

enum pfs_writer_health {
  PFS_WRITER_READY,
  PFS_WRITER_READABLE_STOPPED,
  PFS_WRITER_ACCESS_STOPPED,
};

/* NONE reports no maintenance outcome and does not certify debt absence.
 * PENDING is healthy, funded volume debt after completed mutation/cleanup.
 * COMPLETE confirms an explicitly requested retirement fence. */
enum pfs_maintenance_completion {
  PFS_MAINTENANCE_NONE,
  PFS_MAINTENANCE_PENDING,
  PFS_MAINTENANCE_COMPLETE,
  PFS_MAINTENANCE_STOPPED,
  PFS_MAINTENANCE_UNKNOWN,
};

struct pfs_view_close_result {
  bool released;
  enum pfs_maintenance_completion maintenance_completion;
  enum pfs_status maintenance_status;
  enum pfs_writer_health health;
};

struct pfs_rights {
  uint64_t file;
  uint64_t directory;
  uint64_t admin;
};

/* The embedding authority establishes this context. Principal IDs supplied by
 * untrusted callers are selectors, not authentication. The root and ceiling
 * bound all acquisitions; inherited grants above root cannot expand either. */
struct pfs_trusted_context {
  struct pfs_principal_id principal;
  struct pfs_object_id root;
  enum pfs_grant_scope scope;
  struct pfs_rights ceiling;
};

struct pfs_access_result {
  bool allowed;
  struct pfs_rights effective;
};

/* Evaluate the complete request, including every intervening directory lookup.
 * OK, DENIED and READ_ONLY publish result. READ_ONLY means policy allowed the
 * complete request, but requested mutation rights have no operational interface.
 * Other failures leave result unchanged. No handle is created. */
enum pfs_status pfs_access_evaluate(struct pfs_volume *volume,
                                   const struct pfs_trusted_context *context,
                                   const struct pfs_object_id *target,
                                   enum pfs_grant_scope scope,
                                   const struct pfs_rights *requested,
                                   struct pfs_access_result *result);

struct pfs_view;
struct pfs_view_directory;

/* Output pointers must initially be NULL. Views are opaque, serial and never
 * copied; they retain their volume until close. No failed acquisition publishes
 * a view. The view contains exactly requested rights, never an implicit subset.
 * Paths are relative to the trusted root and cannot contain '.' or '..'. */
enum pfs_status pfs_view_acquire(struct pfs_volume *volume,
                                const struct pfs_trusted_context *context,
                                const struct pfs_object_id *target,
                                enum pfs_grant_scope scope,
                                const struct pfs_rights *requested,
                                struct pfs_view **out);
enum pfs_status pfs_view_acquire_path(struct pfs_volume *volume,
                                     const struct pfs_trusted_context *context,
                                     const uint8_t *path, size_t length,
                                     enum pfs_grant_scope scope,
                                     const struct pfs_rights *requested,
                                     struct pfs_view **out);
/* Accepted close consumes the view even if final orphan cleanup fails. BUSY
 * preserves the view and reports released=false. Stopped writers release runtime
 * state without cleanup. Healthy orphan deletion can leave PENDING retirement
 * debt; the deletion itself is complete. Close performs no checkpoint or retry. */
enum pfs_status pfs_view_close(struct pfs_view **view,
  struct pfs_view_close_result *result);

struct pfs_view_identity {
  struct pfs_pool_id pool;
  struct pfs_volume_id volume;
  struct pfs_object_id object;
  /* Immutable read-only generation; zero for live writer views. */
  uint64_t generation;
  uint16_t kind;
};

struct pfs_view_metadata {
  struct pfs_view_identity identity;
  /* File byte length or directory entry count, selected by identity.kind. */
  uint64_t size;
};

struct pfs_view_entry {
  struct pfs_name name;
  uint16_t kind;
};

/* Ordinary metadata discloses identity, kind and size/count only; file.read
 * alone does not authorize metadata. Outputs are copied only on success. */
enum pfs_status pfs_view_metadata(struct pfs_view *view,
                                 struct pfs_view_metadata *out);
/* Inspect returns only policy owner and explicit grants on this object. It
 * requires admin.inspect and does not grant read, list or metadata authority.
 * NULL grants with zero capacity returns owner and required count. Otherwise
 * capacity must cover all explicit grants. Outputs stay unchanged on failure. */
enum pfs_status pfs_view_inspect(struct pfs_view *view,
                                struct pfs_principal_id *owner,
                                struct pfs_grant_record *grants,
                                size_t capacity, size_t *count);
/* Invalid calls leave read_count unchanged. Valid calls report the proved
 * prefix, including zero on permission denial; remaining bytes are unspecified. */
enum pfs_status pfs_view_read(struct pfs_view *view, uint64_t offset,
                             void *buffer, size_t length, size_t *read_count);

/* A cursor independently retains the volume and captures list authority. Names
 * and kinds are the entire output: listing reveals neither IDs nor child views.
 * Close the cursor explicitly. Capacity must be positive; empty/end returns
 * count zero and done true. No disk pointer is exposed as a cursor. */
enum pfs_status pfs_view_directory_open(struct pfs_view *view,
                                       struct pfs_view_directory **out);
enum pfs_status pfs_view_directory_next(struct pfs_view_directory *cursor,
                                       struct pfs_view_entry *out,
                                       size_t capacity, size_t *count,
                                       bool *done);
enum pfs_status pfs_view_directory_close(struct pfs_view_directory **cursor);

/* Stateless LIST page in unsigned name order. Zero after starts; next is an
 * opaque 64-bit resume point interpreted only against this held immutable view.
 * Tokens carry no authority or provenance and retain no resources. They can be
 * replayed/forked; even a forged valid point may skip entries within this view.
 * Capacity is positive and at most PFS_RECORD_COUNT_MAX. On success, next is the
 * last returned point (after when count is zero), and done signals exhaustion.
 * Repeating a terminal point returns zero entries/done. Every failure preserves
 * entries, count, done and next. All buffers/outputs are disjoint from the view
 * and each other. Invalid token paths return INVALID; rooted media, format and
 * resource errors retain their statuses. Ordinary page proof is not a full-list
 * count reconciliation or uniqueness check across pages. */
enum pfs_status pfs_view_directory_page(struct pfs_view *view, uint64_t after,
                                       struct pfs_view_entry *out, size_t capacity,
                                       size_t *count, bool *done, uint64_t *next);

/* Nonempty relative paths derive a child using held rights only. Subtree scope
 * and dir.lookup are required on every containing directory. Requested rights
 * must be contained in held masks; the child cannot expand the held root or
 * become a subtree if it is a file. Identity/kind are published with the view
 * without requiring separate metadata authority. No fresh policy is acquired. */
enum pfs_status pfs_view_lookup(struct pfs_view *view,
                               const uint8_t *path, size_t length,
                               enum pfs_grant_scope scope,
                               const struct pfs_rights *requested,
                               struct pfs_view **out,
                               struct pfs_view_identity *identity);

/* Delegate this held identity with exactly requested rights. Rights and scope
 * may only narrow; object scope on files cannot carry directory rights. No
 * namespace or fresh policy acquisition occurs, including for retained orphans. */
enum pfs_status pfs_view_delegate(struct pfs_view *view,
  enum pfs_grant_scope scope, const struct pfs_rights *requested,
  struct pfs_view **out, struct pfs_view_identity *identity);

#endif
