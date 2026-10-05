#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/physical_wall_room.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
QString capture_directory;

void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T& control(MainWindow& window,const char* name) {
    auto* value=window.findChild<T*>(QString::fromLatin1(name));
    require(value,"native callout control exists");return *value;
}
PlanCanvas& canvas(MainWindow& window,const char* name="measurementPlanCanvas") {
    auto* value=dynamic_cast<PlanCanvas*>(&control<QWidget>(window,name));
    require(value,"native callout canvas exists");return *value;
}
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1400,950);window.show();
    window.setMetricUnits(true);QApplication::processEvents();
    canvas(window).setOverviewMapEnabled(false);canvas(window).fitView();
}
std::vector<CanvasLabel> labels(PlanCanvas& drawing,const QString& id) {
    std::vector<CanvasLabel> result;
    for (const auto& label:drawing.labels()) if (label.id==id && label.avoid_components) result.push_back(label);
    return result;
}
CanvasLabel callout(PlanCanvas& drawing,const QString& id,const QString& role) {
    for (const auto& value:labels(drawing,id)) if (value.callout_role==role) return value;
    throw std::runtime_error("Expected native callout missing: "+id.toStdString()+" / "+role.toStdString());
}
PresentationOverride presentation(const DocumentSnapshot& snapshot,const QString& id,const char* role) {
    for (const auto& [owner_id,entity]:snapshot.entities()) {
        (void)owner_id;if (entity.type!=kAnnotationEntityType) continue;
        for (const auto& value:decode_annotation_entity(entity).overrides)
            if (value.target_id==id.toStdString() && value.target_kind==role) return value;
    }
    throw std::runtime_error("Persisted callout presentation missing");
}
void capture(MainWindow& window,const char* filename) {
    if (capture_directory.isEmpty()) return;
    QApplication::processEvents();
    require(QDir().mkpath(capture_directory) && window.grab().save(QDir(capture_directory).filePath(QString::fromLatin1(filename))),
        "actual offscreen workspace capture saves");
}
Boundary square() {return {{{0,0},{4,0},0},{{4,0},{4,4},0},{{4,4},{0,4},0},{{0,4},{0,0},0}};}
void name_area(MainWindow& window,const QString& id,const char* name) {
    auto source=window.document().snapshot();auto entity=source.entities().at(id.toStdString());
    entity.properties["name"]=name;
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(entity)}, {},"Name test area"});
    require(window.selectEntity(id),"select named native area");
}

