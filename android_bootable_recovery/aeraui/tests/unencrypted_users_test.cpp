/* SPDX-License-Identifier: Apache-2.0 */
#include "../platform/unencrypted_users.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  char temporary[] = "/tmp/aera-users-test.XXXXXX";
  const char *directory = mkdtemp(temporary);
  assert(directory);
  const std::string root = directory;
  const std::string metadata = root + "/system/users/";
  fs::create_directories(metadata);
  fs::create_directories(root + "/data");
  fs::create_directories(root + "/user/10");
  const auto write = [&](const std::string &name, const std::string &text) {
    std::ofstream output(metadata + name, std::ios::binary);
    output << text;
    assert(output.good());
  };
  write("0.xml", "<user id='0'><name>Owner name</name></user>");
  write("10.xml", "<user id='10'><name>Second user</name></user>");
  write("userlist.xml", "<users><user id='0'/><user id='10'/></users>");
  write("20.xml.reservecopy", "<user id='20'/>");
  write("01.xml", "<user id='1'/>");
  write("2147483648.xml", "<user id='2147483648'/>");
  write("11.xml", "<user id='12'/>");
  write("12.xml", "<user id='12' partial='true'/>");
  write("13.xml", "<user id='13' preCreated='true'/>");
  write("14.xml", "broken");
  fs::create_symlink(metadata + "0.xml", metadata + "15.xml");
  auto users = aeraui::UnencryptedAndroidUsers(root, true, false);
  assert(users.size() == 2);
  assert(users[0].id == 0 && users[0].decrypted && users[0].name == "Owner name");
  assert(users[1].id == 10 && users[1].decrypted && users[1].name == "Second user");
  assert(aeraui::UnencryptedAndroidUsers(root, false, false).empty());
  assert(aeraui::UnencryptedAndroidUsers(root, true, true).empty());
  assert(aeraui::UnencryptedAndroidUsers(root + "/missing", true, false).empty());
  fs::remove(root + "/user/10");
  users = aeraui::UnencryptedAndroidUsers(root, true, false);
  assert(users.size() == 2 && !users[1].decrypted);
  fs::remove(root + "/data");
  fs::create_directories(root + "/user/0");
  users = aeraui::UnencryptedAndroidUsers(root, true, false);
  assert(users[0].decrypted);
  write("0.xml", "<user id='0'/>");
  users = aeraui::UnencryptedAndroidUsers(root, true, false);
  assert(users[0].name == "Owner");

  // Optional real-device ABX fixtures stay outside the public repository.
  if (argc == 3) {
    fs::copy_file(argv[1], metadata + "0.xml", fs::copy_options::overwrite_existing);
    fs::copy_file(argv[2], metadata + "10.xml", fs::copy_options::overwrite_existing);
    fs::create_directories(root + "/user/10");
    users = aeraui::UnencryptedAndroidUsers(root, true, false);
    assert(users.size() == 2 && users[0].id == 0 && users[1].id == 10);
    assert(users[0].decrypted && users[1].decrypted);
    std::cout << "Real-device ABX user metadata passed\n";
  }
  fs::remove_all(root);
  std::cout << "Unencrypted Android user discovery passed\n";
}
