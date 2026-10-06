#include "sketch/visualization/native_geometry_preparation.hpp"
#include "sketch/document.hpp"
#include "sketch/workspace_regeneration_queue.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/project_visibility.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/architecture.hpp"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include "support/noninteractive_errors.hpp"
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
using namespace sketch;
using namespace sketch::visualization;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Document walls(int count) {
    std::vector<Entity> entities;
    for (int i = 0; i != count; ++i) {
        entities.push_back(Entity::create("wall", {
            {"baseline", {{"start", {0.0, double(i)}}, {"end", {4.0, double(i)}},
                          {"sweep_radians", 0.0}}},
            {"thickness_m", 0.2}, {"height_m", 2.8}, {"elevation_m", 0.0}}));
    }
    return Document::create(std::move(entities));
}
std::optional<PreparedNativeGeometry> await_result(NativeGeometryRegenerator& worker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (worker.is_pending() && std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.take_completed()) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("geometry preparation timed out");
}

void test_coalescing() {
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<int> calls{};
    NativeGeometryRegenerator worker([&](const DocumentSnapshot& snapshot,
        std::optional<NativeGeometryVisibleIds> visible, const std::function<bool()>&) {
        if (++calls == 1) {
            entered.set_value();
            // Deliberately model an OCCT call which ignores cancellation.
            if (gate.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
                throw std::runtime_error("test preparation gate timed out");
        }
        return std::optional<PreparedNativeGeometry>{PreparedNativeGeometry{
            snapshot.revision(), std::move(visible), {}, {}, {snapshot.document_id()}}};
    });
    auto document = walls(1);
    worker.request(document.snapshot(), std::nullopt);
    const auto started = entered_future.wait_for(std::chrono::seconds(30));
    bool bounded = started == std::future_status::ready;
    for (int i = 0; i < 100 && bounded; ++i) {
        document.apply(NameRevision{document.revision(), "superseded-" + std::to_string(i)});
        worker.request(document.snapshot(), NativeGeometryVisibleIds{});
        bounded = worker.retained_snapshot_count() == 2;
    }
    const auto latest_revision = document.revision();
    release.set_value();
    check(bounded, "blocked worker must retain only running and latest waiting snapshot histories");
    const auto result = await_result(worker);
    check(calls == 2 && result && result->revision == latest_revision &&
          result->visible_ids == std::optional<NativeGeometryVisibleIds>{NativeGeometryVisibleIds{}},
          "only running and latest requests may execute, and only latest may publish");
    worker.shutdown();
    check(worker.retained_snapshot_count() == 0, "shutdown must release every snapshot");
}

void check_meshed(const TopoDS_Shape& shape) {
    std::size_t faces = 0;
    for (TopExp_Explorer face(shape, TopAbs_FACE); face.More(); face.Next()) {
        TopLoc_Location location;
        check(!BRep_Tool::Triangulation(TopoDS::Face(face.Current()), location).IsNull(),
              "every prepared face must carry worker-built triangulation");
        ++faces;
    }
    check(faces != 0, "mesh regression requires real faces");
}

PreparedNativeGeometry prepare(const Document& document) {
    auto result = prepare_native_geometry(document.snapshot(), std::nullopt);
    check(result && result->errors.empty(), "dependency fixture must produce valid geometry");
    return std::move(*result);
}

void architectural_context(Entity& entity) {
    entity.properties.update({{"property_id", "site"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}});
}

void update(Document& document, Entity entity) {
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(std::move(entity))}, {}, "geometry dependency regression"});
}

Entity retained_ifc_reference(std::string id, std::string native_type) {
    return Entity{std::move(id), "ifc_reference",
        {{"ifc_name", "Retained source"}, {"ifc_type", "IFCBUILDINGELEMENTPROXY"}}, false,
        {{"ifc_source", {{"record_id", 1}, {"record_type", "IFCBUILDINGELEMENTPROXY"},
                         {"arguments", "'guid',#1,'Retained, O''Brien',$,'property',#2, $ ,$,.NOTDEFINED."}}},
         {"ifc_vertex_properties", {{"native_entity", {{"id", "source-id"},
             {"type", std::move(native_type)}, {"required", false},
             {"properties", {{"name", "Source descriptor"}}},
             {"extensions", {{"opaque", "Preserved source evidence"}}}}}}}}};
}

void test_ifc_provenance_does_not_block_native_geometry() {
    const auto wall = walls(1).snapshot().entities().begin()->second;
    std::vector<Entity> entities{wall,
        {"receipt", "ifc_source", {{"asset_id", "source-step"}, {"format", "IFC4 STEP"},
            {"diagnostics", nlohmann::json::array({{{"code", "native_reference_only"}}})}}}};
    for (const auto* type : {"property", "building", "floor", "layer", "annotation_state", "ifc_source"})
        entities.push_back(retained_ifc_reference(std::string{"retained-"} + type, type));
    const auto document = Document::create(std::move(entities),
        {Asset::create("source-step", "application/step", {std::byte{'I'}, std::byte{'F'}, std::byte{'C'}})});
    const auto before = document.snapshot();
    const auto result = prepare(document);
    check(result.pending.empty() && result.solids.size() == 1 && result.solids.contains(wall.id),
          "inert IFC organization descriptors and source receipts must not block a native solid");
    check_meshed(result.solids.at(wall.id).shape);
    const auto after = document.snapshot();
    check(after.revision() == before.revision() && after.entities() == before.entities() &&
          after.assets() == before.assets() && after.history().size() == before.history().size(),
          "classifying inert IFC provenance must preserve the complete source document");
}

