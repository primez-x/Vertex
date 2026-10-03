#include "sketch/constraint_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/wall_measurement.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace sketch;
using namespace sketch::desktop;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<PersistentConstraint> coincident_constraints(const DocumentSnapshot& snapshot) {
    std::vector<PersistentConstraint> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type != "constraint") continue;
        const auto decoded = decode_constraint_entity(entity);
        require(decoded.supported(), "auto-authored wall constraint decodes as a supported relation");
        if (decoded.constraint->relation == ConstraintRelationKind::coincident)
            result.push_back(*decoded.constraint);
    }
    return result;
}

std::vector<std::string> wall_ids(const DocumentSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& [id, entity] : snapshot.entities())
        if (entity.type == "wall") result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
}

Segment wall_baseline(const Entity& entity) {
    const auto& baseline = entity.properties.at("baseline");
    return {{baseline.at("start").at(0).get<double>(), baseline.at("start").at(1).get<double>()},
            {baseline.at("end").at(0).get<double>(), baseline.at("end").at(1).get<double>()},
            baseline.at("sweep_radians").get<double>()};
}

void test_public_wall_creation_persists_one_atomic_connection() {
    MainWindow window;
    window.setMetricUnits(true);
    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    require(!first.isEmpty(), "first public straight wall is created");
    const auto before_second = window.document().snapshot();

    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!second.isEmpty(), "second public straight wall is created");
    const auto connected = window.document().snapshot();
    const auto constraints = coincident_constraints(connected);
    require(constraints.size() == 1,
            "public straight-wall creation persists one coincident relation at a shared endpoint");
    const auto& relation = constraints.front();
    require(relation.bindings.size() == 2 &&
                relation.bindings[0].owner_id == first.toStdString() &&
                relation.bindings[0].role == WallEndpointRole::end &&
                relation.bindings[1].owner_id == second.toStdString() &&
                relation.bindings[1].role == WallEndpointRole::start,
            "auto-authored relation binds the exact shared endpoint pair");
    require(connected.revision() == before_second.revision() + 1,
            "new wall and its persistent relation share one document history command");
    const auto* navigator = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(navigator, "wall authoring retains its layer navigator");
    for (QTreeWidgetItemIterator item(const_cast<QTreeWidget*>(navigator)); *item; ++item)
        require((*item)->text(0) != QStringLiteral("Constraint") &&
                    (*item)->text(0) != QStringLiteral("Unassigned / unresolved"),
                "wall connections do not expose internal constraint records in the layer navigator");

    require(window.undoCommand() &&
                window.document().snapshot().entities() == before_second.entities(),
            "one undo removes the new wall and its connection together");
    require(window.redoCommand() &&
                window.document().snapshot().entities() == connected.entities(),
            "one redo restores the new wall and its connection together");

    QTemporaryDir directory;
    require(directory.isValid(), "connection persistence uses a temporary project directory");
    const auto path = directory.filePath(QStringLiteral("connected-walls.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "connected wall project saves and reopens");
    const auto reopened = window.document().snapshot();
    require(coincident_constraints(reopened).size() == 1,
            "shared endpoint relation survives project save and reopen");

    require(window.selectEntity(first) && window.editSelectedLength(QStringLiteral("4 m")),
            "a connected wall length can be edited after reopening");
    const auto resized = window.document().snapshot();
    const auto first_baseline = wall_baseline(resized.entities().at(first.toStdString()));
    const auto second_baseline = wall_baseline(resized.entities().at(second.toStdString()));
    require(std::abs(first_baseline.end.x - 4.0) < 1e-8 &&
                std::abs(first_baseline.end.y) < 1e-8 &&
                std::hypot(second_baseline.start.x - first_baseline.end.x,
                           second_baseline.start.y - first_baseline.end.y) < 1e-8,
            "resizing one wall moves its adjacent shared endpoint through the persisted relation");

    require(window.selectEntity(second) && window.deleteSelection(),
            "a connected wall can be deleted through normal selection removal");
    const auto after_delete = window.document().snapshot();
    require(after_delete.entities().contains(first.toStdString()) &&
                !after_delete.entities().contains(second.toStdString()) &&
                coincident_constraints(after_delete).empty(),
            "deleting a connected wall removes only its dangling relation and preserves its neighbor");
}

void test_wall_connections_respect_layer_and_phase_contexts() {
    {
        MainWindow window;
        const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
        require(!first.isEmpty(), "layer-scope fixture wall is created");
        const auto active_layer = window.activeLayerId().toStdString();
        const auto source = window.document().snapshot();
        const auto floor = source.entities().at(active_layer).properties.at("floor_id").get<std::string>();
        const auto other_layer = window.createLayer(QString::fromStdString(floor),
                                                    QStringLiteral("Separate wall layer"));
        require(!other_layer.isEmpty() && window.activeLayerId() == other_layer,
                "layer-scope fixture selects a separate layer on the same floor");
        const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
        require(!second.isEmpty() && coincident_constraints(window.document().snapshot()).empty(),
                "coincident walls on different layers remain unconnected");
    }

    {
        MainWindow window;
        const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
        require(!first.isEmpty(), "phase-scope fixture wall is created");
        const auto first_id = first.toStdString();
        const auto phases = ModelPhases::create(
            {first_id}, {first_id},
            {RemodelingAlternative{"demolish-host", "Demolish host", {first_id}, {}}});
        auto phase_record = Entity::create("model_phases", {{"model", phases.to_json()}});
        phase_record.id = "model-phases-wall-connection-test";
        window.document().apply(ApplyEntityChanges{
            window.document().revision(), {EntityChange::upsert(std::move(phase_record))}, {},
            "Prepare phase-scoped wall connection fixture"});
        require(window.selectRemodelingAlternative(QStringLiteral("demolish-host")),
                "phase-scope fixture selects the alternative that demolishes the existing wall");

        const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
        require(!second.isEmpty() && coincident_constraints(window.document().snapshot()).empty(),
                "a demolished wall from another active phase is not auto-connected");
    }
}

void test_interactive_wall_chain_closure_finishes_at_its_start() {
    MainWindow window;
    window.resize(1200, 800);
    // The fixture's two-metre coordinates belong to the metric grid. In
    // Imperial, the first click rounds elsewhere and returning to its raw
    // coordinate is not clicking the actual authored endpoint.
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
    require(canvas && create_wall, "interactive closure fixture exposes its plan and wall command");
    create_wall->trigger();
    QApplication::processEvents();

    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const auto model_to_canvas = [&](Vec2 point) {
        const auto center = canvas->viewCenter();
        const auto scale = canvas->viewScale();
        const auto viewport = QRectF(canvas->rect());
        return QPointF(viewport.center().x() + (point.x - center.x) * scale,
                       viewport.center().y() - (point.y - center.y) * scale);
    };
    const auto click = [&](Vec2 point) {
        const auto screen = model_to_canvas(point);
        mouse(QEvent::MouseMove, screen, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, screen, Qt::LeftButton, Qt::NoButton);
    };

    click({-2.0, -2.0});
    click({2.0, -2.0});
    click({2.0, 2.0});
    click({-2.0, -2.0});
    require(wall_ids(window.document().snapshot()).size() == 3,
            "clicking the original chain start commits the closing wall segment");
    require(!canvas->wallPreview().has_value(),
            "closing the wall loop retires its draft preview and finishes the chain");

    click({5.0, 4.0});
    require(wall_ids(window.document().snapshot()).size() == 3,
            "a click after loop closure cannot begin an unintended fourth segment");
}

void test_automatic_appraisal_wall_chain_closure(bool survey_only=false) {
    const auto run = [](bool metric, const char* policy_kind, const char* property_kind,
                        const char* workflow, const char* basis, bool close_chain, int existing_region = 0,
                        bool curved_closure = false) {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.resize(1200,800);
        auto property = window.document().snapshot().entities().at("property-1");
        property.properties["calculation_workflow"] = workflow;
        property.properties["appraisal_policy"] = {{"policy_kind",policy_kind},{"version",1},
            {"property_kind",property_kind},{"measurement_basis",basis}};
        if (std::string(policy_kind)=="ansi_z765_2021")
            property.properties["appraisal_policy"]["ansi"] = {{"interior_inspected",true},{"direct_measurement",true},
                {"acquisition_increment","tenth_foot"},{"limitations_statement","Fixture explicitly declares measurement acquisition only."}};
        window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(property)}, {},
            "Configure declared appraisal closure fixture"});
        window.setMetricUnits(metric);
        window.setWorkspace(Workspace::measurement);
        QString existing_area;
        if (existing_region!=0) {
            const auto drawing_layer = window.activeLayerId();
            QString hidden_layer;
            if (existing_region==3) {
                const auto source = window.document().snapshot();
                const auto floor = source.entities().at(drawing_layer.toStdString()).properties.at("floor_id").get<std::string>();
                hidden_layer = window.createLayer(QString::fromStdString(floor),"Hidden existing area");
                require(!hidden_layer.isEmpty(),"overlap fixture creates another layer on the drawing floor");
            }
            const Vec2 low = existing_region==1 || existing_region==4 ? Vec2{-4,-3} : existing_region==2 ? Vec2{-1,-0.5} : Vec2{-1,-2};
            const Vec2 high = existing_region==1 || existing_region==4 ? Vec2{4,3} : existing_region==2 ? Vec2{1,0.5} : Vec2{3,1};
            existing_area = window.createBoundary({{low,{high.x,low.y},0},{{high.x,low.y},high,0},
                {high,{low.x,high.y},0},{{low.x,high.y},low,0}});
            require(!existing_area.isEmpty(),"automatic closure overlap fixture has an existing measured owner");
            if (existing_region==4) {
                auto parcel=window.document().snapshot().entities().at(existing_area.toStdString());
                parcel.properties["classification"]="survey";
                parcel.properties["measurement_classification"]="survey";
                parcel.properties.erase("calculation_scope");
                window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(parcel)}, {},
                    "Legacy parcel inherits survey site scope"});
            }
            if (!hidden_layer.isEmpty())
                require(window.setContainerVisible(hidden_layer,false) && window.setActiveLayer(drawing_layer),
                    "hidden same-floor measurement retains its semantic ownership while physical drawing uses another layer");
        }
        window.show();
        QApplication::processEvents();
        auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
        auto* create_wall = window.findChild<QAction*>("createWall");
        auto* snap = window.findChild<QToolButton*>("snapTool");
        require(canvas && create_wall && snap,"automatic appraisal fixture exposes actual canvas wall authoring");
        // Coordinates are exact canonical metres in both display systems.
        snap->setChecked(false);
        create_wall->trigger();
        const auto click = [&](Vec2 point) {
            const auto center = QRectF(canvas->rect()).center();
            const auto view = canvas->viewCenter();
            const QPointF screen{center.x()+(point.x-view.x)*canvas->viewScale(),
                                 center.y()-(point.y-view.y)*canvas->viewScale()};
            QMouseEvent move(QEvent::MouseMove,screen,canvas->mapToGlobal(screen.toPoint()),
                Qt::NoButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&move);
            QMouseEvent press(QEvent::MouseButtonPress,screen,canvas->mapToGlobal(screen.toPoint()),
                Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&press);
            QMouseEvent release(QEvent::MouseButtonRelease,screen,canvas->mapToGlobal(screen.toPoint()),
                Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&release);
        };
        const auto measured_ids = [](const DocumentSnapshot& snapshot) {
            std::vector<std::string> ids;
            for (const auto& [id,entity] : snapshot.entities())
                if (entity.type=="measurement_boundary") ids.push_back(id);
            return ids;
        };
        click({-2,-1.5}); click({2,-1.5}); click({2,1.5}); click({-2,1.5});
        const auto open = window.document().snapshot();
        require(wall_ids(open).size()==3 && measured_ids(open).size()==(existing_region==0 ? 0U : 1U),
            "open appraisal wall chain must not create a premature area");
        if (!close_chain) {
            QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
            QApplication::sendEvent(canvas,&cancel);
            require(window.document().snapshot().entities()==open.entities() && measured_ids(window.document().snapshot()).empty() &&
                    !canvas->wallPreview(),"aborted appraisal chain preserves committed walls and creates no area");
            return;
        }
        if (curved_closure) {
            bool seen = false;
            std::exception_ptr failure;
            QTimer::singleShot(0,&window,[&] {
                auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                try {
                    auto* input = dynamic_cast<BoundaryInputDialog*>(modal);
                    require(input,"D on an active wall chain must open real precise analytical input");
                    input->setAttribute(Qt::WA_DontShowOnScreen,true);
                    seen = true;
                    auto* method = input->findChild<QComboBox*>("boundaryInputMethod");
                    auto* x = input->findChild<QLineEdit*>("boundaryInputEndX");
                    auto* y = input->findChild<QLineEdit*>("boundaryInputEndY");
                    auto* sweep = input->findChild<QLineEdit*>("boundaryInputSweep");
                    require(method && x && y && sweep,"precise closure exposes chord endpoint and signed sweep fields");
                    method->setCurrentIndex(4);
                    x->setText("-2 m"); y->setText("-1.5 m"); sweep->setText("90 deg");
                    require(input->submit(),"precise analytical arc closing at original anchor must validate");
                } catch (...) {
                    failure = std::current_exception();
                    if (modal) modal->reject();
                }
            });
            QKeyEvent precise(QEvent::KeyPress,Qt::Key_D,Qt::NoModifier);
            QApplication::sendEvent(canvas,&precise);
            QApplication::processEvents();
            if (failure) std::rethrow_exception(failure);
            require(seen,"real wall drawing precision action must execute its closure dialog");
        } else click({-2,-1.5});
        const auto closed = window.document().snapshot();
        const auto sources = wall_ids(closed);
        require(sources.size()==4 && !canvas->wallPreview(),"actual close click commits four walls and retires the drawing chain");
        const bool automatic = std::string(workflow)=="appraisal" && std::string(basis)=="exterior";
        const auto areas = measured_ids(closed);
        if (existing_region>=1 && existing_region<=3) {
            require(areas.size()==1 && areas.front()==existing_area.toStdString() &&
                    closed.entities().at(existing_area.toStdString())==open.entities().at(existing_area.toStdString()) &&
                    closed.revision()==open.revision()+1 &&
                    window.statusBar()->currentMessage().contains(QStringLiteral("Automatic exterior measurement was not created")),
                "nested, enclosing or hidden same-floor overlapping area must retain physical closure, explain refusal and avoid a duplicate contributor");
            return;
        }
        if (!automatic) {
            require(areas.empty() && closed.revision()==open.revision()+1,
                "measurement workflow and interior basis retain physical closure without automatic appraisal area");
            return;
        }
        if (areas.size()!=(existing_region==4 ? 2U : 1U))
            throw std::runtime_error("Automatic closure lacks a measured owner (policy="+std::string(policy_kind)+
                ", curved="+std::to_string(curved_closure)+", region="+std::to_string(existing_region)+
                "): "+window.statusBar()->currentMessage().toStdString()+"; "+window.lastError().toStdString());
        const auto measured=std::find_if(areas.begin(),areas.end(),[&](const auto& id){return id!=existing_area.toStdString();});
        require(measured!=areas.end(),"closed walls create their own measured owner without replacing a parcel");
        const auto& owner = closed.entities().at(*measured);
        require(closed.revision()==open.revision()+1 && wall_measurement_source_current(closed,owner) &&
                exterior_wall_measurement_source_ids(owner)==sources,
            "closing wall, current analytical exterior source and area must share one history revision");
        const double thickness = closed.entities().at(sources.front()).properties.at("thickness_m").get<double>();
        for (const auto& id : sources)
            require(std::abs(closed.entities().at(id).properties.at("thickness_m").get<double>()-thickness)<1e-12,
                "rectangular exterior fixture uses a consistent physical thickness");
        const auto boundary = decode_identified_boundary_entity(owner);
        const double area = std::abs(signed_area(boundary_geometry(boundary)));
        if (curved_closure) {
            const auto exact = derive_exterior_wall_measurement(closed,sources);
            require(std::abs(area-std::abs(signed_area(exact.boundary)))<1e-8 &&
                    std::any_of(boundary.segments.begin(),boundary.segments.end(),[](const auto& edge){return edge.segment.sweep_radians!=0;}),
                "precise curved closure must retain exact analytical exterior geometry rather than chord approximation");
            bool precise_wall = false;
            for (const auto& id : sources) {
                const auto& wall = closed.entities().at(id);
                if (wall_baseline(wall).sweep_radians==0) continue;
                precise_wall = true;
                const auto receipt = decode_construction_receipt(wall.properties.at("original_drawing_input"));
                require(receipt.kind==BoundaryConstructionKind::arc_chord_angle && receipt.angle &&
                        receipt.angle->original_expression=="90 deg",
                    "precise automatic closure must preserve original curved-wall construction evidence");
            }
            require(precise_wall,"actual precise closure must publish one physical analytical curved wall");
        } else require(std::abs(area-(4+thickness)*(3+thickness))<1e-8 && area>12,
            "automatic measurement must use exact exterior faces rather than twelve-square-metre centerline area");
        std::size_t dimensions = 0;
        double perimeter = 0;
        bool arc_dimension = false;
        for (const auto& [id,entity] : closed.entities()) {
            (void)id;
            if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.dimension || decoded.dimension->boundary_id!=owner.id) continue;
            require(decoded.dimension->kind==BoundaryDimensionKind::segment_length &&
                    decoded.dimension->placement==BoundaryDimensionPlacement::automatic,
                "automatic exterior dimensions must retain physical analytical edge bindings");
            const auto resolution = decoded.dimension->resolve(owner);
            perimeter += resolution.segment_length_metres;
            if (resolution.segment.sweep_radians!=0) {
                arc_dimension = true;
                require(resolution.segment_length_metres>std::hypot(resolution.segment.end.x-resolution.segment.start.x,
                    resolution.segment.end.y-resolution.segment.start.y),"curved closure dimension must measure the arc rather than its chord");
            }
            ++dimensions;
        }
        require(dimensions==boundary.segments.size() && dimensions==4 &&
                (curved_closure ? arc_dimension : std::abs(perimeter-2*(7+2*thickness))<1e-8),
            "every exterior edge must have one current exact dimension");
        require(!owner.properties.contains("appraisal_facts") && !owner.properties.contains("grade") &&
                closed.entities().at("property-1")==property,
            "automatic closure must retain declared policy without inventing finish, grade, access or ANSI area facts");
        require(window.selectEntity(QString::fromStdString(owner.id)),"automatic measured owner remains selectable");
        auto* details = dynamic_cast<AppraisalDetailsPanel*>(window.findChild<QWidget*>("appraisalDetailsPanel"));
        auto* gla = window.findChild<QLabel*>("appraisalDetailsGla");
        require(details && details->report() && !details->report()->qualified && gla && gla->text()=="Totals unavailable",
            "Details must remain unqualified until actual area and floor declarations are supplied");
        auto* tabs = window.findChild<QTabWidget*>("sidebarTabs");
        require(tabs,"automatic appraisal UI retains the real Details tab");
        tabs->setCurrentIndex(2);
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture_directory.isEmpty()) {
            require(QDir().mkpath(capture_directory),"automatic appraisal captures use requested artifact directory");
            const auto stem = QStringLiteral("automatic-appraisal-%1-%2%3").arg(metric ? "metric" : "imperial")
                .arg(QString::fromLatin1(policy_kind)).arg(curved_closure ? "-curved" : "");
            QApplication::processEvents();
            require(window.grab().save(QDir(capture_directory).filePath(stem+"-window.png")) &&
                    canvas->grab().save(QDir(capture_directory).filePath(stem+"-canvas.png")) &&
                    details->grab().save(QDir(capture_directory).filePath(stem+"-details.png")),
                "actual automatic owner, dimensioned canvas and unqualified Details UI must render to captures");
        }
        require(window.undoCommand() && window.document().snapshot().entities()==open.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==closed.entities(),
            "one Undo and Redo must remove and restore closing wall, area, dimensions and connections together");
        QTemporaryDir directory;
        // Reopen in the owning window; a concurrent second editor correctly
        // opens the still-locked project read-only.
        MainWindow& reopened=window;
        require(directory.isValid() && window.saveProjectAs(directory.filePath("automatic-appraisal-closure.bldproj")) &&
                reopened.openProject(directory.filePath("automatic-appraisal-closure.bldproj")) &&
                reopened.document().snapshot().entities()==closed.entities(),
            "automatic appraisal closure must save and reopen exact physical and measured entities");
        for (std::size_t index=0;index<sources.size();++index)
            require(reopened.selectEntity(QString::fromStdString(sources[index]),index!=0),"reopened exterior sources remain selectable");
        const auto before_manual = reopened.document().snapshot();
        const auto manually_measured=reopened.createMeasurementBoundaryFromSelectedWalls();
        if (manually_measured!=QString::fromStdString(owner.id))
            throw std::runtime_error("Manual exterior reuse returned '"+manually_measured.toStdString()+
                "' rather than '"+owner.id+"': "+reopened.lastError().toStdString()+
                "; revision "+std::to_string(before_manual.revision())+" -> "+std::to_string(reopened.document().revision()));
        require(manually_measured==QString::fromStdString(owner.id) &&
                reopened.document().revision()==before_manual.revision() &&
                reopened.document().snapshot().entities()==before_manual.entities(),
            "manual exterior derivation must reuse automatic owner without duplicate area or dimensions");
        if (metric && !curved_closure && std::string(policy_kind)=="residential_declared") {
            require(reopened.selectEntity(QString::fromStdString(owner.id)),"automatic area remains selectable for explicit facts");
            const nlohmann::json declarations{{"appraisal_policy",property.properties.at("appraisal_policy")},{"grade","above"},
                {"appraisal_facts",{{"finish","finished"},{"access","direct_interior"},{"ceiling_eligibility","standard"},
                    {"area_use","dwelling"},{"boundary_role","measured_area"}}}};
            require(reopened.editSelectedAppraisalFacts(QString::fromStdString(declarations.dump())),
                "actual user declarations must qualify the automatically measured exterior area");
            reopened.setMetricUnits(false);
            auto* qualified_details = dynamic_cast<AppraisalDetailsPanel*>(reopened.findChild<QWidget*>("appraisalDetailsPanel"));
            auto* qualified_gla = reopened.findChild<QLabel*>("appraisalDetailsGla");
            const auto profile = appraisal_display_profile(property.properties,AreaUnit::square_foot);
            const auto expected = display_area(area,profile);
            require(qualified_details && qualified_details->report() && qualified_details->report()->qualified &&
                    qualified_details->report()->calculation && qualified_gla &&
                    std::abs(qualified_details->report()->calculation->property.gla().total.square_metres-area)<1e-8 &&
                    qualified_gla->text()==QString::fromStdString(expected.text)+" sq ft",
                "qualified Details GLA must equal exact exterior area converted to square feet after explicit declarations");
            auto* qualified_tabs = reopened.findChild<QTabWidget*>("sidebarTabs");
            require(qualified_tabs,"qualified automatic area retains the Details tab");
            qualified_tabs->setCurrentIndex(2);
            reopened.resize(1280,900); reopened.show(); QApplication::processEvents();
            if (!capture_directory.isEmpty())
                require(reopened.grab().save(QDir(capture_directory).filePath("automatic-appraisal-closure-details.png")),
                    "qualified automatic exterior area must render its actual selected Details tab to the requested capture");
        }
    };
    if (survey_only) {
        run(true,"residential_declared","detached_single_family","appraisal","exterior",true,4);
        return;
    }
    run(true,"residential_declared","detached_single_family","appraisal","exterior",true,4);
    run(true,"residential_declared","detached_single_family","appraisal","exterior",true);
    run(false,"light_commercial_declared","light_commercial","appraisal","exterior",true);
    run(false,"ansi_z765_2021","detached_single_family","appraisal","exterior",true);
    run(true,"residential_declared","detached_single_family","measurement","exterior",true);
    run(true,"residential_declared","detached_single_family","appraisal","interior",true);
    run(true,"residential_declared","detached_single_family","appraisal","exterior",false);
    run(true,"residential_declared","detached_single_family","appraisal","exterior",true,0,true);
    for (const int existing_region : {1,2,3})
        run(true,"residential_declared","detached_single_family","appraisal","exterior",true,existing_region);
}

