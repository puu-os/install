// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_join.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include "block.h"
#include "command.h"
#include "config.h"
#include "context.h"
#include "firmware.h"
#include "image.h"
#include "k3s.h"
#include "partition.h"
#include "passwd.h"
#include "pipeline.h"
#include "progress.h"
#include "secureboot.h"
#include "status.h"
#include "step.h"
#include "util.h"

namespace puu {
namespace {

constexpr std::array<std::string_view, 4> kDependencies = {
    "bootc", "mke2fs", "mkfs.fat", "mkfs.xfs"};

constexpr std::chrono::seconds kDeviceTimeout{30};
constexpr std::chrono::seconds kHeartbeatInterval{30};

constexpr mode_t kPublicFileMode = 0644;
constexpr mode_t kSecretFileMode = 0600;

constexpr std::string_view kBootcStorageConf =
    "/run/puu-install/bootc-storage.conf";
constexpr std::string_view kBootcGraphRoot =
    "/run/puu-install/target/var/tmp/puu-bootc-storage/containers/storage";
constexpr std::string_view kContainersRunRoot = "/run/containers/storage";
constexpr std::string_view kHostVarTmp = "/var/tmp";

std::filesystem::path InTarget(const std::filesystem::path& path) {
  return std::filesystem::path(kTargetMount) / path.relative_path();
}

StatusOr<DiskLayout*> MutableLayout(Context& context) {
  if (!context.layout.has_value()) {
    return FailedPreconditionError("drive layout is not initialized");
  }
  return &*context.layout;
}

StatusOr<const DiskLayout*> GetLayout(const Context& context) {
  if (!context.layout.has_value()) {
    return FailedPreconditionError("drive layout is not initialized");
  }
  return &*context.layout;
}

Status MountEfi(const Runner& runner) {
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout,
                       GetLayout(runner.GetContext()));
  return MountTarget(layout->EfiDevice(), kTargetEfiMount, "vfat",
                     "fmask=0077,dmask=0077");
}

StatusOr<uint64_t> ValidateTargetDrive(const std::filesystem::path& drive,
                                       const Config& config, bool hibernate) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(drive, error);
  if (error) {
    return FilesystemError(
        error, fmt::format("inspect target drive {}", drive.string()));
  }
  if (!std::filesystem::is_block_file(status)) {
    return InvalidArgumentError("'{}' is not a block device", drive.string());
  }
  PUU_ASSIGN_OR_RETURN(const BusySources busy_sources, ReadBusySources());
  PUU_ASSIGN_OR_RETURN(const std::optional<BusyReason> busy,
                       DiskBusy(drive, busy_sources));
  if (busy.has_value()) {
    return FailedPreconditionError("'{}' is busy: {}", drive.string(),
                                   BusyReasonText(*busy));
  }
  PUU_ASSIGN_OR_RETURN(const uint64_t size, DriveSizeBytes(drive));
  PUU_ASSIGN_OR_RETURN(const uint64_t minimum,
                       DiskLayout::MinimumDriveBytes(config, hibernate));
  if (size < minimum) {
    return FailedPreconditionError(
        "'{}' is too small: {} detected, need at least {} for {}",
        drive.string(), HumanSize(size), HumanSize(minimum),
        hibernate ? "EFI, root, swap and home partitions"
                  : "EFI, root and home partitions");
  }
  return size;
}

// Confirms the disk behind `context.drive` is still the one the operator
// selected, then returns its size.
//
// The first call records the identity; later calls compare against it. A drive
// udev cannot identify is matched on size alone, which is weaker but still
// catches the common case of a swapped removable disk.
StatusOr<uint64_t> ValidateSelectedDrive(Context& context) {
  PUU_ASSIGN_OR_RETURN(
      const uint64_t size,
      ValidateTargetDrive(context.drive, context.config, context.hibernate));
  PUU_ASSIGN_OR_RETURN(const std::string id, DriveIdentity(context.drive));
  if (context.drive_size == 0) {
    context.drive_id = id;
    context.drive_size = size;
    return size;
  }
  if (id != context.drive_id || size != context.drive_size) {
    return FailedPreconditionError(
        "'{}' is no longer the disk that was selected; refusing to erase it. "
        "Restart the installer and choose the target again",
        context.drive.string());
  }
  return size;
}

StatusOr<ImageRef> SourceImageRef(const std::filesystem::path& source) {
  const std::string_view transport =
      IsDirectory(source) ? "dir" : "oci-archive";
  return ImageRef::Parse(fmt::format("{}:{}", transport, source.string()));
}

