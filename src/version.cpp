// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/version.hpp"

#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

const char* version_string() noexcept { return "1.0.0"; }

const Limits& default_limits() noexcept {
  static const Limits kLimits{};
  return kLimits;
}

}  // namespace summon::capacity_reconciliation
