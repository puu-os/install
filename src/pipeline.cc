// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#include "pipeline.h"

#include <fcntl.h>
#include <libmount/libmount.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <fmt/format.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "status.h"
#include "util.h"

namespace puu {
namespace {
struct MountContextDeleter {
  void operator()(libmnt_context* cxt) const {
    if (cxt != nullptr) {
      mnt_free_context(cxt);
    }
  }
};
using UniqueMountContext = std::unique_ptr<libmnt_context, MountContextDeleter>;

constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
    kChrootBindMounts = {
        {{"/proc", "proc"}, {"/sys", "sys"}, {"/dev", "dev"}, {"/run", "run"}}};

StatusOr<std::vector<std::filesystem::path>> TeardownMountPoints() {
  PUU_ASSIGN_OR_RETURN(const Mounts mounts, Mounts::Read());
  return mounts.Below(kTargetMount);
}

Status MountContextError(libmnt_context* cxt, int rc, std::string_view action,
                         const std::filesystem::path& target,
                         const std::filesystem::path& source = {}) {
  char error_buf[512] = {};
  mnt_context_get_excode(cxt, rc, error_buf, sizeof(error_buf));
  const int error = mnt_context_get_syscall_errno(cxt);
  const std::string desc = source.empty()
                               ? fmt::format("{} {}", action, target.string())
                               : fmt::format("{} {} at {}", action,
                                             source.string(), target.string());
  if (error_buf[0] != '\0') {
    return InternalError("failed to {}: {}", desc, error_buf);
  }
  if (error != 0) {
    return ErrnoToStatus(error, desc);
  }
  return InternalError("failed to {}: code {}", desc, rc);
}

}  // namespace

std::shared_ptr<spdlog::logger> InstallationLogger() {
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(kInstallationLogTarget));
  return logger ? logger : spdlog::default_logger();
}

Status EnterPrivateMountNamespace() {
  if (::unshare(CLONE_NEWNS) != 0) {
    const int error = errno;
    return ErrnoToStatus(error, "unshare(CLONE_NEWNS)");
  }

  // namespace, which is exactly what the new namespace is meant to prevent.
  if (::mount(nullptr, "/", nullptr, MS_PRIVATE | MS_REC, nullptr) != 0) {
    const int error = errno;
    return ErrnoToStatus(error, "mount / as private");
  }
  return absl::OkStatus();
}

Status MountTarget(const std::filesystem::path& source,
                   const std::filesystem::path& target,
                   std::string_view filesystem_type, std::string_view options) {
  PUU_RETURN_IF_ERROR(CreateDirectories(target));
  PUU_ASSIGN_OR_RETURN(const Mounts mounts, Mounts::Read());
  if (mounts.IsMountPoint(target)) {
    InstallationLogger()->info("{} is already mounted; reusing it",
                               target.string());
    return absl::OkStatus();
  }

  UniqueMountContext cxt(mnt_new_context());
  if (!cxt) {
    return InternalError("failed to allocate mount context");
  }
  mnt_context_force_unrestricted(cxt.get());
  if (mnt_context_set_source(cxt.get(), source.c_str()) != 0 ||
      mnt_context_set_target(cxt.get(), target.c_str()) != 0) {
    return InternalError("failed to configure mount of {} on {}",
                         source.string(), target.string());
  }
  if (!filesystem_type.empty()) {
    const std::string type(filesystem_type);
    if (mnt_context_set_fstype(cxt.get(), type.c_str()) != 0) {
      return InternalError("failed to set fstype {} for mount", type);
    }
  }
  if (!options.empty()) {
    const std::string opt(options);
    if (mnt_context_set_options(cxt.get(), opt.c_str()) != 0) {
      return InternalError("failed to set options {} for mount", opt);
    }
  }

  const int rc = mnt_context_mount(cxt.get());
  if (rc == 0) {
    return absl::OkStatus();
  }
  return MountContextError(cxt.get(), rc, "mount", target, source);
}

Status BindMount(const std::filesystem::path& source,
                 const std::filesystem::path& target) {
  return MountTarget(source, target, /*filesystem_type=*/"",
                     /*options=*/"rbind");
}

Status Unmount(const std::filesystem::path& target) {
  auto attempt = [](const std::filesystem::path& path, bool lazy) -> Status {
    UniqueMountContext cxt(mnt_new_context());
    if (!cxt) {
      return InternalError("failed to allocate mount context");
    }
    mnt_context_force_unrestricted(cxt.get());
    if (mnt_context_set_target(cxt.get(), path.c_str()) != 0) {
      return InternalError("failed to configure umount target {}",
                           path.string());
    }
    if (lazy) {
      mnt_context_enable_lazy(cxt.get(), 1);
    }
    const int rc = mnt_context_umount(cxt.get());
    if (rc == 0) {
      return absl::OkStatus();
    }
    const int error = mnt_context_get_syscall_errno(cxt.get());
    if (error == EINVAL || error == ENOENT) {
      return absl::OkStatus();
    }
    if (error == EBUSY && !lazy) {
      return absl::UnavailableError("busy");
    }
    return MountContextError(cxt.get(), rc, "unmount", path);
  };

  Status status = attempt(target, /*lazy=*/false);
  if (status.ok() || !absl::IsUnavailable(status)) {
    return status;
  }

  InstallationLogger()->warn("{} is busy, detaching lazily", target.string());
  return attempt(target, /*lazy=*/true);
}