Status RunChrootedHelper(const Runner& runner,
                         const std::filesystem::path& helper,
                         std::string heartbeat, double progress_base,
                         double progress_span) {
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* target, runner.GetDeployment());
  const std::filesystem::path& deployment = target->Root();

  CleanupStack binds;
  binds.Push([deployment] {
    for (const std::string_view relative :
         {"run", "dev", "sys", "proc", "var"}) {
      Unmount(deployment / relative).IgnoreError();
    }
  });

  // Without this the helper writes into the deployment's empty /var, which
  // ostree-prepare-root shadows with the stateroot's /var at boot, so every
  // change would be silently discarded.
  const std::filesystem::path var = target->InDeployment("/var");
  PUU_RETURN_IF_ERROR(CreateDirectories(var));
  PUU_RETURN_IF_ERROR(BindMount(target->InState("/var"), var));
  PUU_RETURN_IF_ERROR(MountChrootBinds(deployment));

  MarkerProgress progress;
  Command command(helper.string());
  command.Chroot(deployment)
      .Heartbeat(kHeartbeatInterval, std::move(heartbeat))
      .Track(
          &progress, [&runner, progress_base, progress_span](double fraction) {
            runner.ReportProgress(
                progress_base + progress_span * std::clamp(fraction, 0.0, 1.0));
          });
  return command.Run();
}

StatusOr<std::vector<std::string>> ParseManifestRefs(
    const std::filesystem::path& manifest) {
  if (!Exists(manifest)) {
    return std::vector<std::string>();
  }

  PUU_ASSIGN_OR_RETURN(const std::string content, ReadFile(manifest));
  std::vector<std::string> refs;
  std::string_view remaining(content);
  while (!remaining.empty()) {
    const size_t newline = remaining.find('\n');
    std::string_view line = remaining.substr(0, newline);
    remaining = newline == std::string_view::npos
                    ? std::string_view()
                    : remaining.substr(newline + 1);
    const size_t comment = line.find('#');
    if (comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    const std::string_view trimmed = Trim(line);
    if (!trimmed.empty()) {
      refs.emplace_back(trimmed);
    }
  }
  return refs;
}

Status SeedStore(const Runner& runner, const SeedConfig& seed,
                 double progress_base, double progress_span) {
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  const std::string_view name = seed.name.Value();
  if (seed.live_store.has_value() && IsDirectory(*seed.live_store)) {
    PUU_RETURN_IF_ERROR(
        CopyTree(*seed.live_store, deployment->InState(seed.target)));
    InstallationLogger()->info("{}: copied the store from the live image",
                               name);
    return absl::OkStatus();
  }
  if (seed.manifest_path.has_value()) {
    const std::filesystem::path manifest =
        deployment->InDeployment(*seed.manifest_path);
    PUU_ASSIGN_OR_RETURN(const std::vector<std::string> refs,
                         ParseManifestRefs(manifest));
    if (refs.empty()) {
      InstallationLogger()->info("{}: no refs listed in {}", name,
                                 seed.manifest_path->string());
      return absl::OkStatus();
    }
  }
  if (!seed.sync_helper.has_value()) {
    InstallationLogger()->info("{}: nothing to pre-stage", name);
    return absl::OkStatus();
  }
  const std::filesystem::path helper =
      deployment->InDeployment(*seed.sync_helper);
  if (!IsRegularFile(helper)) {
    InstallationLogger()->info("{}: nothing to pre-stage", name);
    return absl::OkStatus();
  }
  InstallationLogger()->info("{}: running {}", name,
                             seed.sync_helper->string());
  PUU_RETURN_IF_ERROR(RunChrootedHelper(
      runner, *seed.sync_helper,
      fmt::format("{} pre-seed still running; downloading content", name),
      progress_base, progress_span));
  InstallationLogger()->info("{}: pre-staged", name);
  return absl::OkStatus();
}

StatusOr<std::filesystem::path> SystemdBootSource(
    const TargetDeployment& deployment) {
  const std::array<std::filesystem::path, 2> directories = {
      deployment.InDeployment("/usr/lib/systemd/boot/efi"),
      std::filesystem::path("/usr/lib/systemd/boot/efi")};
  for (const std::filesystem::path& directory : directories) {
    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);
    if (error) {
      continue;
    }
    const std::filesystem::directory_iterator end;
    for (; entries != end && !error; entries.increment(error)) {
      std::filesystem::path path = entries->path();
      const std::string name = path.filename().string();
      std::string extension = path.extension().string();
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return std::tolower(c); });
      if (name.starts_with("systemd-boot") && extension == ".efi") {
        return path;
      }
    }
  }
  return NotFoundError("systemd-boot EFI binary not found");
}