void test_generic_wall_api_does_not_auto_create_appraisal_area() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    auto property = window.document().snapshot().entities().at("property-1");
    property.properties["calculation_workflow"] = "appraisal";
    property.properties["appraisal_policy"] = {{"policy_kind","residential_declared"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(property)}, {},
        "Configure generic wall API appraisal fixture"});
    window.setMetricUnits(true);
    require(!window.createStraightWall({0,0},{4,0}).isEmpty() && !window.createStraightWall({4,0},{4,3}).isEmpty() &&
            !window.createStraightWall({4,3},{0,3}).isEmpty() && !window.createStraightWall({0,3},{0,0}).isEmpty(),
        "generic wall API fixture authors four physical closed walls");
    const auto source = window.document().snapshot();
    require(std::none_of(source.entities().begin(),source.entities().end(),[](const auto& value) {
        return value.second.type=="measurement_boundary";
    }),"public physical wall APIs must retain explicit area derivation instead of adopting interactive automatic closure");
}

void test_active_wall_chain_history_tracks_authoritative_endpoint(bool metric, bool workspace_history) {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200, 800);
    window.setMetricUnits(metric);
    window.show();
    QApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    auto* create_wall = window.findChild<QAction*>("createWall");
    auto* snap = window.findChild<QToolButton*>("snapTool");
    require(canvas && create_wall && snap, "active history fixture exposes wall authoring");
    snap->setChecked(false);
    create_wall->trigger();
    const auto undo = [&] {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        return QApplication::sendEvent(canvas,&event);
    };
    const auto redo = [&] {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
        return QApplication::sendEvent(canvas,&event);
    };
    const auto click = [&](Vec2 point) {
        const auto center = QRectF(canvas->rect()).center();
        const auto view = canvas->viewCenter();
        const QPointF screen{center.x()+(point.x-view.x)*canvas->viewScale(),
                             center.y()-(point.y-view.y)*canvas->viewScale()};
        QMouseEvent press(QEvent::MouseButtonPress, screen, canvas->mapToGlobal(screen.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas,&press);
        QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas->mapToGlobal(screen.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas,&release);
    };
    const auto close = [](Vec2 a, Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<1e-8; };
    if (workspace_history) {
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,"living"),
                "history fixture first uses the recoverable measured-boundary workflow");
        QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QApplication::sendEvent(canvas,&cancel);
        create_wall->trigger();
    }
    const auto initial = window.document().snapshot();
    click({0,0});
    require(undo() && window.document().snapshot().entities()==initial.entities() &&
                !canvas->wallPreview(),
            "Undo on an uncommitted wall anchor must cancel drawing without undoing project setup");
    create_wall->trigger();
    click({0,0});
    click({3,0});
    const auto first = window.document().snapshot();
    click({3,2});
    const auto second = window.document().snapshot();
    require(wall_ids(second).size()==2 && coincident_constraints(second).size()==1,
            "two live chain edges commit with an exact persisted corner");
    require(undo() && window.document().snapshot().entities()==first.entities(),
            "active wall Undo removes its last edge and joint");
    require(canvas->wallPreview() && close(canvas->wallPreview()->start,{3,0}),
            "active wall Undo must return the drawing start to the retained wall endpoint");
    require(redo() && window.document().snapshot().entities()==second.entities() &&
                canvas->wallPreview() && close(canvas->wallPreview()->start,{3,2}),
            "active wall Redo must resume from the restored wall endpoint");
    require(undo() && undo() &&
                window.document().snapshot().entities()==initial.entities() &&
                canvas->wallPreview() && close(canvas->wallPreview()->start,{0,0}),
            "undoing all live edges retains their original drawing anchor");
    require(redo() && canvas->wallPreview() && close(canvas->wallPreview()->start,{3,0}),
            "Redo after removing all chain edges restores the first continuation endpoint");
    click({4,1});
    const auto replacement = window.document().snapshot();
    require(window.lastError().isEmpty() && wall_ids(replacement).size()==2 &&
                coincident_constraints(replacement).size()==1 && !window.document().can_redo(),
            "redrawing after Undo replaces the branch without stale endpoint ownership");
    require(undo() && canvas->wallPreview() && close(canvas->wallPreview()->start,{3,0}) &&
                redo() && canvas->wallPreview() && close(canvas->wallPreview()->start,{4,1}),
            "replacement branch remains usable through further Undo and Redo");
    QKeyEvent finish(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(canvas,&finish);
    QTemporaryDir directory;
    MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("chain-history.bldproj")) &&
                reopened.openProject(directory.filePath("chain-history.bldproj")) &&
                reopened.document().snapshot().entities()==replacement.entities(),
            "continued chain and its connections survive save and reopen");
}

