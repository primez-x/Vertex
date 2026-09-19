#include "sketch/visualization/native_geometry_preparation.hpp"
#include "sketch/architecture.hpp"
#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_visibility.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/terrain_surface.hpp"
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>

namespace sketch::visualization {
namespace {
void append_entity_content(std::string& result, const Entity& entity) {
    result.append(entity.id);
    result.push_back('\0');
    result.append(entity.type);
    result.push_back('\0');
    result.append(entity.required ? "required" : "optional");
    result.push_back('\0');
    auto geometry_properties = entity.properties;
    // Material assignment is presentation state, never a shape input.
    geometry_properties.erase("material_assignment");
    result.append(geometry_properties.dump());
    result.push_back('\0');
    result.append(entity.extensions.dump());
    result.push_back('\0');
}

void mesh_shape(const TopoDS_Shape& shape) {
    // These are newly constructed worker-owned shapes; meshing never touches
    // topology already installed in a live AIS presentation.
    BRepMesh_IncrementalMesh mesh(shape, 0.001, false, 0.5, false);
    if (!mesh.IsDone()) throw std::runtime_error("native shape meshing failed");
}

std::string entity_content(const Entity& entity,
                           const std::vector<const Entity*>& hosted_openings = {}) {
    std::string canonical;
    append_entity_content(canonical, entity);
    for (const auto* opening : hosted_openings) {
        if (opening != nullptr) {
            append_entity_content(canonical, *opening);
        }
    }
    return canonical;
}

void append_unique(std::vector<std::string>& messages, std::string message) {
    if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
        messages.push_back(std::move(message));
    }
}

bool is_ignored_hierarchy_type(std::string_view type) {
    static constexpr std::string_view ignored[] = {
        "property",          "building",        "floor",       "layer",      "label",
        "sheet",             "view",            "constraint",  "annotation", "dimension",
        "annotation_state",  "sheet_view_model", "boundary", "measurement_boundary",
        "reference_asset",   "assembly_model",    "model_phases", "room_relationships",
        "vertical_levels",   "room_boundary",     "terrain_surface"};
    return std::find(std::begin(ignored), std::end(ignored), type) != std::end(ignored);
}

bool is_pending_geometry_type(std::string_view type) {
    // Architectural rooms now have a native semantic volume when their
    // explicit boundary, height, and elevation fields are present.  Keep this
    // helper for future bounded geometry types without treating rooms as a
    // permanent placeholder category.
    (void)type;
    return false;
}

} // namespace

