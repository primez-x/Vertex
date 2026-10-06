#pragma once

#include "sketch/annotation_catalog.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sketch {

inline constexpr std::size_t kTextLibraryEntryLimit = 1000;
inline constexpr std::size_t kTextLibraryByteLimit = 4 * 1024 * 1024;

struct TextLibraryEntry {
    std::string id;
    std::string name;
    std::string category;
    std::string content;
    AnnotationStyle style;
};

struct TextLibraryDocument {
    // v1 keeps implicit centered styles; v2 retains explicit text alignment.
    // Encoding promotes v1 to v2 when any entry uses a noncenter alignment.
    // v3 retains full opacity/pattern styles; older versions refuse these values.
    int version{1};
    std::vector<TextLibraryEntry> entries;
};

// Strict versioned codecs: unknown fields and invalid styles are refused.
void validate_text_library(const TextLibraryDocument& document);
[[nodiscard]] nlohmann::json encode_text_library(const TextLibraryDocument& document);
[[nodiscard]] TextLibraryDocument decode_text_library(const nlohmann::json& document);

} // namespace sketch
