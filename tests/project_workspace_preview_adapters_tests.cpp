#include "sketch/project_workspace.hpp"
#include "sketch/boundary_commit.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <filesystem>
#include <stdexcept>

using namespace sketch;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects_unchanged(ProjectWorkspace& workspace, F action) {
    const auto before = workspace.capture();
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid preview was accepted");
    const auto after = workspace.capture();
    require(document_snapshot_digest(before.document()) == document_snapshot_digest(after.document()) &&
        before.epoch() == after.epoch() && before.edited_generation() == after.edited_generation() &&
        before.checkpoint_generation() == after.checkpoint_generation() &&
        before.lifecycle_history().size() == after.lifecycle_history().size() &&
        before.document_history().events.size() == after.document_history().events.size(),
        "rejection changed workspace");
}
Document fixture() {
    return Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}, {"name", "Building"}}},
        {"f", "floor", {{"building_id", "b"}, {"name", "Floor"}}},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Layer"}}},
        {"wall", "wall", {{"baseline", {{"start", {0., 0.}}, {"end", {4., 0.}},
            {"sweep_radians", 0.}}}, {"thickness_m", 0.14}, {"height_m", 2.4},
            {"elevation_m", 0.}, {"classification", "existing"}}}
    });
}
BoundaryCommitIntent boundary_intent() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    session.set_classification("living_area");
    (void)session.anchor({0., 0.});
    (void)session.add_line_rise_run(parse_quantity("0 m"), parse_quantity("4 m"));
    (void)session.add_line_rise_run(parse_quantity("3 m"), parse_quantity("0 m"));
    (void)session.add_line_rise_run(parse_quantity("0 m"), parse_quantity("-4 m"));
    (void)session.add_closing_segment();
    return {options, {session.close_chain()}, {"p", "b", "f", "l"}, "Boundary adapter"};
}
template<class Preview, class Prepare, class Apply>
void exercise(ProjectWorkspace& workspace, const Preview& preview, Prepare prepare, Apply apply) {
    require(preview.accepted(), "fixture preview rejected");
    const auto source = workspace.snapshot();
    auto direct = Document::fork(source);
    apply(direct, preview);
    auto edit = prepare(workspace, preview);
    require(document_snapshot_digest(edit.preview()) == document_snapshot_digest(direct.snapshot()),
            "workspace candidate differs from authoritative apply");
    require(document_snapshot_digest(workspace.snapshot()) == document_snapshot_digest(source),
            "preparation changed live document");
    ProjectWorkspace other(source);
    rejects_unchanged(other, [&] { other.commit(edit); });
    workspace.commit(edit);
    require(workspace.edited_generation() == 1 && workspace.document_history().events.size() == 1 &&
        workspace.capture().lifecycle_history().back().kind == WorkspaceLifecycleKind::document_edit,
        "adapter bypassed workspace history");
    rejects_unchanged(workspace, [&] { (void)prepare(workspace, preview); });
    auto undo = workspace.prepare_undo();
    workspace.commit(undo);
    require(workspace.snapshot().entities() == source.entities(), "adapter undo failed");
    auto redo = workspace.prepare_redo();
    workspace.commit(redo);
    require(workspace.snapshot().entities() == direct.snapshot().entities(), "adapter redo failed");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        auto document = fixture();
        ProjectWorkspace constraints(document.snapshot());
        ConstraintAuthoringIntent resize;
        resize.wall_resize = WallResizeIntent{"wall", parse_quantity("5 m")};
        resize.message = "Resize adapter";
        const auto constraint = preview_constraint_authoring(constraints.snapshot(), resize);
        const auto prepare_constraint = [](ProjectWorkspace& w, const auto& p) {
            return w.prepare_constraint_authoring(p);
        };
        const auto rejected = preview_constraint_authoring(constraints.snapshot(), {});
        rejects_unchanged(constraints, [&] { (void)constraints.prepare_constraint_authoring(rejected); });
        auto foreign = fixture();
        const auto foreign_constraint = preview_constraint_authoring(foreign.snapshot(), resize);
        rejects_unchanged(constraints, [&] { (void)constraints.prepare_constraint_authoring(foreign_constraint); });
        exercise(constraints, constraint, prepare_constraint, apply_constraint_authoring);

        // Removal exercises erase changes rather than only geometry upserts.
        auto related = Document::fork(document.snapshot());
        PersistentConstraint relation;
        relation.id = "horizontal";
        relation.relation = ConstraintRelationKind::horizontal;
        relation.bindings = {{"wall", WallEndpointRole::start}, {"wall", WallEndpointRole::end}};
        ConstraintAuthoringIntent add_relation;
        add_relation.relation_mutations = {ConstraintRelationMutation::upsert(relation)};
        apply_constraint_authoring(related, preview_constraint_authoring(related.snapshot(), add_relation));
        ProjectWorkspace removals(related.snapshot());
        ConstraintAuthoringIntent remove_relation;
        remove_relation.relation_mutations = {ConstraintRelationMutation::remove("horizontal")};
        exercise(removals, preview_constraint_authoring(removals.snapshot(), remove_relation),
                 prepare_constraint, apply_constraint_authoring);

        ProjectWorkspace boundaries(document.snapshot());
        const auto intent = boundary_intent();
        const auto boundary = preview_boundary_commit(boundaries.snapshot(), intent);
        const auto bad_boundary = preview_boundary_commit(boundaries.snapshot(), {});
        rejects_unchanged(boundaries, [&] { (void)boundaries.prepare_boundary_commit(bad_boundary); });
        const auto foreign_boundary = preview_boundary_commit(foreign.snapshot(), intent);
        rejects_unchanged(boundaries, [&] { (void)boundaries.prepare_boundary_commit(foreign_boundary); });
        exercise(boundaries, boundary, [](ProjectWorkspace& w, const auto& p) {
            return w.prepare_boundary_commit(p);
        }, apply_boundary_commit);

        // The adapter must retain typed geometry evidence when both a drawn
        // boundary and its architectural neighbor move in the same solve.
        auto mixed_document = fixture();
        auto mixed_wall = mixed_document.snapshot().entities().at("wall");
        mixed_wall.properties["baseline"]["start"] = {4.,0.};
        mixed_wall.properties["baseline"]["end"] = {6.,0.};
        mixed_document.apply(ApplyEntityChanges{mixed_document.revision(),
            {EntityChange::upsert(mixed_wall)}, {}, "Position wall"});
        const auto drawn = preview_boundary_commit(mixed_document.snapshot(),boundary_intent());
        require(drawn.accepted(),"mixed receipt fixture rejected");
        apply_boundary_commit(mixed_document,drawn);
        const auto boundary_id = drawn.created_boundary_ids().front();
        const auto geometry = decode_identified_boundary_entity(mixed_document.snapshot().entities().at(boundary_id));
        const auto edge = geometry.segments.front();
        PersistentConstraint joined;
        joined.id = "mixed-join";
        joined.relation = ConstraintRelationKind::coincident;
        joined.bindings = {{boundary_id,WallEndpointRole::end,edge.segment_id,edge.end_vertex_id},
            {"wall",WallEndpointRole::start}};
        ConstraintAuthoringIntent add_join;
        add_join.relation_mutations = {ConstraintRelationMutation::upsert(joined)};
        apply_constraint_authoring(mixed_document,preview_constraint_authoring(mixed_document.snapshot(),add_join));
        const auto receipt = mixed_document.snapshot().entities().at(boundary_id).properties.at("boundary_authoring");
        ProjectWorkspace mixed(mixed_document.snapshot());
        ConstraintAuthoringIntent mixed_resize;
        mixed_resize.wall_resize = WallResizeIntent{"wall",parse_quantity("3 m"),WallResizeAnchor::end,true};
        const auto mixed_preview = preview_constraint_authoring(mixed.snapshot(),mixed_resize);
        require(mixed_preview.accepted() && mixed_preview.changed_boundaries().size() == 1 &&
            mixed_preview.changed_walls().size() == 1,"mixed workspace preview omitted movement");
        exercise(mixed,mixed_preview,prepare_constraint,apply_constraint_authoring);
        require(mixed.snapshot().history().back().source_revision.has_value(),"workspace redo proof fixture missing");
        require(mixed.snapshot().entities().at(boundary_id).extensions.at("boundary_geometry_derivation")
            .at("source_boundary_authoring") == receipt,"workspace discarded exact construction inputs");
        const auto path = std::filesystem::temp_directory_path() / ("mixed-adapter-"+make_stable_id()+".bldproj");
        (void)ProjectStore::save(path,mixed.snapshot());
        auto reopened = ProjectStore::load(path);
        std::filesystem::remove(path);
        auto saved = Document::fork(mixed.snapshot());
        saved.mark_saved(saved.revision());
        require(document_snapshot_digest(reopened.document.snapshot()) == document_snapshot_digest(saved.snapshot()),
            "mixed workspace save/reopen lost typed history evidence");
        std::cout << "project_workspace_preview_adapters_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
