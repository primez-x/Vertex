#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/redraw_angle_fixture.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void test_appraisal_reporting_export_floor(const std::filesystem::path& root) {
  auto property = sketch::Entity::create("property", {{"name", "Report property"}});
  auto document = sketch::Document::create({property});
  property.properties["appraisal_reporting"] = {{"version", 1}, {"contract", "uad_3_6"},
      {"room_inventory_complete", false}};
  document.apply(sketch::ApplyEntityChanges{document.revision(), {sketch::EntityChange::upsert(property)}, {}, "Declare reporting"});
  const auto changed = document.snapshot();
  auto deleted = sketch::Document::fork(changed);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(), {sketch::EntityChange::erase(property.id)}, {}, "Delete reported owner"});
  document.undo(document.revision());
  unsigned index = 0;
  for (const auto& snapshot : {changed, document.snapshot(), deleted.snapshot()}) {
    const auto output = root / ("reporting-v54-" + std::to_string(index++));
    sketch::extract_project(snapshot, output);
    std::ifstream input(output / "project.json");
    const auto encoded = nlohmann::json::parse(input);
    check(encoded.at("exchange_version") == 54 && encoded.at("revisions").size() == snapshot.history().size(),
        "current, undone and deleted reporting facts require exchange54 with complete retained history");
  }
}

void test_translation_export(const std::filesystem::path& root) {
  sketch::BoundaryConstructionRecord record;
  record.schema_version = sketch::boundary_receipt_schema_version_v2;
  record.boundary_id = "boundary";
  const sketch::Vec2 points[]{{0, 0}, {2, 0}, {2, 1}, {0, 1}};
  sketch::IdentifiedBoundary boundary{record.boundary_id, "measurement_boundary", {}};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto edge = "edge-" + std::to_string(i);
    const auto start = "vertex-" + std::to_string(i);
    const auto end = "vertex-" + std::to_string((i + 1) % 4);
    sketch::ConstructionReceipt receipt;
    receipt.segment_id = edge;
    receipt.kind = sketch::BoundaryConstructionKind::line_to_point;
    receipt.start = points[i]; receipt.chord_end = points[(i + 1) % 4];
    record.edges.push_back({edge, start, end, receipt});
    boundary.segments.push_back({edge, start, end, {points[i], points[(i + 1) % 4], 0}});
  }
  auto entity = sketch::encode_identified_boundary_entity(boundary);
  entity.properties["boundary_authoring"] = sketch::encode_boundary_receipt_envelope(record);
  auto document = sketch::Document::create({entity});
  document.apply(sketch::TranslateBoundary{0, {"boundary", {8, -4}}});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "translation");
  std::ifstream input(root / "translation" / "project.json");
  const auto json = nlohmann::json::parse(input);
  check(json.at("exchange_version") == 2, "translation export must advertise its proof schema");
  const auto& rows = json.at("revisions");
  check(rows.size() == 3 && !rows[0].contains("boundary_translation") &&
        !rows[2].contains("boundary_translation"), "proof belongs only to its command revision");
  const auto proof = sketch::decode_boundary_translation(rows[1].at("boundary_translation"));
  check(proof.boundary_id == "boundary" && proof.offset.x == 8 && proof.offset.y == -4,
        "export must retain proof even in undone history");
  document.redo(document.revision());
  sketch::PlanarTransform transform;
  transform.pivot = {1, 0.5}; transform.rotation_radians = 0.25;
  transform.flip_horizontal = true; transform.offset = {2, 3};
  document.apply(sketch::TransformBoundary{document.revision(), {"boundary", transform}});
  // A later translation must not overwrite the newer exchange version.
  document.apply(sketch::TranslateBoundary{document.revision(), {"boundary", {1, 2}}});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "transform");
  std::ifstream transformed_input(root / "transform" / "project.json");
  const auto transformed_json = nlohmann::json::parse(transformed_input);
  check(transformed_json.at("exchange_version") == 3, "mixed transform export must advertise version3");
  const auto& transformed_rows = transformed_json.at("revisions");
  check(transformed_rows[4].at("boundary_transform") ==
        sketch::encode_boundary_transform({"boundary", transform}), "export must preserve every transform parameter");
  check(!transformed_rows[5].contains("boundary_transform") &&
        transformed_rows[5].contains("boundary_translation") &&
        !transformed_rows[6].contains("boundary_transform"), "export must keep proof roles separate");

  sketch::BoundaryGeometryEdit edit;
  edit.boundary_id = "boundary";
  edit.kind = sketch::BoundaryGeometryEditKind::move_vertex;
  edit.target_id = "vertex-1";
  const auto current = sketch::decode_identified_boundary_entity(
      document.snapshot().entities().at("boundary"));
  edit.target_position = current.segments[1].segment.start;
  edit.target_position.x += 0.25;
  document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "geometry-edit");
  std::ifstream edit_input(root / "geometry-edit" / "project.json");
  const auto edit_json = nlohmann::json::parse(edit_input);
  check(edit_json.at("exchange_version") == 4,
        "boundary edit export must advertise version 4");
  const auto& edit_rows = edit_json.at("revisions");
  check(edit_rows[7].at("boundary_geometry_edit") ==
            sketch::encode_boundary_geometry_edit(edit) &&
        !edit_rows[8].contains("boundary_geometry_edit"),
        "exchange must preserve an undone boundary edit proof on only its command revision");
  sketch::ApplyBoundaryConstraintChanges transaction{document.revision(), {edit}, {}, "constrained edit"};
  document.apply(transaction);
  document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "constraint-edit");
  std::ifstream constraint_input(root / "constraint-edit" / "project.json");
  const auto constraint_json = nlohmann::json::parse(constraint_input);
  check(constraint_json.at("exchange_version") == 5,
        "later geometry edit must not downgrade constraint exchange version");
  check(constraint_json.at("revisions")[9].at("boundary_constraint_changes") ==
            sketch::command_to_json(transaction) &&
        !constraint_json.at("revisions")[10].contains("boundary_constraint_changes"),
        "exchange must preserve exact typed constraint proof only on its command row");

  sketch::Entity label{"group-label", "label", {{"text", "moved with boundary"}},
                       false, {{"vendor", "retain"}}};
  sketch::TranslateBoundaries batch{document.revision(), {{"boundary", {4, -2}}},
      {sketch::EntityChange::upsert(label)}, "Move measured group"};
  const auto batch_revision = document.apply(batch);
  const auto moved_boundary = sketch::decode_identified_boundary_entity(
      document.snapshot().entities().at("boundary"));
  sketch::BoundaryGeometryEdit later_edit;
  later_edit.boundary_id = "boundary";
  later_edit.kind = sketch::BoundaryGeometryEditKind::move_vertex;
  later_edit.target_id = "vertex-3";
  later_edit.target_position = moved_boundary.segments[3].segment.start;
  later_edit.target_position.x -= 0.1;
  sketch::ApplyBoundaryConstraintChanges later_constraint{
      document.revision(), {later_edit}, {}, "Later constrained edit"};
  const auto constraint_revision = document.apply(later_constraint);
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "translation-group");
  std::ifstream batch_input(root / "translation-group" / "project.json");
  const auto batch_json = nlohmann::json::parse(batch_input);
  check(batch_json.at("exchange_version") == 6,
        "later constraint row must not downgrade translation group exchange version");
  check(batch_json.at("revisions")[batch_revision].at("boundary_translations") == sketch::command_to_json(batch) &&
        batch_json.at("revisions")[constraint_revision].at("boundary_constraint_changes") == sketch::command_to_json(later_constraint) &&
        !batch_json.at("revisions").back().contains("boundary_translations"),
        "exchange must retain exact mixed proofs only on their command rows");
}

