#include "sketch/document.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/architecture.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/visualization/framebuffer_image.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPushButton>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QThread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void mouse(sketch::visualization::NativeModelView&,QEvent::Type,QPointF,
           Qt::MouseButton,Qt::MouseButtons,Qt::KeyboardModifiers);
void check_framebuffer_conversion() {
    for (const auto format : {Image_Format_RGB, Image_Format_BGR, Image_Format_RGB32,
                             Image_Format_BGR32, Image_Format_RGBA, Image_Format_BGRA}) {
        for (const bool top_down : {false, true}) {
            Image_PixMap pixels;
            check(pixels.InitZero(format, 2, 2, 16), "Pixel fixture must allocate padded rows");
            pixels.SetTopDown(top_down);
            pixels.SetPixelColor(0, 0, Quantity_Color(1, 0, 0, Quantity_TOC_RGB));
            pixels.SetPixelColor(1, 0, Quantity_Color(0, 1, 0, Quantity_TOC_RGB));
            pixels.SetPixelColor(0, 1, Quantity_Color(0, 0, 1, Quantity_TOC_RGB));
            pixels.SetPixelColor(1, 1, Quantity_Color(1, 1, 1, Quantity_TOC_RGB));
            const auto image = sketch::visualization::detail::framebufferImage(pixels);
            check(image.size() == QSize(2, 2) && image.pixelColor(0, 0) == QColor(Qt::red) &&
                  image.pixelColor(1, 0) == QColor(Qt::green) && image.pixelColor(0, 1) == QColor(Qt::blue) &&
                  image.pixelColor(1, 1) == QColor(Qt::white),
                  "Framebuffer copy must preserve color channels, logical row order and padding");
        }
    }
    Image_PixMap empty;
    check(sketch::visualization::detail::framebufferImage(empty).isNull(), "Empty framebuffer must be refused");
    Image_PixMap depth;
    check(depth.InitZero(Image_Format_GrayF, 2, 2) &&
          sketch::visualization::detail::framebufferImage(depth).isNull(), "Depth buffers cannot be exported as RGB");
}

void check_export_paths(sketch::visualization::NativeModelView& view, QTemporaryDir& temporary,
                        const QString& wall_id) {
    check_framebuffer_conversion();
    const auto reference = temporary.filePath("short-native.png");
    check(view.exportViewImage(reference), "Short-path native export must succeed");
    const QImage expected(reference);
    const auto directory = temporary.filePath(QString(110, QLatin1Char('x')) + QLatin1Char('/') +
                                               QString(110, QLatin1Char('y')) + QStringLiteral("/\u00e9tage"));
    check(QDir().mkpath(directory), "Long Unicode export fixture must create its own directory");
    const auto long_path = directory + QStringLiteral("/native-framebuffer.png");
    check(long_path.size() > 260 && view.exportViewImage(long_path) && QImage(long_path) == expected,
          "Long Unicode paths must preserve the exact native framebuffer pixels");
    const auto blocked = temporary.filePath("preserved.unsupported-vertex-image");
    QFile original(blocked);
    check(original.open(QIODevice::WriteOnly) && original.write("keep original") == 13,
          "Failed-output fixture must create its own destination");
    original.close();
    view.setSelectedEntity(wall_id);
    check(view.transformControlsVisible(), "Failed export fixture must begin with selected-object controls");
    check(!view.exportViewImage(blocked), "Unknown image encodings must fail explicitly");
    check(original.open(QIODevice::ReadOnly) && original.readAll() == QByteArray("keep original"),
          "Failed encoding must preserve existing destination bytes");
    const auto export_error = view.lastError();
    const auto original_transform = view.nativePresentationTransform(wall_id);
    check(!export_error.isEmpty() && view.isReady() && view.beginMove(wall_id),
          "A recoverable export failure must retain its diagnostic and permit Move before any successful retry");
    const QPointF start(300,230), end(325,250);
    mouse(view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    check(original_transform && view.nativePresentationTransform(wall_id) != original_transform,
          "Move after failed export must produce an actual native presentation preview");
    view.cancelInteraction();
    mouse(view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    check(view.lastError() == export_error && view.isReady() && !view.isMoveActive() &&
          view.transformControlsVisible() && view.nativePresentationTransform(wall_id) == original_transform,
          "Cancelling a real Move after failed export must restore controls without losing the export diagnostic");
    check(view.transformControlsVisible() && view.exportViewImage(temporary.filePath("after-failure.png")) &&
          view.transformControlsVisible() && QImage(temporary.filePath("after-failure.png")) == expected,
          "Export failure must restore editing controls and leave the framebuffer free of selection overlays");
    view.setSelectedEntity({});
}
void settle_geometry(sketch::visualization::NativeModelView& view) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (view.isGeometryPending() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    check(!view.isGeometryPending(), "Native geometry worker must finish within the test deadline");
}
bool export_settled(sketch::visualization::NativeModelView& view, const QString& path) {
    settle_geometry(view);
    return view.exportViewImage(path);
}
bool ready_settled(sketch::visualization::NativeModelView& view) {
    settle_geometry(view);
    return view.isReady();
}
bool parse_positive_finite(const QString& text, double& value) {
    bool ok = false;
    value = text.toDouble(&ok);
    return ok && std::isfinite(value) && value > 0.0;
}
struct Frame { QImage image; QRect bounds; QPointF centre; };
Frame capture(sketch::visualization::NativeModelView& view, const QString& path) {
    const auto began = std::chrono::steady_clock::now();
    std::cerr << "Native capture started: " << path.section('/', -1).toStdString() << std::endl;
    check(export_settled(view, path), "Native framebuffer export failed");
    const auto artifact_directory = qEnvironmentVariable("SKETCH_TEST_ARTIFACT_DIR");
    if (!artifact_directory.isEmpty()) {
        QDir().mkpath(artifact_directory);
        const auto retained = QDir(artifact_directory).filePath(QFileInfo(path).fileName());
        QFile::remove(retained);
        check(QFile::copy(path, retained), "Native framebuffer artifact copy failed");
    }
    QImage image(path);
    check(!image.isNull(), "Native framebuffer must be a readable image");
    const auto scan_began = std::chrono::steady_clock::now();
    // Scan normalized rows directly. Per-pixel QColor construction added
    // substantial Debug overhead at high DPI; retain the same RGB threshold and
    // original image while avoiding that test-only overhead.
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    const auto background = image.pixelColor(0,0);
    int left=image.width(),right=-1,top=image.height(),bottom=-1;
    double sx=0,sy=0,count=0;
    for(int y=0;y<image.height();++y) {
      const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
      for(int x=0;x<image.width();++x) {
        const auto pixel=row[x];
        if(std::abs(qRed(pixel)-background.red())+std::abs(qGreen(pixel)-background.green())
            +std::abs(qBlue(pixel)-background.blue())<60) continue;
        left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
        sx+=x;sy+=y;count+=1;
      }
    }
    check(count>1000,"Native framebuffer must contain the model");
    const auto finished = std::chrono::steady_clock::now();
    std::cerr << "Native capture finished: export/load "
              << std::chrono::duration_cast<std::chrono::milliseconds>(scan_began - began).count()
              << " ms, pixel scan "
              << std::chrono::duration_cast<std::chrono::milliseconds>(finished - scan_began).count()
              << " ms" << std::endl;
    return {image,QRect(QPoint(left,top),QPoint(right,bottom)),QPointF(sx/count,sy/count)};
}
void mouse(sketch::visualization::NativeModelView& view,QEvent::Type type,QPointF p,
           Qt::MouseButton button,Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type,p,view.mapToGlobal(p.toPoint()),button,buttons,modifiers);
    QApplication::sendEvent(&view,&event);
}

using NativeTransform = std::array<double, 12>;
constexpr NativeTransform identity_transform{1,0,0,0, 0,1,0,0, 0,0,1,0};
bool near_transform(const NativeTransform& a, const NativeTransform& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 1e-8) return false;
    return true;
}
NativeTransform presentation_transform(sketch::visualization::NativeModelView& view,
                                       const std::string& id) {
    const auto value = view.nativePresentationTransform(QString::fromStdString(id));
    check(value.has_value(), "Lifecycle fixture must have the actual AIS presentation");
    return *value;
}
QPointF find_control(sketch::visualization::NativeModelView& view,
                    sketch::visualization::NativeModelView::TransformControl mode) {
    // Ask OCCT's sensitive-owner picker, rather than assuming handle colors or
    // a screen position. Logical-pixel sampling also covers fractional DPI.
    for (int y = 2; y < view.height(); y += 3)
        for (int x = 2; x < view.width(); x += 3)
            if (view.transformControlAt(QPointF(x, y)) == mode) return QPointF(x, y);
    throw std::runtime_error("Actual OCCT transform handle was not pickable");
}

sketch::Document hosted_gesture_document(bool stairs) {
    using namespace sketch;
    const std::string host_id = stairs ? "gesture-stair" : "gesture-wall";
    std::vector<Entity> entities{
        {"gesture-property", "property", nlohmann::json::object()},
        {"gesture-building", "building", {{"property_id", "gesture-property"}}},
        {"gesture-floor", "floor", {{"building_id", "gesture-building"}}},
        {"gesture-layer", "layer", {{"floor_id", "gesture-floor"}}}};
    const auto organized = [](Entity entity) {
        entity.properties.update({{"property_id", "gesture-property"},
            {"building_id", "gesture-building"}, {"floor_id", "gesture-floor"},
            {"layer_id", "gesture-layer"}, {"mark", entity.id}});
        return entity;
    };
    if (stairs) {
        entities.push_back(organized(encode_building_entity(StairFlight{
            .id=host_id, .base_position={0,0,0}, .riser_count=8, .total_rise=2,
            .going=.3, .width=1, .flights={{"gesture-lower",4},{"gesture-upper",4}},
            .landings={{"gesture-turn",1.2,.15,StairTurn::left_quarter,0}}})));
        entities.push_back(organized(encode_building_entity(Railing{
            .id="gesture-flight-rail", .height=1, .thickness=.05, .post_spacing=.4,
            .host=StairRailingHost{host_id,"gesture-upper",StairRailingSide::left,0,1}})));
        entities.push_back(organized(encode_building_entity(Railing{
            .id="gesture-landing-rail", .height=1, .thickness=.05, .post_spacing=.4,
            .landing_host=StairLandingRailingHost{host_id,StairLandingRole::connecting,
                "gesture-turn","gesture-lower","gesture-upper",0,0,1}})));
    } else {
        entities.push_back(organized(Entity{host_id, "wall", {
            {"baseline",{{"start",{-3.,0.}},{"end",{3.,0.}},{"sweep_radians",0.}}},
            {"thickness_m",.2},{"height_m",2.8},{"elevation_m",0.}}}));
        for (const bool door : {true, false}) {
            const auto id = door ? "gesture-door" : "gesture-window";
            entities.push_back(organized(Entity{id, "opening", {{"wall_id", host_id},
                {"opening_kind",door ? "door" : "window"}, {"offset_m",door ? .7 : 3.5},
                {"width_m",1.}, {"sill_m",door ? 0. : .8}, {"height_m",door ? 2.1 : 1.3},
                {"opening_assembly",opening_assembly_json(default_opening_assembly(
                    door ? OpeningAssemblyKind::door : OpeningAssemblyKind::window))}}}));
        }
    }
    if (stairs)
        entities.push_back(organized(encode_building_entity(Railing{
            "gesture-unrelated", {3,2,0}, 0,1,1,.05,.4})));
    else
        entities.push_back(organized(encode_building_entity(RectangularColumn{
            "gesture-unrelated", {3,2,0}, .2,.2,1,0})));
    return Document::create(std::move(entities));
}

