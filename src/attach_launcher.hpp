#pragma once

#include <string>

bool launch_attach_terminal(const std::string& executable_path, const std::string& socket_path, std::string* out_error);
