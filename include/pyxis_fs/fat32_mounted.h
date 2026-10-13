/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PYXIS_FS_FAT32_MOUNTED_H
#define PYXIS_FS_FAT32_MOUNTED_H

#include <pyxis_fs/fat32_write.h>

#define FAT32_MOUNTED_PAYLOAD_MAX 4080u

enum fat32_mounted_phase {
  FAT32_MOUNTED_SNAPSHOT,
  FAT32_MOUNTED_MARK,
  FAT32_MOUNTED_DATA,
  FAT32_MOUNTED_FAT,
  FAT32_MOUNTED_DIRECTORY,
  FAT32_MOUNTED_RECLAIM,
  FAT32_MOUNTED_FSINFO,
  FAT32_MOUNTED_CLEAN,
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
  bool attached;
};
struct fat32_mounted_recovery {
  enum fat32_status failure;
  uint8_t phase;
  uint64_t failed_sector;
  struct fat32_mounted_object *source, *victim, *target_parent;
  char source_name[FAT32_NAME_MAX + 1], target_name[FAT32_NAME_MAX + 1];
  const struct fat32_prepared_sector *prepared_sectors;
  size_t prepared_sector_count;
  const uint32_t *allocated_chain;
  uint32_t allocated_clusters;
  bool uncertain;
};
struct fat32_mounted {
  struct fat32_volume volume;
  struct fat32_write_io io;
  struct fat32_mounted_object *objects;
  uint32_t object_count;
  uint8_t *allocation;
  size_t allocation_bytes;
  uint32_t free_count, next_free;
  bool allocation_valid, dirty_owned, mutated, stopped;
  struct fat32_mounted_recovery recovery;
};

/* Serialized, nonreentrant calls; caller excludes all competing media writers.
 * All storage and callback contexts remain alive through calls and held objects.
 * If stopped, retain the failed operation workspace unchanged with this mount;
 * no operation, retry, reclamation or rebinding is then permitted. No heap.
 * changed_bytes bounds content images (gap and cluster slack included);
 * allocated_clusters bounds new candidate storage and changed_clusters bounds
 * distinct FAT entries across allocation/reclamation phases. Work/sector limits
 * are finite. Allocation backing remains separate from replaceable workspace.
 * Allocation of both workspace and sufficient object chain capacity must
 * precede the operation/dirty mark.
 * Namespace insertion can grow its parent by at most two clusters; reserve
 * parent.chain_count + 2 before create/rename. */
enum fat32_status fat32_mounted_workspace_size(const struct fat32_volume *volume,
    uint64_t changed_bytes, uint32_t allocated_clusters, uint32_t changed_clusters, size_t *bytes);
/* Allocation backing is caller-owned, retained until this mount is forgotten.
 * CHECK needs a disjoint replacement map; success transfers its borrowed lifetime
 * and permits the caller to release the old map. Backing/capacity refusal before
 * validation preserves proof. Validation failure leaves proof invalid, retains
 * the old map, and permits releasing the rejected replacement. Neither CHECK nor
 * reopening may reset an uncertain stopped writer. */
enum fat32_status fat32_mounted_allocation_size(const struct fat32_volume *volume, size_t *bytes);
/* OPEN requires a fresh zero-initialized record; it never reopens/reset one. */
enum fat32_status fat32_mounted_open(struct fat32_mounted *mounted,
    const struct fat32_volume *volume, const struct fat32_write_io *io,
    uint8_t *allocation, size_t allocation_bytes, struct fat32_workspace *workspace);
enum fat32_status fat32_mounted_check(struct fat32_mounted *mounted,
    uint8_t *allocation, size_t allocation_bytes, struct fat32_workspace *workspace,
    struct fat32_check_report *report);
enum fat32_status fat32_mounted_object_chain_size(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace,
    uint32_t *count);
enum fat32_status fat32_mounted_object_bind(struct fat32_mounted *mounted,
    const struct fat32_locator *locator, struct fat32_workspace *workspace,
    struct fat32_mounted_object *object);
enum fat32_status fat32_mounted_object_reserve(struct fat32_mounted_object *object,
    uint32_t *chain, uint32_t capacity);
/* Forget only after final caller reference drains. Attached records need no
 * media effects; detached chains are durably freed before the record unlinks.
 * A failure leaves this record and its chain retained. */
enum fat32_status fat32_mounted_object_forget(struct fat32_mounted_object *object,
    struct fat32_workspace *workspace);
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
/* All writes return only after ordered flushes. No atomic rename or rollback.
 * Content mutation of a detached file uses its retained chain, never its old
 * reusable slot. New files are empty; directories contain only dot entries. */
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
