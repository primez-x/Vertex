#include "sketch/vertical_levels.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace sketch {
namespace {
[[noreturn]] void fail(VerticalLevelErrorCode code, const char* message) {
    throw VerticalLevelError(code, message);
}
void validate_id(const std::string& id) {
    if (id.empty() || id.size() > 256)
        fail(VerticalLevelErrorCode::invalid_input, "IDs must contain 1 to 256 UTF-8 bytes");
    try { (void)nlohmann::json(id).dump(); }
    catch (const nlohmann::json::type_error&) {
        fail(VerticalLevelErrorCode::invalid_input, "Invalid UTF-8 ID");
    }
}
const char* state_name(RelationshipState state) {
    switch (state) {
    case RelationshipState::connected: return "connected";
    case RelationshipState::frozen: return "frozen";
    case RelationshipState::disconnected: return "disconnected";
    }
    fail(VerticalLevelErrorCode::invalid_input, "Unknown link state");
}
}
VerticalLevelError::VerticalLevelError(VerticalLevelErrorCode code, std::string message)
    : std::invalid_argument(std::move(message)), code_(code) {}

VerticalLevelGraph::VerticalLevelGraph(std::vector<VerticalLevel> levels, std::vector<FloorToFloorLink> links)
    : levels_(std::move(levels)), links_(std::move(links)) {
    if (levels_.size() > maximum_levels || links_.size() > maximum_links)
        fail(VerticalLevelErrorCode::limit_exceeded, "Vertical graph exceeds resource limits");
    const auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(levels_.begin(), levels_.end(), by_id);
    std::sort(links_.begin(), links_.end(), by_id);
    std::map<std::string, double> elevations;
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> successors;
    for (auto& level : levels_) {
        validate_id(level.id);
        if (!std::isfinite(level.elevation_m)) fail(VerticalLevelErrorCode::invalid_input, "Elevation must be finite");
        if (level.elevation_m == 0) level.elevation_m = 0; // Canonical positive zero.
        if (!elevations.emplace(level.id, level.elevation_m).second)
            fail(VerticalLevelErrorCode::duplicate_id, "Duplicate level ID");
        indegree.emplace(level.id, 0);
    }
    std::set<std::string> ids;
    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto& link : links_) {
        validate_id(link.id);
        (void)state_name(link.state);
        if (!ids.insert(link.id).second) fail(VerticalLevelErrorCode::duplicate_id, "Duplicate link ID");
        for (const auto* id : {&link.lower_level_id, &link.upper_level_id}) {
            validate_id(*id);
            if (!elevations.contains(*id)) fail(VerticalLevelErrorCode::dangling_reference, "Unknown level reference");
        }
        if (link.state == RelationshipState::connected) {
            if (link.retained_height_m) fail(VerticalLevelErrorCode::invalid_input, "Connected links cannot retain a height");
        } else if (!link.retained_height_m || !std::isfinite(*link.retained_height_m) || *link.retained_height_m <= 0) {
            fail(VerticalLevelErrorCode::invalid_input, "Inactive links require a finite positive retained height");
        }
        if (link.lower_level_id == link.upper_level_id) fail(VerticalLevelErrorCode::cycle, "Self link is invalid");
        if (link.state == RelationshipState::disconnected) continue;
        if (!pairs.emplace(link.lower_level_id, link.upper_level_id).second)
            fail(VerticalLevelErrorCode::duplicate_link, "Duplicate retained floor-to-floor link");
        successors[link.lower_level_id].push_back(link.upper_level_id);
        ++indegree.at(link.upper_level_id);
    }
    std::vector<std::string> ready;
    for (const auto& [id, degree] : indegree) if (degree == 0) ready.push_back(id);
    for (std::size_t i = 0; i < ready.size(); ++i) {
        const auto found = successors.find(ready[i]);
        if (found == successors.end()) continue;
        for (const auto& target : found->second) if (--indegree.at(target) == 0) ready.push_back(target);
    }
    if (ready.size() != levels_.size()) fail(VerticalLevelErrorCode::cycle, "Vertical graph contains a cycle");
    for (const auto& link : links_) {
        if (link.state == RelationshipState::disconnected) continue;
        const double height = elevations.at(link.upper_level_id) - elevations.at(link.lower_level_id);
        if (!std::isfinite(height) || height <= 0)
            fail(VerticalLevelErrorCode::non_monotonic, "Upper level must be above lower level with finite height");
        if (link.state == RelationshipState::frozen && std::abs(height - *link.retained_height_m) > height_tolerance_m)
            fail(VerticalLevelErrorCode::frozen_height_changed, "Elevation edit changes a frozen floor-to-floor height");
    }
}
VerticalLevelGraph VerticalLevelGraph::create_level(VerticalLevel level) const {
    auto levels = levels_;
    levels.push_back(std::move(level));
    return VerticalLevelGraph(std::move(levels), links_);
}
VerticalLevelGraph VerticalLevelGraph::create_link(FloorToFloorLink link) const {
    if (link.state != RelationshipState::connected || link.retained_height_m)
        fail(VerticalLevelErrorCode::invalid_transition, "New links must be connected without retained height");
    auto links = links_;
    links.push_back(std::move(link));
    return VerticalLevelGraph(levels_, std::move(links));
}
VerticalLevelGraph VerticalLevelGraph::with_elevation(std::string_view id, double elevation) const {
    auto levels = levels_;
    const auto found = std::find_if(levels.begin(), levels.end(), [&](const auto& level) { return level.id == id; });
    if (found == levels.end()) fail(VerticalLevelErrorCode::missing_level, "Unknown level ID");
    found->elevation_m = elevation;
    return VerticalLevelGraph(std::move(levels), links_);
}
double VerticalLevelGraph::floor_to_floor_height(std::string_view id) const {
    const auto found = std::find_if(links_.begin(), links_.end(), [&](const auto& link) { return link.id == id; });
    if (found == links_.end()) fail(VerticalLevelErrorCode::missing_link, "Unknown link ID");
    if (found->retained_height_m) return *found->retained_height_m;
    const auto elevation = [&](const std::string& level_id) {
        return std::find_if(levels_.begin(), levels_.end(), [&](const auto& level) { return level.id == level_id; })->elevation_m;
    };
    return elevation(found->upper_level_id) - elevation(found->lower_level_id);
}
VerticalLevelGraph VerticalLevelGraph::transition(std::string_view id, RelationshipState state) const {
    auto links = links_;
    const auto found = std::find_if(links.begin(), links.end(), [&](const auto& link) { return link.id == id; });
    if (found == links.end()) fail(VerticalLevelErrorCode::missing_link, "Unknown link ID");
    if (found->state == RelationshipState::disconnected || found->state == state)
        fail(VerticalLevelErrorCode::invalid_transition, "Invalid vertical link transition");
    found->retained_height_m = floor_to_floor_height(id);
    found->state = state;
    return VerticalLevelGraph(levels_, std::move(links));
}
VerticalLevelGraph VerticalLevelGraph::freeze(std::string_view id) const { return transition(id, RelationshipState::frozen); }
VerticalLevelGraph VerticalLevelGraph::disconnect(std::string_view id) const { return transition(id, RelationshipState::disconnected); }
std::string VerticalLevelGraph::serialize() const {
    auto levels = nlohmann::json::array();
    auto links = nlohmann::json::array();
    for (const auto& level : levels_) levels.push_back({{"id", level.id}, {"elevation_m", level.elevation_m}});
    for (const auto& link : links_) {
        nlohmann::json value{{"id", link.id}, {"lower_level_id", link.lower_level_id},
            {"upper_level_id", link.upper_level_id}, {"state", state_name(link.state)},
            {"height_m", floor_to_floor_height(link.id)}};
        links.push_back(std::move(value));
    }
    return nlohmann::json{{"version", 1}, {"levels", std::move(levels)}, {"links", std::move(links)}}.dump();
}
} // namespace sketch
