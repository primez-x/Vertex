#include "sketch/appraisal_document.hpp"
#include "sketch/document.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using sketch::BoundaryConstructionKind;
using sketch::ConstructionReceipt;
using sketch::ConstructionTopologyEdge;
using sketch::Document;
using sketch::DocumentError;
using sketch::Entity;
using sketch::EntityChange;
using sketch::MeasurementLinework;
using sketch::ProjectStore;
using sketch::Vec2;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

Entity entity(std::string id, std::string type, Json properties = Json::object(),
              bool required = false, Json extensions = Json::object()) {
    return {std::move(id), std::move(type), std::move(properties), required,
            std::move(extensions)};
}

ConstructionTopologyEdge line_edge(std::string segment_id, std::string start_vertex_id,
                                    std::string end_vertex_id, Vec2 start, Vec2 end) {
    ConstructionReceipt receipt;
    receipt.segment_id = segment_id;
    receipt.kind = BoundaryConstructionKind::line_to_point;
    receipt.start = start;
    receipt.chord_end = end;
    return {std::move(segment_id), std::move(start_vertex_id), std::move(end_vertex_id),
            std::move(receipt)};
}

MeasurementLinework one_edge_model(std::string stroke_id, std::string segment_id,
                                   std::string start_vertex_id, std::string end_vertex_id,
                                   Vec2 start, Vec2 end) {
    MeasurementLinework model;
    model.stroke_id = std::move(stroke_id);
    model.anchor = start;
    model.edges.push_back(line_edge(std::move(segment_id), std::move(start_vertex_id),
                                    std::move(end_vertex_id), start, end));
    return model;
}

struct ContextIds {
    std::string property_id = "property-1";
    std::string building_id = "building-1";
    std::string floor_id = "floor-1";
    std::string layer_id = "layer-1";
};

Entity linework_entity(const MeasurementLinework& model, ContextIds context = {},
                       bool required = true, Json entity_extensions = Json::object()) {
    return entity(model.stroke_id, "measurement_linework",
                  {{"property_id", std::move(context.property_id)},
                   {"building_id", std::move(context.building_id)},
                   {"floor_id", std::move(context.floor_id)},
                   {"layer_id", std::move(context.layer_id)},
                   {"model", sketch::encode_measurement_linework_model(model)}},
                  required, std::move(entity_extensions));
}

std::vector<Entity> context_entities() {
    return {
        entity("property-1", "property", {{"name", "Primary property"}}),
        entity("building-1", "building", {{"property_id", "property-1"}}),
        entity("floor-1", "floor", {{"building_id", "building-1"}}),
        entity("layer-1", "layer", {{"floor_id", "floor-1"}}),
        entity("property-2", "property", {{"name", "Other property"}}),
        entity("building-2", "building", {{"property_id", "property-2"}}),
        entity("floor-2", "floor", {{"building_id", "building-2"}}),
        entity("layer-2", "layer", {{"floor_id", "floor-2"}}),
    };
}

std::vector<Entity> context_with(Entity additional) {
    auto result = context_entities();
    result.push_back(std::move(additional));
    return result;
}

bool document_error_from(const auto& operation) {
    try {
        operation();
    } catch (const DocumentError&) {
        return true;
    }
    return false;
}

void require_document_error(const auto& operation, std::string_view message) {
    require(document_error_from(operation), message);
}

void require_rejected_atomically(const Entity& invalid, std::string_view message) {
    require_document_error([&] { (void)Document::create(context_with(invalid)); },
                           "Document::create must reject malformed measurement linework");

    auto document = Document::create(context_entities());
    const auto before = document.snapshot();
    require_document_error(
        [&] {
            (void)document.apply(sketch::ApplyEntityChanges{
                .expected_revision = before.revision(),
                .entity_changes = {EntityChange::upsert(invalid)},
                .message = "invalid linework must be atomic",
            });
        },
        message);
    const auto after = document.snapshot();
    require(after.revision() == before.revision() &&
                after.entities() == before.entities() &&
                after.history().size() == before.history().size() &&
                document.is_editable() == before.is_editable(),
            "rejected linework must not change entities, revision, editability, or history");
}

Json appraisal_policy() {
    return {{"policy_kind", "residential_declared"},
            {"version", 1},
            {"property_kind", "detached_single_family"},
            {"measurement_basis", "exterior"}};
}

