#include "sketch/boundary_commit.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;
using sketch::AcceptedBoundaryChain;
using sketch::BoundaryAuthoringMode;
using sketch::BoundaryAuthoringOptions;
using sketch::BoundaryAuthoringSession;
using sketch::BoundaryCommitIntent;
using sketch::BoundaryCommitPreview;
using sketch::BoundaryDimension;
using sketch::Document;
using sketch::DocumentSnapshot;
using sketch::Entity;
using sketch::EntityChange;
using sketch::DrawingContext;
using sketch::Revision;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Entity entity(std::string id, std::string type, json properties = json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, json::object()};
}

Document make_document() {
    return Document::create({
        entity("property-1", "property", {{"name", "Property"}}),
        entity("building-1", "building", {{"property_id", "property-1"}, {"name", "Building"}}),
        entity("floor-1", "floor", {{"building_id", "building-1"}, {"name", "Floor"}}),
        entity("layer-1", "layer", {{"floor_id", "floor-1"}, {"name", "Layer"}}),
    });
}

DrawingContext context() {
    return {"property-1", "building-1", "floor-1", "layer-1"};
}

BoundaryAuthoringOptions options_for(BoundaryAuthoringMode mode) {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = mode == BoundaryAuthoringMode::draw_first;
    return options;
}

AcceptedBoundaryChain rectangle(BoundaryAuthoringMode mode,
                                 const BoundaryAuthoringOptions& options) {
    BoundaryAuthoringSession session(mode, options);
    session.set_classification("living_area");
    (void)session.anchor({0.0, 0.0});
    (void)session.add_line_rise_run(sketch::parse_quantity("0 m"),
                                    sketch::parse_quantity("4 m"));
    if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_manual_dimension({2.0, 0.25});
    (void)session.add_line_rise_run(sketch::parse_quantity("3 m"),
                                    sketch::parse_quantity("0 m"));
    if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_manual_dimension({3.75, 1.5});
    (void)session.add_line_rise_run(sketch::parse_quantity("0 m"),
                                    sketch::parse_quantity("-4 m"));
    if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_manual_dimension({2.0, 2.75});
    (void)session.add_closing_segment();
    if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_manual_dimension({0.25, 1.5});
    return session.close_chain();
}

BoundaryCommitIntent intent_for(const AcceptedBoundaryChain& chain,
                                const BoundaryAuthoringOptions& options) {
    BoundaryCommitIntent intent;
    intent.options = options;
    intent.chains = {chain};
    intent.context = context();
    intent.message = "commit test boundary";
    return intent;
}

void require_rejected(const BoundaryCommitPreview& preview, std::string_view message) {
    require(!preview.accepted(), message);
    require(!preview.diagnostics().empty(), "rejected boundary commit needs diagnostics");
}

template <typename Function>
void expect_document_unchanged(Document& document, Function&& operation,
                               std::string_view message) {
    const auto before = document.snapshot();
    const auto before_digest = sketch::document_snapshot_digest(before);
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
    require(sketch::document_snapshot_digest(document.snapshot()) == before_digest,
            "rejected boundary commit mutated document history");
    require(document.snapshot().entities() == before.entities(),
            "rejected boundary commit mutated document entities");
}

void require_exact_context_fields(const Entity& value, const DrawingContext& expected) {
    require(value.properties.at("property_id") == expected.property_id,
            "boundary commit property context mismatch");
    require(value.properties.at("building_id") == expected.building_id,
            "boundary commit building context mismatch");
    require(value.properties.at("floor_id") == expected.floor_id,
            "boundary commit floor context mismatch");
    require(value.properties.at("layer_id") == expected.layer_id,
            "boundary commit layer context mismatch");
}

void test_both_modes_encode_geometry_dimensions_and_factor_defaults() {
    for (const auto mode : {BoundaryAuthoringMode::draw_first,
                            BoundaryAuthoringMode::define_first}) {
        const auto options = options_for(mode);
        auto chain = rectangle(mode, options);
        auto document = make_document();
        const auto preview = sketch::preview_boundary_commit(
            document.snapshot(), intent_for(chain, options));
        require(preview.accepted(), "valid boundary commit preview was rejected");
        require(preview.candidate_entities().size() ==
                    document.snapshot().entities().size() + 1 + chain.dimensions.size(),
                "preview must contain source entities and all boundary dimensions");
        const auto& boundary = preview.candidate_entities().at(chain.boundary.id);
        require(boundary.type == "measurement_boundary",
                "commit must encode the recognized measurement boundary type");
        require_exact_context_fields(boundary, context());
        require(boundary.properties.at("classification") == chain.classification,
                "commit must preserve boundary classification");
        require(boundary.properties.at("factor") == 1.0 &&
                    boundary.properties.at("factor_expression") == "1" &&
                    boundary.properties.at("factor_numerator") == 1 &&
                    boundary.properties.at("factor_denominator") == 1,
                "commit must use the createBoundary factor-one defaults");
        require(boundary.properties.contains("boundary_authoring"),
                "commit must retain the construction envelope");
        require(sketch::decode_identified_boundary_entity(boundary) == chain.boundary,
                "commit boundary geometry or stable identities changed");
        for (const auto& dimension : chain.dimensions) {
            const auto& encoded = preview.candidate_entities().at(dimension.id);
            require(encoded.type == "dimension", "dimension entity type changed");
            require_exact_context_fields(encoded, context());
            require(sketch::decode_boundary_dimension_entity(encoded).dimension == dimension,
                    "commit dimension geometry or stable identity changed");
        }
        require(preview.candidate_digest() ==
                    sketch::entity_map_digest(preview.candidate_entities()),
                "preview candidate digest must bind the displayed candidate map");
    }
}

