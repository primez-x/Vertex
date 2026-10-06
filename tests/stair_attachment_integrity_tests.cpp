#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/stair_semantics.hpp"
#include "sketch/vertical_levels.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Map = std::map<std::string, Entity, std::less<>>;
using Json = nlohmann::json;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void reject(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid stair attachment/identity admitted");
}
Entity entity(std::string id, std::string type, Json p = Json::object()) {
    return {std::move(id), std::move(type), std::move(p)};
}
StairFlight stair(std::string id = "stair") {
    StairFlight s{std::move(id), {}, 0, 8, 1.6, 0.3, 1};
    s.flights = {{"lower",4},{"upper",4}};
    s.landings = {{"turn",1.2,0.15,StairTurn::left_quarter,0}};
    return s;
}
void context(Entity& e, std::string layer = "layer") {
    e.properties.update(Json{{"property_id","property"},{"building_id","building"},
        {"floor_id","floor"},{"layer_id",std::move(layer)}});
}
Map fixture() {
    Map m;
    for (auto e : {entity("property","property"),
                  entity("building","building",{{"property_id","property"}}),
                  entity("floor","floor",{{"building_id","building"}}),
                  entity("layer","layer",{{"floor_id","floor"}}),
                  entity("other-layer","layer",{{"floor_id","floor"}})})
        m.emplace(e.id,e);
    auto s = entity("stair","stair",encode_stair_properties(stair())); context(s);
    m.emplace(s.id,s);
    Railing r{"rail",{},0,0,0.9,0.04,0.45,
        StairRailingHost{"stair","lower",StairRailingSide::left,0,1}};
    auto rail = entity("rail","railing",encode_railing_properties(r)); context(rail);
    m.emplace(rail.id,rail);
    return m;
}
void attachment_failures() {
    const auto original=fixture(); validate_stair_attachment_state(original);
    require(original==fixture(),"pure state validator changed source");
    auto m=original; m.at("stair").properties["visible"]=false; validate_stair_attachment_state(m);
    m=original; context(m.at("rail"),"other-layer"); validate_stair_attachment_state(m);
    m=original; m.erase("stair"); reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("stair").type="roof"; reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("rail").properties["host"]["flight_id"]="turn"; reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("rail").properties["host"]["flight_id"]="retired"; reject([&]{validate_stair_attachment_state(m);});
    for (const char* key : {"property_id","building_id","floor_id","layer_id"}) {
        m=original; m.at("rail").properties[key]="missing"; reject([&]{validate_stair_attachment_state(m);});
        m=original; m.at("rail").properties.erase(key); reject([&]{validate_stair_attachment_state(m);});
    }
    m=original;
    m["other-floor"]=entity("other-floor","floor",{{"building_id","building"}});
    m["floor-layer"]=entity("floor-layer","layer",{{"floor_id","other-floor"}});
    m.at("rail").properties["floor_id"]="other-floor";
    m.at("rail").properties["layer_id"]="floor-layer";
    reject([&]{validate_stair_attachment_state(m);});
    m=original;
    m["other-property"]=entity("other-property","property");
    m["other-building"]=entity("other-building","building",{{"property_id","other-property"}});
    m["other-floor"]=entity("other-floor","floor",{{"building_id","other-building"}});
    m["floor-layer"]=entity("floor-layer","layer",{{"floor_id","other-floor"}});
    m.at("rail").properties.update(Json{{"property_id","other-property"},{"building_id","other-building"},
        {"floor_id","other-floor"},{"layer_id","floor-layer"}});
    reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("rail").properties["vertical_placement"]={{"version",1},{"mode","absolute"},{"offset_m",0}};
    reject([&]{validate_stair_attachment_state(m);});
    for(const char* key : {"base_position_m","orientation_rad","length_m"}) {
        m=original; m.at("rail").properties[key]=0; reject([&]{validate_stair_attachment_state(m);});
    }
    m=original; m.at("rail").properties["host"]["start_fraction"]=0.24;
    reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("rail").properties["post_spacing_m"]=0.01;
    reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("rail").properties["host"]["side"]="center";
    reject([&]{validate_stair_attachment_state(m);});
    m=original; m.at("stair").properties["flights"][0]["id"]="rail";
    reject([&]{validate_stair_attachment_state(m);});
}
void compatibility() {
    Map m{{"legacy",entity("legacy","stair",{{"width",1}})}};
    validate_stair_attachment_state(m);
    m.at("legacy").properties["landings"]="legacy descriptor, no canonical authority";
    validate_stair_attachment_state(m);
    m.at("legacy").properties={{"form","future_stair"},{"version",3},{"flights","opaque"}};
    validate_stair_attachment_state(m);
    StairFlight old{"legacy",{},0,4,0.8,0.3,1};
    m.at("legacy").properties=encode_stair_properties(old); validate_stair_attachment_state(m);
    m.at("legacy").properties["flights"]=Json::array(); reject([&]{validate_stair_attachment_state(m);});
    m=fixture(); m.erase("rail"); m.at("stair").properties.erase("version");
    reject([&]{validate_stair_attachment_state(m);});
    m=fixture(); m.at("rail").properties["version"]=1; reject([&]{validate_stair_attachment_state(m);});
    m=fixture(); m.at("stair").properties=encode_stair_properties(old); context(m.at("stair"));
    m.at("rail").properties["host"]["flight_id"]="stair";
    reject([&]{validate_stair_attachment_state(m);});
    m=fixture(); m.at("stair").properties["flights"]=Json::array();
    for (int i=0;i<257;++i) m.at("stair").properties["flights"].push_back({{"id","f"},{"riser_count",1}});
    reject([&]{validate_stair_attachment_state(m);});
}
void phases() {
    auto m=fixture();
    const auto add=[&](ModelPhases p){m["phases"]=entity("phases","model_phases",{{"model",p.to_json()}});};
    add(ModelPhases::create({"stair","rail"},{"stair","rail"},{{"a","A",{},{}},{"b","B",{},{} }}));
    validate_stair_attachment_state(m);
    add(ModelPhases::create({"stair","rail"},{"stair","rail"},{{"a","A",{"stair"},{} }}));
    reject([&]{validate_stair_attachment_state(m);});
    add(ModelPhases::create({"stair","rail"},{"stair","rail"},{{"a","A",{"stair","rail"},{} }}));
    validate_stair_attachment_state(m);
    add(ModelPhases::create({"stair","rail"},{"stair"},{{"a","A",{}, {"rail"} }}));
    validate_stair_attachment_state(m);
    add(ModelPhases::create({"stair","rail"},{"rail"},{{"a","A",{}, {"stair"} }}));
    reject([&]{validate_stair_attachment_state(m);});
    add(ModelPhases::create({"stair","rail"},{},{{"a","A",{}, {"stair"}},{"b","B",{}, {"rail"}} }));
    reject([&]{validate_stair_attachment_state(m);});
    add(ModelPhases::create({"stair","rail"},{},{{"a","A",{}, {"stair","rail"}} }));
    validate_stair_attachment_state(m);
    m["second-phases"]=entity("second-phases","model_phases",m.at("phases").properties);
    reject([&]{validate_stair_attachment_state(m);}); m.erase("second-phases");
    m.at("rail").properties["phase_id"]="phase-a"; reject([&]{validate_stair_attachment_state(m);});
    m.at("stair").properties["phase_id"]="phase-a"; validate_stair_attachment_state(m);
    m.at("stair").properties["phase_id"]="phase-b"; reject([&]{validate_stair_attachment_state(m);});
    m=fixture(); add(ModelPhases::create({"rail"},{"rail"},{}));
    reject([&]{validate_stair_attachment_state(m);});
}
void levels() {
    auto m=fixture();
    VerticalLevelGraph graph({{"low",10},{"high",11.6}},{{"link","low","high"}});
    m["levels"]=entity("levels","vertical_levels",{{"model",Json::parse(graph.serialize())}});
    m.at("floor").properties["vertical_level_binding"]=VerticalLevelBinding{"levels","low"}.to_json();
    m.at("stair").properties["level_connection"]={{"version",1},{"graph_id","levels"},
        {"link_id","link"},{"lower_level_id","low"},{"upper_level_id","high"}};
    m.at("stair").properties["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",0}};
    validate_stair_attachment_state(m);
    m.at("floor").properties["vertical_level_binding"]=VerticalLevelBinding{"levels","high"}.to_json();
    reject([&]{validate_stair_attachment_state(m);});
}
RevisionRecord record(const Map& m) { RevisionRecord r; r.entities=m; return r; }
void identities() {
    auto before=fixture(); before.erase("rail"); auto after=before;
    std::vector<RevisionRecord> history{record(before)};
    std::swap(after.at("stair").properties["flights"][0],after.at("stair").properties["flights"][1]);
    validate_stair_identity_transition(before,after,history);
    after=before; after.at("stair").properties["flights"][0]["id"]="fresh";
    validate_stair_identity_transition(before,after,history);
    auto retired=after; history.push_back(record(retired));
    reject([&]{validate_stair_identity_transition(retired,before,history);});
    after=before; after.at("stair").properties["landings"][0]["id"]="lower";
    after.at("stair").properties["flights"][0]["id"]="turn";
    reject([&]{validate_stair_identity_transition(before,after,history);});
    auto clone=entity("clone","stair",encode_stair_properties(stair("clone")));
    after=before; after.emplace(clone.id,clone); reject([&]{validate_stair_identity_transition(before,after,history);});
    clone.properties["flights"][0]["id"]="new-lower";
    clone.properties["flights"][1]["id"]="new-upper";
    clone.properties["landings"][0]["id"]="new-turn";
    after=before; after.emplace(clone.id,clone); validate_stair_identity_transition(before,after,history);
    auto branch=before; branch.at("stair").properties["flights"][0]["id"]="branch-child";
    history.push_back(record(branch)); after=before;
    after.at("stair").properties["flights"][0]["id"]="branch-child";
    reject([&]{validate_stair_identity_transition(before,after,history);});
    branch=before; branch["retired-entity"]=entity("retired-entity","metadata"); history.push_back(record(branch));
    after=before; after.at("stair").properties["flights"][0]["id"]="retired-entity";
    reject([&]{validate_stair_identity_transition(before,after,history);});
    after=retired; after["lower"]=entity("lower","metadata");
    reject([&]{validate_stair_identity_transition(retired,after,history);});
    auto empty=before; empty.erase("stair"); reject([&]{validate_stair_identity_transition(empty,before,history);});
    branch=before; branch.at("stair").id="wrong-map-key"; history.push_back(record(branch));
    reject([&]{validate_stair_identity_transition(before,before,history);});
}
void unrelated_history_and_identity_only_work() {
    // Large opaque payloads and unrelated phase data acquire no new stair
    // admission limits. Their ordinary validation remains Document's contract.
    Map legacy;
    legacy["metadata"]=entity("metadata","metadata",{{"payload",Json::array()}});
    legacy.at("metadata").properties["payload"]=Json::array();
    for(int i=0;i<100001;++i) legacy.at("metadata").properties["payload"].push_back(i);
    legacy["phases"]=entity("phases","model_phases",{{"model","not stair authority"}});
    legacy["old-stair"]=entity("old-stair","stair",{{"description","legacy stair"}});
    validate_stair_attachment_state(legacy);
    std::vector<RevisionRecord> history{record(legacy)};
    validate_stair_identity_transition(legacy,legacy,history);
    // v1 has no retained child identities, so unrelated phase payloads are not
    // inspected by identity transition or the standalone v1 state check.
    StairFlight old{"old-stair",{},0,4,0.8,0.3,1};
    legacy.at("old-stair").properties=encode_stair_properties(old);
    validate_stair_attachment_state(legacy);
    validate_stair_identity_transition(legacy,legacy,history);
    // A history longer than the old helper's 100,000-record limit is valid when
    // there is no child namespace. Empty records keep this fixture inexpensive.
    std::vector<RevisionRecord> long_history(100001);
    validate_stair_identity_transition(legacy,legacy,long_history);

    auto current=fixture(); current.erase("rail");
    auto malformed_geometry=current;
    malformed_geometry.at("stair").properties["width_m"]=0;
    // Identity remains geometry-neutral; root must separately admit every map.
    validate_stair_identity_transition(current,malformed_geometry,{});
    reject([&]{validate_stair_attachment_state(malformed_geometry);});
    history={record(current)};
    history.front().entities.at("stair").properties["flights"][0]["riser_count"]="bad";
    reject([&]{validate_stair_identity_transition(current,current,history);});
    history={record(current)};
    history.front().entities.at("stair").properties["landings"][0]["id"]=Json::array();
    reject([&]{validate_stair_identity_transition(current,current,history);});
}
void incremental_ledger() {
    auto original=fixture(); original.erase("rail");
    auto branch=original; branch.at("stair").properties["flights"][0]["id"]="branch-flight";
    StairIdentityHistory ledger; ledger.reserve_state(original);
    require(ledger.initialized(),"initial v2 state activates explicit ledger");
    ledger.validate_transition(original,branch); ledger.reserve_state(branch);
    // Exact Undo/Redo/navigation reservation can reintroduce historical IDs.
    ledger.reserve_state(original);
    reject([&]{ledger.validate_transition(original,branch);});
    std::vector<RevisionRecord> history{record(original),record(branch),record(original)};
    reject([&]{validate_stair_identity_transition(original,branch,history);});
    auto fresh=original; fresh.at("stair").properties["flights"][0]["id"]="fresh-branch-flight";
    ledger.validate_transition(original,fresh);
    validate_stair_identity_transition(original,fresh,history);
    ledger.reserve_state(fresh);
    ledger.reserve_state(branch);
    reject([&]{ledger.validate_transition(branch,original);});
    // Reservation rejects a future typed owner/role conflict before mutation.
    auto bad=original;
    std::swap(bad.at("stair").properties["flights"][0]["id"],bad.at("stair").properties["landings"][0]["id"]);
    bad["unreserved-id"]=entity("unreserved-id","metadata");
    reject([&]{ledger.reserve_state(bad);});
    fresh=branch; fresh.at("stair").properties["flights"][0]["id"]="unreserved-id";
    ledger.validate_transition(branch,fresh);

    // No-op paths never inspect a supplied prefix: these records are hostile
    // evidence that would throw immediately if initialize scanned them.
    auto hostile=record(original); hostile.entities.at("stair").properties["flights"]=false;
    std::vector<RevisionRecord> hostile_prefix{hostile};
    ledger.initialize_if_needed(hostile_prefix,original,original);
    StairIdentityHistory lazy;
    Map metadata{{"old-entity",entity("old-entity","metadata")}};
    lazy.initialize_if_needed(hostile_prefix,metadata,metadata);
    require(!lazy.initialized(),"no-v2 initialization must not scan the prefix");
    lazy.reserve_state(metadata);
    require(!lazy.initialized(),"no-v2 reservation retains lazy ledger");

    StairFlight v1{"old-v1-stair",{},0,4,0.8,0.3,1};
    auto legacy=metadata;
    legacy.emplace("old-v1-stair",entity("old-v1-stair","stair",encode_stair_properties(v1)));
    history={record(legacy),record(Map{})};
    auto first=original;
    lazy.initialize_if_needed(history,Map{},first);
    require(lazy.initialized(),"first v2 reconstructs prior IDs once");
    auto old_entity_child=first;
    old_entity_child.at("stair").properties["flights"][0]["id"]="old-entity";
    reject([&]{lazy.validate_transition(Map{},old_entity_child);});
    auto old_owner=first;
    auto owner=old_owner.extract("stair"); owner.key()="old-v1-stair"; owner.mapped().id="old-v1-stair";
    old_owner.insert(std::move(owner));
    reject([&]{lazy.validate_transition(Map{},old_owner);});
    lazy.validate_transition(Map{},first); lazy.reserve_state(first);
    lazy.reserve_state(Map{});
    Map after_retirement{{"later-entity",entity("later-entity","metadata")}};
    lazy.reserve_state(after_retirement);
    auto reappearance=first; reappearance.at("stair").properties["flights"][0]["id"]="later-entity";
    reject([&]{lazy.validate_transition(after_retirement,reappearance);});
    // New stair with fresh children still cannot resurrect a retired owner.
    reappearance=first;
    reappearance.at("stair").properties["flights"][0]["id"]="new-lower";
    reappearance.at("stair").properties["flights"][1]["id"]="new-upper";
    reappearance.at("stair").properties["landings"][0]["id"]="new-turn";
    reject([&]{lazy.validate_transition(after_retirement,reappearance);});
    lazy.reserve_state(first); // Exact Undo remains admissible.
    auto rebuilt=StairIdentityHistory::from_retained_history(history);
    require(!rebuilt.initialized(),"full v1-only history stays lazy");
    history.push_back(record(first)); history.push_back(record(after_retirement));
    rebuilt=StairIdentityHistory::from_retained_history(history);
    reject([&]{rebuilt.validate_transition(after_retirement,reappearance);});
    reject([&]{validate_stair_identity_transition(after_retirement,reappearance,history);});
    StairIdentityHistory uninitialized;
    reject([&]{uninitialized.validate_transition(Map{},first);});
}
void retired_owner_opaque_bridge() {
    auto original=fixture(); original.erase("rail");
    auto retired=original; retired.erase("stair");
    auto bridge=retired;
    bridge.emplace("stair",entity("stair","stair",{{"description","legacy descriptor"}}));
    auto fresh=original;
    fresh.at("stair").properties["flights"][0]["id"]="bridge-fresh-lower";
    fresh.at("stair").properties["flights"][1]["id"]="bridge-fresh-upper";
    fresh.at("stair").properties["landings"][0]["id"]="bridge-fresh-turn";
    StairFlight straight{"stair",{},0,8,1.6,0.3,1};
    auto known_v1=original;
    known_v1.at("stair").properties=encode_stair_properties(straight); context(known_v1.at("stair"));
    StairIdentityHistory ledger; ledger.reserve_state(original); ledger.reserve_state(retired);
    ledger.validate_transition(retired,bridge); ledger.reserve_state(bridge);
    std::vector<RevisionRecord> history{record(original),record(retired),record(bridge)};
    reject([&]{ledger.validate_transition(bridge,fresh);});
    reject([&]{ledger.validate_transition(bridge,known_v1);});
    reject([&]{validate_stair_identity_transition(bridge,fresh,history);});
    reject([&]{validate_stair_identity_transition(bridge,known_v1,history);});
    auto future=bridge;
    future.at("stair").properties={{"form","multi_flight_stair"},{"version",3}};
    ledger.reserve_state(future);
    reject([&]{ledger.validate_transition(future,fresh);});
    history.push_back(record(future));
    reject([&]{validate_stair_identity_transition(future,fresh,history);});

    // A live canonical owner may simplify v2 to v1 and later upgrade with fresh
    // children; exact navigation reservation remains independent of authoring.
    ledger.reserve_state(original);
    ledger.validate_transition(original,known_v1); ledger.reserve_state(known_v1);
    validate_stair_attachment_state(known_v1);
    history={record(original),record(known_v1)};
    ledger.validate_transition(known_v1,fresh);
    validate_stair_identity_transition(known_v1,fresh,history);
    ledger.reserve_state(fresh);
    auto reordered=fresh;
    std::swap(reordered.at("stair").properties["flights"][0],reordered.at("stair").properties["flights"][1]);
    ledger.validate_transition(fresh,reordered); ledger.reserve_state(reordered);
    ledger.reserve_state(original); ledger.reserve_state(known_v1); ledger.reserve_state(fresh);
}
void landing_admission_and_future_forms() {
    auto original=fixture();
    Railing rail{"rail",{},0,0,.9,.04,.3};
    rail.landing_host=StairLandingRailingHost{"stair",StairLandingRole::connecting,"turn","lower","upper",0,0,1};
    original.at("rail").properties=encode_railing_properties(rail);context(original.at("rail"));
    validate_stair_attachment_state(original);
    for(const char* key:{"incoming_flight_id","outgoing_flight_id","landing_id"}) {
        auto m=original;m.at("rail").properties["host"][key]="missing";
        const auto before=m;reject([&]{validate_stair_attachment_state(m);});require(m==before,"landing refusal mutated original map");
    }
    auto m=original;std::swap(m.at("stair").properties["flights"][0],m.at("stair").properties["flights"][1]);
    reject([&]{validate_stair_attachment_state(m);});
    // An explicit atomic rehost is admitted against the new ordered topology.
    m.at("rail").properties["host"]["incoming_flight_id"]="upper";
    m.at("rail").properties["host"]["outgoing_flight_id"]="lower";validate_stair_attachment_state(m);
    m=original;m.at("rail").properties["host"]["edge_index"]=3;reject([&]{validate_stair_attachment_state(m);});
    m=original;m.at("rail").properties["host"]["edge_index"]=2;reject([&]{validate_stair_attachment_state(m);});
    m=original;m.at("rail").properties["version"]=2;reject([&]{validate_stair_attachment_state(m);});
    m=original;m.at("rail").properties["host"].erase("role");reject([&]{validate_stair_attachment_state(m);});
    m=original;m.at("rail").properties["version"]=4;m.at("rail").properties["host"]="opaque future";
    m.erase("stair");validate_stair_attachment_state(m);
    for(const char* form:{"stair_flight_railing","straight_railing","unknown_railing"}) {
        m=original;m.at("rail").properties={{"form",form},{"version",3},{"host","opaque"}};
        m.erase("stair");validate_stair_attachment_state(m);
    }
    m=original;m.at("stair").properties["version"]=3;reject([&]{validate_stair_attachment_state(m);});
    m.erase("rail");m.at("stair").properties["flights"]="opaque v3";validate_stair_attachment_state(m);
    m=original;m.at("stair").properties["base_position_m"]={3,-2,7};m.at("stair").properties["orientation_rad"]=.4;
    m.at("stair").properties["going_m"]=.45;m.at("stair").properties["width_m"]=1.1;
    validate_stair_attachment_state(m);
}
} // namespace
int main() {
    try { attachment_failures(); compatibility(); phases(); levels(); identities(); unrelated_history_and_identity_only_work(); incremental_ledger(); retired_owner_opaque_bridge(); landing_admission_and_future_forms(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    std::cout<<"stair attachment and identity checks passed\n"; return 0;
}
