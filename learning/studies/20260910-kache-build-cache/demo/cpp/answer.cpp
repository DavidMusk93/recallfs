#include "answer.h"

#include <array>
#include <cassert>
#include <numeric>

int answer() {
    constexpr std::array<int, 4> values{10, 20, 7, 5};
    return std::accumulate(values.begin(), values.end(), 0);
}

int main() {
    assert(answer() == 42);
    return 0;
}
