/* SPDX-License-Identifier: MPL-2.0 */
#include "support.h"
#include "ram_guard.h"
#include "build_tests.h"
#include "plan_tests.h"
#include "codec_tests.h"
#include "edit_tests.h"
#include "unity.h"
#include "live_tests.h"
#include "publication_tests.h"
#include "failure.h"
#include "failure_tests.h"
#include "admit_tests.h"
#include "file_tests.h"
#include "file_workloads.h"
#include "extended_tests.h"
#include "recovery_workload.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void run_baseline_tests(void);
void run_namespace_tests(void);
void run_namespace_failure_tests(void);

void
setUp(void)
{
}

void
tearDown(void)
{
  test_failures_cleanup();
  test_fixtures_cleanup();
}

static bool
parse_seed(const char *text, uint64_t *out)
{
  uint64_t value = 0;
  if (!*text) {
    return false;
  }
  for (; *text; text++) {
    if (*text < '0' || *text > '9' || value > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    value = value * 10 + (*text - '0');
  }
  *out = value;
  return value != 0;
}

static int
usage(void)
{
  fputs("usage: pyxis-fs-tests --suite pr|file-workloads\n"
    "       pyxis-fs-tests --suite extended --seed N\n"
    "       pyxis-fs-tests --suite workload --profile recovery --seed N --source PATH\n",
    stderr);
  return 2;
}

int
main(int argc, char **argv)
{
  pfs_test_require_ram();
  const char *suite = NULL, *profile = NULL, *source = NULL;
  uint64_t seed = 0;
  bool seed_set = false;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) {
      return usage();
    }
    if (!strcmp(argv[i], "--suite") && !suite) {
      suite = argv[i + 1];
    } else if (!strcmp(argv[i], "--profile") && !profile) {
      profile = argv[i + 1];
    } else if (!strcmp(argv[i], "--source") && !source) {
      source = argv[i + 1];
    } else if (!strcmp(argv[i], "--seed") && !seed_set && parse_seed(argv[i + 1], &seed)) {
      seed_set = true;
    } else {
      return usage();
    }
  }
  if (!suite) {
    return usage();
  }
  bool quick = !strcmp(suite, "pr"), files = !strcmp(suite, "file-workloads");
  bool extended = !strcmp(suite, "extended"), workload = !strcmp(suite, "workload");
  if ((!quick && !files && !extended && !workload) ||
      ((quick || files) && (seed_set || profile || source)) ||
      (extended && (!seed_set || profile || source)) ||
      (workload && (!seed_set || !profile || strcmp(profile, "recovery") || !source || !*source))) {
    return usage();
  }
  if (seed_set) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("suite=%s seed=%" PRIu64 "\n", suite, seed);
  }
  UNITY_BEGIN();
  if (files) {
    run_file_workloads();
  } else if (extended) {
    run_extended_edit_tests(seed);
    run_extended_tests(seed);
  } else if (workload) {
    run_recovery_workload(seed, source);
  } else {
    run_baseline_tests();
    run_build_tests();
    run_plan_tests();
    run_codec_tests();
    run_edit_tests();
    run_live_tests();
    run_failure_tests();
    run_admit_tests();
    run_publication_tests();
    run_file_tests();
    run_namespace_tests();
    run_namespace_failure_tests();
  }
  UNITY_END();
  return Unity.NumberOfTests == 0 || Unity.TestFailures != 0 ||
         Unity.TestIgnores != 0 ? 1 : 0;
}
