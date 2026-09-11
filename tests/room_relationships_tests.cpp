#include "sketch/room_relationships.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace sketch;
using R = RoomRelationKind;
using K = RoomReferenceKind;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid relationship accepted");
}
std::vector<RoomReference> refs() {
    return {{"room", K::room_boundary}, {"measure", K::appraisal_measurement_boundary},
            {"wall-a", K::architectural_wall}, {"wall-b", K::architectural_wall}};
}
void run() {
    static_assert(std::is_same_v<decltype(std::declval<RoomRelationshipSnapshot&>().references()),
                                const std::vector<RoomReference>&>);
    auto references = refs();
    std::vector<RoomRelation> relations{{"room", "wall-a", R::derived_from},
        {"room", "wall-b", R::derived_from}, {"measure", "room", R::independent}};
    const auto snapshot = RoomRelationshipSnapshot::create(references, relations);
    const auto retained = snapshot.to_json().dump();
    std::reverse(references.begin(), references.end());
    std::reverse(relations.begin(), relations.end());
    require(RoomRelationshipSnapshot::create(references, relations).to_json().dump() == retained,
            "Input order affects canonical serialization");
    references.front().id = "changed";
    relations.clear();
    require(snapshot.to_json().dump() == retained, "Snapshot aliases caller storage");
    require(RoomRelationshipSnapshot::from_json(snapshot.to_json()).to_json().dump() == retained,
            "Roundtrip loses semantic roles or relations");
    require(RoomRelationshipSnapshot::create(refs(), {{"room", "measure", R::independent}}).to_json() ==
            RoomRelationshipSnapshot::create(refs(), {{"measure", "room", R::independent}}).to_json(),
            "Independent relation orientation affects serialization");
    const auto unlinked = RoomRelationshipSnapshot::create(refs(), {});
    require(unlinked.relations().empty(), "Missing relations infer unwanted coupling");
    const auto follows = RoomRelationshipSnapshot::create(refs(), {{"measure", "room", R::follows},
        {"room", "wall-a", R::derived_from}});
    require(follows.relations().size() == 2, "Valid dependency chain lost");
    rejects([] { auto r = refs(); r.push_back({"room", K::architectural_wall}); RoomRelationshipSnapshot::create(r, {}); });
    rejects([] { RoomRelationshipSnapshot::create({{" ", K::room_boundary}}, {}); });
    rejects([] { RoomRelationshipSnapshot::create({{"a", static_cast<K>(99)}}, {}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "missing", R::follows}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "room", R::independent}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "wall-a", static_cast<R>(99)}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"wall-a", "room", R::follows}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "measure", R::follows}, {"measure", "room", R::derived_from}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "wall-a", R::follows}, {"room", "wall-b", R::follows}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "wall-a", R::follows}, {"room", "wall-b", R::derived_from}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "wall-a", R::derived_from}, {"room", "wall-a", R::derived_from}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"room", "measure", R::independent}, {"measure", "room", R::independent}}); });
    rejects([] { RoomRelationshipSnapshot::create(refs(), {{"measure", "room", R::follows},
        {"room", "wall-a", R::derived_from}, {"wall-a", "measure", R::independent}}); });
    auto malformed = snapshot.to_json();
    malformed["schema_version"] = 1.0;
    rejects([&] { (void)RoomRelationshipSnapshot::from_json(malformed); });
    malformed = snapshot.to_json(); malformed["references"][0]["id"] = 7;
    rejects([&] { (void)RoomRelationshipSnapshot::from_json(malformed); });
    malformed = snapshot.to_json(); malformed["relations"][0]["kind"] = "implied";
    rejects([&] { (void)RoomRelationshipSnapshot::from_json(malformed); });
    malformed = snapshot.to_json(); malformed["extra"] = true;
    rejects([&] { (void)RoomRelationshipSnapshot::from_json(malformed); });
    require(RoomRelationshipSnapshot::create({}, {}).to_json().at("references").empty(), "Empty graph invalid");
}
}
int main() {
    try { run(); std::cout << "Room relationship tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
