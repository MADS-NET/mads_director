#include "process_manager.hpp"

#include "attach_launcher.hpp"
#include "platform_process.hpp"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMaxLogLines = 600;

bool detect_cycle_dfs(const std::string& node, const std::unordered_map<std::string, std::vector<std::string>>& graph,
                      std::unordered_set<std::string>* visiting, std::unordered_set<std::string>* visited) {
  if (visiting->contains(node)) {
    return true;
  }

  if (visited->contains(node)) {
    return false;
  }

  visiting->insert(node);
  const auto iter = graph.find(node);
  if (iter != graph.end()) {
    for (const auto& dep : iter->second) {
      if (detect_cycle_dfs(dep, graph, visiting, visited)) {
        return true;
      }
    }
  }

  visiting->erase(node);
  visited->insert(node);
  return false;
}

std::string scaled_name(const std::string& base_name, int index, int scale) {
  if (scale == 1) {
    return base_name;
  }
  std::ostringstream stream;
  stream << base_name << "[" << (index + 1) << "]";
  return stream.str();
}

}  // namespace

struct ProcessManager::ManagedProcess {
  ProcessRuntimeView view;
  std::string command;
  std::string workdir;
  std::unique_ptr<PlatformProcess> process;
  bool launched_by_scheduler = false;
  bool externally_attached = false;
  std::string pending_line;
  bool pending_cr = false;
};

ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
  stop_all();
}

bool ProcessManager::initialize(const DirectorConfig& config, std::string* out_error) {
  _processes.clear();
  const std::filesystem::path base_workdir = std::filesystem::current_path();

  std::unordered_map<std::string, int> base_scale;
  std::unordered_map<std::string, std::optional<std::string>> base_after;
  std::unordered_map<std::string, std::vector<std::string>> graph;

  for (const auto& process : config.processes) {
    base_scale[process.name] = process.scale;
    base_after[process.name] = process.after;
    if (process.after.has_value()) {
      graph[process.name].push_back(*process.after);
    }
  }

  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  for (const auto& process : config.processes) {
    if (detect_cycle_dfs(process.name, graph, &visiting, &visited)) {
      *out_error = "Dependency cycle detected around process '" + process.name + "'.";
      return false;
    }
  }

  for (const auto& process : config.processes) {
    for (int i = 0; i < process.scale; ++i) {
      ManagedProcess managed;
      managed.view.name = scaled_name(process.name, i, process.scale);
      managed.view.base_name = process.name;
      managed.view.enabled = process.enabled;
      managed.view.relaunch = process.relaunch;
      managed.view.tty = process.tty;
      managed.view.command = process.command;
      managed.command = process.command;
      if (process.workdir.has_value()) {
        const std::filesystem::path candidate(*process.workdir);
        managed.workdir = candidate.is_absolute() ? candidate.string() : (base_workdir / candidate).string();
      } else {
        managed.workdir = base_workdir.string();
      }
      managed.process = create_platform_process();

      if (process.after.has_value()) {
        const int dep_scale = base_scale[*process.after];
        for (int dep_index = 0; dep_index < dep_scale; ++dep_index) {
          managed.view.dependencies.push_back(scaled_name(*process.after, dep_index, dep_scale));
        }
      }

      _processes.push_back(std::move(managed));
    }
  }

  return true;
}

void ProcessManager::launch_ready_processes() {
  std::string ignored_error;
  for (std::size_t i = 0; i < _processes.size(); ++i) {
    auto& process = _processes[i];
    if (!process.view.enabled) {
      continue;
    }
    if (process.view.running || process.view.ever_started) {
      continue;
    }
    if (!can_start(process)) {
      continue;
    }

    if (start_process_internal(i, &ignored_error)) {
      process.launched_by_scheduler = true;
    }
  }
}

void ProcessManager::tick() {
  poll_external_attach();

  for (std::size_t i = 0; i < _processes.size(); ++i) {
    auto& process = _processes[i];
    if (!process.externally_attached) {
      capture_output(&process);
    }

    if (!process.view.running) {
      continue;
    }

    if (process.process->is_running()) {
      process.view.pid = process.process->process_id();
      continue;
    }

    process.view.running = false;
    process.view.pid = -1;
    process.view.exit_code = process.process->exit_code();

    std::ostringstream line;
    line << "Process exited with status " << process.view.exit_code << ".";
    append_log(&process, line.str());

    if (process.view.relaunch && process.view.exit_code != 0) {
      append_log(&process, "Relaunch enabled and exit status non-zero. Restarting process.");
      std::string error;
      if (!start_process_internal(i, &error)) {
        append_log(&process, "Restart failed: " + error);
      }
    }
  }
}

