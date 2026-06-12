#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace demo {

struct Run {
    size_t start;
    size_t length;
    int power;
};

struct SortStats {
    size_t mergeCount = 0;
    size_t comparisonCount = 0;
    size_t mergeWork = 0;
    size_t maxStackDepth = 0;
};

struct SortResult {
    std::vector<int> values;
    SortStats stats;
    std::string trace;
};

struct TestCase {
    std::string name;
    std::vector<int> values;
    bool verboseTrace;
};

std::string rangeText(const std::vector<int> &values, size_t start, size_t length) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < length; i++) {
        if (i != 0) {
            out << " ";
        }
        out << values[start + i];
    }
    out << "]";
    return out.str();
}

std::string sequenceText(const std::vector<int> &values) {
    return rangeText(values, 0, values.size());
}

size_t countRunAndNormalize(std::vector<int> &values, size_t start) {
    const size_t size = values.size();
    if (start + 1 >= size) {
        return 1;
    }

    size_t end = start + 2;
    if (values[start + 1] < values[start]) {
        while (end < size && values[end] < values[end - 1]) {
            end++;
        }
        std::reverse(values.begin() + static_cast<std::ptrdiff_t>(start),
                     values.begin() + static_cast<std::ptrdiff_t>(end));
    } else {
        while (end < size && values[end] >= values[end - 1]) {
            end++;
        }
    }
    return end - start;
}

int computePower(size_t leftStart, size_t leftLength, size_t rightLength, size_t totalLength) {
    /*
    `power` describes where the boundary between two adjacent runs belongs in a virtual complete merge tree.

    In this demo, power is computed with the CPython-style midpoint loop:
    - compare the midpoints of the left and right runs,
    - repeatedly zoom into the virtual binary partition of the whole input,
    - return the first tree level where the two midpoints fall on different sides.

    A smaller value means the boundary is closer to the root split, so it should usually wait for a larger,
    more balanced merge. A larger value means the boundary is deeper, so the adjacent runs are more local.
    The stack policy uses this value to avoid repeatedly appending tiny runs onto a large prefix.
    */
    if (leftLength == 0 || rightLength == 0 || totalLength == 0 || leftStart + leftLength + rightLength > totalLength) {
        throw std::invalid_argument("invalid adjacent runs for computePower");
    }

    size_t leftMidTwice = 2 * leftStart + leftLength;
    size_t rightMidTwice = leftMidTwice + leftLength + rightLength;
    const int maxPower = std::numeric_limits<size_t>::digits + 1;

    for (int power = 1; power <= maxPower; power++) {
        if (leftMidTwice >= totalLength) {
            leftMidTwice -= totalLength;
            rightMidTwice -= totalLength;
        } else if (rightMidTwice >= totalLength) {
            return power;
        }
        leftMidTwice <<= 1;
        rightMidTwice <<= 1;
    }

    throw std::logic_error("computePower failed to separate adjacent run midpoints");
}

bool validatePowerExamples() {
    return computePower(0, 4, 4, 8) == 1 && computePower(0, 4, 4, 16) == 2 && computePower(8, 4, 4, 16) == 2 &&
           computePower(0, 40, 22, 62) == 1;
}

void mergeStable(std::vector<int> &values, const Run &leftRun, const Run &rightRun, SortStats &stats) {
    std::vector<int> merged;
    merged.reserve(leftRun.length + rightRun.length);

    size_t left = leftRun.start;
    size_t right = rightRun.start;
    const size_t leftEnd = leftRun.start + leftRun.length;
    const size_t rightEnd = rightRun.start + rightRun.length;

    while (left < leftEnd && right < rightEnd) {
        stats.comparisonCount++;
        if (values[right] < values[left]) {
            merged.push_back(values[right++]);
        } else {
            merged.push_back(values[left++]);
        }
    }
    while (left < leftEnd) {
        merged.push_back(values[left++]);
    }
    while (right < rightEnd) {
        merged.push_back(values[right++]);
    }

    stats.mergeCount++;
    stats.mergeWork += merged.size();
    std::copy(merged.begin(), merged.end(), values.begin() + static_cast<std::ptrdiff_t>(leftRun.start));
}

