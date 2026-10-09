#include "sketch/drawing_removal_command.hpp"

#include <stdexcept>

namespace sketch {
namespace {
bool exact(const Entity& left,const Entity& right) {
    return left==right && left.properties.dump()==right.properties.dump() &&
        left.extensions.dump()==right.extensions.dump();
}
bool exact_entities(const DrawingSelectionRemovalEntities& left,const DrawingSelectionRemovalEntities& right) {
    if (left.size()!=right.size()) return false;
    for (const auto& [id,entity]:left) {
        const auto found=right.find(id);
        if (found==right.end() || !exact(entity,found->second)) return false;
    }
    return true;
}
void metadata(const DocumentSnapshot& source,const DocumentSnapshot& candidate) {
    if (!candidate.is_editable() || candidate.document_id()!=source.document_id() ||
        candidate.assets()!=source.assets() || candidate.read_only_reason()!=source.read_only_reason() ||
        candidate.named_revisions()!=source.named_revisions() ||
        candidate.saved_revision_optional()!=source.saved_revision_optional())
        throw std::invalid_argument("Drawing removal changed captured assets or project metadata.");
    for (const auto& [id,asset]:source.assets())
        if (asset.metadata.dump()!=candidate.assets().at(id).metadata.dump())
            throw std::invalid_argument("Drawing removal changed retained asset metadata.");
}
} // namespace

Command complete_drawing_removal_command(const DocumentSnapshot& source,const Command& original,
    const DrawingSelectionRemovalIntent& intent) {
    if (!source.is_editable()) throw std::invalid_argument("This project is read-only.");
    const auto selection=encode_drawing_selection_removal_intent(intent);
    // Actual-source analytical admission precedes any retained geometry replay.
    (void)replay_drawing_selection_removal(source.entities(),intent,source.uses_active_phase_constraints());
    if (std::visit([&](const auto& command){return command.expected_revision!=source.revision();},original))
        throw std::invalid_argument("The drawing removal operation belongs to another source revision.");
    if (const auto* raw=std::get_if<ApplyEntityChanges>(&original)) {
        if (!raw->asset_changes.empty()) throw std::invalid_argument("Drawing removal cannot change assets.");
    } else if (const auto* reviewed=std::get_if<ApplyBoundaryConstraintChanges>(&original)) {
        const bool phase_authoring=reviewed->phase_constraint_authoring_completion &&
            !reviewed->phase_constraint_authoring_intent.is_null();
        if (reviewed->independent_drawing_removal_completion || !reviewed->independent_drawing_removal_intent.is_null() ||
            (!(reviewed->room_review_completion && reviewed->room_review_geometry_completion) &&
                !reviewed->phase_room_review_completion && !phase_authoring))
            throw std::invalid_argument("Drawing removal requires one complete unnested wall/room review or active design authoring.");
        // Let the strict command mode reject competing authorities before geometry replay.
        const auto proof=command_to_json(original);
        if (proof.dump().size()>1024*1024 || command_to_json(command_from_json(proof)).dump()!=proof.dump() ||
            ((reviewed->phase_constraint_authoring_completion || !reviewed->phase_constraint_authoring_intent.is_null()) &&
                (!phase_authoring || proof.at("version")!=34)))
            throw std::invalid_argument("Drawing removal requires an exactly retained unnested preceding operation.");
    } else throw std::invalid_argument("The selected drawing removal has no supported complete operation.");
    const auto stage=Document::preview_command(source,original);
    metadata(source,stage);
    const auto expected=replay_drawing_selection_removal_after_review(
        source.entities(),stage.entities(),intent,stage.uses_active_phase_constraints());
    Command completed;
    if (const auto* raw=std::get_if<ApplyEntityChanges>(&original)) {
        ApplyEntityChanges result{source.revision(),{}, {},raw->message};
        for (const auto& [id,entity]:source.entities()) {
            const auto after=expected.find(id);
            if (after==expected.end()) result.entity_changes.push_back(EntityChange::erase(id));
            else if (!exact(entity,after->second)) result.entity_changes.push_back(EntityChange::upsert(after->second));
        }
        for (const auto& [id,entity]:expected)
            if (!source.entities().contains(id)) result.entity_changes.push_back(EntityChange::upsert(entity));
        if (result.entity_changes.empty() || result.entity_changes.size()>4096)
            throw std::invalid_argument("The complete drawing removal change inventory is invalid.");
        completed=std::move(result);
    } else {
        auto result=std::get<ApplyBoundaryConstraintChanges>(original);
        result.independent_drawing_removal_completion=true;
        result.independent_drawing_removal_intent=selection;
        completed=std::move(result);
    }
    const auto wire=command_to_json(completed);
    if (wire.dump().size()>1024*1024 || command_to_json(command_from_json(wire)).dump()!=wire.dump())
        throw std::invalid_argument("The complete drawing removal cannot be retained exactly.");
    const auto candidate=Document::preview_command(source,completed);
    metadata(source,candidate);
    if (!exact_entities(candidate.entities(),expected))
        throw std::invalid_argument("Drawing removal differs from complete actual-source reconstruction.");
    return completed;
}
} // namespace sketch
