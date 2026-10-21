/*
 * Unit test driver for copy_path_recursive() (test-only).
 * Calls the function directly, so the copy logic can be tested even if
 * main.c never reaches it.
 *
 * Usage: unit_copy <src_dir> <dst_dir>
 * Prints: ret=<return value> leaked_fds=<n>
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>

#include "../../filesystem.h"

static int count_fds(void) {
  int n = 0;
  DIR *d = opendir("/proc/self/fd");
  if (d == NULL) return -1;
  while (readdir(d) != NULL) n++;
  closedir(d);
  return n;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <src_dir> <dst_dir>\n", argv[0]);
    return 2;
  }
  int before = count_fds();
  int ret = copy_path_recursive(argv[1], argv[2]);
  int after = count_fds();
  printf("ret=%d leaked_fds=%d\n", ret, after - before);
  fflush(stdout); /* LeakSanitizer exits without flushing stdio */
  return 0;
}
