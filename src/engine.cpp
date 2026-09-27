// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/engine.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

#include "reconcile.hpp"
#include "summon/capacity_reconciliation/platform.hpp"

namespace summon::capacity_reconciliation {
namespace {

constexpr std::size_t kMaxDetailBytes = 160;

/// Build a history detail token from a bounded, character-restricted string.
std::string history_detail(std::string_view text) {
  auto token = make_token(text, kMaxDetailBytes, "history detail");
  return token.ok() ? token.value() : std::string("detail_rejected");
}

}  // namespace

Engine::~Engine() = default;

Engine::Engine(Engine&& other) noexcept
    : options_(std::move(other.options_)),
      store_(std::move(other.store_)),
      evidence_(std::move(other.evidence_)),
      attributions_(std::move(other.attributions_)),
      evidence_digest_(other.evidence_digest_),
      attribution_digest_(other.attribution_digest_),
      runs_(std::move(other.runs_)),
      history_(std::move(other.history_)),
      history_sequence_(other.history_sequence_),
      generation_(other.generation_),
      incarnation_(other.incarnation_),
      attempt_(other.attempt_),
      observer_(std::move(other.observer_)),
      last_stats_(other.last_stats_) {
  other.history_sequence_ = 0;
  other.generation_ = Generation{};
  other.incarnation_ = IncarnationId{};
  other.attempt_ = AttemptId{};
}

Engine& Engine::operator=(Engine&& other) noexcept {
  if (this != &other) {
    std::scoped_lock guard(mutex_, other.mutex_);
    options_ = std::move(other.options_);
    store_ = std::move(other.store_);
    evidence_ = std::move(other.evidence_);
    attributions_ = std::move(other.attributions_);
    evidence_digest_ = other.evidence_digest_;
    attribution_digest_ = other.attribution_digest_;
    runs_ = std::move(other.runs_);
    history_ = std::move(other.history_);
    history_sequence_ = other.history_sequence_;
    generation_ = other.generation_;
    incarnation_ = other.incarnation_;
    attempt_ = other.attempt_;
    observer_ = std::move(other.observer_);
    last_stats_ = other.last_stats_;
    other.history_sequence_ = 0;
    other.generation_ = Generation{};
    other.incarnation_ = IncarnationId{};
    other.attempt_ = AttemptId{};
  }
  return *this;
}

bool Engine::stale_incarnation(const IncarnationId& expected) const {
  if (store_.has_value()) {
    return !(expected == store_->incarnation());
  }
  return !(expected == incarnation_);
}

Status Engine::append_history(HistoryEntry& entry, StoreImage* image) {
  auto next_sequence = checked_increment(history_sequence_);
  if (!next_sequence.ok()) {
    return make_error(ErrorCode::Overflow, "history sequence exhausted", "history_sequence");
  }
  entry.sequence = next_sequence.value();
  if (entry.detail.size() > kMaxDetailBytes) {
    entry.detail.resize(kMaxDetailBytes);
  }
  history_sequence_ = entry.sequence;
  history_.push_back(entry);

  // History is bounded: the oldest entries are retired first, which keeps the
  // sequence strictly increasing and the newest records intact.
  if (history_.size() > options_.limits.max_history_retained) {
    const std::size_t excess = history_.size() - options_.limits.max_history_retained;
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  if (image != nullptr) {
    image->history = history_;
    image->history_sequence = history_sequence_;
  }
  return Status::success();
}

void Engine::append_history_rollback() {
  if (!history_.empty()) {
    history_.pop_back();
    history_sequence_ = history_.empty() ? 0 : history_.back().sequence;
  }
}

void Engine::notify(const HistoryEntry& entry) const {
  // The observer runs after every internal lock has been released, on the
  // calling thread, with a copy of the event. It may therefore call back into
  // the engine without deadlocking, and it can never observe a partially
  // applied mutation.
  Observer observer;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    observer = observer_;
  }
  if (observer) {
    observer(entry);
  }
}

void Engine::set_observer(Observer observer) {
  std::lock_guard<std::mutex> guard(mutex_);
  observer_ = std::move(observer);
}

Result<Engine> Engine::create(EngineOptions options) {
  if (options.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "an engine requires a declared clock domain; ticks from an unnamed domain "
                      "cannot be compared with anything",
                      "clock_domain");
  }
  if (options.max_runs_retained == 0 ||
      options.max_runs_retained > options.limits.max_runs_retained) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_runs_retained must be between 1 and the configured limit of " +
                          std::to_string(options.limits.max_runs_retained),
                      "max_runs_retained");
  }
  Engine engine;
  engine.options_ = std::move(options);
  engine.incarnation_ = IncarnationId::generate();
  auto evidence = EvidenceSet::build({}, engine.options_.limits.max_evidence_items);
  if (!evidence.ok()) {
    return evidence.error();
  }
  engine.evidence_ = std::make_shared<const EvidenceSet>(std::move(evidence.value()));
  engine.evidence_digest_ = engine.evidence_->digest();
  auto attributions = AttributionSet::build({}, engine.options_.limits.max_attribution_items);
  if (!attributions.ok()) {
    return attributions.error();
  }
  engine.attributions_ = std::make_shared<const AttributionSet>(std::move(attributions.value()));
  engine.attribution_digest_ = engine.attributions_->digest();
  return engine;
}

