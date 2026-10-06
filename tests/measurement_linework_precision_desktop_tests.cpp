#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/model_phases.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <exception>
#include <functional>
#include <numbers>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
PlanCanvas& prepare(MainWindow& window, bool metric = true) {
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); events(); window.setMetricUnits(metric);
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "native measurement canvas exists");
    canvas->setOverviewMapEnabled(false); canvas->setSnapEnabled(false); return *canvas;
}
void key(PlanCanvas& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(&canvas, &event); events();
}
void field(QDialog& dialog, const char* name, const QString& text) {
    auto* edit = dialog.findChild<QLineEdit*>(QString::fromLatin1(name));
    require(edit, "shared precision field exists"); edit->setText(text);
}
void choose(QDialog& dialog, int index) {
    auto* methods = dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"));
    require(methods && methods->count() == 8, "all eight analytical methods are available");
    methods->setCurrentIndex(index);
    if (index >= 4 && index <= 6)
        dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"))->setCurrentIndex(0);
}
void accept(QDialog& dialog) {
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("boundaryInputAdd"));
    require(add && add->isEnabled(), "valid analytical input enables admission"); add->click();
}
void input(MainWindow& window, PlanCanvas& canvas, const std::function<void(BoundaryInputDialog&)>& operation) {
    bool recognized = false; std::exception_ptr failure;
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<BoundaryInputDialog*>(window.findChild<QDialog*>(QStringLiteral("measuredLineInput")));
        recognized = dialog && dialog->isVisible();
        if (!dialog) {
            if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject();
            return;
        }
        try { operation(*dialog); }
        catch (...) { failure = std::current_exception(); dialog->reject(); }
    });
    key(canvas, Qt::Key_D);
    if (failure) std::rethrow_exception(failure);
    require(recognized, "D opens shared native measured precision input");
}
MeasurementLinework model(const DocumentSnapshot& snapshot) {
    std::optional<MeasurementLinework> result;
    for (const auto& [id, entity] : snapshot.entities()) if (entity.type == "measurement_linework") {
        require(!result, "uninterrupted input keeps one measured stroke");
        result = decode_measurement_linework_model(entity.properties.at("model")).model;
    }
    require(result.has_value(), "admitted measured model decodes"); return *result;
}
bool close_point(Vec2 actual, Vec2 expected) {
    return std::abs(actual.x-expected.x) < 1e-9 && std::abs(actual.y-expected.y) < 1e-9;
}
void capture(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name)), "native precision capture saves");
}

// This exercises the actual D command. Reverting it to the old distance-only
// dialog loses analytical construction choices and makes this regression fail.
void test_live_stroke_offers_analytical_methods() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); events();
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "native measurement canvas exists");
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0, 0}),
            "native precision fixture anchors a live stroke");
    bool recognized = false;
    bool all_methods = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("measuredLineInput"));
        recognized = dialog && dialog->isVisible();
        if (dialog) {
            auto* method = dialog->findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"));
            all_methods = method && method->count() == 8;
            dialog->reject();
        }
    });
    QKeyEvent key(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, QStringLiteral("d"));
    QApplication::sendEvent(canvas, &key); events();
    require(recognized, "D opens the existing native measured input dialog");
    require(all_methods, "live Measured lines D input must offer all eight analytical construction methods");
}

