#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {
enum class ArchitecturalAction { create, select, property_edit, transform, duplicate, erase };
// Intent in model metres/radians, uniform positive scale; execution is adapter-owned.
struct ArchitecturalTransform {
    double x{}, y{}, z{}, rotation_z_radians{}, scale{1};
};
struct ArchitecturalOperation {
    ArchitecturalAction action;
    std::string object_id;
    std::string semantic_type;
    std::string duplicate_id;
    std::map<std::string, std::string> properties;
    std::optional<ArchitecturalTransform> transform;
};

// Validated immutable descriptor, not a Document mutation or history implementation.
class ArchitecturalTransaction final {
public:
    [[nodiscard]] static ArchitecturalTransaction create(std::string id, std::string base_revision,
        std::vector<std::string> existing_ids, std::vector<ArchitecturalOperation> operations,
        std::string undo_label);
    [[nodiscard]] const std::vector<std::string>& resulting_ids() const noexcept { return resulting_ids_; }
    [[nodiscard]] nlohmann::json to_json() const;
private:
    ArchitecturalTransaction() = default;
    std::string id_, base_revision_, undo_label_;
    std::vector<std::string> existing_ids_, resulting_ids_;
    std::vector<ArchitecturalOperation> operations_;
};

enum class ArchitecturalOutputKind { plan, elevation, section, view_3d, schedule };
struct ArchitecturalOutputRequirement {
    std::string id;
    ArchitecturalOutputKind kind;
    std::vector<std::string> source_ids;
    std::string sheet_id;
};
// An issue/revision groups explicitly scoped outputs at one model revision.
// Requirements do not certify generation, freshness, or completeness of deliverables.
class ArchitecturalOutputContract final {
public:
    [[nodiscard]] static ArchitecturalOutputContract create(std::string issue_revision,
        std::string model_revision, std::vector<std::string> object_ids,
        std::vector<std::string> sheet_ids, std::vector<ArchitecturalOutputRequirement> requirements);
    [[nodiscard]] nlohmann::json to_json() const;
private:
    ArchitecturalOutputContract() = default;
    std::string issue_revision_, model_revision_;
    std::vector<std::string> object_ids_, sheet_ids_;
    std::vector<ArchitecturalOutputRequirement> requirements_;
};
} // namespace sketch
