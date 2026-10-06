#include "pinc_import_worker.hpp"
#include "support/noninteractive_errors.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QProcess>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Malformed Pinc worker response must be refused");
}
template<class Function> void cancels(Function function) {
    bool cancelled = false;
    try { function(); } catch (const std::runtime_error& error) {
        cancelled = std::string_view(error.what()) == "Pinc import cancelled.";
    }
    require(cancelled, "Cancelled Pinc import must return the stable cancellation outcome");
}
sketch::WindowsImportWorkerReport attested(std::vector<std::byte> output) {
    sketch::WindowsImportWorkerReport result;
    result.status = sketch::WindowsImportWorkerStatus::completed;
    result.completed = result.launched = result.app_container_verified = result.restricted_token_verified =
        result.network_denial_verified = result.job_limits_verified = result.parent_exit_kill_verified =
        result.job_membership_verified =
        result.brokered_handles_verified = result.private_temporary_root_verified =
        result.immutable_module_roots_verified = result.fixed_search_applied = result.proj_offline_applied = true;
    result.output = std::move(output); return result;
}
std::span<const std::byte> bytes(const QByteArray& input) {
    return {reinterpret_cast<const std::byte*>(input.constData()), static_cast<std::size_t>(input.size())};
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QCoreApplication app(argc, argv);
    try {
        using namespace sketch::desktop;
        require(argc == 2, "Require the built Pinc import worker path");
        const auto run = [&](const QByteArray& source, bool success = true) {
            QProcess process;
#ifdef _WIN32
            process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
                args->flags |= CREATE_NO_WINDOW;
            });
#endif
            process.start(QString::fromLocal8Bit(argv[1]), {"pinc", "0"});
            require(process.waitForStarted(5000), "Pinc worker must start");
            require(process.write(source) == source.size(), "Pinc source must be queued");
            process.closeWriteChannel();
            if (!process.waitForFinished(30000)) {
                process.kill(); process.waitForFinished(5000);
                throw std::runtime_error("Task-owned Pinc codec child exceeded its deadline");
            }
            const auto output = process.readAllStandardOutput();
            require(process.exitStatus() == QProcess::NormalExit, "Worker must exit normally");
            require(success ? process.exitCode() == 0 : process.exitCode() != 0,
                    "Pinc worker outcome must match the supplied fixture");
            if (!success) require(output.isEmpty(), "Failed import cannot publish partial candidate bytes");
            return std::vector<std::byte>(bytes(output).begin(), bytes(output).end());
        };
        nlohmann::json page{{"id","page"},{"name","Plan"},
            {"calcWalls", {{{"id","wall"},{"kind","line"},{"a",{{"x",0},{"y",0}}},
                            {"b",{{"x",10},{"y",0}}}}}}};
        nlohmann::json project{{"format","PincSketch"},{"version","4.2"},
            {"currentPage",0},{"pages",nlohmann::json::array({page})}};
        const auto plain = QByteArray::fromStdString(project.dump());
        auto wire = run(plain);
        auto decoded = validatePincWorkerReply(wire);
        require(decoded.project.pages.size() == 1 && decoded.underlays.empty() &&
                !decoded.isolation_controls_attested,
                "Pure transport cannot assert sandbox attestation");
        int calls = 0;
        auto reply = attested(wire);
        const ReferenceBroker broker = [&](const sketch::WindowsImportWorkerOptions& options) {
            ++calls;
            require(options.arguments == std::vector<std::wstring>{L"pinc",L"0"} && options.input.size() == plain.size() &&
                options.timeout_ms == 30000 && options.memory_bytes == 768ULL * 1024 * 1024 &&
                options.max_active_processes == 1 && options.max_output_bytes == pincReplyLimit && options.proj_offline_required,
                "Pinc broker must pin arguments, resources and offline policy");
            return reply;
        };
        require(importPincProjectBytes(bytes(plain),{},broker).isolation_controls_attested && calls == 1,
                "Only the attested outer broker can accept a candidate");
        reply.network_denial_verified = false;
        rejects([&] { (void)importPincProjectBytes(bytes(plain),{},broker); });
        reply = attested(wire); reply.status = sketch::WindowsImportWorkerStatus::timed_out;
        rejects([&] { (void)importPincProjectBytes(bytes(plain),{},broker); });
        reply = attested(wire); reply.status = sketch::WindowsImportWorkerStatus::cancelled;
        cancels([&] { (void)importPincProjectBytes(bytes(plain),{},broker); });
        sketch::WindowsImportWorkerOptions cancellable;
        auto cancellation = std::make_shared<std::atomic_bool>(true);
        cancellable.cancellation_requested = cancellation;
        const auto before_cancel = calls;
        cancels([&] { (void)importPincProjectBytes(bytes(plain),cancellable,broker); });
        require(calls == before_cancel, "Pre-cancelled Pinc import must not invoke the broker");
        cancellation->store(false);
        const ReferenceBroker completion_race = [&](const sketch::WindowsImportWorkerOptions& options) {
            require(options.cancellation_requested == cancellation, "Broker must receive the caller's cancellation flag");
            cancellation->store(true);
            return attested(wire);
        };
        cancels([&] { (void)importPincProjectBytes(bytes(plain),cancellable,completion_race); });
        const auto before = calls;
        rejects([&] { (void)importPincProjectBytes({}, {}, broker); });
        require(calls == before, "Empty input must be refused before worker launch");
        auto invalid = wire; invalid[0] = std::byte{'X'};
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = wire; invalid.pop_back();
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = wire; invalid.push_back(std::byte{});
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = wire;
        qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar*>(invalid.data()) + 8);
        rejects([&] { (void)validatePincWorkerReply(invalid); });

        QImage image(2,1,QImage::Format_RGBA8888);
        image.setPixelColor(0,0,QColor(200,20,30)); image.setPixelColor(1,0,QColor(10,40,230));
        QByteArray png; QBuffer encoded(&png);
        require(encoded.open(QIODevice::WriteOnly) && image.save(&encoded,"PNG"), "Encode a synthetic reference fixture");
        project["pages"][0]["underlay"] = {{"data","data:image/png;base64," + png.toBase64().toStdString()},
            {"x",2},{"y",3},{"width",40},{"opacity",.4}};
        const auto illustrated = QByteArray::fromStdString(project.dump());
        auto illustrated_wire = run(illustrated);
        auto illustrated_result = validatePincWorkerReply(illustrated_wire);
        require(illustrated_result.underlays.size() == 1 && illustrated_result.underlays[0].page_index == 0 &&
                illustrated_result.underlays[0].reference.source == png &&
                illustrated_result.underlays[0].reference.image.pixelColor(0,0) == QColor(200,20,30) &&
                illustrated_result.underlays[0].reference.image.pixelColor(1,0) == QColor(10,40,230),
                "Actual sandbox-capable worker must transport original PNG bytes and decoded raw pixels");
        const auto candidate_size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(illustrated_wire.data()) + 8);
        const auto frame_offset = std::size_t{16} + candidate_size;
        const auto source_size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(illustrated_wire.data()) + frame_offset + 4);
        const auto pixels_offset = frame_offset + 12 + source_size;
        invalid = illustrated_wire;
        qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar*>(invalid.data()) + pixels_offset + 8);
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = illustrated_wire; invalid[frame_offset + 12] ^= std::byte{1};
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = illustrated_wire;
        qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(invalid.data()) + frame_offset);
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        invalid = illustrated_wire;
        qToLittleEndian<quint32>(0, reinterpret_cast<uchar*>(invalid.data()) + 12);
        invalid.resize(16 + candidate_size);
        rejects([&] { (void)validatePincWorkerReply(invalid); });
        std::fill(illustrated_wire.begin(), illustrated_wire.end(), std::byte{});
        require(illustrated_result.underlays[0].reference.source == png &&
                illustrated_result.underlays[0].reference.image.pixelColor(1,0) == QColor(10,40,230),
                "Retained images and source bytes must outlive the transport buffer");
        auto failed_project = project;
        failed_project["pages"][0]["underlay"]["data"] = "data:image/png;base64,YmFk";
        const auto failed_result = validatePincWorkerReply(run(QByteArray::fromStdString(failed_project.dump())));
        require(failed_result.underlays.empty() && std::any_of(failed_result.project.diagnostics.begin(),
            failed_result.project.diagnostics.end(), [](const auto& item) { return item.code == "underlay_decode_failed"; }),
            "Unreadable raster must remain explicitly unresolved, without a desktop image decoder");
        failed_project = project;
        failed_project["pages"][0]["underlay"]["data"] = "data:image/tiff;base64," + png.toBase64().toStdString();
        const auto disguised = validatePincWorkerReply(run(QByteArray::fromStdString(failed_project.dump())));
        require(disguised.underlays.empty() && std::any_of(disguised.project.diagnostics.begin(),
            disguised.project.diagnostics.end(), [](const auto& item) { return item.code == "underlay_decode_failed"; }),
            "TIFF descriptor must not auto-detect and decode another WIC container format");
        (void)run("{\"format\":\"PincSketch\",\"format\":\"PincSketch\"}", false);
        failed_project = project; failed_project["version"] = "future";
        (void)run(QByteArray::fromStdString(failed_project.dump()), false);
        std::cout << "pinc_import_worker_tests: PASS (codec/pixel transport; no independent sandbox qualification)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pinc_import_worker_tests: " << error.what() << '\n'; return 1;
    }
}