void test_all_methods_in_both_units() {
    constexpr double pi = std::numbers::pi;
    const BoundaryConstructionKind kinds[] = {BoundaryConstructionKind::line_heading,
        BoundaryConstructionKind::line_rise_run, BoundaryConstructionKind::line_relative_turn,
        BoundaryConstructionKind::line_to_point, BoundaryConstructionKind::arc_chord_angle,
        BoundaryConstructionKind::arc_chord_height, BoundaryConstructionKind::arc_chord_length,
        BoundaryConstructionKind::arc_start_tangent};
    for (bool metric : {false,true}) for (int method=0; method<8; ++method) {
        MainWindow window; auto& canvas=prepare(window,metric); const double unit=metric ? 1.0 : 0.3048;
        require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "method fixture anchors measured stroke");
        if (method==2) require(window.appendMeasurementLineworkPoint({unit,0}), "relative input has real previous edge");
        const auto before=window.document().snapshot();
        input(window,canvas,[&](BoundaryInputDialog& dialog) {
            choose(dialog,method);
            field(dialog,"boundaryInputLength",QStringLiteral("2"));
            field(dialog,"boundaryInputHeading",QStringLiteral("0 deg"));
            field(dialog,"boundaryInputRise",QStringLiteral("1")); field(dialog,"boundaryInputRun",QStringLiteral("2"));
            field(dialog,"boundaryInputTurn",QStringLiteral("pi/2"));
            field(dialog,"boundaryInputEndX",QStringLiteral("2"));
            field(dialog,"boundaryInputEndY",method==3 ? QStringLiteral("1") : QStringLiteral("0"));
            field(dialog,"boundaryInputSweep",QStringLiteral("90 deg"));
            field(dialog,"boundaryInputHeight",QStringLiteral("1"));
            field(dialog,"boundaryInputArcLength",QStringLiteral("3.141592653589793"));
            field(dialog,"boundaryInputTangent",QStringLiteral("0 rad"));
            if (method==6) dialog.findChild<QCheckBox*>(QStringLiteral("boundaryInputClockwise"))->setChecked(true);
            if (method==7) capture(dialog,metric ? QStringLiteral("measured-precision-metric-arc.png") : QStringLiteral("measured-precision-imperial-arc.png"));
            accept(dialog);
        });
        const auto after=window.document().snapshot(); const auto saved=model(after);
        require(after.revision()==before.revision()+1 && saved.edges.size()==(method==2 ? 2U : 1U), "each analytical method commits exactly one edge");
        const auto replay=replay_measurement_linework(saved); const auto& segment=replay.edges.back().segment;
        const Vec2 endpoints[]={{2*unit,0},{2*unit,unit},{unit,2*unit},{2*unit,unit},
            {2*unit,0},{2*unit,0},{2*unit,0},{2*unit,2*unit}};
        const double sweeps[]={0,0,0,0,pi/2,pi,-pi,pi/2};
        const double lengths[]={2*unit,std::sqrt(5.0)*unit,2*unit,std::sqrt(5.0)*unit,
            pi/std::sqrt(2.0)*unit,pi*unit,pi*unit,pi*unit};
        require(close_point(segment.end,endpoints[method]) && std::abs(segment.sweep_radians-sweeps[method])<1e-9 &&
            std::abs(segment_length(segment)-lengths[method])<1e-9, "all methods preserve independent analytical endpoint, sweep and length");
        const auto& receipt=saved.edges.back().receipt;
        require(receipt.kind==kinds[method], "selected form persists its true analytical receipt kind");
        if (receipt.distance) require(receipt.distance->original_expression=="2" && receipt.distance->entered_unit==(metric ? Unit::metre : Unit::foot), "bare length keeps expression and unit basis");
        if (receipt.heading) require(receipt.heading->original_expression=="0 deg", "heading expression survives");
        if (receipt.turn) require(receipt.turn->original_expression=="pi/2", "relative turn expression survives");
        if (receipt.rise) require(receipt.rise->original_expression=="1" && receipt.run->original_expression=="2", "rise and run expressions survive");
        if (receipt.angle) require(receipt.angle->original_expression=="90 deg", "chord angle expression survives");
        if (receipt.height) require(receipt.height->original_expression=="1", "height expression survives");
        if (receipt.arc_length) require(receipt.arc_length->original_expression=="3.141592653589793", "arc length expression survives");
        if (receipt.tangent) require(receipt.tangent->original_expression=="0 rad" && receipt.sweep->original_expression=="90 deg", "start tangent and sweep expressions survive");
    }
}