void test_unresolved_ifc_references_remain_pending() {
    std::vector<Entity> entities{
        retained_ifc_reference("unknown-physical", "future_equipment"),
        retained_ifc_reference("known-physical", "roof"),
        retained_ifc_reference("physical-room-source", "room_boundary")};
    auto malformed = retained_ifc_reference("malformed-descriptor", "property");
    malformed.extensions["ifc_vertex_properties"]["native_entity"].erase("extensions");
    entities.push_back(std::move(malformed));
    auto physical_record = retained_ifc_reference("physical-record", "property");
    physical_record.properties["ifc_type"] = "IFCWALL";
    physical_record.extensions["ifc_source"]["record_type"] = "IFCWALL";
    entities.push_back(std::move(physical_record));
    auto represented_proxy = retained_ifc_reference("represented-proxy", "property");
    represented_proxy.extensions["ifc_source"]["arguments"] =
        "'guid',#1,'Unreconstructed proxy',$,'property',#2,#99,$,.NOTDEFINED.";
    entities.push_back(std::move(represented_proxy));
    auto extra_fields = retained_ifc_reference("extra-fields-proxy", "property");
    extra_fields.extensions["ifc_source"]["arguments"] =
        "'guid',#1,'Ambiguous proxy',$,'property',#2,#99,$,$,.NOTDEFINED.";
    entities.push_back(std::move(extra_fields));
    auto quoted_representation = retained_ifc_reference("quoted-representation", "property");
    quoted_representation.extensions["ifc_source"]["arguments"] =
        "'guid',#1,'Misleading proxy',$,'property',#2,'$',$,.NOTDEFINED.";
    entities.push_back(std::move(quoted_representation));
    auto malformed_arguments = retained_ifc_reference("malformed-arguments", "property");
    malformed_arguments.extensions["ifc_source"]["arguments"] =
        "'guid',#1,'Unterminated proxy,$,'property',#2,$,$,.NOTDEFINED.";
    entities.push_back(std::move(malformed_arguments));
    auto nested_representation = retained_ifc_reference("nested-representation", "property");
    nested_representation.extensions["ifc_source"]["arguments"] =
        "'guid',#1,'Nested proxy',$,'property',#2,(#99,$),$,.NOTDEFINED.";
    entities.push_back(std::move(nested_representation));
    entities.push_back(Entity{"unclassified-reference", "ifc_reference",
        {{"ifc_name", "property"}, {"ifc_type", "IFCBUILDINGELEMENTPROXY"}}});
    const auto document = Document::create(entities);
    for (const auto& mask : {std::optional<NativeGeometryVisibleIds>{},
                            std::optional<NativeGeometryVisibleIds>{NativeGeometryVisibleIds{}}}) {
        const auto result = prepare_native_geometry(document.snapshot(), mask);
        check(result && result->errors.empty() && result->solids.empty() &&
              result->pending.size() == entities.size(),
              "unknown, physical, and malformed IFC references must stay pending even with no visible solids");
        for (const auto& entity : entities)
            check(std::any_of(result->pending.begin(), result->pending.end(), [&](const auto& message) {
                return message.find("'" + entity.id + "'") != std::string::npos;
            }), "every unresolved IFC reference must retain an identifiable pending diagnostic");
    }
}

void test_assembly_identity() {
    auto document = walls(1);
    auto wall = document.snapshot().entities().begin()->second;
    AssemblyInstance instance{"copy", "type", {}, {}, {}, AssemblyPlacement{wall.id}};
    AssemblyType type{"type", "Type", {}, {{"finish", "paint"}}, {}};
    auto catalog = Entity::create("assembly_model", {{"model", AssemblyModel::create(
        {{"paint", "Paint", "#ff0000"}}, {type}, {instance}).to_json()}});
    update(document, catalog);
    const auto child = catalog.id + ":instance:copy";
    const auto initial = prepare(document);
    check_meshed(initial.solids.at(child).shape);
    check(std::abs(solid_volume(initial.solids.at(child).shape) - solid_volume(initial.solids.at(wall.id).shape)) < 1e-6 &&
          initial.solids.at(child).material_regions.empty(),
          "legacy declaration without profiles retains exact host-copy volume and presentation");
    catalog.properties["model"] = AssemblyModel::create(
        {{"paint", "Paint", "#0000ff"}}, {type}, {instance}).to_json();
    update(document, catalog);
    const auto recolored = prepare(document);
    check(initial.solids.at(child).content == recolored.solids.at(child).content &&
          initial.solids.at(child).material_color != recolored.solids.at(child).material_color,
          "assembly material changes must preserve reusable geometry identity");
    instance.placement->translation_m.x = 3.0;
    catalog.properties["model"] = AssemblyModel::create(
        {{"paint", "Paint", "#0000ff"}}, {type}, {instance}).to_json();
    update(document, catalog);
    const auto translated = prepare(document);
    check(recolored.solids.at(child).content != translated.solids.at(child).content,
          "assembly placement must invalidate geometry identity");
    wall.properties["height_m"] = 4.0;
    update(document, wall);
    check(translated.solids.at(child).content != prepare(document).solids.at(child).content,
          "assembly host geometry must invalidate instance identity");
    wall.properties["material_assignment"] = {
        {"version", 1}, {"catalog_id", catalog.id}, {"material_id", "paint"}};
    const auto before_material = prepare(document);
    update(document, wall);
    const auto after_material = prepare(document);
    check(before_material.solids.at(wall.id).content == after_material.solids.at(wall.id).content,
          "host material assignment must remain separate from geometry identity");
}

