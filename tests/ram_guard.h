/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_TEST_RAM_GUARD_H
#define PFS_TEST_RAM_GUARD_H

/* Unprivileged Linux host tests require a capped, unswappable scratch mount
 * and cgroup. Provisioning owns capacities; guards verify actual finite bounds.
 * Failure exits before fixture writes; success pins TMPDIR for the process. */
void pfs_test_require_ram(void);
/* Quick CI only: fresh fixture pages use the verified job's zero-swap cgroup.
 * TMPDIR must start empty; heavier runners still require mount-level noswap. */
void pfs_test_require_ci_ram(void);
/* Borrowed descriptor: callers must not close it or resolve TMPDIR again. */
int pfs_test_ram_directory_fd(void);

#endif
