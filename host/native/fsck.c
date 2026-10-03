/* SPDX-License-Identifier: MPL-2.0 */
#include "check.h"

#include <string.h>

static int
usage(void)
{
  fputs("usage: pyxisfs-native-fsck --image PATH [--replay]\n", stderr);
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
  struct native_image image;
  enum pnf_status status = native_image_open(&image, path, replay, replay);
  if (status != PNF_OK)
    return native_report("open", status, &image);
  if (image.degraded_header)
    fputs("warning: only one valid pool header; header redundancy is degraded\n", stderr);
  if (replay)
    status = native_replay(&image);
  if (status == PNF_OK)
    status = native_check_image(&image);
  int result = status == PNF_OK ? 0 : native_report("check", status, &image);
  enum pnf_status close_status = native_image_close(&image);
  if (!result && close_status != PNF_OK)
    result = native_report("close", close_status, &image);
  if (!result)
    puts("structural check passed");
  return result;
}