void test_embedded_assembly_profiles() {
    auto document = walls(1);
    const auto host = document.snapshot().entities().begin()->second;
    const Boundary outer{{{0,0},{4,0}},{{4,0},{4,3}},{{4,3},{0,3}},{{0,3},{0,0}}};
    const Boundary hole{{{1,1},{2,1}},{{2,1},{2,2}},{{2,2},{1,2}},{{1,2},{1,1}}};
    AssemblyType leaf; leaf.id="leaf"; leaf.name="Ring"; leaf.materials["finish"]="red";
    leaf.profiles.push_back({"ring",outer,{hole},0,0.5,"finish"});
    AssemblyType root; root.id="root"; root.name="Nested ring";
    AssemblyPart part; part.id="nested"; part.type_id=leaf.id; root.parts={part};
    AssemblyInstance placed; placed.id="placed"; placed.type_id=root.id;
    placed.placement=AssemblyPlacement{host.id,{10,20},0,2};
    AssemblyInstance standalone; standalone.id="standalone"; standalone.type_id=root.id;
    standalone.root_transform=AssemblyTransform{{30,40,3},0,1};
    AssemblyPathOverride finish; finish.part_path={"nested"}; finish.material_overrides["finish"]="blue";
    standalone.nested_overrides={finish};
    const auto model = [&](const char* color) { return AssemblyModel::create(
        {{"red","Red",color},{"blue","Blue","#0000ff"}},{leaf,root},{placed,standalone}); };
    Entity catalog{"embedded-catalog","assembly_model",{{"model",model("#ff0000").to_json()}}};
    update(document,catalog);
    const auto initial=prepare(document);
    const auto placed_id=catalog.id+":instance:placed", standalone_id=catalog.id+":instance:standalone";
    const auto& solid=initial.solids.at(placed_id);
    check(std::abs(solid_volume(solid.shape)-44)<1e-6 && solid.material_regions.size()==1,
          "embedded nested ring uses holed profile volume rather than differently sized wall host");
    check(initial.solids.at(standalone_id).material_regions.front().material_id=="blue" &&
          initial.solids.at(standalone_id).material_regions.front().material_color=="#0000ff",
          "embedded path material override colors its actual nested profile");
    const auto provenance=nlohmann::json::parse(solid.material_regions.front().source_id);
    check(provenance.at("entity_id")==placed_id && provenance.at("part_path")==nlohmann::json::array({"nested"}) &&
          provenance.at("type_id")==leaf.id && provenance.at("profile_id")=="ring",
          "embedded native region retains typed path/profile provenance and stable catalog child identity");
    Bnd_Box bounds; BRepBndLib::AddOptimal(initial.solids.at(standalone_id).shape,bounds,false,false);
    double x0,y0,z0,x1,y1,z1; bounds.Get(x0,y0,z0,x1,y1,z1);
    check(std::abs(x0-30)<1e-6 && std::abs(x1-34)<1e-6 && std::abs(y0-40)<1e-6 &&
          std::abs(y1-43)<1e-6 && std::abs(z0-3)<1e-6 && std::abs(z1-3.5)<1e-6,
          "embedded root transform publishes actual XYZ geometry without a placement host");
    catalog.properties["model"]=model("#00ff00").to_json(); update(document,catalog);
    const auto recolored=prepare(document);
    check(recolored.solids.at(placed_id).content==solid.content &&
          recolored.solids.at(placed_id).appearance_content!=solid.appearance_content &&
          recolored.solids.at(placed_id).material_regions.front().material_color=="#00ff00",
          "embedded appearance updates regions while preserving geometry cache identity");
    leaf.profiles.front().height_m=1; catalog.properties["model"]=model("#00ff00").to_json(); update(document,catalog);
    const auto updated=prepare(document);
    check(updated.solids.at(placed_id).content!=solid.content &&
          std::abs(solid_volume(updated.solids.at(placed_id).shape)-88)<1e-6,
          "embedded nested type edits invalidate native geometry and change measured profile volume");
    const auto hidden=prepare_native_geometry(document.snapshot(),NativeGeometryVisibleIds{});
    check(hidden && hidden->errors.empty() && !hidden->solids.at(standalone_id).visible,
          "hidden embedded standalone profiles are still validated");
    // One catalog is locally within the cap. Their combined expansion exceeds
    // the document cap and must never publish apparently valid embedded solids.
    AssemblyType repeated=root; repeated.parts.clear();
    for(int i=0;i<2100;++i) { auto p=part; p.id="part-"+std::to_string(i); repeated.parts.push_back(p); }
    auto many=standalone; many.id="many";
    many.nested_overrides.clear();
    const auto bounded=AssemblyModel::create({{"red","Red","#ff0000"}},{leaf,repeated},{many}).to_json();
    auto admitted=Document::create({Entity{"budget-a","assembly_model",{{"model",bounded}}}});
    const auto before_budget=admitted.snapshot();
    bool budget_refused=false;
    try {
        admitted.apply(ApplyEntityChanges{before_budget.revision(),
            {EntityChange::upsert(Entity{"budget-b","assembly_model",{{"model",bounded}}})},
            {},"Exceed aggregate embedded expansion budget"});
    } catch(const DocumentError& error) {
        budget_refused=error.code()==DocumentErrorCode::invalid_entity;
    }
    check(budget_refused && admitted.snapshot().entities()==before_budget.entities() &&
          admitted.revision()==before_budget.revision() &&
          admitted.snapshot().history().size()==before_budget.history().size(),
          "aggregate expansion is refused atomically before invalid native geometry can be published");
    auto invalid=catalog; invalid.properties["model"]["instances"][0]["type_id"]="missing";
    const auto before_invalid=document.snapshot();
    bool invalid_refused=false;
    try { update(document,invalid); }
    catch(const DocumentError& error) {
        invalid_refused=error.code()==DocumentErrorCode::invalid_entity;
    }
    check(invalid_refused && document.snapshot().entities()==before_invalid.entities() &&
          document.revision()==before_invalid.revision() &&
          document.snapshot().history().size()==before_invalid.history().size(),
          "invalid embedded type references preserve the valid native source without a host fallback");
}

