#include "pinc_project_admission.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/area_type_presets.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/project_store.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Unsafe detached Pinc admission accepted");
}
std::vector<std::byte> bytes(const std::string& text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}
Json fixture() {
    Json edges = Json::array();
    const double points[5][2]{{0,0},{8,0},{8,8},{0,8},{0,0}};
    for (int i = 0; i < 4; ++i) edges.push_back({{"id",std::string(1,static_cast<char>('a'+i))},
        {"kind","line"},{"a",{{"x",points[i][0]},{"y",points[i][1]}}},
        {"b",{{"x",points[i+1][0]},{"y",points[i+1][1]}}}});
    Json page{{"name","Original plan"},{"calcWalls",edges},{"interiorWalls",Json::array()},
        {"assignments",{{"a|b|c|d",{{"code","GLA1"},{"name","Original room"},{"_area",999}}}}},
        {"texts",{{{"id","note"},{"text","Portable note"},{"x",2},{"y",2}}}},
        {"ghostPrevious",true},{"showPrintGuide",false}};
    return {{"format","PincSketch"},{"version","4.2"},{"currentPage",1},{"pages",{page,page}}};
}
PincWorkerProject worker(const std::vector<std::byte>& original) {
    return {parse_pinc_project(original), {}, true};
}
std::vector<PincPageFloorChoice> choices() { return {{0,7,"Reviewed floor"},{1,7,"Reviewed floor"}}; }
void detached_content_history_and_save_open() {
    const auto original = bytes(" \n" + fixture().dump() + "\n");
    auto result = preparePincProjectAdmission(worker(original),original,"source.pinc",choices(),{},"admission");
    auto snapshot = result.document.snapshot();
    require(result.current_page == 1 && result.contexts.size() == 2 &&
        result.contexts[0].calculation.floor_id == result.contexts[1].calculation.floor_id &&
        result.contexts[0].calculation.layer_id != result.contexts[1].calculation.layer_id &&
        result.contexts[0].interior.layer_id != result.contexts[1].interior.layer_id,
        "Explicit shared floor or independent page lanes lost");
    require(snapshot.assets().size() == 1 && snapshot.assets().at(result.original_asset_id).bytes == original &&
        snapshot.assets().at(result.original_asset_id).sha256 == sha256_hex(original) &&
        snapshot.assets().at(result.original_asset_id).media_type == "application/x-pincsketch",
        "Exact original source must be retained once as a hashed portable asset");
    std::size_t areas = 0, floors = 0, labels = 0,joints=0;
    for (const auto& [id,entity] : snapshot.entities()) {
        (void)id;
        require(entity.properties.dump().find("base64") == std::string::npos,
                "Original bytes must not be embedded in native entity payloads");
        if (entity.type == "property") {
            require(entity.properties.at("calculation_workflow") == "measurement" &&
                !entity.properties.contains("appraisal_facts"), "Scaffold must remain generic measurement");
            require(entity.properties.at("calculation_profile").at("version").is_number_unsigned(),
                "New native profile uses the immediate desktop reader's unsigned version contract");
            const auto& rules = entity.properties.at("calculation_profile").at("classifications");
            for (const auto& preset : area_type_presets) if (!preset.classification.empty())
                require(rules.contains(std::string(preset.classification)), "Descriptive preset absent from scaffold");
        }
        if (entity.type == "floor") {
            ++floors; require(!entity.properties.contains("elevation_m") && !entity.properties.contains("grade"),
                "Source page categories must not infer floor facts");
        }
        if(entity.type=="constraint") {
            ++joints;const auto joint=decode_constraint_entity(entity);
            require(joint.supported() && joint.constraint->relation==ConstraintRelationKind::coincident &&
                joint.constraint->bindings.size()==2 && entity.extensions.at("pinc_joint").at("basis")=="exact_source_endpoints",
                "Imported exact source endpoints have explicit native coincidence joints");
            for(const auto& binding:joint.constraint->bindings)
                require(snapshot.entities().at(binding.owner_id).properties.at("layer_id")==entity.properties.at("layer_id"),
                    "Source endpoint joints must not connect different page/collection layers");
        }
        if (entity.type == "measurement_boundary") {
            ++areas;
            require(entity.properties.at("name") == "Original room" &&
                entity.properties.at("classification") == "first_floor" &&
                measurement_linework_source_checks(snapshot.entities()).at(entity.id).current,
                "Imported areas must retain editable native lineage and names");
            require(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(entity)))-5.94579456)<1e-10,
                "Source cached area must not become calculated authority");
        }
        if (entity.type == kAnnotationEntityType) {
            const auto state = decode_annotation_entity(entity); labels += state.labels.size();
            if(entity.properties.contains("layer_id"))require(std::any_of(state.overrides.begin(),state.overrides.end(),[](const auto& role) {
                return role.target_kind == "area_calculation";
            }), "Area calculation callout must reference a live native area role");
        }
    }
    require(areas == 2 && floors == 1 && labels == 2 && result.document.is_editable(), "Native content incomplete");
    require(joints==8,"Two coincident square outlines retain four exact joints per independent page");
    require(result.sheets.sheet_order().size() == 2 && result.sheets.sheet_order()[0] == "admission:p0:sheet" &&
        result.sheets.sheet_order()[1] == "admission:p1:sheet", "Source page order must control native sheet order");
    for (std::size_t p=0;p<result.sheets.views().size();++p) {
        const auto& view=result.sheets.views()[p];
        const auto& sheet=result.sheets.sheets()[p];
        require(sheet.viewports.front().bounds.y_mm+sheet.viewports.front().bounds.height_mm <= sheet.height_mm-36,
            "Generated portrait viewport must leave clear space for the title block and footer gap");
        require(view.restrict_to_objects && !view.object_ids.empty(),
            "Page view must restrict geometry, area and annotations explicitly");
        for(const auto& id:view.object_ids) {
            const auto& entity=snapshot.entities().at(id);
            const auto layer=entity.properties.at("layer_id").get<std::string>();
            require(layer==result.contexts[p].calculation.layer_id || layer==result.contexts[p].interior.layer_id,
                "Shared-floor pages must not leak objects into each other's restricted output view");
        }
    }
    const auto& metadata=snapshot.entities().at(result.sheet_view_entity_id).extensions.at("pinc_import");
    require(metadata.at("current_page")==1&&metadata.at("pages")[0].at("ghost_previous")==true&&
        metadata.at("pages")[0].at("show_print_guide")==false&&
        metadata.at("pages")[1].at("view_id")=="admission:p1:view"&&
        metadata.at("pages")[1].at("sheet_id")=="admission:p1:sheet",
        "Page selection, source flags and native page adapter identities must remain portable");
    result.document.undo(result.document.revision());
    const auto undone=result.document.snapshot();
    require(undone.assets().empty() &&
        std::none_of(undone.entities().begin(),undone.entities().end(),
            [](const auto& item) { return item.second.type == "measurement_boundary"; }),
        "One Undo must remove all import entities and assets");
    result.document.redo(result.document.revision());
    require(result.document.snapshot().entities() == snapshot.entities() &&
        result.document.snapshot().assets() == snapshot.assets(), "One Redo must restore exact imported content");
    const auto path = std::filesystem::temp_directory_path() / ("pinc-admission-"+make_stable_id()+".bldproj");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    (void)ProjectStore::save(path,result.document.snapshot());
    const auto reopened = ProjectStore::load(path).document.snapshot();
    require(reopened.entities() == snapshot.entities() && reopened.assets() == snapshot.assets() &&
        decode_sheet_view_entity(reopened.entities().at(result.sheet_view_entity_id)).sheet_order() == result.sheets.sheet_order(),
        "Save/Open must preserve native import and exact original provenance");
}
void attestation_and_exact_source_are_required() {
    auto original = bytes(fixture().dump()); auto input = worker(original);
    input.isolation_controls_attested = false;
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",choices(),{},"bad");});
    input = worker(original); input.project.pages[0].calculation_segments[0].geometry.end.x += 1;
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",choices(),{},"bad");});
    input = worker(original); input.project.pages[0].name = "Forged title";
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",choices(),{},"bad");});
    input = worker(original);
    rejects([&]{(void)preparePincProjectAdmission(input,original,"../source.pinc",choices(),{},"bad");});
    rejects([&]{(void)preparePincProjectAdmission(input,original,"CON.pinc",choices(),{},"bad");});
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",{}, {},"bad");});
    auto contradictory = choices(); contradictory[1].floor_name = "Different floor";
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",contradictory,{},"bad");});
}
void undefined_and_unknown_categories_do_not_infer_authority() {
    auto source=fixture();source["pages"][0]["assignments"]["a|b|c|d"]["code"]="UND";
    source["pages"][1]["assignments"]["a|b|c|d"]["code"]="future-code";
    const auto original=bytes(source.dump());
    auto result=preparePincProjectAdmission(worker(original),original,"source.pinc",choices(),{},"categories");
    const auto snapshot=result.document.snapshot();
    std::size_t areas=0;
    for(const auto& [id,entity]:snapshot.entities()) {
        (void)id;if(entity.type!="measurement_boundary")continue;
        ++areas;require(entity.properties.at("classification")=="non_calculated"&&
            !entity.properties.contains("appraisal_facts")&&!entity.properties.contains("deduction_ids"),
            "Undefined source category must remain explicitly non-calculated without appraisal authority");
    }
    require(areas==1&&std::any_of(result.diagnostics.begin(),result.diagnostics.end(),[](const auto& diagnostic) {
        return diagnostic.code=="assignment_reference_only";
    }),"Unknown category must remain a visible source reference");
    const auto& saved=snapshot.entities().at(result.sheet_view_entity_id).extensions.at("pinc_import");
    require(!saved.at("diagnostics").empty(),"Unresolved import diagnostics must survive Save/Open");
}
void decoded_underlay_orientation_and_budget() {
    auto source = fixture(); source["pages"].erase(1); source["currentPage"] = 0;
    QImage pixels(1,2,QImage::Format_RGBA8888); pixels.setPixelColor(0,0,QColor(Qt::red)); pixels.setPixelColor(0,1,QColor(Qt::blue));
    QByteArray png; QBuffer buffer(&png); require(buffer.open(QIODevice::WriteOnly)&&pixels.save(&buffer,"PNG"), "Test PNG encode failed");
    source["pages"][0]["underlay"] = {{"data","data:image/png;base64,"+png.toBase64().toStdString()},
        {"x",2},{"y",3},{"width",4},{"opacity",.5}};
    auto original = bytes(source.dump()); auto input = worker(original);
    std::vector<std::byte> frame(40); std::memcpy(frame.data(),"PSIR0002",8);
    auto* header = reinterpret_cast<uchar*>(frame.data());
    qToLittleEndian<quint32>(1,header+8);qToLittleEndian<quint32>(2,header+12);qToLittleEndian<quint32>(1,header+16);
    std::memcpy(header+32,pixels.constScanLine(0),4);std::memcpy(header+36,pixels.constScanLine(1),4);
    input.underlays.push_back({0,validateReferencePixelFrame(frame,png,"png")});
    const std::vector<PincPageFloorChoice> floor{{0,0,"Reviewed floor"}};
    auto result = preparePincProjectAdmission(input,original,"source.pinc",floor,{},"raster");
    const auto snapshot = result.document.snapshot();
    const auto& reference = snapshot.entities().at(result.page_reference_ids[0]);
    const auto& descriptor = *input.project.pages[0].underlay;
    require(reference.properties.at("flip_vertical") == true && reference.properties.at("metres_per_source_unit") == descriptor.width_metres &&
        !reference.properties.contains("calibration"), "Known underlay width must not claim measured calibration");
    const auto position = reference.properties.at("position_m");
    require(std::abs(position[0].get<double>()-(descriptor.top_left_metres.x+descriptor.width_metres/2))<1e-12 &&
        std::abs(position[1].get<double>()-(descriptor.top_left_metres.y-descriptor.width_metres))<1e-12,
        "Underlay Cartesian centre must preserve source top-left placement");
    const auto& asset = snapshot.assets().at(reference.properties.at("render_asset_id").get<std::string>());
    const auto preview = QImage::fromData(reinterpret_cast<const uchar*>(asset.bytes.data()),static_cast<int>(asset.bytes.size()),"PNG");
    require(preview.pixelColor(0,0)==QColor(Qt::red)&&preview.pixelColor(0,1)==QColor(Qt::blue),
        "PNG preview must preserve decoded red-top/blue-bottom pixels before the Cartesian display flip");
    input.underlays.push_back(input.underlays.front());
    rejects([&]{(void)preparePincProjectAdmission(input,original,"source.pinc",floor,{},"bad");});
}
void combined_pair_budget_precedes_native_work() {
    auto source=fixture();source["pages"].erase(1);source["currentPage"]=0;
    auto& page=source["pages"][0];page["calcWalls"]=Json::array();page["assignments"]=Json::object();
    for(int i=0;i<240;++i)page["calcWalls"].push_back({{"id","edge"+std::to_string(i)},{"kind","line"},
        {"a",{{"x",0},{"y",i*2}}},{"b",{{"x",1},{"y",i*2}}}});
    const auto original=bytes(source.dump());const auto input=worker(original);
    const std::vector<PincPageFloorChoice> reviewed{{0,0,"Floor"}};
    try {
        (void)preparePincProjectAdmission(input,original,"source.pinc",reviewed,{},"budget");
    } catch(const std::invalid_argument& error) {
        require(std::string_view(error.what()).find("combined geometry verification pair budget")!=std::string_view::npos,
            "Combined budget must refuse before graph admission starts");return;
    }
    throw std::runtime_error("Combined nine-pass pair budget was reset between modules");
}
void exact_joint_scope_and_three_way_star() {
    const auto line=[](const char* id,double ax,double ay,double bx,double by) {
        return Json{{"id",id},{"kind","line"},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}}};
    };
    const Json source{{"format","PincSketch"},{"version","4.2"},{"currentPage",0},{"pages",Json::array({
        {{"name","Joint scope"},{"calcWalls",Json::array({line("calc",0,0,5,0)})},
            {"interiorWalls",Json::array({line("one",0,0,1,0),line("two",0,0,0,1),
                line("three",0,0,-1,0),line("near",.0001,0,2,2)})}}})}};
    const auto original=bytes(source.dump());const auto input=worker(original);
    const std::vector<PincPageFloorChoice> reviewed{{0,0,"Joint floor"}};
    const auto imported=preparePincProjectAdmission(input,original,"joints.pinc",reviewed,{},"joints");
    const auto snapshot=imported.document.snapshot();std::size_t count=0;
    for(const auto& [id,entity]:snapshot.entities()) {
        (void)id;if(entity.type!="constraint")continue;++count;
        require(entity.properties.at("layer_id")==imported.contexts[0].interior.layer_id,
            "Exact same-coordinate calculation and interior endpoints remain in independent lanes");
        const auto decoded=decode_constraint_entity(entity);
        require(decoded.supported(),"Exact imported joint retains native coincidence semantics");
        for(const auto& binding:decoded.constraint->bindings)
            require(binding.owner_id.find(":interior:3:stroke")==std::string::npos,
                "Near-equal source endpoints must not be joined by a tolerance");
    }
    require(count==2,"Three exactly shared endpoints create a linear two-relation star");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QCoreApplication application(argc,argv);
    try { detached_content_history_and_save_open();attestation_and_exact_source_are_required();
        undefined_and_unknown_categories_do_not_infer_authority();decoded_underlay_orientation_and_budget();
        combined_pair_budget_precedes_native_work();exact_joint_scope_and_three_way_star();
        std::cout << "pinc_project_admission_tests passed\n";return 0;
    } catch(const std::exception& error){std::cerr << "pinc_project_admission_tests: " << error.what() << '\n';return 1;}
}
