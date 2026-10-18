/* AERA userspace SMB/SFTP network storage manager. */
#ifndef AERA_NAS_MANAGER_HPP
#define AERA_NAS_MANAGER_HPP
#include <atomic>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <vector>

class NasManager {
public:
    static const std::string Mount_Point, Config_Dir, Config_File, Cache_Dir, Log_File, Native_Binary;
    static bool IsFuseAvailable();
    static bool IsMounted();
    static bool Mount();
    static bool Unmount();
    static bool SelectAsStorage();
    static bool RefreshStatus();
    static bool ResetTransferStats();
    static bool WaitForPendingUploads(int max_seconds = 1800, uint64_t expected_upload_bytes = 0,
                                     const std::atomic<bool>* cancel = nullptr);
    static std::string GetLastError();
private:
    static std::string last_error;
    static void SetError(const std::string&);
    static bool EnsureDirectory(const std::string&, mode_t = 0777);
    static bool WriteFile(const std::string&, const std::string&, mode_t = 0600);
    static bool ReadFile(const std::string&, std::string&);
    static bool RunDetached(const std::vector<std::string>&);
    static bool BuildNativeConfig();
    static std::string GetValue(const std::string&, const std::string&);
    static std::string Trim(const std::string&);
};
#endif
