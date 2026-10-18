# AERA Host API 2

Host API 2 lets a plugin with a previously unknown ID install and run without
rebuilding recovery. The Host API surface remains rendered by AERA. The plugin
is an ARM64 root worker that sends bounded declarative UI messages over
a `SOCK_SEQPACKET` channel inherited as file descriptor 4. It runs in recovery's
filesystem and network namespaces as UID/GID 0, with recovery's capabilities,
devices, mounted partitions and data available directly. Running the plugin
therefore grants it the same access as recovery itself.

## Manifest contract

An API 2 manifest uses `type: "ui-runtime"`, `entry: "main"`,
`min_host_api: 2`, `protocol_version: 2`, and
`executable: "usr/bin/aera-plugin"`. It must request `display` and
`touch-input`. Supported privileged permissions are:

- `settings-backup` / `settings-restore` for AERA Recovery preferences
- `android-settings-backup` / `android-settings-restore` for Android user
  0's SettingsProvider `system`, `secure`, and `global` databases, plus
  the LineageSettingsProvider database when the installed ROM supplies it
- `screen-mirror` to start or stop AERA's existing USB-only display stream;
  framebuffer capture and input injection remain in the trusted host

Host-mediated operations are denied unless the permission is declared and the
user confirms the individual request in trusted AERA UI. Root plugins may also
operate on recovery resources directly. The Android settings operations use
wire operation IDs 3 and 4; USB mirror start/stop use IDs 5 and 6. The Android
settings convenience implementation includes ROM customization values such as
Infinity-X and Lineage settings but excludes lock credentials, accounts, app
data and SettingsProvider SSAIDs. Restore validates and stages the complete
snapshot before transactionally replacing live files.

API 1 manifests and their built-in scene routing remain compatible.

## Protocol and limits

`protocol.hpp` is the canonical wire definition. Every packet is exactly 1144
bytes, includes the `A2PI` magic and contains only fixed-size strings. The
worker starts with `HELLO`, placing its minimum protocol version in `value` and
maximum in `flags`. AERA replies with `HELLO_ACK` and `LIFECYCLE/RESUME`.

The initial declarative surface supports a page title/body, up to 12 action
buttons, status updates, lifecycle events and mediated operations. The host
advertises optional extensions in the `HELLO_ACK` flags. `kFeatureMetrics`
adds compact section headers, up to 64 keyed metric rows, and in-place metric
updates. `kFeatureBackNavigation` lets a page declare one enabled action as its
parent with `SET_BACK_ACTION`; AERA's edge-back gesture then invokes that
action before leaving the plugin. A worker must not send extension messages
unless the corresponding feature bit was advertised; this preserves
compatibility with older API 2 hosts. AERA rejects bad sequences, duplicate
action or metric IDs, unknown flags, oversized packets and more than 128
worker messages per second.

The host expands a hash-verified runtime into private RAM and launches its musl
loader directly as root. It does not apply a chroot, Minijail, namespaces,
seccomp, capability removal or resource limits to Host API 2 plugins. Leaving
the scene terminates the plugin's process group and closes the channel. `PATH`
contains both the plugin's `usr/bin` and recovery command directories, while
`AERA_PLUGIN_ROOT` names the extracted runtime for bundled resources.
`AERA_LOCALE` contains the selected recovery locale (for example `de_DE` or
`zh_CN`) and `LANG` carries the matching UTF-8 locale. Runtime plugins own
their visible strings and must fall back to English when a locale or string is
missing. Relaunching a plugin after a language change supplies the new locale
without changing the Host API 2 wire format.

`AERA-settings-backup-plugin` is the reference implementation. It proves a new
ID can render and request backup/restore without being added to recovery's
launcher source.
