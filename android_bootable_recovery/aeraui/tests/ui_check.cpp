#include "scene.hpp"
#include "ui_components.hpp"
#include "pc_transfer_banner.hpp"
#include "hardware_keyboard.hpp"
#include "themed_app_art.hpp"
#include "package_review.hpp"
#include "../core/usb_attachment.hpp"
#include "screenshot_flash.hpp"
#include "update/payload_inspector.hpp"
#include "update/payload_arb.hpp"
#include "update/payload_full_flash.hpp"
#include "picture_viewer.hpp"
#include "browser/runtime.hpp"
#include "browser/protocol.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <png.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace aeraui;
#include "../../aera_remote/pc_connection.hpp"
namespace aera::pc {
static Status pc_status;
bool SetEnabled(bool enabled, int, bool) { pc_status.enabled = enabled; return true; }
Status GetStatus() { return pc_status; }
bool ForgetComputers() { return true; }
BackupCancellation GetBackupCancellation() { return {}; }
static bool pc_finalizing = false;
BackupTransferStatistics GetBackupTransferStatistics() { return {120000000, 87500000, pc_finalizing, pc_finalizing ? 5U : 0U}; }
bool CancelBackup(const std::string &, bool) { return false; }
static bool upload_cancelled = false;
bool CancelUpload(const std::string &) { upload_cancelled = true; return true; }
}
static std::string backup_root;
static std::string file_root;
static bool preferences[12] = {true, false, true, true, false, false, false, true,
                               true, false, true, true};
