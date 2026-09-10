#include "sketch/boundary_commit.hpp"

#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {

namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

void require_context(const DocumentSnapshot& snapshot,
                     const DrawingContext& requested) {
    if (!requested.complete()) {
        invalid("boundary commit drawing context must contain property, building, floor and layer IDs");
    }

    const auto organization = organize_project(snapshot);
    const auto layer = organization.nodes.find(requested.layer_id);
    if (layer == organization.nodes.end() || layer->second.type != "layer") {
        invalid("boundary commit context layer ID does not identify a layer entity");
    }
    const auto actual = organization.drawing_context(requested.layer_id);
    if (!actual.has_value()) {
        invalid("boundary commit context layer has no resolved drawing context");
    }
    if (*actual != requested) {
        invalid("boundary commit drawing context does not match the resolved layer context");
    }
}

void add_context(Json& properties, const DrawingContext& context) {
    properties["property_id"] = context.property_id;
    properties["building_id"] = context.building_id;
    properties["floor_id"] = context.floor_id;
    properties["layer_id"] = context.layer_id;
}

Entity encode_boundary(const AcceptedBoundaryChain& chain,
                       const BoundaryAuthoringOptions& options,
                       const DrawingContext& context) {
    auto result = encode_identified_boundary_entity(chain.boundary);
    auto& properties = result.properties;
    add_context(properties, context);
    properties["classification"] = chain.classification;
    // These fields intentionally match MainWindow::createBoundary exactly.
    properties["factor"] = 1.0;
    properties["factor_expression"] = "1";
    properties["factor_numerator"] = 1;
    properties["factor_denominator"] = 1;

    const auto envelope = boundary_construction_envelope(chain, options);
    if (!envelope.is_object()) {
        invalid("boundary construction envelope must be a JSON object");
    }
    properties["boundary_authoring"] = envelope;
    return result;
}

Entity encode_dimension(const BoundaryDimension& dimension,
                        const DrawingContext& context) {
    auto result = encode_boundary_dimension_entity(dimension);
    add_context(result.properties, context);
    return result;
}

void require_new_entity_id(const std::map<std::string, Entity, std::less<>>& source,
                           const std::set<std::string, std::less<>>& created,
                           std::string_view id,
                           std::string_view label) {
    if (id.empty()) invalid(std::string(label) + " ID is empty");
    if (source.contains(id)) {
        invalid(std::string(label) + " ID already exists in the document: " + std::string(id));
    }
    if (created.contains(id)) {
        invalid(std::string(label) + " ID is created more than once: " + std::string(id));
    }
}

std::vector<Entity> encode_new_entities(const DocumentSnapshot& snapshot,
                                        const BoundaryCommitIntent& intent,
                                        std::vector<std::string>& created_boundary_ids) {
    if (intent.chains.empty()) invalid("boundary commit requires at least one accepted chain");
    require_context(snapshot, intent.context);

    std::set<std::string, std::less<>> created_entity_ids;
    std::set<std::string, std::less<>> created_segment_ids;
    std::map<std::string, std::string, std::less<>> created_vertex_owners;
    std::vector<Entity> result;
    for (const auto& chain : intent.chains) {
        if (!chain.classified) invalid("boundary commit requires every chain to be classified");
        if (chain.classification.empty()) {
            invalid("boundary commit requires a nonempty classification for every chain");
        }

        // Replay is the geometry and receipt authority. The source Segment
        // values are not trusted by the encoder until this exact check passes.
        verify_accepted_chain(chain, intent.options);
        require_new_entity_id(snapshot.entities(), created_entity_ids,
                              chain.boundary.id, "boundary");
        created_entity_ids.insert(chain.boundary.id);
        created_boundary_ids.push_back(chain.boundary.id);

        for (const auto& edge : chain.boundary.segments) {
            if (!created_segment_ids.insert(edge.segment_id).second) {
                invalid("boundary commit creates a duplicate segment ID: " + edge.segment_id);
            }
            for (const auto* vertex_id : {&edge.start_vertex_id, &edge.end_vertex_id}) {
                const auto [owner, inserted] =
                    created_vertex_owners.emplace(*vertex_id, chain.boundary.id);
                if (!inserted && owner->second != chain.boundary.id) {
                    invalid("boundary commit reuses a vertex ID across boundaries: " +
                            *vertex_id);
                }
            }
        }

        result.push_back(encode_boundary(chain, intent.options, intent.context));
        for (const auto& dimension : chain.dimensions) {
            require_new_entity_id(snapshot.entities(), created_entity_ids,
                                  dimension.id, "dimension");
            created_entity_ids.insert(dimension.id);
            result.push_back(encode_dimension(dimension, intent.context));
        }
    }
    return result;
}

ApplyEntityChanges command_for(const DocumentSnapshot& snapshot,
                               const BoundaryCommitIntent& intent,
                               const std::vector<Entity>& created) {
    std::vector<EntityChange> changes;
    changes.reserve(created.size());
    for (const auto& entity : created) changes.push_back(EntityChange::upsert(entity));
    return ApplyEntityChanges{
        .expected_revision = snapshot.revision(),
        .entity_changes = std::move(changes),
        .asset_changes = {},
        .message = intent.message.empty() ? "Commit boundary authoring" : intent.message,
    };
}

}  // namespace

