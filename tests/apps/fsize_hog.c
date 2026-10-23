/* Writes 1 byte at offset 1 GiB + 4 KiB (a sparse file: uses no real disk space).
 * With RLIMIT_FSIZE = 1 GiB (VM with 1 GB disk) this write fails with EFBIG. */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  signal(SIGXFSZ, SIG_IGN); /* otherwise the process is killed instead of getting EFBIG */
  char big[4096], path[4096];
  snprintf(big, sizeof(big), "%s/big.%d", getenv("RESULT_DIR"), (int)getpid());
  snprintf(path, sizeof(path), "%s/fsize.%d", getenv("RESULT_DIR"), (int)getpid());
  int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ssize_t w = fd < 0 ? -1 : pwrite(fd, "x", 1, ((off_t)1 << 30) + 4096);
  int limited = (w < 0 && errno == EFBIG);
  if (fd >= 0) close(fd);
  unlink(big);
  FILE *f = fopen(path, "w");
  if (f) { fputs(limited ? "LIMITED\n" : "NOT_LIMITED\n", f); fclose(f); }
  return 0;
}