Result<Engine> Engine::attach(Store store, EngineOptions options) {
  if (options.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "an engine requires a declared clock domain", "clock_domain");
  }
  if (options.max_runs_retained == 0 ||
      options.max_runs_retained > options.limits.max_runs_retained) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_runs_retained must be between 1 and the configured limit of " +
                          std::to_string(options.limits.max_runs_retained),
                      "max_runs_retained");
  }
  Engine engine;
  engine.options_ = std::move(options);

  const StoreImage& image = store.image();
  auto evidence = EvidenceSet::build(image.evidence, engine.options_.limits.max_evidence_items);
  if (!evidence.ok()) {
    return evidence.error();
  }
  engine.evidence_ = std::make_shared<const EvidenceSet>(std::move(evidence.value()));
  engine.evidence_digest_ = engine.evidence_->digest();

  auto attributions =
      AttributionSet::build(image.attributions, engine.options_.limits.max_attribution_items);
  if (!attributions.ok()) {
    return attributions.error();
  }
  engine.attributions_ = std::make_shared<const AttributionSet>(std::move(attributions.value()));
  engine.attribution_digest_ = engine.attributions_->digest();

  engine.runs_ = image.runs;
  engine.history_ = image.history;
  engine.history_sequence_ = image.history_sequence;
  engine.store_ = std::move(store);

  // Recovered state is never silently fresh. Every recovered run keeps the
  // resolution it was committed with, its evidence keeps the ticks it was
  // stamped with, and the kernel re-evaluates freshness at whatever evaluation
  // instant the caller supplies. A genuine recovery from the previous commit is
  // recorded durably so that it cannot be forgotten.
  if (engine.store_->recovery().outcome == RecoveryOutcome::RecoveredFromPreviousCommit) {
    HistoryEntry event;
    event.kind = HistoryEventKind::StoreRecovered;
    event.generation = engine.store_->generation();
    event.attempt = engine.store_->attempt();
    event.subject_digest = engine.store_->recovery().record_digest;
    event.detail = history_detail("recovered_from_previous_commit");
    StoreImage next = engine.store_->image();
    next.clock_domain = engine.options_.clock_domain;
    auto status = engine.append_history(event, &next);
    if (!status.ok()) {
      return status.error();
    }
    auto committed = engine.store_->mutate(engine.store_->generation(),
                                           engine.store_->incarnation(),
                                           [&next](StoreImage& target) {
                                             target = next;
                                             return Status::success();
                                           });
    if (!committed.ok()) {
      engine.append_history_rollback();
      return committed.error();
    }
  }
  return engine;
}

