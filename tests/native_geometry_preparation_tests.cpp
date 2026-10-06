#include "sketch/visualization/native_geometry_preparation.hpp"
#include "sketch/document.hpp"
#include "sketch/workspace_regeneration_queue.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/project_visibility.hpp"
#include "sketch/building_entity.hpp"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include "support/noninteractive_errors.hpp"
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <atomic>
#include <chrono>
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

void test_hosted_stair_dependencies() {
    StairFlight stair{"stair", {0, 0, 0}, 0, 8, 2.0, 0.25, 1.0};
    stair.flights = {{"lower", 4}, {"upper", 4}};
    stair.landings = {{"turn", 1.0, 0.15, StairTurn::left_quarter, 0.0}};
    auto source = encode_building_entity(stair);
    architectural_context(source);
    source.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.25}};
    Railing railing{"rail", {}, 0, 0, 0.9, 0.05, 0.5};
    railing.host = StairRailingHost{"stair", "upper", StairRailingSide::left, 0, 1};
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
        test_assembly_identity();
        test_resolved_join_and_opening_identity();
        test_opening_assembly_obeys_its_drawing_layer_and_floor();
        test_terrain_identity();
        test_hosted_stair_dependencies();
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
