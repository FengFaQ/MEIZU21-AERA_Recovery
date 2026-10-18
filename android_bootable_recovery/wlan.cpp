#include "wlan.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <regex>
#include <cutils/properties.h>

#include <iomanip>
#include <map>

#include <mutex>

#include "data.hpp"
#include "aera_adbd.hpp"
#include "aera_secrets/aera_secrets.hpp"
#include "aeraui/platform/aera_ui_host.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "aera_supplicant_link.hpp"

#ifndef LOGINFO
#define LOGINFO(...) printf(__VA_ARGS__)
#endif

#ifndef LOGERR
#define LOGERR(...) fprintf(stderr, __VA_ARGS__)
#endif

// Serializes whole high-level WLAN operations (Enable/Disable/Scan/Connect/
// ConnectSaved/Info) so the AERA Wi-Fi dispatcher and the GUI ActionThread
// can never interleave their multi-step supplicant sequences — e.g. a background
// scan rewriting "network 0" in the middle of a user-initiated connect. This is
// deliberately coarser than g_supp_mutex (which only guards a single ctrl-socket
// exchange). Recursive because Scan()/Connect()/ConnectSaved() call Enable().
static std::recursive_mutex g_wlan_op_mutex;

static const char* WLAN_TMP_DIR        = "/tmp/wlan";
static const char* WLAN_LIST_DIR       = "/tmp/wlan/list";
static const char* WLAN_LIST_FILE      = "/tmp/wlan/list.txt";
static const char* WLAN_SAVED_FILE     = "/tmp/wlan/saved.txt";
static const char* WLAN_CONNECTED_FILE = "/tmp/wlan/connected_name.txt";
static const char* WLAN_INFO_FILE      = "/tmp/wlan/info.txt";

static const char* DEFAULT_WLAN_IFACE  = "wlan0";
static const char* DEFAULT_CTRL_DIR    = "/tmp/recovery/sockets";
static const char* DEFAULT_SUPP_CONF   = "/vendor/etc/wifi/wpa_supplicant.conf";

static const char* BIN_WPA_CLI         = "/system/bin/wpa_cli";
static const char* BIN_IFCONFIG        = "/system/bin/ifconfig";
static const char* BIN_DHCPTOOL        = "/system/bin/dhcptool";

static const char* WLAN_SUPP_SERVICE   = "wpa_supplicant";
static const char* WLAN_SUPP_SVC_PROP  = "init.svc.wpa_supplicant";
static constexpr int WLAN_SUPP_READY_TIMEOUT_MS = 60000;
/* Set to "1" to fire the `on property:sys.aera.wlan.up=1` block in
 * init.recovery.wifi.rc, which brings up the QCA6490 driver on demand and then
 * `start`s the wpa_supplicant service. The bring-up used to live in a shell
 * wrapper (mondrian_wlan_up.sh); it is now native init builtins. */
static const char* WLAN_SUPP_PREP_PROP = "sys.aera.wlan.up";

static int HexDigitValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static std::string DecodeSupplicantText(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '\\' || i + 1 >= encoded.size()) {
            decoded.push_back(encoded[i]);
            continue;
        }

        const char escaped = encoded[i + 1];
        if (escaped == 'x' && i + 3 < encoded.size()) {
            const int high = HexDigitValue(encoded[i + 2]);
            const int low = HexDigitValue(encoded[i + 3]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 3;
                continue;
            }
        }

        switch (escaped) {
            case '\\': decoded.push_back('\\'); ++i; continue;
            case '"': decoded.push_back('"'); ++i; continue;
            case 'n': decoded.push_back('\n'); ++i; continue;
            case 'r': decoded.push_back('\r'); ++i; continue;
            case 't': decoded.push_back('\t'); ++i; continue;
            case 'e': decoded.push_back('\x1b'); ++i; continue;
            default: break;
        }

        if (escaped >= '0' && escaped <= '7') {
            unsigned value = 0;
            size_t end = i + 1;
            size_t digits = 0;
            while (end < encoded.size() && digits < 3 &&
                   encoded[end] >= '0' && encoded[end] <= '7') {
                value = (value << 3) | static_cast<unsigned>(encoded[end] - '0');
                ++end;
                ++digits;
            }
            decoded.push_back(static_cast<char>(value & 0xff));
            i = end - 1;
            continue;
        }

        // Preserve unknown escape sequences instead of silently changing an
        // SSID produced by a different supplicant version.
        decoded.push_back('\\');
    }
    return decoded;
}

static std::string HexEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0f]);
    }
    return encoded;
}

static void SetWlanTestResult(const std::string& title,
                              const std::string& line1,
                              const std::string& line2,
                              const std::string& line3,
                              const std::string& line4,
                              const std::string& line5)
{
    DataManager::SetValue("wlan_test_title", title);
    DataManager::SetValue("wlan_test_line1", line1);
    DataManager::SetValue("wlan_test_line2", line2);
    DataManager::SetValue("wlan_test_line3", line3);
    DataManager::SetValue("wlan_test_line4", line4);
    DataManager::SetValue("wlan_test_line5", line5);
    DataManager::SetValue("wlan_test_done", 1);
}


static bool OF_SaveEncryptedNetwork(const std::string& ssid, const std::string& password, const std::string& encryption)
{
    return AeraSecrets::SetWlanNetwork(ssid, password, encryption);
}

static bool OF_LoadEncryptedNetwork(const std::string& ssid, std::string& password, std::string& encryption)
{
    return AeraSecrets::GetWlanNetwork(ssid, password, encryption);
}

static bool OF_DeleteEncryptedNetwork(const std::string& ssid)
{
    return AeraSecrets::DeleteWlanNetwork(ssid);
}

static bool OF_GetEncryptedSavedSsids(std::vector<std::string>& ssids)
{
    return AeraSecrets::ListWlanNetworks(ssids);
}

bool Wlan::Init() {
    // Qualcomm selects the interface MAC while the WLAN module is loading.
    // Prepare its config and firmware file now, before post-decrypt modules
    // create wlan0. Runtime SIOCSIFHWADDR is rejected by newer QCA drivers.
    if (!PrepareStableMacFirmware())
        LOGERR("WLAN: stable MAC firmware was not prepared; driver fallback remains available\n");

    DataManager::SetValue("tw_wlan_enabled", 0);
    DataManager::SetValue("tw_wlan_connected", 0);
    DataManager::SetValue("wlan_connected_name", "");
    DataManager::SetValue("wlan_test_title", "Network Connection");
    DataManager::SetValue("wlan_test_line1", "");
    DataManager::SetValue("wlan_test_line2", "");
    DataManager::SetValue("wlan_test_line3", "");
    DataManager::SetValue("wlan_test_line4", "");
    DataManager::SetValue("wlan_test_line5", "");
    DataManager::SetValue("wlan_test_done", 0);
    return EnsureTmpLayout();
}

