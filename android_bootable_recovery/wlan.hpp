#pragma once

#include <string>
#include <vector>
#include <regex>

class Wlan {
public:
    static bool Init();

    static bool Enable();
    static bool Disable();
    static bool Disconnect();
    static bool IsEnabled();

    static bool Scan();
    static bool Connect();
    static bool ConnectSaved();
    static bool ForgetSaved();
    static bool Info();
    // Background status refresh that yields instead of blocking when another WLAN
    // operation is in progress. Used by the worker's idle poll so it never stalls
    // (for seconds) behind a foreground scan/connect. Returns false if skipped.
    static bool RefreshInfoIfIdle();
    static bool RefreshSaved();
    static bool UpdateConnectedName();
    static bool TestConnection();
    static std::string NetworkMetadataPath(const std::string& ssid);

    // Reload the "wlan" page (re-running its load action so a freshly written
    // /tmp/wlan/list.txt is displayed) but only if it is the page currently
    // shown — so a background scan can surface results without yanking the user
    // off whatever page they navigated to. Safe to call from any thread: the
    // Native AERA UI observes the published state without page reload hooks.
    static void RefreshWlanPageIfShown();

private:
    static bool EnsureTmpLayout();
    static bool EnsureSupplicantConf();

    static bool StartSupplicant();
    static bool StopSupplicant();
    static bool PrepareStableMacFirmware();
    static bool WaitForProperty(const std::string& key, const std::string& expected, int timeout_ms);
    static bool WaitForSupplicantReady(int timeout_ms);
    static bool StartInitSupplicantService();
    static bool StopInitSupplicantService();
    static bool StartDhcp();
    static bool StopDhcp();

    // Info() body, run with g_wlan_op_mutex already held (by Info() or by the
    // try-locked RefreshInfoIfIdle()).
    static bool InfoLocked();

    static bool BuildScanList();
    static bool BuildSavedList();
    static bool BuildConnectedName();
    static bool ParseSupplicantStatus(const std::string& status, std::string& wpa_state, std::string& ssid, std::string& ip_addr);

    static bool RunCommand(const std::string& cmd);
    static bool RunCommand(const std::string& cmd, std::string& output);

    // wpa_supplicant control-interface transport. SuppCmd sends a ctrl command
    // (e.g. `STATUS`, `SET_NETWORK 0 ssid "x"`) over a persistent wpa_ctrl
    // connection, transparently falling back to a wpa_cli passthrough if the
    // control socket cannot be opened. SuppWaitEvent blocks for an unsolicited
    // CTRL-EVENT-* via the attached monitor connection. All are serialized by
    // an internal mutex so the GUI ActionThread and AERA dispatcher can
    // share the connection safely.
    static bool SuppCmd(const std::string& ctrl_cmd, std::string& out);
    static bool SuppCmd(const std::string& ctrl_cmd);
    static bool SuppWaitEvent(const std::vector<std::string>& any_of, int timeout_ms, std::string& matched);
    static bool EnsureSuppChannel();
    static void CloseSuppChannel();

    static bool WriteFile(const std::string& path, const std::string& content);
    static bool ReadFile(const std::string& path, std::string& out);
    static bool FileExists(const std::string& path);
    static bool MkdirRecursive(const std::string& path);
    static bool IsProcessRunning(const std::string& name);

    static std::string Trim(const std::string& s);
    static std::string GetIface();
    static std::string GetCtrlDir();
    static std::string GetSupplicantConf();
    static std::string EscapeDoubleQuotes(const std::string& s);
    static std::string GetWpaCliBinary();
    static std::string GetBusyboxBinary();
    static std::string FindBinary(const std::vector<std::string>& paths);
    static std::string GetIfconfigBinary();
};
