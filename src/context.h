// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_CONTEXT_H_
#define PUU_CONTEXT_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "config.h"
#include "image.h"
#include "partition.h"
#include "secret_string.h"
namespace puu {

// Whether the installer should enrol its own Secure Boot keys. Chosen by the
// user on the Secure Boot screen.
enum class SecureBootMode : std::uint8_t { kEnroll, kSkip };

// Firmware Secure Boot setup state, as read from efivarfs.
struct SetupMode {
  enum class State : std::uint8_t {
    // Firmware is in Setup Mode; key enrolment is possible.
    kOn,
    // Firmware has already-enrolled keys; enrolment would fail.
    kOff,
    // Not an EFI system, or the variable could not be read.
    kUnknown,
  };

  State state = State::kUnknown;
  // Only meaningful when `state` is kOn: whether Secure Boot is also enabled.
  std::optional<bool> secure_boot_enabled;
  // Only meaningful when `state` is kUnknown: why detection failed.
  std::string reason;
};

// Everything a pipeline run needs. Assembled by the CLI, completed by the UI,
// then cloned into the worker thread.
struct Context {
  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = default;
  Context& operator=(Context&&) = default;

  [[nodiscard]] Context Clone() const {
    Context copy;
    copy.config = config;
    copy.source_image = source_image;
    copy.target_imgref = target_imgref;
    copy.image_version = image_version;
    copy.image_manifest = image_manifest;
    copy.drive = drive;
    copy.drive_id = drive_id;
    copy.drive_size = drive_size;
    copy.drive_label = drive_label;
    copy.username = username;
    copy.user_password = user_password.Clone();
    copy.user_shell = user_shell;
    copy.secure_boot = secure_boot;
    copy.hibernate = hibernate;
    copy.k3s_token = k3s_token.Clone();
    copy.k3s_token_file = k3s_token_file;
    copy.layout = layout;
    return copy;
  }

  Config config;

  // Local bootc source, if one was found or supplied. Absent means the image is
  // pulled from the network, which is only supported inside a container.
  std::optional<std::filesystem::path> source_image;
  ImageRef target_imgref;
  std::optional<std::string> image_version;
  ImageManifest image_manifest;

  std::filesystem::path drive;
  // Hardware identity and size recorded when the operator picked the drive.
  // Re-checked before the partition table is written, because kernel names are
  // reassigned when devices come and go.
  std::string drive_id;
  uint64_t drive_size = 0;
  std::optional<std::string> drive_label;

  std::optional<Username> username;
  SecretString user_password;
  UserShell user_shell = UserShell::kBash;

  SecureBootMode secure_boot = SecureBootMode::kEnroll;
  bool hibernate = false;
  SecretString k3s_token;
  std::optional<std::filesystem::path> k3s_token_file;

  std::optional<DiskLayout> layout;
};

}  // namespace puu

#endif  // PUU_CONTEXT_H_
