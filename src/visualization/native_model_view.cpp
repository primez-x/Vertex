#include "sketch/visualization/native_model_view.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/visualization/native_geometry_preparation.hpp"
#include "framebuffer_image.hpp"

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/stair_semantics.hpp"
#include "sketch/assembly_document_adapter.hpp"

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Manipulator.hxx>
#include <AIS_ManipulatorMode.hxx>
#include <AIS_ManipulatorOwner.hxx>
#include <AIS_SelectionScheme.hxx>
#include <Graphic3d_Camera.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ColoredShape.hxx>
#include <BRep_Builder.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Handle.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <OpenGl_View.hxx>
#include <OpenGl_Window.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Compound.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <QLabel>
#include <QFrame>
#include <QRectF>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPointer>
#include <QPointingDevice>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sketch::visualization {
namespace {

bool same_snapshot_content(const DocumentSnapshot& left, const DocumentSnapshot& right) {
    // Snapshots own their history by value; there is no shared immutable storage
    // token to compare. Compare the retained head directly, without hashing asset
    // bytes or serializing the complete history on each shell refresh.
    if (left.entities() != right.entities() || left.assets() != right.assets()) return false;
    // Match Document's exact state comparison: JSON equality alone conflates
    // integer/float values and signed zero, including inside opaque metadata.
    for (const auto& [id, entity] : left.entities()) {
        const auto& other = right.entities().at(id);
        if (entity.properties.dump() != other.properties.dump() ||
            entity.extensions.dump() != other.extensions.dump()) return false;
    }
    for (const auto& [id, asset] : left.assets()) {
        if (asset.metadata.dump() != right.assets().at(id).metadata.dump()) return false;
    }
    return true;
}

QString status_text(std::string_view title, const std::vector<std::string>& messages) {
    QString result = QString::fromUtf8(title.data(), static_cast<int>(title.size()));
    for (const auto& message : messages) {
        result += QStringLiteral("\n• ");
        result += QString::fromStdString(message);
    }
    return result;
}

QString exception_text(const std::exception& error) {
    const auto* message = error.what();
    return (message != nullptr && *message != '\0') ? QString::fromUtf8(message)
                                                      : QStringLiteral("unknown failure");
}

struct NativeInputPoint {
    int x{};
    int y{};
};

qreal input_device_pixel_ratio(const QWidget& widget) noexcept {
    const auto ratio = widget.devicePixelRatioF();
    return std::isfinite(ratio) && ratio > 0.0 ? ratio : 1.0;
}

NativeInputPoint map_input_point(const QWidget& widget, const QPointF& logical_point,
                                 int native_width = 0, int native_height = 0) noexcept {
    const auto ratio = input_device_pixel_ratio(widget);
    if (native_width <= 0) {
        native_width = std::max(1, qRound(widget.width() * ratio));
    }
    if (native_height <= 0) {
        native_height = std::max(1, qRound(widget.height() * ratio));
    }

    const auto native_x = qBound(0, qRound(logical_point.x() * ratio), native_width - 1);
    // QMouseEvent::position() is widget-local with a top-left origin. OCCT's
    // AIS picker and direct V3d mouse helpers use that same pixel convention:
    // AIS selection flips Y internally when projecting, while V3d_View::Convert
    // performs the corresponding top-left to view-space conversion for
    // Pan/ZoomAtPoint. Keep the adapter's point top-left and let each OCCT API
    // apply its own view-space conversion.
    const auto top_left_y = qBound(0, qRound(logical_point.y() * ratio), native_height - 1);
    return NativeInputPoint{native_x, top_left_y};
}

occ::handle<AIS_Shape> prepared_presentation(const PreparedNativeSolid& solid) {
    occ::handle<AIS_Shape> result;
    if (solid.material_regions.empty()) {
        result = new AIS_Shape(solid.shape);
    } else {
        // Region topology belongs to this preparation. A boolean fusion has
        // unrelated face identities, so matching regions to its cached faces
        // cannot reliably refresh material assignments or colors.
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        bool has_region = false;
        for (const auto& region : solid.material_regions) {
            if (region.shape.IsNull()) continue; // Fully occluded roof member.
            builder.Add(compound, region.shape);
            has_region = true;
        }
        if (!has_region) throw std::invalid_argument("native material presentation has no visible regions");
        auto colored = occ::handle<AIS_ColoredShape>(new AIS_ColoredShape(compound));
        for (const auto& region : solid.material_regions)
            if (!region.shape.IsNull()) colored->SetCustomColor(region.shape, region.color);
        result = colored;
    }
    result->SetColor(solid.color);
    result->SetDisplayMode(AIS_Shaded);
    return result;
}

}  // namespace

class NativeModelView::Impl {
public:
    struct CachedSolid {
        // Exact equality avoids reusing stale geometry after a hash collision.
        std::string content;
        std::string appearance_content;
        TopoDS_Shape shape;
        occ::handle<AIS_Shape> presentation;
        Quantity_Color color;
    };

    NativeModelView* owner{};
    QLabel* status_label{};
    QFrame* selection_rectangle{};
    std::shared_ptr<const DocumentSnapshot> snapshot;
    std::shared_ptr<const DocumentSnapshot> published_snapshot;
    std::shared_ptr<const DocumentSnapshot> gesture_snapshot;
    std::shared_ptr<const DocumentSnapshot> commit_snapshot;
    std::optional<NativeModelView::VisibleEntityIds> visible_ids;
    NativeGeometryRegenerator regenerator;
    QTimer* preparation_timer{};
    std::optional<PreparedNativeGeometry> prepared_geometry;
    std::optional<NativeModelView::PublicationMetrics> publication_metrics;
    bool geometry_prepared{};
    QString native_error;
    QString geometry_status;
    QString operation_error;
    QString input_error;
    QString export_error;
    bool native_attempted{};
    bool native_ready{};
    bool has_fit{};
    bool fit_requested{};
    bool initial_fit_pending{};

    occ::handle<Aspect_DisplayConnection> display_connection;
    occ::handle<OpenGl_GraphicDriver> graphic_driver;
    occ::handle<V3d_Viewer> viewer;
    occ::handle<V3d_View> view;
    occ::handle<AIS_InteractiveContext> context;
    occ::handle<AIS_Manipulator> manipulator;
    occ::handle<WNT_Window> window;
    std::map<std::string, CachedSolid, std::less<>> solids;
    QStringList selected_entity_ids;
    std::optional<std::string> selected_entity_id;
    std::optional<std::string> manipulator_entity_id;
    std::optional<gp_Trsf> manipulation_transform;
    std::uint64_t navigation_generation{};

    struct SelectionCapture {
        std::shared_ptr<const DocumentSnapshot> source;
        Graphic3d_WorldViewProjState camera;
        QSize logical_size;
        QSize native_size;
        qreal pixel_ratio{};
        QStringList selection;
        std::uint64_t navigation_generation{};
    };
    std::optional<SelectionCapture> selection_capture;

    struct WheelScroll {
        SelectionCapture context;
        QPointer<const QPointingDevice> device;
        QPointer<QScreen> screen;
        QPoint anchor;
        double remainder{};
        std::chrono::steady_clock::time_point last_event;
        bool pixel_input{};
    };
    std::optional<WheelScroll> wheel_scroll;

    enum class Gesture { none, select, additive_select, overlap_select, overlap_pan, edit, pan, orbit, move, manipulate };
    Gesture gesture = Gesture::none;
    Qt::MouseButton initiating_button = Qt::NoButton;
    bool tablet_active{};
    Qt::MouseButton tablet_button = Qt::NoButton;
    QPointer<const QPointingDevice> tablet_device;
    int tablet_dispatch_depth{};
    bool tablet_dispatch_retired{};
    QPointer<const QPointingDevice> tablet_dispatch_device;
    Qt::MouseButton tablet_dispatch_button = Qt::NoButton;
    std::chrono::steady_clock::time_point tablet_mouse_suppression_until;
    QPoint navigation_start;
    QPointF left_press;
    bool left_moved{};
    bool space_pan_armed{};
    std::optional<std::string> translation_entity_id;
    std::vector<std::string> translation_preview_ids;
    struct WorldPoint {
        double x{};
        double y{};
        double z{};
    };
    std::optional<WorldPoint> translation_start;

    explicit Impl(NativeModelView* widget) : owner(widget) {
        preparation_timer = new QTimer(widget);
        preparation_timer->setInterval(10);
        QObject::connect(preparation_timer, &QTimer::timeout, widget,
                         [this] { owner->pollGeometryPreparation(); });
    }

    NativeInputPoint input_point(const QPointF& logical_point) const {
        int native_width = 0;
        int native_height = 0;
        if (!window.IsNull()) {
            window->Size(native_width, native_height);
        }
        return map_input_point(*owner, logical_point, native_width, native_height);
    }

    qreal input_scale() const noexcept { return input_device_pixel_ratio(*owner); }

    void navigation_changed() noexcept {
        // Do not wrap: at exhaustion selection fails closed rather than letting
        // an old capture become current again. Camera ownership is unchanged.
        if (navigation_generation != std::numeric_limits<std::uint64_t>::max())
            ++navigation_generation;
    }

    void synchronize_native_size() {
        if (!native_ready || view.IsNull() || window.IsNull()) return;
        int native_width = 0;
        int native_height = 0;
        window->Size(native_width, native_height);
        // A minimized native client has no useful camera aspect or framebuffer.
        if (native_width <= 0 || native_height <= 0) return;
        const auto gl_view = occ::handle<OpenGl_View>::DownCast(view->View());
        if (gl_view.IsNull() || gl_view->GlWindow().IsNull()) return;
        if (gl_view->GlWindow()->Width() == native_width &&
            gl_view->GlWindow()->Height() == native_height) return;

        // Qt can deliver resizeEvent before it applies the final child HWND
        // geometry (notably when showing a hidden splitter pane). OCCT caches
        // the OpenGL window extent separately from WNT_Window's live client
        // size. Recheck at paint/show boundaries so the first settled frame
        // updates both that extent and the camera aspect, without refitting or
        // losing the user's pan, zoom, orbit, selection, or manipulation state.
        navigation_changed();
        view->MustBeResized();
    }

    void fit_all() {
        if (!native_ready || view.IsNull() || window.IsNull()) {
            return;
        }

        // WNT_Window reports the physical client size. Refresh both the
        // OpenGL viewport and the camera aspect immediately before fitting so
        // a late QWidget/DPI resize cannot leave FitAll using an old aspect.
        navigation_changed();
        view->MustBeResized();
        int native_width = 0;
        int native_height = 0;
        window->Size(native_width, native_height);
        if (native_width <= 0 || native_height <= 0) {
            show_operation_error(QStringLiteral(
                "Native OCCT 3D viewport has no usable pixel dimensions"));
            return;
        }
        view->FitAll(0.05, true);
        has_fit = true;
        initial_fit_pending = false;
    }

    void complete_initial_fit() noexcept {
        if (!initial_fit_pending || !owner->isVisible() || !native_ready ||
            window.IsNull() || !geometry_prepared || regenerator.is_pending() || prepared_geometry) return;
        int native_width = 0;
        int native_height = 0;
        window->Size(native_width, native_height);
        if (native_width <= 0 || native_height <= 0) return;
        try {
            fit_all();
        } catch (const Standard_Failure& error) {
            show_operation_error(QStringLiteral("Initial 3D fit failed: ") + exception_text(error));
        } catch (const std::exception& error) {
            show_operation_error(QStringLiteral("Initial 3D fit failed: ") + exception_text(error));
        } catch (...) {
            show_operation_error(QStringLiteral("Initial 3D fit failed: unknown failure"));
        }
    }

