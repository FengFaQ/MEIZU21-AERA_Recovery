// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <initializer_list>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aera_sandbox {
// Defense-in-depth recognition of the platform jail. The trusted launcher
// establishes the mounts and inherited filter; no environment opt-out exists.
// This is not a substitute for the launcher's policy or its device tests.
inline bool Validate() {
  gid_t groups[1] = {};
  const int group_count = getgroups(1, groups);
  if (getuid() != 99090 || geteuid() != 99090 || getgid() != 99090 ||
      getegid() != 99090 || group_count < 0 || group_count > 1 ||
      (group_count == 1 && groups[0] != 3003) ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
      prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2 ||
      getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) return false;
  struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct c[2] = {};
  if (syscall(SYS_capget, &h, c) || c[0].effective || c[1].effective ||
      c[0].permitted || c[1].permitted || c[0].inheritable || c[1].inheritable) return false;
  for (int i = 0; i <= 40; ++i) {
    errno = 0;
    const int state = prctl(PR_CAPBSET_READ, i, 0, 0, 0);
    if (state == 0) continue;
    if (state < 0 && errno == EINVAL) break;
    return false;
  }
  struct stat root = {}; struct statfs fs = {}; struct statvfs flags = {};
  if (stat("/", &root) || root.st_uid || (root.st_mode & 0777) != 0755 ||
      statfs("/", &fs) || (fs.f_type != 0x01021994 && fs.f_type != 0x858458f6) ||
      statvfs("/", &flags) || !(flags.f_flag & ST_RDONLY)) return false;
  for (const char *path : {"/data", "/dev/block", "/sys", "/proc/1"}) {
    struct stat info = {};
    if (lstat(path, &info) != -1 || errno != ENOENT) return false;
  }
  // exec resets dumpability, so every auxiliary process repeats this check
  // before initializing WebKit or accepting content.
  return prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0;
}
} // namespace aera_sandbox