void ProcessManager::stop_all() {
  close_external_attach("Director shutting down.");

  for (auto& process : _processes) {
    if (!process.view.running) {
      continue;
    }
    process.process->stop();
    process.view.running = false;
    process.view.pid = -1;
  }
}

std::size_t ProcessManager::process_count() const {
  return _processes.size();
}

const ProcessRuntimeView* ProcessManager::process_at(std::size_t index) const {
  if (index >= _processes.size()) {
    return nullptr;
  }
  return &_processes[index].view;
}

bool ProcessManager::start_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (!can_start(process)) {
    *out_error = "Dependencies are not started yet.";
    return false;
  }

  return start_process_internal(index, out_error);
}

bool ProcessManager::stop_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  process.process->stop();
  process.view.running = false;
  process.view.pid = -1;
  append_log(&process, "Process stopped by user.");
  return true;
}

bool ProcessManager::restart_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (process.view.running) {
    process.process->stop();
    process.view.running = false;
    process.view.pid = -1;
  }

  return start_process_internal(index, out_error);
}

bool ProcessManager::send_input(std::size_t index, const std::string& input, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  if (!process.process->write_input(input)) {
    *out_error = "Failed to send input. This process may not accept stdin in current mode.";
    return false;
  }

  return true;
}

bool ProcessManager::attach_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  if (!process.view.tty) {
    *out_error = "Attach is only available for processes with tty=true.";
    return false;
  }

  append_log(&process, "Attach session started.");
  if (!process.process->attach_until_detach(out_error)) {
    append_log(&process, "Attach failed: " + *out_error);
    return false;
  }
  append_log(&process, "Attach session ended.");
  return true;
}

bool ProcessManager::open_external_attach(std::size_t index, const std::string& executable_path, std::string* out_error) {
#ifdef _WIN32
  (void)index;
  (void)executable_path;
  *out_error = "External attach is currently implemented only on POSIX.";
  return false;
#else
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  if (_external_attach.active) {
    *out_error = "An external attach session is already active.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.enabled) {
    *out_error = "Process is disabled in config (enabled=false).";
    return false;
  }
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }
  if (!process.view.tty) {
    *out_error = "External attach is only available for tty=true processes.";
    return false;
  }

  const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    *out_error = std::string("socket failed: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string socket_path = "/tmp/director_attach_" + std::to_string(getpid()) + "_" + std::to_string(index) +
                                  "_" + std::to_string(static_cast<unsigned long long>(time(nullptr))) + ".sock";
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    close(listen_fd);
    *out_error = "Attach socket path is too long.";
    return false;
  }
  std::strcpy(addr.sun_path, socket_path.c_str());
  unlink(socket_path.c_str());

  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string error = std::string("bind failed: ") + std::strerror(errno);
    close(listen_fd);
    *out_error = error;
    return false;
  }
  if (listen(listen_fd, 1) != 0) {
    const std::string error = std::string("listen failed: ") + std::strerror(errno);
    close(listen_fd);
    unlink(socket_path.c_str());
    *out_error = error;
    return false;
  }

  const int flags = fcntl(listen_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  }

  if (!launch_attach_terminal(executable_path, socket_path, out_error)) {
    close(listen_fd);
    unlink(socket_path.c_str());
    return false;
  }

  _external_attach.active = true;
  _external_attach.process_index = index;
  _external_attach.listen_fd = listen_fd;
  _external_attach.client_fd = -1;
  _external_attach.socket_path = socket_path;

  append_log(&process, "Waiting for external attach client...");
  return true;
#endif
}

bool ProcessManager::can_start(const ManagedProcess& process) const {
  for (const auto& dep_name : process.view.dependencies) {
    const auto iter = std::find_if(_processes.begin(), _processes.end(),
                                   [&](const ManagedProcess& candidate) { return candidate.view.name == dep_name; });

    if (iter == _processes.end()) {
      return false;
    }

    if (!iter->view.ever_started) {
      return false;
    }
  }

  return true;
}