void test_uncommitted_wall_anchor_toolbar_undo_without_document_history() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200,800);
    window.show();
    QApplication::processEvents();
    std::vector<Entity> initial_entities;
    const auto initial_snapshot = window.document().snapshot();
    for (const auto& [id,entity] : initial_snapshot.entities()) initial_entities.push_back(entity);
    window.document() = Document::create(std::move(initial_entities));
    require(window.selectEntity({}) && !window.document().can_undo(),
            "anchor toolbar fixture starts with a valid hierarchy and no document undo records");
    QAction* undo_action = nullptr;
    for (auto* action : window.findChildren<QAction*>())
        if (action->text()==QStringLiteral("Undo")) { undo_action=action; break; }
    auto* create_wall = window.findChild<QAction*>("createWall");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(undo_action && create_wall && canvas && !undo_action->isEnabled(),
            "Undo is disabled before there is a document edit or a drawing anchor");
    create_wall->trigger();
    const auto before = window.document().snapshot();
    const QPointF point=QRectF(canvas->rect()).center();
    QMouseEvent press(QEvent::MouseButtonPress,point,canvas->mapToGlobal(point.toPoint()),
                      Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,point,canvas->mapToGlobal(point.toPoint()),
                        Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&press);
    QApplication::sendEvent(canvas,&release);
    require(canvas->wallPreview() && undo_action->isEnabled(),
            "toolbar Undo must become available for an uncommitted wall anchor without document history");
    undo_action->trigger();
    require(!canvas->wallPreview() && window.document().snapshot().entities()==before.entities() &&
                window.document().revision()==before.revision() && !undo_action->isEnabled(),
            "toolbar Undo cancels only the anchor and returns to its disabled no-history state");
}

