#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace sketch {

using Sha256Sink = std::function<void(std::span<const std::byte>)>;

// The producer invokes the borrowed sink synchronously; neither the sink nor
// its input spans may escape the call. Empty input hashes the empty byte stream.
// Input is fed to Windows BCrypt through a fixed-size staging buffer.
[[nodiscard]] std::string sha256_hex_stream(
    const std::function<void(const Sha256Sink&)>& produce);

} // namespace sketch
