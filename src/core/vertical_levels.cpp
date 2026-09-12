#include "sketch/vertical_levels.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
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
void validate_graph_entity_id(const std::string& id) {
    if (id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        fail(VerticalLevelErrorCode::invalid_input,
             "Graph entity ID must contain 1 to 128 ASCII identifier bytes");
    }
}
void exact_fields(const nlohmann::json& value, std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size() != fields.size())
        fail(VerticalLevelErrorCode::invalid_input, "Invalid vertical level JSON fields");
    for (const auto* field : fields) {
        if (!value.contains(field))
            fail(VerticalLevelErrorCode::invalid_input, "Missing vertical level JSON field");
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

nlohmann::json VerticalLevelBinding::to_json() const {
    validate_graph_entity_id(graph_entity_id);
    validate_id(level_id);
    return nlohmann::json{{"version", 1}, {"graph_id", graph_entity_id}, {"level_id", level_id}};
}

VerticalLevelBinding VerticalLevelBinding::from_json(const nlohmann::json& value) {
    try {
        exact_fields(value, {"version", "graph_id", "level_id"});
        if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.at("graph_id").is_string() || !value.at("level_id").is_string()) {
            fail(VerticalLevelErrorCode::invalid_input, "Invalid vertical level binding value");
        }
        VerticalLevelBinding result{value.at("graph_id").get<std::string>(),
                                    value.at("level_id").get<std::string>()};
        validate_graph_entity_id(result.graph_entity_id);
        validate_id(result.level_id);
        return result;
    } catch (const VerticalLevelError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw VerticalLevelError(VerticalLevelErrorCode::invalid_input,
                                 std::string("Invalid vertical level binding JSON: ") + error.what());
    } catch (const std::exception& error) {
        throw VerticalLevelError(VerticalLevelErrorCode::invalid_input,
                                 std::string("Invalid vertical level binding JSON: ") + error.what());
    }
}

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

VerticalLevelGraph VerticalLevelGraph::from_json(const nlohmann::json& value) {
    try {
        exact_fields(value, {"version", "levels", "links"});
        if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.at("levels").is_array() || !value.at("links").is_array()) {
            fail(VerticalLevelErrorCode::invalid_input, "Unsupported vertical level JSON version");
        }
        std::vector<VerticalLevel> levels;
        levels.reserve(value.at("levels").size());
        for (const auto& entry : value.at("levels")) {
            exact_fields(entry, {"id", "elevation_m"});
            if (!entry.at("id").is_string() || !entry.at("elevation_m").is_number())
                fail(VerticalLevelErrorCode::invalid_input, "Invalid vertical level value");
            levels.push_back({entry.at("id").get<std::string>(), entry.at("elevation_m").get<double>()});
        }
        std::vector<FloorToFloorLink> links;
        links.reserve(value.at("links").size());
        std::vector<std::pair<std::string, double>> connected_heights;
        for (const auto& entry : value.at("links")) {
            exact_fields(entry, {"id", "lower_level_id", "upper_level_id", "state", "height_m"});
            if (!entry.at("id").is_string() || !entry.at("lower_level_id").is_string() ||
                !entry.at("upper_level_id").is_string() || !entry.at("state").is_string() ||
                !entry.at("height_m").is_number()) {
                fail(VerticalLevelErrorCode::invalid_input, "Invalid vertical link value");
            }
            const auto state = entry.at("state").get<std::string>();
            RelationshipState parsed_state{};
            if (state == "connected") parsed_state = RelationshipState::connected;
            else if (state == "frozen") parsed_state = RelationshipState::frozen;
            else if (state == "disconnected") parsed_state = RelationshipState::disconnected;
            else fail(VerticalLevelErrorCode::invalid_input, "Unknown vertical link state");
            const auto height = entry.at("height_m").get<double>();
            if (!std::isfinite(height) || height <= 0)
                fail(VerticalLevelErrorCode::invalid_input, "Vertical link height must be finite and positive");
            links.push_back({entry.at("id").get<std::string>(),
                             entry.at("lower_level_id").get<std::string>(),
                             entry.at("upper_level_id").get<std::string>(), parsed_state,
                             parsed_state == RelationshipState::connected
                                 ? std::nullopt : std::optional<double>(height)});
            if (parsed_state == RelationshipState::connected)
                connected_heights.emplace_back(entry.at("id").get<std::string>(), height);
        }
        const auto graph = VerticalLevelGraph(std::move(levels), std::move(links));
        for (const auto& [id, encoded_height] : connected_heights) {
            if (std::abs(graph.floor_to_floor_height(id) - encoded_height) > height_tolerance_m)
                fail(VerticalLevelErrorCode::invalid_input, "Connected vertical link height is inconsistent");
        }
        return graph;
    } catch (const VerticalLevelError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw VerticalLevelError(VerticalLevelErrorCode::invalid_input,
                                 std::string("Invalid vertical level JSON: ") + error.what());
    } catch (const std::exception& error) {
        throw VerticalLevelError(VerticalLevelErrorCode::invalid_input,
                                 std::string("Invalid vertical level JSON: ") + error.what());
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
