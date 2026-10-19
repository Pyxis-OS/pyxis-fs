/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_FAT32_MOUNTED_H
#define PYXIS_FS_FAT32_MOUNTED_H

#include <pyxis_fs/fat32_write.h>

#define FAT32_MOUNTED_PAYLOAD_MAX (1024u * 1024u)

enum fat32_mounted_phase {
  FAT32_MOUNTED_SNAPSHOT,
  FAT32_MOUNTED_MARK,
  FAT32_MOUNTED_DATA,
  FAT32_MOUNTED_FAT,
  FAT32_MOUNTED_DIRECTORY,
  FAT32_MOUNTED_RECLAIM,
  FAT32_MOUNTED_FSINFO,
  FAT32_MOUNTED_CLEAN,
  FAT32_MOUNTED_MIRROR,
};

struct fat32_mounted;
/* Caller embeds records and supplies disjoint chain storage. Library fields are
 * mutable, never copied/edited by the caller. IDs, authority and reference counts
 * are caller-owned. An attached locator is canonical within this mount. */
struct fat32_mounted_object {
  struct fat32_mounted *mounted;
  struct fat32_mounted_object *previous, *next;
  struct fat32_entry entry;
  struct fat32_locator locator;
  uint32_t *chain;
  uint32_t chain_capacity, chain_count;
  uint64_t read_epoch, append_epoch;
  bool attached;
};
struct fat32_mounted_recovery {
  enum fat32_status failure;
  uint8_t phase;
  uint64_t failed_sector;
  uint32_t failed_sector_count; /* Any prefix of this write may have taken effect. */
  struct fat32_mounted_object *source, *victim, *target_parent;
  char source_name[FAT32_NAME_MAX + 1], target_name[FAT32_NAME_MAX + 1];
  const struct fat32_prepared_sector *prepared_sectors;
  size_t prepared_sector_count;
  const uint32_t *allocated_chain;
  uint32_t allocated_clusters;
  bool uncertain;
  const uint8_t *mirror_image; /* Retained repair buffer; failed reads may be partial. */
};
struct fat32_mounted_batch {
  struct fat32_source backing;
  void *memory, *context;
  struct fat32_prepared_sector *sectors;
  uint8_t *images, *run, *quarantine;
  size_t *index, *order;
  size_t count, capacity, slots, quarantine_bytes;
  uint32_t quarantined;
};
struct fat32_mounted {
  struct fat32_volume volume;
  struct fat32_write_io io;
  struct fat32_mounted_object *objects;
  uint32_t object_count;
  uint8_t *allocation;
  size_t allocation_bytes;
  uint32_t free_count, next_free;
  uint64_t read_epoch, append_epoch; /* Zero permanently disables proof reuse after wrap. */
  bool allocation_valid, dirty_owned, mutated, stopped;
  struct fat32_mounted_recovery recovery;
  struct fat32_mounted_batch batch;
};

/* Serialized, nonreentrant calls; caller excludes all competing media writers.
 * All storage and callback contexts remain alive through calls and held objects.
 * If stopped, retain the first recovery record's backing unchanged with this mount
 * (operation workspace or persistent batch backing);
 * no operation, retry, reclamation or rebinding is then permitted. No heap.
 * changed_bytes bounds content images (gap and cluster slack included);
 * allocated_clusters bounds new candidate storage and changed_clusters bounds
 * distinct FAT entries across allocation/reclamation phases. Work/sector limits
 * are finite. Allocation backing remains separate from replaceable workspace.
 * Allocation of both workspace and sufficient object chain capacity must
 * precede the operation/dirty mark.
 * max_write_sectors is nonzero and matches the mounted I/O callback bound;
 * workspace includes a disjoint run buffer of that many logical sectors.
 * Namespace insertion can grow its parent by at most two clusters; reserve
 * parent.chain_count + 2 before create/rename. */
enum fat32_status fat32_mounted_workspace_size(const struct fat32_volume *volume,
    uint64_t changed_bytes, uint32_t allocated_clusters, uint32_t changed_clusters,
    uint32_t max_write_sectors, size_t *bytes);
/* Allocation backing is caller-owned, retained until this mount is forgotten.
 * CHECK rebuilds in its reader scratch and copies to allocation only after
 * full validation succeeds. The destination may be the existing retained map,
 * but remains disjoint from workspace. Backing/capacity refusal before validation
 * preserves proof. Validation failure leaves proof invalid and allocation bytes
 * unchanged. Neither CHECK nor reopening may reset an uncertain stopped writer. */
enum fat32_status fat32_mounted_allocation_size(const struct fat32_volume *volume, size_t *bytes);
/* Includes image/descriptor/index backing, a separate checkpoint run buffer and
 * scratch context, and quarantine bits. image_bytes is a tunable target, not a
 * file limit: an individually larger operation checkpoints synchronously.
 * The caller retains this disjoint, max_align_t-aligned backing for the whole
 * mounted lifetime, including indefinitely after uncertain I/O. */
enum fat32_status fat32_mounted_batch_size(const struct fat32_volume *volume,
    size_t image_bytes, uint32_t max_write_sectors, size_t *bytes);
bool fat32_mounted_pending(const struct fat32_mounted *mounted);
/* Serialized under the same exclusive ownership as other mounted operations.
 * Ends regular-file read-proof reuse without I/O, allocation, changing allocation
 * proof or resetting a stopped writer. Adapters call before WRITE attempts;
 * the engine consumes append eligibility. Other attempts/errors use both below. */
void fat32_mounted_invalidate_reads(struct fat32_mounted *mounted);
/* Also ends strict-EOF append proof. Use for other mutation/lifecycle attempts
 * and every bound error, including adapter errors outside engine operations.
 * Before WRITE, invalidate reads only; the engine owns append eligibility. */
