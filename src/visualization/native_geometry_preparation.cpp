#include "sketch/visualization/native_geometry_preparation.hpp"
#include "sketch/architecture.hpp"
#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_visibility.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/opening_host_geometry.hpp"
#include "sketch/terrain_surface.hpp"
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
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

nlohmann::json assembly_boundary_content(const Boundary& boundary) {
    auto result = nlohmann::json::array();
    for (const auto& segment : boundary)
        result.push_back({{segment.start.x, segment.start.y},
                          {segment.end.x, segment.end.y}, segment.sweep_radians});
    return result;
}

// Cache only the resolved geometry inputs. Material slots/overrides and
// catalog colors have a separate appearance identity below.
std::string assembly_geometry_content(const std::string& id, const AssemblyExpansion& expansion) {
    auto profiles = nlohmann::json::array();
    for (const auto& source : expansion.profiles) {
        auto holes = nlohmann::json::array();
        for (const auto& hole : source.profile.holes) holes.push_back(assembly_boundary_content(hole));
        profiles.push_back({{"path", source.part_path}, {"type", source.type_id},
            {"profile", source.profile.id}, {"outer", assembly_boundary_content(source.profile.outer)},
            {"holes", std::move(holes)}, {"elevation", source.profile.elevation_m},
            {"height", source.profile.height_m}, {"transform", encode_assembly_transform(source.transform)}});
    }
    return nlohmann::json{{"entity", id}, {"profiles", std::move(profiles)}}.dump();
}

Entity effective_geometry_entity(const DocumentSnapshot& snapshot, const Entity& source) {
    if (source.type == "railing") {
        const auto object = decode_building_entity(source);
        if (const auto* rail = std::get_if<Railing>(&object); rail && (rail->host || rail->landing_host)) return source;
    }
    return resolve_vertical_placement(snapshot, source);
}

TopoDS_Shape place_native_shape(const TopoDS_Shape& source,
                               const SitePresentationPlacement& placement) {
    if (source.IsNull()) return source; // Fully occluded material region.
    const auto& pose=placement.forward;
    // The strict resolver has already validated the rigid frame. A zero pose
    // is a resolved identity, never a fallback for malformed persisted data.
    if (pose.rotation_radians==0 && pose.translation_m.x==0 &&
        pose.translation_m.y==0 && pose.translation_m.z==0) return source;
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),pose.rotation_radians);
    transform.SetTranslationPart(gp_Vec(pose.translation_m.x,pose.translation_m.y,pose.translation_m.z));
    BRepBuilderAPI_Transform placed(source,transform,true);
    if (!placed.IsDone() || placed.Shape().IsNull())
        throw std::invalid_argument("native site-frame transform failed");
    return placed.Shape();
}

void append_site_placement_content(std::string& content, const DocumentSnapshot& snapshot,
                                   const SitePresentationPlacement& placement) {
    // The full resolver digest also binds material assignments/catalog colors.
    // Retain that receipt on PreparedNativeSolid, but keep reusable geometry
    // separate from appearance by capturing only coordinate-frame dependencies.
    auto dependencies=nlohmann::json::array();
    for (const auto& id:placement.dependency_ids) {
        const auto& entity=snapshot.entities().at(id);
        auto frame=nlohmann::json::object();
        for (const auto* key:{"site_frame","site_placement","presentation_frame",
            "terrain_elevation_binding","property_id","building_id","floor_id","layer_id",
            "vertical_level_binding","vertical_placement","level_connection"}) {
            if (const auto found=entity.properties.find(key);found!=entity.properties.end())
                frame[key]=*found;
        }
        dependencies.push_back({{"id",id},{"type",entity.type},{"frame",std::move(frame)}});
    }
    const auto& pose=placement.forward;
    content.append(nlohmann::json{{"domain","native-site-placement-v1"},
        {"frame",{{"mode",static_cast<int>(placement.source_frame.mode)},
            {"property",placement.source_frame.property_id},{"building",placement.source_frame.building_id}}},
        {"translation",{pose.translation_m.x,pose.translation_m.y,pose.translation_m.z}},
        {"yaw",pose.rotation_radians},{"dependencies",std::move(dependencies)}}.dump()).push_back('\0');
}

