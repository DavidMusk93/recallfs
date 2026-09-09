#ifndef COMPLETENESS_H
#define COMPLETENESS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace completeness {

struct EvidenceKey {
  std::uint64_t root_bucket;
  std::uint64_t payload_id;
  std::uint32_t segment_id;

  bool operator==(const EvidenceKey &) const = default;
};

struct EvidenceKeyHash {
  std::size_t operator()(const EvidenceKey &key) const noexcept;
};

// AckBeforeCreate preserves a reordered acknowledgment until its create
// arrives.
enum class EvidenceState : std::uint8_t {
  Created,
  AckBeforeCreate,
  Complete,
};

struct SegmentSnapshot {
  std::uint64_t creates = 0;
  std::uint64_t completed = 0;
  std::uint64_t acknowledgments_before_create = 0;

  [[nodiscard]] std::optional<double> ratio() const;
};

class SegmentTracker {
public:
  bool observe_create(const EvidenceKey &key);
  bool observe_acknowledgment(const EvidenceKey &key);

  [[nodiscard]] std::optional<EvidenceState>
  state(const EvidenceKey &key) const;
  [[nodiscard]] SegmentSnapshot snapshot(std::uint64_t root_bucket,
                                         std::uint32_t segment_id) const;

private:
  std::unordered_map<EvidenceKey, EvidenceState, EvidenceKeyHash> states_;
};

struct SegmentCount {
  std::uint64_t creates;
  std::uint64_t acknowledgments;
};

struct CompositionResult {
  bool valid;
  double ratio;
  std::string reason;
};

// Multiplication is valid only when adjacent segments conserve one cohort.
[[nodiscard]] CompositionResult
compose_sequential(std::span<const SegmentCount> segments);

struct BranchCount {
  std::uint64_t expected_obligations;
  std::uint64_t acknowledged_obligations;
  bool required;
};

struct BranchSummary {
  bool valid;
  double delivery_mass_ratio;
  std::optional<double> required_branch_floor;
  std::string reason;
};

// Delivery mass and required-branch health answer different questions.
[[nodiscard]] BranchSummary
summarize_branches(std::span<const BranchCount> branches);

// A propagated seed gives every segment the same decision for one payload.
[[nodiscard]] bool stable_sample(std::uint64_t payload_id,
                                 std::uint64_t propagated_seed,
                                 std::uint64_t one_in);

enum class MeasurementHealth : std::uint8_t {
  Healthy,
  Invalidated,
  ObserverDegraded,
};

struct DecisionSignal {
  double completeness;
  std::uint64_t sampled_creates;
  std::uint64_t age_seconds;
  bool topology_known;
  MeasurementHealth health;
};

struct AutomationPolicy {
  double minimum_completeness;
  std::uint64_t minimum_samples;
  std::uint64_t maximum_age_seconds;
};

[[nodiscard]] bool safe_for_automation(const DecisionSignal &signal,
                                       const AutomationPolicy &policy);

} // namespace completeness

#endif
