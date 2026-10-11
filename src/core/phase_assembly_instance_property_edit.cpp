#include "sketch/phase_assembly_instance_property_edit.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_store.hpp"
#include "sketch/retained_authoring_identities.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Target = AssemblyInstancePropertyEditTarget;
constexpr std::size_t count_limit = 1000;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t presentation_identity_limit = 4096;
[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(std::string("Phase assembly instance Properties: ") + message);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
struct WireBudget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t value) {
        if (value > byte_limit - bytes) reject("wire byte budget exceeded");
        bytes += value;
    }
    void string(std::string_view value) {
        add(2);
        for (const unsigned char c : value) add(c == '"' || c == '\\' ? 2 : c < 32 ? 6 : 1);
    }
    void visit(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 100000) reject("wire node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable wire JSON");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { string(key); add(1); visit(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) visit(child, depth + 1);
        } else if (value.is_string()) string(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
void admit_identity(std::string_view text) {
    if (text.empty() || text.size() > 128 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid bounded document identity");
}
std::string identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    admit_identity(text);
    return text;
}
// Existing sheet overlay identities are codec-owned bounded text, not newly
// allocated document tokens. Exact actual-source matching supplies authority.
void admit_original_overlay_identity(std::string_view text) {
    if (text.empty() || text.size() > 128) reject("original overlay identity exceeds bound");
}
std::string original_overlay_identity(const Json& value) {
    if (!value.is_string()) reject("original overlay identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    admit_original_overlay_identity(text);
    return text;
}
Json optional(const auto& value) { return value ? Json(*value) : Json(nullptr); }
Target root_target(const Json& value) {
    // Reuse the existing qualified target codec through an unchanged intent;
    // neither identity parsing nor historical property intent authority forks.
    AssemblyInstancePropertyEditIntent empty;
    empty.target = {"catalog", "instance", std::nullopt, {}};
    auto wire = encode_assembly_instance_property_edit_intent(empty);
    wire["target"] = value;
    auto target = decode_assembly_instance_property_edit_intent(wire).target;
    if (!target.part_path.empty()) reject("allocation must name an exact qualified root");
    return target;
}
Json encode_root(const Target& target) {
    if (!target.part_path.empty()) reject("allocation must name an exact qualified root");
    AssemblyInstancePropertyEditIntent empty; empty.target = target;
    return encode_assembly_instance_property_edit_intent(empty).at("target");
}
PhaseAssemblyInstancePropertyEditAuthoring decode_authoring(const Json& value) {
    WireBudget{}.visit(value);
    // UTF-8 is admitted only after the byte/node bound; a malformed request
    // cannot force an unbounded dump or reach source model decoding.
    (void)value.dump();
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2)) reject("unsupported authoring version");
    const bool carry = value.at("version") == 2;
    if (carry) {
        fields(value, {"version", "intents", "allocation", "carry_presentation_references", "presentation_overlay_identities"});
        if (!value.at("carry_presentation_references").is_boolean() || value.at("carry_presentation_references") != true)
            reject("v2 requires literal presentation carry");
    } else fields(value, {"version", "intents", "allocation"});
    const auto& intents = value.at("intents");
    if (!intents.is_array() || intents.empty() || intents.size() > count_limit) reject("invalid intent count");
    if (carry) {
        const auto& allocation = value.at("allocation");
        fields(allocation, {"roots", "membership_entity_ids"});
        const auto& roots = allocation.at("roots");
        const auto& memberships = allocation.at("membership_entity_ids");
        const auto& overlays = value.at("presentation_overlay_identities");
        if (!roots.is_array() || roots.size() > count_limit || !memberships.is_array() || memberships.size() > count_limit ||
            !overlays.is_array() || overlays.size() > presentation_identity_limit) reject("typed presentation request count exceeds bound");
        std::size_t fresh_count = memberships.size() + overlays.size();
        for (const auto& row : roots) {
            fields(row, {"root", "proposed_entity_id", "proposed_catalog_id", "proposed_instance_id"});
            fresh_count += row.at("proposed_entity_id").is_null() ? 3 : 1;
        }
        if (fresh_count > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    }
    PhaseAssemblyInstancePropertyEditAuthoring result;
    result.carry_presentation_references = carry;
    for (const auto& row : intents) {
        auto intent = decode_assembly_instance_property_edit_intent(row);
        if (!result.intents.empty() && !(result.intents.back().target < intent.target)) reject("intent targets must be sorted unique");
        result.intents.push_back(std::move(intent));
    }
    const auto& allocation = value.at("allocation");
    fields(allocation, {"roots", "membership_entity_ids"});
    const auto& roots = allocation.at("roots");
    if (!roots.is_array() || roots.size() > count_limit) reject("invalid root allocation count");
    std::optional<Target> previous;
    std::set<std::string, std::less<>> fresh;
    const auto reserve = [&](const Json& wire) {
        auto id = identity(wire);
        if (!fresh.insert(id).second) reject("allocation identities must be injective");
        return id;
    };
    for (const auto& row : roots) {
        fields(row, {"root", "proposed_entity_id", "proposed_catalog_id", "proposed_instance_id"});
        auto root = root_target(row.at("root"));
        if (previous && !(*previous < root)) reject("root allocation targets must be sorted unique");
        previous = root;
        PhaseAssemblyInstanceReplacementIdentity ids;
        if (!row.at("proposed_entity_id").is_null()) ids.proposed_entity_id = reserve(row.at("proposed_entity_id"));
        if (!row.at("proposed_catalog_id").is_null()) ids.proposed_catalog_id = reserve(row.at("proposed_catalog_id"));
        if (root.document_entity_id ? (!ids.proposed_entity_id || ids.proposed_catalog_id.has_value()) :
            (!ids.proposed_catalog_id || ids.proposed_entity_id.has_value())) reject("allocation role differs from qualified root role");
        if (root.document_entity_id) {
            ids.proposed_instance_id = identity(row.at("proposed_instance_id"));
            if (ids.proposed_instance_id != *ids.proposed_entity_id)
                reject("independent entity and instance require the same fresh identity");
        } else ids.proposed_instance_id = reserve(row.at("proposed_instance_id"));
        result.allocation.roots.emplace(std::move(root), std::move(ids));
    }
    const auto& memberships = allocation.at("membership_entity_ids");
    if (!memberships.is_array() || memberships.size() > count_limit) reject("invalid membership allocation count");
    std::optional<std::string> previous_registry;
    for (const auto& row : memberships) {
        fields(row, {"registry_id", "membership_entity_id"});
        auto registry = identity(row.at("registry_id"));
        if (previous_registry && !(*previous_registry < registry)) reject("membership allocations must be sorted unique");
        previous_registry = registry;
        result.allocation.membership_entity_ids.emplace(std::move(registry), reserve(row.at("membership_entity_id")));
    }
    if (carry) {
        const auto& overlays = value.at("presentation_overlay_identities");
        if (!overlays.is_array() || overlays.size() > presentation_identity_limit) reject("invalid overlay allocation count");
        std::optional<std::string> previous_overlay;
        for (const auto& row : overlays) {
            fields(row, {"original_overlay_id", "proposed_overlay_id"});
            auto original = original_overlay_identity(row.at("original_overlay_id"));
            if (previous_overlay && !(*previous_overlay < original)) reject("overlay allocations must be sorted unique");
            previous_overlay = original;
            result.presentation_overlay_identities.emplace(std::move(original), reserve(row.at("proposed_overlay_id")));
        }
        const auto aliases = std::count_if(result.allocation.roots.begin(), result.allocation.roots.end(),
            [](const auto& row) { return !row.first.document_entity_id; });
        if (fresh.size() + static_cast<std::size_t>(aliases) > presentation_identity_limit)
            reject("complete fresh presentation inventory exceeds bound");
    }
    return result;
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid captured revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source digest must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("invalid source digest");
}
PhaseAssemblyInstancePropertyEditAuthoring decode_request(const Json& value) {
    WireBudget{}.visit(value);
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "authoring"});
    if (!value.at("version").is_number_integer() || (value.at("version") != 1 && value.at("version") != 2))
        reject("unsupported request version");
    (void)revision(value.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(name));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    auto authoring = decode_authoring(value.at("authoring"));
    if (value.at("version") != (authoring.carry_presentation_references ? 2 : 1))
        reject("request and authoring versions differ");
    return authoring;
}
void authenticate(const DocumentSnapshot& source, const Json& request) {
    if (!source.is_editable()) reject("captured source is read-only");
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() || source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size())) reject("invalid source history/save metadata");
    const auto saved = request.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(request.at("source_saved_revision"))};
    if (source.revision() != revision(request.at("expected_revision")) || source.saved_revision_optional() != saved ||
        document_snapshot_digest(source) != request.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source) != request.at("source_authoring_digest") ||
        entity_map_digest(source.entities()) != request.at("source_entities_digest")) reject("complete captured source changed");
}
using PresentationIds = std::set<std::string, std::less<>>;
using PresentationMap = std::map<std::string, std::string, std::less<>>;
// Preflight all retained raw presentation/opaque records before decoding or
// making scratch copies. This ledger includes catalogs and envelope siblings.
struct PresentationSourceBudget {
    std::size_t nodes{}, bytes{};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("presentation source node/nesting budget exceeded");
        const auto charge = [&](std::size_t amount) {
            if (amount > 64 * 1024 * 1024 - bytes) reject("presentation source byte budget exceeded");
            bytes += amount;
        };
        charge(32);
        if (value.is_string()) {
            const auto size = value.get_ref<const std::string&>().size();
            if (size > (64 * 1024 * 1024 - bytes) / 6) reject("presentation source byte budget exceeded");
            charge(size * 6);
        } else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            if (key.size() > (64 * 1024 * 1024 - bytes) / 6) reject("presentation source byte budget exceeded");
            charge(key.size() * 6); read(child, depth + 1);
        } else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable presentation source JSON");
    }
};
void admit_presentation_source(const AssemblyDocumentEntities& source) {
    if (source.size() > 65536) reject("presentation source entity budget exceeded");
    PresentationSourceBudget budget;
    for (const auto& [id, entity] : source) {
        if (id.empty() || id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("presentation source requires actual identified entity envelopes");
        budget.read(entity.properties); budget.read(entity.extensions);
    }
}
PresentationIds replacement_presentation_owners(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditPlan& plan) {
    const auto aliases = embedded_assembly_presentation_ids(source);
    PresentationIds owners;
    for (const auto& replacement : plan.replacement_roots) {
        const auto& root = replacement.root;
        const auto& owner = root.document_entity_id ? *root.document_entity_id : aliases.at({root.catalog_id, root.instance_id});
        if (!owners.insert(owner).second) reject("replacement presentation owners overlap");
    }
    return owners;
}
bool affected_overlay(const Json& overlay, const PresentationIds& owners) {
    return owners.contains(overlay.at("object_id").get<std::string>()) ||
        (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null() &&
            owners.contains(overlay.at("dimension_binding").at("object_id").get<std::string>()));
}
PresentationIds presentation_overlay_ids(const AssemblyDocumentEntities& source, const PresentationIds& owners) {
    PresentationIds ids;
    std::map<std::string, std::size_t, std::less<>> occurrences;
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType) {
        (void)id; (void)decode_sheet_view_entity(entity);
        for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
            for (const auto& overlay : view.at("overlays")) {
                const auto overlay_id = original_overlay_identity(overlay.at("id"));
                ++occurrences[overlay_id];
                if (affected_overlay(overlay, owners)) {
                    ids.insert(overlay_id);
                    if (ids.size() > presentation_identity_limit) reject("affected overlay count exceeds bound");
                }
            }
    }
    for (const auto& id : ids) if (occurrences.at(id) != 1 || source.contains(id))
        reject("affected overlay identity aliases another retained owner");
    return ids;
}
bool touches_presentation_ids(const Json& value, const PresentationIds& ids) {
    if (value.is_string()) return ids.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if (ids.contains(key) || touches_presentation_ids(child, ids)) return true;
    } else if (value.is_array()) for (const auto& child : value)
        if (touches_presentation_ids(child, ids)) return true;
    return false;
}
// This scratch remainder never becomes output. Closed local component values
// (including arbitrary property strings) are not document reference authority.
// Only admitted typed fields are removed; envelope/extension siblings remain.
Entity opaque_presentation_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == "assembly_instance") {
        (void)decode_document_assembly_instance(entity);
        p.erase("instance"); p.erase("assembly_catalog_id");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == assembly_phase_membership_entity_type) {
        (void)AssemblyPhaseMembership::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == kSheetViewEntityType) {
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
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) row.erase("target_id");
    }
    return entity;
}
void complete_presentation_carry(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring, PhaseAssemblyInstancePropertyEditResult& result) {
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, authoring.intents);
    const auto owners = replacement_presentation_owners(source, plan);
    const auto overlays = presentation_overlay_ids(source, owners);
    if (overlays.size() != authoring.presentation_overlay_identities.size()) reject("overlay allocation differs from actual affected roster");
    for (const auto& id : overlays) if (!authoring.presentation_overlay_identities.contains(id))
        reject("missing actual affected overlay allocation");
    const auto source_aliases = embedded_assembly_presentation_ids(source);
    const auto proposed_aliases = embedded_assembly_presentation_ids(result.entities);
    PresentationMap mappings;
    // Re-derive root destinations from the sealed mathematical result. A selected
    // nested path must retain that exact path, and all rows of a root must agree.
    for (const auto& replacement : plan.replacement_roots) {
        const auto& root = replacement.root;
        std::optional<Target> destination;
        for (const auto& intent : authoring.intents) {
            auto selected_root = intent.target; selected_root.part_path.clear();
            if (selected_root != root) continue;
            auto proposed = result.original_to_proposed.at(intent.target);
            if (proposed.part_path != intent.target.part_path) reject("presentation destination changed selected part path");
            proposed.part_path.clear();
            if (destination && *destination != proposed) reject("replacement root has conflicting actual destinations");
            destination = std::move(proposed);
        }
        if (!destination || *destination == root || destination->document_entity_id.has_value() != root.document_entity_id.has_value())
            reject("replacement root has no actual fresh presentation destination");
        const auto& old_id = root.document_entity_id ? *root.document_entity_id : source_aliases.at({root.catalog_id, root.instance_id});
        const auto& new_id = destination->document_entity_id ? *destination->document_entity_id :
            proposed_aliases.at({destination->catalog_id, destination->instance_id});
        if (!mappings.emplace(old_id, new_id).second) reject("duplicate actual presentation owner mapping");
    }
    PresentationIds affected = owners;
    affected.insert(overlays.begin(), overlays.end());
    for (const auto& [id, entity] : source) {
        (void)id;
        auto remainder = opaque_presentation_remainder(entity);
        if (touches_presentation_ids(remainder.properties, affected) || touches_presentation_ids(remainder.extensions, affected))
            reject("opaque source presentation reference cannot be carried safely");
    }
    PresentationIds fresh(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
    for (const auto& [original, proposed] : authoring.presentation_overlay_identities) {
        (void)original;
        if (!fresh.insert(proposed).second) reject("fresh overlay allocation collides with actual replacement identities");
        result.fresh_identity_ids.push_back(proposed);
    }
    if (fresh.size() > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    validate_phase_assembly_instance_property_edit_fresh_identities(source, result.fresh_identity_ids);
    const auto remap = [&](Json& row, const char* key) {
        const auto found = mappings.find(row.at(key).get<std::string>());
        if (found != mappings.end()) row.at(key) = found->second;
    };
    for (const auto& [id, entity] : source) {
        auto& candidate = result.entities.at(id);
        if (entity.type == kSheetViewEntityType) {
            const auto& views = entity.properties.at("model").at("views");
            auto& changed_views = candidate.properties.at("model").at("views");
            for (std::size_t index = 0; index < views.size(); ++index) {
                const auto& original = views.at(index); auto& changed = changed_views.at(index);
                if (original.contains("object_ids")) for (const auto& object : original.at("object_ids")) {
                    const auto found = mappings.find(object.get<std::string>());
                    if (found != mappings.end()) changed.at("object_ids").push_back(found->second);
                }
                const auto& presentation = original.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    for (const auto& original_row : presentation.at("appearance").at("objects"))
                        if (owners.contains(original_row.at("object_id").get<std::string>())) {
                            auto row = original_row; remap(row, "object_id");
                            changed.at("presentation").at("appearance").at("objects").push_back(std::move(row));
                        }
                if (original.contains("overlays")) for (const auto& original_row : original.at("overlays"))
                    if (affected_overlay(original_row, owners)) {
                        auto row = original_row;
                        row.at("id") = authoring.presentation_overlay_identities.at(original_row.at("id").get<std::string>());
                        remap(row, "object_id");
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                            remap(row.at("dimension_binding"), "object_id");
                        changed.at("overlays").push_back(std::move(row));
                    }
            }
            validate_sheet_view_entity(candidate);
        } else if (entity.type == kAnnotationEntityType) {
            for (const auto& original_row : entity.properties.at("state").at("overrides"))
                if (owners.contains(original_row.at("target_id").get<std::string>())) {
                    auto row = original_row; remap(row, "target_id");
                    candidate.properties.at("state").at("overrides").push_back(std::move(row));
                }
            validate_annotation_entity(candidate);
        }
    }
    // Overlay children must not perturb retained embedded aliases or the actual
    // proposed alias used above. Their fresh inventory includes those aliases.
    if (embedded_assembly_presentation_ids(result.entities) != proposed_aliases)
        reject("presentation carry changed actual component aliases");
    std::sort(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
}
} // namespace

std::vector<std::string> inspect_phase_assembly_instance_property_presentation_overlay_ids(
    const AssemblyDocumentEntities& source, std::span<const AssemblyInstancePropertyEditIntent> intents) {
    admit_presentation_source(source);
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, intents);
    const auto ids = presentation_overlay_ids(source, replacement_presentation_owners(source, plan));
    return {ids.begin(), ids.end()};
}

