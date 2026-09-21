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
        const auto raw = readFile(path);
        const auto mapped = import_project_ifc(raw.toStdString());
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.properties.contains("ifc_type")) {
                imported.emplace(entity.type, id);
                require(!entity.required, "imported objects must remain editable");
                require(std::none_of(mapped.entities.begin(), mapped.entities.end(),
                    [&](const auto& candidate) { return candidate.id == id; }),
                    "imported objects must receive fresh document identities");
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
                imported.contains("slab"), "IFC import must insert all four supported semantic objects");
        require(snapshot.entities().at(imported.at("opening")).properties.at("wall_id") == imported.at("wall"),
                "imported opening must reference its remapped wall");
        const auto report = nlohmann::json::parse(readFile(path + ".fidelity.json").toStdString());
        require(report.at("inserted_entity_count") == 4 && report.at("rejected_entity_count") == 0,
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
            require(entity.properties.at("inserted_entity_count") == 4 &&
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
        require(repeated.entities().size() == snapshot.entities().size() + 5,
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
        require(saved.assets() == destination.document().snapshot().assets(),
                "retained IFC source must survive reopening");
        const auto second_path = temporary.filePath("roundtrip.ifc");
        require(reopened.exportIfc(second_path), "imported semantic objects must re-export");
        const auto second = import_project_ifc(readFile(second_path).toStdString());
        for (const auto* type : {"boundary", "wall", "opening", "slab"})
            require(std::count_if(second.entities.begin(), second.entities.end(),
                [&](const auto& entity) { return entity.type == type; }) == 1,
                "re-export must retain each supported semantic object");
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
