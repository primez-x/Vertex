#include "sketch/visualization/native_model_view.hpp"

#include "sketch/architecture.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/terrain_surface.hpp"
#include <QColor>

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Shape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Handle.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
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

#include <algorithm>
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

using Json = nlohmann::json;

void append_entity_content(std::string& result, const Entity& entity) {
    result.append(entity.id);
    result.push_back('\0');
    result.append(entity.type);
    result.push_back('\0');
    result.append(entity.required ? "required" : "optional");
    result.push_back('\0');
    result.append(entity.properties.dump());
    result.push_back('\0');
    result.append(entity.extensions.dump());
    result.push_back('\0');
}

std::string entity_content(const Entity& entity,
                           const std::vector<const Entity*>& hosted_openings = {}) {
    std::string canonical;
    append_entity_content(canonical, entity);
    for (const auto* opening : hosted_openings) {
        if (opening != nullptr) {
            append_entity_content(canonical, *opening);
        }
    }
    return canonical;
}

void append_unique(std::vector<std::string>& messages, std::string message) {
    if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
        messages.push_back(std::move(message));
    }
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

bool is_ignored_hierarchy_type(std::string_view type) {
    static constexpr std::string_view ignored[] = {
        "property",          "building",        "floor",       "layer",      "label",
        "sheet",             "view",            "constraint",  "annotation", "dimension",
        "annotation_state",  "sheet_view_model", "boundary", "measurement_boundary",
        "reference_asset",   "assembly_model",    "model_phases", "room_relationships",
        "vertical_levels",   "room_boundary",     "terrain_surface"};
    return std::find(std::begin(ignored), std::end(ignored), type) != std::end(ignored);
}

