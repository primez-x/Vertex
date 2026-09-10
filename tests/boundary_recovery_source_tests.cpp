#include "sketch/boundary_recovery_source.hpp"
#include "sketch/boundary_active_recovery.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Document fixture() {
    return Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}},
        {"other", "layer", {{"floor_id", "f"}}}});
}
void check_binding() {
    auto document = fixture();
    const DrawingContext context{"p", "b", "f", "l"};
    const auto original = document.snapshot();
    const auto captured = capture_boundary_recovery_source(original, context);
    BoundaryAuthoringSession draft(BoundaryAuthoringMode::draw_first);
    (void)draft.anchor({1, 2});
    (void)draft.add_line_to({5, 2});
    const auto decoded = decode_boundary_active_recovery(
        encode_boundary_active_recovery({captured, draft.recovery_checkpoint()}));
    require(decoded.supported(), "current active record must decode");
    const auto source = decoded.active->source;
    require(source == captured, "record round trip must preserve the actual captured binding");
    require(inspect_boundary_recovery_source(original, source) ==
                BoundaryRecoverySourceStatus::current, "fresh source must be current");
    document.mark_saved(document.revision());
    require(inspect_boundary_recovery_source(document.snapshot(), source) ==
                BoundaryRecoverySourceStatus::current, "save acknowledgement must not stale source");
    auto foreign = fixture();
    require(inspect_boundary_recovery_source(foreign.snapshot(), source) ==
                BoundaryRecoverySourceStatus::foreign_document, "same geometry is not same document");
    auto wrong_context = source;
    wrong_context.context.floor_id = "other-floor";
    require(inspect_boundary_recovery_source(original, wrong_context) ==
                BoundaryRecoverySourceStatus::mismatched_context, "context must match exact hierarchy");
    wrong_context.context.layer_id = "missing";
    require(inspect_boundary_recovery_source(original, wrong_context) ==
                BoundaryRecoverySourceStatus::missing_context, "missing layer must stay stale");
    bool rejected = false;
    try { (void)capture_boundary_recovery_source(original, wrong_context.context); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "capture cannot silently repair a context");
    auto forged = original;
    const_cast<std::vector<RevisionRecord>&>(forged.history()).front()
        .entities.at("p").properties["name"] = "Changed under same revision";
    require(inspect_boundary_recovery_source(forged, source) ==
                BoundaryRecoverySourceStatus::stale_digest, "same identity and revision cannot hide changes");
    auto changed = original.entities().at("p");
    changed.properties["name"] = "Renamed";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(changed)}, .message = "Rename"});
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(), "undo fixture must recover geometry");
    require(inspect_boundary_recovery_source(document.snapshot(), source) ==
                BoundaryRecoverySourceStatus::stale_revision, "undo to same geometry is still a new lineage");
    auto locked = Document::create({Entity::create("future_required", nlohmann::json::object(), true)});
    require(inspect_boundary_recovery_source(locked.snapshot(), source) ==
                BoundaryRecoverySourceStatus::read_only, "read-only source must not authorize finalization");
    require(document_authoring_source_digest_v1(original) == source.authoring_digest,
            "inspection must not mutate source snapshots");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_binding(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
