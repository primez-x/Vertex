#include "sketch/document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Entity wall(std::string id = "wall-a", double length = 4.0) {
    return {std::move(id), "wall",
            {{"baseline", {{"start", {0.0, 0.0}}, {"end", {length, 0.0}},
                           {"sweep_radians", 0.0}}},
             {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0}},
            false, json::object()};
}

json binding(std::string owner, std::string role) {
    return {{"owner_id", std::move(owner)}, {"feature", "baseline"},
            {"role", std::move(role)}};
}

Entity horizontal() {
    return {"horizontal-a", "constraint",
            {{"version", 1}, {"relation", "horizontal"}, {"wall_ids", {"wall-a"}},
             {"bindings", {binding("wall-a", "start"), binding("wall-a", "end")}}},
            false, {{"future_metadata", {{"preserve", true}}}}};
}

template <typename Function>
void rejected_unchanged(Document& document, Function&& operation, std::string_view message) {
    const auto before = document.snapshot();
    bool rejected = false;
    try { operation(); }
    catch (const DocumentError&) { rejected = true; }
    require(rejected, message);
    const auto after = document.snapshot();
    require(after.entities() == before.entities(), "rejected edit changed entities");
    require(after.assets() == before.assets(), "rejected edit changed assets");
    require(after.revision() == before.revision(), "rejected edit advanced revision");
    require(after.history().size() == before.history().size(), "rejected edit appended history");
    require(after.saved_revision_optional() == before.saved_revision_optional(),
            "rejected edit changed saved state");
}

void test_direct_edits_cannot_bypass_a_persisted_relation() {
    auto document = Document::create({wall(), horizontal()});
    document.mark_saved(document.revision());
    auto changed = wall();
    changed.properties["baseline"]["end"] = {4.0, 1.0};
    rejected_unchanged(document, [&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(changed)}, {},
                                          "bypass horizontal lock"});
    }, "raw Document::apply bypassed a horizontal constraint");
    rejected_unchanged(document, [&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase("wall-a")}, {},
                                          "delete locked owner"});
    }, "raw Document::apply deleted a constraint owner");

    changed = wall();
    changed.properties["name"] = "Renamed wall";
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(changed)}, {},
                                      "rename without solving"});
    require(document.snapshot().entities().at("wall-a").properties["baseline"] ==
                wall().properties["baseline"], "metadata edit moved constrained geometry");
    require(document.snapshot().entities().at("horizontal-a") == horizontal(),
            "metadata edit rewrote constraint metadata");
}

void test_relation_removal_and_geometry_edit_are_one_reversible_command() {
    auto document = Document::create({wall(), horizontal()});
    const auto original = document.snapshot().entities();
    auto changed = wall();
    changed.properties["baseline"]["end"] = {4.0, 1.0};
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::erase("horizontal-a"), EntityChange::upsert(changed)}, {},
        "remove lock and reshape"});
    require(!document.snapshot().entities().contains("horizontal-a"), "lock was not removed");
    document.undo(document.revision());
    require(document.snapshot().entities() == original, "undo did not restore lock and wall together");
    document.redo(document.revision());
    require(document.snapshot().entities().at("wall-a") == changed, "redo did not restore geometry");
    require(!document.snapshot().entities().contains("horizontal-a"), "redo restored removed lock");
}

void test_wall_shortening_cannot_strand_a_hosted_opening() {
    Entity opening{"door-a", "opening",
        {{"wall_id", "wall-a"}, {"offset_m", 2.5}, {"width_m", 1.0},
         {"sill_m", 0.0}, {"height_m", 2.1}}, false, json::object()};
    auto document = Document::create({wall(), horizontal(), opening});
    auto shortened = wall("wall-a", 2.0);
    rejected_unchanged(document, [&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(shortened)}, {},
                                          "shorten host beyond opening"});
    }, "a valid line relation concealed an invalid hosted opening");
}

