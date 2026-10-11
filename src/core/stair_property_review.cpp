#include "sketch/stair_property_review.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"

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
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024, node_limit = 100000;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Stair Properties review: ") + reason);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 128 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid actual source identity");
    return text;
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid source revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("source binding must be canonical SHA-256");
}
// Borrowed-input accounting precedes dumps, copies and nested codec traversal.
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void quoted(std::string_view value) {
        add(2);
        for (const auto c : value) add(c == '"' || c == '\\' ? 2 :
            static_cast<unsigned char>(c) < 0x20 ? 6 : 1);
    }
    void visit(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("JSON complexity budget exceeded");
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); visit(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) visit(child, depth + 1);
        } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
void source_metadata(const DocumentSnapshot& source) {
    if (!source.is_editable()) reject("captured source is read-only");
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() ||
        source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("captured source history or save metadata is invalid");
}
void authenticate(const DocumentSnapshot& source, const StairPropertyReviewRequest& request) {
    source_metadata(source);
    if (request.expected_revision != source.revision() ||
        request.source_saved_revision != source.saved_revision_optional() ||
        request.source_snapshot_digest != document_snapshot_digest(source) ||
        request.source_authoring_digest != document_authoring_source_digest_v2(source) ||
        request.source_entities_digest != entity_map_digest(source.entities()))
        reject("captured complete source authority changed");
}
std::vector<Entity> changed_editors(const DocumentSnapshot& source,
    const std::vector<StairProfileEditIntent>& profiles) {
    if (profiles.empty() || profiles.size() > maximum_architectural_group_targets)
        reject("requires 1..1000 actual scalar owners");
    std::string previous;
    for (const auto& profile : profiles) {
        if (profile.object_id <= previous) reject("scalar owners must be distinct and ascending");
        previous = profile.object_id;
    }
    const auto edited = prepare_stair_profile_edit_entities(source.entities(), profiles);
    std::vector<Entity> changed;
    for (const auto& entity : edited)
        if (!exact(source.entities().at(entity.id), entity)) changed.push_back(entity);
    return changed;
}
} // namespace

Json encode_stair_property_review_request(const StairPropertyReviewRequest& request) {
    digest(request.source_snapshot_digest); digest(request.source_authoring_digest);
    digest(request.source_entities_digest);
    if (request.profiles.empty() || request.profiles.size() > maximum_architectural_group_targets ||
        request.dependency_dispositions.size() > 128) reject("owner or decision budget exceeded");
    if (request.message.empty() || request.message.size() > 4096) reject("message requires 1..4096 bytes");
    Json profiles = Json::array(), decisions = Json::array();
    std::string previous;
    for (const auto& profile : request.profiles) {
        if (profile.object_id <= previous) reject("scalar owners must be distinct and ascending");
        previous = profile.object_id;
        profiles.push_back(encode_stair_profile_edit_intent(profile));
    }
    previous.clear();
    for (const auto& decision : request.dependency_dispositions) {
        (void)identity(decision.rail_id);
        if (decision.rail_id <= previous) reject("rail decisions must be distinct and ascending");
        previous = decision.rail_id;
        if (decision.action == PhaseStairReplacementDependencyAction::retire) {
            if (!decision.target_key.empty()) reject("retirement cannot carry a target");
            decisions.push_back({{"rail_id", decision.rail_id}, {"action", "retire"}});
        } else if (decision.action == PhaseStairReplacementDependencyAction::rehost) {
            if (decision.target_key.empty() || decision.target_key.size() > 256) reject("invalid rehost target key");
            decisions.push_back({{"rail_id", decision.rail_id}, {"action", "rehost"}, {"target_key", decision.target_key}});
        } else reject("unsupported dependency action");
    }
    Json result{{"version", 1}, {"expected_revision", request.expected_revision},
        {"source_snapshot_digest", request.source_snapshot_digest}, {"source_authoring_digest", request.source_authoring_digest},
        {"source_entities_digest", request.source_entities_digest},
        {"source_saved_revision", request.source_saved_revision ? Json(*request.source_saved_revision) : Json(nullptr)},
        {"profiles", std::move(profiles)}, {"dependency_dispositions", std::move(decisions)}, {"message", request.message}};
    Budget{}.visit(result);
    return result;
}

