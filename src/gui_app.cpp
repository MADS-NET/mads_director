#include "gui_app.hpp"

#include "config.hpp"
#include "process_manager.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace {

std::string runtime_label(const ProcessRuntimeView& view) {
  if (view.running) {
    if (view.pid > 0) {
      return "running - " + std::to_string(view.pid);
    }
    return "running";
  }
  if (!view.enabled && !view.ever_started) {
    return "autostart disabled";
  }
  if (!view.ever_started) {
    return "pending";
  }
  return "exited(" + std::to_string(view.exit_code) + ")";
}

std::string process_list_label(const ProcessRuntimeView& view) {
  if (view.pid > 0) {
    return view.name + "  [pid " + std::to_string(view.pid) + "]";
  }
  return view.name + "  [pid -]";
}

std::string format_bytes(std::uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }

  char buffer[64];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, kUnits[unit]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
  }
  return std::string(buffer);
}

std::size_t clamp_selected(ProcessManager* manager, std::size_t selected) {
  if (manager->process_count() == 0) {
    return 0;
  }
  return std::min(selected, manager->process_count() - 1);
}

struct AnsiStyleState {
  ImVec4 color = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
};

ImVec4 ansi_color(int code) {
  switch (code) {
    case 30:
      return ImVec4(0.00f, 0.00f, 0.00f, 1.0f);
    case 31:
      return ImVec4(0.88f, 0.27f, 0.27f, 1.0f);
    case 32:
      return ImVec4(0.35f, 0.82f, 0.38f, 1.0f);
    case 33:
      return ImVec4(0.94f, 0.83f, 0.36f, 1.0f);
    case 34:
      return ImVec4(0.43f, 0.61f, 0.93f, 1.0f);
    case 35:
      return ImVec4(0.82f, 0.47f, 0.89f, 1.0f);
    case 36:
      return ImVec4(0.34f, 0.82f, 0.84f, 1.0f);
    case 37:
      return ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    case 90:
      return ImVec4(0.42f, 0.42f, 0.42f, 1.0f);
    case 91:
      return ImVec4(1.00f, 0.46f, 0.46f, 1.0f);
    case 92:
      return ImVec4(0.52f, 1.00f, 0.58f, 1.0f);
    case 93:
      return ImVec4(1.00f, 0.93f, 0.52f, 1.0f);
    case 94:
      return ImVec4(0.56f, 0.73f, 1.00f, 1.0f);
    case 95:
      return ImVec4(0.92f, 0.60f, 1.00f, 1.0f);
    case 96:
      return ImVec4(0.52f, 1.00f, 1.00f, 1.0f);
    case 97:
      return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    default:
      return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
  }
}

void apply_sgr_code(int code, AnsiStyleState* state) {
  if (code == 0 || code == 39) {
    state->color = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    return;
  }
  if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
    state->color = ansi_color(code);
  }
}

void render_ansi_line(const std::string& line, AnsiStyleState* state) {
  std::string chunk;
  bool first_chunk = true;

  auto flush_chunk = [&]() {
    if (chunk.empty()) {
      return;
    }
    if (!first_chunk) {
      ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, state->color);
    ImGui::TextUnformatted(chunk.c_str());
    ImGui::PopStyleColor();
    chunk.clear();
    first_chunk = false;
  };

  std::size_t i = 0;
  while (i < line.size()) {
    if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
      flush_chunk();
      i += 2;
      std::string code_text;
      while (i < line.size() && (line[i] < '@' || line[i] > '~')) {
        code_text.push_back(line[i]);
        ++i;
      }
      if (i >= line.size()) {
        break;
      }
      const char command = line[i];
      ++i;

      if (command != 'm') {
        if (command == 'K') {
          chunk.clear();
        }
        continue;
      }

      if (code_text.empty()) {
        apply_sgr_code(0, state);
        continue;
      }

      std::size_t start = 0;
      while (start <= code_text.size()) {
        const std::size_t sep = code_text.find(';', start);
        const std::size_t end = (sep == std::string::npos) ? code_text.size() : sep;
        const std::string token = code_text.substr(start, end - start);
        if (token.empty()) {
          apply_sgr_code(0, state);
        } else {
          apply_sgr_code(std::atoi(token.c_str()), state);
        }
        if (sep == std::string::npos) {
          break;
        }
        start = sep + 1;
      }
      continue;
    }

    chunk.push_back(line[i]);
    ++i;
  }

  flush_chunk();
  if (first_chunk) {
    ImGui::TextUnformatted("");
  }
}