Status StepDependencies(Runner& runner) {
  const Config& config = runner.GetContext().config;
  std::vector<KernelArg> configured_args = config.kernel_args;
  PUU_ASSIGN_OR_RETURN(const Arch arch, HostArch());
  const auto console = config.serial_console.find(std::string(ArchName(arch)));
  if (console != config.serial_console.end()) {
    configured_args.push_back(console->second);
  }
  for (const KernelArg& arg : configured_args) {
    const std::string_view value = arg.Value();
    std::string key(value.substr(0, value.find('=')));
    std::replace(key.begin(), key.end(), '-', '_');
    if (key == "resume" || key == "resume_offset" || key == "noresume" ||
        key == "systemd.zram") {
      return InvalidArgumentError(
          "kernel argument '{}' conflicts with installer-managed swap; "
          "use the Hibernate option instead",
          value);
    }
  }
  std::vector<std::string_view> required(kDependencies.begin(),
                                         kDependencies.end());
  if (runner.GetContext().hibernate) {
    required.emplace_back("mkswap");
  }
  std::vector<std::string> missing;
  for (const std::string_view dependency : required) {
    if (!FindInPath(dependency).has_value()) {
      missing.emplace_back(dependency.data(), dependency.size());
    }
  }
  if (!missing.empty()) {
    for (const std::string_view dependency : missing) {
      InstallationLogger()->info("missing required tool: {}", dependency);
    }
    return FailedPreconditionError("missing dependencies: {}",
                                   absl::StrJoin(missing, ", "));
  }
  InstallationLogger()->info("all {} dependencies present", required.size());
  return absl::OkStatus();
}

Status StepSetupMode(Runner& runner) {
  if (runner.GetContext().secure_boot == SecureBootMode::kSkip) {
    InstallationLogger()->info(
        "Secure Boot disabled; skipping firmware setup check");
    return absl::OkStatus();
  }
  const SetupMode mode = DetectSetupMode();
  switch (mode.state) {
    case SetupMode::State::kUnknown:

      runner.GetContext().secure_boot = SecureBootMode::kSkip;
      InstallationLogger()->info(
          "Secure Boot Setup Mode could not be detected; continuing without "
          "Secure Boot");
      if (!mode.reason.empty()) {
        InstallationLogger()->info("  reason: {}", mode.reason);
      }
      return absl::OkStatus();
    case SetupMode::State::kOff:
      return FailedPreconditionError(
          "Secure Boot Setup Mode is off. Reboot to firmware setup or "
          "continue without Secure Boot.");
    case SetupMode::State::kOn:
      InstallationLogger()->info(
          "firmware reports Setup Mode on; Secure Boot is {}",
          mode.secure_boot_enabled.value_or(false) ? "enabled" : "disabled");
      return absl::OkStatus();
  }
  return InternalError("unhandled Secure Boot setup mode");
}

Status StepCleanup(Runner& /*runner*/) { return CleanupMounts(); }

Status StepValidateDrive(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(const uint64_t size, ValidateSelectedDrive(context));
  if (context.source_image.has_value() &&
      !IsRegularFile(*context.source_image) &&
      !IsDirectory(*context.source_image)) {
    return NotFoundError("source bootc image not found at {}",
                         context.source_image->string());
  }

  PUU_ASSIGN_OR_RETURN(
      context.layout,
      DiskLayout::Plan(context.drive, context.config, context.hibernate));

  std::vector<std::string> devices;
  devices.reserve(context.layout->DevicePaths().size());
  for (const std::filesystem::path& path : context.layout->DevicePaths()) {
    devices.push_back(path.string());
  }
  InstallationLogger()->info("target {} ({}; {})", context.drive.string(),
                             HumanSize(size), absl::StrJoin(devices, ", "));
  return absl::OkStatus();
}

Status StepVerifyImage(Runner& runner) {
  const Context& context = runner.GetContext();
  if (!context.source_image.has_value()) {
    InstallationLogger()->info("using the running bootc image as the source");
    return absl::OkStatus();
  }
  const std::optional<Digest>& expected = context.config.image.expected_digest;
  if (!expected.has_value()) {
    return InvalidArgumentError(
        "image.expectedDigest is required when installing from a local image "
        "source");
  }
  InstallationLogger()->info("expected image digest: {}", expected->Value());
  PUU_RETURN_IF_ERROR(
      context.image_manifest.VerifyDigest(*context.source_image, *expected));
  InstallationLogger()->info("image digest verified: {}", expected->Value());
  return absl::OkStatus();
}

