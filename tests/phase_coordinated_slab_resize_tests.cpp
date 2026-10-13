#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_slab_replacement.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;

// Reserved by resize-design.md. Root must coordinate these reservations.
constexpr int coordinated_phase_version = 46;
constexpr int coordinated_inner_version = 10;
constexpr unsigned coordinated_reader_floor = 260;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require_near(double a, double b, const char* message) {
    require(std::abs(a-b) < 1e-8, message);
}
template<class F> void invalid(F&& action, const char* message, const char* expected_error = nullptr) {
    try { action(); }
    catch (const std::invalid_argument& error) {
        if (expected_error && std::string(error.what())!=expected_error)
            throw std::runtime_error(std::string(message)+": expected ["+expected_error+"] but got ["+error.what()+"]");
        return;
    }
    throw std::runtime_error(message);
}
template<class F> void stale(F&& action, const char* message) {
    try { action(); }
    catch (const DocumentError& error) {
        require(error.code() == DocumentErrorCode::stale_revision, message);
        return;
    }
    throw std::runtime_error(message);
}
Boundary ring(bool curved) {
    return {{{0,0},{4,0},0}, {{4,0},{4,3},0},
            {{4,3},{0,3},curved ? std::numbers::pi : 0}, {{0,3},{0,0},0}};
}
Json raw_ring(const Boundary& boundary) {
    auto rows = Json::array();
    for (const auto& edge : boundary)
        rows.push_back({{"start",{edge.start.x,edge.start.y}},
                        {"end",{edge.end.x,edge.end.y}},
                        {"sweep_radians",edge.sweep_radians}});
    return rows;
}
Entity slab(const std::string& id, bool curved) {
    return {id,"slab",{{"property_id","property"},{"building_id","building"},
        {"floor_id","floor"},{"layer_id","layer"},{"boundary",raw_ring(ring(curved))},
        {"holes",Json::array()},{"thickness_m",0.2},{"elevation_m",1.25},
        {"layers",Json::array({{{"id",id+"-core"},{"thickness_m",0.2}}})}},false};
}
AssemblyType component_type(bool curved) {
    AssemblyProfile profile;
    profile.id = "profile";
    profile.outer = ring(curved);
    profile.elevation_m = 0.4;
    profile.height_m = 0.6;
    profile.topology = AssemblyProfileTopology{
        {"outer",{"e0","e1","e2","e3"},{"v0","v1","v2","v3"}},{}};
    AssemblyType type;
    type.id = "component";
    type.name = "Retained source component";
    type.profiles = {profile};
    return type;
}
AssemblyType parent_type() {
    AssemblyType parent;
    parent.id = "parent";
    parent.name = "Retained nested component";
    parent.properties = {{"opaque","source root property"}};
    AssemblyPart child;
    child.id = "child";
    child.type_id = "component";
    child.transform.translation_m = {0.3,-0.2,0.75};
    child.transform.rotation_radians = 0.17;
    parent.parts = {child};
    return parent;
}
AssemblyInstance hosted(const std::string& id, const std::string& host) {
    AssemblyInstance row;
    row.id = id;
    row.type_id = "parent";
    row.placement = AssemblyPlacement{host,{0.6,-0.35},0.23,1,false,0.2,1};
    row.property_overrides = {{"opaque","retained root override"}};
    return row;
}
Entity catalog(const std::string& id, bool curved, const std::vector<AssemblyInstance>& rows) {
    const auto model = AssemblyModel::create({}, {component_type(curved),parent_type()}, rows);
    Entity result{id,"assembly_model",{{"model",model.to_json()},{"note","retain catalog context"}},false};
    result.extensions = {{"vendor",{{"literal","unrelated-literal"},{"retain",Json::array({7,"opaque"})}}}};
    return result;
}
Entity dimension(const std::string& id, const std::string& catalog_id,
                 const std::string& instance_id, bool nested, bool profile = false) {
    BoundaryDimension value;
    value.id = id;
    value.boundary_id = catalog_id;
    value.kind = profile ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;
    value.text_position = {7.5,-2.25};
    value.presentation = BoundaryDimensionPresentation{3.2,"#713ba2",true,false,true,0.31};
    AssemblyDimensionTarget target;
    target.component = {catalog_id,instance_id,std::nullopt,nested ? std::vector<std::string>{"child"} :
                                                                  std::vector<std::string>{}};
    target.axis = AssemblyDimensionAxis::width;
    if (profile) target.profile = AssemblyProfileDimensionTarget{
        "profile","outer",AssemblyProfileDimensionMeasurement::edge_length,
        std::string{"e2"},std::nullopt,std::nullopt};
    value.assembly_target = target;
    auto result = encode_boundary_dimension_entity(value);
    result.extensions = {{"vendor",{{"literal","child"},{"retain",Json::array({7,"opaque"})}}}};
    return result;
}
Document fixture(bool curved, bool baseline = false, bool with_dimensions = true) {
    std::vector<Entity> values{
        {"property","property",{{"site_frame",{{"version",1},{"origin_m",{12.0,-9.0,3.0}},
            {"rotation_radians",0.21},{"vertical_datum",{{"identifier","local"},{"height_at_origin_m",0.0}}}}}},false},
        {"building","building",{{"property_id","property"},{"site_placement",{{"version",1},
            {"translation_m",{2.0,-1.0,0.5}},{"rotation_radians",-0.14}}}},false},
        {"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},
        slab("slab",curved)
    };
    auto column = encode_building_entity(RectangularColumn{
        .id="column",.base_center={9,2,0},.width=0.4,.depth=0.5,.height=3,.rotation_radians=0.1});
    for (const auto* field : {"property_id","building_id","floor_id","layer_id"})
        column.properties[field] = values.back().properties.at(field);
    values.push_back(column);
    const auto slab_catalog = baseline ? "slab-catalog" : "shared-catalog";
    const auto column_catalog = baseline ? "column-catalog" : "shared-catalog";
    if (baseline) {
        values.push_back(catalog(slab_catalog,curved,{hosted("slab-root","slab")}));
        values.push_back(catalog(column_catalog,false,{hosted("column-root","column")}));
    } else {
        values.push_back(slab("unit-slab",false));
        values.push_back(catalog(slab_catalog,curved,{
            hosted("slab-root","slab"),hosted("unit-root","unit-slab"),hosted("column-root","column")}));
        if (with_dimensions) values.push_back(dimension("unit-dimension",slab_catalog,"unit-root",true));
    }
    // Parent extents and exact nested topology share one actual hosted root.
    if (with_dimensions) {
        values.push_back(dimension("slab-dimension",slab_catalog,"slab-root",false));
        values.push_back(dimension("nested-dimension",slab_catalog,"slab-root",true));
        values.push_back(dimension("profile-dimension",slab_catalog,"slab-root",true,true));
        values.push_back(dimension("column-dimension",column_catalog,"column-root",true));
    }
    if (baseline) {
        std::vector<std::string> baseline_ids{"slab","slab-catalog"};
        if (with_dimensions) baseline_ids.insert(baseline_ids.end(),{
            "slab-dimension","nested-dimension","profile-dimension"});
        const auto phases = ModelPhases::create(baseline_ids,baseline_ids,
            {RemodelingAlternative{"alternative","Actual selected alternative",{}, {}}},"alternative");
        values.push_back({"phases","model_phases",{{"model",phases.to_json()}},false});
    }
    return Document::create(values);
}
SlabGeometryEditIntent resize(const std::string& id, bool mapped, bool unit = false) {
    SlabGeometryEditIntent edit;
    edit.slab_id = id;
    edit.kind = SlabGeometryEditKind::resize_plan;
    edit.resize = SlabPlanAxisResize{unit ? 1.0 : 1.8,unit ? 1.0 : 0.65,{0.75,-0.25},0.37};
    edit.coordinate_world_hosted_geometry = true;
    edit.preserve_planar_source_geometry = mapped;
    return edit;
}
StructuralObjectEditIntent column_move() {
    StructuralObjectEditIntent edit;
    edit.object_id = "column";
    edit.transform = ArchitecturalGroupTransform{{0,0,0},{1.5,-0.75,0.4},0.28,1.2,false,false};
    return edit;
}
Json empty_body() {
    return {{"version",coordinated_inner_version},{"wall_authoring",nullptr},
        {"roof_replacement",nullptr},{"slab_replacement",nullptr},
        {"structural_replacement",nullptr},{"stair_replacement",nullptr},
        {"ordinary_roof_edits",Json::array()},{"ordinary_slab_geometry",Json::array()},
        {"ordinary_structural_edits",Json::array()},{"ordinary_stair_transforms",Json::array()},
        {"complete_hosted_component_dimensions",true}};
}
Json proof(const DocumentSnapshot& source, const Json& body) {
    ConstraintAuthoringIntent semantic;
    semantic.message = "Compose actual-source slab resize and column transform";
    auto wire = encode_phase_constraint_authoring_intent(make_phase_constraint_authoring_intent(source,semantic));
    wire["version"] = coordinated_phase_version;
    wire["coordinated_replacements"] = body;
    return wire;
}
Json ordinary_proof(const DocumentSnapshot& source, bool curved) {
    auto body = empty_body();
    body["ordinary_slab_geometry"] = Json::array({
        encode_slab_geometry_edit_intent(resize("slab",curved)),
        encode_slab_geometry_edit_intent(resize("unit-slab",false,true))});
    body["ordinary_structural_edits"] = Json::array({encode_structural_object_edit_intent(column_move())});
    return proof(source,body);
}
ApplyBoundaryConstraintChanges command(const Json& encoded) {
    const auto intent = decode_phase_constraint_authoring_intent(encoded);
    ApplyBoundaryConstraintChanges result;
    result.expected_revision = intent.expected_revision;
    result.message = intent.intent.message;
    result.phase_constraint_authoring_completion = true;
    result.phase_constraint_authoring_intent = encoded;
    return result;
}
std::vector<SlabGeometryEditIntent> ordinary_intents(bool curved) {
    return {resize("slab",curved),resize("unit-slab",false,true)};
}
Entities ordinary_oracle(const DocumentSnapshot& before, bool curved) {
    const auto& source = before.entities();
    const auto slab_candidate = replay_slab_geometry_with_hosted_entities(source,ordinary_intents(curved));
    const auto structural_candidate = replay_structural_hosted_component_geometry(source,{column_move()});
    auto expected = source;
    expected.at("slab") = slab_candidate.at("slab");
    expected.at("unit-slab") = slab_candidate.at("unit-slab");
    expected.at("column") = structural_candidate.at("column");
    auto& model = expected.at("shared-catalog").properties.at("model");
    model = merge_disjoint_directionally_resized_hosted_assembly_models(
        source.at("shared-catalog").properties.at("model"),
        slab_candidate.at("shared-catalog").properties.at("model"),
        structural_candidate.at("shared-catalog").properties.at("model"));
    for (const auto& entity : carry_hosted_assembly_directional_resize_dimension_entities(
             source,slab_candidate,ordinary_intents(curved)))
        expected.at(entity.id) = entity;
    const std::vector<std::string> hosts{"column"};
    for (const auto& entity : carry_hosted_assembly_dimension_transform_entities(source,structural_candidate,hosts))
        expected.at(entity.id) = entity;
    return expected;
}
BoundaryDimension decoded_dimension(const Entities& entities, const std::string& id) {
    const auto decoded = decode_boundary_dimension_entity(entities.at(id));
    require(decoded.supported(), "expected supported current dimension");
    return *decoded.dimension;
}
const Json& row(const Entities& entities, const std::string& catalog_id, const std::string& instance_id) {
    for (const auto& value : entities.at(catalog_id).properties.at("model").at("instances"))
        if (value.at("id") == instance_id) return value;
    throw std::runtime_error("expected actual hosted row");
}
void assert_dimension_envelope(const Entities& before, const Entities& after, const std::string& id) {
    const auto a = decoded_dimension(before,id), b = decoded_dimension(after,id);
    // Rotation follows the selected node's frame; the independent family
    // oracle above checks its exact value. Paper styling remains unchanged.
    auto carried_presentation=b.presentation;
    if (a.presentation && carried_presentation)
        carried_presentation->rotation_radians=a.presentation->rotation_radians;
    require(a.assembly_target == b.assembly_target && a.kind == b.kind && a.placement == b.placement &&
        a.presentation == carried_presentation && before.at(id).extensions == after.at(id).extensions,
        "ordinary dimension carry must retain qualified selectors, style, mode and opaque extensions");
}

void ordinary_mapped_and_directional_composition() {
    for (const bool curved : {false,true}) {
        auto document = fixture(curved);
        const auto before = document.snapshot();
        const auto encoded = ordinary_proof(before,curved);
        const auto decoded = decode_phase_constraint_authoring_intent(encoded);
        require(encode_phase_constraint_authoring_intent(decoded).dump() == encoded.dump(),
                "new closed coordinated envelope must canonically roundtrip");
        require(phase_constraint_coordinated_authoring_body(decoded.coordinated_replacements) ==
                    encoded.at("coordinated_replacements"),
                "direct coordinated slab body must not unwrap a historical wrapper");
        const auto expected = ordinary_oracle(before,curved);
        const auto replayed = replay_phase_constraint_authoring(before.entities(),encoded);
        require(replayed == expected, "mixed-family replay must equal independent actual-source family consequences and dimension frames");
        const auto& original_model = before.entities().at("shared-catalog").properties.at("model");
        const auto& final_model = replayed.at("shared-catalog").properties.at("model");
        require(final_model.at("types") == original_model.at("types") &&
                final_model.at("materials") == original_model.at("materials"),
                "directional composition must retain shared catalog definitions and materials");
        require(row(before.entities(),"shared-catalog","slab-root").at("placement") ==
                row(replayed,"shared-catalog","slab-root").at("placement"),
                "directional hosted deformation must retain the actual root pose");
        require(replayed.at("unit-slab") == before.entities().at("unit-slab") &&
                replayed.at("unit-dimension") == before.entities().at("unit-dimension"),
                "authenticated unit member must retain host and dimension exactly");
        require(replayed.at("column") != before.entities().at("column") &&
                replayed.at("column-dimension") != before.entities().at("column-dimension"),
                "the sibling lane must carry an actual column transform and hosted dimension");
        for (const auto* id : {"slab-dimension","nested-dimension","profile-dimension","column-dimension"})
            assert_dimension_envelope(before.entities(),replayed,id);
        require_near(replayed.at("slab").properties.at("thickness_m").get<double>(),0.2,"plan resize must preserve slab Z thickness");
        require_near(replayed.at("slab").properties.at("elevation_m").get<double>(),1.25,"plan resize must preserve slab elevation");
        if (curved) {
            require(replayed.at("slab").properties.at("boundary") == before.entities().at("slab").properties.at("boundary"),
                    "unequal curved resize must retain exact analytical source ring");
            require(replayed.at("slab").properties.contains("planar_affine"),"curved resize must publish its real planar map");
            const auto target = *decoded_dimension(replayed,"profile-dimension").assembly_target;
            const auto original = resolve_assembly_dimension(before.entities(),target);
            const auto final = resolve_assembly_dimension(replayed,target);
            require(final.profile_geometry && final.profile_geometry->affine_edge,
                    "nested curved component dimension must resolve its actual mapped arc");
            require(std::abs(final.span_metres-original.span_metres) > 0.01,
                    "unequal resize must change true curved length rather than merely carrying a text label");
        }
        const auto typed = command(encoded);
        const auto wire = command_to_json(Command{typed});
        require(wire.at("version") == 34 && command_to_json(command_from_json(wire)).dump() == wire.dump(),
                "existing command34 must retain the new phase proof exactly");
        document.apply(typed);
        const auto after = document.snapshot();
        require(after.entities() == replayed && after.revision() == before.revision()+1 &&
                after.history().size() == before.history().size()+1,
                "two actual family changes and dimensions must publish as one history event");
        document.undo(document.revision());
        require(document.snapshot().entities() == before.entities(),"one Undo must restore all source geometry and dimensions");
        require(ProjectStore::required_format_version(document.snapshot()) == coordinated_reader_floor,
                "undone new coordinated authority must retain reader260");
        document.redo(document.revision());
        require(document.snapshot().entities() == after.entities(),"one Redo must restore the exact final candidate once");
    }
}
void historical_and_closed_schema_refusal() {
    auto document = fixture(true);
    const auto source = document.snapshot();
    const auto encoded = ordinary_proof(source,true);
    auto historical_body = encoded.at("coordinated_replacements");
    historical_body.erase("complete_hosted_component_dimensions");
    historical_body["version"] = 4;
    auto historical = encoded;
    historical["version"] = 14;
    historical["coordinated_replacements"] = historical_body;
    invalid([&] { (void)decode_phase_constraint_authoring_intent(historical); },
            "historical inner4/outer14 cannot borrow mapped/directional slab leaves");
    historical["version"] = 31;
    historical["coordinated_replacements"] = {{"version",6},{"authoring",historical_body},
        {"complete_hosted_component_dimensions",true}};
    invalid([&] { (void)decode_phase_constraint_authoring_intent(historical); },
            "historical dimension wrapper cannot borrow mapped/directional slab leaves");
    for (const auto& variant : std::vector<Json>{
        Json{{"unknown",true}}, Json{{"complete_hosted_component_dimensions",false}},
        Json{{"complete_slab_component_dimensions",true}}}) {
        auto changed = encoded;
        for (const auto& [key,value] : variant.items()) changed["coordinated_replacements"][key] = value;
        invalid([&] { (void)decode_phase_constraint_authoring_intent(changed); },
                "new slab authority must refuse unknown, false or absent-child completion flags");
    }
    auto nested = encoded;
    nested["coordinated_replacements"] = {{"version",6},{"authoring",encoded.at("coordinated_replacements")},
        {"complete_hosted_component_dimensions",true}};
    invalid([&] { (void)decode_phase_constraint_authoring_intent(nested); },"coordinated slab authority must not nest");
    auto one_family = encoded;
    one_family["coordinated_replacements"]["ordinary_structural_edits"] = Json::array();
    invalid([&] { (void)replay_phase_constraint_authoring(source.entities(),one_family); },
            "a slab cohort is not a mixed-family envelope");
    auto unit_family = encoded;
    unit_family["coordinated_replacements"]["ordinary_slab_geometry"] =
        Json::array({encode_slab_geometry_edit_intent(resize("unit-slab",false,true))});
    invalid([&] { (void)replay_phase_constraint_authoring(source.entities(),unit_family); },
            "unit-only slab lane cannot fabricate a second changed family");
}
void unit_authentication_and_shared_catalog_conflict() {
    auto document = fixture(true);
    const auto snapshot = document.snapshot();
    const auto& source = snapshot.entities();
    const auto edits = ordinary_intents(true);
    const auto slab_candidate = replay_slab_geometry_with_hosted_entities(source,edits);
    // Three changed slab associations plus one unit association, despite no unit upsert.
    (void)carry_hosted_assembly_directional_resize_dimension_entities(source,slab_candidate,edits,4);
    invalid([&] { (void)carry_hosted_assembly_directional_resize_dimension_entities(source,slab_candidate,edits,3); },
            "unchanged unit association must consume the aggregate directional budget");
    auto forged = slab_candidate;
    forged.at("unit-slab").extensions["unproved"] = true;
    invalid([&] { (void)carry_hosted_assembly_directional_resize_dimension_entities(source,forged,edits); },
            "unit directional host must be independently authenticated");
    forged = slab_candidate;
    for (auto& value : forged.at("shared-catalog").properties["model"]["instances"])
        if (value.at("id") == "unit-root") value["property_overrides"]["unproved"] = "changed";
    invalid([&] { (void)carry_hosted_assembly_directional_resize_dimension_entities(source,forged,edits); },
            "unit dimension-bearing root must retain its actual replayed complete row");
    auto conflicting = replay_structural_hosted_component_geometry(source,{column_move()});
    for (auto& value : conflicting.at("shared-catalog").properties["model"]["instances"])
        if (value.at("id") == "slab-root") value["placement"]["translation_m"][0] = 999.0;
    invalid([&] { (void)merge_disjoint_directionally_resized_hosted_assembly_models(
        source.at("shared-catalog").properties.at("model"),
        slab_candidate.at("shared-catalog").properties.at("model"),
        conflicting.at("shared-catalog").properties.at("model")); },
        "directional geometry and a conflicting sibling placement cannot share a source row");
}
PhaseSlabReplacementAuthoring complete_leaf(const Entities& source, bool curved, bool copies = true) {
    const auto inventory = inspect_phase_slab_replacement_plan(source,{"slab"},"phases","alternative",copies);
    require(inventory.ready(),"baseline fixture must have a genuine complete replacement inventory");
    PhaseSlabReplacementAuthoring leaf;
    leaf.registry_id = "phases";
    leaf.alternative_id = "alternative";
    leaf.seed_slab_ids = {"slab"};
    leaf.slab_geometry = {resize("slab",curved)};
    leaf.coordinate_hosted_directional_geometry = true;
    leaf.preserve_slab_planar_source_geometry = curved;
    leaf.complete_component_dimensions = copies;
    for (const auto& id : inventory.required_entity_ids) leaf.identities.emplace(id,"proposed-"+id);
    for (const auto& id : inventory.required_child_ids) leaf.identities.emplace(id,"proposed-"+id);
    for (const auto& root : inventory.required_hosted_instance_ids)
        leaf.hosted_instance_identities.emplace(root,"proposed-"+root.second);
    for (const auto& id : inventory.required_dimension_ids) leaf.dimension_identities.emplace(id,"proposed-"+id);
    return leaf;
}
PhaseSlabReplacementAuthoring complete_leaf(const DocumentSnapshot& source, bool curved, bool copies = true) {
    return complete_leaf(source.entities(),curved,copies);
}
void baseline_complete_directional_and_mapped_children() {
    for (const bool curved : {false,true}) {
        auto document = fixture(curved,true);
        const auto before = document.snapshot();
        const auto leaf = complete_leaf(before,curved);
        const auto encoded_leaf = encode_phase_slab_replacement_authoring(leaf);
        require(encoded_leaf.at("version") == (curved ? 12 : 10) &&
                encoded_leaf.at("authoring").at("version") == (curved ? 11 : 9),
                "complete slab child must retain the exact standalone mapped/directional wrapper pair");
        auto body = empty_body();
        body["slab_replacement"] = encoded_leaf;
        body["complete_slab_component_dimensions"] = true;
        body["ordinary_structural_edits"] = Json::array({encode_structural_object_edit_intent(column_move())});
        const auto encoded = proof(before,body);
        const auto components = phase_constraint_replacement_components(decode_phase_constraint_authoring_intent(encoded));
        require(components.size() == 1 && encode_phase_constraint_authoring_intent(components.front()).at("version") ==
                    (curved ? 45 : 43),
                "identity guards must receive the real exact standalone slab component");
        require(components.front().source_entities_digest == entity_map_digest(before.entities()) &&
                components.front().source_snapshot_digest == document_snapshot_digest(before),
                "replacement component must retain original source binding");
        auto expected = replay_phase_slab_replacement_authoring(before.entities(),leaf);
        const auto column_candidate = replay_structural_hosted_component_geometry(before.entities(),{column_move()});
        expected.at("column") = column_candidate.at("column");
        expected.at("column-catalog") = column_candidate.at("column-catalog");
        const std::vector<std::string> hosts{"column"};
        for (const auto& value : carry_hosted_assembly_dimension_transform_entities(before.entities(),column_candidate,hosts))
            expected.at(value.id) = value;
        const auto replayed = replay_phase_constraint_authoring(before.entities(),encoded);
        require(replayed == expected,"mixed complete replacement must retain exact independently replayed clones and final ordinary carry");
        for (const auto* id : {"slab","slab-catalog","slab-dimension","nested-dimension","profile-dimension"})
            require(replayed.at(id) == before.entities().at(id),"baseline physical/catalog/dimension source envelopes must remain exact");
        for (const auto& [original,proposed] : leaf.dimension_identities) {
            const auto dimension = decoded_dimension(replayed,proposed);
            require(dimension.assembly_target->component.catalog_id == "proposed-slab-catalog" &&
                    dimension.assembly_target->component.instance_id == "proposed-slab-root",
                    "dimension copies must bind actual fresh qualified component identities");
            require(dimension.presentation == decoded_dimension(before.entities(),original).presentation &&
                    replayed.at(proposed).extensions == before.entities().at(original).extensions,
                    "complete copies must retain source presentation and opaque extensions");
        }
        auto missing = encoded;
        missing["coordinated_replacements"]["slab_replacement"]["dimension_identities"].erase("nested-dimension");
        invalid([&] { (void)replay_phase_constraint_authoring(before.entities(),missing); },
                "complete nested dimension inventory cannot omit an actual source member");
        auto extra = encoded;
        extra["coordinated_replacements"]["slab_replacement"]["dimension_identities"]["column-dimension"] = "unproved-copy";
        invalid([&] { (void)replay_phase_constraint_authoring(before.entities(),extra); },
                "dimension mapping cannot borrow an ordinary sibling root");
        auto historical = encoded;
        historical["version"] = 41;
        auto old_body = body;
        old_body["version"] = 4;
        old_body.erase("complete_hosted_component_dimensions");
        old_body.erase("complete_slab_component_dimensions");
        historical["coordinated_replacements"] = {{"version",9},{"authoring",old_body},
            {"complete_hosted_component_dimensions",true},{"complete_slab_component_dimensions",true}};
        invalid([&] { (void)decode_phase_constraint_authoring_intent(historical); },
                "historical coordinated complete wrapper9 cannot borrow slab10/12");
        document.apply(command(encoded));
        const auto after = document.snapshot();
        require(after.entities() == replayed && after.history().size() == before.history().size()+1,
                "baseline copy and ordinary sibling must publish together");
        document.undo(document.revision());
        require(document.snapshot().entities() == before.entities(),"one Undo must restore baseline replacement and sibling");
        document.redo(document.revision());
        require(document.snapshot().entities() == after.entities(),"one Redo must restore complete clones without double carry");
    }
}
void baseline_raw_directional_and_mapped_children() {
    for (const bool curved : {false,true}) {
        auto document = fixture(curved,true,false);
        const auto before = document.snapshot();
        const auto leaf = complete_leaf(before,curved,false);
        const auto leaf_wire = encode_phase_slab_replacement_authoring(leaf);
        require(leaf_wire.at("version") == (curved ? 11 : 9),
                "dimension-free baseline must retain genuine raw slab11/9 authority");
        auto body = empty_body();
        body["slab_replacement"] = leaf_wire;
        body["ordinary_structural_edits"] = Json::array({encode_structural_object_edit_intent(column_move())});
        const auto encoded = proof(before,body);
        auto expected = replay_phase_slab_replacement_authoring(before.entities(),leaf);
        const auto column_candidate = replay_structural_hosted_component_geometry(before.entities(),{column_move()});
        expected.at("column") = column_candidate.at("column");
        expected.at("column-catalog") = column_candidate.at("column-catalog");
        const auto replayed = replay_phase_constraint_authoring(before.entities(),encoded);
        require(replayed == expected && replayed.at("slab") == before.entities().at("slab") &&
                replayed.at("slab-catalog") == before.entities().at("slab-catalog"),
                "new mixed-family raw slab leaf must replay geometry while retaining exact baseline owners");
        const auto children = phase_constraint_replacement_components(decode_phase_constraint_authoring_intent(encoded));
        require(children.size() == 1 && encode_phase_constraint_authoring_intent(children.front()).at("version") ==
                    (curved ? 44 : 42), "raw component must retain its exact standalone phase44/42 wrapper");
        document.apply(command(encoded));
        require(document.snapshot().entities() == expected && document.revision() == before.revision()+1,
                "dimension-free raw baseline and actual sibling must publish in one event");
    }
}

// These boundaries catch counting outputs instead of associations, accepting
// unproved raw catalog fields, dropping ordinary dimension phase policy, and
// checking fresh IDs against the current map without retained history.
void complete_slab_leaf_authenticates_mixed_ordinary_directional_cohort() {
    // Curved/nested mapping is covered above. This cohort isolates complete
    // copy + mapped ordinary + rigid carry within catalog work limits.
    auto seed=fixture(false,true);
    auto entities=seed.snapshot().entities();
    entities.emplace("ordinary-slab",slab("ordinary-slab",false));
    entities.emplace("unit-slab",slab("unit-slab",false));
    entities.emplace("moved-slab",slab("moved-slab",false));
    entities.emplace("ordinary-slab-catalog",catalog("ordinary-slab-catalog",false,{
        hosted("ordinary-root","ordinary-slab"),hosted("unit-root","unit-slab"),hosted("moved-root","moved-slab")}));
    entities.emplace("ordinary-dimension",dimension("ordinary-dimension","ordinary-slab-catalog","ordinary-root",true,true));
    entities.emplace("unit-dimension",dimension("unit-dimension","ordinary-slab-catalog","unit-root",true));
    entities.emplace("moved-dimension",dimension("moved-dimension","ordinary-slab-catalog","moved-root",true));
    std::vector<Entity> values;
    for (const auto& [key,value]:entities) { (void)key; values.push_back(value); }
    auto document=Document::create(values);
    const auto before=document.snapshot();
    auto leaf=complete_leaf(before,true);
    SlabGeometryEditIntent move;
    move.slab_id="moved-slab";
    move.kind=SlabGeometryEditKind::transform_plan;
    move.transform=PlanarTransform{.rotation_radians=0.22,.offset={2.5,-1.5}};
    move.coordinate_world_hosted_geometry=true;
    leaf.ordinary_geometry={resize("ordinary-slab",true),move};
    leaf.coordinate_ordinary_hosted_geometry=true;
    auto extra_unit=leaf;
    extra_unit.ordinary_geometry.push_back(resize("unit-slab",false,true));
    invalid([&] { (void)replay_phase_slab_replacement_authoring(before.entities(),extra_unit); },
            "a complete replacement leaf cannot claim unchanged ordinary membership",
            "Phase slab replacement: mixed geometry roles differ from actual changed saved membership");
    auto expected=replay_phase_slab_replacement_authoring(before.entities(),leaf);
    require(expected.at("moved-dimension")!=before.entities().at("moved-dimension") &&
            expected.at("unit-dimension")==before.entities().at("unit-dimension"),
            "complete leaf must carry its ordinary dimension and retain the untargeted unit dimension");
    const auto sibling=replay_structural_hosted_component_geometry(before.entities(),{column_move()});
    expected.at("column")=sibling.at("column");
    expected.at("column-catalog")=sibling.at("column-catalog");
    const std::vector<std::string> hosts{"column"};
    for (const auto& value:carry_hosted_assembly_dimension_transform_entities(before.entities(),sibling,hosts))
        expected.at(value.id)=value;
    auto body=empty_body();
    body["slab_replacement"]=encode_phase_slab_replacement_authoring(leaf);
    body["complete_slab_component_dimensions"]=true;
    body["ordinary_structural_edits"]=Json::array({encode_structural_object_edit_intent(column_move())});
    const auto encoded=proof(before,body);
    require(replay_phase_constraint_authoring(before.entities(),encoded)==expected,
            "complete directional cohort must be authenticated independently and carried once with its sibling");
    document.apply(command(encoded));
    require(document.snapshot().entities()==expected,
            "complete clones and mixed ordinary cohort must publish in one coordinated event");
}
Entity budget_catalog(const std::string& id, std::vector<AssemblyInstance> rows) {
    // Association limits need only one straight profile and one selected node.
    // Curved/nested geometry is exercised by the separate geometry oracles.
    for (auto& instance:rows) instance.type_id="component";
    auto type=component_type(false);
    type.properties={{"opaque","source root property"}};
    const auto model=AssemblyModel::create({}, {type},rows);
    return {id,"assembly_model",{{"model",model.to_json()}},false};
}
Entities dimension_budget_fixture(bool baseline, std::size_t slab_count,
                                  std::size_t unit_count, std::size_t column_count) {
    auto seed = fixture(false,baseline,false);
    auto entities = seed.snapshot().entities();
    for (auto& [id,entity]:entities) if (entity.type=="assembly_model")
        entity=budget_catalog(id,AssemblyModel::from_json(entity.properties.at("model")).instances());
    std::vector<std::string> baseline_ids{"slab","slab-catalog"};
    const auto add = [&](const char* prefix, std::size_t count, const char* catalog_id,
                         const char* instance_id, bool baseline_dimensions) {
        for (std::size_t i=0;i<count;++i) {
            const auto id=std::string(prefix)+std::to_string(i);
            require(entities.emplace(id,dimension(id,catalog_id,instance_id,false)).second,
                    "budget fixture dimension IDs must be distinct");
            if (baseline_dimensions) baseline_ids.push_back(id);
        }
    };
    add("budget-slab-",slab_count,baseline ? "slab-catalog" : "shared-catalog","slab-root",baseline);
    require(!baseline || unit_count==0,"baseline budget fixture has no ordinary unit host");
    if (!baseline) add("budget-unit-",unit_count,"shared-catalog","unit-root",false);
    add("budget-column-",column_count,baseline ? "column-catalog" : "shared-catalog","column-root",false);
    if (baseline) {
        const auto phases=ModelPhases::create(baseline_ids,baseline_ids,
            {RemodelingAlternative{"alternative","Actual selected alternative",{}, {}}},"alternative");
        entities.at("phases").properties["model"]=phases.to_json();
    }
    return entities;
}
// Resource-boundary maps deliberately need not be publishable Documents.
// Pure-map replay authenticates the actual entity digest and saved choices;
// these fixed snapshot fields satisfy the wire codec but grant no publication
// authority. Physical, curved and nested tests above use real snapshot bindings.
PhaseConstraintAuthoringIntent budget_map_intent(const Entities& source,const ConstraintAuthoringIntent& semantic) {
    return {0,std::string(64,'0'),std::string(64,'1'),entity_map_digest(source),
        std::nullopt,phase_constraint_authoring_selections(source),semantic};
}
Json budget_map_proof(const Entities& source,const Json& body) {
    ConstraintAuthoringIntent semantic;
    semantic.message="Pure-map component dimension resource boundary";
    auto intent=budget_map_intent(source,semantic);
    intent.coordinated_replacements=body;
    return encode_phase_constraint_authoring_intent(intent);
}
template<class F> void budget_case(const char* context,F&& action) {
    try { action(); }
    catch (const std::exception& error) { throw std::runtime_error(std::string(context)+": "+error.what()); }
}
std::size_t budget_component_dimension_count(const Entities& entities) {
    std::size_t count=0;
    for (const auto& [id,entity]:entities) {
        (void)id;
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded=decode_boundary_dimension_entity(entity);
        require(decoded.supported(),"budget fixture must contain supported dimensions");
        if (is_assembly_boundary_dimension_kind(decoded.dimension->kind)) ++count;
    }
    return count;
}
Json budget_proof(const Entities& source, bool baseline) {
    auto body=empty_body();
    if (baseline) {
        body["slab_replacement"]=encode_phase_slab_replacement_authoring(complete_leaf(source,false));
        body["complete_slab_component_dimensions"]=true;
    } else body["ordinary_slab_geometry"]=Json::array({
        encode_slab_geometry_edit_intent(resize("slab",false)),
        encode_slab_geometry_edit_intent(resize("unit-slab",false,true))});
    body["ordinary_structural_edits"]=Json::array({encode_structural_object_edit_intent(column_move())});
    return budget_map_proof(source,body);
}
void aggregate_association_budget_across_families_and_copies() {
    // Both independent families stay under their own 1000-target bounds.
    // Only a shared association ledger can reject the combined 1001 targets.
    for (const bool baseline:{false,true}) budget_case(baseline ? "baseline clones + column" : "ordinary slab + column",[&] {
        const auto source=dimension_budget_fixture(baseline,500,0,500);
        const auto unchanged=source;
        const auto candidate=replay_phase_constraint_authoring(source,budget_proof(source,baseline));
        // The baseline map retains 1000 source rows and adds 500 clones. Its
        // 1000-association success exercises replay defense, not publication.
        require(budget_component_dimension_count(candidate)==(baseline ? 1500 : 1000),
                "exact aggregate replay must retain its full source and clone inventory");
        require(candidate.at("budget-column-0")!=source.at("budget-column-0"),
                "exact 1000-target boundary must carry the actual sibling dimension");
        if (baseline) {
            require(candidate.at("budget-slab-0")==source.at("budget-slab-0") &&
                    candidate.contains("proposed-budget-slab-0") &&
                    candidate.contains("proposed-budget-slab-499"),
                    "exact mixed clone boundary must preserve originals and contribute every fresh clone");
        } else require(candidate.at("slab")!=source.at("slab"),
                       "exact mixed ordinary boundary must retain the genuine physical resize");
        require(source==unchanged,"exact aggregate replay cannot mutate source");
        auto over=dimension_budget_fixture(baseline,501,0,500);
        const auto before=over;
        invalid([&] { (void)replay_phase_constraint_authoring(over,budget_proof(over,baseline)); },
                "1001 actual associations across independently bounded families/copies must refuse",
                "Assembly dimension transform: embedded clone dimension count exceeds bound");
        require(over==before,
                "aggregate budget refusal cannot mutate source");
    });
}
void unit_no_upsert_associations_consume_final_budget() {
    const auto source=dimension_budget_fixture(false,499,1,500);
    const auto unchanged=source;
    const auto candidate=replay_phase_constraint_authoring(source,budget_proof(source,false));
    require(candidate.at("budget-unit-0")==source.at("budget-unit-0") &&
            candidate.at("unit-slab")==source.at("unit-slab"),
            "unit association at the exact shared bound must produce no upsert");
    require(source==unchanged,"exact unit-association replay cannot mutate source");
    auto over=dimension_budget_fixture(false,499,2,500);
    const auto before=over;
    const auto slab_candidate=replay_slab_geometry_with_hosted_entities(over,ordinary_intents(false));
    const auto slab_dimensions=carry_hosted_assembly_directional_resize_dimension_entities(
        over,slab_candidate,ordinary_intents(false));
    require(slab_dimensions.size()<=499,
            "501 slab associations must omit both authenticated unit outputs; unchanged ink needs no upsert");
    for (const auto& value:slab_dimensions)
        require(value.id!="budget-unit-0" && value.id!="budget-unit-1",
                "authenticated unit dimensions must not contribute an upsert");
    invalid([&] { (void)replay_phase_constraint_authoring(over,budget_proof(over,false)); },
            "1001 associations must refuse even when two authenticated units contribute no upserts",
            "Assembly dimension transform: embedded clone dimension count exceeds bound");
    require(over==before,"unit-association budget refusal cannot mutate source");
}
Entities wall_dimension_budget_fixture(std::size_t opening_count,std::size_t clone_count) {
    auto entities=dimension_budget_fixture(false,499,1,0);
    const auto wall=[&](const std::string& id,double x) {
        Entity result{id,"wall",{{"baseline",{{"start",{x,0.0}},{"end",{x+4,0.0}},
            {"sweep_radians",0.0}}},{"thickness_m",0.14},{"height_m",2.4},{"elevation_m",0.0}},false};
        for (const auto* field:{"property_id","building_id","floor_id","layer_id"})
            result.properties[field]=entities.at("slab").properties.at(field);
        return result;
    };
    entities.emplace("baseline-wall",wall("baseline-wall",20));
    entities.emplace("ordinary-wall",wall("ordinary-wall",30));
    entities.emplace("ordinary-opening",Entity{"ordinary-opening","opening",
        {{"wall_id","ordinary-wall"},{"offset_m",1.0},{"width_m",1.0},
            {"sill_m",0.0},{"height_m",2.0}},false});
    entities.emplace("baseline-wall-catalog",budget_catalog("baseline-wall-catalog",{
        hosted("baseline-wall-root","baseline-wall")}));
    entities.emplace("ordinary-wall-catalog",budget_catalog("ordinary-wall-catalog",{
        hosted("ordinary-wall-root","ordinary-wall"),hosted("ordinary-opening-root","ordinary-opening")}));
    std::vector<std::string> baseline_ids{"baseline-wall","baseline-wall-catalog"};
    const auto add=[&](const char* prefix,std::size_t count,const char* catalog_id,const char* root,bool baseline) {
        for (std::size_t i=0;i<count;++i) {
            const auto id=std::string(prefix)+std::to_string(i);
            require(entities.emplace(id,dimension(id,catalog_id,root,false)).second,
                    "wall budget fixture dimension IDs must be distinct");
            if (baseline) baseline_ids.push_back(id);
        }
    };
    add("budget-wall-clone-",clone_count,"baseline-wall-catalog","baseline-wall-root",true);
    add("budget-wall-ordinary-",50,"ordinary-wall-catalog","ordinary-wall-root",false);
    add("budget-opening-ordinary-",opening_count,"ordinary-wall-catalog","ordinary-opening-root",false);
    const auto phases=ModelPhases::create(baseline_ids,baseline_ids,
        {RemodelingAlternative{"alternative","Actual selected alternative",{}, {}}},"alternative");
    entities.emplace("phases",Entity{"phases","model_phases",{{"model",phases.to_json()}},false});
    return entities;
}
Json wall_budget_child(const Entities& source,bool copies=true) {
    ConstraintAuthoringIntent semantic;
    semantic.message="Move walls with complete component dimensions";
    semantic.wall_geometry_move=WallGeometryMoveIntent{{{"ordinary-wall",{31,2},{35,2}}},false};
    if (copies) semantic.wall_geometry_move->targets.push_back({"baseline-wall",{21,2},{25,2}});
    auto child=budget_map_intent(source,semantic);
    if (!copies) return encode_phase_constraint_authoring_intent(child);
    const auto inventory=inspect_phase_wall_replacement_plan(source,{"baseline-wall"},
        "phases","alternative",true,true,true,true);
    require(inventory.ready(),"wall budget fixture must have a complete typed replacement inventory");
    PhaseWallReplacementAuthoring leaf;
    leaf.registry_id="phases";
    leaf.alternative_id="alternative";
    leaf.seed_wall_ids={"baseline-wall"};
    leaf.complete_presentations=true;
    leaf.complete_corner_windows=true;
    leaf.complete_hosted_instances=true;
    leaf.complete_component_dimensions=true;
    for (const auto& id:inventory.required_entity_ids) leaf.identities.emplace(id,"proposed-"+id);
    for (const auto& id:inventory.required_child_ids) leaf.identities.emplace(id,"proposed-"+id);
    for (const auto& root:inventory.required_hosted_instance_ids)
        leaf.hosted_instance_identities.emplace(root,"proposed-"+root.second);
    for (const auto& id:inventory.required_dimension_ids) leaf.dimension_identities.emplace(id,"proposed-"+id);
    child.wall_replacement=encode_phase_wall_replacement_authoring(leaf);
    return encode_phase_constraint_authoring_intent(child);
}
Json wall_budget_proof(const Entities& source,const Json& child) {
    auto body=empty_body();
    body["wall_authoring"]=child;
    if (child.contains("wall_replacement")) body["complete_wall_component_dimensions"]=true;
    body["ordinary_slab_geometry"]=Json::array({encode_slab_geometry_edit_intent(resize("slab",false)),
        encode_slab_geometry_edit_intent(resize("unit-slab",false,true))});
    return budget_map_proof(source,body);
}
void wall_clones_and_dependencies_respect_reconstructed_inventory_bound() {
    // Wall physical reconstruction validates every retained and fresh dimension
    // before the enclosing shared ledger runs. With 500 slab associations and
    // 100 ordinary wall dependencies, 200 clones yield exactly 1000 stored
    // dimensions (800 associations). The next opening reaches the earlier
    // inventory guard; a 1001-association wall case cannot reach the final ledger.
    const auto source=wall_dimension_budget_fixture(50,200);
    const auto unchanged=source;
    const auto child=wall_budget_child(source);
    const auto wall_candidate=replay_phase_constraint_authoring(source,child);
    require(wall_candidate.at("ordinary-opening")==source.at("ordinary-opening"),
            "wall dependency fixture must retain unchanged opening fields");
    require(wall_candidate.at("ordinary-wall")!=source.at("ordinary-wall"),
            "wall dependency fixture must actually move the ordinary wall");
    auto expected_dimensions=source;
    const std::vector<std::string> wall_hosts{"ordinary-wall","ordinary-opening"};
    for (const auto& dimension:carry_wall_hosted_assembly_dimension_transform_entities(source,wall_candidate,wall_hosts))
        expected_dimensions.at(dimension.id)=dimension;
    require(wall_candidate.at("budget-wall-ordinary-0")==expected_dimensions.at("budget-wall-ordinary-0") &&
            wall_candidate.at("budget-opening-ordinary-0")==expected_dimensions.at("budget-opening-ordinary-0"),
            "wall and unchanged-opening associations must follow their independently resolved authored frames");
    const auto candidate=replay_phase_constraint_authoring(source,wall_budget_proof(source,child));
    require(budget_component_dimension_count(candidate)==1000,
            "reachable wall clone boundary must contain exactly 1000 retained and fresh dimensions");
    require(candidate.at("budget-wall-clone-0")==source.at("budget-wall-clone-0") &&
            candidate.at("proposed-budget-wall-clone-199")==wall_candidate.at("proposed-budget-wall-clone-199") &&
            candidate.at("budget-wall-ordinary-0")==wall_candidate.at("budget-wall-ordinary-0") &&
            candidate.at("budget-opening-ordinary-49")==wall_candidate.at("budget-opening-ordinary-49") &&
            candidate.at("slab")!=source.at("slab") &&
            candidate.at("shared-catalog")!=source.at("shared-catalog") &&
            candidate.at("budget-unit-0")==source.at("budget-unit-0") &&
            candidate.at("unit-slab")==source.at("unit-slab"),
            "200 wall clones, 100 ordinary dependencies and 500 slab associations must pass exactly once");
    require(source==unchanged,"exact wall-clone inventory replay cannot mutate source");
    auto over=wall_dimension_budget_fixture(51,200);
    const auto before=over;
    const auto over_child=wall_budget_child(before);
    invalid([&] { (void)replay_phase_constraint_authoring(over,over_child); },
            "wall child with 801 retained dimensions and 200 clones must refuse at its inventory guard",
            "Standalone component dimension inventory exceeds its shared resolution bound");
    const auto slab_candidate=replay_slab_geometry_with_hosted_entities(over,ordinary_intents(false));
    const auto slab_dimensions=carry_hosted_assembly_directional_resize_dimension_entities(
        over,slab_candidate,ordinary_intents(false));
    require(slab_dimensions.size()<=499,
            "500 slab associations must omit the unit output; unchanged ink needs no upsert");
    for (const auto& dimension:slab_dimensions)
        require(dimension.id!="budget-unit-0","the slab unit association must contribute no upsert");
    invalid([&] { (void)replay_phase_constraint_authoring(over,wall_budget_proof(over,over_child)); },
            "enclosing replay must preserve the wall child's 1001-dimension inventory refusal",
            "Standalone component dimension inventory exceeds its shared resolution bound");
    require(over==before,"wall inventory refusal cannot mutate source");
}
void ordinary_wall_dependencies_respect_inventory_bound_without_clones() {
    const auto source=wall_dimension_budget_fixture(450,0);
    const auto unchanged=source;
    const auto child=wall_budget_child(source,false);
    const auto wall_candidate=replay_phase_constraint_authoring(source,child);
    auto expected_dimensions=source;
    const std::vector<std::string> wall_hosts{"ordinary-wall","ordinary-opening"};
    for (const auto& dimension:carry_wall_hosted_assembly_dimension_transform_entities(source,wall_candidate,wall_hosts))
        expected_dimensions.at(dimension.id)=dimension;
    const auto candidate=replay_phase_constraint_authoring(source,wall_budget_proof(source,child));
    require(candidate.at("ordinary-opening")==source.at("ordinary-opening") &&
            candidate.at("ordinary-wall")!=source.at("ordinary-wall") &&
            candidate.at("budget-wall-ordinary-0")==expected_dimensions.at("budget-wall-ordinary-0") &&
            candidate.at("budget-opening-ordinary-449")==expected_dimensions.at("budget-opening-ordinary-449") &&
            candidate.at("budget-unit-0")==source.at("budget-unit-0"),
            "500 ordinary wall/opening and 500 slab associations must pass without replacement authority");
    require(source==unchanged,"exact ordinary wall budget replay cannot mutate source");
    auto over=wall_dimension_budget_fixture(451,0);
    const auto before=over;
    // Ordinary wall reconstruction also runs full boundary integrity before
    // composition, so its 1001-source map exercises that earlier defense.
    invalid([&] { (void)replay_phase_constraint_authoring(over,
        wall_budget_proof(before,wall_budget_child(before,false))); },
        "ordinary wall reconstruction must refuse the 1001-dimension source inventory",
        "Standalone component dimension inventory exceeds its shared resolution bound");
    require(over==before,"ordinary wall inventory refusal cannot mutate source");
}
void document_dimension_inventory_bound_remains_enforced() {
    const auto values=[](const Entities& source) {
        std::vector<Entity> result;
        for (const auto& [id,entity]:source) { (void)id; result.push_back(entity); }
        return result;
    };
    const auto exact=dimension_budget_fixture(false,500,0,500);
    const auto admitted=Document::create(values(exact));
    require(admitted.snapshot().entities()==exact,"Document must still admit exactly 1000 valid component dimensions");
    auto over=dimension_budget_fixture(false,501,0,500);
    const auto before=over;
    bool refused=false;
    try { (void)Document::create(values(over)); }
    catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::invalid_entity &&
                std::string(error.what())=="Standalone component dimension inventory exceeds its shared resolution bound",
                "Document inventory refusal must retain its typed validation error");
        refused=true;
    }
    require(refused,"pure-map budget fixtures cannot authorize publishing a 1001-dimension Document");
    require(over==before,"Document admission refusal cannot mutate the source map");
}
void conflicting_actual_catalog_contributions_refuse() {
    auto document=fixture(true);
    const auto source=document.snapshot().entities();
    const auto slab_candidate=replay_slab_geometry_with_hosted_entities(source,ordinary_intents(true));
    const auto structural_candidate=replay_structural_hosted_component_geometry(source,{column_move()});
    const auto& original=source.at("shared-catalog").properties.at("model");
    const auto& slab_model=slab_candidate.at("shared-catalog").properties.at("model");
    auto incoming=structural_candidate.at("shared-catalog").properties.at("model");
    // Mutate the actual independently replayed sibling, rather than supplying
    // an artificial catalog with no connection to the source contributions.
    for (auto& value:incoming.at("instances")) if (value.at("id")=="slab-root")
        value["placement"]["translation_m"][0]=999.0;
    invalid([&] { (void)merge_disjoint_directionally_resized_hosted_assembly_models(original,slab_model,incoming); },
            "same-row directional and sibling placement contributions must refuse");
    incoming=structural_candidate.at("shared-catalog").properties.at("model");
    for (auto& type:incoming.at("types")) if (type.at("id")=="parent")
        type["name"]="Unproved sibling definition";
    invalid([&] { (void)merge_disjoint_directionally_resized_hosted_assembly_models(original,slab_model,incoming); },
            "a genuine sibling consequence cannot contribute conflicting actual catalog definitions");
}
void retained_history_dimension_destination_collision() {
    auto document=fixture(true,true);
    const std::string collision="history-only-dimension-token";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(Entity{collision,"label",{{"text","Historical reservation"}},false})},
        .message="Reserve an identity in history"});
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::erase(collision)},.message="Remove current reservation owner"});
    const auto before=document.snapshot();
    require(!before.entities().contains(collision),"collision owner must exist only in retained history");
    bool retained=false;
    for (const auto& record:before.history()) retained=retained || record.entities.contains(collision);
    require(retained,"collision fixture must genuinely reserve its token in retained history");
    auto leaf=complete_leaf(before,true);
    leaf.dimension_identities.at("slab-dimension")=collision;
    auto body=empty_body();
    body["slab_replacement"]=encode_phase_slab_replacement_authoring(leaf);
    body["complete_slab_component_dimensions"]=true;
    body["ordinary_structural_edits"]=Json::array({encode_structural_object_edit_intent(column_move())});
    const auto encoded=proof(before,body);
    // Pure replay can derive a valid fresh clone; lifetime authority belongs
    // to Document and must refuse precisely when the command is published.
    require(replay_phase_constraint_authoring(before.entities(),encoded).contains(collision),
            "current-map clone admission cannot invent a retained-history collision");
    invalid([&] { document.apply(command(encoded)); },
            "history-only dimension identity must fail retained lifetime admission");
    require(document_snapshot_digest(document.snapshot())==document_snapshot_digest(before),
            "retained-history-only dimension identity collision must refuse atomically");
}
void active_and_inactive_dimension_membership_policy() {
    for (const int lane:{0,1,2}) {
        auto seed=fixture(true);
        auto entities=seed.snapshot().entities();
        const std::string id="column-dimension";
        const std::vector<std::string> baseline=lane==2 ? std::vector<std::string>{id} : std::vector<std::string>{};
        const auto phases=ModelPhases::create({id},baseline,
            {RemodelingAlternative{"selected","Selected",{},lane==0 ? std::vector<std::string>{id} : std::vector<std::string>{}},
             RemodelingAlternative{"other","Inactive",{},lane==1 ? std::vector<std::string>{id} : std::vector<std::string>{}}},
            "selected");
        entities.emplace("dimension-phases",Entity{"dimension-phases","model_phases",{{"model",phases.to_json()}},false});
        std::vector<Entity> values;
        for (const auto& [key,value]:entities) { (void)key; values.push_back(value); }
        auto document=Document::create(values);
        const auto before=document.snapshot();
        const auto scope=constraint_phase_scope(before.entities());
        require(scope.inactive_owner_ids.contains(id)==(lane==1),
                "dimension policy fixture must establish the actual intended active/inactive source");
        const auto encoded=ordinary_proof(before,true);
        if (lane==0) {
            const auto candidate=replay_phase_constraint_authoring(before.entities(),encoded);
            require(candidate.at(id)!=before.entities().at(id),
                    "active proposed dimension must receive its ordinary hosted contribution");
            document.apply(command(encoded));
            require(document.snapshot().entities()==candidate,
                    "active dimension authority must publish the coordinated contribution");
        } else {
            invalid([&] { (void)replay_phase_constraint_authoring(before.entities(),encoded); },
                    "ordinary coordinated carry must refuse inactive and protected baseline dimensions");
            require(document_snapshot_digest(document.snapshot())==document_snapshot_digest(before),
                    "dimension policy refusal must retain source and history");
        }
    }
}