void independent_live_callouts_and_reopen() {
    QTemporaryDir files;require(files.isValid(),"callout fixture has temporary storage");
    MainWindow window({},nullptr,files.filePath("text-library.json"));display(window);
    const auto id=window.createBoundary(square(),"living");
    require(!id.isEmpty() && window.selectEntity(id),"create qualified area owner");
    auto& workflow=control<QComboBox>(window,"calculationWorkflow");
    workflow.setCurrentIndex(workflow.findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),
        "fixture declares actual appraisal eligibility");
    name_area(window,id,"Studio");
    auto legacy=labels(canvas(window),id);
    require(legacy.size()==1 && legacy.front().callout_role.isEmpty() && legacy.front().text.contains("Studio") &&
        legacy.front().text.contains("16.00"),"legacy name and live quantity remain combined until explicit adoption");
    const auto original=window.document().snapshot();
    control<QPushButton>(window,"separateAreaCallouts").click();
    auto separated=window.document().snapshot();
    require(separated.revision()==original.revision()+1 && separated.entities().at(id.toStdString())==original.entities().at(id.toStdString()),
        "native separation is one presentation-only command");
    require(labels(canvas(window),id).size()==2 && callout(canvas(window),id,"area_name").text=="Studio" &&
        callout(canvas(window),id,"area_calculation").text.contains("16.00"),"separation creates two live labels with the same native owner");
    require(presentation(separated,id,"area_name").inherit_appearance && presentation(separated,id,"area_calculation").inherit_appearance,
        "new callouts inherit theme readability");
    require(window.separateSelectedAreaCallouts() && window.document().revision()==separated.revision(),"repeated adoption is idempotent");
    window.setWorkspaceTheme(WorkspaceTheme::dark);
    require(!callout(canvas(window),id,"area_name").color.isValid() && !callout(canvas(window),id,"area_calculation").color.isValid(),
        "default separated labels retain the dark-theme color sentinel");
    capture(window,"area-callouts-dark.png");window.setWorkspaceTheme(WorkspaceTheme::light);

    require(window.editSelectedAreaCallout("area_name",".8","3.3","3.5","#2455AA","left",true,"0"),"edit independent name appearance");
    const auto name_before=callout(canvas(window),id,"area_name");
    require(window.editSelectedAreaCallout("area_calculation","3.2",".8","4.5","#AA3322","right",true,"15"),"edit independent calculation appearance");
    const auto name_after=callout(canvas(window),id,"area_name");
    const auto calculation=callout(canvas(window),id,"area_calculation");
    require(name_after.text_alignment=="left" && name_after.color==QColor("#2455AA") && name_after.paper_height_mm==3.5 &&
        name_after.position.x==name_before.position.x && name_after.position.y==name_before.position.y,
        "calculation style does not change name style or position");
    require(calculation.text_alignment=="right" && calculation.color==QColor("#AA3322") && calculation.paper_height_mm==4.5 &&
        std::abs(calculation.rotation_radians-std::numbers::pi/12)<1e-10,"calculation carries independent alignment, size, color and rotation");
    require(window.setSelectedAreaCalloutRole("area_name") && window.setSelectedPlanLabelPosition({1.1,3.1}),"role-aware placement moves the name");
    const auto after_place=window.document().snapshot();
    const auto calc_before_hide=presentation(after_place,id,"area_calculation");
    require(window.editSelectedAreaCallout("area_name","1.1","3.1","3.5","#2455AA","left",false,"0"),"hide only name callout");
    require(labels(canvas(window),id).size()==1 && callout(canvas(window),id,"area_calculation").text.contains("16.00") &&
        std::any_of(canvas(window).entities().begin(),canvas(window).entities().end(),[&](const auto& value){return value.id==id;}),
        "hiding a callout retains the quantity and area geometry");
    require(window.undoCommand() && labels(canvas(window),id).size()==2 && window.redoCommand() && labels(canvas(window),id).size()==1,
        "independent callout visibility follows native Undo and Redo");
    require(window.undoCommand(),"restore visible name for the live geometry edit");
    require(window.editSelectedAreaCallout("area_calculation","3.2",".8","4.5","#AA3322","right",false,"15") &&
        labels(canvas(window),id).size()==1 && callout(canvas(window),id,"area_name").text=="Studio" && window.undoCommand(),
        "calculation visibility is independent and Undo restores its live value");
    const auto calc_after_hide=presentation(window.document().snapshot(),id,"area_calculation").plan_label_offset;
    require(calc_after_hide && calc_before_hide.plan_label_offset &&
        calc_after_hide->x==calc_before_hide.plan_label_offset->x && calc_after_hide->y==calc_before_hide.plan_label_offset->y,
        "name visibility does not alter calculation placement");
    require(window.setSelectedAreaCalloutRole("area_calculation") && window.resetSelectedPlanLabelPlacement(),
        "Automatic clears only the calculation's manual offset");
    const auto before_transform=window.document().snapshot();
    const auto original_name=presentation(before_transform,id,"area_name");
    const auto original_calc=presentation(before_transform,id,"area_calculation");
    require(original_name.plan_label_offset && !original_calc.plan_label_offset && original_calc.plan_label_rotation_radians,
        "closed-area transform fixture includes a manual angle without an offset");
    require(window.transformSelectedBoundary("90",false,false,"0","0",false),
        "actual closed-area rotation includes the owned role records");
    const auto rotated_name=presentation(window.document().snapshot(),id,"area_name");
    const auto rotated_calc=presentation(window.document().snapshot(),id,"area_calculation");
    require(rotated_name.plan_label_offset &&
        std::abs(rotated_name.plan_label_offset->x+original_name.plan_label_offset->y)<1e-9 &&
        std::abs(rotated_name.plan_label_offset->y-original_name.plan_label_offset->x)<1e-9 &&
        !rotated_calc.plan_label_offset && rotated_calc.plan_label_rotation_radians &&
        std::abs(*rotated_calc.plan_label_rotation_radians-7*std::numbers::pi/12)<1e-9,
        "rotation transforms the name offset and offset-free calculation angle once");
    require(window.undoCommand() && window.document().snapshot().entities()==before_transform.entities(),
        "rotation Undo restores exact closed geometry and presentation records");
    require(window.transformSelectedBoundary("0",true,false,"0","0",false),
        "actual closed-area reflection includes the owned role records");
    const auto reflected_name=presentation(window.document().snapshot(),id,"area_name");
    const auto reflected_calc=presentation(window.document().snapshot(),id,"area_calculation");
    require(reflected_name.plan_label_offset &&
        std::abs(reflected_name.plan_label_offset->x+original_name.plan_label_offset->x)<1e-9 &&
        std::abs(reflected_name.plan_label_offset->y-original_name.plan_label_offset->y)<1e-9 &&
        !reflected_calc.plan_label_offset && reflected_calc.plan_label_rotation_radians &&
        std::abs(*reflected_calc.plan_label_rotation_radians-11*std::numbers::pi/12)<1e-9,
        "reflection transforms role offset and offset-free authored angle once");
    require(window.undoCommand() && window.document().snapshot().entities()==before_transform.entities(),
        "reflection Undo restores the exact owner and role records");
    auto identified=decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
    const auto vertex=std::find_if(identified.segments.begin(),identified.segments.end(),[](const auto& edge){
        return edge.segment.start.x==4 && edge.segment.start.y==4;});
    require(vertex!=identified.segments.end() && window.moveSelectedBoundaryVertex(QString::fromStdString(vertex->start_vertex_id),{6,4}),
        "native stable-ID geometry edit changes actual area");
    require(callout(canvas(window),id,"area_calculation").text.contains("20.00") && callout(canvas(window),id,"area_name").text=="Studio",
        "calculation updates from geometry while name remains independent");
    capture(window,"area-callouts-light.png");
    const auto expected=window.document().snapshot();const auto path=files.filePath("callouts.bldproj");
    require(window.saveProjectAs(path) && window.createNewProject(),"save callouts and release project ownership");
    MainWindow reopened({},nullptr,files.filePath("text-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().snapshot().entities()==expected.entities() && reopened.selectEntity(id),
        "native reopen retains exact callout records and source geometry");
    require(callout(canvas(reopened),id,"area_calculation").text.contains("20.00") &&
        callout(canvas(reopened),id,"area_name").text_alignment=="left","reopen rederives quantity and restores independent name alignment");
    require(reopened.editArchitecturalViewPresentation("view-plan","1.2","100",".5",".25",true,"solid","1","medium",
        id,std::nullopt,true),"named sheet view can filter by native area owner");
    reopened.setWorkspace(Workspace::architectural);
    auto& views=control<QComboBox>(reopened,"architecturalView");
    const auto index=views.findData(QStringLiteral("view-plan"),Qt::UserRole+1);
    require(index>=0,"named owner-filtered plan exists");views.setCurrentIndex(index);QApplication::processEvents();
    require(labels(canvas(reopened,"architecturalPlanCanvas"),id).size()==2,
        "owner-only named sheet filtering retains both callout roles");
    reopened.setWorkspace(Workspace::measurement);
    const auto unrelated=reopened.createStraightWall({8,0},{8,3});
    require(!unrelated.isEmpty() && reopened.selectEntity(id),"conflict fixture retains unrelated real geometry");
    const auto before_conflict=reopened.document().snapshot();
    AnnotationState duplicate;duplicate.overrides.push_back(presentation(before_conflict,id,"area_name"));
    reopened.document().apply(ApplyEntityChanges{before_conflict.revision(),
        {EntityChange::upsert(make_annotation_entity("conflicting-callouts",duplicate))}, {},"Seed individually valid duplicate providers"});
    require(reopened.selectEntity(id) && labels(canvas(reopened),id).size()==1 &&
        callout(canvas(reopened),id,"area_calculation").text.contains("20.00") &&
        control<QLabel>(reopened,"areaCalloutDiagnostic").text().contains("Conflicting"),
        "cross-provider conflict withholds only its affected role and diagnoses it");
    require(std::any_of(canvas(reopened).entities().begin(),canvas(reopened).entities().end(),[&](const auto& value){
        return value.id==unrelated;}),"duplicate callout providers do not suppress unrelated geometry");
    const auto conflicted=reopened.document().snapshot();
    require(!reopened.editSelectedAreaCallout("area_name","1","3","3.5","#2455AA","left",true,"0") &&
        reopened.document().revision()==conflicted.revision() && reopened.document().snapshot().entities()==conflicted.entities(),
        "editing a conflicting role refuses without choosing an arbitrary provider or mutating history");
    const auto conflict_path=files.filePath("conflicting-callouts.bldproj");
    require(reopened.saveProjectAs(conflict_path) && reopened.createNewProject(),"valid conflict project saves without provider substitution");
    MainWindow conflict_open({},nullptr,files.filePath("text-library.json"));display(conflict_open);
    require(conflict_open.openProject(conflict_path) && conflict_open.selectEntity(id) && labels(canvas(conflict_open),id).size()==1 &&
        callout(canvas(conflict_open),id,"area_calculation").text.contains("20.00"),"actual native open renders valid conflicting providers without a global exception");
    capture(conflict_open,"area-callouts-conflict.png");
}

