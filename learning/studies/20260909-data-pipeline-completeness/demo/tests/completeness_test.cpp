#include "completeness.h"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using completeness::AutomationPolicy;
using completeness::BranchCount;
using completeness::DecisionSignal;
using completeness::EvidenceKey;
using completeness::EvidenceState;
using completeness::MeasurementHealth;
using completeness::SegmentCount;
using completeness::SegmentTracker;

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_near(double actual, double expected, double tolerance) {
  if (!std::isfinite(actual) || !std::isfinite(expected) ||
      !std::isfinite(tolerance) || tolerance < 0.0 ||
      std::abs(actual - expected) > tolerance) {
    throw std::runtime_error("expected " + std::to_string(expected) + ", got " +
                             std::to_string(actual));
  }
}

void near_assertion_rejects_nan() {
  bool rejected = false;
  try {
    require_near(std::numeric_limits<double>::quiet_NaN(), 1.0, 1e-12);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  require(rejected, "NaN must never satisfy a numeric assertion");
}

void create_then_ack_is_idempotent() {
  SegmentTracker tracker;
  const EvidenceKey key{.root_bucket = 100, .payload_id = 7, .segment_id = 1};

  require(tracker.observe_create(key), "first create must change state");
  require(!tracker.observe_create(key), "duplicate create must be ignored");
  require(tracker.observe_acknowledgment(key), "first ack must complete state");
  require(!tracker.observe_acknowledgment(key),
          "duplicate ack must be ignored");
  require(tracker.state(key) == EvidenceState::Complete,
          "state must be complete");

  const auto snapshot = tracker.snapshot(100, 1);
  require(snapshot.creates == 1, "create count must remain one");
  require(snapshot.completed == 1, "completed count must remain one");
  require(snapshot.acknowledgments_before_create == 0, "no early ack remains");
  require_near(snapshot.ratio().value(), 1.0, 1e-12);
}

void ack_before_create_converges() {
  SegmentTracker tracker;
  const EvidenceKey key{.root_bucket = 100, .payload_id = 8, .segment_id = 1};

  require(tracker.observe_acknowledgment(key),
          "first early ack must be retained");
  require(!tracker.observe_acknowledgment(key),
          "duplicate early ack must be ignored");

  const auto pending = tracker.snapshot(100, 1);
  require(pending.creates == 0, "early ack is not a create");
  require(pending.completed == 0, "early ack is not complete");
  require(pending.acknowledgments_before_create == 1,
          "early ack must be visible");
  require(!pending.ratio().has_value(), "zero creates has no ratio");

  require(tracker.observe_create(key), "late create must complete early ack");
  require(tracker.state(key) == EvidenceState::Complete, "state must converge");
  require_near(tracker.snapshot(100, 1).ratio().value(), 1.0, 1e-12);
}

void every_short_event_sequence_converges() {
  for (unsigned int length = 1; length <= 8; ++length) {
    const auto sequence_count = 1U << length;
    for (unsigned int sequence = 0; sequence < sequence_count; ++sequence) {
      SegmentTracker tracker;
      const EvidenceKey key{
          .root_bucket = length,
          .payload_id = sequence,
          .segment_id = 2,
      };
      bool saw_create = false;
      bool saw_acknowledgment = false;

      for (unsigned int index = 0; index < length; ++index) {
        if ((sequence & (1U << index)) == 0) {
          tracker.observe_create(key);
          saw_create = true;
        } else {
          tracker.observe_acknowledgment(key);
          saw_acknowledgment = true;
        }
      }

      const auto expected = saw_create && saw_acknowledgment
                                ? EvidenceState::Complete
                                : (saw_create ? EvidenceState::Created
                                              : EvidenceState::AckBeforeCreate);
      require(tracker.state(key) == expected,
              "state must depend on evidence, not delivery order");
    }
  }
}

void root_bucket_is_part_of_identity() {
  SegmentTracker tracker;
  const EvidenceKey first{.root_bucket = 100, .payload_id = 9, .segment_id = 1};
  const EvidenceKey second{
      .root_bucket = 101, .payload_id = 9, .segment_id = 1};

  tracker.observe_create(first);
  tracker.observe_acknowledgment(second);

  require_near(tracker.snapshot(100, 1).ratio().value(), 0.0, 1e-12);
  require(tracker.snapshot(101, 1).acknowledgments_before_create == 1,
          "locally re-bucketed ack must not complete another cohort");

  tracker.observe_acknowledgment(first);
  tracker.observe_create(second);
  require_near(tracker.snapshot(100, 1).ratio().value(), 1.0, 1e-12);
  require_near(tracker.snapshot(101, 1).ratio().value(), 1.0, 1e-12);
}

void empty_segment_has_no_ratio() {
  SegmentTracker tracker;
  require(!tracker.snapshot(100, 1).ratio().has_value(),
          "empty ratio is unknown");
}

void conserved_sequential_chain_composes() {
  const std::array segments{
      SegmentCount{.creates = 100, .acknowledgments = 90},
      SegmentCount{.creates = 90, .acknowledgments = 81},
      SegmentCount{.creates = 81, .acknowledgments = 72},
  };

  const auto result = completeness::compose_sequential(segments);
  require(result.valid, result.reason);
  require_near(result.ratio, 0.72, 1e-12);
}

void cohort_mismatch_rejects_naive_multiplication() {
  const std::array segments{
      SegmentCount{.creates = 100, .acknowledgments = 90},
      SegmentCount{.creates = 100, .acknowledgments = 90},
  };

  const auto result = completeness::compose_sequential(segments);
  require(!result.valid, "different adjacent cohorts must be rejected");
  require(result.reason.find("conserve") != std::string::npos,
          "failure must identify the conservation contract");
}

void impossible_segment_count_is_rejected() {
  const std::array segments{
      SegmentCount{.creates = 10, .acknowledgments = 11},
  };
  require(!completeness::compose_sequential(segments).valid,
          "acknowledgments cannot exceed creates in a finalized cohort");
}

void branch_mass_does_not_hide_required_floor() {
  const std::array branches{
      BranchCount{
          .expected_obligations = 990,
          .acknowledged_obligations = 990,
          .required = false,
      },
      BranchCount{
          .expected_obligations = 10,
          .acknowledged_obligations = 0,
          .required = true,
      },
  };

  const auto summary = completeness::summarize_branches(branches);
  require(summary.valid, summary.reason);
  require_near(summary.delivery_mass_ratio, 0.99, 1e-12);
  require_near(summary.required_branch_floor.value(), 0.0, 1e-12);
}

void invalid_branch_count_is_rejected() {
  const std::array branches{
      BranchCount{
          .expected_obligations = 2,
          .acknowledged_obligations = 3,
          .required = true,
      },
  };
  require(!completeness::summarize_branches(branches).valid,
          "branch acknowledgments cannot exceed obligations");
}

void propagated_sampling_decision_preserves_pairs() {
  SegmentTracker tracker;
  constexpr std::uint64_t bucket = 200;
  constexpr std::uint32_t segment = 4;
  constexpr std::uint64_t seed = 0x5eed;

  for (std::uint64_t id = 0; id < 16'384; ++id) {
    const EvidenceKey key{bucket, id, segment};
    if (completeness::stable_sample(id, seed, 8)) {
      tracker.observe_create(key);
      tracker.observe_acknowledgment(key);
    }
  }

  const auto snapshot = tracker.snapshot(bucket, segment);
  require(snapshot.creates > 1'500, "sample must be large enough for the test");
  require_near(snapshot.ratio().value(), 1.0, 1e-12);
}

void independent_sampling_manufactures_incompleteness() {
  SegmentTracker tracker;
  constexpr std::uint64_t bucket = 201;
  constexpr std::uint32_t segment = 4;

  for (std::uint64_t id = 0; id < 16'384; ++id) {
    const EvidenceKey key{bucket, id, segment};
    if (completeness::stable_sample(id, 0x1111, 8)) {
      tracker.observe_create(key);
    }
    if (completeness::stable_sample(id, 0x9999, 8)) {
      tracker.observe_acknowledgment(key);
    }
  }

  const auto snapshot = tracker.snapshot(bucket, segment);
  require(snapshot.creates > 1'500, "create sample must be nontrivial");
  require(
      snapshot.ratio().value() < 0.25,
      "independent decisions should expose a severe false completeness loss");
  require(snapshot.acknowledgments_before_create > 1'000,
          "unmatched acknowledgments should reveal sampling inconsistency");
}

void automation_requires_measurement_health() {
  const AutomationPolicy policy{
      .minimum_completeness = 0.99,
      .minimum_samples = 1'000,
      .maximum_age_seconds = 60,
  };
  const DecisionSignal healthy{
      .completeness = 0.999,
      .sampled_creates = 2'000,
      .age_seconds = 20,
      .topology_known = true,
      .health = MeasurementHealth::Healthy,
  };
  require(completeness::safe_for_automation(healthy, policy),
          "healthy signal should pass");

  auto boundary = healthy;
  boundary.completeness = policy.minimum_completeness;
  boundary.sampled_creates = policy.minimum_samples;
  boundary.age_seconds = policy.maximum_age_seconds;
  require(completeness::safe_for_automation(boundary, policy),
          "inclusive policy boundaries should pass");

  auto unsafe = healthy;
  unsafe.health = MeasurementHealth::Invalidated;
  require(!completeness::safe_for_automation(unsafe, policy),
          "invalidated signal must fail closed");

  unsafe = healthy;
  unsafe.health = MeasurementHealth::ObserverDegraded;
  require(!completeness::safe_for_automation(unsafe, policy),
          "degraded observer must fail closed");

  unsafe = healthy;
  unsafe.completeness = 0.98;
  require(!completeness::safe_for_automation(unsafe, policy),
          "below-threshold completeness must fail closed");

  unsafe = healthy;
  unsafe.completeness = std::numeric_limits<double>::quiet_NaN();
  require(!completeness::safe_for_automation(unsafe, policy),
          "non-finite completeness must fail closed");

  unsafe = healthy;
  unsafe.completeness = 1.01;
  require(!completeness::safe_for_automation(unsafe, policy),
          "out-of-range completeness must fail closed");

  unsafe = healthy;
  unsafe.age_seconds = 61;
  require(!completeness::safe_for_automation(unsafe, policy),
          "stale signal must fail closed");

  unsafe = healthy;
  unsafe.sampled_creates = 999;
  require(!completeness::safe_for_automation(unsafe, policy),
          "undersampled signal must fail closed");

  unsafe = healthy;
  unsafe.topology_known = false;
  require(!completeness::safe_for_automation(unsafe, policy),
          "unknown topology must fail closed");
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"near_assertion_rejects_nan", near_assertion_rejects_nan},
      {"create_then_ack_is_idempotent", create_then_ack_is_idempotent},
      {"ack_before_create_converges", ack_before_create_converges},
      {"every_short_event_sequence_converges",
       every_short_event_sequence_converges},
      {"root_bucket_is_part_of_identity", root_bucket_is_part_of_identity},
      {"empty_segment_has_no_ratio", empty_segment_has_no_ratio},
      {"conserved_sequential_chain_composes",
       conserved_sequential_chain_composes},
      {
          "cohort_mismatch_rejects_naive_multiplication",
          cohort_mismatch_rejects_naive_multiplication,
      },
      {"impossible_segment_count_is_rejected",
       impossible_segment_count_is_rejected},
      {
          "branch_mass_does_not_hide_required_floor",
          branch_mass_does_not_hide_required_floor,
      },
      {"invalid_branch_count_is_rejected", invalid_branch_count_is_rejected},
      {
          "propagated_sampling_decision_preserves_pairs",
          propagated_sampling_decision_preserves_pairs,
      },
      {
          "independent_sampling_manufactures_incompleteness",
          independent_sampling_manufactures_incompleteness,
      },
      {"automation_requires_measurement_health",
       automation_requires_measurement_health},
  };

  std::size_t passed = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      return 1;
    }
  }

  std::cout << "PASS all " << passed << " tests\n";
  return 0;
}
