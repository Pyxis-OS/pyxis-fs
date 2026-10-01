/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_TEST_RAM_GUARD_H
#define PFS_TEST_RAM_GUARD_H

/* Linux host tests require a capped, unswappable scratch mount and cgroup.
 * Failure exits before fixture writes; success pins TMPDIR for the process. */
void pfs_test_require_ram(void);
/* Borrowed descriptor: callers must not close it or resolve TMPDIR again. */
int pfs_test_ram_directory_fd(void);

#endif
