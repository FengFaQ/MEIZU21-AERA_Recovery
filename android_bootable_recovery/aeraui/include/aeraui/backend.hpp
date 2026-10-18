/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "aeraui/lock_theme.hpp"
#include "aeraui/home_layout.hpp"

namespace aeraui {
enum class Job {
  kInstall,
  kSideload,
  kFlashImage,
  kExtractPayload,
  kFlashPayload,
  kBackup,
  kUploadBackup,
  kRestore,
  kWipe,
  kMount,
  kUnmount,
  kFormatData,
  kClearSnapshotCow,
  kRootOperation,
  kPcBackupOperation,
  kFormatUsb,
  kNetworkStorage
};
struct Volume {
  std::string name;
  std::string path;
  uint64_t bytes = 0;
  bool selected = false;
  bool slot_select = false;
  bool logical = false;
};
struct AndroidUser {
  int id = 0;
  std::string name;
  int credential_type = 0;
  bool decrypted = false;
};
struct NasConfig {
  std::string type = "sftp";
  std::string host;
  std::string port = "22";
  std::string share = "Backups";
  std::string path;
  std::string user;
  std::string password;
  std::string domain = "WORKGROUP";
  std::string cache_mode = "off";
  std::string auth = "password";
  std::string key_name;
};
struct JobRequest {
  Job job = Job::kInstall;
  std::string title;
  std::string path;
  std::string name;
  std::vector<std::string> partitions;
  bool compression = true;
  bool digest = true;
  bool both_slots = false;
  std::string confirmation;
  std::string usb_identity, usb_filesystem;
  bool present_before_run = false;
  bool show_on_device = true;
  std::string root_action;
  std::string root_provider;
  std::string root_slot;
  std::string backup_action, backup_kind, backup_source, backup_method;
  std::string backup_storage, backup_snapshot;
  int backup_user = 0;
  NasConfig nas_config;
  std::string nas_action, nas_private_key;
  bool nas_password_update = false;
  std::string payload_manifest_hash;
  std::string payload_slot;
  bool payload_arb_acknowledged = false;
  std::string expected_image_sha256; // Only set for verified selected OTA images.
  uint64_t expected_image_bytes = 0;
  bool payload_direct = false;
  bool payload_full = false; // Every image is mandatory except explicitly protected partitions.
  bool payload_override_protection = false; // Explicit manual Advanced selection only.
};
inline bool PayloadTargetMatches(const std::string &name, const std::string &path) {
  return path == "/" + name || (name == "system" && path == "/system_root");
}
inline bool FormatDataAuthorized(const JobRequest &request) {
  return request.job == Job::kFormatData && request.path == "/data" &&
         request.confirmation == "yes" && request.partitions.empty();
}
inline bool SnapshotCowCleanupAuthorized(const JobRequest &request) {
  return request.job == Job::kClearSnapshotCow && request.path.empty() &&
         request.partitions.empty() && request.confirmation == "remove-cow";
}
// Implemented beside the stock GUI bridge, using the same recovery backend.
std::vector<Volume> RecoveryVolumes(const std::string &kind);
struct BackupStorage {
  std::string name, path;
  bool mounted = false;
  uint64_t free_bytes = 0;
};
std::vector<BackupStorage> RecoveryBackupStorages();
bool RecoveryPrepareBackupStorage(const std::string &path, std::string *error);
bool RecoveryBackupPathReady(const std::string &path);
std::string RecoveryAppVaultStorage();
bool RecoveryRememberAppVaultStorage(const std::string &path);
struct UsbFormatTarget {
  bool available = false;
  std::string name, path, block, filesystem, identity, error;
  uint64_t bytes = 0;
  std::vector<std::string> filesystems;
};
UsbFormatTarget RecoveryUsbFormatTarget(const std::string &path);
std::vector<Volume> RecoveryImageVolumes();
struct PayloadFlashTarget {
  std::string name;
  std::string path;
  uint64_t bytes = 0;
  bool raw = false;
  uint64_t device = 0;
  std::string reason;
};
enum class PayloadPhase { Queued, Preparing, Writing, Written, Verifying, Verified, Failed, NotFlashed };
struct PayloadPartitionProgress {
  std::string name;
  uint64_t bytes = 0;
  uint64_t done = 0;
  PayloadPhase phase = PayloadPhase::Queued;
};
struct PayloadFlashProgress {
  bool available = false;
  unsigned lanes = 1;
  std::vector<PayloadPartitionProgress> partitions;
};
PayloadFlashProgress RecoveryPayloadFlashProgress();
std::vector<PayloadFlashTarget> RecoveryPayloadTargets(const std::vector<std::string> &names);
std::vector<Volume> RecoveryRestoreVolumes(const std::string &folder);
std::string RecoveryStorage();
std::string RecoveryInternalStorage();
std::string RecoveryBackupRoot();
void RecoveryEnsurePcStorage();
int RecoveryRunPcBackup(const JobRequest &request);
bool RecoveryDeleteBackup(const std::string &folder);
bool RecoveryBackupCanUpload(const std::string &folder);
std::string RecoverySlot();
std::string RecoveryBootSlot();
std::string RecoveryVersion();
std::string RecoveryBuildType();
std::string RecoveryBuildStatus();
std::string RecoveryDevice();
std::string RecoveryBuildDate();
std::string RecoveryMaintainer();
bool RecoverySetActiveSlot(const std::string &slot);
struct SnapshotCowStatus {
  bool supported = false;
  bool metadata_readable = false;
  bool safe_to_remove = false;
  uint64_t bytes = 0;
  std::vector<std::string> partitions;
};
SnapshotCowStatus RecoverySnapshotCowStatus();
bool RecoveryDataLocked();
int RecoveryDecrypt(const std::string &credential, int user_id = 0);

// Switch the recovery backend between ordinary AERA and userspace fastboot
// without replacing the UI process or its Qualcomm display context.
bool RecoveryEnterFastbootd();
bool RecoveryLeaveFastbootd(bool initialize_recovery = false);
void RecoveryCompleteColdStartup();
int RecoveryCredentialType();
bool RecoveryUsesFileBasedEncryption();
int RecoveryPatternGridSize();
std::vector<AndroidUser> RecoveryAndroidUsers();
bool RecoverySetStorage(const std::string &path);
int RecoveryRunJob(const JobRequest &request);
struct SideloadStatus {
  bool active = false;
  bool cancel_requested = false;
  uint64_t received_bytes = 0;
  uint64_t total_bytes = 0;
};
SideloadStatus RecoverySideloadStatus();
bool RecoveryCancelSideload();
int RecoveryProgress();
std::string RecoveryOperationDetail();
std::string RecoveryInstallerStatus();
struct InstallerPresentation {
  bool active = false;
  std::string package_name;
  std::string device;
  std::string author;
  std::string stage_title;
  std::string stage_detail;
  bool rebooting = false;
  std::string reboot_target;
  int reboot_seconds = 0;
  int stage = 0;
  int stage_count = 0;
};
struct InstallerPrompt {
  bool active = false;
  std::string id;
  std::string title;
  std::string message;
  std::string accept;
  std::string decline;
};
InstallerPresentation RecoveryInstallerPresentation();
InstallerPrompt RecoveryInstallerPrompt();
bool RecoveryAnswerInstallerPrompt(bool accepted);
int RecoveryBrightness();
void RecoverySetBrightness(int percent);
bool RecoveryScreenTimeoutSupported();
int RecoveryScreenTimeout();
void RecoverySetScreenTimeout(int seconds);
bool RecoveryFlashlightSupported();
bool RecoveryFlashlightEnabled();
bool RecoverySetFlashlight(bool enabled);
bool RecoveryMtpEnabled();
bool RecoverySetMtp(bool enabled);
enum class Preference {
  kClock24,
  kHiddenFiles,
  kCompression,
  kSha256,
  kVerifyZip,
  kPluginAutoUpdate,
  kUpdateNightly,
  kRecents,
  kPreserveRecovery,
  kPreserveAbl,
  kBackupChecksums,
  kVerifyBackupChecksums,
};
bool RecoveryPreference(Preference preference);
bool RecoverySetPreference(Preference preference, bool enabled);
bool RecoveryPreservationSupported();
bool RecoveryAblPreservationSupported();
bool RecoverySha256Available();
int RecoveryUtcOffset();  // Minutes east of UTC.
bool RecoverySetUtcOffset(int minutes);
uint32_t RecoveryAccentColor();
bool RecoverySetAccentColor(uint32_t rgb);
enum class AppearanceMode {
  kGraphite = 0,
  kLight = 1,
  kAmoled = 2,
};
AppearanceMode RecoveryAppearanceMode();
bool RecoverySetAppearanceMode(AppearanceMode mode);
std::string RecoveryWallpaperPath();
bool RecoverySetWallpaperPath(const std::string &path);
bool RecoveryLightMode();
bool RecoverySetLightMode(bool enabled);
enum class SurfaceStyle {
  kSolid = 0,
  kFrosted = 1,
  kOutline = 2,
  kElevated = 3,
};
SurfaceStyle RecoverySurfaceStyle();
bool RecoveryThemedHomeIcons();
bool RecoverySetThemedHomeIcons(bool enabled);
bool RecoverySetSurfaceStyle(SurfaceStyle style);
bool RecoveryTintedIconBackgrounds();
bool RecoverySetTintedIconBackgrounds(bool enabled);
std::string RecoveryUiFont();
bool RecoverySetUiFont(const std::string &plugin_id);
enum class InterfaceSize {
  kSmall = 0,
  kNormal = 1,
  kLarge = 2,
  kSpacious = 3,
};
InterfaceSize RecoveryInterfaceSize();
bool RecoverySetInterfaceSize(InterfaceSize size);
enum class KeyboardLayout { kQwerty = 0, kQwertz = 1 };
KeyboardLayout RecoveryKeyboardLayout();
bool RecoverySetKeyboardLayout(KeyboardLayout layout);
std::vector<HomePluginPreference> RecoveryHomePluginLayout();
bool RecoverySaveHomePluginLayout(const std::vector<HomePluginPreference> &layout);
int RecoveryHomeGridColumns();
bool RecoverySetHomeGridColumns(int columns);
enum class DockLayout {
  kGlass = 0,
  kCompact = 1,
  kMinimal = 2,
  kIcons = 3,
  // Append values: saved preferences and imported themes use these IDs.
  kTextOnly = 4,
};
inline constexpr int kDockLayoutMax = static_cast<int>(DockLayout::kTextOnly);
DockLayout RecoveryDockLayout();
bool RecoverySetDockLayout(DockLayout layout);
int RecoveryDockTransparency();
bool RecoverySetDockTransparency(int percent);
int RecoveryDockBlur();
bool RecoverySetDockBlur(int percent);
bool RecoveryDockHideInApps();
bool RecoverySetDockHideInApps(bool enabled);
std::string RecoveryLanguage();
bool RecoverySetLanguage(const std::string &language);
enum class BrowserCookiePolicy {
  kBlockAll = 0,
  kFirstParty = 1,
  kAllowAll = 2,
};
std::string RecoveryBrowserHomepage();
bool RecoverySetBrowserHomepage(const std::string &homepage);
int RecoveryBrowserZoom();
bool RecoverySetBrowserZoom(int percent);
BrowserCookiePolicy RecoveryBrowserCookiePolicy();
bool RecoverySetBrowserCookiePolicy(BrowserCookiePolicy policy);
bool RecoverySavePreferences();
// Autosave reports failure if either the full or enabled early cache cannot be written.
bool RecoveryAutoSavePreferences();
bool RecoveryPcAutoEnable();
bool RecoverySetPcAutoEnable(bool enabled);
// Reset the in-memory group; the UI then applies it and autosaves once.
bool RecoveryResetThemeSettings();
bool RecoveryResetPreferences();
enum class Haptic { kTouch, kKeyboard, kAction };
bool RecoveryHapticsAvailable();
int RecoveryHapticDuration(Haptic haptic);
int RecoveryDefaultHapticDuration(Haptic haptic);
bool RecoverySetHapticDuration(Haptic haptic, int milliseconds);
void RecoveryVibrate(Haptic haptic);

// AERA-owned PTY terminal service used by the native terminal scene.
enum class TerminalKey { kUp, kDown, kLeft, kRight, kTab, kEscape, kInterrupt };
void RecoveryTerminalStart(int columns, int rows, int pixel_width,
                           int pixel_height);
bool RecoveryTerminalPoll();
int RecoveryTerminalUpdateCounter();
std::vector<std::string> RecoveryTerminalLines(size_t maximum_lines);
void RecoveryTerminalWrite(const std::string &text);
void RecoveryTerminalSendKey(TerminalKey key);
void RecoveryTerminalClear();
bool RecoveryTerminalRunning();

// Native AERA Wi-Fi presentation over the recovery supplicant, DHCP and
// encrypted credential store. UI code never persists plain-text passwords.
struct WifiNetwork {
  std::string ssid;
  std::string security;
  bool saved = false;
  bool connected = false;
};
struct WifiStatus {
  bool supported = false;
  bool enabled = false;
  bool connected = false;
  bool busy = false;
  std::string state;
  std::string ssid;
  std::string ip_address;
  std::string test_result;
  std::vector<WifiNetwork> networks;
};
struct WifiConnection {
  bool connected = false;
  std::string ssid;
  int signal_dbm = -999;
};
struct WifiTraffic {
  bool valid = false;
  std::string interface;
  unsigned interface_index = 0;
  uint64_t received_bytes = 0;
  uint64_t sent_bytes = 0;
};
struct WifiStatusDisplay {
  bool show_speed = true;
  bool show_name = true;
};
struct TemperatureDisplay {
  bool cpu = false;
  bool battery = false;
  bool cpu_load = false;
  bool ram = false;
  bool fahrenheit = false;
  bool decimals = false;
  bool labels = true;
  bool colors = true;
  int cpu_warm = 70, cpu_hot = 90;
  int battery_warm = 40, battery_hot = 48;
  int size = 24;
  int position = 0;
};
constexpr int kTemperatureUnavailable = -1000000;
struct Temperatures {
  int cpu = kTemperatureUnavailable;
  int battery = kTemperatureUnavailable;
};
TemperatureDisplay RecoveryTemperatureDisplay();
bool RecoverySetTemperatureDisplay(const TemperatureDisplay &display, bool save = true);
Temperatures RecoveryTemperatures();
struct SystemMetrics {
  int cpu_percent = -1;
  uint64_t memory_used = 0, memory_total = 0;
};
SystemMetrics RecoverySystemMetrics();
enum class BatteryStyle { kClassic = 0, kCircle = 1 };
constexpr int kBatteryCircleSizeMin = 48;
constexpr int kBatteryCircleSizeMax = 96;
constexpr int kBatteryCircleSizeDefault = 64;
BatteryStyle RecoveryBatteryStyle();
bool RecoverySetBatteryStyle(BatteryStyle style);
int RecoveryBatteryCircleSize();
bool RecoverySetBatteryCircleSize(int size, bool save = true);
bool RecoveryBatteryPercentage();
bool RecoverySetBatteryPercentage(bool show);
enum class WifiOperation {
  kEnable,
  kDisable,
  kDisconnect,
  kScan,
  kConnect,
  kForget,
  kTest,
  kEnableAdb,
  kDisableAdb,
  kEnableFastbootWifi,
  kDisableFastbootWifi,
};
struct WifiRequest {
  WifiOperation operation = WifiOperation::kScan;
  std::string ssid;
  std::string password;
  bool use_saved_credentials = false;
};
void RecoveryWifiInitialize();
void RecoveryWifiInitializeEarly();
WifiConnection RecoveryWifiConnection();
WifiTraffic RecoveryWifiTraffic();
WifiStatusDisplay RecoveryWifiStatusDisplay();
bool RecoverySetWifiStatusDisplay(const WifiStatusDisplay &display, bool save = true);
WifiStatus RecoveryWifiStatus();
int RecoveryRunWifi(const WifiRequest &request);
bool RecoveryWifiAutoEnable();
bool RecoveryWifiAutoConnect();
bool RecoverySetWifiAutoEnable(bool enabled);
bool RecoverySetWifiAutoConnect(bool enabled);
bool RecoveryAdbOverWifi();
bool RecoverySetAdbOverWifi(bool enabled);
struct AdbWifiStatus {
  bool enabled = false;
  bool secure = false;
  bool no_auth = false;
  bool wifi_connected = false;
  bool pairing = false;
  std::string ip_address;
  std::string connect_port;
  std::string connect_command;
  std::string pairing_command;
  std::string pairing_code;
};
struct AdbPairedDevice {
  std::string fingerprint;
  std::string name;
  long long last_seen = 0;
};
AdbWifiStatus RecoveryAdbWifiStatus();
std::vector<AdbPairedDevice> RecoveryAdbPairedDevices();
bool RecoveryForgetAdbDevice(const std::string &fingerprint);
bool RecoveryStartAdbPairing();
bool RecoveryStopAdbPairing();
struct FastbootWifiStatus {
  bool enabled = false;
  bool available = false;
  bool fastboot_mode = false;
  bool active_wifi = false;
  std::string reason;
  std::string connect_command;
  std::string forward_command;
  std::string fastboot_command;
};
FastbootWifiStatus RecoveryFastbootWifiStatus();
bool RecoverySetFastbootOverWifi(bool enabled);
bool RecoverySelectFastbootTransport(bool wireless);

// Native presentation of AERA's SMB/SFTP FUSE network storage manager.
struct NasStatus {
  bool supported = false;
  bool mounted = false;
  bool selected = false;
  bool key_available = false;
  std::string status;
  std::string error;
  NasConfig config;
};
enum class NasOperation { kMountAndUse, kUse, kUnmount };
struct NasRequest { NasOperation operation = NasOperation::kMountAndUse; };
NasStatus RecoveryNasStatus();
NasConfig RecoverySavedNasConfig(const std::string &protocol);
bool RecoverySetNasConfig(const NasConfig &config, std::string *error);
bool RecoveryImportNasKey(const std::string &key, const std::string &name,
                          std::string *error);
int RecoveryRunNas(const NasRequest &request);
}  // namespace aeraui