Status Engine::append_evidence(std::vector<EvidenceItem> items, Generation expected,
                               const IncarnationId& expected_incarnation) {
  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (items.empty()) {
      return make_error(ErrorCode::InvalidArgument, "no evidence supplied", "evidence");
    }
    if (!(expected == generation())) {
      return generation_mismatch(expected.value(), generation().value());
    }
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (evidence_->size() + items.size() > options_.limits.max_evidence_items) {
      return make_error(ErrorCode::LimitExceeded,
                        "appending " + std::to_string(items.size()) +
                            " items would exceed the evidence limit of " +
                            std::to_string(options_.limits.max_evidence_items),
                        "evidence_items");
    }

    std::vector<EvidenceItem> merged = evidence_->items();
    for (const EvidenceItem& item : items) {
      for (const EvidenceItem& existing : merged) {
        if (existing.id() == item.id()) {
          return make_error(ErrorCode::AlreadyExists,
                            "evidence identity " + item.id().to_string() +
                                " is already present: identity is never reused",
                            "evidence_id");
        }
      }
      if (item.stamp().generation > expected) {
        return make_error(ErrorCode::StaleGeneration,
                          "evidence " + item.id().to_string() + " is stamped at generation " +
                              std::to_string(item.stamp().generation.value()) +
                              ", beyond the current generation " +
                              std::to_string(expected.value()),
                          "evidence_generation");
      }
    }
    merged.insert(merged.end(), items.begin(), items.end());

    auto rebuilt = EvidenceSet::build(std::move(merged), options_.limits.max_evidence_items);
    if (!rebuilt.ok()) {
      return rebuilt.error();
    }
    auto next = std::make_shared<const EvidenceSet>(std::move(rebuilt.value()));

    event.kind = HistoryEventKind::EvidenceAppended;
    event.generation = expected;
    event.attempt = attempt();
    event.subject_digest = next->digest();
    event.detail = history_detail("evidence_appended_" + std::to_string(items.size()));

    if (store_.has_value()) {
      StoreImage image = stage_image();
      image.evidence = next->items();
      auto status = append_history(event, &image);
      if (!status.ok()) {
        return status;
      }
      auto committed = store_->mutate(expected, expected_incarnation, [&image](StoreImage& target) {
        target = image;
        return Status::success();
      });
      if (!committed.ok()) {
        append_history_rollback();
        return committed.error();
      }
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        return status;
      }
      auto next_generation = expected.next();
      if (!next_generation.ok()) {
        append_history_rollback();
        return next_generation.error();
      }
      generation_ = next_generation.value();
    }
    evidence_ = std::move(next);
    evidence_digest_ = evidence_->digest();
  }
  notify(event);
  return Status::success();
}

Status Engine::append_attributions(std::vector<AttributionItem> items, Generation expected,
                                   const IncarnationId& expected_incarnation) {
  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (items.empty()) {
      return make_error(ErrorCode::InvalidArgument, "no attributions supplied", "attribution");
    }
    if (!(expected == generation())) {
      return generation_mismatch(expected.value(), generation().value());
    }
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (attributions_->size() + items.size() > options_.limits.max_attribution_items) {
      return make_error(ErrorCode::LimitExceeded,
                        "appending " + std::to_string(items.size()) +
                            " attributions would exceed the limit of " +
                            std::to_string(options_.limits.max_attribution_items),
                        "attribution_items");
    }

    std::vector<AttributionItem> merged = attributions_->items();
    for (const AttributionItem& item : items) {
      for (const AttributionItem& existing : merged) {
        if (existing.id() == item.id()) {
          return make_error(ErrorCode::AlreadyExists,
                            "attribution identity " + item.id().to_string() +
                                " is already present",
                            "attribution_id");
        }
      }
      if (item.stamp().generation > expected) {
        return make_error(ErrorCode::StaleGeneration,
                          "attribution " + item.id().to_string() +
                              " is stamped beyond the current generation",
                          "attribution_generation");
      }
    }
    merged.insert(merged.end(), items.begin(), items.end());

    auto rebuilt =
        AttributionSet::build(std::move(merged), options_.limits.max_attribution_items);
    if (!rebuilt.ok()) {
      return rebuilt.error();
    }
    auto next = std::make_shared<const AttributionSet>(std::move(rebuilt.value()));

    event.kind = HistoryEventKind::AttributionAppended;
    event.generation = expected;
    event.attempt = attempt();
    event.subject_digest = next->digest();
    event.detail = history_detail("attribution_appended_" + std::to_string(items.size()));

    if (store_.has_value()) {
      StoreImage image = stage_image();
      image.attributions = next->items();
      auto status = append_history(event, &image);
      if (!status.ok()) {
        return status;
      }
      auto committed = store_->mutate(expected, expected_incarnation, [&image](StoreImage& target) {
        target = image;
        return Status::success();
      });
      if (!committed.ok()) {
        append_history_rollback();
        return committed.error();
      }
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        return status;
      }
      auto next_generation = expected.next();
      if (!next_generation.ok()) {
        append_history_rollback();
        return next_generation.error();
      }
      generation_ = next_generation.value();
    }
    attributions_ = std::move(next);
    attribution_digest_ = attributions_->digest();
  }
  notify(event);
  return Status::success();
}

