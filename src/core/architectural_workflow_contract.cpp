#include "sketch/architectural_workflow_contract.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && value.size() <= 256 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "invalid architectural identifier");
}
void canonical(std::vector<std::string>& ids) {
    for (const auto& id : ids) identifier(id);
    std::sort(ids.begin(), ids.end());
    require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "duplicate architectural identifier");
}
const char* name(ArchitecturalAction action) {
    switch (action) {
    case ArchitecturalAction::create: return "create";
    case ArchitecturalAction::select: return "select";
    case ArchitecturalAction::property_edit: return "property_edit";
    case ArchitecturalAction::transform: return "transform";
    case ArchitecturalAction::duplicate: return "duplicate";
    case ArchitecturalAction::erase: return "delete";
    }
    throw std::invalid_argument("unknown architectural operation");
}
const char* name(ArchitecturalOutputKind kind) {
    switch (kind) {
    case ArchitecturalOutputKind::plan: return "plan";
    case ArchitecturalOutputKind::elevation: return "elevation";
    case ArchitecturalOutputKind::section: return "section";
    case ArchitecturalOutputKind::view_3d: return "3d";
    case ArchitecturalOutputKind::schedule: return "schedule";
    }
    throw std::invalid_argument("unknown architectural output kind");
}
}

bool can_transform_architectural_entity_type(std::string_view type) noexcept {
    return type == "wall" || type == "slab" || type == "room" ||
           type == "column" || type == "beam" || type == "stair" ||
           type == "railing" || type == "roof";
}

ArchitecturalTransaction ArchitecturalTransaction::create(std::string id, std::string base_revision,
    std::vector<std::string> existing_ids, std::vector<ArchitecturalOperation> operations,
    std::string undo_label) {
    identifier(id); identifier(base_revision); canonical(existing_ids);
    require(!undo_label.empty() && undo_label.find_first_not_of(" \t\r\n") != std::string::npos,
        "undo label must not be blank");
    require(!operations.empty(), "transaction must contain operations");
    std::set<std::string> live(existing_ids.begin(), existing_ids.end());
    // IDs cannot be recycled in one transaction, even after deletion.
    auto allocated = live;
    for (const auto& op : operations) {
        (void)name(op.action); identifier(op.object_id);
        const bool creates = op.action == ArchitecturalAction::create;
        require(creates ? !allocated.contains(op.object_id) : live.contains(op.object_id),
            "operation references missing or already allocated object");
        require(creates || op.semantic_type.empty(), "unexpected semantic type payload");
        require(op.action == ArchitecturalAction::duplicate || op.duplicate_id.empty(), "unexpected duplicate payload");
        require(creates || op.action == ArchitecturalAction::property_edit || op.properties.empty(),
            "unexpected property payload");
        require((op.action == ArchitecturalAction::transform) == op.transform.has_value(),
            "missing or unexpected transform payload");
        for (const auto& [key, value] : op.properties) { identifier(key); (void)value; }
        switch (op.action) {
        case ArchitecturalAction::create:
            identifier(op.semantic_type); live.insert(op.object_id); allocated.insert(op.object_id); break;
        case ArchitecturalAction::property_edit:
            require(!op.properties.empty(), "property edit must contain properties"); break;
        case ArchitecturalAction::duplicate:
            identifier(op.duplicate_id);
            require(!allocated.contains(op.duplicate_id), "duplicate target identity already allocated");
            live.insert(op.duplicate_id); allocated.insert(op.duplicate_id); break;
        case ArchitecturalAction::erase: live.erase(op.object_id); break;
        case ArchitecturalAction::select: break;
        case ArchitecturalAction::transform: {
            const auto& t = *op.transform;
            require(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) &&
                std::isfinite(t.rotation_z_radians) && std::isfinite(t.scale) && t.scale > 0,
                "transform must be finite with positive scale");
            break;
        }
        }
    }
    ArchitecturalTransaction result;
    result.id_ = std::move(id); result.base_revision_ = std::move(base_revision);
    result.undo_label_ = std::move(undo_label); result.existing_ids_ = std::move(existing_ids);
    result.resulting_ids_.assign(live.begin(), live.end()); result.operations_ = std::move(operations);
    return result;
}
nlohmann::json ArchitecturalTransaction::to_json() const {
    auto ops = nlohmann::json::array();
    for (const auto& op : operations_) {
        nlohmann::json value{{"action", name(op.action)}, {"object_id", op.object_id}};
        if (op.action == ArchitecturalAction::create) value["semantic_type"] = op.semantic_type;
        if (!op.properties.empty()) value["properties"] = op.properties;
        if (!op.duplicate_id.empty()) value["duplicate_id"] = op.duplicate_id;
        if (op.transform) {
            const auto& t = *op.transform;
            value["transform"] = {{"translation_m", {t.x, t.y, t.z}},
                {"rotation_z_radians", t.rotation_z_radians}, {"uniform_scale", t.scale}};
        }
        ops.push_back(std::move(value));
    }
    return {{"schema", "architectural_transaction_v1"}, {"id", id_}, {"base_revision", base_revision_},
        {"existing_ids", existing_ids_}, {"resulting_ids", resulting_ids_}, {"operations", ops},
        {"history", {{"intent", "single_undoable_transaction"}, {"label", undo_label_}}}};
}
ArchitecturalOutputContract ArchitecturalOutputContract::create(std::string issue_revision,
    std::string model_revision, std::vector<std::string> object_ids,
    std::vector<std::string> sheet_ids, std::vector<ArchitecturalOutputRequirement> requirements) {
    identifier(issue_revision); identifier(model_revision); canonical(object_ids); canonical(sheet_ids);
    require(!sheet_ids.empty() && !requirements.empty(), "output contract requires sheets and outputs");
    std::set<std::string> ids, used_sheets;
    for (auto& output : requirements) {
        identifier(output.id); (void)name(output.kind); canonical(output.source_ids); identifier(output.sheet_id);
        require(ids.insert(output.id).second, "duplicate output identity");
        require(!output.source_ids.empty(), "output requires explicit architectural source scope");
        for (const auto& source : output.source_ids)
            require(std::binary_search(object_ids.begin(), object_ids.end(), source), "unknown output source");
        require(std::binary_search(sheet_ids.begin(), sheet_ids.end(), output.sheet_id), "unknown output sheet");
        used_sheets.insert(output.sheet_id);
    }
    require(used_sheets.size() == sheet_ids.size(), "sheet has no output requirements");
    std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    ArchitecturalOutputContract result;
    result.issue_revision_ = std::move(issue_revision); result.model_revision_ = std::move(model_revision);
    result.object_ids_ = std::move(object_ids); result.sheet_ids_ = std::move(sheet_ids);
    result.requirements_ = std::move(requirements); return result;
}
nlohmann::json ArchitecturalOutputContract::to_json() const {
    auto requirements = nlohmann::json::array();
    for (const auto& output : requirements_)
        requirements.push_back({{"id", output.id}, {"kind", name(output.kind)},
            {"source_ids", output.source_ids}, {"sheet_id", output.sheet_id}});
    return {{"schema", "architectural_output_contract_v1"}, {"issue_revision", issue_revision_},
        {"model_revision", model_revision_}, {"object_ids", object_ids_}, {"sheet_ids", sheet_ids_},
        {"requirements", requirements}};
}
} // namespace sketch