void test_grouped_measured_region_exchange(const std::filesystem::path& root) {
  using Json=nlohmann::json;
  sketch::IdentifiedBoundary geometry{"grouped-area","measurement_boundary",{
      {"e0","v0","v1",{{0,0},{4,0},0}},{"e1","v1","v2",{{4,0},{4,4},0}},
      {"e2","v2","v3",{{4,4},{0,4},0}},{"e3","v3","v0",{{0,4},{0,0},0}}}};
  auto area=sketch::encode_identified_boundary_entity(geometry);
  // An understood top-level group with stale member evidence remains
  // editable; extraction must preserve that evidence without qualifying it.
  area.extensions["measurement_linework_group"]={{"version",1},{"members",Json::array({"stale first member","stale second member"})}};
  auto document=sketch::Document::create({area}); const auto head=document.snapshot();
  document.apply(sketch::ApplyEntityChanges{.expected_revision=document.revision(),
      .entity_changes={sketch::EntityChange::erase(area.id)},.message="Delete grouped exchange area"});
  const auto deleted=document.snapshot(); document.undo(document.revision());
  unsigned sequence=0;
  for (const auto& snapshot:{head,deleted,document.snapshot()}) {
    const auto destination=root/("grouped-region-history-"+std::to_string(sequence++));
    sketch::extract_project(snapshot,destination); std::ifstream input(destination/"project.json"); const auto encoded=Json::parse(input);
    check(encoded.at("exchange_version")==31 && encoded.at("revisions").size()==snapshot.history().size(),
        "group marker in current, deleted and undone history requires extraction31");
    const auto& first=encoded.at("revisions")[0].at("entities")[0];
    check(first.at("extensions").dump()==area.extensions.dump(),"exchange preserves retained grouped member JSON exactly");
  }
  for (unsigned fault=0;fault<3;++fault) {
    auto opaque=area;
    if (fault==0) opaque.extensions["measurement_linework_group"]["version"]=999;
    if (fault==1) opaque.extensions["measurement_linework_group"]="opaque malformed marker";
    if (fault==2) opaque.extensions["measurement_linework_group"]=nullptr;
    auto future=sketch::Document::create({opaque});
    const auto destination=root/("opaque-grouped-region-"+std::to_string(fault)); sketch::extract_project(future.snapshot(),destination);
    std::ifstream input(destination/"project.json"); const auto encoded=Json::parse(input);
    check(encoded.at("exchange_version")==31 && !encoded.at("document").at("editable").get<bool>() &&
        encoded.at("revisions")[0].at("entities")[0].at("extensions").dump()==opaque.extensions.dump(),
        "unsupported group without outer lineage keeps extraction31, read-only policy and exact opaque JSON");
  }
  auto vendor=sketch::Entity{"vendor-group","property",Json::object(),false,area.extensions};
  const auto collision=sketch::Document::create({vendor}); const auto destination=root/"vendor-group-collision";
  sketch::extract_project(collision.snapshot(),destination); std::ifstream input(destination/"project.json"); const auto encoded=Json::parse(input);
  check(encoded.at("exchange_version")==1 && encoded.at("document").at("editable").get<bool>() &&
      encoded.at("revisions")[0].at("entities")[0].at("extensions").dump()==vendor.extensions.dump(),
      "vendor group collision retains historical extraction floor and exact editable payload");
}
void test_svg_palette_export_floor(const std::filesystem::path& root) {
  const auto catalog=sketch::default_symbol_catalog();
  const auto definition=std::find_if(catalog.begin(),catalog.end(),[](const auto& value){return value.svg_asset.has_value();});
  check(definition!=catalog.end(),"palette export fixture has SVG definition");
  sketch::SymbolInstance symbol{"palette-symbol",definition->id};symbol.definition=*definition;symbol.svg_palette=sketch::SymbolSvgPalette{};
  sketch::AnnotationState state;state.symbols.push_back(symbol);
  auto owner=sketch::make_annotation_entity("palette-owner",state);auto document=sketch::Document::create({});
  document.apply(sketch::ApplyEntityChanges{0,{sketch::EntityChange::upsert(owner)},{},"SVG colors"});
  const auto head=document.snapshot();auto deleted=sketch::Document::fork(head);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(),{sketch::EntityChange::erase(owner.id)},{},"Delete palette"});
  document.undo(document.revision());int sequence=0;
  for(const auto& snapshot:{head,document.snapshot(),deleted.snapshot()}) {
    const auto destination=root/("svg-palette-"+std::to_string(sequence++));sketch::extract_project(snapshot,destination);
    std::ifstream input(destination/"project.json");const auto encoded=nlohmann::json::parse(input);
    check(encoded.at("exchange_version")==23 && encoded.at("revisions").size()==snapshot.history().size(),"all palette history requires extraction23");
  }
}
void test_view_appearance_export_floor(const std::filesystem::path& root) {
  sketch::CoordinatedView plan{"plan", "Plan"};
  sketch::DrawingSheet sheet; sheet.id = "sheet"; sheet.number = "A101";
  auto graph = sketch::make_sheet_view_entity("views", sketch::SheetViewModel::create({plan}, {sheet}));
  auto document = sketch::Document::create({});
  graph.properties["model"]["version"] = 7;
  graph.properties["model"]["views"][0]["presentation"]["appearance"] = {
      {"visible", false}, {"style", nullptr}, {"objects", nlohmann::json::array()}};
  document.apply(sketch::ApplyEntityChanges{0, {sketch::EntityChange::upsert(graph)}, {}, "Hide output view"});
  const auto head = document.snapshot();
  auto deleted = sketch::Document::fork(head);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(), {sketch::EntityChange::erase(graph.id)}, {}, "Delete view"});
  document.undo(document.revision());
  int sequence = 0;
  for (const auto& snapshot : {head, document.snapshot(), deleted.snapshot()}) {
    const auto destination = root / ("view-appearance-" + std::to_string(sequence++));
    sketch::extract_project(snapshot, destination);
    std::ifstream input(destination / "project.json");
    const auto encoded = nlohmann::json::parse(input);
    check(encoded.at("exchange_version") == 22 && encoded.at("revisions").size() == snapshot.history().size(),
        "current, undone and deleted view appearance retains extraction22 and history");
  }
}

void test_selected_rigid_curve_exchange_v25(const std::filesystem::path& root, bool compact = false) {
  const auto curve = sketch::arc_from_chord_arc_length({0,0},{4,0},5.0,false);
  const auto angle = sketch::angle_from_radians(curve.sweep_radians);
  sketch::Entity source{"selected-rigid-curve","wall",{{"baseline",{{"start",{0,0}},{"end",{4,0}},
      {"sweep_radians",curve.sweep_radians},{"vendor","original geometry"}}},{"thickness_m",0.14},
      {"height_m",2.4},{"elevation_m",0}},false,{{"curve_input",{{"version",2},{"construction","arc_length"},
      {"measure","5 m"},{"normalized_measure","5 m"},{"measure_value",5.0},{"clockwise",false},
      {"start",{0,0}},{"end",{4,0}},{"sweep",angle.original_expression},{"normalized_sweep",angle.normalized_expression},
      {"radians",curve.sweep_radians},{"vendor",{{"exact","original input"}}}}}}};
  auto neighbor = source; neighbor.id = "selected-rigid-neighbor"; neighbor.extensions = nlohmann::json::object();
  neighbor.properties["baseline"] = {{"start",{4,0}},{"end",{8,0}},{"sweep_radians",0.0}};
  const sketch::PersistentConstraint join{"selected-rigid-join",sketch::ConstraintRelationKind::coincident,
      {{source.id,sketch::WallEndpointRole::end},{neighbor.id,sketch::WallEndpointRole::start}}};
  const sketch::Entity opening{"selected-rigid-opening","opening",{{"wall_id",source.id},{"offset_m",1.0},
      {"width_m",0.5},{"sill_m",0.0},{"height_m",2.0}},false,{{"vendor","exact host"}}};
  std::vector<sketch::Entity> values{source,neighbor,opening,sketch::encode_constraint_entity(join)};
  std::vector<sketch::Asset> assets;
  if (compact) {
    values.push_back({"rigid-exchange-label","label",{{"text","Before"}},false,nlohmann::json::object()});
    values.push_back({"rigid-exchange-object","object",{{"asset_id","rigid-exchange-image"}},false,nlohmann::json::object()});
    assets.push_back(sketch::Asset::create("rigid-exchange-image","application/octet-stream",{std::byte{1}},{{"caption","Before"}}));
  }
  auto document = sketch::Document::create(values,assets);
  const auto before = document.snapshot();
  const sketch::PlanarTransform transform{{2,0},0.2,false,false,{1,1}};
  const auto target = sketch::transform_segment(curve,transform);
  sketch::ApplyBoundaryConstraintChanges typed;
  typed.expected_revision=before.revision();
  typed.wall_edits={{source.id,target,std::nullopt,4,transform},
      {neighbor.id,{target.end,{8,0},0},std::nullopt,1}};
  typed.rigid_wall_transform_completion=true;
  sketch::Command command=typed;
  if (compact) {
    auto& proof = std::get<sketch::ApplyBoundaryConstraintChanges>(command);
    auto label = before.entities().at("rigid-exchange-label"); label.properties["text"] = "After";
    proof.supplemental_entity_changes.push_back(sketch::EntityChange::upsert(label));
    proof.supplemental_asset_changes.push_back(sketch::AssetChange::upsert(sketch::Asset::create("rigid-exchange-image",
        "application/octet-stream",std::vector<std::byte>(530*1024,std::byte{42}),{{"caption","After"},{"vendor",{{"exact",true}}}})));
    proof.supplemental_source_completion = true; proof.supplemental_asset_reference_completion = true;
  }
  const auto wire = sketch::command_to_json(command);
  check(wire.at("version")==10 && wire.at("wall_edits")[0].at("version")==4 &&
      wire.at("source_completion")==false,"rigid extraction fixture must retain outer10 selected proof without exterior authority");
  document.apply(command); const auto changed = document.snapshot();
  check(changed.entities().at(source.id).extensions.at("curve_input_derivation").at("source_input")==source.extensions.at("curve_input") &&
      changed.entities().at(source.id).extensions.at("curve_input_derivation").at("source_baseline")==source.properties.at("baseline") &&
      changed.entities().at(opening.id)==opening && changed.entities().at(neighbor.id)!=neighbor,
      "extracted rigid motion must retain original source and host while moving its joined neighbor");
  auto deleted = sketch::Document::fork(changed);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(),{sketch::EntityChange::erase(source.id),
      sketch::EntityChange::erase(opening.id),sketch::EntityChange::erase(join.id)}, {}, "Delete selected curve later"});
  document.undo(document.revision());
  for (const auto& snapshot : {changed,document.snapshot(),deleted.snapshot()}) {
    const auto destination = root/("selected-rigid-v25-"+sketch::make_stable_id());
    sketch::extract_project(snapshot,destination);
    std::ifstream input(destination/"project.json"); const auto exchanged = nlohmann::json::parse(input);
    const auto& rows = exchanged.at("revisions");
    check(exchanged.at("exchange_version")==25 && rows.size()==snapshot.history().size() &&
        rows[1].at("boundary_constraint_changes")==wire,
        "head, Undo and deleted selected rigid history must retain exact exchange25 proof");
    for (std::size_t i=0;i<rows.size();++i)
      check(rows[i].at("undo_stack")==nlohmann::json(snapshot.history()[i].undo_stack) &&
          rows[i].at("redo_stack")==nlohmann::json(snapshot.history()[i].redo_stack),"rigid extraction must retain exact navigation stacks");
    std::map<std::string,sketch::Asset> published;
    for (const auto& row : rows[1].at("assets")) {
      std::ifstream bytes_file(destination/row.at("path").get<std::string>(),std::ios::binary);
      const std::string payload{std::istreambuf_iterator<char>(bytes_file),std::istreambuf_iterator<char>()};
      std::vector<std::byte> bytes(payload.size());
      std::transform(payload.begin(),payload.end(),bytes.begin(),[](char value){return static_cast<std::byte>(static_cast<unsigned char>(value));});
      auto asset = sketch::Asset::create(row.at("id").get<std::string>(),row.at("media_type").get<std::string>(),std::move(bytes),row.at("metadata"));
      check(asset.sha256==row.at("sha256").get<std::string>(),"rigid result asset bytes must retain their exact published identity");
      if (compact) check(payload==std::string(530*1024,'*'),"rigid compact extraction must publish all exact supplemental bytes");
      published.emplace(asset.id,std::move(asset));
    }
    const auto resolver = [&published](std::string_view id)->const sketch::Asset* {
      const auto found=published.find(std::string(id)); return found==published.end()?nullptr:&found->second;
    };
    if (compact) check(wire.dump().size()<1024*1024 &&
        !wire.at("supplemental_asset_changes")[0].at("asset").contains("bytes_hex"),"outer10 extraction must retain bounded compact references");
    auto replay = sketch::Document::create(values,assets);
    replay.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes"),resolver));
    check(replay.snapshot().entities()==changed.entities() && replay.snapshot().assets()==changed.assets(),
        "hydrated exchange25 proof must independently reproduce geometry, provenance and assets");
    replay.undo(replay.revision());
    check(replay.snapshot().entities()==before.entities() && replay.snapshot().assets()==before.assets(),"hydrated rigid proof must undo once exactly");
    replay.redo(replay.revision());
    check(replay.snapshot().entities()==changed.entities() && replay.snapshot().assets()==changed.assets(),"hydrated rigid proof must redo once exactly");
    for (const int mutation : {0,1,2}) {
      auto forged=wire;
      if (mutation==0) forged["wall_edits"][0]["rigid_transform"]["offset"][0]=99.0;
      if (mutation==1) forged["wall_edits"][0]["baseline"]["end"][0]=99.0;
      if (mutation==2) forged["wall_edits"][0]["rigid_transform"]["unknown"]=true;
      auto refused=sketch::Document::create(values,assets); const auto digest=sketch::document_snapshot_digest(refused.snapshot());
      bool rejected=false; try { refused.apply(sketch::command_from_json(forged,resolver)); } catch (const std::exception&) { rejected=true; }
      check(rejected && sketch::document_snapshot_digest(refused.snapshot())==digest,
          "rigid extraction transform/baseline/unknown-field tampering must reject atomically");
    }
    if (compact) {
      auto forged=wire; forged["supplemental_asset_changes"][0]["asset"]["metadata_sha256"]=std::string(64,'0');
      bool rejected=false; try { (void)sketch::command_from_json(forged,resolver); } catch (const std::exception&) { rejected=true; }
      check(rejected,"rigid extraction hydration must reject mismatched exact metadata digest");
      rejected=false; try { (void)sketch::command_from_json(wire); } catch (const std::exception&) { rejected=true; }
      check(rejected,"rigid extraction compact proof must require its published result assets");
    }
  }
  sketch::ProjectWorkspace workspace(document.snapshot()); const auto capture=workspace.capture();
  const auto history=sketch::capture_workspace_history_record(capture);
  sketch::RecoveryLedger ledger{{"selected-rigid-history","workspace_history",
      sketch::encode_workspace_history_record(capture.document(),history,std::nullopt)}};
  const auto native=root/("selected-rigid-recovery-"+sketch::make_stable_id()+".bldproj");
  (void)sketch::ProjectStore::save_archive(native,{capture.document(),ledger,sketch::ArchiveRole::ordinary});
  const auto recovered=sketch::ProjectStore::load_archive(native,sketch::ArchiveRole::ordinary);
  check(recovered.supported(),"rigid extraction recovery fixture must hydrate its native history");
  const auto destination=root/("selected-rigid-recovery-exchange-"+sketch::make_stable_id());
  sketch::extract_project_archive(*recovered.archive,destination);
  std::ifstream input(destination/"project.json"); const auto archive=nlohmann::json::parse(input);
  check(archive.at("exchange_version")==25 && archive.at("revisions")[1].at("boundary_constraint_changes")==wire &&
      archive.at("recovery_records")[0].at("envelope")==ledger[0].envelope,
      "recovery extraction must preserve rigid ten proof, assets and raw ledger at exchange25 while undone");
}

