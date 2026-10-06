#include "sketch/desktop/main_window.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include "sketch/project_import_worker.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/visualization/native_geometry_preparation.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void requireMainOperation(bool value, const sketch::desktop::MainWindow& window,
                          const char* message) {
    if (!value)
        throw std::runtime_error(std::string{message} + ": " + window.lastError().toStdString());
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

void ifcNativeFamilyDesktopRoundtrip(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    using Json = nlohmann::json;
    MainWindow source;
    source.setMetricUnits(true);
    const auto source_building = source.createBuilding("property-1", "Native IFC source");
    requireMainOperation(!source_building.isEmpty(), source, "native source building must be created");
    const auto source_floor = source.createFloor(source_building, "Source floor");
    requireMainOperation(!source_floor.isEmpty(), source, "native source floor must be created");
    const auto source_layer = source.createLayer(source_floor, "Source native families");
    requireMainOperation(!source_layer.isEmpty(), source, "native source layer must be created");
    requireMainOperation(source.setActiveLayer(source_layer), source, "native source layer must be activated");
    const auto human = [](const char* family) {
        return Json{{"human", {{"family", family}, {"opaque", {{"stair_id", "native-stair"},
            {"incoming_flight_id", "native-lower"}, {"landing_id", "native-middle"}}}}}};
    };
    StairFlight stair{"native-stair", {2, 3, 0}, .37, 8, 1.6, .3, 1,
                      StairLanding{1.1, .15}};
    stair.flights = {{"native-lower", 4}, {"native-upper", 4}};
    stair.landings = {{"native-middle", 1.3, .15, StairTurn::straight, 0}};
    const auto stair_id = source.commitBuildingObject(encode_building_entity(stair, human("stair")),
                                                     source.document().revision());
    requireMainOperation(stair_id == QString::fromStdString(stair.id), source,
                         "native compound stair must be created through Main");
    Railing guard{"native-guard", {}, 0, 0, .9, .04, .3};
    guard.landing_host = StairLandingRailingHost{stair.id, StairLandingRole::connecting,
        "native-middle", "native-lower", "native-upper", 0, .1, .9};
    const auto exposed = derive_stair_landing_edge(stair, *guard.landing_host);
    require(std::any_of(exposed.exposed_intervals.begin(), exposed.exposed_intervals.end(),
        [&](const auto& interval) { return interval.start_fraction <= guard.landing_host->start_fraction &&
            interval.end_fraction >= guard.landing_host->end_fraction; }),
        "native v3 guard must cover an actually exposed edge of the current landing");
    const auto guard_id = source.commitBuildingObject(encode_building_entity(guard, human("guard")),
                                                     source.document().revision());
    requireMainOperation(!guard_id.isEmpty(), source, "Main must create the native v3 landing guard");
    Railing flight_rail{"native-flight-rail", {}, 0, 0, 1, .04, .45};
    flight_rail.host = StairRailingHost{stair.id, "native-upper", StairRailingSide::right, .1, .9};
    auto flight_entity = encode_building_entity(flight_rail, human("flight-rail"));
    // These names are opaque extras in v2, even when their values look like
    // canonical child identities. Only flight_id is a v2 host child reference.
    flight_entity.properties["host"]["landing_id"] = "native-middle";
    flight_entity.properties["host"]["incoming_flight_id"] = "native-lower";
    flight_entity.properties["host"]["outgoing_flight_id"] = "native-upper";
    const auto rail_id = source.commitBuildingObject(flight_entity,
                                                    source.document().revision());
    requireMainOperation(!rail_id.isEmpty(), source, "Main must create the native v2 flight rail with opaque host extras");
    const auto roof_id = source.commitBuildingObject(encode_building_entity(
        SlopedRoofPanel{"native-roof", {10, 8, 4}, .15, 4, 3, 1, std::atan(.25), .2, .1,
                        {{"native-roof-opening", 1, 1, .5, .5}}}, human("roof")), source.document().revision());
    requireMainOperation(!roof_id.isEmpty(), source, "Main must create the native roof with a through-opening");
    const double quarter = std::acos(-1.0) / 2;
    const auto room_id = source.createRoomVolumeFromBoundary({
        {{14, 2}, {12, 4}, quarter}, {{12, 4}, {10, 2}, quarter},
        {{10, 2}, {12, 0}, quarter}, {{12, 0}, {14, 2}, quarter}}, "2.5 m", "0 m");
    requireMainOperation(!room_id.isEmpty(), source, "Main must create the native curved room");
    auto room = source.document().snapshot().entities().at(room_id.toStdString());
    room.extensions.update(human("room"));
    auto source_property = source.document().snapshot().entities().at("property-1");
    // Exercise the bounded chunk carrier independently of the default profile's
    // current size. This opaque property witness must survive as inert provenance.
    source_property.extensions["ifc_provenance_witness"] = std::string(6000, 'p');
    source.document().apply(ApplyEntityChanges{source.document().revision(),
        {EntityChange::upsert(room), EntityChange::upsert(source_property)}, {}, "Decorate native IFC source metadata"});
    const auto original = source.document().snapshot();
    // Main exports organization and unsupported annotation/receipt records as
    // exact inert provenance. They are separate from physical-family admission.
    const auto retainedDescriptor = [](const Entity& entity) {
        return Json{{"native_entity", {{"id", entity.id}, {"type", entity.type},
            {"required", entity.required}, {"properties", entity.properties},
            {"extensions", entity.extensions}}}};
    };
    const auto expectedDescriptors = [&](const DocumentSnapshot& snapshot) {
        std::multiset<std::string> descriptors;
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "property" || entity.type == "building" || entity.type == "floor" ||
                entity.type == "annotation_state")
                descriptors.insert(retainedDescriptor(entity).dump());
        }
        return descriptors;
    };
    const auto source_descriptors = expectedDescriptors(original);
    IfcExchangeLimits restricted_retention;
    restricted_retention.max_string_bytes = 256;
    const auto withheld = export_project_ifc(Document::create({source_property}).snapshot(), restricted_retention);
    require(std::any_of(withheld.diagnostics.begin(), withheld.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.source_id == "property-1" && diagnostic.code == "vertex_properties_not_exported";
            }) && withheld.step.find("Pset_VertexExchange_v2") == std::string::npos,
            "string limits below the bounded chunk minimum must diagnose withheld property metadata");
    const std::map<std::string, std::string> source_ids{{"stair", stair_id.toStdString()},
        {"guard", guard_id.toStdString()}, {"flight-rail", rail_id.toStdString()},
        {"roof", roof_id.toStdString()}, {"room", room_id.toStdString()}};
    for (const auto& [family, id] : source_ids) {
        (void)family;
        const auto context = organize_project(original).drawing_context(id);
        require(context && context->complete() && context->layer_id == source_layer.toStdString(),
                "every exported native family must use the real source context");
    }
    const auto path = directory + "/native-families.ifc";
    requireMainOperation(source.exportIfc(path), source, "native family source must export through Main");
    const auto raw = readFile(path);
    const auto fidelity = Json::parse(readFile(path + ".fidelity.json").toStdString());
    require(raw.contains("Pset_VertexExchange_v2") &&
            std::none_of(fidelity.at("diagnostics").begin(), fidelity.at("diagnostics").end(), [](const auto& diagnostic) {
                return diagnostic.at("code") == "vertex_properties_not_exported";
            }), "native source export must retain complete bounded property provenance instead of an empty proxy");
    require(raw.count("=IFCSTAIR(") == 1 && raw.count("=IFCRAILING(") == 2 &&
            raw.count("=IFCROOF(") == 1 && raw.count("=IFCSPACE(") == 1 &&
            raw.count("=IFCSTAIRFLIGHT(") == 0,
            "native source must export exact physical family counts without duplicate stair children");
    if (const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !capture.isEmpty()) {
        QFile::remove(capture + "/desktop-native-families.ifc");
        require(QFile::copy(path, capture + "/desktop-native-families.ifc"),
                "native family IFC fixture must capture");
    }
    if (static_cast<std::size_t>(raw.count("=IFCBUILDINGELEMENTPROXY(")) != source_descriptors.size())
        throw std::runtime_error("native source must export exactly its known organization and annotation provenance: expected=" +
            std::to_string(source_descriptors.size()) + " actual=" +
            std::to_string(raw.count("=IFCBUILDINGELEMENTPROXY(")));

    MainWindow destination;
    destination.setMetricUnits(true);
    destination.setAttribute(Qt::WA_DontShowOnScreen);
    destination.setWorkspace(Workspace::architectural);
    destination.setNativeModelViewVisible(false);
    const auto unrelated = destination.createStraightWall({20, 20}, {23, 20});
    requireMainOperation(!unrelated.isEmpty(), destination, "native destination unrelated wall must be created");
    const auto building = destination.createBuilding("property-1", "Native IFC destination");
    requireMainOperation(!building.isEmpty(), destination, "native destination building must be created");
    const auto floor = destination.createFloor(building, "Imported native floor");
    requireMainOperation(!floor.isEmpty(), destination, "native destination floor must be created");
    const auto layer = destination.createLayer(floor, "Imported native families");
    requireMainOperation(!layer.isEmpty(), destination, "native destination layer must be created");
    requireMainOperation(destination.setActiveLayer(layer), destination, "native destination layer must be activated");
    const auto initial = destination.document().snapshot();
    const auto source_children = stair_child_ids(stair);
    const auto inspect = [&](const DocumentSnapshot& snapshot) {
        const auto admissionDiagnostic = [&] {
            Json detail{{"expected_native_families", source_ids.size()},
                        {"expected_provenance_references", source_descriptors.size()},
                        {"source_proxy_count", raw.count("=IFCBUILDINGELEMENTPROXY(")},
                        {"new_type_counts", Json::object()}, {"new_entities", Json::array()},
                        {"receipts", Json::array()}};
            std::map<std::string, std::size_t> type_counts;
            std::size_t new_entity_count = 0;
            for (const auto& [new_id, added] : snapshot.entities()) {
                if (initial.entities().contains(new_id)) continue;
                ++new_entity_count;
                ++type_counts[added.type];
                if (detail["new_entities"].size() < 12) {
                    Json item{{"id", new_id}, {"type", added.type}};
                    if (const auto source_record = added.extensions.find("ifc_source");
                        source_record != added.extensions.end() && source_record->is_object()) {
                        for (const auto* key : {"record_id", "record_type"})
                            if (source_record->contains(key)) item[key] = source_record->at(key);
                    }
                    if (const auto human = added.extensions.find("human");
                        human != added.extensions.end() && human->is_object() && human->contains("family"))
                        item["family"] = human->at("family");
                    if (const auto metadata = added.extensions.find("ifc_vertex_properties");
                        metadata != added.extensions.end() && metadata->is_object() &&
                        metadata->contains("_vertex_ifc_entity")) {
                        const auto& envelope = metadata->at("_vertex_ifc_entity");
                        if (envelope.is_object() && envelope.contains("type"))
                            item["source_entity_type"] = envelope.at("type");
                    }
                    if (const auto metadata = added.extensions.find("ifc_vertex_properties");
                        metadata != added.extensions.end() && metadata->is_object() && metadata->contains("native_entity")) {
                        const auto& descriptor = metadata->at("native_entity");
                        if (descriptor.is_object() && descriptor.contains("type"))
                            item["source_entity_type"] = descriptor.at("type");
                    }
                    detail["new_entities"].push_back(std::move(item));
                }
                if (added.type != "ifc_source" || detail["receipts"].size() >= 2) continue;
                Json receipt = Json::object();
                for (const auto* key : {"isolated_import", "mapped_entity_count", "inserted_entity_count",
                                       "rejected_entity_count", "source_retention_required"})
                    receipt[key] = added.properties.contains(key) ? added.properties.at(key) : Json{};
                receipt["diagnostics"] = Json::array();
                if (const auto diagnostics = added.properties.find("diagnostics");
                    diagnostics != added.properties.end() && diagnostics->is_array()) {
                    receipt["diagnostic_count"] = diagnostics->size();
                    for (const auto& diagnostic : *diagnostics) {
                        if (receipt["diagnostics"].size() >= 16) break;
                        receipt["diagnostics"].push_back(diagnostic);
                    }
                }
                detail["receipts"].push_back(std::move(receipt));
            }
            detail["new_type_counts"] = type_counts;
            detail["new_entity_count"] = new_entity_count;
            return detail.dump();
        };
        std::map<std::string, std::string> ids;
        std::map<std::string, std::size_t> counts;
        std::multiset<std::string> retained_descriptors;
        std::size_t receipts = 0;
        for (const auto& [id, entity] : snapshot.entities()) {
            require(!id.starts_with("ifc-validation-"), "synthetic validation organization must never enter the project");
            if (initial.entities().contains(id)) {
                require(entity == initial.entities().at(id), "native import must preserve unrelated destination content");
                continue;
            }
            if (entity.type == "ifc_source") {
                ++receipts;
                if (!(entity.properties.at("isolated_import") == true &&
                      entity.properties.at("mapped_entity_count") == source_ids.size() + source_descriptors.size() &&
                      entity.properties.at("inserted_entity_count") == source_ids.size() + source_descriptors.size() &&
                      entity.properties.at("rejected_entity_count") == 0))
                    throw std::runtime_error("native receipt must attest actual broker parsing, all five admitted families, and exact known provenance: " +
                                             admissionDiagnostic());
                const auto& asset = snapshot.assets().at(entity.properties.at("asset_id").get<std::string>());
                require(QByteArray(reinterpret_cast<const char*>(asset.bytes.data()),
                                  static_cast<qsizetype>(asset.bytes.size())) == raw,
                        "native import must retain exact original IFC bytes");
                continue;
            }
            if (entity.type == "ifc_reference") {
                require(!entity.required && !original.entities().contains(id) &&
                        !entity.properties.contains("property_id") && !entity.properties.contains("building_id") &&
                        !entity.properties.contains("floor_id") && !entity.properties.contains("layer_id"),
                        "source organization provenance must receive fresh identities and remain inert");
                const auto descriptor = entity.extensions.at("ifc_vertex_properties").dump();
                require(source_descriptors.contains(descriptor),
                        "every inert reference must match exact known source organization or annotation provenance");
                retained_descriptors.insert(descriptor);
                continue;
            }
            if (!(entity.type == "stair" || entity.type == "railing" || entity.type == "roof" || entity.type == "room"))
                throw std::runtime_error("native import must not create unexpected inert references or duplicate measurement sections: " +
                                         admissionDiagnostic());
            ++counts[entity.type];
            const auto family = entity.extensions.at("human").at("family").get<std::string>();
            require(ids.emplace(family, id).second && !entity.required && !original.entities().contains(id),
                    "every imported native family must be unique, editable, and freshly identified");
            const auto context = organize_project(snapshot).drawing_context(id);
            require(context && context->complete() && context->property_id == "property-1" &&
                    context->building_id == building.toStdString() && context->floor_id == floor.toStdString() &&
                    context->layer_id == layer.toStdString(), "all active native families must use full destination context");
            require(entity.properties.at("property_id") == "property-1" &&
                    entity.properties.at("building_id") == building.toStdString() &&
                    entity.properties.at("floor_id") == floor.toStdString() &&
                    entity.properties.at("layer_id") == layer.toStdString(),
                    "native objects must persist every destination context field explicitly");
            const auto& expected = original.entities().at(source_ids.at(family));
            Json envelope{{"version", 1}, {"type", expected.type}, {"required", expected.required},
                          {"properties", expected.properties}, {"extensions", expected.extensions}};
            if (expected.type == "stair" || expected.type == "railing") envelope["id"] = expected.id;
            require(entity.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity") == envelope &&
                    entity.extensions.at("human") == expected.extensions.at("human"),
                    "original context, authoring, and opaque source witness metadata must remain exact");
        }
        require(counts == std::map<std::string, std::size_t>{{"stair", 1}, {"railing", 2}, {"roof", 1}, {"room", 1}} &&
                ids.size() == source_ids.size() && retained_descriptors == source_descriptors &&
                receipts == 1 && snapshot.assets().size() == initial.assets().size() + 1,
                "real broker import must admit exactly five native families, exact known inert provenance, and one source receipt/asset");
        const auto imported_stair = decode_stair_properties(ids.at("stair"), snapshot.entities().at(ids.at("stair")).properties);
        const auto children = stair_child_ids(imported_stair);
        require(children.size() == source_children.size() && std::set<std::string>(children.begin(), children.end()).size() == children.size(),
                "imported compound topology must retain independent unique children");
        for (const auto& child : children)
            require(std::find(source_children.begin(), source_children.end(), child) == source_children.end() &&
                    !original.entities().contains(child) && !snapshot.entities().contains(child),
                    "stair child identities must be fresh and remain nested authoring records");
        auto expected_stair = stair;
        for (std::size_t i = 0; i < expected_stair.flights.size(); ++i) expected_stair.flights[i].id = imported_stair.flights[i].id;
        for (std::size_t i = 0; i < expected_stair.landings.size(); ++i) expected_stair.landings[i].id = imported_stair.landings[i].id;
        require(encode_stair_properties(imported_stair) == encode_stair_properties(expected_stair),
                "native stair geometry and ordered topology must survive child remapping exactly");
        auto expected_guard = guard;
        expected_guard.landing_host->stair_id = ids.at("stair");
        expected_guard.landing_host->landing_id = imported_stair.landings[0].id;
        expected_guard.landing_host->incoming_flight_id = imported_stair.flights[0].id;
        expected_guard.landing_host->outgoing_flight_id = imported_stair.flights[1].id;
        auto expected_rail = flight_rail;
        expected_rail.host->stair_id = ids.at("stair");
        expected_rail.host->flight_id = imported_stair.flights[1].id;
        require(encode_railing_properties(decode_railing_properties(ids.at("guard"), snapshot.entities().at(ids.at("guard")).properties)) ==
                    encode_railing_properties(expected_guard) &&
                encode_railing_properties(decode_railing_properties(ids.at("flight-rail"), snapshot.entities().at(ids.at("flight-rail")).properties)) ==
                    encode_railing_properties(expected_rail),
                "v3 landing witnesses and v2 flight hosts must remap to the actual fresh compound topology");
        for (const auto* key : {"landing_id", "incoming_flight_id", "outgoing_flight_id"})
            require(snapshot.entities().at(ids.at("flight-rail")).properties.at("host").at(key) ==
                flight_entity.properties.at("host").at(key), "v2 opaque host extras must not be remapped as v3 witness authority");
        require(snapshot.entities().at(ids.at("room")).properties.at("boundary") == room.properties.at("boundary") &&
                snapshot.entities().at(ids.at("roof")).properties.at("roof_openings") ==
                    original.entities().at(roof_id.toStdString()).properties.at("roof_openings"),
                "native curved room arcs and roof through-opening authoring must remain exact");
        return ids;
    };
    const auto import = [&] {
        if (!destination.importIfc(path)) throw std::runtime_error("native families must import: " + destination.lastError().toStdString());
        return inspect(destination.document().snapshot());
    };
    const auto first = import();
    const auto first_children = stair_child_ids(decode_stair_properties(first.at("stair"),
        destination.document().snapshot().entities().at(first.at("stair")).properties));
    requireMainOperation(destination.undoCommand(), destination, "native import must undo");
    require(destination.document().snapshot().entities() == initial.entities() &&
            destination.document().snapshot().assets() == initial.assets(), "one native import undo must restore destination and assets exactly");
    const auto ids = import();
    for (const auto& [family, id] : ids) require(id != first.at(family), "reimport must allocate fresh native entity identities");
    for (const auto& child : stair_child_ids(decode_stair_properties(ids.at("stair"),
            destination.document().snapshot().entities().at(ids.at("stair")).properties)))
        require(std::find(first_children.begin(), first_children.end(), child) == first_children.end(),
                "reimport must allocate fresh child and landing witness identities after undo");
    const auto reexport = [&](const char* name) {
        const auto target = destination.document().snapshot();
        auto expected_references = expectedDescriptors(initial);
        expected_references.insert(source_descriptors.begin(), source_descriptors.end());
        std::size_t source_receipts = 0;
        for (const auto& [id, entity] : target.entities()) {
            (void)id;
            if (entity.type != "ifc_source") continue;
            ++source_receipts;
            expected_references.insert(retainedDescriptor(entity).dump());
        }
        require(source_receipts == 1, "re-export must retain exactly one actual import source receipt");
        require(target.entities().at(unrelated.toStdString()) == initial.entities().at(unrelated.toStdString()),
                "native authoring must preserve the unrelated destination wall");
        const auto export_path = directory + "/" + QString::fromLatin1(name) + ".ifc";
        requireMainOperation(destination.exportIfc(export_path), destination, "imported native graph must re-export through Main");
        if (const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !capture.isEmpty()) {
            const auto captured = capture + "/" + QString::fromLatin1(name) + ".ifc";
            QFile::remove(captured);
            require(QFile::copy(export_path, captured), "re-export evidence must be retained");
        }
        const auto recovered = import_project_ifc(readFile(export_path).toStdString());
        require(recovered.entities.size() == ids.size() + expected_references.size() + 1,
                "re-export must contain five native families, the unrelated wall, and exact known provenance only");
        std::map<std::string, Entity> families;
        std::multiset<std::string> recovered_references;
        std::size_t unrelated_walls = 0;
        for (const auto& entity : recovered.entities) {
            if (entity.type == "ifc_reference") {
                const auto descriptor = entity.extensions.at("ifc_vertex_properties").dump();
                require(expected_references.contains(descriptor),
                        std::string{name} + ": re-exported inert provenance must match exact source, destination, or receipt descriptors");
                recovered_references.insert(descriptor);
                continue;
            }
            if (entity.type == "wall") {
                ++unrelated_walls;
                const auto& expected_wall = initial.entities().at(unrelated.toStdString());
                require(entity.properties.at("ifc_name") == expected_wall.id &&
                        entity.properties.at("baseline") == expected_wall.properties.at("baseline") &&
                        entity.properties.at("thickness_m") == expected_wall.properties.at("thickness_m") &&
                        entity.properties.at("height_m") == expected_wall.properties.at("height_m") &&
                        entity.properties.at("elevation_m") == expected_wall.properties.at("elevation_m"),
                        "re-export must preserve the unrelated wall's actual physical geometry");
                continue;
            }
            require(entity.type == "stair" || entity.type == "railing" || entity.type == "roof" || entity.type == "room",
                    "re-export must not add unexpected active or measurement objects");
            require(families.emplace(entity.extensions.at("human").at("family").get<std::string>(), entity).second,
                    "re-export must preserve unique native family cardinality");
        }
        require(families.size() == ids.size() && unrelated_walls == 1 && recovered_references == expected_references,
                "re-export must recover every native family and exactly preserve known unrelated content and provenance");
        return families;
    };
    const auto unchanged = reexport("native-families-unchanged");
    for (const auto& [family, id] : ids)
        require(unchanged.at(family).extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity") ==
                    destination.document().snapshot().entities().at(id).extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity"),
                std::string{"unchanged re-export must retain the exact original source proof for "} + family);
    auto opaque_edit = destination.document().snapshot().entities().at(ids.at("flight-rail"));
    const auto opaque_value = destination.document().snapshot().entities().at(ids.at("stair")).properties.at("landings")[0].at("id");
    opaque_edit.properties["host"]["landing_id"] = opaque_value;
    const auto prior_opaque_proof = opaque_edit.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
    destination.document().apply(ApplyEntityChanges{destination.document().revision(),
        {EntityChange::upsert(opaque_edit)}, {}, "Edit opaque v2 host metadata"});
    const auto opaque_reexport = reexport("native-families-opaque-host-edit");
    require(opaque_reexport.at("flight-rail").properties.at("host").at("landing_id") == opaque_value &&
        opaque_reexport.at("flight-rail").extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity") != prior_opaque_proof &&
        destination.document().snapshot().entities().at(ids.at("flight-rail")).extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity") == prior_opaque_proof,
        "a v2 opaque host-field edit must produce a fresh export proof while preserving the immutable old source evidence");
    const auto before_metadata = destination.document().snapshot();
    std::vector<EntityChange> metadata_changes;
    for (const auto* family : {"stair", "guard", "roof", "room"}) {
        auto edited = before_metadata.entities().at(ids.at(family));
        edited.extensions["human"]["reviewed_note"] = std::string{"Updated imported "} + family;
        metadata_changes.push_back(EntityChange::upsert(std::move(edited)));
    }
    destination.document().apply(ApplyEntityChanges{before_metadata.revision(), std::move(metadata_changes), {},
                                                   "Edit imported native human metadata"});
    requireMainOperation(destination.selectEntity(QString::fromStdString(ids.at("stair"))), destination,
            "Main must refresh the imported graph after ordinary metadata commands");
    const auto after_metadata = destination.document().snapshot();
    const auto metadata_edited = reexport("native-families-metadata-edited");
    for (const auto* family : {"stair", "guard", "roof", "room"}) {
        const auto& active = after_metadata.entities().at(ids.at(family));
        const auto& recovered = metadata_edited.at(family);
        const auto& envelope = recovered.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
        require(active.extensions.at("ifc_vertex_properties") == before_metadata.entities().at(ids.at(family)).extensions.at("ifc_vertex_properties") &&
                recovered.extensions.at("human") == active.extensions.at("human") &&
                envelope.at("extensions").at("human") == active.extensions.at("human") &&
                envelope != unchanged.at(family).extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity"),
                std::string{"human metadata edits must export fresh source proof without rewriting old opaque evidence for "} + family);
    }
    const auto before_required = destination.document().snapshot();
    std::vector<EntityChange> required_changes;
    for (const auto* family : {"roof", "room"}) {
        auto edited = before_required.entities().at(ids.at(family));
        edited.required = true;
        required_changes.push_back(EntityChange::upsert(std::move(edited)));
    }
    destination.document().apply(ApplyEntityChanges{before_required.revision(), std::move(required_changes), {},
                                                   "Require imported roof and room"});
    requireMainOperation(destination.selectEntity(QString::fromStdString(ids.at("roof"))), destination,
                         "Main must refresh required imported families");
    const auto required_edited = reexport("native-families-required-edited");
    for (const auto* family : {"roof", "room"}) {
        const auto& original_proof = before_required.entities().at(ids.at(family)).extensions.at("ifc_vertex_properties");
        const auto& new_proof = required_edited.at(family).extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
        require(new_proof.at("required") == true &&
                destination.document().snapshot().entities().at(ids.at(family)).extensions.at("ifc_vertex_properties") == original_proof,
                std::string{"a required-flag edit must export fresh proof without changing retained source evidence for "} + family);
    }
    require(destination.undoCommand(), "required-flag edit must undo atomically before physical editing");
    const auto native = [&](MainWindow& window) {
        QElapsedTimer timer; timer.start();
        while (!window.regenerationReadyForCurrentRevision() && timer.elapsed() < 15000) {
            QApplication::processEvents(); QThread::msleep(1);
        }
        require(window.regenerationReadyForCurrentRevision(), "Main must prepare native 3D for the current imported revision");
        auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
        require(canvas, "native families must use Main's actual architectural plan canvas");
        auto prepared = visualization::prepare_native_geometry(window.document().snapshot(), std::nullopt);
        require(prepared && prepared->errors.empty() && prepared->pending.empty(), "imported native geometry must prepare without unresolved objects");
        for (const auto& [family, id] : ids) {
            (void)family;
            require(std::any_of(canvas->entities().begin(), canvas->entities().end(), [&](const auto& item) {
                        return item.id.toStdString() == id && !item.segments.empty(); }) &&
                    prepared->solids.contains(id) && !prepared->solids.at(id).shape.IsNull(),
                    "every imported family must appear as actual editable plan geometry and a native 3D solid");
        }
        return std::move(*prepared);
    };
    const auto plan_bounds = [&] {
        auto* canvas = dynamic_cast<PlanCanvas*>(destination.findChild<QWidget*>("architecturalPlanCanvas"));
        require(canvas, "native transform must retain the actual plan canvas");
        std::map<std::string, std::array<double, 4>> result;
        for (const auto* family : {"stair", "guard", "flight-rail"}) {
            const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
                [&](const auto& item) { return item.id.toStdString() == ids.at(family); });
            require(found != canvas->entities().end() && !found->segments.empty(), "transformed host graph must retain actual plan edges");
            const auto first_point = found->segments.front().start;
            std::array<double, 4> value{first_point.x, first_point.y, first_point.x, first_point.y};
            for (const auto& edge : found->segments) for (const auto point : {edge.start, edge.end}) {
                value[0] = std::min(value[0], point.x); value[1] = std::min(value[1], point.y);
                value[2] = std::max(value[2], point.x); value[3] = std::max(value[3], point.y);
            }
            result.emplace(family, value);
        }
        return result;
    };
    const auto before_edit = native(destination);
    const auto before_plan = plan_bounds();
    requireMainOperation(destination.selectEntity(QString::fromStdString(ids.at("stair"))), destination,
                         "imported stair must be selected for native transformation");
    requireMainOperation(destination.transformSelectedArchitecturalObject("0", "2 m", "1 m", "0 m", "1", false), destination,
            "imported stair must support Main's native architectural transform with attached guards");
    const auto after_edit = native(destination);
    const auto after_plan = plan_bounds();
    const auto bounds = [](const TopoDS_Shape& shape) {
        Bnd_Box box; BRepBndLib::Add(shape, box);
        require(!box.IsVoid(), "native edited solid must have actual bounds");
        std::array<double, 6> value{}; box.Get(value[0], value[1], value[2], value[3], value[4], value[5]); return value;
    };
    for (const auto* family : {"stair", "guard", "flight-rail"}) {
        for (std::size_t i = 0; i < before_plan.at(family).size(); ++i)
            require(std::abs(after_plan.at(family)[i] - before_plan.at(family)[i] - (i % 2 == 0 ? 2 : 1)) < 1e-6,
                    "native host transform must refresh actual stair and both hosted rail plan projections");
        const auto before = bounds(before_edit.solids.at(ids.at(family)).shape);
        const auto after = bounds(after_edit.solids.at(ids.at(family)).shape);
        for (std::size_t i = 0; i < before.size(); ++i)
            require(std::abs(after[i] - before[i] - (i % 3 == 0 ? 2 : i % 3 == 1 ? 1 : 0)) < 1e-6,
                    "native stair transform must move actual v2/v3 hosted solids by the same world displacement");
    }
    const auto physically_edited = reexport("native-families-translated");
    const auto translated_snapshot = destination.document().snapshot();
    const auto& translated_stair = translated_snapshot.entities().at(ids.at("stair"));
    const auto& fresh_proof = physically_edited.at("stair").extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
    auto translated_extensions = translated_stair.extensions;
    translated_extensions.erase("ifc_source"); translated_extensions.erase("ifc_vertex_properties");
    require(fresh_proof.at("id") == translated_stair.id && fresh_proof.at("properties") == translated_stair.properties &&
            fresh_proof.at("extensions") == translated_extensions &&
            fresh_proof != unchanged.at("stair").extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity"),
            "legitimate stair physical edit must export a fresh proof with current topology and unchanged opaque metadata");
    auto edited_guard = destination.document().snapshot().entities().at(ids.at("guard"));
    edited_guard.properties["height_m"] = 1.1;
    requireMainOperation(destination.selectEntity(QString::fromStdString(edited_guard.id)), destination,
                         "imported landing guard must be selected for editing");
    requireMainOperation(!destination.commitBuildingObject(edited_guard, destination.document().revision(), true).isEmpty(), destination,
                         "imported landing guard must support semantic Main editing");
    requireMainOperation(destination.selectEntity(QString::fromStdString(ids.at("room"))), destination,
                         "imported curved room must be selected for editing");
    requireMainOperation(destination.editSelectedRoomVolume("2.7 m", "0 m"), destination,
                         "imported curved room must support semantic Main editing");
    requireMainOperation(destination.selectEntity(QString::fromStdString(ids.at("roof"))), destination,
                         "imported opened roof must be selected for editing");
    requireMainOperation(destination.transformSelectedArchitecturalObject("0", "0 m", "0 m", "0.2 m", "1", false), destination,
                         "imported opened roof must support semantic Main editing");
    const auto project = directory + "/native-families-ifc.sketch";
    requireMainOperation(destination.saveProjectAs(project), destination, "edited native family project must save");
    const auto expected_saved = destination.document().snapshot();
    requireMainOperation(destination.createNewProject(), destination, "native source project ownership must release before reopening");
    MainWindow reopened;
    reopened.setAttribute(Qt::WA_DontShowOnScreen);
    reopened.setWorkspace(Workspace::architectural);
    reopened.setNativeModelViewVisible(false);
    requireMainOperation(reopened.openProject(project), reopened, "native project must reopen");
    requireMainOperation(reopened.setActiveLayer(layer), reopened, "native reopened destination layer must be activated");
    const auto saved = reopened.document().snapshot();
    require(saved.entities() == expected_saved.entities() && saved.assets() == expected_saved.assets(),
            "native semantic edits, child hosts, source metadata, context, and exact assets must survive save/reopen");
    (void)native(reopened);
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        using namespace sketch;
        using namespace sketch::desktop;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        ifcOpeningProfileDesktopRoundtrip(temporary.path());
        ifcNativeFamilyDesktopRoundtrip(temporary.path());
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
            // Retain a well-formed spatial relationship. Malformed four-field
            // aggregates are separately rejected by the core admission checks.
            const QByteArray aggregate("=IFCRELAGGREGATES(");
            const auto relation = bytes.indexOf(aggregate);
            require(relation >= 0, "fixture needs an actual spatial aggregate");
            const auto start = relation + aggregate.size();
            const auto end = bytes.indexOf(");\n", start);
            require(end > start, "spatial aggregate must have a complete row");
            auto arguments = bytes.mid(start, end - start);
            const auto global_id_end = arguments.indexOf(',');
            require(arguments.startsWith('\'') && global_id_end > 0,
                    "spatial aggregate must have an explicit global identity");
            arguments.replace(0, global_id_end, "'3aaaaaaaaaaaaaaaaaaaaa'");
            bytes.insert(terminator, QByteArray("#999999=IFCRELAGGREGATES(") + arguments + ");\n");
            QFile fixture(path);
            require(fixture.open(QIODevice::WriteOnly) && fixture.write(bytes) == bytes.size(),
                    "unsupported relationship fixture must be written");
        }

        if (const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !capture.isEmpty()) {
            require(QFile::copy(path, capture + "/desktop-mapped-unsupported.ifc"),
                    "mapped IFC input must be retained before its actual broker import");
        }
        MainWindow destination;
        const auto initial = destination.document().snapshot();
        const auto before = destination.document().revision();
        if (!destination.importIfc(path)) {
            // Retain the actual broker's bounded control/status codes when a
            // later integrated phase fails after the earlier imports passed.
            // This diagnostic retry never replaces MainWindow's failed result.
            WindowsImportWorkerOptions options;
            const auto root = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
            options.executable = root / "vertex-import-worker.exe";
            options.immutable_module_roots = {root};
            const auto plugins = root.parent_path() / "plugins";
            if (std::filesystem::is_directory(plugins)) options.immutable_module_roots.push_back(plugins);
            options.temporary_root = std::filesystem::path(QDir::tempPath().toStdWString());
            const auto bytes = readFile(path);
            try {
                (void)import_project_in_worker(std::span(reinterpret_cast<const std::byte*>(bytes.constData()),
                    static_cast<std::size_t>(bytes.size())), ProjectImportKind::ifc, std::move(options),
                    [](const WindowsImportWorkerOptions& actual) {
                        auto report = run_windows_import_worker(actual);
                        std::cerr << "Failed-phase actual broker: " << report.to_json().dump() << '\n';
                        return report;
                    });
            } catch (const std::exception&) { }
            throw std::runtime_error(
                "native project must import IFC: " + destination.lastError().toStdString());
        }
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
        if (report.at("diagnostics").size() != mapped.diagnostics.size())
            std::cerr << "Worker diagnostics: " << report.at("diagnostics").dump()
                      << " native count=" << mapped.diagnostics.size() << '\n';
        require(report.at("diagnostics").size() == mapped.diagnostics.size(),
                "fidelity report must preserve mapper diagnostics");
        require(report.at("source_retention_required") == true &&
                std::any_of(mapped.diagnostics.begin(), mapped.diagnostics.end(), [](const auto& item) {
                    return item.source_id == "#999999" && item.source_kind == "IFCRELAGGREGATES" &&
                           item.code == "relationship_not_reconstructed";
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