    void schedule_initial_fit() {
        initial_fit_pending = true;
        // showEvent precedes the shell's splitter setSizes call. Fit on the
        // settled paint/event-loop boundary, after the HWND receives its final
        // aspect. This one-shot fit never runs after subsequent navigation.
        owner->update();
        QTimer::singleShot(0, owner, [this] { complete_initial_fit(); });
    }

    void notify_error(const QString& text) noexcept {
        // Observers do not own preparation or publication state. In particular,
        // a throwing failure observer must not replace the real diagnostic.
        try {
            // Retain the callable while dispatching: the observer may replace
            // its own registration during the callback.
            const auto callback = owner->onError;
            if (callback) callback(text);
        } catch (...) {
        }
    }

    void show_status(const QString& text, bool report_error = true) {
        geometry_status = text;
        operation_error.clear();
        input_error.clear();
        export_error.clear();
        refresh_status_label();
        const QPointer<NativeModelView> owner_guard(owner);
        try {
            const auto callback = owner->onGeometryStatusChanged;
            if (callback) callback(text);
        } catch (...) {
            // A progress observer cannot prevent scheduling or publication.
        }
        if (owner_guard && report_error && !text.isEmpty()) notify_error(text);
    }

    void show_native_error(const QString& text) {
        native_error = text;
        refresh_status_label();
        notify_error(text);
    }

    void show_operation_error(const QString& text) {
        operation_error = text;
        refresh_status_label();
        notify_error(text);
    }

    void show_input_error(const QString& text) {
        // Called after cancellation restores the published scene. An observer
        // refusing this input does not invalidate geometry or a later gesture.
        input_error = text;
        refresh_status_label();
        notify_error(text);
    }

    void show_export_error(const QString& text) {
        // A refused destination or image writer failure does not invalidate
        // the prepared model, viewport or a restored manipulation gesture.
        export_error = text;
        refresh_status_label();
        notify_error(text);
    }

    void refresh_status_label() {
        const auto text = !native_error.isEmpty()
                              ? native_error
                              : (!geometry_status.isEmpty() ? geometry_status :
                                 (!operation_error.isEmpty() ? operation_error :
                                  (!input_error.isEmpty() ? input_error : export_error)));
        status_label->setText(text);
        status_label->setVisible(!text.isEmpty());
        status_label->raise();
        auto bounds = owner->rect().adjusted(12, 12, -12, -12);
        if ((regenerator.is_pending() || !input_error.isEmpty() || !export_error.isEmpty()) &&
            native_error.isEmpty() && operation_error.isEmpty()) {
            // Keep the valid scene visible during preparation or a recovered
            // input/export failure; a diagnostic must not cover the viewport.
            bounds.setHeight(std::min(bounds.height(), status_label->sizeHint().height()));
        }
        status_label->setGeometry(bounds);
    }

    void rebuild_snapshot() {
        if (!snapshot) return;
        publication_metrics.reset();
        prepared_geometry.reset();
        geometry_prepared = false;
        regenerator.request(*snapshot, visible_ids);
        const QPointer<NativeModelView> owner_guard(owner);
        show_status(QStringLiteral("Preparing 3D geometry…"), false);
        if (!owner_guard) return;
        preparation_timer->start();
    }