void test_independent_assembly_profiles() {
    const Boundary rectangle{{{0,0},{2,0}},{{2,0},{2,1}},{{2,1},{0,1}},{{0,1},{0,0}}};
    AssemblyType leaf; leaf.id = "leaf"; leaf.name = "Leaf";
    leaf.materials["finish"] = "red";
    leaf.profiles.push_back({"panel", rectangle, {}, 0.0, 0.2, "finish"});
    AssemblyType pair; pair.id = "pair"; pair.name = "Pair";
    AssemblyPart first; first.id = "a"; first.type_id = "leaf";
    AssemblyPart second; second.id = "b"; second.type_id = "leaf";
    second.transform.translation_m = {3,0,0}; second.material_overrides["finish"] = "blue";
    pair.parts = {first, second};
    Entity catalog{"independent-catalog", "assembly_model", {{"model", AssemblyModel::create(
        {{"red","Red","#ff0000"},{"blue","Blue","#0000ff"}}, {leaf,pair}, {}).to_json()}}};
    AssemblyInstance instance; instance.id = "independent"; instance.type_id = "pair";
    instance.root_transform = AssemblyTransform{{10,20,1},0,2};
    auto entity = encode_document_assembly_instance(Entity{instance.id,"assembly_instance"},
        {catalog.id,instance});
    auto document = Document::create({catalog,entity});
    const auto source = document.snapshot();
    const auto initial = prepare(document);
    check(initial.pending.empty() && initial.solids.size() == 1 && initial.solids.contains(entity.id),
          "independent assembly must publish one semantic solid without host or catalog duplicates");
    const auto& solid = initial.solids.at(entity.id);
    check_meshed(solid.shape);
    check(std::abs(solid_volume(solid.shape) - 6.4) < 1e-6 && solid.material_regions.size() == 2,
          "two transformed profiles contribute their actual cubic volume exactly once");
    check(solid.material_regions[0].source_id != solid.material_regions[1].source_id &&
          solid.material_regions[0].catalog_id == catalog.id &&
          solid.material_regions[0].material_id == "red" && solid.material_regions[1].material_id == "blue",
          "each nested profile retains stable independent provenance and resolved material override");
    for (const auto& region : solid.material_regions) {
        check_meshed(region.shape);
        check(std::abs(region.net_volume - 3.2) < 1e-6 && region.net_volume == region.gross_volume,
              "assembly regions retain actual profile volume without roof overlap deductions");
    }
    // Measure geometric extrema rather than triangulation deflection padding.
    Bnd_Box bounds; BRepBndLib::AddOptimal(solid.shape,bounds,false,false);
    double x0,y0,z0,x1,y1,z1; bounds.Get(x0,y0,z0,x1,y1,z1);
    check(std::abs(x0-10) < 1e-6 && std::abs(x1-20) < 1e-6 &&
          std::abs(y0-20) < 1e-6 && std::abs(y1-22) < 1e-6 &&
          std::abs(z0-1) < 1e-6 && std::abs(z1-1.4) < 1e-6,
          "independent assembly uses profile geometry and composed XYZ placement");
    catalog.properties["model"] = AssemblyModel::create(
        {{"red","Red","#00ff00"},{"blue","Blue"}}, {leaf,pair}, {}).to_json();
    update(document,catalog);
    const auto recolored = prepare(document);
    const auto& changed = recolored.solids.at(entity.id);
    check(changed.content == solid.content && changed.appearance_content != solid.appearance_content &&
          changed.material_regions[0].source_id == solid.material_regions[0].source_id &&
          changed.material_regions[0].material_color == "#00ff00" &&
          !changed.material_regions[1].material_color,
          "catalog color edits change appearance alone and clear every obsolete profile color");
    const auto hidden = prepare_native_geometry(document.snapshot(),NativeGeometryVisibleIds{});
    check(hidden && hidden->errors.empty() && !hidden->solids.at(entity.id).visible,
          "hidden independent assemblies remain validated without displaying profiles separately");
    instance.root_transform->translation_m.z = 4;
    entity = encode_document_assembly_instance(entity,{catalog.id,instance}); update(document,entity);
    check(prepare(document).solids.at(entity.id).content != changed.content,
          "semantic root placement must invalidate native profile geometry");
    check(source.entities().at(entity.id).properties.at("instance").at("root_transform") !=
          document.snapshot().entities().at(entity.id).properties.at("instance").at("root_transform"),
          "geometry preparation must retain the original immutable snapshot");
    const auto translated = prepare(document);
    leaf.profiles.front().height_m = 0.4;
    catalog.properties["model"] = AssemblyModel::create(
        {{"red","Red","#00ff00"},{"blue","Blue"}}, {leaf,pair}, {}).to_json();
    update(document,catalog);
    const auto resized = prepare(document);
    check(resized.solids.at(entity.id).content != translated.solids.at(entity.id).content &&
          std::abs(solid_volume(resized.solids.at(entity.id).shape)-12.8) < 1e-6,
          "nested type profile edits invalidate cached native geometry and regenerate every repeated profile");
    std::size_t progress = 0;
    const auto cancelled = prepare_native_geometry(document.snapshot(),std::nullopt,
        [&] { return progress != 0; }, [&](std::size_t) { ++progress; });
    check(!cancelled && progress == 1, "cancelling after assembly meshing discards the complete candidate");
}

