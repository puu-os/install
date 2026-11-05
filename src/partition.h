// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_PARTITION_H_
#define PUU_PARTITION_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"
#include "guid.h"
#include "status.h"

namespace puu {

struct EfiPartitionLayout {
  uint32_t partition_number = 0;
  uint64_t start_lba = 0;
  uint64_t size_lba = 0;
  EfiGuid unique_guid{};
};

class DiskLayout {
 public:
  static StatusOr<uint64_t> SwapBytes();
  static StatusOr<uint64_t> MinimumDriveBytes(const Config& config,
                                              bool hibernate);
  static StatusOr<DiskLayout> Plan(const std::filesystem::path& drive,
                                   const Config& config, bool hibernate);

  Status WriteGpt();

  [[nodiscard]] const std::filesystem::path& EfiDevice() const {
    return efi_device_;
  }
  [[nodiscard]] const std::filesystem::path& RootDevice() const {
    return root_device_;
  }
  [[nodiscard]] const std::filesystem::path& HomeDevice() const {
    return home_device_;
  }
  [[nodiscard]] bool HasSwap() const { return swap_device_.has_value(); }
  [[nodiscard]] StatusOr<std::filesystem::path> SwapDevice() const;
  [[nodiscard]] std::span<const std::filesystem::path> DevicePaths() const {
    return device_paths_;
  }

  void SetEfiPartUuid(std::string part_uuid) {
    efi_part_uuid_ = std::move(part_uuid);
  }
  void SetRootUuid(std::string uuid) { root_uuid_ = std::move(uuid); }
  void SetSwapUuid(std::string uuid) { swap_uuid_ = std::move(uuid); }
  void SetHomeUuid(std::string uuid) { home_uuid_ = std::move(uuid); }

  [[nodiscard]] StatusOr<std::string_view> RootUuid() const;
  [[nodiscard]] StatusOr<std::optional<std::string_view>> SwapUuid() const;
  [[nodiscard]] StatusOr<EfiPartitionLayout> EspLayout() const;
  [[nodiscard]] StatusOr<std::string> MergeFstab(
      std::string_view existing) const;

 private:
  DiskLayout() = default;

  std::filesystem::path drive_;
  uint64_t efi_bytes_ = 0;
  uint64_t root_min_bytes_ = 0;
  uint64_t swap_bytes_ = 0;
  uint64_t home_bytes_ = 0;
  std::filesystem::path efi_device_;
  std::filesystem::path root_device_;
  std::optional<std::filesystem::path> swap_device_;
  std::filesystem::path home_device_;
  std::vector<std::filesystem::path> device_paths_;
  std::string efi_part_uuid_;
  std::string root_uuid_;
  std::string swap_uuid_;
  std::string home_uuid_;
  std::optional<EfiPartitionLayout> esp_layout_;
};
}  // namespace puu

#endif  // PUU_PARTITION_H_
