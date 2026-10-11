#include "sketch/phase_assembly_instance_property_edit.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"

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
    fields(value, {"version", "intents", "allocation"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1) reject("unsupported authoring version");
    const auto& intents = value.at("intents");
    if (!intents.is_array() || intents.empty() || intents.size() > count_limit) reject("invalid intent count");
    PhaseAssemblyInstancePropertyEditAuthoring result;
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
    if (!value.at("version").is_number_integer() || value.at("version") != 1) reject("unsupported request version");
    (void)revision(value.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(name));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    return decode_authoring(value.at("authoring"));
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
} // namespace

Json encode_phase_assembly_instance_property_edit_authoring(const PhaseAssemblyInstancePropertyEditAuthoring& authoring) {
    if (authoring.intents.empty() || authoring.intents.size() > count_limit || authoring.allocation.roots.size() > count_limit ||
        authoring.allocation.membership_entity_ids.size() > count_limit) reject("typed request count exceeds bound");
    Json intents = Json::array(), roots = Json::array(), memberships = Json::array();
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
    Json result{{"version", 1}, {"intents", std::move(intents)},
        {"allocation", {{"roots", std::move(roots)}, {"membership_entity_ids", std::move(memberships)}}}};
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
    Json request{{"version", 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", optional(source.saved_revision_optional())},
        {"authoring", encode_phase_assembly_instance_property_edit_authoring(authoring)}};
    return replay_phase_assembly_instance_property_edit_preparation(source, request);
}
PhaseAssemblyInstancePropertyEditPreparation replay_phase_assembly_instance_property_edit_preparation(const DocumentSnapshot& source,
    const Json& request) {
    const auto authoring = decode_request(request);
    authenticate(source, request);
    (void)Document::fork(source); // Actual complete history/assets/read-only admission.
    auto result = replay_phase_assembly_instance_property_edits(source.entities(), authoring.intents, authoring.allocation);
    // Includes undone states. Every allocated ID becomes live in this dialect;
    // exact no-ops cannot carry speculative/unpublished allocations.
    for (const auto& record : source.history()) {
        validate_phase_assembly_instance_property_edit_fresh_identities(record.entities, result.fresh_identity_ids);
        for (const auto& id : result.fresh_identity_ids) if (record.assets.contains(id)) reject("fresh identity reuses retained asset identity");
    }
    // No raw ApplyEntityChanges preview: the enclosing typed composer must
    // complete dependencies and final Document admission before publication.
    return {request, std::move(result)};
}
void require_phase_assembly_instance_property_edit_source_current(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditPreparation& preparation) {
    (void)decode_request(preparation.request()); authenticate(source, preparation.request());
}

} // namespace sketch
