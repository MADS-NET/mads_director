#include "config.hpp"

#include <toml++/toml.hpp>

#include <sstream>
#include <unordered_set>

namespace {

bool parse_process(const std::string& section_name, const toml::table& table, ProcessSpec* out_process,
                   std::string* out_error) {
  const auto command = table["command"].value<std::string>();

  if (section_name.empty()) {
    *out_error = "Process section name must be non-empty.";
    return false;
  }

  if (!command.has_value() || command->empty()) {
    *out_error = "Process '" + section_name + "' must provide a non-empty 'command' string.";
    return false;
  }

  ProcessSpec process;
  process.name = section_name;
  process.command = *command;

  if (const auto after = table["after"].value<std::string>(); after.has_value() && !after->empty()) {
    process.after = *after;
  }

  if (const auto scale = table["scale"].value<int64_t>(); scale.has_value()) {
    if (*scale < 1) {
      *out_error = "Process '" + process.name + "' has invalid 'scale'. Must be >= 1.";
      return false;
    }
    process.scale = static_cast<int>(*scale);
  }

  if (const auto relaunch = table["relaunch"].value<bool>(); relaunch.has_value()) {
    process.relaunch = *relaunch;
  }

  if (const auto tty = table["tty"].value<bool>(); tty.has_value()) {
    process.tty = *tty;
  }

  *out_process = std::move(process);
  return true;
}

}  // namespace

bool load_config(const std::string& path, DirectorConfig* out_config, std::string* out_error) {
  try {
    const auto parsed = toml::parse_file(path);

    DirectorConfig config;
    std::unordered_set<std::string> names;
    bool has_any_process = false;

    for (const auto& [key, node] : parsed) {
      const auto table = node.as_table();
      if (table == nullptr) {
        *out_error = "All top-level entries must be process tables like [api], [db], etc.";
        return false;
      }

      ProcessSpec process;
      const std::string process_name = std::string(key.str());
      if (!parse_process(process_name, *table, &process, out_error)) {
        return false;
      }
      has_any_process = true;

      if (!names.insert(process.name).second) {
        *out_error = "Duplicate process name detected: '" + process.name + "'.";
        return false;
      }

      config.processes.push_back(std::move(process));
    }

    if (!has_any_process) {
      *out_error = "Config must include at least one process section like [api].";
      return false;
    }

    for (const auto& process : config.processes) {
      if (!process.after.has_value()) {
        continue;
      }
      if (!names.contains(*process.after)) {
        *out_error = "Process '" + process.name + "' references unknown dependency in 'after': '" +
                     *process.after + "'.";
        return false;
      }
    }

    *out_config = std::move(config);
    return true;
  } catch (const toml::parse_error& error) {
    std::ostringstream stream;
    stream << "TOML parse error: " << error.description() << " at " << error.source().begin;
    *out_error = stream.str();
    return false;
  } catch (const std::exception& error) {
    *out_error = std::string("Config load failed: ") + error.what();
    return false;
  }
}
