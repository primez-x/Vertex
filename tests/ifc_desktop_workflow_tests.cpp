#include "sketch/desktop/main_window.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <algorithm>
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
QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "fixture file must be readable");
    return file.readAll();
}
}

void ifcOpeningProfileDesktopRoundtrip(const QString& directory) {
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
    const auto path = directory + "/opening-profiles.ifc";
    require(source.exportIfc(path), "profile fixture must export");
    if (const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !capture.isEmpty()) {
        QFile::remove(capture+"/desktop-hosted.ifc");
        require(QFile::copy(path,capture+"/desktop-hosted.ifc"), "desktop IFC fixture must capture");
    }

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
    if (!destination.importIfc(path))
        throw std::runtime_error("profile fixture must import: " + destination.lastError().toStdString());
    const auto first = inspect(destination.document().snapshot());
    require(destination.undoCommand(), "profile import must undo");
    require(destination.document().snapshot().entities() == initial.entities() &&
            destination.document().snapshot().assets() == initial.assets(),
            "profile import undo must restore destination objects and assets");
    if (!destination.importIfc(path))
        throw std::runtime_error("profile fixture must reimport after undo: " + destination.lastError().toStdString());
    const auto imported = inspect(destination.document().snapshot());
    for (const auto& [kind, id] : imported)
        require(id != first.at(kind), "reimport after undo must allocate fresh opening identities");
    const auto project = directory + "/opening-profiles-ifc.sketch";
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
            std::cout << "IFC desktop worker fixture skipped: host process is in a parent job\n";
            return 77;
        }
