#include "sketch/wall_split.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void expect_near(double a,double b,const char* message){require(std::isfinite(a)&&std::abs(a-b)<1e-7,message);}
template<class F>void rejects(F operation,const char* message){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}require(rejected,message);}
Json geometry(Segment b){return {{"start",{b.start.x,b.start.y}},{"end",{b.end.x,b.end.y}},{"sweep_radians",b.sweep_radians}};}
Segment baseline(const Entity& e){const auto& b=e.properties.at("baseline");return {{b["start"][0],b["start"][1]},{b["end"][0],b["end"][1]},b["sweep_radians"]};}
Entity wall(std::string id,Segment b){return {id,"wall",{{"baseline",geometry(b)},{"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"classification","partition"}},false,{{"vendor",{{"retain",17}}}}};}
Entity opening(std::string id,double offset,double width){return {id,"opening",{{"wall_id","wall"},{"offset_m",offset},{"width_m",width},{"sill_m",0.5},{"height_m",1.0}}};}
WallSplitIntent intent(double fraction=0.4){return {"wall","second",fraction,"seam",{}};}

void straight_split_hosts_slope_codec_and_history() {
    auto original=wall("wall",{{0,0},{10,0},0});original.properties["slope_rise_m"]=2.0;
    const auto q=parse_quantity("10 m");
    original.extensions["constraint_authoring"]={{"version",1},{"last_length_entry",{{"version",1},
        {"original_expression",q.original_expression},{"entered_unit","m"},{"exact_metres",{{"numerator",q.exact_metres.numerator},
        {"denominator",q.exact_metres.denominator}}},{"baseline",original.properties["baseline"]}}}};
    auto document=Document::create({original,opening("before",1,1),opening("after",6,1)});
    const auto source=document.snapshot();const auto command=make_wall_split_command(source,intent());
    const auto codec=command_to_json(command);require(codec["version"]==12,"split requires envelope twelve");
    require(command_to_json(command_from_json(codec))==codec,"split codec is exact");
    auto raw=codec;raw["entity_changes"]=Json::array();rejects([&]{(void)command_from_json(raw);},"raw lanes cannot borrow split authority");
    document.apply(command);const auto split=document.snapshot();
    const auto& first=split.entities().at("wall");const auto& second=split.entities().at("second");
    expect_near(baseline(first).end.x,4,"first span");expect_near(baseline(second).start.x,4,"second span");
    expect_near(first.properties["slope_rise_m"],0.8,"first slope");expect_near(second.properties["slope_rise_m"],1.2,"second slope");
    expect_near(second.properties["height_m"],3.8,"second slope starts at old seam height");
    require(first.extensions.at("vendor")==original.extensions.at("vendor") && second.extensions.at("vendor")==original.extensions.at("vendor"),"opaque wall data survives");
    require(first.extensions["wall_split_archive"]["pieces"][0]["length_entry"]==original.extensions["constraint_authoring"]["last_length_entry"],"exact full length input is archived verbatim");
    require(!first.extensions["constraint_authoring"].contains("last_length_entry"),"whole receipt is inactive on shortened piece");
    require(split.entities().at("before")==source.entities().at("before"),"before seam host unchanged");
    require(split.entities().at("after").properties["wall_id"]=="second","after seam host remapped");
    expect_near(split.entities().at("after").properties["offset_m"],2,"after seam station rebased");
    auto reopened=Document::fork(split);require(reopened.snapshot().entities()==split.entities(),"retained split history replays exactly");
    require(document.undo(document.revision()) && document.snapshot().entities()==source.entities(),"Undo restores exact source");
    require(document.redo(document.revision()) && document.snapshot().entities()==split.entities(),"Redo restores exact split");
    rejects([&]{document.apply(command);},"stale split command rejects");
    auto bad=first;bad.extensions.erase("wall_split_archive");rejects([&]{document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(bad)}, {},"erase archive"});},"ordinary edits retain archive");
}

void signed_arc_and_whole_endpoint_relations() {
    for(const auto sweep:{1.6,-1.6}) {
        auto original=wall("wall",{{0,0},{10,0},sweep});
        PersistentConstraint total{"total",ConstraintRelationKind::fixed_arc_length,{{"wall",WallEndpointRole::start},{"wall",WallEndpointRole::end}}};
        std::ostringstream expression;expression<<std::setprecision(17)<<segment_length(baseline(original))<<" m";total.length=parse_quantity(expression.str());
        original.extensions["curve_input"]={{"version",2},{"construction","arc_length"},
            {"start",{0,0}},{"end",{10,0}},{"radians",sweep},{"measure",total.length->original_expression},
            {"measure_value",total.length->metres},{"clockwise",sweep<0},{"vendor_input","preserved"}};
        auto lock=encode_constraint_entity(total);lock.properties["bindings"][1]["vendor_endpoint"]="retained";
        PersistentConstraint chord{"chord",ConstraintRelationKind::fixed_length,{{"wall",WallEndpointRole::start},{"wall",WallEndpointRole::end}},parse_quantity("10 m")};
        auto document=Document::create({original,lock,encode_constraint_entity(chord)});
        document.apply(make_wall_split_command(document.snapshot(),intent(0.3)));
        const auto snapshot=document.snapshot();const auto& entities=snapshot.entities();
        const auto a=baseline(entities.at("wall")),b=baseline(entities.at("second"));
        expect_near(a.sweep_radians,sweep*0.3,"directed first sweep");expect_near(b.sweep_radians,sweep*0.7,"directed second sweep");
        expect_near(segment_length(a)+segment_length(b),segment_length(baseline(original)),"signed arc children reconstruct physical measure");
        for(const auto* id:{"wall","second"}) {
            const auto& piece=entities.at(id);validate_wall_curve_input(piece);validate_wall_split_archive(piece);
            require(piece.extensions["curve_input_derivation"]["source_input"]==original.extensions["curve_input"],"each child archives exact full-span curve construction");
        }
        const auto chain=decode_constraint_entity(entities.at("total"));
        require(chain.supported()&&chain.version==4&&chain.constraint->bindings.size()==4,"whole arc lock becomes one total chain");
        require(chain.constraint->length->original_expression==total.length->original_expression,"exact original total remains unchanged");
        require(entities.at("total").properties["bindings"][3]["vendor_endpoint"]=="retained","remapped opaque endpoint metadata survives");
        require(!entities.at("total").properties["bindings"][1].contains("vendor_endpoint"),"seam does not inherit metadata belonging to original endpoint");
        require(decode_constraint_entity(entities.at("chord")).constraint->bindings[1].owner_id=="second","whole chord endpoints preserve old end");
        auto recursive=intent(0.5);recursive.second_wall_id="third";recursive.seam_constraint_id="seam2";
        document.apply(make_wall_split_command(document.snapshot(),recursive));
        require(decode_constraint_entity(document.snapshot().entities().at("total")).constraint->bindings.size()==6,"recursive arc splits extend the same total chain");
    }
}

void measured_outline_retains_children_and_dimension_meaning(bool curved=false) {
    std::vector<Entity> entities{{"property","property"},{"building","building",{{"property_id","property"}}},
        {"floor","floor",{{"building_id","building"}}},{"layer","layer",{{"floor_id","floor"}}}};
    const std::vector<std::string> ids{"wall","right","top","left"};
    const std::vector<Segment> pieces=curved ? std::vector<Segment>{{{10,0},{10,6},std::numbers::pi},
        {{10,6},{0,6},0},{{0,6},{0,0},std::numbers::pi},{{0,0},{10,0},0}} :
        std::vector<Segment>{{{0,0},{10,0},0},{{10,0},{10,6},0},{{10,6},{0,6},0},{{0,6},{0,0},0}};
    for(std::size_t i=0;i<ids.size();++i){auto e=wall(ids[i],pieces[i]);e.properties["property_id"]="property";e.properties["building_id"]="building";e.properties["floor_id"]="floor";e.properties["layer_id"]="layer";entities.push_back(e);}
    auto document=Document::create(entities);const auto derived=derive_exterior_wall_measurement(document.snapshot(),ids);
    auto boundary=Json::array();for(const auto& b:derived.boundary)boundary.push_back(geometry(b));
    Entity owner{"area","measurement_boundary",{{"boundary",boundary},{"wall_measurement_source",derived.source},
        {"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"},{"classification","gla"}}};
    owner=upgrade_legacy_boundary_entity(owner);const auto identified=decode_identified_boundary_entity(owner);
    const auto index=static_cast<std::size_t>(std::find(derived.ordered_wall_ids.begin(),derived.ordered_wall_ids.end(),"wall")-derived.ordered_wall_ids.begin());
    const auto target=identified.segments.at(index).segment_id;
    BoundaryDimension manual{"manual","area",target,{4,-1}};
    BoundaryDimension automatic{"automatic","area",target,{5,-0.5},BoundaryDimensionPlacement::automatic,2};
    automatic.presentation=BoundaryDimensionPresentation{3.25,"#123456",true};
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(owner),EntityChange::upsert(encode_boundary_dimension_entity(manual)),EntityChange::upsert(encode_boundary_dimension_entity(automatic))}, {},"prepare measurements"});
    const auto source=document.snapshot();auto split_intent=intent();split_intent.measured_owners={{"area","new-vertex","new-edge","new-dimension"}};
    document.apply(make_wall_split_command(source,split_intent));const auto after=document.snapshot();
    const auto updated=decode_identified_boundary_entity(after.entities().at("area"));
    require(updated.segments.size()==identified.segments.size()+1,"current measured outline gains one analytical edge");
    require(wall_measurement_source_current(after,after.entities().at("area")),"completed measured outline remains current");
    expect_near(std::abs(signed_area(boundary_geometry(updated))),std::abs(signed_area(boundary_geometry(identified))),"measured area invariant");
    const auto retained=decode_boundary_dimension_entity(after.entities().at("manual"));
    require(retained.dimension->text_position.x==4&&retained.dimension->segment_chain_ids.size()==2,"manual whole span retains placement and chain meaning");
    expect_near(retained.dimension->resolve(after.entities().at("area")).segment_length(),manual.resolve(owner).segment_length(),"manual whole span retains original length");
    require(decode_boundary_dimension_entity(after.entities().at("automatic")).dimension->presentation==automatic.presentation,"existing automatic style and identity survive");
    require(after.entities().contains("new-dimension"),"only needed second automatic dimension is added");
    require(Document::fork(after).snapshot().entities()==after.entities(),"measured split history replays exactly");
}

