#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/measurement_linework.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPdfWriter>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::AppraisalReportDialog;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
sketch::Boundary square(double x, double y, double side) {
    return {{{x,y},{x+side,y},0},{{x+side,y},{x+side,y+side},0},
            {{x+side,y+side},{x,y+side},0},{{x,y+side},{x,y},0}};
}
QString declarations(const char* use="dwelling", const char* role="measured_area") {
    return QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"%1","boundary_role":"%2"}})")
        .arg(QString::fromLatin1(use),QString::fromLatin1(role));
}
struct Areas {QString parent,garage,void_id;};
Areas fixture(MainWindow& window) {
    window.setMetricUnits(true);
    Areas ids;ids.parent=window.createBoundary(square(0,0,10));require(!ids.parent.isEmpty(),"report needs real 100m² parent");
    auto* workflow=window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));require(workflow,"report needs actual workflow selector");
    const auto index=workflow->findData(QStringLiteral("appraisal"));require(index>=0,"actual workflow must offer appraisal");workflow->setCurrentIndex(index);
    require(window.editSelectedAppraisalFacts(declarations()),"declare report parent through actual MainWindow API");
    ids.garage=window.createBoundary(square(1,1,4),QStringLiteral("garage"));
    require(!ids.garage.isEmpty() && window.editSelectedAppraisalFacts(declarations("garage")) && window.applySelectedAutoSubtract(ids.parent),
        "declare and explicitly link garage deduction");
    ids.void_id=window.createBoundary(square(7,1,2));
    require(!ids.void_id.isEmpty() && window.editSelectedAppraisalFacts(declarations("dwelling","open_to_below")) &&
        window.applySelectedAutoSubtract(ids.parent),"declare and explicitly link void deduction");
    require(window.selectEntity(ids.parent),"select report parent");
    window.resize(1200,850);window.show();QApplication::processEvents();return ids;
}
template<class Widget> Widget& child(QWidget& owner,const char* name) {
    auto* result=owner.findChild<Widget*>(QString::fromLatin1(name));require(result,"actual report control must exist");return *result;
}
QTreeWidgetItem& row(QTreeWidget& tree,const QString& id) {
    for(int i=0;i<tree.topLevelItemCount();++i) if(tree.topLevelItem(i)->data(0,Qt::UserRole).toString()==id) return *tree.topLevelItem(i);
    throw std::runtime_error("report needs source-ID-addressable boundary row");
}
void report_action(MainWindow& window,const std::function<void(AppraisalReportDialog&)>& inspect) {
    auto& action=child<QAction>(window,"appraisalReport");require(action.isEnabled(),"Tools report action must be available");
    std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=dynamic_cast<AppraisalReportDialog*>(window.findChild<QDialog*>(QStringLiteral("appraisalReportDialog")));
        try {require(dialog,"Tools action must open actual report dialog");opened=true;inspect(*dialog);}
        catch(...){failure=std::current_exception();}
        if(dialog && dialog->isVisible()) dialog->reject();
    });
    action.trigger();if(failure)std::rethrow_exception(failure);require(opened,"report action must execute its dialog");
}
void capture(QWidget& widget,const QString& filename) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    QApplication::processEvents();require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(filename)),"save actual report widget capture");
}
QByteArray bytes(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"read actual retained output bytes");return file.readAll();
}
QString pdf_text(const QString& path,bool capture_pages=false) {
    QPdfDocument document;require(document.load(path)==QPdfDocument::Error::None && document.pageCount()>0,"actual appraisal PDF must reopen");
    QString result;const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    for(int page=0;page<document.pageCount();++page) {
        result+=document.getAllText(page).text()+QLatin1Char('\n');
        if(capture_pages && !directory.isEmpty()) {
            const auto rendered=document.render(page,QSize(1000,1400));
            require(!rendered.isNull(),"actual PDF page must render");
            QImage paper(rendered.size(),QImage::Format_RGB32);paper.fill(Qt::white);
            QPainter painter(&paper);painter.drawImage(0,0,rendered);painter.end();
            require(QDir().mkpath(directory) && paper.save(QDir(directory).filePath(QStringLiteral("appraisal-report-page-%1.png").arg(page+1))),
                "retain actual paginated appraisal PDF render on white paper");
        }
    }
    return result.simplified();
}
void sheet_summary_preserves_per_row_policy_and_status() {
    QTemporaryDir directory;require(directory.isValid(),"sheet renderer needs local PDF directory");
    sketch::ScheduleRow ansi_row;ansi_row.kind=sketch::ScheduleRowKind::appraisal;
    ansi_row.cells["label"].value=std::string("ANSI primary dwelling GLA");
    ansi_row.cells["area"].value=sketch::ScheduleQuantity{100.0,sketch::ScheduleUnit::square_metre};
    ansi_row.cells["area"].display_decimal_places=3;
    ansi_row.cells["policy_kind"].value=std::string("ansi_z765_2021");
    ansi_row.cells["profile_id"].value=sketch::ansi_appraisal_profile().id;
    auto legacy=ansi_row;legacy.cells.erase("policy_kind");legacy.cells.erase("profile_id");
    legacy.cells["label"].value=std::string("Legacy category");legacy.cells["area"].display_decimal_places=1;
    auto malformed=legacy;malformed.cells["label"].value=std::string("Malformed policy diagnostic");
    malformed.cells["policy_kind"].value=true;
    auto status=ansi_row;status.cells.erase("area");status.cells["label"].value=std::string("ANSI rule status");
    status.cells["status"].value=std::string("Unqualified: totals withheld. Vertex rule checks are not ANSI approval / certification.");
    const std::vector<const sketch::ScheduleRow*> rows{&ansi_row,&legacy,&malformed,&status};
    auto render=[&](bool metric,bool overflow) {
        const auto path=directory.filePath(QStringLiteral("sheet-%1-%2.pdf").arg(metric).arg(overflow));
        {
            QPdfWriter writer(path);writer.setResolution(144);
            QPainter painter(&writer);require(painter.isActive(),"sheet summary PDF painter must start");
            sketch::desktop::render_appraisal_summary_schedule(painter,
                QRectF(0,0,writer.width(),overflow?110.0:writer.height()),144.0/25.4,rows,metric);
            require(painter.end(),"sheet summary PDF painter must finish");
        }
        return pdf_text(path);
    };
    for(const bool metric:{true,false}) {
        const auto content=render(metric,false);
        require(content.contains(QStringLiteral("ANSI primary dwelling GLA 1076 ft²")),
            "ANSI sheet row must use canonical whole square feet in either workspace unit mode");
        require(content.contains(metric?QStringLiteral("Legacy category 100.0 m²"):QStringLiteral("Legacy category 1076.4 ft²")),
            "mixed legacy sheet row must retain its own workspace units and precision");
        require(content.contains(metric?QStringLiteral("Malformed policy diagnostic 100.0 m²"):QStringLiteral("Malformed policy diagnostic 1076.4 ft²")),
            "nontext policy cells must not be interpreted as ANSI");
        require(content.contains("Unqualified: totals withheld") && content.contains("not ANSI approval / certification"),
            "sheet status must preserve withheld totals and avoid approval implication");
        require(content.contains("supplemental: 100.00 m²")==metric,
            "metric ANSI sheet row must label its metric quantity as supplemental");
        const auto overflow=render(metric,true);
        require(overflow.contains("more rows") && overflow.contains("complete Appraisal area report"),
            "bounded sheet overflow must direct readers to the complete report");
    }
}
void set_precision(MainWindow& window,unsigned precision) {
    const auto source=window.document().snapshot();auto property=source.entities().at("property-1");
    property.properties["calculation_profile"]["decimal_places"]=precision;
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(property)}, {},"report display precision fixture"});
}
void ansi_report_html_pdf_canonical_units_and_evidence() {
    using nlohmann::json;using sketch::Entity;
    const auto make=[](const char* id,const char* type,json properties){return Entity{id,type,std::move(properties),false,json::object()};};
    json boundary=json::array();for(const auto& edge:square(0,0,3.048))boundary.push_back(
        {{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    std::vector<Entity> entities={make("p","property",{{"name","ANSI Home"},{"calculation_workflow","appraisal"},
        {"appraisal_policy",{{"policy_kind","ansi_z765_2021"},{"version",1},{"property_kind","detached_single_family"},
            {"measurement_basis","exterior"},{"ansi",{{"interior_inspected",true},{"direct_measurement",true},
                {"acquisition_increment","inch"},{"limitations_statement","All rooms inspected <verified>."}}}}}}),
        make("b","building",{{"name","Main"},{"property_id","p"}}),
        make("f","floor",{{"name","Ground"},{"building_id","b"},{"appraisal_facts",{{"grade","above"},{"ansi",{{"any_part_below_grade",false}}}}}}),
        make("l","layer",{{"floor_id","f"}}),
        make("a","measurement_boundary",{{"name","Primary room"},{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
            {"calculation_scope","building"},{"boundary",boundary},{"appraisal_facts",{{"finish","finished"},{"access","direct_interior"},
                {"area_use","dwelling"},{"boundary_role","measured_area"},{"ansi",{{"year_round_suitable",true},{"finish_matches_dwelling",true},
                    {"dwelling_identity","primary"},{"ceiling",{{"kind","flat"},{"minimum_height_m",2.4384}}}}}}}})};
    entities.front().properties["appraisal_reporting"]=sketch::appraisal_reporting_json(
        sketch::AppraisalReportingSettings{sketch::AppraisalReportingContract::uad_3_6,true});
    sketch::AppraisalAreaReportingFacts room_facts;
    room_facts.source_geometry_sha256=sketch::appraisal_reporting_source_digest(sketch::Document::create(entities).snapshot(),"a");
    room_facts.rooms={{"bedroom-explicit",sketch::AppraisalRoomUse::bedroom,true}};
    entities.back().properties["appraisal_reporting"]=sketch::appraisal_reporting_json(room_facts);
    auto document=sketch::Document::create(entities);const auto source=document.snapshot();
    const auto report=sketch::build_appraisal_document_report(source,"p",sketch::AreaUnit::square_metre);
    require(report.qualified,"ANSI report fixture qualifies under declared rule checks");
    const auto html=sketch::desktop::appraisal_report_html(source,report,true,true);
    require(html.contains("UAD 3.6 reporting") && html.contains("Primary all-grade room counts") &&
        html.contains("1 bedrooms") && html.contains("bedroom-explicit") && html.contains("September 2026"),
        "Actual HTML includes versioned fields, explicit room membership and guidance editions");
    sketch::desktop::AppraisalReportingDialog editor(source,report);
    auto& contract=child<QComboBox>(editor,"appraisalReportingContract");
    auto& rooms=child<QTableWidget>(editor,"appraisalReportingRooms");
    require(rooms.rowCount()==1 && rooms.item(0,0)->text()=="bedroom-explicit",
        "Reporting editor exposes persisted typed room declarations");
    contract.setCurrentIndex(contract.findData(static_cast<int>(sketch::AppraisalReportingContract::legacy_uad_2_6)));
    bool applied=false;
    editor.setApplyRequested([&](const sketch::AppraisalReportingChanges& changes,QString&){
        sketch::validate_appraisal_reporting_changes(source,changes);
        applied=changes.settings.contract==sketch::AppraisalReportingContract::legacy_uad_2_6 && changes.areas.size()==1;
        return true;
    });
    child<QPushButton>(editor,"appraisalReportingSave").click();
    require(applied && editor.result()==QDialog::Accepted,"Actual Save returns typed source-fenced reporting changes");
    sketch::desktop::AppraisalReportingDialog rejected(source,report);
    rejected.setApplyRequested([](const sketch::AppraisalReportingChanges&,QString& error){error="Source changed; reopen reporting";return false;});
    child<QPushButton>(rejected,"appraisalReportingSave").click();
    require(rejected.result()!=QDialog::Accepted && child<QLabel>(rejected,"appraisalReportingError").text().contains("Source changed"),
        "Failed source-fenced transaction keeps the reporting editor and actionable error");
    for (const bool malformed : {false, true}) {
        auto scoped_entities = entities;
        auto hidden = entities.back(); hidden.id = "hidden-room-area";
        hidden.properties["boundary"] = json::array();
        for (const auto& edge : square(10,0,3.048)) hidden.properties["boundary"].push_back(
            {{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
        if (malformed) hidden.properties["appraisal_reporting"] = {{"version",1},{"rooms","invalid hidden data"}};
        scoped_entities.push_back(std::move(hidden));
        const auto scoped_source = sketch::Document::create(scoped_entities).snapshot();
        std::set<std::string,std::less<>> visible;
        for (const auto& [id, item] : scoped_source.entities()) if (id != "hidden-room-area") visible.insert(id);
        const auto scoped_report = sketch::build_appraisal_document_report(scoped_source,"p",sketch::AreaUnit::square_foot,&visible);
        require(scoped_report.qualified && scoped_report.boundaries.size()==1,
            "Reporting fixture independently excludes the other design-phase area");
        sketch::desktop::AppraisalReportingDialog scoped_editor(scoped_source,scoped_report,nullptr,&visible);
        require(child<QComboBox>(scoped_editor,"appraisalReportingArea").count()==1,
            "Reporting editor lists only current semantic-phase areas");
        bool scoped_applied=false;
        scoped_editor.setApplyRequested([&](const sketch::AppraisalReportingChanges& changes,QString&){
            sketch::validate_appraisal_reporting_changes(scoped_source,changes,&visible);
            scoped_applied=changes.areas.size()==1 && changes.areas.front().first=="a";
            return true;
        });
        child<QPushButton>(scoped_editor,"appraisalReportingSave").click();
        require(scoped_applied && scoped_editor.result()==QDialog::Accepted,
            "Hidden duplicate room membership and hidden malformed declarations cannot block a visible editor save");
    }
    require(html.contains("MEASUREMENT SUMMARY") &&
        html.contains("Gross boundary area</td><td align='right'>100 sq ft") &&
        !html.contains("Gross boundary area</td><td align='right'>100.00 sq ft") &&
        html.contains("Rectangle components") && html.contains("100.00 sq ft") &&
        html.contains("40.0 ft") && html.contains("10.0 ft") && html.contains("Supplemental metric diagnostic"),
        "ANSI HTML uses canonical areas and tenth-foot dimensions with clearly supplemental metric display");
    require(html.contains("Interior inspected") && html.contains("Direct measurement") && html.contains("Inch") &&
        html.contains("Year-round suitable") && html.contains("Finish matches dwelling") && html.contains("Primary") &&
        html.contains("Recorded minimum ceiling height") && html.contains("8 ft") &&
        html.contains("Rounded minimum ceiling height") && html.contains("8 ft 0 in (nearest inch)") &&
        html.contains("normative",Qt::CaseInsensitive) && html.contains("&lt;verified&gt;"),
        "ANSI HTML exposes source declarations and truthful escaped validation limitations");
    QTemporaryDir directory;require(directory.isValid(),"ANSI PDF fixture directory");QString error;
    const auto path=directory.filePath("ansi-measurement.pdf");
    require(sketch::desktop::write_appraisal_report_pdf(source,report,true,path,error),"ANSI PDF exports authoritative report");
    const auto contents=pdf_text(path);
    require(contents.contains("UAD 3.6 reporting") && contents.contains("bedroom-explicit"),
        "Reopened PDF retains the same form projection and declared room evidence");
    require(contents.contains("MEASUREMENT SUMMARY") && contents.contains("100 sq ft") && contents.contains("40.0 ft") &&
        contents.contains("Interior inspected") && contents.contains("Direct measurement") && contents.contains("All rooms inspected <verified>.") &&
        contents.contains("normative",Qt::CaseInsensitive),"actual reopened PDF retains canonical measurements and declarations");
}
void actual_report_summary_audit_navigation_and_refresh() {
    QTemporaryDir directory;require(directory.isValid(),"report fixture needs isolated local directory");
    MainWindow window({},nullptr,directory.filePath("text-library.json"));const auto ids=fixture(window);
    const auto source=window.document().snapshot();const auto report=sketch::build_appraisal_document_report(source,"property-1",sketch::AreaUnit::square_metre);
    require(report.qualified && report.calculation && std::abs(report.calculation->property.gla().total.square_metres-80)<1e-8 &&
        std::abs(report.calculation->property.by_category.at(sketch::AppraisalAreaCategory::garage).total.square_metres-16)<1e-8 &&
        report.calculation->calculation.areas.size()==2,"report must show parent80 and garage16 without standalone void contribution");
    report_action(window,[&](auto& dialog) {
        const auto summary=child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText();
        require(child<QLabel>(dialog,"appraisalReportState").text().contains("Qualified") && summary.contains("80.00 m²") &&
            summary.contains("16.00 m²") && summary.contains("96.00 m²") && !summary.contains("100.00 m²"),
            "actual summary must separate finished/garage quantities and exclude void from all-category total");
        require(summary.contains(QString::fromStdString(source.document_id())) && summary.contains("property-1"),"summary must expose immutable source provenance");
        for(const auto& phrase:QStringList{"Property policy","Residential declared","Detached single family","Exterior"})
            require(summary.contains(phrase),"summary must disclose policy qualification inputs");
        capture(dialog,"appraisal-report-summary.png");
        auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");auto& parent=row(tree,ids.parent);tree.setCurrentItem(&parent);
        require(tree.topLevelItemCount()==3 && parent.text(2)=="100.00 m²" && parent.text(3)=="20.00 m²" && parent.text(4)=="80.00 m²" &&
            parent.text(5)=="1/1" && parent.text(6)=="80.00 m²" && parent.childCount()==2,"actual audit row must expose gross, union deduction, factor and physical net");
        auto& excluded=row(tree,ids.void_id);require(excluded.text(1)=="Deduction only" && excluded.text(4)=="4.00 m²","void's trace must remain inspectable and explicitly excluded");
        const auto detail=child<QTextBrowser>(dialog,"appraisalReportDetail").toPlainText();
        require(detail.contains("Floor grade") && detail.contains("floor-1 / appraisal_facts") && detail.contains("Above"),
            "boundary audit must disclose floor grade and its source");
        for(const auto& phrase:QStringList{"Gross boundary area","Applied deductions","Physical net","Exact factor","Adjusted area","Display rounding change","Boundary perimeter",ids.parent,ids.garage,ids.void_id})
            require(detail.contains(phrase),"actual boundary detail must retain calculation audit and all deduction source IDs");
        require(parent.child(0)->text(2)==parent.child(0)->text(3) && parent.child(1)->text(2)==parent.child(1)->text(3),"nonoverlapping deductions requested and applied values must agree");
        child<QTabWidget>(dialog,"appraisalReportTabs").setCurrentIndex(1);capture(dialog,"appraisal-report-areas.png");
    });
    require(window.document().revision()==source.revision() && window.document().snapshot().entities()==source.entities(),"opening/reporting must not change document history or entities");
    require(window.setContainerVisible("floor-1",false),"hide actual floor presentation");
    report_action(window,[&](auto& dialog){require(child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText().contains("96.00 m²") &&
        row(child<QTreeWidget>(dialog,"appraisalReportAreas"),ids.parent).text(4)=="80.00 m²","view presentation hiding must not change semantic appraisal quantities");});
    require(window.setContainerVisible("floor-1",true),"restore actual floor presentation");
    const auto pdf=directory.filePath("retained.pdf");require(window.exportAppraisalReportPdf(pdf),"qualified audit must export");const auto retained=bytes(pdf);
    report_action(window,[&](auto& dialog) {
        const auto prior=window.document().snapshot();auto parent=prior.entities().at(ids.parent.toStdString());parent.properties["name"]="Refreshed source name";
        window.document().apply(sketch::ApplyEntityChanges{prior.revision(),{sketch::EntityChange::upsert(parent)}, {},"report refresh fixture"});
        const auto edited=window.document().snapshot();
        child<QPushButton>(dialog,"locateAppraisalBoundary").click();
        require(dialog.isVisible() && child<QLabel>(dialog,"appraisalReportError").text().contains("Refresh"),"stale source navigation must refuse without closing dialog");
        child<QPushButton>(dialog,"exportAppraisalReportPdf").click();
        require(dialog.isVisible() && child<QLabel>(dialog,"appraisalReportError").isVisible() && bytes(pdf)==retained,
            "stale UI export must refuse before destination chooser and preserve existing output");
        require(!window.exportAppraisalReportPdf(pdf,"property-1",prior.revision()) && bytes(pdf)==retained,"revision-fenced PDF export must preserve previous bytes");
        child<QPushButton>(dialog,"refreshAppraisalReport").click();
        require(!child<QLabel>(dialog,"appraisalReportError").isVisible() && child<QLabel>(dialog,"appraisalReportState").text().contains(QString::number(edited.revision())) &&
            row(child<QTreeWidget>(dialog,"appraisalReportAreas"),ids.parent).text(0)=="Refreshed source name","actual Refresh must replace stale projection from current revision");
        auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");tree.setCurrentItem(&row(tree,ids.garage));child<QPushButton>(dialog,"locateAppraisalBoundary").click();
        require(dialog.result()==QDialog::Accepted && window.selectedEntityId()==ids.garage && window.document().revision()==edited.revision() &&
            window.document().snapshot().entities()==edited.entities(),"current source navigation must select exact boundary without a model command");
    });
    const auto final=window.document().snapshot();window.document().mark_read_only("report read-only fixture");
    require(window.exportAppraisalReportPdf(directory.filePath("read-only.pdf"),"property-1",final.revision()) &&
        window.document().revision()==final.revision() && window.document().snapshot().entities()==final.entities(),"read-only report export must remain available and leave history/entities intact");
}

void display_precision_unqualified_diagnostics_and_invalid_trace() {
    QTemporaryDir directory;MainWindow window({},nullptr,directory.filePath("text-library.json"));const auto ids=fixture(window);
    require(window.selectEntity(ids.parent) && window.editSelectedFactor("0.5"),"report fixture must create nonunity adjustment");set_precision(window,1);
    report_action(window,[&](auto& dialog) {
        const auto summary=child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText();
        auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");tree.setCurrentItem(&row(tree,ids.parent));const auto& parent=row(tree,ids.parent);
        require(summary.contains("totals withheld") && !summary.contains("All measured categories total") && parent.text(1)=="Unqualified" &&
            parent.text(4)=="80.0 m²" && parent.text(5)=="1/2" && parent.text(6)=="40.0 m²", "unqualified report must expose physical/adjusted diagnostics without property totals");
        require(child<QTextBrowser>(dialog,"appraisalReportDetail").toPlainText().contains("factor_not_unity") &&
            child<QTextBrowser>(dialog,"appraisalReportIssues").toPlainText().contains("factor exactly equal to one"),"nonunity qualification reason must persist alongside numeric diagnostics");
    });
    const auto diagnostic_path=directory.filePath("diagnostic.pdf");require(window.exportAppraisalReportPdf(diagnostic_path),"unqualified diagnostics may be exported with explicit withheld status");
    require(pdf_text(diagnostic_path).contains("totals withheld") && pdf_text(diagnostic_path).contains("factor_not_unity"),"diagnostic PDF must retain qualification reason");
    window.setMetricUnits(false);
    report_action(window,[&](auto& dialog){const auto& parent=row(child<QTreeWidget>(dialog,"appraisalReportAreas"),ids.parent);
        require(parent.text(4)=="861.1 sq ft" && parent.text(6)=="430.6 sq ft" &&
            child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText().contains("Display precision 1"),"actual report must use active units and configured precision on physical/adjusted amounts");});
    const auto source=window.document().snapshot();auto parent=source.entities().at(ids.parent.toStdString());parent.properties["wall_measurement_source"]={{"version",99}};
    bool refused=false;
    try { window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(parent)}, {},"invalid measurement source fixture"}); }
    catch(const sketch::DocumentError&) { refused=true; }
    require(refused && window.document().snapshot().entities()==source.entities(),"ordinary editing cannot inject an unsupported exterior source");
    std::vector<sketch::Entity> imported;
    for(const auto& [id,entity]:source.entities())imported.push_back(id==parent.id?parent:entity);
    auto diagnostic_document=std::make_shared<sketch::Document>(sketch::Document::create(imported));
    MainWindow diagnostic_window(diagnostic_document,nullptr,directory.filePath("diagnostic-library.json"));
    report_action(diagnostic_window,[&](auto& dialog){auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");tree.setCurrentItem(&row(tree,ids.parent));
        const auto& invalid=row(tree,ids.parent);for(int column=2;column<7;++column)require(invalid.text(column)==QString::fromUtf8("—"),"stale source must suppress every current numeric audit field");
        const auto detail=child<QTextBrowser>(dialog,"appraisalReportDetail").toPlainText();require(detail.contains("Current measurement unavailable") && !detail.contains("Gross boundary area"),"stale detail must explain missing trace without plausible numeric geometry");});
}

void active_design_phase_report_scope() {
    QTemporaryDir directory;MainWindow window({},nullptr,directory.filePath("text-library.json"));const auto ids=fixture(window);
    const auto extra=window.createBoundary(square(20,0,2));
    require(!extra.isEmpty() && window.editSelectedAppraisalFacts(declarations()),"phase report needs a separate qualified area");
    const std::vector<std::string> members{ids.parent.toStdString(),ids.garage.toStdString(),ids.void_id.toStdString(),extra.toStdString()};
    const auto phases=sketch::ModelPhases::create(members,members,{{"omit-extra","Without annex",{extra.toStdString()},{}}});
    auto entity=sketch::Entity::create("model_phases",{{"model",phases.to_json()}});
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),{sketch::EntityChange::upsert(entity)}, {},"report phase fixture"});
    require(window.selectRemodelingAlternative("omit-extra"),"actual phase selector must activate semantic alternative");
    report_action(window,[&](auto& dialog) {
        const auto summary=child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText();
        require(summary.contains("Design alternative: Without annex") && summary.contains("96.00 m²") &&
            child<QTreeWidget>(dialog,"appraisalReportAreas").topLevelItemCount()==3,
            "report must identify the saved semantic phase and omit its demolished area from traces and totals");
    });
    require(window.selectRemodelingAlternative({}),"restore real baseline phase");
    report_action(window,[&](auto& dialog) {
        const auto summary=child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText();
        require(summary.contains("Design alternative: Existing baseline") && summary.contains("100.00 m²") &&
            child<QTreeWidget>(dialog,"appraisalReportAreas").topLevelItemCount()==4,
            "baseline report must restore the actual annex contribution");
    });
}

void undeclared_policy_and_malformed_area_remain_inspectable() {
    QTemporaryDir directory;MainWindow window({},nullptr,directory.filePath("text-library.json"));
    const auto ids=fixture(window);
    const auto declared=window.document().snapshot();
    auto property=declared.entities().at("property-1");property.properties.erase("appraisal_policy");
    window.document().apply(sketch::ApplyEntityChanges{declared.revision(),{sketch::EntityChange::upsert(property)}, {},"remove policy declaration"});
    report_action(window,[&](auto& dialog) {
        const auto summary=child<QTextBrowser>(dialog,"appraisalReportSummary").toPlainText();
        auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");auto& parent=row(tree,ids.parent);tree.setCurrentItem(&parent);
        require(summary.contains("Undeclared") && summary.contains("totals withheld") && !summary.contains("All measured categories total"),
            "missing policy must not manufacture qualified summary totals");
        require(tree.topLevelItemCount()==3 && parent.text(1)=="Unqualified" && parent.text(2)=="100.00 m²" && parent.text(4)=="80.00 m²",
            "missing policy must retain every source and valid gross/deduction diagnostic");
        require(child<QTextBrowser>(dialog,"appraisalReportDetail").toPlainText().contains("undeclared_policy"),
            "individual diagnostic must disclose its missing policy reason");
    });
    const auto diagnostic=directory.filePath("undeclared.pdf");require(window.exportAppraisalReportPdf(diagnostic),"undeclared policy diagnostic may export");
    require(pdf_text(diagnostic).contains("undeclared_policy") && !pdf_text(diagnostic).contains("All measured categories total"),
        "export must retain missing-policy provenance and withheld totals");
    auto source=window.document().snapshot();property=declared.entities().at("property-1");
    auto malformed=source.entities().at(ids.parent.toStdString());malformed.properties["appraisal_facts"]["finish"]=42;
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(property),sketch::EntityChange::upsert(malformed)}, {},"invalid appraisal fact"});
    report_action(window,[&](auto& dialog) {
        auto& tree=child<QTreeWidget>(dialog,"appraisalReportAreas");auto& parent=row(tree,ids.parent);tree.setCurrentItem(&parent);
        require(parent.text(1)=="Unqualified","malformed facts must remain as an unqualified source row");
        for(int column=2;column<7;++column)require(parent.text(column)==QString::fromUtf8("—"),"invalid area must suppress all numeric audit fields");
        const auto detail=child<QTextBrowser>(dialog,"appraisalReportDetail").toPlainText();
        require(detail.contains("Current measurement unavailable") && detail.contains("invalid_input") && !detail.contains("Gross boundary area"),
            "invalid source detail must retain actionable reason without a numeric trace");
        child<QTabWidget>(dialog,"appraisalReportTabs").setCurrentIndex(1);capture(dialog,"appraisal-invalid-source.png");
        child<QPushButton>(dialog,"locateAppraisalBoundary").click();
        require(window.selectedEntityId()==ids.parent,"invalid report row must navigate to its actual source for repair");
    });
    require(window.editSelectedAppraisalFacts(declarations()),"invalid source can be repaired through actual authoring API");
    report_action(window,[&](auto& dialog) {
        require(child<QLabel>(dialog,"appraisalReportState").text().contains("Qualified") &&
            row(child<QTreeWidget>(dialog,"appraisalReportAreas"),ids.parent).text(4)=="80.00 m²",
            "repair must restore qualification and fresh diagnostic geometry");
    });
}

