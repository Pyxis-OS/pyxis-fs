/* SPDX-License-Identifier: MPL-2.0 */
#include "check.h"

#include <string.h>

static int
usage(void)
{
  fputs("usage: fsck.npfs --image PATH [--replay]\n", stderr);
  return 2;
}

int
main(int argc, char **argv)
{
  const char *path = NULL;
  bool replay = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--image") && i + 1 < argc && !path)
      path = argv[++i];
    else if (!strcmp(argv[i], "--replay") && !replay)
      replay = true;
    else
      return usage();
  }
  if (!path)
    return usage();
  struct npfs_image image;
  enum npfs_status status = npfs_image_open(&image, path, replay, replay);
  if (status != NPFS_OK)
    return npfs_report("open", status, &image);
  if (image.degraded_header)
    fputs("warning: only one valid pool header; header redundancy is degraded\n", stderr);
  if (replay)
    status = npfs_replay(&image);
  if (status == NPFS_OK)
    status = npfs_check_image(&image);
  int result = status == NPFS_OK ? 0 : npfs_report("check", status, &image);
  enum npfs_status close_status = npfs_image_close(&image);
  if (!result && close_status != NPFS_OK)
    result = npfs_report("close", close_status, &image);
  if (!result)
    puts("structural check passed");
  return result;
}
