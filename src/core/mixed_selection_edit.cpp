#include "sketch/mixed_selection_edit.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/ordinary_selection_edit.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/project_store.hpp"
#include "sketch/roof_opening_group_edit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using MemberKey = std::pair<std::string, std::string>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t node_limit = 100000;
constexpr std::size_t depth_limit = 64;
constexpr std::size_t memo_limit = 128 * 1024 * 1024;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Mixed selection edit: ") + reason);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
void identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 128 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid identity");
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("source binding must be canonical SHA-256");
}
// Conservative compact-wire accounting before copies, dumping or codecs.
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void node(std::size_t depth) {
        if (depth > depth_limit || ++nodes > node_limit) reject("JSON complexity budget exceeded");
    }
    void quoted(std::string_view value) {
        add(2);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= value.size() - i) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(value[i + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) reject("invalid UTF-8 string");
                for (std::size_t j = 1; j <= tail; ++j) {
                    const auto byte = static_cast<unsigned char>(value[i + j]);
                    if (byte < 0x80 || byte > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1); i += tail;
            } else if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') add(2);
            else add(c < 0x20 ? 6 : 1);
        }
    }
    void json(const Json& value, std::size_t depth) {
        node(depth);
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else if (value.is_object()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); json(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& child : value) json(child, depth + 1);
        } else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