    void collect_prepared_geometry() {
        const QPointer<NativeModelView> owner_guard(owner);
        try {
            if (auto completed = regenerator.take_completed()) prepared_geometry = std::move(completed);
            if (!regenerator.is_pending()) preparation_timer->stop();
            if (!prepared_geometry || !snapshot ||
                prepared_geometry->revision != snapshot->revision() ||
                prepared_geometry->visible_ids != visible_ids) return;
            auto& prepared = prepared_geometry;
            if (!prepared->errors.empty() || !prepared->pending.empty()) {
                auto messages = prepared->errors;
                messages.insert(messages.end(), prepared->pending.begin(), prepared->pending.end());
                const auto text = status_text(prepared->errors.empty() ? "3D geometry pending:"
                                                                      : "3D geometry is incomplete:", messages);
                geometry_prepared = false;
                prepared_geometry.reset();
                show_status(text);
                return; // Preserve the previous complete scene on invalid geometry.
            }
            const bool newly_prepared = !geometry_prepared;
            geometry_prepared = true;
            // Preparation is independent of a visible/native window. Keep a
            // completed candidate until Qt initializes the presentation owner.
            if (!native_ready) {
                if (newly_prepared) show_status(QString());
                return;
            }

            // All expensive semantic reconstruction has finished. AIS and its
            // context remain strictly on this widget's thread. Build candidates
            // before removing any previous valid presentation.
            const auto publication_started = std::chrono::steady_clock::now();
            NativeModelView::PublicationMetrics metrics;
            std::map<std::string, CachedSolid, std::less<>> replacement;
            for (auto& [id, solid] : prepared->solids) {
                const auto cached = solids.find(id);
                const bool same_geometry = cached != solids.end() && cached->second.content == solid.content;
                if (same_geometry && cached->second.appearance_content == solid.appearance_content) {
                    // Retain the live topology as well as the AIS handle. The
                    // worker's fresh triangulation must never replace or mutate
                    // a shape already owned by an unchanged presentation.
                    auto retained = cached->second;
                    retained.color = solid.color;
                    replacement.emplace(id, std::move(retained));
                    ++metrics.reused;
                    continue;
                }
                auto presentation = prepared_presentation(solid);
                // Appearance-only publication consumes fresh region topology,
                // while the retained fused/assembly shape remains authoritative.
                auto geometry_shape = same_geometry ? cached->second.shape : std::move(solid.shape);
                replacement.emplace(id, CachedSolid{std::move(solid.content), std::move(solid.appearance_content),
                                                   std::move(geometry_shape),
                                                   presentation, solid.color});
                ++metrics.created;
            }
            std::map<std::string, bool, std::less<>> previous_visibility;
            for (const auto& [id, solid] : solids) {
                previous_visibility.emplace(id, context->IsDisplayed(solid.presentation));
                const auto next = replacement.find(id);
                if (next == replacement.end() || next->second.presentation != solid.presentation)
                    ++metrics.removed;
            }
            auto published_source = std::make_shared<const DocumentSnapshot>(*snapshot);
            const bool had_solids = !solids.empty();
            const bool previously_fit = has_fit;
            const bool previously_pending_fit = initial_fit_pending;
            bool defer_initial_fit = false;
            // A pending edit cannot move a stale displayed object in the new
            // snapshot. Reset any preview before updating the scene.
            clear_translation_preview();
            detach_manipulator();
            try {
                for (const auto& [id, solid] : replacement) {
                    const auto old = solids.find(id);
                    if (old != solids.end() && old->second.presentation == solid.presentation &&
                        old->second.color != solid.color) {
                        // Context updates refresh an existing presentation's
                        // aspects, including restoration of its default color.
                        context->SetColor(solid.presentation, solid.color, false);
                    }
                    const bool visible = prepared->solids.at(id).visible;
                    if (visible != context->IsDisplayed(solid.presentation)) {
                        if (visible) context->Display(solid.presentation, false);
                        else context->Erase(solid.presentation, false);
                    }
                }
                // All candidates have been displayed successfully before any
                // obsolete object is detached. Unchanged handles stay registered.
                for (const auto& [id, solid] : solids) {
                    const auto next = replacement.find(id);
                    if (next == replacement.end() || next->second.presentation != solid.presentation)
                        context->Remove(solid.presentation, false);
                }
                // A changed regional appearance replaces its AIS handle; keep
                // the semantic selection attached to the new root presentation.
                context->ClearSelected(false);
                for (const auto& id : selected_entity_ids) {
                    const auto selected = replacement.find(id.toStdString());
                    if (selected != replacement.end() && context->IsDisplayed(selected->second.presentation))
                        context->AddOrRemoveSelected(selected->second.presentation, false);
                }
                const bool has_visible_solids = std::any_of(prepared->solids.begin(), prepared->solids.end(),
                    [](const auto& entry) { return entry.second.visible; });
                if (has_visible_solids && fit_requested) {
                    fit_all();
                    if (!owner_guard) return;
                }
                else if (has_visible_solids && (!has_fit || !had_solids)) defer_initial_fit = true;
                else if (!has_visible_solids) {
                    if (replacement.empty()) has_fit = false;
                    initial_fit_pending = false;
                }
                viewer->Redraw();
            } catch (...) {
                // Keep the authoritative cache until publication and redraw
                // succeed. Best-effort rollback also restores reused objects
                // changed before a later context operation failed. A failing
                // driver must not mask the original publication diagnostic.
                for (const auto& [id, solid] : replacement) {
                    const auto old = solids.find(id);
                    if (old == solids.end() || old->second.presentation != solid.presentation) {
                        try { context->Remove(solid.presentation, false); } catch (...) {}
                    }
                }
                for (const auto& [id, solid] : solids) {
                    try {
                        const auto next = replacement.find(id);
                        if (next != replacement.end() && next->second.presentation == solid.presentation &&
                            next->second.color != solid.color)
                            context->SetColor(solid.presentation, solid.color, false);
                        if (previous_visibility.at(id)) context->Display(solid.presentation, false);
                        else context->Erase(solid.presentation, false);
                    } catch (...) {}
                }
                has_fit = previously_fit;
                initial_fit_pending = previously_pending_fit;
                try {
                    context->ClearSelected(false);
                    for (const auto& id : selected_entity_ids) {
                        const auto selected = solids.find(id.toStdString());
                        if (selected != solids.end() && previous_visibility.at(selected->first))
                            context->AddOrRemoveSelected(selected->second.presentation, false);
                    }
                } catch (...) {}
                try { attach_manipulator(); } catch (...) {}
                try { viewer->Redraw(); } catch (...) {}
                throw;
            }
            solids.swap(replacement);
            published_snapshot = std::move(published_source);
            fit_requested = false;
            prepared_geometry.reset();
            metrics.elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - publication_started).count();
            publication_metrics = metrics;
            show_status(QString());
            if (!owner_guard) return;
            if (defer_initial_fit) schedule_initial_fit();
            try {
                attach_manipulator();
            } catch (const Standard_Failure& error) {
                detach_manipulator();
                show_operation_error(QStringLiteral("3D transform controls unavailable: ") +
                                     exception_text(error));
            } catch (const std::exception& error) {
                detach_manipulator();
                show_operation_error(QStringLiteral("3D transform controls unavailable: ") +
                                     exception_text(error));
            } catch (...) {
                detach_manipulator();
                show_operation_error(
                    QStringLiteral("3D transform controls unavailable: unknown failure"));
            }
        } catch (const Standard_Failure& error) {
            preparation_timer->stop();
            prepared_geometry.reset();
            show_status(QStringLiteral("3D geometry preparation failed: ") + exception_text(error));
        } catch (const std::exception& error) {
            preparation_timer->stop();
            prepared_geometry.reset();
            show_status(QStringLiteral("3D geometry preparation failed: ") + exception_text(error));
        } catch (...) {
            preparation_timer->stop();
            prepared_geometry.reset();
            show_status(QStringLiteral("3D geometry preparation failed: unknown failure"));
        }
    }

    void initialize_native_view() {
        if (native_attempted || native_ready) {
            return;
        }
        native_attempted = true;
        const QPointer<NativeModelView> owner_guard(owner);
        try {
            const auto platform_name = QGuiApplication::platformName().toLower();
            if (platform_name == QStringLiteral("offscreen") ||
                platform_name == QStringLiteral("minimal")) {
                show_native_error(QStringLiteral(
                                      "Native OCCT 3D view unavailable on Qt platform '%1'")
                                      .arg(platform_name));
                return;
            }
            owner->setAttribute(Qt::WA_NativeWindow, true);
            owner->setAttribute(Qt::WA_PaintOnScreen, true);
            owner->setAttribute(Qt::WA_NoSystemBackground, true);
            owner->setAttribute(Qt::WA_OpaquePaintEvent, true);
            const auto native_id = owner->winId();
            if (native_id == 0) {
                throw std::runtime_error("Qt did not provide a native window handle");
            }
            display_connection = occ::handle<Aspect_DisplayConnection>(new Aspect_DisplayConnection());
            graphic_driver = occ::handle<OpenGl_GraphicDriver>(
                new OpenGl_GraphicDriver(display_connection, true));
            if (graphic_driver.IsNull()) {
                throw std::runtime_error("Open CASCADE OpenGL driver could not be created");
            }
            viewer = occ::handle<V3d_Viewer>(new V3d_Viewer(graphic_driver));
            context = occ::handle<AIS_InteractiveContext>(new AIS_InteractiveContext(viewer));
            view = occ::handle<V3d_View>(viewer->CreateView());
            if (view.IsNull() || context.IsNull()) {
                throw std::runtime_error("Open CASCADE viewer context could not be created");
            }
#ifdef _WIN32
            const auto aspect_handle = reinterpret_cast<Aspect_Handle>(native_id);
#else
            const auto aspect_handle = static_cast<Aspect_Handle>(native_id);
#endif
            window = occ::handle<WNT_Window>(new WNT_Window(aspect_handle));
            view->SetWindow(window);
            // Qt owns visibility. Mapping the external HWND here overrides
            // WA_DontShowOnScreen and can expose automated test windows.
            viewer->SetDefaultLights();
            viewer->SetLightOn();
            view->SetBackgroundColor(Quantity_Color(0.09, 0.11, 0.14, Quantity_TOC_RGB));
            view->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);
            navigation_changed();
            view->SetProj(V3d_XposYnegZpos, false);
            view->MustBeResized();
            native_ready = true;
            native_error.clear();
            operation_error.clear();
            if (snapshot) {
                collect_prepared_geometry();
                if (!owner_guard) return;
            } else {
                refresh_status_label();
            }
            view->Redraw();
        } catch (const Standard_Failure& error) {
            show_native_error(QStringLiteral("Native OCCT 3D view unavailable: ") +
                              exception_text(error));
        } catch (const std::exception& error) {
            show_native_error(QStringLiteral("Native OCCT 3D view unavailable: ") +
                              exception_text(error));
        } catch (...) {
            show_native_error(QStringLiteral("Native OCCT 3D view unavailable: unknown failure"));
        }
    }

    template <typename PresentationHandle>
    QString entity_id_for_presentation(const PresentationHandle& selected) const {
        if (selected.IsNull()) {
            return {};
        }
        for (const auto& [id, solid] : solids) {
            if (solid.presentation == selected && !context.IsNull() && context->IsDisplayed(solid.presentation)) {
                return QString::fromStdString(id);
            }
        }
        return {};
    }

    bool supports_direct_translation(const QString& id) const {
        return supports_direct_transform(id.toStdString());
    }

    bool supports_direct_transform(const std::string& id) const {
        if (!snapshot || !snapshot->is_editable() || id.empty()) return false;
        const auto found = snapshot->entities().find(id);
        if (found == snapshot->entities().end()) {
            // Catalog-owned geometric instances use derived root IDs. Their
            // controller edits the catalog instance; legacy host copies do
            // not gain independent transform authority.
            for (const auto& [catalog_id, entity] : snapshot->entities()) {
                if (entity.type != "assembly_model" || !entity.properties.contains("model")) continue;
                const auto prefix = catalog_id + ":instance:";
                if (!id.starts_with(prefix)) continue;
                try {
                    const auto model = AssemblyModel::from_json(entity.properties.at("model"));
                    const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
                        [&](const auto& value) { return value.id == id.substr(prefix.size()); });
                    return instance != model.instances().end() && !model.expand(instance->id).profiles.empty();
                } catch (...) { return false; }
            }
            return false;
        }
        if (found->second.type == "assembly_instance") {
            try {
                // The strict envelope requires independent world placement and
                // excludes legacy catalog-host copies from direct manipulation.
                (void)decode_document_assembly_instance(found->second);
                return true;
            } catch (...) { return false; }
        }
        if (!can_transform_architectural_entity_type(found->second.type)) return false;
        if (found->second.type == "railing") {
            try {
                const auto rail = decode_railing_properties(id, found->second.properties);
                // Hosted guards are placed by their stair. The shell commits
                // one selected host, so a standalone guard cannot transform.
                return !rail.host && !rail.landing_host;
            } catch (...) { return false; }
        }
        return true;
    }

    std::vector<std::string> transform_presentation_ids(const std::string& host_id) const {
        std::vector<std::string> result{host_id};
        if (!snapshot || context.IsNull()) return result;
        const auto host = snapshot->entities().find(host_id);
        if (host == snapshot->entities().end()) return result;
        for (const auto& [id, entity] : snapshot->entities()) {
            bool dependent = false;
            if (host->second.type == "wall" && entity.type == "opening") {
                std::string wall_id, error;
                dependent = read_document_wall_id(entity, wall_id, error) && wall_id == host_id;
            } else if (host->second.type == "stair" && entity.type == "railing") {
                try {
                    const auto rail = decode_railing_properties(id, entity.properties);
                    dependent = (rail.host && rail.host->stair_id == host_id) ||
                                (rail.landing_host && rail.landing_host->stair_id == host_id);
                } catch (...) { }
            }
            const auto solid = solids.find(id);
            if (dependent && solid != solids.end() && !solid->second.presentation.IsNull() &&
                context->IsDisplayed(solid->second.presentation)) result.push_back(id);
        }
        return result;
    }

    void detach_manipulator() noexcept {
        manipulation_transform.reset();
        if (!manipulator.IsNull()) {
            try {
                if (manipulator->HasActiveTransformation())
                    manipulator->StopTransform(false);
                manipulator->DeactivateCurrentMode();
                if (manipulator->IsAttached()) manipulator->Detach();
            } catch (...) {
            }
        }
        manipulator_entity_id.reset();
    }

    void attach_manipulator() {
        if (selected_entity_ids.size() != 1 || !selected_entity_id.has_value() || !supports_direct_transform(*selected_entity_id) ||
            !native_ready || !geometry_prepared || regenerator.is_pending() || prepared_geometry ||
            !geometry_status.isEmpty() || context.IsNull() || viewer.IsNull()) {
            detach_manipulator();
            return;
        }
        const auto found = solids.find(*selected_entity_id);
        if (found == solids.end() || found->second.presentation.IsNull() ||
            !context->IsDisplayed(found->second.presentation)) {
            detach_manipulator();
            return;
        }
        if (manipulator_entity_id == selected_entity_id && !manipulator.IsNull() &&
            manipulator->IsAttached()) return;

        detach_manipulator();
        if (manipulator.IsNull()) {
            manipulator = occ::handle<AIS_Manipulator>(new AIS_Manipulator());
            manipulator->SetModeActivationOnDetection(true);
            manipulator->SetZoomPersistence(true);
            manipulator->SetSkinMode(AIS_Manipulator::ManipulatorSkin_Flat);
            manipulator->SetSize(80.0f);
            manipulator->SetGap(6.0f);
            // Architectural document transforms support world translation,
            // signed rotation around Z, and uniform scaling. Hide operations
            // the authoritative model cannot reproduce exactly.
            manipulator->SetPart(0, AIS_MM_Rotation, false);
            manipulator->SetPart(1, AIS_MM_Rotation, false);
            manipulator->SetPart(2, AIS_MM_Rotation, true);
            manipulator->SetPart(AIS_MM_TranslationPlane, false);
        }
        AIS_Manipulator::OptionsForAttach options;
        options.SetAdjustPosition(true).SetAdjustSize(false).SetEnableModes(true);
        auto group = occ::handle<NCollection_HSequence<occ::handle<AIS_InteractiveObject>>>(
            new NCollection_HSequence<occ::handle<AIS_InteractiveObject>>());
        for (const auto& id : transform_presentation_ids(*selected_entity_id))
            group->Append(solids.at(id).presentation);
        manipulator->Attach(group, options);
        manipulator_entity_id = selected_entity_id;
        context->ClearSelected(false);
        context->AddOrRemoveSelected(found->second.presentation, false);
        viewer->Redraw();
    }

    bool begin_manipulation(const NativeInputPoint point) {
        if (manipulator.IsNull() || !manipulator->IsAttached() ||
            manipulator_entity_id != selected_entity_id || context.IsNull() || view.IsNull())
            return false;
        try {
            context->MoveTo(point.x, point.y, view, false);
            if (!manipulator->HasActiveMode()) return false;
            manipulator->StartTransform(point.x, point.y, view);
            manipulation_transform.reset();
            return true;
        } catch (const Standard_Failure& error) {
            show_operation_error(QStringLiteral("3D transform could not start: ") +
                                 exception_text(error));
        } catch (const std::exception& error) {
            show_operation_error(QStringLiteral("3D transform could not start: ") +
                                 exception_text(error));
        } catch (...) {
            show_operation_error(QStringLiteral("3D transform could not start: unknown failure"));
        }
        return false;
    }

    void preview_manipulation(const NativeInputPoint point) {
        if (manipulator.IsNull() || !manipulator->HasActiveTransformation() || view.IsNull()) return;
        try {
            manipulation_transform = manipulator->Transform(point.x, point.y, view);
            if (!viewer.IsNull()) viewer->RedrawImmediate();
        } catch (const Standard_Failure& error) {
            show_operation_error(QStringLiteral("3D transform preview failed: ") +
                                 exception_text(error));
        } catch (const std::exception& error) {
            show_operation_error(QStringLiteral("3D transform preview failed: ") +
                                 exception_text(error));
        } catch (...) {
            show_operation_error(QStringLiteral("3D transform preview failed: unknown failure"));
        }
    }

    std::optional<WorldPoint> world_point(const NativeInputPoint point) const {
        if (!native_ready || view.IsNull()) {
            return std::nullopt;
        }
        try {
            WorldPoint result;
            view->Convert(point.x, point.y, result.x, result.y, result.z);
            if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
                !std::isfinite(result.z)) {
                return std::nullopt;
            }
            return result;
        } catch (const Standard_Failure&) {
            return std::nullopt;
        } catch (const std::exception&) {
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    void clear_translation_preview() {
        for (const auto& id : translation_preview_ids) {
            const auto found = solids.find(id);
            if (found != solids.end() && !found->second.presentation.IsNull()) {
                found->second.presentation->ResetTransformation();
                if (native_ready && !context.IsNull()) {
                    context->Redisplay(found->second.presentation, false);
                }
            }
        }
        translation_preview_ids.clear();
    }

    void preview_translation(const WorldPoint& current) {
        if (!translation_entity_id.has_value() || !translation_start.has_value()) {
            return;
        }
        const auto found = solids.find(*translation_entity_id);
        if (found == solids.end() || found->second.presentation.IsNull()) {
            return;
        }
        const auto dx = current.x - translation_start->x;
        const auto dy = current.y - translation_start->y;
        const auto dz = current.z - translation_start->z;
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
            return;
        }
        gp_Trsf transform;
        transform.SetTranslation(gp_Vec(dx, dy, dz));
        for (const auto& id : translation_preview_ids) {
            const auto member = solids.find(id);
            if (member == solids.end() || member->second.presentation.IsNull()) continue;
            member->second.presentation->SetLocalTransformation(transform);
            if (native_ready && !context.IsNull()) {
                context->Redisplay(member->second.presentation, false);
            }
        }
        if (native_ready && !viewer.IsNull()) {
            viewer->RedrawImmediate();
        }
    }

    void refuse_overlap_input(const QString& message) noexcept {
        const QPointer<NativeModelView> guard(owner);
        try { owner->cancelInteraction(); } catch (...) {}
        if (guard) { try { show_input_error(message); } catch (...) {} }
    }

    void begin_overlap_selection() noexcept {
        const QPointer<NativeModelView> guard(owner);
        try {
            owner->resetInteraction(true,true);
            detach_manipulator();
            view->Redraw();
            if (!owner->claimPointer(Qt::LeftButton)) return;
            selection_capture=capture_selection();
            gesture_snapshot=published_snapshot;
            gesture=Gesture::overlap_select;
        } catch (const Standard_Failure& error) {
            if (guard) refuse_overlap_input(QStringLiteral("3D overlap selection failed: ")+exception_text(error));
        } catch (const std::exception& error) {
            if (guard) refuse_overlap_input(QStringLiteral("3D overlap selection failed: ")+exception_text(error));
        } catch (...) {
            if (guard) refuse_overlap_input(QStringLiteral("3D overlap selection failed: unknown failure"));
        }
    }

    void pan_overlap_drag(const NativeInputPoint point, bool finishing=false) noexcept {
        const QPointer<NativeModelView> guard(owner);
        try {
            if (gesture==Gesture::overlap_select) {
                if (!selection_capture || !selection_current(*selection_capture))
                    throw std::invalid_argument("The displayed 3D source, selection or camera changed. Start again.");
                const auto press=input_point(left_press);
                navigation_start=QPoint(press.x,press.y);
                // Transfer only this owned Alt gesture to camera input. Never
                // reset an ongoing pan when its navigation epoch advances.
                selection_capture.reset();
                gesture_snapshot.reset();
                gesture=Gesture::overlap_pan;
                navigation_changed();
                view->Pan(0,0,1.0,true);
                owner->setCursor(Qt::ClosedHandCursor);
            }
            const auto delta=QPoint(point.x,point.y)-navigation_start;
            navigation_changed();
            view->Pan(delta.x(),-delta.y(),1.0,false);
            if (finishing) owner->resetInteraction(true);
        } catch (const Standard_Failure& error) {
            if (guard) refuse_overlap_input(QStringLiteral("3D Alt pan failed: ")+exception_text(error));
        } catch (const std::exception& error) {
            if (guard) refuse_overlap_input(QStringLiteral("3D Alt pan failed: ")+exception_text(error));
        } catch (...) {
            if (guard) refuse_overlap_input(QStringLiteral("3D Alt pan failed: unknown failure"));
        }
    }

    SelectionCapture capture_selection() const {
        int width=0, height=0;
        if (!window.IsNull()) window->Size(width,height);
        return {published_snapshot,view->Camera()->WorldViewProjState(),owner->size(),
                QSize(width,height),input_scale(),selected_entity_ids,navigation_generation};
    }

    bool selection_current(const SelectionCapture& capture) const {
        if (!owner->isVisible() || !owner->isReady() || !capture.source || published_snapshot != capture.source ||
            capture.navigation_generation != navigation_generation ||
            navigation_generation == std::numeric_limits<std::uint64_t>::max() ||
            view.IsNull() || view->Camera().IsNull() || owner->size() != capture.logical_size ||
            input_scale() != capture.pixel_ratio || selected_entity_ids != capture.selection ||
            view->Camera()->WorldViewProjState() != capture.camera) return false;
        int width=0, height=0;
        if (!window.IsNull()) window->Size(width,height);
        return QSize(width,height)==capture.native_size;
    }

    void restore_selection_highlights() {
        if (context.IsNull()) return;
        context->ClearSelected(false);
        for (const auto& id : selected_entity_ids) {
            const auto found=solids.find(id.toStdString());
            if (found!=solids.end() && !found->second.presentation.IsNull() &&
                context->IsDisplayed(found->second.presentation))
                context->AddOrRemoveSelected(found->second.presentation,false);
        }
    }

    std::optional<QString> select_at(const NativeInputPoint point, const SelectionCapture& capture,
                                      bool editing=false, bool toggle=false, bool cycle=false) {
        const QPointer<NativeModelView> guard(owner);
        try {
            if (!selection_current(capture) || !owner->admitSceneInput(false) || !guard ||
                !selection_current(capture)) return std::nullopt;
            context->MoveTo(point.x,point.y,view,false);
            QString id;
            if (cycle && !toggle) {
                // OCCT's detected sequence is ordered by the actual pick. A
                // material/face owner may repeat the same semantic object.
                QStringList targets;
                std::set<QString> seen;
                for (context->InitDetected();context->MoreDetected();context->NextDetected()) {
                    const auto detected=context->DetectedCurrentOwner();
                    if (detected.IsNull() || !detected->HasSelectable()) continue;
                    const auto target=entity_id_for_presentation(
                        occ::handle<AIS_InteractiveObject>::DownCast(detected->Selectable()));
                    if (!target.isEmpty() && seen.insert(target).second) targets.append(target);
                }
                if (!targets.isEmpty()) {
                    const auto primary=capture.selection.isEmpty() ? QString{} : capture.selection.back();
                    id=targets.at((targets.indexOf(primary)+1)%targets.size());
                }
            } else if (context->HasDetected()) id=entity_id_for_presentation(context->DetectedInteractive());
            if ((editing || toggle || cycle) && id.isEmpty()) return QString{};
            if (!selection_current(capture)) return std::nullopt;
            // Preserve selected groups on their first plain click, as Qt sends
            // that release before a possible double-click. Context uses this
            // same policy; its selected member never replaces the group.
            const bool retained=!toggle && !cycle && !id.isEmpty() && selected_entity_ids.contains(id);
            if (!retained) {
                auto next=toggle ? selected_entity_ids : QStringList{};
                if (toggle && next.contains(id)) next.removeAll(id);
                else if (!id.isEmpty()) next.append(id);
                const auto clicked=owner->onEntitySelectionClicked;
                const auto legacy=owner->onEntitySelected;
                owner->setSelectedEntities(next);
                if (!guard) return id;
                if (clicked) clicked(id,toggle);
                else if (legacy) legacy(toggle && !next.contains(id) ? QString{} : id);
            }
            if (guard && editing && !id.isEmpty()) {
                const auto callback=owner->onEntityEditRequested;
                if (callback) callback(id);
            }
            return id;
        } catch (const Standard_Failure& error) {
            if (guard) show_input_error(QStringLiteral("3D selection failed: ")+exception_text(error));
        } catch (const std::exception& error) {
            if (guard) show_input_error(QStringLiteral("3D selection failed: ")+exception_text(error));
        } catch (...) {
            if (guard) show_input_error(QStringLiteral("3D selection failed: unknown failure"));
        }
        return std::nullopt;
    }

    void select_rectangle(const QPointF& start, const QPointF& end, const SelectionCapture& capture) {
        const QPointer<NativeModelView> guard(owner);
        try {
            if (!selection_current(capture) || !owner->admitSceneInput(false) || !guard ||
                !selection_current(capture)) return;
            const auto first=input_point(start), last=input_point(end);
            const auto selector=context->MainSelector();
            const auto previous_overlap=selector->GetManager().IsOverlapAllowed();
            struct RestoreOverlap {
                occ::handle<StdSelect_ViewerSelector3d> selector;
                bool previous;
                ~RestoreOverlap() { try { selector->AllowOverlapDetection(previous); } catch (...) {} }
            } restore_overlap{selector,previous_overlap};
            selector->AllowOverlapDetection(end.x()<start.x());
            context->SelectRectangle(NCollection_Vec2<int>(std::min(first.x,last.x),std::min(first.y,last.y)),
                NCollection_Vec2<int>(std::max(first.x,last.x),std::max(first.y,last.y)),view,AIS_SelectionScheme_Replace);
            std::set<QString> selected;
            for (context->InitSelected();context->MoreSelected();context->NextSelected()) {
                const auto id=entity_id_for_presentation(context->SelectedInteractive());
                if (!id.isEmpty()) selected.insert(id);
            }
            restore_selection_highlights();
            if (!selection_current(capture)) return;
            QStringList hits;
            auto next=selected_entity_ids;
            for (const auto& id : selected) {
                hits.append(id);
                if (!next.contains(id)) next.append(id);
            }
            const auto callback=owner->onEntitiesSelected;
            const auto legacy=owner->onEntitySelected;
            owner->setSelectedEntities(next);
            if (!guard) return;
            if (callback) callback(hits,true);
            else if (legacy && !hits.isEmpty()) legacy(hits.back());
        } catch (const Standard_Failure& error) {
            if (guard) {
                try { restore_selection_highlights(); } catch (...) {}
                show_input_error(QStringLiteral("3D marquee selection failed: ")+exception_text(error));
            }
        } catch (const std::exception& error) {
            if (guard) {
                try { restore_selection_highlights(); } catch (...) {}
                show_input_error(QStringLiteral("3D marquee selection failed: ")+exception_text(error));
            }
        } catch (...) {
            if (guard) {
                try { restore_selection_highlights(); } catch (...) {}
                show_input_error(QStringLiteral("3D marquee selection failed: unknown failure"));
            }
        }
    }

    bool export_view_image(const QString& path) {
        if (!native_ready || view.IsNull()) {
            show_operation_error(QStringLiteral(
                "Native OCCT 3D view is not ready; show the viewport before exporting an image"));
            return false;
        }
        if (!geometry_prepared || regenerator.is_pending() || prepared_geometry ||
            !native_error.isEmpty() || !geometry_status.isEmpty()) {
            // The framebuffer cannot represent the complete current model.
            // Keep the existing geometry diagnostic and never export stale or
            // partial solids as a successful image.
            refresh_status_label();
            return false;
        }
        if (path.trimmed().isEmpty()) {
            show_export_error(QStringLiteral("3D view image export requires a destination path"));
            return false;
        }
        // Publication schedules the first fit for a settled native client.
        // An export can arrive before that queued paint/timer, particularly
        // when visibility changes from an initially empty scene. Complete the
        // pending fit against the current HWND before capturing its camera.
        if (initial_fit_pending) {
            const QPointer<NativeModelView> owner_guard(owner);
            complete_initial_fit();
            if (!owner_guard) return false;
            if (initial_fit_pending) {
                if (native_error.isEmpty())
                    show_export_error(QStringLiteral("3D view layout must settle before exporting an image"));
                return false;
            }
        }
        const bool restore_manipulator = !manipulator.IsNull() && manipulator->IsAttached();
        const int control_display_mode = restore_manipulator && manipulator->HasDisplayMode()
            ? manipulator->DisplayMode() : context->DisplayMode();
        const auto restore_controls = [this, restore_manipulator, control_display_mode] {
            try {
                if (restore_manipulator && !context.IsNull())
                    context->MainPrsMgr()->SetVisibility(manipulator, control_display_mode, true);
                restore_selection_highlights();
                return true;
            } catch (...) {
                show_operation_error(QStringLiteral("3D export could not restore editing controls. Refresh the model view."));
                return false;
            }
        };
        try {
            // Retain attachment and OCCT gesture start state. This phase is
            // inside the restoration boundary, including partial hide failures.
            if (restore_manipulator && !context.IsNull()) {
                context->MainPrsMgr()->SetVisibility(manipulator, control_display_mode, false);
                context->MainPrsMgr()->ClearImmediateDraw();
            }
            if (!context.IsNull()) context->ClearSelected(false);
            int width = 0, height = 0;
            view->Window()->Size(width, height);
            Image_PixMap pixels;
            if (width <= 0 || height <= 0 || !view->ToPixMap(pixels, width, height, Graphic3d_BT_RGB)) {
                restore_controls();
                show_export_error(QStringLiteral(
                    "OCCT could not capture the 3D framebuffer"));
                return false;
            }
            const auto image = detail::framebufferImage(pixels);
            // Restore before publishing a file. A control-restoration failure
            // therefore cannot replace the user's existing image destination.
            if (!restore_controls()) return false;
            QSaveFile destination(path);
            const auto encoding = QFileInfo(path).suffix().toLatin1().toLower();
            QImageWriter writer(&destination, encoding);
            if (image.isNull() || encoding.isEmpty() || !destination.open(QIODevice::WriteOnly) ||
                !writer.write(image) || !destination.commit()) {
                destination.cancelWriting();
                restore_controls();
                show_export_error(QStringLiteral("3D image could not be saved: %1")
                    .arg(writer.errorString()));
                return false;
            }
        } catch (const Standard_Failure& error) {
            restore_controls();
            show_export_error(QStringLiteral("OCCT 3D framebuffer export failed: ") +
                                 exception_text(error));
            return false;
        } catch (const std::exception& error) {
            restore_controls();
            show_export_error(QStringLiteral("3D framebuffer export failed: ") +
                                 exception_text(error));
            return false;
        } catch (...) {
            restore_controls();
            show_export_error(QStringLiteral("3D framebuffer export failed: unknown failure"));
            return false;
        }
        export_error.clear();
        refresh_status_label();
        return true;
    }
};

