#include "support/detached_document_snapshot.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/workspace_slot_validation.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/workspace_lifecycle_validation.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
Document fixture() {
    return Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}, {"label", "label", {{"text", "Original"}}}});
}
BoundaryActiveRecovery input(const DocumentSnapshot& source, BoundaryAuthoringMode mode,
                             bool complete = true, bool classified = true) {
    BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(mode, options);
    if (classified) session.set_classification("living_area");
    (void)session.anchor({0, 0}); (void)session.add_line_to({4, 0});
    (void)session.add_line_to({4, 3}); (void)session.add_line_to({0, 3});
    if (complete) { (void)session.add_closing_segment(); (void)session.close_chain(); }
    auto checkpoint = session.recovery_checkpoint();
    checkpoint.extensions["future"] = {{"number", 1.0}, {"items", Json::array({nullptr, "x"})}};
    return {capture_boundary_recovery_source(source, {"p", "b", "f", "l"}),
            checkpoint, {{"future", {{"number", 2.0}}}}};
}
void activate(ProjectWorkspace& workspace, const BoundaryActiveRecovery& active) {
    auto ticket = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(ticket);
}
void validate_slots(const ProjectWorkspace& workspace) {
    const auto s = workspace.capture();
    validate_workspace_lifecycle_slots(s.document(), s.document_history(), s.lifecycle_history(), s.navigation(),
        s.active_boundary(), s.retired_boundaries(), s.resource_policy());
}
void undo(ProjectWorkspace& workspace) {
    auto ticket = workspace.prepare_undo(); (void)workspace.commit(ticket);
    validate_workspace_document_history(workspace.snapshot(), workspace.document_history());
    validate_slots(workspace);
}
void redo(ProjectWorkspace& workspace) {
    auto ticket = workspace.prepare_redo(); (void)workspace.commit(ticket);
    validate_workspace_document_history(workspace.snapshot(), workspace.document_history());
    validate_slots(workspace);
}
template<class F> void unchanged_rejection(ProjectWorkspace& workspace, F&& operation) {
    const auto before = workspace.capture(); const auto digest = document_snapshot_digest(before.document());
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid finish operation must reject");
    const auto after = workspace.capture();
    require(document_snapshot_digest(after.document()) == digest &&
            after.active_boundary() == before.active_boundary() &&
            after.retired_boundaries().size() == before.retired_boundaries().size() &&
            after.navigation() == before.navigation() && after.document_history() == before.document_history() &&
            after.epoch() == before.epoch() && after.edited_generation() == before.edited_generation() &&
            after.checkpoint_generation() == before.checkpoint_generation(),
            "rejected finish must leave document, draft, retirement and generations unchanged");
}
void check_finish_round_trip() {
    for (auto mode : {BoundaryAuthoringMode::draw_first, BoundaryAuthoringMode::define_first}) {
        auto document = fixture(); const auto source = document.snapshot(); ProjectWorkspace workspace(source);
        const auto active = input(source, mode); const auto ns = active.checkpoint.identity_namespace;
        activate(workspace, active); const auto before = workspace.capture();
        auto finish = workspace.prepare_finish_boundary(); auto preview = finish.preview();
        require(preview.entities().size() == source.entities().size() + 5,
                "finish preview contains one boundary and all four dimensions");
        require(workspace.active_boundary() == std::optional{active} &&
                document_snapshot_digest(workspace.snapshot()) == document_snapshot_digest(source) &&
                workspace.epoch() == before.epoch(), "preparing finish must be isolated");
        auto detached = Document::fork(preview);
        auto label = detached.snapshot().entities().at("label"); label.properties["text"] = "detached";
        detached.apply(ApplyEntityChanges{.expected_revision = detached.revision(),
            .entity_changes = {EntityChange::upsert(label)}, .message = "Mutate preview"});
        (void)workspace.commit(finish); const auto finished = workspace.snapshot();
        require(finished.entities() == preview.entities() && finished.revision() == source.revision() + 1 &&
                finished.history().size() == source.history().size() + 1 && !workspace.active_boundary(),
                "finish publishes one atomic document revision and removes active state");
        require(workspace.epoch() == before.epoch() + 1 &&
                workspace.edited_generation() == before.edited_generation() + 1 &&
                workspace.checkpoint_generation() == before.checkpoint_generation() + 1,
                "finish advances each workspace generation once");
        unchanged_rejection(workspace, [&] { (void)workspace.commit(finish); });
        for (int cycle = 0; cycle < 3; ++cycle) {
            undo(workspace);
            require(workspace.snapshot().entities() == source.entities() && !workspace.active_boundary() &&
                    workspace.retired_boundary(ns) == std::optional{active},
                    "undo finish restores exact source entities and retires original input");
            require(workspace.retired_boundary(ns)->checkpoint.extensions.at("future").at("number").is_number_float(),
                    "retired input preserves opaque floating point values");
            unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(); });
            unchanged_rejection(workspace, [&] { activate(workspace, active); });
            redo(workspace);
            require(workspace.snapshot().entities() == finished.entities() && !workspace.active_boundary() &&
                    !workspace.retired_boundary(ns), "redo finish restores original entity and dimension IDs");
        }
        undo(workspace); const auto retired_capture = workspace.capture(); undo(workspace);
        require(!workspace.active_boundary() && !workspace.retired_boundary(ns),
                "undo activation removes the matching retired session");
        require(retired_capture.retired_boundaries().size() == 1, "retired capture remains detached");
        redo(workspace);
        require(!workspace.active_boundary() && workspace.retired_boundary(ns) == std::optional{active},
                "redo activation preserves retired status");
        redo(workspace);
        require(workspace.snapshot().entities() == finished.entities() && !workspace.retired_boundary(ns),
                "redo finish after activation navigation restores exact geometry");
    }
}
void check_multiple_retired_inputs() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    const auto a = input(workspace.snapshot(), BoundaryAuthoringMode::draw_first);
    const auto a_ns = a.checkpoint.identity_namespace;
    activate(workspace, a);
    auto finish_a = workspace.prepare_finish_boundary(); (void)workspace.commit(finish_a);
    undo(workspace);
    const auto b = input(workspace.snapshot(), BoundaryAuthoringMode::define_first);
    const auto b_ns = b.checkpoint.identity_namespace;
    require(a_ns != b_ns, "new input has a fresh namespace");
    activate(workspace, b);
    auto finish_b = workspace.prepare_finish_boundary(); (void)workspace.commit(finish_b);
    const auto finished_b = workspace.snapshot();
    undo(workspace);
    require(workspace.retired_boundary(a_ns) == std::optional{a} &&
            workspace.retired_boundary(b_ns) == std::optional{b},
            "branch finish preserves both retired inputs");
    undo(workspace);
    require(workspace.retired_boundary(a_ns) && !workspace.retired_boundary(b_ns),
            "undo second activation removes only its retired input");
    undo(workspace);
    require(workspace.capture().retired_boundaries().empty(), "undo first activation removes remaining input");
    redo(workspace); redo(workspace);
    require(!workspace.active_boundary() && workspace.retired_boundary(a_ns) == std::optional{a} &&
            workspace.retired_boundary(b_ns) == std::optional{b},
            "redo activations preserves both retired statuses");
    redo(workspace);
    require(workspace.snapshot().entities() == finished_b.entities() &&
            workspace.retired_boundary(a_ns) == std::optional{a} && !workspace.retired_boundary(b_ns),
            "redo branched finish restores exact geometry and preserves older retired input");
}
void check_mixed_document_navigation() {
    auto document = fixture();
    auto label = document.snapshot().entities().at("label"); label.properties["text"] = "Baseline edit";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Baseline edit"});
    const auto baseline = document.snapshot(); ProjectWorkspace workspace(baseline);
    const auto active = input(baseline, BoundaryAuthoringMode::draw_first);
    activate(workspace, active);
    auto finish = workspace.prepare_finish_boundary(); (void)workspace.commit(finish);
    const auto finished = workspace.snapshot();
    label.properties["text"] = "After finish";
    auto edit = workspace.prepare(ApplyEntityChanges{.expected_revision = finished.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "After finish"});
    (void)workspace.commit(edit); const auto edited = workspace.snapshot();
    validate_workspace_document_history(edited, workspace.document_history());
    undo(workspace);
    require(workspace.snapshot().entities() == finished.entities(), "undo edit preserves finished geometry");
    undo(workspace); undo(workspace); undo(workspace);
    require(workspace.snapshot().entities().at("label").properties.at("text") == "Original",
            "navigation crosses the imported baseline command");
    redo(workspace); redo(workspace); redo(workspace); redo(workspace);
    require(workspace.snapshot().entities() == edited.entities() && !workspace.active_boundary() &&
            workspace.capture().retired_boundaries().empty(),
            "mixed round trip preserves exact finished entities and lifecycle status");
}
void check_revise_input() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    const auto original = input(workspace.snapshot(), BoundaryAuthoringMode::draw_first);
    const auto ns = original.checkpoint.identity_namespace;
    activate(workspace, original);
    auto finish = workspace.prepare_finish_boundary(); (void)workspace.commit(finish);
    const auto old_geometry = workspace.snapshot().entities();
    undo(workspace);
    const auto before = workspace.capture();
    auto revise = workspace.prepare_revise_boundary(ns);
    require(!workspace.active_boundary() && workspace.epoch() == before.epoch(), "revise preparation is isolated");
    (void)workspace.commit(revise);
    const auto revised = *workspace.active_boundary();
    require(revised.checkpoint.identity_namespace != ns && revised.checkpoint.counters == original.checkpoint.counters &&
            revised.checkpoint.extensions.dump() == original.checkpoint.extensions.dump() &&
            revised.extensions.dump() == original.extensions.dump(), "revise regenerates identity and preserves opaque data");
    require(inspect_boundary_recovery_source(workspace.snapshot(), revised.source) == BoundaryRecoverySourceStatus::current &&
            workspace.retired_boundary(ns) == std::optional{original} && !workspace.can_redo(),
            "revise rebinds current source, preserves retired input and clears old finish redo");
    const auto captured = workspace.capture();
    require(captured.lifecycle_history().back().session->revised_from_namespace == ns &&
            captured.lifecycle_history().back().session->revised_from_finish_event_id.has_value(),
            "revision activation records origin provenance");
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_revise_boundary(ns); });
    undo(workspace); redo(workspace);
    require(workspace.active_boundary() == std::optional{revised}, "revision activation round trip retains fresh identities");
    auto second = workspace.prepare_finish_boundary(); (void)workspace.commit(second);
    const auto revised_geometry = workspace.snapshot();
    for (const auto& [id, entity] : revised_geometry.entities()) {
        if (entity.type == "boundary" || entity.type == "dimension")
            require(!old_geometry.contains(id), "revised finish must not reuse old geometry IDs");
    }
    require(workspace.retired_boundary(ns) == std::optional{original}, "revised finish retains original retired view");
}
void check_invalid_inputs_and_tickets() {
    auto document = fixture(); const auto source = document.snapshot(); ProjectWorkspace empty(source);
    unchanged_rejection(empty, [&] { (void)empty.prepare_finish_boundary(); });
    for (const auto complete : {false, true}) {
        ProjectWorkspace invalid(source);
        activate(invalid, input(source, BoundaryAuthoringMode::draw_first, complete, !complete));
        unchanged_rejection(invalid, [&] { (void)invalid.prepare_finish_boundary(); });
    }
    ProjectWorkspace workspace(source), foreign(source);
    const auto active = input(source, BoundaryAuthoringMode::draw_first); activate(workspace, active);
    auto foreign_ticket = workspace.prepare_finish_boundary();
    unchanged_rejection(foreign, [&] { (void)foreign.commit(foreign_ticket); });
    require(workspace.active_boundary() == std::optional{active}, "foreign commit leaves owner active");
    auto stale = workspace.prepare_finish_boundary(); auto pointer = active; pointer.checkpoint.pointer = Vec2{9, 8};
    activate(workspace, pointer);
    unchanged_rejection(workspace, [&] { (void)workspace.commit(stale); });
    auto label = workspace.snapshot().entities().at("label"); label.properties["text"] = "changed";
    auto edit = workspace.prepare(ApplyEntityChanges{.expected_revision = workspace.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Make source stale"});
    (void)workspace.commit(edit);
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(); });
}