void message(const Json& value) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > 1024 ||
        value.get_ref<const std::string&>().find('\0') != std::string::npos) reject("invalid message");
}
struct Member {
    MemberKey key;
    bool carried{};
};
std::vector<Member> members(const Json& value) {
    if (!value.is_array() || value.empty() || value.size() > 1000) reject("requires one to 1000 roof children");
    std::vector<Member> result;
    std::optional<MemberKey> previous;
    for (const auto& row : value) {
        fields(row, {"roof_id", "opening_id", "coverage"});
        identity(row.at("roof_id")); identity(row.at("opening_id"));
        if (row.at("coverage") != "independent" && row.at("coverage") != "carried") reject("unsupported child coverage");
        MemberKey key{row.at("roof_id").get<std::string>(), row.at("opening_id").get<std::string>()};
        if (previous && !(key > *previous)) reject("roof children must be sorted and unique");
        previous = key;
        result.push_back({std::move(key), row.at("coverage") == "carried"});
    }
    return result;
}
std::set<std::string, std::less<>> explicit_parents(const Json& ordinary) {
    std::set<std::string, std::less<>> result;
    for (const auto& row : ordinary.at("architectural")) result.insert(row.at("entity_id").get<std::string>());
    return result;
}
std::vector<RoofEditIntent> roof_edits(const Json& roof, const Json& enclosing) {
    if (roof.is_null()) return {};
    if (!roof.is_object() || !roof.contains("version") || !roof.at("version").is_number_integer() ||
        (roof.at("version") != 4 && roof.at("version") != 17 && roof.at("version") != 20 && roof.at("version") != 21))
        reject("requires standalone roof phase dialect four, seventeen, twenty or twenty-one");
    const auto decoded = decode_phase_constraint_authoring_intent(roof);
    if (encode_phase_constraint_authoring_intent(decoded).dump() != roof.dump()) reject("roof authority is not canonical");
    for (const auto* key : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision"})
        if (roof.at(key).dump() != enclosing.at(key).dump()) reject("roof leaf must bind the same original source");
    auto empty = decoded;
    empty.intent = ConstraintAuthoringIntent{};
    empty.intent.message = decoded.intent.message;
    if (encode_phase_constraint_authoring_intent(empty).dump() != roof.dump()) reject("roof leaf cannot borrow ordinary constraint authority");
    message(roof.at("intent").at("message"));
    std::vector<RoofEditIntent> result;
    if (!decoded.roof_replacement.is_null()) {
        const auto replacement = decode_phase_roof_replacement_authoring(decoded.roof_replacement);
        if (replacement.demolition || !replacement.roof_profiles.empty() || !replacement.roof_opening_edits.empty() ||
            replacement.roof_edits.empty()) reject("replacement requires combined opening edits without legacy slices or demolition");
        result = replacement.roof_edits;
        result.insert(result.end(), replacement.ordinary_roof_edits.begin(), replacement.ordinary_roof_edits.end());
    } else {
        if (!decoded.ordinary_roof_edits.is_array() || decoded.ordinary_roof_edits.empty()) reject("roof edit leaf is empty");
        for (const auto& row : decoded.ordinary_roof_edits) result.push_back(decode_roof_edit_intent(row));
    }
    return result;
}
void validate(const Json& value) {
    Budget budget; budget.json(value, 0);
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "ordinary_request", "roof_authoring", "roof_opening_members", "message"});
    if (!value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2)) reject("unsupported intent version");
    const auto expected = revision(value.at("expected_revision"));
    if (expected == std::numeric_limits<Revision>::max()) reject("source revision cannot advance");
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(key));
    if (!value.at("source_saved_revision").is_null() && revision(value.at("source_saved_revision")) > expected)
        reject("saved revision exceeds source head");
    message(value.at("message"));
    const auto ordinary = validate_ordinary_selection_edit_request(value.at("ordinary_request"));
    if (ordinary.at("version") != value.at("version")) reject("ordinary request requires its matching mixed intent dialect");
    if (ordinary.dump() != value.at("ordinary_request").dump() || revision(ordinary.at("expected_revision")) != expected)
        reject("ordinary request is not canonical or binds another revision");
    const auto selected = members(value.at("roof_opening_members"));
    std::size_t count = selected.size();
    for (const auto* lane : {"architectural", "embedded", "annotations", "references"}) count += ordinary.at(lane).size();
    if (count > 1000) reject("aggregate selection exceeds 1000 members");
    const auto parents = explicit_parents(ordinary);
    std::set<MemberKey> independent;
    for (const auto& member : selected) {
        if (member.carried != parents.contains(member.key.first)) reject("carried children require selected parent; independent children cannot share one");
        if (!member.carried) independent.insert(member.key);
    }
    if (value.at("roof_authoring").is_null() != independent.empty()) reject("roof leaf must cover exactly the independent children");
    std::set<MemberKey> covered;
    for (const auto& edit : roof_edits(value.at("roof_authoring"), value)) {
        if (!edit.openings || edit.profile || edit.pose || edit.form || edit.transform || edit.resize || edit.uniform_transform ||
            edit.openings->upserts.empty() || !edit.openings->removed_opening_ids.empty()) reject("only existing child opening upserts are permitted");
        for (const auto& upsert : edit.openings->upserts) {
            const MemberKey key{edit.roof_id, upsert.opening_id};
            if (upsert.clone_source || (upsert.skylight && upsert.skylight->is_null()) ||
                !independent.contains(key) || !covered.insert(key).second) reject("upsert is not one unique independently selected actual child");
        }
    }
    if (covered != independent) reject("roof leaf must cover the full independent roster");
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() && left.extensions.dump() == right.extensions.dump();
}
bool exact(const Entities& left, const Entities& right) {
    if (left.size() != right.size()) return false;
    for (const auto& [id, entity] : left) {
        const auto found = right.find(id);
        if (found == right.end() || !exact(entity, found->second)) return false;
    }
    return true;
}
void metadata(const DocumentSnapshot& source, const DocumentSnapshot& candidate) {
    if (!candidate.is_editable() || source.document_id() != candidate.document_id() || source.assets() != candidate.assets() ||
        source.saved_revision_optional() != candidate.saved_revision_optional() || source.named_revisions() != candidate.named_revisions() ||
        source.read_only_reason() != candidate.read_only_reason()) reject("leaf changed source assets or project metadata");
    for (const auto& [id, asset] : source.assets())
        if (asset.metadata.dump() != candidate.assets().at(id).metadata.dump()) reject("leaf changed asset metadata");
    if (candidate.revision() == source.revision() && candidate.history().size() == source.history().size() &&
        (candidate.shares_full_snapshot_with(source) || document_snapshot_digest(candidate) == document_snapshot_digest(source)))
        return; // An independently admitted identity leaf creates no history.
    if (candidate.history().size() != source.history().size() + 1 || candidate.revision() != source.history().size())
        reject("leaf must retain the exact source or produce one detached history event");
}
const Json& child_row(const Entities& entities, const MemberKey& member) {
    const auto found = entities.find(member.first);
    if (found == entities.end() || found->second.id != member.first || found->second.type != "roof") reject("actual roof owner is missing");
    // This query independently decodes the host and requires a genuine profiled
    // skylight, with finite actual source centre and unique child identity.
    (void)roof_opening_group_member_center_world(entities, {member.first, member.second});
    for (const auto& row : found->second.properties.at("roof_openings"))
        if (row.at("id") == member.second) return row;
    reject("actual roof child is missing");
}
void preserve_wall_lane(const Entities& source, const Entities& candidate) {
    std::set<std::string, std::less<>> protected_owners;
    for (const auto& [id, entity] : source)
        if (entity.type == "wall" || is_physical_wall_room(entity)) protected_owners.insert(id);
    for (const auto& [id, entity] : source) {
        bool preserve = protected_owners.contains(id) || entity.type == "constraint";
        if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded = decode_boundary_dimension_entity(entity);
            // Unknown dimensions cannot acquire edit authority from this lane.
            preserve = preserve || !decoded.dimension || protected_owners.contains(decoded.dimension->boundary_id);
        }
        if (preserve) {
            const auto found = candidate.find(id);
            if (found == candidate.end() || !exact(entity, found->second)) reject("wall, physical room, constraint or wall-room dimension changed");
        }
    }
    for (const auto& [id, entity] : candidate)
        if (!source.contains(id) && (entity.type == "wall" || entity.type == "constraint" || is_physical_wall_room(entity)))
            reject("mixed rigid edit cannot introduce a wall, physical room or constraint");
}
struct ReplayMemo {
    std::map<std::pair<std::string, std::string>, Entities> results;
    std::set<std::pair<std::string, std::string>> in_progress;
    std::size_t bytes{}, active_bytes{};
};
thread_local std::unique_ptr<ReplayMemo> replay_memo;
thread_local std::size_t replay_scope_depth{};
void remember(const std::pair<std::string, std::string>& key, const Entities& result) {
    // Capacity is an optimization limit, never a new document/history limit.
    if (replay_memo->results.size() >= 4096) return;
    std::size_t bytes = key.first.size() + key.second.size();
    const auto add = [&](std::size_t size) {
        if (size > memo_limit - bytes) return false;
        bytes += size;
        return true;
    };
    for (const auto& [id, entity] : result) {
        if (!add(sizeof(Entity)) || !add(id.size()) || !add(entity.id.size()) || !add(entity.type.size()) ||
            !add(entity.properties.dump().size()) || !add(entity.extensions.dump().size())) return;
    }
    if (bytes > memo_limit - replay_memo->bytes - replay_memo->active_bytes) return;
    replay_memo->results.emplace(key, result); replay_memo->bytes += bytes;
}
struct Replaying {
    const std::pair<std::string, std::string>& key;
    std::size_t bytes;
    explicit Replaying(const std::pair<std::string, std::string>& value) : key(value), bytes(value.first.size() + value.second.size()) {
        if (bytes > memo_limit - replay_memo->bytes - replay_memo->active_bytes) {
            replay_memo->results.clear(); replay_memo->bytes = 0;
        }
        if (bytes > memo_limit - replay_memo->active_bytes) reject("aggregate recursive proof budget exceeded");
        if (replay_memo->in_progress.size() >= 64 || !replay_memo->in_progress.insert(key).second) reject("recursive replay refused");
        replay_memo->active_bytes += bytes;
    }
    ~Replaying() { replay_memo->in_progress.erase(key); replay_memo->active_bytes -= bytes; }
};
} // namespace

