#include "sketch/floor_reference.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using nlohmann::json;
using sketch::Entity;
using sketch::FloorReferenceSettings;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid reference was accepted");
}
Entity entity(std::string id, std::string type, json properties = json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, json::object()};
}
std::vector<Entity> fixture() {
    return {entity("property", "property"),
        entity("building", "building", {{"property_id", "property"}}),
        entity("source", "floor", {{"building_id", "building"}}),
        entity("destination", "floor", {{"building_id", "building"},
            {"tracing_reference", FloorReferenceSettings{"source"}.to_json()}})};
}
void test_codec() {
    const FloorReferenceSettings defaults{"source"};
    require(defaults.visible && defaults.opacity == .25 && defaults.offset_m.x == 0 &&
        defaults.offset_m.y == 0, "default settings are wrong");
    const FloorReferenceSettings settings{"source", false, .75, {-1e6, 1e6}};
    const auto value = settings.to_json();
    require(FloorReferenceSettings::from_json(value) == settings, "roundtrip must be exact");
    const auto floor = entity("floor", "floor", {{"tracing_reference", value}});
    require(FloorReferenceSettings::from_entity(floor) == settings, "entity codec must roundtrip");
    require(!FloorReferenceSettings::from_entity(entity("floor", "floor")), "metadata is optional");
    rejects([&] { (void)FloorReferenceSettings::from_entity(entity("layer", "layer")); });
    rejects([&] { (void)FloorReferenceSettings::from_entity(entity("floor", "floor", nullptr)); });
    const auto invalid = [&](json candidate) {
        rejects([&] { (void)FloorReferenceSettings::from_json(candidate); });
    };
    auto candidate = value; candidate["version"] = 2; invalid(candidate);
    candidate = value; candidate["version"] = 1.0; invalid(candidate);
    candidate = value; candidate["extra"] = 0; invalid(candidate);
    candidate = value; candidate.erase("visible"); invalid(candidate);
    candidate = value; candidate["source_floor_id"] = ""; invalid(candidate);
    candidate = value; candidate["source_floor_id"] = 3; invalid(candidate);
    candidate = value; candidate["visible"] = 1; invalid(candidate);
    candidate = value; candidate["opacity"] = "0.25"; invalid(candidate);
    for (const auto opacity : {0.049, 0.751, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()}) {
        candidate = value; candidate["opacity"] = opacity; invalid(candidate);
        auto bad = defaults; bad.opacity = opacity; rejects([&] { (void)bad.to_json(); });
    }
    candidate = value; candidate["offset_m"] = {0, 0}; invalid(candidate);
    candidate = value; candidate["offset_m"]["z"] = 0; invalid(candidate);
    candidate = value; candidate["offset_m"]["x"] = false; invalid(candidate);
    for (const char* axis : {"x", "y"}) {
        for (const auto coordinate : {-1000000.1, 1000000.1,
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            candidate = value; candidate["offset_m"][axis] = coordinate; invalid(candidate);
        }
    }
    require(FloorReferenceSettings::from_json(FloorReferenceSettings{"source", true, .05}.to_json()).opacity == .05,
        "lower opacity boundary is valid");
}
void test_resolution() {
    const auto document = sketch::Document::create(fixture());
    const auto snapshot = document.snapshot();
    const auto retained = snapshot.entities();
    require(sketch::resolve_floor_reference(snapshot, "destination") == FloorReferenceSettings{"source"},
        "same-building floors without layers must resolve");
    require(!sketch::resolve_floor_reference(snapshot, "source"), "absent metadata must remain absent");
    require(snapshot.entities() == retained && document.snapshot().entities() == retained,
        "resolution must preserve original and retained snapshots");
    rejects([&] { (void)sketch::resolve_floor_reference(snapshot, "missing"); });
    rejects([&] { (void)sketch::resolve_floor_reference(snapshot, "building"); });
    const auto invalid_fixture = [](std::vector<Entity> entities) {
        try {
            const auto bad_document = sketch::Document::create(std::move(entities));
            rejects([&] { (void)sketch::resolve_floor_reference(bad_document.snapshot(), "destination"); });
        } catch (const sketch::DocumentError& error) {
            // A missing structural parent is rejected at document admission,
            // before a reference resolver can see that impossible snapshot.
            require(error.code() == sketch::DocumentErrorCode::dangling_reference,
                    "invalid parent must be rejected as a dangling reference");
        }
    };
    auto entities = fixture(); entities[3].properties["tracing_reference"]["source_floor_id"] = "destination";
    invalid_fixture(entities);
    entities = fixture(); entities.erase(entities.begin() + 2); invalid_fixture(entities);
    entities = fixture(); entities[3].properties["tracing_reference"]["source_floor_id"] = "building";
    invalid_fixture(entities);
    entities = fixture(); entities[2].properties["building_id"] = "missing"; invalid_fixture(entities);
    entities = fixture(); entities[3].properties["building_id"] = "missing"; invalid_fixture(entities);
    entities = fixture(); entities[1].properties["property_id"] = "missing"; invalid_fixture(entities);
    entities = fixture(); entities.push_back(entity("other", "building", {{"property_id", "property"}}));
    entities[2].properties["building_id"] = "other"; invalid_fixture(entities);
    entities = fixture(); entities.push_back(entity("other-property", "property"));
    entities[2].properties["property_id"] = "other-property"; invalid_fixture(entities);
    auto updated = fixture(); updated[2].properties["property_id"] = "property";
    updated[3].properties["property_id"] = "property";
    const auto same_context = sketch::Document::create(std::move(updated));
    require(sketch::resolve_floor_reference(same_context.snapshot(), "destination").has_value(),
        "matching redundant context must remain valid");
    auto live = sketch::Document::create(fixture());
    const auto original = live.snapshot();
    live.apply(sketch::ApplyEntityChanges{live.revision(), {sketch::EntityChange::erase("source")}, {}, "delete source"});
    rejects([&] { (void)sketch::resolve_floor_reference(live.snapshot(), "destination"); });
    require(sketch::resolve_floor_reference(original, "destination").has_value(),
        "retained snapshot must resolve its original source after deletion");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_codec(); test_resolution();
        std::cout << "Floor reference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
