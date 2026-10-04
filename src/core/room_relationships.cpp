#include "sketch/room_relationships.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch {
namespace {
const char* name(RoomReferenceKind kind) {
    switch (kind) {
    case RoomReferenceKind::room_boundary: return "room_boundary";
    case RoomReferenceKind::appraisal_measurement_boundary: return "appraisal_measurement_boundary";
    case RoomReferenceKind::architectural_wall: return "architectural_wall";
    }
    throw std::invalid_argument("Unknown room reference kind");
}
const char* name(RoomRelationKind kind) {
    switch (kind) {
    case RoomRelationKind::independent: return "independent";
    case RoomRelationKind::follows: return "follows";
    case RoomRelationKind::derived_from: return "derived_from";
    }
    throw std::invalid_argument("Unknown room relation kind");
}
bool valid_id(const std::string& id) {
    return !id.empty() && std::any_of(id.begin(), id.end(), [](unsigned char ch) { return ch > 32; })
        && std::none_of(id.begin(), id.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
}
using Graph = std::map<std::string, std::vector<std::string>>;
bool reaches(const Graph& graph, const std::string& start, const std::string& end) {
    std::vector<std::string> pending{start};
    std::set<std::string> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (current == end) return true;
        if (!visited.insert(current).second) continue;
        const auto found = graph.find(current);
        if (found != graph.end()) pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    return false;
}
void exact_keys(const nlohmann::json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) throw std::invalid_argument("Invalid relationship JSON fields");
    for (const auto key : keys) if (!value.contains(key)) throw std::invalid_argument("Missing relationship JSON field");
}
}

std::vector<std::string> room_reference_wall_ids(const RoomReference& reference) {
    return reference.wall_members.empty() ? std::vector<std::string>{reference.id} : reference.wall_members;
}

std::uint64_t room_relationship_model_version(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("schema_version") ||
        !value.at("schema_version").is_number_integer())
        throw std::invalid_argument("Relationship schema version must be a positive integer");
    const auto& version=value.at("schema_version");
    if (version.is_number_unsigned()) {
        const auto parsed=version.get<std::uint64_t>();
        if (parsed>0) return parsed;
    } else {
        const auto parsed=version.get<std::int64_t>();
        if (parsed>0) return static_cast<std::uint64_t>(parsed);
    }
    throw std::invalid_argument("Relationship schema version must be a positive integer");
}

RoomRelationshipSnapshot::RoomRelationshipSnapshot(std::vector<RoomReference> references,
                                                   std::vector<RoomRelation> relations)
    : references_(std::move(references)), relations_(std::move(relations)) {}

RoomRelationshipSnapshot RoomRelationshipSnapshot::create(std::vector<RoomReference> references,
                                                           std::vector<RoomRelation> relations) {
    std::map<std::string, RoomReferenceKind> identities;
    std::set<std::string> physical_members;
    bool chains=false;
    for (const auto& reference : references) {
        (void)name(reference.kind);
        if (!valid_id(reference.id) || !identities.emplace(reference.id, reference.kind).second)
            throw std::invalid_argument("Invalid or duplicate room reference identity");
        if (!reference.wall_members.empty()) {
            if (reference.kind!=RoomReferenceKind::architectural_wall || reference.wall_members.size()<2 ||
                reference.wall_members.size()>256 || reference.wall_members.front()!=reference.id)
                throw std::invalid_argument("Room wall chain requires bounded ordered architectural members beginning with its identity");
            chains=true;
        }
        for (const auto& member : room_reference_wall_ids(reference))
            if (!valid_id(member) || !physical_members.insert(member).second)
                throw std::invalid_argument("Room references contain invalid or aliased physical membership");
    }
    Graph dependencies;
    std::map<std::string, RoomRelationKind> drivers;
    std::set<std::pair<std::string, std::string>> pairs;
    for (auto& relation : relations) {
        (void)name(relation.kind);
        if (!identities.contains(relation.source_id) || !identities.contains(relation.target_id))
            throw std::invalid_argument("Relationship references an unknown identity");
        if (relation.source_id == relation.target_id) throw std::invalid_argument("Self relationship is invalid");
        if (relation.kind == RoomRelationKind::independent && relation.target_id < relation.source_id)
            std::swap(relation.source_id, relation.target_id);
        if (!pairs.emplace(relation.source_id, relation.target_id).second)
            throw std::invalid_argument("Duplicate or contradictory relationship pair");
        if (relation.kind == RoomRelationKind::independent) continue;
        if (identities.at(relation.source_id) == RoomReferenceKind::architectural_wall)
            throw std::invalid_argument("Architectural walls cannot be driven by boundary relationships");
        const auto [driver, inserted] = drivers.emplace(relation.source_id, relation.kind);
        if (!inserted && (driver->second != relation.kind || relation.kind == RoomRelationKind::follows))
            throw std::invalid_argument("Ambiguous relationship drivers");
        dependencies[relation.source_id].push_back(relation.target_id);
    }
    for (const auto& [source, targets] : dependencies)
        for (const auto& target : targets)
            if (reaches(dependencies, target, source)) throw std::invalid_argument("Relationship dependency cycle");
    for (const auto& relation : relations)
        if (relation.kind == RoomRelationKind::independent &&
            (reaches(dependencies, relation.source_id, relation.target_id) ||
             reaches(dependencies, relation.target_id, relation.source_id)))
            throw std::invalid_argument("Independence conflicts with dependency path");
    std::sort(references.begin(), references.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(relations.begin(), relations.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.target_id, a.kind) < std::tie(b.source_id, b.target_id, b.kind);
    });
    auto result=RoomRelationshipSnapshot(std::move(references), std::move(relations));
    result.schema_version_=chains ? 2 : 1;
    return result;
}