NativeModelView::NativeModelView(QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(this)) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TabletTracking, true);
    // Receive TouchBegin so live pen input can reject palm contacts. Idle
    // touch still falls through QWidget::event, which ignores it and retains
    // Qt's normal mouse fallback rather than claiming a touch gesture here.
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setMinimumSize(480, 360);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    m_impl->selection_rectangle = new QFrame(this);
    m_impl->selection_rectangle->setAttribute(Qt::WA_TransparentForMouseEvents,true);
    m_impl->selection_rectangle->setStyleSheet(QStringLiteral(
        "QFrame { border: 1px solid #64b5f6; background: rgba(100, 181, 246, 35); }"));
    m_impl->selection_rectangle->hide();
    m_impl->status_label = new QLabel(this);
    m_impl->status_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_impl->status_label->setWordWrap(true);
    m_impl->status_label->setTextFormat(Qt::PlainText);
    m_impl->status_label->setStyleSheet(
        QStringLiteral("QLabel { color: #ffd166; background: rgba(15, 20, 28, 220); "
                       "border: 1px solid #7e6b32; padding: 8px; }"));
    m_impl->status_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_impl->status_label->hide();
}

NativeModelView::~NativeModelView() {
    m_impl->preparation_timer->stop();
    m_impl->regenerator.shutdown();
    m_impl->detach_manipulator();
    if (m_impl->native_ready && !m_impl->context.IsNull()) {
        m_impl->context->RemoveAll(false);
    }
    if (!m_impl->view.IsNull()) {
        m_impl->view->Remove();
    }
    if (!m_impl->viewer.IsNull()) {
        m_impl->viewer->Remove();
    }
}

