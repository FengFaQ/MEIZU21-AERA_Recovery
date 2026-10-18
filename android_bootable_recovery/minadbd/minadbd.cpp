/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <string>

#include <android-base/logging.h>
#include <android-base/parseint.h>

#include "adb.h"
#include "adb_auth.h"
#include "transport.h"

#include "minadbd/types.h"
#include "minadbd_services.h"

using namespace std::string_literals;

int main(int argc, char** argv) {
  android::base::InitLogging(argv, &android::base::StderrLogger);
  int socket_fd = -1;
  int progress_fd = -1;
  bool rescue = false;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if ((option == "--socket_fd" || option == "--progress_fd") &&
        i + 1 < argc) {
      int value = -1;
      if (!android::base::ParseInt(argv[++i], &value)) {
        LOG(ERROR) << "Failed to parse fd for " << option;
        exit(kMinadbdArgumentsParsingError);
      }
      if (option == "--socket_fd") socket_fd = value;
      else progress_fd = value;
    } else if (option == "--rescue") {
      rescue = true;
    } else {
      LOG(ERROR) << "minadbd has invalid argument " << option;
      exit(kMinadbdArgumentsParsingError);
    }
  }
  if (socket_fd < 0) {
    LOG(ERROR) << "minadbd requires --socket_fd";
    exit(kMinadbdArgumentsParsingError);
  }
  if (fcntl(socket_fd, F_GETFD, 0) == -1) {
    PLOG(ERROR) << "Failed to get minadbd socket";
    exit(kMinadbdSocketIOError);
  }
  SetMinadbdSocketFd(socket_fd);
  if (progress_fd >= 0) {
    if (fcntl(progress_fd, F_GETFD, 0) == -1) {
      PLOG(ERROR) << "Failed to get sideload progress socket";
      exit(kMinadbdSocketIOError);
    }
    SetSideloadProgressFd(progress_fd);
  }

  if (rescue) {
    SetMinadbdRescueMode(true);
    adb_device_banner = "rescue";
  } else {
    adb_device_banner = "sideload";
  }

  signal(SIGPIPE, SIG_IGN);

  // We can't require authentication for sideloading. http://b/22025550.
  auth_required = false;
  socket_access_allowed = false;

  usb_init();

    //VLOG(ADB) << "Event loop starting";
    fdevent_loop();

  return 0;
}