void test_live_exterior_source_exchange_v17(const std::filesystem::path& root, bool mixed = false, bool compact = false) {
  std::vector<sketch::Entity> values{
      {"exchange-property", "property", nlohmann::json::object(), false, nlohmann::json::object()},
      {"exchange-building", "building", {{"property_id", "exchange-property"}}, false, nlohmann::json::object()},
      {"exchange-floor", "floor", {{"building_id", "exchange-building"}}, false, nlohmann::json::object()},
      {"exchange-layer", "layer", {{"floor_id", "exchange-floor"}}, false, nlohmann::json::object()}};
  const sketch::Vec2 corners[]{{0,0},{4,0},{4,3},{0,3}};
  std::vector<std::string> ids;
  for (std::size_t i = 0; i < 4; ++i) {
    ids.push_back("exchange-wall-" + std::to_string(i));
    const auto a = corners[i], b = corners[(i + 1) % 4];
    values.push_back({ids.back(), "wall", {{"baseline", {{"start", {a.x,a.y}}, {"end", {b.x,b.y}},
        {"sweep_radians", 0.0}}}, {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0},
        {"floor_id", "exchange-floor"}, {"layer_id", "exchange-layer"}}, false, nlohmann::json::object()});
  }
  const auto measured = sketch::derive_exterior_wall_measurement(sketch::Document::create(values).snapshot(), ids);
  auto geometry = nlohmann::json::array();
  for (const auto& edge : measured.boundary)
    geometry.push_back({{"start", {edge.start.x,edge.start.y}}, {"end", {edge.end.x,edge.end.y}},
        {"sweep_radians", edge.sweep_radians}});
  values.push_back(sketch::upgrade_legacy_boundary_entity({"exchange-area", "measurement_boundary",
      {{"boundary", geometry}, {"floor_id", "exchange-floor"}, {"layer_id", "exchange-layer"},
       {"wall_measurement_source", measured.source}}, false, nlohmann::json::object()}));
  std::vector<sketch::Asset> assets;
  if (mixed) {
    values.push_back({"exchange-label", "label", {{"text", "Before"}}, false, nlohmann::json::object()});
    values.push_back({"exchange-object", "object", {{"asset_id", "exchange-image"}, {"name", "Before"}}, false, nlohmann::json::object()});
    assets.push_back(sketch::Asset::create("exchange-image", "application/octet-stream", {std::byte{1}}, {{"caption", "Before"}}));
  }
  auto document = sketch::Document::create(values, assets);
  const auto before = document.snapshot();
  auto wall = before.entities().at(ids.front());
  wall.properties["thickness_m"] = 0.4;
  sketch::ApplyEntityChanges ordinary{before.revision(), {sketch::EntityChange::upsert(wall)}, {}, "Widen source wall"};
  if (mixed) {
    auto label = before.entities().at("exchange-label");
    label.properties["text"] = "After";
    auto object = before.entities().at("exchange-object");
    object.properties["name"] = "After";
    ordinary.entity_changes.push_back(sketch::EntityChange::upsert(label));
    ordinary.entity_changes.push_back(sketch::EntityChange::upsert(object));
    ordinary.asset_changes.push_back(sketch::AssetChange::upsert(sketch::Asset::create("exchange-image",
        "application/octet-stream", compact ? std::vector<std::byte>(530*1024,std::byte{42}) : std::vector<std::byte>{std::byte{2},std::byte{3}}, {{"caption", "After"}})));
  }
  auto command = sketch::complete_exterior_wall_measurement_command(before, ordinary);
  if(mixed && !compact)std::get<sketch::ApplyBoundaryConstraintChanges>(command).supplemental_asset_reference_completion=false;
  document.apply(command);
  const auto changed = document.snapshot();
  auto deleted = sketch::Document::fork(changed);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(), {sketch::EntityChange::erase("exchange-area")}, {}, "Delete owner later"});
  document.undo(document.revision());
  for (const auto& snapshot : {changed, document.snapshot(), deleted.snapshot()}) {
    const auto destination = root / ("source-v17-" + sketch::make_stable_id());
    sketch::extract_project(snapshot, destination);
    std::ifstream input(destination / "project.json");
    const auto exchanged = nlohmann::json::parse(input);
    const auto& rows = exchanged.at("revisions");
    check(exchanged.at("exchange_version") == (compact ? 24 : mixed ? 18 : 17) && rows.size() == snapshot.history().size() &&
        rows[1].at("boundary_constraint_changes") == sketch::command_to_json(command),
        "current, undone and deleted live source history must advertise its exchange floor and retain exact proof");
    for (std::size_t i = 0; i < rows.size(); ++i) {
      check(rows[i].contains("boundary_constraint_changes") == (i == 1),
          "only the source command revision owns the completion proof");
      check(rows[i].at("undo_stack") == nlohmann::json(snapshot.history()[i].undo_stack) &&
          rows[i].at("redo_stack") == nlohmann::json(snapshot.history()[i].redo_stack),
          "exchange must retain every navigation stack");
    }
    std::vector<sketch::Entity> source_values;
    for (const auto& value : rows[0].at("entities"))
      source_values.push_back({value.at("id").get<std::string>(), value.at("type").get<std::string>(),
          value.at("properties"), value.at("required").get<bool>(), value.at("extensions")});
    auto replay = sketch::Document::create(source_values, assets);
    std::map<std::string,sketch::Asset> published_assets;
    for(const auto& row:rows[1].at("assets")) {
      std::ifstream payload_file(destination/row.at("path").get<std::string>(),std::ios::binary);
      const std::string payload{std::istreambuf_iterator<char>(payload_file),std::istreambuf_iterator<char>()};
      std::vector<std::byte> bytes(payload.size());
      std::transform(payload.begin(),payload.end(),bytes.begin(),[](char value){return static_cast<std::byte>(static_cast<unsigned char>(value));});
      auto asset=sketch::Asset::create(row.at("id").get<std::string>(),row.at("media_type").get<std::string>(),std::move(bytes),row.at("metadata"));
      check(asset.sha256==row.at("sha256").get<std::string>(),"published result assets retain validated exact content identity");published_assets.emplace(asset.id,std::move(asset));
    }
    const auto resolver=[&published_assets](std::string_view id)->const sketch::Asset* {
      const auto found=published_assets.find(std::string(id));return found==published_assets.end()?nullptr:&found->second;
    };
    replay.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes"),resolver));
    check(replay.snapshot().entities() == changed.entities() && replay.snapshot().assets() == changed.assets() &&
        sketch::wall_measurement_source_current(replay.snapshot(), replay.snapshot().entities().at("exchange-area")),
        "decoded exchange source completion must replay exact current physical and measured geometry");
    replay.undo(replay.revision());
    check(replay.snapshot().entities() == before.entities() && replay.snapshot().assets() == before.assets(),
        "exchanged completion must undo geometry, supplemental entities and assets once");
    replay.redo(replay.revision());
    check(replay.snapshot().entities() == changed.entities() && replay.snapshot().assets() == changed.assets(),
        "exchanged completion must redo geometry, supplemental entities and assets once");

    for (const bool physical : {false, true}) {
      auto forged = rows[1].at("boundary_constraint_changes");
      if (physical) forged["physical_entity_changes"][0]["entity"]["properties"]["thickness_m"] = 0.6;
      else forged["exterior_source_edits"][0]["replacement_segments"][0]["start"][0] = 999.0;
      auto refused = sketch::Document::create(source_values, assets);
      const auto untouched = sketch::document_snapshot_digest(refused.snapshot());
      bool rejected = false;
      try { refused.apply(sketch::command_from_json(forged,resolver)); }
      catch (const std::exception&) { rejected = true; }
      check(rejected && sketch::document_snapshot_digest(refused.snapshot()) == untouched,
          "exchanged source proof tampering must reject atomically at document admission");
    }
    if (mixed) {
      const auto& asset_rows = rows[1].at("assets");
      check(asset_rows.size() == 1 && asset_rows[0].at("sha256") == changed.assets().at("exchange-image").sha256,
          "mixed extraction must retain the referenced asset identity and exact byte digest");
      std::ifstream asset_input(destination / asset_rows[0].at("path").get<std::string>(), std::ios::binary);
      const std::string extracted_bytes{std::istreambuf_iterator<char>(asset_input), std::istreambuf_iterator<char>()};
      check(extracted_bytes == (compact ? std::string(530*1024,'*') : std::string("\x02\x03", 2)), "mixed extraction must publish exact supplemental asset bytes");
      if(compact)check(rows[1].at("boundary_constraint_changes").dump().size()<1024*1024 &&
          !rows[1].at("boundary_constraint_changes").at("supplemental_asset_changes")[0].at("asset").contains("bytes_hex"),"extraction24 keeps large assets separate from compact proof JSON");
      for (const int mutation : {0, 1, 2, 3}) {
        auto forged = rows[1].at("boundary_constraint_changes");
        if (mutation == 0) forged["supplemental_entity_changes"][0]["entity"]["properties"]["text"] = "Forged";
        if (mutation == 1) {
          if(compact)forged["supplemental_asset_changes"][0]["asset"]["metadata_sha256"]=std::string(64,'0');
          else forged["supplemental_asset_changes"][0]["asset"]["metadata"]["caption"] = "Forged";
        }
        if (mutation == 2) forged.erase("supplemental_asset_changes");
        if (mutation == 3) forged["version"] = 6;
        if (mutation < 2) {
          auto tampered = snapshot;
          auto& proof = *const_cast<std::vector<sketch::RevisionRecord>&>(tampered.history())[1].boundary_constraint_changes;
          bool rejected = false;
          try { proof=std::get<sketch::ApplyBoundaryConstraintChanges>(sketch::command_from_json(forged,resolver));(void)sketch::Document::fork(tampered); } catch (const std::exception&) { rejected = true; }
          check(rejected, "extracted supplemental proof must independently reproduce the recorded entities and assets");
        } else {
          bool rejected = false;
          try { (void)sketch::command_from_json(forged,resolver); } catch (const std::exception&) { rejected = true; }
          check(rejected, "extracted v7 proof must reject removed lanes and downgraded envelopes");
        }
      }
    }
  }
}

