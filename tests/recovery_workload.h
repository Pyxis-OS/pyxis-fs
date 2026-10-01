/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_RECOVERY_WORKLOAD_H
#define PFS_RECOVERY_WORKLOAD_H

#include <stdint.h>

/* Explicit quiescent source census; never fetches repositories or adjusts the
 * agreed 4 GiB/E=8192/M=4096/128 MiB profile after an unexpected refusal. */
void run_recovery_workload(uint64_t seed, const char *source_path);

#endif
