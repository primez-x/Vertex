#include "plan_canvas.hpp"
#include "sketch_content_bounds.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/desktop/symbol_svg_palette.hpp"
#include "sketch/desktop/svg_admission.hpp"
#include "sketch/desktop/area_class_palette.hpp"

#include <QApplication>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QSvgRenderer>
#include <QPainterPathStroker>
#include <QSet>
#include <QPaintEvent>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QTransform>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>

namespace sketch::desktop {
namespace {

// Opt-in diagnostic only; acceptance metrics retain their existing boundaries.
void capture_diagnostic_stage(const char* stage) {
    if (!qEnvironmentVariableIsSet("VERTEX_CAPTURE_STAGES")) return;
    static const auto started = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "capture stage: %s [%.3f ms since first TU marker]\n", stage, elapsed);
    std::fflush(stderr);
}


// Site surveys can span kilometres. Fit and navigation must not crop them
// at a building-sized zoom floor; the adaptive grid already scales with zoom.
constexpr double minimum_scale = 0.0001;
constexpr double maximum_scale = 4000.0;
constexpr double output_minimum_scale = minimum_scale;
constexpr double pi = std::numbers::pi;

constexpr auto symbol_drag_mime_type = "application/x-vertex-symbol";
struct SymbolDragPayload {
    QString id;
    double scale{};
};

std::optional<SymbolDragPayload> decode_symbol_drag(const QMimeData* mime) {
    if (!mime || !mime->hasFormat(symbol_drag_mime_type)) return std::nullopt;
    const auto payload = mime->data(symbol_drag_mime_type);
    if (payload.isEmpty() || payload.size() > 4096) return std::nullopt;
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return std::nullopt;
    const auto object = document.object();
    const auto id_value = object.value(QStringLiteral("id"));
    const auto scale_value = object.value(QStringLiteral("scale"));
    if (!id_value.isString() || !scale_value.isDouble()) return std::nullopt;
    const auto id = id_value.toString();
    const auto scale = scale_value.toDouble();
    if (id.isEmpty() || id.size() > 256 || !std::isfinite(scale) || scale <= 0.0 || scale > 100.0)
        return std::nullopt;
    return SymbolDragPayload{id, scale};
}

double practical_resize_scale(double factor, double extent, double step) noexcept {
    // Snap a physical dimension, retaining uniform scaling and the captured
    // size at zero motion and the existing limits. A zero step selects fine input.
    factor = std::clamp(factor, .05, 20.0);
    if (factor > .05 && factor < 20.0 && std::abs(factor - 1.0) > 1e-12 &&
        std::isfinite(extent) && extent > 1e-9 &&
        std::isfinite(step) && step > 0.0) {
        const auto snapped = std::round(extent * factor / step) * step / extent;
        if (std::isfinite(snapped)) factor = snapped;
    }
    return std::clamp(factor, .05, 20.0);
}

struct GridSpacing {
    double minor;
    double major;
};

GridSpacing grid_spacing(double scale, bool metric) noexcept {
    const auto safe_scale = std::isfinite(scale) && scale > 0.0
        ? std::clamp(scale, minimum_scale, maximum_scale) : 80.0;
    const auto minimum_minor = 14.0 / safe_scale;
    const auto minimum_major = 64.0 / safe_scale;
    GridSpacing result{};
    const auto consider = [&](double metres) {
        if (result.minor == 0.0 && metres >= minimum_minor) result.minor = metres;
        if (result.minor == 0.0 || result.major != 0.0 || metres < minimum_major) return;
        const auto multiple = metres / result.minor;
        if (std::abs(multiple - std::round(multiple)) < 1e-8)
            result.major = metres;
    };
    if (!metric) {
        // Inch fractions and common construction increments, then a 1/2/5
        // foot ladder. Every emphasized interval is also a real ladder entry.
        for (const auto inches : {1.0/16.0, 1.0/8.0, 1.0/4.0, 1.0/2.0,
                                   1.0, 2.0, 3.0, 6.0})
            consider(inches * 0.0254);
    }
    double decade = metric ? 0.001 : 0.3048;
    // This covers the full supported zoom range with ample room for a major
    // interval; the bounded ladder cannot loop on invalid view values.
    for (int exponent = 0; exponent < 13; ++exponent, decade *= 10.0) {
        for (const auto multiple : {1.0, 2.0, 5.0}) consider(decade * multiple);
        if (result.major > 0.0) break;
    }
    return result;
}

QString grid_spacing_label(double metres, bool metric) {
    if (metric) {
        const auto unit = metres < 0.01 ? QStringLiteral("mm")
            : metres < 1.0 ? QStringLiteral("cm") : QStringLiteral("m");
        const auto value = metres < 0.01 ? metres * 1000.0
            : metres < 1.0 ? metres * 100.0 : metres;
        return QStringLiteral("%1 %2").arg(value, 0, 'g', 6).arg(unit);
    }
    const auto inches = metres / 0.0254;
    if (inches < 1.0)
        return QStringLiteral("1/%1 in").arg(qRound(1.0 / inches));
    if (inches < 12.0)
        return QStringLiteral("%1 in").arg(inches, 0, 'g', 6);
    return QStringLiteral("%1 ft").arg(metres / 0.3048, 0, 'g', 6);
}

double paper_stroke_pixels(const CanvasEntity& entity, double pixels_per_mm) {
    if (!std::isfinite(entity.output_stroke_width_mm) || entity.output_stroke_width_mm <= 0.0 ||
        !std::isfinite(pixels_per_mm) || pixels_per_mm <= 0.0) return 0.0;
    const auto width = entity.output_stroke_width_mm * pixels_per_mm;
    return std::isfinite(width) ? std::max(0.1, width) : 0.0;
}

Qt::CursorShape jamb_resize_cursor(const CanvasOpeningWidthControls& controls,
                                   bool keep_start_jamb, double width_scale = 1.0) {
    auto tangent = std::atan2(controls.end_jamb.y-controls.start_jamb.y,
                              controls.end_jamb.x-controls.start_jamb.x);
    if (controls.host_baseline) {
        const auto& host = *controls.host_baseline;
        try {
            const auto length = segment_length(host);
            const auto station = controls.offset_metres + controls.width_metres *
                (keep_start_jamb ? width_scale : 1.0 - width_scale);
            tangent = std::atan2(host.end.y-host.start.y, host.end.x-host.start.x) -
                host.sweep_radians / 2.0 + station * host.sweep_radians / length;
        } catch (const std::exception&) {
            return Qt::SizeHorCursor;
        }
    }
    // Undefined geometry must never reach lround or escape a Qt event handler.
    if (!std::isfinite(tangent)) return Qt::SizeHorCursor;
    const auto angle = std::remainder(-tangent, 2.0*pi);
    const auto direction = (static_cast<int>(std::lround(angle*4/pi)) % 4 + 4) % 4;
    switch (direction) {
    case 1: return Qt::SizeFDiagCursor;
    case 2: return Qt::SizeVerCursor;
    case 3: return Qt::SizeBDiagCursor;
    default: return Qt::SizeHorCursor;
    }
}

bool drawable_label(const CanvasLabel& label) {
    return !label.text.isEmpty() && std::isfinite(label.position.x) &&
           std::isfinite(label.position.y);
}

struct LabelLayout {
    QFont font;
    QRectF bounds;
};

bool same_entity_presentation(const CanvasEntity& left, const CanvasEntity& right) {
    return left.id == right.id && left.presentation_key == right.presentation_key;
}

bool valid_reference_previews(const std::vector<CanvasReference>& proposed,
                              const std::vector<CanvasReference>& retained) {
    if (proposed.empty()) return true;
    QSet<QString> retained_ids;
    for (const auto& reference : retained) retained_ids.insert(reference.id);
    std::set<QString> ids;
    for (const auto& reference : proposed) {
        if (!ids.insert(reference.id).second || reference.image.isNull() ||
            !std::isfinite(reference.position.x) || !std::isfinite(reference.position.y) ||
            !std::isfinite(reference.rotation_degrees) || !std::isfinite(reference.scale) || reference.scale <= 0 ||
            !std::isfinite(reference.metres_per_source_unit) || reference.metres_per_source_unit <= 0 ||
            !retained_ids.contains(reference.id))
            return false;
    }
    return true;
}

bool unambiguous_entity_presentations(const std::vector<CanvasEntity>& entities,
                                      const std::vector<CanvasEntity>& retained) {
    QSet<QString> keyed_roots;
    for (const auto& source : retained)
        if (!source.presentation_key.isEmpty()) keyed_roots.insert(source.id);
    QHash<QString,QSet<QString>> seen;
    for (const auto& entity : entities) {
        auto& keys = seen[entity.id];
        if (entity.presentation_key.isEmpty()) {
            if (keyed_roots.contains(entity.id) ||
                (!keys.isEmpty() && !keys.contains(QString{}))) return false;
        } else {
            if (keys.contains(QString{}) || keys.contains(entity.presentation_key)) return false;
        }
        keys.insert(entity.presentation_key);
    }
    return true;
}

using PresentationSelection = QHash<QPair<QString, QString>, bool>;

PresentationSelection retained_entity_presentation_selection(
    const std::vector<CanvasEntity>& entities) {
    PresentationSelection selected;
    selected.reserve(static_cast<qsizetype>(entities.size()));
    for (const auto& entity : entities) {
        const QPair<QString, QString> key{entity.id, entity.presentation_key};
        // Match find_if: the first retained presentation supplies selection,
        // even when a later duplicate has a different selected flag.
        if (!selected.contains(key)) selected.insert(key, entity.selected);
    }
    return selected;
}

PresentationSelection retained_label_presentation_selection(
    const std::vector<CanvasLabel>& labels) {
    PresentationSelection selected;
    selected.reserve(static_cast<qsizetype>(labels.size()));
    for (const auto& label : labels) {
        // Label presentation identity is exactly owner ID plus callout role.
        const QPair<QString, QString> key{label.id, label.callout_role};
        if (!selected.contains(key)) selected.insert(key, label.selected);
    }
    return selected;
}

int label_text_alignment(const CanvasLabel& label) {
    if (label.text_alignment == QStringLiteral("left")) return Qt::AlignLeft | Qt::AlignVCenter;
    if (label.text_alignment == QStringLiteral("right")) return Qt::AlignRight | Qt::AlignVCenter;
    return Qt::AlignCenter;
}

LabelLayout label_layout(const CanvasLabel& label, QFont base_font,
                         const QPaintDevice* device, double scale, double dpi) {
    if (!label.font_family.isEmpty() &&
        label.font_family.compare(QStringLiteral("sans-serif"), Qt::CaseInsensitive) != 0)
        base_font.setFamily(label.font_family);
    const auto paper_pixels = label.paper_height_mm * dpi / 25.4;
    const auto device_dpi = device ? device->logicalDpiY() : dpi;
    const auto paper_points = (paper_pixels / device_dpi) * 72.0;
    const bool paper = std::isfinite(paper_pixels) && paper_pixels > 0.0 &&
                       paper_pixels <= std::numeric_limits<int>::max() &&
                       std::isfinite(paper_points) && paper_points > 0.0;
    if (paper) {
        // Keep fractional physical size. Fitted sheets can supply a paper
        // transform different from device DPI; convert their desired pixels
        // back to points on the actual metrics/painting device.
        base_font.setPointSizeF(paper_points);
        // Full pixel-grid hinting changes advances with output density.
        // Use scalable paper-text metrics for both layout and painting.
        base_font.setHintingPreference(QFont::PreferNoHinting);
        base_font.setBold(label.bold);
        base_font.setItalic(label.italic);
    } else {
        const auto text_height = std::isfinite(label.text_height_metres) &&
                                 label.text_height_metres > 0.0 ? label.text_height_metres : 0.15;
        const auto instance_scale = std::isfinite(label.scale) && label.scale > 0.0 ? label.scale : 1.0;
        base_font.setPixelSize(static_cast<int>(std::lround(
            std::clamp(text_height * scale * instance_scale, 8.0, 96.0))));
        if (label.bold) base_font.setBold(true);
        if (label.italic) base_font.setItalic(true);
    }
    const QFontMetricsF metrics(base_font, device);
    auto bounds = label.text.contains(QLatin1Char('\n'))
        ? metrics.boundingRect(QRectF(0, 0, 1e6, 1e6), Qt::AlignLeft | Qt::AlignTop, label.text)
        : metrics.boundingRect(label.text);
    bounds.moveCenter(QPointF(0.0, 0.0));
    if (label.text_alignment == QStringLiteral("left")) bounds.moveLeft(0.0);
    else if (label.text_alignment == QStringLiteral("right")) bounds.moveRight(0.0);
    bounds.adjust(-5.0, -3.0, 5.0, 3.0);
    return {base_font, bounds};
}

QTransform label_transform(const CanvasLabel& label, QPointF center) {
    QTransform transform;
    transform.translate(center.x(), center.y());
    if (std::isfinite(label.rotation_radians)) {
        // Model coordinates are y-up while Qt device coordinates are y-down.
        transform.rotate(-label.rotation_radians * 180.0 / pi);
    }
    return transform;
}

CanvasSelectionFrame label_selection_frame(const CanvasLabel& label,
                                           const QRectF& bounds, double scale) {
    const auto center = label_transform(label, {}).map(bounds.center());
    return {{label.position.x + center.x()/scale, label.position.y - center.y()/scale},
            label.rotation_radians, bounds.width()/scale, bounds.height()/scale};
}

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

QString display_cursor_length(double metres, bool metric) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    if (metric) return QStringLiteral("%1 m").arg(metres, 0, 'f', 3);
    constexpr double metres_per_foot = 0.3048;
    return QStringLiteral("%1 ft").arg(metres / metres_per_foot, 0, 'f', 2);
}

Vec2 operator+(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 operator-(Vec2 left, Vec2 right) {
    return {left.x - right.x, left.y - right.y};
}

Vec2 operator*(Vec2 point, double factor) {
    return {point.x * factor, point.y * factor};
}

struct ArcInfo {
    Vec2 center{};
    double radius{};
    double start_angle{};
};

std::optional<ArcInfo> arc_info(const Segment& segment) {
    if (segment.sweep_radians == 0.0) {
        return std::nullopt;
    }
    const auto chord = segment.end - segment.start;
    const auto chord_length = distance(segment.start, segment.end);
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto tangent = std::tan(half_sweep);
    const auto sine = std::sin(std::abs(half_sweep));
    if (!(chord_length > 0.0) || tangent == 0.0 || sine == 0.0 ||
        !std::isfinite(tangent) || !std::isfinite(sine)) {
        return std::nullopt;
    }
    const auto midpoint = (segment.start + segment.end) * 0.5;
    const Vec2 left_normal{-chord.y / chord_length, chord.x / chord_length};
    const auto center = midpoint + left_normal * (chord_length / (2.0 * tangent));
    const auto radius = chord_length / (2.0 * sine);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius)) {
        return std::nullopt;
    }
    return ArcInfo{center, radius,
                   std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}

Vec2 arc_point(const Segment& segment, const ArcInfo& arc, double parameter) {
    const auto angle = arc.start_angle + segment.sweep_radians * parameter;
    return arc.center + Vec2{std::cos(angle), std::sin(angle)} * arc.radius;
}

double point_segment_distance(QPointF point, QPointF start, QPointF end) {
    const auto dx = end.x() - start.x();
    const auto dy = end.y() - start.y();
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared <= std::numeric_limits<double>::epsilon()) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }
    const auto projection =
        std::clamp(((point.x() - start.x()) * dx + (point.y() - start.y()) * dy) /
                       length_squared,
                   0.0, 1.0);
    return std::hypot(point.x() - (start.x() + projection * dx),
                      point.y() - (start.y() + projection * dy));
}

QColor color_for(const CanvasEntity& entity, bool light_surface) {
    if (entity.selected) {
        return light_surface ? QColor(37, 99, 235) : QColor(75, 210, 255);
    }
    if (entity.type == QStringLiteral("wall")) {
        return light_surface ? QColor(35, 77, 113) : QColor(143, 198, 245);
    }
    if (entity.type == QStringLiteral("room_boundary")) {
        return light_surface ? QColor(48, 91, 128) : QColor(139, 199, 244);
    }
    if (entity.type == QStringLiteral("measurement_boundary")) {
        return light_surface ? QColor(37, 91, 145) : QColor(128, 194, 246);
    }
    if (entity.type == QStringLiteral("boundary")) {
        return light_surface ? QColor(37, 91, 145) : QColor(128, 194, 246);
    }
    if (entity.type == QStringLiteral("slab")) {
        return light_surface ? QColor(69, 102, 129) : QColor(152, 195, 226);
    }
    if (entity.type == QStringLiteral("terrain_surface")) {
        return QColor(119, 164, 113);
    }
    if (entity.type == QStringLiteral("dimension_line")) {
        return light_surface ? QColor(82, 101, 121) : QColor(190, 207, 225);
    }
    if (entity.type == QStringLiteral("opening") ||
        entity.type == QStringLiteral("window") ||
        entity.type == QStringLiteral("symbol")) {
        return light_surface ? QColor(57, 70, 84) : QColor(210, 226, 239);
    }
    return light_surface ? QColor(83, 99, 116) : QColor(188, 205, 222);
}

Qt::BrushStyle hatch_style(QString pattern) {
    pattern = pattern.trimmed().toLower();
    if (pattern.isEmpty() || pattern == QStringLiteral("none")) {
        return Qt::NoBrush;
    }
    if (pattern == QStringLiteral("solid") || pattern == QStringLiteral("filled")) {
        return Qt::SolidPattern;
    }
    if (pattern == QStringLiteral("horizontal") || pattern == QStringLiteral("hor")) {
        return Qt::HorPattern;
    }
    if (pattern == QStringLiteral("vertical") || pattern == QStringLiteral("vert")) {
        return Qt::VerPattern;
    }
    if (pattern == QStringLiteral("cross")) {
        return Qt::CrossPattern;
    }
    if (pattern == QStringLiteral("diagonal") || pattern == QStringLiteral("diag") ||
        pattern == QStringLiteral("backward_diagonal")) {
        return Qt::BDiagPattern;
    }
    if (pattern == QStringLiteral("forward_diagonal")) {
        return Qt::FDiagPattern;
    }
    if (pattern == QStringLiteral("diagonal_cross") || pattern == QStringLiteral("diagcross")) {
        return Qt::DiagCrossPattern;
    }
    if (pattern == QStringLiteral("dots") || pattern == QStringLiteral("concrete")) {
        return Qt::Dense4Pattern;
    }
    if (pattern == QStringLiteral("dense")) {
        return Qt::Dense6Pattern;
    }
    // ViewPresentation deliberately accepts user-defined identifier names.
    // An unknown name still renders deterministically while remaining visible
    // in the persisted document for a future catalog entry.
    return Qt::BDiagPattern;
}

bool append_closed_boundary(QPainterPath& path, const Boundary& boundary) {
    // Analytical loops are not polygons: a semicircle plus its closing chord
    // is a valid two-segment boundary. Continuity and closure below are the
    // relevant presentation checks; semantic geometry validation remains in
    // the document model.
    if (boundary.empty()) return false;
    constexpr double endpoint_tolerance = 1e-7;
    const auto& first = boundary.front();
    path.moveTo(first.start.x, first.start.y);
    auto previous = first.start;
    for (const auto& segment : boundary) {
        if (distance(previous, segment.start) > endpoint_tolerance) {
            return false;
        }
        if (segment.sweep_radians == 0.0) {
            path.lineTo(segment.end.x, segment.end.y);
        } else {
            const auto arc = arc_info(segment);
            if (!arc.has_value()) return false;
            const QRectF bounds(arc->center.x - arc->radius, arc->center.y - arc->radius,
                               arc->radius * 2.0, arc->radius * 2.0);
            path.arcTo(bounds, -arc->start_angle * 180.0 / pi,
                       -segment.sweep_radians * 180.0 / pi);
        }
        previous = segment.end;
    }
    if (distance(previous, first.start) > endpoint_tolerance) return false;
    path.closeSubpath();
    return true;
}

std::optional<QPainterPath> closed_entity_path(const CanvasEntity& entity) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    if (entity.type == QStringLiteral("wall")) {
        // Hosted cuts split the footprint into separate closed wall runs.
        // Keep their gaps empty when picking the physical body of a wall.
        Boundary run;
        for (const auto& segment : entity.segments) {
            run.push_back(segment);
            if (distance(run.front().start, segment.end) <= 1e-7) {
                if (!append_closed_boundary(path, run)) return std::nullopt;
                run.clear();
            }
        }
        if (!run.empty() || path.isEmpty()) return std::nullopt;
    } else if (!append_closed_boundary(path, entity.segments)) return std::nullopt;
    for (const auto& hole : entity.holes) {
        if (!append_closed_boundary(path, hole)) return std::nullopt;
    }
    return path;
}

bool wall_baseline_only(const CanvasEntity& entity) {
    // Semantic projections already carry both wall faces and opening gaps.
    // Only the legacy single-baseline representation needs a thickness pen.
    return entity.type == QStringLiteral("wall") && entity.segments.size() == 1;
}

struct EntityStrokeWidth {
    double width;
    bool cosmetic;
};

EntityStrokeWidth entity_stroke_width(const CanvasEntity& entity, bool output,
                                     double model_scale, double pixels_per_mm,
                                     double default_pixels_per_mm) {
    const auto paper_width = (output || entity.paper_stroke_width_on_screen)
        ? paper_stroke_pixels(entity, pixels_per_mm) : 0.0;
    if (paper_width > 0.0 && std::isfinite(paper_width))
        return {std::max(0.1, paper_width), true};
    if (!output && entity.type == QStringLiteral("symbol"))
        return {std::max(1.15, entity.stroke_width_metres * model_scale), true};
    if (wall_baseline_only(entity)) return {std::max(entity.thickness_metres, 0.04), false};
    if (entity.stroke_width_metres > 0.0 && std::isfinite(entity.stroke_width_metres))
        return {entity.stroke_width_metres, false};
    return {output ? default_pixels_per_mm * 0.25 : entity.selected ? 3.0 : 1.5, true};
}

struct EntityStrokeEnvelope {
    double model_width{};
    double cosmetic_pixels{};
    double symbol_metres{};
    double paper_mm{};
};

// Qt's winding test excludes one side of an exact path boundary. A subpixel
// outward margin preserves inclusive pointer tolerance without changing any
// model measurement; the retained query includes the same margin.
constexpr double pick_edge_padding_pixels = 1.0 / 1024.0;

// Retain the same width precedence as entity_stroke_width without binding an
// index to the current zoom, DPI or selection. Both paint and picking consume
// these coefficients; the default encloses either selected cosmetic width.
std::optional<EntityStrokeEnvelope> entity_stroke_envelope(const CanvasEntity& entity) {
    EntityStrokeEnvelope envelope;
    if (entity.paper_stroke_width_on_screen &&
        std::isfinite(entity.output_stroke_width_mm) && entity.output_stroke_width_mm > 0.0) {
        envelope.paper_mm = entity.output_stroke_width_mm;
        envelope.cosmetic_pixels = 0.1;
    } else if (entity.type == QStringLiteral("symbol")) {
        if (!std::isfinite(entity.stroke_width_metres)) return std::nullopt;
        envelope.cosmetic_pixels = 1.15;
        envelope.symbol_metres = std::max(0.0, entity.stroke_width_metres);
    } else if (wall_baseline_only(entity)) {
        if (!std::isfinite(entity.thickness_metres)) return std::nullopt;
        envelope.model_width = std::max(entity.thickness_metres, 0.04);
    } else if (entity.stroke_width_metres > 0.0 && std::isfinite(entity.stroke_width_metres)) {
        envelope.model_width = entity.stroke_width_metres;
    } else envelope.cosmetic_pixels = 3.0;
    return envelope;
}

double entity_screen_stroke_width(const CanvasEntity& entity, double model_scale,
                                  double pixels_per_mm) {
    const auto stroke = entity_stroke_width(entity, false, model_scale,
                                            pixels_per_mm, pixels_per_mm);
    const auto width = stroke.cosmetic ? stroke.width : stroke.width * model_scale;
    return std::isfinite(width) && width > 0.0 ? width : 0.0;
}

double point_path_distance(QPointF point, const QPainterPath& screen_path) {
    auto best = std::numeric_limits<double>::max();
    // Qt flattens the actual painted cubics at the current screen scale. Fixed
    // model-space arc samples leave increasingly large unpickable gaps on zoom.
    for (const auto& polygon : screen_path.toSubpathPolygons()) {
        for (qsizetype i = 1; i < polygon.size(); ++i)
            best = std::min(best, point_segment_distance(point, polygon[i - 1], polygon[i]));
    }
    return best;
}

bool finite_rect(const QRectF& rect) {
    return std::isfinite(rect.left()) && std::isfinite(rect.right()) &&
           std::isfinite(rect.top()) && std::isfinite(rect.bottom());
}

// Placement only asks whether any existing label intersects a candidate.
// Keep the exact rectangles and Qt predicate, using cells to narrow that
// boolean query. Large or unsafe rectangles always take the original scan.
class LabelObstacleIndex {
public:
    void add(const QRectF& bounds) {
        m_rectangles.push_back(bounds);
        if (m_indexed) index(m_rectangles.size() - 1);
    }

    void enable() {
        if (m_indexed) return;
        m_indexed = true;
        for (std::size_t i = 0; i < m_rectangles.size(); ++i) index(i);
    }

    [[nodiscard]] bool intersects(const QRectF& candidate) const {
        const auto range = cellRange(candidate);
        if (!m_indexed || !range) {
            return std::any_of(m_rectangles.begin(), m_rectangles.end(),
                [&](const auto& bounds) { return candidate.intersects(bounds); });
        }
        for (const auto i : m_fallback)
            if (candidate.intersects(m_rectangles[i])) return true;
        for (auto x = range->left; x <= range->right; ++x) {
            for (auto y = range->top; y <= range->bottom; ++y) {
                const auto cell = m_cells.find({x, y});
                if (cell == m_cells.end()) continue;
                for (const auto i : cell->second)
                    if (candidate.intersects(m_rectangles[i])) return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<QRectF>& rectangles() const { return m_rectangles; }

private:
    struct CellRange {
        std::int64_t left, right, top, bottom;
    };

    static std::optional<CellRange> cellRange(const QRectF& bounds) {
        const auto normalized = bounds.normalized();
        // Power-of-two cells preserve ordered endpoints without lossy integer
        // quantization. Guard conversion and bound both insertion/query work.
        constexpr double cell_size = 128.0;
        constexpr double coordinate_limit = 1099511627776.0;
        if (!finite_rect(normalized) ||
            std::max({std::abs(normalized.left()), std::abs(normalized.right()),
                      std::abs(normalized.top()), std::abs(normalized.bottom())}) > coordinate_limit)
            return std::nullopt;
        const CellRange range{
            static_cast<std::int64_t>(std::floor(normalized.left() / cell_size)),
            static_cast<std::int64_t>(std::floor(normalized.right() / cell_size)),
            static_cast<std::int64_t>(std::floor(normalized.top() / cell_size)),
            static_cast<std::int64_t>(std::floor(normalized.bottom() / cell_size))};
        const auto columns = range.right - range.left + 1;
        const auto rows = range.bottom - range.top + 1;
        if (columns <= 0 || rows <= 0 || columns > 64 || rows > 64 || columns * rows > 64)
            return std::nullopt;
        return range;
    }

    void index(std::size_t i) {
        const auto range = cellRange(m_rectangles[i]);
        if (!range) { m_fallback.push_back(i); return; }
        for (auto x = range->left; x <= range->right; ++x)
            for (auto y = range->top; y <= range->bottom; ++y)
                m_cells[{x, y}].push_back(i);
    }

    bool m_indexed{};
    std::vector<QRectF> m_rectangles;
    std::vector<std::size_t> m_fallback;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> m_cells;
};

bool ordinary_axis_transform(const QTransform& transform) {
    return transform.isAffine() && transform.m33() == 1.0 &&
           transform.m12() == 0.0 && transform.m21() == 0.0 &&
           std::isfinite(transform.m11()) && transform.m11() != 0.0 &&
           std::isfinite(transform.m22()) && transform.m22() != 0.0 &&
           std::isfinite(transform.dx()) && std::isfinite(transform.dy());
}

void append_boundary_strokes(QPainterPath& path, const Boundary& boundary) {
    for (const auto& segment : boundary) {
        path.moveTo(segment.start.x, segment.start.y);
        if (segment.sweep_radians == 0.0) {
            path.lineTo(segment.end.x, segment.end.y);
        } else if (const auto arc = arc_info(segment)) {
            path.arcTo(QRectF(arc->center.x - arc->radius, arc->center.y - arc->radius,
                             2.0 * arc->radius, 2.0 * arc->radius),
                       -arc->start_angle * 180.0 / pi,
                       -segment.sweep_radians * 180.0 / pi);
        }
    }
}

QPainterPath symbol_pick_footprint(const CanvasEntity& entity, const QPainterPath& stroke) {
    std::optional<CanvasSelectionFrame> frame = entity.resize_frame;
    if (!frame && entity.svg_symbol) {
        const auto& symbol = *entity.svg_symbol;
        frame = CanvasSelectionFrame{symbol.position, symbol.rotation_radians,
            symbol.width_metres, symbol.depth_metres};
    }
    if (frame && std::isfinite(frame->center.x) && std::isfinite(frame->center.y) &&
        std::isfinite(frame->rotation_radians) && std::isfinite(frame->width_metres) &&
        std::isfinite(frame->depth_metres) && frame->width_metres > 0.0 && frame->depth_metres > 0.0) {
        QPainterPath footprint;
        footprint.addRect(-frame->width_metres * 0.5, -frame->depth_metres * 0.5,
                           frame->width_metres, frame->depth_metres);
        QTransform placement;
        placement.translate(frame->center.x, frame->center.y);
        placement.rotate(frame->rotation_radians * 180.0 / pi);
        auto placed = placement.map(footprint);
        if (finite_rect(placed.controlPointRect())) return placed;
    }
    // Older vector symbols have a footprint loop plus independent artwork
    // chains, so they are not one closed boundary. Preserve their established
    // complete bounding footprint when no physical axes were retained.
    QPainterPath footprint;
    if (!stroke.isEmpty() && finite_rect(stroke.controlPointRect()))
        footprint.addRect(stroke.controlPointRect());
    return footprint;
}

}  // namespace

QRectF canvasLabelLayoutBounds(const CanvasLabel& label, QFont font,
    const QPaintDevice* device, double pixels_per_metre, double dpi_y) {
    if (!device || !(pixels_per_metre > 0.0) || !std::isfinite(pixels_per_metre) ||
        !(dpi_y > 0.0) || !std::isfinite(dpi_y)) return {};
    return label_layout(label, std::move(font), device, pixels_per_metre, dpi_y).bounds;
}

PlanCanvas::PlanCanvas(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(480, 360);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setAttribute(Qt::WA_TabletTracking, true);
    setAutoFillBackground(false);
    qApp->installEventFilter(this);
}

void PlanCanvas::setEntities(std::vector<CanvasEntity> entities) {
    m_pending_dimension_space_tap.reset();
    // Replacing the document projection invalidates the captured revision and
    // its transient host-wall geometry, even when the selected ID survives.
    if (m_touch_active || m_gesture_button != Qt::NoButton ||
        m_opening_width_handle || m_vertex_move_handle || m_move_release_pending ||
        m_transform_frame_start) resetGesture();
    else {
        ++m_opening_width_preview_serial;
        m_opening_width_labels_preview.clear();
        ++m_boundary_vertex_preview_serial;
        ++m_move_preview_serial;
        ++m_transform_preview_serial;
    }
    resetTouchInput();
    m_entities = std::move(entities);
    m_published_entity_geometry.clear();
    m_published_geometry_index_ready = false;
    m_published_geometry_index_entries.clear();
    m_published_geometry_index_nodes.clear();
    m_published_geometry_index_fallback.clear();
    m_entity_hit_index_ready = false;
    m_entity_hit_index_entries.clear();
    m_entity_hit_index_nodes.clear();
    m_entity_hit_index_fallback.clear();
    m_local_snap_index_ready = false;
    m_local_snap_index_entries.clear();
    m_local_snap_index_nodes.clear();
    m_local_snap_index_fallback.clear();
    invalidateRetainedPresentation();
    ++m_sketch_content_revision;
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setFloorGhost(std::vector<CanvasEntity> entities, double opacity,
                             Vec2 offset_metres, std::vector<CanvasLabel> labels) {
    if (!std::isfinite(offset_metres.x) || !std::isfinite(offset_metres.y)) {
        clearFloorGhost();
        return;
    }
    // Selected source-floor items are ordinary reference ink, never active
    // selection presentation. Clear only the retained canvas copy.
    for (auto& entity : entities) entity.selected = false;
    for (auto& label : labels) label.selected = false;
    m_floor_ghost_entities = std::move(entities);
    m_floor_ghost_labels = std::move(labels);
    m_floor_ghost_opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.05, 0.75) : 0.25;
    m_floor_ghost_offset = offset_metres;
    m_label_placement_cache[3] = {};
    update();
}

void PlanCanvas::clearFloorGhost() {
    m_floor_ghost_entities.clear();
    m_floor_ghost_labels.clear();
    m_floor_ghost_offset = {};
    m_label_placement_cache[3] = {};
    update();
}

void PlanCanvas::setTool(CanvasTool tool) {
    resetGesture();
    resetTouchInput();
    m_tool = tool;
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    else setCursor(tool == CanvasTool::select ? Qt::ArrowCursor : Qt::CrossCursor);
    setFocus();
    update();
}

void PlanCanvas::setGridEnabled(bool enabled) {
    m_grid_enabled = enabled;
    update();
}

void PlanCanvas::setSnapEnabled(bool enabled) {
    if (m_snap_enabled == enabled) return;
    clearSymbolDragPreview();
    m_snap_enabled = enabled;
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setWallSnapEnabled(bool enabled) {
    if (m_wall_snap_enabled == enabled) return;
    m_wall_snap_enabled = enabled;
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setPerformanceMeasured(std::function<void(PerformanceMetric,
                                       std::chrono::steady_clock::duration)> callback) {
    resetPerformanceMeasurements();
    m_performance_measured = std::move(callback);
}

void PlanCanvas::beginPerformanceMeasurement(PerformanceMetric metric,
                                            PerformanceClock::time_point started) {
    if (!m_performance_measured || !isVisible() || QApplication::activeModalWidget()) return;
    // Use only the enum: the standalone canvas does not link core telemetry.
    switch (metric) {
    case PerformanceMetric::navigation:
    case PerformanceMetric::input:
    case PerformanceMetric::edit:
    case PerformanceMetric::open:
    case PerformanceMetric::save:
        break;
    default:
        return;
    }
    const auto pending = std::find_if(m_pending_measurements.begin(), m_pending_measurements.end(),
        [metric](const auto& item) { return item.first == metric; });
    if (pending == m_pending_measurements.end())
        m_pending_measurements.emplace_back(metric, started);
    else
        pending->second = std::min(pending->second, started);
    // Even an input that only changes focus needs a completed canvas paint.
    update();
}

void PlanCanvas::cancelPerformanceMeasurement(PerformanceMetric metric) {
    std::erase_if(m_pending_measurements, [metric](const auto& item) { return item.first == metric; });
}

void PlanCanvas::resetPerformanceMeasurements() {
    m_pending_measurements.clear();
    ++m_measurement_generation;
}

void PlanCanvas::setOverviewMapEnabled(bool enabled) {
    if (m_overview_map_enabled == enabled) return;
    m_overview_map_enabled = enabled;
    update();
}

void PlanCanvas::setMetricUnits(bool metric) {
    if (m_metric_units == metric) return;
    m_metric_units = metric;
    invalidateRetainedPresentation();
    ++m_sketch_content_revision;
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setSelectionFilter(CanvasSelectionFilter filter) {
    if (m_selection_filter == filter) return;
    m_selection_filter = filter;
    // A selection press captures a target under the old filter. Navigation
    // and explicit selected transform controls do not capture a new pick.
    if (m_gesture_button == Qt::RightButton || m_left_gesture == LeftGesture::marquee ||
        m_left_gesture == LeftGesture::canvas_pan || m_left_gesture == LeftGesture::object_move ||
        m_transform_frame_start) {
        resetGesture();
        resetTouchInput();
    }
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setCanvasBackground(QColor background) {
    if (!background.isValid()) return;
    if (m_canvas_background == background) return;
    m_canvas_background = std::move(background);
    m_overview_geometry_cache = {};
    update();
}

void PlanCanvas::setSelectedId(const QString& entity_id) {
    setSelectedIds(entity_id.isEmpty() ? QStringList{} : QStringList{entity_id});
}

void PlanCanvas::setSelectedIds(const QStringList& entity_ids) {
    if ((m_gesture_button != Qt::NoButton || m_opening_width_handle || m_vertex_move_handle || m_opening_move_active ||
         m_transform_frame_start) &&
        selectedIds() != entity_ids)
        resetGesture();
    for (auto& entity : m_entities) {
        entity.selected = entity_ids.contains(entity.id);
    }
    for (auto& label : m_labels)
        label.selected = !label.plan_only && entity_ids.contains(label.id);
    for (auto& reference : m_references) reference.selected = entity_ids.contains(reference.id);
    invalidateRetainedPresentation();
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setSelectionCaption(QString caption) {
    if (m_selection_caption == caption) return;
    m_selection_caption = std::move(caption);
    update();
}

void PlanCanvas::setSelectionTransformEnabled(bool resize_enabled, bool rotate_enabled) {
    if (m_selection_resize_enabled == resize_enabled &&
        m_selection_rotate_enabled == rotate_enabled) return;
    m_selection_resize_enabled = resize_enabled;
    m_selection_rotate_enabled = rotate_enabled;
    if ((!resize_enabled && m_left_gesture == LeftGesture::selection_resize) ||
        (!rotate_enabled && m_left_gesture == LeftGesture::selection_rotate)) resetGesture();
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setSelectionAxisResizeEnabled(bool enabled) {
    if (m_selection_axis_resize_enabled == enabled) return;
    m_selection_axis_resize_enabled = enabled;
    if (!enabled && m_left_gesture == LeftGesture::selection_axis_resize) resetGesture();
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setLabels(std::vector<CanvasLabel> labels) {
    // A captured projection depends on the source annotations as well as the
    // geometry, even when a replacement retains every annotation identity.
    if (m_touch_active || m_gesture_button != Qt::NoButton || m_opening_width_handle || m_vertex_move_handle || m_move_release_pending ||
        m_transform_frame_start) resetGesture();
    else {
        ++m_opening_width_preview_serial;
        m_opening_width_labels_preview.clear();
    }
    resetTouchInput();
    // Derived plan labels share their owner's ID for output filtering; they
    // are not independent annotations with their own transform controls.
    for (auto& label : labels) if (label.plan_only) label.selected = false;
    m_labels = std::move(labels);
    invalidateRetainedPresentation();
    ++m_sketch_content_revision;
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setReference(std::optional<CanvasReference> reference) {
    std::vector<CanvasReference> references;
    if (reference) references.push_back(std::move(*reference));
    setReferences(std::move(references));
}

void PlanCanvas::setReferences(std::vector<CanvasReference> references) {
    if (m_touch_active || m_gesture_button != Qt::NoButton || m_move_release_pending || m_transform_frame_start) resetGesture();
    resetTouchInput();
    m_references = std::move(references);
    invalidateRetainedPresentation();
    if (m_last_mouse_position) updatePointerCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setReferenceGrids(std::vector<CanvasReferenceGrid> grids) {
    if (m_touch_active) resetGesture();
    resetTouchInput();
    m_reference_grids = std::move(grids);
    invalidateRetainedPresentation();
    update();
}

void PlanCanvas::setBoundaryPreview(std::vector<Vec2> points) {
    m_boundary_preview = std::move(points);
    update();
}

void PlanCanvas::setWallPreview(std::optional<WallDraftPreview> wall) {
    m_wall_preview = std::move(wall);
    update();
}

void PlanCanvas::setDrawingWitnesses(std::vector<DrawingWitness> witnesses) {
    m_drawing_witnesses = std::move(witnesses);
    update();
}

void PlanCanvas::setBoundaryDraftPreview(std::optional<BoundaryDraftPreview> preview) {
    m_boundary_draft_preview = std::move(preview);
    update();
}

void PlanCanvas::setComponentPlacementPreview(std::optional<CanvasEntity> preview) {
    if (preview) preview->selected = false;
    m_component_placement_preview = std::move(preview);
    update();
}

void PlanCanvas::clearPreview() {
    clearSymbolDragPreview();
    m_pending_dimension_space_tap.reset();
    m_boundary_preview.clear();
    m_wall_preview.reset();
    m_drawing_witnesses.clear();
    m_boundary_draft_preview.reset();
    m_component_placement_preview.reset();
    update();
}

std::optional<std::pair<Vec2, Vec2>> PlanCanvas::committedContentBounds() const {
    QByteArray metrics_key;
    QDataStream metrics(&metrics_key, QIODevice::WriteOnly);
    metrics << font() << qint32(logicalDpiX()) << qint32(logicalDpiY())
            << qint32(physicalDpiX()) << qint32(physicalDpiY())
            << devicePixelRatioF() << qint32(devType()) << size();
    if (m_content_bounds_cache.ready && m_content_bounds_cache.metrics_key == metrics_key)
        return m_content_bounds_cache.bounds;
    Vec2 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 maximum{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool has_content = false;
    const auto include = [&](Vec2 point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        has_content = true;
    };
    for (const auto& entity : m_entities) {
        for (const auto& segment : entity.segments) {
            include(segment.start);
            include(segment.end);
            try {
                const auto bounds = segment_bounds(segment);
                include(bounds.minimum);
                include(bounds.maximum);
            } catch (const std::invalid_argument&) {
                // Invalid retained presentation geometry still contributes its
                // finite endpoints; semantic diagnostics belong to the model.
            }
        }
        for (const auto& hole : entity.holes) {
            for (const auto& segment : hole) {
                include(segment.start);
                include(segment.end);
                try {
                    const auto bounds = segment_bounds(segment);
                    include(bounds.minimum);
                    include(bounds.maximum);
                } catch (const std::invalid_argument&) {
                }
            }
        }
    }
    for (const auto& label : m_labels) {
        include(label.position);
        if (label.leader_start) include(*label.leader_start);
        if (label.avoid_components && drawable_label(label)) {
            const auto layout=label_layout(label,font(),this,80.0,logicalDpiY());
            const auto footprint=label_transform(label,{}).mapRect(layout.bounds);
            include({label.position.x+footprint.left()/80.0,label.position.y-footprint.bottom()/80.0});
            include({label.position.x+footprint.right()/80.0,label.position.y-footprint.top()/80.0});
        }
    }
    for (const auto& reference : m_references) {
        if (reference.visible && !reference.image.isNull() &&
            std::isfinite(reference.metres_per_source_unit) &&
            reference.metres_per_source_unit > 0.0 && std::isfinite(reference.scale) &&
            reference.scale > 0.0) {
            const auto width = reference.image.width() * reference.metres_per_source_unit *
                               reference.scale;
            const auto height = reference.image.height() * reference.metres_per_source_unit *
                                reference.scale;
            include({reference.position.x - width * 0.5, reference.position.y - height * 0.5});
            include({reference.position.x + width * 0.5, reference.position.y + height * 0.5});
        }
    }
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            include(line.start);
            include(line.end);
        }
    }
    m_content_bounds_cache.metrics_key = std::move(metrics_key);
    m_content_bounds_cache.bounds = has_content ? std::make_optional(std::make_pair(minimum, maximum))
                                               : std::nullopt;
    m_content_bounds_cache.ready = true;
    return m_content_bounds_cache.bounds;
}

std::optional<std::pair<Vec2, Vec2>> PlanCanvas::contentBounds(bool include_drafts) const {
    const auto committed = committedContentBounds();
    if (!include_drafts) return committed;
    // Start with the exact committed extrema, retaining the original min/max
    // tie behavior, then append live draft contributions in their source order.
    Vec2 minimum = committed ? committed->first :
        Vec2{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 maximum = committed ? committed->second :
        Vec2{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool has_content = committed.has_value();
    const auto include = [&](Vec2 point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        has_content = true;
    };
    // Draft extents are navigation-only. Fitted output and fitView retain the
    // committed content contract, with no transient entities manufactured.
    if (include_drafts) {
        const auto include_segment = [&](const Segment& segment) {
            include(segment.start);
            include(segment.end);
            try {
                const auto bounds = segment_bounds(segment);
                include(bounds.minimum);
                include(bounds.maximum);
            } catch (const std::invalid_argument&) {
                // As with retained content, finite endpoints remain useful.
            }
        };
        for (const auto& point : m_boundary_preview) include(point);
        if (m_wall_preview) {
            include(m_wall_preview->start);
            include(m_wall_preview->end);
        }
        if (m_boundary_draft_preview) {
            const auto& draft = *m_boundary_draft_preview;
            for (const auto& segment : draft.segments) include_segment(segment);
            if (draft.rubber_band) include_segment(*draft.rubber_band);
            if (draft.anchor) include(*draft.anchor);
            if (draft.pen_position) include(*draft.pen_position);
        }
    }
    if (!has_content) return std::nullopt;
    return std::make_pair(minimum, maximum);
}

QRectF PlanCanvas::overviewMapRect() const noexcept {
    if (!m_overview_map_enabled) return {};
    constexpr qreal width = 180.0;
    constexpr qreal height = 118.0;
    constexpr qreal margin = 12.0;
    const auto available_width = std::max<qreal>(0.0, rect().width() - margin * 2.0);
    const auto available_height = std::max<qreal>(0.0, rect().height() - margin * 2.0);
    const auto map_width = std::min(width, available_width);
    const auto map_height = std::min(height, available_height);
    if (map_width < 80.0 || map_height < 56.0) return {};
    return QRectF(rect().right() - margin - map_width + 1.0,
                  rect().bottom() - margin - map_height + 1.0, map_width, map_height);
}

void PlanCanvas::fitView() {
    const auto previous_center = m_view_center;
    const auto previous_scale = m_scale;
    beginPerformanceMeasurement(PerformanceMetric::navigation);
    const auto bounds = contentBounds();
    if (!bounds) {
        m_view_center = {0.0, 0.0};
        m_scale = 80.0;
        if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
        update();
        notifyNavigationChanged(previous_center, previous_scale);
        return;
    }
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto width = std::max(maximum.x - minimum.x, 0.1);
    const auto height = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(width, height) * 0.12 + 0.25;
    m_view_center = {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y)};
    m_scale = std::clamp(std::min((width + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().width())) /
                                            (width + padding * 2.0)
                                      : 80.0,
                                  (height + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().height())) /
                                            (height + padding * 2.0)
                                      : 80.0),
                        minimum_scale, maximum_scale);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
    notifyNavigationChanged(previous_center, previous_scale);
}

void PlanCanvas::zoomBy(double factor, QPointF anchor) {
    if (!(factor > 0.0) || !std::isfinite(factor)) {
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::navigation);
    const auto previous_center = m_view_center;
    const auto previous_scale = m_scale;
    if (anchor.isNull()) {
        anchor = rect().center();
    }
    const auto before = toModel(anchor, rect());
    m_scale = std::clamp(m_scale * factor, minimum_scale, maximum_scale);
    const auto after = toModel(anchor, rect());
    m_view_center = m_view_center + (before - after);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
    notifyNavigationChanged(previous_center, previous_scale);
}

void PlanCanvas::setViewTransform(Vec2 center, double scale) {
    if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(scale) || scale <= 0.0) return;
    const auto previous_center = m_view_center;
    const auto previous_scale = m_scale;
    scale = std::clamp(scale, minimum_scale, maximum_scale);
    if (center.x == previous_center.x && center.y == previous_center.y &&
        scale == previous_scale) return;
    beginPerformanceMeasurement(PerformanceMetric::navigation);
    m_view_center = center;
    m_scale = scale;
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
    notifyNavigationChanged(previous_center, previous_scale);
}

void PlanCanvas::setNavigationChanged(std::function<void(Vec2, double)> callback) {
    m_navigation_changed = std::move(callback);
}

void PlanCanvas::notifyNavigationChanged(Vec2 previous_center, double previous_scale) {
    if (m_view_center.x == previous_center.x && m_view_center.y == previous_center.y &&
        m_scale == previous_scale) return;
    clearSymbolDragPreview();
    // Hosted station captures belong to the pressed view. Navigation must
    // invalidate a drag or released pending admission before it can reappear.
    if (m_opening_move_active) resetGesture();
    ++m_navigation_generation;
    m_pending_dimension_space_tap.reset();
    const auto callback = m_navigation_changed;
    if (callback) callback(m_view_center, m_scale);
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport) const {
    renderSceneWithTransform(painter, viewport, false, m_canvas_background,
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport, bool fit_to_content,
                             QColor background) const {
    renderSceneWithTransform(painter, viewport, fit_to_content, background,
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderSceneAt(QPainter& painter, const QRectF& viewport, double scale,
                               Vec2 view_center, QColor background,
                               std::optional<double> paper_pixels_per_mm) const {
    if (!(std::isfinite(scale) && scale > 0.0) || !std::isfinite(view_center.x) ||
        !std::isfinite(view_center.y)) {
        return;
    }
    renderSceneWithTransform(painter, viewport, false, background, scale, view_center,
                             paper_pixels_per_mm);
}

std::optional<CanvasSketchContentRecording> PlanCanvas::recordSketchContent(
    double model_scale, QString* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    const auto reject = [&](const QString& reason) -> std::optional<CanvasSketchContentRecording> {
        if (diagnostic) *diagnostic = reason;
        return std::nullopt;
    };
    if (!std::isfinite(model_scale) || model_scale < minimum_scale || model_scale > maximum_scale)
        return reject(tr("The Sketch output scale is outside the supported range."));

    // QPicture stores integer device bounds. Preflight before recording to
    // prevent overflow from becoming a plausible but silently clipped crop.
    constexpr double maximum_coordinate = 1'000'000.0;
    Vec2 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 maximum{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool has_content = false;
    const auto include = [&](Vec2 point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
            throw std::invalid_argument("nonfinite content coordinate");
        minimum.x = std::min(minimum.x, point.x); minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x); maximum.y = std::max(maximum.y, point.y);
        has_content = true;
    };
    const auto finite_rect = [](QRectF rect) {
        return std::isfinite(rect.left()) && std::isfinite(rect.top()) &&
               std::isfinite(rect.right()) && std::isfinite(rect.bottom());
    };
    const auto include_boundary = [&](const Boundary& boundary) {
        for (const auto& segment : boundary) {
            const auto bounds = segment_bounds(segment);
            include(bounds.minimum); include(bounds.maximum);
        }
    };
    CanvasSketchContentRecording recording;
    recording.model_scale = model_scale;
    recording.pixels_per_mm = recording.picture.logicalDpiX() / 25.4;
    if (recording.picture.logicalDpiX() != recording.picture.logicalDpiY() ||
        !std::isfinite(recording.pixels_per_mm) || recording.pixels_per_mm <= 0)
        return reject(tr("The Sketch recorder has unsupported device resolution."));
    try {
        for (const auto& entity : m_entities) {
            if (!std::isfinite(entity.thickness_metres) ||
                !std::isfinite(entity.stroke_width_metres) ||
                !std::isfinite(entity.output_stroke_width_mm) || !std::isfinite(entity.hatch_scale))
                throw std::invalid_argument("nonfinite entity presentation");
            const auto stroke_extent = std::max({0.0, entity.thickness_metres * model_scale,
                entity.stroke_width_metres * model_scale,
                entity.output_stroke_width_mm * recording.pixels_per_mm});
            if (!std::isfinite(stroke_extent) || stroke_extent > maximum_coordinate)
                return reject(tr("The Sketch line weight exceeds the supported output bounds."));
            include_boundary(entity.segments);
            if (entity.stroke_segments) include_boundary(*entity.stroke_segments);
            for (const auto& hole : entity.holes) include_boundary(hole);
            if (entity.svg_symbol) {
                const auto& symbol = *entity.svg_symbol;
                if (!finite_rect(symbol.view_box) || !finite_rect(symbol.footprint_view_box) ||
                    symbol.view_box.width() <= 0 || symbol.view_box.height() <= 0 ||
                    symbol.footprint_view_box.width() <= 0 || symbol.footprint_view_box.height() <= 0 ||
                    !std::isfinite(symbol.rotation_radians) || !std::isfinite(symbol.width_metres) ||
                    !std::isfinite(symbol.depth_metres) || symbol.width_metres <= 0 || symbol.depth_metres <= 0)
                    throw std::invalid_argument("invalid symbol presentation");
                include(symbol.position);
                QTransform transform;
                transform.rotate(symbol.rotation_radians * 180 / pi);
                transform.scale((symbol.flip_horizontal ? -1.0 : 1.0) *
                                    symbol.width_metres / symbol.footprint_view_box.width(),
                                (symbol.flip_vertical ? 1.0 : -1.0) *
                                    symbol.depth_metres / symbol.footprint_view_box.height());
                transform.translate(-symbol.footprint_view_box.center().x(),
                                    -symbol.footprint_view_box.center().y());
                const auto footprint = transform.mapRect(symbol.view_box);
                if (!finite_rect(footprint)) throw std::invalid_argument("overflowing symbol footprint");
                include({symbol.position.x + footprint.left(), symbol.position.y + footprint.top()});
                include({symbol.position.x + footprint.right(), symbol.position.y + footprint.bottom()});
            }
        }
        for (const auto& label : m_labels) {
            if (label.text.isEmpty()) continue;
            if (!std::isfinite(label.rotation_radians) || !std::isfinite(label.scale) ||
                !std::isfinite(label.text_height_metres) || !std::isfinite(label.paper_height_mm))
                throw std::invalid_argument("nonfinite label presentation");
            if (label.paper_height_mm * recording.pixels_per_mm > 10'000)
                return reject(tr("The Sketch font size exceeds the supported output bounds."));
            const auto model_font_pixels = (label.text_height_metres > 0 ? label.text_height_metres : .15) *
                (label.scale > 0 ? label.scale : 1) * model_scale;
            if (!std::isfinite(model_font_pixels) || model_font_pixels > 10'000)
                return reject(tr("The Sketch model font size exceeds the supported output bounds."));
            include(label.position);
            if (label.leader_start) include(*label.leader_start);
            if (label.plan_label_offset) include(label.position + *label.plan_label_offset);
            if (label.automatic_linear_placement) {
                const auto& placement = *label.automatic_linear_placement;
                include_boundary({placement.anchor});
                if (!std::isfinite(placement.outward_normal.x) || !std::isfinite(placement.outward_normal.y) ||
                    !std::isfinite(placement.clearance_metres))
                    throw std::invalid_argument("nonfinite label placement");
            }
        }
    } catch (const std::exception&) {
        return reject(tr("The committed Sketch contains invalid or unrepresentable drawing geometry."));
    }
    if (!has_content) return reject(tr("The Sketch has no committed drawing content to export."));
    recording.model_center = {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y)};
    const auto bounded_point = [&](Vec2 point) {
        const auto x = (point.x - recording.model_center.x) * model_scale;
        const auto y = (point.y - recording.model_center.y) * model_scale;
        return std::isfinite(x) && std::isfinite(y) &&
               std::abs(x) <= maximum_coordinate / 2 && std::abs(y) <= maximum_coordinate / 2;
    };
    if (!bounded_point(minimum) || !bounded_point(maximum))
        return reject(tr("The Sketch drawing exceeds the supported output bounds."));

    // Include placed font geometry in preflight; the recorder below remains
    // authoritative for the final crop (including SVG strokes and callouts).
    auto output_font = font();
    output_font.setFeature("calt", 0); output_font.setFeature("case", 0);
    for (const auto& label : positionedLabels(font(), &recording.picture, model_scale,
        recording.picture.logicalDpiY(), true, true, recording.model_center)) {
        if (!drawable_label(label)) continue;
        const auto layout = label_layout(label, output_font, &recording.picture,
                                         model_scale, recording.picture.logicalDpiY());
        const auto point = label.position - recording.model_center;
        const auto rect = label_transform(label, {point.x * model_scale, -point.y * model_scale})
            .mapRect(layout.bounds);
        if (!finite_rect(rect) || std::max({std::abs(rect.left()), std::abs(rect.right()),
                std::abs(rect.top()), std::abs(rect.bottom())}) > maximum_coordinate)
            return reject(tr("The Sketch annotations exceed the supported output bounds."));
    }
    SketchContentBoundsDevice device(recording.picture);
    QPainter painter;
    if (!painter.begin(&device)) return reject(tr("The Sketch vector recorder could not start."));
    painter.setFont(font());
    // A tiny positive viewport supplies an origin only. Content-only mode
    // never paints it or clips to it, so it cannot extend the recorded crop.
    renderSceneWithTransform(painter, QRectF(-.5, -.5, 1, 1), false, Qt::white,
        model_scale, recording.model_center, recording.pixels_per_mm, true);
    if (!painter.end()) return reject(tr("The Sketch vector recording could not finish."));
    if (!device.valid()) return reject(tr("The painted Sketch contains unrepresentable drawing geometry."));
    // QPicture reconstructs text runs during replay. Measure that exact
    // finished command stream at its output DPI, including fallback/italic
    // glyph positions, rather than assuming the initial shaped run's bounds
    // equal the PDF replay. Keep the original picture as output authority.
    QPicture measurement_sink;
    SketchContentBoundsDevice replay_device(measurement_sink);
    QPainter replay;
    if (!replay.begin(&replay_device))
        return reject(tr("The Sketch output bounds recorder could not start."));
    const bool replayed = recording.picture.play(&replay);
    const bool finished = replay.end();
    if (!replayed || !finished || !replay_device.valid())
        return reject(tr("The Sketch output bounds could not be measured."));
    const auto ink_bounds = replay_device.inkBounds();
    if (!ink_bounds || ink_bounds->isEmpty())
        return reject(tr("The Sketch has no painted committed drawing content to export."));
    recording.ink_bounds = *ink_bounds;
    if (!finite_rect(recording.ink_bounds) || std::max({std::abs(recording.ink_bounds.left()),
            std::abs(recording.ink_bounds.right()), std::abs(recording.ink_bounds.top()),
            std::abs(recording.ink_bounds.bottom()), recording.ink_bounds.width(),
            recording.ink_bounds.height()}) > maximum_coordinate)
        return reject(tr("The painted Sketch exceeds the supported output bounds."));
    recording.picture.setBoundingRect(recording.ink_bounds.toAlignedRect());
    return recording;
}

void PlanCanvas::setSketchCompositionGuideEnabled(bool enabled) {
    if (m_sketch_composition_guide_enabled == enabled) return;
    m_sketch_composition_guide_enabled = enabled;
    update();
}

std::optional<QRectF> PlanCanvas::sketchCompositionGuideRect() const {
    if (!m_sketch_composition_guide_enabled) return std::nullopt;
    if (m_sketch_guide_revision != m_sketch_content_revision || m_sketch_guide_font != font()) {
        try {
            m_sketch_guide_recording = recordSketchContent();
        } catch (const SvgAdmissionError& error) {
            // The composition frame is optional screen presentation. Unsafe
            // or unsupported artwork must still fail every explicit export.
            m_sketch_guide_recording.reset();
            qWarning("Sketch composition guide unavailable: %s", error.what());
        }
        m_sketch_guide_revision = m_sketch_content_revision;
        m_sketch_guide_font = font();
    }
    if (!m_sketch_guide_recording) return std::nullopt;
    const auto& recording = *m_sketch_guide_recording;
    const auto padding = sketch_content_padding_mm * recording.pixels_per_mm;
    const auto bounds = recording.ink_bounds.adjusted(-padding, -padding, padding, padding);
    const auto to_model = [&](QPointF point) {
        return Vec2{recording.model_center.x + point.x() / recording.model_scale,
                    recording.model_center.y - point.y() / recording.model_scale};
    };
    return QRectF(toScreen(to_model(bounds.topLeft()), rect()),
                  toScreen(to_model(bounds.bottomRight()), rect())).normalized();
}

Vec2 PlanCanvas::contentCenter() const noexcept {
    const auto bounds = contentBounds();
    return bounds ? Vec2{std::midpoint(bounds->first.x, bounds->second.x),
                         std::midpoint(bounds->first.y, bounds->second.y)} : m_view_center;
}

void PlanCanvas::renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                          bool fit_to_content, QColor background,
                                          std::optional<double> explicit_scale,
                                          std::optional<Vec2> explicit_center,
                                          std::optional<double> paper_pixels_per_mm,
                                          bool content_only, SceneLayer layer) const {
    if (viewport.width() <= 0.0 || viewport.height() <= 0.0) {
        return;
    }
    auto scale = explicit_scale.value_or(m_scale);
    auto view_center = explicit_center.value_or(m_view_center);
    // Public renderers use the committed lane, including the current viewport
    // overload. Only paintEvent opts into previews and interaction overlays.
    const bool output = layer == SceneLayer::committed ||
                        fit_to_content || explicit_scale.has_value();
    const bool floor_ghost = layer == SceneLayer::floor_ghost;
    const bool interactive = !output && !floor_ghost;
    const bool reference_preview = !output && layer == SceneLayer::screen_with_floor_ghost;
    if (floor_ghost) view_center = view_center - m_floor_ghost_offset;
    if (!explicit_scale.has_value() && fit_to_content) {
        if (const auto bounds = contentBounds()) {
            const auto minimum = bounds->first;
            const auto maximum = bounds->second;
            const auto width = std::max(maximum.x - minimum.x, 0.1);
            const auto height = std::max(maximum.y - minimum.y, 0.1);
            const auto padding = std::max(width, height) * 0.12 + 0.25;
            view_center = {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y)};
            scale = std::min(viewport.width() / (width + padding * 2.0),
                             viewport.height() / (height + padding * 2.0));
            scale = std::clamp(scale, output_minimum_scale, maximum_scale);
        }
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!content_only && !floor_ghost) painter.fillRect(viewport, background);
    const auto canvas_transform = painter.worldTransform();
    const auto canvas_device_transform = painter.deviceTransform();
    painter.translate(viewport.center());
    painter.scale(scale, -scale);
    // Tight output subtracts the origin from primitive coordinates before
    // scaling, avoiding cancellation from large survey/world coordinates.
    if (!content_only) painter.translate(-view_center.x, -view_center.y);

    for (const auto& reference : m_references) {
        if (content_only || floor_ghost) break;
        if (!reference.visible) continue;
        const CanvasReference* exact_reference = nullptr;
        if (reference_preview && m_move_preview_exact && m_move_preview_valid)
            for (const auto& proposed : m_move_references_preview)
                if (proposed.id == reference.id) exact_reference = &proposed;
        if (reference_preview && m_transform_preview_exact && m_transform_preview_valid)
            for (const auto& proposed : m_transform_references_preview)
                if (proposed.id == reference.id) exact_reference = &proposed;
        if (exact_reference) {
            drawReference(painter, *exact_reference);
        } else if (reference_preview && m_move_preview_delta &&
            (!m_move_preview_exact || m_move_preview_valid) && m_move_ids.contains(reference.id)) {
            painter.save();
            painter.translate(m_move_preview_delta->x, m_move_preview_delta->y);
            drawReference(painter, reference);
            painter.restore();
        } else if (reference_preview && reference.selected && m_transform_frame_start && !m_transform_preview_exact &&
                   (m_left_gesture == LeftGesture::selection_resize ||
                    m_left_gesture == LeftGesture::selection_rotate)) {
            auto preview = reference;
            // Compose in model radians before converting once, so snapped
            // quarter turns use the same exact angle as committed painting.
            preview.rotation_degrees = (reference.rotation_degrees*pi/180 +
                m_transform_rotation_preview) * 180/pi;
            preview.scale *= m_transform_scale_preview;
            drawReference(painter, preview);
        } else {
            drawReference(painter, reference);
        }
    }

    if (layer == SceneLayer::screen_with_floor_ghost && !output && !content_only &&
        (!m_floor_ghost_entities.empty() || !m_floor_ghost_labels.empty())) {
        // Trace over raster underlays but beneath active ink and furniture.
        // Preserve the caller's transform/clip rather than resetting to device
        // coordinates. Only paintEvent opts into this nonprinting lane.
        painter.save();
        painter.setWorldTransform(canvas_transform);
        painter.setOpacity(painter.opacity() * m_floor_ghost_opacity);
        renderSceneWithTransform(painter, viewport, false, background,
                                 std::nullopt, std::nullopt, paper_pixels_per_mm,
                                 false, SceneLayer::floor_ghost);
        painter.restore();
    }

    if (m_grid_enabled && interactive) {
        drawGrid(painter, viewport, scale, view_center);
    }
    if (!content_only && !floor_ghost) drawReferenceGrids(painter);
    struct PaintedEntity {
        const CanvasEntity* entity;
        const EntityGeometry* geometry;
    };
    std::vector<PaintedEntity> painted_entities;
    const auto& scene_entities = floor_ghost ? m_floor_ghost_entities : m_entities;
    if (interactive) ensurePublishedEntityGeometry();
    const auto entity_device_transform = painter.deviceTransform();
    const auto device_viewport = canvas_device_transform.mapRect(viewport);
    const bool reject_offscreen = interactive && finite_rect(device_viewport) &&
        ordinary_axis_transform(canvas_device_transform) &&
        ordinary_axis_transform(entity_device_transform);
    // Cosmetic treatment can be in device pixels or scaled logical pixels
    // depending on the device. The larger envelope covers both, including DPR.
    const auto cosmetic_pixel_scale = std::max({1.0, std::abs(canvas_device_transform.m11()),
                                               std::abs(canvas_device_transform.m22())});
    const auto screen_pixels_per_mm = logicalDpiX()/25.4;
    const auto outside_viewport = [&](const CanvasEntity& entity, const EntityGeometry* geometry) {
        if (!reject_offscreen || !geometry || !geometry->bounds || entity.selected ||
            entity.svg_symbol || entity.dimension_end_ticks ||
            entity.type == QStringLiteral("dimension_line") ||
            entity.type == QStringLiteral("section_overlay")) return false;
        const auto bounds = entity_device_transform.mapRect(*geometry->bounds);
        const auto stroke = entity_stroke_width(entity, false, m_scale,
                                                screen_pixels_per_mm, screen_pixels_per_mm);
        // Round caps/joins extend at most half a pen width. Two logical pixels
        // additionally enclose raster antialiasing and the boundary comparison.
        const auto padding_x = 2.0*cosmetic_pixel_scale + .5*stroke.width *
            (stroke.cosmetic ? cosmetic_pixel_scale : std::abs(entity_device_transform.m11()));
        const auto padding_y = 2.0*cosmetic_pixel_scale + .5*stroke.width *
            (stroke.cosmetic ? cosmetic_pixel_scale : std::abs(entity_device_transform.m22()));
        if (!finite_rect(bounds) || !std::isfinite(stroke.width) || stroke.width < 0.0 ||
            !std::isfinite(padding_x) || !std::isfinite(padding_y)) return false;
        const auto padded = bounds.adjusted(-padding_x, -padding_y, padding_x, padding_y);
        if (!finite_rect(padded)) return false;
        return padded.right() < device_viewport.left() || padded.left() > device_viewport.right() ||
               padded.bottom() < device_viewport.top() || padded.top() > device_viewport.bottom();
    };
    // Transient owners can move into view from any committed location. Keep
    // their complete original traversal, including pending/rejected proposals.
    const auto visible_indices = interactive && !hasInteractivePresentation()
        ? visiblePublishedEntityIndices(entity_device_transform, canvas_device_transform,
                                        device_viewport)
        : std::nullopt;
    const auto paint_count = visible_indices ? visible_indices->size() : scene_entities.size();
    painted_entities.reserve(paint_count);
    for (std::size_t position = 0; position < paint_count; ++position) {
        const auto index = visible_indices ? (*visible_indices)[position] : position;
        painted_entities.push_back({&scene_entities[index],
            interactive ? &m_published_entity_geometry[index] : nullptr});
    }
    if (interactive && ((m_vertex_move_handle && m_boundary_vertex_preview_valid) ||
                    (m_opening_width_handle && m_opening_width_preview_valid) ||
                    m_move_preview_valid ||
                    m_transform_preview_valid)) {
        // Candidate model owners can enter a crop/depth slice while another
        // owner's corner is dragged. They are interactive projections only;
        // neither committed entities nor output bounds gain these entries.
        const auto layer = [](const CanvasEntity& entity) {
            if (entity.type==QStringLiteral("terrain_surface") || entity.type==QStringLiteral("measurement_boundary") ||
                entity.type==QStringLiteral("room_boundary") || entity.type==QStringLiteral("boundary") ||
                entity.type==QStringLiteral("slab") || entity.type==QStringLiteral("room")) return 0;
            if (entity.type==QStringLiteral("wall")) return 10;
            if (entity.type==QStringLiteral("opening") || entity.type==QStringLiteral("window")) return 20;
            if (entity.type==QStringLiteral("symbol") || entity.type==QStringLiteral("assembly_instance")) return 30;
            if (entity.type==QStringLiteral("dimension_line")) return 40;
            return 15;
        };
        const auto& proposals = m_transform_preview_valid ? m_transform_entities_preview :
            m_move_preview_exact ? m_move_entities_preview :
            m_opening_width_preview_valid ? m_opening_width_entities_preview : m_boundary_vertex_entities_preview;
        for (const auto& preview : proposals) {
            if (std::any_of(m_entities.begin(),m_entities.end(),
                [&](const auto& entity) { return same_entity_presentation(entity, preview); })) continue;
            const auto next=std::find_if(painted_entities.begin(),painted_entities.end(),
                [&](const auto& entry) { return layer(*entry.entity)>layer(preview); });
            painted_entities.insert(next,{&preview, nullptr});
        }
    }
    for (const auto& painted_entity : painted_entities) {
        const auto& entity=*painted_entity.entity;
        if (content_only) {
            auto local = entity;
            const auto localize = [&](Boundary& boundary) {
                for (auto& segment : boundary) {
                    segment.start = segment.start - view_center;
                    segment.end = segment.end - view_center;
                }
            };
            localize(local.segments);
            if (local.stroke_segments) localize(*local.stroke_segments);
            for (auto& hole : local.holes) localize(hole);
            if (local.svg_symbol) local.svg_symbol->position = local.svg_symbol->position - view_center;
            drawEntity(painter, local, true, background, paper_pixels_per_mm);
        } else if (floor_ghost) {
            drawEntity(painter, entity, false, background, paper_pixels_per_mm);
        } else if (!output && &interactiveEntity(entity) != &entity) {
            drawEntity(painter, interactiveEntity(entity), false, background, paper_pixels_per_mm);
        } else if (!output && !m_boundary_vertex_preview_requested &&
            m_vertex_move_handle && m_vertex_move_preview &&
            m_vertex_move_handle->entity_id == entity.id) {
            auto preview = entity;
            const auto source = m_vertex_move_handle->source_position;
            const auto target = *m_vertex_move_preview;
            const auto same = [](Vec2 left, Vec2 right) {
                return left.x == right.x && left.y == right.y;
            };
            for (auto& segment : preview.segments) {
                if (same(segment.start, source)) segment.start = target;
                if (same(segment.end, source)) segment.end = target;
            }
            for (auto& handle : preview.vertex_handles) {
                if (handle.id == m_vertex_move_handle->vertex_id) handle.position = target;
            }
            drawEntity(painter, preview, output, background, paper_pixels_per_mm);
        } else if (!output && m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(entity.id)) {
            painter.save();
            painter.translate(m_move_preview_delta->x, m_move_preview_delta->y);
            drawEntity(painter, entity, output, background, paper_pixels_per_mm,
                       painted_entity.geometry);
            painter.restore();
        } else if (!output && entity.selected && m_transform_frame_start && !m_transform_preview_exact &&
                   m_left_gesture == LeftGesture::selection_axis_resize) {
            painter.save();
            painter.translate(m_axis_anchor.x, m_axis_anchor.y);
            painter.rotate(m_axis_rotation * 180.0 / pi);
            painter.scale(m_axis_scale_x_preview, m_axis_scale_y_preview);
            painter.rotate(-m_axis_rotation * 180.0 / pi);
            painter.translate(-m_axis_anchor.x, -m_axis_anchor.y);
            drawEntity(painter, entity, output, background, paper_pixels_per_mm,
                       painted_entity.geometry);
            painter.restore();
        } else if (!output && entity.selected && m_transform_frame_start && !m_transform_preview_exact &&
                   (m_left_gesture == LeftGesture::selection_resize ||
                    m_left_gesture == LeftGesture::selection_rotate)) {
            const Vec2 center{view_center.x + (m_transform_center.x() - viewport.center().x()) / scale,
                              view_center.y - (m_transform_center.y() - viewport.center().y()) / scale};
            painter.save();
            painter.translate(center.x, center.y);
            painter.rotate(m_transform_rotation_preview * 180.0 / pi);
            painter.scale(m_transform_scale_preview, m_transform_scale_preview);
            painter.translate(-center.x, -center.y);
            drawEntity(painter, entity, output, background, paper_pixels_per_mm,
                       painted_entity.geometry);
            painter.restore();
        } else {
            // Resolve every exact and local preview branch before consulting
            // committed bounds. Culling affects presentation alone.
            if (outside_viewport(entity, painted_entity.geometry)) continue;
            drawEntity(painter, entity, output, background, paper_pixels_per_mm,
                       painted_entity.geometry);
        }
    }

    // Transient overlays belong to the interactive canvas only. Both fitted
    // and explicitly scaled output must contain document entities alone.
    if (interactive) {
        const auto& placement_preview = m_symbol_drag_active
            ? m_symbol_drag_preview : m_component_placement_preview;
        if (placement_preview) {
            painter.save();
            painter.setOpacity(painter.opacity() * 0.6);
            drawEntity(painter, *placement_preview, false, background,
                       paper_pixels_per_mm);
            painter.restore();
        }
        if (m_boundary_preview.size() >= 2) {
            QPen pen(QColor(255, 220, 126), 0.0, Qt::DashLine);
            painter.setPen(pen);
            QPainterPath path;
            path.moveTo(m_boundary_preview.front().x, m_boundary_preview.front().y);
            for (std::size_t index = 1; index < m_boundary_preview.size(); ++index) {
                path.lineTo(m_boundary_preview[index].x, m_boundary_preview[index].y);
            }
            painter.drawPath(path);
        }
        if (m_wall_preview.has_value()) {
            const auto& wall = *m_wall_preview;
            const auto dx = wall.end.x - wall.start.x;
            const auto dy = wall.end.y - wall.start.y;
            const auto length = std::hypot(dx, dy);
            if (std::isfinite(length) && length > 1e-9 &&
                std::isfinite(wall.thickness_metres) && wall.thickness_metres > 0.0) {
                const Vec2 normal{-dy * wall.thickness_metres / (2.0 * length),
                                   dx * wall.thickness_metres / (2.0 * length)};
                const std::array<Vec2, 4> corners{{
                    {wall.start.x + normal.x, wall.start.y + normal.y},
                    {wall.end.x + normal.x, wall.end.y + normal.y},
                    {wall.end.x - normal.x, wall.end.y - normal.y},
                    {wall.start.x - normal.x, wall.start.y - normal.y}}};
                QPainterPath footprint;
                footprint.moveTo(corners.front().x, corners.front().y);
                for (std::size_t index = 1; index < corners.size(); ++index)
                    footprint.lineTo(corners[index].x, corners[index].y);
                footprint.closeSubpath();
                QPen outline(QColor(37, 99, 235), 1.8,
                             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                outline.setCosmetic(true);
                painter.setPen(outline);
                painter.setBrush(QColor(37, 99, 235, 42));
                painter.drawPath(footprint);
                QPen centerline(QColor(37, 99, 235, 225), 0.0, Qt::DashLine);
                painter.setPen(centerline);
                painter.setBrush(Qt::NoBrush);
                painter.drawLine(QPointF(wall.start.x, wall.start.y),
                                 QPointF(wall.end.x, wall.end.y));
            }
        }
        if (m_wall_snap_enabled && m_last_mouse_position) {
            const auto snap = snapResult(*m_last_mouse_position);
            if (snap.kind != SnapKind::none) {
                const QColor color = snap.kind == SnapKind::endpoint ? QColor(57, 197, 132)
                    : snap.kind == SnapKind::perpendicular ? QColor(85, 167, 255)
                    : snap.kind == SnapKind::alignment ? QColor(111, 176, 245)
                    : snap.kind == SnapKind::on_boundary ? QColor(186, 117, 255)
                                                       : QColor(255, 196, 82);
                if (snap.guide) {
                    painter.setPen(QPen(color, 0.0, Qt::DashLine));
                    painter.setBrush(Qt::NoBrush);
                    drawSegment(painter, *snap.guide);
                }
                const auto radius = 7.0 / scale;
                painter.setPen(QPen(color, 0.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QPointF(snap.point.x, snap.point.y), radius, radius);
                painter.drawLine(QPointF(snap.point.x - radius, snap.point.y),
                                 QPointF(snap.point.x + radius, snap.point.y));
                painter.drawLine(QPointF(snap.point.x, snap.point.y - radius),
                                 QPointF(snap.point.x, snap.point.y + radius));
                QString cue;
                switch (snap.kind) {
                case SnapKind::endpoint: cue = QStringLiteral("Endpoint"); break;
                case SnapKind::on_wall: cue = QStringLiteral("Wall centerline"); break;
                case SnapKind::on_boundary: cue = QStringLiteral("On boundary"); break;
                case SnapKind::perpendicular: cue = QStringLiteral("Perpendicular"); break;
                case SnapKind::alignment: cue = QStringLiteral("Alignment"); break;
                case SnapKind::grid: cue = QStringLiteral("Grid"); break;
                case SnapKind::length: cue = QStringLiteral("Length %1").arg(
                    drawingLengthText(drawingLengthIncrementMetres(),m_metric_units)); break;
                case SnapKind::none: break;
                }
                if (!cue.isEmpty()) {
                    painter.save();
                    painter.resetTransform();
                    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
                    const auto cue_position = toScreen(snap.point, rect()) + QPointF(10.0, -10.0);
                    const auto text_bounds = QFontMetrics(painter.font()).boundingRect(cue);
                    const QRectF label_rect(cue_position.x() - 3.0, cue_position.y() - text_bounds.height() + 2.0,
                                            text_bounds.width() + 8.0, text_bounds.height() + 5.0);
                    const bool light_surface = m_canvas_background.lightnessF() > 0.5;
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(light_surface ? QColor(255, 255, 255, 230)
                                                   : QColor(17, 24, 39, 235));
                    painter.drawRoundedRect(label_rect, 3.0, 3.0);
                    painter.setPen(light_surface ? QColor(24, 37, 54) : QColor(237, 242, 251));
                    painter.drawText(QPointF(label_rect.left() + 4.0,
                                             label_rect.bottom() - 3.0), cue);
                    painter.restore();
                }
            }
        }
        if (m_boundary_draft_preview.has_value()) {
            const auto& draft = *m_boundary_draft_preview;
            QPen draft_pen(QColor(255, 220, 126, 185), 1.5,
                           Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            draft_pen.setCosmetic(true);
            painter.setPen(draft_pen);
            for (const auto& segment : draft.segments) {
                drawSegment(painter, segment);
            }

            if (draft.rubber_band.has_value()) {
                painter.setPen(QPen(QColor(255, 111, 173, 235), 0.0,
                                    Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
                drawSegment(painter, *draft.rubber_band);
            }

            const auto draw_marker = [&](std::optional<Vec2> point, QColor color) {
                if (!point.has_value()) return;
                const auto radius = 8.0 / scale;
                painter.setPen(QPen(color, 0.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QPointF(point->x, point->y), radius, radius);
                painter.drawLine(QPointF(point->x - radius, point->y),
                                 QPointF(point->x + radius, point->y));
                painter.drawLine(QPointF(point->x, point->y - radius),
                                 QPointF(point->x, point->y + radius));
            };
            draw_marker(draft.anchor, QColor(114, 222, 164));
            draw_marker(draft.pen_position, QColor(103, 202, 255));
        }
    }
    painter.restore();

    if (interactive && m_wall_preview && !m_wall_preview->dimension_text.isEmpty()) {
        const auto center = QPointF(
            viewport.center().x() + ((m_wall_preview->start.x + m_wall_preview->end.x) * 0.5 -
                                     view_center.x) * scale,
            viewport.center().y() - ((m_wall_preview->start.y + m_wall_preview->end.y) * 0.5 -
                                     view_center.y) * scale);
        painter.save();
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QFontMetricsF metrics(painter.font());
        auto bounds = metrics.boundingRect(m_wall_preview->dimension_text);
        bounds.moveCenter(center);
        bounds.adjust(-6.0, -3.0, 6.0, 3.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background.lightnessF() > 0.5
                             ? QColor(255, 255, 255, 238)
                             : QColor(20, 25, 34, 225));
        painter.drawRoundedRect(bounds, 3.0, 3.0);
        painter.setPen(background.lightnessF() > 0.5
                           ? QColor(50, 65, 84) : QColor(255, 239, 172));
        painter.setBrush(Qt::NoBrush);
        painter.drawText(bounds, Qt::AlignCenter, m_wall_preview->dimension_text);
        painter.restore();
    }

    if (!content_only && !floor_ghost)
        drawReferenceGridLabels(painter, viewport, scale, view_center, output, background,
                                paper_pixels_per_mm);

    // Committed labels use the same model-to-screen mapping as the current
    // scene, including fit-to-content output. Drawing after restoring the
    // world transform keeps text upright and readable at its device scale.
    std::vector<QRectF> annotation_footprints;
    drawLabels(painter, viewport, scale, view_center, output, background,
               paper_pixels_per_mm, interactive ? &annotation_footprints : nullptr,
               content_only, floor_ghost);
    if (floor_ghost) return;

    if (!output) {
        if (m_grid_enabled) {
            painter.save();
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            const auto spacing = grid_spacing(scale, m_metric_units).minor;
            const auto label = grid_spacing_label(spacing, m_metric_units);
            const QFontMetricsF metrics(painter.font());
            const auto length = spacing * scale;
            const auto width = std::max(length, metrics.horizontalAdvance(label));
            const auto left = viewport.left() + 12.0;
            const auto bottom = viewport.bottom() - 12.0;
            const QRectF backing(left - 6.0, bottom - metrics.height() - 15.0,
                                 width + 12.0, metrics.height() + 21.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background.lightnessF() > 0.5
                                 ? QColor(255, 255, 255, 230) : QColor(20, 25, 34, 230));
            painter.drawRoundedRect(backing, 3.0, 3.0);
            painter.setPen(background.lightnessF() > 0.5
                               ? QColor(86, 102, 124) : QColor(190, 201, 219));
            painter.drawText(QPointF(left, bottom - 10.0), label);
            painter.drawLine(QLineF(left, bottom, left + length, bottom));
            for (const auto x : {left, left + length})
                painter.drawLine(QLineF(x, bottom - 3.0, x, bottom + 3.0));
            painter.restore();
        }
        if (m_selection_controls_visible) {
            drawSelectionFrame(painter, viewport, annotation_footprints);
            drawVertexHandles(painter, viewport);
            drawSelectionDimensions(painter, viewport, annotation_footprints);
            drawOpeningWidthHandles(painter, viewport);
            drawSelectionCaption(painter, viewport, background);
        }
        drawCursorReadout(painter, viewport, background);
    }

    if (!output) {
        QString instruction;
        if (m_boundary_draft_preview.has_value() &&
            !m_boundary_draft_preview->instruction.isEmpty()) {
            instruction = m_boundary_draft_preview->instruction;
        } else if (m_tool != CanvasTool::select) {
            if (m_tool == CanvasTool::boundary) {
                instruction = QStringLiteral("Boundary tool  •  click points, Enter closes, D precise segment");
            } else if (m_tool == CanvasTool::sloped_wall) {
                instruction = QStringLiteral("Sloped wall tool  •  click two points, then enter the signed rise");
            } else {
                instruction = QStringLiteral("Wall tool  •  click two points");
            }
        }
        if (!instruction.isEmpty()) {
            painter.save();
            painter.setPen(background.lightnessF() > 0.5
                               ? QColor(86, 102, 124)
                               : QColor(190, 201, 219));
            painter.drawText(viewport.adjusted(12.0, 10.0, -12.0, -10.0),
                             Qt::AlignTop | Qt::AlignLeft, instruction);
            painter.restore();
        }
    }
    if (!output && m_boundary_draft_preview.has_value()) {
        const auto& draft = *m_boundary_draft_preview;
        const auto to_screen = [&](Vec2 point) {
            return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                           viewport.center().y() - (point.y - view_center.y) * scale);
        };
        painter.save();
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QFontMetricsF metrics(painter.font());
        for (const auto& label : draft.labels) {
            if (label.text.isEmpty()) continue;
            const auto center = to_screen(label.position);
            painter.save();
            painter.translate(center);
            painter.rotate(-label.rotation_radians * 180.0 / std::numbers::pi);
            auto bounds = metrics.boundingRect(label.text);
            bounds.moveCenter(QPointF{});
            bounds.adjust(-5.0, -3.0, 5.0, 3.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background.lightnessF() > 0.5
                                 ? QColor(255, 255, 255, 238)
                                 : QColor(20, 25, 34, 225));
            painter.drawRoundedRect(bounds, 3.0, 3.0);
            painter.setPen(background.lightnessF() > 0.5
                               ? QColor(50, 65, 84)
                               : QColor(255, 239, 172));
            painter.setBrush(Qt::NoBrush);
            painter.drawText(bounds, Qt::AlignCenter, label.text);
            painter.restore();
        }
        painter.restore();
    }
    if (!output) {
        drawDrawingWitnesses(painter, viewport, scale, view_center, background);
        drawOverviewMap(painter);
    }
}

void PlanCanvas::drawOverviewMap(QPainter& painter) const {
    const auto map = overviewMapRect();
    if (map.isEmpty()) return;
    const auto bounds = contentBounds(true);
    const auto light = m_canvas_background.lightnessF() > 0.5;
    painter.save();
    painter.setPen(QPen(light ? QColor(128, 147, 173) : QColor(120, 143, 174), 1.0));
    painter.setBrush(light ? QColor(255, 255, 255, 242) : QColor(17, 25, 38, 242));
    painter.drawRoundedRect(map, 8.0, 8.0);
    // An opaque navigation body keeps the drawing legible and permits owned
    // committed geometry to be replayed without translucent precomposition.
    const QColor body_color = light ? QColor(255, 255, 255) : QColor(17, 25, 38);
    painter.fillRect(map.adjusted(8.0, 22.0, -8.0, -8.0), body_color);
    painter.setPen(light ? QColor(50, 65, 84) : QColor(220, 232, 246));
    QFont map_font = painter.font();
    map_font.setPixelSize(10);
    painter.setFont(map_font);
    painter.drawText(map.adjusted(8.0, 3.0, -8.0, -3.0), Qt::AlignTop | Qt::AlignLeft,
                     tr("Overview · click or drag to pan"));
    if (!bounds) {
        painter.drawText(map.adjusted(8.0, 22.0, -8.0, -8.0), Qt::AlignCenter,
                         tr("No drawing yet"));
        painter.restore();
        return;
    }
    painter.restore();

    const auto inner = map.adjusted(8.0, 22.0, -8.0, -8.0);
    if (inner.width() <= 0.0 || inner.height() <= 0.0) return;
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto span_x = std::max(maximum.x - minimum.x, 0.1);
    const auto span_y = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(span_x, span_y) * 0.08 + 0.1;
    const auto world_min = Vec2{minimum.x - padding, minimum.y - padding};
    const auto world_max = Vec2{maximum.x + padding, maximum.y + padding};
    const auto world_span_x = std::max(world_max.x - world_min.x, 0.1);
    const auto world_span_y = std::max(world_max.y - world_min.y, 0.1);
    const auto map_scale = std::min(inner.width() / world_span_x,
                                    inner.height() / world_span_y);
    const auto world_center = Vec2{(world_min.x + world_max.x) * 0.5,
                                   (world_min.y + world_max.y) * 0.5};
    const auto to_map = [&](Vec2 point) {
        return QPointF(inner.center().x() + (point.x - world_center.x) * map_scale,
                       inner.center().y() - (point.y - world_center.y) * map_scale);
    };
    const auto dpr = devicePixelRatioF();
    const auto device_transform = painter.deviceTransform();
    // Child-widget backing stores may add device-pixel offsets even while the
    // logical transform is identity. Integral offsets preserve raster phase;
    // fractional offsets, other scales and unsafe states paint directly. Check
    // logical transforms separately: combinedTransform includes the device DPR.
    const bool cache_safe = painter.isActive() && painter.device() == this &&
        painter.worldTransform().isIdentity() && !painter.viewTransformEnabled() && !painter.hasClipping() &&
        painter.opacity() == 1.0 && painter.compositionMode() == QPainter::CompositionMode_SourceOver &&
        std::isfinite(dpr) && dpr > 0.0 && dpr <= 8.0 &&
        device_transform.m11() == dpr && device_transform.m22() == dpr &&
        device_transform.m12() == 0.0 && device_transform.m21() == 0.0 &&
        device_transform.m13() == 0.0 && device_transform.m23() == 0.0 &&
        device_transform.m33() == 1.0 &&
        std::isfinite(device_transform.dx()) && std::isfinite(device_transform.dy()) &&
        std::abs(device_transform.dx()) <= 1048576.0 &&
        std::abs(device_transform.dy()) <= 1048576.0 &&
        std::trunc(device_transform.dx()) == device_transform.dx() &&
        std::trunc(device_transform.dy()) == device_transform.dy() &&
        std::isfinite(map_scale) && map_scale > 0.0 &&
        std::isfinite(world_center.x) && std::isfinite(world_center.y);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setClipRect(inner, Qt::IntersectClip);
    const auto draw_segment = [&](QPainter& target, const Segment& segment) {
        if (!std::isfinite(segment.start.x) || !std::isfinite(segment.start.y) ||
            !std::isfinite(segment.end.x) || !std::isfinite(segment.end.y)) return;
        if (segment.sweep_radians == 0.0) {
            target.drawLine(to_map(segment.start), to_map(segment.end));
        } else if (const auto arc = arc_info(segment)) {
            QPainterPath path;
            path.moveTo(to_map(segment.start));
            constexpr int samples = 32;
            for (int index = 1; index <= samples; ++index)
                path.lineTo(to_map(arc_point(segment, *arc,
                                             static_cast<double>(index) / samples)));
            target.drawPath(path);
        }
    };
    const auto draw_boundary = [&](QPainter& target, const Boundary& boundary) {
        for (const auto& segment : boundary) draw_segment(target, segment);
    };
    const auto draw_committed = [&](QPainter& target) {
        for (const auto& entity : m_entities) {
            auto pen_color = color_for(entity, light);
            pen_color.setAlpha(light ? 235 : 220);
            QPen pen(pen_color, entity.selected ? 2.0 : 1.0,
                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            pen.setCosmetic(true);
            target.setPen(pen);
            draw_boundary(target, entity.stroke_segments ? *entity.stroke_segments : entity.segments);
            for (const auto& hole : entity.holes) draw_boundary(target, hole);
        }
    };
    bool reused_geometry = false;
    if (cache_safe) {
        const auto left = std::floor(inner.left() * dpr);
        const auto top = std::floor(inner.top() * dpr);
        const auto pixel_width = std::ceil(inner.right() * dpr) - left;
        const auto pixel_height = std::ceil(inner.bottom() * dpr) - top;
        // Bound allocation before narrowing or constructing the image.
        if (std::isfinite(left) && std::isfinite(top) &&
            std::isfinite(pixel_width) && std::isfinite(pixel_height) &&
            pixel_width > 0.0 && pixel_height > 0.0 &&
            pixel_width <= 4096.0 && pixel_height <= 4096.0 &&
            pixel_width * pixel_height <= 4.0 * 1024.0 * 1024.0) {
            // Under device mapping DPR*p + integral offset, this logical
            // origin still lands on an integer device pixel. The offset does
            // not change image-local phase or the cache's reusable contents.
            const QPointF origin(left / dpr, top / dpr);
            QByteArray key;
            QDataStream signature(&key, QIODevice::WriteOnly);
            signature << inner << world_center.x << world_center.y << map_scale
                      << dpr << body_color << font() << qint32(logicalDpiX())
                      << qint32(logicalDpiY()) << qint32(physicalDpiX())
                      << qint32(physicalDpiY()) << quint32(painter.renderHints());
            if (m_overview_geometry_cache.key != key || m_overview_geometry_cache.image.isNull()) {
                QImage image(static_cast<int>(pixel_width), static_cast<int>(pixel_height),
                             QImage::Format_ARGB32_Premultiplied);
                if (!image.isNull()) {
                    image.setDevicePixelRatio(dpr);
                    image.fill(Qt::transparent);
                    QPainter layer(&image);
                    layer.setRenderHints(painter.renderHints());
                    layer.translate(-origin);
                    layer.setClipRect(inner, Qt::IntersectClip);
                    layer.fillRect(inner, body_color);
                    layer.setBrush(Qt::NoBrush);
                    draw_committed(layer);
                    layer.end();
                    m_overview_geometry_cache = {std::move(key), std::move(image), origin};
                } else {
                    m_overview_geometry_cache = {};
                }
            }
            if (!m_overview_geometry_cache.image.isNull()) {
                painter.save();
                painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                painter.drawImage(m_overview_geometry_cache.origin, m_overview_geometry_cache.image);
                painter.restore();
                reused_geometry = true;
            }
        }
    }
    if (!reused_geometry) draw_committed(painter);
    const auto draft_pen = [&](QColor color, Qt::PenStyle style = Qt::SolidLine) {
        QPen pen(color, 1.5, style, Qt::RoundCap, Qt::RoundJoin);
        pen.setCosmetic(true);
        painter.setPen(pen);
    };
    draft_pen(QColor(255, 220, 126, 185));
    for (std::size_t index = 1; index < m_boundary_preview.size(); ++index)
        draw_segment(painter, {m_boundary_preview[index-1], m_boundary_preview[index], 0});
    if (m_boundary_draft_preview) {
        const auto& draft = *m_boundary_draft_preview;
        draw_boundary(painter, draft.segments);
        if (draft.rubber_band) {
            draft_pen(QColor(255, 111, 173, 235), Qt::DashLine);
            draw_segment(painter, *draft.rubber_band);
        }
        const auto draw_marker = [&](std::optional<Vec2> point, QColor color) {
            if (!point || !std::isfinite(point->x) || !std::isfinite(point->y)) return;
            draft_pen(color);
            painter.drawEllipse(to_map(*point), 2.5, 2.5);
        };
        draw_marker(draft.anchor, QColor(114, 222, 164));
        draw_marker(draft.pen_position, QColor(103, 202, 255));
    }
    if (m_wall_preview) {
        draft_pen(QColor(37, 99, 235, 225), Qt::DashLine);
        draw_segment(painter, {m_wall_preview->start, m_wall_preview->end, 0});
    }
    const auto visible_width = width() / std::max(m_scale, minimum_scale);
    const auto visible_height = height() / std::max(m_scale, minimum_scale);
    const auto top_left = to_map({m_view_center.x - visible_width * 0.5,
                                  m_view_center.y + visible_height * 0.5});
    const auto bottom_right = to_map({m_view_center.x + visible_width * 0.5,
                                      m_view_center.y - visible_height * 0.5});
    auto viewport_rect = QRectF(top_left, bottom_right).normalized();
    QPen viewport_pen(light ? QColor(32, 139, 220, 235) : QColor(103, 202, 255, 245), 1.5,
                      Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
    viewport_pen.setCosmetic(true);
    painter.setPen(viewport_pen);
    painter.setBrush(QColor(32, 139, 220, 28));
    // Keep an enclosing viewport visible instead of clipping all four edges
    // away when the full drawing is already in view.
    painter.drawRect(viewport_rect.intersected(inner.adjusted(1.0, 1.0, -1.0, -1.0)));
    painter.restore();

    painter.save();
    painter.setPen(QPen(light ? QColor(128, 147, 173, 220) : QColor(120, 143, 174, 220), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(map, 8.0, 8.0);
    painter.restore();
}

bool PlanCanvas::navigateOverviewMap(QPointF position) {
    const auto map = overviewMapRect();
    if (map.isEmpty() || !map.contains(position)) return false;
    const auto bounds = contentBounds(true);
    if (!bounds) return true;
    const auto inner = map.adjusted(8.0, 22.0, -8.0, -8.0);
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto span_x = std::max(maximum.x - minimum.x, 0.1);
    const auto span_y = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(span_x, span_y) * 0.08 + 0.1;
    const auto world_min = Vec2{minimum.x - padding, minimum.y - padding};
    const auto world_max = Vec2{maximum.x + padding, maximum.y + padding};
    const auto world_span_x = std::max(world_max.x - world_min.x, 0.1);
    const auto world_span_y = std::max(world_max.y - world_min.y, 0.1);
    const auto map_scale = std::min(inner.width() / world_span_x,
                                    inner.height() / world_span_y);
    if (!(map_scale > 0.0) || !std::isfinite(map_scale)) return true;
    const auto world_center = Vec2{(world_min.x + world_max.x) * 0.5,
                                   (world_min.y + world_max.y) * 0.5};
    const auto previous_center = m_view_center;
    const auto previous_scale = m_scale;
    m_view_center = {world_center.x + (position.x() - inner.center().x()) / map_scale,
                     world_center.y - (position.y() - inner.center().y()) / map_scale};
    update();
    notifyNavigationChanged(previous_center, previous_scale);
    return true;
}

void PlanCanvas::setPointClicked(std::function<void(Vec2)> callback) {
    m_point_clicked = std::move(callback);
}

void PlanCanvas::setPointPlacementRequested(std::function<void(Vec2)> callback) {
    resetGesture();
    m_point_placement_requested = std::move(callback);
    update();
}

void PlanCanvas::setEntityClicked(std::function<void(QString)> callback) {
    m_entity_clicked = std::move(callback);
}

void PlanCanvas::setEntityDoubleClicked(std::function<void(QString)> callback) {
    m_entity_double_clicked = std::move(callback);
}

void PlanCanvas::setEntitySelectionClicked(std::function<void(QString, bool)> callback) {
    m_entity_selection_clicked = std::move(callback);
}

void PlanCanvas::setOverlapSelectionRequested(std::function<bool(bool, QStringList)> callback) {
    m_overlap_selection_requested = std::move(callback);
}

void PlanCanvas::setEntitiesSelected(std::function<void(QStringList, bool)> callback) {
    m_entities_selected = std::move(callback);
}

void PlanCanvas::setEntitiesMoveRequested(std::function<bool(QStringList, Vec2)> callback) {
    m_entities_move_requested = std::move(callback);
}

void PlanCanvas::setEntitiesMoveStarted(std::function<void(QStringList)> callback) {
    resetGesture();
    m_entities_move_started = std::move(callback);
}

void PlanCanvas::setEntitiesMoveRejected(std::function<void(QStringList, Vec2)> callback) {
    resetGesture();
    m_entities_move_rejected = std::move(callback);
}

void PlanCanvas::setEntitiesMovePreviewRequested(
    std::function<std::optional<std::vector<CanvasEntity>>(QStringList, Vec2, std::uint64_t)> callback) {
    resetGesture();
    m_entities_move_preview_requested = std::move(callback);
}

void PlanCanvas::captureOpeningMove(QPointF point) {
    if (m_move_ids.size() != 1) return;
    ensureRetainedSelection();
    for (const auto index : m_selected_entity_indices) {
        const auto& entity = m_entities[index];
        if (entity.type == QStringLiteral("opening") || entity.type == QStringLiteral("window") ||
            entity.type == QStringLiteral("door") || entity.type == QStringLiteral("doorway") ||
            entity.opening_width_controls) {
            m_opening_move_active = true;
            break;
        }
    }
    if (!m_opening_move_active) return;
    const auto* entity = selectedOpening();
    if (!entity || !entity->opening_width_controls->host_baseline) return;
    const auto source = *entity->opening_width_controls;
    try {
        const auto& host = *source.host_baseline;
        const auto length = segment_length(host);
        if (!std::isfinite(length) || !std::isfinite(source.width_metres) ||
            source.width_metres <= 1e-6 || !std::isfinite(source.offset_metres) ||
            !std::isfinite(source.start_jamb.x) || !std::isfinite(source.start_jamb.y) ||
            !std::isfinite(source.end_jamb.x) || !std::isfinite(source.end_jamb.y)) return;
        // Share the document geometry's endpoint tolerance. Rigid/reflected
        // presentation can change the computed host length by a few ulps.
        (void)hosted_opening_span(host, source.offset_metres, source.width_metres);
        const auto middle_station = source.offset_metres + source.width_metres * .5;
        const auto midpoint = point_at_host_station(host, middle_station);
        const auto station = project_host_station(host, toModel(point, rect()), middle_station);
        if (!std::isfinite(station) || !std::isfinite(midpoint.x) || !std::isfinite(midpoint.y)) return;
        m_opening_move_source = source;
        m_opening_move_original_midpoint = midpoint;
        m_opening_move_press_station = station;
        m_opening_move_pointer_station = station;
    } catch (const std::exception&) {
        // The exact source is unavailable. Retain the semantic gesture so its
        // preview/release rejects rather than manufacturing a free translation.
    }
}

void PlanCanvas::updateEntitiesMovePreview(QPointF point, Qt::KeyboardModifiers modifiers) {
    m_move_preview_pointer = point;
    m_move_preview_fine = modifiers.testFlag(Qt::ShiftModifier) || !m_snap_enabled || m_raw_point_input;
    std::optional<Vec2> delta;
    std::optional<CanvasOpeningMoveIntent> opening_intent;
    if (!m_opening_move_active) {
        delta = dragDelta(point);
    } else if (m_opening_move_source && m_opening_move_press_station &&
               m_opening_move_pointer_station && m_opening_move_original_midpoint) {
        const auto& source = *m_opening_move_source;
        const auto& host = *source.host_baseline;
        try {
            // Arc projection unwraps around the previous pointer station, not
            // around the clamped opening, so crossing atan2's cut stays smooth.
            const auto station = project_host_station(host, toModel(point, rect()),
                *m_opening_move_pointer_station);
            if (std::isfinite(station)) {
                m_opening_move_pointer_station = station;
                const auto displacement = station - *m_opening_move_press_station;
                const auto maximum_offset = std::max(0.0, segment_length(host) - source.width_metres);
                auto offset = source.offset_metres;
                if (std::abs(displacement) > 1e-12) {
                    offset += displacement;
                    if (!std::isfinite(offset)) throw std::invalid_argument("Invalid opening station");
                    // Saturated endpoints stay reachable even when the host's
                    // available run is not an exact construction increment.
                    if (!m_move_preview_fine && offset > 0.0 && offset < maximum_offset) {
                        const auto step = drawingLengthIncrementMetres();
                        if (!std::isfinite(step) || step <= 0.0)
                            throw std::invalid_argument("Invalid opening station increment");
                        offset = std::round(offset / step) * step;
                    }
                    offset = std::clamp(offset, 0.0, maximum_offset);
                }
                const auto midpoint = point_at_host_station(host, offset + source.width_metres * .5);
                const auto proposed = midpoint - *m_opening_move_original_midpoint;
                if (std::isfinite(offset) && std::isfinite(proposed.x) && std::isfinite(proposed.y)) {
                    delta = proposed;
                    opening_intent = CanvasOpeningMoveIntent{m_move_ids.front(), offset, source.source_revision};
                }
            }
        } catch (const std::exception&) {
            // A centre hit on an arc or an invalid analytical source invalidates
            // the current proposal, including any older pending admission.
        }
    }
    // Preserve an admitted/pending serial for an unchanged constrained station.
    // This also avoids re-preparing when the pointer pushes beyond a host end.
    if (m_opening_move_active && opening_intent && m_opening_move_preview_intent &&
        delta && m_move_preview_delta &&
        opening_intent->offset_metres == m_opening_move_preview_intent->offset_metres &&
        delta->x == m_move_preview_delta->x && delta->y == m_move_preview_delta->y) return;
    m_move_preview_delta = delta;
    m_opening_move_preview_intent = opening_intent;
    const auto serial = ++m_move_preview_serial;
    m_move_entities_preview.clear();
    m_move_entities_preview_index.clear();
    m_move_labels_preview.clear(); m_move_references_preview.clear();
    m_move_preview_pending = false;
    m_move_preview_exact = m_opening_move_active;
    m_move_preview_valid = false;
    m_move_preview_request_in_progress = false;
    if (!delta || (m_opening_move_active && !m_entities_move_preview_requested)) {
        setCursor(Qt::ForbiddenCursor);
        return;
    }
    if (m_entities_move_preview_requested) {
        // A configured exact provider owns admission, including an unavailable
        // result. Hosted openings always require this semantic provider.
        m_move_preview_exact = true;
        m_move_preview_request_in_progress = true;
        std::optional<std::vector<CanvasEntity>> proposed;
        bool provider_failed = false;
        const QPointer<PlanCanvas> guard(this);
        try {
            proposed = m_entities_move_preview_requested(m_move_ids, *delta, serial);
        } catch (const std::exception&) { provider_failed = true; }
        if (!guard) return;
        if (serial != m_move_preview_serial || !m_move_preview_request_in_progress) return;
        m_move_preview_request_in_progress = false;
        if (provider_failed) m_move_preview_pending = false;
        if (!m_move_preview_pending)
            (void)applyEntitiesMovePreview(serial, std::move(proposed));
    }
}

bool PlanCanvas::markEntitiesMovePreviewPending(std::uint64_t serial) {
    if (serial != m_move_preview_serial || !m_move_preview_request_in_progress ||
        m_left_gesture != LeftGesture::object_move || !m_move_preview_delta) return false;
    m_move_preview_exact = true;
    m_move_preview_valid = false;
    m_move_preview_pending = true;
    m_move_entities_preview.clear();
    m_move_entities_preview_index.clear();
    m_move_labels_preview.clear(); m_move_references_preview.clear();
    return true;
}

bool PlanCanvas::completeEntitiesMovePreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels, std::vector<CanvasReference> references) {
    if (!m_move_preview_pending) return false;
    return applyEntitiesMovePreview(serial, std::move(result), std::move(labels), std::move(references));
}

bool PlanCanvas::applyEntitiesMovePreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels, std::vector<CanvasReference> references) {
    if (serial != m_move_preview_serial || m_left_gesture != LeftGesture::object_move ||
        !m_move_preview_delta) return false;
    // Completion inside a synchronous callback owns admission over its return.
    m_move_preview_request_in_progress = false;
    m_move_preview_pending = false;
    m_move_preview_exact = true;
    m_move_entities_preview_index.clear();
    m_move_entities_preview = result ? std::move(*result) : std::vector<CanvasEntity>{};
    m_move_labels_preview = std::move(labels);
    m_move_references_preview = std::move(references);
    if (!m_move_references_preview.empty()) {
        QSet<QString> selected_references;
        for (const auto& retained : m_references)
            if (retained.selected) selected_references.insert(retained.id);
        for (auto& proposed : m_move_references_preview)
            proposed.selected = selected_references.contains(proposed.id);
    }
    QSet<QString> admitted_ids;
    for (const auto& entity : m_move_entities_preview) admitted_ids.insert(entity.id);
    for (const auto& label : m_move_labels_preview) admitted_ids.insert(label.id);
    ensureRetainedSelection();
    if (m_has_selected_reference)
        for (const auto& reference : m_move_references_preview.empty() ? m_references : m_move_references_preview)
            if (reference.selected) admitted_ids.insert(reference.id);
    m_move_preview_valid = result && valid_reference_previews(m_move_references_preview, m_references) &&
        unambiguous_entity_presentations(m_move_entities_preview, m_entities) &&
        std::all_of(m_move_ids.begin(), m_move_ids.end(),
            [&](const auto& id) { return admitted_ids.contains(id); });
    if (!m_move_preview_valid) {
        m_move_entities_preview.clear();
        m_move_entities_preview_index.clear();
        m_move_labels_preview.clear(); m_move_references_preview.clear();
    }
    // Preserve the retained root's selection without rescanning the entire
    // drawing for every dependent body in a compound move proposal.
    if (!m_move_entities_preview.empty()) {
        QHash<QString, bool> retained_selection;
        for (const auto& retained : m_entities) retained_selection.insert(retained.id, retained.selected);
        for (auto& proposed : m_move_entities_preview)
            if (const auto selected = retained_selection.constFind(proposed.id); selected != retained_selection.cend())
                proposed.selected = selected.value();
    }
    if (m_move_preview_valid)
        rebuildEntityPresentationIndex(m_move_entities_preview_index, m_move_entities_preview);
    setCursor(m_move_preview_valid ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
    update();
    if (m_move_release_pending) {
        QTimer::singleShot(0,this,[this,serial] {
            if (serial != m_move_preview_serial || !m_move_release_pending || !m_move_preview_delta) return;
            const auto ids=m_move_ids;
            const auto delta=*m_move_preview_delta;
            const auto accepted=m_move_preview_valid;
            resetGesture();
            if (accepted && m_entities_move_requested) (void)m_entities_move_requested(ids,delta);
            else if (m_entities_move_rejected) m_entities_move_rejected(ids,delta);
        });
    }
    return true;
}

void PlanCanvas::setAreaClassDropped(std::function<bool(QString, Vec2)> callback,
    std::function<void()> malformed_drop_rejected) {
    m_area_class_dropped=std::move(callback);
    m_area_class_drop_rejected=std::move(malformed_drop_rejected);
}

bool PlanCanvas::containsAreaPoint(const Boundary& boundary, Vec2 point) {
    QPainterPath path;
    if(!append_closed_boundary(path,boundary))return false;
    QPainterPathStroker border;border.setWidth(default_geometry_tolerance_metres*2);
    if(border.createStroke(path).contains(QPointF(point.x,point.y)))return false;
    return path.contains(QPointF(point.x,point.y));
}

QStringList PlanCanvas::areaIdsAt(Vec2 point) const {
    QStringList ids;
    for(const auto& entity:m_entities) {
        if(entity.type!=QStringLiteral("boundary") && entity.type!=QStringLiteral("measurement_boundary") &&
            entity.type!=QStringLiteral("room_boundary"))continue;
        if(const auto path=closed_entity_path(entity);path && path->contains(QPointF(point.x,point.y))) {
            QPainterPathStroker border;border.setWidth(default_geometry_tolerance_metres*2);
            if(!border.createStroke(*path).contains(QPointF(point.x,point.y)))ids.push_back(entity.id);
        }
    }
    return ids;
}

void PlanCanvas::setAreaClassCaption(QString caption) {m_area_class_caption=std::move(caption);update();}

void PlanCanvas::setSymbolDropped(std::function<void(QString, double, Vec2)> callback,
                                 std::function<bool(const QString&)> uses_raw_point) {
    clearSymbolDragPreview();
    m_symbol_dropped = std::move(callback);
    m_symbol_drop_uses_raw_point = std::move(uses_raw_point);
}

void PlanCanvas::setSymbolDragPreviewRequested(
    std::function<std::optional<CanvasEntity>(const QString&, double, Vec2)> callback) {
    clearSymbolDragPreview();
    m_symbol_drag_preview_requested = std::move(callback);
}

void PlanCanvas::clearSymbolDragPreview() {
    ++m_symbol_drag_preview_serial;
    if (!m_symbol_drag_active && !m_symbol_drag_preview) return;
    m_symbol_drag_active = false;
    m_symbol_drag_preview.reset();
    update();
}

std::optional<Vec2> PlanCanvas::symbolDropPoint(const QString& id, QPointF position) {
    const auto raw = toModel(position, rect());
    if (!std::isfinite(raw.x) || !std::isfinite(raw.y)) return std::nullopt;
    const auto uses_raw_point = m_symbol_drop_uses_raw_point;
    const QPointer<PlanCanvas> guard(this);
    bool project_onto_host = false;
    try {
        project_onto_host = uses_raw_point && uses_raw_point(id);
    } catch (...) {
        if (guard) clearSymbolDragPreview();
        return std::nullopt;
    }
    if (!guard) return std::nullopt;
    const auto point = project_onto_host ? raw : snapped(raw);
    return std::isfinite(point.x) && std::isfinite(point.y) ? std::optional{point} : std::nullopt;
}

bool PlanCanvas::updateSymbolDragPreview(const QMimeData* mime, QPointF position) {
    clearSymbolDragPreview();
    const auto payload = decode_symbol_drag(mime);
    if (!m_symbol_dropped || !payload) return false;
    const auto serial = m_symbol_drag_preview_serial;
    const auto callback = m_symbol_drag_preview_requested;
    const QPointer<PlanCanvas> guard(this);
    const auto point = symbolDropPoint(payload->id, position);
    if (!guard || serial != m_symbol_drag_preview_serial || !point) return false;
    // A valid library drag can enter away from a suitable host. An absent
    // proposal clears its ink while allowing later moves to find a host.
    std::optional<CanvasEntity> preview;
    try {
        if (callback) preview = callback(payload->id, payload->scale, *point);
    } catch (...) {
        if (guard) clearSymbolDragPreview();
        return false;
    }
    if (!guard || serial != m_symbol_drag_preview_serial) return false;
    if (preview) preview->selected = false;
    m_symbol_drag_active = true;
    m_symbol_drag_preview = std::move(preview);
    update();
    return true;
}

void PlanCanvas::dragEnterEvent(QDragEnterEvent* event) {
    m_pending_dimension_space_tap.reset();
    if(event->mimeData()->hasFormat(area_class_mime_type)) {
        clearSymbolDragPreview();
        if(m_area_class_dropped && decode_area_class_drag(event->mimeData()))event->acceptProposedAction();else event->ignore();
        return;
    }
    const QPointer<PlanCanvas> guard(this);
    const bool accepted = updateSymbolDragPreview(event->mimeData(), event->position());
    if (guard && accepted) event->acceptProposedAction();
    else event->ignore();
}

void PlanCanvas::dragMoveEvent(QDragMoveEvent* event) {
    m_pending_dimension_space_tap.reset();
    if(event->mimeData()->hasFormat(area_class_mime_type)) {
        clearSymbolDragPreview();
        if(m_area_class_dropped && decode_area_class_drag(event->mimeData()))event->acceptProposedAction();else event->ignore();
        return;
    }
    const QPointer<PlanCanvas> guard(this);
    const bool accepted = updateSymbolDragPreview(event->mimeData(), event->position());
    if (guard && accepted) event->acceptProposedAction();
    else event->ignore();
}

void PlanCanvas::dragLeaveEvent(QDragLeaveEvent* event) {
    clearSymbolDragPreview();
    event->accept();
}

void PlanCanvas::dropEvent(QDropEvent* event) {
    m_pending_dimension_space_tap.reset();
    clearSymbolDragPreview();
    const QPointer<PlanCanvas> guard(this);
    const bool admitted = admitInteraction();
    if (!guard || !admitted) { event->ignore(); return; }
    if(event->mimeData()->hasFormat(area_class_mime_type)) {
        const auto classification=decode_area_class_drag(event->mimeData());
        if(!classification && m_area_class_drop_rejected)m_area_class_drop_rejected();
        if(classification && m_area_class_dropped && m_area_class_dropped(*classification,toModel(event->position(),rect())))event->acceptProposedAction();
        else event->ignore();
        return;
    }
    const auto payload = decode_symbol_drag(event->mimeData());
    const auto callback = m_symbol_dropped;
    if (!callback || !payload) { event->ignore(); return; }
    const auto serial = m_symbol_drag_preview_serial;
    const auto point = symbolDropPoint(payload->id, event->position());
    if (!guard) { event->ignore(); return; }
    if (serial != m_symbol_drag_preview_serial || !point) {
        clearSymbolDragPreview();
        event->ignore();
        return;
    }
    try {
        callback(payload->id, payload->scale, *point);
    } catch (...) {
        if (guard) clearSymbolDragPreview();
        event->ignore();
        return;
    }
    if (!guard) { event->ignore(); return; }
    clearSymbolDragPreview();
    event->acceptProposedAction();
}

void PlanCanvas::setCursorMoved(std::function<void(Vec2)> callback) {
    m_cursor_moved = std::move(callback);
}

void PlanCanvas::setFinishRequested(std::function<void()> callback) {
    m_finish_requested = std::move(callback);
}

void PlanCanvas::setCancelRequested(std::function<void()> callback) {
    m_cancel_requested = std::move(callback);
}

void PlanCanvas::setPreciseInputRequested(std::function<void()> callback) {
    m_precise_input_requested = std::move(callback);
}

void PlanCanvas::setPendingDimensionTargetRequested(
    std::function<std::optional<CanvasPendingDimensionTarget>()> callback) {
    m_pending_dimension_target_requested = std::move(callback);
}

void PlanCanvas::setPendingDimensionOrientationRequested(
    std::function<void(CanvasPendingDimensionTarget, bool)> callback) {
    m_pending_dimension_orientation_requested = std::move(callback);
}

void PlanCanvas::setPendingDimensionOmissionRequested(
    std::function<void(CanvasPendingDimensionTarget)> callback) {
    m_pending_dimension_omission_requested = std::move(callback);
}

void PlanCanvas::setBayWindowReturnRequested(std::function<void()> callback) {
    m_bay_window_return_requested = std::move(callback);
}

void PlanCanvas::setWitnessAlignmentRequested(std::function<void(bool)> callback) {
    m_witness_alignment_requested = std::move(callback);
}

void PlanCanvas::setDirectionalAlignmentRequested(std::function<void(int, int, bool)> callback) {
    m_directional_alignment_requested = std::move(callback);
}

void PlanCanvas::setDrawingCornerJumpRequested(std::function<bool()> callback) {
    m_drawing_corner_jump_requested = std::move(callback);
}
void PlanCanvas::setDrawingTravelRequested(std::function<bool(int, int)> callback) {
    m_drawing_travel_requested = std::move(callback);
}
void PlanCanvas::setDrawingPenUpRequested(std::function<bool()> callback) {
    m_drawing_pen_up_requested = std::move(callback);
}

void PlanCanvas::setAutoCloseDrawingRequested(std::function<void()> callback) {
    m_auto_close_drawing_requested = std::move(callback);
}

bool PlanCanvas::drawingCommandIdle() const noexcept {
    return m_gesture_button == Qt::NoButton && !m_touch_active && !m_tablet_active &&
        !m_space_pan_armed && !m_panning && m_left_gesture == LeftGesture::none &&
        !m_move_release_pending && !m_move_preview_pending &&
        !m_transform_release_pending && !m_transform_preview_pending && !m_transform_frame_start &&
        !m_vertex_move_handle && !m_boundary_vertex_preview_pending &&
        !m_opening_width_handle && !m_opening_width_preview_pending;
}

void PlanCanvas::setDrawingTextRequested(std::function<bool(const QString&)> callback) {
    m_drawing_text_requested = std::move(callback);
}

void PlanCanvas::setDraftUndoRequested(std::function<void()> callback) {
    m_draft_undo_requested = std::move(callback);
}

void PlanCanvas::setDraftRedoRequested(std::function<void()> callback) {
    m_draft_redo_requested = std::move(callback);
}

bool PlanCanvas::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show) {
        const auto* dialog = qobject_cast<QDialog*>(watched);
        if (dialog && dialog->isModal()) {
            resetPerformanceMeasurements();
            resetGesture();
            resetTouchInput();
        }
    } else if ((event->type() == QEvent::WindowBlocked ||
                event->type() == QEvent::WindowDeactivate) && watched == window()) {
        resetPerformanceMeasurements();
        resetGesture();
        resetTouchInput();
    }
    return QWidget::eventFilter(watched, event);
}

bool PlanCanvas::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
    case QEvent::ScreenChangeInternal:
    case QEvent::DevicePixelRatioChange:
        if (event->type() == QEvent::ScreenChangeInternal ||
            event->type() == QEvent::DevicePixelRatioChange) ++m_navigation_generation;
        // System metrics can change while serialized QFont values remain equal.
        // Retire every label lane and its selection/bounds derivatives together;
        // an output recording must not reuse metrics from the previous font.
        invalidateRetainedPresentation();
        break;
    case QEvent::FocusOut:
        // Focus can move to a panel after release while an exact proposal or
        // queued admission still owns the gesture. Retire that authority, not
        // the drawing draft, and release keys whose key-up may go elsewhere.
        m_space_pan_armed = false;
        resetGesture();
        resetTouchInput();
        resetTabletInput();
        break;
    case QEvent::Leave:
        clearSymbolDragPreview();
        // Leave retires only the hover image. The placement command remains
        // armed and the next admitted cursor update supplies its new preview.
        if (m_component_placement_preview) {
            m_component_placement_preview.reset();
            update();
        }
        break;
    case QEvent::Hide:
    case QEvent::WindowBlocked:
    case QEvent::WindowDeactivate:
    case QEvent::UngrabMouse:
        m_component_placement_preview.reset();
        resetPerformanceMeasurements();
        resetGesture();
        m_space_pan_armed = false;
        resetTouchInput();
        resetTabletInput();
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        m_pending_dimension_space_tap.reset();
        beginPerformanceMeasurement(PerformanceMetric::input);
        break;
    default:
        break;
    }
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        handleTouchEvent(*static_cast<QTouchEvent*>(event));
        event->accept();
        return true;
    }
    case QEvent::TabletPress: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        retireDisconnectedTablet();
        const auto button=tablet->button();
        if (!m_tablet_active && !m_touch_active && m_gesture_button==Qt::NoButton && tablet->pointingDevice() &&
            (button==Qt::LeftButton || button==Qt::RightButton || button==Qt::MiddleButton)) {
            // Tip and barrel buttons share the mouse contract. Extra buttons
            // and another device cannot replace the button that owns a drag.
            m_tablet_active=true;
            m_tablet_button=button;
            m_tablet_device=tablet->pointingDevice();
            pointerPress(tablet->position(),button,tablet->modifiers());
        }
        event->accept();
        return true;
    }
    case QEvent::TabletMove: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        retireDisconnectedTablet();
        if (!m_touch_active &&
            ((m_tablet_active && m_tablet_device.data()==tablet->pointingDevice()) ||
             (!m_tablet_active && m_gesture_button==Qt::NoButton)))
            pointerMove(tablet->position(),tablet->modifiers());
        event->accept();
        return true;
    }
    case QEvent::TabletRelease: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        retireDisconnectedTablet();
        if (m_tablet_active && m_tablet_device.data()==tablet->pointingDevice() &&
            tablet->button()==m_tablet_button) {
            const auto button=m_tablet_button;
            // Retire this device before a context/placement callback can run
            // a nested event loop. Its late release cannot affect a new press.
            resetTabletInput();
            pointerRelease(tablet->position(),button,tablet->modifiers());
        }
        event->accept();
        return true;
    }
    default:
        return QWidget::event(event);
    }
}

void PlanCanvas::resetTouchInput() {
    m_touch_active = false;
    m_touch_id = -1;
    m_touch_navigation = false;
    m_touch_navigation_start.reset();
}

void PlanCanvas::resetTabletInput() {
    m_tablet_active=false;
    m_tablet_button=Qt::NoButton;
    m_tablet_device.clear();
}

void PlanCanvas::retireDisconnectedTablet() {
    if (!m_tablet_active || !m_tablet_device.isNull()) return;
    resetGesture();
    resetTabletInput();
}

void PlanCanvas::handleTouchEvent(QTouchEvent& event) {
    retireDisconnectedTablet();
    const auto valid = [](QPointF point) {
        return std::isfinite(point.x()) && std::isfinite(point.y());
    };
    if (event.type() == QEvent::TouchBegin) {
        if (m_tablet_active) return;
        resetGesture();
        resetTouchInput();
        setFocus();
        m_touch_active = true;
    }
    if (!m_touch_active) return; // A late update/release cannot restart input.
    if (event.type() == QEvent::TouchCancel || event.type() == QEvent::TouchEnd) {
        const auto point = std::find_if(event.points().cbegin(), event.points().cend(),
            [&](const auto& value) { return value.id() == m_touch_id; });
        if (event.type() == QEvent::TouchEnd && !m_touch_navigation &&
            point != event.points().cend() && valid(point->position()))
            pointerRelease(point->position(), Qt::LeftButton, event.modifiers());
        else
            resetGesture();
        resetTouchInput();
        return;
    }
    std::vector<const QEventPoint*> contacts;
    for (const auto& point : event.points()) {
        if (point.state() != QEventPoint::State::Released && valid(point.position()))
            contacts.push_back(&point);
    }
    std::sort(contacts.begin(), contacts.end(),
        [](const auto* a, const auto* b) { return a->id() < b->id(); });
    if (contacts.size() >= 2) {
        if (!m_touch_navigation) {
            // Never publish a one-finger object/vertex preview when navigation
            // takes over. Keep navigation latched until every finger lifts.
            resetGesture();
            m_touch_navigation = true;
        }
        const auto* first = contacts[0];
        const auto* second = contacts[1];
        const auto ids = std::pair{first->id(), second->id()};
        const auto centroid = (first->position() + second->position()) * 0.5;
        const auto span = first->position() - second->position();
        const auto distance = std::hypot(span.x(), span.y());
        if (!valid(centroid) || !std::isfinite(distance)) return;
        if (!m_touch_navigation_start || m_touch_navigation_start->ids != ids ||
            m_touch_navigation_start->initial_distance < 4.0) {
            // Rebase on a changed pair (or coincident contacts), without jumps.
            m_touch_navigation_start = TouchNavigation{ids, toModel(centroid, rect()),
                                                       distance, m_scale};
            return;
        }
        const auto& start = *m_touch_navigation_start;
        if (distance < 4.0) return;
        const auto scale = std::clamp(start.initial_scale * distance / start.initial_distance,
                                      minimum_scale, maximum_scale);
        const auto center = QRectF(rect()).center();
        const Vec2 proposed{start.anchor.x - (centroid.x()-center.x())/scale,
                            start.anchor.y + (centroid.y()-center.y())/scale};
        if (!std::isfinite(proposed.x) || !std::isfinite(proposed.y)) return;
        beginPerformanceMeasurement(PerformanceMetric::navigation);
        const auto previous_center = m_view_center;
        const auto previous_scale = m_scale;
        m_scale = scale;
        m_view_center = proposed;
        update();
        notifyNavigationChanged(previous_center, previous_scale);
        return;
    }
    if (m_touch_navigation) {
        m_touch_navigation_start.reset();
        return;
    }
    if (contacts.empty()) return;
    if (m_touch_id == -1) {
        m_touch_id = contacts.front()->id();
        pointerPress(contacts.front()->position(), Qt::LeftButton, event.modifiers());
    } else {
        const auto tracked = std::find_if(contacts.begin(), contacts.end(),
            [&](const auto* point) { return point->id() == m_touch_id; });
        if (tracked != contacts.end()) {
            pointerMove((*tracked)->position(), event.modifiers());
        } else {
            resetGesture();
            m_touch_navigation = true; // Suppress implicit finger handoff edits.
        }
    }
}

void PlanCanvas::pointerPress(QPointF position, Qt::MouseButton button,
                              Qt::KeyboardModifiers modifiers) {
    const bool middle_pan = button == Qt::MiddleButton;
    // Middle navigation can take over a pending left gesture. Other extra
    // buttons cannot retarget or complete the gesture that owns the press.
    if (m_gesture_button != Qt::NoButton && (m_panning || !middle_pan)) return;
    resetGesture();
    if (middle_pan || (button == Qt::LeftButton && overviewMapRect().contains(position)))
        beginPerformanceMeasurement(PerformanceMetric::navigation);
    setFocus();
    // The overview is a navigation surface, even while a temporary dimension
    // placement is active. Never publish its device points as authoring input.
    // Space-pan keeps its existing priority over a left overview press.
    if (button == Qt::LeftButton && !m_space_pan_armed && navigateOverviewMap(position)) {
        m_gesture_button = button;
        m_overview_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    updateCursor(position);
    m_gesture_button = button;
    if (middle_pan) {
        m_panning = true;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (button == Qt::RightButton) {
        m_right_start = position;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        return;
    }
    if (button != Qt::LeftButton) return;
    if (m_space_pan_armed) {
        m_left_gesture = LeftGesture::space_pan;
        m_left_start = position;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (!admitInteraction()) { resetGesture(); return; }
    if (m_point_placement_requested) {
        m_left_start = position;
        m_left_dragging = false;
        m_left_gesture = LeftGesture::canvas_pan;
        m_pan_start=position;
        m_pan_view_start=m_view_center;
        return;
    }
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    if (control) {
        m_left_gesture = LeftGesture::marquee;
        m_selection_start = position;
        m_selection_end = position;
        m_selection_dragging = false;
        m_selection_additive = true;
        return;
    }
    m_left_start = position;
    m_left_dragging = false;
    if (modifiers.testFlag(Qt::AltModifier) && selectionInteractionEnabled() &&
        m_overlap_selection_requested) {
        try {
            if (!m_overlap_selection_requested(true,{})) { resetGesture(); return; }
        } catch (...) { resetGesture(); return; }
        m_overlap_selection = true;
        m_overlap_view_scale = m_scale;
        m_overlap_view_size = size();
        m_overlap_view_dpr = devicePixelRatioF();
        m_overlap_navigation_generation = m_navigation_generation;
        m_left_gesture = LeftGesture::canvas_pan;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        return;
    }
    if (selectionInteractionEnabled()) {
        if (const auto jamb = openingWidthHandleAt(position, QRectF(rect()))) {
            m_left_gesture = LeftGesture::opening_width_resize;
            m_opening_width_handle = *jamb;
            if (m_entity_edit_gesture_started) m_entity_edit_gesture_started(jamb->entity_id);
            m_opening_width_jamb_preview = jamb->keep_start_jamb
                ? jamb->source.end_jamb : jamb->source.start_jamb;
            m_opening_width_scale_preview = 1.0;
            const auto host = jamb->source.host_baseline.value_or(
                Segment{jamb->source.start_jamb, jamb->source.end_jamb});
            try {
                const auto width = jamb->source.host_baseline
                    ? jamb->source.width_metres : segment_length(host);
                const auto offset = jamb->source.host_baseline ? jamb->source.offset_metres : 0.0;
                const auto moving_station = offset + (jamb->keep_start_jamb ? width : 0.0);
                const auto station = project_host_station(
                    host, toModel(position, rect()), moving_station);
                if (std::isfinite(station)) {
                    m_opening_width_press_station = station;
                    m_opening_width_pointer_station = station;
                }
            } catch (const std::exception&) {
                // Keep the gesture available as an invalid red preview.
            }
            setCursor(jamb_resize_cursor(jamb->source, jamb->keep_start_jamb));
            return;
        }
        if (const auto vertex = vertexHandleAt(position, QRectF(rect()))) {
            m_left_gesture = LeftGesture::vertex_move;
            m_vertex_move_handle = *vertex;
            m_vertex_move_press_pointer = toModel(position, rect());
            m_vertex_move_preview = vertex->source_position;
            const QPointer<PlanCanvas> guard(this);
            if (m_entity_edit_gesture_started) m_entity_edit_gesture_started(vertex->entity_id);
            if (!guard || m_left_gesture != LeftGesture::vertex_move || !m_vertex_move_handle) return;
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    const auto handle = selectionInteractionEnabled()
        ? selectionHandleAt(position, QRectF(rect())) : SelectionHandle::none;
    if (handle != SelectionHandle::none) {
        m_move_ids = selectedIds();
        m_transform_frame_start = selectionFrame(QRectF(rect()));
        m_transform_center = m_transform_frame_start->center();
        const auto axes = selectionAxes();
        if (axes) m_transform_center = toScreen(axes->center, rect());
        m_transform_initial_rotation = axes ? axes->rotation_radians : 0.0;
        m_transform_source_rotation = axes ? axes->source_rotation_radians : std::nullopt;
        m_transform_source_rotation_direction = axes ? axes->source_rotation_direction : 1.0;
        m_transform_start = position;
        // The longest physical axis supplies one practical dimension for a
        // corner's uniform resize; independently sized SVG axes keep their ratio.
        m_transform_resize_extent = axes ? std::max(axes->width_metres, axes->depth_metres) : 0.0;
        m_transform_scale_preview = 1.0;
        m_transform_rotation_preview = 0.0;
        if (handle == SelectionHandle::resize) {
            m_left_gesture = LeftGesture::selection_resize;
        } else if (handle == SelectionHandle::rotate) {
            m_left_gesture = LeftGesture::selection_rotate;
        } else if (axes) {
            m_left_gesture = LeftGesture::selection_axis_resize;
            m_axis_handle = handle;
            m_axis_rotation = axes->rotation_radians;
            const bool horizontal = handle == SelectionHandle::left || handle == SelectionHandle::right;
            const double sign = handle == SelectionHandle::right || handle == SelectionHandle::top ? 1.0 : -1.0;
            m_axis_extent = horizontal ? axes->width_metres : axes->depth_metres;
            const Vec2 direction = horizontal
                ? Vec2{std::cos(m_axis_rotation), std::sin(m_axis_rotation)}
                : Vec2{-std::sin(m_axis_rotation), std::cos(m_axis_rotation)};
            m_axis_anchor = {axes->center.x - sign * direction.x * m_axis_extent * .5,
                             axes->center.y - sign * direction.y * m_axis_extent * .5};
        }
        if (m_left_gesture == LeftGesture::selection_resize ||
            m_left_gesture == LeftGesture::selection_rotate ||
            m_left_gesture == LeftGesture::selection_axis_resize) {
            m_transform_source_id = m_move_ids.size() == 1 ? m_move_ids.front() : QString{};
            m_transform_pivot = toModel(m_transform_center, rect());
            if (!m_transform_source_id.isEmpty() && m_entity_transform_started)
                m_entity_transform_started(m_transform_source_id);
        }
        return;
    }
    m_pressed_entity = hitTest(position);
    m_pressed_occupied = !m_pressed_entity.isEmpty() || !hitTest(position, false).isEmpty();
    const auto retained_selection = selectedIds();
    const auto frame = selectionFrame(QRectF(rect()));
    // A selected annotation can paint away from its same-ID guide geometry.
    // Its painted hit still owns movement of the retained selection.
    const bool selected_hit = !m_pressed_entity.isEmpty() && retained_selection.contains(m_pressed_entity);
    if (selectionInteractionEnabled() && !retained_selection.isEmpty() &&
        (selected_hit || (frame && frame->contains(position)))) {
        m_left_gesture = LeftGesture::object_move;
        m_move_ids = retained_selection;
        captureOpeningMove(position);
        if (m_entities_move_started) m_entities_move_started(m_move_ids);
    } else {
        m_left_gesture = LeftGesture::canvas_pan;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
    }
}

void PlanCanvas::pointerMove(QPointF position, Qt::KeyboardModifiers modifiers) {
    m_pending_dimension_space_tap.reset();
    if (m_overview_dragging) {
        (void)navigateOverviewMap(position);
        return;
    }
    if (m_gesture_button == Qt::NoButton && overviewMapRect().contains(position)) {
        // Hovering into the map must not move a pending endpoint before its
        // navigation press or change the extents used by the next map gesture.
        setCursor(Qt::OpenHandCursor);
        return;
    }
    m_last_mouse_position = position;
    if (m_move_release_pending || m_transform_release_pending || m_vertex_release_pending) return;
    if (m_gesture_button == Qt::RightButton && !m_right_dragging &&
        (position - m_right_start).manhattanLength() >= QApplication::startDragDistance()) {
        m_right_dragging = true;
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (m_left_gesture == LeftGesture::marquee && m_selection_start) {
        m_selection_end = position;
        if ((position - *m_selection_start).manhattanLength() >= QApplication::startDragDistance())
            m_selection_dragging = true;
        update();
    }
    if (m_left_gesture == LeftGesture::canvas_pan) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_left_dragging = true;
            m_panning = true;
            setCursor(Qt::ClosedHandCursor);
        }
    } else if (m_left_gesture == LeftGesture::object_move) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_left_dragging = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_left_dragging) {
            const QPointer<PlanCanvas> guard(this);
            updateEntitiesMovePreview(position, modifiers);
            if (!guard) return;
            update();
        }
    } else if (m_left_gesture == LeftGesture::opening_width_resize) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance())
            m_left_dragging = true;
        if (m_left_dragging && m_opening_width_handle) {
            updateOpeningWidthPreview(position, modifiers);
            update();
        }
    } else if (m_left_gesture == LeftGesture::selection_axis_resize) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance())
            m_left_dragging = true;
        if (m_left_dragging && m_axis_extent > 1e-9) {
            const auto start = toModel(m_transform_start, rect());
            const auto current = toModel(position, rect());
            const bool horizontal = m_axis_handle == SelectionHandle::left || m_axis_handle == SelectionHandle::right;
            const double sign = m_axis_handle == SelectionHandle::right || m_axis_handle == SelectionHandle::top ? 1.0 : -1.0;
            const Vec2 direction = horizontal
                ? Vec2{std::cos(m_axis_rotation), std::sin(m_axis_rotation)}
                : Vec2{-std::sin(m_axis_rotation), std::cos(m_axis_rotation)};
            const auto factor = 1.0 + sign * ((current.x-start.x)*direction.x +
                                              (current.y-start.y)*direction.y) / m_axis_extent;
            const bool fine = modifiers.testFlag(Qt::ShiftModifier) || !m_snap_enabled || m_raw_point_input;
            if (std::isfinite(factor)) {
                const auto scale = practical_resize_scale(factor, m_axis_extent,
                    fine ? 0.0 : drawingLengthIncrementMetres());
                if (horizontal) m_axis_scale_x_preview = scale;
                else m_axis_scale_y_preview = scale;
            }
            setCursor(horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            m_transform_preview_pointer = position;
            m_transform_preview_fine = fine;
            const QPointer<PlanCanvas> guard(this);
            updateEntityTransformPreview();
            if (!guard) return;
            update();
        }
    } else if (m_left_gesture == LeftGesture::selection_resize ||
               m_left_gesture == LeftGesture::selection_rotate) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance())
            m_left_dragging = true;
        if (m_left_dragging) {
            const auto start = m_transform_start - m_transform_center;
            const auto current = position - m_transform_center;
            const bool fine = modifiers.testFlag(Qt::ShiftModifier) ||
                (m_left_gesture == LeftGesture::selection_resize && (!m_snap_enabled || m_raw_point_input));
            if (m_left_gesture == LeftGesture::selection_resize) {
                const auto initial_distance = std::hypot(start.x(), start.y());
                const auto current_distance = std::hypot(current.x(), current.y());
                if (initial_distance > 1e-6 && std::isfinite(current_distance))
                    m_transform_scale_preview = practical_resize_scale(current_distance / initial_distance,
                        m_transform_resize_extent, fine ? 0.0 : drawingLengthIncrementMetres());
                setCursor(Qt::SizeFDiagCursor);
            } else {
                if (std::hypot(current.x(), current.y()) > 1e-6) {
                    const auto delta = std::remainder(std::atan2(current.y(), current.x()) -
                        std::atan2(start.y(), start.x()), 2.0 * pi);
                    const auto initial = m_transform_source_rotation.value_or(m_transform_initial_rotation);
                    const auto direction = m_transform_source_rotation
                        ? m_transform_source_rotation_direction : 1.0;
                    auto absolute = initial - delta * direction;
                    if (std::abs(delta) > 1e-12 && !fine) {
                        constexpr double step = pi / 4.0;
                        absolute = std::round(absolute / step) * step;
                    }
                    m_transform_rotation_preview = std::remainder(
                        (absolute - initial) * direction, 2.0 * pi);
                }
                setCursor(Qt::CrossCursor);
            }
            m_transform_preview_pointer = position;
            m_transform_preview_fine = fine;
            const QPointer<PlanCanvas> guard(this);
            updateEntityTransformPreview();
            if (!guard) return;
            update();
        }
    } else if (m_left_gesture == LeftGesture::vertex_move) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_left_dragging = true;
        }
        if (m_left_dragging && m_vertex_move_handle) {
            updateBoundaryVertexPreview(position);
            setCursor(Qt::SizeAllCursor);
            update();
        }
    }
    if (m_panning) {
        beginPerformanceMeasurement(PerformanceMetric::navigation);
        const auto previous_center = m_view_center;
        const auto previous_scale = m_scale;
        const auto delta = position - m_pan_start;
        m_view_center = {m_pan_view_start.x - delta.x() / m_scale,
                         m_pan_view_start.y + delta.y() / m_scale};
        update();
        notifyNavigationChanged(previous_center, previous_scale);
    }
    updateCursor(position);
}

void PlanCanvas::pointerRelease(QPointF position, Qt::MouseButton button,
                               Qt::KeyboardModifiers modifiers) {
    m_pending_dimension_space_tap.reset();
    if (button != m_gesture_button) return;
    if (m_overview_dragging) {
        // Apply the final map location even when the platform omitted a move,
        // then leave the authoring cursor and draft exactly as they were.
        (void)navigateOverviewMap(position);
        resetGesture();
        return;
    }
    // Publish the final effective point before any click callback. A normal
    // click can cross a snap boundary between press and release without Qt
    // delivering an intervening move event; authoring must use the release
    // point the user actually chose.
    updateCursor(position);
    const bool left_navigation = m_left_gesture == LeftGesture::space_pan ||
        (m_left_gesture == LeftGesture::canvas_pan &&
            (m_left_dragging || m_panning ||
             (position-m_left_start).manhattanLength() >= QApplication::startDragDistance()));
    if (button == Qt::LeftButton && !left_navigation && !admitInteraction()) {
        resetGesture(); update(); return;
    }
    if (button == Qt::LeftButton && m_point_placement_requested &&
        m_left_gesture != LeftGesture::space_pan) {
        if (m_left_dragging || m_panning ||
            (position-m_left_start).manhattanLength()>=QApplication::startDragDistance()) {
            pointerMove(position,modifiers);
            resetGesture();
            update();
            return;
        }
        const auto callback = m_point_placement_requested;
        const auto point = toModel(position, QRectF(rect()));
        resetGesture();
        callback(point);
        return;
    }
    if (button == Qt::RightButton) {
        // Some devices omit motion events before release. Apply the same pan
        // threshold and final location without turning that release into a menu.
        pointerMove(position, modifiers);
        const bool clicked = !m_right_dragging &&
            (position - m_right_start).manhattanLength() < QApplication::startDragDistance();
        if (clicked && !admitInteraction(true)) { resetGesture(); update(); return; }
        const auto target = clicked ? contextTarget(position) : QString{};
        resetGesture();
        updateCursor(position);
        if (clicked && m_right_clicked) m_right_clicked(inputPoint(position), target);
        return;
    }
    if (button == Qt::MiddleButton && m_panning) {
        // Pen and mouse drivers may coalesce the whole drag into its release.
        pointerMove(position,modifiers);
        resetGesture();
        updateCursor(position);
        return;
    }
    if (button == Qt::LeftButton) {
        // Consume the final location or fine-input change even if the platform
        // omitted a move event. An unchanged transform keeps its admitted serial.
        if (m_left_gesture == LeftGesture::space_pan || m_left_gesture == LeftGesture::canvas_pan ||
            (m_left_gesture == LeftGesture::object_move &&
             (!m_opening_move_active || !m_move_preview_pointer || *m_move_preview_pointer != position ||
              m_move_preview_fine != (modifiers.testFlag(Qt::ShiftModifier) || !m_snap_enabled || m_raw_point_input))) ||
            ((m_left_gesture == LeftGesture::selection_axis_resize ||
              m_left_gesture == LeftGesture::selection_resize ||
              m_left_gesture == LeftGesture::selection_rotate) &&
             (!m_transform_preview_pointer || *m_transform_preview_pointer != position ||
              m_transform_preview_fine != (modifiers.testFlag(Qt::ShiftModifier) ||
                  (m_left_gesture != LeftGesture::selection_rotate && (!m_snap_enabled || m_raw_point_input))))) ||
            (m_left_gesture == LeftGesture::opening_width_resize &&
             (!m_opening_width_preview_pointer || *m_opening_width_preview_pointer != position ||
              m_opening_width_preview_fine != modifiers.testFlag(Qt::ShiftModifier))) ||
            (m_left_gesture == LeftGesture::vertex_move &&
             (!m_boundary_vertex_preview_pointer || *m_boundary_vertex_preview_pointer != position))) {
            const QPointer<PlanCanvas> guard(this);
            pointerMove(position, modifiers);
            if (!guard) return;
        }
        const auto gesture = m_left_gesture;
        const auto start = m_left_start;
        const auto selection_start = m_selection_start;
        const bool dragging = m_left_dragging || m_selection_dragging ||
            (gesture != LeftGesture::none &&
             (position - (selection_start ? *selection_start : start)).manhattanLength() >=
                 QApplication::startDragDistance());
        const auto move_ids = m_move_ids;
        const auto pressed_entity = m_pressed_entity;
        const auto pressed_occupied = m_pressed_occupied;
        const auto overlap_selection = m_overlap_selection;
        const bool overlap_view_current = m_overlap_view_scale==m_scale && m_overlap_view_size==size() &&
            m_overlap_view_dpr==devicePixelRatioF() &&
            m_overlap_navigation_generation==m_navigation_generation &&
            m_pan_view_start.x==m_view_center.x && m_pan_view_start.y==m_view_center.y;
        // Hosted body moves commit the final constrained midpoint displacement;
        // recomputing a free XY drag would lose the captured absolute station.
        const auto delta = gesture == LeftGesture::object_move
            ? m_move_preview_delta.value_or(Vec2{}) : dragDelta(position);
        if (gesture==LeftGesture::object_move && dragging && m_move_preview_pending) {
            // Keep the final exact proposal alive after button release. A
            // completion commits it on the UI thread; cancel/scene replacement
            // invalidates the serial and cannot revive the released edit.
            m_move_release_pending=true;
            m_gesture_button=Qt::NoButton;
            return;
        }
        if (gesture==LeftGesture::object_move && dragging && m_move_preview_exact && !m_move_preview_valid) {
            resetGesture();
            if (m_entities_move_rejected) m_entities_move_rejected(move_ids,delta);
            return;
        }
        if (gesture==LeftGesture::vertex_move && dragging && m_boundary_vertex_preview_requested) {
            // Keep the final source capture alive until the exact release-point
            // projection finishes. Missing/invalid proposals cannot authorize
            // a commit merely because the core might accept its coordinates.
            m_vertex_release_pending=true;
            m_gesture_button=Qt::NoButton;
            if (!m_boundary_vertex_preview_pending)
                finishBoundaryVertexPreview(m_boundary_vertex_preview_serial);
            return;
        }
        if ((gesture == LeftGesture::selection_resize || gesture == LeftGesture::selection_rotate ||
             gesture == LeftGesture::selection_axis_resize) &&
            dragging && m_transform_preview_exact) {
            if (m_transform_preview_pending) {
                m_transform_release_pending = true;
                m_gesture_button = Qt::NoButton;
            } else {
                finishEntityTransformPreview(m_transform_preview_serial);
            }
            return;
        }
        const auto transform_scale = m_transform_scale_preview;
        const auto transform_rotation = m_transform_rotation_preview;
        const auto axis_scale_x = m_axis_scale_x_preview;
        const auto axis_scale_y = m_axis_scale_y_preview;
        const auto axis_anchor = m_axis_anchor;
        const auto vertex_handle = m_vertex_move_handle;
        const auto opening_handle = m_opening_width_handle;
        const auto opening_scale = m_opening_width_scale_preview;
        // Pending projection is not admission. Only its exact admitted command
        // may complete the released gesture; basic inverse widths never reach it.
        const auto opening_valid = m_opening_width_preview_valid ||
            (m_opening_width_preview_pending && std::isfinite(opening_scale) &&
             opening_scale > 0.0);
        // pointerMove above also handles a release that crosses the drag
        // threshold without an intermediate move event. Commit its snapped,
        // grab-offset-adjusted target in the local callback path as well.
        const auto vertex_target = m_vertex_move_preview;
        // Pending exact geometry is not trusted for admission. Release retains
        // the target until its captured preview is admitted; invalid results reject.
        const bool vertex_valid = vertex_target && std::isfinite(vertex_target->x) &&
            std::isfinite(vertex_target->y) && (!m_boundary_vertex_preview_requested ||
                m_boundary_vertex_preview_valid || m_boundary_vertex_preview_pending);
        const auto closing = closingAnchor(position);
        resetGesture();
        if (gesture == LeftGesture::marquee && selection_start) {
            if (dragging) {
                const auto ids = rectangleHits(QRectF(*selection_start, position).normalized(),
                                               position.x() < selection_start->x());
                if (m_entities_selected) m_entities_selected(ids, true);
            } else if (m_entity_selection_clicked) {
                m_entity_selection_clicked(hitTest(position), true);
            }
        } else if (gesture == LeftGesture::canvas_pan && !dragging) {
            if (overlap_selection && m_overlap_selection_requested) {
                QStringList overlapping;
                if (overlap_view_current) (void)hitTest(position,true,&overlapping);
                (void)m_overlap_selection_requested(false,std::move(overlapping));
            } else if (selectionInteractionEnabled() && !pressed_entity.isEmpty()) {
                if (m_entity_selection_clicked)
                    m_entity_selection_clicked(pressed_entity, false);
                else if (m_entity_clicked)
                    m_entity_clicked(pressed_entity);
            } else if (selectionInteractionEnabled() && pressed_occupied) {
                // Excluded painted content is an ignored pick, never empty
                // drawing space. Keep tracing in active drafts unchanged.
                if (m_entity_selection_clicked) m_entity_selection_clicked({}, false);
                else if (m_entity_clicked) m_entity_clicked({});
            } else if (selectionInteractionEnabled() && !selectedIds().isEmpty()) {
                // An empty click outside the retained selection is an explicit
                // deselect. Do not also interpret it as the first drawing node;
                // the next empty click begins authoring once selection is clear.
                if (m_entity_selection_clicked) m_entity_selection_clicked({}, false);
            } else if (closing && m_finish_requested) {
                m_finish_requested();
            } else if (m_point_clicked) {
                m_point_clicked(inputPoint(position));
            }
        } else if (gesture == LeftGesture::object_move) {
            if (dragging && m_entities_move_requested &&
                (std::abs(delta.x) > 1e-12 || std::abs(delta.y) > 1e-12)) {
                (void)m_entities_move_requested(move_ids, delta);
            } else if (!dragging) {
                // The frame owns a drag after the platform threshold, but a
                // stationary click still resolves the exact painted target.
                // Empty space within that frame is outside the painted object,
                // so the first click clears the retained selection without
                // also creating a drawing node.
                if (!pressed_entity.isEmpty() && !move_ids.contains(pressed_entity)) {
                    if (m_entity_selection_clicked)
                        m_entity_selection_clicked(pressed_entity, false);
                    else if (m_entity_clicked)
                        m_entity_clicked(pressed_entity);
                } else if (pressed_entity.isEmpty() && m_entity_selection_clicked) {
                    m_entity_selection_clicked({}, false);
                }
            }
        } else if (gesture == LeftGesture::opening_width_resize && dragging &&
                   opening_handle && opening_valid && m_opening_width_resize_requested &&
                   std::abs(opening_scale - 1.0) > 1e-12) {
            (void)m_opening_width_resize_requested(opening_handle->entity_id, opening_scale,
                opening_handle->keep_start_jamb, opening_handle->source.source_revision);
        } else if (gesture == LeftGesture::selection_axis_resize && dragging &&
                   move_ids.size() == 1 && m_entity_axis_resize_requested) {
            (void)m_entity_axis_resize_requested(move_ids.front(), axis_scale_x,
                                                axis_scale_y, axis_anchor);
        } else if ((gesture == LeftGesture::selection_resize ||
                    gesture == LeftGesture::selection_rotate) && dragging &&
                   move_ids.size() == 1 && m_entity_transform_requested) {
            (void)m_entity_transform_requested(move_ids.front(), transform_scale,
                                               transform_rotation);
        } else if (gesture == LeftGesture::vertex_move && dragging &&
                   vertex_handle && vertex_target && vertex_valid && m_boundary_vertex_move_requested &&
                   (vertex_target->x != vertex_handle->source_position.x ||
                    vertex_target->y != vertex_handle->source_position.y)) {
            (void)m_boundary_vertex_move_requested(
                vertex_handle->entity_id, vertex_handle->vertex_id, *vertex_target,
                vertex_handle->source_revision);
        }
        updateCursor(position);
    }
    if (m_gesture_button != Qt::NoButton) resetGesture();
}

void PlanCanvas::resetGesture() {
    clearSymbolDragPreview();
    m_pending_dimension_space_tap.reset();
    ++m_opening_width_preview_serial;
    ++m_boundary_vertex_preview_serial;
    m_gesture_button = Qt::NoButton;
    m_panning = false;
    m_overview_dragging = false;
    m_selection_start.reset();
    m_selection_dragging = false;
    m_right_dragging = false;
    m_left_gesture = LeftGesture::none;
    m_left_dragging = false;
    m_pressed_entity.clear();
    m_pressed_occupied = false;
    m_overlap_selection = false;
    m_move_ids.clear();
    m_move_preview_delta.reset();
    ++m_move_preview_serial;
    m_move_entities_preview.clear();
    m_move_entities_preview_index.clear();
    m_move_labels_preview.clear(); m_move_references_preview.clear();
    m_move_preview_exact = false;
    m_move_preview_valid = false;
    m_move_preview_pending = false;
    m_move_preview_request_in_progress = false;
    m_move_release_pending = false;
    m_opening_move_active = false;
    m_opening_move_source.reset();
    m_opening_move_press_station.reset();
    m_opening_move_pointer_station.reset();
    m_opening_move_original_midpoint.reset();
    m_opening_move_preview_intent.reset();
    m_move_preview_pointer.reset();
    m_move_preview_fine = false;
    m_transform_frame_start.reset();
    m_transform_resize_extent = 0.0;
    m_transform_scale_preview = 1.0;
    m_transform_rotation_preview = 0.0;
    m_transform_preview_pointer.reset();
    m_transform_preview_fine = false;
    m_transform_source_rotation.reset();
    m_transform_source_rotation_direction = 1.0;
    ++m_transform_preview_serial;
    m_transform_source_id.clear();
    m_transform_entities_preview.clear();
    m_transform_entities_preview_index.clear();
    m_transform_labels_preview.clear(); m_transform_references_preview.clear();
    m_transform_preview_exact = false;
    m_transform_preview_valid = false;
    m_transform_preview_pending = false;
    m_transform_preview_request_in_progress = false;
    m_transform_release_pending = false;
    m_axis_handle = SelectionHandle::none;
    m_axis_scale_x_preview = 1.0;
    m_axis_scale_y_preview = 1.0;
    m_vertex_move_handle.reset();
    m_vertex_move_press_pointer.reset();
    m_vertex_move_preview.reset();
    m_boundary_vertex_entities_preview.clear();
    m_boundary_vertex_entities_preview_index.clear();
    m_boundary_vertex_labels_preview.clear();
    m_boundary_vertex_metrics_preview.reset();
    m_boundary_vertex_preview_valid = false;
    m_boundary_vertex_preview_pending = false;
    m_vertex_release_pending = false;
    m_boundary_vertex_preview_request_in_progress = false;
    m_boundary_vertex_preview_pointer.reset();
    m_opening_width_handle.reset();
    m_opening_width_press_station.reset();
    m_opening_width_pointer_station.reset();
    m_opening_width_jamb_preview.reset();
    m_opening_width_entities_preview.clear();
    m_opening_width_entities_preview_index.clear();
    m_opening_width_labels_preview.clear();
    m_opening_width_scale_preview = 1.0;
    m_opening_width_preview_valid = false;
    m_opening_width_preview_pending = false;
    m_opening_width_preview_request_in_progress = false;
    m_opening_width_preview_pointer.reset();
    m_opening_width_preview_fine = false;
    if (m_space_pan_armed) {
        setCursor(Qt::OpenHandCursor);
    } else if (m_last_mouse_position) {
        updatePointerCursor(*m_last_mouse_position);
    } else {
        setCursor(Qt::CrossCursor);
    }
    update();
}

void PlanCanvas::setRightClicked(std::function<void(Vec2, QString)> callback) {
    m_right_clicked = std::move(callback);
}

void PlanCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    retireDisconnectedTablet();
    if (event->source()!=Qt::MouseEventNotSynthesized || m_touch_active || m_tablet_active) {
        event->accept();
        return;
    }
    // Rapid Alt clicks still cycle once per physical click: Qt replaces the
    // second press with this event. Its release uses the normal captured pick.
    // Ctrl retains priority and its existing first-toggle-only double click.
    if (event->button() == Qt::LeftButton && event->source() == Qt::MouseEventNotSynthesized &&
        !m_point_placement_requested && event->modifiers().testFlag(Qt::AltModifier) &&
        !event->modifiers().testFlag(Qt::ControlModifier) && selectionInteractionEnabled() &&
        m_overlap_selection_requested) {
        pointerPress(event->position(), event->button(), event->modifiers());
        event->accept();
        return;
    }
    // Qt dispatches press/release/double-click/release. The first click already
    // authored or selected. An idle pointer's unmodified left double-click
    // opens contextual properties for the stable hit target. Active authoring
    // consumes the second press so a double-click cannot add a duplicate point
    // or replay Ctrl-selection.
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
        selectionInteractionEnabled() && m_entity_double_clicked && admitInteraction()) {
        const auto target = hitTest(event->position());
        if (!target.isEmpty()) m_entity_double_clicked(target);
    }
    event->accept();
}

std::optional<QRectF> PlanCanvas::selectionBounds() const {
    return selectionFrame(QRectF(rect()));
}

QRectF PlanCanvas::labelLayoutBounds(const CanvasLabel& label, double pixels_per_metre) const {
    return canvasLabelLayoutBounds(label, font(), this, pixels_per_metre, logicalDpiY());
}

std::optional<QPointF> PlanCanvas::selectionRotationHandlePosition() const {
    const QRectF viewport(rect());
    if (!m_selection_controls_visible || !m_selection_rotate_enabled ||
        selectedIds().size()!=1 || selectedOpening() || !selectionFrame(viewport)) return std::nullopt;
    return selectionControlTransform(viewport).map(selectionRotationPoint(
        viewport,selectionAnnotationFootprints(viewport,font())));
}

std::optional<QRectF> PlanCanvas::selectionBounds(const QRectF& viewport) const {
    if (hasInteractivePresentation()) return computeSelectionBounds(viewport);
    const auto key = retainedSelectionKey(viewport);
    if (m_selection_bounds_cache.key != key) {
        m_selection_bounds_cache.bounds = computeSelectionBounds(viewport);
        m_selection_bounds_cache.key = key;
    }
    return m_selection_bounds_cache.bounds;
}

std::optional<QRectF> PlanCanvas::computeSelectionBounds(const QRectF& viewport) const {
    std::optional<QRectF> result;
    const auto include = [&](const QRectF& bounds) {
        if (!std::isfinite(bounds.left()) || !std::isfinite(bounds.right()) ||
            !std::isfinite(bounds.top()) || !std::isfinite(bounds.bottom())) return;
        // Explicit extrema retain zero-width/height geometry such as lines.
        result = result ? QRectF(QPointF(std::min(result->left(), bounds.left()),
                                         std::min(result->top(), bounds.top())),
                                 QPointF(std::max(result->right(), bounds.right()),
                                         std::max(result->bottom(), bounds.bottom())))
                        : bounds;
    };
    ensureRetainedSelection();
    for (const auto index : m_selected_entity_indices) {
        const auto& retained_entity = m_entities[index];
        const auto& entity = interactiveEntity(retained_entity);
        const auto preview = m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(entity.id)
            ? *m_move_preview_delta : Vec2{};
        const auto width = wall_baseline_only(entity)
            ? std::max(entity.thickness_metres, 0.04) : entity.stroke_width_metres;
        const auto padding = std::isfinite(width) && width > 0.0 ? width * m_scale * 0.5 : 1.5;
        for (const auto& segment : entity.segments) {
            Vec2 minimum = segment.start;
            Vec2 maximum = segment.end;
            try {
                const auto bounds = segment_bounds(segment);
                minimum = bounds.minimum;
                maximum = bounds.maximum;
            } catch (const std::invalid_argument&) {
                // Match the finite endpoint fallback used for view fitting.
            }
            minimum.x += preview.x;
            minimum.y += preview.y;
            maximum.x += preview.x;
            maximum.y += preview.y;
            include(QRectF(toScreen(minimum, viewport), toScreen(maximum, viewport))
                        .normalized().adjusted(-padding, -padding, padding, padding));
        }
    }
    for (const auto& label : positionedLabels(font(), this, m_scale, logicalDpiY(), false)) {
        if (!label.selected || !drawable_label(label)) continue;
        const auto layout = label_layout(label, font(), this, m_scale, logicalDpiY());
        include(label_transform(label, toScreen(label.position, viewport)).mapRect(layout.bounds));
    }
    for (const auto& retained : m_references) {
        const auto& reference = interactiveReference(retained);
        if (!reference.selected || !reference.visible || reference.image.isNull()) continue;
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (!std::isfinite(unit) || unit <= 0.0) continue;
        QTransform transform;
        transform.translate(viewport.center().x(), viewport.center().y());
        transform.scale(m_scale, -m_scale);
        auto position = reference.position;
        if (m_move_preview_delta && &reference == &retained && (!m_move_preview_exact || m_move_preview_valid) &&
            m_move_ids.contains(reference.id)) {
            position.x += m_move_preview_delta->x;
            position.y += m_move_preview_delta->y;
        }
        transform.translate(position.x - m_view_center.x,
                            position.y - m_view_center.y);
        if (std::isfinite(reference.rotation_degrees)) transform.rotate(reference.rotation_degrees);
        const auto width = reference.image.width() * unit;
        const auto height = reference.image.height() * unit;
        include(transform.mapRect(QRectF(-width * 0.5, -height * 0.5, width, height)));
    }
    if (result) *result = result->adjusted(-6.0, -6.0, 6.0, 6.0);
    return result;
}

std::optional<QRectF> PlanCanvas::selectionFrame(const QRectF& viewport) const {
    if (hasInteractivePresentation()) return computeSelectionFrame(viewport);
    const auto key = retainedSelectionKey(viewport);
    if (m_selection_frame_cache.key != key) {
        m_selection_frame_cache.bounds = computeSelectionFrame(viewport);
        m_selection_frame_cache.key = key;
    }
    return m_selection_frame_cache.bounds;
}

std::optional<QRectF> PlanCanvas::computeSelectionFrame(const QRectF& viewport) const {
    if (selectionAxes()) {
        const auto bounds = selectionControlTransform(viewport).mapRect(selectionControlRect(viewport));
        const auto visible = bounds.intersected(viewport.adjusted(5.0, 5.0, -5.0, -5.0));
        return visible.isEmpty() ? std::nullopt : std::optional<QRectF>{visible};
    }
    auto bounds = selectionBounds(viewport);
    if (!bounds) return std::nullopt;
    // The painted frame is also the move hit target. A 44 px minimum keeps
    // thin lines and small symbols usable with touch or a pen without adding
    // an invisible halo outside the feedback the user can see.
    constexpr qreal minimum_target = 44.0;
    if (bounds->width() < minimum_target) {
        const auto expansion = (minimum_target - bounds->width()) * 0.5;
        bounds->adjust(-expansion, 0.0, expansion, 0.0);
    }
    if (bounds->height() < minimum_target) {
        const auto expansion = (minimum_target - bounds->height()) * 0.5;
        bounds->adjust(0.0, -expansion, 0.0, expansion);
    }
    const auto frame = bounds->intersected(viewport.adjusted(5.0, 5.0, -5.0, -5.0));
    return frame.isEmpty() ? std::nullopt : std::optional<QRectF>{frame};
}

std::optional<CanvasSelectionFrame> PlanCanvas::entitySelectionAxes(const CanvasEntity& entity) const {
    const auto valid = [](const CanvasSelectionFrame& frame) {
        return std::isfinite(frame.center.x) && std::isfinite(frame.center.y) &&
            std::isfinite(frame.rotation_radians) && std::isfinite(frame.width_metres) &&
            std::isfinite(frame.depth_metres) && frame.width_metres >= 0 &&
            frame.depth_metres >= 0 && (frame.width_metres > 0 || frame.depth_metres > 0) &&
            (!frame.source_rotation_radians ||
             (std::isfinite(*frame.source_rotation_radians) &&
              (frame.source_rotation_direction == 1.0 || frame.source_rotation_direction == -1.0)));
    };
    if (entity.resize_frame && entity.resize_frame->source_rotation_radians &&
        valid(*entity.resize_frame)) return entity.resize_frame;
    if (entity.svg_symbol) {
        const auto& s = *entity.svg_symbol;
        const CanvasSelectionFrame frame{s.position, s.rotation_radians, s.width_metres, s.depth_metres};
        if (valid(frame)) return frame;
    }
    if (entity.resize_frame && valid(*entity.resize_frame)) return entity.resize_frame;
    try {
        const auto bounds = boundary_bounds(entity.segments);
        const CanvasSelectionFrame frame{
            {(bounds.minimum.x+ bounds.maximum.x)*.5, (bounds.minimum.y+bounds.maximum.y)*.5},
            0, bounds.maximum.x-bounds.minimum.x, bounds.maximum.y-bounds.minimum.y};
        if (valid(frame)) return frame;
    } catch (const std::invalid_argument&) {}
    return std::nullopt;
}

const CanvasReference& PlanCanvas::interactiveReference(const CanvasReference& reference) const {
    if (m_move_preview_exact && m_move_preview_valid)
        for (const auto& proposed : m_move_references_preview)
            if (proposed.id == reference.id) return proposed;
    if (m_transform_preview_exact && m_transform_preview_valid)
        for (const auto& proposed : m_transform_references_preview)
            if (proposed.id == reference.id) return proposed;
    return reference;
}

std::optional<CanvasSelectionFrame> PlanCanvas::selectionAxes() const {
    if (hasInteractivePresentation()) return computeSelectionAxes();
    // Oriented axes are in model space. Panning does not change them.
    const auto key = retainedSelectionKey({}, true);
    if (m_selection_axes_key != key) {
        m_selection_axes_cache = computeSelectionAxes();
        m_selection_axes_key = key;
    }
    return m_selection_axes_cache;
}

std::optional<CanvasSelectionFrame> PlanCanvas::computeSelectionAxes() const {
    if (selectedIds().size() != 1) return std::nullopt;
    // A compound producer supplies the whole assembly's oriented frame.
    // Without one, combine all profiles of the sole semantic selection.
    std::optional<CanvasSelectionFrame> compound;
    int selected_profiles=0;
    for (const auto index : m_selected_entity_indices) {
        const auto& retained = m_entities[index];
        if (retained.presentation_key.isEmpty()) continue;
        ++selected_profiles;
        const auto& entity=interactiveEntity(retained);
        if (entity.resize_frame) return entitySelectionAxes(entity);
        if (const auto frame=entitySelectionAxes(entity)) {
            // Rotated artwork retains its established single-profile frame.
            if (!compound) compound=frame;
            else try {
                const auto bounds=boundary_bounds(entity.segments);
                const double left=std::min(compound->center.x-compound->width_metres*.5,bounds.minimum.x);
                const double right=std::max(compound->center.x+compound->width_metres*.5,bounds.maximum.x);
                const double bottom=std::min(compound->center.y-compound->depth_metres*.5,bounds.minimum.y);
                const double top=std::max(compound->center.y+compound->depth_metres*.5,bounds.maximum.y);
                compound=CanvasSelectionFrame{{(left+right)*.5,(bottom+top)*.5},0,right-left,top-bottom};
            } catch (const std::invalid_argument&) {}
        }
    }
    if (selected_profiles>1 && compound) return compound;
    for (const auto index : m_selected_entity_indices)
        return entitySelectionAxes(interactiveEntity(m_entities[index]));
    for (const auto& retained : m_references) {
        const auto& reference = interactiveReference(retained);
        if (!reference.selected || !reference.visible || reference.image.isNull()) continue;
        const auto unit = reference.metres_per_source_unit*reference.scale;
        if (!(unit > 0) || !std::isfinite(unit) || !std::isfinite(reference.rotation_degrees) ||
            !std::isfinite(reference.position.x) || !std::isfinite(reference.position.y)) continue;
        const auto position = m_move_preview_delta && &reference == &retained && m_move_preview_valid && m_move_ids.contains(reference.id)
            ? reference.position + *m_move_preview_delta : reference.position;
        return CanvasSelectionFrame{position, reference.rotation_degrees*pi/180,
            reference.image.width()*unit, reference.image.height()*unit};
    }
    for (const auto& label : positionedLabels(font(), this, m_scale, logicalDpiY(), false)) {
        if (!label.selected || !drawable_label(label) || !std::isfinite(label.rotation_radians)) continue;
        const auto layout = label_layout(label,font(),this,m_scale,logicalDpiY());
        return label_selection_frame(label, layout.bounds, m_scale);
    }
    return std::nullopt;
}

const std::vector<CanvasLabel>& PlanCanvas::positionedLabels(
    const QFont& base_font, const QPaintDevice* device, double scale,
    double dpi, bool output, bool content_only, Vec2 layout_origin, bool floor_ghost) const {
    auto& cache = m_label_placement_cache[floor_ghost ? 3 : content_only ? 2 : output ? 1 : 0];
    const bool interactive = !output && !floor_ghost;
    QByteArray retained_key;
    if (!interactive || !hasInteractivePresentation()) {
        QDataStream retained_signature(&retained_key, QIODevice::WriteOnly);
        retained_signature << base_font << font() << scale << dpi << output << content_only
            << floor_ghost << m_metric_units << layout_origin.x << layout_origin.y
            << quint64(reinterpret_cast<quintptr>(device))
            << device->logicalDpiX() << device->logicalDpiY()
            << device->physicalDpiX() << device->physicalDpiY()
            << device->devicePixelRatioF() << device->devType()
            << device->width() << device->height();
        // Setters clear this key even for same-ID/revision replacements. Avoid
        // copying/serializing every label during unchanged navigation paints.
        if (cache.retained_key == retained_key) return cache.labels;
    }
    // A preview result must never become the settled-input fast path on release.
    cache.retained_key.clear();
    auto retained_labels = floor_ghost ? m_floor_ghost_labels : m_labels;
    using LabelPresentationKey = QPair<QString, QString>;
    QHash<LabelPresentationKey, const CanvasLabel*> preview_labels;
    if (interactive && (m_transform_preview_valid || m_move_preview_valid ||
                        m_boundary_vertex_preview_valid || m_opening_width_preview_valid)) {
        QSet<LabelPresentationKey> retained_presentations;
        retained_presentations.reserve(retained_labels.size());
        for (const auto& label : retained_labels)
            retained_presentations.insert({label.id, label.callout_role});
        const auto append_preview_labels = [&](const std::vector<CanvasLabel>& preview, bool valid) {
            if (!valid) return;
            for (const auto& proposed : preview) {
                const LabelPresentationKey presentation{proposed.id, proposed.callout_role};
                if (retained_presentations.contains(presentation)) continue;
                retained_presentations.insert(presentation);
                retained_labels.push_back(proposed);
            }
        };
        append_preview_labels(m_transform_labels_preview, m_transform_preview_valid);
        append_preview_labels(m_move_labels_preview, m_move_preview_valid);
        append_preview_labels(m_boundary_vertex_labels_preview, m_boundary_vertex_preview_valid);
        append_preview_labels(m_opening_width_labels_preview, m_opening_width_preview_valid);

        // Highest precedence first preserves the first match within each preview,
        // including blank replacements that withhold a retained label's text.
        const auto index_preview_labels = [&](const std::vector<CanvasLabel>& preview, bool valid) {
            if (!valid) return;
            for (const auto& proposed : preview) {
                const LabelPresentationKey presentation{proposed.id, proposed.callout_role};
                if (!preview_labels.contains(presentation))
                    preview_labels.insert(presentation, &proposed);
            }
        };
        index_preview_labels(m_opening_width_labels_preview, m_opening_width_preview_valid);
        index_preview_labels(m_boundary_vertex_labels_preview, m_boundary_vertex_preview_valid);
        index_preview_labels(m_move_labels_preview, m_move_preview_valid);
        index_preview_labels(m_transform_labels_preview, m_transform_preview_valid);
    }
    std::vector<CanvasLabel> labels;
    labels.reserve(retained_labels.size());
    QByteArray key;
    QDataStream signature(&key, QIODevice::WriteOnly);
    signature << retained_key << base_font << font() << scale << dpi << output << content_only
              << layout_origin.x << layout_origin.y << quint64(retained_labels.size())
              << device->logicalDpiX() << device->logicalDpiY()
              << device->devicePixelRatioF() << device->devType();
    const auto point_key = [&](Vec2 p) { signature << p.x << p.y; };
    for (const auto& retained : retained_labels) {
        auto label = retained;
        if (interactive) {
            const auto preview_label = preview_labels.constFind({retained.id, retained.callout_role});
            if (preview_label != preview_labels.cend()) label = **preview_label;
            if (retained.selected && m_transform_frame_start && !m_transform_preview_exact &&
                m_move_ids.contains(retained.id) &&
                (m_left_gesture == LeftGesture::selection_resize ||
                 m_left_gesture == LeftGesture::selection_rotate)) {
                label.rotation_radians += m_transform_rotation_preview;
                label.scale *= m_transform_scale_preview;
            }
        }
        if (interactive && m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(label.id)) {
            const auto delta = *m_move_preview_delta;
            label.position = label.position + delta;
            if (label.leader_start) label.leader_start = *label.leader_start + delta;
            if (label.automatic_linear_placement) {
                label.automatic_linear_placement->anchor.start =
                    label.automatic_linear_placement->anchor.start + delta;
                label.automatic_linear_placement->anchor.end =
                    label.automatic_linear_placement->anchor.end + delta;
            }
        }
        if (retained_key.isEmpty()) {
            signature << label.id << label.text << label.selected << label.rotation_radians
                      << label.scale << label.text_height_metres << label.paper_height_mm
                      << label.color << label.bold << label.italic << label.fill_color
                      << label.fill_pattern << label.show_background << label.avoid_components
                      << label.plan_only << label.selection_type << label.font_family << label.model_plan
                      << label.wall_dimension_manual_rotation << label.text_alignment << label.callout_role;
            signature << label.fill_opacity.has_value();
            if (label.fill_opacity) signature << *label.fill_opacity;
            point_key(label.position);
            signature << label.leader_start.has_value() << label.plan_label_offset.has_value();
            if (label.leader_start) point_key(*label.leader_start);
            if (label.plan_label_offset) point_key(*label.plan_label_offset);
            signature << label.automatic_linear_placement.has_value();
            if (label.automatic_linear_placement) {
                const auto& automatic = *label.automatic_linear_placement;
                point_key(automatic.anchor.start);
                point_key(automatic.anchor.end);
                signature << automatic.anchor.sweep_radians << automatic.clearance_metres;
                point_key(automatic.outward_normal);
            }
        }
        labels.push_back(std::move(label));
    }
    // Reference-grid callouts are rendered labels too, with their own font.
    if (retained_key.isEmpty()) {
        signature << quint64(content_only || floor_ghost ? 0 : m_reference_grids.size());
        for (const auto& grid : m_reference_grids) {
            if (content_only || floor_ghost) break;
            signature << grid.id << grid.visible << grid.x_label << grid.y_label;
            signature << quint64(grid.lines.size());
            for (const auto& line : grid.lines) {
                point_key(line.start);
                point_key(line.end);
                signature << line.index << static_cast<int>(line.axis);
            }
        }
    }
    if (cache.key == key) {
        cache.retained_key = std::move(retained_key);
        return cache.labels;
    }

    // A label replacement must never inherit paint layouts from the previous
    // key, including the invalid-scale/DPI early publication below.
    cache.paint_layouts.clear();
    cache.hit_index = {};
    if (!(scale > 0) || !std::isfinite(scale) || !(dpi > 0) || !std::isfinite(dpi)) {
        cache.key = std::move(key);
        cache.retained_key = std::move(retained_key);
        cache.labels = std::move(labels);
        return cache.labels;
    }
    const auto model_screen = [&](Vec2 p) {
        return QPointF((p.x - layout_origin.x) * scale, -(p.y - layout_origin.y) * scale);
    };
    std::vector<QRectF> footprints(labels.size());
    std::vector<LabelPaintLayout> paint_layouts(labels.size());
    LabelObstacleIndex obstacles;
    std::vector<std::size_t> automatic_labels;
    const auto finite_rect = [](const QRectF& bounds) {
        return std::isfinite(bounds.left()) && std::isfinite(bounds.right()) &&
               std::isfinite(bounds.top()) && std::isfinite(bounds.bottom());
    };
    const auto valid_automatic = [](const CanvasLinearLabelPlacement& placement) {
        const auto& anchor = placement.anchor;
        const auto chord = distance(anchor.start, anchor.end);
        const auto normal_length = std::hypot(placement.outward_normal.x, placement.outward_normal.y);
        if (!std::isfinite(anchor.start.x) || !std::isfinite(anchor.start.y) ||
            !std::isfinite(anchor.end.x) || !std::isfinite(anchor.end.y) ||
            !std::isfinite(anchor.sweep_radians) || !std::isfinite(chord) || chord <= 1e-9 ||
            !std::isfinite(normal_length) || normal_length <= 1e-9 ||
            !std::isfinite(placement.clearance_metres) || placement.clearance_metres < 0) return false;
        // The opt-in normal must identify one of the host's two sides.
        const auto cross = (anchor.end.x-anchor.start.x)*placement.outward_normal.y -
                           (anchor.end.y-anchor.start.y)*placement.outward_normal.x;
        return std::isfinite(cross) && std::abs(cross) > chord*normal_length*1e-8 &&
               (anchor.sweep_radians == 0 || arc_info(anchor).has_value());
    };
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto& label = labels[i];
        if (!drawable_label(label)) continue;
        auto label_font = label.paper_height_mm > 0 && std::isfinite(label.paper_height_mm)
            ? font() : base_font;
        if (output) { label_font.setFeature("calt", 0); label_font.setFeature("case", 0); }
        const auto layout = label_layout(label, label_font, device, scale, dpi);
        auto ink_bounds = layout.bounds;
        if (!output) {
            const auto text_bounds = label.text_alignment == QStringLiteral("left") ||
                                     label.text_alignment == QStringLiteral("right")
                ? layout.bounds.adjusted(5.0, 3.0, -5.0, -3.0) : layout.bounds;
            // Rectangle text metrics include multiline spacing and font
            // bearings under the exact drawText alignment policy. Keep these
            // solely for culling; authored layout/placement bounds are unchanged.
            const auto text_ink = QFontMetricsF(layout.font, device).boundingRect(
                text_bounds, label_text_alignment(label), label.text);
            ink_bounds = finite_rect(text_ink) ? ink_bounds.united(text_ink) : text_ink;
        }
        paint_layouts[i] = {layout.font,layout.bounds,ink_bounds};
        footprints[i] = label_transform(label, {}).mapRect(layout.bounds);
        const auto bounds = footprints[i].translated(model_screen(label.position));
        if (!finite_rect(bounds)) continue;
        if (label.automatic_linear_placement && valid_automatic(*label.automatic_linear_placement))
            automatic_labels.push_back(i);
        else obstacles.add(bounds);
    }
    QFont grid_font = base_font;
    grid_font.setPixelSize(output ? static_cast<int>(std::clamp(std::lround(
        std::min(2.5*dpi/25.4, 48.0)), 8L, 48L)) : 11);
    grid_font.setWeight(QFont::DemiBold);
    const QFontMetricsF grid_metrics(grid_font, device);
    for (const auto& grid : m_reference_grids) {
        if (content_only || floor_ghost) break;
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            const auto& prefix = line.axis == ReferenceGridAxis::x ? grid.x_label : grid.y_label;
            const auto start = model_screen(line.start), end = model_screen(line.end);
            const auto outward = line.axis == ReferenceGridAxis::x ? end-start : start-end;
            const auto length = std::hypot(outward.x(), outward.y());
            if (prefix.isEmpty() || !(length > 0) || !std::isfinite(length)) continue;
            auto bounds = grid_metrics.boundingRect(prefix + QString::number(line.index));
            bounds.moveCenter((line.axis == ReferenceGridAxis::x ? end : start) + outward*(6.0/length));
            bounds.adjust(-4, -2, 4, 2);
            if (finite_rect(bounds)) obstacles.add(bounds);
        }
    }
    std::stable_sort(automatic_labels.begin(), automatic_labels.end(), [&](auto a, auto b) {
        if (labels[a].id != labels[b].id) return labels[a].id < labels[b].id;
        return labels[a].callout_role < labels[b].callout_role;
    });
    if (!automatic_labels.empty()) obstacles.enable();
    // Millimetres converted through the actual paper/device scale, including
    // high-DPI and fitted-sheet output. Padded rotated rectangles are a
    // conservative footprint: their glyphs/backgrounds cannot overlap.
    const double gap = std::max(3.0, dpi / 25.4);
    for (const auto i : automatic_labels) {
        auto& label = labels[i];
        const auto& placement = *label.automatic_linear_placement;
        const auto& anchor = placement.anchor;
        const auto chord = distance(anchor.start, anchor.end);
        const auto arc = arc_info(anchor);
        const auto point_at = [&](double t) {
            return arc ? arc_point(anchor, *arc, t) : anchor.start + (anchor.end-anchor.start)*t;
        };
        const Vec2 left{-(anchor.end.y-anchor.start.y)/chord,
                        (anchor.end.x-anchor.start.x)/chord};
        const auto side = left.x*placement.outward_normal.x + left.y*placement.outward_normal.y >= 0 ? 1.0 : -1.0;
        const auto normal_at = [&](double t) {
            if (!arc) return left * side;
            const auto tangent_angle = arc->start_angle + anchor.sweep_radians*t +
                (anchor.sweep_radians > 0 ? pi/2 : -pi/2);
            return Vec2{-std::sin(tangent_angle)*side, std::cos(tangent_angle)*side};
        };
        const auto midpoint = point_at(.5);
        const auto normal = normal_at(.5);
        const QPointF screen_normal(normal.x, -normal.y);
        const QPointF screen_tangent(normal.y, normal.x);
        const auto footprint = footprints[i];
        const auto extent = [&](QPointF axis) {
            double result = 0;
            for (const auto corner : {footprint.topLeft(), footprint.topRight(),
                                      footprint.bottomLeft(), footprint.bottomRight()})
                result = std::max(result, std::abs(QPointF::dotProduct(corner, axis)));
            return result;
        };
        const auto outward_extent = extent(screen_normal);
        const auto length = arc ? arc->radius*std::abs(anchor.sweep_radians) : chord;
        if (!std::isfinite(length) || !(length > 0)) continue;
        const auto margin = std::min(.5, extent(screen_tangent)/(scale*length));
        const auto preferred_distance = (label.position.x-midpoint.x)*normal.x +
                                        (label.position.y-midpoint.y)*normal.y;
        const auto baseline_distance = std::max({placement.clearance_metres, preferred_distance,
                                                 (outward_extent+gap)/scale});
        const auto lane_step = (2*outward_extent+gap)/scale;
        bool placed = false;
        const auto attempt = [&](double station, double offset) {
            const auto t = std::clamp(station, margin, 1-margin);
            const auto position = point_at(t) + normal_at(t)*offset;
            const auto bounds = footprint.translated(model_screen(position));
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !finite_rect(bounds)) return false;
            const auto padded = bounds.adjusted(-gap, -gap, gap, gap);
            if (obstacles.intersects(padded)) return false;
            label.position = position;
            obstacles.add(bounds);
            return true;
        };
        for (int lane = 0; lane < 16 && !placed; ++lane) {
            for (const auto station : {.5, .25, .75, .125, .875, .375, .625}) {
                if (attempt(station, baseline_distance+lane*lane_step)) { placed = true; break; }
            }
        }
        if (!placed) {
            // A dense plan must keep every measurement. Jump beyond every
            // obstacle in the midpoint normal, rather than unbounded retries.
            double offset_pixels = baseline_distance*scale;
            const auto origin = model_screen(midpoint);
            for (const auto& other : obstacles.rectangles())
                for (const auto corner : {other.topLeft(), other.topRight(), other.bottomLeft(), other.bottomRight()})
                    offset_pixels = std::max(offset_pixels,
                        QPointF::dotProduct(corner-origin, screen_normal)+outward_extent+gap+1);
            if (!attempt(.5, offset_pixels/scale)) {
                // Overflowing metadata is presentation-invalid; retain the
                // finite source anchor and reserve its footprint for others.
                const auto bounds = footprint.translated(model_screen(label.position));
                if (finite_rect(bounds)) obstacles.add(bounds);
            }
        }
    }
    cache.key = std::move(key);
    cache.retained_key = std::move(retained_key);
    cache.labels = std::move(labels);
    cache.paint_layouts = std::move(paint_layouts);
    return cache.labels;
}

QRectF PlanCanvas::selectionControlRect(const QRectF& viewport) const {
    if (const auto axes = selectionAxes()) {
        double padding = 7.5;
        for (const auto index : m_selected_entity_indices) {
            const auto& entity = m_entities[index];
            const auto width = wall_baseline_only(entity)
                ? std::max(entity.thickness_metres, .04) : entity.stroke_width_metres;
            if (std::isfinite(width) && width > 0) padding = 6.0 + width * m_scale * .5;
        }
        const bool label_selection = m_has_selected_label;
        const auto sx = label_selection || m_transform_preview_exact ? 1.0 : m_left_gesture == LeftGesture::selection_axis_resize
            ? m_axis_scale_x_preview : m_transform_scale_preview;
        const auto sy = label_selection || m_transform_preview_exact ? 1.0 : m_left_gesture == LeftGesture::selection_axis_resize
            ? m_axis_scale_y_preview : m_transform_scale_preview;
        const auto width = std::max(44.0, axes->width_metres*m_scale*sx + 2*padding);
        const auto depth = std::max(44.0, axes->depth_metres*m_scale*sy + 2*padding);
        return {-width*.5, -depth*.5, width, depth};
    }
    return selectionFrame(viewport).value_or(QRectF{});
}

QTransform PlanCanvas::selectionControlTransform(const QRectF& viewport) const {
    QTransform transform;
    if (auto axes = selectionAxes()) {
        if (m_transform_frame_start && !m_transform_preview_exact &&
            m_left_gesture == LeftGesture::selection_axis_resize) {
            const auto dx = axes->center.x-m_axis_anchor.x;
            const auto dy = axes->center.y-m_axis_anchor.y;
            const auto c = std::cos(m_axis_rotation), s = std::sin(m_axis_rotation);
            const auto x = (dx*c+dy*s)*m_axis_scale_x_preview;
            const auto y = (-dx*s+dy*c)*m_axis_scale_y_preview;
            axes->center = {m_axis_anchor.x+x*c-y*s, m_axis_anchor.y+x*s+y*c};
        } else if (m_transform_frame_start && !m_transform_preview_exact && m_left_gesture == LeftGesture::selection_rotate) {
            // Label axes already include their presented rotation.
            const bool label_selection = m_has_selected_label;
            if (!label_selection) axes->rotation_radians += m_transform_rotation_preview;
        }
        // Same-ID wall measurements are selected with their geometry. Only
        // label-only axes have already incorporated the move delta.
        const bool selected_label = m_selected_entity_indices.empty() &&
            !m_has_selected_reference && m_has_selected_label;
        if (!selected_label && m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(selectedIds().front())) {
            axes->center.x += m_move_preview_delta->x;
            axes->center.y += m_move_preview_delta->y;
        }
        const auto center = toScreen(axes->center, viewport);
        transform.translate(center.x(), center.y());
        transform.rotate(-axes->rotation_radians*180/pi);
        return transform;
    }
    if (m_transform_frame_start && !m_transform_preview_exact &&
        m_left_gesture == LeftGesture::selection_axis_resize) {
        const auto anchor = toScreen(m_axis_anchor, viewport);
        transform.translate(anchor.x(), anchor.y());
        transform.rotate(-m_axis_rotation*180/pi);
        transform.scale(m_axis_scale_x_preview, m_axis_scale_y_preview);
        transform.rotate(m_axis_rotation*180/pi);
        transform.translate(-anchor.x(), -anchor.y());
    } else if (m_transform_frame_start && !m_transform_preview_exact &&
               (m_left_gesture == LeftGesture::selection_resize ||
                m_left_gesture == LeftGesture::selection_rotate)) {
        transform.translate(m_transform_center.x(), m_transform_center.y());
        transform.rotate(-m_transform_rotation_preview*180/pi);
        transform.scale(m_transform_scale_preview, m_transform_scale_preview);
        transform.translate(-m_transform_center.x(), -m_transform_center.y());
    }
    return transform;
}

std::vector<QRectF> PlanCanvas::selectionAnnotationFootprints(
    const QRectF& viewport, const QFont& base_font) const {
    std::vector<QRectF> result;
    const auto& labels = positionedLabels(base_font, this, m_scale, logicalDpiY(), false);
    result.reserve(labels.size());
    for (const auto& label : labels) {
        if (!drawable_label(label)) continue;
        const auto label_font = std::isfinite(label.paper_height_mm) && label.paper_height_mm > 0.0
            ? font() : base_font;
        const auto layout = label_layout(label, label_font, this, m_scale, logicalDpiY());
        const auto center = toScreen(label.position, viewport);
        auto footprint = label_transform(label, center).mapRect(layout.bounds)
                             .adjusted(-2.0, -2.0, 2.0, 2.0);
        if (std::isfinite(footprint.left()) && std::isfinite(footprint.top()) &&
            std::isfinite(footprint.right()) && std::isfinite(footprint.bottom()) &&
            footprint.width() > 0 && footprint.height() > 0)
            result.push_back(footprint);
    }
    return result;
}

QPointF PlanCanvas::selectionHandlePoint(QPointF anchor, QPointF preferred_direction,
    const QRectF& viewport, const std::vector<QRectF>& annotation_footprints) const {
    constexpr qreal hit_radius = 12.0;
    const auto transform = selectionControlTransform(viewport);
    const auto available = viewport.adjusted(hit_radius, hit_radius, -hit_radius, -hit_radius);
    const auto clear = [&](QPointF local_point) {
        const auto center = transform.map(local_point);
        if (!available.contains(center)) return false;
        const QRectF hit(center.x() - hit_radius, center.y() - hit_radius,
                         hit_radius * 2.0, hit_radius * 2.0);
        return std::none_of(annotation_footprints.begin(), annotation_footprints.end(),
            [&](const QRectF& footprint) { return hit.intersects(footprint); });
    };
    if (clear(anchor)) return anchor;

    const auto length = std::hypot(preferred_direction.x(), preferred_direction.y());
    if (length > 1e-9) {
        preferred_direction /= length;
    } else {
        preferred_direction = {1.0, 0.0};
    }
    std::vector<QPointF> directions{preferred_direction};
    constexpr qreal diagonal = 0.7071067811865475244;
    const std::array<QPointF, 8> compass{{
        {1,0}, {diagonal,diagonal}, {0,1}, {-diagonal,diagonal},
        {-1,0}, {-diagonal,-diagonal}, {0,-1}, {diagonal,-diagonal}}};
    directions.insert(directions.end(), compass.begin(), compass.end());
    std::stable_sort(directions.begin() + 1, directions.end(), [&](QPointF left, QPointF right) {
        return QPointF::dotProduct(left, preferred_direction) >
               QPointF::dotProduct(right, preferred_direction);
    });
    const auto maximum_distance = 2.0 * std::hypot(viewport.width(), viewport.height()) + 48.0;
    for (const auto direction : directions) {
        bool entered_viewport = false;
        for (qreal distance = 16.0; distance <= maximum_distance; distance += 8.0) {
            const auto candidate = anchor + direction * distance;
            const auto center = transform.map(candidate);
            if (!available.contains(center)) {
                if (entered_viewport) break;
                continue;
            }
            entered_viewport = true;
            if (clear(candidate)) return candidate;
        }
    }
    // If annotations and viewport edges leave no clear position, retain the
    // original control location and its existing hit target.
    return anchor;
}

QPointF PlanCanvas::selectionRotationPoint(
    const QRectF& viewport, const std::vector<QRectF>& annotation_footprints) const {
    const auto frame = selectionControlRect(viewport);
    const auto transform = selectionControlTransform(viewport);
    constexpr qreal hit_radius = 12.0;
    const auto available = viewport.adjusted(hit_radius, hit_radius, -hit_radius, -hit_radius);
    const auto maximum_distance = std::hypot(viewport.width(), viewport.height()) +
                                  std::max(frame.width(), frame.height()) + 24.0;
    for (qreal distance = 24.0; distance <= maximum_distance; distance += 4.0) {
        const QPointF candidate(frame.center().x(), frame.top() - distance);
        const auto center = transform.map(candidate);
        if (!available.contains(center)) break;
        const QRectF hit(center.x() - hit_radius, center.y() - hit_radius,
                         hit_radius * 2.0, hit_radius * 2.0);
        if (std::any_of(annotation_footprints.begin(), annotation_footprints.end(),
                        [&](const QRectF& footprint) { return hit.intersects(footprint); }))
            continue;
        return candidate;
    }
    const auto inward_limit = std::min(frame.height() - hit_radius,
        2.0 * std::hypot(viewport.width(), viewport.height()) + 48.0);
    for (qreal distance = 24.0; distance <= inward_limit; distance += 4.0) {
        const QPointF candidate(frame.center().x(), frame.top() + distance);
        const auto center = transform.map(candidate);
        if (!available.contains(center)) continue;
        const QRectF hit(center.x() - hit_radius, center.y() - hit_radius,
                         hit_radius * 2.0, hit_radius * 2.0);
        if (std::any_of(annotation_footprints.begin(), annotation_footprints.end(),
                        [&](const QRectF& footprint) { return hit.intersects(footprint); }))
            continue;
        return candidate;
    }
    const QPointF outside(frame.center().x(), frame.top()-24);
    if (available.contains(transform.map(outside))) return outside;
    return {frame.center().x(), std::min(frame.bottom()-10, frame.top()+24)};
}

PlanCanvas::SelectionHandle PlanCanvas::selectionHandleAt(
    QPointF point, const QRectF& viewport) const {
    if (selectedOpening()) return SelectionHandle::none;
    if (selectedIds().size() != 1) return SelectionHandle::none;
    if (!selectionFrame(viewport)) return SelectionHandle::none;
    const auto frame = selectionControlRect(viewport);
    const auto transform = selectionControlTransform(viewport);
    const auto annotation_footprints = selectionAnnotationFootprints(viewport, font());
    constexpr qreal hit_size = 24.0;
    const auto hit = [&](QPointF center) {
        center = transform.map(center);
        return QRectF(center.x() - hit_size * 0.5, center.y() - hit_size * 0.5,
                      hit_size, hit_size).contains(point);
    };
    if (m_selection_rotate_enabled &&
        hit(selectionRotationPoint(viewport, annotation_footprints)))
        return SelectionHandle::rotate;
    // Resolve overlaps by proximity. Small objects can put a corner's touch
    // region over a side handle; the point actually nearest the pointer wins.
    SelectionHandle nearest = SelectionHandle::none;
    double distance = std::numeric_limits<double>::infinity();
    const auto consider = [&](QPointF center, SelectionHandle handle) {
        if (!hit(center)) return;
        const auto candidate = QLineF(point, transform.map(center)).length();
        if (candidate < distance) { distance = candidate; nearest = handle; }
    };
    if (m_selection_axis_resize_enabled && m_entity_axis_resize_requested) {
        if (const auto axes = selectionAxes()) {
            if (axes->width_metres > 1e-9) {
                consider(selectionHandlePoint({frame.left(), frame.center().y()}, {-1, 0},
                    viewport, annotation_footprints), SelectionHandle::left);
                consider(selectionHandlePoint({frame.right(), frame.center().y()}, {1, 0},
                    viewport, annotation_footprints), SelectionHandle::right);
            }
            if (axes->depth_metres > 1e-9) {
                consider(selectionHandlePoint({frame.center().x(), frame.top()}, {0, -1},
                    viewport, annotation_footprints), SelectionHandle::top);
                consider(selectionHandlePoint({frame.center().x(), frame.bottom()}, {0, 1},
                    viewport, annotation_footprints), SelectionHandle::bottom);
            }
        }
    }
    if (m_selection_resize_enabled) {
        const std::array<std::pair<QPointF, QPointF>, 4> corners{{
            {frame.topLeft(), {-1, -1}}, {frame.topRight(), {1, -1}},
            {frame.bottomLeft(), {-1, 1}}, {frame.bottomRight(), {1, 1}}}};
        for (const auto& [anchor, direction] : corners)
            consider(selectionHandlePoint(anchor, direction, viewport, annotation_footprints),
                     SelectionHandle::resize);
    }
    return nearest;
}

const CanvasEntity* PlanCanvas::selectedOpening() const {
    if (selectedIds().size() != 1) return nullptr;
    for (const auto index : m_selected_entity_indices) {
        const auto& entity = m_entities[index];
        if (entity.opening_width_controls) return &entity;
    }
    return nullptr;
}

void PlanCanvas::rebuildEntityPresentationIndex(EntityPresentationIndex& index,
    const std::vector<CanvasEntity>& entities) {
    index.clear();
    for (std::size_t position = 0; position < entities.size(); ++position) {
        const auto& entity = entities[position];
        auto& presentations = index[entity.id];
        // Anonymous duplicates are admitted by the existing proposal contract;
        // retain the same first match as the original ordered vector lookup.
        if (!presentations.contains(entity.presentation_key))
            presentations.insert(entity.presentation_key, position);
    }
}

const CanvasEntity* PlanCanvas::indexedPreviewEntity(const CanvasEntity& entity,
    const std::vector<CanvasEntity>& previews, const EntityPresentationIndex& index) {
    const auto root = index.constFind(entity.id);
    if (root == index.cend()) return nullptr;
    const auto presentation = root.value().constFind(entity.presentation_key);
    if (presentation == root.value().cend()) return nullptr;
    return &previews[presentation.value()];
}

const CanvasEntity& PlanCanvas::interactiveEntity(const CanvasEntity& entity) const {
    if (m_transform_preview_valid) {
        if (const auto* preview = indexedPreviewEntity(entity,
                m_transform_entities_preview, m_transform_entities_preview_index)) return *preview;
    }
    if (m_move_preview_valid) {
        if (const auto* preview = indexedPreviewEntity(entity,
                m_move_entities_preview, m_move_entities_preview_index)) return *preview;
    }
    if (m_vertex_move_handle && m_boundary_vertex_preview_valid) {
        if (const auto* preview = indexedPreviewEntity(entity,
                m_boundary_vertex_entities_preview, m_boundary_vertex_entities_preview_index)) return *preview;
    }
    if (m_opening_width_handle && m_opening_width_preview_valid) {
        if (const auto* preview = indexedPreviewEntity(entity,
                m_opening_width_entities_preview, m_opening_width_entities_preview_index)) return *preview;
    }
    return entity;
}

std::optional<PlanCanvas::OpeningWidthHandleHit> PlanCanvas::openingWidthHandleAt(
    QPointF point, const QRectF& viewport) const {
    const auto* entity = selectedOpening();
    if (!entity || !m_opening_width_preview_requested || !m_opening_width_resize_requested)
        return std::nullopt;
    const auto& controls = *entity->opening_width_controls;
    const auto length = std::hypot(controls.end_jamb.x - controls.start_jamb.x,
                                   controls.end_jamb.y - controls.start_jamb.y);
    if (!std::isfinite(controls.start_jamb.x) || !std::isfinite(controls.start_jamb.y) ||
        !std::isfinite(controls.end_jamb.x) || !std::isfinite(controls.end_jamb.y) ||
        (!controls.host_baseline && (!std::isfinite(length) || length <= 1e-6)) ||
        !std::isfinite(controls.width_metres) || controls.width_metres <= 0 ||
        !std::isfinite(controls.height_metres) || controls.height_metres <= 0)
        return std::nullopt;
    const auto start_distance = QLineF(point, toScreen(controls.start_jamb, viewport)).length();
    const auto end_distance = QLineF(point, toScreen(controls.end_jamb, viewport)).length();
    constexpr qreal hit_radius = 13.0;
    if (std::min(start_distance, end_distance) > hit_radius) return std::nullopt;
    return OpeningWidthHandleHit{entity->id, controls, end_distance < start_distance};
}

void PlanCanvas::updateOpeningWidthPreview(QPointF point, Qt::KeyboardModifiers modifiers) {
    if (!m_opening_width_handle) return;
    const auto serial = ++m_opening_width_preview_serial;
    const auto handle = *m_opening_width_handle;
    const auto& source = handle.source;
    m_opening_width_preview_valid = false;
    m_opening_width_preview_pending = false;
    m_opening_width_preview_request_in_progress = false;
    m_opening_width_preview_pointer = point;
    m_opening_width_preview_fine = modifiers.testFlag(Qt::ShiftModifier);
    m_opening_width_entities_preview.clear();
    m_opening_width_entities_preview_index.clear();
    m_opening_width_labels_preview.clear();
    m_opening_width_scale_preview = std::numeric_limits<double>::quiet_NaN();
    if (!m_opening_width_press_station || !m_opening_width_pointer_station) return;
    const auto host = source.host_baseline.value_or(Segment{source.start_jamb, source.end_jamb});
    double width{};
    try {
        const auto original_width = source.host_baseline ? source.width_metres : segment_length(host);
        const auto offset = source.host_baseline ? source.offset_metres : 0.0;
        // Unwrap around the last finite pointer station, including across the
        // atan2 branch cut. Subtract the press station to preserve halo grabs.
        const auto station = project_host_station(
            host, toModel(point, rect()), *m_opening_width_pointer_station);
        if (!std::isfinite(station)) return;
        m_opening_width_pointer_station = station;
        const auto displacement = station - *m_opening_width_press_station;
        width = original_width + (handle.keep_start_jamb ? displacement : -displacement);
        // Use the same zoom-relative construction lengths as wall drawing.
        // Preserve the captured width at zero displacement, including halo
        // grabs; reject a crossed jamb before rounding can change its sign.
        if (!std::isfinite(original_width) || original_width <= 1e-6 ||
            !std::isfinite(width)) return;
        if (width > 1e-6 && std::abs(displacement) > 1e-12 &&
            m_snap_enabled && !m_raw_point_input && !m_opening_width_preview_fine) {
            const auto step = drawingLengthIncrementMetres();
            if (!std::isfinite(step) || step <= 0.0) return;
            width = std::round(width / step) * step;
        }
        const auto moving_station = handle.keep_start_jamb
            ? offset + width : offset + original_width - width;
        const auto moving_jamb = point_at_host_station(host, moving_station);
        if (!std::isfinite(width) || !std::isfinite(moving_jamb.x) ||
            !std::isfinite(moving_jamb.y)) return;
        m_opening_width_scale_preview = width / original_width;
        if (!std::isfinite(m_opening_width_scale_preview)) return;
        m_opening_width_jamb_preview = moving_jamb;
        setCursor(jamb_resize_cursor(source, handle.keep_start_jamb, m_opening_width_scale_preview));
    } catch (const std::exception&) {
        return;
    }
    // Crossing the fixed jamb is invalid; do not silently clamp or fabricate
    // stretched opening artwork. Document validation owns further constraints.
    if (!std::isfinite(width) || width <= 1e-6 || !m_opening_width_preview_requested) return;
    std::optional<std::vector<CanvasEntity>> preview;
    m_opening_width_preview_request_in_progress = true;
    try {
        preview = m_opening_width_preview_requested(handle.entity_id,
            m_opening_width_scale_preview, handle.keep_start_jamb, source.source_revision);
    } catch (const std::exception&) {
        if (m_opening_width_preview_serial == serial) {
            m_opening_width_preview_request_in_progress = false;
            m_opening_width_preview_pending = false;
            m_opening_width_preview_valid = false;
            m_opening_width_entities_preview.clear();
            m_opening_width_entities_preview_index.clear();
            m_opening_width_labels_preview.clear();
        }
        return;
    }
    // A projection callback may synchronously replace the scene or selection.
    // Such replacement cancels the gesture; never revive its stale overrides.
    if (m_opening_width_preview_serial != serial || !m_opening_width_preview_request_in_progress)
        return;
    m_opening_width_preview_request_in_progress = false;
    if (!preview && m_opening_width_preview_pending) return;
    (void)applyOpeningWidthPreview(serial, std::move(preview));
}

bool PlanCanvas::markOpeningWidthPreviewPending(std::uint64_t serial) {
    if (serial != m_opening_width_preview_serial || !m_opening_width_preview_request_in_progress ||
        !m_opening_width_handle || m_left_gesture != LeftGesture::opening_width_resize ||
        !std::isfinite(m_opening_width_scale_preview) || m_opening_width_scale_preview <= 0.0)
        return false;
    m_opening_width_preview_pending = true;
    update();
    return true;
}

bool PlanCanvas::completeOpeningWidthPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels) {
    if (!m_opening_width_preview_pending) return false;
    return applyOpeningWidthPreview(serial, std::move(result), std::move(labels));
}

bool PlanCanvas::applyOpeningWidthPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels) {
    if (serial != m_opening_width_preview_serial || !m_opening_width_handle ||
        m_left_gesture != LeftGesture::opening_width_resize) return false;
    m_opening_width_preview_request_in_progress = false;
    m_opening_width_preview_pending = false;
    m_opening_width_preview_valid = false;
    m_opening_width_entities_preview.clear();
    m_opening_width_entities_preview_index.clear();
    m_opening_width_labels_preview.clear();
    // Exact projection must contain the captured opening. Further geometry
    // constraints belong to the document projection and final resize command.
    if (!result || !unambiguous_entity_presentations(*result, m_entities) || std::none_of(result->begin(), result->end(),
        [&](const CanvasEntity& entity) { return entity.id == m_opening_width_handle->entity_id; })) {
        update();
        return true;
    }
    std::vector<CanvasLabel> admitted_labels;
    admitted_labels.reserve(labels.size());
    const auto retained_labels = retained_label_presentation_selection(m_labels);
    QSet<QString> proposed_owner_ids;
    for (const auto& entity : *result) proposed_owner_ids.insert(entity.id);
    const auto finite_point = [](Vec2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    for (auto& label : labels) {
        const auto original = retained_labels.constFind({label.id, label.callout_role});
        if (original == retained_labels.cend()) {
            // New derived callouts must belong to this admitted projection,
            // including linked-view dimensions whose line and label share ID.
            if (!proposed_owner_ids.contains(label.id)) continue;
            label.selected = false;
        } else label.selected = original.value();
        // Empty text is an intentional clearing override, not drawable input.
        // Match the committed output's finite presentation requirements for
        // visible labels, including optional placement and fill values.
        bool finite = label.text.isEmpty() ||
            (finite_point(label.position) && std::isfinite(label.rotation_radians) &&
             std::isfinite(label.scale) && std::isfinite(label.text_height_metres) &&
             std::isfinite(label.paper_height_mm) &&
             (!label.leader_start || finite_point(*label.leader_start)) &&
             (!label.plan_label_offset || (finite_point(*label.plan_label_offset) &&
                 finite_point(label.position + *label.plan_label_offset))) &&
             (!label.fill_opacity || std::isfinite(*label.fill_opacity)));
        if (finite && !label.text.isEmpty() && label.automatic_linear_placement) {
            const auto& placement = *label.automatic_linear_placement;
            finite = finite_point(placement.anchor.start) && finite_point(placement.anchor.end) &&
                std::isfinite(placement.anchor.sweep_radians) &&
                finite_point(placement.outward_normal) && std::isfinite(placement.clearance_metres);
        }
        if (!finite) {
            update();
            return true;
        }
        admitted_labels.push_back(std::move(label));
    }
    const auto retained_entities = retained_entity_presentation_selection(m_entities);
    for (auto& entity : *result) {
        const auto original = retained_entities.constFind({entity.id, entity.presentation_key});
        if (original != retained_entities.cend()) entity.selected = original.value();
    }
    m_opening_width_entities_preview = std::move(*result);
    rebuildEntityPresentationIndex(m_opening_width_entities_preview_index, m_opening_width_entities_preview);
    m_opening_width_labels_preview = std::move(admitted_labels);
    m_opening_width_preview_valid = true;
    update();
    return true;
}

void PlanCanvas::drawOpeningWidthHandles(QPainter& painter, const QRectF& viewport) const {
    if (!selectionInteractionEnabled()) return;
    const auto* retained = selectedOpening();
    if (!retained || !m_opening_width_preview_requested || !m_opening_width_resize_requested) return;
    const auto& entity = interactiveEntity(*retained);
    if (!entity.opening_width_controls) return;
    const auto& controls = *entity.opening_width_controls;
    auto start = controls.start_jamb;
    auto end = controls.end_jamb;
    if (m_opening_width_handle && m_opening_width_jamb_preview) {
        if (m_opening_width_handle->keep_start_jamb) end = *m_opening_width_jamb_preview;
        else start = *m_opening_width_jamb_preview;
    }
    if (m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(entity.id)) {
        start.x += m_move_preview_delta->x; start.y += m_move_preview_delta->y;
        end.x += m_move_preview_delta->x; end.y += m_move_preview_delta->y;
    }
    if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y)) return;
    const bool invalid = m_opening_width_handle && m_left_dragging &&
                         !m_opening_width_preview_valid && !m_opening_width_preview_pending;
    const auto color = invalid ? QColor(220, 38, 38) : QColor(37, 99, 235);
    painter.save();
    painter.setClipRect(viewport, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.5));
    if (m_opening_width_handle && m_left_dragging) {
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(toScreen(start, viewport), toScreen(end, viewport));
    }
    painter.setBrush(QColor(255, 255, 255, 245));
    for (const auto jamb : {start, end}) {
        const auto screen = toScreen(jamb, viewport);
        painter.drawRect(QRectF(screen.x() - 5.0, screen.y() - 5.0, 10.0, 10.0));
    }
    painter.restore();
}

void PlanCanvas::updateBoundaryVertexPreview(QPointF point) {
    if (!m_vertex_move_handle) return;
    const auto serial = ++m_boundary_vertex_preview_serial;
    const auto handle = *m_vertex_move_handle;
    m_boundary_vertex_preview_pointer = point;
    const auto pointer = toModel(point, rect());
    const auto target = m_vertex_move_press_pointer
        ? handle.source_position + (pointer - *m_vertex_move_press_pointer) : pointer;
    // The hit area is deliberately larger than the painted grip. Keep that
    // press offset during motion, then apply the normal model/grid snapping.
    m_vertex_move_preview = inputPoint(toScreen(target, rect()));
    m_boundary_vertex_preview_valid = false;
    m_boundary_vertex_preview_pending = false;
    m_boundary_vertex_preview_request_in_progress = false;
    m_boundary_vertex_entities_preview.clear();
    m_boundary_vertex_entities_preview_index.clear();
    m_boundary_vertex_labels_preview.clear();
    m_boundary_vertex_metrics_preview.reset();
    const auto snapped_target = *m_vertex_move_preview;
    if (!std::isfinite(snapped_target.x) || !std::isfinite(snapped_target.y) ||
        !m_boundary_vertex_preview_requested) return;
    std::optional<std::vector<CanvasEntity>> preview;
    m_boundary_vertex_preview_request_in_progress = true;
    try {
        preview = m_boundary_vertex_preview_requested(handle.entity_id, handle.vertex_id,
                                                      snapped_target, handle.source_revision);
    } catch (const std::exception&) {
        if (m_boundary_vertex_preview_serial == serial) {
            m_boundary_vertex_preview_request_in_progress = false;
            m_boundary_vertex_preview_pending = false;
            m_boundary_vertex_preview_valid = false;
            m_boundary_vertex_entities_preview.clear();
            m_boundary_vertex_entities_preview_index.clear();
            m_boundary_vertex_labels_preview.clear();
            m_boundary_vertex_metrics_preview.reset();
        }
        return;
    }
    // A callback can synchronously replace the scene, selection, or callback,
    // or even complete its pending request. Never revive canceled geometry.
    if (m_boundary_vertex_preview_serial != serial ||
        !m_boundary_vertex_preview_request_in_progress) return;
    m_boundary_vertex_preview_request_in_progress = false;
    if (!preview && m_boundary_vertex_preview_pending) return;
    (void)applyBoundaryVertexPreview(serial, std::move(preview));
}

bool PlanCanvas::markBoundaryVertexPreviewPending(std::uint64_t serial) {
    if (serial != m_boundary_vertex_preview_serial ||
        !m_boundary_vertex_preview_request_in_progress || !m_vertex_move_handle ||
        m_left_gesture != LeftGesture::vertex_move || !m_vertex_move_preview ||
        !std::isfinite(m_vertex_move_preview->x) || !std::isfinite(m_vertex_move_preview->y))
        return false;
    m_boundary_vertex_preview_pending = true;
    update();
    return true;
}

bool PlanCanvas::completeBoundaryVertexPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels,
    std::optional<CanvasBoundaryPreviewMetrics> metrics) {
    if (!m_boundary_vertex_preview_pending) return false;
    return applyBoundaryVertexPreview(serial, std::move(result), std::move(labels), metrics);
}

bool PlanCanvas::applyBoundaryVertexPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels,
    std::optional<CanvasBoundaryPreviewMetrics> metrics) {
    if (serial != m_boundary_vertex_preview_serial || !m_vertex_move_handle ||
        m_left_gesture != LeftGesture::vertex_move) return false;
    m_boundary_vertex_preview_request_in_progress = false;
    m_boundary_vertex_preview_pending = false;
    m_boundary_vertex_preview_valid = false;
    m_boundary_vertex_entities_preview.clear();
    m_boundary_vertex_entities_preview_index.clear();
    m_boundary_vertex_labels_preview.clear();
    m_boundary_vertex_metrics_preview.reset();
    // Native projection owns geometric validation. An absent captured owner
    // cannot be a preview of this edit and is treated as a known invalid result.
    const bool invalid_metrics = metrics &&
        (!std::isfinite(metrics->area_square_metres) || metrics->area_square_metres < 0 ||
         !std::isfinite(metrics->perimeter_metres) || metrics->perimeter_metres < 0 ||
         (metrics->length_metres && (!std::isfinite(*metrics->length_metres) || *metrics->length_metres <= 0)));
    if (invalid_metrics || !result || !unambiguous_entity_presentations(*result, m_entities) || std::none_of(result->begin(), result->end(),
        [&](const CanvasEntity& entity) { return entity.id == m_vertex_move_handle->entity_id; })) {
        update();
        if (m_vertex_release_pending) QTimer::singleShot(0,this,[this,serial]{finishBoundaryVertexPreview(serial);});
        return true;
    }
    const auto retained_entities = retained_entity_presentation_selection(m_entities);
    const auto retained_labels = retained_label_presentation_selection(m_labels);
    QSet<QString> retained_owner_ids;
    for (const auto& entity : m_entities) retained_owner_ids.insert(entity.id);
    QSet<QString> proposed_view_dimension_ids;
    for (const auto& entity : *result)
        if (entity.type == QStringLiteral("section_overlay") && entity.dimension_end_ticks)
            proposed_view_dimension_ids.insert(entity.id);
    for (auto& entity : *result) {
        const auto original = retained_entities.constFind({entity.id, entity.presentation_key});
        if (original != retained_entities.cend()) entity.selected = original.value();
    }
    for (auto& label : labels) {
        const auto original = retained_labels.constFind({label.id, label.callout_role});
        if (original == retained_labels.cend()) {
            // A repaired area may acquire its first qualified quantity. Only
            // Derived area labels belong to retained visible owners. A bound
            // view dimension may newly resolve during the proposal; admit its
            // value only alongside that exact proposed dimension line.
            const bool area_label = label.plan_only && label.avoid_components &&
                retained_owner_ids.contains(label.id);
            const bool view_dimension = label.callout_role.isEmpty() &&
                proposed_view_dimension_ids.contains(label.id);
            if (!area_label && !view_dimension) continue;
            label.selected=false;
        } else label.selected = original.value();
        m_boundary_vertex_labels_preview.push_back(std::move(label));
    }
    m_boundary_vertex_entities_preview = std::move(*result);
    rebuildEntityPresentationIndex(m_boundary_vertex_entities_preview_index, m_boundary_vertex_entities_preview);
    m_boundary_vertex_metrics_preview = metrics;
    m_boundary_vertex_preview_valid = true;
    update();
    if (m_vertex_release_pending) QTimer::singleShot(0,this,[this,serial]{finishBoundaryVertexPreview(serial);});
    return true;
}

void PlanCanvas::finishBoundaryVertexPreview(std::uint64_t serial) {
    if (serial!=m_boundary_vertex_preview_serial || !m_vertex_release_pending || !m_vertex_move_handle) return;
    const auto handle=*m_vertex_move_handle;
    const auto target=m_vertex_move_preview;
    const bool accepted=m_boundary_vertex_preview_valid && target &&
        std::isfinite(target->x) && std::isfinite(target->y) &&
        (target->x!=handle.source_position.x || target->y!=handle.source_position.y);
    const auto callback=m_boundary_vertex_move_requested;
    resetGesture();update();
    if (accepted && callback) (void)callback(handle.entity_id,handle.vertex_id,*target,handle.source_revision);
}

std::optional<PlanCanvas::VertexHandleHit> PlanCanvas::vertexHandleAt(
    QPointF point, const QRectF& viewport) const {
    if (selectedOpening()) return std::nullopt;
    if (selectedIds().size() != 1) return std::nullopt;
    constexpr qreal hit_radius = 13.0;
    for (const auto& entity : m_entities) {
        if (!entity.selected || entity.vertex_handles.empty()) continue;
        for (const auto& handle : entity.vertex_handles) {
            const auto screen = toScreen(handle.position, viewport);
            if (QLineF(point, screen).length() <= hit_radius) {
                return VertexHandleHit{entity.id, handle.id, handle.position,
                                       handle.source_revision};
            }
        }
    }
    return std::nullopt;
}

void PlanCanvas::drawVertexHandles(QPainter& painter, const QRectF& viewport) const {
    if (selectedOpening()) return;
    if (!selectionInteractionEnabled() || selectedIds().size() != 1) return;
    painter.save();
    painter.setClipRect(viewport, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool editing = m_vertex_move_handle && m_left_dragging &&
                         m_boundary_vertex_preview_requested;
    const bool pending = editing && m_boundary_vertex_preview_pending;
    const bool invalid = editing && !m_boundary_vertex_preview_valid && !pending;
    const auto feedback_color = invalid ? QColor(220,38,38)
        : pending ? QColor(180,110,10) : QColor(37,99,235);
    painter.setBrush(QColor(255, 255, 255, 245));
    for (const auto& retained : m_entities) {
        if (!retained.selected) continue;
        const auto& entity = interactiveEntity(retained);
        for (const auto& handle : entity.vertex_handles) {
            auto position = handle.position;
            const bool moving = m_vertex_move_handle && m_vertex_move_preview &&
                m_vertex_move_handle->entity_id == entity.id &&
                m_vertex_move_handle->vertex_id == handle.id;
            if (moving && !m_boundary_vertex_preview_valid) {
                position = *m_vertex_move_preview;
            }
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) continue;
            painter.setPen(QPen(moving ? feedback_color : QColor(37,99,235), 1.5));
            const auto screen = toScreen(position, viewport);
            painter.drawEllipse(screen, 5.5, 5.5);
            if (moving && (pending || invalid)) {
                painter.setPen(QPen(feedback_color, 1.5, Qt::DashLine));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(screen, 9.0, 9.0);
                painter.setBrush(QColor(255,255,255,245));
            }
        }
    }
    if (editing && m_vertex_move_preview && std::isfinite(m_vertex_move_preview->x) &&
        std::isfinite(m_vertex_move_preview->y)) {
        auto readout_font = font();
        readout_font.setPixelSize(11);
        readout_font.setWeight(QFont::Medium);
        painter.setFont(readout_font);
        auto text = QStringLiteral("X %1  ·  Y %2")
            .arg(display_cursor_length(m_vertex_move_preview->x,m_metric_units),
                 display_cursor_length(m_vertex_move_preview->y,m_metric_units));
        if (pending) text += QStringLiteral("  ·  Checking");
        else if (invalid) text += QStringLiteral("  ·  Invalid");
        else if (m_boundary_vertex_preview_valid && m_boundary_vertex_metrics_preview) {
            const auto& totals = *m_boundary_vertex_metrics_preview;
            if (totals.length_metres) {
                text += QStringLiteral("\nLength %1").arg(display_cursor_length(*totals.length_metres,m_metric_units));
            } else {
                constexpr double metres_per_foot = .3048;
                const auto area = m_metric_units
                    ? QStringLiteral("%1 m²").arg(totals.area_square_metres,0,'f',2)
                    : QStringLiteral("%1 ft²").arg(totals.area_square_metres /
                        (metres_per_foot*metres_per_foot),0,'f',2);
                text += QStringLiteral("\nArea %1  ·  Perimeter %2")
                    .arg(area,display_cursor_length(totals.perimeter_metres,m_metric_units));
            }
        }
        const QFontMetricsF metrics(readout_font,painter.device());
        auto panel = metrics.boundingRect(QRectF(0,0,1000,1000),Qt::AlignCenter,text)
            .adjusted(-7,-4,7,4);
        panel.moveCenter(toScreen(*m_vertex_move_preview,viewport) + QPointF(0,25));
        panel.moveLeft(std::clamp(panel.left(),viewport.left()+4,
            std::max(viewport.left()+4,viewport.right()-panel.width()-4)));
        panel.moveTop(std::clamp(panel.top(),viewport.top()+4,
            std::max(viewport.top()+4,viewport.bottom()-panel.height()-4)));
        painter.setPen(QPen(feedback_color,1));
        painter.setBrush(invalid ? QColor(254,226,226,244)
                         : pending ? QColor(255,247,221,244) : QColor(239,246,255,244));
        painter.drawRoundedRect(panel,4,4);
        painter.setPen(feedback_color);
        painter.drawText(panel,Qt::AlignCenter,text);
    }
    painter.restore();
}

void PlanCanvas::drawSelectionFrame(QPainter& painter, const QRectF& viewport,
                                    const std::vector<QRectF>& annotation_footprints) const {
    if (!selectionFrame(viewport)) return;
    const auto frame = selectionControlRect(viewport);
    const auto transform = selectionControlTransform(viewport);
    const auto polygon = transform.map(QPolygonF(frame));
    painter.save();
    painter.setClipRect(viewport, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath visible_frame;
    visible_frame.addRect(viewport);
    QPainterPath annotation_mask;
    annotation_mask.setFillRule(Qt::WindingFill);
    for (const auto& footprint : annotation_footprints)
        annotation_mask.addRect(footprint);
    if (!annotation_mask.isEmpty())
        visible_frame = visible_frame.subtracted(annotation_mask);

    const bool controls = selectedIds().size() == 1 && !selectedOpening() &&
        (m_selection_resize_enabled || m_selection_rotate_enabled || m_selection_axis_resize_enabled);
    painter.save();
    painter.setClipPath(visible_frame, Qt::IntersectClip);
    painter.setBrush(Qt::NoBrush);
    // A white halo keeps the blue frame legible over dark fills and underlays.
    painter.setPen(QPen(QColor(255, 255, 255, 235), 4.0));
    painter.drawPolygon(polygon);
    painter.setPen(QPen(QColor(37, 99, 235), 1.5));
    painter.drawPolygon(polygon);
    if (controls && m_selection_rotate_enabled) {
        const auto handle = transform.map(selectionRotationPoint(viewport, annotation_footprints));
        painter.drawLine(transform.map(QPointF(frame.center().x(), frame.top())), handle);
    }
    painter.restore();

    if (controls) {
        painter.save();
        painter.setClipPath(visible_frame, Qt::IntersectClip);
        const auto draw_link = [&](QPointF anchor, QPointF point) {
            if (QLineF(anchor, point).length() <= 1.0) return;
            QPen link_pen(QColor(37, 99, 235, 115), 1.0, Qt::DashLine);
            link_pen.setCosmetic(true);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(link_pen);
            painter.drawLine(transform.map(anchor), transform.map(point));
        };
        if (m_selection_resize_enabled) {
            const std::array<std::pair<QPointF, QPointF>, 4> corners{{
                {frame.topLeft(), {-1, -1}}, {frame.topRight(), {1, -1}},
                {frame.bottomLeft(), {-1, 1}}, {frame.bottomRight(), {1, 1}}}};
            std::array<QPointF, 4> marker_points{};
            for (std::size_t index = 0; index < corners.size(); ++index) {
                const auto& [anchor, direction] = corners[index];
                marker_points[index] = selectionHandlePoint(
                    anchor, direction, viewport, annotation_footprints);
                draw_link(anchor, marker_points[index]);
            }
            painter.setBrush(QColor(255, 255, 255));
            painter.setPen(QPen(QColor(37, 99, 235), 1.5));
            for (const auto marker : marker_points) {
                const auto point = transform.map(marker);
                painter.drawRect(QRectF(point.x() - 4.0, point.y() - 4.0, 8.0, 8.0));
            }
        }
        if (m_selection_axis_resize_enabled && m_entity_axis_resize_requested) {
            if (const auto axes = selectionAxes()) {
                std::vector<std::pair<QPointF, QPointF>> axis_handles;
                axis_handles.reserve(4);
                if (axes->width_metres > 1e-9) {
                    axis_handles.emplace_back(QPointF(frame.left(),frame.center().y()), QPointF(-1,0));
                    axis_handles.emplace_back(QPointF(frame.right(),frame.center().y()), QPointF(1,0));
                }
                if (axes->depth_metres > 1e-9) {
                    axis_handles.emplace_back(QPointF(frame.center().x(),frame.top()), QPointF(0,-1));
                    axis_handles.emplace_back(QPointF(frame.center().x(),frame.bottom()), QPointF(0,1));
                }
                std::vector<QPointF> marker_points;
                marker_points.reserve(axis_handles.size());
                for (const auto& [anchor, direction] : axis_handles) {
                    marker_points.push_back(selectionHandlePoint(
                        anchor, direction, viewport, annotation_footprints));
                    draw_link(anchor, marker_points.back());
                }
                painter.setBrush(QColor(255, 255, 255));
                painter.setPen(QPen(QColor(37, 99, 235), 1.5));
                for (const auto marker : marker_points) {
                    const auto point = transform.map(marker);
                    painter.drawRect(QRectF(point.x()-4.5, point.y()-4.5, 9, 9));
                }
            }
        }
        if (m_selection_rotate_enabled) {
            painter.setBrush(QColor(255, 255, 255));
            painter.setPen(QPen(QColor(37, 99, 235), 1.5));
            const auto handle = transform.map(selectionRotationPoint(viewport, annotation_footprints));
            painter.drawEllipse(handle, 5.0, 5.0);
        }
        painter.restore();
    }
    painter.restore();
}

void PlanCanvas::drawSelectionDimensions(QPainter& painter, const QRectF& viewport,
    const std::vector<QRectF>& annotation_footprints) const {
    // The vertex readout supplies live coordinates, area, and perimeter. Avoid
    // obscuring it with a second bounding-box readout during the same gesture.
    if (m_left_gesture == LeftGesture::vertex_move && m_left_dragging) return;
    painter.save();
    painter.setClipRect(viewport, Qt::IntersectClip);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    auto dimension_font = font();
    dimension_font.setPixelSize(11);
    dimension_font.setWeight(QFont::Medium);
    painter.setFont(dimension_font);
    const QFontMetricsF metrics(dimension_font, painter.device());
    std::vector<QRectF> occupied_panels;
    const auto draw = [&](CanvasSelectionFrame axes, const QString& id, bool sizes_presented = false,
                          std::optional<CanvasOpeningWidthControls> opening = std::nullopt) {
        if (!std::isfinite(axes.center.x) || !std::isfinite(axes.center.y) ||
            !std::isfinite(axes.rotation_radians) || !std::isfinite(axes.width_metres) ||
            !std::isfinite(axes.depth_metres) || axes.width_metres < 0 || axes.depth_metres < 0)
            return;
        auto width = axes.width_metres;
        auto depth = axes.depth_metres;
        auto center = axes.center;
        const bool transforming = m_transform_frame_start && m_move_ids.contains(id);
        if (transforming && !m_transform_preview_exact) {
            if (m_left_gesture == LeftGesture::selection_axis_resize) {
                width *= m_axis_scale_x_preview;
                depth *= m_axis_scale_y_preview;
                const auto dx = center.x-m_axis_anchor.x;
                const auto dy = center.y-m_axis_anchor.y;
                const auto c = std::cos(m_axis_rotation), s = std::sin(m_axis_rotation);
                const auto x = (dx*c+dy*s)*m_axis_scale_x_preview;
                const auto y = (-dx*s+dy*c)*m_axis_scale_y_preview;
                center = {m_axis_anchor.x+x*c-y*s, m_axis_anchor.y+x*s+y*c};
            } else {
                if (!sizes_presented) {
                    width *= m_transform_scale_preview;
                    depth *= m_transform_scale_preview;
                    axes.rotation_radians += m_transform_rotation_preview;
                }
            }
        }
        if (!sizes_presented && m_move_preview_delta && !m_move_preview_exact && m_move_ids.contains(id)) {
            center.x += m_move_preview_delta->x;
            center.y += m_move_preview_delta->y;
        }
        const bool editing_opening = opening && m_opening_width_handle &&
                                     m_opening_width_handle->entity_id == id;
        const bool moving_opening=opening && m_opening_move_active && m_left_dragging &&
            m_move_ids==QStringList{id};
        const bool invalid_opening = (editing_opening && m_left_dragging &&
                                     !m_opening_width_preview_valid && !m_opening_width_preview_pending) ||
            (moving_opening && !m_move_preview_valid && !m_move_preview_pending);
        const auto dimension_width = opening ? opening->width_metres *
            (editing_opening ? m_opening_width_scale_preview : 1.0) : width;
        const auto dimension_depth = opening ? opening->height_metres : depth;
        const bool curved_opening = opening && opening->host_baseline &&
                                    opening->host_baseline->sweep_radians != 0;
        QString text = (curved_opening ? QStringLiteral("Arc W %1  ×  H %2")
                       : opening ? QStringLiteral("W %1  ×  H %2") : QStringLiteral("W %1  ×  D %2"))
            .arg(drawingLengthText(dimension_width, m_metric_units),
                 drawingLengthText(dimension_depth, m_metric_units));
        if (moving_opening && m_opening_move_preview_intent) {
            text+=QStringLiteral("  ·  Along wall %1").arg(
                drawingLengthText(m_opening_move_preview_intent->offset_metres,m_metric_units));
            if (m_move_preview_pending) text+=QStringLiteral("  ·  Checking");
        }
        if (invalid_opening) text += QStringLiteral("  ·  Invalid");
        if (transforming && m_transform_preview_exact) {
            if (m_transform_preview_pending) text += QStringLiteral("  ·  Checking");
            else if (!m_transform_preview_valid) text += QStringLiteral("  ·  Invalid");
        }
        if (transforming && m_left_gesture == LeftGesture::selection_rotate) {
            const auto radians = m_transform_source_rotation
                ? *m_transform_source_rotation + m_transform_rotation_preview * m_transform_source_rotation_direction
                : m_transform_preview_exact
                ? m_transform_initial_rotation + m_transform_rotation_preview : axes.rotation_radians;
            auto degrees = std::fmod(radians * 180/pi, 360.0);
            if (degrees < 0) degrees += 360;
            if (std::abs(degrees-360) < 1e-6 || std::abs(degrees) < 1e-6) degrees = 0;
            text += QStringLiteral("  ·  %1°").arg(degrees, 0, 'f', 1);
        }
        const auto c = std::cos(axes.rotation_radians), s = std::sin(axes.rotation_radians);
        // Keep the upright callout outside the screen extent at every angle.
        const auto middle = toScreen(center, viewport);
        const auto lower = middle + QPointF(0,
            (std::abs(s)*width + std::abs(c)*depth)*m_scale*.5);
        auto panel = metrics.boundingRect(text).adjusted(-7,-4,7,4);
        const auto preferred = lower + QPointF(0, 28);
        panel.moveCenter(preferred);
        const auto inner = viewport.adjusted(4, 4, -4, -4);
        std::vector<QPointF> candidates;
        candidates.reserve(annotation_footprints.size() * 4 + 65);
        candidates.push_back(preferred);
        for (const auto& footprint : annotation_footprints) {
            candidates.push_back({footprint.left() - panel.width() * .5 - 4,
                                  footprint.center().y()});
            candidates.push_back({footprint.right() + panel.width() * .5 + 4,
                                  footprint.center().y()});
            candidates.push_back({footprint.center().x(),
                                  footprint.top() - panel.height() * .5 - 4});
            candidates.push_back({footprint.center().x(),
                                  footprint.bottom() + panel.height() * .5 + 4});
        }
        for (int row = 0; row <= 8; ++row) {
            for (int column = 0; column <= 8; ++column) {
                candidates.push_back({inner.left() + inner.width() * column / 8.0,
                                      inner.top() + inner.height() * row / 8.0});
            }
        }
        const auto bounded_panel = [&](QPointF center) {
            const auto minimum_x = inner.left() + panel.width() * .5;
            const auto maximum_x = inner.right() - panel.width() * .5;
            const auto minimum_y = inner.top() + panel.height() * .5;
            const auto maximum_y = inner.bottom() - panel.height() * .5;
            center.setX(minimum_x <= maximum_x
                ? std::clamp(center.x(), minimum_x, maximum_x) : inner.center().x());
            center.setY(minimum_y <= maximum_y
                ? std::clamp(center.y(), minimum_y, maximum_y) : inner.center().y());
            auto candidate = panel;
            candidate.moveCenter(center);
            return candidate;
        };
        std::sort(candidates.begin(), candidates.end(), [&](QPointF left, QPointF right) {
            const auto bounded_left = bounded_panel(left).center();
            const auto bounded_right = bounded_panel(right).center();
            return QLineF(bounded_left, preferred).length() <
                   QLineF(bounded_right, preferred).length();
        });
        std::optional<QRectF> chosen_panel;
        qreal least_overlap = std::numeric_limits<qreal>::infinity();
        for (const auto candidate_center : candidates) {
            const auto candidate = bounded_panel(candidate_center);
            if (!inner.contains(candidate) ||
                std::any_of(occupied_panels.begin(), occupied_panels.end(),
                            [&](const QRectF& occupied) { return candidate.intersects(occupied); }))
                continue;
            qreal overlap = 0;
            for (const auto& footprint : annotation_footprints) {
                const auto intersection = candidate.intersected(footprint);
                overlap += intersection.width() * intersection.height();
            }
            if (overlap < least_overlap) {
                least_overlap = overlap;
                chosen_panel = candidate;
            }
            if (overlap == 0) break;
        }
        if (chosen_panel) {
            panel = *chosen_panel;
            occupied_panels.push_back(panel);
        } else {
            // Very small viewports cannot contain even the compact callout.
            // Preserve the legacy edge placement so the measurement remains
            // available while the view is zoomed or resized.
            panel.moveLeft(std::clamp(panel.left(), viewport.left()+4,
                std::max(viewport.left()+4, viewport.right()-panel.width()-4)));
            panel.moveTop(std::clamp(panel.top(), viewport.top()+4,
                std::max(viewport.top()+4, viewport.bottom()-panel.height()-4)));
        }
        const bool dark = m_canvas_background.lightnessF() < .45;
        painter.setPen(QPen(invalid_opening ? QColor(220,38,38)
                           : dark ? QColor(125,179,255) : QColor(37,99,235),1));
        painter.setBrush(invalid_opening ? QColor(254,226,226,244)
                         : dark ? QColor(27,52,87,238) : QColor(239,246,255,244));
        painter.drawRoundedRect(panel,4,4);
        painter.setPen(invalid_opening ? QColor(185,28,28)
                       : dark ? QColor(223,235,255) : QColor(29,78,216));
        painter.drawText(panel,Qt::AlignCenter,text);
    };
    QSet<QString> measured_entities;
    for (const auto& entity : m_entities) {
        // A dimension guide is presentation geometry. Its selection extent is
        // not another building measurement and must not masquerade as one.
        if (entity.selected && entity.type != QStringLiteral("dimension_line") &&
            !measured_entities.contains(entity.id)) {
            measured_entities.insert(entity.id);
            const auto axes = !entity.presentation_key.isEmpty() && selectedIds().size()==1
                ? selectionAxes() : entitySelectionAxes(interactiveEntity(entity));
            if (axes)
                draw(*axes,entity.id,false,m_move_preview_valid
                    ? interactiveEntity(entity).opening_width_controls : entity.opening_width_controls);
        }
    }
    for (const auto& retained : m_references) {
        const auto& reference = interactiveReference(retained);
        if (!reference.selected || !reference.visible || reference.image.isNull()) continue;
        const auto unit = reference.metres_per_source_unit*reference.scale;
        if (!std::isfinite(unit) || unit <= 0) continue;
        const auto position = m_move_preview_delta && &reference == &retained && m_move_preview_valid && m_move_ids.contains(reference.id)
            ? reference.position + *m_move_preview_delta : reference.position;
        draw({position, reference.rotation_degrees*pi/180,
              reference.image.width()*unit, reference.image.height()*unit},reference.id);
    }
    for (const auto& label : positionedLabels(font(), this, m_scale, logicalDpiY(), false)) {
        if (!label.selected || !drawable_label(label) || label.selection_type==QStringLiteral("dimension")) continue;
        const auto layout = label_layout(label,font(),this,m_scale,logicalDpiY());
        draw(label_selection_frame(label, layout.bounds, m_scale),label.id,true);
    }
    painter.restore();
}

void PlanCanvas::drawSelectionCaption(QPainter& painter, const QRectF& viewport,
                                      QColor background) const {
    const auto caption=m_area_class_caption.isEmpty()?m_selection_caption:m_area_class_caption;
    if (caption.isEmpty() || viewport.width() < 100.0 || viewport.height() < 60.0)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto caption_font = font();
    caption_font.setPointSizeF(std::max(8.0, caption_font.pointSizeF() - 1.0));
    caption_font.setWeight(QFont::DemiBold);
    painter.setFont(caption_font);
    const QFontMetricsF metrics(caption_font);
    const auto maximum_width = std::max<qreal>(72.0, viewport.width() * 0.42);
    const auto text = metrics.elidedText(caption, Qt::ElideRight,
                                         qFloor(maximum_width - 20.0));
    auto badge = metrics.boundingRect(text).adjusted(-10.0, -5.0, 10.0, 5.0);
    badge.moveTopRight(viewport.topRight() + QPointF(-12.0, 12.0));
    const bool dark = background.lightnessF() < 0.45;
    painter.setPen(QPen(dark ? QColor(125, 179, 255) : QColor(37, 99, 235), 1.0));
    painter.setBrush(dark ? QColor(27, 52, 87, 238) : QColor(239, 246, 255, 244));
    painter.drawRoundedRect(badge, 8.0, 8.0);
    painter.setPen(dark ? QColor(223, 235, 255) : QColor(29, 78, 216));
    painter.drawText(badge, Qt::AlignCenter, text);
    painter.restore();
}

void PlanCanvas::paintEvent(QPaintEvent* event) {
    capture_diagnostic_stage("paint.enter");
    Q_UNUSED(event);
    const auto pending = std::exchange(m_pending_measurements, {});
    const auto generation = m_measurement_generation;
    {
        QPainter painter(this);
        renderSceneWithTransform(painter, QRectF(rect()), false, m_canvas_background,
                                 std::nullopt, std::nullopt, std::nullopt, false,
                                 SceneLayer::screen_with_floor_ghost);
        capture_diagnostic_stage("paint.render_scene.end");
        if (const auto frame = sketchCompositionGuideRect()) {
            painter.save();
            painter.setClipRect(rect());
            painter.setPen(QPen(m_canvas_background.lightnessF() > .5
                ? QColor(117, 133, 156, 190) : QColor(173, 188, 208, 190), 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(*frame);
            painter.restore();
        }
        if (m_selection_start && m_selection_dragging) {
            painter.setPen(QPen(QColor(37, 99, 235), 1.5, Qt::DashLine));
            painter.setBrush(QColor(37, 99, 235, 38));
            painter.drawRect(QRectF(*m_selection_start, m_selection_end).normalized());
        }
        if (m_last_mouse_position) {
            if (const auto anchor = closingAnchor(*m_last_mouse_position)) {
                const auto screen = toScreen(*anchor, rect());
                painter.setPen(QPen(QColor(22, 163, 74), 2.0));
                painter.setBrush(QColor(22, 163, 74, 60));
                painter.drawEllipse(screen, 10.0, 10.0);
                painter.drawText(screen + QPointF(14.0, -12.0), tr("Click to close"));
            }
        }
    }
    const auto completed = PerformanceClock::now();
    capture_diagnostic_stage("paint.end");
    const auto callback = m_performance_measured;
    if (!callback || !isVisible() || QApplication::activeModalWidget()) return;
    for (const auto& [metric, started] : pending) {
        // A reset from a callback must also discard the remainder of this batch.
        if (generation != m_measurement_generation) break;
        callback(metric, completed - started);
    }
}

void PlanCanvas::mousePressEvent(QMouseEvent* event) {
    retireDisconnectedTablet();
    if (event->source() != Qt::MouseEventNotSynthesized || m_touch_active || m_tablet_active) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerPress(event->position(), event->button(), event->modifiers());
    event->accept();
}

void PlanCanvas::mouseMoveEvent(QMouseEvent* event) {
    retireDisconnectedTablet();
    if (event->source() != Qt::MouseEventNotSynthesized || m_touch_active || m_tablet_active) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerMove(event->position(), event->modifiers());
    event->accept();
}

void PlanCanvas::mouseReleaseEvent(QMouseEvent* event) {
    retireDisconnectedTablet();
    if (event->source() != Qt::MouseEventNotSynthesized || m_touch_active || m_tablet_active) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerRelease(event->position(), event->button(), event->modifiers());
    event->accept();
}

void PlanCanvas::wheelEvent(QWheelEvent* event) {
    m_pending_dimension_space_tap.reset();
    beginPerformanceMeasurement(PerformanceMetric::input);
    // Prefer angular deltas when both representations are supplied. Pixel-only
    // scrolling uses the same continuous detent scale in logical coordinates.
    const auto delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    const auto steps = static_cast<double>(delta) / 120.0;
    if (steps != 0.0) {
        zoomBy(std::pow(1.18, steps), event->position());
    }
    event->accept();
}

void PlanCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() != Qt::Key_Space) m_pending_dimension_space_tap.reset();
    if (hasFocus() && event->modifiers() == Qt::NoModifier && !event->isAutoRepeat() &&
        drawingCommandIdle() && !m_point_placement_requested && selectedIds().isEmpty() &&
        (m_tool == CanvasTool::select || m_tool == CanvasTool::wall || m_tool == CanvasTool::boundary)) {
        bool consumed = false;
        if (event->key() == Qt::Key_J && m_drawing_corner_jump_requested)
            consumed = m_drawing_corner_jump_requested();
        else if (m_drawing_travel_requested) {
            int dx = 0, dy = 0;
            switch (event->key()) {
            case Qt::Key_Right: dx = 1; break;
            case Qt::Key_Left: dx = -1; break;
            case Qt::Key_Up: dy = 1; break;
            case Qt::Key_Down: dy = -1; break;
            default: break;
            }
            if (dx || dy) consumed = m_drawing_travel_requested(dx, dy);
        }
        if (consumed) { event->accept(); return; }
    }
    if ((event->key() == Qt::Key_H || event->key() == Qt::Key_V) &&
        hasFocus() && event->modifiers() == Qt::NoModifier && !event->isAutoRepeat() &&
        drawingCommandIdle() && !m_point_placement_requested &&
        m_pending_dimension_target_requested && m_pending_dimension_orientation_requested) {
        if (const auto target = m_pending_dimension_target_requested()) {
            m_pending_dimension_orientation_requested(*target, event->key() == Qt::Key_H);
            event->accept();
            return;
        }
    }
    const bool directional_modifiers = event->modifiers() == Qt::ControlModifier ||
        event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier);
    if (hasFocus() && directional_modifiers && drawingCommandIdle() &&
        !m_point_placement_requested && selectedIds().isEmpty() &&
        (m_tool == CanvasTool::boundary || m_tool == CanvasTool::wall) &&
        m_directional_alignment_requested) {
        int dx = 0, dy = 0;
        switch (event->key()) {
        case Qt::Key_Left: dx = -1; break;
        case Qt::Key_Right: dx = 1; break;
        case Qt::Key_Up: dy = 1; break;
        case Qt::Key_Down: dy = -1; break;
        default: break;
        }
        if (dx != 0 || dy != 0) {
            if (!event->isAutoRepeat()) {
                beginPerformanceMeasurement(PerformanceMetric::input);
                m_directional_alignment_requested(dx, dy,
                    event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier));
            }
            event->accept();
            return;
        }
    }
    // Exact entry returns focus here. A held key must not finish/cancel the
    // draft or reopen a modal after its initial input action was consumed.
    if (event->isAutoRepeat() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
         event->key() == Qt::Key_Escape || event->key() == Qt::Key_D ||
         event->key() == Qt::Key_B)) {
        event->accept();
        return;
    }
    if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
        m_gesture_button == Qt::NoButton && !m_touch_active &&
        event->text().size() == 1 && m_drawing_text_requested) {
        const auto character = event->text().front();
        if ((character.isDigit() || character == QLatin1Char('.') ||
             character == QLatin1Char('+') || character == QLatin1Char('-')) &&
            m_drawing_text_requested(event->text())) {
            event->accept();
            return;
        }
    }
    // Precise input opens a dialog; exclude the entire dispatch even if a
    // supplied callback happens to be nonmodal (for example in an embedder).
    if (event->key() == Qt::Key_D &&
        (m_tool == CanvasTool::boundary || m_tool == CanvasTool::wall || m_tool == CanvasTool::select)) {
        resetPerformanceMeasurements();
    } else if ((event->matches(QKeySequence::Undo) && m_draft_undo_requested) ||
               (event->matches(QKeySequence::Redo) && m_draft_redo_requested) ||
               event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
               event->key() == Qt::Key_Escape || event->key() == Qt::Key_F) {
        beginPerformanceMeasurement(PerformanceMetric::input);
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_pending_dimension_space_tap.reset();
        if (hasFocus() && event->modifiers() == Qt::NoModifier && drawingCommandIdle() &&
            !m_point_placement_requested && m_pending_dimension_target_requested)
            m_pending_dimension_space_tap = m_pending_dimension_target_requested();
        m_space_pan_armed = true;
        if (m_gesture_button == Qt::NoButton) setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Undo) && m_draft_undo_requested) {
        m_draft_undo_requested();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo) && m_draft_redo_requested) {
        m_draft_redo_requested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        // Navigation may already have abandoned a selected proposal. Never
        // fall through to ordinary Finish while that gesture is still active.
        if (event->modifiers() != Qt::NoModifier || !drawingCommandIdle()) {
            event->accept();
            return;
        }
        if (!m_point_placement_requested && selectedIds().isEmpty() &&
            m_drawing_pen_up_requested && m_drawing_pen_up_requested()) {
            event->accept();
            return;
        }
        if (m_finish_requested) {
            m_finish_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        clearSymbolDragPreview();
        m_drawing_witnesses.clear();
        update();
        if (m_touch_active || m_gesture_button != Qt::NoButton || m_move_release_pending || m_vertex_release_pending || m_transform_frame_start) {
            resetGesture();
            resetTouchInput();
            event->accept();
            return;
        }
        if (m_cancel_requested) {
            m_cancel_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_D && event->modifiers() == Qt::NoModifier &&
        m_gesture_button == Qt::NoButton && !m_touch_active && !m_move_release_pending && !m_transform_release_pending &&
        (m_tool == CanvasTool::boundary || m_tool == CanvasTool::wall || m_tool == CanvasTool::select)) {
        if (m_precise_input_requested) {
            m_precise_input_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_B &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
        drawingCommandIdle() &&
        (m_tool == CanvasTool::boundary || m_tool == CanvasTool::wall)) {
        if (m_bay_window_return_requested) m_bay_window_return_requested();
        event->accept();
        return;
    }
    if (hasFocus() && !event->isAutoRepeat() &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
        drawingCommandIdle() && !m_point_placement_requested &&
        (m_tool == CanvasTool::boundary || m_tool == CanvasTool::wall)) {
        if ((event->key() == Qt::Key_X || event->key() == Qt::Key_Y) &&
            m_witness_alignment_requested) {
            beginPerformanceMeasurement(PerformanceMetric::input);
            m_witness_alignment_requested(event->key() == Qt::Key_X);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_A && m_auto_close_drawing_requested) {
            beginPerformanceMeasurement(PerformanceMetric::input);
            m_auto_close_drawing_requested();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_F) {
        fitView();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PlanCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        const auto tap = std::exchange(m_pending_dimension_space_tap, std::nullopt);
        m_space_pan_armed = false;
        if (m_gesture_button == Qt::NoButton) {
            if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
            else setCursor(Qt::CrossCursor);
        }
        if (tap && hasFocus() && event->modifiers() == Qt::NoModifier && drawingCommandIdle() &&
            !m_point_placement_requested && m_pending_dimension_omission_requested)
            m_pending_dimension_omission_requested(*tap);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void PlanCanvas::resizeEvent(QResizeEvent* event) {
    if (event->oldSize() != event->size()) {
        clearSymbolDragPreview();
        ++m_navigation_generation;
    }
    m_pending_dimension_space_tap.reset();
    QWidget::resizeEvent(event);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

QPointF PlanCanvas::toScreen(Vec2 point, const QRectF& viewport) const {
    return {viewport.center().x() + (point.x - m_view_center.x) * m_scale,
            viewport.center().y() - (point.y - m_view_center.y) * m_scale};
}

Vec2 PlanCanvas::toModel(QPointF point, const QRectF& viewport) const {
    return {m_view_center.x + (point.x() - viewport.center().x()) / m_scale,
            m_view_center.y - (point.y() - viewport.center().y()) / m_scale};
}

double PlanCanvas::gridSpacingMetres() const noexcept {
    return grid_spacing(m_scale, m_metric_units).minor;
}

double PlanCanvas::drawingLengthIncrementMetres() const noexcept {
    // Length magnets can be finer than the painted XY grid. Six logical pixels
    // keep nearby choices reachable while allowing quarter-foot runs at 80 px/m.
    const auto minimum = 6.0 / m_scale;
    if (!m_metric_units) {
        for (const auto inches : {1.0/16.0,1.0/8.0,1.0/4.0,1.0/2.0,1.0,2.0,3.0,6.0}) {
            const auto step=inches*0.0254;
            if (step>=minimum) return step;
        }
    }
    double decade=m_metric_units ? 0.001 : 0.3048;
    for (int exponent=0;exponent<13;++exponent,decade*=10.0)
        for (const auto multiple : {1.0,2.0,5.0})
            if (decade*multiple>=minimum) return decade*multiple;
    return decade;
}

QString PlanCanvas::drawingLengthText(double metres, bool metric) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    if (metric) {
        const auto ticks = metres * 1000.0;
        if (!std::isfinite(ticks))
            return QStringLiteral("≈ %1 m").arg(metres,0,'g',6);
        const auto rounded = std::round(ticks) / 1000.0;
        const auto millimetres = std::abs(rounded) < 1.0;
        auto number = QString::number(rounded == 0.0 ? 0.0 :
            rounded * (millimetres ? 1000.0 : 1.0), 'f', millimetres ? 0 : 3);
        if (!millimetres) {
            while (number.endsWith(QLatin1Char('0'))) number.chop(1);
            if (number.endsWith(QLatin1Char('.'))) number.chop(1);
        }
        // Ignore only numerical noise in ordinary exact construction values.
        // This label is derived presentation; it never feeds geometry edits.
        const auto prefix = std::abs(metres-rounded) > 1e-10
            ? QStringLiteral("≈ ") : QString{};
        return QStringLiteral("%1%2 %3").arg(prefix,number,
            millimetres ? QStringLiteral("mm") : QStringLiteral("m"));
    }
    const auto inches=std::abs(metres)/0.0254;
    const auto sixteenths=std::round(inches*16.0);
    if (!std::isfinite(sixteenths)) return QStringLiteral("—");
    const auto prefix = std::abs(inches*16.0-sixteenths)>1e-7
        ? QStringLiteral("≈ ") : QString{};
    const auto sign=metres<0 && sixteenths>0 ? QStringLiteral("-") : QString{};
    if (sixteenths>9e15)
        return QStringLiteral("≈ %1%2 in").arg(sign).arg(inches,0,'g',6);
    const auto ticks=static_cast<qint64>(sixteenths);
    const auto feet=ticks/192;
    const auto whole=(ticks%192)/16;
    const auto numerator=ticks%16;
    QString inch_text=whole || !numerator ? QString::number(whole) : QString{};
    if (numerator) {
        const auto divisor=std::gcd(numerator,qint64{16});
        if (!inch_text.isEmpty()) inch_text += QLatin1Char(' ');
        inch_text += QStringLiteral("%1/%2").arg(numerator/divisor).arg(16/divisor);
    }
    return feet>0 ? QStringLiteral("%1%2%3 ft %4 in").arg(prefix,sign).arg(feet).arg(inch_text)
                  : QStringLiteral("%1%2%3 in").arg(prefix,sign,inch_text);
}

std::optional<Vec2> PlanCanvas::drawingOrigin() const {
    if (m_wall_preview && (m_tool==CanvasTool::wall || m_tool==CanvasTool::sloped_wall))
        return m_wall_preview->start;
    if (m_boundary_draft_preview && m_boundary_draft_preview->length_snap_active &&
        (m_tool==CanvasTool::boundary || m_tool==CanvasTool::select))
        return m_boundary_draft_preview->pen_position;
    return std::nullopt;
}

std::optional<Vec2> PlanCanvas::directionalDrawingAlignment(
    Vec2 origin, int dx, int dy, bool intersections_only) const {
    const bool cardinal = ((dx == -1 || dx == 1) && dy == 0) ||
        ((dy == -1 || dy == 1) && dx == 0);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !cardinal) return std::nullopt;
    const bool horizontal = dx != 0;
    const int direction = horizontal ? dx : dy;
    const auto coordinate = [&](Vec2 point) { return horizontal ? point.x : point.y; };
    const auto transverse = [&](Vec2 point) { return horizontal ? point.y : point.x; };
    std::optional<Vec2> best;
    const auto consider = [&](double value, double minimum_distance = 0.0) {
        if (!std::isfinite(value)) return;
        const auto origin_coordinate = coordinate(origin);
        if (direction > 0 ? value <= origin_coordinate : value >= origin_coordinate) return;
        // Compare world coordinates directly. Subtracting a distant origin
        // can round distinct targets to the same distance or overflow.
        if (best && (direction > 0 ? value >= coordinate(*best) : value <= coordinate(*best))) return;
        // Only analytical arc contacts need a tiny roundoff exclusion near
        // the ray origin; endpoint admission and ordering remain exact.
        if (minimum_distance > 0.0 &&
            (static_cast<long double>(value)-origin_coordinate)*direction <= minimum_distance) return;
        best = horizontal ? Vec2{value, origin.y} : Vec2{origin.x, value};
    };
    const auto segment_target = [&](const Segment& segment) {
        try {
            // Validate retained analytical geometry before using its endpoints.
            const auto source_length = segment_length(segment);
            if (!std::isfinite(source_length) || source_length <= 0.0) return;
            auto bounds = segment_bounds(segment);
            if (!intersections_only) {
                consider(coordinate(segment.start));
                consider(coordinate(segment.end));
                return;
            }
            if (segment.sweep_radians == 0.0) {
                // Work in long-double local differences: the transverse axis
                // must actually cross the line, without a screen snap radius.
                const auto start = transverse(segment.start);
                const auto end = transverse(segment.end);
                const auto axis = transverse(origin);
                if (start == end) {
                    if (start == axis) {
                        // An overlapping ray has no isolated closest contact.
                        // Its next finite structural endpoint is deterministic.
                        consider(coordinate(segment.start));
                        consider(coordinate(segment.end));
                    }
                } else if (axis >= std::min(start, end) && axis <= std::max(start, end)) {
                    const long double fraction = (static_cast<long double>(axis)-start)/
                        (static_cast<long double>(end)-start);
                    const auto value = static_cast<double>(static_cast<long double>(coordinate(segment.start)) +
                        (static_cast<long double>(coordinate(segment.end))-coordinate(segment.start))*fraction);
                    consider(value);
                }
                return;
            }
            // Exact endpoint contacts do not depend on the circle solve.
            const auto axis = transverse(origin);
            if (transverse(segment.start) == axis) consider(coordinate(segment.start));
            if (transverse(segment.end) == axis) consider(coordinate(segment.end));
            // Restrict the ray to the arc's analytical box. Empty distance
            // before the arc must not inflate the contact tolerance or cause
            // translated line/circle calculations to lose nearby roots.
            if (axis < transverse(bounds.minimum) || axis > transverse(bounds.maximum)) return;
            const auto start_coordinate = direction > 0
                ? std::max(coordinate(origin), coordinate(bounds.minimum))
                : std::min(coordinate(origin), coordinate(bounds.maximum));
            const auto extreme = coordinate(direction > 0 ? bounds.maximum : bounds.minimum);
            const auto length = (static_cast<long double>(extreme)-start_coordinate)*direction;
            if (!(length > 0.0) || length > std::numeric_limits<double>::max()) return;
            const Vec2 start = horizontal ? Vec2{start_coordinate, origin.y} : Vec2{origin.x, start_coordinate};
            const Vec2 end = horizontal ? Vec2{extreme, origin.y} : Vec2{origin.x, extreme};
            // Numerical roundoff only, independent of display zoom, grid and
            // the canvas's metre-based mouse snapping tolerance.
            const auto tolerance = 32.0 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, static_cast<double>(length), source_length});
            if (!std::isfinite(tolerance)) return;
            // Drawing alignment and topology use the same analytical kernel.
            // An unresolved contact must never move the pen.
            const auto hit = segment_intersection({start, end, 0.0}, segment, tolerance);
            if (hit.kind == SegmentIntersectionKind::proper || hit.kind == SegmentIntersectionKind::touch)
                for (const auto point : hit.points) consider(coordinate(point), tolerance);
        } catch (const std::exception&) {
            // Invalid or numerically unresolved geometry cannot move the pen.
        }
    };
    for (const auto& entity : m_entities) {
        const bool wall = entity.type == QStringLiteral("wall");
        const bool boundary = entity.type == QStringLiteral("boundary") ||
            entity.type == QStringLiteral("measurement_boundary") ||
            entity.type == QStringLiteral("room_boundary") ||
            entity.type == QStringLiteral("measurement_linework");
        if ((!wall && !boundary) || entity.svg_symbol) continue;
        if (!intersections_only) {
            for (const auto point : entity.snap_points) {
                if (std::isfinite(point.x) && std::isfinite(point.y)) consider(coordinate(point));
            }
        }
        // A painted wall footprint is not its exact authoring baseline.
        const auto& segments = wall
            ? (entity.drawing_alignment_segments.empty() ? entity.snap_segments : entity.drawing_alignment_segments)
            : (entity.snap_segments.empty() ? entity.segments : entity.snap_segments);
        for (const auto& segment : segments) segment_target(segment);
        if (boundary) {
            for (const auto& hole : entity.holes)
                for (const auto& segment : hole) segment_target(segment);
        }
    }
    if (m_boundary_draft_preview) {
        for (const auto& segment : m_boundary_draft_preview->segments) segment_target(segment);
    }
    return best;
}

Vec2 PlanCanvas::snapped(Vec2 point) const {
    if (!m_snap_enabled) {
        return point;
    }
    const auto grid = gridSpacingMetres();
    return {std::round(point.x / grid) * grid, std::round(point.y / grid) * grid};
}

std::optional<Vec2> PlanCanvas::closingAnchor(QPointF point) const {
    if ((m_tool != CanvasTool::boundary && m_tool != CanvasTool::select) || !m_boundary_draft_preview ||
        !m_boundary_draft_preview->can_close_on_anchor ||
        !m_boundary_draft_preview->anchor) return std::nullopt;
    const auto anchor = *m_boundary_draft_preview->anchor;
    const auto offset = point - toScreen(anchor, rect());
    // A fixed pixel target remains easy to hit at any zoom and takes priority
    // over grid rounding, including with grid snap disabled.
    return std::hypot(offset.x(), offset.y()) <= 12.0
        ? std::optional{anchor} : std::nullopt;
}

void PlanCanvas::setRawPointInput(bool enabled) {
    if (m_raw_point_input == enabled) return;
    m_raw_point_input = enabled;
    update();
}

PlanCanvas::SnapResult PlanCanvas::snapResult(QPointF screen_point) const {
    const auto raw = toModel(screen_point, rect());
    if (m_raw_point_input || !m_snap_enabled) return {raw, SnapKind::none, {}, {}};
    if (m_panning || m_left_dragging || m_selection_dragging || m_overview_dragging ||
        m_touch_navigation) {
        return {snapped(raw), SnapKind::none, {}, {}};
    }
    const auto origin=drawingOrigin();
    const auto length_snap=[&](SnapResult result) {
        if (!origin) return result;
        const auto length=distance(*origin,result.point);
        if (!std::isfinite(length) || length<=1e-12) return result;
        const auto step=drawingLengthIncrementMetres();
        const auto target=std::round(length/step)*step;
        const auto factor=target/length;
        result.point={origin->x+(result.point.x-origin->x)*factor,
                      origin->y+(result.point.y-origin->y)*factor};
        if (result.guide) result.guide->end=result.point;
        return result;
    };
    SnapResult grid_result{snapped(raw), SnapKind::grid, {}, {}};
    if (origin) grid_result=length_snap({raw,SnapKind::length,origin,{}});
    if (std::hypot(grid_result.point.x - raw.x, grid_result.point.y - raw.y) * m_scale < 0.5)
        grid_result.kind = SnapKind::none;
    if (!m_wall_snap_enabled || m_vertex_move_handle || m_opening_width_handle)
        return grid_result;

    constexpr double snap_radius_pixels = 12.0;
    const auto screen_distance = [&](Vec2 candidate) {
        return QLineF(screen_point, toScreen(candidate, rect())).length();
    };
    struct Candidate {
        SnapResult result;
        double distance{std::numeric_limits<double>::infinity()};
    };
    Candidate endpoint, on_wall, axis_intersection, perpendicular, alignment;
    const auto consider = [&](Candidate& best, Vec2 candidate, SnapKind kind,
                              std::optional<Vec2> anchor = std::nullopt,
                              std::optional<Segment> guide = std::nullopt) {
        if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y)) return;
        const auto distance = screen_distance(candidate);
        if (distance <= snap_radius_pixels && distance < best.distance) {
            best.result = {candidate, kind, anchor, guide};
            best.distance = distance;
        }
    };

    const auto local_targets = retainedSnapCandidates(screen_point, snap_radius_pixels);
    if (local_targets) {
        for (const auto& target : *local_targets) {
            if (target.segment) continue;
            const auto point = m_entities[target.entity_index].snap_points[target.target_index];
            consider(endpoint, point, SnapKind::endpoint, point);
        }
    } else {
        for (const auto& entity : m_entities) {
            for (const auto point : entity.snap_points)
                consider(endpoint, point, SnapKind::endpoint, point);
        }
    }
    // A nearby true endpoint takes precedence over every projection. This
    // prevents an apparently aligned point from replacing a connected corner
    // with a nearby point that leaves a small gap.
    if (std::isfinite(endpoint.distance)) return endpoint.result;

    const auto consider_baseline = [&](const CanvasEntity& entity, const Segment& baseline) {
        try {
            const auto length = segment_length(baseline);
            const auto station = std::clamp(
                project_host_station(baseline, raw, length * 0.5), 0.0, length);
            const auto candidate = point_at_host_station(baseline, station);
            consider(on_wall, candidate,
                     entity.type == QStringLiteral("wall") ? SnapKind::on_wall
                                                           : SnapKind::on_boundary,
                     candidate,
                     Segment{candidate, candidate, 0.0});
        } catch (const std::exception&) {
            // An ambiguous or malformed baseline is not a snap target.
        }
    };
    if (local_targets) {
        for (const auto& target : *local_targets) {
            if (!target.segment) continue;
            const auto& entity = m_entities[target.entity_index];
            consider_baseline(entity, entity.snap_segments[target.target_index]);
        }
    } else {
        for (const auto& entity : m_entities)
            for (const auto& baseline : entity.snap_segments)
                consider_baseline(entity, baseline);
    }
    if (std::isfinite(on_wall.distance)) return on_wall.result;

    // Endpoint and wall-segment alignment guides keep chained wall corners
    // square and make common 45-degree runs easy to place without changing
    // the snap point used by the committed segment.
    if (origin) {
        const auto anchor = *origin;
        const auto snap_radius = snap_radius_pixels / std::max(m_scale, 1e-9);
        const auto consider_endpoint_guides = [&](Vec2 endpoint_point) {
            // Resolve both axes together before choosing a single guide.
            // Otherwise a perfectly horizontal run can miss the nearby
            // x-coordinate of its starting corner and skew the last wall.
            for (const auto corner : {Vec2{endpoint_point.x, anchor.y},
                                      Vec2{anchor.x, endpoint_point.y}}) {
                if (std::hypot(corner.x - anchor.x, corner.y - anchor.y) <= snap_radius)
                    continue;
                consider(axis_intersection, corner, SnapKind::alignment, endpoint_point,
                         Segment{endpoint_point, corner, 0.0});
            }
            for (const auto direction : {Vec2{1.0, 0.0}, Vec2{0.0, 1.0}}) {
                const auto amount = (raw.x - endpoint_point.x) * direction.x +
                                    (raw.y - endpoint_point.y) * direction.y;
                const Vec2 projected{endpoint_point.x + amount * direction.x,
                                     endpoint_point.y + amount * direction.y};
                consider(alignment, projected, SnapKind::alignment, endpoint_point,
                         Segment{endpoint_point, projected, 0.0});
            }
        };
        const auto consider_segment_guides = [&](const Segment& segment) {
            const auto dx = segment.end.x - segment.start.x;
            const auto dy = segment.end.y - segment.start.y;
            const auto length = std::hypot(dx, dy);
            if (!std::isfinite(length) || length <= 1e-9) return;
            const Vec2 tangent{dx / length, dy / length};
            const Vec2 normal{-tangent.y, tangent.x};
            for (const auto endpoint_point : {segment.start, segment.end}) {
                if (std::hypot(anchor.x - endpoint_point.x, anchor.y - endpoint_point.y) > snap_radius) continue;
                const auto amount = (raw.x - anchor.x) * normal.x +
                                    (raw.y - anchor.y) * normal.y;
                const Vec2 projected{anchor.x + amount * normal.x,
                                     anchor.y + amount * normal.y};
                consider(perpendicular, projected, SnapKind::perpendicular, endpoint_point,
                         Segment{endpoint_point, projected, 0.0});
            }
        };
        // Coordinate strips retain guides from arbitrarily distant endpoints.
        // Segment normals need only the original anchor-near endpoint test.
        const auto guide_targets = retainedSnapCandidates(screen_point, snap_radius_pixels, anchor);
        if (guide_targets) {
            for (const auto& target : *guide_targets) {
                const auto& entity = m_entities[target.entity_index];
                if (target.segment) consider_segment_guides(entity.snap_segments[target.target_index]);
                else consider_endpoint_guides(entity.snap_points[target.target_index]);
            }
        } else {
            for (const auto& entity : m_entities) {
                for (const auto endpoint_point : entity.snap_points) consider_endpoint_guides(endpoint_point);
                for (const auto& segment : entity.snap_segments) consider_segment_guides(segment);
            }
        }
        constexpr double diagonal = 0.7071067811865475244;
        for (const Vec2 direction : {Vec2{1.0, 0.0}, Vec2{0.0, 1.0},
                                     Vec2{diagonal, diagonal}, Vec2{diagonal, -diagonal}}) {
            const auto amount = (raw.x - anchor.x) * direction.x +
                                (raw.y - anchor.y) * direction.y;
            const Vec2 projected{anchor.x + amount * direction.x,
                                 anchor.y + amount * direction.y};
            consider(alignment, projected, SnapKind::alignment, anchor,
                     Segment{anchor, projected, 0.0});
        }
    }
    if (std::isfinite(axis_intersection.distance)) return axis_intersection.result;
    if (std::isfinite(perpendicular.distance)) return length_snap(perpendicular.result);
    if (std::isfinite(alignment.distance)) {
        // A guide through this segment's own start remains coherent after
        // radial rounding; an external coordinate guide is an exact object snap.
        if (alignment.result.anchor && origin &&
            distance(*alignment.result.anchor,*origin)<1e-10)
            return length_snap(alignment.result);
        return alignment.result;
    }
    return grid_result;
}

Vec2 PlanCanvas::inputPoint(QPointF point) const {
    if (const auto anchor = closingAnchor(point)) return *anchor;
    return snapResult(point).point;
}

QStringList PlanCanvas::rectangleHits(const QRectF& rectangle, bool crossing) const {
    QStringList result;
    const auto add = [&](const QString& id) {
        if (!id.isEmpty() && !result.contains(id)) result.push_back(id);
    };
    const auto matches = [&](const QPainterPath& path) {
        return !path.isEmpty() && (crossing ? path.intersects(rectangle)
                                           : rectangle.contains(path.boundingRect()));
    };
    QTransform model_to_screen;
    model_to_screen.translate(QRectF(rect()).center().x(), QRectF(rect()).center().y());
    model_to_screen.scale(m_scale, -m_scale);
    model_to_screen.translate(-m_view_center.x, -m_view_center.y);
    ensurePublishedEntityGeometry();
    for (std::size_t index = 0; index < m_entities.size(); ++index) {
        const auto& entity = m_entities[index];
        if (!matchesSelectionType(entity.type)) continue;
        auto path = m_published_entity_geometry[index].stroke;
        append_boundary_strokes(path, entity.hit_segments);
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(3.0, entity_screen_stroke_width(entity, m_scale, logicalDpiX()/25.4)));
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        // Stroke open paths before intersection so Qt cannot implicitly fill
        // an open chain and select empty space between unrelated segments.
        const auto screen_stroke = stroker.createStroke(model_to_screen.map(path));
        QPainterPath screen_fill;
        if (entity.type == QStringLiteral("symbol"))
            screen_fill = model_to_screen.map(symbol_pick_footprint(entity, path));
        else if (entity.filled)
            if (const auto fill = closed_entity_path(entity)) screen_fill = model_to_screen.map(*fill);
        // Test the two footprints separately. Adding a fill to an odd-even
        // stroke path would turn their overlap into an artificial empty hole.
        const bool has_stroke = !screen_stroke.isEmpty(), has_fill = !screen_fill.isEmpty();
        const bool hit = crossing ? matches(screen_stroke) || matches(screen_fill)
            : (has_stroke || has_fill) && (!has_stroke || matches(screen_stroke)) &&
                (!has_fill || matches(screen_fill));
        if (hit) add(entity.id);
    }
    for (const auto& label : positionedLabels(font(), this, m_scale, logicalDpiY(), false)) {
        if (!matchesSelectionFilter(label.id)) continue;
        if (!drawable_label(label)) continue;
        QPainterPath path;
        path.addRect(label_layout(label, font(), this, m_scale, logicalDpiY()).bounds);
        if (matches(label_transform(label, toScreen(label.position, rect())).map(path)))
            add(label.id);
    }
    for (const auto& reference : m_references) {
        if (m_selection_filter != CanvasSelectionFilter::all &&
            m_selection_filter != CanvasSelectionFilter::references) continue;
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (!reference.visible || reference.image.isNull() ||
            !std::isfinite(reference.position.x) || !std::isfinite(reference.position.y) ||
            !std::isfinite(reference.rotation_degrees) || !std::isfinite(unit) || unit <= 0.0) continue;
        QTransform transform = model_to_screen;
        transform.translate(reference.position.x, reference.position.y);
        transform.rotate(reference.rotation_degrees);
        QPainterPath path;
        path.addRect(QRectF(-reference.image.width() * unit * 0.5,
                            -reference.image.height() * unit * 0.5,
                            reference.image.width() * unit, reference.image.height() * unit));
        if (matches(transform.map(path))) add(reference.id);
    }
    return result;
}

bool PlanCanvas::matchesSelectionType(const QString& type) const {
    if (m_selection_filter == CanvasSelectionFilter::all) return true;
    if (type == QStringLiteral("boundary") || type == QStringLiteral("measurement_boundary") ||
        type == QStringLiteral("room_boundary") || type == QStringLiteral("room"))
        return m_selection_filter == CanvasSelectionFilter::areas;
    if (type == QStringLiteral("dimension") || type == QStringLiteral("dimension_line"))
        return m_selection_filter == CanvasSelectionFilter::dimensions;
    if (type == QStringLiteral("symbol")) return m_selection_filter == CanvasSelectionFilter::symbols;
    if (type == QStringLiteral("measurement_linework"))
        return m_selection_filter == CanvasSelectionFilter::objects;
    if (type == QStringLiteral("wall") || type == QStringLiteral("opening") ||
        type == QStringLiteral("door") || type == QStringLiteral("window") ||
        type == QStringLiteral("slab") || type == QStringLiteral("roof") ||
        type == QStringLiteral("stair") || type == QStringLiteral("railing") ||
        type == QStringLiteral("column") || type == QStringLiteral("beam") || type == QStringLiteral("assembly") ||
        type == QStringLiteral("assembly_instance"))
        return m_selection_filter == CanvasSelectionFilter::objects;
    return false;
}

bool PlanCanvas::matchesSelectionFilter(const QString& id) const {
    for (const auto& entity : m_entities)
        if (entity.id == id) return matchesSelectionType(entity.type);
    for (const auto& reference : m_references)
        if (reference.id == id)
            return m_selection_filter == CanvasSelectionFilter::all ||
                   m_selection_filter == CanvasSelectionFilter::references;
    for (const auto& label : m_labels) {
        if (label.id != id) continue;
        // Unowned plan labels are generated titles, not authored annotations.
        if (label.plan_only) return false;
        if (!label.selection_type.isEmpty()) return matchesSelectionType(label.selection_type);
        return m_selection_filter == CanvasSelectionFilter::all ||
               m_selection_filter == CanvasSelectionFilter::labels;
    }
    return false;
}

void PlanCanvas::ensureLocalSnapIndex() const {
    if (m_local_snap_index_ready) return;
    auto& entries = m_local_snap_index_entries;
    auto& nodes = m_local_snap_index_nodes;
    auto& fallback = m_local_snap_index_fallback;
    entries.clear();
    nodes.clear();
    fallback.clear();
    constexpr double safe_extent = 1e12;
    const auto safe_point = [safe_extent](Vec2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y) &&
            std::abs(point.x) <= safe_extent && std::abs(point.y) <= safe_extent;
    };
    const auto add = [&](LocalSnapTarget target, Vec2 minimum, Vec2 maximum,
                         double arithmetic_extent) {
        if (!safe_point(minimum) || !safe_point(maximum) ||
            !std::isfinite(arithmetic_extent) || arithmetic_extent > safe_extent) {
            fallback.push_back(target);
            return;
        }
        const auto rounding = 256.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, arithmetic_extent, std::abs(minimum.x), std::abs(minimum.y),
                      std::abs(maximum.x), std::abs(maximum.y)});
        const QRectF bounds(QPointF(minimum.x - rounding, minimum.y - rounding),
                            QPointF(maximum.x + rounding, maximum.y + rounding));
        if (!finite_rect(bounds) || !std::isfinite(bounds.width()) ||
            !std::isfinite(bounds.height())) fallback.push_back(target);
        else entries.push_back({bounds, target});
    };
    for (std::size_t entity_index = 0; entity_index < m_entities.size(); ++entity_index) {
        const auto& entity = m_entities[entity_index];
        for (std::size_t index = 0; index < entity.snap_points.size(); ++index) {
            const auto point = entity.snap_points[index];
            add({entity_index, index, false}, point, point, 0.0);
        }
        for (std::size_t index = 0; index < entity.snap_segments.size(); ++index) {
            const LocalSnapTarget target{entity_index, index, true};
            const auto& segment = entity.snap_segments[index];
            if (!safe_point(segment.start) || !safe_point(segment.end) ||
                !std::isfinite(segment.sweep_radians) ||
                std::abs(segment.sweep_radians) >= 2.0 * pi) {
                fallback.push_back(target);
                continue;
            }
            try {
                // Validate actual analytical snap geometry, never paint/pick bounds.
                const auto bounds = segment_bounds(segment);
                if (segment.sweep_radians == 0.0) {
                    add(target, bounds.minimum, bounds.maximum,
                        std::hypot(segment.end.x - segment.start.x,
                                   segment.end.y - segment.start.y));
                    continue;
                }
                // Match host_geometry's start-relative rotation. Its entire
                // support circle encloses every clamped station, including
                // major/clockwise arcs and tiny sweeps without sample gaps.
                const Vec2 chord{segment.end.x - segment.start.x,
                                 segment.end.y - segment.start.y};
                const auto chord_length = std::hypot(chord.x, chord.y);
                const auto tangent = std::tan(segment.sweep_radians * 0.5);
                if (!std::isfinite(chord_length) || chord_length <= 0.0 ||
                    !std::isfinite(tangent) || tangent == 0.0) {
                    fallback.push_back(target);
                    continue;
                }
                const auto center_distance = (chord_length * 0.5) / tangent;
                const Vec2 left{-chord.y / chord_length, chord.x / chord_length};
                const Vec2 radial{-chord.x * 0.5 - left.x * center_distance,
                                  -chord.y * 0.5 - left.y * center_distance};
                const auto radius = std::hypot(radial.x, radial.y);
                const Vec2 center{segment.start.x - radial.x, segment.start.y - radial.y};
                if (!safe_point(radial) || !safe_point(center) ||
                    !std::isfinite(center_distance) || std::abs(center_distance) > safe_extent ||
                    !std::isfinite(radius) || radius > safe_extent) {
                    fallback.push_back(target);
                    continue;
                }
                // Include authored endpoints explicitly: the station helper
                // returns them exactly rather than evaluating the rotation.
                add(target,
                    {std::min(bounds.minimum.x, center.x - radius),
                     std::min(bounds.minimum.y, center.y - radius)},
                    {std::max(bounds.maximum.x, center.x + radius),
                     std::max(bounds.maximum.y, center.y + radius)}, radius);
            } catch (const std::exception&) {
                // Keep malformed/numerically unresolved targets on the original path.
                fallback.push_back(target);
            }
        }
    }
    nodes.reserve(entries.size());
    const auto build = [&](auto&& self, std::size_t first, std::size_t count) -> std::size_t {
        LocalSnapIndexNode node;
        auto left_edge = entries[first].bounds.left();
        auto top_edge = entries[first].bounds.top();
        auto right_edge = entries[first].bounds.right();
        auto bottom_edge = entries[first].bounds.bottom();
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto& bounds = entries[first + offset].bounds;
            left_edge = std::min(left_edge, bounds.left());
            top_edge = std::min(top_edge, bounds.top());
            right_edge = std::max(right_edge, bounds.right());
            bottom_edge = std::max(bottom_edge, bounds.bottom());
        }
        const auto rounding = 4.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(left_edge), std::abs(top_edge),
                      std::abs(right_edge), std::abs(bottom_edge)});
        node.bounds = QRectF(QPointF(left_edge - rounding, top_edge - rounding),
                             QPointF(right_edge + rounding, bottom_edge + rounding));
        const auto node_index = nodes.size();
        nodes.push_back(node);
        if (count <= 8) {
            nodes[node_index].first = first;
            nodes[node_index].count = count;
        } else {
            const bool split_x = node.bounds.width() >= node.bounds.height();
            const auto middle = first + count / 2;
            std::nth_element(entries.begin() + first, entries.begin() + middle,
                entries.begin() + first + count, [split_x](const auto& left, const auto& right) {
                    return split_x
                        ? std::midpoint(left.bounds.left(), left.bounds.right()) <
                          std::midpoint(right.bounds.left(), right.bounds.right())
                        : std::midpoint(left.bounds.top(), left.bounds.bottom()) <
                          std::midpoint(right.bounds.top(), right.bounds.bottom());
                });
            const auto left = self(self, first, middle - first);
            const auto right = self(self, middle, first + count - middle);
            nodes[node_index].left = left;
            nodes[node_index].right = right;
        }
        return node_index;
    };
    if (!entries.empty()) build(build, 0, entries.size());
    m_local_snap_index_ready = true;
}

std::optional<std::vector<PlanCanvas::LocalSnapTarget>> PlanCanvas::retainedSnapCandidates(
    QPointF point, double radius_pixels, std::optional<Vec2> alignment_anchor) const {
    // Previews/pending releases keep the original traversal and input authority.
    constexpr double safe_extent = 1e12;
    const auto viewport = QRectF(rect());
    if (hasInteractivePresentation() || !std::isfinite(m_scale) ||
        m_scale < minimum_scale || m_scale > maximum_scale ||
        !std::isfinite(m_view_center.x) || !std::isfinite(m_view_center.y) ||
        std::abs(m_view_center.x) > safe_extent || std::abs(m_view_center.y) > safe_extent ||
        !std::isfinite(point.x()) || !std::isfinite(point.y()) ||
        std::abs(point.x()) > safe_extent || std::abs(point.y()) > safe_extent ||
        !finite_rect(viewport) || !std::isfinite(radius_pixels) || radius_pixels < 0.0)
        return std::nullopt;
    const auto model = toModel(point, viewport);
    if (!std::isfinite(model.x) || !std::isfinite(model.y) ||
        std::abs(model.x) > safe_extent || std::abs(model.y) > safe_extent) return std::nullopt;
    const auto anchor = alignment_anchor.value_or(model);
    if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y) ||
        std::abs(anchor.x) > safe_extent || std::abs(anchor.y) > safe_extent) return std::nullopt;
    ensureLocalSnapIndex();
    auto result = m_local_snap_index_fallback;
    const auto overlaps = [](const QRectF& left, const QRectF& right) {
        // Inclusive scalar comparisons retain zero-width points/lines.
        return !(left.right() < right.left() || left.left() > right.right() ||
                 left.bottom() < right.top() || left.top() > right.bottom());
    };
    std::vector<std::size_t> pending;
    if (!m_local_snap_index_nodes.empty()) pending.push_back(0);
    while (!pending.empty()) {
        const auto node_index = pending.back();
        pending.pop_back();
        const auto& node = m_local_snap_index_nodes[node_index];
        // Bound inverse mapping and the unchanged screen-space distance test.
        // Unbounded projection subtracts then adds the source coordinate. Its
        // roundoff includes the distant endpoint, even when the cursor is local.
        const auto rounding = (alignment_anchor ? 256.0 : 64.0) * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(model.x), std::abs(model.y),
                std::abs(anchor.x), std::abs(anchor.y),
                std::abs(m_view_center.x), std::abs(m_view_center.y),
                std::abs(node.bounds.left()), std::abs(node.bounds.right()),
                std::abs(node.bounds.top()), std::abs(node.bounds.bottom()),
                (std::abs(point.x()) + std::abs(viewport.center().x())) / m_scale,
                (std::abs(point.y()) + std::abs(viewport.center().y())) / m_scale});
        const auto padding = radius_pixels / m_scale + rounding;
        const QRectF query(QPointF(model.x - padding, model.y - padding),
                           QPointF(model.x + padding, model.y + padding));
        const QRectF anchor_query(QPointF(anchor.x - padding, anchor.y - padding),
                                  QPointF(anchor.x + padding, anchor.y + padding));
        if (!std::isfinite(padding) || !finite_rect(query) ||
            !std::isfinite(query.width()) || !std::isfinite(query.height()) ||
            !finite_rect(anchor_query) || !std::isfinite(anchor_query.width()) ||
            !std::isfinite(anchor_query.height()) ||
            !finite_rect(node.bounds)) return std::nullopt;
        const auto overlaps_axis = [&](const QRectF& bounds) {
            return !(bounds.right() < query.left() || bounds.left() > query.right()) ||
                   !(bounds.bottom() < query.top() || bounds.top() > query.bottom());
        };
        const bool node_admitted = alignment_anchor
            ? overlaps_axis(node.bounds) || overlaps(node.bounds, anchor_query)
            : overlaps(node.bounds, query);
        if (!node_admitted) continue;
        if (node.count) {
            for (std::size_t offset = 0; offset < node.count; ++offset) {
                const auto& entry = m_local_snap_index_entries[node.first + offset];
                const bool admitted = alignment_anchor
                    ? (entry.target.segment ? overlaps(entry.bounds, anchor_query) : overlaps_axis(entry.bounds))
                    : overlaps(entry.bounds, query);
                if (admitted) result.push_back(entry.target);
            }
        } else {
            pending.push_back(node.right);
            pending.push_back(node.left);
        }
    }
    // Each phase keeps entity/within-entity order for strict-distance ties.
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.entity_index != right.entity_index) return left.entity_index < right.entity_index;
        if (left.segment != right.segment) return left.segment < right.segment;
        return left.target_index < right.target_index;
    });
    return result;
}

void PlanCanvas::ensureEntityHitIndex() const {
    if (m_entity_hit_index_ready) return;
    auto& entries = m_entity_hit_index_entries;
    auto& nodes = m_entity_hit_index_nodes;
    auto& fallback = m_entity_hit_index_fallback;
    entries.clear();
    nodes.clear();
    fallback.clear();
    entries.reserve(m_entities.size());
    // Keep extreme inputs on the full traversal. Retained envelopes include
    // actual paint curves, interaction-only spans and device width coefficients.
    constexpr double safe_extent = 1e12;
    const auto safe_point = [safe_extent](Vec2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y) &&
            std::abs(point.x) <= safe_extent && std::abs(point.y) <= safe_extent;
    };
    for (std::size_t index = 0; index < m_entities.size(); ++index) {
        const auto& entity = m_entities[index];
        bool has_bounds = false;
        double left_edge{}, top_edge{}, right_edge{}, bottom_edge{};
        bool safe = true;
        const auto include = [&](Vec2 point) {
            if (!safe_point(point)) { safe = false; return; }
            if (!has_bounds) {
                left_edge = right_edge = point.x;
                top_edge = bottom_edge = point.y;
                has_bounds = true;
            } else {
                // Keep scalar extrema until the final rectangle, avoiding
                // repeated origin/extent conversion and retaining line bounds.
                left_edge = std::min(left_edge, point.x);
                top_edge = std::min(top_edge, point.y);
                right_edge = std::max(right_edge, point.x);
                bottom_edge = std::max(bottom_edge, point.y);
            }
        };
        const auto include_boundary = [&](const Boundary& boundary) {
            for (const auto& segment : boundary) {
                if (!safe_point(segment.start) || !safe_point(segment.end) ||
                    !std::isfinite(segment.sweep_radians) ||
                    std::abs(segment.sweep_radians) > 2.0 * pi) {
                    safe = false;
                    return;
                }
                include(segment.start);
                include(segment.end);
                if (segment.sweep_radians != 0.0) {
                    const auto arc = arc_info(segment);
                    if (!arc || !safe_point(arc->center) || arc->radius > safe_extent) {
                        safe = false;
                        return;
                    }
                }
            }
            QPainterPath path;
            append_boundary_strokes(path, boundary);
            if (!path.isEmpty()) {
                const auto controls = path.controlPointRect();
                if (!finite_rect(controls)) safe = false;
                else {
                    include({controls.left(), controls.top()});
                    include({controls.right(), controls.bottom()});
                }
            }
        };
        include_boundary(entity.stroke_segments ? *entity.stroke_segments : entity.segments);
        for (const auto& hole : entity.holes) include_boundary(hole);
        include_boundary(entity.hit_segments);
        if (safe && entity.type == QStringLiteral("symbol")) {
            QPainterPath stroke;
            append_boundary_strokes(stroke, entity.stroke_segments ? *entity.stroke_segments : entity.segments);
            const auto footprint = symbol_pick_footprint(entity, stroke);
            if (!footprint.isEmpty()) {
                const auto controls = footprint.controlPointRect();
                include({controls.left(), controls.top()});
                include({controls.right(), controls.bottom()});
            }
        }
        const bool area = entity.type == QStringLiteral("boundary") ||
            entity.type == QStringLiteral("measurement_boundary") ||
            entity.type == QStringLiteral("room_boundary");
        if (safe && (entity.filled || entity.type == QStringLiteral("wall") || area)) {
            if (const auto fill = closed_entity_path(entity)) {
                // Complete fill geometry remains selectable when custom paint
                // omits internal seams. Control bounds also enclose interiors.
                const auto controls = fill->controlPointRect();
                if (!finite_rect(controls)) safe = false;
                else {
                    include({controls.left(), controls.top()});
                    include({controls.right(), controls.bottom()});
                }
            }
        }
        const auto envelope = entity_stroke_envelope(entity);
        if (!safe || !has_bounds || !envelope) { fallback.push_back(index); continue; }
        const auto rounding = 32.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(left_edge), std::abs(right_edge),
                      std::abs(top_edge), std::abs(bottom_edge), envelope->model_width});
        const auto padding = envelope->model_width * 0.5 + rounding;
        HitIndexEntry entry;
        entry.entity_index = index;
        entry.bounds = QRectF(QPointF(left_edge - padding, top_edge - padding),
                             QPointF(right_edge + padding, bottom_edge + padding));
        entry.cosmetic_pixels = envelope->cosmetic_pixels;
        entry.symbol_metres = envelope->symbol_metres;
        entry.paper_mm = envelope->paper_mm;
        if (!finite_rect(entry.bounds) || !std::isfinite(entry.bounds.width()) ||
            !std::isfinite(entry.bounds.height())) { fallback.push_back(index); continue; }
        entries.push_back(entry);
    }
    nodes.reserve(entries.size());
    const auto build = [&](auto&& self, std::size_t first, std::size_t count) -> std::size_t {
        HitIndexNode node;
        auto left_edge = entries[first].bounds.left();
        auto top_edge = entries[first].bounds.top();
        auto right_edge = entries[first].bounds.right();
        auto bottom_edge = entries[first].bounds.bottom();
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& entry = entries[first + offset];
            left_edge = std::min(left_edge, entry.bounds.left());
            top_edge = std::min(top_edge, entry.bounds.top());
            right_edge = std::max(right_edge, entry.bounds.right());
            bottom_edge = std::max(bottom_edge, entry.bounds.bottom());
            node.cosmetic_pixels = std::max(node.cosmetic_pixels, entry.cosmetic_pixels);
            node.symbol_metres = std::max(node.symbol_metres, entry.symbol_metres);
            node.paper_mm = std::max(node.paper_mm, entry.paper_mm);
        }
        const auto rounding = 4.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(left_edge), std::abs(top_edge),
                      std::abs(right_edge), std::abs(bottom_edge)});
        node.bounds = QRectF(QPointF(left_edge - rounding, top_edge - rounding),
                             QPointF(right_edge + rounding, bottom_edge + rounding));
        const auto node_index = nodes.size();
        nodes.push_back(node);
        if (count <= 8) {
            nodes[node_index].first = first;
            nodes[node_index].count = count;
        } else {
            const bool split_x = node.bounds.width() >= node.bounds.height();
            const auto middle = first + count / 2;
            std::nth_element(entries.begin() + first, entries.begin() + middle,
                entries.begin() + first + count, [split_x](const auto& left, const auto& right) {
                    const auto center = [split_x](const auto& entry) {
                        return split_x ? std::midpoint(entry.bounds.left(), entry.bounds.right())
                                       : std::midpoint(entry.bounds.top(), entry.bounds.bottom());
                    };
                    const auto left_center = center(left), right_center = center(right);
                    return left_center == right_center ? left.entity_index < right.entity_index
                                                       : left_center < right_center;
                });
            const auto left = self(self, first, middle - first);
            const auto right = self(self, middle, first + count - middle);
            nodes[node_index].left = left;
            nodes[node_index].right = right;
        }
        return node_index;
    };
    if (!entries.empty()) build(build, 0, entries.size());
    m_entity_hit_index_ready = true;
}

std::optional<std::vector<std::size_t>> PlanCanvas::entityHitCandidates(
    QPointF point, double hit_pixels) const {
    // Preview transforms are intentionally outside the retained input proof.
    // Full traversal preserves the original drag/rejection/release behavior.
    constexpr double safe_extent = 1e12;
    const auto viewport = QRectF(rect());
    const auto pixels_per_mm = logicalDpiX() / 25.4;
    if (hasInteractivePresentation() || !std::isfinite(m_scale) ||
        m_scale < minimum_scale || m_scale > maximum_scale ||
        !std::isfinite(m_view_center.x) || !std::isfinite(m_view_center.y) ||
        std::abs(m_view_center.x) > safe_extent || std::abs(m_view_center.y) > safe_extent ||
        !std::isfinite(point.x()) || !std::isfinite(point.y()) ||
        std::abs(point.x()) > safe_extent || std::abs(point.y()) > safe_extent ||
        !finite_rect(viewport) || !std::isfinite(hit_pixels) || hit_pixels < 0.0 ||
        !std::isfinite(pixels_per_mm) || pixels_per_mm <= 0.0) return std::nullopt;
    const auto model = toModel(point, viewport);
    if (!std::isfinite(model.x) || !std::isfinite(model.y) ||
        std::abs(model.x) > safe_extent || std::abs(model.y) > safe_extent) return std::nullopt;
    ensureEntityHitIndex();
    auto result = m_entity_hit_index_fallback;
    const auto overlaps = [](const QRectF& left, const QRectF& right) {
        return !(left.right() < right.left() || left.left() > right.right() ||
                 left.bottom() < right.top() || left.top() > right.bottom());
    };
    std::vector<std::size_t> pending;
    if (!m_entity_hit_index_nodes.empty()) pending.push_back(0);
    while (!pending.empty()) {
        const auto node_index = pending.back();
        pending.pop_back();
        const auto& node = m_entity_hit_index_nodes[node_index];
        const auto cosmetic_width = std::max({node.cosmetic_pixels,
            node.symbol_metres * m_scale, node.paper_mm * pixels_per_mm});
        // Nine-pixel strokes also enclose the symbol's four-pixel expanded
        // bounding rectangle. Model-width padding is already in the bounds.
        const auto radius = (std::max(hit_pixels, 4.0) + pick_edge_padding_pixels +
                             cosmetic_width * 0.5) / m_scale;
        // Cover both toScreen's subtraction and the fill QTransform's affine
        // translation, plus inverse mapping and closest-point arithmetic.
        const auto rounding = 64.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(model.x), std::abs(model.y),
                std::abs(m_view_center.x), std::abs(m_view_center.y),
                std::abs(node.bounds.left()), std::abs(node.bounds.right()),
                std::abs(node.bounds.top()), std::abs(node.bounds.bottom()),
                (std::abs(point.x()) + std::abs(viewport.center().x())) / m_scale,
                (std::abs(point.y()) + std::abs(viewport.center().y())) / m_scale});
        const auto padding = radius + rounding;
        const QRectF query(QPointF(model.x - padding, model.y - padding),
                           QPointF(model.x + padding, model.y + padding));
        if (!std::isfinite(padding) || !finite_rect(query) ||
            !std::isfinite(query.width()) || !std::isfinite(query.height()) ||
            !finite_rect(node.bounds)) return std::nullopt;
        if (!overlaps(node.bounds, query)) continue;
        if (node.count) {
            for (std::size_t offset = 0; offset < node.count; ++offset) {
                const auto& entry = m_entity_hit_index_entries[node.first + offset];
                if (overlaps(entry.bounds, query)) result.push_back(entry.entity_index);
            }
        } else {
            pending.push_back(node.right);
            pending.push_back(node.left);
        }
    }
    // Strict stroke ties and last interior/symbol wins depend on source order.
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<std::vector<std::size_t>> PlanCanvas::labelHitCandidates(
    QPointF point, double hit_pixels) const {
    auto& cache = m_label_placement_cache[0];
    constexpr double safe_extent = 1e12;
    const auto viewport = QRectF(rect());
    const auto safe_point = [safe_extent](Vec2 p) {
        return std::isfinite(p.x) && std::isfinite(p.y) &&
            std::abs(p.x) <= safe_extent && std::abs(p.y) <= safe_extent;
    };
    // hitTest has just requested this exact screen publication. Preview keys
    // and invalid-scale/DPI publications do not prove retained layout coverage.
    if (hasInteractivePresentation() || cache.retained_key.isEmpty() ||
        cache.paint_layouts.size() != cache.labels.size() ||
        !std::isfinite(m_scale) || m_scale < minimum_scale || m_scale > maximum_scale ||
        !safe_point(m_view_center) || !safe_point({point.x(), point.y()}) ||
        !finite_rect(viewport) || !std::isfinite(hit_pixels) || hit_pixels < 0.0)
        return std::nullopt;
    const auto model = toModel(point, viewport);
    if (!safe_point(model)) return std::nullopt;
    auto& index = cache.hit_index;
    if (!index.ready) {
        index.entries.clear();
        index.nodes.clear();
        index.entries.reserve(cache.labels.size());
        index.usable = true;
        for (std::size_t i = 0; i < cache.labels.size(); ++i) {
            const auto& label = cache.labels[i];
            if (!drawable_label(label)) continue; // Same exclusion as the narrow loop.
            const auto& local_bounds = cache.paint_layouts[i].bounds;
            const auto transform = label_transform(label, {});
            bool invertible = false;
            const auto inverse = transform.inverted(&invertible);
            if (!safe_point(label.position) || !std::isfinite(label.rotation_radians) ||
                std::abs(label.rotation_radians) > 2.0 * pi || !invertible ||
                !std::isfinite(inverse.m11()) || !std::isfinite(inverse.m12()) ||
                !std::isfinite(inverse.m21()) || !std::isfinite(inverse.m22()) ||
                !std::isfinite(transform.determinant()) ||
                std::abs(transform.determinant() - 1.0) > 1e-12 ||
                !finite_rect(local_bounds) || !(local_bounds.width() > 0.0) ||
                !(local_bounds.height() > 0.0) ||
                local_bounds.width() > safe_extent || local_bounds.height() > safe_extent) {
                index.usable = false;
                break;
            }
            const auto rotated = transform.mapRect(local_bounds);
            const Vec2 minimum{label.position.x + rotated.left() / m_scale,
                               label.position.y - rotated.bottom() / m_scale};
            const Vec2 maximum{label.position.x + rotated.right() / m_scale,
                               label.position.y - rotated.top() / m_scale};
            if (!finite_rect(rotated) || !safe_point(minimum) || !safe_point(maximum)) {
                index.usable = false;
                break;
            }
            const auto rounding = 64.0 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, std::abs(label.position.x), std::abs(label.position.y),
                    std::abs(minimum.x), std::abs(minimum.y),
                    std::abs(maximum.x), std::abs(maximum.y),
                    std::abs(rotated.left()) / m_scale, std::abs(rotated.right()) / m_scale,
                    std::abs(rotated.top()) / m_scale, std::abs(rotated.bottom()) / m_scale});
            const QRectF bounds(QPointF(minimum.x - rounding, minimum.y - rounding),
                                QPointF(maximum.x + rounding, maximum.y + rounding));
            if (!finite_rect(bounds) || !std::isfinite(bounds.width()) ||
                !std::isfinite(bounds.height())) {
                index.usable = false;
                break;
            }
            index.entries.push_back({bounds, i});
        }
        if (index.usable) {
            index.nodes.reserve(index.entries.size());
            const auto build = [&](auto&& self, std::size_t first, std::size_t count) -> std::size_t {
                auto left_edge = index.entries[first].bounds.left();
                auto top_edge = index.entries[first].bounds.top();
                auto right_edge = index.entries[first].bounds.right();
                auto bottom_edge = index.entries[first].bounds.bottom();
                for (std::size_t offset = 1; offset < count; ++offset) {
                    const auto& bounds = index.entries[first + offset].bounds;
                    left_edge = std::min(left_edge, bounds.left());
                    top_edge = std::min(top_edge, bounds.top());
                    right_edge = std::max(right_edge, bounds.right());
                    bottom_edge = std::max(bottom_edge, bounds.bottom());
                }
                const auto rounding = 4.0 * std::numeric_limits<double>::epsilon() *
                    std::max({1.0, std::abs(left_edge), std::abs(right_edge),
                              std::abs(top_edge), std::abs(bottom_edge)});
                LabelHitIndexNode node;
                node.bounds = QRectF(QPointF(left_edge - rounding, top_edge - rounding),
                                     QPointF(right_edge + rounding, bottom_edge + rounding));
                const auto node_index = index.nodes.size();
                index.nodes.push_back(node);
                if (count <= 8) {
                    index.nodes[node_index].first = first;
                    index.nodes[node_index].count = count;
                } else {
                    const bool split_x = node.bounds.width() >= node.bounds.height();
                    const auto middle = first + count / 2;
                    std::nth_element(index.entries.begin() + first, index.entries.begin() + middle,
                        index.entries.begin() + first + count, [split_x](const auto& a, const auto& b) {
                            const auto center = [split_x](const auto& entry) {
                                return split_x ? std::midpoint(entry.bounds.left(), entry.bounds.right())
                                               : std::midpoint(entry.bounds.top(), entry.bounds.bottom());
                            };
                            const auto a_center = center(a), b_center = center(b);
                            return a_center == b_center ? a.label_index < b.label_index : a_center < b_center;
                        });
                    const auto left = self(self, first, middle - first);
                    const auto right = self(self, middle, first + count - middle);
                    index.nodes[node_index].left = left;
                    index.nodes[node_index].right = right;
                }
                return node_index;
            };
            if (!index.entries.empty()) build(build, 0, index.entries.size());
        } else {
            index.entries.clear();
        }
        index.ready = true;
    }
    if (!index.usable) return std::nullopt;
    const auto overlaps = [](const QRectF& a, const QRectF& b) {
        return !(a.right() < b.left() || a.left() > b.right() ||
                 a.bottom() < b.top() || a.top() > b.bottom());
    };
    std::vector<std::size_t> result;
    std::vector<std::size_t> pending;
    if (!index.nodes.empty()) pending.push_back(0);
    while (!pending.empty()) {
        const auto& node = index.nodes[pending.back()];
        pending.pop_back();
        // Cover view subtraction, inverse rotation/translation and conversion
        // to model coordinates as well as the exact nine-pixel narrow radius.
        const auto rounding = 128.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(model.x), std::abs(model.y),
                std::abs(m_view_center.x), std::abs(m_view_center.y),
                std::abs(node.bounds.left()), std::abs(node.bounds.right()),
                std::abs(node.bounds.top()), std::abs(node.bounds.bottom()),
                (std::abs(point.x()) + std::abs(viewport.center().x())) / m_scale,
                (std::abs(point.y()) + std::abs(viewport.center().y())) / m_scale});
        const auto padding = hit_pixels / m_scale + rounding;
        const QRectF query(QPointF(model.x - padding, model.y - padding),
                           QPointF(model.x + padding, model.y + padding));
        if (!std::isfinite(padding) || !finite_rect(query) ||
            !std::isfinite(query.width()) || !std::isfinite(query.height()) ||
            !finite_rect(node.bounds)) return std::nullopt;
        if (!overlaps(node.bounds, query)) continue;
        if (node.count) {
            for (std::size_t offset = 0; offset < node.count; ++offset) {
                const auto& entry = index.entries[node.first + offset];
                if (overlaps(entry.bounds, query)) result.push_back(entry.label_index);
            }
        } else {
            pending.push_back(node.right);
            pending.push_back(node.left);
        }
    }
    // Labels are tested in paint/source order; a later zero-distance hit wins.
    std::sort(result.begin(), result.end());
    return result;
}

QString PlanCanvas::hitTest(QPointF point, bool filtered, QStringList* overlapping) const {
    constexpr double hit_pixels = 9.0;
    if (overlapping) overlapping->clear();
    const auto add_overlap = [&](const QString& id) {
        if (overlapping && !id.isEmpty() && !overlapping->contains(id)) overlapping->push_back(id);
    };
    QString result;
    QString interior_area;
    const auto model_point = toModel(point, rect());
    QTransform model_to_screen;
    model_to_screen.translate(QRectF(rect()).center().x(), QRectF(rect()).center().y());
    model_to_screen.scale(m_scale, -m_scale);
    model_to_screen.translate(-m_view_center.x, -m_view_center.y);
    auto best = std::numeric_limits<double>::max();
    const auto candidates = entityHitCandidates(point, hit_pixels);
    const auto candidate_count = candidates ? candidates->size() : m_entities.size();
    ensurePublishedEntityGeometry();
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto entity_index = candidates ? (*candidates)[index] : index;
        const auto& entity = m_entities[entity_index];
        if (filtered && !matchesSelectionType(entity.type)) continue;
        auto entity_best = std::numeric_limits<double>::max();
        bool entity_interior = false;
        const auto painted_half_width = entity_screen_stroke_width(entity, m_scale, logicalDpiX()/25.4) * 0.5;
        auto pick_path = m_published_entity_geometry[entity_index].stroke;
        append_boundary_strokes(pick_path, entity.hit_segments);
        const auto painted_footprint = model_to_screen.map(pick_path);
        QPainterPathStroker pick_stroker;
        pick_stroker.setWidth(2.0 * (hit_pixels + pick_edge_padding_pixels + painted_half_width));
        pick_stroker.setCapStyle(Qt::RoundCap);
        pick_stroker.setJoinStyle(Qt::RoundJoin);
        if (pick_stroker.createStroke(painted_footprint).contains(point)) {
            // The stroked paint path owns admission, including round caps and
            // custom seam omissions. Adaptive screen-distance only ranks hits;
            // its flattening cannot discard a point on the actual curve.
            entity_best = std::min(hit_pixels, std::max(0.0,
                point_path_distance(point, painted_footprint) - painted_half_width));
            if (entity_best < best) {
                best = entity_best;
                result = entity.id;
            }
        }
        if (entity.filled || entity.type == QStringLiteral("wall")) {
            if (const auto fill = closed_entity_path(entity)) {
                if (model_to_screen.map(*fill).contains(point)) {
                    entity_interior = true;
                    if (entity.type == QStringLiteral("boundary") || entity.type == QStringLiteral("measurement_boundary") ||
                        entity.type == QStringLiteral("room_boundary")) interior_area = entity.id;
                    else { best = 0.0; result = entity.id; }
                }
            }
        }
        if (!entity.filled && (entity.type == QStringLiteral("boundary") ||
            entity.type == QStringLiteral("measurement_boundary") || entity.type == QStringLiteral("room_boundary"))) {
            if (const auto area = closed_entity_path(entity); area && area->contains(QPointF(model_point.x,model_point.y))) {
                interior_area = entity.id;
                entity_interior = true;
            }
        }
        // Plan components are picked by their complete painted footprint, not
        // only by a thin stroke. This keeps an empty-looking seat cushion or
        // appliance centre from being misclassified as canvas space and panned.
        if (entity.type == QStringLiteral("symbol")) {
            const auto footprint = model_to_screen.map(symbol_pick_footprint(entity, pick_path));
            QPainterPathStroker tolerance;
            tolerance.setWidth(2.0 * (4.0 + pick_edge_padding_pixels));
            tolerance.setCapStyle(Qt::RoundCap);
            tolerance.setJoinStyle(Qt::RoundJoin);
            if (footprint.contains(point) || tolerance.createStroke(footprint).contains(point)) {
                best = 0.0;
                result = entity.id;
                entity_interior = true;
            }
        }
        if (entity_best <= hit_pixels || entity_interior) add_overlap(entity.id);
    }
    // Area interiors are selectable even with outline-only styling. Their
    // regions remain behind component footprints and nearby actual strokes.
    if (!interior_area.isEmpty() && best > hit_pixels) { result = interior_area; best = 0.0; }
    // Measure the same font and padded rotated rectangle as interactive paint.
    // Retain the geometry selection tolerance outside that painted rectangle.
    const auto& labels = positionedLabels(font(), this, m_scale, logicalDpiY(), false);
    const auto label_candidates = labelHitCandidates(point, hit_pixels);
    const auto& paint_layouts = m_label_placement_cache[0].paint_layouts;
    const auto label_count = label_candidates ? label_candidates->size() : labels.size();
    for (std::size_t candidate_index = 0; candidate_index < label_count; ++candidate_index) {
        const auto label_index = label_candidates ? (*label_candidates)[candidate_index] : candidate_index;
        const auto& label = labels[label_index];
        if (filtered && !matchesSelectionFilter(label.id)) continue;
        if (!drawable_label(label)) continue;
        const auto screen = toScreen(label.position, rect());
        const auto bounds = label_candidates ? paint_layouts[label_index].bounds
            : label_layout(label, font(), this, m_scale, logicalDpiY()).bounds;
        const auto local = label_transform(label, screen).inverted().map(point);
        const auto dx = std::max({bounds.left() - local.x(), 0.0,
                                  local.x() - bounds.right()});
        const auto dy = std::max({bounds.top() - local.y(), 0.0,
                                  local.y() - bounds.bottom()});
        const auto candidate = std::hypot(dx, dy);
        if (candidate <= hit_pixels) add_overlap(label.id);
        // Labels paint after geometry; a hit inside their painted rectangle
        // wins a zero-distance tie, including later overlapping labels.
        if (candidate < best || candidate == 0.0) {
            best = candidate;
            result = label.id;
        }
    }
    if (best <= hit_pixels && !overlapping) return result;
    if (best > hit_pixels) result.clear();
    const auto model = toModel(point, rect());
    // Underlays sit beneath geometry; pick the topmost visible reference only
    // when no authored geometry or label was hit.
    for (auto it = m_references.rbegin(); it != m_references.rend(); ++it) {
        const auto& reference = *it;
        if (filtered && m_selection_filter != CanvasSelectionFilter::all &&
            m_selection_filter != CanvasSelectionFilter::references) continue;
        if (!reference.visible || reference.image.isNull() ||
            !std::isfinite(reference.rotation_degrees)) continue;
        const auto radians = reference.rotation_degrees * std::numbers::pi / 180.0;
        const auto dx = model.x - reference.position.x;
        const auto dy = model.y - reference.position.y;
        const auto x = dx * std::cos(radians) + dy * std::sin(radians);
        const auto y = -dx * std::sin(radians) + dy * std::cos(radians);
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (std::isfinite(unit) && unit > 0.0 &&
            std::abs(x) <= reference.image.width() * unit * 0.5 &&
            std::abs(y) <= reference.image.height() * unit * 0.5) {
            if (!overlapping) return reference.id;
            add_overlap(reference.id);
            if (result.isEmpty()) result=reference.id;
        }
    }
    if (overlapping && !result.isEmpty()) {
        overlapping->removeAll(result);
        overlapping->push_front(result);
    }
    return result;
}

void PlanCanvas::invalidateRetainedPresentation() {
    clearSymbolDragPreview();
    m_content_bounds_cache = {};
    m_overview_geometry_cache = {};
    m_retained_selection_ready = false;
    m_selection_bounds_cache = {};
    m_selection_frame_cache = {};
    m_selection_axes_key.clear();
    m_selection_axes_cache.reset();
    for (auto& cache : m_label_placement_cache) cache = {};
}

bool PlanCanvas::hasInteractivePresentation() const {
    // Bypass settled caches throughout capture, deferred completion, rejection
    // and release. Preview contents need not carry unique IDs or revisions.
    return m_move_preview_delta || m_transform_frame_start || m_vertex_move_handle ||
        m_opening_width_handle || m_vertex_move_preview || m_opening_width_jamb_preview ||
        m_left_gesture == LeftGesture::selection_resize ||
        m_left_gesture == LeftGesture::selection_rotate ||
        m_left_gesture == LeftGesture::selection_axis_resize ||
        m_transform_scale_preview != 1.0 || m_transform_rotation_preview != 0.0 ||
        m_axis_scale_x_preview != 1.0 || m_axis_scale_y_preview != 1.0 ||
        m_opening_width_scale_preview != 1.0 ||
        m_move_preview_exact || m_move_preview_valid || m_move_preview_pending ||
        m_move_preview_request_in_progress || m_move_release_pending ||
        m_transform_preview_exact || m_transform_preview_valid || m_transform_preview_pending ||
        m_transform_preview_request_in_progress || m_transform_release_pending ||
        m_boundary_vertex_preview_valid || m_boundary_vertex_preview_pending ||
        m_boundary_vertex_preview_request_in_progress || m_vertex_release_pending ||
        m_opening_width_preview_valid || m_opening_width_preview_pending ||
        m_opening_width_preview_request_in_progress ||
        !m_move_entities_preview.empty() || !m_move_labels_preview.empty() ||
        !m_move_references_preview.empty() || !m_transform_entities_preview.empty() ||
        !m_transform_labels_preview.empty() || !m_transform_references_preview.empty() ||
        !m_boundary_vertex_entities_preview.empty() || !m_boundary_vertex_labels_preview.empty() ||
        !m_opening_width_entities_preview.empty() || !m_opening_width_labels_preview.empty();
}

QByteArray PlanCanvas::retainedSelectionKey(const QRectF& viewport, bool model_axes) const {
    QByteArray key;
    QDataStream signature(&key, QIODevice::WriteOnly);
    signature << font() << m_scale << m_metric_units << logicalDpiX() << logicalDpiY()
              << devicePixelRatioF() << viewport << model_axes;
    if (!model_axes) signature << m_view_center.x << m_view_center.y;
    return key;
}

void PlanCanvas::ensureRetainedSelection() const {
    if (m_retained_selection_ready) return;
    m_retained_selected_ids.clear();
    m_selected_entity_indices.clear();
    m_has_selected_label = false;
    m_has_selected_reference = false;
    const auto add = [&](const QString& id) {
        if (!id.isEmpty() && !m_retained_selected_ids.contains(id))
            m_retained_selected_ids.push_back(id);
    };
    for (std::size_t index = 0; index < m_entities.size(); ++index) {
        const auto& entity = m_entities[index];
        if (!entity.selected) continue;
        m_selected_entity_indices.push_back(index);
        add(entity.id);
    }
    for (const auto& label : m_labels) if (label.selected) {
        m_has_selected_label = true;
        add(label.id);
    }
    for (const auto& reference : m_references) if (reference.selected) {
        m_has_selected_reference = true;
        add(reference.id);
    }
    m_retained_selection_ready = true;
}

const QStringList& PlanCanvas::selectedIds() const {
    ensureRetainedSelection();
    return m_retained_selected_ids;
}

bool PlanCanvas::selectionInteractionEnabled() const {
    return m_selection_controls_visible && (m_tool == CanvasTool::select || m_tool == CanvasTool::boundary) &&
           !m_boundary_draft_preview;
}

QString PlanCanvas::contextTarget(QPointF point) const {
    // The same visible frame owns movement and contextual selection actions.
    // A retained selection takes precedence over unrelated geometry under it.
    if (selectionInteractionEnabled()) {
        const auto ids = selectedIds();
        const auto frame = selectionFrame(QRectF(rect()));
        if (!ids.isEmpty() && frame && frame->contains(point) && matchesSelectionFilter(ids.back()))
            return ids.back();
    }
    return hitTest(point);
}

Vec2 PlanCanvas::dragDelta(QPointF position) const {
    const auto start = toModel(m_left_start, rect());
    const auto end = toModel(position, rect());
    if (!m_snap_enabled) return {end.x - start.x, end.y - start.y};
    const auto snapped_start = snapped(start);
    const auto snapped_end = snapped(end);
    return {snapped_end.x - snapped_start.x, snapped_end.y - snapped_start.y};
}

void PlanCanvas::updatePointerCursor(QPointF point) {
    if (m_opening_move_active && m_left_dragging && m_move_preview_exact &&
        !m_move_preview_valid && !m_move_preview_pending) {
        setCursor(Qt::ForbiddenCursor);
    } else if (m_panning ||
        ((m_left_gesture == LeftGesture::object_move ||
          m_left_gesture == LeftGesture::vertex_move) && m_left_dragging)) {
        setCursor(Qt::ClosedHandCursor);
    } else if (m_space_pan_armed && m_gesture_button == Qt::NoButton) {
        setCursor(Qt::OpenHandCursor);
    } else if (selectionInteractionEnabled() && m_gesture_button == Qt::NoButton) {
        if (const auto jamb = openingWidthHandleAt(point, QRectF(rect()))) {
            setCursor(jamb_resize_cursor(jamb->source, jamb->keep_start_jamb));
            return;
        }
        if (vertexHandleAt(point, QRectF(rect()))) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
        const auto handle = selectionHandleAt(point, QRectF(rect()));
        if (handle != SelectionHandle::none) {
            setCursor(handle == SelectionHandle::resize ? Qt::SizeFDiagCursor :
                      handle == SelectionHandle::rotate ? Qt::CrossCursor :
                      handle == SelectionHandle::left || handle == SelectionHandle::right
                        ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            return;
        }
        const auto frame = selectionFrame(QRectF(rect()));
        if (frame && frame->contains(point)) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
        const auto target = hitTest(point);
        setCursor(target.isEmpty() ? Qt::CrossCursor : Qt::ArrowCursor);
    } else if (m_gesture_button == Qt::NoButton) {
        setCursor(Qt::CrossCursor);
    }
}

void PlanCanvas::setEntityTransformRequested(
    std::function<bool(QString, double, double)> callback) {
    if (m_transform_frame_start) resetGesture();
    m_entity_transform_requested = std::move(callback);
}

void PlanCanvas::setEntityEditGestureStarted(std::function<void(QString)> callback) {
    m_entity_edit_gesture_started = std::move(callback);
}

void PlanCanvas::setInteractionAdmissionRequested(std::function<bool(bool)> callback) {
    m_interaction_admission_requested = std::move(callback);
}

bool PlanCanvas::admitInteraction(bool context) {
    try {
        return !m_interaction_admission_requested || m_interaction_admission_requested(context);
    } catch (...) {
        return false; // An observer cannot authorize a partially initialized gesture.
    }
}

void PlanCanvas::setEntityTransformStarted(std::function<void(QString)> callback) {
    resetGesture();
    m_entity_transform_started = std::move(callback);
}

void PlanCanvas::setEntityTransformPreviewRequested(
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, double, Vec2, std::uint64_t)> callback) {
    resetGesture();
    m_entity_transform_preview_requested = std::move(callback);
}

void PlanCanvas::updateEntityTransformPreview() {
    const auto serial = ++m_transform_preview_serial;
    m_transform_entities_preview.clear();
    m_transform_entities_preview_index.clear();
    m_transform_labels_preview.clear(); m_transform_references_preview.clear();
    m_transform_preview_exact = false;
    m_transform_preview_valid = false;
    m_transform_preview_pending = false;
    m_transform_preview_request_in_progress = false;
    const bool axis_resize = m_left_gesture == LeftGesture::selection_axis_resize;
    const auto callback = axis_resize ? m_entity_axis_resize_preview_requested
                                     : m_entity_transform_preview_requested;
    if (!callback || m_transform_source_id.isEmpty()) return;
    m_transform_preview_request_in_progress = true;
    std::optional<std::vector<CanvasEntity>> proposed;
    bool provider_failed = false;
    const QPointer<PlanCanvas> guard(this);
    try {
        proposed = callback(m_transform_source_id,
            axis_resize ? m_axis_scale_x_preview : m_transform_scale_preview,
            axis_resize ? m_axis_scale_y_preview : m_transform_rotation_preview,
            axis_resize ? m_axis_anchor : m_transform_pivot, serial);
    } catch (...) { provider_failed = true; }
    if (!guard || serial != m_transform_preview_serial || !m_transform_preview_request_in_progress) return;
    m_transform_preview_request_in_progress = false;
    // Failure after marking pending must reject rather than wait forever or
    // authorize the providerless affine release path.
    if (provider_failed) {
        (void)applyEntityTransformPreview(serial, std::vector<CanvasEntity>{});
        return;
    }
    // A host can supply labels by marking/completing this serial inside the
    // callback. Its completion is authoritative over the callback's return.
    if (!m_transform_preview_exact && proposed)
        (void)applyEntityTransformPreview(serial, std::move(proposed));
}

bool PlanCanvas::markEntityTransformPreviewPending(std::uint64_t serial) {
    if (serial != m_transform_preview_serial || !m_transform_preview_request_in_progress ||
        !m_transform_frame_start || m_transform_source_id.isEmpty() ||
        (m_left_gesture != LeftGesture::selection_resize &&
         m_left_gesture != LeftGesture::selection_rotate &&
         m_left_gesture != LeftGesture::selection_axis_resize)) return false;
    m_transform_preview_exact = true;
    m_transform_preview_pending = true;
    return true;
}

bool PlanCanvas::completeEntityTransformPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels, std::vector<CanvasReference> references) {
    if (!m_transform_preview_pending) return false;
    return applyEntityTransformPreview(serial, std::move(result), std::move(labels), std::move(references));
}

bool PlanCanvas::applyEntityTransformPreview(std::uint64_t serial,
    std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels, std::vector<CanvasReference> references) {
    if (serial != m_transform_preview_serial || !m_transform_frame_start ||
        m_transform_source_id.isEmpty() ||
        (m_left_gesture != LeftGesture::selection_resize &&
         m_left_gesture != LeftGesture::selection_rotate &&
         m_left_gesture != LeftGesture::selection_axis_resize)) return false;
    m_transform_preview_pending = false;
    m_transform_preview_exact = true;
    m_transform_preview_valid = result && valid_reference_previews(references, m_references) && unambiguous_entity_presentations(*result, m_entities) && (
        std::any_of(result->begin(), result->end(), [&](const auto& entity) { return entity.id == m_transform_source_id; }) ||
        std::any_of(references.begin(), references.end(), [&](const auto& reference) { return reference.id == m_transform_source_id; }));
    m_transform_references_preview = m_transform_preview_valid ? std::move(references) : std::vector<CanvasReference>{};
    if (!m_transform_references_preview.empty()) {
        QSet<QString> selected_references;
        for (const auto& retained : m_references)
            if (retained.selected) selected_references.insert(retained.id);
        for (auto& proposed : m_transform_references_preview)
            proposed.selected = selected_references.contains(proposed.id);
    }
    m_transform_entities_preview_index.clear();
    m_transform_entities_preview = m_transform_preview_valid
        ? std::move(*result) : std::vector<CanvasEntity>{};
    m_transform_labels_preview = m_transform_preview_valid
        ? std::move(labels) : std::vector<CanvasLabel>{};
    // Preview IDs may newly enter the view, but only an exact proposal rooted
    // in the selected source can expose them. Preserve retained selection.
    if (!m_transform_entities_preview.empty()) {
        ensureRetainedSelection();
        QSet<QString> selected_entities;
        for (const auto index : m_selected_entity_indices) selected_entities.insert(m_entities[index].id);
        for (auto& proposed : m_transform_entities_preview)
            proposed.selected = selected_entities.contains(proposed.id);
    }
    if (m_transform_preview_valid)
        rebuildEntityPresentationIndex(m_transform_entities_preview_index, m_transform_entities_preview);
    if (!m_transform_labels_preview.empty()) {
        QHash<QString, QSet<QString>> selected_labels;
        for (const auto& retained : m_labels)
            if (retained.selected) selected_labels[retained.id].insert(retained.callout_role);
        for (auto& proposed : m_transform_labels_preview) {
            const auto selected = selected_labels.constFind(proposed.id);
            proposed.selected = selected != selected_labels.cend() && selected.value().contains(proposed.callout_role);
        }
    }
    auto cursor = Qt::ForbiddenCursor;
    if (m_transform_preview_valid) {
        cursor = m_left_gesture == LeftGesture::selection_rotate ? Qt::CrossCursor : Qt::SizeFDiagCursor;
        if (m_left_gesture == LeftGesture::selection_axis_resize)
            cursor = m_axis_handle == SelectionHandle::left || m_axis_handle == SelectionHandle::right
                ? Qt::SizeHorCursor : Qt::SizeVerCursor;
    }
    setCursor(cursor);
    update();
    if (m_transform_release_pending) {
        QTimer::singleShot(0, this, [this, serial] {
            if (m_transform_release_pending) finishEntityTransformPreview(serial);
        });
    }
    return true;
}

void PlanCanvas::finishEntityTransformPreview(std::uint64_t serial) {
    if (serial != m_transform_preview_serial || !m_transform_frame_start ||
        m_transform_preview_pending || !m_transform_preview_exact) return;
    const auto id = m_transform_source_id;
    const auto scale = m_transform_scale_preview;
    const auto radians = m_transform_rotation_preview;
    const auto axis_scale_x = m_axis_scale_x_preview;
    const auto axis_scale_y = m_axis_scale_y_preview;
    const auto axis_anchor = m_axis_anchor;
    const auto gesture = m_left_gesture;
    const auto accepted = m_transform_preview_valid;
    // Consume release before the host can run a nested event loop, while
    // retaining the serial and source capture for its command admission.
    m_transform_release_pending = false;
    m_gesture_button = Qt::NoButton;
    // The host admits the captured exact command using this live serial.
    // Scene replacement in the callback may itself invalidate the gesture.
    const QPointer<PlanCanvas> guard(this);
    if (accepted && gesture == LeftGesture::selection_axis_resize) {
        const auto callback = m_entity_axis_resize_requested;
        if (callback) (void)callback(id, axis_scale_x, axis_scale_y, axis_anchor);
    } else if (accepted) {
        const auto callback = m_entity_transform_requested;
        if (callback) (void)callback(id, scale, radians);
    }
    if (guard && serial == m_transform_preview_serial) resetGesture();
}

void PlanCanvas::setEntityAxisResizeRequested(
    std::function<bool(QString, double, double, Vec2)> callback) {
    m_entity_axis_resize_requested = std::move(callback);
}

void PlanCanvas::setEntityAxisResizePreviewRequested(
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, double, Vec2, std::uint64_t)> callback) {
    resetGesture();
    m_entity_axis_resize_preview_requested = std::move(callback);
}

void PlanCanvas::setOpeningWidthPreviewRequested(std::function<std::optional<std::vector<CanvasEntity>>(
    QString, double, bool, std::uint64_t)> callback) {
    if (m_opening_width_handle) resetGesture();
    m_opening_width_preview_requested = std::move(callback);
    update();
}

void PlanCanvas::setOpeningWidthResizeRequested(
    std::function<bool(QString, double, bool, std::uint64_t)> callback) {
    if (m_opening_width_handle) resetGesture();
    m_opening_width_resize_requested = std::move(callback);
    update();
}

void PlanCanvas::setBoundaryVertexPreviewRequested(
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, QString, Vec2, std::uint64_t)> callback) {
    if (m_vertex_move_handle) resetGesture();
    m_boundary_vertex_preview_requested = std::move(callback);
    update();
}

void PlanCanvas::setBoundaryVertexMoveRequested(
    std::function<bool(QString, QString, Vec2, std::uint64_t)> callback) {
    if (m_vertex_move_handle) resetGesture();
    m_boundary_vertex_move_requested = std::move(callback);
}

void PlanCanvas::updateCursor(QPointF point) {
    m_last_mouse_position = point;
    if (m_cursor_moved) {
        m_cursor_moved(inputPoint(point));
    }
    updatePointerCursor(point);
    update();
}

void PlanCanvas::drawGrid(QPainter& painter, const QRectF& viewport, double scale,
                          Vec2 view_center) const {
    const auto to_model = [&](QPointF point) {
        return Vec2{view_center.x + (point.x() - viewport.center().x()) / scale,
                    view_center.y - (point.y() - viewport.center().y()) / scale};
    };
    const auto top_left = to_model(viewport.topLeft());
    const auto bottom_right = to_model(viewport.bottomRight());
    const auto min_x = std::min(top_left.x, bottom_right.x);
    const auto max_x = std::max(top_left.x, bottom_right.x);
    const auto min_y = std::min(top_left.y, bottom_right.y);
    const auto max_y = std::max(top_left.y, bottom_right.y);

    const auto spacing = grid_spacing(scale, m_metric_units);
    const auto step = spacing.minor;
    const bool light_surface = m_canvas_background.lightnessF() > 0.5;
    const bool high_contrast = m_canvas_background == QColor(Qt::black);
    QPen minor(high_contrast ? QColor(96, 96, 96)
                             : light_surface ? QColor(232, 237, 243) : QColor(43, 51, 62), 0.0);
    QPen major(high_contrast ? QColor(160, 160, 160)
                             : light_surface ? QColor(207, 217, 229) : QColor(58, 68, 82), 0.0);
    const auto first_x = std::floor(min_x / step) * step;
    const auto first_y = std::floor(min_y / step) * step;
    const auto major_multiple = std::round(spacing.major / step);
    const auto pen_for = [&](double coordinate) {
        const auto index = std::round(coordinate / step);
        return std::fmod(std::abs(index), major_multiple) < 0.001 ? major : minor;
    };
    // Count from device extents instead of incrementing world coordinates:
    // rounding at distant centers must never create a nonprogressing loop.
    const auto columns = std::ceil(viewport.width() / (step * scale)) + 2.0;
    const auto rows = std::ceil(viewport.height() / (step * scale)) + 2.0;
    for (double index = 0; index < columns; ++index) {
        const auto x = first_x + index * step;
        painter.setPen(pen_for(x));
        painter.drawLine(QLineF(x, min_y, x, max_y));
    }
    for (double index = 0; index < rows; ++index) {
        const auto y = first_y + index * step;
        painter.setPen(pen_for(y));
        painter.drawLine(QLineF(min_x, y, max_x, y));
    }
}

void PlanCanvas::drawReferenceGrids(QPainter& painter) const {
    const bool light_surface = m_canvas_background.lightnessF() > 0.5;
    const QColor minor_color = light_surface ? QColor(121, 149, 181, 135)
                                             : QColor(136, 178, 221, 155);
    const QColor major_color = light_surface ? QColor(65, 111, 157, 205)
                                             : QColor(176, 214, 244, 220);
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            if (!std::isfinite(line.start.x) || !std::isfinite(line.start.y) ||
                !std::isfinite(line.end.x) || !std::isfinite(line.end.y)) {
                continue;
            }
            QPen pen(line.major ? major_color : minor_color,
                     line.major ? 1.35 : 0.75, Qt::SolidLine,
                     Qt::SquareCap, Qt::MiterJoin);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawLine(QLineF(line.start.x, line.start.y, line.end.x, line.end.y));
        }
    }
}

void PlanCanvas::drawReferenceGridLabels(QPainter& painter, const QRectF& viewport,
                                         double scale, Vec2 view_center, bool output,
                                         QColor background,
                                         std::optional<double> paper_pixels_per_mm) const {
    if (!(scale > 0.0) || !std::isfinite(scale)) return;
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };

    double dpi = output ? painter.device()->logicalDpiY() : logicalDpiY();
    if (output && paper_pixels_per_mm.has_value() && *paper_pixels_per_mm > 0.0 &&
        std::isfinite(*paper_pixels_per_mm * 25.4)) {
        dpi = *paper_pixels_per_mm * 25.4;
    }
    const auto pixel_height = output
        ? std::clamp(std::lround(2.5 * dpi / 25.4), 8L, 48L)
        : 11L;
    QFont font = painter.font();
    font.setPixelSize(static_cast<int>(pixel_height));
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font, painter.device());
    const bool light_surface = background.lightnessF() > 0.5;
    const auto foreground = light_surface ? QColor(47, 83, 121, 235)
                                          : QColor(202, 226, 246, 245);
    const auto fill = light_surface ? QColor(255, 255, 255, 228)
                                    : QColor(18, 30, 45, 232);
    const auto border = light_surface ? QColor(134, 164, 194, 210)
                                      : QColor(93, 132, 171, 230);

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setClipRect(viewport);
    painter.setFont(font);
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            const auto& prefix = line.axis == ReferenceGridAxis::x ? grid.x_label : grid.y_label;
            if (prefix.isEmpty() || !std::isfinite(line.start.x) ||
                !std::isfinite(line.start.y) || !std::isfinite(line.end.x) ||
                !std::isfinite(line.end.y)) {
                continue;
            }
            const auto start = to_screen(line.start);
            const auto end = to_screen(line.end);
            const auto outward = line.axis == ReferenceGridAxis::x
                ? end - start
                : start - end;
            const auto length = std::hypot(outward.x(), outward.y());
            if (!(length > 0.0) || !std::isfinite(length)) continue;
            const auto unit = outward / length;
            auto anchor = line.axis == ReferenceGridAxis::x ? end : start;
            anchor += unit * 6.0;
            const auto text = prefix + QString::number(line.index);
            auto bounds = metrics.boundingRect(text);
            bounds.moveCenter(anchor);
            bounds.adjust(-4.0, -2.0, 4.0, 2.0);
            painter.setPen(QPen(border, 1.0));
            painter.setBrush(fill);
            painter.drawRoundedRect(bounds, 3.0, 3.0);
            painter.setPen(foreground);
            painter.setBrush(Qt::NoBrush);
            painter.drawText(bounds, Qt::AlignCenter, text);
        }
    }
    painter.restore();
}

void PlanCanvas::drawDrawingWitnesses(QPainter& painter, const QRectF& viewport, double scale,
                                      Vec2 view_center, QColor background) const {
    if (m_drawing_witnesses.empty() ||
        (m_tool != CanvasTool::boundary && m_tool != CanvasTool::wall)) return;

    painter.save();
    painter.setClipRect(viewport);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const bool light = background.lightnessF() > 0.5;
    const auto surface = light ? QColor(255, 255, 255, 244) : QColor(24, 34, 49, 246);
    const auto foreground = light ? QColor(31, 48, 69) : QColor(232, 240, 251);
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };
    std::vector<QRectF> label_bounds;
    for (const auto& witness : m_drawing_witnesses) {
        const auto start = to_screen(witness.segment.start);
        const auto end = to_screen(witness.segment.end);
        if (!std::isfinite(start.x()) || !std::isfinite(start.y()) ||
            !std::isfinite(end.x()) || !std::isfinite(end.y())) continue;
        const QColor accent = witness.horizontal
            ? (light ? QColor(37, 99, 235) : QColor(103, 202, 255))
            : (light ? QColor(126, 58, 190) : QColor(202, 158, 255));
        painter.setPen(QPen(accent, witness.selected ? 2.4 : 1.4,
                            Qt::DashLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(start, end);
        const double radius = witness.selected ? 7.0 : 5.0;
        painter.setPen(QPen(accent, witness.selected ? 2.2 : 1.5));
        painter.setBrush(witness.selected ? accent : surface);
        painter.drawEllipse(end, radius, radius);
        painter.setPen(QPen(witness.selected ? surface : accent, 1.2));
        painter.drawLine(end + QPointF(-3.0, 0.0), end + QPointF(3.0, 0.0));
        painter.drawLine(end + QPointF(0.0, -3.0), end + QPointF(0.0, 3.0));

        QFont font = painter.font();
        font.setPixelSize(12);
        font.setWeight(witness.selected ? QFont::DemiBold : QFont::Medium);
        painter.setFont(font);
        QString text = witness.command_text.isEmpty()
            ? (witness.horizontal ? QStringLiteral("X") : QStringLiteral("Y"))
            : witness.command_text;
        if (!witness.dimension_text.isEmpty()) text += QStringLiteral("  ") + witness.dimension_text;
        if (witness.selected) text += QStringLiteral("  •  ") + tr("Enter");
        const QFontMetricsF metrics(font, painter.device());
        const QSizeF size(metrics.horizontalAdvance(text) + 12.0, metrics.height() + 6.0);
        const QPointF midpoint((start.x() + end.x()) * 0.5, (start.y() + end.y()) * 0.5);
        QPointF position = witness.horizontal
            ? midpoint + QPointF(-size.width() * 0.5, -size.height() - 12.0)
            : midpoint + QPointF(12.0, -size.height() * 0.5);
        const auto clamp_position = [&](QPointF point) {
            point.setX(std::clamp(point.x(), viewport.left() + 6.0,
                std::max(viewport.left() + 6.0, viewport.right() - size.width() - 6.0)));
            point.setY(std::clamp(point.y(), viewport.top() + 6.0,
                std::max(viewport.top() + 6.0, viewport.bottom() - size.height() - 6.0)));
            return point;
        };
        QRectF bounds(clamp_position(position), size);
        for (const auto& occupied : label_bounds) {
            if (bounds.intersects(occupied.adjusted(-3.0, -3.0, 3.0, 3.0)))
                bounds.moveTopLeft(clamp_position(QPointF(bounds.left(), occupied.bottom() + 6.0)));
        }
        label_bounds.push_back(bounds);
        painter.setPen(QPen(accent, witness.selected ? 1.8 : 1.0));
        painter.setBrush(surface);
        painter.drawRoundedRect(bounds, 3.0, 3.0);
        painter.setPen(foreground);
        painter.drawText(bounds, Qt::AlignCenter, text);
    }
    painter.restore();
}

void PlanCanvas::drawCursorReadout(QPainter& painter, const QRectF& viewport,
                                   QColor background) const {
    if (!m_last_mouse_position || m_tool == CanvasTool::select ||
        !viewport.contains(*m_last_mouse_position)) {
        return;
    }
    const auto point = inputPoint(*m_last_mouse_position);
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;

    QString text = QStringLiteral("X %1   Y %2")
        .arg(display_cursor_length(point.x, m_metric_units),
             display_cursor_length(point.y, m_metric_units));
    if (const auto origin=drawingOrigin()) {
        const auto anchor = *origin;
        const auto dx = point.x - anchor.x;
        const auto dy = point.y - anchor.y;
        const auto length = std::hypot(dx, dy);
        if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(length)) {
            const auto angle = std::atan2(dy, dx) * 180.0 / pi;
            text += QStringLiteral("\nΔ %1   ∠ %2°")
                .arg(drawingLengthText(length, m_metric_units))
                .arg(angle, 0, 'f', 1);
        }
    }

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setWeight(QFont::Medium);
    painter.setFont(font);
    const QFontMetricsF metrics(font, painter.device());
    const QRectF text_bounds = metrics.boundingRect(QRectF(), Qt::TextExpandTabs, text);
    const QRectF panel_bounds = text_bounds.adjusted(-9.0, -6.0, 9.0, 6.0);
    const bool light_surface = background.lightnessF() > 0.5;
    const auto border = light_surface ? QColor(176, 191, 211, 235)
                                      : QColor(98, 119, 149, 245);
    const auto surface = light_surface ? QColor(255, 255, 255, 244)
                                       : QColor(24, 34, 49, 246);
    const auto foreground = light_surface ? QColor(31, 48, 69)
                                          : QColor(232, 240, 251);
    QPointF top_left = *m_last_mouse_position + QPointF(14.0, 14.0);
    if (top_left.x() + panel_bounds.width() > viewport.right() - 8.0) {
        top_left.setX(m_last_mouse_position->x() - panel_bounds.width() - 14.0);
    }
    if (top_left.y() + panel_bounds.height() > viewport.bottom() - 8.0) {
        top_left.setY(m_last_mouse_position->y() - panel_bounds.height() - 14.0);
    }
    top_left.setX(std::clamp(top_left.x(), viewport.left() + 8.0,
                             viewport.right() - panel_bounds.width() - 8.0));
    top_left.setY(std::clamp(top_left.y(), viewport.top() + 8.0,
                             viewport.bottom() - panel_bounds.height() - 8.0));
    const QRectF panel(top_left, panel_bounds.size());
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(surface);
    painter.drawRoundedRect(panel, 6.0, 6.0);
    painter.setPen(foreground);
    painter.drawText(panel.adjusted(9.0, 6.0, -9.0, -6.0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, text);
    painter.restore();
}

void PlanCanvas::ensurePublishedEntityGeometry() const {
    if (m_published_entity_geometry.size() == m_entities.size()) return;
    // Construct once in model coordinates with the same analytical arc and
    // closed-path helpers as fresh painting. Painter transforms, cosmetic pens
    // and brush transforms remain device-specific at draw time.
    std::vector<EntityGeometry> geometry;
    geometry.reserve(m_entities.size());
    for (const auto& entity : m_entities) {
        EntityGeometry paths;
        append_boundary_strokes(paths.stroke,
            entity.stroke_segments ? *entity.stroke_segments : entity.segments);
        for (const auto& hole : entity.holes) append_boundary_strokes(paths.stroke, hole);
        if (entity.filled) paths.fill = closed_entity_path(entity);
        const auto finite_boundary = [](const Boundary& boundary) {
            return std::all_of(boundary.begin(), boundary.end(), [](const auto& segment) {
                return std::isfinite(segment.start.x) && std::isfinite(segment.start.y) &&
                       std::isfinite(segment.end.x) && std::isfinite(segment.end.y) &&
                       std::isfinite(segment.sweep_radians);
            });
        };
        const bool safe = finite_boundary(entity.segments) &&
            (!entity.stroke_segments || finite_boundary(*entity.stroke_segments)) &&
            std::all_of(entity.holes.begin(), entity.holes.end(), finite_boundary);
        if (safe) {
            bool bounds_safe = true;
            const auto include = [&](const QPainterPath& path) {
                if (path.isEmpty()) return;
                const auto bounds = path.controlPointRect();
                if (!finite_rect(bounds)) { bounds_safe = false; return; }
                if (!paths.bounds) paths.bounds = bounds;
                else {
                    // QRectF::united can discard empty (line) rectangles.
                    // Explicit extrema retain horizontal and vertical strokes.
                    const auto left = std::min(paths.bounds->left(), bounds.left());
                    const auto top = std::min(paths.bounds->top(), bounds.top());
                    const auto right = std::max(paths.bounds->right(), bounds.right());
                    const auto bottom = std::max(paths.bounds->bottom(), bounds.bottom());
                    paths.bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
                }
            };
            include(paths.stroke);
            if (paths.fill) include(*paths.fill);
            if (!bounds_safe || (paths.bounds && !finite_rect(*paths.bounds))) paths.bounds.reset();
        }
        geometry.push_back(std::move(paths));
    }
    m_published_entity_geometry = std::move(geometry);
}

void PlanCanvas::ensurePublishedGeometryIndex() const {
    if (m_published_geometry_index_ready) return;
    ensurePublishedEntityGeometry();
    auto& entries = m_published_geometry_index_entries;
    auto& nodes = m_published_geometry_index_nodes;
    auto& fallback = m_published_geometry_index_fallback;
    entries.clear();
    nodes.clear();
    fallback.clear();
    entries.reserve(m_entities.size());
    for (std::size_t index = 0; index < m_entities.size(); ++index) {
        const auto& entity = m_entities[index];
        const auto& geometry = m_published_entity_geometry[index];
        if (!geometry.bounds || entity.svg_symbol || entity.dimension_end_ticks ||
            entity.type == QStringLiteral("dimension_line") ||
            entity.type == QStringLiteral("section_overlay")) {
            fallback.push_back(index);
            continue;
        }
        GeometryIndexEntry entry;
        entry.entity_index = index;
        const auto envelope = entity_stroke_envelope(entity);
        if (!envelope) { fallback.push_back(index); continue; }
        const auto model_width = envelope->model_width;
        entry.paper_mm = envelope->paper_mm;
        entry.cosmetic_pixels = envelope->cosmetic_pixels;
        entry.symbol_metres = envelope->symbol_metres;
        const auto& bounds = *geometry.bounds;
        // Outward rounding protects huge survey coordinates and degenerate
        // horizontal/vertical paths. Unrepresentable envelopes stay unindexed.
        const auto rounding = 32.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(bounds.left()), std::abs(bounds.right()),
                      std::abs(bounds.top()), std::abs(bounds.bottom()), model_width});
        const auto padding = .5 * model_width + rounding;
        entry.bounds = bounds.adjusted(-padding, -padding, padding, padding);
        if (!std::isfinite(model_width) || model_width < 0.0 ||
            !std::isfinite(padding) || !finite_rect(entry.bounds) ||
            !std::isfinite(entry.bounds.width()) || !std::isfinite(entry.bounds.height())) {
            fallback.push_back(index);
            continue;
        }
        entries.push_back(entry);
    }
    // Median partitions bound tree depth even for coincident/degenerate paths.
    // Each entity has one leaf; there is no grid duplication or zoom rebuild.
    nodes.reserve(entries.size());
    const auto build = [&](auto&& self, std::size_t first, std::size_t count) -> std::size_t {
        GeometryIndexNode node;
        auto left_edge = entries[first].bounds.left();
        auto top_edge = entries[first].bounds.top();
        auto right_edge = entries[first].bounds.right();
        auto bottom_edge = entries[first].bounds.bottom();
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& entry = entries[first + offset];
            left_edge = std::min(left_edge, entry.bounds.left());
            top_edge = std::min(top_edge, entry.bounds.top());
            right_edge = std::max(right_edge, entry.bounds.right());
            bottom_edge = std::max(bottom_edge, entry.bounds.bottom());
            node.cosmetic_pixels = std::max(node.cosmetic_pixels, entry.cosmetic_pixels);
            node.symbol_metres = std::max(node.symbol_metres, entry.symbol_metres);
            node.paper_mm = std::max(node.paper_mm, entry.paper_mm);
        }
        // Form the rectangle once, so QRectF's origin/extent conversion cannot
        // accumulate rounding while unioning a large retained drawing.
        const auto rounding = 4.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(left_edge), std::abs(top_edge),
                      std::abs(right_edge), std::abs(bottom_edge)});
        node.bounds = QRectF(QPointF(left_edge - rounding, top_edge - rounding),
                             QPointF(right_edge + rounding, bottom_edge + rounding));
        const auto node_index = nodes.size();
        nodes.push_back(node);
        if (count <= 8) {
            nodes[node_index].first = first;
            nodes[node_index].count = count;
        } else {
            const bool split_x = node.bounds.width() >= node.bounds.height();
            const auto middle = first + count / 2;
            std::nth_element(entries.begin() + first, entries.begin() + middle,
                entries.begin() + first + count, [split_x](const auto& left, const auto& right) {
                    const auto center = [split_x](const auto& entry) {
                        return split_x ? std::midpoint(entry.bounds.left(), entry.bounds.right())
                                       : std::midpoint(entry.bounds.top(), entry.bounds.bottom());
                    };
                    const auto left_center = center(left), right_center = center(right);
                    return left_center == right_center ? left.entity_index < right.entity_index
                                                       : left_center < right_center;
                });
            const auto left = self(self, first, middle - first);
            const auto right = self(self, middle, first + count - middle);
            nodes[node_index].left = left;
            nodes[node_index].right = right;
        }
        return node_index;
    };
    if (!entries.empty()) build(build, 0, entries.size());
    m_published_geometry_index_ready = true;
}

std::optional<std::vector<std::size_t>> PlanCanvas::visiblePublishedEntityIndices(
    const QTransform& model_to_device, const QTransform& canvas_to_device,
    const QRectF& device_viewport) const {
    // The existing navigation/culling proof covers finite axis-aligned affine
    // transforms. Other caller transforms retain the original full traversal.
    if (!ordinary_axis_transform(model_to_device) || !ordinary_axis_transform(canvas_to_device) ||
        !finite_rect(device_viewport) || !std::isfinite(device_viewport.width()) ||
        !std::isfinite(device_viewport.height())) return std::nullopt;
    bool invertible = false;
    const auto device_to_model = model_to_device.inverted(&invertible);
    const auto pixels_per_mm = logicalDpiX() / 25.4;
    if (!invertible || !ordinary_axis_transform(device_to_model) ||
        !std::isfinite(pixels_per_mm) || pixels_per_mm <= 0.0 ||
        !std::isfinite(m_scale) || m_scale <= 0.0) return std::nullopt;
    ensurePublishedGeometryIndex();
    ensureRetainedSelection();
    auto result = m_published_geometry_index_fallback;
    result.insert(result.end(), m_selected_entity_indices.begin(), m_selected_entity_indices.end());
    const auto cosmetic_scale = std::max({1.0, std::abs(canvas_to_device.m11()),
                                         std::abs(canvas_to_device.m22())});
    const auto overlaps = [](const QRectF& left, const QRectF& right) {
        // Inclusive extrema preserve zero-area bounds and touching round caps.
        return !(left.right() < right.left() || left.left() > right.right() ||
                 left.bottom() < right.top() || left.top() > right.bottom());
    };
    std::vector<std::size_t> pending;
    if (!m_published_geometry_index_nodes.empty()) pending.push_back(0);
    while (!pending.empty()) {
        const auto node_index = pending.back();
        pending.pop_back();
        const auto& node = m_published_geometry_index_nodes[node_index];
        const auto cosmetic_width = std::max({node.cosmetic_pixels, node.symbol_metres * m_scale,
                                               node.paper_mm * pixels_per_mm});
        // Enclose both device-pixel and logical-pixel cosmetic treatment, DPR,
        // round caps/joins, and the same two-pixel AA margin as direct culling.
        const auto device_padding = cosmetic_scale * (2.0 + .5 * cosmetic_width);
        const auto padded_viewport = device_viewport.adjusted(-device_padding, -device_padding,
                                                              device_padding, device_padding);
        auto query = device_to_model.mapRect(padded_viewport);
        // Include cancellation/rounding in inverse mapping of remote origins.
        const auto device_extent = std::max({1.0, std::abs(padded_viewport.left()),
            std::abs(padded_viewport.right()), std::abs(padded_viewport.top()),
            std::abs(padded_viewport.bottom())});
        const auto rounding = 32.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(query.left()), std::abs(query.right()),
                std::abs(query.top()), std::abs(query.bottom()),
                std::abs(device_to_model.dx()), std::abs(device_to_model.dy()),
                device_extent * std::abs(device_to_model.m11()),
                device_extent * std::abs(device_to_model.m22())});
        query.adjust(-rounding, -rounding, rounding, rounding);
        if (!std::isfinite(device_padding) || !finite_rect(padded_viewport) ||
            !std::isfinite(padded_viewport.width()) || !std::isfinite(padded_viewport.height()) ||
            !std::isfinite(rounding) || !finite_rect(query) ||
            !std::isfinite(query.width()) || !std::isfinite(query.height()) ||
            !finite_rect(node.bounds) || !std::isfinite(node.bounds.width()) ||
            !std::isfinite(node.bounds.height())) return std::nullopt;
        if (!overlaps(node.bounds, query)) continue;
        if (node.count) {
            for (std::size_t offset = 0; offset < node.count; ++offset) {
                const auto& entry = m_published_geometry_index_entries[node.first + offset];
                if (overlaps(entry.bounds, query)) result.push_back(entry.entity_index);
            }
        } else {
            pending.push_back(node.right);
            pending.push_back(node.left);
        }
    }
    // Tree order is spatial. Restore the original painter order, and merge
    // selected/unsafe entries without drawing any retained owner twice.
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void PlanCanvas::drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                           QColor background,
                           std::optional<double> paper_pixels_per_mm,
                           const EntityGeometry* geometry) const {
    const auto light_surface = background.lightnessF() > 0.5;
    const auto default_color = output
        ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25) : QColor(235, 235, 235))
        : color_for(entity, light_surface);
    // Legacy/default presentation records may supply only the light stroke.
    // Keep recognizably custom colors, but never let a semantic light default
    // (or the annotation default black) defeat dark-canvas contrast.
    const auto custom_stroke = entity.stroke_color.isValid() &&
        entity.stroke_color != color_for(entity, true) &&
        entity.stroke_color != QColor(Qt::black);
    const auto color = !output && entity.selected
        ? default_color
        : !output && !light_surface
            ? (entity.dark_stroke_color.isValid() ? entity.dark_stroke_color
                : custom_stroke ? entity.stroke_color : default_color)
            : entity.stroke_color.isValid() ? entity.stroke_color : default_color;

    if (entity.svg_symbol.has_value()) {
        const auto& symbol = *entity.svg_symbol;
        // Artwork is pinned per instance. The catalog ID alone is therefore
        // not a rendering identity: two saved revisions of one component may
        // intentionally coexist in the same project.
        const auto cacheable = symbol.artwork_sha256.size() == 64;
        QByteArray render_identity;
        if (cacheable) {
            QDataStream identity(&render_identity, QIODevice::WriteOnly);
            identity << symbol.artwork_sha256 << symbol.svg_palette.has_value();
            if (symbol.svg_palette) {
                const auto& palette = *symbol.svg_palette;
                // Version paint semantics independently from the pinned artwork.
                identity << quint32(1) << QString::fromStdString(palette.profile)
                         << QString::fromStdString(palette.outline_color)
                         << QString::fromStdString(palette.surface_color);
            }
        }
        const auto artwork_key = cacheable ? QString::fromLatin1(
            QCryptographicHash::hash(render_identity, QCryptographicHash::Sha256).toHex()) : QString{};
        auto renderer = cacheable ? m_svg_renderers.value(artwork_key)
                                  : QSharedPointer<QSvgRenderer>{};
        if (!renderer || !renderer->isValid()) {
            try {
                // Absent intent keeps the exact historical rendering path.
                const auto document = symbol.svg_palette
                    ? colored_symbol_svg(symbol.document, *symbol.svg_palette) : symbol.document;
                renderer = QSharedPointer<QSvgRenderer>::create();
                if (!load_admitted_svg(*renderer, document))
                    throw std::invalid_argument("Component SVG artwork cannot be rendered");
            } catch (const std::invalid_argument& error) {
                // Preserve the public hard refusal with a typed boundary so
                // optional composition guides cannot swallow geometry errors.
                if (output) throw SvgAdmissionError(error.what());
                renderer.reset();
            }
            if (renderer && renderer->isValid() && cacheable) {
                // Bound memory even while opening many projects or revisions.
                // Clearing at the limit keeps the policy deterministic and
                // avoids retaining obsolete project artwork indefinitely.
                if (m_svg_renderers.size() >= 128) m_svg_renderers.clear();
                m_svg_renderers.insert(artwork_key, renderer);
            }
        }
        const auto footprint = symbol.footprint_view_box;
        if (renderer && renderer->isValid() && footprint.width() > 0.0 &&
            footprint.height() > 0.0 && symbol.width_metres > 0.0 &&
            symbol.depth_metres > 0.0) {
            painter.save();
            painter.translate(symbol.position.x, symbol.position.y);
            painter.rotate(symbol.rotation_radians * 180.0 / pi);
            // SVG coordinates grow downward. Mirror the local Y axis so the
            // outer Cartesian canvas transform restores the authored artwork
            // orientation while rotation remains model-space counterclockwise.
            painter.scale((symbol.flip_horizontal ? -1.0 : 1.0) * symbol.width_metres / footprint.width(),
                          (symbol.flip_vertical ? 1.0 : -1.0) * symbol.depth_metres / footprint.height());
            painter.translate(-footprint.center());
            renderer->render(&painter, symbol.view_box);
            painter.restore();
            return;
        }
    }
    const auto pattern = entity.line_pattern;
    const auto pen_style = pattern == QStringLiteral("dash") ? Qt::DashLine
        : pattern == QStringLiteral("dot") ? Qt::DotLine
        : pattern == QStringLiteral("dashdot") ? Qt::DashDotLine
        : entity.dashed_stroke ? Qt::DashLine : Qt::SolidLine;
    QPen pen(color, 0.0, pen_style,
             Qt::RoundCap, Qt::RoundJoin);
    const auto default_pixels_per_mm =
        (output ? painter.device()->logicalDpiX() : logicalDpiX()) / 25.4;
    const auto pixels_per_mm = output && paper_pixels_per_mm &&
        std::isfinite(*paper_pixels_per_mm) && *paper_pixels_per_mm > 0.0
            ? *paper_pixels_per_mm : default_pixels_per_mm;
    const auto stroke = entity_stroke_width(entity, output, m_scale,
                                            pixels_per_mm, default_pixels_per_mm);
    pen.setCosmetic(stroke.cosmetic);
    pen.setWidthF(stroke.width);
    if (entity.filled) {
        const auto fresh_fill = geometry ? std::optional<QPainterPath>{} : closed_entity_path(entity);
        const auto& fill_path = geometry ? geometry->fill : fresh_fill;
        if (fill_path) {
            const auto style = hatch_style(entity.hatch_pattern);
            if (style != Qt::NoBrush) {
                auto fill_color = entity.fill_color.isValid()
                                      ? entity.fill_color
                                      : output
                                          ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                             : QColor(235, 235, 235))
                                          : color;
                if (entity.fill_opacity) fill_color.setAlphaF(*entity.fill_opacity);
                else fill_color.setAlpha(output ? 64 : 48);
                QBrush brush(fill_color, style);
                const auto scale = std::isfinite(entity.hatch_scale) && entity.hatch_scale > 0.0
                                        ? std::clamp(entity.hatch_scale, 0.1, 10.0)
                                        : 1.0;
                QTransform brush_transform;
                brush_transform.scale(scale, scale);
                brush.setTransform(brush_transform);
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(brush);
                painter.drawPath(*fill_path);
                painter.restore();
            }
        }
    }
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath path;
    // Start every semantic segment independently so hosted-opening gaps and
    // unrelated paths never acquire synthetic connector strokes.
    if (!geometry) {
        append_boundary_strokes(path, entity.stroke_segments ? *entity.stroke_segments : entity.segments);
        for (const auto& hole : entity.holes) append_boundary_strokes(path, hole);
    }
    if (!entity.segments.empty() || !entity.holes.empty()) {
        painter.drawPath(geometry ? geometry->stroke : path);
    }
    // Angular dimension overlays share the dimension_line type, but carry
    // an arc between their radial witnesses. A stale tick flag must not add
    // linear-dimension ticks to either witness of an angular dimension.
    const auto linear_dimension =
        (entity.type == QStringLiteral("dimension_line") ||
         entity.type == QStringLiteral("section_overlay")) &&
        std::all_of(entity.segments.begin(), entity.segments.end(),
                    [](const Segment& segment) { return segment.sweep_radians == 0.0; });
    if (entity.dimension_end_ticks && linear_dimension && !entity.segments.empty()) {
        const auto& dimension = entity.segments.back();
        const auto dx = dimension.end.x - dimension.start.x;
        const auto dy = dimension.end.y - dimension.start.y;
        const auto length = std::hypot(dx, dy);
        const auto device_scale = std::max(std::abs(painter.transform().m11()),
                                           std::abs(painter.transform().m22()));
        if (length > 1e-9 && device_scale > 1e-9 && std::isfinite(length) &&
            std::isfinite(device_scale)) {
            double half_tick_pixels = 4.5;
            if (output) {
                const auto pixels_per_mm =
                    paper_pixels_per_mm && std::isfinite(*paper_pixels_per_mm) &&
                            *paper_pixels_per_mm > 0.0
                        ? *paper_pixels_per_mm
                        : painter.device()->logicalDpiX() / 25.4;
                // Dimension endpoint marks are a 2.5 mm paper-space feature.
                half_tick_pixels = 1.25 * pixels_per_mm;
            }
            const auto half_tick = half_tick_pixels / device_scale;
            const Vec2 normal{-dy / length * half_tick, dx / length * half_tick};
            painter.drawLine(QLineF(dimension.start.x - normal.x,
                                    dimension.start.y - normal.y,
                                    dimension.start.x + normal.x,
                                    dimension.start.y + normal.y));
            painter.drawLine(QLineF(dimension.end.x - normal.x,
                                    dimension.end.y - normal.y,
                                    dimension.end.x + normal.x,
                                    dimension.end.y + normal.y));
        }
    }
}

void PlanCanvas::drawSegment(QPainter& painter, const Segment& segment) const {
    QPainterPath path;
    path.moveTo(segment.start.x, segment.start.y);
    if (segment.sweep_radians == 0.0) {
        path.lineTo(segment.end.x, segment.end.y);
    } else if (const auto arc = arc_info(segment)) {
        const QRectF bounds(arc->center.x - arc->radius, arc->center.y - arc->radius,
                            arc->radius * 2.0, arc->radius * 2.0);
        path.arcTo(bounds, -arc->start_angle * 180.0 / pi,
                   -segment.sweep_radians * 180.0 / pi);
    } else {
        return;
    }
    painter.drawPath(path);
}

void PlanCanvas::drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                            Vec2 view_center, bool output, QColor background,
                            std::optional<double> paper_pixels_per_mm,
                            std::vector<QRectF>* annotation_footprints,
                            bool content_only, bool floor_ghost) const {
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return;
    }
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const auto legacy_font = painter.font();
    const auto* metrics_device = output ? painter.device() : static_cast<const QPaintDevice*>(this);
    double dpi = output ? painter.device()->logicalDpiY() : logicalDpiY();
    if (output && paper_pixels_per_mm.has_value() && *paper_pixels_per_mm > 0.0 &&
        std::isfinite(*paper_pixels_per_mm * 25.4)) {
        // Fitted sheet previews need their page's paper transform, which can
        // differ from device DPI. Model scale remains independent of this.
        dpi = *paper_pixels_per_mm * 25.4;
    }
    const auto& labels = positionedLabels(legacy_font, metrics_device, scale, dpi, output,
                                         content_only, content_only ? view_center : Vec2{},
                                         floor_ghost);
    const auto& paint_layouts = m_label_placement_cache[
        floor_ghost ? 3 : content_only ? 2 : output ? 1 : 0].paint_layouts;
    const bool cached_layouts = paint_layouts.size() == labels.size();
    const auto dpr = devicePixelRatioF();
    // Only the widget's ordinary screen paint has a viewport in these logical
    // coordinates. Output, content recorders and transformed callers keep all
    // original painting. A custom window/viewport mapping bypasses culling;
    // the ordinary device DPR does not change these logical coordinates.
    const bool cull_screen = !output && !content_only && painter.device() == this &&
        painter.worldTransform().isIdentity() && !painter.viewTransformEnabled() &&
        finite_rect(viewport) && std::isfinite(dpr) && dpr > 0.0;
    const double cull_padding = cull_screen ? 3.0*std::max(1.0,1.0/dpr) : 0.0;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto& label = labels[i];
        if (!drawable_label(label)) continue;
        std::optional<LabelPaintLayout> fallback;
        const LabelPaintLayout* layout;
        if (cached_layouts) {
            layout = &paint_layouts[i];
        } else {
            // Invalid-scale/DPI publications have no derived paint layouts.
            // Retain the original per-label font/features/layout path.
            const auto paper = std::isfinite(label.paper_height_mm) && label.paper_height_mm > 0.0;
            auto base_font = paper ? font() : legacy_font;
            if (output) {
                // Keep contextual PDF alternates from changing copied text.
                base_font.setFeature("calt", 0);
                base_font.setFeature("case", 0);
            }
            const auto uncached = label_layout(label, base_font, metrics_device, scale, dpi);
            fallback.emplace(LabelPaintLayout{uncached.font,uncached.bounds,uncached.bounds});
            layout = &*fallback;
        }
        const auto& bounds = layout->bounds;
        const auto center = to_screen(label.position);
        if (annotation_footprints) {
            annotation_footprints->push_back(
                label_transform(label, center).mapRect(bounds).adjusted(-2.0, -2.0, 2.0, 2.0));
        }
        if (cull_screen && cached_layouts && finite_rect(bounds) &&
            bounds.width() >= 2.0 && bounds.height() >= 2.0 &&
            finite_rect(layout->ink_bounds) && std::isfinite(center.x()) && std::isfinite(center.y()) &&
            std::isfinite(label.rotation_radians*180.0/pi)) {
            auto ink = label_transform(label, center).mapRect(layout->ink_bounds);
            bool safe = finite_rect(ink);
            if (label.leader_start) {
                const auto start = to_screen(*label.leader_start);
                safe = safe && std::isfinite(start.x()) && std::isfinite(start.y());
                if (safe) {
                    // The leader edge lies on the label bounds. Their union
                    // with the start encloses the whole cosmetic line, even
                    // when the text is offscreen but the leader crosses it.
                    ink = QRectF(QPointF(std::min(ink.left(),start.x()),std::min(ink.top(),start.y())),
                                 QPointF(std::max(ink.right(),start.x()),std::max(ink.bottom(),start.y())));
                }
            }
            const auto padded = ink.adjusted(-cull_padding,-cull_padding,cull_padding,cull_padding);
            // Padding covers the selection border, cosmetic leader stroke and
            // antialias fringe in logical or device pixels, including low DPR.
            if (safe && finite_rect(padded) &&
                (padded.right() < viewport.left() || padded.left() > viewport.right() ||
                 padded.bottom() < viewport.top() || padded.top() > viewport.bottom())) continue;
        }
        painter.setFont(layout->font);
        if (label.leader_start) {
            const auto start = to_screen(*label.leader_start);
            const auto transform = label_transform(label, center);
            const auto local_start = transform.inverted().map(start);
            const auto delta = local_start-bounds.center();
            const auto factor = std::max(std::abs(delta.x()) / std::max(1.0,bounds.width()*0.5),
                                         std::abs(delta.y()) / std::max(1.0,bounds.height()*0.5));
            if (factor > 1.0) {
                const auto edge = transform.map(bounds.center() + delta / factor);
                auto color = label.color.isValid() ? label.color :
                    background.lightnessF()>0.5 ? QColor(85,98,115) : QColor(190,195,200);
                color.setAlpha(160);
                QPen pen(color, output ? std::max(0.7,dpi*0.15/25.4) : 0.7);
                pen.setCosmetic(true);
                painter.setPen(pen);
                painter.drawLine(start, edge);
            }
        }
        painter.save();
        painter.setTransform(label_transform(label, center), true);
        if (!output && label.selected) {
            painter.setPen(QPen(QColor(37, 99, 235), 1.0));
        } else {
            painter.setPen(Qt::NoPen);
        }
        const auto label_background = output
            ? background
            : background.lightnessF() > 0.5
                ? QColor(255, 255, 255, 238)
                : QColor(20, 25, 34, 225);
        const auto label_fill_style = label.fill_color.isValid()
            ? hatch_style(label.fill_pattern)
            : Qt::NoBrush;
        const auto custom_fill = label.fill_color.isValid() &&
                                 label_fill_style != Qt::NoBrush;
        auto brush = QBrush(custom_fill ? label.fill_color : label_background,
                            custom_fill ? label_fill_style : Qt::SolidPattern);
        if (custom_fill) {
            auto fill = label.fill_color;
            if (label.fill_opacity) fill.setAlphaF(*label.fill_opacity);
            else fill.setAlpha(output ? 220 : 238);
            brush.setColor(fill);
        }
        if (label.show_background || custom_fill) {
            painter.setBrush(brush);
            painter.drawRoundedRect(bounds, 2.0, 2.0);
        } else {
            painter.setBrush(Qt::NoBrush);
        }
        painter.setPen(label.color.isValid() ? label.color
                       : output ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                : QColor(255, 239, 172))
                              : label.selected ? QColor(37, 99, 235)
                                                : background.lightnessF() > 0.5
                                                    ? QColor(50, 65, 84)
                                                    : QColor(255, 239, 172));
        painter.setBrush(Qt::NoBrush);
        const auto text_bounds = label.text_alignment == QStringLiteral("left") ||
                                 label.text_alignment == QStringLiteral("right")
            ? bounds.adjusted(5.0, 3.0, -5.0, -3.0) : bounds;
        painter.drawText(text_bounds, label_text_alignment(label), label.text);
        painter.restore();
    }
    painter.restore();
}

void PlanCanvas::drawReference(QPainter& painter, const CanvasReference& reference) const {
    if (reference.image.isNull() || !std::isfinite(reference.position.x) ||
        !std::isfinite(reference.position.y) ||
        !(reference.metres_per_source_unit > 0.0) || !(reference.scale > 0.0) ||
        !std::isfinite(reference.metres_per_source_unit) || !std::isfinite(reference.scale)) {
        return;
    }
    auto image = reference.image;
    if (reference.flip_horizontal || reference.flip_vertical) {
        image = image.mirrored(reference.flip_horizontal, reference.flip_vertical);
    }
    const auto width = image.width() * reference.metres_per_source_unit * reference.scale;
    const auto height = image.height() * reference.metres_per_source_unit * reference.scale;
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height)) {
        return;
    }
    painter.save();
    painter.translate(reference.position.x, reference.position.y);
    if (std::isfinite(reference.rotation_degrees)) {
        painter.rotate(reference.rotation_degrees);
    }
    painter.setOpacity(std::clamp(reference.intensity, 0.0, 1.0));
    painter.drawImage(QRectF(-width * 0.5, -height * 0.5, width, height), image);
    painter.restore();
}

}  // namespace sketch::desktop
