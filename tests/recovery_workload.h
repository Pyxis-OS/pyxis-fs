/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PFS_RECOVERY_WORKLOAD_H
#define PFS_RECOVERY_WORKLOAD_H

#include <stdbool.h>
#include <stdint.h>

/* Explicit quiescent source census; never fetches repositories or adjusts the
 * chosen 4 GiB/E=8192/M=4096/128 MiB measurement configuration after refusal.
 * True only when the requested history and independent verification complete.
 * A verified healthy capacity refusal leaves correctness assertions passing,
 * but returns false because the requested history remains incomplete. */
bool run_recovery_workload(uint64_t seed, const char *source_path);

#endif
