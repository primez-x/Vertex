#include "sketch/visualization/native_model_view.hpp"
#include "sketch/visualization/native_geometry_preparation.hpp"

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Handle.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <chrono>
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

}  // namespace

class NativeModelView::Impl {
public:
    struct CachedSolid {
        // Exact equality avoids reusing stale geometry after a hash collision.
        std::string content;
        TopoDS_Shape shape;
        occ::handle<AIS_Shape> presentation;
        Quantity_Color color;
    };

    NativeModelView* owner{};
    QLabel* status_label{};
    std::optional<DocumentSnapshot> snapshot;
    std::optional<NativeModelView::VisibleEntityIds> visible_ids;
    NativeGeometryRegenerator regenerator;
    QTimer* preparation_timer{};
    std::optional<PreparedNativeGeometry> prepared_geometry;
    std::optional<NativeModelView::PublicationMetrics> publication_metrics;
    bool geometry_prepared{};
    QString native_error;
    QString geometry_status;
    QString operation_error;
    bool native_attempted{};
    bool native_ready{};
    bool has_fit{};
    bool fit_requested{};

    occ::handle<Aspect_DisplayConnection> display_connection;
    occ::handle<OpenGl_GraphicDriver> graphic_driver;
    occ::handle<V3d_Viewer> viewer;
    occ::handle<V3d_View> view;
    occ::handle<AIS_InteractiveContext> context;
    occ::handle<WNT_Window> window;
    std::map<std::string, CachedSolid, std::less<>> solids;

    Qt::MouseButton navigation_button = Qt::NoButton;
    QPoint navigation_start;
    QPointF left_press;
    bool left_pressed{};
    bool left_moved{};
    bool left_translate{};
    std::optional<std::string> translation_entity_id;
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

    void fit_all() {
        if (!native_ready || view.IsNull() || window.IsNull()) {
            return;
        }

        // WNT_Window reports the physical client size. Refresh both the
        // OpenGL viewport and the camera aspect immediately before fitting so
        // a late QWidget/DPI resize cannot leave FitAll using an old aspect.
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
    }

    void notify_error(const QString& text) noexcept {
        // Observers do not own preparation or publication state. In particular,
        // a throwing "Preparing" observer must not prevent the timer starting,
        // and a throwing failure observer must not replace the real diagnostic.
        try {
            // Retain the callable while dispatching: the observer may replace
            // its own registration during the callback.
            const auto callback = owner->onError;
            if (callback) callback(text);
        } catch (...) {
        }
    }

