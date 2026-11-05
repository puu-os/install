// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_K3S_H_
#define PUU_K3S_H_

#include <filesystem>

#include "config.h"
#include "secret_string.h"
#include "status.h"

namespace puu {

[[nodiscard]] Status StageK3s(const std::filesystem::path& deployment,
                              const K3sConfig& config, SecretString token);

}

#endif
