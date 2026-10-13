#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/physical_wall_room.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QAction>
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

CanvasLabel dimension_label(PlanCanvas& drawing,const QString& id) {
    for (const auto& label:drawing.labels()) if (label.id==id) return label;
    throw std::runtime_error("Expected fixed dimension label missing");
}
QRectF output_label_bounds(const CanvasLabel& label,QFont font) {
    PlanCanvas output;output.setFont(font);output.setLabels({label});
    const auto recording=output.recordSketchContent(80.0);
    require(recording.has_value(),"actual vector output records the retained label");
    return recording->ink_bounds.translated(recording->model_center.x*80.0,
        -recording->model_center.y*80.0);
}
void fixed_dimensions_are_area_label_obstacles() {
    for (const auto angle : {QStringLiteral("0"),QStringLiteral("45")}) {
        QTemporaryDir files;require(files.isValid(),"label collision fixture has isolated storage");
        MainWindow window({},nullptr,files.filePath("collision-text.json"));display(window);
        const auto area=window.createBoundary(square(),"living");
        require(!area.isEmpty() && window.selectEntity(area),"collision fixture creates actual area");
        name_area(window,area,"Authored Room / ADU");
        const auto unobstructed=labels(canvas(window),area);
        require(unobstructed.size()==1,"fixture starts with one automatic combined area label");
        const auto anchor=unobstructed.front().position;
        const auto owner=decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
        const auto dimension=window.createLengthDimension(area,
            QString::fromStdString(owner.segments.front().segment_id),anchor);
        require(!dimension.isEmpty() && window.editBoundaryDimension(dimension,
            QString::number(anchor.x,'g',17)+" m",QString::number(anchor.y,'g',17)+" m",
            "4.2","#713ba2",true,false,true,angle),"fixed dimension text occupies area centroid");
        const auto source=window.document().snapshot();
        require(window.selectEntity(area),"refresh projection without changing document");
        QApplication::processEvents();
        auto& drawing=canvas(window);
        const auto fixed=dimension_label(drawing,dimension);
        const auto automatic=labels(drawing,area);
        require(automatic.size()==1 && automatic.front().text.contains("Authored Room / ADU"),
            "authored area name retains its original spelling and acronyms");
        require(fixed.position.x==anchor.x && fixed.position.y==anchor.y && !fixed.automatic_linear_placement,
            "collision avoidance keeps authored dimension placement exact");
        const auto area_bounds=drawing.labelScreenBounds({area,{},automatic.front().selection_type});
        const auto dimension_bounds=drawing.labelScreenBounds({dimension,{},fixed.selection_type});
        require(area_bounds && dimension_bounds && !area_bounds->intersects(*dimension_bounds),
            "automatic area text avoids the painted fixed dimension including rotation");
        require(!output_label_bounds(automatic.front(),drawing.font()).intersects(output_label_bounds(fixed,drawing.font())),
            "committed vector output retains separated area and dimension ink");
        require(window.document().snapshot().entities()==source.entities() && window.document().revision()==source.revision(),
            "derived placement preserves exact geometry, dimension state and history");
        require(window.editBoundaryDimension(dimension,QString::number(anchor.x,'g',17)+" m",
            QString::number(anchor.y,'g',17)+" m","4.2","#713ba2",true,false,false,angle),
            "hide the fixed dimension through its public presentation API");
        require(window.selectEntity(area),"refresh hidden dimension projection");
        const auto hidden=window.document().snapshot();
        const auto restored=labels(drawing,area);
        require(restored.size()==1 && restored.front().position.x==anchor.x && restored.front().position.y==anchor.y,
            "hidden dimension does not reserve an invisible area-label obstacle");
        require(window.document().snapshot().entities()==hidden.entities(),"hidden-label projection is presentation only");
        require(window.undoCommand() && window.selectEntity(area) && window.setSelectedPlanLabelPosition(anchor),
            "explicit manual area placement can overlap the restored dimension");
        const auto manual=window.document().snapshot();
        require(window.selectEntity(area),"refresh manual area placement");
        const auto pinned=labels(drawing,area);
        require(pinned.size()==1 && pinned.front().plan_label_offset &&
            pinned.front().position.x==anchor.x && pinned.front().position.y==anchor.y &&
            dimension_label(drawing,dimension).position.x==anchor.x && dimension_label(drawing,dimension).position.y==anchor.y,
            "manual area and fixed dimension anchors both remain authored even when overlapping");
        require(window.document().snapshot().entities()==manual.entities() && window.document().revision()==manual.revision(),
            "manual placement refresh changes no document geometry or dimension state");
    }
}