void unfinished_measured_lines_refuse_pdf_without_changing_destination_or_drawing() {
    QTemporaryDir directory;require(directory.isValid(),"unfinished drawing needs local PDF directory");
    MainWindow window({},nullptr,directory.filePath("text-library.json"));(void)fixture(window);
    auto* canvas=dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"unfinished drawing needs actual measurement canvas");
    const auto path=directory.filePath("retained-drawing-report.pdf");
    require(window.exportAppraisalReportPdf(path),"finished appraisal source must initially export");
    const auto retained=bytes(path);
    const auto unchanged=[&](const sketch::DocumentSnapshot& before) {
        const auto after=window.document().snapshot();
        return after.document_id()==before.document_id() && after.revision()==before.revision() &&
            after.entities()==before.entities() && after.assets()==before.assets() &&
            after.history().size()==before.history().size() &&
            after.saved_revision_optional()==before.saved_revision_optional();
    };

    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({20,20}),
        "anchor-only export fixture must start real measured-line drawing");
    const auto anchored=window.document().snapshot();
    require(canvas->boundaryDraftPreview() && canvas->boundaryDraftPreview()->anchor &&
        canvas->boundaryDraftPreview()->segments.empty(),"anchor-only fixture must have no committed side");
    require(!window.exportAppraisalReportPdf(path,{},anchored.revision()),
        "anchor-only Measured lines must refuse appraisal PDF export");
    require(window.lastError().contains(QStringLiteral("Finish or cancel")),
        "refused anchor-only PDF must explain how to end the unfinished drawing");
    require(bytes(path)==retained && unchanged(anchored) && canvas->boundaryDraftPreview() &&
        canvas->boundaryDraftPreview()->anchor &&
        canvas->boundaryDraftPreview()->anchor->x==20 && canvas->boundaryDraftPreview()->anchor->y==20 &&
        canvas->boundaryDraftPreview()->segments.empty(),
        "refused anchor-only PDF must preserve destination bytes, drawing anchor and document history");
    QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(canvas,&cancel);
    require(!canvas->boundaryDraftPreview() && unchanged(anchored) && window.exportAppraisalReportPdf(path),
        "explicit Cancel must end anchor-only drawing and allow PDF export without document edits");

    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({20,20}) &&
        window.appendMeasurementLineworkHeading(QStringLiteral("2 m"),QStringLiteral("0 deg")),
        "active-stroke export fixture must accept a real measured side");
    const auto accepted=window.document().snapshot();const auto accepted_bytes=bytes(path);
    const auto accepted_stroke=std::find_if(accepted.entities().begin(),accepted.entities().end(),
        [](const auto& item){return item.second.type=="measurement_linework";});
    require(accepted_stroke!=accepted.entities().end(),"active-stroke fixture must retain its committed measured entity");
    const auto accepted_model=sketch::decode_measurement_linework_model(accepted_stroke->second.properties.at("model"));
    require(accepted_model.supported() && accepted_model.model->edges.size()==1 &&
        canvas->boundaryDraftPreview() &&
        canvas->boundaryDraftPreview()->pen_position,"active stroke must retain its accepted side and next pen");
    require(!window.exportAppraisalReportPdf(path,{},accepted.revision()),
        "accepted-side Measured lines must refuse appraisal PDF export while stroke remains active");
    require(window.lastError().contains(QStringLiteral("Finish or cancel")),
        "refused active-stroke PDF must explain how to end the unfinished drawing");
    require(bytes(path)==accepted_bytes && unchanged(accepted) && canvas->boundaryDraftPreview() &&
        canvas->boundaryDraftPreview()->pen_position &&
        canvas->boundaryDraftPreview()->pen_position->x==22 && canvas->boundaryDraftPreview()->pen_position->y==20,
        "refused active-stroke PDF must preserve destination bytes, accepted geometry, pen and history");
    require(window.appendMeasurementLineworkHeading(QStringLiteral("1 m"),QStringLiteral("90 deg")),
        "refused PDF must leave the accepted stroke able to append its next receipt");
    const auto continued=window.document().snapshot();
    require(continued.history().size()==accepted.history().size()+1,"only the subsequent authored side may add history");
    const auto stroke=std::find_if(continued.entities().begin(),continued.entities().end(),
        [](const auto& item){return item.second.type=="measurement_linework";});
    require(stroke!=continued.entities().end(),"accepted drawing must retain its measured-stroke entity");
    const auto decoded=sketch::decode_measurement_linework_model(stroke->second.properties.at("model"));
    require(decoded.supported() && decoded.model->edges.size()==2 &&
        decoded.model->edges.back().receipt.start.x==22 && decoded.model->edges.back().receipt.start.y==20,
        "next accepted receipt must continue the exact pen retained through refused output");
    window.finishMeasurementLinework();
    require(!canvas->boundaryDraftPreview() && unchanged(continued) && window.exportAppraisalReportPdf(path),
        "explicit Finish must retain accepted sides/history and allow PDF export again");
}

