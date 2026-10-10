#pragma once

#include "sketch/corner_window_transfer.hpp"
#include "sketch/phase_roof_opening_edit.hpp"

#include <optional>
#include <string_view>

namespace sketch {

// Captured source content only. No destination, Document, command, placement,
// history, or publication authority is conveyed by admission or decoding.
struct MixedClipboardTransfer {
    std::optional<nlohmann::json> ordinary;
    std::vector<CornerWindowTransfer> corners;
    std::vector<Entity> catalogs;
    std::vector<RoofOpeningCloneSource> skylights;
};

// Closed vertex-mixed-clipboard version 1: exactly format, version, ordinary,
// corners, catalogs, skylights. ordinary is null or the closed legacy
// sketch.document.clipboard version 1 packet. Corner rows always contain owner,
// walls, cuts, dimensions; skylight rows contain roof and opening_id. Every raw
// Entity envelope contains exactly id/type/properties/required/extensions.
// At least two nonempty family lanes are required. Limits: 4 MiB compact wire
// bytes, 100000 JSON values, depth 64, 1000 semantic members, 4096 entity rows,
// 128 corners, 2048 transported dimensions, 128 ordinary rows/catalogs.
// Preflight uses an encoded-size upper bound before copying/dumping/codecs;
// unusually dense numeric content near the byte ceiling can refuse early.
// Ordinary family remapping/semantics and full skylight fit/native validation
// remain the existing authoritative destination producers' responsibility.
// An ordinary transported host may also appear as an exact passive host;
// root must resolve selected-host/child dominance and actual destination mapping
// before granting any clone authority. Shared material catalogs admit separate
// referenced subsets with exact shared raw definitions and compatible model
// dialect/opaque fields; source carrier context remains local to its lane.
void validate_mixed_clipboard_transfer(const MixedClipboardTransfer& transfer);
[[nodiscard]] nlohmann::json encode_mixed_clipboard_transfer(const MixedClipboardTransfer& transfer);
[[nodiscard]] MixedClipboardTransfer decode_mixed_clipboard_transfer(const nlohmann::json& value);
// Bounds wire bytes before parsing and parser depth/values before materializing
// untrusted nested content. Then performs the same closed passive admission.
[[nodiscard]] MixedClipboardTransfer decode_mixed_clipboard_transfer(std::string_view encoded);

} // namespace sketch