void test_typed_anchor_mixed_units_and_preferences() {
    MainWindow window; auto& canvas=prepare(window,false); const auto initial=window.document().snapshot();
    require(window.beginMeasurementLinework(), "measured stroke begins before first click");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        field(dialog,"boundaryInputEndX",QStringLiteral("12 ft 6 in"));
        field(dialog,"boundaryInputEndY",QStringLiteral("-123.456789 cm")); accept(dialog);
    });
    require(window.document().snapshot().entities()==initial.entities() && window.document().revision()==initial.revision(), "typed anchor creates no empty entity or document command");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        require(dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"))->currentIndex()==0, "first anchored form defaults to absolute heading");
        field(dialog,"boundaryInputLength",QStringLiteral("1250 mm"));
        field(dialog,"boundaryInputHeading",QStringLiteral("0 deg")); accept(dialog);
    });
    auto saved=model(window.document().snapshot());
    require(saved.anchor.x==3.81 && saved.anchor.y==-1.23456789 && saved.edges.front().receipt.distance->original_expression=="1250 mm", "mixed units preserve exact typed anchor and edge expression");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        field(dialog,"boundaryInputLength",QStringLiteral("99 ft")); choose(dialog,1); dialog.reject();
    });
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        require(dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"))->currentIndex()==0 &&
            dialog.findChild<QLineEdit*>(QStringLiteral("boundaryInputLength"))->text()==QStringLiteral("1250 mm"), "cancelled input does not persist method or expressions"); dialog.reject();
    });
    window.setMetricUnits(true);
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        require(dialog.findChild<QLineEdit*>(QStringLiteral("boundaryInputLength"))->text()==QStringLiteral("1 m"), "unit basis change resets measured preferences");
        field(dialog,"boundaryInputLength",QStringLiteral("2 ft 3 in")); accept(dialog);
    });
    saved=model(window.document().snapshot());
    require(saved.edges.back().receipt.distance->original_expression=="2 ft 3 in" && std::abs(saved.edges.back().receipt.distance->metres-0.6858)<1e-12, "mixed imperial expression survives metric input basis");
}

void test_invalid_cancel_and_stale_input() {
    MainWindow window; auto& canvas=prepare(window); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "guard fixture starts");
    const auto initial=window.document().snapshot();
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        choose(dialog,2); require(!dialog.submit() && dialog.isVisible() && !dialog.lastError().isEmpty(), "relative turn without previous edge stays open with inline reason");
        require(window.document().revision()==initial.revision(), "invalid relative turn is atomic");
        choose(dialog,0); field(dialog,"boundaryInputLength",QStringLiteral("nonsense"));
        require(!dialog.submit() && dialog.isVisible() && dialog.findChild<QLabel*>(QStringLiteral("boundaryInputError"))->isVisible(), "invalid typed field stays open for correction");
        field(dialog,"boundaryInputLength",QStringLiteral("2 m")); field(dialog,"boundaryInputHeading",QStringLiteral("0 deg")); accept(dialog);
    });
    const auto first=window.document().snapshot();
    input(window,canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,4); capture(dialog,QStringLiteral("measured-precision-cancel.png")); dialog.reject(); });
    require(window.document().snapshot().entities()==first.entities() && window.document().revision()==first.revision(), "Cancel changes no committed geometry");
    input(window,canvas,[&](BoundaryInputDialog& dialog) { window.setMetricUnits(false); window.setMetricUnits(true); accept(dialog); });
    require(window.document().snapshot().entities()==first.entities() && window.document().revision()==first.revision(), "units changed away and back cannot admit obsolete dialog input");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        window.finishMeasurementLinework(); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "replace live pen session"); accept(dialog);
    });
    require(window.document().snapshot().entities()==first.entities() && window.document().revision()==first.revision(), "replacement session at same coordinate cannot accept old input");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        window.document().apply(NameRevision{window.document().revision(),"foreign precision fixture"}); accept(dialog);
    });
    require(window.document().snapshot().entities()==first.entities() && window.document().revision()==first.revision()+1, "changed document source admits no extra measured edge");
}

