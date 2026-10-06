#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace sketch::desktop {
// Worker-only entry point. The caller passes the already trusted application
// root, never a document/environment-supplied model path. Exceptions publish no
// partial OCR reply. The outer broker owns process time/memory/cancellation.
[[nodiscard]] std::vector<std::byte> recognizeAssistanceOcrFrame(
    std::span<const std::byte> frame, const std::filesystem::path& application_root);
} // namespace sketch::desktop
