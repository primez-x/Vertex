#include "sketch/phase_wall_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/assembly_phase_membership_transport.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/opening_host_geometry.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/wall_layer_stack_edit.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_replacements = 4096;
constexpr std::size_t maximum_component_dimensions = 1000;
constexpr std::size_t maximum_json_nodes = 4 * 1024 * 1024;
constexpr std::size_t maximum_json_string_bytes = 64 * 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase wall replacement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
void presentation_source_identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isspace(c);
    })) reject("presentation source identity must be nonblank and at most 128 bytes");
}
// Read-only conservative identity inspection, never a string-rewrite codec.
// All retained strings reserve fresh IDs, including unknown future children.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > maximum_json_nodes) reject("source JSON nesting/node budget exceeded");
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            if (text.size() > maximum_json_string_bytes - bytes) reject("source string budget exceeded");
            bytes += text.size(); values.insert(text);
        } else if (value.is_array()) {
            for (const auto& child : value) read(child, depth + 1);
        } else if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                if (key.size() > maximum_json_string_bytes - bytes) reject("source key budget exceeded");
                bytes += key.size(); values.insert(key); read(child, depth + 1);
            }
        }
    }
};
bool touches(const Json& value, const Ids& ids) {
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
void diagnostic(PhaseWallReplacementPlan& plan, const std::string& id, const std::string& reason) {
    const PhaseWallReplacementDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
bool hosted_root_active(const PhaseWallReplacementEntities& source, const ConstraintPhaseScope& scope,
    const PhaseWallReplacementHostedInstanceKey& root, const std::string& host) {
    return !scope.assembly_memberships.owners.contains(root) ||
        (!scope.assembly_memberships.inactive_roots.contains(root) &&
            !assembly_host_inactive_in_phase(source, scope.inactive_owner_ids, host));
}
// Native admission follows the actual profile/legacy host-copy split. Cache
// each real wall/ opening body and charge layered-cut work before fabrication.
struct HostedGeometryAdmission {
    AssemblyExpansionBudget expansion;
    std::size_t native_work{};
    std::map<std::string, TopoDS_Shape, std::less<>> hosts;
    TopoDS_Shape wall_shape(const PhaseWallReplacementEntities& source, const ConstraintPhaseScope& scope,
        const std::string& id) {
        if (const auto found = hosts.find(id); found != hosts.end()) return found->second;
        std::vector<const Entity*> cuts;
        for (const auto& [cut_id, entity] : source)
            if (entity.type == "opening" && !scope.inactive_owner_ids.contains(cut_id) &&
                entity.properties.value("wall_id", std::string{}) == id) {
                if (cuts.size() >= 128) reject("hosted wall cut budget exceeded");
                cuts.push_back(&entity);
            }
        Wall wall; std::string error;
        if (!read_document_wall(resolve_vertical_placement(source, source.at(id)), cuts, wall, error)) reject(error);
        validate_wall_semantics(wall);
        const auto layers = std::max<std::size_t>(1, wall.layers.size());
        if (layers > 32) reject("hosted wall layer budget exceeded");
        const auto work = layers * (wall.openings.size() + 1);
        if (work > 262144 - native_work) reject("hosted native geometry budget exceeded");
        native_work += work;
        const auto shape = make_wall(wall);
        hosts.emplace(id, shape); return shape;
    }
    void admit(const PhaseWallReplacementEntities& source, const ConstraintPhaseScope& scope,
        const AssemblyModel& model, const AssemblyInstance& instance) try {
        if (!instance.placement) reject("hosted component requires an actual placement");
        const auto& placement = *instance.placement;
        const auto host = source.find(placement.host_entity_id);
        if (host == source.end() || (host->second.type != "wall" && host->second.type != "opening") ||
            assembly_host_inactive_in_phase(source, scope.inactive_owner_ids, host->first))
            reject("hosted component requires its actual active wall/opening host");
        // Context is owned by the actual catalog and host, never a synthetic row.
        (void)resolve_site_presentation(source, host->first);
        const auto expanded = model.expand(instance, expansion);
        if (!expanded.profiles.empty()) {
            if (host->second.type == "wall") (void)wall_shape(source, scope, host->first);
            else {
                std::string wall_id, error;
                if (!read_document_wall_id(host->second, wall_id, error) || !source.contains(wall_id) ||
                    source.at(wall_id).type != "wall") reject("hosted opening has no actual wall");
                (void)wall_shape(source, scope, wall_id);
            }
            if (make_assembly_geometry(expanded).shape.IsNull()) reject("hosted profiles have no native body");
            return;
        }
        auto found = hosts.find(host->first);
        if (found == hosts.end()) {
            const auto shape = host->second.type == "wall" ? wall_shape(source, scope, host->first)
                : make_document_opening_host_shape(source, host->first, &native_work);
            found = hosts.emplace(host->first, shape).first;
        }
        (void)transform_assembly_shape(found->second,
            {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
                placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
    } catch (const Standard_Failure& error) {
        reject(std::string("hosted native admission failed: ") + error.what());
    }
};
// Local definition/material namespaces remain exact. Only admitted qualified
// row declarations and host bindings are released from the opaque scan.
Entity catalog_remainder(Entity scratch, const Ids& released_rows) {
    auto& model = scratch.properties.at("model");
    for (auto& material : model.at("materials")) material.erase("id");
    const auto clear_overrides = [](Json& row) { row.erase("material_overrides"); };
    for (auto& type : model.at("types")) {
        type.erase("id"); type.erase("materials");
        if (type.contains("profiles")) for (auto& profile : type.at("profiles")) {
            profile.erase("id"); profile.erase("material_slot");
        }
        if (type.contains("parts")) for (auto& part : type.at("parts")) {
            part.erase("id"); part.erase("type_id"); clear_overrides(part);
        }
    }
    for (auto& row : model.at("instances")) {
        if (released_rows.contains(row.at("id").get<std::string>())) {
            row.erase("id");
            if (row.contains("placement")) row.at("placement").erase("host_entity_id");
        }
        row.erase("type_id"); clear_overrides(row);
        if (row.contains("nested_overrides")) for (auto& change : row.at("nested_overrides")) {
            change.erase("part_path"); clear_overrides(change);
        }
    }
    return scratch;
}
Ids discover_hosted_instances(PhaseWallReplacementPlan& plan, const PhaseWallReplacementEntities& source,
    const Ids& owners, const ConstraintPhaseScope& scope, const ProjectOrganization& organization,
    const std::map<std::string, std::string, std::less<>>& memberships, const Ids& baseline) {
    Ids catalogs;
    HostedGeometryAdmission geometry;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto raw_model = field(entity.properties, "model");
        const auto rows = raw_model ? field(*raw_model, "instances") : nullptr;
        if (!rows || !rows->is_array()) continue;
        bool affected = false;
        for (const auto& row : *rows) {
            const auto placement = field(row, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            const auto local = field(row, "id");
            if (host && host->is_string() && owners.contains(host->get_ref<const std::string&>()) &&
                (!local || !local->is_string() || hosted_root_active(source, scope,
                    {id, local->get<std::string>()}, host->get_ref<const std::string&>()))) affected = true;
        }
        if (!affected) continue;
        try {
            const auto member = memberships.find(id);
            if (member != memberships.end() && (member->second != plan.registry_id || !baseline.contains(id)))
                reject("hosted catalog support is outside the actual target baseline registry");
            for (const auto& [key, type] : std::vector<std::pair<const char*, const char*>>{
                {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}) {
                if (!entity.properties.contains(key)) continue;
                const auto target = entity.properties.at(key).get<std::string>();
                if (!source.contains(target) || source.at(target).type != type) reject("hosted catalog context is unresolved");
            }
            const auto node = organization.nodes.find(id);
            if (node != organization.nodes.end() && !node->second.issues.empty()) reject("hosted catalog drawing context is contradictory");
            (void)organization.drawing_context(id);
            (void)resolve_vertical_placement(source, entity);
            (void)resolve_site_presentation(source, id);
            const auto model = AssemblyModel::from_json(*raw_model);
            Ids released, selected;
            for (const auto& instance : model.instances()) {
                if (!instance.placement || !owners.contains(instance.placement->host_entity_id)) continue;
                const PhaseWallReplacementHostedInstanceKey key{id, instance.id};
                if (!hosted_root_active(source, scope, key, instance.placement->host_entity_id)) {
                    released.insert(instance.id); continue;
                }
                const auto row_owner = scope.assembly_memberships.owners.find(key);
                if (row_owner != scope.assembly_memberships.owners.end() && row_owner->second.registry_id != plan.registry_id)
                    reject("hosted row and replacement host belong to different actual registries");
                if (row_owner == scope.assembly_memberships.owners.end() && scope.inactive_owner_ids.contains(id))
                    reject("unlisted hosted row retains inactive legacy catalog support");
                geometry.admit(source, scope, model, instance);
                plan.required_hosted_instance_ids.push_back(key);
                selected.insert(instance.id); released.insert(instance.id);
            }
            auto remainder = catalog_remainder(entity, released);
            Ids opaque_ids = owners; opaque_ids.insert(id); opaque_ids.insert(selected.begin(), selected.end());
            if (touches(remainder.properties, opaque_ids) || touches(remainder.extensions, opaque_ids))
                reject("hosted catalog contains an affected opaque reference without a qualified copy codec");
            catalogs.insert(id);
            if (owners.size() + catalogs.size() + plan.required_hosted_instance_ids.size() > maximum_replacements)
                reject("hosted replacement identity budget exceeded");
        } catch (const std::exception& error) {
            diagnostic(plan, id, "affected hosted catalog requires supported typed source admission: " + std::string(error.what()));
        }
    }
    std::sort(plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
    return catalogs;
}
using ComponentDimensionRoots = std::map<std::string, PhaseWallReplacementHostedInstanceKey, std::less<>>;
ComponentDimensionRoots discover_component_dimensions(PhaseWallReplacementPlan& plan,
    const PhaseWallReplacementEntities& source, std::size_t existing_identity_count) {
    const std::set<PhaseWallReplacementHostedInstanceKey> roots(
        plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
    ComponentDimensionRoots dimensions;
    std::vector<AssemblyDimensionTarget> targets;
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto target = field(entity.properties, "target");
        const auto assembly = target ? field(*target, "assembly") : nullptr;
        const auto catalog = assembly ? field(*assembly, "catalog_id") : nullptr;
        const auto instance = assembly ? field(*assembly, "instance_id") : nullptr;
        if (!catalog || !catalog->is_string() || !instance || !instance->is_string()) continue;
        const PhaseWallReplacementHostedInstanceKey key{catalog->get<std::string>(), instance->get<std::string>()};
        if (!roots.contains(key)) continue;
        // Independent document owners can share the tuple. Only an explicit
        // null admits an embedded target, never its entity_id carrier alone.
        const auto document = field(*assembly, "document_entity_id");
        if (document && !document->is_null()) continue;
        try {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!document || !decoded.supported() || decoded.version != 6 ||
                decoded.dimension->kind != BoundaryDimensionKind::assembly_extent || !decoded.dimension->assembly_target ||
                decoded.dimension->assembly_target->component.document_entity_id)
                reject("affected embedded component dimension requires a supported v6 extent");
            if (dimensions.size() >= maximum_component_dimensions) reject("associated embedded dimension budget exceeded");
            dimensions.emplace(id, key);
            targets.push_back(*decoded.dimension->assembly_target);
        } catch (const std::exception& error) {
            diagnostic(plan, id, "affected embedded component dimension is unsupported: " + std::string(error.what()));
        }
    }
    if (existing_identity_count > maximum_replacements || dimensions.size() > maximum_replacements - existing_identity_count)
        reject("combined replacement/dimension identity budget exceeded");
    // One complete original-source batch admits every retained nested path.
    if (!targets.empty()) {
        try { (void)resolve_assembly_dimensions(source, targets); }
        catch (const std::exception& error) {
            for (const auto& [id, key] : dimensions) {
                (void)key; diagnostic(plan, id, "affected embedded component dimension cannot resolve: " + std::string(error.what()));
            }
        }
    }
    plan.required_dimension_ids.clear();
    for (const auto& [id, key] : dimensions) { (void)key; plan.required_dimension_ids.push_back(id); }
    return dimensions;
}
Entity component_dimension_remainder(Entity scratch) {
    // Called only for the exact supported roster admitted above. Keep opaque
    // property and extension siblings in the dependency scan.
    auto& target = scratch.properties.at("target");
    target.erase("entity_id");
    auto& assembly = target.at("assembly");
    for (const auto* key : {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path", "axis"}) assembly.erase(key);
    return scratch;
}
Wall decoded_wall(const PhaseWallReplacementEntities& source, const std::string& id) {
    Wall wall; std::string error;
    const auto placed = resolve_vertical_placement(source, source.at(id));
    if (!read_document_wall(placed, {}, wall, error)) reject(id + ": " + error);
    validate_wall_semantics(wall); return wall;
}
std::string remap(const std::string& id, const PhaseWallReplacementIdentityMap& identities) {
    const auto found = identities.find(id);
    return found == identities.end() ? id : found->second;
}
void remap_field(Json& object, const char* key, const PhaseWallReplacementIdentityMap& identities) {
    if (object.contains(key)) object.at(key) = remap(object.at(key).get<std::string>(), identities);
}
bool measured_source(const Entity& entity) {
    return entity.extensions.contains("measurement_linework_sources") ||
        (entity.type == "measurement_boundary" && entity.extensions.contains("measurement_linework_group"));
}
// This vocabulary is deliberately limited to qualified source lineage. The
// shared source checker validates intervals, group members, holes and faces.
template<class Reference> void visit_lineage(Json& lineage, const Reference& reference) {
    if (!lineage.is_array()) reject("measured source lineage must be an array");
    for (auto& edge : lineage) {
        if (!edge.is_array()) reject("measured source edge must be an array");
        for (auto& use : edge) {
            use.at("owner_id") = reference(use.at("owner_id").template get<std::string>(), false);
            use.at("segment_id") = reference(use.at("segment_id").template get<std::string>(), false);
        }
    }
}
template<class Reference> void visit_measured_sources(Entity& entity, const Reference& reference) {
    if (entity.extensions.contains("measurement_linework_sources"))
        visit_lineage(entity.extensions.at("measurement_linework_sources"), reference);
    if (entity.extensions.contains("measurement_linework_group")) {
        const auto& group = entity.extensions.at("measurement_linework_group");
        if (entity.type != "measurement_boundary" || !group.is_object() || group.size() != 2 ||
            !group.contains("version") || !group.at("version").is_number_integer() || group.at("version") != 1 ||
            !group.contains("members") || !group.at("members").is_array() ||
            group.at("members").size() < 2 || group.at("members").size() > 2048)
            reject("unsupported measured area group metadata");
        for (auto& member : entity.extensions.at("measurement_linework_group").at("members")) visit_lineage(member, reference);
    }
}
Ids measured_source_ids(const Entity& entity) {
    Ids ids;
    auto copy = entity;
    visit_measured_sources(copy, [&](const std::string& id, bool) { ids.insert(id); return id; });
    return ids;
}
// Work on the retained wire record after typed decoding, changing only genuine
// identity fields. In particular, never round-trip opaque receipt extensions.
template<class Reference> void visit_stroke_copy(Entity& entity, const Reference& reference) {
    auto& model = entity.properties.at("model");
    const auto decoded = decode_measurement_linework_model(model);
    if (!decoded.supported()) reject(decoded.diagnostic);
    if (decoded.model->stroke_id != entity.id) reject("measured stroke identity does not match its actual owner");
    (void)measurement_linework_copy_isolated(entity);
    model.at("stroke_id") = reference(model.at("stroke_id").template get<std::string>(), false);
    for (auto& edge : model.at("segments")) {
        for (const auto* key : {"segment_id", "start_vertex_id", "end_vertex_id"})
            edge.at(key) = reference(edge.at(key).template get<std::string>(), true);
        auto& receipt = edge.at("receipt");
        receipt.at("segment_id") = reference(receipt.at("segment_id").template get<std::string>(), true);
    }
    const auto edit = [&](Json& value) {
        value.at("boundary_id") = reference(value.at("boundary_id").get<std::string>(), false);
        const auto* key = value.at("kind") == "move_vertex" ? "vertex_id" : "segment_id";
        value.at(key) = reference(value.at(key).get<std::string>(), true);
    };
    if (model.contains("operations")) for (auto& operation : model.at("operations")) {
        if (operation.at("type") == "edit") edit(operation.at("edit"));
        else if (operation.at("type") == "vertex_batch") for (auto& value : operation.at("edits")) edit(value);
    }
}
template<class Reference> void visit_boundary_copy_evidence(Entity& entity, const Reference& reference) {
    const auto topology = [&](Json& segments, bool receipts) {
        for (auto& edge : segments) {
            for (const auto* key : {"segment_id", "start_vertex_id", "end_vertex_id"})
                edge.at(key) = reference(edge.at(key).template get<std::string>(), true);
            if (receipts) {
                auto& receipt = edge.at("receipt");
                receipt.at("segment_id") = reference(receipt.at("segment_id").template get<std::string>(), true);
            }
        }
    };
    const auto authoring = [&](Json& value) {
        const auto decoded = decode_boundary_receipt_envelope(value);
        if (!decoded.supported()) reject(decoded.diagnostic);
        if (decoded.record->boundary_id != entity.id) reject("boundary construction owner does not match");
        value.at("boundary_id") = reference(value.at("boundary_id").template get<std::string>(), false);
        topology(value.at("segments"), true);
    };
    if (entity.properties.contains("boundary_authoring")) authoring(entity.properties.at("boundary_authoring"));
    if (!entity.extensions.contains("boundary_geometry_derivation")) return;
    if (entity.properties.contains("boundary_authoring")) reject("conflicting boundary geometry provenance");
    auto& derivation = entity.extensions.at("boundary_geometry_derivation");
    if (!derivation.is_object() || derivation.size() != 3 || !derivation.contains("version") ||
        !derivation.at("version").is_number_integer() || (derivation.at("version") != 1 && derivation.at("version") != 2) ||
        !derivation.contains("operations") || !derivation.at("operations").is_array() || derivation.at("operations").empty())
        reject("unsupported boundary geometry derivation");
    if (derivation.at("version") == 1) authoring(derivation.at("source_boundary_authoring"));
    else {
        auto& origin = derivation.at("source_boundary");
        if (!origin.is_object() || origin.size() != 2 || !origin.contains("boundary_model_version") || !origin.contains("segments"))
            reject("unsupported archived identified boundary origin");
        (void)decode_identified_boundary_entity(Entity{entity.id, entity.type, origin, false, Json::object()});
        topology(origin.at("segments"), false);
    }
    const auto edit = [&](Json& value) {
        const auto decoded = decode_boundary_geometry_edit(value);
        if (decoded.boundary_id != entity.id) reject("boundary geometry derivation owner does not match");
        if (decoded.physical_wall_room_repair)
            reject("boundary derivation requires reviewed physical source completion");
        for (const auto* key : {"boundary_id", "vertex_id", "segment_id", "new_vertex_id", "new_segment_id", "new_dimension_id"})
            if (value.contains(key) && !value.at(key).template get<std::string>().empty())
                value.at(key) = reference(value.at(key).template get<std::string>(), std::string_view(key) != "boundary_id");
        if (value.contains("construction")) {
            auto& receipt = value.at("construction");
            receipt.at("segment_id") = reference(receipt.at("segment_id").template get<std::string>(), true);
        }
        if (decoded.kind == BoundaryGeometryEditKind::redefine_boundary) {
            topology(value.at("replacement_segments"), false);
            if (value.contains("replacement_authoring") && !value.at("replacement_authoring").is_null()) authoring(value.at("replacement_authoring"));
            for (const auto* key : {"replacement_dimension_ids", "replacement_removed_reference_ids", "replacement_wall_source_ids"})
                if (value.contains(key)) for (auto& id : value.at(key))
                    id = reference(id.template get<std::string>(), std::string_view(key) != "replacement_wall_source_ids");
            if (value.contains("replacement_child_mapping")) {
                auto mappings = Json::object();
                for (const auto& [group, mapping] : value.at("replacement_child_mapping").items()) {
                    mappings[group] = Json::object();
                    for (const auto& [old_id, new_id] : mapping.items())
                        mappings[group][reference(old_id, true)] = reference(new_id.template get<std::string>(), true);
                }
                value.at("replacement_child_mapping") = std::move(mappings);
            }
            if (value.contains("replacement_linework_sources")) visit_lineage(value.at("replacement_linework_sources"), reference);
            if (decoded.wall_source_translation && value.at("wall_source_translation").contains("genesis")) {
                auto& genesis = value.at("wall_source_translation").at("genesis");
                // Materialization/integrity admission below qualifies the
                // captured physical origin. Geometry and context stay exact.
                if (!genesis.is_object() || genesis.size() != 3 || !genesis.contains("kernel") ||
                    !genesis.contains("walls") || !genesis.at("walls").is_array() || !genesis.contains("origin_outline"))
                    reject("unsupported physical translation genesis");
                auto captured = entity;
                auto captured_source = genesis;
                captured_source["version"] = 2; captured_source["basis"] = "exterior";
                captured_source["translations"] = Json::array({value.at("wall_source_translation").at("offset")});
                captured.properties["wall_measurement_source"] = std::move(captured_source);
                (void)materialize_exterior_wall_measurement(captured);
                auto captured_ids = exterior_wall_measurement_source_ids(captured);
                auto replacement_ids = decoded.replacement_wall_source_ids;
                std::sort(captured_ids.begin(), captured_ids.end()); std::sort(replacement_ids.begin(), replacement_ids.end());
                if (captured_ids != replacement_ids) reject("physical translation genesis source identities differ from its typed replacement");
                for (auto& wall : genesis.at("walls")) wall.at("id") = reference(wall.at("id").template get<std::string>(), false);
                std::sort(genesis.at("walls").begin(), genesis.at("walls").end(), [](const Json& a, const Json& b) {
                    return a.at("id").get<std::string>() < b.at("id").get<std::string>();
                });
            }
        }
    };
    for (auto& operation : derivation.at("operations")) {
        if (!operation.is_object() || operation.size() != 2 || !operation.contains("kind") || !operation.contains("value"))
            reject("unsupported boundary derivation operation envelope");
        if (operation.at("kind") == "geometry_edit") edit(operation.at("value"));
        else if (operation.at("kind") == "vertex_batch") {
            if (!operation.at("value").is_array() || operation.at("value").empty()) reject("boundary vertex batch must be nonempty");
            for (auto& value : operation.at("value")) {
                if (decode_boundary_geometry_edit(value).kind != BoundaryGeometryEditKind::move_vertex) reject("unsupported boundary vertex batch edit");
                edit(value);
            }
        } else if (operation.at("kind") == "transform") {
            auto& value = operation.at("value");
            if (decode_boundary_transform(value).boundary_id != entity.id) reject("boundary transform owner does not match");
            value.at("boundary_id") = reference(value.at("boundary_id").template get<std::string>(), false);
        } else if (operation.at("kind") == "wall_merge") {
            // The shared integrity replay proves the seam and complete span.
            // This strict historical operation owns its retired wall identity;
            // current source walls remain external typed references.
            auto& value = operation.at("value");
            if (!value.is_object() || value.size() != 5 || !value.contains("version") ||
                !value.at("version").is_number_integer() || value.at("version") != 1 ||
                !value.contains("vertex_id") || !value.contains("segments") ||
                !value.at("segments").is_array() || !value.contains("wall_source_ids") ||
                !value.at("wall_source_ids").is_array() || !value.contains("removed_wall_id"))
                reject("unsupported historical wall merge proof");
            value.at("vertex_id") = reference(value.at("vertex_id").template get<std::string>(), true);
            topology(value.at("segments"), false);
            for (auto& wall : value.at("wall_source_ids")) wall = reference(wall.template get<std::string>(), false);
            value.at("removed_wall_id") = reference(value.at("removed_wall_id").template get<std::string>(), true);
        } else reject("boundary derivation copy operation requires a dedicated codec: " + operation.at("kind").template get<std::string>());
    }
}
// Remove only precisely understood identity fields from a temporary inspection
// copy. A reference elsewhere in a copied payload needs its own typed codec.
Entity opaque_remainder(const Entity& entity, bool complete_presentations = false,
    bool complete_corner_windows = false) {
    auto remainder = entity;
    wall_scale_quantity_reference_remainder(remainder);
    if (entity.type == "wall" && remainder.extensions.contains("wall_layer_stack_retirement")) {
        auto& archive = remainder.extensions.at("wall_layer_stack_retirement");
        validate_wall_layer_stack_retirement(archive);
        // Retired layer IDs are historical provenance. Only this qualified
        // slot is exempted; opaque receipt siblings still require a codec.
        for (auto& row : archive.at("receipts")) row.erase("layer_id");
    }
    auto& p = remainder.properties;
    if (entity.type == assembly_phase_membership_entity_type) {
        // Only the admitted closed qualified binding model is consumed.
        // Opaque properties and extension siblings remain source-exact.
        (void)AssemblyPhaseMembership::from_json(p.at("model"));
        p.erase("model");
    }
    const auto consumed = [](const std::string&, bool) { return std::string{}; };
    if (entity.type == "measurement_linework") visit_stroke_copy(remainder, consumed);
    if (entity.type == "wall" && p.contains("layers"))
        for (auto& layer : p.at("layers")) layer.erase("id");
    if (entity.type == "opening") p.erase("wall_id");
    if (complete_corner_windows) {
        if (entity.type == "corner_window") {
            (void)parse_corner_window(entity);
            p.erase("wall_ids"); p.erase("opening_ids");
        } else if (entity.type == "opening" && p.contains("corner_window_id")) {
            // Complete cohort admission qualifies this backlink and leg. No
            // generic reference keys or nested metadata are consumed here.
            p.erase("corner_window_id");
        }
    }
    if (entity.type == "wall_join") p.erase("wall_ids");
    if (can_recognize_boundary_entity_type(entity.type)) {
        visit_measured_sources(remainder, consumed);
        visit_boundary_copy_evidence(remainder, consumed);
        if (p.contains("segments")) for (auto& edge : p.at("segments")) {
            edge.erase("segment_id"); edge.erase("start_vertex_id"); edge.erase("end_vertex_id");
        }
        if (p.contains("wall_measurement_source"))
            for (auto& wall : p.at("wall_measurement_source").at("walls")) wall.erase("id");
    }
    if (entity.type == "constraint") {
        p.erase("wall_ids"); p.erase("entity_ids");
        if (p.contains("bindings")) for (auto& binding : p.at("bindings")) {
            binding.erase("owner_id"); binding.erase("segment_id"); binding.erase("vertex_id");
        }
    }
    if (can_recognize_boundary_dimension_entity_type(entity.type) && p.contains("target")) {
        auto& target = p.at("target");
        for (const auto* key : {"entity_id", "segment_id", "segment_ids", "second_segment_id", "vertex_id"}) target.erase(key);
    }
    if (complete_presentations && entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& overlay : view.at("overlays")) {
                overlay.erase("id"); overlay.erase("object_id");
                if (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null())
                    overlay.at("dimension_binding").erase("object_id");
            }
        }
    } else if (complete_presentations && entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) row.erase("target_id");
    }
    return remainder;
}
bool affected_overlay(const Json& overlay, const Ids& owners) {
    return (overlay.contains("object_id") && owners.contains(overlay.at("object_id").get<std::string>())) ||
        (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null() &&
         owners.contains(overlay.at("dimension_binding").at("object_id").get<std::string>()));
}
// Append source-derived typed rows on the retained wire. Every original row,
// scope, role and drawing context remains exact; owners are never copied.
void complete_presentation(PhaseWallReplacementEntities& candidate,
    const PhaseWallReplacementEntities& source, const Ids& owners,
    const PhaseWallReplacementIdentityMap& identities) {
    for (const auto& [id, original] : source) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(original);
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids"); const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.get<std::string>()))
                        rows.push_back(identities.at(row.get<std::string>()));
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects"); const auto retained = rows;
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) {
                        remap_field(row, "object_id", identities); rows.push_back(std::move(row));
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays"); const auto retained = rows;
                    for (auto row : retained) if (affected_overlay(row, owners)) {
                        row.at("id") = identities.at(row.at("id").get<std::string>());
                        remap_field(row, "object_id", identities);
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                            remap_field(row.at("dimension_binding"), "object_id", identities);
                        rows.push_back(std::move(row));
                    }
                }
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            validate_annotation_entity(original);
            auto& rows = candidate.at(id).properties.at("state").at("overrides");
            const auto retained = rows;
            for (auto row : retained) if (row.at("target_kind") != "output_view" &&
                    owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
bool room_touches(const Entity& room, const Ids& walls) {
    // The room descriptor is retained evidence. Unknown descriptors that mention
    // affected identities are diagnosed by the caller, never interpreted here.
    const auto descriptor = decode_physical_wall_room_descriptor(room);
    return walls.contains(descriptor.selected_wall_id) || touches(descriptor.source_lineage, walls);
}
} // namespace

bool PhaseWallReplacementPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) { return d.blocking; });
}

PhaseWallReplacementPlan inspect_phase_wall_replacement_plan(
    const PhaseWallReplacementEntities& source, const std::vector<std::string>& seed_wall_ids,
    const std::string& registry_id, const std::string& alternative_id, bool complete_presentations,
    bool complete_corner_windows, bool complete_hosted_instances, bool complete_component_dimensions) {
    try {
        if (complete_component_dimensions && !complete_hosted_instances)
            reject("component dimension completion requires hosted instance completion");
        identity(registry_id); identity(alternative_id);
        if (source.size() > maximum_entities) reject("source entity budget exceeded");
        Strings occupied;
        for (const auto& [id, entity] : source) {
            if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
                reject("source must contain actual identified entity envelopes");
            occupied.values.insert(id); occupied.read(entity.properties); occupied.read(entity.extensions);
        }
        AssemblyCatalogTransferBudget catalog_work;
        if (complete_hosted_instances) {
            // Complete-map phase and alias resolvers decode catalog rows too.
            // Admit their raw graph work before the first semantic decoder.
            for (const auto& [id, entity] : source) {
                (void)id;
                if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_work);
            }
        }
        if (!source.contains(registry_id) || source.at(registry_id).type != "model_phases") reject("registry is not an actual model_phases entity");
        const auto model = ModelPhases::from_json(source.at(registry_id).properties.at("model"));
        if (model.active_alternative() != std::optional<std::string>{alternative_id}) reject("alternative must be the actual saved active selection");
        const auto scope = constraint_phase_scope(source);
        std::map<std::string, std::string, std::less<>> memberships;
        Ids foreign_active_baseline;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids) {
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry model membership: " + id);
            const auto state = registry.states.find(id);
            if (registry.registry_id != registry_id && state != registry.states.end() && state->second == ModelPhase::existing)
                foreign_active_baseline.insert(id);
        }
        const Ids baseline(model.baseline_ids().begin(), model.baseline_ids().end());
        PhaseWallReplacementPlan plan;
        plan.registry_id = registry_id; plan.alternative_id = alternative_id; plan.seed_wall_ids = seed_wall_ids;
        plan.complete_presentations = complete_presentations;
        plan.complete_corner_windows = complete_corner_windows;
        plan.complete_hosted_instances = complete_hosted_instances;
        plan.complete_component_dimensions = complete_component_dimensions;
        if (seed_wall_ids.empty() || seed_wall_ids.size() > maximum_replacements) reject("requires a bounded nonempty explicit wall seed");
        std::sort(plan.seed_wall_ids.begin(), plan.seed_wall_ids.end());
        if (std::adjacent_find(plan.seed_wall_ids.begin(), plan.seed_wall_ids.end()) != plan.seed_wall_ids.end()) reject("wall seeds must be unique");
        Ids walls;
        for (const auto& id : plan.seed_wall_ids) {
            identity(id);
            if (!source.contains(id) || source.at(id).type != "wall" || !baseline.contains(id) || scope.inactive_owner_ids.contains(id))
                reject("seed must be an active actual baseline wall in the selected registry: " + id);
            walls.insert(id);
        }
        // Discovery uses the complete map and all actual saved choices. The
        // shared contact codec proves endpoint/T contact and actual hierarchy;
        // replacement additionally requires the same effective elevation plane.
        std::vector<std::string> active_walls;
        for (const auto& [id, entity] : source) if (entity.type == "wall" && !scope.inactive_owner_ids.contains(id)) active_walls.push_back(id);
        if (active_walls.size() > 2048) reject("active wall contact discovery budget exceeded");
        const auto organization = organize_project(source);
        std::map<std::string, Wall, std::less<>> geometry;
        for (const auto& id : active_walls) geometry.emplace(id, decoded_wall(source, id));
        const auto contacts = exterior_corner_physical_contact_graph_active_phase(source);
        struct InteriorContact { std::string first, second; bool indeterminate; };
        std::vector<InteriorContact> interior_contacts;
        std::map<std::string, Bounds2, std::less<>> bounds;
        for (const auto& [id, wall] : geometry) bounds.emplace(id, segment_bounds(wall.baseline));
        for (std::size_t i = 0; i < active_walls.size(); ++i) for (std::size_t j = i + 1; j < active_walls.size(); ++j) {
            const auto& a = active_walls[i]; const auto& b = active_walls[j];
            if (organization.drawing_context(a) != organization.drawing_context(b) ||
                source.at(a).properties.value("phase_id", Json(nullptr)) != source.at(b).properties.value("phase_id", Json(nullptr)) ||
                std::abs(geometry.at(a).elevation - geometry.at(b).elevation) > default_geometry_tolerance_metres) continue;
            const auto& first = bounds.at(a); const auto& second = bounds.at(b);
            const auto t = default_geometry_tolerance_metres;
            if (first.maximum.x + t < second.minimum.x || second.maximum.x + t < first.minimum.x ||
                first.maximum.y + t < second.minimum.y || second.maximum.y + t < first.minimum.y) continue;
            const auto intersection = segment_intersection(geometry.at(a).baseline, geometry.at(b).baseline);
            if (intersection.kind == SegmentIntersectionKind::none) continue;
            if (interior_contacts.size() >= 8192) reject("analytical physical contact budget exceeded");
            interior_contacts.push_back({a, b, intersection.kind == SegmentIntersectionKind::indeterminate});
        }
        std::map<std::string, PersistentConstraint, std::less<>> constraints;
        std::map<std::string, WallJoin, std::less<>> joins;
        for (const auto& [id, entity] : source) {
            if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (decoded.constraint) {
                    for (const auto& binding : decoded.constraint->bindings)
                        if (!source.contains(binding.owner_id)) reject("constraint endpoint does not resolve to an actual source owner: " + binding.owner_id);
                    if (constraint_participates(*decoded.constraint, scope)) constraints.emplace(id, *decoded.constraint);
                }
            } else if (entity.type == "wall_join" && !scope.inactive_owner_ids.contains(id)) {
                const auto join = parse_wall_join(entity.properties, id);
                if (std::none_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& wall) { return scope.inactive_owner_ids.contains(wall); })) joins.emplace(id, join);
            }
        }
        const auto add_baseline = [&](const std::string& id) {
            if (source.contains(id) && source.at(id).type == "wall" && baseline.contains(id) && !scope.inactive_owner_ids.contains(id)) return walls.insert(id).second;
            return false;
        };
        Ids analytical_endpoints, room_transit, semantic_visible, corner_owners;
        for (const auto& [id, entity] : source) if (!scope.inactive_owner_ids.contains(id)) semantic_visible.insert(id);
        const auto measured_checks = measurement_linework_source_checks(source, &semantic_visible);
        const auto add_analytical = [&](const std::string& id) {
            const auto& endpoint = source.at(id);
            if (!baseline.contains(id) || scope.inactive_owner_ids.contains(id)) {
                diagnostic(plan, id, "connected analytical owner requires actual target baseline membership"); return false;
            }
            if (endpoint.type == "measurement_linework") {
                const auto decoded = decode_measurement_linework_model(endpoint.properties.at("model"));
                if (!decoded.supported()) { diagnostic(plan, id, "connected measured stroke: " + decoded.diagnostic); return false; }
                if (decoded.model->stroke_id != id) reject("measured stroke identity differs from its source owner: " + id);
                (void)measurement_linework_copy_isolated(endpoint);
            } else if (!can_recognize_boundary_entity_type(endpoint.type) ||
                inspect_boundary_entity_version(endpoint).format != BoundaryEntityFormat::identified_v1) {
                diagnostic(plan, id, "connected analytical owner needs supported stable topology"); return false;
            }
            return analytical_endpoints.insert(id).second;
        };
        bool changed = true;
        while (changed) {
            changed = false;
            if (complete_corner_windows) for (const auto& [id, entity] : source) {
                if (entity.type != "corner_window" || scope.inactive_owner_ids.contains(id)) continue;
                const auto hosts = entity.properties.find("wall_ids");
                if (hosts == entity.properties.end() || !touches(*hosts, walls)) continue;
                try {
                    const auto window = parse_corner_window(entity);
                    bool eligible = baseline.contains(id);
                    for (const auto& host : window.wall_ids) {
                        if (!source.contains(host) || source.at(host).type != "wall" ||
                            !baseline.contains(host) || scope.inactive_owner_ids.contains(host)) {
                            diagnostic(plan, id, "corner replacement requires both actual active target baseline hosts: " + host);
                            eligible = false;
                        } else changed = add_baseline(host) || changed;
                    }
                    for (const auto& cut : window.opening_ids) {
                        if (!source.contains(cut) || source.at(cut).type != "opening" ||
                            !baseline.contains(cut) || scope.inactive_owner_ids.contains(cut)) {
                            diagnostic(plan, id, "corner replacement requires both actual active target baseline cuts: " + cut);
                            eligible = false;
                        }
                    }
                    if (!baseline.contains(id))
                        diagnostic(plan, id, "corner replacement requires actual target baseline owner membership");
                    if (eligible) changed = corner_owners.insert(id).second || changed;
                } catch (const std::exception& error) {
                    diagnostic(plan, id, "affected corner aggregate is unsupported: " + std::string(error.what()));
                }
            }
            for (const auto& contact : contacts) {
                if (organization.drawing_context(contact.owner) != organization.drawing_context(contact.host) ||
                    std::abs(geometry.at(contact.owner).elevation - geometry.at(contact.host).elevation) > default_geometry_tolerance_metres) continue;
                if (walls.contains(contact.owner)) {
                    if (foreign_active_baseline.contains(contact.host) || !memberships.contains(contact.host))
                        diagnostic(plan, contact.host, "physical contact reaches a shared wall outside the target baseline registry");
                    changed = add_baseline(contact.host) || changed;
                }
                if (walls.contains(contact.host)) {
                    if (foreign_active_baseline.contains(contact.owner) || !memberships.contains(contact.owner))
                        diagnostic(plan, contact.owner, "physical contact reaches a shared wall outside the target baseline registry");
                    changed = add_baseline(contact.owner) || changed;
                }
            }
            for (const auto& contact : interior_contacts) {
                if (!walls.contains(contact.first) && !walls.contains(contact.second)) continue;
                if (contact.indeterminate) diagnostic(plan, contact.first, "physical contact is numerically indeterminate with " + contact.second);
                for (const auto* wall : {&contact.first, &contact.second}) {
                    if (foreign_active_baseline.contains(*wall) || !memberships.contains(*wall))
                        diagnostic(plan, *wall, "analytical physical contact reaches a shared wall outside the target baseline registry");
                    changed = add_baseline(*wall) || changed;
                }
            }
            // Descriptor lineage can reach a room without a direct wall-room
            // relation. Traverse that room before the fixed point closes so
            // every other typed baseline endpoint receives the same complete
            // replacement and registry admission. The room itself remains an
            // original owner for separately reviewed analytical completion.
            for (const auto& [id, entity] : source) {
                if (!is_physical_wall_room(entity) || scope.inactive_owner_ids.contains(id) ||
                    !touches(entity.extensions, walls)) continue;
                if (validate_physical_wall_room_descriptor(entity))
                    diagnostic(plan, id, "unsupported affected physical-room descriptor requires reviewed room completion");
                else if (room_touches(entity, walls)) changed = room_transit.insert(id).second || changed;
            }
            for (const auto& [id, constraint] : constraints) {
                (void)id;
                if (std::any_of(constraint.bindings.begin(), constraint.bindings.end(), [&](const auto& binding) {
                    return walls.contains(binding.owner_id) || analytical_endpoints.contains(binding.owner_id) || room_transit.contains(binding.owner_id);
                })) for (const auto& binding : constraint.bindings) {
                    changed = add_baseline(binding.owner_id) || changed;
                    const auto& endpoint = source.at(binding.owner_id);
                    if (endpoint.type == "wall" && (!memberships.contains(binding.owner_id) || foreign_active_baseline.contains(binding.owner_id)))
                        diagnostic(plan, id, "active relation reaches a shared wall outside the target baseline registry: " + binding.owner_id);
                    if (is_physical_wall_room(endpoint))
                        changed = room_transit.insert(binding.owner_id).second || changed;
                    if (!is_physical_wall_room(endpoint) &&
                        (can_recognize_boundary_entity_type(endpoint.type) || endpoint.type == "measurement_linework"))
                        changed = add_analytical(binding.owner_id) || changed;
                }
            }
            // Derived consumers and their complete canonical source strokes
            // participate in the same fixed point as physical/typed relations.
            // Copying a subset could alter the preserved measured graph.
            for (const auto& [id, entity] : source) {
                if (scope.inactive_owner_ids.contains(id) || !can_recognize_boundary_entity_type(entity.type) || is_physical_wall_room(entity)) continue;
                if (entity.properties.contains("wall_measurement_source")) {
                    std::vector<std::string> ids;
                    try { ids = exterior_wall_measurement_source_ids(entity); }
                    catch (const std::exception& error) {
                        if (analytical_endpoints.contains(id) || touches(entity.properties.at("wall_measurement_source"), walls))
                            diagnostic(plan, id, "affected exterior source has unsupported evidence: " + std::string(error.what()));
                        continue;
                    }
                    if (!analytical_endpoints.contains(id) && std::none_of(ids.begin(), ids.end(), [&](const auto& wall) { return walls.contains(wall); })) continue;
                    changed = add_analytical(id) || changed;
                    if (!wall_measurement_source_current(source, entity)) diagnostic(plan, id, "affected exterior consumer has stale physical source evidence");
                    for (const auto& wall : ids) {
                        if (!source.contains(wall) || source.at(wall).type != "wall" || !baseline.contains(wall) || scope.inactive_owner_ids.contains(wall))
                            diagnostic(plan, id, "exterior consumer requires complete target baseline wall sources: " + wall);
                        else changed = add_baseline(wall) || changed;
                    }
                }
                if (measured_source(entity)) {
                    // Unknown metadata is inspected read-only for relevance,
                    // then refused by the authoritative source/group checker.
                    if (!analytical_endpoints.contains(id) && !touches(entity.extensions, analytical_endpoints)) continue;
                    const auto check = measured_checks.find(id);
                    if (check == measured_checks.end() || !check->second.current) {
                        diagnostic(plan, id, "affected measured consumer has unsupported or stale source evidence: " +
                            (check == measured_checks.end() ? std::string("missing source check") : check->second.diagnostic)); continue;
                    }
                    changed = add_analytical(id) || changed;
                    for (const auto& source_id : measured_source_ids(entity)) if (source.contains(source_id)) {
                        if (source.at(source_id).type != "measurement_linework") diagnostic(plan, id, "measured consumer source is not a canonical stroke: " + source_id);
                        else changed = add_analytical(source_id) || changed;
                    }
                }
            }
            for (const auto& [id, join] : joins) {
                if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& wall) { return walls.contains(wall); }))
                    for (const auto& wall : join.wall_ids) {
                        if (!source.contains(wall) || source.at(wall).type != "wall") reject("wall join does not resolve an actual physical wall: " + wall);
                        if (!memberships.contains(wall) || foreign_active_baseline.contains(wall))
                            diagnostic(plan, id, "active wall join reaches a shared wall outside the target baseline registry: " + wall);
                        changed = add_baseline(wall) || changed;
                    }
            }
            if (walls.size() + analytical_endpoints.size() + room_transit.size() + corner_owners.size() > maximum_replacements) reject("closed wall replacement budget exceeded");
        }
        Ids owners = walls, children, rooms;
        owners.insert(analytical_endpoints.begin(), analytical_endpoints.end());
        owners.insert(corner_owners.begin(), corner_owners.end());
        rooms.insert(room_transit.begin(), room_transit.end());
        for (const auto& [id, entity] : source) {
            if (entity.type == "opening") {
                const auto host = entity.properties.find("wall_id");
                if (host == entity.properties.end() || !host->is_string() ||
                    !walls.contains(host->get_ref<const std::string&>()) ||
                    scope.inactive_owner_ids.contains(id)) continue;
                std::string wall_id, error;
                if (!read_document_wall_id(entity, wall_id, error)) reject(id + ": " + error);
                owners.insert(id);
                if (complete_corner_windows && (entity.properties.contains("corner_window_id") ||
                    entity.properties.contains("corner_leg"))) {
                    const auto owner = entity.properties.find("corner_window_id");
                    if (owner == entity.properties.end() || !owner->is_string() ||
                        !corner_owners.contains(owner->get_ref<const std::string&>()))
                        diagnostic(plan, id, "managed corner cut requires its complete eligible owner cohort");
                }
            }
        }
        if (!corner_owners.empty()) {
            // Shared admission proves raw context, vertical placement, exact
            // child ownership/dimensions and every saved phase membership.
            // A source-derived copy never repairs an invalid retained cohort.
            try { validate_corner_window_state(source); }
            catch (const std::exception& error) {
                for (const auto& id : corner_owners)
                    diagnostic(plan, id, "corner source cohort is not admissible: " + std::string(error.what()));
            }
        }
        // Validate complete hosted geometry, including opening envelope, before
        // any caller gets authority to create a copy.
        for (const auto& wall_id : walls) {
            std::vector<const Entity*> openings;
            for (const auto& id : owners) if (source.at(id).type == "opening" && source.at(id).properties.at("wall_id") == wall_id) openings.push_back(&source.at(id));
            auto placed = resolve_vertical_placement(source, source.at(wall_id));
            Wall wall; std::string error;
            if (!read_document_wall(placed, openings, wall, error)) reject(wall_id + ": " + error);
            validate_wall_semantics(wall);
            for (const auto& layer : wall.layers) children.insert(layer.id);
        }
        for (const auto& [id, join] : joins)
            if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& wall) { return walls.contains(wall); })) owners.insert(id);
        // A connected relation may also move a baseline analytical endpoint.
        // Admit only owners already qualified by the complete physical/source
        // fixed point. Physical rooms require the separate reviewed completion.
        changed = true;
        while (changed) {
            changed = false;
            for (const auto& [id, constraint] : constraints) {
                (void)id;
                if (!std::any_of(constraint.bindings.begin(), constraint.bindings.end(), [&](const auto& b) { return owners.contains(b.owner_id); })) continue;
                for (const auto& binding : constraint.bindings) {
                    const auto& endpoint = source.at(binding.owner_id);
                    if (owners.contains(binding.owner_id) || is_physical_wall_room(endpoint)) continue;
                    if (endpoint.type == "wall") {
                        // Baseline endpoints discovered through an exterior
                        // consumer must join the physical seed in a new plan;
                        // silently retaining them permits edits of shared facts.
                        if (baseline.contains(binding.owner_id)) diagnostic(plan, id, "connected baseline wall requires explicit physical closure: " + binding.owner_id);
                        continue;
                    }
                    if ((can_recognize_boundary_entity_type(endpoint.type) || endpoint.type == "measurement_linework") && analytical_endpoints.contains(binding.owner_id))
                        changed = owners.insert(binding.owner_id).second || changed;
                    else diagnostic(plan, id, "connected analytical endpoint requires a typed source replacement: " + binding.owner_id);
                }
            }
            if (owners.size() > maximum_replacements) reject("connected analytical replacement budget exceeded");
        }
        for (const auto& id : owners) if (can_recognize_boundary_entity_type(source.at(id).type)) {
            const auto& owner = source.at(id);
            const auto boundary = decode_identified_boundary_entity(owner);
            for (const auto& edge : boundary.segments) { children.insert(edge.segment_id); children.insert(edge.start_vertex_id); children.insert(edge.end_vertex_id); }
            for (const auto* key : {"receipt", "receipts"})
                if (owner.properties.contains(key) || owner.extensions.contains(key))
                    diagnostic(plan, id, "copied boundary has retained source/authoring evidence requiring its own typed completion");
            try {
                auto retained = owner;
                visit_boundary_copy_evidence(retained, [&](const std::string& child, bool owned) {
                    if (owned && !child.empty() && child != id) children.insert(child); return child;
                });
                if (const auto unsupported = validate_boundary_integrity({{id, owner}})) diagnostic(plan, id, *unsupported);
            } catch (const std::exception& error) { diagnostic(plan, id, "copied boundary proof cannot be independently remapped: " + std::string(error.what())); }
        }
        for (const auto& id : owners) if (source.at(id).type == "measurement_linework") {
            auto retained = source.at(id);
            visit_stroke_copy(retained, [&](const std::string& child, bool owned) {
                if (owned) children.insert(child); return child;
            });
        }
        for (const auto& [id, constraint] : constraints)
            if (std::any_of(constraint.bindings.begin(), constraint.bindings.end(), [&](const auto& binding) { return owners.contains(binding.owner_id) || rooms.contains(binding.owner_id); })) {
                owners.insert(id);
                if (std::any_of(constraint.bindings.begin(), constraint.bindings.end(), [&](const auto& binding) {
                    return is_physical_wall_room(source.at(binding.owner_id));
                })) {
                    plan.room_constraint_ids_requiring_review.push_back(id);
                    plan.diagnostics.push_back({id, "copied constraint retains physical-room bindings requiring explicit reviewed continuity", false});
                }
            }
        for (const auto& id : rooms)
            if (validate_physical_wall_room_descriptor(source.at(id)))
                diagnostic(plan, id, "unsupported physical-room transit requires reviewed source completion");
        for (const auto& [id, entity] : source) if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (decoded.dimension && (!complete_component_dimensions || decoded.dimension->kind != BoundaryDimensionKind::assembly_extent) &&
                owners.contains(decoded.dimension->boundary_id)) {
                validate_boundary_dimension_target(*decoded.dimension, source.at(decoded.dimension->boundary_id)); owners.insert(id);
            }
        }
        // A retained construction insertion can name a still-live dimension;
        // that entity is copied through its normal typed target codec. Deleted
        // historical labels remain reserved children of the copied proof.
        for (auto child = children.begin(); child != children.end();) {
            if (!source.contains(*child)) { ++child; continue; }
            if (!owners.contains(*child) || !can_recognize_boundary_dimension_entity_type(source.at(*child).type))
                diagnostic(plan, *child, "historical proof identity aliases an unrelated actual source entity");
            child = children.erase(child);
        }
        if (owners.size() + children.size() > maximum_replacements) reject("replacement entity/child budget exceeded");
        const auto catalogs = complete_hosted_instances ? discover_hosted_instances(plan, source, owners,
            scope, organization, memberships, baseline) : Ids{};
        const auto component_dimensions = complete_component_dimensions ? discover_component_dimensions(plan, source,
            owners.size() + catalogs.size() + children.size() + plan.required_hosted_instance_ids.size()) : ComponentDimensionRoots{};
        Ids presentation_owners = owners;
        for (const auto& [id, key] : component_dimensions) { (void)key; presentation_owners.insert(id); }
        if (complete_hosted_instances) {
            const auto aliases = embedded_assembly_presentation_ids(source);
            for (const auto& key : plan.required_hosted_instance_ids) presentation_owners.insert(aliases.at(key));
        }
        // Distinct source owners cannot silently share a remapped child ID.
        // Inspect typed owned identities globally, including inactive designs.
        enum class ChildOwnerKind { physical_entity, sheet_view };
        using ChildOwner = std::pair<ChildOwnerKind, std::pair<std::string, std::string>>;
        std::map<std::string, ChildOwner, std::less<>> child_owners;
        Ids ambiguous_children;
        const auto reserve_child = [&](const std::string& child, const std::string& owner) {
            if (source.contains(child)) reject("owned child collides with actual entity identity: " + child);
            const ChildOwner owner_key{ChildOwnerKind::physical_entity, {owner, {}}};
            const auto [found, inserted] = child_owners.emplace(child, owner_key);
            if (!inserted && found->second != owner_key) ambiguous_children.insert(child);
            if (!inserted && found->second != owner_key && children.contains(child))
                reject("replacement child identity is owned by multiple source entities: " + child);
        };
        for (const auto& [id, entity] : source) {
            if (entity.type == "wall" && entity.properties.contains("layers")) {
                const auto& layers = entity.properties.at("layers");
                if (!layers.is_array()) reject("wall layers must be an array");
                for (const auto& layer : layers) reserve_child(layer.at("id").get<std::string>(), id);
            }
            if (can_recognize_boundary_entity_type(entity.type) && inspect_boundary_entity_version(entity).format == BoundaryEntityFormat::identified_v1)
                for (const auto& edge : decode_identified_boundary_entity(entity).segments) {
                    reserve_child(edge.segment_id, id); reserve_child(edge.start_vertex_id, id); reserve_child(edge.end_vertex_id, id);
                }
            // Historical topology and canonical stroke children occupy the
            // same document namespace, including inactive designs. Future
            // proofs are retained in the global string reservation above.
            auto retained = entity;
            Ids proof_children;
            try {
                const auto collect = [&](const std::string& child, bool owned) {
                    if (owned && !child.empty() && child != id && !source.contains(child)) proof_children.insert(child); return child;
                };
                if (entity.type == "measurement_linework") visit_stroke_copy(retained, collect);
                else if (can_recognize_boundary_entity_type(entity.type)) visit_boundary_copy_evidence(retained, collect);
            } catch (const std::exception&) {
                if (owners.contains(id)) diagnostic(plan, id, "affected owned identities require a supported source proof");
            }
            for (const auto& child : proof_children) reserve_child(child, id);
            if (complete_presentations && entity.type == kSheetViewEntityType) {
                try {
                    const auto views = decode_sheet_view_entity(entity);
                    for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
                        if (presentation_owners.contains(overlay.object_id) ||
                            (overlay.dimension_binding && presentation_owners.contains(overlay.dimension_binding->object_id)))
                            children.insert(overlay.id);
                        // Scan the complete roster even when an unrelated
                        // retained overlay already aliases an entity. A later
                        // row may own one of this replacement's required IDs.
                        const ChildOwner owner_key{ChildOwnerKind::sheet_view, {id, view.id}};
                        const auto [found, inserted] = child_owners.emplace(overlay.id, owner_key);
                        if (source.contains(overlay.id) || (!inserted && found->second != owner_key))
                            ambiguous_children.insert(overlay.id);
                    }
                } catch (const std::exception& error) {
                    if (touches(entity.properties, presentation_owners) || touches(entity.extensions, presentation_owners))
                        diagnostic(plan, id, "affected sheet/view record is unsupported: " + std::string(error.what()));
                }
            }
        }
        if (complete_presentations) {
            for (const auto& child : children) if (ambiguous_children.contains(child))
                diagnostic(plan, child, "replacement child identity is owned by multiple actual source owners");
            if (owners.size() + children.size() > maximum_replacements) reject("replacement entity/child budget exceeded");
        }
        if (owners.size() + catalogs.size() + children.size() + plan.required_hosted_instance_ids.size() +
            plan.required_dimension_ids.size() > maximum_replacements)
            reject("replacement entity/child/hosted/dimension budget exceeded");
        Ids affected = presentation_owners; affected.insert(children.begin(), children.end());
        Ids component_dimension_dependencies = affected;
        for (const auto& key : plan.required_hosted_instance_ids) {
            component_dimension_dependencies.insert(key.first);
            component_dimension_dependencies.insert(key.second);
        }
        for (const auto& id : owners) {
            if (children.contains(id)) reject("entity and owned-child identities overlap: " + id);
            const auto member = memberships.find(id);
            if (is_model_phase_entity_type(source.at(id).type) && member == memberships.end())
                diagnostic(plan, id, "copied model owner must be an actual target baseline registry member");
            if (member != memberships.end() && (member->second != registry_id || !baseline.contains(id)))
                diagnostic(plan, id, "copy would supersede a registered owner outside the target shared baseline");
            try {
                const auto remainder = opaque_remainder(source.at(id), complete_presentations, complete_corner_windows);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "copied payload contains an affected reference outside supported typed fields");
            } catch (const std::exception& error) { diagnostic(plan, id, "copied payload has unsupported typed evidence: " + std::string(error.what())); }
        }
        for (const auto& [id, entity] : source) {
            if (owners.contains(id) || (scope.inactive_owner_ids.contains(id) && !component_dimensions.contains(id))) continue;
            if (rooms.contains(id)) {
                auto retained = entity; retained.extensions.erase("physical_wall_room");
                if (touches(retained.properties, affected) || touches(retained.extensions, affected))
                    diagnostic(plan, id, "retained physical room has an affected opaque dependency outside its typed source descriptor");
                continue;
            }
            if (entity.type == "model_phases") {
                auto retained = entity; retained.properties.erase("model");
                if (touches(retained.properties, affected) || touches(retained.extensions, affected))
                    diagnostic(plan, id, "phase registry has an affected opaque dependency outside its typed model");
                continue;
            }
            if (entity.type == assembly_phase_membership_entity_type) {
                const auto remainder = opaque_remainder(entity);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "assembly phase companion has an affected opaque dependency outside its closed model");
                continue;
            }
            if (entity.type == "assembly_model" &&
                (touches(entity.properties, affected) || touches(entity.extensions, affected))) {
                try {
                    const auto catalog = AssemblyModel::from_json(entity.properties.at("model"));
                    auto remainder = entity;
                    Ids inactive_rows;
                    for (const auto& instance : catalog.instances()) {
                        if (!instance.placement || !owners.contains(instance.placement->host_entity_id)) continue;
                        const AssemblyPhaseRoot root{id, instance.id};
                        if (scope.assembly_memberships.owners.contains(root) &&
                            scope.assembly_memberships.inactive_roots.contains(root)) {
                            inactive_rows.insert(instance.id);
                            continue;
                        }
                        if (complete_hosted_instances && std::binary_search(plan.required_hosted_instance_ids.begin(),
                            plan.required_hosted_instance_ids.end(), root)) {
                            inactive_rows.insert(instance.id);
                            continue;
                        }
                        // This historical plan has no catalog-qualified row
                        // allocation or materialization authority. Retaining
                        // the refusal prevents silently activating a host copy.
                        diagnostic(plan, id, "active hosted assembly row requires qualified wall/opening catalog and local-instance allocation with typed copy replay: " + instance.id);
                    }
                    // Inactive declarations stay in their original catalog;
                    // exclude only their exact typed host slots from dependency
                    // discovery. Unlisted rows retain the legacy refusal.
                    for (auto& row : remainder.properties.at("model").at("instances"))
                        if (inactive_rows.contains(row.at("id").get<std::string>()))
                            row.at("placement").erase("host_entity_id");
                    if (complete_hosted_instances) remainder = catalog_remainder(entity, inactive_rows);
                    if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                        diagnostic(plan, id, "affected dependent has no safe typed replacement codec; original remains preserved");
                } catch (const std::exception& error) {
                    diagnostic(plan, id, "affected hosted catalog requires supported typed source admission: " + std::string(error.what()));
                }
                continue;
            }
            if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (decoded.constraint && !constraint_participates(*decoded.constraint, scope)) continue;
            }
            if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                if (component_dimensions.contains(id)) {
                    const auto remainder = component_dimension_remainder(entity);
                    if (touches(remainder.properties, component_dimension_dependencies) ||
                        touches(remainder.extensions, component_dimension_dependencies))
                        diagnostic(plan, id, "component dimension has an affected reference outside its validated qualified target");
                    continue;
                }
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (decoded.dimension && scope.inactive_owner_ids.contains(decoded.dimension->boundary_id)) continue;
            }
            if (complete_presentations && (entity.type == kSheetViewEntityType || entity.type == kAnnotationEntityType)) {
                try {
                    const auto remainder = opaque_remainder(entity, true);
                    if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                        diagnostic(plan, id, "presentation has an affected reference outside supported typed fields");
                } catch (const std::exception& error) {
                    if (touches(entity.properties, affected) || touches(entity.extensions, affected))
                        diagnostic(plan, id, "affected presentation record is unsupported: " + std::string(error.what()));
                }
                continue;
            }
            if (touches(entity.properties, affected) || touches(entity.extensions, affected))
                diagnostic(plan, id, "affected dependent has no safe typed replacement codec; original remains preserved");
        }
        plan.required_entity_ids.assign(owners.begin(), owners.end());
        plan.required_entity_ids.insert(plan.required_entity_ids.end(), catalogs.begin(), catalogs.end());
        std::sort(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        plan.required_child_ids.assign(children.begin(), children.end());
        plan.affected_original_room_ids.assign(rooms.begin(), rooms.end());
        std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
            return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
        });
        return plan;
    } catch (const Json::exception& error) { reject(std::string("malformed typed source: ") + error.what()); }
}