void test_preview_is_deterministic_and_owns_a_copy_of_intent() {
    const auto options = options_for(BoundaryAuthoringMode::draw_first);
    const auto chain = rectangle(BoundaryAuthoringMode::draw_first, options);
    auto document = make_document();
    auto intent = intent_for(chain, options);
    const auto first = sketch::preview_boundary_commit(document.snapshot(), intent);
    const auto second = sketch::preview_boundary_commit(document.snapshot(), intent);
    require(first.accepted() && second.accepted(), "repeatable valid preview was rejected");
    require(first.candidate_digest() == second.candidate_digest() &&
                first.candidate_entities() == second.candidate_entities() &&
                first.created_boundary_ids() == second.created_boundary_ids() &&
                first.diagnostics() == second.diagnostics(),
            "repeat previews must produce identical sealed display values");

    auto copied = intent;
    const auto preview = sketch::preview_boundary_commit(document.snapshot(), copied);
    copied.chains.front().boundary.segments.front().segment.end.x += 9.0;
    copied.context.layer_id = "other-layer";
    require(preview.accepted(), "valid copied intent preview was rejected");
    (void)sketch::apply_boundary_commit(document, preview);
    require(document.snapshot().entities().contains(chain.boundary.id),
            "preview must apply its retained intent after caller mutation");
}

void test_invalid_input_rejects_without_source_mutation() {
    const auto options = options_for(BoundaryAuthoringMode::draw_first);
    const auto chain = rectangle(BoundaryAuthoringMode::draw_first, options);
    const auto snapshot = make_document().snapshot();

    auto missing = intent_for(chain, options);
    missing.chains.clear();
    require_rejected(sketch::preview_boundary_commit(snapshot, missing),
                     "missing chains must reject");

    auto bad_context = intent_for(chain, options);
    bad_context.context.layer_id = "property-1";
    require_rejected(sketch::preview_boundary_commit(snapshot, bad_context),
                     "context with a non-layer node must reject");

    auto unclassified = intent_for(chain, options);
    unclassified.chains.front().classified = false;
    unclassified.chains.front().classification.clear();
    require_rejected(sketch::preview_boundary_commit(snapshot, unclassified),
                     "unclassified chain must reject");

    auto malformed_receipt = intent_for(chain, options);
    require(malformed_receipt.chains.front().receipts.front().rise.has_value(),
            "malformed-input fixture must start with a present required rise");
    malformed_receipt.chains.front().receipts.front().rise.reset();
    require_rejected(sketch::preview_boundary_commit(snapshot, malformed_receipt),
                     "malformed construction receipt must reject");

    auto geometry_tampered = intent_for(chain, options);
    geometry_tampered.chains.front().boundary.segments.front().segment.end.x += 0.01;
    require_rejected(sketch::preview_boundary_commit(snapshot, geometry_tampered),
                     "geometry that disagrees with its receipt must reject");

    auto existing_id = intent_for(chain, options);
    existing_id.chains.front().boundary.id = "layer-1";
    require_rejected(sketch::preview_boundary_commit(snapshot, existing_id),
                     "boundary identity collision must reject");

    auto duplicate = intent_for(chain, options);
    duplicate.chains.push_back(chain);
    require_rejected(sketch::preview_boundary_commit(snapshot, duplicate),
                     "duplicate boundary creation must reject");
}