Generation Engine::generation() const {
  if (store_.has_value()) {
    return store_->generation();
  }
  return generation_;
}

IncarnationId Engine::incarnation() const {
  if (store_.has_value()) {
    return store_->incarnation();
  }
  return incarnation_;
}

StoreImage Engine::stage_image() const {
  StoreImage image = store_->image();
  // The engine's clock domain is the one every run and every statement it
  // accepts is evaluated in. Recording it with the state means a later reopen
  // -- by this process or another -- knows which domain the stored ticks belong
  // to, instead of falling back to a placeholder and then refusing its own
  // evidence.
  image.clock_domain = options_.clock_domain;
  return image;
}

Epoch Engine::epoch() const {
  if (store_.has_value()) {
    return store_->epoch();
  }
  return Epoch{};
}

AttemptId Engine::attempt() const {
  if (store_.has_value()) {
    return store_->attempt();
  }
  return attempt_;
}

Result<ReconciliationRun> Engine::compute(const RunRequest& request, ReconcileStats* stats) const {
  std::shared_ptr<const EvidenceSet> evidence;
  std::shared_ptr<const AttributionSet> attributions;
  RunRequest effective = request;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    evidence = evidence_;
    attributions = attributions_;
    // The engine's policy is authoritative for every run it computes. A request
    // cannot smuggle in a different policy: that would let a caller change the
    // meaning of a classification without the change appearing in the engine's
    // recorded policy history.
    effective.policy = options_.policy;
  }
  KernelInput input;
  input.evidence = evidence.get();
  input.attributions = attributions.get();
  input.request = &effective;
  input.limits = &options_.limits;
  input.run_id = ReconciliationRunId::generate();

  KernelStats kernel_stats;
  auto run = reconcile_kernel(input, &kernel_stats);
  if (!run.ok()) {
    return run.error();
  }
  if (stats != nullptr) {
    stats->cells = kernel_stats.cells;
    stats->evidence_considered = kernel_stats.evidence_considered;
    stats->attributions_considered = kernel_stats.attributions_considered;
    stats->conflicts = kernel_stats.conflicts;
    stats->unexplained_cells = kernel_stats.unexplained_cells;
  }
  return run;
}

Result<ReconciliationRun> Engine::reconcile(const RunRequest& request) const {
  if (!(request.generation == generation())) {
    return generation_mismatch(request.generation.value(), generation().value());
  }
  ReconcileStats stats;
  auto run = compute(request, &stats);
  if (!run.ok()) {
    return run.error();
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    last_stats_ = stats;
  }
  return run;
}

Result<ReconciliationRun> Engine::reconcile_and_commit(const RunRequest& request, Generation expected,
                                                       const IncarnationId& expected_incarnation) {
  if (!(expected == generation())) {
    return generation_mismatch(expected.value(), generation().value());
  }
  ReconcileStats stats;
  auto computed = compute(request, &stats);
  if (!computed.ok()) {
    return computed.error();
  }
  ReconciliationRun run = std::move(computed.value());

  auto committed_generation = expected.next();
  if (!committed_generation.ok()) {
    return committed_generation.error();
  }

  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (runs_.size() >= options_.max_runs_retained) {
      // Retire the oldest run that no other retained run names as its parent,
      // so lineage never dangles.
      bool retired = false;
      for (auto it = runs_.begin(); it != runs_.end(); ++it) {
        // A run is retained when another run names it as its parent, and also
        // when the run about to be added names it: retiring the predecessor of
        // the run being committed would leave the lineage dangling.
        bool referenced = request.parent_run.has_value() && *request.parent_run == it->id();
        for (const ReconciliationRun& other : runs_) {
          if (other.request().parent_run.has_value() && *other.request().parent_run == it->id()) {
            referenced = true;
            break;
          }
        }
        if (!referenced) {
          runs_.erase(it);
          retired = true;
          break;
        }
      }
      if (!retired) {
        return make_error(ErrorCode::LimitExceeded,
                          "the retained run limit of " +
                              std::to_string(options_.max_runs_retained) +
                              " is reached and every retained run is referenced by another run's "
                              "lineage",
                          "runs");
      }
    }

    run.set_commit_attempt(attempt());
    run.set_committed_generation(committed_generation.value());

    event.kind = HistoryEventKind::RunComputed;
    event.generation = expected;
    event.attempt = attempt();
    event.run = run.id();
    event.subject_digest = run.run_digest();
    event.detail = history_detail(std::string("resolution_") + to_string(run.resolution()));

    if (store_.has_value()) {
      StoreImage image = stage_image();
      image.runs = runs_;
      image.runs.push_back(run);
      auto status = append_history(event, &image);
      if (!status.ok()) {
        return status.error();
      }
      auto committed = store_->mutate(expected, expected_incarnation, [&image](StoreImage& target) {
        target = image;
        return Status::success();
      });
      if (!committed.ok()) {
        append_history_rollback();
        return committed.error();
      }
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        return status.error();
      }
      generation_ = committed_generation.value();
    }
    runs_.push_back(run);
    last_stats_ = stats;
  }
  notify(event);
  return run;
}

