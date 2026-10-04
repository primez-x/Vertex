#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QUuid>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTextDocument>
#include <QPlainTextEdit>
#include <QToolButton>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using sketch::Entity;
using nlohmann::json;
using sketch::desktop::AppraisalDetailsPanel;
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
Entity entity(std::string id, std::string type, json properties) {
    return {std::move(id),std::move(type),std::move(properties),false,json::object()};
}
json square(double x,double y,double side) {
    return json::array({{{"start",{x,y}},{"end",{x+side,y}},{"sweep_radians",0}},
        {{"start",{x+side,y}},{"end",{x+side,y+side}},{"sweep_radians",0}},
        {{"start",{x+side,y+side}},{"end",{x,y+side}},{"sweep_radians",0}},
        {{"start",{x,y+side}},{"end",{x,y}},{"sweep_radians",0}}});
}
json facts(std::string use="dwelling") {
    return {{"finish","finished"},{"access","direct_interior"},{"ceiling_eligibility","standard"},
        {"area_use",std::move(use)},{"boundary_role","measured_area"}};
}
std::vector<Entity> fixture() {
    return {entity("p","property",{{"name","Home"},{"calculation_workflow","appraisal"},
        {"appraisal_policy",{{"policy_kind","residential_declared"},{"version",1},
            {"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}}),
        entity("b","building",{{"name","Main building"},{"property_id","p"}}),
        entity("f","floor",{{"name","Ground floor"},{"building_id","b"},{"appraisal_facts",{{"grade","above"}}}}),
        entity("l","layer",{{"floor_id","f"}}),
        entity("a","measurement_boundary",{{"name","Living"},{"property_id","p"},{"building_id","b"},
            {"floor_id","f"},{"layer_id","l"},{"calculation_scope","building"},
            {"boundary",square(0,0,3.048)},{"appraisal_facts",facts()}})};
}
QString label(AppraisalDetailsPanel& panel,const char* object) {
    const auto* value=panel.findChild<QLabel*>(QString::fromLatin1(object));
    require(value!=nullptr,"expected named detail label");return value->text();
}
QPushButton* button(AppraisalDetailsPanel& panel,const char* object) {
    auto* value=panel.findChild<QPushButton*>(QString::fromLatin1(object));
    require(value!=nullptr,"expected named detail action");return value;
}
void ansi_canonical_units_declarations_and_curve_dimensions() {
    auto entities=fixture();
    auto& policy=entities.front().properties["appraisal_policy"];
    policy["policy_kind"]="ansi_z765_2021";
    policy["ansi"]={{"interior_inspected",true},{"direct_measurement",true},
        {"acquisition_increment","tenth_foot"},{"limitations_statement","Interior inspected; no inaccessible areas."}};
    entities[2].properties["appraisal_facts"]["ansi"]={{"any_part_below_grade",false}};
    entities.back().properties["appraisal_facts"]["ansi"]={{"year_round_suitable",true},
        {"finish_matches_dwelling",true},{"dwelling_identity","primary"},
        {"ceiling",{{"kind","flat"},{"minimum_height_m",2.4384}}}};
    auto adu=entities.back();adu.id="adu";adu.properties["name"]="Guest ADU";
    adu.properties["boundary"]=square(8,0,3.048);
    adu.properties["appraisal_facts"]["ansi"]["dwelling_identity"]="attached_adu";entities.push_back(adu);
    AppraisalDetailsPanel panel;auto document=sketch::Document::create(entities);
    panel.setDocument(document.snapshot(),"p",true);panel.setSelectedBoundary("a");
    require(panel.report() && panel.report()->qualified && label(panel,"appraisalDetailsGla")=="100 sq ft",
        "ANSI primary GLA remains canonical whole square feet in metric workspace and excludes ADU");
    require(label(panel,"appraisalDetailsStatus").contains("ANSI Z765-2021 profile") &&
        label(panel,"appraisalDetailsStatus").contains("validation pending"),
        "the profile and unresolved validation must be visible beside the prominent GLA total");
    const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture_directory.isEmpty()) {
        panel.resize(340, 850); panel.show(); QApplication::processEvents();
        require(QDir().mkpath(capture_directory) && panel.grab().save(QDir(capture_directory).filePath("appraisal-details-ansi-profile.png")),
            "save the actual ANSI profile and GLA panel capture");
        QTemporaryDir library_directory;
        sketch::desktop::MainWindow window(std::make_shared<sketch::Document>(sketch::Document::create(entities)), nullptr,
            library_directory.filePath("library.json"));
        window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280, 900);
        window.setWorkspaceTheme(sketch::WorkspaceTheme::light); window.setMetricUnits(true);
        auto* tabs = window.findChild<QTabWidget*>("sidebarTabs");
        require(tabs != nullptr && tabs->count() == 3, "actual workspace exposes Layers, Library and Details");
        tabs->setCurrentIndex(2); window.show(); QApplication::processEvents();
        require(window.grab().save(QDir(capture_directory).filePath("appraisal-details-ansi-workspace.png")),
            "save the actual workspace with the prominent ANSI profile and GLA total");
    }
    const auto totals=label(panel,"appraisalDetailsTotals");
    require(totals.contains("ADU",Qt::CaseInsensitive) && totals.contains("100 sq ft") && totals.contains("Supplemental metric diagnostic"),
        "ANSI keeps ADU category separate and clearly labels metric diagnostic totals");
    const auto policy_text=label(panel,"appraisalDetailsPolicy");
    require(policy_text.contains("Interior inspected") && policy_text.contains("Direct measurement") &&
        policy_text.contains("Tenth foot") && policy_text.contains("no inaccessible areas"),"ANSI property declarations are visible");
    require(label(panel,"appraisalDetailsStandards").contains("normative",Qt::CaseInsensitive),
        "ANSI rule qualification retains explicit unresolved normative validation");
    const auto trace=label(panel,"appraisalDetailsTrace");
    require(trace.contains("40.0 ft") && trace.contains("10.0 ft") && trace.contains("8.0 ft") &&
        trace.contains("Year-round suitable") && trace.contains("Finish matches dwelling") && trace.contains("Primary"),
        "ANSI tenth-foot dimensions and retained source facts are visible");
    entities[4].properties["boundary"]=json::array({{{"start",{-.3048,0}},{"end",{.3048,0}},{"sweep_radians",std::acos(-1.0)}},
        {{"start",{.3048,0}},{"end",{-.3048,0}},{"sweep_radians",std::acos(-1.0)}}});
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);
    const auto curved=label(panel,"appraisalDetailsTrace");
    require(curved.contains("6.3 ft") && curved.contains("Arc 1: 3.1 ft") && curved.contains("Arc 2: 3.1 ft"),
        "ANSI curve dimensions use analytical arc length and tenth-foot presentation");
    entities[4].properties["boundary"]=square(0,0,3.048);
    entities[4].properties["appraisal_facts"]["access"]="through_unfinished";
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);
    require(panel.report()->qualified && label(panel,"appraisalDetailsGla")=="0 sq ft" &&
        label(panel,"appraisalDetailsTrace").contains("access passes through unfinished space"),
        "nonstandard finished area shows its real classification reason without inflating primary GLA");
    entities[4].properties["appraisal_facts"]["access"]="direct_interior";
    entities[4].properties["appraisal_facts"]["boundary_role"]="stair_footprint";
    entities[4].properties["appraisal_facts"]["ansi"]["ceiling"]={{"kind","stairs"},{"stair_from_floor_id","f"}};
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);
    require(panel.report()->qualified && label(panel,"appraisalDetailsTrace").contains("Stair from floor source") &&
        label(panel,"appraisalDetailsTrace").contains("Stairs"),"descending stair source floor is visible");
    entities.front().properties["appraisal_policy"]["ansi"].erase("direct_measurement");
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);
    require(!panel.report()->qualified && label(panel,"appraisalDetailsGla")=="Totals unavailable",
        "missing ANSI source declaration withholds prominent total");
}
void ansi_ceiling_height_uses_declared_acquisition_precision() {
    auto entities=fixture();
    auto& policy=entities.front().properties["appraisal_policy"];
    policy["policy_kind"]="ansi_z765_2021";
    policy["ansi"]={{"interior_inspected",true},{"direct_measurement",true},
        {"acquisition_increment","tenth_foot"}};
    entities[2].properties["appraisal_facts"]["ansi"]={{"any_part_below_grade",false}};
    auto& evidence=entities.back().properties["appraisal_facts"]["ansi"];
    evidence={{"year_round_suitable",true},{"finish_matches_dwelling",true},
        {"dwelling_identity","primary"},{"ceiling",{{"kind","flat"},{"minimum_height_m",6.96*.3048}}}};
    AppraisalDetailsPanel panel;
    const auto show=[&] {
        auto document=sketch::Document::create(entities);
        panel.setDocument(document.snapshot(),"p",true);panel.setSelectedBoundary("a");
        return label(panel,"appraisalDetailsTrace");
    };
    auto trace=show();
    require(panel.report()->qualified && label(panel,"appraisalDetailsGla")=="100 sq ft",
        "6.96-foot observation rounds to seven feet for flat-ceiling GLA classification");
    require(trace.contains("Recorded minimum ceiling height") && trace.contains("6.96 ft") &&
        trace.contains("Rounded minimum ceiling height") && trace.contains("7.0 ft (nearest tenth foot)"),
        "Details distinguishes retained observation from the actual rounded classification height");
    evidence["ceiling"]["minimum_height_m"]=6.85*.3048;trace=show();
    require(panel.report()->qualified && label(panel,"appraisalDetailsGla")=="0 sq ft" &&
        trace.contains("6.85 ft") && trace.contains("6.9 ft (nearest tenth foot)"),
        "6.85-foot observation reports 6.9 feet and stays nonstandard finished");
    evidence["ceiling"]["minimum_height_m"]=6.951*.3048;trace=show();
    require(label(panel,"appraisalDetailsGla")=="100 sq ft","tenth-foot acquisition governs ceiling threshold");
    policy["ansi"]["acquisition_increment"]="inch";trace=show();
    require(label(panel,"appraisalDetailsGla")=="0 sq ft" && trace.contains("6 ft 11 in (nearest inch)"),
        "inch acquisition uses its own rounded threshold and displays whole inches");
    policy["ansi"].erase("acquisition_increment");trace=show();
    require(!panel.report()->qualified && label(panel,"appraisalDetailsGla")=="Totals unavailable" &&
        trace.contains("Acquisition precision undeclared"),"missing precision cannot silently select a height rule");
}
void narrow_details_keeps_dimensions_and_full_sources_readable() {
    auto entities=fixture();
    const auto long_id=std::string("measurement-boundary-")+std::string(100,'x');
    entities.back().id=long_id;
    entities.back().properties["name"]="Living room with a long retained source identifier";
    auto& policy=entities.front().properties["appraisal_policy"];
    policy["policy_kind"]="ansi_z765_2021";policy["version"]=2;
    policy["ansi"]={{"interior_inspected",true},{"direct_measurement",true},{"acquisition_increment","tenth_foot"}};
    entities[2].properties["appraisal_facts"]["ansi"]={{"any_part_below_grade",false}};
    const sketch::Boundary geometry{{{0,0},{3.048,0},0},{{3.048,0},{3.048,3.048},0},
        {{3.048,3.048},{0,3.048},0},{{0,3.048},{0,0},0}};
    entities.back().properties["appraisal_facts"]["ansi"]={{"year_round_suitable",true},
        {"finish_matches_dwelling",true},{"dwelling_identity","primary"},
        {"ceiling",{{"kind","sloped"},{"room_boundary_id",long_id},{"room_floor_area_m2",9.290304},
            {"at_least_7ft_area_m2",7.0},{"below_5ft_deduction_ids",json::array()},
            {"complete_room_observed",true},{"source_geometry_sha256",sketch::appraisal_ceiling_geometry_digest(geometry,{})}}}};
    AppraisalDetailsPanel panel;panel.resize(320,800);panel.show();
    const auto document=sketch::Document::create(entities);
    panel.setDocument(document.snapshot(),"p",false);panel.setSelectedBoundary(QString::fromStdString(long_id));
    QApplication::processEvents();
    auto* trace=panel.findChild<QLabel*>("appraisalDetailsTrace");
    auto* provenance=panel.findChild<QPlainTextEdit*>("appraisalDetailsProvenance");
    auto* disclosure=panel.findChild<QToolButton*>("appraisalDetailsProvenanceToggle");
    require(trace && provenance && disclosure,"technical source provenance has an independent expandable, copyable view");
    require(!trace->text().contains(QString::fromStdString(long_id)) &&
        provenance->toPlainText().contains(QString::fromStdString(long_id)),"numeric trace omits raw IDs while full source identity remains available without truncation");
    require(provenance->isReadOnly() && !provenance->isVisible(),"technical provenance starts collapsed and cannot edit authoritative data");
    disclosure->click();QApplication::processEvents();
    require(provenance->isVisible(),"source disclosure opens the copyable provenance view");
    auto cursor=provenance->textCursor();cursor.select(QTextCursor::Document);
    require(cursor.selectedText().contains(QString::fromStdString(long_id)),"selected provenance preserves exact source ID for copying");
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!directory.isEmpty())require(QDir().mkpath(directory) && provenance->grab().save(QDir(directory).filePath("appraisal-details-provenance.png")),
        "save actual wrapped source provenance view");
    disclosure->click();require(!provenance->isVisible(),"source disclosure collapses without discarding identity");
    for(const int width:{260,320,460}) {
        QTextDocument rendered;rendered.setDefaultFont(trace->font());rendered.setHtml(trace->text());
        rendered.setTextWidth(width);
        require(rendered.size().width()<=width+1 && rendered.idealWidth()<=width+1,
            "Details trace must wrap inside a narrow panel without clipping measurements or full source identities");
        require(rendered.toPlainText().contains("10.0 ft"),"wrapped dimensions retain their displayed value");
    }
    if(!directory.isEmpty()) {
        require(QDir().mkpath(directory) && trace->grab().save(QDir(directory).filePath("appraisal-details-long-source.png")),
            "save native narrow source trace capture");
    }
    panel.setDocument(document.snapshot(),"missing",false);
    require(provenance->toPlainText().isEmpty() && !disclosure->isEnabled() && !provenance->isVisible(),
        "missing property context clears and closes prior technical source identities");
}
void qualified_units_refresh_and_callbacks() {
    AppraisalDetailsPanel panel;
    auto entities=fixture();auto document=sketch::Document::create(entities);
    panel.setDocument(document.snapshot(),"p",false);
    require(panel.report() && panel.report()->qualified,"qualified report without canvas selection");
    require(label(panel,"appraisalDetailsGla")=="100.00 sq ft","prominent authoritative GLA");
    require(label(panel,"appraisalDetailsStandards").contains("ANSI review not verified"),"truthful residential standards status");
    panel.setSelectedBoundary("a");
    require(label(panel,"appraisalDetailsTrace").contains("40.00 ft") &&
        label(panel,"appraisalDetailsTrace").contains("10.00 ft"),"perimeter and edge dimensions visible");
    QString located,edited,setup,reported;sketch::Revision revision=999;
    panel.setLocateRequested([&](const QString& id,sketch::Revision value){located=id;revision=value;});
    panel.setFactsRequested([&](const QString& id,sketch::Revision){edited=id;});
    panel.setSetupRequested([&](const QString& id){setup=id;});
    panel.setReportRequested([&](const QString& id){reported=id;});
    button(panel,"appraisalDetailsLocate")->click();button(panel,"appraisalDetailsEditFacts")->click();
    button(panel,"appraisalDetailsSetup")->click();button(panel,"appraisalDetailsReport")->click();
    require(located=="a" && edited=="a" && setup=="p" && reported=="p" && revision==document.revision(),"scoped current callbacks");
    auto* tree=panel.findChild<QTreeWidget*>("appraisalDetailsAreas");require(tree!=nullptr,"area hierarchy exists");
    auto* item=tree->currentItem();located.clear();tree->itemActivated(item,0);
    require(located=="a","row activation locates source boundary");
    panel.setDocument(document.snapshot(),"p",true);
    require(label(panel,"appraisalDetailsGla")=="9.29 m²" && label(panel,"appraisalDetailsTrace").contains("12.19 m"),"metric refresh retains physical values");
    entities.back().properties["boundary"]=square(0,0,6.096);document=sketch::Document::create(entities);
    panel.setDocument(document.snapshot(),"p",false);
    require(label(panel,"appraisalDetailsGla")=="400.00 sq ft","new snapshot replaces stale header even same revision");
    panel.setDocument(document.snapshot(),"missing",false);
    require(!panel.report() && panel.selectedBoundaryId().isEmpty() && !button(panel,"appraisalDetailsLocate")->isEnabled(),"missing property clears previous report and actions");
    require(!label(panel,"appraisalDetailsGla").contains("400"),"no stale total after context failure");
}
void undeclared_and_invalid_measurements() {
    AppraisalDetailsPanel panel;auto entities=fixture();entities.front().properties.erase("calculation_workflow");
    auto document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",false);
    require(panel.report() && !panel.report()->qualified && label(panel,"appraisalDetailsGla")=="Totals unavailable", "undeclared property never shows numeric totals");
    require(label(panel,"appraisalDetailsStatus").contains("Setup"),"new/manual workflow gives actionable setup guidance");
    entities=fixture();entities.back().properties["appraisal_facts"].erase("finish");
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",false);panel.setSelectedBoundary("a");
    require(!panel.report()->qualified && label(panel,"appraisalDetailsTrace").contains("Diagnostic") &&
        label(panel,"appraisalDetailsTrace").contains("finish"),"missing facts retain physical trace and reason but no totals");
    entities.back().properties["boundary"][0]["sweep_radians"]="bad";
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",false);
    require(label(panel,"appraisalDetailsTrace").contains("Current measurement unavailable") &&
        !label(panel,"appraisalDetailsTrace").contains("40.00 ft"),"invalid geometry removes stale dimensions");
}
void categories_floors_deductions_and_phase() {
    AppraisalDetailsPanel panel;auto entities=fixture();entities.back().properties["boundary"]=square(0,0,4);
    entities.back().properties["deduction_ids"]=json::array({"v2","v1"});
    for(const auto& [id,x]:std::vector<std::pair<std::string,double>>{{"v1",.5},{"v2",1.5}})
        entities.push_back(entity(id,"measurement_boundary",{{"property_id","p"},{"building_id","b"},
            {"floor_id","f"},{"layer_id","l"},{"boundary",square(x,.5,2)},
            {"appraisal_facts",{{"boundary_role","other_void"}}}}));
    auto garage=fixture().back();garage.id="garage";garage.properties["boundary"]=square(8,0,2);garage.properties["appraisal_facts"]=facts("garage");entities.push_back(garage);
    entities.push_back(entity("f2","floor",{{"name","Basement"},{"building_id","b"},{"appraisal_facts",{{"grade","below"}}}}));
    auto basement=fixture().back();basement.id="basement";basement.properties["floor_id"]="f2";basement.properties["boundary"]=square(0,0,3);entities.push_back(basement);
    auto document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);panel.setSelectedBoundary("a");
    require(panel.report()->qualified && label(panel,"appraisalDetailsGla")=="10.00 m²","union deductions and other categories never inflate GLA");
    const auto totals=label(panel,"appraisalDetailsTotals");
    require(totals.contains("Garage") && totals.contains("4.00 m²") && totals.contains("Below grade finished") && totals.contains("9.00 m²"),"separate garage and below grade totals");
    const auto trace=label(panel,"appraisalDetailsTrace");
    require(trace.contains("Requested: 4.00 m²") && trace.contains("Applied: 2.00 m²") && trace.contains("Physical net"),"overlap deduction trace exposes requested and marginal applied values");
    std::set<std::string,std::less<>> visible;for(const auto& entry:entities)if(entry.id!="basement")visible.insert(entry.id);
    panel.setDocument(document.snapshot(),"p",true,&visible);
    require(panel.report()->qualified && panel.report()->boundaries.size()==4 &&
        label(panel,"appraisalDetailsGla")=="10.00 m²","semantic phase scope applied to authoritative report");
    panel.setDocument(document.snapshot(),"p",true);
    require(panel.report()->boundaries.size()==5,"ordinary refresh restores all boundaries independent of workspace visibility");
    entities.front().properties["appraisal_policy"]["policy_kind"]="light_commercial_declared";
    entities.front().properties["appraisal_policy"]["property_kind"]="light_commercial";
    document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",true);
    require(label(panel,"appraisalDetailsStandards").contains("ANSI residential standard not applicable"),"commercial standards status never suggests ANSI certification");
}
void stale_wall_sources() {
    AppraisalDetailsPanel panel;auto entities=fixture();std::vector<std::string> ids;
    for(const auto& edge:square(0,0,3.048)) {
        const auto id="w"+std::to_string(ids.size());ids.push_back(id);
        entities.push_back(entity(id,"wall",{{"baseline",edge},{"thickness_m",.2},{"height_m",3},{"elevation_m",0},
            {"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"}}));
    }
    auto document=sketch::Document::create(entities);const auto derived=sketch::derive_exterior_wall_measurement(document.snapshot(),ids);
    entities[4].properties["boundary"]=json::array();
    for(const auto& edge:derived.boundary)entities[4].properties["boundary"].push_back(
        {{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    entities[4].properties["wall_measurement_source"]=derived.source;document=sketch::Document::create(entities);
    panel.setDocument(document.snapshot(),"p",false);panel.setSelectedBoundary("a");require(panel.report()->qualified,"current wall fixture qualifies");
    entities[5].properties["thickness_m"]=.4;document=sketch::Document::create(entities);panel.setDocument(document.snapshot(),"p",false);
    require(!panel.report()->qualified && label(panel,"appraisalDetailsGla")=="Totals unavailable" &&
        label(panel,"appraisalDetailsTrace").contains("Current measurement unavailable"),"stale wall provenance clears old totals and trace");
}

using sketch::desktop::MainWindow;
template<class Widget> Widget& child(QWidget& parent,const char* name) {
    auto* value=dynamic_cast<Widget*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));
    require(value!=nullptr,"native Details control must exist");return *value;
}
void details_dialog(MainWindow& window,const char* action,const char* name,
    const std::function<void(QDialog&)>& inspect) {
    std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>(QString::fromLatin1(name));
        try {require(dialog!=nullptr,"Details action must open its existing native flow");opened=true;inspect(*dialog);}
        catch(...) {failure=std::current_exception();}
        if(dialog && dialog->isVisible())dialog->reject();
    });
    child<QPushButton>(window,action).click();
    if(failure)std::rethrow_exception(failure);
    require(opened,"Details dialog action executed");
}
void save_setup(QDialog& dialog) {
    child<QDialogButtonBox>(dialog,"appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
}
void capture(MainWindow& window,const char* name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    QApplication::processEvents();
    require(QDir().mkpath(directory) && window.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),
        "save full native MainWindow Details capture");
}
void native_main_window_details() {
    QTemporaryDir directory;require(directory.isValid(),"isolated native fixture directory");
    MainWindow window({},nullptr,directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1280,900);window.show();QApplication::processEvents();
    auto& tabs=child<QTabWidget>(window,"sidebarTabs");
    require(tabs.count()==3 && tabs.tabText(2)=="Details" && window.selectedEntityId().isEmpty(),
        "third Details tab is present and usable without a canvas selection");
    tabs.setCurrentIndex(2);
    auto& panel=child<AppraisalDetailsPanel>(window,"appraisalDetailsPanel");
    require(label(panel,"appraisalDetailsGla")=="Totals unavailable" && button(panel,"appraisalDetailsSetup")->isEnabled(),
        "new property exposes actionable Setup without fabricated totals");
    const auto initial=window.document().snapshot();
    details_dialog(window,"appraisalDetailsSetup","appraisalSetupDialog",[&](QDialog& dialog) {
        child<QCheckBox>(dialog,"appraisalSetupEnabled").setChecked(true);
        child<QSpinBox>(dialog,"appraisalSetupPrecision").setValue(3);
        dialog.reject();
    });
    require(window.document().revision()==initial.revision() && window.document().snapshot().entities()==initial.entities(),
        "Setup cancellation is mutation-free");
    details_dialog(window,"appraisalDetailsSetup","appraisalSetupDialog",[&](QDialog& dialog) {
        require(child<QCheckBox>(dialog,"appraisalSetupEnabled").isChecked(),"new document Setup offers enabled appraisal");
        const auto unchanged=window.document().snapshot();save_setup(dialog);
        require(dialog.isVisible() && window.document().snapshot().entities()==unchanged.entities() &&
            window.document().revision()==unchanged.revision(),"undeclared enabled Setup Save is rejected without mutations");
        for (const auto& [name,token] : std::initializer_list<std::pair<const char*,const char*>>{
            {"appraisalSetupPolicy","residential_declared"},{"appraisalSetupPropertyKind","detached_single_family"},
            {"appraisalSetupMeasurementBasis","exterior"}}) {
            auto& combo=child<QComboBox>(dialog,name);
            require(combo.currentData().toString().isEmpty(),"new property declarations are explicitly undeclared");
            combo.setCurrentIndex(combo.findData(QString::fromLatin1(token)));
        }
        save_setup(dialog);
    });
    const auto configured=window.document().snapshot();
    const auto& property=configured.entities().at("property-1");
    require(configured.revision()==initial.revision()+1 && property.properties.at("calculation_workflow")=="appraisal" &&
        property.properties.at("appraisal_policy").at("version")==1 &&
        property.properties.at("appraisal_policy").at("measurement_basis")=="exterior", "Setup saves policy and workflow in one ordinary command");
    require(window.undoCommand() && window.document().snapshot().entities()==initial.entities() && window.redoCommand(),
        "Setup has atomic undo and redo");
    window.setMetricUnits(true);
    const sketch::Boundary shape={{{0,0},{10,0},0},{{10,0},{10,10},0},{{10,10},{0,10},0},{{0,10},{0,0},0}};
    const auto area=window.createBoundary(shape);require(!area.isEmpty(),"native fixture can author an area after Setup");
    const auto declaration=QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})");
    require(window.editSelectedAppraisalFacts(declaration),"existing facts command declares the native area");
    require(label(panel,"appraisalDetailsGla")=="100.00 m²" && panel.selectedBoundaryId()==area,
        "document edits refresh GLA and canvas area selection synchronizes Details");
    require(window.selectEntity({}),"clear canvas selection");
    panel.setSelectedBoundary(area);
    window.setMetricUnits(false);
    require(label(panel,"appraisalDetailsGla")=="1076.39 sq ft" && panel.selectedBoundaryId()==area && window.selectedEntityId().isEmpty(),
        "unit refresh preserves an independently chosen Details row");
    window.setMetricUnits(true);
    const auto before_hide=label(panel,"appraisalDetailsGla");
    require(window.setContainerVisible(window.activeLayerId(),false) && label(panel,"appraisalDetailsGla")==before_hide,
        "workspace hidden layer never changes authoritative Details total");
    panel.setSelectedBoundary(area);
    button(panel,"appraisalDetailsLocate")->click();require(window.selectedEntityId()==area &&
        window.entityVisible(area) && window.workspace()==sketch::desktop::Workspace::measurement,
        "Details locate reveals the hidden source layer and selects the area on its fitted canvas");
    details_dialog(window,"appraisalDetailsEditFacts","appraisalFactsDialog",[](QDialog& dialog){dialog.reject();});
    details_dialog(window,"appraisalDetailsReport","appraisalReportDialog",[](QDialog& dialog){dialog.reject();});
    const auto before_precision=window.document().snapshot();
    details_dialog(window,"appraisalDetailsSetup","appraisalSetupDialog",[&](QDialog& dialog) {
        child<QSpinBox>(dialog,"appraisalSetupPrecision").setValue(3);save_setup(dialog);
    });
    const auto precision=window.document().snapshot();
    require(precision.revision()==before_precision.revision()+1 && label(panel,"appraisalDetailsGla")=="100.000 m²" &&
        precision.entities().at("property-1").properties.at("measurement_calculation_profile")==
        before_precision.entities().at("property-1").properties.at("measurement_calculation_profile") &&
        precision.entities().at(area.toStdString()).properties==before_precision.entities().at(area.toStdString()).properties,
        "same-workflow Setup edits precision without resetting saved measurement rules or area facts");
    require(window.undoCommand() && window.document().snapshot().entities()==before_precision.entities(),"policy-only Setup undo is atomic");
    require(window.selectEntity({}),"native captures show Details without canvas inspector");panel.setSelectedBoundary(area);
    window.setWorkspaceTheme(sketch::WorkspaceTheme::light);capture(window,"appraisal-details-main-window-light.png");
    window.setWorkspaceTheme(sketch::WorkspaceTheme::dark);capture(window,"appraisal-details-main-window-dark.png");
}

