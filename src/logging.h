// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

#ifndef PUU_LOGGING_H_
#define PUU_LOGGING_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"

namespace puu {

// Truncates to at most `max_bytes` without splitting a UTF-8 sequence.
std::string TruncateUtf8(std::string_view text, size_t max_bytes);

// Creates /run/puu-installer/install.log and points spdlog at it, plus an
// in-memory sink feeding the installation screen. Call once, before the UI
// starts. The log holds passwords in neither sink, but it does hold device
// paths, so both the directory and the file are owner-only.
Status InitLogging();

// A copy of the most recent log lines, oldest first, capped at the ring size.
// Safe to call from the UI thread while the installation worker is logging.
std::vector<std::string> SnapshotLogLines();

}  // namespace puu

#endif  // PUU_LOGGING_H_