void mergeAt(std::vector<int> &values, std::vector<Run> &runStack, size_t index, SortStats &stats,
             std::ostream &trace) {
    const Run leftRun = runStack[index];
    const Run rightRun = runStack[index + 1];

    trace << "merge: left(start=" << leftRun.start << ", len=" << leftRun.length << ", power=" << leftRun.power
          << ") right(start=" << rightRun.start << ", len=" << rightRun.length << ", power=" << rightRun.power << ")\n";

    mergeStable(values, leftRun, rightRun, stats);
    runStack[index] = Run{leftRun.start, leftRun.length + rightRun.length, leftRun.power};
    runStack.erase(runStack.begin() + static_cast<std::ptrdiff_t>(index + 1));
}

std::vector<Run> detectRuns(std::vector<int> &values, std::ostream &trace) {
    std::vector<Run> runs;
    size_t start = 0;
    while (start < values.size()) {
        const size_t length = countRunAndNormalize(values, start);
        runs.push_back(Run{start, length, 0});
        trace << "run: start=" << start << " len=" << length << " values=" << rangeText(values, start, length) << "\n";
        start += length;
    }
    return runs;
}

SortResult powerSort(std::vector<int> input, bool verboseTrace) {
    /*
    PowerSort uses natural runs and power-guided merges.

    +--------------------------+
    | input sequence<int>      |
    +------------+-------------+
                 |
                 v
    +--------------------------+
    | detect natural runs      |
    +------------+-------------+
                 |
                 v
    +--------------------------+
    | compute boundary power   |
    +------------+-------------+
                 |
                 v
    +--------------------------+---- lower new power ---->+--------------------------+
    | push run on stack        |                           | merge previous top runs |
    +------------+-------------+                           +--------------------------+
                 |
                 v
    +--------------------------+
    | collapse remaining stack |
    +--------------------------+
    */
    std::ostringstream trace;
    SortStats stats;
    if (input.empty()) {
        return SortResult{input, stats, ""};
    }

    auto detectedRuns = detectRuns(input, trace);
    std::vector<Run> runStack;
    runStack.push_back(Run{detectedRuns[0].start, detectedRuns[0].length, std::numeric_limits<int>::max()});
    stats.maxStackDepth = runStack.size();

    for (size_t i = 1; i < detectedRuns.size(); i++) {
        const auto &nextRun = detectedRuns[i];
        const int power = computePower(runStack.back().start, runStack.back().length, nextRun.length, input.size());
        trace << "power: boundary(leftStart=" << runStack.back().start << ", leftLen=" << runStack.back().length
              << ", rightLen=" << nextRun.length << ") -> " << power << "\n";

        while (runStack.size() > 1 && runStack.back().power > power) {
            mergeAt(input, runStack, runStack.size() - 2, stats, trace);
        }
        runStack.push_back(Run{nextRun.start, nextRun.length, power});
        stats.maxStackDepth = std::max(stats.maxStackDepth, runStack.size());
    }

    while (runStack.size() > 1) {
        mergeAt(input, runStack, runStack.size() - 2, stats, trace);
    }

    return SortResult{input, stats, verboseTrace ? trace.str() : ""};
}

SortResult sequentialRunMergeSort(std::vector<int> input) {
    std::ostringstream trace;
    SortStats stats;
    if (input.empty()) {
        return SortResult{input, stats, ""};
    }

    auto runs = detectRuns(input, trace);
    if (runs.empty()) {
        return SortResult{input, stats, ""};
    }

    Run mergedRun = runs[0];
    stats.maxStackDepth = 2;
    for (size_t i = 1; i < runs.size(); i++) {
        std::vector<Run> pair{mergedRun, runs[i]};
        mergeAt(input, pair, 0, stats, trace);
        mergedRun = pair[0];
    }

    return SortResult{input, stats, ""};
}

