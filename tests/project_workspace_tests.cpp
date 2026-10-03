#include "sketch/project_workspace.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/project_store.hpp"
#include "sketch/workspace_lifecycle_validation.hpp"
#include "support/noninteractive_errors.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace sketch;
static_assert(!std::is_copy_constructible_v<ProjectWorkspace>);
static_assert(!std::is_move_constructible_v<ProjectWorkspace>);
static_assert(!std::is_copy_constructible_v<PreparedWorkspaceEdit>);
static_assert(std::is_move_constructible_v<PreparedWorkspaceEdit>);
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejected(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("stale or foreign workspace ticket accepted");
}
Command edit(const DocumentSnapshot& snapshot, const char* text) {
    auto label = snapshot.entities().at("label");
    label.properties["text"] = text;
    return ApplyEntityChanges{.expected_revision = snapshot.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Edit label"};
}
void check_publication() {
    auto document = Document::create({{"label", "label", {{"text", "Original"}}}});
    const auto initial = document.snapshot();
    ProjectWorkspace workspace(initial), foreign(initial);
    require(!workspace.active_boundary(), "a document-only workspace has no active boundary");
    require(workspace.edited_generation() == 0 && workspace.checkpoint_generation() == 0,
            "new workspace generations must start at zero");
    require(workspace.identity() != foreign.identity(), "instances need independent identities");
    auto command = edit(initial, "First");
    auto first = workspace.prepare(command);
    auto stale = workspace.prepare(edit(initial, "Second"));
    require(workspace.epoch() == 0 && document_snapshot_digest(workspace.snapshot()) ==
                document_snapshot_digest(initial), "prepare must leave workspace unchanged");
    std::get<ApplyEntityChanges>(command).entity_changes.front().entity.properties["text"] = "Tampered command";
    auto preview = first.preview();
    const_cast<std::map<std::string, Entity, std::less<>>&>(preview.entities())
        .at("label").properties["text"] = "Tampered preview";
    rejected([&] { (void)foreign.commit(first); });
    require(foreign.epoch() == 0, "foreign rejection must not mutate receiver");
    const auto committed = workspace.commit(first);
    require(workspace.epoch() == 1 && workspace.snapshot().revision() == committed &&
                workspace.snapshot().entities().at("label").properties.at("text") == "First",
            "publication must use sealed candidate exactly once");
    require(workspace.document_history().events.size() == 1,
            "successful publication must include exactly one history event");
    require(workspace.edited_generation() == 1 && workspace.checkpoint_generation() == 1,
            "document publication must advance both content generations");
    validate_workspace_document_history(workspace.snapshot(), workspace.document_history());
    const auto digest = document_snapshot_digest(workspace.snapshot());
    rejected([&] { (void)workspace.commit(first); });
    rejected([&] { (void)first.preview(); });
    rejected([&] { (void)workspace.commit(stale); });
    require(workspace.epoch() == 1 && document_snapshot_digest(workspace.snapshot()) == digest,
            "rejected commits must preserve complete current state");
    require(workspace.document_history().events.size() == 1 && foreign.document_history().events.empty(),
            "rejected tickets cannot append events to either workspace");
    require(workspace.edited_generation() == 1 && workspace.checkpoint_generation() == 1 &&
                foreign.edited_generation() == 0 && foreign.checkpoint_generation() == 0,
            "rejected tickets must preserve generations");
    auto undo = workspace.prepare_undo();
    (void)workspace.commit(undo);
    require(workspace.epoch() == 2 && workspace.snapshot().entities() == initial.entities(),
            "workspace undo must retain Document navigation semantics");
    auto redo = workspace.prepare_redo();
    auto moved = std::move(redo);
    rejected([&] { (void)workspace.commit(redo); });
    (void)workspace.commit(moved);
    require(workspace.epoch() == 3 &&
                workspace.snapshot().entities().at("label").properties.at("text") == "First",
            "moved redo ticket must publish original retained IDs and values");
    const auto history = workspace.document_history();
    require(history.events.size() == 3 && history.events[1].kind == WorkspaceDocumentEventKind::undo &&
                history.events[2].kind == WorkspaceDocumentEventKind::redo,
            "document navigation must publish ordered typed events");
    validate_workspace_document_history(workspace.snapshot(), history);
    require(workspace.edited_generation() == 3 && workspace.checkpoint_generation() == 3,
            "undo and redo each advance generations rather than rewinding them");
    auto detached_history = history;
    detached_history.events.clear();
    require(workspace.document_history() == history, "history snapshots must be detached");
    require(document_snapshot_digest(document.snapshot()) == document_snapshot_digest(initial),
            "workspace must not retain mutable access to source Document");
}
void check_rejections() {
    auto locked = Document::create({Entity::create("future_required", nlohmann::json::object(), true)});
    ProjectWorkspace workspace(locked.snapshot());
    bool read_only = false;
    try { (void)workspace.prepare(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Forbidden"}}))}}); }
    catch (const DocumentError& error) { read_only = error.code() == DocumentErrorCode::read_only; }
    require(read_only && workspace.epoch() == 0 && !workspace.snapshot().is_editable(),
            "workspace cannot bypass read-only state");
    auto malformed = Document::create().snapshot();
    const_cast<std::vector<RevisionRecord>&>(malformed.history()).front().undo_stack.push_back(999);
    bool invalid = false;
    try { ProjectWorkspace bad(malformed); } catch (const DocumentError&) { invalid = true; }
    require(invalid, "workspace construction must validate retained history");
    ProjectWorkspace empty(Document::create().snapshot());
    rejected([&] { (void)empty.prepare_finish_boundary(); });
    bool no_undo = false;
    try { (void)empty.prepare_undo(); }
    catch (const DocumentError& error) { no_undo = error.code() == DocumentErrorCode::no_undo; }
    require(no_undo && empty.epoch() == 0 && empty.snapshot().revision() == 0,
            "failed navigation preparation must leave authoritative state unchanged");
}
void check_checkpoint_policy() {
    auto document = Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
    auto policy = boundary_authoring_default_resource_policy;
    policy.max_actions = 0;
    ProjectWorkspace workspace(document.snapshot(), policy);
    require(workspace.capture().resource_policy() == policy,
            "worker capture must retain the workspace's actual resource policy");
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0, 0});
    BoundaryActiveRecovery checkpoint{
        capture_boundary_recovery_source(document.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(checkpoint); });
    require(!workspace.active_boundary() && workspace.epoch() == 0 &&
                workspace.edited_generation() == 0 && workspace.checkpoint_generation() == 0,
            "workspace must enforce its caller-supplied checkpoint policy before publication");
}

struct RedrawPlanFixture {
    DocumentSnapshot source;
    BoundaryActiveRecovery active;
    EditBoundaryGeometry command;
    std::string manual_id;
};
RedrawPlanFixture redraw_plan_fixture(bool remove_reference = false, bool with_reference = true,
    bool automatic_angle = false) {
    auto document = Document::create({
        {"p", "property", {{"name", "Property"}}}, {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}}, {"l", "layer", {{"floor_id", "f"}}},
        {"unrelated", "label", {{"text", "Preserve"}}}});
    ProjectWorkspace original(document.snapshot());
    BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
    BoundaryAuthoringSession rectangle(BoundaryAuthoringMode::draw_first, options);
    rectangle.set_classification("living_area");
    (void)rectangle.anchor({0, 0}); (void)rectangle.add_line_to({4, 0});
    (void)rectangle.add_line_to({4, 3}); (void)rectangle.add_line_to({0, 3});
    (void)rectangle.add_closing_segment(); (void)rectangle.close_chain();
    BoundaryActiveRecovery original_input{
        capture_boundary_recovery_source(original.snapshot(), {"p", "b", "f", "l"}),
        rectangle.recovery_checkpoint()};
    auto activate = original.prepare_boundary_checkpoint(original_input); (void)original.commit(activate);
    auto finish = original.prepare_finish_boundary(); (void)original.commit(finish);
    auto source = original.snapshot();
    const auto found = std::find_if(source.entities().begin(), source.entities().end(),
        [](const auto& item) { return can_recognize_boundary_entity_type(item.second.type); });
    require(found != source.entities().end(), "reference plan fixture needs a completed boundary");
    const auto owner = found->second;
    const auto identified = decode_identified_boundary_entity(owner);
    const std::string manual_id = with_reference ? "workspace-manual-dimension" : "";
    if (with_reference) {
        BoundaryDimension manual{manual_id, owner.id, identified.segments.front().segment_id, {2, -1}};
        if (automatic_angle) {
            manual.kind = BoundaryDimensionKind::angle;
            manual.placement = BoundaryDimensionPlacement::automatic;
            manual.automatic_placement_version = 2;
            manual.secondary_segment_id = identified.segments[1].segment_id;
            manual.vertex_id = identified.segments.front().end_vertex_id;
        }
        auto entity = encode_boundary_dimension_entity(manual);
        entity.extensions["opaque_manual"] = {{"number", 1.0}};
        auto addition = original.prepare(ApplyEntityChanges{.expected_revision = source.revision(),
            .entity_changes = {EntityChange::upsert(entity)}, .message = "Reference plan fixture"});
        (void)original.commit(addition); source = original.snapshot();
    }
    BoundaryAuthoringSession triangle(BoundaryAuthoringMode::draw_first, options);
    triangle.set_classification("living_area");
    (void)triangle.anchor({0, 0}); (void)triangle.add_line_to({4, 0});
    (void)triangle.add_line_to({0, 3}); (void)triangle.add_closing_segment(); (void)triangle.close_chain();
    const auto chain = triangle.accepted_chains().front();
    IdentifiedBoundary replacement{owner.id, owner.type, {
        {"workspace-plan-e0", "workspace-plan-v0", "workspace-plan-v1", {{0, 0}, {4, 0}, 0}},
        {"workspace-plan-e1", "workspace-plan-v1", "workspace-plan-v2", {{4, 0}, {0, 3}, 0}},
        {"workspace-plan-e2", "workspace-plan-v2", "workspace-plan-v0", {{0, 3}, {0, 0}, 0}}}};
    BoundaryGeometryEdit edit;
    edit.kind = BoundaryGeometryEditKind::redefine_boundary;
    edit.boundary_id = edit.target_id = owner.id;
    edit.replacement_segments = encode_identified_boundary_entity(replacement).properties.at("segments");
    edit.replacement_authoring = boundary_construction_envelope(chain, options);
    edit.replacement_properties = boundary_redefinition_classification_properties(source, owner, "living_area");
    edit.replacement_dimension_ids = {"workspace-plan-d0", "workspace-plan-d1", "workspace-plan-d2"};
    if (with_reference) {
        if (remove_reference) edit.replacement_removed_reference_ids = {manual_id};
        else edit.replacement_child_mapping = {{"segments", {{identified.segments.front().segment_id, "workspace-plan-e0"}}},
            {"vertices", nlohmann::json::object()}};
    }
    edit.allow_automatic_angle_removal = automatic_angle && remove_reference;
    nlohmann::json operation{{"version", edit.allow_automatic_angle_removal ? 3 : with_reference ? 2 : 1},
        {"kind", "redefine"}, {"target_id", owner.id}};
    if (with_reference) {
        operation["replacement_child_mapping"] = edit.replacement_child_mapping;
        operation["replacement_removed_reference_ids"] = edit.replacement_removed_reference_ids;
        const auto geometry_json = edit.replacement_segments.dump();
        operation["replacement_segments_sha256"] = sha256_hex(std::as_bytes(std::span(geometry_json.data(), geometry_json.size())));
    }
    if (edit.allow_automatic_angle_removal) operation["allow_automatic_angle_removal"] = true;
    BoundaryActiveRecovery active{capture_boundary_recovery_source(source, {"p", "b", "f", "l"}),
        triangle.recovery_checkpoint(), {{"desktop_operation", operation}, {"opaque_input", {{"number", 1.0}}}}};
    return {source, active, EditBoundaryGeometry{source.revision(), edit}, manual_id};
}
template<class F> void unchanged_finish_rejection(ProjectWorkspace& workspace, F operation) {
    const auto before = workspace.capture();
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid archived reference plan must reject");
    const auto after = workspace.capture();
    require(document_snapshot_digest(after.document()) == document_snapshot_digest(before.document()) &&
        after.active_boundary() == before.active_boundary() && after.navigation() == before.navigation() &&
        after.document_history() == before.document_history() && after.epoch() == before.epoch() &&
        after.edited_generation() == before.edited_generation() &&
        after.checkpoint_generation() == before.checkpoint_generation() &&
        after.lifecycle_history().size() == before.lifecycle_history().size() &&
        after.retired_boundaries().size() == before.retired_boundaries().size(),
        "rejected reference plan must preserve the whole authoritative workspace");
}
void activate_redraw(ProjectWorkspace& workspace, const BoundaryActiveRecovery& active) {
    auto ticket = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(ticket);
}
void check_archived_redraw_reference_plan() {
    for (const bool remove_reference : {false, true}) {
        const auto fixture = redraw_plan_fixture(remove_reference);
        ProjectWorkspace workspace(fixture.source);
        activate_redraw(workspace, fixture.active);
        const auto before = workspace.capture();
        const auto canonical = Document::preview_command(fixture.source, fixture.command);
        auto finish = workspace.prepare_finish_boundary(fixture.command);
        require(workspace.epoch() == before.epoch() && workspace.active_boundary() == std::optional{fixture.active},
            "accepted plan preparation must remain isolated");
        (void)workspace.commit(finish);
        const auto finished = workspace.capture();
        require(finished.document().entities() == canonical.entities() && !finished.active_boundary(),
            "accepted reference plan must publish the canonical replacement exactly");
        require(finished.lifecycle_history().back().input->value->extensions.at("desktop_operation").dump() ==
            fixture.active.extensions.at("desktop_operation").dump(),
            "finish must archive the exact accepted mapping/removal envelope");
        if (remove_reference) require(!finished.document().entities().contains(fixture.manual_id),
            "accepted removal must retire the eligible manual dimension");
        else {
            auto expected = fixture.source.entities().at(fixture.manual_id);
            expected.properties["target"]["segment_id"] = "workspace-plan-e0";
            require(finished.document().entities().at(fixture.manual_id).properties.dump() == expected.properties.dump() &&
                finished.document().entities().at(fixture.manual_id).extensions.dump() == expected.extensions.dump(),
                "accepted mapping must preserve manual dimension identity and opaque metadata");
        }
        validate_workspace_finish_deltas(finished.document(), finished.lifecycle_history());
        auto forged = finished.lifecycle_history();
        auto forged_input = fixture.active;
        auto& forged_operation = forged_input.extensions["desktop_operation"];
        forged_operation["replacement_removed_reference_ids"] = nlohmann::json::array({"unrelated"});
        forged.back().input->value = std::make_shared<const BoundaryActiveRecovery>(forged_input);
        rejected([&] { validate_workspace_finish_deltas(finished.document(), forged); });
        forged = finished.lifecycle_history();
        forged_input = fixture.active;
        forged_input.extensions["desktop_operation"]["replacement_segments_sha256"] = std::string(64, '0');
        forged.back().input->value = std::make_shared<const BoundaryActiveRecovery>(forged_input);
        rejected([&] { validate_workspace_finish_deltas(finished.document(), forged); });
        require(document_snapshot_digest(workspace.snapshot()) == document_snapshot_digest(finished.document()),
            "forged persisted finish validation must not mutate the workspace");
        const auto path = std::filesystem::temp_directory_path() / ("workspace-reference-plan-" + make_stable_id() + ".sketch");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(
            finished.document(), capture_workspace_history_record(finished), finished.active_boundary())}};
        (void)ProjectStore::save_archive(path, ProjectArchiveSnapshot(finished.document(), ledger, ArchiveRole::ordinary));
        const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
        require(loaded.supported(), "accepted reference plan archive must reopen supported");
        auto restored = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
        require(restored->snapshot().entities() == finished.document().entities(), "reopen must retain accepted plan result");
        auto undo = restored->prepare_undo(); (void)restored->commit(undo);
        require(restored->snapshot().entities() == fixture.source.entities() &&
            restored->retired_boundary(fixture.active.checkpoint.identity_namespace) == std::optional{fixture.active},
            "undo after reopen must restore references and the exact accepted input plan");
        auto redo = restored->prepare_redo(); (void)restored->commit(redo);
        require(restored->snapshot().entities() == finished.document().entities(),
            "redo after reopen must restore exact mapped/removed references");
    }
}
void check_archived_automatic_angle_removal() {
    const auto fixture = redraw_plan_fixture(true, true, true);
    require(fixture.command.edit.allow_automatic_angle_removal &&
        fixture.active.extensions.at("desktop_operation").at("version") == 3,
        "automatic angle decision must use the explicit archived policy");
    ProjectWorkspace workspace(fixture.source);
    activate_redraw(workspace, fixture.active);
    const auto canonical = Document::preview_command(fixture.source, fixture.command);
    auto finish = workspace.prepare_finish_boundary(fixture.command);
    (void)workspace.commit(finish);
    const auto finished = workspace.capture();
    require(finished.document().entities() == canonical.entities() &&
        !finished.document().entities().contains(fixture.manual_id),
        "version-three finish must remove only the reviewed automatic angle");
    validate_workspace_finish_deltas(finished.document(), finished.lifecycle_history());
    for (const std::string fault : {"missing", "false", "old_version", "changed_removals"}) {
        auto forged = finished.lifecycle_history();
        auto input = fixture.active;
        auto& operation = input.extensions["desktop_operation"];
        if (fault == "missing") operation.erase("allow_automatic_angle_removal");
        else if (fault == "false") operation["allow_automatic_angle_removal"] = false;
        else if (fault == "old_version") operation["version"] = 2;
        else operation["replacement_removed_reference_ids"] = nlohmann::json::array({"unrelated"});
        forged.back().input->value = std::make_shared<const BoundaryActiveRecovery>(input);
        rejected([&] { validate_workspace_finish_deltas(finished.document(), forged); });
        ProjectWorkspace live(fixture.source);
        activate_redraw(live, input);
        unchanged_finish_rejection(live, [&] { (void)live.prepare_finish_boundary(fixture.command); });
    }
    auto downgraded_command = fixture.command;
    downgraded_command.edit.allow_automatic_angle_removal = false;
    ProjectWorkspace live(fixture.source);
    activate_redraw(live, fixture.active);
    unchanged_finish_rejection(live, [&] { (void)live.prepare_finish_boundary(downgraded_command); });
    const auto path = std::filesystem::temp_directory_path() / ("workspace-angle-removal-" + make_stable_id() + ".bldproj");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(
        finished.document(), capture_workspace_history_record(finished), finished.active_boundary())}};
    (void)ProjectStore::save_archive(path, ProjectArchiveSnapshot(finished.document(), ledger, ArchiveRole::ordinary));
    const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.supported(), "automatic-angle finish archive must remain supported");
    auto restored = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
    require(restored->snapshot().entities() == finished.document().entities(), "archive must preserve exact removal result");
    auto undo = restored->prepare_undo(); (void)restored->commit(undo);
    require(restored->snapshot().entities() == fixture.source.entities() &&
        restored->retired_boundary(fixture.active.checkpoint.identity_namespace) == std::optional{fixture.active},
        "undo after reopen must restore the angle and exact accepted decision");
    auto redo = restored->prepare_redo(); (void)restored->commit(redo);
    require(restored->snapshot().entities() == finished.document().entities(), "redo must reapply the same removal");
}