void fat32_mounted_invalidate_proofs(struct fat32_mounted *mounted);
/* OPEN requires a fresh zero-initialized record; it never reopens/reset one. */
enum fat32_status fat32_mounted_open(struct fat32_mounted *mounted,
    const struct fat32_volume *volume, const struct fat32_write_io *io,
    uint8_t *allocation, size_t allocation_bytes, void *batch_memory,
    size_t batch_bytes, size_t image_bytes, struct fat32_workspace *workspace);
enum fat32_status fat32_mounted_check(struct fat32_mounted *mounted,
    uint8_t *allocation, size_t allocation_bytes, struct fat32_workspace *workspace,
    struct fat32_check_report *report);
/* Selects and locally validates root, including any canonical held root record.
 * Outputs change only on success; detected backing/structure errors invalidate
 * allocation proof. This does not bind a new object or perform a full CHECK. */
enum fat32_status fat32_mounted_root(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace, struct fat32_entry *entry, struct fat32_locator *locator);
enum fat32_status fat32_mounted_object_chain_size(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace,
    uint32_t *count);
enum fat32_status fat32_mounted_object_bind(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace,
    struct fat32_mounted_object *object);
enum fat32_status fat32_mounted_object_reserve(struct fat32_mounted_object *object,
    uint32_t *chain, uint32_t capacity);
/* Forget only after final caller reference drains. Attached records need no
 * media effects; detached reclamation transfers its images/quarantine into the
 * mount-owned batch before the record unlinks.
 * A failure leaves this record and its chain retained. Pending mirror repair
 * refuses detached reclamation with READ_ONLY; forgetting never repairs. */
enum fat32_status fat32_mounted_object_forget(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace);
/* Regular files retain geometry/metadata/complete-chain proof in an unchanged
 * exclusive mount epoch while allocation proof is valid. Reads use pending images
 * where present, otherwise the backing source; read proofs cache no payload.
 * Mutation/CHECK/sync/forget attempts and mounted-operation errors invalidate it.
 * Directory validation remains strict. Append reuse is separate. Epoch wrap
 * disables reuse. */
enum fat32_status fat32_mounted_locate(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace, struct fat32_entry *entry);
enum fat32_status fat32_mounted_lookup(struct fat32_mounted_object *directory,
    const char *component, size_t bytes, struct fat32_workspace *workspace,
    struct fat32_entry *entry, struct fat32_locator *child);
enum fat32_status fat32_mounted_enumerate(struct fat32_mounted_object *directory,
    uint64_t continuation, struct fat32_workspace *workspace,
    struct fat32_entry *entry, struct fat32_locator *child, uint64_t *next, bool *found);
enum fat32_status fat32_mounted_read_range(struct fat32_mounted_object *object,
    uint64_t offset, size_t bytes, struct fat32_workspace *workspace,
    fat32_bytes_sink_fn sink, void *context);
/* At most FAT32_MOUNTED_PAYLOAD_MAX bytes into caller-owned private backing,
 * stable and disjoint from workspace, records and outputs throughout this call.
 * Backing has at least bytes capacity; full sectors read directly into it.
 * read_bytes changes only on success, clipped at EOF. A failure may change any
 * destination prefix, including partial failed-read bytes; publish only success.
 * Uses the same read proof and invalidation rules as read_range. No allocation. */
enum fat32_status fat32_mounted_read_into(struct fat32_mounted_object *object,
    uint64_t offset, size_t bytes, struct fat32_workspace *workspace,
    void *destination, size_t *read_bytes);
/* Mutations publish into the mount-owned pending view before returning. They
 * may checkpoint for capacity or ordering dependencies; SYNC is the durability
 * point. No atomic rename or rollback across power loss.
 * Content mutation of a detached file uses its retained chain, never its old
 * reusable slot. New files are empty; directories contain only dot entries.
 * A strict positive-length EOF append retains its fully validated prefix only
 * through confirmed appends. Recheck geometry, parent, selected metadata and
 * old tail on each reuse. Other mutations, CHECK, lifecycle attempts and errors
 * end reuse; overwrite/gap/resize use full validation. Pending images own copies
 * of producer bytes, independently of this append proof. */
enum fat32_status fat32_mounted_write(struct fat32_mounted_object *object,
    uint64_t offset, const void *payload, size_t bytes, struct fat32_workspace *workspace);
enum fat32_status fat32_mounted_resize(struct fat32_mounted_object *object,
    uint64_t bytes, struct fat32_workspace *workspace);
enum fat32_status fat32_mounted_create(struct fat32_mounted_object *parent,
    const char *component, size_t bytes, bool directory,
    struct fat32_workspace *workspace, struct fat32_mounted_object *object);
enum fat32_status fat32_mounted_remove(struct fat32_mounted_object *parent,
    struct fat32_mounted_object *object, struct fat32_workspace *workspace);
/* Files only. victim is the canonical retained destination object, or NULL if
 * no destination exists. Replacement requires replace=true. Source identity
 * moves; a distinct victim detaches and keeps its own bytes/chain. */
enum fat32_status fat32_mounted_rename(struct fat32_mounted_object *source_parent,
    struct fat32_mounted_object *source, struct fat32_mounted_object *target_parent,
    const char *component, size_t bytes, bool replace,
    struct fat32_mounted_object *victim, struct fat32_workspace *workspace);
enum fat32_status fat32_mounted_sync(struct fat32_mounted *mounted);
/* Power seal retains every view. Detached allocated chains leave the mark dirty
 * and do not refuse power. A later mutation durably marks dirty again. Caller
 * enforces normal retirement drainage before using seal for retirement. */
enum fat32_status fat32_mounted_seal(struct fat32_mounted *mounted,
    struct fat32_workspace *workspace);

#endif