Json appraisal_facts() {
    return {{"finish", "finished"},
            {"access", "direct_interior"},
            {"ceiling_eligibility", "standard"},
            {"area_use", "dwelling"},
            {"boundary_role", "measured_area"}};
}

Json square(double x, double y, double side) {
    return Json::array({
        {{"start", {x, y}}, {"end", {x + side, y}}, {"sweep_radians", 0.0}},
        {{"start", {x + side, y}}, {"end", {x + side, y + side}}, {"sweep_radians", 0.0}},
        {{"start", {x + side, y + side}}, {"end", {x, y + side}}, {"sweep_radians", 0.0}},
        {{"start", {x, y + side}}, {"end", {x, y}}, {"sweep_radians", 0.0}},
    });
}

std::vector<Entity> appraisal_context_with_area() {
    auto result = context_entities();
    result.front().properties["calculation_workflow"] = "appraisal";
    result.front().properties["appraisal_policy"] = appraisal_policy();
    result[2].properties["appraisal_facts"] = {{"grade", "above"}};
    result.push_back(entity("area-1", "measurement_boundary",
        {{"property_id", "property-1"}, {"building_id", "building-1"},
         {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"boundary", square(0.0, 0.0, 3.048)}, {"calculation_scope", "building"},
         {"appraisal_facts", appraisal_facts()}}));
    return result;
}

void known_entity_type() {
    require(sketch::is_known_entity_type("measurement_linework"),
            "measurement_linework must be a registered entity type");
}

void admission_and_atomic_rejection() {
    const auto ordinary = one_edge_model("stroke-valid", "segment-valid", "vertex-start",
                                         "vertex-end", {0.125, -4.75},
                                         {3.141592653589793, 2.718281828459045});

    auto invalid_receipt = linework_entity(ordinary);
    invalid_receipt.properties["model"]["segments"][0]["receipt"]["segment_id"] =
        "different-segment";
    require_rejected_atomically(invalid_receipt,
        "a receipt whose segment identity differs from its topology edge must reject on apply");

    auto joined = one_edge_model("stroke-joined", "segment-first", "join-a", "join-b",
                                 {1.0, 2.0}, {4.0, 5.0});
    joined.edges.push_back(line_edge("segment-second", "join-b", "join-c",
                                     {4.0, 5.0}, {7.0, 8.0}));
    auto invalid_join = linework_entity(joined);
    invalid_join.properties["model"]["segments"][1]["receipt"]["start"][0] = 4.25;
    require_rejected_atomically(invalid_join,
        "joined receipt starts must match the prior endpoint and coordinate exactly");

    const auto wrong_identity_model = one_edge_model("stroke-model-id", "segment-identity",
        "identity-a", "identity-b", {0.0, 0.0}, {1.0, 1.0});
    auto wrong_identity = linework_entity(wrong_identity_model);
    wrong_identity.id = "stroke-entity-id";
    require_rejected_atomically(wrong_identity,
        "linework model stroke_id must equal the owning entity ID");

    require_rejected_atomically(linework_entity(ordinary, {}, false),
        "measurement linework must be required data");

    auto zero_schema = linework_entity(ordinary);
    zero_schema.properties["model"]["version"] = 0;
    require_rejected_atomically(zero_schema,
        "zero measurement linework schema versions must be rejected");

    auto malformed_replay = linework_entity(ordinary);
    malformed_replay.properties["model"]["replay_version"] = "1";
    require_rejected_atomically(malformed_replay,
        "malformed measurement linework replay versions must be rejected");
}