void NativeModelView::setSnapshot(const DocumentSnapshot& snapshot,
                                 std::optional<VisibleEntityIds> visible_ids) {
    // Forks may share both identity and revision while holding different content.
    // Deduplicate only the same immutable head and visibility request, retaining
    // in-flight work, completed topology, and failures for unchanged refreshes.
    if (m_impl->snapshot &&
        m_impl->snapshot->document_id() == snapshot.document_id() &&
        m_impl->snapshot->revision() == snapshot.revision() &&
        m_impl->visible_ids == visible_ids &&
        (m_impl->snapshot->shares_full_snapshot_with(snapshot) ||
         (same_snapshot_content(*m_impl->snapshot, snapshot) &&
          document_snapshot_digest(*m_impl->snapshot) == document_snapshot_digest(snapshot)))) {
        return;
    }

    auto requested_source = std::make_shared<const DocumentSnapshot>(snapshot);
    cancelInteraction();
    m_impl->detach_manipulator();
    m_impl->visible_ids = std::move(visible_ids);
    m_impl->snapshot = std::move(requested_source);
    const QPointer<NativeModelView> owner_guard(this);
    m_impl->rebuild_snapshot();
    if (!owner_guard) return;
    if (isVisible()) {
        m_impl->initialize_native_view();
    }
}

std::shared_ptr<const DocumentSnapshot> NativeModelView::preparationSourceSnapshot() const noexcept {
    return m_impl->snapshot;
}

std::shared_ptr<const DocumentSnapshot> NativeModelView::publishedSnapshot() const noexcept {
    return m_impl->published_snapshot;
}

std::shared_ptr<const DocumentSnapshot> NativeModelView::gestureSourceSnapshot() const noexcept {
    return m_impl->commit_snapshot ? m_impl->commit_snapshot : m_impl->gesture_snapshot;
}

void NativeModelView::fitAll() {
    cancelInteraction();
    if (isGeometryPending()) {
        m_impl->fit_requested = true;
        return;
    }
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        return;
    }
    m_impl->fit_all();
}

void NativeModelView::setSelectedEntity(const QString& entity_id) {
    setSelectedEntities(entity_id.trimmed().isEmpty() ? QStringList{} : QStringList{entity_id});
}

void NativeModelView::setSelectedEntities(const QStringList& entity_ids) {
    QStringList normalized;
    for (const auto& raw : entity_ids) {
        const auto id=raw.trimmed();
        if (!id.isEmpty() && !normalized.contains(id)) normalized.append(id);
    }
    if (normalized!=m_impl->selected_entity_ids) cancelInteraction();
    m_impl->selected_entity_ids=normalized;
    m_impl->selected_entity_id=normalized.isEmpty() ? std::nullopt
        : std::optional<std::string>{normalized.back().toStdString()};
    const QPointer<NativeModelView> guard(this);
    try {
        m_impl->restore_selection_highlights();
        m_impl->attach_manipulator();
        if (m_impl->native_ready && !m_impl->viewer.IsNull()) m_impl->viewer->Redraw();
    } catch (const Standard_Failure& error) {
        if (guard) {
            m_impl->detach_manipulator();
            m_impl->show_operation_error(QStringLiteral("3D selection controls unavailable: ")+exception_text(error));
        }
    } catch (const std::exception& error) {
        if (guard) {
            m_impl->detach_manipulator();
            m_impl->show_operation_error(QStringLiteral("3D selection controls unavailable: ")+exception_text(error));
        }
    } catch (...) {
        if (guard) {
            m_impl->detach_manipulator();
            m_impl->show_operation_error(QStringLiteral("3D selection controls unavailable: unknown failure"));
        }
    }
}

bool NativeModelView::transformControlsVisible() const noexcept {
    return m_impl->selected_entity_ids.size() == 1 && m_impl->selected_entity_id.has_value() &&
           m_impl->manipulator_entity_id == m_impl->selected_entity_id &&
           !m_impl->manipulator.IsNull() && m_impl->manipulator->IsAttached();
}

bool NativeModelView::exportViewImage(const QString& path) {
    return m_impl->export_view_image(path);
}

bool NativeModelView::isReady() const noexcept {
    return m_impl->geometry_prepared && !m_impl->regenerator.is_pending() &&
           !m_impl->prepared_geometry && m_impl->native_ready && m_impl->native_error.isEmpty() &&
           m_impl->geometry_status.isEmpty() && m_impl->operation_error.isEmpty();
}

QString NativeModelView::lastError() const {
    if (!m_impl->native_error.isEmpty()) {
        return m_impl->native_error;
    }
    if (!m_impl->geometry_status.isEmpty()) {
        return m_impl->geometry_status;
    }
    if (!m_impl->operation_error.isEmpty()) return m_impl->operation_error;
    return !m_impl->input_error.isEmpty() ? m_impl->input_error : m_impl->export_error;
}

bool NativeModelView::isGeometryPending() const noexcept {
    return m_impl->regenerator.is_pending();
}

void NativeModelView::pollGeometryPreparation() noexcept {
    if (QThread::currentThread() != thread()) return;
    try {
        m_impl->collect_prepared_geometry();
    } catch (...) {
        // Collection records worker/publication failures before notifying the
        // shell. A throwing error callback must not escape a polling boundary.
        // In particular, never clear the separate native initialization error.
    }
}

bool NativeModelView::isGeometryPrepared() const noexcept {
    return m_impl->geometry_prepared;
}

std::optional<NativeModelView::PublicationMetrics>
NativeModelView::lastPublicationMetrics() const noexcept {
    return m_impl->publication_metrics;
}

std::optional<QSize> NativeModelView::nativeRenderSizePixels() const noexcept {
    if (!m_impl->native_ready || m_impl->view.IsNull()) return std::nullopt;
    const auto gl_view = occ::handle<OpenGl_View>::DownCast(m_impl->view->View());
    if (gl_view.IsNull() || gl_view->GlWindow().IsNull()) return std::nullopt;
    return QSize(gl_view->GlWindow()->Width(), gl_view->GlWindow()->Height());
}

void NativeModelView::setEntitySelectedCallback(std::function<void(QString)> callback) {
    onEntitySelected = std::move(callback);
}

void NativeModelView::setEntitySelectionClickedCallback(std::function<void(QString, bool)> callback) {
    onEntitySelectionClicked=std::move(callback);
}

void NativeModelView::setEntitiesSelectedCallback(std::function<void(QStringList, bool)> callback) {
    onEntitiesSelected=std::move(callback);
}

void NativeModelView::setEntityEditRequestedCallback(std::function<void(QString)> callback) {
    onEntityEditRequested = std::move(callback);
}

void NativeModelView::setEntityTranslationRequestedCallback(
    std::function<void(QString, double, double, double)> callback) {
    onEntityTranslationRequested = std::move(callback);
}

void NativeModelView::setEntityTransformRequestedCallback(
    std::function<void(QString, double, double, double, double, double)> callback) {
    onEntityTransformRequested = std::move(callback);
}

void NativeModelView::setErrorCallback(std::function<void(QString)> callback) {
    onError = std::move(callback);
}

void NativeModelView::setGeometryStatusChangedCallback(std::function<void(QString)> callback) {
    onGeometryStatusChanged = std::move(callback);
}

bool NativeModelView::beginMove(const QString& entity_id) {
    cancelInteraction();
    if (m_impl->selected_entity_ids.size()>1 || !isReady() || !m_impl->supports_direct_translation(entity_id)) return false;
    const auto found = m_impl->solids.find(entity_id.toStdString());
    if (found == m_impl->solids.end() ||
        !m_impl->context->IsDisplayed(found->second.presentation)) return false;
    m_impl->selected_entity_ids=QStringList{entity_id};
    m_impl->selected_entity_id = entity_id.toStdString();
    m_impl->detach_manipulator();
    m_impl->translation_entity_id = entity_id.toStdString();
    m_impl->translation_preview_ids = m_impl->transform_presentation_ids(entity_id.toStdString());
    setCursor(Qt::SizeAllCursor);
    return true;
}

bool NativeModelView::isMoveActive() const noexcept {
    return m_impl->translation_entity_id.has_value();
}