void rejection_and_reference_handlers() {
    auto document=Document::create({wall("wall",{{0,0},{10,0},0}),opening("crossing",3,2)});
    rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"straddling opening rejects atomically");
    for(const auto fraction:{0.0,1.0,-0.1})rejects([&]{(void)encode_wall_split(intent(fraction));},"fraction must be strict interior");
    auto unsupported=wall("wall",{{0,0},{10,0},0});unsupported.extensions["vendor"]["nested"]={{"wall_id","wall"}};
    document=Document::create({unsupported});rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"unhandled canonical nested reference refuses");
    auto relationship=RoomRelationshipSnapshot::create({{"wall",RoomReferenceKind::architectural_wall},
        {"zzz-room",RoomReferenceKind::room_boundary}},{{"wall","zzz-room",RoomRelationKind::independent}});
    Entity graph{"relationships","room_relationships",{{"model",relationship.to_json()}}};
    document=Document::create({wall("wall",{{0,0},{10,0},0}),graph,Entity{"zzz-room","room_boundary"}});
    rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"independent room graph source reference cannot silently shorten");
    relationship=RoomRelationshipSnapshot::create({{"wall",RoomReferenceKind::architectural_wall}},{});
    graph.properties["model"]=relationship.to_json();document=Document::create({wall("wall",{{0,0},{10,0},0}),graph});
    rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"schema-owned room reference without relation cannot silently shorten");
    for(const auto* key:{"refs","references"}){
        Entity referent{"referent","label",{{key,Json::array({"wall"})}}};
        document=Document::create({wall("wall",{{0,0},{10,0},0}),referent});
        rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"canonical generic entity references cannot silently shorten");
    }
    AssemblyType type{"assembly-type","Wall assembly",{},{},{}};
    AssemblyInstance instance{"placed","assembly-type",{},{},{},AssemblyPlacement{"wall",{0,0},0,1}};
    const auto assemblies=AssemblyModel::create({}, {type}, {instance});
    document=Document::create({wall("wall",{{0,0},{10,0},0}),Entity{"assemblies","assembly_model",{{"model",assemblies.to_json()}}}});
    rejects([&]{(void)make_wall_split_command(document.snapshot(),intent());},"hosted whole-wall assembly cannot silently shorten");
    auto join=Entity{"join","wall_join",{{"version",1},{"wall_ids",{"wall","other"}},{"style","fused"}}};
    const auto phases=ModelPhases::create({"other","wall"},{"other","wall"},{{"option","Option",{"wall"},{}}},"option");
    Entity phase{"phase","model_phases",{{"model",phases.to_json()}}};
    document=Document::create({wall("wall",{{0,0},{10,0},0}),wall("other",{{10,0},{10,4},0}),join,phase});
    document.apply(make_wall_split_command(document.snapshot(),intent()));
    const auto snapshot=document.snapshot();const auto& state=snapshot.entities();
    require(state.at("join").properties["wall_ids"].size()==3,"every containing join includes second piece");
    require(state.at("phase").properties["model"]["baseline_ids"].size()==3&&state.at("phase").properties["model"]["alternatives"][0]["demolished_ids"].size()==2,"all phase lists retain complete split object meaning");
}
}
int main(){sketch::testing::noninteractive_errors();try{straight_split_hosts_slope_codec_and_history();signed_arc_and_whole_endpoint_relations();measured_outline_retains_children_and_dimension_meaning();measured_outline_retains_children_and_dimension_meaning(true);rejection_and_reference_handlers();std::cout<<"wall_vertex_insertion_tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<"wall_vertex_insertion_tests: "<<error.what()<<'\n';return 1;}}
