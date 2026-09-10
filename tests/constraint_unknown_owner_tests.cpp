#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/document.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using json = nlohmann::json;
using namespace sketch;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Entity wall(std::string id = "wall-a") {
    return {std::move(id), "wall",
        {{"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                        {"sweep_radians", 0.0}}},
         {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0}},
        false, json::object()};
}

Entity curved_wall() {
    return {"wall-a", "wall",
        {{"baseline", {{"start", {2.0, 0.0}}, {"end", {0.0, 2.0}},
                        {"sweep_radians", std::numbers::pi / 2.0}}},
         {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0}},
        false, json::object()};
}

Entity future_constraint(std::uint64_t version, std::string relation,
                         std::string feature = "baseline", std::string role = "start") {
    return {"future-lock", "constraint",
        {{"version", version}, {"relation", std::move(relation)}, {"wall_ids", {"wall-a"}},
         {"bindings", {{{"owner_id", "wall-a"}, {"feature", std::move(feature)},
                        {"role", std::move(role)}}}}},
        false, {{"future_payload", {{"keep", true}, {"token", "opaque"}}}}};
}

Entity stranded_opening() {
    return {"opening-a", "opening",
        {{"wall_id", "wall-a"}, {"offset_m", 3.5}, {"width_m", 1.0},
         {"sill_m", 0.0}, {"height_m", 2.0}},
        false, json::object()};
}

template <typename Function>
void expect_create_rejected(Function&& make_entities, std::string_view message) {
    bool rejected = false;
    try {
        static_cast<void>(Document::create(make_entities()));
    } catch (const DocumentError&) {
        rejected = true;
    }
    require(rejected, message);
}

void test_unknown_envelopes_still_validate_host_dimensions() {
    const auto invalid_dimension = [] {
        auto owner = wall();
        owner.properties["thickness_m"] = 0.0;
        return std::vector<Entity>{owner, future_constraint(99, "horizontal")};
    };
    const auto degenerate_baseline = [] {
        auto owner = wall();
        owner.properties["baseline"]["end"] = {0.0, 0.0};
        return std::vector<Entity>{owner, future_constraint(99, "horizontal")};
    };
    const auto stranded = [] {
        return std::vector<Entity>{wall(), stranded_opening(),
                                   future_constraint(99, "horizontal")};
    };

    for (const auto& relation : {std::string("horizontal"), std::string("future_relation")}) {
        expect_create_rejected([&] {
            auto owner = wall();
            owner.properties["thickness_m"] = 0.0;
            return std::vector<Entity>{owner,
                                       future_constraint(relation == "horizontal" ? 99 : 1,
                                                          relation)};
        }, "future constraint did not reject an invalid host dimension");
        expect_create_rejected([&] {
            auto owner = wall();
            owner.properties["baseline"]["end"] = {0.0, 0.0};
            return std::vector<Entity>{owner,
                                       future_constraint(relation == "horizontal" ? 99 : 1,
                                                          relation)};
        }, "future constraint did not reject a degenerate host baseline");
        expect_create_rejected([&] {
            return std::vector<Entity>{wall(), stranded_opening(),
                                       future_constraint(relation == "horizontal" ? 99 : 1,
                                                          relation)};
        }, "future constraint did not reject a stranded hosted opening");
    }

    // Keep the named fixtures above as direct characterization lambdas too;
    // this makes the three rejection dimensions explicit to future maintainers.
    expect_create_rejected(invalid_dimension, "invalid future dimension fixture was accepted");
    expect_create_rejected(degenerate_baseline, "degenerate future baseline fixture was accepted");
    expect_create_rejected(stranded, "stranded future opening fixture was accepted");
}

void test_valid_future_envelope_is_preserved_and_loadable() {
    // A future feature/role may describe a curve endpoint. Its owner still
    // receives full shared wall validation, but v1 must not apply a straight-
    // wall residual to semantics it cannot interpret.
    const auto future = future_constraint(99, "future_relation", "curve", "midpoint");
    const auto original = std::vector<Entity>{curved_wall(), future};
    auto document = Document::create(original);
    require(!document.is_editable(), "valid future constraint was made editable");
    require(document.snapshot().entities().at("future-lock") == future,
            "valid future constraint changed during create");
    require(!document.read_only_reason().empty(), "future constraint needs a read-only reason");

    const auto directory = std::filesystem::temp_directory_path() /
        ("property-unknown-constraint-" + make_stable_id());
    require(std::filesystem::create_directory(directory), "could not reserve fixture directory");
    const auto path = directory / "future.bldproj";
    struct Cleanup {
        std::filesystem::path path;
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{path, directory};
    static_cast<void>(ProjectStore::save(path, document.snapshot()));
    auto loaded = ProjectStore::load(path);
    require(!loaded.document.is_editable(), "loaded future constraint became editable");
    require(loaded.document.snapshot().entities() == document.snapshot().entities(),
            "load changed valid future constraint payload");
    require(loaded.document.snapshot().entities().at("future-lock").extensions.at(
                 "future_payload").at("token") == "opaque",
            "load dropped opaque future metadata");
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_unknown_envelopes_still_validate_host_dimensions();
        test_valid_future_envelope_is_preserved_and_loadable();
        std::cout << "Unknown constraint owner tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_unknown_owner_tests: " << error.what() << '\n';
        return 1;
    }
}
