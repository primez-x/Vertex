#include "sketch/drawing_selection_removal.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architectural_selection_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_selection_removal.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/opening_architectural_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/mixed_wall_opening_removal.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/phase_selection_removal.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/sheet_view_restriction_migration.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = DrawingSelectionRemovalEntities;
using Ids = std::set<std::string, std::less<>>;
using Children = std::map<std::string, Ids, std::less<>>;
using OverlayChildren = std::map<std::pair<std::string, std::string>, Ids>;
constexpr std::size_t entity_limit = 65536, target_limit = 1000, row_limit = 65536;
constexpr std::size_t byte_limit = 64 * 1024 * 1024, node_limit = 2 * 1024 * 1024;
constexpr std::size_t scan_limit = 32 * 1024 * 1024, work_limit = 16 * 1024 * 1024;
constexpr std::size_t intent_byte_limit = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Drawing selection removal: " + reason);
}
void identity(std::string_view id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
void annotation_identity(std::string_view id) {
    // The actual annotation codec permits arbitrary nonempty local spellings.
    // Owner/kind qualification, actual-row lookup and alias collision admission
    // supply authority; imposing the document-ID alphabet would lose old rows.
    if (id.empty() || id.size() > 256) reject("annotation child ID must contain 1..256 bytes");
}
const Json* field(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
std::string kind_name(DrawingSelectionAnnotationKind kind) {
    switch (kind) {
    case DrawingSelectionAnnotationKind::label: return "label";
    case DrawingSelectionAnnotationKind::symbol: return "symbol";
    }
    reject("unknown annotation selection kind");
}
bool owner_override(const Json& row) {
    const auto kind=row.at("target_kind").get<std::string>();
    return kind=="object" || kind=="area" || kind=="area_name" ||
        kind=="area_calculation" || kind=="wall_dimension";
}
bool child_override(const Json& row,const Ids& children) {
    return row.at("target_kind")=="object" && children.contains(row.at("target_id").get<std::string>());
}
auto key(const DrawingSelectionAnnotationTarget& target) {
    return std::tuple{target.owner_id, kind_name(target.kind), target.child_id};
}
void intent_bounds(const DrawingSelectionRemovalIntent& intent) {
    if (intent.owner_ids.size() > target_limit ||
        intent.annotations.size() > target_limit - intent.owner_ids.size() ||
        (intent.owner_ids.empty() && intent.annotations.empty()))
        reject("requires 1..1000 aggregate actual owners/annotation rows");
    std::string previous;
    for (const auto& id : intent.owner_ids) {
        identity(id);
        if (!previous.empty() && previous >= id) reject("owner IDs must be strictly sorted and unique");
        previous = id;
    }
    for (std::size_t i = 0; i < intent.annotations.size(); ++i) {
        const auto& target = intent.annotations[i];
        identity(target.owner_id); annotation_identity(target.child_id); (void)kind_name(target.kind);
        if (i && key(intent.annotations[i - 1]) >= key(target))
            reject("qualified annotation targets must be strictly sorted and unique");
        if (std::binary_search(intent.owner_ids.begin(), intent.owner_ids.end(), target.owner_id))
            reject("annotation owner cannot also be selected as a drawing root");
    }
}
struct Budget {
    std::size_t nodes{}, bytes{}, scans{}, work{}, wire_upper_bound{};
    void wire(std::size_t count) {
        if (count > byte_limit - wire_upper_bound) reject("conservative complete source wire-byte budget exceeded");
        wire_upper_bound += count;
    }
    void text(std::string_view value) {
        if (value.size() > byte_limit - bytes) reject("aggregate source string/key byte budget exceeded");
        if (value.size() > byte_limit / 6) reject("individual source string/key byte budget exceeded");
        bytes += value.size();
        wire(6 * value.size() + 3);
    }
    void charge(std::size_t count) {
        if (count > work_limit - work) reject("analytical replay work budget exceeded");
        work += count;
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("aggregate JSON node/nesting budget exceeded");
        wire(32);
        if (value.is_binary() || value.is_discarded()) reject("source JSON cannot be binary or discarded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("source number must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [name, child] : value.items()) {
            text(name); read(child, depth + 1);
        } else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    bool touches(const Json& value, const Ids& ids, std::size_t depth = 0) {
        if (depth > 64 || ++scans > scan_limit) reject("complete source-reference scan budget exceeded");
        bool result = value.is_string() && ids.contains(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [name, child] : value.items()) {
            result |= ids.contains(name); result |= touches(child, ids, depth + 1);
        } else if (value.is_array()) for (const auto& child : value) result |= touches(child, ids, depth + 1);
        return result;
    }
};
std::size_t array_count(const Json* rows, const std::string& description) {
    if (!rows || !rows->is_array() || rows->size() > row_limit)
        reject(description + " requires a bounded actual array");
    return rows->size();
}
// Reserve complete source inventories and repeated codec work before any codec.
// Bounds intentionally apply to all retained source, including opaque entities.
void bounds(const Entities& source, Budget& budget) {
    if (source.size() > entity_limit) reject("complete source entity budget exceeded");
    std::map<std::string, std::size_t, std::less<>> geometry;
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified object envelopes: " + id);
        budget.text(id); budget.text(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type == "boundary" || entity.type == "measurement_boundary" || entity.type == "room_boundary") {
            const auto rows = field(entity.properties, entity.properties.contains("segments") ? "segments" : "boundary");
            if (rows && rows->is_array()) {
                const auto count = array_count(rows, "boundary geometry " + id);
                if (count > 4096) reject("boundary analytical edge budget exceeded: " + id);
                budget.charge(count * count); geometry.emplace(id, count * count + count);
            }
        } else if (entity.type == "measurement_linework") {
            const auto model = field(entity.properties, "model");
            const auto segments = model ? field(*model, "segments") : nullptr;
            // Unrelated future models stay opaque. The complete JSON bound
            // still applies; a selected or constrained owner must pass its
            // actual reader below and cannot borrow this inspection branch.
            if (!segments || !segments->is_array()) continue;
            const auto count = array_count(segments, "measured geometry " + id);
            if (count > 4096) reject("measured analytical edge budget exceeded: " + id);
            std::size_t operations{};
            std::size_t weighted_operations{};
            for (const auto* name : {"operations", "transforms"}) if (const auto rows = model ? field(*model, name) : nullptr) {
                operations += array_count(rows, "measured derivations " + id);
                for (const auto& row : *rows) {
                    const auto edits = field(row, "edits");
                    const auto weight = edits ? array_count(edits, "measured vertex derivations " + id) + 1 : 1;
                    if (weight > row_limit - weighted_operations) reject("measured derivation work budget exceeded: " + id);
                    weighted_operations += weight;
                }
            }
            if (operations > 4096) reject("measured derivation budget exceeded: " + id);
            budget.charge(count * (weighted_operations + 1)); geometry.emplace(id, count * (weighted_operations + 1));
        } else if (entity.type == "model_phases") {
            const auto model = field(entity.properties, "model");
            const auto members = model ? field(*model, "entity_ids") : nullptr;
            const auto alternatives = model ? field(*model, "alternatives") : nullptr;
            const auto n = array_count(members, "phase inventory " + id);
            const auto a = array_count(alternatives, "phase alternatives " + id);
            if (a > 1000 || n > work_limit / (a + 1)) reject("phase evaluation budget exceeded: " + id);
            budget.charge(n * (a + 1));
        } else if (entity.type == kAnnotationEntityType) {
            const auto state = field(entity.properties, "state");
            for (const auto* name : {"labels", "symbols", "overrides"})
                (void)array_count(state ? field(*state, name) : nullptr, "annotation rows " + id);
        } else if (entity.type == "assembly_model") {
            const auto model = field(entity.properties, "model");
            (void)array_count(model ? field(*model, "instances") : nullptr, "assembly alias inventory " + id);
        }
    }
    for (const auto& [id, entity] : source) {
        if (entity.type == "constraint") {
            const auto rows = field(entity.properties, "bindings");
            (void)array_count(rows, "constraint bindings " + id);
            for (const auto& row : *rows) if (const auto owner = field(row, "owner_id"); owner && owner->is_string()) {
                const auto found = geometry.find(owner->get<std::string>());
                if (found != geometry.end()) budget.charge(8 * found->second);
            }
        } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto target = field(entity.properties, "target");
            const auto owner = target ? field(*target, "entity_id") : nullptr;
            if (owner && owner->is_string()) if (const auto found = geometry.find(owner->get<std::string>()); found != geometry.end())
                budget.charge(2 * found->second);
        }
    }
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() && left.extensions.dump() == right.extensions.dump();
}
template<class Predicate> void filter(Json& rows, Predicate keep) {
    Json survivors = Json::array();
    for (const auto& row : rows) if (keep(row)) survivors.push_back(row);
    rows = std::move(survivors);
}
void remove_ids(Json& rows, const Ids& retired) {
    filter(rows, [&](const Json& row) { return !retired.contains(row.get<std::string>()); });
}
bool area(const Entity& entity) {
    return entity.type == "boundary" || entity.type == "measurement_boundary" ||
        entity.type == "room" || entity.type == "room_boundary";
}
void validate_deductions(const Entity& entity) {
    if (!area(entity) || !entity.properties.contains("deduction_ids")) return;
    const auto& rows = entity.properties.at("deduction_ids"); Ids unique;
    if (!rows.is_array() || rows.size() > row_limit) reject("deductions require a bounded actual ID array: " + entity.id);
    for (const auto& row : rows) {
        if (!row.is_string()) reject("deduction ID must be a string: " + entity.id);
        identity(row.get<std::string>());
        if (!unique.insert(row.get<std::string>()).second) reject("duplicate deduction ID: " + entity.id);
    }
}
void boundary_admission(const Entity& entity) {
    if (entity.type != "boundary" && entity.type != "measurement_boundary")
        reject("room/physical geometry requires its own reviewed removal path: " + entity.id);
    if (entity.extensions.contains("physical_wall_room") || entity.properties.contains("physical_wall_room") ||
        entity.properties.contains("wall_measurement_source") || entity.extensions.contains("wall_measurement_source"))
        reject("physical room lineage cannot be removed as independent drawing geometry: " + entity.id);
    const auto version = inspect_boundary_entity_version(entity);
    if (version.format == BoundaryEntityFormat::unsupported_version) reject(version.diagnostic + ": " + entity.id);
    if (version.format == BoundaryEntityFormat::identified_v1) {
        (void)decode_identified_boundary_entity(entity); return;
    }
    // Admit the actual legacy geometry without generating/promoting identities.
    if (entity.properties.contains("segments") == entity.properties.contains("boundary"))
        reject("legacy boundary requires exactly one geometry array: " + entity.id);
    const auto& rows = entity.properties.at(entity.properties.contains("segments") ? "segments" : "boundary");
    Boundary geometry;
    for (const auto& row : rows) {
        if (row.contains("segment_id") || row.contains("start_vertex_id") || row.contains("end_vertex_id"))
            reject("legacy geometry contains unqualified stable identity fields: " + entity.id);
        const auto point = [&](const char* name) -> Vec2 {
            const auto& value = row.at(name);
            if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
                reject("legacy geometry requires numeric coordinate pairs: " + entity.id);
            return {value[0].get<double>(), value[1].get<double>()};
        };
        if (!row.at("sweep_radians").is_number()) reject("legacy sweep must be numeric: " + entity.id);
        geometry.push_back({point("start"), point("end"), row.at("sweep_radians").get<double>()});
    }
    const auto diagnostics = validate_boundary(geometry);
    if (geometry.empty() || !diagnostics.empty()) reject("legacy boundary geometry is invalid: " + entity.id);
}
struct Phases {
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> owners;
    Ids inactive;
};
Phases phases(const Entities& source) {
    Phases result;
    for (const auto& [id, entity] : source) if (entity.type == "model_phases") {
        auto model = ModelPhases::from_json(entity.properties.at("model"));
        const auto state = model.active_state();
        for (const auto& member : model.entity_ids()) {
            const auto found = source.find(member);
            if (found == source.end() || !is_model_phase_entity_type(found->second.type))
                reject("phase registry has missing/unsupported actual member: " + member);
            if (!result.owners.emplace(member, id).second) reject("overlapping saved phase ownership: " + member);
            const auto active = state.find(member);
            if (active == state.end() || active->second == ModelPhase::demolished) result.inactive.insert(member);
        }
        result.models.emplace(id, std::move(model));
    }
    return result;
}
void removable(const Phases& phase, const std::string& id, std::optional<std::string>& registry) {
    if (phase.inactive.contains(id)) reject("selected owner is inactive in the saved design: " + id);
    const auto owner = phase.owners.find(id);
    if (owner == phase.owners.end()) return;
    if (registry && *registry != owner->second) reject("selection spans foreign saved phase registries: " + id);
    registry = owner->second;
    const auto& model = phase.models.at(owner->second);
    if (std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id)) {
        if (model.active_alternative() || !model.alternatives().empty())
            reject("shared baseline owner requires typed phase demolition: " + id);
        return;
    }
    if (!model.active_alternative()) reject("owner is outside saved baseline: " + id);
    std::size_t memberships{};
    for (const auto& alternative : model.alternatives()) {
        if (std::binary_search(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id))
            reject("owner has protected demolition membership: " + id);
        if (!std::binary_search(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), id)) continue;
        if (alternative.id != *model.active_alternative()) reject("owner belongs to another saved alternative: " + id);
        ++memberships;
    }
    if (memberships != 1) reject("owner lacks sole active proposed ownership: " + id);
}
bool bound_overlay(const Json& row, const Ids& retired) {
    const auto object = field(row, "object_id"), binding = field(row, "dimension_binding");
    const auto bound = binding ? field(*binding, "object_id") : nullptr;
    return (object && object->is_string() && retired.contains(object->get<std::string>())) ||
        (bound && bound->is_string() && retired.contains(bound->get<std::string>()));
}
// Known measured observations are retained exactly and become stale when their
// stroke disappears. Mask only strict v1 use.owner_id slots for inspection.
void mask_lineage(Json& lineage, const Entities& actual, const Ids& strokes) {
    if (!lineage.is_array() || lineage.size() > 16384) reject("affected measured source lineage has unsupported shape");
    for (auto& edge : lineage) {
        if (!edge.is_array() || edge.empty()) reject("affected measured edge requires actual source uses");
        for (auto& use : edge) {
            if (!use.is_object() || use.size() != 5 || !use.contains("owner_id") || !use.contains("segment_id") ||
                !use.contains("parameter_start") || !use.contains("parameter_end") || !use.contains("reversed") ||
                !use.at("owner_id").is_string() || !use.at("segment_id").is_string() ||
                !use.at("parameter_start").is_number() || !use.at("parameter_end").is_number() || !use.at("reversed").is_boolean())
                reject("affected measured source use is malformed or unsupported");
            identity(use.at("owner_id").get<std::string>()); identity(use.at("segment_id").get<std::string>());
            const auto lo = use.at("parameter_start").get<double>(), hi = use.at("parameter_end").get<double>();
            if (lo < 0 || hi > 1 || !(lo < hi)) reject("affected measured source interval is invalid");
            const auto owner = use.at("owner_id").get<std::string>();
            if (strokes.contains(owner)) {
                const auto found = actual.find(owner);
                if (found == actual.end() || found->second.type != "measurement_linework")
                    reject("measured observation exemption requires an actual selected stroke: " + owner);
                use.erase("owner_id");
            }
        }
    }
}
Entity remainder(Entity entity, const Entities& actual, const Ids& strokes) {
    auto& p = entity.properties;
    if (entity.type == "constraint") {
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.supported()) reject("affected constraint requires newer reader: " + entity.id);
        p.erase("entity_ids"); p.erase("wall_ids");
        for (auto& row : p.at("bindings")) row.erase("owner_id");
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        if (!decode_boundary_dimension_entity(entity).supported()) reject("affected dimension requires newer reader: " + entity.id);
        p.at("target").erase("entity_id");
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) {
            const auto kind = row.at("target_kind").get<std::string>();
            // View IDs are a separate namespace even when their spelling
            // matches a retired owner. Mask only this codec-known field;
            // opaque metadata elsewhere remains subject to reference scans.
            if (owner_override(row) || kind=="output_view") row.erase("target_id");
        }
    } else if (entity.type == kSheetViewEntityType) {
        validate_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); auto& model = p.at("model");
        model.erase("entity_ids"); model.erase("baseline_ids");
        for (auto& alternative : model.at("alternatives")) { alternative.erase("proposed_ids"); alternative.erase("demolished_ids"); }
    } else if (entity.type == "room_relationships") {
        (void)RoomRelationshipSnapshot::from_json(p.at("model"));
        for (auto& row : p.at("model").at("references")) row.erase("id");
        for (auto& row : p.at("model").at("relations")) { row.erase("source_id"); row.erase("target_id"); }
    }
    if (area(entity) && p.contains("deduction_ids")) { validate_deductions(entity); p.erase("deduction_ids"); }
    if (!strokes.empty() && (entity.type == "boundary" || entity.type == "measurement_boundary")) {
        if (entity.extensions.contains("measurement_linework_sources")) mask_lineage(entity.extensions.at("measurement_linework_sources"), actual, strokes);
        if (entity.extensions.contains("measurement_linework_group")) {
            auto& group = entity.extensions.at("measurement_linework_group");
            if (!group.is_object() || group.size() != 2 || !group.contains("version") ||
                !group.at("version").is_number_integer() || group.at("version") != 1 || !group.contains("members") ||
                !group.at("members").is_array() || group.at("members").size() < 2 || group.at("members").size() > 2048)
                reject("affected measured area group requires a supported v1 reader");
            for (auto& member : group.at("members")) mask_lineage(member, actual, strokes);
        }
    }
    return entity;
}
bool touches(const Entity& entity, const Ids& ids, Budget& budget) {
    return budget.touches(entity.properties, ids) | budget.touches(entity.extensions, ids);
}
void validate_constraints(const Entities& source, bool active) {
    const auto unsupported = active ? validate_active_phase_constraint_integrity(source) : validate_constraint_integrity(source);
    if (unsupported) reject("constraint admission is read-only: " + *unsupported);
}
struct Authority {
    Ids roots, retired, strokes;
    Children children;
    OverlayChildren overlays;
    Phases phase;
};
Authority authority(const Entities& actual, const DrawingSelectionRemovalIntent& intent, Budget& budget) {
    Authority result; result.phase = phases(actual);
    std::optional<std::string> registry;
    for (const auto& id : intent.owner_ids) {
        const auto found = actual.find(id);
        if (found == actual.end()) reject("selected actual owner is missing: " + id);
        const auto& entity = found->second;
        // Existing ordinary removal treats required supported measured strokes
        // as authored geometry: the flag requires reader support, rather than
        // protecting their lifetime. Other required roots remain protected.
        if (entity.required && entity.type != "measurement_linework") reject("selected actual owner is required: " + id);
        if (entity.type == "boundary" || entity.type == "measurement_boundary") boundary_admission(entity);
        else if (entity.type == "measurement_linework") {
            if (entity.extensions.contains("physical_wall_room") || entity.properties.contains("physical_wall_room") ||
                entity.properties.contains("wall_measurement_source") || entity.extensions.contains("wall_measurement_source"))
                reject("measurement stroke carries physical room lineage: " + id);
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (!decoded.supported()) reject(decoded.diagnostic + ": " + id);
            if (decoded.model->stroke_id != id) reject("actual measured owner differs from its stroke identity: " + id);
            result.strokes.insert(id);
        } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) reject(decoded.unsupported_reason + ": " + id);
        } else reject("selected owner requires a dedicated removal path: " + id);
        removable(result.phase, id, registry); result.roots.insert(id); result.retired.insert(id);
    }
    std::map<std::string, std::set<std::pair<std::string, std::string>>, std::less<>> annotation_rows;
    for (const auto& [id, entity] : actual) if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        auto& rows = annotation_rows[id];
        for (const auto* collection : {"labels", "symbols"}) for (const auto& row : entity.properties.at("state").at(collection)) {
            const auto child = row.at("id").get<std::string>();
            if (result.roots.contains(child)) reject("selected owner collides with an annotation child identity");
            rows.emplace(std::string_view(collection) == "labels" ? "label" : "symbol", child);
        }
    }
    for (const auto& target : intent.annotations) {
        const auto found = actual.find(target.owner_id);
        if (found == actual.end() || found->second.type != kAnnotationEntityType)
            reject("qualified annotation lacks actual annotation owner: " + target.owner_id);
        if (actual.contains(target.child_id)) reject("annotation child collides with an actual owner ID: " + target.child_id);
        removable(result.phase, target.owner_id, registry);
        if (!annotation_rows.at(target.owner_id).contains({kind_name(target.kind), target.child_id}))
            reject("actual qualified annotation row/kind is missing: " + target.owner_id + "/" + target.child_id);
        if (!result.children[target.owner_id].insert(target.child_id).second)
            reject("annotation row selected twice across kinds: " + target.owner_id + "/" + target.child_id);
    }
    // Dependence on physical room/wall lineage cannot be disguised as a
    // measurement owner. Only explicitly independent relations can retire.
    for (const auto& [id, entity] : actual) if (entity.type == "room_relationships" && touches(entity, result.roots, budget)) {
        const auto model = RoomRelationshipSnapshot::from_json(entity.properties.at("model"));
        for (const auto& ref : model.references()) if (result.roots.contains(ref.id) && ref.kind != RoomReferenceKind::appraisal_measurement_boundary)
            reject("selected measurement root has physical/room relationship role: " + ref.id);
        for (const auto& relation : model.relations()) if ((result.roots.contains(relation.source_id) || result.roots.contains(relation.target_id)) &&
            relation.kind != RoomRelationKind::independent) reject("dependent room relationship requires explicit review: " + id);
    }
    const auto constraint_scope=constraint_phase_scope(actual);
    for (const auto& [id, entity] : actual) {
        if (result.retired.contains(id)) continue;
        if (can_recognize_boundary_dimension_entity_type(entity.type) && touches(entity, result.roots, budget)) {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) reject("affected dimension requires newer reader: " + id);
            if (result.roots.contains(decoded.dimension->boundary_id)) {
                if (entity.required) reject("dependent dimension is required: " + id);
                result.retired.insert(id);
            }
        } else if (entity.type == "constraint" && touches(entity, result.roots, budget)) {
            const auto decoded = decode_constraint_entity(entity);
            if (!decoded.supported()) reject("affected constraint requires newer reader: " + id);
            if (std::any_of(decoded.constraint->bindings.begin(), decoded.constraint->bindings.end(),
                [&](const auto& binding) { return result.roots.contains(binding.owner_id); })) {
                if (entity.required) reject("dependent constraint is required: " + id);
                if (!constraint_participates(*decoded.constraint,constraint_scope))
                    reject("dependent constraint belongs to an inactive design and needs explicit relationship review: " + id);
                result.retired.insert(id);
            }
        }
    }
    if (result.retired.size() > row_limit) reject("dependent retirement closure budget exceeded");
    return result;
}
void collect_overlays(const Entities& source, Authority& selected, Budget& budget) {
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType && touches(entity, selected.retired, budget)) {
        validate_sheet_view_entity(entity);
        for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
            for (const auto& row : view.at("overlays")) if (bound_overlay(row, selected.retired))
                selected.overlays[{id, view.at("id").get<std::string>()}].insert(row.at("id").get<std::string>());
    }
}
// No wire codec presently retires an external reference to an annotation or
// overlay child. Detect qualified references without granting local spellings
// global owner authority; those references require a separate explicit review.
void local_references(const Json& value, const std::string& current_owner,
    const Authority& selected, Budget& budget, std::size_t depth = 0) {
    if (depth > 64 || ++budget.scans > scan_limit) reject("qualified child-reference scan budget exceeded");
    if (value.is_object()) {
        std::string owner = current_owner;
        bool explicit_owner = false;
        for (const auto* name : {"annotation_entity_id", "annotation_owner_id", "owner_id", "entity_id"}) {
            const auto declared = field(value, name);
            if (declared && declared->is_string()) { owner = declared->get<std::string>(); explicit_owner = true; break; }
        }
        const auto children = selected.children.find(owner);
        if (children != selected.children.end() && explicit_owner && budget.touches(value, children->second))
            reject("opaque qualified annotation reference requires explicit review: " + owner);
        if (children != selected.children.end()) for (const auto* name : {"child_id", "annotation_id", "label_id", "symbol_id", "target_id"}) {
            const auto child = field(value, name);
            if (child && child->is_string() && children->second.contains(child->get<std::string>()))
                reject("qualified reference to selected annotation requires explicit review: " + owner);
        }
        const auto view = field(value, "view_id"), overlay = field(value, "overlay_id");
        if (view && view->is_string() && overlay && overlay->is_string()) {
            std::string sheet_owner = current_owner;
            for (const auto* name : {"sheet_view_entity_id", "sheet_view_id", "entity_id"}) {
                const auto declared = field(value, name);
                if (declared && declared->is_string()) { sheet_owner = declared->get<std::string>(); break; }
            }
            const auto rows = selected.overlays.find({sheet_owner, view->get<std::string>()});
            if (rows != selected.overlays.end() && rows->second.contains(overlay->get<std::string>()))
                reject("qualified reference to retired view overlay requires explicit review: " + sheet_owner);
        }
        for (const auto& child : value) local_references(child, owner, selected, budget, depth + 1);
    } else if (value.is_array()) for (const auto& child : value) local_references(child, current_owner, selected, budget, depth + 1);
}
void local_reference_admission(const Entities& source, const Authority& selected, Budget& budget) {
    if (selected.children.empty() && selected.overlays.empty()) return;
    for (const auto& [id, original] : source) {
        if (selected.retired.contains(id)) continue;
        auto entity = original;
        if (const auto children = selected.children.find(id); children != selected.children.end()) {
            for (const auto* name : {"labels", "symbols"}) filter(entity.properties.at("state").at(name),
                [&](const Json& row) { return !children->second.contains(row.at("id").get<std::string>()); });
            filter(entity.properties.at("state").at("overrides"),
                [&](const Json& row) { return !child_override(row,children->second); });
        }
        if (entity.type==kAnnotationEntityType) {
            validate_annotation_entity(entity);
            // These understood targets have owner/view namespaces, not an
            // implicit annotation-child binding. Unknown qualified references
            // in their other fields still require explicit review.
            for (auto& row:entity.properties.at("state").at("overrides"))
                if (owner_override(row) || row.at("target_kind")=="output_view") row.erase("target_id");
        }
        if (entity.type == kSheetViewEntityType && !selected.overlays.empty()) {
            for (auto& view : entity.properties.at("model").at("views")) {
                const auto children = selected.overlays.find({id, view.at("id").get<std::string>()});
                if (children == selected.overlays.end()) continue;
                if (view.contains("overlays")) filter(view.at("overlays"),
                    [&](const Json& row) { return !children->second.contains(row.at("id").get<std::string>()); });
                auto scratch = view; scratch.erase("id"); scratch.erase("object_ids");
                auto& presentation = scratch.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
                if (scratch.contains("overlays")) for (auto& row : scratch.at("overlays")) {
                    row.erase("id"); row.erase("object_id");
                    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
                }
                if (budget.touches(scratch, children->second))
                    reject("surviving view data references a retired local overlay: " + id);
            }
        }
        local_references(entity.properties, id, selected, budget);
        local_references(entity.extensions, id, selected, budget);
    }
}
// Review may already omit a selected row, but must preserve every surviving
// row and carrier field exactly. Ordinary known override retirement is allowed
// only for an actual owner already retired by that independently admitted stage.
void selected_stage(const Entities& actual, const Entities& stage, const Authority& selected,
    const Entities* reconstructed_architectural_stage) {
    const auto stage_phase = phases(stage);
    for (const auto& id : selected.roots) {
        const auto before = selected.phase.owners.find(id), after = stage_phase.owners.find(id);
        if ((before == selected.phase.owners.end()) != (after == stage_phase.owners.end()) ||
            (before != selected.phase.owners.end() && before->second != after->second))
            reject("review stage changed selected drawing phase ownership: " + id);
        if (before == selected.phase.owners.end()) continue;
        const auto& original = selected.phase.models.at(before->second);
        const auto& retained = stage_phase.models.at(after->second);
        const auto original_state = original.active_state(), retained_state = retained.active_state();
        const auto original_member = original_state.find(id), retained_member = retained_state.find(id);
        if (original.active_alternative() != retained.active_alternative() ||
            original_member == original_state.end() || retained_member == retained_state.end() ||
            original_member->second != retained_member->second)
            reject("review stage changed selected drawing saved phase state: " + id);
    }
    for (const auto& id : selected.roots) {
        const auto after = stage.find(id);
        if (after == stage.end()) {
            if (!can_recognize_boundary_dimension_entity_type(actual.at(id).type))
                reject("review stage already erased independent selected owner: " + id);
        } else if (!exact(actual.at(id), after->second)) reject("review stage changed selected actual owner: " + id);
    }
    for (const auto& [owner, children] : selected.children) {
        const auto after = stage.find(owner);
        if (after == stage.end()) reject("review stage erased selected annotation carrier: " + owner);
        validate_annotation_entity(after->second);
        auto before = actual.at(owner), retained = after->second;
        for (const auto* collection : {"labels", "symbols"}) {
            auto& original_rows = before.properties.at("state").at(collection);
            const auto& staged_rows = retained.properties.at("state").at(collection);
            Ids staged_ids;
            for (const auto& row : staged_rows) staged_ids.insert(row.at("id").get<std::string>());
            filter(original_rows, [&](const Json& row) {
                if (!children.contains(row.at("id").get<std::string>())) return true;
                return staged_ids.contains(row.at("id").get<std::string>());
            });
        }
        auto& overrides = before.properties.at("state").at("overrides");
        const auto& staged_overrides = retained.properties.at("state").at("overrides");
        // This permission exists only inside the architectural producer below,
        // after it reconstructs this exact stage from the immutable actual map.
        // A computed component alias is not an actual entity key; roof-join
        // splitting may also copy overrides. Admit those exact consequences,
        // while the final comparison still protects every other carrier field
        // and all unselected local rows.
        if (reconstructed_architectural_stage) {
            const auto architectural = reconstructed_architectural_stage->find(owner);
            if (architectural == reconstructed_architectural_stage->end() ||
                !exact(architectural->second, retained))
                reject("architectural annotation stage lost its source provenance: " + owner);
            overrides = architectural->second.properties.at("state").at("overrides");
        }
        std::set<std::pair<std::string, std::string>> staged_targets;
        for (const auto& row : staged_overrides)
            staged_targets.emplace(row.at("target_kind").get<std::string>(), row.at("target_id").get<std::string>());
        filter(overrides, [&](const Json& row) {
            const auto target = row.at("target_id").get<std::string>();
            const bool admitted = child_override(row,children) ||
                (owner_override(row) && actual.contains(target) && !stage.contains(target));
            if (!admitted) return true;
            return staged_targets.contains({row.at("target_kind").get<std::string>(), target});
        });
        if (!exact(before, retained)) reject("review stage changed surviving selected annotation namespace: " + owner);
    }
}
void cleanup(Entity& entity, const Authority& selected) {
    auto& p = entity.properties;
    if (area(entity) && p.contains("deduction_ids")) {
        validate_deductions(entity);
        const auto& rows = p.at("deduction_ids");
        const auto size = rows.size(); remove_ids(p.at("deduction_ids"), selected.retired);
        if (size != p.at("deduction_ids").size() && p.at("deduction_ids").empty()) p.erase("deduction_ids");
    }
    if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        const auto children = selected.children.find(entity.id);
        if (children != selected.children.end()) for (const auto* name : {"labels", "symbols"})
            filter(p.at("state").at(name), [&](const Json& row) { return !children->second.contains(row.at("id").get<std::string>()); });
        filter(p.at("state").at("overrides"), [&](const Json& row) {
            const auto target = row.at("target_id").get<std::string>();
            return !(owner_override(row) && selected.retired.contains(target)) &&
                (children == selected.children.end() || !child_override(row,children->second));
        });
        validate_annotation_entity(entity);
    } else if (entity.type == kSheetViewEntityType) {
        validate_sheet_view_entity(entity);
        // An older source list becomes unrestricted when its final ID is
        // removed. Promote only that previously refused case, preserving every
        // surviving raw collection and the existing reader's legacy meaning.
        const auto& model = p.at("model");
        if (model.at("version").get<unsigned>() < 6 &&
            std::any_of(model.at("views").begin(), model.at("views").end(), [&](const Json& view) {
                const auto ids = field(view, "object_ids");
                return ids && !ids->empty() && std::all_of(ids->begin(), ids->end(), [&](const Json& id) {
                    return selected.retired.contains(id.get<std::string>());
                });
            })) entity = upgrade_sheet_view_entity_for_empty_restriction(entity);
        for (auto& view : p.at("model").at("views")) {
            if (view.contains("object_ids")) {
                auto& ids = view.at("object_ids"); const bool restricted = !ids.empty() || view.value("restrict_to_objects", false);
                const auto size = ids.size(); remove_ids(ids, selected.retired);
                if (size != ids.size() && restricted && ids.empty()) {
                    view["restrict_to_objects"] = true;
                }
            }
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                filter(presentation.at("appearance").at("objects"), [&](const Json& row) { return !selected.retired.contains(row.at("object_id").get<std::string>()); });
            if (view.contains("overlays")) filter(view.at("overlays"), [&](const Json& row) { return !bound_overlay(row, selected.retired); });
        }
        validate_sheet_view_entity(entity);
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); auto& model = p.at("model");
        remove_ids(model.at("entity_ids"), selected.retired); remove_ids(model.at("baseline_ids"), selected.retired);
        for (auto& alternative : model.at("alternatives")) {
            remove_ids(alternative.at("proposed_ids"), selected.retired);
            remove_ids(alternative.at("demolished_ids"), selected.retired);
        }
        (void)ModelPhases::from_json(model);
    } else if (entity.type == "room_relationships") {
        (void)RoomRelationshipSnapshot::from_json(p.at("model"));
        filter(p.at("model").at("references"), [&](const Json& row) { return !selected.retired.contains(row.at("id").get<std::string>()); });
        filter(p.at("model").at("relations"), [&](const Json& row) {
            return !selected.retired.contains(row.at("source_id").get<std::string>()) && !selected.retired.contains(row.at("target_id").get<std::string>());
        });
        (void)RoomRelationshipSnapshot::from_json(p.at("model"));
    }
}
Entities replay(const Entities& actual, const Entities& stage, const DrawingSelectionRemovalIntent& intent, bool active,
    const Entities* reconstructed_architectural_stage = nullptr) {
    intent_bounds(intent); Budget budget; bounds(actual, budget);
    if (&actual != &stage) bounds(stage, budget);
    validate_constraints(actual, active);
    auto selected = authority(actual, intent, budget);
    collect_overlays(actual, selected, budget);
    if (&actual != &stage) collect_overlays(stage, selected, budget);
    local_reference_admission(actual, selected, budget);
    // Inspect the full original source even when the admitted review stage
    // already retired the same consequence. It cannot hide unknown references.
    for (const auto& [id, entity] : actual) if (!selected.roots.contains(id) && touches(entity, selected.retired, budget)) {
        const auto scratch = remainder(entity, actual, selected.strokes);
        if (touches(scratch, selected.retired, budget)) reject("actual opaque reference requires a qualified removal codec: " + id);
    }
    selected_stage(actual, stage, selected, reconstructed_architectural_stage);
    const auto original_aliases = embedded_assembly_presentation_ids(stage);
    Entities result = stage;
    for (const auto& id : selected.retired) {
        const auto after = result.find(id);
        if (after != result.end()) {
            if (!exact(actual.at(id), after->second)) reject("review stage changed dependent retirement owner: " + id);
            result.erase(after);
        }
    }
    for (auto& [id, entity] : result) {
        (void)id;
        const bool affected = touches(entity, selected.retired, budget) || selected.children.contains(entity.id);
        if (!affected) continue;
        // Unknown source fields cannot be hidden by dropping their containing
        // known row. Check the original affected envelope before filtering.
        auto scratch = remainder(entity, actual, selected.strokes);
        if (touches(scratch, selected.retired, budget)) reject("affected opaque reference requires a qualified removal codec: " + entity.id);
        cleanup(entity, selected);
    }
    for (const auto& [owner, children] : selected.children) {
        auto scratch = result.at(owner);
        // Actual survivor declarations are local, not dangling references.
        for (const auto* name : {"labels", "symbols"}) for (auto& row : scratch.properties.at("state").at(name)) row.erase("id");
        for (auto& row:scratch.properties.at("state").at("overrides"))
            if (row.at("target_kind")!="object" &&
                (owner_override(row) || row.at("target_kind")=="output_view")) row.erase("target_id");
        if (touches(scratch, children, budget)) reject("surviving annotation data references a removed local row: " + owner);
    }
    for (const auto& [id, entity] : result) if (touches(entity, selected.retired, budget)) {
        auto scratch = remainder(entity, actual, selected.strokes);
        if (touches(scratch, selected.retired, budget)) reject("surviving reference requires explicit review: " + id);
    }
    local_reference_admission(result, selected, budget);
    Budget final_budget; bounds(result, final_budget);
    if (embedded_assembly_presentation_ids(result) != original_aliases)
        reject("drawing removal would change a surviving computed component alias");
    const auto stage_phase = phases(stage), final_phase = phases(result);
    if (stage_phase.inactive != final_phase.inactive) reject("removal changed protected inactive/foreign saved phase ownership");
    validate_constraints(result, active);
    return result;
}
} // namespace

