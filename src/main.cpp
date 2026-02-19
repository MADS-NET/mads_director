#include "attach_client.hpp"
#include "config.hpp"
#include "gui_app.hpp"
#include "process_manager.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_example_config() {
  std::cout
      << "# director example configuration\n"
      << "# Notes:\n"
      << "# - Each top-level section name is the process name (mandatory).\n"
      << "# - All options are shown below with defaults and purpose.\n\n"
      << "[db] # process section name (mandatory)\n"
      << "command = \"./bin/db --port 5432\" # mandatory: command line to execute\n"
      << "after = \"\" # optional, default=\"\": start after this process name\n"
      << "workdir = \"\" # optional, default=\"\": working directory (uses director working directory)\n"
      << "enabled = true # optional, default=true: when false, process is listed but never started\n"
      << "scale = 1 # optional, default=1: number of instances to launch\n"
      << "relaunch = false # optional, default=false: restart on non-zero exit status\n"
      << "tty = false # optional, default=false: enable TTY/attach interaction\n"
      << "# if workdir/.venv exists, director exposes it to launched Python commands\n\n"
      << "[api] # process section name (mandatory)\n"
      << "command = \"./bin/api_server --port 8080\" # mandatory: command line to execute\n"
      << "after = \"db\" # optional, default=\"\": dependency by process name\n"
      << "workdir = \"services/api\" # optional, default=\"\": working directory for this process\n"
      << "enabled = true # optional, default=true: when false, process is listed but never started\n"
      << "scale = 1 # optional, default=1: number of instances to launch\n"
      << "relaunch = false # optional, default=false: restart on non-zero exit status\n"
      << "tty = false # optional, default=false: enable TTY/attach interaction\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "director.toml";
  std::string attach_socket_path;
  std::string executable_path = argc > 0 ? argv[0] : "director";
  bool force_show_example = false;
  if (!executable_path.empty()) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(executable_path, ec);
    if (!ec) {
      executable_path = absolute.string();
    }
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--example" || arg == "--example-config") {
      force_show_example = true;
      continue;
    }
    if (arg == "--attach-socket" && i + 1 < argc) {
      attach_socket_path = argv[++i];
      continue;
    }
    if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
      config_path = argv[++i];
      continue;
    }
    config_path = arg;
  }

  if (!attach_socket_path.empty()) {
    std::string error;
    const int code = run_attach_client(attach_socket_path, &error);
    if (code != 0 && !error.empty()) {
      std::cerr << "Attach client failed: " << error << "\n";
    }
    return code;
  }

  if (force_show_example) {
    print_example_config();
    return 0;
  }

  DirectorConfig config;
  std::string error;
  if (!load_config(config_path, &config, &error)) {
    std::cerr << "Failed to load config '" << config_path << "': " << error << "\n\n";
    print_example_config();
    return 1;
  }

  ProcessManager manager;
  if (!manager.initialize(config, &error)) {
    std::cerr << "Failed to initialize process manager: " << error << "\n";
    return 1;
  }

  manager.launch_ready_processes();

  GuiApp gui;
  return gui.run(&manager, executable_path);
}
