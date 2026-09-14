#include "reference_import.hpp"
#include "support/noninteractive_errors.hpp"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QBuffer>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "unsafe worker response must be rejected");
}
sketch::WindowsImportWorkerReport successfulReply() {
    sketch::WindowsImportWorkerReport r;
    r.status = sketch::WindowsImportWorkerStatus::completed;
    r.completed = r.launched = r.app_container_verified = r.restricted_token_verified =
        r.network_denial_verified = r.job_limits_verified = r.parent_exit_kill_verified =
        r.brokered_handles_verified = r.private_temporary_root_verified =
        r.immutable_module_roots_verified = r.fixed_search_applied = r.proj_offline_applied = true;
    r.output.resize(28);
    std::memcpy(r.output.data(), "PSIR0001", 8);
    auto* p = reinterpret_cast<uchar*>(r.output.data());
    qToLittleEndian<quint32>(1, p + 8);
    qToLittleEndian<quint32>(1, p + 12);
    qToLittleEndian<quint32>(1, p + 16);
    qToLittleEndian<quint32>(0, p + 20);
    p[24] = 10; p[25] = 20; p[26] = 30; p[27] = 255;
    return r;
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QGuiApplication app(argc, argv);
    try {
        using namespace sketch::desktop;
        int calls = 0;
        auto response = successfulReply();
        const ReferenceBroker broker = [&](const sketch::WindowsImportWorkerOptions& options) {
            ++calls;
            require(options.input.size() == 6 && options.arguments == std::vector<std::wstring>{L"png", L"0"},
                    "caller must broker source bytes and fixed arguments");
            require(options.timeout_ms == 30000 && options.max_active_processes == 1 &&
                    options.proj_offline_required && options.max_output_bytes == 67108888,
                    "caller must enforce bounded offline worker policy");
            return response;
        };
        const auto decoded = decodeReferenceBytes("source", "png", 0, {}, broker);
        require(calls == 1 && decoded.image.pixelColor(0, 0) == QColor(10, 20, 30) &&
                decoded.page_count == 1, "caller must accept only validated worker pixels");
        response.network_denial_verified = false;
        rejects([&] { (void)decodeReferenceBytes("source", "png", 0, {}, broker); });
        response = successfulReply();
        response.output.pop_back();
        rejects([&] { (void)decodeReferenceBytes("source", "png", 0, {}, broker); });
        response = successfulReply();
        qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar*>(response.output.data()) + 8);
        rejects([&] { (void)decodeReferenceBytes("source", "png", 0, {}, broker); });
        response = successfulReply();
        qToLittleEndian<quint32>(2, reinterpret_cast<uchar*>(response.output.data()) + 16);
        rejects([&] { (void)decodeReferenceBytes("source", "png", 0, {}, broker); });
        response = successfulReply();
        response.status = sketch::WindowsImportWorkerStatus::timed_out;
        rejects([&] { (void)decodeReferenceBytes("source", "png", 0, {}, broker); });
        const auto before = calls;
        rejects([&] { (void)decodeReferenceBytes("source", "svg", 0, {}, broker); });
        rejects([&] { (void)decodeReferenceBytes("source", "tiff", 0, {}, broker); });
        rejects([&] { (void)decodeReferenceBytes({}, "png", 0, {}, broker); });
        rejects([&] { (void)decodeReferenceBytes("source", "png", -1, {}, broker); });
        require(calls == before, "invalid requests must be rejected before launch");
        // Exercise the actual broker rejection, not just the injected response.
        sketch::WindowsImportWorkerOptions missing;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary fixture directory must exist");
        missing.executable = std::filesystem::path(temporary.path().toStdWString()) / "missing.exe";
        missing.immutable_module_roots = {missing.executable.parent_path()};
        missing.temporary_root = std::filesystem::temp_directory_path();
        bool actionable = false;
        try { (void)decodeReferenceBytes("source", "png", 0, missing, sketch::run_windows_import_worker); }
        catch (const std::exception& error) {
            actionable = std::string(error.what()).find("Install or repair") != std::string::npos;
        }
        require(actionable, "missing bundled worker must fail closed with repair guidance");
        // Codec/protocol checks use generated fixtures in a direct child. These
        // do not claim AppContainer execution or production sandbox acceptance.
        require(argc == 2, "worker codec tests require the built worker path");
        const auto run_codec = [&](const QByteArray& input, const QString& format, int page, bool success) {
            QProcess process;
            process.start(QString::fromLocal8Bit(argv[1]), {format, QString::number(page)});
            require(process.waitForStarted(5000), "codec fixture worker must start");
            require(process.write(input) == input.size(), "codec fixture input must be queued");
            process.closeWriteChannel();
            if (!process.waitForFinished(10000)) {
                process.kill();
                process.waitForFinished(5000);
                throw std::runtime_error("codec fixture worker exceeded its deadline");
            }
            const auto bytes = process.readAllStandardOutput();
            if (!success) {
                require(process.exitCode() != 0 && bytes.isEmpty(), "malformed codec input must produce no preview");
                return DecodedReference{};
            }
            require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                    "generated codec fixture must decode");
            auto reply = successfulReply();
            reply.output.resize(static_cast<std::size_t>(bytes.size()));
            std::memcpy(reply.output.data(), bytes.constData(), reply.output.size());
            return decodeReferenceBytes(input, format, page, {}, [&](const auto&) { return reply; });
        };
        QImage raster(2, 3, QImage::Format_RGBA8888);
        raster.fill(QColor(10, 20, 30));
        QByteArray png;
        QBuffer png_buffer(&png);
        require(png_buffer.open(QIODevice::WriteOnly) && raster.save(&png_buffer, "PNG"), "generate PNG fixture");
        png_buffer.close();
        const auto raster_result = run_codec(png, "png", 0, true);
        require(raster_result.image.size() == raster.size() &&
                raster_result.image.pixelColor(1, 2) == QColor(10, 20, 30), "worker must preserve raster pixels");
        (void)run_codec("not an image", "png", 0, false);
        (void)run_codec(png, "png", 1, false);
        QByteArray pdf;
        QBuffer pdf_buffer(&pdf);
        require(pdf_buffer.open(QIODevice::WriteOnly), "generate PDF fixture buffer");
        {
            QPdfWriter writer(&pdf_buffer);
            QPainter painter(&writer);
            painter.fillRect(QRect(0, 0, 100, 100), Qt::red);
            require(writer.newPage(), "PDF fixture second page");
            painter.fillRect(QRect(0, 0, 100, 100), Qt::blue);
        }
        pdf_buffer.close();
        const auto pdf_result = run_codec(pdf, "pdf", 1, true);
        require(pdf_result.page_count == 2 && !pdf_result.image.isNull(), "worker must render selected PDF page");
        (void)run_codec(pdf, "pdf", 2, false);
        std::cout << "reference import broker boundary passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