void test_empty_independent_assembly_reports_geometry_failure() {
    AssemblyType empty; empty.id = "empty"; empty.name = "Quantity-only type";
    Entity catalog{"empty-catalog","assembly_model",{{"model",AssemblyModel::create({}, {empty}, {}).to_json()}}};
    AssemblyInstance instance; instance.id = "empty-instance"; instance.type_id = empty.id;
    instance.root_transform = AssemblyTransform{};
    const auto entity = encode_document_assembly_instance(Entity{instance.id,"assembly_instance"},
        {catalog.id,instance});
    const auto document = Document::create({catalog,entity});
    for (const auto& mask : {std::optional<NativeGeometryVisibleIds>{},
                            std::optional<NativeGeometryVisibleIds>{NativeGeometryVisibleIds{}}}) {
        const auto result = prepare_native_geometry(document.snapshot(),mask);
        check(result && result->solids.empty() && result->errors.size() == 1 &&
              result->errors.front().find(instance.id) != std::string::npos,
              "missing independent profiles retain an identifiable native failure even when hidden");
    }
}

void test_roof_join_material_regions_and_color_dependencies() {
    auto a = encode_building_entity(SlopedRoofPanel{"a", {0,0,0}, 0,2,4,1,std::atan(.5),0,.1,{}});
    auto b = encode_building_entity(SlopedRoofPanel{"b", {1.9,0,.95}, 0,2,4,1,std::atan(.5),0,.1,{}});
    a.properties["material_assignment"] = {{"version",1},{"catalog_id","catalog"},{"material_id","red"}};
    b.properties["material_assignment"] = {{"version",1},{"catalog_id","catalog"},{"material_id","blue"}};
    Entity catalog{"catalog", "assembly_model", {{"model", AssemblyModel::create(
        {{"red","Red","#ff0000"},{"blue","Blue","#0000ff"}}, {}, {}).to_json()}}};
    RoofJoin semantic{"join", {"a","b"}};
    Entity join{"join", "roof_join", roof_join_json(semantic)};
    auto document = Document::create({a,b,catalog,join});
    const auto before = document.snapshot();
    const auto initial = prepare(document);
    const auto& solid = initial.solids.at("join");
    check(solid.material_regions.size() == 2 && solid.material_regions[0].source_id == "a" &&
        solid.material_regions[1].source_id == "b" &&
        solid.material_regions[0].material_color == std::optional<std::string>{"#ff0000"} &&
        solid.material_regions[1].material_color == std::optional<std::string>{"#0000ff"},
        "authored roof order and member material colors must survive preparation");
    double total = 0;
    for (const auto& region : solid.material_regions) {
        check_meshed(region.shape);
        total += solid_volume(region.shape);
    }
    check(std::abs(total-solid_volume(solid.shape)) < 1e-8,
        "meshed appearance regions conserve authoritative fused quantity");
    catalog.properties["model"] = AssemblyModel::create(
        {{"red","Red","#00ff00"},{"blue","Blue","#0000ff"}}, {}, {}).to_json();
    update(document,catalog);
    const auto recolored = prepare(document);
    check(solid.content == recolored.solids.at("join").content &&
        solid.appearance_content != recolored.solids.at("join").appearance_content &&
        recolored.solids.at("join").material_regions[0].material_color == std::optional<std::string>{"#00ff00"},
        "catalog color-only edits propagate through separate appearance identity");
    semantic.material_assignment = RoofJoinMaterialAssignment{"catalog","blue"};
    join.properties = roof_join_json(semantic);
    update(document,join);
    const auto overridden = prepare(document);
    check(std::all_of(overridden.solids.at("join").material_regions.begin(),
        overridden.solids.at("join").material_regions.end(), [](const auto& region) {
            return region.material_id == std::optional<std::string>{"blue"}; }),
        "explicit V2 assignment intentionally overrides every joined region");
    bool cancel = false;
    std::size_t completions = 0;
    const auto cancelled = prepare_native_geometry(before, std::nullopt,
        [&] { return cancel; }, [&](std::size_t) { ++completions; cancel = true; });
    check(!cancelled && completions == 1, "cancellation must discard the complete region candidate before publication");
    check(before.entities().at("a").properties == a.properties &&
        before.entities().at("b").properties == b.properties,
        "derived partition must preserve authored sources");
    b.properties.erase("material_assignment");
    update(document,b);
    semantic.material_assignment.reset(); join.properties = roof_join_json(semantic); update(document,join);
    const auto defaulted = prepare(document);
    check(!defaulted.solids.at("join").material_regions[1].material_id &&
        !defaulted.solids.at("join").material_regions[1].material_color,
        "unassigned source retains default roof appearance");
    bool refused = false;
    try {
        auto missing = a; missing.properties["material_assignment"]["material_id"] = "missing";
        (void)Document::create({missing,b,catalog,join});
    } catch (const std::exception&) { refused = true; }
    check(refused, "missing material reference must fail without a partial document");
    refused = false;
    try { (void)AssemblyModel::create({{"invalid","Invalid","not-a-color"}}, {}, {}); }
    catch (const std::exception&) { refused = true; }
    check(refused, "invalid catalog colors must fail before native publication");
}