PhaseWallReplacementResult replay_phase_wall_replacement(
    const PhaseWallReplacementEntities& source, const PhaseWallReplacementPlan& plan,
    const PhaseWallReplacementIdentityMap& identities,
    const PhaseWallReplacementHostedInstanceIdentityMap& hosted_instance_identities,
    const PhaseWallReplacementIdentityMap& dimension_identities) {
    try {
        if (!plan.complete_component_dimensions && !dimension_identities.empty())
            reject("historical replacement does not admit component dimension mappings");
        const auto derived = inspect_phase_wall_replacement_plan(source, plan.seed_wall_ids, plan.registry_id,
            plan.alternative_id, plan.complete_presentations, plan.complete_corner_windows, plan.complete_hosted_instances,
            plan.complete_component_dimensions);
        if (derived != plan) reject("supplied plan differs from actual source discovery");
        if (!derived.ready()) reject("replacement has unresolved affected dependencies");
        Ids expected(derived.required_entity_ids.begin(), derived.required_entity_ids.end());
        expected.insert(derived.required_child_ids.begin(), derived.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires the complete exact entity/child mapping");
        Strings occupied;
        for (const auto& [id, entity] : source) { occupied.values.insert(id); occupied.read(entity.properties); occupied.read(entity.extensions); }
        EmbeddedAssemblyPresentationIds original_aliases;
        Ids presentation_owners;
        for (const auto& id : derived.required_entity_ids)
            if (source.at(id).type != "assembly_model") presentation_owners.insert(id);
        presentation_owners.insert(derived.required_dimension_ids.begin(), derived.required_dimension_ids.end());
        if (derived.complete_hosted_instances) {
            original_aliases = embedded_assembly_presentation_ids(source);
            for (const auto& [key, alias] : original_aliases) { (void)key; occupied.values.insert(alias); }
            for (const auto& key : derived.required_hosted_instance_ids) presentation_owners.insert(original_aliases.at(key));
        }
        Ids presentation_source_ids;
        if (derived.complete_presentations || derived.complete_hosted_instances) {
            for (const auto* key : {"id", "type", "properties", "required", "extensions"}) occupied.values.insert(key);
            for (const auto& [id, entity] : source) { (void)id; occupied.values.insert(entity.type); }
            // Extend only names of actual affected overlays admitted by the
            // source plan. Physical children and all destinations stay strict.
            for (const auto& [id, entity] : source) {
                (void)id;
                if (entity.type != kSheetViewEntityType ||
                    (!touches(entity.properties, presentation_owners) && !touches(entity.extensions, presentation_owners))) continue;
                const auto views = decode_sheet_view_entity(entity);
                for (const auto& view : views.views()) for (const auto& overlay : view.overlays)
                    if (presentation_owners.contains(overlay.object_id) ||
                        (overlay.dimension_binding && presentation_owners.contains(overlay.dimension_binding->object_id)))
                        presentation_source_ids.insert(overlay.id);
            }
        }
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (presentation_source_ids.contains(old_id)) presentation_source_identity(old_id);
            else identity(old_id);
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        const std::set<PhaseWallReplacementHostedInstanceKey> expected_instances(
            derived.required_hosted_instance_ids.begin(), derived.required_hosted_instance_ids.end());
        if (hosted_instance_identities.size() != expected_instances.size()) reject("requires exact qualified hosted instance mapping");
        for (const auto& [key, new_id] : hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("unrequested qualified hosted instance mapping");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh hosted identity collision: " + new_id);
        }
        const Ids expected_dimensions(derived.required_dimension_ids.begin(), derived.required_dimension_ids.end());
        if (dimension_identities.size() != expected_dimensions.size()) reject("requires exact component dimension mapping");
        for (const auto& [old_id, new_id] : dimension_identities) {
            if (!expected_dimensions.contains(old_id) || expected.contains(old_id)) reject("unrequested or overlapping component dimension mapping");
            identity(old_id); identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh dimension identity collision: " + new_id);
        }
        auto copy_identities = identities;
        for (const auto& [old_id, new_id] : dimension_identities)
            if (!copy_identities.emplace(old_id, new_id).second) reject("component dimension overlaps an existing mapping");
        if (derived.complete_hosted_instances && fresh.size() > maximum_replacements) reject("replacement fresh identity budget exceeded");
        if (derived.complete_hosted_instances && derived.required_entity_ids.size() + expected_dimensions.size() > maximum_entities - source.size())
            reject("final entity budget exceeded");
        if (derived.complete_hosted_instances) {
            // Each private replica initially copies its complete source
            // envelope before narrowing rows. Bound the retained catalogs plus
            // every full replica before the first whole-map/catalog copy.
            AssemblyCatalogTransferBudget destination_work;
            for (const auto& [id, entity] : source) {
                (void)id;
                if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, destination_work);
            }
            for (const auto& id : derived.required_entity_ids)
                if (source.at(id).type == "assembly_model")
                    admit_existing_assembly_catalog_work(source.at(id), destination_work);
        }
        PhaseWallReplacementResult result{source, copy_identities, {}, derived.affected_original_room_ids,
            derived.room_constraint_ids_requiring_review, {}, {}};
        AssemblyPhaseCopiedRoots declared_copied_roots;
        AssemblyPhaseCopiedHosts declared_copied_hosts;
        AssemblyCatalogTransferBudget copied_catalog_work;
        const auto source_scope = derived.complete_hosted_instances ? constraint_phase_scope(source) : ConstraintPhaseScope{};
        const auto model = ModelPhases::from_json(source.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& a) { return a.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("target alternative disappeared");
        const Ids baseline(model.baseline_ids().begin(), model.baseline_ids().end());
        for (const auto& old_id : derived.required_entity_ids) {
            auto copy = source.at(old_id);
            const auto proof_reference = [&](const std::string& id, bool owned) {
                if (id.empty()) return id;
                return owned ? copy_identities.at(id) : remap(id, copy_identities);
            };
            if (copy.type == "measurement_linework") visit_stroke_copy(copy, proof_reference);
            else if (can_recognize_boundary_entity_type(copy.type)) {
                visit_boundary_copy_evidence(copy, proof_reference);
                visit_measured_sources(copy, [&](const std::string& id, bool) { return identities.at(id); });
            }
            copy.id = identities.at(old_id);
            auto& p = copy.properties;
            if (copy.type == "wall") {
                if (p.contains("layers")) for (auto& layer : p.at("layers")) remap_field(layer, "id", identities);
            } else if (copy.type == "opening") {
                remap_field(p, "wall_id", identities);
                if (derived.complete_corner_windows) remap_field(p, "corner_window_id", identities);
            } else if (copy.type == "corner_window" && derived.complete_corner_windows) {
                for (const auto* key : {"wall_ids", "opening_ids"})
                    for (auto& id : p.at(key)) id = identities.at(id.get<std::string>());
                (void)parse_corner_window(copy);
            } else if (copy.type == "wall_join") {
                auto join = parse_wall_join(p, copy.id);
                for (auto& id : join.wall_ids) id = remap(id, identities);
                p["wall_ids"] = wall_join_json(join).at("wall_ids");
            } else if (copy.type == "measurement_linework") {
                copy.extensions["measurement_linework_copy_scope"] = {{"version", 1}};
                (void)measurement_linework_copy_isolated(copy);
                const auto decoded = decode_measurement_linework_model(p.at("model"));
                if (!decoded.supported() || decoded.model->stroke_id != copy.id) reject("copied measured stroke lost independent replayable identity");
            } else if (can_recognize_boundary_entity_type(copy.type)) {
                for (auto& edge : p.at("segments")) for (const auto* key : {"segment_id", "start_vertex_id", "end_vertex_id"}) remap_field(edge, key, identities);
                if (p.contains("wall_measurement_source")) {
                    auto& records = p.at("wall_measurement_source").at("walls");
                    for (auto& record : records) remap_field(record, "id", identities);
                    std::sort(records.begin(), records.end(), [](const Json& a, const Json& b) { return a.at("id").get<std::string>() < b.at("id").get<std::string>(); });
                }
                (void)decode_identified_boundary_entity(copy);
                if (const auto unsupported = validate_boundary_integrity({{copy.id, copy}})) reject(*unsupported);
            } else if (copy.type == "assembly_model" && derived.complete_hosted_instances) {
                const auto retained = p.at("model").at("instances");
                auto& rows = p.at("model").at("instances");
                rows = Json::array();
                for (auto row : retained) {
                    const PhaseWallReplacementHostedInstanceKey key{old_id, row.at("id").get<std::string>()};
                    if (!expected_instances.contains(key)) continue;
                    const auto host = row.at("placement").at("host_entity_id").get<std::string>();
                    if (!identities.contains(host) || (source.at(host).type != "wall" && source.at(host).type != "opening"))
                        reject("proposed hosted row lost its actual copied wall/opening host");
                    const PhaseWallReplacementHostedInstanceKey destination{copy.id, hosted_instance_identities.at(key)};
                    row.at("id") = destination.second;
                    row.at("placement").at("host_entity_id") = identities.at(host);
                    rows.push_back(std::move(row));
                    if (!result.hosted_instance_original_to_proposed.emplace(key, destination).second)
                        reject("duplicate qualified hosted copy");
                    if (source_scope.assembly_memberships.owners.contains(key)) {
                        declared_copied_roots.emplace(key, destination);
                        declared_copied_hosts.emplace(host, identities.at(host));
                    }
                }
                if (rows.empty()) reject("copied catalog requires its exact nonempty qualified roster");
                admit_existing_assembly_catalog_work(copy, copied_catalog_work);
                (void)AssemblyModel::from_json(p.at("model"));
            } else if (copy.type == "constraint") {
                for (auto& binding : p.at("bindings")) for (const auto* key : {"owner_id", "segment_id", "vertex_id"}) remap_field(binding, key, copy_identities);
                for (const auto* key : {"wall_ids", "entity_ids"}) if (p.contains(key)) {
                    auto ids = p.at(key).get<std::vector<std::string>>();
                    for (auto& id : ids) id = remap(id, copy_identities);
                    std::sort(ids.begin(), ids.end()); p.at(key) = ids;
                }
                const auto decoded = decode_constraint_entity(copy);
                if (!decoded.constraint) reject("copied constraint lost understood semantics");
                copy = encode_constraint_entity(*decoded.constraint, &copy);
            } else if (can_recognize_boundary_dimension_entity_type(copy.type)) {
                auto& target_fields = p.at("target");
                for (const auto* key : {"entity_id", "segment_id", "second_segment_id", "vertex_id"}) remap_field(target_fields, key, identities);
                if (target_fields.contains("segment_ids")) for (auto& id : target_fields.at("segment_ids")) id = remap(id.get<std::string>(), identities);
                const auto decoded = decode_boundary_dimension_entity(copy);
                if (!decoded.dimension) reject("copied dimension lost understood semantics");
                copy = encode_boundary_dimension_entity(*decoded.dimension, &copy);
            } else reject("unsupported entity reached replay: " + old_id);
            if (copy.type == "constraint" && std::binary_search(derived.room_constraint_ids_requiring_review.begin(),
                derived.room_constraint_ids_requiring_review.end(), old_id))
                result.deferred_room_constraints.emplace(copy.id, copy);
            else if (!result.entities.emplace(copy.id, copy).second) reject("copy insertion collides");
            if (is_model_phase_entity_type(copy.type)) {
                model_ids.push_back(copy.id); target->proposed_ids.push_back(copy.id);
                if (copy.type != "assembly_model" && baseline.contains(old_id)) target->demolished_ids.push_back(old_id);
            }
        }
        // Preserve the raw order of all original registry lists and every other
        // alternative. Canonical validation admits only our exact additions.
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = source.at(plan.registry_id).properties.at("model");
        for (const auto& id : derived.required_entity_ids) if (is_model_phase_entity_type(source.at(id).type)) raw.at("entity_ids").push_back(identities.at(id));
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id) {
            for (const auto& id : derived.required_entity_ids) if (is_model_phase_entity_type(source.at(id).type)) {
                alternative.at("proposed_ids").push_back(identities.at(id));
                if (source.at(id).type != "assembly_model" && baseline.contains(id)) alternative.at("demolished_ids").push_back(id);
            }
        }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json()) reject("registry reconstruction differs from canonical update");
        result.entities.at(plan.registry_id).properties["model"] = std::move(raw);
        if (!declared_copied_roots.empty())
            result.entities = transport_assembly_phase_memberships_for_host_copies(source, result.entities,
                declared_copied_roots, declared_copied_hosts, plan.registry_id, plan.alternative_id);
        if (derived.complete_component_dimensions) {
            // Complete initial physical/catalog copies precede a single atomic
            // contribution. Geometry edits belong to the enclosing stage.
            std::map<PhaseWallReplacementHostedInstanceKey, std::size_t> requests_by_root;
            std::vector<AssemblyDimensionTransformRequest> requests;
            for (const auto& id : derived.required_dimension_ids) {
                const auto decoded = decode_boundary_dimension_entity(source.at(id));
                const auto& component = decoded.dimension->assembly_target->component;
                const PhaseWallReplacementHostedInstanceKey key{component.catalog_id, component.instance_id};
                const auto [request, inserted] = requests_by_root.emplace(key, requests.size());
                if (inserted) requests.push_back({{key.first, key.second, std::nullopt, {}},
                    {identities.at(key.first), hosted_instance_identities.at(key), std::nullopt, {}},
                    AssemblyDimensionTransformMode::clone, {}});
                requests.at(request->second).fresh_dimension_ids.emplace(id, dimension_identities.at(id));
            }
            const auto copies = requests.empty() ? std::vector<Entity>{} :
                carry_assembly_dimension_transform_entities(source, result.entities, requests);
            if (copies.size() != expected_dimensions.size()) reject("component dimension contribution differs from exact source roster");
            Ids copied_ids;
            for (const auto& [old_id, new_id] : dimension_identities) {
                (void)old_id;
                copied_ids.insert(new_id);
            }
            for (const auto& copy : copies)
                if (!copied_ids.contains(copy.id) || !result.entities.emplace(copy.id, copy).second)
                    reject("component dimension contribution has an undeclared or colliding owner");

            // Append only NEW owners to the actual saved active choice. Raw
            // original lists and every other alternative retain their order.
            auto& registry = result.entities.at(plan.registry_id);
            const auto phases = ModelPhases::from_json(registry.properties.at("model"));
            if (phases.active_alternative() != std::optional<std::string>{plan.alternative_id})
                reject("component dimension copy lost the actual active alternative");
            auto dimension_model_ids = phases.entity_ids();
            auto dimension_alternatives = phases.alternatives();
            const auto dimension_target = std::find_if(dimension_alternatives.begin(), dimension_alternatives.end(),
                [&](const auto& row) { return row.id == plan.alternative_id; });
            if (dimension_target == dimension_alternatives.end()) reject("component dimension copy lost its registry choice");
            auto dimension_raw = registry.properties.at("model");
            for (const auto& id : copied_ids) {
                dimension_model_ids.push_back(id); dimension_target->proposed_ids.push_back(id);
                dimension_raw.at("entity_ids").push_back(id);
            }
            for (auto& row : dimension_raw.at("alternatives")) if (row.at("id") == plan.alternative_id)
                for (const auto& id : copied_ids) row.at("proposed_ids").push_back(id);
            const auto dimension_phases = ModelPhases::create(dimension_model_ids, phases.baseline_ids(),
                dimension_alternatives, phases.active_alternative());
            if (ModelPhases::from_json(dimension_raw).to_json() != dimension_phases.to_json())
                reject("raw dimension append differs from validated phase update");
            registry.properties.at("model") = std::move(dimension_raw);
        }
        auto presentation_identities = result.original_to_proposed;
        if (derived.complete_hosted_instances) {
            if (result.hosted_instance_original_to_proposed.size() != expected_instances.size())
                reject("copied hosted roster differs from actual qualified source discovery");
            const auto final_aliases = embedded_assembly_presentation_ids(result.entities);
            for (const auto& [key, alias] : original_aliases)
                if (final_aliases.at(key) != alias) reject("replacement changed an original hosted presentation alias");
            for (const auto& [key, destination] : result.hosted_instance_original_to_proposed) {
                const auto& alias = final_aliases.at(destination);
                if (occupied.values.contains(alias) || (fresh.contains(alias) && alias != destination.second))
                    reject("proposed hosted presentation alias collision");
                if (!presentation_identities.emplace(original_aliases.at(key), alias).second)
                    reject("hosted presentation alias overlaps mapped owner identity");
            }
        }
        if (derived.complete_corner_windows && std::any_of(derived.required_entity_ids.begin(),
            derived.required_entity_ids.end(), [&](const auto& id) { return source.at(id).type == "corner_window"; }))
            validate_corner_window_state(result.entities);
        if (derived.complete_presentations)
            complete_presentation(result.entities, source, presentation_owners, presentation_identities);
        const auto result_scope = constraint_phase_scope(result.entities);
        if (derived.complete_hosted_instances) {
            HostedGeometryAdmission geometry;
            std::set<PhaseWallReplacementHostedInstanceKey> destinations;
            for (const auto& [key, destination] : result.hosted_instance_original_to_proposed) {
                (void)key; destinations.insert(destination);
            }
            for (const auto& id : derived.required_entity_ids) {
                const auto& original = source.at(id);
                const auto& retained = result.entities.at(id);
                if (original != retained || original.properties.dump() != retained.properties.dump() ||
                    original.extensions.dump() != retained.extensions.dump()) reject("replacement changed an original copy source");
                if (original.type != "assembly_model") continue;
                const auto& catalog = result.entities.at(identities.at(id));
                const auto decoded = AssemblyModel::from_json(catalog.properties.at("model"));
                for (const auto& instance : decoded.instances()) {
                    const PhaseWallReplacementHostedInstanceKey key{catalog.id, instance.id};
                    if (!destinations.contains(key) || !instance.placement)
                        reject("proposed catalog contains an unrequested hosted row");
                    if (!hosted_root_active(result.entities, result_scope, key, instance.placement->host_entity_id))
                        reject("copied hosted row is inactive in the proposed design");
                    geometry.admit(result.entities, result_scope, decoded, instance);
                }
            }
        }
        Ids result_visible;
        for (const auto& [id, entity] : result.entities) if (!result_scope.inactive_owner_ids.contains(id)) result_visible.insert(id);
        const auto copied_measured_checks = measurement_linework_source_checks(result.entities, &result_visible);
        for (const auto& old_id : derived.required_entity_ids) {
            if (result.deferred_room_constraints.contains(identities.at(old_id))) continue;
            const auto& copy = result.entities.at(identities.at(old_id));
            if (measured_source(copy) && !measurement_linework_source_current(copied_measured_checks, copy))
                reject("copied measured consumer is not current against its complete independent source cohort");
            if (copy.type == "measurement_boundary" && copy.properties.contains("wall_measurement_source") && !wall_measurement_source_current(result.entities, copy))
                reject("copied exterior consumer is not current against full proposed sources");
            if (can_recognize_boundary_dimension_entity_type(copy.type)) {
                const auto decoded = decode_boundary_dimension_entity(copy);
                validate_boundary_dimension_target(*decoded.dimension, result.entities.at(decoded.dimension->boundary_id));
            }
        }
        if (derived.complete_component_dimensions) {
            std::vector<AssemblyDimensionTarget> copied_targets;
            for (const auto& old_id : derived.required_dimension_ids) {
                const auto& original = source.at(old_id);
                const auto& retained = result.entities.at(old_id);
                if (original != retained || original.properties.dump() != retained.properties.dump() ||
                    original.extensions.dump() != retained.extensions.dump()) reject("component copy changed an original dimension");
                const auto decoded = decode_boundary_dimension_entity(result.entities.at(dimension_identities.at(old_id)));
                if (!decoded.supported() || decoded.version != 6 || decoded.dimension->kind != BoundaryDimensionKind::assembly_extent ||
                    !decoded.dimension->assembly_target || decoded.dimension->assembly_target->component.document_entity_id)
                    reject("component dimension copy lost its supported embedded extent");
                auto expected_target = *decode_boundary_dimension_entity(original).dimension->assembly_target;
                const PhaseWallReplacementHostedInstanceKey root{expected_target.component.catalog_id, expected_target.component.instance_id};
                expected_target.component.catalog_id = identities.at(root.first);
                expected_target.component.instance_id = hosted_instance_identities.at(root);
                if (*decoded.dimension->assembly_target != expected_target || decoded.dimension->boundary_id != expected_target.component.catalog_id)
                    reject("component dimension copy differs from its exact qualified target transport");
                copied_targets.push_back(*decoded.dimension->assembly_target);
            }
            // Qualified extents require complete-map resolution; ordinary
            // copied dimensions keep the entity-target check above.
            if (!copied_targets.empty()) (void)resolve_assembly_dimensions(result.entities, copied_targets);
        }
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed typed replay: ") + error.what()); }
}

} // namespace sketch
