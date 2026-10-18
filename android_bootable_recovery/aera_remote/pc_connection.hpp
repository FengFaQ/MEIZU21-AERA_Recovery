/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <aeraui/backend.hpp>
namespace Json { class Value; }

namespace aeraui::root { struct Progress; }

namespace aera::pc {
struct Status {
  bool enabled = false;
  bool remember_available = false;
  bool manually_disabled = false;
  std::string address;
  std::string ip_address;
  std::string usb_address;
  std::string certificate;
  std::string error;
  std::string pending_connection;
};
struct ConnectionRequest {
  std::string id;
  std::string name;
  std::string fingerprint;
  bool remember_available = false;
};
struct InstallRequest {
  std::string id;
  std::string name;
  std::string computer;
  uint64_t bytes = 0;
  aeraui::JobRequest job;
};
struct BackupTransferStatistics {
  uint64_t bytes = 0;
  double bytes_per_second = 0;
  bool finalizing = false;
  uint64_t waiting_seconds = 0;
};
struct UploadProgress {
  std::string id, name;
  uint64_t bytes = 0, received = 0;
  double bytes_per_second = 0, remaining_seconds = -1;
  bool active = false, finalizing = false, cancelling = false;
};
UploadProgress GetUploadProgress();
bool CancelUpload(const std::string &id);
BackupTransferStatistics GetBackupTransferStatistics();
bool SetEnabled(bool enabled, int port = 443, bool manual = true);
void RefreshNetwork();
// AERA Remote takes priority on port 80; the PC workspace remains on HTTPS.
void ReleaseHttpRedirect();
void RestoreHttpRedirect();
std::string HttpRedirectLocation(const std::string &host);
Status GetStatus();
bool NeedsUiAttention();
void SetRecoveryBusy(bool busy);
bool TakeRebootRequest(std::string *target);
// These methods are called only by the UI thread, never by network handlers.
void RefreshPlatformInfo();
void CleanupOldTransfers();
bool TakeConnectionRequest(ConnectionRequest *request);
void ResolveConnection(const std::string &id, bool allow, bool remember);
bool TakeInstallRequest(InstallRequest *request);
bool BeginInstall(const std::string &id);
void RejectInstall(const std::string &id);
void UpdateInstall(int percent, const std::string &detail, const std::string &log);
void CompleteInstall(const std::string &id, int result);
// Called by the root job worker after its inspection has finished.
void UpdateRootInspection(const aeraui::root::Progress &progress);
void UpdateBackupResult(const Json::Value &result);
struct BackupCancellation {
  std::string id;
  bool active = false, restore = false, requested = false;
};
BackupCancellation GetBackupCancellation();
bool CancelBackup(const std::string &id, bool acknowledge_partial_restore);
void UpdateInstallerPrompt(const aeraui::InstallerPrompt &prompt);
void UpdateInstallerPresentation(const aeraui::InstallerPresentation &presentation);
bool TakePromptAnswer(std::string *id, bool *accepted);
bool ForgetComputers();
}  // namespace aera::pc