std::optional<NativeModelView::TransformControl> NativeModelView::transformControlAt(
    const QPointF& point) {
    if (!isReady() || m_impl->context.IsNull() || m_impl->view.IsNull() ||
        m_impl->manipulator.IsNull() || !m_impl->manipulator->IsAttached()) return std::nullopt;
    const auto native = m_impl->input_point(point);
    m_impl->context->MoveTo(native.x, native.y, m_impl->view, false);
    const auto detected = occ::handle<AIS_ManipulatorOwner>::DownCast(
        m_impl->context->DetectedOwner());
    if (detected.IsNull() || m_impl->context->DetectedInteractive() != m_impl->manipulator)
        return std::nullopt;
    switch (detected->Mode()) {
    case AIS_MM_Translation: return TransformControl::translation;
    case AIS_MM_Rotation: return TransformControl::rotation;
    case AIS_MM_Scaling: return TransformControl::scale;
    default: return std::nullopt;
    }
}

std::optional<std::array<double, 12>> NativeModelView::nativePresentationTransform(
    const QString& entity_id) const {
    const auto found = m_impl->solids.find(entity_id.toStdString());
    if (found == m_impl->solids.end() || found->second.presentation.IsNull()) return std::nullopt;
    std::array<double, 12> result{};
    const auto& transform = found->second.presentation->LocalTransformation();
    for (int row = 1; row <= 3; ++row)
        for (int column = 1; column <= 4; ++column)
            result[static_cast<std::size_t>((row - 1) * 4 + column - 1)] = transform.Value(row, column);
    return result;
}

void NativeModelView::cancelInteraction() {
    resetInteraction(true);
}

void NativeModelView::resetInteraction(bool restore_controls, bool keep_tablet_dispatch) {
    if (m_impl->tablet_dispatch_depth && !keep_tablet_dispatch) m_impl->tablet_dispatch_retired=true;
    m_impl->tablet_active=false;
    m_impl->tablet_button=Qt::NoButton;
    m_impl->tablet_device.clear();
    m_impl->initiating_button=Qt::NoButton;
    // A retired gesture cannot revive a fractional wheel burst after focus,
    // capture or selection returns. wheelEvent computes carry before this
    // reset and publishes its own new context only after navigation succeeds.
    m_impl->wheel_scroll.reset();
    if (!m_impl->manipulator.IsNull()) {
        try {
            if (m_impl->manipulator->HasActiveTransformation())
                m_impl->manipulator->StopTransform(false);
            m_impl->manipulator->DeactivateCurrentMode();
        } catch (...) {
        }
    }
    m_impl->manipulation_transform.reset();
    m_impl->gesture_snapshot.reset();
    m_impl->selection_capture.reset();
    if (m_impl->selection_rectangle) m_impl->selection_rectangle->hide();
    m_impl->clear_translation_preview();
    m_impl->gesture = Impl::Gesture::none;
    m_impl->left_moved = false;
    m_impl->translation_entity_id.reset();
    m_impl->translation_start.reset();
    if (restore_controls) { try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); } }
    unsetCursor();
    if (m_impl->native_ready && !m_impl->view.IsNull()) m_impl->view->Redraw();
}

bool NativeModelView::event(QEvent* event) {
    const QPointer<NativeModelView> input_guard(this);
    if (m_impl) retireDisconnectedTablet();
    if (!input_guard) { event->accept(); return true; }
    if (m_impl && (m_impl->tablet_active || m_impl->tablet_dispatch_depth) && event->type()==QEvent::Wheel) {
        event->accept();
        return true;
    }
    if (m_impl && (event->type()==QEvent::TouchBegin || event->type()==QEvent::TouchUpdate ||
                   event->type()==QEvent::TouchEnd || event->type()==QEvent::TouchCancel)) {
        if (m_impl->tablet_active || (m_impl->tablet_dispatch_depth && m_impl->initiating_button!=Qt::NoButton)) {
            event->accept();
            return true;
        }
        // An independent touch sequence keeps Qt's original fallback behavior;
        // it must not be mistaken for a finished pen's synthesized mouse tail.
        if (event->type()==QEvent::TouchBegin) m_impl->tablet_mouse_suppression_until={};
    }
    if (m_impl && (event->type()==QEvent::ScreenChangeInternal ||
                   event->type()==QEvent::DevicePixelRatioChange))
        m_impl->navigation_changed();
    if (m_impl && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide ||
                   event->type() == QEvent::WindowBlocked || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut)) {
        cancelInteraction();
        if (event->type()!=QEvent::UngrabMouse) m_impl->space_pan_armed=false;
    }
    if (event->type()==QEvent::ShortcutOverride &&
        static_cast<QKeyEvent*>(event)->key()==Qt::Key_Space) {
        event->accept();
        return true;
    }
    if ((event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease) &&
        static_cast<QKeyEvent*>(event)->key()==Qt::Key_Space) {
        const auto* key=static_cast<QKeyEvent*>(event);
        if (!key->isAutoRepeat()) m_impl->space_pan_armed=event->type()==QEvent::KeyPress;
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        cancelInteraction();
        m_impl->space_pan_armed=false;
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void NativeModelView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const QPointer<NativeModelView> owner_guard(this);
    m_impl->initialize_native_view();
    if (!owner_guard) return;
    m_impl->synchronize_native_size();
    m_impl->refresh_status_label();
    if (m_impl->initial_fit_pending) m_impl->schedule_initial_fit();
    update();
}

void NativeModelView::resizeEvent(QResizeEvent* event) {
    m_impl->navigation_changed();
    QWidget::resizeEvent(event);
    m_impl->refresh_status_label();
    if (m_impl->native_ready && !m_impl->view.IsNull()) {
        m_impl->synchronize_native_size();
        m_impl->view->Redraw();
    }
}

void NativeModelView::paintEvent(QPaintEvent* event) {
    (void)event;
    if (m_impl->native_ready && !m_impl->view.IsNull()) {
        m_impl->synchronize_native_size();
        const QPointer<NativeModelView> owner_guard(this);
        m_impl->complete_initial_fit();
        if (!owner_guard) return;
        m_impl->view->Redraw();
    }
}

bool NativeModelView::claimPointer(Qt::MouseButton button) {
    if (m_impl->tablet_dispatch_depth) {
        if (m_impl->tablet_dispatch_retired || !m_impl->tablet_dispatch_device) return false;
        m_impl->tablet_active=true;
        m_impl->tablet_button=button;
        m_impl->tablet_device=m_impl->tablet_dispatch_device;
    }
    m_impl->initiating_button=button;
    return true;
}

bool NativeModelView::suppressMouseInput(const QMouseEvent* event) const {
    if (m_impl->tablet_active || m_impl->tablet_dispatch_depth) return true;
    if (std::chrono::steady_clock::now()>=m_impl->tablet_mouse_suppression_until) return false;
    const auto device=event->pointingDevice();
    const bool pen=device && (device->type()==QInputDevice::DeviceType::Stylus ||
                              device->type()==QInputDevice::DeviceType::Airbrush);
    return pen || event->source()!=Qt::MouseEventNotSynthesized;
}

void NativeModelView::retireDisconnectedTablet() noexcept {
    if (!m_impl->tablet_active || m_impl->tablet_device) return;
    cancelTabletInteraction();
}

void NativeModelView::cancelTabletInteraction() noexcept {
    const QPointer<NativeModelView> guard(this);
    try { cancelInteraction(); }
    catch (const Standard_Failure& error) {
        if (guard) { try { m_impl->show_input_error(QStringLiteral("3D pen cancellation failed: ")+exception_text(error)); } catch (...) {} }
    } catch (const std::exception& error) {
        if (guard) { try { m_impl->show_input_error(QStringLiteral("3D pen cancellation failed: ")+exception_text(error)); } catch (...) {} }
    } catch (...) {
        if (guard) { try { m_impl->show_input_error(QStringLiteral("3D pen cancellation failed: unknown failure")); } catch (...) {} }
    }
}

void NativeModelView::mousePressEvent(QMouseEvent* event) {
    if (suppressMouseInput(event)) { event->accept(); return; }
    const QPointer<NativeModelView> guard(this);
    pointerPress(event);
    if (guard && !event->isAccepted()) QWidget::mousePressEvent(event);
}

void NativeModelView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (suppressMouseInput(event)) { event->accept(); return; }
    pointerDoubleClick(event);
}

void NativeModelView::mouseMoveEvent(QMouseEvent* event) {
    if (suppressMouseInput(event)) { event->accept(); return; }
    const QPointer<NativeModelView> guard(this);
    pointerMove(event);
    if (guard && !event->isAccepted()) QWidget::mouseMoveEvent(event);
}

void NativeModelView::mouseReleaseEvent(QMouseEvent* event) {
    if (suppressMouseInput(event)) { event->accept(); return; }
    const QPointer<NativeModelView> guard(this);
    pointerRelease(event);
    if (guard && !event->isAccepted()) QWidget::mouseReleaseEvent(event);
}

void NativeModelView::tabletEvent(QTabletEvent* event) {
    const QPointer<NativeModelView> guard(this);
    retireDisconnectedTablet();
    if (!guard) { event->accept(); return; }
    // Accept actual tablet packets, including refused/stale packets, so Qt
    // does not synthesize a second mouse authoring gesture from them.
    event->accept();
    if (!event->pointingDevice()) return;
    const auto device=event->pointingDevice();
    if (m_impl->tablet_dispatch_depth) {
        // Admission/capture callbacks may process nested Qt events before the
        // shared press routine claims its button. Retain that dispatched owner
        // too: a same-pen release cannot disappear and let the outer press arm.
        if (!m_impl->tablet_dispatch_retired && m_impl->tablet_dispatch_device.data()==device &&
            m_impl->tablet_dispatch_button!=Qt::NoButton &&
            !event->buttons().testFlag(m_impl->tablet_dispatch_button))
            cancelTabletInteraction();
        return;
    }
    const auto dispatch_button = m_impl->tablet_active ? m_impl->tablet_button
        : event->type()==QEvent::TabletPress ? event->button() : Qt::NoButton;
    if (event->type()==QEvent::TabletPress) {
        if (m_impl->tablet_active) {
            // Qt can classify Left -> Right as a press because the new mask
            // is numerically larger. The old owner still ended in that packet.
            if (m_impl->tablet_device.data()==device && !event->buttons().testFlag(m_impl->tablet_button))
                cancelTabletInteraction();
            return;
        }
        if (m_impl->initiating_button!=Qt::NoButton ||
            (event->button()!=Qt::LeftButton && event->button()!=Qt::RightButton && event->button()!=Qt::MiddleButton) ||
            !event->buttons().testFlag(event->button())) return;
    } else if (event->type()==QEvent::TabletMove) {
        if (m_impl->tablet_active) {
            if (m_impl->tablet_device.data()!=device) return;
            if (!event->buttons().testFlag(m_impl->tablet_button)) {
                cancelTabletInteraction();
                return;
            }
        } else if (m_impl->initiating_button!=Qt::NoButton || event->buttons()!=Qt::NoButton) return;
    } else if (event->type()==QEvent::TabletRelease) {
        if (!m_impl->tablet_active || m_impl->tablet_device.data()!=device) return;
        // Qt names the first changed button bit when several bits change in
        // one packet. A missing owner with another named button cancels the
        // gesture; an extra-button release while the owner is held does not.
        if (event->button()!=m_impl->tablet_button) {
            if (!event->buttons().testFlag(m_impl->tablet_button)) cancelTabletInteraction();
            return;
        }
        if (event->buttons().testFlag(m_impl->tablet_button)) return;
        // A release retires its device before contextual callbacks can run a
        // nested event loop. The shared routine still owns the original button.
        m_impl->tablet_active=false;
        m_impl->tablet_button=Qt::NoButton;
        m_impl->tablet_device.clear();
    } else return;
    m_impl->tablet_mouse_suppression_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    m_impl->tablet_dispatch_depth++;
    m_impl->tablet_dispatch_retired=false;
    m_impl->tablet_dispatch_device=device;
    m_impl->tablet_dispatch_button=dispatch_button;
    struct TabletDispatchReset {
        QPointer<NativeModelView> owner;
        ~TabletDispatchReset() {
            if (!owner) return;
            auto& impl=*owner->m_impl;
            if (--impl.tablet_dispatch_depth==0) {
                impl.tablet_dispatch_device.clear();
                impl.tablet_dispatch_button=Qt::NoButton;
                impl.tablet_dispatch_retired=false;
            }
        }
    } reset_dispatch{guard};
    const auto refuse=[&](const QString& message) noexcept {
        if (!guard) return;
        try { cancelInteraction(); } catch (...) {}
        if (guard) { try { m_impl->show_input_error(message); } catch (...) {} }
    };
    try {
        const auto point=event->position(),global=event->globalPosition();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            !std::isfinite(global.x()) || !std::isfinite(global.y()))
            throw std::invalid_argument("The pen pointer position is unavailable.");
        if (event->type()==QEvent::TabletPress) pointerPress(event);
        else if (event->type()==QEvent::TabletMove) pointerMove(event);
        else pointerRelease(event);
    } catch (const Standard_Failure& error) {
        refuse(QStringLiteral("3D pen input failed: ")+exception_text(error));
    } catch (const std::exception& error) {
        refuse(QStringLiteral("3D pen input failed: ")+exception_text(error));
    } catch (...) {
        refuse(QStringLiteral("3D pen input failed: unknown failure"));
    }
    event->accept();
}