bool is_pending_geometry_type(std::string_view type) {
    // Architectural rooms now have a native semantic volume when their
    // explicit boundary, height, and elevation fields are present.  Keep this
    // helper for future bounded geometry types without treating rooms as a
    // permanent placeholder category.
    (void)type;
    return false;
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
        std::optional<std::string> material_color;
    };

    NativeModelView* owner{};
    QLabel* status_label{};
    std::optional<DocumentSnapshot> snapshot;
    std::optional<NativeModelView::VisibleEntityIds> visible_ids;
    QString native_error;
    QString geometry_status;
    QString operation_error;
    bool native_attempted{};
    bool native_ready{};
    bool has_fit{};

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

    explicit Impl(NativeModelView* widget) : owner(widget) {}

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

    void show_status(const QString& text) {
        geometry_status = text;
        operation_error.clear();
        refresh_status_label();
        if (!text.isEmpty() && owner->onError) {
            owner->onError(text);
        }
    }

    void show_native_error(const QString& text) {
        native_error = text;
        refresh_status_label();
        if (owner->onError) {
            owner->onError(text);
        }
    }

    void show_operation_error(const QString& text) {
        operation_error = text;
        refresh_status_label();
        if (owner->onError) {
            owner->onError(text);
        }
    }

    void refresh_status_label() {
        const auto text = !native_error.isEmpty()
                              ? native_error
                              : (!geometry_status.isEmpty() ? geometry_status : operation_error);
        status_label->setText(text);
        status_label->setVisible(!text.isEmpty());
        status_label->raise();
        status_label->setGeometry(owner->rect().adjusted(12, 12, -12, -12));
    }

    void remove_solid(const std::string& id) {
        const auto found = solids.find(id);
        if (found == solids.end()) {
            return;
        }
        if (native_ready && !found->second.presentation.IsNull()) {
            context->Remove(found->second.presentation, false);
        }
        solids.erase(found);
    }

    void clear_solids() {
        if (native_ready && !context.IsNull()) {
            for (const auto& [id, solid] : solids) {
                (void)id;
                if (!solid.presentation.IsNull()) {
                    context->Remove(solid.presentation, false);
                }
            }
        }
        solids.clear();
        has_fit = false;
    }

    void rebuild_snapshot() {
        if (!native_ready || !snapshot.has_value()) {
            return;
        }

        std::vector<std::string> errors;
        std::vector<std::string> pending;
        const auto& entities = snapshot->entities();
        std::map<std::pair<std::string, std::string>, std::string> material_colors;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "assembly_model") continue;
            try {
                const auto catalog = AssemblyModel::from_json(entity.properties.at("model"));
                for (const auto& material : catalog.materials())
                    if (material.color_srgb) material_colors[{id, material.id}] = *material.color_srgb;
            } catch (const std::exception& error) {
                append_unique(errors, "material catalog '" + id + "': " + error.what());
            }
        }
        std::set<std::string, std::less<>> wall_ids;
        for (const auto& [id, entity] : entities) {
            if (entity.type == "wall") {
                wall_ids.insert(id);
            }
        }

        std::map<std::string, std::vector<const Entity*>, std::less<>> openings_by_wall;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "opening") {
                continue;
            }
            std::string wall_id;
            std::string relation_error;
            if (!read_document_wall_id(entity, wall_id, relation_error)) {
                append_unique(pending, "opening '" + id + "': " + relation_error);
            } else if (!wall_ids.contains(wall_id)) {
                append_unique(pending, "opening '" + id + "' references missing wall '" + wall_id + "'");
            } else {
                openings_by_wall[wall_id].push_back(&entity);
            }
        }

        std::set<std::string, std::less<>> supported_ids;
        const bool had_solids = !solids.empty();
        bool changed = false;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "wall" && entity.type != "slab" && entity.type != "room" &&
                entity.type != "terrain_surface" &&
                !can_recognize_building_entity_type(entity.type)) {
                if (entity.type == "opening") {
                    continue;
                }
                if (is_pending_geometry_type(entity.type)) {
                    append_unique(pending, "entity '" + id + "' of type '" + entity.type +
                                             "' has no native solid representation yet");
                } else if (!is_ignored_hierarchy_type(entity.type)) {
                    append_unique(pending, "entity '" + id + "' of unsupported type '" + entity.type +
                                             "' is pending native geometry");
                }
                continue;
            }

            Entity geometry_entity;
            try {
                geometry_entity = resolve_vertical_placement(*snapshot, entity);
            } catch (const std::exception& error) {
                append_unique(errors, entity.type + " '" + id + "': " + error.what());
                remove_solid(id);
                changed = true;
                continue;
            }

            supported_ids.insert(id);
            const auto hosted = entity.type == "wall"
                                    ? openings_by_wall[id]
                                    : std::vector<const Entity*>{};
            auto content = entity_content(geometry_entity, hosted);
            std::optional<std::string> material_color;
            if (geometry_entity.properties.contains("material_assignment")) {
                const auto& assignment = geometry_entity.properties.at("material_assignment");
                const auto found = material_colors.find({assignment.at("catalog_id").get<std::string>(),
                    assignment.at("material_id").get<std::string>()});
                if (found != material_colors.end()) material_color = found->second;
            }
            auto presentation_color = entity.type == "wall"
                ? Quantity_Color(0.84, 0.66, 0.32, Quantity_TOC_RGB)
                : entity.type == "terrain_surface"
                    ? Quantity_Color(0.47, 0.64, 0.44, Quantity_TOC_RGB)
                    : Quantity_Color(0.46, 0.70, 0.86, Quantity_TOC_RGB);
            if (material_color) {
                const QColor color(QString::fromStdString(*material_color));
                presentation_color = Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB);
            }
            const auto cached = solids.find(id);
            if (cached != solids.end() && cached->second.content == content) {
                if (cached->second.material_color != material_color) {
                    cached->second.presentation->SetColor(presentation_color);
                    context->Redisplay(cached->second.presentation, false);
                    cached->second.material_color = material_color;
                    changed = true;
                }
                const bool visible = !visible_ids || visible_ids->contains(id);
                if (visible != static_cast<bool>(context->IsDisplayed(cached->second.presentation))) {
                    if (visible) context->Display(cached->second.presentation, false);
                    else context->Erase(cached->second.presentation, false);
                    changed = true;
                }
                continue;
            }

            std::string parse_error;
            TopoDS_Shape shape;
            try {
                if (geometry_entity.type == "wall") {
                    Wall wall;
                    if (!read_document_wall(geometry_entity, hosted, wall, parse_error)) {
                        append_unique(errors, "wall '" + id + "': " + parse_error);
                        remove_solid(id);
                        changed = true;
                        continue;
                    }
                    shape = make_wall(wall);
                } else if (geometry_entity.type == "slab") {
                    Slab slab;
                    if (!read_document_slab(geometry_entity, slab, parse_error)) {
                        append_unique(errors, "slab '" + id + "': " + parse_error);
                        remove_solid(id);
                        changed = true;
                        continue;
                    }
                    shape = make_slab(slab);
                } else if (geometry_entity.type == "terrain_surface") {
                    shape = make_terrain_surface(
                        TerrainSurface::from_json(geometry_entity.properties.at("model")));
                } else if (geometry_entity.type == "room") {
                    RoomVolume room;
                    if (!read_document_room(geometry_entity, room, parse_error)) {
                        append_unique(errors, "room '" + id + "': " + parse_error);
                        remove_solid(id);
                        changed = true;
                        continue;
                    }
                    shape = make_room_volume(room);
                } else {
                    shape = make_building_shape(decode_building_entity(geometry_entity));
                }
                if (shape.IsNull()) {
                    append_unique(errors, entity.type + " '" + id + "' produced a null solid");
                    remove_solid(id);
                    changed = true;
                    continue;
                }

                auto presentation = occ::handle<AIS_Shape>(new AIS_Shape(shape));
                presentation->SetColor(presentation_color);
                presentation->SetDisplayMode(AIS_Shaded);

                remove_solid(id);
                if (!visible_ids || visible_ids->contains(id)) context->Display(presentation, false);
                solids.emplace(id, CachedSolid{std::move(content), std::move(shape), presentation, material_color});
                changed = true;
            } catch (const std::exception& error) {
                append_unique(errors, entity.type + " '" + id + "': " + error.what());
                remove_solid(id);
                changed = true;
            } catch (...) {
                append_unique(errors, entity.type + " '" + id + "': unknown OCCT failure");
                remove_solid(id);
                changed = true;
            }
        }

        const auto make_assembly_host_shape = [&](const std::string& host_id) -> TopoDS_Shape {
            const auto host = entities.find(host_id);
            if (host == entities.end()) {
                throw std::invalid_argument("assembly host is missing");
            }
            const auto& source = host->second;
            const auto geometry_entity = resolve_vertical_placement(*snapshot, source);
            if (can_recognize_building_entity_type(geometry_entity.type)) {
                return make_building_shape(decode_building_entity(geometry_entity));
            }
            if (geometry_entity.type == "wall") {
                Wall wall;
                std::string error;
                if (!read_document_wall(geometry_entity, openings_by_wall[host_id], wall, error)) {
                    throw std::invalid_argument(error);
                }
                return make_wall(wall);
            }
            if (geometry_entity.type == "slab") {
                Slab slab;
                std::string error;
                if (!read_document_slab(geometry_entity, slab, error)) {
                    throw std::invalid_argument(error);
                }
                return make_slab(slab);
            }
            if (geometry_entity.type == "room") {
                RoomVolume room;
                std::string error;
                if (!read_document_room(geometry_entity, room, error)) {
                    throw std::invalid_argument(error);
                }
                return make_room_volume(room);
            }
            throw std::invalid_argument("assembly host has no native architectural solid");
        };
        const auto transform_assembly_shape = [](const TopoDS_Shape& source,
                                                 const AssemblyPlacement& placement) {
            if (source.IsNull()) throw std::invalid_argument("assembly host solid is empty");
            if (!std::isfinite(placement.scale) || placement.scale <= 0.0 ||
                !std::isfinite(placement.rotation_radians) ||
                !std::isfinite(placement.translation_m.x) ||
                !std::isfinite(placement.translation_m.y)) {
                throw std::invalid_argument("assembly placement transform is invalid");
            }
            gp_Trsf scale;
            scale.SetScale(gp_Pnt(0.0, 0.0, 0.0), placement.scale);
            BRepBuilderAPI_Transform scaled(source, scale, true);
            if (!scaled.IsDone() || scaled.Shape().IsNull()) {
                throw std::invalid_argument("assembly scale transform failed");
            }
            gp_Trsf rotate;
            rotate.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
                               placement.rotation_radians);
            BRepBuilderAPI_Transform rotated(scaled.Shape(), rotate, true);
            if (!rotated.IsDone() || rotated.Shape().IsNull()) {
                throw std::invalid_argument("assembly rotation transform failed");
            }
            gp_Trsf translate;
            translate.SetTranslation(gp_Vec(placement.translation_m.x,
                                             placement.translation_m.y, 0.0));
            BRepBuilderAPI_Transform translated(rotated.Shape(), translate, true);
            if (!translated.IsDone() || translated.Shape().IsNull()) {
                throw std::invalid_argument("assembly translation transform failed");
            }
            return translated.Shape();
        };

        for (const auto& [catalog_id, catalog_entity] : entities) {
            if (catalog_entity.type != "assembly_model" ||
                !catalog_entity.properties.contains("model")) continue;
            try {
                const auto catalog = AssemblyModel::from_json(catalog_entity.properties.at("model"));
                for (const auto& instance : catalog.instances()) {
                    if (!instance.placement) continue;
                    const auto child_id = catalog_id + ":instance:" + instance.id;
                    const auto host = entities.find(instance.placement->host_entity_id);
                    if (host == entities.end()) {
                        append_unique(errors, "assembly instance '" + child_id +
                                             "' references missing host '" +
                                             instance.placement->host_entity_id + "'");
                        continue;
                    }
                    const auto geometry_entity = resolve_vertical_placement(*snapshot, host->second);
                    std::string content;
                    content.reserve(catalog_entity.properties.dump().size() +
                                    geometry_entity.properties.dump().size() + child_id.size() + 32);
                    content.append(child_id);
                    content.push_back('\0');
                    append_entity_content(content, catalog_entity);
                    append_entity_content(content, geometry_entity);
                    if (geometry_entity.type == "wall") {
                        for (const auto* opening : openings_by_wall[host->first]) {
                            if (opening != nullptr) append_entity_content(content, *opening);
                        }
                    }
                    std::optional<std::string> material_color;
                    const auto resolved = catalog.resolve(instance.id);
                    for (const auto& [slot, material_id] : resolved.materials) {
                        (void)slot;
                        const auto material = material_colors.find({catalog_id, material_id});
                        if (material != material_colors.end()) {
                            material_color = material->second;
                            break;
                        }
                    }
                    const auto cached = solids.find(child_id);
                    const auto presentation_color = [&] {
                        if (material_color) {
                            const QColor color(QString::fromStdString(*material_color));
                            if (color.isValid()) {
                                return Quantity_Color(color.redF(), color.greenF(), color.blueF(),
                                                       Quantity_TOC_sRGB);
                            }
                        }
                        return Quantity_Color(0.63, 0.48, 0.78, Quantity_TOC_RGB);
                    }();
                    if (cached != solids.end() && cached->second.content == content) {
                        if (cached->second.material_color != material_color) {
                            cached->second.presentation->SetColor(presentation_color);
                            context->Redisplay(cached->second.presentation, false);
                            cached->second.material_color = material_color;
                            changed = true;
                        }
                        const bool visible = !visible_ids || visible_ids->contains(child_id);
                        if (visible != static_cast<bool>(context->IsDisplayed(cached->second.presentation))) {
                            if (visible) context->Display(cached->second.presentation, false);
                            else context->Erase(cached->second.presentation, false);
                            changed = true;
                        }
                        supported_ids.insert(child_id);
                        continue;
                    }

                    TopoDS_Shape shape = transform_assembly_shape(
                        make_assembly_host_shape(instance.placement->host_entity_id),
                        *instance.placement);
                    if (shape.IsNull()) {
                        append_unique(errors, "assembly instance '" + child_id + "' produced a null solid");
                        remove_solid(child_id);
                        changed = true;
                        continue;
                    }
                    auto presentation = occ::handle<AIS_Shape>(new AIS_Shape(shape));
                    presentation->SetColor(presentation_color);
                    presentation->SetDisplayMode(AIS_Shaded);
                    remove_solid(child_id);
                    if (!visible_ids || visible_ids->contains(child_id)) context->Display(presentation, false);
                    solids.emplace(child_id, CachedSolid{std::move(content), std::move(shape),
                                                         presentation, material_color});
                    supported_ids.insert(child_id);
                    changed = true;
                }
            } catch (const std::exception& error) {
                append_unique(errors, "assembly catalog '" + catalog_id + "': " + error.what());
            } catch (...) {
                append_unique(errors, "assembly catalog '" + catalog_id + "': unknown OCCT failure");
            }
        }

        for (auto it = solids.begin(); it != solids.end();) {
            if (!supported_ids.contains(it->first)) {
                if (native_ready && !it->second.presentation.IsNull()) {
                    context->Remove(it->second.presentation, false);
                }
                it = solids.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        const bool has_visible_solids = std::any_of(solids.begin(), solids.end(), [this](const auto& entry) {
            return context->IsDisplayed(entry.second.presentation);
        });
        if (has_visible_solids && (!has_fit || (!had_solids && changed))) {
            fit_all();
        } else if (solids.empty()) {
            has_fit = false;
        }
        if (changed) {
            viewer->Redraw();
        }

        std::vector<std::string> status_messages;
        status_messages.reserve(errors.size() + pending.size());
        for (auto& message : errors) {
            status_messages.push_back(std::move(message));
        }
        for (auto& message : pending) {
            status_messages.push_back(std::move(message));
        }
        if (status_messages.empty()) {
            show_status(QString());
        } else if (!errors.empty()) {
            show_status(status_text("3D geometry is incomplete:", status_messages));
        } else {
            show_status(status_text("3D geometry pending:", status_messages));
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
                rebuild_snapshot();
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

    void select_at(const NativeInputPoint point) {
        if (!native_ready || context.IsNull() || view.IsNull()) {
            return;
        }
        const auto x = point.x;
        const auto y = point.y;
        context->MoveTo(x, y, view, false);
        context->ClearSelected(false);
        context->SelectDetected(AIS_SelectionScheme_Replace);
        const auto selected = context->FirstSelectedObject();
        QString selected_id;
        if (!selected.IsNull()) {
            for (const auto& [id, solid] : solids) {
                if (solid.presentation == selected) {
                    selected_id = QString::fromStdString(id);
                    break;
                }
            }
        }
        if (owner->onEntitySelected) {
            owner->onEntitySelected(selected_id);
        }
        viewer->Redraw();
    }

    bool export_view_image(const QString& path) {
        if (!native_ready || view.IsNull()) {
            show_operation_error(QStringLiteral(
                "Native OCCT 3D view is not ready; show the viewport before exporting an image"));
            return false;
        }
        if (!native_error.isEmpty() || !geometry_status.isEmpty()) {
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
    m_impl->visible_ids = std::move(visible_ids);
    m_impl->snapshot = snapshot;
    if (isVisible()) {
        m_impl->initialize_native_view();
    }
    if (m_impl->native_ready) {
        // A previously initialized hidden viewport may still be exported.
        // Keep its derived geometry synchronized with the current snapshot.
        m_impl->rebuild_snapshot();
    }
}

void NativeModelView::fitAll() {
    if (!m_impl->native_ready || m_impl->view.IsNull()) {
        return;
    }
    m_impl->fit_all();
}

bool NativeModelView::exportViewImage(const QString& path) {
    return m_impl->export_view_image(path);
}

bool NativeModelView::isReady() const noexcept {
    return m_impl->native_ready && m_impl->native_error.isEmpty() &&
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

void NativeModelView::setEntitySelectedCallback(std::function<void(QString)> callback) {
    onEntitySelected = std::move(callback);
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
    if (!m_impl->status_label->isHidden()) {
        m_impl->status_label->setGeometry(rect().adjusted(12, 12, -12, -12));
    }
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
        m_impl->left_pressed = true;
        m_impl->left_moved = false;
        m_impl->left_press = logical_point;
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
        m_impl->left_pressed = false;
        m_impl->left_moved = false;
        if (was_click) {
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
