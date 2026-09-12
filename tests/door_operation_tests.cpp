#include "sketch/door_operation.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"invalid door operation accepted");}
void near(double a,double b){require(std::abs(a-b)<1e-8,"unexpected door symbol geometry");}
}
int main(){
    using namespace sketch;
    try {
        const Segment wall{{0,0},{10,0},0};
        for(bool end:{false,true}) for(bool left:{false,true}) {
            DoorOperation operation{end,left,90};
            require(decode_door_operation(encode_door_operation(operation))==operation,"door operation round trip");
            const auto symbol=door_plan_symbol(wall,2,1,operation);
            near(symbol[0].start.x,end?3:2);near(symbol[0].start.y,0);
            near(symbol[0].end.x,end?3:2);near(symbol[0].end.y,left?1:-1);
            near(segment_length(symbol[0]),1);near(segment_length(symbol[1]),std::numbers::pi/2);
            const auto rotated=door_plan_symbol({{7,5},{7,15},0},2,1,operation);
            near(rotated[0].end.x,7-symbol[0].end.y);near(rotated[0].end.y,5+symbol[0].end.x);
        }
        const Segment curve{{0,0},{4,0},std::numbers::pi/2};
        const auto curved=door_plan_symbol(curve,0,segment_length(curve),{});
        near(segment_length(curved[0]),4);near(segment_length(curved[1]),2*std::numbers::pi);
        near(door_plan_symbol(wall,2,1,{false,true,180})[0].end.x,1);
        rejects([&]{(void)encode_door_operation({false,true,0});});
        rejects([&]{(void)door_plan_symbol(wall,9.5,1,{});});
        auto malformed=encode_door_operation({}); malformed["version"]=2;
        rejects([&]{(void)decode_door_operation(malformed);});
        auto opening=Entity::create("opening",{{"opening_kind","door"},{"mark","D1"},
            {"width_m",1},{"height_m",2},{"door_operation",encode_door_operation({true,false,120})}});
        opening.id="door";
        auto document=Document::create({opening});
        const auto projection=build_document_schedules(document.snapshot());
        const auto& cells=projection.snapshot.rows.front().cells;
        require(std::get<std::string>(cells.at("hinge").value)=="end" &&
            std::get<std::string>(cells.at("swing_side").value)=="right" &&
            std::get<double>(cells.at("swing_angle_degrees").value)==120 && !cells.at("hinge").editable,
            "schedule must expose stored handing with provenance");
        opening.properties["door_operation"]=malformed;
        rejects([&]{document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(opening)},{},"invalid handing"});});
        require(document.revision()==0,"invalid handing must preserve document revision");
        opening=document.snapshot().entities().at("door");
        opening.properties["opening_kind"]="window";
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(opening)},{},"change classification"});
        require(!build_document_schedules(document.snapshot()).snapshot.rows.front().cells.contains("hinge") &&
            document.snapshot().entities().at("door").properties.contains("door_operation"),
            "window conversion must retain dormant handing without showing it as a window operation");
        document.undo(document.revision());
        require(build_document_schedules(document.snapshot()).snapshot.rows.front().cells.contains("hinge"),
            "undo restores the door schedule operation");
        std::cout<<"door operation tests passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
