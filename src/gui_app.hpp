#pragma once

#include <string>

class ProcessManager;

class GuiApp {
 public:
  int run(ProcessManager* manager, const std::string& executable_path);
};