std::string strip_ansi_sequences(const std::string& text) {
  std::string out;
  out.reserve(text.size());

  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && (text[i] < '@' || text[i] > '~')) {
        ++i;
      }
      if (i < text.size()) {
        ++i;
      }
      continue;
    }
    out.push_back(text[i]);
    ++i;
  }

  return out;
}

std::string compose_plain_log_text(const ProcessRuntimeView& view) {
  std::string text;
  for (const auto& line : view.logs) {
    text += strip_ansi_sequences(line);
    text.push_back('\n');
  }
  if (!view.live_line.empty()) {
    text += strip_ansi_sequences(view.live_line);
  }
  return text;
}

std::filesystem::path current_executable_path() {
#if defined(_WIN32)
  std::wstring buffer(512, L'\0');
  while (true) {
    const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return {};
  }
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
  if (!ec) {
    return canonical;
  }
  return std::filesystem::path(buffer.data());
#elif defined(__linux__)
  std::array<char, PATH_MAX> buffer {};
  const auto copied = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (copied <= 0) {
    return {};
  }
  buffer[static_cast<std::size_t>(copied)] = '\0';
  return std::filesystem::path(buffer.data());
#else
  return {};
#endif
}

std::filesystem::path executable_directory(const std::string& executable_path) {
  const std::filesystem::path runtime_path = current_executable_path();
  if (!runtime_path.empty() && runtime_path.has_parent_path()) {
    return runtime_path.parent_path();
  }

  const std::filesystem::path configured_path(executable_path);
  if (configured_path.has_parent_path()) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(configured_path, ec);
    if (!ec && absolute.has_parent_path()) {
      return absolute.parent_path();
    }
    return configured_path.parent_path();
  }

  return {};
}

}  // namespace