void NativeModelView::pointerPress(QSinglePointEvent* event) {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        event->ignore();
        return;
    }
    const QPointer<NativeModelView> owner_guard(this);
    m_impl->complete_initial_fit();
    if (!owner_guard) { event->accept(); return; }
    const auto logical_point = event->position();
    const auto point = m_impl->input_point(logical_point);
    setFocus();
    if (m_impl->initiating_button != Qt::NoButton) {
        event->accept();
        return; // Extra buttons cannot replace the gesture owner.
    }
    m_impl->left_press = logical_point;
    m_impl->left_moved = false;
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton ||
        (event->button()==Qt::LeftButton && m_impl->space_pan_armed)) {
        // Camera changes invalidate an armed Move's view-plane anchor.
        resetInteraction(true,true);
        if (event->button()==Qt::RightButton && isReady()) {
            m_impl->detach_manipulator();
            m_impl->view->Redraw();
            m_impl->selection_capture=m_impl->capture_selection();
            m_impl->gesture_snapshot=m_impl->published_snapshot;
        }
        if (!claimPointer(event->button())) { event->accept(); return; }
        m_impl->gesture = event->button() == Qt::RightButton ? Impl::Gesture::orbit : Impl::Gesture::pan;
        m_impl->navigation_start = QPoint(point.x, point.y);
        if (m_impl->gesture == Impl::Gesture::orbit)
            m_impl->view->StartRotation(point.x, point.y, 0.4);
        else
            m_impl->view->Pan(0, 0, 1.0, true);
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (!isReady()) {
            event->ignore();
            return;
        }
        if (!admitSceneInput(true)) { event->accept(); return; }
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            resetInteraction(true,true);
            m_impl->detach_manipulator();
            m_impl->view->Redraw();
            if (!claimPointer(Qt::LeftButton)) { event->accept(); return; }
            m_impl->gesture=Impl::Gesture::additive_select;
            m_impl->selection_capture=m_impl->capture_selection();
            m_impl->gesture_snapshot=m_impl->published_snapshot;
            event->accept();
            return;
        }
        if (event->modifiers().testFlag(Qt::AltModifier)) {
            m_impl->begin_overlap_selection();
            event->accept();
            return;
        }
        if (!claimPointer(Qt::LeftButton)) { event->accept(); return; }
        const auto capture_transform = [this](const QString& target) {
            m_impl->gesture_snapshot = m_impl->published_snapshot;
            const QPointer<NativeModelView> guard(this);
            try {
                const auto observer=onTransformGestureStarted;
                if (observer) observer(target);
                return !guard.isNull() && (!m_impl->tablet_dispatch_depth || !m_impl->tablet_dispatch_retired);
            } catch (const std::exception& error) {
                if (guard) {
                    guard->cancelInteraction();
                    guard->m_impl->show_input_error(QStringLiteral("3D edit capture failed: ") +
                        QString::fromUtf8(error.what()));
                }
            } catch (...) {
                if (guard) {
                    guard->cancelInteraction();
                    guard->m_impl->show_input_error(QStringLiteral("3D edit capture failed: unknown observer failure"));
                }
            }
            return false;
        };
        const bool manipulating = !isMoveActive() && m_impl->begin_manipulation(point);
        if (!owner_guard) { event->accept(); return; }
        if (manipulating) {
            if (!capture_transform(QString::fromStdString(*m_impl->manipulator_entity_id))) {
                event->accept();
                return;
            }
            m_impl->gesture = Impl::Gesture::manipulate;
            setCursor(Qt::SizeAllCursor);
            event->accept();
            return;
        }
        m_impl->gesture = isMoveActive() ? Impl::Gesture::move : Impl::Gesture::select;
        if (m_impl->gesture==Impl::Gesture::select) {
            m_impl->selection_capture=m_impl->capture_selection();
            m_impl->gesture_snapshot=m_impl->published_snapshot;
        }
        m_impl->translation_start.reset();
        if (m_impl->gesture == Impl::Gesture::move) {
            if (!capture_transform(QString::fromStdString(*m_impl->translation_entity_id))) {
                event->accept();
                return;
            }
            m_impl->translation_start = m_impl->world_point(point);
        }
        event->accept();
        return;
    }
    event->ignore();
}

void NativeModelView::pointerDoubleClick(QSinglePointEvent* event) {
    // Qt replaces the second press with a double-click event. Modified Alt
    // repeats still need their normal press/release cycle, with Ctrl priority.
    if (event->button()==Qt::LeftButton && event->buttons()==Qt::LeftButton &&
        event->modifiers().testFlag(Qt::AltModifier) &&
        !event->modifiers().testFlag(Qt::ControlModifier)) {
        pointerPress(event);
        return;
    }
    // The second press belongs to Edit, never to an armed Move. Delay the
    // request until release so a drag or cancellation cannot open an editor.
    const bool can_edit = isReady() && event->button() == Qt::LeftButton &&
                          event->buttons() == Qt::LeftButton &&
                          event->modifiers() == Qt::NoModifier &&
                          !m_impl->space_pan_armed &&
                          m_impl->initiating_button == Qt::NoButton;
    cancelInteraction();
    if (can_edit && admitSceneInput(true)) {
        // The manipulator origin commonly overlaps the object's centre. Hide it
        // for the semantic edit pick so a real double-click never targets the
        // derived control instead of its document object.
        m_impl->detach_manipulator();
        m_impl->view->Redraw();
        m_impl->selection_capture=m_impl->capture_selection();
        m_impl->gesture_snapshot=m_impl->published_snapshot;
        m_impl->gesture = Impl::Gesture::edit;
        m_impl->initiating_button = Qt::LeftButton;
        m_impl->left_press = event->position();
    }
    event->accept();
}

void NativeModelView::pointerMove(QSinglePointEvent* event) {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        event->ignore();
        return;
    }
    const auto logical_point = event->position();
    const auto point = m_impl->input_point(logical_point);
    if (m_impl->initiating_button != Qt::NoButton &&
        !event->buttons().testFlag(m_impl->initiating_button)) {
        cancelInteraction();
        event->accept();
        return;
    }
    if (m_impl->initiating_button != Qt::NoButton &&
        (logical_point - m_impl->left_press).manhattanLength() >= QApplication::startDragDistance())
        m_impl->left_moved = true;
    if (m_impl->gesture==Impl::Gesture::overlap_select || m_impl->gesture==Impl::Gesture::overlap_pan) {
        if (m_impl->left_moved) m_impl->pan_overlap_drag(point);
        event->accept();
        return;
    }
    if (m_impl->gesture==Impl::Gesture::additive_select) {
        const QPointer<NativeModelView> guard(this);
        if (!m_impl->selection_capture || !m_impl->selection_current(*m_impl->selection_capture) ||
            !admitSceneInput(false)) {
            if (guard) guard->cancelInteraction();
            event->accept();
            return;
        }
        if (m_impl->left_moved) {
            const auto rectangle=QRectF(m_impl->left_press,logical_point).normalized().toAlignedRect().intersected(rect());
            m_impl->selection_rectangle->setGeometry(rectangle);
            m_impl->selection_rectangle->show();
            m_impl->selection_rectangle->raise();
        }
        event->accept();
        return;
    }
    if (m_impl->gesture == Impl::Gesture::orbit) {
        if (m_impl->left_moved) {
            m_impl->navigation_changed();
            m_impl->view->Rotation(point.x, point.y);
        }
        event->accept();
        return;
    }
    if (m_impl->gesture == Impl::Gesture::pan) {
        const auto delta = QPoint(point.x, point.y) - m_impl->navigation_start;
        // V3d::Pan accepts view-plane displacement (positive y is up),
        // unlike picking/rotation/zoom mouse positions measured from the top.
        m_impl->navigation_changed();
        m_impl->view->Pan(delta.x(), -delta.y(), 1.0, false);
        event->accept();
        return;
    }
    if (m_impl->initiating_button == Qt::LeftButton) {
        if (m_impl->gesture == Impl::Gesture::manipulate && m_impl->left_moved) {
            const QPointer<NativeModelView> owner_guard(this);
            m_impl->preview_manipulation(point);
            if (!owner_guard) { event->accept(); return; }
        }
        if (m_impl->gesture == Impl::Gesture::move && m_impl->left_moved &&
            m_impl->translation_entity_id.has_value()) {
            if (const auto current = m_impl->world_point(point)) {
                m_impl->preview_translation(*current);
            }
        }
        if (!m_impl->context.IsNull()) {
            m_impl->context->MoveTo(point.x, point.y, m_impl->view, false);
            m_impl->viewer->RedrawImmediate();
        }
        event->accept();
        return;
    }
    if (!m_impl->context.IsNull()) {
        m_impl->context->MoveTo(point.x, point.y, m_impl->view, false);
        m_impl->viewer->RedrawImmediate();
    }
    event->ignore();
}

