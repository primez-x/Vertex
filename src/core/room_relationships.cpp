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

RoomRelationshipSnapshot::RoomRelationshipSnapshot(std::vector<RoomReference> references,
                                                   std::vector<RoomRelation> relations)
    : references_(std::move(references)), relations_(std::move(relations)) {}

RoomRelationshipSnapshot RoomRelationshipSnapshot::create(std::vector<RoomReference> references,
                                                           std::vector<RoomRelation> relations) {
    std::map<std::string, RoomReferenceKind> identities;
    for (const auto& reference : references) {
        (void)name(reference.kind);
        if (!valid_id(reference.id) || !identities.emplace(reference.id, reference.kind).second)
            throw std::invalid_argument("Invalid or duplicate room reference identity");
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
    return RoomRelationshipSnapshot(std::move(references), std::move(relations));
}

const std::vector<RoomReference>& RoomRelationshipSnapshot::references() const noexcept { return references_; }
const std::vector<RoomRelation>& RoomRelationshipSnapshot::relations() const noexcept { return relations_; }
nlohmann::json RoomRelationshipSnapshot::to_json() const {
    auto references = nlohmann::json::array();
    auto relations = nlohmann::json::array();
    for (const auto& ref : references_) references.push_back({{"id", ref.id}, {"kind", name(ref.kind)}});
    for (const auto& rel : relations_) relations.push_back({{"source_id", rel.source_id}, {"target_id", rel.target_id}, {"kind", name(rel.kind)}});
    return {{"schema_version", 1}, {"references", references}, {"relations", relations}};
}
RoomRelationshipSnapshot RoomRelationshipSnapshot::from_json(const nlohmann::json& value) {
    try {
        exact_keys(value, {"schema_version", "references", "relations"});
        if (!value.at("schema_version").is_number_integer() || value.at("schema_version") != 1 ||
            !value.at("references").is_array() || !value.at("relations").is_array())
            throw std::invalid_argument("Unsupported relationship JSON schema");
        std::vector<RoomReference> references;
        std::vector<RoomRelation> relations;
        for (const auto& ref : value.at("references")) {
            exact_keys(ref, {"id", "kind"});
            const auto kind = ref.at("kind").get<std::string>();
            RoomReferenceKind parsed;
            if (kind == "room_boundary") parsed = RoomReferenceKind::room_boundary;
            else if (kind == "appraisal_measurement_boundary") parsed = RoomReferenceKind::appraisal_measurement_boundary;
            else if (kind == "architectural_wall") parsed = RoomReferenceKind::architectural_wall;
            else throw std::invalid_argument("Unknown room reference kind");
            references.push_back({ref.at("id").get<std::string>(), parsed});
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
        return create(std::move(references), std::move(relations));
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Invalid relationship JSON types");
    }
}
} // namespace sketch