// Cache identity retains the authored context inputs as well as effective
// coordinates. A level graph or organizational rebind can change a dependent
// rail even when its own persisted JSON is byte-for-byte unchanged.
void append_placement_content(std::string& content, const DocumentSnapshot& snapshot,
                              const Entity& source) {
    const auto& entities = snapshot.entities();
    const Entity* current = &source;
    std::set<std::string, std::less<>> visited;
    for (;;) {
        if (!visited.insert(current->id).second) break;
        append_entity_content(content, *current);
        for (const auto* binding : {"vertical_level_binding", "level_connection"}) {
            const auto value = current->properties.find(binding);
            if (value == current->properties.end() || !value->is_object()) continue;
            const auto graph_id = value->find("graph_id");
            if (graph_id == value->end() || !graph_id->is_string()) continue;
            const auto graph = entities.find(graph_id->get<std::string>());
            if (graph != entities.end()) append_entity_content(content, graph->second);
        }
        const Entity* parent = nullptr;
        for (const auto* key : {"layer_id", "floor_id", "building_id", "property_id"}) {
            const auto reference = current->properties.find(key);
            if (reference == current->properties.end() || !reference->is_string()) continue;
            const auto found = entities.find(reference->get<std::string>());
            if (found != entities.end()) { parent = &found->second; break; }
        }
        if (!parent) break;
        current = parent;
    }
}

void append_building_dependencies(std::string& content, const DocumentSnapshot& snapshot,
                                  const Entity& effective) {
    if (effective.type != "railing") return;
    const auto object = decode_building_entity(effective);
    const auto* rail = std::get_if<Railing>(&object);
    if (!rail || (!rail->host && !rail->landing_host)) return;
    const auto& stair_id = rail->host ? rail->host->stair_id : rail->landing_host->stair_id;
    const auto host = snapshot.entities().find(stair_id);
    if (host == snapshot.entities().end() || host->second.type != "stair")
        throw std::invalid_argument("hosted railing stair is missing");
    const auto resolved = resolve_vertical_placement(snapshot, host->second);
    const auto host_object = decode_building_entity(resolved);
    const auto* stair = std::get_if<StairFlight>(&host_object);
    if (!stair) throw std::invalid_argument("hosted railing source is not a supported stair");
    (void)derive_hosted_railing_layout(*rail, *stair);
    append_entity_content(content, resolved);
    append_placement_content(content, snapshot, host->second);
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
        "vertical_levels",   "room_boundary",     "terrain_surface", "ifc_source"};
    return std::find(std::begin(ignored), std::end(ignored), type) != std::end(ignored);
}

bool has_null_ifc_proxy_representation(std::string_view arguments) {
    // IFC4 proxies have nine fields. Inspect Representation (index six) as a
    // complete token, rather than trusting a suffix or commas inside strings.
    if (arguments.empty() || arguments.size() > 1024 * 1024) return false;
    std::size_t start = 0;
    std::size_t field_count = 0;
    std::size_t depth = 0;
    bool quoted = false;
    bool null_representation = false;
    const auto field = [&](std::size_t end) {
        auto value = arguments.substr(start, end - start);
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos || field_count == 9) return false;
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        if (field_count == 6) null_representation = value == "$";
        ++field_count;
        return true;
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto character = arguments[index];
        if (character == '\'') {
            if (quoted && index + 1 < arguments.size() && arguments[index + 1] == '\'')
                ++index;
            else quoted = !quoted;
            continue;
        }
        if (quoted) continue;
        if (character == '(') {
            if (++depth > 128) return false;
        } else if (character == ')') {
            if (depth == 0) return false;
            --depth;
        } else if (character == ',' && depth == 0) {
            if (!field(index)) return false;
            start = index + 1;
        }
    }
    return !quoted && depth == 0 && field(arguments.size()) &&
           field_count == 9 && null_representation;
}