void check_atomic_redefinition_finish() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    activate(workspace, input(workspace.snapshot(), BoundaryAuthoringMode::draw_first));
    auto creation = workspace.prepare_finish_boundary(); (void)workspace.commit(creation);
    const auto original = workspace.snapshot();
    const auto target = std::find_if(original.entities().begin(), original.entities().end(),
        [](const auto& entry) { return can_recognize_boundary_entity_type(entry.second.type); });
    require(target != original.entities().end(), "redraw fixture has no created boundary");
    const auto make_redraw = [&](double width) {
        BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
        session.set_classification("living_area");
        (void)session.anchor({0, 0}); (void)session.add_line_to({width, 0});
        (void)session.add_line_to({width, 2}); (void)session.add_line_to({0, 2});
        (void)session.add_closing_segment(); (void)session.close_chain();
        return BoundaryActiveRecovery{capture_boundary_recovery_source(original, {"p", "b", "f", "l"}),
            session.recovery_checkpoint(), {{"desktop_operation", {{"version", 1},
                {"kind", "redefine"}, {"target_id", target->first}}}}};
    };
    const auto active = make_redraw(6);
    const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(active.checkpoint);
    auto replacement = decode_identified_boundary_entity(target->second);
    const auto raw = session.accepted_chains().front();
    for (std::size_t i = 0; i < replacement.segments.size(); ++i)
        replacement.segments[i].segment = raw.boundary.segments[i].segment;
    BoundaryGeometryEdit edit;
    edit.boundary_id = edit.target_id = target->first;
    edit.kind = BoundaryGeometryEditKind::redefine_boundary;
    edit.replacement_segments = encode_identified_boundary_entity(replacement).properties.at("segments");
    edit.replacement_authoring = boundary_construction_envelope(raw, session.options());
    edit.replacement_properties = boundary_redefinition_classification_properties(original, target->second, "living_area");
    const EditBoundaryGeometry command{original.revision(), edit};
    require(encode_boundary_geometry_edit(edit).dump().size() > 2048,
        "redraw fixture must exercise the former small proof limit");
    activate(workspace, active); const auto before = workspace.capture();
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(); });
    auto wrong_target = command; wrong_target.edit.boundary_id = wrong_target.edit.target_id = "label";
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(wrong_target); });
    auto no_receipt = command; no_receipt.edit.replacement_authoring = nullptr;
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(no_receipt); });
    auto stale = command; ++stale.expected_revision;
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(stale); });
    for (const auto* key : {"classification", "measurement_classification", "appraisal_category", "name"}) {
        auto forged_metadata = command; forged_metadata.edit.replacement_properties[key] = "garage";
        unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(forged_metadata); });
    }
    auto foreign_receipt = command;
    const auto foreign_input = make_redraw(8);
    const auto foreign_session = BoundaryAuthoringSession::from_recovery_checkpoint(foreign_input.checkpoint);
    foreign_receipt.edit.replacement_authoring = boundary_construction_envelope(
        foreign_session.accepted_chains().front(), foreign_session.options());
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(foreign_receipt); });
    auto finish = workspace.prepare_finish_boundary(command);
    require(workspace.active_boundary() == std::optional{active} &&
        workspace.snapshot().entities() == original.entities(), "redraw preparation must remain isolated");
    (void)workspace.commit(finish); const auto finished = workspace.capture();
    require(!finished.active_boundary() && finished.document().revision() == original.revision() + 1 &&
        finished.epoch() == before.epoch() + 1 &&
        finished.edited_generation() == before.edited_generation() + 1 &&
        finished.checkpoint_generation() == before.checkpoint_generation() + 1 &&
        finished.document().entities().size() == original.entities().size(),
        "redraw must replace geometry and retire input in one publication");
    validate_workspace_finish_deltas(finished.document(), finished.lifecycle_history(), finished.resource_policy());
    auto forged = finished.lifecycle_history();
    forged.back().input->value = std::make_shared<const BoundaryActiveRecovery>(foreign_input);
    bool rejected = false;
    try { validate_workspace_finish_deltas(finished.document(), forged, finished.resource_policy()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "persisted redraw must reject valid geometry bound to unrelated archived input");
    sketch::test::DetachedDocumentSnapshotFixture forged_classification(finished.document());
    auto& forged_record = forged_classification.history().back();
    auto forged_properties = command.edit.replacement_properties;
    forged_properties["classification"] = forged_properties["measurement_classification"] = "garage";
    forged_record.boundary_geometry_edit->replacement_properties = forged_properties;
    auto& forged_owner = forged_record.entities.at(target->first);
    forged_owner.properties["classification"] = forged_owner.properties["measurement_classification"] = "garage";
    forged_owner.extensions["boundary_geometry_derivation"]["operations"].back()["value"]["replacement_properties"] = forged_properties;
    // It is a valid typed document edit; the workspace must independently bind
    // it to the archived classification, which is still living_area.
    (void)Document::fork(forged_classification);
    rejected = false;
    try { validate_workspace_finish_deltas(forged_classification, finished.lifecycle_history(), finished.resource_policy()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "persisted redraw must reject canonical geometry with a forged area classification");
    const auto ns = active.checkpoint.identity_namespace;
    undo(workspace);
    require(workspace.snapshot().entities() == original.entities() && !workspace.active_boundary() &&
        workspace.retired_boundary(ns) == std::optional{active},
        "one redraw Undo must restore the original geometry and recoverable redraw input");
    redo(workspace);
    require(workspace.snapshot().entities() == finished.document().entities() &&
        !workspace.active_boundary() && !workspace.retired_boundary(ns),
        "one redraw Redo must restore exact replacement identities and retire input again");
}

void check_appraisal_redefinition_classification() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    activate(workspace, input(workspace.snapshot(), BoundaryAuthoringMode::draw_first));
    auto creation = workspace.prepare_finish_boundary(); (void)workspace.commit(creation);
    auto source = workspace.snapshot();
    auto property = source.entities().at("p"); property.properties["calculation_workflow"] = "appraisal";
    const auto found = std::find_if(source.entities().begin(), source.entities().end(),
        [](const auto& entry) { return can_recognize_boundary_entity_type(entry.second.type); });
    auto target = found->second;
    target.properties["classification"] = target.properties["measurement_classification"] = "measurement";
    target.properties["appraisal_category"] = "above_grade_finished";
    auto normalized = workspace.prepare(ApplyEntityChanges{source.revision(),
        {EntityChange::upsert(property), EntityChange::upsert(target)}, {}, "Appraisal fixture"});
    (void)workspace.commit(normalized); source = workspace.snapshot();
    const auto changes = boundary_redefinition_classification_properties(source, target, "garage");
    require(changes == Json{{"classification", "measurement"}, {"measurement_classification", "measurement"},
        {"appraisal_category", "garage"}}, "redraw must preserve measurement labels independently of appraisal categories");
    bool rejected = false;
    try { (void)boundary_redefinition_classification_properties(source, target, "living_area"); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "appraisal redraw must reject an undefined area category");
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try { check_finish_round_trip(); check_multiple_retired_inputs(); check_mixed_document_navigation(); check_revise_input(); check_invalid_inputs_and_tickets(); check_atomic_redefinition_finish(); check_appraisal_redefinition_classification(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
