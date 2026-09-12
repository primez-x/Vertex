#include "sketch/vertical_levels.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <limits>

namespace {
using namespace sketch;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(VerticalLevelErrorCode code, F action) {
    try { action(); }
    catch (const VerticalLevelError& error) { require(error.code() == code, "Wrong rejection code"); return; }
    throw std::runtime_error("Invalid vertical graph accepted");
}
void lifecycle() {
    const VerticalLevelGraph base({{"ground", 0}, {"first", 3}, {"roof", 7}});
    require(base.links().empty(), "Levels inferred unwanted relationships");
    const auto linked = base.create_link({"storey", "ground", "first"});
    const auto changed = linked.with_elevation("first", 4);
    require(linked.floor_to_floor_height("storey") == 3 && changed.floor_to_floor_height("storey") == 4,
            "Connected height did not follow immutable elevation edit");
    const auto frozen = changed.freeze("storey");
    const auto before = frozen.serialize();
    rejects(VerticalLevelErrorCode::frozen_height_changed, [&] { (void)frozen.with_elevation("first", 5); });
    require(frozen.serialize() == before, "Rejected edit changed graph");
    require(frozen.with_elevation("roof", 8).floor_to_floor_height("storey") == 4, "Freeze blocked unrelated level");
    const auto detached = frozen.disconnect("storey").with_elevation("first", -2);
    require(detached.floor_to_floor_height("storey") == 4, "Disconnected height lost provenance");
    require(detached.links()[0].lower_level_id == "ground", "Disconnected link lost source");
    const auto replacement = detached.create_link({"new", "first", "ground"});
    require(replacement.floor_to_floor_height("new") == 2, "Disconnected edge still constrained graph");
    require(linked.disconnect("storey").floor_to_floor_height("storey") == 3, "Direct disconnect lost height");
    rejects(VerticalLevelErrorCode::invalid_transition, [&] { (void)frozen.freeze("storey"); });
    rejects(VerticalLevelErrorCode::invalid_transition, [&] { (void)detached.freeze("storey"); });
    rejects(VerticalLevelErrorCode::invalid_transition, [&] { (void)detached.disconnect("storey"); });
    rejects(VerticalLevelErrorCode::missing_level, [&] { (void)base.with_elevation("missing", 1); });
    rejects(VerticalLevelErrorCode::missing_link, [&] { (void)base.freeze("missing"); });
    rejects(VerticalLevelErrorCode::missing_link, [&] { (void)base.floor_to_floor_height("missing"); });
    require(base.create_level({"basement", -3}).levels().size() == 4 && base.levels().size() == 3,
            "Level creation mutated source");
}
void validation() {
    const std::vector<VerticalLevel> levels{{"a", 0}, {"b", 3}, {"c", 6}};
    const VerticalLevelGraph base(levels);
    const auto linked = base.create_link({"ab", "a", "b"});
    rejects(VerticalLevelErrorCode::duplicate_id, [&] { (void)base.create_level({"a", 10}); });
    rejects(VerticalLevelErrorCode::duplicate_id, [&] { (void)linked.create_link({"ab", "b", "c"}); });
    rejects(VerticalLevelErrorCode::duplicate_link, [&] { (void)linked.freeze("ab").create_link({"other", "a", "b"}); });
    rejects(VerticalLevelErrorCode::dangling_reference, [&] { (void)base.create_link({"x", "a", "missing"}); });
    rejects(VerticalLevelErrorCode::cycle, [&] { (void)base.create_link({"self", "a", "a"}); });
    rejects(VerticalLevelErrorCode::cycle, [&] { (void)linked.create_link({"back", "b", "a"}); });
    rejects(VerticalLevelErrorCode::non_monotonic, [&] { (void)base.create_link({"down", "b", "a"}); });
    rejects(VerticalLevelErrorCode::non_monotonic, [&] { (void)linked.with_elevation("b", 0); });
    rejects(VerticalLevelErrorCode::invalid_input, [&] { (void)base.with_elevation("b", std::numeric_limits<double>::infinity()); });
    rejects(VerticalLevelErrorCode::invalid_input, [&] { (void)base.with_elevation("b", std::numeric_limits<double>::quiet_NaN()); });
    rejects(VerticalLevelErrorCode::invalid_input, [] { (void)VerticalLevelGraph({{"", 0}}); });
    rejects(VerticalLevelErrorCode::invalid_input, [] { (void)VerticalLevelGraph({{std::string(257, 'x'), 0}}); });
    rejects(VerticalLevelErrorCode::invalid_input, [] { (void)VerticalLevelGraph({{std::string(1, static_cast<char>(0xff)), 0}}); });
    rejects(VerticalLevelErrorCode::invalid_input, [&] {
        (void)VerticalLevelGraph(levels, {{"x", "a", "b", static_cast<RelationshipState>(99)}});
    });
    rejects(VerticalLevelErrorCode::invalid_input, [&] {
        (void)VerticalLevelGraph(levels, {{"x", "a", "b", RelationshipState::frozen}});
    });
    rejects(VerticalLevelErrorCode::invalid_input, [&] {
        (void)VerticalLevelGraph(levels, {{"x", "a", "b", RelationshipState::connected, 3}});
    });
    rejects(VerticalLevelErrorCode::frozen_height_changed, [&] {
        (void)VerticalLevelGraph(levels, {{"x", "a", "b", RelationshipState::frozen, 4}});
    });
    rejects(VerticalLevelErrorCode::dangling_reference, [&] {
        (void)VerticalLevelGraph(levels, {{"x", "a", "missing", RelationshipState::disconnected, 4}});
    });
    rejects(VerticalLevelErrorCode::invalid_transition, [&] {
        (void)base.create_link({"x", "a", "b", RelationshipState::frozen, 3});
    });
    rejects(VerticalLevelErrorCode::non_monotonic, [] {
        const auto max = std::numeric_limits<double>::max();
        (void)VerticalLevelGraph({{"a", -max}, {"b", max}}, {{"x", "a", "b"}});
    });
    rejects(VerticalLevelErrorCode::limit_exceeded, [] {
        (void)VerticalLevelGraph(std::vector<VerticalLevel>(VerticalLevelGraph::maximum_levels + 1));
    });
    rejects(VerticalLevelErrorCode::limit_exceeded, [&] {
        (void)VerticalLevelGraph(levels, std::vector<FloorToFloorLink>(VerticalLevelGraph::maximum_links + 1));
    });
}
void deterministic_and_bounded() {
    const VerticalLevelGraph a({{"b", 3}, {"a", -0.0}, {"c", 7}}, {{"z", "a", "b"}, {"y", "b", "c"}});
    const VerticalLevelGraph b({{"c", 7}, {"a", 0.0}, {"b", 3}}, {{"y", "b", "c"}, {"z", "a", "b"}});
    require(a.serialize() == b.serialize(), "Serialization depends on input order or signed zero");
    const auto json = nlohmann::json::parse(a.freeze("z").serialize());
    require(json.at("version") == 1 && json.at("links")[1].at("height_m") == 3 &&
            json.at("links")[1].at("state") == "frozen", "Serialization omitted retained state");
    const auto decoded = VerticalLevelGraph::from_json(json);
    require(decoded.serialize() == a.freeze("z").serialize(),
            "Vertical level JSON did not round-trip through the validated decoder");
    auto malformed = json;
    malformed.at("links")[0].at("height_m") = -1;
    rejects(VerticalLevelErrorCode::invalid_input, [&] {
        (void)VerticalLevelGraph::from_json(malformed);
    });
    malformed = json;
    malformed.at("links")[0].erase("height_m");
    rejects(VerticalLevelErrorCode::invalid_input, [&] {
        (void)VerticalLevelGraph::from_json(malformed);
    });
    const std::string escaped = "quote\"\\\n";
    require(nlohmann::json::parse(VerticalLevelGraph({{escaped, 0}}).serialize()).at("levels")[0].at("id") == escaped,
            "JSON escaping failed");
    std::vector<VerticalLevel> levels;
    std::vector<FloorToFloorLink> links;
    for (std::size_t i = 0; i < VerticalLevelGraph::maximum_levels; ++i) {
        levels.push_back({std::to_string(i), static_cast<double>(i)});
        if (i) links.push_back({"r" + std::to_string(i), std::to_string(i - 1), std::to_string(i)});
    }
    const VerticalLevelGraph chain(levels, links);
    require(chain.levels().size() == VerticalLevelGraph::maximum_levels, "Maximum chain rejected");
    rejects(VerticalLevelErrorCode::cycle, [&] {
        (void)chain.create_link({"back", std::to_string(VerticalLevelGraph::maximum_levels - 1), "0"});
    });
}
}
int main() {
    try { lifecycle(); validation(); deterministic_and_bounded(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "vertical level tests passed\n";
}
