#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

void dxfOpeningProfileDesktopRoundtrip(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    MainWindow source;
    source.setMetricUnits(true);
    const auto wall = source.createCurvedWall({0, 0}, {8, 0}, "0.5 rad");
    require(!wall.isEmpty() && source.selectEntity(wall),
            "profile roundtrip curved host must be created");
    require(source.editSelectedThickness("0.3 m"), "profile host thickness must be configured");
    const auto door = source.createHostedOpening("door", "1 m", "0.9 m", "0 m", "2 m",
        std::nullopt, DoorOperation{true, false, 67.0});
    require(!door.isEmpty() && source.selectEntity(door) &&
            source.editSelectedOpeningAssembly("0.08 m", "0.25 m", "0.035 m", "0.012 m", "-0.015 m"),
            "profile roundtrip door assembly must be configured");
    const auto window_wall = source.createStraightWall({0, 5}, {12, 5});
    require(!window_wall.isEmpty() && source.selectEntity(window_wall) &&
            source.editSelectedThickness("0.3 m"), "second profile host must be created");
    const auto window = source.createHostedOpening("window", "9 m", "1 m", "0.8 m", "1.2 m");
    require(!window.isEmpty() && source.selectEntity(window) &&
            source.editSelectedOpeningAssembly("0.07 m", "0.24 m", "0.03 m", "0.014 m", "0.01 m"),
            "profile roundtrip window assembly must be configured");
    const auto original = source.document().snapshot();
    const auto path = directory + "/opening-profiles.dxf";
    require(source.exportDxf(path), "profile fixture must export");

    MainWindow destination;
    destination.setMetricUnits(true);
    const auto unrelated_wall = destination.createStraightWall({0, 10}, {3, 10});
    require(!unrelated_wall.isEmpty() && destination.selectEntity(unrelated_wall) &&
            !destination.createHostedOpening("door", "1 m", "0.8 m", "0 m", "2 m").isEmpty(),
            "unrelated target wall and opening must be created");
    const auto floor = destination.createFloor("building-1", "Imported profiles");
    const auto layer = destination.createLayer(floor, "Imported openings");
    require(!floor.isEmpty() && !layer.isEmpty() && destination.setActiveLayer(layer),
            "profile import destination floor and layer must be configured");
    const auto initial = destination.document().snapshot();
    const auto inspect = [&](const auto& snapshot) {
        std::size_t host_count = 0;
        std::map<std::string, std::string> openings;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (initial.entities().contains(id)) {
                require(entity == initial.entities().at(id), "import must preserve unrelated target entities");
                continue;
            }
            if (entity.type != "wall" && entity.type != "opening") continue;
            require(entity.properties.at("floor_id") == floor.toStdString() &&
                    entity.properties.at("layer_id") == layer.toStdString(),
                    "imported profile objects must use the active destination floor and layer");
            require(!entity.required && !original.entities().contains(id),
                    "imported profile objects must be editable and receive fresh identities");
            if (entity.type == "wall") ++host_count;
            else openings.emplace(entity.properties.at("opening_kind").template get<std::string>(), id);
        }
        require(host_count == 2 && openings.size() == 2 && openings.contains("door") &&
                openings.contains("window"), "profile import must restore host, door, and window");
        for (const auto& [kind, id] : openings) {
            const auto& opening = snapshot.entities().at(id);
            const auto& expected = original.entities().at((kind == "door" ? door : window).toStdString());
            const auto host = opening.properties.at("wall_id").template get<std::string>();
            require(!initial.entities().contains(host) && snapshot.entities().at(host).type == "wall" &&
                    snapshot.entities().at(host).properties.at("baseline") ==
                        original.entities().at((kind == "door" ? wall : window_wall).toStdString()).properties.at("baseline"),
                    "each profile opening must reference its own remapped host with preserved baseline");
            require(opening.properties.at("offset_m") == expected.properties.at("offset_m"),
                    "each imported opening must retain its own host station");
            require(opening.properties.at("opening_assembly") == expected.properties.at("opening_assembly"),
                    "configured opening assembly must survive desktop exchange");
            if (kind == "door")
                require(opening.properties.at("door_operation") == expected.properties.at("door_operation"),
                        "configured door handing and angle must survive desktop exchange");
        }
        return openings;
    };
    if (!destination.importDxf(path))
        throw std::runtime_error("profile fixture must import: " + destination.lastError().toStdString());
    const auto first = inspect(destination.document().snapshot());
    require(destination.undoCommand(), "profile import must undo");
    require(destination.document().snapshot().entities() == initial.entities() &&
            destination.document().snapshot().assets() == initial.assets(),
            "profile import undo must restore destination objects and assets");
    if (!destination.importDxf(path))
        throw std::runtime_error("profile fixture must reimport after undo: " + destination.lastError().toStdString());
    const auto imported = inspect(destination.document().snapshot());
    for (const auto& [kind, id] : imported)
        require(id != first.at(kind), "reimport after undo must allocate fresh opening identities");
    const auto project = directory + "/opening-profiles-dxf.sketch";
    require(destination.saveProjectAs(project), "profile project must save");
    const auto expected_saved = destination.document().snapshot();
    require(destination.createNewProject(), "saved project ownership must be released before reopen");
    MainWindow reopened;
    require(reopened.openProject(project), "profile project must reopen");
    require(reopened.setActiveLayer(layer), "reopened imported profile layer must be activated");
    const auto saved = reopened.document().snapshot();
    inspect(saved);
    require(saved.entities() == expected_saved.entities() &&
            saved.assets() == expected_saved.assets(),
            "profile entities, hosts, assignments, and retained source must survive reopening");
    for (const auto& [kind, id] : imported) {
        (void)kind;
        if (!reopened.selectEntity(QString::fromStdString(id)) ||
                !reopened.editSelectedOpeningAssembly("0.06 m", "0.23 m", "0.025 m", "0.01 m", "0 m"))
            throw std::runtime_error("reopened " + kind + " profile must support semantic assembly editing: " +
                                     reopened.lastError().toStdString());
    }
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
#ifdef _WIN32
        BOOL in_job = FALSE;
        if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) || in_job != FALSE) {
            std::cout << "DXF desktop worker fixture skipped: host process is in a parent job\n";
            return 77;
        }
#endif
        using namespace sketch;
        using namespace sketch::desktop;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        dxfOpeningProfileDesktopRoundtrip(temporary.path());
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
        if (!destination.importDxf(path))
            throw std::runtime_error(
                "native project must import DXF: " + destination.lastError().toStdString());
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
                require(entity.properties.value("isolated_import", false),
                        "DXF source receipt must attest isolated worker parsing");
            }
        }
        require(imported_boundary, "DXF import must create a mapped boundary");
        require(retained_source, "DXF import must retain source provenance");
        require(destination.undoCommand(), "DXF import must be undoable");
        const auto restored = destination.document().snapshot();
        const auto invalid_path = temporary.filePath(QStringLiteral("invalid.dxf"));
        {
            QFile invalid(invalid_path);
            require(invalid.open(QIODevice::WriteOnly) &&
                        invalid.write("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n") > 0,
                    "invalid DXF fixture must be written");
        }
        require(!destination.importDxf(invalid_path), "malformed DXF must fail closed");
        require(destination.document().revision() == restored.revision() &&
                    destination.document().snapshot().entities() == restored.entities() &&
                    destination.document().snapshot().assets() == restored.assets(),
                "failed isolated DXF import must leave the document unchanged");
        std::cout << "DXF desktop workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
