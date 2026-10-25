/*
 * LD_PRELOAD helper for run_tests.sh (test-only, not part of the project).
 *
 * 1. "Application start" snapshot: when the program is about to start a VM
 *    application, a listing of /tmp/CloudIST-114255 (type, size, path) is appended to
 *    $CLOUDIST_TEST_LOG. Start is detected by:
 *      - the base-code stub message "VM execution not implemented" (exercise 1)
 *      - fork()/execve()/execv()/execvp() (exercise 2 onwards)
 *    The test script compares this snapshot to the input directory, which proves
 *    the copy had already finished when the application started.
 *
 * 2. File descriptor leak check: at exit, every open fd other than 0/1/2 is
 *    written to the log as "OPENFD <n> <target>".
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLOUD_ROOT "/tmp/CloudIST-114255"
#define START_MSG "VM execution not implemented"

static int log_fd = -1;

/* Only act inside cloudIST itself, not in wrappers such as timeout. */
static int enabled(void) {
  extern char *program_invocation_short_name;
  return strcmp(program_invocation_short_name, "cloudIST") == 0;
}

static int open_log(void) {
  const char *path = getenv("CLOUDIST_TEST_LOG");
  if (path == NULL || !enabled()) return -1;
  return open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
}

static int snap_cb(const char *fpath, const struct stat *sb, int flag, struct FTW *ftw) {
  if (ftw->level == 0) return 0;
  const char *rel = fpath + strlen(CLOUD_ROOT) + 1;
  if (flag == FTW_D || flag == FTW_DNR)
    dprintf(log_fd, "d 0 %s\n", rel);
  else if (flag == FTW_SL)
    dprintf(log_fd, "l 0 %s\n", rel);
  else
    dprintf(log_fd, "f %lld %s\n", (long long)sb->st_size, rel);
  return 0;
}

static void snapshot(const char *why) {
  log_fd = open_log();
  if (log_fd < 0) return;
  /* Several VM children can take a snapshot at the same time: lock the log so
   * their lines don't get mixed. The lock is released by close(). */
  flock(log_fd, LOCK_EX);
  dprintf(log_fd, "SNAPSHOT %s\n", why);
  nftw(CLOUD_ROOT, snap_cb, 16, FTW_PHYS);
  dprintf(log_fd, "ENDSNAPSHOT\n");
  close(log_fd);
  log_fd = -1;
}

static void check_msg(const char *s, size_t n) {
  if (s != NULL && memmem(s, n, START_MSG, strlen(START_MSG)) != NULL)
    snapshot("stub-message");
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
  static size_t (*real)(const void *, size_t, size_t, FILE *);
  if (!real) real = dlsym(RTLD_NEXT, "fwrite");
  if (stream == stderr) check_msg(ptr, size * nmemb);
  return real(ptr, size, nmemb, stream);
}

int fputs(const char *s, FILE *stream) {
  static int (*real)(const char *, FILE *);
  if (!real) real = dlsym(RTLD_NEXT, "fputs");
  if (stream == stderr) check_msg(s, strlen(s));
  return real(s, stream);
}

int fprintf(FILE *stream, const char *fmt, ...) {
  if (stream == stderr) check_msg(fmt, strlen(fmt));
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(stream, fmt, ap);
  va_end(ap);
  return r;
}

pid_t fork(void) {
  static pid_t (*real)(void);
  if (!real) real = dlsym(RTLD_NEXT, "fork");
  snapshot("fork");
  return real();
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  static int (*real)(const char *, char *const[], char *const[]);
  if (!real) real = dlsym(RTLD_NEXT, "execve");
  snapshot("execve");
  return real(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
  static int (*real)(const char *, char *const[]);
  if (!real) real = dlsym(RTLD_NEXT, "execv");
  snapshot("execv");
  return real(path, argv);
}

int execvp(const char *file, char *const argv[]) {
  static int (*real)(const char *, char *const[]);
  if (!real) real = dlsym(RTLD_NEXT, "execvp");
  snapshot("execvp");
  return real(file, argv);
}

/* execl/execlp are variadic: collect the arguments and call the real execv/execvp. */
#define MAX_EXEC_ARGS 64
static void collect_args(const char *arg0, va_list ap, char **argv) {
  int i = 0;
  argv[i++] = (char *)arg0;
  while (i < MAX_EXEC_ARGS - 1 && (argv[i] = va_arg(ap, char *)) != NULL) i++;
  argv[i] = NULL;
}

int execl(const char *path, const char *arg0, ...) {
  static int (*real)(const char *, char *const[]);
  if (!real) real = dlsym(RTLD_NEXT, "execv");
  char *argv[MAX_EXEC_ARGS];
  va_list ap;
  va_start(ap, arg0);
  collect_args(arg0, ap, argv);
  va_end(ap);
  snapshot("execl");
  return real(path, argv);
}

int execlp(const char *file, const char *arg0, ...) {
  static int (*real)(const char *, char *const[]);
  if (!real) real = dlsym(RTLD_NEXT, "execvp");
  char *argv[MAX_EXEC_ARGS];
  va_list ap;
  va_start(ap, arg0);
  collect_args(arg0, ap, argv);
  va_end(ap);
  snapshot("execlp");
  return real(file, argv);
}

__attribute__((destructor)) static void report_open_fds(void) {
  int fd = open_log();
  if (fd < 0) return;
  DIR *d = opendir("/proc/self/fd");
  if (d != NULL) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      int n = atoi(e->d_name);
      if (e->d_name[0] == '.' || n <= 2 || n == fd || n == dirfd(d)) continue;
      char link[64], target[4096];
      snprintf(link, sizeof(link), "/proc/self/fd/%d", n);
      ssize_t len = readlink(link, target, sizeof(target) - 1);
      target[len < 0 ? 0 : len] = '\0';
      dprintf(fd, "OPENFD %d %s\n", n, target);
    }
    closedir(d);
  }
  close(fd);
}