void test_curved_tangent_history_branch_and_retrace() {
    MainWindow window; auto& canvas=prepare(window); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "curved history fixture starts");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        choose(dialog,4); field(dialog,"boundaryInputEndX",QStringLiteral("2 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m"));
        field(dialog,"boundaryInputSweep",QStringLiteral("90 deg")); accept(dialog);
    });
    const auto arc=window.document().snapshot(); const auto arc_model=model(arc);
    input(window,canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,2); field(dialog,"boundaryInputLength",QStringLiteral("1 m")); field(dialog,"boundaryInputTurn",QStringLiteral("0 deg")); accept(dialog); });
    const auto relative=window.document().snapshot();
    require(close_point(replay_measurement_linework(model(relative)).edges.back().segment.end,{2.7071067811865475,0.7071067811865475}), "relative line uses curved end tangent instead of chord heading");
    key(canvas,Qt::Key_Z,Qt::ControlModifier); require(window.document().snapshot().entities()==arc.entities(), "keyboard Undo restores exact previous arc");
    key(canvas,Qt::Key_Y,Qt::ControlModifier); require(window.document().snapshot().entities()==relative.entities(), "keyboard Redo restores exact tangent receipt and IDs");
    key(canvas,Qt::Key_Z,Qt::ControlModifier);
    input(window,canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,2); field(dialog,"boundaryInputLength",QStringLiteral("2 m")); field(dialog,"boundaryInputTurn",QStringLiteral("90 deg")); accept(dialog); });
    const auto branch=model(window.document().snapshot());
    require(branch.edges.size()==2 && branch.edges.front()==arc_model.edges.front() && branch.edges.back().start_vertex_id==arc_model.edges.front().end_vertex_id && !window.document().can_redo(), "D branch keeps analytical prefix and pen IDs and retires redo");
    require(close_point(replay_measurement_linework(branch).edges.back().segment.end,{0.585786437626905,1.414213562373095}), "D after history still uses actual arc tangent");
    window.finishMeasurementLinework();
    MainWindow retrace; auto& retrace_canvas=prepare(retrace); require(retrace.beginMeasurementLinework() && retrace.appendMeasurementLineworkPoint({0,0}) && retrace.appendMeasurementLineworkPoint({2,0}) && retrace.appendMeasurementLineworkPoint({2,2}), "retrace fixture starts");
    input(retrace,retrace_canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,3); field(dialog,"boundaryInputEndX",QStringLiteral("2 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m")); accept(dialog); });
    require(model(retrace.document().snapshot()).edges.size()==3 && !model(retrace.document().snapshot()).closed, "exact analytical backtrack stays valid open linework");
    input(retrace,retrace_canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,3); field(dialog,"boundaryInputEndX",QStringLiteral("0 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m")); accept(dialog); });
    require(model(retrace.document().snapshot()).closed && !retrace_canvas.boundaryDraftPreview(), "precision closure follows real anchor and finishes stroke");
    MainWindow two; auto& two_canvas=prepare(two); require(two.beginMeasurementLinework() && two.appendMeasurementLineworkPoint({0,0}) && two.appendMeasurementLineworkPoint({2,0}), "two edge fixture starts");
    input(two,two_canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,3); field(dialog,"boundaryInputEndX",QStringLiteral("0 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m")); accept(dialog); });
    require(model(two.document().snapshot()).closed && model(two.document().snapshot()).edges.size()==2, "two edge retrace closure is valid measured topology");
}