void paginated_pdf_escaping_and_atomic_destination_failures() {
    QTemporaryDir directory;require(directory.isValid(),"pagination fixture needs local directory");MainWindow window({},nullptr,directory.filePath("text-library.json"));const auto ids=fixture(window);
    std::vector<QString> source_ids{ids.parent,ids.garage,ids.void_id};
    for(int i=0;i<17;++i) {
        const auto id=window.createBoundary(square(20+i*12,0,10));require(!id.isEmpty() && window.editSelectedAppraisalFacts(declarations()),"pagination must create real qualified nonoverlapping areas");source_ids.push_back(id);
    }
    const auto source=window.document().snapshot();auto last=source.entities().at(source_ids.back().toStdString());
    const QString long_name=QStringLiteral("Audit <script>foreign-marker</script> & <img src='file:///not-a-real-image'> ")+QString(420,QLatin1Char('W'));
    last.properties["name"]=long_name.toStdString();window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(last)}, {},"escaped report name fixture"});
    const auto before=window.document().snapshot();const auto report=sketch::build_appraisal_document_report(before,"property-1",sketch::AreaUnit::square_metre);
    const auto html=sketch::desktop::appraisal_report_html(before,report,true,true);
    require(html.contains(long_name.toHtmlEscaped()) && !html.contains("<script>") && !html.contains("<img src="),
        "source names must be escaped as text without injecting foreign HTML elements");
    QTextDocument rendered;rendered.setHtml(html);require(rendered.toPlainText().contains(long_name),"long escaped source name must survive document layout as literal text");
    const auto path=directory.filePath("complete-report.pdf");require(window.exportAppraisalReportPdf(path,"property-1",before.revision()),"complete audit must export to real paginated PDF");
    QPdfDocument pdf;require(pdf.load(path)==QPdfDocument::Error::None && pdf.pageCount()>1,"twenty boundary audit must paginate instead of clipping after page one");
    const auto text=pdf_text(path,true);for(const auto& id:source_ids)require(text.contains(id),"every source ID including final area must survive exported pagination");
    require(text.contains("foreign-marker") && text.contains("All measured categories total") && text.contains("1796.00 m²"),"complete PDF must retain escaped names and qualified property total");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!captures.isEmpty()) {
        require(QDir().mkpath(captures),"create actual report capture directory");
        QFile retained_pdf(QDir(captures).filePath("appraisal-report-complete.pdf"));const auto actual=bytes(path);
        require(retained_pdf.open(QIODevice::WriteOnly) && retained_pdf.write(actual)==actual.size(),"retain actual full audit PDF");
    }
    const auto retained=bytes(path);QString error;
    auto stale=report;stale.revision=before.revision()+1;
    require(!sketch::desktop::write_appraisal_report_pdf(before,stale,true,path,error) && !error.isEmpty() && bytes(path)==retained,"stale immutable report must refuse before touching existing output bytes");
    auto foreign=report;foreign.source_document_id="another-document";
    require(!sketch::desktop::write_appraisal_report_pdf(before,foreign,true,path,error) && bytes(path)==retained,
        "matching revision and property IDs must not allow a report from another document");
    auto changed_state=report;changed_state.source_entities_sha256=std::string(64,'0');
    require(!sketch::desktop::write_appraisal_report_pdf(before,changed_state,true,path,error) && bytes(path)==retained,
        "matching document identity and revision must still require the report's entity-state digest");
    require(!window.exportAppraisalReportPdf({}) && !window.exportAppraisalReportPdf(directory.filePath("missing-parent/report.pdf")),
        "empty and nonexistent parent destinations must fail visibly");
    const auto directory_destination=directory.filePath("directory.pdf");require(QDir().mkdir(directory_destination) &&
        !window.exportAppraisalReportPdf(directory_destination) && QDir(directory_destination).exists(),"atomic output replacement must refuse a directory and preserve it");
    require(bytes(path)==retained && window.document().revision()==before.revision() && window.document().snapshot().entities()==before.entities(),"failed/report exports must leave previous output and exact project history intact");
    const auto project=directory.filePath("project.pdf");require(window.saveProjectAs(project),"collision fixture stores real native project at misleading PDF suffix");
    const auto project_bytes=bytes(project);const auto saved=window.document().snapshot();
    require(!window.exportAppraisalReportPdf(project) && bytes(project)==project_bytes && window.document().snapshot().entities()==saved.entities(),"PDF output must never overwrite current native project despite matching suffix");
}
}

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-appraisal-report-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled font must load for actual report rendering");app.setFont(QFont(QStringLiteral("Inter"),10));
        sheet_summary_preserves_per_row_policy_and_status();ansi_report_html_pdf_canonical_units_and_evidence();actual_report_summary_audit_navigation_and_refresh();display_precision_unqualified_diagnostics_and_invalid_trace();
        active_design_phase_report_scope();undeclared_policy_and_malformed_area_remain_inspectable();
        unfinished_measured_lines_refuse_pdf_without_changing_destination_or_drawing();
        paginated_pdf_escaping_and_atomic_destination_failures();
        std::cout<<"appraisal_report_desktop_tests passed\n";return 0;
    } catch(const std::exception& failure){std::cerr<<"appraisal_report_desktop_tests: "<<failure.what()<<'\n';return 1;}
}