Status StepPartition(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(DiskLayout * layout, MutableLayout(context));
  PUU_RETURN_IF_ERROR(ValidateSelectedDrive(context).status());
  PUU_RETURN_IF_ERROR(layout->WriteGpt());

  PUU_RETURN_IF_ERROR(WaitForUdevSettle(kDeviceTimeout));
  for (const std::filesystem::path& part : layout->DevicePaths()) {
    PUU_RETURN_IF_ERROR(WaitForBlockDevice(part, kDeviceTimeout));
  }
  return absl::OkStatus();
}

Status StepFormatEfi(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(DiskLayout * layout, MutableLayout(context));
  const std::filesystem::path& part = layout->EfiDevice();
  InstallationLogger()->info("formatting {} as FAT32 (label EFI)",
                             part.string());
  Command command("mkfs.fat");
  PUU_RETURN_IF_ERROR(command.Args({"-F32", "-n", "EFI", part.string()}).Run());

  PUU_ASSIGN_OR_RETURN(std::string part_uuid,
                       ProbeBlockTag(part, BlockTag::kPartUuid));
  layout->SetEfiPartUuid(std::move(part_uuid));
  return absl::OkStatus();
}

Status StepFormatRoot(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(DiskLayout * layout, MutableLayout(context));
  const std::filesystem::path& part = layout->RootDevice();
  const std::string label = context.config.labels.root.Value();
  InstallationLogger()->info("formatting {} as XFS (label {})", part.string(),
                             label);
  Command command("mkfs.xfs");
  PUU_RETURN_IF_ERROR(command.Args({"-f", "-L", label, part.string()}).Run());
  PUU_ASSIGN_OR_RETURN(std::string uuid, ProbeBlockTag(part, BlockTag::kUuid));
  layout->SetRootUuid(std::move(uuid));
  return absl::OkStatus();
}

Status StepFormatSwap(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(DiskLayout * layout, MutableLayout(context));
  PUU_ASSIGN_OR_RETURN(const std::filesystem::path part, layout->SwapDevice());
  const std::string label = context.config.labels.swap.Value();
  InstallationLogger()->info("formatting {} as swap (label {})", part.string(),
                             label);
  Command command("mkswap");
  PUU_RETURN_IF_ERROR(command.Args({"-L", label, part.string()}).Run());
  PUU_ASSIGN_OR_RETURN(std::string uuid, ProbeBlockTag(part, BlockTag::kUuid));
  layout->SetSwapUuid(std::move(uuid));
  return absl::OkStatus();
}
Status StepFormatHome(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(DiskLayout * layout, MutableLayout(context));
  const std::filesystem::path& part = layout->HomeDevice();
  const std::string label = context.config.labels.home.Value();
  InstallationLogger()->info("formatting {} as ext4 (label {})", part.string(),
                             label);
  Command command("mke2fs");
  PUU_RETURN_IF_ERROR(command
                          .Args({"-t", "ext4", "-O", "encrypt", "-F", "-L",
                                 label, part.string()})
                          .Run());
  PUU_ASSIGN_OR_RETURN(std::string uuid, ProbeBlockTag(part, BlockTag::kUuid));
  layout->SetHomeUuid(std::move(uuid));
  return absl::OkStatus();
}

Status StepMount(Runner& runner) {
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout,
                       GetLayout(runner.GetContext()));
  return MountTarget(layout->RootDevice(), kTargetMount, "xfs", "");
}