void test_resolved_join_and_opening_identity() {
    auto wall = walls(1).snapshot().entities().begin()->second;
    wall.id = "wall-a";
    wall.properties["layer_id"] = "layer";
    wall.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}};
    auto second = wall;
    second.id = "wall-b";
    second.properties["baseline"] = {{"start", {4.0, 0.0}}, {"end", {4.0, 3.0}}, {"sweep_radians", 0.0}};
    const auto graph = VerticalLevelGraph({{"ground", 0.0}});
    Entity levels{"levels", "vertical_levels", {{"model", nlohmann::json::parse(graph.serialize())}}};
    Entity opening{"opening", "opening", {
        {"wall_id", wall.id}, {"opening_kind", "window"},
        {"offset_m", 1.0}, {"width_m", 1.0},
        {"sill_m", 0.2}, {"height_m", 2.1},
        {"opening_assembly", opening_assembly_json(default_opening_assembly(OpeningAssemblyKind::window))}}};
    auto document = Document::create({
        {"site", "property", nlohmann::json::object()},
        {"building", "building", {{"property_id", "site"}}},
        {"floor", "floor", {{"building_id", "building"},
            {"vertical_level_binding", {{"version", 1}, {"graph_id", "levels"}, {"level_id", "ground"}}}}},
        {"layer", "layer", {{"floor_id", "floor"}}}, levels, wall, second, opening,
        {"join", "wall_join", {{"version", 1}, {"style", "fused"}, {"wall_ids", {wall.id, second.id}}}}});
    const auto initial = prepare(document);
    check_meshed(initial.solids.at("join").shape);
    check_meshed(initial.solids.at("opening").shape);
    levels.properties["model"] = nlohmann::json::parse(graph.with_elevation("ground", 2.0).serialize());
    update(document, levels);
    const auto raised = prepare(document);
    check(initial.solids.at("join").content != raised.solids.at("join").content,
          "join identity must include resolved elevations of members, not only their stored properties");
    check(initial.solids.at("opening").content != raised.solids.at("opening").content,
          "opening assembly identity must include resolved host elevation");
    opening.properties["width_m"] = 1.2;
    update(document, opening);
    const auto widened = prepare(document);
    check(raised.solids.at("join").content != widened.solids.at("join").content &&
          raised.solids.at("opening").content != widened.solids.at("opening").content,
          "hosted opening geometry must invalidate both join and opening assembly identities");
}

void test_terrain_identity() {
    const auto model = [](double elevation) {
        return TerrainSurface("terrain", {{"a", 0, 0, 0}, {"b", 2, 0, elevation}, {"c", 0, 2, 0}},
            {TerrainTriangle{{0, 1, 2}}}).to_json();
    };
    auto terrain = Entity::create("terrain_surface", {{"model", model(0)}});
    auto document = Document::create({terrain});
    const auto before = prepare(document);
    check_meshed(before.solids.at(terrain.id).shape);
    terrain.properties["model"] = model(1);
    update(document, terrain);
    check(before.solids.at(terrain.id).content != prepare(document).solids.at(terrain.id).content,
          "terrain point elevation must invalidate prepared geometry identity");
}

