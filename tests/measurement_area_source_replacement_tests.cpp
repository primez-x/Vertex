#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_authoring_session.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_visibility.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
void require_near(double actual, double expected, std::string_view message) {
    require(std::abs(actual - expected) < 1e-10, message);
}
template<class F> void rejects(F&& action, std::string_view message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
Entity stroke(const std::string& id, const std::vector<Vec2>& points, bool closed = false) {
    MeasurementLinework model; model.stroke_id = id; model.anchor = points.front(); model.closed = closed;
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt; receipt.segment_id = id + ":e" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = points[i - 1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, id + ":v" + std::to_string(i - 1),
            closed && i + 1 == points.size() ? id + ":v0" : id + ":v" + std::to_string(i), receipt});
    }
    return {id, "measurement_linework", {{"property_id", "p"}, {"building_id", "b"},
        {"floor_id", "f"}, {"layer_id", "l"}, {"model", encode_measurement_linework_model(model)}}, true};
}
MeasurementAreaGraph graph(const Entities& values) {
    std::vector<MeasurementGraphSource> sources;
    for (const auto& [id, entity] : values) if (entity.type == "measurement_linework" && entity.properties.at("layer_id") == "l")
        for (const auto& edge : replay_measurement_linework(*decode_measurement_linework_model(entity.properties.at("model")).model).edges)
            sources.push_back({id, edge.segment_id, edge.segment});
    return build_measurement_area_graph(sources);
}
Json lineage(const MeasurementAreaGraph& source, const DerivedMeasurementFace& face) {
    auto result = Json::array();
    for (const auto& edge : face.edge_uses) {
        auto values = Json::array();
        for (const auto& use : source.edges.at(edge.edge_index).source_uses)
            values.push_back({{"owner_id", use.owner_id}, {"segment_id", use.segment_id},
                {"parameter_start", use.parameter_start}, {"parameter_end", use.parameter_end},
                {"reversed", use.reversed != edge.reversed}});
        result.push_back(std::move(values));
    }
    return result;
}
const DerivedMeasurementFace& face_at(const MeasurementAreaGraph& source, double x, double y) {
    for (const auto& face : source.faces) {
        double min_x = 100, max_x = -100, min_y = 100, max_y = -100;
        for (const auto& edge : face.boundary) {
            min_x = std::min(min_x, edge.start.x); max_x = std::max(max_x, edge.start.x);
            min_y = std::min(min_y, edge.start.y); max_y = std::max(max_y, edge.start.y);
        }
        if (x > min_x && x < max_x && y > min_y && y < max_y) return face;
    }
    throw std::runtime_error("Fixture source face missing");
}
Entity area(const std::string& id, const MeasurementAreaGraph& source, const DerivedMeasurementFace& face,
    const std::string& classification) {
    auto segments = Json::array();
    for (const auto& edge : face.boundary) segments.push_back({{"start", {edge.start.x, edge.start.y}},
        {"end", {edge.end.x, edge.end.y}}, {"sweep_radians", edge.sweep_radians}});
    Entity result{id, "measurement_boundary", {{"property_id", "p"}, {"building_id", "b"},
        {"floor_id", "f"}, {"layer_id", "l"}, {"classification", classification}, {"segments", segments},
        {"name", id + " original"}, {"factor_numerator", 2}, {"factor_denominator", 2}, {"factor_expression", "2/2"},
        {"appraisal_facts", {{"finish", "finished"}, {"access", "direct_interior"},
            {"ceiling_eligibility", "standard"}, {"area_use", classification == "garage" ? "garage" : "dwelling"},
            {"boundary_role", "measured_area"}}}, {"vendor_metadata", {{"preserve", 17}}}}, false,
        {{"measurement_linework_sources", lineage(source, face)}, {"vendor_style", {{"color", "#123456"}}}}};
    return upgrade_legacy_boundary_entity(result);
}
Entities fixture() {
    Entities values;
    for (auto entity : std::vector<Entity>{
        {"p", "property", {{"calculation_workflow", "appraisal"}, {"appraisal_policy", {
            {"policy_kind", "residential_declared"}, {"version", 1}, {"property_kind", "detached_single_family"},
            {"measurement_basis", "exterior"}}}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}, {"appraisal_facts", {{"grade", "above"}}}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false},
        stroke("outline", {{0,0}, {4,0}, {4,4}, {0,4}, {0,0}}, true),
        stroke("separator", {{2,-1}, {2,5}})}) values.emplace(entity.id, std::move(entity));
    const auto source = graph(values);
    auto left = area("left", source, face_at(source, 1, 1), "living");
    auto right = area("right", source, face_at(source, 3, 1), "garage");
    values.emplace(left.id, std::move(left)); values.emplace(right.id, std::move(right));
    return values;
}
Document document(const Entities& values) {
    std::vector<Entity> result; for (const auto& [id, entity] : values) result.push_back(entity);
    return Document::create(std::move(result));
}
BoundaryGeometryEdit replacement(const Entities& values, const std::string& id = "left") {
    const auto source = graph(values); const auto& face = face_at(source, id == "right" ? 3 : 1, 1);
    BoundaryGeometryEdit edit; edit.kind = BoundaryGeometryEditKind::redefine_boundary;
    edit.boundary_id = id; edit.target_id = id; edit.fresh_topology = true;
    IdentifiedBoundary selected{id, "measurement_boundary", {}};
    for (std::size_t i = 0; i < face.boundary.size(); ++i)
        selected.segments.push_back({id + ":replacement:e" + std::to_string(i),
            id + ":replacement:v" + std::to_string(i), id + ":replacement:v" + std::to_string((i + 1) % face.boundary.size()), face.boundary[i]});
    edit.replacement_segments = encode_identified_boundary_entity(selected).properties.at("segments");
    edit.replacement_linework_sources = lineage(source, face);
    return edit;
}
void replacement_restores_same_owner_and_history() {
    auto original = fixture(); auto doc = document(original);
    const auto report = build_appraisal_document_report(doc.snapshot(), "p");
    require(report.qualified && report.calculation, "Original paired areas must be appraisal qualified");
    require_near(report.calculation->property.gla().total.square_metres, 8, "Original living GLA is eight square metres");
    doc.apply(ApplyEntityChanges{doc.revision(), {EntityChange::upsert(stroke("horizontal", {{-1,2}, {2,2}}))}, {}, "Split left source face"});
    const auto stale = doc.snapshot();
    require(!measurement_linework_source_checks(stale.entities()).at("left").current, "Split face must stale its retained owner");
    const auto edit = replacement(stale.entities());
    const auto preview = Document::preview_command(stale, EditBoundaryGeometry{stale.revision(), edit});
    require(doc.snapshot().entities() == stale.entities(), "Face replacement preview is immutable");
    doc.apply(EditBoundaryGeometry{stale.revision(), edit});
    const auto repaired = doc.snapshot();
    require(repaired.entities() == preview.entities() && repaired.revision() == stale.revision() + 1,
        "Face replacement applies as one exact preview revision");
    const auto checks = measurement_linework_source_checks(repaired.entities());
    require(checks.at("left").current, "Explicit replacement restores the selected current source face");
    require(!checks.at("right").current, "Another owner with a newly split shared edge still requires its own explicit review");
    const auto& actual = repaired.entities().at("left");
    for (const auto* key : {"name", "classification", "factor_numerator", "factor_denominator", "factor_expression", "appraisal_facts", "vendor_metadata"})
        require(actual.properties.at(key) == original.at("left").properties.at(key), "Face replacement preserves measured owner facts");
    require(actual.extensions.at("vendor_style") == original.at("left").extensions.at("vendor_style"), "Face replacement preserves arbitrary extensions");
    require(actual.extensions.at("measurement_linework_sources") == *edit.replacement_linework_sources, "Selected complete lineage is retained exactly");
    for (const auto& [id, entity] : stale.entities()) if (id != "left")
        require(repaired.entities().at(id) == entity, "Other owners, excluded garage and sources remain exact");
    require(!build_appraisal_document_report(repaired, "p").qualified,
        "Property totals stay withheld until the other stale source owner is reviewed");
    require(Document::fork(repaired).snapshot().entities() == repaired.entities(), "Typed replacement replays exact history");
    doc.undo(doc.revision()); require(doc.snapshot().entities() == stale.entities(), "One Undo restores stale owner exactly");
    doc.redo(doc.revision()); require(doc.snapshot().entities() == repaired.entities(), "One Redo restores reviewed owner exactly");
    doc.apply(EditBoundaryGeometry{doc.revision(), replacement(doc.snapshot().entities(), "right")});
    const auto qualified = build_appraisal_document_report(doc.snapshot(), "p");
    require(qualified.qualified && qualified.calculation, "Reviewing both affected owners restores property calculations");
    require_near(qualified.calculation->property.gla().total.square_metres, 4, "Selected lower left face contributes four living square metres");
    auto forged = repaired.entities(); forged.at("left").extensions["measurement_linework_sources"][0][0]["owner_id"] = "invented";
    rejects([&] { (void)document(forged); }, "Imported lineage must agree with retained replacement proof");
}
void strict_versions_retain_old_encoding() {
    auto values = fixture(); values.emplace("horizontal", stroke("horizontal", {{-1,2}, {2,2}}));
    auto edit = replacement(values);
    const auto wire = encode_boundary_geometry_edit(edit);
    require(wire.at("version") == 6 && wire.at("replacement_linework_sources") == *edit.replacement_linework_sources &&
        decode_boundary_geometry_edit(wire) == edit, "Explicit lineage uses a strict round-tripping version six envelope");
    auto bad = wire; bad["unknown"] = true;
    rejects([&] { (void)decode_boundary_geometry_edit(bad); }, "Version six unknown fields reject");
    bad = wire; bad.erase("replacement_linework_sources");
    rejects([&] { (void)decode_boundary_geometry_edit(bad); }, "Version six lineage is required");
    for (const auto malformed : {Json(nullptr), Json::object(), Json::array()}) {
        bad = wire; bad["replacement_linework_sources"] = malformed;
        rejects([&] { (void)decode_boundary_geometry_edit(bad); }, "Version six requires nonempty edge lineage");
    }
    bad = wire; bad["replacement_linework_sources"][0][0]["extra"] = true;
    rejects([&] { (void)decode_boundary_geometry_edit(bad); }, "Source-use envelopes are strict");
    edit.replacement_linework_sources.reset(); edit.fresh_topology = false;
    auto expected = encode_boundary_geometry_edit(edit);
    require(expected.at("version") == 1 && expected.size() == 7, "Unadorned redraw retains exact version one envelope");
    for (int version = 1; version <= 5; ++version) {
        auto old = expected; old["version"] = version;
        if (version >= 2) { old["replacement_child_mapping"] = Json::object(); old["replacement_removed_reference_ids"] = Json::array(); }
        if (version == 2) old["replacement_removed_reference_ids"] = {"manual"};
        if (version >= 3) old["replacement_wall_source_ids"] = version == 3 ? Json({"w1", "w2", "w3"}) : Json::array();
        if (version >= 4) old["fresh_topology"] = true;
        if (version == 5) { old["allow_automatic_angle_removal"] = true; old["replacement_removed_reference_ids"] = {"angle"}; }
        require(encode_boundary_geometry_edit(decode_boundary_geometry_edit(old)).dump() == old.dump(), "Every old version retains its exact codec shape");
    }
    BoundaryGeometryEdit coordinate; coordinate.boundary_id = "left"; coordinate.target_id = "vertex";
    coordinate.replacement_linework_sources = wire.at("replacement_linework_sources");
    rejects([&] { validate_boundary_geometry_edit(coordinate); }, "Only redefinition may replace linework lineage");
}
void invalid_assignments_reject_atomically() {
    auto values = fixture(); values.emplace("horizontal", stroke("horizontal", {{-1,2}, {2,2}}));
    const auto edit = replacement(values);
    const auto reject = [&](const Entities& input, const BoundaryGeometryEdit& invalid, std::string_view message) {
        auto doc = document(input); const auto before = doc.snapshot();
        rejects([&] { (void)Document::preview_command(before, EditBoundaryGeometry{before.revision(), invalid}); }, message);
        rejects([&] { doc.apply(EditBoundaryGeometry{before.revision(), invalid}); }, message);
        require(doc.snapshot().entities() == before.entities() && doc.revision() == before.revision(), "Rejected lineage leaves all document state exact");
    };
    auto bad = edit; (*bad.replacement_linework_sources)[0][0]["owner_id"] = "missing";
    reject(values, bad, "Missing candidate source is rejected");
    bad = edit; (*bad.replacement_linework_sources)[0][0]["segment_id"] = "invented";
    reject(values, bad, "Invented source segment is rejected");
    bad = edit; (*bad.replacement_linework_sources)[0][0]["parameter_end"] = .123;
    reject(values, bad, "Invented source interval is rejected");
    bad = edit; (*bad.replacement_linework_sources)[0][0]["reversed"] = !(*bad.replacement_linework_sources)[0][0]["reversed"].get<bool>();
    reject(values, bad, "Wrong traversal reversal is rejected");
    bad = edit; (*bad.replacement_linework_sources)[0].push_back((*bad.replacement_linework_sources)[0][0]);
    reject(values, bad, "Duplicate source use cannot impersonate complete face lineage");
    auto clones = values; clones.emplace("outline-clone", stroke("outline-clone", {{0,0},{4,0},{4,4},{0,4},{0,0}}, true));
    bad = replacement(clones);
    for (auto& edge : *bad.replacement_linework_sources) {
        auto kept = Json::array(); for (const auto& use : edge) if (use.at("owner_id") != "outline") kept.push_back(use);
        edge = std::move(kept);
    }
    reject(clones, bad, "Same face clone cannot omit coincident source mixture");
    auto other = values; other.emplace("other-layer", Entity{"other-layer", "layer", {{"floor_id", "f"}}, false});
    auto foreign = stroke("foreign", {{0,0},{4,0},{4,4},{0,4},{0,0}}, true); foreign.properties["layer_id"] = "other-layer";
    other.emplace(foreign.id, foreign); bad = edit;
    for (auto& edge : *bad.replacement_linework_sources) for (auto& use : edge) if (use.at("owner_id") == "outline") {
        use["owner_id"] = "foreign"; const auto segment = use.at("segment_id").get<std::string>(); use["segment_id"] = "foreign" + segment.substr(7);
    }
    reject(other, bad, "Same geometry in a different context is rejected");
    auto hidden = values;
    const auto phases = ModelPhases::create({"outline", "separator", "horizontal"}, {"outline", "separator"},
        {{"future", "Future", {}, {"horizontal"}}});
    hidden.emplace("phases", Entity{"phases", "model_phases", {{"model", phases.to_json()}}, false});
    reject(hidden, edit, "Phase-hidden proposed source is unavailable");
    bad = edit; bad.replacement_wall_source_ids = {"wall-a", "wall-b", "wall-c"};
    reject(values, bad, "Wall and measured-line replacement authority cannot mix");
    bad = edit; bad.replacement_properties = {{"name", "silently replaced"}};
    reject(values, bad, "Source reassignment cannot overwrite retained facts");
    auto unsourced = values; unsourced.at("left").extensions.erase("measurement_linework_sources");
    reject(unsourced, edit, "Explicit reassignment cannot manufacture a measured-line owner");
    auto ordinary = values; ordinary.at("left").type = "boundary";
    reject(ordinary, edit, "Measured-line face assignment only applies to measurement boundaries");
    auto occupied = values;
    auto already_assigned = edited_boundary_entities(values, edit).at("left");
    already_assigned.id = "assigned"; already_assigned.extensions.erase("boundary_geometry_derivation");
    occupied.emplace(already_assigned.id, already_assigned);
    reject(occupied, edit, "A current source face already assigned to another owner is rejected");
    auto missing_old = values; missing_old.at("left").extensions["measurement_linework_sources"][0][0]["owner_id"] = "deleted-old-source";
    auto repaired = edited_boundary_entities(missing_old, edit);
    require(measurement_linework_source_checks(repaired).at("left").current, "Historical missing owner does not prevent valid explicit candidate reassignment");
    const auto ordinary_hidden = document(values).snapshot();
    ProjectViewFilter filter; filter.hidden_layer_ids.insert("l");
    require(!visible_project_entities(ordinary_hidden, filter).contains("outline"), "Fixture ordinary layer visibility hides source presentation");
    repaired = edited_boundary_entities(ordinary_hidden.entities(), edit);
    require(measurement_linework_source_checks(repaired).at("left").current, "Ordinary visibility does not suppress source semantics");
}
void authored_owner_preserves_construction_origin() {
    auto values = fixture();
    const auto original = values.at("left");
    const auto outline = boundary_geometry(decode_identified_boundary_entity(original));
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor(outline.front().start);
    for (const auto& edge : outline) (void)session.add_line_to(edge.end);
    session.classify_current_chain("living");
    auto chain = session.close_chain(); chain.boundary.id = "left"; chain.boundary.type = "measurement_boundary";
    values.at("left") = encode_identified_boundary_entity(chain.boundary, &original);
    values.at("left").properties["boundary_authoring"] = boundary_construction_envelope(chain, session.options());
    const auto receipt = values.at("left").properties.at("boundary_authoring");
    values.emplace("horizontal", stroke("horizontal", {{-1,2}, {2,2}}));
    auto doc = document(values); const auto edit = replacement(values);
    doc.apply(EditBoundaryGeometry{doc.revision(), edit});
    const auto repaired = doc.snapshot();
    const auto& result = repaired.entities().at("left");
    require(!result.properties.contains("boundary_authoring") &&
        result.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring") == receipt,
        "Authored source replacement archives its exact construction origin");
    require(measurement_linework_source_checks(doc.snapshot().entities()).at("left").current,
        "Authored source replacement proves the current selected face");
    require(Document::fork(doc.snapshot()).snapshot().entities() == doc.snapshot().entities(),
        "Authored source replacement replays exact retained proof");
}
void retained_references_require_reviewed_mapping() {
    auto values = fixture(); values.emplace("horizontal", stroke("horizontal", {{-1,2}, {2,2}}));
    const auto old = decode_identified_boundary_entity(values.at("left"));
    BoundaryDimension manual; manual.id = "manual"; manual.boundary_id = "left"; manual.segment_id = old.segments.front().segment_id;
    values.emplace(manual.id, encode_boundary_dimension_entity(manual));
    auto edit = replacement(values);
    rejects([&] { (void)edited_boundary_entities(values, edit); }, "Source replacement cannot silently retire a manual reference");
    edit.replacement_child_mapping = {{"segments", {{manual.segment_id, edit.replacement_segments[0]["segment_id"]}}}, {"vertices", Json::object()}};
    const auto mapped = edited_boundary_entities(values, edit);
    require(mapped.at("manual").properties.at("target").at("segment_id") == edit.replacement_segments[0]["segment_id"], "Reviewed child mapping remains authoritative");
    edit.replacement_child_mapping = Json::object(); edit.replacement_removed_reference_ids = {"manual"};
    require(!edited_boundary_entities(values, edit).contains("manual"), "Only reviewed explicit removal retires an affected reference");
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try {
        replacement_restores_same_owner_and_history(); strict_versions_retain_old_encoding();
        invalid_assignments_reject_atomically(); retained_references_require_reviewed_mapping();
        authored_owner_preserves_construction_origin();
        std::cout << "measured area source replacement tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