Status StepBootcInstall(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout, GetLayout(context));
  PUU_RETURN_IF_ERROR(layout->RootUuid().status());
  if (!context.source_image.has_value() && !RunningInContainer()) {
    return FailedPreconditionError(
        "no local bootc source image found; expected {}/image.tar",
        context.config.image.source_mount.string());
  }

  CleanupStack cleanups;
  const std::filesystem::path scratch = InTarget("/var/tmp");
  PUU_RETURN_IF_ERROR(CreateDirectories(scratch));
  PUU_RETURN_IF_ERROR(BindMount(scratch, scratch));
  cleanups.Push([scratch] {
    Unmount(scratch).IgnoreError();
    RemoveTree(scratch).IgnoreError();
  });
  PUU_RETURN_IF_ERROR(BindMount(scratch, kHostVarTmp));
  cleanups.Push([] { Unmount(kHostVarTmp).IgnoreError(); });
  PUU_RETURN_IF_ERROR(CreateDirectories(kBootcGraphRoot));
  PUU_RETURN_IF_ERROR(CreateDirectories(kContainersRunRoot));
  PUU_RETURN_IF_ERROR(WriteFile(
      kBootcStorageConf,
      fmt::format("[storage]\ndriver = \"overlay\"\ngraphroot = \"{}\"\n"
                  "runroot = \"{}\"\n",
                  kBootcGraphRoot, kContainersRunRoot),
      kPublicFileMode));

  std::vector<std::string> args = {"install", "to-filesystem"};
  if (context.source_image.has_value()) {
    ImageRef source;
    if (context.image_manifest.source_imgref.has_value()) {
      source = *context.image_manifest.source_imgref;
    } else {
      PUU_ASSIGN_OR_RETURN(source, SourceImageRef(*context.source_image));
    }
    args.emplace_back("--source-imgref");
    args.push_back(source.Value());
  }
  args.emplace_back("--target-imgref");
  args.push_back(context.target_imgref.Value());
  args.emplace_back("--skip-fetch-check");
  args.emplace_back("--bootloader");
  args.emplace_back("none");
  args.emplace_back("--skip-finalize");
  PUU_ASSIGN_OR_RETURN(const std::vector<KernelArg> kernel_args,
                       InstallKernelArgs(*layout, context.config));
  for (const KernelArg& argument : kernel_args) {
    args.emplace_back("--karg");
    args.push_back(argument.Value());
  }
  args.emplace_back(kTargetMount);

  InstallationLogger()->info(
      "bootc image deployment may take several minutes without native "
      "progress output");

  Command command("bootc");
  command.Args(args)
      .NewPidNamespace()
      .Env("CONTAINERS_STORAGE_CONF", std::string(kBootcStorageConf))
      .Env("TMPDIR", std::string(kHostVarTmp))
      .Heartbeat(kHeartbeatInterval,
                 "bootc install still running; deploying image");

  std::optional<DiskWriteProgress> progress;
  if (context.source_image.has_value()) {
    const StatusOr<uint64_t> expected =
        context.image_manifest.EstimatedInstallBytes(*context.source_image);
    if (expected.ok()) {
      std::error_code error;
      const uintmax_t archive_bytes =
          std::filesystem::file_size(*context.source_image, error);
      progress.emplace(kTargetMount,
                       *expected + (error ? 0 : uint64_t{archive_bytes}));
      command.Track(&progress.value(), [&runner](double fraction) {
        runner.ReportProgress(fraction);
      });
    }
  }
  PUU_RETURN_IF_ERROR(command.Run());
  return runner.ResolveDeployment(kTargetMount);
}

Status StepMountHome(Runner& runner) {
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout,
                       GetLayout(runner.GetContext()));
  return MountTarget(layout->HomeDevice(), kTargetHomeMount, "ext4", "");
}

Status StepConfigureMounts(Runner& runner) {
  const Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout, GetLayout(context));
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());

  for (const std::string_view relative : {"efi", "var", "var/home", "etc"}) {
    PUU_RETURN_IF_ERROR(CreateDirectories(deployment->InDeployment(relative)));
  }

  const std::filesystem::path fstab = deployment->InDeployment("/etc/fstab");
  std::string existing;
  if (Exists(fstab)) {
    PUU_ASSIGN_OR_RETURN(existing, ReadFile(fstab));
  }
  PUU_ASSIGN_OR_RETURN(const std::string merged, layout->MergeFstab(existing));
  PUU_RETURN_IF_ERROR(WriteFile(fstab, merged, kPublicFileMode));

  const std::filesystem::path zram_conf = deployment->InDeployment(
      "/etc/systemd/zram-generator.conf.d/99-puu-install.conf");
  if (context.hibernate) {
    if (Exists(zram_conf)) {
      std::error_code error;
      std::filesystem::remove(zram_conf, error);
      if (error) {
        return FilesystemError(error,
                               fmt::format("remove {}", zram_conf.string()));
      }
    }
    return absl::OkStatus();
  }

  PUU_RETURN_IF_ERROR(CreateDirectories(zram_conf.parent_path()));
  return WriteFile(zram_conf,
                   "[zram0]\n"
                   "zram-size = ceil(ram / 1024 + (ram / 1024) ^ 0.5) * 1024\n",
                   kPublicFileMode);
}

