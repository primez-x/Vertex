#include "sketch/pinc_project_import.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void require_close(double actual, double expected) {
    require(std::abs(actual-expected)<1e-10,"Pinc import changed a known-answer quantity");
}
Vec2 arc_midpoint(const Segment& s) {
    const double dx=s.end.x-s.start.x,dy=s.end.y-s.start.y;
    const double height=std::hypot(dx,dy)*.5*std::tan(s.sweep_radians*.25);
    const double chord=std::hypot(dx,dy);
    return {(s.start.x+s.end.x)*.5+dy/chord*height,
            (s.start.y+s.end.y)*.5-dx/chord*height};
}
PincImportProject parse_bytes(std::string_view bytes, PincImportLimits limits={}) {
    return parse_pinc_project(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),limits);
}
PincImportProject parse(const Json& value, PincImportLimits limits={}) { return parse_bytes(value.dump(),limits); }
template<class Operation> void rejects(Operation operation, std::string_view pointer={}) {
    try { operation(); } catch(const std::invalid_argument& error) {
        require(pointer.empty() || std::string_view(error.what()).find(pointer)!=std::string_view::npos,
                "Pinc refusal lost its source pointer"); return;
    }
    throw std::runtime_error("Pinc parser admitted malformed or over-budget data");
}
Json edge(std::string id, double ax, double ay, double bx, double by, double bulge=0) {
    return {{"id",id},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}},
            {"kind",bulge==0?"line":"arc"},{"bulge",bulge}};
}
Json page() { return {{"id","pg1"},{"name","Sketch"},{"calcWalls",Json::array()},
    {"interiorWalls",Json::array()},{"assignments",Json::object()},
    {"symbols",Json::array()},{"texts",Json::array()},{"underlay",nullptr}}; }