void stale_captured_source_refuses_atomically() {
    auto document = fixture(true);
    const auto encoded = ordinary_proof(document.snapshot(),true);
    const auto old_command = command(encoded);
    auto changed = document.snapshot().entities().at("unit-slab");
    changed.extensions["source-member-change"] = "must authenticate even though unit";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(changed)},.message="Change actual unit member"});
    const auto before = document.snapshot();
    stale([&] { document.apply(old_command); },"stale captured source must refuse before any geometry/history mutation");
    require(document.snapshot().entities() == before.entities() && document.revision() == before.revision() &&
            document.snapshot().history().size() == before.history().size(),
            "stale refusal must preserve the exact current source and history");
    // Rebasing only the revision cannot repair the original snapshot/digest binding.
    auto forged = encoded;
    forged["expected_revision"] = before.revision();
    stale([&] { document.apply(command(forged)); },"revision-only rebase cannot replace actual captured source authority");
    auto entities_only = encoded;
    entities_only["source_entities_digest"] = entity_map_digest(before.entities());
    entities_only["expected_revision"] = before.revision();
    stale([&] { document.apply(command(entities_only)); },"entity-only rebase cannot replace snapshot and authoring source authority");
}
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path()/("vertex-coordinated-resize-"+make_stable_id());
    Temporary() { std::filesystem::create_directory(path); }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};
