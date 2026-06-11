#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace demo {

struct Item {
	uint32_t value;
	size_t index;
};

std::array<uint8_t, 4> EncodeUint32AsSortableBytes(uint32_t value) {
	// DuckDB idea extracted from duckdb/common/radix.hpp:
	// unsigned integers are stored in big-endian byte order so that
	// lexicographic byte comparison matches numeric ordering.
	return {static_cast<uint8_t>((value >> 24) & 0xFF), static_cast<uint8_t>((value >> 16) & 0xFF),
	        static_cast<uint8_t>((value >> 8) & 0xFF), static_cast<uint8_t>(value & 0xFF)};
}

uint32_t ExtractBits(uint32_t value, int shift, uint32_t mask) {
	return (value >> shift) & mask;
}

std::vector<Item> SortBaseline(const std::vector<uint32_t> &values, size_t k, bool largest) {
	std::vector<Item> items;
	for (size_t i = 0; i < values.size(); i++) {
		items.push_back({values[i], i});
	}
	auto cmp = [largest](const Item &lhs, const Item &rhs) {
		if (lhs.value != rhs.value) {
			return largest ? lhs.value > rhs.value : lhs.value < rhs.value;
		}
		return lhs.index < rhs.index;
	};
	std::sort(items.begin(), items.end(), cmp);
	if (items.size() > k) {
		items.resize(k);
	}
	return items;
}

std::vector<Item> RadixTopK(const std::vector<uint32_t> &values, size_t k, uint32_t bits_per_iter, bool largest) {
	if (bits_per_iter == 0 || bits_per_iter > 8 || 32 % bits_per_iter != 0) {
		throw std::invalid_argument("bits_per_iter must be one of 1, 2, 4, 8");
	}
	if (k > values.size()) {
		throw std::invalid_argument("k must be <= input size");
	}

	const uint32_t num_buckets = 1U << bits_per_iter;
	const uint32_t mask = num_buckets - 1;

	std::vector<Item> current;
	std::vector<Item> candidates;
	current.reserve(k);
	candidates.reserve(values.size());
	for (size_t i = 0; i < values.size(); i++) {
		candidates.push_back({values[i], i});
	}

	/*
	Radix Top-K as traversal over an implicit radix tree.

	+---------------------------+
	| candidates at tree level  |
	+-------------+-------------+
	              |
	              | extract next B bits from sortable key
	              v
	+---------------------------+
	| bucket[0..2^B-1] counts   |
	+-------------+-------------+
	              |
	              | prefix count in desired order
	              v
	+---------------------------+---- before border ---->+---------------------------+
	| find boundary bucket      |                         | guaranteed top-k prefix   |
	+-------------+-------------+                         +---------------------------+
	              |
	              | border bucket only
	              v
	+---------------------------+
	| next-level candidates     |
	+---------------------------+

	For top-k smallest, traversal order is bucket 0 -> bucket max.
	For top-k largest, traversal order is bucket max -> bucket 0.
	*/
	for (int shift = 32 - static_cast<int>(bits_per_iter); shift >= 0 && current.size() < k;
	     shift -= static_cast<int>(bits_per_iter)) {
		std::vector<size_t> counts(num_buckets, 0);
		for (const auto &item : candidates) {
			counts[ExtractBits(item.value, shift, mask)]++;
		}

		const size_t need = k - current.size();
		uint32_t border_bucket = 0;
		size_t prefix_before_border = 0;
		size_t prefix = 0;

		for (uint32_t rank = 0; rank < num_buckets; rank++) {
			const uint32_t bucket = largest ? (num_buckets - 1 - rank) : rank;
			const size_t next_prefix = prefix + counts[bucket];
			if (next_prefix >= need) {
				border_bucket = bucket;
				prefix_before_border = prefix;
				break;
			}
			prefix = next_prefix;
		}

		std::vector<Item> next_candidates;
		next_candidates.reserve(counts[border_bucket]);

		for (const auto &item : candidates) {
			const auto bucket = ExtractBits(item.value, shift, mask);
			const bool before_border = largest ? bucket > border_bucket : bucket < border_bucket;
			if (before_border) {
				current.push_back(item);
			} else if (bucket == border_bucket) {
				next_candidates.push_back(item);
			}
		}

		if (prefix_before_border == need) {
			break;
		}
		candidates.swap(next_candidates);
	}

	const size_t remaining = k - current.size();
	for (size_t i = 0; i < remaining && i < candidates.size(); i++) {
		current.push_back(candidates[i]);
	}

	auto cmp = [largest](const Item &lhs, const Item &rhs) {
		if (lhs.value != rhs.value) {
			return largest ? lhs.value > rhs.value : lhs.value < rhs.value;
		}
		return lhs.index < rhs.index;
	};
	std::sort(current.begin(), current.end(), cmp);
	return current;
}

void PrintItems(const std::string &label, const std::vector<Item> &items) {
	std::cout << label << "\n";
	for (const auto &item : items) {
		const auto bytes = EncodeUint32AsSortableBytes(item.value);
		std::cout << "  value=" << std::setw(2) << item.value << " index=" << item.index << " key=[";
		for (size_t i = 0; i < bytes.size(); i++) {
			if (i != 0) {
				std::cout << ' ';
			}
			std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
		}
		std::cout << std::dec << std::setfill(' ') << "]\n";
	}
}

bool SameItems(const std::vector<Item> &lhs, const std::vector<Item> &rhs) {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (size_t i = 0; i < lhs.size(); i++) {
		if (lhs[i].value != rhs[i].value || lhs[i].index != rhs[i].index) {
			return false;
		}
	}
	return true;
}

} // namespace demo

int main() {
	const std::vector<uint32_t> values {12, 4, 1, 8, 6, 5, 13, 0, 14, 6, 2, 11};
	const size_t k = 4;
	const uint32_t bits_per_iter = 2;

	const auto radix_smallest = demo::RadixTopK(values, k, bits_per_iter, false);
	const auto sort_smallest = demo::SortBaseline(values, k, false);
	const auto radix_largest = demo::RadixTopK(values, k, bits_per_iter, true);
	const auto sort_largest = demo::SortBaseline(values, k, true);

	demo::PrintItems("radix top-k smallest", radix_smallest);
	demo::PrintItems("sort baseline smallest", sort_smallest);
	demo::PrintItems("radix top-k largest", radix_largest);
	demo::PrintItems("sort baseline largest", sort_largest);

	if (!demo::SameItems(radix_smallest, sort_smallest) || !demo::SameItems(radix_largest, sort_largest)) {
		std::cerr << "validation failed\n";
		return 1;
	}

	std::cout << "validation ok\n";
	return 0;
}