std::optional<PreparedNativeGeometry> prepare_native_geometry(
    const DocumentSnapshot& snapshot, std::optional<NativeGeometryVisibleIds> visible_ids,
    const std::function<bool()>& cancelled,
    const std::function<void(std::size_t)>& progress) {
    PreparedNativeGeometry result;
    result.revision = snapshot.revision();
    result.visible_ids = visible_ids;
    auto& errors = result.errors;
    auto& pending = result.pending;
    auto& solids = result.solids;
    const auto& entities = snapshot.entities();
    std::map<std::pair<std::string, std::string>, std::string> material_colors;
    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (entity.type != "assembly_model") continue;
        try {
            const auto catalog = AssemblyModel::from_json(entity.properties.at("model"));
            for (const auto& material : catalog.materials())
                if (material.color_srgb) material_colors[{id, material.id}] = *material.color_srgb;
        } catch (const std::exception& error) {
            append_unique(errors, "material catalog '" + id + "': " + error.what());
        }
    }
    std::set<std::string, std::less<>> wall_ids;
    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (entity.type == "wall") {
            wall_ids.insert(id);
        }
    }

    // A fused join replaces its sources only while the join and every
    // member are visible. Re-derive on each mask transition; source entities
    // and document history remain authoritative and unchanged.
    const auto join_presentation_ids = derived_join_presentation_entities(
        snapshot, visible_ids ? *visible_ids : visible_project_entities(snapshot, {}));

    std::map<std::string, std::vector<const Entity*>, std::less<>> openings_by_wall;
    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (entity.type != "opening") {
            continue;
        }
        std::string wall_id;
        std::string relation_error;
        if (!read_document_wall_id(entity, wall_id, relation_error)) {
            append_unique(pending, "opening '" + id + "': " + relation_error);
        } else if (!wall_ids.contains(wall_id)) {
            append_unique(pending, "opening '" + id + "' references missing wall '" + wall_id + "'");
        } else {
            openings_by_wall[wall_id].push_back(&entity);
        }
    }

    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        // Suppress visible members owned by a fused join. Hidden members
        // still pass through geometry validation below even when their
        // presentation will be hidden.
        if ((entity.type == "wall" || entity.type == "roof") &&
            (!visible_ids || visible_ids->contains(id)) &&
            !join_presentation_ids.contains(id)) {

            continue;
        }
        if (entity.type == "opening" && entity.properties.contains("opening_assembly")) {
            std::string wall_id;
            std::string relation_error;
            if (!read_document_wall_id(entity, wall_id, relation_error)) {
                append_unique(errors, "opening assembly '" + id + "': " + relation_error);

                continue;
            }
            const auto host = entities.find(wall_id);
            if (host == entities.end() || host->second.type != "wall") {
                append_unique(errors, "opening assembly '" + id +
                                         "' references missing wall '" + wall_id + "'");

                continue;
            }
            try {
                const auto resolved_host = resolve_vertical_placement(snapshot, host->second);
                Wall host_wall;
                std::string parse_error;
                if (!read_document_wall(resolved_host, openings_by_wall[wall_id],
                                        host_wall, parse_error)) {
                    throw std::invalid_argument(parse_error);
                }
                const auto hosted = std::find_if(host_wall.openings.begin(),
                                                 host_wall.openings.end(),
                                                 [&](const HostedOpening& candidate) {
                                                     return candidate.id == id;
                                                 });
                if (hosted == host_wall.openings.end()) {
                    throw std::invalid_argument("opening is not present on its host wall");
                }
                const auto assembly = parse_opening_assembly(
                    entity.properties.at("opening_assembly"));
                std::optional<DoorOperation> operation;
                if (assembly.kind == OpeningAssemblyKind::door &&
                    entity.properties.contains("door_operation")) {
                    operation = decode_door_operation(entity.properties.at("door_operation"));
                }
                auto content = entity_content(resolved_host, openings_by_wall[wall_id]);
                append_entity_content(content, entity);
                std::optional<std::string> material_color;
                if (entity.properties.contains("material_assignment")) {
                    const auto& assignment = entity.properties.at("material_assignment");
                    const auto found = material_colors.find({
                        assignment.at("catalog_id").get<std::string>(),
                        assignment.at("material_id").get<std::string>()});
                    if (found != material_colors.end()) material_color = found->second;
                }
                auto presentation_color = assembly.kind == OpeningAssemblyKind::door
                    ? Quantity_Color(0.92, 0.58, 0.28, Quantity_TOC_RGB)
                    : Quantity_Color(0.30, 0.78, 0.88, Quantity_TOC_RGB);
                if (material_color) {
                    const QColor color(QString::fromStdString(*material_color));
                    if (color.isValid()) {
                        presentation_color = Quantity_Color(color.redF(), color.greenF(),
                                                           color.blueF(), Quantity_TOC_sRGB);
                    }
                }

                const auto shape = make_opening_assembly(host_wall, *hosted, assembly,
                                                         operation);
                if (cancelled && cancelled()) return std::nullopt;
                mesh_shape(shape);
                const bool visible = !visible_ids || visible_ids->contains(id) ||
                                     visible_ids->contains(wall_id);
                solids.emplace(id, PreparedNativeSolid{std::move(content), shape,
                                presentation_color, material_color, visible});
                if (progress) progress(solids.size());
            } catch (const std::exception& error) {
                append_unique(errors, "opening assembly '" + id + "': " + error.what());

            } catch (...) {
                append_unique(errors, "opening assembly '" + id + "': unknown OCCT failure");

            }
            continue;
        }
        if (entity.type != "wall" && entity.type != "slab" && entity.type != "room" &&
            entity.type != "terrain_surface" && entity.type != "wall_join" &&
            entity.type != "roof_join" &&
            !can_recognize_building_entity_type(entity.type)) {
            if (entity.type == "opening") {
                continue;
            }
            if (is_pending_geometry_type(entity.type)) {
                append_unique(pending, "entity '" + id + "' of type '" + entity.type +
                                         "' has no native solid representation yet");
            } else if (!is_ignored_hierarchy_type(entity.type)) {
                append_unique(pending, "entity '" + id + "' of unsupported type '" + entity.type +
                                         "' is pending native geometry");
            }
            continue;
        }

        Entity geometry_entity;
        try {
            geometry_entity = resolve_vertical_placement(snapshot, entity);
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());

            continue;
        }

        const auto hosted = entity.type == "wall"
                                ? openings_by_wall[id]
                                : std::vector<const Entity*>{};
        auto content = entity_content(geometry_entity, hosted);
        if (geometry_entity.type == "wall_join") {
            try {
                const auto join = parse_wall_join(geometry_entity.properties, id);
                for (const auto& wall_id : join.wall_ids) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto source = entities.find(wall_id);
                    if (source != entities.end()) {
                        append_entity_content(content, resolve_vertical_placement(snapshot, source->second));
                        for (const auto* opening : openings_by_wall[wall_id]) {
                            if (opening != nullptr) append_entity_content(content, *opening);
                        }
                    }
                }
            } catch (const std::exception& error) {
                append_unique(errors, "wall join '" + id + "': " + error.what());
            }
        }
        if (geometry_entity.type == "roof_join") {
            try {
                const auto join = parse_roof_join(geometry_entity.properties, id);
                for (const auto& roof_id : join.roof_ids) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto source = entities.find(roof_id);
                    if (source != entities.end())
                        append_entity_content(content, resolve_vertical_placement(snapshot, source->second));
                }
            } catch (const std::exception& error) {
                append_unique(errors, "roof join '" + id + "': " + error.what());
            }
        }
        std::optional<std::string> material_color;
        if (geometry_entity.properties.contains("material_assignment")) {
            const auto& assignment = geometry_entity.properties.at("material_assignment");
            const auto found = material_colors.find({assignment.at("catalog_id").get<std::string>(),
                assignment.at("material_id").get<std::string>()});
            if (found != material_colors.end()) material_color = found->second;
        }
        auto presentation_color = entity.type == "wall"
            ? Quantity_Color(0.84, 0.66, 0.32, Quantity_TOC_RGB)
            : entity.type == "terrain_surface"
                ? Quantity_Color(0.47, 0.64, 0.44, Quantity_TOC_RGB)
                : Quantity_Color(0.46, 0.70, 0.86, Quantity_TOC_RGB);
        if (material_color) {
            const QColor color(QString::fromStdString(*material_color));
            presentation_color = Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB);
        }

        std::string parse_error;
        TopoDS_Shape shape;
        try {
            if (geometry_entity.type == "wall") {
                Wall wall;
                if (!read_document_wall(geometry_entity, hosted, wall, parse_error)) {
                    append_unique(errors, "wall '" + id + "': " + parse_error);

                    continue;
                }
                shape = make_wall(wall);
            } else if (geometry_entity.type == "wall_join") {
                const auto join = parse_wall_join(geometry_entity.properties, id);
                std::vector<Wall> source_walls;
                source_walls.reserve(join.wall_ids.size());
                for (const auto& wall_id : join.wall_ids) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto source = entities.find(wall_id);
                    if (source == entities.end() || source->second.type != "wall") {
                        throw std::invalid_argument("wall join source wall is missing: " + wall_id);
                    }
                    const auto resolved_source = resolve_vertical_placement(snapshot,
                                                                             source->second);
                    Wall wall;
                    std::string error;
                    if (!read_document_wall(resolved_source, openings_by_wall[wall_id],
                                            wall, error)) {
                        throw std::invalid_argument(error);
                    }
                    source_walls.push_back(std::move(wall));
                }
                shape = make_wall_join(join, source_walls);
            } else if (geometry_entity.type == "roof_join") {
                const auto join = parse_roof_join(geometry_entity.properties, id);
                std::vector<TopoDS_Shape> source_roofs;
                source_roofs.reserve(join.roof_ids.size());
                for (const auto& roof_id : join.roof_ids) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto source = entities.find(roof_id);
                    if (source == entities.end() || source->second.type != "roof") {
                        throw std::invalid_argument("roof join source roof is missing: " + roof_id);
                    }
                    const auto resolved_source = resolve_vertical_placement(snapshot,
                                                                             source->second);
                    source_roofs.push_back(make_building_shape(
                        decode_building_entity(resolved_source)));
                }
                shape = make_roof_join(join, source_roofs);
            } else if (geometry_entity.type == "slab") {
                Slab slab;
                if (!read_document_slab(geometry_entity, slab, parse_error)) {
                    append_unique(errors, "slab '" + id + "': " + parse_error);

                    continue;
                }
                shape = make_slab(slab);
            } else if (geometry_entity.type == "terrain_surface") {
                shape = make_terrain_surface(
                    TerrainSurface::from_json(geometry_entity.properties.at("model")));
            } else if (geometry_entity.type == "room") {
                RoomVolume room;
                if (!read_document_room(geometry_entity, room, parse_error)) {
                    append_unique(errors, "room '" + id + "': " + parse_error);

                    continue;
                }
                shape = make_room_volume(room);
            } else {
                shape = make_building_shape(decode_building_entity(geometry_entity));
            }
            if (shape.IsNull()) {
                append_unique(errors, entity.type + " '" + id + "' produced a null solid");

                continue;
            }

            if (cancelled && cancelled()) return std::nullopt;
            mesh_shape(shape);
            solids.emplace(id, PreparedNativeSolid{std::move(content), std::move(shape),
                            presentation_color, material_color, join_presentation_ids.contains(id)});
            if (progress) progress(solids.size());
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());

        } catch (...) {
            append_unique(errors, entity.type + " '" + id + "': unknown OCCT failure");

        }
    }

    const auto make_assembly_host_shape = [&](const std::string& host_id) -> TopoDS_Shape {
        const auto host = entities.find(host_id);
        if (host == entities.end()) {
            throw std::invalid_argument("assembly host is missing");
        }
        const auto& source = host->second;
        const auto geometry_entity = resolve_vertical_placement(snapshot, source);
        if (can_recognize_building_entity_type(geometry_entity.type)) {
            return make_building_shape(decode_building_entity(geometry_entity));
        }
        if (geometry_entity.type == "wall") {
            Wall wall;
            std::string error;
            if (!read_document_wall(geometry_entity, openings_by_wall[host_id], wall, error)) {
                throw std::invalid_argument(error);
            }
            return make_wall(wall);
        }
        if (geometry_entity.type == "slab") {
            Slab slab;
            std::string error;
            if (!read_document_slab(geometry_entity, slab, error)) {
                throw std::invalid_argument(error);
            }
            return make_slab(slab);
        }
        if (geometry_entity.type == "room") {
            RoomVolume room;
            std::string error;
            if (!read_document_room(geometry_entity, room, error)) {
                throw std::invalid_argument(error);
            }
            return make_room_volume(room);
        }
        throw std::invalid_argument("assembly host has no native architectural solid");
    };
    const auto transform_assembly_shape = [](const TopoDS_Shape& source,
                                             const AssemblyPlacement& placement) {
        if (source.IsNull()) throw std::invalid_argument("assembly host solid is empty");
        if (!std::isfinite(placement.scale) || placement.scale <= 0.0 ||
            !std::isfinite(placement.rotation_radians) ||
            !std::isfinite(placement.translation_m.x) ||
            !std::isfinite(placement.translation_m.y)) {
            throw std::invalid_argument("assembly placement transform is invalid");
        }
        gp_Trsf scale;
        scale.SetScale(gp_Pnt(0.0, 0.0, 0.0), placement.scale);
        BRepBuilderAPI_Transform scaled(source, scale, true);
        if (!scaled.IsDone() || scaled.Shape().IsNull()) {
            throw std::invalid_argument("assembly scale transform failed");
        }
        gp_Trsf rotate;
        rotate.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
                           placement.rotation_radians);
        BRepBuilderAPI_Transform rotated(scaled.Shape(), rotate, true);
        if (!rotated.IsDone() || rotated.Shape().IsNull()) {
            throw std::invalid_argument("assembly rotation transform failed");
        }
        gp_Trsf translate;
        translate.SetTranslation(gp_Vec(placement.translation_m.x,
                                         placement.translation_m.y, 0.0));
        BRepBuilderAPI_Transform translated(rotated.Shape(), translate, true);
        if (!translated.IsDone() || translated.Shape().IsNull()) {
            throw std::invalid_argument("assembly translation transform failed");
        }
        return translated.Shape();
    };

    for (const auto& [catalog_id, catalog_entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (catalog_entity.type != "assembly_model" ||
            !catalog_entity.properties.contains("model")) continue;
        try {
            const auto catalog = AssemblyModel::from_json(catalog_entity.properties.at("model"));
            for (const auto& instance : catalog.instances()) {
                    if (cancelled && cancelled()) return std::nullopt;
                if (!instance.placement) continue;
                const auto child_id = catalog_id + ":instance:" + instance.id;
                const auto host = entities.find(instance.placement->host_entity_id);
                if (host == entities.end()) {
                    append_unique(errors, "assembly instance '" + child_id +
                                         "' references missing host '" +
                                         instance.placement->host_entity_id + "'");
                    continue;
                }
                const auto geometry_entity = resolve_vertical_placement(snapshot, host->second);
                std::string content;
                content.reserve(catalog_entity.properties.dump().size() +
                                geometry_entity.properties.dump().size() + child_id.size() + 32);
                content.append(child_id);
                content.push_back('\0');
                const auto& placement = *instance.placement;
                content.append(nlohmann::json{
                    {"host", placement.host_entity_id},
                    {"translation", {placement.translation_m.x, placement.translation_m.y}},
                    {"rotation", placement.rotation_radians}, {"scale", placement.scale}}.dump());
                content.push_back('\0');
                append_entity_content(content, geometry_entity);
                if (geometry_entity.type == "wall") {
                    for (const auto* opening : openings_by_wall[host->first]) {
                        if (opening != nullptr) append_entity_content(content, *opening);
                    }
                }
                std::optional<std::string> material_color;
                const auto resolved = catalog.resolve(instance.id);
                for (const auto& [slot, material_id] : resolved.materials) {
                    (void)slot;
                    const auto material = material_colors.find({catalog_id, material_id});
                    if (material != material_colors.end()) {
                        material_color = material->second;
                        break;
                    }
                }

                const auto presentation_color = [&] {
                    if (material_color) {
                        const QColor color(QString::fromStdString(*material_color));
                        if (color.isValid()) {
                            return Quantity_Color(color.redF(), color.greenF(), color.blueF(),
                                                   Quantity_TOC_sRGB);
                        }
                    }
                    return Quantity_Color(0.63, 0.48, 0.78, Quantity_TOC_RGB);
                }();

                TopoDS_Shape shape = transform_assembly_shape(
                    make_assembly_host_shape(instance.placement->host_entity_id),
                    *instance.placement);
                if (shape.IsNull()) {
                    append_unique(errors, "assembly instance '" + child_id + "' produced a null solid");

                    continue;
                }
                if (cancelled && cancelled()) return std::nullopt;
                mesh_shape(shape);
                solids.emplace(child_id, PreparedNativeSolid{std::move(content), std::move(shape),
                    presentation_color, material_color, !visible_ids || visible_ids->contains(child_id)});
                if (progress) progress(solids.size());
            }
        } catch (const std::exception& error) {
            append_unique(errors, "assembly catalog '" + catalog_id + "': " + error.what());
        } catch (...) {
            append_unique(errors, "assembly catalog '" + catalog_id + "': unknown OCCT failure");
        }
    }

    if (cancelled && cancelled()) return std::nullopt;
    return result;
}

