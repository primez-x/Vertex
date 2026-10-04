#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/geometry_operations.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using sketch::desktop::MainWindow;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T& child(QWidget& owner,const char* name) {
    auto* value=owner.findChild<T*>(QString::fromLatin1(name));
    require(value!=nullptr,std::string("Missing actual control: ")+name);return *value;
}
void modal(QWidget& owner,const char* name,const std::function<void()>& launch,
    const std::function<void(QDialog&)>& interact) {
    std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,&owner,[&] {
        auto* dialog=owner.findChild<QDialog*>(QString::fromLatin1(name));
        try {require(dialog!=nullptr,"Expected native dialog");opened=true;interact(*dialog);}
        catch (...) {failure=std::current_exception();}
        if(dialog && dialog->isVisible())dialog->reject();
    });
    launch();if(failure)std::rethrow_exception(failure);require(opened,"Dialog must open");
}
void capture(QWidget& owner,const QString& file) {
    const auto dir=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!dir.isEmpty())require(QDir().mkpath(dir) && owner.grab().save(QDir(dir).filePath(file)),"Save actual native capture");
}

void exterior_arc_edit(bool metric,bool curved,const char* operation,const char* expression,bool clockwise) {
    QTemporaryDir directory;require(directory.isValid(),"Isolated fixture directory");
    MainWindow window({},nullptr,directory.filePath("library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.setMetricUnits(metric);
    window.resize(1180,820);window.show();QApplication::processEvents();
    QStringList walls{curved ? window.createCurvedWall({0,0},{4,0},"0.6 rad","exterior") :
        window.createStraightWall({0,0},{4,0},"exterior"),
        window.createStraightWall({4,0},{4,3},"exterior"),
        window.createStraightWall({4,3},{0,3},"exterior"),
        window.createStraightWall({0,3},{0,0},"exterior")};
    for(qsizetype i=0;i<walls.size();++i) {
        require(!walls[i].isEmpty() && window.selectEntity(walls[i]) &&
            window.editSelectedThickness(i==0 ? "200 mm" : "150 mm"),"Create physical perimeter with differing thicknesses");
    }
    for(qsizetype i=0;i<walls.size();++i)require(window.selectEntity(walls[i],i!=0),"Select physical source loop");
    const auto area=window.createMeasurementBoundaryFromSelectedWalls();require(!area.isEmpty(),window.lastError().toStdString());
    const auto facts=QStringLiteral(R"({"appraisal_policy":{"policy_kind":"ansi_z765_2021","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior","ansi":{"interior_inspected":true,"direct_measurement":true,"acquisition_increment":"tenth_foot","limitations_statement":"All portions directly measured in this fixture."}},"floor_appraisal_facts":{"grade":"above","ansi":{"any_part_below_grade":false}},"appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area","ansi":{"year_round_suitable":true,"finish_matches_dwelling":true,"dwelling_identity":"primary","ceiling":{"kind":"flat","minimum_height_m":2.4384}}}})");
    require(window.editSelectedAppraisalFacts(facts),"Declare explicit appraisal facts");
    modal(window,"appraisalSetupDialog",[&]{child<QPushButton>(window,"appraisalDetailsSetup").click();},[&](QDialog& dialog) {
        child<QCheckBox>(dialog,"appraisalSetupEnabled").setChecked(true);
        child<QDialogButtonBox>(dialog,"appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(dialog.result()==QDialog::Accepted,"Enable declared appraisal profile");
    });
    require(child<QLabel>(window,"appraisalDetailsTrace").text().contains("Boundary dimensions"),
        "Enabling appraisal follows the already selected measured area into Details");
    const auto before=window.document().snapshot();
    const auto original=sketch::decode_identified_boundary_entity(before.entities().at(area.toStdString()));
    const auto selected=curved ? std::find_if(original.segments.begin(),original.segments.end(),[](const auto& edge){return edge.segment.sweep_radians!=0;}) : original.segments.begin();
    require(selected!=original.segments.end(),"Selected analytical edge exists");
    const auto property=before.entities().at(area.toStdString()).properties.at("property_id").get<std::string>();
    const auto report_before=sketch::build_appraisal_document_report(before,property);
    require(report_before.qualified && report_before.calculation,"Fixture begins with declared GLA");
    const auto name=QStringLiteral("exterior-curve-%1-%2-%3").arg(metric?"metric":"imperial",curved?"arc":"line",operation);
    const auto edit=[&](bool apply) {
        require(window.selectEntity(area),"Select measured exterior");
        modal(window,"boundaryGeometryDialog",[&]{child<QPushButton>(window,"editBoundaryGeometry").click();},[&](QDialog& dialog) {
            auto& edge=child<QComboBox>(dialog,"boundaryEdge");const auto index=edge.findData(QString::fromStdString(selected->segment_id));
            require(index>=0,"Editor exposes stable selected source edge");edge.setCurrentIndex(index);
            auto& mode=child<QComboBox>(dialog,"boundaryEditOperation");
            const auto mode_index=mode.findData(QString::fromLatin1(operation));
            require(mode_index>=0,"Actual editor offers the requested curve construction");mode.setCurrentIndex(mode_index);
            require(!child<QComboBox>(dialog,"boundaryFixedEndpoint").isEnabled() &&
                !child<QCheckBox>(dialog,"boundaryMoveConnected").isEnabled(),"Curve reconstruction fixes both measured chord endpoints");
            require(child<QCheckBox>(dialog,"boundaryMoveRelatedObjects").isEnabled(),"Source-aware curve editing offers related-object movement");
            child<QCheckBox>(dialog,"boundaryMoveRelatedObjects").setChecked(false);
            child<QCheckBox>(dialog,"boundaryCurveClockwise").setChecked(clockwise);
            child<QLineEdit>(dialog,"boundaryEdgeLength").setText(QString::fromLatin1(expression));
            auto& buttons=child<QDialogButtonBox>(dialog,"boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"Source-aware curve proposal refused: "+child<QLabel>(dialog,"boundaryGeometryStatus").text().toStdString());
            require(child<QTableWidget>(dialog,"boundaryGeometryChanges").rowCount()>=4 &&
                window.document().snapshot().entities()==before.entities(),"Preview shows physical consequences without committing");
            capture(dialog,name+(apply?"-apply.png":"-cancel.png"));
            buttons.button(apply?QDialogButtonBox::Apply:QDialogButtonBox::Cancel)->click();
        });
    };
    edit(false);require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),"Cancel retains exact original source and history");
    edit(true);const auto after=window.document().snapshot();
    const auto current=sketch::decode_identified_boundary_entity(after.entities().at(area.toStdString()));
    const auto rebuilt=std::find_if(current.segments.begin(),current.segments.end(),[&](const auto& edge){return edge.segment_id==selected->segment_id;});
    require(rebuilt!=current.segments.end(),"Curve reconstruction retains the selected stable edge");
    require(after.revision()==before.revision()+1 &&
        std::hypot(rebuilt->segment.start.x-selected->segment.start.x,rebuilt->segment.start.y-selected->segment.start.y)<1e-6 &&
        std::hypot(rebuilt->segment.end.x-selected->segment.end.x,rebuilt->segment.end.y-selected->segment.end.y)<1e-6 &&
        std::abs(rebuilt->segment.sweep_radians-selected->segment.sweep_radians)>1e-6,"One Apply fixes both chord endpoints and changes analytical curvature");
    require(sketch::wall_measurement_source_current(after,after.entities().at(area.toStdString())),"Measured outline remains derived from actual edited physical walls");
    const auto& proof=after.history().back().boundary_constraint_changes;
    require(proof && proof->exterior_segment_arc,"Saved proof retains explicit source-aware curvature authority");
    const auto& receipt=proof->exterior_segment_arc->arc_construction;
    require(receipt.start.x==selected->segment.start.x && receipt.start.y==selected->segment.start.y &&
        receipt.chord_end && receipt.chord_end->x==selected->segment.end.x && receipt.chord_end->y==selected->segment.end.y,
        "Saved construction retains the original exact measured chord");
    if(std::string_view(operation)=="angle")require(receipt.angle && *receipt.angle==sketch::parse_angle(expression) &&
        std::abs(rebuilt->segment.sweep_radians-receipt.angle->radians)<1e-7,"Actual angle edit retains and realizes the entered signed angle");
    else if(std::string_view(operation)=="height")require(receipt.height && receipt.height->original_expression==expression,
        "Actual height edit retains the entered expression");
    else require(receipt.arc_length && receipt.arc_length->original_expression==expression && receipt.clockwise==clockwise &&
        std::abs(sketch::segment_length(rebuilt->segment)-receipt.arc_length->metres)<1e-6,"Actual arc-length edit realizes its entered physical length and side");
    const auto expected=sketch::reconstruct_boundary_arc(original,selected->segment_id,receipt);
    require(expected.segments.size()==current.segments.size(),"Complete measured topology remains stable");
    for(std::size_t i=0;i<expected.segments.size();++i) {
        const auto& a=expected.segments[i]; const auto& b=current.segments[i];
        require(a.segment_id==b.segment_id && a.start_vertex_id==b.start_vertex_id && a.end_vertex_id==b.end_vertex_id &&
            std::hypot(a.segment.start.x-b.segment.start.x,a.segment.start.y-b.segment.start.y)<1e-6 &&
            std::hypot(a.segment.end.x-b.segment.end.x,a.segment.end.y-b.segment.end.y)<1e-6 &&
            std::abs(a.segment.sweep_radians-b.segment.sweep_radians)<1e-7,"Every final exterior edge realizes the complete fixed-chord target");
    }
    const auto report_after=sketch::build_appraisal_document_report(after,property);
    require(report_after.qualified && report_after.calculation &&
        report_after.calculation->property.gla().total.display.text!=report_before.calculation->property.gla().total.display.text,"Declared GLA recalculates from reconstructed exterior");
    require(child<QLabel>(window,"appraisalDetailsTrace").text().contains("Boundary dimensions") &&
        child<QLabel>(window,"appraisalDetailsTrace").text().contains("Calculation trace"),
        "Details displays the reconstructed analytical dimensions and calculation trace");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(2);QApplication::processEvents();
    capture(window,name+"-details.png");
    capture(child<QLabel>(window,"appraisalDetailsTrace"),name+"-dimensions.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),"Whole source curve reconstruction Undo/Redo is atomic");
    const auto file=directory.filePath(name+".bldproj");require(window.saveProjectAs(file) && window.createNewProject(),"Save edited project and release writer lease");
    const auto evidence=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!evidence.isEmpty())require(QFile::copy(file,QDir(evidence).filePath(name+".bldproj")),"Retain actual native40 fixture for transfer-package verification");
    MainWindow reopened({},nullptr,directory.filePath("other-library.json"));
    require(reopened.openProject(file) && reopened.document().is_editable() && reopened.document().snapshot().entities()==after.entities(),"Source curve reconstruction retains editable ownership-safe reopen");
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities() &&
        reopened.redoCommand() && reopened.document().snapshot().entities()==after.entities(),"Restored typed history exactly replays");
    // Whole-area reflection is a grouped transform in the real editor. It
    // transforms the physical sources and measured owner together; endpoint
    // authoring intentionally retains corner adjacency and is not that API.
    require(reopened.selectEntity(area) &&
        reopened.transformSelectedBoundary("45",true,false,"2 m","1 m",false),
        "Reflect and rotate the reconstructed area through its actual editor: "+reopened.lastError().toStdString());
    const auto reflected=reopened.document().snapshot();
    require(sketch::wall_measurement_source_current(reflected,reflected.entities().at(area.toStdString())),
        "Actual grouped reflection keeps reconstructed physical sources current");
    for(const auto& id:walls) {
        const auto& old_wall=after.entities().at(id.toStdString());
        if(!old_wall.extensions.contains("curve_input_derivation"))continue;
        const auto& old_archive=old_wall.extensions.at("curve_input_derivation");
        const auto& archive=reflected.entities().at(id.toStdString()).extensions.at("curve_input_derivation");
        const auto expected_version=old_archive.at("version")==3 ? 3 : 2;
        require(archive.at("version")==expected_version &&
            archive.at("source_input")==old_archive.at("source_input") &&
            archive.at("source_baseline")==old_archive.at("source_baseline") &&
            archive.at("operations").size()==old_archive.at("operations").size()+1,
            "Editor reflection preserves the original source and appends one rigid operation");
        for(std::size_t index=0;index<old_archive.at("operations").size();++index)
            require(archive.at("operations")[index]==old_archive.at("operations")[index],
                "Editor reflection cannot rewrite any earlier curve operation");
        require(archive.at("operations").back().at("kind")=="rigid_transform",
            "Editor reflection appends a typed rigid operation");
    }
    const auto reflected_report=sketch::build_appraisal_document_report(reflected,property);
    require(reflected_report.qualified && reflected_report.calculation &&
        reflected_report.calculation->property.gla().total.display.text==report_after.calculation->property.gla().total.display.text,
        "Rotation and reflection retain the area and its declared GLA");
    const auto reflected_file=directory.filePath(name+"-reflected.bldproj");
    require(reopened.saveProjectAs(reflected_file) && reopened.createNewProject(),"Save reflected area and release writer lease");
    MainWindow final_window({},nullptr,directory.filePath("final-library.json"));
    require(final_window.openProject(reflected_file) && final_window.document().is_editable() &&
        final_window.document().snapshot().entities()==reflected.entities() &&
        final_window.undoCommand() && final_window.document().snapshot().entities()==after.entities() &&
        final_window.redoCommand() && final_window.document().snapshot().entities()==reflected.entities(),
        "Actual grouped reflection reopens editable and replays exact source history");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);QStandardPaths::setTestModeEnabled(true);
    QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    try {exterior_arc_edit(false,false,"angle","30 deg",false);
        exterior_arc_edit(true,true,"angle","-30 deg",false);
        exterior_arc_edit(true,false,"height","6 in",false);
        exterior_arc_edit(false,true,"height","-6 in",false);
        exterior_arc_edit(false,false,"arc_length","15 ft",false);
        exterior_arc_edit(true,true,"arc_length","15 ft",true);
        std::cout<<"exterior_boundary_arc_edit_desktop passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"exterior_boundary_arc_edit_desktop: "<<error.what()<<'\n';return 1;}
}