void context_consistency() {
    const auto model = one_edge_model("stroke-context", "segment-context", "context-a",
                                      "context-b", {0.0, 0.0}, {1.0, 0.0});
    const std::vector<ContextIds> invalid_contexts{
        {"property-2", "building-1", "floor-1", "layer-1"},
        {"property-1", "building-2", "floor-1", "layer-1"},
        {"property-1", "building-1", "floor-2", "layer-1"},
        {"property-1", "building-1", "floor-1", "layer-2"},
    };
    for (const auto& context : invalid_contexts) {
        require_rejected_atomically(linework_entity(model, context),
            "all four measurement linework context IDs must resolve along one consistent hierarchy");
    }

    const auto valid_document = Document::create(context_with(linework_entity(model)));
    const auto snapshot = valid_document.snapshot();
    const auto organization = sketch::organize_project(snapshot);
    require(organization.drawing_context(model.stroke_id) ==
                sketch::DrawingContext{"property-1", "building-1", "floor-1", "layer-1"},
            "project organization must resolve a linework entity's complete drawing context");
    require(organization.nodes.at(model.stroke_id).parent_id == "layer-1",
            "linework must appear under its owning drawing layer");
    auto conflicting_parents = context_with(linework_entity(model));
    const auto layer = std::find_if(conflicting_parents.begin(), conflicting_parents.end(),
        [](const auto& value) { return value.id == "layer-1"; });
    layer->properties["building_id"] = "building-2";
    require_document_error([&] { (void)Document::create(conflicting_parents); },
        "linework must reject contradictory redundant references on its parent layer");
    auto editable = Document::create(context_with(linework_entity(model)));
    const auto before = editable.snapshot();
    require_document_error([&] {
        (void)editable.apply(sketch::ApplyEntityChanges{before.revision(),
            {EntityChange::upsert(*layer)}, {}, "Contradictory parent context"});
    }, "a parent edit must not strand existing linework in an unresolved drawing context");
    const auto after = editable.snapshot();
    require(after.entities() == before.entities() && after.revision() == before.revision() &&
            after.history().size() == before.history().size(),
            "rejected parent context edits must preserve geometry and history atomically");
}

void namespace_isolation_and_exact_revisit() {
    const auto collided_entity_id = one_edge_model("stroke-entity-collision", "property-1",
        "collision-a", "collision-b", {0.0, 0.0}, {1.0, 0.0});
    require_rejected_atomically(linework_entity(collided_entity_id),
        "topology IDs must not collide with document entity IDs");

    auto document = Document::create(context_entities());
    const auto first = one_edge_model("stroke-namespace-a", "segment-shared", "vertex-shared",
                                      "vertex-a-end", {0.0, 0.0}, {1.0, 0.0});
    (void)document.apply(sketch::ApplyEntityChanges{
        .expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(linework_entity(first))},
        .message = "first independent stroke",
    });
    const auto before_collision = document.snapshot();
    const auto same_segment = one_edge_model("stroke-namespace-b", "segment-shared", "vertex-b-start",
                                             "vertex-b-end", {4.0, 4.0}, {5.0, 4.0});
    require_document_error([&] {
        (void)document.apply(sketch::ApplyEntityChanges{
            .expected_revision = before_collision.revision(),
            .entity_changes = {EntityChange::upsert(linework_entity(same_segment))},
            .message = "colliding segment ID",
        });
    }, "topology IDs must be unique between independent strokes");
    const auto after_collision = document.snapshot();
    require(after_collision.revision() == before_collision.revision() &&
                after_collision.entities() == before_collision.entities() &&
                after_collision.history().size() == before_collision.history().size(),
            "a second stroke with a colliding segment ID must reject atomically");

    auto revisit = one_edge_model("stroke-revisit", "revisit-edge-0", "revisit-v0", "revisit-v1",
                                  {0.0, 0.0}, {1.0, 0.0});
    revisit.edges.push_back(line_edge("revisit-edge-1", "revisit-v1", "revisit-v0",
                                      {1.0, 0.0}, {0.0, 0.0}));
    revisit.edges.push_back(line_edge("revisit-edge-2", "revisit-v0", "revisit-v2",
                                      {0.0, 0.0}, {0.0, 2.0}));
    const auto replayed = sketch::replay_measurement_linework(revisit);
    require(replayed.edges.size() == 3 && !replayed.closed,
            "an open stroke may revisit an existing vertex identity at the same exact point");
    const auto revisit_document = Document::create(context_with(linework_entity(revisit)));
    require(revisit_document.snapshot().entities().contains("stroke-revisit"),
            "valid same-owner vertex revisits must be admitted by Document");
}