void site_fixed_dimensions_in_other_frames_are_area_label_obstacles() {
    // Omitting fixed dimensions from the Site obstacle index, or comparing
    // their untransformed source coordinates, must leave this ink overlapping.
    for (const auto frame_rotation : {0.0,std::numbers::pi/2})
    for (const auto angle : {QStringLiteral("0"),QStringLiteral("45")}) {
        QTemporaryDir files;require(files.isValid(),"Site label fixture has isolated storage");
        MainWindow window({},nullptr,files.filePath("site-collision-text.json"));display(window);
        const auto area=window.createBoundary(square(),"living");
        require(!area.isEmpty() && window.selectEntity(area),"Site fixture creates area owner");
        name_area(window,area,"Authored Room / ADU");
        const auto building=window.createBuilding("property-1","Dimension building");
        const auto floor=window.createFloor(building,"Ground");
        const auto layer=window.createLayer(floor,"Dimensions");
        require(window.setActiveLayer(layer),"dimension uses a separate Site source frame");
        auto remote=square();
        for (auto& edge:remote) {edge.start.x+=20;edge.end.x+=20;}
        const auto measured=window.createBoundary(remote,"living");
        require(!measured.isEmpty(),"Site dimension has its own analytical owner");
        const auto owner=decode_identified_boundary_entity(window.document().snapshot().entities().at(measured.toStdString()));
        const auto dimension=window.createLengthDimension(measured,
            QString::fromStdString(owner.segments.front().segment_id),{22,2});
        require(!dimension.isEmpty() && window.editBoundaryDimension(dimension,"22 m","2 m",
            "4.2","#713ba2",true,false,true,angle),"remote fixed dimension has authored typography");
        auto source=window.document().snapshot();auto placed=source.entities().at(building.toStdString());
        const auto frame_offset=frame_rotation==0.0 ? Vec2{-20,0} : Vec2{4,-20};
        placed.properties["site_placement"]={{"version",1},
            {"translation_m",{frame_offset.x,frame_offset.y,0}},{"rotation_radians",frame_rotation}};
        auto framed=source.entities().at(measured.toStdString());
        framed.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        window.document().apply(ApplyEntityChanges{source.revision(),
            {EntityChange::upsert(placed),EntityChange::upsert(framed)}, {},"Place Site dimension frame"});
        require(window.selectEntity(measured) && window.setSelectedPlanLabelPosition({25,7}),
            "remote area callout is pinned away from the fixed dimension");
        require(window.setActiveLayer("layer-1"),"return to visible area source layer");
        window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();
        require(window.selectEntity(area),"publish actual Site area and dimension");
        QApplication::processEvents();
        auto& drawing=canvas(window,"architecturalPlanCanvas");
        const auto automatic=labels(drawing,area);const auto fixed=dimension_label(drawing,dimension);
        require(automatic.size()==1 && std::abs(fixed.position.x-2)<1e-9 && std::abs(fixed.position.y-2)<1e-9,
            "Site presents the remote fixed dimension at the local area centroid");
        const auto area_bounds=drawing.labelScreenBounds({area,{},automatic.front().selection_type});
        const auto fixed_bounds=drawing.labelScreenBounds({dimension,{},fixed.selection_type});
        require(area_bounds && fixed_bounds && !area_bounds->intersects(*fixed_bounds),
            "Site automatic area text avoids fixed dimension ink from another frame");
        require(!output_label_bounds(automatic.front(),drawing.font()).intersects(output_label_bounds(fixed,drawing.font())),
            "Site vector output separates fixed dimension and area ink");
        const auto shown=window.document().snapshot();
        require(window.selectEntity(area) && window.document().snapshot().entities()==shown.entities() &&
            window.document().revision()==shown.revision(),"Site layout preserves exact authored source and history");
        require(window.editBoundaryDimension(dimension,"22 m","2 m","4.2","#713ba2",true,false,false,angle) &&
            window.selectEntity(area),"hide remote fixed dimension through its public presentation API");
        const auto restored=labels(drawing,area);
        require(restored.size()==1 && restored.front().position.x==2 && restored.front().position.y==2,
            "hidden remote dimension reserves no Site obstacle");
    }
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
        legacy.front().text.contains("16.0"),"legacy name and live quantity remain combined until explicit adoption");
    const auto original=window.document().snapshot();
    control<QPushButton>(window,"separateAreaCallouts").click();
    auto separated=window.document().snapshot();
    require(separated.revision()==original.revision()+1 && separated.entities().at(id.toStdString())==original.entities().at(id.toStdString()),
        "native separation is one presentation-only command");
    require(labels(canvas(window),id).size()==2 && callout(canvas(window),id,"area_name").text=="Studio" &&
        callout(canvas(window),id,"area_calculation").text.contains("16.0"),"separation creates two live labels with the same native owner");
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
    require(labels(canvas(window),id).size()==1 && callout(canvas(window),id,"area_calculation").text.contains("16.0") &&
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
        // Separated roles keep their absolute placement through the owner's
        // rigid transform (format v69): the generated (0,-.18) default turns
        // with the area into an explicit (.18,0) offset, rotated once.
        rotated_calc.plan_label_offset &&
        std::abs(rotated_calc.plan_label_offset->x-.18)<1e-9 && std::abs(rotated_calc.plan_label_offset->y)<1e-9 &&
        rotated_calc.plan_label_rotation_radians &&
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
    require(callout(canvas(window),id,"area_calculation").text.contains("20.0") && callout(canvas(window),id,"area_name").text=="Studio",
        "calculation updates from geometry while name remains independent");
    capture(window,"area-callouts-light.png");
    const auto expected=window.document().snapshot();const auto path=files.filePath("callouts.bldproj");
    require(window.saveProjectAs(path) && window.createNewProject(),"save callouts and release project ownership");
    MainWindow reopened({},nullptr,files.filePath("text-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().snapshot().entities()==expected.entities() && reopened.selectEntity(id),
        "native reopen retains exact callout records and source geometry");
    require(callout(canvas(reopened),id,"area_calculation").text.contains("20.0") &&
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
        callout(canvas(reopened),id,"area_calculation").text.contains("20.0") &&
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
        callout(canvas(conflict_open),id,"area_calculation").text.contains("20.0"),"actual native open renders valid conflicting providers without a global exception");
    capture(conflict_open,"area-callouts-conflict.png");
}

Entity wall(const char* id,Vec2 start,Vec2 end) {
    return {id,"wall",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"baseline",{{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"sweep_radians",0}}},
        {"thickness_m",.2},{"height_m",3},{"elevation_m",0}},false};
}
void generic_measurement_callouts_do_not_claim_appraisal() {
    QTemporaryDir files;require(files.isValid(),"generic callout fixture has private storage");
    MainWindow window({},nullptr,files.filePath("text-library.json"));display(window);
    const auto id=window.createBoundary(square(),"living");
    require(!id.isEmpty() && window.selectEntity(id),"generic area is created");
    name_area(window,id,"studio-West_1");
    require(window.separateSelectedAreaCallouts(),"generic name and calculated value can be separated");
    require(callout(canvas(window),id,"area_name").text=="studio-West_1" &&
        callout(canvas(window),id,"area_calculation").text.contains("16.0"),"authored name is exact and measurement area comes from geometry");
    require(window.editSelectedFactor("1/2") && callout(canvas(window),id,"area_calculation").text.contains("8.0"),
        "generic callout uses the same stored factor as Details");
    const Boundary cut{{{1,1},{2,1},0},{{2,1},{2,2},0},{{2,2},{1,2},0},{{1,2},{1,1},0}};
    const auto deduction=window.createBoundary(cut,"living");
    require(!deduction.isEmpty(),"actual contained deduction geometry is created");
    auto source=window.document().snapshot();auto parent=source.entities().at(id.toStdString());
    parent.properties["deduction_ids"]=nlohmann::json::array({deduction.toStdString()});
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(parent)}, {},"Add actual measurement deduction"});
    require(window.selectEntity(id) && callout(canvas(window),id,"area_calculation").text.contains("7.5"),
        "actual geometric deduction precedes the stored factor");
    window.setMetricUnits(false);
    require(callout(canvas(window),id,"area_calculation").text.contains("80.7"),"generic net quantity converts to square feet");
    window.setMetricUnits(true);
    const auto path=files.filePath("generic-callouts.bldproj");
    require(window.saveProjectAs(path),"generic calculation saves natively");
    require(window.createNewProject(),"release the saved native project before opening another editor");
    MainWindow reopened({},nullptr,files.filePath("text-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.selectEntity(id) &&
        callout(canvas(reopened),id,"area_calculation").text.contains("7.5"),"native reopen derives the same generic net quantity");
    for(const auto& corrupt_id:{id,deduction}) {
        const auto before=reopened.document().snapshot();auto corrupt=before.entities().at(corrupt_id.toStdString());
        corrupt.properties["property_id"]="other-property";
        reopened.document().apply(ApplyEntityChanges{before.revision(),{EntityChange::upsert(corrupt),
            EntityChange::upsert(Entity{"other-property","property",{{"name","Different property"}},false})}, {},"Corrupt explicit area ownership"});
        reopened.setMetricUnits(false);
        const auto invalid=labels(canvas(reopened),id);
        require(std::none_of(invalid.begin(),invalid.end(),[](const auto& value){return value.callout_role=="area_calculation" && !value.text.isEmpty();}),
            "contradictory parent or deduction property ownership withholds a numeric callout");
        require(reopened.undoCommand(),"restore valid measurement owner context");reopened.setMetricUnits(true);
        require(callout(canvas(reopened),id,"area_calculation").text.contains("7.5"),"Undo restores the exact valid generic calculation");
    }
    auto& workflow=control<QComboBox>(reopened,"calculationWorkflow");
    workflow.setCurrentIndex(workflow.findData(QStringLiteral("appraisal")));
    const auto unqualified=labels(canvas(reopened),id);
    require(reopened.document().snapshot().is_editable() && workflow.currentData().toString()=="appraisal" && reopened.lastError().isEmpty(),
        "actual editable workflow changes to appraisal");
    require(std::none_of(unqualified.begin(),unqualified.end(),[](const auto& value){
        return value.callout_role=="area_calculation" && !value.text.isEmpty();}),
        "unqualified appraisal never falls back to the generic geometric label");
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
    // Clear 3.8 m x 2.8 m = 10.6 m² (centerline would be 12 m²); a physical
    // room readout uses the design-system metric area: one decimal, "m²".
    require(window.separateSelectedAreaCallouts() &&
        callout(canvas(window),id,"area_calculation").text==QStringLiteral(u"10.6 m²"),
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
        fixed_dimensions_are_area_label_obstacles();site_fixed_dimensions_in_other_frames_are_area_label_obstacles();
        independent_live_callouts_and_reopen();generic_measurement_callouts_do_not_claim_appraisal();stale_physical_calculation_is_withheld();
        std::cout<<"area_callout_desktop_tests passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"area_callout_desktop_tests: "<<error.what()<<'\n';return 1;}
}
