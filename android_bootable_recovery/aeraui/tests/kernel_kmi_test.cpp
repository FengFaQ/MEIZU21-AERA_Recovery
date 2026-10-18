/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/root/kernel_kmi.hpp"
#include <cassert>
#include <string>
int main() {
  using namespace std::string_literals;
  using aeraui::root::KernelKmiFromBanner;
  assert(KernelKmiFromBanner("Linux version 6.6.89-android15-8-g123 (builder)\n") == "android15-6.6");
  assert(KernelKmiFromBanner("Linux version 5.10.228-android12-9-gabc (builder)\n") == "android12-5.10");
  assert(KernelKmiFromBanner("Linux version 6.1.75-android14-11-g123 (builder)\n") == "android14-6.1");
  assert(KernelKmiFromBanner("Linux version 4.19.157-perf+ (builder)\n").empty());
  assert(KernelKmiFromBanner("Linux version 6.6.89-android15").empty());
  assert(KernelKmiFromBanner("6.6.89-android15-8 random data").empty());
  const std::string binary(40, '\0');
  assert(KernelKmiFromBanner(binary + "Linux version 6.6.89-android15-8\0"s) == "android15-6.6");
}
