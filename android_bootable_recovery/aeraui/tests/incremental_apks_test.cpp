/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/backup/incremental_apks.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/wire_format_lite.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace aeraui::appvault;
namespace fs = std::filesystem;

std::string Binding(const std::string& source, const std::string& target) {
  std::string result;
  google::protobuf::io::StringOutputStream stream(&result);
  {
    google::protobuf::io::CodedOutputStream output(&stream);
    using google::protobuf::internal::WireFormatLite;
    WireFormatLite::WriteInt32(1, 2, &output);
    WireFormatLite::WriteString(2, source, &output);
    WireFormatLite::WriteString(3, target, &output);
  }
  return result;
}

int main() {
  char temporary[] = "/tmp/aera-incfs-test-XXXXXX";
  assert(mkdtemp(temporary));
  const fs::path root = temporary;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{ root };
  const std::string code = "/data/app/~~random==/com.example.app-token==";
  const fs::path backing = root / "MT_app" / "backing_store";
  IncrementalMapping mapping;
  std::string error;
  assert(FindIncrementalMapping(code, root.string(), mapping, error));
  assert(mapping.backing.empty());

  fs::create_directories(backing / "st_1_0");
  std::ofstream(backing / "st_1_0" / "base.apk") << "installer staging, not installed app";
  assert(FindIncrementalMapping(code, root.string(), mapping, error));
  assert(mapping.backing.empty());

  const fs::path app = backing / "st_2_1" / "com.example.app-token==";
  fs::create_directories(app);
  std::ofstream(app / "base.apk") << "raw IncFS backing file, not an APK";
  assert(FindIncrementalMapping(code, root.string(), mapping, error));
  assert(mapping.backing == backing.string());
  assert(mapping.subdirectory == "st_2_1");
  assert(mapping.target == "/data/app/~~random==");
  const auto source = (backing.parent_path() / "mount" / "st_2_1").string();
  const auto record = Binding(source, mapping.target);
  assert(MatchesIncrementalBinding(record, mapping));
  assert(!MatchesIncrementalBinding(Binding("st_2_1", mapping.target), mapping));
  assert(!MatchesIncrementalBinding(Binding(source, "/data/app/~~wrong=="), mapping));
  assert(!MatchesIncrementalBinding(record.substr(0, record.size() - 1), mapping));
  assert(!MatchesIncrementalBinding({}, mapping));
  assert(FindIncrementalMapping("/data/app/~~random==/com.example.app-other==", root.string(),
                                mapping, error));
  assert(mapping.backing.empty());
  assert(!FindIncrementalMapping("/data/app/../system", root.string(), mapping, error));
  assert(!FindIncrementalMapping("/data/app/~~random==/..", root.string(), mapping, error));
  assert(!FindIncrementalMapping("/data/app/~~random==/.", root.string(), mapping, error));
  assert(!FindIncrementalMapping("/data/app//com.example.app", root.string(), mapping, error));

  fs::create_directory_symlink(backing, root / "MT_link");
  // The store's backing_store path must not follow a symlink either.
  fs::create_directory(root / "MT_symlink");
  fs::create_directory_symlink(backing, root / "MT_symlink" / "backing_store");
  assert(FindIncrementalMapping(code, root.string(), mapping, error));
  assert(mapping.backing == backing.string());

  const fs::path other =
      root / "MT_duplicate" / "backing_store" / "st_2_1" / "com.example.app-token==";
  fs::create_directories(other);
  std::ofstream(other / "base.apk") << "ambiguous store";
  assert(!FindIncrementalMapping(code, root.string(), mapping, error));
  assert(error.find("Ambiguous") != std::string::npos);
  std::cout << "Incremental APK mapping tests passed\n";
}
