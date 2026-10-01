/* SPDX-License-Identifier: MPL-2.0 */
#include "support.h"
#include "build_tests.h"
#include "plan_tests.h"
#include "codec_tests.h"
#include "edit_tests.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void run_baseline_tests(void);

void
setUp(void)
{
}

void
tearDown(void)
{
  test_fixtures_cleanup();
}

int
main(int argc, char **argv)
{
  if (argc != 3 || strcmp(argv[1], "--suite") || strcmp(argv[2], "pr")) {
    fputs("usage: pyxis-fs-tests --suite pr\n", stderr);
    return 2;
  }
  UNITY_BEGIN();
  run_baseline_tests();
  run_build_tests();
  run_plan_tests();
  run_codec_tests();
  run_edit_tests();
  UNITY_END();
  return Unity.NumberOfTests == 0 || Unity.TestFailures != 0 ||
         Unity.TestIgnores != 0 ? 1 : 0;
}