void unsupported_versions_are_opaque_read_only() {
    const auto supported = one_edge_model("stroke-future", "segment-future", "future-a",
                                          "future-b", {0.125, -2.5}, {3.0, 4.0});
    const auto encoded = sketch::encode_measurement_linework_model(supported);
    const std::vector<std::pair<std::string, std::uint64_t>> future_versions{
        {"version", 2}, {"replay_version", 2},
    };
    const auto directory = std::filesystem::absolute(std::filesystem::temp_directory_path() /
        ("vertex-measurement-linework-" + sketch::make_stable_id()));
    require(!std::filesystem::exists(directory) && std::filesystem::create_directory(directory),
            "the linework persistence case must create its own unique temporary directory");
    struct DirectoryCleanup {
        std::filesystem::path path;
        ~DirectoryCleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{directory};

    for (const auto& [version_key, version_value] : future_versions) {
        auto future_model = encoded;
        future_model[version_key] = version_value;
        const auto decoded = sketch::decode_measurement_linework_model(future_model);
        require(!decoded.supported() && decoded.original_model &&
                    *decoded.original_model == future_model,
                "unsupported positive schema or replay versions must decode as exact opaque JSON");

        auto future_entity = linework_entity(supported);
        future_entity.properties["model"] = future_model;
        auto document = Document::create(context_with(future_entity));
        const auto opaque_before_save = document.snapshot().entities().at("stroke-future");
        require(!document.is_editable() && opaque_before_save == future_entity,
                "unsupported required linework must preserve its entity and make the document read-only");

        const auto file = directory / (version_key + ".bldproj");
        (void)ProjectStore::save(file, document.snapshot());
        auto loaded = ProjectStore::load(file);
        const auto reopened_entity = loaded.document.snapshot().entities().at("stroke-future");
        require(!loaded.document.is_editable() && reopened_entity == future_entity &&
                    reopened_entity.properties.at("model") == future_model,
                "opaque future linework and its read-only status must survive save/reopen");

        const auto before_rejected_edit = loaded.document.snapshot();
        require_document_error([&] {
            (void)loaded.document.apply(sketch::ApplyEntityChanges{
                .expected_revision = before_rejected_edit.revision(),
                .entity_changes = {EntityChange::upsert(entity("label-after-future", "label",
                    {{"text", "must not edit"}}))},
                .message = "required future linework is read-only",
            });
        }, "a required future linework model must block further document mutation");
        const auto after_rejected_edit = loaded.document.snapshot();
        require(after_rejected_edit.revision() == before_rejected_edit.revision() &&
                    after_rejected_edit.entities() == before_rejected_edit.entities() &&
                    after_rejected_edit.history().size() == before_rejected_edit.history().size(),
                "read-only rejection must preserve the exact entity state and history");
    }
}

void independent_strokes_roundtrip_history_and_inputs() {
    auto document = Document::create(context_entities());
    const Vec2 first_start{0.12345678901234566, -12.3456789012345};
    const Vec2 first_end{3.141592653589793, 2.718281828459045};
    auto first_model = one_edge_model("stroke-a", "segment-a", "vertex-a0", "vertex-a1",
                                      first_start, first_end);
    first_model.extensions = {{"vendor", {{"preserve", true}, {"sequence", {1, 2, 3}}}}};
    const auto first_entity = linework_entity(first_model, {}, true,
                                               {{"source", "capture-a"}, {"future", {"keep", 7}}});
    (void)document.apply(sketch::ApplyEntityChanges{
        .expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(first_entity)},
        .message = "first stroke",
    });

    const Vec2 second_start{-99.87654321098765, 0.000000000000031};
    const Vec2 second_end{-97.125, 0.125};
    auto second_model = one_edge_model("stroke-b", "segment-b", "vertex-b0", "vertex-b1",
                                       second_start, second_end);
    second_model.extensions = {{"vendor", {{"preserve", "independent"}, {"count", 2}}}};
    const auto second_entity = linework_entity(second_model, {}, true,
                                                {{"source", "capture-b"}, {"unit_hint", "metres"}});
    (void)document.apply(sketch::ApplyEntityChanges{
        .expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(second_entity)},
        .message = "second independent stroke",
    });

    const auto saved_snapshot = document.snapshot();
    require(saved_snapshot.entities().at("stroke-a") == first_entity &&
                saved_snapshot.entities().at("stroke-b") == second_entity,
            "independent strokes must retain exact coordinates, receipt inputs, and extensions");

    const auto directory = std::filesystem::absolute(std::filesystem::temp_directory_path() /
        ("vertex-measurement-linework-history-" + sketch::make_stable_id()));
    require(!std::filesystem::exists(directory) && std::filesystem::create_directory(directory),
            "the history case must create its own unique temporary directory");
    struct DirectoryCleanup {
        std::filesystem::path path;
        ~DirectoryCleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{directory};
    const auto file = directory / "linework-history.bldproj";
    (void)ProjectStore::save(file, saved_snapshot);
    auto loaded = ProjectStore::load(file);
    const auto reopened = loaded.document.snapshot();
    require(!loaded.document.dirty() && reopened.entities() == saved_snapshot.entities() &&
                reopened.history().size() == saved_snapshot.history().size() &&
                loaded.document.can_undo(),
            "save/reopen must preserve independent linework entities and undo history");

    for (const auto& expected : {first_entity, second_entity}) {
        const auto& actual = reopened.entities().at(expected.id);
        const auto decoded = sketch::decode_measurement_linework_model(actual.properties.at("model"));
        require(decoded.supported() &&
                    sketch::encode_measurement_linework_model(*decoded.model) ==
                        expected.properties.at("model"),
                "stored stroke receipts and original coordinate inputs must decode and encode exactly");
    }
    const auto first_decoded = sketch::decode_measurement_linework_model(
        reopened.entities().at("stroke-a").properties.at("model"));
    require(first_decoded.model->edges.front().receipt.start.x == first_start.x &&
                first_decoded.model->edges.front().receipt.start.y == first_start.y &&
                first_decoded.model->edges.front().receipt.chord_end &&
                first_decoded.model->edges.front().receipt.chord_end->x == first_end.x &&
                first_decoded.model->edges.front().receipt.chord_end->y == first_end.y &&
                first_decoded.model->extensions == first_model.extensions &&
                reopened.entities().at("stroke-a").extensions == first_entity.extensions,
            "receipt coordinates plus model and entity extensions must remain exact across persistence");

    loaded.document.undo(loaded.document.revision());
    auto after_undo = loaded.document.snapshot();
    require(after_undo.entities().contains("stroke-a") &&
                !after_undo.entities().contains("stroke-b") && loaded.document.can_redo(),
            "undo after reopen must remove only the second stroke");
    loaded.document.redo(loaded.document.revision());
    const auto after_redo = loaded.document.snapshot();
    require(after_redo.entities().at("stroke-a") == first_entity &&
                after_redo.entities().at("stroke-b") == second_entity,
            "redo after reopen must restore both exact independent strokes");
}

