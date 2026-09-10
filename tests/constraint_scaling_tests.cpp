#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/document.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_semantics.hpp"
#include "support/noninteractive_errors.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::json;
using Entities = std::map<std::string, sketch::Entity, std::less<>>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

json wall_properties(std::size_t index) {
    const auto origin = static_cast<double>(index) * 2.0;
    return {
        {"baseline", {
            {"start", {origin, 0.0}},
            {"end", {origin + 1.0, 0.0}},
            {"sweep_radians", 0.0},
        }},
        {"thickness_m", 0.20},
        {"height_m", 3.0},
        {"elevation_m", 0.0},
    };
}

json opening_properties(const std::string& wall_id, bool invalid_host) {
    if (invalid_host) {
        // The opening remains attached to the indexed host, but extends beyond
        // its one-metre baseline.  This exercises negative hosted validation.
        return {
            {"wall_id", wall_id},
            {"offset_m", 0.20},
            {"width_m", 2.0},
            {"sill_m", 0.10},
            {"height_m", 1.0},
        };
    }
    return {
        {"wall_id", wall_id},
        {"offset_m", 0.20},
        {"width_m", 0.30},
        {"sill_m", 0.10},
        {"height_m", 1.0},
    };
}

Entities make_fixture(std::size_t count, bool invalid_host) {
    Entities entities;
    for (std::size_t index = 0; index < count; ++index) {
        const auto wall_id = "wall-" + std::to_string(index);
        const auto opening_id = "opening-" + std::to_string(index);
        entities.emplace(wall_id, sketch::Entity{
            wall_id, "wall", wall_properties(index), false, json::object()});
        entities.emplace(opening_id, sketch::Entity{
            opening_id, "opening", opening_properties(
                wall_id, invalid_host && index == count / 2), false, json::object()});

        sketch::PersistentConstraint constraint;
        constraint.id = "constraint-" + std::to_string(index);
        constraint.relation = sketch::ConstraintRelationKind::horizontal;
        constraint.bindings = {
            {wall_id, sketch::WallEndpointRole::start},
            {wall_id, sketch::WallEndpointRole::end},
        };
        const auto encoded = sketch::encode_constraint_entity(constraint);
        entities.emplace(encoded.id, encoded);
    }

    // A malformed unrelated opening was historically ignored by read_wall.
    // Keep it in every fixture so indexing cannot accidentally broaden the
    // validator's semantic scope while removing the map rescans.
    entities.emplace("unrelated-opening", sketch::Entity{
        "unrelated-opening", "opening", {{"wall_id", 17}}, false, json::object()});
    return entities;
}

void validate_valid_fixture(std::size_t count) {
    auto entities = make_fixture(count, false);
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::string> unsupported;
    try {
        unsupported = sketch::validate_constraint_integrity(entities);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "valid fixture of size " + std::to_string(count) + " threw: " + error.what());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    require(!unsupported.has_value(),
        "valid fixture of size " + std::to_string(count) + " reported unsupported semantics");
    std::cout << "valid_host_fixture entities=" << entities.size()
              << " constrained_walls=" << count << " elapsed_us=" << elapsed.count() << '\n';
}

void validate_negative_fixture(std::size_t count) {
    auto entities = make_fixture(count, true);
    const auto started = std::chrono::steady_clock::now();
    bool rejected = false;
    try {
        static_cast<void>(sketch::validate_constraint_integrity(entities));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    require(rejected,
        "negative hosted fixture of size " + std::to_string(count) + " was accepted");
    std::cout << "negative_host_fixture entities=" << entities.size()
              << " constrained_walls=" << count << " elapsed_us=" << elapsed.count() << '\n';
}

sketch::Wall make_opening_scale_fixture(std::size_t count, bool invalid_overlap) {
    const auto wall_length = static_cast<double>(count) * 2.0 + 1.0;
    sketch::Wall wall{
        "opening-scale", {{0.0, 0.0}, {wall_length, 0.0}, 0.0}, 0.20, 3.0, 0.0, {}};
    wall.openings.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto offset = static_cast<double>(index) * 2.0;
        if (invalid_overlap && index == count / 2) offset -= 1.0;
        wall.openings.push_back({"opening-" + std::to_string(index), offset, 2.0, 0.0, 3.0});
    }
    return wall;
}

