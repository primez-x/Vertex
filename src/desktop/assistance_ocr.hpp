#pragma once

#include "sketch/assistance_engine.hpp"
#include "sketch/windows_import_worker.hpp"
#include <filesystem>
#include <functional>

namespace sketch::desktop {

inline constexpr std::size_t assistanceOcrDimensionLimit = 4096;
inline constexpr std::size_t assistanceOcrPixelLimit = 4096ULL * 4096;
inline constexpr std::size_t assistanceOcrTextLimit = 16384;
inline constexpr std::size_t assistanceOcrRunLimit = 512;
inline constexpr std::size_t assistanceOcrReplyLimit = 1024 * 1024;
inline constexpr std::size_t assistanceOcrModelBytes = 4113088;
inline constexpr const char* assistanceOcrModelSha256 =
    "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2";
inline constexpr const char* assistanceOcrModelPath = "assets/assistance/ocr/eng.traineddata";
inline constexpr const char* assistanceOcrEnginePath = "assets/assistance/ocr-engine-v1.json";
inline constexpr const char* assistanceOcrLicensePath = "assets/assistance/ocr/LICENSE";
inline constexpr const char* assistanceOcrProducer = "tesseract-5.5.2/eng-tessdata-fast-4.1.0";

struct AssistanceOcrResult {
    std::string text;
    std::vector<AssistanceTextRun> runs;
    std::vector<AssistanceResource> resources;
    std::string producer;
    // Set only by the outer broker after its independently observed controls.
    bool isolation_controls_attested{};
};
using AssistanceOcrBroker = std::function<WindowsImportWorkerReport(const WindowsImportWorkerOptions&)>;

// The development runtime keeps assets beside the binary. An installed
// runtime has the fixed bin/ sibling layout. No current-directory or
// environment search participates in this choice.
[[nodiscard]] std::filesystem::path assistanceOcrApplicationRoot();

// VXOC0001 + little-endian uint32 width/height + tightly packed gray bytes.
[[nodiscard]] std::vector<std::byte> encodeAssistanceOcrFrame(const AssistanceRaster& raster);
[[nodiscard]] AssistanceRaster decodeAssistanceOcrFrame(std::span<const std::byte> frame);
[[nodiscard]] std::vector<std::byte> encodeAssistanceOcrReply(const AssistanceOcrResult& result);
// Pure validation cannot establish sandbox attestation.
[[nodiscard]] AssistanceOcrResult validateAssistanceOcrReply(std::span<const std::byte> reply);
// Verifies every fixed resource below the trusted application root. Failure
// means unavailable, never a download, environment lookup or alternative model.
[[nodiscard]] std::vector<AssistanceResource> verifiedAssistanceOcrResources(
    const std::filesystem::path& application_root);
[[nodiscard]] AssistanceOcrResult recognizeAssistanceRaster(
    const AssistanceRaster& raster, WindowsImportWorkerOptions options,
    const AssistanceOcrBroker& broker = run_windows_import_worker);

} // namespace sketch::desktop