void linework_never_contributes_appraisal_area() {
    auto entities = appraisal_context_with_area();
    const auto baseline = Document::create(entities);
    const auto baseline_report = sketch::build_appraisal_document_report(
        baseline.snapshot(), "property-1", sketch::AreaUnit::square_metre);
    require(baseline_report.qualified && baseline_report.calculation,
            "the measured-area fixture must produce a qualified baseline appraisal");

    const auto linework = one_edge_model("stroke-appraisal", "segment-appraisal",
        "appraisal-a", "appraisal-b", {0.0, 0.0}, {3.048, 3.048});
    entities.push_back(linework_entity(linework));
    const auto with_linework = Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(
        with_linework.snapshot(), "property-1", sketch::AreaUnit::square_metre);
    require(report.qualified && report.calculation,
            "adding an open analytical line must not invalidate a measured-area report");
    require_near(report.calculation->property.gla().total.square_metres,
         baseline_report.calculation->property.gla().total.square_metres, 1e-10,
         "measurement linework must make no GLA contribution");
    require_near(report.calculation->property.gla().total.square_metres, 9.290304, 1e-8,
         "the existing 100 square foot measured area must remain the entire GLA total");
}

using Test = void (*)();

const std::vector<std::pair<std::string_view, Test>>& cases() {
    static const std::vector<std::pair<std::string_view, Test>> all{
        {"known", known_entity_type},
        {"admission", admission_and_atomic_rejection},
        {"context", context_consistency},
        {"namespace", namespace_isolation_and_exact_revisit},
        {"future", unsupported_versions_are_opaque_read_only},
        {"history", independent_strokes_roundtrip_history_and_inputs},
        {"appraisal", linework_never_contributes_appraisal_area},
    };
    return all;
}

}  // namespace

int main(int argc, char** argv) {
    sketch::runtime::configure_noninteractive_errors();
    try {
        if (argc > 2) throw std::runtime_error("expected at most one named case");
        if (argc == 2) {
            const std::string_view requested(argv[1]);
            const auto found = std::find_if(cases().begin(), cases().end(), [&](const auto& item) {
                return item.first == requested;
            });
            if (found == cases().end()) throw std::runtime_error("unknown measurement linework case");
            found->second();
            return 0;
        }
        for (const auto& [name, run] : cases()) {
            (void)name;
            run();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "measurement_linework_document_tests: " << error.what() << '\n';
        return 1;
    }
}