Result<ReconciliationRun> Engine::find_run(ReconciliationRunId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  for (const ReconciliationRun& run : runs_) {
    if (run.id() == id) {
      return run;
    }
  }
  return make_error(ErrorCode::NotFound, "no run with identity " + id.to_string(), "run_id");
}

std::vector<HistoryEntry> Engine::history_for(ReconciliationRunId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<HistoryEntry> out;
  for (const HistoryEntry& entry : history_) {
    if ((entry.run.has_value() && *entry.run == id) ||
        (entry.related_run.has_value() && *entry.related_run == id)) {
      out.push_back(entry);
    }
  }
  return out;
}

Result<RunDiff> Engine::diff(ReconciliationRunId before, ReconciliationRunId after) const {
  ReconciliationRun left;
  ReconciliationRun right;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    bool found_left = false;
    bool found_right = false;
    for (const ReconciliationRun& run : runs_) {
      if (run.id() == before) {
        left = run;
        found_left = true;
      }
      if (run.id() == after) {
        right = run;
        found_right = true;
      }
    }
    if (!found_left) {
      return make_error(ErrorCode::NotFound, "no run with identity " + before.to_string(),
                        "run_id");
    }
    if (!found_right) {
      return make_error(ErrorCode::NotFound, "no run with identity " + after.to_string(), "run_id");
    }
  }
  return diff_runs(left, right, options_.limits.max_diff_entries);
}

Result<ReconciliationRun> Engine::revalidate(ReconciliationRunId prior, Tick instant,
                                             Generation expected,
                                             const IncarnationId& expected_incarnation) {
  ReconciliationRun predecessor;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    bool found = false;
    for (const ReconciliationRun& run : runs_) {
      if (run.id() == prior) {
        predecessor = run;
        found = true;
        break;
      }
    }
    if (!found) {
      return make_error(ErrorCode::NotFound, "no run with identity " + prior.to_string(),
                        "run_id");
    }
  }
  if (predecessor.resolution() == ResolutionState::Closed) {
    return make_error(ErrorCode::InvalidArgument,
                      "run " + prior.to_string() +
                          " is closed; reopen it before asking for a successor run",
                      "resolution");
  }

  RunRequest request = predecessor.request();
  request.generation = expected;
  request.evaluation_instant = instant;
  request.parent_run = prior;
  request.from_recovered_state = false;
  request.label = "revalidate";

  auto successor = reconcile_and_commit(request, expected, expected_incarnation);
  if (!successor.ok()) {
    return successor.error();
  }

  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (ReconciliationRun& run : runs_) {
      if (run.id() == prior) {
        run.set_resolution(ResolutionState::Superseded);
        run.set_superseded_by(successor.value().id());
      }
    }
    event.kind = HistoryEventKind::RunSuperseded;
    event.run = prior;
    event.related_run = successor.value().id();
    event.subject_digest = successor.value().run_digest();
    event.detail = history_detail("superseded_by_successor_run");

    if (store_.has_value()) {
      event.generation = store_->generation();
      event.attempt = store_->attempt();
      StoreImage image = stage_image();
      image.runs = runs_;
      auto status = append_history(event, &image);
      if (!status.ok()) {
        return status.error();
      }
      auto committed = store_->mutate(store_->generation(), store_->incarnation(),
                                      [&image](StoreImage& target) {
                                        target = image;
                                        return Status::success();
                                      });
      if (!committed.ok()) {
        append_history_rollback();
        return committed.error();
      }
    } else {
      event.generation = expected;
      event.attempt = attempt();
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        return status.error();
      }
      auto next = expected.next();
      if (!next.ok()) {
        append_history_rollback();
        return next.error();
      }
      generation_ = next.value();
    }
  }
  notify(event);
  return successor;
}