Status StepFirstboot(Runner& runner) {
  const IdentityConfig& identity = runner.GetContext().config.identity;
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());

  if (!IsRegularFile(deployment->InDeployment("/usr/share/zoneinfo") /
                     identity.timezone.Value())) {
    return NotFoundError("timezone not found in target system: {}",
                         identity.timezone.Value());
  }

  const std::filesystem::path etc = deployment->InDeployment("/etc");
  PUU_RETURN_IF_ERROR(CreateDirectories(etc));

  // These overwrite whatever the image ships. Image defaults are placeholders;
  // preserving them would silently discard the operator's choice and leave
  // every machine on the same hostname.
  PUU_RETURN_IF_ERROR(WriteFile(etc / "hostname",
                                fmt::format("{}\n", identity.hostname.Value()),
                                kPublicFileMode));
  InstallationLogger()->info("staged hostname {} in /etc/hostname",
                             identity.hostname.Value());
  if (!identity.locale.Empty()) {
    PUU_RETURN_IF_ERROR(WriteFile(
        etc / "locale.conf", fmt::format("LANG={}\n", identity.locale.Value()),
        kPublicFileMode));
    InstallationLogger()->info("staged locale {} in /etc/locale.conf",
                               identity.locale.Value());
  }

  const std::filesystem::path localtime = etc / "localtime";
  PUU_RETURN_IF_ERROR(RemoveFile(localtime));
  PUU_RETURN_IF_ERROR(
      CreateSymlink(std::filesystem::path("../usr/share/zoneinfo") /
                        identity.timezone.Value(),
                    localtime));

  if (!identity.keymap.Empty()) {
    PUU_RETURN_IF_ERROR(WriteFile(
        etc / "vconsole.conf",
        fmt::format("KEYMAP={}\n", identity.keymap.Value()), kPublicFileMode));
    InstallationLogger()->info("staged keymap {} in /etc/vconsole.conf",
                               identity.keymap.Value());
  }
  return absl::OkStatus();
}

Status StepConfigureK3s(Runner& runner) {
  Context& context = runner.GetContext();
  SecretString token(std::move(context.k3s_token));
  if (!context.config.k3s.has_value()) {
    return absl::OkStatus();
  }
  const K3sConfig& k3s = *context.config.k3s;
  if (k3s.role == K3sRole::kServer && !k3s.node_name.has_value()) {
    InstallationLogger()->info(
        "k3s cluster: using image default server configuration");
    return absl::OkStatus();
  }
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  PUU_RETURN_IF_ERROR(StageK3s(deployment->Root(), k3s, std::move(token)));
  InstallationLogger()->info("k3s cluster: staged {} configuration",
                             K3sRoleName(k3s.role));
  return absl::OkStatus();
}

Status StepStageHomedIdentity(Runner& runner) {
  Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  if (!context.username.has_value()) {
    return InvalidArgumentError(
        "username is required to stage a homed identity");
  }
  const std::string name = context.username->Value();
  const std::string_view password = context.user_password.Value();

  if (password.find_first_of("\r\n") != std::string_view::npos) {
    return InvalidArgumentError("password must not contain newline characters");
  }
  PUU_ASSIGN_OR_RETURN(const std::string hashed, HashPassword(password));

  std::vector<std::string> groups;
  groups.reserve(context.config.homed.groups.size());
  for (const GroupName& group : context.config.homed.groups) {
    groups.push_back(group.Value());
  }

  nlohmann::ordered_json identity;
  identity["userName"] = name;
  identity["secret"]["password"].push_back(std::string(password));
  identity["realName"] = name;
  identity["shell"] = std::string(UserShellPath(context.user_shell));
  identity["memberOf"] = groups;
  identity["homeDirectory"] = fmt::format("/var/home/{}", name);
  identity["imagePath"] = fmt::format("/var/home/{}.homedir", name);
  identity["storage"] = "fscrypt";
  if (context.config.homed.session_launcher.has_value()) {
    identity["preferredSessionLauncher"] =
        context.config.homed.session_launcher->Value();
  }
  identity["enforcePasswordPolicy"] = false;
  identity["privileged"]["hashedPassword"].push_back(hashed);

  const std::filesystem::path& configured_path =
      context.config.homed.identity_path;
  const std::filesystem::path target = deployment->InState(configured_path);
  PUU_RETURN_IF_ERROR(CreateDirectories(target.parent_path()));

  std::string text =
      identity.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
  const Status written = WriteFile(target, text, kSecretFileMode, O_NOFOLLOW);

  for (auto& value : identity["secret"]["password"]) {
    auto& held = value.get_ref<std::string&>();
    OPENSSL_cleanse(held.data(), held.size());
  }
  OPENSSL_cleanse(text.data(), text.size());
  identity = nlohmann::ordered_json();
  context.user_password.Clear();
  PUU_RETURN_IF_ERROR(written);

  InstallationLogger()->info("staged homed identity for {} at {}", name,
                             configured_path.string());
  return absl::OkStatus();
}