bool Wlan::Enable() {
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    EnsureTmpLayout();

    if (!StartSupplicant()) {
        DataManager::SetValue("tw_wlan_enabled", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        DataManager::SetValue("wlan_connected_name", "");
        gui_print("WLAN: failed to start supplicant service\n");
        return false;
    }

    DataManager::SetValue("tw_wlan_enabled", 1);
    UpdateConnectedName();
    return true;
}

bool Wlan::StopSupplicant() {
    // Drop the persistent ctrl/monitor sockets: the supplicant (and its socket)
    // goes away here, so the next Enable must reopen fresh connections.
    CloseSuppChannel();
    return StopInitSupplicantService();
}

bool Wlan::Disable() {
	std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
	AeraAdbd::StopAll();
	StopDhcp();
	StopSupplicant();

	/*
	 * Do not delete /tmp/wlan/saved.txt.
	 * It is our UI-side saved WLAN list for the current recovery session.
	 */
	RunCommand("rm -rf /tmp/wlan/list");
	RunCommand("rm -f /tmp/wlan/list.txt");
	RunCommand("rm -f /tmp/wlan/list_saved.txt");
	RunCommand("rm -f /tmp/wlan/list_unsaved.txt");
	RunCommand("rm -f /tmp/wlan/connected_name.txt");
	RunCommand("rm -f /tmp/wlan/info.txt");

	EnsureTmpLayout();

	DataManager::SetValue("tw_wlan_enabled", 0);
	DataManager::SetValue("tw_wlan_connected", 0);
	DataManager::SetValue("wlan_connected_name", "");
	return true;
}

bool Wlan::Disconnect() {
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    if (!IsEnabled())
        return true;

    // Keep supplicant, scan results, and saved credentials alive. Only drop
    // the current association so the user can immediately choose another AP.
    const bool result = SuppCmd("DISCONNECT");
    unlink(WLAN_CONNECTED_FILE);
    unlink("/tmp/wlan/info.txt");
    DataManager::SetValue("tw_wlan_connected", 0);
    DataManager::SetValue("wlan_connected_name", "");
    DataManager::SetValue("wlan_info_connected", "0");
    DataManager::SetValue("wlan_info_ssid", "");
    DataManager::SetValue("wlan_info_ip", "");
    DataManager::SetValue("wlan_info_text", "");
    DataManager::SetValue("wlan_connect_text", "Disconnected");
    DataManager::SetValue("wlan_connect_done", result ? 1 : 0);
    return result;
}

bool Wlan::IsEnabled() {
    return DataManager::GetIntValue("tw_wlan_enabled") == 1;
}

bool Wlan::Scan() {
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    if (!IsEnabled() && !Enable())
        return false;

    SuppCmd("SCAN");

    // Wait for the supplicant to report results instead of polling on a fixed
    // sleep. Falls back to a short poll if the monitor is unavailable.
    std::string ev;
    if (SuppWaitEvent({"CTRL-EVENT-SCAN-RESULTS"}, 8000, ev)) {
        if (BuildScanList()) {
            UpdateConnectedName();
            RefreshSaved();
            return true;
        }
    }

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);
        if (BuildScanList()) {
            UpdateConnectedName();
            RefreshSaved();
            return true;
        }
    }

    gui_print("WLAN: scan failed\n");
    return false;
}

