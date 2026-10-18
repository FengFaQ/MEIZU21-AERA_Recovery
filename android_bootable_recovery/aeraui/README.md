# AERA Recovery Project native engine

AERA Recovery Project's interface is a code-defined LVGL recovery engine. It is not an XML theme
and does not use PageManager for ordinary navigation or tool screens.

## Architecture

- LVGL owns layout, drawing, widgets, transitions and animation.
- minui remains the Android recovery display/input compatibility layer.
- The initial backend uses one full-screen XRGB8888 buffer so minui's DRM
  double-buffering cannot expose stale partial frames.
- Recovery operations use the existing native backend through the narrow
  `include/aeraui/backend.hpp` controller API. PartitionManager remains
  responsible for partition discovery, backup, restore, wipe and mounting.

`gr_init()` must run before `Engine::Initialize()`. The recovery event loop feeds
normalized touch coordinates through `Engine::SetPointer()` and calls
`Engine::RunFrame()` at the returned interval.

Device trees can set `AERA_UI_ADAPTIVE_RESOLUTION := true` together with
`AERA_SCREEN_H`, `AERA_STATUS_H`, `AERA_STATUS_INDENT_LEFT`, and
`AERA_STATUS_INDENT_RIGHT`. Screen height uses the stock theme's 1080-wide
reference units and is converted to AERA UI's 1440-wide coordinate space; status-bar
height and horizontal safe-area indents remain literal. Infiniti's 2340/141
values therefore become a 1440 x 3120 canvas with a 141-pixel status bar. That
canvas renders across the complete native framebuffer without letterbox bars,
and physical touch coordinates are mapped back to it. Leaving adaptive mode
unset retains native one-to-one rendering.

For bring-up, create `/tmp/aeraui-capture` over ADB. The runner removes
the request and writes the current framebuffer to `/tmp/aeraui.png`.

The native engine is the unconditional AERA recovery interface; device trees do
not need an enable marker or runtime environment switch. Hardware Back returns
to the AERA home surface. The XML renderer remains linked only for legacy
support code and is not selected during normal AERA startup.

## Main interface

Files, Backup, Wipe and Menu are the persistent navigation destinations. Menu
opens Restore, Mounts, Recovery log, Android Users, Preferences and Reboot.
Only Android user 0 participates in startup decryption. Additional FBE users
remain locked until explicitly selected on the Android Users page; a secondary
unlock never repeats the startup handoff or changes the active recovery page.
The file browser
requires a review and a swipe before installing a ZIP; selecting a file alone
never starts installation. Partition operations also require an exact-target
review and a swipe, except Format Data's explicit typed confirmation below.
Navigation is blocked while a backend job runs, and its
result and recovery log are shown on the operation page.

Main and unlock pages use a uniform charcoal base and one continuous procedural
grain background in `design.hpp`, matching the 1440 x 3168 native design surface.
The opaque XRGB surface is generated once in shared static storage (about
17.4 MiB RAM), not tiled or embedded as a large wallpaper asset. It matches
scanout's pixel format to avoid per-pixel alpha blending during scrolling.
Scrollable rows retain color feedback without a pressed scaling layer.
Grain does not animate or use a
blur buffer. Boot retains its separate styling. Backup contains
Create and Restore tabs, compact partition checkboxes and an estimated selected
size (before compression, not a promise of final archive size). Operation pages
animate an activity indicator and interpolate backend progress; partition and
byte/file counts come from stock DataManager values, not simulated stages.

Wipe has separate partition-wipe and Format Data tabs, following the stock
`pages/wipe.xml` flow. Format Data requires exact lowercase `yes` and a separate
button press. Keyboard Enter never submits. The controller checks the request
again, then uses `PartitionManager.Format_Data()` (the stock DATAMEDIA path),
including its merge/encryption/device hooks. Confirmation clears on submission
or leaving the page. No device format is performed by automated UI tests.
The bottom navigation is opaque and anchored at the screen's bottom edge.

