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
#include <Bnd_Box.hxx>
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
#include <QContextMenuEvent>
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
#include <QTouchEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <iterator>
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
            if (region.roof_opening) continue; // Rendered and picked separately.
            if (region.shape.IsNull()) continue; // Fully occluded roof member.
            builder.Add(compound, region.shape);
            has_region = true;
        }
        if (!has_region) throw std::invalid_argument("native material presentation has no visible regions");
        auto colored = occ::handle<AIS_ColoredShape>(new AIS_ColoredShape(compound));
        for (const auto& region : solid.material_regions)
            if (!region.roof_opening && !region.shape.IsNull()) colored->SetCustomColor(region.shape, region.color);
        result = colored;
    }
    result->SetColor(solid.color);
    result->SetDisplayMode(AIS_Shaded);
    return result;
}

}  // namespace

class NativeModelView::Impl {
public:
    struct RoofOpeningPresentation {
        NativeRoofOpeningTarget target;
        occ::handle<AIS_Shape> presentation;
    };
    struct CachedSolid {
        // Exact equality avoids reusing stale geometry after a hash collision.
        std::string content;
        std::string appearance_content;
        TopoDS_Shape shape;
        occ::handle<AIS_Shape> presentation;
        Quantity_Color color;
        std::vector<RoofOpeningPresentation> roof_openings;
    };

