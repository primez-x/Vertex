#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        using namespace sketch;
        using namespace sketch::desktop;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        MainWindow source;
        const auto boundary_id = source.createBoundary({
            {{0.0, 0.0}, {4.0, 0.0}, 0.0},
            {{4.0, 0.0}, {4.0, 3.0}, 0.0},
            {{4.0, 3.0}, {0.0, 3.0}, 0.0},
            {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
        require(!boundary_id.isEmpty(), "source boundary must be created");
        const auto path = temporary.filePath(QStringLiteral("mapped.dxf"));
        require(source.exportDxf(path), "native project must export DXF");
        require(QFileInfo::exists(path) && QFileInfo(path).size() > 0,
                "DXF export must write bytes");
        require(QFileInfo::exists(path + QStringLiteral(".fidelity.json")),
                "DXF export must write a fidelity report");

        MainWindow destination;
        const auto before = destination.document().revision();
        require(destination.importDxf(path), "native project must import DXF");
        require(destination.document().revision() == before + 1,
                "DXF import must be one document revision");
        const auto snapshot = destination.document().snapshot();
        bool imported_boundary = false;
        bool retained_source = false;
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "boundary" && entity.properties.value("classification", "") ==
                    "dxf_polyline_closed") imported_boundary = true;
            if (entity.type == "dxf_source") {
                retained_source = true;
                const auto asset_id = entity.properties.value("asset_id", "");
                require(snapshot.assets().contains(asset_id), "DXF source asset must be retained");
            }
        }
        require(imported_boundary, "DXF import must create a mapped boundary");
        require(retained_source, "DXF import must retain source provenance");
        require(destination.undoCommand(), "DXF import must be undoable");
        std::cout << "DXF desktop workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