void test_every_persisted_relation_has_an_independent_residual_check() {
    const auto check = [](std::vector<Entity> initial, Entity changed, std::string_view message) {
        auto document = Document::create(std::move(initial));
        require(document.is_editable(), "valid relation fixture was not editable");
        rejected_unchanged(document, [&] {
            document.apply(ApplyEntityChanges{document.revision(),
                {EntityChange::upsert(changed)}, {}, "violate a persisted relation"});
        }, message);
    };

    auto vertical_wall = wall();
    vertical_wall.properties["baseline"]["end"] = {0.0, 4.0};
    auto vertical = horizontal();
    vertical.properties["relation"] = "vertical";
    auto tilted = vertical_wall;
    tilted.properties["baseline"]["end"] = {0.01, 4.0};
    check({vertical_wall, vertical}, tilted, "vertical residual was not enforced");

    auto fixed_length = horizontal();
    fixed_length.properties["relation"] = "fixed_length";
    fixed_length.properties["length_m"] = 4.0;
    fixed_length.properties["quantity_entries"] = {{"/length_m",
        {{"version", 1}, {"original_expression", "4 m"}, {"entered_unit", "m"},
         {"exact_metres", {{"numerator", 4}, {"denominator", 1}}}}}};
    check({wall(), fixed_length}, wall("wall-a", 5.0), "fixed length residual was not enforced");

    auto anchor = horizontal();
    anchor.properties["relation"] = "fixed_anchor";
    anchor.properties["bindings"] = json::array({binding("wall-a", "start")});
    anchor.properties["anchor_m"] = {0.0, 0.0};
    auto moved = wall();
    moved.properties["baseline"]["start"] = {1.0, 0.0};
    check({wall(), anchor}, moved, "fixed anchor residual was not enforced");

    auto second = wall("wall-b");
    second.properties["baseline"]["start"] = {4.0, 0.0};
    second.properties["baseline"]["end"] = {4.0, 3.0};
    auto coincident = horizontal();
    coincident.properties["relation"] = "coincident";
    coincident.properties["wall_ids"] = {"wall-a", "wall-b"};
    coincident.properties["bindings"] = {binding("wall-a", "end"), binding("wall-b", "start")};
    moved = second;
    moved.properties["baseline"]["start"] = {5.0, 0.0};
    moved.properties["baseline"]["end"] = {5.0, 3.0};
    check({wall(), second, coincident}, moved, "coincident residual was not enforced");

    auto paired = coincident;
    paired.properties["relation"] = "perpendicular";
    paired.properties["bindings"] = {binding("wall-a", "start"), binding("wall-a", "end"),
                                      binding("wall-b", "start"), binding("wall-b", "end")};
    tilted = second;
    tilted.properties["baseline"]["end"] = {5.0, 3.0};
    check({wall(), second, paired}, tilted, "perpendicular residual was not enforced");

    paired.properties["relation"] = "parallel";
    second.properties["baseline"]["start"] = {0.0, 2.0};
    second.properties["baseline"]["end"] = {4.0, 2.0};
    tilted = second;
    tilted.properties["baseline"]["end"] = {4.0, 3.0};
    check({wall(), second, paired}, tilted, "parallel residual was not enforced");
}

void test_unknown_lock_semantics_are_preserved_read_only() {
    auto future = horizontal();
    future.properties["version"] = 99;
    future.properties["future_rule"] = {{"payload", {1, "retain", true}}};
    auto document = Document::create({wall(), future});
    require(!document.is_editable(), "unknown optional constraint version was ignored");
    require(document.snapshot().entities().at(future.id) == future, "unknown lock data changed");
    require(!document.read_only_reason().empty(), "unsupported lock needs an explanation");
    rejected_unchanged(document, [&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(wall("wall-a", 8))}, {},
                                          "bypass unknown lock"});
    }, "unknown lock allowed a generic edit");
}

void test_nested_owner_list_must_be_complete() {
    auto malformed = horizontal();
    malformed.properties["bindings"][1]["owner_id"] = "wall-b";
    bool rejected = false;
    try { (void)Document::create({wall(), wall("wall-b"), malformed}); }
    catch (const DocumentError&) { rejected = true; }
    require(rejected, "nested binding owner escaped the typed owner list");
}

