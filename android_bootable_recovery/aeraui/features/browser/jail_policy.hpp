// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <vector>

namespace aera_jail {
// Applied to the entire browser process tree before exec. Unknown syscalls
// fail closed. No user/PID namespace support is needed by this policy.
inline std::vector<sock_filter> Policy() {
  std::vector<sock_filter> p;
  auto stmt = [&](uint16_t code, uint32_t k) { p.push_back({code, 0, 0, k}); };
  auto jump = [&](uint16_t code, uint32_t k, uint8_t yes, uint8_t no) {
    p.push_back({code, yes, no, k});
  };
  auto load = [&](unsigned offset) { stmt(BPF_LD | BPF_W | BPF_ABS, offset); };
  auto ret = [&](uint32_t action) { stmt(BPF_RET | BPF_K, action); };
  const uint32_t denied = SECCOMP_RET_ERRNO | EPERM;
  load(offsetof(seccomp_data, arch));
#if defined(__aarch64__)
  jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
#elif defined(__x86_64__)
  jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
#else
#error Unsupported browser jail architecture
#endif
  ret(SECCOMP_RET_KILL_PROCESS);
  load(offsetof(seccomp_data, nr));
  auto special = [&](int nr, const auto &body) {
    const size_t at = p.size(); jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 0);
    body();
    p[at].jf = static_cast<uint8_t>(p.size() - at - 1);
  };
  special(SYS_clone, [&] {
    load(offsetof(seccomp_data, args[0]) + 4);
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0); ret(denied);
    load(offsetof(seccomp_data, args[0]));
    // fork/vfork/pthreads only: no new namespaces, CLONE_PARENT or tracing.
    constexpr uint32_t allowed = 0xff | 0x100 | 0x200 | 0x400 | 0x800 |
        0x4000 | 0x10000 | 0x40000 | 0x80000 | 0x100000 | 0x200000 | 0x400000 | 0x1000000;
    // 0x400000 is the obsolete CLONE_DETACHED bit still set by musl pthreads.
    stmt(BPF_ALU | BPF_AND | BPF_K, ~allowed);
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0); ret(denied); ret(SECCOMP_RET_ALLOW);
  });
#ifdef SYS_clone3
  special(SYS_clone3, [&] { ret(SECCOMP_RET_ERRNO | ENOSYS); });