int GuiApp::run(ProcessManager* manager, const std::string& executable_path, const std::string& config_path) {
  if (!glfwInit()) {
    return 1;
  }

#if defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
  const char* glsl_version = "#version 130";
#else
  const char* glsl_version = "#version 150";
#endif
#if defined(__APPLE__)
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow* window = glfwCreateWindow(1280, 820, "director", nullptr, nullptr);
  if (window == nullptr) {
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Load logo from ../share/images/logo_white.png relative to the executable location.
  GLuint logo_texture = 0;
  float logo_display_w = 0.0f;
  float logo_display_h = 0.0f;
  {
    const std::filesystem::path exe_dir = executable_directory(executable_path);
    if (!exe_dir.empty()) {
      const std::filesystem::path logo_path =
          (exe_dir / ".." / "share" / "images" / "logo_white.png").lexically_normal();
      int w = 0, h = 0, channels = 0;
      unsigned char* data = stbi_load(logo_path.string().c_str(), &w, &h, &channels, 4);
      if (data != nullptr) {
        const float max_h = 30.0f;
        const float scale = max_h / static_cast<float>(h);
        logo_display_w = static_cast<float>(w) * scale;
        logo_display_h = max_h;
        glGenTextures(1, &logo_texture);
        glBindTexture(GL_TEXTURE_2D, logo_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
      }
    }
  }

  std::size_t selected = 0;
  std::string status_line;
  std::vector<std::string> send_buffers;
  std::vector<int> tty_modes;
  bool auto_scroll = true;
  bool shutdown_notice_shown = false;

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    if (glfwWindowShouldClose(window) && !shutdown_notice_shown) {
      status_line = "Waiting for all processes to exit before closing Director";
      shutdown_notice_shown = true;
    }

    manager->tick();
    manager->launch_ready_processes();
    selected = clamp_selected(manager, selected);

    if (send_buffers.size() != manager->process_count()) {
      send_buffers.resize(manager->process_count());
    }
    if (tty_modes.size() != manager->process_count()) {
      tty_modes.resize(manager->process_count(), 0);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::Begin("director", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    const float footer_height = ImGui::GetFrameHeightWithSpacing() * 2.4f;
    ImGui::BeginChild("main_content", ImVec2(0.0f, -footer_height), false);

    ImGui::Columns(2, "layout", true);
    ImGui::SetColumnWidth(0, 360.0f);

    ImGui::TextUnformatted("Processes");
    ImGui::SameLine();
    if (ImGui::Button("Reload file")) {
      DirectorConfig reloaded_config;
      std::string error;
      if (!load_config(config_path, &reloaded_config, &error)) {
        status_line = "Reload failed: " + error;
      } else if (!manager->reload(reloaded_config, &error)) {
        status_line = "Reload failed: " + error;
      } else {
        selected = clamp_selected(manager, selected);
        status_line = "Configuration reloaded.";
      }
    }
    ImGui::Separator();

    if (ImGui::BeginTable("process_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
      ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.68f);
      ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 0.32f);

      for (std::size_t i = 0; i < manager->process_count(); ++i) {
        const auto* view = manager->process_at(i);
        if (view == nullptr) {
          continue;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool is_selected = (i == selected);
        const std::string label = process_list_label(*view);
        if (ImGui::Selectable(label.c_str(), is_selected)) {
          selected = i;
        }
        ImGui::TextDisabled("%s", runtime_label(*view).c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(static_cast<int>(i));
        const float max_cpu = std::max(100.0f, static_cast<float>(view->cpu_count) * 100.0f);
        if (view->cpu_percent_per_core_history.empty()) {
          float zero = 0.0f;
          ImGui::PlotLines("##cpu_sparkline", &zero, 1, 0, nullptr, 0.0f, max_cpu, ImVec2(-1.0f, 36.0f));
        } else {
          ImGui::PlotLines("##cpu_sparkline", view->cpu_percent_per_core_history.data(),
                           static_cast<int>(view->cpu_percent_per_core_history.size()), 0, nullptr, 0.0f, max_cpu,
                           ImVec2(-1.0f, 36.0f));
        }
        ImGui::Text("%.1f%%/core", view->cpu_percent_per_core);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    ImGui::NextColumn();

    const auto* view = manager->process_at(selected);
    if (view != nullptr) {
      ImGui::Text("Selected: %s", view->name.c_str());
      ImGui::Text("State: %s", runtime_label(*view).c_str());
      ImGui::Text("Enabled: %s", view->enabled ? "true" : "false");
      ImGui::Text("Threads: %u", view->thread_count);
      ImGui::Text("Memory: %s", format_bytes(view->ram_bytes).c_str());
      ImGui::Text("CPU/Core: %.1f%%", view->cpu_percent_per_core);
      std::array<char, 4096> command_buffer{};
      const std::size_t command_copy = std::min(view->command.size(), command_buffer.size() - 1);
      view->command.copy(command_buffer.data(), command_copy);
      command_buffer[command_copy] = '\0';

      ImGui::TextUnformatted("Command");
      ImGui::SetNextItemWidth(-120.0f);
      ImGui::InputText("##selected_command", command_buffer.data(), command_buffer.size(),
                       ImGuiInputTextFlags_ReadOnly);
      ImGui::SameLine();
      if (ImGui::Button("Copy Command")) {
        ImGui::SetClipboardText(view->command.c_str());
        status_line = "Command copied to clipboard.";
      }
      ImGui::Spacing();

      if (!view->enabled) {
        ImGui::TextUnformatted("Autostart is disabled (enabled=false). You can still start it manually.");
      }
      if (ImGui::Button("Start")) {
        std::string error;
        status_line = manager->start_process(selected, &error) ? "Process started." : error;
      }
      ImGui::SameLine();
      if (ImGui::Button("Stop")) {
        std::string error;
        status_line = manager->stop_process(selected, &error) ? "Process stopped." : error;
      }
      ImGui::SameLine();
      if (ImGui::Button("Restart")) {
        std::string error;
        status_line = manager->restart_process(selected, &error) ? "Process restarted." : error;
      }
      ImGui::SameLine();
#ifdef _WIN32
      ImGui::BeginDisabled();
      ImGui::Button("Send SIGINFO");
      ImGui::EndDisabled();
#else
      if (ImGui::Button("Send SIGINFO")) {
        std::string error;
        status_line = manager->send_siginfo(selected, &error) ? "SIGINFO sent." : error;
      }
#endif

      if (view->tty) {
        ImGui::Spacing();
        ImGui::TextUnformatted("TTY Interaction");

        ImGui::RadioButton("Use send input box", &tty_modes[selected], 0);
        ImGui::SameLine();
        ImGui::RadioButton("Attach to running process", &tty_modes[selected], 1);

        if (tty_modes[selected] == 1) {
          ImGui::TextUnformatted("Open a new terminal and attach it to this running process.");
          if (ImGui::Button("Open Attached Terminal")) {
            std::string error;
            status_line = manager->open_external_attach(selected, executable_path, &error)
                              ? "Opening attached terminal..."
                              : error;
          }
        }
      }

      if (!view->tty || tty_modes[selected] == 0) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Send Input");

        std::array<char, 1024> input_buffer{};
        const std::string& current = send_buffers[selected];
        const std::size_t copy_size = std::min(current.size(), input_buffer.size() - 1);
        current.copy(input_buffer.data(), copy_size);
        input_buffer[copy_size] = '\0';

        if (ImGui::InputText("##send_input", input_buffer.data(), input_buffer.size())) {
          send_buffers[selected] = input_buffer.data();
        }

        ImGui::SameLine();
        if (ImGui::Button("Send")) {
          std::string error;
          std::string payload = send_buffers[selected];
#ifdef _WIN32
          if (!view->tty) {
            payload += "\n";
          }
#else
          payload += "\n";
#endif
          status_line = manager->send_input(selected, payload, &error) ? "Input sent." : error;
        }
      }

      ImGui::Spacing();
      ImGui::Checkbox("Auto-scroll logs", &auto_scroll);
      if (ImGui::BeginTabBar("logs_tabs")) {
        if (ImGui::BeginTabItem("Colorized")) {
          ImGui::BeginChild("logs_color", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
          AnsiStyleState ansi_state;
          for (const auto& line : view->logs) {
            render_ansi_line(line, &ansi_state);
          }
          if (!view->live_line.empty()) {
            render_ansi_line(view->live_line, &ansi_state);
          }
          if (auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
            ImGui::SetScrollHereY(1.0f);
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Selectable / Copyable")) {
          std::string plain_log = compose_plain_log_text(*view);
          std::vector<char> plain_buffer(plain_log.begin(), plain_log.end());
          plain_buffer.push_back('\0');

          if (ImGui::Button("Copy Log")) {
            ImGui::SetClipboardText(plain_log.c_str());
            status_line = "Log copied to clipboard.";
          }
          ImGui::Separator();
          ImGui::BeginChild("logs_plain_container", ImVec2(0.0f, 0.0f), false);
          const ImVec2 avail = ImGui::GetContentRegionAvail();
          ImGui::InputTextMultiline("##plain_log", plain_buffer.data(), plain_buffer.size(), avail,
                                    ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }

    ImGui::Columns(1);
    ImGui::EndChild();
    ImGui::Separator();
    if (logo_texture != 0) {
      ImGui::Image(static_cast<ImTextureID>(logo_texture), ImVec2(logo_display_w, logo_display_h));
      ImGui::SameLine();
      ImGui::BeginGroup();
    }
    ImGui::Text("Status: %s", status_line.empty() ? "ready" : status_line.c_str());
    const auto* selected_view = manager->process_at(selected);
    ImGui::TextWrapped("Command: %s",
                       (selected_view == nullptr || selected_view->command.empty()) ? "<none>"
                                                                                     : selected_view->command.c_str());
    if (logo_texture != 0) {
      ImGui::EndGroup();
    }

    ImGui::End();

    ImGui::Render();
    int display_width = 0;
    int display_height = 0;
    glfwGetFramebufferSize(window, &display_width, &display_height);
    glViewport(0, 0, display_width, display_height);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  manager->stop_all();

  if (logo_texture != 0) {
    glDeleteTextures(1, &logo_texture);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
