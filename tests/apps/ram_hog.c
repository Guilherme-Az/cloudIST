/* Tries to reserve 1.5 GiB. With RLIMIT_AS = 1 GiB (VM with 1 GB RAM) this fails. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/ram.%d", getenv("RESULT_DIR"), (int)getpid());
  void *p = malloc((size_t)1536 << 20);
  FILE *f = fopen(path, "w");
  if (f) { fputs(p == NULL ? "LIMITED\n" : "NOT_LIMITED\n", f); fclose(f); }
  free(p);
  return 0;
}
