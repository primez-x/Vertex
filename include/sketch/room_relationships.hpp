#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sketch {

enum class RoomReferenceKind { room_boundary, appraisal_measurement_boundary, architectural_wall };
enum class RoomRelationKind { independent, follows, derived_from };

struct RoomReference {
    std::string id;
    RoomReferenceKind kind;
    // Empty retains legacy singleton identity. Explicit wall chains follow
    // native baseline direction, with id naming their first physical member.
    std::vector<std::string> wall_members{};
    bool operator==(const RoomReference&) const = default;
};

[[nodiscard]] std::vector<std::string> room_reference_wall_ids(const RoomReference& reference);
// Inspect an envelope without interpreting unknown positive schema versions.
[[nodiscard]] std::uint64_t room_relationship_model_version(const nlohmann::json& value);

struct RoomRelation {
    // For dependencies, source follows/is derived from target.
    std::string source_id;
    std::string target_id;
    RoomRelationKind kind;
    bool operator==(const RoomRelation&) const = default;
};

// Identifies one declared relation and the endpoint that should replace its
// current target. Retargeting is validated as a complete graph transition;
// callers can therefore preview and publish the returned snapshot as one
// ordinary document operation.
struct RoomRelationshipRetarget {
    std::string source_id;
    std::string target_id;
    std::string replacement_target_id;
    RoomRelationKind kind{};
    bool operator==(const RoomRelationshipRetarget&) const = default;
};

// Value-owned semantic snapshot. Construction validates the entire graph and
// throws std::invalid_argument on contradictions; no partial result escapes.
// References declare identity and role, not geometry or document existence.
class RoomRelationshipSnapshot {
public:
    static RoomRelationshipSnapshot create(std::vector<RoomReference> references,
                                           std::vector<RoomRelation> relations);
    [[nodiscard]] const std::vector<RoomReference>& references() const noexcept;
    [[nodiscard]] const std::vector<RoomRelation>& relations() const noexcept;
    [[nodiscard]] std::uint64_t schema_version() const noexcept;
    [[nodiscard]] RoomRelationshipSnapshot retarget(
        const RoomRelationshipRetarget& edit) const;
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static RoomRelationshipSnapshot from_json(const nlohmann::json& value);

private:
    RoomRelationshipSnapshot(std::vector<RoomReference> references,
                             std::vector<RoomRelation> relations);
    std::vector<RoomReference> references_;
    std::vector<RoomRelation> relations_;
    std::uint64_t schema_version_{1};
};

} // namespace sketch
