#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/application_platform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QAction>
#include <QAbstractItemModel>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QFontDatabase>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfWriter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPushButton>
#include <QRawFont>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>

namespace {
using namespace sketch;using namespace sketch::desktop;using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void require_near(double a,double b){if(std::abs(a-b)>=1e-9)throw std::runtime_error("actual measured dimension "+std::to_string(a)+" differs from analytical expectation "+std::to_string(b));}
Json pdf_font_evidence(const QFont& font) {
    QByteArray bytes;QBuffer buffer(&bytes);require(buffer.open(QIODevice::WriteOnly),"PDF font probe must stage");
    const QString text=QStringLiteral("Vertex PDF font probe: 3.000 m 90.0°");
    {QPdfWriter writer(&buffer);writer.setResolution(144);QPainter painter(&writer);
        require(painter.isActive(),"PDF font probe must start");painter.setFont(font);
        painter.drawText(QRectF(30,30,1000,100),Qt::AlignLeft|Qt::AlignTop,text);
        require(painter.end(),"PDF font probe must finish");}
    buffer.close();require(buffer.open(QIODevice::ReadOnly),"PDF font probe must reopen");
    QPdfDocument pdf;pdf.load(&buffer);const auto raw=QRawFont::fromFont(font);
    const auto extracted=pdf.pageCount()>0?pdf.getAllText(0).text().simplified():QString{};
    return {{"requested_family",font.family().toStdString()},{"resolved_family",raw.familyName().toStdString()},
        {"raw_font_valid",raw.isValid()},{"page_count",pdf.pageCount()},{"text",extracted.toStdString()},
        {"expected_text",text.toStdString()},{"expected_text_present",extracted.contains(text)}};
}
bool same(Vec2 a,Vec2 b){return a.x==b.x&&a.y==b.y;}
Entity stroke(const std::string& id,const std::vector<Vec2>& points,bool arc=false,bool closed=false) {
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();model.closed=closed;
    model.extensions={{"vendor",{{"literal",id+":v1"},{"keep",7}}}};
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt;receipt.segment_id=id+":e"+std::to_string(i);receipt.start=points[i-1];receipt.chord_end=points[i];
        receipt.kind=arc&&i==1?BoundaryConstructionKind::arc_chord_angle:BoundaryConstructionKind::line_to_point;
        if(arc&&i==1)receipt.angle=parse_angle("90 deg");
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),closed&&i+1==points.size()?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture() {
    auto repeated=stroke("repeat",{{0,6},{2,6},{2,8},{2,6},{3,6}});
    auto model=*decode_measurement_linework_model(repeated.properties.at("model")).model;
    model.edges[2].end_vertex_id="repeat:v1";model.edges[3].start_vertex_id="repeat:v1";
    repeated.properties["model"]=encode_measurement_linework_model(model);
    return std::make_shared<Document>(Document::create({{"p","property",Json::object(),false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},
        stroke("open",{{0,0},{2,0},{2,3}}),stroke("arc",{{5,0},{7,0},{7,2}},true),
        stroke("closed",{{10,0},{12,0},{12,2},{10,0}},false,true),repeated,
        stroke("partner",{{2,3},{4,3}})}));
}
template<class T>T& control(QWidget& owner,const char* name){auto* result=owner.findChild<T*>(name);require(result,"actual dimension control must exist");return *result;}
void choose(QComboBox& box,const QString& data){const auto index=box.findData(data);require(index>=0,"creator must offer stable measured target");box.setCurrentIndex(index);QApplication::processEvents();}
void prepare(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1200,800);window.setMetricUnits(true);window.show();QApplication::processEvents();}
PlanCanvas& canvas(MainWindow& window){auto* value=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(value,"real plan canvas must exist");return *value;}
void capture(QWidget& widget,const QString& name){const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    require(QDir().mkpath(directory)&&widget.grab().save(QDir(directory).filePath(name)),"actual measured dimension capture must save");}
BoundaryDimension dimension(const DocumentSnapshot& snapshot,const std::string& id){const auto decoded=decode_boundary_dimension_entity(snapshot.entities().at(id));require(decoded.supported(),"saved dimension must decode");return *decoded.dimension;}
std::vector<BoundaryDimension> dimensions(const DocumentSnapshot& snapshot,const std::string& owner){std::vector<BoundaryDimension> result;
    for(const auto& [id,entity]:snapshot.entities()){(void)id;if(entity.type!="dimension")continue;const auto decoded=decode_boundary_dimension_entity(entity);
        if(decoded.dimension&&decoded.dimension->boundary_id==owner)result.push_back(*decoded.dimension);}return result;}
void creator(MainWindow& window,const std::function<void(QDialog&)>& fn){std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,&window,[&]{auto* dialog=window.findChild<QDialog*>("dimensionCreatorDialog");try{require(dialog,"actual creator dialog opens");opened=true;fn(*dialog);}catch(...){failure=std::current_exception();}
        if(dialog)dialog->reject();});auto& action=control<QAction>(window,"dimensionCreator");require(action.isEnabled(),"actual creator QAction must be enabled");action.trigger();
    if(failure)std::rethrow_exception(failure);require(opened,"creator callback must run");}
void styled(MainWindow& window,const QString& id,Vec2 point){require(!id.isEmpty(),"native dimension creation must succeed");
    require(window.editBoundaryDimension(id,QString::number(point.x,'g',17)+" m",QString::number(point.y,'g',17)+" m","4.2","#713ba2",true,false,true,"0"),"native dimension style and manual placement edit must succeed");}
void exact_history(MainWindow& window,const DocumentSnapshot& before,const DocumentSnapshot& after){require(after.revision()==before.revision()+1&&after.history().size()==before.history().size()+1,"dimension workflow must commit one atomic revision");
    require(window.undoCommand()&&window.document().snapshot().entities()==before.entities()&&window.redoCommand()&&window.document().snapshot().entities()==after.entities(),"UndoRedo must restore exact source/dimension entities");}