bool ProcessManager::start_process_internal(std::size_t index, std::string* out_error) {
  auto& process = _processes[index];
  if (process.view.running) {
    return true;
  }

  std::string start_error;
  if (!process.process->start(process.command, process.workdir, process.view.tty, &start_error)) {
    *out_error = "Failed to start '" + process.view.name + "': " + start_error;
    append_log(&process, *out_error);
    return false;
  }

  process.view.running = true;
  process.view.ever_started = true;
  process.view.pid = process.process->process_id();
  process.view.exit_code = 0;

  append_log(&process, "Process started.");
  append_log(&process, "Command: " + process.command);
  append_log(&process, "Working directory: " + process.workdir);
  if (process.view.tty) {
    append_log(&process, "TTY mode enabled. Use GUI attach controls. Detach with Ctrl-] in attached terminal.");
  }

  return true;
}

void ProcessManager::capture_output(ManagedProcess* process) {
  std::string chunk;
  while (process->process->read_available(&chunk)) {
    append_log(process, chunk);
    chunk.clear();
  }
}

void ProcessManager::append_log(ManagedProcess* process, const std::string& text) {
  for (char c : text) {
    if (c == '\r') {
      process->pending_cr = true;
      continue;
    }

    if (c == '\n') {
      process->pending_cr = false;
      process->view.logs.push_back(process->pending_line);
      process->pending_line.clear();
      process->view.live_line.clear();
      continue;
    }

    if (process->pending_cr) {
      process->pending_cr = false;
      process->pending_line.clear();
      process->view.live_line.clear();
    }

    process->pending_line.push_back(c);
  }

  process->view.live_line = process->pending_line;

  enforce_log_limit(process);
}

void ProcessManager::poll_external_attach() {
#ifdef _WIN32
  return;
#else
  if (!_external_attach.active) {
    return;
  }
  if (_external_attach.process_index >= _processes.size()) {
    close_external_attach("External attach session closed.");
    return;
  }

  auto& process = _processes[_external_attach.process_index];
  if (!process.view.running) {
    close_external_attach("Target process exited.");
    return;
  }

  if (_external_attach.client_fd < 0) {
    const int client_fd = accept(_external_attach.listen_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      const int flags = fcntl(client_fd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
      }
      _external_attach.client_fd = client_fd;
      process.externally_attached = true;
      append_log(&process, "External attach connected.");
    }
  }

  if (_external_attach.client_fd < 0) {
    return;
  }

  std::string chunk;
  while (process.process->read_available(&chunk)) {
    append_log(&process, chunk);
    const ssize_t written = write(_external_attach.client_fd, chunk.data(), chunk.size());
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close_external_attach("External attach disconnected.");
      return;
    }
    chunk.clear();
  }

  char input[512];
  const ssize_t bytes = read(_external_attach.client_fd, input, sizeof(input));
  if (bytes == 0) {
    close_external_attach("External attach disconnected.");
    return;
  }
  if (bytes > 0) {
    std::string payload(input, static_cast<std::size_t>(bytes));
    std::string ignored;
    (void)send_input(_external_attach.process_index, payload, &ignored);
  } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    close_external_attach("External attach disconnected.");
    return;
  }
#endif
}

void ProcessManager::close_external_attach(const std::string& reason) {
#ifdef _WIN32
  (void)reason;
  return;
#else
  if (!_external_attach.active) {
    return;
  }

  if (_external_attach.process_index < _processes.size()) {
    auto& process = _processes[_external_attach.process_index];
    process.externally_attached = false;
    if (!reason.empty()) {
      append_log(&process, reason);
    }
  }

  if (_external_attach.client_fd >= 0) {
    close(_external_attach.client_fd);
  }
  if (_external_attach.listen_fd >= 0) {
    close(_external_attach.listen_fd);
  }
  if (!_external_attach.socket_path.empty()) {
    unlink(_external_attach.socket_path.c_str());
  }

  _external_attach = ExternalAttachSession{};
#endif
}

void ProcessManager::enforce_log_limit(ManagedProcess* process) {
  while (process->view.logs.size() > kMaxLogLines) {
    process->view.logs.pop_front();
  }
}