Result<ReopenTicket> Engine::reopen(ReconciliationRunId prior, Tick instant, Generation expected,
                                    const IncarnationId& expected_incarnation) {
  ReopenTicket ticket;
  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (!(expected == generation())) {
      return generation_mismatch(expected.value(), generation().value());
    }
    ReconciliationRun* target = nullptr;
    for (ReconciliationRun& run : runs_) {
      if (run.id() == prior) {
        target = &run;
        break;
      }
    }
    if (target == nullptr) {
      return make_error(ErrorCode::NotFound, "no run with identity " + prior.to_string(),
                        "run_id");
    }
    if (target->resolution() == ResolutionState::Reopened) {
      return make_error(ErrorCode::AlreadyExists,
                        "run " + prior.to_string() + " is already reopened", "resolution");
    }
    if (!is_terminal(target->resolution())) {
      return make_error(ErrorCode::InvalidArgument,
                        "run " + prior.to_string() + " is " + to_string(target->resolution()) +
                            "; only a closed or superseded run can be reopened",
                        "resolution");
    }
    const ResolutionState previous = target->resolution();
    target->set_resolution(ResolutionState::Reopened);

    event.kind = HistoryEventKind::RunReopened;
    event.generation = expected;
    event.attempt = attempt();
    event.run = prior;
    event.subject_digest = target->run_digest();
    event.detail = history_detail("reopened");
    event.recorded_at = instant;

    if (store_.has_value()) {
      StoreImage image = stage_image();
      image.runs = runs_;
      auto status = append_history(event, &image);
      if (!status.ok()) {
        target->set_resolution(previous);
        return status.error();
      }
      auto committed = store_->mutate(expected, expected_incarnation,
                                      [&image](StoreImage& target_image) {
                                        target_image = image;
                                        return Status::success();
                                      });
      if (!committed.ok()) {
        append_history_rollback();
        target->set_resolution(previous);
        return committed.error();
      }
      ticket.issued_at = store_->generation();
      ticket.incarnation = store_->incarnation();
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        target->set_resolution(previous);
        return status.error();
      }
      auto next = expected.next();
      if (!next.ok()) {
        append_history_rollback();
        target->set_resolution(previous);
        return next.error();
      }
      generation_ = next.value();
      ticket.issued_at = generation_;
      ticket.incarnation = incarnation_;
    }
    ticket.reopened_run = prior;
    ticket.issued_at_tick = instant;
  }
  notify(event);
  return ticket;
}

Result<ReconciliationRun> Engine::reconcile_successor(const ReopenTicket& ticket, Tick instant,
                                                      Generation expected,
                                                      const IncarnationId& expected_incarnation) {
  if (!(ticket.issued_at == expected)) {
    return make_error(ErrorCode::StaleAuthority,
                      "the reopen ticket was issued at generation " +
                          std::to_string(ticket.issued_at.value()) +
                          " and cannot be presented at generation " +
                          std::to_string(expected.value()),
                      "ticket");
  }
  if (!(ticket.incarnation == expected_incarnation)) {
    return make_error(ErrorCode::StaleAuthority,
                      "the reopen ticket was issued under a different incarnation", "ticket");
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    bool found = false;
    for (const ReconciliationRun& run : runs_) {
      if (run.id() == ticket.reopened_run) {
        found = true;
        if (run.resolution() != ResolutionState::Reopened) {
          return make_error(ErrorCode::StaleAuthority,
                            "the reopened run is no longer in the reopened state", "resolution");
        }
        break;
      }
    }
    if (!found) {
      return make_error(ErrorCode::NotFound,
                        "no run with identity " + ticket.reopened_run.to_string(), "run_id");
    }
  }
  return revalidate(ticket.reopened_run, instant, expected, expected_incarnation);
}

