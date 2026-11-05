// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_APP_H_
#define PUU_APP_H_

#include <cstdint>
#include <memory>

#include "context.h"

namespace puu {

enum class Trap : std::uint8_t { kQuit, kReboot, kRebootFirmware, kShell };

class Installer {
 public:
  explicit Installer(Context context);
  ~Installer();
  Installer(const Installer&) = delete;
  Installer& operator=(const Installer&) = delete;
  // Movable so that main() can restart the UI after a recovery shell without
  // rebuilding the context.
  Installer(Installer&&) noexcept;
  Installer& operator=(Installer&&) noexcept;

  Trap Run();

  // Surrenders the context so a new Installer can resume after a shell.
  Context IntoContext() &&;

 private:
  // The terminal, the installation worker and the screen state stay out of the
  // header: main() only ever needs the trap and the context.
  class Impl;

  std::unique_ptr<Impl> impl_;
};

}  // namespace puu

#endif  // PUU_APP_H_
