#include "sketch/pinc_presentation_admission.hpp"
#include "sketch/boundary_entity.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void close(double actual,double expected){require(std::abs(actual-expected)<1e-10,"presentation coordinate or scale changed");}
template<class F>void rejects(F operation){try{operation();}catch(const std::invalid_argument&){return;}throw std::runtime_error("unsafe presentation accepted");}
Json edge(std::string id,double ax,double ay,double bx,double by) {
    return {{"id",id},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}},{"kind","line"},
        {"color","#123456"},{"weight",3},{"lineType","dash"},{"dimSize",.8},{"dimFont","Arial"},
        {"dimColor","#abcdef"},{"dimOffset",{{"x",1},{"y",2}}}};
}
Json square(std::string prefix="",double x=0) {
    return Json::array({edge(prefix+"a",x,0,x+8,0),edge(prefix+"b",x+8,0,x+8,8),
        edge(prefix+"c",x+8,8,x,8),edge(prefix+"d",x,8,x,0)});
}
PincImportProject source(bool two=false) {
    auto edges=square();if(two)for(const auto& e:square("i",12))edges.push_back(e);
    edges[0]["dimHidden"]=true;
    Json assignment={{"code","GLA1"},{"name","First native area"},{"color","#654321"},{"opacity",.37},
        {"hatch","cross"},{"labelColor","#fedcba"},{"nameSize",.9},{"calcSize",.7},
        {"showName",false},{"showCalc",true},{"namePos",{{"x",1},{"y",2}}},
        {"calcPos",{{"x",3},{"y",4}}},{"_anchor",{{"x",500},{"y",500}}},{"_area",987654321}};
    Json assignments={{"a|b|c|d",assignment}};
    if(two){assignment["name"]="Second native area";assignments["ia|ib|ic|id"]=assignment;}
    Json text={{"id",1},{"text","First line\nSecond line"},{"x",2},{"y",3},{"size",.5},{"rot",35},
        {"color","#112233"},{"font","Segoe UI"},{"align","right"},{"bold",true},{"italic",true}};
    Json symbol={{"id",2},{"kind","fixture"},{"x",4},{"y",5},{"w",3},{"h",2},{"rot",90},
        {"mirrorX",true},{"mirrorY",true},{"wallRef",{{"type","calc"},{"id","a"},{"t",.25}}}};
    Json page={{"calcWalls",edges},{"interiorWalls",Json::array()},{"assignments",assignments},{"texts",{text}}, {"symbols",{symbol}}};
    const auto bytes=Json{{"format","PincSketch"},{"version","4.2"},{"pages",{page}}}.dump();
    return parse_pinc_project(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
}
PincImportProject many_source(std::size_t count) {
    auto edges=Json::array();auto assignments=Json::object();
    for(std::size_t i=0;i<count;++i) {
        const auto prefix="island"+std::to_string(i)+"_";
        for(const auto& item:square(prefix,12*static_cast<double>(i)))edges.push_back(item);
        assignments[prefix+"a|"+prefix+"b|"+prefix+"c|"+prefix+"d"]={{"code","GLA1"},{"name","Region "+std::to_string(i)}};
    }
    const Json page={{"calcWalls",edges},{"assignments",assignments}};
    const auto bytes=Json{{"format","PincSketch"},{"version","4.2"},{"pages",{page}}}.dump();
    return parse_pinc_project(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
}
Document base() {
    return Document::create({{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},{"floor","floor",{{"building_id","building"}}},
        {"calc","layer",{{"floor_id","floor"}}},{"interior","layer",{{"floor_id","floor"}}}});
}
PincSymbolBinding binding() {
    PincSymbolBinding result;result.source_kind="fixture";result.fidelity_note="Reviewed source-compatible footprint";
    auto& d=result.definition;d.id="test_fixture";d.name="Test fixture";d.family="fixture";d.category="test";
    d.width_metres=1;d.depth_metres=.5;d.preview={{{-.5,-.25},{.5,.25}}};
    result.pinned_svg="<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\"><path d=\"M0 0L100 50\"/></svg>";
    d.svg_asset=SymbolSvgAsset{"symbols/test_fixture.svg",sha256_hex(std::span(
        reinterpret_cast<const std::byte*>(result.pinned_svg.data()),result.pinned_svg.size())),{0,0,100,50},{0,0,100,50},false};
    return result;
}
PincMeasurementAdmission measurement(const PincImportProject& src,const DocumentSnapshot& initial) {
    const std::vector<PincPageGeometryContext> contexts={{0,{"property","building","floor","calc"},{"property","building","floor","interior"}}};
    std::vector<PincAreaReview> reviews;for(std::size_t a=0;a<src.pages[0].assignments.size();++a)reviews.push_back({0,a,true,"first_floor"});
    return prepare_pinc_measurement_admission(src,initial,contexts,"measurement",reviews);
}
DocumentSnapshot preview(const DocumentSnapshot& initial,const std::vector<Entity>& entities) {
    ApplyEntityChanges command{initial.revision(),{}, {},"Private candidate"};
    for(const auto& e:entities)command.entity_changes.push_back(EntityChange::upsert(e));return Document::preview_command(initial,command);
}
const PresentationOverride& role(const AnnotationState& state,std::string_view kind,std::string_view target) {
    for(const auto& item:state.overrides)if(item.target_kind==kind&&item.target_id==target)return item;
    throw std::runtime_error("missing native presentation role");
}
void live_roles_styles_anchors_and_detachment() {
    const auto src=source();auto document=base();const auto initial=document.snapshot();const auto measured=measurement(src,initial);
    const auto candidate=preview(initial,measured.entities);const auto saved_binding=binding();
    const auto result=prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"presentation");
    require(result.entities.size()==1,"source page has no single carrier");
    require(document.snapshot().entities()==initial.entities()&&document.revision()==initial.revision(),"detached preparation mutated source document");
    const auto& carrier=result.entities[0];const auto state=decode_annotation_entity(carrier);
    require(organize_project(preview(candidate,result.entities)).drawing_context(carrier.id)==measured.geometry.pages[0].calculation_context,
        "carrier lost complete page context");
    const auto area=measured.area_mappings[0].area_id;const auto& name=role(state,"area_name",area);const auto& calc=role(state,"area_calculation",area);
    require(!name.visible&&calc.visible,"independent area role visibility lost");
    close(name.style.text_height_metres,.9*.3048);close(calc.style.text_height_metres,.7*.3048);
    require(name.style.stroke_color=="#fedcba"&&calc.style.stroke_color=="#fedcba","source role color lost");
    const auto anchor=area_label_anchor(boundary_geometry(decode_identified_boundary_entity(candidate.entities().at(area))));
    close(anchor.x+name.plan_label_offset->x,.3048);close(anchor.y+name.plan_label_offset->y,-2*.3048);
    close(anchor.x+calc.plan_label_offset->x,3*.3048);close(anchor.y+calc.plan_label_offset->y,-4*.3048);
    const auto& appearance=role(state,"area",area);require(appearance.style.fill_opacity.has_value(),"explicit source opacity missing");close(*appearance.style.fill_opacity,.37);
    require(appearance.style.fill_pattern=="cross"&&appearance.style.fill_color=="#654321","source fill style lost");
    const auto stroke=measured.geometry.source_mappings[0].stroke_id;const auto& line=role(state,"object",stroke);
    require(line.style.line_pattern=="dash"&&line.style.stroke_color=="#123456","source segment style lost");
    close(*line.paper_line_width_mm,3*25.4/96);
    const auto& dimension=role(state,"wall_dimension",stroke);require(!dimension.visible&&dimension.use_model_text_height,"hidden source dimension or model-height intent lost");
    close(dimension.style.text_height_metres,.8*.3048);close(dimension.plan_label_offset->x,.3048);close(dimension.plan_label_offset->y,-2*.3048);
    require(state.labels.size()==1&&state.labels[0].content=="First line\nSecond line"&&state.labels[0].style.text_alignment=="right"&&
        state.labels[0].style.bold&&state.labels[0].style.italic&&state.labels[0].model_plan,"source multiline label style lost");
    close(state.labels[0].placement.rotation_radians,-35*std::numbers::pi/180);
    require(state.labels[0].placement.layer_id=="calc","text lost page layer");
    const auto wire=carrier.properties.at("state").dump();
    require(wire.find("987654321")==std::string::npos&&wire.find("First native area")==std::string::npos&&
        wire.find("500")==std::string::npos,"presentation stored source cached quantities or static area text");
    const auto roundtrip=decode_annotation_entity(Entity{carrier.id,carrier.type,Json::parse(carrier.properties.dump()),carrier.required,carrier.extensions});
    require(encode_annotation_state(roundtrip,{})==encode_annotation_state(state,{}),"native annotation codec changed presentation");
    const auto completed=preview(candidate,result.entities);auto imported=Document::fork(initial);
    std::vector<Entity> all=measured.entities;all.insert(all.end(),result.entities.begin(),result.entities.end());
    ApplyEntityChanges command{initial.revision(),{}, {},"One admitted import"};for(const auto& e:all)command.entity_changes.push_back(EntityChange::upsert(e));
    imported.apply(command);require(imported.snapshot().entities()==completed.entities(),"combined import differs from private preview");
    imported.undo(imported.revision());require(imported.snapshot().entities()==initial.entities(),"single import undo was not exact");
}
void pinned_footprint_transforms_and_visual_association() {
    const auto src=source();auto document=base();const auto measured=measurement(src,document.snapshot());const auto candidate=preview(document.snapshot(),measured.entities);
    const auto saved_binding=binding();const auto result=prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"presentation");
    const auto state=decode_annotation_entity(result.entities[0]);require(state.symbols.size()==1,"reviewed symbol missing");
    require(result.diagnostics.size()==1&&result.diagnostics[0].code=="symbol_artwork_fidelity"&&
        result.diagnostics[0].source_pointer==src.pages[0].symbols[0].source.json_pointer&&
        result.diagnostics[0].message==saved_binding.fidelity_note,"mapped symbol artwork note omitted from occurrence diagnostics");
    const auto& symbol=state.symbols[0];require(symbol.pinned_svg==saved_binding.pinned_svg&&symbol.definition&&symbol.flip_horizontal&&symbol.flip_vertical,
        "pinned artwork or source mirrors changed");
    close(symbol.definition->width_metres*symbol.width_scale*symbol.placement.scale,3*.3048);
    close(symbol.definition->depth_metres*symbol.depth_scale*symbol.placement.scale,2*.3048);
    close(symbol.placement.rotation_radians,-std::numbers::pi/2);
    const auto point=transformed_symbol_point(*symbol.definition,symbol,{.5,.25});
    close(point.x,4*.3048-.3048);close(point.y,-5*.3048+1.5*.3048);
    const auto& metadata=result.entities[0].extensions.at("pinc_presentation").at("children").at(symbol.id);
    const auto& association=metadata.at("visual_wall_reference");
    require(association.at("stroke_id")==measured.geometry.source_mappings[0].stroke_id&&association.at("segment_id")==measured.geometry.source_mappings[0].segment_id,
        "visual wall reference lost actual native stroke/segment");
    close(association.at("parameter").get<double>(),.25);
    require(metadata.at("source_pointer")==src.pages[0].symbols[0].source.json_pointer&&metadata.at("door_hinge")=="left"&&metadata.at("door_side")==1,
        "source provenance or orientation association missing");
    require(metadata.at("fidelity_note")==result.diagnostics[0].message,"artwork note differs between provenance and review diagnostic");
    for(const auto& e:result.entities)require(e.type==kAnnotationEntityType&&!e.properties.contains("wall_id")&&!e.properties.contains("thickness_m"),"symbol manufactured physical hosting");
    auto unknown=src;unknown.pages[0].symbols[0].kind="unreviewed";
    const auto unresolved=prepare_pinc_presentation_admission(unknown,measured,candidate,{&saved_binding,1},"unknown");
    require(decode_annotation_entity(unresolved.entities[0]).symbols.empty()&&unresolved.diagnostics.size()==1&&
        unresolved.diagnostics[0].source_pointer==unknown.pages[0].symbols[0].source.json_pointer&&unresolved.diagnostics[0].code=="unresolved_symbol_kind",
        "unknown kind silently fabricated artwork or lost source pointer");
    auto without_note=saved_binding;without_note.fidelity_note=" \t\r\n";
    const auto quiet=prepare_pinc_presentation_admission(src,measured,candidate,{&without_note,1},"quiet");
    require(quiet.diagnostics.empty()&&decode_annotation_entity(quiet.entities[0]).symbols.size()==1,"blank artwork note created a spurious warning or dropped symbol");
}
void mapping_attacks_limits_and_atomic_failures() {
    const auto src=source(true);auto document=base();const auto initial=document.snapshot();const auto measured=measurement(src,initial);const auto candidate=preview(initial,measured.entities);
    const auto saved_binding=binding();
    auto forged=measured;std::swap(forged.area_mappings[0].area_id,forged.area_mappings[1].area_id);
    rejects([&]{(void)prepare_pinc_presentation_admission(src,forged,candidate,{&saved_binding,1},"bad");});
    forged=measured;forged.geometry.source_mappings[0].segment_id="forged";
    rejects([&]{(void)prepare_pinc_presentation_admission(src,forged,candidate,{&saved_binding,1},"bad");});
    forged=measured;forged.geometry.pages[0].calculation_context.layer_id="interior";
    rejects([&]{(void)prepare_pinc_presentation_admission(src,forged,candidate,{&saved_binding,1},"bad");});
    forged=measured;
    for(auto& entity:forged.entities)if(entity.id==forged.area_mappings[0].area_id)
        entity.extensions["measurement_linework_sources"][0][0]["parameter_start"]=.1;
    const auto stale_candidate=preview(initial,forged.entities);
    rejects([&]{(void)prepare_pinc_presentation_admission(src,forged,stale_candidate,{&saved_binding,1},"stale");});
    forged=measured;forged.area_mappings.pop_back();
    rejects([&]{(void)prepare_pinc_presentation_admission(src,forged,candidate,{&saved_binding,1},"missing");});
    auto hostile=src;hostile.pages[0].texts.push_back(hostile.pages[0].texts[0]);
    rejects([&]{(void)prepare_pinc_presentation_admission(hostile,measured,candidate,{&saved_binding,1},"bad");});
    hostile=src;hostile.pages[0].symbols[0].wall_reference->target=hostile.pages[0].texts[0].source;
    rejects([&]{(void)prepare_pinc_presentation_admission(hostile,measured,candidate,{&saved_binding,1},"bad");});
    hostile=src;hostile.pages[0].symbols[0].width_metres=0;
    rejects([&]{(void)prepare_pinc_presentation_admission(hostile,measured,candidate,{&saved_binding,1},"bad");});
    auto bad_binding=saved_binding;bad_binding.pinned_svg+=" ";
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&bad_binding,1},"bad");});
    const std::vector<PincSymbolBinding> duplicate_bindings={saved_binding,saved_binding};
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,duplicate_bindings,"bad");});
    PincPresentationAdmissionLimits limit;limit.max_symbols=0;
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"bad",limit);});
    limit={};limit.max_labels=0;rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"bad",limit);});
    limit={};limit.max_pinned_svg_bytes=1;rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"bad",limit);});
    limit={};limit.max_bindings=81;rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"bad",limit);});
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved_binding,1},"measurement");});
    require(document.snapshot().entities()==initial.entities()&&document.snapshot().history().size()==initial.history().size(),"rejected/cancelled preparation changed live document");
}
void many_faces_share_one_bounded_page_detection() {
    constexpr std::size_t count=12;const auto src=many_source(count);auto document=base();const auto initial=document.snapshot();
    const auto measured=measurement(src,initial);require(measured.area_mappings.size()==count,"many-face fixture lacks native owners");
    const auto candidate=preview(initial,measured.entities);PincPresentationAdmissionLimits limits;
    const auto edges=static_cast<std::uint64_t>(4*count),pairs=edges*(edges-1)/2;
    limits.geometry_verification.source.max_calculation_pairs=4*pairs;
    limits.geometry_verification.max_graph_edges=4*edges;
    limits.geometry_verification.max_face_edge_uses=4*edges;
    const auto result=prepare_pinc_presentation_admission(src,measured,candidate,{},"many",limits);
    require(result.entities.size()==1,"many-face page produced multiple carriers");
    const auto state=decode_annotation_entity(result.entities[0]);
    for(const auto& mapping:measured.area_mappings) {
        require(role(state,"area_name",mapping.area_id).visible&&role(state,"area_calculation",mapping.area_id).visible,
            "many-face page lost independent native owner presentation");
    }
    // Exactly four graph reservations cover one page regardless of how many
    // assignments it owns. One less pair cannot admit the complete work.
    --limits.geometry_verification.source.max_calculation_pairs;
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{},"pairs",limits);});
    limits.geometry_verification.source.max_calculation_pairs=4*pairs;
    limits.geometry_verification.max_correspondence_work=20'000;
    try{(void)prepare_pinc_presentation_admission(src,measured,candidate,{},"work",limits);throw std::runtime_error("unreserved native ownership matching accepted");}
    catch(const std::invalid_argument& error){require(std::string(error.what()).find("presentation verification work")!=std::string::npos,
        "native detection work was not reserved before its execution");}
    limits.geometry_verification.max_correspondence_work=250'001;
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{},"over",limits);});
    require(document.snapshot().entities()==initial.entities(),"budget rejection mutated live document");
}
void canonical_door_orientation_and_opening_anchor() {
    auto src=source();src.pages[0].symbols[0].door_hinge="right";src.pages[0].symbols[0].door_side=-1;
    auto document=base();const auto measured=measurement(src,document.snapshot());const auto candidate=preview(document.snapshot(),measured.entities);
    auto saved=binding();saved.door_transform=PincDoorArtworkTransform::swing_left_positive_svg_y;saved.opening_anchor_fraction={0,.25};
    auto result=prepare_pinc_presentation_admission(src,measured,candidate,{&saved,1},"swing");
    auto symbol=decode_annotation_entity(result.entities[0]).symbols[0];
    require(!symbol.flip_horizontal&&!symbol.flip_vertical,"canonical swing hinge/side did not combine with source mirrors");
    const auto& input=src.pages[0].symbols[0];
    close(symbol.placement.position.x,input.centre_metres.x-.25*input.depth_metres);close(symbol.placement.position.y,input.centre_metres.y);
    const auto opening=transformed_symbol_point(*symbol.definition,symbol,{symbol.definition->anchor.x,
        symbol.definition->anchor.y+.25*symbol.definition->depth_metres});
    close(opening.x,input.centre_metres.x);close(opening.y,input.centre_metres.y);
    saved.door_transform=PincDoorArtworkTransform::side_positive_svg_y;
    result=prepare_pinc_presentation_admission(src,measured,candidate,{&saved,1},"side");
    symbol=decode_annotation_entity(result.entities[0]).symbols[0];
    require(symbol.flip_horizontal&&!symbol.flip_vertical,"side-only artwork incorrectly applied hinge transform");
    saved.door_transform=PincDoorArtworkTransform::symmetric_source_garage;saved.opening_anchor_fraction={};
    result=prepare_pinc_presentation_admission(src,measured,candidate,{&saved,1},"garage");
    symbol=decode_annotation_entity(result.entities[0]).symbols[0];
    require(!symbol.flip_horizontal&&!symbol.flip_vertical,"symmetric source garage acquired source-invisible flips");
    close(symbol.placement.position.x,input.centre_metres.x);close(symbol.placement.position.y,input.centre_metres.y);
    saved.door_transform=static_cast<PincDoorArtworkTransform>(999);
    rejects([&]{(void)prepare_pinc_presentation_admission(src,measured,candidate,{&saved,1},"enum");});
}
}
int main(){try{live_roles_styles_anchors_and_detachment();pinned_footprint_transforms_and_visual_association();mapping_attacks_limits_and_atomic_failures();
    many_faces_share_one_bounded_page_detection();canonical_door_orientation_and_opening_anchor();
    std::cout<<"Pinc presentation admission tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