Json modern(Json pg=page()) { return {{"format","PincSketch"},{"version","4.2"},{"pages",{pg}}}; }
bool diagnostic(const PincImportProject& p, std::string_view code) {
    for(const auto& d:p.diagnostics) if(d.code==code) return true; return false;
}
void shared_sources_and_occurrence_identity() {
    auto pg=page();pg["calcWalls"]={edge("shared",10,0,10,8),edge("left",0,0,10,0),edge("right",10,0,20,0)};
    auto input=modern(pg);input["pages"].push_back(pg);
    const auto result=parse(input);
    require(result.pages.size()==2 && result.pages[0].calculation_segments.size()==3,"shared edges were expanded or lost");
    const auto& a=result.pages[0].calculation_segments[0];const auto& b=result.pages[1].calculation_segments[0];
    require(a.source.identity!=b.source.identity && a.source.json_pointer=="/pages/0/calcWalls/0","page occurrence identity lost");
    require_close(a.geometry.end.y,-2.4384);require_close(a.geometry.start.x,3.048);
    input["pages"][0]["calcWalls"].push_back(pg["calcWalls"][0]);
    rejects([&]{(void)parse(input);},"/pages/0/calcWalls/3/id");
    auto numeric=modern();numeric["pages"][0]["calcWalls"]={edge("a",0,0,1,0),edge("b",1,0,2,0)};
    numeric["pages"][0]["calcWalls"][0]["id"]=1;numeric["pages"][0]["calcWalls"][1]["id"]=1.0;
    rejects([&]{(void)parse(numeric);},"/pages/0/calcWalls/1/id");
}
void sagitta_and_threshold_known_answers() {
    auto pg=page();pg["calcWalls"]={edge("p",0,2,8,2,2),edge("n",0,2,8,2,-2),
        edge("major",0,0,8,0,8),edge("negative-major",0,0,8,0,-8),
        edge("tiny",0,0,8,0,.000999),edge("threshold",0,0,8,0,.001),
        edge("short",0,0,.0009,0,1)};
    const auto r=parse(modern(pg));const auto& e=r.pages[0].calculation_segments;
    require_close(e[0].geometry.sweep_radians,1.8545904360032244);
    require_close(e[1].geometry.sweep_radians,-1.8545904360032244);
    require_close(e[2].geometry.sweep_radians,4.428594871176362);
    require_close(e[3].geometry.sweep_radians,-4.428594871176362);
    // Independent circle midpoint: the transformed chord is at -.6096m;
    // positive source sagitta bows another .6096m toward negative Y.
    const auto midpoint=arc_midpoint(e[0].geometry);
    require_close(midpoint.x,1.2192);require_close(midpoint.y,-1.2192);
    require_close(arc_midpoint(e[1].geometry).y,0);
    require(e[4].geometry.sweep_radians==0 && e[5].geometry.sweep_radians>0 &&
        e[6].geometry.sweep_radians==0,"source curve thresholds changed");
}
void legacy_preserves_original_areas_and_conflicts() {
    auto first=edge("a",0,0,8,0,2),reverse=edge("b",8,0,0,0,-2),nearby=edge("c",.01,0,8,0,3);
    Json input={{"version","2.1"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",{first}}},
        {{"id",2},{"code","GAR"},{"segments",{reverse,nearby}}}}}};
    const auto r=parse(input);const auto& pg=r.pages[0];
    require(r.dialect==PincImportDialect::legacy_v2 && pg.legacy_areas.size()==2 &&
        pg.legacy_areas[1].segments.size()==2 && pg.assignments.size()==2,"legacy evidence was discarded");
    require(pg.calculation_segments.size()==2 && pg.calculation_segments[0].equivalent_sources.size()==1,
        "reversed exact analytical duplicate was not recognized");
    require(diagnostic(r,"legacy_near_edge_conflict") && diagnostic(r,"legacy_assignment_requires_review"),
        "legacy tolerant conversion was silently adopted");
    require(pg.assignments[0].source_segment_references.size()==1 && !pg.assignments[0].face_key,
        "legacy assignment acquired fabricated face authority");
    require_close(pg.legacy_areas[0].segments[0].geometry.sweep_radians,1.8545904360032244);
    input["version"]=2.1;require(parse(input).dialect==PincImportDialect::legacy_v2,"inspected numeric legacy version refused");
}
void categories_and_presentation_are_descriptive() {
    auto pg=page();pg["calcWalls"]=Json::array({edge("wall",0,0,8,0)});
    const char* codes[]={"GLA1","GLA2","GLA3","GLA4","GBA","BSMT-F","BSMT-U","GAR","DGAR","ADU","OUT","CAR","PORCH","PATIO","DECK","BALC","STG","LOW","OPEN","NCA","SITE","UND"};
    for(const auto* code:codes) { pg["assignments"][code]={{"code",code},{"name","Retained name"}}; }
    pg["texts"]={{{"id","t"},{"text","Kitchen"},{"x",2},{"y",3},{"size",.75},
        {"rot",90},{"align","right"},{"bold",true},{"font","Arial"}}};
    pg["symbols"]={{{"id","s"},{"kind","Door"},{"x",4},{"y",0},{"w",3},{"h",3},
        {"rot",90},{"mirrorX",true},{"mirrorY",false},{"doorHinge","right"},{"doorSide",-1},
        {"wallRef",{{"type","calc"},{"id","wall"},{"t",.5}}}}};
    pg["calcWalls"][0]["weight"]=3;pg["calcWalls"][0]["lineType"]="dashdot";
    const auto r=parse(modern(pg));const auto& p=r.pages[0];
    for(const auto& a:p.assignments) require(a.known_category,"known category was not retained descriptively");
    require(p.assignments.size()==22,"category assignment count changed");
    require_close(p.texts[0].rotation_radians,-std::numbers::pi/2);
    require(p.texts[0].alignment=="right" && p.texts[0].bold,"text presentation lost");
    require_close(p.symbols[0].width_metres,.9144);require_close(p.symbols[0].rotation_radians,-std::numbers::pi/2);
    require(p.symbols[0].mirror_x && p.symbols[0].wall_reference &&
        p.symbols[0].wall_reference->target.json_pointer=="/pages/0/calcWalls/0","visual wall association lost");
    require(p.calculation_segments[0].presentation.weight_pixels==3 &&
        p.calculation_segments[0].presentation.line_type=="dashdot","visual stroke weight changed");
}
void references_and_unsupported_content_remain_inspectable() {
    auto pg=page();pg["underlay"]={{"data","data:image/png;base64,aGVsbG8="},{"x",2},{"y",3},{"width",40},{"opacity",.28}};
    auto r=parse(modern(pg));require(r.pages[0].underlay->supported_raster_descriptor,"raster descriptor refused");
    require_close(r.pages[0].underlay->top_left_metres.y,-.9144);
    pg["underlay"]["data"]="https://example.invalid/trace.svg";
    r=parse(modern(pg));require(r.pages[0].underlay && !r.pages[0].underlay->supported_raster_descriptor &&
        diagnostic(r,"underlay_unsupported_descriptor"),"external reference silently adopted");
    pg["underlay"]["data"]="data:image/svg+xml;base64,PHN2Zz4=";
    require(diagnostic(parse(modern(pg)),"underlay_unsupported_descriptor"),"SVG underlay adopted");
    auto unknown=modern();unknown["pages"][0]["physical_wall_room"]={{"version",999}};
    require(diagnostic(parse(unknown),"unsupported_source_field"),"unknown authority-looking fields silently promoted");
}
void structural_refusals_and_budget_boundaries() {
    rejects([&]{(void)parse_bytes("{\"format\":\"PincSketch\",\"format\":\"PincSketch\",\"version\":\"4.2\",\"pages\":[{}]}");});
    auto input=modern();input["version"]="4.3";rejects([&]{(void)parse(input);},"/version");
    input=modern();input["pages"][0]["calcWalls"]=Json::array({edge("bad",0,0,0,0)});rejects([&]{(void)parse(input);},"/pages/0/calcWalls/0");
    input["pages"][0]["calcWalls"][0]["kind"]="ellipse";rejects([&]{(void)parse(input);},"/kind");
    input=modern();input["pages"][0]["calcWalls"]=Json::array({edge("bad",0,0,8,0,2)});
    input["pages"][0]["calcWalls"][0].erase("bulge");rejects([&]{(void)parse(input);},"/bulge");
    input=modern();input["pages"][0]["texts"]={{{"x",0},{"y",0},{"size",-1},{"text","bad"}}};
    rejects([&]{(void)parse(input);},"/size");
    PincImportLimits limits;limits.max_bytes=64*1024*1024+1;rejects([&]{(void)parse(modern(),limits);});
    input=modern();const auto bytes=input.dump();limits={};limits.max_bytes=bytes.size();(void)parse_bytes(bytes,limits);
    --limits.max_bytes;rejects([&]{(void)parse_bytes(bytes,limits);});
    limits={};limits.max_pages=1;input["pages"].push_back(page());rejects([&]{(void)parse(input,limits);},"/pages");
    input=modern();input["pages"][0]["calcWalls"]={edge("a",0,0,1,0),edge("b",1,0,2,0),edge("c",2,0,3,0)};
    limits={};limits.max_calculation_pairs=3;(void)parse(input,limits);limits.max_calculation_pairs=2;
    rejects([&]{(void)parse(input,limits);},"/calcWalls");
    limits={};limits.max_total_edges=3;(void)parse(input,limits);limits.max_total_edges=2;rejects([&]{(void)parse(input,limits);});
    limits={};limits.max_records=4;(void)parse(input,limits);limits.max_records=3;rejects([&]{(void)parse(input,limits);});
    limits={};limits.max_json_depth=4;input=modern();input["opaque"]=Json{{"nested",Json{{"too",Json{{"deep",Json::array({1})}}}}}};
    rejects([&]{(void)parse(input,limits);});
    limits={};limits.max_string_bytes=1;rejects([&]{(void)parse(modern(),limits);});
    limits={};limits.max_json_nodes=1;rejects([&]{(void)parse(modern(),limits);});
    input=modern();input["pages"][0]["symbols"]={{{"kind","Door"},{"x",0},{"y",0},{"w",3},{"h",3},
        {"wallRef",{{"type","calc"},{"id","missing"},{"t",.5}}}}};
    rejects([&]{(void)parse(input);},"/wallRef/id");
}
void aggregate_work_and_legacy_copy_ledgers() {
    auto pg=page();pg["calcWalls"]={edge("a",0,0,1,0),edge("b",1,0,2,0)};
    auto input=modern(pg);input["pages"].push_back(pg);
    PincImportLimits limits;limits.max_calculation_pairs=2;(void)parse(input,limits);
    limits.max_calculation_pairs=1;rejects([&]{(void)parse(input,limits);},"/pages/1/calcWalls");
    Json legacy={{"version","2.0"},{"areas",{{{"segments",{edge("a",0,0,8,0)}}},
        {{"segments",{edge("a",8,0,0,0)}}}}}};
    limits={};limits.max_total_edges=3;const auto r=parse(legacy,limits);
    require(r.pages[0].calculation_segments.size()==1 && r.pages[0].legacy_areas.size()==2,
        "legacy dedup lost original edges");
    limits.max_total_edges=2;rejects([&]{(void)parse(legacy,limits);});
    auto id=r.pages[0].calculation_segments[0].source;id.identity="forged";
    require(pinc_source_identity(id)==r.pages[0].calculation_segments[0].source.identity,
        "source identity helper trusted the supplied identity");
    id.scalar_id_json="[1]";rejects([&]{(void)pinc_source_identity(id);});
}
void shared_json_preflight_exact_boundaries() {
    const auto bounded=[](std::string_view input,PincImportLimits limits) {
        return parse_pinc_json_bounded(std::span(reinterpret_cast<const std::byte*>(input.data()),input.size()),limits);};
    PincImportLimits limits;limits.max_json_nodes=2;limits.max_string_bytes=2;limits.max_json_depth=1;
    require(bounded("{\"a\":\"b\"}",limits).at("a")=="b","exact SAX resource boundary refused");
    limits.max_json_nodes=1;rejects([&]{(void)bounded("{\"a\":\"b\"}",limits);});
    limits.max_json_nodes=2;limits.max_string_bytes=1;rejects([&]{(void)bounded("{\"a\":\"b\"}",limits);});
    limits={};limits.max_json_depth=2;(void)bounded("{\"a\":{}}",limits);
    limits.max_json_depth=1;rejects([&]{(void)bounded("{\"a\":{}}",limits);});
    limits={};rejects([&]{(void)bounded("{\"a\":{\"b\":1,\"b\":2}}",limits);});
    rejects([&]{(void)bounded("{\"a\":1e999}",limits);});
    rejects([&]{(void)bounded("[",limits);});
    auto input=modern();input["pages"][0]["calcWalls"]=Json::array({edge("a",0,0,1e8,0)});
    rejects([&]{(void)parse(input);},"/b/x");
    input=modern();input["pages"][0]["underlay"]={{"data","data:image/png;base64,invalid"},
        {"x",0},{"y",0},{"width",40},{"opacity",1}};
    rejects([&]{(void)parse(input);},"/underlay/data");
}
void assignment_reference_budgets() {
    auto pg = page();
    pg["calcWalls"] = {edge("a",0,0,1,0),edge("b",1,0,2,0),edge("c",2,0,3,0)};
    pg["assignments"]["a|b|c"] = {{"code","GLA1"}};
    PincImportLimits limits; limits.max_records = 9;
    require(parse(modern(pg), limits).pages[0].assignments[0].source_segment_references.size() == 3,
        "exact modern reference budget was refused");
    limits.max_records = 8; rejects([&] { (void)parse(modern(pg), limits); }, "assignments");
    // Five records precede reference expansion; the second reference must fail
    // before its copy is appended, rather than waiting for wire encoding.
    limits.max_records = 6; rejects([&] { (void)parse(modern(pg), limits); }, "assignments");
    const Json old{{"version","2.1"},{"areas",{{{"code","GLA1"},
        {"segments",{edge("a",0,0,1,0)}}}}}};
    limits.max_records = 7;
    require(parse(old, limits).pages[0].assignments[0].source_segment_references.size() == 1,
        "exact legacy reference budget was refused");
    limits.max_records = 6; rejects([&] { (void)parse(old, limits); }, "areas");
}
}
int main() {
    const char* active = "start";
    const auto run = [&](const char* name, auto test) { active = name; test(); };
    try { run("shared sources", shared_sources_and_occurrence_identity);
        run("sagitta thresholds", sagitta_and_threshold_known_answers);
        run("legacy conflicts", legacy_preserves_original_areas_and_conflicts);
        run("presentation", categories_and_presentation_are_descriptive);
        run("unsupported content", references_and_unsupported_content_remain_inspectable);
        run("budgets", structural_refusals_and_budget_boundaries);
        run("aggregate ledgers", aggregate_work_and_legacy_copy_ledgers);
        run("JSON preflight", shared_json_preflight_exact_boundaries);
        run("assignment reference budgets", assignment_reference_budgets);
        std::cout<<"Pinc project import tests passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<active<<": "<<error.what()<<'\n';return 1; }
}
