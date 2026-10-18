// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "aeraui/platform/aera_message.hpp"

void set_select_fd();

void gui_msg(const char* text);
void gui_warn(const char* text);
void gui_err(const char* text);
void gui_highlight(const char* text);
void gui_msg(Message message);
void gui_err(Message message);

std::string gui_parse_text(std::string text);
std::string gui_lookup(const std::string& resource_name,
                       const std::string& default_value);

// Compatibility hooks used by recovery-core operations. AERA UI owns page
// navigation, so these never route through an XML PageManager.
int gui_forceRender();
int gui_changePage(std::string page);
int gui_changeOverlay(std::string overlay);
void gui_switchControlMode();
void gui_notifyVarChange(const char* name, const char* value);

// Returns a writable, NUL-terminated file buffer owned by the caller.
char* aera_read_file_buffer(const std::string& path);
