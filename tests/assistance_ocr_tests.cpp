#include "assistance_ocr.hpp"
#include "support/noninteractive_errors.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Invalid OCR input/reply must be refused");
}
template<class F> void cancels(F f) {
    bool cancelled = false;
    try { f(); } catch (const std::runtime_error& e) {
        cancelled = std::string(e.what()) == "Assistance OCR cancelled.";
    }
    require(cancelled, "OCR cancellation must preserve its stable outcome");
}
sketch::WindowsImportWorkerReport attested(std::vector<std::byte> output) {
    sketch::WindowsImportWorkerReport r;
    r.status = sketch::WindowsImportWorkerStatus::completed;
    r.completed = r.launched = r.app_container_verified = r.restricted_token_verified =
        r.network_denial_verified = r.job_limits_verified = r.parent_exit_kill_verified =
        r.job_membership_verified =
        r.brokered_handles_verified = r.private_temporary_root_verified =
        r.immutable_module_roots_verified = r.fixed_search_applied = r.proj_offline_applied = true;
    r.output = std::move(output); return r;
}
std::vector<std::byte> wire(const std::string& text) {
    const auto* start = reinterpret_cast<const std::byte*>(text.data());
    return {start, start + text.size()};
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QCoreApplication app(argc, argv);
    try {
        using namespace sketch;
        using namespace sketch::desktop;
        AssistanceRaster raster; raster.width = 2; raster.height = 2;
        raster.luminance = {255, 0, 20, 100};
        auto frame = encodeAssistanceOcrFrame(raster);
        const auto decoded = decodeAssistanceOcrFrame(frame);
        require(decoded.width == 2 && decoded.height == 2 && decoded.luminance == raster.luminance,
                "Grayscale framing must preserve exact pixels");
        auto invalid = frame; invalid.push_back(std::byte{});
        rejects([&] { (void)decodeAssistanceOcrFrame(invalid); });
        invalid = frame; invalid.pop_back();
        rejects([&] { (void)decodeAssistanceOcrFrame(invalid); });
        invalid = frame; invalid[0] = std::byte{'X'};
        rejects([&] { (void)decodeAssistanceOcrFrame(invalid); });
        invalid = frame; qToLittleEndian<quint32>(4097, reinterpret_cast<uchar*>(invalid.data()) + 8);
        rejects([&] { (void)decodeAssistanceOcrFrame(invalid); });
        auto bad_raster = raster; bad_raster.luminance.pop_back();
        rejects([&] { (void)encodeAssistanceOcrFrame(bad_raster); });
        bad_raster = raster; bad_raster.height = 0;
        rejects([&] { (void)encodeAssistanceOcrFrame(bad_raster); });

        AssistanceOcrResult result;
        result.text = "12 ft\nCaf\xc3\xa9\n";
        result.runs = {{0, 5, .1, .2, .5, .1, .93}, {6, 5, .1, .4, .5, .1, .82}};
        auto reply = encodeAssistanceOcrReply(result);
        const auto parsed = validateAssistanceOcrReply(reply);
        require(parsed.text == result.text && parsed.runs.size() == 2 &&
                parsed.runs[1].length == 5 && parsed.runs[0].confidence == .93 &&
                parsed.resources.size() == 2 && !parsed.isolation_controls_attested,
                "Reply must retain original UTF-8, actual confidence and fixed resources");
        auto json = nlohmann::json::parse(reinterpret_cast<const char*>(reply.data()),
                                         reinterpret_cast<const char*>(reply.data()) + reply.size());
        const auto mutated = [&](auto mutate) {
            auto candidate = json; mutate(candidate);
            rejects([&] { (void)validateAssistanceOcrReply(wire(candidate.dump())); });
        };
        mutated([](auto& j) { j["schema"] = "wrong"; });
        mutated([](auto& j) { j["version"] = 2; });
        mutated([](auto& j) { j["model_sha256"] = "wrong"; });
        mutated([](auto& j) { j["engine_version"] = "5.5.1"; });
        mutated([](auto& j) { j["extra"] = true; });
        mutated([](auto& j) { j["runs"][1]["offset"] = 10; j["runs"][1]["length"] = 1; });
        mutated([](auto& j) { j["runs"][1]["offset"] = 4; });
        mutated([](auto& j) { j["runs"][0]["confidence"] = 1.1; });
        mutated([](auto& j) { j["runs"][0]["confidence"] = nullptr; });
        mutated([](auto& j) { j["runs"][0]["x"] = -.1; });
        mutated([](auto& j) { j["runs"][0]["width"] = 1; });
        mutated([](auto& j) { j["runs"][0]["length"] = -1; });
        mutated([](auto& j) { j["runs"] = nlohmann::json::array(); });
        mutated([](auto& j) { j["text"] = std::string(16385, 'x'); });
        mutated([](auto& j) { j["runs"] = nlohmann::json::array(); j["text"] = "";
            for (int i = 0; i != 513; ++i) j["runs"].push_back({}); });
        rejects([&] { (void)validateAssistanceOcrReply(wire(json.dump() + " {}")); });
        rejects([&] { (void)validateAssistanceOcrReply(wire("{\"schema\":1,\"schema\":2}")); });
        auto raw_invalid_utf8 = json.dump(); const auto at = raw_invalid_utf8.find("12 ft");
        require(at != std::string::npos, "Fixture text must appear in serialized reply");
        raw_invalid_utf8[at] = char(0xff);
        rejects([&] { (void)validateAssistanceOcrReply(wire(raw_invalid_utf8)); });
        auto bad_result = result; bad_result.runs[0].confidence = std::numeric_limits<double>::infinity();
        rejects([&] { (void)encodeAssistanceOcrReply(bad_result); });
        AssistanceOcrResult empty;
        require(validateAssistanceOcrReply(encodeAssistanceOcrReply(empty)).text.empty(),
                "No text is a valid observed recognition result");

        int calls = 0; auto report = attested(reply);
        AssistanceOcrBroker broker = [&](const WindowsImportWorkerOptions& options) {
            ++calls;
            require(options.arguments == std::vector<std::wstring>{L"ocr", L"0"} &&
                options.input == frame && options.timeout_ms <= 30000 && options.memory_bytes == 512ULL*1024*1024 &&
                options.max_active_processes == 1 && options.max_output_bytes == assistanceOcrReplyLimit &&
                options.proj_offline_required, "OCR broker must pin invocation and all isolation bounds");
            return report;
        };
        require(recognizeAssistanceRaster(raster, {}, broker).isolation_controls_attested && calls == 1,
                "Only the attested broker can return accepted OCR");
        report.network_denial_verified = false;
        rejects([&] { (void)recognizeAssistanceRaster(raster, {}, broker); });
        report = attested(reply); report.status = WindowsImportWorkerStatus::timed_out;
        rejects([&] { (void)recognizeAssistanceRaster(raster, {}, broker); });
        report = attested(reply); report.status = WindowsImportWorkerStatus::cancelled;
        cancels([&] { (void)recognizeAssistanceRaster(raster, {}, broker); });
        auto flag = std::make_shared<std::atomic_bool>(true);
        WindowsImportWorkerOptions options; options.cancellation_requested = flag;
        const auto before = calls;
        cancels([&] { (void)recognizeAssistanceRaster(raster, options, broker); });
        require(calls == before, "Pre-cancelled OCR must not launch");
        flag->store(false);
        AssistanceOcrBroker race = [&](const WindowsImportWorkerOptions& o) {
            require(o.cancellation_requested == flag, "Cancellation handle must reach worker broker");
            flag->store(true); return attested(reply);
        };
        cancels([&] { (void)recognizeAssistanceRaster(raster, options, race); });
        QTemporaryDir directory;
        require(directory.isValid(), "Create private resource fixture directory");
        rejects([&] { (void)verifiedAssistanceOcrResources(directory.path().toStdWString()); });
        require(QDir(directory.path()).mkpath("assets/assistance/ocr"), "Create fixed model fixture paths");
        const auto save = [&](const char* relative, const QByteArray& contents) {
            QFile file(QDir(directory.path()).filePath(QString::fromLatin1(relative)));
            require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(),
                    "Write private bounded resource fixtures");
        };
        nlohmann::json descriptor{{"schema", "vertex.assistance.ocr-engine"}, {"version", 1},
            {"id", "vertex-ocr-engine-v1"}, {"engine", "tesseract"}, {"engine_version", "5.5.2"},
            {"model_id", "tessdata-fast-eng-v1"}, {"model_path", assistanceOcrModelPath},
            {"model_sha256", assistanceOcrModelSha256}, {"model_bytes", assistanceOcrModelBytes},
            {"model_revision", "65727574dfcd264acbb0c3e07860e4e9e9b22185"}, {"language", "eng"},
            {"license", "Apache-2.0"}, {"license_path", assistanceOcrLicensePath}};
        save(assistanceOcrEnginePath, QByteArray::fromStdString(descriptor.dump()));
        save(assistanceOcrLicensePath, "Apache License\nVersion 2.0\n");
        save(assistanceOcrModelPath, QByteArray(assistanceOcrModelBytes, '\0'));
        rejects([&] { (void)verifiedAssistanceOcrResources(directory.path().toStdWString()); });
        save(assistanceOcrModelPath, "tampered model");
        rejects([&] { (void)verifiedAssistanceOcrResources(directory.path().toStdWString()); });
        descriptor["engine_version"] = "5.5.1";
        save(assistanceOcrEnginePath, QByteArray::fromStdString(descriptor.dump()));
        rejects([&] { (void)verifiedAssistanceOcrResources(directory.path().toStdWString()); });
        if (argc == 2) {
            const auto installed = verifiedAssistanceOcrResources(std::filesystem::path(argv[1]));
            require(installed == parsed.resources, "Pinned installed resources must match reply provenance");
        }
        std::cout << "assistance_ocr_tests: PASS (bounded protocol and broker; real recognizer tested separately)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "assistance_ocr_tests: " << e.what() << '\n'; return 1;
    }
}
