#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPdfDocument>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Entity stroke(std::string id, const std::vector<Vec2>& points, bool closed) {
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=closed;
    for (std::size_t i=1; i<points.size(); ++i) {
        const auto segment=id+":e"+std::to_string(i);
        ConstructionReceipt receipt; receipt.segment_id=segment; receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=points[i-1]; receipt.chord_end=points[i];
        model.edges.push_back({segment,id+":v"+std::to_string(i-1),
            closed&&i==points.size()-1?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
        {"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({
        {"p","property",{{"name","Measured property"}},false},
        {"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"},{"name","Areas"}},false},
        stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),
        stroke("separator",{{2,-1},{2,5}},false)
    }));
}
void test_define_areas_and_history() {
    MainWindow window(fixture()); window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900); window.show(); QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"canvas exists"); require(window.selectEntity(QStringLiteral("outline")),"select measured outline");
    const auto source=window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living"),source.revision()+1).isEmpty() &&
        window.document().snapshot().entities()==source.entities(),"stale area detection leaves measured source unchanged");
    const auto ids=window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living"),source.revision());
    require(ids.size()==2,"crossing separator must define two measured areas on the native canvas");
    const auto defined=window.document().snapshot();
    require(defined.revision()==source.revision()+1,"all derived areas commit in one undo step");
    require(defined.entities().at("outline")==source.entities().at("outline") &&
        defined.entities().at("separator")==source.entities().at("separator"),"area creation never rewrites source strokes");
    double total=0;
    for (const auto& id:ids) {
        const auto& area=defined.entities().at(id.toStdString());
        require(area.type=="measurement_boundary" && area.properties.at("classification")=="living",
            "derived areas are actual classified measurement boundaries");
        require(area.properties.at("property_id")=="p" && area.properties.at("layer_id")=="l",
            "derived areas retain source drawing context");
        require(area.extensions.contains("measurement_linework_sources"),"derived areas retain edge source lineage");
        total+=std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(area))));
    }
    require(std::abs(total-16)<1e-10,"separated measured areas exactly cover original sixteen square metres");
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities(),"one Undo removes both areas and preserves strokes");
    require(window.redoCommand() && window.document().snapshot().entities()==defined.entities(),"Redo restores exact derived boundaries and lineage");
    require(window.selectEntity(QStringLiteral("outline")),"select source again");
    const auto before_repeat=window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living")).isEmpty() &&
        window.document().snapshot().entities()==before_repeat.entities(),"repeated definition does not silently double-count identical faces");
    QTemporaryDir temp; const auto path=temp.filePath("areas.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path),"linework and defined areas save and reopen");
    require(window.document().snapshot().entities()==defined.entities(),"native storage preserves source lineage and exact boundary geometry");
    bool exterior_bottom=false;
    for (const auto& label:canvas->labels()) if (label.id==QStringLiteral("outline") && label.automatic_linear_placement) {
        const auto& placement=*label.automatic_linear_placement;
        if(placement.anchor.start.y==0 && placement.anchor.end.y==0) {
            require(placement.outward_normal.y<0 && label.position.y<0,"closed measured outline places its bottom dimension on the exterior");
            require(label.text.contains(QStringLiteral("13'")) && !label.text.contains(QStringLiteral("157.480")),
                "permanent measured dimensions use readable project units rather than unlimited decimal inches");
            exterior_bottom=true;
        }
    }
    require(exterior_bottom,"closed measured outline exposes its exterior dimension");
    const auto pdf_path=temp.filePath("measured-areas.pdf");
    require(window.exportDraftPdf(pdf_path),"defined measured areas and original strokes export through shared PDF output");
    QPdfDocument pdf;
    require(pdf.load(pdf_path)==QPdfDocument::Error::None && pdf.pageCount()>0,"measured output is a readable PDF");
    const auto rendered=pdf.render(0,pdf.pagePointSize(0).scaled(QSizeF(1200,900),Qt::KeepAspectRatio).toSize());
    require(!rendered.isNull(),"shared measured PDF output renders");
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!capture.isEmpty()) {
        QDir().mkpath(capture);
        require(window.grab().save(QDir(capture).filePath("measured-lines-defined-areas.png")),"save native canvas capture");
        require(rendered.save(QDir(capture).filePath("measured-lines-output.png")),"save actual measured PDF output capture");
    }
}
void test_ansi_profile_dimensions() {
    auto document=fixture();
    const auto source=document->snapshot();
    auto property=source.entities().at("p");
    property.properties["calculation_workflow"]="appraisal";
    property.properties["appraisal_policy"]={{"policy_kind","ansi_z765_2021"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property)},{},"Set dimension presentation profile"});
    MainWindow window(document); window.setMetricUnits(true);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"ANSI dimension canvas exists");
    bool canonical=false;
    for(const auto& label:canvas->labels()) if(label.id==QStringLiteral("outline"))
        canonical=canonical || label.text==QStringLiteral("13.1 ft (4.000 m)");
    require(canonical,"ANSI-profile measured dimensions remain tenths of feet with supplemental metric display");
    require(document->snapshot().entities().at("outline")==source.entities().at("outline"),
        "dimension display rounding does not change exact retained measurements");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-linework-area-test-"+QUuid::createUuid().toString());
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled Inter font loads");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        test_define_areas_and_history();test_ansi_profile_dimensions();std::cout<<"measurement linework area desktop checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