static int utc_offset = 120;
static uint32_t accent_color = design::kDefaultAccentRgb;
static bool light_mode = false;
static bool save_succeeds = true;
static bool preservation_supported = true;
static bool abl_preservation_supported = true;
static bool haptics_available = true;
static bool early_save_succeeds = true;
static int autosave_count = 0;
static int haptic_durations[] = {40, 40, 160};
static InterfaceSize interface_size = InterfaceSize::kNormal;
static DockLayout dock_layout = DockLayout::kGlass;
static std::string active_slot = "A";
static bool wifi_auto_enable = false;
static bool wifi_auto_connect = false;
static WifiStatusDisplay wifi_display;
static BatteryStyle battery_style = BatteryStyle::kClassic;
static int battery_circle_size = kBatteryCircleSizeDefault;
static TemperatureDisplay temperature_display;
static bool battery_percentage = true;
static bool adb_over_wifi = false;
static bool adb_pairing = false;
static std::vector<plugins::Plugin> home_plugins;
static payload::Info review_info;
namespace aeraui::payload {
Info InspectZip(const std::string &, bool) { return review_info; }
std::string Summary(const Info &) { return "Installer ZIP"; }
DeviceArb ReadDeviceArb() { return {}; }
ArbDecision CompareArb(bool, const Arb &, const DeviceArb &) { return ArbDecision::Unknown; }
bool ArbAllowsInstall(ArbDecision decision, bool acknowledged) {
  return decision != ArbDecision::Downgrade && (decision != ArbDecision::Upgrade || acknowledged);
}
}
static WifiRequest wifi_request;
static SideloadStatus sideload_status;
static int callback_count = 0;
static Action last_action = Action::kNone;
static void RecordAction(Action action, void*) { ++callback_count; last_action = action; }
namespace aeraui::web {
bool RuntimeInstalled() { return true; }
void PrepareRuntime(Preparation &state, const char *, const char *) {
  state.verified = true; state.progress.store(100); state.done.store(true);
}
void RemoveRuntime(const std::string &) {}
std::string LaunchBlockReason() { return "Sandbox unavailable in this test."; }
}
namespace aeraui {
bool RecoveryPreference(Preference p) { return preferences[static_cast<int>(p)]; }
bool RecoverySetPreference(Preference p, bool enabled) { preferences[static_cast<int>(p)] = enabled; return true; }
bool RecoveryPreservationSupported() { return preservation_supported; }
bool RecoveryAblPreservationSupported() { return abl_preservation_supported; }
bool RecoverySha256Available() { return true; }
int RecoveryUtcOffset() { return utc_offset; }
bool RecoverySetUtcOffset(int minutes) { utc_offset = minutes; return true; }
uint32_t RecoveryAccentColor() { return accent_color; }
bool RecoverySetAccentColor(uint32_t rgb) { accent_color = rgb; return true; }
bool RecoveryLightMode() { return light_mode; }
bool RecoverySetLightMode(bool enabled) { light_mode = enabled; return true; }
AppearanceMode RecoveryAppearanceMode() {
  return light_mode ? AppearanceMode::kLight : AppearanceMode::kGraphite;
}
bool RecoverySetAppearanceMode(AppearanceMode mode) {
  if (mode < AppearanceMode::kGraphite || mode > AppearanceMode::kAmoled)
    return false;
  light_mode = mode == AppearanceMode::kLight;
  return true;
}
std::string RecoveryWallpaperPath() { return {}; }
static LockTheme lock_theme;
WifiConnection RecoveryWifiConnection() { return {}; }
LockTheme RecoveryLockTheme() { return lock_theme; }
bool RecoverySetLockTheme(const LockTheme &theme) {
  if (!ValidLockTheme(theme)) return false;
  lock_theme = theme; return true;
}
bool RecoveryFlashlightSupported() { return true; }
bool RecoveryFlashlightEnabled() { return false; }
bool RecoverySetFlashlight(bool) { return true; }
bool RecoverySetWallpaperPath(const std::string &) { return true; }
SurfaceStyle RecoverySurfaceStyle() { return SurfaceStyle::kSolid; }
static bool themed_home_icons = false;
bool RecoveryThemedHomeIcons() { return themed_home_icons; }
bool RecoverySetThemedHomeIcons(bool enabled) { themed_home_icons = enabled; return true; }
bool RecoverySetSurfaceStyle(SurfaceStyle style) {
  return static_cast<int>(style) >= 0 && static_cast<int>(style) <= 3;
}
bool RecoveryTintedIconBackgrounds() { return false; }
bool RecoverySetTintedIconBackgrounds(bool) { return true; }
std::string RecoveryUiFont() { return {}; }
bool RecoverySetUiFont(const std::string &) { return true; }
InterfaceSize RecoveryInterfaceSize() { return interface_size; }
bool RecoverySetInterfaceSize(InterfaceSize size) {
  return static_cast<int>(size) >= 0 && static_cast<int>(size) <= 3;
}
KeyboardLayout RecoveryKeyboardLayout() { return KeyboardLayout::kQwerty; }
bool RecoverySetKeyboardLayout(KeyboardLayout layout) {
  return layout == KeyboardLayout::kQwerty ||
         layout == KeyboardLayout::kQwertz;
}
static int home_grid_columns = 3;
static std::vector<HomePluginPreference> home_layout;
std::vector<HomePluginPreference> RecoveryHomePluginLayout() { return home_layout; }
bool RecoverySaveHomePluginLayout(const std::vector<HomePluginPreference> &layout) {
  if (!save_succeeds) return false;
  home_layout = layout; return true;
}
int RecoveryHomeGridColumns() { return home_grid_columns; }
bool RecoverySetHomeGridColumns(int columns) {
  if (columns < 2 || columns > 5) return false;
  home_grid_columns = columns; return true;
}
DockLayout RecoveryDockLayout() { return dock_layout; }
bool RecoverySetDockLayout(DockLayout layout) { dock_layout = layout; return true; }
int RecoveryDockTransparency() { return 60; }
bool RecoverySetDockTransparency(int) { return true; }
int RecoveryDockBlur() { return 24; }
bool RecoverySetDockBlur(int) { return true; }
bool RecoveryDockHideInApps() { return false; }
bool RecoverySetDockHideInApps(bool) { return true; }
std::string recovery_language = "en";
std::string RecoveryLanguage() { return recovery_language; }
bool RecoverySetLanguage(const std::string &language) {
  recovery_language = language;
  return true;
}
std::string RecoveryBrowserHomepage() { return "aera://start"; }
bool RecoverySetBrowserHomepage(const std::string &) { return true; }
int RecoveryBrowserZoom() { return 100; }
bool RecoverySetBrowserZoom(int) { return true; }
BrowserCookiePolicy RecoveryBrowserCookiePolicy() {
  return BrowserCookiePolicy::kFirstParty;
}
bool RecoverySetBrowserCookiePolicy(BrowserCookiePolicy) { return true; }
bool RecoverySavePreferences() { return save_succeeds; }
bool RecoveryPcAutoEnable() { return false; }
bool RecoverySetPcAutoEnable(bool) { return true; }
bool RecoveryAutoSavePreferences() {
  ++autosave_count;
  return save_succeeds && early_save_succeeds;
}
bool RecoveryResetThemeSettings() {
  themed_home_icons = false;
  accent_color = design::kDefaultAccentRgb;
  light_mode = false;
  interface_size = InterfaceSize::kNormal;
  return true;
}
bool RecoveryResetPreferences() {
  preferences[static_cast<int>(Preference::kClock24)] = false;
  haptic_durations[0] = haptic_durations[1] = 40;
  haptic_durations[2] = 160;
  return true;
}
bool RecoveryHapticsAvailable() { return haptics_available; }
int RecoveryHapticDuration(Haptic haptic) {
  return haptic_durations[static_cast<int>(haptic)];
}
int RecoveryDefaultHapticDuration(Haptic haptic) {
  return haptic == Haptic::kAction ? 160 : 40;
}
bool RecoverySetHapticDuration(Haptic haptic, int duration) {
  haptic_durations[static_cast<int>(haptic)] = duration;
  return true;
}
void RecoveryVibrate(Haptic) {}
void RecoveryTerminalStart(int, int, int, int) {}
bool RecoveryTerminalPoll() { return false; }
int RecoveryTerminalUpdateCounter() { return 0; }
std::vector<std::string> RecoveryTerminalLines(size_t) { return {}; }
void RecoveryTerminalWrite(const std::string &) {}
void RecoveryTerminalSendKey(TerminalKey) {}
void RecoveryTerminalClear() {}
bool RecoveryTerminalRunning() { return true; }
void AttachStatusBar(lv_obj_t *screen, void (*)(Action,void*),void*,StatusBarAction,bool) {
  auto *label=design::Label(screen,"23:30                         100%",&lv_font_montserrat_32,design::kText);
  lv_obj_set_pos(label,54,65);
}
int32_t StatusBarHeight() { return 165; }
bool StatusBarShadeOpen() { return false; }
void RefreshStatusBars() {}
std::vector<Volume> RecoveryVolumes(const std::string &kind) {
  if (kind == "wipe") return {{"FRP","/frp",524288},{"Dalvik / ART cache","DALVIK",0},{"Data","/data",23000000000},
    {"Internal storage","INTERNAL",12000000000},{"Metadata","/metadata",40960000},
    {"USB storage","/usb-otg",32000000000}};
  return {{"Metadata","/metadata",40960000},{"Data","/data",23000000000},{"Vendor Boot","/vendor_boot",100663296},
    {"Boot","/boot",100663296},{"DTBO","/dtbo",25165824},{"EFS","/efs",12648448},{"Init Boot","/init_boot",8388608},{"Modem","/modem",446693376}};
}
std::vector<Volume> RecoveryRestoreVolumes(const std::string &) { return RecoveryVolumes(""); }
static bool usb_connected = true;
UsbFormatTarget RecoveryUsbFormatTarget(const std::string &path) {
  if (path != "/usb-otg" || !usb_connected) return {};
  return {true, "USB storage", path, "/dev/block/sda1", "vfat", "usb:1:32000000000", "",
          32000000000, {"vfat", "exfat", "ext4", "f2fs"}};
}
std::string RecoveryStorage() { return file_root.empty() ? "/tmp" : file_root; }
std::string RecoveryBackupRoot() { return backup_root; }
bool RecoveryDeleteBackup(const std::string &) { return true; }
bool RecoveryBackupCanUpload(const std::string &) { return true; }
SnapshotCowStatus RecoverySnapshotCowStatus() {
  SnapshotCowStatus status;
  status.supported = true;
  status.metadata_readable = true;
  status.safe_to_remove = true;
  status.bytes = 384 * 1024 * 1024ULL;
  status.partitions = {"system_a-cow", "vendor_a-cow"};
  return status;
}
std::string RecoverySlot() { return active_slot; }
std::string RecoveryVersion() { return "R1.0"; }
std::string RecoveryBuildType() { return "Stable"; }
std::string RecoveryBuildStatus() { return "Unofficial"; }
std::string RecoveryDevice() { return "dodge"; }
std::string RecoveryBuildDate() { return "2026-09-13"; }
std::string RecoveryMaintainer() { return "Jonas Salo & Daniel Springer"; }
bool RecoverySetActiveSlot(const std::string &slot) {
  if (slot != "A" && slot != "B") return false;
  active_slot = slot;
  return true;
}
bool RecoveryDataLocked() { return false; }
std::vector<AndroidUser> RecoveryAndroidUsers() {
  return {{0, "Owner", 3, true}, {10, "Work profile", 2, false}};
}
bool RecoverySetStorage(const std::string &) { return true; }
int RecoveryBrightness() { return 50; }
void RecoverySetBrightness(int) {}
static int screen_timeout = 60;
bool RecoveryScreenTimeoutSupported() { return true; }
int RecoveryScreenTimeout() { return screen_timeout; }
void RecoverySetScreenTimeout(int seconds) { screen_timeout = seconds; }
bool RecoveryMtpEnabled() { return false; }
bool RecoverySetMtp(bool) { return true; }
std::vector<Volume> RecoveryImageVolumes() {
  return {{"Recovery", "/recovery", 100663296},
          {"Boot", "/boot", 100663296}};
}
int recovery_progress = 38;
PayloadFlashProgress payload_progress;
PayloadFlashProgress RecoveryPayloadFlashProgress() { return payload_progress; }
std::vector<PayloadFlashTarget> RecoveryPayloadTargets(const std::vector<std::string> &names) {
  std::vector<PayloadFlashTarget> targets;
  for (const auto &name : names) {
    if (name == "boot" || name == "system" || name == "recovery") targets.push_back({name, "/" + name});
    else if (name == "modem") targets.push_back({name, "/dev/block/fake_modem_a", 128ULL << 20, true, 1, {}});
    else if (name == "abl") targets.push_back({name, "/dev/block/fake_abl_a", 8ULL << 20, true, 2, {}});
    else targets.push_back({name, {}, 0, false, 0, "No verified A/B firmware target on this device"});
  }
  return targets;
}
std::string installer_status;
int RecoveryProgress() { return recovery_progress; }
std::string operation_detail = "Backing up / Boot\n38MB of 100MB (38%)";
std::string RecoveryOperationDetail() { return operation_detail; }
std::string RecoveryInstallerStatus() { return installer_status; }
InstallerPresentation RecoveryInstallerPresentation() { return {}; }
InstallerPrompt RecoveryInstallerPrompt() { return {}; }
bool RecoveryAnswerInstallerPrompt(bool) { return true; }
SideloadStatus RecoverySideloadStatus() { return sideload_status; }
bool RecoveryCancelSideload() {
  sideload_status.cancel_requested = true;
  return true;
}
void RecoveryWifiInitialize() {}
WifiStatus RecoveryWifiStatus() {
  WifiStatus status;
  status.supported = true;
  status.enabled = true;
  status.connected = true;
  status.state = "enabled";
  status.ssid = "AERA Lab";
  status.ip_address = "192.168.1.42";
  status.networks = {{"AERA Lab", "WPA2", false, false},
                     {"Saved network", "WPA3", true, false},
                     {"Guest", "OPEN", false, false}};
  return status;
}
int RecoveryRunWifi(const WifiRequest &request) { wifi_request = request; return 0; }
bool RecoveryWifiAutoEnable() { return wifi_auto_enable; }
bool RecoveryWifiAutoConnect() { return wifi_auto_connect; }
bool RecoverySetWifiAutoEnable(bool enabled) { wifi_auto_enable = enabled; return true; }
bool RecoverySetWifiAutoConnect(bool enabled) { wifi_auto_connect = enabled; return true; }
WifiStatusDisplay RecoveryWifiStatusDisplay() { return wifi_display; }
TemperatureDisplay RecoveryTemperatureDisplay() { return temperature_display; }
Temperatures RecoveryTemperatures() { return {42500, 28700}; }
SystemMetrics RecoverySystemMetrics() { return {27, 512000000, 2048000000}; }
bool RecoverySetTemperatureDisplay(const TemperatureDisplay &display, bool save) {
  if (save && !save_succeeds) return false;
  temperature_display = display;
  temperature_display.cpu_hot = std::max(display.cpu_hot, display.cpu_warm + 1);
  temperature_display.battery_hot = std::max(display.battery_hot, display.battery_warm + 1);
  return true;
}
bool RecoverySetWifiStatusDisplay(const WifiStatusDisplay &display, bool save) {
  if (save && !save_succeeds) return false;
  wifi_display = display;
  return true;
}
BatteryStyle RecoveryBatteryStyle() { return battery_style; }
int RecoveryBatteryCircleSize() { return battery_circle_size; }
bool RecoverySetBatteryCircleSize(int size, bool save) {
  if (save && !save_succeeds) return false;
  battery_circle_size = std::clamp(size, kBatteryCircleSizeMin, kBatteryCircleSizeMax);
  return true;
}
bool RecoveryBatteryPercentage() { return battery_percentage; }
bool RecoverySetBatteryPercentage(bool show) {
  if (!save_succeeds) return false;
  battery_percentage = show;
  return true;
}
bool RecoverySetBatteryStyle(BatteryStyle style) {
  if (!save_succeeds) return false;
  battery_style = style;
  return true;
}
bool RecoveryAdbOverWifi() { return adb_over_wifi; }
bool RecoverySetAdbOverWifi(bool enabled) { adb_over_wifi = enabled; return true; }
AdbWifiStatus RecoveryAdbWifiStatus() {
  AdbWifiStatus status;
  status.enabled = adb_over_wifi;
  status.secure = adb_over_wifi;
  status.wifi_connected = true;
  status.pairing = adb_pairing;
  status.ip_address = "192.168.1.42";
  if (adb_over_wifi) {
    status.connect_port = "5555";
    status.connect_command = "adb connect 192.168.1.42:5555";
  }
  if (adb_pairing) {
    status.pairing_command = "adb pair 192.168.1.42:37123";
    status.pairing_code = "482913";
  }
  return status;
}
FastbootWifiStatus RecoveryFastbootWifiStatus() {
  FastbootWifiStatus status;
  status.available = true;
  status.reason = "Ready to enable through paired ADB";
  status.connect_command = "adb connect 192.168.1.2:5555";
  status.forward_command =
      "adb -s 192.168.1.2:5555 forward tcp:5554 tcp:5554";
  status.fastboot_command = "fastboot -s tcp:127.0.0.1:5554 devices";
  return status;
}
bool RecoverySetFastbootOverWifi(bool) { return true; }
bool RecoverySelectFastbootTransport(bool) { return true; }
bool RecoveryStartAdbPairing() {
  adb_over_wifi = true;
  adb_pairing = true;
  return true;
}
bool RecoveryStopAdbPairing() { adb_pairing = false; return true; }
std::vector<AdbPairedDevice> RecoveryAdbPairedDevices() {
  return {{"5d4b9f8a74ce91b2", "koaan@pop-os", 0},
          {"a21c44881e730a56", "Workshop laptop", 0}};
}
bool RecoveryForgetAdbDevice(const std::string &) { return true; }
static plugins::Request selected_plugin_request;
void SetPluginRequest(const plugins::Request &request) { selected_plugin_request = request; }
void SetSelectedPluginId(const std::string &) {}
}
namespace aeraui::plugins {
std::vector<Plugin> Installed() { return home_plugins; }
std::vector<PluginUpdate> AvailableUpdates(const std::vector<Plugin> &) { return {}; }
bool IsGeneric(const Plugin &plugin) { return plugin.entry == "main"; }
bool IsLaunchable(const Plugin &plugin) { return !IsThemeExtension(plugin); }
bool IsThemeExtension(const Plugin &plugin) { return plugin.type == "theme-extension"; }
bool ResolvePayload(const std::string &, Plugin &, std::string &, std::string &) { return false; }
bool IsPackageFile(const std::string &name) {
  return name.size() >= 6 && name.substr(name.size() - 6) == ".aerap";
}
bool InspectLocalPackage(const std::string &, Plugin &, std::string &error) {
  error = "Package inspection is unavailable in this host test.";
  return false;
}
}
static void Tick(int count=40) {
  for(int i=0;i<count;++i) { lv_tick_inc(16); lv_timer_handler(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
static lv_obj_t *Find(lv_obj_t *root,const char *text) {
  if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return lv_obj_get_parent(root);
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) if(auto *found=Find(lv_obj_get_child(root,i),text)) return found;
  return nullptr;
}
static lv_obj_t *FindLabel(lv_obj_t *root,const char *text) {
  if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return root;
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
    if(auto *found=FindLabel(lv_obj_get_child(root,i),text)) return found;
  return nullptr;
}
static lv_obj_t *FindLabelContaining(lv_obj_t *root,const char *text) {
  if (lv_obj_check_type(root, &lv_label_class) &&
      strstr(lv_label_get_text(root), text) != nullptr) return root;
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *found = FindLabelContaining(lv_obj_get_child(root, i), text)) return found;
  return nullptr;
}
static lv_obj_t *FindType(lv_obj_t *root, const lv_obj_class_t *type) {
  if(lv_obj_check_type(root,type)) return root;
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
    if(auto *found=FindType(lv_obj_get_child(root,i),type)) return found;
  return nullptr;
}
static void ConfirmSettingsModal(lv_obj_t *screen) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
    auto *overlay = lv_obj_get_child(screen, i);
    if (lv_obj_get_user_data(overlay) != &widgets::kModalMarker) continue;
    for (uint32_t j = 0; j < lv_obj_get_event_count(overlay); ++j) {
      auto *event = lv_obj_get_event_dsc(overlay, j);
      if (lv_event_dsc_get_user_data(event) == nullptr) continue;
      auto *state = static_cast<widgets::SheetState *>(lv_event_dsc_get_user_data(event));
      assert(state && state->confirm);
      auto confirm = state->confirm;
      confirm();
      assert(widgets::DismissModal(screen));
      return;
    }
  }
  assert(false && "No settings confirmation found");
}
static void PressKeyboardKey(lv_obj_t *keyboard, const char *key) {
  const char *const *map = lv_buttonmatrix_get_map(keyboard);
  uint32_t button = 0;
  for (size_t index = 0; map[index][0] != '\0'; ++index) {
    if (!strcmp(map[index], "\n")) continue;
    if (!strcmp(map[index], key)) {
      lv_buttonmatrix_set_selected_button(keyboard, button);
      lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, nullptr);
      return;
    }
    ++button;
  }
  assert(false);
}
static lv_area_t Bounds(lv_obj_t *object) {
  lv_area_t area{};
  lv_obj_get_coords(object, &area);
  return area;
}
static lv_obj_t *FindPreferenceEntry(lv_obj_t *screen, const char *title) {
  auto *item = FindLabel(screen, title);
  auto *reset = Find(screen, i18n::Translate("Reset preferences"));
  if (!item || !reset) return nullptr;
  auto *list = lv_obj_get_parent(reset);
  while (item && lv_obj_get_parent(item) != list) item = lv_obj_get_parent(item);
  return item;
}
static bool Overlaps(lv_obj_t *left, lv_obj_t *right, int gap = 0) {
  const lv_area_t a = Bounds(left);
  const lv_area_t b = Bounds(right);
  return a.x1 < b.x2 + gap && a.x2 + gap > b.x1 &&
         a.y1 < b.y2 + gap && a.y2 + gap > b.y1;
}
static void AssertInside(lv_obj_t *object, int width, int height) {
  const lv_area_t area = Bounds(object);
  if (area.x1 < 0 || area.y1 < 0 || area.x2 >= width || area.y2 >= height)
    fprintf(stderr, "Object outside viewport: (%d,%d)-(%d,%d), viewport %dx%d\n",
            area.x1, area.y1, area.x2, area.y2, width, height);
  assert(area.x1 >= 0 && area.y1 >= 0);
  assert(area.x2 < width && area.y2 < height);
}
static void AssertContained(lv_obj_t *child, lv_obj_t *parent) {
  const lv_area_t inner = Bounds(child);
  const lv_area_t outer = Bounds(parent);
  if (inner.x1 < outer.x1 || inner.y1 < outer.y1 ||
      inner.x2 > outer.x2 || inner.y2 > outer.y2)
    fprintf(stderr, "Child outside parent in %s, size %d: (%d,%d)-(%d,%d) / (%d,%d)-(%d,%d) %s\n",
            i18n::CurrentLanguage(), static_cast<int>(interface_size),
            inner.x1, inner.y1, inner.x2, inner.y2,
            outer.x1, outer.y1, outer.x2, outer.y2,
            lv_obj_check_type(child, &lv_label_class) ? lv_label_get_text(child) : "control");
  assert(inner.x1 >= outer.x1 && inner.y1 >= outer.y1);
  assert(inner.x2 <= outer.x2 && inner.y2 <= outer.y2);
}
static void AssertCentered(lv_obj_t *child, lv_obj_t *parent) {
  const lv_area_t inner = Bounds(child);
  const lv_area_t outer = Bounds(parent);
  const int dx = (inner.x1 + inner.x2) - (outer.x1 + outer.x2);
  const int dy = (inner.y1 + inner.y2) - (outer.y1 + outer.y2);
  assert(dx >= 34 && dx <= 38);
  assert(dy >= -2 && dy <= 2);
}
static void AssertVisibleCaret(lv_obj_t *input) {
  assert(input && lv_obj_check_type(input, &lv_textarea_class));
  assert(lv_obj_get_style_border_width(input, LV_PART_CURSOR) >= 3);
  assert(lv_obj_get_style_border_opa(input, LV_PART_CURSOR) == LV_OPA_COVER);
  assert(lv_obj_get_style_border_side(input, LV_PART_CURSOR) == LV_BORDER_SIDE_LEFT);
  assert(lv_obj_get_style_anim_duration(input, LV_PART_CURSOR) > 0);
}
int main(int argc,char **argv) {
  assert(argc==2);
  char fixture_dir[]="/tmp/aera-restore-test-XXXXXX"; assert(mkdtemp(fixture_dir));
  backup_root=fixture_dir;
  file_root=backup_root+"/files";
  std::filesystem::create_directory(file_root);
  std::filesystem::create_directory(file_root+"/Folder");
  {
    std::ofstream(file_root+"/alpha.txt") << "alpha\n";
    std::ofstream(file_root+"/long-file-name-for-layout-validation.txt") << "layout\n";
    std::ofstream(file_root+"/package.zip") << "not a real package";
    std::ofstream log(file_root+"/broken.log", std::ios::binary);
    log << "plain\n\033[31mred\033[0m\ninvalid: ";
    const char invalid[] = {static_cast<char>(0xc3), '('};
    log.write(invalid, sizeof(invalid));
    std::ofstream(file_root+"/large.log", std::ios::binary)
        << std::string(160 * 1024, 'x');
  }
  std::filesystem::create_directory(backup_root+"/AERA-test-backup");
  lv_init();
  i18n::Initialize("en");
  const lv_font_t *latin_font = design::UiFont(&lv_font_montserrat_32);
  for (uint32_t codepoint : {0x00c4u, 0x00d6u, 0x00dcu, 0x00dfu,
                             0x00e4u, 0x00f6u, 0x00fcu}) {
    lv_font_glyph_dsc_t glyph{};
    assert(lv_font_get_glyph_dsc(latin_font, &glyph, codepoint, 0));
    assert(glyph.adv_w > 0);
  }
  std::vector<uint8_t> frame(1440*3168*4);
  auto *display=lv_display_create(1440,3168);
  lv_display_set_buffers(display,frame.data(),nullptr,frame.size(),LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display,[](lv_display_t *d,const lv_area_t *,uint8_t *){lv_display_flush_ready(d);});
  auto save=[&](const char *path, int ticks = 40) {
    Tick(ticks); png_image image{}; image.version=PNG_IMAGE_VERSION;
    image.width=lv_display_get_horizontal_resolution(display);
    image.height=lv_display_get_vertical_resolution(display);
    image.format=PNG_FORMAT_BGRA;
    assert(png_image_write_to_file(&image,path,0,frame.data(),0,nullptr));
  };
  auto *screen=lv_screen_active();
  if (!strcmp(argv[1], "--lock-theme")) {
    LockTheme settings; settings.owner="AERA user's phone";settings.blur=45;
    settings.wallpaper="/sdcard/My wallpaper.png";
    assert(EncodeLockTheme(DecodeLockTheme(EncodeLockTheme(settings))) == EncodeLockTheme(settings));
    assert(DecodeLockTheme("broken").clock == 0);
    settings.clock_size=140;
    assert(DecodeLockTheme(EncodeLockTheme(settings)).clock_size==140);
    assert(DecodeLockTheme("1 0 0 1 16777215 25 0 0 1 1 0 0 0 0 0 0 \"\" \"\"").clock_size==100);
    settings.clock_size=181;assert(!ValidLockTheme(settings));settings.clock_size=100;
    settings.date_style=3;settings.date_size=200;settings.aod=true;
    assert(DecodeLockTheme(EncodeLockTheme(settings)).aod);
    assert(DecodeLockTheme(EncodeLockTheme(settings)).date_size==200);
    settings.date_colour=2;settings.date_placement=2;settings.aod_brightness=15;
    assert(DecodeLockTheme(EncodeLockTheme(settings)).date_colour==2);
    assert(DecodeLockTheme(EncodeLockTheme(settings)).date_placement==2);
    assert(DecodeLockTheme(EncodeLockTheme(settings)).aod_brightness==15);
    for(int animation=0;animation<2;++animation) {
      settings.animation=animation;
      assert(DecodeLockTheme(EncodeLockTheme(settings)).animation==animation);
    }
    settings.animation=2;assert(!ValidLockTheme(settings));settings.animation=1;
    settings.dim=101;assert(!ValidLockTheme(settings));
    const std::string lock_wallpaper="/tmp/aera-lock-corners.png";
    png_image lock_image{};lock_image.version=PNG_IMAGE_VERSION;
    lock_image.width=lock_image.height=2;lock_image.format=PNG_FORMAT_RGBA;
    const unsigned char lock_pixels[]={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
    assert(png_image_write_to_file(&lock_image,lock_wallpaper.c_str(),0,lock_pixels,0,nullptr));
    assert(wallpaper::Load(lock_wallpaper));
    auto *page=lv_obj_create(nullptr);
    BuildToolScene(page,Action::kTheme,RecordAction,nullptr);
    lv_screen_load(page);Tick();
    auto *title=FindLabel(page,"Lock screen");assert(title);
    auto *section=lv_obj_get_parent(title);
    auto *preview_image=FindType(section,&lv_image_class);assert(preview_image);
    const auto *rounded=static_cast<const lv_image_dsc_t *>(lv_image_get_src(preview_image));
    assert(rounded->data[3]==0);
    assert(rounded->data[(rounded->header.h/2*rounded->header.w+rounded->header.w/2)*4+3]==255);
    lv_obj_scroll_to_y(lv_obj_get_parent(section),lv_obj_get_y(section),LV_ANIM_OFF);Tick();
    save("/tmp/aera-lock-theme-preview.png");
    auto *analogue=Find(page,"Analogue");assert(analogue);
    lv_obj_send_event(analogue,LV_EVENT_CLICKED,nullptr);
    assert(RecoveryLockTheme().clock==1);
    auto *follow=Find(section,"Follow accent");assert(follow);
    lv_obj_send_event(follow,LV_EVENT_CLICKED,nullptr);
    assert(RecoveryLockTheme().colour==0);
    auto *user_colour=Find(section,"User select");assert(user_colour);
    const auto old_accent=RecoveryAccentColor();
    lv_obj_send_event(user_colour,LV_EVENT_CLICKED,nullptr);Tick();
    assert(FindLabel(page,"Choose clock colour"));
    auto *use_colour=Find(page,"Use colour");assert(use_colour);
    lv_obj_send_event(use_colour,LV_EVENT_CLICKED,nullptr);Tick();
    assert(RecoveryLockTheme().colour==2);
    assert(RecoveryAccentColor()==old_accent);
    assert(!Find(section,"Charging details"));
    assert(!Find(section,"Battery percentage"));
    assert(!Find(section,"Wi-Fi status"));
    lv_screen_load(screen);lv_obj_delete(page);
    wallpaper::Clear();
    for(bool landscape:{false,true}) {
      lv_display_set_resolution(display,landscape?3168:1440,landscape?1440:3168);Tick();
      for(int style=0;style<3;++style) {
        LockTheme theme;theme.clock=style;theme.owner="AERA Recovery";theme.clock_size=180;
        for(int placement=0;placement<3;++placement) {
          theme.placement=placement;
          auto *lock=BuildLockScene(screen,RecordAction,nullptr,true,&theme);Tick();
          assert(lv_obj_get_width(lock)==(landscape?3168:1440));
          assert(FindLabel(lock,"AERA Recovery"));
          if(!landscape && placement==0 && style==0)save("/tmp/aera-lock-screen.png");
          if(!landscape && placement==0)save(("/tmp/aera-clock-"+std::to_string(style)+".png").c_str());
          lv_obj_delete(lock);
        }
      }
    }
    lv_display_set_resolution(display,1440,3168);Tick();
    for(int style=0;style<3;++style) {
      LockTheme theme;theme.clock=style;theme.owner="Private owner text";
      auto *ambient=BuildLockScene(screen,RecordAction,nullptr,false,&theme,true);Tick();
      assert(!lv_obj_has_flag(ambient,LV_OBJ_FLAG_CLICKABLE));
      auto *hint=FindLabel(ambient,"Slide to unlock");assert(hint);
      assert(lv_obj_has_flag(lv_obj_get_parent(hint),LV_OBJ_FLAG_HIDDEN));
      if(style==1) {
        auto *dial=lv_obj_get_parent(FindType(ambient,&lv_line_class));
        assert(lv_obj_get_child_count(dial)==63);
        const auto *points=lv_line_get_points(lv_obj_get_child(dial,62));
        const double centre=lv_obj_get_width(dial)/2;
        const double tick=std::atan2(points[1].y-centre,points[1].x-centre)*30.0/3.14159265358979323846;
        assert(std::abs(tick-std::round(tick))<0.05);
      }
      save(("/tmp/aera-aod-"+std::to_string(style)+".png").c_str());
      lv_obj_delete(ambient);
    }
    struct Pointer {int x=0,y=0;bool down=false;} pointer;
    auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(input,&pointer);
    lv_indev_set_read_cb(input,[](lv_indev_t *i,lv_indev_data_t *data){
      auto *p=static_cast<Pointer *>(lv_indev_get_user_data(i));
      data->point={p->x,p->y};data->state=p->down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    });
    auto point=[&](int x,int y,bool down,int ticks=4){pointer={x,y,down};Tick(ticks);};
    for(int mode=0;mode<3;++mode) {
      LockTheme theme;theme.unlock=mode;theme.animation=0;
      auto *lock=BuildLockScene(screen,RecordAction,nullptr,false,&theme);Tick();
      auto *hint=FindLabel(lock,mode==0?"Slide to unlock":mode==1?"Swipe up to unlock":"Hold to unlock");assert(hint);
      const auto box=Bounds(lv_obj_get_parent(hint));
      const int x=box.x1+40,y=(box.y1+box.y2)/2;
      last_action=Action::kNone;
      if(mode==0) {point(x,y,true);point(x+600,y,true);point(x,y,true);point(x,y,false);}
      if(mode==1) {
        point(720,2000,true);point(720,1500,true);
        assert(lv_obj_get_style_translate_y(lv_obj_get_parent(hint),LV_PART_MAIN)<0);
        point(720,1990,true);point(720,1990,false);
      }
      if(mode==2) {
        point(x,y,true,10);
        auto *progress=lv_obj_get_child(lv_obj_get_parent(hint),1);
        assert(!lv_obj_has_flag(progress,LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_width(progress)>1);
        point(x,y-200,true);point(x,y-200,false);
        assert(lv_obj_has_flag(progress,LV_OBJ_FLAG_HIDDEN));
      }
      assert(last_action!=Action::kUnlock);
      if(mode==0) {point(x,y,true);point(box.x2-10,y,true);point(box.x2-10,y,false);}
      if(mode==1) {point(720,2000,true);point(720,1500,true);point(720,1500,false);}
      if(mode==2) {point(x,y,true,70);point(x,y,false);}
      Tick(30); assert(last_action==Action::kUnlock);
      lv_obj_delete(lock);Tick();
    }
    lv_indev_delete(input);
    puts("Lock theme serialization, live selection, portrait/landscape and cancellable unlock gestures passed");
    return 0;
  }
  if (!strcmp(argv[1], "--screenshot-flash")) {
    for (bool landscape : {false, true}) {
      lv_display_set_resolution(display, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      auto *page = lv_obj_create(nullptr);
      BuildToolScene(page, Action::kWipe, RecordAction, nullptr);
      lv_screen_load(page); Tick();
      const auto count = lv_obj_get_child_count(lv_layer_top());
      auto *flash = ScreenshotFlash(lv_layer_top());
      Tick(7);
      assert(!lv_obj_has_flag(flash, LV_OBJ_FLAG_CLICKABLE));
      assert(lv_obj_get_child_count(flash) == 4);
      assert(lv_obj_get_width(flash) == (landscape ? 3168 : 1440));
      if (!landscape) save("/tmp/aera-screenshot-flash-preview.png", 0);
      Tick();
      assert(lv_obj_get_child_count(lv_layer_top()) == count);
      lv_screen_load(screen); lv_obj_delete(page);
    }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Screenshot flash: portrait/landscape, non-blocking overlay and cleanup passed");
    return 0;
  }
  if (!strcmp(argv[1], "--usb-format")) {
    UsbAttachmentTracker attachments;
    assert(!attachments.Update({"existing"}));
    assert(!attachments.Update({"existing"}));
    assert(attachments.Update({"existing", "new:1"}));
    assert(!attachments.Update({"existing", "new:1"}));
    const auto removed = attachments.Update({"existing"});
    assert(removed.disconnected && !removed.connected);
    assert(attachments.Update({"existing", "new:2"}).connected);
    assert(attachments.Update({}).disconnected);
    assert(attachments.Update({"new:2"}));
    const auto replaced = attachments.Update({"replacement"});
    assert(replaced.connected && replaced.disconnected);
    UsbAttachmentTracker mice, keyboards;
    assert(!mice.Update({}) && !keyboards.Update({}));
    assert(mice.Update({"mouse"}).connected);
    assert(keyboards.Update({"keyboard"}).connected);
    assert(!mice.Update({"mouse"}) && !keyboards.Update({"keyboard"}));
    assert(mice.Update({}).disconnected && keyboards.Update({}).disconnected);
    for (bool landscape : {false, true}) {
      lv_display_set_resolution(display, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      auto *wipe = lv_obj_create(nullptr);
      BuildToolScene(wipe, Action::kWipe, RecordAction, nullptr);
      lv_screen_load(wipe); Tick();
      auto *advanced = Find(wipe, "Advanced"); assert(advanced);
      assert(lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN));
      auto *usb = Find(wipe, "USB storage"); assert(usb);
      auto *data = Find(wipe, "Data"); assert(data);
      lv_obj_send_event(usb, LV_EVENT_CLICKED, nullptr); Tick();
      assert(!lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN));
      auto *review = Find(wipe, "Review wipe"); assert(review);
      assert(lv_obj_get_parent(advanced) == wipe);
      assert(lv_obj_get_width(advanced) == lv_obj_get_width(review));
      assert(lv_obj_get_y(advanced) == lv_obj_get_y(review));
      assert(lv_obj_get_height(advanced) == lv_obj_get_height(review));
      assert(lv_obj_get_x(advanced) + lv_obj_get_width(advanced) < lv_obj_get_x(review));
      assert(lv_color_eq(lv_obj_get_style_bg_color(advanced, LV_PART_MAIN), lv_obj_get_style_bg_color(review, LV_PART_MAIN)));
      assert(lv_obj_get_height(lv_obj_get_parent(usb)) > 200);
      if (!landscape) save("/tmp/aera-usb-advanced-layout.png");
      lv_obj_send_event(data, LV_EVENT_CLICKED, nullptr); Tick();
      assert(lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN));
      assert(lv_obj_get_width(review) == (landscape ? 940 : 1280));
      lv_obj_send_event(data, LV_EVENT_CLICKED, nullptr); Tick();
      assert(!lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN));
      callback_count = 0;
      lv_obj_send_event(advanced, LV_EVENT_CLICKED, nullptr); Tick();
      assert(Find(wipe, "Format USB storage"));
      auto *ext4 = Find(wipe, "ext4 — Linux filesystem"); assert(ext4);
      lv_obj_send_event(ext4, LV_EVENT_CLICKED, nullptr); Tick();
      assert(Find(wipe, "Review USB format"));
      assert(Find(wipe, "Swipe to format USB"));
      assert(callback_count == 0);
      if (!landscape) save("/tmp/aera-usb-format-review.png");
      assert(widgets::DismissModal(wipe)); Tick();
      assert(widgets::DismissModal(wipe)); Tick();
      lv_obj_send_event(advanced, LV_EVENT_CLICKED, nullptr); Tick();
      auto *f2fs = Find(wipe, "F2FS"); assert(f2fs);
      lv_obj_send_event(f2fs, LV_EVENT_CLICKED, nullptr); Tick();
      assert(Find(wipe, "Review USB format"));
      assert(Find(wipe, "Swipe to format USB"));
      assert(callback_count == 0);
      assert(widgets::DismissModal(wipe)); Tick();
      assert(widgets::DismissModal(wipe)); Tick();
      usb_connected = false;
      lv_obj_send_event(advanced, LV_EVENT_CLICKED, nullptr); Tick();
      assert(Find(wipe, "USB format unavailable"));
      assert(callback_count == 0);
      usb_connected = true;
      lv_screen_load(screen); lv_obj_delete(wipe);
    }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("USB format: sole-selection gating, review-only choices, cancellation and disconnect passed");
    return 0;
  }
  if (!strcmp(argv[1], "--wipe-layout")) {
    for (const char *language : {"en", "de_DE", "fr_FR"}) {
      assert(i18n::SetLanguage(language));
      for (bool landscape : {false, true}) {
        lv_display_set_resolution(display, landscape ? 3168 : 1440,
                                  landscape ? 1440 : 3168);
        for (int size = 0; size < 4; ++size) {
          interface_size = static_cast<InterfaceSize>(size);
          design::ApplyInterfaceSize(size);
          auto *wipe = lv_obj_create(nullptr);
          BuildToolScene(wipe, Action::kWipe, RecordAction, nullptr);
          lv_screen_load(wipe); Tick(2);
          auto *frp = FindLabel(wipe, "FRP"); assert(frp);
          auto *list = lv_obj_get_parent(lv_obj_get_parent(frp));
          lv_obj_t *previous = nullptr;
          for (uint32_t index = 0; index < lv_obj_get_child_count(list); ++index) {
            auto *row = lv_obj_get_child(list, index);
            if (previous) assert(Bounds(previous).y2 < Bounds(row).y1);
            lv_obj_t *previous_label = nullptr;
            for (uint32_t child = 0; child < lv_obj_get_child_count(row); ++child) {
              auto *label = lv_obj_get_child(row, child);
              if (!lv_obj_check_type(label, &lv_label_class) || !*lv_label_get_text(label)) continue;
              AssertContained(label, row);
              if (previous_label) assert(!Overlaps(previous_label, label));
              previous_label = label;
            }
            previous = row;
          }
          if (!landscape && !strcmp(language, "en") && size == 2)
            save("/tmp/aera-wipe-wrapped-host.png");
          lv_screen_load(screen); lv_obj_delete(wipe);
        }
      }
    }
    std::filesystem::remove_all(backup_root);
    puts("Wipe rows fit wrapped warnings across languages, interface sizes and orientations.");
    return 0;
  }
  if (!strcmp(argv[1], "--status-bar-settings")) {
    for (const char *language : {"en", "de_DE"}) {
      assert(i18n::SetLanguage(language));
      for (int size = 0; size < 4; ++size) {
        interface_size = static_cast<InterfaceSize>(size);
        design::ApplyInterfaceSize(size);
        wifi_display = {};
        temperature_display = {};
        battery_style = BatteryStyle::kClassic;
        battery_circle_size = kBatteryCircleSizeDefault;
        battery_percentage = true;
        auto *prefs = lv_obj_create(nullptr);
        BuildToolScene(prefs, Action::kPreferences, RecordAction, nullptr);
        lv_screen_load(prefs); Tick();
        auto *entry = FindPreferenceEntry(prefs, i18n::Translate("Status bar")); assert(entry);
        lv_obj_send_event(entry, LV_EVENT_CLICKED, nullptr);
        assert(last_action == Action::kStatusBar);
        lv_screen_load(screen); lv_obj_delete(prefs);
        auto *page = lv_obj_create(nullptr);
        BuildToolScene(page, Action::kStatusBar, RecordAction, nullptr);
        lv_screen_load(page); Tick();
        auto *circle = Find(page, i18n::Translate("Circle")); assert(circle);
        auto *classic = Find(page, i18n::Translate("Classic")); assert(classic);
        lv_obj_send_event(circle, LV_EVENT_CLICKED, nullptr);
        assert(battery_style == BatteryStyle::kCircle);
        auto *size_title = FindLabel(page, i18n::Translate("Circle size")); assert(size_title);
        auto *size_panel = lv_obj_get_parent(lv_obj_get_parent(size_title));
        auto *size_slider = FindType(size_panel, &lv_slider_class); assert(size_slider);
        auto *percentage_text = Find(page, i18n::Translate("Battery percentage")); assert(percentage_text);
        auto *percentage_row = lv_obj_get_parent(percentage_text);
        assert(!lv_obj_has_flag(size_panel, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(percentage_row, LV_OBJ_FLAG_HIDDEN));
        lv_slider_set_value(size_slider, 53, LV_ANIM_OFF);
        lv_obj_send_event(size_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        assert(battery_circle_size == 53);
        lv_obj_send_event(size_slider, LV_EVENT_RELEASED, nullptr);
        save_succeeds = false;
        lv_slider_set_value(size_slider, 70, LV_ANIM_OFF);
        lv_obj_send_event(size_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(size_slider, LV_EVENT_RELEASED, nullptr);
        assert(battery_circle_size == 53 && lv_slider_get_value(size_slider) == 53);
        save_succeeds = true;
        lv_slider_set_value(size_slider, 90, LV_ANIM_OFF);
        lv_obj_send_event(size_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(size_slider, LV_EVENT_PRESS_LOST, nullptr);
        assert(battery_circle_size == 53);
        for (int circle_size : {kBatteryCircleSizeMin, kBatteryCircleSizeMax, kBatteryCircleSizeDefault}) {
          lv_slider_set_value(size_slider, circle_size, LV_ANIM_OFF);
          lv_obj_send_event(size_slider, LV_EVENT_VALUE_CHANGED, nullptr);
          lv_obj_send_event(size_slider, LV_EVENT_RELEASED, nullptr);
          assert(battery_circle_size == circle_size);
        }
        lv_obj_send_event(classic, LV_EVENT_CLICKED, nullptr);
        assert(battery_style == BatteryStyle::kClassic);
        assert(lv_obj_has_flag(size_panel, LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_flag(percentage_row, LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(percentage_row, LV_EVENT_CLICKED, nullptr);
        assert(!battery_percentage);
        lv_obj_send_event(percentage_row, LV_EVENT_CLICKED, nullptr);
        assert(battery_percentage);
        auto *speed_text = Find(page, i18n::Translate("Network speed")); assert(speed_text);
        auto *speed = lv_obj_get_parent(speed_text);
        assert(!FindLabel(page, i18n::Translate("Speed position")));
        assert(!FindLabel(page, i18n::Translate("CPU temperature")));
        auto *temperature_text = Find(page, i18n::Translate("SysInfo"));
        auto *temperature_entry = temperature_text ? lv_obj_get_parent(temperature_text) : nullptr;
        assert(temperature_entry);
        lv_obj_send_event(temperature_entry, LV_EVENT_CLICKED, nullptr);
        assert(last_action == Action::kTemperatures);
        auto *status_page = page;
        page = lv_obj_create(nullptr);
        BuildToolScene(page, Action::kTemperatures, RecordAction, nullptr);
        lv_screen_load(page); Tick();
        auto *cpu_text = Find(page, i18n::Translate("CPU temperature")); assert(cpu_text);
        auto *cpu_row = lv_obj_get_parent(cpu_text);
        auto *scroller = lv_obj_get_parent(cpu_row);
        assert(lv_obj_get_x(scroller) + lv_obj_get_width(scroller) <= lv_obj_get_width(page));
        assert(lv_obj_get_y(scroller) + lv_obj_get_height(scroller) <= lv_obj_get_height(page));
        lv_obj_send_event(cpu_row, LV_EVENT_CLICKED, nullptr);
        assert(temperature_display.cpu);
        for (const char *title : {"CPU usage", "RAM usage"}) {
          auto *label = Find(page, i18n::Translate(title)); assert(label);
          lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
        }
        assert(temperature_display.cpu_load && temperature_display.ram);
        auto *position_title = FindLabel(page, i18n::Translate("Spacing")); assert(position_title);
        auto *position = lv_obj_get_parent(lv_obj_get_parent(position_title));
        auto *slider = FindType(position, &lv_slider_class); assert(slider);
        lv_obj_send_event(speed, LV_EVENT_CLICKED, nullptr);
        assert(!wifi_display.show_speed);
        lv_obj_send_event(speed, LV_EVENT_CLICKED, nullptr);
        assert(wifi_display.show_speed && !lv_obj_has_flag(position, LV_OBJ_FLAG_HIDDEN));
        lv_slider_set_value(slider, 25, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        assert(temperature_display.position == 25);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
        auto *battery_temp_text = Find(page, i18n::Translate("Battery temperature")); assert(battery_temp_text);
        lv_obj_send_event(lv_obj_get_parent(battery_temp_text), LV_EVENT_CLICKED, nullptr);
        assert(temperature_display.battery);
        for (const char *title : {"Use Fahrenheit", "Show decimal places", "Show labels", "Temperature colors"}) {
          auto *label = Find(page, i18n::Translate(title)); assert(label);
          lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
        }
        assert(temperature_display.fahrenheit && temperature_display.decimals && !temperature_display.labels && !temperature_display.colors);
        auto *colors_text = Find(page, i18n::Translate("Temperature colors")); assert(colors_text);
        lv_obj_send_event(lv_obj_get_parent(colors_text), LV_EVENT_CLICKED, nullptr);
        auto *warm_title = FindLabel(page, i18n::Translate("CPU orange from")); assert(warm_title);
        auto *warm_panel = lv_obj_get_parent(lv_obj_get_parent(warm_title));
        auto *warm_slider = FindType(warm_panel, &lv_slider_class); assert(warm_slider);
        lv_slider_set_value(warm_slider, 100, LV_ANIM_OFF);
        lv_obj_send_event(warm_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        assert(temperature_display.cpu_hot == 101);
        lv_obj_send_event(warm_slider, LV_EVENT_PRESS_LOST, nullptr);
        assert(temperature_display.cpu_warm == 70 && temperature_display.cpu_hot == 90);
        auto *name_text = Find(status_page, i18n::Translate("Show network name")); assert(name_text);
        auto *name = lv_obj_get_parent(name_text);
        lv_obj_send_event(name, LV_EVENT_CLICKED, nullptr);
        assert(!wifi_display.show_name);
        lv_obj_send_event(name, LV_EVENT_CLICKED, nullptr);
        assert(wifi_display.show_name);
        save_succeeds = false;
        lv_slider_set_value(slider, 50, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
        assert(temperature_display.position == 25 && lv_slider_get_value(slider) == 25);
        save_succeeds = true;
        lv_slider_set_value(slider, 100, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
        Tick();
        AssertContained(FindLabel(status_page, i18n::Translate("Circle")), circle);
        AssertContained(FindLabel(status_page, i18n::Translate("Classic")), classic);
        AssertContained(position_title, lv_obj_get_parent(position_title));
        assert(!Overlaps(circle, classic));
        lv_obj_send_event(circle, LV_EVENT_CLICKED, nullptr);
        lv_screen_load(status_page);
        Tick();
        if (size == 1 && !strcmp(language, "en")) save("/tmp/aera-status-settings-en.png");
        if (size == 2 && !strcmp(language, "de_DE")) save("/tmp/aera-status-settings-de.png");
        lv_screen_load(page);
        lv_obj_scroll_to_view(cpu_row, LV_ANIM_OFF); Tick();
        for (const char *title : {"CPU temperature", "Battery temperature", "Use Fahrenheit", "Show decimal places", "Show labels", "Temperature colors"}) {
          auto *label = FindLabel(page, i18n::Translate(title)); assert(label);
          AssertContained(label, lv_obj_get_parent(label));
        }
        if (size == 1 && !strcmp(language, "en")) save("/tmp/aera-temperature-settings-en.png");
        if (size == 2 && !strcmp(language, "de_DE")) save("/tmp/aera-temperature-settings-de.png");
        lv_obj_scroll_to_view_recursive(warm_panel, LV_ANIM_OFF); Tick();
        AssertContained(warm_title, lv_obj_get_parent(warm_title));
        if (size == 2 && !strcmp(language, "de_DE")) save("/tmp/aera-temperature-colors-de.png");
        auto *reset_sysinfo = Find(page, i18n::Translate("Reset SysInfo")); assert(reset_sysinfo);
        lv_obj_scroll_to_view_recursive(reset_sysinfo, LV_ANIM_OFF); Tick();
        assert(lv_obj_get_scroll_y(scroller) > 0);
        lv_obj_send_event(reset_sysinfo, LV_EVENT_CLICKED, nullptr);
        ConfirmSettingsModal(page); Tick(2);
        assert(!temperature_display.cpu && !temperature_display.battery &&
               !temperature_display.cpu_load && !temperature_display.ram && temperature_display.position == 0);
        assert(lv_obj_get_scroll_y(scroller) == 0);
        assert(lv_obj_get_y(cpu_row) >= 0);
        lv_screen_load(screen); lv_obj_delete(page);
        lv_obj_delete(status_page);
      }
    }
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Preferences status-bar page, battery previews, switches, positioning and save errors passed");
    return 0;
  }
  if (!strcmp(argv[1], "--pc-upload-progress")) {
    widgets::PcTransferBanner banner;
    aera::pc::UploadProgress progress;
    progress.active = true; progress.id = "fixture";
    progress.name = "SuperHybrid-very-long-package-name-to-check-the-upload-layout.zip";
    progress.bytes = 2000000000; progress.received = 720000000;
    progress.bytes_per_second = 85000000; progress.remaining_seconds = 15;
    for (const char *language : {"en", "de_DE"}) {
      assert(i18n::SetLanguage(language));
      for (int density = 0; density <= 3; ++density) {
        design::ApplyInterfaceSize(density);
        banner.Refresh(progress);
        auto *name = FindLabel(lv_layer_top(), progress.name.c_str());
        assert(name); Tick(); AssertContained(name, lv_obj_get_parent(name));
        auto *compact = lv_obj_get_parent(name);
        assert(lv_obj_get_style_bg_opa(compact, LV_PART_MAIN) == LV_OPA_90);
        assert(lv_obj_get_y(compact) > StatusBarHeight());
        lv_obj_send_event(compact, LV_EVENT_CLICKED, nullptr);
        assert(banner.Expanded());
        auto *cancel = FindLabel(lv_layer_top(), i18n::Translate("Cancel transfer"));
        assert(cancel); Tick(); AssertContained(cancel, lv_obj_get_parent(cancel));
        auto *eta = FindLabel(lv_layer_top(), i18n::Format("About %s remaining", "0:15").c_str());
        assert(eta); AssertContained(eta, lv_obj_get_parent(eta));
        if (density == 2 && !strcmp(language, "en")) save("/tmp/aera-pc-upload-detail.png");
        aera::pc::upload_cancelled = false;
        lv_obj_send_event(lv_obj_get_parent(cancel), LV_EVENT_CLICKED, nullptr);
        assert(aera::pc::upload_cancelled);
        banner.Close(); assert(!banner.Expanded());
        banner.Refresh(progress);
        lv_obj_clean(screen);  // Navigation must not destroy the top-layer banner.
        assert(FindLabel(lv_layer_top(), progress.name.c_str()));
        if (density == 2 && !strcmp(language, "en")) save("/tmp/aera-pc-upload-banner.png");
        progress.finalizing = true; banner.Refresh(progress);
        assert(FindLabelContaining(lv_layer_top(), i18n::Translate("Finishing up")));
        progress.finalizing = false;
        banner.Refresh(progress, false);
        assert(lv_obj_has_flag(compact, LV_OBJ_FLAG_HIDDEN));
        banner.Refresh({}); assert(lv_obj_get_child_count(lv_layer_top()) == 0);
      }
    }
    return 0;
  }
  if (!strcmp(argv[1], "--hardware-keyboard")) {
    hardware_keys::State keys;
    auto key=[&](unsigned code,int value=1) {
      assert(keys.Update(code,value));
      return hardware_keyboard::Handle(code,value,keys);
    };
    auto *input=widgets::TextArea(screen);
    lv_textarea_set_one_line(input,true);
    lv_textarea_set_text(input,"");
    lv_obj_send_event(input,LV_EVENT_PRESSED,nullptr);
    key(KEY_A);key(KEY_A,0);key(KEY_LEFTSHIFT);key(KEY_B);key(KEY_LEFTSHIFT,0);
    key(KEY_1);key(KEY_1,2);key(KEY_1,0);
    assert(std::string(lv_textarea_get_text(input))=="aB11");
    key(KEY_BACKSPACE);key(KEY_HOME);key(KEY_DELETE);
    assert(std::string(lv_textarea_get_text(input))=="B1");
    int control=0;
    text_edit::SetControlHandler(input,[](lv_obj_t *,unsigned k,void *data) {
      *static_cast<int *>(data)=static_cast<int>(k);
    },&control);
    key(KEY_LEFTCTRL);key(KEY_C);key(KEY_C,0);key(KEY_LEFTCTRL,0);
    assert(control==3 && std::string(lv_textarea_get_text(input))=="B1");
    int submitted=0,clicked=0;
    lv_obj_add_event_cb(input,[](lv_event_t *e) {
      ++*static_cast<int *>(lv_event_get_user_data(e));
    },LV_EVENT_READY,&submitted);
    key(KEY_ENTER);key(KEY_ENTER,2);key(KEY_ENTER,0);assert(submitted==1);
    auto *button=widgets::Button(screen,"Test",[&]{++clicked;});
    lv_obj_set_y(button,300);
    key(KEY_TAB);key(KEY_ENTER);key(KEY_ENTER,0);assert(clicked==1);
    key(KEY_Z);assert(std::string(lv_textarea_get_text(input))=="B1");
    key(KEY_LEFTSHIFT);key(KEY_TAB);key(KEY_LEFTSHIFT,0);
    assert(hardware_keyboard::focus==input);
    lv_obj_delete(input);assert(!hardware_keyboard::focus&&!text_edit::FocusedInput());
    auto *modal=lv_obj_create(screen);
    lv_obj_set_user_data(modal,&widgets::kModalMarker);
    auto *inside=widgets::Button(modal,"Inside",[]{});
    key(KEY_TAB);assert(hardware_keyboard::focus==inside);
    key(KEY_TAB);assert(hardware_keyboard::focus==inside);
    lv_obj_delete(modal);assert(!hardware_keyboard::focus);
    assert(!key(KEY_ESC));
    lv_deinit();std::filesystem::remove_all(backup_root);
    puts("Physical keyboard UI: typing, modifiers, repeat, editing, submit, focus, deletion and modal isolation passed.");
    return 0;
  }
  if (!strcmp(argv[1], "--pc-backup-progress")) {
    for (const char *language : {"en", "de_DE"}) {
      assert(i18n::SetLanguage(language));
      for (const bool restore : {false, true}) {
        lv_obj_clean(screen);
        JobRequest request;
        request.job = Job::kPcBackupOperation;
        request.title = "App Backup Vault";
        request.backup_kind = "apps";
        request.backup_action = restore ? "restore" : "backup";
        request.backup_source = "computer";
        auto operation = BuildJobScene(screen, request, RecordAction, nullptr);
        assert(std::string(lv_label_get_text(operation.status)) != i18n::Translate("Preparing installation"));
        operation_detail = "Creating app backup repository";
        RefreshOperationScene(operation);
        assert(operation.transfer_speed && !strcmp(lv_label_get_text(operation.transfer_speed), "87.5 MB/s"));
        Tick();AssertContained(operation.transfer_speed, lv_obj_get_parent(operation.transfer_speed));
        assert(!strcmp(lv_label_get_text(operation.status), i18n::Translate("Creating app backup repository")));
        operation_detail = "Restoring apps\nWorking...\n2 / 4";
        RefreshOperationScene(operation);
        assert(!strcmp(lv_label_get_text(operation.metrics), i18n::Translate("Working...")));
        assert(std::string(lv_label_get_text(operation.files)).find("2 / 4") != std::string::npos);
        operation_detail = std::string(restore ? "Restoring apps" : "Backing up selected apps\u2026") +
            "\n12 MB / 24 MB\n47 / 123\n/data/user/0/com.spotify.music/files/test.db";
        RefreshOperationScene(operation);
        assert(std::string(lv_label_get_text(operation.status)) == (restore
            ? i18n::Format("Restoring %s", i18n::Translate("Apps")) : i18n::Translate("Backing up selected apps\u2026")));
        if (!strcmp(language, "de_DE") && !restore)
          assert(strcmp(lv_label_get_text(operation.status), "Backing up selected apps\u2026"));
        assert(!strcmp(lv_label_get_text(operation.metrics), "12 MB / 24 MB"));
        assert(std::string(lv_label_get_text(operation.files)).find("47 / 123") != std::string::npos);
        assert(std::string(lv_label_get_text(operation.activity_summary)).find("com.spotify.music") != std::string::npos);
        Tick();AssertContained(operation.activity_summary, lv_obj_get_parent(operation.activity_summary));
        if (!restore && !strcmp(language, "de_DE")) save("/tmp/aera-pc-app-backup-progress.png");
        aera::pc::pc_finalizing = true;
        RefreshOperationScene(operation);Tick();
        assert(!strcmp(lv_label_get_text(operation.status), i18n::Translate("Saving file on computer")));
        assert(!strcmp(lv_label_get_text(operation.detail), i18n::Translate("Keep the browser open while the computer finishes saving.")));
        assert(!strcmp(lv_label_get_text(operation.percent), i18n::Translate("Working...")));
        assert(operation.progress_pulse && !lv_obj_has_flag(operation.progress_pulse, LV_OBJ_FLAG_HIDDEN));
        aera::pc::pc_finalizing = false;
        RefreshOperationScene(operation);
        assert(lv_obj_has_flag(operation.progress_pulse, LV_OBJ_FLAG_HIDDEN));
        aera::pc::pc_finalizing = true;
        RefreshOperationScene(operation);
        assert(!lv_obj_has_flag(operation.progress_pulse, LV_OBJ_FLAG_HIDDEN));
        if (!restore && !strcmp(language, "de_DE")) save("/tmp/aera-pc-finalizing.png");
        aera::pc::pc_finalizing = false;
        CompleteOperationScene(operation, true, restore ? "App restore completed successfully." : "App backup completed successfully.");
      }
    }
    for (const char *language : {"en", "de_DE", "zh_CN"}) {
      assert(i18n::SetLanguage(language));
      for (auto size : {InterfaceSize::kSmall, InterfaceSize::kNormal, InterfaceSize::kLarge, InterfaceSize::kSpacious}) {
        interface_size=size;design::ApplyInterfaceSize(static_cast<int>(size));
        for (bool landscape : {false,true}) {
          lv_display_set_resolution(display, landscape?3168:1440, landscape?1440:3168);
          for (bool restore : {false,true}) {
            lv_obj_clean(screen);
            JobRequest request;request.job=Job::kPcBackupOperation;request.title="Recovery backup";
            request.backup_kind="recovery";request.backup_source="computer";request.backup_action=restore?"restore":"backup";
            auto operation=BuildJobScene(screen,request,RecordAction,nullptr);
            const char *partition="Super (system system_ext product vendor)";
            operation_detail=std::string("backing / ")+partition+"\n4584MB of 18032MB (25%)";
            RefreshOperationScene(operation);Tick(2);
            assert(std::string(lv_label_get_text(operation.status))==i18n::Format(restore?"Restoring %s":"Backing up %s",partition));
            lv_area_t title,detail,percent;
            lv_obj_get_coords(operation.status,&title);lv_obj_get_coords(operation.detail,&detail);lv_obj_get_coords(operation.percent,&percent);
            assert(title.y2<detail.y1 && detail.y2<percent.y1);
            AssertContained(operation.status,lv_obj_get_parent(operation.status));
            AssertContained(operation.detail,lv_obj_get_parent(operation.detail));
            AssertContained(operation.destination,lv_obj_get_parent(operation.destination));
            if(!landscape&&restore&&size==InterfaceSize::kSpacious&&!strcmp(language,"en"))save("/tmp/aera-pc-long-restore.png");
            CompleteOperationScene(operation,true,"Everything finished successfully.");
          }
        }
      }
    }
    std::filesystem::remove_all(backup_root);return 0;
  }
  if (!strcmp(argv[1], "--payload-parallel") || !strcmp(argv[1], "--payload-parallel-landscape")) {
    const bool landscape = !strcmp(argv[1], "--payload-parallel-landscape");
    if (landscape) lv_display_set_resolution(display, 3168, 1440);
    JobRequest request;
    request.job = Job::kFlashPayload;
    request.payload_direct = true;
    request.title = "Flash payload images";
    request.path = "/sdcard/update.zip";
    payload_progress = {true, 8, {{"system", 1000, 500, PayloadPhase::Writing},
        {"vendor", 1000, 250, PayloadPhase::Verifying},
        {"product", 1000, 700, PayloadPhase::Writing},
        {"system_ext", 1000, 400, PayloadPhase::Writing},
        {"my_product", 1000, 300, PayloadPhase::Writing},
        {"my_stock", 1000, 200, PayloadPhase::Writing},
        {"vendor_dlkm", 1000, 600, PayloadPhase::Verifying},
        {"odm", 1000, 900, PayloadPhase::Writing},
        {"boot", 100, 0, PayloadPhase::Queued}}};
    for (int i = 9; i < 32; ++i)
      payload_progress.partitions.push_back({"test_" + std::to_string(i), 100, 0, PayloadPhase::Queued});
    for (const auto& p : payload_progress.partitions) request.partitions.push_back(p.name);
    auto operation = BuildJobScene(screen, request, RecordAction, nullptr);
    assert(operation.payload_cards.size() == 32);
    RefreshOperationScene(operation);
    save(landscape ? "/tmp/aera-parallel-landscape.png" : "/tmp/aera-parallel-progress.png");
    assert(!lv_obj_has_flag(operation.payload_cards[0], LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(operation.payload_cards[1], LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(operation.payload_names[0]), "system"));
    assert(!strcmp(lv_label_get_text(operation.payload_names[1]), "vendor"));
    assert(lv_bar_get_value(operation.payload_bars[0]) == 50);
    assert(lv_bar_get_value(operation.payload_bars[1]) == 25);
    assert(lv_obj_get_style_bg_opa(operation.payload_bars[0], LV_PART_INDICATOR) == LV_OPA_COVER);
    assert(!lv_obj_has_flag(operation.details, LV_OBJ_FLAG_HIDDEN));
    lv_area_t row, details;
    for (auto* card : operation.payload_cards) assert(!lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
    lv_obj_get_coords(operation.payload_list, &row);
    lv_obj_get_coords(operation.details, &details);
    assert(row.y2 < details.y1);
    assert(details.y2 < lv_display_get_vertical_resolution(display));
    lv_area_t panel;
    lv_obj_get_coords(operation.activity_panel, &panel);
    const int running_height = lv_obj_get_height(operation.activity_panel);
    assert(panel.y2 == lv_display_get_vertical_resolution(display) -
        (landscape ? 70 : 130) - 1);
    lv_obj_scroll_to_view(operation.payload_cards.back(), LV_ANIM_OFF);
    Tick();
    lv_area_t last;
    lv_obj_get_coords(operation.payload_cards.back(), &last);
    assert(last.y2 <= row.y2 && last.y1 >= row.y1);
    payload_progress.partitions[0].phase = PayloadPhase::Failed;
    RefreshOperationScene(operation);
    assert(!strcmp(lv_label_get_text(operation.payload_names[0]), "system"));
    assert(!strcmp(lv_label_get_text(operation.payload_states[0]), "Failed"));
    payload_progress.partitions[1].phase = PayloadPhase::Verified;
    RefreshOperationScene(operation);
    assert(!strcmp(lv_label_get_text(operation.payload_states[1]), "Verified"));
    CompleteOperationScene(operation, true, "Flash verified successfully.");
    Tick();
    lv_area_t done;
    lv_obj_get_coords(operation.activity_panel, &panel);
    lv_obj_get_coords(operation.done, &done);
    lv_obj_get_coords(operation.payload_list, &row);
    lv_obj_get_coords(operation.details, &details);
    assert(!lv_obj_has_flag(operation.done, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_height(operation.activity_panel) < running_height);
    assert(panel.y2 < done.y1 && row.y2 < details.y1);
    assert(details.y2 <= panel.y2);
    std::filesystem::remove_all(backup_root);
    return 0;
  }
  if (!strcmp(argv[1], "--payload-review")) {
    preferences[static_cast<int>(Preference::kPreserveAbl)] = true;
    preferences[static_cast<int>(Preference::kPreserveRecovery)] = true;
    const std::string wallpaper_path = backup_root + "/wallpaper.png";
    png_image wallpaper_image{};
    wallpaper_image.version = PNG_IMAGE_VERSION;
    wallpaper_image.width = wallpaper_image.height = 2;
    wallpaper_image.format = PNG_FORMAT_RGBA;
    const unsigned char pixels[] = {20,30,40,255, 25,35,45,255,
                                   30,40,50,255, 35,45,55,255};
    assert(png_image_write_to_file(&wallpaper_image, wallpaper_path.c_str(), 0, pixels, 0, nullptr));
    for (bool custom_wallpaper : {false, true}) {
    if (custom_wallpaper) assert(wallpaper::Load(wallpaper_path));
    else wallpaper::Clear();
    review_info.is_payload = review_info.valid = true;
    review_info.partial = true; // OEM full-image OTAs may cover only some partitions.
    review_info.target_build = "Test full OTA";
    review_info.manifest_hash.assign(32, 'a');
    review_info.partitions = {{"boot", 64ULL << 20, 2, true, ""},
                             {"system", 4ULL << 30, 42, true, ""},
                             {"modem", 64ULL << 20, 8, true, ""},
                             {"unsupported", 1ULL << 20, 1, false, "Requires source image"},
                             {"abl", 1ULL << 20, 1, true, ""},
                             {"recovery", 64ULL << 20, 1, true, ""}};
    for (bool incremental : {false, true}) {
      review_info.incremental = incremental;
      ReviewPackage(screen, "/test.zip", RecordAction, nullptr);
      for (int i = 0; i < 50 && !Find(screen, "Advanced  " LV_SYMBOL_RIGHT); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); Tick();
      }
      auto *advanced = Find(screen, "Advanced  " LV_SYMBOL_RIGHT); assert(advanced);
      assert(bool(Find(screen, "Fast flash (experimental)")) == !incremental);
      lv_obj_send_event(advanced, LV_EVENT_CLICKED, nullptr); Tick();
      assert(Find(screen, "Advanced ZIP options"));
      AssertInside(Find(screen, "Advanced ZIP options"), 1440, 3168);
      if (incremental) {
        assert(!Find(screen, "Extract selected images"));
      } else {
        auto *flash = Find(screen, "Flash selected partitions"); assert(flash);
        auto *package = Find(screen, "SELECTED PACKAGE"); assert(package);
        for (int surface = 0; surface < 4; ++surface) {
          design::ApplySurfaceStyle(surface);
          // Reopen the page so every control is created using this surface.
          lv_obj_send_event(Find(screen, LV_SYMBOL_LEFT "  Back"), LV_EVENT_CLICKED, nullptr); Tick();
          lv_obj_send_event(advanced, LV_EVENT_CLICKED, nullptr); Tick();
          package = Find(screen, "SELECTED PACKAGE"); assert(package);
          for (const char *text : {LV_SYMBOL_LEFT "  Back", "Flash selected partitions",
                                  "Direct flash selected partitions", "Extract selected images"}) {
            auto *control = Find(screen, text); assert(control);
            assert(lv_color_eq(lv_obj_get_style_bg_color(control, LV_PART_MAIN), lv_obj_get_style_bg_color(package, LV_PART_MAIN)));
            assert(lv_obj_get_style_bg_opa(control, LV_PART_MAIN) == lv_obj_get_style_bg_opa(package, LV_PART_MAIN));
            assert(lv_obj_get_style_border_width(control, LV_PART_MAIN) == lv_obj_get_style_border_width(package, LV_PART_MAIN));
            assert(lv_obj_get_style_blur_backdrop(control, LV_PART_MAIN) == lv_obj_get_style_blur_backdrop(package, LV_PART_MAIN));
          }
        }
        design::ApplySurfaceStyle(0);
        flash = Find(screen, "Flash selected partitions");
        lv_obj_send_event(flash, LV_EVENT_CLICKED, nullptr); Tick();
        auto *flash_review = Find(screen, "Review selected flash"); assert(flash_review);
        assert(lv_obj_has_state(flash_review, LV_STATE_DISABLED));
        auto *all = Find(screen, "Select all"); assert(all);
        lv_obj_send_event(all, LV_EVENT_CLICKED, nullptr); Tick();
        assert(Find(screen, "Deselect all"));
        assert(FindLabelContaining(screen, "3 selected /"));
        auto *abl = Find(screen, "abl"); assert(abl);
        assert(!lv_obj_has_state(abl, LV_STATE_DISABLED));
        lv_obj_send_event(abl, LV_EVENT_CLICKED, nullptr); Tick();
        assert(FindLabelContaining(screen, "4 selected /"));
        lv_obj_send_event(flash_review, LV_EVENT_CLICKED, nullptr); Tick();
        assert(FindLabelContaining(screen, "Manually selected protected partitions will be overwritten."));
        auto *override_slider = Find(screen, "Swipe to flash selected"); assert(override_slider);
        lv_obj_send_event(Find(lv_obj_get_parent(override_slider), "Cancel"), LV_EVENT_CLICKED, nullptr); Tick();
        lv_obj_send_event(abl, LV_EVENT_CLICKED, nullptr); Tick();
        assert(FindLabelContaining(screen, "3 selected /"));
        assert(!lv_obj_has_state(flash_review, LV_STATE_DISABLED));
        lv_obj_send_event(all, LV_EVENT_CLICKED, nullptr); Tick();
        assert(Find(screen, "Select all"));
        assert(lv_obj_has_state(flash_review, LV_STATE_DISABLED));
        auto *modem = Find(screen, "modem"); assert(modem);
        assert(Find(screen, "Firmware"));
        lv_obj_send_event(modem, LV_EVENT_CLICKED, nullptr); Tick();
        assert(!lv_obj_has_state(flash_review, LV_STATE_DISABLED));
        assert(callback_count == 0);
        lv_obj_send_event(flash_review, LV_EVENT_CLICKED, nullptr); Tick();
        auto *flash_slider = Find(screen, "Swipe to flash selected"); assert(flash_slider);
        AssertInside(flash_slider, 1440, 3168);
        auto *flash_cancel = Find(lv_obj_get_parent(flash_slider), "Cancel"); assert(flash_cancel);
        AssertInside(flash_cancel, 1440, 3168);
        lv_obj_send_event(flash_cancel, LV_EVENT_CLICKED, nullptr); Tick();
        assert(callback_count == 0);
        auto *back = Find(lv_obj_get_parent(flash_review), LV_SYMBOL_LEFT "  Back"); assert(back);
        lv_obj_send_event(back, LV_EVENT_CLICKED, nullptr); Tick();
        auto *extract = Find(screen, "Extract selected images"); assert(extract);
        auto *direct = Find(screen, "Direct flash selected partitions"); assert(direct);
        AssertInside(direct, 1440, 3168);
        lv_obj_send_event(direct, LV_EVENT_CLICKED, nullptr); Tick();
        auto *direct_all = Find(screen, "Select all"); assert(direct_all);
        lv_obj_send_event(direct_all, LV_EVENT_CLICKED, nullptr); Tick();
        auto *direct_review = Find(screen, "Review selected flash"); assert(direct_review);
        lv_obj_send_event(direct_review, LV_EVENT_CLICKED, nullptr); Tick();
        assert(FindLabelContaining(screen, "No temporary images are saved"));
        auto *direct_slider = Find(screen, "Swipe to flash selected"); assert(direct_slider);
        AssertInside(direct_slider, 1440, 3168);
        save("/tmp/aera-direct-flash-confirmation.png");
        auto *direct_cancel = Find(lv_obj_get_parent(direct_slider), "Cancel"); assert(direct_cancel);
        lv_obj_send_event(direct_cancel, LV_EVENT_CLICKED, nullptr); Tick();
        auto *direct_back = Find(lv_obj_get_parent(direct_review), LV_SYMBOL_LEFT "  Back"); assert(direct_back);
        lv_obj_send_event(direct_back, LV_EVENT_CLICKED, nullptr); Tick();
        assert(callback_count == 0);
        AssertInside(extract, 1440, 3168);
        lv_obj_send_event(extract, LV_EVENT_CLICKED, nullptr); Tick();
        auto *review = Find(screen, "Review extraction"); assert(review);
        assert(lv_obj_has_state(review, LV_STATE_DISABLED));
        auto *boot = Find(screen, "boot"); assert(boot);
        auto *extract_all = Find(screen, "Select all"); assert(extract_all);
        AssertInside(extract_all, 1440, 3168);
        lv_obj_send_event(extract_all, LV_EVENT_CLICKED, nullptr); Tick();
        assert(Find(screen, "Deselect all"));
        assert(FindLabelContaining(screen, "5 selected /"));
        // A manual deselection updates the bulk button and the total.
        lv_obj_send_event(boot, LV_EVENT_CLICKED, nullptr); Tick();
        assert(Find(screen, "Select all"));
        assert(FindLabelContaining(screen, "4 selected /"));
        lv_obj_send_event(extract_all, LV_EVENT_CLICKED, nullptr); Tick();
        assert(Find(screen, "Deselect all"));
        lv_obj_send_event(extract_all, LV_EVENT_CLICKED, nullptr); Tick();
        assert(lv_obj_has_state(review, LV_STATE_DISABLED));
        lv_obj_send_event(boot, LV_EVENT_CLICKED, nullptr); Tick();
        assert(!lv_obj_has_state(review, LV_STATE_DISABLED));
        AssertInside(review, 1440, 3168);
        assert(Find(screen, "Requires source image"));
        auto *unavailable = Find(screen, "Unavailable images (1)  " LV_SYMBOL_DOWN);
        assert(unavailable);
        lv_obj_send_event(unavailable, LV_EVENT_CLICKED, nullptr); Tick();
        save("/tmp/aera-advanced-partitions.png");
        const auto review_bounds = Bounds(review);
        lv_obj_send_event(review, LV_EVENT_CLICKED, nullptr); Tick();
        auto *extract_slider = Find(screen, "Swipe to extract"); assert(extract_slider);
        AssertInside(extract_slider, 1440, 3168);
        auto *extract_sheet = lv_obj_get_parent(extract_slider);
        auto *cancel = Find(extract_sheet, "Cancel"); assert(cancel);
        AssertInside(cancel, 1440, 3168);
        const auto after_review_bounds = Bounds(review);
        assert(review_bounds.y1 == after_review_bounds.y1);
        assert(review_bounds.y2 == after_review_bounds.y2);
        save("/tmp/aera-extraction-confirmation.png");
        assert(callback_count == 0); // Reviewing never starts a write.
      }
      lv_obj_clean(screen); Tick();
    }
    }
    wallpaper::Clear();
    review_info = {};
    ReviewPackage(screen, "/installer.zip", RecordAction, nullptr);
    for (int i = 0; i < 50 && !Find(screen, "Advanced  " LV_SYMBOL_RIGHT); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10)); Tick();
    }
    assert(Find(screen, "Install this package?"));
    assert(Find(screen, "Advanced  " LV_SYMBOL_RIGHT));
    lv_obj_clean(screen); Tick();
    lv_deinit(); std::filesystem::remove_all(backup_root);
    puts("Advanced payload review: selection, incremental blocking, normal ZIP fallback and no implicit flash passed.");
    return 0;
  }
  if (!strcmp(argv[1], "--home-edit") || !strcmp(argv[1], "--home-drag-stability") ||
      !strcmp(argv[1], "--home-gap-packing")) {
    using namespace widgets;
    for (int i = 0; i < 30; ++i) {
      plugins::Plugin plugin;
      plugin.id = "plugin-" + std::to_string(i); plugin.entry = "main";
      plugin.name = "Plugin " + std::to_string(i);
      plugin.description = "Test app description";
      home_plugins.push_back(plugin);
    }
    struct Pointer { int x = 0, y = 0; bool down = false; } pointer;
    auto *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(input, &pointer);
    lv_indev_set_read_cb(input, [](lv_indev_t *i, lv_indev_data_t *data) {
      auto *p = static_cast<Pointer *>(lv_indev_get_user_data(i));
      data->point = {p->x, p->y};
      data->state = p->down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    auto point = [&](int x, int y, bool down, int ticks = 5) { pointer = {x, y, down}; Tick(ticks); };
    auto centre = [](lv_obj_t *object) {
      auto b = Bounds(object); return lv_point_t{(b.x1 + b.x2) / 2, (b.y1 + b.y2) / 2};
    };
    auto hold = [&](lv_obj_t *tile) {
      auto p = centre(tile); point(p.x, p.y, true, 45); assert(PointerDragActive());
    };
    if (!strcmp(argv[1], "--home-gap-packing")) {
      for (int columns : {3, 5}) {
        RecoverySetHomeGridColumns(columns); home_layout.clear(); callback_count = 0;
        auto *home = lv_obj_create(nullptr); BuildHomeScene(home, RecordAction, nullptr);
        lv_screen_load(home); Tick();
        const std::string wide_name = "Plugin " + std::to_string(columns - 1);
        const std::string filler_name = "Plugin " + std::to_string(columns);
        const std::string moved_name = "Plugin " + std::to_string(columns + 1);
        auto *wide = Find(home, wide_name.c_str()); auto p = centre(wide);
        hold(wide); point(p.x, p.y, false);
        lv_obj_send_event(Find(home, "2 x 1"), LV_EVENT_CLICKED, nullptr); Tick();
        const auto first = Bounds(Find(home, "Plugin 0"));
        const auto previous = Bounds(Find(home, ("Plugin " + std::to_string(columns - 2)).c_str()));
        const auto filler = Bounds(Find(home, filler_name.c_str()));
        assert(first.y1 == previous.y1 && first.y1 == filler.y1 && filler.x1 > previous.x1);
        assert(Bounds(Find(home, wide_name.c_str())).y1 > first.y1);
        auto target = centre(Find(home, filler_name.c_str()));
        hold(Find(home, moved_name.c_str()));
        point(target.x, target.y, true); point(target.x, target.y, false); Tick();
        assert(Bounds(Find(home, moved_name.c_str())).y1 == first.y1);
        assert(Bounds(Find(home, filler_name.c_str())).y1 > first.y1);
        assert(callback_count == 0);
        save(("/tmp/aera-home-gap-fixed-" + std::to_string(columns) + ".png").c_str());
        lv_screen_load(screen); lv_obj_delete(home); Tick();
        home = lv_obj_create(nullptr); BuildHomeScene(home, RecordAction, nullptr);
        lv_screen_load(home); Tick();
        assert(Bounds(Find(home, moved_name.c_str())).y1 == Bounds(Find(home, "Plugin 0")).y1);
        lv_screen_load(screen); lv_obj_delete(home); Tick();
      }
      lv_indev_delete(input); lv_deinit(); std::filesystem::remove_all(backup_root);
      puts("Home: 3x3 and 5x5 top-right resize, gap fill, replacement drag and saved layout passed.");
      return 0;
    }
    if (!strcmp(argv[1], "--home-drag-stability")) {
      for (int columns : {5, 3}) {
        home_layout.clear(); RecoverySetHomeGridColumns(columns);
        auto *home = lv_obj_create(nullptr); BuildHomeScene(home, RecordAction, nullptr);
        lv_screen_load(home); Tick();
        auto *tile = Find(home, "Plugin 0"); auto p = centre(tile);
        hold(tile);
        auto *page = lv_obj_get_parent(tile);
        const int original_scroll = lv_obj_get_scroll_y(page);
        for (int dy : {36, 72, 108, 180, 240, 300, 348, 300, 240, 180, 108, 72, 36, 0}) {
          point(p.x, p.y + dy, true);
          if (lv_obj_get_scroll_y(page) != original_scroll)
            fprintf(stderr, "Native page drift in %dx%d: %d -> %d\n", columns, columns,
                original_scroll, lv_obj_get_scroll_y(page));
          assert(lv_obj_get_scroll_y(page) == original_scroll);
        }
        auto *ghost = lv_obj_get_child(home, -1);
        assert(lv_obj_get_user_data(ghost) != &kPersistentModalMarker);
        assert(!lv_obj_has_flag(ghost, LV_OBJ_FLAG_CLICKABLE));
        assert(Find(ghost, "Plugin 0"));
        auto *remove = Find(home, "Remove"); assert(remove);
        auto trash = centre(remove);
        point(trash.x, trash.y, true);
        assert(lv_obj_get_child(home, -1) == ghost);
        point(trash.x, trash.y, false);
        assert(Find(home, "Remove plugin?"));
        assert(DismissModal(home)); Tick();
        lv_screen_load(screen); lv_obj_delete(home); Tick();
      }
      lv_indev_delete(input); lv_deinit(); std::filesystem::remove_all(backup_root);
      puts("Home drag: no vertical page drift, ghost above toolbar and trash confirmation passed.");
      return 0;
    }
    for (int columns = 2; columns <= 5; ++columns) {
      home_layout.clear(); RecoverySetHomeGridColumns(columns);
      interface_size = columns % 2 == 0 ? InterfaceSize::kLarge : InterfaceSize::kNormal;
      themed_home_icons = columns % 2 == 0;
      callback_count = 0; selected_plugin_request = {};
      auto *home = lv_obj_create(nullptr);
      BuildHomeScene(home, RecordAction, nullptr);
      lv_screen_load(home); Tick();
      auto *first = Find(home, "Plugin 0"); assert(first);
      auto *second = Find(home, "Plugin 1"); assert(second);
      auto p = centre(first), target = centre(second);
      // A short tap launches exactly once. Long presses never launch.
      point(p.x, p.y, true); point(p.x, p.y, false);
      assert(callback_count == 1); callback_count = 0;
      hold(first); point(target.x, target.y, true); point(target.x, target.y, false);
      assert(!PointerDragActive() && callback_count == 0);
      assert(!home_layout.empty() && home_layout[0].id == "plugin-1" && home_layout[1].id == "plugin-0");
      auto *done = Find(home, "Done"); assert(done);
      {
        assert(lv_obj_get_width(done) >= 300 && lv_obj_get_height(done) >= 100);
        assert(Find(home, "1 x 2") && Find(home, "1 x 3") && Find(home, "2 x 3"));
        if (columns == 2) assert(!Find(home, "3 x 1"));
        else {
          for (const char *format : {"3 x 1", "3 x 2", "3 x 3"}) assert(Find(home, format));
          lv_obj_send_event(Find(home, "3 x 1"), LV_EVENT_CLICKED, nullptr); Tick();
          first = Find(home, "Plugin 0");
          const int expected_width = columns == 3 ? 1312 :
              2 * (1312 - 170) / (columns - 1) + 170;
          // Integer spacing can differ by one pixel depending on the starting column.
          assert(std::abs(lv_obj_get_width(first) - expected_width) <= 1);
        }
        auto *size = Find(home, "2 x 2"); assert(size);
        lv_obj_send_event(size, LV_EVENT_CLICKED, nullptr); Tick();
        first = Find(home, "Plugin 0"); assert(first);
        const int expected_height = columns < 4 ? 666 : columns == 4 ? 516 : 510;
        assert(lv_obj_get_height(first) == expected_height);
        assert(home_layout[1].size == 2);
        save(("/tmp/aera-home-edit-expanded-" + std::to_string(columns) + ".png").c_str());
        // A failed save restores both dimensions and the previous preference.
        save_succeeds = false;
        lv_obj_send_event(Find(home, "1 x 1"), LV_EVENT_CLICKED, nullptr); Tick();
        save_succeeds = true;
        assert(Find(home, "Layout unavailable"));
        first = Find(home, "Plugin 0"); assert(lv_obj_get_height(first) == expected_height);
        assert(DismissModal(home)); Tick();
      }
      assert(DismissModal(home)); Tick();
      // Loading another Home instance keeps the order and size.
      lv_screen_load(screen); lv_obj_delete(home); Tick();
      home = lv_obj_create(nullptr); BuildHomeScene(home, RecordAction, nullptr);
      lv_screen_load(home); Tick();
      first = Find(home, "Plugin 1"); second = Find(home, "Plugin 0");
      assert(centre(first).x < centre(second).x);
      // Drag to trash opens confirmation, without issuing a removal request.
      hold(first);
      auto *trash_label = FindLabelContaining(home, "Remove"); assert(trash_label);
      auto trash = centre(lv_obj_get_parent(trash_label));
      point(trash.x, trash.y, true); point(trash.x, trash.y, false); Tick();
      assert(!PointerDragActive() && callback_count == 0);
      assert(Find(home, "Remove plugin?"));
      assert(selected_plugin_request.id.empty());
      ConfirmSettingsModal(home); Tick();
      assert(selected_plugin_request.job == plugins::Job::kRemove && selected_plugin_request.id == "plugin-1");
      assert(callback_count == 1 && last_action == Action::kRunPluginOperation);
      callback_count = 0;
      // Holding the edge changes pages without losing the dragged object.
      hold(Find(home, "Plugin 1"));
      first = Find(home, "Plugin 1");
      auto *pager = lv_obj_get_parent(lv_obj_get_parent(first));
      point(1368, 1480, true, 50);
      assert(lv_obj_get_scroll_x(pager) > 0 && PointerDragActive());
      point(280, 1490, true); point(280, 1490, false);
      assert(!PointerDragActive() && callback_count == 0);
      assert(home_layout[0].id != "plugin-1");
      assert(DismissModal(home)); Tick();
      // Leaving Home mid-drag releases the global gesture claim and timers.
      hold(Find(home, "Plugin 1"));
      lv_screen_load(screen); lv_obj_delete(home); Tick();
      assert(!PointerDragActive());
      point(0, 0, false);
    }
    // Landscape also keeps all plugins accessible through pages.
    lv_display_set_resolution(display, 3120, 1440);
    home_layout.clear();
    auto *home = lv_obj_create(nullptr); BuildHomeScene(home, RecordAction, nullptr);
    lv_screen_load(home); Tick();
    hold(Find(home, "Plugin 0"));
    save("/tmp/aera-home-edit-landscape.png");
    point(300, 950, false);
    lv_screen_load(screen); lv_obj_delete(home); Tick();
    assert(!PointerDragActive());
    lv_indev_delete(input);
    lv_deinit(); std::filesystem::remove_all(backup_root);
    puts("Home plugin taps, reorder, sizes, persistence, trash confirmation and drag teardown passed.");
    return 0;
  }
  if (!strcmp(argv[1], "--home-art")) {
    for (const char *entry : {"retroarch", "browser", "telegram", "gallery", "media",
                             "recorder", "streams", "doom", "appvault", "main"}) {
      plugins::Plugin plugin;
      plugin.id = plugin.entry = entry;
      plugin.name = std::string("AERA ") + entry;
      plugin.description = "Test app description";
      home_plugins.push_back(plugin);
    }
    for (bool themed : {false, true}) {
      RecoverySetThemedHomeIcons(themed);
      auto *home = lv_obj_create(nullptr);
      BuildHomeScene(home, RecordAction, nullptr);
      lv_screen_load(home); Tick();
      auto *terminal = Find(home, "Terminal"); assert(terminal);
      // The terminal icon is an object tree containing an LVGL line; matrix
      // decoration is labels only and must remain in both modes.
      assert((FindType(terminal, &lv_line_class) != nullptr) == !themed);
      assert(Find(home, "AERA main"));
      save(themed ? "/tmp/aera-home-themed.png" : "/tmp/aera-home-classic.png");
      lv_screen_load(screen); lv_obj_delete(home);
    }
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Home themed/classic layouts and Terminal icon hiding passed.");
    return 0;
  }
  if (!strcmp(argv[1], "--art")) {
    using namespace themed_art;
    {
      detail::Artwork cached(Kind::Browser);
      cached.Prepare(600, 330, 44, false);
      assert(cached.renders == 1 && !cached.pixels.empty());
      assert((cached.pixels.front() >> 24) == 0);
      auto *pixels = cached.image.data;
      auto generation = cached.generation;
      for (int frame = 0; frame < 120; ++frame) cached.Prepare(600, 330, 44, false);
      assert(cached.renders == 1 && cached.image.data == pixels);
      assert(cached.generation == generation);
      cached.Prepare(200, 200, 34, false);
      assert(cached.renders == 2 && cached.generation != generation);
      cached.Prepare(200, 200, 34, true);
      assert(cached.renders == 3);
    }
    assert(ForPlugin("future-app", "main", "tools") == Kind::Tools);
    assert(ForPlugin("future-game", "main", "games") == Kind::Games);
    assert(ForPlugin("mirror", "main", "network") == Kind::Mirror);
    assert(ForPlugin("settings-backup", "main", "backup") == Kind::Backup);
    const char *names[] = {"Files", "Plugin Manager", "Browser", "Telegram", "Gallery",
      "Media", "Recorder", "RetroArch", "Doom", "Streams", "Mirror", "App Vault",
      "Settings", "System Info", "Other plugins"};
    for (int mode = 0; mode < 3; ++mode) {
      design::ApplyAppearanceMode(mode);
      lv_obj_set_style_bg_color(screen, design::kMainCanvas, 0);
      for (int i = 0; i < 15; ++i) {
        auto *card = lv_button_create(screen);
        design::Panel(card, 34, design::kMainPanel);
        lv_obj_set_pos(card, 30 + i % 3 * 470, 30 + i / 3 * 370);
        lv_obj_set_size(card, 440, 340);
        auto *art = Attach(card, static_cast<Kind>(i));
        assert(!lv_obj_has_flag(art, LV_OBJ_FLAG_CLICKABLE));
        assert(!lv_obj_get_style_clip_corner(card, LV_PART_MAIN));
        auto *label = design::Label(card, names[i], &lv_font_montserrat_28, design::kText);
        lv_obj_set_pos(label, 22, 245);
      }
      lv_display_set_resolution(display, 1440, 1900);
      const std::string path = "/tmp/aera-themed-art-" + std::to_string(mode) + ".png";
      save(path.c_str());
      lv_obj_clean(screen);
    }
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Themed artwork: every design rendered in three appearances; fallback and input transparency passed.");
    return 0;
  }
  if (!strcmp(argv[1], "--settings")) {
    auto *prefs = lv_obj_create(nullptr);
    BuildToolScene(prefs, Action::kPreferences, RecordAction, nullptr);
    lv_screen_load(prefs); Tick();
    assert(!Find(prefs, "Save preferences"));
    assert(Find(prefs, "Reset preferences"));
    auto *clock = FindPreferenceEntry(prefs, "24-hour clock"); assert(clock);
    const bool old_clock = RecoveryPreference(Preference::kClock24);
    lv_obj_send_event(clock, LV_EVENT_CLICKED, nullptr);
    assert(RecoveryPreference(Preference::kClock24) != old_clock);
    assert(autosave_count == 1);
    auto *brightness = FindPreferenceEntry(prefs, "Brightness"); assert(brightness);
    auto *slider = FindType(brightness, &lv_slider_class); assert(slider);
    lv_slider_set_value(slider, 70, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(autosave_count == 1);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    assert(autosave_count == 2);
    auto *timeout = FindPreferenceEntry(prefs, "Screen-off timeout"); assert(timeout);
    auto *timeout_slider = FindType(timeout, &lv_slider_class); assert(timeout_slider);
    assert(lv_slider_get_min_value(timeout_slider) == 30);
    assert(lv_slider_get_max_value(timeout_slider) == 300);
    assert(lv_slider_get_value(timeout_slider) == 60);
    lv_slider_set_value(timeout_slider, 300, LV_ANIM_OFF);
    lv_obj_send_event(timeout_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(Find(timeout, "300 s"));
    assert(screen_timeout == 60);
    lv_obj_send_event(timeout_slider, LV_EVENT_RELEASED, nullptr);
    assert(screen_timeout == 300);
    assert(autosave_count == 3);
    lv_obj_send_event(FindPreferenceEntry(prefs, "Haptics"), LV_EVENT_CLICKED, nullptr);
    Tick(2);
    save("/tmp/aera-preferences-haptics-host.png");
    for (const auto &item : std::vector<std::pair<const char *, Haptic>>{
        {"Touch feedback", Haptic::kTouch},
        {"Keyboard feedback", Haptic::kKeyboard},
        {"Operation feedback", Haptic::kAction}}) {
      auto *card = Find(prefs, item.first); assert(card);
      auto *control = FindType(card, &lv_slider_class); assert(control);
      const int before = autosave_count;
      lv_slider_set_value(control, 220, LV_ANIM_OFF);
      lv_obj_send_event(control, LV_EVENT_VALUE_CHANGED, nullptr);
      assert(autosave_count == before);
      lv_obj_send_event(control, LV_EVENT_RELEASED, nullptr);
      assert(autosave_count == before + 1);
      assert(RecoveryHapticDuration(item.second) == 220);
      auto *reset = Find(card, LV_SYMBOL_REFRESH); assert(reset);
      lv_obj_send_event(reset, LV_EVENT_CLICKED, nullptr);
      assert(autosave_count == before + 2);
      assert(RecoveryHapticDuration(item.second) == RecoveryDefaultHapticDuration(item.second));
      assert(lv_slider_get_value(control) == RecoveryDefaultHapticDuration(item.second));
    }
    assert(widgets::DismissModal(prefs)); Tick(2);
    const int before_zone = autosave_count;
    const int previous_zone = utc_offset;
    lv_obj_send_event(FindPreferenceEntry(prefs, "Time zone"), LV_EVENT_CLICKED, nullptr);
    Tick(2);
    auto *zone_plus = Find(prefs, LV_SYMBOL_PLUS); assert(zone_plus);
    lv_obj_send_event(zone_plus, LV_EVENT_CLICKED, nullptr);
    assert(utc_offset == previous_zone + 15 && autosave_count == before_zone + 1);
    lv_obj_send_event(Find(prefs, LV_SYMBOL_MINUS), LV_EVENT_CLICKED, nullptr);
    assert(utc_offset == previous_zone && autosave_count == before_zone + 2);
    utc_offset = 840;
    lv_obj_send_event(zone_plus, LV_EVENT_CLICKED, nullptr); assert(utc_offset == 840);
    utc_offset = -720;
    lv_obj_send_event(Find(prefs, LV_SYMBOL_MINUS), LV_EVENT_CLICKED, nullptr);
    assert(utc_offset == -720);
    utc_offset = previous_zone;
    assert(widgets::DismissModal(prefs)); Tick(2);
    for (bool main_failure : {true, false}) {
      save_succeeds = !main_failure;
      early_save_succeeds = main_failure;
      lv_obj_send_event(clock, LV_EVENT_CLICKED, nullptr);
      assert(Find(prefs, "Could not save preferences"));
      assert(widgets::DismissModal(prefs)); Tick(2);
    }
    save_succeeds = early_save_succeeds = true;
    const auto kept_accent = accent_color;
    const int before_reset = autosave_count;
    lv_obj_send_event(Find(prefs, "Reset preferences"), LV_EVENT_CLICKED, nullptr);
    assert(Find(prefs, "Restore default preferences? Theme, language, Wi-Fi and plugin settings are kept."));
    assert(autosave_count == before_reset);
    ConfirmSettingsModal(prefs); Tick(2);
    assert(autosave_count == before_reset + 1);
    assert(!RecoveryPreference(Preference::kClock24));
    assert(accent_color == kept_accent);
    assert(last_action == Action::kPreferences);
    lv_screen_load(screen); lv_obj_delete(prefs);
    png_image portrait{};
    portrait.version = PNG_IMAGE_VERSION;
    portrait.width = 2;
    portrait.height = 4;
    portrait.format = PNG_FORMAT_RGBA;
    const unsigned char portrait_pixels[32] = {};
    const std::string portrait_path = backup_root + "/portrait-wallpaper.png";
    assert(png_image_write_to_file(&portrait, portrait_path.c_str(), 0,
                                   portrait_pixels, 0, nullptr));
    assert(wallpaper::Load(portrait_path));
    assert(wallpaper::PreviewHeight(260, 172) == 520);
    auto *theme = lv_obj_create(nullptr);
    BuildToolScene(theme, Action::kTheme, RecordAction, nullptr);
    lv_screen_load(theme); Tick();
    auto *choose_wallpaper = Find(theme, "Choose image"); assert(choose_wallpaper);
    auto *wallpaper_card = lv_obj_get_parent(choose_wallpaper);
    auto *preview_image = FindType(wallpaper_card, &lv_image_class); assert(preview_image);
    assert(lv_image_get_inner_align(preview_image) == LV_IMAGE_ALIGN_CONTAIN);
    assert(lv_obj_get_width(lv_obj_get_parent(preview_image)) == 260);
    assert(lv_obj_get_height(lv_obj_get_parent(preview_image)) == 520);
    assert(lv_obj_get_height(wallpaper_card) == 568);
    assert(lv_obj_get_height(choose_wallpaper) == 108);
    assert(lv_obj_get_height(Find(theme, "Reset wallpaper")) == 108);
    auto *surface_title = FindLabel(theme, "Surface style"); assert(surface_title);
    assert(lv_obj_get_y(surface_title) >= lv_obj_get_y(wallpaper_card) + lv_obj_get_height(wallpaper_card));
    assert(!Find(theme, "Save theme"));
    assert(Find(theme, "Reset theme settings"));
    assert(!RecoveryThemedHomeIcons());
    const int before_icons = autosave_count;
    lv_obj_send_event(Find(theme, "Themed"), LV_EVENT_CLICKED, nullptr);
    Tick(2);
    assert(RecoveryThemedHomeIcons());
    assert(autosave_count == before_icons + 1);
    const int before = autosave_count;
    auto *preset = Find(theme, "Violet"); assert(preset);
    lv_obj_send_event(preset, LV_EVENT_CLICKED, nullptr);
    assert(autosave_count == before + 1);
    assert(accent_color == 0xa991ff);
    save("/tmp/aera-theme-autosave.png");
    lv_obj_send_event(Find(theme, "Reset theme settings"), LV_EVENT_CLICKED, nullptr);
    assert(autosave_count == before + 1);
    ConfirmSettingsModal(theme); Tick(2);
    assert(autosave_count == before + 2);
    assert(accent_color == design::kDefaultAccentRgb);
    assert(!RecoveryThemedHomeIcons());
    assert(last_action == Action::kTheme);
    lv_screen_load(screen); lv_obj_delete(theme);
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Settings autosave, deferred sliders, per-haptic resets, reset confirmation and save failures passed");
    return 0;
  }
  if (!strcmp(argv[1], "--pc-settings")) {
    aera::pc::SetEnabled(true, 443, false);
    aera::pc::pc_status.address = "https://aera.local";
    aera::pc::pc_status.ip_address = "https://192.168.1.42";
    aera::pc::pc_status.certificate = std::string(64, 'a');
    for (const char *language : {"en", "de_DE", "fr_FR", "sv_SE", "zh_CN", "ar_SA"})
      for (bool landscape : {false, true})
        for (auto size : {InterfaceSize::kSmall, InterfaceSize::kNormal,
                          InterfaceSize::kLarge, InterfaceSize::kSpacious}) {
          assert(i18n::SetLanguage(language));
          interface_size = size;
          design::ApplyInterfaceSize(static_cast<int>(size));
          lv_display_set_resolution(display, landscape ? 3168 : 1440,
                                    landscape ? 1440 : 3168);
          auto *wifi = lv_obj_create(nullptr);
          BuildToolScene(wifi, Action::kPcConnection, RecordAction, nullptr);
          lv_screen_load(wifi); Tick(2);
          for (const char *text : {"adb -d forward tcp:8443 tcp:443",
                                   "https://localhost:8443",
                                   "https://aera.local\nhttps://192.168.1.42"}) {
            auto *label = FindLabel(wifi, text); assert(label);
            auto *card = lv_obj_get_parent(label);
            assert(lv_obj_get_height(card) >= 128);
            assert(lv_label_get_long_mode(label) == LV_LABEL_LONG_WRAP);
            AssertContained(label, card);
            AssertContained(card, lv_obj_get_parent(card));
            auto *section = lv_obj_get_parent(card);
            auto *details = lv_obj_get_parent(section);
            assert(lv_obj_get_x(details) == 0);
            assert(lv_obj_get_width(section) == lv_obj_get_width(lv_obj_get_parent(details)));
          }
          if (!landscape && size == InterfaceSize::kNormal && !strcmp(language, "en"))
            save("/tmp/aera-pc-command-cards.png");
          for (const char *tag : {"PC connection", "Auto-enable PC connection"}) {
            auto *expected = lv_label_create(wifi);
            lv_label_set_text(expected, i18n::Translate(tag));
            const std::string displayed = lv_label_get_text(expected);
            lv_obj_delete(expected);
            auto *label = FindLabel(wifi, displayed.c_str());
            if (label && lv_obj_get_parent(label) == wifi) {
              label = nullptr;
              for (uint32_t i = 0; i < lv_obj_get_child_count(wifi); ++i) {
                auto *candidate = FindLabel(lv_obj_get_child(wifi, i), displayed.c_str());
                if (candidate && lv_obj_get_parent(candidate) != wifi) { label = candidate; break; }
              }
            }
            if (!label) fprintf(stderr, "Missing PC label %s in %s, size %d, landscape %d\n",
                                tag, language, static_cast<int>(size), landscape);
            assert(label);
            auto *row = lv_obj_get_parent(label);
            AssertContained(label, row);
            auto *copy = lv_obj_get_child(row, 1);
            AssertContained(copy, row);
            assert(!Overlaps(label, copy));
            auto *toggle = lv_obj_get_child(row, 2);
            assert(!Overlaps(label, toggle));
            assert(!Overlaps(copy, toggle));
          }
          if (!landscape && size == InterfaceSize::kLarge && !strcmp(language, "de_DE"))
            save("/tmp/aera-pc-settings-host.png");
          lv_screen_load(screen); lv_obj_delete(wifi); Tick(1);
        }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Menu PC settings fit six languages, four sizes and both orientations");
    return 0;
  }
  if (!strcmp(argv[1], "--payload-protection")) {
    const bool old_abl = preferences[static_cast<int>(Preference::kPreserveAbl)];
    const bool old_recovery = preferences[static_cast<int>(Preference::kPreserveRecovery)];
    payload::Info info;
    info.valid = info.is_payload = true;
    info.partitions = {{"boot", 4096, 1, true, {}, std::string(32, 'b')},
                       {"abl", 4096, 1, true, {}, std::string(32, 'a')},
                       {"recovery", 4096, 1, true, {}, std::string(32, 'r')}};
    const std::vector<PayloadFlashTarget> targets = {{"boot", "/boot", 8192, false, 0, {}}};
    for (bool abl : {false, true}) for (bool recovery : {false, true}) {
      preferences[static_cast<int>(Preference::kPreserveAbl)] = abl;
      preferences[static_cast<int>(Preference::kPreserveRecovery)] = recovery;
      const auto protect = payload::ProtectedPartitions();
      assert(bool(protect.count("abl")) == abl);
      assert(bool(protect.count("recovery")) == recovery);
      std::vector<std::string> names;
      std::string error;
      const bool ok = payload::PlanFullFlash(info, targets, names, error, protect);
      assert(ok == (abl && recovery));
      if (ok) assert(names == std::vector<std::string>{"boot"});
      else assert(names.empty());
    }
    info.partitions.erase(info.partitions.begin());
    std::vector<std::string> names;
    std::string error;
    assert(!payload::PlanFullFlash(info, targets, names, error, {"abl", "recovery"}));
    preferences[static_cast<int>(Preference::kPreserveAbl)] = old_abl;
    preferences[static_cast<int>(Preference::kPreserveRecovery)] = old_recovery;
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("ABL/recovery protection and full-payload planning passed; no partitions written");
    return 0;
  }
  if (!strcmp(argv[1], "--preferences-layout")) {
    for (const char *language : {"en", "de_DE", "sv_SE"})
    for (bool landscape : {false, true})
      for (auto size : {InterfaceSize::kSmall, InterfaceSize::kNormal,
                        InterfaceSize::kLarge, InterfaceSize::kSpacious})
        for (bool abl : {false, true})
          for (bool haptics : {false, true}) {
            assert(i18n::SetLanguage(language));
            abl_preservation_supported = abl;
            haptics_available = haptics;
            interface_size = size;
            design::ApplyInterfaceSize(static_cast<int>(size));
            lv_display_set_resolution(display, landscape ? 3168 : 1440,
                                      landscape ? 1440 : 3168);
            auto *prefs = lv_obj_create(nullptr);
            BuildToolScene(prefs, Action::kPreferences, RecordAction, nullptr);
            lv_screen_load(prefs); Tick(1);
            std::vector<const char *> titles{
                "Language", "Brightness", "24-hour clock", "Time zone", "Gesture navigation"};
            if (haptics) titles.push_back("Haptics");
            titles.push_back("Show hidden files");
            titles.push_back("Verify ZIP signatures");
            titles.push_back("Keep AERA installed");
            if (abl) titles.push_back("Keep current ABL");
            titles.push_back("Compress backups by default");
            titles.push_back("SHA-256 backup checksums");
            titles.push_back("USB file transfer");
            titles.push_back("Reset preferences");
            lv_obj_t *previous = nullptr;
            for (const char *title : titles) {
              auto *item = FindPreferenceEntry(prefs, i18n::Translate(title)); assert(item);
              auto inspect = [&](auto &&self, lv_obj_t *object) -> void {
                for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
                  auto *child = lv_obj_get_child(object, i);
                  AssertContained(child, object);
                  self(self, child);
                }
              };
              inspect(inspect, item);
              if (previous) {
                assert(Bounds(previous).y2 < Bounds(item).y1);
                assert(!Overlaps(previous, item));
              }
              previous = item;
            }
            auto *reset = Find(prefs, i18n::Translate("Reset preferences"));
            auto *list = lv_obj_get_parent(reset);
            lv_obj_scroll_to_view(reset, LV_ANIM_OFF); Tick(1);
            AssertContained(reset, list);
            if (!strcmp(language, "en") && !landscape && abl && haptics && size == InterfaceSize::kNormal)
              save("/tmp/aera-preferences-bottom-host.png");
            lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); Tick(1);
            if (!strcmp(language, "en") && !landscape && abl && haptics && size == InterfaceSize::kNormal)
              save("/tmp/aera-preferences-top-host.png");
            if (haptics) {
              lv_obj_send_event(FindPreferenceEntry(prefs, i18n::Translate("Haptics")),
                                LV_EVENT_CLICKED, nullptr);
              Tick(2);
              lv_obj_t *previous_card = nullptr;
              for (const char *name : {"Touch feedback", "Keyboard feedback", "Operation feedback"}) {
                auto *card = Find(prefs, i18n::Translate(name)); assert(card);
                assert(FindType(card, &lv_slider_class));
                if (previous_card) assert(Bounds(previous_card).y2 < Bounds(card).y1);
                previous_card = card;
              }
              lv_obj_scroll_to_view(previous_card, LV_ANIM_OFF); Tick(2);
              AssertContained(previous_card, lv_obj_get_parent(previous_card));
              assert(widgets::DismissModal(prefs)); Tick(2);
            }
            lv_screen_load(screen); lv_obj_delete(prefs);
          }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Preferences layout: ordered rows, separate sliders and reachable bottom reset passed");
    return 0;
  }
  if (!strcmp(argv[1], "--settings-navigation")) {
    assert(i18n::SetLanguage("en"));
    for (bool supported : {false, true}) {
      preservation_supported = supported;
      abl_preservation_supported = false;
      auto *independent = lv_obj_create(nullptr);
      BuildToolScene(independent, Action::kPreferences, RecordAction, nullptr);
      auto *keep_aera = FindPreferenceEntry(independent, "Keep AERA installed");
      assert(keep_aera && !Find(independent, "Keep current ABL"));
      const bool previous = RecoveryPreference(Preference::kPreserveRecovery);
      lv_obj_send_event(keep_aera, LV_EVENT_CLICKED, nullptr);
      assert(RecoveryPreference(Preference::kPreserveRecovery) != previous);
      lv_obj_send_event(keep_aera, LV_EVENT_CLICKED, nullptr);
      assert(RecoveryPreference(Preference::kPreserveRecovery) == previous);
      lv_obj_delete(independent);
    }
    preservation_supported = true;
    abl_preservation_supported = true;
    auto *menu = lv_obj_create(nullptr);
    BuildToolScene(menu, Action::kSettings, RecordAction, nullptr);
    assert(Find(menu, "PC connection"));
    assert(!Find(menu, "Language"));
    lv_obj_send_event(Find(menu, "PC connection"), LV_EVENT_CLICKED, nullptr);
    assert(last_action == Action::kPcConnection);
    lv_obj_delete(menu);
    auto *prefs = lv_obj_create(nullptr);
    BuildToolScene(prefs, Action::kPreferences, RecordAction, nullptr);
    assert(Find(prefs, "Language"));
    lv_obj_send_event(FindPreferenceEntry(prefs, "Language"), LV_EVENT_CLICKED, nullptr);
    assert(last_action == Action::kLanguage);
    lv_obj_delete(prefs);
    for (int layout = 0; layout <= kDockLayoutMax; ++layout)
      for (bool landscape : {false, true})
        for (auto size : {InterfaceSize::kSmall, InterfaceSize::kNormal,
                          InterfaceSize::kLarge, InterfaceSize::kSpacious}) {
          dock_layout = static_cast<DockLayout>(layout);
          interface_size = size;
          design::ApplyInterfaceSize(static_cast<int>(size));
          lv_display_set_resolution(display, landscape ? 3168 : 1440,
                                    landscape ? 1440 : 3168);
          auto *language = lv_obj_create(nullptr);
          BuildToolScene(language, Action::kLanguage, RecordAction, nullptr);
          lv_screen_load(language); Tick(1);
          lv_obj_t *list = nullptr;
          for (uint32_t i = 0; i < lv_obj_get_child_count(language); ++i) {
            auto *child = lv_obj_get_child(language, i);
            if (lv_obj_get_scrollbar_mode(child) == LV_SCROLLBAR_MODE_ON) list = child;
          }
          assert(list);
          assert(lv_obj_get_y(list) + lv_obj_get_height(list) ==
              lv_obj_get_height(language) - widgets::NavigationHeight(language) - 28);
          assert(lv_obj_get_width(list) == (landscape ? 3040 : 1312));
          assert(lv_obj_get_scroll_bottom(list) > 0);
          if (landscape && layout == 0 && size == InterfaceSize::kNormal)
            save("/tmp/aera-language-landscape-host.png");
          lv_screen_load(screen); lv_obj_delete(language);
        }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("Settings destinations and language viewport match all five docks and four sizes");
    return 0;
  }
  if (!strcmp(argv[1], "--file-dates")) {
    for (bool landscape : {false, true}) for (bool clock24 : {false, true})
      for (auto size : {InterfaceSize::kSmall, InterfaceSize::kNormal,
                        InterfaceSize::kLarge, InterfaceSize::kSpacious}) {
        lv_display_set_resolution(display, landscape ? 3168 : 1440,
                                  landscape ? 1440 : 3168);
        interface_size = size;
        preferences[static_cast<int>(Preference::kClock24)] = clock24;
        auto *files = lv_obj_create(nullptr);
        BuildFilesScene(files, RecordAction, nullptr);
        lv_screen_load(files); Tick();
        auto *row = Find(files, "alpha.txt"); assert(row);
        auto *date = FindLabelContaining(row, ":"); assert(date);
        const char *text = lv_label_get_text(date);
        assert(strlen(text) == (clock24 ? 17 : 20));
        const auto *font = lv_obj_get_style_text_font(date, LV_PART_MAIN);
        lv_point_t measured{};
        lv_text_get_size(&measured, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        assert(lv_obj_get_width(date) >= measured.x);
        assert(lv_obj_get_height(date) >= measured.y);
        AssertContained(date, row);
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
          auto *child = lv_obj_get_child(row, i);
          if (child != date && lv_obj_check_type(child, &lv_label_class) &&
              Bounds(child).y1 == Bounds(date).y1)
            assert(!Overlaps(child, date));
        }
        lv_screen_load(screen); lv_obj_delete(files); Tick(2);
      }
    lv_display_delete(display); lv_deinit();
    std::filesystem::remove_all(backup_root);
    puts("File timestamps fit all four sizes, both clock formats and both orientations");
    return 0;
  }
  if (!strcmp(argv[1], "--about")) {
    auto *about=lv_obj_create(nullptr);
    BuildAboutScene(about,RecordAction,nullptr);
    lv_screen_load(about); Tick();
    assert(Find(about,"Jonas Salo · koaaN"));
    assert(Find(about,"Daniel Springer · Daniel210191"));
    assert(Find(about,"R1.0"));
    save("/tmp/aera-about-host.png");
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    return 0;
  }
  if (strcmp(argv[1], "--files")) {
  auto *wifi=lv_obj_create(nullptr); BuildWifiScene(wifi,RecordAction,nullptr);
  lv_screen_load(wifi); Tick();
  assert(Find(wifi,"AERA Lab"));
  lv_obj_send_event(Find(wifi,"AERA Lab"),LV_EVENT_CLICKED,nullptr);
  assert(FindType(wifi,&lv_keyboard_class));
  auto *wifi_password=FindType(wifi,&lv_textarea_class); assert(wifi_password);
  AssertVisibleCaret(wifi_password);
  lv_textarea_set_text(wifi_password,"correct horse battery staple");
  lv_obj_send_event(Find(wifi,"Connect"),LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kRunWifiOperation);
  assert(GetWifiRequest().ssid=="AERA Lab");
  assert(GetWifiRequest().password=="correct horse battery staple");
  auto *wifi_settings_label = FindLabelContaining(wifi,"Wi-Fi settings");
  assert(wifi_settings_label);
  auto *wifi_settings = lv_obj_get_parent(wifi_settings_label);
  lv_obj_send_event(wifi_settings,LV_EVENT_CLICKED,nullptr); Tick();
  assert(Find(wifi,"Automation & wireless debugging"));
  assert(Find(wifi,"Wireless ADB is off"));
  assert(RecoverySetAdbOverWifi(true)); Tick();
  assert(Find(wifi,"adb connect 192.168.1.42:5555"));
  auto *pair = Find(wifi,"Pair new computer"); assert(pair);
  lv_obj_send_event(pair,LV_EVENT_CLICKED,nullptr); Tick();
  assert(Find(wifi,"adb pair 192.168.1.42:37123"));
  assert(Find(wifi,"482913"));
  lv_screen_load(screen); lv_obj_delete(wifi);
  auto *web = lv_obj_create(nullptr); BuildWebScene(web, RecordAction, nullptr, -1, -1, false);
  lv_screen_load(web); Tick();
  auto *web_input = FindType(web, &lv_textarea_class); assert(web_input);
  AssertVisibleCaret(web_input);
  auto *web_keyboard = FindType(web, &lv_keyboard_class); assert(web_keyboard);
  // Browser surfaces are gesture-first and no longer contain the recovery dock.
  assert(!Find(web, "Menu"));
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  lv_obj_send_event(web_input, LV_EVENT_CLICKED, nullptr);
  assert(!lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  Tick(); save("/tmp/aera-browser-keyboard-host.png");
  lv_textarea_set_text(web_input, "example.org");
  const int before_browser_go = callback_count;
  lv_obj_send_event(web_keyboard, LV_EVENT_READY, nullptr);
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(!strcmp(lv_textarea_get_text(web_input), "https://example.org"));
  assert(Find(web, "Browsing unavailable"));
  assert(callback_count == before_browser_go); // Never starts a recovery job.
  assert(widgets::DismissModal(web));
  lv_obj_send_event(Find(web, "Verify and prepare engine"), LV_EVENT_CLICKED, nullptr);
  Tick(); assert(Find(web, "Runtime verified"));
  save("/tmp/aera-browser-host.png");
  lv_screen_load(screen); lv_obj_delete(web);
  int browser_channels[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, browser_channels) == 0);
  int browser_frame = memfd_create("aera-ui-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  assert(browser_frame >= 0 && ftruncate(browser_frame, web::kSharedBytes) == 0);
  auto *browser_pixels = static_cast<uint32_t *>(mmap(nullptr, web::kSharedBytes,
      PROT_READ | PROT_WRITE, MAP_SHARED, browser_frame, 0));
  assert(browser_pixels != MAP_FAILED);
  auto *browser_slot = browser_pixels +
      web::FrameSlot(1) * web::kWidth * web::kHeight;
  std::fill(browser_slot, browser_slot + web::kWidth * web::kHeight, 0xff38536b);
  assert(fcntl(browser_frame, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
  web = lv_obj_create(nullptr); BuildWebScene(web, RecordAction, nullptr,
      browser_frame, browser_channels[0], false);
  lv_screen_load(web);
  web::Message browser_message; browser_message.kind = web::Kind::kFrame;
  browser_message.sequence = 1; browser_message.x = web::kWidth; browser_message.y = web::kHeight;
  browser_message.value = web::kFrameBytes;
  assert(send(browser_channels[1], &browser_message, sizeof(browser_message), 0) == sizeof(browser_message));
  Tick();
  web::Message browser_ack;
  assert(recv(browser_channels[1], &browser_ack, sizeof(browser_ack), MSG_DONTWAIT) == sizeof(browser_ack));
  assert(browser_ack.kind == web::Kind::kAck && browser_ack.sequence == 1);
  assert(!lv_obj_is_visible(Find(web, "Web engine installed")));
  web::Message keyboard_message; keyboard_message.kind = web::Kind::kKeyboardShow;
  assert(send(browser_channels[1], &keyboard_message, sizeof(keyboard_message), 0) == sizeof(keyboard_message));
  Tick();
  web_keyboard = FindType(web, &lv_keyboard_class); assert(web_keyboard);
  assert(!lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  keyboard_message = web::Message{}; keyboard_message.kind = web::Kind::kKeyboardHide;
  assert(send(browser_channels[1], &keyboard_message, sizeof(keyboard_message), 0) == sizeof(keyboard_message));
  Tick();
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  web_input = FindType(web, &lv_textarea_class);
  lv_textarea_set_text(web_input, "example.org");
  lv_obj_send_event(Find(web, "Go"), LV_EVENT_CLICKED, nullptr);
  assert(recv(browser_channels[1], &browser_ack, sizeof(browser_ack), MSG_DONTWAIT) == sizeof(browser_ack));
  assert(browser_ack.kind == web::Kind::kOpen && !strcmp(browser_ack.text, "https://example.org"));
  save("/tmp/aera-browser-viewport-host.png");
  close(browser_channels[1]); Tick(); assert(Find(web, "Browser stopped"));
  lv_screen_load(screen); lv_obj_delete(web);
  munmap(browser_pixels, web::kSharedBytes);
  JobRequest format_request; format_request.job=Job::kFormatData; format_request.path="/data";
  for(const char *invalid : {"", "YES", "yes ", "yesplease", " yes", "yes\n"}) {
    format_request.confirmation=invalid; assert(!FormatDataAuthorized(format_request));
  }
  format_request.confirmation="yes"; assert(FormatDataAuthorized(format_request));
  format_request.path="/metadata"; assert(!FormatDataAuthorized(format_request));
  format_request.path="/data"; format_request.partitions={"/data"}; assert(!FormatDataAuthorized(format_request));
  format_request.partitions.clear(); format_request.job=Job::kWipe; assert(!FormatDataAuthorized(format_request));
  JobRequest cow_request; cow_request.job=Job::kClearSnapshotCow;
  assert(!SnapshotCowCleanupAuthorized(cow_request));
  cow_request.confirmation="remove-cow"; assert(SnapshotCowCleanupAuthorized(cow_request));
  cow_request.path="/super"; assert(!SnapshotCowCleanupAuthorized(cow_request));
  auto *wipe=lv_obj_create(nullptr); BuildToolScene(wipe,Action::kWipe,RecordAction,nullptr);
  lv_screen_load(wipe); Tick();
  assert(Find(wipe,"Format Data"));
  assert(Find(wipe,"Snapshot COW cleanup"));
  auto *wipe_review=Find(wipe,"Review wipe"); assert(wipe_review && lv_obj_has_state(wipe_review,LV_STATE_DISABLED));
  auto *wipe_data=Find(wipe,"Data"); assert(wipe_data); lv_obj_send_event(wipe_data,LV_EVENT_CLICKED,nullptr);
  assert(!lv_obj_has_state(wipe_review,LV_STATE_DISABLED));
  lv_area_t bar_area{};
  lv_obj_get_coords(lv_obj_get_parent(Find(wipe,"Home")),&bar_area);
  assert(bar_area.y2==3167);
  save("/tmp/aera-wipe-host.png");
  lv_obj_send_event(Find(wipe,"Format Data"),LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kFormatData);
  lv_screen_load(screen); lv_obj_delete(wipe);
  auto *format=lv_obj_create(nullptr); BuildToolScene(format,Action::kFormatData,RecordAction,nullptr);
  lv_screen_load(format); Tick();
  auto *input=FindType(format,&lv_textarea_class), *submit=Find(format,"Format data now");
  assert(input && submit && lv_obj_has_state(submit,LV_STATE_DISABLED));
  callback_count=0;
  for(const char *invalid : {"", "YES", "yes ", "yesplease", " yes"}) {
    lv_textarea_set_text(input,invalid);
    assert(lv_obj_has_state(submit,LV_STATE_DISABLED));
    lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr); assert(callback_count==0);
  }
  lv_textarea_set_text(input,"yes");
  assert(!lv_obj_has_state(submit,LV_STATE_DISABLED) && callback_count==0);
  lv_obj_send_event(FindType(format,&lv_keyboard_class),LV_EVENT_READY,nullptr);
  assert(callback_count==0);
  save("/tmp/aera-format-host.png");
  lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr);
  assert(callback_count==1 && last_action==Action::kRunOperation);
  assert(FormatDataAuthorized(GetJobRequest()));
  assert(lv_obj_has_state(submit,LV_STATE_DISABLED) && !strcmp(lv_textarea_get_text(input),""));
  lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr); assert(callback_count==1);
  lv_obj_send_event(Find(format,"Cancel"),LV_EVENT_CLICKED,nullptr); assert(last_action==Action::kWipe);
  lv_screen_load(screen); lv_obj_delete(format);
  auto *prefs=lv_obj_create(nullptr); BuildToolScene(prefs,Action::kPreferences,RecordAction,nullptr);
  lv_screen_load(prefs); Tick();
  auto *clock_row=FindPreferenceEntry(prefs,"24-hour clock");
  assert(clock_row && lv_obj_get_width(clock_row)>=1200);
  for(const auto &item : std::vector<std::pair<const char*,Preference>>{
      {"24-hour clock",Preference::kClock24},{"Gesture navigation",Preference::kRecents},
      {"Show hidden files",Preference::kHiddenFiles},
      {"Verify ZIP signatures",Preference::kVerifyZip},{"Compress backups by default",Preference::kCompression},
      {"Keep AERA installed",Preference::kPreserveRecovery},
      {"Keep current ABL",Preference::kPreserveAbl},
      {"Generate backup checksums",Preference::kBackupChecksums},
      {"Verify backup checksums",Preference::kVerifyBackupChecksums},
      {"SHA-256 backup checksums",Preference::kSha256}}) {
    const bool old=RecoveryPreference(item.second);
    auto *toggle=FindPreferenceEntry(prefs,item.first); assert(toggle);
    lv_obj_send_event(toggle,LV_EVENT_CLICKED,nullptr); assert(RecoveryPreference(item.second)!=old);
    lv_obj_send_event(toggle,LV_EVENT_CLICKED,nullptr); assert(RecoveryPreference(item.second)==old);
  }
  save("/tmp/aera-preferences-host.png");
  lv_obj_send_event(FindPreferenceEntry(prefs,"Time zone"),LV_EVENT_CLICKED,nullptr); Tick(2);
  lv_obj_send_event(Find(prefs,LV_SYMBOL_PLUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==135);
  utc_offset=840; lv_obj_send_event(Find(prefs,LV_SYMBOL_PLUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==840);
  utc_offset=-720; lv_obj_send_event(Find(prefs,LV_SYMBOL_MINUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==-720);
  assert(widgets::DismissModal(prefs)); Tick(2);
  assert(!Find(prefs,"Save preferences"));
  assert(Find(prefs,"Reset preferences"));
  save_succeeds=false;
  lv_obj_send_event(clock_row,LV_EVENT_CLICKED,nullptr);
  assert(Find(prefs,"Could not save preferences")); assert(widgets::DismissModal(prefs)); Tick();
  save_succeeds=true;
  lv_screen_load(screen); lv_obj_delete(prefs);
  auto *users=lv_obj_create(nullptr);
  BuildToolScene(users,Action::kUsers,RecordAction,nullptr);
  lv_screen_load(users); Tick();
  assert(Find(users,"Owner") && Find(users,"Unlocked"));
  auto *work_user=Find(users,"Work profile"); assert(work_user);
  lv_obj_send_event(work_user,LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kDecryptUser);
  const auto user_request=GetUserDecryptRequest();
  assert(user_request.user.id==10 && user_request.user.credential_type==2);
  lv_screen_load(screen); lv_obj_delete(users);
  BuildToolScene(screen,Action::kBackup,[](Action,void*){},nullptr);
  assert(!lv_obj_get_style_bg_image_tiled(screen,LV_PART_MAIN));
  assert(lv_obj_get_style_bg_grad_dir(screen,LV_PART_MAIN)==LV_GRAD_DIR_NONE);
  const auto *grain=static_cast<const lv_image_dsc_t *>(lv_obj_get_style_bg_image_src(screen,LV_PART_MAIN));
  assert(grain && grain->header.w==1440 && grain->header.h==3168);
  assert(grain->header.cf==LV_COLOR_FORMAT_XRGB8888 && grain->header.stride==1440*4);
  assert(memcmp(grain->data,grain->data+192*4,192*4)!=0);
  assert(memcmp(grain->data,grain->data+192*grain->header.stride,192*grain->header.stride)!=0);
  Tick();
  auto *row=Find(screen,"Boot"); assert(row); lv_obj_send_event(row,LV_EVENT_CLICKED,nullptr);
  auto *review=Find(screen,"Review backup"); assert(review && !lv_obj_has_state(review,LV_STATE_DISABLED));
  save("/tmp/aera-matte-backup-host.png");
  auto *restore=lv_obj_create(nullptr); BuildToolScene(restore,Action::kRestore,[](Action,void*){},nullptr);
  lv_screen_load(restore); Tick();
  auto *restore_review=Find(restore,"Review restore"); assert(restore_review && lv_obj_has_state(restore_review,LV_STATE_DISABLED));
  auto *folder=Find(restore,"AERA-test-backup"); assert(folder); lv_obj_send_event(folder,LV_EVENT_CLICKED,nullptr); Tick();
  auto *restore_boot=Find(restore,"Boot"); assert(restore_boot); lv_obj_send_event(restore_boot,LV_EVENT_CLICKED,nullptr);
  assert(!lv_obj_has_state(restore_review,LV_STATE_DISABLED));
  save("/tmp/aera-restore-host.png"); lv_screen_load(screen); lv_obj_delete(restore);
  auto *unlock=lv_obj_create(nullptr); BuildDecryptScene(unlock,3,true,0,3,[](Action,void*){},nullptr);
  lv_screen_load(unlock); save("/tmp/aera-matte-unlock-host.png"); lv_screen_load(screen); lv_obj_delete(unlock);
  for(int i=0;i<8;++i) {
    OpenPicture(screen,argv[1]);
    Tick(5);
    if(i%2) Tick(500); // Alternate immediate cancellation and completed decode.
    auto *plus=Find(screen,LV_SYMBOL_PLUS);
    if (!plus) {
      assert(widgets::DismissModal(screen));
      Tick(20);
      continue;
    }
    lv_obj_send_event(plus,LV_EVENT_CLICKED,nullptr); Tick(5);
    if(i==7) save("/tmp/aera-picture-host.png");
    assert(widgets::DismissModal(screen)); Tick(20);
  }
  auto *job=lv_obj_create(nullptr);
  JobRequest request; request.job=Job::kBackup; request.title="Backup"; request.path="/tmp/test-backup";
  auto operation=BuildJobScene(job,request,[](Action,void*){},nullptr);
  lv_screen_load(job); RefreshOperationScene(operation); save("/tmp/aera-operation-host.png");
  CompleteOperationScene(operation,true,"Backup completed"); Tick();
  lv_screen_load(screen); lv_obj_delete(job);
  recovery_progress=0;
  installer_status="- Preparing recovery update...\n- Installing AERA to slot _a...";
  auto *install_job=lv_obj_create(nullptr);
  JobRequest install_request; install_request.job=Job::kInstall; install_request.title="Install AERA Update";
  auto installing=BuildJobScene(install_job,install_request,[](Action,void*){},nullptr);
  lv_screen_load(install_job); RefreshOperationScene(installing); Tick();
  assert(Find(install_job,"Installing"));
  assert(Find(install_job,"Installing AERA to slot _a..."));
  assert(Find(install_job,"Preparing recovery update...\nInstalling AERA to slot _a..."));
  lv_screen_load(screen); lv_obj_delete(install_job);
  recovery_progress=38;
  installer_status.clear();
  auto *format_job=lv_obj_create(nullptr);
  format_request.job=Job::kFormatData;
  auto formatting=BuildJobScene(format_job,format_request,RecordAction,nullptr);
  lv_screen_load(format_job); Tick();
  assert(lv_obj_has_flag(formatting.done,LV_OBJ_FLAG_HIDDEN));
  CompleteOperationScene(formatting,true,"Format completed"); Tick();
  assert(Find(format_job,"Data was formatted successfully. Reboot recovery before using /data again."));
  auto *reboot_options=Find(format_job,"Reboot options"); assert(reboot_options);
  lv_obj_send_event(reboot_options,LV_EVENT_CLICKED,nullptr); assert(last_action==Action::kOpenReboot);
  lv_screen_load(screen); lv_obj_delete(format_job);
  auto *sideload=lv_obj_create(nullptr);
  BuildSideloadScene(sideload,RecordAction,nullptr);
  lv_screen_load(sideload); Tick();
  assert(Find(sideload,"Start ADB sideload"));
  assert(Find(sideload,"adb sideload package.zip"));
  assert(Find(sideload,"No partition or persistent file will be modified.")==nullptr);
  lv_screen_load(screen); lv_obj_delete(sideload);
  sideload_status={true,false,32ULL*1024*1024,64ULL*1024*1024};
  installer_status="Reading test package";
  auto *sideload_job=lv_obj_create(nullptr);
  JobRequest sideload_request; sideload_request.job=Job::kSideload;
  sideload_request.title="ADB Sideload";
  auto receiving=BuildJobScene(sideload_job,sideload_request,RecordAction,nullptr);
  lv_screen_load(sideload_job); RefreshOperationScene(receiving); Tick();
  assert(Find(sideload_job,"50%"));
  assert(Find(sideload_job,"32.0 MB / 64.0 MB"));
  assert(Find(sideload_job,"Reading test package"));
  lv_screen_load(screen); lv_obj_delete(sideload_job);
  installer_status.clear(); sideload_status={};
  }

  const std::vector<std::pair<InterfaceSize, const char*>> file_sizes = {
      {InterfaceSize::kSmall, "small"},
      {InterfaceSize::kNormal, "normal"},
      {InterfaceSize::kLarge, "large"},
      {InterfaceSize::kSpacious, "spacious"},
  };
  for (bool landscape : {false, true}) {
    lv_display_set_resolution(display, landscape ? 3168 : 1440,
                              landscape ? 1440 : 3168);
    for (const auto& [size, size_name] : file_sizes) {
      interface_size = size;
      design::ApplyInterfaceSize(static_cast<int>(size));
      auto* files = lv_obj_create(nullptr);
      BuildFilesScene(files, RecordAction, nullptr);
      lv_screen_load(files);
      Tick();
      assert(Find(files, "alpha.txt"));
      assert(Find(files, "long-file-name-for-layout-validation.txt"));
      auto* alpha_row = Find(files, "alpha.txt");
      assert(lv_obj_get_height(alpha_row) ==
             (size == InterfaceSize::kSmall ? 150
              : size == InterfaceSize::kSpacious ? 264
              : size == InterfaceSize::kLarge ? 228
                                               : 174));
      assert(Find(files, LV_SYMBOL_PLUS));
      for (const char* symbol : {LV_SYMBOL_LIST, LV_SYMBOL_PLUS,
                                 LV_SYMBOL_OK, LV_SYMBOL_REFRESH}) {
        auto* button = Find(files, symbol);
        assert(button && lv_obj_get_child_count(button) != 0);
        AssertCentered(lv_obj_get_child(button, 0), button);
      }
      if (!landscape && size == InterfaceSize::kNormal) {
        lv_obj_send_event(Find(files, LV_SYMBOL_LIST), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Date"));
        assert(Find(files, "Ascending"));
        assert(Find(files, "Descending"));
        lv_obj_send_event(Find(files, "Date"), LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(Find(files, "Descending"), LV_EVENT_CLICKED, nullptr);
        Tick();
        auto* sort_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        lv_obj_send_event(sort_overlay, LV_EVENT_CLICKED, nullptr);
        Tick();
      }
      char screenshot[128];
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-files-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);

      if (!landscape && size == InterfaceSize::kLarge) {
        lv_obj_send_event(Find(files, LV_SYMBOL_OK), LV_EVENT_CLICKED, nullptr);
        Tick();
        auto* copy = Find(files, "Copy");
        assert(copy && lv_obj_get_height(copy) == 180);
        save("/tmp/aera-file-selection-portrait-large.png");
        lv_obj_send_event(Find(files, LV_SYMBOL_CLOSE), LV_EVENT_CLICKED, nullptr);
        Tick();
      }

      if (!landscape && size == InterfaceSize::kNormal) {
        auto* row = Find(files, "alpha.txt");
        lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Information"));
        assert(Find(files, "Delete"));
        save("/tmp/aera-file-actions-portrait-normal.png");
        auto* action_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        lv_obj_send_event(action_overlay, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(!Find(files, "Information"));

        row = Find(files, "alpha.txt");
        lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        Tick();
        lv_obj_send_event(Find(files, "Information"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Information"));
        save("/tmp/aera-file-information-portrait-normal.png");
        auto* information_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        lv_obj_send_event(information_overlay, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(!Find(files, "Information"));

        lv_obj_send_event(Find(files, "broken.log"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "plain\nred\ninvalid: ?("));
        assert(widgets::DismissModal(files));
        Tick();

        lv_obj_send_event(Find(files, "large.log"), LV_EVENT_CLICKED, nullptr);
        Tick();
        auto* viewer_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        assert(FindLabelContaining(viewer_overlay, "Preview truncated at 128 KB"));
        auto* large_page = FindLabelContaining(viewer_overlay, "xxxxxxxxxxxxxxxx");
        assert(large_page && lv_obj_get_height(large_page) < 8000);
        auto* next_page = Find(viewer_overlay, LV_SYMBOL_RIGHT);
        assert(next_page);
        lv_obj_send_event(next_page, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(FindLabelContaining(viewer_overlay, "2 / "));
        assert(widgets::DismissModal(files));
        Tick();

        auto* select_button = Find(files, LV_SYMBOL_OK);
        assert(select_button);
        lv_obj_send_event(select_button, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "0 selected"));
        lv_obj_send_event(Find(files, "alpha.txt"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "1 selected"));
        assert(Find(files, "Copy") && Find(files, "Cut") && Find(files, "Delete"));
        assert(lv_obj_get_height(Find(files, "Copy")) == 146);
        save("/tmp/aera-file-selection-portrait-normal.png");
        lv_obj_send_event(Find(files, "Copy"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Paste here"));
        save("/tmp/aera-file-clipboard-portrait-normal.png");
        lv_obj_send_event(Find(files, "Paste here"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Replace") && Find(files, "Keep both") && Find(files, "Skip"));
        save("/tmp/aera-file-conflict-portrait-normal.png");
        lv_obj_send_event(Find(files, "Keep both"), LV_EVENT_CLICKED, nullptr);
        Tick(200);
        auto* paste_after_work = Find(files, "Paste here");
        assert(paste_after_work);
        assert(lv_obj_has_flag(lv_obj_get_parent(paste_after_work), LV_OBJ_FLAG_HIDDEN));
        assert(Find(files, "alpha (1).txt"));
      }

      lv_obj_send_event(Find(files, LV_SYMBOL_PLUS), LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* new_text = Find(files, "New text file");
      assert(new_text);
      lv_obj_send_event(new_text, LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* name_input = FindType(files, &lv_textarea_class);
      auto* name_keyboard = FindType(files, &lv_keyboard_class);
      auto* name_cancel = Find(files, "Cancel");
      auto* name_create = Find(files, "Create");
      assert(name_input && name_keyboard && name_cancel && name_create);
      AssertVisibleCaret(name_input);
      save("/tmp/aera-file-name-debug.png");
      AssertInside(name_input, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_keyboard, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_cancel, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_create, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      assert(!Overlaps(name_input, name_keyboard, 16));
      assert(!Overlaps(name_keyboard, name_cancel, 16));
      assert(!Overlaps(name_keyboard, name_create, 16));
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-name-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);

      const std::string filename = std::string("ui-") +
          (landscape ? "landscape-" : "portrait-") + size_name + ".txt";
      lv_textarea_set_text(name_input, filename.c_str());
      if (!landscape && size == InterfaceSize::kNormal)
        lv_obj_send_event(name_keyboard, LV_EVENT_READY, nullptr);
      else
        lv_obj_send_event(name_create, LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* editor = FindType(files, &lv_textarea_class);
      auto* editor_keyboard = FindType(files, &lv_keyboard_class);
      auto* editor_cancel = Find(files, "Cancel");
      auto* editor_save = Find(files, "Save");
      assert(editor && editor_keyboard && editor_cancel && editor_save);
      AssertVisibleCaret(editor);
      AssertInside(editor, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_keyboard, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_cancel, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_save, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      assert(!Overlaps(editor, editor_keyboard, 16));
      assert(!Overlaps(editor_keyboard, editor_cancel, 16));
      assert(!Overlaps(editor_keyboard, editor_save, 16));
      lv_textarea_set_text(editor, "First line");
      lv_textarea_set_cursor_pos(editor, LV_TEXTAREA_CURSOR_LAST);
      PressKeyboardKey(editor_keyboard, LV_SYMBOL_NEW_LINE);
      assert(!strcmp(lv_textarea_get_text(editor), "First line\n"));
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-editor-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);
      lv_textarea_set_text(editor, "Created by the AERA file-manager UI check.\n");
      lv_obj_send_event(editor_save, LV_EVENT_CLICKED, nullptr);
      Tick();
      std::ifstream created(file_root + "/" + filename);
      std::string contents((std::istreambuf_iterator<char>(created)),
                           std::istreambuf_iterator<char>());
      assert(contents == "Created by the AERA file-manager UI check.\n");
      lv_screen_load(screen);
      lv_obj_delete(files);
      Tick();
    }
  }
  lv_display_set_resolution(display, 1440, 3168);
  interface_size = InterfaceSize::kLarge;
  for (const auto& language : i18n::AvailableLanguages()) {
    assert(i18n::SetLanguage(language.code));
    auto* files = lv_obj_create(nullptr);
    BuildFilesScene(files, RecordAction, nullptr);
    lv_screen_load(files);
    Tick();
    if (!language.rtl) {
      assert(Find(files, i18n::Translate("Files")));
    }
    if (!strcmp(language.code, "ar_SA"))
      save("/tmp/aera-file-language-ar_SA.png");

    auto* row = Find(files, "alpha.txt");
    assert(row);
    lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    Tick();
    if (!language.rtl) {
      assert(Find(files, i18n::Translate("Copy")));
      assert(Find(files, i18n::Translate("Information")));
      assert(Find(files, i18n::Translate("Delete")));
    }
    assert(widgets::DismissModal(files));
    Tick();

    if (language.rtl) {
      lv_screen_load(screen);
      lv_obj_delete(files);
      Tick();
      continue;
    }

    lv_obj_send_event(Find(files, LV_SYMBOL_PLUS), LV_EVENT_CLICKED, nullptr);
    Tick();
    auto* create_text = Find(files, i18n::Translate("New text file"));
    assert(create_text);
    lv_obj_send_event(create_text, LV_EVENT_CLICKED, nullptr);
    Tick();
    auto* keyboard = FindType(files, &lv_keyboard_class);
    auto* cancel = Find(files, i18n::Translate("Cancel"));
    auto* create = Find(files, i18n::Translate("Create"));
    assert(keyboard && cancel && create);
    AssertInside(keyboard, 1440, 3168);
    assert(!Overlaps(keyboard, cancel, 16));
    assert(!Overlaps(keyboard, create, 16));
    auto* cancel_label = FindLabel(cancel, i18n::Translate("Cancel"));
    auto* create_label = FindLabel(create, i18n::Translate("Create"));
    assert(cancel_label && create_label);
    AssertContained(cancel_label, cancel);
    AssertContained(create_label, create);
    if (!strcmp(language.code, "de_DE") || !strcmp(language.code, "sv_SE") ||
        !strcmp(language.code, "zh_CN") ||
        !strcmp(language.code, "ru")) {
      char screenshot[128];
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-language-%s.png",
               language.code);
      save(screenshot);
    }
    assert(widgets::DismissModal(files));
    Tick();
    lv_screen_load(screen);
    lv_obj_delete(files);
    Tick();
  }
  i18n::SetLanguage("en");
  interface_size = InterfaceSize::kNormal;

  if (!strcmp(argv[1], "--files")) {
    puts("Headless File Manager UI checks passed at all four density sizes.");
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    return 0;
  }

  auto *about=lv_obj_create(nullptr);
  BuildAboutScene(about,RecordAction,nullptr);
  lv_screen_load(about); Tick();
  assert(Find(about,"Jonas Salo · koaaN"));
  assert(Find(about,"Daniel Springer · Daniel210191"));
  assert(Find(about,"R1.0"));
  save("/tmp/aera-about-host.png");
  lv_screen_load(screen); lv_obj_delete(about);
  puts("Headless UI checks passed: file-manager long press, creation, editor and keyboard geometry in six viewport/size combinations; wipe selection, format confirmation guard, keyboard no auto-submit, bottom navigation geometry, preference toggles, backup/restore, Android users, unlock, viewer, operation completion.");
  lv_deinit();
  std::filesystem::remove_all(backup_root);
}
