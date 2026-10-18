/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "../features/root/root_manager.hpp"

static_assert(aeraui::root::Enabled() == EXPECT_ROOT_ENABLED,
              "Only AERA_ENABLE_ROOT_MANAGER=1 enables Root Manager");

int main() { return 0; }