void saved_dimension_canvas_drag(int kind,bool mixed=false) {
    const bool partial=kind>=9;
    const bool plain=kind==13;
    const bool rotated=kind==12;
    const Vec2 shift=rotated?Vec2{.4,-.8}:Vec2{.8,.4};
    const auto projected=[&](Vec2 point){return rotated?Vec2{-(point.y+4),point.x-10}:point;};
    QTemporaryDir directory;require(directory.isValid(),"Isolated dimension drag project directory");
    auto document=fixture();
    if(plain)document->apply(ApplyEntityChanges{document->revision(),
        {EntityChange::upsert(make_annotation_entity("callout-annotations",AnnotationState{}))},{},"Create annotation state for grouped note"});
    Entity area{"drag-area","boundary",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"classification","living"},{"segments",Json::array({{{"start",{10,0}},{"end",{12,0}},{"sweep_radians",0}},
        {{"start",{12,0}},{"end",{12,2}},{"sweep_radians",0}},{{"start",{12,2}},{"end",{10,2}},{"sweep_radians",0}},
        {{"start",{10,2}},{"end",{10,0}},{"sweep_radians",0}}})}},false};
    area=upgrade_legacy_boundary_entity(area);
    document->apply(ApplyEntityChanges{.expected_revision=document->revision(),.entity_changes={EntityChange::upsert(area)},.message="Create identified callout owner"});
    MainWindow window(document);prepare(window);
    QString id;
    std::string owner=kind==2 || kind==4 ? "drag-area" : "open";
    std::string second_owner;QString second_id;
    QStringList source_walls;
    if(kind>=5) {
        source_walls={window.createStraightWall({20,0},{24,0},"exterior"),
            window.createStraightWall({24,0},{24,3},"exterior"),
            window.createStraightWall({24,3},{20,3},"exterior"),
            window.createStraightWall({20,3},{20,0},"exterior")};
        for(qsizetype i=0;i<source_walls.size();++i)
            require(!source_walls[i].isEmpty() && window.selectEntity(source_walls[i],i!=0),"Select complete physical perimeter");
        owner=plain?"drag-area":window.createMeasurementBoundaryFromSelectedWalls().toStdString();
        require(!owner.empty(),"Create or retain measured callout owner");
    }
    if(kind==7 || kind==8) {
        const QStringList second_walls{window.createStraightWall({28,0},{32,0},"exterior"),
            window.createStraightWall({32,0},{32,3},"exterior"),window.createStraightWall({32,3},{28,3},"exterior"),
            window.createStraightWall({28,3},{28,0},"exterior")};
        for(qsizetype i=0;i<second_walls.size();++i)require(!second_walls[i].isEmpty() && window.selectEntity(second_walls[i],i!=0),
            "Select second complete physical perimeter");
        second_owner=window.createMeasurementBoundaryFromSelectedWalls().toStdString();
        require(!second_owner.empty(),"Create second independent exterior owner");
        const auto items=dimensions(window.document().snapshot(),second_owner);
        require(items.size()==4,"Second physical exterior has four automatic callouts");second_id=QString::fromStdString(items.front().id);
        source_walls.append(second_walls);
        if(kind==8)std::reverse(source_walls.begin(),source_walls.end());
    }
    if(kind==0)id=window.createLengthDimension("open","open:e2",{4,2});
    else if(kind==1)id=window.createAngleDimension("open","open:e1","open:e2","open:v1",{1,-1});
    else if(kind==2)id=window.createAreaDimension("drag-area",{11,1});
    else if(kind==5 || kind==9 || kind==13)id=window.createAreaDimension(QString::fromStdString(owner),{22,1.5});
    else if(kind>=6) {
        const auto items=dimensions(window.document().snapshot(),owner);
        require(items.size()==4 && items.front().placement==BoundaryDimensionPlacement::automatic,
            "Physical exterior authoring creates one automatic callout per edge");
        id=QString::fromStdString(items.front().id);
    }
    else {
        require(window.selectEntity(QString::fromStdString(owner)),"Select automatic callout source");
        creator(window,[&](QDialog& dialog) {
            if(kind==3)choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"open:e2");
            control<QPushButton>(dialog,"addLengthDimension").click();
        });
        const auto items=dimensions(window.document().snapshot(),owner);
        require(items.size()==1,"Actual creator saves one automatic dimension");id=QString::fromStdString(items.front().id);
    }
    require(!id.isEmpty(),"Create dimension through actual authoring API");
    const auto initial=dimension(window.document().snapshot(),id.toStdString());
    if(kind<3 || kind==5 || kind==9)styled(window,id,initial.text_position);
    auto retained=window.document().snapshot().entities().at(id.toStdString());
    retained.extensions["vendor_drag_metadata"]={{"literal",owner},{"keep",17}};
    window.document().apply(ApplyEntityChanges{.expected_revision=window.document().revision(),
        .entity_changes={EntityChange::upsert(retained)},.message="Retain dimension metadata"});
    if(!second_owner.empty()) {
        PersistentConstraint parallel;parallel.id="selected-shells-parallel";parallel.relation=ConstraintRelationKind::parallel;
        for(const auto& boundary_id:{owner,second_owner}) {
            const auto boundary=decode_identified_boundary_entity(window.document().snapshot().entities().at(boundary_id));
            const auto edge=std::find_if(boundary.segments.begin(),boundary.segments.end(),[](const auto& value){return
                value.segment.sweep_radians==0 && std::abs(value.segment.start.y-value.segment.end.y)<1e-12;});
            require(edge!=boundary.segments.end(),"Perimeter has a horizontal analytical edge");
            parallel.bindings.push_back({boundary_id,WallEndpointRole::start,edge->segment_id,edge->start_vertex_id});
            parallel.bindings.push_back({boundary_id,WallEndpointRole::end,edge->segment_id,edge->end_vertex_id});
        }
        ConstraintAuthoringIntent intent;intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(parallel));
        const auto proof=preview_constraint_authoring(window.document().snapshot(),intent);
        require(proof.accepted(),"Parallel relation between both current measured owners is valid");
        apply_constraint_authoring(window.document(),proof);
    }
    if(rotated) {
        CoordinatedView view;view.id="callout-offset-plan";view.name="Offset rotated plan";
        view.origin_m={10,-4,0};view.up={1,0,0};
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(make_sheet_view_entity("callout-plan-owner",SheetViewModel::create({view},{})))},
            {},"Create rotated callout view"});
        window.setWorkspace(Workspace::architectural);
        require(window.selectEntity(id),"Refresh rotated callout view choices");
        auto& views=control<QComboBox>(window,"architecturalView");
        const auto index=views.findData(QStringLiteral("callout-offset-plan"),Qt::UserRole+1);
        require(index>=3,"Persisted rotated plan is available");views.setCurrentIndex(index);QApplication::processEvents();
    }
    const auto note=plain?window.createAnnotationLabel("note","Move with wall",{22,1.5}):QString{};
    if(plain && note.isEmpty())throw std::runtime_error("Create ordinary label with plain wall and saved callout: "+window.lastError().toStdString());
    require(window.selectEntity(kind==8?second_id:id),"Select saved measurement callout");
    if(!second_owner.empty())require(window.selectEntity(kind==8?id:second_id,true),"Select both perimeter callouts");
    if(partial)source_walls.resize(kind==11?2:1);
    if(mixed) {
        if(source_walls.isEmpty())require(window.selectEntity(QString::fromStdString(owner),true),"Select source and its callout together");
        else for(const auto& wall:source_walls)require(window.selectEntity(wall,true),"Select physical sources with callout, without analytical owner");
    }
    if(plain)require(window.selectEntity(note,true),"Include ordinary note in plain-wall callout selection");
    control<QToolButton>(window,"snapTool").setChecked(false);
    auto* target_surface=rotated?dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas")):&canvas(window);
    require(target_surface,"Target callout canvas exists");auto& surface=*target_surface;
    surface.setSnapEnabled(false);surface.setOverviewMapEnabled(false);
    const auto before=window.document().snapshot();const auto original=dimension(before,id.toStdString());
    const auto measured_before=original.resolve(before.entities().at(owner));
    const auto other_original=second_owner.empty()?std::optional<BoundaryDimension>{}:dimension(before,second_id.toStdString());
    surface.setViewTransform(projected(original.text_position),80);QApplication::processEvents();
    const auto bounds=surface.selectionBounds();require(bounds.has_value(),"Visible dimension selection frame");
    const QPointF start=mixed ? bounds->center() : QRectF(surface.rect()).center();
    const QPointF end=start+QPointF(64,-32);
    const auto send=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,point,surface.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
        QApplication::sendEvent(&surface,&event);
    };
    const auto settle=[&] {
        QElapsedTimer timer;timer.start();
        while(surface.entitiesMovePreviewPending() && timer.elapsed()<3000)QApplication::processEvents(QEventLoop::AllEvents,30);
        QApplication::processEvents();require(!surface.entitiesMovePreviewPending(),"Exact callout move preview completes");
    };
    if(!mixed) {
        send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        send(QEvent::MouseButtonRelease,start,Qt::LeftButton,Qt::NoButton);
        require(window.document().revision()==before.revision(),"A click alone cannot change callout placement");
    }
    if(!mixed || partial) {
        send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        send(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);settle();
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&surface,&escape);
        send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);QApplication::processEvents();
        require(window.document().revision()==before.revision() && window.document().snapshot().entities()==before.entities(),
            "Escape cancels the live callout move without changing source or history");
        require(window.selectEntity(id),"Reselect canceled dimension");
        if(partial)for(const auto& wall:source_walls)require(window.selectEntity(wall,true),"Restore partial perimeter selection after cancellation");
        if(plain)require(window.selectEntity(note,true),"Restore grouped note after cancellation");
        surface.setViewTransform(projected(original.text_position),80);
    }
    send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    send(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);settle();
    require(window.document().snapshot().entities()==before.entities(),"Exact callout preview does not commit early");
    const auto proposal=surface.entitiesMovePreview();
    if(partial)require(!proposal.empty(),"Partial wall and callout move exposes its admitted geometry proposal");
    capture(window,QStringLiteral("dimension-drag-%1-%2-preview.png").arg(kind).arg(mixed?"group":"single"));
    send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);settle();
    const auto after=window.document().snapshot();
    if(after.revision()!=before.revision()+1)throw std::runtime_error("Saved dimension canvas drag kind "+std::to_string(kind)+
        (mixed?" group":" single")+" did not commit one revision: "+window.lastError().toStdString());
    const auto moved=dimension(after,id.toStdString());
    if(other_original) {
        const auto other=dimension(after,second_id.toStdString());
        require_near(other.text_position.x,other_original->text_position.x+.8);require_near(other.text_position.y,other_original->text_position.y+.4);
        require(other.placement==other_original->placement && other.automatic_placement_version==other_original->automatic_placement_version,
            "Both promoted exterior callouts retain placement provenance");
        require(wall_measurement_source_current(after,after.entities().at(owner)) &&
            wall_measurement_source_current(after,after.entities().at(second_owner)),"Both measured owners remain current in either selection order");
    }
    require_near(moved.text_position.x,original.text_position.x+shift.x);require_near(moved.text_position.y,original.text_position.y+shift.y);
    require(moved.kind==original.kind && moved.boundary_id==original.boundary_id && moved.segment_id==original.segment_id &&
        moved.vertex_id==original.vertex_id && moved.secondary_segment_id==original.secondary_segment_id && moved.presentation==original.presentation,
        "Callout drag retains its analytical target, identity and styling");
    require(after.entities().at(id.toStdString()).extensions==before.entities().at(id.toStdString()).extensions,
        "Callout drag retains unrelated metadata");
    if(!mixed || partial) {
        require(moved.placement==BoundaryDimensionPlacement::manual && !moved.automatic_placement_version,
            "Explicit callout drag becomes manual placement");
        if(!mixed)for(const auto& [key,entity]:before.entities())if(key!=id.toStdString())require(after.entities().at(key)==entity,
            "Callout-only drag cannot alter source geometry or other objects");
    } else require(moved.placement==original.placement && moved.automatic_placement_version==original.automatic_placement_version &&
        after.entities().at(owner)!=before.entities().at(owner),"Grouped source motion shifts its callout once and retains placement provenance");
    const auto measured_after=moved.resolve(after.entities().at(owner));
    if(partial) {
        if(plain) {
            require(after.entities().at(owner)==before.entities().at(owner),"Plain wall movement does not redraw an unrelated measured outline");
            bool found{};
            for(const auto& [key,entity]:after.entities())if(entity.type==kAnnotationEntityType) {
                for(const auto& value:decode_annotation_entity(entity).labels)if(value.id==note.toStdString()) {
                    found=true;require_near(value.placement.position.x,22+shift.x);require_near(value.placement.position.y,1.5+shift.y);
                }
            }
            require(found,"Grouped ordinary annotation survives plain-wall move");
        } else require(after.entities().at(owner)!=before.entities().at(owner) &&
            wall_measurement_source_current(after,after.entities().at(owner)),"Partial wall movement redraws a current measured exterior");
        for(const auto& wall:source_walls) {
            const auto& old=before.entities().at(wall.toStdString()).properties.at("baseline");
            const auto& now=after.entities().at(wall.toStdString()).properties.at("baseline");
            for(const auto* endpoint:{"start","end"}) {
                require_near(now.at(endpoint).at(0).get<double>(),old.at(endpoint).at(0).get<double>()+shift.x);
                require_near(now.at(endpoint).at(1).get<double>(),old.at(endpoint).at(1).get<double>()+shift.y);
            }
        }
        require(after.history().back().boundary_constraint_changes &&
            command_to_json(*after.history().back().boundary_constraint_changes).at("version")==15,
            "Partial wall and callout drag retains its typed placement command");
        if(kind==9)require(std::abs(measured_after.area_square_metres-measured_before.area_square_metres)>1e-4,
            "Partial wall movement updates the actual area callout value");
    } else {
        require_near(measured_after.segment_length_metres,measured_before.segment_length_metres);
        require_near(measured_after.angle_radians,measured_before.angle_radians);require_near(measured_after.area_square_metres,measured_before.area_square_metres);
    }
    const auto label=std::find_if(surface.labels().begin(),surface.labels().end(),[&](const auto& value){return value.id==id;});
    require(label!=surface.labels().end(),"Committed callout label is visible");
    const auto expected_position=projected(moved.text_position);
    require_near(label->position.x,expected_position.x);require_near(label->position.y,expected_position.y);
    if(partial)for(const auto& proposed:proposal) {
        const auto committed=std::find_if(surface.entities().begin(),surface.entities().end(),[&](const auto& value){return value.id==proposed.id;});
        require(committed!=surface.entities().end() && committed->segments.size()==proposed.segments.size(),
            "Committed partial move retains every admitted projected owner and dimension guide");
        for(std::size_t edge=0;edge<proposed.segments.size();++edge) {
            const auto& expected=proposed.segments[edge];const auto& actual=committed->segments[edge];
            require_near(actual.start.x,expected.start.x);require_near(actual.start.y,expected.start.y);
            require_near(actual.end.x,expected.end.x);require_near(actual.end.y,expected.end.y);
            require_near(actual.sweep_radians,expected.sweep_radians);
        }
    }
    exact_history(window,before,after);capture(window,QStringLiteral("dimension-drag-%1-%2-applied.png").arg(kind).arg(mixed?"group":"single"));
    const auto file=directory.filePath("callout-drag.bldproj");require(window.saveProjectAs(file) && window.createNewProject(),"Save moved callout and release writer lease");
    MainWindow reopened;prepare(reopened);
    require(reopened.openProject(file) && reopened.document().is_editable() && reopened.document().snapshot().entities()==after.entities() &&
        reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities() &&
        reopened.redoCommand() && reopened.document().snapshot().entities()==after.entities(),"Callout drag reopens editable with exact source and placement history");
}

