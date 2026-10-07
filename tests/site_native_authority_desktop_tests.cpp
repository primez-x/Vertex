#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/site_placement_dialog.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using View = sketch::visualization::NativeModelView;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void expect_near(double actual, double expected) { require(std::abs(actual-expected)<1.e-6,"independent numeric expectation"); }
template<class T> T& control(QObject& owner, const char* name) {
    for (auto* child : owner.findChildren<QObject*>(QString::fromLatin1(name)))
        if (auto* result=dynamic_cast<T*>(child)) return *result;
    throw std::runtime_error(name);
}
void mode(QDialog& dialog, const char* value) {
    auto& choice=control<QComboBox>(dialog,"sitePlacementMode");
    const auto index=choice.findData(QString::fromLatin1(value));require(index>=0,"typed site mode");choice.setCurrentIndex(index);
}
void text(QDialog& dialog, const char* name, const char* value) {control<QLineEdit>(dialog,name).setText(QString::fromLatin1(value));}
void save(QDialog& dialog) {control<QDialogButtonBox>(dialog,"sitePlacementButtons").button(QDialogButtonBox::Save)->click();}
std::shared_ptr<Document> fixture() {
    auto column=encode_building_entity(RectangularColumn{"post",{1,2,4},.6,1.1,3,.2});
    column.properties.update({{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"name","Survey post"}});
    column.extensions["observed_measurements"]={{"width_expression","23 5/8 in"},{"grade","above_grade"}};
    TerrainSurface terrain("Authored independent survey",{{"a",0,0,154},{"b",2,0,155},{"c",0,2,154}},{{{0,1,2}}});
    AnnotationState notes;LabelInstance label;label.id="note";label.content="Survey observation";label.placement.layer_id="l";notes.labels.push_back(label);
    auto annotations=make_annotation_entity("notes",notes,AnnotationEntityContext{"p","b","f","l",std::nullopt});
    auto result=std::make_shared<Document>(Document::create({
        {"p","property",{{"name","Surveyed parcel"},{"site_frame",{{"version",1},{"origin_m",{103.,209.,19.}},
            {"rotation_radians",.4},{"vertical_datum",{{"identifier","survey-A"},{"height_at_origin_m",150.}}}}}}},
        {"b","building",{{"name","Main building"},{"property_id","p"},{"site_placement",{{"version",1},{"translation_m",{7.,-3.,2.}}, {"rotation_radians",.7}}}}},
        {"f","floor",{{"building_id","b"}}}, {"l","layer",{{"floor_id","f"}}}, column,
        {"terrain","terrain_surface",{{"property_id","p"},{"name","Survey heights"},{"model",terrain.to_json()}}},annotations}));
    auto property=result->snapshot().entities().at("p");property.extensions["fixture_checkpoint"]=true;
    result->apply(ApplyEntityChanges{result->revision(),{EntityChange::upsert(property)},{},"Authored fixture checkpoint"});
    property.extensions["fixture_checkpoint"]=2;
    result->apply(ApplyEntityChanges{result->revision(),{EntityChange::upsert(property)},{},"Second authored fixture checkpoint"});
    return result;
}
void openPlacement(MainWindow& window,const std::function<void(QDialog&)>& edit) {
    std::exception_ptr failure;bool observed=false;QTimer timer;timer.setSingleShot(true);
    QObject::connect(&timer,&QTimer::timeout,[&] {
        auto* dialog=dynamic_cast<QDialog*>(QApplication::activeModalWidget());
        try {require(dialog && dialog->objectName()=="sitePlacementDialog","real site modal action");observed=true;edit(*dialog);}
        catch(...) {failure=std::current_exception();if(dialog)dialog->reject();}
    });
    timer.start(0);control<QAction>(window,"sitePlacement").trigger();timer.stop();
    if(failure)std::rethrow_exception(failure);require(observed,"discoverable action opened actual dialog");
}
DocumentSnapshot replaceSameHead(MainWindow& window, bool history_only=false) {
    const auto captured=window.document().snapshot();const auto digest=document_snapshot_digest(captured);
    require(captured.revision()>=2,"replacement fixture has two retained revisions");
    auto replacement=Document::fork_at_revision(captured,captured.revision()-2);
    auto intermediate=captured.entities().at("b");
    static std::uint64_t replacement_generation=0;
    intermediate.extensions["independent_replacement_history"]=++replacement_generation;
    replacement.apply(ApplyEntityChanges{replacement.revision(),{EntityChange::upsert(intermediate)},{},"Independent intermediate replacement history"});
    std::vector<EntityChange> changes;
    for(const auto& [id,original]:captured.entities()) {
        auto next=original;if(id=="b" && !history_only)next.extensions["independent_concurrent_work"]=true;
        changes.push_back(EntityChange::upsert(next));
    }
    replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independent replaced head history"});
    require(replacement.revision()==captured.revision() && replacement.snapshot().document_id()==captured.document_id(),"same ID and revision replacement");
    require(document_snapshot_digest(captured)==digest,"independent fork leaves original full source intact");
    if(history_only)require(replacement.snapshot().entities()==captured.entities(),"history-only replacement has equal head entities");
    require(document_snapshot_digest(replacement.snapshot())!=digest,"replacement differs in full retained source");
    window.document()=std::move(replacement);return window.document().snapshot();
}
void modalLifecycle(QTemporaryDir& files) {
    MainWindow window(fixture(),nullptr,files.filePath("site-modal-text.json"));window.setMetricUnits(true);
    require(window.selectEntity("b"),"select building from actual source");
    const auto before=window.document().snapshot();const auto digest=document_snapshot_digest(before);
    openPlacement(window,[](QDialog& dialog){text(dialog,"sitePlacementX","11 m");dialog.reject();});
    require(document_snapshot_digest(window.document().snapshot())==digest,"Cancel preserves complete source and history");
    openPlacement(window,[](QDialog& dialog){require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"review unchanged placement");save(dialog);});
    require(document_snapshot_digest(window.document().snapshot())==digest,"untouched Save creates no history");
    openPlacement(window,[](QDialog& dialog){text(dialog,"sitePlacementX","11 m");require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"review actual preview");save(dialog);});
    const auto edited=window.document().snapshot();require(edited.revision()==before.revision()+1,"one reviewed atomic revision");
    expect_near(decode_building_site_placement(edited.entities().at("b").properties.at("site_placement")).translation_m.x,11);
    require(edited.entities().at("post")==before.entities().at("post"),"site placement preserves authored geometry, local measurements and grade");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"one Undo restores placement and source");
    require(window.redoCommand() && window.document().snapshot().entities()==edited.entities(),"one Redo restores reviewed placement");
    require(window.selectEntity("p"),"select actual property");
    const auto property_before=window.document().snapshot();
    openPlacement(window,[](QDialog& dialog){text(dialog,"sitePlacementY","219 m");require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"property impact review");save(dialog);});
    require(window.document().revision()==property_before.revision()+1 && window.document().snapshot().entities().at("post")==property_before.entities().at("post"),"property placement changes one owner without local measurement edits");
    require(window.selectEntity("terrain"),"select actual terrain source");
    const auto terrain_before=window.document().snapshot();
    openPlacement(window,[](QDialog& dialog){mode(dialog,"declared_absolute");text(dialog,"sitePlacementDatumIdentifier","survey-A");require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"terrain exact datum review");save(dialog);});
    require(window.document().revision()==terrain_before.revision()+1 && window.document().snapshot().entities().at("terrain").properties.at("model")==terrain_before.entities().at("terrain").properties.at("model"),"terrain enrollment retains authored elevations and triangles");
    require(window.selectEntity("notes"),"select actual explicit annotation owner");
    const auto notes_before=window.document().snapshot();
    openPlacement(window,[](QDialog& dialog){mode(dialog,"building");require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"own-layer explicit object frame review");save(dialog);});
    require(window.document().revision()==notes_before.revision()+1 && decode_annotation_entity(window.document().snapshot().entities().at("notes")).labels.front().placement.layer_id=="l","explicit frame preserves own-layer annotation source");
    for(const bool history_only:{false,true}) {
        require(window.selectEntity("b"),"stale fixture selection");DocumentSnapshot replacement=window.document().snapshot();
        std::exception_ptr refusal_failure;
        openPlacement(window,[&](QDialog& dialog){
            text(dialog,"sitePlacementX","81 m");require(dynamic_cast<SitePlacementDialog*>(&dialog)->previewUpdate(),"review before replacement");
            replacement=replaceSameHead(window,history_only);save(dialog);
            // Controller redisplays the same retained draft after rejection.
            QTimer::singleShot(0,&dialog,[&dialog,&refusal_failure]{
                try {
                    require(control<QLineEdit>(dialog,"sitePlacementX").text()=="81 m","refused draft remains visible");
                    require(!control<QDialogButtonBox>(dialog,"sitePlacementButtons").button(QDialogButtonBox::Save)->isEnabled(),"stale Save disabled");
                    require(!control<QLabel>(dialog,"sitePlacementError").text().isEmpty(),"explicit stale refusal");
                } catch(...) {refusal_failure=std::current_exception();}
                dialog.reject();
            });
        });
        if(refusal_failure)std::rethrow_exception(refusal_failure);
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement),"stale same-head Save preserves replacement geometry, assets and retained history");
    }
}
void mouse(View& view,QEvent::Type kind,QPointF p,Qt::MouseButton button,Qt::MouseButtons buttons) {
    QMouseEvent event(kind,p,view.mapToGlobal(p.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(&view,&event);
}
void settle(View& view) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    do {QApplication::processEvents();view.pollGeometryPreparation();QThread::msleep(1);}
    while(view.isGeometryPending() && std::chrono::steady_clock::now()<deadline);
    require(view.isReady(),"real native scene published");
}
QPointF handle(View& view,View::TransformControl mode) {
    for(int y=2;y<view.height();y+=3)for(int x=2;x<view.width();x+=3)
        if(view.transformControlAt({static_cast<double>(x),static_cast<double>(y)})==mode)return {static_cast<double>(x),static_cast<double>(y)};
    throw std::runtime_error("actual OCCT handle pick");
}
bool changed(const std::array<double,12>& m) {
    constexpr std::array<double,12> identity{1,0,0,0,0,1,0,0,0,0,1,0};
    for(std::size_t i=0;i<m.size();++i)if(std::abs(m[i]-identity[i])>1.e-8)return true;return false;
}
void nativeLifecycle(QTemporaryDir& files) {
    require(QApplication::platformName()!="offscreen" && QApplication::platformName()!="minimal","native scenario needs actual Windows surface");
    MainWindow window(fixture(),nullptr,files.filePath("site-native-text.json"));window.setAttribute(Qt::WA_DontShowOnScreen);window.setAttribute(Qt::WA_ShowWithoutActivating);window.resize(1200,800);window.setWorkspace(Workspace::architectural);
    View* view=nullptr;for(auto* child:window.findChildren<QWidget*>())if(auto* value=dynamic_cast<View*>(child))view=value;
    require(view,"actual MainWindow native view");view->setAttribute(Qt::WA_DontShowOnScreen);view->setAttribute(Qt::WA_ShowWithoutActivating);window.show();settle(*view);require(window.selectEntity("post"),"actual native selection");settle(*view);view->fitAll();
    const auto original=window.document().snapshot();
    const auto frame=resolve_site_presentation(original,"post");
    // Independent composition includes asymmetric XY, nonzero Z, and two yaws.
    expect_near(frame.forward.rotation_radians,1.1);expect_near(frame.forward.translation_m.x,103+7*std::cos(.4)+3*std::sin(.4));
    expect_near(frame.forward.translation_m.y,209+7*std::sin(.4)-3*std::cos(.4));expect_near(frame.forward.translation_m.z,21);
    const Vec3 probe{1,2,4};const auto world=site_transform_point(probe,frame.forward);const auto roundtrip=site_transform_point(world,frame.inverse);
    expect_near(roundtrip.x,1);expect_near(roundtrip.y,2);expect_near(roundtrip.z,4);
    require(view->publishedSnapshot() && document_snapshot_digest(*view->publishedSnapshot())==document_snapshot_digest(original),"published capture binds actual full source");
    for(const int mode_index:{0,1,2}) {
        require(window.selectEntity("post"),"native lifecycle sole selection");settle(*view);
        const auto before=window.document().snapshot();const auto digest=document_snapshot_digest(before);
        const auto source=std::get<RectangularColumn>(decode_building_entity(before.entities().at("post")));
        const auto placement=resolve_site_presentation(before,"post");
        QPointF start=view->rect().center();if(mode_index==0)require(view->beginMove("post"),"actual Move armed");
        else start=handle(*view,mode_index==1?View::TransformControl::rotation:View::TransformControl::scale);
        QPointF end;std::array<double,12> preview{};bool previewed=false;
        for(const auto delta:{QPointF(28,18),QPointF(-24,22),QPointF(20,-25)}) {
            mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);end=start+delta;
            require(view->gestureSourceSnapshot() && document_snapshot_digest(*view->gestureSourceSnapshot())==digest,"actual press retains full source");
            mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);preview=*view->nativePresentationTransform("post");
            if(changed(preview)){previewed=true;break;}view->cancelInteraction();if(mode_index==0)require(view->beginMove("post"),"rearm actual Move");
        }
        require(previewed && document_snapshot_digest(window.document().snapshot())==digest,"real preview preserves source");
        const auto p=site_transform_point(source.base_center,placement.forward);
        const Vec3 expected_world{preview[0]*p.x+preview[1]*p.y+preview[2]*p.z+preview[3],preview[4]*p.x+preview[5]*p.y+preview[6]*p.z+preview[7],preview[8]*p.x+preview[9]*p.y+preview[10]*p.z+preview[11]};
        const auto dx=expected_world.x-placement.forward.translation_m.x,dy=expected_world.y-placement.forward.translation_m.y;
        const auto yaw=placement.forward.rotation_radians;
        const Vec3 expected_local{dx*std::cos(yaw)+dy*std::sin(yaw),-dx*std::sin(yaw)+dy*std::cos(yaw),expected_world.z-placement.forward.translation_m.z};
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);settle(*view);
        const auto after=window.document().snapshot();require(after.revision()==before.revision()+1,"one source-bound native commit");
        const auto edited=std::get<RectangularColumn>(decode_building_entity(after.entities().at("post")));
        expect_near(edited.base_center.x,expected_local.x);expect_near(edited.base_center.y,expected_local.y);expect_near(edited.base_center.z,expected_local.z);
        const auto scale=std::sqrt(preview[0]*preview[0]+preview[4]*preview[4]+preview[8]*preview[8]);expect_near(edited.width,source.width*scale);expect_near(edited.depth,source.depth*scale);expect_near(edited.height,source.height*scale);
        require(after.entities().at("p")==before.entities().at("p") && after.entities().at("b")==before.entities().at("b"),"native edit retains site origins and datums");
        require(after.entities().at("post").extensions==before.entities().at("post").extensions,"native edit retains measurement observations and grade");
        require(!view->gestureSourceSnapshot(),"release clears gesture capture");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"native Undo exact source");settle(*view);
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities(),"native Redo exact source");settle(*view);
    }
    require(window.selectEntity("post") && view->beginMove("post"),"Cancel gesture fixture");
    auto digest=document_snapshot_digest(window.document().snapshot());const QPointF start=view->rect().center(),end=start+QPointF(31,23);
    mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);view->cancelInteraction();
    mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);require(!view->gestureSourceSnapshot() && document_snapshot_digest(window.document().snapshot())==digest,"native Cancel preserves complete source");
    for(const bool history_only:{false,true}) {
        require(window.selectEntity("post"),"stale actual native selection");settle(*view);require(view->beginMove("post"),"stale Move armed");
        mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        const auto replacement=replaceSameHead(window,history_only);mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement) && !window.lastError().isEmpty(),"actual stale native callback refuses without mutation");
    }
    // A replacement made before PRESS must be refused before native picking
    // or preview can operate on the old published solids.
    for (const bool history_only : {false, true}) {
        require(window.selectEntity("post"),"before-press stale selection");settle(*view);
        require(view->beginMove("post"),"before-press stale Move armed");
        const auto replacement=replaceSameHead(window,history_only);
        mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        require(!view->gestureSourceSnapshot() && !view->isMoveActive(),
            "stale displayed native source is rejected at PRESS before gesture capture");
        mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement) &&
            !window.lastError().isEmpty(),"before-press refusal preserves complete replacement source");
        require(window.selectEntity({}),"publish unselected current native scene");settle(*view);
        const auto selection_replacement=replaceSameHead(window,history_only);
        mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        mouse(*view,QEvent::MouseButtonRelease,start,Qt::LeftButton,Qt::NoButton);
        require(window.selectedEntityId().isEmpty() &&
            document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(selection_replacement),
            "stale native hit cannot select then refresh new authority");
    }
    // Observer failures stay inside the native input boundary. They must not
    // leave a captured source or a partial transform armed for the next press.
    const auto started = view->onTransformGestureStarted;
    const auto admission = view->onSceneInputRequested;
    for (const bool capture_failure : {true, false}) {
      for (const bool standard_exception : {true, false}) {
        require(window.selectEntity("post"),"throwing observer selection");settle(*view);
        require(view->beginMove("post"),"throwing observer Move armed");
        const auto before_observer = window.document().snapshot();
        bool observer_failed = false;
        const auto throw_observer = [standard_exception, &observer_failed] {
            observer_failed = true;
            if (standard_exception) throw std::runtime_error("fixture gesture observer");
            throw 17;
        };
        if (capture_failure)
            view->onTransformGestureStarted = [throw_observer](QString) { throw_observer(); };
        else
            view->onSceneInputRequested = [throw_observer](bool) -> bool { throw_observer(); return true; };
        mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        require(observer_failed,"actual native input reaches the throwing observer");
        require(!view->gestureSourceSnapshot() && !view->isMoveActive() &&
            document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(before_observer),
            "throwing observer cancels authority without committing source or history");
        const auto native_transform = view->nativePresentationTransform("post");
        require(native_transform && !changed(*native_transform),
            "throwing observer restores published geometry");
        require(view->isReady() && view->transformControlsVisible(),
            "throwing observer keeps native view and controls usable");
        require(!view->lastError().isEmpty() && !window.lastError().isEmpty(),
            "recovered observer failure remains visible to native view and shell");
        view->onTransformGestureStarted = started;
        view->onSceneInputRequested = admission;
        require(view->beginMove("post"),"recovered native view accepts actual Move retry");
        mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        require(view->gestureSourceSnapshot() &&
            document_snapshot_digest(*view->gestureSourceSnapshot()) == document_snapshot_digest(before_observer),
            "recovered native press captures the complete current source");
        require(view->lastError().isEmpty(),"admitted native retry clears recovered input diagnostic");
        mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        const auto retry_transform = view->nativePresentationTransform("post");
        require(retry_transform && changed(*retry_transform),"recovered native Move previews actual geometry");
        view->cancelInteraction();
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        const auto cancelled_retry = view->nativePresentationTransform("post");
        require(cancelled_retry && !changed(*cancelled_retry) && !view->gestureSourceSnapshot() &&
            !view->isMoveActive() && document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(before_observer),
            "recovered Move cancellation restores geometry and preserves full source/history");
      }
    }
    // New requested source must never masquerade as the old displayed scene.
    require(window.selectEntity("post"),"refresh actual current native source");settle(*view);const auto published=view->publishedSnapshot();
    auto changed_source=Document::fork(window.document().snapshot());auto property=changed_source.snapshot().entities().at("p");property.extensions["queued_source"]=true;
    changed_source.apply(ApplyEntityChanges{changed_source.revision(),{EntityChange::upsert(property)},{},"New requested geometry source"});
    view->setSnapshot(changed_source.snapshot());require(view->publishedSnapshot()==published && !view->gestureSourceSnapshot(),"queued snapshot retains old publication and cancels gesture authority");
    // Publish the actual read-only Document; selection and copy remain reads.
    const auto editable_capture=window.document().snapshot();
    const auto editable_digest=document_snapshot_digest(editable_capture);
    window.document().mark_read_only("Native inspection regression");
    require(window.selectEntity({}),"refresh read-only native publication");settle(*view);
    require(view->publishedSnapshot() && !view->publishedSnapshot()->is_editable(),"native scene binds read-only snapshot");
    require(window.selectEntity("post"),"read-only native source remains selectable");settle(*view);
    const auto read_digest=document_snapshot_digest(window.document().snapshot());
    require(window.copySelection(),"read-only native selection copies");
    require(!view->transformControlsVisible() && !view->beginMove("post"),"read-only native scene exposes no transform or Move");
    require(document_snapshot_digest(window.document().snapshot())==read_digest &&
        document_snapshot_digest(editable_capture)==editable_digest,"native inspection preserves current and captured editable source/history");

}
void nativeStatusObserverLifetime() {
    enum class Boundary { request, publication, showing, geometry_status_error, geometry_error };
    for (const auto boundary : {Boundary::request,Boundary::publication,Boundary::showing,
                               Boundary::geometry_status_error,Boundary::geometry_error}) {
        const auto document=fixture();const auto source=document->snapshot();
        const auto digest=document_snapshot_digest(source);
        auto owned=std::make_unique<View>();QPointer<View> view(owned.get());
        view->setAttribute(Qt::WA_DontShowOnScreen);view->setAttribute(Qt::WA_ShowWithoutActivating);
        view->resize(800,600);
        if(boundary==Boundary::publication) {
            // Initialize the real native surface before requesting geometry.
            view->show();require(view->nativeRenderSizePixels().has_value(),
                "status lifetime fixture initializes an actual native surface");
        }
        bool called=false;bool published=false;QString diagnostic;int errors=0;
        const auto destroy=[&](const QString& message) {
            called=true;diagnostic=message;
            const auto capture=view->publishedSnapshot();
            published=capture && document_snapshot_digest(*capture)==digest;
            owned.reset();
        };
        const auto observe_status=[&](const QString& message) {
            const bool request=boundary==Boundary::request && !message.isEmpty();
            const bool completed=(boundary==Boundary::publication || boundary==Boundary::showing) && message.isEmpty();
            const bool failed=boundary==Boundary::geometry_status_error &&
                message.startsWith(QStringLiteral("3D geometry is incomplete:"));
            if(request || completed || failed)destroy(message);
        };
        view->setErrorCallback([&](const QString& message) {
            ++errors;if(boundary==Boundary::geometry_error)destroy(message);
        });
        if(boundary!=Boundary::showing)view->setGeometryStatusChangedCallback(observe_status);
        const bool invalid=boundary==Boundary::geometry_status_error || boundary==Boundary::geometry_error;
        const auto requested=invalid ? Document::create({Entity::create("wall", {
            {"baseline", {{"start", {0.0,0.0}}, {"end", {4.0,0.0}}, {"sweep_radians",0.0}}},
            {"thickness_m",0.2}, {"height_m",-1.0}, {"elevation_m",0.0}})}).snapshot() : source;
        const auto requested_digest=document_snapshot_digest(requested);
        if(invalid)view->setSnapshot(requested);
        else view->setSnapshot(requested,View::VisibleEntityIds{"post"});
        if(boundary!=Boundary::request) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            // Poll the real worker through the public API without delivering
            // timer/show events that could obscure which continuation is tested.
            while(view && !called && std::chrono::steady_clock::now()<deadline) {
                view->pollGeometryPreparation();
                if(boundary==Boundary::showing && view && view->isGeometryPrepared())break;
                QThread::msleep(1);
            }
            if(boundary==Boundary::showing) {
                require(view && view->isGeometryPrepared() && !view->publishedSnapshot(),
                    "hidden real preparation retains a candidate for native initialization");
                view->setGeometryStatusChangedCallback(observe_status);
                // A real show event enters the widget's initialize/collect path
                // without making Qt's setVisible caller retain the deleted owner.
                QShowEvent event;QApplication::sendEvent(view.data(),&event);
            }
        }
        require(called && !view,"public native status/error observer may synchronously dispose its owner");
        if(boundary==Boundary::request)
            require(!diagnostic.isEmpty() && !published && errors==0,
                "preparation status disposal precedes publication and error dispatch");
        if(boundary==Boundary::publication || boundary==Boundary::showing)
            require(diagnostic.isEmpty() && published && errors==0,
                "completion status receives the actual published native source before disposal");
        if(invalid)
            require(diagnostic.startsWith(QStringLiteral("3D geometry is incomplete:")) && !published &&
                errors==(boundary==Boundary::geometry_error ? 1 : 0),
                "failed geometry preserves its diagnostic and stops error dispatch after status disposal");
        require(document_snapshot_digest(document->snapshot())==digest && document_snapshot_digest(source)==digest &&
                document_snapshot_digest(requested)==requested_digest,
            "status/error disposal preserves complete requested and authoritative source/history");
    }
}
void nativeReleaseObserverLifetime() {
    // A real single-visible-solid scene makes the centre a verified semantic
    // pick. Each destructive callback gets its own actual native owner.
    enum class Boundary { admission, selection, edit, move, rotation, cancellation };
    for (const auto boundary : {Boundary::admission,Boundary::selection,Boundary::edit,
                               Boundary::move,Boundary::rotation,Boundary::cancellation}) {
        const auto document=fixture();const auto source=document->snapshot();
        const auto digest=document_snapshot_digest(source);
        auto owned=std::make_unique<View>();QPointer<View> view(owned.get());
        view->setAttribute(Qt::WA_DontShowOnScreen);view->setAttribute(Qt::WA_ShowWithoutActivating);
        view->resize(800,600);view->setSnapshot(source,View::VisibleEntityIds{"post"});
        view->show();settle(*view);view->fitAll();
        require(view->publishedSnapshot() && document_snapshot_digest(*view->publishedSnapshot())==digest,
            "release lifetime fixture publishes the complete source");
        const QPointF centre=view->rect().center();QString selected;
        view->onEntitySelected=[&](QString id){selected=std::move(id);};
        mouse(*view,QEvent::MouseButtonPress,centre,Qt::LeftButton,Qt::LeftButton);
        mouse(*view,QEvent::MouseButtonRelease,centre,Qt::LeftButton,Qt::NoButton);
        require(selected=="post","release lifetime fixture uses an actual native semantic pick");
        view->onEntitySelected={};view->setSelectedEntity({});
        bool called=false;bool captured=false;int edit_calls=0;
        const auto destroy=[&] {called=true;owned.reset();};
        if(boundary==Boundary::admission)view->onSceneInputRequested=[&](bool starting){if(!starting)destroy();return true;};
        if(boundary==Boundary::selection) {
            view->onEntitySelected=[&](QString id){require(id=="post","deleting selection receives semantic ID");destroy();};
            view->onEntityEditRequested=[&](QString){++edit_calls;};
        }
        if(boundary==Boundary::edit)view->onEntityEditRequested=[&](QString id){require(id=="post","deleting Edit receives semantic ID");destroy();};
        QPointF start=centre,end=centre;
        if(boundary>=Boundary::move) {
            view->setSelectedEntity("post");
            if(boundary==Boundary::rotation)start=handle(*view,View::TransformControl::rotation);
            else require(view->beginMove("post"),"release lifetime actual Move armed");
            end=start+QPointF(31,23);
            view->onEntityTranslationRequested=[&](QString id,double,double,double) {
                require(id=="post","release lifetime Move uses semantic ID");
                const auto capture=view->gestureSourceSnapshot();
                captured=capture && document_snapshot_digest(*capture)==digest;
                if(boundary==Boundary::cancellation) {
                    called=true;view->cancelInteraction();
                    require(view->gestureSourceSnapshot()==capture,
                        "reentrant Cancel preserves release authority during the callback");
                } else destroy();
            };
            view->onEntityTransformRequested=[&](QString id,double,double,double,double,double) {
                require(id=="post","release lifetime manipulator uses semantic ID");
                const auto capture=view->gestureSourceSnapshot();
                captured=capture && document_snapshot_digest(*capture)==digest;destroy();
            };
        }
        const auto press=(boundary==Boundary::selection || boundary==Boundary::edit)?QEvent::MouseButtonDblClick:QEvent::MouseButtonPress;
        mouse(*view,press,start,Qt::LeftButton,Qt::LeftButton);
        if(boundary>=Boundary::move) {
            mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
            const auto preview=view->nativePresentationTransform("post");
            require(preview && changed(*preview),"release lifetime callback follows an actual changed native preview");
        }
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        require(called,"actual native release reaches the lifetime callback");
        require(document_snapshot_digest(document->snapshot())==digest && document_snapshot_digest(source)==digest,
            "native release observer preserves authoritative source and captured history");
        if(boundary==Boundary::cancellation) {
            const auto restored=view->nativePresentationTransform("post");
            require(captured && view->isReady() && view->transformControlsVisible() &&
                !view->gestureSourceSnapshot() && !view->isMoveActive() && restored && !changed(*restored),
                "reentrant release Cancel restores geometry and resets commit authority");
        } else {
            require(!view,"release observer may synchronously destroy the native owner");
            if(boundary>=Boundary::move)require(captured,"deleting commit observer receives exact press authority");
            require(edit_calls==0,"deleting selection cannot dispatch Edit on a destroyed owner");
        }
    }
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);QTemporaryDir files;
    try {
        require(files.isValid(),"isolated test data");require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"real bundled font fixture");
        if(argc>1 && std::string_view(argv[1])=="native") {nativeLifecycle(files);nativeReleaseObserverLifetime();nativeStatusObserverLifetime();}else modalLifecycle(files);
        std::cout<<"Site authority lifecycle passed\n";return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
