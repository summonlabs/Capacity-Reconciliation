// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal reconciliation kernel.
//
// This is a pure function: (evidence, attributions, request, limits) -> run.
// It holds no state, takes no locks and calls nothing that can fail for a
// reason other than the inputs. Everything the public Engine does around it is
// bookkeeping; everything that decides *what is true* happens here.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/run.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Statistics produced alongside a run. Diagnosis only; never authoritative.
struct KernelStats {
  std::uint64_t cells = 0;
  std::uint64_t evidence_considered = 0;
  std::uint64_t attributions_considered = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t unexplained_cells = 0;
};

struct KernelInput {
  const EvidenceSet* evidence = nullptr;
  const AttributionSet* attributions = nullptr;
  const RunRequest* request = nullptr;
  const Limits* limits = nullptr;
  /// Identity to assign to the produced run. The identity is *not* part of the
  /// run digest: two computations over identical inputs produce equal digests
  /// and different identities.
  ReconciliationRunId run_id;
};

/// Compute a run. Deterministic: the result depends only on the values in
/// `input`, never on the order evidence or attributions were supplied in.
[[nodiscard]] Result<ReconciliationRun> reconcile_kernel(const KernelInput& input,
                                                         KernelStats* stats);

/// Build the canonical encoding of a request. Exposed so that tests can assert
/// determinism of the request encoding independently of the kernel.
void encode_request(CanonicalWriter& writer, const RunRequest& request);

/// Encode one cell. Exposed for the same reason.
void encode_cell(CanonicalWriter& writer, const ReconciliationCell& cell);

/// Decode one cell from a canonical stream. Used by the store and by tooling.
[[nodiscard]] Result<ReconciliationCell> decode_cell(CanonicalReader& reader, const Limits& limits);

/// Encode/decode the mutable envelope of a run (identity, resolution, attempt,
/// commit generation, supersession). Deliberately outside the run digest.
void encode_run_envelope(CanonicalWriter& writer, const ReconciliationRun& run);

}  // namespace summon::capacity_reconciliation