struct NativeGeometryRegenerator::Impl {
    struct Request {
        DocumentSnapshot snapshot;
        std::optional<NativeGeometryVisibleIds> visible_ids;
        std::uint64_t sequence;
    };
    Preparation preparation;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::optional<Request> waiting;
    std::optional<PreparedNativeGeometry> completed;
    std::exception_ptr error;
    std::atomic<std::uint64_t> sequence{};
    std::atomic<bool> stopped{false};
    bool pending{};
    bool running{};
    bool finished{};
    std::thread worker;

    explicit Impl(Preparation prepare) : preparation(std::move(prepare)), worker([this] { run(); }) {}

    void run() {
        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return stopped || waiting.has_value(); });
            if (stopped) return;
            auto request = std::move(*waiting);
            waiting.reset();
            running = true;
            lock.unlock();
            std::optional<PreparedNativeGeometry> result;
            std::exception_ptr failure;
            try {
                result = preparation(request.snapshot, request.visible_ids, [&] {
                    return stopped || sequence.load() != request.sequence;
                });
                if (result && (result->revision != request.snapshot.revision() ||
                               result->visible_ids != request.visible_ids)) {
                    throw std::runtime_error("native preparation returned mismatched request identity");
                }
            } catch (...) { failure = std::current_exception(); }
            lock.lock();
            running = false;
            if (!stopped && sequence.load() == request.sequence) {
                completed = std::move(result);
                error = failure;
                finished = true;
            }
        }
    }
};