void downgrade_markers(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    const auto u8 = file.u8string();
    const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
    require(sqlite3_open_v2(name.c_str(),&db,SQLITE_OPEN_READWRITE,nullptr) == SQLITE_OK,"open controlled saved native project");
    sqlite3_stmt* query = nullptr;
    require(sqlite3_prepare_v2(db,"SELECT value FROM metadata WHERE key='format_version'",-1,&query,nullptr) == SQLITE_OK &&
            sqlite3_step(query) == SQLITE_ROW,"read new coordinated native metadata marker");
    const auto marker = std::string(reinterpret_cast<const char*>(sqlite3_column_text(query,0)));
    sqlite3_finalize(query);
    require(marker == std::to_string(coordinated_reader_floor),"saved metadata must advertise exact reader260");
    require(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&query,nullptr) == SQLITE_OK &&
            sqlite3_step(query) == SQLITE_ROW,"read new coordinated native PRAGMA marker");
    const auto pragma = sqlite3_column_int(query,0);
    sqlite3_finalize(query);
    require(pragma == static_cast<int>(coordinated_reader_floor),"saved native marker pair must agree on reader260");
    const auto status = sqlite3_exec(db,
        "PRAGMA user_version=259; UPDATE metadata SET value='259' WHERE key='format_version'",nullptr,nullptr,nullptr);
    sqlite3_close(db);
    require(status == SQLITE_OK,"write controlled previous-reader marker pair");
    const auto digest = ProjectStore::file_sha256(file);
    bool refused = false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) { refused = error.code() == StorageErrorCode::unsupported_format; }
    require(refused && ProjectStore::file_sha256(file) == digest,
            "previous reader markers must refuse retained new coordinated semantics without rewriting bytes");
}
void storage_history_and_exact_reader_floor() {
    Temporary temporary;
    for (const bool baseline : {false,true}) {
        auto document = fixture(true,baseline);
        const auto source = document.snapshot();
        Json encoded;
        if (baseline) {
            auto body = empty_body();
            body["slab_replacement"] = encode_phase_slab_replacement_authoring(complete_leaf(source,true));
            body["complete_slab_component_dimensions"] = true;
            body["ordinary_structural_edits"] = Json::array({encode_structural_object_edit_intent(column_move())});
            encoded = proof(source,body);
        } else encoded = ordinary_proof(source,true);
        require(ProjectStore::required_format_version(source) < coordinated_reader_floor,
                "fixture cannot obtain reader260 from unrelated preexisting state");
        document.apply(command(encoded));
        const auto applied = document.snapshot();
        document.undo(document.revision());
        const auto undone = document.snapshot();
        document.redo(document.revision());
        const std::vector<DocumentSnapshot> snapshots{applied,undone,document.snapshot()};
        for (std::size_t i = 0; i < snapshots.size(); ++i) {
            const auto& snapshot = snapshots[i];
            require(ProjectStore::required_format_version(snapshot) == coordinated_reader_floor &&
                    ProjectStore::required_semantic_format_version(snapshot) == coordinated_reader_floor,
                    "current/undone/redo history must retain exact coordinated reader260");
            const auto stem = std::string(baseline ? "baseline-" : "ordinary-")+std::to_string(i);
            const auto file = temporary.path/(stem+".bldproj");
            (void)ProjectStore::save(file,snapshot);
            {
                auto reopened = ProjectStore::load(file);
                const auto restored = reopened.document.snapshot();
                require(restored.entities() == snapshot.entities() && restored.revision() == snapshot.revision() &&
                        restored.history().size() == snapshot.history().size(),
                        "native reopen must retain exact coordinated current state and replayed history");
                require(ProjectStore::required_format_version(restored) == coordinated_reader_floor,
                        "reopened retained new proof must retain its reader floor");
            }
            const auto extracted = temporary.path/(stem+"-extract");
            extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");
            require(Json::parse(input).at("exchange_version") == coordinated_reader_floor-2,
                    "logical extraction must advertise exchange258 for new coordinated authority");
            downgrade_markers(file);
        }
    }
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    const char* active_test="initialization";
    try {
        const std::pair<const char*,void(*)()> tests[]{
            {"ordinary_mapped_and_directional_composition",ordinary_mapped_and_directional_composition},
            {"historical_and_closed_schema_refusal",historical_and_closed_schema_refusal},
            {"unit_authentication_and_shared_catalog_conflict",unit_authentication_and_shared_catalog_conflict},
            {"baseline_complete_directional_and_mapped_children",baseline_complete_directional_and_mapped_children},
            {"baseline_raw_directional_and_mapped_children",baseline_raw_directional_and_mapped_children},
            {"complete_slab_leaf_authenticates_mixed_ordinary_directional_cohort",complete_slab_leaf_authenticates_mixed_ordinary_directional_cohort},
            {"aggregate_association_budget_across_families_and_copies",aggregate_association_budget_across_families_and_copies},
            {"unit_no_upsert_associations_consume_final_budget",unit_no_upsert_associations_consume_final_budget},
            {"wall_clones_and_dependencies_respect_reconstructed_inventory_bound",wall_clones_and_dependencies_respect_reconstructed_inventory_bound},
            {"ordinary_wall_dependencies_respect_inventory_bound_without_clones",ordinary_wall_dependencies_respect_inventory_bound_without_clones},
            {"document_dimension_inventory_bound_remains_enforced",document_dimension_inventory_bound_remains_enforced},
            {"conflicting_actual_catalog_contributions_refuse",conflicting_actual_catalog_contributions_refuse},
            {"retained_history_dimension_destination_collision",retained_history_dimension_destination_collision},
            {"active_and_inactive_dimension_membership_policy",active_and_inactive_dimension_membership_policy},
            {"stale_captured_source_refuses_atomically",stale_captured_source_refuses_atomically},
            {"storage_history_and_exact_reader_floor",storage_history_and_exact_reader_floor}
        };
        for (const auto& test:tests) {
            active_test=test.first;
            test.second();
        }
        std::cout << "Coordinated slab resize regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << active_test << ": " << error.what() << '\n';
        return 1;
    }
}
