#pragma once

#ifdef OF_ENABLE_WLAN

#include <string>
#include <vector>

class AeraAdbd {
public:
	struct PairedDevice {
		std::string fingerprint;
		std::string name;
		long long last_seen = 0;
	};

	struct Status {
		bool enabled = false;
		bool secure = false;
		bool no_auth = false;
		bool wlan_connected = false;
		bool pairing = false;
		std::string mode;
		std::string ip;
		std::string connect_port;
		std::string connect_command;
		int pairing_port = 0;
		std::string pairing_code;
		std::string pairing_command;
	};

	static bool StartSecure(int port);
	static bool StartNoAuth(int port);
	static bool StartPairing(int timeout_sec);
	static bool StopPairing();
	static bool StopAll();
	static Status GetStatus();
	static void PrintStatus();
	static std::string WlanIp();

	// Authorized-device registry (backed by AeraSecrets). PrintDevices emits a
	// AERA_ADB_DEVICES_BEGIN/END block; ForgetDevice revokes by fingerprint or
	// name, removing the key from both the registry and the adb_keys files.
	static void PrintDevices();
	static std::vector<PairedDevice> ListDevices();
	static bool ForgetDevice(const std::string& id);
};

#endif // OF_ENABLE_WLAN
