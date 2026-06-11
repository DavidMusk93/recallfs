#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace demo {

struct Record {
    int key;
    int originalIndex;
};

struct Run {
    size_t start;
    size_t length;
    int power;
};

bool recordLess(const Record &lhs, const Record &rhs) {
    return lhs.key < rhs.key;
}

std::string recordText(const Record &record) {
    std::ostringstream out;
    out << "(" << record.key << "," << record.originalIndex << ")";
    return out.str();
}

std::string rangeText(const std::vector<Record> &records, size_t start, size_t length) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < length; i++) {
        if (i != 0) {
            out << " ";
        }
        out << recordText(records[start + i]);
    }
    out << "]";
    return out.str();
}

size_t countRunAndNormalize(std::vector<Record> &records, size_t start) {
    const size_t size = records.size();
    if (start + 1 >= size) {
        return 1;
    }

    size_t end = start + 2;
    if (records[start + 1].key < records[start].key) {
        while (end < size && records[end].key < records[end - 1].key) {
            end++;
        }
        std::reverse(records.begin() + static_cast<std::ptrdiff_t>(start),
                     records.begin() + static_cast<std::ptrdiff_t>(end));
    } else {
        while (end < size && records[end].key >= records[end - 1].key) {
            end++;
        }
    }
    return end - start;
}

int computePower(size_t leftStart, size_t leftLength, size_t rightLength, size_t totalLength) {
    size_t leftMidTwice = 2 * leftStart + leftLength;
    size_t rightMidTwice = leftMidTwice + leftLength + rightLength;
    int power = 0;

    while (true) {
        power++;
        if (leftMidTwice >= totalLength) {
            leftMidTwice -= totalLength;
            rightMidTwice -= totalLength;
        } else if (rightMidTwice >= totalLength) {
            return power;
        }
        leftMidTwice <<= 1;
        rightMidTwice <<= 1;
    }
}

void mergeStable(std::vector<Record> &records, const Run &leftRun, const Run &rightRun) {
    std::vector<Record> merged;
    merged.reserve(leftRun.length + rightRun.length);

    const auto leftBegin = records.begin() + static_cast<std::ptrdiff_t>(leftRun.start);
    const auto leftEnd = leftBegin + static_cast<std::ptrdiff_t>(leftRun.length);
    const auto rightBegin = records.begin() + static_cast<std::ptrdiff_t>(rightRun.start);
    const auto rightEnd = rightBegin + static_cast<std::ptrdiff_t>(rightRun.length);

    std::merge(leftBegin, leftEnd, rightBegin, rightEnd, std::back_inserter(merged), recordLess);
    std::copy(merged.begin(), merged.end(), leftBegin);
}

void mergeAt(std::vector<Record> &records, std::vector<Run> &runStack, size_t index, std::ostream &trace) {
    const Run leftRun = runStack[index];
    const Run rightRun = runStack[index + 1];

    trace << "merge runs: left(start=" << leftRun.start << ", len=" << leftRun.length << ", power=" << leftRun.power
          << ") right(start=" << rightRun.start << ", len=" << rightRun.length << ", power=" << rightRun.power << ")\n";

    mergeStable(records, leftRun, rightRun);
    runStack[index] = Run{leftRun.start, leftRun.length + rightRun.length, leftRun.power};
    runStack.erase(runStack.begin() + static_cast<std::ptrdiff_t>(index + 1));
}

std::vector<Record> powerSort(std::vector<Record> records, std::ostream &trace) {
    /*
    PowerSort demo flow.

    +-------------------------+
    | scan natural run        |
    +-----------+-------------+
                |
                v
    +-------------------------+
    | compute adjacent power  |
    +-----------+-------------+
                |
                v
    +-------------------------+---- power violation ---->+-------------------------+
    | push run to stack       |                           | merge stack top runs   |
    +-----------+-------------+                           +-------------------------+
                |
                v
    +-------------------------+
    | collapse final stack    |
    +-------------------------+

    The `power` calculation mirrors CPython's midpoint-loop idea at demo scale.
    It maps adjacent run midpoints onto a virtual complete merge tree.
    */
    if (records.empty()) {
        return records;
    }

    std::vector<Run> runStack;
    size_t start = 0;
    size_t runLength = countRunAndNormalize(records, start);
    runStack.push_back(Run{start, runLength, std::numeric_limits<int>::max()});
    trace << "run: start=" << start << " len=" << runLength << " values=" << rangeText(records, start, runLength)
          << "\n";
    start += runLength;

    while (start < records.size()) {
        runLength = countRunAndNormalize(records, start);
        const int power = computePower(runStack.back().start, runStack.back().length, runLength, records.size());

        trace << "run: start=" << start << " len=" << runLength << " power=" << power
              << " values=" << rangeText(records, start, runLength) << "\n";

        while (runStack.size() > 1 && runStack.back().power > power) {
            mergeAt(records, runStack, runStack.size() - 2, trace);
        }

        runStack.push_back(Run{start, runLength, power});
        start += runLength;
    }

    while (runStack.size() > 1) {
        mergeAt(records, runStack, runStack.size() - 2, trace);
    }

    return records;
}

bool sameRecords(const std::vector<Record> &lhs, const std::vector<Record> &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); i++) {
        if (lhs[i].key != rhs[i].key || lhs[i].originalIndex != rhs[i].originalIndex) {
            return false;
        }
    }
    return true;
}

void printRecords(const std::string &label, const std::vector<Record> &records) {
    std::cout << label << "\n";
    for (const auto &record : records) {
        std::cout << "  key=" << record.key << " originalIndex=" << record.originalIndex << "\n";
    }
}

} // namespace demo

int main() {
    const std::vector<demo::Record> input{{1, 0},  {2, 1}, {3, 2}, {9, 3},  {8, 4},   {7, 5},  {10, 6},
                                          {11, 7}, {4, 8}, {5, 9}, {6, 10}, {12, 11}, {3, 12}, {13, 13}};

    std::ostringstream trace;
    const auto powersortResult = demo::powerSort(input, trace);

    auto stableSortResult = input;
    std::stable_sort(stableSortResult.begin(), stableSortResult.end(), demo::recordLess);

    std::cout << "powersort trace\n" << trace.str();
    demo::printRecords("powersort result", powersortResult);
    demo::printRecords("stable_sort baseline", stableSortResult);

    if (!demo::sameRecords(powersortResult, stableSortResult)) {
        std::cerr << "validation failed\n";
        return 1;
    }

    std::cout << "validation ok\n";
    return 0;
}