Status StepSeedStores(Runner& runner) {
  const std::vector<SeedConfig>& seeds = runner.GetContext().config.seeds;
  if (seeds.empty()) {
    return absl::OkStatus();
  }
  const auto total = static_cast<double>(seeds.size());
  for (size_t index = 0; index < seeds.size(); ++index) {
    const double base = static_cast<double>(index) / total;
    const double span = 1.0 / total;
    PUU_RETURN_IF_ERROR(SeedStore(runner, seeds[index], base, span));
    runner.ReportProgress(static_cast<double>(index + 1) / total);
  }
  return absl::OkStatus();
}

Status StepInstallBootloader(Runner& runner) {
  PUU_RETURN_IF_ERROR(MountEfi(runner));
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  PUU_ASSIGN_OR_RETURN(const std::filesystem::path source,
                       SystemdBootSource(*deployment));
  PUU_ASSIGN_OR_RETURN(const Arch arch, HostArch());
  const std::string suffix(ArchEfiSuffix(arch));
  std::string upper = suffix;
  std::ranges::transform(upper, upper.begin(),
                         [](unsigned char c) { return std::toupper(c); });

  const std::filesystem::path systemd_dir =
      std::filesystem::path(kTargetEfiMount) / "EFI/systemd";
  const std::filesystem::path fallback_dir =
      std::filesystem::path(kTargetEfiMount) / "EFI/BOOT";
  PUU_RETURN_IF_ERROR(CreateDirectories(systemd_dir));
  PUU_RETURN_IF_ERROR(CreateDirectories(fallback_dir));

  PUU_RETURN_IF_ERROR(CopyFile(
      source, systemd_dir / fmt::format("systemd-boot{}.efi", suffix)));
  PUU_RETURN_IF_ERROR(
      CopyFile(source, fallback_dir / fmt::format("BOOT{}.EFI", upper)));
  InstallationLogger()->info("installed systemd-boot on the ESP");
  return absl::OkStatus();
}

Status StepStageEspBoot(Runner& runner) {
  PUU_RETURN_IF_ERROR(MountEfi(runner));
  const std::filesystem::path entries = InTarget("/boot/loader/entries");
  const std::filesystem::path ostree = InTarget("/boot/ostree");
  if (!IsDirectory(entries)) {
    return NotFoundError("boot loader entries not found: {}", entries.string());
  }
  if (!IsDirectory(ostree)) {
    return NotFoundError("boot ostree files not found: {}", ostree.string());
  }
  PUU_RETURN_IF_ERROR(CopyTreeFiles(
      entries, std::filesystem::path(kTargetEfiMount) / "loader/entries"));
  PUU_RETURN_IF_ERROR(CopyTreeFiles(
      ostree, std::filesystem::path(kTargetEfiMount) / "boot/ostree"));

  SyncAll();
  InstallationLogger()->info(
      "staged boot loader entries and ostree kernel files on ESP");
  return absl::OkStatus();
}

Status StepSign(Runner& runner) {
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  KeyStore store(deployment->InState("/var/lib/sbctl"));
  PUU_RETURN_IF_ERROR(store.EnsureKeys());
  PUU_ASSIGN_OR_RETURN(const KeyPair database, store.Get(KeyKind::kDatabase));
  PUU_ASSIGN_OR_RETURN(const std::vector<std::filesystem::path> binaries,
                       TargetEfiBinaries(kTargetEfiMount));
  if (binaries.empty()) {
    return NotFoundError("no EFI binaries found under {}", kTargetEfiMount);
  }

  size_t signed_count = 0;
  size_t skipped_count = 0;
  for (const std::filesystem::path& binary : binaries) {
    PUU_ASSIGN_OR_RETURN(const bool already, database.Verify(binary));
    if (already) {
      ++skipped_count;
      continue;
    }
    PUU_RETURN_IF_ERROR(database.Sign(binary));
    ++signed_count;
  }
  InstallationLogger()->info(
      "signed {} EFI binaries on target ESP ({} already signed)", signed_count,
      skipped_count);

  for (const std::filesystem::path& binary : binaries) {
    PUU_ASSIGN_OR_RETURN(const bool valid, database.Verify(binary));
    if (!valid) {
      return InternalError("signature verification failed for {}",
                           binary.string());
    }
  }
  return absl::OkStatus();
}

