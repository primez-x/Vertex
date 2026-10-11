#include "sketch/phase_assembly_catalog_edit_presentation.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = AssemblyDocumentEntities;
using Ids = std::set<std::string, std::less<>>;
using IdMap = std::map<std::string, std::string, std::less<>>;
using Gap = PhaseAssemblyCatalogPresentationReferenceGap;
using Target = AssemblyInstancePropertyEditTarget;
using Dimensions = std::map<std::string, BoundaryDimension, std::less<>>;
constexpr std::size_t identity_limit = 4096;
constexpr std::size_t dimension_limit = 1000;

[[noreturn]] void reject(std::string message) {
    throw std::invalid_argument("Phase assembly catalog presentation: " + std::move(message));
}
void require(bool condition, const char* message) { if (!condition) reject(message); }
void fresh_id(std::string_view text) {
    require(!text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid fresh bounded document identity");
}
void overlay_id(std::string_view text) {
    require(!text.empty() && text.size() <= 128 && text.find('\0') == std::string_view::npos &&
        text.find_first_not_of(" \t\r\n") != std::string_view::npos, "invalid original overlay identity");
}
struct Budget {
    std::size_t bytes{}, nodes{};
    std::size_t max_bytes{64 * 1024 * 1024}, max_nodes{4 * 1024 * 1024};
    void charge(std::size_t count) {
        require(count <= max_bytes - bytes, "complete raw byte budget exceeded"); bytes += count;
    }
    void text(std::string_view value) {
        require(value.size() <= (max_bytes - bytes) / 6, "complete raw string budget exceeded"); charge(value.size() * 6);
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64 && ++nodes <= max_nodes, "complete raw node/depth budget exceeded"); charge(32);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())), "nonportable source/allocation JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
    }
    void map(const Entities& entities) {
        require(entities.size() <= 100000, "complete source/candidate entity count exceeds bound");
        for (const auto& [id, entity] : entities) {
            fresh_id(id);
            require(entity.id == id && entity.properties.is_object() && entity.extensions.is_object(), "invalid actual entity envelope");
            text(id); text(entity.type); read(entity.properties); read(entity.extensions);
        }
    }
};
void admit(const Entities& source, const Json& request) {
    Budget{}.map(source);
    Budget wire{0, 0, 16 * 1024 * 1024, 262144}; wire.read(request);
    AssemblyCatalogTransferBudget catalogs;
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalogs);
    }
}
void closed(const Json& value, std::initializer_list<const char*> fields) {
    require(value.is_object() && value.size() == fields.size(), "unsupported allocation fields");
    for (const auto* field : fields) require(value.contains(field), "missing allocation field");
}
void admit_allocation(const PhaseAssemblyCatalogEditPresentationAllocation& allocation) {
    require(allocation.overlay_ids.size() <= identity_limit &&
        allocation.dimension_ids.size() <= identity_limit - allocation.overlay_ids.size(),
        "complete presentation allocation count exceeds bound");
    Ids allocated;
    for (const auto& [old_id, new_id] : allocation.overlay_ids) {
        overlay_id(old_id); fresh_id(new_id);
        require(allocated.insert(new_id).second, "fresh overlay allocation is not injective");
    }
    for (const auto& [old_id, new_id] : allocation.dimension_ids) {
        fresh_id(old_id); fresh_id(new_id);
        require(allocated.insert(new_id).second, "fresh dimension allocation is not injective across presentation allocations");
    }
}
void collect_local_ids(const Json& value, Ids& ids) {
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (key == "id" && child.is_string()) ids.insert(child.get_ref<const std::string&>());
        collect_local_ids(child, ids);
    } else if (value.is_array()) for (const auto& child : value) collect_local_ids(child, ids);
}
Ids reserved_ids(const Entities& entities, const EmbeddedAssemblyPresentationIds& aliases) {
    Ids ids;
    for (const auto& [id, entity] : entities) {
        ids.insert(id); collect_local_ids(entity.properties, ids); collect_local_ids(entity.extensions, ids);
    }
    for (const auto& [root, alias] : aliases) { (void)root; ids.insert(alias); }
    return ids;
}
void validate_aliases(const Entities& entities, const EmbeddedAssemblyPresentationIds& aliases) {
    Ids occupied, seen;
    for (const auto& [id, entity] : entities) {
        occupied.insert(id);
        if (entity.type == kAnnotationEntityType) {
            validate_annotation_entity(entity);
            for (const auto* collection : {"labels", "symbols"})
                for (const auto& row : entity.properties.at("state").at(collection)) occupied.insert(row.at("id").get<std::string>());
        } else if (entity.type == kSheetViewEntityType) {
            validate_sheet_view_entity(entity);
            for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
                for (const auto& row : view.at("overlays")) occupied.insert(row.at("id").get<std::string>());
        }
    }
    for (const auto& [root, alias] : aliases) {
        (void)root;
        require(seen.insert(alias).second && !occupied.contains(alias), "actual embedded presentation alias is not unique");
    }
}
struct Owners {
    Ids ordinary, baseline;
};
Owners owners(const PhaseAssemblyCatalogEditPlan& plan, const EmbeddedAssemblyPresentationIds& aliases) {
    Owners result;
    for (const auto& row : plan.isolation_roots) {
        // An ordinary independent root keeps its persisted owner identity.
        if (row.root.document_entity_id && !row.baseline_replacement) continue;
        const auto id = row.root.document_entity_id ? *row.root.document_entity_id : aliases.at({row.root.catalog_id, row.root.instance_id});
        require(!result.ordinary.contains(id) && !result.baseline.contains(id), "actual affected presentation owners overlap");
        (row.baseline_replacement ? result.baseline : result.ordinary).insert(id);
    }
    return result;
}
bool affected_overlay(const Json& row, const Ids& ids) {
    return ids.contains(row.at("object_id").get<std::string>()) ||
        (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
            ids.contains(row.at("dimension_binding").at("object_id").get<std::string>()));
}
void add_gap(std::vector<Gap>& gaps, const std::string& entity, std::string field,
    std::string target, std::string reason) {
    require(gaps.size() < identity_limit && field.size() <= 8192 && target.size() <= 16384,
        "unsupported reference inventory exceeds bound");
    gaps.push_back({entity, std::move(field), std::move(target), std::move(reason)});
}
std::string escaped(std::string_view value) {
    std::string result;
    for (const auto c : value) { if (c == '~') result += "~0"; else if (c == '/') result += "~1"; else result += c; }
    return result;
}
void opaque_gaps(const Json& value, const Ids& affected, const std::string& entity,
    const std::string& path, std::vector<Gap>& gaps) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (affected.contains(text)) add_gap(gaps, entity, path, text, "opaque reference requires an owning typed translation");
    } else if (value.is_object()) for (const auto& [key, child] : value.items()) {
        require(path.size() + key.size() * 2 + 1 <= 8192, "opaque reference path exceeds bound");
        const auto child_path = path + "/" + escaped(key);
        if (affected.contains(key)) add_gap(gaps, entity, child_path, key, "opaque reference key requires an owning typed translation");
        opaque_gaps(child, affected, entity, child_path, gaps);
    } else if (value.is_array()) for (std::size_t index = 0; index < value.size(); ++index)
        opaque_gaps(value.at(index), affected, entity, path + "/" + std::to_string(index), gaps);
}
Target root_target(Target target) { target.part_path.clear(); return target; }
Dimensions inspect_dimensions(const Entities& source, const PhaseAssemblyCatalogEditPlan& producer) {
    std::set<Target> roots;
    for (const auto& impact : producer.source.roots)
        require(roots.insert(impact.root).second, "actual source root inventory overlaps");
    Dimensions dimensions;
    std::vector<AssemblyDimensionTarget> targets;
    for (const auto& [id, entity] : source) if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || decoded.dimension->kind != BoundaryDimensionKind::assembly_extent) continue;
        require(dimensions.size() < dimension_limit, "complete standalone assembly dimension inventory exceeds bound");
        const auto& dimension = *decoded.dimension;
        require(roots.contains(root_target(dimension.assembly_target->component)),
            "standalone dimension does not name an exact actual qualified source root");
        dimensions.emplace(id, dimension); targets.push_back(*dimension.assembly_target);
    }
    // One shared complete-map admission/expansion ledger for all actual targets.
    if (!targets.empty()) (void)resolve_assembly_dimensions(source, targets);
    return dimensions;
}
// This bounded scratch remainder never becomes output. Closed local component
// values are not document reference authority. Unknown envelope siblings stay.
Entity remainder(const Entity& entity, const Ids& affected, const Ids& copied_overlays,
    const Ids& copied_dimensions, const Ids& dimension_owners, std::vector<Gap>& gaps) {
    auto result = entity;
    auto& p = result.properties;
    if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == "assembly_instance") {
        (void)decode_document_assembly_instance(entity); p.erase("instance"); p.erase("assembly_catalog_id");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == assembly_phase_membership_entity_type) {
        (void)AssemblyPhaseMembership::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        auto& views = p.at("model").at("views");
        for (std::size_t index = 0; index < views.size(); ++index) {
            auto& view = views.at(index);
            const auto base = "/properties/model/views/" + std::to_string(index);
            // Overlay IDs are view children, not analytical source objects.
            // Do not erase a typed slot whose translation is unsupported.
            const auto copied_reference = [&](const Json& value, const std::string& path) {
                const auto& target = value.get_ref<const std::string&>();
                if (copied_overlays.contains(target)) add_gap(gaps, entity.id, path, target,
                    "view source references cannot bind copied overlay children");
                if (copied_dimensions.contains(target)) add_gap(gaps, entity.id, path, target,
                    "view reference requires deliberate retained/proposed standalone dimension identity carry");
            };
            if (view.contains("object_ids")) for (std::size_t item = 0; item < view.at("object_ids").size(); ++item)
                copied_reference(view.at("object_ids").at(item), base + "/object_ids/" + std::to_string(item));
            view.erase("object_ids");
            if (view.contains("presentation")) {
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    for (std::size_t item = 0; item < presentation.at("appearance").at("objects").size(); ++item) {
                        auto& row = presentation.at("appearance").at("objects").at(item);
                        copied_reference(row.at("object_id"), base + "/presentation/appearance/objects/" + std::to_string(item) + "/object_id");
                        row.erase("object_id");
                    }
            }
            if (view.contains("overlays")) for (std::size_t item = 0; item < view.at("overlays").size(); ++item) {
                auto& row = view.at("overlays").at(item);
                const auto row_path = base + "/overlays/" + std::to_string(item);
                copied_reference(row.at("object_id"), row_path + "/object_id");
                row.erase("id"); row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) {
                    copied_reference(row.at("dimension_binding").at("object_id"), row_path + "/dimension_binding/object_id");
                    row.at("dimension_binding").erase("object_id");
                }
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        auto& rows = p.at("state").at("overrides");
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto target = rows.at(index).at("target_id").get<std::string>();
            if (copied_overlays.contains(target))
                add_gap(gaps, entity.id, "/properties/state/overrides/" + std::to_string(index) + "/target_id", target,
                    "annotation override codec has no associated overlay-child target translation");
            else if (copied_dimensions.contains(target))
                add_gap(gaps, entity.id, "/properties/state/overrides/" + std::to_string(index) + "/target_id", target,
                    "annotation override requires deliberate retained/proposed standalone dimension identity carry");
            else if (affected.contains(target) && rows.at(index).at("target_kind") != "object")
                add_gap(gaps, entity.id, "/properties/state/overrides/" + std::to_string(index) + "/target_id", target,
                    "area and wall callout targets require genuine analytical owners, not component aliases");
            rows.at(index).erase("target_id");
        }
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (decoded.supported()) {
            if (decoded.dimension->kind == BoundaryDimensionKind::assembly_extent) {
                // The closed target codecs have genuinely validated these
                // owned fields, including local path/axis values. The scratch
                // remainder supplies no opaque document-reference authority.
                p.at("target").erase("assembly");
            } else if (affected.contains(decoded.dimension->boundary_id) || dimension_owners.contains(decoded.dimension->boundary_id))
                add_gap(gaps, entity.id, "/properties/target/entity_id", decoded.dimension->boundary_id,
                    "standalone dimension codec has no component-bound target; identified boundary/stroke, physical wall or corner-window semantics are required");
            p.at("target").erase("entity_id");
        }
    }
    return result;
}
PhaseAssemblyCatalogEditPresentationPlan inspect(const Entities& source, const Json& request, Dimensions* inventory = nullptr) {
    admit(source, request);
    PhaseAssemblyCatalogEditPresentationPlan result;
    result.producer = inspect_phase_assembly_catalog_edit_plan(source, request);
    auto dimensions = inspect_dimensions(source, result.producer);
    const auto aliases = embedded_assembly_presentation_ids(source); validate_aliases(source, aliases);
    const auto affected_owners = owners(result.producer, aliases);
    Ids overlays;
    std::map<std::string, std::size_t, std::less<>> occurrences;
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType) {
        (void)id;
        for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
            for (const auto& row : view.at("overlays")) {
                const auto original = row.at("id").get<std::string>(); overlay_id(original); ++occurrences[original];
                if (affected_overlay(row, affected_owners.baseline)) {
                    require(overlays.size() < identity_limit, "affected overlay inventory exceeds bound"); overlays.insert(original);
                }
            }
    }
    for (const auto& id : overlays) require(occurrences.at(id) == 1 && !source.contains(id), "affected overlay aliases another retained owner");
    result.required_overlay_ids.assign(overlays.begin(), overlays.end());
    Ids copied_dimensions, dimension_owners;
    std::set<Target> baseline_roots;
    for (const auto& row : result.producer.isolation_roots)
        if (row.baseline_replacement) baseline_roots.insert(row.root);
    for (const auto& [id, dimension] : dimensions)
        if (baseline_roots.contains(root_target(dimension.assembly_target->component))) copied_dimensions.insert(id);
    result.required_dimension_ids.assign(copied_dimensions.begin(), copied_dimensions.end());
    require(copied_dimensions.size() <= identity_limit - overlays.size(), "complete affected presentation roster exceeds bound");
    for (const auto& impact : result.producer.source.roots)
        if (impact.dependency_changed || impact.expansion_changed || impact.definitions_changed)
            dimension_owners.insert(impact.root.document_entity_id ? *impact.root.document_entity_id : impact.root.catalog_id);
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == kAnnotationEntityType) {
            for (const auto* collection : {"labels", "symbols"})
                for (const auto& row : entity.properties.at("state").at(collection))
                    require(!copied_dimensions.contains(row.at("id").get<std::string>()), "baseline dimension aliases another retained annotation child");
        } else if (entity.type == kSheetViewEntityType)
            for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
                for (const auto& row : view.at("overlays"))
                    require(!copied_dimensions.contains(row.at("id").get<std::string>()), "baseline dimension aliases another retained overlay child");
    }
    Ids affected = affected_owners.ordinary;
    affected.insert(affected_owners.baseline.begin(), affected_owners.baseline.end());
    affected.insert(overlays.begin(), overlays.end());
    affected.insert(copied_dimensions.begin(), copied_dimensions.end());
    if (!affected.empty() || !dimension_owners.empty()) for (const auto& [id, entity] : source) {
        const auto opaque = remainder(entity, affected, overlays, copied_dimensions, dimension_owners, result.unsupported_references);
        opaque_gaps(opaque.properties, affected, id, "/properties", result.unsupported_references);
        opaque_gaps(opaque.extensions, affected, id, "/extensions", result.unsupported_references);
    }
    if (inventory) *inventory = std::move(dimensions);
    return result;
}
void remap(Json& row, const char* key, const IdMap& mapping) {
    const auto found = mapping.find(row.at(key).get<std::string>());
    if (found != mapping.end()) row.at(key) = found->second;
}
void remap_overlay(Json& row, const IdMap& mapping) {
    remap(row, "object_id", mapping);
    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) remap(row.at("dimension_binding"), "object_id", mapping);
}
using DimensionTargets = std::map<std::string, AssemblyDimensionTarget, std::less<>>;
DimensionTargets proposed_dimension_targets(const Dimensions& dimensions, const PhaseAssemblyCatalogEditResult& producer) {
    DimensionTargets result;
    std::vector<AssemblyDimensionTarget> targets;
    for (const auto& [id, dimension] : dimensions) {
        auto target = *dimension.assembly_target;
        const auto original = root_target(target.component);
        const auto found = producer.original_to_proposed.find(original);
        require(found != producer.original_to_proposed.end(), "actual producer omitted a qualified dimension source root");
        const auto& destination = found->second;
        require(destination.part_path.empty() &&
            destination.document_entity_id.has_value() == original.document_entity_id.has_value(),
            "actual producer changed dimension root identity kind/path");
        target.component.catalog_id = destination.catalog_id;
        target.component.instance_id = destination.instance_id;
        target.component.document_entity_id = destination.document_entity_id;
        result.emplace(id, target); targets.push_back(std::move(target));
    }
    // Missing/retired nested paths or non-measurable proposed geometry reject
    // the whole replay before any dimension or presentation row is copied.
    if (!targets.empty()) (void)resolve_assembly_dimensions(producer.entities, targets);
    return result;
}
void retarget_dimension(Entity& entity, const AssemblyDimensionTarget& destination) {
    auto& target = entity.properties.at("target");
    target.at("entity_id") = destination.component.document_entity_id ?
        *destination.component.document_entity_id : destination.component.catalog_id;
    auto& assembly = target.at("assembly");
    assembly.at("catalog_id") = destination.component.catalog_id;
    assembly.at("instance_id") = destination.component.instance_id;
    assembly.at("document_entity_id") = destination.component.document_entity_id ?
        Json(*destination.component.document_entity_id) : Json(nullptr);
    const auto decoded = decode_boundary_dimension_entity(entity);
    require(decoded.supported() && decoded.dimension->assembly_target == destination,
        "retargeted dimension differs from actual qualified proposed target");
}
void admit_fresh_before_producer(const Entities& source, const EmbeddedAssemblyPresentationIds& aliases,
    const PhaseAssemblyCatalogEditAllocation& allocation, const PhaseAssemblyCatalogEditPresentationAllocation& presentation) {
    const auto occupied = reserved_ids(source, aliases);
    Ids fresh;
    const auto reserve = [&](const std::string& id) {
        require(!occupied.contains(id) && fresh.insert(id).second,
            "fresh allocation collides across actual retained owners/local identities/aliases and presentation allocations");
    };
    for (const auto& [root, identity] : allocation.roots) {
        (void)root; reserve(identity.private_catalog_id);
        if (identity.replacement_entity_id) reserve(*identity.replacement_entity_id);
        // A baseline independent owner and its instance deliberately share one
        // producer identity; the closed allocation codec owns that equality.
        if (identity.replacement_instance_id && identity.replacement_instance_id != identity.replacement_entity_id)
            reserve(*identity.replacement_instance_id);
    }
    for (const auto& [registry, id] : allocation.membership_entity_ids) { (void)registry; reserve(id); }
    for (const auto& [original, proposed] : presentation.overlay_ids) { (void)original; reserve(proposed); }
    for (const auto& [original, proposed] : presentation.dimension_ids) { (void)original; reserve(proposed); }
    require(fresh.size() <= identity_limit, "complete fresh allocation inventory exceeds bound");
}
void admit_carry_growth(const Entities& candidate, const Entities& source, const IdMap& ordinary,
    const IdMap& baseline, const PhaseAssemblyCatalogEditPresentationAllocation& allocation,
    const Dimensions& dimensions, const DimensionTargets& dimension_targets) {
    // Charge the complete actual producer result plus every prospective copy
    // before allocating presentation arrays/rows. Charge replacement strings
    // in addition to their retained spelling, conservatively bounding growth.
    Budget budget; budget.map(candidate);
    require(allocation.dimension_ids.size() <= 100000 - candidate.size(), "standalone dimension copies exceed entity count bound");
    for (const auto& [id, dimension] : dimensions) {
        const auto& destination = dimension_targets.at(id);
        if (allocation.dimension_ids.contains(id)) {
            const auto& entity = source.at(id);
            budget.text(allocation.dimension_ids.at(id)); budget.text(entity.type);
            budget.read(entity.properties); budget.read(entity.extensions);
        }
        if (destination != *dimension.assembly_target) {
            // Include both the owner carrier and qualified closed target;
            // unchanged axis/path/presentation fields are already charged.
            budget.text(destination.component.document_entity_id ?
                *destination.component.document_entity_id : destination.component.catalog_id);
            budget.text(destination.component.catalog_id); budget.text(destination.component.instance_id);
            if (destination.component.document_entity_id) budget.text(*destination.component.document_entity_id);
        }
    }
    Ids baseline_owners;
    for (const auto& [id, proposed] : baseline) { (void)proposed; baseline_owners.insert(id); }
    const auto changed_string = [&](const Json& value, const IdMap& mapping) {
        const auto found = mapping.find(value.get_ref<const std::string&>());
        if (found != mapping.end()) budget.text(found->second);
    };
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == kSheetViewEntityType) for (const auto& view : entity.properties.at("model").at("views")) {
            if (view.contains("object_ids")) for (const auto& object : view.at("object_ids")) {
                changed_string(object, ordinary);
                const auto found = baseline.find(object.get_ref<const std::string&>());
                if (found != baseline.end()) budget.read(Json(found->second));
            }
            const auto& p = view.at("presentation");
            if (p.contains("appearance") && !p.at("appearance").is_null())
                for (const auto& row : p.at("appearance").at("objects")) {
                    changed_string(row.at("object_id"), ordinary);
                    if (baseline.contains(row.at("object_id").get_ref<const std::string&>())) {
                        budget.read(row); changed_string(row.at("object_id"), baseline);
                    }
                }
            if (view.contains("overlays")) for (const auto& row : view.at("overlays")) {
                changed_string(row.at("object_id"), ordinary);
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    changed_string(row.at("dimension_binding").at("object_id"), ordinary);
                if (affected_overlay(row, baseline_owners)) {
                    budget.read(row); budget.text(allocation.overlay_ids.at(row.at("id").get<std::string>()));
                    changed_string(row.at("object_id"), baseline);
                    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                        changed_string(row.at("dimension_binding").at("object_id"), baseline);
                }
            }
        } else if (entity.type == kAnnotationEntityType)
            for (const auto& row : entity.properties.at("state").at("overrides")) {
                changed_string(row.at("target_id"), ordinary);
                if (baseline.contains(row.at("target_id").get_ref<const std::string&>())) {
                    budget.read(row); changed_string(row.at("target_id"), baseline);
                }
            }
    }
}
} // namespace

