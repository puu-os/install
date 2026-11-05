# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

# Directory scope keeps the warnings and sanitizers off the vendored
# dependencies.
if(PUU_SANITIZE)
  add_compile_options(-fsanitize=${PUU_SANITIZE})
  add_link_options(-fsanitize=${PUU_SANITIZE})
endif()
add_compile_options(${PUU_COMPILE_OPTIONS})
add_compile_definitions(PUU_INSTALLER_VERSION="${PROJECT_VERSION}")
if(ftxui_VERSION_MAJOR)
  add_compile_definitions(FTXUI_VERSION_MAJOR=${ftxui_VERSION_MAJOR})
endif()

add_executable(puu-installer
  app.cc
  block.cc
  cli.cc
  command.cc
  config.cc
  der.cc
  firmware.cc
  guid.cc
  image.cc
  k3s.cc
  logging.cc
  main.cc
  partition.cc
  passwd.cc
  pipeline.cc
  progress.cc
  runner.cc
  secret_string.cc
  secureboot.cc
  status.cc
  steps.cc
  timezone.cc
  util.cc
  widgets.cc)
target_include_directories(puu-installer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(puu-installer PRIVATE
  argtable3::argtable3
  yaml-cpp::yaml-cpp
  fmt::fmt
  ftxui::component
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  absl::base
  absl::status
  absl::statusor
  absl::strings
  absl::synchronization
  absl::time
  OpenSSL::SSL
  OpenSSL::Crypto
  PkgConfig::BLKID
  PkgConfig::UDEV
  PkgConfig::MOUNT
  Threads::Threads)
install(TARGETS puu-installer RUNTIME)
