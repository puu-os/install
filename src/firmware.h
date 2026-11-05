// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_FIRMWARE_H_
#define PUU_FIRMWARE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "config.h"
#include "context.h"
#include "guid.h"
#include "status.h"

namespace puu {

// Reads the firmware Secure Boot setup state from efivarfs.
//
// Never fails: an unreadable or absent variable is reported as
// SetupMode::State::kUnknown with a reason, because a non-EFI system is a
// legitimate configuration rather than an error.
SetupMode DetectSetupMode();

class EfiVariable {
 public:
  EfiVariable(std::string_view name, const EfiGuid& guid);

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }
  [[nodiscard]] bool Exists() const;
  [[nodiscard]] StatusOr<std::vector<uint8_t>> Read() const;
  [[nodiscard]] Status Write(uint32_t attributes,
                             std::span<const uint8_t> payload) const;

 private:
  std::filesystem::path path_;
};

// Creates a Boot#### variable for the installed system and puts it first in
// BootOrder.
//
// On failure to update BootOrder the new entry is removed and the previous
// order restored, so a partial change cannot leave the machine unbootable.
Status ManageBootEntry(const EfiPartitionLayout& layout, Arch arch,
                       std::string_view description);

// Requests firmware setup mode on the next reboot by setting
// EFI_OS_INDICATIONS_BOOT_TO_FW_UI in the OsIndications variable.
Status RequestRebootToFirmware();

}  // namespace puu

#endif  // PUU_FIRMWARE_H_
