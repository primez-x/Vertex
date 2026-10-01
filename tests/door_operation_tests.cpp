#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include <cmath>
#include <iostream>
#include <limits>
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
        // A double door must decode as two independently hinged half leaves;
        // rejecting v2 or retaining the old single-leaf branch fails here.
        const auto double_operation=decode_door_operation({{"version",2},{"kind","double_hinged"},
            {"hinge","start"},{"side","left"},{"angle_degrees",90},{"slide_fraction",0}});
        const auto double_symbol=door_plan_symbol(wall,2,2,double_operation);
        require(double_symbol.size()==4,"double door must contain two leaves and two arcs");
        near(double_symbol[0].start.x,2);near(double_symbol[0].end.x,2);near(double_symbol[0].end.y,1);
        near(double_symbol[2].start.x,4);near(double_symbol[2].end.x,4);near(double_symbol[2].end.y,1);
        require(encode_door_operation({})==nlohmann::json({{"version",1},{"hinge","start"},
            {"side","left"},{"angle_degrees",90}}),"legacy hinged encoding changed");
        for(bool end:{false,true}) for(bool left:{false,true}) {
            const DoorOperation doubled{end,left,60,DoorOperationKind::double_hinged};
            require(decode_door_operation(encode_door_operation(doubled))==doubled,"double operation round trip");
            const auto symbol=door_plan_symbol(wall,2,2,doubled);
            for(std::size_t i=0;i<2;++i) {
                const bool jamb=i==0?end:!end;
                const auto& leaf=symbol[i*2];
                near(leaf.start.x,jamb?4:2);near(leaf.end.x,jamb?3.5:2.5);
                near(leaf.end.y,(left?1:-1)*std::sqrt(3.0)/2);
                near(segment_length(leaf),1);near(segment_length(symbol[i*2+1]),std::numbers::pi/3);
            }
            const auto rotated=door_plan_symbol({{7,5},{7,15},0},2,2,doubled);
            for(std::size_t i=0;i<symbol.size();++i) {
                near(rotated[i].end.x,7-symbol[i].end.y);near(rotated[i].end.y,5+symbol[i].end.x);
            }
            const DoorOperation sliding{end,left,60,DoorOperationKind::sliding,0.5};
            require(decode_door_operation(encode_door_operation(sliding))==sliding,"sliding operation round trip");
            const auto panels=door_plan_symbol(wall,2,2,sliding);
            require(panels.size()==2 && panels[0].sweep_radians==0 && panels[1].sweep_radians==0,
                "sliding door must have panels without swing arcs");
            near(segment_length(panels[0]),1);near(segment_length(panels[1]),1);
            near(panels[0].start.x,2.5);near(panels[0].end.x,3.5);
            near(panels[1].start.x,end?2:3);near(panels[0].start.y,left?0.02:-0.02);
            auto different_angle=sliding;different_angle.angle_degrees=170;
            const auto unchanged=door_plan_symbol(wall,2,2,different_angle);
            near(unchanged[0].end.x,panels[0].end.x);near(unchanged[0].end.y,panels[0].end.y);
            const auto turned=door_plan_symbol({{7,5},{7,15},0},2,2,sliding);
            for(std::size_t i=0;i<panels.size();++i) {
                near(turned[i].end.x,7-panels[i].end.y);near(turned[i].end.y,5+panels[i].end.x);
            }
        }
        auto extended=encode_door_operation(double_operation);
        for(const char* key:{"version","kind","hinge","side","angle_degrees","slide_fraction"}) {
            auto missing=extended;missing.erase(key);
            rejects([&]{(void)decode_door_operation(missing);});
        }
        auto unknown=extended;unknown["extra"]=true;
        rejects([&]{(void)decode_door_operation(unknown);});
        for(const nlohmann::json& invalid:{nlohmann::json("pivot"),nlohmann::json(1),nlohmann::json(nullptr)}) {
            auto bad=extended;bad["kind"]=invalid;rejects([&]{(void)decode_door_operation(bad);});
        }
        for(double fraction:{-0.1,1.1}) rejects([&]{(void)encode_door_operation({false,true,90,DoorOperationKind::sliding,fraction});});
        rejects([&]{(void)encode_door_operation({false,true,90,DoorOperationKind::double_hinged,0.1});});
        rejects([&]{(void)encode_door_operation({false,true,90,static_cast<DoorOperationKind>(99)});});
        rejects([&]{(void)encode_door_operation({false,true,90,DoorOperationKind::sliding,
            std::numeric_limits<double>::quiet_NaN()});});
        for(const char* key:{"hinge","side","angle_degrees","slide_fraction"}) {
            auto bad=extended;bad[key]=nlohmann::json::array();
            rejects([&]{(void)decode_door_operation(bad);});
        }
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
            {"width_m",1},{"height_m",2},{"door_operation",encode_door_operation({true,false,120})},
            {"opening_assembly", opening_assembly_json(default_opening_assembly(OpeningAssemblyKind::door))}});
        opening.id="door";
        auto document=Document::create({opening});
        const auto projection=build_document_schedules(document.snapshot());
        const auto& cells=projection.snapshot.rows.front().cells;
        require(std::get<std::string>(cells.at("hinge").value)=="end" &&
            std::get<std::string>(cells.at("swing_side").value)=="right" &&
            std::get<double>(cells.at("swing_angle_degrees").value)==120 &&
            std::get<std::string>(cells.at("assembly_kind").value)=="door" &&
            std::get<ScheduleQuantity>(cells.at("frame_width").value).value > 0.0 &&
            !cells.at("hinge").editable,
            "schedule must expose stored handing with provenance");
        opening.properties["door_operation"]=malformed;
        rejects([&]{document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(opening)},{},"invalid handing"});});
        require(document.revision()==0,"invalid handing must preserve document revision");
        auto variant=opening;variant.properties["door_operation"]=encode_door_operation(double_operation);
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(variant)},{},"double door"});
        require(decode_door_operation(document.snapshot().entities().at("door").properties.at("door_operation"))==double_operation,
            "document failed to retain versioned double operation");
        const auto double_schedule=build_document_schedules(document.snapshot());
        require(std::get<std::string>(double_schedule.snapshot.rows.front().cells.at("mechanism").value)=="Double hinged",
            "schedule identifies a double door rather than a generic hinged door");
        variant.properties["door_operation"]=encode_door_operation({true,false,90,DoorOperationKind::sliding,0.75});
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(variant)},{},"sliding door"});
        near(decode_door_operation(document.snapshot().entities().at("door").properties.at("door_operation")).slide_fraction,0.75);
        const auto sliding_schedule=build_document_schedules(document.snapshot());
        const auto& sliding_cells=sliding_schedule.snapshot.rows.front().cells;
        require(std::get<std::string>(sliding_cells.at("mechanism").value)=="Sliding" &&
            std::get<double>(sliding_cells.at("open_percent").value)==75.0 &&
            !sliding_cells.contains("swing_angle_degrees") && !sliding_cells.at("open_percent").editable &&
            sliding_cells.at("open_percent").sources==std::vector<ScheduleSourceRef>{{"door","door_operation"}},
            "sliding schedule exposes actual travel with source provenance and no fictional swing");
        document.undo(document.revision());
        require(decode_door_operation(document.snapshot().entities().at("door").properties.at("door_operation"))==double_operation,
            "undo failed to restore double operation");
        document.undo(document.revision());
        opening=document.snapshot().entities().at("door");
        opening.properties["opening_kind"]="window";
        opening.properties["opening_assembly"] =
            opening_assembly_json(default_opening_assembly(OpeningAssemblyKind::window));
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
