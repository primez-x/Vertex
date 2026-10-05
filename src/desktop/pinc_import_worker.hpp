#pragma once

#include "reference_import.hpp"
#include "sketch/pinc_import_candidate_codec.hpp"

namespace sketch::desktop {

inline constexpr std::uint64_t pincReplyLimit = 192ULL * 1024 * 1024;
inline constexpr std::uint64_t pincImageSourceLimit = 32ULL * 1024 * 1024;
inline constexpr std::uint64_t pincDecodedPixelLimit = 4096ULL * 4096;

struct PincRasterFrame {
    std::size_t page_index{};
    QByteArray source;
    std::vector<std::byte> pixel_frame; // Existing PSIR0002 raw RGBA frame.
};
struct PincDecodedUnderlay {
    std::size_t page_index{};
    DecodedReference reference;
};
struct PincWorkerProject {
    PincImportProject project;
    std::vector<PincDecodedUnderlay> underlays;
    // Broker-derived only, never present in a worker's serialized candidate.
    bool isolation_controls_attested{};
};

// Base64 conversion only, no image parser. The actual encoded-image decoder
// remains inside the sandbox. Unsupported descriptors cannot enter this lane.
[[nodiscard]] QByteArray pincUnderlaySourceBytes(const PincImportUnderlay& underlay);
[[nodiscard]] QString pincUnderlaySuffix(const PincImportUnderlay& underlay);

[[nodiscard]] std::vector<std::byte> encodePincWorkerReply(
    const PincImportProject& project, const std::vector<PincRasterFrame>& frames);
// Pure untrusted framing/typed-candidate/pixel validation. Production callers
// must attest the outer worker before invoking this codec.
[[nodiscard]] PincWorkerProject validatePincWorkerReply(std::span<const std::byte> bytes);
[[nodiscard]] PincWorkerProject importPincProjectBytes(
    std::span<const std::byte> source, WindowsImportWorkerOptions options,
    const ReferenceBroker& broker = run_windows_import_worker);

} // namespace sketch::desktop