void test_hosted_stair_dependencies(bool landing_guard=false) {
    StairFlight stair{"stair", {0, 0, 0}, 0, 8, 2.0, 0.25, 1.0};
    stair.flights = {{"lower", 4}, {"upper", 4}};
    stair.landings = {{"turn", 1.0, 0.15, StairTurn::left_quarter, 0.0}};
    auto source = encode_building_entity(stair);
    architectural_context(source);
    source.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.25}};
    Railing railing{"rail", {}, 0, 0, 0.9, 0.05, 0.5};
    railing.host = StairRailingHost{"stair", "upper", StairRailingSide::left, 0, 1};
    if(landing_guard) {
        railing.host.reset();
        railing.landing_host=StairLandingRailingHost{"stair",StairLandingRole::connecting,"turn","lower","upper",0,0,1};
    }
    auto rail = encode_building_entity(railing);
    architectural_context(rail);
    Entity levels{"levels", "vertical_levels", {{"model", nlohmann::json::parse(
        VerticalLevelGraph({{"ground", 3.0}}).serialize())}}};
    AssemblyInstance instance{"copy", "type", {}, {}, {}, AssemblyPlacement{"rail"}};
    const auto catalog = Entity::create("assembly_model", {{"model", AssemblyModel::create(
        {}, {{"type", "Rail copy", {}, {}, {}}}, {instance}).to_json()}});
    auto document = Document::create({
        {"site", "property", nlohmann::json::object()},
        {"building", "building", {{"property_id", "site"}}},
        {"floor", "floor", {{"building_id", "building"}, {"vertical_level_binding",
            {{"version", 1}, {"graph_id", "levels"}, {"level_id", "ground"}}}}},
        {"layer", "layer", {{"floor_id", "floor"}}}, levels, source, rail, catalog});
    const auto before = document.snapshot();
    const auto initial = prepare(document);
    check_meshed(initial.solids.at("rail").shape);
    check(document.snapshot().entities() == before.entities() &&
          document.revision() == before.revision() &&
          document.snapshot().history().size() == before.history().size() &&
          document.snapshot().history().front().entities == before.history().front().entities,
          "host derivation must preserve exact authored entities and history");
    Bnd_Box box;
    BRepBndLib::Add(initial.solids.at("rail").shape, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    check(z0 > 4.2 && z0 < 4.6, "hosted rail must use its raised upper flight, exactly once");
    levels.properties["model"] = nlohmann::json::parse(
        VerticalLevelGraph({{"ground", 5.0}}).serialize());
    update(document, levels);
    const auto raised = prepare(document);
    const auto child = catalog.id + ":instance:copy";
    check(initial.solids.at("rail").content != raised.solids.at("rail").content &&
          initial.solids.at(child).content != raised.solids.at(child).content,
          "level changes invalidate unchanged rail and assembly-host identities");
    stair.going = 0.35;
    auto changed = encode_building_entity(stair);
    architectural_context(changed);
    changed.properties["vertical_placement"] = source.properties["vertical_placement"];
    update(document, changed);
    const auto extended = prepare(document);
    check(raised.solids.at("rail").content != extended.solids.at("rail").content &&
          raised.solids.at(child).content != extended.solids.at(child).content,
          "flight geometry changes invalidate hosted rails and their assembly copies");
    const auto before_retirement = document.snapshot();
    bool host_retirement_refused = false;
    try {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase("stair")}, {},
            "remove hosted stair"});
    } catch (const std::exception&) {
        host_retirement_refused = true;
    }
    const auto after_retirement = document.snapshot();
    check(host_retirement_refused && after_retirement.entities() == before_retirement.entities() &&
          after_retirement.revision() == before_retirement.revision() &&
          after_retirement.history().size() == before_retirement.history().size(),
          "document admission must refuse host retirement before producing a missing-host projection");
    check(before.entities().at("rail") == rail && before.entities().at("stair") == source,
          "derived preparation preserves captured authored sources");
}

void test_opening_assembly_obeys_its_drawing_layer_and_floor() {
    auto wall = walls(1).snapshot().entities().begin()->second;
    wall.id = "wall";
    wall.properties["layer_id"] = "walls";
    Entity opening{"opening","opening",{{"wall_id","wall"},{"layer_id","windows"},
        {"opening_kind","window"},{"offset_m",1.0},{"width_m",1.0},
        {"sill_m",0.2},{"height_m",2.1},
        {"opening_assembly",opening_assembly_json(default_opening_assembly(OpeningAssemblyKind::window))}}};
    const auto document = Document::create({
        {"site","property",nlohmann::json::object()},
        {"building","building",{{"property_id","site"}}},
        {"floor","floor",{{"building_id","building"}}},
        {"walls","layer",{{"floor_id","floor"}}},
        {"windows","layer",{{"floor_id","floor"}}},wall,opening});
    const auto source = document.snapshot();
    const auto with_filter = [&](ProjectViewFilter filter) {
        auto prepared = prepare_native_geometry(source,visible_project_entities(source,filter));
        check(prepared && prepared->errors.empty() && prepared->solids.contains("opening"),
            "filtered native window must retain valid prepared geometry and host relationship");
        return std::move(*prepared);
    };
    ProjectViewFilter filter;
    filter.hidden_layer_ids.insert("windows");
    const auto hidden_window = with_filter(filter);
    check(hidden_window.solids.at("wall").visible && !hidden_window.solids.at("opening").visible,
        "a visible host cannot override the hidden opening assembly layer");
    filter.hidden_layer_ids = {"walls"};
    const auto hidden_host = with_filter(filter);
    check(!hidden_host.solids.at("wall").visible && hidden_host.solids.at("opening").visible,
        "independent opening assembly layer remains visible when wall layer is hidden");
    filter.hidden_floor_ids.insert("floor");
    const auto hidden_floor = with_filter(filter);
    check(!hidden_floor.solids.at("wall").visible && !hidden_floor.solids.at("opening").visible,
        "shared floor mask hides both actual native solids");
    check(document.snapshot().entities() == source.entities(), "native visibility does not alter source geometry");
}

