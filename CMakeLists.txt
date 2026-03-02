cmake_minimum_required(VERSION 3.20)

find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" tag --sort=-creatordate
  OUTPUT_VARIABLE DIRECTOR_GIT_TAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE DIRECTOR_GIT_TAGS_RESULT
)

if(DIRECTOR_GIT_TAGS_RESULT EQUAL 0 AND NOT DIRECTOR_GIT_TAGS STREQUAL "")
  string(REPLACE "\n" ";" DIRECTOR_GIT_TAG_LIST "${DIRECTOR_GIT_TAGS}")
  list(GET DIRECTOR_GIT_TAG_LIST 0 DIRECTOR_LATEST_TAG)
  string(REGEX REPLACE "^[vV]" "" DIRECTOR_VERSION "${DIRECTOR_LATEST_TAG}")
  string(REGEX REPLACE "-.*$" "" DIRECTOR_VERSION "${DIRECTOR_VERSION}")

  if(DIRECTOR_VERSION STREQUAL "")
    message(FATAL_ERROR "Latest Git tag '${DIRECTOR_LATEST_TAG}' produced an empty normalized version.")
  endif()
else()
  if(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    message(FATAL_ERROR "Could not determine version from Git tags on a tag build.")
  endif()
  set(DIRECTOR_VERSION "0.0.0")
  message(WARNING "Could not determine version from Git tags; using fallback version '${DIRECTOR_VERSION}'.")
endif()

project(director VERSION ${DIRECTOR_VERSION} LANGUAGES CXX)
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
  find_program(MADS_EXECUTABLE mads)
  if(MADS_EXECUTABLE)
    execute_process(
      COMMAND "${MADS_EXECUTABLE}" -p
      OUTPUT_VARIABLE MADS_INSTALL_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE MADS_INSTALL_PREFIX_RESULT
    )

    if(MADS_INSTALL_PREFIX_RESULT EQUAL 0 AND NOT MADS_INSTALL_PREFIX STREQUAL "")
      set(CMAKE_INSTALL_PREFIX "${MADS_INSTALL_PREFIX}" CACHE PATH "Install path prefix." FORCE)
    else()
      message(WARNING "Could not determine install prefix from 'mads -p'; using default CMAKE_INSTALL_PREFIX='${CMAKE_INSTALL_PREFIX}'.")
    endif()
  else()
    message(WARNING "Command 'mads' not found; using default CMAKE_INSTALL_PREFIX='${CMAKE_INSTALL_PREFIX}'.")
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

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG f1c79c0
)

FetchContent_MakeAvailable(glfw imgui stb)

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

target_include_directories(${DIRECTOR_EXE_NAME} PRIVATE src ${stb_SOURCE_DIR})

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
endif()

target_compile_definitions(${DIRECTOR_EXE_NAME} PRIVATE
  GL_SILENCE_DEPRECATION
  DIRECTOR_CMD_PREFIX="${MADS_CMD_PREFIX}"
)

install(TARGETS ${DIRECTOR_EXE_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "MADS")
set(PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION "${PACKAGE_VERSION}")

if(WIN32)
  # Inno Setup packaging requires a relative staging prefix. Absolute prefixes
  # like "C:/..." break CPack temporary directory generation.
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_GENERATOR "INNOSETUP")
  set(CPACK_INNOSETUP_ARCHITECTURE "x64")

  # Map CMAKE_INSTALL_PREFIX (possibly from `mads -p`) to Inno Setup defaults.
  file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" DIRECTOR_WIN_INSTALL_PREFIX)
  if(DIRECTOR_WIN_INSTALL_PREFIX MATCHES "^([A-Za-z]:)(/.*)$")
    set(DIRECTOR_WIN_INSTALL_DRIVE "${CMAKE_MATCH_1}")
    set(DIRECTOR_WIN_INSTALL_PATH "${CMAKE_MATCH_2}")
    string(REGEX REPLACE "/+$" "" DIRECTOR_WIN_INSTALL_PATH "${DIRECTOR_WIN_INSTALL_PATH}")

    get_filename_component(DIRECTOR_WIN_INSTALL_BASENAME "${DIRECTOR_WIN_INSTALL_PATH}" NAME)
    get_filename_component(DIRECTOR_WIN_INSTALL_PARENT "${DIRECTOR_WIN_INSTALL_PATH}" DIRECTORY)

    if(DIRECTOR_WIN_INSTALL_PARENT STREQUAL "")
      set(CPACK_INNOSETUP_INSTALL_ROOT "${DIRECTOR_WIN_INSTALL_DRIVE}/")
    else()
      set(CPACK_INNOSETUP_INSTALL_ROOT "${DIRECTOR_WIN_INSTALL_DRIVE}${DIRECTOR_WIN_INSTALL_PARENT}")
    endif()

    if(DIRECTOR_WIN_INSTALL_BASENAME STREQUAL "")
      set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
    else()
      set(CPACK_PACKAGE_INSTALL_DIRECTORY "${DIRECTOR_WIN_INSTALL_BASENAME}")
    endif()
  else()
    message(WARNING "Could not parse Windows install prefix '${CMAKE_INSTALL_PREFIX}' for Inno Setup defaults; using CPack defaults.")
  endif()
elseif(APPLE)
  set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_GENERATOR "STGZ")
elseif(UNIX)
  set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MADS")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)
