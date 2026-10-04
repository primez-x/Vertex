#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_area_definition.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QListWidget>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
namespace {
using namespace sketch; using namespace sketch::desktop;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents,50); }
double luminance(QColor color) {
    const auto linear=[](double value){return value<=0.04045?value/12.92:std::pow((value+0.055)/1.055,2.4);};
    return 0.2126*linear(color.redF())+0.7152*linear(color.greenF())+0.0722*linear(color.blueF());
}
Entity stroke(std::string id,double x,double y,double size) {
    const std::vector<Vec2> points{{x,y},{x+size,y},{x+size,y+size},{x,y+size},{x,y}};
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=true;
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt; receipt.segment_id=id+":edge"+std::to_string(i); receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=points[i-1]; receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":vertex"+std::to_string(i-1),i==4?id+":vertex0":id+":vertex"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({{"p","property",{{"name","Nested measured areas"}},false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"},{"name","Measured"}},false},stroke("outer",0,0,10),stroke("inner",2,2,4)}));
}
void prepare(MainWindow& window) { window.setAttribute(Qt::WA_DontShowOnScreen,true); window.resize(1400,900); window.show(); window.setMetricUnits(true); events(); require(window.selectEntity("outer"),"select real outer stroke"); }
using Review=std::function<void(QDialog&,QTableWidget&,QDialogButtonBox&)>;
void review(MainWindow& window,const Review& use) {
    require(window.selectEntity("outer"),"review selects measured source");
    auto* action=window.findChild<QAction*>("detectClosedAreas"); require(action && action->isEnabled(),"native Detect closed areas is available");
    std::exception_ptr failure; bool opened=false;
    QTimer::singleShot(0,[&] {
        auto* dialog=window.findChild<QDialog*>("measuredAreaReviewDialog");
        if(!dialog) {failure=std::make_exception_ptr(std::runtime_error("native measured-area review opens")); if(auto* modal=QApplication::activeModalWidget()) modal->close(); return;}
        opened=true;
        try { auto* rows=dialog->findChild<QTableWidget*>("measuredAreaReviewRows"); auto* buttons=dialog->findChild<QDialogButtonBox*>("measuredAreaReviewButtons"); require(rows && buttons,"native per-area review controls exist"); use(*dialog,*rows,*buttons); }
        catch(...) { failure=std::current_exception(); dialog->reject(); }
    });
    action->trigger(); if(failure) std::rethrow_exception(failure); require(opened,"native review callback ran"); events();
}
int row(const QTableWidget& rows,double gross) {
    for(int i=0;i<rows.rowCount();++i) if(std::abs(rows.item(i,2)->data(Qt::UserRole).toDouble()-gross)<1e-8) return i;
    throw std::runtime_error("review includes exact expected gross outline");
}
void choice(QTableWidget& rows,int index,MeasurementAreaDisposition disposition,const char* classification) {
    auto* use=qobject_cast<QComboBox*>(rows.cellWidget(index,4)); auto* type=qobject_cast<QComboBox*>(rows.cellWidget(index,5));
    require(use && type,"each outline has independent disposition and classification");
    use->setCurrentIndex(use->findData(static_cast<int>(disposition))); type->setEditText(QString::fromLatin1(classification));
}
std::vector<Entity> areas(const DocumentSnapshot& snapshot) { std::vector<Entity> result; for(const auto& [id,entity]:snapshot.entities()) if(entity.type=="measurement_boundary") result.push_back(entity); return result; }
void test_cancel_defaults_and_reference() {
    MainWindow window(fixture()); prepare(window); const auto before=window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls("living").isEmpty() && window.lastError().contains("review",Qt::CaseInsensitive),"uniform programmatic classification refuses nested outlines instead of overlapping totals");
    review(window,[&](QDialog&,QTableWidget& rows,QDialogButtonBox& buttons) {
        require(rows.rowCount()==2,"nested outlines are independently reviewed"); const auto inner=row(rows,16);
        require(qobject_cast<QComboBox*>(rows.cellWidget(inner,4))->currentData().toInt()==static_cast<int>(MeasurementAreaDisposition::reference_only),"nested default is reference only with no guessed deduction");
        buttons.button(QDialogButtonBox::Cancel)->click();
    }); require(window.document().snapshot().entities()==before.entities(),"cancel keeps all measured sources and history unchanged");
    review(window,[&](QDialog&,QTableWidget& rows,QDialogButtonBox& buttons) {
        for(int i=0;i<rows.rowCount();++i) choice(rows,i,MeasurementAreaDisposition::reference_only,"");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"all-reference review remains a valid no-change choice"); buttons.button(QDialogButtonBox::Apply)->click();
    }); require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),"all-reference Apply writes no geometry or history");
    review(window,[&](QDialog&,QTableWidget& rows,QDialogButtonBox& buttons) {
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"living");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"explicit outer definition has a valid preview"); buttons.button(QDialogButtonBox::Apply)->click();
    }); const auto after=window.document().snapshot(); require(areas(after).size()==1 && after.revision()==before.revision()+1,"reference inner creates only the explicit outer hundred-square-metre area");
    require(!areas(after).front().properties.contains("deduction_ids"),"reference loop adds no inferred deduction");
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        const auto outer=row(rows,100); require(!rows.item(outer,3)->text().isEmpty(),"repeat review exposes existing area identity");
        choice(rows,outer,MeasurementAreaDisposition::define_area,"garage");
        if(buttons.button(QDialogButtonBox::Apply)->isEnabled()) buttons.button(QDialogButtonBox::Apply)->click(); else dialog.reject();
    }); require(window.document().snapshot().entities()==after.entities(),"repeat preserves existing classifications and creates no duplicate");
}
void test_explicit_deduction_history_details_and_pdf() {
    MainWindow window(fixture()); prepare(window); const auto before=window.document().snapshot();
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"above_grade_finished");
        choice(rows,row(rows,16),MeasurementAreaDisposition::deduct_from_parent,"garage");
        require(rows.item(row(rows,100),6)->text().contains("84"),"review shows exact parent net eighty-four square metres");
        require(std::abs(rows.item(row(rows,100),7)->data(Qt::UserRole).toDouble()-16)<1e-8,"review displays explicit sixteen-square-metre deduction");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"explicit different classifications permit one atomic deduction preview");
        if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!capture.isEmpty()) {
            QDir().mkpath(capture); require(dialog.grab().save(QDir(capture).filePath("nested-area-review.png")),"native nested review capture saves");
        }
        buttons.button(QDialogButtonBox::Apply)->click();
    }); const auto after=window.document().snapshot(); const auto result=areas(after); require(result.size()==2 && after.revision()==before.revision()+1,"parent child and deduction link commit together once");
    Entity parent,child; for(const auto& entity:result) { if(entity.properties.contains("deduction_ids")) parent=entity; else child=entity; }
    require(!parent.id.empty() && parent.properties.at("deduction_ids")==std::vector<std::string>{child.id},"chosen inner identity is linked to chosen parent");
    require(after.entities().at("outer")==before.entities().at("outer") && after.entities().at("inner")==before.entities().at("inner"),"area review preserves original typed strokes");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.redoCommand() && window.document().snapshot().entities()==after.entities(),"one Undo and Redo preserve exact nested definition");
    require(window.selectEntity(QString::fromStdString(parent.id)),"select nested parent for Details");
    auto* workflow=window.findChild<QComboBox*>("calculationWorkflow"); require(workflow,"Details workflow exists"); workflow->setCurrentIndex(workflow->findData("appraisal"));
    const auto declarations=[](const char* use) { return QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"%1","boundary_role":"measured_area"}})").arg(QString::fromLatin1(use)); };
    require(window.editSelectedAppraisalFacts(declarations("dwelling")) && window.selectEntity(QString::fromStdString(child.id)) && window.editSelectedAppraisalFacts(declarations("garage")),"appraisal eligibility is explicitly declared after geometric definition"); events();
    auto* gla=window.findChild<QLabel*>("appraisalGlaTotal"); require(gla && gla->text().contains("84.00"),"Details shows eighty-four square metres of explicitly qualified living area");
    QTemporaryDir temp; require(window.saveProjectAs(temp.filePath("nested.bldproj")) && window.openProject(temp.filePath("nested.bldproj")),"nested areas and deduction link save and reopen");
    require(window.exportDraftPdf(temp.filePath("nested.pdf")),"actual shared PDF includes nested measured definition"); QPdfDocument pdf;
    require(pdf.load(temp.filePath("nested.pdf"))==QPdfDocument::Error::None && pdf.pageCount()>0 && !pdf.render(0,QSize(1000,800)).isNull(),"actual nested measured PDF renders");
    const auto pdf_text=pdf.getAllText(0).text();
    if(!pdf_text.contains("84.00"))std::cerr<<"PDF text diagnostic:\n"<<pdf_text.toStdString()<<'\n';
    if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !capture.isEmpty()) {
        window.fitView();
        auto* tabs=window.findChild<QTabWidget*>("sidebarTabs");require(tabs,"native left tabs exist");
        for(int i=0;i<tabs->count();++i)if(tabs->tabText(i)=="Details")tabs->setCurrentIndex(i);
        events();
        QDir().mkpath(capture); require(window.grab().save(QDir(capture).filePath("nested-defined-canvas.png")),"native nested canvas capture saves");
        require(pdf.render(0,QSize(1200,900)).save(QDir(capture).filePath("nested-defined-pdf.png")),"rendered nested PDF capture saves");
    }
    require(pdf_text.contains("84.00"),"actual PDF retains the qualified eighty-four-square-metre net measurement");
}
void test_phase_review_and_exact_augmented_apply() {
    auto document=fixture();
    auto phases=ModelPhases::create({"outer","inner"},{"outer"},{{"future","Future",{}, {"inner"}}});
    Entity registry{"phases","model_phases",{{"model",phases.to_json()}}};
    document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert(registry)},{},"Create future alternative"});
    MainWindow window(document);prepare(window);
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox&) {
        require(rows.rowCount()==1,"baseline native review excludes inactive proposed inner outline");dialog.reject();
    });
    phases=phases.with_active("future");registry.properties["model"]=phases.to_json();
    document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert(registry)},{},"Select future"});
    require(window.selectEntity("outer"),"refresh future-phase source selection");
    const auto before=document->snapshot();
    review(window,[&](QDialog&,QTableWidget& rows,QDialogButtonBox& buttons) {
        require(rows.rowCount()==2,"active native review includes proposed inner outline");
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"living");
        choice(rows,row(rows,16),MeasurementAreaDisposition::deduct_from_parent,"garage");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"fully augmented active-phase preview validates");
        buttons.button(QDialogButtonBox::Apply)->click();
    });
    const auto after=document->snapshot();const auto created=areas(after);
    require(created.size()==2 && after.revision()==before.revision()+1,"phase registry and areas commit in one history entry");
    const auto accepted=ModelPhases::from_json(after.entities().at("phases").properties.at("model"));
    for(const auto& area:created)require(accepted.active_state().at(area.id)==ModelPhase::proposed,"accepted preview registers every new area in selected alternative");
    require(window.undoCommand() && document->snapshot().entities()==before.entities() &&
        window.redoCommand() && document->snapshot().entities()==after.entities(),"phase registry and geometry undo/redo together exactly");
}
void test_ansi_nested_partition_detection() {
    const nlohmann::json policy={{"policy_kind","ansi_z765_2021"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"},
        {"ansi",{{"interior_inspected",true},{"direct_measurement",true},{"acquisition_increment","inch"}}}};
    auto document=std::make_shared<Document>(Document::create({
        {"p","property",{{"name","ANSI nested measured partition"},{"calculation_workflow","appraisal"},{"appraisal_policy",policy}},false},
        {"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"},{"appraisal_facts",{{"grade","above"},{"ansi",{{"any_part_below_grade",false}}}}}},false},
        {"l","layer",{{"floor_id","f"},{"name","Measured"}},false},
        stroke("outer",0,0,10),stroke("child",1,1,6),stroke("void",2,2,2)}));
    MainWindow window(document);prepare(window);const auto before=document->snapshot();
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        require(rows.rowCount()==3,"ANSI detection reviews floor room and void separately");
        require(rows.visualRect(rows.model()->index(2,0)).bottom()<rows.viewport()->height(),
            "small nested review shows the complete void row without vertical scrolling");
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"above_grade_finished");
        choice(rows,row(rows,36),MeasurementAreaDisposition::deduct_from_parent,"above_grade_finished");
        choice(rows,row(rows,4),MeasurementAreaDisposition::deduct_from_parent,"role:other_void");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"ANSI nested partition review admits explicit same-class floor room and void chain");
        require(rows.item(row(rows,100),6)->text().contains("64") && rows.item(row(rows,36),6)->text().contains("32"),
            "ANSI review removes child gross footprints once for parent net64 and child net32");
        require(std::abs(rows.item(row(rows,100),7)->data(Qt::UserRole).toDouble()-36)<1e-8 &&
            std::abs(rows.item(row(rows,36),7)->data(Qt::UserRole).toDouble()-4)<1e-8,
            "ANSI review displays immediate gross deductions rather than recursive net deductions");
        if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!capture.isEmpty()) {
            QDir().mkpath(capture);require(dialog.grab().save(QDir(capture).filePath("ansi-nested-area-review.png")),"capture actual three-level ANSI detection review");
        }
        buttons.button(QDialogButtonBox::Apply)->click();
    });
    const auto defined=document->snapshot();const auto created=areas(defined);
    require(created.size()==3 && defined.revision()==before.revision()+1 && defined.history().size()==before.history().size()+1,
        "ANSI nested detection commits three identities and both links as one command");
    Entity outer,child,void_area;
    for(const auto& area:created) {
        const auto gross=std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(area))));
        if(std::abs(gross-100)<1e-8)outer=area;else if(std::abs(gross-36)<1e-8)child=area;else if(std::abs(gross-4)<1e-8)void_area=area;
    }
    require(!outer.id.empty() && !child.id.empty() && !void_area.id.empty() &&
        outer.properties.at("deduction_ids")==std::vector<std::string>{child.id} &&
        child.properties.at("deduction_ids")==std::vector<std::string>{void_area.id},"ANSI detected links retain exact immediate nested identities");
    for(const auto& [id,entity]:before.entities())require(defined.entities().at(id)==entity,"ANSI definition preserves every original source and declared fact");
    require(void_area.properties.at("appraisal_facts").at("boundary_role")=="other_void",
        "explicit void role is retained without fictional finish or access facts");
    require(!build_appraisal_document_report(defined,"p",AreaUnit::square_metre).qualified,
        "above-grade-finished labels do not infer ANSI eligibility declarations");
    const auto sources=measurement_linework_source_checks(defined.entities());
    for(const auto& area:created)require(sources.at(area.id).current,"every nested area retains current measured source lineage");
    require(window.undoCommand() && document->snapshot().entities()==before.entities() &&
        window.redoCommand() && document->snapshot().entities()==defined.entities(),"one Undo and Redo restore exact nested IDs links sources and facts");
    const nlohmann::json facts={{"finish","finished"},{"access","direct_interior"},{"area_use","dwelling"},{"boundary_role","measured_area"},
        {"ansi",{{"year_round_suitable",true},{"finish_matches_dwelling",true},{"dwelling_identity","primary"},
            {"ceiling",{{"kind","flat"},{"minimum_height_m",2.4384}}}}}};
    const auto declaration=QString::fromStdString(nlohmann::json{{"appraisal_policy",policy},
        {"floor_appraisal_facts",before.entities().at("f").properties.at("appraisal_facts")},{"appraisal_facts",facts}}.dump());
    for(const auto& area:{outer,child}) {
        require(window.selectEntity(QString::fromStdString(area.id)) &&
            window.editSelectedAppraisalFacts(declaration),"ANSI counted regions receive explicit observed eligibility facts");
        const auto declared=document->snapshot();
        require(declared.entities().at(outer.id).properties.at("deduction_ids")==std::vector<std::string>{child.id} &&
            declared.entities().at(child.id).properties.at("deduction_ids")==std::vector<std::string>{void_area.id} &&
            declared.entities().at(void_area.id)==defined.entities().at(void_area.id),
            "fact-only ANSI declaration preserves complete floor room void chain and exact exclusion facts");
        const auto current=measurement_linework_source_checks(declared.entities());
        for(const auto& created_area:created)require(current.at(created_area.id).current &&
            declared.entities().at(created_area.id).extensions==defined.entities().at(created_area.id).extensions,
            "fact-only declaration preserves exact current measured source lineage for every nested owner");
    }
    events();const auto qualified=document->snapshot();const auto report=build_appraisal_document_report(qualified,"p",AreaUnit::square_metre);
    require(report.qualified && report.calculation && std::abs(report.calculation->property.gla().total.square_metres-96)<1e-8,
        "ANSI qualified GLA counts disjoint outer64 and child32 exactly once");
    for(const auto& status:report.boundaries)if(status.boundary_id==outer.id || status.boundary_id==child.id)
        require(status.measurement && std::abs(status.measurement->net_square_metres-(status.boundary_id==outer.id?64:32))<1e-8,
            "core ANSI net measurements agree with detection preview");
    auto* details=window.findChild<QLabel*>("appraisalDetailsGla");
    require(details && details->text()=="1033 sq ft","actual ANSI Details reports canonical whole-square-foot GLA");
    const auto edit_void_deduction=[&](bool remove) {
        require(window.selectEntity(QString::fromStdString(child.id)),"select actual ANSI child for native deductions editor");
        auto* edit=window.findChild<QPushButton*>("editDeductions");require(edit && edit->isEnabled(),"native deductions editor is available");
        std::exception_ptr failure;bool opened=false;
        QTimer::singleShot(0,[&] {
            auto* dialog=window.findChild<QDialog*>("calculationDeductionDialog");
            if(!dialog) {failure=std::make_exception_ptr(std::runtime_error("native ANSI deductions dialog opens"));if(auto* modal=QApplication::activeModalWidget())modal->close();return;}
            opened=true;
            try {
                auto* list=dialog->findChild<QListWidget*>("calculationDeductionList");
                auto* source=dialog->findChild<QComboBox*>("calculationDeductionSource");
                auto* buttons=dialog->findChild<QDialogButtonBox*>("calculationDeductionButtons");
                require(list && source && buttons,"actual ANSI deduction staging controls exist");
                if(remove) {
                    require(list->count()==1 && list->item(0)->data(Qt::UserRole).toString()==QString::fromStdString(void_area.id),"child editor shows exact void deduction identity");
                    list->setCurrentRow(0);auto* button=dialog->findChild<QPushButton*>("removeCalculationDeduction");require(button && button->isEnabled(),"native deduction removal is enabled");button->click();
                } else {
                    require(list->count()==0,"removed void leaves no staged child deductions");
                    const auto index=source->findData(QString::fromStdString(void_area.id));require(index>=0,"exact void boundary remains available for restoration");source->setCurrentIndex(index);
                    auto* button=dialog->findChild<QPushButton*>("addCalculationDeduction");require(button && button->isEnabled(),"native deduction addition is enabled");button->click();
                }
                buttons->button(QDialogButtonBox::Apply)->click();
                require(dialog->result()==QDialog::Accepted,"helper-backed native ANSI deduction edit is admitted atomically");
            } catch(...) {failure=std::current_exception();dialog->reject();}
        });
        edit->click();if(failure)std::rethrow_exception(failure);require(opened,"native ANSI deduction callback ran");events();
    };
    edit_void_deduction(true);const auto removed=document->snapshot();
    require(removed.revision()==qualified.revision()+1 && removed.history().size()==qualified.history().size()+1 &&
        (!removed.entities().at(child.id).properties.contains("deduction_ids") || removed.entities().at(child.id).properties.at("deduction_ids").empty()),
        "actual ANSI deduction removal commits one history entry");
    for(const auto& [id,entity]:qualified.entities())if(id!=child.id)require(removed.entities().at(id)==entity,"child deduction removal preserves all other sources owners links and facts");
    require(removed.entities().at(child.id).properties.at("appraisal_facts")==qualified.entities().at(child.id).properties.at("appraisal_facts") &&
        measurement_linework_source_checks(removed.entities()).at(child.id).current,"native deduction edit retains child eligibility and current source lineage");
    require(window.undoCommand() && document->snapshot().entities()==qualified.entities() && window.redoCommand() &&
        document->snapshot().entities()==removed.entities(),"native child deduction removal undoes and redoes as one exact command");
    const auto before_restore=document->snapshot();
    edit_void_deduction(false);const auto restored=document->snapshot();
    require(restored.revision()==before_restore.revision()+1 && restored.history().size()==before_restore.history().size()+1 &&
        restored.entities()==qualified.entities(),"native void restoration commits one command and restores exact qualified graph");
    const auto restored_report=build_appraisal_document_report(restored,"p",AreaUnit::square_metre);
    require(restored_report.qualified && restored_report.calculation && std::abs(restored_report.calculation->property.gla().total.square_metres-96)<1e-8,
        "restored actual deductions dialog chain retains qualified GLA96");
    require(window.undoCommand() && document->snapshot().entities()==removed.entities() && window.redoCommand() &&
        document->snapshot().entities()==restored.entities(),"native void restoration undoes and redoes atomically");
    QTemporaryDir directory;require(directory.isValid() && window.saveProjectAs(directory.filePath("ansi-nested.bldproj")) &&
        window.openProject(directory.filePath("ansi-nested.bldproj")) && window.document().snapshot().entities()==qualified.entities(),
        "ANSI nested sources identities links and actual facts survive save/reopen exactly");
    require(window.exportDraftPdf(directory.filePath("ansi-nested-plan.pdf")),"actual ANSI nested plan PDF exports");QPdfDocument plan_pdf;
    require(plan_pdf.load(directory.filePath("ansi-nested-plan.pdf"))==QPdfDocument::Error::None && plan_pdf.pageCount()>0 &&
        !plan_pdf.render(0,QSize(1000,800)).isNull(),"actual ANSI nested drawing sheet PDF renders");
    require(window.exportAppraisalReportPdf(directory.filePath("ansi-nested.pdf"),"p",window.document().revision()),"actual ANSI nested appraisal report PDF exports");QPdfDocument pdf;
    require(pdf.load(directory.filePath("ansi-nested.pdf"))==QPdfDocument::Error::None && pdf.pageCount()>0 &&
        !pdf.render(0,QSize(1000,800)).isNull(),"actual ANSI nested PDF renders");
    QString text;for(int page=0;page<pdf.pageCount();++page)text+=pdf.getAllText(page).text();
    if(!text.contains("1033"))std::cerr<<"ANSI nested appraisal report PDF text diagnostic:\n"<<text.toStdString()<<'\n';
    require(text.contains("1033") && text.contains("Primary dwelling GLA"),"actual ANSI nested PDF agrees with canonical GLA96 square metres as1033 square feet");
    if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!capture.isEmpty()) {
        require(window.selectEntity(QString::fromStdString(child.id)),"select nested child for actual final Details capture");window.fitView();
        auto* tabs=window.findChild<QTabWidget*>("sidebarTabs");require(tabs,"native sidebar Details tab exists");
        for(int index=0;index<tabs->count();++index)if(tabs->tabText(index)=="Details")tabs->setCurrentIndex(index);
        events();QDir().mkpath(capture);
        require(window.grab().save(QDir(capture).filePath("ansi-nested-defined-details.png")),"capture actual final nested ANSI Details and canvas");
        require(pdf.render(0,QSize(1200,900)).save(QDir(capture).filePath("ansi-nested-defined-pdf.png")),"capture rendered actual nested ANSI PDF");
        window.setWorkspaceTheme(WorkspaceTheme::dark);events();
        review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
            require(rows.rowCount()==3 && rows.visualRect(rows.model()->index(2,0)).bottom()<rows.viewport()->height(),
                "dark nested review shows all three complete rows");
            require(dialog.grab().save(QDir(capture).filePath("ansi-nested-area-review-dark.png")),"capture actual dark nested review");
            buttons.button(QDialogButtonBox::Cancel)->click();
        });
        require(window.selectEntity(QString::fromStdString(child.id)),"restore Details selection after dark review");events();
        require(window.grab().save(QDir(capture).filePath("ansi-nested-defined-details-dark.png")),"capture actual dark nested Details");
        window.setWorkspaceTheme(WorkspaceTheme::light);events();
    }
    // Bind the sloped-room observation to the actual authored room and void,
    // after reopening, when MainWindow owns the newly loaded document.
    const auto v2_source=window.document().snapshot();auto v2_policy=policy;v2_policy["version"]=2;
    auto sloped_facts=facts;
    const auto room_shape=boundary_geometry(decode_identified_boundary_entity(v2_source.entities().at(child.id)));
    const auto void_shape=boundary_geometry(decode_identified_boundary_entity(v2_source.entities().at(void_area.id)));
    sloped_facts["ansi"]["ceiling"]={{"kind","sloped"},{"complete_room_observed",true},
        {"at_least_7ft_area_m2",20},{"room_floor_area_m2",36},{"room_boundary_id",child.id},
        {"below_5ft_deduction_ids",{void_area.id}},
        {"source_geometry_sha256",appraisal_ceiling_geometry_digest(room_shape,{{void_area.id,void_shape}})}};
    const nlohmann::json sloped_declaration={{"appraisal_policy",v2_policy},
        {"floor_appraisal_facts",v2_source.entities().at("f").properties.at("appraisal_facts")},
        {"appraisal_facts",sloped_facts},{"deduction_ids",{void_area.id}}};
    require(window.selectEntity(QString::fromStdString(child.id)) &&
        window.editSelectedAppraisalFacts(QString::fromStdString(sloped_declaration.dump())),
        "native facts author explicit V2 complete sloped-room and bound low-height evidence");
    events();const auto sloped=window.document().snapshot();
    require(sloped.revision()==v2_source.revision()+1 && sloped.history().size()==v2_source.history().size()+1 &&
        sloped.entities().at(outer.id)==v2_source.entities().at(outer.id) && sloped.entities().at(void_area.id)==v2_source.entities().at(void_area.id) &&
        sloped.entities().at(child.id).properties.at("deduction_ids")==std::vector<std::string>{void_area.id} &&
        sloped.entities().at(child.id).properties.at("appraisal_facts")==sloped_facts,
        "V2 observation retains exact flat floor room void graph and actual declarations atomically");
    const auto sloped_sources=measurement_linework_source_checks(sloped.entities());
    for(const auto& area:created)require(sloped_sources.at(area.id).current &&
        sloped.entities().at(area.id).extensions==v2_source.entities().at(area.id).extensions,
        "V2 sloped facts preserve exact current authored source lineage");
    const auto sloped_report=build_appraisal_document_report(sloped,"p",AreaUnit::square_metre);
    require(sloped_report.qualified && sloped_report.calculation && sloped_report.policy && sloped_report.policy->version==2 &&
        std::abs(sloped_report.calculation->property.gla().total.square_metres-96)<1e-8,
        "actual V2 sloped room excludes real low-height leaf once for qualified GLA96");
    require(window.findChild<QLabel*>("appraisalDetailsGla")->text()=="1033 sq ft","V2 sloped room actual Details retains1033 square feet");
    require(window.undoCommand() && window.document().snapshot().entities()==v2_source.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==sloped.entities(),"V2 complete sloped observation undoes and redoes exactly");
    require(window.saveProjectAs(directory.filePath("ansi-nested-v2.bldproj")) && window.openProject(directory.filePath("ansi-nested-v2.bldproj")) &&
        window.document().snapshot().entities()==sloped.entities(),"actual V2 nested observations save and reopen exactly");
    require(window.exportAppraisalReportPdf(directory.filePath("ansi-nested-v2.pdf"),"p",window.document().revision()),"actual V2 nested sloped appraisal report PDF exports");QPdfDocument v2_pdf;
    require(v2_pdf.load(directory.filePath("ansi-nested-v2.pdf"))==QPdfDocument::Error::None && v2_pdf.pageCount()>0 &&
        !v2_pdf.render(0,QSize(1000,800)).isNull(),"actual V2 nested sloped PDF renders");
    QString v2_text;for(int page=0;page<v2_pdf.pageCount();++page)v2_text+=v2_pdf.getAllText(page).text();
    if(!v2_text.contains("1033"))std::cerr<<"ANSI V2 nested appraisal report PDF text diagnostic:\n"<<v2_text.toStdString()<<'\n';
    require(v2_text.contains("1033") && v2_text.contains("Primary dwelling GLA"),"actual V2 nested sloped PDF retains canonical1033 square feet");
    if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!capture.isEmpty()) {
        require(window.selectEntity(QString::fromStdString(child.id)),"select V2 child for final Details capture");events();
        require(window.grab().save(QDir(capture).filePath("ansi-nested-v2-details.png")),"capture actual V2 nested Details");
        require(v2_pdf.render(0,QSize(1200,900)).save(QDir(capture).filePath("ansi-nested-v2-pdf.png")),"capture actual V2 nested PDF");
    }
}
void test_stale_preview_and_same_type_rejection() {
    auto document=fixture(); MainWindow window(document); prepare(window); const auto before=document->snapshot();
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"living"); choice(rows,row(rows,16),MeasurementAreaDisposition::deduct_from_parent,"living");
        require(!buttons.button(QDialogButtonBox::Apply)->isEnabled(),"same-type deduction is rejected by existing explicit subtraction policy"); dialog.reject();
    });
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        choice(rows,row(rows,100),MeasurementAreaDisposition::define_area,"living");
        auto changed=document->snapshot().entities().at("p"); changed.properties["name"]="Changed during review";
        document->apply(ApplyEntityChanges{.expected_revision=document->revision(),.entity_changes={EntityChange::upsert(changed)},.message="external fixture change"});
        buttons.button(QDialogButtonBox::Apply)->click(); require(areas(document->snapshot()).empty(),"stale Apply cannot commit retained preview"); dialog.reject();
    }); require(areas(document->snapshot()).empty() && document->revision()==before.revision()+1,"stale review leaves only unrelated authorized source edit");
}
void test_native_deductions_without_layer_assignment() {
    for(const bool ansi:{true,false}) {
        const auto boundary=[](const char* id,double x,double y,double size,const char* classification) {
            IdentifiedBoundary model{id,"measurement_boundary",{}};
            const Vec2 points[]{{x,y},{x+size,y},{x+size,y+size},{x,y+size}};
            for(std::size_t index=0;index<4;++index)model.segments.push_back({std::string(id)+":edge"+std::to_string(index),
                std::string(id)+":vertex"+std::to_string(index),std::string(id)+":vertex"+std::to_string((index+1)%4),{points[index],points[(index+1)%4],0}});
            auto entity=encode_identified_boundary_entity(model);
            entity.properties.update({{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"classification",classification},
                {"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1},{"calculation_scope","building"}});
            return entity;
        };
        auto outer=boundary("outer-area",0,0,10,"living");auto child=boundary("child-area",1,1,6,ansi?"living":"garage");
        auto leaf=boundary("void-area",2,2,2,"role:other_void");outer.properties["deduction_ids"]={child.id};
        if(ansi) {child.properties["deduction_ids"]={leaf.id};leaf.properties["appraisal_facts"]={{"boundary_role","other_void"}};}
        nlohmann::json policy={{"policy_kind",ansi?"ansi_z765_2021":"residential_declared"},{"version",1},
            {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
        if(ansi)policy["ansi"]={{"interior_inspected",true},{"direct_measurement",true},{"acquisition_increment","inch"}};
        std::vector<Entity> entities{{"p","property",{{"calculation_workflow","appraisal"},{"appraisal_policy",policy}},false},
            {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},outer,child};
        if(ansi)entities.push_back(leaf);
        MainWindow window(std::make_shared<Document>(Document::create(entities)));window.setAttribute(Qt::WA_DontShowOnScreen,true);window.show();events();
        const auto edit_link=[&](bool remove) {
            require(window.selectEntity("outer-area"),"select saved source-free area without layer assignment");
            auto* edit=window.findChild<QPushButton*>("editDeductions");require(edit && edit->isEnabled(),"layerless saved area exposes real deductions editor");
            std::exception_ptr failure;bool opened=false;
            QTimer::singleShot(0,[&] {
                auto* dialog=window.findChild<QDialog*>("calculationDeductionDialog");
                if(!dialog){failure=std::make_exception_ptr(std::runtime_error("layerless saved area deduction dialog opens"));if(auto* modal=QApplication::activeModalWidget())modal->close();return;}
                opened=true;
                try {
                    auto* list=dialog->findChild<QListWidget*>("calculationDeductionList");auto* source=dialog->findChild<QComboBox*>("calculationDeductionSource");
                    auto* buttons=dialog->findChild<QDialogButtonBox*>("calculationDeductionButtons");require(list && source && buttons,"layerless editor has actual staging controls");
                    if(remove){require(list->count()==1,"layerless owner retains existing deduction");list->setCurrentRow(0);dialog->findChild<QPushButton*>("removeCalculationDeduction")->click();}
                    else {const auto index=source->findData(QString::fromStdString(child.id));require(index>=0,"layerless child offered by native deductions editor");source->setCurrentIndex(index);dialog->findChild<QPushButton*>("addCalculationDeduction")->click();}
                    buttons->button(QDialogButtonBox::Apply)->click();require(dialog->result()==QDialog::Accepted,"ANSI and declared policies preserve layerless native deduction editing");
                }catch(...){failure=std::current_exception();dialog->reject();}
            });
            edit->click();if(failure)std::rethrow_exception(failure);require(opened,"layerless deduction callback ran");events();
        };
        const auto before=window.document().snapshot();edit_link(true);const auto removed=window.document().snapshot();
        require(removed.revision()==before.revision()+1,"layerless deduction removal is atomic");
        edit_link(false);const auto restored=window.document().snapshot();
        require(restored.revision()==removed.revision()+1 && restored.entities()==before.entities(),"layerless native restoration keeps exact source-free graph without inventing layers");
        for(const auto& area:areas(restored))require(!area.properties.contains("layer_id") && area.extensions.empty(),"fresh layerless regression owners remain source-free and unassigned");
        require(window.undoCommand() && window.document().snapshot().entities()==removed.entities() && window.redoCommand() &&
            window.document().snapshot().entities()==restored.entities(),"layerless native deduction restoration undo redo retains exact graph");
    }
}
std::shared_ptr<Document> adjacent_fixture(bool four,bool extend_horizontal=true) {
    auto document=fixture();
    auto seam=[](const char* id,Vec2 start,Vec2 end) {
        MeasurementLinework model;model.stroke_id=id;model.anchor=start;
        ConstructionReceipt receipt;receipt.segment_id=std::string(id)+":edge";receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=start;receipt.chord_end=end;
        model.edges.push_back({receipt.segment_id,std::string(id)+":start",std::string(id)+":end",receipt});
        return Entity{id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
    };
    std::vector<EntityChange> changes{EntityChange::erase("inner"),EntityChange::upsert(seam("seam-v",{5,0},{5,10}))};
    // Continue beyond the editable outline so positive coordinate edits retain
    // all four bounded faces. The original endpoint fixture tests topology loss.
    if(four) changes.push_back(EntityChange::upsert(seam("seam-h",{0,5},{extend_horizontal?15.0:10.0,5})));
    document->apply(ApplyEntityChanges{document->revision(),std::move(changes),{},"Adjacent measured regions"});return document;
}
void combine_all(QDialog& dialog,QTableWidget& rows) {
    auto* combine=dialog.findChild<QPushButton*>("combineMeasuredAreaRows");
    auto* separate=dialog.findChild<QPushButton*>("separateMeasuredAreaRows");
    require(combine && separate,"native review exposes visible Combine selected and Separate selected actions");
    for(int i=0;i<rows.rowCount();++i) choice(rows,i,MeasurementAreaDisposition::define_area,"living");
    rows.selectAll();combine->click();
    require(rows.columnCount()>8 && rows.item(0,8)->text().contains("Combined"),"combined membership has a visible group indicator");
    require(rows.item(0,6)->text().contains("100") && rows.item(0,6)->text().contains("Combined"),"combined whole-owner net is explicitly labeled");
    for(int i=1;i<rows.rowCount();++i)require(rows.item(i,6)->text().contains("Included"),"member rows do not repeat the whole group net as individual net");
}
void test_combined_adjacent_regions_review_history_and_sources() {
    for(bool four:{false,true}) {
        MainWindow window(adjacent_fixture(four));prepare(window);const auto before=window.document().snapshot();
        for(auto theme:{WorkspaceTheme::light,WorkspaceTheme::dark}) {
            window.setWorkspaceTheme(theme);
            review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
                require(rows.rowCount()==(four?4:2),"adjacent fixture exposes every bounded measured region");combine_all(dialog,rows);
                auto* type=qobject_cast<QComboBox*>(rows.cellWidget(0,5));type->setEditText("above_grade_finished");
                require(type->currentText()=="Above grade finished" && type->currentData().toString()=="above_grade_finished","known group classifications display readable labels while retaining their data IDs");
                for(int i=1;i<rows.rowCount();++i)require(qobject_cast<QComboBox*>(rows.cellWidget(i,5))->currentText()==type->currentText(),"editing one group classification synchronizes every member");
                events();const auto background=dialog.grab().toImage().pixelColor(dialog.width()/2,4);
                for(const auto* name:{"measuredAreaReviewExplanation","measuredAreaReviewGroupHelp","measuredAreaReviewStatus"}) {
                    auto* label=dialog.findChild<QLabel*>(name);
                    require(label,"review guidance labels exist");
                    const auto style=label->styleSheet();const auto color_at=style.indexOf('#');
                    const QColor foreground(style.mid(color_at,7));
                    const auto fg=luminance(foreground),bg=luminance(background);
                    require(color_at>=0 && foreground.isValid() && (std::max(fg,bg)+0.05)/(std::min(fg,bg)+0.05)>=4.5,"all review guidance has readable contrast against the rendered light/dark dialog background");
                }
                if(const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!capture.isEmpty()) {
                    events();QDir().mkpath(capture);require(dialog.grab().save(QDir(capture).filePath(QStringLiteral("combined-%1-regions-%2.png").arg(four?4:2).arg(theme==WorkspaceTheme::light?"light":"dark"))),"actual combined-region review capture saves");
                }
                buttons.button(QDialogButtonBox::Cancel)->click();
            });require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),"combination and classification remain transient on Cancel");
        }
        review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
            combine_all(dialog,rows);dialog.findChild<QPushButton*>("separateMeasuredAreaRows")->click();
            for(int i=0;i<rows.rowCount();++i)require(rows.item(i,8)->text().contains("Separate"),"Separate selected restores individual review membership");
            dialog.findChild<QPushButton*>("combineMeasuredAreaRows")->click();
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"edge-connected combination has an admitted exact preview");buttons.button(QDialogButtonBox::Apply)->click();
        });
        const auto combined=window.document().snapshot();const auto result=areas(combined);
        require(result.size()==1 && combined.revision()==before.revision()+1,"two/four adjacent regions create one owner in one revision");
        const auto& area=result.front();require(area.extensions.contains("measurement_linework_group") && area.extensions.at("measurement_linework_group").at("members").size()==(four?4:2),"combined owner retains complete member lineage");
        require(std::abs(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(area))))-100)<1e-8 && !area.properties.contains("deduction_ids"),"combined boundary has exact hundred-square-metre gross/net without invented deductions");
        auto checks=measurement_linework_source_checks(combined.entities());require(checks.at(area.id).current,"combined owner validates against every current measured source");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.redoCommand() && window.document().snapshot().entities()==combined.entities(),"combined definition undoes/redoes atomically");
        const auto retained=window.document().snapshot();
        review(window,[&](QDialog&,QTableWidget& rows,QDialogButtonBox& buttons) {
            for(int i=0;i<rows.rowCount();++i)require(!qobject_cast<QComboBox*>(rows.cellWidget(i,4))->isEnabled() && !qobject_cast<QComboBox*>(rows.cellWidget(i,5))->isEnabled() && rows.item(i,8)->text().contains("existing"),"redetection keeps existing whole-group membership and classification readonly");
            buttons.button(QDialogButtonBox::Apply)->click();
        });require(window.document().snapshot().entities()==retained.entities() && window.document().revision()==retained.revision() && window.document().snapshot().history().size()==retained.history().size(),"redetection Apply creates no duplicate group or history");
        require(window.selectEntity(QString::fromStdString(area.id)),"select combined owner for explicit appraisal facts");
        auto* workflow=window.findChild<QComboBox*>("calculationWorkflow");workflow->setCurrentIndex(workflow->findData("appraisal"));
        require(window.editSelectedAppraisalFacts(QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),"combined geometric owner requires explicit appraisal declarations");events();
        auto* gla=window.findChild<QLabel*>("appraisalGlaTotal");require(gla && gla->text().contains("100.00"),"Details calculates combined owner once as hundred-square-metre qualified GLA");
        QTemporaryDir directory;require(window.saveProjectAs(directory.filePath("combined.bldproj")) && window.openProject(directory.filePath("combined.bldproj")),"combined metadata saves/reopens");
        const auto reopened=window.document().snapshot();require(areas(reopened).size()==1 && measurement_linework_source_checks(reopened.entities()).at(area.id).current,"reopened combined owner remains current without duplicates");
        require(window.selectEntity("outer") && window.moveSelectedBoundaryVertex("outer:vertex1",{12,0},reopened.revision()),"native measured source vertex edit refreshes the whole combined owner");
        const auto copy_source=window.document().snapshot();
        require(copy_source.revision()==reopened.revision()+1 && copy_source.entities().at(area.id)!=reopened.entities().at(area.id) && measurement_linework_source_checks(copy_source.entities()).at(area.id).current,"source geometry exterior lineage and complete group metadata refresh atomically");
        const auto check_copy=[&](const DocumentSnapshot& copied) {
            const auto copy_areas=areas(copied);require(copy_areas.size()==2,"group copy creates exactly one independent owner");
            const auto copied_checks=measurement_linework_source_checks(copied.entities());
            require(copied_checks.at(area.id).current,"original group remains current after an independent copy");
            for(const auto& copy:copy_areas) if(copy.id!=area.id) {
                require(copied_checks.at(copy.id).current,"copied group validates against its complete copied source graph");
                const auto old_bounds=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(copy_source.entities().at(area.id))));
                const auto new_bounds=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(copy)));
                require(new_bounds.minimum.x>old_bounds.maximum.x,"copied measured geometry is placed outside the original bounds");
                std::set<std::string> owners;
                for(const auto& member:copy.extensions.at("measurement_linework_group").at("members"))
                    for(const auto& edge:member) for(const auto& use:edge) owners.insert(use.at("owner_id").get<std::string>());
                require(owners.size()==(four?3:2) && !owners.contains("outer") && !owners.contains("seam-v") && !owners.contains("seam-h"),"group copy independently remaps exterior and canceled internal seam owners");
                for(const auto& id:owners)require(copied.entities().at(id).type=="measurement_linework" && copied.entities().at(id).required,"every copied group source is real required typed measured geometry");
                require(copied.entities().at(area.id)==copy_source.entities().at(area.id),"group copy preserves the original owner exactly");
            }
        };
        require(window.selectEntity(QString::fromStdString(area.id)) && window.transformSelectedBoundary("0",false,false,"20 m","0 m",true),"native group clone includes complete measured source graph");
        const auto cloned=window.document().snapshot();check_copy(cloned);
        auto cloned_area=areas(cloned);const auto clone=cloned_area.front().id==area.id?cloned_area.back():cloned_area.front();
        require(window.selectEntity(QString::fromStdString(clone.id)) && window.transformSelectedBoundary("0",false,false,"5 m","0 m",false),"native combined-owner move includes canceled internal seams atomically");
        check_copy(window.document().snapshot());
        require(window.undoCommand() && window.document().snapshot().entities()==cloned.entities() && window.undoCommand() && window.document().snapshot().entities()==copy_source.entities(),"group movement and clone each undo as one exact transaction");
        require(window.selectEntity(QString::fromStdString(area.id)),"select native group clipboard source");
        if(!window.copySelection())throw std::runtime_error("Native group clipboard copy: "+window.lastError().toStdString());
        if(!window.pasteSelection())throw std::runtime_error("Native group clipboard paste: "+window.lastError().toStdString());
        const auto pasted=window.document().snapshot();check_copy(pasted);
        const auto old_report=build_appraisal_document_report(copy_source,"p",AreaUnit::square_metre);
        const auto pasted_report=build_appraisal_document_report(pasted,"p",AreaUnit::square_metre);
        require(old_report.qualified && pasted_report.qualified && std::abs(pasted_report.calculation->property.gla().total.square_metres-2*old_report.calculation->property.gla().total.square_metres)<1e-8,"separated pasted group contributes its explicit GLA exactly once alongside the original");
        Entity pasted_owner;for(const auto& value:areas(pasted))if(value.id!=area.id)pasted_owner=value;
        require(!pasted_owner.extensions.contains("boundary_geometry_derivation"),"new plain source-derived pasted owner does not acquire an unrelated rigid origin proof");
        std::string copied_outer;
        for(const auto& member:pasted_owner.extensions.at("measurement_linework_group").at("members"))
            for(const auto& edge:member) for(const auto& use:edge) {
                const auto id=use.at("owner_id").get<std::string>();
                const auto decoded=decode_measurement_linework_model(pasted.entities().at(id).properties.at("model"));
                if(decoded.model->closed)copied_outer=id;
            }
        require(!copied_outer.empty(),"pasted group retains its independent closed outer stroke");
        const auto outer_model=decode_measurement_linework_model(pasted.entities().at(copied_outer).properties.at("model"));
        auto position=replay_measurement_linework(*outer_model.model).edges.front().segment.end;position.x+=1;
        require(window.selectEntity(QString::fromStdString(copied_outer)),"select pasted measured source for subsequent authored edit");
        if(!window.moveSelectedBoundaryVertex(QString::fromStdString(outer_model.model->edges.front().end_vertex_id),position,pasted.revision()))
            throw std::runtime_error("Edit pasted group source: "+window.lastError().toStdString());
        const auto edited_copy=window.document().snapshot();const auto edited_checks=measurement_linework_source_checks(edited_copy.entities());
        require(edited_copy.revision()==pasted.revision()+1 && edited_copy.entities().at(pasted_owner.id)!=pasted_owner && edited_checks.at(pasted_owner.id).current && edited_checks.at(area.id).current,"post-paste source edit refreshes the complete copied group in one revision");
        for(const auto& [id,original]:copy_source.entities())require(edited_copy.entities().at(id)==original,"post-paste source edit preserves every original entity");
        const auto edited_report=build_appraisal_document_report(edited_copy,"p",AreaUnit::square_metre);
        require(edited_report.qualified && std::abs(edited_report.calculation->property.gla().total.square_metres-pasted_report.calculation->property.gla().total.square_metres-5)<1e-8,"post-paste source edit recalculates qualified GLA from copied geometry");
        require(window.undoCommand() && window.document().snapshot().entities()==pasted.entities() && window.redoCommand() && window.document().snapshot().entities()==edited_copy.entities() && window.undoCommand() && window.document().snapshot().entities()==pasted.entities(),"post-paste source edit undoes/redoes as one exact transaction");
        require(window.undoCommand() && window.document().snapshot().entities()==copy_source.entities() && window.undoCommand() && window.document().snapshot().entities()==reopened.entities(),"group clipboard paste and source refresh each undo exactly once");
    }
}
void test_combined_stale_review() {
    auto document=adjacent_fixture(false);MainWindow window(document);prepare(window);
    const auto before=document->snapshot();
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        combine_all(dialog,rows);require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"combined preview is initially admitted");
        auto property=document->snapshot().entities().at("p");property.properties["name"]="Changed during combined review";
        document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert(property)},{},"External fixture change"});
        buttons.button(QDialogButtonBox::Apply)->click();
        require(areas(document->snapshot()).empty() && !buttons.button(QDialogButtonBox::Apply)->isEnabled(),"stale combined preview is refused and disabled");dialog.reject();
    });
    require(document->revision()==before.revision()+1 && areas(document->snapshot()).empty(),"stale combination leaves only the unrelated authorized change");
}
void test_combined_member_topology_loss_stays_stale() {
    MainWindow window(adjacent_fixture(true,false));prepare(window);
    review(window,[&](QDialog& dialog,QTableWidget& rows,QDialogButtonBox& buttons) {
        require(rows.rowCount()==4,"original-endpoint fixture starts with four bounded regions");combine_all(dialog,rows);
        buttons.button(QDialogButtonBox::Apply)->click();
    });
    const auto owner=areas(window.document().snapshot()).front();
    auto* workflow=window.findChild<QComboBox*>("calculationWorkflow");require(workflow,"topology-loss fixture has the native workflow selector");
    workflow->setCurrentIndex(workflow->findData("appraisal"));
    require(window.selectEntity(QString::fromStdString(owner.id)) && window.editSelectedAppraisalFacts(QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),"topology-loss fixture has explicit qualified appraisal facts");
    const auto before=window.document().snapshot();
    const auto report=build_appraisal_document_report(before,"p",AreaUnit::square_metre);
    const auto source_check=measurement_linework_source_checks(before.entities()).at(owner.id);
    if (!report.qualified || !source_check.current) {
        std::string diagnostic="Four-member baseline is not qualified/current: "+source_check.diagnostic;
        for (const auto& issue:report.issues) diagnostic+="; "+issue;
        throw std::runtime_error(diagnostic);
    }
    require(window.selectEntity("outer") && window.moveSelectedBoundaryVertex("outer:vertex1",{12,0},before.revision()),"authored source edit can leave an explicitly stale measured group");
    const auto after=window.document().snapshot();const auto checks=measurement_linework_source_checks(after.entities());
    require(after.revision()==before.revision()+1 && after.entities().at("outer")!=before.entities().at("outer"),"topology-loss edit changes only its authored source transaction");
    require(after.entities().at(owner.id)==before.entities().at(owner.id) && !checks.at(owner.id).current,"dangling member seam leaves the saved whole group unchanged and explicitly stale");
    require(!build_appraisal_document_report(after,"p",AreaUnit::square_metre).qualified,"lost member topology withholds qualified GLA instead of guessing a new membership");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"Undo restores the exact current group and its original member topology");
}
void test_vendor_group_marker_on_other_boundary_owners() {
    for(const auto* owner_type:{"boundary","room_boundary"}) {
        IdentifiedBoundary model{"vendor-area",owner_type,{}};
        const Vec2 points[]{{0,0},{10,0},{10,10},{0,10}};
        for(std::size_t i=0;i<4;++i)model.segments.push_back({"edge"+std::to_string(i),"vertex"+std::to_string(i),"vertex"+std::to_string((i+1)%4),{points[i],points[(i+1)%4],0}});
        auto entity=encode_identified_boundary_entity(model);
        entity.properties.update({{"floor_id","f"},{"layer_id","l"},{"classification","living"},{"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1}});
        const nlohmann::json marker={{"version",99},{"members",nlohmann::json::array({{{"vendor","opaque metadata"},{"owner_id","vendor-area"}}})}};
        entity.extensions["measurement_linework_group"]=marker;const auto marker_bytes=marker.dump();
        auto document=std::make_shared<Document>(Document::create({{"p","property",{{"name","Vendor boundary"}},false},{"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},entity}));
        MainWindow window(document);window.setAttribute(Qt::WA_DontShowOnScreen,true);window.show();window.setMetricUnits(true);require(window.selectEntity("vendor-area"),"select ordinary owner with opaque vendor group marker");events();
        auto* total=window.findChild<QLabel*>("calculationBuildingTotal");require(total && total->text().contains("100.00"),"vendor group marker on other owners does not withhold calculable area totals");
        auto* review_action=window.findChild<QAction*>("reviewMeasuredAreaSources");require(review_action && !review_action->isEnabled(),"ordinary owner vendor marker offers no measured-group source repair");
        const auto original=document->snapshot();
        const auto verify=[&] {
            std::size_t count=0;
            const auto copied_snapshot=document->snapshot();
            for(const auto& [id,copied]:copied_snapshot.entities())if(copied.type==owner_type) {
                ++count;require(copied.extensions.at("measurement_linework_group").dump()==marker_bytes,"ordinary boundary clone/paste preserves opaque vendor marker bytes");
            }
            require(count==2,"ordinary boundary copy produces exactly one copied owner");
        };
        require(window.transformSelectedBoundary("0",false,false,"20 m","0 m",true),"ordinary boundary with vendor marker clones normally");verify();
        require(window.undoCommand() && document->snapshot().entities()==original.entities(),"ordinary vendor-marker clone undoes exactly");
        require(window.selectEntity("vendor-area") && window.copySelection() && window.pasteSelection(),"ordinary boundary with vendor marker copies and pastes normally");verify();
    }
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication application(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-nested-area-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled font loads for native review and PDF");
        application.setFont(QFont(QStringLiteral("Inter"),10));
        test_cancel_defaults_and_reference();test_explicit_deduction_history_details_and_pdf();test_stale_preview_and_same_type_rejection();test_phase_review_and_exact_augmented_apply();
        test_ansi_nested_partition_detection();
        test_native_deductions_without_layer_assignment();
        test_combined_adjacent_regions_review_history_and_sources();
        test_combined_stale_review();
        test_combined_member_topology_loss_stays_stale();
        test_vendor_group_marker_on_other_boundary_owners();
    }
    catch(const std::exception& error) {std::cerr << "nested_measured_area_desktop_tests: " << error.what() << '\n'; return 1;}
    std::cout << "Nested measured-area desktop tests passed\n"; return 0;
}
