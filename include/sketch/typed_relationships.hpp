#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

enum class RelationshipKind { wall_derived, room_boundary, appraisal_measurement_boundary };
enum class RelationshipState { connected, frozen, disconnected };
enum class RelationshipErrorCode {
    invalid_input, limit_exceeded, duplicate_id, dangling_reference, ambiguous_target,
    cycle, missing_relationship, invalid_transition
};

class RelationshipError final : public std::invalid_argument {
public:
    RelationshipError(RelationshipErrorCode code, std::string message);
    [[nodiscard]] RelationshipErrorCode code() const noexcept { return code_; }
private:
    RelationshipErrorCode code_;
};

struct TypedRelationship {
    std::string id;
    RelationshipKind kind{RelationshipKind::wall_derived};
    std::string owner_id;
    std::string source_id;
    std::string target_id;
    RelationshipState state{RelationshipState::connected};
    bool operator==(const TypedRelationship&) const = default;
};

// Value snapshots: operations validate a candidate and return a new graph.
// No mutable references or shared writable storage escape this object.
class TypedRelationshipGraph {
public:
    static constexpr std::size_t maximum_objects = 4096;
    static constexpr std::size_t maximum_relationships = 8192;

    explicit TypedRelationshipGraph(std::vector<std::string> object_ids = {},
                                    std::vector<TypedRelationship> relationships = {});
    [[nodiscard]] const std::vector<std::string>& object_ids() const noexcept { return object_ids_; }
    [[nodiscard]] const std::vector<TypedRelationship>& relationships() const noexcept { return relationships_; }
    [[nodiscard]] TypedRelationshipGraph create(TypedRelationship relationship) const;
    [[nodiscard]] TypedRelationshipGraph freeze(std::string_view relationship_id) const;
    [[nodiscard]] TypedRelationshipGraph disconnect(std::string_view relationship_id) const;
    // Sorted, versioned JSON; deserialization / Document integration are separate work.
    [[nodiscard]] std::string serialize() const;

private:
    std::vector<std::string> object_ids_;
    std::vector<TypedRelationship> relationships_;
    [[nodiscard]] TypedRelationshipGraph transition(std::string_view id, RelationshipState state) const;
};

} // namespace sketch