void validate_opening_scale_fixture(std::size_t count, bool invalid_overlap) {
    auto wall = make_opening_scale_fixture(count, invalid_overlap);
    const auto started = std::chrono::steady_clock::now();
    bool rejected = false;
    try {
        sketch::validate_wall_semantics(wall);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    require(rejected == invalid_overlap,
        std::string(invalid_overlap ? "negative" : "valid") +
            " one-wall opening fixture of size " + std::to_string(count) +
            " had an unexpected validation result");
    std::cout << (invalid_overlap ? "negative_opening_fixture" : "valid_opening_fixture")
              << " openings=" << count << " elapsed_us=" << elapsed.count() << '\n';
}

void validate_history_like_repeats() {
    constexpr std::size_t count = 256;
    constexpr std::size_t revisions = 32;
    const auto entities = make_fixture(count, false);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t revision = 0; revision < revisions; ++revision) {
        const auto unsupported = sketch::validate_constraint_integrity(entities);
        require(!unsupported.has_value(),
            "history-like valid fixture reported unsupported semantics at revision " +
            std::to_string(revision));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "history_like_revisions revisions=" << revisions
              << " constrained_walls=" << count << " elapsed_us=" << elapsed.count() << '\n';
}

void validate_document_history_fixture() {
    constexpr std::size_t count = 64;
    constexpr std::size_t revisions = 16;
    auto fixture = make_fixture(count, false);
    std::vector<sketch::Entity> initial;
    initial.reserve(fixture.size() - 1);
    for (const auto& [id, entity] : fixture) {
        // The direct integrity fixture intentionally carries a malformed
        // unrelated opening to characterize ignored input. Document's typed
        // reference validator correctly rejects that field, so omit it from
        // this history/restore fixture.
        if (id != "unrelated-opening") initial.push_back(entity);
    }

    const auto started = std::chrono::steady_clock::now();
    auto document = sketch::Document::create(std::move(initial));
    for (std::size_t revision = 0; revision < revisions; ++revision) {
        auto changed = document.snapshot().entities().at("wall-0");
        changed.properties["history_marker"] = static_cast<std::uint64_t>(revision);
        document.apply(sketch::ApplyEntityChanges{
            document.revision(), {sketch::EntityChange::upsert(std::move(changed))}, {},
            "constraint scaling metadata revision"});
    }
    const auto expected_entities = document.snapshot().entities();

    const auto directory = std::filesystem::temp_directory_path() /
        ("property-constraint-scaling-" + sketch::make_stable_id());
    require(std::filesystem::create_directory(directory),
            "could not reserve history scaling fixture directory");
    const auto path = directory / "history.bldproj";
    struct Cleanup {
        std::filesystem::path path;
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{path, directory};

    static_cast<void>(sketch::ProjectStore::save(path, document.snapshot()));
    auto loaded = sketch::ProjectStore::load(path);
    require(loaded.document.snapshot().entities() == expected_entities,
            "history scaling save/reopen changed constrained entities");
    loaded.document.undo(loaded.document.revision());
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == expected_entities,
            "history scaling restore changed constrained entities");

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "document_history_restore revisions=" << revisions
              << " constrained_walls=" << count << " elapsed_us=" << elapsed.count() << '\n';
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        for (const auto count : {std::size_t{1000}, std::size_t{2000}, std::size_t{4000}}) {
            validate_valid_fixture(count);
            validate_negative_fixture(count);
            validate_opening_scale_fixture(count, false);
            validate_opening_scale_fixture(count, true);
        }
        validate_history_like_repeats();
        validate_document_history_fixture();
        std::cout << "constraint scaling checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint scaling checks failed: " << error.what() << '\n';
        return 1;
    }
}
