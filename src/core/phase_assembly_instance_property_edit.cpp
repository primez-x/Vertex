#include "sketch/phase_assembly_instance_property_edit.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/drawing_selection_removal.hpp"
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
bool has_geometry_intent(const PhaseAssemblyInstancePropertyEditAuthoring& authoring) {
    return std::any_of(authoring.intents.begin(), authoring.intents.end(), [](const auto& intent) {
        return intent.profile_geometry.operation != AssemblyInstancePropertyPatchOperation::unchanged;
    });
}
using ProfileDecisions = std::map<std::string, std::optional<AssemblyProfileDimensionTarget>, std::less<>>;
Json encode_profile_selector(const AssemblyProfileDimensionTarget& selector) {
    AssemblyDimensionTarget target{{"catalog", "instance", std::nullopt, {}}, AssemblyDimensionAxis::width, selector};
    auto result = encode_assembly_dimension_target(target);
    for (const auto* field : {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path"})
        result.erase(field);
    return result;
}
AssemblyProfileDimensionTarget decode_profile_selector(const Json& selector) {
    if (!selector.is_object() || !selector.contains("measurement")) reject("invalid closed profile selector");
    if (selector.at("measurement") == "edge_length")
        fields(selector, {"profile_id", "ring_id", "measurement", "segment_id"});
    else fields(selector, {"profile_id", "ring_id", "measurement", "start_vertex_id", "end_vertex_id"});
    auto wire = selector;
    wire["version"] = 2; wire["form"] = "assembly_profile_dimension_target";
    wire["catalog_id"] = "catalog"; wire["instance_id"] = "instance";
    wire["document_entity_id"] = nullptr; wire["part_path"] = Json::array();
    return *decode_assembly_dimension_target(wire).profile;
}
std::set<Target> reviewed_profile_nodes(std::span<const AssemblyInstancePropertyEditIntent> intents) {
    std::set<Target> result;
    for (const auto& intent : intents) if (intent.review_profile_topology) result.insert(intent.target);
    return result;
}
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
        (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4 && value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7)) reject("unsupported authoring version");
    const bool reviewed_topology = value.at("version") == 7;
    const bool initialize_topology = value.at("version") == 6;
    const bool topology_geometry = value.at("version") == 5;
    const bool geometry = value.at("version") == 3 || topology_geometry || initialize_topology || reviewed_topology;
    const bool complete_dimensions = value.at("version") == 4;
    const bool dimensions = geometry || complete_dimensions;
    const bool carry = value.at("version") != 1;
    if (carry) {
        if (complete_dimensions) {
            fields(value, {"version", "intents", "allocation", "carry_presentation_references",
                "presentation_overlay_identities", "complete_component_dimensions", "dimension_identities"});
            if (!value.at("complete_component_dimensions").is_boolean() || value.at("complete_component_dimensions") != true)
                reject("v4 requires literal component dimension completion");
        } else if (reviewed_topology) fields(value, {"version", "intents", "allocation", "carry_presentation_references",
            "presentation_overlay_identities", "dimension_identities", "profile_dimension_decisions"});
        else if (geometry) fields(value, {"version", "intents", "allocation", "carry_presentation_references",
            "presentation_overlay_identities", "dimension_identities"});
        else fields(value, {"version", "intents", "allocation", "carry_presentation_references", "presentation_overlay_identities"});
        if (!value.at("carry_presentation_references").is_boolean() || value.at("carry_presentation_references") != true)
            reject("v2/v3/v4/v5/v6 require literal presentation carry");
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
        if (dimensions) {
            const auto& identities = value.at("dimension_identities");
            if (!identities.is_object() || identities.size() > count_limit) reject("invalid dimension allocation count");
            fresh_count += identities.size();
        }
        for (const auto& row : roots) {
            fields(row, {"root", "proposed_entity_id", "proposed_catalog_id", "proposed_instance_id"});
            fresh_count += row.at("proposed_entity_id").is_null() ? 3 : 1;
        }
        if (fresh_count > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    }
    PhaseAssemblyInstancePropertyEditAuthoring result;
    result.carry_presentation_references = carry;
    result.complete_component_dimensions = complete_dimensions;
    bool has_topology_geometry = false;
    bool has_initial_topology = false;
    bool has_reviewed_topology = false;
    for (const auto& row : intents) {
        auto intent = decode_assembly_instance_property_edit_intent(row);
        if (row.at("version") == 5) {
            if (!reviewed_topology) reject("historical authoring cannot carry reviewed profile topology authority");
            has_reviewed_topology = true;
        }
        if (row.at("version") == 4) {
            if (!initialize_topology && !reviewed_topology) reject("historical authoring cannot carry initial profile topology authority");
            has_initial_topology = true;
        }
        if (row.at("version") == 3) {
            if (!topology_geometry && !initialize_topology && !reviewed_topology) reject("historical authoring cannot carry topology-preserving geometry authority");
            has_topology_geometry = true;
        }
        if (!geometry && intent.profile_geometry.operation != AssemblyInstancePropertyPatchOperation::unchanged)
            reject("nongeometry authoring cannot carry profile geometry authority");
        if (!result.intents.empty() && !(result.intents.back().target < intent.target)) reject("intent targets must be sorted unique");
        result.intents.push_back(std::move(intent));
    }
    if (geometry && !has_geometry_intent(result)) reject("v3/v5/v6 require a geometry intent");
    if (topology_geometry && !has_topology_geometry) reject("v5 requires a topology-preserving geometry intent");
    if (initialize_topology && !has_initial_topology) reject("v6 requires an initial profile topology intent");
    if (reviewed_topology && !has_reviewed_topology) reject("v7 requires a reviewed profile topology intent");
    if (reviewed_topology) {
        const auto& decisions = value.at("profile_dimension_decisions");
        if (!decisions.is_object() || decisions.size() > count_limit) reject("invalid profile dimension decision count");
        for (const auto& [id, row] : decisions.items()) {
            admit_identity(id); fields(row, {"operation", "destination"});
            if (row.at("operation") == "retire" && row.at("destination").is_null())
                result.profile_dimension_decisions.emplace(id, std::nullopt);
            else if (row.at("operation") == "remap" && !row.at("destination").is_null())
                result.profile_dimension_decisions.emplace(id, decode_profile_selector(row.at("destination")));
            else reject("invalid profile dimension decision operation/destination");
        }
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
        if (dimensions) for (const auto& [source, destination] : value.at("dimension_identities").items()) {
            admit_identity(source);
            result.dimension_identities.emplace(source, reserve(destination));
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
    if (!value.at("version").is_number_integer() || (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4 && value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7))
        reject("unsupported request version");
    (void)revision(value.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(name));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    auto authoring = decode_authoring(value.at("authoring"));
    if (value.at("version") != value.at("authoring").at("version"))
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
    // Admit the complete actual catalogs before masking codec-owned local IDs.
    AssemblyCatalogTransferBudget catalog_budget;
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_budget);
    }
    validate_embedded_assembly_drawing_contexts(source);
}
Target component_root(Target target) { target.part_path.clear(); return target; }
struct DimensionRoster {
    std::map<std::string, Target, std::less<>> components;
    std::set<std::string, std::less<>> copies;
    std::map<std::string, AssemblyDimensionTarget, std::less<>> targets;
};
DimensionRoster inspect_dimension_roster(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditPlan& plan, bool measure) {
    // Whole raw admission precedes even a malformed/future target scan. The
    // enclosing plan has separately admitted every actual source catalog.
    admit_presentation_source(source);
    std::set<Target> roots, replacements;
    std::map<Target, const PhaseAssemblyInstanceReplacementRoot*> cohorts;
    std::set<std::string, std::less<>> catalogs, carriers, independent_carriers;
    const auto add = [&](const Target& root) {
        roots.insert(root); catalogs.insert(root.catalog_id);
        carriers.insert(root.document_entity_id.value_or(root.catalog_id));
        if (root.document_entity_id) independent_carriers.insert(*root.document_entity_id);
    };
    for (const auto& root : plan.ordinary_roots) add(root);
    for (const auto& row : plan.replacement_roots) {
        add(row.root); replacements.insert(row.root); cohorts.emplace(row.root, &row);
    }
    const auto field = [](const Json& value, const char* key) -> const Json* {
        if (!value.is_object()) return nullptr;
        const auto found = value.find(key); return found == value.end() ? nullptr : &*found;
    };
    DimensionRoster result;
    std::vector<AssemblyDimensionTarget> targets;
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto* target = field(entity.properties, "target");
        const auto* carrier = target ? field(*target, "entity_id") : nullptr;
        const bool selected_carrier = carrier && carrier->is_string() &&
            carriers.contains(carrier->get_ref<const std::string&>());
        const auto* assembly = target ? field(*target, "assembly") : nullptr;
        const auto* catalog = assembly ? field(*assembly, "catalog_id") : nullptr;
        const bool selected_catalog = catalog && catalog->is_string() &&
            catalogs.contains(catalog->get_ref<const std::string&>());
        if (!selected_catalog) {
            if (selected_carrier) reject("affected dimension has no actual qualified catalog target");
            continue;
        }
        const auto* instance = field(*assembly, "instance_id");
        const auto* owner = field(*assembly, "document_entity_id");
        if (!instance || !instance->is_string() || !owner || (!owner->is_null() && !owner->is_string()))
            reject("affected dimension has malformed qualified root fields");
        Target root{catalog->get_ref<const std::string&>(), instance->get_ref<const std::string&>(),
            owner->is_null() ? std::nullopt : std::optional<std::string>{owner->get_ref<const std::string&>()}, {}};
        if (!roots.contains(root)) {
            if (selected_carrier && independent_carriers.contains(carrier->get_ref<const std::string&>()))
                reject("affected independent dimension carrier and target disagree");
            continue;
        }
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || (decoded.version != 6 && decoded.version != 7) || !decoded.dimension ||
            !is_assembly_boundary_dimension_kind(decoded.dimension->kind) || !decoded.dimension->assembly_target)
            reject("affected geometry dimension requires a supported assembly dimension");
        const auto& dimension = *decoded.dimension;
        if (component_root(dimension.assembly_target->component) != root ||
            dimension.boundary_id != root.document_entity_id.value_or(root.catalog_id))
            reject("affected dimension carrier differs from its actual qualified target");
        if (result.components.size() >= count_limit) reject("complete dimension association count exceeds bound");
        targets.push_back(*dimension.assembly_target);
        result.components.emplace(id, dimension.assembly_target->component);
        result.targets.emplace(id, *dimension.assembly_target);
        if (replacements.contains(root)) result.copies.insert(id);
    }
    if (!result.copies.empty()) {
        const auto scope = constraint_phase_scope(source);
        for (const auto& id : result.copies) if (scope.inactive_owner_ids.contains(id))
            reject("cannot copy an inactive source assembly dimension");
        // Decode each registry once, rather than once per association. Copy
        // authority follows the root's actual cohort and never demolishes or
        // speculatively enrolls an existing dimension from another choice.
        for (const auto& [registry_id, entity] : source) if (entity.type == "model_phases") {
            const auto phases = ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& id : phases.entity_ids()) if (result.copies.contains(id)) {
                const auto& cohort = *cohorts.at(component_root(result.components.at(id)));
                if (registry_id != cohort.registry_id || phases.active_alternative() != std::optional<std::string>{cohort.alternative_id} ||
                    std::find(phases.baseline_ids().begin(), phases.baseline_ids().end(), id) == phases.baseline_ids().end())
                    reject("source assembly dimension belongs to an incompatible registered baseline cohort");
            }
        }
    }
    if (measure && !targets.empty()) (void)resolve_assembly_dimensions(source, targets);
    return result;
}
void validate_profile_decisions(const DimensionRoster& dimensions,
    std::span<const AssemblyInstancePropertyEditIntent> intents, const ProfileDecisions& decisions) {
    const auto reviewed = reviewed_profile_nodes(intents);
    std::size_t required = 0;
    for (const auto& [id, target] : dimensions.targets) if (target.profile && reviewed.contains(target.component)) {
        ++required;
        const auto decision = decisions.find(id);
        if (decision == decisions.end()) reject("missing actual reviewed profile dimension decision");
        if (decision->second) {
            (void)encode_profile_selector(*decision->second);
            if (decision->second->measurement != target.profile->measurement)
                reject("reviewed profile dimension changed its measurement kind");
        }
    }
    if (required != decisions.size()) reject("profile dimension decisions differ from exact reviewed source roster");
}
PresentationIds retired_profile_dimensions(const ProfileDecisions& decisions) {
    PresentationIds result;
    for (const auto& [id, destination] : decisions) if (!destination) result.insert(id);
    return result;
}
DimensionRoster dimension_carry_roster(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditPlan& plan, std::span<const AssemblyInstancePropertyEditIntent> intents,
    const ProfileDecisions& decisions, bool measure) {
    auto dimensions = inspect_dimension_roster(source, plan, measure);
    validate_profile_decisions(dimensions, intents, decisions);
    for (const auto& id : retired_profile_dimensions(decisions)) dimensions.copies.erase(id);
    return dimensions;
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
PresentationIds presentation_overlay_ids(const AssemblyDocumentEntities& source, const PresentationIds& owners,
    const PresentationIds& excluded_dimensions = {}) {
    PresentationIds ids;
    std::map<std::string, std::size_t, std::less<>> occurrences;
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType) {
        (void)id; (void)decode_sheet_view_entity(entity);
        for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
            for (const auto& overlay : view.at("overlays")) {
                const auto overlay_id = original_overlay_identity(overlay.at("id"));
                ++occurrences[overlay_id];
                if (affected_overlay(overlay, owners) && !affected_overlay(overlay, excluded_dimensions)) {
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
Entity opaque_presentation_remainder(Entity entity, const DimensionRoster* dimensions = nullptr) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model")); p.erase("model");
        if (p.contains("embedded_instance_drawing_contexts"))
            for (auto& row : p.at("embedded_instance_drawing_contexts").at("rows")) row.erase("instance_id");
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
    } else if (dimensions && dimensions->components.contains(entity.id)) {
        // Only the inventoried supported assembly carrier is understood.
        // Profile/edge anchor identities never gain remapping authority;
        // opaque envelope/target/extension siblings remain fenced below.
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || (decoded.version != 6 && decoded.version != 7) ||
            !is_assembly_boundary_dimension_kind(decoded.dimension->kind) || !decoded.dimension->assembly_target)
            reject("dimension dependency remainder differs from actual supported roster");
        auto& target = p.at("target"); target.erase("entity_id");
        auto& assembly = target.at("assembly");
        for (const auto* name : {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path", "axis"})
            assembly.erase(name);
    }
    return entity;
}
Target actual_dimension_destination(const PhaseAssemblyInstancePropertyEditResult& result, const Target& root) {
    std::optional<Target> destination;
    for (const auto& [source, proposed] : result.original_to_proposed) {
        if (component_root(source) != root) continue;
        if (source.part_path != proposed.part_path) reject("dimension destination changed an actual selected path");
        const auto proposed_root = component_root(proposed);
        if (destination && *destination != proposed_root) reject("dimension root has conflicting actual destinations");
        destination = proposed_root;
    }
    if (!destination) reject("dimension root has no actual replay destination");
    return *destination;
}
void complete_dimension_carry(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring, PhaseAssemblyInstancePropertyEditResult& result) {
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, authoring.intents);
    const auto dimensions = dimension_carry_roster(source, plan, authoring.intents, authoring.profile_dimension_decisions, false);
    if (dimensions.copies.size() != authoring.dimension_identities.size())
        reject("dimension allocation differs from the actual replacement roster");
    for (const auto& id : dimensions.copies) if (!authoring.dimension_identities.contains(id))
        reject("missing actual replacement dimension allocation");
    if (result.fresh_identity_ids.size() > presentation_identity_limit ||
        authoring.dimension_identities.size() > presentation_identity_limit - result.fresh_identity_ids.size() ||
        authoring.presentation_overlay_identities.size() > presentation_identity_limit -
            result.fresh_identity_ids.size() - authoring.dimension_identities.size())
        reject("complete fresh geometry inventory exceeds bound");
    PresentationIds fresh(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
    for (const auto& [original, proposed] : authoring.dimension_identities) {
        (void)original;
        if (!fresh.insert(proposed).second) reject("fresh dimension collides with another replacement identity");
        result.fresh_identity_ids.push_back(proposed);
    }
    validate_phase_assembly_instance_property_edit_fresh_identities(source, result.fresh_identity_ids);
    std::map<Target, const PhaseAssemblyInstanceReplacementRoot*> replacements;
    for (const auto& row : plan.replacement_roots) replacements.emplace(row.root, &row);
    std::map<Target, std::size_t> request_roots;
    std::vector<AssemblyDimensionReviewedTransformRequest> requests;
    const auto reviewed_nodes = reviewed_profile_nodes(authoring.intents);
    std::map<std::string, const PhaseAssemblyInstanceReplacementRoot*, std::less<>> copy_cohorts;
    for (const auto& [id, component] : dimensions.components) {
        const auto root = component_root(component);
        const auto replacement = replacements.find(root);
        const bool clone = replacement != replacements.end();
        const bool retired = authoring.profile_dimension_decisions.contains(id) && !authoring.profile_dimension_decisions.at(id);
        if (clone && !retired) {
            const auto& cohort = *replacement->second;
            copy_cohorts.emplace(authoring.dimension_identities.at(id), &cohort);
        }
        const auto [request, inserted] = request_roots.emplace(root, requests.size());
        if (inserted) {
            AssemblyDimensionReviewedTransformRequest reviewed;
            reviewed.transform = {root, actual_dimension_destination(result, root),
                clone ? AssemblyDimensionTransformMode::clone : AssemblyDimensionTransformMode::move, {}};
            for (const auto& node : reviewed_nodes)
                if (component_root(node) == root) reviewed.reviewed_profile_nodes.push_back(node);
            requests.push_back(std::move(reviewed));
        }
        auto& reviewed = requests.at(request->second);
        if (const auto decision = authoring.profile_dimension_decisions.find(id); decision != authoring.profile_dimension_decisions.end())
            reviewed.profile_dimension_decisions.emplace(*decision);
        if (clone && !retired) reviewed.transform.fresh_dimension_ids.emplace(id, authoring.dimension_identities.at(id));
    }
    // One shared source/final measurement batch. This admits complete qualified
    // targets and actual final extents, retaining nested paths and axes exactly.
    AssemblyDimensionReviewedCarryResult carried;
    if (!requests.empty()) {
        if (std::none_of(requests.begin(), requests.end(), [](const auto& request) {
                return !request.reviewed_profile_nodes.empty();
            })) {
            std::vector<AssemblyDimensionTransformRequest> historical;
            for (const auto& request : requests) historical.push_back(request.transform);
            carried.upserts = carry_assembly_dimension_transform_entities(source, result.entities, historical);
        } else carried = carry_reviewed_assembly_dimension_transform_entities(source, result.entities, requests);
    }
    const auto& contribution = carried.upserts;
    result.retired_dimension_ids = carried.retired_source_dimension_ids;
    std::vector<std::string> expected_retired;
    for (const auto& [id, selector] : authoring.profile_dimension_decisions)
        if (!selector && !replacements.contains(component_root(dimensions.components.at(id)))) expected_retired.push_back(id);
    if (result.retired_dimension_ids != expected_retired)
        reject("ordinary retirement contribution differs from exact reviewed source decisions");
    validate_ordinary_assembly_dimension_changes(source, result.retired_dimension_ids);
    if (!result.retired_dimension_ids.empty()) {
        // Only actual source replay may derive typed reference cleanup. Compose
        // its exact row delta with the body contribution when the rows agree.
        const auto removed = replay_drawing_selection_removal(source, {result.retired_dimension_ids, {}}, true);
        for (const auto& [id, before] : source) {
            const auto after = removed.find(id);
            if (after == removed.end() && !std::binary_search(result.retired_dimension_ids.begin(), result.retired_dimension_ids.end(), id))
                reject("ordinary profile dimension cleanup removed an unreviewed source owner");
            if (after != removed.end() && after->second == before &&
                after->second.properties.dump() == before.properties.dump() && after->second.extensions.dump() == before.extensions.dump()) continue;
            const auto candidate = result.entities.find(id);
            if (candidate == result.entities.end() || candidate->second != before ||
                candidate->second.properties.dump() != before.properties.dump() || candidate->second.extensions.dump() != before.extensions.dump())
                reject("ordinary profile dimension cleanup overlaps component body changes");
            if (after == removed.end()) result.entities.erase(candidate);
            else candidate->second = after->second;
        }
    }
    std::vector<std::string> moved;
    PresentationIds copied;
    for (const auto& entity : contribution) {
        if (copy_cohorts.contains(entity.id)) copied.insert(entity.id);
        else moved.push_back(entity.id);
    }
    validate_ordinary_assembly_dimension_changes(source, moved);
    if (copied.size() != dimensions.copies.size()) reject("dimension contribution differs from actual copy roster");
    for (const auto& entity : contribution) {
        if (copy_cohorts.contains(entity.id)) {
            if (!result.entities.emplace(entity.id, entity).second) reject("dimension clone collides with a final owner");
        } else result.entities.at(entity.id) = entity;
    }
    std::map<std::string, PresentationIds, std::less<>> registry_copies;
    for (const auto& [id, cohort] : copy_cohorts) registry_copies[cohort->registry_id].insert(id);
    for (const auto& [registry_id, ids] : registry_copies) {
        auto& entity = result.entities.at(registry_id);
        const auto phases = ModelPhases::from_json(entity.properties.at("model"));
        const auto& alternative_id = copy_cohorts.at(*ids.begin())->alternative_id;
        if (phases.active_alternative() != std::optional<std::string>{alternative_id})
            reject("dimension copies lost the actual saved registry alternative");
        auto owners = phases.entity_ids(); auto alternatives = phases.alternatives();
        const auto active = std::find_if(alternatives.begin(), alternatives.end(),
            [&](const auto& row) { return row.id == alternative_id; });
        if (active == alternatives.end()) reject("dimension copies have no actual registry alternative");
        auto raw = entity.properties.at("model");
        for (const auto& id : ids) { owners.push_back(id); active->proposed_ids.push_back(id); raw.at("entity_ids").push_back(id); }
        for (auto& row : raw.at("alternatives")) if (row.at("id") == alternative_id)
            for (const auto& id : ids) row.at("proposed_ids").push_back(id);
        const auto expected = ModelPhases::create(owners, phases.baseline_ids(), alternatives, phases.active_alternative());
        if (ModelPhases::from_json(raw).to_json() != expected.to_json()) reject("dimension registry append differs from typed admission");
        entity.properties.at("model") = std::move(raw);
    }
    for (const auto& [id, component] : dimensions.components) if (replacements.contains(component_root(component))) {
        const auto& before = source.at(id); const auto& after = result.entities.at(id);
        if (before != after || before.properties.dump() != after.properties.dump() || before.extensions.dump() != after.extensions.dump())
            reject("dimension copy changed its exact original source envelope");
    }
}
void complete_presentation_carry(const AssemblyDocumentEntities& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring, PhaseAssemblyInstancePropertyEditResult& result) {
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, authoring.intents);
    auto owners = replacement_presentation_owners(source, plan);
    const bool carry_dimensions = has_geometry_intent(authoring) || authoring.complete_component_dimensions;
    const auto dimensions = carry_dimensions ? dimension_carry_roster(source, plan, authoring.intents,
        authoring.profile_dimension_decisions, false) : DimensionRoster{};
    const auto retired = retired_profile_dimensions(authoring.profile_dimension_decisions);
    owners.insert(dimensions.copies.begin(), dimensions.copies.end());
    const auto overlays = presentation_overlay_ids(source, owners, retired);
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
    if (carry_dimensions) for (const auto& id : dimensions.copies) {
        const auto found = authoring.dimension_identities.find(id);
        if (found == authoring.dimension_identities.end() || !result.entities.contains(found->second) ||
            !mappings.emplace(id, found->second).second) reject("missing actual dimension presentation destination");
    }
    PresentationIds affected = owners;
    affected.insert(overlays.begin(), overlays.end());
    for (const auto& [id, entity] : source) {
        if (std::binary_search(result.retired_dimension_ids.begin(), result.retired_dimension_ids.end(), id)) continue;
        auto remainder = opaque_presentation_remainder(entity, carry_dimensions ? &dimensions : nullptr);
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
        if (std::binary_search(result.retired_dimension_ids.begin(), result.retired_dimension_ids.end(), id)) continue;
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
                    if (affected_overlay(original_row, owners) && !affected_overlay(original_row, retired)) {
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
    const AssemblyDocumentEntities& source, std::span<const AssemblyInstancePropertyEditIntent> intents,
    bool complete_component_dimensions, const ProfileDecisions& decisions) {
    admit_presentation_source(source);
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, intents);
    auto owners = replacement_presentation_owners(source, plan);
    if (complete_component_dimensions || std::any_of(intents.begin(), intents.end(), [](const auto& intent) {
        return intent.profile_geometry.operation != AssemblyInstancePropertyPatchOperation::unchanged;
    })) {
        const auto dimensions = dimension_carry_roster(source, plan, intents, decisions, true);
        owners.insert(dimensions.copies.begin(), dimensions.copies.end());
    } else if (!decisions.empty()) reject("historical presentation inventory cannot carry profile decisions");
    const auto ids = presentation_overlay_ids(source, owners, retired_profile_dimensions(decisions));
    return {ids.begin(), ids.end()};
}
std::map<std::string, Target, std::less<>> inspect_phase_assembly_instance_property_dimension_clone_roster(
    const AssemblyDocumentEntities& source, std::span<const AssemblyInstancePropertyEditIntent> intents,
    const ProfileDecisions& decisions) {
    admit_presentation_source(source);
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, intents);
    const auto dimensions = dimension_carry_roster(source, plan, intents, decisions, true);
    std::map<std::string, Target, std::less<>> result;
    for (const auto& id : dimensions.copies) result.emplace(id, dimensions.components.at(id));
    return result;
}
std::map<std::string, AssemblyDimensionTarget, std::less<>> inspect_phase_assembly_instance_profile_dimension_review_roster(
    const AssemblyDocumentEntities& source, std::span<const AssemblyInstancePropertyEditIntent> intents) {
    const auto plan = inspect_phase_assembly_instance_property_edit_plan(source, intents);
    const auto dimensions = inspect_dimension_roster(source, plan, true);
    const auto nodes = reviewed_profile_nodes(intents);
    std::map<std::string, AssemblyDimensionTarget, std::less<>> result;
    for (const auto& [id, target] : dimensions.targets)
        if (target.profile && nodes.contains(target.component)) result.emplace(id, target);
    return result;
}

Json encode_phase_assembly_instance_property_edit_authoring(const PhaseAssemblyInstancePropertyEditAuthoring& authoring) {
    if (authoring.intents.empty() || authoring.intents.size() > count_limit || authoring.allocation.roots.size() > count_limit ||
        authoring.allocation.membership_entity_ids.size() > count_limit) reject("typed request count exceeds bound");
    const bool geometry = has_geometry_intent(authoring);
    const bool dimensions = geometry || authoring.complete_component_dimensions;
    if (geometry && authoring.complete_component_dimensions) reject("v4 component dimension completion excludes profile geometry");
    if (dimensions && !authoring.carry_presentation_references) reject("dimension authoring requires literal presentation carry");
    if ((!dimensions && !authoring.dimension_identities.empty()) || authoring.dimension_identities.size() > count_limit)
        reject("dimension identities require geometry v3/v5/v6 or explicit v4 and at most 1000 copies");
    if (!authoring.carry_presentation_references && !authoring.presentation_overlay_identities.empty())
        reject("historical v1 cannot allocate presentation overlays");
    if (authoring.carry_presentation_references) {
        if (authoring.presentation_overlay_identities.size() > presentation_identity_limit) reject("invalid overlay allocation count");
        std::size_t fresh_count = authoring.allocation.membership_entity_ids.size() + authoring.presentation_overlay_identities.size() +
            authoring.dimension_identities.size();
        for (const auto& [root, ids] : authoring.allocation.roots) {
            (void)ids; fresh_count += root.document_entity_id ? 1 : 3;
        }
        if (fresh_count > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    }
    Json intents = Json::array(), roots = Json::array(), memberships = Json::array(), overlays = Json::array();
    WireBudget budget;
    bool topology_geometry = false;
    bool initialize_topology = false;
    bool reviewed_topology = false;
    for (const auto& intent : authoring.intents) {
        auto wire = encode_assembly_instance_property_edit_intent(intent);
        topology_geometry = topology_geometry || wire.at("version") == 3;
        initialize_topology = initialize_topology || wire.at("version") == 4;
        reviewed_topology = reviewed_topology || wire.at("version") == 5;
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
    if ((!reviewed_topology && !authoring.profile_dimension_decisions.empty()) || authoring.profile_dimension_decisions.size() > count_limit)
        reject("profile dimension decisions require reviewed topology and at most 1000 associations");
    Json result{{"version", reviewed_topology ? 7 : initialize_topology ? 6 : topology_geometry ? 5 : authoring.complete_component_dimensions ? 4 : geometry ? 3 : authoring.carry_presentation_references ? 2 : 1}, {"intents", std::move(intents)},
        {"allocation", {{"roots", std::move(roots)}, {"membership_entity_ids", std::move(memberships)}}}};
    if (authoring.carry_presentation_references) {
        result["carry_presentation_references"] = true;
        result["presentation_overlay_identities"] = std::move(overlays);
    }
    if (authoring.complete_component_dimensions) result["complete_component_dimensions"] = true;
    if (dimensions) {
        auto identities = Json::object();
        for (const auto& [source, destination] : authoring.dimension_identities) {
            admit_identity(source); admit_identity(destination);
            budget.string(source); budget.string(destination);
            identities[source] = destination;
        }
        result["dimension_identities"] = std::move(identities);
    }
    if (reviewed_topology) {
        auto decisions = Json::object();
        for (const auto& [id, selector] : authoring.profile_dimension_decisions) {
            admit_identity(id);
            decisions[id] = {{"operation", selector ? "remap" : "retire"},
                {"destination", selector ? encode_profile_selector(*selector) : Json(nullptr)}};
        }
        result["profile_dimension_decisions"] = std::move(decisions);
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
    const auto version = wire.at("version");
    Json request{{"version", version}, {"expected_revision", source.revision()},
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
    if (has_geometry_intent(authoring) || authoring.complete_component_dimensions)
        complete_dimension_carry(source.entities(), authoring, result);
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
