#include "sketch/typed_relationships.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(RelationshipErrorCode code, F action) {
    try { action(); }
    catch (const RelationshipError& error) {
        require(error.code() == code, "wrong rejection reason");
        return;
    }
    throw std::runtime_error("invalid relationship accepted");
}
TypedRelationship link(std::string id, std::string source, std::string target,
                       RelationshipKind kind = RelationshipKind::wall_derived) {
    return {std::move(id), kind, "owner", std::move(source), std::move(target)};
}
void state_and_snapshot_tests() {
    const TypedRelationshipGraph independent({"owner", "wall", "room", "measurement"});
    const auto linked = independent.create(link("r1", "wall", "room"));
    const auto frozen = linked.freeze("r1");
    const auto disconnected = frozen.disconnect("r1");
    require(independent.relationships().empty(), "create mutated snapshot");
    require(linked.relationships()[0].state == RelationshipState::connected, "freeze mutated snapshot");
    require(frozen.relationships()[0].state == RelationshipState::frozen, "disconnect mutated snapshot");
    require(disconnected.relationships()[0].state == RelationshipState::disconnected, "disconnect lost state");
    require(disconnected.relationships()[0].source_id == "wall", "disconnect lost provenance");
    auto copy = frozen;
    copy = independent;
    require(frozen.relationships().size() == 1, "copy assignment aliased snapshot");
    rejects(RelationshipErrorCode::invalid_transition, [&] { (void)frozen.freeze("r1"); });
    rejects(RelationshipErrorCode::invalid_transition, [&] { (void)disconnected.freeze("r1"); });
    rejects(RelationshipErrorCode::invalid_transition, [&] { (void)disconnected.disconnect("r1"); });
    rejects(RelationshipErrorCode::missing_relationship, [&] { (void)linked.disconnect("absent"); });
    require(disconnected.create(link("r2", "measurement", "room", RelationshipKind::room_boundary)).relationships().size() == 2,
            "disconnected link did not release target");
}
void validation_tests() {
    const TypedRelationshipGraph base({"owner", "a", "b", "c"});
    const auto graph = base.create(link("1", "a", "b"))
                           .create(link("2", "b", "c", RelationshipKind::appraisal_measurement_boundary));
    const auto before = graph.serialize();
    rejects(RelationshipErrorCode::cycle, [&] { (void)graph.create(link("3", "c", "a")); });
    rejects(RelationshipErrorCode::cycle, [&] { (void)base.create(link("3", "a", "a")); });
    rejects(RelationshipErrorCode::ambiguous_target, [&] { (void)graph.create(link("3", "a", "c", RelationshipKind::room_boundary)); });
    rejects(RelationshipErrorCode::ambiguous_target, [&] { (void)graph.freeze("2").create(link("3", "a", "c")); });
    rejects(RelationshipErrorCode::duplicate_id, [&] { (void)graph.create(link("1", "c", "a")); });
    for (int field = 0; field < 3; ++field) {
        auto dangling = link("3", "a", "b");
        (field == 0 ? dangling.owner_id : field == 1 ? dangling.source_id : dangling.target_id) = "missing";
        rejects(RelationshipErrorCode::dangling_reference, [&] { (void)base.create(dangling); });
        dangling.state = RelationshipState::disconnected;
        rejects(RelationshipErrorCode::dangling_reference, [&] { (void)TypedRelationshipGraph(base.object_ids(), {dangling}); });
    }
    auto invalid = link("3", "a", "b");
    invalid.kind = static_cast<RelationshipKind>(99);
    rejects(RelationshipErrorCode::invalid_input, [&] { (void)base.create(invalid); });
    invalid = link("3", "a", "b");
    invalid.state = static_cast<RelationshipState>(99);
    rejects(RelationshipErrorCode::invalid_input, [&] { (void)TypedRelationshipGraph(base.object_ids(), {invalid}); });
    invalid.state = RelationshipState::frozen;
    rejects(RelationshipErrorCode::invalid_transition, [&] { (void)base.create(invalid); });
    rejects(RelationshipErrorCode::duplicate_id, [] { (void)TypedRelationshipGraph({"a", "a"}); });
    rejects(RelationshipErrorCode::invalid_input, [] { (void)TypedRelationshipGraph({""}); });
    rejects(RelationshipErrorCode::invalid_input, [] { (void)TypedRelationshipGraph({std::string(257, 'a')}); });
    rejects(RelationshipErrorCode::invalid_input, [] { (void)TypedRelationshipGraph({std::string(1, static_cast<char>(0xff))}); });
    rejects(RelationshipErrorCode::invalid_input, [&] { (void)base.create(link("", "a", "b")); });
    rejects(RelationshipErrorCode::limit_exceeded, [] {
        (void)TypedRelationshipGraph(std::vector<std::string>(TypedRelationshipGraph::maximum_objects + 1, "a"));
    });
    rejects(RelationshipErrorCode::limit_exceeded, [&] {
        (void)TypedRelationshipGraph(base.object_ids(), std::vector<TypedRelationship>(TypedRelationshipGraph::maximum_relationships + 1));
    });
    rejects(RelationshipErrorCode::cycle, [&] {
        (void)TypedRelationshipGraph(base.object_ids(), {link("1", "a", "b"), link("2", "b", "a")});
    });
    rejects(RelationshipErrorCode::cycle, [&] { (void)graph.freeze("1").create(link("3", "c", "a")); });
    require(before == graph.serialize(), "rejected operations mutated graph");
    const auto acyclic = graph.disconnect("1").create(link("3", "c", "a"));
    require(acyclic.relationships().size() == 3, "disconnected edge participated in cycle detection");
}
void long_chain_tests() {
    std::vector<std::string> objects{"owner"};
    std::vector<TypedRelationship> links;
    for (std::size_t i = 0; i < TypedRelationshipGraph::maximum_objects - 1; ++i) {
        objects.push_back(std::to_string(i));
        if (i != 0) links.push_back(link("r" + std::to_string(i), std::to_string(i - 1), std::to_string(i)));
    }
    const TypedRelationshipGraph chain(objects, links);
    require(chain.object_ids().size() == TypedRelationshipGraph::maximum_objects, "maximum valid chain rejected");
    rejects(RelationshipErrorCode::cycle, [&] {
        (void)chain.create(link("closing", std::to_string(TypedRelationshipGraph::maximum_objects - 2), "0"));
    });
}
void serialization_tests() {
    auto first = link("z", "a", "b");
    auto second = link("a", "b", "c", RelationshipKind::room_boundary);
    const TypedRelationshipGraph left({"owner", "c", "a", "b"}, {first, second});
    const TypedRelationshipGraph right({"a", "b", "c", "owner"}, {second, first});
    require(left.serialize() == right.serialize(), "serialization depends on insertion order");
    const auto json = nlohmann::json::parse(left.freeze("z").serialize());
    require(json.at("version") == 1 && json.at("relationships")[1].at("state") == "frozen", "serialized state missing");
    require(json.at("relationships")[0].at("kind") == "room_boundary", "serialized type missing");
    const std::string quoted = "quote\"\\\n";
    const TypedRelationshipGraph escaped({"owner", quoted, "b"}, {link("id", quoted, "b")});
    require(nlohmann::json::parse(escaped.serialize()).at("relationships")[0].at("source_id") == quoted,
            "serialization did not escape IDs");
}
}
int main() {
    try { state_and_snapshot_tests(); validation_tests(); long_chain_tests(); serialization_tests(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "typed relationship tests passed\n";
}