StairPropertyReviewRequest decode_stair_property_review_request(const Json& value) try {
    Budget{}.visit(value);
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "profiles", "dependency_dispositions", "message"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1) reject("unsupported request version");
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(name));
    if (!value.at("profiles").is_array() || value.at("profiles").empty() ||
        value.at("profiles").size() > maximum_architectural_group_targets ||
        !value.at("dependency_dispositions").is_array() || value.at("dependency_dispositions").size() > 128)
        reject("requires a bounded scalar roster and decision array");
    if (!value.at("message").is_string()) reject("message must be a string");
    StairPropertyReviewRequest result;
    result.expected_revision = revision(value.at("expected_revision"));
    result.source_snapshot_digest = value.at("source_snapshot_digest").get<std::string>();
    result.source_authoring_digest = value.at("source_authoring_digest").get<std::string>();
    result.source_entities_digest = value.at("source_entities_digest").get<std::string>();
    if (!value.at("source_saved_revision").is_null()) result.source_saved_revision = revision(value.at("source_saved_revision"));
    result.message = value.at("message").get<std::string>();
    for (const auto& profile : value.at("profiles")) result.profiles.push_back(decode_stair_profile_edit_intent(profile));
    for (const auto& decision : value.at("dependency_dispositions")) {
        if (!decision.is_object() || !decision.contains("action")) reject("missing dependency action");
        PhaseStairReplacementDependencyDisposition decoded;
        if (decision.at("action") == "retire") {
            fields(decision, {"rail_id", "action"}); decoded.action = PhaseStairReplacementDependencyAction::retire;
        } else if (decision.at("action") == "rehost") {
            fields(decision, {"rail_id", "action", "target_key"});
            if (!decision.at("target_key").is_string()) reject("target key must be a string");
            decoded.action = PhaseStairReplacementDependencyAction::rehost;
            decoded.target_key = decision.at("target_key").get<std::string>();
        } else reject("unsupported dependency action");
        decoded.rail_id = identity(decision.at("rail_id"));
        result.dependency_dispositions.push_back(std::move(decoded));
    }
    if (encode_stair_property_review_request(result).dump() != value.dump()) reject("request must use its exact canonical codec");
    return result;
} catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("Stair Properties review: malformed request: ") + error.what());
}

PhaseStairReplacementDependencyPlan inspect_stair_property_review_dependencies(
    const DocumentSnapshot& source, const std::vector<StairProfileEditIntent>& profiles) {
    try {
        source_metadata(source);
        const auto edited = changed_editors(source, profiles);
        return edited.empty() ? PhaseStairReplacementDependencyPlan{} :
            inspect_ordinary_stair_edit_dependencies(source.entities(), edited);
    } catch (const std::exception& error) {
        PhaseStairReplacementDependencyPlan result;
        result.diagnostics.push_back({{}, error.what(), true}); return result;
    }
}

StairPropertyReviewPreparation::StairPropertyReviewPreparation(Json request, ApplyEntityChanges command,
    Entities entities, std::vector<std::string> retired, std::vector<std::string> erased)
    : request_(std::move(request)), command_(std::move(command)), entities_(std::move(entities)),
      retired_rails_(std::move(retired)), erased_entities_(std::move(erased)) {}
const Json& StairPropertyReviewPreparation::request() const noexcept { return request_; }
const ApplyEntityChanges& StairPropertyReviewPreparation::command() const noexcept { return command_; }
const Entities& StairPropertyReviewPreparation::entities() const noexcept { return entities_; }
const std::vector<std::string>& StairPropertyReviewPreparation::retired_rail_ids() const noexcept { return retired_rails_; }
const std::vector<std::string>& StairPropertyReviewPreparation::erased_entity_ids() const noexcept { return erased_entities_; }
bool StairPropertyReviewPreparation::makes_change() const noexcept { return !command_.entity_changes.empty(); }