void mixed_complete_partial_canvas_drag(bool reverse,bool select_whole_callout=true) {
    QTemporaryDir temporary;require(temporary.isValid(),"Mixed exterior evidence directory exists");
    MainWindow window(fixture());prepare(window);
    const auto shell=[&](double x) {
        QStringList walls{window.createStraightWall({x,0},{x+4,0},"exterior"),
            window.createStraightWall({x+4,0},{x+4,3},"exterior"),
            window.createStraightWall({x+4,3},{x,3},"exterior"),
            window.createStraightWall({x,3},{x,0},"exterior")};
        for(qsizetype i=0;i<walls.size();++i)require(!walls[i].isEmpty() && window.selectEntity(walls[i],i!=0),
            "Select new complete exterior source");
        const auto owner=window.createMeasurementBoundaryFromSelectedWalls();
        require(!owner.isEmpty(),"Create both independent measured exteriors");
        return std::pair{owner,walls};
    };
    const auto [whole,whole_walls]=shell(20);const auto [partial,partial_walls]=shell(28);
    const auto whole_dimensions=dimensions(window.document().snapshot(),whole.toStdString());
    const auto partial_dimensions=dimensions(window.document().snapshot(),partial.toStdString());
    require(whole_dimensions.size()==4 && partial_dimensions.size()==4,"Both exteriors have automatic callouts");
    const auto whole_callout=QString::fromStdString(whole_dimensions.front().id);
    const auto partial_callout=window.createAreaDimension(partial,{30,1.5});
    const auto manual=window.createAreaDimension(whole,{22,1.5});
    require(!partial_callout.isEmpty() && !manual.isEmpty(),"Create selected partial and unselected whole-area callouts");
    QStringList selection{partial_callout};if(select_whole_callout)selection.prepend(whole_callout);
    selection.append(whole_walls);selection.append(partial_walls.front());
    if(reverse)std::reverse(selection.begin(),selection.end());
    const auto select=[&] {for(qsizetype i=0;i<selection.size();++i)require(window.selectEntity(selection[i],i!=0),
        "Restore mixed whole and partial perimeter selection");};select();
    control<QToolButton>(window,"snapTool").setChecked(false);
    auto& surface=canvas(window);surface.setSnapEnabled(false);surface.setOverviewMapEnabled(false);
    surface.setViewTransform({26,1.5},60);QApplication::processEvents();
    const auto before=window.document().snapshot();const Vec2 delta{.8,.4};
    const auto bounds=surface.selectionBounds();require(bounds.has_value(),"Mixed group has visible selection boundary");
    const auto start=bounds->center(),end=start+QPointF(48,-24);
    const auto send=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,point,surface.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(&surface,&event);
    };
    const auto settle=[&] {QElapsedTimer timer;timer.start();
        while(surface.entitiesMovePreviewPending() && timer.elapsed()<5000)QApplication::processEvents(QEventLoop::AllEvents,30);
        QApplication::processEvents();require(!surface.entitiesMovePreviewPending(),"Mixed move preview completes");};
    send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);send(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);settle();
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&surface,&escape);
    send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);settle();
    require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
        "Cancel preserves the entire mixed group and history");select();surface.setViewTransform({26,1.5},60);QApplication::processEvents();
    send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);send(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);settle();
    require(window.document().snapshot().entities()==before.entities(),"Mixed preview cannot commit early");
    const auto proposal=surface.entitiesMovePreview();
    capture(window,QStringLiteral("mixed-complete-partial-%1-%2-preview.png").arg(reverse).arg(select_whole_callout));
    send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);settle();
    const auto after=window.document().snapshot();
    if(after.revision()!=before.revision()+1)throw std::runtime_error("Mixed complete and partial exterior drag refused: "+window.lastError().toStdString());
    require(!proposal.empty(),"Mixed move has an admitted geometry preview");
    require(after.history().back().boundary_constraint_changes &&
        command_to_json(*after.history().back().boundary_constraint_changes).at("version")==17 &&
        after.history().back().boundary_constraint_changes->joint_translation_completion &&
        after.history().back().boundary_constraint_changes->joint_translation &&
        !after.history().back().boundary_constraint_changes->rigid_group_transform,
        "Mixed drag retains its independently reconstructed joint translation proof");
    require(wall_measurement_source_current(after,after.entities().at(whole.toStdString())) &&
        wall_measurement_source_current(after,after.entities().at(partial.toStdString())),"Both exteriors stay current after mixed movement");
    for(const auto& old:dimensions(before,whole.toStdString())) {
        const auto now=dimension(after,old.id);require_near(now.text_position.x,old.text_position.x+delta.x);
        require_near(now.text_position.y,old.text_position.y+delta.y);
        require(now.placement==old.placement && now.automatic_placement_version==old.automatic_placement_version && now.presentation==old.presentation,
            "Every selected or unselected whole-owner callout follows once with original provenance and styling");
    }
    const auto old=dimension(before,partial_callout.toStdString()),now=dimension(after,partial_callout.toStdString());
    require_near(now.text_position.x,old.text_position.x+delta.x);require_near(now.text_position.y,old.text_position.y+delta.y);
    require(now.placement==BoundaryDimensionPlacement::manual && !now.automatic_placement_version,
        "Explicit partial-owner callout move retains manual placement");
    require(after.entities().at(partial_walls[2].toStdString())==before.entities().at(partial_walls[2].toStdString()),
        "Unconnected far wall remains unchanged");
    for(const auto& proposed:proposal) {
        const auto committed=std::find_if(surface.entities().begin(),surface.entities().end(),[&](const auto& value){return value.id==proposed.id;});
        require(committed!=surface.entities().end() && committed->segments.size()==proposed.segments.size(),"Mixed committed geometry matches admitted preview");
        for(std::size_t i=0;i<proposed.segments.size();++i) {
            require_near(committed->segments[i].start.x,proposed.segments[i].start.x);require_near(committed->segments[i].start.y,proposed.segments[i].start.y);
            require_near(committed->segments[i].end.x,proposed.segments[i].end.x);require_near(committed->segments[i].end.y,proposed.segments[i].end.y);
        }
    }
    exact_history(window,before,after);capture(window,QStringLiteral("mixed-complete-partial-%1-%2-applied.png").arg(reverse).arg(select_whole_callout));
    const auto path=temporary.filePath("mixed-exteriors.bldproj");
    if (!window.saveProjectAs(path)) throw std::runtime_error("Save mixed proof: "+window.lastError().toStdString());
    if (!window.createNewProject()) throw std::runtime_error("Release mixed proof writer lease: "+window.lastError().toStdString());
    MainWindow reopened;prepare(reopened);require(reopened.openProject(path) && reopened.document().is_editable() &&
        reopened.document().snapshot().entities()==after.entities() && reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities() &&
        reopened.redoCommand() && reopened.document().snapshot().entities()==after.entities(),"Mixed proof reopens editable with exact one-step history");
}