#endif
        using namespace sketch;
        using namespace sketch::desktop;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        ifcOpeningProfileDesktopRoundtrip(temporary.path());
        MainWindow source;
        const auto boundary_id = source.createBoundary({
            {{0.0, 0.0}, {4.0, 0.0}, 0.0},
            {{4.0, 0.0}, {4.0, 3.0}, 0.0},
            {{4.0, 3.0}, {0.0, 3.0}, 0.0},
            {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
        require(!boundary_id.isEmpty(), "source boundary must be created");
        const auto wall_id = source.createStraightWall({0, 0}, {4, 0});
        require(!wall_id.isEmpty() && source.selectEntity(wall_id), "source wall must be created");
        require(!source.createHostedOpening("door", "1 m", "0.9 m", "0 m", "2 m").isEmpty(),
                "source door must be created");
        require(source.selectEntity(boundary_id) &&
                !source.createSlabFromSelectedBoundary("0.15 m", "0 m").isEmpty(),
                "source slab must be created");
        auto required_sloped_wall = source.document().snapshot().entities().at(wall_id.toStdString());
        required_sloped_wall.id = "required-sloped-wall";
        required_sloped_wall.required = true;
        required_sloped_wall.properties["slope_rise_m"] = 1.0;
        required_sloped_wall.extensions["source_note"] = "retain unsupported wall semantics";
        source.document().apply(ApplyEntityChanges{source.document().revision(),
            {EntityChange::upsert(required_sloped_wall)}, {}, "Add required IFC reference fixture"});
        const auto path = temporary.filePath(QStringLiteral("mapped.ifc"));
        require(source.exportIfc(path), "native project must export IFC");
        require(QFileInfo::exists(path) && QFileInfo(path).size() > 0,
                "IFC export must write bytes");
        require(QFileInfo::exists(path + QStringLiteral(".fidelity.json")),
                "IFC export must write a fidelity report");
        {
            auto bytes = readFile(path);
            const auto terminator = bytes.indexOf("ENDSEC;\nEND-ISO-10303-21;");
            require(terminator >= 0, "fixture must have a STEP terminator");
            bytes.insert(terminator, "#999999=IFCRELAGGREGATES($,$,$,$);\n");
            QFile fixture(path);
            require(fixture.open(QIODevice::WriteOnly) && fixture.write(bytes) == bytes.size(),
                    "unsupported relationship fixture must be written");
        }

        MainWindow destination;
        const auto initial = destination.document().snapshot();
        const auto before = destination.document().revision();
        if (!destination.importIfc(path))
            throw std::runtime_error(
                "native project must import IFC: " + destination.lastError().toStdString());
        require(destination.document().revision() == before + 1,
                "IFC import must be one document revision");
        const auto snapshot = destination.document().snapshot();
        bool imported_boundary = false;
        bool retained_source = false;
        std::map<std::string, std::string> imported;
        std::string required_reference_id;
        const auto raw = readFile(path);
        const auto mapped = import_project_ifc(raw.toStdString());
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.properties.contains("ifc_type")) {
                if (entity.type != "ifc_reference") imported.emplace(entity.type, id);
                require(!entity.required, "imported objects must remain editable");
                require(std::none_of(mapped.entities.begin(), mapped.entities.end(),
                    [&](const auto& candidate) { return candidate.id == id; }),
                    "imported objects must receive fresh document identities");
                if (entity.type == "ifc_reference" &&
                    entity.extensions.contains("ifc_vertex_properties") &&
                    entity.extensions.at("ifc_vertex_properties").contains("native_entity") &&
                    entity.extensions.at("ifc_vertex_properties").at("native_entity")
                        .value("id", std::string{}) == "required-sloped-wall")
                    required_reference_id = id;
            }
            if (entity.type == "boundary" && entity.properties.contains("ifc_type"))
                imported_boundary = true;
            if (entity.type == "ifc_source") {
                retained_source = true;
                const auto asset_id = entity.properties.value("asset_id", "");
                require(snapshot.assets().contains(asset_id), "IFC source asset must be retained");
                require(entity.properties.value("isolated_import", false),
                        "IFC source receipt must attest isolated worker parsing");
                const auto& asset = snapshot.assets().at(asset_id);
                require(QByteArray(reinterpret_cast<const char*>(asset.bytes.data()),
                                  static_cast<qsizetype>(asset.bytes.size())) == raw,
                        "retained IFC bytes must be exact");
            }
        }
        require(imported_boundary, "IFC import must create a mapped boundary");
        require(retained_source, "IFC import must retain source provenance");
        require(imported.size() == 4 && imported.contains("wall") && imported.contains("opening") &&
                imported.contains("slab") && !required_reference_id.empty(),
                "IFC import must insert semantic objects and the required reference-only object");
        const auto& imported_reference = snapshot.entities().at(required_reference_id);
        require(!imported_reference.properties.contains("floor_id") &&
                !imported_reference.properties.contains("layer_id") &&
                imported_reference.extensions.at("ifc_vertex_properties").at("native_entity")
                    .at("properties").at("slope_rise_m") == 1.0,
                "reference-only IFC objects must remain inert and retain unsupported native semantics");
        require(snapshot.entities().at(imported.at("opening")).properties.at("wall_id") == imported.at("wall"),
                "imported opening must reference its remapped wall");
        const auto report = nlohmann::json::parse(readFile(path + ".fidelity.json").toStdString());
        require(report.at("inserted_entity_count") == mapped.entities.size() &&
                report.at("rejected_entity_count") == 0,
                "fidelity report must count objects actually inserted");
        require(report.at("diagnostics").size() == mapped.diagnostics.size(),
                "fidelity report must preserve mapper diagnostics");
        require(report.at("source_retention_required") == true &&
                std::any_of(mapped.diagnostics.begin(), mapped.diagnostics.end(), [](const auto& item) {
                    return item.source_kind == "IFCRELAGGREGATES" && item.code == "relationship_not_reconstructed";
                }), "unsupported IFC relationship must remain diagnosed");
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type != "ifc_source") continue;
            require(entity.properties.at("inserted_entity_count") == mapped.entities.size() &&
                    entity.properties.at("rejected_entity_count") == 0 &&
                    entity.properties.at("diagnostics") == report.at("diagnostics"),
                    "persisted source receipt must report actual insertions and fidelity diagnostics");
        }
        require(destination.undoCommand(), "IFC import must be undoable");
        require(destination.document().snapshot().entities() == initial.entities() &&
                destination.document().snapshot().assets() == initial.assets(),
                "one undo must remove all imported objects and source assets");
        require(destination.redoCommand(), "IFC import must be redoable");
        require(destination.importIfc(path), "repeated IFC import must allocate independent identities");
        const auto repeated = destination.document().snapshot();
        require(repeated.entities().size() == snapshot.entities().size() + mapped.entities.size() + 1,
                "repeated import must add rather than overwrite semantic objects");
        for (const auto& [id, entity] : repeated.entities()) {
            if (entity.type == "opening" && id != imported.at("opening"))
                require(entity.properties.at("wall_id") != imported.at("wall") &&
                        repeated.entities().at(entity.properties.at("wall_id").get<std::string>()).type == "wall",
                        "repeated import must retain its own wall host");
        }
        require(destination.undoCommand(), "repeated import must undo as one operation");
        require(destination.selectEntity(QString::fromStdString(imported.at("wall"))) &&
                destination.editSelectedHeight("3 m"), "imported wall must support semantic editing");
        require(destination.selectEntity(QString::fromStdString(imported.at("opening"))) &&
                destination.editSelectedHeight("2.1 m"), "imported opening must support semantic editing");
        require(destination.selectEntity(QString::fromStdString(imported.at("slab"))) &&
                destination.editSelectedThickness("0.2 m"), "imported slab must support semantic editing");
        const auto project_path = temporary.filePath("imported.sketch");
        require(destination.saveProjectAs(project_path), "imported project must save");
        MainWindow reopened;
        require(reopened.openProject(project_path), "imported project must reopen");
        const auto saved = reopened.document().snapshot();
        for (const auto& [type, id] : imported) {
            (void)type;
            require(saved.entities().at(id) == destination.document().snapshot().entities().at(id),
                    "saved semantic edits and host references must survive reopening");
        }
        require(saved.entities().at(required_reference_id) ==
                    destination.document().snapshot().entities().at(required_reference_id),
                "required reference-only semantics must survive reopening");
        require(saved.assets() == destination.document().snapshot().assets(),
                "retained IFC source must survive reopening");
        const auto second_path = temporary.filePath("roundtrip.ifc");
        require(reopened.exportIfc(second_path), "imported semantic objects must re-export");
        const auto second = import_project_ifc(readFile(second_path).toStdString());
        for (const auto* type : {"boundary", "wall", "opening", "slab"})
            require(std::count_if(second.entities.begin(), second.entities.end(),
                [&](const auto& entity) { return entity.type == type; }) == 1,
                "re-export must retain each supported semantic object");
        require(std::any_of(second.entities.begin(), second.entities.end(), [](const auto& entity) {
                    return entity.type == "ifc_reference" &&
                        entity.extensions.contains("ifc_vertex_properties") &&
                        entity.extensions.at("ifc_vertex_properties").contains("native_entity") &&
                        entity.extensions.at("ifc_vertex_properties").at("native_entity")
                            .value("id", std::string{}) == "required-sloped-wall";
                }), "re-export must retain the named reference-only native payload");
        const auto invalid_path = temporary.filePath("invalid.ifc");
        {
            QFile invalid(invalid_path);
            require(invalid.open(QIODevice::WriteOnly) && invalid.write("invalid STEP") > 0,
                    "invalid IFC fixture must be written");
        }
        require(!reopened.importIfc(invalid_path), "malformed IFC must fail closed");
        require(reopened.document().revision() == saved.revision() &&
                reopened.document().snapshot().entities() == saved.entities() &&
                reopened.document().snapshot().assets() == saved.assets(),
                "failed import must leave document and retained assets unchanged");
        std::cout << "IFC desktop workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