std::vector<int> stableSortBaseline(std::vector<int> input) {
    std::stable_sort(input.begin(), input.end());
    return input;
}

std::vector<int> makeNearlySortedWithLateBatch() {
    std::vector<int> values;
    for (int value = 0; value < 40; value++) {
        values.push_back(value);
    }
    for (int value = 10; value < 18; value++) {
        values.push_back(value);
    }
    for (int value = 41; value < 55; value++) {
        values.push_back(value);
    }
    return values;
}

std::vector<int> makeTimeWindowBatches() {
    return {100, 101, 102, 103, 20, 21, 22, 23, 70, 71, 72, 10, 11, 12, 13, 200, 201, 202};
}

std::vector<int> makeAlternatingServicePages() {
    std::vector<int> values;
    const std::vector<std::pair<int, int>> ranges{{300, 316}, {10, 18}, {200, 214}, {50, 58}, {120, 132}, {80, 88}};
    for (const auto &[begin, end] : ranges) {
        for (int value = begin; value < end; value++) {
            values.push_back(value);
        }
    }
    return values;
}

std::vector<int> makeReverseImportedChunk() {
    return {1, 2, 3, 4, 5, 30, 29, 28, 27, 26, 40, 41, 42, 6, 7, 8, 9};
}

void printStats(const std::string &name, const SortStats &stats) {
    std::cout << "  " << name << ": merges=" << stats.mergeCount << " comparisons=" << stats.comparisonCount
              << " mergeWork=" << stats.mergeWork << " maxStackDepth=" << stats.maxStackDepth << "\n";
}

bool runCase(const TestCase &testCase) {
    std::cout << "case: " << testCase.name << "\n";
    std::cout << "input: " << sequenceText(testCase.values) << "\n";

    const auto powersortResult = powerSort(testCase.values, testCase.verboseTrace);
    const auto sequentialResult = sequentialRunMergeSort(testCase.values);
    const auto baseline = stableSortBaseline(testCase.values);

    if (testCase.verboseTrace) {
        std::cout << "powersort trace\n" << powersortResult.trace;
    }

    std::cout << "output: " << sequenceText(powersortResult.values) << "\n";
    printStats("powersort", powersortResult.stats);
    printStats("sequential-run-merge", sequentialResult.stats);

    const bool valid = powersortResult.values == baseline && sequentialResult.values == baseline;
    std::cout << "lesson: ";
    if (powersortResult.stats.mergeWork < sequentialResult.stats.mergeWork) {
        std::cout << "power-guided merges reduce total merge work for this run layout\n";
    } else if (powersortResult.stats.maxStackDepth < sequentialResult.stats.maxStackDepth) {
        std::cout << "power-guided merges keep stack usage lower for this run layout\n";
    } else {
        std::cout << "power-guided merges do not improve this case; the baseline is already adequate\n";
    }
    std::cout << "\n";
    return valid;
}

} // namespace demo

int main() {
    if (!demo::validatePowerExamples()) {
        std::cerr << "computePower validation failed\n";
        return 1;
    }
    std::cout << "computePower sanity: ok\n\n";

    const std::vector<demo::TestCase> testCases{
        {"nearly sorted list with a late small batch", demo::makeNearlySortedWithLateBatch(), true},
        {"time-window batches from multiple producers", demo::makeTimeWindowBatches(), false},
        {"alternating service pages", demo::makeAlternatingServicePages(), false},
        {"reverse imported chunk inside sorted data", demo::makeReverseImportedChunk(), false},
    };

    bool allValid = true;
    for (const auto &testCase : testCases) {
        allValid = demo::runCase(testCase) && allValid;
        std::cout << "\n";
    }

    if (!allValid) {
        std::cerr << "validation failed\n";
        return 1;
    }

    std::cout << "validation ok\n";
    return 0;
}
