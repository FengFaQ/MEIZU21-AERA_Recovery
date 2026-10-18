/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failed;
static void *thread_test(void *arg) { return arg; }
static void check(int ok, const char *name) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  failed |= !ok;
}
int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--inside")) return 78;
  check(getuid() == 99090 && geteuid() == 99090 && getgid() == 99090, "dedicated unprivileged UID/GID");
  check(getgroups(0, NULL) == 0, "no inherited supplementary groups");
  pthread_t thread; void *answer = NULL;
  int thread_result = pthread_create(&thread, NULL, thread_test, &failed);
  check(!thread_result && !pthread_join(thread, &answer) && answer == &failed, "musl thread creation and joining");
  check(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, "no-new-privileges");
  check(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2, "seccomp filter active");
  check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0, "can protect process memory");
  check(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1 && errno == EPERM, "cannot re-enable dumpability");
  struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct caps[2] = {{0}};
  check(syscall(SYS_capget, &header, caps) == 0 && !caps[0].effective && !caps[1].effective &&
        !caps[0].permitted && !caps[1].permitted && !caps[0].inheritable && !caps[1].inheritable, "zero capabilities");
  int bounded = 0;
  for (int i = 0; i <= 40; ++i) if (prctl(PR_CAPBSET_READ, i, 0, 0, 0) > 0) bounded = 1;
  check(!bounded, "zero capability bounding set");
  check(open("/data", O_RDONLY | O_DIRECTORY) == -1, "decrypted data absent");
  check(open("/dev/block", O_RDONLY | O_DIRECTORY) == -1, "raw partitions absent");
  check(open("/proc/1/root", O_RDONLY | O_DIRECTORY) == -1, "host root not reachable through proc");
  check(open("/proc/1/mem", O_RDONLY) == -1, "host process memory inaccessible");
  check(open("/escape-test", O_CREAT | O_WRONLY, 0600) == -1 && errno == EROFS, "runtime root read-only");
  int tmp = open("/tmp/probe", O_CREAT | O_EXCL | O_WRONLY, 0600);
  check(tmp >= 0 && write(tmp, "ok", 2) == 2, "private temporary directory writable");
  if (tmp >= 0) close(tmp);
  int unix_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  check(unix_socket == -1 && errno == EPERM, "host Unix sockets denied");
  if (unix_socket >= 0) close(unix_socket);
  int network = socket(AF_INET, SOCK_STREAM, 0);
  check(network >= 0, "ordinary TCP socket available (no connection attempted)");
  if (network >= 0) close(network);
  int raw = socket(AF_INET, SOCK_RAW, 1);
  check(raw == -1 && errno == EPERM, "raw socket denied");
  if (raw >= 0) close(raw);
  check(unshare(CLONE_NEWNS) == -1 && errno == EPERM, "namespace changes denied after setup");
  check(mount("none", "/tmp", "tmpfs", 0, NULL) == -1 && errno == EPERM, "mount changes denied after setup");
  check(chroot("/tmp") == -1 && errno == EPERM, "changing jail root denied");
  check(setsid() == -1 && errno == EPERM, "cannot leave managed process group");
  check(setuid(0) == -1 && errno == EPERM, "cannot regain root");
  printf("JAIL_PROBE_RESULT=%s\n", failed ? "FAIL" : "PASS");
  return failed ? 1 : 0;
}
