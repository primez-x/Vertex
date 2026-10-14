#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/phase_roof_edit.hpp"
#include "sketch/roof_clone.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
constexpr auto shared = PlanAxisResizeAnchorMode::shared_frame;
const std::string archive_key{roof_plan_resize_derivations_key};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require_near(double actual, double expected, const char* message) {
    const double tolerance = 5e-7+128*std::numeric_limits<double>::epsilon()*
        std::max({1.0,std::abs(actual),std::abs(expected)});
    require(std::isfinite(actual) && std::abs(actual-expected) <= tolerance,message);
}
template<class F> void rejects(F function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
Entity roof(int form = 0, double angle = 0, Vec3 position = {2,3,4}) {
    Entity result;
    if (form == 0) result = encode_building_entity(SlopedRoofPanel{.id="roof",
        .base_position=position,.orientation_radians=angle,.run=6,.span=5,.rise=1.2,
        .pitch_radians=std::atan(.2),.overhang=.3,.thickness=.2});
    else if (form == 1) result = encode_building_entity(GableRoof{.id="roof",
        .base_position=position,.orientation_radians=angle,.length=8,.span=6,.rise=1.2,
        .pitch_radians=std::atan(.4),.overhang=.3,.thickness=.2});
    else result = encode_building_entity(HipRoof{.id="roof",
        .base_position=position,.orientation_radians=angle,.length=12,.span=6,.rise=1.2,
        .pitch_radians=std::atan(.4),.overhang=.3,.thickness=.2});
    result.properties["vendor"] = {{"numeric",17},{"text","retained"}};
    result.extensions["vendor"] = {{"version",81},{"payload",Json::array({2.0,"opaque"})}};
    return result;
}
Entity flat() {
    return encode_building_entity(SlopedRoofPanel{.id="roof",.base_position={.8,0,4},
        .run=2,.span=2,.rise=0,.pitch_radians=0,.overhang=0,.thickness=.25});
}
// Independent native solid query; the expected operator below never reads
// candidate dimensions, its archive, or the stage's extent solver.
Bounds2 native_bounds(const Entity& entity) {
    const auto object = decode_roof_entity(entity);
    const auto solid = make_roof_structure_shape(object);
    require(!solid.IsNull() && solid_volume(solid) > 0,"actual native roof must have positive volume");
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),
        -entity.properties.at("orientation_rad").get<double>());
    const auto local = BRepBuilderAPI_Transform(solid,rotation,true).Shape();
    Bnd_Box box;
    BRepBndLib::AddOptimal(local,box,false,false);
    double xmin,ymin,zmin,xmax,ymax,zmax;
    box.Get(xmin,ymin,zmin,xmax,ymax,zmax);
    return {{xmin,ymin},{xmax,ymax}};
}
Bounds2 affine_bounds(const Bounds2& before, double angle, const RoofPlanResizeIntent& intent) {
    const double c=std::cos(angle),s=std::sin(angle);
    const double fc=std::cos(intent.frame_rotation_radians),fs=std::sin(intent.frame_rotation_radians);
    Bounds2 expected{{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()},
        {-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}};
    for (double x : {before.minimum.x,before.maximum.x}) for (double y : {before.minimum.y,before.maximum.y}) {
        const double wx=c*x-s*y,wy=s*x+c*y;
        const double dx=wx-intent.anchor.x,dy=wy-intent.anchor.y;
        const double u=intent.scale_x*(fc*dx+fs*dy),v=intent.scale_y*(-fs*dx+fc*dy);
        const double ax=intent.anchor.x+fc*u-fs*v,ay=intent.anchor.y+fs*u+fc*v;
        const double lx=c*ax+s*ay,ly=-s*ax+c*ay;
        expected.minimum.x=std::min(expected.minimum.x,lx);expected.maximum.x=std::max(expected.maximum.x,lx);
        expected.minimum.y=std::min(expected.minimum.y,ly);expected.maximum.y=std::max(expected.maximum.y,ly);
    }
    return expected;
}
void compare_bounds(const Bounds2& actual, const Bounds2& expected) {
    require_near(actual.minimum.x,expected.minimum.x,"native MIN X must follow supplied affine operator");
    require_near(actual.maximum.x,expected.maximum.x,"native MAX X must follow supplied affine operator");
    require_near(actual.minimum.y,expected.minimum.y,"native MIN Y must follow supplied affine operator");
    require_near(actual.maximum.y,expected.maximum.y,"native MAX Y must follow supplied affine operator");
}
void shared_native_operator() {
    for (int form : {0,1,2}) for (double angle : {0.,.47}) for (bool swapped : {false,true})
        for (int axis : {0,1,2}) for (bool georeferenced : {false,true}) {
            const auto source=roof(form,angle,georeferenced ? Vec3{1e7,-2e7,40} : Vec3{2,3,4});
            const auto before=native_bounds(source);
            const Vec2 anchor=georeferenced ? Vec2{1e7-30,-2e7+20} : Vec2{-31,19};
            const RoofPlanResizeIntent intent{"roof",axis == 1 ? 1. : 1.25,
                axis == 0 ? 1. : 1.5,anchor,angle+(swapped ? std::numbers::pi/2 : 0),shared};
            const auto result=replay_roof_plan_resize_entity(source,intent);
            compare_bounds(native_bounds(result),affine_bounds(before,angle,intent));
            for (const auto* key : {"version","orientation_rad","rise_m","overhang_m","thickness_m","vendor"})
                require(result.properties.at(key).dump()==source.properties.at(key).dump(),"shared stage must retain authored raw scalars");
            require(result.properties.at("base_position_m").at(2).dump()==source.properties.at("base_position_m").at(2).dump(),"authored Z remains byte exact");
            require(result.extensions.at("vendor").dump()==source.extensions.at("vendor").dump(),"opaque extensions remain byte exact");
            require(capture_roof_plan_resize(source,result,intent).has_value(),"actual affine proof must capture native result");
            auto counterfeit=result;counterfeit.properties["vendor"]["numeric"]=18;
            rejects([&] { (void)capture_roof_plan_resize(source,counterfeit,intent); },"capture must reject unrelated candidate changes");
        }
    const auto source=roof(0,.47);
    const RoofPlanResizeIntent uniform{"roof",1.2,1.2,{-90,60},.73,shared};
    compare_bounds(native_bounds(replay_roof_plan_resize_entity(source,uniform)),affine_bounds(native_bounds(source),.47,uniform));
    for (double offset : {5e-11,.2})
        rejects([&] { (void)replay_roof_plan_resize_entity(source,{"roof",1.25,1.5,{},.47+offset,shared}); },
            "real near-axis anisotropic shear must refuse rather than snap");
}
void historical_oracle_and_units() {
    const auto anchor_red=encode_building_entity(SlopedRoofPanel{.id="roof",.base_position={1,0,4},
        .run=2,.span=2,.rise=1,.pitch_radians=std::atan(.5),.overhang=.2,.thickness=.25});
    require_near(native_bounds(anchor_red).minimum.x,.8,"recorded anchor RED source has genuine native MIN .8");
    const RoofPlanResizeIntent red_operator{"roof",1.4,1,{-.5,-1},0,shared};
    require_near(native_bounds(replay_roof_plan_resize_entity(anchor_red,red_operator)).minimum.x,1.32,
        "recorded sloped native RED must follow shared anchor to MIN 1.32");
    auto red_legacy=red_operator;red_legacy.anchor_mode=PlanAxisResizeAnchorMode::physical_edge;
    require_near(native_bounds(replay_roof_plan_resize_entity(anchor_red,red_legacy)).minimum.x,.8,
        "recorded RED v1 remains historical physical-edge MIN .8");
    const auto source=flat();
    const RoofPlanResizeIntent legacy{"roof",.8,1,{3.4,0},0};
    const auto wire=encode_roof_plan_resize_intent(legacy);
    require(wire.size()==6 && wire.at("version")==1 && !wire.contains("anchor_mode"),"legacy wire stays exact six-field v1");
    const auto old=replay_roof_plan_resize_entity(source,legacy);
    const auto historical=native_bounds(old);
    require_near(historical.minimum.x,1.2,"v1 retains the nearest physical MAX edge");
    require_near(historical.maximum.x,2.8,"v1 MAX edge remains pinned");
    auto intent=legacy;intent.anchor_mode=shared;
    const auto current=replay_roof_plan_resize_entity(source,intent);
    require_near(native_bounds(current).minimum.x,1.32,"shared cohort anchor fixes actual 1.32 native MIN regression");
    require_near(native_bounds(current).maximum.x,2.92,"shared cohort anchor moves native MAX by actual affine rule");
    const auto sloped=roof();const auto before=native_bounds(sloped);
    const auto v1=replay_roof_plan_resize_entity(sloped,{"roof",1.25,1,{-31,19},0});
    const auto after=native_bounds(v1);
    require_near(after.minimum.x,before.minimum.x,"v1 edited X retains nearest physical edge");
    require_near((after.minimum.y+after.maximum.y)*.5,(before.minimum.y+before.maximum.y)*.5,"v1 unchanged axis retains historical centre");
    for (const auto& retained : {source,old,current}) for (auto mode : {PlanAxisResizeAnchorMode::physical_edge,shared}) {
        const RoofPlanResizeIntent unit{"roof",1,1,{-800,600},.79,mode};
        const auto result=replay_roof_plan_resize_entity(retained,unit);
        require(exact(result,retained),"unit must preserve every property and archive byte");
        require(!capture_roof_plan_resize(retained,result,unit),"exact unit capture has no operation");
    }
}
void codecs_and_unit_admission() {
    const auto source=flat();
    const RoofPlanResizeIntent intent{"roof",1,1,{},0,shared};
    const auto wire=encode_roof_plan_resize_intent(intent);
    require(wire.size()==7 && wire.at("version")==2 && wire.at("anchor_mode")=="shared_frame", "shared wire is closed v2");
    require(encode_roof_plan_resize_intent(decode_roof_plan_resize_intent(wire)).dump()==wire.dump(),"v2 codec must roundtrip exactly");
    for (const auto mode : {static_cast<PlanAxisResizeAnchorMode>(-1),static_cast<PlanAxisResizeAnchorMode>(2)}) {
        auto bad=intent;bad.anchor_mode=mode;
        rejects([&] { (void)encode_roof_plan_resize_intent(bad); },"invalid enum must reject in encoder even for units");
        rejects([&] { (void)replay_roof_plan_resize_entity(source,bad); },"invalid enum must reject in replay even for units");
        rejects([&] { (void)stage_roof_plan_axis_resize_entity(source,1,1,{},0,mode); },"invalid low-level enum must reject before unit return");
    }
    for (const Json& mode : {Json("physical_edge"),Json("future"),Json(1),Json(nullptr)}) {
        auto bad=wire;bad["anchor_mode"]=mode;
        rejects([&] { (void)decode_roof_plan_resize_intent(bad); },"v2 admits only explicit shared_frame token");
    }
    auto bad=wire;bad["version"]=1;
    rejects([&] { (void)decode_roof_plan_resize_intent(bad); },"v1 may not borrow seventh field");
    bad=wire;bad.erase("anchor_mode");
    rejects([&] { (void)decode_roof_plan_resize_intent(bad); },"v2 requires seventh field");
    bad=wire;bad["version"]=3;
    rejects([&] { (void)decode_roof_plan_resize_intent(bad); },"future proof schema refuses");
    bad=wire;bad["opaque"]=true;
    rejects([&] { (void)decode_roof_plan_resize_intent(bad); },"closed proof refuses extra fields");
    for (int schema : {2,3,4}) {
        auto older=source;older.properties["version"]=schema;older.properties["roof_openings"]=Json::array();
        rejects([&] { (void)stage_roof_plan_axis_resize_entity(older,1,1,{},0,shared); },"shared schema gate precedes low-level unit return");
        rejects([&] { (void)replay_roof_plan_resize_entity(older,intent); },"shared schema gate applies to units in proof stage");
    }
}
void archive_capabilities() {
    // Every retained schema remains reproducible with v1 in archive4; older
    // archives keep their narrower frame contracts, independently of caching.
    for (int schema : {1,2,3,4}) {
        auto source=roof();source.properties["version"]=schema;
        if (schema != 1) source.properties["roof_openings"]=Json::array({
            {{"id","historical-cut"},{"x_m",1},{"y_m",1},{"width_m",.5},{"depth_m",.5}}});
        auto resized=replay_roof_plan_resize_entity(source,{"roof",1.2,1.1,{-31,19},0});
        const auto historical=resized.extensions.at(archive_key).at("operations").at(0).dump();
        resized.extensions.at(archive_key).at("version")=4;
        validate_roof_plan_resize_derivations(resized);
        require(resized.extensions.at(archive_key).at("operations").at(0).dump()==historical,"archive4 must retain v1 record bytes");
        for (int version : {1,2,3}) {
            auto older=resized;older.extensions.at(archive_key).at("version")=version;
            if ((schema==3 && version<2) || (schema==4 && version<3))
                rejects([&] { validate_roof_plan_resize_derivations(older); },"older archive may not borrow newer frame schema");
            else validate_roof_plan_resize_derivations(older);
        }
    }
    const auto current=replay_roof_plan_resize_entity(roof(),{"roof",1.2,1.1,{-31,19},0,shared});
    require(current.extensions.at(archive_key).at("version")==4,"shared change requires archive4");
    validate_roof_plan_resize_derivations(current);
    for (int version : {1,2,3}) {
        auto older=current;older.extensions.at(archive_key).at("version")=version;
        rejects([&] { validate_roof_plan_resize_derivations(older); },"older archive must reject cached shared v2 records");
    }
    auto counterfeit=current;
    counterfeit.extensions.at(archive_key).at("operations").at(0).at("result").at("base_position_m").at(0)=123;
    rejects([&] { validate_roof_plan_resize_derivations(counterfeit); },"archive cannot borrow cached validation after result tampering");
    counterfeit=current;
    counterfeit.extensions.at(archive_key).at("operations").at(0).at("source")["version"]=2;
    counterfeit.extensions.at(archive_key).at("operations").at(0).at("source")["roof_openings"]=Json::array();
    rejects([&] { validate_roof_plan_resize_derivations(counterfeit); },"shared archive records require schema1 individually");
    counterfeit=current;
    counterfeit.extensions.at(archive_key).at("operations").at(0).at("result")["version"]=2;
    counterfeit.extensions.at(archive_key).at("operations").at(0).at("result")["roof_openings"]=Json::array();
    rejects([&] { validate_roof_plan_resize_derivations(counterfeit); },"shared archive result frames require schema1 individually");
    auto receipted=flat();
    const Json raw{{"version",1},{"original_expression","2 m"},{"entered_unit","m"},
        {"exact_metres",{{"numerator",2},{"denominator",1},{"vendor","rational sibling"}}},
        {"vendor",{{"payload",Json::array({1.0,"opaque"})}}}};
    receipted.properties["quantity_entries"]={{"/run_m",raw},{"/vendor",{{"version",81},{"opaque",true}}}};
    const auto archived=replay_roof_plan_resize_entity(receipted,{"roof",1.2,1,{-31,19},0,shared});
    require(!archived.properties.at("quantity_entries").contains("/run_m"),"changed known receipt retires to shared archive");
    require(archived.properties.at("quantity_entries").at("/vendor").dump()==receipted.properties.at("quantity_entries").at("/vendor").dump(),"unaffected opaque receipt remains exact");
    require(archived.extensions.at(archive_key).at("operations").at(0).at("receipts").at("quantity_entries").at("/run_m").dump()==raw.dump(),"archive retains every raw receipt sibling");
    const auto remainder=roof_plan_resize_opaque_remainder(archived);
    require(remainder.at("operations").at(0).at("receipts").at("quantity_entries").at("/run_m").at("vendor")==raw.at("vendor"),"validated historical remainder retains opaque receipt authority");
    require(remainder.at("operations").at(0).at("receipts").at("quantity_entries").at("/run_m").at("exact_metres").at("vendor")=="rational sibling","historical remainder retains rational siblings");
    require(exact(replay_roof_plan_resize_entity(archived,{"roof",1,1,{900,-700},.3,shared}),archived),"receipted shared unit retains exact archive bytes");
}
RoofOpeningQuantityInput quantity(const char* text) { return {parse_quantity(text,Unit::metre),Unit::metre}; }
void monotonic_archive_and_clone() {
    const auto resized=replay_roof_plan_resize_entity(roof(),{"roof",1.2,1.1,{-31,19},0,shared});
    const auto shared_record=resized.extensions.at(archive_key).at("operations").at(0).dump();
    RoofOpeningUpsertIntent cut;
    cut.opening_id="actual-cut";cut.x=quantity("1 m");cut.y=quantity("1 m");
    cut.width=quantity("0.6 m");cut.depth=quantity("0.6 m");
    RoofOpeningEditIntent opening{"roof",{cut},{}};
    auto with_opening=replay_roof_opening_entity(resized,opening);
    require(with_opening.properties.at("version")==2,"actual opening promotes schema2");
    require(with_opening.extensions.at(archive_key).dump()==resized.extensions.at(archive_key).dump(),"opening change retains archive4 bytes");
    cut.x.reset();cut.y.reset();cut.width.reset();cut.depth.reset();
    cut.skylight=Json{{"version",1},{"frame_width_m",.05},{"curb_height_m",.1},{"glazing_thickness_m",.02}};
    opening.upserts={cut};opening.uses_skylight_schema=true;
    with_opening=replay_roof_opening_entity(with_opening,opening);
    require(with_opening.properties.at("version")==3,"actual skylight promotes schema3");
    cut.skylight.reset();cut.rotation_radians=.1;
    opening.upserts={cut};opening.uses_rotation_schema=true;
    with_opening=replay_roof_opening_entity(with_opening,opening);
    require(with_opening.properties.at("version")==4,"actual skylight angle promotes schema4");
    const auto after=replay_roof_plan_resize_entity(with_opening,{"roof",1.1,1.1,{-31,19},0});
    const auto& archive=after.extensions.at(archive_key);
    require(archive.at("version")==4 && archive.at("operations").size()==2,"legacy schema4 resize must not downgrade retained archive4");
    require(archive.at("operations").at(0).dump()==shared_record,"legacy resize preserves exact shared historical record");
    require(archive.at("operations").at(1).at("operation").at("version")==1,"later legacy resize retains v1 authority");
    require(archive.at("operations").at(1).at("receipts").at("roof_opening_input").at("actual-cut").at("width_m").dump()==
        with_opening.extensions.at("roof_opening_input").at("entries").at("actual-cut").at("width_m").dump(),"legacy record after shared archive retains historical child receipt bytes");
    validate_roof_plan_resize_derivations(after);
    const auto unit=replay_roof_plan_resize_entity(after,{"roof",1,1,{900,-700},.3});
    require(exact(unit,after),"unit after schema upgrades preserves every archive byte");
    const RoofCloneEntities map{{"roof",after}};
    const auto plan=inspect_roof_clone_plan(map,{"roof"});
    require(plan.ready(),"historical archive identities must not become live clone dependencies");
    const auto cloned=replay_roof_clone(map,plan,{{"roof","fresh-roof"},{"actual-cut","fresh-cut"}});
    const auto& clone=cloned.entities.at("fresh-roof");
    require(clone.properties.at("roof_openings").at(0).at("id")=="fresh-cut","clone retargets current child identity");
    require(clone.extensions.at(archive_key).dump()==archive.dump(),"clone preserves historical owner and opening identity bytes");
    require(clone.extensions.at(archive_key).at("operations").at(1).at("operation").at("roof_id")=="roof", "clone leaves historical operation owner intact");
    require(clone.extensions.at(archive_key).at("operations").at(1).at("source").at("roof_openings").at(0).at("id")=="actual-cut", "clone leaves historical child identity intact");
    validate_roof_plan_resize_source_entity(clone);
}
void historical_composite_gates() {
    RoofEditIntent composite;composite.roof_id="roof";
    composite.resize=RoofPlanResizeIntent{"roof",1.2,1.1,{},0};
    const auto v4=encode_roof_edit_intent(composite);
    composite.resize->anchor_mode=shared;
    rejects([&] { (void)encode_roof_edit_intent(composite); },"historical composite encoder rejects shared mode");
    composite.coordinate_world_hosted_geometry=true;
    rejects([&] { (void)encode_roof_edit_intent(composite); },"outer v6 override cannot authorize shared mode");
    for (int version : {7,8,9}) {
        RoofOpeningEditIntent opening{"roof",{}, {"actual-cut"}};
        opening.uses_skylight_schema=true;opening.uses_clone_schema=version>=8;opening.uses_rotation_schema=version==9;
        composite.openings=opening;
        rejects([&] { (void)encode_roof_edit_intent(composite); },"opening outer-version override cannot bypass encoder shared gate");
    }
    composite.openings.reset();
    const auto nested=encode_roof_plan_resize_intent(*composite.resize);
    auto bad=v4;bad["resize"]=nested;
    rejects([&] { (void)decode_roof_edit_intent(bad); },"v4 composite decoder rejects shared nested proof");
    for (int version : {6,7,8,9}) {
        bad=v4;bad["version"]=version;bad["resize"]=nested;
        bad["uniform_transform"]=nullptr;bad["coordinate_world_hosted_geometry"]=true;
        if (version != 6) {
            RoofOpeningEditIntent opening{"roof",{}, {"actual-cut"}};
            opening.uses_skylight_schema=true;opening.uses_clone_schema=version>=8;opening.uses_rotation_schema=version==9;
            bad["openings"]=encode_roof_opening_edit_intent(opening);
        }
        rejects([&] { (void)decode_roof_edit_intent(bad); },"every outer override dialect rejects shared nested proof");
    }
    composite.coordinate_world_hosted_geometry=false;composite.resize->anchor_mode=PlanAxisResizeAnchorMode::physical_edge;
    require(encode_roof_edit_intent(decode_roof_edit_intent(v4)).dump()==v4.dump(),"historical physical-edge composite remains exact");
}
} // namespace
int main() {
    testing::noninteractive_errors();
    try {
        shared_native_operator();
        historical_oracle_and_units();
        codecs_and_unit_admission();
        archive_capabilities();
        monotonic_archive_and_clone();
        historical_composite_gates();
        std::cout << "Roof shared frame resize proof tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "roof_frame_resize_proof_tests: " << error.what() << '\n';
        return 1;
    }
}