StairPropertyReviewPreparation replay_stair_property_review_preparation(
    const DocumentSnapshot& source, const Json& encoded) {
    const auto request = decode_stair_property_review_request(encoded);
    authenticate(source, request);
    const auto edited = changed_editors(source, request.profiles);
    std::vector<StairProfileEditIntent> retained_checks;
    for (const auto& profile : request.profiles) {
        StairProfileEditIntent retained;
        retained.object_id = profile.object_id;
        retained_checks.push_back(std::move(retained));
    }
    // Changed-only dependency authoring must not omit physical admission of
    // selected retained members. This no-op lane admits the complete actual
    // source, including their attached rails and hosted catalogs.
    (void)replay_stair_profile_entities(source.entities(), retained_checks);
    ApplyEntityChanges command;
    if (edited.empty()) {
        if (!request.dependency_dispositions.empty()) reject("invariant scalar cohort has no dependency decisions");
        const auto admitted = replay_stair_profile_entities(source.entities(), request.profiles);
        if (admitted.size() != source.entities().size()) reject("invariant admission changed actual inventory");
        for (const auto& [id, entity] : source.entities())
            if (!admitted.contains(id) || !exact(entity, admitted.at(id))) reject("invariant admission changed retained source");
        command.expected_revision = source.revision(); command.message = request.message;
    } else {
        // Actual scalar replay owns every editor envelope; the complete real
        // source and retained history stay inputs to the existing producer.
        validate_ordinary_stair_dependency_dispositions(source.entities(), edited, request.dependency_dispositions);
        command = prepare_ordinary_stair_dependency_edit(source, edited, request.dependency_dispositions, request.message);
    }
    if (command.expected_revision != source.revision() || command.message != request.message || !command.asset_changes.empty())
        reject("ordinary producer changed command authority");
    Entities result = source.entities();
    std::vector<std::string> erased, retired;
    for (const auto& decision : request.dependency_dispositions)
        if (decision.action == PhaseStairReplacementDependencyAction::retire) retired.push_back(decision.rail_id);
    std::set<std::string, std::less<>> seen;
    for (const auto& change : command.entity_changes) {
        const auto& id = change.kind == EntityChangeKind::erase ? change.entity_id : change.entity.id;
        if (!source.entities().contains(id) || !seen.insert(id).second) reject("producer introduced a fresh or duplicate owner");
        if (change.kind == EntityChangeKind::erase) {
            erased.push_back(id); result.erase(id);
            if (source.entities().at(id).type == "railing" && !std::binary_search(retired.begin(), retired.end(), id))
                reject("producer retired an unchosen rail");
        } else if (change.kind == EntityChangeKind::upsert) result.at(id) = change.entity;
        else reject("unsupported producer change kind");
    }
    std::sort(erased.begin(), erased.end());
    for (const auto& id : retired) if (!std::binary_search(erased.begin(), erased.end(), id))
        reject("explicit retirement is missing from the producer closure");
    std::erase_if(retained_checks, [&](const auto& profile) {
        return std::binary_search(erased.begin(), erased.end(), profile.object_id);
    });
    (void)replay_stair_profile_entities(result, retained_checks);
    return StairPropertyReviewPreparation(encoded, std::move(command), std::move(result), std::move(retired), std::move(erased));
}

StairPropertyReviewPreparation prepare_stair_property_review(const DocumentSnapshot& source,
    const std::vector<StairProfileEditIntent>& profiles,
    const std::vector<PhaseStairReplacementDependencyDisposition>& choices, const std::string& message) {
    source_metadata(source);
    StairPropertyReviewRequest request;
    request.expected_revision = source.revision(); request.source_saved_revision = source.saved_revision_optional();
    request.source_snapshot_digest = document_snapshot_digest(source);
    request.source_authoring_digest = document_authoring_source_digest_v2(source);
    request.source_entities_digest = entity_map_digest(source.entities());
    request.profiles = profiles; request.dependency_dispositions = choices; request.message = message;
    return replay_stair_property_review_preparation(source, encode_stair_property_review_request(request));
}

void require_stair_property_review_source_current(
    const DocumentSnapshot& source, const StairPropertyReviewPreparation& preparation) {
    authenticate(source, decode_stair_property_review_request(preparation.request()));
}

} // namespace sketch