nlohmann::json encode_drawing_selection_removal_intent(const DrawingSelectionRemovalIntent& intent) {
    intent_bounds(intent);
    Json annotations = Json::array();
    for (const auto& target : intent.annotations)
        annotations.push_back({{"owner_id", target.owner_id}, {"kind", kind_name(target.kind)}, {"child_id", target.child_id}});
    return {{"version", 1}, {"owner_ids", intent.owner_ids}, {"annotations", std::move(annotations)}};
}
DrawingSelectionRemovalIntent decode_drawing_selection_removal_intent(const nlohmann::json& value) {
    try {
        Budget budget; budget.read(value);
        if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("owner_ids") || !value.at("owner_ids").is_array() ||
            !value.contains("annotations") || !value.at("annotations").is_array()) reject("intent requires exact v1 fields");
        if (value.at("owner_ids").size() > target_limit || value.at("annotations").size() > target_limit - value.at("owner_ids").size())
            reject("intent aggregate target budget exceeded");
        DrawingSelectionRemovalIntent result;
        for (const auto& id : value.at("owner_ids")) {
            if (!id.is_string()) reject("intent owner ID must be a string");
            result.owner_ids.push_back(id.get<std::string>());
        }
        for (const auto& row : value.at("annotations")) {
            if (!row.is_object() || row.size() != 3 || !row.contains("owner_id") || !row.at("owner_id").is_string() ||
                !row.contains("kind") || !row.at("kind").is_string() || !row.contains("child_id") || !row.at("child_id").is_string())
                reject("annotation target requires exact owner_id/kind/child_id fields");
            const auto kind = row.at("kind").get<std::string>();
            if (kind != "label" && kind != "symbol") reject("annotation target kind must be label or symbol");
            result.annotations.push_back({row.at("owner_id").get<std::string>(),
                kind == "label" ? DrawingSelectionAnnotationKind::label : DrawingSelectionAnnotationKind::symbol, row.at("child_id").get<std::string>()});
        }
        intent_bounds(result); return result;
    } catch (const Json::exception& error) { reject(std::string("malformed intent: ") + error.what()); }
}
std::string drawing_selection_removal_intent_bytes(const DrawingSelectionRemovalIntent& intent) {
    try {
        auto bytes = encode_drawing_selection_removal_intent(intent).dump();
        if (bytes.size() > intent_byte_limit) reject("canonical intent byte budget exceeded");
        return bytes;
    } catch (const Json::exception& error) { reject(std::string("malformed intent serialization: ") + error.what()); }
}
DrawingSelectionRemovalIntent decode_drawing_selection_removal_intent_bytes(std::string_view bytes) {
    if (bytes.size() > intent_byte_limit) reject("intent byte budget exceeded");
    try {
        // Reject excessive nesting during parse, before building a JSON tree.
        const auto value = Json::parse(bytes.begin(), bytes.end(), [](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) reject("intent parsing nesting budget exceeded");
            return true;
        });
        auto result = decode_drawing_selection_removal_intent(value);
        if (drawing_selection_removal_intent_bytes(result) != bytes) reject("intent bytes must be canonical exact v1 JSON");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed intent bytes: ") + error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal(const DrawingSelectionRemovalEntities& actual,
    const DrawingSelectionRemovalIntent& intent, bool active_phase_constraints) {
    try { return replay(actual, actual, intent, active_phase_constraints); }
    catch (const Json::exception& error) { reject(std::string("malformed actual source: ") + error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_after_review(const DrawingSelectionRemovalEntities& actual,
    const DrawingSelectionRemovalEntities& admitted_review_stage, const DrawingSelectionRemovalIntent& intent, bool active_phase_constraints) {
    try { return replay(actual, admitted_review_stage, intent, active_phase_constraints); }
    catch (const Json::exception& error) { reject(std::string("malformed actual/review source: ") + error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_architectural(
    const DrawingSelectionRemovalEntities& actual, const DrawingSelectionRemovalIntent& drawing,
    const ArchitecturalSelectionRemovalIntent& architectural,
    bool allow_manufactured_opening_hosts, bool active_phase_constraints) {
    try {
        // Analytical actual-source admission runs before any native factory.
        // No caller-supplied candidate can grant this narrower composition path.
        (void)replay(actual, actual, drawing, active_phase_constraints);
        const auto stage = replay_architectural_selection_removal(
            actual, architectural, allow_manufactured_opening_hosts);
        return replay(actual, stage, drawing, active_phase_constraints, &stage);
    } catch (const Json::exception& error) {
        reject(std::string("malformed actual/architectural source: ") + error.what());
    }
}
namespace {
Entities deletion_geometry_stage(const Entities& actual,const Json& proof,bool active) {
    Budget budget;budget.read(proof);
    if (budget.wire_upper_bound>intent_byte_limit || !proof.is_object() ||
        !proof.contains("kind") || !proof.at("kind").is_string())
        reject("deletion geometry requires a bounded explicit typed proof");
    const auto kind=proof.at("kind").get<std::string>();
    const auto command=[&]()->Command {
        if (kind=="physical_wall_deletion") return decode_physical_wall_deletion_review_proof(proof);
        if (kind=="mixed_wall_deletion") return Command{decode_mixed_wall_deletion_review_proof(proof).command};
        if (kind=="mixed_wall_opening_deletion") return Command{decode_mixed_wall_opening_deletion_review_proof(proof).command};
        if (kind=="apply_entity_changes" && proof.at("version")==1) {
            const auto decoded=command_from_json(proof);
            if (command_to_json(decoded).dump()!=proof.dump()) reject("single-wall deletion proof is not canonical");
            return decoded;
        }
        reject("unsupported deletion geometry authority");
    }();
    const auto* raw=std::get_if<ApplyEntityChanges>(&command);
    if (!raw || !raw->asset_changes.empty()) reject("deletion geometry must retain an asset-free raw child");
    auto stage=actual;
    for (const auto& change:raw->entity_changes) {
        if (change.kind==EntityChangeKind::erase) stage.erase(change.entity_id);
        else stage.insert_or_assign(change.entity.id,change.entity);
    }
    // This independently rebuilds every attached deletion consequence from the
    // immutable actual map; raw single-wall proofs cannot carry arbitrary edits.
    validate_physical_wall_room_deletion_review_source(actual,stage,command,proof,active);
    return stage;
}
} // namespace
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_openings(
    const DrawingSelectionRemovalEntities& actual,const DrawingSelectionRemovalIntent& drawing,
    const OpeningArchitecturalRemovalIntent& openings,bool active_phase_constraints) {
    try {
        (void)replay(actual,actual,drawing,active_phase_constraints);
        const auto stage=[&] {
            if (!openings.other.object_ids.empty() || !openings.other.components.empty())
                return replay_opening_architectural_removal(actual,openings,active_phase_constraints);
            if (!openings.other.roof_additional_identities.empty()) reject("opening-only removal cannot allocate architectural destinations");
            const auto candidate=replay_hosted_opening_removal(actual,openings.opening_ids,active_phase_constraints,true);
            if (!candidate) reject("opening removal requires actual selected semantic openings");
            return *candidate;
        }();
        return replay(actual,stage,drawing,active_phase_constraints,&stage);
    } catch (const Json::exception& error) { reject(std::string("malformed actual/opening source: ")+error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_deletion_geometry(
    const DrawingSelectionRemovalEntities& actual,const DrawingSelectionRemovalIntent& drawing,
    const nlohmann::json& geometry_proof,bool active_phase_constraints) {
    try {
        (void)replay(actual,actual,drawing,active_phase_constraints);
        const auto stage=deletion_geometry_stage(actual,geometry_proof,active_phase_constraints);
        return replay(actual,stage,drawing,active_phase_constraints,&stage);
    } catch (const Json::exception& error) { reject(std::string("malformed actual/deletion source: ")+error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_deletion_review(
    const DocumentSnapshot& source,const DrawingSelectionRemovalIntent& drawing,
    const nlohmann::json& geometry_proof,const Command& pure_room_review_command) {
    try {
        const auto active=source.uses_active_phase_constraints();
        (void)replay(source.entities(),source.entities(),drawing,active);
        (void)deletion_geometry_stage(source.entities(),geometry_proof,active);
        const auto wire=command_to_json(pure_room_review_command);
        const auto version=wire.at("version").get<int>();
        const auto* review=std::get_if<ApplyBoundaryConstraintChanges>(&pure_room_review_command);
        if (!review || !review->room_review_completion || !review->room_review_geometry_completion ||
            review->room_review_geometry_proof.dump()!=geometry_proof.dump() || review->expected_revision!=source.revision() ||
            (version!=27 && version!=30 && version!=31 && version!=35 && version!=36 && version!=37 &&
                version!=38 && version!=39 && version!=40 && version!=48 && version!=49 && version!=50))
            reject("drawing composition requires the exact closed deletion room review");
        const auto canonical=command_from_json(wire);
        if (command_to_json(canonical).dump()!=wire.dump()) reject("deletion room review is not canonical");
        // The complete typed review is independently admitted from the actual
        // snapshot. Its accepted choices, not a supplied stage, own overrides.
        const auto stage=Document::preview_command(source,canonical);
        return replay(source.entities(),stage.entities(),drawing,active,&stage.entities());
    } catch (const Json::exception& error) { reject(std::string("malformed actual/deletion review source: ")+error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_phase(
    const DocumentSnapshot& source,const DrawingSelectionRemovalIntent& drawing,
    const ArchitecturalSelectionRemovalIntent& architectural,const Command& pure_phase_deletion_command) {
    try {
        // Demolition and its accepted room choices activate the same phase
        // policy as the original command, even if the captured snapshot has
        // not yet published phase-aware authoring history.
        (void)replay(source.entities(),source.entities(),drawing,true);
        (void)phase_selection_removal_base_authority(source,pure_phase_deletion_command,architectural);
        const auto wire=command_to_json(pure_phase_deletion_command);
        const auto canonical=command_from_json(wire);
        if (command_to_json(canonical).dump()!=wire.dump()) reject("phase deletion is not canonical");
        const auto stage=Document::preview_command(source,canonical);
        auto result=replay(source.entities(),stage.entities(),drawing,true,&stage.entities());
        Document::validate_phase_drawing_removal_dependents(source.entities(),stage.entities(),result);
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed actual/phase deletion source: ")+error.what()); }
}
DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_corners(
    const DrawingSelectionRemovalEntities& actual, const DrawingSelectionRemovalIntent& drawing,
    const CornerSelectionRemovalIntent& corners, bool active_phase_constraints) {
    try {
        (void)replay(actual,actual,drawing,active_phase_constraints);
        const auto stage=replay_corner_selection_removal_architectural(actual,corners,active_phase_constraints);
        auto result=replay(actual,stage,drawing,active_phase_constraints,&stage);
        Document::validate_phase_drawing_removal_dependents(actual,stage,result);
        return result;
    } catch (const Json::exception& error) {
        reject(std::string("malformed actual/corner deletion source: ")+error.what());
    }
}
} // namespace sketch
