#include "noninteractive_errors.hpp"

namespace {
// Link this object directly into every test executable. The policy is installed
// before main, including fixture construction before main's try/catch. Tests
// must not use throwing cross-translation-unit static fixtures: their relative
// dynamic initialization order is unspecified.
struct TestProcessPolicy {
    TestProcessPolicy() { sketch::testing::noninteractive_errors(); }
};
const TestProcessPolicy policy;
} // namespace