#endif
  special(SYS_socket, [&] {
    load(offsetof(seccomp_data, args[0]));
    // GLib's threaded resolver creates a private AF_UNIX stream socket before
    // issuing DNS requests. The browser has a private chroot/run directory,
    // a dedicated UID and no listen permission; allow only stream sockets
    // here, while continuing to reject datagram/raw Unix sockets.
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 5);
    load(offsetof(seccomp_data, args[1]));
    stmt(BPF_ALU | BPF_AND | BPF_K, ~(SOCK_CLOEXEC | SOCK_NONBLOCK));
    jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 1);
    ret(SECCOMP_RET_ALLOW);
    ret(denied);
    // Address-family discovery used by libc before DNS. The browser runs as
    // an unprivileged dedicated UID with an empty capability set.
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 0, 1);
    ret(SECCOMP_RET_ALLOW);
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0);
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 4);
    load(offsetof(seccomp_data, args[1]));
    stmt(BPF_ALU | BPF_AND | BPF_K, ~(SOCK_CLOEXEC | SOCK_NONBLOCK));
    jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 2, 0);
    jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_DGRAM, 1, 0);
    ret(denied); ret(SECCOMP_RET_ALLOW);
  });
  special(SYS_socketpair, [&] {
    load(offsetof(seccomp_data, args[0]));
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0); ret(denied); ret(SECCOMP_RET_ALLOW);
  });
  special(SYS_prctl, [&] {
    load(offsetof(seccomp_data, args[0]));
    // Linux 4.19 reports EINVAL when userspace asks about capability numbers
    // added by newer kernels. Existing browser runtimes check through CAP 40
    // and interpret that EINVAL as a broken jail. Capabilities above the
    // kernel's last supported number cannot be present, so answer those
    // read-only queries as absent while retaining the real kernel check for
    // every capability this kernel understands.
    jump(BPF_JMP | BPF_JEQ | BPF_K, PR_CAPBSET_READ, 0, 7);
    load(offsetof(seccomp_data, args[1]) + 4);
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0); ret(denied);
    load(offsetof(seccomp_data, args[1]));
    jump(BPF_JMP | BPF_JGE | BPF_K, 38, 1, 0);
    ret(SECCOMP_RET_ALLOW);
    ret(SECCOMP_RET_ERRNO);
    load(offsetof(seccomp_data, args[0]));
    const int allowed[] = {PR_GET_NO_NEW_PRIVS, PR_GET_SECCOMP, PR_GET_DUMPABLE,
      PR_GET_NAME, PR_SET_NAME, PR_GET_PDEATHSIG,
      PR_GET_TIMERSLACK, PR_SET_TIMERSLACK};
    for (int option : allowed) {
      jump(BPF_JMP | BPF_JEQ | BPF_K, option, 0, 1); ret(SECCOMP_RET_ALLOW);
    }
    jump(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_DUMPABLE, 0, 4);
    load(offsetof(seccomp_data, args[1]));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1); ret(SECCOMP_RET_ALLOW); ret(denied);
    jump(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_NO_NEW_PRIVS, 0, 4);
    load(offsetof(seccomp_data, args[1]));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 1); ret(SECCOMP_RET_ALLOW); ret(denied);
    ret(denied);
  });
  // Deliberately absent: mount/chroot/setns/unshare, ptrace/process_vm*,
  // pidfd*, bpf/perf/keyrings, credential changes, raw sockets and listen,
  // setsid/setpgid (children must remain in the launcher's process group).
  const int allowed[] = {
    SYS_read, SYS_write, SYS_readv, SYS_writev, SYS_pread64, SYS_pwrite64,
    SYS_openat, SYS_close, SYS_lseek, SYS_fstat, SYS_newfstatat, SYS_fstatfs,
    SYS_statfs, SYS_faccessat, SYS_readlinkat, SYS_getdents64, SYS_fcntl,
    SYS_ioctl,
    SYS_dup, SYS_dup3, SYS_pipe2, SYS_ftruncate, SYS_fallocate, SYS_fadvise64,
    SYS_fsync, SYS_fdatasync, SYS_mkdirat, SYS_unlinkat, SYS_renameat,
    SYS_linkat, SYS_symlinkat, SYS_fchmod, SYS_fchmodat, SYS_umask,
    SYS_utimensat, SYS_chdir, SYS_fchdir, SYS_getcwd,
    SYS_mmap, SYS_mprotect, SYS_munmap, SYS_mremap, SYS_madvise, SYS_brk,
    SYS_msync, SYS_mlock, SYS_munlock, SYS_memfd_create,
    SYS_rt_sigaction, SYS_rt_sigprocmask, SYS_rt_sigreturn, SYS_sigaltstack,
    SYS_rt_sigsuspend, SYS_rt_sigtimedwait, SYS_restart_syscall,
    SYS_getpid, SYS_getppid, SYS_gettid, SYS_getuid, SYS_geteuid, SYS_getgid,
    SYS_getegid, SYS_getresuid, SYS_getresgid, SYS_getgroups, SYS_getpgid,
    SYS_getsid, SYS_uname, SYS_sysinfo, SYS_getrandom, SYS_getcpu, SYS_capget,
    SYS_futex, SYS_set_tid_address, SYS_set_robust_list, SYS_get_robust_list,
    SYS_sched_yield, SYS_sched_getaffinity, SYS_sched_setaffinity,
    SYS_sched_setscheduler,
    SYS_sched_getparam, SYS_sched_getscheduler, SYS_sched_get_priority_max,
    SYS_sched_get_priority_min, SYS_getpriority, SYS_setpriority,
    SYS_clock_gettime, SYS_clock_getres, SYS_gettimeofday, SYS_nanosleep,
    SYS_clock_nanosleep, SYS_getrusage, SYS_getrlimit, SYS_prlimit64,
    SYS_timer_create, SYS_timer_settime, SYS_timer_gettime, SYS_timer_delete,
    SYS_timerfd_create, SYS_timerfd_settime, SYS_timerfd_gettime,
    SYS_eventfd2, SYS_epoll_create1, SYS_epoll_ctl, SYS_epoll_pwait,
    SYS_ppoll, SYS_pselect6, SYS_signalfd4,
    // musl binds DNS UDP clients to an ephemeral local port before sendto.
    // Socket creation is already restricted above and listen remains denied.
    SYS_bind, SYS_connect, SYS_sendto, SYS_recvfrom, SYS_sendmsg, SYS_recvmsg,
    SYS_sendmmsg, SYS_recvmmsg, SYS_shutdown, SYS_getsockopt, SYS_setsockopt,
    SYS_getsockname, SYS_getpeername,
    SYS_execve, SYS_wait4, SYS_waitid, SYS_exit, SYS_exit_group,
#ifdef SYS_statx
    SYS_statx,
#endif
#ifdef SYS_faccessat2
    SYS_faccessat2,
#endif
#ifdef SYS_renameat2
    SYS_renameat2,
#endif
#ifdef SYS_rseq
    SYS_rseq,
#endif
#ifdef SYS_membarrier
    SYS_membarrier,
#endif
#ifdef SYS_epoll_pwait2
    SYS_epoll_pwait2,
#endif
#if defined(__x86_64__)
    SYS_open, SYS_access, SYS_stat, SYS_lstat, SYS_readlink, SYS_poll,
    SYS_epoll_wait, SYS_dup2, SYS_arch_prctl, SYS_time,
#endif
  };
  for (int nr : allowed) {
    jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1); ret(SECCOMP_RET_ALLOW);
  }
  // Signals are limited by the dedicated UID at the kernel credential check.
  // WebKit needs these to stop its own children and suspend its own threads.
  for (int nr : {SYS_kill, SYS_tgkill, SYS_tkill}) {
    jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1); ret(SECCOMP_RET_ALLOW);
  }
  ret(denied);
  return p;
}
}  // namespace aera_jail