Status StepEnrollKeys(Runner& runner) {
  PUU_ASSIGN_OR_RETURN(const TargetDeployment* deployment,
                       runner.GetDeployment());
  KeyStore store(deployment->InState("/var/lib/sbctl"));
  PUU_RETURN_IF_ERROR(store.EnsureKeys());

  Status status = store.Enroll();
  if (status.ok()) {
    InstallationLogger()->info("enrolled Secure Boot db, KEK and PK");
    return absl::OkStatus();
  }

  if (DetectSetupMode().state == SetupMode::State::kOff) {
    InstallationLogger()->info(
        "Secure Boot keys appear to be enrolled already");
    return absl::OkStatus();
  }
  return status;
}

Status StepNvram(Runner& runner) {
  PUU_RETURN_IF_ERROR(MountEfi(runner));
  const Context& context = runner.GetContext();
  PUU_ASSIGN_OR_RETURN(const DiskLayout* layout, GetLayout(context));
  PUU_ASSIGN_OR_RETURN(const EfiPartitionLayout esp, layout->EspLayout());
  PUU_ASSIGN_OR_RETURN(const Arch arch, HostArch());
  return ManageBootEntry(esp, arch, context.config.branding.name.Value());
}

Status StepFinalCleanup(Runner& /*runner*/) {
  PUU_RETURN_IF_ERROR(CleanupMounts());
  InstallationLogger()->info("target filesystems unmounted; ready to reboot");
  return absl::OkStatus();
}

bool RequiresSecureBoot(const Context& context) {
  return context.secure_boot == SecureBootMode::kEnroll;
}

bool HasHibernate(const Context& context) { return context.hibernate; }

bool HasK3s(const Context& context) { return context.config.k3s.has_value(); }

bool HasSeeds(const Context& context) { return !context.config.seeds.empty(); }

constexpr std::array<Step, 24> kSteps = {{
    {.title = "Verify dependencies", .run = &StepDependencies},
    {.title = "Verify Secure Boot setup",
     .run = &StepSetupMode,
     .enabled = &RequiresSecureBoot},
    {.title = "Clean up previous mounts", .run = &StepCleanup},
    {.title = "Validate target drive", .run = &StepValidateDrive},
    {.title = "Verify image integrity", .run = &StepVerifyImage},
    {.title = "Partition drive",
     .run = &StepPartition,
     .point_of_no_return = true},
    {.title = "Format EFI system partition", .run = &StepFormatEfi},
    {.title = "Format root filesystem", .run = &StepFormatRoot},
    {.title = "Format swap partition",
     .run = &StepFormatSwap,
     .enabled = &HasHibernate},
    {.title = "Format home filesystem", .run = &StepFormatHome},
    {.title = "Mount target filesystems", .run = &StepMount},
    {.title = "Install bootc image", .run = &StepBootcInstall, .weight = 30},
    {.title = "Mount home filesystem", .run = &StepMountHome},
    {.title = "Configure persistent mounts", .run = &StepConfigureMounts},
    {.title = "Configure firstboot identity", .run = &StepFirstboot},
    {.title = "Configure k3s cluster",
     .run = &StepConfigureK3s,
     .enabled = &HasK3s},
    {.title = "Stage homed user identity", .run = &StepStageHomedIdentity},
    {.title = "Pre-stage stores",
     .run = &StepSeedStores,
     .weight = 15,
     .enabled = &HasSeeds},
    {.title = "Install systemd-boot", .run = &StepInstallBootloader},
    {.title = "Stage ESP boot files", .run = &StepStageEspBoot},
    {.title = "Sign EFI binaries",
     .run = &StepSign,
     .enabled = &RequiresSecureBoot},
    {.title = "Enroll Secure Boot keys",
     .run = &StepEnrollKeys,
     .enabled = &RequiresSecureBoot},
    {.title = "Manage NVRAM boot entry", .run = &StepNvram},
    {.title = "Unmount target filesystems", .run = &StepFinalCleanup},
}};

}  // namespace

std::span<const Step> Steps() { return kSteps; }

}  // namespace puu
