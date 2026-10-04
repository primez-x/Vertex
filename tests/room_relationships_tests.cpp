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
    const nlohmann::json chain={{"schema_version",2},{"references",nlohmann::json::array({
        {{"id","room"},{"kind","room_boundary"}},
        {{"id","wall-a"},{"kind","architectural_wall"},{"wall_members",{"wall-a","wall-c"}}}})},{"relations",nlohmann::json::array({
        {{"source_id","room"},{"target_id","wall-a"},{"kind","follows"}}})}};
    require(RoomRelationshipSnapshot::from_json(chain).to_json()==chain,
        "Explicit whole-wall chain semantics must roundtrip as schema two");
    const auto chain_model=RoomRelationshipSnapshot::from_json(chain);
    require(chain_model.schema_version()==2 && room_reference_wall_ids(chain_model.references().back())==
        std::vector<std::string>{"wall-a","wall-c"},"wall members lost native correspondence");
    auto sticky=RoomRelationshipSnapshot::create(refs(),{}).to_json();sticky["schema_version"]=2;
    const auto sticky_model=RoomRelationshipSnapshot::from_json(sticky);
    require(sticky_model.schema_version()==2 && sticky_model.to_json()==sticky,"schema two ordinary references must not downgrade");
    auto sticky_retarget=RoomRelationshipSnapshot::create(refs(),{{"room","wall-a",R::follows}}).to_json();
    sticky_retarget["schema_version"]=2;
    require(RoomRelationshipSnapshot::from_json(sticky_retarget).retarget({"room","wall-a","wall-b",R::follows}).schema_version()==2,
        "retarget downgraded sticky schema two");
    for(int corruption=0;corruption<6;++corruption){
        auto bad=chain;
        if(corruption==0)bad["references"][1]["wall_members"]={"wall-c","wall-a"};
        if(corruption==1)bad["references"][1]["wall_members"]={"wall-a","wall-a"};
        if(corruption==2)bad["references"][1]["wall_members"]={"wall-a"};
        if(corruption==3)bad["references"][0]["wall_members"]={"room","other"};
        if(corruption==4)bad["references"].push_back({{"id","wall-c"},{"kind","architectural_wall"}});
        if(corruption==5)bad["schema_version"]=1;
        rejects([&]{(void)RoomRelationshipSnapshot::from_json(bad);});
    }
    auto future=chain;future["schema_version"]=3;
    require(room_relationship_model_version(future)==3,"future positive schema inspection must remain opaque");
    rejects([&]{(void)RoomRelationshipSnapshot::from_json(future);});
    for(const auto version : {nlohmann::json(0),nlohmann::json(-1),nlohmann::json(2.0),nlohmann::json("2")}) {
        auto invalid_version=chain;invalid_version["schema_version"]=version;
        rejects([&]{(void)room_relationship_model_version(invalid_version);});
    }
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
    const auto retargeted = follows.retarget({"measure", "room", "wall-b", R::follows});
    require(retargeted.relations().size() == 2 &&
                std::any_of(retargeted.relations().begin(), retargeted.relations().end(),
                    [](const auto& relation) {
                        return relation.source_id == "measure" &&
                               relation.target_id == "wall-b" &&
                               relation.kind == R::follows;
                    }),
            "Valid retarget should replace only the declared dependency target");
    require(follows.relations().size() == 2 && follows.relations()[0].target_id != "wall-b",
            "Retarget must not mutate the original relationship snapshot");
    const auto independent = RoomRelationshipSnapshot::create(refs(), {{"measure", "room", R::independent}});
    const auto independent_retargeted = independent.retarget(
        {"room", "measure", "wall-a", R::independent});
    require(independent_retargeted.relations().size() == 1 &&
                independent_retargeted.relations().front().source_id == "room" &&
                independent_retargeted.relations().front().target_id == "wall-a",
            "Independent retarget should preserve the selected endpoint");
    rejects([&] { (void)follows.retarget({"measure", "room", "missing", R::follows}); });
    rejects([&] { (void)follows.retarget({"measure", "wall-a", "wall-b", R::follows}); });
    rejects([&] {
        const auto cyclic = RoomRelationshipSnapshot::create(
            {{"a", K::room_boundary}, {"b", K::room_boundary}, {"c", K::room_boundary}},
            {{"a", "b", R::follows}, {"b", "c", R::follows}});
        (void)cyclic.retarget({"b", "c", "a", R::follows});
    });
    rejects([&] { (void)follows.retarget({"measure", "room", "room", R::follows}); });
    rejects([&] { (void)follows.retarget({"measure", "room", "wall-a", R::derived_from}); });
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