Entity wall(const char* id,Vec2 start,Vec2 end) {
    return {id,"wall",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"baseline",{{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"sweep_radians",0}}},
        {"thickness_m",.2},{"height_m",3},{"elevation_m",0}},false};
}
void stale_physical_calculation_is_withheld() {
    auto document=std::make_shared<Document>(Document::create({
        {"p","property",{{"name","Physical callout fixture"}},false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"}},false},wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})}));
    MainWindow window(document);display(window);
    require(window.setActiveLayer("l") && window.selectEntity("bottom"),"physical fixture has active source context");
    const auto rooms=window.detectRoomBoundariesFromExistingWalls("office");
    require(rooms.size()==1 && window.selectEntity(rooms.front()),"detect actual clear physical room");
    const auto id=rooms.front();name_area(window,id,"Office");
    require(window.separateSelectedAreaCallouts() && callout(canvas(window),id,"area_calculation").text.contains("10.64"),
        "physical calculation comes from current clear geometry");
    auto source=document->snapshot();auto changed=source.entities().at("right");changed.properties["thickness_m"]=.4;
    document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(changed)}, {},"Change physical room source"});
    require(window.selectEntity("right"),"refresh native projection after wall source edit");
    require(!physical_wall_room_checks(document->snapshot()).at(id.toStdString()).current &&
        std::none_of(canvas(window).labels().begin(),canvas(window).labels().end(),[&](const auto& value){
            return value.id==id && value.callout_role=="area_calculation";}),"stale physical sources withhold the claimed live clear total");
}
} // namespace

int main(int argc,char** argv) {
    testing::noninteractive_errors();QApplication app(argc,argv);
    capture_directory=argc>1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled font loads for actual native captures");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        independent_live_callouts_and_reopen();stale_physical_calculation_is_withheld();
        std::cout<<"area_callout_desktop_tests passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"area_callout_desktop_tests: "<<error.what()<<'\n';return 1;}
}