void native_stale_context_withholds_actions_and_totals() {
    QTemporaryDir directory;require(directory.isValid(),"isolated stale-context fixture directory");
    auto document=std::make_shared<sketch::Document>(sketch::Document::create(fixture()));
    MainWindow window(document,nullptr,directory.filePath("text-library.json"));window.setMetricUnits(true);
    auto& panel=child<AppraisalDetailsPanel>(window,"appraisalDetailsPanel");panel.setSelectedBoundary("a");
    require(window.selectedEntityId().isEmpty(),"Details row selection is independent before stale action");
    auto source=document->snapshot();auto changed=source.entities().at("a");changed.properties["boundary"]=square(0,0,6.096);
    document->apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(changed)},{},"external area edit"});
    button(panel,"appraisalDetailsLocate")->click();
    require(window.selectedEntityId().isEmpty() && !window.lastError().isEmpty() && label(panel,"appraisalDetailsGla")=="37.16 m²",
        "stale locate rejects selection and rebuilds authoritative edited GLA");
    source=document->snapshot();
    bool refused=false;
    try { document->apply(sketch::ApplyEntityChanges{source.revision(),
        {sketch::EntityChange::upsert(entity("invalid-phase","model_phases",json::object()))},{},"invalid semantic phase fixture"}); }
    catch(const sketch::DocumentError& error) { refused=error.code()==sketch::DocumentErrorCode::invalid_entity; }
    require(refused && document->revision()==source.revision() && document->snapshot().entities()==source.entities(),
        "malformed semantic phase cannot enter the authoritative document");
    button(panel,"appraisalDetailsLocate")->click();
    require(panel.report() && panel.report()->qualified && label(panel,"appraisalDetailsGla")=="37.16 m²" &&
        window.selectedEntityId()=="a", "rejected semantic corruption leaves current Details totals and action intact");
}

}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-appraisal-details-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled Inter font loads for native Details capture");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        narrow_details_keeps_dimensions_and_full_sources_readable();ansi_ceiling_height_uses_declared_acquisition_precision();ansi_canonical_units_declarations_and_curve_dimensions();qualified_units_refresh_and_callbacks();undeclared_and_invalid_measurements();
        categories_floors_deductions_and_phase();stale_wall_sources();native_main_window_details();native_stale_context_withholds_actions_and_totals();
        std::cout<<"appraisal_details_panel_tests passed\n";return 0;
    } catch(const std::exception& failure) {std::cerr<<"appraisal_details_panel_tests: "<<failure.what()<<'\n';return 1;}
}