void test_stale_maps_layer_phase_and_workspace() {
    for (bool asset_change : {false,true}) {
        // Start with a complete valid scaffold in a create record, so an
        // altered copied snapshot remains valid without forging command history.
        MainWindow scaffold; const auto scaffold_source=scaffold.document().snapshot();
        std::vector<Entity> entities;
        for (const auto& [id,entity] : scaffold_source.entities()) entities.push_back(entity);
        std::vector<Asset> assets;
        for (const auto& [id,asset] : scaffold_source.assets()) assets.push_back(asset);
        MainWindow window(std::make_shared<Document>(Document::create(std::move(entities),std::move(assets))));
        auto& canvas=prepare(window);
        require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "same revision source fixture starts");
        const auto source=window.document().snapshot();
        require(source.revision()==0, "valid source-map fixture has a create record only");
        input(window,canvas,[&](BoundaryInputDialog& dialog) {
            auto altered=source;
            auto& record=const_cast<std::vector<RevisionRecord>&>(altered.history()).front();
            if (!asset_change) record.entities.begin()->second.extensions["precision_fixture"]="changed source map";
            else record.assets.emplace("precision-map-asset",Asset::create("precision-map-asset","application/octet-stream",{std::byte{1}}));
            auto replacement=Document::fork(altered);
            require(replacement.revision()==source.revision() && replacement.snapshot().document_id()==source.document_id(),
                "valid changed source maps retain both document identity and revision");
            window.document()=std::move(replacement); accept(dialog);
        });
        const auto rejected=window.document().snapshot();
        require(rejected.revision()==source.revision() && rejected.document_id()==source.document_id(), "same identity and revision changed source admits no edge");
        require(asset_change ? rejected.assets()!=source.assets() : rejected.entities()!=source.entities(), "source-map test actually changes its intended authority map");
        {
            const auto stale_source = window.document().snapshot();
            for (const auto& [id,entity] : stale_source.entities()) require(entity.type!="measurement_linework", "stale source maps cannot create a stroke");
        }
    }
    MainWindow layer; auto& layer_canvas=prepare(layer); const auto original=layer.activeLayerId();
    const auto floor=QString::fromStdString(layer.document().snapshot().entities().at(original.toStdString()).properties.at("floor_id").get<std::string>());
    const auto sibling=layer.createLayer(floor,QStringLiteral("Precision alternate"));
    require(!sibling.isEmpty() && layer.setActiveLayer(original) && layer.beginMeasurementLinework() && layer.appendMeasurementLineworkPoint({0,0}), "layer guard fixture starts");
    const auto layer_before=layer.document().snapshot();
    input(layer,layer_canvas,[&](BoundaryInputDialog& dialog) { require(layer.setActiveLayer(sibling), "change precision source layer"); accept(dialog); });
    require(layer.document().snapshot().entities()==layer_before.entities(), "obsolete layer dialog creates no measured edge");
    MainWindow phase; auto& phase_canvas=prepare(phase); std::vector<std::string> registry;
    {
        const auto registry_source = phase.document().snapshot();
        for (const auto& [id,entity] : registry_source.entities()) if (entity.type=="building" || entity.type=="floor") registry.push_back(id);
    }
    const auto phases=ModelPhases::create(registry,registry,{{"precision-alternative","Precision alternative",{}, {}}});
    phase.document().apply(ApplyEntityChanges{.expected_revision=phase.document().revision(),
        .entity_changes={EntityChange::upsert(Entity::create("model_phases",{{"model",phases.to_json()}}))},.message="precision phase fixture"});
    require(phase.beginMeasurementLinework() && phase.appendMeasurementLineworkPoint({0,0}), "phase guard fixture starts");
    const auto phase_revision=phase.document().revision();
    input(phase,phase_canvas,[&](BoundaryInputDialog& dialog) { require(phase.selectRemodelingAlternative(QStringLiteral("precision-alternative")), "change semantic phase while precision dialog is open"); accept(dialog); });
    require(phase.document().revision()==phase_revision+1, "phase change is the only admitted document operation");
    {
        const auto stale_phase = phase.document().snapshot();
        for (const auto& [id,entity] : stale_phase.entities()) require(entity.type!="measurement_linework", "stale phase dialog creates no stroke");
    }
    MainWindow workspace; auto& workspace_canvas=prepare(workspace); require(workspace.beginMeasurementLinework() && workspace.appendMeasurementLineworkPoint({0,0}), "workspace guard fixture starts");
    const auto workspace_before=workspace.document().snapshot();
    input(workspace,workspace_canvas,[&](BoundaryInputDialog& dialog) { workspace.finishMeasurementLinework(); workspace.setWorkspace(Workspace::architectural); accept(dialog); });
    require(workspace.document().snapshot().entities()==workspace_before.entities(), "obsolete workspace dialog creates no measured edge");
}

