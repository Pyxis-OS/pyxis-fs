/* SPDX-License-Identifier: MPL-2.0 */
#include "support.h"
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

#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void run_baseline_tests(void);
void run_namespace_tests(void);

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

int
main(int argc, char **argv)
{
  if (argc != 3 || strcmp(argv[1], "--suite") ||
      (strcmp(argv[2], "pr") && strcmp(argv[2], "file-workloads"))) {
    fputs("usage: pyxis-fs-tests --suite pr|file-workloads\n", stderr);
    return 2;
  }
  UNITY_BEGIN();
  if (!strcmp(argv[2], "file-workloads")) {
    run_file_workloads();
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
  }
  UNITY_END();
  return Unity.NumberOfTests == 0 || Unity.TestFailures != 0 ||
         Unity.TestIgnores != 0 ? 1 : 0;
}
