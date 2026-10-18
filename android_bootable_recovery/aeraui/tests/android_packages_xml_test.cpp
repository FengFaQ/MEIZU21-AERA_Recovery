/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/backup/android_packages_xml.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
  tinyxml2::XMLDocument document;
  std::string error;
  assert(aeraui::appvault::ParsePackagesXml(
      "<packages><package name='example.app' userId='10123'/></packages>", document, error));
  assert(!aeraui::appvault::ParsePackagesXml("ABX", document, error));
  const std::string truncated("ABX\0\x10\x32\xff\xff\x00\x04pkg", 14);
  assert(!aeraui::appvault::ParsePackagesXml(truncated, document, error));
  for (int i = 1; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    assert(file.good());
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (!aeraui::appvault::ParsePackagesXml(bytes, document, error)) {
      std::cerr << argv[i] << ": " << error << '\n';
      return 1;
    }
    size_t records = 0;
    for (auto* node = document.RootElement()->FirstChildElement(); node;
         node = node->NextSiblingElement())
      ++records;
    std::cout << argv[i] << ": " << document.RootElement()->Name() << ", " << records
              << " records parsed\n";
    tinyxml2::XMLPrinter printer;
    document.Print(&printer);
    tinyxml2::XMLDocument roundtrip;
    assert(aeraui::appvault::ParsePackagesXml(printer.CStr(), roundtrip, error));
    for (size_t length : { size_t(4), size_t(5), bytes.size() / 2 }) {
      assert(!aeraui::appvault::ParsePackagesXml(bytes.substr(0, length), roundtrip, error));
    }
  }
  std::cout << "Android XML/ABX checks passed\n";
}
