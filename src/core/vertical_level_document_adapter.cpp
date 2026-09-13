#include "sketch/vertical_level_document_adapter.hpp"
#include "sketch/document_digest.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sketch {
namespace {
[[noreturn]] void invalid(const std::string& message) {
    throw DocumentError(DocumentErrorCode::invalid_entity, message);
}

const FloorToFloorLink& connected_link(const VerticalLevelGraph& graph,
    const nlohmann::json& connection) {
    const auto id = connection.at("link_id").get<std::string>();
    const auto link = std::find_if(graph.links().begin(), graph.links().end(),
        [&](const auto& item) { return item.id == id; });
    if (link == graph.links().end() || link->state != RelationshipState::connected ||
        link->lower_level_id != connection.at("lower_level_id").get<std::string>() ||
        link->upper_level_id != connection.at("upper_level_id").get<std::string>()) {
        invalid("Connected stair level edit requires an unchanged connected link and endpoints: " + id);
    }
    return *link;
}

double finite_number(const nlohmann::json& properties, const char* field, bool positive = false) {
    const auto& value = properties.at(field);
    if (!value.is_number()) invalid(std::string("Invalid straight stair field: ") + field);
    const auto number = value.get<double>();
    if (!std::isfinite(number) || (positive && number <= 0))
        invalid(std::string("Invalid straight stair field: ") + field);
    return number;
}

void validate_straight_stair(const Entity& stair, double new_rise) {
    const auto& p = stair.properties;
    if (!p.at("version").is_number_integer() || p.at("version") != 1 ||
        p.at("form") != "straight_stair_flight")
        invalid("Level propagation supports only version-1 canonical straight stairs: " + stair.id);
    const auto& count = p.at("riser_count");
    if (!count.is_number_integer() || count.get<double>() < 1 || count.get<double>() > 10000)
        invalid("Invalid connected stair riser count: " + stair.id);
    (void)finite_number(p, "total_rise_m", true);
    (void)finite_number(p, "going_m", true);
    (void)finite_number(p, "width_m", true);
    (void)finite_number(p, "orientation_rad");
    const auto& base = p.at("base_position_m");
    if (!base.is_array() || base.size() != 3) invalid("Invalid connected stair placement: " + stair.id);
    for (const auto& coordinate : base)
        if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()))
            invalid("Invalid connected stair placement: " + stair.id);
    const auto& landing = p.at("top_landing");
    if (!landing.is_null()) {
        (void)finite_number(landing, "depth_m", true);
        if (finite_number(landing, "thickness_m", true) > new_rise)
            invalid("Connected stair landing would extend below its base: " + stair.id);
    }
}
} // namespace

VerticalLevelEditCandidate::VerticalLevelEditCandidate(DocumentSnapshot snapshot,
    ApplyEntityChanges command, std::string source_digest,
    std::vector<ConnectedStairRiseChange> affected_stairs)
    : snapshot_(std::move(snapshot)), command_(std::move(command)),
      source_digest_(std::move(source_digest)), affected_stairs_(std::move(affected_stairs)) {}

VerticalLevelEditCandidate prepare_vertical_level_edit(const DocumentSnapshot& source,
    const std::string& graph_entity_id, const VerticalLevelGraph& replacement) {
    // Validate retained history before inspecting any source relationship.
    (void)Document::fork(source);
    try {
        const auto found = source.entities().find(graph_entity_id);
        if (found == source.entities().end() || found->second.type != "vertical_levels")
            invalid("Level edit target must be an existing vertical_levels entity");
        const auto original = VerticalLevelGraph::from_json(found->second.properties.at("model"));
        auto graph_entity = found->second;
        graph_entity.properties["model"] = nlohmann::json::parse(replacement.serialize());
        ApplyEntityChanges command{source.revision(), {EntityChange::upsert(std::move(graph_entity))},
            {}, "Edit levels and connected stair rises"};
        std::vector<ConnectedStairRiseChange> changes;
        for (const auto& [id, entity] : source.entities()) {
            if (entity.type != "stair" || !entity.properties.contains("level_connection") ||
                entity.properties.at("level_connection").is_null()) continue;
            const auto& connection = entity.properties.at("level_connection");
            if (connection.at("graph_id") != graph_entity_id) continue;
            const auto& before = connected_link(original, connection);
            const auto& after = connected_link(replacement, connection);
            const auto rise = replacement.floor_to_floor_height(after.id);
            validate_straight_stair(entity, rise);
            const auto old_rise = entity.properties.at("total_rise_m").get<double>();
            if (std::abs(old_rise - original.floor_to_floor_height(before.id)) >
                VerticalLevelGraph::height_tolerance_m)
                invalid("Connected stair source rise does not match its link: " + id);
            if (old_rise == rise) continue;
            auto stair = entity;
            stair.properties["total_rise_m"] = rise;
            if (auto receipts = stair.properties.find("quantity_entries");
                receipts != stair.properties.end() && receipts->is_object())
                receipts->erase("/total_rise_m");
            command.entity_changes.push_back(EntityChange::upsert(std::move(stair)));
            changes.push_back({id, old_rise, rise});
        }
        auto snapshot = Document::preview_command(source, command);
        return VerticalLevelEditCandidate(std::move(snapshot), std::move(command),
            document_snapshot_digest(source), std::move(changes));
    } catch (const nlohmann::json::exception& error) {
        invalid(std::string("Malformed connected stair level edit: ") + error.what());
    }
}

VerticalLevelEditReceipt apply_vertical_level_edit(Document& document,
    const VerticalLevelEditCandidate& candidate) {
    if (document.revision() != candidate.command_.expected_revision ||
        document_snapshot_digest(document.snapshot()) != candidate.source_digest_)
        throw DocumentError(DocumentErrorCode::stale_revision,
            "Level edit preview no longer matches the current document; prepare it again");
    const auto revision = document.apply(candidate.command_);
    return {revision, candidate.affected_stairs_};
}
} // namespace sketch
