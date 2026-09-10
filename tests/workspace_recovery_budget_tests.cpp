#include "sketch/workspace_recovery_budget.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace {
bool watch_allocations = false;
bool oversized_allocation = false;
void* allocate(std::size_t size) {
    if (watch_allocations && size > 64U * 1024U) oversized_allocation = true;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace sketch;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("excessive aggregate recovery was accepted");
}
template<class F> void rejects_without_large_copy(F operation) {
    oversized_allocation = false; watch_allocations = true;
    try { rejects(operation); } catch (...) { watch_allocations = false; throw; }
    watch_allocations = false;
    require(!oversized_allocation, "raw preflight copied oversized input before rejection");
}
void check_raw_preallocation() {
    auto policy = boundary_authoring_default_resource_policy; policy.max_encoded_bytes = 150'000;
    const nlohmann::json escaped = std::string(100'000, '\x01');
    rejects_without_large_copy([&] { (void)detail::measure_authoring_recovery_json(escaped, policy); });
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0, 0});
    auto checkpoint = session.recovery_checkpoint();
    checkpoint.options.default_boundary_type = std::string(1'000'000, 'x');
    policy.max_encoded_bytes = 128;
    rejects_without_large_copy([&] { (void)detail::measure_authoring_checkpoint_raw(checkpoint, policy); });
    checkpoint = session.recovery_checkpoint();
    checkpoint.actions.front().classification = std::string(1'000'000, 'x');
    rejects_without_large_copy([&] { (void)detail::measure_authoring_checkpoint_raw(checkpoint, policy); });
    checkpoint = session.recovery_checkpoint();
    checkpoint.actions.front().kind = BoundaryAuthoringActionKind::set_classification;
    checkpoint.actions.front().point.reset();
    checkpoint.actions.front().classification = std::string(1'000'000, 'x');
    rejects_without_large_copy([&] { (void)detail::measure_authoring_checkpoint_raw(checkpoint, policy); });
}
void run() {
    auto doc = Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
    ProjectWorkspace w(doc.snapshot()); BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0, 0}); (void)session.add_line_to({5, 0});
    BoundaryActiveRecovery a{capture_boundary_recovery_source(w.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    auto activate = w.prepare_boundary_checkpoint(a); (void)w.commit(activate);
    auto discard = w.prepare_discard_boundary(); (void)w.commit(discard);
    const auto first = w.capture();
    const auto preflight = [](const ProjectWorkspaceSnapshot& s, const WorkspaceRecoveryLimits& limits = {}) {
        return preflight_workspace_recovery(s.document(), s.document_history(), s.lifecycle_history(), s.navigation(),
            s.active_boundary(), s.retired_boundaries(), s.resource_policy(), limits);
    };
    const auto first_usage = preflight(first);
    for (int i = 0; i < 4; ++i) {
        auto undo = w.prepare_undo(); (void)w.commit(undo);
        auto redo = w.prepare_redo(); (void)w.commit(redo);
    }
    const auto s = w.capture(); const auto usage = preflight(s);
    require(usage.inputs == 1 && usage.actions == a.checkpoint.actions.size() &&
            usage.actions == first_usage.actions && usage.encoded_bytes > first_usage.encoded_bytes,
            "shared references must charge metadata without duplicating checkpoint actions");
    validate_workspace_recovery(s.document(), s.document_history(), s.lifecycle_history(), s.navigation(),
        s.active_boundary(), s.retired_boundaries());
    const auto limited = [&](auto configure) { WorkspaceRecoveryLimits limits; configure(limits);
        rejects([&] { (void)preflight(s, limits); }); };
    limited([](auto& p) { p.max_events = 0; });
    limited([](auto& p) { p.max_inputs = 0; });
    limited([](auto& p) { p.max_actions = 0; });
    limited([](auto& p) { p.max_encoded_bytes = 1; });
    limited([](auto& p) { p.max_json_values = 1; });
    limited([](auto& p) { p.max_string_bytes = 1; });
    limited([](auto& p) { p.max_replay_work = 0; });
    limited([](auto& p) { p.max_document_revisions = 0; });
    limited([](auto& p) { p.max_entity_rows = 0; });
    limited([](auto& p) { p.max_validation_work = 0; });
    auto events = s.lifecycle_history();
    for (auto& event : events) if (event.input) {
        event.input->value = std::make_shared<const BoundaryActiveRecovery>(*event.input->value);
    }
    const auto separate = preflight_workspace_recovery(s.document(), s.document_history(), events, s.navigation(),
        s.active_boundary(), s.retired_boundaries());
    require(separate.inputs > usage.inputs && separate.actions > usage.actions,
            "distinct substituted payload allocations must not evade aggregate charging");
    WorkspaceRecoveryLimits small; small.max_inputs = 1;
    rejects([&] { (void)preflight_workspace_recovery(s.document(), s.document_history(), events, s.navigation(),
        s.active_boundary(), s.retired_boundaries(), s.resource_policy(), small); });
    WorkspaceRecoveryLimits exact;
    exact.max_events = usage.events; exact.max_inputs = usage.inputs; exact.max_actions = usage.actions;
    exact.max_encoded_bytes = usage.encoded_bytes; exact.max_json_values = usage.json_values;
    exact.max_string_bytes = usage.string_bytes; exact.max_replay_work = usage.replay_work;
    exact.max_document_revisions = usage.document_revisions; exact.max_entity_rows = usage.entity_rows;
    exact.max_validation_work = usage.validation_work;
    (void)preflight(s, exact);
    --exact.max_encoded_bytes; rejects([&] { (void)preflight(s, exact); });
    auto malformed = s.document();
    const_cast<std::vector<RevisionRecord>&>(malformed.history()).front().revision = 99;
    WorkspaceRecoveryLimits denied; denied.max_events = 0;
    bool budget_first = false;
    try { validate_workspace_recovery(malformed, s.document_history(), s.lifecycle_history(), s.navigation(),
        s.active_boundary(), s.retired_boundaries(), s.resource_policy(), denied); }
    catch (const std::invalid_argument& e) { budget_first = std::string(e.what()).find("budget") != std::string::npos; }
    require(budget_first, "resource rejection must precede canonical document restoration");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run(); check_raw_preallocation(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
