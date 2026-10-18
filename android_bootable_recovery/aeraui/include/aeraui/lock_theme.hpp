/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <algorithm>

namespace aeraui {
struct LockTheme {
  int clock = 0; // Classic, analogue, stacked.
  int clock_size = 100; // Percent, independent of clock design.
  int date_style = 0, date_size = 100, date_colour = 0;
  bool aod = false;
  int date_placement = 0; // Below clock, above clock, bottom.
  int aod_brightness = 2; // Percentage of panel brightness range.
  int placement = 0; // Top, centre, bottom.
  int colour = 1; // Accent, white, custom.
  uint32_t rgb = 0xffffff;
  int dim = 25, blur = 0;
  int unlock = 0; // Slide, swipe up, hold.
  int animation = 1; // None or fade (slide/hold only).
  // Retained only to read older saved settings; no longer exposed or rendered.
  bool charging = false, wifi = false, battery = false, activity = false;
  bool flashlight = false, screenshot = false, minimal = false;
  std::string owner, wallpaper;
};
inline bool ValidLockTheme(const LockTheme &t) {
  auto text = [](const std::string &s, size_t limit) {
    return s.size() <= limit && std::none_of(s.begin(), s.end(), [](unsigned char c) {
      return c < 32 || c == 127;
    });
  };
  return t.clock >= 0 && t.clock <= 2 && t.date_style>=0 && t.date_style<=4 &&
      t.date_size>=60 && t.date_size<=200 && t.date_colour>=0 && t.date_colour<=2 &&
      t.date_placement>=0 && t.date_placement<=2 && t.aod_brightness>=1 && t.aod_brightness<=100 && t.placement >= 0 && t.placement <= 2 &&
      t.colour >= 0 && t.colour <= 2 && t.rgb <= 0xffffff && t.dim >= 0 && t.dim <= 90 &&
      t.blur >= 0 && t.blur <= 100 && t.unlock >= 0 && t.unlock <= 2 &&
      t.animation >= 0 && t.animation <= 1 && t.clock_size>=60 && t.clock_size<=180 && text(t.owner, 160) && text(t.wallpaper, 4096);
}
inline std::string EncodeLockTheme(const LockTheme &t) {
  std::ostringstream s;
  s << 4 << ' ' << t.clock << ' ' << t.placement << ' ' << t.colour << ' ' << t.rgb
    << ' ' << t.dim << ' ' << t.blur << ' ' << t.unlock << ' ' << t.animation
    << ' ' << t.charging << ' ' << t.wifi << ' ' << t.battery << ' ' << t.activity
    << ' ' << t.flashlight << ' ' << t.screenshot << ' ' << t.minimal
    << ' ' << std::quoted(t.owner) << ' ' << std::quoted(t.wallpaper) << ' ' << t.clock_size
    << ' ' << t.date_style << ' ' << t.date_size << ' ' << t.date_colour << ' ' << t.aod
    << ' ' << t.date_placement << ' ' << t.aod_brightness;
  return s.str();
}
inline LockTheme DecodeLockTheme(const std::string &value) {
  LockTheme t; int version = 0;
  std::istringstream s(value);
  if (!(s >> version >> t.clock >> t.placement >> t.colour >> t.rgb >> t.dim >> t.blur
      >> t.unlock >> t.animation >> t.charging >> t.wifi >> t.battery >> t.activity
      >> t.flashlight >> t.screenshot >> t.minimal >> std::quoted(t.owner)
      >> std::quoted(t.wallpaper)) || version<1 || version>4) return {};
  if(version>=2 && !(s>>t.clock_size)) return {};
  if(version>=3 && !(s>>t.date_style>>t.date_size>>t.date_colour>>t.aod)) return {};
  if(version>=4 && !(s>>t.date_placement>>t.aod_brightness)) return {};
  if(version<3 && t.clock>2)t.clock=0;
  if(t.animation>=2 && t.animation<=5)t.animation=1;
  if(!ValidLockTheme(t)) return {};
  return t;
}
LockTheme RecoveryLockTheme();
bool RecoverySetLockTheme(const LockTheme &theme);
} // namespace aeraui
