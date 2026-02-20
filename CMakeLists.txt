cmake_minimum_required(VERSION 3.20)
project(director LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MADS_CMD_PREFIX "mads" CACHE STRING "Optional command prefix for the director executable name.")
if(MADS_CMD_PREFIX STREQUAL "")
  set(DIRECTOR_EXE_NAME "director")
else()
  set(DIRECTOR_EXE_NAME "${MADS_CMD_PREFIX}-director")
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  execute_process(
    COMMAND mads -p
    OUTPUT_VARIABLE MADS_INSTALL_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE MADS_INSTALL_PREFIX_RESULT
  )

  if(MADS_INSTALL_PREFIX_RESULT EQUAL 0 AND NOT MADS_INSTALL_PREFIX STREQUAL "")
    set(CMAKE_INSTALL_PREFIX "${MADS_INSTALL_PREFIX}" CACHE PATH "Install path prefix." FORCE)
  else()
    message(WARNING "Could not determine install prefix from 'mads -p'; using default CMAKE_INSTALL_PREFIX='${CMAKE_INSTALL_PREFIX}'.")
  endif()
endif()

include(FetchContent)

if(UNIX AND NOT APPLE)
  option(
    DIRECTOR_ENABLE_WAYLAND
    "Enable GLFW Wayland backend (requires wayland-scanner and Wayland development packages)."
    OFF
  )
  set(GLFW_BUILD_X11 ON CACHE BOOL "Build GLFW with X11 backend" FORCE)
  set(GLFW_BUILD_WAYLAND ${DIRECTOR_ENABLE_WAYLAND} CACHE BOOL "Build GLFW with Wayland backend" FORCE)
endif()

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

add_executable(${DIRECTOR_EXE_NAME}
  src/main.cpp
  src/config.cpp
  src/attach_client.cpp
  src/attach_launcher.cpp
  src/process_manager.cpp
  src/gui_app.cpp
  src/platform_process_posix.cpp
  src/platform_process_windows.cpp
)

target_include_directories(${DIRECTOR_EXE_NAME} PRIVATE src)

find_package(OpenGL REQUIRED)

target_link_libraries(${DIRECTOR_EXE_NAME} PRIVATE
  tomlplusplus::tomlplusplus
  imgui
  glfw
  OpenGL::GL
)

if(MSVC)
  target_compile_options(${DIRECTOR_EXE_NAME} PRIVATE /W4 /permissive-)
else()
  target_compile_options(${DIRECTOR_EXE_NAME} PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(${DIRECTOR_EXE_NAME} PRIVATE GL_SILENCE_DEPRECATION)
endif()

install(TARGETS ${DIRECTOR_EXE_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
