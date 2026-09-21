#include "reference_import.hpp"
#include "sketch/project_import_worker.hpp"
#include "sketch/ifc_project_exchange.hpp"
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
#include <algorithm>
#include <cstring>
#include <bit>
#include <limits>
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
    r.output.resize(36);
    std::memcpy(r.output.data(), "PSIR0002", 8);
    auto* p = reinterpret_cast<uchar*>(r.output.data());
    qToLittleEndian<quint32>(1, p + 8);
    qToLittleEndian<quint32>(1, p + 12);
    qToLittleEndian<quint32>(1, p + 16);
    qToLittleEndian<quint32>(0, p + 20);
    p[32] = 10; p[33] = 20; p[34] = 30; p[35] = 255;
    return r;
}
sketch::WindowsImportWorkerReport textReply() {
    auto reply = successfulReply();
    reply.output.resize(36 + 5 + 40);
    auto* p = reinterpret_cast<uchar*>(reply.output.data());
    qToLittleEndian<quint32>(5, p + 24);
    qToLittleEndian<quint32>(1, p + 28);
    std::memcpy(p + 36, "12 ft", 5);
    qToLittleEndian<quint32>(0, p + 41);
    qToLittleEndian<quint32>(5, p + 45);
    const double values[]{0.2, 0.3, 0.4, 0.05};
    for (int i = 0; i < 4; ++i)
        qToLittleEndian<quint64>(std::bit_cast<quint64>(values[i]), p + 49 + i * 8);
    return reply;
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
                    options.proj_offline_required && options.max_output_bytes ==
                        referenceHeaderSize + 4ULL * referenceDimensionLimit * referenceDimensionLimit +
                            referenceTextLimit + referenceTextRunLimit * referenceTextRunSize,
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
        rejects([&] { (void)decodeReferenceBytes({}, "png", 0, {}, broker); });
        rejects([&] { (void)decodeReferenceBytes("source", "png", -1, {}, broker); });
        require(calls == before, "invalid requests must be rejected before launch");
        auto text_response = textReply();
        const ReferenceBroker text_broker = [&](const auto&) { return text_response; };
        const auto text_decoded = decodeReferenceBytes("pdf", "pdf", 0, {}, text_broker);
        require(text_decoded.source_text == "12 ft" && text_decoded.text_runs.size() == 1 &&
                    text_decoded.text_runs.front().bounds == QRectF(0.2, 0.3, 0.4, 0.05),
                "broker must preserve exact validated selection metadata");
        rejects([&] { (void)decodeReferenceBytes("png", "png", 0, {}, text_broker); });
        const auto reject_text = [&] {
            rejects([&] { (void)decodeReferenceBytes("pdf", "pdf", 0, {}, text_broker); });
        };
        for (const auto [offset, value] : std::vector<std::pair<int, quint32>>{
                {24, referenceTextLimit + 1}, {28, referenceTextRunLimit + 1},
                {41, 0xffffffff}, {45, 6}, {45, 0}}) {
            text_response = textReply();
            qToLittleEndian<quint32>(value, reinterpret_cast<uchar*>(text_response.output.data()) + offset);
            reject_text();
        }
        for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity(), -0.1, 1.1}) {
            text_response = textReply();
            qToLittleEndian<quint64>(std::bit_cast<quint64>(value),
                reinterpret_cast<uchar*>(text_response.output.data()) + 49);
            reject_text();
        }
        for (const auto value : {0, 0xff, 0xc2}) {
            text_response = textReply();
            text_response.output[40] = static_cast<std::byte>(value);
            reject_text();
        }
        text_response = textReply();
        text_response.output[36] = std::byte{0xc3};
        text_response.output[37] = std::byte{0xa9};
        qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(text_response.output.data()) + 41);
        qToLittleEndian<quint32>(4, reinterpret_cast<uchar*>(text_response.output.data()) + 45);
        reject_text(); // A valid UTF-8 string, but a selection splits its first character.
        text_response = textReply();
        const auto first_record = std::vector<std::byte>(text_response.output.begin() + 41,
                                                        text_response.output.end());
        text_response.output.insert(text_response.output.end(), first_record.begin(), first_record.end());
        qToLittleEndian<quint32>(2, reinterpret_cast<uchar*>(text_response.output.data()) + 28);
        reject_text(); // Duplicate/overlapping ranges are not accepted.
        text_response = textReply();
        text_response.output.push_back(std::byte{});
        reject_text();
        text_response = textReply();
        text_response.output[7] = std::byte{'1'};
        reject_text();
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
        const auto run_project_codec = [&](const QByteArray& input, sketch::ProjectImportKind kind,
                                           bool success) {
            QProcess process;
            process.start(QString::fromLocal8Bit(argv[1]),
                {QString::fromLatin1(sketch::project_import_kind_name(kind)), "0"});
            require(process.waitForStarted(5000), "project codec worker must start");
            require(process.write(input) == input.size(), "project input must be queued");
            process.closeWriteChannel();
            if (!process.waitForFinished(10000)) {
                process.kill(); process.waitForFinished(5000);
                throw std::runtime_error("project codec worker exceeded deadline");
            }
            const auto bytes = process.readAllStandardOutput();
            if (!success) {
                require(process.exitCode() != 0 && bytes.isEmpty(), "malformed project must publish no candidate");
                return sketch::ProjectImportCandidate{};
            }
            require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                    "project worker must decode supported fixture");
            auto reply = successfulReply();
            reply.output.resize(static_cast<std::size_t>(bytes.size()));
            std::memcpy(reply.output.data(), bytes.constData(), reply.output.size());
            const auto source = std::span(reinterpret_cast<const std::byte*>(input.constData()),
                                          static_cast<std::size_t>(input.size()));
            int project_calls = 0;
            const auto broker = [&](const sketch::WindowsImportWorkerOptions& options) {
                ++project_calls;
                require(options.arguments == std::vector<std::wstring>{
                    kind == sketch::ProjectImportKind::dxf ? L"dxf" : L"ifc", L"0"} &&
                    options.input.size() == source.size() && options.max_output_bytes == sketch::project_import_output_limit &&
                    options.timeout_ms == 30000 && options.max_active_processes == 1 && options.proj_offline_required,
                    "project request must have fixed kind and bounded broker policy");
                return reply;
            };
            const auto result = sketch::import_project_in_worker(source, kind, {}, broker);
            require(project_calls == 1 && result.kind == kind && result.isolation_controls_attested,
                    "project response must retain matching kind and broker attestation");
            const auto good = reply;
            for (const auto status : {sketch::WindowsImportWorkerStatus::timed_out,
                                      sketch::WindowsImportWorkerStatus::failed,
                                      sketch::WindowsImportWorkerStatus::launch_failed}) {
                reply = good; reply.status = status;
                rejects([&] { (void)sketch::import_project_in_worker(source, kind, {}, broker); });
            }
            reply = good; reply.network_denial_verified = false;
            rejects([&] { (void)sketch::import_project_in_worker(source, kind, {}, broker); });
            reply = good; reply.output.pop_back();
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            reply = good;
            rejects([&] { (void)sketch::decode_project_import_candidate(reply,
                kind == sketch::ProjectImportKind::dxf ? sketch::ProjectImportKind::ifc : sketch::ProjectImportKind::dxf); });
            auto json = nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
            require(!json.contains("isolation_controls_attested"), "worker cannot assert broker attestation");
            const auto set_wire = [&](const auto& value) {
                const auto wire = value.dump();
                reply = good; reply.output.resize(wire.size());
                std::memcpy(reply.output.data(), wire.data(), wire.size());
            };
            auto invalid = json; invalid["entities"][0]["type"] = "reference_asset";
            set_wire(invalid);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            if (kind == sketch::ProjectImportKind::dxf) {
                invalid = json;
                invalid["entities"][0]["properties"]["boundary"] = nlohmann::json::array();
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                auto& segment = invalid["entities"][0]["properties"]["boundary"][0];
                segment["end"] = segment["start"];
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                const auto repeated = invalid["entities"][0]["properties"]["boundary"][0];
                invalid["entities"][0]["properties"]["boundary"] = nlohmann::json::array();
                for (int index = 0; index < 2048; ++index)
                    invalid["entities"][0]["properties"]["boundary"].push_back(repeated);
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            } else {
                invalid = json;
                invalid["entities"][0]["properties"].erase("baseline");
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                invalid["entities"][0]["properties"]["thickness_m"] = -0.2;
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                invalid["entities"][0]["properties"]["slope_rise_m"] = -5.0;
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                invalid["entities"].push_back({
                    {"id", "unhosted-opening"}, {"type", "opening"},
                    {"properties", {{"wall_id", "missing-wall"}, {"offset_m", 0.2},
                        {"width_m", 0.9}, {"sill_m", 0.0}, {"height_m", 2.0}}},
                    {"extensions", nlohmann::json::object()}});
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
                invalid = json;
                invalid["entities"].push_back({
                    {"id", "invalid-slab"}, {"type", "slab"},
                    {"properties", {{"boundary", nlohmann::json::array()},
                        {"holes", nlohmann::json::array()}, {"thickness_m", -0.1},
                        {"elevation_m", 0.0}}}, {"extensions", nlohmann::json::object()}});
                set_wire(invalid);
                rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            }
            const auto duplicated = std::string("{\"kind\":\"dxf\",") + json.dump().substr(1);
            reply = good; reply.output.resize(duplicated.size());
            std::memcpy(reply.output.data(), duplicated.data(), duplicated.size());
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            invalid = json; invalid["entities"].push_back(invalid["entities"][0]);
            set_wire(invalid);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            invalid = json; invalid["entities"][0]["properties"]["parent_id"] = "foreign-floor";
            set_wire(invalid);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            invalid = json; invalid["diagnostics"] = {{{"source_id", ""}, {"source_kind", ""}, {"code", "loss"}}};
            invalid["source_retention_required"] = false;
            set_wire(invalid);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            invalid = json;
            nlohmann::json deep = nlohmann::json::object();
            for (int depth = 0; depth < 40; ++depth) deep = {{"nested", std::move(deep)}};
            invalid["entities"][0]["extensions"] = std::move(deep);
            set_wire(invalid);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            reply = good; reply.output.resize(sketch::project_import_output_limit + 1);
            rejects([&] { (void)sketch::decode_project_import_candidate(reply, kind); });
            const auto calls_before = project_calls;
            std::vector<std::byte> oversized(sketch::project_import_input_limit + 1);
            rejects([&] { (void)sketch::import_project_in_worker(oversized, kind, {}, broker); });
            rejects([&] { (void)sketch::import_project_in_worker({}, kind, {}, broker); });
            require(project_calls == calls_before, "invalid source must be rejected before broker launch");
            return result;
        };
        const QByteArray dxf("0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n9\n$INSUNITS\n70\n6\n0\nENDSEC\n"
            "0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n0\n20\n0\n11\n4\n21\n0\n0\nENDSEC\n0\nEOF\n");
        const auto dxf_result = run_project_codec(dxf, sketch::ProjectImportKind::dxf, true);
        require(dxf_result.entities.size() == 1 && dxf_result.entities[0].type == "boundary",
                "DXF line must cross worker as an editable boundary");
        sketch::Entity wall{"wall-1", "wall",
            {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.0}}},
             {"thickness_m", 0.2}, {"height_m", 2.5}, {"elevation_m", 0.0}}, false, nlohmann::json::object()};
        const auto ifc = sketch::export_project_ifc(sketch::Document::create({wall}).snapshot()).step;
        const auto ifc_result = run_project_codec(QByteArray::fromStdString(ifc), sketch::ProjectImportKind::ifc, true);
        require(ifc_result.entities.size() == 1 && ifc_result.entities[0].type == "wall",
                "IFC wall must cross worker as editable semantic geometry");
        sketch::Entity retained{"ifc-42", "ifc_reference",
            {{"ifc_name", "Unsupported native object"}, {"ifc_type", "IFCBUILDINGELEMENTPROXY"}}, false,
            {{"ifc_source", {{"record_id", 42}, {"record_type", "IFCBUILDINGELEMENTPROXY"},
                {"arguments", "'retained'"}}}, {"ifc_vertex_properties", {{"native_entity", {{"id", "roof-1"}}}}}}};
        sketch::ProjectImportCandidate retained_candidate;
        retained_candidate.kind = sketch::ProjectImportKind::ifc;
        retained_candidate.entities = {retained};
        retained_candidate.source_retention_required = true;
        auto retained_report = successfulReply();
        retained_report.output = sketch::encode_project_import_candidate(retained_candidate);
        const auto retained_result = sketch::decode_project_import_candidate(
            retained_report, sketch::ProjectImportKind::ifc);
        require(retained_result.entities == std::vector<sketch::Entity>{retained} &&
                retained_result.source_retention_required && retained_result.isolation_controls_attested,
                "unsupported IFC references must cross the isolated candidate boundary without semantic loss");
        (void)run_project_codec("invalid", sketch::ProjectImportKind::dxf, false);
        (void)run_project_codec("invalid", sketch::ProjectImportKind::ifc, false);
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
            require(bytes.startsWith("PSIR0002"),
                    "worker must return versioned text-and-selection metadata");
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
        require(raster_result.source_text.isEmpty() && raster_result.text_runs.empty(),
                "raster-only import must not fabricate OCR text");
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
        require(pdf_result.source_text.isEmpty() && pdf_result.text_runs.empty(),
                "PDF pages without embedded text must not fabricate dimensions");
        const auto make_text_pdf = [](int rotation) {
            QByteArray result("%PDF-1.4\n");
            std::vector<int> offsets{0};
            const QByteArray content(
                "BT /F1 12 Tf 72 650 Td (Wall: 12 ft) Tj 100 -200 Td (Depth: 900 mm) Tj ET\n");
            const auto rotation_entry = rotation ? " /Rotate " + QByteArray::number(rotation) : QByteArray{};
            const std::vector<QByteArray> objects{
                "<< /Type /Catalog /Pages 2 0 R >>",
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]" + rotation_entry +
                    " /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
                "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream"};
            for (const auto& object : objects) {
                offsets.push_back(static_cast<int>(result.size()));
                result += QByteArray::number(offsets.size() - 1) + " 0 obj\n" + object + "\nendobj\n";
            }
            const auto xref = result.size();
            result += "xref\n0 6\n0000000000 65535 f \n";
            for (std::size_t i = 1; i < offsets.size(); ++i)
                result += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
            result += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
                QByteArray::number(xref) + "\n%%EOF\n";
            return result;
        };
        const auto text_pdf = make_text_pdf(0);
        const auto text_result = run_codec(text_pdf, "pdf", 0, true);
        require(text_result.source_text.contains("12 ft") && text_result.source_text.contains("900 mm") &&
                    text_result.text_runs.size() == 2,
                "worker must extract embedded PDF text with a real selection");
        const auto box = text_result.text_runs.front().bounds;
        require(box.x() > 0.05 && box.x() < 0.2 && box.y() > 0.1 && box.y() < 0.3 &&
                    box.width() > 0 && box.height() > 0 && box.height() < 0.1,
                "PDF selection must follow the text placement on the page");
        const auto second_box = text_result.text_runs.back().bounds;
        require(second_box.x() > box.x() + 0.1 && second_box.y() > box.y() + 0.2 &&
                    text_result.text_runs.back().offset > text_result.text_runs.front().offset,
                "separate PDF text lines must retain their distinct page locations");
        const auto repeat = run_codec(text_pdf, "pdf", 0, true);
        require(repeat.source_text == text_result.source_text && repeat.text_runs.size() == 2 &&
                    repeat.text_runs.front().bounds == box && repeat.text_runs.back().bounds == second_box,
                "PDF extraction must produce deterministic text selection bounds");
        for (const auto rotation : {90, 180, 270}) {
            const auto rotated = run_codec(make_text_pdf(rotation), "pdf", 0, true);
            require(rotated.source_text == text_result.source_text && rotated.text_runs.size() == 2 &&
                        std::all_of(rotated.text_runs.begin(), rotated.text_runs.end(), [](const auto& run) {
                            return run.bounds.x() >= 0 && run.bounds.y() >= 0 && run.bounds.width() > 0 &&
                                run.bounds.height() > 0 && run.bounds.right() <= 1 && run.bounds.bottom() <= 1;
                        }), "rotated PDF text must retain valid page-relative selection bounds");
        }
        (void)run_codec(pdf, "pdf", 2, false);
        // Minimal little-endian, uncompressed RGB TIFF fixture. The worker
        // decodes it through the Windows Imaging Component path rather than
        // relying on a Qt TIFF plugin being installed on the target machine.
        const auto append_u16 = [](QByteArray& bytes, quint16 value) {
            bytes.append(static_cast<char>(value & 0xff));
            bytes.append(static_cast<char>((value >> 8) & 0xff));
        };
        const auto append_u32 = [](QByteArray& bytes, quint32 value) {
            bytes.append(static_cast<char>(value & 0xff));
            bytes.append(static_cast<char>((value >> 8) & 0xff));
            bytes.append(static_cast<char>((value >> 16) & 0xff));
            bytes.append(static_cast<char>((value >> 24) & 0xff));
        };
        QByteArray tiff;
        tiff.append("II", 2);
        append_u16(tiff, 42);
        append_u32(tiff, 8);
        append_u16(tiff, 10);
        const auto append_short_entry = [&](quint16 tag, quint16 value) {
            append_u16(tiff, tag); append_u16(tiff, 3); append_u32(tiff, 1);
            append_u16(tiff, value); append_u16(tiff, 0);
        };
        const auto append_long_entry = [&](quint16 tag, quint32 value) {
            append_u16(tiff, tag); append_u16(tiff, 4); append_u32(tiff, 1);
            append_u32(tiff, value);
        };
        append_short_entry(256, 2); // ImageWidth
        append_short_entry(257, 2); // ImageLength
        append_u16(tiff, 258); append_u16(tiff, 3); append_u32(tiff, 3);
        append_u32(tiff, 134); // BitsPerSample values
        append_short_entry(259, 1); // Compression = none
        append_short_entry(262, 2); // PhotometricInterpretation = RGB
        append_long_entry(273, 140); // StripOffsets
        append_short_entry(277, 3); // SamplesPerPixel
        append_long_entry(278, 2); // RowsPerStrip
        append_long_entry(279, 12); // StripByteCounts
        append_short_entry(284, 1); // PlanarConfiguration = chunky
        append_u32(tiff, 0); // no next IFD
        append_u16(tiff, 8); append_u16(tiff, 8); append_u16(tiff, 8);
        tiff.append(char(255)); tiff.append(char(0)); tiff.append(char(0));
        tiff.append(char(0)); tiff.append(char(255)); tiff.append(char(0));
        tiff.append(char(0)); tiff.append(char(0)); tiff.append(char(255));
        tiff.append(char(255)); tiff.append(char(255)); tiff.append(char(255));
        const auto tiff_result = run_codec(tiff, "tiff", 0, true);
        require(tiff_result.image.size() == QSize(2, 2) &&
                    tiff_result.image.pixelColor(0, 0) == QColor(255, 0, 0) &&
                    tiff_result.image.pixelColor(1, 1) == QColor(255, 255, 255),
                "worker must decode TIFF pixels through the native Windows codec");
        (void)run_codec(tiff, "tiff", 1, false);
        std::cout << "reference import broker boundary passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
