#include "sketch/model_phases.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class Operation> void rejects(Operation operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid phases accepted");
}
sketch::ModelPhases fixture() {
    return sketch::ModelPhases::create({"wall", "new-b", "floor", "new-a"}, {"wall", "floor"},
        {{"b", "Retain wall", {}, {"new-b"}}, {"a", "Replace wall", {"wall"}, {"new-a"}}}, "a");
}
void isolation_and_comparison() {
    const auto original = fixture();
    const auto saved = original.to_json();
    const auto a = original.active_state();
    require(a.at("wall") == sketch::ModelPhase::demolished && a.at("new-a") == sketch::ModelPhase::proposed,
            "selected alternative preserves explicit demolition and proposal phases");
    require(a.at("floor") == sketch::ModelPhase::existing && !a.contains("new-b"), "unrelated proposal is excluded");
    const auto switched = original.with_active("b");
    require(switched.active_state().at("wall") == sketch::ModelPhase::existing &&
            !switched.active_state().contains("new-a"), "alternative switching is exclusive");
    const auto baseline = original.with_active(std::nullopt).active_state();
    require(baseline.size() == 2 && baseline.at("wall") == sketch::ModelPhase::existing,
            "shared baseline is unchanged");
    const auto comparison = original.compare("a", "b");
    require(comparison.left_alternative == "a" && comparison.right_alternative == "b" &&
            comparison.differences.size() == 3, "comparison identifies both alternatives");
    require(comparison.differences[0] == sketch::PhaseComparisonEntry{"new-a", sketch::ModelPhase::proposed, std::nullopt},
            "comparison distinguishes absent from demolition");
    require(comparison.differences[2] == sketch::PhaseComparisonEntry{"wall", sketch::ModelPhase::demolished, sketch::ModelPhase::existing},
            "comparison reports retained versus demolished");
    require(original.compare("a", "a").differences.empty(), "self comparison is empty");
    require(!original.compare(std::nullopt, "a").left_alternative, "baseline comparison is explicit");
    require(original.to_json() == saved && original.active_alternative() == "a", "operations do not mutate original");
    rejects([&] { (void)original.with_active("missing"); });
    rejects([&] { (void)original.compare("a", "missing"); });
}
void serialization_and_validation() {
    const auto model = fixture();
    const auto serialized = model.to_json();
    require(sketch::ModelPhases::from_json(serialized).to_json().dump() == serialized.dump(), "canonical roundtrip");
    const auto reordered = sketch::ModelPhases::create({"floor", "wall", "new-a", "new-b"}, {"floor", "wall"},
        {{"a", "Replace wall", {"wall"}, {"new-a"}}, {"b", "Retain wall", {}, {"new-b"}}}, "a");
    require(reordered.to_json().dump() == serialized.dump(), "input ordering does not affect canonical JSON");
    auto detached = serialized;
    detached["baseline_ids"] = {"bogus"};
    rejects([&] { (void)sketch::ModelPhases::from_json(detached); });
    require(model.to_json() == serialized, "exported JSON is independent");
    for (const auto* field : {"entity_ids", "baseline_ids", "alternatives", "active_alternative", "schema", "version"}) {
        auto invalid = serialized;
        invalid.erase(field);
        rejects([&] { (void)sketch::ModelPhases::from_json(invalid); });
    }
    for (const auto& invalid_version : {nlohmann::json(2), nlohmann::json(1.0), nlohmann::json("1")}) {
        auto invalid = serialized;
        invalid["version"] = invalid_version;
        rejects([&] { (void)sketch::ModelPhases::from_json(invalid); });
    }
    auto invalid = serialized;
    invalid["active_alternative"] = nlohmann::json::array({"a", "b"});
    rejects([&] { (void)sketch::ModelPhases::from_json(invalid); });
    invalid = serialized;
    invalid["extra"] = true;
    rejects([&] { (void)sketch::ModelPhases::from_json(invalid); });
    require(sketch::ModelPhases::create({}, {}, {}).active_state().empty(), "empty model is valid");
    rejects([] { (void)sketch::ModelPhases::create({"x", "x"}, {"x"}, {}); });
    rejects([] { (void)sketch::ModelPhases::create({" "}, {" "}, {}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {}, {}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {"x", "x"}, {}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {"x"}, {}, "unknown"); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {"x"}, {{"a", "", {}, {}}, {"a", "", {}, {}}}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {"x"}, {{"a", "", {"missing"}, {}}}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {"x"}, {{"a", "", {"x"}, {"x"}}}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {}, {{"a", "", {}, {"x"}}, {"b", "", {}, {"x"}}}); });
    rejects([] { (void)sketch::ModelPhases::create({}, {}, {{"a", "", {}, {"unknown"}}}); });
    rejects([] { (void)sketch::ModelPhases::create({"x"}, {}, {{"a", "", {}, {"x", "x"}}}); });
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        isolation_and_comparison();
        serialization_and_validation();
        std::cout << "model phases tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