void test_reviewed_source_export_floor(const std::filesystem::path& root) {
 for (const bool fresh : {false,true}) {
  sketch::IdentifiedBoundary boundary{"imported-source-area", "measurement_boundary", {}};
  const sketch::Vec2 points[]{{0,0},{4,0},{4,3},{0,3}};
  for (std::size_t i = 0; i < 4; ++i)
    boundary.segments.push_back({"source-edge-" + std::to_string(i), "source-vertex-" + std::to_string(i),
      "source-vertex-" + std::to_string((i+1)%4), {points[i], points[(i+1)%4], 0.0}});
  auto owner = sketch::encode_identified_boundary_entity(boundary);
  sketch::BoundaryGeometryEdit edit;
  edit.boundary_id = owner.id; edit.target_id = owner.id;
  edit.kind = sketch::BoundaryGeometryEditKind::redefine_boundary;
  edit.replacement_segments = owner.properties.at("segments");
  edit.replacement_wall_source_ids = {"historical-wall-0", "historical-wall-1", "historical-wall-2", "historical-wall-3"};
  const auto origin = owner.properties;
  if (fresh) {
    for (std::size_t i = 0; i < edit.replacement_segments.size(); ++i) {
      edit.replacement_segments[i]["segment_id"] = "fresh-exchange-edge-" + std::to_string(i);
      edit.replacement_segments[i]["start_vertex_id"] = "fresh-exchange-corner-" + std::to_string(i);
      edit.replacement_segments[i]["end_vertex_id"] = "fresh-exchange-corner-" + std::to_string((i+1)%4);
    }
    auto wire = sketch::encode_boundary_geometry_edit(edit);wire["version"] = 4;wire["fresh_topology"] = true;
    edit = sketch::decode_boundary_geometry_edit(wire);
    owner.properties["segments"] = edit.replacement_segments;
  }
  auto records = nlohmann::json::array();
  for (const auto& id : edit.replacement_wall_source_ids)
    records.push_back({{"id", id}, {"context", nlohmann::json::object()}});
  owner.properties["wall_measurement_source"] = {{"version", 1}, {"basis", "exterior"}, {"walls", records}};
  owner.extensions["boundary_geometry_derivation"] = {{"version", 2}, {"source_boundary", origin},
    {"operations", nlohmann::json::array({{{"kind", "geometry_edit"}, {"value", sketch::encode_boundary_geometry_edit(edit)}}})}};
  auto document = sketch::Document::create({owner});
  check(document.snapshot().history().size() == 1 && sketch::ProjectStore::required_format_version(document.snapshot()) == (fresh ? 17U : 16U),
    "imported topology source intent must retain its native floor without originating history or current walls");
  const auto destination = root / (fresh ? "fresh-source-import" : "reviewed-source-import");
  sketch::extract_project(document.snapshot(), destination);
  std::ifstream input(destination / "project.json");
  const auto exported = nlohmann::json::parse(input);
  check(exported.at("exchange_version") == (fresh ? 15 : 14), "retained imported source redefinition requires the appropriate exchange reader floor");
  check(document.snapshot().entities().at(owner.id) == owner, "source extraction must preserve the imported analytical proof");
 }
}

void test_automatic_angle_redraw_export_floor(const std::filesystem::path& root) {
  auto document = sketch::testing::document_with_removed_automatic_angle();
  const auto changed = document.snapshot();
  std::vector<sketch::Entity> entities;
  for (const auto& [id, value] : changed.entities()) { (void)id; entities.push_back(value); }
  const auto imported = sketch::Document::create(std::move(entities));
  auto deleted = sketch::Document::fork(changed);
  deleted.apply(sketch::ApplyEntityChanges{deleted.revision(), {sketch::EntityChange::erase("angle-area")}, {}, "Delete later"});
  document.undo(document.revision());
  int sequence = 0;
  for (const auto& snapshot : {changed, document.snapshot(), imported.snapshot(), deleted.snapshot()}) {
    const auto destination = root / ("angle-redraw-" + std::to_string(sequence++));
    sketch::extract_project(snapshot, destination);
    std::ifstream input(destination / "project.json");
    const auto encoded = nlohmann::json::parse(input);
    check(encoded.at("exchange_version") == 21 && encoded.at("revisions").size() == snapshot.history().size(),
        "automatic-angle redraw extraction requires exchange21 without dropping history");
    if (snapshot.history().size() > 1) {
      check(encoded.at("revisions")[1].at("boundary_geometry_edit").at("version") == 5 &&
          encoded.at("revisions")[1].at("boundary_geometry_edit").at("allow_automatic_angle_removal") == true,
          "extraction must retain the exact explicit version-five decision");
    } else {
      bool found = false;
      for (const auto& owner : encoded.at("revisions")[0].at("entities")) {
        if (owner.at("id") != "angle-area") continue;
        found = true;
        check(owner.at("extensions").dump() == snapshot.entities().at("angle-area").extensions.dump(),
            "history-free extraction must retain the exact imported version-five geometry proof");
      }
      check(found, "history-free extraction must retain its measured boundary");
    }
  }
}