bool is_inert_ifc_reference(const Entity& entity) {
    if (entity.type != "ifc_reference" || !entity.properties.is_object() ||
        !entity.extensions.is_object()) return false;
    // The exporter uses geometry-free proxies for retained organization and
    // receipt descriptors. A physical IFC record or an unclassified reference
    // must still report missing native geometry, even when hidden.
    const auto ifc_type = entity.properties.find("ifc_type");
    const auto source = entity.extensions.find("ifc_source");
    if (ifc_type == entity.properties.end() || *ifc_type != "IFCBUILDINGELEMENTPROXY" ||
        source == entity.extensions.end() || !source->is_object()) return false;
    const auto record_type = source->find("record_type");
    if (record_type == source->end() || *record_type != "IFCBUILDINGELEMENTPROXY") return false;
    const auto arguments = source->find("arguments");
    if (arguments == source->end() || !arguments->is_string() ||
        !has_null_ifc_proxy_representation(arguments->get_ref<const std::string&>())) return false;
    const auto metadata = entity.extensions.find("ifc_vertex_properties");
    if (metadata == entity.extensions.end() || !metadata->is_object() || metadata->size() != 1)
        return false;
    const auto native = metadata->find("native_entity");
    if (native == metadata->end() || !native->is_object() || native->size() != 5) return false;
    const auto id = native->find("id");
    const auto type = native->find("type");
    const auto required = native->find("required");
    const auto properties = native->find("properties");
    const auto extensions = native->find("extensions");
    if (id == native->end() || !id->is_string() || id->get_ref<const std::string&>().empty() ||
        type == native->end() || !type->is_string() ||
        required == native->end() || !required->is_boolean() ||
        properties == native->end() || !properties->is_object() ||
        extensions == native->end() || !extensions->is_object()) return false;
    static constexpr std::string_view inert_types[] = {
        "property", "building", "floor", "layer", "annotation_state", "ifc_source"};
    const std::string_view native_type = type->get_ref<const std::string&>();
    return std::find(std::begin(inert_types), std::end(inert_types), native_type) != std::end(inert_types);
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
    std::set<std::string, std::less<>> inactive_owner_ids;
    try {
        // Saved registry choices establish physical activity. Keep the complete
        // captured map for authority, relationships and assembly dependencies.
        inactive_owner_ids = constraint_phase_scope(entities).inactive_owner_ids;
    } catch (const std::exception& error) {
        append_unique(errors, "native phase scope: " + std::string(error.what()));
        return result;
    }
    if (!inactive_owner_ids.empty()) {
        for (const auto& [id, entity] : entities) {
            if (cancelled && cancelled()) return std::nullopt;
            if (inactive_owner_ids.contains(id)) continue;
            try {
                bool inactive_dependency = false;
                if (entity.type == "opening") {
                    std::string host_id;
                    std::string diagnostic;
                    if (read_document_wall_id(entity, host_id, diagnostic))
                        inactive_dependency = inactive_owner_ids.contains(host_id);
                } else if (entity.type == "wall_join") {
                    const auto join = parse_wall_join(entity.properties, id);
                    inactive_dependency = std::any_of(join.wall_ids.begin(), join.wall_ids.end(),
                        [&](const auto& member) { return inactive_owner_ids.contains(member); });
                } else if (entity.type == "roof_join") {
                    const auto join = parse_roof_join(entity.properties, id);
                    inactive_dependency = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
                        [&](const auto& member) { return inactive_owner_ids.contains(member); });
                } else if (entity.type == "railing") {
                    const auto object = decode_building_entity(entity);
                    if (const auto* rail = std::get_if<Railing>(&object)) {
                        inactive_dependency =
                            (rail->host && inactive_owner_ids.contains(rail->host->stair_id)) ||
                            (rail->landing_host && inactive_owner_ids.contains(rail->landing_host->stair_id));
                    }
                }
                if (inactive_dependency) inactive_owner_ids.insert(id);
            } catch (const std::exception&) {
                // Active malformed relationships retain the normal diagnostic
                // path below; they cannot establish inactive-owner authority.
            }
        }
    }
    // One captured batch shares organization, host/join and frame caches.
    // Hidden semantic owners are included because this preparation validates
    // them too; an invalid/cross-frame join must never reach local fusion.
    std::vector<std::string> site_owner_ids;
    for (const auto& [id,entity]:entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (inactive_owner_ids.contains(id)) continue;
        if (entity.type=="wall" || entity.type=="slab" || entity.type=="room" ||
            entity.type=="terrain_surface" || entity.type=="opening" || entity.type=="wall_join" ||
            entity.type=="roof_join" || entity.type=="assembly_instance" ||
            can_recognize_building_entity_type(entity.type)) site_owner_ids.push_back(id);
    }
    std::map<std::string,SitePresentationPlacement,std::less<>> site_placements;
    try { site_placements=resolve_site_presentations(snapshot,site_owner_ids); }
    catch (const std::exception& error) {
        append_unique(errors,"native site presentation: "+std::string(error.what()));
        return result;
    }
    if (cancelled && cancelled()) return std::nullopt;
    std::map<std::pair<std::string, std::string>, std::string> material_colors;
    std::set<std::pair<std::string, std::string>> material_bindings;
    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (entity.type != "assembly_model") continue;
        try {
            const auto catalog = AssemblyModel::from_json(entity.properties.at("model"));
            for (const auto& material : catalog.materials()) {
                material_bindings.emplace(id, material.id);
                if (material.color_srgb) material_colors[{id, material.id}] = *material.color_srgb;
            }
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
    auto active_visible_ids = visible_ids ? *visible_ids : visible_project_entities(snapshot, {});
    for (const auto& id : inactive_owner_ids) active_visible_ids.erase(id);
    const auto join_presentation_ids = derived_join_presentation_entities(snapshot, active_visible_ids);

    std::map<std::string, std::vector<const Entity*>, std::less<>> openings_by_wall;
    for (const auto& [id, entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (entity.type != "opening" || inactive_owner_ids.contains(id)) {
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
        if (inactive_owner_ids.contains(id)) continue;
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
                const auto& site_placement=site_placements.at(id);
                append_site_placement_content(content,snapshot,site_placement);
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

                const auto shape = place_native_shape(make_opening_assembly(host_wall, *hosted, assembly,
                                                         operation),site_placement);
                if (cancelled && cancelled()) return std::nullopt;
                mesh_shape(shape);
                const bool visible = !visible_ids || visible_ids->contains(id);
                solids.emplace(id, PreparedNativeSolid{std::move(content), shape,
                                presentation_color, material_color, visible, {}, {}, site_placement});
                if (progress) progress(solids.size());
            } catch (const std::exception& error) {
                append_unique(errors, "opening assembly '" + id + "': " + error.what());

            } catch (...) {
                append_unique(errors, "opening assembly '" + id + "': unknown OCCT failure");

            }
            continue;
        }
        // Independent assemblies are expanded together below, sharing the
        // document-wide budget with all legacy catalog instances.
        if (entity.type == "assembly_instance") continue;
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
            } else if (!is_ignored_hierarchy_type(entity.type) && !is_inert_ifc_reference(entity)) {
                append_unique(pending, "entity '" + id + "' of unsupported type '" + entity.type +
                                         "' is pending native geometry");
            }
            continue;
        }

        if (entity.type == "room" && !has_document_room_volume_fields(entity)) {
            // Historical rooms can be authoritative 2D footprints. Validate
            // their plan topology without inventing a solid or blocking the
            // complete 3D scene, just as analytical area boundaries are 2D.
            DocumentRoomFootprint footprint;
            std::string error;
            if (!read_document_room_footprint(entity, footprint, error))
                append_unique(errors, "room '" + id + "': " + error);
            continue;
        }

        Entity geometry_entity;
        try {
            geometry_entity = effective_geometry_entity(snapshot, entity);
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());

            continue;
        }

        const auto hosted = entity.type == "wall"
                                ? openings_by_wall[id]
                                : std::vector<const Entity*>{};
        auto content = entity_content(geometry_entity, hosted);
        try {
            append_building_dependencies(content, snapshot, geometry_entity);
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());
            continue;
        }
        const auto& site_placement=site_placements.at(id);
        append_site_placement_content(content,snapshot,site_placement);
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
        auto presentation_color = entity.type == "wall"
            ? Quantity_Color(0.84, 0.66, 0.32, Quantity_TOC_RGB)
            : entity.type == "terrain_surface"
                ? Quantity_Color(0.47, 0.64, 0.44, Quantity_TOC_RGB)
                : Quantity_Color(0.46, 0.70, 0.86, Quantity_TOC_RGB);
        try {
            if (geometry_entity.properties.contains("material_assignment")) {
                const auto& assignment = geometry_entity.properties.at("material_assignment");
                const auto found = material_colors.find({assignment.at("catalog_id").get<std::string>(),
                    assignment.at("material_id").get<std::string>()});
                if (found != material_colors.end()) material_color = found->second;
            }
            if (material_color) {
                const QColor color(QString::fromStdString(*material_color));
                if (!color.isValid()) throw std::invalid_argument("material color is invalid");
                presentation_color = Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB);
            }
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());
            continue;
        }

        std::string parse_error;
        TopoDS_Shape shape;
        std::vector<PreparedNativeMaterialRegion> material_regions;
        std::string appearance_content;
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
                std::vector<Entity> source_entities;
                source_roofs.reserve(join.roof_ids.size());
                for (const auto& roof_id : join.roof_ids) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto source = entities.find(roof_id);
                    if (source == entities.end() || source->second.type != "roof") {
                        throw std::invalid_argument("roof join source roof is missing: " + roof_id);
                    }
                    const auto resolved_source = resolve_vertical_placement(snapshot,
                                                                             source->second);
                    source_entities.push_back(resolved_source);
                    source_roofs.push_back(make_building_shape(
                        decode_building_entity(resolved_source)));
                }
                const auto partition = make_roof_join_partition(join, source_roofs);
                shape = partition.shape;
                for (std::size_t index = 0; index < partition.regions.size(); ++index) {
                    if (cancelled && cancelled()) return std::nullopt;
                    const auto& region = partition.regions[index];
                    const auto& binding_entity = geometry_entity.properties.contains("material_assignment")
                        ? geometry_entity : source_entities[index];
                    PreparedNativeMaterialRegion prepared;
                    prepared.source_id = region.source_roof_id;
                    prepared.shape = region.shape;
                    prepared.color = Quantity_Color(0.46, 0.70, 0.86, Quantity_TOC_RGB);
                    prepared.gross_volume = region.gross_volume;
                    prepared.net_volume = region.net_volume;
                    appearance_content.append(prepared.source_id).push_back('\0');
                    if (binding_entity.properties.contains("material_assignment")) {
                        const auto& assignment = binding_entity.properties.at("material_assignment");
                        prepared.catalog_id = assignment.at("catalog_id").get<std::string>();
                        prepared.material_id = assignment.at("material_id").get<std::string>();
                        const auto key = std::pair{*prepared.catalog_id, *prepared.material_id};
                        if (!material_bindings.contains(key))
                            throw std::invalid_argument("roof material assignment references a missing catalog material");
                        appearance_content.append(assignment.dump()).push_back('\0');
                        const auto color = material_colors.find(key);
                        if (color != material_colors.end()) {
                            prepared.material_color = color->second;
                            const QColor resolved(QString::fromStdString(color->second));
                            if (!resolved.isValid()) throw std::invalid_argument("roof material color is invalid");
                            prepared.color = Quantity_Color(resolved.redF(), resolved.greenF(),
                                                            resolved.blueF(), Quantity_TOC_sRGB);
                        }
                    } else {
                        appearance_content.append("default").push_back('\0');
                    }
                    appearance_content.append(prepared.material_color.value_or("default")).push_back('\0');
                    if (cancelled && cancelled()) return std::nullopt;
                    material_regions.push_back(std::move(prepared));
                }
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
                shape = make_building_shape(decode_building_entity(geometry_entity), entities);
            }
            if (shape.IsNull()) {
                append_unique(errors, entity.type + " '" + id + "' produced a null solid");

                continue;
            }

            if (cancelled && cancelled()) return std::nullopt;
            // Local vertical placement, openings/rails and joins are complete.
            // Transform fused truth and each independently colored region once.
            shape=place_native_shape(shape,site_placement);
            for (auto& region:material_regions) {
                if (cancelled && cancelled()) return std::nullopt;
                region.shape=place_native_shape(region.shape,site_placement);
                if (!region.shape.IsNull()) mesh_shape(region.shape);
            }
            mesh_shape(shape);
            solids.emplace(id, PreparedNativeSolid{std::move(content), std::move(shape),
                            presentation_color, material_color, join_presentation_ids.contains(id),
                            std::move(material_regions), std::move(appearance_content),site_placement});
            if (progress) progress(solids.size());
        } catch (const std::exception& error) {
            append_unique(errors, entity.type + " '" + id + "': " + error.what());

        } catch (...) {
            append_unique(errors, entity.type + " '" + id + "': unknown OCCT failure");

        }
    }

    // Both independent and catalog-owned roots publish the same resolved
    // profile geometry, provenance and appearance dependencies.
    const auto publish_assembly = [&](const std::string& id, const std::string& catalog_id,
                                      const AssemblyExpansion& expansion,
                                      const std::optional<std::string>& document_entity_id) {
        if (inactive_owner_ids.contains(id) ||
            (expansion.source_instance.placement &&
             inactive_owner_ids.contains(expansion.source_instance.placement->host_entity_id))) return true;
        const auto source=entities.find(id);
        const auto placement = source==entities.end()
            ? SitePresentationPlacement{} : site_placements.at(id);
        const auto geometry = make_assembly_geometry(expansion);
        if (geometry.shape.IsNull()) throw std::invalid_argument("assembly has no native profiles");
        std::vector<PreparedNativeMaterialRegion> regions;
        auto appearance = nlohmann::json::array();
        for (const auto& profile : geometry.solids) {
            if (cancelled && cancelled()) return false;
            PreparedNativeMaterialRegion region;
            // Qualified origin/catalog/root fields disambiguate equal displayed
            // IDs and local profile triples. Retain the historical entity_id in
            // provenance, while presentations use the shared structured key.
            auto identity = assembly_profile_presentation_key(catalog_id,
                expansion.source_instance, profile.source, document_entity_id);
            region.presentation_key = identity.dump();
            identity["entity_id"] = id;
            region.source_id = identity.dump();
            region.shape = place_native_shape(profile.shape,placement);
            region.color = Quantity_Color(0.63, 0.48, 0.78, Quantity_TOC_RGB);
            region.catalog_id = catalog_id;
            region.material_id = profile.source.material_id;
            region.gross_volume = profile.volume_m3;
            region.net_volume = profile.volume_m3;
            if (region.material_id) {
                const auto key = std::pair{*region.catalog_id, *region.material_id};
                if (!material_bindings.contains(key))
                    throw std::invalid_argument("assembly profile references a missing catalog material");
                const auto color = material_colors.find(key);
                if (color != material_colors.end()) {
                    region.material_color = color->second;
                    const QColor resolved(QString::fromStdString(color->second));
                    if (!resolved.isValid()) throw std::invalid_argument("assembly material color is invalid");
                    region.color = Quantity_Color(resolved.redF(), resolved.greenF(),
                                                  resolved.blueF(), Quantity_TOC_sRGB);
                }
            }
            appearance.push_back({{"source",region.source_id}, {"catalog",*region.catalog_id},
                {"material",region.material_id.value_or("")},
                {"color",region.material_color.value_or("default")}});
            mesh_shape(region.shape);
            regions.push_back(std::move(region));
        }
        if (cancelled && cancelled()) return false;
        auto content=assembly_geometry_content(id,expansion);
        if (source!=entities.end()) append_site_placement_content(content,snapshot,placement);
        auto shape=place_native_shape(geometry.shape,placement);
        mesh_shape(shape);
        solids.emplace(id, PreparedNativeSolid{std::move(content),
            std::move(shape), Quantity_Color(0.63,0.48,0.78,Quantity_TOC_RGB), std::nullopt,
            !visible_ids || visible_ids->contains(id), std::move(regions), appearance.dump(),
            source==entities.end() ? std::nullopt : std::optional<SitePresentationPlacement>{placement}});
        if (progress) progress(solids.size());
        return true;
    };

    // The adapter charges both catalog-owned and independent instances to
    // one budget. Embedded materialization below is permitted only after this
    // complete preflight; it must not publish a partial over-budget document.
    bool assembly_document_valid = false;
    EmbeddedAssemblyPresentationIds embedded_presentation_ids;
    try {
        AssemblyExpansionBudget budget;
        const auto expansions = expand_document_assembly_instances(entities, budget);
        embedded_presentation_ids = embedded_assembly_presentation_ids(entities);
        assembly_document_valid = true;
        if (cancelled && cancelled()) return std::nullopt;
        for (const auto& [id, expansion] : expansions) {
            if (cancelled && cancelled()) return std::nullopt;
            try {
                const auto binding = decode_document_assembly_instance(entities.at(id));
                if (!publish_assembly(id, binding.assembly_catalog_id, expansion, id)) return std::nullopt;
            } catch (const std::exception& error) {
                append_unique(errors, "assembly instance '" + id + "': " + error.what());
            } catch (...) {
                append_unique(errors, "assembly instance '" + id + "': unknown OCCT failure");
            }
        }
    } catch (const std::exception& error) {
        append_unique(errors, "assembly document expansion: " + std::string(error.what()));
    } catch (...) {
        append_unique(errors, "assembly document expansion: unknown failure");
    }

    std::size_t opening_native_work{};
    std::map<std::string,TopoDS_Shape,std::less<>> opening_host_shapes;
    const auto make_assembly_host_shape = [&](const std::string& host_id) -> TopoDS_Shape {
        const auto host = entities.find(host_id);
        if (host == entities.end()) {
            throw std::invalid_argument("assembly host is missing");
        }
        const auto& source = host->second;
        const auto geometry_entity = effective_geometry_entity(snapshot, source);
        if (geometry_entity.type == "opening") {
            const auto found=opening_host_shapes.find(host_id);
            if (found!=opening_host_shapes.end()) return found->second;
            auto shape=make_document_opening_host_shape(snapshot,host_id,&opening_native_work);
            opening_host_shapes.emplace(host_id,shape);
            return shape;
        }
        if (can_recognize_building_entity_type(geometry_entity.type)) {
            return make_building_shape(decode_building_entity(geometry_entity), entities);
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
        return sketch::transform_assembly_shape(source,
            {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
                placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
    };

    AssemblyExpansionBudget embedded_materialization_budget;
    for (const auto& [catalog_id, catalog_entity] : entities) {
        if (cancelled && cancelled()) return std::nullopt;
        if (!assembly_document_valid) continue;
        if (catalog_entity.type != "assembly_model" ||
            !catalog_entity.properties.contains("model")) continue;
        try {
            const auto catalog = AssemblyModel::from_json(catalog_entity.properties.at("model"));
            for (const auto& instance : catalog.instances()) {
                    if (cancelled && cancelled()) return std::nullopt;
                const auto child_id = embedded_presentation_ids.at({catalog_id, instance.id});
                try {
                const auto expansion = catalog.expand(instance, embedded_materialization_budget);
                if (!expansion.profiles.empty()) {
                    if (!publish_assembly(child_id, catalog_id, expansion, std::nullopt)) return std::nullopt;
                    continue;
                }
                // Genuine V1-V3 declarations retain their host-copy behavior.
                if (!instance.placement) continue;
                if (inactive_owner_ids.contains(instance.placement->host_entity_id)) continue;
                const auto host = entities.find(instance.placement->host_entity_id);
                if (host == entities.end()) {
                    append_unique(errors, "assembly instance '" + child_id +
                                         "' references missing host '" +
                                         instance.placement->host_entity_id + "'");
                    continue;
                }
                const auto geometry_entity = effective_geometry_entity(snapshot, host->second);
                std::string content;
                content.reserve(catalog_entity.properties.dump().size() +
                                geometry_entity.properties.dump().size() + child_id.size() + 32);
                content.append(child_id);
                content.push_back('\0');
                const auto& placement = *instance.placement;
                auto placement_content = nlohmann::json{
                    {"host", placement.host_entity_id},
                    {"translation", {placement.translation_m.x, placement.translation_m.y}},
                    {"rotation", placement.rotation_radians}, {"scale", placement.scale}};
                if (placement.translation_z_m != 0)
                    placement_content.at("translation").push_back(placement.translation_z_m);
                if (placement.mirrored_y) placement_content["mirrored_y"] = true;
                if (placement.vertical_scale != 1) placement_content["vertical_scale"] = placement.vertical_scale;
                content.append(placement_content.dump());
                content.push_back('\0');
                append_entity_content(content, geometry_entity);
                append_building_dependencies(content, snapshot, geometry_entity);
                if (geometry_entity.type=="opening") {
                    const auto wall_id=geometry_entity.properties.at("wall_id").get<std::string>();
                    const auto resolved_wall=effective_geometry_entity(snapshot,entities.at(wall_id));
                    content.append(entity_content(resolved_wall,openings_by_wall[wall_id]));
                }
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
                } catch (const std::exception& error) {
                    append_unique(errors, "assembly instance '" + child_id + "': " + error.what());
                } catch (...) {
                    append_unique(errors, "assembly instance '" + child_id + "': unknown OCCT failure");
                }
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
