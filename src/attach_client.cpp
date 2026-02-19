#include "attach_client.hpp"

#include <string>

#ifdef _WIN32

int run_attach_client(const std::string& socket_path, std::string* out_error) {
  (void)socket_path;
  *out_error = "Attach client mode is currently implemented only on POSIX.";
  return 1;
}

#else

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

namespace {

class TerminalGuard {
 public:
  TerminalGuard() {
    _active = tcgetattr(STDIN_FILENO, &_saved_attrs) == 0;
    if (!_active) {
      return;
    }

    termios raw = _saved_attrs;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      _active = false;
      return;
    }

    _saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (_saved_stdin_flags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, _saved_stdin_flags | O_NONBLOCK);
    }
  }

  ~TerminalGuard() {
    if (!_active) {
      return;
    }

    if (_saved_stdin_flags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, _saved_stdin_flags);
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &_saved_attrs);
  }

  TerminalGuard(const TerminalGuard&) = delete;
  TerminalGuard& operator=(const TerminalGuard&) = delete;

 private:
  termios _saved_attrs{};
  int _saved_stdin_flags = -1;
  bool _active = false;
};

}  // namespace

int run_attach_client(const std::string& socket_path, std::string* out_error) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *out_error = std::string("socket failed: ") + std::strerror(errno);
    return 1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    close(fd);
    *out_error = "Attach socket path is too long.";
    return 1;
  }
  std::strcpy(addr.sun_path, socket_path.c_str());

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string error = std::string("connect failed: ") + std::strerror(errno);
    close(fd);
    *out_error = error;
    return 1;
  }

  const int saved_sock_flags = fcntl(fd, F_GETFL, 0);
  if (saved_sock_flags >= 0) {
    fcntl(fd, F_SETFL, saved_sock_flags | O_NONBLOCK);
  }

  TerminalGuard terminal_guard;
  const char* banner = "\r\n[external attach: Ctrl-] to detach]\r\n";
  (void)write(STDOUT_FILENO, banner, std::strlen(banner));

  while (true) {
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(STDIN_FILENO, &reads);
    FD_SET(fd, &reads);
    const int max_fd = fd > STDIN_FILENO ? fd : STDIN_FILENO;

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 150000;

    const int ready = select(max_fd + 1, &reads, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (FD_ISSET(fd, &reads)) {
      char buffer[4096];
      const ssize_t bytes = read(fd, buffer, sizeof(buffer));
      if (bytes <= 0) {
        break;
      }
      (void)write(STDOUT_FILENO, buffer, static_cast<std::size_t>(bytes));
    }

    if (FD_ISSET(STDIN_FILENO, &reads)) {
      char input[512];
      const ssize_t bytes = read(STDIN_FILENO, input, sizeof(input));
      if (bytes <= 0) {
        continue;
      }

      bool detach = false;
      for (ssize_t i = 0; i < bytes; ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == 0x1d) {
          detach = true;
          break;
        }
      }
      if (detach) {
        break;
      }

      const ssize_t written = write(fd, input, static_cast<std::size_t>(bytes));
      if (written <= 0) {
        break;
      }
    }
  }

  if (saved_sock_flags >= 0) {
    fcntl(fd, F_SETFL, saved_sock_flags);
  }

  close(fd);
  const char* footer = "\r\n[detached]\r\n";
  (void)write(STDOUT_FILENO, footer, std::strlen(footer));
  return 0;
}

#endif