void check_reviewed_geometry_identity_binding() {
    auto fixture = redraw_plan_fixture(false);
    auto substituted = fixture.command;
    std::swap(substituted.edit.replacement_segments[0]["segment_id"], substituted.edit.replacement_segments[1]["segment_id"]);
    (void)Document::preview_command(fixture.source, substituted);
    ProjectWorkspace workspace(fixture.source);
    auto activation = workspace.prepare_boundary_checkpoint(fixture.active); (void)workspace.commit(activation);
    unchanged_finish_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(substituted); });
}

void check_rejected_archived_redraw_plans() {
    for (const std::string fault : {"omission", "mapping", "removal", "extra", "malformed_mapping", "malformed_removal", "numeric_version", "empty_v2"}) {
        auto fixture = redraw_plan_fixture(false, fault != "empty_v2");
        auto& operation = fixture.active.extensions["desktop_operation"];
        if (fault == "omission") operation = {{"version", 1}, {"kind", "redefine"}, {"target_id", fixture.command.edit.boundary_id}};
        else if (fault == "mapping") operation["replacement_child_mapping"].begin().value() = "workspace-plan-e1";
        else if (fault == "removal") operation["replacement_removed_reference_ids"] = nlohmann::json::array({fixture.manual_id});
        else if (fault == "extra") operation["unreviewed_plan"] = true;
        else if (fault == "malformed_mapping") operation["replacement_child_mapping"] = nlohmann::json::array();
        else if (fault == "malformed_removal") operation["replacement_removed_reference_ids"] = fixture.manual_id;
        else if (fault == "numeric_version") operation["version"] = 2.0;
        else {
            operation["version"] = 2;
            operation["replacement_child_mapping"] = nlohmann::json::object();
            operation["replacement_removed_reference_ids"] = nlohmann::json::array();
        }
        ProjectWorkspace workspace(fixture.source); activate_redraw(workspace, fixture.active);
        unchanged_finish_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(fixture.command); });
    }
    auto fixture = redraw_plan_fixture();
    ProjectWorkspace workspace(fixture.source); activate_redraw(workspace, fixture.active);
    auto forged_command = fixture.command;
    forged_command.edit.replacement_removed_reference_ids = {fixture.manual_id};
    unchanged_finish_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(forged_command); });
    // A matching envelope cannot authorize deletion of an ineligible entity.
    auto ineligible = fixture;
    ineligible.command.edit.replacement_removed_reference_ids = {"unrelated"};
    ineligible.active.extensions["desktop_operation"]["replacement_removed_reference_ids"] = nlohmann::json::array({"unrelated"});
    ProjectWorkspace invalid(ineligible.source); activate_redraw(invalid, ineligible.active);
    unchanged_finish_rejection(invalid, [&] { (void)invalid.prepare_finish_boundary(ineligible.command); });
    const auto legacy = redraw_plan_fixture(false, false);
    ProjectWorkspace old(legacy.source); activate_redraw(old, legacy.active);
    auto finish = old.prepare_finish_boundary(legacy.command); (void)old.commit(finish);
    require(old.capture().lifecycle_history().back().input->value->extensions.at("desktop_operation").at("version") == 1,
        "empty reference plans must retain the legacy version one archive contract");
    validate_workspace_finish_deltas(old.snapshot(), old.capture().lifecycle_history());
}
void check_legacy_classification_only_finished_archive() {
    auto original = Document::create({{"p", "property", {{"name", "Legacy appraisal"}, {"calculation_workflow", "appraisal"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
    BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    session.set_classification("garage"); (void)session.anchor({0, 0});
    (void)session.add_line_to({4, 0}); (void)session.add_line_to({4, 3});
    (void)session.add_line_to({0, 3}); (void)session.add_closing_segment();
    const auto chain = session.close_chain();
    BoundaryActiveRecovery active{capture_boundary_recovery_source(original.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    require(encode_boundary_active_recovery(active).at("version") == 1,
        "legacy fixture must retain the ordinary active-v1 discriminator");

    // Assemble the historic committed payload independently of the commit
    // encoder. It predates classification normalization even in Appraisal.
    const auto add_context = [](Entity& entity) {
        entity.properties["property_id"] = "p"; entity.properties["building_id"] = "b";
        entity.properties["floor_id"] = "f"; entity.properties["layer_id"] = "l";
    };
    auto legacy_boundary = encode_identified_boundary_entity(chain.boundary);
    add_context(legacy_boundary);
    legacy_boundary.properties["classification"] = "garage";
    legacy_boundary.properties["factor"] = 1.0;
    legacy_boundary.properties["factor_expression"] = "1";
    legacy_boundary.properties["factor_numerator"] = 1;
    legacy_boundary.properties["factor_denominator"] = 1;
    legacy_boundary.properties["boundary_authoring"] = boundary_construction_envelope(chain, options);
    std::vector<EntityChange> legacy_changes{EntityChange::upsert(legacy_boundary)};
    for (const auto& dimension : chain.dimensions) {
        auto entity = encode_boundary_dimension_entity(dimension); add_context(entity);
        legacy_changes.push_back(EntityChange::upsert(entity));
    }
    auto legacy = Document::fork(original.snapshot());
    legacy.apply(ApplyEntityChanges{legacy.revision(), legacy_changes, {}, "Finish boundary"});
    const auto legacy_head = legacy.snapshot();
    require(!legacy_head.entities().at(chain.boundary.id).properties.contains("measurement_classification") &&
            !legacy_head.entities().at(chain.boundary.id).properties.contains("appraisal_category"),
        "legacy fixture must encode classification-only entity state, not the new normalizer result");

    // Lifecycle metadata names the original v1 input; the archived entity state
    // is supplied by the independent legacy command above.
    ProjectWorkspace layout(original.snapshot());
    auto ticket = layout.prepare_boundary_checkpoint(active); (void)layout.commit(ticket);
    ticket = layout.prepare_finish_boundary(); (void)layout.commit(ticket);
    const auto captured = layout.capture();
    const auto wire = encode_workspace_history_record(captured.document(),
        capture_workspace_history_record(captured), std::nullopt);
    require(decode_workspace_history_record(legacy_head, wire, std::nullopt).supported(),
        "classification-only legacy finished history must replay against its original v1 input");
    const auto path = std::filesystem::temp_directory_path() / ("workspace-legacy-finish-" + make_stable_id() + ".bldproj");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    RecoveryLedger ledger{{"history", "workspace_history", wire}};
    (void)ProjectStore::save_archive(path, {legacy_head, ledger, ArchiveRole::ordinary});
    const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.supported(), "legacy v1 finished archive must load as supported");
    auto restored = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
    require(restored->snapshot().entities() == legacy_head.entities(), "legacy archive reopen must preserve exact old payloads");
    ticket = restored->prepare_undo(); (void)restored->commit(ticket);
    require(restored->snapshot().entities() == original.snapshot().entities(), "legacy finish undo must restore its exact source");
    ticket = restored->prepare_redo(); (void)restored->commit(ticket);
    require(restored->snapshot().entities() == legacy_head.entities(), "legacy finish redo must retain classification-only payloads");
}

void check_subtraction_recovery_finish_and_revise() {
    auto document = Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}},
        {"parent", "measurement_boundary", {{"floor_id", "f"}, {"layer_id", "l"}, {"classification", "garage"},
            {"segments", nlohmann::json::array({{{"start", {-1, -1}}, {"end", {6, -1}}, {"sweep_radians", 0}},
                {{"start", {6, -1}}, {"end", {6, 5}}, {"sweep_radians", 0}}, {{"start", {6, 5}}, {"end", {-1, 5}}, {"sweep_radians", 0}},
                {{"start", {-1, 5}}, {"end", {-1, -1}}, {"sweep_radians", 0}}})}}}});
    ProjectWorkspace workspace(document.snapshot());
    BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    session.set_classification("living_area"); (void)session.anchor({0, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint(), nlohmann::json::object(), "parent"};
    auto ticket = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(ticket);
    const auto path = std::filesystem::temp_directory_path() / ("workspace-subtraction-" + make_stable_id() + ".bldproj");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    const auto save = [&](const ProjectWorkspaceSnapshot& snapshot) {
        RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(snapshot.document(),
            capture_workspace_history_record(snapshot), snapshot.active_boundary())}};
        if (snapshot.active_boundary()) ledger.push_back({"active", "boundary_active", encode_boundary_active_recovery(*snapshot.active_boundary())});
        SaveOptions save_options;
        if (std::filesystem::exists(path)) save_options.expected_destination_sha256 = ProjectStore::file_sha256(path);
        (void)ProjectStore::save_archive(path, {snapshot.document(), ledger, ArchiveRole::ordinary}, save_options);
    };
    save(workspace.capture());
    auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.supported(), "unfinished subtraction workspace must save and reopen supported");
    auto restored = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
    require(restored->active_boundary()->auto_subtract_target_id == "parent", "saved unfinished target must remain exact");
    (void)session.add_line_to({4, 0}); (void)session.add_line_to({4, 3});
    (void)session.add_line_to({0, 3}); (void)session.add_closing_segment(); (void)session.close_chain();
    active.checkpoint = session.recovery_checkpoint();
    ticket = restored->prepare_boundary_checkpoint(active); (void)restored->commit(ticket);
    ticket = restored->prepare_finish_boundary(); (void)restored->commit(ticket);
    const auto finished = restored->capture();
    require(finished.document().entities().at("parent").properties.at("deduction_ids") ==
        nlohmann::json::array({session.accepted_chains().front().boundary.id}), "finish must publish its retained target link");
    validate_workspace_finish_deltas(finished.document(), finished.lifecycle_history());
    auto forged = finished.lifecycle_history(); auto bad_input = *forged.back().input->value;
    bad_input.auto_subtract_target_id = "missing-parent";
    forged.back().input->value = std::make_shared<const BoundaryActiveRecovery>(bad_input);
    rejected([&] { validate_workspace_finish_deltas(finished.document(), forged); });
    ticket = restored->prepare_undo(); (void)restored->commit(ticket);
    require(restored->snapshot().entities() == document.snapshot().entities(), "recovered subtraction undo must restore source and parent");
    ticket = restored->prepare_redo(); (void)restored->commit(ticket);
    require(restored->snapshot().entities() == finished.document().entities(), "recovered subtraction redo must restore exact target and source");
    save(restored->capture());
    for (const auto fault : {"deleted", "same-type", "foreign-floor"}) {
        loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
        auto changed = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
        std::vector<EntityChange> changes;
        if (std::string_view(fault) == "deleted") changes.push_back(EntityChange::erase("parent"));
        else {
            auto parent = changed->snapshot().entities().at("parent");
            if (std::string_view(fault) == "same-type") parent.properties["classification"] = "living_area";
            else {
                changes.push_back(EntityChange::upsert({"f2", "floor", {{"building_id", "b"}}}));
                changes.push_back(EntityChange::upsert({"l2", "layer", {{"floor_id", "f2"}}}));
                parent.properties["floor_id"] = "f2"; parent.properties["layer_id"] = "l2";
            }
            changes.push_back(EntityChange::upsert(parent));
        }
        ticket = changed->prepare(ApplyEntityChanges{changed->snapshot().revision(), changes}); (void)changed->commit(ticket);
        const auto before = document_snapshot_digest(changed->snapshot());
        rejected([&] { (void)changed->prepare_revise_boundary(session.recovery_checkpoint().identity_namespace); });
        require(document_snapshot_digest(changed->snapshot()) == before, "failed target revalidation must not mutate workspace");
    }
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_legacy_classification_only_finished_archive(); check_subtraction_recovery_finish_and_revise();
          check_publication(); check_rejections(); check_checkpoint_policy();
          check_archived_redraw_reference_plan(); check_archived_automatic_angle_removal();
          check_reviewed_geometry_identity_binding(); check_rejected_archived_redraw_plans(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
