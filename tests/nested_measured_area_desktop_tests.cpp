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
        test_combined_adjacent_regions_review_history_and_sources();
        test_combined_stale_review();
        test_combined_member_topology_loss_stays_stale();
        test_vendor_group_marker_on_other_boundary_owners();
    }
    catch(const std::exception& error) {std::cerr << "nested_measured_area_desktop_tests: " << error.what() << '\n'; return 1;}
    std::cout << "Nested measured-area desktop tests passed\n"; return 0;
}