void test_curved_constraint_export_floor(const std::filesystem::path& root) {
  sketch::Entity wall{"curve-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.4}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto source = wall;
  const auto source_baseline = sketch::arc_from_chord_arc_length({0, 0}, {4, 0}, 5.0, false);
  source.properties["baseline"]["sweep_radians"] = source_baseline.sweep_radians;
  const auto angle = sketch::angle_from_radians(source_baseline.sweep_radians);
  source.extensions["curve_input"] = {{"version", 2}, {"construction", "arc_length"},
      {"measure", "5 m"}, {"normalized_measure", "5 m"}, {"measure_value", 5.0},
      {"clockwise", false}, {"start", {0, 0}}, {"end", {4, 0}},
      {"sweep", angle.original_expression}, {"normalized_sweep", angle.normalized_expression},
      {"radians", source_baseline.sweep_radians}, {"vendor", "preserve"}};
  const auto derived = sketch::replay_constraint_wall_edit(source,
      {source.id, {{0, 0}, {4.5, 0}, source_baseline.sweep_radians}, std::nullopt, 2});
  const auto imported = sketch::Document::create({derived});
  sketch::extract_project(imported.snapshot(), root / "imported-curve-derivation");
  std::ifstream imported_input(root / "imported-curve-derivation" / "project.json");
  const auto imported_json = nlohmann::json::parse(imported_input);
  check(imported_json.at("exchange_version") == 7 &&
        !imported_json.at("revisions")[0].contains("boundary_constraint_changes"),
        "imported curve derivation must advertise exchange7 without originating command history");
  const auto collision = sketch::Document::create({{"vendor-label", "label", {{"text", "opaque"}},
      false, {{"curve_input_derivation", derived.extensions.at("curve_input_derivation")}}}});
  sketch::extract_project(collision.snapshot(), root / "opaque-curve-derivation-collision");
  std::ifstream collision_input(root / "opaque-curve-derivation-collision" / "project.json");
  check(nlohmann::json::parse(collision_input).at("exchange_version") == 1,
        "generic vendor curve derivation extension must retain its historical exchange floor");
  sketch::PersistentConstraint relation;
  relation.id = "curve-horizontal";
  relation.relation = sketch::ConstraintRelationKind::horizontal;
  relation.bindings = {{wall.id, sketch::WallEndpointRole::start},
                       {wall.id, sketch::WallEndpointRole::end}};
  auto state = sketch::Document::create({wall, sketch::encode_constraint_entity(relation)});
  sketch::extract_project(state.snapshot(), root / "curve-state");
  std::ifstream state_input(root / "curve-state" / "project.json");
  const auto state_json = nlohmann::json::parse(state_input);
  check(state_json.at("exchange_version") == 7 &&
        !state_json.at("revisions")[0].contains("boundary_constraint_changes"),
        "already satisfied curved relation must advertise exchange7 without a wall edit proof");
  auto undone = sketch::Document::create({wall});
  undone.apply(sketch::ApplyEntityChanges{0,
      {sketch::EntityChange::upsert(sketch::encode_constraint_entity(relation))}, {}, "curve relation"});
  undone.undo(undone.revision());
  sketch::extract_project(undone.snapshot(), root / "curve-state-undone");
  std::ifstream undone_input(root / "curve-state-undone" / "project.json");
  const auto undone_json = nlohmann::json::parse(undone_input);
  check(undone_json.at("exchange_version") == 7 && undone_json.at("revisions").size() == 3,
        "undone curve-bound relation must retain exchange7 and all states");

  auto proof = sketch::Document::create({wall});
  sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "curved wall chord"};
  command.wall_edits.push_back({wall.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
  proof.apply(command);
  sketch::extract_project(proof.snapshot(), root / "curve-proof-head");
  std::ifstream head_input(root / "curve-proof-head" / "project.json");
  check(nlohmann::json::parse(head_input).at("exchange_version") == 7,
        "curved wall proof at head must advertise exchange7 without relations");
  // Later ordinary rows and undo must not lower the proof's retained reader floor.
  proof.apply(sketch::ApplyEntityChanges{proof.revision(),
      {sketch::EntityChange::upsert({"label", "label", {{"text", "later"}}, false,
                                  nlohmann::json::object()})}, {}, "later row"});
  proof.undo(proof.revision());
  proof.undo(proof.revision());
  sketch::extract_project(proof.snapshot(), root / "curve-proof-undone");
  std::ifstream proof_input(root / "curve-proof-undone" / "project.json");
  const auto proof_json = nlohmann::json::parse(proof_input);
  check(proof_json.at("exchange_version") == 7,
        "retained undone curved wall proof must advertise exchange7");
  const auto& rows = proof_json.at("revisions");
  check(rows[1].at("boundary_constraint_changes").at("version") == 3 &&
        rows[1].at("boundary_constraint_changes").at("wall_edits")[0].at("version") == 2 &&
        !rows.back().contains("boundary_constraint_changes"),
        "exchange must retain curved proof versions only on their command row");

  sketch::IdentifiedBoundary boundary{"curved-boundary", "measurement_boundary", {
      {"edge-0", "vertex-0", "vertex-1", {{0, 0}, {4, 0}, 0.2}},
      {"edge-1", "vertex-1", "vertex-2", {{4, 0}, {4, 3}, 0}},
      {"edge-2", "vertex-2", "vertex-3", {{4, 3}, {0, 3}, 0}},
      {"edge-3", "vertex-3", "vertex-0", {{0, 3}, {0, 0}, 0}}}};
  relation.relation = sketch::ConstraintRelationKind::vertical;
  relation.bindings = {{boundary.id, sketch::WallEndpointRole::start, "edge-1", "vertex-1"},
                       {boundary.id, sketch::WallEndpointRole::end, "edge-1", "vertex-2"}};
  auto straight = sketch::Document::create({sketch::encode_identified_boundary_entity(boundary),
                                           sketch::encode_constraint_entity(relation)});
  sketch::extract_project(straight.snapshot(), root / "straight-edge-curved-owner");
  std::ifstream straight_input(root / "straight-edge-curved-owner" / "project.json");
  check(nlohmann::json::parse(straight_input).at("exchange_version") == 1,
        "straight edge binding on curved boundary must retain historical exchange floor");
  relation.relation = sketch::ConstraintRelationKind::horizontal;
  relation.bindings = {{boundary.id, sketch::WallEndpointRole::start, "edge-0", "vertex-0"},
                       {boundary.id, sketch::WallEndpointRole::end, "edge-0", "vertex-1"}};
  auto curve = sketch::Document::create({sketch::encode_identified_boundary_entity(boundary),
                                        sketch::encode_constraint_entity(relation)});
  sketch::extract_project(curve.snapshot(), root / "curved-edge-relation");
  std::ifstream curve_input(root / "curved-edge-relation" / "project.json");
  check(nlohmann::json::parse(curve_input).at("exchange_version") == 7,
        "bound curved boundary edge must advertise exchange7 without a wall edit proof");
}

void test_safe_curved_wall_history_exchange_replays(const std::filesystem::path& root) {
  sketch::Entity source{"curve-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.4}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto obstacle = source;
  obstacle.id = "crossing-obstacle";
  obstacle.properties["baseline"] = {{"start", {6, -10}}, {"end", {6, 10}}, {"sweep_radians", 0}};
  auto document = sketch::Document::create({source, obstacle});
  const auto initial = document.snapshot().entities();
  sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "safe curved endpoint edit"};
  command.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
  document.apply(command);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "safe-curved-wall-history");
  std::ifstream input(root / "safe-curved-wall-history" / "project.json");
  const auto exchanged = nlohmann::json::parse(input);
  const auto& rows = exchanged.at("revisions");
  check(exchanged.at("exchange_version") == 7 && rows.size() == 3,
        "valid undone curved wall history must preserve its reader floor and all revisions");
  std::vector<sketch::Entity> source_entities;
  for (const auto& value : rows[0].at("entities"))
    source_entities.push_back({value.at("id").get<std::string>(), value.at("type").get<std::string>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")});
  auto replayed = sketch::Document::create(std::move(source_entities));
  check(replayed.snapshot().entities() == initial, "exchange must reconstruct the exact source wall state");
  replayed.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited,
        "exchanged curved command must replay exact safe geometry after topology admission");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial,
        "exchanged safe curved command must undo exactly");
  replayed.redo(replayed.revision());
  check(replayed.snapshot().entities() == edited,
        "exchanged safe curved command must redo exactly");

  auto forged_exchange = exchanged;
  auto& forged_rows = forged_exchange.at("revisions");
  auto& forged_proof = forged_rows[1].at("boundary_constraint_changes");
  forged_proof["wall_edits"][0]["baseline"]["end"][0] = 7.0;
  for (auto& value : forged_rows[1].at("entities"))
    if (value.at("id") == source.id) value["properties"]["baseline"]["end"][0] = 7.0;
  const auto forged_command = sketch::command_from_json(forged_proof);
  const auto& typed = std::get<sketch::ApplyBoundaryConstraintChanges>(forged_command);
  const auto forged_wall = sketch::replay_constraint_wall_edit(source, typed.wall_edits.front());
  std::size_t matched_walls = 0;
  for (const auto& value : forged_rows[1].at("entities"))
    if (value.at("id") == source.id) {
      ++matched_walls;
      check(value.at("properties") == forged_wall.properties && value.at("extensions") == forged_wall.extensions,
            "forged exchanged proof and retained wall result must agree exactly");
    }
  check(matched_walls == 1, "forged exchanged result must contain exactly one matching wall");
  // Exchange has no history importer: exercise its decoded typed command at
  // the same document admission boundary used by history restoration.
  auto refused = sketch::Document::create({source, obstacle});
  const auto before_refusal = refused.snapshot();
  const auto source_path = root / "safe-curved-wall-history" / "project.json";
  const auto source_hash = sketch::ProjectStore::file_sha256(source_path);
  bool rejected = false;
  try {
    refused.apply(forged_command);
  } catch (const sketch::DocumentError& error) {
    rejected = std::string_view(error.what()).find("topology") != std::string_view::npos;
  }
  check(rejected && refused.snapshot().entities() == before_refusal.entities() &&
        refused.snapshot().revision() == before_refusal.revision() &&
        refused.snapshot().history().size() == before_refusal.history().size(),
        "matching forged exchanged proof/result must refuse new crossing atomically");
  check(sketch::ProjectStore::file_sha256(source_path) == source_hash,
        "refusing forged exchanged command must preserve exact source artifact bytes");
}

void test_straight_wall_only_authoring_exchange_v8(const std::filesystem::path& root) {
  sketch::Entity source{"straight-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto document = sketch::Document::create({source});
  const auto initial = document.snapshot().entities();
  sketch::ApplyBoundaryConstraintChanges intent{document.revision(), {}, {}, "Resize straight wall"};
  intent.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0}, sketch::parse_quantity("5 m")});
  document.apply(intent);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "straight-wall-only-v8");
  std::ifstream input(root / "straight-wall-only-v8" / "project.json");
  const auto exchanged = nlohmann::json::parse(input);
  const auto& rows = exchanged.at("revisions");
  check(exchanged.at("exchange_version") == 8 && rows[1].at("boundary_constraint_changes").at("version") == 4 &&
        !rows[2].contains("boundary_constraint_changes"),
        "undone straight wall-only authoring must retain exchange8 and originating version4 proof");
  auto replayed = sketch::Document::create({source});
  replayed.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited, "exchanged version4 must replay exact straight geometry and receipt");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial, "exchanged version4 must undo to exact source");
  replayed.redo(replayed.revision());
  check(replayed.snapshot().entities() == edited, "exchanged version4 must redo exact guarded endpoint intent");
  auto curve = source;
  curve.id = "later-curve";
  curve.properties["baseline"] = {{"start", {10, 0}}, {"end", {14, 0}}, {"sweep_radians", 0.4}};
  document.apply(sketch::ApplyEntityChanges{document.revision(),
      {sketch::EntityChange::upsert(curve)}, {}, "Add later curve"});
  sketch::ApplyBoundaryConstraintChanges curve_edit{document.revision(), {}, {}, "Edit later curve"};
  curve_edit.wall_edits.push_back({curve.id, {{10, 0}, {15, 0}, 0.4}, std::nullopt, 2});
  document.apply(curve_edit);
  sketch::extract_project(document.snapshot(), root / "mixed-straight-curved-v8");
  std::ifstream mixed_input(root / "mixed-straight-curved-v8" / "project.json");
  const auto mixed = nlohmann::json::parse(mixed_input);
  check(mixed.at("exchange_version") == 8 &&
        mixed.at("revisions").back().at("boundary_constraint_changes").at("version") == 3,
        "later curved proof must not lower exchange8 required by retained straight wall-only intent");
}

