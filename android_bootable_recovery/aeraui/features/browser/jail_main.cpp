// SPDX-License-Identifier: Apache-2.0
// Trusted, non-setuid launcher. Invoked by root recovery, not by web content.
#include "jail_policy.hpp"
#include <libminijail.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr uid_t kBrowserUid = 99090;
static constexpr uid_t kTelegramUid = 99091;
static constexpr uid_t kMediaUid = 99092;
static constexpr uid_t kRecorderUid = 99093;
static constexpr uid_t kDoomUid = 99094;
static constexpr gid_t kMediaRwGid = 1023;
static constexpr gid_t kInetGid = 3003;
static void Die(const char *message) { perror(message); _exit(78); }
static void Check(int result, const char *what) { if (result < 0) Die(what); }
static void CloseUnrelatedDescriptors() {
  if (!syscall(SYS_close_range, 5U, ~0U, 0)) return;
  if (errno != ENOSYS) Die("close unrelated descriptors");

  const int directory = open("/proc/self/fd",
      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) Die("open descriptor directory");
  DIR *entries = fdopendir(directory);
  if (!entries) {
    close(directory);
    Die("read descriptor directory");
  }
  const int iterator = dirfd(entries);
  errno = 0;
  while (dirent *entry = readdir(entries)) {
    char *end = nullptr;
    const long descriptor = strtol(entry->d_name, &end, 10);
    if (*entry->d_name && end && !*end && descriptor >= 5 &&
        descriptor != iterator) {
      close(static_cast<int>(descriptor));
    }
    errno = 0;
  }
  if (errno) {
    closedir(entries);
    Die("enumerate descriptors");
  }
  closedir(entries);
}
static bool SupportsProcSubsetPid() {
  utsname version{};
  unsigned major = 0;
  unsigned minor = 0;
  return !uname(&version) &&
      sscanf(version.release, "%u.%u", &major, &minor) == 2 &&
      (major > 5 || (major == 5 && minor >= 8));
}
static volatile sig_atomic_t stopping;
static void Stop(int) { stopping = 1; }
static bool WriteFile(const std::string &path, const char *value) {
  int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  const auto size = strlen(value);
  const bool ok = write(fd, value, size) == static_cast<ssize_t>(size);
  close(fd); return ok;
}
static void PrepareRetroStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "RetroArch", 0770) && errno != EEXIST)
    Die("RetroArch storage directory");
  int retro = openat(aera, "RetroArch",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (retro < 0) Die("safe RetroArch storage directory");
  Check(fchown(retro, 0, kMediaRwGid), "RetroArch storage ownership");
  Check(fchmod(retro, 0770), "RetroArch storage permissions");
  for (const char *name : {"Saves", "States", "System"}) {
    if (mkdirat(retro, name, 0770) && errno != EEXIST)
      Die("RetroArch data directory");
    int child = openat(retro, name,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) Die("safe RetroArch data directory");
    Check(fchown(child, 0, kMediaRwGid), "RetroArch data ownership");
    Check(fchmod(child, 0770), "RetroArch data permissions");
    close(child);
  }
  close(retro);
}
static void PrepareDoomStorage() {
  // Internal storage can be unavailable before decryption. The bundled WAD
  // remains playable; persistent saves and user WADs become available once
  // /sdcard is mounted.
  int aera = open("/sdcard/AERA",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) return;
  if (mkdirat(aera, "Doom", 0770) && errno != EEXIST) {
    close(aera);
    return;
  }
  int doom = openat(aera, "Doom",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (doom < 0) return;
  Check(fchown(doom, 0, kMediaRwGid), "Doom storage ownership");
  Check(fchmod(doom, 0770), "Doom storage permissions");
  close(doom);
}
static void PrepareRecorderStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "Recordings", 0770) && errno != EEXIST)
    Die("AERA recordings directory");
  int recordings = openat(aera, "Recordings",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (recordings < 0) Die("safe AERA recordings directory");
  Check(fchown(recordings, 0, kMediaRwGid), "recordings ownership");
  Check(fchmod(recordings, 0770), "recordings permissions");
  close(recordings);
}
static void PrepareBrowserStorage() {
  int aera = open("/sdcard/AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) Die("AERA storage directory");
  if (mkdirat(aera, "Downloads", 0770) && errno != EEXIST)
    Die("AERA downloads directory");
  int downloads = openat(aera, "Downloads",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (downloads < 0) Die("safe AERA downloads directory");
  // WebKit writes as its dedicated UID; Android keeps shared-storage access
  // through media_rw. The browser's private profile below remains separate.
  Check(fchown(downloads, kBrowserUid, kMediaRwGid), "downloads ownership");
  Check(fchmod(downloads, 02770), "downloads permissions");
  close(downloads);

  // Keep browser identity outside shared/MTP storage. Only the final profile
  // directory is writable by WebKit; every parent remains root-owned and is
  // opened without following symlinks.
  int parent = open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent < 0) Die("browser profile data root");
  // /data/recovery is deliberately media_rw-owned on AERA devices, so it
  // cannot be part of the trusted parent chain for private browser identity.
  // Use a dedicated root-owned subtree directly below /data instead.
  for (const char *name : {"aera-recovery", "browser"}) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST)
      Die("browser profile parent");
    int child = openat(parent, name,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (child < 0) Die("safe browser profile parent");
    struct stat info{};
    if (fstat(child, &info) || !S_ISDIR(info.st_mode) || info.st_uid)
      Die("unsafe browser profile parent");
    parent = child;
  }
  if (mkdirat(parent, "profile", 0700) && errno != EEXIST)
    Die("browser profile directory");
  int profile = openat(parent, "profile",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(parent);
  if (profile < 0) Die("safe browser profile directory");
  Check(fchown(profile, kBrowserUid, kBrowserUid), "browser profile ownership");
  Check(fchmod(profile, 0700), "browser profile permissions");
  close(profile);
}
static void PrepareTelegramStorage() {
  int aera = open("/sdcard/AERA",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (aera < 0) return;
  if (mkdirat(aera, "Telegram", 0770) && errno != EEXIST) {
    close(aera);
    return;
  }
  int telegram = openat(aera, "Telegram",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (telegram < 0) return;
  // The isolated client can write only completed downloads to this narrow
  // directory. It never receives write access to the rest of shared storage.
  if (fchown(telegram, kTelegramUid, kMediaRwGid) != 0 && errno != EPERM)
    Die("Telegram download ownership");
  if (fchmod(telegram, 0770) != 0 && errno != EPERM)
    Die("Telegram download permissions");
  close(telegram);
}
static void WriteResolverConfig(int root) {
  const int etc = openat(root, "etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (etc < 0) Die("browser resolver directory");
  std::string contents;
  const char *keys[] = {"net.wlan0.dns1", "net.wlan0.dns2", "net.dns1", "net.dns2"};
  for (const char *key : keys) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(key, value) <= 0) continue;
    in_addr address4{};
    in6_addr address6{};
    if (inet_pton(AF_INET, value, &address4) != 1 &&
        inet_pton(AF_INET6, value, &address6) != 1) continue;
    const std::string line = std::string("nameserver ") + value + "\n";
    if (contents.find(line) == std::string::npos) contents += line;
  }
  if (contents.empty()) contents = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
  const int output = openat(etc, "resolv.conf",
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  close(etc);
  if (output < 0) Die("browser resolver file");
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = write(output, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { close(output); Die("browser resolver write"); }
    offset += static_cast<size_t>(written);
  }
  close(output);
}
// Root supervisor has no web engine and accepts no browser commands. Its only
// job is to bound memory and reclaim this exact process tree, even if WebKit
// crashes or the native UI disappears. No PID/user namespace is required.
static void Supervise(const std::string &root, const char *memory_limit) {
  // A Streams scene keeps its media worker alive while launching short-lived
  // extractor queries from the same runtime directory. A root-only group name
  // makes those independent supervisors collide at mkdir(), so use the trusted
  // launcher PID to give every process tree its own bounded resource group.
  const std::string name = root.substr(5) + "-" + std::to_string(getpid());
  const std::string tasks = "/dev/cg2_bpf/" + name;
  const std::string memory = "/dev/memcg/" + name;
  Check(mkdir(tasks.c_str(), 0700), "private browser task group");
  if (mkdir(memory.c_str(), 0700)) { rmdir(tasks.c_str()); Die("private browser memory group"); }
  int kill_file = open((tasks + "/cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  int killer = kill_file < 0 ? -1 : fcntl(kill_file, F_DUPFD_CLOEXEC, 5);
  if (kill_file >= 0) close(kill_file);
  // cgroup.kill was added after Linux 5.10. Some otherwise fully capable
  // recovery kernels (including Garnet's) therefore expose the Android task
  // and memory controllers without that file. Keep the hard memory bound and
  // fall back to the jail's dedicated process group for tree termination.
  if (!WriteFile(memory + "/memory.limit_in_bytes", memory_limit)) {
    if (killer >= 0) close(killer);
    rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser resource limits");
  }
  struct stat swap{};
  if (!stat((memory + "/memory.memsw.limit_in_bytes").c_str(), &swap) &&
      !WriteFile(memory + "/memory.memsw.limit_in_bytes", memory_limit)) {
    close(killer); rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser swap limit");
  }
  struct sigaction action{}; action.sa_handler = Stop; sigemptyset(&action.sa_mask);
  for (int signal : {SIGTERM, SIGINT, SIGHUP}) Check(sigaction(signal, &action, nullptr), "supervisor signals");
  const pid_t parent = getppid();
  Check(prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0), "supervisor lifetime");
  if (getppid() != parent) stopping = 1;
  const pid_t child = fork();
  if (child < 0) {
    if (killer >= 0) close(killer);
    rmdir(memory.c_str()); rmdir(tasks.c_str()); Die("browser fork");
  }
  if (!child) {
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::signal(signal, SIG_DFL);
    Check(setsid(), "private browser process group");
    if (!WriteFile(tasks + "/cgroup.procs", "0") || !WriteFile(memory + "/cgroup.procs", "0"))
      Die("join browser resource groups");
    if (killer >= 0) close(killer);
    return;
  }
  // The supervisor must not keep the UI connection alive after worker exit.
  close(3); close(4);
  int status = 0;
  while (!stopping) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) break;
    if (result < 0 && errno != EINTR) { stopping = 1; break; }
    usleep(100000);
  }
  // Newer kernels provide atomic cgroup tree termination. Linux 5.10 devices
  // use the dedicated process group instead; the seccomp policy denies
  // setsid/setpgid after sandbox entry, so descendants cannot escape it.
  if (killer >= 0) {
    if (write(killer, "1", 1) != 1) Die("stop browser process tree");
    close(killer);
  } else {
    if (kill(-child, SIGKILL) && errno != ESRCH) Die("stop browser process group");
    if (kill(child, SIGKILL) && errno != ESRCH) Die("stop browser leader");
  }
  if (stopping) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  bool removed_memory = false, removed_tasks = false;
  for (int attempt = 0; attempt < 50 && (!removed_memory || !removed_tasks); ++attempt) {
    if (!removed_memory) removed_memory = !rmdir(memory.c_str());
    if (!removed_tasks) removed_tasks = !rmdir(tasks.c_str());
    if (!removed_memory || !removed_tasks) usleep(100000);
  }
  if (!removed_memory || !removed_tasks) fprintf(stderr, "Browser resource group cleanup incomplete\n");
  _exit(!stopping && WIFEXITED(status) ? WEXITSTATUS(status) : 78);
}
int main(int argc, char **argv) {
  const bool streams_engine_mode = argc >= 4 && argc <= 6 &&
      !strcmp(argv[1], "--streams-engine");
  if ((!streams_engine_mode && argc != 3) || (strcmp(argv[1], "--probe") &&
      strcmp(argv[1], "--browser") && strcmp(argv[1], "--retroarch") &&
      strcmp(argv[1], "--doom") &&
      strcmp(argv[1], "--telegram") && strcmp(argv[1], "--media") &&
      strcmp(argv[1], "--recorder") && strcmp(argv[1], "--streams-media") &&
      strcmp(argv[1], "--streams-engine")) || getuid() || geteuid()) {
    fprintf(stderr, "Usage (root recovery only): aera-browser-jail MODE RUNTIME [COMMAND [ARGUMENT]]\n");
    return 78;
  }
  const bool retroarch = !strcmp(argv[1], "--retroarch");
  const bool doom = !strcmp(argv[1], "--doom");
  const bool telegram = !strcmp(argv[1], "--telegram");
  const bool media = !strcmp(argv[1], "--media");
  const bool recorder = !strcmp(argv[1], "--recorder");
  const bool browser = !strcmp(argv[1], "--browser");
  const bool streams_media = !strcmp(argv[1], "--streams-media");
  const bool streams_engine = !strcmp(argv[1], "--streams-engine");
  const std::string root = argv[2];
  const size_t prefix = (streams_media || streams_engine) ? 18 :
      doom ? 15 : recorder ? 14 : media ? 16 :
      (retroarch || telegram) ? 13 : 14;
  const char *expected = retroarch ? "/tmp/aera-ra-" :
      doom ? "/tmp/aera-doom-" :
      telegram ? "/tmp/aera-tg-" : media ? "/tmp/aera-media-" :
      recorder ? "/tmp/aera-rec-" :
      (streams_media || streams_engine) ? "/tmp/aera-streams-" :
      "/tmp/aera-web-";
  if (retroarch) PrepareRetroStorage();
  if (doom) PrepareDoomStorage();
  if (recorder) PrepareRecorderStorage();
  if (browser) PrepareBrowserStorage();
  if (telegram) PrepareTelegramStorage();
  if (root.size() != prefix + 6 || root.compare(0, prefix, expected) ||
      !std::all_of(root.begin() + prefix, root.end(),
                   [](unsigned char c) { return std::isalnum(c); })) return 78;
  int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{}; struct statfs filesystem{};
  if (directory < 0 || fstat(directory, &info)) Die("private RAM runtime");
  const mode_t runtime_mode = info.st_mode & 0777;
  if (info.st_uid || (runtime_mode != 0700 && runtime_mode != 0755) ||
      fstatfs(directory, &filesystem) ||
      (filesystem.f_type != 0x01021994 && filesystem.f_type != 0x858458f6)) Die("private RAM runtime");
  for (const char *name : {"etc", "proc", "tmp", "dev", "run", "storage",
                           "sdcard", "state", "recordings", "downloads",
                           "profile", "doom", "mnt", "usb_otg",
                           "external_sd"}) {
    if (mkdirat(directory, name, 0755) && errno != EEXIST) Die("runtime directory");
    struct stat child{};
    if (fstatat(directory, name, &child, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(child.st_mode) || child.st_uid)
      Die("unsafe runtime directory");
  }
  if (telegram) {
    const int mnt = openat(directory, "mnt",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (mnt < 0 || (mkdirat(mnt, "nas", 0755) && errno != EEXIST))
      Die("Telegram NAS mount point");
    close(mnt);
  }
  if (retroarch) {
    const int sdcard = openat(directory, "sdcard",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sdcard < 0 || (mkdirat(sdcard, "AERA", 0755) && errno != EEXIST))
      Die("RetroArch AERA mount point");
    close(sdcard);
    const int storage = openat(directory, "storage",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (storage < 0 || (mkdirat(storage, "AERA", 0755) && errno != EEXIST))
      Die("RetroArch compatibility mount point");
    close(storage);
  }
  if (!retroarch && !doom && !media && !recorder && !streams_media)
    WriteResolverConfig(directory);
  Check(fchmod(directory, 0755), "runtime root permissions");
  close(directory);
  Supervise(root, (telegram || doom) ? "536870912" :
      (media || recorder || streams_media) ? "1073741824" : "1610612736");
  const rlim_t file_limit = (browser || telegram) ? 16ULL << 30 :
      recorder ? 2ULL << 30 : 64ULL << 20;
  rlimit files{512U, 512U}, processes{192U, 192U}, core{0, 0},
      size{file_limit, file_limit};
  Check(setrlimit(RLIMIT_NOFILE, &files), "file descriptor limit");
  Check(setrlimit(RLIMIT_NPROC, &processes), "process limit");
  Check(setrlimit(RLIMIT_CORE, &core), "core limit");
  Check(setrlimit(RLIMIT_FSIZE, &size), "file size limit");
  // Close recovery/ADB inherited descriptors. Only stdio, pixels (3), and
  // browser-only control (4) survive. No mount/device/data descriptor leaks.
  CloseUnrelatedDescriptors();
  auto *jail = minijail_new();
  if (!jail) Die("minijail_new");
  minijail_namespace_vfs(jail);
  minijail_namespace_uts(jail);
  // Use recovery's already-configured WLAN route. The browser still runs as a
  // dedicated UID inside its chroot with no capabilities and a socket-filtered
  // seccomp policy; it cannot listen, open raw sockets, or alter routes.
  Check(minijail_namespace_set_hostname(jail,
      retroarch ? "aera-retroarch" :
      doom ? "aera-doom" :
      telegram ? "aera-telegram" : media ? "aera-media" :
        streams_media ? "aera-streams-media" :
        streams_engine ? "aera-streams-engine" :
        recorder ? "aera-recorder" : "aera-browser"), "private hostname");
  Check(minijail_enter_chroot(jail, root.c_str()), "private filesystem");
  Check(minijail_bind(jail, root.c_str(), "/", 0), "read-only runtime");
  const unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC;
  const char *proc_options =
      SupportsProcSubsetPid() ? "hidepid=2,subset=pid" : "hidepid=2";
  Check(minijail_mount_with_data(jail, "proc", "/proc", "proc", flags | MS_RDONLY,
                                proc_options), "private proc view");
  Check(minijail_mount_with_data(jail, "tmpfs", "/tmp", "tmpfs", flags,
                                "size=512M,mode=1777"), "private temporary storage");
  Check(minijail_mount_with_data(jail, "tmpfs", "/run", "tmpfs", flags,
                                "size=16M,mode=0755,uid=99090,gid=99090"), "private runtime storage");
  Check(minijail_mount_with_data(jail, "tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC,
                                "size=1M,mode=0755"), "private devices");
  for (const char *device : {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"})
    Check(minijail_bind(jail, device, device, 1), "safe character device");
  // The browser receives one GPU character device plus the system DMA heap
  // Turnip needs for Vulkan external-memory file descriptors. It receives no
  // display, input, camera, Binder or storage devices; exposing the wider host
  // /dev tree or Android's vendor EGL stack is unnecessary.
  if (!retroarch && !doom && !telegram && !media && !recorder &&
      !streams_media && !streams_engine) {
    Check(minijail_bind(jail, "/dev/kgsl-3d0", "/dev/kgsl-3d0", 1),
          "browser GPU device");
    if (!access("/dev/dma_heap/system", R_OK | W_OK)) {
      Check(minijail_bind(jail, "/dev/dma_heap/system", "/dev/dma_heap/system", 1),
            "browser system DMA heap");
    } else if (!access("/dev/ion", R_OK | W_OK)) {
      // Android 10-era Qualcomm kernels predate DMA-BUF heaps. Turnip retains
      // its legacy ION allocator for these devices.
      Check(minijail_bind(jail, "/dev/ion", "/dev/ion", 1),
            "browser system ION allocator");
    }
    if (browser)
      Check(minijail_bind(jail, "/sdcard/AERA/Downloads", "/downloads", 1),
            "writable AERA downloads");
    if (browser)
      Check(minijail_bind(jail, "/data/aera-recovery/browser/profile",
                          "/profile", 1),
            "private browser profile");
  } else if (retroarch && !access("/sdcard/AERA", R_OK | W_OK)) {
    // RetroArch receives only AERA's directory, never the rest of /sdcard.
    // ROMs, saves, states and screenshots can persist under this narrow mount.
    Check(minijail_bind(jail, "/sdcard/AERA", "/sdcard/AERA", 1),
          "AERA emulation content");
    // Keep compatibility with already-published RetroArch configurations that
    // started their browser at /storage/AERA. Both mounts name the same narrow
    // AERA directory; neither exposes the remainder of /sdcard.
    Check(minijail_bind(jail, "/sdcard/AERA", "/storage/AERA", 1),
          "legacy AERA emulation path");
  } else if (doom && !access("/sdcard/AERA/Doom", R_OK | W_OK)) {
    Check(minijail_bind(jail, "/sdcard/AERA/Doom", "/doom", 1),
          "writable Doom data");
  } else if (telegram) {
    Check(minijail_bind(jail, "/data/recovery/AERA/telegram", "/state", 1),
          "encrypted Telegram state");
    if (!access("/sdcard", R_OK))
      Check(minijail_bind(jail, "/sdcard", "/sdcard", 0),
            "read-only Telegram attachments");
    if (!access("/mnt/nas", R_OK))
      Check(minijail_bind(jail, "/mnt/nas", "/mnt/nas", 0),
            "read-only Telegram NAS attachments");
    if (!access("/usb_otg", R_OK))
      Check(minijail_bind(jail, "/usb_otg", "/usb_otg", 0),
            "read-only Telegram OTG attachments");
    if (!access("/external_sd", R_OK))
      Check(minijail_bind(jail, "/external_sd", "/external_sd", 0),
            "read-only Telegram external storage attachments");
    if (!access("/sdcard/AERA/Telegram", R_OK | W_OK))
      Check(minijail_bind(jail, "/sdcard/AERA/Telegram", "/downloads", 1),
            "writable Telegram downloads");
  } else if (media && !access("/sdcard", R_OK)) {
    Check(minijail_bind(jail, "/sdcard", "/sdcard", 0),
          "read-only media library");
  } else if (recorder && !access("/sdcard/AERA/Recordings", R_OK | W_OK)) {
    Check(minijail_bind(jail, "/sdcard/AERA/Recordings", "/recordings", 1),
          "writable AERA recordings");
  }
  Check(minijail_mount_with_data(jail, "tmpfs", "/dev/shm", "tmpfs", flags,
                                "size=128M,mode=1777"), "private shared memory");
  minijail_change_uid(jail, telegram ? kTelegramUid :
      (media || streams_media || streams_engine) ? kMediaUid :
      recorder ? kRecorderUid :
      doom ? kDoomUid :
      kBrowserUid);
  minijail_change_gid(jail, telegram ? kTelegramUid :
      (media || streams_media || streams_engine) ? kMediaUid :
      recorder ? kRecorderUid :
      doom ? kDoomUid :
      kBrowserUid);
  // Android kernels gate Internet sockets through AID_INET. Grant that one
  // non-storage group only to the browser; media plugins retain their narrow
  // media_rw access and every other mode clears supplementary groups.
  const bool media_access = retroarch || doom || telegram || media || recorder ||
      streams_media || streams_engine;
  const gid_t group = browser ? kInetGid : (media_access ? kMediaRwGid : 0);
  minijail_set_supplementary_gids(jail, (browser || media_access) ? 1 : 0,
                                  &group);
  minijail_use_caps(jail, 0);
  minijail_no_new_privs(jail);
  const bool probe = !strcmp(argv[1], "--probe");
  auto policy = aera_jail::Policy();
  sock_fprog filter{static_cast<unsigned short>(policy.size()), policy.data()};
  minijail_set_seccomp_filters(jail, &filter);
  minijail_use_seccomp_filter(jail);
  // Any failed mount, privilege drop or filter load terminates the helper.
  minijail_enter(jail);
  Check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0), "non-dumpable process");
  std::string locale = getenv("AERA_LOCALE") ? getenv("AERA_LOCALE") : "en";
  if (locale.empty() || locale.size() > 16 ||
      !std::all_of(locale.begin(), locale.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
      })) {
    locale = "en";
  }
  std::string locale_env = "AERA_LOCALE=" + locale;
  std::string lang_env = "LANG=" + locale + ".UTF-8";
  char *const browser_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/tmp"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>(lang_env.c_str()),
    const_cast<char *>(locale_env.c_str()),
    const_cast<char *>("XDG_RUNTIME_DIR=/run"), const_cast<char *>("XDG_CACHE_HOME=/tmp/cache"),
    const_cast<char *>("XDG_DATA_HOME=/tmp/data"), const_cast<char *>("XDG_CONFIG_HOME=/tmp/config"),
    const_cast<char *>("XDG_DATA_DIRS=/usr/share"), const_cast<char *>("GSETTINGS_BACKEND=memory"),
    const_cast<char *>("GIO_MODULE_DIR=/usr/lib/gio/modules"),
    // Recovery has no system D-Bus or desktop network manager. Force GIO's
    // local implementations so proxy/network discovery cannot turn a missing
    // desktop service into a resolver failure before libc sends DNS.
    const_cast<char *>("GIO_USE_PROXY_RESOLVER=dummy"),
    const_cast<char *>("GIO_USE_NETWORK_MONITOR=base"),
    // WebKit coordinated graphics -> Mesa EGL -> Zink -> Turnip -> KGSL.
    // Output remains a WPE SHM frame; the browser never gets the recovery
    // framebuffer or the trusted UI's EGL context.
    const_cast<char *>("VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json"),
    const_cast<char *>("MESA_LOADER_DRIVER_OVERRIDE=zink"),
    const_cast<char *>("LIBGL_DRIVERS_PATH=/usr/lib/dri"),
    const_cast<char *>("EGL_PLATFORM=surfaceless"),
    const_cast<char *>("ZINK_DESCRIPTORS=lazy"),
    // Keep WebKit's independently updated video and page layers ordered on
    // Turnip, and finish SHM readback before publishing the composed frame.
    const_cast<char *>("ZINK_DEBUG=sync,flushsync"),
    const_cast<char *>("MESA_SHADER_CACHE_DISABLE=true"),
    // Keep multimedia discovery completely inside the extracted runtime. The
    // registry belongs in the private writable tmpfs; the runtime itself and
    // recovery ramdisk remain read-only.
    const_cast<char *>("GST_PLUGIN_SYSTEM_PATH=/usr/lib/gstreamer-1.0"),
    const_cast<char *>("GST_PLUGIN_SCANNER=/usr/libexec/gstreamer-1.0/gst-plugin-scanner"),
    const_cast<char *>("GST_REGISTRY=/tmp/gstreamer-registry.bin"),
    const_cast<char *>("GST_REGISTRY_REUSE_PLUGIN_SCANNER=no"),
    // The software Cairo tile painter defaults to half the detected cores.
    // Dodge has six fast cores available in recovery; use all six so complex
    // pages do not serialize repaint work behind touch input.
    const_cast<char *>("WEBKIT_CAIRO_PAINTING_THREADS=6"),
    const_cast<char *>("ICU_DATA=/usr/share/icu/76.1"),
    const_cast<char *>("FONTCONFIG_PATH=/etc/fonts"),
    const_cast<char *>("XKB_CONFIG_ROOT=/usr/share/X11/xkb"),
    nullptr};
  char *const retroarch_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/tmp"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>(lang_env.c_str()),
    const_cast<char *>(locale_env.c_str()),
    const_cast<char *>("XDG_RUNTIME_DIR=/run"),
    const_cast<char *>("XDG_CACHE_HOME=/tmp/cache"),
    const_cast<char *>("XDG_DATA_HOME=/tmp/data"),
    const_cast<char *>("XDG_CONFIG_HOME=/tmp/config"), nullptr};
  char *const doom_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/doom"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>(lang_env.c_str()),
    const_cast<char *>(locale_env.c_str()), nullptr};
  char *const telegram_environment[] = {
    const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("HOME=/state"),
    const_cast<char *>("TMPDIR=/tmp"), const_cast<char *>(lang_env.c_str()),
    const_cast<char *>(locale_env.c_str()),
    nullptr};
  const char *program = probe ? "/usr/bin/aera-jail-probe" :
      retroarch ? "/usr/bin/retroarch" :
      doom ? "/usr/bin/aera-doom" :
      telegram ? "/usr/bin/aera-telegram" :
      media ? "/usr/bin/aera-media" :
      streams_media ? "/usr/bin/aera-streams-media" :
      streams_engine ? "/usr/lib/jvm/java-17-openjdk/bin/java" :
      recorder ? "/usr/bin/aera-recorder" :
      "/usr/bin/aera-browser-worker";
  char *const browser_args[] = {const_cast<char *>(program),
    const_cast<char *>(probe ? "--inside" : "--isolated-ipc-v1"), nullptr};
  char *const retroarch_args[] = {const_cast<char *>(program),
    const_cast<char *>("--config"), const_cast<char *>("/etc/retroarch.cfg"),
    const_cast<char *>("--menu"), nullptr};
  char *const doom_args[] = {const_cast<char *>(program), nullptr};
  char *const telegram_args[] = {const_cast<char *>(program),
    const_cast<char *>("--isolated-ipc-v1"), nullptr};
  char *const media_args[] = {const_cast<char *>(program), nullptr};
  char *const streams_media_args[] = {const_cast<char *>(program), nullptr};
  char *streams_engine_args[] = {
    const_cast<char *>(program), const_cast<char *>("-jar"),
    const_cast<char *>("/usr/share/aera-streams/engine.jar"),
    argc >= 4 ? argv[3] : nullptr, argc >= 5 ? argv[4] : nullptr,
    argc >= 6 ? argv[5] : nullptr, nullptr};
  char *const recorder_args[] = {const_cast<char *>(program), nullptr};
  execve(program, retroarch ? retroarch_args : doom ? doom_args :
         telegram ? telegram_args : media ? media_args :
         streams_media ? streams_media_args :
         streams_engine ? streams_engine_args :
         recorder ? recorder_args : browser_args,
         retroarch ? retroarch_environment : doom ? doom_environment :
         telegram ? telegram_environment :
         browser_environment);
  Die("exec isolated browser");
}