MixedSelectionEditReplayScope::MixedSelectionEditReplayScope() {
    if (!replay_scope_depth) replay_memo = std::make_unique<ReplayMemo>();
    ++replay_scope_depth;
}
MixedSelectionEditReplayScope::~MixedSelectionEditReplayScope() {
    if (!--replay_scope_depth) replay_memo.reset();
}
Json validate_mixed_selection_edit_intent(const Json& value) {
    try { validate(value); return value; }
    catch (const Json::exception&) { reject("malformed intent"); }
}
bool mixed_selection_edit_active_phase_policy(const Json& value) {
    validate(value); return !value.at("roof_authoring").is_null();
}
std::optional<Revision> mixed_selection_edit_source_saved_revision(const Json& value) {
    validate(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
}
Entities replay_mixed_selection_edit(const DocumentSnapshot& source, const Json& value) {
    MixedSelectionEditReplayScope scope;
    validate(value);
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() ||
        source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("captured source history or metadata is invalid");
    const auto source_digest = document_snapshot_digest(source);
    const auto saved = value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
    if (!source.is_editable() || source.revision() != revision(value.at("expected_revision")) || source.saved_revision_optional() != saved ||
        source_digest != value.at("source_snapshot_digest") || document_authoring_source_digest_v2(source) != value.at("source_authoring_digest") ||
        entity_map_digest(source.entities()) != value.at("source_entities_digest")) reject("captured full source authority changed");
    const auto key = std::pair{source_digest, value.dump()};
    if (const auto found = replay_memo->results.find(key); found != replay_memo->results.end()) return found->second;
    Replaying replaying(key);
    const auto selected = members(value.at("roof_opening_members"));
    for (const auto& member : selected) (void)child_row(source.entities(), member.key);

    // Complete closed producers and previews use ORIGINAL source. Neither
    // detached result is ever supplied as authority to the other leaf.
    const auto ordinary = replay_ordinary_selection_edit(source, value.at("ordinary_request"));
    if (ordinary.expected_revision != source.revision() || !ordinary.asset_changes.empty()) reject("ordinary leaf changed revision or assets");
    const auto ordinary_stage = Document::preview_command(source, Command{ordinary});
    metadata(source, ordinary_stage);
    // The closed architectural producer above owns rigid reflection and uniform
    // scaling of the parent's complete child roster. These may legitimately
    // change local scalar/angle rows. Admit the same actual qualified child in
    // its complete result, rather than inventing unchanged-row carry semantics.
    for (const auto& member : selected) if (member.carried)
        (void)child_row(ordinary_stage.entities(), member.key);
    std::vector<Entities> leaves{ordinary_stage.entities()};
    if (!value.at("roof_authoring").is_null()) {
        const auto expected = replay_phase_constraint_authoring(source.entities(), value.at("roof_authoring"));
        ApplyBoundaryConstraintChanges command;
        command.expected_revision = source.revision();
        command.message = value.at("message").get<std::string>();
        command.phase_constraint_authoring_completion = true;
        command.phase_constraint_authoring_intent = value.at("roof_authoring");
        const auto roof_stage = Document::preview_command(source, Command{std::move(command)});
        metadata(source, roof_stage);
        if (!exact(expected, roof_stage.entities())) reject("roof preview differs from complete actual-source replay");
        leaves.push_back(roof_stage.entities());
    }
    for (const auto& leaf : leaves) preserve_wall_lane(source.entities(), leaf);
    auto result = compose_mixed_selection_edit_candidates(source.entities(), leaves);
    preserve_wall_lane(source.entities(), result);
    // Document owns complete final geometry, phase policy and retained-history
    // identity validation, followed by the only live publication.
    remember(key, result);
    return result;
}
Json make_mixed_selection_edit_intent(const DocumentSnapshot& source, const Json& ordinary_request,
    const Json& roof_authoring, const Json& roof_members, std::string_view text) {
    // Account borrowed inputs together before constructing the compound JSON.
    Budget budget; budget.node(0); budget.add(1024);
    // Scalar fields synthesized below still consume the shared node budget.
    for (std::size_t i = 0; i < 6; ++i) budget.node(1);
    budget.json(ordinary_request, 1); budget.json(roof_authoring, 1); budget.json(roof_members, 1);
    budget.node(1); budget.quoted(text);
    if (text.size() > 1024 || text.find('\0') != std::string_view::npos) reject("invalid message");
    const auto canonical_ordinary = validate_ordinary_selection_edit_request(ordinary_request);
    Json value = {{"version", canonical_ordinary.at("version")}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"ordinary_request", ordinary_request}, {"roof_authoring", roof_authoring},
        {"roof_opening_members", roof_members}, {"message", std::string(text)}};
    (void)replay_mixed_selection_edit(source, value);
    return value;
}
} // namespace sketch