void NativeModelView::pointerRelease(QSinglePointEvent* event) {
    if (event->button() != m_impl->initiating_button || event->button() == Qt::NoButton) {
        event->accept();
        return;
    }
    const QPointer<NativeModelView> owner_guard(this);
    struct CommitSourceReset {
        QPointer<NativeModelView> owner;
        ~CommitSourceReset() { if (owner) owner->m_impl->commit_snapshot.reset(); }
    } reset_source{owner_guard};
    const auto point = m_impl->input_point(event->position());
    if ((event->position() - m_impl->left_press).manhattanLength() >= QApplication::startDragDistance())
        m_impl->left_moved = true;
    if (m_impl->gesture==Impl::Gesture::overlap_select || m_impl->gesture==Impl::Gesture::overlap_pan) {
        if (m_impl->left_moved) {
            // Also covers press/release beyond threshold with no move event.
            m_impl->pan_overlap_drag(point,true);
        } else {
            try {
                const auto capture=m_impl->selection_capture;
                if (!capture || !m_impl->selection_current(*capture))
                    throw std::invalid_argument("The displayed 3D source, selection or camera changed. Start again.");
                resetInteraction(false);
                m_impl->commit_snapshot=capture->source;
                (void)m_impl->select_at(point,*capture,false,false,true);
                if (owner_guard) {
                    try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
                }
            } catch (const Standard_Failure& error) {
                if (owner_guard) m_impl->refuse_overlap_input(QStringLiteral("3D overlap selection failed: ")+exception_text(error));
            } catch (const std::exception& error) {
                if (owner_guard) m_impl->refuse_overlap_input(QStringLiteral("3D overlap selection failed: ")+exception_text(error));
            } catch (...) {
                if (owner_guard) m_impl->refuse_overlap_input(QStringLiteral("3D overlap selection failed: unknown failure"));
            }
        }
        event->accept();
        return;
    }
    if (m_impl->gesture == Impl::Gesture::orbit || m_impl->gesture == Impl::Gesture::pan) {
        const bool context_click = m_impl->gesture == Impl::Gesture::orbit && !m_impl->left_moved;
        const auto global_position = event->globalPosition().toPoint();
        const auto capture=m_impl->selection_capture;
        resetInteraction(!context_click);
        const auto callback = onContextMenuRequested;
        event->accept();
        if (context_click) {
            if (!capture || !m_impl->selection_current(*capture)) { cancelInteraction(); return; }
            if (!admitSceneInput(true)) return;
            m_impl->commit_snapshot=capture->source;
            m_impl->detach_manipulator();
            const auto target = m_impl->select_at(point,*capture);
            if (owner_guard) {
                try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
            }
            if (owner_guard && target && callback) callback(*target, global_position);
        }
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const auto capture=m_impl->selection_capture;
        const auto selection_start=m_impl->left_press;
        const auto was_additive=m_impl->gesture==Impl::Gesture::additive_select;
        const auto was_edit = m_impl->gesture == Impl::Gesture::edit && !m_impl->left_moved;
        const auto was_click = m_impl->gesture == Impl::Gesture::select && !m_impl->left_moved;
        const auto was_translation = m_impl->gesture == Impl::Gesture::move && m_impl->left_moved &&
                                     m_impl->translation_entity_id.has_value() &&
                                     m_impl->translation_start.has_value();
        const auto was_manipulation = m_impl->gesture == Impl::Gesture::manipulate &&
                                      m_impl->left_moved &&
                                      m_impl->manipulator_entity_id.has_value();
        if ((was_additive || was_edit || was_click) &&
            (!capture || !m_impl->selection_current(*capture))) {
            cancelInteraction();
            m_impl->show_input_error(QStringLiteral("The displayed 3D source, selection or camera changed. Start selection again."));
            event->accept();
            return;
        }
        const auto selection_drag=m_impl->left_moved;
        std::optional<NativeModelView::Impl::WorldPoint> end_world;
        if (was_translation) {
            end_world = m_impl->world_point(point);
        }
        const auto translation_id = m_impl->translation_entity_id;
        const auto translation_start = m_impl->translation_start;
        if (was_manipulation) m_impl->preview_manipulation(point);
        if (!owner_guard) { event->accept(); return; }
        const auto manipulation_id = m_impl->manipulator_entity_id;
        const auto manipulation_transform = m_impl->manipulation_transform;
        const auto gesture_source = m_impl->gesture_snapshot;
        resetInteraction(!(was_additive || was_edit || was_click));
        // Reset even when an observer throws; the shell can read only this
        // actual press capture, never the newest requested snapshot.

        m_impl->commit_snapshot = gesture_source;
        if (was_additive) {
            m_impl->detach_manipulator();
            if (selection_drag) m_impl->select_rectangle(selection_start,event->position(),*capture);
            else (void)m_impl->select_at(point,*capture,false,true);
            if (owner_guard) {
                try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
            }
            event->accept();
            return;
        }
        if (was_edit && isReady()) {
            m_impl->detach_manipulator();
            (void)m_impl->select_at(point,*capture,true);
            if (!owner_guard) { event->accept(); return; }
            try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
            event->accept();
            return;
        }
        if (was_translation && end_world.has_value() && translation_id.has_value() &&
            translation_start.has_value()) {
            const auto dx = end_world->x - translation_start->x;
            const auto dy = end_world->y - translation_start->y;
            const auto dz = end_world->z - translation_start->z;
            const auto callback = onEntityTranslationRequested;
            constexpr double epsilon = 1.0e-9;
            if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                (std::abs(dx) > epsilon || std::abs(dy) > epsilon || std::abs(dz) > epsilon) &&
                callback) {
                callback(QString::fromStdString(*translation_id), dx, dy, dz);
                if (!owner_guard) { event->accept(); return; }
            }
        }
        const auto transform_callback = onEntityTransformRequested;
        if (was_manipulation && manipulation_id.has_value() &&
            manipulation_transform.has_value() && transform_callback) {
            const auto& transform = *manipulation_transform;
            const auto translation = transform.TranslationPart();
            const auto scale = transform.ScaleFactor();
            gp_XYZ rotation_axis;
            double rotation = 0.0;
            if (transform.GetRotation(rotation_axis, rotation) && rotation_axis.Z() < 0.0)
                rotation = -rotation;
            constexpr double epsilon = 1.0e-9;
            const bool finite = std::isfinite(translation.X()) &&
                                std::isfinite(translation.Y()) &&
                                std::isfinite(translation.Z()) &&
                                std::isfinite(rotation) && std::isfinite(scale) && scale > 0.0;
            const bool changed = std::abs(translation.X()) > epsilon ||
                                 std::abs(translation.Y()) > epsilon ||
                                 std::abs(translation.Z()) > epsilon ||
                                 std::abs(rotation) > epsilon ||
                                 std::abs(scale - 1.0) > epsilon;
            // X/Y rotation controls are disabled, but fail closed if an OCCT
            // transform ever reports a materially different rotation axis.
            const bool supported_axis = std::abs(rotation) <= epsilon ||
                                        (std::abs(rotation_axis.X()) <= epsilon &&
                                         std::abs(rotation_axis.Y()) <= epsilon &&
                                         std::abs(std::abs(rotation_axis.Z()) - 1.0) <= epsilon);
            if (finite && changed && supported_axis) {
                transform_callback(QString::fromStdString(*manipulation_id),
                    translation.X(), translation.Y(), translation.Z(), rotation, scale);
                if (!owner_guard) { event->accept(); return; }
            } else if (finite && changed && !supported_axis) {
                m_impl->show_operation_error(
                    QStringLiteral("Vertex supports direct 3D rotation around the vertical axis only."));
            }
        }
        if (was_click) {
            (void)m_impl->select_at(point,*capture);
        }
        event->accept();
        return;
    }
    event->ignore();
}

bool NativeModelView::admitSceneInput(bool starting) {
    const QPointer<NativeModelView> guard(this);
    try {
        const auto observer = onSceneInputRequested;
        const bool accepted = !observer || observer(starting);
        if (!guard) return false;
        if (!accepted) cancelInteraction();
        else if (!m_impl->input_error.isEmpty()) {
            m_impl->input_error.clear();
            m_impl->refresh_status_label();
        }
        return accepted;
    } catch (const std::exception& error) {
        if (guard) {
            guard->cancelInteraction();
            guard->m_impl->show_input_error(QStringLiteral("3D input admission failed: ") +
                QString::fromUtf8(error.what()));
        }
    } catch (...) {
        if (guard) {
            guard->cancelInteraction();
            guard->m_impl->show_input_error(QStringLiteral("3D input admission failed: unknown observer failure"));
        }
    }
    return false;
}

void NativeModelView::wheelEvent(QWheelEvent* event) {
    if (event->phase()==Qt::ScrollBegin) m_impl->wheel_scroll.reset();
    const QPointer<NativeModelView> owner_guard(this);
    struct ScrollEndReset {
        QPointer<NativeModelView> owner;
        bool ending;
        ~ScrollEndReset() { if (owner && ending) owner->m_impl->wheel_scroll.reset(); }
    } reset_scroll{owner_guard,event->phase()==Qt::ScrollEnd};
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        m_impl->wheel_scroll.reset();
        event->ignore();
        return;
    }
    m_impl->complete_initial_fit();
    if (!owner_guard) { event->accept(); return; }
    const auto angle=event->angleDelta().y();
    const bool pixel_input=angle==0;
    const auto logical_movement=std::clamp(pixel_input ? static_cast<double>(event->pixelDelta().y())
                                                      : static_cast<double>(angle)/8.0,-120.0,120.0);
    if (logical_movement==0.0) {
        QWidget::wheelEvent(event);
        return;
    }
    const auto refuse=[&](const QString& message) noexcept {
        if (!owner_guard) return;
        m_impl->wheel_scroll.reset();
        try { cancelInteraction(); } catch (...) {}
        if (owner_guard) { try { m_impl->show_input_error(message); } catch (...) {} }
    };
    try {
        const auto point=m_impl->input_point(event->position());
        const auto ratio=m_impl->input_scale();
        const auto exact_native=logical_movement*ratio;
        if (!std::isfinite(ratio) || ratio<=0 || !std::isfinite(exact_native) ||
            std::abs(exact_native)>static_cast<double>(std::numeric_limits<int>::max())/2.0)
            throw std::invalid_argument("The native wheel pixel mapping is unavailable.");
        const auto now=std::chrono::steady_clock::now();
        const auto& previous=m_impl->wheel_scroll;
        const bool carry=previous && previous->device && previous->screen &&
            previous->device.data()==event->pointingDevice() && previous->screen.data()==screen() &&
            previous->anchor==QPoint(point.x,point.y) && previous->pixel_input==pixel_input &&
            now-previous->last_event<=std::chrono::milliseconds(500) &&
            m_impl->selection_current(previous->context);
        // Whole wheel detents retain their existing independently rounded
        // displacement. Continuous input carries less than one native pixel.
        const bool detent=!pixel_input && angle%120==0;
        const auto accumulated=exact_native+(!detent && carry ? previous->remainder : 0.0);
        const auto native_movement=detent ? qRound(exact_native) : static_cast<int>(std::trunc(accumulated));
        const auto end_y=static_cast<std::int64_t>(point.y)+native_movement;
        if (end_y<std::numeric_limits<int>::min() || end_y>std::numeric_limits<int>::max())
            throw std::invalid_argument("The native wheel anchor is outside the supported pixel range.");
        cancelInteraction();
        if (!owner_guard) { event->accept(); return; }
        if (native_movement!=0) {
            m_impl->navigation_changed();
            m_impl->view->StartZoomAtPoint(point.x,point.y);
            m_impl->view->ZoomAtPoint(point.x,point.y,point.x,static_cast<int>(end_y));
        }
        if (detent) m_impl->wheel_scroll.reset();
        else m_impl->wheel_scroll=Impl::WheelScroll{m_impl->capture_selection(),event->pointingDevice(),
            screen(),QPoint(point.x,point.y),accumulated-native_movement,now,pixel_input};
    } catch (const Standard_Failure& error) {
        refuse(QStringLiteral("3D zoom failed: ")+exception_text(error));
    } catch (const std::exception& error) {
        refuse(QStringLiteral("3D zoom failed: ")+exception_text(error));
    } catch (...) {
        refuse(QStringLiteral("3D zoom failed: unknown failure"));
    }
    event->accept();
}

QPaintEngine* NativeModelView::paintEngine() const {
    return nullptr;
}

}  // namespace sketch::visualization
