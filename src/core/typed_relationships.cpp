#include "sketch/typed_relationships.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace sketch {
namespace {
[[noreturn]] void fail(RelationshipErrorCode code, const char* message) {
    throw RelationshipError(code, message);
}
void validate_id(const std::string& id) {
    if (id.empty() || id.size() > 256) fail(RelationshipErrorCode::invalid_input, "IDs must contain 1 to 256 UTF-8 bytes");
    try { (void)nlohmann::json(id).dump(); }
    catch (const nlohmann::json::type_error&) { fail(RelationshipErrorCode::invalid_input, "ID contains invalid UTF-8"); }
}
const char* kind_name(RelationshipKind kind) {
    switch (kind) {
    case RelationshipKind::wall_derived: return "wall_derived";
    case RelationshipKind::room_boundary: return "room_boundary";
    case RelationshipKind::appraisal_measurement_boundary: return "appraisal_measurement_boundary";
    }
    fail(RelationshipErrorCode::invalid_input, "Unknown relationship kind");
}
const char* state_name(RelationshipState state) {
    switch (state) {
    case RelationshipState::connected: return "connected";
    case RelationshipState::frozen: return "frozen";
    case RelationshipState::disconnected: return "disconnected";
    }
    fail(RelationshipErrorCode::invalid_input, "Unknown relationship state");
}
}

RelationshipError::RelationshipError(RelationshipErrorCode code, std::string message)
    : std::invalid_argument(std::move(message)), code_(code) {}
TypedRelationshipGraph::TypedRelationshipGraph(std::vector<std::string> objects, std::vector<TypedRelationship> links)
    : object_ids_(std::move(objects)), relationships_(std::move(links)) {
    if (object_ids_.size() > maximum_objects || relationships_.size() > maximum_relationships)
        fail(RelationshipErrorCode::limit_exceeded, "Relationship graph exceeds resource limits");
    std::sort(object_ids_.begin(), object_ids_.end());
    std::sort(relationships_.begin(), relationships_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> successors;
    for (const auto& id : object_ids_) {
        validate_id(id);
        if (!indegree.emplace(id, 0).second) fail(RelationshipErrorCode::duplicate_id, "Duplicate object ID");
    }
    std::set<std::string> link_ids;
    std::set<std::string> retained_targets;
    for (const auto& link : relationships_) {
        validate_id(link.id);
        (void)kind_name(link.kind);
        (void)state_name(link.state);
        if (!link_ids.insert(link.id).second) fail(RelationshipErrorCode::duplicate_id, "Duplicate relationship ID");
        for (const auto* id : {&link.owner_id, &link.source_id, &link.target_id}) {
            validate_id(*id);
            if (!indegree.contains(*id)) fail(RelationshipErrorCode::dangling_reference, "Relationship references an unknown object");
        }
        if (link.state == RelationshipState::disconnected) continue;
        if (!retained_targets.insert(link.target_id).second)
            fail(RelationshipErrorCode::ambiguous_target, "Target already has a retained relationship");
        successors[link.source_id].push_back(link.target_id);
        ++indegree.at(link.target_id);
    }
    // Iterative topological validation avoids recursion on imported chains.
    std::vector<std::string> ready;
    for (const auto& [id, count] : indegree) if (count == 0) ready.push_back(id);
    for (std::size_t index = 0; index < ready.size(); ++index) {
        const auto found = successors.find(ready[index]);
        if (found == successors.end()) continue;
        for (const auto& target : found->second) if (--indegree.at(target) == 0) ready.push_back(target);
    }
    if (ready.size() != object_ids_.size()) fail(RelationshipErrorCode::cycle, "Relationship graph contains a cycle");
}
TypedRelationshipGraph TypedRelationshipGraph::create(TypedRelationship relationship) const {
    if (relationship.state != RelationshipState::connected)
        fail(RelationshipErrorCode::invalid_transition, "New relationships must be connected");
    auto links = relationships_;
    links.push_back(std::move(relationship));
    return TypedRelationshipGraph(object_ids_, std::move(links));
}
TypedRelationshipGraph TypedRelationshipGraph::transition(std::string_view id, RelationshipState state) const {
    auto links = relationships_;
    const auto found = std::find_if(links.begin(), links.end(), [&](const auto& link) { return link.id == id; });
    if (found == links.end()) fail(RelationshipErrorCode::missing_relationship, "Unknown relationship ID");
    if (found->state == RelationshipState::disconnected || found->state == state)
        fail(RelationshipErrorCode::invalid_transition, "Relationship state transition is not permitted");
    found->state = state;
    return TypedRelationshipGraph(object_ids_, std::move(links));
}
TypedRelationshipGraph TypedRelationshipGraph::freeze(std::string_view id) const { return transition(id, RelationshipState::frozen); }
TypedRelationshipGraph TypedRelationshipGraph::disconnect(std::string_view id) const { return transition(id, RelationshipState::disconnected); }
std::string TypedRelationshipGraph::serialize() const {
    auto links = nlohmann::json::array();
    for (const auto& link : relationships_) {
        links.push_back({{"id", link.id}, {"kind", kind_name(link.kind)}, {"owner_id", link.owner_id},
                         {"source_id", link.source_id}, {"target_id", link.target_id}, {"state", state_name(link.state)}});
    }
    return nlohmann::json{{"version", 1}, {"object_ids", object_ids_}, {"relationships", std::move(links)}}.dump();
}
} // namespace sketch