Status Engine::close_run(ReconciliationRunId run, Tick instant, Generation expected,
                         const IncarnationId& expected_incarnation) {
  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (!(expected == generation())) {
      return generation_mismatch(expected.value(), generation().value());
    }
    ReconciliationRun* target = nullptr;
    for (ReconciliationRun& candidate : runs_) {
      if (candidate.id() == run) {
        target = &candidate;
        break;
      }
    }
    if (target == nullptr) {
      return make_error(ErrorCode::NotFound, "no run with identity " + run.to_string(), "run_id");
    }
    if (target->resolution() == ResolutionState::Closed) {
      return make_error(ErrorCode::AlreadyExists, "run " + run.to_string() + " is already closed",
                        "resolution");
    }
    if (is_terminal(target->resolution())) {
      return make_error(ErrorCode::InvalidArgument,
                        "run " + run.to_string() + " is " + to_string(target->resolution()) +
                            " and cannot be closed",
                        "resolution");
    }
    const ResolutionState previous = target->resolution();
    target->set_resolution(ResolutionState::Closed);

    event.kind = HistoryEventKind::RunClosed;
    event.generation = expected;
    event.attempt = attempt();
    event.run = run;
    event.subject_digest = target->run_digest();
    event.detail = history_detail("closed");
    event.recorded_at = instant;

    if (store_.has_value()) {
      StoreImage image = stage_image();
      image.runs = runs_;
      auto status = append_history(event, &image);
      if (!status.ok()) {
        target->set_resolution(previous);
        return status;
      }
      auto committed = store_->mutate(expected, expected_incarnation,
                                      [&image](StoreImage& target_image) {
                                        target_image = image;
                                        return Status::success();
                                      });
      if (!committed.ok()) {
        append_history_rollback();
        target->set_resolution(previous);
        return committed.error();
      }
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        target->set_resolution(previous);
        return status;
      }
      auto next = expected.next();
      if (!next.ok()) {
        append_history_rollback();
        target->set_resolution(previous);
        return next.error();
      }
      generation_ = next.value();
    }
  }
  notify(event);
  return Status::success();
}

Status Engine::set_policy(ReconciliationPolicy policy, Generation expected,
                          const IncarnationId& expected_incarnation) {
  HistoryEntry event;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (stale_incarnation(expected_incarnation)) {
      return make_error(ErrorCode::StaleAuthority,
                        "the caller holds an incarnation that is no longer current", "incarnation");
    }
    if (!(expected == generation())) {
      return generation_mismatch(expected.value(), generation().value());
    }
    ReconciliationPolicy previous = options_.policy;
    options_.policy = std::move(policy);

    event.kind = HistoryEventKind::PolicyChanged;
    event.generation = expected;
    event.attempt = attempt();
    event.subject_digest = options_.policy.digest();
    event.detail = history_detail("policy_changed");

    if (store_.has_value()) {
      StoreImage image = stage_image();
      auto status = append_history(event, &image);
      if (!status.ok()) {
        options_.policy = std::move(previous);
        return status;
      }
      auto committed = store_->mutate(expected, expected_incarnation, [&image](StoreImage& target) {
        target = image;
        return Status::success();
      });
      if (!committed.ok()) {
        append_history_rollback();
        options_.policy = std::move(previous);
        return committed.error();
      }
    } else {
      auto status = append_history(event, nullptr);
      if (!status.ok()) {
        options_.policy = std::move(previous);
        return status;
      }
      auto next = expected.next();
      if (!next.ok()) {
        append_history_rollback();
        options_.policy = std::move(previous);
        return next.error();
      }
      generation_ = next.value();
    }
  }
  notify(event);
  return Status::success();
}

Status Engine::advance_epoch(Epoch new_epoch, Generation expected,
                             const IncarnationId& expected_incarnation) {
  if (!store_.has_value()) {
    return make_error(ErrorCode::Unsupported,
                      "an in-memory engine has no persisted epoch to advance", "epoch");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  if (!(expected == store_->generation())) {
    return generation_mismatch(expected.value(), store_->generation().value());
  }
  auto report = store_->advance_epoch(expected, expected_incarnation, new_epoch);
  if (!report.ok()) {
    return report.error();
  }
  return Status::success();
}

std::shared_ptr<const EvidenceSet> Engine::evidence_snapshot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evidence_;
}

std::shared_ptr<const AttributionSet> Engine::attribution_snapshot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return attributions_;
}

ReconcileStats Engine::last_stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_stats_;
}

}  // namespace summon::capacity_reconciliation
