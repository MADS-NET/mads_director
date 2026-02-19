cmake_minimum_required(VERSION 3.20)
project(director LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0
)

FetchContent_MakeAvailable(tomlplusplus)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
)

FetchContent_MakeAvailable(glfw imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${glfw_SOURCE_DIR}/include
)

target_link_libraries(imgui PUBLIC glfw)

add_executable(director
  src/main.cpp
  src/config.cpp
  src/attach_client.cpp
  src/attach_launcher.cpp
  src/process_manager.cpp
  src/gui_app.cpp
  src/platform_process_posix.cpp
  src/platform_process_windows.cpp
)

target_include_directories(director PRIVATE src)

find_package(OpenGL REQUIRED)

target_link_libraries(director PRIVATE
  tomlplusplus::tomlplusplus
  imgui
  glfw
  OpenGL::GL
)

if(MSVC)
  target_compile_options(director PRIVATE /W4 /permissive-)
else()
  target_compile_options(director PRIVATE -Wall -Wextra -Wpedantic)
endif()