void check_hosted_gesture_lifecycle(sketch::visualization::NativeModelView& view,
                                   QTemporaryDir& temporary, bool stairs) {
    using namespace sketch;
    using Control = visualization::NativeModelView::TransformControl;
    const std::string host_id = stairs ? "gesture-stair" : "gesture-wall";
    const std::vector<std::string> dependent_ids = stairs
        ? std::vector<std::string>{"gesture-flight-rail","gesture-landing-rail"}
        : std::vector<std::string>{"gesture-door","gesture-window"};
    auto model = hosted_gesture_document(stairs);
    view.setSnapshot(model.snapshot());
    check(ready_settled(view), "Hosted lifecycle scene must publish completely");
    view.fitAll();
    const auto selected = QString::fromStdString(host_id);
    const auto name = QString::fromStdString(host_id);
    if (stairs) {
        view.setSelectedEntity(QStringLiteral("gesture-unrelated"));
        check(view.transformControlsVisible() && view.beginMove(QStringLiteral("gesture-unrelated")),
              "Independent v1 railings must retain their own transform controls and Move");
        view.cancelInteraction();
        for (const auto& id : dependent_ids) {
            view.setSelectedEntity(QString::fromStdString(id));
            check(!view.transformControlsVisible() && !view.beginMove(QString::fromStdString(id)),
                  "Hosted flight and landing rails must remain selectable without standalone handles or Move");
        }
    }
    int commits = 0;
    std::map<Control, QPointF> control_points;
    std::optional<ArchitecturalTransform> requested;
    const auto commit = [&](QString target, ArchitecturalTransform transform) {
        check(target == selected, "A dependent preview must commit only the selected semantic host");
        requested = transform;
        std::vector<std::string> ids;
        const auto source = model.snapshot();
        for (const auto& [id, entity] : source.entities()) { (void)entity; ids.push_back(id); }
        ArchitecturalOperation operation{ArchitecturalAction::transform,host_id};
        operation.transform = transform;
        const auto transaction = ArchitecturalTransaction::create(make_stable_id(), "source",
            std::move(ids), {operation}, "Native gesture lifecycle");
        model.apply(architectural_transaction_command(source, transaction, model.revision()));
        ++commits;
        view.setSnapshot(model.snapshot());
    };
    view.onEntityTranslationRequested = [&](QString target,double x,double y,double z) {
        commit(target,{x,y,z,0,1});
    };
    view.onEntityTransformRequested = [&](QString target,double x,double y,double z,double angle,double scale) {
        commit(target,{x,y,z,angle,scale});
    };
    const auto check_group = [&] {
        const auto preview = presentation_transform(view, host_id);
        check(!near_transform(preview, identity_transform), "Real gesture must change the host AIS transform");
        for (const auto& id : dependent_ids)
            check(near_transform(presentation_transform(view,id),preview),
                  "Every visible dependent must preview the exact host transform");
        check(near_transform(presentation_transform(view,"gesture-unrelated"),identity_transform),
              "Host gesture must not transform unrelated presentations");
        return preview;
    };
    const auto check_reset = [&] {
        check(near_transform(presentation_transform(view,host_id),identity_transform),
              "Ending a gesture must discard the renderer's host preview");
        for (const auto& id : dependent_ids)
            check(near_transform(presentation_transform(view,id),identity_transform),
                  "Ending a gesture must discard every dependent preview");
    };
    for (const auto mode : {std::optional<Control>{}, std::optional<Control>{Control::rotation},
                             std::optional<Control>{Control::scale}}) {
        for (const bool cancel : {true, false}) {
            const auto before = model.snapshot();
            const auto previous_commits = commits;
            view.setSelectedEntity(selected);
            const auto baseline = capture(view, temporary.filePath(name+"-baseline.png"));
            QPointF start = baseline.centre / view.devicePixelRatioF();
            if (!mode) check(view.beginMove(selected), "Host must support explicit Move");
            else {
                const auto cached = control_points.find(*mode);
                if (cached == control_points.end()) {
                    start = find_control(view,*mode);
                    control_points.emplace(*mode,start);
                } else start = cached->second;
            }
            QPointF end;
            bool previewed = false;
            // A screen delta can lie on the projected rotation/scale axis.
            // Try bounded non-collinear drags through the actual event path.
            for (const auto delta : {QPointF(28,18),QPointF(-24,22),QPointF(20,-25)}) {
                mouse(view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
                end = start + delta;
                mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
                if (!near_transform(presentation_transform(view,host_id),identity_transform)) {
                    previewed = true;
                    break;
                }
                view.cancelInteraction();
                if (!mode) check(view.beginMove(selected), "Move retry must rearm the host");
            }
            check(previewed, "Actual handle drag must produce a nonidentity preview");
            const auto preview = check_group();
            const auto preview_frame = capture(view, temporary.filePath(name+"-preview.png"));
            check(preview_frame.image != baseline.image &&
                  near_transform(presentation_transform(view,host_id),preview),
                  "Export must show the transformed solids without cancelling an active gesture");
            check(model.snapshot().entities() == before.entities() && commits == previous_commits,
                  "A renderer preview must leave document and history unchanged");
            if (cancel) {
                QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
                QApplication::sendEvent(&view,&escape);
            }
            mouse(view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
            mouse(view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
            if (cancel) {
                check(commits == previous_commits && model.snapshot().entities() == before.entities(),
                      "Cancelled handle/Move and duplicate release must not commit");
                check_reset();
                check(capture(view,temporary.filePath(name+"-cancel.png")).image == baseline.image,
                      "Cancelling must restore the complete host/dependent framebuffer");
            } else {
                check(commits == previous_commits+1 && requested && model.revision() != before.revision() &&
                      model.snapshot().history().size() == before.history().size()+1,
                      "Gesture release must produce exactly one authoritative command");
                const double cosine = requested->scale * std::cos(requested->rotation_z_radians);
                const double sine = requested->scale * std::sin(requested->rotation_z_radians);
                const NativeTransform committed_transform{cosine,-sine,0,requested->x,
                    sine,cosine,0,requested->y, 0,0,requested->scale,requested->z};
                check(near_transform(committed_transform,preview),
                      "Semantic commit must reproduce the exact preview matrix, including the pivot translation");
                if (mode == Control::rotation)
                    check(std::abs(requested->rotation_z_radians)>1e-8 && std::abs(requested->scale-1)<1e-8,
                          "Rotation handle must commit signed Z rotation without scaling");
                if (mode == Control::scale)
                    check(requested->scale>0 && std::abs(requested->scale-1)>1e-8 &&
                          std::abs(requested->rotation_z_radians)<1e-8,
                          "Scale handle must commit positive uniform scale without rotation");
                check(ready_settled(view), "Committed host and dependents must regenerate together");
                check_reset();
                const auto committed = model.snapshot();
                check(committed.entities() != before.entities(), "Gesture must change semantic authoring");
                model.undo(model.revision());
                check(model.snapshot().entities() == before.entities(), "One undo must restore the complete hosted model");
                model.redo(model.revision());
                check(model.snapshot().entities() == committed.entities(), "One redo must restore the complete gesture commit");
                model.undo(model.revision());
                view.setSnapshot(model.snapshot());
                check(ready_settled(view), "Undo must regenerate all hosted geometry");
                check(capture(view,temporary.filePath(name+"-undo.png")).image == baseline.image,
                      "Undo must restore the complete native geometry without a camera refit");
            }
        }
    }
    view.onEntityTranslationRequested = {};
    view.onEntityTransformRequested = {};
    // A visibility mask never creates a presentation for a hidden dependency.
    view.setSnapshot(model.snapshot(), visualization::NativeModelView::VisibleEntityIds{
        host_id,"gesture-unrelated"});
    check(ready_settled(view) && view.beginMove(selected), "Host Move must work with hidden dependencies");
    const QPointF start(300,230), end(325,250);
    mouse(view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
    for (const auto& id : dependent_ids)
        check(near_transform(presentation_transform(view,id),identity_transform),
              "Hidden dependent presentations must not be transformed or displayed by the host preview");
    view.cancelInteraction();
}

void check_native_resize(const sketch::DocumentSnapshot& snapshot, QTemporaryDir& temporary) {
    QSplitter splitter;
    splitter.setAttribute(Qt::WA_DontShowOnScreen, true);
    splitter.setAttribute(Qt::WA_ShowWithoutActivating, true);
    splitter.addWidget(new QWidget(&splitter));
    auto* view = new sketch::visualization::NativeModelView(&splitter);
    splitter.addWidget(view);
    view->setSnapshot(snapshot);
    view->hide();
    check(!view->nativeRenderSizePixels(), "An uninitialized view has no native render extent");
    splitter.resize(1200, 760);
    splitter.show();
    QApplication::processEvents();
    settle_geometry(*view);
    view->pollGeometryPreparation();
    check(view->isGeometryPrepared(), "Hidden resize fixture must prepare before native publication");

    const auto check_extent = [&] {
        QApplication::processEvents();
        view->repaint();
        const auto physical_size = QSize(qRound(view->width() * view->devicePixelRatioF()),
                                         qRound(view->height() * view->devicePixelRatioF()));
        const auto render_size = view->nativeRenderSizePixels();
        if (render_size != std::optional<QSize>(physical_size)) {
            std::cerr << "Native extent: render "
                      << (render_size ? render_size->width() : 0) << 'x'
                      << (render_size ? render_size->height() : 0) << ", client "
                      << physical_size.width() << 'x' << physical_size.height() << '\n';
        }
        check(render_size == std::optional<QSize>(physical_size),
              "The live OpenGL render extent must follow late-show splitter layout before Fit or export");
    };

    // Match the desktop's 3D toggle: show first, then assign splitter sizes.
    view->show();
    splitter.setSizes({600, 600});
    check_extent();
    check(view->isReady(), "Late-show resize fixture must publish native geometry");
    // Export does not fit the camera. Checking the live extent first prevents
    // Dump's private framebuffer from masking the native resize failure.
    const auto first = capture(*view, temporary.filePath("late-show-first-fit.png"));
    check(first.bounds.left() > 0 && first.bounds.top() > 0 &&
              first.bounds.right() < first.image.width() - 1 &&
              first.bounds.bottom() < first.image.height() - 1,
          "The first automatic fit must contain the model after late-show layout, without explicit Fit");

    const QPointF centre = first.centre / view->devicePixelRatioF();
    mouse(*view, QEvent::MouseButtonPress, centre, Qt::MiddleButton, Qt::MiddleButton);
    mouse(*view, QEvent::MouseMove, centre + QPointF(24, 16), Qt::NoButton, Qt::MiddleButton);
    mouse(*view, QEvent::MouseButtonRelease, centre + QPointF(24, 16), Qt::MiddleButton, Qt::NoButton);
    const auto navigated = capture(*view, temporary.filePath("late-show-navigated.png"));
    check(navigated.image != first.image, "Resize fixture must exercise an actual camera pan");
    const auto navigated_size = splitter.size();

    splitter.resize(1482, 934);
    splitter.setSizes({900, 580});
    check_extent();
    view->hide();
    splitter.resize(1300, 810);
    QApplication::processEvents();
    view->show();
    splitter.setSizes({700, 600});
    check_extent();
    splitter.resize(navigated_size);
    splitter.setSizes({600, 600});
    check_extent();
    check(capture(*view, temporary.filePath("late-show-restored-size.png")).image == navigated.image,
          "Later resize and show cycles must preserve the user's navigated camera");

    // Preserve the normal native-render evidence after checking the live extent:
    // Dump can create its own correctly sized framebuffer and mask this defect.
    view->fitAll();
    const auto frame = capture(*view, temporary.filePath("late-show-resized.png"));
    check(frame.image.size() == *view->nativeRenderSizePixels() && !frame.bounds.isEmpty(),
          "The resized native viewport must render the prepared model");
}

void check_gestures(sketch::visualization::NativeModelView& view, const QString& id,
                    QTemporaryDir& temporary) {
    view.setSelectedEntity({});
    int selections = 0, translations = 0, menus = 0;
    view.onEntitySelected = [&](QString) { ++selections; };
    view.onEntityTranslationRequested = [&](QString target, double, double, double) {
        check(target == id, "Move must retain its explicit target");
        ++translations;
    };
    QPoint menu_position;
    view.onContextMenuRequested = [&](QString, QPoint position) { ++menus; menu_position = position; };
    const QPointF start(300, 230), end(330, 255);
    check(!view.beginMove(QStringLiteral("missing-native-entity")) && !view.isMoveActive(),
          "Move must reject absent targets without arming");
    const auto before = capture(view, temporary.filePath("gesture-before.png"));
    int edits = 0;
    view.onEntityEditRequested = [&](QString target) {
        check(target == id, "Edit must report the picked stable entity ID");
        ++edits;
    };
    const auto edit_point = before.centre / view.devicePixelRatioF();
    mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton);
    check(edits == 0, "Edit must wait for a stationary release");
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    check(edits == 1 && selections == 1 && translations == 0,
          "Stationary double click must select and request one entity edit");
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    check(edits == 1, "Duplicate release must not repeat the edit request");
    for (const auto type : {QEvent::KeyPress, QEvent::WindowDeactivate, QEvent::FocusOut,
                            QEvent::Hide, QEvent::UngrabMouse}) {
        mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton);
        if (type == QEvent::KeyPress) {
            QKeyEvent escape(type, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&view, &escape);
        } else {
            QEvent cancel(type);
            QApplication::sendEvent(&view, &cancel);
        }
        mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
        check(edits == 1 && translations == 0, "Cancellation must suppress pending edit release");
    }
    check(view.beginMove(id), "Move must arm before edit drag test");
    mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, edit_point + QPointF(30, 30), Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, edit_point, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    check(edits == 1 && translations == 0 && !view.isMoveActive(),
          "Double click drag returning to its origin must neither edit nor translate");
    mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, edit_point + QPointF(30, 30), Qt::LeftButton, Qt::NoButton);
    check(edits == 1, "Release displacement without move events must suppress edit");
    mouse(view, QEvent::MouseButtonDblClick, {2, 2}, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton, Qt::NoButton);
    check(edits == 1 && selections == 1, "Background double click must preserve selection without editing");
    mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    check(edits == 1, "Modified double click must not request editing");
    view.setSelectedEntity({});
    mouse(view, QEvent::MouseButtonPress, edit_point, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonDblClick, edit_point, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, edit_point, Qt::LeftButton, Qt::NoButton);
    check(edits == 2 && selections == 2 && translations == 0,
          "Qt double click sequence must select once and edit once without translation");
    selections = 0;
    mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    mouse(view, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    const auto panned = capture(view, temporary.filePath("ctrl-pan.png"));
    check(panned.centre.x() > before.centre.x() + 5 && panned.centre.y() > before.centre.y() + 5 &&
          selections == 0 && translations == 0, "Ctrl drag must pan without selection or document callbacks");
    mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    mouse(view, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    check(selections == 0 && translations == 0, "Ctrl click must not select or edit");

    check(view.beginMove(id), "Visible entity must support explicit Move");
    mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonPress, end, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    mouse(view, QEvent::MouseButtonRelease, end, Qt::RightButton, Qt::LeftButton);
    check(translations == 0 && menus == 0 && view.isMoveActive(), "Mixed release cannot complete Move");
    mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    check(translations == 1 && !view.isMoveActive(), "Explicit Move must commit exactly once");

    for (const auto type : {QEvent::KeyPress, QEvent::UngrabMouse, QEvent::Hide,
                            QEvent::WindowDeactivate, QEvent::FocusOut, QEvent::User}) {
        check(view.beginMove(id), "Move must rearm after completion");
        mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        if (type == QEvent::KeyPress) {
            QKeyEvent escape(type, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&view, &escape);
        } else if (type == QEvent::User) {
            view.cancelInteraction();
        } else {
            QEvent cancel(type);
            QApplication::sendEvent(&view, &cancel);
        }
        mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        check(!view.isMoveActive() && translations == 1, "Cancellation must suppress later Move release");
    }
    mouse(view, QEvent::MouseMove, {2, 2}, Qt::NoButton, Qt::NoButton);
    check(capture(view, temporary.filePath("cancelled-move.png")).image == panned.image,
          "Cancelled Move must restore the original presentation");
    mouse(view, QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton);
    mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::RightButton | Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::RightButton);
    check(menus == 0 && selections == 0, "Extra left button must not steal right gesture");
    mouse(view, QEvent::MouseButtonRelease, start, Qt::RightButton, Qt::NoButton);
    check(menus == 1 && menu_position == view.mapToGlobal(start.toPoint()), "Right click must expose global context position");
    check(selections == 1, "Stationary context click must publish the hit selection");
    selections = 0;
    mouse(view, QEvent::MouseButtonDblClick, start, Qt::RightButton, Qt::RightButton);
    mouse(view, QEvent::MouseButtonRelease, start, Qt::RightButton, Qt::NoButton);
    check(menus == 1, "Right double click must not open a duplicate context menu");
    mouse(view, QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton);
    mouse(view, QEvent::MouseMove, end, Qt::NoButton, Qt::RightButton);
    mouse(view, QEvent::MouseButtonRelease, end, Qt::RightButton, Qt::NoButton);
    check(menus == 1 && capture(view, temporary.filePath("orbit.png")).image != panned.image,
          "Right drag must orbit without opening a menu");
    mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonDblClick, start, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    check(selections == 1 && translations == 1, "Double click must select only once");
    check(view.beginMove(id), "Move must rearm for double click suppression");
    mouse(view, QEvent::MouseButtonDblClick, start, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    check(!view.isMoveActive() && translations == 1, "Double click must not start another Move");
    view.onEntitySelected = {};
    view.onEntityEditRequested = {};
    view.onEntityTranslationRequested = {};
    view.onContextMenuRequested = {};
}

void check_context_target(sketch::visualization::NativeModelView& view, QTemporaryDir& temporary) {
    auto a = sketch::encode_building_entity(sketch::RectangularColumn{"context-a", {-3,0,0}, 1,1,3,0});
    auto b = sketch::encode_building_entity(sketch::RectangularColumn{"context-b", {3,0,0}, 1,1,3,0});
    auto document = sketch::Document::create({a, b});
    view.setSnapshot(document.snapshot());
    check(ready_settled(view), "Context fixture must publish");
    view.fitAll();
    view.setSnapshot(document.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{a.id});
    const auto point_a = capture(view, temporary.filePath("context-a.png")).centre / view.devicePixelRatioF();
    view.setSnapshot(document.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{b.id});
    const auto point_b = capture(view, temporary.filePath("context-b.png")).centre / view.devicePixelRatioF();
    view.setSnapshot(document.snapshot());
    check(ready_settled(view), "Both context targets must publish");
    QString selected, target;
    int menus = 0;
    view.onEntitySelected = [&](QString id) { selected = id; };
    view.onContextMenuRequested = [&](QString id, QPoint global) {
        target = id;
        ++menus;
        check(selected == id, "Context target must be selected before its menu callback");
        if (!id.isEmpty()) check(global == view.mapToGlobal(point_b.toPoint()),
                                "Context callback must preserve global logical position");
    };
    mouse(view, QEvent::MouseButtonPress, point_a, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, point_a, Qt::LeftButton, Qt::NoButton);
    check(selected == QString::fromStdString(a.id), "Context test must initially select A");
    mouse(view, QEvent::MouseButtonPress, point_b, Qt::RightButton, Qt::RightButton);
    mouse(view, QEvent::MouseButtonRelease, point_b, Qt::RightButton, Qt::NoButton);
    check(menus == 1 && target == QString::fromStdString(b.id),
          "Right click on B must target B even when A was selected");
    mouse(view, QEvent::MouseButtonPress, {2,2}, Qt::RightButton, Qt::RightButton);
    mouse(view, QEvent::MouseButtonRelease, {2,2}, Qt::RightButton, Qt::NoButton);
    check(menus == 2 && target.isEmpty(), "Background context menu must receive an empty target");
    view.onEntitySelected = {};
    view.onContextMenuRequested = {};
}

void check_desktop_room_activation(QTemporaryDir& temporary) {
    sketch::desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    window.resize(1200, 760);
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    const sketch::Boundary boundary{{{{0.0,0.0},{4.0004,0.0},0.0},
                                     {{4.0004,0.0},{4.0004,2.0007},0.0},
                                     {{4.0004,2.0007},{0.0,2.0007},0.0},
                                     {{0.0,2.0007},{0.0,0.0},0.0}}};
    const auto room_id = window.createRoomVolumeFromBoundary(
        boundary, QStringLiteral("2.4004 m"), QStringLiteral("0.1234 m"));
    check(!room_id.isEmpty(), "Desktop native activation fixture must create a room");
    sketch::visualization::NativeModelView* view = nullptr;
    for (auto* widget : window.findChildren<QWidget*>()) {
        if (auto* candidate = dynamic_cast<sketch::visualization::NativeModelView*>(widget)) {
            view = candidate;
            break;
        }
    }
    check(view != nullptr, "Architectural workspace must contain the native model view");
    view->setAttribute(Qt::WA_DontShowOnScreen, true);
    view->setAttribute(Qt::WA_ShowWithoutActivating, true);
    window.show();
    QApplication::processEvents();
    check(ready_settled(*view), "Desktop room must prepare in the native viewport");
    view->fitAll();
    const auto hit = capture(*view, temporary.filePath("desktop-room-activation.png")).centre /
                     view->devicePixelRatioF();
    check(window.selectEntity(room_id) && view->transformControlsVisible(),
          "Plan or navigator selection must synchronize native room transform controls");
    const auto before = window.document().snapshot();
    bool opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        auto* width = dialog ? dialog->findChild<QLineEdit*>(
                                   QStringLiteral("roomDimensionWidth")) : nullptr;
        auto* status = dialog ? dialog->findChild<QLabel*>(
                                    QStringLiteral("roomDimensionStatus")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
                                     QStringLiteral("roomDimensionButtons")) : nullptr;
        check(dialog && width && status && buttons,
              "Native room double click must open the shared dimension editor");
        opened = true;
        width->setText(QStringLiteral("5 m"));
        check(buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                  status->text().contains(QStringLiteral("floor area")),
              "Native room editor must produce a valid detached preview");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    mouse(*view, QEvent::MouseButtonDblClick, hit, Qt::LeftButton, Qt::LeftButton);
    mouse(*view, QEvent::MouseButtonRelease, hit, Qt::LeftButton, Qt::NoButton);
    check(opened && window.document().snapshot().revision() == before.revision() &&
              window.document().snapshot().entities() == before.entities(),
          "Canceling the native-activated room preview must restore the authoritative document");
    window.hide();
}

void check_desktop_hosted_gestures(QTemporaryDir& temporary) {
    using namespace sketch;
    using Control = visualization::NativeModelView::TransformControl;
    auto document = std::make_shared<Document>(hosted_gesture_document(true));
    desktop::MainWindow window(document);
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.setAttribute(Qt::WA_ShowWithoutActivating,true);
    window.resize(1200,760);
    window.setWorkspace(desktop::Workspace::architectural);
    window.setNativeModelViewVisible(true);
    visualization::NativeModelView* view = nullptr;
    for (auto* widget : window.findChildren<QWidget*>())
        if (auto* candidate = dynamic_cast<visualization::NativeModelView*>(widget)) {
            view = candidate;
            break;
        }
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    check(view && canvas, "Actual shell fixture must expose native and architectural plan surfaces");
    view->setAttribute(Qt::WA_DontShowOnScreen,true);
    view->setAttribute(Qt::WA_ShowWithoutActivating,true);
    window.show();
    QApplication::processEvents();
    check(ready_settled(*view) && view->isVisible(), "Actual shell native surface must publish while shown");
    check(window.lastError().isEmpty(),
          "Actual shell preparation progress must not become a retained operation error");
    view->fitAll();
    const std::vector<std::string> affected{"gesture-stair","gesture-flight-rail","gesture-landing-rail"};
    const auto host = QStringLiteral("gesture-stair");
    for (const auto& id : {QStringLiteral("gesture-flight-rail"),QStringLiteral("gesture-landing-rail")})
        check(window.selectEntity(id) && !view->transformControlsVisible() && !view->beginMove(id),
              "Shell rail selection must retain the semantic rail without unsupported standalone controls");
    const auto plans = [&] {
        std::map<std::string,Boundary> result;
        for (const auto& id : affected) {
            const auto found = std::find_if(canvas->entities().begin(),canvas->entities().end(),
                [&](const auto& entity) { return entity.id == QString::fromStdString(id); });
            check(found != canvas->entities().end() && !found->segments.empty(),
                  "Actual retained plan must contain the host and both railing forms");
            result.emplace(id,found->segments);
        }
        return result;
    };
    const auto quantities = [&] {
        const auto projection = window.scheduleSnapshot();
        std::string schedule_context =
            "Shell schedule must be complete and bound to the actual gesture revision; schedule revision=" +
            std::to_string(projection.snapshot.revision) + "; document revision=" +
            std::to_string(window.document().revision());
        for (const auto& diagnostic : projection.diagnostics)
            schedule_context += "\n  " + diagnostic;
        check(projection.diagnostics.empty() &&
              projection.snapshot.revision == window.document().revision(),
              schedule_context.c_str());
        std::map<std::string,std::map<std::string,ScheduleQuantity>> result;
        for (const auto& id : affected) {
            const auto row = std::find_if(projection.snapshot.rows.begin(),projection.snapshot.rows.end(),
                [&](const auto& value) { return value.object_id == id; });
            check(row != projection.snapshot.rows.end(), "Shell schedule must retain each hosted building object");
            auto& values = result[id];
            for (const auto& [key,cell] : row->cells)
                if (const auto* quantity = std::get_if<ScheduleQuantity>(&cell.value)) values.emplace(key,*quantity);
            check(!values.empty(), "Hosted schedule rows must contain actual dimensional quantities");
        }
        return result;
    };
    // Deliberately retain MainWindow's real callbacks. The view must invoke the
    // same selection, authoritative command and refresh path used by the UI.
    int stage = 0;
    for (const auto mode : {std::optional<Control>{},std::optional<Control>{Control::rotation},
                             std::optional<Control>{Control::scale}}) {
        ++stage;
        check(window.selectEntity(host) && view->transformControlsVisible(),
              "Shell selection must expose the native stair handles");
        const auto source = window.document().snapshot();
        const auto before_plans = plans();
        const auto before_quantities = quantities();
        const auto baseline = capture(*view,temporary.filePath(QString("shell-gesture-%1-before.png").arg(stage)));
        QPointF start = baseline.centre / view->devicePixelRatioF();
        if (!mode) check(view->beginMove(host), "Real shell host must support native Move");
        else start = find_control(*view,*mode);
        QPointF end;
        bool changed = false;
        for (const auto delta : {QPointF(28,18),QPointF(-24,22),QPointF(20,-25)}) {
            mouse(*view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
            end = start + delta;
            mouse(*view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
            changed = !near_transform(presentation_transform(*view,"gesture-stair"),identity_transform);
            if (changed) break;
            view->cancelInteraction();
            if (!mode) check(view->beginMove(host), "Shell Move retry must rearm the host");
        }
        check(changed, "Actual shell native mouse drag must produce a preview");
        const auto preview = presentation_transform(*view,"gesture-stair");
        for (const auto& id : affected)
            check(near_transform(presentation_transform(*view,id),preview),
                  "Shell preview must move host and both hosted rail presentations together");
        check(window.document().snapshot().entities() == source.entities() &&
              window.document().snapshot().history().size() == source.history().size(),
              "Actual shell preview must not publish authoring or history");
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        mouse(*view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        check(ready_settled(*view) && window.regenerationReadyForCurrentRevision(),
              "Real shell callback must regenerate the current coordinated model");
        const auto committed = window.document().snapshot();
        const auto commit_context =
            "Actual shell gesture and duplicate release must commit exactly one authoritative command; stage=" +
            std::to_string(stage) + "; source revision=" + std::to_string(source.revision()) +
            "; committed revision=" + std::to_string(committed.revision()) +
            "; source history=" + std::to_string(source.history().size()) +
            "; committed history=" + std::to_string(committed.history().size()) +
            "; entities changed=" + std::to_string(committed.entities() != source.entities());
        check(committed.revision() == source.revision()+1 &&
              committed.entities() != source.entities() &&
              committed.history().size() == source.history().size()+1,
              commit_context.c_str());
        const auto error_context = "Successful shell gesture must settle without a stale preparation diagnostic; stage=" +
            std::to_string(stage) + "; shell error=" + window.lastError().toStdString() +
            "; native error=" + view->lastError().toStdString();
        check(window.lastError().isEmpty(), error_context.c_str());
        const auto after_plans = plans();
        bool plan_changed = false;
        for (const auto& [id,boundary] : before_plans) {
            const auto& after = after_plans.at(id);
            for (const auto& segment : boundary)
                for (const auto point : {segment.start,segment.end}) {
                    const Vec2 expected{preview[0]*point.x+preview[1]*point.y+preview[3],
                                        preview[4]*point.x+preview[5]*point.y+preview[7]};
                    plan_changed |= std::hypot(expected.x-point.x,expected.y-point.y)>1e-7;
                    check(std::any_of(after.begin(),after.end(),[&](const auto& target) {
                        return std::hypot(target.start.x-expected.x,target.start.y-expected.y)<1e-5 ||
                               std::hypot(target.end.x-expected.x,target.end.y-expected.y)<1e-5;
                    }), "Actual retained plan endpoints must follow the exact native preview transform");
                }
        }
        check(plan_changed, "A shell gesture must update coordinated plan placement");
        const auto after_quantities = quantities();
        const double scale = std::hypot(preview[0],preview[4]);
        for (const auto& [id,values] : before_quantities)
            for (const auto& [key,quantity] : values) {
                const auto& after = after_quantities.at(id).at(key);
                const int power = quantity.unit == ScheduleUnit::square_metre ? 2 :
                                  quantity.unit == ScheduleUnit::cubic_metre ? 3 : 1;
                check(after.unit == quantity.unit &&
                      std::abs(after.value-quantity.value*std::pow(scale,power)) <
                          1e-7*std::max(1.,std::abs(after.value)),
                      "Actual schedule dimensional quantities must scale once and remain unchanged by rigid gestures");
            }
        for (const auto& id : affected)
            check(near_transform(presentation_transform(*view,id),identity_transform),
                  "Shell regeneration must replace previews with authoritative native geometry");
        check(capture(*view,temporary.filePath(QString("shell-gesture-%1-after.png").arg(stage))).image != baseline.image,
              "Real shell gesture must visibly regenerate the native model");
        check(window.undoCommand() && ready_settled(*view) &&
              window.document().snapshot().entities() == source.entities(),
              "One actual shell undo must restore the complete stair/railing graph");
        check(quantities() == before_quantities &&
              capture(*view,temporary.filePath(QString("shell-gesture-%1-undo.png").arg(stage))).image == baseline.image,
              "Shell undo must restore quantities and all native host/dependent geometry");
        check(window.redoCommand() && ready_settled(*view) &&
              window.document().snapshot().entities() == committed.entities() &&
              quantities() == after_quantities,
              "One actual shell redo must restore the exact hosted gesture and quantities");
        const auto path = temporary.filePath(QString("shell-gesture-%1.bldproj").arg(stage));
        const auto saved = window.document().snapshot();
        const auto saved_frame = capture(*view,temporary.filePath(QString("shell-gesture-%1-saved.png").arg(stage)));
        check(window.saveProjectAs(path) && window.openProject(path) && ready_settled(*view) &&
              window.document().snapshot().entities() == saved.entities() &&
              window.document().snapshot().history().size() == saved.history().size() &&
              quantities() == after_quantities,
              "Local shell save/reopen must retain the exact stair, hosted rails, history and quantities");
        check(capture(*view,temporary.filePath(QString("shell-gesture-%1-reopened.png").arg(stage))).image == saved_frame.image,
              "Reopened hosted native geometry must reproduce the saved frame");
        check(window.undoCommand() && ready_settled(*view) &&
              window.document().snapshot().entities() == source.entities() &&
              window.redoCommand() && ready_settled(*view) &&
              window.document().snapshot().entities() == saved.entities(),
              "Reopened shell must retain one-command undo and redo for the native gesture");
    }
    window.hide();
}

struct MaterialPixels {
    std::size_t red{}, green{}, blue{};
    std::optional<QPointF> red_point;
};
MaterialPixels material_pixels(const QImage& image) {
    MaterialPixels result;
    double red_x = 0, red_y = 0;
    for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
        const auto color = image.pixelColor(x,y);
        if (color.red() > 1.5*color.green()+30 && color.red() > 1.5*color.blue()+30) {
            ++result.red;
            red_x += x; red_y += y;
        }
        if (color.green() > 1.5*color.red()+30 && color.green() > 1.5*color.blue()+30) ++result.green;
        if (color.blue() > 1.5*color.red()+30 && color.blue() > 1.5*color.green()+30) ++result.blue;
    }
    if (result.red) result.red_point = QPointF(red_x/result.red,red_y/result.red);
    return result;
}

// Catches stale region-face matching, collapsed root identity and host copies
// by exercising actual preparation, AIS rendering and native gesture callbacks.
void check_regional_material_publication(sketch::visualization::NativeModelView& view,
                                         QTemporaryDir& temporary) {
    using namespace sketch;
    const Boundary rectangle{{{0,0},{2,0}},{{2,0},{2,4}},{{2,4},{0,4}},{{0,4},{0,0}}};
    AssemblyType type; type.id = "native-pair"; type.name = "Pair";
    type.materials = {{"first","red"},{"second","blue"}};
    type.profiles.push_back({"a",rectangle,{},0,0.2,"first"});
    auto second = type.profiles.front(); second.id = "b"; second.material_slot = "second";
    for (auto& segment : second.outer) { segment.start.x += 2; segment.end.x += 2; }
    type.profiles.push_back(second);
    for (const int fixture : {0,1,2}) {
        const bool assembly = fixture != 0;
        const bool embedded = fixture == 2;
        const std::string root_id = embedded ? "native-region-catalog:instance:regional-child" :
            assembly ? "native-independent" : "native-roof-regions";
        std::vector<AssemblyInstance> embedded_instances;
        if (embedded) {
            AssemblyInstance instance; instance.id = "regional-child"; instance.type_id = type.id;
            instance.root_transform = AssemblyTransform{};
            embedded_instances.push_back(instance);
        }
        Entity catalog{"native-region-catalog","assembly_model",{{"model",AssemblyModel::create(
            {{"red","Red","#ff0000"},{"blue","Blue","#0000ff"}}, {type}, embedded_instances).to_json()}}};
        std::vector<Entity> entities{catalog};
        if (assembly && !embedded) {
            AssemblyInstance instance; instance.id = root_id; instance.type_id = type.id;
            instance.root_transform = AssemblyTransform{};
            entities.push_back(encode_document_assembly_instance(Entity{root_id,"assembly_instance"},
                {catalog.id,instance}));
        } else if (!assembly) {
            auto first = encode_building_entity(SlopedRoofPanel{"native-roof-a",{0,0,0},0,2,4,0,0,0,.2,{}});
            auto last = encode_building_entity(SlopedRoofPanel{"native-roof-b",{1.9,0,0},0,2,4,0,0,0,.2,{}});
            first.properties["material_assignment"] = {{"version",1},{"catalog_id",catalog.id},{"material_id","red"}};
            last.properties["material_assignment"] = {{"version",1},{"catalog_id",catalog.id},{"material_id","blue"}};
            entities.push_back(first); entities.push_back(last);
            entities.push_back(Entity{root_id,"roof_join",roof_join_json(RoofJoin{root_id,{first.id,last.id}})});
        }
        auto model = Document::create(entities);
        const auto original = model.snapshot();
        const auto id = QString::fromStdString(root_id);
        const auto path = [&](const char* suffix) { return temporary.filePath(QStringLiteral("native-regions-%1").arg(fixture) + suffix); };
        view.setSelectedEntity({}); view.setSnapshot(model.snapshot());
        check(ready_settled(view), "Material region fixture must prepare and publish actual native geometry");
        view.fitAll();
        const auto initial = capture(view,path("-red-blue.png"));
        const auto pixels = material_pixels(initial.image);
        check(pixels.red > 100 && pixels.blue > 100,
              "AIS must render the actual material of both roof or assembly regions");
        {
            // Pick an interior red pixel, away from edge antialiasing and the
            // centroid gap between profiles. Double-click reports root identity.
            check(pixels.red_point.has_value(), "Regional selection fixture requires an interior material pixel");
            const QPointF pick = *pixels.red_point/view.devicePixelRatioF();
            QString selected, edited;
            view.onEntitySelected = [&](QString value) {
                selected = value;
                // MainWindow synchronizes its semantic selection back to the
                // view. Retain that production observer contract in this
                // standalone viewport fixture as well.
                view.setSelectedEntity(value);
            };
            view.onEntityEditRequested = [&](QString value) { edited = value; };
            mouse(view,QEvent::MouseButtonDblClick,pick,Qt::LeftButton,Qt::LeftButton);
            mouse(view,QEvent::MouseButtonRelease,pick,Qt::LeftButton,Qt::NoButton);
            check(selected == id && edited == id && view.transformControlsVisible() == assembly,
                  "Picking any region selects its semantic root; both independent and catalog-owned geometric assemblies receive transform controls");
            view.onEntitySelected = {}; view.onEntityEditRequested = {};
            view.setSelectedEntity(id);
        }
        catalog.properties["model"] = AssemblyModel::create(
            {{"red","Red","#00ff00"},{"blue","Blue"}}, {type}, embedded_instances).to_json();
        model.apply(ApplyEntityChanges{model.revision(),{EntityChange::upsert(catalog)}, {}, "Recolor regions"});
        view.setSnapshot(model.snapshot());
        const auto recolored = capture(view,path("-green-default.png"));
        const auto changed = material_pixels(recolored.image);
        check(changed.green > 100 && changed.red == 0 && changed.blue == 0 &&
              recolored.bounds == initial.bounds && recolored.image != initial.image,
              "Appearance-only publication must recolor every region and remove cleared stale colors");
        check(view.lastPublicationMetrics() && view.lastPublicationMetrics()->created == 1 &&
              view.lastPublicationMetrics()->removed == 1,
              "Regional appearance refresh replaces exactly one root AIS presentation");
        if (assembly) {
            check(view.transformControlsVisible(), "Regional recoloring retains selected root transform controls");
            int moves = 0;
            view.onEntityTranslationRequested = [&](QString target,double x,double y,double z) {
                check(target == id && std::isfinite(x+y+z) && std::abs(x)+std::abs(y)+std::abs(z)>1e-8,
                      "Assembly Move emits one independent root target and a finite nonzero world delta");
                ++moves;
            };
            check(view.beginMove(id), "Independent root must support native Move");
            const QPointF start = initial.centre / view.devicePixelRatioF();
            const auto before = view.nativePresentationTransform(id);
            mouse(view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
            mouse(view,QEvent::MouseMove,start+QPointF(20,12),Qt::NoButton,Qt::LeftButton);
            check(view.nativePresentationTransform(id) != before,
                  "Native Move previews the whole root compound as one object");
            mouse(view,QEvent::MouseButtonRelease,start+QPointF(20,12),Qt::LeftButton,Qt::NoButton);
            check(moves == 1 && view.nativePresentationTransform(id) == before && view.transformControlsVisible(),
                  "Move release emits one semantic request and restores the derived root preview");
            view.onEntityTranslationRequested = {};
        }
        model.undo(model.revision()); view.setSnapshot(model.snapshot());
        check(capture(view,path("-undo.png")).image == initial.image,
              "Undo restores all regional materials without moving geometry or camera");
        check(model.snapshot().entities() == original.entities(),
              "Native region rendering and gesture requests preserve authoritative source entities");
        view.setSelectedEntity({});
    }
}

void check_publication_reuse(sketch::visualization::NativeModelView& view) {
    // Exercise actual AIS publication on the Windows driver. Counts express
    // bounded presentation work; recorded timings do not qualify production
    // performance or a reference-hardware frame target.
    constexpr std::size_t object_count = 256;
    std::vector<sketch::Entity> walls;
    for (std::size_t i = 0; i < object_count; ++i) {
        const double x = static_cast<double>(i % 16) * 6.0;
        const double y = static_cast<double>(i / 16) * 6.0;
        auto wall = sketch::Entity::create("wall", {
            {"baseline", {{"start", {x, y}}, {"end", {x + 4.0, y}}, {"sweep_radians", 0.0}}},
            {"thickness_m", 0.2}, {"height_m", 2.8}, {"elevation_m", 0.0}});
        wall.id = "publication-wall-" + std::to_string(i);
        walls.push_back(std::move(wall));
    }
    auto document = sketch::Document::create(std::move(walls));
    const auto publish = [&](const char* label,
                             std::optional<sketch::visualization::NativeModelView::VisibleEntityIds> visible = std::nullopt) {
        view.setSnapshot(document.snapshot(), std::move(visible));
        check(!view.lastPublicationMetrics(), "New requests must clear stale publication diagnostics");
        check(ready_settled(view), "Publication fixture must be ready");
        const auto metrics = view.lastPublicationMetrics();
        check(metrics.has_value() && std::isfinite(metrics->elapsed_ms) && metrics->elapsed_ms >= 0.0,
              "Successful native publication must record finite owner-thread timing");
        std::cerr << "Native publication " << label << ": " << metrics->elapsed_ms
                  << " ms; created=" << metrics->created << "; reused=" << metrics->reused
                  << "; removed=" << metrics->removed << '\n';
        return *metrics;
    };
    auto metrics = publish("256-object initial scene");
    check(metrics.created == object_count && metrics.reused == 0,
          "A new model must create one presentation for each solid");
    auto edited = document.snapshot().entities().at("publication-wall-0");
    edited.properties["height_m"] = 3.2;
    document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::upsert(edited)}, {}, "Edit one of 256 walls"});
    metrics = publish("one-object edit");
    check(metrics.created == 1 && metrics.reused == object_count - 1 && metrics.removed == 1,
          "A one-object edit must retain every unchanged AIS presentation");
    metrics = publish("hide all", sketch::visualization::NativeModelView::VisibleEntityIds{});
    check(metrics.created == 0 && metrics.reused == object_count && metrics.removed == 0,
          "Visibility changes must update existing presentations without rebuilding shapes");
    metrics = publish("reveal all");
    check(metrics.created == 0 && metrics.reused == object_count && metrics.removed == 0,
          "Revealing unchanged geometry must reuse every presentation");
    document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::erase("publication-wall-0")}, {}, "Delete one of 256 walls"});
    metrics = publish("one-object deletion");
    check(metrics.created == 0 && metrics.reused == object_count - 1 && metrics.removed == 1,
          "Deleting one solid must detach only its presentation");
    document.undo(document.revision());
    metrics = publish("undo deletion");
    check(metrics.created == 1 && metrics.reused == object_count - 1 && metrics.removed == 0,
          "Undoing deletion must create only the restored solid's presentation");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc,argv);
    if(QGuiApplication::platformName()!=QStringLiteral("windows")) return 77;
    const auto arguments = application.arguments();
    const int expected_dpr_index = arguments.indexOf(QStringLiteral("--expected-dpr"));
    if (expected_dpr_index < 0 || expected_dpr_index + 1 >= arguments.size()) {
        std::cerr << "Native test requires --expected-dpr followed by a finite positive number\n";
        return 2;
    }
    const QString expected_dpr_text = arguments.at(expected_dpr_index + 1);
    double expected_dpr = 0.0;
    if (!parse_positive_finite(expected_dpr_text, expected_dpr)) {
        std::cerr << "Native --expected-dpr must be finite and greater than zero; got '"
                  << expected_dpr_text.toStdString() << "'\n";
        return 2;
    }
    const int scenario_index = arguments.indexOf(QStringLiteral("--scenario"));
    const QString scenario = scenario_index < 0 ? QStringLiteral("all") : arguments.value(scenario_index + 1);
    if (scenario != "all" && scenario != "geometry" && scenario != "forms" && scenario != "publication" && scenario != "gestures" && scenario != "resize" && scenario != "export-path") {
        std::cerr << "Native scenario must be all, geometry, forms, publication, gestures, resize or export-path\n";
        return 1;
    }
    QTemporaryDir temporary;
    check(temporary.isValid(),"Native test needs temporary storage");
    auto document=sketch::Document::create({sketch::Entity::create("wall",{
        {"baseline",{{"start",{-3.0,0.0}},{"end",{3.0,0.0}},{"sweep_radians",0.0}}},
        {"thickness_m",0.2},{"height_m",2.8},{"elevation_m",0.0}})});
    const auto wall_id=QString::fromStdString(document.snapshot().entities().begin()->first);
    sketch::visualization::NativeModelView view;
    view.setAttribute(Qt::WA_DontShowOnScreen,true);
    view.setAttribute(Qt::WA_ShowWithoutActivating,true);
    view.resize(700,500);
    view.setSnapshot(document.snapshot());
    QString selected;
    view.onEntitySelected=[&](QString id){selected=std::move(id);};
    view.show();
    QTimer::singleShot(100,&application,[&] {
        try {
            const double ratio=view.devicePixelRatioF();
            constexpr double dpr_tolerance = 0.001;
            if (!std::isfinite(ratio) || std::abs(ratio - expected_dpr) > dpr_tolerance) {
                std::cerr << "Native DPR mismatch: expected " << expected_dpr
                          << ", actual " << ratio << " (tolerance " << dpr_tolerance << ")\n";
                application.exit(1);
                return;
            }
            check(ready_settled(view),"Native viewport must be ready");
            if (scenario == "all" || scenario == "export-path") {
                check_export_paths(view, temporary, wall_id);
                if (scenario == "export-path") {
                    std::cout << "Native export path checks passed at DPR " << ratio << '\n';
                    application.exit(0);
                    return;
                }
            }
            if (scenario == "all" || scenario == "resize") {
                check_native_resize(document.snapshot(), temporary);
                if (scenario == "resize") {
                    std::cout << "Native resize checks passed at DPR " << ratio << '\n';
                    application.exit(0);
                    return;
                }
            }
            view.setSelectedEntity(wall_id);
            check(view.transformControlsVisible(),
                  "A selected transformable solid must expose native transform controls");
            const auto selected_export = capture(
                view, temporary.filePath("selected-export-without-controls.png"));
            check(view.transformControlsVisible(),
                  "Framebuffer export must restore selected-object transform controls");
            view.setSelectedEntity({});
            check(capture(view, temporary.filePath("unselected-export.png")).image ==
                      selected_export.image,
                  "Exported 3D imagery must omit editing transform controls");
            view.setSelectedEntity(QStringLiteral("missing-native-entity"));
            check(!view.transformControlsVisible(),
                  "Native transform controls must reject a missing semantic target");
            if (scenario == "all" || scenario == "gestures") {
                check_hosted_gesture_lifecycle(view, temporary, false);
                check_hosted_gesture_lifecycle(view, temporary, true);
                check_desktop_hosted_gestures(temporary);
                view.setSnapshot(document.snapshot());
                check(ready_settled(view), "Hosted lifecycle checks must restore the primary gesture scene");
                view.fitAll();
            }
            if (scenario == "gestures") {
                check_gestures(view, wall_id, temporary);
                int hidden_edits = 0, hidden_selections = 0;
                view.onEntityEditRequested = [&](QString) { ++hidden_edits; };
                view.onEntitySelected = [&](QString) { ++hidden_selections; };
                view.setSnapshot(document.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{});
                check(ready_settled(view), "Hidden geometry must publish successfully");
                mouse(view, QEvent::MouseButtonDblClick, {350, 250}, Qt::LeftButton, Qt::LeftButton);
                mouse(view, QEvent::MouseButtonRelease, {350, 250}, Qt::LeftButton, Qt::NoButton);
                check(hidden_edits == 0 && hidden_selections == 0,
                      "Hidden entities must not be selected or edited by double click");
                view.onEntityEditRequested = {};
                view.onEntitySelected = {};
                check_context_target(view, temporary);
                check_desktop_room_activation(temporary);
                std::cout << "Native gesture checks passed at DPR " << ratio << '\n';
                application.exit(0);
                return;
            }
            if (scenario == "publication") {
                check_regional_material_publication(view, temporary);
                check_publication_reuse(view);
                application.exit(0);
                return;
            }
            if (scenario != "forms") {
                view.fitAll();
                auto first=capture(view,temporary.filePath("first.png"));
                {
                    const auto source = document.snapshot();
                    std::vector<sketch::Entity> walls;
                    for (int i = 0; i != 24; ++i) {
                        auto wall = source.entities().begin()->second;
                        wall.id = "cancel-native-wall-" + std::to_string(i);
                        wall.properties["elevation_m"] = double(i);
                        walls.push_back(std::move(wall));
                    }
                    const auto expensive = sketch::Document::create(std::move(walls));
                    view.setSnapshot(expensive.snapshot());
                    check(view.isGeometryPending() && !view.isReady() &&
                          !view.exportViewImage(temporary.filePath("pending-must-not-export.png")),
                          "Pending real geometry must not be reported or exported as current");
                    view.setSnapshot(source,
                        sketch::visualization::NativeModelView::VisibleEntityIds{});
                    view.setSnapshot(source);
                    const auto restored = capture(view, temporary.filePath("cancel-restored.png"));
                    check(restored.image == first.image &&
                          document.revision() == source.revision() &&
                          document.snapshot().history().size() == source.history().size(),
                          "Superseded geometry and visibility must preserve the valid scene and history");
                }
                check(first.bounds.left()>2 && first.bounds.right()<first.image.width()-3
                      &&first.bounds.top()>2 && first.bounds.bottom()<first.image.height()-3,
                      "Fit must leave all geometry inside the native frame");
                QString translated_wall_id;
                double translated_wall_x = 0.0;
                double translated_wall_y = 0.0;
                double translated_wall_z = 0.0;
                view.onEntityTranslationRequested =
                    [&](QString id, double x, double y, double z) {
                        translated_wall_id = std::move(id);
                        translated_wall_x = x;
                        translated_wall_y = y;
                        translated_wall_z = z;
                    };
                const auto wall_drag_start = first.centre / view.devicePixelRatioF();
                const auto wall_drag_end = wall_drag_start + QPointF(16.0, 10.0);
                mouse(view, QEvent::MouseButtonPress, wall_drag_start, Qt::LeftButton,
                      Qt::LeftButton, Qt::ControlModifier);
                mouse(view, QEvent::MouseButtonRelease, wall_drag_start, Qt::LeftButton,
                      Qt::NoButton, Qt::ControlModifier);
                check(selected.isEmpty(), "Ctrl+click must not select an object");
                check(view.beginMove(wall_id), "Wall must support explicit Move");
                mouse(view, QEvent::MouseButtonPress, wall_drag_start, Qt::LeftButton,
                      Qt::LeftButton);
                mouse(view, QEvent::MouseMove, wall_drag_end, Qt::NoButton,
                      Qt::LeftButton);
                check(capture(view, temporary.filePath("wall-translation-preview.png")).image !=
                          first.image,
                      "Explicit Move must preview a shared wall-solid translation");
                mouse(view, QEvent::MouseButtonRelease, wall_drag_end, Qt::LeftButton,
                      Qt::NoButton);
                mouse(view, QEvent::MouseButtonPress, {2, 2}, Qt::LeftButton,
                      Qt::LeftButton);
                mouse(view, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton,
                      Qt::NoButton);
                check(capture(view, temporary.filePath("wall-translation-reset.png")).image ==
                          first.image && translated_wall_id == wall_id &&
                          std::isfinite(translated_wall_x) && std::isfinite(translated_wall_y) &&
                          std::isfinite(translated_wall_z) &&
                          (std::abs(translated_wall_x) > 1.0e-9 ||
                           std::abs(translated_wall_y) > 1.0e-9 ||
                           std::abs(translated_wall_z) > 1.0e-9),
                      "shared wall-solid translation must emit one finite semantic request");
                view.onEntityTranslationRequested = {};
                // Choose a solid interior pixel away from the symmetrical centre;
                // an erroneous y flip must not still pass the picking test.
                QPoint native_pick;
                bool found=false;
                for(int y=first.bounds.top()+8;y<first.bounds.center().y() && !found;++y)
                    for(int x=first.bounds.left()+8;x<first.bounds.right()-8;++x) {
                        const auto pixel=first.image.pixelColor(x,y);
                        if(pixel.red()>pixel.blue()+35 && pixel.green()>pixel.blue()+20) {
                            native_pick={x,y};found=true;break;
                        }
                    }
                check(found,"Test needs a visible wall pixel");
                const QPointF pick=QPointF(native_pick)/ratio;
                mouse(view,QEvent::MouseButtonPress,pick,Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,pick,Qt::LeftButton,Qt::NoButton);
                check(selected==wall_id,"Picking must map native pixels to the stable wall ID");
                mouse(view,QEvent::MouseButtonPress,{2,2},Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,{2,2},Qt::LeftButton,Qt::NoButton);
                check(selected.isEmpty(),"Empty-space picking must clear selection");
                const auto unchanged_source = document.snapshot();
                view.setSnapshot(unchanged_source, sketch::visualization::NativeModelView::VisibleEntityIds{});
                check(export_settled(view, temporary.filePath("hidden.png")), "Hidden view must export its current presentation");
                const QImage hidden(temporary.filePath("hidden.png"));
                check(!hidden.isNull(), "Hidden view export must be readable");
                const auto hidden_background = hidden.pixelColor(0, 0);
                for (int y = 0; y < hidden.height(); ++y) for (int x = 0; x < hidden.width(); ++x)
                    check(hidden.pixelColor(x, y) == hidden_background, "Visibility mask must erase native solids");
                mouse(view,QEvent::MouseButtonPress,pick,Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,pick,Qt::LeftButton,Qt::NoButton);
                check(selected.isEmpty(), "Hidden solids must not be pickable");
                const auto replacement = sketch::Document::create({sketch::Entity::create("wall",
                    unchanged_source.entities().at(wall_id.toStdString()).properties)});
                view.setSnapshot(replacement.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{});
                check(export_settled(view, temporary.filePath("hidden-new.png")) &&
                      QImage(temporary.filePath("hidden-new.png")) == hidden,
                      "Newly built solids must also honor an empty visibility mask");
                view.setSnapshot(unchanged_source);
                const auto revealed = capture(view, temporary.filePath("revealed.png"));
                check(revealed.image == first.image, "Removing the filter must restore the same geometry and camera");
                check(document.revision() == unchanged_source.revision() &&
                      document.snapshot().entities() == unchanged_source.entities(), "View filtering must not edit the document");
                {
                    sketch::visualization::NativeModelView initially_hidden;
                    initially_hidden.setAttribute(Qt::WA_DontShowOnScreen, true);
                    initially_hidden.setAttribute(Qt::WA_ShowWithoutActivating, true);
                    initially_hidden.resize(view.size());
                    initially_hidden.setSnapshot(unchanged_source, sketch::visualization::NativeModelView::VisibleEntityIds{});
                    initially_hidden.show();
                    application.processEvents();
                    check(export_settled(initially_hidden, temporary.filePath("initially-hidden.png")) &&
                          QImage(temporary.filePath("initially-hidden.png")) == hidden,
                          "Initial native construction must honor hidden solids");
                    initially_hidden.setSnapshot(unchanged_source);
                    const auto initial_reveal = capture(initially_hidden, temporary.filePath("initial-reveal.png"));
                    check(initial_reveal.image == first.image,
                          "First visible geometry must be fitted after an initially empty view");
                }
                {
                    auto second_wall = unchanged_source.entities().at(wall_id.toStdString());
                    second_wall.id = "partial-mask-second-wall";
                    second_wall.properties["baseline"]["start"] = {12.0, 0.0};
                    second_wall.properties["baseline"]["end"] = {18.0, 0.0};
                    const auto pair = sketch::Document::create({
                        unchanged_source.entities().at(wall_id.toStdString()), second_wall});
                    sketch::visualization::NativeModelView partial;
                    partial.setAttribute(Qt::WA_DontShowOnScreen, true);
                    partial.setAttribute(Qt::WA_ShowWithoutActivating, true);
                    partial.resize(view.size());
                    partial.setSnapshot(pair.snapshot());
                    partial.show();
                    application.processEvents();
                    partial.fitAll();
                    const auto all = capture(partial, temporary.filePath("partial-all.png"));
                    partial.setSnapshot(pair.snapshot(),
                        sketch::visualization::NativeModelView::VisibleEntityIds{second_wall.id});
                    const auto second_only = capture(partial, temporary.filePath("partial-second.png"));
                    check(second_only.image != all.image, "Partial mask must remove only the first wall");
                    QPointF second_pick;
                    bool found_second = false;
                    for (int y = second_only.bounds.top() + 4; y < second_only.bounds.bottom() - 4 && !found_second; ++y)
                        for (int x = second_only.bounds.left() + 4; x < second_only.bounds.right() - 4; ++x) {
                            const auto pixel = second_only.image.pixelColor(x, y);
                            if (pixel.red() > pixel.blue() + 35 && pixel.green() > pixel.blue() + 20) {
                                second_pick = QPointF(x, y) / partial.devicePixelRatioF();
                                found_second = true;
                                break;
                            }
                        }
                    check(found_second, "Partial mask must retain a visible interior wall pixel");
                    QString partial_selected;
                    partial.onEntitySelected = [&](QString id) { partial_selected = std::move(id); };
                    mouse(partial, QEvent::MouseButtonPress, second_pick, Qt::LeftButton, Qt::LeftButton);
                    mouse(partial, QEvent::MouseButtonRelease, second_pick, Qt::LeftButton, Qt::NoButton);
                    check(partial_selected == QString::fromStdString(second_wall.id),
                          "Remaining partial-mask geometry must retain its stable picking identity");
                    partial.setSnapshot(pair.snapshot(),
                        sketch::visualization::NativeModelView::VisibleEntityIds{wall_id.toStdString()});
                    const auto first_only = capture(partial, temporary.filePath("partial-first.png"));
                    check(first_only.image != all.image && first_only.image != second_only.image,
                          "Switching partial masks must independently display each wall");
                    mouse(partial, QEvent::MouseButtonPress, second_pick, Qt::LeftButton, Qt::LeftButton);
                    mouse(partial, QEvent::MouseButtonRelease, second_pick, Qt::LeftButton, Qt::NoButton);
                    check(partial_selected.isEmpty(), "Excluded partial-mask geometry must not be pickable");
                    partial.setSnapshot(pair.snapshot());
                    check(capture(partial, temporary.filePath("partial-restored.png")).image == all.image,
                          "Removing a partial mask must restore both solids without moving the camera");
                }
                for (const std::string kind : {"wall", "roof"}) {
                    auto member_a = unchanged_source.entities().at(wall_id.toStdString());
                    member_a.id = "join-member-a";
                    auto member_b = member_a;
                    member_b.id = "join-member-b";
                    member_b.properties["baseline"]["start"] = {3.0, 0.0};
                    member_b.properties["baseline"]["end"] = {3.0, 4.0};
                    if (kind == "roof") {
                        member_a = sketch::encode_building_entity(sketch::SlopedRoofPanel{
                            member_a.id, {0.0, 0.0, 0.0}, 0.0, 2.0, 4.0,
                            0.0, 0.0, 0.0, 0.2, {}});
                        member_b = sketch::encode_building_entity(sketch::SlopedRoofPanel{
                            member_b.id, {1.9, 0.0, 0.0}, 0.0, 2.0, 4.0,
                            0.0, 0.0, 0.0, 0.2, {}});
                    }
                    const sketch::Entity join{"native-join", kind + "_join",
                        {{"version", 1}, {"style", "fused"},
                         {kind + "_ids", {member_a.id, member_b.id}}}, false,
                        nlohmann::json::object()};
                    const auto joined = sketch::Document::create({member_a, member_b, join});
                    const auto separate = sketch::Document::create({member_a, member_b});
                    const auto joined_source = joined.snapshot();
                    sketch::visualization::NativeModelView joined_view;
                    joined_view.setAttribute(Qt::WA_DontShowOnScreen, true);
                    joined_view.setAttribute(Qt::WA_ShowWithoutActivating, true);
                    joined_view.resize(view.size());
                    joined_view.setSnapshot(joined_source);
                    joined_view.show();
                    application.processEvents();
                    joined_view.fitAll();
                    const auto path = [&](const char* suffix) {
                        return temporary.filePath(QString::fromStdString(kind) + suffix);
                    };
                    const auto fused = capture(joined_view, path("-join-all.png"));
                    joined_view.setSnapshot(joined_source,
                        sketch::visualization::NativeModelView::VisibleEntityIds{join.id, member_a.id});
                    const auto partial_join = capture(joined_view, path("-join-partial.png"));
                    check(partial_join.image != fused.image,
                          "A hidden join member must disappear from the native framebuffer");
                    joined_view.setSnapshot(separate.snapshot(),
                        sketch::visualization::NativeModelView::VisibleEntityIds{member_a.id});
                    check(capture(joined_view, path("-join-reference.png")).image == partial_join.image,
                          "A partially visible join must exactly match its visible source geometry");
                    joined_view.setSnapshot(joined_source,
                        sketch::visualization::NativeModelView::VisibleEntityIds{member_a.id, member_b.id});
                    const auto hidden_join = capture(joined_view, path("-join-hidden.png"));
                    joined_view.setSnapshot(separate.snapshot());
                    check(capture(joined_view, path("-join-sources.png")).image == hidden_join.image,
                          "A hidden join must preserve both visible source solids");
                    joined_view.setSnapshot(joined_source,
                        sketch::visualization::NativeModelView::VisibleEntityIds{join.id});
                    check(export_settled(joined_view, path("-join-members-hidden.png")) &&
                              QImage(path("-join-members-hidden.png")) == hidden,
                          "A visible join must not leak geometry when all members are hidden");
                    joined_view.setSnapshot(joined_source,
                        sketch::visualization::NativeModelView::VisibleEntityIds{});
                    check(export_settled(joined_view, path("-join-all-hidden.png")) &&
                              QImage(path("-join-all-hidden.png")) == hidden,
                          "An empty mask must erase every join and source presentation");
                    joined_view.setSnapshot(joined_source);
                    check(capture(joined_view, path("-join-restored.png")).image == fused.image,
                          "Removing join masks must restore the fused framebuffer and camera");
                    check(joined.snapshot().entities() == joined_source.entities() &&
                              joined.revision() == joined_source.revision(),
                          "Native join masks must not edit their source document");
                }
                sketch::PersistentConstraint horizontal;
                horizontal.id = "native-horizontal";
                horizontal.relation = sketch::ConstraintRelationKind::horizontal;
                horizontal.bindings = {{wall_id.toStdString(), sketch::WallEndpointRole::start},
                                       {wall_id.toStdString(), sketch::WallEndpointRole::end}};
                sketch::ConstraintAuthoringIntent constrain;
                constrain.relation_mutations.push_back(sketch::ConstraintRelationMutation::upsert(horizontal));
                const auto relation_preview = sketch::preview_constraint_authoring(document.snapshot(), constrain);
                check(relation_preview.accepted(), "Native constraint fixture must preview");
                sketch::apply_constraint_authoring(document, relation_preview);
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constrained.png")).image == first.image,
                      "Persistent relations must preserve native geometry and remain renderable");
                sketch::ConstraintAuthoringIntent resize;
                resize.wall_resize = sketch::WallResizeIntent{wall_id.toStdString(), sketch::parse_quantity("7 m"),
                    sketch::WallResizeAnchor::start, true};
                const auto resize_preview = sketch::preview_constraint_authoring(document.snapshot(), resize);
                check(resize_preview.accepted(), "Native constrained resize must preview");
                sketch::apply_constraint_authoring(document, resize_preview);
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constrained-resize.png")).image != first.image,
                      "Constraint Apply must update the real native solid");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constraint-undo.png")).image == first.image,
                      "Undo of a constrained resize must restore native geometry");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                QPointF start(300,230),end=start+QPointF(14,12);
                mouse(view,QEvent::MouseButtonPress,start,Qt::MiddleButton,Qt::MiddleButton);
                mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::MiddleButton);
                mouse(view,QEvent::MouseButtonRelease,end,Qt::MiddleButton,Qt::NoButton);
                auto panned=capture(view,temporary.filePath("panned.png"));
                check(panned.centre.x()>first.centre.x()+5 && panned.centre.y()>first.centre.y()+5,
                      "Pan must move the model with the pointer on both axes");
                view.resize(340,700);
                application.processEvents();
                view.fitAll();
                auto portrait=capture(view,temporary.filePath("portrait.png"));
                check(portrait.bounds.left()>2 && portrait.bounds.right()<portrait.image.width()-3,
                      "Fit must refresh aspect after a portrait resize");
                auto edited_wall=document.snapshot().entities().at(wall_id.toStdString());
                edited_wall.properties["height_m"]=1.4;
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(edited_wall)},{},"Reduce wall height"});
                view.setSnapshot(document.snapshot());
                auto changed=capture(view,temporary.filePath("changed.png"));
                check(changed.image!=portrait.image,"Semantic edits must invalidate derived solids");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto undone=capture(view,temporary.filePath("undone.png"));
                check(undone.image==portrait.image,"Undo must restore the original derived geometry");
                document.redo(document.revision());
                view.setSnapshot(document.snapshot());
                auto redone=capture(view,temporary.filePath("redone.png"));
                check(redone.image==changed.image,"Redo must restore the edited derived geometry");
                auto catalog = sketch::Entity::create("assembly_model", {{"version",1},
                    {"model", sketch::AssemblyModel::create({{"finish","Finish","#e02020"}}, {}, {}).to_json()}});
                auto painted_wall = document.snapshot().entities().at(wall_id.toStdString());
                painted_wall.properties["material_assignment"] = {{"version",1},
                    {"catalog_id",catalog.id},{"material_id","finish"}};
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog),sketch::EntityChange::upsert(painted_wall)},{},"Assign red material"});
                view.setSnapshot(document.snapshot());
                auto red = capture(view,temporary.filePath("material-red.png"));
                check(red.image != redone.image && red.bounds == redone.bounds,
                    "material assignment changes appearance without changing geometry");
                check(view.lastPublicationMetrics().has_value() &&
                      view.lastPublicationMetrics()->created == 0 &&
                      view.lastPublicationMetrics()->reused == 1 &&
                      view.lastPublicationMetrics()->removed == 0,
                      "Assigning material must retain the live wall presentation");
                catalog.properties["model"] = sketch::AssemblyModel::create({{"finish","Finish","#2020e0"}}, {}, {}).to_json();
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog)},{},"Change catalog color"});
                view.setSnapshot(document.snapshot());
                auto blue = capture(view,temporary.filePath("material-blue.png"));
                check(blue.image != red.image && blue.bounds == red.bounds,
                    "catalog color changes must refresh cached presentations");
                check(view.lastPublicationMetrics().has_value() &&
                      view.lastPublicationMetrics()->created == 0 &&
                      view.lastPublicationMetrics()->reused == 1,
                      "Changing a catalog color must reuse the wall's existing AIS presentation");
                catalog.properties["model"] = sketch::AssemblyModel::create({{"finish","Finish"}}, {}, {}).to_json();
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog)},{},"Clear catalog color"});
                view.setSnapshot(document.snapshot());
                auto default_color = capture(view,temporary.filePath("material-default.png"));
                check(default_color.image == redone.image,"clearing a catalog color restores default shading");
                check(view.lastPublicationMetrics().has_value() &&
                      view.lastPublicationMetrics()->created == 0 &&
                      view.lastPublicationMetrics()->reused == 1 &&
                      view.lastPublicationMetrics()->removed == 0,
                      "Clearing material color must update the retained presentation in place");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                check(capture(view,temporary.filePath("material-clear-undo.png")).image == blue.image,
                    "undo restores a cleared color");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto restored_color = capture(view,temporary.filePath("material-undo.png"));
                check(restored_color.image == red.image,"undo restores the prior material appearance");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto unassigned = capture(view,temporary.filePath("material-unassigned.png"));
                check(unassigned.image == redone.image,"removing assignment restores default appearance");
                check_gestures(view, wall_id, temporary);
            }
            if (scenario != "geometry") {
                // Keep the same portrait context as the combined sequence, even
                // when the forms scenario runs in its own guarded process.
                view.resize(340,700);
                application.processEvents();
                const std::vector<sketch::BuildingObject> building_objects{
                    sketch::RectangularColumn{"native-column", {0,0,0}, 0.4,0.5,3.0,0.2},
                    sketch::CircularColumn{"native-round-column", {0,0,0}, 0.25,3.0},
                    sketch::Beam{"native-beam", {0,0,2.5}, {4,1,3}, {0,0,1},0.2,0.35},
                    sketch::StairFlight{"native-stair", {0,0,0},0.0,12,2.4,0.25,1.2,
                        sketch::StairLanding{1.2,0.15}},
                    sketch::Railing{"native-railing", {0,3,0},0.0,4.0,1.1,0.08,1.0},
                    sketch::SlopedRoofPanel{"native-panel", {0,0,3},0.0,4,6,2,
                        std::atan(0.5),0.25,0.15},
                    sketch::GableRoof{"native-gable", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15},
                    sketch::HipRoof{"native-hip", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15},
                    sketch::HipRoof{"native-hip-opening", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15, {{"skylight", -1,-1,2,2}}},
                };
                for (const auto& object : building_objects) {
                    auto entity=sketch::encode_building_entity(object);
                    auto model=sketch::Document::create({entity});
                    view.setSnapshot(model.snapshot());
                    check(ready_settled(view),"Supported building form must have native geometry");
                    view.fitAll();
                    auto frame=capture(view,temporary.filePath(QString::fromStdString(entity.id)+".png"));
                    check(!frame.bounds.isEmpty(),"Every supported form must render a solid");
                    if (entity.type == "column") {
                        QString translated_id;
                        double translated_x = 0.0;
                        double translated_y = 0.0;
                        double translated_z = 0.0;
                        view.onEntityTranslationRequested =
                            [&](QString id, double x, double y, double z) {
                                translated_id = std::move(id);
                                translated_x = x;
                                translated_y = y;
                                translated_z = z;
                            };
                        const auto start = frame.centre / view.devicePixelRatioF();
                        const auto end = start + QPointF(18.0, 12.0);
                        check(view.beginMove(QString::fromStdString(entity.id)), "Column must support explicit Move");
                        mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton,
                              Qt::LeftButton);
                        mouse(view, QEvent::MouseMove, end, Qt::NoButton,
                              Qt::LeftButton);
                        const auto preview = capture(
                            view, temporary.filePath(QString::fromStdString(entity.id) + "-translation-preview.png"));
                        check(preview.image != frame.image,
                              "Explicit Move must visibly preview the native translation");
                        mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton,
                              Qt::NoButton);
                        // Clear hover before comparing with the pre-drag capture.
                        mouse(view, QEvent::MouseButtonPress, {2, 2}, Qt::LeftButton,
                              Qt::LeftButton);
                        mouse(view, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton,
                              Qt::NoButton);
                        check(capture(view, temporary.filePath(QString::fromStdString(entity.id) +
                                                               "-translation-reset.png"))
                                  .image == frame.image,
                              "Releasing a native translation must clear the presentation preview");
                        check(translated_id == QString::fromStdString(entity.id),
                              "Explicit Move must request translation of the selected architectural object");
                        check(std::isfinite(translated_x) && std::isfinite(translated_y) &&
                                  std::isfinite(translated_z) &&
                                  (std::abs(translated_x) > 1.0e-9 ||
                                   std::abs(translated_y) > 1.0e-9 ||
                                   std::abs(translated_z) > 1.0e-9),
                              "Native translation request must contain a finite world-space delta");
                        view.onEntityTranslationRequested = {};
                    }
                    entity.properties["form"]="unsupported-future-form";
                    view.hide();
                    model.apply(sketch::ApplyEntityChanges{model.revision(),
                        {sketch::EntityChange::upsert(entity)},{},"Malformed future form"});
                    view.setSnapshot(model.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{});
                    check(!ready_settled(view) && !view.lastError().isEmpty(),
                          "Invalid hidden building data must still surface a geometry error");
                    check(!export_settled(view, temporary.filePath("invalid.png")),
                          "Invalid building data must block successful image export");
                    model.undo(model.revision());
                    view.setSnapshot(model.snapshot());
                    check(ready_settled(view),"Undo must restore a valid building presentation");
                    view.show();
                }
            }
            if (scenario == "all") check_publication_reuse(view);
            std::cout<<"Native "<<scenario.toStdString()<<" checks passed at DPR "<<ratio<<'\n';
            application.exit(0);
        } catch(const std::exception& error) {
            std::cerr<<error.what()<<'\n';application.exit(1);
        }
    });
    return application.exec();
}