void test_worker_failure_recovery() {
    NativeGeometryRegenerator worker([](const DocumentSnapshot& snapshot,
        std::optional<NativeGeometryVisibleIds> visible, const std::function<bool()>&) {
        if (!visible) throw std::runtime_error("injected preparation failure");
        return std::optional<PreparedNativeGeometry>{PreparedNativeGeometry{snapshot.revision(), visible}};
    });
    auto document = walls(1);
    worker.request(document.snapshot(), std::nullopt);
    bool failed = false;
    try { (void)await_result(worker); }
    catch (const std::runtime_error& error) { failed = std::string(error.what()) == "injected preparation failure"; }
    check(failed && !worker.is_pending(), "worker error must reach owner and clear pending state");
    worker.request(document.snapshot(), NativeGeometryVisibleIds{});
    check(await_result(worker).has_value(), "worker must recover after a preparation exception");
    worker.shutdown();
    bool rejected = false;
    try { worker.request(document.snapshot(), std::nullopt); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "requests after shutdown must be rejected");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_coalescing();
        test_ifc_provenance_does_not_block_native_geometry();
        test_unresolved_ifc_references_remain_pending();
        test_assembly_identity();
        test_embedded_assembly_profiles();
        test_independent_assembly_profiles();
        test_empty_independent_assembly_reports_geometry_failure();
        test_roof_join_material_regions_and_color_dependencies();
        test_resolved_join_and_opening_identity();
        test_opening_assembly_obeys_its_drawing_layer_and_floor();
        test_terrain_identity();
        test_hosted_stair_dependencies();
        test_hosted_stair_dependencies(true);
        test_worker_failure_recovery();
        auto document = walls(12);
        const auto before = document.snapshot();
        bool cancel = false;
        std::size_t completed = 0;
        auto cancelled = prepare_native_geometry(before, std::nullopt,
            [&] { return cancel; }, [&](std::size_t count) {
                completed = count;
                if (count == 2) cancel = true;
            });
        check(!cancelled && completed == 2,
              "cancellation must discard real partial multi-object geometry");
        {
            WorkspaceRegenerationQueue queue;
            std::promise<void> two_shapes;
            auto reached = two_shapes.get_future();
            std::atomic<bool> partial_discarded{false};
            const auto owner_thread = std::this_thread::get_id();
            const auto sequence = queue.enqueue([&](const RegenerationCancellationToken& token) {
                auto result = prepare_native_geometry(before, std::nullopt,
                    [&] { return token.is_cancelled(); }, [&](std::size_t count) {
                        check(std::this_thread::get_id() != owner_thread,
                              "real geometry must run off the owner thread");
                        if (count == 2) {
                            two_shapes.set_value();
                            while (!token.is_cancelled()) std::this_thread::yield();
                        }
                    });
                partial_discarded = !result;
                return RegenerationReceipt{before.revision(), {}};
            });
            const auto started = reached.wait_for(std::chrono::seconds(30));
            if (started != std::future_status::ready) {
                (void)queue.cancel(sequence);
                queue.shutdown(false);
                throw std::runtime_error("worker did not construct two real solids");
            }
            check(queue.cancel(sequence), "running real geometry must accept cancellation");
            queue.shutdown(false);
            const auto completions = queue.take_completed();
            check(partial_discarded && completions.size() == 1 &&
                  completions.front().kind == RegenerationCompletionKind::cancelled &&
                  !completions.front().receipt,
                  "cancelled multi-object worker must not publish partial geometry");
        }
        check(document.revision() == before.revision() &&
              document.snapshot().entities().size() == before.entities().size() &&
              document.snapshot().history().size() == before.history().size(),
              "preparation must preserve document and revision");

        NativeGeometryRegenerator worker;
        worker.request(before, std::nullopt);
        auto valid = await_result(worker);
        check(valid && valid->solids.size() == 12 && valid->errors.empty(),
              "initial scene must contain all real solids");
        for (const auto& [id, solid] : valid->solids) {
            (void)id;
            check(!solid.shape.IsNull(), "prepared shape must be a real OCCT solid");
            check_meshed(solid.shape);
        }
        const auto old_count = valid->solids.size();
        auto edited_wall = before.entities().begin()->second;
        edited_wall.properties["height_m"] = 4.0;
        document.apply(ApplyEntityChanges{document.revision(),
            {EntityChange::upsert(edited_wall)}, {}, "geometry revision regression"});
        const auto edited = document.snapshot();
        worker.request(before, std::nullopt);
        worker.request(edited, std::nullopt);
        auto revised = await_result(worker);
        check(revised && revised->revision == edited.revision() &&
              revised->revision != before.revision(),
              "a superseded revision must never become the accepted scene");
        check(document.snapshot().history().size() == edited.history().size() &&
              document.snapshot().entities().at(edited_wall.id).properties == edited_wall.properties,
              "preparation must preserve the exact authoritative entity and history");
        auto replacement = walls(3);
        worker.request(before, std::nullopt);
        worker.request(replacement.snapshot(), NativeGeometryVisibleIds{});
        check(valid->solids.size() == old_count,
              "pending replacement must preserve previous valid scene");
        auto newest = await_result(worker);
        check(newest && newest->solids.size() == 3 && newest->visible_ids &&
              newest->visible_ids->empty(),
              "same-revision replacement and visibility must suppress stale output");
        for (const auto& [id, solid] : newest->solids) {
            check(replacement.snapshot().entities().contains(id) && !solid.visible,
                  "latest immutable snapshot and visibility must win");
        }
        worker.request(before, std::nullopt);
        worker.shutdown();
        check(!worker.is_pending() && !worker.take_completed(),
              "shutdown must discard in-flight output");
        check(document.revision() == edited.revision(), "worker must not change history");
        std::cout << "native geometry preparation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