void test_queued_input_and_same_document_read_only() {
    MainWindow window; auto& canvas=prepare(window);
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "queued precision fixture starts");
    const auto source=window.document().snapshot();
    bool queued_invalid=false; std::exception_ptr queued_failure;
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        field(dialog,"boundaryInputLength",QStringLiteral("invalid"));
        QTimer::singleShot(0,&dialog,[&] {
            queued_invalid=true;
            try {
                require(!dialog.submit() && dialog.isVisible() && !dialog.receipt() && !dialog.acceptedPreferences(), "queued invalid submit stays editable and publishes no result");
                field(dialog,"boundaryInputLength",QStringLiteral("2 m"));
            } catch (...) { queued_failure=std::current_exception(); }
            dialog.reject();
        });
    });
    if (queued_failure) std::rethrow_exception(queued_failure);
    require(queued_invalid && window.document().snapshot().entities()==source.entities() && window.document().revision()==source.revision(), "queued invalid input followed by Cancel is atomic");
    bool queued_accept=false;
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        window.setMetricUnits(false); window.setMetricUnits(true);
        QTimer::singleShot(0,&dialog,[&] {
            queued_accept=true;
            try { accept(dialog); }
            catch (...) { queued_failure=std::current_exception(); dialog.reject(); }
        });
    });
    if (queued_failure) std::rethrow_exception(queued_failure);
    require(queued_accept && window.document().snapshot().entities()==source.entities() && window.document().revision()==source.revision(), "queued valid receipt cannot bypass stale unit generation");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        window.document().mark_read_only("precision read-only fixture"); accept(dialog);
    });
    const auto read_only=window.document().snapshot();
    require(!window.document().is_editable() && read_only.document_id()==source.document_id() && read_only.revision()==source.revision() &&
        read_only.entities()==source.entities() && read_only.assets()==source.assets(), "same document made read-only admits no precise geometry or document command");
}

void test_saved_arc_area_and_export() {
    MainWindow window; auto& canvas=prepare(window); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}), "area arc fixture starts");
    input(window,canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,4); field(dialog,"boundaryInputEndX",QStringLiteral("2 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m")); field(dialog,"boundaryInputSweep",QStringLiteral("180 deg")); accept(dialog); });
    input(window,canvas,[&](BoundaryInputDialog& dialog) { choose(dialog,3); field(dialog,"boundaryInputEndX",QStringLiteral("0 m")); field(dialog,"boundaryInputEndY",QStringLiteral("0 m")); accept(dialog); });
    const auto source=window.document().snapshot(); const auto saved=model(source);
    require(saved.closed && std::abs(replay_measurement_linework(saved).edges.front().segment.sweep_radians-std::numbers::pi)<1e-12, "closed model retains semicircular edge");
    require(window.selectEntity(QString::fromStdString(saved.stroke_id)), "select curved measured outline");
    const auto areas=window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living"),window.document().revision());
    require(areas.size()==1, "native area definition detects true curved measured face");
    const auto defined=window.document().snapshot();
    require(std::abs(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(defined.entities().at(areas.front().toStdString())))))-std::numbers::pi/2)<1e-9, "derived area uses analytical arc instead of chord approximation");
    require(defined.entities().at(saved.stroke_id)==source.entities().at(saved.stroke_id), "area definition leaves original receipts intact");
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("precision.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path) && window.document().snapshot().entities()==defined.entities(), "save and reopen preserves exact receipt model and analytical area");
    const auto pdf=directory.filePath(QStringLiteral("precision.pdf"));
    require(window.exportDraftPdf(pdf) && QFileInfo(pdf).size()>0, "saved measured arc and area export through native PDF path");
    capture(window,QStringLiteral("measured-precision-saved-curved-area.png"));
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-linework-precision-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled Inter font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        test_live_stroke_offers_analytical_methods();
        test_all_methods_in_both_units(); test_typed_anchor_mixed_units_and_preferences();
        test_invalid_cancel_and_stale_input(); test_curved_tangent_history_branch_and_retrace();
        test_stale_maps_layer_phase_and_workspace();
        test_queued_input_and_same_document_read_only();
        test_saved_arc_area_and_export();
    } catch (const std::exception& error) {
        std::cerr << "measurement_linework_precision_desktop_tests: " << error.what() << '\n'; return 1;
    }
    std::cout << "Measurement linework precision desktop tests passed\n"; return 0;
}
