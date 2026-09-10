#include "sketch/project_workspace.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"
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
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_publication(); check_rejections(); check_checkpoint_policy(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
