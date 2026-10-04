#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/wall_measurement.hpp"
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

void exterior_resize(bool metric,bool curved,bool end_anchor,bool chain) {
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
    const auto anchored=end_anchor ? selected->segment.end : selected->segment.start;
    const auto property=before.entities().at(area.toStdString()).properties.at("property_id").get<std::string>();
    const auto report_before=sketch::build_appraisal_document_report(before,property);
    require(report_before.qualified && report_before.calculation,"Fixture begins with declared GLA");
    const auto name=QStringLiteral("exterior-resize-%1-%2-%3-%4").arg(metric?"metric":"imperial",curved?"arc":"line",end_anchor?"end":"start",chain?"chain":"local");
    const auto edit=[&](bool apply) {
        require(window.selectEntity(area),"Select measured exterior");
        modal(window,"boundaryGeometryDialog",[&]{child<QPushButton>(window,"editBoundaryGeometry").click();},[&](QDialog& dialog) {
            auto& edge=child<QComboBox>(dialog,"boundaryEdge");const auto index=edge.findData(QString::fromStdString(selected->segment_id));
            require(index>=0,"Editor exposes stable selected source edge");edge.setCurrentIndex(index);
            auto& fixed=child<QComboBox>(dialog,"boundaryFixedEndpoint");fixed.setCurrentIndex(end_anchor?1:0);
            child<QCheckBox>(dialog,"boundaryMoveConnected").setChecked(chain);
            child<QCheckBox>(dialog,"boundaryMoveRelatedObjects").setChecked(false);
            child<QLineEdit>(dialog,"boundaryEdgeLength").setText("5 m");
            auto& buttons=child<QDialogButtonBox>(dialog,"boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"Source-aware resize proposal refused: "+child<QLabel>(dialog,"boundaryGeometryStatus").text().toStdString());
            require(child<QTableWidget>(dialog,"boundaryGeometryChanges").rowCount()>=4 &&
                window.document().snapshot().entities()==before.entities(),"Preview shows physical consequences without committing");
            capture(dialog,name+(apply?"-apply.png":"-cancel.png"));
            buttons.button(apply?QDialogButtonBox::Apply:QDialogButtonBox::Cancel)->click();
        });
    };
    edit(false);require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),"Cancel retains exact original source and history");
    edit(true);const auto after=window.document().snapshot();
    const auto current=sketch::decode_identified_boundary_entity(after.entities().at(area.toStdString()));
    const auto resized=std::find_if(current.segments.begin(),current.segments.end(),[&](const auto& edge){return edge.segment_id==selected->segment_id;});
    require(resized!=current.segments.end(),"Resize retains the selected stable edge");
    const auto new_anchor=end_anchor?resized->segment.end:resized->segment.start;
    require(after.revision()==before.revision()+1 && std::abs(sketch::segment_length(resized->segment)-5)<1e-6 &&
        std::hypot(new_anchor.x-anchored.x,new_anchor.y-anchored.y)<1e-6 &&
        std::abs(resized->segment.sweep_radians-selected->segment.sweep_radians)<1e-8,"One Apply satisfies measured length, anchor and signed sweep");
    require(sketch::wall_measurement_source_current(after,after.entities().at(area.toStdString())),"Measured outline remains derived from actual edited physical walls");
    const auto& proof=after.history().back().boundary_constraint_changes;
    require(proof && proof->exterior_segment_resize && proof->exterior_segment_resize->exact_length.original_expression=="5 m","Saved proof retains entered quantity and explicit source authority");
    const auto report_after=sketch::build_appraisal_document_report(after,property);
    require(report_after.qualified && report_after.calculation &&
        report_after.calculation->property.gla().total.display.text!=report_before.calculation->property.gla().total.display.text,"Declared GLA recalculates from resized exterior");
    require(child<QLabel>(window,"appraisalDetailsTrace").text().contains("Boundary dimensions") &&
        child<QLabel>(window,"appraisalDetailsTrace").text().contains("16.4 ft"),
        "Details displays the actual resized edge using the declared ANSI tenth-foot presentation");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(2);QApplication::processEvents();
    capture(window,name+"-details.png");
    capture(child<QLabel>(window,"appraisalDetailsTrace"),name+"-dimensions.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),"Whole source resize Undo/Redo is atomic");
    const auto file=directory.filePath(name+".bldproj");require(window.saveProjectAs(file) && window.createNewProject(),"Save edited project and release writer lease");
    const auto evidence=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!evidence.isEmpty())require(QFile::copy(file,QDir(evidence).filePath(name+".bldproj")),"Retain actual native39 fixture for transfer-package verification");
    MainWindow reopened({},nullptr,directory.filePath("other-library.json"));
    require(reopened.openProject(file) && reopened.document().is_editable() && reopened.document().snapshot().entities()==after.entities(),"Source resize retains editable ownership-safe reopen");
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities() &&
        reopened.redoCommand() && reopened.document().snapshot().entities()==after.entities(),"Restored typed history exactly replays");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);QStandardPaths::setTestModeEnabled(true);
    QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    try {exterior_resize(false,true,false,true);exterior_resize(true,true,true,false);
        exterior_resize(true,false,false,false);exterior_resize(false,false,true,true);
        std::cout<<"exterior_boundary_resize_desktop passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"exterior_boundary_resize_desktop: "<<error.what()<<'\n';return 1;}
}
