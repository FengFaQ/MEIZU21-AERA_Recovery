// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>

#include "third_party/rapidxml/rapidxml.hpp"
#include "twcommon.h"

void rapidxml::parse_error_handler(const char* reason, void* location) {
  LOGERR("XML parse error: %s\n", reason != nullptr ? reason : "unknown");
  if (location != nullptr) {
    std::fprintf(stderr, "XML input near: %.160s\n",
                 static_cast<const char*>(location));
  }
}
