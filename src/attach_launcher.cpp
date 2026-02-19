#include "attach_launcher.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string shell_single_quote(const std::string& input) {
  std::string quoted = "'";
  for (char ch : input) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  return quoted;
}

std::string applescript_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    if (ch == '\\') {
      out += "\\\\";
    } else if (ch == '"') {
      out += "\\\"";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace

bool launch_attach_terminal(const std::string& executable_path, const std::string& socket_path, std::string* out_error) {
#ifdef _WIN32
  (void)executable_path;
  (void)socket_path;
  *out_error = "External attach terminal is currently implemented only on POSIX.";
  return false;
#elif defined(__APPLE__)
  const std::string command = shell_single_quote(executable_path) + " --attach-socket " + shell_single_quote(socket_path);
  const std::string escaped = applescript_escape(command);
  std::string osascript = "osascript -e \"tell application \\\"Terminal\\\" to activate\" "
                          "-e \"tell application \\\"Terminal\\\" to do script \\\"" +
                          escaped + "\\\"\"";

  if (std::system(osascript.c_str()) != 0) {
    *out_error = "Failed to open macOS Terminal for attach.";
    return false;
  }
  return true;
#else
  const std::string command = shell_single_quote(executable_path) + " --attach-socket " + shell_single_quote(socket_path);
  const std::string wrapped = "sh -lc " + shell_single_quote(command);
  const std::vector<std::string> candidates = {
      "x-terminal-emulator -e " + wrapped,
      "gnome-terminal -- " + wrapped,
      "konsole -e " + wrapped,
      "xterm -e " + wrapped,
  };

  for (const auto& candidate : candidates) {
    if (std::system(candidate.c_str()) == 0) {
      return true;
    }
  }

  *out_error = "Failed to open terminal window for attach.";
  return false;
#endif
}
