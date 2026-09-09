#include "completeness.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

completeness::SegmentSnapshot
sampled_pipeline(std::uint64_t create_seed, std::uint64_t acknowledgment_seed) {
  completeness::SegmentTracker tracker;
  constexpr std::uint64_t bucket = 300;
  constexpr std::uint32_t segment = 7;

  for (std::uint64_t id = 0; id < 16'384; ++id) {
    const completeness::EvidenceKey key{bucket, id, segment};
    if (completeness::stable_sample(id, create_seed, 8)) {
      tracker.observe_create(key);
    }
    if (completeness::stable_sample(id, acknowledgment_seed, 8)) {
      tracker.observe_acknowledgment(key);
    }
  }
  return tracker.snapshot(bucket, segment);
}

} // namespace

int main() {
  const std::array conserved_chain{
      completeness::SegmentCount{100, 90},
      completeness::SegmentCount{90, 81},
      completeness::SegmentCount{81, 72},
  };
  const std::array mismatched_chain{
      completeness::SegmentCount{100, 90},
      completeness::SegmentCount{100, 90},
  };
  const std::array branches{
      completeness::BranchCount{990, 990, false},
      completeness::BranchCount{10, 0, true},
  };

  const auto conserved = completeness::compose_sequential(conserved_chain);
  const auto mismatched = completeness::compose_sequential(mismatched_chain);
  const auto branch_summary = completeness::summarize_branches(branches);
  const auto stable = sampled_pipeline(0x5eed, 0x5eed);
  const auto independent = sampled_pipeline(0x1111, 0x9999);

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "conserved_chain.valid=" << std::boolalpha << conserved.valid
            << '\n';
  std::cout << "conserved_chain.ratio=" << conserved.ratio << '\n';
  std::cout << "mismatched_chain.valid=" << mismatched.valid << '\n';
  std::cout << "mismatched_chain.reason=" << mismatched.reason << '\n';
  std::cout << "branches.delivery_mass=" << branch_summary.delivery_mass_ratio
            << '\n';
  std::cout << "branches.required_floor="
            << branch_summary.required_branch_floor.value() << '\n';
  std::cout << "stable_sampling.creates=" << stable.creates << '\n';
  std::cout << "stable_sampling.completeness=" << stable.ratio().value()
            << '\n';
  std::cout << "independent_sampling.creates=" << independent.creates << '\n';
  std::cout << "independent_sampling.completeness="
            << independent.ratio().value() << '\n';
  std::cout << "independent_sampling.early_acks="
            << independent.acknowledgments_before_create << '\n';
  return 0;
}
