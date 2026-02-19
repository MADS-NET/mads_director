#pragma once

#include <optional>
#include <string>
#include <vector>

struct ProcessSpec {
  std::string name;
  std::string command;
  std::optional<std::string> after;
  std::optional<std::string> workdir;
  bool enabled = true;
  int scale = 1;
  bool relaunch = false;
  bool tty = false;
};

struct DirectorConfig {
  std::vector<ProcessSpec> processes;
};

bool load_config(const std::string& path, DirectorConfig* out_config, std::string* out_error);