void actual_creator_terminal_arc_and_revisits() {
    MainWindow window(fixture());prepare(window);require(window.selectEntity("open"),"select real measured source");
    creator(window,[&](QDialog& dialog){auto& owner=control<QComboBox>(dialog,"dimensionSourceBoundary");require(owner.currentData().toString()=="open","creator must default to selected measured stroke");
        auto& kind=control<QComboBox>(dialog,"dimensionCreateKind");const auto area=kind.findData(QStringLiteral("area"));
        require(area<0||!(kind.model()->flags(kind.model()->index(area,0))&Qt::ItemIsEnabled),"Area must be unavailable for measured strokes");
        choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"open:e2");
        require(control<QComboBox>(dialog,"dimensionVertex").count()==0&&!control<QPushButton>(dialog,"addAngleDimension").isEnabled(),"identical edge pair must offer no false shared angle");
        const auto before=window.document().snapshot();control<QPushButton>(dialog,"addLengthDimension").click();
        const auto list=dimensions(window.document().snapshot(),"open");require(list.size()==1&&list[0].segment_id=="open:e2","actual Add length must retain terminal edge target");
        require_near(list[0].resolve(window.document().snapshot().entities().at("open")).segment_length_metres,3);
        require(window.document().revision()==before.revision()+1&&!control<QLabel>(dialog,"dimensionCreatorStatus").text().isEmpty(),"creator reports one saved dimension");
        choose(owner,"arc");choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"arc:e1");control<QPushButton>(dialog,"addLengthDimension").click();
        choose(kind,"angle");choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"arc:e1");choose(control<QComboBox>(dialog,"dimensionSecondSegment"),"arc:e2");
        choose(control<QComboBox>(dialog,"dimensionVertex"),"arc:v1");control<QLineEdit>(dialog,"dimensionCreateX").setText("8");control<QLineEdit>(dialog,"dimensionCreateY").setText("1");
        control<QPushButton>(dialog,"addAngleDimension").click();const auto arc=dimensions(window.document().snapshot(),"arc");require(arc.size()==2,"real creator saves arc length and tangent angle");
        for(const auto& item:arc){const auto resolved=item.resolve(window.document().snapshot().entities().at("arc"));if(item.kind==BoundaryDimensionKind::angle)require_near(resolved.angle_radians,3*std::numbers::pi/4);else require_near(resolved.segment_length_metres,std::sqrt(2.0)*std::numbers::pi/2);}
        choose(owner,"repeat");auto& vertex=control<QComboBox>(dialog,"dimensionVertex");require(vertex.count()==1&&vertex.currentData()==QStringLiteral("repeat:v1"),"revisited shared vertex is listed once by stable identity");
        choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"repeat:e1");choose(control<QComboBox>(dialog,"dimensionSecondSegment"),"repeat:e3");choose(vertex,"repeat:v1");
        control<QPushButton>(dialog,"addAngleDimension").click();require(dimensions(window.document().snapshot(),"repeat").size()==1,"nonadjacent edges sharing revisited ID support saved angle");
        capture(dialog,"measured-dimension-creator.png");});
    const auto before=window.document().snapshot();require(window.createAreaDimension("closed",{11,1}).isEmpty()&&window.document().snapshot().entities()==before.entities()&&window.document().revision()==before.revision(),
        "even a closed measured stroke cannot acquire saved area semantics");
    const auto angle=dimensions(before,"repeat").front();require(window.selectEntity("repeat")&&window.moveSelectedBoundaryVertex("repeat:v1",{3,7},before.revision()),"native revisited vertex edit must preserve dimension binding");
    const auto after=window.document().snapshot();require(dimension(after,angle.id)==angle,"manual angle placement and stable references survive vertex edits");
    (void)angle.resolve(after.entities().at("repeat"));exact_history(window,before,after);
}