void test_physical_curve_length_exchange_v9(const std::filesystem::path& root) {
  const auto pi = std::numbers::pi;
  sketch::Entity wall{"physical-curve", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {8 / pi, 0}}, {"sweep_radians", pi}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0}}, false,
      nlohmann::json::object()};
  sketch::PersistentConstraint relation;
  relation.id = "curve-length";
  relation.relation = sketch::ConstraintRelationKind::fixed_arc_length;
  relation.bindings = {{wall.id, sketch::WallEndpointRole::start}, {wall.id, sketch::WallEndpointRole::end}};
  relation.length = sketch::parse_quantity("4 m");
  auto document = sketch::Document::create({wall, sketch::encode_constraint_entity(relation)});
  const auto initial = document.snapshot().entities();
  relation.length = sketch::parse_quantity("6 m");
  sketch::ApplyBoundaryConstraintChanges command{0, {},
      {sketch::EntityChange::upsert(sketch::encode_constraint_entity(relation))}, "Edit physical curve length"};
  command.wall_edits.push_back({wall.id, {{0, 0}, {12 / pi, 0}, pi}, std::nullopt, 2});
  document.apply(command);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "physical-curve-v9");
  std::ifstream input(root / "physical-curve-v9" / "project.json");
  const auto json = nlohmann::json::parse(input);
  check(json.at("exchange_version") == 9,
        "retained physical curve-length constraint must advertise exchange9");
  auto replayed = sketch::Document::create({wall, sketch::encode_constraint_entity(
      *sketch::decode_constraint_entity(initial.at(relation.id)).constraint)});
  replayed.apply(sketch::command_from_json(json.at("revisions").at(1).at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited, "physical curve exchange must replay exact geometry and quantity");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial, "physical curve exchange must undo exactly");
  document.apply(sketch::ApplyEntityChanges{document.revision(),
      {sketch::EntityChange::erase(wall.id), sketch::EntityChange::erase(relation.id)}, {}, "Delete curve"});
  sketch::extract_project(document.snapshot(), root / "deleted-physical-curve-v9");
  std::ifstream deleted_input(root / "deleted-physical-curve-v9" / "project.json");
  check(nlohmann::json::parse(deleted_input).at("exchange_version") == 9,
        "deleted and undone physical curve history must retain exchange9");
}
void test_direct_curve_length_exchange_v10(const std::filesystem::path& root) {
  const auto pi = std::numbers::pi;
  const sketch::Entity wall{"direct-curve", "wall",
      {{"baseline", {{"start", {0,0}}, {"end", {8/pi,0}}, {"sweep_radians", pi}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0}}};
  auto document = sketch::Document::create({wall});
  const auto initial = document.snapshot().entities();
  sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "Direct physical length"};
  command.wall_edits.push_back({wall.id, {{0,0},{12/pi,0},pi}, sketch::parse_quantity("6 m"), 3});
  document.apply(command);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "direct-curve-v10");
  std::ifstream input(root / "direct-curve-v10" / "project.json");
  const auto json = nlohmann::json::parse(input);
  check(json.at("exchange_version") == 10 &&
        json.at("revisions").at(1).at("boundary_constraint_changes").at("version") == 5,
        "direct physical input must advertise exchange10 and explicit command5");
  auto replayed = sketch::Document::create({wall});
  replayed.apply(sketch::command_from_json(json.at("revisions").at(1).at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited, "extracted physical input must replay exact receipt and geometry");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial, "extracted physical input must undo exactly");
  auto imported = sketch::Document::create({edited.at(wall.id)});
  sketch::extract_project(imported.snapshot(), root / "imported-direct-curve-v10");
  std::ifstream imported_input(root / "imported-direct-curve-v10" / "project.json");
  check(nlohmann::json::parse(imported_input).at("exchange_version") == 10,
        "known physical input receipt without history must advertise exchange10");
  document.redo(document.revision());
  sketch::ApplyBoundaryConstraintChanges later{document.revision(), {}, {}, "Later curve endpoint"};
  later.wall_edits.push_back({wall.id, {{0,0},{14/pi,0},pi}, std::nullopt, 2});
  document.apply(later);
  document.apply(sketch::ApplyEntityChanges{document.revision(), {sketch::EntityChange::erase(wall.id)}, {}, "Delete curve"});
  sketch::extract_project(document.snapshot(), root / "deleted-direct-curve-v10");
  std::ifstream deleted_input(root / "deleted-direct-curve-v10" / "project.json");
  check(nlohmann::json::parse(deleted_input).at("exchange_version") == 10,
        "later geometry-only proof and owner deletion must retain exchange10");
}
void test_rigid_curve_archive_exchange_v11(const std::filesystem::path& root) {
  const auto curve = sketch::arc_from_chord_arc_length({0, 0}, {4, 0}, 5.0, false);
  const auto angle = sketch::angle_from_radians(curve.sweep_radians);
  sketch::Entity source{"rigid-curve", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", curve.sweep_radians}, {"vendor", "original geometry"}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0}}, false,
      {{"curve_input", {{"version", 2}, {"construction", "arc_length"}, {"measure", "5 m"},
          {"normalized_measure", "5 m"}, {"measure_value", 5.0}, {"clockwise", false},
          {"start", {0, 0}}, {"end", {4, 0}}, {"sweep", angle.original_expression},
          {"normalized_sweep", angle.normalized_expression}, {"radians", curve.sweep_radians},
          {"vendor", {{"preserve", "exact input"}}}}}}};
  auto reflected = source;
  const sketch::PlanarTransform transform{{2, 0}, 0, true, false, {}};
  sketch::transform_wall_curve_input(reflected, transform);
  const auto changed = sketch::transform_segment(curve, transform);
  reflected.properties["baseline"]["start"] = {changed.start.x, changed.start.y};
  reflected.properties["baseline"]["end"] = {changed.end.x, changed.end.y};
  reflected.properties["baseline"]["sweep_radians"] = changed.sweep_radians;
  auto document = sketch::Document::create({source});
  document.apply(sketch::ApplyEntityChanges{document.revision(),
      {sketch::EntityChange::upsert(reflected)}, {}, "Reflect measured curve"});
  const auto extract = [&](const sketch::DocumentSnapshot& snapshot, const char* directory) {
    sketch::extract_project(snapshot, root / directory);
    std::ifstream input(root / directory / "project.json");
    auto json = nlohmann::json::parse(input);
    check(json.at("exchange_version") == 11, "head, undone, deleted and imported rigid archives must advertise exchange11");
    return json;
  };
  const auto head = extract(document.snapshot(), "rigid-curve-head-v11");
  std::size_t found = 0;
  for (const auto& value : head.at("revisions").at(1).at("entities")) if (value.at("id") == source.id) {
    ++found;
    const sketch::Entity encoded{value.at("id").get<std::string>(), value.at("type").get<std::string>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")};
    check(encoded == reflected, "exchanged rigid archive must retain exact entity and unknown source metadata");
    const auto& archive = encoded.extensions.at("curve_input_derivation");
    check(archive.at("version") == 2 && archive.at("source_input") == source.extensions.at("curve_input") &&
          archive.at("source_baseline") == source.properties.at("baseline"),
        "exchanged reflection must retain its exact original measured source");
    auto replayed = sketch::Document::create({source});
    replayed.apply(sketch::ApplyEntityChanges{replayed.revision(), {sketch::EntityChange::upsert(encoded)}, {}, "Replay rigid transform"});
    check(replayed.snapshot().entities() == document.snapshot().entities(), "exchanged rigid entity must independently validate on replay");
    replayed.undo(replayed.revision());
    check(replayed.snapshot().entities().at(source.id) == source, "exchanged rigid transform must undo exactly");
    replayed.redo(replayed.revision());
    check(replayed.snapshot().entities().at(source.id) == reflected, "exchanged rigid transform must redo exactly");
  }
  check(found == 1, "rigid exchange must contain exactly one reflected owner");
  document.undo(document.revision());
  const auto undone = extract(document.snapshot(), "rigid-curve-undone-v11");
  check(undone.at("revisions").size() == 3 && document.snapshot().entities().at(source.id) == source,
      "undone rigid exchange must retain source head and full transform history");
  const auto imported = sketch::Document::create({reflected});
  const auto imported_json = extract(imported.snapshot(), "rigid-curve-imported-v11");
  check(imported_json.at("revisions").size() == 1 &&
        imported_json.at("revisions").at(0).at("entities").at(0).at("extensions") == reflected.extensions,
      "rigid archive must retain exchange11 and exact evidence without its originating history");
  document.redo(document.revision());
  document.apply(sketch::ApplyEntityChanges{document.revision(), {sketch::EntityChange::erase(source.id)}, {}, "Delete rigid curve"});
  (void)extract(document.snapshot(), "rigid-curve-deleted-v11");
}

void test_archive_extraction(const std::filesystem::path& root, bool styled = false) {
  for (const auto role : {sketch::ArchiveRole::ordinary,
                          sketch::ArchiveRole::recovery_copy}) {
    auto document = styled ? sketch::Document::create({
        {"p", "property", nlohmann::json::object()}, {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}}, {"l", "layer", {{"floor_id", "f"}}}}) : sketch::Document::create();
    sketch::ProjectWorkspace workspace(document.snapshot());
    if (styled) {
      sketch::BoundaryAuthoringSession session(sketch::BoundaryAuthoringMode::define_first);
      session.set_classification("living_area"); (void)session.anchor({0, 0});
      (void)session.add_line_to({4, 0});
      sketch::BoundaryDimensionPresentation presentation;
      presentation.visible = false; presentation.rotation_radians = 1.5707963267948966;
      (void)session.place_manual_dimension({2, -1}, presentation);
      check(session.undo(), "styled extraction must carry a Redo-only placement");
      sketch::BoundaryActiveRecovery active{sketch::capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
      auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    }
    const auto captured = workspace.capture();
    auto history = sketch::capture_workspace_history_record(captured);
    history.extensions = {{"preserve", nlohmann::json::array({nullptr, 1, "raw"})}};
    sketch::RecoveryLedger recovery{{
        "z-history", "workspace_history",
        sketch::encode_workspace_history_record(captured.document(), history,
                                                 captured.active_boundary())}};
    if (captured.active_boundary()) recovery.push_back({"active", "boundary_active",
        sketch::encode_boundary_active_recovery(*captured.active_boundary())});
    if (role == sketch::ArchiveRole::recovery_copy) {
      sketch::RecoveryCopyRecord copy;
      copy.archive_id = "archive-exchange-copy";
      copy.owner_token = "archive-exchange-owner";
      copy.document_id = captured.document().document_id();
      copy.workspace_epoch = history.workspace_epoch;
      copy.edited_generation = history.edited_generation;
      copy.checkpoint_generation = history.checkpoint_generation;
      copy.explicitly_saved_document_revision =
          captured.document().saved_revision_optional();
      recovery.push_back({"m-copy", "recovery_copy",
                          sketch::encode_recovery_copy_record(copy)});
    }
    const sketch::ProjectArchiveSnapshot source(captured.document(), recovery,
                                                 role);
    const auto prefix = styled ? std::string("styled-") : std::string{};
    const auto archive_path = root / (prefix + (role == sketch::ArchiveRole::ordinary
                                          ? "exchange-ordinary.bldproj"
                                          : "exchange-recovery.bldproj"));
    (void)sketch::ProjectStore::save_archive(archive_path, source);
    const auto loaded = sketch::ProjectStore::load_archive(archive_path, role);
    check(loaded.supported() && loaded.archive.has_value(),
          "archive extraction fixture must reopen in its recorded role");

    const auto destination = root / (prefix + (role == sketch::ArchiveRole::ordinary
                                         ? "exchange-ordinary"
                                         : "exchange-recovery"));
    sketch::extract_project_archive(*loaded.archive, destination);
    std::ifstream input(destination / "project.json");
    const auto exported = nlohmann::json::parse(input);
    check(exported.at("exchange_version") == 12,
          "archive extraction must advertise the recovery-aware interchange version");
    check(exported.at("archive_role") ==
              (role == sketch::ArchiveRole::ordinary ? "ordinary" : "recovery_copy"),
          "archive extraction must retain its role");
    auto expected = nlohmann::json::array();
    for (const auto& record : loaded.archive->recovery())
      expected.push_back({{"record_id", record.record_id},
                          {"record_kind", record.record_kind},
                          {"envelope", record.envelope}});
    check(exported.at("recovery_records") == expected,
          "archive extraction must retain every raw recovery record");
    nlohmann::json history_envelope;
    for (const auto& record : exported.at("recovery_records"))
      if (record.at("record_id") == "z-history")
        history_envelope = record.at("envelope");
    check(history_envelope.at("extensions").at("preserve") ==
              nlohmann::json::array({nullptr, 1, "raw"}),
          "archive extraction must retain unknown recovery envelope fields");
    check(exported.at("revisions").size() ==
              loaded.archive->document().history().size(),
          "archive extraction must retain complete document revision history");

    input.close();
    std::ifstream raw_input(destination / "project.json", std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(raw_input)), {});
    bool refused = false;
    try {
      sketch::extract_project_archive(*loaded.archive, destination);
    } catch (const std::exception&) {
      refused = true;
    }
    check(refused, "archive extraction must refuse an existing destination");
    std::ifstream after_input(destination / "project.json", std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(after_input)), {});
    check(after == original,
          "refused archive extraction must preserve target bytes");

    auto unsupported_records = loaded.archive->recovery();
    unsupported_records.front().record_kind = "future_workspace_history";
    const sketch::ProjectArchiveSnapshot unsupported(
        loaded.archive->document(), unsupported_records, role);
    const auto unsupported_destination = destination.string() + "-unsupported";
    refused = false;
    try {
      sketch::extract_project_archive(unsupported, unsupported_destination);
    } catch (const std::exception&) {
      refused = true;
    }
    check(refused && !std::filesystem::exists(unsupported_destination),
          "direct archive extraction must refuse unsupported recovery before publishing");
  }
}
} // namespace
int main() {
  sketch::testing::noninteractive_errors();
  auto root = std::filesystem::temp_directory_path() /
              ("property-exchange-" + sketch::make_stable_id());
  std::filesystem::create_directory(root);
  try {
    test_grouped_measured_region_exchange(root);
    test_physical_curve_length_exchange_v9(root);
    test_direct_curve_length_exchange_v10(root);
    test_rigid_curve_archive_exchange_v11(root);
    test_archive_extraction(root);
    test_archive_extraction(root, true);
    test_straight_wall_only_authoring_exchange_v8(root);
    test_safe_curved_wall_history_exchange_replays(root);
    const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{0},
                                       std::byte{255}};
    auto asset =
        sketch::Asset::create("asset-a", "application/octet-stream", bytes);
    auto document = sketch::Document::create({}, {asset});
    auto duplicate = asset;
    duplicate.id = "asset-b";
    document.apply(
        sketch::ApplyEntityChanges{document.revision(),
                                   {},
                                   {sketch::AssetChange::upsert(duplicate)},
                                   "add repeated content"});
    const auto target = root / L"export-\u4F4F\u5B85";
    sketch::extract_project(document.snapshot(), target);
    std::ifstream input(target / "project.json");
    auto json = nlohmann::json::parse(input);
    input.close();
    check(json.at("exchange_format") == "vertex-json-assets",
          "New exports must use the Vertex exchange identifier");
    auto rows = json.at("revisions");
    check(json.at("exchange_version") == 1 && !rows[0].contains("boundary_translation"),
          "proof-free exports must preserve the existing format");
    check(rows.size() == 2, "All revisions must be extracted");
    auto asset_path = rows[1].at("assets")[0].at("path").get<std::string>();
    check(asset_path == "assets/" + asset.sha256 + ".bin",
          "Asset path must be portable and content-addressed");
    std::ifstream asset_input(target / std::filesystem::path(asset_path),
                              std::ios::binary);
    std::vector<char> actual((std::istreambuf_iterator<char>(asset_input)), {});
    asset_input.close();
    check(actual.size() == bytes.size() &&
              static_cast<unsigned char>(actual.back()) == 255,
          "Extracted bytes must match");
    check(std::distance(std::filesystem::directory_iterator(target / "assets"),
                        {}) == 1,
          "Repeated content must be deduplicated");
    bool rejected = false;
    try {
      sketch::extract_project(document.snapshot(), target);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected && std::filesystem::exists(target / "project.json"),
          "Existing extraction must remain untouched");
    const auto directory_names = [&] {
      std::set<std::filesystem::path> names;
      for (const auto& entry : std::filesystem::directory_iterator(root))
        names.insert(entry.path().filename());
      return names;
    };
    const auto retained_entries = directory_names();
    for (auto fault : {sketch::ExtractFaultStage::after_assets,
                       sketch::ExtractFaultStage::before_publish,
                       sketch::ExtractFaultStage::after_publish}) {
      rejected = false;
      try {
        sketch::extract_project(document.snapshot(), root / "fault", {fault});
      } catch (const std::exception &) {
        rejected = true;
      }
      check(rejected, "Injected extraction failure must report an error");
      check(!std::filesystem::exists(root / "fault"),
            "Failure must not publish a partial destination");
      check(directory_names() == retained_entries,
            "Failure must clean all private staging data");
    }
    // A live extraction owns directory and payload identities until
    // publication.
    sketch::ExtractOptions tamper;
    tamper.stage_observer = [&](auto stage,
                                const std::filesystem::path &staging) {
      if (stage != sketch::ExtractFaultStage::before_publish)
        return;
      const auto displaced = root / L"displaced";
      check(!MoveFileExW(staging.c_str(), displaced.c_str(), 0),
            "Staging directory must deny rename-and-plant interference");
      check(!CreateDirectoryW(staging.c_str(), nullptr),
            "Reserved staging path must remain occupied");
      check(!MoveFileExW((staging / L"assets").c_str(),
                         (root / L"moved-assets").c_str(), 0),
            "Assets directory must deny rename during construction");
      for (const auto &path :
           {staging / L"project.json", staging / asset_path}) {
        const auto writer =
            CreateFileW(path.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writer != INVALID_HANDLE_VALUE)
          CloseHandle(writer);
        check(writer == INVALID_HANDLE_VALUE,
              "Payload must deny mutation before publication");
        check(!DeleteFileW(path.c_str()), "Payload must deny deletion");
        check(!MoveFileExW(path.c_str(), (root / L"moved-payload").c_str(), 0),
              "Payload must deny rename");
      }
    };
    sketch::extract_project(document.snapshot(), root / "protected", tamper);
    check(std::filesystem::exists(root / "protected" / "project.json"),
          "Protected directory must still publish successfully");

    // Once the exact root has been renamed, every expected descendant is
    // reopened relative to that retained root and revalidated. A mutation in
    // the unavoidable close-to-rename interval must therefore roll back.
    std::filesystem::path post_publish_staging;
    sketch::ExtractOptions post_publish_tamper;
    post_publish_tamper.stage_observer =
        [&](auto stage, const std::filesystem::path &path) {
          if (stage == sketch::ExtractFaultStage::before_publish) {
            post_publish_staging = path;
          } else if (stage == sketch::ExtractFaultStage::after_publish) {
            check(path == root / L"post-publish-tamper",
                  "Post-publication observer must receive the destination");
            std::ofstream output(path / L"project.json",
                                 std::ios::binary | std::ios::app);
            check(static_cast<bool>(output),
                  "Post-publication fixture must be able to mutate payload");
            output << "tamper";
          }
        };
    rejected = false;
    try {
      sketch::extract_project(document.snapshot(),
                              root / L"post-publish-tamper",
                              post_publish_tamper);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected &&
              !std::filesystem::exists(root / L"post-publish-tamper") &&
              !std::filesystem::exists(post_publish_staging),
          "Post-publication tamper must be detected, rolled back, and cleaned");

    // If another object occupies the reserved staging leaf after publication,
    // rollback must fail closed and identify the contaminated destination.
    std::filesystem::path rollback_staging;
    const auto rollback_target = root / L"rollback-collision";
    sketch::ExtractOptions rollback_collision;
    rollback_collision.stage_observer =
        [&](auto stage, const std::filesystem::path &path) {
          if (stage == sketch::ExtractFaultStage::before_publish) {
            rollback_staging = path;
          } else if (stage == sketch::ExtractFaultStage::after_publish) {
            std::ofstream output(path / L"project.json",
                                 std::ios::binary | std::ios::app);
            check(static_cast<bool>(output),
                  "Rollback fixture must mutate the published payload");
            output << "tamper";
            output.close();
            check(CreateDirectoryW(rollback_staging.c_str(), nullptr),
                  "Rollback fixture must reserve the old staging leaf");
            std::ofstream(rollback_staging / L"foreign-marker.txt") << "keep";
          }
        };
    bool rollback_reported = false;
    try {
      sketch::extract_project(document.snapshot(), rollback_target,
                              rollback_collision);
    } catch (const sketch::ExtractionCleanupError &error) {
      rollback_reported = true;
      check(error.residual_path() == rollback_target,
            "Blocked rollback must report the contaminated destination");
      check(std::string(error.what()).find("post-publication") !=
                std::string::npos,
            "Blocked rollback must preserve the validation failure");
    }
    check(rollback_reported &&
              std::filesystem::exists(rollback_target / L"project.json") &&
              std::filesystem::exists(rollback_staging /
                                      L"foreign-marker.txt"),
          "Blocked rollback must preserve destination and foreign collision");
    std::filesystem::remove_all(rollback_target);
    std::filesystem::remove_all(rollback_staging);

    std::filesystem::path residual;
    {
      sketch::ExtractOptions options;
      options.fault_stage = sketch::ExtractFaultStage::before_publish;
      options.stage_observer = [&](auto stage,
                                   const std::filesystem::path &staging) {
        if (stage != sketch::ExtractFaultStage::before_publish)
          return;
        std::ofstream(staging / L"foreign-marker.txt")
            << "must survive cleanup";
      };
      bool reported = false;
      try {
        sketch::extract_project(document.snapshot(), root / "cleanup-denied",
                                options);
      } catch (const sketch::ExtractionCleanupError &error) {
        reported = true;
        residual = error.residual_path();
        check(std::string(error.what())
                          .find("Injected extraction write failure") !=
                      std::string::npos &&
                  std::string(error.what()).find("cleanup failed") !=
                      std::string::npos,
              "Cleanup error must preserve both failure causes");
        check(residual.parent_path() == std::filesystem::canonical(root) &&
                  std::filesystem::exists(residual / L"foreign-marker.txt"),
              "Cleanup failure must identify the exact remaining private "
              "staging directory");
      }
      check(reported,
            "Foreign contents must survive with an exact residual diagnostic");
      check(!std::filesystem::exists(root / "cleanup-denied"),
            "Blocked cleanup must never publish partial output");
    }
    check(residual.parent_path() == std::filesystem::canonical(root),
          "Cleanup retry must remain within this test's unique directory");
    std::filesystem::remove_all(residual);
    check(!std::filesystem::exists(residual),
          "Cleanup succeeds once the blocking handle is released");
    // A creator racing the final rename owns its destination. Failed
    // publication must clean only our released, identity-checked children.
    sketch::ExtractOptions collision;
    collision.stage_observer = [&](auto stage, const auto &) {
      if (stage == sketch::ExtractFaultStage::before_publish) {
        std::filesystem::create_directory(root / L"racing-destination");
        std::ofstream(root / L"racing-destination" / L"marker") << "keep";
      }
    };
    rejected = false;
    const auto before_collision =
        std::distance(std::filesystem::directory_iterator(root), {});
    try {
      sketch::extract_project(document.snapshot(), root / L"racing-destination",
                              collision);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(
        rejected &&
            std::filesystem::exists(root / L"racing-destination" / L"marker") &&
            !std::filesystem::exists(root / L"racing-destination" /
                                     L"project.json"),
        "Racing destination must survive unchanged");
    check(std::distance(std::filesystem::directory_iterator(root), {}) ==
              before_collision + 1,
          "Failed publication must clean its released descendants by identity");

    // Foreign additions are rejected even without an injected write fault.
    sketch::ExtractOptions addition;
    addition.stage_observer = [&](auto stage, const auto &staging) {
      if (stage == sketch::ExtractFaultStage::before_publish)
        std::ofstream(staging / L"foreign-marker.txt") << "keep";
    };
    residual.clear();
    try {
      sketch::extract_project(document.snapshot(), root / L"foreign-addition",
                              addition);
    } catch (const sketch::ExtractionCleanupError &error) {
      residual = error.residual_path();
      check(std::string(error.what()).find("Unexpected content") !=
                std::string::npos,
            "Unexpected staging contents must block publication");
    }
    check(!residual.empty() &&
              std::filesystem::exists(residual / L"foreign-marker.txt") &&
              !std::filesystem::exists(root / L"foreign-addition"),
          "Foreign additions must remain unpublished and preserved");
    check(residual.parent_path() == std::filesystem::canonical(root),
          "Foreign-marker fixture cleanup must stay inside the unique test "
          "directory");
    std::filesystem::remove_all(residual);
    // Only this test's unique, resolved temporary tree is removed.
    // Retained appraisal evidence has a reader floor even after Undo/deletion.
    for (const unsigned policy_version : {1U,2U}) {
    auto ansi_property = sketch::Entity{"exchange-ansi", "property",
        {{"appraisal_policy", {{"policy_kind", "ansi_z765_2021"}, {"version", policy_version}}}}, false, nlohmann::json::object()};
    auto ansi_document = sketch::Document::create({});
    ansi_document.apply(sketch::ApplyEntityChanges{0, {sketch::EntityChange::upsert(ansi_property)}, {}, "ANSI rules"});
    const auto ansi_head = ansi_document.snapshot();
    auto ansi_deleted = sketch::Document::fork(ansi_head);
    ansi_deleted.apply(sketch::ApplyEntityChanges{ansi_deleted.revision(),
        {sketch::EntityChange::erase(ansi_property.id)}, {}, "Remove property"});
    ansi_document.undo(ansi_document.revision());
    int ansi_sequence = 0;
    for (const auto& snapshot : {ansi_head, ansi_document.snapshot(), ansi_deleted.snapshot()}) {
      const auto destination = root / ("ansi-evidence-v" + std::to_string(policy_version) + "-" + std::to_string(ansi_sequence++));
      sketch::extract_project(snapshot, destination);
      std::ifstream input(destination / "project.json");
      const auto encoded = nlohmann::json::parse(input);
      check(encoded.at("exchange_version") == (policy_version==2?30:19) && encoded.at("revisions").size() == snapshot.history().size(),
          "current, undone and deleted appraisal evidence must advertise the recorded rule reader floor without dropping history");
    }
    }
    test_view_appearance_export_floor(root);
    test_appraisal_reporting_export_floor(root);
    test_live_exterior_source_exchange_v17(root);
    test_live_exterior_source_exchange_v17(root, true);
    test_live_exterior_source_exchange_v17(root, true,true);
    test_selected_rigid_curve_exchange_v25(root);
    test_selected_rigid_curve_exchange_v25(root,true);
    test_svg_palette_export_floor(root);
    test_translation_export(root);
    test_reviewed_source_export_floor(root);
    test_automatic_angle_redraw_export_floor(root);
    test_curved_constraint_export_floor(root);
    check(std::filesystem::equivalent(
              std::filesystem::canonical(root).parent_path(),
              std::filesystem::temp_directory_path()),
          "Unexpected test cleanup path");
    std::filesystem::remove_all(root);
    std::cout << "Project exchange tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    if (std::filesystem::equivalent(
            std::filesystem::canonical(root).parent_path(),
            std::filesystem::temp_directory_path()))
      std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