class BoundaryCommitBuilder final {
public:
    static BoundaryCommitPreview build(const DocumentSnapshot& snapshot,
                                       const BoundaryCommitIntent& raw_intent) {
        BoundaryCommitPreview result;
        result.document_id_ = snapshot.document_id();
        result.expected_revision_ = snapshot.revision();
        result.source_snapshot_digest_ = document_snapshot_digest(snapshot);
        result.candidate_entities_ = snapshot.entities();
        // Retain a private copy before any validation. Apply always rebuilds
        // from this value, never from a caller-owned intent or display map.
        result.normalized_intent_ = raw_intent;
        if (result.normalized_intent_.message.empty()) {
            result.normalized_intent_.message = "Commit boundary authoring";
        }

        try {
            if (!snapshot.is_editable()) {
                invalid(snapshot.read_only_reason().empty() ? "Document is read-only"
                                                            : snapshot.read_only_reason());
            }

            std::vector<std::string> created_boundary_ids;
            const auto created = encode_new_entities(
                snapshot, result.normalized_intent_, created_boundary_ids);
            auto candidate = snapshot.entities();
            for (const auto& entity : created) {
                if (!candidate.emplace(entity.id, entity).second) {
                    invalid("boundary commit created duplicate entity ID: " + entity.id);
                }
            }

            const auto command = command_for(snapshot, result.normalized_intent_, created);
            const auto trial = Document::preview_command(snapshot, Command{command});
            if (trial.entities() != candidate) {
                invalid("boundary commit trial does not match its encoded candidate");
            }

            result.accepted_ = true;
            result.candidate_entities_ = std::move(candidate);
            result.candidate_digest_ = entity_map_digest(result.candidate_entities_);
            result.created_boundary_ids_ = std::move(created_boundary_ids);
            result.trial_snapshot_digest_ = document_snapshot_digest(trial);
            return result;
        } catch (const std::exception& error) {
            result.accepted_ = false;
            result.candidate_entities_ = snapshot.entities();
            result.candidate_digest_.clear();
            result.trial_snapshot_digest_.clear();
            result.created_boundary_ids_.clear();
            result.diagnostics_.clear();
            result.diagnostics_.push_back(error.what());
            return result;
        }
    }
};

bool BoundaryCommitPreview::accepted() const noexcept { return accepted_; }
const std::string& BoundaryCommitPreview::document_id() const noexcept { return document_id_; }
Revision BoundaryCommitPreview::expected_revision() const noexcept { return expected_revision_; }
const std::string& BoundaryCommitPreview::source_snapshot_digest() const noexcept {
    return source_snapshot_digest_;
}
const std::string& BoundaryCommitPreview::candidate_digest() const noexcept {
    return candidate_digest_;
}
const std::map<std::string, Entity, std::less<>>&
BoundaryCommitPreview::candidate_entities() const noexcept {
    return candidate_entities_;
}
const std::vector<std::string>& BoundaryCommitPreview::created_boundary_ids() const noexcept {
    return created_boundary_ids_;
}
const std::vector<std::string>& BoundaryCommitPreview::diagnostics() const noexcept {
    return diagnostics_;
}

BoundaryCommitPreview preview_boundary_commit(const DocumentSnapshot& snapshot,
                                              const BoundaryCommitIntent& intent) {
    return BoundaryCommitBuilder::build(snapshot, intent);
}

Revision apply_boundary_commit(Document& document,
                               const BoundaryCommitPreview& preview) {
    if (!preview.accepted_) invalid("a rejected boundary commit preview cannot be applied");

    const auto current = document.snapshot();
    if (current.document_id() != preview.document_id_) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "boundary commit preview belongs to another document");
    }
    if (current.revision() != preview.expected_revision_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "boundary commit preview revision is stale");
    }
    if (document_snapshot_digest(current) != preview.source_snapshot_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "boundary commit preview source snapshot has changed");
    }
    if (entity_map_digest(preview.candidate_entities_) != preview.candidate_digest_) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "boundary commit preview candidate display was modified");
    }

    const auto recomputed = BoundaryCommitBuilder::build(current, preview.normalized_intent_);
    if (!recomputed.accepted_ || recomputed.candidate_digest_ != preview.candidate_digest_ ||
        recomputed.created_boundary_ids_ != preview.created_boundary_ids_ ||
        recomputed.diagnostics_ != preview.diagnostics_ ||
        recomputed.trial_snapshot_digest_ != preview.trial_snapshot_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "boundary commit preview no longer reproduces the shown result");
    }

    std::vector<Entity> created;
    created.reserve(recomputed.candidate_entities_.size() - current.entities().size());
    for (const auto& [id, entity] : recomputed.candidate_entities_) {
        if (!current.entities().contains(id)) created.push_back(entity);
    }
    if (created.empty()) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "boundary commit preview does not contain a document change");
    }

    const auto command = command_for(current, recomputed.normalized_intent_, created);
    // The same private command shape was validated during preview. This
    // second trial is intentionally before the live apply, preserving the
    // no-mutation guarantee for every rejection path.
    const auto trial = Document::preview_command(current, Command{command});
    if (trial.entities() != recomputed.candidate_entities() ||
        document_snapshot_digest(trial) != recomputed.trial_snapshot_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "boundary commit trial changed before application");
    }
    return document.apply(command);
}

}  // namespace sketch