    void show_status(const QString& text) {
        geometry_status = text;
        operation_error.clear();
        refresh_status_label();
        if (!text.isEmpty()) notify_error(text);
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

    void refresh_status_label() {
        const auto text = !native_error.isEmpty()
                              ? native_error
                              : (!geometry_status.isEmpty() ? geometry_status : operation_error);
        status_label->setText(text);
        status_label->setVisible(!text.isEmpty());
        status_label->raise();
        auto bounds = owner->rect().adjusted(12, 12, -12, -12);
        if (regenerator.is_pending() && native_error.isEmpty()) {
            // Keep the previous valid scene visible while preparing its
            // replacement; a progress banner must not cover the viewport.
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
        show_status(QStringLiteral("Preparing 3D geometry…"));
        preparation_timer->start();
    }

    void collect_prepared_geometry() {
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
                if (cached != solids.end() && cached->second.content == solid.content) {
                    // Retain the live topology as well as the AIS handle. The
                    // worker's fresh triangulation must never replace or mutate
                    // a shape already owned by an unchanged presentation.
                    auto retained = cached->second;
                    retained.color = solid.color;
                    replacement.emplace(id, std::move(retained));
                    ++metrics.reused;
                    continue;
                }
                auto presentation = occ::handle<AIS_Shape>(new AIS_Shape(solid.shape));
                presentation->SetColor(solid.color);
                presentation->SetDisplayMode(AIS_Shaded);
                replacement.emplace(id, CachedSolid{std::move(solid.content), std::move(solid.shape),
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
            const bool had_solids = !solids.empty();
            const bool previously_fit = has_fit;
            // A pending edit cannot move a stale displayed object in the new
            // snapshot. Reset any preview before updating the scene.
            clear_translation_preview();
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
                const bool has_visible_solids = std::any_of(prepared->solids.begin(), prepared->solids.end(),
                    [](const auto& entry) { return entry.second.visible; });
                if (has_visible_solids && (fit_requested || !has_fit || !had_solids)) fit_all();
                else if (replacement.empty()) has_fit = false;
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
                try { viewer->Redraw(); } catch (...) {}
                throw;
            }
            solids.swap(replacement);
            fit_requested = false;
            prepared_geometry.reset();
            metrics.elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - publication_started).count();
            publication_metrics = metrics;
            show_status(QString());
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
            view->SetProj(V3d_XposYnegZpos, false);
            view->MustBeResized();
            native_ready = true;
            native_error.clear();
            operation_error.clear();
            if (snapshot.has_value()) {
                collect_prepared_geometry();
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
            if (solid.presentation == selected) {
                return QString::fromStdString(id);
            }
        }
        return {};
    }

    bool supports_direct_translation(const QString& id) const {
        if (!snapshot.has_value() || id.isEmpty()) {
            return false;
        }
        const auto found = snapshot->entities().find(id.toStdString());
        return found != snapshot->entities().end() &&
               can_transform_architectural_entity_type(found->second.type);
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
        if (translation_entity_id.has_value()) {
            const auto found = solids.find(*translation_entity_id);
            if (found != solids.end() && !found->second.presentation.IsNull()) {
                found->second.presentation->ResetTransformation();
                if (native_ready && !context.IsNull()) {
                    context->Redisplay(found->second.presentation, false);
                }
            }
        }
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
        found->second.presentation->SetLocalTransformation(transform);
        if (native_ready && !context.IsNull()) {
            context->Redisplay(found->second.presentation, false);
        }
        if (native_ready && !viewer.IsNull()) {
            viewer->RedrawImmediate();
        }
    }

    QString select_at(const NativeInputPoint point) {
        if (!native_ready || !geometry_status.isEmpty() || context.IsNull() || view.IsNull()) {
            return {};
        }
        const auto x = point.x;
        const auto y = point.y;
        context->MoveTo(x, y, view, false);
        context->ClearSelected(false);
        context->SelectDetected(AIS_SelectionScheme_Replace);
        const auto selected = context->FirstSelectedObject();
        const auto selected_id = entity_id_for_presentation(selected);
        if (owner->onEntitySelected) {
            owner->onEntitySelected(selected_id);
        }
        viewer->Redraw();
        return selected_id;
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
            show_operation_error(QStringLiteral("3D view image export requires a destination path"));
            return false;
        }
        const QByteArray encoded_path = path.toUtf8();
        try {
            if (!view->Dump(encoded_path.constData(), Graphic3d_BT_RGB)) {
                show_operation_error(QStringLiteral(
                    "OCCT could not export the 3D framebuffer (check the path and image codec)"));
                return false;
            }
        } catch (const Standard_Failure& error) {
            show_operation_error(QStringLiteral("OCCT 3D framebuffer export failed: ") +
                                 exception_text(error));
            return false;
        } catch (const std::exception& error) {
            show_operation_error(QStringLiteral("3D framebuffer export failed: ") +
                                 exception_text(error));
            return false;
        } catch (...) {
            show_operation_error(QStringLiteral("3D framebuffer export failed: unknown failure"));
            return false;
        }
        operation_error.clear();
        refresh_status_label();
        return true;
    }
};

NativeModelView::NativeModelView(QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(this)) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(480, 360);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

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
        same_snapshot_content(*m_impl->snapshot, snapshot)) {
        return;
    }
    m_impl->clear_translation_preview();
    m_impl->left_pressed = false;
    m_impl->translation_entity_id.reset();
    m_impl->translation_start.reset();
    m_impl->visible_ids = std::move(visible_ids);
    m_impl->snapshot = snapshot;
    m_impl->rebuild_snapshot();
    if (isVisible()) {
        m_impl->initialize_native_view();
    }
}

void NativeModelView::fitAll() {
    if (isGeometryPending()) {
        m_impl->fit_requested = true;
        return;
    }
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        return;
    }
    m_impl->fit_all();
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
    return m_impl->operation_error;
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

void NativeModelView::setEntitySelectedCallback(std::function<void(QString)> callback) {
    onEntitySelected = std::move(callback);
}

void NativeModelView::setEntityTranslationRequestedCallback(
    std::function<void(QString, double, double, double)> callback) {
    onEntityTranslationRequested = std::move(callback);
}

void NativeModelView::setErrorCallback(std::function<void(QString)> callback) {
    onError = std::move(callback);
}

void NativeModelView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->initialize_native_view();
    m_impl->refresh_status_label();
}

void NativeModelView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->refresh_status_label();
    if (m_impl->native_ready && !m_impl->view.IsNull()) {
        m_impl->view->MustBeResized();
        m_impl->view->Redraw();
    }
}

void NativeModelView::paintEvent(QPaintEvent* event) {
    (void)event;
    if (m_impl->native_ready && !m_impl->view.IsNull()) {
        m_impl->view->Redraw();
    }
}