Json encode_phase_assembly_instance_property_edit_authoring(const PhaseAssemblyInstancePropertyEditAuthoring& authoring) {
    if (authoring.intents.empty() || authoring.intents.size() > count_limit || authoring.allocation.roots.size() > count_limit ||
        authoring.allocation.membership_entity_ids.size() > count_limit) reject("typed request count exceeds bound");
    if (!authoring.carry_presentation_references && !authoring.presentation_overlay_identities.empty())
        reject("historical v1 cannot allocate presentation overlays");
    if (authoring.carry_presentation_references) {
        if (authoring.presentation_overlay_identities.size() > presentation_identity_limit) reject("invalid overlay allocation count");
        std::size_t fresh_count = authoring.allocation.membership_entity_ids.size() + authoring.presentation_overlay_identities.size();
        for (const auto& [root, ids] : authoring.allocation.roots) {
            (void)ids; fresh_count += root.document_entity_id ? 1 : 3;
        }
        if (fresh_count > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    }
    Json intents = Json::array(), roots = Json::array(), memberships = Json::array(), overlays = Json::array();
    WireBudget budget;
    for (const auto& intent : authoring.intents) {
        auto wire = encode_assembly_instance_property_edit_intent(intent);
        budget.visit(wire); intents.push_back(std::move(wire));
    }
    for (const auto& [root, ids] : authoring.allocation.roots) {
        // Bound all new strings before creating allocation JSON.
        admit_identity(ids.proposed_instance_id);
        if (ids.proposed_entity_id) admit_identity(*ids.proposed_entity_id);
        if (ids.proposed_catalog_id) admit_identity(*ids.proposed_catalog_id);
        Json wire{{"root", encode_root(root)}, {"proposed_entity_id", optional(ids.proposed_entity_id)},
            {"proposed_catalog_id", optional(ids.proposed_catalog_id)}, {"proposed_instance_id", ids.proposed_instance_id}};
        budget.visit(wire); roots.push_back(std::move(wire));
    }
    for (const auto& [registry, membership] : authoring.allocation.membership_entity_ids) {
        admit_identity(registry); admit_identity(membership);
        Json wire{{"registry_id", registry}, {"membership_entity_id", membership}};
        budget.visit(wire); memberships.push_back(std::move(wire));
    }
    for (const auto& [original, proposed] : authoring.presentation_overlay_identities) {
        admit_original_overlay_identity(original); admit_identity(proposed);
        Json wire{{"original_overlay_id", original}, {"proposed_overlay_id", proposed}};
        budget.visit(wire); overlays.push_back(std::move(wire));
    }
    Json result{{"version", authoring.carry_presentation_references ? 2 : 1}, {"intents", std::move(intents)},
        {"allocation", {{"roots", std::move(roots)}, {"membership_entity_ids", std::move(memberships)}}}};
    if (authoring.carry_presentation_references) {
        result["carry_presentation_references"] = true;
        result["presentation_overlay_identities"] = std::move(overlays);
    }
    (void)decode_authoring(result);
    return result;
}
PhaseAssemblyInstancePropertyEditAuthoring decode_phase_assembly_instance_property_edit_authoring(const Json& value) {
    return decode_authoring(value);
}
PhaseAssemblyInstancePropertyEditPreparation::PhaseAssemblyInstancePropertyEditPreparation(Json request,
    PhaseAssemblyInstancePropertyEditResult result)
    : request_(std::move(request)), result_(std::move(result)),
      digest_(request_.at("source_snapshot_digest").get<std::string>()) {}
Json validate_phase_assembly_instance_property_edit_request(const Json& request) {
    (void)decode_request(request); return request;
}
PhaseAssemblyInstancePropertyEditPreparation prepare_phase_assembly_instance_property_edit(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring) {
    auto wire = encode_phase_assembly_instance_property_edit_authoring(authoring);
    if (authoring.carry_presentation_references) admit_presentation_source(source.entities());
    Json request{{"version", authoring.carry_presentation_references ? 2 : 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", optional(source.saved_revision_optional())},
        {"authoring", std::move(wire)}};
    return replay_phase_assembly_instance_property_edit_preparation(source, request);
}
PhaseAssemblyInstancePropertyEditPreparation replay_phase_assembly_instance_property_edit_preparation(const DocumentSnapshot& source,
    const Json& request) {
    const auto authoring = decode_request(request);
    if (authoring.carry_presentation_references) admit_presentation_source(source.entities());
    authenticate(source, request);
    (void)Document::fork(source); // Actual complete history/assets/read-only admission.
    auto result = replay_phase_assembly_instance_property_edits(source.entities(), authoring.intents, authoring.allocation);
    if (authoring.carry_presentation_references) complete_presentation_carry(source.entities(), authoring, result);
    // Includes undone states. Every allocated ID becomes live in this dialect;
    // exact no-ops cannot carry speculative/unpublished allocations.
    for (const auto& record : source.history()) {
        validate_phase_assembly_instance_property_edit_fresh_identities(record.entities, result.fresh_identity_ids);
        for (const auto& id : result.fresh_identity_ids) if (record.assets.contains(id)) reject("fresh identity reuses retained asset identity");
    }
    const std::set<std::string, std::less<>> fresh(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
    validate_retained_authoring_fresh_identities(source.history(), source.history().size(), fresh);
    // No raw ApplyEntityChanges preview: the enclosing typed composer must
    // complete dependencies and final Document admission before publication.
    return {request, std::move(result)};
}
void require_phase_assembly_instance_property_edit_source_current(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditPreparation& preparation) {
    (void)decode_request(preparation.request()); authenticate(source, preparation.request());
}

} // namespace sketch