void connected_edit_updates_saved_values() {
    MainWindow window(fixture());prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    const auto related=window.createLengthDimension("partner","partner:e1",{3,4});styled(window,related,{3,4});
    const auto angle=window.createAngleDimension("open","open:e1","open:e2","open:v1",{1,-1});styled(window,angle,{1,-1});
    PersistentConstraint join;join.id="terminal-join";join.relation=ConstraintRelationKind::coincident;
    join.bindings={{"open",WallEndpointRole::end,"open:e2","open:v2"},{"partner",WallEndpointRole::start,"partner:e1","partner:v0"}};
    window.document().apply(ApplyEntityChanges{.expected_revision=window.document().revision(),.entity_changes={EntityChange::upsert(encode_constraint_entity(join))},.message="Retain terminal relationship"});
    const auto before=window.document().snapshot();require(window.selectEntity("open")&&window.moveSelectedBoundaryVertex("open:v2",{5,4},before.revision()),"native endpoint edit must follow persisted stroke relationship");
    const auto after=window.document().snapshot();const auto changed=dimension(after,length.toStdString());require_near(changed.resolve(after.entities().at("open")).segment_length_metres,5);
    require(changed==dimension(before,length.toStdString())&&dimension(after,related.toStdString())==dimension(before,related.toStdString()),"nonrigid edits retain manual placements/styles/identities");
    require(std::abs(dimension(after,related.toStdString()).resolve(after.entities().at("partner")).segment_length_metres-2)>1e-5,"connected owner's saved value must reflect moved geometry");
    exact_history(window,before,after);
}

void transformed_clone_is_independent() {
    MainWindow window(fixture());prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    const auto angle=window.createAngleDimension("open","open:e1","open:e2","open:v1",{1,-1});styled(window,angle,{1,-1});
    const auto before=window.document().snapshot();
    require(window.selectEntity("open"),"select source for measured clone");
    if(!window.transformSelectedBoundary("90",true,false,"3 m","-2 m",true))
        throw std::runtime_error("transformed measured clone: "+window.lastError().toStdString());
    const auto after=window.document().snapshot();std::string copy;
    for(const auto& [id,entity]:after.entities())if(!before.entities().contains(id)&&entity.type=="measurement_linework")copy=id;
    require(!copy.empty(),"transformed clone must have a fresh measured owner");
    for(const auto& [id,entity]:before.entities())require(after.entities().at(id)==entity,"transformed clone must preserve every original entity");
    const auto owned=dimensions(after,copy);require(owned.size()==2,"transformed clone must preserve both owned dimensions");
    for(const auto& item:owned) {
        const auto original=dimension(before,item.kind==BoundaryDimensionKind::angle?angle.toStdString():length.toStdString());
        require(item.id!=original.id&&item.segment_id!=original.segment_id&&item.presentation==original.presentation,"clone must retain style and remap identity");
        require_near(item.text_position.x,item.kind==BoundaryDimensionKind::angle?1.5:4.5);
        require_near(item.text_position.y,item.kind==BoundaryDimensionKind::angle?-.5:2.5);
        const auto resolved=item.resolve(after.entities().at(copy));
        if(item.kind==BoundaryDimensionKind::angle)require_near(resolved.angle_radians,std::numbers::pi/2);else require_near(resolved.segment_length_metres,3);
    }
    exact_history(window,before,after);
}

void actual_transform_preview_cancel_and_apply() {
    MainWindow window(fixture());prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    require(window.selectEntity("open"),"select measured owner for real transform dialog");
    const auto before=window.document().snapshot();
    const auto exercise=[&](bool apply) {
        std::exception_ptr failure;bool opened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
            try {
                require(dialog,"measured transform dialog must open");opened=true;
                control<QLineEdit>(*dialog,"boundaryRotationDegrees").setText("90");
                control<QLineEdit>(*dialog,"boundaryOffsetX").setText("3 m");
                control<QLineEdit>(*dialog,"boundaryOffsetY").setText("-2 m");
                control<QCheckBox>(*dialog,"boundaryFlipHorizontal").setChecked(true);
                QApplication::processEvents();
                auto& buttons=control<QDialogButtonBox>(*dialog,"boundaryTransformButtons");
                require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"valid measured transform preview must enable Apply");
                auto* preview=dynamic_cast<PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
                require(preview&&preview->labels().size()==2,"real preview must show original and proposed saved dimension labels");
                require(window.document().snapshot().entities()==before.entities(),"preview must not mutate document or label placement");
                capture(*dialog,apply?"measured-transform-apply.png":"measured-transform-cancel.png");
                buttons.button(apply?QDialogButtonBox::Apply:QDialogButtonBox::Cancel)->click();
            }catch(...){failure=std::current_exception();if(dialog)dialog->reject();}
        });
        window.showBoundaryTransformEditor();
        if(failure)std::rethrow_exception(failure);require(opened,"transform callback must execute");
    };
    exercise(false);require(window.document().snapshot().entities()==before.entities()&&window.document().revision()==before.revision(),"Cancel must preserve source, labels and revision");
    exercise(true);const auto after=window.document().snapshot();const auto moved=dimension(after,length.toStdString());
    require_near(moved.text_position.x,4.5);require_near(moved.text_position.y,2.5);exact_history(window,before,after);
}