    NativeModelView* owner{};
    QLabel* status_label{};
    QFrame* selection_rectangle{};
    std::shared_ptr<const DocumentSnapshot> snapshot;
    std::shared_ptr<const DocumentSnapshot> published_snapshot;
    std::shared_ptr<const DocumentSnapshot> gesture_snapshot;
    std::shared_ptr<const DocumentSnapshot> commit_snapshot;
    // Synchronous callbacks can pump nested pointer or menu events. Each
    // callback exposes its own captured source and then restores the enclosing
    // callback's evidence, even when both captures share the same snapshot.
    class CommitSourceScope {
    public:
        explicit CommitSourceScope(NativeModelView* owner)
            : m_owner(owner), m_previous(owner->m_impl->commit_snapshot) {}
        CommitSourceScope(const CommitSourceScope&)=delete;
        CommitSourceScope& operator=(const CommitSourceScope&)=delete;
        ~CommitSourceScope() { restore(); }
        void restore() noexcept {
            if (m_restored) return;
            if (m_owner) m_owner->m_impl->commit_snapshot=std::move(m_previous);
            m_restored=true;
        }
    private:
        QPointer<NativeModelView> m_owner;
        std::shared_ptr<const DocumentSnapshot> m_previous;
        bool m_restored{};
    };
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
    occ::handle<AIS_Shape> roof_transform_proxy;
    occ::handle<WNT_Window> window;
    std::map<std::string, CachedSolid, std::less<>> solids;
    QStringList selected_entity_ids;
    std::optional<std::string> selected_entity_id;
    std::vector<NativeRoofOpeningTarget> selected_roof_openings;
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
        std::vector<NativeRoofOpeningTarget> roof_openings;
        std::uint64_t navigation_generation{};
    };
    std::optional<SelectionCapture> selection_capture;
    // Separate transient preparation never changes solids/published_snapshot.
    NativeGeometryRegenerator roof_preview_regenerator;
    QTimer* roof_preview_timer{};
    std::optional<SelectionCapture> roof_transform_capture;
    std::optional<NativeRoofOpeningTransform> roof_transform_proposal;
    std::array<double,3> roof_transform_pivot{};
    std::uint64_t roof_transform_serial{};
    std::uint64_t roof_candidate_serial{};
    std::uint64_t roof_presented_serial{};
    bool roof_restoration_needs_republication{};
    std::shared_ptr<const DocumentSnapshot> roof_candidate;
    std::optional<NativeModelView::VisibleEntityIds> roof_candidate_visible_ids;
    std::vector<NativeRoofOpeningTarget> roof_candidate_targets;
    std::vector<occ::handle<AIS_Shape>> roof_preview_presentations;
    std::vector<std::pair<occ::handle<AIS_Shape>,bool>> roof_original_visibility;
    bool roof_release_pending{};
    bool roof_committing{};
    QString roof_transform_feedback;
    bool roof_transform_metric_units{};
    double roof_transform_length_step{0.00635};
    double roof_transform_reference_length{1.0};

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
    struct TouchGesture {
        QPointer<const QPointingDevice> device;
        std::set<int> ids;
        int primary_id{};
        bool navigation{};
        std::optional<std::array<int, 2>> pair;
        QPointF centroid;
        double distance{};
        double zoom_remainder{};
        SelectionCapture context;
    };
    std::optional<TouchGesture> touch;
    int touch_dispatch_depth{};
    bool touch_dispatch_retired{};
    QPointer<const QPointingDevice> touch_dispatch_device;
    std::chrono::steady_clock::time_point touch_mouse_suppression_until;
    QPoint navigation_start;
    QPointF left_press;
    bool left_moved{};
    bool space_pan_armed{};
    bool keyboard_context_dispatch_active{};
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
        roof_preview_timer = new QTimer(widget);
        roof_preview_timer->setInterval(10);
        QObject::connect(roof_preview_timer, &QTimer::timeout, widget,
                         [this] { collect_roof_preview(); });
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
        // The generation is a permanent fence even if the camera later returns
        // to identical coordinates. Input handlers retire before navigation;
        // the preview timer also restores any independently invalidated scene.
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
        status_label->setText(text.isEmpty() ? roof_transform_feedback : text);
        status_label->setVisible(!text.isEmpty() || !roof_transform_feedback.isEmpty());
        status_label->raise();
        auto bounds = owner->rect().adjusted(12, 12, -12, -12);
        if ((regenerator.is_pending() || !input_error.isEmpty() || !export_error.isEmpty() || !roof_transform_feedback.isEmpty()) &&
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
            // A failed transient rollback owns native handles until cleanup
            // actually succeeds. A new source cannot certify that stale scene.
            if (!restore_roof_preview()) { prepared_geometry.reset(); return; }

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
                    metrics.reused += 1 + cached->second.roof_openings.size();
                    continue;
                }
                auto presentation = prepared_presentation(solid);
                std::vector<RoofOpeningPresentation> roof_openings;
                for (const auto& region : solid.material_regions) {
                    if (!region.roof_opening) continue;
                    if (region.shape.IsNull() || region.roof_opening->roof_id.empty() ||
                        region.roof_opening->opening_id.empty())
                        throw std::invalid_argument("native roof opening presentation lacks topology or provenance");
                    NativeRoofOpeningTarget target{QString::fromStdString(region.roof_opening->roof_id),
                                                   QString::fromStdString(region.roof_opening->opening_id)};
                    if (std::any_of(roof_openings.begin(),roof_openings.end(),[&](const auto& child) {
                            return child.target == target;
                        })) throw std::invalid_argument("native roof opening presentation has duplicate provenance");
                    auto fill = occ::handle<AIS_Shape>(new AIS_Shape(region.shape));
                    fill->SetColor(region.color);
                    fill->SetDisplayMode(AIS_Shaded);
                    roof_openings.push_back({std::move(target),std::move(fill)});
                }
                metrics.created += 1 + roof_openings.size();
                // Appearance-only publication consumes fresh region topology,
                // while the retained fused/assembly shape remains authoritative.
                auto geometry_shape = same_geometry ? cached->second.shape : std::move(solid.shape);
                replacement.emplace(id, CachedSolid{std::move(solid.content), std::move(solid.appearance_content),
                                                   std::move(geometry_shape),
                                                   presentation, solid.color, std::move(roof_openings)});
            }
            std::map<std::string, bool, std::less<>> previous_visibility;
            std::map<std::pair<std::string,std::size_t>,bool> previous_child_visibility;
            for (const auto& [id, solid] : solids) {
                previous_visibility.emplace(id, context->IsDisplayed(solid.presentation));
                for (std::size_t index=0; index<solid.roof_openings.size(); ++index)
                    previous_child_visibility.emplace(std::pair{id,index},context->IsDisplayed(solid.roof_openings[index].presentation));
                const auto next = replacement.find(id);
                if (next == replacement.end() || next->second.presentation != solid.presentation)
                    metrics.removed += 1 + solid.roof_openings.size();
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
                    for (const auto& child : solid.roof_openings) {
                        if (visible != context->IsDisplayed(child.presentation)) {
                            if (visible) context->Display(child.presentation,false);
                            else context->Erase(child.presentation,false);
                        }
                    }
                }
                // All candidates have been displayed successfully before any
                // obsolete object is detached. Unchanged handles stay registered.
                for (const auto& [id, solid] : solids) {
                    const auto next = replacement.find(id);
                    if (next == replacement.end() || next->second.presentation != solid.presentation) {
                        context->Remove(solid.presentation, false);
                        for (const auto& child : solid.roof_openings) context->Remove(child.presentation,false);
                    }
                }
                // A changed regional appearance replaces its AIS handle; keep
                // the semantic selection attached to the new root presentation.
                restore_selection_highlights(replacement);
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
                        for (const auto& child : solid.roof_openings)
                            try { context->Remove(child.presentation,false); } catch (...) {}
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
                    for (std::size_t index=0; index<solid.roof_openings.size(); ++index) {
                        try {
                            if (previous_child_visibility.at(std::pair{id,index}))
                                context->Display(solid.roof_openings[index].presentation,false);
                            else context->Erase(solid.roof_openings[index].presentation,false);
                        } catch (...) {}
                    }
                }
                has_fit = previously_fit;
                initial_fit_pending = previously_pending_fit;
                try {
                    restore_selection_highlights();
                } catch (...) {}
                try { attach_manipulator(); } catch (...) {}
                try { viewer->Redraw(); } catch (...) {}
                throw;
            }
            solids.swap(replacement);
            published_snapshot = std::move(published_source);
            roof_restoration_needs_republication=false;
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

    struct PickTarget {
        QString entity_id;
        std::optional<NativeRoofOpeningTarget> roof_opening;
        bool operator==(const PickTarget&) const = default;
        bool empty() const { return entity_id.isEmpty() && !roof_opening; }
    };

    template <typename PresentationHandle>
    PickTarget target_for_presentation(const PresentationHandle& selected) const {
        if (selected.IsNull() || context.IsNull()) return {};
        for (const auto& [id, solid] : solids) {
            for (const auto& child : solid.roof_openings)
                if (child.presentation == selected && context->IsDisplayed(child.presentation))
                    return {{},child.target};
            if (solid.presentation == selected && !context.IsNull() && context->IsDisplayed(solid.presentation)) {
                return {QString::fromStdString(id),std::nullopt};
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
            // Resolve the exact alias generated for this captured source,
            // including escaped collision aliases. Only geometric instances
            // gain direct transform authority; metadata-only entries do not.
            try {
                const auto embedded = resolve_embedded_assembly_presentation(snapshot->entities(), id);
                if (!embedded) return false;
                const auto& catalog = snapshot->entities().at(embedded->assembly_catalog_id);
                const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
                return !model.expand(embedded->instance.id).profiles.empty();
            } catch (...) { return false; }
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
        if (!roof_transform_proxy.IsNull() && !context.IsNull()) {
            try { context->Remove(roof_transform_proxy,false); } catch (...) {}
        }
        roof_transform_proxy.Nullify();
    }

    void attach_manipulator() {
        if (roof_transform_capture) return;
        const bool children = !selected_roof_openings.empty();
        if ((!children && (selected_entity_ids.size() != 1 || !selected_entity_id.has_value() || !supports_direct_transform(*selected_entity_id))) ||
            !native_ready || !geometry_prepared || regenerator.is_pending() || prepared_geometry ||
            !geometry_status.isEmpty() || context.IsNull() || viewer.IsNull()) {
            detach_manipulator();
            return;
        }
        const auto found = children ? solids.end() : solids.find(*selected_entity_id);
        if (!children && (found == solids.end() || found->second.presentation.IsNull() ||
            !context->IsDisplayed(found->second.presentation))) {
            detach_manipulator();
            return;
        }
        if (!children && manipulator_entity_id == selected_entity_id && !manipulator.IsNull() &&
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
        manipulator->SetPart(2, AIS_MM_Translation, !children);
        if (children) {
            if (!published_snapshot || !published_snapshot->is_editable() ||
                !owner->onRoofOpeningTransformPreviewRequested || !owner->onRoofOpeningTransformRequested) return;
            TopoDS_Compound compound;
            BRep_Builder builder;
            builder.MakeCompound(compound);
            Bnd_Box bounds;
            for (const auto& target : selected_roof_openings) {
                const auto child = roof_opening_presentation(target);
                if (child.IsNull() || child->Shape().IsNull()) return;
                const auto box = child->BoundingBox().Transformed(child->Transformation());
                if (box.IsVoid() || box.IsOpen()) return;
                bounds.Add(box);
                builder.Add(compound,child->Shape());
            }
            const auto low = bounds.CornerMin(), high = bounds.CornerMax();
            roof_transform_pivot = {(low.X()+high.X())*0.5,(low.Y()+high.Y())*0.5,(low.Z()+high.Z())*0.5};
            roof_transform_reference_length = std::max({high.X()-low.X(),high.Y()-low.Y(),high.Z()-low.Z()});
            if (!std::isfinite(roof_transform_reference_length) || roof_transform_reference_length <= 1.0e-9) return;
            // This undisplayed proxy is the only affine-transformed object.
            // Actual hosts and fills wait for manufactured candidate geometry.
            roof_transform_proxy = new AIS_Shape(compound);
            context->Load(roof_transform_proxy,-1);
            group->Append(roof_transform_proxy);
        } else {
            for (const auto& id : transform_presentation_ids(*selected_entity_id)) {
                group->Append(solids.at(id).presentation);
                for (const auto& child : solids.at(id).roof_openings) group->Append(child.presentation);
            }
        }
        manipulator->Attach(group, options);
        if (children) context->Display(manipulator,false);
        if (!children) manipulator_entity_id = selected_entity_id;
        restore_selection_highlights();
        viewer->Redraw();
    }

    bool begin_manipulation(const NativeInputPoint point) {
        if (roof_release_pending || manipulator.IsNull() || !manipulator->IsAttached() ||
            (selected_roof_openings.empty() && manipulator_entity_id != selected_entity_id) || context.IsNull() || view.IsNull())
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

    void preview_manipulation(const NativeInputPoint point, Qt::KeyboardModifiers modifiers = {}) {
        if (manipulator.IsNull() || !manipulator->HasActiveTransformation() || view.IsNull()) return;
        const QPointer<NativeModelView> guard(owner);
        const bool child_gesture = roof_transform_capture.has_value();
        const auto fail = [this,guard,child_gesture](const QString& message) {
            if (!guard) return;
            if (child_gesture) {
                owner->cancelInteraction();
                if (guard) show_input_error(message);
            } else show_operation_error(message);
        };
        try {
            manipulation_transform = manipulator->Transform(point.x, point.y, view);
            if (roof_transform_capture) request_roof_preview(modifiers);
            if (!guard) return;
            if (!viewer.IsNull()) viewer->RedrawImmediate();
        } catch (const Standard_Failure& error) {
            fail(QStringLiteral("3D transform preview failed: ") + exception_text(error));
        } catch (const std::exception& error) {
            fail(QStringLiteral("3D transform preview failed: ") + exception_text(error));
        } catch (...) {
            fail(QStringLiteral("3D transform preview failed: unknown failure"));
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
                for (const auto& child : found->second.roof_openings) {
                    child.presentation->ResetTransformation();
                    if (native_ready && !context.IsNull()) context->Redisplay(child.presentation,false);
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
            for (const auto& child : member->second.roof_openings) {
                child.presentation->SetLocalTransformation(transform);
                if (native_ready && !context.IsNull()) context->Redisplay(child.presentation,false);
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
                QSize(width,height),input_scale(),selected_entity_ids,selected_roof_openings,navigation_generation};
    }

    bool selection_current(const SelectionCapture& capture) const {
        if (!owner->isVisible() || !owner->isReady() || !capture.source || published_snapshot != capture.source ||
            capture.navigation_generation != navigation_generation ||
            navigation_generation == std::numeric_limits<std::uint64_t>::max() ||
            view.IsNull() || view->Camera().IsNull() || owner->size() != capture.logical_size ||
            input_scale() != capture.pixel_ratio || selected_entity_ids != capture.selection ||
            selected_roof_openings != capture.roof_openings ||
            view->Camera()->WorldViewProjState() != capture.camera) return false;
        int width=0, height=0;
        if (!window.IsNull()) window->Size(width,height);
        return QSize(width,height)==capture.native_size;
    }

    bool roof_preview_current(std::uint64_t serial) const {
        return serial != 0 && serial == roof_transform_serial && roof_transform_proposal &&
            roof_transform_capture && owner->hasFocus() && !QApplication::activeModalWidget() &&
            selection_current(*roof_transform_capture);
    }

    bool restore_roof_preview() noexcept {
        if (roof_preview_presentations.empty() && roof_original_visibility.empty()) {
            roof_presented_serial=0;
            return true;
        }
        bool restored=!context.IsNull();
        if (!context.IsNull()) {
            for (const auto& presentation : roof_preview_presentations)
                try { context->Remove(presentation,false); } catch (...) { restored=false; }
            for (const auto& [presentation,visible] : roof_original_visibility)
                try {
                    if (visible) context->Display(presentation,false);
                    else context->Erase(presentation,false);
                } catch (...) { restored=false; }
        }
        roof_presented_serial = 0;
        try { restore_selection_highlights(); } catch (...) { restored=false; }
        try { if (!view.IsNull()) view->Redraw(); } catch (...) { restored=false; }
        if (!restored) {
            roof_restoration_needs_republication=true;
            geometry_prepared=false;
            published_snapshot.reset();
            // Preserve rollback handles for the next real source publication.
            // Readiness and input authority stay false until cleanup succeeds.
            operation_error=QStringLiteral("The 3D preview could not restore the displayed scene. Refresh the project view before editing.");
            try { refresh_status_label(); } catch (...) {}
            return false;
        }
        roof_preview_presentations.clear();
        roof_original_visibility.clear();
        return true;
    }

    void retire_roof_transform() noexcept {
        const bool notify = roof_transform_capture.has_value() && !roof_committing;
        roof_transform_capture.reset();
        roof_transform_proposal.reset();
        roof_release_pending = false;
        roof_candidate_serial = 0;
        roof_candidate.reset();
        roof_candidate_visible_ids.reset();
        roof_candidate_targets.clear();
        roof_preview_timer->stop();
        roof_transform_feedback.clear();
        (void)restore_roof_preview();
        try { refresh_status_label(); } catch (...) {}
        // Clear all ownership before a cancellation observer can reenter.
        if (notify) {
            try { const auto callback = owner->onRoofOpeningTransformCanceled; if (callback) callback(); }
            catch (...) {}
        }
    }

    void request_roof_preview(Qt::KeyboardModifiers modifiers) {
        if (!roof_transform_capture || !selection_current(*roof_transform_capture) ||
            !owner->hasFocus() || QApplication::activeModalWidget()) {
            owner->cancelInteraction();
            return;
        }
        const QPointer<NativeModelView> guard(owner);
        CommitSourceScope source_scope(owner);
        commit_snapshot = roof_transform_capture->source;
        if (!owner->admitSceneInput(false) || !guard || !roof_transform_capture) return;
        if (!manipulation_transform) return;
        const auto& transform = *manipulation_transform;
        gp_XYZ axis;
        double angle = 0.0;
        if (transform.GetRotation(axis,angle) && axis.Z() < 0.0) angle = -angle;
        const auto scale = transform.ScaleFactor();
        constexpr double epsilon = 1.0e-9;
        if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(angle) ||
            (std::abs(angle) > epsilon && (std::abs(axis.X()) > epsilon || std::abs(axis.Y()) > epsilon ||
             std::abs(std::abs(axis.Z())-1.0) > epsilon))) {
            owner->cancelInteraction();
            return;
        }
        auto pivot = gp_Pnt(roof_transform_pivot[0],roof_transform_pivot[1],roof_transform_pivot[2]);
        const auto moved = pivot.Transformed(transform);
        NativeRoofOpeningTransform proposal{roof_transform_capture->roof_openings,roof_transform_pivot,
            {moved.X()-pivot.X(),moved.Y()-pivot.Y(),0.0},angle,scale};
        if (std::abs(moved.Z()-pivot.Z()) > epsilon ||
            !std::isfinite(proposal.translation_world_m[0]) || !std::isfinite(proposal.translation_world_m[1])) {
            owner->cancelInteraction(); return;
        }
        constexpr double pi = 3.14159265358979323846;
        const bool fine = modifiers.testFlag(Qt::ShiftModifier);
        const auto angle_step = fine ? pi/180.0 : pi/4.0;
        proposal.rotation_radians = std::round(angle/angle_step)*angle_step;
        const double step = fine ? (roof_transform_metric_units ? 0.001 : 0.00635) : roof_transform_length_step;
        for (int i=0;i<2;++i) proposal.translation_world_m[i] =
            std::round(proposal.translation_world_m[i]/step)*step;
        if (std::abs(scale-1.0) > epsilon) {
            const auto length = std::max(step,std::round(roof_transform_reference_length*scale/step)*step);
            proposal.uniform_scale = length/roof_transform_reference_length;
            if (!std::isfinite(proposal.uniform_scale) || proposal.uniform_scale <= 0.0) {
                owner->cancelInteraction(); return;
            }
        }
        if (roof_transform_proposal && *roof_transform_proposal == proposal) return;
        if (roof_transform_serial == std::numeric_limits<std::uint64_t>::max()) {
            owner->cancelInteraction(); return;
        }
        ++roof_transform_serial;
        roof_transform_proposal = proposal;
        roof_candidate_serial = 0;
        roof_candidate.reset();
        roof_candidate_visible_ids.reset();
        roof_candidate_targets.clear();
        // Never retain an older admitted presentation under a newer proposal.
        if (!restore_roof_preview()) { owner->cancelInteraction(); return; }
        const double display_factor = roof_transform_metric_units ? 1.0 : 1.0/0.0254;
        roof_transform_feedback = QStringLiteral("Skylights: ΔX %1 %5 · ΔY %2 %5 · %3° · size %4 %5 — checking")
            .arg(proposal.translation_world_m[0]*display_factor,0,'f',3)
            .arg(proposal.translation_world_m[1]*display_factor,0,'f',3)
            .arg(proposal.rotation_radians*180.0/pi,0,'f',1)
            .arg(roof_transform_reference_length*proposal.uniform_scale*display_factor,0,'f',3)
            .arg(roof_transform_metric_units ? QStringLiteral("m") : QStringLiteral("in"));
        refresh_status_label();
        try {
            const auto callback = owner->onRoofOpeningTransformPreviewRequested;
            if (callback) callback(proposal,roof_transform_serial);
        } catch (...) {
            if (guard) owner->cancelInteraction();
        }
    }

    void commit_roof_preview() {
        if (!roof_release_pending || roof_presented_serial != roof_transform_serial ||
            !roof_preview_current(roof_transform_serial)) return;
        const auto proposal = *roof_transform_proposal;
        const auto serial = roof_transform_serial;
        const auto source = roof_transform_capture->source;
        const QPointer<NativeModelView> guard(owner);
        CommitSourceScope source_scope(owner);
        commit_snapshot = source;
        if (!owner->admitSceneInput(false) || !guard || !roof_preview_current(serial)) return;
        const auto callback = owner->onRoofOpeningTransformRequested;
        // One release invokes exactly one commit observer, including refusal.
        roof_release_pending = false;
        roof_committing = true;
        bool accepted = false;
        try { if (callback) accepted = callback(proposal,serial); } catch (...) {}
        if (!guard) return;
        roof_committing = false;
        owner->cancelInteraction();
        if (!guard) return;
        if (!accepted) show_input_error(QStringLiteral("Skylight transform was refused. Start again."));
    }

    void collect_roof_preview() noexcept {
        const QPointer<NativeModelView> guard(owner);
        // A retired native request can fail while a newer pointer proposal is
        // still awaiting its root candidate. Its failure owns only the bound
        // candidate serial, never the newest gesture/proposal serial.
        const auto collecting_serial=roof_candidate_serial;
        try {
            if (roof_transform_capture && (!owner->hasFocus() || QApplication::activeModalWidget() ||
                !selection_current(*roof_transform_capture))) { owner->cancelInteraction(); return; }
            auto prepared = roof_preview_regenerator.take_completed();
            if (!roof_preview_regenerator.is_pending() && !roof_transform_capture) roof_preview_timer->stop();
            if (!prepared || !roof_candidate || !roof_preview_current(roof_candidate_serial)) return;
            const auto serial = roof_candidate_serial;
            if (prepared->revision != roof_candidate->revision() || prepared->visible_ids != roof_candidate_visible_ids ||
                !prepared->errors.empty() || !prepared->pending.empty()) {
                (void)owner->rejectRoofOpeningTransformPreview(serial,
                    QStringLiteral("The candidate skylight geometry is incomplete."));
                return;
            }
            std::vector<std::pair<occ::handle<AIS_Shape>,bool>> next;
            std::vector<occ::handle<AIS_Shape>> selected_fills;
            std::vector<NativeRoofOpeningTarget> found_targets;
            for (const auto& [id,solid] : prepared->solids) {
                next.emplace_back(prepared_presentation(solid),solid.visible);
                for (const auto& region : solid.material_regions) {
                    if (!region.roof_opening) continue;
                    if (region.shape.IsNull()) throw std::invalid_argument("candidate skylight topology is absent");
                    NativeRoofOpeningTarget target{QString::fromStdString(region.roof_opening->roof_id),
                        QString::fromStdString(region.roof_opening->opening_id)};
                    auto fill = occ::handle<AIS_Shape>(new AIS_Shape(region.shape));
                    fill->SetColor(region.color);
                    fill->SetDisplayMode(AIS_Shaded);
                    next.emplace_back(fill,solid.visible);
                    if (solid.visible) found_targets.push_back(target);
                    if (solid.visible && std::find(roof_candidate_targets.begin(),roof_candidate_targets.end(),target) != roof_candidate_targets.end())
                        selected_fills.push_back(fill);
                }
            }
            for (const auto& target : roof_candidate_targets)
                if (std::find(found_targets.begin(),found_targets.end(),target) == found_targets.end())
                    throw std::invalid_argument("candidate skylight cohort lacks visible native provenance");
            if (!restore_roof_preview()) { owner->cancelInteraction(); return; }
            for (const auto& [id,solid] : solids) {
                roof_original_visibility.emplace_back(solid.presentation,context->IsDisplayed(solid.presentation));
                for (const auto& child : solid.roof_openings)
                    roof_original_visibility.emplace_back(child.presentation,context->IsDisplayed(child.presentation));
            }
            // Register rollback ownership before any display operation can fail.
            for (const auto& [presentation,visible] : next) roof_preview_presentations.push_back(presentation);
            for (const auto& [presentation,visible] : next) if (visible) context->Display(presentation,false);
            for (const auto& [presentation,visible] : roof_original_visibility) if (visible) context->Erase(presentation,false);
            context->ClearSelected(false);
            for (const auto& fill : selected_fills) context->AddOrRemoveSelected(fill,false);
            viewer->Redraw();
            if (!roof_preview_current(serial)) { owner->cancelInteraction(); return; }
            roof_presented_serial = serial;
            roof_transform_feedback.replace(QStringLiteral(" — preparing"),QStringLiteral(" — preview"));
            refresh_status_label();
            commit_roof_preview();
        } catch (const Standard_Failure& error) {
            if (guard && collecting_serial && roof_preview_current(collecting_serial))
                (void)owner->rejectRoofOpeningTransformPreview(collecting_serial,exception_text(error));
        } catch (const std::exception& error) {
            if (guard && collecting_serial && roof_preview_current(collecting_serial))
                (void)owner->rejectRoofOpeningTransformPreview(collecting_serial,exception_text(error));
        } catch (...) {
            if (guard && collecting_serial && roof_preview_current(collecting_serial))
                (void)owner->rejectRoofOpeningTransformPreview(collecting_serial,QStringLiteral("Native skylight preview failed."));
        }
    }

    occ::handle<AIS_Shape> roof_opening_presentation(const NativeRoofOpeningTarget& target) const {
        if (context.IsNull()) return {};
        for (const auto& [id,solid] : solids)
            for (const auto& child : solid.roof_openings)
                if (child.target == target && context->IsDisplayed(child.presentation)) return child.presentation;
        return {};
    }

    QPoint keyboard_context_anchor(const SelectionCapture& capture, const QRect& visible) const {
        const auto fallback=visible.center();
        if (capture.native_size.isEmpty()) return fallback;
        std::vector<occ::handle<AIS_Shape>> presentations;
        if (!capture.roof_openings.empty()) {
            for (const auto& target : capture.roof_openings) {
                const auto presentation=roof_opening_presentation(target);
                if (!presentation.IsNull()) presentations.push_back(presentation);
            }
        } else if (!capture.selection.isEmpty()) {
            const auto found=solids.find(capture.selection.back().toStdString());
            if (found!=solids.end()) presentations.push_back(found->second.presentation);
        }
        // Use the displayed shape (including material-region compounds), not
        // document coordinates or a newly prepared solid. AIS shape bounds are
        // local; apply the presentation's complete parent/local transformation.
        double left=std::numeric_limits<double>::infinity();
        double top=left;
        double right=-left;
        double bottom=right;
        for (const auto& presentation : presentations) {
            if (presentation.IsNull() || presentation->Shape().IsNull() ||
                !context->IsDisplayed(presentation)) continue;
            const auto& bounds=presentation->BoundingBox();
            if (bounds.IsVoid() || bounds.IsOpen()) return fallback;
            const auto low=bounds.CornerMin();
            const auto high=bounds.CornerMax();
            double member_left=std::numeric_limits<double>::infinity();
            double member_top=member_left;
            double member_right=-member_left;
            double member_bottom=member_right;
            for (int corner=0;corner<8;++corner) {
                auto world=gp_Pnt((corner&1) ? high.X() : low.X(),
                                  (corner&2) ? high.Y() : low.Y(),
                                  (corner&4) ? high.Z() : low.Z());
                world.Transform(presentation->Transformation());
                if (!std::isfinite(world.X()) || !std::isfinite(world.Y()) || !std::isfinite(world.Z()))
                    return fallback;
                const auto projected=view->Camera()->Project(world);
                // Keep projection floating point until visible clipping so
                // far-offscreen shapes cannot overflow integer conversion.
                const auto x=(projected.X()+1.0)*0.5*capture.native_size.width()/capture.pixel_ratio;
                const auto y=(capture.native_size.height()-1.0-
                    (projected.Y()+1.0)*0.5*capture.native_size.height())/capture.pixel_ratio;
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(projected.Z()) ||
                    projected.Z() < -1.0 || projected.Z() > 1.0) return fallback;
                member_left=std::min(member_left,x);
                member_top=std::min(member_top,y);
                member_right=std::max(member_right,x);
                member_bottom=std::max(member_bottom,y);
            }
            if (member_right<visible.left() || member_left>visible.right() ||
                member_bottom<visible.top() || member_top>visible.bottom()) continue;
            left=std::min(left,std::max(member_left,static_cast<double>(visible.left())));
            right=std::max(right,std::min(member_right,static_cast<double>(visible.right())));
            top=std::min(top,std::max(member_top,static_cast<double>(visible.top())));
            bottom=std::max(bottom,std::min(member_bottom,static_cast<double>(visible.bottom())));
        }
        if (!std::isfinite(left)) return fallback;
        // Center the union of visible selected fills in Qt logical pixels.
        return QPoint(qBound(visible.left(),qRound(left+(right-left)*0.5),visible.right()),
                      qBound(visible.top(),qRound(top+(bottom-top)*0.5),visible.bottom()));
    }

    void restore_selection_highlights(const std::map<std::string,CachedSolid,std::less<>>& scene) {
        if (context.IsNull()) return;
        context->ClearSelected(false);
        if (!selected_roof_openings.empty()) {
            for (const auto& [id,solid] : scene)
                for (const auto& child : solid.roof_openings)
                    if (std::find(selected_roof_openings.begin(),selected_roof_openings.end(),child.target)!=selected_roof_openings.end() &&
                        context->IsDisplayed(child.presentation))
                        context->AddOrRemoveSelected(child.presentation,false);
            return;
        }
        for (const auto& id : selected_entity_ids) {
            const auto found=scene.find(id.toStdString());
            if (found!=scene.end() && !found->second.presentation.IsNull() &&
                context->IsDisplayed(found->second.presentation)) {
                context->AddOrRemoveSelected(found->second.presentation,false);
                for (const auto& child : found->second.roof_openings)
                    if (context->IsDisplayed(child.presentation)) context->AddOrRemoveSelected(child.presentation,false);
            }
        }
    }
    void restore_selection_highlights() { restore_selection_highlights(solids); }

    std::optional<PickTarget> select_at(const NativeInputPoint point, const SelectionCapture& capture,
                                      bool editing=false, bool toggle=false, bool cycle=false) {
        const QPointer<NativeModelView> guard(owner);
        try {
            if (!selection_current(capture) || !owner->admitSceneInput(false) || !guard ||
                !selection_current(capture)) return std::nullopt;
            context->MoveTo(point.x,point.y,view,false);
            PickTarget target;
            if (cycle && !toggle) {
                // OCCT's detected sequence is ordered by the actual pick. A
                // material/face owner may repeat the same semantic object.
                std::vector<PickTarget> targets;
                for (context->InitDetected();context->MoreDetected();context->NextDetected()) {
                    const auto detected=context->DetectedCurrentOwner();
                    if (detected.IsNull() || !detected->HasSelectable()) continue;
                    const auto hit=target_for_presentation(
                        occ::handle<AIS_InteractiveObject>::DownCast(detected->Selectable()));
                    if (!hit.empty() && std::find(targets.begin(),targets.end(),hit)==targets.end()) targets.push_back(hit);
                }
                if (!targets.empty()) {
                    const auto primary_child=capture.roof_openings.empty() ? std::optional<NativeRoofOpeningTarget>{}
                        : std::optional<NativeRoofOpeningTarget>{capture.roof_openings.back()};
                    const PickTarget primary{primary_child || capture.selection.isEmpty() ? QString{} : capture.selection.back(),primary_child};
                    const auto current=std::find(targets.begin(),targets.end(),primary);
                    target=current==targets.end() || std::next(current)==targets.end() ? targets.front() : *std::next(current);
                }
            } else if (context->HasDetected()) target=target_for_presentation(context->DetectedInteractive());
            if ((editing || toggle || cycle) && target.empty()) return target;
            if (!selection_current(capture)) return std::nullopt;
            if (target.roof_opening) {
                const auto child=*target.roof_opening;
                const bool retained=!toggle && !cycle &&
                    std::find(selected_roof_openings.begin(),selected_roof_openings.end(),child)!=selected_roof_openings.end();
                const auto clicked=owner->onRoofOpeningSelectionClicked;
                const auto cycled=owner->onRoofOpeningSelectionCycled;
                if (!retained) {
                    auto next=toggle ? selected_roof_openings : std::vector<NativeRoofOpeningTarget>{};
                    const auto member=std::find(next.begin(),next.end(),child);
                    if (toggle && member!=next.end()) next.erase(member);
                    else next.push_back(child);
                    owner->setSelectedRoofOpenings(std::move(next));
                    if (!guard) return target;
                }
                if (cycle && !toggle && cycled) cycled(child);
                else if (clicked) clicked(child,toggle);
                if (!guard) return target;
                auto continuation=capture;
                continuation.selection=selected_entity_ids;
                continuation.roof_openings=selected_roof_openings;
                if (!selection_current(continuation)) return std::nullopt;
                if (editing &&
                    std::find(selected_roof_openings.begin(),selected_roof_openings.end(),child)!=selected_roof_openings.end() &&
                    !roof_opening_presentation(child).IsNull()) {
                    const auto callback=owner->onRoofOpeningEditRequested;
                    if (callback) callback(child);
                }
                return target;
            }
            const auto& id=target.entity_id;
            // Preserve selected groups on their first plain click, as Qt sends
            // that release before a possible double-click. Context uses this
            // same policy; its selected member never replaces the group.
            const bool retained=selected_roof_openings.empty() && !toggle && !cycle && !id.isEmpty() && selected_entity_ids.contains(id);
            if (!retained) {
                auto next=toggle ? selected_entity_ids : QStringList{};
                if (toggle && next.contains(id)) next.removeAll(id);
                else if (!id.isEmpty()) next.append(id);
                const auto clicked=owner->onEntitySelectionClicked;
                const auto legacy=owner->onEntitySelected;
                owner->setSelectedEntities(next);
                if (!guard) return target;
                if (clicked) clicked(id,toggle);
                else if (legacy) legacy(toggle && !next.contains(id) ? QString{} : id);
            }
            if (!guard) return target;
            auto continuation=capture;
            continuation.selection=selected_entity_ids;
            continuation.roof_openings=selected_roof_openings;
            if (!selection_current(continuation)) return std::nullopt;
            if (editing && !id.isEmpty() && selected_roof_openings.empty() && selected_entity_ids.contains(id)) {
                const auto callback=owner->onEntityEditRequested;
                if (callback) callback(id);
            }
            return target;
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
            std::vector<NativeRoofOpeningTarget> children;
            for (context->InitSelected();context->MoreSelected();context->NextSelected()) {
                const auto target=target_for_presentation(context->SelectedInteractive());
                if (target.roof_opening) {
                    if (std::find(children.begin(),children.end(),*target.roof_opening)==children.end())
                        children.push_back(*target.roof_opening);
                } else if (!target.entity_id.isEmpty()) selected.insert(target.entity_id);
            }
            restore_selection_highlights();
            if (!selection_current(capture)) return;
            QStringList hits;
            auto next=selected_entity_ids;
            for (const auto& id : selected) {
                hits.append(id);
                if (!next.contains(id)) next.append(id);
            }
            std::sort(children.begin(),children.end(),[](const auto& left,const auto& right) {
                return left.roof_id==right.roof_id ? left.opening_id<right.opening_id : left.roof_id<right.roof_id;
            });
            const auto combined=owner->onSelectionMarqueeRequested;
            if (combined) {
                // The shell owns grouped/mixed child policy. Dispatch once and
                // retain selection until it atomically accepts the complete set.
                combined(hits,children,true);
                return;
            }
            if (!children.empty()) {
                show_input_error(QStringLiteral("3D skylight marquee selection requires a semantic selection handler"));
                return;
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
    // Own actual touch sequences; accepting them prevents Qt's mouse fallback
    // from duplicating a touch selection or an object-edit completion.
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
    m_impl->roof_preview_timer->stop();
    m_impl->roof_preview_regenerator.shutdown();
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
    if (!m_impl->roof_restoration_needs_republication && m_impl->snapshot &&
        m_impl->snapshot->document_id() == snapshot.document_id() &&
        m_impl->snapshot->revision() == snapshot.revision() &&
        m_impl->visible_ids == visible_ids &&
        (m_impl->snapshot->shares_full_snapshot_with(snapshot) ||
         (same_snapshot_content(*m_impl->snapshot, snapshot) &&
          document_snapshot_digest(*m_impl->snapshot) == document_snapshot_digest(snapshot)))) {
        return;
    }

    auto requested_source = std::make_shared<const DocumentSnapshot>(snapshot);
    const QPointer<NativeModelView> cancellation_guard(this);
    cancelInteraction();
    if (!cancellation_guard) return;
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
    setSemanticSelections(entity_ids,{});
}

void NativeModelView::setSemanticSelection(const QStringList& entity_ids,
    std::optional<NativeRoofOpeningTarget> target) {
    std::vector<NativeRoofOpeningTarget> targets;
    if (target) targets.push_back(std::move(*target));
    setSemanticSelections(entity_ids,std::move(targets));
}

void NativeModelView::setSemanticSelections(const QStringList& entity_ids,
    std::vector<NativeRoofOpeningTarget> targets) {
    QStringList normalized;
    for (const auto& raw : entity_ids) {
        const auto id=raw.trimmed();
        if (!id.isEmpty() && !normalized.contains(id)) normalized.append(id);
    }
    std::vector<NativeRoofOpeningTarget> normalized_targets;
    normalized_targets.reserve(targets.size());
    for (auto& target : targets) {
        if (!target.roof_id.isEmpty() && !target.opening_id.isEmpty() &&
            std::find(normalized_targets.begin(),normalized_targets.end(),target)==normalized_targets.end())
            normalized_targets.push_back(std::move(target));
    }
    const bool changed=normalized!=m_impl->selected_entity_ids || m_impl->selected_roof_openings!=normalized_targets;
    m_impl->selected_roof_openings=std::move(normalized_targets);
    m_impl->selected_entity_ids=normalized;
    m_impl->selected_entity_id=normalized.isEmpty() ? std::nullopt
        : std::optional<std::string>{normalized.back().toStdString()};
    const QPointer<NativeModelView> guard(this);
    try {
        // Publish logical intent even if restoring a derived driver preview
        // fails. The failure is contained below and retires input readiness.
        if (changed) cancelInteraction();
        if (!guard) return;
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

void NativeModelView::setSelectedRoofOpening(std::optional<NativeRoofOpeningTarget> target) {
    setSemanticSelection(m_impl->selected_entity_ids,std::move(target));
}

void NativeModelView::setSelectedRoofOpenings(std::vector<NativeRoofOpeningTarget> targets) {
    setSemanticSelections(m_impl->selected_entity_ids,std::move(targets));
}

bool NativeModelView::transformControlsVisible() const noexcept {
    return ((!m_impl->selected_roof_openings.empty() && !m_impl->roof_transform_proxy.IsNull()) ||
           (m_impl->selected_roof_openings.empty() && m_impl->selected_entity_ids.size() == 1 && m_impl->selected_entity_id.has_value() &&
           m_impl->manipulator_entity_id == m_impl->selected_entity_id)) &&
           !m_impl->manipulator.IsNull() && m_impl->manipulator->IsAttached();
}

bool NativeModelView::roofOpeningTransformPreviewCurrent(std::uint64_t serial) const noexcept {
    try { return m_impl->roof_preview_current(serial); } catch (...) { return false; }
}

bool NativeModelView::completeRoofOpeningTransformPreview(
    std::uint64_t serial, const DocumentSnapshot& candidate,
    std::vector<NativeRoofOpeningTarget> remapped_targets,
    VisibleEntityIds candidate_visible_ids) {
    if (QThread::currentThread() != thread() || !roofOpeningTransformPreviewCurrent(serial)) return false;
    if (!candidate.is_editable() || candidate.document_id() != m_impl->roof_transform_capture->source->document_id() ||
        remapped_targets.size() != m_impl->roof_transform_proposal->targets.size()) {
        (void)rejectRoofOpeningTransformPreview(serial,QStringLiteral("The skylight candidate source or cohort is invalid."));
        return false;
    }
    for (std::size_t i=0;i<remapped_targets.size();++i) {
        if (remapped_targets[i].roof_id.isEmpty() || remapped_targets[i].opening_id.isEmpty() ||
            std::find(remapped_targets.begin(),remapped_targets.begin()+static_cast<std::ptrdiff_t>(i),remapped_targets[i]) !=
                remapped_targets.begin()+static_cast<std::ptrdiff_t>(i)) {
            (void)rejectRoofOpeningTransformPreview(serial,QStringLiteral("The skylight candidate has invalid child identities."));
            return false;
        }
    }
    // Duplicate completions cannot replace an already bound candidate source.
    if (m_impl->roof_candidate_serial == serial) return false;
    try {
        auto source = std::make_shared<const DocumentSnapshot>(candidate);
        m_impl->roof_preview_regenerator.request(candidate,candidate_visible_ids);
        m_impl->roof_candidate = std::move(source);
        m_impl->roof_candidate_visible_ids = std::move(candidate_visible_ids);
        m_impl->roof_candidate_targets = std::move(remapped_targets);
        m_impl->roof_candidate_serial = serial;
        m_impl->roof_transform_feedback.replace(QStringLiteral(" — checking"),QStringLiteral(" — preparing"));
        m_impl->refresh_status_label();
        m_impl->roof_preview_timer->start();
        return true;
    } catch (const std::exception& error) {
        (void)rejectRoofOpeningTransformPreview(serial,exception_text(error));
    } catch (...) {
        (void)rejectRoofOpeningTransformPreview(serial,QStringLiteral("Native skylight preparation could not start."));
    }
    return false;
}

bool NativeModelView::rejectRoofOpeningTransformPreview(std::uint64_t serial, QString message) {
    if (QThread::currentThread() != thread() || !m_impl->roof_transform_capture ||
        serial != m_impl->roof_transform_serial || !m_impl->roof_transform_proposal) return false;
    const bool released = m_impl->roof_release_pending;
    m_impl->roof_transform_proposal.reset();
    m_impl->roof_candidate_serial = 0;
    m_impl->roof_candidate.reset();
    m_impl->roof_candidate_visible_ids.reset();
    m_impl->roof_candidate_targets.clear();
    if (!m_impl->restore_roof_preview()) {
        cancelInteraction();
        return true;
    }
    if (released) {
        const QPointer<NativeModelView> guard(this);
        cancelInteraction();
        if (!guard) return true;
        m_impl->show_input_error(message);
    } else {
        m_impl->roof_transform_feedback = std::move(message);
        m_impl->refresh_status_label();
    }
    return true;
}

void NativeModelView::setRoofOpeningTransformMetricUnits(bool metric) {
    if (m_impl->roof_transform_metric_units == metric) return;
    const QPointer<NativeModelView> guard(this);
    cancelInteraction();
    if (guard) m_impl->roof_transform_metric_units = metric;
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
        const QPointer<NativeModelView> guard(this);
        m_impl->collect_prepared_geometry();
        if (guard) m_impl->collect_roof_preview();
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
    if (!m_impl->selected_roof_openings.empty() || m_impl->selected_entity_ids.size()>1 || !isReady() || !m_impl->supports_direct_translation(entity_id)) return false;
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
    const QPointer<NativeModelView> guard(this);
    m_impl->retire_roof_transform();
    if (!guard) return;
    if (m_impl->tablet_dispatch_depth && !keep_tablet_dispatch) m_impl->tablet_dispatch_retired=true;
    if (!keep_tablet_dispatch || !m_impl->touch_dispatch_depth) {
        if (m_impl->touch_dispatch_depth) m_impl->touch_dispatch_retired=true;
        m_impl->touch.reset();
    }
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
    // Retire logical ownership before native presentation restoration can
    // throw. The retained preview IDs suffice to restore its derived solids.
    m_impl->gesture = Impl::Gesture::none;
    m_impl->left_moved = false;
    m_impl->translation_entity_id.reset();
    m_impl->translation_start.reset();
    m_impl->clear_translation_preview();
    if (restore_controls) { try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); } }
    unsetCursor();
    if (m_impl->native_ready && !m_impl->view.IsNull()) m_impl->view->Redraw();
}

void NativeModelView::resetCompletedPointerInteraction(bool restore_controls) {
    // Shared release picking performs a second admission after restoring the
    // native preview. Preserve only its still-valid completing touch proof;
    // explicit cancellation continues to retire the dispatch unconditionally.
    const bool completing_touch=m_impl->touch_dispatch_depth && !m_impl->touch_dispatch_retired &&
        m_impl->touch && m_impl->touch->device && m_impl->selection_current(m_impl->touch->context);
    resetInteraction(restore_controls,completing_touch);
}

bool NativeModelView::event(QEvent* event) {
    // A context request must not retire/cancel another device's gesture merely
    // by arriving. Its handler checks idle ownership without changing it.
    if (event->type()==QEvent::ContextMenu) return QWidget::event(event);
    const QPointer<NativeModelView> input_guard(this);
    if (m_impl) retireDisconnectedTablet();
    if (!input_guard) { event->accept(); return true; }
    if (m_impl) retireDisconnectedTouch();
    if (!input_guard) { event->accept(); return true; }
    if (m_impl && (m_impl->tablet_active || m_impl->tablet_dispatch_depth ||
                   m_impl->touch || m_impl->touch_dispatch_depth) && event->type()==QEvent::Wheel) {
        event->accept();
        return true;
    }
    if (m_impl && (event->type()==QEvent::TouchBegin || event->type()==QEvent::TouchUpdate ||
                   event->type()==QEvent::TouchEnd || event->type()==QEvent::TouchCancel)) {
        if (m_impl->tablet_active || m_impl->tablet_dispatch_depth) {
            event->accept();
            return true;
        }
        touchEvent(static_cast<QTouchEvent*>(event));
        return true;
    }
    if (m_impl && (event->type()==QEvent::ScreenChangeInternal ||
                   event->type()==QEvent::DevicePixelRatioChange)) {
        m_impl->navigation_changed();
        if (m_impl->touch) cancelTouchInteraction();
        if (!input_guard) { event->accept(); return true; }
    }
    if (m_impl && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide ||
                   event->type() == QEvent::WindowBlocked || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut)) {
        if (m_impl->touch || m_impl->touch_dispatch_depth) cancelTouchInteraction();
        else cancelInteraction();
        if (!input_guard) { event->accept(); return true; }
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
        if (m_impl->touch || m_impl->touch_dispatch_depth) cancelTouchInteraction();
        else cancelInteraction();
        if (!input_guard) { event->accept(); return true; }
        m_impl->space_pan_armed=false;
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void NativeModelView::contextMenuEvent(QContextMenuEvent* event) {
    // Pointer menus are already dispatched by stationary right release. Consume
    // Qt's redundant event even after orbit so it cannot open a parent menu.
    if (event->reason()==QContextMenuEvent::Mouse) { event->accept(); return; }
    if (event->reason()!=QContextMenuEvent::Keyboard) { QWidget::contextMenuEvent(event); return; }
    event->accept();
    if (m_impl->keyboard_context_dispatch_active) return;
    const QPointer<NativeModelView> guard(this);
    const auto idle=[this] {
        return hasFocus() && isVisible() && isReady() && !m_impl->view.IsNull() && !m_impl->context.IsNull() &&
            !m_impl->window.IsNull() && !m_impl->view->Camera().IsNull() &&
            m_impl->gesture==Impl::Gesture::none && m_impl->initiating_button==Qt::NoButton &&
            !isMoveActive() && !m_impl->translation_start && m_impl->translation_preview_ids.empty() &&
            !m_impl->manipulation_transform && !m_impl->selection_capture && !m_impl->gesture_snapshot &&
            !m_impl->tablet_active && !m_impl->tablet_dispatch_depth &&
            !m_impl->touch && !m_impl->touch_dispatch_depth && !m_impl->space_pan_armed &&
            QApplication::mouseButtons()==Qt::NoButton &&
            (m_impl->manipulator.IsNull() || !m_impl->manipulator->HasActiveTransformation());
    };
    struct ContextDispatchReset {
        QPointer<NativeModelView> owner;
        ~ContextDispatchReset() {
            if (owner) owner->m_impl->keyboard_context_dispatch_active=false;
        }
    } reset_dispatch{guard};
    Impl::CommitSourceScope source_scope(this);
    m_impl->keyboard_context_dispatch_active=true;
    try {
        if (!idle()) return;
        const auto callback=onContextMenuRequested;
        const auto child_callback=onRoofOpeningContextMenuRequested;
        if (!m_impl->selected_roof_openings.empty() ? !child_callback : !callback) return;
        const auto capture=m_impl->capture_selection();
        if (!m_impl->selection_current(capture)) return;
        if (!admitSceneInput(true) || !guard || !idle() || !m_impl->selection_current(capture)) return;
        const auto visible=visibleRegion().boundingRect().intersected(rect());
        if (visible.isEmpty()) return;
        const auto anchor=mapToGlobal(m_impl->keyboard_context_anchor(capture,visible));
        if (!m_impl->selection_current(capture)) return;
        const auto primary=capture.selection.isEmpty() ? QString{} : capture.selection.back();
        m_impl->commit_snapshot=capture.source;
        if (!capture.roof_openings.empty()) child_callback(capture.roof_openings.back(),anchor);
        else callback(primary,anchor);
    } catch (const Standard_Failure& error) {
        source_scope.restore();
        if (guard) m_impl->show_input_error(QStringLiteral("3D keyboard context failed: ")+exception_text(error));
    } catch (const std::exception& error) {
        source_scope.restore();
        if (guard) m_impl->show_input_error(QStringLiteral("3D keyboard context failed: ")+exception_text(error));
    } catch (...) {
        source_scope.restore();
        if (guard) m_impl->show_input_error(QStringLiteral("3D keyboard context failed: unknown failure"));
    }
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
    const QPointer<NativeModelView> guard(this);
    if (m_impl->touch) cancelTouchInteraction();
    if (!guard) return;
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
    if (m_impl->touch_dispatch_depth && m_impl->tablet_dispatch_depth) return false;
    if (m_impl->touch_dispatch_depth &&
        (m_impl->touch_dispatch_retired || !m_impl->touch || !m_impl->touch->device)) return false;
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
    if (m_impl->tablet_active || m_impl->tablet_dispatch_depth || m_impl->touch || m_impl->touch_dispatch_depth) return true;
    if (std::chrono::steady_clock::now()<m_impl->touch_mouse_suppression_until &&
        (event->source()!=Qt::MouseEventNotSynthesized ||
         (event->pointingDevice() && (event->pointingDevice()->type()==QInputDevice::DeviceType::TouchScreen ||
                                     event->pointingDevice()->type()==QInputDevice::DeviceType::TouchPad)))) return true;
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

void NativeModelView::retireDisconnectedTouch() noexcept {
    if (m_impl->touch && !m_impl->touch->device) cancelTouchInteraction();
}

void NativeModelView::cancelTouchInteraction() noexcept {
    const QPointer<NativeModelView> guard(this);
    try { cancelInteraction(); }
    catch (...) {
        if (!guard) return;
        try { unsetCursor(); } catch (...) {}
        if (guard) { try { m_impl->show_input_error(QStringLiteral("3D touch cancellation failed.")); } catch (...) {} }
    }
}

void NativeModelView::touchEvent(QTouchEvent* event) {
    const QPointer<NativeModelView> guard(this);
    event->accept();
    if (m_impl->tablet_active || m_impl->tablet_dispatch_depth) return;
    const auto device=event->pointingDevice();
    if (m_impl->touch_dispatch_depth) {
        if (m_impl->touch_dispatch_retired) return;
        const auto dispatch_device=m_impl->touch_dispatch_device.data();
        if (device && dispatch_device && device!=dispatch_device) return;
        // Qt can deliver terminal/contact-changing packets while an admission
        // observer pumps events. Consuming them without retirement would let
        // the outer press claim a sequence that has already ended.
        bool retire=!device || !dispatch_device || event->type()==QEvent::TouchCancel ||
            event->type()==QEvent::TouchEnd || event->type()==QEvent::TouchBegin || event->points().isEmpty();
        bool primary_present=false;
        for (const auto& point : event->points()) {
            if (point.state()==QEventPoint::Released ||
                (m_impl->touch && !m_impl->touch->ids.contains(point.id())) ||
                (!m_impl->touch && point.state()==QEventPoint::Pressed)) retire=true;
            if (m_impl->touch && point.id()==m_impl->touch->primary_id && point.state()!=QEventPoint::Released)
                primary_present=true;
        }
        if (m_impl->touch && !m_impl->touch->navigation && !primary_present) retire=true;
        if (retire) {
            m_impl->touch_dispatch_retired=true;
            m_impl->touch_mouse_suppression_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            cancelTouchInteraction();
        }
        return;
    }
    if (event->type()==QEvent::TouchCancel) {
        if (m_impl->touch && (!device || m_impl->touch->device.data()==device)) cancelTouchInteraction();
        return;
    }
    if (!device || (m_impl->touch && m_impl->touch->device.data()!=device)) return;
    m_impl->touch_mouse_suppression_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    if (!m_impl->touch && event->type()!=QEvent::TouchBegin) return;
    if (!m_impl->touch && m_impl->initiating_button!=Qt::NoButton) return;
    m_impl->touch_dispatch_depth++;
    m_impl->touch_dispatch_retired=false;
    m_impl->touch_dispatch_device=device;
    struct TouchDispatchReset {
        QPointer<NativeModelView> owner;
        ~TouchDispatchReset() {
            if (!owner) return;
            auto& impl=*owner->m_impl;
            if (--impl.touch_dispatch_depth==0) {
                impl.touch_dispatch_retired=false;
                impl.touch_dispatch_device.clear();
            }
        }
    } reset_dispatch{guard};
    const auto refuse=[&](const QString& message) noexcept {
        if (!guard) return;
        cancelTouchInteraction();
        if (guard) { try { m_impl->show_input_error(message); } catch (...) {} }
    };
    try {
        if (event->pointCount()>32) throw std::invalid_argument("Too many touch contacts. Start again.");
        std::map<int, const QEventPoint*> active;
        const QEventPoint* primary=nullptr;
        for (const auto& point : event->points()) {
            const auto position=point.position(),global=point.globalPosition();
            if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
                !std::isfinite(global.x()) || !std::isfinite(global.y()))
                throw std::invalid_argument("The touch position is unavailable.");
            if (point.state()!=QEventPoint::Released && !active.emplace(point.id(),&point).second)
                throw std::invalid_argument("The touch contact identity is ambiguous.");
        }
        if (!m_impl->touch) {
            if (!isReady() || active.empty()) return;
            m_impl->complete_initial_fit();
            if (!guard || m_impl->touch_dispatch_retired) return;
            if (!m_impl->touch_dispatch_device) { cancelTouchInteraction(); return; }
            if (!isReady()) return;
            const auto first=active.begin()->first;
            m_impl->touch=Impl::TouchGesture{m_impl->touch_dispatch_device.data(),{},first,false,std::nullopt,
                {},0.0,0.0,m_impl->capture_selection()};
            m_impl->tablet_mouse_suppression_until={};
            m_impl->touch_mouse_suppression_until={};
        }
        auto capture=*m_impl->touch;
        if (!m_impl->selection_current(capture.context))
            throw std::invalid_argument("The displayed 3D source, selection or camera changed. Start touch again.");
        for (const auto& point : event->points()) {
            if (point.id()==capture.primary_id) primary=&point;
            if (point.state()==QEventPoint::Released) capture.ids.erase(point.id());
            else if (point.state()==QEventPoint::Pressed || event->type()==QEvent::TouchBegin)
                capture.ids.insert(point.id());
            else if (!capture.ids.contains(point.id()))
                throw std::invalid_argument("The touch contact sequence changed. Start again.");
        }
        if (capture.ids.size()>32 || (event->type()==QEvent::TouchEnd && !capture.ids.empty()))
            throw std::invalid_argument("The touch contact sequence is incomplete. Start again.");
        m_impl->touch->ids=capture.ids;
        m_impl->touch_mouse_suppression_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        if (!capture.navigation && (capture.ids.size()>=2 ||
            (!capture.ids.empty() && !capture.ids.contains(capture.primary_id)))) {
            // Retire and restore any single-finger object preview before the
            // second contact can move the camera. This path emits no command.
            m_impl->touch->navigation=true;
            resetInteraction(true,true);
            if (!guard || m_impl->touch_dispatch_retired || !m_impl->touch) return;
            m_impl->detach_manipulator();
            if (!m_impl->selection_current(capture.context))
                throw std::invalid_argument("The displayed 3D context changed before touch navigation.");
            capture.navigation=true;
        }
        if (capture.navigation) {
            if (capture.ids.empty()) { cancelTouchInteraction(); return; }
            auto pair=capture.pair;
            if (pair && (!capture.ids.contains((*pair)[0]) || !capture.ids.contains((*pair)[1]))) pair.reset();
            if (!pair && active.size()>=2) pair=std::array<int,2>{active.begin()->first,std::next(active.begin())->first};
            if (!pair || !active.contains((*pair)[0]) || !active.contains((*pair)[1])) {
                m_impl->touch->pair.reset();
                m_impl->touch->zoom_remainder=0.0;
                return; // A one-finger remainder never resumes object input.
            }
            const auto first=active.at((*pair)[0])->position(),second=active.at((*pair)[1])->position();
            const auto centroid=(first+second)*0.5;
            const auto distance=std::hypot(first.x()-second.x(),first.y()-second.y());
            if (!std::isfinite(distance) || distance<=0.0)
                throw std::invalid_argument("The touch navigation span is unavailable.");
            if (capture.pair==pair) {
                const auto previous=m_impl->input_point(capture.centroid),current=m_impl->input_point(centroid);
                const auto dx=static_cast<std::int64_t>(current.x)-previous.x;
                const auto dy=static_cast<std::int64_t>(previous.y)-current.y;
                if (std::abs(dx)>std::numeric_limits<int>::max() || std::abs(dy)>std::numeric_limits<int>::max())
                    throw std::invalid_argument("The touch pan is outside the supported pixel range.");
                const auto ratio=m_impl->input_scale();
                const auto movement=(distance-capture.distance)*ratio;
                if (!std::isfinite(ratio) || ratio<=0.0 || !std::isfinite(movement))
                    throw std::invalid_argument("The native touch pixel mapping is unavailable.");
                const auto accumulated=std::clamp(movement,-120.0,120.0)+capture.zoom_remainder;
                const auto zoom=static_cast<int>(std::trunc(accumulated));
                const auto end_y=static_cast<std::int64_t>(current.y)+zoom;
                if (end_y<std::numeric_limits<int>::min() || end_y>std::numeric_limits<int>::max())
                    throw std::invalid_argument("The touch zoom anchor is outside the supported pixel range.");
                if (dx || dy || zoom) {
                    m_impl->navigation_changed();
                    if (dx || dy) m_impl->view->Pan(static_cast<int>(dx),static_cast<int>(dy),1.0,true);
                    if (zoom) {
                        m_impl->view->StartZoomAtPoint(current.x,current.y);
                        m_impl->view->ZoomAtPoint(current.x,current.y,current.x,static_cast<int>(end_y));
                    }
                }
                if (!guard || m_impl->touch_dispatch_retired || !m_impl->touch) return;
                m_impl->touch->zoom_remainder=accumulated-zoom;
                // Only our successful camera change advances this capture;
                // source, selection, size and DPR remain the original proof.
                m_impl->touch->context.camera=m_impl->view->Camera()->WorldViewProjState();
                m_impl->touch->context.navigation_generation=m_impl->navigation_generation;
            } else m_impl->touch->zoom_remainder=0.0;
            m_impl->touch->pair=pair;
            m_impl->touch->centroid=centroid;
            m_impl->touch->distance=distance;
            return;
        }
        if (!primary) throw std::invalid_argument("The initiating touch contact is unavailable.");
        class TouchPointer final : public QSinglePointEvent {
        public:
            TouchPointer(QEvent::Type type,const QPointingDevice* device,const QEventPoint& point,
                         Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers modifiers)
                : QSinglePointEvent(type,device,point,button,buttons,modifiers,Qt::MouseEventNotSynthesized) {}
        } pointer(event->type()==QEvent::TouchBegin ? QEvent::MouseButtonPress :
                  primary->state()==QEventPoint::Released ? QEvent::MouseButtonRelease : QEvent::MouseMove,
                  device,*primary,event->type()==QEvent::TouchBegin || primary->state()==QEventPoint::Released ?
                  Qt::LeftButton : Qt::NoButton,primary->state()==QEventPoint::Released ? Qt::NoButton : Qt::LeftButton,
                  event->modifiers());
        pointer.setTimestamp(event->timestamp());
        if (event->type()==QEvent::TouchBegin) pointerPress(&pointer);
        else if (primary->state()==QEventPoint::Released) {
            if (!capture.ids.empty()) throw std::invalid_argument("The initiating touch ended with contacts still active.");
            if (!admitSceneInput(false) || !guard || m_impl->touch_dispatch_retired || !m_impl->touch) return;
            if (!m_impl->touch->device || !m_impl->selection_current(capture.context)) {
                cancelTouchInteraction();
                return;
            }
            pointerRelease(&pointer);
            if (guard && m_impl->touch) cancelTouchInteraction();
        } else {
            if (!admitSceneInput(false) || !guard || m_impl->touch_dispatch_retired || !m_impl->touch) return;
            if (!m_impl->touch->device || !m_impl->selection_current(capture.context)) {
                cancelTouchInteraction();
                return;
            }
            const auto camera_gesture=m_impl->gesture==Impl::Gesture::pan || m_impl->gesture==Impl::Gesture::orbit ||
                m_impl->gesture==Impl::Gesture::overlap_select || m_impl->gesture==Impl::Gesture::overlap_pan;
            pointerMove(&pointer);
            if (guard && !m_impl->touch_dispatch_retired && m_impl->touch && camera_gesture) {
                m_impl->touch->context.camera=m_impl->view->Camera()->WorldViewProjState();
                m_impl->touch->context.navigation_generation=m_impl->navigation_generation;
            }
        }
    } catch (const Standard_Failure& error) {
        refuse(QStringLiteral("3D touch input failed: ")+exception_text(error));
    } catch (const std::exception& error) {
        refuse(QStringLiteral("3D touch input failed: ")+exception_text(error));
    } catch (...) {
        refuse(QStringLiteral("3D touch input failed: unknown failure"));
    }
}

void NativeModelView::tabletEvent(QTabletEvent* event) {
    const QPointer<NativeModelView> guard(this);
    retireDisconnectedTablet();
    if (!guard) { event->accept(); return; }
    // Accept actual tablet packets, including refused/stale packets, so Qt
    // does not synthesize a second mouse authoring gesture from them.
    event->accept();
    if (m_impl->touch || m_impl->touch_dispatch_depth) return;
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
    if (m_impl->roof_release_pending) {
        cancelInteraction();
        if (!owner_guard) { event->accept(); return; }
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
            Impl::CommitSourceScope source_scope(this);
            m_impl->commit_snapshot=m_impl->gesture_snapshot;
            try {
                if (m_impl->roof_transform_capture) {
                    const auto observer=onRoofOpeningTransformStarted;
                    if (observer) observer(m_impl->roof_transform_capture->roof_openings);
                } else {
                    const auto observer=onTransformGestureStarted;
                    if (observer) observer(target);
                }
                if (!guard) return false;
                if (m_impl->touch_dispatch_depth && (m_impl->touch_dispatch_retired || !m_impl->touch ||
                    !m_impl->touch->device || !m_impl->selection_current(m_impl->touch->context))) {
                    if (!m_impl->touch_dispatch_retired) cancelTouchInteraction();
                    return false;
                }
                return !guard.isNull() && (!m_impl->tablet_dispatch_depth || !m_impl->tablet_dispatch_retired) &&
                    (!m_impl->touch_dispatch_depth || !m_impl->touch_dispatch_retired);
            } catch (const std::exception& error) {
                source_scope.restore();
                if (guard) {
                    guard->cancelInteraction();
                    guard->m_impl->show_input_error(QStringLiteral("3D edit capture failed: ") +
                        QString::fromUtf8(error.what()));
                }
            } catch (...) {
                source_scope.restore();
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
            const bool children = !m_impl->selected_roof_openings.empty();
            if (children) {
                m_impl->roof_transform_capture = m_impl->capture_selection();
                // Capture the world length magnet once; camera changes retire
                // the gesture rather than changing its quantization mid-drag.
                const double desired = 8.0 * m_impl->view->Camera()->Scale() / std::max(1,height());
                const std::vector<double> steps = m_impl->roof_transform_metric_units ?
                    std::vector<double>{0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.25,0.5,1.0} :
                    std::vector<double>{0.00635,0.0127,0.0254,0.0762,0.1524,0.3048};
                const auto step = std::lower_bound(steps.begin(),steps.end(),desired);
                m_impl->roof_transform_length_step = step == steps.end() ? steps.back() : *step;
            }
            if (!capture_transform(children ? QString() : QString::fromStdString(*m_impl->manipulator_entity_id)) ||
                !owner_guard || (children && (!m_impl->roof_transform_capture ||
                    !m_impl->selection_current(*m_impl->roof_transform_capture)))) {
                if (owner_guard) cancelInteraction();
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
            m_impl->preview_manipulation(point,event->modifiers());
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
    Impl::CommitSourceScope source_scope(this);
    const auto point = m_impl->input_point(event->position());
    // Sub-threshold release movement must not replace the press-owned hit.
    // Dragging still uses the release point for its final camera/edit endpoint.
    const auto selection_point = m_impl->input_point(m_impl->left_press);
    if ((event->position() - m_impl->left_press).manhattanLength() >= QApplication::startDragDistance())
        m_impl->left_moved = true;
    if (m_impl->gesture == Impl::Gesture::manipulate && m_impl->roof_transform_capture) {
        if (!m_impl->left_moved || !m_impl->selection_current(*m_impl->roof_transform_capture)) {
            cancelInteraction(); event->accept(); return;
        }
        m_impl->preview_manipulation(point,event->modifiers());
        if (!owner_guard) { event->accept(); return; }
        if (!m_impl->roof_transform_proposal || !m_impl->roof_transform_capture) {
            cancelInteraction(); event->accept(); return;
        }
        try {
            if (!m_impl->manipulator.IsNull()) {
                m_impl->manipulator->StopTransform(false);
                m_impl->manipulator->DeactivateCurrentMode();
            }
        } catch (...) { cancelInteraction(); event->accept(); return; }
        // Pointer ownership ends, but the exact source/camera/cohort proposal
        // survives until its manufactured presentation completes or is retired.
        m_impl->initiating_button = Qt::NoButton;
        m_impl->gesture = Impl::Gesture::none;
        m_impl->tablet_active = false;
        m_impl->tablet_button = Qt::NoButton;
        m_impl->tablet_device.clear();
        m_impl->touch.reset();
        m_impl->roof_release_pending = true;
        m_impl->roof_preview_timer->start();
        unsetCursor();
        m_impl->commit_roof_preview();
        event->accept();
        return;
    }
    if (m_impl->gesture==Impl::Gesture::overlap_select || m_impl->gesture==Impl::Gesture::overlap_pan) {
        if (m_impl->left_moved) {
            // Also covers press/release beyond threshold with no move event.
            m_impl->pan_overlap_drag(point,true);
        } else {
            try {
                const auto capture=m_impl->selection_capture;
                if (!capture || !m_impl->selection_current(*capture))
                    throw std::invalid_argument("The displayed 3D source, selection or camera changed. Start again.");
                resetCompletedPointerInteraction(false);
                if (!owner_guard) { event->accept(); return; }
                m_impl->commit_snapshot=capture->source;
                (void)m_impl->select_at(selection_point,*capture,false,false,true);
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
        resetCompletedPointerInteraction(!context_click);
        if (!owner_guard) { event->accept(); return; }
        const auto callback = onContextMenuRequested;
        const auto child_callback = onRoofOpeningContextMenuRequested;
        event->accept();
        if (context_click) {
            if (!capture || !m_impl->selection_current(*capture)) { cancelInteraction(); return; }
            if (!admitSceneInput(true)) return;
            m_impl->commit_snapshot=capture->source;
            m_impl->detach_manipulator();
            const auto target = m_impl->select_at(selection_point,*capture);
            if (owner_guard) {
                try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
            }
            if (owner_guard && target && isReady() && m_impl->published_snapshot==capture->source) {
                if (target->roof_opening) {
                    if (child_callback && std::find(m_impl->selected_roof_openings.begin(),m_impl->selected_roof_openings.end(),
                            *target->roof_opening)!=m_impl->selected_roof_openings.end() &&
                        !m_impl->roof_opening_presentation(*target->roof_opening).IsNull())
                        child_callback(*target->roof_opening,global_position);
                } else if (callback) callback(target->entity_id,global_position);
            }
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
        // Final preview failures notify observers synchronously. Revalidate
        // touch ownership after those callbacks before retaining commit data.
        if (m_impl->touch_dispatch_depth && (m_impl->touch_dispatch_retired || !m_impl->touch ||
            !m_impl->touch->device || !m_impl->selection_current(m_impl->touch->context))) {
            if (!m_impl->touch_dispatch_retired) cancelTouchInteraction();
            event->accept();
            return;
        }
        const auto manipulation_id = m_impl->manipulator_entity_id;
        const auto manipulation_transform = m_impl->manipulation_transform;
        const auto gesture_source = m_impl->gesture_snapshot;
        resetCompletedPointerInteraction(!(was_additive || was_edit || was_click));
        if (!owner_guard) { event->accept(); return; }
        if (m_impl->touch_dispatch_depth && (m_impl->touch_dispatch_retired || !m_impl->touch ||
            !m_impl->touch->device || !m_impl->selection_current(m_impl->touch->context))) {
            if (!m_impl->touch_dispatch_retired) cancelTouchInteraction();
            event->accept();
            return;
        }
        // Reset even when an observer throws; the shell can read only this
        // actual press capture, never the newest requested snapshot.

        m_impl->commit_snapshot = gesture_source;
        if (was_additive) {
            m_impl->detach_manipulator();
            if (selection_drag) m_impl->select_rectangle(selection_start,event->position(),*capture);
            else (void)m_impl->select_at(selection_point,*capture,false,true);
            if (owner_guard) {
                try { m_impl->attach_manipulator(); } catch (...) { m_impl->detach_manipulator(); }
            }
            event->accept();
            return;
        }
        if (was_edit && isReady()) {
            m_impl->detach_manipulator();
            (void)m_impl->select_at(selection_point,*capture,true);
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
            (void)m_impl->select_at(selection_point,*capture);
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
        if (m_impl->touch_dispatch_depth && (m_impl->touch_dispatch_retired || !m_impl->touch ||
            !m_impl->touch->device || !m_impl->selection_current(m_impl->touch->context))) {
            if (!m_impl->touch_dispatch_retired) cancelTouchInteraction();
            return false;
        }
        if (!accepted) {
            // A refused first-finger authoring action may still become camera
            // navigation. Never restore a capture cancelled by the observer,
            // and never keep a changed source, selection or camera proof.
            if (starting && m_impl->touch_dispatch_depth && !m_impl->touch_dispatch_retired &&
                m_impl->touch && m_impl->touch->device && m_impl->selection_current(m_impl->touch->context)) {
                m_impl->touch->navigation=true;
                resetInteraction(true,true);
                if (guard && m_impl->touch && !m_impl->touch_dispatch_retired) m_impl->detach_manipulator();
            } else cancelInteraction();
        }
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