bool Wlan::Connect() {
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    DataManager::SetValue("wlan_connect_text", "Connecting");
    DataManager::SetValue("wlan_connect_done", 0);

    if (!IsEnabled() && !Enable()) {
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid = DataManager::GetStrValue("wlanselectedid");
    std::string pass = DataManager::GetStrValue("wlan_password");

    if (ssid.empty()) {
        gui_print("WLAN: no SSID selected\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string enc;
    if (!ReadFile(NetworkMetadataPath(ssid), enc)) {
        gui_print("WLAN: missing metadata for selected SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }
    enc = Trim(enc);

    std::string key_mgmt = "NONE";
    if (enc == "WPA3") {
        key_mgmt = "SAE";
    } else if (enc == "WPA2" || enc == "WPA") {
        key_mgmt = "WPA-PSK";
    } else if (enc == "OPEN") {
        key_mgmt = "NONE";
    } else {
        key_mgmt = "WPA-PSK";
    }

    if (key_mgmt != "NONE" && pass.empty()) {
        gui_print("WLAN: password required\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Beginning WLAN connection...\n");
    gui_print("Connecting to wlan: %s\n", ssid.c_str());
    gui_print("Encryption: %s (key_mgmt: %s)\n", enc.c_str(), key_mgmt.c_str());
    gui_print(" \n");

    gui_print("Remove old network config...\n");
    std::string list_out;
    if (SuppCmd("LIST_NETWORKS", list_out)) {
        bool has_net0 = false;
        std::istringstream lss(list_out);
        std::string lline;
        bool first_line = true;

        while (std::getline(lss, lline)) {
            if (first_line) {
                first_line = false;
                continue;
            }

            lline = Trim(lline);
            if (lline.empty())
                continue;

            if (lline.rfind("0\t", 0) == 0 || lline.rfind("0 ", 0) == 0 || lline == "0") {
                has_net0 = true;
                break;
            }
        }

        if (has_net0) {
            SuppCmd("REMOVE_NETWORK 0");
        }
    }

    gui_print("Add new network config...\n");
    if (!SuppCmd("ADD_NETWORK")) {
        gui_print("WLAN: add_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add SSID to new config...\n");
    if (!SuppCmd("SET_NETWORK 0 ssid " + HexEncode(ssid))) {
        gui_print("WLAN: failed setting SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add encryption to new config...\n");
    if (!SuppCmd("SET_NETWORK 0 key_mgmt " + key_mgmt)) {
        gui_print("WLAN: failed setting key_mgmt\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    if (key_mgmt != "NONE") {
        std::string esc_pass = EscapeDoubleQuotes(pass);
        gui_print("Add password to new config...\n");

        bool pass_ok = false;
        if (key_mgmt == "SAE") {
            pass_ok = SuppCmd("SET_NETWORK 0 sae_password \"" + esc_pass + "\"");
        } else {
            pass_ok = SuppCmd("SET_NETWORK 0 psk \"" + esc_pass + "\"");
        }

        if (!pass_ok) {
            gui_print("WLAN: failed setting password\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            return false;
        }
    }

    gui_print("Enable new config for network...\n");
    if (!SuppCmd("ENABLE_NETWORK 0")) {
        gui_print("WLAN: enable_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Select new config for network...\n");
    if (!SuppCmd("SELECT_NETWORK 0")) {
        gui_print("WLAN: select_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Reconnect...\n");
    if (!SuppCmd("RECONNECT")) {
        gui_print("WLAN: reconnect failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print(" \n");
    gui_print("Connect to network with new config...\n");
    gui_print(" \n");

    // Wait for association via supplicant events instead of polling STATUS on a
    // fixed 1s cadence: each iteration blocks up to 1.5s for a relevant event,
    // then confirms the SSID via STATUS. Bounded to ~15s worst case but returns
    // the instant CTRL-EVENT-CONNECTED lands.
    bool completed = false;

    for (int tries = 0; tries < 12 && !completed; ++tries) {
        std::string ev;
        bool got_event = SuppWaitEvent({"CTRL-EVENT-CONNECTED", "CTRL-EVENT-DISCONNECTED",
                                        "CTRL-EVENT-ASSOC-REJECT", "CTRL-EVENT-AUTH-REJECT"},
                                       1500, ev);

        std::string status;
        if (SuppCmd("STATUS", status)) {
            std::string wpa_state;
            std::string current_ssid;
            std::string ip_addr;
            ParseSupplicantStatus(status, wpa_state, current_ssid, ip_addr);

            gui_print("Connection state: %s (%d/12)\n", wpa_state.c_str(), tries);

            if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                completed = true;
                break;
            }
        }

        if (!got_event)
            usleep(500 * 1000);
    }

    if (!completed) {
        gui_print(" \n");
        gui_print("WLAN: association failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        RefreshSaved();
        return false;
    }

    DataManager::SetValue("wlan_connect_text", "Getting IP-Address");
    DataManager::SetValue("wlan_connect_done", 0);

    gui_print(" \n");
    gui_print("Checking connection...\n");
    gui_print(" \n");
    gui_print("Getting dhcp ip...\n");
    gui_print(" \n");

    if (!StartDhcp()) {
        gui_print("WLAN: DHCP failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        RefreshSaved();
        return false;
    }

    std::string ip_addr;
    std::string connected_ssid;

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);

        std::string status;
        if (SuppCmd("STATUS", status)) {
            std::string wpa_state;
            ParseSupplicantStatus(status, wpa_state, connected_ssid, ip_addr);

            if (!ip_addr.empty() && !connected_ssid.empty())
                break;
        }
    }

    UpdateConnectedName();

    if (ip_addr.empty()) {
        gui_print("WLAN: no IP address assigned\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        RefreshSaved();
        return false;
    }

    gui_print("Connected SSID: %s\n", connected_ssid.c_str());
    gui_print("IP address: %s\n", ip_addr.c_str());

    std::string connected;
    ReadFile(WLAN_CONNECTED_FILE, connected);

    if (Trim(connected).empty()) {
        gui_print("WLAN: connected name not updated\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        RefreshSaved();
        return false;
    }

    /*
     * UI-side saved WLAN handling.
     * This does not depend on wpa_supplicant.conf.
     * If the connection worked, add the SSID to /tmp/wlan/saved.txt.
     */
    EnsureTmpLayout();

    std::string saved;
    ReadFile(WLAN_SAVED_FILE, saved);

    bool already_saved = false;
    std::istringstream iss_saved(saved);
    std::string saved_line;

    while (std::getline(iss_saved, saved_line)) {
        if (!saved_line.empty() && saved_line.back() == '\r') saved_line.pop_back();
        if (saved_line == ssid) {
            already_saved = true;
            break;
        }
    }

    if (!already_saved) {
        if (!saved.empty() && saved[saved.size() - 1] != '\n')
            saved += "\n";

        saved += ssid + "\n";

        if (WriteFile(WLAN_SAVED_FILE, saved)) {
            gui_print("WLAN: saved network: %s\n", ssid.c_str());
        } else {
            gui_print("WLAN: failed to write saved network file\n");
        }
    } else {
        gui_print("WLAN: network already saved: %s\n", ssid.c_str());
    }

    if (OF_SaveEncryptedNetwork(ssid, pass, enc)) {
        gui_print("WLAN: encrypted credentials saved for: %s\n", ssid.c_str());
    } else {
        gui_print("WLAN: failed to save encrypted credentials for: %s\n", ssid.c_str());
    }

    RefreshSaved();

    std::string saved_debug;
    if (ReadFile(WLAN_SAVED_FILE, saved_debug)) {
        gui_print("WLAN: saved.txt after refresh:\n%s", saved_debug.c_str());
    } else {
        gui_print("WLAN: saved.txt missing after refresh\n");
    }

    DataManager::SetValue("tw_wlan_connected", 1);
    DataManager::SetValue("wlan_connected_name", ssid);
    // Remember the last network we connected to so "connect automatically" can
    // re-join it on the next boot (persisted via the of_* variable).
    DataManager::SetValue("of_wlan_last_ssid", ssid);

    /*
     * Show the check icon and "Connected" text in the overlay before it closes.
     */
    DataManager::SetValue("wlan_connect_text", "Connected");
    DataManager::SetValue("wlan_connect_done", 1);
    usleep(1000 * 1000);

    gui_print("Wlan connect successfully!\n");
    return true;
}

bool Wlan::ConnectSaved() {
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    DataManager::SetValue("wlan_connect_text", "Connecting");
    DataManager::SetValue("wlan_connect_done", 0);

    if (!IsEnabled() && !Enable()) {
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid = DataManager::GetStrValue("wlanselectedid");

    if (ssid.empty()) {
        gui_print("WLAN: no saved SSID selected\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string saved_pass;
    std::string saved_enc;

    if (!OF_LoadEncryptedNetwork(ssid, saved_pass, saved_enc)) {
        gui_print("WLAN: no encrypted saved credentials for: %s\n", ssid.c_str());
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Beginning saved WLAN connection...\n");
    gui_print("Connecting to saved wlan: %s\n", ssid.c_str());
    gui_print("Saved encryption: %s\n", saved_enc.c_str());
    gui_print(" \n");

    std::string key_mgmt = "NONE";

    if (saved_enc == "WPA3") {
        key_mgmt = "SAE";
    } else if (saved_enc == "WPA2" || saved_enc == "WPA") {
        key_mgmt = "WPA-PSK";
    } else if (saved_enc == "OPEN") {
        key_mgmt = "NONE";
    } else {
        key_mgmt = "WPA-PSK";
    }

    gui_print("Disconnect current WLAN state...\n");
    SuppCmd("DISCONNECT");
    usleep(500 * 1000);

    gui_print("Remove old network config...\n");
    std::string list_out;

    if (SuppCmd("LIST_NETWORKS", list_out)) {
        std::istringstream lss(list_out);
        std::string lline;
        bool first_line = true;

        while (std::getline(lss, lline)) {
            if (first_line) {
                first_line = false;
                continue;
            }

            lline = Trim(lline);
            if (lline.empty())
                continue;

            std::vector<std::string> cols;
            std::stringstream ls(lline);
            std::string col;

            while (std::getline(ls, col, '\t')) {
                cols.push_back(col);
            }

            if (cols.size() >= 1) {
                std::string id = Trim(cols[0]);
                if (!id.empty()) {
                    SuppCmd("REMOVE_NETWORK " + id);
                }
            }
        }
    }

    gui_print("Add saved network config...\n");
    if (!SuppCmd("ADD_NETWORK")) {
        gui_print("WLAN: add_network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add saved SSID to config...\n");
    if (!SuppCmd("SET_NETWORK 0 ssid " + HexEncode(ssid))) {
        gui_print("WLAN: failed setting saved SSID\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Add saved encryption to config...\n");
    if (!SuppCmd("SET_NETWORK 0 key_mgmt " + key_mgmt)) {
        gui_print("WLAN: failed setting saved key_mgmt\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    if (key_mgmt != "NONE") {
        std::string esc_pass = EscapeDoubleQuotes(saved_pass);

        gui_print("Add saved password to config...\n");

        bool pass_ok = false;
        if (key_mgmt == "SAE") {
            pass_ok = SuppCmd("SET_NETWORK 0 sae_password \"" + esc_pass + "\"");
        } else {
            pass_ok = SuppCmd("SET_NETWORK 0 psk \"" + esc_pass + "\"");
        }

        if (!pass_ok) {
            gui_print("WLAN: failed setting saved password\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            return false;
        }
    }

    gui_print("Enable saved config...\n");
    if (!SuppCmd("ENABLE_NETWORK 0")) {
        gui_print("WLAN: enable saved network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Select saved config...\n");
    if (!SuppCmd("SELECT_NETWORK 0")) {
        gui_print("WLAN: select saved network failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    gui_print("Reconnect saved network...\n");
    if (!SuppCmd("RECONNECT")) {
        gui_print("WLAN: saved reconnect failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    int tries = 0;
    const int max_tries = 10;
    bool completed = false;
    bool saw_transition = false;

    while (tries < max_tries) {
        std::string ev;
        bool got_event = SuppWaitEvent({"CTRL-EVENT-CONNECTED", "CTRL-EVENT-DISCONNECTED",
                                        "CTRL-EVENT-ASSOC-REJECT", "CTRL-EVENT-AUTH-REJECT"},
                                       2000, ev);
        if (!got_event)
            usleep(1000 * 1000);

        std::string status;
        if (SuppCmd("STATUS", status)) {
            std::string wpa_state;
            std::string current_ssid;
            std::string ip_addr;
            ParseSupplicantStatus(status, wpa_state, current_ssid, ip_addr);

            gui_print("Saved connection state: %s (%d/%d)\n", wpa_state.c_str(), tries, max_tries);

            if (wpa_state != "COMPLETED")
                saw_transition = true;

            if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                completed = true;
                break;
            }
        }

        tries++;
    }

    if (!completed) {
        gui_print("WLAN: saved association failed\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        return false;
    }

    if (!saw_transition) {
        gui_print("WLAN: saved association was already completed; forcing reassociation before DHCP\n");
        SuppCmd("REASSOCIATE");

        completed = false;

        for (int i = 0; i < max_tries; ++i) {
            usleep(1000 * 1000);

            std::string status;
            if (SuppCmd("STATUS", status)) {
                std::string wpa_state;
                std::string current_ssid;
                std::string ip_addr;
                ParseSupplicantStatus(status, wpa_state, current_ssid, ip_addr);

                gui_print("Saved reassociation state: %s (%d/%d)\n", wpa_state.c_str(), i, max_tries);

                if (wpa_state == "COMPLETED" && current_ssid == ssid) {
                    completed = true;
                    break;
                }
            }
        }

        if (!completed) {
            gui_print("WLAN: saved reassociation failed\n");
            DataManager::SetValue("wlan_connect_text", "Failed");
            DataManager::SetValue("wlan_connect_done", 0);
            DataManager::SetValue("tw_wlan_connected", 0);
            UpdateConnectedName();
            return false;
        }
    }

    DataManager::SetValue("wlan_connect_text", "Getting IP-Address");
    DataManager::SetValue("wlan_connect_done", 0);

    gui_print("Getting dhcp ip for saved network...\n");

    if (!StartDhcp()) {
        gui_print("WLAN: DHCP failed on saved network\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        UpdateConnectedName();
        return false;
    }

    std::string ip_addr;
    std::string connected_ssid;

    for (int i = 0; i < 5; ++i) {
        usleep(1000 * 1000);

        std::string status;
        if (SuppCmd("STATUS", status)) {
            std::string wpa_state;
            ParseSupplicantStatus(status, wpa_state, connected_ssid, ip_addr);

            if (!ip_addr.empty() && !connected_ssid.empty())
                break;
        }
    }

    UpdateConnectedName();
    RefreshSaved();

    if (ip_addr.empty()) {
        gui_print("WLAN: no IP address assigned on saved network\n");
        DataManager::SetValue("wlan_connect_text", "Failed");
        DataManager::SetValue("wlan_connect_done", 0);
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    DataManager::SetValue("tw_wlan_connected", 1);
    DataManager::SetValue("wlan_connected_name", ssid);
    DataManager::SetValue("of_wlan_last_ssid", ssid);

    /*
     * Show the check icon and "Connected" text in the overlay before it closes.
     */
    DataManager::SetValue("wlan_connect_text", "Connected");
    DataManager::SetValue("wlan_connect_done", 1);
    usleep(1000 * 1000);

    gui_print("Connected saved SSID: %s\n", connected_ssid.c_str());
    gui_print("IP address: %s\n", ip_addr.c_str());
    gui_print("WLAN: saved network connected successfully!\n");

    return true;
}

bool Wlan::Info()
{
    std::lock_guard<std::recursive_mutex> op(g_wlan_op_mutex);
    return InfoLocked();
}

bool Wlan::RefreshInfoIfIdle()
{
    // Background poll: never block a foreground scan/connect. If another op holds
    // the operation lock, skip this refresh — the next idle tick will retry.
    std::unique_lock<std::recursive_mutex> op(g_wlan_op_mutex, std::try_to_lock);
    if (!op.owns_lock())
        return false;
    return InfoLocked();
}

bool Wlan::InfoLocked()
{
    /*
     * Do not clear cached UI state before checking status.
     * A temporary control-interface failure would otherwise remove the accent
     * row and connected info even if WLAN is still connected.
     */
    std::string status;
    if (!SuppCmd("STATUS", status)) {
        gui_print("WLAN: failed to read status for info, keeping cached WLAN info\n");
        return true;
    }

    std::string wpa_state;
    std::string ssid;
    std::string ip_addr;
    ParseSupplicantStatus(status, wpa_state, ssid, ip_addr);

    DataManager::SetValue("wlan_info_state", wpa_state);

    if (wpa_state == "COMPLETED" && !ssid.empty() && !ip_addr.empty()) {
        // The WLAN worker refreshes RSSI; status-bar rendering only reads it.
        std::string signal;
        int rssi = -999;
        if (SuppCmd("SIGNAL_POLL", signal)) {
            std::istringstream lines(signal);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.compare(0, 5, "RSSI=") != 0) continue;
                const std::string value = Trim(line.substr(5));
                char *end = nullptr;
                errno = 0;
                const long parsed = strtol(value.c_str(), &end, 10);
                if (errno == 0 && end != value.c_str() && *end == '\0' && parsed >= -127 && parsed < 0)
                    rssi = static_cast<int>(parsed);
                break;
            }
        }
        DataManager::SetValue("aera_wlan_rssi", rssi);
        DataManager::SetValue("wlan_info_connected", "1");
        DataManager::SetValue("wlan_info_ssid", ssid);
        DataManager::SetValue("wlan_info_ip", ip_addr);

        DataManager::SetValue("tw_wlan_connected", 1);
        DataManager::SetValue("wlan_connected_name", ssid);
        DataManager::SetValue("wlan_info_text", "Connected: " + ssid + "  IP: " + ip_addr);

        return true;
    }

    /*
     * Only clear connected UI state after a valid status response that says
     * WLAN is not actually completed/connected.
     */
    DataManager::SetValue("aera_wlan_rssi", -999);
    DataManager::SetValue("wlan_info_connected", "0");
    DataManager::SetValue("wlan_info_ssid", "");
    DataManager::SetValue("wlan_info_ip", "");
    DataManager::SetValue("wlan_info_text", "");

    DataManager::SetValue("tw_wlan_connected", 0);
    DataManager::SetValue("wlan_connected_name", "");

    return true;
}

bool Wlan::TestConnection()
{
    DataManager::SetValue("wlan_test_title", "Network Connection");
    DataManager::SetValue("wlan_test_line1", "Testing network connection...");
    DataManager::SetValue("wlan_test_line2", "");
    DataManager::SetValue("wlan_test_line3", "");
    DataManager::SetValue("wlan_test_line4", "");
    DataManager::SetValue("wlan_test_line5", "");
    DataManager::SetValue("wlan_test_done", 0);

    Info();

    std::string ssid = Trim(DataManager::GetStrValue("wlan_info_ssid"));
    std::string ip_addr = Trim(DataManager::GetStrValue("wlan_info_ip"));

    if (DataManager::GetIntValue("tw_wlan_connected") != 1 || ssid.empty() || ip_addr.empty()) {
        SetWlanTestResult(
            "Network Connection",
            "WLAN is not connected.",
            "Connect to a WLAN network first.",
            "",
            "",
            "");
        return true;
    }

    std::string busybox = GetBusyboxBinary();
    if (busybox.empty())
        busybox = "busybox";

    std::string internet_output;
    bool internet_ok = RunCommand(busybox + " ping -c 1 -w 4 1.1.1.1 2>&1", internet_output);

    std::string dns_output;
    bool dns_ok = RunCommand(busybox + " ping -c 1 -w 4 bing.com 2>&1", dns_output);

    LOGINFO("WLAN test internet output:\n%s\n", internet_output.c_str());
    LOGINFO("WLAN test DNS output:\n%s\n", dns_output.c_str());

    if (internet_ok && dns_ok) {
        SetWlanTestResult(
            "Network Connection",
            "Connected: " + ssid,
            "IP address: " + ip_addr,
            "Internet: OK",
            "DNS: OK",
            "");
        return true;
    }

    if (internet_ok) {
        SetWlanTestResult(
            "Network Connection",
            "Connected: " + ssid,
            "IP address: " + ip_addr,
            "Internet: OK",
            "DNS: Failed",
            "Name lookup may be broken.");
        return true;
    }

    SetWlanTestResult(
        "Network Connection",
        "Connected: " + ssid,
        "IP address: " + ip_addr,
        "Internet: Failed",
        "DNS: " + std::string(dns_ok ? "OK" : "Failed"),
        "Router or upstream network may be down.");
    return true;
}


bool Wlan::RefreshSaved() {
    return BuildSavedList();
}

bool Wlan::ForgetSaved()
{
    std::string ssid = DataManager::GetStrValue("wlanselectedid");

    if (ssid.empty()) {
        gui_print("WLAN: no saved network selected for delete\n");
        return false;
    }

    gui_print("WLAN: removing saved network: %s\n", ssid.c_str());

    /*
     * Remove from the plain runtime saved list.
     */
    std::string saved;
    std::ostringstream out;

    if (ReadFile(WLAN_SAVED_FILE, saved)) {
        std::istringstream iss(saved);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string saved_ssid = line;

            if (saved_ssid.empty())
                continue;

            if (saved_ssid == ssid)
                continue;

            out << saved_ssid << "\n";
        }

        std::string new_saved = out.str();

        if (new_saved.empty()) {
            unlink(WLAN_SAVED_FILE);
        } else {
            WriteFile(WLAN_SAVED_FILE, new_saved);
        }
    }

    /*
     * Remove encrypted credentials too.
     */
    if (OF_DeleteEncryptedNetwork(ssid)) {
        gui_print("WLAN: encrypted credentials removed for: %s\n", ssid.c_str());
    } else {
        gui_print("WLAN: failed removing encrypted credentials for: %s\n", ssid.c_str());
    }

    DataManager::SetValue("wlanselectedid", "");

    RefreshSaved();

    return true;
}

bool Wlan::UpdateConnectedName() {
    return BuildConnectedName();
}

bool Wlan::EnsureTmpLayout() {
    if (!MkdirRecursive(WLAN_TMP_DIR)) return false;
    if (!MkdirRecursive(WLAN_LIST_DIR)) return false;
    return true;
}

bool Wlan::EnsureSupplicantConf() {
    // Path for vendor config
    const std::string conf = "/vendor/etc/wifi/wpa_supplicant.conf";

    if (FileExists(conf)) {
        return true;
    }

    // If the file doesn't exist, try to create a basic one if needed
    std::ostringstream ss;
    ss << "ctrl_interface=/data/misc/wifi/sockets\n";
    ss << "update_config=1\n";
    ss << "country=US\n";

    return WriteFile(conf, ss.str());
}

bool Wlan::StartSupplicant() {
    EnsureSupplicantConf();
    MkdirRecursive(GetCtrlDir());
    return StartInitSupplicantService();
}

bool Wlan::PrepareStableMacFirmware() {
    char serial_prop[PROPERTY_VALUE_MAX] = {0};
    property_get("ro.serialno", serial_prop, "");
    std::string serial = serial_prop;
    if (serial.empty()) {
        property_get("ro.boot.serialno", serial_prop, "");
        serial = serial_prop;
    }

    if (serial.empty()) {
        LOGERR("WLAN: cannot derive stable MAC: device serial is empty\n");
        return false;
    }

    /*
     * Qualcomm CNSS normally obtains the factory WLAN MAC from DMS. Recovery
     * does not run that vendor service, so the driver falls back to a
     * different default MAC on every boot. Derive a deterministic,
     * recovery-only address from the device serial instead. FNV-1a is enough
     * here because this is a stable identifier, not a cryptographic secret.
     * AERA uses its own namespace so the address remains stable across AERA
     * builds without sharing identity material with another recovery.
     */
    uint64_t hash = UINT64_C(1469598103934665603);
    const std::string material = "AERA-WLAN-v1:" + serial;
    for (unsigned char c : material) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }

    unsigned char mac[6];
    mac[0] = 0x02;  // Locally administered, unicast.
    for (int i = 1; i < 6; ++i)
        mac[i] = static_cast<unsigned char>(hash >> ((i - 1) * 8));

    std::ostringstream compact_address;
    compact_address << std::uppercase << std::hex << std::setfill('0');
    for (int i = 0; i < 6; ++i)
        compact_address << std::setw(2) << static_cast<unsigned int>(mac[i]);

    const std::string mac_file =
        "Intf0MacAddress=" + compact_address.str() + "\nEND\n";
    const std::vector<std::string> config_roots = {
        "/vendor/etc/wifi",
        "/odm/etc/wifi",
        "/vendor/odm/etc/wifi",
        "/vendor/firmware/wlan/qca_cld",
        "/odm/firmware/wlan/qca_cld",
        "/system/etc/firmware/wlan/qca_cld",
    };

    std::set<std::string> config_paths;
    std::set<std::string> chip_names;
    for (const std::string& root : config_roots) {
        const std::string direct = root + "/WCNSS_qcom_cfg.ini";
        if (FileExists(direct))
            config_paths.insert(direct);

        DIR* dir = opendir(root.c_str());
        if (dir == nullptr)
            continue;

        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;

            const std::string candidate =
                root + "/" + name + "/WCNSS_qcom_cfg.ini";
            if (FileExists(candidate)) {
                config_paths.insert(candidate);
                chip_names.insert(name);
            }
        }
        closedir(dir);
    }

    const std::vector<std::string> legacy_configs = {
        "/system/etc/firmware/wlan/WCNSS_qcom_cfg.ini",
        "/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
        "/vendor/odm/etc/wifi/WCNSS_qcom_cfg.ini",
    };
    for (const std::string& path : legacy_configs) {
        if (FileExists(path))
            config_paths.insert(path);
    }

    const std::string firmware_image_root = "/vendor/firmware_mnt/image";
    DIR* firmware_dirs = opendir(firmware_image_root.c_str());
    if (firmware_dirs != nullptr) {
        while (dirent* entry = readdir(firmware_dirs)) {
            const std::string chip = entry->d_name;
            if (chip == "." || chip == "..")
                continue;

            const std::string candidate = firmware_image_root + "/" + chip +
                                          "/wlan/WCNSS_qcom_cfg.ini";
            if (FileExists(candidate)) {
                config_paths.insert(candidate);
                chip_names.insert(chip);
            }
        }
        closedir(firmware_dirs);
    }

    bool configured = false;
    const std::string setting = "read_mac_addr_from_mac_file";
    for (const std::string& path : config_paths) {
        std::string config;
        if (!ReadFile(path, config))
            continue;

        std::istringstream input(config);
        std::ostringstream output;
        std::string line;
        bool inserted_setting = false;
        while (std::getline(input, line)) {
            const std::string trimmed = Trim(line);
            if (trimmed.compare(0, setting.size(), setting) == 0)
                continue;
            if (!inserted_setting && trimmed == "END") {
                output << setting << "=1\n";
                inserted_setting = true;
            }
            output << line << '\n';
        }
        if (!inserted_setting)
            output << setting << "=1\n";

        if (WriteFile(path, output.str())) {
            configured = true;
        } else {
            LOGERR("WLAN: cannot enable MAC file in %s\n", path.c_str());
        }
    }

    std::set<std::string> mac_paths = {
        "/vendor/firmware/wlan/qca_cld/wlan_mac.bin",
        "/system/etc/firmware/wlan/qca_cld/wlan_mac.bin",
    };
    for (const std::string& chip : chip_names) {
        mac_paths.insert("/vendor/firmware/wlan/qca_cld/" + chip + "/wlan_mac.bin");
        mac_paths.insert("/system/etc/firmware/wlan/qca_cld/" + chip + "/wlan_mac.bin");
    }

    bool firmware_written = false;
    for (const std::string& path : mac_paths) {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || !MkdirRecursive(path.substr(0, slash)))
            continue;
        if (WriteFile(path, mac_file))
            firmware_written = true;
    }

    if (!configured || !firmware_written)
        return false;

    LOGINFO("WLAN: prepared deterministic MAC %s for %zu Qualcomm profile(s)\n",
            compact_address.str().c_str(), chip_names.size());
    return true;
}

bool Wlan::StartDhcp() {
    const std::string iface = GetIface();
    const std::string dhcptool = "/system/bin/dhcptool";

    if (!FileExists(dhcptool)) {
        gui_print("WLAN: dhcptool binary not found\n");
        return false;
    }

    if (!RunCommand(dhcptool + " " + iface + " >/tmp/dhcptool.log 2>&1"))
        return false;

    /*
     * dhcptool publishes DNS servers as Android properties, but recovery has
     * no netd/resolver service to turn them into /etc/resolv.conf. BusyBox,
     * musl and other recovery tools therefore have an IP route while every
     * hostname lookup fails with EAI_SYSTEM. Install the lease DNS directly;
     * /etc is a ramdisk symlink to /system/etc, so this remains session-only.
     */
    std::vector<std::string> servers;
    const std::string prefix = "net." + iface + ".dns";
    for (int index = 1; index <= 4; ++index) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get((prefix + std::to_string(index)).c_str(), value, "");
        in_addr address4 {};
        in6_addr address6 {};
        if (inet_pton(AF_INET, value, &address4) != 1 &&
            inet_pton(AF_INET6, value, &address6) != 1)
            continue;
        bool duplicate = false;
        for (const auto& server : servers)
            duplicate = duplicate || server == value;
        if (!duplicate)
            servers.emplace_back(value);
    }
    for (const char* fallback : {"1.1.1.1", "8.8.8.8"}) {
        bool duplicate = false;
        for (const auto& server : servers)
            duplicate = duplicate || server == fallback;
        if (!duplicate)
            servers.emplace_back(fallback);
    }

    std::ostringstream resolver;
    for (const auto& server : servers)
        resolver << "nameserver " << server << "\n";
    if (!WriteFile("/system/etc/resolv.conf", resolver.str())) {
        gui_print("WLAN: DHCP succeeded, but resolver configuration failed\n");
        return false;
    }
    if (!servers.empty()) {
        property_set("net.dns1", servers[0].c_str());
        if (servers.size() > 1)
            property_set("net.dns2", servers[1].c_str());
    }
    return true;
}

bool Wlan::StopDhcp() {
    return true;
}

bool Wlan::BuildScanList() {
    RunCommand("mkdir -p /tmp/wlan/list");
    RunCommand("rm -rf /tmp/wlan/list/*");

    std::string output;
    if (!SuppCmd("SCAN_RESULTS", output)) {
        gui_print("WLAN: failed to get scan_results\n");
        return false;
    }

    std::istringstream iss(output);
    std::string line;
    bool first_line = true;
    std::vector<std::pair<std::string, std::string> > networks;

    while (std::getline(iss, line)) {
        if (first_line) {
            first_line = false;
            continue;
        }

        if (Trim(line).empty())
            continue;

        std::vector<std::string> cols;
        std::stringstream ls(line);
        std::string col;
        while (std::getline(ls, col, '\t')) {
            cols.push_back(col);
        }

        if (cols.size() < 5)
            continue;

        std::string flags = cols[3];
        std::string encoded_ssid = cols[4];
        if (!encoded_ssid.empty() && encoded_ssid.back() == '\r')
            encoded_ssid.pop_back();
        std::string ssid = DecodeSupplicantText(encoded_ssid);

        if (ssid.empty() || ssid.find('\0') != std::string::npos ||
            ssid.find('\n') != std::string::npos ||
            ssid.find('\r') != std::string::npos)
            continue;

        std::string encryption = "OPEN";
        if (flags.find("SAE") != std::string::npos) {
            encryption = "WPA3";
        } else if (flags.find("WPA2") != std::string::npos || flags.find("RSN") != std::string::npos) {
            encryption = "WPA2";
        } else if (flags.find("WPA-PSK") != std::string::npos || flags.find("WPA") != std::string::npos) {
            encryption = "WPA";
        }

        bool exists = false;
        for (size_t i = 0; i < networks.size(); ++i) {
            if (networks[i].first == ssid) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            networks.push_back(std::make_pair(ssid, encryption));
        }
    }

    if (networks.empty()) {
        unlink(WLAN_LIST_FILE);
        return false;
    }

    std::ostringstream list;
    for (size_t i = 0; i < networks.size(); ++i) {
        list << networks[i].first << "\n";
        WriteFile(NetworkMetadataPath(networks[i].first), networks[i].second);
    }

    return WriteFile(WLAN_LIST_FILE, list.str());
}

bool Wlan::BuildSavedList() {
    EnsureTmpLayout();

    std::vector<std::string> unique;

    /*
     * First read encrypted persistent saved networks.
     */
    std::vector<std::string> encrypted_ssids;
    if (OF_GetEncryptedSavedSsids(encrypted_ssids)) {
        for (size_t i = 0; i < encrypted_ssids.size(); ++i) {
            if (encrypted_ssids[i].empty())
                continue;

            bool duplicate = false;
            for (size_t j = 0; j < unique.size(); ++j) {
                if (unique[j] == encrypted_ssids[i]) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                unique.push_back(encrypted_ssids[i]);
        }
    }

    /*
     * Also merge current-session /tmp/wlan/saved.txt.
     */
    std::string saved;
    if (ReadFile(WLAN_SAVED_FILE, saved)) {
        std::istringstream iss(saved);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string ssid = line;

            if (ssid.empty())
                continue;

            bool duplicate = false;
            for (size_t i = 0; i < unique.size(); ++i) {
                if (unique[i] == ssid) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                unique.push_back(ssid);
        }
    }

    if (unique.empty()) {
        unlink(WLAN_SAVED_FILE);
        return false;
    }

    std::ostringstream out;
    for (size_t i = 0; i < unique.size(); ++i)
        out << unique[i] << "\n";

    return WriteFile(WLAN_SAVED_FILE, out.str());
}

bool Wlan::BuildConnectedName() {
    std::string status;

    if (!SuppCmd("STATUS", status)) {
        unlink(WLAN_CONNECTED_FILE);
        DataManager::SetValue("wlan_connected_name", "");
        DataManager::SetValue("tw_wlan_connected", 0);
        return false;
    }

    std::string ssid;
    std::string wpa_state;
    std::string ip_addr;
    ParseSupplicantStatus(status, wpa_state, ssid, ip_addr);

    if (wpa_state == "COMPLETED" && !ssid.empty() && !ip_addr.empty()) {
        WriteFile(WLAN_CONNECTED_FILE, ssid + "\n");
        DataManager::SetValue("wlan_connected_name", ssid);
        DataManager::SetValue("tw_wlan_connected", 1);
        return true;
    }

    unlink(WLAN_CONNECTED_FILE);
    DataManager::SetValue("wlan_connected_name", "");
    DataManager::SetValue("tw_wlan_connected", 0);
    return false;
}

bool Wlan::ParseSupplicantStatus(const std::string& status, std::string& wpa_state, std::string& ssid, std::string& ip_addr) {
    wpa_state.clear();
    ssid.clear();
    ip_addr.clear();

    std::istringstream iss(status);
    std::string line;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("wpa_state=", 0) == 0)
            wpa_state = Trim(line.substr(10));
        else if (line.rfind("ssid=", 0) == 0)
            ssid = DecodeSupplicantText(line.substr(5));
        else if (line.rfind("ip_address=", 0) == 0)
            ip_addr = Trim(line.substr(11));
    }

    return !wpa_state.empty();
}

std::string Wlan::NetworkMetadataPath(const std::string& ssid) {
    return std::string(WLAN_LIST_DIR) + "/ssid-" + HexEncode(ssid);
}

bool Wlan::RunCommand(const std::string& cmd) {
    LOGINFO("WLAN CMD: %s\n", cmd.c_str());
    return TWFunc::Exec_Cmd(cmd) == 0;
}

bool Wlan::RunCommand(const std::string& cmd, std::string& output) {
    LOGINFO("WLAN CMD: %s\n", cmd.c_str());

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        LOGERR("popen failed for %s: %s\n", cmd.c_str(), strerror(errno));
        return false;
    }

    char buf[512];
    output.clear();

    while (fgets(buf, sizeof(buf), fp) != NULL)
        output += buf;

    int rc = pclose(fp);
    return rc == 0;
}

// ---------------------------------------------------------------------------
// wpa_supplicant control-interface transport (replaces fork+exec wpa_cli).
//
// One persistent command connection + one attached monitor connection, shared
// between the GUI ActionThread (scan/connect) and the AERA dispatcher
// (status). All access is serialized by g_supp_mutex.
// ---------------------------------------------------------------------------
namespace {
AeraSupplicantLink g_supp_cmd;  // command/reply channel
AeraSupplicantLink g_supp_mon;  // attached event channel
std::mutex g_supp_mutex;

// Single-quote a string for safe inclusion as ONE shell argument. The whole
// ctrl command is passed to wpa_cli as a single argument, which wpa_cli then
// forwards verbatim to the socket — so embedded double quotes survive.
std::string ShellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}
}  // namespace

bool Wlan::EnsureSuppChannel() {
    // Caller must hold g_supp_mutex.
    const std::string path = GetCtrlDir() + "/" + GetIface();
    if (!g_supp_cmd.Connected()) {
        if (!g_supp_cmd.Connect(path))
            return false;
    }
    if (!g_supp_mon.Connected()) {
        if (g_supp_mon.Connect(path)) {
            if (!g_supp_mon.Subscribe())
                g_supp_mon.Reset();  // commands still work without event delivery
        }
    }
    return true;
}

void Wlan::CloseSuppChannel() {
    std::lock_guard<std::mutex> lk(g_supp_mutex);
    g_supp_cmd.Reset();
    g_supp_mon.Reset();
}

void Wlan::RefreshWlanPageIfShown() {
    // AERA UI reads Wi-Fi state directly from the backend. Updating the
    // DataManager values and scan result file is enough; no page framework is
    // involved and worker threads never touch the renderer.
}

bool Wlan::SuppCmd(const std::string& ctrl_cmd, std::string& out) {
    std::lock_guard<std::mutex> lk(g_supp_mutex);

    if (EnsureSuppChannel() && g_supp_cmd.Execute(ctrl_cmd, &out))
        return true;

    // Fallback: the control socket is unavailable — shell out to wpa_cli,
    // passing the ctrl command as one shell-quoted argument so wpa_cli forwards
    // it unchanged. Drop the stale connection so we retry opening it next time.
    g_supp_cmd.Reset();
    g_supp_mon.Reset();

    const std::string wpacli = GetWpaCliBinary();
    if (wpacli.empty())
        return false;

    const std::string cmd = wpacli + " -i " + GetIface() + " -p " + GetCtrlDir() +
                            " " + ShellSingleQuote(ctrl_cmd);
    return RunCommand(cmd, out);
}

bool Wlan::SuppCmd(const std::string& ctrl_cmd) {
    std::string out;
    return SuppCmd(ctrl_cmd, out);
}

bool Wlan::SuppWaitEvent(const std::vector<std::string>& any_of, int timeout_ms,
                         std::string& matched) {
    std::lock_guard<std::mutex> lk(g_supp_mutex);
    if (!EnsureSuppChannel() || !g_supp_mon.Connected())
        return false;
    return g_supp_mon.AwaitAny(any_of, timeout_ms, &matched);
}

bool Wlan::WriteFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    return true;
}

bool Wlan::ReadFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return false;

    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

bool Wlan::FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool Wlan::MkdirRecursive(const std::string& path) {
    if (path.empty())
        return false;
    if (FileExists(path))
        return true;

    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' && current != "/")
            mkdir(current.c_str(), 0755);
    }

    mkdir(path.c_str(), 0755);
    return FileExists(path);
}

std::string Wlan::Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        ++start;

    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1]))
        --end;

    return s.substr(start, end - start);
}

std::string Wlan::GetIface() {
    std::string iface = DataManager::GetStrValue("tw_wlan_iface");
    return iface.empty() ? DEFAULT_WLAN_IFACE : iface;
}

std::string Wlan::GetCtrlDir() {
    std::string ctrl = DataManager::GetStrValue("tw_wlan_ctrl_dir");
    return ctrl.empty() ? DEFAULT_CTRL_DIR : ctrl;
}

std::string Wlan::GetSupplicantConf() {
    std::string conf = DataManager::GetStrValue("tw_wlan_conf");
    return conf.empty() ? DEFAULT_SUPP_CONF : conf;
}

std::string Wlan::EscapeDoubleQuotes(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"' || s[i] == '\\') {
            out += '\\';
            out += s[i];
        }
        else
            out += s[i];
    }
    return out;
}

std::string Wlan::GetWpaCliBinary() {
    return FindBinary({
        "/system/bin/wpa_cli",
        "/vendor/bin/wpa_cli",
        "/vendor/bin/hw/wpa_cli",
        "/system_ext/bin/wpa_cli",
        "/sbin/wpa_cli"
    });
}

std::string Wlan::GetBusyboxBinary() {
    return FindBinary({
        "/system/bin/busybox",
        "/sbin/busybox",
        "/vendor/bin/busybox",
        "/system/xbin/busybox",
        "/bin/busybox"
    });
}

std::string Wlan::FindBinary(const std::vector<std::string>& paths) {
    for (size_t i = 0; i < paths.size(); ++i) {
        if (FileExists(paths[i])) {
            return paths[i];
        }
    }
    return "";
}

std::string Wlan::GetIfconfigBinary() {
    return FindBinary({
        "/system/bin/ifconfig",
        "/vendor/bin/ifconfig",
        "/system_ext/bin/ifconfig",
        "/sbin/ifconfig",
        "/bin/ifconfig"
    });
}

bool Wlan::WaitForProperty(const std::string& key, const std::string& expected, int timeout_ms) {
    const int step_ms = 100;
    int waited = 0;
    char value[PROPERTY_VALUE_MAX] = {0};

    while (waited < timeout_ms) {
        property_get(key.c_str(), value, "");
        if (expected == value)
            return true;
        usleep(step_ms * 1000);
        waited += step_ms;
    }
    return false;
}

bool Wlan::WaitForSupplicantReady(int timeout_ms) {
    const int step_ms = 100;
    int waited = 0;
    const std::string socket = GetCtrlDir() + "/" + GetIface();

    while (waited < timeout_ms) {
        if (FileExists(socket)) {
            std::string reply;
            if (SuppCmd("PING", reply) && Trim(reply) == "PONG") {
                LOGINFO("WLAN: supplicant control interface is ready\n");
                return true;
            }
        }
        usleep(step_ms * 1000);
        waited += step_ms;
    }

    LOGERR("WLAN: supplicant control interface did not become ready: %s\n",
           socket.c_str());
    return false;
}

bool Wlan::StartInitSupplicantService() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get(WLAN_SUPP_SVC_PROP, value, "");

    std::string state = value;
    if (state == "running" || state == "restarting") {
        LOGINFO("WLAN: supplicant service already active (%s)\n", state.c_str());
        return WaitForSupplicantReady(WLAN_SUPP_READY_TIMEOUT_MS);
    }

    LOGINFO("WLAN: triggering supplicant bring-up (%s)\n", WLAN_SUPP_PREP_PROP);
    /* The init block resets this to 0 when it finishes, so a plain set to 1 is a
     * 0->1 (or ""->1) transition every time and re-fires the bring-up. The block
     * does the driver bring-up and then `start wpa_supplicant` itself, so we
     * still wait on init.svc.wpa_supplicant below. */
    property_set(WLAN_SUPP_PREP_PROP, "1");

    if (!WaitForProperty(WLAN_SUPP_SVC_PROP, "running", 20000)) {
        LOGERR("WLAN: init service %s failed to enter running state\n", WLAN_SUPP_SERVICE);
        return false;
    }

    // Qualcomm may expose wlan0 and mark the service running while CNSS is
    // still completing its DMS fallback. The control socket appears only
    // after that cold-start sequence, which can take more than 20 seconds.
    return WaitForSupplicantReady(WLAN_SUPP_READY_TIMEOUT_MS);
}

bool Wlan::StopInitSupplicantService() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get(WLAN_SUPP_SVC_PROP, value, "");

    if (std::string(value) == "stopped") {
        LOGINFO("WLAN: supplicant service already stopped\n");
        return true;
    }

    LOGINFO("WLAN: stopping init service %s\n", WLAN_SUPP_SERVICE);
    property_set("ctl.stop", WLAN_SUPP_SERVICE);

    if (!WaitForProperty(WLAN_SUPP_SVC_PROP, "stopped", 5000)) {
        LOGERR("WLAN: init service %s failed to enter stopped state\n", WLAN_SUPP_SERVICE);
        return false;
    }

    return true;
}

bool Wlan::IsProcessRunning(const std::string& name) {
    return TWFunc::Exec_Cmd("pidof " + name + " >/dev/null 2>&1") == 0;
}