void mixed_canvas_move_retains_placed_dimensions() {
    auto document=fixture();
    Entity area{"ordinary-area","boundary",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"classification","living"},{"segments",Json::array({{{"start",{10,0}},{"end",{12,0}},{"sweep_radians",0}},
        {{"start",{12,0}},{"end",{12,2}},{"sweep_radians",0}},{{"start",{12,2}},{"end",{10,2}},{"sweep_radians",0}},
        {{"start",{10,2}},{"end",{10,0}},{"sweep_radians",0}}})}},false};
    area=upgrade_legacy_boundary_entity(area);
    document->apply(ApplyEntityChanges{.expected_revision=document->revision(),.entity_changes={EntityChange::upsert(area)},.message="Add ordinary identified area"});
    MainWindow window(document);prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    require(window.selectEntity("open")&&window.selectEntity("ordinary-area",true)&&window.selectedEntityIds().size()==2,"select stroke and unauthored area together");
    auto& surface=canvas(window);surface.setSnapEnabled(false);surface.setOverviewMapEnabled(false);surface.setViewTransform({6,1},50);
    const auto before=window.document().snapshot();
    const QPointF start=QRectF(surface.rect()).center()+QPointF(5*surface.viewScale(),0);
    const QPointF end=start+QPointF(75,-50);
    QMouseEvent down(QEvent::MouseButtonPress,start,surface.mapToGlobal(start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove,end,surface.mapToGlobal(end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease,end,surface.mapToGlobal(end.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&surface,&down);QApplication::sendEvent(&surface,&drag);
    require(window.document().snapshot().entities()==before.entities(),"mixed canvas preview cannot alter placements");
    QApplication::sendEvent(&surface,&up);QElapsedTimer completion;completion.start();
    while(surface.entitiesMovePreviewPending()&&completion.elapsed()<3000)QApplication::processEvents(QEventLoop::AllEvents,30);
    QApplication::processEvents();
    const auto after=window.document().snapshot();
    if(after.revision()==before.revision())throw std::runtime_error("mixed ordinary area Move did not commit: "+window.lastError().toStdString());
    const auto placement=dimension(after,length.toStdString());require_near(placement.text_position.x,5.5);require_near(placement.text_position.y,3);
    require(placement.presentation==dimension(before,length.toStdString()).presentation,"mixed ordinary Move must preserve label style");
    require(after.entities().at("ordinary-area")!=before.entities().at("ordinary-area")&&after.entities().at("open")!=before.entities().at("open"),"both selected owners must move");
    exact_history(window,before,after);capture(window,"mixed-measured-dimension-move.png");
}

void automatic_placement_uses_active_workspace_scale() {
    for(const auto workspace:{Workspace::measurement,Workspace::architectural}) {
        MainWindow window(fixture());prepare(window);window.setWorkspace(workspace);QApplication::processEvents();
        auto& measurement=canvas(window);
        auto* architectural=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
        require(architectural,"second real workspace canvas must exist");
        measurement.setViewTransform({2,1},100);architectural->setViewTransform({2,1},40);
        require(window.selectEntity("open"),"select measured stroke in each workspace");
        creator(window,[&](QDialog& dialog) {
            choose(control<QComboBox>(dialog,"dimensionFirstSegment"),"open:e2");
            control<QPushButton>(dialog,"addLengthDimension").click();
        });
        const auto items=dimensions(window.document().snapshot(),"open");require(items.size()==1,"active workspace creator must save one length");
        const auto scale=workspace==Workspace::measurement?measurement.viewScale():architectural->viewScale();
        require_near(std::hypot(items[0].text_position.x-2,items[0].text_position.y-1.5)*scale,24);
    }
}

void mixed_wall_anchor_move_keeps_whole_transaction_admission() {
    MainWindow window(fixture());prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    const auto wall=window.createStraightWall({2,3},{4,3});require(!wall.isEmpty(),"mixed movement fixture needs real wall");
    const auto neighbor=window.createStraightWall({4,3},{6,3});require(!neighbor.isEmpty(),"mixed movement fixture needs an unselected anchored neighbor");
    PersistentConstraint join;join.id="stroke-wall-join";join.relation=ConstraintRelationKind::coincident;
    join.bindings={{"open",WallEndpointRole::end,"open:e2","open:v2"},{wall.toStdString(),WallEndpointRole::start}};
    PersistentConstraint neighbor_join;neighbor_join.id="wall-neighbor-join";neighbor_join.relation=ConstraintRelationKind::coincident;
    neighbor_join.bindings={{wall.toStdString(),WallEndpointRole::end},{neighbor.toStdString(),WallEndpointRole::start}};
    PersistentConstraint anchor;anchor.id="wall-anchor";anchor.relation=ConstraintRelationKind::fixed_anchor;
    anchor.bindings={{neighbor.toStdString(),WallEndpointRole::end}};anchor.anchor=Vec2{6,3};
    window.document().apply(ApplyEntityChanges{.expected_revision=window.document().revision(),
        .entity_changes={EntityChange::upsert(encode_constraint_entity(join)),EntityChange::upsert(encode_constraint_entity(neighbor_join)),
            EntityChange::upsert(encode_constraint_entity(anchor))},.message="Retain mixed hard relations"});
    require(window.selectEntity("open")&&window.selectEntity(wall,true),"select stroke and wall connected to an anchored neighbor together");
    control<QToolButton>(window,"snapTool").setChecked(false);
    auto& surface=canvas(window);surface.setSnapEnabled(false);surface.setOverviewMapEnabled(false);surface.setViewTransform({2,2},64);
    QApplication::processEvents();
    std::set<QString> projected_selection;
    for (const auto& entity:surface.entities()) if (entity.selected) projected_selection.insert(entity.id);
    require(projected_selection==std::set<QString>{"open",wall},"anchored mixed move must start with both owners selected in the real canvas projection");
    const auto bounds=surface.selectionBounds();require(bounds.has_value(),"anchored mixed move must have a real selection frame");
    // Use exactly representable model coordinates inside the actual frame.
    // Label footprints can give the frame center fractional pixels, whose
    // independent screen-to-model subtractions need not yield these same bits.
    const auto before=window.document().snapshot();
    const QPointF start=QRectF(surface.rect()).center()+QPointF(64,-64),end=start+QPointF(96,-64);
    require(bounds->contains(start),"exact anchored mixed press must be inside the actual selected frame");
    ConstraintAuthoringIntent intent;intent.joint_translation=JointTranslationIntent{{1.5,1},{},{"open"},{wall.toStdString()},true};
    const auto expected_preview=preview_constraint_authoring(before,intent);
    if (!expected_preview.accepted()) {
        std::string diagnostic="anchored partial-neighbor fixture preview rejected:";
        for (const auto& message:expected_preview.diagnostics()) diagnostic+=" "+message;
        throw std::runtime_error(diagnostic);
    }
    auto locked=Document::fork(before);
    auto selected_anchor=anchor;selected_anchor.id="selected-wall-anchor";
    selected_anchor.bindings={{wall.toStdString(),WallEndpointRole::start}};selected_anchor.anchor=Vec2{2,3};
    locked.apply(ApplyEntityChanges{.expected_revision=locked.revision(),
        .entity_changes={EntityChange::upsert(encode_constraint_entity(selected_anchor))},.message="Fix selected endpoint for refusal"});
    const auto locked_source=locked.snapshot();const auto refusal=preview_constraint_authoring(locked_source,intent);
    require(!refusal.accepted() && refusal.candidate_entities()==locked_source.entities() &&
        std::any_of(refusal.diagnostics().begin(),refusal.diagnostics().end(),[](const auto& message) {
            return message.find("conflicts with a saved fixed position")!=std::string::npos;
        }) && locked.snapshot().entities()==locked_source.entities() && locked.revision()==locked_source.revision(),
        "mixed selected fixed endpoint must still reject the exact requested offset without mutation");
    const auto serial=surface.entitiesMovePreviewSerial();
    QMouseEvent down(QEvent::MouseButtonPress,start,surface.mapToGlobal(start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove,end,surface.mapToGlobal(end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease,end,surface.mapToGlobal(end.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    const auto settle=[&] {
        QElapsedTimer completion;completion.start();
        while(surface.entitiesMovePreviewPending()&&completion.elapsed()<5000)QApplication::processEvents(QEventLoop::AllEvents,30);
        QApplication::processEvents();
        require(!surface.entitiesMovePreviewPending(),"anchored mixed move must finish its exact preview admission");
    };
    QApplication::sendEvent(&surface,&down);QApplication::sendEvent(&surface,&drag);settle();
    require(surface.entitiesMovePreviewSerial()>serial,"anchored mixed drag must enter object movement and request an exact preview");
    const auto proposal=surface.entitiesMovePreview();
    if (proposal.empty()) throw std::runtime_error("anchored mixed move exact preview was rejected: "+window.lastError().toStdString());
    for (const auto& id:{QStringLiteral("open"),wall}) require(std::any_of(proposal.begin(),proposal.end(),[&](const auto& entity) {
        return entity.id==id;
    }),"anchored mixed move preview must admit both selected owners together");
    const auto open_proposal=std::find_if(proposal.begin(),proposal.end(),[](const auto& entity) { return entity.id==QStringLiteral("open"); });
    require(!open_proposal->segments.empty(),"anchored mixed stroke preview must contain its retained edges");
    const Vec2 admitted_offset=open_proposal->segments.front().start;
    const auto diagnostic_state=[&] {
        const auto center=surface.viewCenter();
        return Json{{"revision",window.document().revision()},{"preview_serial",surface.entitiesMovePreviewSerial()},
            {"pending",surface.entitiesMovePreviewPending()},{"last_error",window.lastError().toStdString()},
            {"snap_checked",control<QToolButton>(window,"snapTool").isChecked()},
            {"view_center",{center.x,center.y}},{"scale",surface.viewScale()},
            {"selection_bounds",{bounds->x(),bounds->y(),bounds->width(),bounds->height()}},
            {"start",{start.x(),start.y()}},{"end",{end.x(),end.y()}},
            {"desired_offset",{1.5,1.0}},{"admitted_offset",{admitted_offset.x,admitted_offset.y}}};
    };
    const auto before_release=diagnostic_state();
    if (!same(admitted_offset,{1.5,1})) throw std::runtime_error("anchored mixed preview changed the exact unsnapped offset: "+before_release.dump());
    require(window.document().snapshot().entities()==before.entities(),"anchored mixed move preview cannot commit early");
    QApplication::sendEvent(&surface,&up);settle();
    const auto after=window.document().snapshot();
    if(after.revision()==before.revision())throw std::runtime_error("mixed anchored wall Move did not commit: "+window.lastError().toStdString());
    const auto placement=dimension(after,length.toStdString());require_near(placement.text_position.x,5.5);require_near(placement.text_position.y,3);
    if (after.entities()!=expected_preview.candidate_entities()) {
        auto detail=Json{{"before_release",before_release},{"after_release",diagnostic_state()},
            {"source_revision",before.revision()},{"actual_revision",after.revision()},
            {"expected_entity_count",expected_preview.candidate_entities().size()},{"actual_entity_count",after.entities().size()}};
        if (after.history().back().boundary_constraint_changes) {
            const auto proof=command_to_json(*after.history().back().boundary_constraint_changes);
            detail["command_version"]=proof.at("version");
            if (proof.contains("joint_translation")) detail["joint_translation"]=proof.at("joint_translation");
        }
        for (const auto& [id,expected]:expected_preview.candidate_entities()) {
            const auto actual=after.entities().find(id);
            if (actual==after.entities().end()) { detail["missing_entity"]=id;break; }
            if (actual->second==expected) continue;
            detail["first_differing_entity"]=id;
            detail["properties_diff"]=Json::diff(expected.properties,actual->second.properties);
            detail["extensions_diff"]=Json::diff(expected.extensions,actual->second.extensions);
            detail["expected_type"]=expected.type;detail["actual_type"]=actual->second.type;
            detail["expected_required"]=expected.required;detail["actual_required"]=actual->second.required;
            break;
        }
        throw std::runtime_error("actual mixed canvas move differs from the independently admitted anchored-neighbor candidate: "+detail.dump());
    }
    for (const auto* id:{"stroke-wall-join","wall-neighbor-join","wall-anchor"})
        require(after.entities().at(id)==before.entities().at(id),"mixed movement preserves saved coincidence and fixed-anchor identities and positions");
    const auto& partial=after.entities().at(neighbor.toStdString()).properties.at("baseline");
    require(partial.at("start")[0]==5.5 && partial.at("start")[1]==4 && partial.at("end")[0]==6 && partial.at("end")[1]==3,
        "unselected anchored neighbor must follow the exact selected contact while retaining its fixed far endpoint");
    for (const auto& expected:proposal) {
        const auto actual=std::find_if(surface.entities().begin(),surface.entities().end(),[&](const auto& entity) { return entity.id==expected.id; });
        require(actual!=surface.entities().end() && actual->segments.size()==expected.segments.size(),"anchored mixed move must commit every admitted owner");
        for (std::size_t i=0;i<expected.segments.size();++i) {
            require(same(actual->segments[i].start,expected.segments[i].start) && same(actual->segments[i].end,expected.segments[i].end) &&
                actual->segments[i].sweep_radians==expected.segments[i].sweep_radians,"anchored mixed move committed geometry must exactly match its admitted preview");
        }
    }
    exact_history(window,before,after);
}

void transform_clipboard_output_delete_and_reopen() {
    QTemporaryDir temporary;require(temporary.isValid(),"native dimension workflow needs output directory");MainWindow window(fixture());prepare(window);
    const auto length=window.createLengthDimension("open","open:e2",{4,2});styled(window,length,{4,2});
    const auto angle=window.createAngleDimension("open","open:e1","open:e2","open:v1",{1,-1});styled(window,angle,{1,-1});
    const auto arc_length=window.createLengthDimension("arc","arc:e1",{8,-1});styled(window,arc_length,{8,-1});
    const auto arc_angle=window.createAngleDimension("arc","arc:e1","arc:e2","arc:v1",{8,1});styled(window,arc_angle,{8,1});
    const auto before=window.document().snapshot();require_near(dimension(before,arc_length.toStdString()).resolve(before.entities().at("arc")).segment_length_metres,std::sqrt(2.0)*std::numbers::pi/2);
    require_near(dimension(before,arc_angle.toStdString()).resolve(before.entities().at("arc")).angle_radians,3*std::numbers::pi/4);
    require(window.selectEntity("open")&&window.transformSelectedBoundary("90",true,false,"3 m","-2 m",false),"native rigid stroke transform must include saved dimensions");
    const auto after=window.document().snapshot();const auto moved_length=dimension(after,length.toStdString()),moved_angle=dimension(after,angle.toStdString());
    require_near(moved_length.text_position.x,4.5);require_near(moved_length.text_position.y,2.5);require_near(moved_angle.text_position.x,1.5);require_near(moved_angle.text_position.y,-.5);
    require(moved_length.presentation==dimension(before,length.toStdString()).presentation&&moved_angle.presentation==dimension(before,angle.toStdString()).presentation,"rigid placement completion retains persisted style");
    exact_history(window,before,after);canvas(window).fitView();QApplication::processEvents();
    std::vector<std::pair<std::string,QString>> texts;for(const auto& item:{moved_length,moved_angle,dimension(after,arc_length.toStdString()),dimension(after,arc_angle.toStdString())}){const auto found=std::find_if(canvas(window).labels().begin(),canvas(window).labels().end(),[&](const auto& label){return label.id==QString::fromStdString(item.id);});
        require(found!=canvas(window).labels().end()&&found->color==QColor("#713ba2")&&found->bold,"real canvas projects saved measured dimension style");texts.emplace_back(item.id,found->text);}
    const auto svg=temporary.filePath("measured-dimensions.svg"),pdf=temporary.filePath("measured-dimensions.pdf");require(window.exportDraftSvg(svg)&&window.exportDraftPdf(pdf),"actual plan export must include measured dimension projection");
    QFile input(svg);require(input.open(QIODevice::ReadOnly),"SVG must reopen");const auto svg_bytes=input.readAll();QXmlStreamReader xml(svg_bytes);QString svg_text;
    while(!xml.atEnd()){xml.readNext();if(xml.isCharacters())svg_text+=xml.text().toString();}require(!xml.hasError(),"SVG must be valid XML");
    QPdfDocument output;require(output.load(pdf)==QPdfDocument::Error::None&&output.pageCount()>0,"actual dimension PDF must reopen");const auto pdf_text=output.getAllText(0).text().simplified();
    auto text_evidence=Json::array();bool all_text_present=true;
    for(const auto& [id,text]:texts) {
        const auto svg_has=svg_text.contains(text),pdf_has=pdf_text.contains(text.simplified());
        all_text_present=all_text_present && svg_has && pdf_has;
        const auto item=dimension(after,id);const auto resolved=item.resolve(after.entities());
        text_evidence.push_back({{"id",id},{"source",item.boundary_id},{"kind",std::string(boundary_dimension_kind_name(item.kind))},
            {"text",text.toStdString()},{"svg_has",svg_has},{"pdf_has",pdf_has},
            {"length_m",resolved.segment_length_metres},{"angle_radians",resolved.angle_radians},
            {"placement",{item.text_position.x,item.text_position.y}}});
    }
    if (!all_text_present) {
        const Json evidence{{"dimensions",text_evidence},{"svg_text",svg_text.toStdString()},
            {"pdf_text",pdf_text.toStdString()},{"pdf_page_count",output.pageCount()},
            {"platform",QGuiApplication::platformName().toStdString()},{"platform_environment",qEnvironmentVariable("QT_QPA_PLATFORM").toStdString()},
            {"application_font_probe",pdf_font_evidence(QApplication::font())},
            {"system_font_probe",pdf_font_evidence(QFont(QStringLiteral("Arial"),10))},
            {"document_revision",after.revision()},{"snapshot_changed_during_export",window.document().snapshot().entities()!=after.entities()}};
        const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            require(QDir().mkpath(directory),"dimension export diagnostic directory must exist");
            QFile saved_svg(QDir(directory).filePath("measured-dimensions-failed.svg"));
            require(saved_svg.open(QIODevice::WriteOnly) && saved_svg.write(svg_bytes)==svg_bytes.size(),"failed dimension SVG diagnostic must save");
            QFile pdf_input(pdf);require(pdf_input.open(QIODevice::ReadOnly),"failed dimension PDF diagnostic must reopen");
            const auto pdf_bytes=pdf_input.readAll();QFile saved_pdf(QDir(directory).filePath("measured-dimensions-failed.pdf"));
            require(saved_pdf.open(QIODevice::WriteOnly) && saved_pdf.write(pdf_bytes)==pdf_bytes.size(),"failed dimension PDF diagnostic must save");
        }
        throw std::runtime_error("SVG and PDF must contain persisted measured dimension text: "+evidence.dump());
    }
    capture(window,"styled-measured-dimensions.png");const auto capture_dir=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!capture_dir.isEmpty()) {
        const auto paper=output.pagePointSize(0);
        require(output.render(0,{1000,static_cast<int>(std::lround(1000*paper.height()/paper.width()))}).save(QDir(capture_dir).filePath("styled-measured-dimensions-pdf.png")),"actual PDF capture must preserve page aspect ratio and save");
    }
    require(window.selectEntity("open")&&window.copySelection(),"native Copy includes saved measured dimensions");MainWindow target;prepare(target);const auto empty=target.document().snapshot();
    if(!target.pasteSelection())throw std::runtime_error("native measured dimension Paste: "+target.lastError().toStdString());
    const auto pasted=target.document().snapshot();std::string copied;for(const auto& [id,entity]:pasted.entities())if(entity.type=="measurement_linework")copied=id;
    require(!copied.empty()&&copied!="open"&&window.document().snapshot().entities()==after.entities(),"copy source remains exact and independent");
    const auto owned=dimensions(pasted,copied);require(owned.size()==2,"paste retains both owned semantic dimensions");
    const auto model=*decode_measurement_linework_model(pasted.entities().at(copied).properties.at("model")).model;
    std::set<std::string> segments,vertices;for(const auto& edge:model.edges){segments.insert(edge.segment_id);vertices.insert(edge.start_vertex_id);vertices.insert(edge.end_vertex_id);}
    for(const auto& item:owned){require(item.id!=length.toStdString()&&item.id!=angle.toStdString()&&segments.contains(item.segment_id)&&item.segment_id!="open:e1"&&item.segment_id!="open:e2","copied dimension and source edge IDs must be fresh");
        const auto original=item.kind==BoundaryDimensionKind::angle?moved_angle:moved_length;require(item.presentation==original.presentation,"copied presentation remains exact");
        if(item.kind==BoundaryDimensionKind::angle)require(vertices.contains(item.vertex_id)&&segments.contains(item.secondary_segment_id)&&item.vertex_id!="open:v1","copied tangent bindings must map to fresh owned geometry");
        const auto resolved=item.resolve(pasted.entities().at(copied));if(item.kind==BoundaryDimensionKind::angle)require_near(resolved.angle_radians,std::numbers::pi/2);else require_near(resolved.segment_length_metres,3);}
    exact_history(target,empty,pasted);
    require(target.selectEntity(QString::fromStdString(copied))&&target.moveSelectedBoundaryVertex(QString::fromStdString(model.edges.back().end_vertex_id),{6,4},target.document().revision()),"pasted independent source remains editable");
    require(window.document().snapshot().entities()==after.entities(),"editing copied dimensions/source cannot alter original");
    require(window.selectEntity("open"),"select source for actual Delete");const auto before_delete=window.document().snapshot();control<QAction>(window,"deleteSelection").trigger();const auto deleted=window.document().snapshot();
    require(!deleted.entities().contains("open")&&!deleted.entities().contains(length.toStdString())&&!deleted.entities().contains(angle.toStdString())&&deleted.entities().at("arc")==after.entities().at("arc"),"Delete removes owner and owned dimensions atomically, preserving unrelated sources");
    exact_history(window,before_delete,deleted);const auto file=temporary.filePath("deleted-measured-dimensions.bldproj");require(window.saveProjectAs(file),"native deleted-state project must save");
    require(window.createNewProject(),"release writer ownership before testing editable reopen");
    MainWindow reopened;prepare(reopened);require(reopened.openProject(file)&&reopened.document().is_editable()&&reopened.document().snapshot().entities()==deleted.entities(),"native reopen retains exact deletion and editable dimension history");
    if(!reopened.undoCommand())throw std::runtime_error("Undo after measured-dimension reopen: "+reopened.lastError().toStdString());
    if(reopened.document().snapshot().entities()!=after.entities()) {
        std::string difference;
        for(const auto& [id,entity]:after.entities()) {
            const auto current=reopened.document().snapshot();const auto found=current.entities().find(id);
            if(found==current.entities().end())difference+=" missing:"+id;
            else if(found->second!=entity)difference+=" changed:"+id;
        }
        throw std::runtime_error("Undo after reopen differs:"+difference);
    }
}
}
int main(int argc,char** argv){sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);sketch::desktop::configure_application_platform();QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-measured-dimensions-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try{require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled Inter must load");app.setFont(QFont("Inter",10));
        if(QCoreApplication::arguments().contains(QStringLiteral("--pdf-font-probe"))) {
            const Json evidence{{"platform",QGuiApplication::platformName().toStdString()},
                {"platform_environment",qEnvironmentVariable("QT_QPA_PLATFORM").toStdString()},
                {"application_font",pdf_font_evidence(QApplication::font())},
                {"system_font",pdf_font_evidence(QFont(QStringLiteral("Arial"),10))}};
            std::cout<<evidence.dump()<<'\n';return 0;
        }
        for(int kind=0;kind<5;++kind)saved_dimension_canvas_drag(kind);
        saved_dimension_canvas_drag(3,true);saved_dimension_canvas_drag(4,true);
        saved_dimension_canvas_drag(5,true);saved_dimension_canvas_drag(6,true);
        saved_dimension_canvas_drag(7,true);saved_dimension_canvas_drag(8,true);
        saved_dimension_canvas_drag(9,true);saved_dimension_canvas_drag(10,true);saved_dimension_canvas_drag(11,true);
        saved_dimension_canvas_drag(12,true);
        saved_dimension_canvas_drag(13,true);
        mixed_complete_partial_canvas_drag(false);mixed_complete_partial_canvas_drag(true);
        mixed_complete_partial_canvas_drag(false,false);mixed_complete_partial_canvas_drag(true,false);
        actual_creator_terminal_arc_and_revisits();connected_edit_updates_saved_values();transformed_clone_is_independent();actual_transform_preview_cancel_and_apply();mixed_canvas_move_retains_placed_dimensions();mixed_wall_anchor_move_keeps_whole_transaction_admission();automatic_placement_uses_active_workspace_scale();transform_clipboard_output_delete_and_reopen();}
    catch(const std::exception& error){std::cerr<<"measurement_linework_dimensions_desktop_tests: "<<error.what()<<'\n';return 1;}
    std::cout<<"Measured dimension desktop tests passed\n";return 0;}