void test_wall_and_boundary_tools_route_from_architectural_views() {
    const std::vector<QString> views{
        QStringLiteral("Plan"), QStringLiteral("Elevation"), QStringLiteral("Section · 1.2 m")};
    for (const auto& view_name : views) {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        const auto existing = window.createStraightWall({0.37, 0.37}, {3.37, 0.37});
        require(!existing.isEmpty(), "architectural routing fixture creates an endpoint snap target");
        window.setWorkspace(Workspace::architectural);
        auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
        require(architectural_view && create_wall,
                "architectural routing fixture exposes the view selector and wall command");
        architectural_view->setCurrentText(view_name);
        QApplication::processEvents();
        require(window.workspace() == Workspace::architectural &&
                    architectural_view->currentText() == view_name,
                "fixture is in the requested architectural plan, elevation, or section view");

        auto* canvas = dynamic_cast<PlanCanvas*>(
            window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        require(canvas, "architectural wall command has a conventional measurement canvas");
        create_wall->trigger();
        QApplication::processEvents();
        require(window.workspace() == Workspace::measurement && canvas->isVisible(),
                "wall authoring from every architectural view switches to the conventional 2D canvas");
        const auto existing_id = existing.toStdString();

        const auto to_screen = [&](Vec2 point) {
            const auto center = canvas->viewCenter();
            const auto scale = canvas->viewScale();
            const auto viewport = QRectF(canvas->rect());
            return QPointF(viewport.center().x() + (point.x - center.x) * scale,
                           viewport.center().y() - (point.y - center.y) * scale);
        };
        const auto click = [&](Vec2 point) {
            const auto screen = to_screen(point);
            QMouseEvent press(QEvent::MouseButtonPress, screen,
                              canvas->mapToGlobal(screen.toPoint()), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, screen,
                                canvas->mapToGlobal(screen.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &release);
        };
        const auto snap_offset = std::min(0.08, 8.0 / canvas->viewScale());
        click({3.37 + snap_offset, 0.37 + snap_offset});
        click({4.20, 2.30});
        const auto after_wall = window.document().snapshot();
        const auto created = wall_ids(after_wall);
        require(created.size() == 2, "routed wall command commits on the measurement canvas");
        const auto new_wall_id = std::find_if(created.begin(), created.end(),
            [&](const auto& id) { return id != existing_id; });
        require(new_wall_id != created.end(), "routed wall is identifiable in the committed model");
        const auto& new_wall = after_wall.entities().at(*new_wall_id);
        const auto snapped_start = wall_baseline(new_wall).start;
        require(std::hypot(snapped_start.x - 3.37, snapped_start.y - 0.37) < 1e-8,
                "routed wall command retains endpoint snapping to the existing wall");
    }

    MainWindow boundary_window;
    boundary_window.setWorkspace(Workspace::architectural);
    auto* architectural_view = boundary_window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(architectural_view, "direct boundary routing fixture exposes architectural view selection");
    architectural_view->setCurrentText(QStringLiteral("Elevation"));
    QApplication::processEvents();
    require(boundary_window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                                  QStringLiteral("living")) &&
                boundary_window.workspace() == Workspace::measurement,
            "the public boundary start path routes architectural callers to conventional 2D drawing");
}

void test_escape_keeps_committed_wall_segments() {
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
    require(canvas && create_wall, "Escape fixture exposes its plan and wall command");
    create_wall->trigger();
    QApplication::processEvents();
    const auto point = [&](Vec2 model) {
        const auto center = canvas->viewCenter();
        const auto scale = canvas->viewScale();
        const auto viewport = QRectF(canvas->rect());
        return QPointF(viewport.center().x() + (model.x - center.x) * scale,
                       viewport.center().y() - (model.y - center.y) * scale);
    };
    const auto click = [&](Vec2 model) {
        const auto screen = point(model);
        QMouseEvent press(QEvent::MouseButtonPress, screen, canvas->mapToGlobal(screen.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas->mapToGlobal(screen.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
    };
    click({0.0, 0.0});
    click({2.0, 0.0});
    const auto committed = wall_ids(window.document().snapshot());
    require(committed.size() == 1, "two clicks commit the first chain wall before Escape");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    QApplication::processEvents();
    require(wall_ids(window.document().snapshot()) == committed &&
                !canvas->wallPreview().has_value(),
            "Escape cancels only the unfinished next segment and preserves committed walls");
}

void test_sloped_wall_endpoint_snapping_respects_snap_toggle() {
    for (const bool snap_enabled : {false, true}) {
        MainWindow window;
        window.resize(1200, 800);
        window.setMetricUnits(true);
        const Vec2 start{0.37, 0.37}, end{3.37, 0.37};
        const auto existing = window.createStraightWall(start, end);
        require(!existing.isEmpty(), "sloped snap fixture creates an analytical endpoint target");
        window.show();
        window.setWorkspace(Workspace::measurement);
        QApplication::processEvents();
        auto* canvas = dynamic_cast<PlanCanvas*>(
            window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        auto* sloped_wall = window.findChild<QAction*>(QStringLiteral("slopedWall"));
        auto* snap = window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        require(canvas && sloped_wall && snap,
                "sloped wall command exposes its canvas and snap toggle");
        canvas->setOverviewMapEnabled(false);
        snap->setChecked(snap_enabled);
        sloped_wall->trigger();
        QApplication::processEvents();

        const auto screen = [&](Vec2 point) {
            const auto center = canvas->viewCenter();
            const auto viewport = QRectF(canvas->rect());
            return QPointF(viewport.center().x() + (point.x - center.x) * canvas->viewScale(),
                           viewport.center().y() - (point.y - center.y) * canvas->viewScale());
        };
        const auto mouse = [&](QEvent::Type type, Vec2 point, Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
            const auto position = screen(point);
            QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()),
                              button, buttons, Qt::NoModifier);
            QApplication::sendEvent(canvas, &event);
        };
        const auto click = [&](Vec2 point) {
            mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        };
        const auto close = [](Vec2 left, Vec2 right) {
            return std::hypot(left.x - right.x, left.y - right.y) < 1e-8;
        };
        // Off-grid coordinates distinguish real endpoint snapping from grid
        // rounding; both near misses remain within the 12 px endpoint target.
        const auto offset = std::min(0.08, 6.0 / canvas->viewScale());
        const Vec2 raw_start{start.x - offset, start.y + offset};
        const Vec2 raw_end{end.x + offset, end.y + offset};
        const auto expected_start = snap_enabled ? start : raw_start;
        const auto expected_end = snap_enabled ? end : raw_end;
        click(raw_start);
        require(canvas->wallPreview() && close(canvas->wallPreview()->start, expected_start),
                "sloped wall start uses analytical endpoint snap only while Snap is enabled");
        mouse(QEvent::MouseMove, raw_end, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview() && close(canvas->wallPreview()->end, expected_end) &&
                    !canvas->wallPreview()->dimension_text.isEmpty(),
                "sloped wall previews its effective endpoint and physical length");

        bool rise_prompt_seen = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            rise_prompt_seen = true;
            dialog->setTextValue(QStringLiteral("0.3 m"));
            dialog->accept();
        });
        click(raw_end);
        require(rise_prompt_seen, "sloped wall commit asks for its signed rise");
        const auto snapshot = window.document().snapshot();
        const auto walls = wall_ids(snapshot);
        require(walls.size() == 2, "two sloped tool clicks and accepted rise commit one wall");
        const auto created = std::find_if(walls.begin(), walls.end(),
            [&](const auto& id) { return id != existing.toStdString(); });
        require(created != walls.end(), "committed sloped wall retains a distinct identity");
        const auto baseline = wall_baseline(snapshot.entities().at(*created));
        require(close(baseline.start, expected_start) && close(baseline.end, expected_end),
                "sloped wall commit keeps the same snapped or raw endpoints as its preview");
    }
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-wall-chain-test-") +
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
    try {
        if (application.arguments().contains(QStringLiteral("--auto-appraisal-survey-only"))) {
            test_automatic_appraisal_wall_chain_closure(true);
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--auto-appraisal-closure-only"))) {
            test_automatic_appraisal_wall_chain_closure();
            test_generic_wall_api_does_not_auto_create_appraisal_area();
            std::cout << "Automatic appraisal wall chain closure workflow passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--active-chain-history-only"))) {
            for (bool metric : {false, true}) for (bool workspace_history : {false,true})
                test_active_wall_chain_history_tracks_authoritative_endpoint(metric,workspace_history);
            std::cout << "Active wall chain history workflow passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--wall-anchor-toolbar-only"))) {
            test_uncommitted_wall_anchor_toolbar_undo_without_document_history();
            std::cout << "Wall anchor toolbar workflow passed\n";
            return 0;
        }
        test_public_wall_creation_persists_one_atomic_connection();
        test_wall_connections_respect_layer_and_phase_contexts();
        test_interactive_wall_chain_closure_finishes_at_its_start();
        test_automatic_appraisal_wall_chain_closure();
        test_generic_wall_api_does_not_auto_create_appraisal_area();
        for (bool metric : {false, true}) for (bool workspace_history : {false,true})
            test_active_wall_chain_history_tracks_authoritative_endpoint(metric,workspace_history);
        test_uncommitted_wall_anchor_toolbar_undo_without_document_history();
        test_wall_and_boundary_tools_route_from_architectural_views();
        test_escape_keeps_committed_wall_segments();
        test_sloped_wall_endpoint_snapping_respects_snap_toggle();
    } catch (const std::exception& error) {
        std::cerr << "wall_chain_connection_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Wall chain connection tests passed\n";
    return 0;
}
