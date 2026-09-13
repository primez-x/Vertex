#pragma once

#include "sketch/noninteractive_errors.hpp"

namespace sketch::testing {
// Compatibility wrapper for existing test call sites. The implementation is
// application-owned so production probes and the desktop smoke path do not
// depend on a test-only header.
inline void noninteractive_errors() {
    sketch::runtime::configure_noninteractive_errors();
}
} // namespace sketch::testing