Status MountChrootBinds(const std::filesystem::path& root) {
  for (const auto& [source, relative] : kChrootBindMounts) {
    PUU_RETURN_IF_ERROR(
        BindMount(std::filesystem::path(source), root / relative));
  }
  return absl::OkStatus();
}

Status CleanupMounts() {
  PUU_ASSIGN_OR_RETURN(const std::vector<std::filesystem::path> points,
                       TeardownMountPoints());
  Status first_error;
  for (const std::filesystem::path& point : points) {
    InstallationLogger()->info("unmounting {}", point.string());
    const Status status = Unmount(point);
    if (!status.ok() && first_error.ok()) {
      first_error = status;
    }
  }
  return first_error;
}

void Rollback() {
  try {
    const StatusOr<std::vector<std::filesystem::path>> points =
        TeardownMountPoints();
    if (!points.ok()) {
      return;
    }
    for (const std::filesystem::path& point : *points) {
      InstallationLogger()->info("rollback: unmounting {}", point.string());

      Unmount(point).IgnoreError();
    }
  } catch (const std::exception& error) {
    InstallationLogger()->error("rollback failed: {}", error.what());
  }
}

CleanupStack::~CleanupStack() {
  while (!cleanups_.empty()) {
    const std::function<void()> cleanup = std::move(cleanups_.back());
    cleanups_.pop_back();

    try {
      cleanup();
    } catch (const std::exception& error) {
      InstallationLogger()->error("cleanup failed: {}", error.what());
    } catch (...) {
      InstallationLogger()->error("cleanup failed");
    }
  }
}

void CleanupStack::Push(std::function<void()> cleanup) {
  cleanups_.push_back(std::move(cleanup));
}

void CleanupStack::Release() { cleanups_.clear(); }

StatusOr<std::vector<KernelArg>> InstallKernelArgs(const DiskLayout& layout,
                                                   const Config& config) {
  PUU_ASSIGN_OR_RETURN(const std::string_view root_uuid, layout.RootUuid());
  PUU_ASSIGN_OR_RETURN(const std::optional<std::string_view> swap_uuid,
                       layout.SwapUuid());
  PUU_ASSIGN_OR_RETURN(const Arch machine, HostArch());
  std::vector<KernelArg> args;
  args.reserve(config.kernel_args.size() + 5);
  PUU_ASSIGN_OR_RETURN(
      KernelArg root, KernelArg::Parse(fmt::format("root=UUID={}", root_uuid)));
  args.push_back(std::move(root));
  PUU_ASSIGN_OR_RETURN(KernelArg rw, KernelArg::Parse("rw"));
  args.push_back(std::move(rw));
  if (swap_uuid.has_value()) {
    PUU_ASSIGN_OR_RETURN(KernelArg resume, KernelArg::Parse(fmt::format(
                                               "resume=UUID={}", *swap_uuid)));
    args.push_back(std::move(resume));
    PUU_ASSIGN_OR_RETURN(KernelArg no_zram, KernelArg::Parse("systemd.zram=0"));
    args.push_back(std::move(no_zram));
  }
  for (const KernelArg& arg : config.kernel_args) {
    args.push_back(arg);
  }
  const auto configured =
      config.serial_console.find(std::string(ArchName(machine)));
  if (configured != config.serial_console.end()) {
    args.push_back(configured->second);
  }
  return args;
}

StatusOr<std::filesystem::path> DeploymentRoot(
    const std::filesystem::path& target) {
  const std::filesystem::path deploy = target / "ostree/deploy";
  std::error_code error;
  std::filesystem::directory_iterator os_entries(deploy, error);
  if (error) {
    return FilesystemError(
        error, fmt::format("read ostree deployments at {}", deploy.string()));
  }

  std::vector<std::filesystem::path> found;
  const std::filesystem::directory_iterator end;
  for (; os_entries != end; os_entries.increment(error)) {
    if (error) {
      return FilesystemError(
          error, fmt::format("read ostree deployments at {}", deploy.string()));
    }

    std::error_code skipped;
    std::filesystem::directory_iterator deployments(
        os_entries->path() / "deploy", skipped);
    if (skipped) {
      continue;
    }
    for (; deployments != end && !skipped; deployments.increment(skipped)) {
      const std::filesystem::path path = deployments->path();
      if (!IsDirectory(path)) {
        continue;
      }

      if (path.filename().string().ends_with(".origin")) {
        continue;
      }
      found.push_back(path);
    }
  }
  if (found.empty()) {
    return NotFoundError(
        "ostree did not report the installed deployment root under {}",
        deploy.string());
  }
  std::ranges::sort(found);
  return found.front();
}

}  // namespace puu
