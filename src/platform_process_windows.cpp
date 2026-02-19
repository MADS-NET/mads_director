#include "platform_process.hpp"

#ifdef _WIN32

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace {

class WindowsProcess final : public PlatformProcess {
 public:
  ~WindowsProcess() override {
    stop();
    close_handles();
  }

  bool start(const std::string& command, const std::string& workdir, bool tty, std::string* out_error) override {
    stop();
    close_handles();

    _tty = tty;
    _exit_code = 0;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = nullptr;
    attributes.bInheritHandle = TRUE;

    if (!_tty) {
      if (!CreatePipe(&_stdout_read, &_stdout_write, &attributes, 0)) {
        *out_error = "CreatePipe for stdout failed.";
        return false;
      }
      if (!SetHandleInformation(_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for stdout failed.";
        return false;
      }

      if (!CreatePipe(&_stdin_read, &_stdin_write, &attributes, 0)) {
        *out_error = "CreatePipe for stdin failed.";
        return false;
      }
      if (!SetHandleInformation(_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for stdin failed.";
        return false;
      }
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);

    if (_tty) {
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    } else {
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdInput = _stdin_read;
      startup.hStdOutput = _stdout_write;
      startup.hStdError = _stdout_write;
    }

    PROCESS_INFORMATION info{};
    std::string run_command = command;

    if (!workdir.empty()) {
      std::error_code ec;
      const std::filesystem::path venv_activate =
          std::filesystem::path(workdir) / ".venv" / "Scripts" / "activate.bat";
      if (std::filesystem::exists(venv_activate, ec)) {
        run_command = "call \"" + venv_activate.string() + "\" && " + run_command;
      }
    }

    std::string cmdline = "cmd.exe /S /C \"" + run_command + "\"";

    const char* current_dir = workdir.empty() ? nullptr : workdir.c_str();
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, current_dir, &startup, &info)) {
      *out_error = "CreateProcess failed.";
      return false;
    }

    _process = info.hProcess;
    _thread = info.hThread;
    _process_id = info.dwProcessId;

    if (!_tty) {
      CloseHandle(_stdout_write);
      _stdout_write = nullptr;
      CloseHandle(_stdin_read);
      _stdin_read = nullptr;
    }

    _running = true;
    return true;
  }

  void stop() override {
    if (!_running || _process == nullptr) {
      return;
    }

    TerminateProcess(_process, 1);
    WaitForSingleObject(_process, 2000);

    DWORD code = 0;
    if (GetExitCodeProcess(_process, &code)) {
      _exit_code = static_cast<int>(code);
    }

    _running = false;
    _process_id = 0;
    close_handles();
  }

  bool is_running() override {
    if (!_running || _process == nullptr) {
      return false;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(_process, &code)) {
      _running = false;
      _process_id = 0;
      return false;
    }

    if (code == STILL_ACTIVE) {
      return true;
    }

    _exit_code = static_cast<int>(code);
    _running = false;
    _process_id = 0;
    close_handles();
    return false;
  }

  int exit_code() const override {
    return _exit_code;
  }

  int process_id() const override {
    if (_running && _process_id != 0) {
      return static_cast<int>(_process_id);
    }
    return -1;
  }

  bool read_available(std::string* out_chunk) override {
    if (_tty || _stdout_read == nullptr) {
      return false;
    }

    DWORD available = 0;
    if (!PeekNamedPipe(_stdout_read, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
      return false;
    }

    const DWORD read_size = available > 4096 ? 4096 : available;
    char buffer[4096];
    DWORD bytes_read = 0;

    if (!ReadFile(_stdout_read, buffer, read_size, &bytes_read, nullptr) || bytes_read == 0) {
      return false;
    }

    out_chunk->assign(buffer, buffer + bytes_read);
    return true;
  }

  bool write_input(const std::string& data) override {
    if (_tty || _stdin_write == nullptr) {
      return false;
    }

    DWORD written = 0;
    if (!WriteFile(_stdin_write, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
      return false;
    }

    return written == data.size();
  }

  bool attach_until_detach(std::string* out_error) override {
    *out_error = "Interactive attach is currently implemented only on POSIX terminals.";
    return false;
  }

 private:
  void close_if_open(HANDLE* handle) {
    if (*handle != nullptr) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  }

  void close_handles() {
    close_if_open(&_stdin_read);
    close_if_open(&_stdin_write);
    close_if_open(&_stdout_read);
    close_if_open(&_stdout_write);
    close_if_open(&_process);
    close_if_open(&_thread);
  }

  HANDLE _stdin_read = nullptr;
  HANDLE _stdin_write = nullptr;
  HANDLE _stdout_read = nullptr;
  HANDLE _stdout_write = nullptr;
  HANDLE _process = nullptr;
  HANDLE _thread = nullptr;
  DWORD _process_id = 0;

  int _exit_code = 0;
  bool _running = false;
  bool _tty = false;
};

}  // namespace

std::unique_ptr<PlatformProcess> create_platform_process() {
  return std::make_unique<WindowsProcess>();
}

#endif