void NativeModelView::mousePressEvent(QMouseEvent* event) {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        event->ignore();
        return;
    }
    const auto logical_point = event->position();
    const auto point = m_impl->input_point(logical_point);
    setFocus();
    if (event->button() == Qt::RightButton) {
        m_impl->navigation_button = Qt::RightButton;
        m_impl->navigation_start = QPoint(point.x, point.y);
        m_impl->view->StartRotation(point.x, point.y, 0.4);
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        m_impl->navigation_button = Qt::MiddleButton;
        m_impl->navigation_start = QPoint(point.x, point.y);
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
        m_impl->left_pressed = true;
        m_impl->left_moved = false;
        m_impl->left_translate = event->modifiers().testFlag(Qt::ControlModifier);
        m_impl->translation_entity_id.reset();
        m_impl->translation_start.reset();
        m_impl->left_press = logical_point;
        if (m_impl->left_translate) {
            // Select immediately so the drag has a stable semantic target and
            // the inspector follows the object before the first preview.
            const auto selected_id = m_impl->select_at(point);
            if (m_impl->supports_direct_translation(selected_id)) {
                if (const auto start = m_impl->world_point(point)) {
                    m_impl->translation_entity_id = selected_id.toStdString();
                    m_impl->translation_start = *start;
                }
            }
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void NativeModelView::mouseMoveEvent(QMouseEvent* event) {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        event->ignore();
        return;
    }
    const auto logical_point = event->position();
    const auto point = m_impl->input_point(logical_point);
    if (m_impl->navigation_button == Qt::RightButton) {
        m_impl->view->Rotation(point.x, point.y);
        event->accept();
        return;
    }
    if (m_impl->navigation_button == Qt::MiddleButton) {
        const auto delta = QPoint(point.x, point.y) - m_impl->navigation_start;
        // V3d::Pan accepts view-plane displacement (positive y is up),
        // unlike picking/rotation/zoom mouse positions measured from the top.
        m_impl->view->Pan(delta.x(), -delta.y(), 1.0, false);
        event->accept();
        return;
    }
    if (m_impl->left_pressed) {
        const auto delta = logical_point - m_impl->left_press;
        if (delta.manhattanLength() >= QApplication::startDragDistance()) {
            m_impl->left_moved = true;
        }
        if (m_impl->left_translate && m_impl->left_moved &&
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
    QWidget::mouseMoveEvent(event);
}

void NativeModelView::mouseReleaseEvent(QMouseEvent* event) {
    const auto point = m_impl->input_point(event->position());
    if (event->button() == m_impl->navigation_button) {
        m_impl->navigation_button = Qt::NoButton;
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_impl->left_pressed) {
        const auto was_click = !m_impl->left_moved;
        const auto was_translation = m_impl->left_translate && !was_click &&
                                     m_impl->translation_entity_id.has_value() &&
                                     m_impl->translation_start.has_value();
        std::optional<NativeModelView::Impl::WorldPoint> end_world;
        if (was_translation) {
            end_world = m_impl->world_point(point);
        }
        const auto translation_id = m_impl->translation_entity_id;
        const auto translation_start = m_impl->translation_start;
        m_impl->clear_translation_preview();
        m_impl->left_pressed = false;
        m_impl->left_moved = false;
        m_impl->left_translate = false;
        m_impl->translation_entity_id.reset();
        m_impl->translation_start.reset();
        if (was_translation && end_world.has_value() && translation_id.has_value() &&
            translation_start.has_value()) {
            const auto dx = end_world->x - translation_start->x;
            const auto dy = end_world->y - translation_start->y;
            const auto dz = end_world->z - translation_start->z;
            constexpr double epsilon = 1.0e-9;
            if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                (std::abs(dx) > epsilon || std::abs(dy) > epsilon || std::abs(dz) > epsilon) &&
                onEntityTranslationRequested) {
                onEntityTranslationRequested(QString::fromStdString(*translation_id), dx, dy, dz);
            }
        }
        if (was_click) {
            // Ctrl+click already selected the target on press; selecting again
            // keeps ordinary click semantics for non-architectural solids.
            m_impl->select_at(point);
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void NativeModelView::wheelEvent(QWheelEvent* event) {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        event->ignore();
        return;
    }
    int delta = event->angleDelta().y();
    if (delta == 0) {
        delta = event->pixelDelta().y() * 8;
    }
    if (delta != 0) {
        const auto point = m_impl->input_point(event->position());
        const auto movement = std::clamp(delta / 8, -120, 120);
        const auto native_movement = qRound(static_cast<qreal>(movement) * m_impl->input_scale());
        m_impl->view->StartZoomAtPoint(point.x, point.y);
        m_impl->view->ZoomAtPoint(point.x, point.y, point.x, point.y + native_movement);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

QPaintEngine* NativeModelView::paintEngine() const {
    return nullptr;
}

}  // namespace sketch::visualization