const std::vector<RoomReference>& RoomRelationshipSnapshot::references() const noexcept { return references_; }
const std::vector<RoomRelation>& RoomRelationshipSnapshot::relations() const noexcept { return relations_; }
std::uint64_t RoomRelationshipSnapshot::schema_version() const noexcept { return schema_version_; }
RoomRelationshipSnapshot RoomRelationshipSnapshot::retarget(
    const RoomRelationshipRetarget& edit) const {
    (void)name(edit.kind);
    if (!valid_id(edit.source_id) || !valid_id(edit.target_id) ||
        !valid_id(edit.replacement_target_id))
        throw std::invalid_argument("Invalid relationship retarget identity");

    const auto matches = [&](const RoomRelation& relation) {
        if (relation.kind != edit.kind) return false;
        if (edit.kind == RoomRelationKind::independent) {
            return (relation.source_id == edit.source_id && relation.target_id == edit.target_id) ||
                   (relation.source_id == edit.target_id && relation.target_id == edit.source_id);
        }
        return relation.source_id == edit.source_id && relation.target_id == edit.target_id;
    };
    const auto found = std::find_if(relations_.begin(), relations_.end(), matches);
    if (found == relations_.end())
        throw std::invalid_argument("The relationship to retarget is not declared");
    if (edit.target_id == edit.replacement_target_id)
        throw std::invalid_argument("The relationship target is unchanged");

    auto updated = relations_;
    const auto index = static_cast<std::size_t>(std::distance(relations_.begin(), found));
    // The caller names the endpoint that is being replaced. Independent
    // relations are symmetric, so canonicalization in create() preserves the
    // other endpoint regardless of the order used by the editor.
    updated[index] = {edit.source_id, edit.replacement_target_id, edit.kind};
    auto result=create(references_,std::move(updated));
    result.schema_version_=schema_version_;
    return result;
}
nlohmann::json RoomRelationshipSnapshot::to_json() const {
    auto references = nlohmann::json::array();
    auto relations = nlohmann::json::array();
    for (const auto& ref : references_) {
        nlohmann::json encoded={{"id",ref.id},{"kind",name(ref.kind)}};
        if (!ref.wall_members.empty()) encoded["wall_members"]=ref.wall_members;
        references.push_back(std::move(encoded));
    }
    for (const auto& rel : relations_) relations.push_back({{"source_id", rel.source_id}, {"target_id", rel.target_id}, {"kind", name(rel.kind)}});
    return {{"schema_version", schema_version_}, {"references", references}, {"relations", relations}};
}
RoomRelationshipSnapshot RoomRelationshipSnapshot::from_json(const nlohmann::json& value) {
    try {
        exact_keys(value, {"schema_version", "references", "relations"});
        const auto version=room_relationship_model_version(value);
        if ((version!=1 && version!=2) ||
            !value.at("references").is_array() || !value.at("relations").is_array())
            throw std::invalid_argument("Unsupported relationship JSON schema");
        std::vector<RoomReference> references;
        std::vector<RoomRelation> relations;
        for (const auto& ref : value.at("references")) {
            if (version==2 && ref.contains("wall_members")) exact_keys(ref,{"id","kind","wall_members"});
            else exact_keys(ref, {"id", "kind"});
            const auto kind = ref.at("kind").get<std::string>();
            RoomReferenceKind parsed;
            if (kind == "room_boundary") parsed = RoomReferenceKind::room_boundary;
            else if (kind == "appraisal_measurement_boundary") parsed = RoomReferenceKind::appraisal_measurement_boundary;
            else if (kind == "architectural_wall") parsed = RoomReferenceKind::architectural_wall;
            else throw std::invalid_argument("Unknown room reference kind");
            std::vector<std::string> members;
            if (ref.contains("wall_members")) {
                if (!ref.at("wall_members").is_array() || ref.at("wall_members").size()<2)
                    throw std::invalid_argument("Explicit room wall chain requires at least two members");
                members=ref.at("wall_members").get<std::vector<std::string>>();
            }
            references.push_back({ref.at("id").get<std::string>(), parsed,std::move(members)});
        }
        for (const auto& rel : value.at("relations")) {
            exact_keys(rel, {"source_id", "target_id", "kind"});
            const auto kind = rel.at("kind").get<std::string>();
            RoomRelationKind parsed;
            if (kind == "independent") parsed = RoomRelationKind::independent;
            else if (kind == "follows") parsed = RoomRelationKind::follows;
            else if (kind == "derived_from") parsed = RoomRelationKind::derived_from;
            else throw std::invalid_argument("Unknown room relation kind");
            relations.push_back({rel.at("source_id").get<std::string>(), rel.at("target_id").get<std::string>(), parsed});
        }
        auto result=create(std::move(references),std::move(relations));
        result.schema_version_=version;
        return result;
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Invalid relationship JSON types");
    }
}
} // namespace sketch