void test_apply_is_atomic_single_undo_redo_and_rejects_tampering() {
    const auto options = options_for(BoundaryAuthoringMode::define_first);
    const auto chain = rectangle(BoundaryAuthoringMode::define_first, options);
    auto document = make_document();
    const auto before = document.snapshot();
    const auto preview = sketch::preview_boundary_commit(
        before, intent_for(chain, options));
    require(preview.accepted(), "valid define-first preview was rejected");
    const auto applied_revision = sketch::apply_boundary_commit(document, preview);
    require(applied_revision == before.revision() + 1,
            "boundary commit must be one document revision");
    const auto committed = document.snapshot();
    require(committed.entities() == preview.candidate_entities(),
            "applied entities must equal the displayed preview candidate");
    require(document.can_undo() && !document.can_redo(),
            "boundary commit must create one undoable command");
    (void)document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(),
            "undo must remove boundary and dimensions as one command");
    require(document.can_redo(), "undo must expose a redo for the boundary command");
    (void)document.redo(document.revision());
    require(document.snapshot().entities() == committed.entities(),
            "redo must restore boundary and dimensions as one command");

    auto tampered = preview;
    auto& candidates = const_cast<std::map<std::string, Entity, std::less<>>&>(
        tampered.candidate_entities());
    candidates.at(chain.boundary.id).properties["classification"] = "tampered";
    auto tamper_document = make_document();
    expect_document_unchanged(tamper_document, [&] {
        (void)sketch::apply_boundary_commit(tamper_document, tampered);
    }, "public candidate mutation must reject");

    auto created_ids_tampered = preview;
    auto& created_ids = const_cast<std::vector<std::string>&>(
        created_ids_tampered.created_boundary_ids());
    created_ids.push_back("forged-boundary");
    auto created_ids_document = make_document();
    expect_document_unchanged(created_ids_document, [&] {
        (void)sketch::apply_boundary_commit(created_ids_document, created_ids_tampered);
    }, "public created-ID mutation must reject");
}

void test_foreign_stale_altered_source_and_retired_id_reject() {
    const auto options = options_for(BoundaryAuthoringMode::draw_first);
    const auto chain = rectangle(BoundaryAuthoringMode::draw_first, options);
    auto document = make_document();
    const auto preview = sketch::preview_boundary_commit(
        document.snapshot(), intent_for(chain, options));
    require(preview.accepted(), "valid preview for identity tests was rejected");

    auto foreign = make_document();
    expect_document_unchanged(foreign, [&] {
        (void)sketch::apply_boundary_commit(foreign, preview);
    }, "foreign document must reject a boundary preview");

    auto stale = make_document();
    const auto stale_preview = sketch::preview_boundary_commit(
        stale.snapshot(), intent_for(chain, options));
    auto property = stale.snapshot().entities().at("property-1");
    property.properties["name"] = "changed";
    stale.apply(sketch::ApplyEntityChanges{stale.revision(),
                                           {EntityChange::upsert(property)}, {}, "advance"});
    expect_document_unchanged(stale, [&] {
        (void)sketch::apply_boundary_commit(stale, stale_preview);
    }, "stale document revision must reject a boundary preview");

    auto altered_snapshot = document.snapshot();
    auto& altered_head = const_cast<std::vector<sketch::RevisionRecord>&>(
        altered_snapshot.history());
    altered_head.at(static_cast<std::size_t>(altered_snapshot.revision()))
        .entities.at("property-1").properties["name"] = "forged same revision";
    const auto altered_preview = sketch::preview_boundary_commit(
        altered_snapshot, intent_for(chain, options));
    require(altered_preview.accepted(), "same-revision altered source must preview independently");
    expect_document_unchanged(document, [&] {
        (void)sketch::apply_boundary_commit(document, altered_preview);
    }, "same-ID same-revision altered source must reject");

    auto retired = make_document();
    const auto first = sketch::preview_boundary_commit(
        retired.snapshot(), intent_for(chain, options));
    require(first.accepted(), "retired-ID seed preview was rejected");
    (void)sketch::apply_boundary_commit(retired, first);
    const auto committed_entities = retired.snapshot().entities();
    (void)retired.undo(retired.revision());
    const auto retired_collision = sketch::preview_boundary_commit(
        retired.snapshot(), intent_for(chain, options));
    require_rejected(retired_collision,
                     "retired boundary identity must not be created again on a branch");
    const auto undone_digest = sketch::document_snapshot_digest(retired.snapshot());
    auto candidate = sketch::Document::fork(retired.snapshot());
    require(sketch::document_snapshot_digest(candidate.snapshot()) == undone_digest,
            "private candidate must retain the full undone history");
    require_rejected(sketch::preview_boundary_commit(candidate.snapshot(), intent_for(chain, options)),
                     "validated fork must preserve the retired identity ledger");
    candidate.redo(candidate.revision());
    require(candidate.snapshot().entities() == committed_entities,
            "exact redo in a fork must restore the original boundary and dimension identities");
    require(sketch::document_snapshot_digest(retired.snapshot()) == undone_digest,
            "candidate redo must not alter its source document");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_both_modes_encode_geometry_dimensions_and_factor_defaults();
        test_preview_is_deterministic_and_owns_a_copy_of_intent();
        test_invalid_input_rejects_without_source_mutation();
        test_apply_is_atomic_single_undo_redo_and_rejects_tampering();
        test_foreign_stale_altered_source_and_retired_id_reject();
        std::cout << "Boundary commit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