NativeGeometryRegenerator::NativeGeometryRegenerator()
    : NativeGeometryRegenerator([](const DocumentSnapshot& snapshot,
                                  std::optional<NativeGeometryVisibleIds> visible,
                                  const std::function<bool()>& cancelled) {
          return prepare_native_geometry(snapshot, std::move(visible), cancelled);
      }) {}
NativeGeometryRegenerator::NativeGeometryRegenerator(Preparation preparation)
    : impl_(std::make_unique<Impl>(std::move(preparation))) {}
NativeGeometryRegenerator::~NativeGeometryRegenerator() { shutdown(); }

void NativeGeometryRegenerator::request(DocumentSnapshot snapshot,
                                       std::optional<NativeGeometryVisibleIds> visible_ids) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) throw std::logic_error("native geometry regenerator is shut down");
    const auto sequence = ++impl_->sequence;
    // Replacement destroys the previous waiting snapshot (including history)
    // immediately, even while an OCCT operation cannot observe cancellation.
    impl_->waiting = Impl::Request{std::move(snapshot), std::move(visible_ids), sequence};
    impl_->completed.reset();
    impl_->error = {};
    impl_->finished = false;
    impl_->pending = true;
    impl_->ready.notify_one();
}

bool NativeGeometryRegenerator::is_pending() const noexcept { return impl_->pending; }

std::size_t NativeGeometryRegenerator::retained_snapshot_count() const {
    std::lock_guard lock(impl_->mutex);
    return std::size_t(impl_->running) + std::size_t(impl_->waiting.has_value());
}

std::optional<PreparedNativeGeometry> NativeGeometryRegenerator::take_completed() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->finished) return std::nullopt;
    impl_->pending = false;
    impl_->finished = false;
    if (auto error = std::exchange(impl_->error, {})) std::rethrow_exception(error);
    return std::exchange(impl_->completed, std::nullopt);
}

void NativeGeometryRegenerator::shutdown() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopped = true;
        impl_->waiting.reset();
        impl_->ready.notify_one();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->pending = false;
    impl_->finished = false;
    impl_->completed.reset();
    impl_->error = {};
}

} // namespace sketch::visualization