void test_wall_reversal_requires_explicit_binding_remap() {
    auto document = Document::create({wall(), horizontal()});
    auto reversed = wall();
    reversed.properties["baseline"]["start"] = {4.0, 0.0};
    reversed.properties["baseline"]["end"] = {0.0, 0.0};
    rejected_unchanged(document, [&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(reversed)}, {},
                                          "reverse without remapping"});
    }, "wall reversal silently reassigned constrained endpoint identities");
    auto remapped = horizontal();
    remapped.properties["bindings"][0]["role"] = "end";
    remapped.properties["bindings"][1]["role"] = "start";
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(reversed), EntityChange::upsert(remapped)}, {}, "reverse and remap"});
    require(document.snapshot().entities().at("horizontal-a") == remapped,
            "explicit reversal did not retain remapped bindings");
    document.undo(document.revision());
    require(document.snapshot().entities().at("wall-a") == wall(), "undo did not restore wall roles");
    require(document.snapshot().entities().at("horizontal-a") == horizontal(),
            "undo did not restore constraint bindings");
}

void test_save_reopen_preserves_enforcement_and_history() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("property-constraint-integrity-" + make_stable_id());
    require(std::filesystem::create_directory(directory), "could not reserve fixture directory");
    const auto path = directory / "constraints.bldproj";
    // Only remove this fixture's exact file and then the empty directory. A
    // foreign file is preserved; there is no recursive cleanup.
    struct Cleanup {
        std::vector<std::filesystem::path> files;
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto& file : files) std::filesystem::remove(file, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{{path, directory / "unsupported-history.bldproj",
               directory / "invalid-history.bldproj",
               directory / "mixed-history.bldproj"}, directory};
    auto document = Document::create({wall(), horizontal()});
    auto renamed = wall();
    renamed.properties["name"] = "Stored wall";
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(renamed)}, {}, "rename"});
    (void)ProjectStore::save(path, document.snapshot());
    auto loaded = ProjectStore::load(path);
    require(loaded.document.snapshot().entities() == document.snapshot().entities(),
            "constraint save/reopen changed entities");
    loaded.document.undo(loaded.document.revision());
    require(loaded.document.snapshot().entities().at("horizontal-a") == horizontal(),
            "restored history lost the constraint");
    auto changed = wall();
    changed.properties["baseline"]["end"] = {4.0, 1.0};
    rejected_unchanged(loaded.document, [&] {
        loaded.document.apply(ApplyEntityChanges{loaded.document.revision(),
            {EntityChange::upsert(changed)}, {}, "bypass restored lock"});
    }, "restored document failed to enforce its lock");

    // Fault-inject a caller-owned, non-const snapshot to exercise the save and
    // restore trust boundary. Production code never edits snapshot internals.
    auto invalid_history = document.snapshot();
    auto& invalid_records = const_cast<std::vector<RevisionRecord>&>(invalid_history.history());
    invalid_records.front().entities.at("wall-a").properties["baseline"]["end"] = {4.0, 1.0};
    bool rejected = false;
    try { (void)ProjectStore::save(directory / "invalid-history.bldproj", invalid_history); }
    catch (const StorageError&) { rejected = true; }
    require(rejected, "save accepted a violated constraint hidden in older history");
    require(!std::filesystem::exists(directory / "invalid-history.bldproj"),
            "invalid history published a destination");

    // An unsupported historical lock must not disable transition validation
    // for the known locks alongside it. Both endpoint orders satisfy the
    // horizontal residual, but the stable endpoint roles must be remapped.
    auto mixed_history = document.snapshot();
    auto& mixed_records = const_cast<std::vector<RevisionRecord>&>(mixed_history.history());
    auto future_lock = horizontal();
    future_lock.id = "future-lock";
    future_lock.properties["version"] = 99;
    for (auto& record : mixed_records)
        record.entities.emplace(future_lock.id, future_lock);
    auto& reversed_baseline = mixed_records.back().entities.at("wall-a").properties["baseline"];
    reversed_baseline["start"] = {4.0, 0.0};
    reversed_baseline["end"] = {0.0, 0.0};
    rejected = false;
    const auto mixed_path = directory / "mixed-history.bldproj";
    try { (void)ProjectStore::save(mixed_path, mixed_history); }
    catch (const StorageError&) { rejected = true; }
    require(rejected, "unsupported history bypassed a known lock's endpoint remapping");
    require(!std::filesystem::exists(mixed_path), "invalid mixed history published a destination");

    auto& known_bindings = mixed_records.back().entities.at("horizontal-a").properties["bindings"];
    known_bindings[0]["role"] = "end";
    known_bindings[1]["role"] = "start";
    (void)ProjectStore::save(mixed_path, mixed_history);
    auto mixed_loaded = ProjectStore::load(mixed_path);
    require(!mixed_loaded.document.is_editable(), "valid mixed history lost its read-only lock");
    require(mixed_loaded.document.snapshot().entities() == mixed_history.entities(),
            "valid mixed history did not preserve the remapped endpoints");

    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::erase("horizontal-a")}, {}, "remove lock"});
    auto future_history = document.snapshot();
    auto& future_records = const_cast<std::vector<RevisionRecord>&>(future_history.history());
    for (auto& record : future_records) {
        const auto found = record.entities.find("horizontal-a");
        if (found != record.entities.end()) found->second.properties["version"] = 99;
    }
    const auto future_path = directory / "unsupported-history.bldproj";
    (void)ProjectStore::save(future_path, future_history);
    auto future_loaded = ProjectStore::load(future_path);
    require(!future_loaded.document.is_editable(),
            "unsupported lock in older history allowed entry into an unsafe undo state");
    require(future_loaded.document.snapshot().entities() == future_history.entities(),
            "read-only restoration changed the current geometry");
    rejected_unchanged(future_loaded.document, [&] {
        future_loaded.document.undo(future_loaded.document.revision());
    }, "undo entered unsupported lock semantics and trapped redo");
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        {
            IdentifiedBoundary boundary{"outline", "measurement_boundary", {
                {"ab", "a", "b", {{0, 0}, {4, 0}, 0}},
                {"bc", "b", "c", {{4, 0}, {4, 4}, 0}},
                {"cd", "c", "d", {{4, 4}, {0, 4}, 0}},
                {"da", "d", "a", {{0, 4}, {0, 0}, 0}}}};
            PersistentConstraint lock;
            lock.id = "level";
            lock.bindings = {{"outline", WallEndpointRole::start, "ab", "a"},
                             {"outline", WallEndpointRole::end, "ab", "b"}};
            auto document = Document::create({encode_identified_boundary_entity(boundary),
                                              encode_constraint_entity(lock)});
            boundary.segments[0].segment.end.y = 1;
            boundary.segments[1].segment.start.y = 1;
            rejected_unchanged(document, [&] {
                document.apply(ApplyEntityChanges{document.revision(),
                    {EntityChange::upsert(encode_identified_boundary_entity(boundary))}, {}, "bypass boundary lock"});
            }, "raw geometry edit bypassed boundary relation");
            rejected_unchanged(document, [&] {
                document.apply(ApplyEntityChanges{document.revision(),
                    {EntityChange::erase("outline")}, {}, "delete boundary owner"});
            }, "missing boundary constraint owner accepted");
            lock.bindings[1].vertex_id = "c";
            rejected_unchanged(document, [&] {
                document.apply(ApplyEntityChanges{document.revision(),
                    {EntityChange::upsert(encode_constraint_entity(lock))}, {}, "retarget wrong vertex"});
            }, "mismatched boundary segment vertex accepted");
        }
        test_direct_edits_cannot_bypass_a_persisted_relation();
        test_relation_removal_and_geometry_edit_are_one_reversible_command();
        test_wall_shortening_cannot_strand_a_hosted_opening();
        test_every_persisted_relation_has_an_independent_residual_check();
        test_unknown_lock_semantics_are_preserved_read_only();
        test_nested_owner_list_must_be_complete();
        test_wall_reversal_requires_explicit_binding_remap();
        test_save_reopen_preserves_enforcement_and_history();
        std::cout << "Persistent constraint integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_integrity_tests: " << error.what() << '\n';
        return 1;
    }
}
