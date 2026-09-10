#include "sketch/detail/persistent_sequence.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sketch::detail::PersistentSequence;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "persistent_sequence_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void test_append_back_and_chunk_boundaries() {
    const auto live_before = PersistentSequence<int>::live_chunk_count();
    const auto allocations_before = PersistentSequence<int>::chunk_allocation_count();
    {
        PersistentSequence<int> values;
        for (int value = 0; value < 65; ++value) values.push_back(value);

        require(values.size() == 65 && values.front() == 0 && values.back() == 64,
                "append must retain chronological endpoints");
        require(values.chunk_count() == 3,
                "65 values must occupy three fixed 32-value chunks");
        const auto materialized = values.materialize();
        require(materialized.size() == 65, "materialization changed the sequence size");
        for (int value = 0; value < 65; ++value) {
            require(materialized[static_cast<std::size_t>(value)] == value,
                    "materialization changed chronological order");
        }
        require(PersistentSequence<int>::live_chunk_count() == live_before + 3,
                "replaced partial tails must not remain live without a shared version");
        require(PersistentSequence<int>::chunk_allocation_count() == allocations_before + 65,
                "each append must allocate exactly one candidate tail chunk");
    }
    require(PersistentSequence<int>::live_chunk_count() == live_before,
            "destroying a sequence must release every reachable chunk");
}

void test_replace_front_removal_and_copy_isolation() {
    PersistentSequence<int> source;
    for (int value = 0; value < 40; ++value) source.push_back(value);
    auto copy = source;
    const auto shared_root = source.root_identity();

    copy.replace_back(400);
    copy.pop_front();
    copy.pop_front();
    copy.push_back(401);

    require(source.root_identity() == shared_root && source.front() == 0 && source.back() == 39,
            "editing a copy must not mutate its source root");
    require(copy.root_identity() != shared_root && copy.size() == 39 && copy.front() == 2 &&
                copy.back() == 401,
            "replace, front removal and append must publish only in the edited copy");
    const auto expected = [] {
        std::vector<int> result;
        for (int value = 2; value < 39; ++value) result.push_back(value);
        result.push_back(400);
        result.push_back(401);
        return result;
    }();
    require(copy.materialize() == expected,
            "logical front removal must preserve the remaining chronological values");
}

struct ThrowingValue {
    int value{};
    static inline int copies_before_throw = -1;

    ThrowingValue() = default;
    explicit ThrowingValue(int source) : value(source) {}
    ThrowingValue(const ThrowingValue& other) : value(other.value) {
        if (copies_before_throw == 0) throw std::runtime_error("injected copy failure");
        if (copies_before_throw > 0) --copies_before_throw;
    }
    ThrowingValue(ThrowingValue&&) noexcept = default;
    ThrowingValue& operator=(const ThrowingValue&) = default;
    ThrowingValue& operator=(ThrowingValue&&) noexcept = default;

    bool operator==(const ThrowingValue&) const = default;
};

void test_failed_append_preserves_the_original_root() {
    PersistentSequence<ThrowingValue> values;
    values.push_back(ThrowingValue{10});
    values.push_back(ThrowingValue{20});
    const auto root_before = values.root_identity();
    const auto live_before = PersistentSequence<ThrowingValue>::live_chunk_count();

    ThrowingValue::copies_before_throw = 1;
    try {
        values.push_back(ThrowingValue{30});
        fail("injected tail-copy failure did not throw");
    } catch (const std::runtime_error&) {
    }
    ThrowingValue::copies_before_throw = -1;

    require(values.root_identity() == root_before && values.size() == 2 &&
                values.front().value == 10 && values.back().value == 20,
            "failed append must preserve the original sequence exactly");
    require(PersistentSequence<ThrowingValue>::live_chunk_count() == live_before,
            "failed append leaked a candidate chunk or predecessor reference");
}

void test_long_chain_destruction_is_iterative() {
    constexpr int value_count = 100'000;
    const auto live_before = PersistentSequence<int>::live_chunk_count();
    {
        PersistentSequence<int> values;
        for (int value = 0; value < value_count; ++value) values.push_back(value);
        require(values.size() == static_cast<std::size_t>(value_count) &&
                    values.chunk_count() == 3'125 && values.back() == value_count - 1,
                "long sequence did not retain the expected chunk chain");
    }
    require(PersistentSequence<int>::live_chunk_count() == live_before,
            "iterative destruction did not release the long chunk chain");
}

}  // namespace

int main() {
    test_append_back_and_chunk_boundaries();
    test_replace_front_removal_and_copy_isolation();
    test_failed_append_preserves_the_original_root();
    test_long_chain_destruction_is_iterative();
    return 0;
}
