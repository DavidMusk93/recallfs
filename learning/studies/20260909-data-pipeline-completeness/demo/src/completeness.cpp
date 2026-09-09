#include "completeness.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace completeness {
namespace {

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

CompositionResult invalid_composition(std::string reason) {
  return CompositionResult{
      .valid = false,
      .ratio = std::numeric_limits<double>::quiet_NaN(),
      .reason = std::move(reason),
  };
}

BranchSummary invalid_branches(std::string reason) {
  return BranchSummary{
      .valid = false,
      .delivery_mass_ratio = std::numeric_limits<double>::quiet_NaN(),
      .required_branch_floor = std::nullopt,
      .reason = std::move(reason),
  };
}

} // namespace

std::size_t EvidenceKeyHash::operator()(const EvidenceKey &key) const noexcept {
  const auto bucket_hash = mix64(key.root_bucket);
  const auto payload_hash = mix64(key.payload_id ^ bucket_hash);
  return static_cast<std::size_t>(
      mix64(static_cast<std::uint64_t>(key.segment_id) ^ payload_hash));
}

std::optional<double> SegmentSnapshot::ratio() const {
  if (creates == 0) {
    return std::nullopt;
  }
  return static_cast<double>(completed) / static_cast<double>(creates);
}

bool SegmentTracker::observe_create(const EvidenceKey &key) {
  const auto [it, inserted] = states_.try_emplace(key, EvidenceState::Created);
  if (inserted) {
    return true;
  }
  if (it->second == EvidenceState::AckBeforeCreate) {
    it->second = EvidenceState::Complete;
    return true;
  }
  return false;
}

bool SegmentTracker::observe_acknowledgment(const EvidenceKey &key) {
  const auto [it, inserted] =
      states_.try_emplace(key, EvidenceState::AckBeforeCreate);
  if (inserted) {
    return true;
  }
  if (it->second == EvidenceState::Created) {
    it->second = EvidenceState::Complete;
    return true;
  }
  return false;
}

std::optional<EvidenceState>
SegmentTracker::state(const EvidenceKey &key) const {
  const auto found = states_.find(key);
  if (found == states_.end()) {
    return std::nullopt;
  }
  return found->second;
}

SegmentSnapshot SegmentTracker::snapshot(std::uint64_t root_bucket,
                                         std::uint32_t segment_id) const {
  SegmentSnapshot result;
  for (const auto &[key, state] : states_) {
    if (key.root_bucket != root_bucket || key.segment_id != segment_id) {
      continue;
    }
    switch (state) {
    case EvidenceState::Created:
      ++result.creates;
      break;
    case EvidenceState::AckBeforeCreate:
      ++result.acknowledgments_before_create;
      break;
    case EvidenceState::Complete:
      ++result.creates;
      ++result.completed;
      break;
    }
  }
  return result;
}

CompositionResult compose_sequential(std::span<const SegmentCount> segments) {
  if (segments.empty()) {
    return invalid_composition("a sequential path must contain a segment");
  }
  if (segments.front().creates == 0) {
    return invalid_composition("the root cohort must contain a create");
  }

  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto &segment = segments[index];
    if (segment.acknowledgments > segment.creates) {
      return invalid_composition("segment " + std::to_string(index) +
                                 " acknowledges more items than it creates");
    }
    if (index > 0 && segment.creates != segments[index - 1].acknowledgments) {
      return invalid_composition(
          "adjacent segments do not conserve the same cohort");
    }
  }

  return CompositionResult{
      .valid = true,
      .ratio = static_cast<double>(segments.back().acknowledgments) /
               static_cast<double>(segments.front().creates),
      .reason = {},
  };
}

BranchSummary summarize_branches(std::span<const BranchCount> branches) {
  if (branches.empty()) {
    return invalid_branches("a branch set must not be empty");
  }

  long double total_expected = 0.0L;
  long double total_acknowledged = 0.0L;
  std::optional<double> required_floor;

  for (std::size_t index = 0; index < branches.size(); ++index) {
    const auto &branch = branches[index];
    if (branch.expected_obligations == 0) {
      return invalid_branches("branch " + std::to_string(index) +
                              " has no obligations");
    }
    if (branch.acknowledged_obligations > branch.expected_obligations) {
      return invalid_branches(
          "branch " + std::to_string(index) +
          " acknowledges more than its expected obligations");
    }

    total_expected += static_cast<long double>(branch.expected_obligations);
    total_acknowledged +=
        static_cast<long double>(branch.acknowledged_obligations);

    if (branch.required) {
      const auto ratio = static_cast<double>(branch.acknowledged_obligations) /
                         static_cast<double>(branch.expected_obligations);
      required_floor = required_floor.has_value()
                           ? std::min(required_floor.value(), ratio)
                           : ratio;
    }
  }

  return BranchSummary{
      .valid = true,
      .delivery_mass_ratio =
          static_cast<double>(total_acknowledged / total_expected),
      .required_branch_floor = required_floor,
      .reason = {},
  };
}

bool stable_sample(std::uint64_t payload_id, std::uint64_t propagated_seed,
                   std::uint64_t one_in) {
  if (one_in == 0) {
    return false;
  }
  return mix64(payload_id ^ propagated_seed) % one_in == 0;
}

bool safe_for_automation(const DecisionSignal &signal,
                         const AutomationPolicy &policy) {
  const bool ratio_valid =
      std::isfinite(signal.completeness) && signal.completeness >= 0.0 &&
      signal.completeness <= 1.0 &&
      std::isfinite(policy.minimum_completeness) &&
      policy.minimum_completeness >= 0.0 && policy.minimum_completeness <= 1.0;

  return ratio_valid && signal.health == MeasurementHealth::Healthy &&
         signal.topology_known &&
         signal.sampled_creates >= policy.minimum_samples &&
         signal.age_seconds <= policy.maximum_age_seconds &&
         signal.completeness >= policy.minimum_completeness;
}

} // namespace completeness