The native status bar opens AERA Quick Settings when tapped or dragged down.
The shade follows the pointer and settles with an eased open/close animation.
It exposes the existing asynchronous WLAN controller, a live brightness slider,
device-gated flashlight control, portrait/landscape rotation, and shortcuts to
Network and Preferences. The shade is not attached to decrypt, lock, picture
viewer, or other surfaces that intentionally omit an interactive status bar.
Destructive native tools return to portrait until each dense operation layout
has a dedicated landscape arrangement, so confirmation controls cannot become
clipped by rotation.

Preferences groups brightness, 12/24-hour time, a fixed UTC offset in 15-minute
steps (no automatic DST), hidden files, ZIP signature verification, default
backup compression, SHA-256/MD5 backup checksums, and USB MTP. Stock DataManager
settings feed the native file/clock UI and existing install/backup backend.
Save preferences uses the stock settings store; failures are reported rather
than presented as success. Backup checksum generation and restore verification
remain enabled. This is not complete OrangeFox settings parity: XML themes,
recovery passwords, native screen timeout/locking, haptics and OTA preferences
still need dedicated native integration.

Host-only regression checks (mock controllers, no destructive backend linked):

```sh
cmake -S bootable/recovery/aeraui/tests -B /tmp/aera-ui-tests \
  -DLVGL_DIR="$PWD/external/lvgl"
cmake --build /tmp/aera-ui-tests -j12
ctest --test-dir /tmp/aera-ui-tests --output-on-failure
```

These cover wipe selection, exact format confirmation/re-arming, no keyboard
auto-submit, navigation geometry, preference toggles, backup/restore selection,
secondary-user selection, decrypt rendering, viewer lifecycle, operation
completion, and Host API 2 protocol negotiation/resource limits.

## Plugin Host API 2

Arbitrary plugin IDs can use the generic Host API 2 entrypoint without a
recovery rebuild. Plugins run as root workers and describe a bounded page;
AERA owns every visible widget, touch event, permission prompt and privileged
operation. Existing Host API 1 apps retain their built-in routing. See
`plugin_api/README.md` for the manifest, protocol, root execution and lifecycle
contract. API 2 also exposes host-mediated backup and restore helpers for AERA
preferences and Android user 0's SettingsProvider databases. Host API 2 plugins
additionally have direct access to recovery's files, devices and mounted data.

## Picture viewer

Files opens PNG and JPEG previews in a modal with Fit, zoom buttons,
drag-to-pan and Back. Edge Back also closes it without losing the folder.
Decoding runs outside the UI thread and only one decoder may run at a time.
PNG uses the already-packaged libpng. Baseline JPEG uses LVGL's bundled TJpgDec
(BGR output); its partial scaling implementation stays disabled and the output
callback samples large JPEGs into a preview with at most 2048 pixels per side.
Progressive JPEG falls back to libjpeg-turbo with bounded native downscaling.
Source JPEGs are limited to 64 megapixels / 16384 pixels per side. PNGs are
limited to 16 megapixels / 8192 pixels per side. Files are limited to 32 MiB.
HEIC, WebP, GIF and EXIF auto-rotation are not supported.
`tests/picture_decode_test.cpp` covers color channels, dimensions, large JPEG
sampling, cancellation and invalid/unsupported input using generated fixtures.

Native sideload, terminal, image flashing and password-protected backup archive
restore are not implemented. They must not be presented as functioning actions.
Device storage decryption is a separate workflow from archive decryption.

## Browser (staged)

Menu now includes Browser. Its compressed WebKit runtime can be verified and
prepared on demand without touching Wi-Fi or delaying recovery boot. Browsing
is still blocked pending a compatible isolated launcher and device validation;
this is not a functioning browser yet. See
`browser/README.md` for implemented pieces, tests and remaining launch gates.

## Size policy

The UI uses built-in fonts and vector primitives by default. Large PNG sequences,
video runtimes, SVG engines and Lottie are disabled. Release builds use function
and data section garbage collection so unused LVGL widgets are not packaged.
