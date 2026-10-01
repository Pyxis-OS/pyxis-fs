/* SPDX-License-Identifier: MPL-2.0 */
#include "inspect.h"
#include <pyxis_fs/check.h>

#include <string.h>

static void
report_problem(void *context, const struct pfs_check_event *event)
{
  (void)context;
  if (event->slot == PFS_POOL_NO_SELECTION) {
    fputs("Global: ", stdout);
  } else {
    printf("Slot %u: ", event->slot);
  }
  printf("%s: %s", event->operation, pfs_status_string(event->status));
  if (event->block != UINT64_MAX) {
    printf("; block=%llu", (unsigned long long)event->block);
  }
  static const uint8_t zero[PFS_ID_SIZE];
  char id[PFS_ID_TEXT_SIZE + 1];
  if (memcmp(event->volume.bytes, zero, PFS_ID_SIZE) != 0) {
    pfs_volume_id_format(&event->volume, id);
    printf("; volume=%s", id);
  }
  if (memcmp(event->object.bytes, zero, PFS_ID_SIZE) != 0) {
    pfs_object_id_format(&event->object, id);
    printf("; object=%s", id);
  }
  fputc('\n', stdout);
}

enum pfs_status
inspect_check(const struct pfs_block_reader *reader, struct pfs_memory *memory)
{
  struct pfs_check_result result = {0};
  enum pfs_status status = pfs_check(reader, memory, report_problem, NULL, &result);
  inspect_selection_print(&result.pool, reader->geometry.block_count);
  for (unsigned i = 0; i < 2; ++i) {
    const struct pfs_check_state_result *state = &result.state[i];
    printf("Slot %u check: %s; %s", i, state->complete ? "complete" : "incomplete",
           pfs_status_string(state->status));
    if (state->generation) {
      printf("; generation=%llu", (unsigned long long)state->generation);
    }
    if (result.pool.selected != PFS_POOL_NO_SELECTION) {
      fputs(result.pool.selected == i ? " (selected)" : " (retained)", stdout);
    }
    printf("\n  visited: volumes=%llu objects=%llu directory-entries=%llu extents=%llu grants=%llu orphans=%llu\n"
           "  claimed blocks: metadata=%llu file-data=%llu; unsupported-volumes=%llu\n",
           (unsigned long long)state->volumes, (unsigned long long)state->objects,
           (unsigned long long)state->directory_entries, (unsigned long long)state->file_extents,
           (unsigned long long)state->grants, (unsigned long long)state->orphans,
           (unsigned long long)state->metadata_blocks,
           (unsigned long long)state->file_blocks, (unsigned long long)state->unsupported_volumes);
  }
  printf("Across-state check: %s; %s\n",
         result.cross_complete ? "complete" : "incomplete",
         pfs_status_string(result.cross_status));
  if (status == PFS_OK) {
    fputs("Check complete: both retained states are structurally consistent.\n", stdout);
  } else {
    fputs("Check did not establish a clean two-state image.\n", stdout);
  }
  fputs("File payloads were not read and have no integrity checksums; contents are not verified.\n",
        stdout);
  return status;
}
