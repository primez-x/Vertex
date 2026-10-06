#pragma once

#include "sketch/assistance_engine.hpp"
#include "sketch/windows_import_worker.hpp"
#include <filesystem>
#include <functional>
#include <cstdint>
#include <stdexcept>

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

enum class AssistanceOcrFailureStage {
    frame_decode, resource_metadata, resource_read, resource_validation, resource_hash,
    model_read, model_hash, engine_version, engine_init, engine_languages, recognition, reply
};
// Diagnostic values are fixed stage identifiers and an optional numeric OS
// error. No resource path, model bytes, recognized text or parser excerpt is
// retained. This exception never represents a successful worker reply.
class AssistanceOcrFailure final : public std::runtime_error {
public:
    explicit AssistanceOcrFailure(AssistanceOcrFailureStage stage, std::uint32_t os_error = 0)
        : std::runtime_error("Offline assistance OCR recognition failed."), stage_(stage), os_error_(os_error) {}
    [[nodiscard]] AssistanceOcrFailureStage stage() const noexcept { return stage_; }
    [[nodiscard]] const char* stage_code() const noexcept;
    [[nodiscard]] std::uint32_t os_error() const noexcept { return os_error_; }
private:
    AssistanceOcrFailureStage stage_;
    std::uint32_t os_error_;
};

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
// Windows requires the actual stored spelling of every directory/file component;
// only the DOS drive letter is case-normalized. Caller paths are never resolved
// to a different spelling before admission, which could conceal a redirect.
[[nodiscard]] std::vector<AssistanceResource> verifiedAssistanceOcrResources(
    const std::filesystem::path& application_root);
struct AssistanceOcrVerifiedBundle {
    std::vector<AssistanceResource> resources;
    std::vector<char> model;
};
// Worker admission returns the exact verified in-memory model, avoiding a
// second pathname open between resource verification and engine initialization.
[[nodiscard]] AssistanceOcrVerifiedBundle loadVerifiedAssistanceOcrBundle(
    const std::filesystem::path& application_root);
[[nodiscard]] AssistanceOcrResult recognizeAssistanceRaster(
    const AssistanceRaster& raster, WindowsImportWorkerOptions options,
    const AssistanceOcrBroker& broker = run_windows_import_worker);

} // namespace sketch::desktop