PhaseAssemblyCatalogEditPresentationPlan inspect_phase_assembly_catalog_edit_presentation_plan(
    const Entities& source, const Json& request) {
    try { return inspect(source, request); }
    catch (const Json::exception& error) { reject(std::string("invalid source presentation: ") + error.what()); }
}
Json encode_phase_assembly_catalog_edit_presentation_allocation(
    const PhaseAssemblyCatalogEditPresentationAllocation& allocation) {
    admit_allocation(allocation);
    auto rows = Json::array();
    for (const auto& [old_id, new_id] : allocation.overlay_ids)
        rows.push_back({{"original_overlay_id", old_id}, {"proposed_overlay_id", new_id}});
    if (allocation.dimension_ids.empty()) return {{"version", 1}, {"overlay_ids", std::move(rows)}};
    auto dimensions = Json::array();
    for (const auto& [old_id, new_id] : allocation.dimension_ids)
        dimensions.push_back({{"original_dimension_id", old_id}, {"proposed_dimension_id", new_id}});
    return {{"version", 2}, {"overlay_ids", std::move(rows)}, {"dimension_ids", std::move(dimensions)}};
}
PhaseAssemblyCatalogEditPresentationAllocation decode_phase_assembly_catalog_edit_presentation_allocation(const Json& wire) {
    try {
        Budget budget{0, 0, 16 * 1024 * 1024, 262144}; budget.read(wire);
        require(wire.is_object() && wire.contains("version") && wire.at("version").is_number_integer() &&
            (wire.at("version") == 1 || wire.at("version") == 2), "unsupported presentation allocation version");
        const bool dimensions = wire.at("version") == 2;
        if (dimensions) closed(wire, {"version", "overlay_ids", "dimension_ids"});
        else closed(wire, {"version", "overlay_ids"});
        const auto& rows = wire.at("overlay_ids");
        require(rows.is_array() && rows.size() <= identity_limit, "invalid overlay allocation roster");
        PhaseAssemblyCatalogEditPresentationAllocation result;
        std::optional<std::string> previous;
        for (const auto& row : rows) {
            closed(row, {"original_overlay_id", "proposed_overlay_id"});
            require(row.at("original_overlay_id").is_string() && row.at("proposed_overlay_id").is_string(), "overlay identities must be text");
            const auto original = row.at("original_overlay_id").get<std::string>();
            const auto proposed = row.at("proposed_overlay_id").get<std::string>(); overlay_id(original); fresh_id(proposed);
            require(!previous || *previous < original, "overlay allocation roster is not sorted and unique"); previous = original;
            result.overlay_ids.emplace(original, proposed);
        }
        if (dimensions) {
            const auto& dimension_rows = wire.at("dimension_ids");
            require(dimension_rows.is_array() && dimension_rows.size() <= identity_limit - rows.size(),
                "invalid dimension allocation roster");
            previous.reset();
            for (const auto& row : dimension_rows) {
                closed(row, {"original_dimension_id", "proposed_dimension_id"});
                require(row.at("original_dimension_id").is_string() && row.at("proposed_dimension_id").is_string(),
                    "dimension identities must be text");
                const auto original = row.at("original_dimension_id").get<std::string>();
                const auto proposed = row.at("proposed_dimension_id").get<std::string>(); fresh_id(original); fresh_id(proposed);
                require(!previous || *previous < original, "dimension allocation roster is not sorted and unique"); previous = original;
                result.dimension_ids.emplace(original, proposed);
            }
        }
        admit_allocation(result); return result;
    } catch (const Json::exception& error) { reject(std::string("invalid overlay allocation: ") + error.what()); }
}
PhaseAssemblyCatalogEditResult replay_phase_assembly_catalog_edit_presentation_entities(
    const Entities& source, const Json& request, const PhaseAssemblyCatalogEditAllocation& allocation,
    const PhaseAssemblyCatalogEditPresentationAllocation& presentation) {
    try {
        // Admit complete raw source and BOTH bounded allocations before any
        // producer copies. Inspect re-derives the actual source-bound roster.
        admit(source, request); admit_allocation(presentation);
        (void)encode_phase_assembly_catalog_edit_allocation(allocation);
        Dimensions dimensions;
        const auto plan = inspect(source, request, &dimensions);
        if (!plan.unsupported_references.empty()) {
            const auto& gap = plan.unsupported_references.front();
            reject("unsupported reference in " + gap.entity_id + gap.field + " to " + gap.referenced_id + ": " + gap.reason);
        }
        require(presentation.overlay_ids.size() == plan.required_overlay_ids.size(), "overlay allocation differs from actual baseline roster");
        for (const auto& id : plan.required_overlay_ids) require(presentation.overlay_ids.contains(id), "missing actual baseline overlay allocation");
        require(presentation.dimension_ids.size() == plan.required_dimension_ids.size(), "dimension allocation differs from actual baseline roster");
        for (const auto& id : plan.required_dimension_ids) require(presentation.dimension_ids.contains(id), "missing actual baseline dimension allocation");
        const auto before_aliases = embedded_assembly_presentation_ids(source);
        admit_fresh_before_producer(source, before_aliases, allocation, presentation);
        auto result = replay_phase_assembly_catalog_edit_entities(source, request, allocation);
        Budget{}.map(result.entities);
        AssemblyCatalogTransferBudget catalogs;
        for (const auto& [id, entity] : result.entities) {
            (void)id; if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalogs);
        }
        const auto after_aliases = embedded_assembly_presentation_ids(result.entities);
        validate_aliases(result.entities, after_aliases);
        const auto dimension_targets = proposed_dimension_targets(dimensions, result);
        IdMap ordinary, baseline;
        for (const auto& row : plan.producer.isolation_roots) {
            const auto& old = row.root;
            const auto& destination = result.original_to_proposed.at(old);
            require(destination.part_path.empty() && destination.document_entity_id.has_value() == old.document_entity_id.has_value(),
                "actual producer changed root identity kind/path");
            const auto old_id = old.document_entity_id ? *old.document_entity_id : before_aliases.at({old.catalog_id, old.instance_id});
            const auto new_id = destination.document_entity_id ? *destination.document_entity_id : after_aliases.at({destination.catalog_id, destination.instance_id});
            if (old_id == new_id) {
                require(!row.baseline_replacement && old.document_entity_id.has_value(), "isolated root has no fresh actual presentation destination");
                continue;
            }
            require((row.baseline_replacement ? baseline : ordinary).emplace(old_id, new_id).second, "actual presentation mapping overlaps");
        }
        Ids fresh(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
        require(fresh.size() == result.fresh_identity_ids.size(), "actual producer fresh inventory is not injective");
        auto reserved = reserved_ids(result.entities, after_aliases);
        const auto source_reserved = reserved_ids(source, before_aliases);
        reserved.insert(source_reserved.begin(), source_reserved.end());
        for (const auto& id : fresh) require(!source_reserved.contains(id), "actual producer fresh identity reuses a retained source-local identity");
        for (const auto& [original, proposed] : presentation.overlay_ids) {
            (void)original;
            require(!reserved.contains(proposed) && fresh.insert(proposed).second, "fresh overlay collides with actual source/candidate identity");
        }
        for (const auto& [original, proposed] : presentation.dimension_ids) {
            (void)original;
            require(!reserved.contains(proposed) && fresh.insert(proposed).second, "fresh dimension collides with actual source/candidate identity or overlay allocation");
        }
        require(fresh.size() <= identity_limit, "complete fresh presentation inventory exceeds bound");
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        validate_phase_assembly_instance_property_edit_fresh_identities(source, result.fresh_identity_ids);
        Ids baseline_owners, ordinary_owners;
        for (const auto& [id, proposed] : baseline) { (void)proposed; baseline_owners.insert(id); }
        for (const auto& [id, proposed] : ordinary) { (void)proposed; ordinary_owners.insert(id); }
        admit_carry_growth(result.entities, source, ordinary, baseline, presentation, dimensions, dimension_targets);
        for (const auto& [id, entity] : source) {
            auto& candidate = result.entities.at(id);
            if (entity.type == kSheetViewEntityType) {
                const auto& views = entity.properties.at("model").at("views");
                auto& changed_views = candidate.properties.at("model").at("views");
                for (std::size_t index = 0; index < views.size(); ++index) {
                    const auto& original = views.at(index); auto& changed = changed_views.at(index);
                    if (original.contains("object_ids")) {
                        for (auto& object : changed.at("object_ids")) {
                            const auto found = ordinary.find(object.get<std::string>()); if (found != ordinary.end()) object = found->second;
                        }
                        for (const auto& object : original.at("object_ids")) {
                            const auto found = baseline.find(object.get<std::string>()); if (found != baseline.end()) changed.at("object_ids").push_back(found->second);
                        }
                    }
                    if (original.contains("presentation")) {
                        const auto& p = original.at("presentation");
                        if (p.contains("appearance") && !p.at("appearance").is_null()) {
                            auto& changed_rows = changed.at("presentation").at("appearance").at("objects");
                            for (auto& row : changed_rows) remap(row, "object_id", ordinary);
                            for (const auto& row : p.at("appearance").at("objects")) if (baseline.contains(row.at("object_id").get<std::string>())) {
                                auto copied = row; remap(copied, "object_id", baseline); changed_rows.push_back(std::move(copied));
                            }
                        }
                    }
                    if (original.contains("overlays")) {
                        auto& changed_rows = changed.at("overlays");
                        for (auto& row : changed_rows) remap_overlay(row, ordinary);
                        for (const auto& row : original.at("overlays")) if (affected_overlay(row, baseline_owners)) {
                            // Admitted dimension bindings require object_id to
                            // be empty or equal to their owner, so a baseline
                            // overlay cannot also target an ordinary remap.
                            require(!affected_overlay(row, ordinary_owners), "overlay crosses retained baseline and ordinary owners");
                            auto copied = row; copied.at("id") = presentation.overlay_ids.at(row.at("id").get<std::string>());
                            remap_overlay(copied, baseline); changed_rows.push_back(std::move(copied));
                        }
                    }
                }
                validate_sheet_view_entity(candidate);
            } else if (entity.type == kAnnotationEntityType) {
                auto& rows = candidate.properties.at("state").at("overrides");
                for (auto& row : rows) remap(row, "target_id", ordinary);
                for (const auto& row : entity.properties.at("state").at("overrides")) if (baseline.contains(row.at("target_id").get<std::string>())) {
                    auto copied = row; remap(copied, "target_id", baseline); rows.push_back(std::move(copied));
                }
                validate_annotation_entity(candidate);
            }
        }
        for (const auto& [id, dimension] : dimensions) {
            const auto& destination = dimension_targets.at(id);
            const auto copied_id = presentation.dimension_ids.find(id);
            if (copied_id != presentation.dimension_ids.end()) {
                require(result.entities.at(id) == source.at(id), "actual producer changed retained baseline dimension envelope");
                auto copied = source.at(id); copied.id = copied_id->second;
                retarget_dimension(copied, destination);
                require(result.entities.emplace(copied_id->second, std::move(copied)).second, "fresh standalone dimension destination is occupied");
            } else if (destination != *dimension.assembly_target) {
                // Qualified catalogs may change while an ordinary independent
                // document owner and dimension identity both remain stable.
                retarget_dimension(result.entities.at(id), destination);
            }
        }
        Budget{}.map(result.entities);
        require(embedded_assembly_presentation_ids(result.entities) == after_aliases, "presentation carry changed actual embedded aliases");
        validate_aliases(result.entities, after_aliases);
        return result;
    } catch (const Json::exception& error) { reject(std::string("invalid actual presentation replay: ") + error.what()); }
}
} // namespace sketch
