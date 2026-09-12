#include "sketch/desktop/main_window.hpp"

#include "plan_canvas.hpp"
#include "draft_image_stamp.hpp"

#include "sketch/architecture.hpp"
#include <QColorDialog>
#include <QDoubleSpinBox>
#include "sketch/architectural_schedule.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/desktop/hosted_opening_dialog.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/building_plan_projection.hpp"
#include "sketch/building_view_projection.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/boundary_commit.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/output_fingerprint.hpp"
#include "sketch/sheet_output_scene.hpp"
#include "sketch/calculations.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/recovery_copy_record.hpp"
#include "sketch/recovery_discovery.hpp"
#include "sketch/offline_policy.hpp"
#include "sketch/workspace_accessibility.hpp"
#include "sketch/workspace_save_coordinator.hpp"
#include "sketch/workspace_save_queue.hpp"
#include "sketch/workspace_autosave_scheduler.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_visibility.hpp"
#include "sketch/quantity.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/visualization/native_model_view.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCloseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPageSize>
#include <QPalette>
#include <QBuffer>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPlainTextEdit>
#include <QSaveFile>
#include "sketch/survey_contract.hpp"
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSvgGenerator>
#include <QSvgRenderer>
#include <QPixmap>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QStatusBar>
#include <QStandardPaths>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace sketch::desktop {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumClipboardBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumClipboardEntities = 128;
constexpr std::string_view kClipboardFormat = "sketch.document.clipboard";

json clipboard_entity_json(const Entity& entity) {
    return json{{"id", entity.id}, {"type", entity.type}, {"properties", entity.properties},
                {"required", entity.required}, {"extensions", entity.extensions}};
}

Entity clipboard_entity_from_json(const json& value) {
    if (!value.is_object() || !value.contains("id") || !value.contains("type") ||
        !value.contains("properties") || !value.contains("required") ||
        !value.contains("extensions") || value.size() != 5 || !value.at("id").is_string() ||
        !value.at("type").is_string() || !value.at("properties").is_object() ||
        !value.at("required").is_boolean() || !value.at("extensions").is_object()) {
        throw std::invalid_argument("clipboard entity record is invalid");
    }
    const auto type = value.at("type").get<std::string>();
    if (!is_known_entity_type(type)) {
        throw std::invalid_argument("clipboard contains an unsupported entity type");
    }
    if (value.at("required").get<bool>()) {
        throw std::invalid_argument("required project entities cannot be pasted");
    }
    return Entity{value.at("id").get<std::string>(), type, value.at("properties"), false,
                  value.at("extensions")};
}

void remap_clipboard_json(json& value,
                          const std::map<std::string, std::string, std::less<>>& remap) {
    if (value.is_string()) {
        const auto found = remap.find(value.get_ref<const std::string&>());
        if (found != remap.end()) value = found->second;
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) remap_clipboard_json(child, remap);
        return;
    }
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            (void)key;
            remap_clipboard_json(child, remap);
        }
    }
}

// Document identities are schema fields, not arbitrary strings. Unknown
// properties and extensions are opaque user data and must survive unchanged.
void remap_entity_references(Entity& entity,
                            const std::map<std::string, std::string, std::less<>>& remap) {
    const auto reference = [&](json& object, const char* key) {
        const auto found = object.find(key);
        if (found != object.end()) remap_clipboard_json(*found, remap);
    };
    auto& properties = entity.properties;
    // Document's top-level entity-reference vocabulary. Asset and catalog-local
    // identities belong to separate namespaces.
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id",
             "boundary_id", "wall_id", "opening_id", "room_id", "slab_id", "roof_id",
             "stair_id", "sheet_id", "view_id", "constraint_id", "label_id", "column_id",
             "beam_id", "parent_id", "host_id", "target_id", "entity_id"}) {
        reference(properties, key);
        reference(properties, (std::string(key) + "s").c_str());
    }
    reference(properties, "refs");
    reference(properties, "references");
    if (properties.contains("material_assignment"))
        reference(properties.at("material_assignment"), "catalog_id");
    if (entity.type == "boundary" || entity.type == "measurement_boundary" ||
        entity.type == "room_boundary") {
        if (properties.contains("segments")) {
            for (auto& segment : properties.at("segments")) {
                for (const auto* key : {"segment_id", "start_vertex_id", "end_vertex_id"})
                    reference(segment, key);
            }
        }
    }
    if (entity.type == "dimension" && properties.contains("target")) {
        reference(properties.at("target"), "entity_id");
        reference(properties.at("target"), "segment_id");
    }
    if (entity.type == kAnnotationEntityType) {
        auto& state = properties.at("state");
        for (const auto* collection : {"labels", "symbols"})
            for (auto& item : state.at(collection)) reference(item, "id");
        for (auto& item : state.at("overrides")) reference(item, "target_id");
    }
    if (entity.type == "room_relationships") {
        auto& model = properties.at("model");
        for (auto& item : model.at("references")) reference(item, "id");
        for (auto& item : model.at("relations")) {
            reference(item, "source_id");
            reference(item, "target_id");
        }
    }
    if (entity.type == "model_phases") {
        auto& model = properties.at("model");
        reference(model, "entity_ids");
        reference(model, "baseline_ids");
        for (auto& item : model.at("alternatives")) {
            reference(item, "demolished_ids");
            reference(item, "proposed_ids");
        }
    }
}

std::optional<std::string> annotation_parent_for_child(const DocumentSnapshot& snapshot,
                                                        std::string_view child_id) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != kAnnotationEntityType) continue;
        try {
            const auto state = decode_annotation_entity(entity);
            const auto label = std::any_of(state.labels.begin(), state.labels.end(),
                                           [&](const auto& item) { return item.id == child_id; });
            const auto symbol = std::any_of(state.symbols.begin(), state.symbols.end(),
                                            [&](const auto& item) { return item.id == child_id; });
            if (label || symbol) return id;
        } catch (const std::exception&) {
            // A malformed annotation remains selectable for diagnostics, but
            // cannot be copied as a semantic graph.
        }
    }
    return std::nullopt;
}

std::vector<Entity> clipboard_entities_for_selection(const DocumentSnapshot& snapshot,
                                                      std::string_view selected_id) {
    std::string root_id(selected_id);
    if (!snapshot.entities().contains(root_id)) {
        const auto parent = annotation_parent_for_child(snapshot, selected_id);
        if (!parent) return {};
        root_id = *parent;
    }
    const auto root = snapshot.entities().find(root_id);
    if (root == snapshot.entities().end()) return {};
    static constexpr std::array<std::string_view, 12> supported{
        "boundary", "measurement_boundary", "room_boundary", "wall", "opening", "room",
        "slab", "roof", "stair", "column", "beam", "annotation_state"};
    if (std::find(supported.begin(), supported.end(), root->second.type) == supported.end()) {
        return {};
    }
    std::vector<Entity> result{root->second};
    const auto add_unique = [&](const Entity& entity) {
        if (std::none_of(result.begin(), result.end(),
                         [&](const Entity& current) { return current.id == entity.id; })) {
            result.push_back(entity);
        }
    };
    if (root->second.type == "wall") {
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "opening" &&
                entity.properties.value("wall_id", "") == root_id) {
                add_unique(entity);
            }
        }
    }
    const bool closed_boundary = root->second.type == "boundary" ||
                                 root->second.type == "measurement_boundary" ||
                                 root->second.type == "room_boundary";
    if (closed_boundary) {
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type != "dimension" || !entity.properties.is_object()) continue;
            const auto target = entity.properties.value("target", json::object());
            if (target.is_object() && target.value("entity_id", "") == root_id) {
                add_unique(entity);
            }
        }
    }
    if (result.size() > kMaximumClipboardEntities) return {};
    return result;
}

// Qt's stock Fusion icons are intentionally conservative and read as legacy
// desktop chrome at the scale used by this workspace. These small inline SVG
// glyphs keep the toolbar crisp, theme-independent, and redistributable.
QIcon modern_toolbar_icon(const char* paths) {
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'>"
        "<g fill='none' stroke='#52657d' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>") +
        QByteArray(paths) + QByteArrayLiteral("</g></svg>");
    QSvgRenderer renderer(svg);
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return QIcon(pixmap);
}

QString id_from(std::string value) {
    return QString::fromStdString(std::move(value));
}

std::string new_id(std::string_view prefix) {
    return std::string(prefix) + "-" +
           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::string digest_text(std::string_view value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    return sha256_hex(std::span<const std::byte>(bytes, value.size()));
}

FingerprintResource fingerprint_resource(std::string id, std::string material,
                                         json metadata = json::object()) {
    return FingerprintResource{std::move(id), digest_text(material), std::move(metadata)};
}

FingerprintDependencyGroup fingerprint_resources(std::vector<FingerprintResource> resources) {
    FingerprintDependencyGroup group;
    group.state = FingerprintGroupState::resources;
    group.resources = std::move(resources);
    return group;
}

FingerprintDependencyGroup fingerprint_not_applicable(std::string reason) {
    FingerprintDependencyGroup group;
    group.state = FingerprintGroupState::not_applicable;
    group.reason = std::move(reason);
    return group;
}

SheetViewModel default_sheet_view_model() {
    CoordinatedView plan_view;
    plan_view.id = "view-plan";
    plan_view.name = "Default plan";
    plan_view.kind = CoordinatedViewKind::plan;
    plan_view.origin_m = {0.0, 0.0, 0.0};
    plan_view.direction = {0.0, 0.0, -1.0};
    plan_view.up = {0.0, 1.0, 0.0};

    CoordinatedView elevation_view;
    elevation_view.id = "view-elevation";
    elevation_view.name = "South elevation";
    elevation_view.kind = CoordinatedViewKind::elevation;
    elevation_view.direction = {0.0, -1.0, 0.0};
    elevation_view.up = {0.0, 0.0, 1.0};

    CoordinatedView section_view;
    section_view.id = "view-section";
    section_view.name = "Section";
    section_view.kind = CoordinatedViewKind::section;
    section_view.origin_m = {0.0, 0.0, 1.2};
    section_view.direction = {0.0, 0.0, -1.0};
    section_view.up = {0.0, 1.0, 0.0};

    DrawingSheet sheet;
    sheet.id = "sheet-1";
    sheet.number = "A-101";
    sheet.width_mm = 420.0;
    sheet.height_mm = 297.0;
    sheet.title_block = {"Untitled property", "Default plan", "", ""};
    sheet.viewports.push_back({"viewport-plan", "view-plan", {10.0, 10.0, 400.0, 100.0}, 100.0});
    sheet.viewports.push_back({"viewport-elevation", "view-elevation", {10.0, 120.0, 195.0, 90.0}, 100.0});
    sheet.viewports.push_back({"viewport-section", "view-section", {215.0, 120.0, 195.0, 90.0}, 100.0});
    sheet.schedules.push_back({"schedule-doors", "doors", {10.0, 220.0, 190.0, 45.0}});
    return SheetViewModel::create({std::move(section_view), std::move(plan_view),
                                   std::move(elevation_view)}, {std::move(sheet)},
                                  {"doors", "windows", "rooms", "materials"});
}

json point_json(Vec2 point) {
    return json::array({point.x, point.y});
}

json segment_json(const Segment& segment) {
    return json{{"start", point_json(segment.start)},
                {"end", point_json(segment.end)},
                {"sweep_radians", segment.sweep_radians}};
}

json boundary_json(const Boundary& boundary) {
    json segments = json::array();
    for (const auto& segment : boundary) {
        segments.push_back(segment_json(segment));
    }
    return segments;
}

std::optional<Vec2> read_point(const json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
        !value[1].is_number()) {
        return std::nullopt;
    }
    const auto point = Vec2{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return std::nullopt;
    }
    return point;
}

std::optional<Segment> read_segment(const json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end")) {
        return std::nullopt;
    }
    const auto start = read_point(value.at("start"));
    const auto end = read_point(value.at("end"));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    double sweep = 0.0;
    if (value.contains("sweep_radians") && value.at("sweep_radians").is_number()) {
        sweep = value.at("sweep_radians").get<double>();
    }
    if (!std::isfinite(sweep)) {
        return std::nullopt;
    }
    return Segment{*start, *end, sweep};
}

std::optional<Segment> read_required_segment(const json& properties, std::string_view key) {
    const auto key_string = std::string(key);
    if (!properties.is_object() || !properties.contains(key_string)) {
        return std::nullopt;
    }
    // Walls have one canonical baseline. Read that value through the existing
    // segment parser, without falling back to auxiliary boundary/segments
    // presentation fields.
    return read_segment(properties.at(key_string));
}

std::optional<Boundary> read_required_boundary_value(const json& value) {
    if (!value.is_array() || value.empty()) {
        return std::nullopt;
    }
    Boundary result;
    result.reserve(value.size());
    for (const auto& segment : value) {
        if (!segment.is_object() || !segment.contains("sweep_radians") ||
            !segment.at("sweep_radians").is_number()) {
            return std::nullopt;
        }
        const auto parsed = read_segment(segment);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        result.push_back(*parsed);
    }
    return result;
}

std::optional<Boundary> read_required_boundary(const json& properties, std::string_view key) {
    const auto key_string = std::string(key);
    if (!properties.is_object() || !properties.contains(key_string)) {
        return std::nullopt;
    }
    return read_required_boundary_value(properties.at(key_string));
}

std::optional<std::vector<Boundary>> read_required_holes(const json& properties) {
    if (!properties.is_object() || !properties.contains("holes") ||
        !properties.at("holes").is_array()) {
        return std::nullopt;
    }
    std::vector<Boundary> result;
    result.reserve(properties.at("holes").size());
    for (const auto& hole : properties.at("holes")) {
        const auto parsed = read_required_boundary_value(hole);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        result.push_back(*parsed);
    }
    return result;
}

Boundary read_boundary(const json& properties) {
    Boundary result;
    if (properties.contains("boundary_model_version")) {
        const auto& version = properties.at("boundary_model_version");
        if (!version.is_number_integer() || version != 1) return result;
        // Identified boundaries have one canonical coordinate array. Auxiliary
        // legacy/vendor geometry is retained metadata, never an alternative
        // source for rendering, measurements or slab construction.
        const auto canonical = read_required_boundary(properties, "segments");
        return canonical.value_or(Boundary{});
    }
    if (properties.contains("boundary") && properties.at("boundary").is_array()) {
        for (const auto& value : properties.at("boundary")) {
            if (const auto segment = read_segment(value)) {
                result.push_back(*segment);
            }
        }
        return result;
    }
    if (properties.contains("segments") && properties.at("segments").is_array()) {
        for (const auto& value : properties.at("segments")) {
            if (const auto segment = read_segment(value)) {
                result.push_back(*segment);
            }
        }
        return result;
    }
    if (properties.contains("baseline") && properties.at("baseline").is_object()) {
        if (const auto segment = read_segment(properties.at("baseline"))) {
            result.push_back(*segment);
        }
    }
    return result;
}

std::optional<std::string> read_string(const json& object, std::string_view key) {
    const auto key_string = std::string(key);
    if (!object.contains(key_string) || !object.at(key_string).is_string()) {
        return std::nullopt;
    }
    return object.at(key_string).get<std::string>();
}

json parse_bounded_string_attributes(const QString& encoded_value) {
    auto encoded = encoded_value.trimmed().toStdString();
    if (encoded.empty()) encoded = "{}";
    const auto attributes = json::parse(encoded);
    if (!attributes.is_object() || attributes.size() > 256) {
        throw std::invalid_argument(
            "Attributes must be a JSON object with at most 256 entries.");
    }
    for (const auto& [key, value] : attributes.items()) {
        if (key.empty() || key.size() > 256 || !value.is_string() ||
            value.get_ref<const std::string&>().size() > 16384 ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos) {
            throw std::invalid_argument(
                "Attributes must contain short string key/value pairs.");
        }
    }
    return attributes;
}

std::optional<double> read_finite_number(const json& object, std::string_view key) {
    const auto key_string = std::string(key);
    if (!object.contains(key_string) || !object.at(key_string).is_number()) {
        return std::nullopt;
    }
    const auto value = object.at(key_string).get<double>();
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<HostedOpening> read_hosted_opening(const Entity& entity,
                                                 QString* error = nullptr) {
    const auto wall_id = read_string(entity.properties, "wall_id");
    const auto offset = read_finite_number(entity.properties, "offset_m");
    const auto width = read_finite_number(entity.properties, "width_m");
    const auto sill = read_finite_number(entity.properties, "sill_m");
    const auto height = read_finite_number(entity.properties, "height_m");
    if (!wall_id.has_value() || !offset.has_value() || !width.has_value() ||
        !sill.has_value() || !height.has_value()) {
        if (error != nullptr) {
            *error = QStringLiteral("Opening requires wall_id, offset_m, width_m, sill_m, and height_m.");
        }
        return std::nullopt;
    }
    if (wall_id->empty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Opening wall_id cannot be empty.");
        }
        return std::nullopt;
    }
    return HostedOpening{entity.id, *offset, *width, *sill, *height};
}

std::optional<Vec2> point_at_segment(const Segment& segment, double fraction) {
    if (!std::isfinite(fraction)) {
        return std::nullopt;
    }
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (segment.sweep_radians == 0.0) {
        return Vec2{std::lerp(segment.start.x, segment.end.x, fraction),
                    std::lerp(segment.start.y, segment.end.y, fraction)};
    }
    const auto chord_x = segment.end.x - segment.start.x;
    const auto chord_y = segment.end.y - segment.start.y;
    const auto chord_length = std::hypot(chord_x, chord_y);
    const auto tangent = std::tan(segment.sweep_radians * 0.5);
    if (!(chord_length > 1e-7) || !std::isfinite(tangent) || std::abs(tangent) <= 1e-12) {
        return std::nullopt;
    }
    const auto midpoint = Vec2{(segment.start.x + segment.end.x) * 0.5,
                               (segment.start.y + segment.end.y) * 0.5};
    const auto factor = chord_length / (2.0 * tangent);
    const auto center = Vec2{midpoint.x - chord_y / chord_length * factor,
                             midpoint.y + chord_x / chord_length * factor};
    const auto radius = std::hypot(segment.start.x - center.x, segment.start.y - center.y);
    if (!(radius > 1e-7) || !std::isfinite(radius)) {
        return std::nullopt;
    }
    const auto start_angle = std::atan2(segment.start.y - center.y,
                                        segment.start.x - center.x);
    const auto angle = start_angle + segment.sweep_radians * fraction;
    return Vec2{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

Boundary wall_segments_without_openings(const Segment& baseline,
                                         const std::vector<HostedOpening>& openings) {
    if (openings.empty()) {
        return {baseline};
    }
    const auto length = segment_length(baseline);
    if (!(length > 1e-7) || !std::isfinite(length)) {
        return {baseline};
    }
    std::vector<std::pair<double, double>> cuts;
    cuts.reserve(openings.size());
    for (const auto& opening : openings) {
        if (!std::isfinite(opening.offset) || !std::isfinite(opening.width) ||
            opening.width <= 1e-7) {
            continue;
        }
        const auto from = std::clamp(opening.offset / length, 0.0, 1.0);
        const auto to = std::clamp((opening.offset + opening.width) / length, 0.0, 1.0);
        if (to > from + 1e-8) {
            cuts.emplace_back(from, to);
        }
    }
    if (cuts.empty()) {
        return {baseline};
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto cut : cuts) {
        if (merged.empty() || cut.first > merged.back().second + 1e-8) {
            merged.push_back(cut);
        } else {
            merged.back().second = std::max(merged.back().second, cut.second);
        }
    }
    Boundary visible;
    double cursor = 0.0;
    const auto append_interval = [&](double from, double to) {
        if (to <= from + 1e-8) {
            return;
        }
        const auto start = point_at_segment(baseline, from);
        const auto end = point_at_segment(baseline, to);
        if (!start.has_value() || !end.has_value()) {
            return;
        }
        visible.push_back(Segment{*start, *end,
                                  baseline.sweep_radians * (to - from)});
    };
    for (const auto [from, to] : merged) {
        append_interval(cursor, from);
        cursor = std::max(cursor, to);
    }
    append_interval(cursor, 1.0);
    return visible.empty() ? Boundary{baseline} : visible;
}

std::string trim_ascii(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::int64_t parse_factor_integer(std::string_view token) {
    const auto trimmed = trim_ascii(token);
    if (trimmed.empty()) {
        throw std::invalid_argument("factor numerator and denominator must be integers");
    }
    std::int64_t result = 0;
    const auto* first = trimmed.data();
    const auto* last = first + trimmed.size();
    const auto parsed = std::from_chars(first, last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        throw std::invalid_argument("factor numerator and denominator must be integers");
    }
    return result;
}

std::uint64_t parse_factor_digits(std::string_view digits) {
    if (digits.empty()) {
        throw std::invalid_argument("factor must contain digits");
    }
    std::uint64_t result = 0;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto character : digits) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument("factor must be a decimal or rational number");
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (maximum - digit) / 10) {
            throw std::invalid_argument("factor exceeds the supported numeric range");
        }
        result = result * 10 + digit;
    }
    return result;
}

ExactRational parse_factor_expression(const QString& expression) {
    const auto text = trim_ascii(expression.trimmed().toStdString());
    if (text.empty()) {
        throw std::invalid_argument("factor cannot be empty");
    }
    const auto slash = text.find('/');
    if (slash != std::string::npos) {
        if (text.find('/', slash + 1) != std::string::npos) {
            throw std::invalid_argument("factor can contain only one rational slash");
        }
        auto numerator = parse_factor_integer(text.substr(0, slash));
        const auto denominator = parse_factor_integer(text.substr(slash + 1));
        if (numerator < 0 || denominator <= 0) {
            throw std::invalid_argument("factor must be nonnegative with a positive denominator");
        }
        const auto divisor = std::gcd(numerator, denominator);
        return {numerator / divisor, denominator / divisor};
    }

    bool negative = false;
    std::size_t offset = 0;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        offset = 1;
    }
    if (negative || offset == text.size()) {
        throw std::invalid_argument("factor must be nonnegative");
    }
    const auto unsigned_text = std::string_view(text).substr(offset);
    const auto dot = unsigned_text.find('.');
    if (dot != std::string_view::npos &&
        unsigned_text.find('.', dot + 1) != std::string_view::npos) {
        throw std::invalid_argument("factor must be a decimal or rational number");
    }
    const auto whole_digits = dot == std::string_view::npos
                                  ? unsigned_text
                                  : unsigned_text.substr(0, dot);
    const auto fractional_digits = dot == std::string_view::npos
                                       ? std::string_view{}
                                       : unsigned_text.substr(dot + 1);
    if (whole_digits.empty() && fractional_digits.empty()) {
        throw std::invalid_argument("factor must contain digits");
    }
    const auto whole = whole_digits.empty() ? 0 : parse_factor_digits(whole_digits);
    const auto fractional = fractional_digits.empty() ? 0 : parse_factor_digits(fractional_digits);
    std::uint64_t denominator = 1;
    for (std::size_t index = 0; index < fractional_digits.size(); ++index) {
        if (denominator > std::numeric_limits<std::uint64_t>::max() / 10) {
            throw std::invalid_argument("factor exceeds the supported numeric range");
        }
        denominator *= 10;
    }
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fractional) / denominator) {
        throw std::invalid_argument("factor exceeds the supported numeric range");
    }
    const auto numerator = whole * denominator + fractional;
    if (numerator > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        denominator > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("factor exceeds the supported numeric range");
    }
    const auto signed_numerator = static_cast<std::int64_t>(numerator);
    const auto signed_denominator = static_cast<std::int64_t>(denominator);
    const auto divisor = std::gcd(signed_numerator, signed_denominator);
    return {signed_numerator / divisor, signed_denominator / divisor};
}

struct StoredFactor {
    ExactRational rational{1, 1};
    QString expression{QStringLiteral("1")};
};

StoredFactor read_stored_factor(const json& properties) {
    if (properties.contains("factor_numerator") || properties.contains("factor_denominator")) {
        if (!properties.contains("factor_numerator") || !properties.contains("factor_denominator") ||
            !properties.at("factor_numerator").is_number_integer() ||
            !properties.at("factor_denominator").is_number_integer()) {
            throw std::invalid_argument("factor numerator and denominator must be integers");
        }
        const auto numerator = properties.at("factor_numerator").get<std::int64_t>();
        const auto denominator = properties.at("factor_denominator").get<std::int64_t>();
        if (numerator < 0 || denominator <= 0) {
            throw std::invalid_argument("factor must be nonnegative with a positive denominator");
        }
        const auto divisor = std::gcd(numerator, denominator);
        const auto expression = read_string(properties, "factor_expression");
        return {{numerator / divisor, denominator / divisor},
                expression.has_value() ? QString::fromStdString(*expression)
                                       : QStringLiteral("%1/%2").arg(numerator).arg(denominator)};
    }
    if (const auto factor = read_finite_number(properties, "factor")) {
        if (*factor < 0.0) {
            throw std::invalid_argument("factor must be nonnegative");
        }
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(std::numeric_limits<double>::max_digits10) << *factor;
        return {parse_factor_expression(QString::fromStdString(text.str())),
                QString::fromStdString(text.str())};
    }
    return {};
}

json stored_factor_json(const StoredFactor& factor) {
    const auto value = static_cast<double>(factor.rational.numerator) /
                       static_cast<double>(factor.rational.denominator);
    return json{{"factor", value},
                {"factor_expression", factor.expression.trimmed().toStdString()},
                {"factor_numerator", factor.rational.numerator},
                {"factor_denominator", factor.rational.denominator}};
}

CalculationProfile default_calculation_profile() {
    return CalculationProfile{
        "property-studio-default",
        1,
        AreaUnit::square_foot,
        2,
        {{"measurement", ClassificationRule{true, false}},
         {"survey", ClassificationRule{false, false}},
         {"room", ClassificationRule{true, true}},
         {"living", ClassificationRule{true, true}},
         {"interior", ClassificationRule{false, false}},
         {"exterior", ClassificationRule{true, false}},
         {"party", ClassificationRule{true, false}}}};
}

std::string area_unit_name(AreaUnit unit) {
    switch (unit) {
    case AreaUnit::square_metre:
        return "square_metre";
    case AreaUnit::square_foot:
        return "square_foot";
    case AreaUnit::acre:
        return "acre";
    }
    throw std::invalid_argument("Unknown area display unit");
}

AreaUnit parse_area_unit(const json& value) {
    if (!value.is_string()) {
        throw std::invalid_argument("Calculation profile display_unit must be a string");
    }
    const auto name = value.get<std::string>();
    if (name == "square_metre") {
        return AreaUnit::square_metre;
    }
    if (name == "square_foot") {
        return AreaUnit::square_foot;
    }
    if (name == "acre") {
        return AreaUnit::acre;
    }
    throw std::invalid_argument("Calculation profile display_unit is unknown");
}

CalculationProfile read_calculation_profile(const json& properties) {
    if (!properties.contains("calculation_profile")) {
        return default_calculation_profile();
    }
    const auto& value = properties.at("calculation_profile");
    if (!value.is_object()) {
        throw std::invalid_argument("Calculation profile must be an object");
    }
    CalculationProfile profile = default_calculation_profile();
    if (const auto id = read_string(value, "id")) {
        profile.id = *id;
    }
    if (value.contains("version")) {
        if (!value.at("version").is_number_unsigned()) {
            throw std::invalid_argument("Calculation profile version must be positive");
        }
        profile.version = value.at("version").get<unsigned>();
    }
    if (profile.id.empty() || profile.version == 0) {
        throw std::invalid_argument("Calculation profile needs an ID and positive version");
    }
    if (value.contains("display_unit")) {
        profile.display_unit = parse_area_unit(value.at("display_unit"));
    }
    if (value.contains("decimal_places")) {
        if (!value.at("decimal_places").is_number_unsigned()) {
            throw std::invalid_argument("Calculation profile decimal_places must be unsigned");
        }
        profile.decimal_places = value.at("decimal_places").get<unsigned>();
    }
    if (profile.decimal_places > 6) {
        throw std::invalid_argument("Calculation profile decimal_places must be 0-6");
    }
    if (value.contains("classifications")) {
        if (!value.at("classifications").is_object()) {
            throw std::invalid_argument("Calculation profile classifications must be an object");
        }
        profile.classifications.clear();
        for (const auto& [name, rule] : value.at("classifications").items()) {
            if (!rule.is_object() || !rule.contains("building_total") ||
                !rule.contains("living_total") || !rule.at("building_total").is_boolean() ||
                !rule.at("living_total").is_boolean()) {
                throw std::invalid_argument("Calculation classification rules must contain boolean totals");
            }
            profile.classifications.emplace(
                name, ClassificationRule{rule.at("building_total").get<bool>(),
                                         rule.at("living_total").get<bool>()});
        }
    }
    return profile;
}

json calculation_profile_json(const CalculationProfile& profile) {
    json classifications = json::object();
    for (const auto& [name, rule] : profile.classifications) {
        classifications[name] = json{{"building_total", rule.building_total},
                                     {"living_total", rule.living_total}};
    }
    return json{{"id", profile.id},
                {"version", profile.version},
                {"display_unit", area_unit_name(profile.display_unit)},
                {"decimal_places", profile.decimal_places},
                {"classifications", std::move(classifications)}};
}

QString area_unit_suffix(AreaUnit unit) {
    switch (unit) {
    case AreaUnit::square_metre:
        return QStringLiteral("m²");
    case AreaUnit::square_foot:
        return QStringLiteral("ft²");
    case AreaUnit::acre:
        return QStringLiteral("ac");
    }
    return QStringLiteral("area");
}

QString format_display_area(const DisplayArea& display) {
    return QString::fromStdString(display.text) + QStringLiteral(" ") +
           area_unit_suffix(display.unit);
}

double read_number(const json& object, std::string_view key, double fallback) {
    const auto key_string = std::string(key);
    if (!object.contains(key_string) || !object.at(key_string).is_number()) {
        return fallback;
    }
    const auto value = object.at(key_string).get<double>();
    return std::isfinite(value) ? value : fallback;
}

QString format_length(double metres, bool metric) {
    if (!std::isfinite(metres)) {
        return QStringLiteral("—");
    }
    if (metric) {
        if (std::abs(metres) < 1.0) {
            return QStringLiteral("%1 mm").arg(metres * 1000.0, 0, 'f', 1);
        }
        return QStringLiteral("%1 m").arg(metres, 0, 'f', 3);
    }
    const auto sign = metres < 0.0 ? QStringLiteral("-") : QString{};
    const auto total_inches = std::abs(metres) / 0.0254;
    const auto feet = static_cast<int>(std::floor(total_inches / 12.0));
    const auto inches = total_inches - static_cast<double>(feet) * 12.0;
    if (feet > 0) {
        return QStringLiteral("%1%2' %3\"")
            .arg(sign)
            .arg(feet)
            .arg(inches, 0, 'f', 1);
    }
    return QStringLiteral("%1%2\"").arg(sign).arg(inches, 0, 'f', 1);
}

QString schedule_value_text(const ScheduleValue& value) {
    return std::visit([](const auto& item) -> QString {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            return QString::fromStdString(item);
        } else if constexpr (std::is_same_v<Value, bool>) {
            return item ? QStringLiteral("Yes") : QStringLiteral("No");
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
            return QString::number(item);
        } else if constexpr (std::is_same_v<Value, double>) {
            return QString::number(item, 'f', 3);
        } else {
            const auto unit = item.unit == ScheduleUnit::metre
                ? QStringLiteral("m") : item.unit == ScheduleUnit::square_metre
                ? QStringLiteral("m²") : QStringLiteral("m³");
            return QStringLiteral("%1 %2").arg(item.value, 0, 'f', 3).arg(unit);
        }
    }, value);
}

QString schedule_kind_text(ScheduleRowKind kind) {
    switch (kind) {
    case ScheduleRowKind::door: return QStringLiteral("Door");
    case ScheduleRowKind::window: return QStringLiteral("Window");
    case ScheduleRowKind::room: return QStringLiteral("Room");
    case ScheduleRowKind::material: return QStringLiteral("Material");
    }
    return QStringLiteral("Unknown");
}

std::optional<ScheduleValue> parse_schedule_value(const ScheduleValue& current,
                                                  const QString& text) {
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;
    return std::visit([&](const auto& item) -> std::optional<ScheduleValue> {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            return ScheduleValue{trimmed.toStdString()};
        } else if constexpr (std::is_same_v<Value, bool>) {
            const auto lowered = trimmed.toLower();
            if (lowered == QStringLiteral("yes") || lowered == QStringLiteral("true") ||
                lowered == QStringLiteral("1")) return ScheduleValue{true};
            if (lowered == QStringLiteral("no") || lowered == QStringLiteral("false") ||
                lowered == QStringLiteral("0")) return ScheduleValue{false};
            return std::nullopt;
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
            bool ok = false;
            const auto value = trimmed.toLongLong(&ok);
            return ok ? std::optional<ScheduleValue>(ScheduleValue{value}) : std::nullopt;
        } else if constexpr (std::is_same_v<Value, double>) {
            bool ok = false;
            const auto value = trimmed.toDouble(&ok);
            return ok && std::isfinite(value)
                ? std::optional<ScheduleValue>(ScheduleValue{value}) : std::nullopt;
        } else {
            if (item.unit != ScheduleUnit::metre) {
                bool ok = false;
                const auto value = trimmed.toDouble(&ok);
                return ok && std::isfinite(value)
                    ? std::optional<ScheduleValue>(ScheduleValue{
                          ScheduleQuantity{value, item.unit}}) : std::nullopt;
            }
            try {
                const auto parsed = parse_quantity(trimmed.toStdString(), Unit::metre);
                return ScheduleValue{ScheduleQuantity{parsed.metres, item.unit}};
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }, current);
}

QString workspace_name(Workspace workspace) {
    return workspace == Workspace::measurement ? QStringLiteral("Measurement")
                                                : QStringLiteral("Architectural");
}

void validate_workspace_profile_json(const json& profile) {
    static constexpr std::array<std::string_view, 10> keys{
        "name", "workspace", "theme", "metric", "grid", "snap", "active_layer",
        "architectural_view", "page_size", "hidden_floors"};
    const bool has_overview = profile.is_object() && profile.contains("overview");
    const bool has_workspace_splitter = profile.is_object() && profile.contains("workspace_splitter");
    const bool has_architectural_splitter = profile.is_object() && profile.contains("architectural_splitter");
    if (!profile.is_object() || profile.size() != keys.size() + 1 +
            (has_overview ? 1 : 0) + (has_workspace_splitter ? 1 : 0) +
            (has_architectural_splitter ? 1 : 0) ||
        !profile.contains("hidden_layers")) {
        throw std::invalid_argument("workspace profile fields are invalid");
    }
    for (const auto key : keys) {
        if (!profile.contains(std::string(key))) {
            throw std::invalid_argument("workspace profile is missing " + std::string(key));
        }
    }
    const auto name = profile.at("name");
    if (!name.is_string() || name.get_ref<const std::string&>().empty() ||
        name.get_ref<const std::string&>().size() > 128) {
        throw std::invalid_argument("workspace profile name is invalid");
    }
    const auto workspace = profile.at("workspace");
    if (!workspace.is_string() ||
        (workspace != "measurement" && workspace != "architectural")) {
        throw std::invalid_argument("workspace profile workspace is invalid");
    }
    const auto theme = profile.at("theme");
    if (!theme.is_string() || (theme != "light" && theme != "dark" &&
                               theme != "high_contrast")) {
        throw std::invalid_argument("workspace profile theme is invalid");
    }
    for (const auto key : {"metric", "grid", "snap"}) {
        if (!profile.at(key).is_boolean())
            throw std::invalid_argument(std::string("workspace profile ") + key + " must be boolean");
    }
    if (has_overview && !profile.at("overview").is_boolean())
        throw std::invalid_argument("workspace profile overview must be boolean");
    for (const auto key : {"workspace_splitter", "architectural_splitter"}) {
        if (!profile.contains(key)) continue;
        const auto& sizes = profile.at(key);
        if (!sizes.is_array() || sizes.empty() || sizes.size() > 8)
            throw std::invalid_argument(std::string("workspace profile ") + key + " must be a bounded size array");
        for (const auto& size : sizes) {
            if (!size.is_number_integer() || size.get<long long>() < 0 || size.get<long long>() > 100000)
                throw std::invalid_argument(std::string("workspace profile ") + key + " contains an invalid size");
        }
    }
    for (const auto key : {"active_layer", "architectural_view"}) {
        if (!profile.at(key).is_string())
            throw std::invalid_argument(std::string("workspace profile ") + key + " must be a string");
    }
    const auto view = profile.at("architectural_view").get<std::string>();
    if (view != "plan" && view != "elevation" && view != "section")
        throw std::invalid_argument("workspace profile architectural view is invalid");
    if (!profile.at("page_size").is_number_integer())
        throw std::invalid_argument("workspace profile page size is invalid");
    if (!profile.at("hidden_floors").is_array() || !profile.at("hidden_layers").is_array() ||
        profile.at("hidden_floors").size() > 1000 || profile.at("hidden_layers").size() > 1000) {
        throw std::invalid_argument("workspace profile visibility filters are invalid");
    }
    for (const auto key : {"hidden_floors", "hidden_layers"}) {
        for (const auto& value : profile.at(key)) {
            if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
                value.get_ref<const std::string&>().size() > 128) {
                throw std::invalid_argument("workspace profile visibility IDs are invalid");
            }
        }
    }
}

bool is_closed_boundary_entity(std::string_view type) {
    return type == "boundary" || type == "measurement_boundary" || type == "room_boundary";
}

bool is_architectural_entity(std::string_view type) {
    return type == "wall" || type == "opening" || type == "room" || type == "slab" ||
           type == "roof" || type == "stair" || type == "column" || type == "beam";
}

bool is_phase_model_entity(std::string_view type) {
    return type == "building" || type == "floor" || type == "wall" || type == "opening" ||
           type == "room" || type == "room_boundary" || type == "measurement_boundary" ||
           type == "boundary" || type == "slab" || type == "roof" || type == "stair" ||
           type == "column" || type == "beam" || type == "assembly_model";
}

struct PhaseModelRecord {
    std::string entity_id;
    ModelPhases model;
};

struct RoomRelationshipRecord {
    std::string entity_id;
    RoomRelationshipSnapshot model;
};

struct AssemblyModelRecord {
    std::string entity_id;
    AssemblyModel model;
};

struct VerticalLevelRecord {
    std::string entity_id;
    VerticalLevelGraph model;
};

struct SheetModelRecord {
    std::string entity_id;
    SheetViewModel model;
};

QString assembly_quantity_unit_label(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return QStringLiteral("count");
    case AssemblyQuantityUnit::metre: return QStringLiteral("m");
    case AssemblyQuantityUnit::square_metre: return QStringLiteral("m2");
    case AssemblyQuantityUnit::cubic_metre: return QStringLiteral("m3");
    case AssemblyQuantityUnit::kilogram: return QStringLiteral("kg");
    }
    throw std::invalid_argument("Unknown assembly quantity unit");
}

AssemblyQuantityUnit assembly_quantity_unit(const QString& value) {
    const auto normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("count")) return AssemblyQuantityUnit::count;
    if (normalized == QStringLiteral("m")) return AssemblyQuantityUnit::metre;
    if (normalized == QStringLiteral("m2")) return AssemblyQuantityUnit::square_metre;
    if (normalized == QStringLiteral("m3")) return AssemblyQuantityUnit::cubic_metre;
    if (normalized == QStringLiteral("kg")) return AssemblyQuantityUnit::kilogram;
    throw std::invalid_argument("Choose a valid quantity unit.");
}

QString assembly_quantity_text(const AssemblyQuantityProperty& value) {
    return QStringLiteral("%1 %2")
        .arg(QString::number(value.value, 'g', 12), assembly_quantity_unit_label(value.unit));
}

std::optional<PhaseModelRecord> decode_phase_model(const DocumentSnapshot& snapshot) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != "model_phases") continue;
        if (!entity.properties.contains("model")) {
            throw std::invalid_argument("The design phase record has no model payload.");
        }
        return PhaseModelRecord{id, ModelPhases::from_json(entity.properties.at("model"))};
    }
    return std::nullopt;
}

std::optional<RoomRelationshipRecord> decode_room_relationships(
    const DocumentSnapshot& snapshot) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != "room_relationships") continue;
        if (!entity.properties.contains("model")) {
            throw std::invalid_argument("The room relationship record has no model payload.");
        }
        return RoomRelationshipRecord{
            id, RoomRelationshipSnapshot::from_json(entity.properties.at("model"))};
    }
    return std::nullopt;
}

std::optional<AssemblyModelRecord> decode_assembly_model(
    const DocumentSnapshot& snapshot) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != "assembly_model") continue;
        if (!entity.properties.contains("model")) {
            throw std::invalid_argument("The assembly catalog has no model payload.");
        }
        return AssemblyModelRecord{
            id, AssemblyModel::from_json(entity.properties.at("model"))};
    }
    return std::nullopt;
}

std::optional<VerticalLevelRecord> decode_vertical_levels(
    const DocumentSnapshot& snapshot) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != "vertical_levels") continue;
        if (!entity.properties.contains("model")) {
            throw std::invalid_argument("The vertical level record has no model payload.");
        }
        return VerticalLevelRecord{
            id, VerticalLevelGraph::from_json(entity.properties.at("model"))};
    }
    return std::nullopt;
}

std::optional<SheetModelRecord> decode_sheet_model(const DocumentSnapshot& snapshot) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type != kSheetViewEntityType) continue;
        return SheetModelRecord{id, decode_sheet_view_entity(entity)};
    }
    return std::nullopt;
}

std::optional<RoomReferenceKind> room_reference_kind_for_entity(std::string_view type) {
    if (type == "room_boundary") return RoomReferenceKind::room_boundary;
    if (type == "measurement_boundary") {
        return RoomReferenceKind::appraisal_measurement_boundary;
    }
    if (type == "wall") return RoomReferenceKind::architectural_wall;
    return std::nullopt;
}

QString room_reference_kind_label(RoomReferenceKind kind) {
    switch (kind) {
    case RoomReferenceKind::room_boundary: return QStringLiteral("Room boundary");
    case RoomReferenceKind::appraisal_measurement_boundary:
        return QStringLiteral("Measurement boundary");
    case RoomReferenceKind::architectural_wall: return QStringLiteral("Architectural wall");
    }
    return QStringLiteral("Reference");
}

QString room_relation_kind_label(RoomRelationKind kind) {
    switch (kind) {
    case RoomRelationKind::independent: return QStringLiteral("Independent");
    case RoomRelationKind::follows: return QStringLiteral("Follows");
    case RoomRelationKind::derived_from: return QStringLiteral("Derived from");
    }
    return QStringLiteral("Relation");
}

std::vector<RoomReference> document_room_references(const DocumentSnapshot& snapshot) {
    std::vector<RoomReference> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        const auto kind = room_reference_kind_for_entity(entity.type);
        if (kind) result.push_back({id, *kind});
    }
    return result;
}

std::vector<std::string> read_deduction_ids(const json& properties) {
    if (!properties.contains("deduction_ids")) return {};
    const auto& value = properties.at("deduction_ids");
    if (!value.is_array()) {
        throw std::invalid_argument("deduction_ids must be an array of entity IDs");
    }
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    result.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw std::invalid_argument("deduction_ids must contain only entity IDs");
        }
        const auto id = item.get<std::string>();
        if (id.empty() || !seen.insert(id).second) {
            throw std::invalid_argument("deduction_ids must contain unique nonempty entity IDs");
        }
        result.push_back(id);
    }
    return result;
}

json deduction_ids_json(const QStringList& ids) {
    json value = json::array();
    for (const auto& id : ids) value.push_back(id.trimmed().toStdString());
    return value;
}

std::vector<std::string> phase_model_entity_ids(const DocumentSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        if (is_phase_model_entity(entity.type)) result.push_back(id);
    }
    return result;
}

QString phase_alternative_label(const RemodelingAlternative& alternative) {
    const auto name = QString::fromStdString(alternative.name).trimmed();
    return name.isEmpty() ? QString::fromStdString(alternative.id) : name;
}

std::set<std::string, std::less<>> visible_project_entities_with_phase(
    const DocumentSnapshot& snapshot, const ProjectViewFilter& filter) {
    auto visible = visible_project_entities(snapshot, filter);
    if (const auto phases = decode_phase_model(snapshot)) {
        const auto active_state = phases->model.active_state();
        for (const auto& id : phases->model.entity_ids()) {
            const auto state = active_state.find(id);
            if (state == active_state.end() || state->second == ModelPhase::demolished) {
                visible.erase(id);
            }
        }
    }
    return visible;
}

BuildingViewFrame architectural_view_frame(BuildingViewKind kind) {
    switch (kind) {
    case BuildingViewKind::plan:
        return {{0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}};
    case BuildingViewKind::elevation:
        return {{0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}};
    case BuildingViewKind::section:
        // The first production view is a conventional horizontal cut at
        // 1.2 m. A persisted coordinated section overrides this fallback.
        return {{0.0, 0.0, 1.2}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}};
    }
    throw std::invalid_argument("unknown architectural view kind");
}

BuildingViewFrame architectural_view_frame(const DocumentSnapshot& snapshot,
                                           BuildingViewKind kind) {
    const auto fallback = architectural_view_frame(kind);
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type != kSheetViewEntityType) continue;
        try {
            const auto model = decode_sheet_view_entity(entity);
            const auto found = std::find_if(model.views().begin(), model.views().end(),
                [&](const auto& view) {
                    const auto expected = kind == BuildingViewKind::plan
                        ? CoordinatedViewKind::plan
                        : kind == BuildingViewKind::elevation
                        ? CoordinatedViewKind::elevation : CoordinatedViewKind::section;
                    return view.kind == expected;
                });
            if (found == model.views().end()) continue;
            BuildingViewFrame result{
                {found->origin_m[0], found->origin_m[1], found->origin_m[2]},
                {found->direction[0], found->direction[1], found->direction[2]},
                {found->up[0], found->up[1], found->up[2]}};
            if (kind == BuildingViewKind::section) {
                result.origin.x += result.direction.x * found->presentation.cut_depth_m;
                result.origin.y += result.direction.y * found->presentation.cut_depth_m;
                result.origin.z += result.direction.z * found->presentation.cut_depth_m;
            }
            return result;
        } catch (const std::exception&) {
            // The typed Document boundary reports malformed sheet/view data;
            // a transient canvas still falls back to the safe default frame.
            return fallback;
        }
    }
    return fallback;
}

const char* architectural_view_name(BuildingViewKind kind) {
    switch (kind) {
    case BuildingViewKind::plan: return "plan";
    case BuildingViewKind::elevation: return "elevation";
    case BuildingViewKind::section: return "section";
    }
    throw std::invalid_argument("unknown architectural view kind");
}

std::size_t architectural_view_index(BuildingViewKind kind) {
    switch (kind) {
    case BuildingViewKind::plan: return 0;
    case BuildingViewKind::elevation: return 1;
    case BuildingViewKind::section: return 2;
    }
    throw std::invalid_argument("unknown architectural view kind");
}

QString tool_name(CanvasTool tool) {
    switch (tool) {
    case CanvasTool::boundary:
        return QStringLiteral("Draw boundary");
    case CanvasTool::wall:
        return QStringLiteral("Draw wall");
    case CanvasTool::select:
    default:
        return QStringLiteral("Select");
    }
}

std::filesystem::path filesystem_path(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

bool same_filesystem_path(const std::filesystem::path& left,
                          const std::filesystem::path& right) {
    const auto normalized_left = left.lexically_normal();
    const auto normalized_right = right.lexically_normal();
#ifdef _WIN32
    return QString::fromStdWString(normalized_left.wstring()).compare(
               QString::fromStdWString(normalized_right.wstring()), Qt::CaseInsensitive) == 0;
#else
    return normalized_left == normalized_right;
#endif
}

bool has_entity(const Document& document, const QString& entity_id) {
    const auto snapshot = document.snapshot();
    return snapshot.entities().contains(entity_id.toStdString());
}

constexpr int visibility_type_role = Qt::UserRole + 1;

bool is_visibility_container_type(std::string_view type) {
    return type == "floor" || type == "layer";
}

bool is_visibility_container_item(const QTreeWidgetItem* item) {
    if (item == nullptr) {
        return false;
    }
    const auto type = item->data(0, visibility_type_role).toString();
    return type == QStringLiteral("floor") || type == QStringLiteral("layer");
}

// QTreeWidget changes the current item while it handles a mouse press on a
// check indicator. Keep that interaction distinguishable from a normal row
// click without relying on the global cursor position (or on a stale deferred
// event). The base widget still owns all selection and keyboard behavior.
class VisibilityTreeWidget final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

    [[nodiscard]] bool checkboxInteraction() const noexcept {
        return m_checkbox_interaction;
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        m_checkbox_interaction = false;
        if (event != nullptr) {
            auto* item = itemAt(event->position().toPoint());
            if (item != nullptr && is_visibility_container_item(item)) {
                QStyleOptionViewItem option;
                option.initFrom(this);
                option.index = indexFromItem(item, 0);
                option.rect = visualItemRect(item);
                option.state |= QStyle::State_Enabled;
                option.features |= QStyleOptionViewItem::HasCheckIndicator;
                option.checkState = item->checkState(0);
                const auto indicator = style()->subElementRect(
                    QStyle::SE_ItemViewItemCheckIndicator, &option, this);
                m_checkbox_interaction = indicator.contains(event->position().toPoint());
                if (m_checkbox_interaction) {
                    // Handle the small checkbox hit target explicitly. Fusion
                    // styles can otherwise let a press/release pair fall
                    // through to row selection when a rounded item stylesheet
                    // changes the delegate's geometry.
                    item->setCheckState(0, item->checkState(0) == Qt::Checked
                                               ? Qt::Unchecked : Qt::Checked);
                    event->accept();
                    return;
                }
            }
        }
        QTreeWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_checkbox_interaction) {
            if (event != nullptr) event->accept();
            m_checkbox_interaction = false;
            return;
        }
        QTreeWidget::mouseReleaseEvent(event);
        m_checkbox_interaction = false;
    }

private:
    bool m_checkbox_interaction{false};
};

void ensure_project_scaffold(Document& document) {
    if (!document.snapshot().entities().empty()) {
        return;
    }
    document.apply(ApplyEntityChanges{
        .expected_revision = document.revision(),
        .entity_changes = {
            EntityChange::upsert(Entity{"property-1",
                                        "property",
                                        json{{"name", "Untitled property"},
                                             {"subject", json{{"name", "Untitled property"},
                                                               {"address", ""},
                                                               {"reference", ""},
                                                               {"attributes", json::object()}}},
                                             {"calculation_profile",
                                              calculation_profile_json(default_calculation_profile())}},
                                        false,
                                        json::object()}),
            EntityChange::upsert(Entity{"building-1",
                                        "building",
                                        json{{"property_id", "property-1"},
                                             {"name", "Building 1"}},
                                        false,
                                        json::object()}),
            EntityChange::upsert(Entity{"floor-1",
                                        "floor",
                                        json{{"building_id", "building-1"},
                                             {"property_id", "property-1"},
                                             {"name", "Floor 1"},
                                             {"elevation_m", 0.0}},
                                        false,
                                        json::object()}),
            EntityChange::upsert(Entity{"layer-1",
                                        "layer",
                                        json{{"floor_id", "floor-1"},
                                             {"name", "Default layer"}},
                                        false,
                                        json::object()}),
            EntityChange::upsert(make_sheet_view_entity("sheet-view-1", default_sheet_view_model())),
            EntityChange::upsert(make_annotation_entity("annotations-1", AnnotationState{})),
        },
        .message = "create project scaffold",
    });
}

bool is_building_quantity_path(const std::string& pointer) {
    for (const auto* scalar : {"/width_m", "/depth_m", "/height_m", "/radius_m", "/total_rise_m",
             "/going_m", "/run_m", "/span_m", "/rise_m", "/overhang_m", "/thickness_m", "/length_m",
             "/top_landing/depth_m", "/top_landing/thickness_m"})
        if (pointer == scalar) return true;
    for (const auto* vector : {"/base_center_m/", "/base_position_m/", "/start_m/", "/end_m/"}) {
        const std::string_view prefix(vector);
        if (pointer.starts_with(prefix) && pointer.size() == prefix.size() + 1 &&
            pointer.back() >= '0' && pointer.back() <= '2') return true;
    }
    return false;
}

bool quantity_entry_matches(const json& receipt, const json& properties, const std::string& pointer) {
    try {
        if (!is_building_quantity_path(pointer)) return false;
        if (!receipt.is_object() || !receipt.contains("version") ||
            !receipt.at("version").is_number_integer() || receipt.at("version") != 1 ||
            !receipt.at("original_expression").is_string() || !receipt.at("entered_unit").is_string())
            return false;
        const auto unit_name = receipt.at("entered_unit").get<std::string>();
        const std::map<std::string, Unit> units{{"m", Unit::metre}, {"mm", Unit::millimetre},
            {"cm", Unit::centimetre}, {"ft", Unit::foot}, {"in", Unit::inch}};
        const auto unit = units.find(unit_name);
        if (unit == units.end()) return false;
        const auto quantity = parse_quantity(receipt.at("original_expression").get<std::string>(), unit->second);
        const auto& rational = receipt.at("exact_metres");
        const auto& value = properties.at(json::json_pointer(pointer));
        const auto exact_integer = [](const json& item, std::int64_t expected) {
            return item.is_number_integer() &&
                (!item.is_number_unsigned() || item.get<std::uint64_t>() <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) &&
                item.get<std::int64_t>() == expected;
        };
        return rational.is_object() &&
            exact_integer(rational.at("numerator"), quantity.exact_metres.numerator) &&
            exact_integer(rational.at("denominator"), quantity.exact_metres.denominator) &&
            quantity.entered_unit == unit->second && value.is_number() &&
            value.get<double>() == quantity.metres;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<json> merged_quantity_entries(const Entity* original, const Entity& proposed,
                                           const json& canonical_properties) {
    const auto new_entries = proposed.properties.find("quantity_entries");
    const json* old_entries = original && original->properties.contains("quantity_entries")
        ? &original->properties.at("quantity_entries") : nullptr;
    const json* entries = new_entries != proposed.properties.end() ? &*new_entries : old_entries;
    if (!entries) return std::nullopt;
    if (!entries->is_object()) throw std::invalid_argument("Quantity entries must be an object.");
    json result = json::object();
    for (const auto& [pointer, receipt] : entries->items()) {
        if (quantity_entry_matches(receipt, canonical_properties, pointer)) {
            result[pointer] = receipt;
            continue;
        }
        const bool inherited = old_entries && old_entries->is_object() && old_entries->contains(pointer) &&
            old_entries->at(pointer) == receipt;
        if (!inherited)
            throw std::invalid_argument("Quantity entry does not match its canonical dimension: " + pointer);
        if (!is_building_quantity_path(pointer)) {
            result[pointer] = receipt;
            continue;
        }
        // Unknown legacy records survive untouched geometry. A programmatic
        // dimension change invalidates its old receipt instead of retaining a
        // measurement expression that now describes a different value.
        try {
            const json::json_pointer path(pointer);
            const bool before = original->properties.contains(path);
            const bool after = canonical_properties.contains(path);
            if ((!before && !after) ||
                (before && after && original->properties.at(path) == canonical_properties.at(path)))
                result[pointer] = receipt;
        } catch (const std::exception&) {
            // An unrecognized pointer is metadata, not an authoritative entry.
            result[pointer] = receipt;
        }
    }
    return result;
}

}  // namespace

class MainWindow::Impl {
public:
    Impl(MainWindow* window, std::shared_ptr<Document> document)
        : owner(window), m_document(std::move(document)) {
        // The product contract is local-first.  Declare the four required
        // capabilities explicitly at the native application boundary and
        // fail closed if a future integration weakens that declaration.
        RuntimeCapabilities runtime;
        runtime.account = RuntimeRequirement::not_required;
        runtime.activation = RuntimeRequirement::not_required;
        runtime.subscription = RuntimeRequirement::not_required;
        runtime.network = RuntimeRequirement::not_required;
        const auto offline_report = evaluate_offline_policy(runtime);
        if (!offline_report.startup_allowed) {
            throw std::runtime_error("Offline independence policy rejected application startup.");
        }
        if (!m_document) {
            m_document = std::make_shared<Document>(Document::create());
        }
        ensure_project_scaffold(*m_document);
        m_project_workspace = std::make_unique<ProjectWorkspace>(m_document->snapshot());
        initializeDrawingContext();
        buildUi();
        refresh();
        m_save_timer = new QTimer(owner);
        m_save_timer->setObjectName(QStringLiteral("workspaceSavePoll"));
        m_save_timer->setInterval(100);
        QObject::connect(m_save_timer, &QTimer::timeout, owner, [this] { pollAutosave(); });
        m_save_timer->start();
    }

    ~Impl() {
        m_save_timer->stop();
        // Jobs own detached values only. Join before destroying any owner state.
        m_save_queue.shutdown(true);
        drainSaveCompletions();
    }

    QString recoveryCopyPath() const { return QString::fromStdWString(m_autosave_path.wstring()); }

    [[nodiscard]] Document& document() noexcept { return *m_document; }
    [[nodiscard]] const Document& document() const noexcept { return *m_document; }
    [[nodiscard]] DocumentScheduleProjection scheduleSnapshot() const {
        const auto source = m_document->snapshot();
        DocumentScheduleProjection projection;
        try {
            const auto visible = visible_project_entities_with_phase(source, m_view_filter);
            projection = build_architectural_schedules(source, visible);
        } catch (const std::exception& error) {
            projection = build_architectural_schedules(source);
            projection.diagnostics.push_back(std::string("Design phase: ") + error.what());
        }
        return projection;
    }

    [[nodiscard]] bool editProjectSubject(const QString& name,
                                           const QString& address,
                                           const QString& reference,
                                           const QString& attributes_json) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto property = propertyEntity();
            if (!property.has_value()) {
                throw std::invalid_argument("Project subject requires a property entity.");
            }
            const auto subject_name = name.trimmed().toStdString();
            const auto subject_address = address.trimmed().toStdString();
            const auto subject_reference = reference.trimmed().toStdString();
            if (subject_name.empty()) {
                throw std::invalid_argument("Project name cannot be empty.");
            }
            if (subject_name.size() > 16384 || subject_address.size() > 16384 ||
                subject_reference.size() > 16384 || subject_name.find('\0') != std::string::npos ||
                subject_address.find('\0') != std::string::npos ||
                subject_reference.find('\0') != std::string::npos) {
                throw std::invalid_argument("Project subject text is too long or contains a NUL byte.");
            }
            auto encoded_attributes = attributes_json.trimmed().toStdString();
            const auto attributes = parse_bounded_string_attributes(
                QString::fromStdString(encoded_attributes));
            auto updated = *property;
            updated.properties["name"] = subject_name;
            auto subject = updated.properties.value("subject", json::object());
            if (!subject.is_object()) subject = json::object();
            subject["name"] = subject_name;
            subject["address"] = subject_address;
            subject["reference"] = subject_reference;
            subject["attributes"] = attributes;
            updated.properties["subject"] = std::move(subject);
            if (!applyEntity(std::move(updated), "edit project subject")) return false;
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Project details: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool editSelectedAreaAttributes(const QString& attributes_json) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto entity = selectedEntity();
            if (!entity.has_value() || !is_closed_boundary_entity(entity->type)) {
                throw std::invalid_argument("Select a closed boundary before editing its attributes.");
            }
            auto updated = *entity;
            updated.properties["area_attributes"] = parse_bounded_string_attributes(attributes_json);
            if (!applyEntity(std::move(updated), "edit area attributes")) return false;
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Area attributes: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    std::pair<Command, std::string> makeWallTransformCommand(const DocumentSnapshot& source, const Entity& original,
                                const QString& rotation_degrees, bool flip_horizontal,
                                bool flip_vertical, const QString& offset_x,
                                const QString& offset_y, bool clone) {
        auto graph = clipboard_entities_for_selection(source, original.id);
        std::vector<const Entity*> openings;
        for (const auto& entity : graph) if (entity.type == "opening") openings.push_back(&entity);
        Wall wall;
        std::string diagnostic;
        if (!read_document_wall(original, openings, wall, diagnostic))
            throw std::invalid_argument(diagnostic);
        validate_wall_semantics(wall);
        bool valid_angle = true;
        const auto degrees = rotation_degrees.trimmed().isEmpty() ? 0.0 : rotation_degrees.toDouble(&valid_angle);
        if (!valid_angle || !std::isfinite(degrees) || std::abs(degrees) > 360000.0)
            throw std::invalid_argument("Rotation must be finite and between -360000 and 360000 degrees.");
        const auto offset = [this](const QString& value) {
            return value.trimmed().isEmpty() ? 0.0 :
                parse_quantity(value.trimmed().toStdString(), m_metric_units ? Unit::metre : Unit::foot).metres;
        };
        const Vec2 translation{offset(offset_x), offset(offset_y)};
        const auto radians = degrees * std::numbers::pi / 180.0;
        const Vec2 pivot{std::midpoint(wall.baseline.start.x, wall.baseline.end.x),
                         std::midpoint(wall.baseline.start.y, wall.baseline.end.y)};
        const auto transform = [&](Vec2 point) {
            const double x = point.x - pivot.x, y = point.y - pivot.y;
            double rx = x * std::cos(radians) - y * std::sin(radians);
            double ry = x * std::sin(radians) + y * std::cos(radians);
            if (flip_horizontal) rx = -rx;
            if (flip_vertical) ry = -ry;
            return Vec2{pivot.x + rx + translation.x, pivot.y + ry + translation.y};
        };
        auto baseline = wall.baseline;
        baseline.start = transform(baseline.start);
        baseline.end = transform(baseline.end);
        const bool reflected = flip_horizontal != flip_vertical;
        if (reflected) baseline.sweep_radians = -baseline.sweep_radians;
        wall.baseline = baseline;
        validate_wall_semantics(wall);
        std::map<std::string, std::string, std::less<>> identities;
        if (clone) for (const auto& entity : graph) identities.emplace(entity.id, new_id(entity.type));
        std::vector<EntityChange> changes;
        for (auto entity : graph) {
            if (entity.type == "wall") {
                rebase_wall_length_receipt(entity, baseline);
                const auto geometry = segment_json(baseline);
                for (const auto& [key, value] : geometry.items()) entity.properties["baseline"][key] = value;
            } else if (reflected && entity.properties.contains("door_operation")) {
                auto operation = decode_door_operation(entity.properties.at("door_operation"));
                operation.swing_left = !operation.swing_left;
                entity.properties["door_operation"] = encode_door_operation(operation);
            }
            if (clone) {
                entity.id = identities.at(entity.id);
                remap_entity_references(entity, identities);
            }
            if (clone || entity != source.entities().at(entity.id))
                changes.push_back(EntityChange::upsert(std::move(entity)));
        }
        return {ApplyEntityChanges{source.revision(), std::move(changes), {},
            clone ? "Clone transformed wall" : "Transform wall"},
            clone ? identities.at(original.id) : original.id};
    }

    [[nodiscard]] bool transformSelectedBoundary(const QString& rotation_degrees,
                                                  bool flip_horizontal,
                                                  bool flip_vertical,
                                                  const QString& offset_x,
                                                  const QString& offset_y,
                                                  bool clone) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            if (m_boundary_session) {
                throw std::runtime_error(
                    "Finish or cancel the active boundary before transforming a selected boundary.");
            }
            const auto source = authoringSnapshot();
            const auto found = source.entities().find(m_selected_id.toStdString());
            if (found == source.entities().end() ||
                (found->second.type != "wall" && !is_closed_boundary_entity(found->second.type))) {
                throw std::invalid_argument("Select a wall or an identified closed boundary first.");
            }
            const auto [command, root] = found->second.type == "wall" ?
                makeWallTransformCommand(source, found->second, rotation_degrees, flip_horizontal,
                    flip_vertical, offset_x, offset_y, clone) :
                makeBoundaryTransformCommand(source, found->second, rotation_degrees, flip_horizontal,
                    flip_vertical, offset_x, offset_y, clone);
            const auto* changes = std::get_if<ApplyEntityChanges>(&command);
            if (!changes || !changes->entity_changes.empty()) {
                (void)Document::preview_command(source, command);
                applyDocumentCommand(command);
                m_selected_id = id_from(root);
                refresh();
            }
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Transform: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    std::pair<Command, std::string> makeBoundaryTransformCommand(
        const DocumentSnapshot& source, Entity original, const QString& rotation_degrees,
        bool flip_horizontal, bool flip_vertical, const QString& offset_x,
        const QString& offset_y, bool clone) {
        const auto version = inspect_boundary_entity_version(original);
        if (version.format == BoundaryEntityFormat::unsupported_version) {
            throw std::invalid_argument(
                "This boundary uses an unsupported model version and cannot be transformed.");
        }
        if (version.format == BoundaryEntityFormat::anonymous_legacy) {
            // An in-place transform cannot combine identity promotion with
            // a geometry edit: the document integrity guard requires the
            // promotion itself to preserve geometry exactly. Clone mode can
            // still allocate fresh identities without changing the source.
            if (!clone) {
                throw std::invalid_argument(
                    "This legacy boundary needs an explicit identity upgrade before an in-place transform.");
            }
            const auto legacy_auxiliary_boundary = original.properties.contains("boundary")
                ? std::optional{original.properties.at("boundary")} : std::nullopt;
            if (legacy_auxiliary_boundary && original.properties.contains("segments"))
                original.properties.erase("boundary");
            original = upgrade_legacy_boundary_entity(original);
            if (legacy_auxiliary_boundary)
                original.properties["boundary"] = *legacy_auxiliary_boundary;
        }
        auto transformed = decode_identified_boundary_entity(original);
        const auto parse_degrees = [](const QString& text) {
            if (text.trimmed().isEmpty()) return 0.0;
            bool ok = false;
            const auto value = text.trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(value) || std::abs(value) > 360000.0)
                throw std::invalid_argument("Rotation must be a finite value between -360000 and 360000 degrees.");
            return value * std::numbers::pi / 180.0;
        };
        const auto parse_offset = [this](const QString& text) {
            if (text.trimmed().isEmpty()) return 0.0;
            return parse_quantity(text.trimmed().toStdString(),
                                  m_metric_units ? Unit::metre : Unit::foot).metres;
        };
        const auto radians = parse_degrees(rotation_degrees);
        const auto bounds = boundary_bounds(boundary_geometry(transformed));
        const Vec2 pivot{std::midpoint(bounds.minimum.x, bounds.maximum.x),
                         std::midpoint(bounds.minimum.y, bounds.maximum.y)};
        if (std::abs(radians) > 0.0)
            transformed = rotate_boundary(transformed, pivot, radians);
        if (flip_horizontal)
            transformed = flip_boundary(transformed, pivot, BoundaryFlipAxis::vertical);
        if (flip_vertical)
            transformed = flip_boundary(transformed, pivot, BoundaryFlipAxis::horizontal);
        const Vec2 offset{parse_offset(offset_x), parse_offset(offset_y)};
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y))
            throw std::invalid_argument("Boundary offsets must be finite.");
        if (!clone && original.properties.contains("boundary_authoring")) {
            if (radians != 0.0 || flip_horizontal || flip_vertical)
                throw std::invalid_argument("Construction-bound rotation and reflection require receipt migration.");
            return {TranslateBoundary{source.revision(), {original.id, offset}}, original.id};
        }
        for (auto& edge : transformed.segments) {
            edge.segment.start.x += offset.x;
            edge.segment.start.y += offset.y;
            edge.segment.end.x += offset.x;
            edge.segment.end.y += offset.y;
        }
        const auto append_dimensions = [&](std::vector<EntityChange>& changes,
            const std::map<std::string, std::string, std::less<>>& identities) {
            for (const auto& [id, entity] : source.entities()) {
                if (entity.type != "dimension" || !entity.properties.contains("target") ||
                    entity.properties.at("target").value("entity_id", std::string{}) != original.id) continue;
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
                auto dimension = *decoded.dimension;
                if (clone) {
                    dimension.id = new_id("dimension");
                    dimension.boundary_id = identities.at(original.id);
                    dimension.segment_id = identities.at(dimension.segment_id);
                }
                const auto x = dimension.text_position.x - pivot.x;
                const auto y = dimension.text_position.y - pivot.y;
                Vec2 position = dimension.text_position;
                if (radians != 0.0)
                    position = {pivot.x + x * std::cos(radians) - y * std::sin(radians),
                                pivot.y + x * std::sin(radians) + y * std::cos(radians)};
                if (flip_horizontal) position.x = pivot.x - (position.x - pivot.x);
                if (flip_vertical) position.y = pivot.y - (position.y - pivot.y);
                dimension.text_position = {position.x + offset.x, position.y + offset.y};
                auto dimension_metadata = entity;
                dimension_metadata.id = dimension.id;
                auto dimension_ids = identities;
                if (clone) dimension_ids.emplace(id, dimension.id);
                remap_entity_references(dimension_metadata, dimension_ids);
                const auto encoded = encode_boundary_dimension_entity(dimension, &dimension_metadata);
                if (clone || encoded != entity) changes.push_back(EntityChange::upsert(encoded));
            }
        };
        const auto revision = source.revision();
        if (clone) {
            std::optional<BoundaryConstructionRecord> construction;
            if (original.properties.contains("boundary_authoring")) {
                if (radians != 0.0 || flip_horizontal || flip_vertical)
                    throw std::invalid_argument("Construction-bound copies currently support offsets; rotation and reflection require receipt migration.");
                const auto decoded = decode_boundary_receipt_envelope(original.properties.at("boundary_authoring"));
                if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
                construction = *decoded.record;
                // Only this qualified envelope is handled below. Other owned
                // semantics still pass through the ordinary retirement guard.
                original.properties.erase("boundary_authoring");
            }
            LegacyBoundaryIdentityOptions ids;
            ids.segment_ids.reserve(transformed.segments.size());
            ids.vertex_ids.reserve(transformed.segments.size());
            for (std::size_t index = 0; index < transformed.segments.size(); ++index) {
                ids.segment_ids.push_back(new_id("segment"));
                ids.vertex_ids.push_back(new_id("vertex"));
            }
            const auto clone_id = new_id("boundary");
            auto cloned = clone_boundary(transformed, clone_id, ids, {});
            // Validate identity retirement as well as geometry changes before
            // remapping. Unknown receipts must not be silently discarded or
            // carried into a new identity without qualified migration.
            auto identity_check = cloned;
            identity_check.id = original.id;
            (void)encode_identified_boundary_entity(identity_check, &original);
            std::map<std::string, std::string, std::less<>> identities{{original.id, clone_id}};
            for (std::size_t index = 0; index < transformed.segments.size(); ++index) {
                identities.emplace(transformed.segments[index].segment_id, ids.segment_ids[index]);
                identities.emplace(transformed.segments[index].start_vertex_id, ids.vertex_ids[index]);
            }
            std::optional<json> envelope;
            if (construction) {
                const auto translated = translated_boundary_construction(*construction, offset, identities);
                const auto replay = replay_boundary_construction(translated);
                cloned.segments.clear();
                for (const auto& edge : replay.edges)
                    cloned.segments.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
                envelope = encode_boundary_receipt_envelope(translated);
            }
            auto metadata = original;
            metadata.id = clone_id;
            remap_entity_references(metadata, identities);
            auto encoded = encode_identified_boundary_entity(cloned, &metadata);
            if (envelope) encoded.properties["boundary_authoring"] = *envelope;
            std::vector<EntityChange> changes{EntityChange::upsert(std::move(encoded))};
            append_dimensions(changes, identities);
            return {ApplyEntityChanges{
                .expected_revision = revision,
                .entity_changes = std::move(changes),
                .message = "clone transformed boundary",
            }, clone_id};
        } else {
            const auto encoded = encode_identified_boundary_entity(transformed, &original);
            std::vector<EntityChange> changes;
            if (encoded != original) changes.push_back(EntityChange::upsert(encoded));
            append_dimensions(changes, {});
            return {ApplyEntityChanges{revision, std::move(changes), {}, "Transform boundary"}, original.id};
        }
    }

    [[nodiscard]] bool editScheduleCell(const QString& object_id, const QString& column,
                                         const QString& replacement) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto source = authoringSnapshot();
            const auto projection = build_document_schedules(source);
            const auto row = std::find_if(projection.snapshot.rows.begin(),
                                          projection.snapshot.rows.end(),
                [&](const auto& candidate) {
                    return candidate.object_id == object_id.toStdString();
                });
            if (row == projection.snapshot.rows.end())
                throw std::invalid_argument("Schedule row was not found");
            const auto cell = row->cells.find(column.toStdString());
            if (cell == row->cells.end())
                throw std::invalid_argument("Schedule column was not found");
            const auto parsed = parse_schedule_value(cell->second.value, replacement);
            if (!parsed) throw std::invalid_argument("Schedule value is not valid for this cell");
            const auto edit = make_schedule_edit(projection.snapshot, row->object_id,
                                                 cell->first, *parsed);
            const auto command = make_document_schedule_edit(source, edit);
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Schedule edit: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool editSheetMetadata(const QString& sheet_id, const QString& number,
                                          const QString& project, const QString& title,
                                          const QString& author, const QString& issue_date) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            if (number.trimmed().isEmpty())
                throw std::invalid_argument("Sheet number cannot be empty");
            const auto source = authoringSnapshot();
            const Entity* sheet_entity = nullptr;
            std::optional<SheetViewModel> model;
            for (const auto& [id, candidate] : source.entities()) {
                (void)id;
                if (candidate.type != kSheetViewEntityType) continue;
                try {
                    auto decoded = decode_sheet_view_entity(candidate);
                    const auto sheet_matches = sheet_id.trimmed().isEmpty() ||
                        std::any_of(decoded.sheets().begin(), decoded.sheets().end(),
                            [&](const auto& sheet) {
                                return sheet.id == sheet_id.trimmed().toStdString();
                            });
                    if (sheet_matches) {
                        model = std::move(decoded);
                        sheet_entity = &candidate;
                        break;
                    }
                } catch (const std::exception&) {
                    // Let the selected typed entity report its validation
                    // failure below rather than silently editing another one.
                    if (sheet_id.trimmed().isEmpty()) throw;
                }
            }
            if (sheet_entity == nullptr || !model || model->sheets().empty())
                throw std::invalid_argument("Drawing sheet was not found");
            const auto selected_sheet_id = sheet_id.trimmed().isEmpty()
                ? model->sheets().front().id : sheet_id.trimmed().toStdString();
            const auto found = std::find_if(model->sheets().begin(), model->sheets().end(),
                [&](const auto& sheet) { return sheet.id == selected_sheet_id; });
            if (found == model->sheets().end())
                throw std::invalid_argument("Drawing sheet identity was not found");
            auto replacement = *found;
            replacement.number = number.trimmed().toStdString();
            replacement.title_block.project = project.toStdString();
            replacement.title_block.title = title.toStdString();
            replacement.title_block.author = author.toStdString();
            replacement.title_block.issue_date = issue_date.toStdString();
            const auto updated_model = model->with_sheet(std::move(replacement));
            auto updated_entity = *sheet_entity;
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Edit drawing sheet metadata"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet edit: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool editSheetViewport(const QString& sheet_id, const QString& viewport_id,
                                          const QString& x_mm, const QString& y_mm,
                                          const QString& width_mm, const QString& height_mm,
                                          const QString& scale_denominator) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto parse_finite = [](const QString& text, const char* label) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value))
                    throw std::invalid_argument(std::string(label) + " must be a finite number");
                return value;
            };
            const auto x = parse_finite(x_mm, "Viewport X");
            const auto y = parse_finite(y_mm, "Viewport Y");
            const auto width = parse_finite(width_mm, "Viewport width");
            const auto height = parse_finite(height_mm, "Viewport height");
            const auto scale = parse_finite(scale_denominator, "Viewport scale");
            const auto source = authoringSnapshot();
            const Entity* sheet_entity = nullptr;
            std::optional<SheetViewModel> model;
            const auto wanted_sheet = sheet_id.trimmed().toStdString();
            const auto wanted_viewport = viewport_id.trimmed().toStdString();
            for (const auto& [id, candidate] : source.entities()) {
                (void)id;
                if (candidate.type != kSheetViewEntityType) continue;
                try {
                    auto decoded = decode_sheet_view_entity(candidate);
                    const bool sheet_matches = wanted_sheet.empty() ||
                        std::any_of(decoded.sheets().begin(), decoded.sheets().end(),
                            [&](const auto& sheet) { return sheet.id == wanted_sheet; });
                    if (!sheet_matches) continue;
                    const auto selected_sheet = wanted_sheet.empty()
                        ? decoded.sheets().front().id : wanted_sheet;
                    const auto sheet = std::find_if(decoded.sheets().begin(), decoded.sheets().end(),
                        [&](const auto& candidate_sheet) { return candidate_sheet.id == selected_sheet; });
                    if (sheet == decoded.sheets().end()) continue;
                    const bool viewport_matches = wanted_viewport.empty() ||
                        std::any_of(sheet->viewports.begin(), sheet->viewports.end(),
                            [&](const auto& viewport) { return viewport.id == wanted_viewport; });
                    if (viewport_matches) {
                        model = std::move(decoded);
                        sheet_entity = &candidate;
                        break;
                    }
                } catch (const std::exception&) {
                    if (wanted_sheet.empty() || wanted_viewport.empty()) throw;
                }
            }
            if (sheet_entity == nullptr || !model || model->sheets().empty())
                throw std::invalid_argument("Drawing sheet viewport was not found");
            const auto selected_sheet_id = wanted_sheet.empty()
                ? model->sheets().front().id : wanted_sheet;
            const auto sheet = std::find_if(model->sheets().begin(), model->sheets().end(),
                [&](const auto& candidate) { return candidate.id == selected_sheet_id; });
            if (sheet == model->sheets().end())
                throw std::invalid_argument("Drawing sheet identity was not found");
            if (sheet->viewports.empty())
                throw std::invalid_argument("Drawing sheet has no viewports");
            const auto selected_viewport_id = wanted_viewport.empty()
                ? sheet->viewports.front().id : wanted_viewport;
            const auto viewport = std::find_if(sheet->viewports.begin(), sheet->viewports.end(),
                [&](const auto& candidate) { return candidate.id == selected_viewport_id; });
            if (viewport == sheet->viewports.end())
                throw std::invalid_argument("Sheet viewport identity was not found");
            auto replacement = *viewport;
            replacement.bounds = {x, y, width, height};
            replacement.scale_denominator = scale;
            const auto updated_model = model->with_viewport(selected_sheet_id,
                                                              std::move(replacement));
            auto updated_entity = *sheet_entity;
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Edit drawing sheet viewport"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Viewport edit: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool editSheetSchedulePlacement(const QString& sheet_id,
                                                   const QString& placement_id,
                                                   const QString& x_mm, const QString& y_mm,
                                                   const QString& width_mm,
                                                   const QString& height_mm) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto parse_finite = [](const QString& text, const char* label) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value))
                    throw std::invalid_argument(std::string(label) + " must be a finite number");
                return value;
            };
            const auto x = parse_finite(x_mm, "Schedule X");
            const auto y = parse_finite(y_mm, "Schedule Y");
            const auto width = parse_finite(width_mm, "Schedule width");
            const auto height = parse_finite(height_mm, "Schedule height");
            const auto source = authoringSnapshot();
            const Entity* sheet_entity = nullptr;
            std::optional<SheetViewModel> model;
            const auto wanted_sheet = sheet_id.trimmed().toStdString();
            const auto wanted_placement = placement_id.trimmed().toStdString();
            for (const auto& [id, candidate] : source.entities()) {
                (void)id;
                if (candidate.type != kSheetViewEntityType) continue;
                try {
                    auto decoded = decode_sheet_view_entity(candidate);
                    const bool sheet_matches = wanted_sheet.empty() ||
                        std::any_of(decoded.sheets().begin(), decoded.sheets().end(),
                            [&](const auto& sheet) { return sheet.id == wanted_sheet; });
                    if (!sheet_matches) continue;
                    const auto selected_sheet = wanted_sheet.empty()
                        ? decoded.sheets().front().id : wanted_sheet;
                    const auto sheet = std::find_if(decoded.sheets().begin(), decoded.sheets().end(),
                        [&](const auto& candidate_sheet) { return candidate_sheet.id == selected_sheet; });
                    if (sheet == decoded.sheets().end()) continue;
                    const bool placement_matches = wanted_placement.empty() ||
                        std::any_of(sheet->schedules.begin(), sheet->schedules.end(),
                            [&](const auto& placement) { return placement.id == wanted_placement; });
                    if (placement_matches) {
                        model = std::move(decoded);
                        sheet_entity = &candidate;
                        break;
                    }
                } catch (const std::exception&) {
                    if (wanted_sheet.empty() || wanted_placement.empty()) throw;
                }
            }
            if (sheet_entity == nullptr || !model || model->sheets().empty())
                throw std::invalid_argument("Schedule placement was not found");
            const auto selected_sheet_id = wanted_sheet.empty()
                ? model->sheets().front().id : wanted_sheet;
            const auto sheet = std::find_if(model->sheets().begin(), model->sheets().end(),
                [&](const auto& candidate) { return candidate.id == selected_sheet_id; });
            if (sheet == model->sheets().end())
                throw std::invalid_argument("Drawing sheet identity was not found");
            if (sheet->schedules.empty())
                throw std::invalid_argument("Drawing sheet has no schedule placements");
            const auto selected_placement_id = wanted_placement.empty()
                ? sheet->schedules.front().id : wanted_placement;
            const auto placement = std::find_if(sheet->schedules.begin(), sheet->schedules.end(),
                [&](const auto& candidate) { return candidate.id == selected_placement_id; });
            if (placement == sheet->schedules.end())
                throw std::invalid_argument("Schedule placement identity was not found");
            auto replacement = *placement;
            replacement.bounds = {x, y, width, height};
            const auto updated_model = model->with_schedule_placement(selected_sheet_id,
                                                                       std::move(replacement));
            auto updated_entity = *sheet_entity;
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Edit schedule placement"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Schedule placement edit: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool editArchitecturalViewPresentation(
        const QString& view_id, const QString& cut_depth_m, const QString& far_depth_m,
        const QString& cut_line_mm, const QString& projection_line_mm, bool hatch_enabled,
        const QString& hatch_pattern, const QString& hatch_scale, const QString& detail) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto parse_finite = [](const QString& text, const char* label) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value))
                    throw std::invalid_argument(std::string(label) + " must be a finite number");
                return value;
            };
            const auto cut = parse_finite(cut_depth_m, "Cut depth");
            const auto far = parse_finite(far_depth_m, "Far depth");
            const auto cut_line = parse_finite(cut_line_mm, "Cut line width");
            const auto projection_line = parse_finite(projection_line_mm, "Projection line width");
            const auto hatch_scale_value = parse_finite(hatch_scale, "Hatch scale");
            ViewDetail detail_value{};
            const auto detail_name = detail.trimmed().toLower();
            if (detail_name == QStringLiteral("coarse")) detail_value = ViewDetail::coarse;
            else if (detail_name == QStringLiteral("medium")) detail_value = ViewDetail::medium;
            else if (detail_name == QStringLiteral("fine")) detail_value = ViewDetail::fine;
            else throw std::invalid_argument("View detail must be coarse, medium, or fine");
            const auto pattern = hatch_pattern.trimmed().toStdString();
            if (pattern.empty()) throw std::invalid_argument("Hatch pattern cannot be empty");
            const auto source = authoringSnapshot();
            const Entity* view_entity = nullptr;
            std::optional<SheetViewModel> model;
            const auto wanted_view = view_id.trimmed().toStdString();
            for (const auto& [id, candidate] : source.entities()) {
                (void)id;
                if (candidate.type != kSheetViewEntityType) continue;
                try {
                    auto decoded = decode_sheet_view_entity(candidate);
                    const auto found = std::find_if(decoded.views().begin(), decoded.views().end(),
                        [&](const auto& view) { return view.id == wanted_view; });
                    if (found != decoded.views().end()) {
                        model = std::move(decoded);
                        view_entity = &candidate;
                        break;
                    }
                } catch (const std::exception&) {
                    if (wanted_view.empty()) throw;
                }
            }
            if (view_entity == nullptr || !model)
                throw std::invalid_argument("Architectural view was not found");
            const auto found = std::find_if(model->views().begin(), model->views().end(),
                [&](const auto& view) { return view.id == wanted_view; });
            if (found == model->views().end())
                throw std::invalid_argument("Architectural view identity was not found");
            auto replacement = *found;
            replacement.presentation.cut_depth_m = cut;
            replacement.presentation.far_depth_m = far;
            replacement.presentation.cut_line_mm = cut_line;
            replacement.presentation.projection_line_mm = projection_line;
            replacement.presentation.hatch_enabled = hatch_enabled;
            replacement.presentation.hatch_pattern = pattern;
            replacement.presentation.hatch_scale = hatch_scale_value;
            replacement.presentation.detail = detail_value;
            const auto updated_model = model->with_view(std::move(replacement));
            auto updated_entity = *view_entity;
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Edit architectural view presentation"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Architectural view edit: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] Workspace workspace() const noexcept { return m_workspace; }

    void setWorkspace(Workspace workspace) {
        m_workspace = workspace;
        if (m_workspaceTabs) {
            m_workspaceTabs->setCurrentIndex(workspace == Workspace::measurement ? 0 : 1);
        }
        if (!m_boundary_session) setTool(CanvasTool::select);
        refreshTitle();
    }

    [[nodiscard]] bool metricUnits() const noexcept { return m_metric_units; }

    void setMetricUnits(bool metric) {
        m_metric_units = metric;
        if (m_unitsCombo) {
            QSignalBlocker blocker(m_unitsCombo);
            m_unitsCombo->setCurrentIndex(metric ? 1 : 0);
        }
        m_measurementCanvas->setMetricUnits(metric);
        m_architecturalCanvas->setMetricUnits(metric);
        refreshCanvases();
        refreshInspector();
        refreshCursorLabel(m_last_cursor);
        refreshBoundaryPreview();
    }

    void applyTheme(WorkspaceTheme theme) {
        const bool dark = theme == WorkspaceTheme::dark;
        const bool contrast = theme == WorkspaceTheme::high_contrast;
        const QString background = contrast ? "#000000" : dark ? "#111827" : "#f3f6fa";
        const QString surface = contrast ? "#000000" : dark ? "#1b2638" : "#ffffff";
        const QString foreground = contrast ? "#ffffff" : dark ? "#edf2fb" : "#182536";
        const QString muted = contrast ? "#ffffff" : dark ? "#a8b7cc" : "#63748a";
        const QString border = contrast ? "#ffffff" : dark ? "#33435a" : "#d9e2ed";
        const QString accent = contrast ? "#ffff00" : dark ? "#7db3ff" : "#2563eb";
        const QString selection = contrast ? "#ffff00" : dark ? "#243e67" : "#e7efff";
        const QString selectedText = contrast ? "#000000" : foreground;
        QPalette palette = owner->style()->standardPalette();
        palette.setColor(QPalette::Window, QColor(background));
        palette.setColor(QPalette::WindowText, QColor(foreground));
        palette.setColor(QPalette::Base, QColor(surface));
        palette.setColor(QPalette::AlternateBase, QColor(background));
        palette.setColor(QPalette::Text, QColor(foreground));
        palette.setColor(QPalette::Button, QColor(surface));
        palette.setColor(QPalette::ButtonText, QColor(foreground));
        palette.setColor(QPalette::Highlight, QColor(selection));
        palette.setColor(QPalette::HighlightedText, QColor(selectedText));
        palette.setColor(QPalette::Link, QColor(accent));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(muted));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(muted));
        owner->setPalette(palette);
        QString stylesheet = QStringLiteral(R"(
            QMainWindow { background: $background; }
            QWidget { font-size: 13px; }
            QDialog { background: $background; }
            QToolBar#primaryToolbar { background: $surface; border: 0; border-bottom: 1px solid $border;
                       padding: 0 3px; spacing: 1px; min-height: 32px; max-height: 32px; }
            QToolBar::separator { background: $border; width: 1px; margin: 0 1px; }
            QPushButton, QToolButton { color: $foreground; background: $surface;
                border: 1px solid $border; border-radius: 8px; padding: 8px 11px; }
            QToolBar QToolButton { border-color: transparent; border-radius: 4px; padding: 3px 5px; min-height: 20px; max-height: 20px; }
            QToolBar QToolButton:hover { background: $selection; border-color: $selection; }
            QToolBar QToolButton:checked { background: $selection; color: $accent; border-color: $accent; }
            QWidget#toolPanel QToolButton { padding: 6px 4px; min-height: 52px; }
            QPushButton:hover, QToolButton:hover { background: $selection; border-color: $accent; }
            QPushButton:pressed, QToolButton:pressed, QToolButton:checked {
                background: $selection; color: $selectedText; border-color: $accent; }
            QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus,
            QAbstractSpinBox:focus { border: 2px solid $accent; }
            QPushButton:disabled, QToolButton:disabled { color: $muted; background: $background; }
            QComboBox, QLineEdit, QAbstractSpinBox { color: $foreground; background: $surface;
                border: 1px solid $border; border-radius: 8px; padding: 5px 10px; min-height: 20px; }
            QComboBox { padding-right: 24px; }
            QComboBox::drop-down { border: 0; width: 24px; }
            QToolBar QComboBox { font-size: 12px; border-radius: 4px; padding: 2px 18px 2px 5px; min-height: 20px; max-height: 20px; }
            QToolBar QComboBox::drop-down { width: 14px; }
            QComboBox QAbstractItemView, QMenu { background: $surface; color: $foreground;
                border: 1px solid $border; selection-background-color: $selection;
                selection-color: $selectedText; padding: 4px; }
            QMenu::item { padding: 8px 20px; }
            QMenu::item:selected { background: $selection; color: $selectedText; }
            QLabel#panelHeading { color: $muted; font-size: 10px; font-weight: 700;
                letter-spacing: 1px; }
            QLabel#inspectorHeading { color: $foreground; font-size: 18px; font-weight: 700; }
            QWidget#navigatorPanel, QWidget#toolPanel, QWidget#inspectorBody { background: $surface; }
            QWidget#navigatorPanel, QWidget#toolPanel { border: 1px solid $border; border-radius: 10px; }
            QLabel#modelViewUnavailable { background: $surface; color: $muted;
                border: 1px solid $border; border-radius: 10px; margin: 12px; padding: 24px; }
            QLabel#drawingContext, QLabel#visibilitySummary { color: $muted; font-size: 11px; }
            QTreeWidget, QListWidget, QTableWidget { background: $surface; color: $foreground;
                border: 0; alternate-background-color: transparent; outline: 0;
                selection-background-color: $selection; selection-color: $selectedText; }
            QTreeWidget::item { min-height: 29px; border-radius: 6px; padding: 3px 6px; }
            QTreeWidget::item:hover { background: $background; }
            QTreeWidget::item:selected { background: $selection; color: $selectedText; }
            QTreeWidget::item:focus { border: 1px solid $accent; }
            QHeaderView::section { background: $background; color: $muted;
                border: 0; border-bottom: 1px solid $border; padding: 6px; }
            QTabWidget#workspaceTabs::pane { border: 0; background: transparent; }
            QTabBar::tab { background: transparent; color: $muted; padding: 10px 18px;
                margin: 3px 2px; border: 1px solid transparent; border-radius: 8px; }
            QTabBar::tab:selected { background: $selection; color: $accent; border-color: $accent; }
            QTabBar::tab:hover { color: $foreground; background: $background; }
            QWidget#measurementPlanCanvas, QWidget#architecturalPlanCanvas {
                border: 1px solid $border; border-radius: 8px; }
            QGroupBox { color: $foreground; background: $surface; border: 1px solid $border;
                border-radius: 10px; margin-top: 18px; padding: 16px 10px 10px; font-weight: 600; }
            QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }
            QScrollArea { border: 0; background: $surface; }
            QSplitter::handle { background: $background; }
            QSplitter::handle:hover { background: $accent; }
            QStatusBar { background: $surface; color: $muted; border-top: 1px solid $border; padding: 6px 14px; }
            QStatusBar::item { border: 0; }
            QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
            QScrollBar::handle:vertical { background: $border; border-radius: 5px; min-height: 28px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
            QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
            QToolTip { background: $surface; color: $foreground; border: 1px solid $border; padding: 7px; }
        )");
        stylesheet.replace("$background", background).replace("$surface", surface)
            .replace("$foreground", foreground).replace("$muted", muted)
            .replace("$border", border).replace("$accent", accent)
            .replace("$selection", selection).replace("$selectedText", selectedText);
        owner->setStyleSheet(stylesheet);
        owner->setProperty("workspaceTheme", QString::fromStdString(workspace_theme_name(theme)));
        m_theme = theme;
        if (m_measurementCanvas) m_measurementCanvas->setCanvasBackground(QColor(dark ? "#141b27" : "#f8fafc"));
        if (m_architecturalCanvas) m_architecturalCanvas->setCanvasBackground(QColor(dark ? "#141b27" : "#f8fafc"));
    }

    QString workspaceProfilesPath() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
               QStringLiteral("/workspace-profiles.json");
    }

    json loadWorkspaceProfiles() const {
        json result{{"schema", "sketch.workspace-profiles"}, {"version", 1},
                    {"profiles", json::array()}};
        QFile file(workspaceProfilesPath());
        if (!file.exists()) return result;
        if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
            throw std::runtime_error("Workspace profiles cannot be read.");
        }
        const auto document = json::parse(file.readAll().toStdString());
        if (!document.is_object() || document.size() != 3 ||
            document.at("schema") != "sketch.workspace-profiles" ||
            !document.at("version").is_number_integer() || document.at("version") != 1 ||
            !document.at("profiles").is_array() || document.at("profiles").size() > 64) {
            throw std::runtime_error("Unsupported workspace profiles format.");
        }
        std::set<std::string> names;
        for (const auto& profile : document.at("profiles")) {
            validate_workspace_profile_json(profile);
            if (!names.insert(profile.at("name").get<std::string>()).second)
                throw std::runtime_error("Workspace profile names must be unique.");
        }
        return document;
    }

    void saveWorkspaceProfiles(const json& document) const {
        if (!document.is_object() || document.value("schema", "") != "sketch.workspace-profiles" ||
            document.value("version", 0) != 1 || !document.contains("profiles") ||
            !document.at("profiles").is_array() || document.at("profiles").size() > 64) {
            throw std::invalid_argument("Workspace profiles document is invalid.");
        }
        std::set<std::string> names;
        for (const auto& profile : document.at("profiles")) {
            validate_workspace_profile_json(profile);
            if (!names.insert(profile.at("name").get<std::string>()).second)
                throw std::invalid_argument("Workspace profile names must be unique.");
        }
        const auto bytes = QByteArray::fromStdString(document.dump(2));
        QSaveFile file(workspaceProfilesPath());
        if (!QDir().mkpath(QFileInfo(file.fileName()).absolutePath()) ||
            !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            throw std::runtime_error("Workspace profiles could not be saved.");
        }
    }

    json captureWorkspaceProfile(const QString& profile_name) const {
        const auto name = profile_name.trimmed().toStdString();
        if (name.empty() || name.size() > 128)
            throw std::invalid_argument("Workspace profile name must be 1-128 characters.");
        const auto splitter_sizes = [](const QSplitter* splitter) {
            json result = json::array();
            if (splitter == nullptr) return result;
            for (const auto size : splitter->sizes()) result.push_back(size);
            return result;
        };
        json hidden_floors = json::array();
        for (const auto& id : m_view_filter.hidden_floor_ids) hidden_floors.push_back(id);
        json hidden_layers = json::array();
        for (const auto& id : m_view_filter.hidden_layer_ids) hidden_layers.push_back(id);
        auto profile = json{
            {"name", name},
            {"workspace", m_workspace == Workspace::measurement ? "measurement" : "architectural"},
            {"theme", workspace_theme_name(m_theme)},
            {"metric", m_metric_units},
            {"grid", m_grid_enabled},
            {"snap", m_snap_enabled},
            {"overview", m_overview_map_enabled},
            {"active_layer", m_active_layer_id.toStdString()},
            {"architectural_view", architectural_view_name(m_architectural_view_kind)},
            {"page_size", m_pageSizeCombo ? m_pageSizeCombo->currentData().toInt() : 0},
            {"workspace_splitter", splitter_sizes(m_workspace_splitter)},
            {"architectural_splitter", splitter_sizes(m_architectural_splitter)},
            {"hidden_floors", std::move(hidden_floors)},
            {"hidden_layers", std::move(hidden_layers)}};
        validate_workspace_profile_json(profile);
        return profile;
    }

    bool applyWorkspaceProfile(const json& profile) {
        try {
            validate_workspace_profile_json(profile);
            const auto decode_splitter_sizes = [](QSplitter* splitter, const json& encoded,
                                                  const char* key) {
                if (splitter == nullptr || !encoded.is_array()) return QList<int>{};
                if (encoded.size() != static_cast<std::size_t>(splitter->count()))
                    throw std::invalid_argument(std::string("Workspace profile ") + key +
                                                " does not match the current layout.");
                QList<int> sizes;
                sizes.reserve(static_cast<int>(encoded.size()));
                for (const auto& value : encoded) sizes.push_back(value.get<int>());
                return sizes;
            };
            const auto workspace_splitter_sizes = profile.contains("workspace_splitter")
                ? decode_splitter_sizes(m_workspace_splitter, profile.at("workspace_splitter"),
                                        "workspace_splitter")
                : QList<int>{};
            const auto architectural_splitter_sizes = profile.contains("architectural_splitter")
                ? decode_splitter_sizes(m_architectural_splitter, profile.at("architectural_splitter"),
                                        "architectural_splitter")
                : QList<int>{};
            const auto workspace = profile.at("workspace").get<std::string>() == "measurement"
                ? Workspace::measurement : Workspace::architectural;
            const auto theme_name = profile.at("theme").get<std::string>();
            const auto theme = theme_name == "dark" ? WorkspaceTheme::dark
                : theme_name == "high_contrast" ? WorkspaceTheme::high_contrast : WorkspaceTheme::light;
            const auto view_name = profile.at("architectural_view").get<std::string>();
            const auto view = view_name == "elevation" ? BuildingViewKind::elevation
                : view_name == "section" ? BuildingViewKind::section : BuildingViewKind::plan;
            m_workspace = workspace;
            m_metric_units = profile.at("metric").get<bool>();
            m_grid_enabled = profile.at("grid").get<bool>();
            m_snap_enabled = profile.at("snap").get<bool>();
            m_overview_map_enabled = profile.value("overview", true);
            m_architectural_view_kind = view;
            m_view_filter.hidden_floor_ids.clear();
            for (const auto& value : profile.at("hidden_floors"))
                m_view_filter.hidden_floor_ids.insert(value.get<std::string>());
            m_view_filter.hidden_layer_ids.clear();
            for (const auto& value : profile.at("hidden_layers"))
                m_view_filter.hidden_layer_ids.insert(value.get<std::string>());
            const auto active_layer = profile.at("active_layer").get<std::string>();
            const auto organization = organize_project(m_document->snapshot());
            const auto active = organization.nodes.find(active_layer);
            m_active_layer_id = active != organization.nodes.end() && active->second.type == "layer" &&
                                        organization.drawing_context(active_layer)
                                    ? QString::fromStdString(active_layer) : QString{};
            applyTheme(theme);
            if (m_workspaceTabs)
                m_workspaceTabs->setCurrentIndex(m_workspace == Workspace::measurement ? 0 : 1);
            if (m_unitsCombo) {
                QSignalBlocker blocker(m_unitsCombo);
                m_unitsCombo->setCurrentIndex(m_metric_units ? 1 : 0);
            }
            if (m_pageSizeCombo) {
                const auto index = m_pageSizeCombo->findData(profile.at("page_size").get<int>());
                if (index < 0) throw std::invalid_argument("Workspace profile page size is unavailable.");
                QSignalBlocker blocker(m_pageSizeCombo);
                m_pageSizeCombo->setCurrentIndex(index);
            }
            if (m_architecturalViewCombo) {
                const auto index = m_architecturalViewCombo->findData(static_cast<int>(view));
                QSignalBlocker blocker(m_architecturalViewCombo);
                m_architecturalViewCombo->setCurrentIndex(index);
            }
            if (m_grid_button) {
                QSignalBlocker blocker(m_grid_button);
                m_grid_button->setChecked(m_grid_enabled);
            }
            if (m_snap_button) {
                QSignalBlocker blocker(m_snap_button);
                m_snap_button->setChecked(m_snap_enabled);
            }
            if (m_overview_button) {
                QSignalBlocker blocker(m_overview_button);
                m_overview_button->setChecked(m_overview_map_enabled);
            }
            m_measurementCanvas->setMetricUnits(m_metric_units);
            m_architecturalCanvas->setMetricUnits(m_metric_units);
            m_measurementCanvas->setGridEnabled(m_grid_enabled);
            m_architecturalCanvas->setGridEnabled(m_grid_enabled);
            m_measurementCanvas->setSnapEnabled(m_snap_enabled);
            m_architecturalCanvas->setSnapEnabled(m_snap_enabled);
            m_measurementCanvas->setOverviewMapEnabled(m_overview_map_enabled);
            m_architecturalCanvas->setOverviewMapEnabled(m_overview_map_enabled);
            if (profile.contains("workspace_splitter")) m_workspace_splitter->setSizes(workspace_splitter_sizes);
            if (profile.contains("architectural_splitter"))
                m_architectural_splitter->setSizes(architectural_splitter_sizes);
            refresh();
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Workspace profile: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showWorkspaceProfiles() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("workspaceProfilesDialog"));
        dialog.setWindowTitle(QStringLiteral("Workspace profiles"));
        dialog.resize(520, 300);
        auto* layout = new QVBoxLayout(&dialog);
        auto* selector = new QComboBox(&dialog);
        selector->setObjectName(QStringLiteral("workspaceProfileSelector"));
        layout->addWidget(selector);
        auto* name = new QLineEdit(&dialog);
        name->setObjectName(QStringLiteral("workspaceProfileName"));
        name->setPlaceholderText(QStringLiteral("Field layout, permit set, or client review"));
        layout->addWidget(name);
        auto* help = new QLabel(QStringLiteral(
            "Profiles are stored locally and restore workspace, theme, units, grid, snap, view, page size, map visibility, panel proportions, and visibility filters."),
            &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("workspaceProfileStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* actions = new QHBoxLayout;
        auto* save = new QPushButton(QStringLiteral("Save current"), &dialog);
        save->setObjectName(QStringLiteral("saveWorkspaceProfile"));
        auto* apply = new QPushButton(QStringLiteral("Apply"), &dialog);
        apply->setObjectName(QStringLiteral("applyWorkspaceProfile"));
        auto* remove = new QPushButton(QStringLiteral("Delete"), &dialog);
        remove->setObjectName(QStringLiteral("deleteWorkspaceProfile"));
        auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
        actions->addWidget(save);
        actions->addWidget(apply);
        actions->addWidget(remove);
        actions->addStretch(1);
        actions->addWidget(close);
        layout->addLayout(actions);

        json profiles;
        const auto populate = [&] {
            const QSignalBlocker blocker(selector);
            selector->clear();
            for (const auto& profile : profiles.at("profiles"))
                selector->addItem(QString::fromStdString(profile.at("name").get<std::string>()));
            if (selector->count() > 0) selector->setCurrentIndex(0);
            name->setText(selector->currentText());
        };
        try {
            profiles = loadWorkspaceProfiles();
            populate();
            if (selector->count() == 0) status->setText(QStringLiteral("No saved profiles yet."));
        } catch (const std::exception& error) {
            profiles = json{{"schema", "sketch.workspace-profiles"}, {"version", 1},
                            {"profiles", json::array()}};
            status->setText(QStringLiteral("Saved profiles were ignored: %1")
                                .arg(QString::fromUtf8(error.what())));
        }
        QObject::connect(selector, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
            if (index >= 0 && index < static_cast<int>(profiles.at("profiles").size()))
                name->setText(QString::fromStdString(
                    profiles.at("profiles").at(index).at("name").get<std::string>()));
        });
        QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
            try {
                const auto profile = captureWorkspaceProfile(name->text());
                const auto profile_name = profile.at("name").get<std::string>();
                auto existing = std::find_if(profiles["profiles"].begin(), profiles["profiles"].end(),
                    [&](const auto& value) { return value.at("name").get<std::string>() == profile_name; });
                if (existing == profiles["profiles"].end()) profiles["profiles"].push_back(profile);
                else *existing = profile;
                saveWorkspaceProfiles(profiles);
                populate();
                selector->setCurrentText(QString::fromStdString(profile_name));
                status->setText(QStringLiteral("Profile saved locally."));
            } catch (const std::exception& error) {
                status->setText(QString::fromUtf8(error.what()));
            }
        });
        QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
            const auto index = selector->currentIndex();
            if (index < 0 || index >= static_cast<int>(profiles.at("profiles").size())) {
                status->setText(QStringLiteral("Choose a saved profile first."));
                return;
            }
            if (applyWorkspaceProfile(profiles.at("profiles").at(index))) {
                status->setText(QStringLiteral("Profile applied."));
            } else {
                status->setText(lastError());
            }
        });
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
            const auto index = selector->currentIndex();
            if (index < 0 || index >= static_cast<int>(profiles.at("profiles").size())) {
                status->setText(QStringLiteral("Choose a saved profile first."));
                return;
            }
            profiles["profiles"].erase(profiles["profiles"].begin() + index);
            try {
                saveWorkspaceProfiles(profiles);
                populate();
                status->setText(QStringLiteral("Profile deleted."));
            } catch (const std::exception& error) {
                status->setText(QString::fromUtf8(error.what()));
            }
        });
        QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
        dialog.exec();
    }

    struct RevisionDiffSummary {
        std::size_t added_entities = 0;
        std::size_t removed_entities = 0;
        std::size_t changed_entities = 0;
        std::size_t added_assets = 0;
        std::size_t removed_assets = 0;
        std::size_t changed_assets = 0;
    };

    static RevisionDiffSummary compareRevisions(const DocumentSnapshot& older,
                                                const DocumentSnapshot& newer) {
        RevisionDiffSummary result;
        const auto count_entities = [](const auto& before, const auto& after,
                                       std::size_t& added, std::size_t& removed,
                                       std::size_t& changed) {
            for (const auto& [id, value] : after) {
                const auto found = before.find(id);
                if (found == before.end()) ++added;
                else if (!(found->second == value)) ++changed;
            }
            for (const auto& [id, value] : before) {
                (void)value;
                if (!after.contains(id)) ++removed;
            }
        };
        count_entities(older.entities(), newer.entities(), result.added_entities,
                       result.removed_entities, result.changed_entities);
        count_entities(older.assets(), newer.assets(), result.added_assets,
                       result.removed_assets, result.changed_assets);
        return result;
    }

    bool restoreNamedRevision(const QString& name, const QString& path) {
        try {
            if (m_boundary_session) {
                throw std::runtime_error("Finish or cancel the active boundary before restoring a revision copy.");
            }
            const auto destination = path.trimmed();
            if (destination.isEmpty()) {
                throw std::invalid_argument("Choose a destination project file.");
            }
            const auto requested_name = name.trimmed().toStdString();
            const auto source = authoringSnapshot();
            const auto found = source.named_revisions().find(requested_name);
            if (found == source.named_revisions().end()) {
                throw std::invalid_argument("The selected named revision is no longer available.");
            }
            auto restored = Document::fork_at_revision(source, found->second);
            restored.mark_saved(restored.revision());
            (void)ProjectStore::save(filesystem_path(destination), restored.snapshot());
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Restore revision: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showRevisionHistory() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("revisionHistoryDialog"));
        dialog.setWindowTitle(QStringLiteral("Named revisions"));
        dialog.resize(760, 470);
        auto* layout = new QVBoxLayout(&dialog);
        auto* help = new QLabel(QStringLiteral(
            "Name stable project states as you work. Comparisons are read-only; restoring writes a new project copy and leaves this document unchanged."),
            &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);

        auto* body = new QHBoxLayout;
        auto* revisions = new QListWidget(&dialog);
        revisions->setObjectName(QStringLiteral("revisionList"));
        revisions->setSelectionMode(QAbstractItemView::SingleSelection);
        body->addWidget(revisions, 1);
        auto* comparison = new QPlainTextEdit(&dialog);
        comparison->setObjectName(QStringLiteral("revisionComparison"));
        comparison->setReadOnly(true);
        comparison->setPlaceholderText(QStringLiteral("Select a named revision and choose Compare."));
        body->addWidget(comparison, 2);
        layout->addLayout(body, 1);

        auto* name = new QLineEdit(&dialog);
        name->setObjectName(QStringLiteral("revisionName"));
        name->setPlaceholderText(QStringLiteral("Existing conditions, permit issue, client review"));
        name->setAccessibleName(QStringLiteral("Revision name"));
        layout->addWidget(name);

        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("revisionStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* actions = new QHBoxLayout;
        auto* name_current = new QPushButton(QStringLiteral("Name current revision"), &dialog);
        name_current->setObjectName(QStringLiteral("nameCurrentRevision"));
        auto* compare = new QPushButton(QStringLiteral("Compare to current"), &dialog);
        compare->setObjectName(QStringLiteral("compareRevisions"));
        auto* restore = new QPushButton(QStringLiteral("Restore as new project…"), &dialog);
        restore->setObjectName(QStringLiteral("restoreRevision"));
        auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
        actions->addWidget(name_current);
        actions->addWidget(compare);
        actions->addWidget(restore);
        actions->addStretch(1);
        actions->addWidget(close);
        layout->addLayout(actions);

        const auto populate = [&] {
            revisions->clear();
            try {
                const auto snapshot = authoringSnapshot();
                std::vector<std::pair<std::string, Revision>> entries;
                entries.reserve(snapshot.named_revisions().size());
                for (const auto& entry : snapshot.named_revisions()) entries.push_back(entry);
                std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
                    return left.second == right.second ? left.first < right.first : left.second < right.second;
                });
                for (const auto& [revision_name, revision] : entries) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  •  revision %2")
                            .arg(QString::fromStdString(revision_name))
                            .arg(revision), revisions);
                    item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(revision));
                    item->setData(Qt::UserRole + 1, QString::fromStdString(revision_name));
                    item->setToolTip(QStringLiteral("Document revision %1").arg(revision));
                }
                if (revisions->count() > 0) revisions->setCurrentRow(0);
                else status->setText(QStringLiteral("No named revisions yet."));
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Revision history is unavailable: %1")
                                    .arg(QString::fromUtf8(error.what())));
            }
        };
        populate();
        QObject::connect(revisions, &QListWidget::currentItemChanged, &dialog,
                         [name](QListWidgetItem* current, QListWidgetItem*) {
            if (current) name->setText(current->data(Qt::UserRole + 1).toString());
        });
        QObject::connect(name_current, &QPushButton::clicked, &dialog, [&] {
            try {
                if (m_boundary_session) throw std::runtime_error(
                    "Finish or cancel the active boundary before naming a revision.");
                const auto revision_name = name->text().trimmed().toStdString();
                const auto source = authoringSnapshot();
                applyDocumentCommand(NameRevision{source.revision(), revision_name});
                refresh();
                populate();
                status->setText(QStringLiteral("Named revision saved in project history."));
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Name revision: %1").arg(QString::fromUtf8(error.what())));
            }
        });
        QObject::connect(compare, &QPushButton::clicked, &dialog, [&] {
            const auto* item = revisions->currentItem();
            if (!item) {
                status->setText(QStringLiteral("Choose a named revision first."));
                return;
            }
            try {
                const auto source = authoringSnapshot();
                const auto revision = item->data(Qt::UserRole).toULongLong();
                const auto name_text = item->data(Qt::UserRole + 1).toString();
                auto historical = Document::fork_at_revision(source, revision);
                const auto older = historical.snapshot();
                const auto diff = compareRevisions(older, source);
                comparison->setPlainText(
                    QStringLiteral("%1\n\nNamed revision: %2\nCurrent revision: %3\n\n"
                                   "Entities added: %4\nEntities removed: %5\nEntities changed: %6\n"
                                   "Assets added: %7\nAssets removed: %8\nAssets changed: %9\n\n"
                                   "The original revision remains immutable.\n")
                        .arg(name_text)
                        .arg(revision)
                        .arg(source.revision())
                        .arg(diff.added_entities)
                        .arg(diff.removed_entities)
                        .arg(diff.changed_entities)
                        .arg(diff.added_assets)
                        .arg(diff.removed_assets)
                        .arg(diff.changed_assets));
                status->setText(QStringLiteral("Comparison generated without changing the document."));
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Compare revisions: %1").arg(QString::fromUtf8(error.what())));
            }
        });
        QObject::connect(restore, &QPushButton::clicked, &dialog, [&] {
            const auto* item = revisions->currentItem();
            if (!item) {
                status->setText(QStringLiteral("Choose a named revision first."));
                return;
            }
            const auto path = QFileDialog::getSaveFileName(
                owner, QStringLiteral("Restore named revision as"), {},
                QStringLiteral("Property Studio project (*.bldproj)"));
            if (path.isEmpty()) return;
            if (restoreNamedRevision(item->data(Qt::UserRole + 1).toString(), path)) {
                status->setText(QStringLiteral("Revision copy written. The current document is unchanged."));
            } else {
                status->setText(lastError());
            }
        });
        QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
        dialog.exec();
    }

    void showBoundaryTransformEditor() {
        const auto context = captureModalContext();
        const auto source = authoringSnapshot();
        const auto original = selectedEntity();
        const bool supported_selection = original &&
            (original->type == "wall" || is_closed_boundary_entity(original->type));
        std::optional<std::pair<Command, std::string>> candidate_command;
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("boundaryTransformDialog"));
        dialog.setWindowTitle(QStringLiteral("Transform selection"));
        dialog.resize(520, 330);
        auto* layout = new QVBoxLayout(&dialog);
        auto* help = new QLabel(QStringLiteral(
            "Pivot: boundary bounds center or wall endpoint midpoint. Rotation is in degrees; offsets use the current input units."),
            &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* form = new QFormLayout;
        auto* rotation = new QLineEdit(QStringLiteral("0"), &dialog);
        rotation->setObjectName(QStringLiteral("boundaryRotationDegrees"));
        rotation->setAccessibleName(QStringLiteral("Rotation in degrees"));
        form->addRow(QStringLiteral("Rotation"), rotation);
        auto* offset_x = new QLineEdit(QStringLiteral("0"), &dialog);
        offset_x->setObjectName(QStringLiteral("boundaryOffsetX"));
        offset_x->setAccessibleName(QStringLiteral("Horizontal offset"));
        form->addRow(QStringLiteral("Offset X"), offset_x);
        auto* offset_y = new QLineEdit(QStringLiteral("0"), &dialog);
        offset_y->setObjectName(QStringLiteral("boundaryOffsetY"));
        offset_y->setAccessibleName(QStringLiteral("Vertical offset"));
        form->addRow(QStringLiteral("Offset Y"), offset_y);
        layout->addLayout(form);
        auto* flip_horizontal = new QCheckBox(QStringLiteral("Flip horizontally"), &dialog);
        flip_horizontal->setObjectName(QStringLiteral("boundaryFlipHorizontal"));
        auto* flip_vertical = new QCheckBox(QStringLiteral("Flip vertically"), &dialog);
        flip_vertical->setObjectName(QStringLiteral("boundaryFlipVertical"));
        layout->addWidget(flip_horizontal);
        layout->addWidget(flip_vertical);
        auto* clone = new QCheckBox(QStringLiteral("Create a copy"), &dialog);
        clone->setObjectName(QStringLiteral("boundaryClone"));
        layout->addWidget(clone);
        PlanCanvas* preview = nullptr;
        if (supported_selection) {
            preview = new PlanCanvas(&dialog);
            preview->setObjectName("wallTransformPreview");
            preview->setAccessibleName("Selection transform preview");
            preview->setMinimumHeight(200);
            preview->setCanvasBackground(QColor(248, 250, 253));
            preview->setOverviewMapEnabled(false);
            preview->setGridEnabled(false);
            layout->addWidget(preview, 1);
            layout->addWidget(new QLabel("Gray: original    Blue: proposed", &dialog));
            dialog.resize(520, 520);
        }
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("boundaryTransformStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
        buttons->setObjectName(QStringLiteral("boundaryTransformButtons"));
        layout->addWidget(buttons);
        const auto update_preview = [&] {
            if (!supported_selection) return;
            candidate_command.reset();
            try {
                if (!m_document->is_editable()) throw std::invalid_argument("This document is read-only.");
                if (m_boundary_session) throw std::invalid_argument("Finish or cancel the active boundary before transforming the selection.");
                if (!modalContextUnchanged(context)) throw std::invalid_argument(lastError().toStdString());
                auto candidate = original->type == "wall" ? makeWallTransformCommand(source, *original, rotation->text(),
                    flip_horizontal->isChecked(), flip_vertical->isChecked(), offset_x->text(),
                    offset_y->text(), clone->isChecked()) : makeBoundaryTransformCommand(source, *original,
                    rotation->text(), flip_horizontal->isChecked(), flip_vertical->isChecked(),
                    offset_x->text(), offset_y->text(), clone->isChecked());
                const auto* changes = std::get_if<ApplyEntityChanges>(&candidate.first);
                const auto proposed = changes && changes->entity_changes.empty() ? source :
                    Document::preview_command(source, candidate.first);
                std::vector<CanvasEntity> geometry;
                const auto add_graph = [&](const DocumentSnapshot& snapshot, const std::string& root, bool selected) {
                    const auto& root_entity = snapshot.entities().at(root);
                    if (is_closed_boundary_entity(root_entity.type)) {
                        const auto boundary = read_boundary(root_entity.properties);
                        if (boundary.empty()) throw std::invalid_argument("Boundary has no preview geometry.");
                        geometry.push_back({id_from(root), selected ? id_from(root_entity.type) : QStringLiteral("source"),
                            boundary, 0, selected});
                        return;
                    }
                    const auto graph = clipboard_entities_for_selection(snapshot, root);
                    std::vector<const Entity*> openings;
                    for (const auto& entity : graph) if (entity.type == "opening") openings.push_back(&entity);
                    Wall wall;
                    std::string error;
                    if (!read_document_wall(snapshot.entities().at(root), openings, wall, error))
                        throw std::invalid_argument(error);
                    validate_wall_semantics(wall);
                    geometry.push_back({id_from(root), selected ? "wall" : "source",
                        wall_segments_without_openings(wall.baseline, wall.openings), wall.thickness, selected});
                    for (const auto* opening : openings) {
                        if (opening->properties.value("opening_kind", std::string{}) != "door" ||
                            !opening->properties.contains("door_operation")) continue;
                        const auto found = std::find_if(wall.openings.begin(), wall.openings.end(),
                            [&](const auto& item) { return item.id == opening->id; });
                        geometry.push_back({id_from(opening->id), "opening",
                            door_plan_symbol(wall.baseline, found->offset, found->width,
                                decode_door_operation(opening->properties.at("door_operation"))), 0, selected});
                    }
                };
                add_graph(source, original->id, false);
                add_graph(proposed, candidate.second, true);
                preview->setEntities(std::move(geometry));
                preview->fitView();
                candidate_command = std::move(candidate);
                status->clear();
                buttons->button(QDialogButtonBox::Apply)->setEnabled(true);
            } catch (const std::exception& error) {
                preview->setEntities({});
                status->setText(QString::fromUtf8(error.what()));
                buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
            }
        };
        if (supported_selection) {
            for (auto* field : {rotation, offset_x, offset_y})
                QObject::connect(field, &QLineEdit::textChanged, &dialog, update_preview);
            for (auto* field : {flip_horizontal, flip_vertical, clone})
                QObject::connect(field, &QCheckBox::toggled, &dialog, update_preview);
            update_preview();
        }
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
            if (!modalContextUnchanged(context)) {
                status->setText(lastError());
                if (supported_selection) {
                    candidate_command.reset();
                    preview->setEntities({});
                    buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
                }
                return;
            }
            if (supported_selection) {
                if (!candidate_command) return;
                try {
                    const auto* changes = std::get_if<ApplyEntityChanges>(&candidate_command->first);
                    if (!changes || !changes->entity_changes.empty()) {
                        applyDocumentCommand(candidate_command->first);
                        m_selected_id = id_from(candidate_command->second);
                        refresh();
                    }
                    clearError();
                    dialog.accept();
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
                return;
            }
        });
        if (!selectedEntity().has_value() ||
            (!is_closed_boundary_entity(selectedEntity()->type) && selectedEntity()->type != "wall")) {
            status->setText(QStringLiteral("Select a wall or an identified closed boundary first."));
            buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        }
        dialog.exec();
    }

    void showBoundaryVertexInsertion() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("boundaryVertexInsertionDialog"));
        dialog.setWindowTitle(QStringLiteral("Insert boundary vertex"));
        dialog.resize(440, 220);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* segment = new QComboBox(&dialog);
        segment->setObjectName(QStringLiteral("boundaryVertexSegment"));
        segment->setAccessibleName(QStringLiteral("Boundary edge"));
        auto* fraction = new QLineEdit(QStringLiteral("0.5"), &dialog);
        fraction->setObjectName(QStringLiteral("boundaryVertexFraction"));
        fraction->setAccessibleName(QStringLiteral("Edge fraction"));
        form->addRow(QStringLiteral("Edge"), segment);
        form->addRow(QStringLiteral("Fraction (0..1)"), fraction);
        layout->addLayout(form);
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("boundaryVertexInsertionStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
                                             &dialog);
        buttons->setObjectName(QStringLiteral("boundaryVertexInsertionButtons"));
        layout->addWidget(buttons);
        const auto selected = selectedEntity();
        if (!selected.has_value() || !is_closed_boundary_entity(selected->type)) {
            status->setText(QStringLiteral("Select an identified closed boundary first."));
            buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        } else {
            try {
                const auto identified = decode_identified_boundary_entity(*selected);
                for (const auto& edge : identified.segments) {
                    segment->addItem(QString::fromStdString(edge.segment_id),
                                     QString::fromStdString(edge.segment_id));
                }
                if (segment->count() == 0) {
                    status->setText(QStringLiteral("The selected boundary has no edges."));
                    buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
                }
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Boundary is unavailable: %1")
                                    .arg(QString::fromUtf8(error.what())));
                buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
            }
        }
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                         &dialog, [&] {
            if (insertSelectedBoundaryVertex(segment->currentData().toString(), fraction->text())) {
                dialog.accept();
            } else {
                status->setText(lastError());
            }
        });
        dialog.exec();
    }

    void showBoundaryRedefinition() {
        const auto selected = selectedEntity();
        if (!selected.has_value() || !is_closed_boundary_entity(selected->type)) {
            setError(QStringLiteral("Select an identified closed boundary first."));
            return;
        }
        try {
            const auto version = inspect_boundary_entity_version(*selected);
            if (version.format == BoundaryEntityFormat::unsupported_version) {
                throw std::invalid_argument("This boundary uses an unsupported model version.");
            }
            if (version.format == BoundaryEntityFormat::anonymous_legacy) {
                throw std::invalid_argument(
                    "This legacy boundary needs an explicit identity upgrade before redefinition.");
            }
            if (selected->properties.contains("boundary_authoring")) {
                throw std::invalid_argument(
                    "Receipt-bound boundaries require an explicit derivation policy before redefinition.");
            }
            const auto source_id = m_selected_id;
            const auto classification = QString::fromStdString(
                read_string(selected->properties, "classification").value_or(""));
            if (!beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, classification)) return;
            m_redefine_boundary_id = source_id;
            owner->statusBar()->showMessage(
                QStringLiteral("Redefining boundary  •  draw the replacement with the same number of edges"),
                6000);
        } catch (const std::exception& error) {
            setError(QStringLiteral("Redefine boundary: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void showAutomaticAreaDetection() {
        const auto selected = selectedEntity();
        if (!selected.has_value() || selected->type != "wall") {
            setError(QStringLiteral("Select a wall in the floor and layer to inspect."));
            return;
        }
        const auto context = captureModalContext();
        bool accepted = false;
        const auto initial = QStringLiteral("room");
        const auto classification = QInputDialog::getText(
            owner, QStringLiteral("Detect closed areas"),
            QStringLiteral("Classification for each detected room:"), QLineEdit::Normal,
            initial, &accepted);
        if (!accepted || classification.trimmed().isEmpty()) return;
        if (!modalContextUnchanged(context)) return;
        const auto created = detectRoomBoundariesFromExistingWalls(
            classification.trimmed(), context.revision);
        if (!created.isEmpty()) {
            owner->statusBar()->showMessage(
                QStringLiteral("Detected %1 closed area%2.")
                    .arg(created.size()).arg(created.size() == 1 ? QString{} : QStringLiteral("s")),
                6000);
        }
    }

    void styleDialog(QDialog& dialog) const {
        // Top-level Qt dialogs do not always inherit a parent window's style
        // sheet. Copy the already-resolved palette and stylesheet so modal
        // editors use the same modern surfaces, focus rings, and spacing.
        dialog.setPalette(owner->palette());
        dialog.setStyleSheet(owner->styleSheet());
    }

    void initializeDrawingContext() {
        m_active_layer_id.clear();
        const auto organization = organize_project(m_document->snapshot());
        for (const auto& [id, node] : organization.nodes) {
            if (node.type == "layer" && organization.drawing_context(id)) {
                if (!m_active_layer_id.isEmpty()) {
                    m_active_layer_id.clear();
                    return;
                }
                m_active_layer_id = id_from(id);
            }
        }
    }

    std::optional<DrawingContext> requireDrawingContext() {
        const auto organization = organize_project(m_document->snapshot());
        const auto context = organization.drawing_context(m_active_layer_id.toStdString());
        if (!context)
            setError(QStringLiteral("Choose an existing drawing layer before creating geometry."));
        return context;
    }

    bool assignDrawingContext(json& properties) {
        const bool explicit_context = properties.contains("layer_id") || properties.contains("floor_id") ||
            properties.contains("building_id") || properties.contains("property_id");
        std::optional<DrawingContext> context;
        if (explicit_context) {
            const auto layer = read_string(properties, "layer_id");
            if (layer) context = organize_project(m_document->snapshot()).drawing_context(*layer);
            if (!context) {
                setError(QStringLiteral("The supplied object placement needs a valid layer and its floor."));
                return false;
            }
            for (const auto& [key, expected] : std::vector<std::pair<std::string, std::string>>{
                     {"property_id", context->property_id}, {"building_id", context->building_id},
                     {"floor_id", context->floor_id}, {"layer_id", context->layer_id}}) {
                if (properties.contains(key) && properties.at(key) != expected) {
                    setError(QStringLiteral("The supplied object placement has conflicting %1.")
                                 .arg(QString::fromStdString(key)));
                    return false;
                }
            }
        } else {
            context = requireDrawingContext();
        }
        if (!context) return false;
        properties["floor_id"] = context->floor_id;
        properties["layer_id"] = context->layer_id;
        return true;
    }

    QString activeLayerId() const { return m_active_layer_id; }

    bool setActiveLayer(const QString& id) {
        const auto organization = organize_project(m_document->snapshot());
        const auto found = organization.nodes.find(id.toStdString());
        if (found == organization.nodes.end() || found->second.type != "layer" ||
            !organization.drawing_context(id.toStdString())) {
            setError(QStringLiteral("Choose a layer with a valid property, building and floor."));
            return false;
        }
        if (m_active_layer_id == id) return true;
        if (!confirmDiscardBoundaryDraft()) return false;
        m_active_layer_id = id;
        clearPreview();
        m_tool = CanvasTool::select;
        syncToolControls();
        clearError();
        refresh();
        return true;
    }

    bool setContainerVisible(const QString& id, bool visible) {
        const auto snapshot = m_document->snapshot();
        const auto found = snapshot.entities().find(id.toStdString());
        if (found == snapshot.entities().end() || !is_visibility_container_type(found->second.type)) {
            setError(QStringLiteral("Visibility can be changed only for a floor or drawing layer."));
            return false;
        }

        auto& hidden = found->second.type == "floor"
            ? m_view_filter.hidden_floor_ids
            : m_view_filter.hidden_layer_ids;
        const auto key = found->first;
        const bool already_visible = !hidden.contains(key);
        if (already_visible == visible) {
            return true;
        }
        if (visible) {
            hidden.erase(key);
        } else {
            hidden.insert(key);
        }
        clearError();
        refresh();
        return true;
    }

    void showAllContainers() {
        if (m_view_filter.hidden_floor_ids.empty() && m_view_filter.hidden_layer_ids.empty()) {
            refreshNavigator();
            return;
        }
        m_view_filter = {};
        clearError();
        refresh();
    }

    [[nodiscard]] bool entityVisible(const QString& id) const {
        try {
            const auto visible = visible_project_entities_with_phase(m_document->snapshot(), m_view_filter);
            return visible.contains(id.toStdString());
        } catch (const std::exception&) {
            // A malformed phase record is a document error; organization
            // visibility remains fail-open for selection and diagnostics.
            const auto visible = visible_project_entities(m_document->snapshot(), m_view_filter);
            return visible.contains(id.toStdString());
        }
    }

    [[nodiscard]] QString createDrawingSheet(const QString& number,
                                             const QString& width_mm,
                                             const QString& height_mm,
                                             const QString& title) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return {};
        }
        try {
            const auto sheet_number = number.trimmed();
            if (sheet_number.isEmpty()) throw std::invalid_argument("Sheet number cannot be empty");
            const auto parse_finite = [](const QString& text, const char* label) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value) || value <= 0.0)
                    throw std::invalid_argument(std::string(label) + " must be finite and positive");
                return value;
            };
            const auto width = parse_finite(width_mm, "Sheet width");
            const auto height = parse_finite(height_mm, "Sheet height");
            const auto source = authoringSnapshot();
            const auto record = decode_sheet_model(source);
            if (!record || record->model.sheets().empty())
                throw std::invalid_argument("No typed drawing sheet is available");
            for (const auto& sheet : record->model.sheets()) {
                if (sheet.number == sheet_number.toStdString())
                    throw std::invalid_argument("Sheet number is already in use");
            }
            std::string id;
            do {
                id = "sheet-" + make_stable_id();
            } while (std::any_of(record->model.sheets().begin(), record->model.sheets().end(),
                                 [&](const auto& sheet) { return sheet.id == id; }));
            DrawingSheet addition;
            addition.id = id;
            addition.number = sheet_number.toStdString();
            addition.width_mm = width;
            addition.height_mm = height;
            addition.title_block = record->model.sheets().front().title_block;
            addition.title_block.title = title.trimmed().isEmpty()
                ? std::string("New sheet") : title.trimmed().toStdString();

            // Seed one independently scaled viewport per coordinated view so
            // a new page is immediately useful.  A small custom page simply
            // starts blank and can be populated through the viewport editor.
            const auto& views = record->model.views();
            constexpr double margin = 10.0;
            constexpr double gap = 5.0;
            const auto columns = std::min<std::size_t>(2, std::max<std::size_t>(1, views.size()));
            const auto rows = views.empty() ? 0U : (views.size() + columns - 1) / columns;
            const auto usable_width = width - margin * 2.0 - gap * static_cast<double>(columns - 1);
            const auto usable_height = height - margin * 2.0 - gap * static_cast<double>(rows > 0 ? rows - 1 : 0);
            const auto cell_width = columns > 0 ? usable_width / static_cast<double>(columns) : 0.0;
            const auto cell_height = rows > 0 ? usable_height / static_cast<double>(rows) : 0.0;
            if (cell_width > 0.0 && cell_height > 0.0) {
                for (std::size_t index = 0; index < views.size(); ++index) {
                    const auto column = index % columns;
                    const auto row = index / columns;
                    addition.viewports.push_back({
                        id + "-viewport-" + std::to_string(index), views[index].id,
                        {margin + static_cast<double>(column) * (cell_width + gap),
                         margin + static_cast<double>(row) * (cell_height + gap),
                         cell_width, cell_height}, 100.0});
                }
            }
            const auto updated_model = record->model.with_added_sheet(std::move(addition));
            auto updated_entity = source.entities().at(record->entity_id);
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Create drawing sheet"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_output_sheet_id = QString::fromStdString(id);
            clearError();
            refresh();
            return QString::fromStdString(id);
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet create: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    [[nodiscard]] bool removeDrawingSheet(const QString& sheet_id) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto wanted = sheet_id.trimmed().toStdString();
            if (wanted.empty()) throw std::invalid_argument("Choose a sheet to remove");
            const auto source = authoringSnapshot();
            const auto record = decode_sheet_model(source);
            if (!record) throw std::invalid_argument("No typed drawing sheet is available");
            const auto updated_model = record->model.with_removed_sheet(wanted);
            auto updated_entity = source.entities().at(record->entity_id);
            updated_entity.properties = make_sheet_view_entity(
                updated_entity.id, updated_model).properties;
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(updated_entity))}, {},
                "Remove drawing sheet"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            if (m_output_sheet_id == QString::fromStdString(wanted) ||
                !std::any_of(updated_model.sheets().begin(), updated_model.sheets().end(),
                             [&](const auto& sheet) { return QString::fromStdString(sheet.id) == m_output_sheet_id; })) {
                m_output_sheet_id = QString::fromStdString(updated_model.sheets().front().id);
            }
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet remove: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool selectOutputSheet(const QString& sheet_id) {
        try {
            const auto wanted = sheet_id.trimmed().toStdString();
            const auto record = decode_sheet_model(m_document->snapshot());
            if (!record || !std::any_of(record->model.sheets().begin(), record->model.sheets().end(),
                                        [&](const auto& sheet) { return sheet.id == wanted; })) {
                throw std::invalid_argument("Drawing sheet identity was not found");
            }
            m_output_sheet_id = QString::fromStdString(wanted);
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet selection: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] QString outputSheetId() const {
        try {
            const auto record = decode_sheet_model(m_document->snapshot());
            if (!record || record->model.sheets().empty()) return {};
            if (!m_output_sheet_id.isEmpty() &&
                std::any_of(record->model.sheets().begin(), record->model.sheets().end(),
                            [&](const auto& sheet) {
                                return QString::fromStdString(sheet.id) == m_output_sheet_id;
                            })) {
                return m_output_sheet_id;
            }
            return QString::fromStdString(record->model.sheets().front().id);
        } catch (const std::exception&) {
            return {};
        }
    }

    [[nodiscard]] QString activeRemodelingAlternative() const {
        try {
            const auto record = decode_phase_model(m_document->snapshot());
            if (!record || !record->model.active_alternative()) return {};
            return id_from(*record->model.active_alternative());
        } catch (const std::exception&) {
            return {};
        }
    }

    [[nodiscard]] bool selectRemodelingAlternative(const QString& alternative_id) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto source = authoringSnapshot();
            const auto record = decode_phase_model(source);
            if (!record) throw std::invalid_argument("Create a design phase record before selecting an alternative.");
            const auto trimmed = alternative_id.trimmed();
            const std::optional<std::string> selected = trimmed.isEmpty()
                ? std::nullopt : std::optional<std::string>(trimmed.toStdString());
            const auto command = model_phase_selection_command(
                source, record->entity_id, selected, source.revision());
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Design phase: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool ensureModelPhaseRecord() {
        try {
            const auto source = authoringSnapshot();
            if (decode_phase_model(source).has_value()) return true;
            const auto model_ids = phase_model_entity_ids(source);
            if (model_ids.empty()) {
                throw std::invalid_argument(
                    "Add a wall, room, boundary, slab, or architectural object before creating a design phase.");
            }
            const auto model = ModelPhases::create(model_ids, model_ids, {});
            auto entity = Entity::create("model_phases", {{"model", model.to_json()}});
            entity.id = new_id("model-phases");
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                "Create design phase record"};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Design phase: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showRemodelingAlternatives() {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        if (!ensureModelPhaseRecord()) return;
        try {
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setObjectName(QStringLiteral("remodelingAlternativesDialog"));
            dialog.setWindowTitle(QStringLiteral("Design phases and alternatives"));
            dialog.setModal(true);
            dialog.resize(620, 540);
            auto* layout = new QVBoxLayout(&dialog);

            auto* phase = new QComboBox(&dialog);
            phase->setObjectName(QStringLiteral("remodelingPhaseSelection"));
            phase->setToolTip(QStringLiteral(
                "Choose the shared existing model or one remodeling alternative to display."));
            layout->addWidget(new QLabel(QStringLiteral("Display phase"), &dialog));
            layout->addWidget(phase);

            auto* name = new QLineEdit(&dialog);
            name->setObjectName(QStringLiteral("remodelingAlternativeName"));
            name->setPlaceholderText(QStringLiteral("Example: Kitchen remodel"));
            layout->addWidget(new QLabel(QStringLiteral("New alternative name"), &dialog));
            layout->addWidget(name);

            auto* demolition = new QListWidget(&dialog);
            demolition->setObjectName(QStringLiteral("remodelingDemolitionList"));
            demolition->setSelectionMode(QAbstractItemView::NoSelection);
            layout->addWidget(new QLabel(QStringLiteral("Demolish baseline objects in the new alternative"), &dialog));
            layout->addWidget(demolition, 1);

            auto* status = new QLabel(&dialog);
            status->setObjectName(QStringLiteral("remodelingAlternativesStatus"));
            status->setWordWrap(true);
            status->setTextFormat(Qt::PlainText);
            layout->addWidget(status);

            auto* buttons = new QHBoxLayout();
            auto* apply = new QPushButton(QStringLiteral("Apply phase"), &dialog);
            apply->setObjectName(QStringLiteral("applyRemodelingPhase"));
            auto* create = new QPushButton(QStringLiteral("Create alternative"), &dialog);
            create->setObjectName(QStringLiteral("createRemodelingAlternative"));
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            close->setDefault(true);
            buttons->addStretch(1);
            buttons->addWidget(apply);
            buttons->addWidget(create);
            buttons->addWidget(close);
            layout->addLayout(buttons);

            std::optional<PhaseModelRecord> record;
            const auto refresh_selection = [&] {
                if (!record) return;
                const auto selected = phase->currentData().toString().toStdString();
                const auto found = selected.empty()
                    ? record->model.alternatives().end()
                    : std::find_if(record->model.alternatives().begin(),
                                   record->model.alternatives().end(),
                        [&](const auto& candidate) { return candidate.id == selected; });
                demolition->clear();
                name->setText(found == record->model.alternatives().end()
                                  ? QString{} : QString::fromStdString(found->name));
                std::set<std::string, std::less<>> selected_demolitions;
                if (found != record->model.alternatives().end()) {
                    selected_demolitions.insert(found->demolished_ids.begin(),
                                                found->demolished_ids.end());
                }
                const auto snapshot = authoringSnapshot();
                for (const auto& id : record->model.baseline_ids()) {
                    const auto entity = snapshot.entities().find(id);
                    const auto type = entity == snapshot.entities().end()
                        ? QStringLiteral("object")
                        : QString::fromStdString(entity->second.type);
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2").arg(type, id_from(id)), demolition);
                    item->setData(Qt::UserRole, id_from(id));
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(selected_demolitions.contains(id)
                                            ? Qt::Checked : Qt::Unchecked);
                }
            };
            const auto refresh_dialog = [&] {
                record = decode_phase_model(authoringSnapshot());
                if (!record) return;
                const QSignalBlocker blocker(phase);
                phase->clear();
                phase->addItem(QStringLiteral("Existing baseline"), QString{});
                for (const auto& alternative : record->model.alternatives()) {
                    phase->addItem(phase_alternative_label(alternative),
                                   id_from(alternative.id));
                }
                const auto active = record->model.active_alternative()
                    ? id_from(*record->model.active_alternative()) : QString{};
                const auto index = phase->findData(active);
                phase->setCurrentIndex(index >= 0 ? index : 0);
                refresh_selection();
                status->setText(QStringLiteral("%1 model objects · %2 alternative%3.")
                    .arg(record->model.entity_ids().size())
                    .arg(record->model.alternatives().size())
                    .arg(record->model.alternatives().size() == 1 ? QString{} : QStringLiteral("s")));
            };
            refresh_dialog();

            QObject::connect(phase, &QComboBox::currentIndexChanged, &dialog,
                             [&](int) { refresh_selection(); });
            QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
                if (!phase->count()) return;
                if (selectRemodelingAlternative(phase->currentData().toString())) {
                    refresh_dialog();
                    status->setText(QStringLiteral("Active phase saved through document history."));
                } else {
                    status->setText(lastError());
                }
            });
            QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto alternative_name = name->text().trimmed();
                    if (alternative_name.isEmpty())
                        throw std::invalid_argument("Enter a name for the new alternative.");
                    const auto source = authoringSnapshot();
                    const auto current = decode_phase_model(source);
                    if (!current) throw std::invalid_argument("The design phase record is unavailable.");
                    RemodelingAlternative candidate;
                    candidate.id = new_id("alternative");
                    candidate.name = alternative_name.toStdString();
                    const auto candidate_id = candidate.id;
                    for (int index = 0; index < demolition->count(); ++index) {
                        const auto* item = demolition->item(index);
                        if (item->checkState() == Qt::Checked)
                            candidate.demolished_ids.push_back(item->data(Qt::UserRole).toString().toStdString());
                    }
                    const auto updated = current->model.with_alternative(std::move(candidate));
                    const auto selected = updated.with_active(candidate_id);
                    auto entity = source.entities().at(current->entity_id);
                    entity.properties["model"] = selected.to_json();
                    const ApplyEntityChanges command{
                        source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                        "Create remodeling alternative"};
                    (void)Document::preview_command(source, Command{command});
                    applyDocumentCommand(Command{command});
                    clearError();
                    refresh();
                    refresh_dialog();
                    status->setText(QStringLiteral("Alternative created and selected."));
                } catch (const std::exception& error) {
                    setError(QStringLiteral("Design phase: %1").arg(QString::fromUtf8(error.what())));
                    status->setText(lastError());
                }
            });
            QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
            dialog.exec();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Design phase: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    bool ensureRoomRelationshipRecord() {
        try {
            const auto source = authoringSnapshot();
            if (decode_room_relationships(source).has_value()) return true;
            const auto references = document_room_references(source);
            if (references.empty()) {
                throw std::invalid_argument(
                    "Add a room boundary, measurement boundary, or wall before setting relationships.");
            }
            auto entity = Entity::create(
                "room_relationships",
                {{"model", RoomRelationshipSnapshot::create(references, {}).to_json()}});
            entity.id = new_id("room-relationships");
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                "Create room relationship record"};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Room relationships: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool applyRoomRelationshipModel(const RoomRelationshipSnapshot& model,
                                    const QString& message) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto source = authoringSnapshot();
            const auto record = decode_room_relationships(source);
            if (!record) {
                throw std::invalid_argument("The room relationship record is unavailable.");
            }
            auto entity = source.entities().at(record->entity_id);
            entity.properties["model"] = model.to_json();
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                message.toStdString()};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Room relationships: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showRoomRelationships() {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        if (!ensureRoomRelationshipRecord()) return;
        try {
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setObjectName(QStringLiteral("roomRelationshipsDialog"));
            dialog.setWindowTitle(QStringLiteral("Room and boundary relationships"));
            dialog.setModal(true);
            dialog.resize(680, 520);
            auto* layout = new QVBoxLayout(&dialog);

            auto* source = new QComboBox(&dialog);
            source->setObjectName(QStringLiteral("roomRelationshipSource"));
            auto* target = new QComboBox(&dialog);
            target->setObjectName(QStringLiteral("roomRelationshipTarget"));
            auto* kind = new QComboBox(&dialog);
            kind->setObjectName(QStringLiteral("roomRelationshipKind"));
            kind->addItem(room_relation_kind_label(RoomRelationKind::independent),
                          static_cast<int>(RoomRelationKind::independent));
            kind->addItem(room_relation_kind_label(RoomRelationKind::follows),
                          static_cast<int>(RoomRelationKind::follows));
            kind->addItem(room_relation_kind_label(RoomRelationKind::derived_from),
                          static_cast<int>(RoomRelationKind::derived_from));
            auto* form = new QFormLayout;
            form->addRow(QStringLiteral("Source"), source);
            form->addRow(QStringLiteral("Target"), target);
            form->addRow(QStringLiteral("Relationship"), kind);
            layout->addLayout(form);

            auto* hint = new QLabel(
                QStringLiteral("A relationship is explicit. Source follows or derives from target; "
                               "independent is symmetric. Geometry is never inferred."),
                &dialog);
            hint->setWordWrap(true);
            hint->setObjectName(QStringLiteral("roomRelationshipHint"));
            layout->addWidget(hint);

            auto* relations = new QListWidget(&dialog);
            relations->setObjectName(QStringLiteral("roomRelationshipList"));
            relations->setSelectionMode(QAbstractItemView::SingleSelection);
            layout->addWidget(new QLabel(QStringLiteral("Declared relationships"), &dialog));
            layout->addWidget(relations, 1);

            auto* status = new QLabel(&dialog);
            status->setObjectName(QStringLiteral("roomRelationshipStatus"));
            status->setWordWrap(true);
            status->setTextFormat(Qt::PlainText);
            layout->addWidget(status);

            auto* buttons = new QHBoxLayout;
            auto* add = new QPushButton(QStringLiteral("Add relationship"), &dialog);
            add->setObjectName(QStringLiteral("addRoomRelationship"));
            auto* remove = new QPushButton(QStringLiteral("Remove selected"), &dialog);
            remove->setObjectName(QStringLiteral("removeRoomRelationship"));
            auto* sync = new QPushButton(QStringLiteral("Sync references"), &dialog);
            sync->setObjectName(QStringLiteral("syncRoomRelationships"));
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            close->setDefault(true);
            buttons->addWidget(add);
            buttons->addWidget(remove);
            buttons->addWidget(sync);
            buttons->addStretch(1);
            buttons->addWidget(close);
            layout->addLayout(buttons);

            std::optional<RoomRelationshipRecord> record;
            Revision record_revision{};
            const auto populate_references = [&] {
                const auto snapshot = authoringSnapshot();
                const auto references = document_room_references(snapshot);
                const QSignalBlocker source_blocker(source);
                const QSignalBlocker target_blocker(target);
                source->clear();
                target->clear();
                for (const auto& reference : references) {
                    const auto label = QStringLiteral("%1  ·  %2")
                        .arg(room_reference_kind_label(reference.kind),
                             id_from(reference.id));
                    source->addItem(label, id_from(reference.id));
                    target->addItem(label, id_from(reference.id));
                }
                const auto has_two = source->count() >= 2;
                source->setEnabled(has_two);
                target->setEnabled(has_two);
                add->setEnabled(has_two);
                if (!has_two) {
                    status->setText(QStringLiteral(
                        "Create at least two references before adding a relationship."));
                }
            };
            const auto populate = [&] {
                const auto snapshot = authoringSnapshot();
                record = decode_room_relationships(snapshot);
                if (!record) return;
                record_revision = snapshot.revision();
                populate_references();
                relations->clear();
                for (std::size_t index = 0; index < record->model.relations().size(); ++index) {
                    const auto& relation = record->model.relations()[index];
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  %2  %3")
                            .arg(id_from(relation.source_id),
                                 room_relation_kind_label(relation.kind),
                                 id_from(relation.target_id)),
                        relations);
                    item->setData(Qt::UserRole, static_cast<int>(index));
                }
                status->setText(QStringLiteral("%1 references · %2 relationship%3")
                    .arg(record->model.references().size())
                    .arg(record->model.relations().size())
                    .arg(record->model.relations().size() == 1 ? QString{} : QStringLiteral("s")));
            };
            populate();

            QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
                try {
                    if (!record) throw std::invalid_argument("The relationship record is unavailable.");
                    if (authoringSnapshot().revision() != record_revision) {
                        populate();
                        throw std::invalid_argument(
                            "The project changed while the relationship editor was open. Review the refreshed list.");
                    }
                    const auto source_id = source->currentData().toString().toStdString();
                    const auto target_id = target->currentData().toString().toStdString();
                    if (source_id.empty() || target_id.empty())
                        throw std::invalid_argument("Choose a source and target.");
                    const auto relation_kind = static_cast<RoomRelationKind>(
                        kind->currentData().toInt());
                    auto updated_relations = record->model.relations();
                    updated_relations.push_back({source_id, target_id, relation_kind});
                    const auto updated = RoomRelationshipSnapshot::create(
                        record->model.references(), std::move(updated_relations));
                    if (applyRoomRelationshipModel(updated, QStringLiteral("Add room relationship"))) {
                        populate();
                        status->setText(QStringLiteral("Relationship added through document history."));
                    } else {
                        status->setText(lastError());
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });
            QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto* item = relations->currentItem();
                    if (!item || !record) throw std::invalid_argument("Choose a relationship to remove.");
                    if (authoringSnapshot().revision() != record_revision) {
                        populate();
                        throw std::invalid_argument(
                            "The project changed while the relationship editor was open. Review the refreshed list.");
                    }
                    const auto index = item->data(Qt::UserRole).toInt();
                    if (index < 0 || index >= static_cast<int>(record->model.relations().size()))
                        throw std::invalid_argument("The relationship list is stale. Refresh it and try again.");
                    auto updated_relations = record->model.relations();
                    updated_relations.erase(updated_relations.begin() + index);
                    const auto updated = RoomRelationshipSnapshot::create(
                        record->model.references(), std::move(updated_relations));
                    if (applyRoomRelationshipModel(updated, QStringLiteral("Remove room relationship"))) {
                        populate();
                        status->setText(QStringLiteral("Relationship removed through document history."));
                    } else {
                        status->setText(lastError());
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });
            QObject::connect(sync, &QPushButton::clicked, &dialog, [&] {
                try {
                    if (!record) throw std::invalid_argument("The relationship record is unavailable.");
                    if (authoringSnapshot().revision() != record_revision) {
                        populate();
                        throw std::invalid_argument(
                            "The project changed while the relationship editor was open. Review the refreshed list.");
                    }
                    const auto references = document_room_references(authoringSnapshot());
                    std::set<std::string, std::less<>> ids;
                    for (const auto& reference : references) ids.insert(reference.id);
                    auto relations_copy = record->model.relations();
                    relations_copy.erase(std::remove_if(relations_copy.begin(), relations_copy.end(),
                        [&](const auto& relation) {
                            return !ids.contains(relation.source_id) || !ids.contains(relation.target_id);
                        }), relations_copy.end());
                    const auto updated = RoomRelationshipSnapshot::create(
                        references, std::move(relations_copy));
                    if (applyRoomRelationshipModel(updated, QStringLiteral("Sync room references"))) {
                        populate();
                        status->setText(QStringLiteral("References synchronized; existing relations retained."));
                    } else {
                        status->setText(lastError());
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });
            QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
            dialog.exec();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Room relationships: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    bool ensureAssemblyModelRecord() {
        try {
            const auto source = authoringSnapshot();
            if (decode_assembly_model(source).has_value()) return true;
            const auto model = AssemblyModel::create({}, {}, {});
            auto entity = Entity::create("assembly_model", {{"model", model.to_json()}});
            entity.id = new_id("assemblies");
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                "Create assembly catalog"};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assemblies: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool applyAssemblyModel(const AssemblyModel& model, const QString& message) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto source = authoringSnapshot();
            const auto record = decode_assembly_model(source);
            if (!record) {
                throw std::invalid_argument("The assembly catalog is unavailable.");
            }
            auto entity = source.entities().at(record->entity_id);
            entity.properties["model"] = model.to_json();
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                message.toStdString()};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assemblies: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool ensureVerticalLevelRecord() {
        try {
            const auto source = authoringSnapshot();
            if (decode_vertical_levels(source).has_value()) return true;
            const auto model = VerticalLevelGraph{};
            auto entity = Entity::create("vertical_levels", {
                {"model", json::parse(model.serialize())}});
            entity.id = new_id("vertical-levels");
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                "Create vertical level graph"};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Levels: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool applyVerticalLevelGraph(const VerticalLevelGraph& model, const QString& message) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto source = authoringSnapshot();
            const auto record = decode_vertical_levels(source);
            if (!record) throw std::invalid_argument("The vertical level graph is unavailable.");
            auto entity = source.entities().at(record->entity_id);
            entity.properties["model"] = json::parse(model.serialize());
            const ApplyEntityChanges command{
                source.revision(), {EntityChange::upsert(std::move(entity))}, {},
                message.toStdString()};
            (void)Document::preview_command(source, Command{command});
            applyDocumentCommand(Command{command});
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Levels: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showVerticalLevels() {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        if (!ensureVerticalLevelRecord()) return;
        try {
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setObjectName(QStringLiteral("verticalLevelsDialog"));
            dialog.setWindowTitle(QStringLiteral("Levels and floor-to-floor links"));
            dialog.setModal(true);
            dialog.resize(820, 570);

            auto* layout = new QVBoxLayout(&dialog);
            auto* columns = new QHBoxLayout;

            auto* level_panel = new QVBoxLayout;
            level_panel->addWidget(new QLabel(QStringLiteral("Levels (metres)"), &dialog));
            auto* levels = new QListWidget(&dialog);
            levels->setObjectName(QStringLiteral("verticalLevelList"));
            levels->setSelectionMode(QAbstractItemView::SingleSelection);
            level_panel->addWidget(levels, 1);
            auto* level_form = new QFormLayout;
            auto* level_id = new QLineEdit(&dialog);
            level_id->setObjectName(QStringLiteral("verticalLevelId"));
            level_id->setPlaceholderText(QStringLiteral("Stable level ID"));
            auto* level_elevation = new QLineEdit(&dialog);
            level_elevation->setObjectName(QStringLiteral("verticalLevelElevation"));
            level_elevation->setPlaceholderText(QStringLiteral("Elevation in metres"));
            level_form->addRow(QStringLiteral("ID"), level_id);
            level_form->addRow(QStringLiteral("Elevation"), level_elevation);
            level_panel->addLayout(level_form);
            auto* level_buttons = new QHBoxLayout;
            auto* save_level = new QPushButton(QStringLiteral("Save level"), &dialog);
            save_level->setObjectName(QStringLiteral("saveVerticalLevel"));
            auto* remove_level = new QPushButton(QStringLiteral("Remove level"), &dialog);
            remove_level->setObjectName(QStringLiteral("removeVerticalLevel"));
            level_buttons->addWidget(save_level);
            level_buttons->addWidget(remove_level);
            level_buttons->addStretch(1);
            level_panel->addLayout(level_buttons);

            auto* link_panel = new QVBoxLayout;
            link_panel->addWidget(new QLabel(QStringLiteral("Floor-to-floor links"), &dialog));
            auto* links = new QListWidget(&dialog);
            links->setObjectName(QStringLiteral("verticalLinkList"));
            links->setSelectionMode(QAbstractItemView::SingleSelection);
            link_panel->addWidget(links, 1);
            auto* link_form = new QFormLayout;
            auto* link_id = new QLineEdit(&dialog);
            link_id->setObjectName(QStringLiteral("verticalLinkId"));
            link_id->setPlaceholderText(QStringLiteral("Stable link ID"));
            auto* link_lower = new QComboBox(&dialog);
            link_lower->setObjectName(QStringLiteral("verticalLinkLower"));
            auto* link_upper = new QComboBox(&dialog);
            link_upper->setObjectName(QStringLiteral("verticalLinkUpper"));
            link_form->addRow(QStringLiteral("ID"), link_id);
            link_form->addRow(QStringLiteral("Lower"), link_lower);
            link_form->addRow(QStringLiteral("Upper"), link_upper);
            link_panel->addLayout(link_form);
            auto* link_buttons = new QHBoxLayout;
            auto* save_link = new QPushButton(QStringLiteral("Add link"), &dialog);
            save_link->setObjectName(QStringLiteral("saveVerticalLink"));
            auto* remove_link = new QPushButton(QStringLiteral("Remove link"), &dialog);
            remove_link->setObjectName(QStringLiteral("removeVerticalLink"));
            auto* freeze_link = new QPushButton(QStringLiteral("Freeze"), &dialog);
            freeze_link->setObjectName(QStringLiteral("freezeVerticalLink"));
            auto* disconnect_link = new QPushButton(QStringLiteral("Disconnect"), &dialog);
            disconnect_link->setObjectName(QStringLiteral("disconnectVerticalLink"));
            link_buttons->addWidget(save_link);
            link_buttons->addWidget(remove_link);
            link_buttons->addWidget(freeze_link);
            link_buttons->addWidget(disconnect_link);
            link_panel->addLayout(link_buttons);

            columns->addLayout(level_panel, 1);
            columns->addLayout(link_panel, 1);
            layout->addLayout(columns, 1);
            auto* status = new QLabel(&dialog);
            status->setObjectName(QStringLiteral("verticalLevelsStatus"));
            status->setWordWrap(true);
            status->setTextFormat(Qt::PlainText);
            layout->addWidget(status);
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            close->setObjectName(QStringLiteral("closeVerticalLevels"));
            close->setDefault(true);
            auto* footer = new QHBoxLayout;
            footer->addStretch(1);
            footer->addWidget(close);
            layout->addLayout(footer);

            std::optional<VerticalLevelRecord> record;
            Revision record_revision{};
            const auto populate = [&] {
                const auto snapshot = authoringSnapshot();
                record = decode_vertical_levels(snapshot);
                if (!record) return;
                record_revision = snapshot.revision();
                const auto selected_level_id = levels->currentItem()
                    ? levels->currentItem()->data(Qt::UserRole).toString() : level_id->text().trimmed();
                const auto selected_link_id = links->currentItem()
                    ? links->currentItem()->data(Qt::UserRole).toString() : link_id->text().trimmed();
                const QSignalBlocker level_blocker(levels);
                const QSignalBlocker link_blocker(links);
                const QSignalBlocker lower_blocker(link_lower);
                const QSignalBlocker upper_blocker(link_upper);
                levels->clear();
                links->clear();
                link_lower->clear();
                link_upper->clear();
                for (const auto& level : record->model.levels()) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2 m")
                            .arg(QString::fromStdString(level.id))
                            .arg(level.elevation_m, 0, 'f', 3), levels);
                    item->setData(Qt::UserRole, QString::fromStdString(level.id));
                    link_lower->addItem(QString::fromStdString(level.id),
                                        QString::fromStdString(level.id));
                    link_upper->addItem(QString::fromStdString(level.id),
                                        QString::fromStdString(level.id));
                }
                for (const auto& link : record->model.links()) {
                    const auto state = link.state == RelationshipState::connected
                        ? QStringLiteral("connected")
                        : link.state == RelationshipState::frozen
                            ? QStringLiteral("frozen") : QStringLiteral("disconnected");
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2 → %3  ·  %4 m  ·  %5")
                            .arg(QString::fromStdString(link.id),
                                 QString::fromStdString(link.lower_level_id),
                                 QString::fromStdString(link.upper_level_id))
                            .arg(record->model.floor_to_floor_height(link.id), 0, 'f', 3)
                            .arg(state), links);
                    item->setData(Qt::UserRole, QString::fromStdString(link.id));
                }
                QListWidgetItem* selected_level = nullptr;
                for (int index = 0; index < levels->count(); ++index)
                    if (levels->item(index)->data(Qt::UserRole).toString() == selected_level_id)
                        selected_level = levels->item(index);
                if (!selected_level && levels->count() > 0) selected_level = levels->item(0);
                levels->setCurrentItem(selected_level);
                QListWidgetItem* selected_link = nullptr;
                for (int index = 0; index < links->count(); ++index)
                    if (links->item(index)->data(Qt::UserRole).toString() == selected_link_id)
                        selected_link = links->item(index);
                if (!selected_link && links->count() > 0) selected_link = links->item(0);
                links->setCurrentItem(selected_link);
                level_id->clear();
                level_elevation->clear();
                if (selected_level) {
                    const auto id = selected_level->data(Qt::UserRole).toString().toStdString();
                    const auto found = std::find_if(record->model.levels().begin(), record->model.levels().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    if (found != record->model.levels().end()) {
                        level_id->setText(QString::fromStdString(found->id));
                        level_elevation->setText(QString::number(found->elevation_m, 'g', 15));
                    }
                }
                link_id->clear();
                if (selected_link) {
                    const auto id = selected_link->data(Qt::UserRole).toString().toStdString();
                    const auto found = std::find_if(record->model.links().begin(), record->model.links().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    if (found != record->model.links().end()) {
                        link_id->setText(QString::fromStdString(found->id));
                        link_lower->setCurrentIndex(link_lower->findData(QString::fromStdString(found->lower_level_id)));
                        link_upper->setCurrentIndex(link_upper->findData(QString::fromStdString(found->upper_level_id)));
                    }
                }
                status->setText(QStringLiteral("%1 level%2 · %3 link%4")
                    .arg(record->model.levels().size())
                    .arg(record->model.levels().size() == 1 ? QString{} : QStringLiteral("s"))
                    .arg(record->model.links().size())
                    .arg(record->model.links().size() == 1 ? QString{} : QStringLiteral("s")));
                const bool has_level = !record->model.levels().empty();
                const bool has_link = !record->model.links().empty();
                remove_level->setEnabled(has_level);
                save_link->setEnabled(record->model.levels().size() >= 2);
                remove_link->setEnabled(has_link);
                freeze_link->setEnabled(has_link);
                disconnect_link->setEnabled(has_link);
            };
            populate();

            const auto current_record = [&]() -> std::optional<VerticalLevelRecord> {
                if (!record) return std::nullopt;
                if (authoringSnapshot().revision() != record_revision) {
                    populate();
                    status->setText(QStringLiteral(
                        "The project changed while the Levels editor was open. Review the refreshed graph."));
                    return std::nullopt;
                }
                return record;
            };

            QObject::connect(levels, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem*, QListWidgetItem*) { populate(); });
            QObject::connect(links, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem*, QListWidgetItem*) { populate(); });

            QObject::connect(save_level, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = level_id->text().trimmed().toStdString();
                    if (id.empty()) throw std::invalid_argument("Enter a level ID.");
                    bool ok = false;
                    const auto elevation = level_elevation->text().trimmed().toDouble(&ok);
                    if (!ok || !std::isfinite(elevation))
                        throw std::invalid_argument("Elevation must be a finite number of metres.");
                    const auto found = std::find_if(current->model.levels().begin(), current->model.levels().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    VerticalLevelGraph updated;
                    if (found == current->model.levels().end()) {
                        updated = current->model.create_level({id, elevation});
                    } else {
                        updated = current->model.with_elevation(id, elevation);
                    }
                    if (applyVerticalLevelGraph(updated, QStringLiteral("Save vertical level"))) {
                        populate();
                        status->setText(QStringLiteral("Level saved through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_level, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = levels->currentItem();
                    if (!item) throw std::invalid_argument("Choose a level first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    if (std::any_of(current->model.links().begin(), current->model.links().end(),
                        [&](const auto& link) { return link.lower_level_id == id || link.upper_level_id == id; }))
                        throw std::invalid_argument("Remove the level's floor-to-floor links first.");
                    auto values = current->model.levels();
                    values.erase(std::remove_if(values.begin(), values.end(),
                        [&](const auto& level) { return level.id == id; }), values.end());
                    const auto updated = VerticalLevelGraph(std::move(values), current->model.links());
                    if (applyVerticalLevelGraph(updated, QStringLiteral("Remove vertical level"))) {
                        populate();
                        status->setText(QStringLiteral("Level removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(save_link, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = link_id->text().trimmed().toStdString();
                    const auto lower = link_lower->currentData().toString().toStdString();
                    const auto upper = link_upper->currentData().toString().toStdString();
                    if (id.empty() || lower.empty() || upper.empty())
                        throw std::invalid_argument("Enter a link ID and choose lower and upper levels.");
                    const auto updated = current->model.create_link({id, lower, upper});
                    if (applyVerticalLevelGraph(updated, QStringLiteral("Add vertical link"))) {
                        populate();
                        status->setText(QStringLiteral("Floor-to-floor link added through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            const auto transition_link = [&](bool freeze) {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = links->currentItem();
                    if (!item) throw std::invalid_argument("Choose a floor-to-floor link first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    const auto updated = freeze ? current->model.freeze(id) : current->model.disconnect(id);
                    if (applyVerticalLevelGraph(updated,
                            freeze ? QStringLiteral("Freeze vertical link")
                                   : QStringLiteral("Disconnect vertical link"))) {
                        populate();
                        status->setText(freeze
                            ? QStringLiteral("Link frozen; its height is now retained.")
                            : QStringLiteral("Link disconnected; its retained height is preserved."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            };
            QObject::connect(freeze_link, &QPushButton::clicked, &dialog,
                             [&] { transition_link(true); });
            QObject::connect(disconnect_link, &QPushButton::clicked, &dialog,
                             [&] { transition_link(false); });

            QObject::connect(remove_link, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = links->currentItem();
                    if (!item) throw std::invalid_argument("Choose a floor-to-floor link first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    auto values = current->model.links();
                    values.erase(std::remove_if(values.begin(), values.end(),
                        [&](const auto& link) { return link.id == id; }), values.end());
                    const auto updated = VerticalLevelGraph(current->model.levels(), std::move(values));
                    if (applyVerticalLevelGraph(updated, QStringLiteral("Remove vertical link"))) {
                        populate();
                        status->setText(QStringLiteral("Link removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });
            QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
            dialog.exec();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Levels: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void showAssemblies() {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        if (!ensureAssemblyModelRecord()) return;
        try {
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setObjectName(QStringLiteral("assemblyCatalogDialog"));
            dialog.setWindowTitle(QStringLiteral("Assembly catalog"));
            dialog.setModal(true);
            dialog.resize(900, 650);

            auto* layout = new QVBoxLayout(&dialog);
            auto* columns = new QHBoxLayout;

            auto* type_panel = new QVBoxLayout;
            type_panel->addWidget(new QLabel(QStringLiteral("Reusable types"), &dialog));
            auto* types = new QListWidget(&dialog);
            types->setObjectName(QStringLiteral("assemblyTypeList"));
            types->setSelectionMode(QAbstractItemView::SingleSelection);
            type_panel->addWidget(types, 1);
            auto* type_id = new QLineEdit(&dialog);
            type_id->setObjectName(QStringLiteral("assemblyTypeId"));
            type_id->setPlaceholderText(QStringLiteral("Type ID"));
            auto* type_name = new QLineEdit(&dialog);
            type_name->setObjectName(QStringLiteral("assemblyTypeName"));
            type_name->setPlaceholderText(QStringLiteral("Type name"));
            type_panel->addWidget(type_id);
            type_panel->addWidget(type_name);
            auto* type_buttons = new QHBoxLayout;
            auto* add_type = new QPushButton(QStringLiteral("Add type"), &dialog);
            add_type->setObjectName(QStringLiteral("addAssemblyType"));
            auto* rename_type = new QPushButton(QStringLiteral("Rename"), &dialog);
            rename_type->setObjectName(QStringLiteral("renameAssemblyType"));
            auto* remove_type = new QPushButton(QStringLiteral("Remove"), &dialog);
            remove_type->setObjectName(QStringLiteral("removeAssemblyType"));
            type_buttons->addWidget(add_type);
            type_buttons->addWidget(rename_type);
            type_buttons->addWidget(remove_type);
            type_panel->addLayout(type_buttons);

            auto* instance_panel = new QVBoxLayout;
            instance_panel->addWidget(new QLabel(QStringLiteral("Placed instances"), &dialog));
            auto* instances = new QListWidget(&dialog);
            instances->setObjectName(QStringLiteral("assemblyInstanceList"));
            instances->setSelectionMode(QAbstractItemView::SingleSelection);
            instance_panel->addWidget(instances, 1);
            auto* instance_id = new QLineEdit(&dialog);
            instance_id->setObjectName(QStringLiteral("assemblyInstanceId"));
            instance_id->setPlaceholderText(QStringLiteral("Instance ID (optional)"));
            instance_panel->addWidget(instance_id);
            auto* instance_type = new QComboBox(&dialog);
            instance_type->setObjectName(QStringLiteral("assemblyInstanceType"));
            instance_panel->addWidget(instance_type);
            auto* instance_buttons = new QHBoxLayout;
            auto* add_instance = new QPushButton(QStringLiteral("Add instance"), &dialog);
            add_instance->setObjectName(QStringLiteral("addAssemblyInstance"));
            auto* remove_instance = new QPushButton(QStringLiteral("Remove"), &dialog);
            remove_instance->setObjectName(QStringLiteral("removeAssemblyInstance"));
            instance_buttons->addWidget(add_instance);
            instance_buttons->addWidget(remove_instance);
            instance_panel->addLayout(instance_buttons);

            columns->addLayout(type_panel, 1);
            columns->addLayout(instance_panel, 1);
            layout->addLayout(columns, 1);

            auto* data_tabs = new QTabWidget(&dialog);
            data_tabs->setObjectName(QStringLiteral("assemblyDataTabs"));
            data_tabs->setDocumentMode(true);

            auto* type_data_page = new QWidget(data_tabs);
            auto* type_data_layout = new QVBoxLayout(type_data_page);
            auto* type_schema = new QListWidget(type_data_page);
            type_schema->setObjectName(QStringLiteral("assemblyTypeSchemaList"));
            type_schema->setSelectionMode(QAbstractItemView::SingleSelection);
            type_schema->setToolTip(QStringLiteral(
                "Properties, material slots, and quantities declared by the selected reusable type."));
            type_data_layout->addWidget(type_schema, 1);
            auto* type_entry_form = new QFormLayout;
            auto* type_entry_kind = new QComboBox(type_data_page);
            type_entry_kind->setObjectName(QStringLiteral("assemblyTypeEntryKind"));
            type_entry_kind->addItem(QStringLiteral("Property"), QStringLiteral("property"));
            type_entry_kind->addItem(QStringLiteral("Material slot"), QStringLiteral("material"));
            type_entry_kind->addItem(QStringLiteral("Quantity"), QStringLiteral("quantity"));
            auto* type_entry_key = new QLineEdit(type_data_page);
            type_entry_key->setObjectName(QStringLiteral("assemblyTypeEntryKey"));
            type_entry_key->setPlaceholderText(QStringLiteral("Key or slot"));
            auto* type_entry_value = new QLineEdit(type_data_page);
            type_entry_value->setObjectName(QStringLiteral("assemblyTypeEntryValue"));
            type_entry_value->setPlaceholderText(QStringLiteral("Default value or material ID"));
            auto* type_entry_unit = new QComboBox(type_data_page);
            type_entry_unit->setObjectName(QStringLiteral("assemblyTypeEntryUnit"));
            for (const auto& unit : {AssemblyQuantityUnit::count, AssemblyQuantityUnit::metre,
                                     AssemblyQuantityUnit::square_metre, AssemblyQuantityUnit::cubic_metre,
                                     AssemblyQuantityUnit::kilogram}) {
                type_entry_unit->addItem(assembly_quantity_unit_label(unit),
                                         assembly_quantity_unit_label(unit));
            }
            type_entry_form->addRow(QStringLiteral("Kind"), type_entry_kind);
            type_entry_form->addRow(QStringLiteral("Key"), type_entry_key);
            type_entry_form->addRow(QStringLiteral("Value"), type_entry_value);
            type_entry_form->addRow(QStringLiteral("Unit"), type_entry_unit);
            type_data_layout->addLayout(type_entry_form);
            auto* type_entry_buttons = new QHBoxLayout;
            auto* save_type_entry = new QPushButton(QStringLiteral("Save entry"), type_data_page);
            save_type_entry->setObjectName(QStringLiteral("saveAssemblyTypeEntry"));
            auto* remove_type_entry = new QPushButton(QStringLiteral("Remove entry"), type_data_page);
            remove_type_entry->setObjectName(QStringLiteral("removeAssemblyTypeEntry"));
            type_entry_buttons->addWidget(save_type_entry);
            type_entry_buttons->addWidget(remove_type_entry);
            type_entry_buttons->addStretch(1);
            type_data_layout->addLayout(type_entry_buttons);
            data_tabs->addTab(type_data_page, QStringLiteral("Type schema"));

            auto* material_page = new QWidget(data_tabs);
            auto* material_layout = new QVBoxLayout(material_page);
            auto* materials = new QListWidget(material_page);
            materials->setObjectName(QStringLiteral("assemblyMaterialList"));
            materials->setSelectionMode(QAbstractItemView::SingleSelection);
            material_layout->addWidget(materials, 1);
            auto* material_form = new QFormLayout;
            auto* material_id = new QLineEdit(material_page);
            material_id->setObjectName(QStringLiteral("assemblyMaterialId"));
            material_id->setPlaceholderText(QStringLiteral("Material ID"));
            auto* material_name = new QLineEdit(material_page);
            material_name->setObjectName(QStringLiteral("assemblyMaterialName"));
            material_name->setPlaceholderText(QStringLiteral("Material name"));
            material_form->addRow(QStringLiteral("ID"), material_id);
            material_form->addRow(QStringLiteral("Name"), material_name);
            auto* material_color = new QLineEdit(material_page);
            material_color->setObjectName(QStringLiteral("assemblyMaterialColor"));
            material_color->setPlaceholderText(QStringLiteral("Default"));
            material_color->setToolTip(QStringLiteral("sRGB color (#RRGGBB). Clear to use the default appearance."));
            auto* color_row = new QHBoxLayout;
            color_row->addWidget(material_color, 1);
            auto* choose_color = new QPushButton(QStringLiteral("Choose…"), material_page);
            choose_color->setAccessibleName(QStringLiteral("Choose material color"));
            QObject::connect(material_color, &QLineEdit::textChanged, &dialog, [choose_color](const QString& text) {
                const QColor color(text);
                QPixmap swatch(14, 14);
                swatch.fill(color.isValid() ? color : Qt::transparent);
                choose_color->setIcon(QIcon(swatch));
            });
            color_row->addWidget(choose_color);
            material_form->addRow(QStringLiteral("Color"), color_row);
            QObject::connect(choose_color, &QPushButton::clicked, &dialog, [&] {
                const QColor initial(material_color->text());
                const auto color = QColorDialog::getColor(initial.isValid() ? initial : QColor(Qt::gray),
                    &dialog, QStringLiteral("Material color"));
                if (color.isValid()) material_color->setText(color.name(QColor::HexRgb));
            });
            material_layout->addLayout(material_form);
            auto* material_buttons = new QHBoxLayout;
            auto* add_material = new QPushButton(QStringLiteral("Save material"), material_page);
            add_material->setObjectName(QStringLiteral("saveAssemblyMaterial"));
            auto* remove_material = new QPushButton(QStringLiteral("Remove material"), material_page);
            remove_material->setObjectName(QStringLiteral("removeAssemblyMaterial"));
            material_buttons->addWidget(add_material);
            material_buttons->addWidget(remove_material);
            material_buttons->addStretch(1);
            material_layout->addLayout(material_buttons);
            data_tabs->addTab(material_page, QStringLiteral("Materials"));

            auto* override_page = new QWidget(data_tabs);
            auto* override_layout = new QVBoxLayout(override_page);
            auto* overrides = new QListWidget(override_page);
            overrides->setObjectName(QStringLiteral("assemblyInstanceOverrideList"));
            overrides->setSelectionMode(QAbstractItemView::SingleSelection);
            override_layout->addWidget(overrides, 1);
            auto* override_form = new QFormLayout;
            auto* override_kind = new QComboBox(override_page);
            override_kind->setObjectName(QStringLiteral("assemblyInstanceOverrideKind"));
            override_kind->addItem(QStringLiteral("Property"), QStringLiteral("property"));
            override_kind->addItem(QStringLiteral("Material slot"), QStringLiteral("material"));
            override_kind->addItem(QStringLiteral("Quantity"), QStringLiteral("quantity"));
            auto* override_key = new QLineEdit(override_page);
            override_key->setObjectName(QStringLiteral("assemblyInstanceOverrideKey"));
            override_key->setPlaceholderText(QStringLiteral("Declared key or slot"));
            auto* override_value = new QLineEdit(override_page);
            override_value->setObjectName(QStringLiteral("assemblyInstanceOverrideValue"));
            override_value->setPlaceholderText(QStringLiteral("Override value or material ID"));
            auto* override_unit = new QComboBox(override_page);
            override_unit->setObjectName(QStringLiteral("assemblyInstanceOverrideUnit"));
            for (const auto& unit : {AssemblyQuantityUnit::count, AssemblyQuantityUnit::metre,
                                     AssemblyQuantityUnit::square_metre, AssemblyQuantityUnit::cubic_metre,
                                     AssemblyQuantityUnit::kilogram}) {
                override_unit->addItem(assembly_quantity_unit_label(unit),
                                       assembly_quantity_unit_label(unit));
            }
            override_form->addRow(QStringLiteral("Kind"), override_kind);
            override_form->addRow(QStringLiteral("Key"), override_key);
            override_form->addRow(QStringLiteral("Value"), override_value);
            override_form->addRow(QStringLiteral("Unit"), override_unit);
            override_layout->addLayout(override_form);
            auto* override_buttons = new QHBoxLayout;
            auto* save_override = new QPushButton(QStringLiteral("Save override"), override_page);
            save_override->setObjectName(QStringLiteral("saveAssemblyInstanceOverride"));
            auto* remove_override = new QPushButton(QStringLiteral("Remove override"), override_page);
            remove_override->setObjectName(QStringLiteral("removeAssemblyInstanceOverride"));
            override_buttons->addWidget(save_override);
            override_buttons->addWidget(remove_override);
            override_buttons->addStretch(1);
            override_layout->addLayout(override_buttons);
            auto* override_note = new QLabel(QStringLiteral(
                "Overrides are explicit instance data. Quantity units must match the type declaration."),
                override_page);
            override_note->setWordWrap(true);
            override_layout->addWidget(override_note);
            data_tabs->addTab(override_page, QStringLiteral("Instance overrides"));
            layout->addWidget(data_tabs, 1);

            auto* status = new QLabel(&dialog);
            status->setObjectName(QStringLiteral("assemblyCatalogStatus"));
            status->setWordWrap(true);
            status->setTextFormat(Qt::PlainText);
            layout->addWidget(status);
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            close->setObjectName(QStringLiteral("closeAssemblyCatalog"));
            close->setDefault(true);
            auto* footer = new QHBoxLayout;
            footer->addStretch(1);
            footer->addWidget(close);
            layout->addLayout(footer);

            std::optional<AssemblyModelRecord> record;
            Revision record_revision{};
            const auto refresh_materials = [&] {
                const auto selected_id = materials->currentItem()
                    ? materials->currentItem()->data(Qt::UserRole).toString() : QString{};
                const QSignalBlocker blocker(materials);
                materials->clear();
                for (const auto& material : record->model.materials()) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2")
                            .arg(QString::fromStdString(material.name),
                                 QString::fromStdString(material.id)), materials);
                    item->setData(Qt::UserRole, QString::fromStdString(material.id));
                }
                QListWidgetItem* selected = nullptr;
                for (int index = 0; index < materials->count(); ++index) {
                    if (materials->item(index)->data(Qt::UserRole).toString() == selected_id) {
                        selected = materials->item(index);
                        break;
                    }
                }
                if (!selected && materials->count() > 0) selected = materials->item(0);
                materials->setCurrentItem(selected);
                material_id->clear();
                material_name->clear();
                material_color->clear();
                if (selected) {
                    const auto id = selected->data(Qt::UserRole).toString().toStdString();
                    const auto found = std::find_if(record->model.materials().begin(),
                                                    record->model.materials().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    if (found != record->model.materials().end()) {
                        material_id->setText(QString::fromStdString(found->id));
                        material_name->setText(QString::fromStdString(found->name));
                        material_color->setText(QString::fromStdString(found->color_srgb.value_or("")));
                    }
                }
            };
            const auto refresh_type_editor = [&] {
                const QSignalBlocker schema_blocker(type_schema);
                const QSignalBlocker kind_blocker(type_entry_kind);
                const QSignalBlocker unit_blocker(type_entry_unit);
                type_schema->clear();
                type_entry_key->clear();
                type_entry_value->clear();
                type_entry_unit->setCurrentIndex(0);
                if (!record) return;
                const auto* selected = types->currentItem();
                if (!selected) return;
                const auto id = selected->data(Qt::UserRole).toString().toStdString();
                const auto found = std::find_if(record->model.types().begin(),
                                                record->model.types().end(),
                    [&](const auto& candidate) { return candidate.id == id; });
                if (found == record->model.types().end()) return;
                type_id->setText(QString::fromStdString(found->id));
                type_name->setText(QString::fromStdString(found->name));
                const auto add_entry = [&](const QString& kind, const QString& key,
                                           const QString& value, const QString& unit = QString{}) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2 = %3")
                            .arg(kind, key, value), type_schema);
                    item->setData(Qt::UserRole, kind);
                    item->setData(Qt::UserRole + 1, key);
                    item->setData(Qt::UserRole + 2, value);
                    item->setData(Qt::UserRole + 3, unit);
                };
                for (const auto& [key, value] : found->properties)
                    add_entry(QStringLiteral("Property"), QString::fromStdString(key),
                              QString::fromStdString(value));
                for (const auto& [key, value] : found->materials)
                    add_entry(QStringLiteral("Material slot"), QString::fromStdString(key),
                              QString::fromStdString(value));
                for (const auto& [key, value] : found->quantities) {
                    const auto unit = assembly_quantity_unit_label(value.unit);
                    add_entry(QStringLiteral("Quantity"), QString::fromStdString(key),
                              assembly_quantity_text(value), unit);
                }
            };
            const auto refresh_instance_editor = [&] {
                const QSignalBlocker blocker(overrides);
                const QSignalBlocker kind_blocker(override_kind);
                const QSignalBlocker unit_blocker(override_unit);
                overrides->clear();
                override_key->clear();
                override_value->clear();
                override_unit->setCurrentIndex(0);
                if (!record) return;
                const auto* selected = instances->currentItem();
                if (!selected) return;
                const auto id = selected->data(Qt::UserRole).toString().toStdString();
                const auto found = std::find_if(record->model.instances().begin(),
                                                record->model.instances().end(),
                    [&](const auto& candidate) { return candidate.id == id; });
                if (found == record->model.instances().end()) return;
                instance_id->setText(QString::fromStdString(found->id));
                const auto type = std::find_if(record->model.types().begin(),
                                               record->model.types().end(),
                    [&](const auto& candidate) { return candidate.id == found->type_id; });
                const auto add_entry = [&](const QString& kind, const QString& key,
                                           const QString& value, const QString& unit = QString{}) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2 = %3")
                            .arg(kind, key, value), overrides);
                    item->setData(Qt::UserRole, kind);
                    item->setData(Qt::UserRole + 1, key);
                    item->setData(Qt::UserRole + 2, value);
                    item->setData(Qt::UserRole + 3, unit);
                };
                for (const auto& [key, value] : found->property_overrides)
                    add_entry(QStringLiteral("Property"), QString::fromStdString(key),
                              QString::fromStdString(value));
                for (const auto& [key, value] : found->material_overrides)
                    add_entry(QStringLiteral("Material slot"), QString::fromStdString(key),
                              QString::fromStdString(value));
                for (const auto& [key, value] : found->quantity_overrides)
                    add_entry(QStringLiteral("Quantity"), QString::fromStdString(key),
                              assembly_quantity_text(value), assembly_quantity_unit_label(value.unit));
                if (type != record->model.types().end()) {
                    for (int index = 0; index < override_unit->count(); ++index) {
                        if (override_unit->itemData(index).toString() == QStringLiteral("count")) {
                            override_unit->setCurrentIndex(index);
                            break;
                        }
                    }
                }
            };
            const auto populate = [&] {
                const auto snapshot = authoringSnapshot();
                record = decode_assembly_model(snapshot);
                if (!record) return;
                record_revision = snapshot.revision();
                const auto selected_type_id = types->currentItem()
                    ? types->currentItem()->data(Qt::UserRole).toString()
                    : type_id->text().trimmed();
                const auto selected_instance_id = instances->currentItem()
                    ? instances->currentItem()->data(Qt::UserRole).toString()
                    : instance_id->text().trimmed();
                const QSignalBlocker type_blocker(types);
                const QSignalBlocker instance_blocker(instances);
                const QSignalBlocker combo_blocker(instance_type);
                types->clear();
                instance_type->clear();
                for (const auto& type : record->model.types()) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2")
                            .arg(QString::fromStdString(type.name),
                                 QString::fromStdString(type.id)),
                        types);
                    item->setData(Qt::UserRole, QString::fromStdString(type.id));
                    instance_type->addItem(QString::fromStdString(type.name),
                                           QString::fromStdString(type.id));
                }
                for (const auto& instance : record->model.instances()) {
                    const auto type = std::find_if(record->model.types().begin(),
                                                   record->model.types().end(),
                        [&](const auto& candidate) { return candidate.id == instance.type_id; });
                    const auto type_name_text = type == record->model.types().end()
                        ? instance.type_id : type->name;
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  %2")
                            .arg(QString::fromStdString(type_name_text),
                                 QString::fromStdString(instance.id)),
                        instances);
                    item->setData(Qt::UserRole, QString::fromStdString(instance.id));
                }
                QListWidgetItem* selected_type = nullptr;
                for (int index = 0; index < types->count(); ++index) {
                    if (types->item(index)->data(Qt::UserRole).toString() == selected_type_id) {
                        selected_type = types->item(index);
                        break;
                    }
                }
                if (!selected_type && types->count() > 0) selected_type = types->item(0);
                types->setCurrentItem(selected_type);
                QListWidgetItem* selected_instance = nullptr;
                for (int index = 0; index < instances->count(); ++index) {
                    if (instances->item(index)->data(Qt::UserRole).toString() == selected_instance_id) {
                        selected_instance = instances->item(index);
                        break;
                    }
                }
                if (!selected_instance && instances->count() > 0) selected_instance = instances->item(0);
                instances->setCurrentItem(selected_instance);
                refresh_materials();
                refresh_type_editor();
                refresh_instance_editor();
                status->setText(QStringLiteral("%1 reusable type%2 · %3 placed instance%4")
                    .arg(record->model.types().size())
                    .arg(record->model.types().size() == 1 ? QString{} : QStringLiteral("s"))
                    .arg(record->model.instances().size())
                    .arg(record->model.instances().size() == 1 ? QString{} : QStringLiteral("s")));
                const bool has_type = !record->model.types().empty();
                rename_type->setEnabled(has_type);
                remove_type->setEnabled(has_type);
                add_instance->setEnabled(has_type);
                remove_instance->setEnabled(!record->model.instances().empty());
                save_type_entry->setEnabled(has_type);
                remove_type_entry->setEnabled(has_type);
                save_override->setEnabled(!record->model.instances().empty());
                remove_override->setEnabled(!record->model.instances().empty());
                remove_material->setEnabled(!record->model.materials().empty());
            };
            populate();

            const auto entry_kind_id = [](const QString& label) -> QString {
                if (label == QStringLiteral("Property")) return QStringLiteral("property");
                if (label == QStringLiteral("Material slot")) return QStringLiteral("material");
                return QStringLiteral("quantity");
            };
            QObject::connect(types, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem* current, QListWidgetItem*) {
                                 if (!current || !record) return;
                                 refresh_type_editor();
                             });
            QObject::connect(instances, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem* current, QListWidgetItem*) {
                                 if (!current || !record) return;
                                 refresh_instance_editor();
                             });
            QObject::connect(type_schema, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem* current, QListWidgetItem*) {
                                 if (!current) return;
                                 const QSignalBlocker kind_blocker(type_entry_kind);
                                 const QSignalBlocker unit_blocker(type_entry_unit);
                                 const auto kind = entry_kind_id(current->data(Qt::UserRole).toString());
                                 const auto kind_index = type_entry_kind->findData(kind);
                                 if (kind_index >= 0) type_entry_kind->setCurrentIndex(kind_index);
                                 type_entry_key->setText(current->data(Qt::UserRole + 1).toString());
                                 const auto value = current->data(Qt::UserRole + 2).toString();
                                 type_entry_value->setText(kind == QStringLiteral("Quantity")
                                     ? value.section(u' ', 0, 0) : value);
                                 const auto unit = current->data(Qt::UserRole + 3).toString();
                                 if (!unit.isEmpty()) {
                                     const auto unit_index = type_entry_unit->findData(unit);
                                     if (unit_index >= 0) type_entry_unit->setCurrentIndex(unit_index);
                                 }
                             });
            QObject::connect(overrides, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem* current, QListWidgetItem*) {
                                 if (!current) return;
                                 const QSignalBlocker kind_blocker(override_kind);
                                 const QSignalBlocker unit_blocker(override_unit);
                                 const auto kind = entry_kind_id(current->data(Qt::UserRole).toString());
                                 const auto kind_index = override_kind->findData(kind);
                                 if (kind_index >= 0) override_kind->setCurrentIndex(kind_index);
                                 override_key->setText(current->data(Qt::UserRole + 1).toString());
                                 const auto value = current->data(Qt::UserRole + 2).toString();
                                 override_value->setText(kind == QStringLiteral("Quantity")
                                     ? value.section(u' ', 0, 0) : value);
                                 const auto unit = current->data(Qt::UserRole + 3).toString();
                                 if (!unit.isEmpty()) {
                                     const auto unit_index = override_unit->findData(unit);
                                     if (unit_index >= 0) override_unit->setCurrentIndex(unit_index);
                                 }
                             });
            QObject::connect(materials, &QListWidget::currentItemChanged, &dialog,
                             [&](QListWidgetItem* current, QListWidgetItem*) {
                                 if (!current || !record) return;
                                 const auto id = current->data(Qt::UserRole).toString().toStdString();
                                 const auto found = std::find_if(record->model.materials().begin(),
                                                                 record->model.materials().end(),
                                     [&](const auto& candidate) { return candidate.id == id; });
                                 if (found == record->model.materials().end()) return;
                                 material_id->setText(QString::fromStdString(found->id));
                                 material_name->setText(QString::fromStdString(found->name));
                                 material_color->setText(QString::fromStdString(found->color_srgb.value_or("")));
                             });
            const auto sync_type_unit = [&] {
                type_entry_unit->setEnabled(type_entry_kind->currentData().toString() == QStringLiteral("quantity"));
            };
            const auto sync_override_unit = [&] {
                override_unit->setEnabled(override_kind->currentData().toString() == QStringLiteral("quantity"));
            };
            QObject::connect(type_entry_kind, &QComboBox::currentIndexChanged, &dialog,
                             [&](int) { sync_type_unit(); });
            QObject::connect(override_kind, &QComboBox::currentIndexChanged, &dialog,
                             [&](int) { sync_override_unit(); });
            sync_type_unit();
            sync_override_unit();

            const auto current_record = [&]() -> std::optional<AssemblyModelRecord> {
                if (!record) return std::nullopt;
                if (authoringSnapshot().revision() != record_revision) {
                    populate();
                    status->setText(QStringLiteral(
                        "The project changed while the assembly catalog was open. Review the refreshed list."));
                    return std::nullopt;
                }
                return record;
            };

            const auto selected_type_id = [&]() -> std::string {
                const auto* item = types->currentItem();
                return item ? item->data(Qt::UserRole).toString().toStdString() : std::string{};
            };
            const auto selected_instance_id = [&]() -> std::string {
                const auto* item = instances->currentItem();
                return item ? item->data(Qt::UserRole).toString().toStdString() : std::string{};
            };
            const auto read_quantity = [](QLineEdit* value, QComboBox* unit) {
                bool ok = false;
                const auto number = value->text().trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(number) || number < 0)
                    throw std::invalid_argument("Enter a finite, nonnegative quantity.");
                const auto parsed_unit = assembly_quantity_unit(unit->currentData().toString());
                if (parsed_unit == AssemblyQuantityUnit::count && std::floor(number) != number)
                    throw std::invalid_argument("Count quantities must be whole numbers.");
                return AssemblyQuantityProperty{number, parsed_unit};
            };

            QObject::connect(add_material, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = material_id->text().trimmed().toStdString();
                    const auto name = material_name->text().trimmed().toStdString();
                    if (id.empty() || name.empty())
                        throw std::invalid_argument("Enter a material ID and name.");
                    auto material_copy = current->model.materials();
                    const auto color = material_color->text().trimmed().toStdString();
                    AssemblyMaterial replacement{id, name, color.empty() ? std::nullopt : std::optional{color}};
                    const auto found = std::find_if(material_copy.begin(), material_copy.end(),
                        [&](const auto& material) { return material.id == id; });
                    if (found == material_copy.end()) material_copy.push_back(std::move(replacement));
                    else *found = std::move(replacement);
                    const auto updated = AssemblyModel::create(
                        std::move(material_copy), current->model.types(), current->model.instances());
                    if (applyAssemblyModel(updated, QStringLiteral("Save assembly material"))) {
                        populate();
                        data_tabs->setCurrentWidget(material_page);
                        status->setText(QStringLiteral("Material saved."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_material, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = materials->currentItem();
                    if (!item) throw std::invalid_argument("Choose a material first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    auto material_copy = current->model.materials();
                    material_copy.erase(std::remove_if(material_copy.begin(), material_copy.end(),
                        [&](const auto& candidate) { return candidate.id == id; }), material_copy.end());
                    const auto updated = AssemblyModel::create(
                        std::move(material_copy), current->model.types(), current->model.instances());
                    if (applyAssemblyModel(updated, QStringLiteral("Remove assembly material"))) {
                        populate();
                        data_tabs->setCurrentWidget(material_page);
                        status->setText(QStringLiteral("Material removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(save_type_entry, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = selected_type_id();
                    if (id.empty()) throw std::invalid_argument("Choose a reusable type first.");
                    const auto key = type_entry_key->text().trimmed().toStdString();
                    const auto value = type_entry_value->text().trimmed().toStdString();
                    if (key.empty() || value.empty())
                        throw std::invalid_argument("Enter a key and value.");
                    auto replacement = *std::find_if(current->model.types().begin(),
                                                     current->model.types().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    const auto kind = type_entry_kind->currentData().toString();
                    if (kind == QStringLiteral("property")) {
                        replacement.properties[key] = value;
                    } else if (kind == QStringLiteral("material")) {
                        replacement.materials[key] = value;
                    } else {
                        replacement.quantities[key] = read_quantity(type_entry_value, type_entry_unit);
                    }
                    const auto updated = current->model.with_type(std::move(replacement));
                    if (applyAssemblyModel(updated, QStringLiteral("Save assembly type entry"))) {
                        populate();
                        data_tabs->setCurrentWidget(type_data_page);
                        status->setText(QStringLiteral("Type schema entry saved through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_type_entry, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = selected_type_id();
                    if (id.empty()) throw std::invalid_argument("Choose a reusable type first.");
                    const auto* entry = type_schema->currentItem();
                    if (!entry) throw std::invalid_argument("Choose a type schema entry first.");
                    auto replacement = *std::find_if(current->model.types().begin(),
                                                     current->model.types().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    const auto key = entry->data(Qt::UserRole + 1).toString().toStdString();
                    const auto kind = entry_kind_id(entry->data(Qt::UserRole).toString());
                    if (kind == QStringLiteral("property")) replacement.properties.erase(key);
                    else if (kind == QStringLiteral("material")) replacement.materials.erase(key);
                    else replacement.quantities.erase(key);
                    const auto updated = current->model.with_type(std::move(replacement));
                    if (applyAssemblyModel(updated, QStringLiteral("Remove assembly type entry"))) {
                        populate();
                        data_tabs->setCurrentWidget(type_data_page);
                        status->setText(QStringLiteral("Type schema entry removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(save_override, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = selected_instance_id();
                    if (id.empty()) throw std::invalid_argument("Choose a placed instance first.");
                    const auto key = override_key->text().trimmed().toStdString();
                    const auto value = override_value->text().trimmed().toStdString();
                    if (key.empty() || value.empty())
                        throw std::invalid_argument("Enter a key and value.");
                    auto replacement = *std::find_if(current->model.instances().begin(),
                                                     current->model.instances().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    const auto kind = override_kind->currentData().toString();
                    if (kind == QStringLiteral("property")) {
                        replacement.property_overrides[key] = value;
                    } else if (kind == QStringLiteral("material")) {
                        replacement.material_overrides[key] = value;
                    } else {
                        replacement.quantity_overrides[key] = read_quantity(override_value, override_unit);
                    }
                    const auto updated = current->model.with_instance(std::move(replacement));
                    if (applyAssemblyModel(updated, QStringLiteral("Save assembly instance override"))) {
                        populate();
                        data_tabs->setCurrentWidget(override_page);
                        status->setText(QStringLiteral("Instance override saved through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_override, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = selected_instance_id();
                    if (id.empty()) throw std::invalid_argument("Choose a placed instance first.");
                    const auto* entry = overrides->currentItem();
                    if (!entry) throw std::invalid_argument("Choose an instance override first.");
                    auto replacement = *std::find_if(current->model.instances().begin(),
                                                     current->model.instances().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    const auto key = entry->data(Qt::UserRole + 1).toString().toStdString();
                    const auto kind = entry_kind_id(entry->data(Qt::UserRole).toString());
                    if (kind == QStringLiteral("property")) replacement.property_overrides.erase(key);
                    else if (kind == QStringLiteral("material")) replacement.material_overrides.erase(key);
                    else replacement.quantity_overrides.erase(key);
                    const auto updated = current->model.with_instance(std::move(replacement));
                    if (applyAssemblyModel(updated, QStringLiteral("Remove assembly instance override"))) {
                        populate();
                        data_tabs->setCurrentWidget(override_page);
                        status->setText(QStringLiteral("Instance override removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(add_type, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto id = type_id->text().trimmed().toStdString();
                    const auto name = type_name->text().trimmed().toStdString();
                    if (id.empty() || name.empty())
                        throw std::invalid_argument("Enter a type ID and name.");
                    auto types_copy = current->model.types();
                    types_copy.push_back({id, name, {}, {}, {}});
                    const auto updated = AssemblyModel::create(
                        current->model.materials(), std::move(types_copy),
                        current->model.instances());
                    if (applyAssemblyModel(updated, QStringLiteral("Add assembly type"))) {
                        populate();
                        status->setText(QStringLiteral("Reusable type added through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(rename_type, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = types->currentItem();
                    if (!item) throw std::invalid_argument("Choose a reusable type first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    const auto name = type_name->text().trimmed().toStdString();
                    if (name.empty()) throw std::invalid_argument("Enter a type name.");
                    auto replacement = *std::find_if(current->model.types().begin(),
                                                     current->model.types().end(),
                        [&](const auto& candidate) { return candidate.id == id; });
                    replacement.name = name;
                    const auto updated = current->model.with_type(std::move(replacement));
                    if (applyAssemblyModel(updated, QStringLiteral("Rename assembly type"))) {
                        populate();
                        status->setText(QStringLiteral("Type name updated through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_type, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = types->currentItem();
                    if (!item) throw std::invalid_argument("Choose a reusable type first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    if (std::any_of(current->model.instances().begin(),
                                    current->model.instances().end(),
                        [&](const auto& instance) { return instance.type_id == id; })) {
                        throw std::invalid_argument(
                            "Remove the type's placed instances before removing the type.");
                    }
                    auto types_copy = current->model.types();
                    types_copy.erase(std::remove_if(types_copy.begin(), types_copy.end(),
                        [&](const auto& candidate) { return candidate.id == id; }), types_copy.end());
                    const auto updated = AssemblyModel::create(
                        current->model.materials(), std::move(types_copy),
                        current->model.instances());
                    if (applyAssemblyModel(updated, QStringLiteral("Remove assembly type"))) {
                        populate();
                        status->setText(QStringLiteral("Type removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(add_instance, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto type_id_value = instance_type->currentData().toString().toStdString();
                    if (type_id_value.empty()) throw std::invalid_argument("Choose a type first.");
                    auto id = instance_id->text().trimmed().toStdString();
                    if (id.empty()) id = new_id("assembly-instance");
                    auto instances_copy = current->model.instances();
                    instances_copy.push_back({id, type_id_value, {}, {}, {}});
                    const auto updated = AssemblyModel::create(
                        current->model.materials(), current->model.types(),
                        std::move(instances_copy));
                    if (applyAssemblyModel(updated, QStringLiteral("Add assembly instance"))) {
                        instance_id->clear();
                        populate();
                        status->setText(QStringLiteral("Instance added through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });

            QObject::connect(remove_instance, &QPushButton::clicked, &dialog, [&] {
                try {
                    const auto current = current_record();
                    if (!current) return;
                    const auto* item = instances->currentItem();
                    if (!item) throw std::invalid_argument("Choose a placed instance first.");
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    auto instances_copy = current->model.instances();
                    instances_copy.erase(std::remove_if(instances_copy.begin(), instances_copy.end(),
                        [&](const auto& candidate) { return candidate.id == id; }), instances_copy.end());
                    const auto updated = AssemblyModel::create(
                        current->model.materials(), current->model.types(),
                        std::move(instances_copy));
                    if (applyAssemblyModel(updated, QStringLiteral("Remove assembly instance"))) {
                        populate();
                        status->setText(QStringLiteral("Instance removed through document history."));
                    }
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                }
            });
            QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
            dialog.exec();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assemblies: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    QString createOrganization(const QString& parent_id, const QString& name, std::string type,
                               std::optional<Revision> expected_revision = std::nullopt) {
        try {
            if (name.trimmed().isEmpty()) throw std::invalid_argument("Enter a name.");
            const auto modal_context = captureModalContext();
            if (!confirmDiscardBoundaryDraft() || !modalContextUnchanged(modal_context)) return {};
            const auto snapshot = m_document->snapshot();
            const auto revision = expected_revision.value_or(snapshot.revision());
            if (snapshot.revision() != revision)
                throw DocumentError(DocumentErrorCode::stale_revision, "The project changed before organization authoring could apply.");
            const auto parent = snapshot.entities().find(parent_id.toStdString());
            const auto expected = type == "building" ? "property" : type == "floor" ? "building" : "floor";
            if (parent == snapshot.entities().end() || parent->second.type != expected)
                throw std::invalid_argument("Choose the parent property, building or floor first.");
            const auto organization = organize_project(snapshot);
            const auto& parent_node = organization.nodes.at(parent->first);
            if (!parent_node.issues.empty() || parent_node.context.property_id.empty())
                throw std::invalid_argument("Resolve the parent hierarchy before adding children.");
            auto entity = Entity::create(type, {{"name", name.trimmed().toStdString()},
                {std::string(expected) + "_id", parent->first}});
            const auto id = entity.id;
            std::vector<EntityChange> changes{EntityChange::upsert(entity)};
            std::string layer_id;
            if (type == "floor") {
                auto layer = Entity::create("layer", {{"name", "Default layer"}, {"floor_id", id}});
                layer_id = layer.id;
                changes.push_back(EntityChange::upsert(std::move(layer)));
            } else if (type == "layer") layer_id = id;
            applyDocumentCommand(ApplyEntityChanges{
                .expected_revision = revision,
                .entity_changes = std::move(changes),
                .message = "create " + type,
            });
            if (!layer_id.empty()) m_active_layer_id = id_from(layer_id);
            m_selected_id = id_from(id);
            clearPreview();
            m_tool = CanvasTool::select;
            syncToolControls();
            clearError();
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Project organization: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    bool renameOrganizationEntity(const QString& id, const QString& name,
                                  std::optional<Revision> expected_revision = std::nullopt) {
        const auto snapshot = m_document->snapshot();
        const auto found = snapshot.entities().find(id.toStdString());
        if (found == snapshot.entities().end() || name.trimmed().isEmpty() ||
            (found->second.type != "property" && found->second.type != "building" &&
             found->second.type != "floor" && found->second.type != "layer")) {
            setError(QStringLiteral("Select a property, building, floor or layer and enter a name."));
            return false;
        }
        auto candidate = found->second;
        candidate.properties["name"] = name.trimmed().toStdString();
        if (!applyEntity(std::move(candidate), "rename project organization",
                         expected_revision.value_or(snapshot.revision()))) return false;
        clearError();
        refresh();
        return true;
    }

    QString createAnnotationLabel(const QString& template_id, const QString& content,
                                  Vec2 position) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
                throw std::invalid_argument("Annotation position must be finite.");
            }
            const auto source = authoringSnapshot();
            auto annotation = std::find_if(source.entities().begin(), source.entities().end(),
                                            [](const auto& entry) {
                                                return entry.second.type == kAnnotationEntityType;
                                            });
            if (annotation == source.entities().end()) {
                throw std::invalid_argument("The project has no annotation state entity.");
            }
            const auto templates = default_label_templates();
            const auto wanted = template_id.trimmed().toStdString();
            const auto definition = std::find_if(
                templates.begin(), templates.end(),
                [&](const auto& candidate) { return candidate.id == wanted; });
            if (definition == templates.end()) {
                throw std::invalid_argument("Unknown annotation label template.");
            }
            auto state = decode_annotation_entity(annotation->second);
            auto label = instantiate_label(*definition, new_id("label"));
            if (!content.trimmed().isEmpty()) {
                label.content = content.toStdString();
            }
            label.placement.position = position;
            state.labels.push_back(label);
            const auto command = ApplyEntityChanges{
                source.revision(),
                {EntityChange::upsert(make_annotation_entity(annotation->second.id, state))},
                {}, "Add annotation label"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = id_from(label.id);
            clearError();
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Annotation label: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    QString createAnnotationSymbol(const QString& symbol_id, Vec2 position) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
                throw std::invalid_argument("Annotation position must be finite.");
            }
            const auto source = authoringSnapshot();
            auto annotation = std::find_if(source.entities().begin(), source.entities().end(),
                                            [](const auto& entry) {
                                                return entry.second.type == kAnnotationEntityType;
                                            });
            if (annotation == source.entities().end()) {
                throw std::invalid_argument("The project has no annotation state entity.");
            }
            const auto catalog = default_symbol_catalog();
            const auto wanted = symbol_id.trimmed().toStdString();
            const auto definition = std::find_if(
                catalog.begin(), catalog.end(),
                [&](const auto& candidate) { return candidate.id == wanted; });
            if (definition == catalog.end()) {
                throw std::invalid_argument("Unknown annotation symbol.");
            }
            auto state = decode_annotation_entity(annotation->second);
            SymbolInstance symbol;
            symbol.id = new_id("symbol");
            symbol.symbol_id = definition->id;
            symbol.placement.position = position;
            state.symbols.push_back(symbol);
            const auto command = ApplyEntityChanges{
                source.revision(),
                {EntityChange::upsert(make_annotation_entity(annotation->second.id, state))},
                {}, "Add annotation symbol"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = id_from(symbol.id);
            clearError();
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Annotation symbol: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    bool editAnnotation(const QString& annotation_id, const QString& content,
                        const QString& x_metres, const QString& y_metres,
                        const QString& rotation_degrees, const QString& scale,
                        bool visible) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto parse_finite = [](const QString& text, const char* name) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value)) throw std::invalid_argument(name);
                return value;
            };
            const auto x = parse_finite(x_metres, "Annotation X must be finite metres.");
            const auto y = parse_finite(y_metres, "Annotation Y must be finite metres.");
            const auto rotation = parse_finite(
                rotation_degrees, "Annotation rotation must be finite degrees.");
            const auto instance_scale = parse_finite(scale, "Annotation scale must be finite.");
            if (!(instance_scale > 0.0) || instance_scale > 100.0) {
                throw std::invalid_argument("Annotation scale must be greater than zero and no more than 100.");
            }
            const auto source = authoringSnapshot();
            auto annotation = std::find_if(source.entities().begin(), source.entities().end(),
                                            [](const auto& entry) {
                                                return entry.second.type == kAnnotationEntityType;
                                            });
            if (annotation == source.entities().end()) {
                throw std::invalid_argument("The project has no annotation state entity.");
            }
            auto state = decode_annotation_entity(annotation->second);
            const auto wanted = annotation_id.trimmed().toStdString();
            bool found = false;
            for (auto& label : state.labels) {
                if (label.id != wanted) continue;
                const auto replacement = content.trimmed();
                if (replacement.isEmpty()) {
                    throw std::invalid_argument("Label text cannot be empty.");
                }
                label.content = replacement.toStdString();
                label.placement.position = {x, y};
                label.placement.rotation_radians = rotation * std::numbers::pi / 180.0;
                label.placement.scale = instance_scale;
                label.visible = visible;
                found = true;
                break;
            }
            if (!found) {
                for (auto& symbol : state.symbols) {
                    if (symbol.id != wanted) continue;
                    symbol.placement.position = {x, y};
                    symbol.placement.rotation_radians = rotation * std::numbers::pi / 180.0;
                    symbol.placement.scale = instance_scale;
                    symbol.visible = visible;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::invalid_argument("Annotation was not found.");
            const auto command = ApplyEntityChanges{
                source.revision(),
                {EntityChange::upsert(make_annotation_entity(annotation->second.id, state))},
                {}, "Edit annotation"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = annotation_id.trimmed();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Edit annotation: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool deleteAnnotation(const QString& annotation_id) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto source = authoringSnapshot();
            auto annotation = std::find_if(source.entities().begin(), source.entities().end(),
                                            [](const auto& entry) {
                                                return entry.second.type == kAnnotationEntityType;
                                            });
            if (annotation == source.entities().end()) {
                throw std::invalid_argument("The project has no annotation state entity.");
            }
            auto state = decode_annotation_entity(annotation->second);
            const auto wanted = annotation_id.trimmed().toStdString();
            const auto labels_before = state.labels.size();
            const auto symbols_before = state.symbols.size();
            std::erase_if(state.labels, [&](const auto& label) { return label.id == wanted; });
            std::erase_if(state.symbols, [&](const auto& symbol) { return symbol.id == wanted; });
            if (state.labels.size() == labels_before && state.symbols.size() == symbols_before) {
                throw std::invalid_argument("Annotation was not found.");
            }
            const auto command = ApplyEntityChanges{
                source.revision(),
                {EntityChange::upsert(make_annotation_entity(annotation->second.id, state))},
                {}, "Delete annotation"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id.clear();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Delete annotation: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    QString importReferenceImage(const QString& path, int page_index = 0) {
        try {
            if (!m_document->is_editable()) {
                throw std::invalid_argument("This document is read-only.");
            }
            const auto cleaned = path.trimmed();
            if (cleaned.isEmpty()) throw std::invalid_argument("Choose a reference image or PDF.");
            QFile file(cleaned);
            if (!file.open(QIODevice::ReadOnly)) {
                throw std::invalid_argument("The reference image could not be opened.");
            }
            const auto raw = file.readAll();
            if (raw.isEmpty()) throw std::invalid_argument("The reference image is empty.");
            QString mime;
            const auto suffix = QFileInfo(cleaned).suffix().trimmed().toLower();
            QImage image;
            std::optional<Asset> render_asset;
            if (suffix == QStringLiteral("pdf")) {
                QPdfDocument pdf;
                if (pdf.load(cleaned) != QPdfDocument::Error::None || pdf.pageCount() < 1) {
                    throw std::invalid_argument("The reference PDF could not be decoded.");
                }
                if (page_index < 0 || page_index >= pdf.pageCount()) {
                    throw std::invalid_argument("The selected PDF page does not exist.");
                }
                const auto page_points = pdf.pagePointSize(page_index);
                if (!(page_points.width() > 0.0) || !(page_points.height() > 0.0) ||
                    !std::isfinite(page_points.width()) || !std::isfinite(page_points.height())) {
                    throw std::invalid_argument("The reference PDF has no valid selected page.");
                }
                const auto pixels = [](qreal points) {
                    return std::clamp(static_cast<int>(std::lround(points * 2.0)), 256, 4096);
                };
                image = pdf.render(page_index, QSize(pixels(page_points.width()), pixels(page_points.height())));
                if (image.isNull()) throw std::invalid_argument("The reference PDF page could not be rasterized.");
                QByteArray preview_bytes;
                QBuffer preview_buffer(&preview_bytes);
                if (!preview_buffer.open(QIODevice::WriteOnly) || !image.save(&preview_buffer, "PNG")) {
                    throw std::invalid_argument("The reference PDF preview could not be encoded.");
                }
                std::vector<std::byte> preview;
                preview.reserve(static_cast<std::size_t>(preview_bytes.size()));
                for (const auto value : preview_bytes) preview.push_back(static_cast<std::byte>(value));
                render_asset = Asset::create(new_id("reference-preview"), "image/png",
                    std::move(preview), { {"content", "pdf-page-preview"},
                                          {"page_index", page_index}, {"page_count", pdf.pageCount()} });
                mime = QStringLiteral("application/pdf");
            } else {
                image = QImage::fromData(raw);
                if (image.isNull()) {
                    throw std::invalid_argument("Only decodable PDF, PNG, JPEG, BMP, and TIFF images are supported.");
                }
                if (suffix == QStringLiteral("png")) mime = QStringLiteral("image/png");
                else if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
                    mime = QStringLiteral("image/jpeg");
                else if (suffix == QStringLiteral("bmp")) mime = QStringLiteral("image/bmp");
                else if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff"))
                    mime = QStringLiteral("image/tiff");
                else {
                    throw std::invalid_argument("Reference image extension must be PDF, PNG, JPEG, BMP, or TIFF.");
                }
            }
            std::vector<std::byte> bytes;
            bytes.reserve(static_cast<std::size_t>(raw.size()));
            for (const auto value : raw) bytes.push_back(static_cast<std::byte>(value));
            const auto source = authoringSnapshot();
            const auto asset_id = new_id("reference-asset");
            const auto entity_id = new_id("reference");
            auto asset = Asset::create(asset_id, mime.toStdString(), std::move(bytes),
                                       { {"source_path", QFileInfo(cleaned).fileName().toStdString()},
                                         {"width_px", image.width()}, {"height_px", image.height()},
                                         {"content", "raster-reference"} });
            auto entity = Entity::create("reference_asset",
                {{"asset_id", asset_id},
                 {"source_path", QFileInfo(cleaned).fileName().toStdString()},
                 {"mime_type", mime.toStdString()},
                 {"render_asset_id", render_asset ? render_asset->id : asset_id},
                 {"position_m", {0.0, 0.0}},
                 {"metres_per_source_unit", 0.01},
                 {"scale", 1.0}, {"rotation_degrees", 0.0},
                 {"flip_horizontal", false}, {"flip_vertical", false},
                 {"intensity", 0.72}, {"visible", true}});
            entity.id = entity_id;
            std::vector<AssetChange> asset_changes;
            asset_changes.push_back(AssetChange::upsert(std::move(asset)));
            if (render_asset) asset_changes.push_back(AssetChange::upsert(std::move(*render_asset)));
            const auto command = ApplyEntityChanges{
                source.revision(), {EntityChange::upsert(std::move(entity))},
                std::move(asset_changes), "Import reference image"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = id_from(entity_id);
            clearError();
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Reference image: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    bool calibrateReference(const QString& reference_id, const QString& first_x,
                            const QString& first_y, const QString& second_x,
                            const QString& second_y, const QString& known_distance) {
        try {
            if (!m_document->is_editable()) throw std::invalid_argument("This document is read-only.");
            const auto parse_finite = [](const QString& text, const char* message) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value)) throw std::invalid_argument(message);
                return value;
            };
            const Vec2 first{parse_finite(first_x, "First calibration X must be finite pixels."),
                             parse_finite(first_y, "First calibration Y must be finite pixels.")};
            const Vec2 second{parse_finite(second_x, "Second calibration X must be finite pixels."),
                              parse_finite(second_y, "Second calibration Y must be finite pixels.")};
            const auto known = parse_quantity(known_distance.trimmed().toStdString(), Unit::metre);
            const auto source_distance = std::hypot(second.x - first.x, second.y - first.y);
            if (!(source_distance > 0.0) || !std::isfinite(source_distance) ||
                !(known.metres > 0.0) || !std::isfinite(known.metres)) {
                throw std::invalid_argument("Calibration distances must be finite and positive.");
            }
            const auto source = authoringSnapshot();
            const auto found = source.entities().find(reference_id.trimmed().toStdString());
            if (found == source.entities().end() || found->second.type != "reference_asset") {
                throw std::invalid_argument("Reference image was not found.");
            }
            auto updated = found->second;
            updated.properties["calibration_first_source"] = json::array({first.x, first.y});
            updated.properties["calibration_second_source"] = json::array({second.x, second.y});
            updated.properties["calibration_known_distance"] = known.original_expression;
            updated.properties["calibration_input_unit"] = static_cast<int>(known.entered_unit);
            updated.properties["metres_per_source_unit"] = known.metres / source_distance;
            const auto command = ApplyEntityChanges{
                source.revision(), {EntityChange::upsert(std::move(updated))}, {},
                "Calibrate reference image"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = reference_id.trimmed();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Reference calibration: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool editReferenceTransform(const QString& reference_id, const QString& x_metres,
                                const QString& y_metres, const QString& metres_per_source_unit,
                                const QString& scale, const QString& rotation_degrees,
                                const QString& intensity, bool flip_horizontal,
                                bool flip_vertical, bool visible) {
        try {
            if (!m_document->is_editable()) throw std::invalid_argument("This document is read-only.");
            const auto parse_finite = [](const QString& text, const char* message) {
                bool ok = false;
                const auto value = text.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(value)) throw std::invalid_argument(message);
                return value;
            };
            const auto x = parse_finite(x_metres, "Reference X must be finite metres.");
            const auto y = parse_finite(y_metres, "Reference Y must be finite metres.");
            const auto calibration = parse_finite(
                metres_per_source_unit, "Reference calibration must be finite.");
            const auto transform_scale = parse_finite(scale, "Reference scale must be finite.");
            const auto rotation = parse_finite(rotation_degrees, "Reference rotation must be finite.");
            const auto alpha = parse_finite(intensity, "Reference intensity must be finite.");
            if (!(calibration > 0.0) || !(transform_scale > 0.0) || alpha < 0.0 || alpha > 1.0) {
                throw std::invalid_argument("Reference calibration/scale must be positive and intensity must be 0..1.");
            }
            const auto source = authoringSnapshot();
            const auto found = source.entities().find(reference_id.trimmed().toStdString());
            if (found == source.entities().end() || found->second.type != "reference_asset") {
                throw std::invalid_argument("Reference image was not found.");
            }
            auto updated = found->second;
            updated.properties["position_m"] = json::array({x, y});
            updated.properties["metres_per_source_unit"] = calibration;
            updated.properties["scale"] = transform_scale;
            updated.properties["rotation_degrees"] = rotation;
            updated.properties["intensity"] = alpha;
            updated.properties["flip_horizontal"] = flip_horizontal;
            updated.properties["flip_vertical"] = flip_vertical;
            updated.properties["visible"] = visible;
            const auto command = ApplyEntityChanges{
                source.revision(), {EntityChange::upsert(std::move(updated))}, {},
                "Edit reference transform"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = reference_id.trimmed();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Reference transform: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool beginReferenceTrace() {
        try {
            const auto selected = m_selected_id.trimmed().toStdString();
            const auto snapshot = m_document->snapshot();
            const auto found = snapshot.entities().find(selected);
            if (found == snapshot.entities().end() ||
                found->second.type != "reference_asset") {
                throw std::invalid_argument("Select a reference image before tracing it.");
            }
            if (!beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, {})) return false;
            owner->statusBar()->showMessage(
                QStringLiteral("Trace reference • click boundary points, use D for precise input, Enter to close"));
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Trace reference: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] bool assistanceEnabled() const noexcept {
        return m_assistance_session.enabled();
    }

    void setAssistanceEnabled(bool enabled) {
        m_assistance_session.set_enabled(enabled);
        if (!enabled) clearError();
    }

    [[nodiscard]] AssistanceRaster decodeAssistanceReference(
        const QString& reference_id) const {
        const auto snapshot = authoringSnapshot();
        const auto found = snapshot.entities().find(reference_id.trimmed().toStdString());
        if (found == snapshot.entities().end() || found->second.type != "reference_asset") {
            throw std::invalid_argument("Select a reference image before generating assistance.");
        }
        const auto render_id = found->second.properties.value(
            "render_asset_id", found->second.properties.value("asset_id", json{}));
        if (!render_id.is_string()) throw std::invalid_argument("Reference render asset is missing.");
        const auto asset = snapshot.assets().find(render_id.get<std::string>());
        if (asset == snapshot.assets().end()) throw std::invalid_argument("Reference render asset is missing.");
        const QByteArray raw(reinterpret_cast<const char*>(asset->second.bytes.data()),
                             static_cast<qsizetype>(asset->second.bytes.size()));
        const auto image = QImage::fromData(raw).convertToFormat(QImage::Format_Grayscale8);
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            throw std::invalid_argument("Reference render asset is not a decodable raster.");
        }
        AssistanceRaster raster;
        raster.reference_id = found->second.id;
        if (const auto source = found->second.properties.find("source_text");
            source != found->second.properties.end() && source->is_string()) {
            raster.source_text = source->get<std::string>();
        }
        raster.width = static_cast<std::size_t>(image.width());
        raster.height = static_cast<std::size_t>(image.height());
        raster.luminance.resize(raster.width * raster.height);
        for (int y = 0; y < image.height(); ++y) {
            const auto* scanline = image.constScanLine(y);
            std::copy(scanline, scanline + image.width(),
                      raster.luminance.begin() + static_cast<std::size_t>(y) * raster.width);
        }
        return raster;
    }

    [[nodiscard]] std::vector<AssistanceProposal> suggestReferenceAssistance(
        const QString& reference_id, AssistanceKind kind) {
        try {
            if (!m_assistance_session.enabled()) {
                throw std::invalid_argument("Enable assistance in the assistance dialog first.");
            }
            const auto raster = decodeAssistanceReference(reference_id);
            const auto snapshot = authoringSnapshot();
            const auto reference = snapshot.entities().find(reference_id.trimmed().toStdString());
            const auto calibration = reference == snapshot.entities().end()
                ? 0.01 : read_number(reference->second.properties, "metres_per_source_unit", 0.01);
            const auto origin = reference == snapshot.entities().end()
                ? Vec2{} : read_point(reference->second.properties.value("position_m", json{})).value_or(Vec2{});
            const auto rotation = reference == snapshot.entities().end()
                ? 0.0 : read_number(reference->second.properties, "rotation_degrees", 0.0) *
                    std::numbers::pi / 180.0;
            const auto scale = reference == snapshot.entities().end()
                ? 1.0 : read_number(reference->second.properties, "scale", 1.0);
            const AssistanceEngineOptions options{calibration, origin, rotation, scale};
            switch (kind) {
            case AssistanceKind::tracing:
                return suggest_tracing(raster, options);
            case AssistanceKind::dimension_extraction:
                return extract_dimensions(raster, options);
            case AssistanceKind::label_placement:
            case AssistanceKind::natural_language:
                throw std::invalid_argument("This assistance kind does not use a reference raster.");
            }
            throw std::invalid_argument("Unknown assistance kind.");
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assistance: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    [[nodiscard]] std::vector<AssistanceProposal> suggestLabelAssistance() {
        try {
            if (!m_assistance_session.enabled()) {
                throw std::invalid_argument("Enable assistance in the assistance dialog first.");
            }
            const auto snapshot = authoringSnapshot();
            std::vector<AssistanceAnchor> anchors;
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type == "property" || entity.type == "building" ||
                    entity.type == "floor" || entity.type == "layer" ||
                    entity.type == kAnnotationEntityType || entity.type == "sheet_view_model" ||
                    !entity.properties.contains("name") || !entity.properties.at("name").is_string()) {
                    continue;
                }
                Vec2 position{};
                for (const auto* key : {"position_m", "base_position_m", "start_m"}) {
                    if (entity.properties.contains(key)) {
                        if (const auto point = read_point(entity.properties.at(key))) {
                            position = *point;
                            break;
                        }
                    }
                }
                anchors.push_back({id, entity.properties.at("name").get<std::string>(), position});
            }
            return sketch::suggest_label_placements(anchors);
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assistance labels: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    [[nodiscard]] std::vector<AssistanceProposal> parseAssistanceCommand(
        const QString& command) {
        try {
            if (!m_assistance_session.enabled()) {
                throw std::invalid_argument("Enable assistance in the assistance dialog first.");
            }
            return parse_natural_language(command.toStdString());
        } catch (const std::exception& error) {
            setError(QStringLiteral("Assistance command: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    [[nodiscard]] Boundary assistanceBoundary(const json& points) const {
        if (!points.is_array() || points.size() < 3 || points.size() > 256) {
            throw std::invalid_argument("Assisted boundary preview requires three to 256 points.");
        }
        std::vector<Vec2> vertices;
        vertices.reserve(points.size());
        for (const auto& value : points) {
            const auto point = read_point(value);
            if (!point) throw std::invalid_argument("Assisted boundary preview contains an invalid point.");
            vertices.push_back(*point);
        }
        Boundary boundary;
        boundary.reserve(vertices.size());
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            boundary.push_back({vertices[index], vertices[(index + 1) % vertices.size()], 0.0});
        }
        const auto diagnostics = validate_boundary(boundary);
        if (!diagnostics.empty()) throw std::invalid_argument(diagnostics.front().message);
        return boundary;
    }

    [[nodiscard]] bool applyAssistedLabel(const AssistanceProposal& proposal) {
        const auto& args = proposal.preview.arguments;
        if (!args.contains("template_id") || !args.at("template_id").is_string() ||
            !args.contains("content") || !args.at("content").is_string() ||
            !args.contains("position")) {
            throw std::invalid_argument("Assisted label preview is incomplete.");
        }
        const auto position = read_point(args.at("position"));
        if (!position) throw std::invalid_argument("Assisted label position is invalid.");
        const auto id = args.value("annotation_id", proposal.id);
        if (id.empty()) {
            throw std::invalid_argument("Assisted label ID is invalid.");
        }
        const auto source = authoringSnapshot();
        if (source.entities().contains(id)) {
            throw std::invalid_argument("Assisted label ID already exists.");
        }
        const auto annotation = std::find_if(source.entities().begin(), source.entities().end(),
                                              [](const auto& entry) {
                                                  return entry.second.type == kAnnotationEntityType;
                                              });
        if (annotation == source.entities().end()) throw std::invalid_argument("Annotation state is missing.");
        const auto templates = default_label_templates();
        const auto template_id = args.at("template_id").get<std::string>();
        const auto definition = std::find_if(templates.begin(), templates.end(),
            [&](const auto& candidate) { return candidate.id == template_id; });
        if (definition == templates.end()) throw std::invalid_argument("Assisted label template is unknown.");
        auto state = decode_annotation_entity(annotation->second);
        auto label = instantiate_label(*definition, id);
        label.content = args.at("content").get<std::string>();
        label.placement.position = *position;
        state.labels.push_back(std::move(label));
        const ApplyEntityChanges command{
            source.revision(), {EntityChange::upsert(make_annotation_entity(annotation->second.id, state))},
            {}, "Accept assisted label"};
        (void)Document::preview_command(source, command);
        applyDocumentCommand(command);
        m_selected_id = id_from(id);
        refresh();
        return true;
    }

    [[nodiscard]] bool applyAssistedBoundary(const AssistanceProposal& proposal) {
        const auto& args = proposal.preview.arguments;
        const auto boundary = assistanceBoundary(args.value("points", json{}));
        const auto id = args.value("boundary_id", proposal.id);
        if (id.empty()) {
            throw std::invalid_argument("Assisted boundary ID is invalid.");
        }
        const auto source = authoringSnapshot();
        if (source.entities().contains(id)) {
            throw std::invalid_argument("Assisted boundary ID already exists.");
        }
        const auto context = requireDrawingContext();
        if (!context) return false;
        IdentifiedBoundary identified{id, "measurement_boundary", {}};
        identified.segments.reserve(boundary.size());
        for (std::size_t index = 0; index < boundary.size(); ++index) {
            const auto segment_id = id + "-segment-" + std::to_string(index);
            const auto start_id = id + "-vertex-" + std::to_string(index);
            const auto end_id = id + "-vertex-" +
                                std::to_string((index + 1) % boundary.size());
            identified.segments.push_back({segment_id, start_id, end_id, boundary[index]});
        }
        auto entity = encode_identified_boundary_entity(identified);
        entity.properties["property_id"] = context->property_id;
        entity.properties["building_id"] = context->building_id;
        entity.properties["floor_id"] = context->floor_id;
        entity.properties["layer_id"] = context->layer_id;
        entity.properties["classification"] = args.value("classification", "measurement");
        entity.properties["factor"] = 1.0;
        entity.properties["factor_expression"] = "1";
        entity.properties["factor_numerator"] = 1;
        entity.properties["factor_denominator"] = 1;
        const ApplyEntityChanges command{
            source.revision(), {EntityChange::upsert(std::move(entity))}, {},
            "Accept assisted boundary"};
        (void)Document::preview_command(source, command);
        applyDocumentCommand(command);
        m_selected_id = id_from(id);
        refresh();
        return true;
    }

    [[nodiscard]] bool applyAssistedDimension(const AssistanceProposal& proposal) {
        const auto& args = proposal.preview.arguments;
        const auto target_value = args.value("target_boundary_id", json{});
        if (!target_value.is_string() || target_value.get<std::string>().empty()) {
            throw std::invalid_argument("Choose a target boundary before accepting a dimension suggestion.");
        }
        const auto source = authoringSnapshot();
        const auto target_found = source.entities().find(target_value.get<std::string>());
        if (target_found == source.entities().end() ||
            !can_recognize_boundary_entity_type(target_found->second.type)) {
            throw std::invalid_argument("Assisted dimension target boundary was not found.");
        }
        auto target = target_found->second;
        const auto original_target = target;
        const auto version = inspect_boundary_entity_version(target);
        if (version.format == BoundaryEntityFormat::anonymous_legacy) {
            target = upgrade_legacy_boundary_entity(target);
        }
        const auto identified = decode_identified_boundary_entity(target);
        if (identified.segments.empty()) throw std::invalid_argument("Target boundary has no segments.");
        auto segment_id = args.value("target_segment_id", std::string{});
        if (segment_id.empty()) {
            segment_id = identified.segments.front().segment_id;
        }
        const auto segment = std::find_if(identified.segments.begin(), identified.segments.end(),
            [&](const auto& candidate) { return candidate.segment_id == segment_id; });
        if (segment == identified.segments.end()) throw std::invalid_argument("Target boundary segment was not found.");
        const auto midpoint = Vec2{(segment->segment.start.x + segment->segment.end.x) * 0.5,
                                   (segment->segment.start.y + segment->segment.end.y) * 0.5};
        const auto dx = segment->segment.end.x - segment->segment.start.x;
        const auto dy = segment->segment.end.y - segment->segment.start.y;
        const auto length = std::hypot(dx, dy);
        const auto text_position = length > 1e-12
            ? Vec2{midpoint.x - dy / length * 0.25, midpoint.y + dx / length * 0.25}
            : midpoint;
        auto dimension = encode_boundary_dimension_entity(
            BoundaryDimension{proposal.id, target.id, segment->segment_id, text_position,
                              BoundaryDimensionPlacement::manual, std::nullopt});
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"}) {
            if (target.properties.contains(key)) dimension.properties[key] = target.properties.at(key);
        }
        std::vector<EntityChange> changes;
        if (target.properties != original_target.properties || target.extensions != original_target.extensions) {
            changes.push_back(EntityChange::upsert(std::move(target)));
        }
        changes.push_back(EntityChange::upsert(std::move(dimension)));
        const ApplyEntityChanges command{source.revision(), std::move(changes), {},
                                         "Accept assisted dimension"};
        (void)Document::preview_command(source, command);
        applyDocumentCommand(command);
        m_selected_id = id_from(proposal.id);
        refresh();
        return true;
    }

    [[nodiscard]] bool acceptAssistanceProposal(const AssistanceProposal& proposal) {
        try {
            const auto request = m_assistance_session.request_acceptance(
                proposal, true, default_assistance_resource_ids());
            if (request.proposal.preview.command_type == "add_label") {
                return applyAssistedLabel(request.proposal);
            }
            if (request.proposal.preview.command_type == "add_boundary") {
                return applyAssistedBoundary(request.proposal);
            }
            if (request.proposal.preview.command_type == "add_dimension_suggestion") {
                return applyAssistedDimension(request.proposal);
            }
            if (request.proposal.preview.command_type == "set_workspace") {
                const auto workspace = request.proposal.preview.arguments.value("workspace", "");
                if (workspace == "measurement") setWorkspace(Workspace::measurement);
                else if (workspace == "architectural") setWorkspace(Workspace::architectural);
                else throw std::invalid_argument("Assisted workspace value is unknown.");
                clearError();
                return true;
            }
            throw std::invalid_argument("Assisted command type is unsupported.");
        } catch (const std::exception& error) {
            setError(QStringLiteral("Accept assistance: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool beginBoundaryDrawing(BoundaryAuthoringMode mode, QString classification) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This project is read-only."));
            return false;
        }
        // Starting a fresh authoring session clears any pending redraw target;
        // the explicit redefinition command reinstates it after this setup
        // succeeds.
        m_redefine_boundary_id.reset();
        const auto context = requireDrawingContext();
        if (!context || !confirmDiscardBoundaryDraft()) return false;
        const auto modal_context = captureModalContext();
        const auto original_session = m_boundary_session
            ? std::optional{m_boundary_session->view()} : std::nullopt;
        try {
            if (mode == BoundaryAuthoringMode::define_first && classification.trimmed().isEmpty()) {
                const auto selected = chooseBoundaryClassification(*context);
                if (!selected) return false;
                classification = *selected;
                if (!modalContextUnchanged(modal_context)) return false;
                if (original_session.has_value() != m_boundary_session.has_value() ||
                    (original_session && !(m_boundary_session->view() == *original_session))) {
                    setError(QStringLiteral("The unfinished boundary changed while choosing its classification. Start the command again."));
                    return false;
                }
            }
            BoundaryAuthoringOptions options;
            options.automatic_dimension_placement = mode == BoundaryAuthoringMode::draw_first;
            BoundaryAuthoringSession candidate(mode, options);
            if (!classification.trimmed().isEmpty())
                candidate.set_classification(classification.trimmed().toStdString());
            clearPreview();
            m_boundary_session = std::move(candidate);
            m_boundary_source = m_document->snapshot();
            m_boundary_context = *context;
            m_boundary_document = m_document;
            m_tool = CanvasTool::boundary;
            syncToolControls();
            clearError();
            boundaryDraftChanged();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Start boundary: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    QString createBoundary(const Boundary& boundary, const QString& classification,
                           std::optional<Revision> expected_revision = std::nullopt,
                           json extensions = json::object()) {
        const auto revision = expected_revision.value_or(m_document->revision());
        const auto drawing_context = requireDrawingContext();
        if (!drawing_context) return {};
        const auto diagnostics = validate_boundary(boundary);
        if (!diagnostics.empty()) {
            setError(QStringLiteral("Boundary rejected: %1").arg(
                QString::fromStdString(diagnostics.front().message)));
            return {};
        }
        const auto entity_id = new_id("boundary");
        const auto id = id_from(entity_id);
        auto entity = Entity{entity_id,
                             "measurement_boundary",
                             json{{"floor_id", drawing_context->floor_id},
                                  {"layer_id", drawing_context->layer_id},
                                  {"segments", boundary_json(boundary)},
                                  {"classification", classification.toStdString()},
                                  {"factor", 1.0},
                                  {"factor_expression", "1"},
                                  {"factor_numerator", 1},
                                  {"factor_denominator", 1}},
                             false,
                             std::move(extensions)};
        // New authoring starts with stable segment/vertex identities so every
        // later edit can remain a single undoable semantic command. The
        // upgrade helper preserves the entered geometry and all metadata.
        entity = upgrade_legacy_boundary_entity(entity);
        if (classification == QStringLiteral("survey")) entity.properties["calculation_scope"] = "site";
        if (!applyEntity(std::move(entity), "create measurement boundary", revision)) {
            return {};
        }
        m_selected_id = id;
        refresh();
        return id;
    }

    QString createRoomBoundary(const Boundary& boundary, const QString& classification,
                               std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        const auto drawing_context = requireDrawingContext();
        if (!drawing_context) return {};
        const auto diagnostics = validate_boundary(boundary);
        if (!diagnostics.empty()) {
            setError(QStringLiteral("Room boundary rejected: %1").arg(
                QString::fromStdString(diagnostics.front().message)));
            return {};
        }
        const auto area = std::abs(signed_area(boundary));
        if (!std::isfinite(area) || area <= default_geometry_tolerance_metres) {
            setError(QStringLiteral("Room boundary must enclose a measurable area."));
            return {};
        }
        const auto entity_id = new_id("room-boundary");
        auto entity = Entity{entity_id,
                             "room_boundary",
                             json{{"floor_id", drawing_context->floor_id},
                                  {"layer_id", drawing_context->layer_id},
                                  {"segments", boundary_json(boundary)},
                                  {"boundary", boundary_json(boundary)},
                                  {"name", classification.toStdString()},
                                  {"classification", classification.toStdString()},
                                  {"area_m2", area},
                                  {"factor", 1.0},
                                  {"factor_expression", "1"},
                                  {"factor_numerator", 1},
                                  {"factor_denominator", 1}},
                             false,
                             json::object()};
        // Keep the legacy auxiliary boundary array for consumers that still
        // inspect it, while promoting the canonical segments array to the
        // identified v1 model used by editing and calculation paths.
        const auto auxiliary_boundary = entity.properties.at("boundary");
        entity.properties.erase("boundary");
        entity = upgrade_legacy_boundary_entity(entity);
        entity.properties["boundary"] = auxiliary_boundary;
        if (!applyEntity(std::move(entity), "create room boundary", revision)) {
            return {};
        }
        m_selected_id = id_from(entity_id);
        refresh();
        return m_selected_id;
    }

    QString createRoomBoundaryFromExistingGeometry(const QString& classification,
                                                   std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        if (revision != m_document->revision()) {
            setError(QStringLiteral(
                "The project changed while existing geometry was being inspected. Start the command again."));
            return {};
        }
        try {
            const auto selected = selectedEntity();
            if (!selected.has_value()) {
                throw std::invalid_argument("Select existing walls or a closed boundary first.");
            }
            Boundary boundary;
            if (is_closed_boundary_entity(selected->type)) {
                boundary = read_boundary(selected->properties);
            } else if (selected->type == "wall") {
                const auto floor_id = read_string(selected->properties, "floor_id");
                const auto layer_id = read_string(selected->properties, "layer_id");
                if (!floor_id.has_value() || !layer_id.has_value()) {
                    throw std::invalid_argument("The selected wall has no floor or layer context.");
                }
                const auto selected_segment = read_required_segment(selected->properties, "baseline");
                if (!selected_segment.has_value()) {
                    throw std::invalid_argument("The selected wall has no valid analytical baseline.");
                }
                struct Candidate {
                    std::string id;
                    Segment segment;
                };
                std::vector<Candidate> candidates;
                std::size_t selected_index = 0;
                const auto snapshot = m_document->snapshot();
                for (const auto& [id, entity] : snapshot.entities()) {
                    if (entity.type != "wall" ||
                        read_string(entity.properties, "floor_id") != floor_id ||
                        read_string(entity.properties, "layer_id") != layer_id) {
                        continue;
                    }
                    const auto baseline = read_required_segment(entity.properties, "baseline");
                    if (!baseline.has_value()) continue;
                    if (entity.id == selected->id) selected_index = candidates.size();
                    candidates.push_back({id, *baseline});
                }
                if (candidates.empty() || selected_index >= candidates.size() ||
                    candidates[selected_index].id != selected->id) {
                    throw std::invalid_argument("The selected wall is not part of the current drawing context.");
                }
                const auto touches = [](const Segment& left, const Segment& right) {
                    const auto same_point = [](Vec2 a, Vec2 b) {
                        return a.x == b.x && a.y == b.y;
                    };
                    return same_point(left.start, right.start) ||
                           same_point(left.start, right.end) ||
                           same_point(left.end, right.start) ||
                           same_point(left.end, right.end);
                };
                std::vector<bool> included(candidates.size(), false);
                std::vector<std::size_t> component;
                component.reserve(candidates.size());
                included[selected_index] = true;
                component.push_back(selected_index);
                for (std::size_t cursor = 0; cursor < component.size(); ++cursor) {
                    const auto source_index = component[cursor];
                    for (std::size_t index = 0; index < candidates.size(); ++index) {
                        if (!included[index] && touches(candidates[source_index].segment,
                                                        candidates[index].segment)) {
                            included[index] = true;
                            component.push_back(index);
                        }
                    }
                }
                std::vector<Segment> segments;
                segments.reserve(component.size());
                std::size_t component_seed = 0;
                for (std::size_t index = 0; index < component.size(); ++index) {
                    segments.push_back(candidates[component[index]].segment);
                    if (component[index] == selected_index) component_seed = index;
                }
                boundary = assemble_boundary_from_segments(segments, component_seed);
            } else {
                throw std::invalid_argument(
                    "Select existing walls or a closed boundary before creating a room.");
            }
            const auto diagnostics = validate_boundary(boundary);
            if (!diagnostics.empty()) {
                throw std::invalid_argument("Existing geometry is invalid: " +
                                            diagnostics.front().message);
            }
            const auto name = classification.trimmed();
            if (name.isEmpty()) throw std::invalid_argument("Room name or classification cannot be empty.");
            return createRoomBoundary(boundary, name, revision);
        } catch (const std::exception& error) {
            setError(QStringLiteral("Create room from existing geometry: %1")
                         .arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    QStringList detectRoomBoundariesFromExistingWalls(
        const QString& classification, std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        if (revision != m_document->revision()) {
            setError(QStringLiteral(
                "The project changed while existing geometry was being inspected. Start the command again."));
            return {};
        }
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This project is read-only."));
            return {};
        }
        try {
            const auto selected = selectedEntity();
            if (!selected.has_value() || selected->type != "wall") {
                throw std::invalid_argument("Select a wall in the floor and layer to inspect.");
            }
            const auto floor_id = read_string(selected->properties, "floor_id");
            const auto layer_id = read_string(selected->properties, "layer_id");
            if (!floor_id.has_value() || !layer_id.has_value() || floor_id->empty() || layer_id->empty()) {
                throw std::invalid_argument("The selected wall has no floor or layer context.");
            }
            const auto name = classification.trimmed();
            if (name.isEmpty()) throw std::invalid_argument("Room classification cannot be empty.");

            const auto source = authoringSnapshot();
            const auto organization = organize_project(source);
            const auto context = organization.drawing_context(*layer_id);
            if (!context.has_value() || context->floor_id != *floor_id) {
                throw std::invalid_argument("The selected wall has no resolved drawing context.");
            }
            std::vector<Segment> segments;
            for (const auto& [id, entity] : source.entities()) {
                (void)id;
                if (entity.type != "wall" ||
                    read_string(entity.properties, "floor_id") != floor_id ||
                    read_string(entity.properties, "layer_id") != layer_id) {
                    continue;
                }
                const auto baseline = read_required_segment(entity.properties, "baseline");
                if (!baseline.has_value()) {
                    throw std::invalid_argument("A wall in the selected drawing context has no valid baseline.");
                }
                segments.push_back(*baseline);
            }
            const auto faces = detect_closed_boundaries(segments);
            if (faces.empty()) {
                throw std::invalid_argument("No closed area was found in the selected wall graph.");
            }

            QStringList created_ids;
            std::vector<EntityChange> changes;
            changes.reserve(faces.size());
            for (const auto& face : faces) {
                const auto area = std::abs(signed_area(face));
                if (!std::isfinite(area) || area <= default_geometry_tolerance_metres) {
                    throw std::invalid_argument("Detected area is not measurable.");
                }
                const auto entity_id = new_id("room-boundary");
                auto entity = Entity{entity_id,
                                     "room_boundary",
                                     json{{"property_id", context->property_id},
                                          {"building_id", context->building_id},
                                          {"floor_id", context->floor_id},
                                          {"layer_id", context->layer_id},
                                          {"segments", boundary_json(face)},
                                          {"boundary", boundary_json(face)},
                                          {"name", name.toStdString()},
                                          {"classification", name.toStdString()},
                                          {"area_m2", area},
                                          {"factor", 1.0},
                                          {"factor_expression", "1"},
                                          {"factor_numerator", 1},
                                          {"factor_denominator", 1}},
                                     false,
                                     json::object()};
                const auto auxiliary = entity.properties.at("boundary");
                entity.properties.erase("boundary");
                entity = upgrade_legacy_boundary_entity(entity);
                entity.properties["boundary"] = auxiliary;
                created_ids.push_back(id_from(entity_id));
                changes.push_back(EntityChange::upsert(std::move(entity)));
            }
            const ApplyEntityChanges command{source.revision(), std::move(changes), {},
                                             "Detect room boundaries"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = created_ids.front();
            clearError();
            refresh();
            return created_ids;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Detect areas: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    void createRoomBoundaryFromSelection() {
        const auto context = captureModalContext();
        const auto selected = selectedEntity();
        if (!selected || (!is_closed_boundary_entity(selected->type) && selected->type != "wall")) {
            setError(QStringLiteral("Select existing walls or a closed boundary first."));
            return;
        }
        const auto initial = QString::fromStdString(
            read_string(selected->properties, "classification").value_or("room"));
        bool accepted = false;
        const auto classification = QInputDialog::getText(
            owner, QStringLiteral("Create room boundary"),
            QStringLiteral("Room name or classification:"), QLineEdit::Normal,
            initial, &accepted);
        if (!accepted || classification.trimmed().isEmpty()) return;
        if (!modalContextUnchanged(context)) return;
        (void)createRoomBoundaryFromExistingGeometry(classification.trimmed(), context.revision);
    }

    QString createStraightWall(Vec2 start, Vec2 end, const QString& classification,
                               std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        const auto drawing_context = requireDrawingContext();
        if (!drawing_context) return {};
        if (!std::isfinite(start.x) || !std::isfinite(start.y) || !std::isfinite(end.x) ||
            !std::isfinite(end.y) || std::hypot(end.x - start.x, end.y - start.y) <= 1e-7) {
            setError(QStringLiteral("Wall endpoints must be finite and distinct."));
            return {};
        }
        const auto entity_id = new_id("wall");
        const auto id = id_from(entity_id);
        const auto properties = json{{"floor_id", drawing_context->floor_id},
                                     {"layer_id", drawing_context->layer_id},
                                     {"baseline", segment_json(Segment{start, end, 0.0})},
                                     {"thickness_m", 0.14},
                                     {"height_m", 2.4384},
                                     {"elevation_m", 0.0},
                                     {"classification", classification.toStdString()}};
        if (!applyEntity(Entity{entity_id, "wall", properties, false, json::object()},
                         "create straight wall", revision)) {
            return {};
        }
        m_selected_id = id;
        refresh();
        return id;
    }

    QString commitBuildingObject(Entity candidate, std::uint64_t expected_revision,
                                 bool replace_selected) {
        try {
            if (!m_document->is_editable())
                throw std::runtime_error("This document is read-only.");
            const auto snapshot = m_document->snapshot();
            auto canonical = encode_building_entity(decode_building_entity(candidate),
                                                    candidate.extensions);
            if (replace_selected) {
                const auto original = selectedEntity();
                if (!original || original->id != candidate.id || original->type != candidate.type)
                    throw std::runtime_error("Select the original object before applying this edit.");
                const auto entries = merged_quantity_entries(&*original, candidate, canonical.properties);
                // Geometry editors must preserve metadata they do not understand.
                candidate = *original;
                candidate.properties.update(canonical.properties);
                if (candidate.type == "roof") {
                    if (!canonical.properties.contains("roof_openings")) candidate.properties.erase("roof_openings");
                    if (canonical.extensions.contains("roof_opening_input"))
                        candidate.extensions["roof_opening_input"] = canonical.extensions.at("roof_opening_input");
                }
                if (entries) candidate.properties["quantity_entries"] = *entries;
            } else {
                if (snapshot.entities().contains(candidate.id))
                    throw std::runtime_error("An object with this identity already exists.");
                const auto entries = merged_quantity_entries(nullptr, candidate, canonical.properties);
                candidate.properties.update(canonical.properties);
                if (entries) candidate.properties["quantity_entries"] = *entries;
                if (!assignDrawingContext(candidate.properties)) return {};
            }
            const auto id = id_from(candidate.id);
            applyDocumentCommand(ApplyEntityChanges{
                .expected_revision = expected_revision,
                .entity_changes = {EntityChange::upsert(std::move(candidate))},
                .message = replace_selected ? "edit building object" : "create building object",
            });
            m_selected_id = id;
            clearError();
            refresh();
            return id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Building object: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    QString createHostedOpening(const QString& kind,
                                const QString& offset_expression,
                                const QString& width_expression,
                                const QString& sill_expression,
                                const QString& height_expression,
                                std::optional<Revision> expected_revision = std::nullopt,
                                std::optional<DoorOperation> door_operation = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        if (revision != m_document->revision()) {
            setError(QStringLiteral("The project changed while the opening was being entered. Start the opening again."));
            return {};
        }
        const auto wall_entity = selectedEntity();
        if (!wall_entity.has_value() || wall_entity->type != "wall") {
            setError(QStringLiteral("Select a wall before creating a door or window."));
            return {};
        }
        const auto normalized_kind = kind.trimmed().toLower();
        if (normalized_kind != QStringLiteral("door") &&
            normalized_kind != QStringLiteral("window")) {
            setError(QStringLiteral("Opening type must be Door or Window."));
            return {};
        }
        try {
            const auto unit = m_metric_units ? Unit::metre : Unit::foot;
            const auto offset = parse_quantity(offset_expression.toStdString(), unit).metres;
            const auto width = parse_quantity(width_expression.toStdString(), unit).metres;
            const auto sill = parse_quantity(sill_expression.toStdString(), unit).metres;
            const auto height = parse_quantity(height_expression.toStdString(), unit).metres;
            if (!std::isfinite(offset) || offset < 0.0) {
                setError(QStringLiteral("Opening offset must be zero or greater."));
                return {};
            }
            if (!std::isfinite(width) || width <= 1e-7) {
                setError(QStringLiteral("Opening width must be greater than zero."));
                return {};
            }
            if (!std::isfinite(sill) || sill < 0.0) {
                setError(QStringLiteral("Opening sill must be zero or greater."));
                return {};
            }
            if (!std::isfinite(height) || height <= 1e-7) {
                setError(QStringLiteral("Opening height must be greater than zero."));
                return {};
            }

            const auto snapshot = m_document->snapshot();
            std::vector<HostedOpening> openings;
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type != "opening") {
                    continue;
                }
                const auto existing_wall_id = read_string(entity.properties, "wall_id");
                if (!existing_wall_id.has_value() || existing_wall_id.value() != wall_entity->id) {
                    continue;
                }
                QString opening_error;
                const auto opening = read_hosted_opening(entity, &opening_error);
                if (!opening.has_value()) {
                    setError(QStringLiteral("Opening preview rejected: %1").arg(opening_error));
                    return {};
                }
                openings.push_back(*opening);
            }
            const auto entity_id = new_id("opening");
            openings.push_back(HostedOpening{entity_id, offset, width, sill, height});
            if (!previewWall(*wall_entity, openings, QStringLiteral("Opening preview"))) {
                return {};
            }

            auto properties = json{{"wall_id", wall_entity->id},
                                         {"offset_m", offset},
                                         {"width_m", width},
                                         {"sill_m", sill},
                                         {"height_m", height},
                                         {"opening_kind", normalized_kind.toStdString()},
                                         {"classification", normalized_kind.toStdString()}};
            if(door_operation) properties["door_operation"] = encode_door_operation(*door_operation);
            if (!applyEntity(Entity{entity_id, "opening", properties, false, json::object()},
                             "create hosted opening", revision)) {
                return {};
            }
            m_selected_id = id_from(entity_id);
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Opening: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    QString createSlabFromSelectedBoundary(const QString& thickness_expression,
                                            const QString& elevation_expression,
                                            std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        const auto entity = selectedEntity();
        if (!entity.has_value() || !is_closed_boundary_entity(entity->type)) {
            setError(QStringLiteral("Select a closed boundary before creating a slab."));
            return {};
        }
        const auto organization = organize_project(m_document->snapshot());
        const auto source_context = organization.drawing_context(entity->id);
        const auto active_context = organization.drawing_context(m_active_layer_id.toStdString());
        if (!source_context || !active_context || *source_context != *active_context) {
            setError(QStringLiteral("Choose the selected boundary's drawing layer before creating its slab."));
            return {};
        }
        const auto boundary = read_boundary(entity->properties);
        if (boundary.empty()) {
            setError(QStringLiteral("The selected boundary has no valid segments."));
            return {};
        }
        return createSlabFromBoundary(boundary, thickness_expression, elevation_expression, {}, revision);
    }

    QString createSlabFromBoundary(const Boundary& boundary,
                                   const QString& thickness_expression,
                                   const QString& elevation_expression,
                                   std::vector<Boundary> holes = {},
                                   std::optional<Revision> expected_revision = std::nullopt) {
        const auto revision = expected_revision.value_or(m_document->revision());
        const auto drawing_context = requireDrawingContext();
        if (!drawing_context) return {};
        const auto diagnostics = validate_boundary(boundary);
        if (!diagnostics.empty()) {
            setError(QStringLiteral("Slab boundary rejected: %1").arg(
                QString::fromStdString(diagnostics.front().message)));
            return {};
        }
        for (const auto& hole : holes) {
            const auto hole_diagnostics = validate_boundary(hole);
            if (!hole_diagnostics.empty()) {
                setError(QStringLiteral("Slab opening rejected: %1").arg(
                    QString::fromStdString(hole_diagnostics.front().message)));
                return {};
            }
        }
        try {
            const auto unit = m_metric_units ? Unit::metre : Unit::foot;
            const auto thickness =
                parse_quantity(thickness_expression.toStdString(), unit).metres;
            const auto elevation =
                parse_quantity(elevation_expression.toStdString(), unit).metres;
            if (!std::isfinite(thickness) || thickness <= 1e-7) {
                setError(QStringLiteral("Slab thickness must be greater than zero."));
                return {};
            }
            if (!std::isfinite(elevation)) {
                setError(QStringLiteral("Slab elevation must be finite."));
                return {};
            }
            const auto entity_id = new_id("slab");
            const auto preview = Slab{entity_id, boundary, holes, thickness, elevation};
            try {
                (void)make_slab(preview);
            } catch (const std::exception& error) {
                setError(QStringLiteral("Slab preview rejected: %1")
                             .arg(QString::fromUtf8(error.what())));
                return {};
            }

            json holes_json = json::array();
            for (const auto& hole : holes) {
                holes_json.push_back(boundary_json(hole));
            }
            const auto properties = json{{"floor_id", drawing_context->floor_id},
                                         {"layer_id", drawing_context->layer_id},
                                         {"boundary", boundary_json(boundary)},
                                         {"holes", std::move(holes_json)},
                                         {"thickness_m", thickness},
                                         {"elevation_m", elevation},
                                         {"classification", "slab"}};
            if (!applyEntity(Entity{entity_id, "slab", properties, false, json::object()},
                             "create slab", revision)) {
                return {};
            }
            m_selected_id = id_from(entity_id);
            refresh();
            return m_selected_id;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Slab: %1").arg(QString::fromUtf8(error.what())));
            return {};
        }
    }

    bool selectEntity(const QString& entity_id) {
        if (entity_id.isEmpty()) {
            m_selected_id.clear();
            refresh();
            return true;
        }
        const auto snapshot = m_document->snapshot();
        bool annotation_child = false;
        if (!has_entity(*m_document, entity_id)) {
            for (const auto& [id, entity] : snapshot.entities()) {
                (void)id;
                if (entity.type != kAnnotationEntityType) continue;
                try {
                    const auto state = decode_annotation_entity(entity);
                    annotation_child = std::any_of(
                        state.labels.begin(), state.labels.end(), [&](const auto& label) {
                            return label.id == entity_id.toStdString();
                        }) || std::any_of(
                        state.symbols.begin(), state.symbols.end(), [&](const auto& symbol) {
                            return symbol.id == entity_id.toStdString();
                        });
                } catch (const std::exception&) {
                    annotation_child = false;
                }
                if (annotation_child) break;
            }
        }
        if (!has_entity(*m_document, entity_id) && !annotation_child) {
            setError(QStringLiteral("No entity named %1 exists in this document.").arg(entity_id));
            return false;
        }
        m_selected_id = entity_id;
        const auto organization = organize_project(snapshot);
        if (annotation_child) {
            refresh();
            return true;
        }
        if (const auto context = organization.drawing_context(entity_id.toStdString())) {
            m_active_layer_id = id_from(context->layer_id);
        } else {
            const auto& node = organization.nodes.at(entity_id.toStdString());
            if (node.type == "property" || node.type == "building" || node.type == "floor") {
                m_active_layer_id.clear();
                for (const auto& [id, candidate] : organization.nodes) {
                    if (candidate.type != "layer") continue;
                    const auto layer_context = organization.drawing_context(id);
                    if (layer_context && (layer_context->property_id == node.id ||
                                          layer_context->building_id == node.id || layer_context->floor_id == node.id)) {
                        if (!m_active_layer_id.isEmpty()) {
                            m_active_layer_id.clear();
                            break;
                        }
                        m_active_layer_id = id_from(id);
                    }
                }
            }
        }
        refresh();
        return true;
    }

    bool copySelection() {
        try {
            const auto snapshot = authoringSnapshot();
            auto entities = clipboard_entities_for_selection(
                snapshot, m_selected_id.toStdString());
            if (entities.empty()) {
                throw std::invalid_argument(
                    "Select supported geometry, an area, an architectural object, or annotations.");
            }
            std::map<std::string, std::set<std::string>> material_dependencies;
            for(const auto& entity : entities) {
                if(!entity.properties.contains("material_assignment")) continue;
                const auto& assignment = entity.properties.at("material_assignment");
                material_dependencies[assignment.at("catalog_id").get<std::string>()].insert(
                    assignment.at("material_id").get<std::string>());
            }
            for(const auto& [catalog_id, material_ids] : material_dependencies) {
                const auto catalog = AssemblyModel::from_json(snapshot.entities().at(catalog_id).properties.at("model"));
                std::vector<AssemblyMaterial> used;
                for(const auto& material : catalog.materials())
                    if(material_ids.contains(material.id)) used.push_back(material);
                entities.push_back(Entity{catalog_id,"assembly_model",{{"version",1},
                    {"model",AssemblyModel::create(std::move(used),{},{}).to_json()}},false,json::object()});
            }
            if(entities.size()>kMaximumClipboardEntities)
                throw std::invalid_argument("Clipboard material dependencies exceed the entity limit.");
            json payload{{"format", std::string(kClipboardFormat)}, {"version", 1}, {"root_id",entities.front().id},
                         {"entities", json::array()}};
            for (const auto& entity : entities) {
                payload["entities"].push_back(clipboard_entity_json(entity));
            }
            const auto encoded = payload.dump();
            if (encoded.size() > kMaximumClipboardBytes) {
                throw std::invalid_argument("The selected geometry graph is too large for the clipboard.");
            }
            auto* clipboard = QGuiApplication::clipboard();
            if (clipboard == nullptr) {
                throw std::runtime_error("The system clipboard is unavailable.");
            }
            clipboard->setText(QString::fromUtf8(encoded.data(),
                                                  static_cast<int>(encoded.size())),
                               QClipboard::Clipboard);
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Copy: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void showSurveyCalculator() {
        auto drawing_context = captureModalContext();
        QDialog dialog(owner);
        dialog.setObjectName(QStringLiteral("surveyCalculator"));
        dialog.setWindowTitle(QStringLiteral("Survey traverse"));
        dialog.resize(900, 600);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* provenance = new QLineEdit(&dialog);
        provenance->setObjectName(QStringLiteral("surveyProvenance"));
        form->addRow(QStringLiteral("Source / reference"), provenance);
        auto* tolerance = new QLineEdit(QStringLiteral("0.001 m"), &dialog);
        tolerance->setObjectName(QStringLiteral("surveyTolerance"));
        form->addRow(QStringLiteral("Closure tolerance"), tolerance);
        auto* input_units = new QComboBox(&dialog);
        input_units->setObjectName(QStringLiteral("surveyInputUnits"));
        input_units->addItem(QStringLiteral("Feet"), QStringLiteral("ft"));
        input_units->addItem(QStringLiteral("Metres"), QStringLiteral("m"));
        input_units->setCurrentIndex(m_metric_units ? 1 : 0);
        form->addRow(QStringLiteral("Unsuffixed distances"), input_units);
        layout->addLayout(form);
        auto* help = new QLabel(QStringLiteral(
            "One leg per line: quadrant, angle, distance with units.\n"
            "Example: NE, 45:30:15, 100 ft. Angles accept decimal degrees or degrees:minutes:seconds.\n"
            "Quadrants: NE, SE, SW, NW; angles: 0–90°.\n"
            "Coordinates start at a local origin. No closure adjustment is applied.\n"
            "Add boundary retains measured legs and adds a closing segment to the origin if needed. "
            "Cyan previews the proposed closing leg."), &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* input = new QPlainTextEdit(&dialog);
        input->setObjectName(QStringLiteral("surveyLegs"));
        input->setAccessibleName(QStringLiteral("Survey bearing and distance legs"));
        auto* entry_split = new QSplitter(Qt::Horizontal, &dialog);
        entry_split->addWidget(input);
        auto* preview = new PlanCanvas(entry_split);
        preview->setObjectName(QStringLiteral("surveyPreview"));
        preview->setAccessibleName(QStringLiteral("Measured survey traverse preview"));
        preview->setTool(CanvasTool::select);
        preview->setOverviewMapEnabled(false);
        preview->setSnapEnabled(false);
        preview->setMinimumSize(280, 220);
        entry_split->addWidget(preview);
        entry_split->setSizes({340, 520});
        layout->addWidget(entry_split, 1);
        auto* result = new QLabel(&dialog);
        result->setObjectName(QStringLiteral("surveyResult"));
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(result);
        auto* buttons = new QDialogButtonBox(&dialog);
        auto* open_report = buttons->addButton(QStringLiteral("Open report…"), QDialogButtonBox::ActionRole);
        open_report->setObjectName(QStringLiteral("surveyOpen"));
        auto* calculate = buttons->addButton(QStringLiteral("Calculate"), QDialogButtonBox::ActionRole);
        calculate->setObjectName(QStringLiteral("surveyCalculate"));
        auto* export_report = buttons->addButton(QStringLiteral("Export report…"), QDialogButtonBox::ActionRole);
        export_report->setObjectName(QStringLiteral("surveyExport"));
        export_report->setEnabled(false);
        auto* add_boundary = buttons->addButton(QStringLiteral("Add boundary"), QDialogButtonBox::ActionRole);
        add_boundary->setObjectName(QStringLiteral("surveyAddBoundary"));
        add_boundary->setEnabled(false);
        auto* close_endpoint = new QCheckBox(QStringLiteral("Close the final leg at the origin (adjust its endpoint)"), &dialog);
        close_endpoint->setObjectName(QStringLiteral("surveyCloseEndpoint"));
        close_endpoint->setEnabled(false);
        layout->addWidget(close_endpoint);
        buttons->addButton(QDialogButtonBox::Close);
        layout->addWidget(buttons);
        std::optional<QString> report;
        const auto measured_boundary = [](const json& source) {
            Boundary boundary;
            const auto& vertices = source.at("vertices");
            const auto point = [](const json& v) {
                return Vec2{v.at("east_m").get<double>(), v.at("north_m").get<double>()};
            };
            for (std::size_t index = 1; index < vertices.size(); ++index)
                boundary.push_back({point(vertices[index - 1]), point(vertices[index]), 0.0});
            return boundary;
        };
        const auto refresh_preview = [&] {
            if (!report) { preview->setEntities({}); return; }
            const auto source = json::parse(report->toStdString());
            const auto measured = measured_boundary(source);
            std::vector<CanvasEntity> geometry{{"survey-measured", "measurement_boundary", measured}};
            if (!measured.empty() && !source.at("diagnostics").at("area_m2").is_null()) {
                const auto origin = measured.front().start;
                const auto end = measured.back().end;
                if (end.x != origin.x || end.y != origin.y) {
                    const auto from = close_endpoint->isChecked() ? measured.back().start : end;
                    geometry.push_back({"survey-proposed-closure", "measurement_boundary",
                                        {{from, origin, 0.0}}, 0.08, true});
                }
            }
            preview->setEntities(std::move(geometry));
            preview->setMetricUnits(input_units->currentData().toString() == "m");
            layout->activate();
            preview->fitView();
        };
        const auto invalidate = [&] {
            report.reset(); result->clear(); export_report->setEnabled(false); add_boundary->setEnabled(false);
            close_endpoint->setChecked(false);
            close_endpoint->setEnabled(false);
            preview->setEntities({});
        };
        QObject::connect(close_endpoint, &QCheckBox::toggled, &dialog, refresh_preview);
        QObject::connect(input, &QPlainTextEdit::textChanged, &dialog, invalidate);
        QObject::connect(provenance, &QLineEdit::textChanged, &dialog, invalidate);
        QObject::connect(tolerance, &QLineEdit::textChanged, &dialog, invalidate);
        QObject::connect(input_units, &QComboBox::currentIndexChanged, &dialog, invalidate);
        QObject::connect(calculate, &QPushButton::clicked, &dialog, [&] {
            invalidate();
            try {
                const auto unit = input_units->currentData().toString() == QStringLiteral("m") ? Unit::metre : Unit::foot;
                if (input->toPlainText().size() > 1024 * 1024)
                    throw std::invalid_argument("Survey input exceeds 1 MiB.");
                std::vector<SurveyLeg> legs;
                auto distance_entries = json::array();
                const auto lines = input->toPlainText().split('\n');
                int line_number = 0;
                for (const auto& line : lines) {
                    ++line_number;
                    if (line.trimmed().isEmpty()) continue;
                    try {
                        if (legs.size() >= SurveyTraverse::maximum_legs)
                            throw std::invalid_argument("Too many survey legs.");
                        const auto fields = line.split(',');
                        if (fields.size() != 3) throw std::invalid_argument("Use quadrant, angle, distance.");
                        const auto quadrant = fields[0].trimmed().toUpper();
                        const std::map<QString, BearingQuadrant> quadrants{
                            {"NE", BearingQuadrant::north_east}, {"SE", BearingQuadrant::south_east},
                            {"SW", BearingQuadrant::south_west}, {"NW", BearingQuadrant::north_west}};
                        if (!quadrants.contains(quadrant)) throw std::invalid_argument("Use NE, SE, SW, or NW.");
                        const auto angle = parse_survey_angle(fields[1].trimmed().toStdString());
                        const auto quantity = parse_quantity(fields[2].trimmed().toStdString(), unit);
                        const auto distance = quantity.metres;
                        if (distance <= 0) throw std::invalid_argument("Distance must be positive.");
                        legs.push_back({"leg-" + std::to_string(legs.size() + 1), quadrants.at(quadrant), angle, distance});
                        distance_entries.push_back({{"leg_id", legs.back().id},
                            {"line_number", line_number}, {"original_expression", quantity.original_expression},
                            {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                                              {"denominator", quantity.exact_metres.denominator}}}});
                    } catch (const std::exception& error) {
                        throw std::invalid_argument("Line " + std::to_string(line_number) + ": " + error.what());
                    }
                }
                const SurveyTraverse traverse(provenance->text().trimmed().toStdString(), std::move(legs),
                    parse_quantity(tolerance->text().trimmed().toStdString(), unit).metres);
                const auto& d = traverse.diagnostics();
                auto summary = QStringLiteral("%1\nClosure error: %2 m (east %3 m; north %4 m)\nPerimeter: %5 m")
                    .arg(d.closed ? QStringLiteral("Within closure tolerance") : QStringLiteral("Open traverse"))
                    .arg(d.linear_error_m, 0, 'g', 10).arg(d.east_error_m, 0, 'g', 10)
                    .arg(d.north_error_m, 0, 'g', 10).arg(d.perimeter_m, 0, 'g', 10);
                if (d.area_m2) summary += QStringLiteral("\nArea: %1 m² · %2 acres")
                    .arg(*d.area_m2, 0, 'f', 4).arg(*d.acres, 0, 'f', 6);
                else summary += QStringLiteral("\nArea unavailable for this traverse.");
                auto report_json = json::parse(traverse.serialize());
                report_json["input_provenance"] = {
                    {"version", 1}, {"default_unit", unit == Unit::metre ? "m" : "ft"},
                    {"legs_text", input->toPlainText().toStdString()},
                    {"source_text", provenance->text().toStdString()},
                    {"closure_tolerance_expression", tolerance->text().toStdString()},
                    {"distances", std::move(distance_entries)}};
                report = QString::fromStdString(report_json.dump(2));
                result->setText(summary);
                export_report->setEnabled(true);
                add_boundary->setEnabled(d.area_m2.has_value() && m_document->is_editable());
                close_endpoint->setEnabled(d.area_m2.has_value() && d.linear_error_m > 0.0);
                refresh_preview();
            } catch (const std::exception& error) { result->setText(QString::fromUtf8(error.what())); }
        });
        QObject::connect(add_boundary, &QPushButton::clicked, &dialog, [&] {
            if (!report) return;
            if (!modalContextUnchanged(drawing_context)) {
                result->setText(QStringLiteral("The project context changed. Reopen Survey traverse before adding geometry."));
                return;
            }
            try {
                const auto source = json::parse(report->toStdString());
                if (source.at("diagnostics").at("area_m2").is_null())
                    throw std::invalid_argument("An open traverse cannot become an area boundary.");
                auto boundary = measured_boundary(source);
                const auto start = boundary.front().start;
                const auto end = boundary.back().end;
                const bool adjust_endpoint = close_endpoint->isEnabled() && close_endpoint->isChecked();
                const bool closing_segment = !adjust_endpoint && (start.x != end.x || start.y != end.y);
                if (adjust_endpoint) boundary.back().end = start;
                if (closing_segment) boundary.push_back({end, start, 0.0});
                const auto id = createBoundary(boundary, QStringLiteral("survey"), drawing_context.revision,
                    {{"survey_source", {{"version", 1}, {"report", source},
                                        {"added_closing_segment", closing_segment},
                                        {"adjusted_final_endpoint", adjust_endpoint},
                                        {"endpoint_adjustment_m", adjust_endpoint ?
                                            json{{"east", -end.x}, {"north", -end.y}} : json(nullptr)}}}});
                if (id.isEmpty()) { result->setText(lastError()); return; }
                drawing_context = captureModalContext();
                fitView();
                result->setText(QStringLiteral("Survey boundary added. Original measurements are preserved in its source metadata."));
            } catch (const std::exception& error) { result->setText(QString::fromUtf8(error.what())); }
        });
        const auto restore_input = [&](const json& loaded) {
                if (!loaded.contains("version") || !loaded.at("version").is_number_integer() ||
                    loaded.at("version") != 1 || !loaded.contains("input_provenance"))
                    throw std::invalid_argument("This report has no supported editable survey input.");
                const auto& entry = loaded.at("input_provenance");
                if (!entry.contains("version") || !entry.at("version").is_number_integer() || entry.at("version") != 1)
                    throw std::invalid_argument("Unsupported survey input version.");
                const auto legs_text = entry.at("legs_text").get<std::string>();
                const auto source_text = entry.at("source_text").get<std::string>();
                const auto tolerance_text = entry.at("closure_tolerance_expression").get<std::string>();
                const auto default_unit = entry.at("default_unit").get<std::string>();
                if (legs_text.size() > 1024 * 1024 || source_text.size() > 4096 || tolerance_text.size() > 4096 ||
                    (default_unit != "m" && default_unit != "ft"))
                    throw std::invalid_argument("Invalid survey input size or units.");
                // Stored vertices, diagnostics, and receipts are never trusted as
                // calculation inputs. Rebuild everything from the entered text.
                input->setPlainText(QString::fromStdString(legs_text));
                provenance->setText(QString::fromStdString(source_text));
                tolerance->setText(QString::fromStdString(tolerance_text));
                input_units->setCurrentIndex(default_unit == "m" ? 1 : 0);
                calculate->click();
        };
        QObject::connect(open_report, &QPushButton::clicked, &dialog, [&] {
            const auto path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Open survey report"),
                QString(), QStringLiteral("Survey report (*.json)"));
            if (path.isEmpty()) return;
            try {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) throw std::invalid_argument("Could not read the survey report.");
                constexpr qint64 limit = 4 * 1024 * 1024;
                const auto bytes = file.read(limit + 1);
                if (bytes.size() > limit) throw std::invalid_argument("Survey report exceeds 4 MiB.");
                restore_input(json::parse(bytes.toStdString()));
            } catch (const std::exception& error) {
                result->setText(QStringLiteral("Open report: %1").arg(QString::fromUtf8(error.what())));
            }
        });
        QObject::connect(export_report, &QPushButton::clicked, &dialog, [&] {
            if (!report) return;
            const auto captured_report = *report;
            const auto path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("Export survey report"),
                QString(), QStringLiteral("Survey report (*.json)"));
            if (path.isEmpty()) return;
            if (!report || *report != captured_report) {
                result->setText(QStringLiteral("Survey input changed. Calculate again before exporting."));
                return;
            }
            QSaveFile file(path);
            const auto bytes = captured_report.toUtf8();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                result->setText(QStringLiteral("Could not save the survey report: %1").arg(file.errorString()));
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (const auto selected = selectedEntity(); selected && selected->extensions.contains("survey_source")) {
            try {
                const auto& source = selected->extensions.at("survey_source");
                if (!source.contains("version") || !source.at("version").is_number_integer() || source.at("version") != 1)
                    throw std::invalid_argument("Unsupported stored survey source version.");
                restore_input(source.at("report"));
                dialog.setWindowTitle(QStringLiteral("Survey traverse — original source"));
                help->setText(help->text() + QStringLiteral(
                    "\nLoaded original calls from the selected boundary. Later drawing edits are not part of these calls."));
            } catch (const std::exception& error) {
                result->setText(QStringLiteral("Stored survey source: %1").arg(QString::fromUtf8(error.what())));
            }
        }
        (void)dialog.exec();
    }

    void refreshRoofDimensionPreview() {
        if (!m_roof_edit_context || m_roof_properties_group->isHidden()) return;
        const auto& context = *m_roof_edit_context;
        try {
            if (m_document != context.document || m_document->revision() != context.revision ||
                m_selected_id != context.selected_id || m_active_layer_id != context.layer_id ||
                m_metric_units != context.metric_units) {
                throw std::invalid_argument("The roof editing context changed. Reselect the roof.");
            }
            const auto original = selectedEntity();
            if (!original) throw std::invalid_argument("Reselect the roof to preview dimensions.");
            const auto roof_form = read_string(original->properties, "form");
            const bool symmetric_roof = roof_form == std::optional<std::string>("gable_roof") ||
                               roof_form == std::optional<std::string>("hip_roof");
            const auto read = [&](QLineEdit* field, const QString& initial, const char* key,
                                  const char* label, bool allow_zero) {
                double value;
                // Untouched rounded display values must not change the preview's
                // geometry any more than they change the committed geometry.
                if (field->text().trimmed() == initial.trimmed()) {
                    value = original->properties.at(key).get<double>();
                } else {
                    try {
                        value = parse_quantity(field->text().trimmed().toStdString(),
                            m_metric_units ? Unit::metre : Unit::foot).metres;
                    } catch (const std::exception&) {
                        throw std::invalid_argument(std::string(label) + ": enter a valid measurement.");
                    }
                }
                if (!std::isfinite(value) || (allow_zero ? value < 0.0 : value <= 0.0)) {
                    throw std::invalid_argument(std::string(label) +
                        (allow_zero ? " must be zero or greater." : " must be greater than zero."));
                }
                return value;
            };
            const auto run = read(m_roof_run_edit, m_roof_run_original_text,
                symmetric_roof ? "length_m" : "run_m", symmetric_roof ? "Length" : "Run", false);
            const auto span = read(m_roof_span_edit, m_roof_span_original_text, "span_m", "Span", false);
            if (roof_form == std::optional<std::string>("hip_roof") && run < span)
                throw std::invalid_argument("Hip roof length must be at least its span.");
            const auto rise = read(m_roof_rise_edit, m_roof_rise_original_text, "rise_m", "Rise", !symmetric_roof);
            (void)read(m_roof_overhang_edit, m_roof_overhang_original_text, "overhang_m", "Overhang", true);
            (void)read(m_roof_thickness_edit, m_roof_thickness_original_text, "thickness_m", "Thickness", false);
            const auto pitch = std::atan(rise / (symmetric_roof ? span / 2.0 : run));
            m_roof_pitch_value->setText(QStringLiteral("%1°").arg(pitch * 180.0 / std::numbers::pi, 0, 'f', 3));
            m_roof_preview_error->clear();
            m_roof_preview_error->hide();
        } catch (const std::exception& error) {
            m_roof_pitch_value->setText(QStringLiteral("—"));
            m_roof_preview_error->setText(QString::fromUtf8(error.what()));
            m_roof_preview_error->show();
        }
    }

    bool editSelectedRoofDimensions(const QString& run_text,
                                         const QString& span_text,
                                         const QString& rise_text,
                                         const QString& overhang_text,
                                         const QString& thickness_text) {
        if (!m_roof_edit_context || !modalContextUnchanged(*m_roof_edit_context)) {
            setError(QStringLiteral("The roof editing context changed. Reselect the roof before applying dimensions."));
            return false;
        }
        const auto context = *m_roof_edit_context;
        const auto original = selectedEntity();
        const auto form = original ? read_string(original->properties, "form") : std::nullopt;
        const bool symmetric_roof = form == std::optional<std::string>("gable_roof") ||
                           form == std::optional<std::string>("hip_roof");
        if (!original || original->type != "roof" ||
            (!symmetric_roof && form != std::optional<std::string>("sloped_roof_panel"))) {
            setError(QStringLiteral("Select a supported roof before applying dimensions."));
            return false;
        }
        try {
            BuildingObjectDialog dialog(*original, m_metric_units, owner);
            const auto set_field = [&](const char* name, const QString& value) {
                auto* field = dialog.findChild<QLineEdit*>(QString::fromLatin1(name));
                if (field == nullptr) {
                    throw std::runtime_error("The roof editor is missing a dimension field.");
                }
                field->setText(value);
            };
            const auto set_if_changed = [&](const char* name, const QString& value,
                                            const QString& original_text) {
                if (value.trimmed() != original_text.trimmed()) {
                    set_field(name, value);
                }
            };
            // Leave untouched dialog fields at their decoded values so the
            // existing quantity-receipt merge preserves exact expressions.
            set_if_changed(symmetric_roof ? "buildingObjectLength" : "buildingObjectRun",
                           run_text, m_roof_run_original_text);
            set_if_changed("buildingObjectSpan", span_text, m_roof_span_original_text);
            set_if_changed("buildingObjectRise", rise_text, m_roof_rise_original_text);
            set_if_changed("buildingObjectOverhang", overhang_text, m_roof_overhang_original_text);
            set_if_changed("buildingObjectThickness", thickness_text,
                           m_roof_thickness_original_text);
            if (!dialog.submit()) {
                setError(QStringLiteral("Roof dimensions: %1").arg(dialog.lastError()));
                return false;
            }
            const auto candidate = dialog.candidate();
            if (!candidate.has_value()) {
                setError(QStringLiteral("Roof dimensions did not produce an editable candidate."));
                return false;
            }
            if (!modalContextUnchanged(context)) return false;
            return !commitBuildingObject(*candidate, context.revision, true).isEmpty();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Roof dimensions: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    void editRoofOpenings() {
        const auto context = captureModalContext();
        const auto original = selectedEntity();
        if (!original || original->type != "roof") return;
        QDialog dialog(owner);
        dialog.setObjectName("roofOpeningsDialog");
        dialog.setWindowTitle("Roof openings");
        dialog.resize(600, 360);
        auto* layout = new QVBoxLayout(&dialog);
        auto* help = new QLabel("Vertical through-openings. X/Y are local to the roof footprint; width/depth are horizontal distances.", &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* table = new QTableWidget(0, 4, &dialog);
        table->setObjectName("roofOpeningsTable");
        table->setHorizontalHeaderLabels({"X", "Y", "Width", "Depth"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(table);
        const std::array<const char*, 4> keys{"x_m", "y_m", "width_m", "depth_m"};
        auto original_receipts = json::object();
        const auto prior = original->extensions.find("roof_opening_input");
        if (prior != original->extensions.end() && prior->is_object() &&
            prior->value("version", json{}) == 1 && prior->contains("entries") && prior->at("entries").is_object())
            original_receipts = prior->at("entries");
        const auto restored_expression = [&](const std::string& id, const char* key, double value) {
            const auto fallback = QString::number(value, 'g', 17) + " m";
            try {
                const auto& receipt = original_receipts.at(id).at(key);
                const auto expression = receipt.at("original_expression").get<std::string>();
                const auto unit_text = receipt.at("default_unit").get<std::string>();
                if (expression.empty() || expression.size() > 4096 || (unit_text != "m" && unit_text != "ft")) return fallback;
                const auto unit = unit_text == "m" ? Unit::metre : Unit::foot;
                const auto quantity = parse_quantity(expression, unit);
                const auto& rational = receipt.at("exact_metres");
                if (!rational.at("numerator").is_number_integer() || !rational.at("denominator").is_number_integer() ||
                    rational.at("numerator") != quantity.exact_metres.numerator ||
                    rational.at("denominator") != quantity.exact_metres.denominator || quantity.metres != value) return fallback;
                const auto current = parse_quantity(expression, context.metric_units ? Unit::metre : Unit::foot);
                const auto text = QString::fromStdString(expression);
                return current.exact_metres.numerator == quantity.exact_metres.numerator &&
                       current.exact_metres.denominator == quantity.exact_metres.denominator
                    ? text : text + " " + QString::fromStdString(unit_text);
            } catch (const std::exception&) { return fallback; }
        };
        const auto append = [&](const json& entry) {
            const auto row = table->rowCount();
            table->insertRow(row);
            for (int column = 0; column < 4; ++column) {
                const auto value = entry.at(keys[column]).get<double>();
                const auto text = restored_expression(entry.at("id").get<std::string>(), keys[column], value);
                auto* item = new QTableWidgetItem(text);
                item->setData(Qt::UserRole, QString::fromStdString(entry.at("id").get<std::string>()));
                item->setData(Qt::UserRole + 1, text);
                item->setData(Qt::UserRole + 2, value);
                table->setItem(row, column, item);
            }
        };
        for (const auto& entry : original->properties.value("roof_openings", json::array())) append(entry);
        auto* error = new QLabel(&dialog);
        error->setObjectName("roofOpeningsError");
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        auto* add = buttons->addButton("Add opening", QDialogButtonBox::ActionRole);
        add->setObjectName("addRoofOpening");
        auto* remove = buttons->addButton("Remove selected", QDialogButtonBox::ActionRole);
        remove->setObjectName("removeRoofOpening");
        layout->addWidget(buttons);
        QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
            if (table->rowCount() >= 256) { error->setText("A roof supports at most 256 openings."); return; }
            append({{"id", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()},
                    {"x_m", 0.5}, {"y_m", 0.5}, {"width_m", 1.0}, {"depth_m", 1.0}});
        });
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
            const auto rows = table->selectionModel()->selectedRows();
            std::vector<int> indices;
            for (const auto& row : rows) indices.push_back(row.row());
            std::sort(indices.rbegin(), indices.rend());
            for (const auto row : indices) table->removeRow(row);
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                if (!modalContextUnchanged(context)) throw std::invalid_argument("Roof editing context changed. Reopen the openings editor.");
                auto candidate = *original;
                auto entries = json::array();
                auto receipts = original_receipts;
                for (int row = 0; row < table->rowCount(); ++row) {
                    const auto id = table->item(row, 0)->data(Qt::UserRole).toString().toStdString();
                    json entry{{"id", id}};
                    for (int column = 0; column < 4; ++column) {
                        const auto* item = table->item(row, column);
                        const auto text = item->text().trimmed();
                        if (text == item->data(Qt::UserRole + 1).toString()) {
                            entry[keys[column]] = item->data(Qt::UserRole + 2).toDouble();
                        } else {
                            const auto quantity = [&] {
                                try { return parse_quantity(text.toStdString(), context.metric_units ? Unit::metre : Unit::foot); }
                                catch (const std::exception& failure) {
                                    table->setCurrentCell(row, column);
                                    table->setFocus();
                                    throw std::invalid_argument(QStringLiteral("Opening %1, %2: %3")
                                        .arg(row + 1).arg(table->horizontalHeaderItem(column)->text())
                                        .arg(QString::fromUtf8(failure.what())).toStdString());
                                }
                            }();
                            entry[keys[column]] = quantity.metres;
                            if (!receipts[id].is_object()) receipts[id] = json::object();
                            receipts[id][keys[column]] = {{"original_expression", quantity.original_expression},
                                {"default_unit", context.metric_units ? "m" : "ft"},
                                {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                                                  {"denominator", quantity.exact_metres.denominator}}}};
                        }
                    }
                    entries.push_back(std::move(entry));
                }
                if (entries == original->properties.value("roof_openings", json::array()) && receipts == original_receipts) {
                    dialog.accept(); return;
                }
                for (auto receipt = receipts.begin(); receipt != receipts.end();) {
                    const bool retained = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                        return entry.at("id") == receipt.key();
                    });
                    if (!retained) receipt = receipts.erase(receipt); else ++receipt;
                }
                if (entries.empty()) { candidate.properties.erase("roof_openings"); candidate.properties["version"] = 1; }
                else { candidate.properties["roof_openings"] = entries; candidate.properties["version"] = 2; }
                candidate.extensions["roof_opening_input"] = {{"version", 1}, {"entries", receipts}};
                if (commitBuildingObject(candidate, context.revision, true).isEmpty())
                    throw std::invalid_argument(m_last_error.toStdString());
                dialog.accept();
            } catch (const std::exception& failure) { error->setText(QString::fromUtf8(failure.what())); }
        });
        dialog.exec();
    }

    bool editSelectedBuildingDimensions() {
        const auto fail = [this](const QString& message) {
            setError(message);
            m_building_dimensions_error->setText(message);
            m_building_dimensions_error->show();
            return false;
        };
        if (!m_building_edit_context || !modalContextUnchanged(*m_building_edit_context))
            return fail(QStringLiteral("The object editing context changed. Reselect the object before applying changes."));
        const auto context = *m_building_edit_context;
        const auto original = selectedEntity();
        if (!original || (original->type != "column" && original->type != "beam" && original->type != "stair"))
            return fail(QStringLiteral("Select a column, beam or stair before applying changes."));
        try {
            BuildingObjectDialog dialog(*original, m_metric_units, owner);
            bool changed = false;
            for (const auto& dimension : m_building_dimensions) {
                if (dimension.edit->isHidden() ||
                    dimension.edit->text().trimmed() == dimension.original_text.trimmed()) continue;
                auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("buildingObject") + dimension.suffix);
                if (!field) throw std::runtime_error("The object editor is missing a dimension field.");
                field->setText(dimension.edit->text());
                changed = true;
            }
            for (const auto& placement : m_building_placement) {
                if (placement.edit->isHidden() ||
                    placement.edit->text().trimmed() == placement.original_text.trimmed()) continue;
                auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("buildingObject") + placement.suffix);
                if (!field) throw std::runtime_error("The object editor is missing a placement field.");
                field->setText(placement.edit->text());
                changed = true;
            }
            if (!changed) {
                clearError();
                m_building_dimensions_error->hide();
                return true;
            }
            if (!dialog.submit()) return fail(QStringLiteral("Object changes: %1").arg(dialog.lastError()));
            const auto candidate = dialog.candidate();
            if (!candidate) return fail(QStringLiteral("Object changes did not produce an editable candidate."));
            if (!modalContextUnchanged(context)) return fail(m_last_error);
            if (commitBuildingObject(*candidate, context.revision, true).isEmpty()) return fail(m_last_error);
            return true;
        } catch (const std::exception& error) {
            return fail(QStringLiteral("Object changes: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    bool cutSelection() {
        try {
            const auto source = authoringSnapshot();
            const auto selected = m_selected_id.toStdString();
            if (!source.entities().contains(selected) &&
                annotation_parent_for_child(source, selected).has_value()) {
                throw std::invalid_argument(
                    "Select the annotation group before cutting its children.");
            }
            const auto entities = clipboard_entities_for_selection(source, selected);
            if (entities.empty()) {
                throw std::invalid_argument(
                    "Select supported geometry, an area, or an architectural object to cut.");
            }
            if (!copySelection()) return false;
            std::vector<EntityChange> changes;
            changes.reserve(entities.size());
            for (const auto& entity : entities) changes.push_back(EntityChange::erase(entity.id));
            const ApplyEntityChanges command{
                source.revision(), std::move(changes), {}, "Cut selection"};
            const auto authored = augmentAuthoredCommand(Command{command});
            (void)Document::preview_command(source, authored);
            applyAuthoredCommand(authored);
            m_selected_id.clear();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Cut: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool pasteSelection() {
        try {
            auto* clipboard = QGuiApplication::clipboard();
            if (clipboard == nullptr) {
                throw std::runtime_error("The system clipboard is unavailable.");
            }
            const auto encoded = clipboard->text(QClipboard::Clipboard).toUtf8();
            if (encoded.isEmpty() || static_cast<std::size_t>(encoded.size()) >
                                         kMaximumClipboardBytes) {
                throw std::invalid_argument("Clipboard data is empty or exceeds the local size limit.");
            }
            const auto payload = json::parse(encoded.constData(), encoded.constData() + encoded.size());
            if (!payload.is_object() || payload.value("format", "") != kClipboardFormat ||
                payload.value("version", 0) != 1 || !payload.contains("entities") ||
                !payload.at("entities").is_array() || payload.at("entities").empty() ||
                payload.at("entities").size() > kMaximumClipboardEntities) {
                throw std::invalid_argument("Clipboard data is not a supported sketch payload.");
            }

            std::vector<Entity> source_entities;
            source_entities.reserve(payload.at("entities").size());
            std::set<std::string, std::less<>> source_ids;
            for (const auto& value : payload.at("entities")) {
                auto entity = clipboard_entity_from_json(value);
                if (!source_ids.insert(entity.id).second) {
                    throw std::invalid_argument("Clipboard contains duplicate entity identities.");
                }
                source_entities.push_back(std::move(entity));
            }

            const auto source = authoringSnapshot();
            std::map<std::string, std::string, std::less<>> remap;
            std::set<std::string> reused_catalogs;
            const auto allocate = [&](std::string_view prefix) {
                std::string id;
                do {
                    id = new_id(prefix);
                } while (source.entities().contains(id) ||
                         std::any_of(remap.begin(), remap.end(),
                                     [&](const auto& entry) { return entry.second == id; }));
                return id;
            };
            for (const auto& entity : source_entities) {
                const auto existing = source.entities().find(entity.id);
                if(entity.type=="assembly_model" && existing!=source.entities().end() &&
                    existing->second.type=="assembly_model" && entity.properties.size()==2 && entity.extensions.empty()) {
                    const auto copied = AssemblyModel::from_json(entity.properties.at("model"));
                    const auto local = AssemblyModel::from_json(existing->second.properties.at("model"));
                    if(copied.types().empty() && copied.instances().empty() &&
                        std::all_of(copied.materials().begin(),copied.materials().end(),[&](const auto& material) {
                            return std::find(local.materials().begin(),local.materials().end(),material)!=local.materials().end();
                        })) {
                        remap.emplace(entity.id,entity.id);
                        reused_catalogs.insert(entity.id);
                        continue;
                    }
                }
                remap.emplace(entity.id, allocate(entity.type));
                if (can_recognize_boundary_entity_type(entity.type) &&
                    entity.properties.contains("boundary_model_version")) {
                    const auto identified = decode_identified_boundary_entity(entity);
                    for (const auto& edge : identified.segments) {
                        remap.emplace(edge.segment_id, allocate("segment"));
                        remap.emplace(edge.start_vertex_id, allocate("vertex"));
                        remap.emplace(edge.end_vertex_id, allocate("vertex"));
                    }
                }
                if (entity.type == kAnnotationEntityType) {
                    const auto state = decode_annotation_entity(entity);
                    for (const auto& label : state.labels) remap.emplace(label.id, allocate("label"));
                    for (const auto& symbol : state.symbols) remap.emplace(symbol.id, allocate("symbol"));
                }
            }

            const auto root_id = payload.value("root_id",source_entities.front().id);
            const auto root_mapping = remap.find(root_id);
            if (root_mapping == remap.end()) {
                throw std::invalid_argument("Clipboard root identity is missing from its payload.");
            }

            std::vector<EntityChange> changes;
            changes.reserve(source_entities.size());
            for (const auto& original : source_entities) {
                if(reused_catalogs.contains(original.id)) continue;
                auto entity = original;
                entity.id = remap.at(original.id);
                if(entity.type!="assembly_model") {
                    remap_entity_references(entity, remap);
                }
                const auto placeable = entity.type == "boundary" ||
                    entity.type == "measurement_boundary" || entity.type == "room_boundary" ||
                    entity.type == "wall" || entity.type == "room" || entity.type == "slab" ||
                    entity.type == "roof" || entity.type == "stair" || entity.type == "column" ||
                    entity.type == "beam";
                if (placeable) {
                    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                        entity.properties.erase(key);
                    if (!assignDrawingContext(entity.properties)) return false;
                }
                changes.push_back(EntityChange::upsert(std::move(entity)));
            }
            const ApplyEntityChanges command{
                source.revision(), std::move(changes), {}, "Paste selection"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = id_from(root_mapping->second);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Paste: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool deleteSelection() {
        try {
            const auto source = authoringSnapshot();
            const auto selected = m_selected_id.toStdString();
            if (!source.entities().contains(selected)) {
                const auto annotation_parent = annotation_parent_for_child(source, selected);
                if (annotation_parent.has_value()) {
                    if (!deleteAnnotation(m_selected_id)) return false;
                    return true;
                }
                throw std::invalid_argument("Select an entity before deleting it.");
            }
            const auto root = source.entities().at(selected);
            if (root.required) {
                throw std::invalid_argument("Required project entities cannot be deleted.");
            }
            const auto entities = clipboard_entities_for_selection(source, selected);
            if (entities.empty()) {
                throw std::invalid_argument(
                    "Select a boundary, wall, opening, architectural object, or annotation group.");
            }
            std::vector<EntityChange> changes;
            changes.reserve(entities.size());
            for (const auto& entity : entities) changes.push_back(EntityChange::erase(entity.id));
            const ApplyEntityChanges command{
                source.revision(), std::move(changes), {}, "Delete selection"};
            const auto authored = augmentAuthoredCommand(Command{command});
            (void)Document::preview_command(source, authored);
            applyAuthoredCommand(authored);
            m_selected_id.clear();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Delete: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool insertSelectedBoundaryVertex(const QString& segment_id, const QString& fraction_text) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto source = authoringSnapshot();
            const auto found = source.entities().find(m_selected_id.toStdString());
            if (found == source.entities().end() || !is_closed_boundary_entity(found->second.type)) {
                throw std::invalid_argument("Select an identified closed boundary first.");
            }
            const auto version = inspect_boundary_entity_version(found->second);
            if (version.format == BoundaryEntityFormat::unsupported_version) {
                throw std::invalid_argument("This boundary uses an unsupported model version.");
            }
            if (version.format == BoundaryEntityFormat::anonymous_legacy) {
                throw std::invalid_argument(
                    "This legacy boundary needs an explicit identity upgrade before vertex insertion.");
            }
            if (found->second.properties.contains("boundary_authoring")) {
                throw std::invalid_argument(
                    "Receipt-bound boundaries require an explicit derivation policy before vertex insertion.");
            }
            bool ok = false;
            const auto fraction = fraction_text.trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0) {
                throw std::invalid_argument("Insertion fraction must be a finite value strictly between 0 and 1.");
            }
            const auto identified = decode_identified_boundary_entity(found->second);
            const auto target_segment = segment_id.trimmed().toStdString();
            const auto target_index = std::find_if(
                identified.segments.begin(), identified.segments.end(),
                [&](const auto& edge) { return edge.segment_id == target_segment; });
            if (target_index == identified.segments.end()) {
                throw std::invalid_argument("The selected boundary edge was not found.");
            }
            std::vector<std::string> replacement_segment_ids;
            std::vector<std::string> replacement_vertex_ids;
            replacement_segment_ids.reserve(identified.segments.size());
            replacement_vertex_ids.reserve(identified.segments.size());
            std::map<std::string, std::string, std::less<>> identity_remap;
            for (const auto& edge : identified.segments) {
                const auto next_segment = new_id("segment");
                replacement_segment_ids.push_back(next_segment);
                identity_remap.emplace(edge.segment_id, next_segment);
                replacement_vertex_ids.push_back(new_id("vertex"));
                identity_remap.emplace(edge.start_vertex_id, replacement_vertex_ids.back());
            }
            // clone_boundary supplies each edge's end vertex from the next
            // slot, so every old vertex identity is covered by its start edge.
            const auto inserted_boundary_id = new_id(
                identified.type == "room_boundary" ? "room-boundary" : "boundary");
            identity_remap.emplace(found->second.id, inserted_boundary_id);
            auto replacement = clone_boundary(
                identified, inserted_boundary_id,
                LegacyBoundaryIdentityOptions{replacement_segment_ids, replacement_vertex_ids},
                {0.0, 0.0});
            const auto inserted = insert_boundary_vertex(
                replacement, replacement_segment_ids[static_cast<std::size_t>(target_index - identified.segments.begin())],
                fraction, new_id("vertex"), new_id("segment"));
            // The old edge and vertex IDs are no longer present after a
            // replacement. Preserve every external semantic link by mapping
            // the old identities to the corresponding fresh first-piece IDs.
            const auto inserted_edge = inserted.segments[static_cast<std::size_t>(target_index - identified.segments.begin())];
            identity_remap[identified.segments[static_cast<std::size_t>(target_index - identified.segments.begin())].segment_id] =
                inserted_edge.segment_id;
            identity_remap[identified.segments[static_cast<std::size_t>(target_index - identified.segments.begin())].start_vertex_id] =
                inserted_edge.start_vertex_id;
            auto updated = found->second;
            updated.id = inserted.id;
            remap_entity_references(updated, identity_remap);
            // Merge by the remapped stable edge IDs. The first split piece
            // continues the original metadata; the new second piece starts
            // without copied ownership. The codec also refuses unhandled
            // directional receipts on geometry that would change.
            updated = encode_identified_boundary_entity(inserted, &updated);
            if (updated.properties.contains("boundary")) {
                updated.properties["boundary"] = boundary_json(boundary_geometry(inserted));
            }
            std::vector<EntityChange> changes;
            changes.push_back(EntityChange::erase(found->second.id));
            for (const auto& [id, entity] : source.entities()) {
                if (id == found->second.id) continue;
                auto migrated = entity;
                remap_entity_references(migrated, identity_remap);
                if (migrated != entity) changes.push_back(EntityChange::upsert(std::move(migrated)));
            }
            changes.push_back(EntityChange::upsert(std::move(updated)));
            const ApplyEntityChanges command{
                source.revision(), std::move(changes), {}, "Insert boundary vertex"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            m_selected_id = id_from(inserted.id);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Insert vertex: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool redefineSelectedBoundary(const Boundary& boundary, const QString& classification) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto source = authoringSnapshot();
            const auto found = source.entities().find(m_selected_id.toStdString());
            if (found == source.entities().end() || !is_closed_boundary_entity(found->second.type)) {
                throw std::invalid_argument("Select an identified closed boundary first.");
            }
            const auto version = inspect_boundary_entity_version(found->second);
            if (version.format == BoundaryEntityFormat::unsupported_version) {
                throw std::invalid_argument("This boundary uses an unsupported model version.");
            }
            if (version.format == BoundaryEntityFormat::anonymous_legacy) {
                throw std::invalid_argument(
                    "This legacy boundary needs an explicit identity upgrade before redefinition.");
            }
            if (found->second.properties.contains("boundary_authoring")) {
                throw std::invalid_argument(
                    "Receipt-bound boundaries require an explicit derivation policy before redefinition.");
            }
            const auto diagnostics = validate_boundary(boundary);
            if (!diagnostics.empty()) {
                throw std::invalid_argument(diagnostics.front().message);
            }
            const auto original = decode_identified_boundary_entity(found->second);
            if (original.segments.size() != boundary.size()) {
                throw std::invalid_argument(
                    "Redefinition must preserve the boundary edge count so existing references remain valid.");
            }
            auto replacement = original;
            for (std::size_t index = 0; index < replacement.segments.size(); ++index) {
                replacement.segments[index].segment = boundary[index];
            }
            auto updated = encode_identified_boundary_entity(replacement, &found->second);
            const auto name = classification.trimmed();
            if (!name.isEmpty()) {
                updated.properties["classification"] = name.toStdString();
                if (updated.type == "room_boundary") updated.properties["name"] = name.toStdString();
            }
            if (updated.properties.contains("boundary")) {
                updated.properties["boundary"] = boundary_json(boundary);
            }
            const ApplyEntityChanges command{
                source.revision(),
                {EntityChange::upsert(std::move(updated))},
                {},
                "Redefine boundary"};
            (void)Document::preview_command(source, command);
            applyDocumentCommand(command);
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Redefine boundary: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] QString selectedEntityId() const { return m_selected_id; }

    bool editSelectedClassification(const QString& classification) {
        if (classification.trimmed().isEmpty()) {
            setError(QStringLiteral("Classification cannot be empty."));
            return false;
        }
        const auto entity = selectedEntity();
        if (!entity.has_value()) {
            setError(QStringLiteral("Select an entity before editing its classification."));
            return false;
        }
        const auto value = classification.trimmed().toLower();
        if (entity->type == "opening" && value != QStringLiteral("door") &&
            value != QStringLiteral("window")) {
            setError(QStringLiteral("An opening classification must be Door or Window."));
            return false;
        }
        return editSelected([&](json& properties) {
            properties["classification"] = value.toStdString();
            if (entity->type == "opening") {
                properties["opening_kind"] = value.toStdString();
            }
        },
                            "edit classification");
    }

    bool editSelectedLength(const QString& expression) {
        const auto entity = selectedEntity();
        if (!entity.has_value() || (entity->type != "wall" && entity->type != "opening")) {
            setError(QStringLiteral("Select a wall or opening to edit its length."));
            return false;
        }
        try {
            const auto quantity = parse_quantity(
                expression.toStdString(), m_metric_units ? Unit::metre : Unit::foot);
            if (!(quantity.metres > 1e-7)) {
                setError(QStringLiteral("Length must be greater than zero."));
                return false;
            }
            if (entity->type == "opening") {
                auto properties = entity->properties;
                properties["width_m"] = quantity.metres;
                if (!previewOpening(*entity, properties)) {
                    return false;
                }
                return editSelectedProperties(std::move(properties), "edit opening width");
            }
            // Programmatic edits use the same validated intent as the dialog.
            // The interactive inspector opens a preview before committing.
            ConstraintDialog dialog(authoringSnapshot(), m_selected_id, m_metric_units, owner);
            dialog.setLengthExpression(expression);
            if (!dialog.previewEdit() || !dialog.submit()) {
                setError(dialog.lastError());
                return false;
            }
            applyConstraintPreview(*dialog.acceptedPreview());
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Length: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool editSelectedHeight(const QString& expression) {
        return editSelectedQuantity(expression, "height_m", "height", "edit wall height");
    }

    bool editSelectedThickness(const QString& expression) {
        return editSelectedQuantity(expression, "thickness_m", "thickness", "edit wall thickness");
    }

    bool editSelectedFactor(const QString& expression) {
        const auto entity = selectedEntity();
        if (!entity.has_value() || !is_closed_boundary_entity(entity->type)) {
            setError(QStringLiteral("Select a closed boundary to edit its area factor."));
            return false;
        }
        try {
            const auto rational = parse_factor_expression(expression);
            const auto value = static_cast<double>(rational.numerator) /
                               static_cast<double>(rational.denominator);
            if (!std::isfinite(value)) {
                setError(QStringLiteral("Area factor exceeds the supported numeric range."));
                return false;
            }
            const auto trimmed = expression.trimmed();
            auto properties = entity->properties;
            const auto stored = stored_factor_json(StoredFactor{rational, trimmed});
            for (const auto& [key, item] : stored.items()) {
                properties[key] = item;
            }
            return editSelectedProperties(std::move(properties), "edit area factor");
        } catch (const std::exception& error) {
            setError(QStringLiteral("Area factor: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool setSelectedDeductions(const QStringList& deduction_ids) {
        const auto entity = selectedEntity();
        if (!entity.has_value() || !is_closed_boundary_entity(entity->type)) {
            setError(QStringLiteral("Select a closed boundary before editing deductions."));
            return false;
        }
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return false;
        }
        try {
            const auto snapshot = m_document->snapshot();
            const auto& entities = snapshot.entities();
            const auto floor_id = read_string(entity->properties, "floor_id");
            if (!floor_id.has_value() || floor_id->empty()) {
                throw std::invalid_argument("The selected boundary has no floor reference.");
            }
            const auto floor = entities.find(*floor_id);
            if (floor == entities.end() || floor->second.type != "floor") {
                throw std::invalid_argument("The selected boundary references an unknown floor.");
            }
            auto building_id = read_string(entity->properties, "building_id");
            if (!building_id.has_value() || building_id->empty()) {
                building_id = read_string(floor->second.properties, "building_id");
            }
            if (!building_id.has_value() || building_id->empty()) {
                throw std::invalid_argument("The selected boundary has no building reference.");
            }
            const auto classification = read_string(entity->properties, "classification");
            if (!classification.has_value() || classification->empty()) {
                throw std::invalid_argument("Assign a classification before editing deductions.");
            }
            const auto property = propertyEntity();
            if (!property.has_value()) {
                throw std::invalid_argument("The calculation profile is unavailable.");
            }
            const auto profile = read_calculation_profile(property->properties);
            const auto base = read_boundary(entity->properties);
            const auto base_diagnostics = validate_boundary(base);
            if (!base_diagnostics.empty()) {
                throw std::invalid_argument("The selected boundary is invalid: " +
                                            base_diagnostics.front().message);
            }
            const auto factor = read_stored_factor(entity->properties);
            std::vector<AreaDeduction> deductions;
            deductions.reserve(deduction_ids.size());
            std::set<std::string, std::less<>> seen;
            for (const auto& raw_id : deduction_ids) {
                const auto id = raw_id.trimmed().toStdString();
                if (id.empty() || !seen.insert(id).second) {
                    throw std::invalid_argument("Choose each deduction boundary only once.");
                }
                if (id == entity->id) {
                    throw std::invalid_argument("A boundary cannot deduct itself.");
                }
                const auto found = entities.find(id);
                if (found == entities.end() || !is_closed_boundary_entity(found->second.type)) {
                    throw std::invalid_argument("Deduction boundary " + id + " is unavailable.");
                }
                const auto candidate_floor = read_string(found->second.properties, "floor_id");
                if (!candidate_floor.has_value() || *candidate_floor != *floor_id) {
                    throw std::invalid_argument("Deduction boundaries must be on the active floor.");
                }
                const auto boundary = read_boundary(found->second.properties);
                const auto diagnostics = validate_boundary(boundary);
                if (!diagnostics.empty()) {
                    throw std::invalid_argument("Deduction boundary " + id + " is invalid: " +
                                                diagnostics.front().message);
                }
                if (!read_deduction_ids(found->second.properties).empty()) {
                    throw std::invalid_argument("A deduction boundary cannot contain another deduction.");
                }
                deductions.push_back({id, boundary});
            }
            MeasurementArea area{entity->id, *building_id, *floor_id, *classification,
                                 base, deductions, factor.rational};
            (void)calculate_area(area, profile);

            auto properties = entity->properties;
            if (deduction_ids.isEmpty()) {
                properties.erase("deduction_ids");
            } else {
                properties["deduction_ids"] = deduction_ids_json(deduction_ids);
            }
            return editSelectedProperties(std::move(properties), "edit area deductions");
        } catch (const std::exception& error) {
            setError(QStringLiteral("Deductions must be valid and inside the selected boundary: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool setSelectedCalculationRule(bool include_in_building, bool include_in_living) {
        const auto entity = selectedEntity();
        if (!entity.has_value() || !is_closed_boundary_entity(entity->type)) {
            setError(QStringLiteral("Select a closed boundary to edit its calculation profile."));
            return false;
        }
        const auto classification = read_string(entity->properties, "classification");
        if (!classification.has_value() || classification->empty()) {
            setError(QStringLiteral("Assign a classification before editing its calculation rule."));
            return false;
        }
        const auto property = propertyEntity();
        if (!property.has_value()) {
            setError(QStringLiteral("Calculation profile is unavailable: no property entity exists."));
            return false;
        }
        try {
            auto profile = read_calculation_profile(property->properties);
            const auto next_rule = ClassificationRule{include_in_building, include_in_living};
            const auto found = profile.classifications.find(*classification);
            if (found != profile.classifications.end() && found->second.building_total ==
                                                        next_rule.building_total &&
                found->second.living_total == next_rule.living_total) {
                clearError();
                refreshCalculationInspector(entity);
                return true;
            }
            if (profile.version == std::numeric_limits<unsigned>::max()) {
                setError(QStringLiteral("Calculation profile version cannot advance further."));
                return false;
            }
            profile.version += 1;
            profile.classifications[*classification] = next_rule;
            auto updated = *property;
            updated.properties["calculation_profile"] = calculation_profile_json(profile);
            if (!applyEntity(std::move(updated), "edit calculation profile")) {
                return false;
            }
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Calculation profile: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool undoCommand() {
        if (m_boundary_session) {
            const bool changed = m_boundary_session->undo();
            if (changed) { clearError(); boundaryDraftChanged(); }
            return changed;
        }
        if (!(m_recovery_ledger.empty() ? m_document->can_undo() : m_project_workspace->can_undo())) {
            return false;
        }
        try {
            const auto selection_before = m_selected_id;
            if (m_recovery_ledger.empty()) m_document->undo(m_document->revision());
            else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_undo();
                commitWorkspaceEdit(edit);
            }
            // Keep the inspector context across edits that leave the selected
            // entity in the document. Creation and deletion commands already
            // clear or replace the selection at their mutation boundary, so a
            // missing identity still fails closed here.
            if (!selection_before.isEmpty() &&
                has_entity(*m_document, selection_before)) {
                m_selected_id = selection_before;
            } else {
                m_selected_id.clear();
            }
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Undo failed: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool redoCommand() {
        if (m_boundary_session) {
            const bool changed = m_boundary_session->redo();
            if (changed) { clearError(); boundaryDraftChanged(); }
            return changed;
        }
        if (!(m_recovery_ledger.empty() ? m_document->can_redo() : m_project_workspace->can_redo())) {
            return false;
        }
        try {
            const auto selection_before = m_selected_id;
            if (m_recovery_ledger.empty()) m_document->redo(m_document->revision());
            else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_redo();
                commitWorkspaceEdit(edit);
            }
            if (!selection_before.isEmpty() &&
                has_entity(*m_document, selection_before)) {
                m_selected_id = selection_before;
            } else if (!selection_before.isEmpty()) {
                m_selected_id.clear();
            }
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Redo failed: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool createNewProject() {
        if (!confirmDirtyTransition(QStringLiteral("Replace current project"),
                                    QStringLiteral("Save current changes before creating a new project?"))) {
            return false;
        }
        try {
            auto candidate = std::make_shared<Document>(Document::create());
            ensure_project_scaffold(*candidate);
            auto candidate_workspace = std::make_unique<ProjectWorkspace>(candidate->snapshot());
            m_document = std::move(candidate);
            m_project_workspace = std::move(candidate_workspace);
            m_recovery_ledger.clear();
            m_saved_edited_generation = 0;
            resetAutosaveSession();
            m_view_filter = {};
            initializeDrawingContext();
            m_file_path.clear();
            m_file_sha256.clear();
            m_selected_id.clear();
            m_output_sheet_id.clear();
            clearPreview();
            m_tool = CanvasTool::select;
            syncToolControls();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("New project failed: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool openProject(const QString& path) {
        if (path.trimmed().isEmpty()) {
            setError(QStringLiteral("Choose a project file to open."));
            return false;
        }
        if (!confirmDirtyTransition(QStringLiteral("Open project"),
                                    QStringLiteral("Save current changes before opening another project?"))) {
            return false;
        }
        try {
            const auto candidate_path = filesystem_path(path);
            std::shared_ptr<Document> candidate;
            std::unique_ptr<ProjectWorkspace> candidate_workspace;
            RecoveryLedger candidate_ledger;
            std::string candidate_sha256;
            try {
                auto loaded = ProjectStore::load(candidate_path);
                candidate = std::make_shared<Document>(std::move(loaded.document));
                candidate_workspace = std::make_unique<ProjectWorkspace>(candidate->snapshot());
                candidate_sha256 = std::move(loaded.file_sha256);
            } catch (const StorageError& error) {
                if (error.code() != StorageErrorCode::unsupported_format) throw;
                auto loaded = ProjectStore::load_archive(candidate_path, ArchiveRole::ordinary);
                if (!loaded.supported()) {
                    // The native Recover action intentionally feeds the same
                    // guarded open path. Recovery-role archives are accepted
                    // here only after their role, ledger and workspace state
                    // pass the archive decoder; opaque or mismatched files
                    // remain blocked.
                    loaded = ProjectStore::load_archive(candidate_path, ArchiveRole::recovery_copy);
                }
                if (!loaded.supported())
                    throw std::runtime_error("This recovery archive is unsupported and cannot be opened for editing.");
                candidate_workspace = ProjectWorkspace::restore_archive(
                    *loaded.archive, *loaded.recovery.decoded);
                candidate = std::make_shared<Document>(Document::fork(candidate_workspace->snapshot()));
                candidate_ledger = loaded.archive->recovery();
                candidate_sha256 = std::move(loaded.file_sha256);
            }
            m_document = std::move(candidate);
            m_project_workspace = std::move(candidate_workspace);
            m_recovery_ledger = std::move(candidate_ledger);
            m_saved_workspace_epoch = m_project_workspace->epoch();
            m_saved_edited_generation = m_document->dirty() ? 0 : m_project_workspace->edited_generation();
            resetAutosaveSession();
            m_view_filter = {};
            m_file_path = candidate_path;
            initializeDrawingContext();
            m_file_sha256 = std::move(candidate_sha256);
            m_selected_id.clear();
            m_output_sheet_id.clear();
            clearPreview();
            m_tool = CanvasTool::select;
            syncToolControls();
            clearError();
            refresh();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Open failed; the current document is unchanged: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    [[nodiscard]] QString draftOutputStamp() const {
        if (m_view_filter.hidden_floor_ids.empty() && m_view_filter.hidden_layer_ids.empty()) {
            return QStringLiteral("DRAFT — internal checkpoint");
        }
        return QStringLiteral(
            "DRAFT — VIEW FILTER ACTIVE • floor/layer filters applied • view filters do not change totals");
    }

    [[nodiscard]] QPageSize::PageSizeId selectedPageSize() const noexcept {
        if (m_pageSizeCombo == nullptr || m_pageSizeCombo->currentData().isNull()) {
            return QPageSize::A4;
        }
        return static_cast<QPageSize::PageSizeId>(m_pageSizeCombo->currentData().toInt());
    }

    [[nodiscard]] PlanCanvas* outputCanvas() const noexcept {
        return m_workspace == Workspace::architectural ? m_architecturalCanvas
                                                        : m_measurementCanvas;
    }

    bool renderSheetOutput(QPainter& painter, const QRectF& target, QColor background) {
        if (target.width() <= 0.0 || target.height() <= 0.0) return false;
        const auto snapshot = m_document->snapshot();
        // Resolve and fingerprint the persisted sheet graph before drawing so
        // preview, PDF, SVG, and print all share one validated output scene.
        // This also blocks authoritative-looking output when a required sheet
        // or dependency is malformed.
        try {
            (void)outputFingerprintForSnapshot(snapshot);
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet output blocked: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
        const Entity* sheet_entity = nullptr;
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == kSheetViewEntityType) {
                sheet_entity = &entity;
                break;
            }
        }
        if (sheet_entity == nullptr) {
            outputCanvas()->renderScene(painter, target, true, background);
            return true;
        }
        try {
            const auto model = decode_sheet_view_entity(*sheet_entity);
            if (model.sheets().empty()) throw std::invalid_argument("no drawing sheets are defined");
            const auto selected_sheet_id = outputSheetId().toStdString();
            const auto selected_sheet = std::find_if(
                model.sheets().begin(), model.sheets().end(),
                [&](const auto& candidate) { return candidate.id == selected_sheet_id; });
            if (selected_sheet == model.sheets().end())
                throw std::invalid_argument("selected drawing sheet is no longer available");
            const auto& sheet = *selected_sheet;
            const auto paper_scale = std::min(target.width() / sheet.width_mm,
                                              target.height() / sheet.height_mm);
            if (!(std::isfinite(paper_scale) && paper_scale > 0.0))
                throw std::invalid_argument("sheet has no renderable dimensions");
            const QRectF page(target.center().x() - sheet.width_mm * paper_scale * 0.5,
                              target.center().y() - sheet.height_mm * paper_scale * 0.5,
                              sheet.width_mm * paper_scale, sheet.height_mm * paper_scale);
            painter.save();
            painter.fillRect(target, background);
            painter.fillRect(page, Qt::white);
            painter.setPen(QPen(QColor(45, 52, 60), std::max(1.0, paper_scale)));
            painter.drawRect(page);

            const auto find_view = [&](const std::string& id) -> const CoordinatedView* {
                const auto found = std::find_if(model.views().begin(), model.views().end(),
                    [&](const auto& view) { return view.id == id; });
                return found == model.views().end() ? nullptr : &*found;
            };
            for (const auto& viewport : sheet.viewports) {
                const auto* view = find_view(viewport.view_id);
                if (view == nullptr) continue;
                const auto view_kind = [&] {
                    switch (view->kind) {
                    case CoordinatedViewKind::plan: return BuildingViewKind::plan;
                    case CoordinatedViewKind::elevation: return BuildingViewKind::elevation;
                    case CoordinatedViewKind::section: return BuildingViewKind::section;
                    }
                    throw std::invalid_argument("unknown coordinated view kind");
                }();
                const QRectF viewport_rect(
                    page.left() + viewport.bounds.x_mm * paper_scale,
                    page.top() + viewport.bounds.y_mm * paper_scale,
                    viewport.bounds.width_mm * paper_scale,
                    viewport.bounds.height_mm * paper_scale);
                painter.save();
                painter.setClipRect(viewport_rect);
                const auto model_scale = paper_scale * 1000.0 / viewport.scale_denominator;
                std::unique_ptr<PlanCanvas> temporary_canvas;
                PlanCanvas* viewport_canvas = nullptr;
                if (view_kind == BuildingViewKind::plan) {
                    viewport_canvas = m_measurementCanvas;
                } else if (view_kind == m_architectural_view_kind) {
                    viewport_canvas = m_architecturalCanvas;
                } else {
                    temporary_canvas = std::make_unique<PlanCanvas>();
                    temporary_canvas->setGridEnabled(false);
                    temporary_canvas->setSnapEnabled(false);
                    temporary_canvas->setEntities(
                        m_architectural_view_entities[architectural_view_index(view_kind)]);
                    temporary_canvas->setLabels(m_measurementCanvas->labels());
                    temporary_canvas->setReferences(m_measurementCanvas->references());
                    viewport_canvas = temporary_canvas.get();
                }
                const auto viewport_center = viewport_canvas->contentCenter();
                viewport_canvas->renderSceneAt(painter, viewport_rect, model_scale,
                                               viewport_center, Qt::white);
                painter.restore();
                painter.setPen(QPen(QColor(115, 125, 138), std::max(1.0, paper_scale * 0.6)));
                painter.drawRect(viewport_rect);
                const auto view_caption = QString::fromStdString(view->name).trimmed().isEmpty()
                    ? QString::fromLatin1(architectural_view_name(view_kind)).toUpper()
                    : QString::fromStdString(view->name).trimmed().toUpper();
                painter.setPen(QColor(45, 52, 60));
                painter.setFont(QFont(QStringLiteral("Inter"),
                                      std::max(6, static_cast<int>(8.0 * paper_scale))));
                painter.drawText(viewport_rect.adjusted(4.0 * paper_scale,
                                                        3.0 * paper_scale,
                                                        -4.0 * paper_scale,
                                                        -3.0 * paper_scale),
                                 Qt::AlignLeft | Qt::AlignTop,
                                 view_caption + QStringLiteral("  •  1:%1")
                                     .arg(QString::number(viewport.scale_denominator, 'f', 0)));
            }

            // Schedule placements are part of the persisted sheet graph. Draw
            // their revision-bound rows from the same document projection used
            // by the Schedules dialog so printed output cannot drift from the
            // editable source model.
            const auto schedule_projection = scheduleSnapshot();
            for (const auto& placement : sheet.schedules) {
                const QRectF schedule_rect(
                    page.left() + placement.bounds.x_mm * paper_scale,
                    page.top() + placement.bounds.y_mm * paper_scale,
                    placement.bounds.width_mm * paper_scale,
                    placement.bounds.height_mm * paper_scale);
                const auto schedule_name =
                    QString::fromStdString(placement.schedule_id).trimmed().toLower();
                std::optional<ScheduleRowKind> kind;
                if (schedule_name.contains(QStringLiteral("door"))) kind = ScheduleRowKind::door;
                else if (schedule_name.contains(QStringLiteral("window"))) kind = ScheduleRowKind::window;
                else if (schedule_name.contains(QStringLiteral("room"))) kind = ScheduleRowKind::room;
                else if (schedule_name.contains(QStringLiteral("material"))) kind = ScheduleRowKind::material;
                QString heading = schedule_name.isEmpty() ? QStringLiteral("SCHEDULE")
                                                            : schedule_name.toUpper() + QStringLiteral(" SCHEDULE");
                std::vector<const ScheduleRow*> rows;
                if (kind) {
                    for (const auto& row : schedule_projection.snapshot.rows)
                        if (row.kind == *kind) rows.push_back(&row);
                }
                painter.save();
                painter.setClipRect(schedule_rect);
                painter.fillRect(schedule_rect, Qt::white);
                painter.setPen(QPen(QColor(45, 52, 60), std::max(1.0, paper_scale * 0.6)));
                painter.drawRect(schedule_rect);
                const auto header_height = std::max(12.0, 16.0 * paper_scale);
                painter.fillRect(QRectF(schedule_rect.left(), schedule_rect.top(),
                                        schedule_rect.width(), header_height),
                                 QColor(229, 235, 241));
                painter.setPen(QColor(35, 41, 48));
                painter.setFont(QFont(QStringLiteral("Inter"),
                                      std::max(6, static_cast<int>(8.0 * paper_scale))));
                painter.drawText(QRectF(schedule_rect.left() + 4.0 * paper_scale,
                                        schedule_rect.top(), schedule_rect.width() - 8.0 * paper_scale,
                                        header_height), Qt::AlignLeft | Qt::AlignVCenter, heading);
                const auto row_height = std::max(10.0, 14.0 * paper_scale);
                const auto available_rows = std::max(0, static_cast<int>(
                    std::floor((schedule_rect.height() - header_height) / row_height)));
                for (int index = 0; index < available_rows; ++index) {
                    const QRectF row_rect(schedule_rect.left(), schedule_rect.top() + header_height +
                                               static_cast<double>(index) * row_height,
                                           schedule_rect.width(), row_height);
                    painter.setPen(QPen(QColor(196, 203, 211), std::max(1.0, paper_scale * 0.35)));
                    painter.drawLine(row_rect.bottomLeft(), row_rect.bottomRight());
                    painter.setPen(QColor(50, 57, 65));
                    QString text = index < static_cast<int>(rows.size())
                        ? QStringLiteral("%1  %2")
                              .arg(QString::fromStdString(rows[static_cast<std::size_t>(index)]->mark),
                                   QString::fromStdString(rows[static_cast<std::size_t>(index)]->object_id))
                        : QString();
                    if (index < static_cast<int>(rows.size())) {
                        const auto& cells = rows[static_cast<std::size_t>(index)]->cells;
                        int appended = 0;
                        for (const auto& [column, cell] : cells) {
                            if (appended++ == 2) break;
                            text += QStringLiteral("  %1: %2")
                                        .arg(QString::fromStdString(column), schedule_value_text(cell.value));
                        }
                    }
                    if (text.isEmpty() && index == 0) text = QStringLiteral("No rows");
                    painter.drawText(row_rect.adjusted(4.0 * paper_scale, 0.0,
                                                       -4.0 * paper_scale, 0.0),
                                     Qt::AlignLeft | Qt::AlignVCenter, text);
                }
                painter.restore();
            }

            const auto title_height = std::max(24.0, 26.0 * paper_scale);
            const QRectF title_rect(page.right() - 220.0 * paper_scale,
                                    page.bottom() - title_height,
                                    220.0 * paper_scale, title_height);
            painter.setPen(QPen(QColor(45, 52, 60), std::max(1.0, paper_scale * 0.6)));
            painter.drawRect(title_rect);
            painter.setPen(QColor(35, 41, 48));
            painter.setFont(QFont(QStringLiteral("Inter"), std::max(7, static_cast<int>(9.0 * paper_scale))));
            const auto& title = sheet.title_block;
            painter.drawText(title_rect.adjusted(8.0 * paper_scale, 4.0 * paper_scale,
                                                 -8.0 * paper_scale, -4.0 * paper_scale),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1  %2\n%3  %4")
                                 .arg(QString::fromStdString(sheet.number),
                                      QString::fromStdString(title.title),
                                      QString::fromStdString(title.project),
                                      QString::fromStdString(title.author)));
            painter.restore();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet output blocked: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool saveProject() {
        if (m_file_path.empty()) {
            const auto selected = QFileDialog::getSaveFileName(
            owner, QStringLiteral("Save project"), {}, QStringLiteral("Property Studio project (*.bldproj)"));
            if (selected.isEmpty()) {
                return false;
            }
            return saveProjectAs(selected);
        }
        return saveTo(m_file_path, true);
    }

    bool saveProjectAs(const QString& path) {
        if (path.trimmed().isEmpty()) {
            setError(QStringLiteral("Choose a destination project file."));
            return false;
        }
        return saveTo(filesystem_path(path), false);
    }

    void refreshOutput() {
        // Mutable/shared Document access can replace even a same-ID,
        // same-revision head. Read the current snapshot for every output;
        // pure geometry caches remain keyed by complete entity properties.
        refreshCanvases();
    }

    [[nodiscard]] std::string currentExecutableDigest() const {
        QFile executable(QCoreApplication::applicationFilePath());
        if (!executable.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("the running application binary cannot be read for output fingerprinting");
        }
        const auto bytes = executable.readAll();
        if (bytes.isEmpty()) {
            throw std::runtime_error("the running application binary is empty");
        }
        const auto* bytes_data = reinterpret_cast<const std::byte*>(bytes.constData());
        return sha256_hex(std::span<const std::byte>(bytes_data,
                                                     static_cast<std::size_t>(bytes.size())));
    }

    [[nodiscard]] OutputFingerprintInputs outputFingerprintInputs(
        const DocumentSnapshot& snapshot, bool include_legacy_view_descriptor = true) const {
        const auto build_digest = currentExecutableDigest();
        OutputFingerprintInputs inputs;
        inputs.profiles = fingerprint_not_applicable(
            "Calculation profiles are stored in the document head.");
        inputs.fonts = fingerprint_not_applicable(
            "Draft output uses the selected local Qt font without embedding font bytes.");
        if (include_legacy_view_descriptor) {
            json view_descriptor{{"page_size", m_pageSizeCombo ? m_pageSizeCombo->currentText().toStdString()
                                                                   : std::string("A4")},
                                 {"architectural_view", architectural_view_name(m_architectural_view_kind)},
                                 {"hidden_floor_ids", std::vector<std::string>(
                                      m_view_filter.hidden_floor_ids.begin(), m_view_filter.hidden_floor_ids.end())},
                                 {"hidden_layer_ids", std::vector<std::string>(
                                      m_view_filter.hidden_layer_ids.begin(), m_view_filter.hidden_layer_ids.end())}};
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type != kSheetViewEntityType) continue;
                view_descriptor["sheet_view_model"] =
                    json{{"entity_id", id}, {"content_sha256", digest_text(entity.properties.dump())}};
                break;
            }
            inputs.views = fingerprint_resources({fingerprint_resource(
                "plan-canvas-view", view_descriptor.dump(),
                json{{"renderer", "PlanCanvas"}, {"descriptor_version", 1}})});
        }
        inputs.crs = fingerprint_not_applicable(
            "Output is expressed in local project coordinates; no georeference is active.");
        inputs.processing_components.state = FingerprintGroupState::resources;
        for (const auto role : {std::string("kernel"), std::string("solver"),
                                std::string("renderer"), std::string("adapters")}) {
            FingerprintRole role_resource;
            role_resource.state = FingerprintGroupState::resources;
            role_resource.resources.push_back(fingerprint_resource(
                "linked-" + role, build_digest, json{{"role", role}, {"identity", "application-build"}}));
            inputs.processing_components.roles.emplace(role, std::move(role_resource));
        }
        inputs.application_build = fingerprint_resources({FingerprintResource{
            "property-studio-executable", build_digest,
            json{{"kind", "Windows-native-executable"}}}});
        return inputs;
    }

    [[nodiscard]] OutputFingerprint outputFingerprintForSnapshot(
        const DocumentSnapshot& snapshot) const {
        const auto sheet = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == kSheetViewEntityType; });
        if (sheet == snapshot.entities().end()) {
            return make_output_fingerprint(snapshot, outputFingerprintInputs(snapshot));
        }

        const auto model = decode_sheet_view_entity(sheet->second);
        if (model.sheets().empty()) {
            throw std::invalid_argument("the persisted sheet graph contains no drawing sheets");
        }
        const auto inputs = outputFingerprintInputs(snapshot, false);
        const auto selected_sheet_id = outputSheetId().toStdString();
        const auto scene = make_sheet_output_scene(snapshot, sheet->first,
                                                   selected_sheet_id.empty() ? model.sheets().front().id
                                                                             : selected_sheet_id,
                                                   inputs);
        const auto current = check_sheet_output_scene_current(scene, snapshot, inputs);
        if (!current.valid) {
            throw std::invalid_argument("sheet output scene is invalid: " + current.error);
        }
        if (!current.current) {
            std::string groups;
            for (std::size_t index = 0; index < current.changed_groups.size(); ++index) {
                if (index > 0) groups += ", ";
                groups += current.changed_groups[index];
            }
            throw std::invalid_argument("sheet output scene is stale" +
                                        (groups.empty() ? std::string() : " (" + groups + ")"));
        }
        std::string error;
        const auto fingerprint = deserialize_output_fingerprint(scene.at("fingerprint"), &error);
        if (!fingerprint) {
            throw std::invalid_argument("sheet output scene fingerprint is invalid: " + error);
        }
        return *fingerprint;
    }

    bool writeOutputFingerprint(const QString& output_path, const DocumentSnapshot& snapshot,
                                QString output_kind) {
        const auto fingerprint = outputFingerprintForSnapshot(snapshot);
        const auto payload = json{{"schema", "property-studio.output-fingerprint.v1"},
                                  {"output_kind", output_kind.toStdString()},
                                  {"output_file", QFileInfo(output_path).fileName().toStdString()},
                                  {"fingerprint", serialize_output_fingerprint(fingerprint)}}.dump(2);
        QSaveFile sidecar(output_path + QStringLiteral(".fingerprint.json"));
        if (!sidecar.open(QIODevice::WriteOnly) ||
            sidecar.write(QByteArray::fromStdString(payload)) !=
                static_cast<qint64>(payload.size()) ||
            !sidecar.commit()) {
            setError(QStringLiteral("%1 export fingerprint could not be written beside the output.")
                         .arg(output_kind));
            return false;
        }
        return true;
    }

    bool writePrintReceipt(const QPrinter& printer, const QRectF& page,
                           const DocumentSnapshot& snapshot) {
        try {
            const auto receipt_path = m_file_path.empty()
                ? (std::filesystem::temp_directory_path() /
                   "property-studio-print-preview-receipt.json")
                : std::filesystem::path(m_file_path.wstring() + L".print-receipt.json");
            const auto page_mm = printer.pageRect(QPrinter::Millimeter);
            const auto paper_mm = printer.paperRect(QPrinter::Millimeter);
            const auto payload = json{
                {"schema", "property-studio.print-receipt.v1"},
                {"document_revision", snapshot.revision()},
                {"page_size", m_pageSizeCombo ? m_pageSizeCombo->currentText().toStdString()
                                                 : std::string("A4")},
                {"printer_name", printer.printerName().toStdString()},
                {"output_format", static_cast<int>(printer.outputFormat())},
                {"resolution_dpi", printer.resolution()},
                {"logical_dpi", {printer.logicalDpiX(), printer.logicalDpiY()}},
                {"physical_dpi", {printer.physicalDpiX(), printer.physicalDpiY()}},
                {"rendered_page_px", {page.x(), page.y(), page.width(), page.height()}},
                {"driver_page_mm", {page_mm.x(), page_mm.y(), page_mm.width(), page_mm.height()}},
                {"driver_paper_mm", {paper_mm.x(), paper_mm.y(), paper_mm.width(), paper_mm.height()}},
                {"verification", "preview-driver-evidence-only"},
            }.dump(2);
            QSaveFile file(QString::fromStdWString(receipt_path.wstring()));
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
                file.write(QByteArray::fromStdString(payload)) !=
                    static_cast<qint64>(payload.size()) || !file.commit()) {
                setError(QStringLiteral("Print receipt could not be written locally."));
                return false;
            }
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Print receipt failed: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool exportDraftPdf(const QString& path) {
        refreshOutput();
        if (!m_plan_geometry_error.isEmpty()) {
            setError(QStringLiteral("PDF export blocked: %1").arg(m_plan_geometry_error));
            return false;
        }
        if (path.trimmed().isEmpty()) {
            setError(QStringLiteral("Choose a PDF destination."));
            return false;
        }
        try {
            QPdfWriter writer(path);
            writer.setPageSize(QPageSize(selectedPageSize()));
            writer.setResolution(144);
            QPainter painter(&writer);
            if (!painter.isActive()) {
                setError(QStringLiteral("PDF export could not open the destination."));
                return false;
            }
            if (!renderSheetOutput(painter,
                                   QRectF(0.0, 0.0, writer.width(), writer.height()), Qt::white)) {
                painter.end();
                return false;
            }
            painter.resetTransform();
            painter.setPen(QColor(150, 50, 50));
            painter.drawText(QRectF(30.0, 30.0, writer.width() - 60.0, 80.0),
                             Qt::TextWordWrap | Qt::AlignRight | Qt::AlignTop,
                             draftOutputStamp());
            painter.end();
            if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0) {
                setError(QStringLiteral("PDF export did not produce a file."));
                return false;
            }
            if (!writeOutputFingerprint(path, m_document->snapshot(), QStringLiteral("pdf"))) {
                return false;
            }
            clearError();
            owner->statusBar()->showMessage(QStringLiteral("Draft PDF exported locally."), 5000);
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("PDF export failed: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool exportDraftSvg(const QString& path) {
        refreshOutput();
        if (!m_plan_geometry_error.isEmpty()) {
            setError(QStringLiteral("SVG export blocked: %1").arg(m_plan_geometry_error));
            return false;
        }
        if (path.trimmed().isEmpty()) {
            setError(QStringLiteral("Choose an SVG destination."));
            return false;
        }
        try {
            constexpr int width = 1600;
            constexpr int height = 1200;
            QSvgGenerator generator;
            generator.setFileName(path);
            generator.setSize(QSize(width, height));
            generator.setViewBox(QRect(0, 0, width, height));
            generator.setTitle(QStringLiteral("Property Studio draft drawing"));
            generator.setDescription(QStringLiteral("Draft output from the shared vector canvas"));
            QPainter painter(&generator);
            if (!painter.isActive()) {
                setError(QStringLiteral("SVG export could not open the destination."));
                return false;
            }
            if (!renderSheetOutput(painter, QRectF(0.0, 0.0, width, height), Qt::white)) {
                painter.end();
                return false;
            }
            painter.resetTransform();
            painter.setPen(QColor(150, 50, 50));
            painter.drawText(QRectF(30.0, 30.0, width - 60.0, 80.0),
                             Qt::TextWordWrap | Qt::AlignRight | Qt::AlignTop,
                             draftOutputStamp());
            painter.end();
            if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0) {
                setError(QStringLiteral("SVG export did not produce a file."));
                return false;
            }
            if (!writeOutputFingerprint(path, m_document->snapshot(), QStringLiteral("svg"))) {
                return false;
            }
            clearError();
            owner->statusBar()->showMessage(QStringLiteral("Draft SVG exported locally."), 5000);
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("SVG export failed: %1").arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool exportNativeViewImage(const QString& path) {
        if (path.trimmed().isEmpty()) {
            setError(QStringLiteral("Choose an image destination."));
            return false;
        }
        if (m_nativeModelView == nullptr) {
            setError(QStringLiteral("Native OCCT 3D is unavailable on this platform."));
            return false;
        }
        refreshOutput();
        if (m_workspace != Workspace::architectural) {
            setWorkspace(Workspace::architectural);
        }
        m_nativeModelView->fitAll();
        if (!m_nativeModelView->isReady()) {
            const auto reason = m_nativeModelView->lastError();
            setError(reason.isEmpty() ? QStringLiteral("Native OCCT 3D is not ready.")
                                      : QStringLiteral("Native OCCT 3D is unavailable: %1")
                                            .arg(reason));
            return false;
        }
        QTemporaryDir staging;
        if (!staging.isValid()) {
            setError(QStringLiteral("Native OCCT 3D export could not stage its draft image."));
            return false;
        }
        const auto staged_path = staging.filePath(QFileInfo(path).fileName());
        if (!m_nativeModelView->exportViewImage(staged_path)) {
            const auto reason = m_nativeModelView->lastError();
            setError(reason.isEmpty() ? QStringLiteral("Native OCCT 3D export failed.")
                                      : QStringLiteral("Native OCCT 3D export failed: %1")
                                            .arg(reason));
            return false;
        }
        if (!QFileInfo::exists(staged_path) || QFileInfo(staged_path).size() <= 0) {
            setError(QStringLiteral("Native OCCT 3D export did not produce an image."));
            return false;
        }
        if (!stampDraftImage(staged_path, draftOutputStamp())) {
            setError(QStringLiteral("Native OCCT 3D export could not write its draft stamp."));
            return false;
        }
        QFile stamped(staged_path);
        if (!stamped.open(QIODevice::ReadOnly)) {
            setError(QStringLiteral("Native OCCT 3D export could not reopen its stamped image."));
            return false;
        }
        const auto bytes = stamped.readAll();
        QSaveFile destination(path);
        if (stamped.error() != QFileDevice::NoError || bytes.isEmpty() ||
            !destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size() ||
            !destination.commit()) {
            setError(QStringLiteral("Native OCCT 3D export could not save its stamped image."));
            return false;
        }
        try {
            if (!writeOutputFingerprint(path, m_document->snapshot(), QStringLiteral("native-3d-image"))) {
                return false;
            }
        } catch (const std::exception& error) {
            setError(QStringLiteral("Native OCCT 3D output fingerprint failed: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
        clearError();
        owner->statusBar()->showMessage(QStringLiteral("Native 3D image exported locally."), 5000);
        return true;
    }

    bool showPrintPreview() {
        refreshOutput();
        if (!m_plan_geometry_error.isEmpty()) {
            setError(QStringLiteral("Print preview blocked: %1").arg(m_plan_geometry_error));
            return false;
        }
        auto* preview = new QPrintPreviewDialog(owner);
        preview->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(preview, &QPrintPreviewDialog::paintRequested, owner,
                         [this](QPrinter* printer) {
                             refreshOutput();
                             if (!m_plan_geometry_error.isEmpty()) {
                                 setError(QStringLiteral("Printing blocked: %1").arg(m_plan_geometry_error));
                                 return;
                             }
                             printer->setPageSize(QPageSize(selectedPageSize()));
                             QPainter painter(printer);
                             const auto page = printer->pageRect(QPrinter::DevicePixel);
                             if (!renderSheetOutput(painter, QRectF(page), Qt::white)) return;
                             painter.resetTransform();
                             painter.setPen(QColor(150, 50, 50));
                             painter.drawText(QRectF(page.left() + 24.0, page.top() + 24.0,
                                                     page.width() - 48.0, 80.0),
                                              Qt::TextWordWrap | Qt::AlignRight | Qt::AlignTop,
                                              draftOutputStamp());
                             if (!writePrintReceipt(*printer, QRectF(page), m_document->snapshot())) return;
                         });
        preview->open();
        return true;
    }

    void showSchedules() {
        const auto projection = scheduleSnapshot();
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setWindowTitle(QStringLiteral("Schedules"));
        dialog.setModal(true);
        dialog.resize(780, 480);
        auto* layout = new QVBoxLayout(&dialog);
        auto* heading = new QLabel(
            QStringLiteral("Document revision %1  •  edit source cells; calculated cells expose their sources")
                .arg(projection.snapshot.revision), &dialog);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* table = new QTableWidget(&dialog);
        table->setObjectName(QStringLiteral("scheduleTable"));
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectItems);
        table->setAlternatingRowColors(true);
        std::set<std::string> column_names{"kind", "mark"};
        for (const auto& row : projection.snapshot.rows)
            for (const auto& [name, unused] : row.cells) {
                (void)unused;
                column_names.insert(name);
            }
        std::vector<std::string> columns(column_names.begin(), column_names.end());
        table->setColumnCount(static_cast<int>(columns.size()));
        QStringList headers;
        for (const auto& column : columns) headers.push_back(QString::fromStdString(column));
        table->setHorizontalHeaderLabels(headers);
        table->setRowCount(static_cast<int>(projection.snapshot.rows.size()));
        for (int row_index = 0; row_index < table->rowCount(); ++row_index) {
            const auto& row = projection.snapshot.rows[static_cast<std::size_t>(row_index)];
            for (int column_index = 0; column_index < table->columnCount(); ++column_index) {
                const auto& column = columns[static_cast<std::size_t>(column_index)];
                QString text;
                QString tooltip;
                if (column == "kind") {
                    text = schedule_kind_text(row.kind);
                } else if (column == "mark") {
                    text = QString::fromStdString(row.mark);
                } else if (const auto cell = row.cells.find(column); cell != row.cells.end()) {
                    text = schedule_value_text(cell->second.value);
                    if (!cell->second.editable) {
                        tooltip = QStringLiteral("Calculated: %1")
                                      .arg(QString::fromStdString(cell->second.explanation));
                        for (const auto& source : cell->second.sources) {
                            tooltip += QStringLiteral("\n%1.%2")
                                           .arg(QString::fromStdString(source.object_id),
                                                QString::fromStdString(source.property));
                        }
                    }
                }
                auto* item = new QTableWidgetItem(text);
                if (!tooltip.isEmpty()) item->setToolTip(tooltip);
                table->setItem(row_index, column_index, item);
            }
        }
        table->resizeColumnsToContents();
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        auto* edit_button = new QPushButton(QStringLiteral("Edit selected source cell"), &dialog);
        edit_button->setEnabled(false);
        layout->addWidget(edit_button);
        const auto update_edit_button = [&, edit_button] {
            const auto row_index = table->currentRow();
            const auto column_index = table->currentColumn();
            bool editable = row_index >= 0 && row_index < static_cast<int>(projection.snapshot.rows.size()) &&
                            column_index >= 0 && column_index < static_cast<int>(columns.size());
            if (editable) {
                const auto& row = projection.snapshot.rows[static_cast<std::size_t>(row_index)];
                const auto cell = row.cells.find(columns[static_cast<std::size_t>(column_index)]);
                editable = cell != row.cells.end() && cell->second.editable;
            }
            edit_button->setEnabled(editable);
        };
        QObject::connect(table, &QTableWidget::itemSelectionChanged, &dialog,
                         update_edit_button);
        const auto edit_selected_cell = [&, edit_button] {
            const auto row_index = table->currentRow();
            const auto column_index = table->currentColumn();
            if (row_index < 0 || column_index < 0 ||
                row_index >= static_cast<int>(projection.snapshot.rows.size()) ||
                column_index >= static_cast<int>(columns.size())) return;
            const auto& row = projection.snapshot.rows[static_cast<std::size_t>(row_index)];
            const auto& column = columns[static_cast<std::size_t>(column_index)];
            const auto cell = row.cells.find(column);
            if (cell == row.cells.end() || !cell->second.editable) return;
            bool accepted = false;
            const auto current = schedule_value_text(cell->second.value);
            const auto value = QInputDialog::getText(
                &dialog, QStringLiteral("Edit schedule cell"),
                QStringLiteral("%1.%2 (source value):").arg(QString::fromStdString(row.object_id),
                                                               QString::fromStdString(column)),
                QLineEdit::Normal, current, &accepted);
            if (!accepted) return;
            if (editScheduleCell(QString::fromStdString(row.object_id),
                                 QString::fromStdString(column), value)) {
                dialog.accept();
            } else {
                update_edit_button();
            }
        };
        QObject::connect(edit_button, &QPushButton::clicked, &dialog, edit_selected_cell);
        QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog,
                         [edit_selected_cell](int, int) { edit_selected_cell(); });
        update_edit_button();
        if (!projection.diagnostics.empty()) {
            auto* diagnostics = new QLabel(&dialog);
            diagnostics->setWordWrap(true);
            QString text = QStringLiteral("Diagnostics:");
            for (const auto& message : projection.diagnostics)
                text += QStringLiteral("\n• %1").arg(QString::fromStdString(message));
            diagnostics->setText(text);
            diagnostics->setStyleSheet(QStringLiteral("color:#8b1a1a;"));
            layout->addWidget(diagnostics);
        }
        dialog.exec();
    }

    void showSheetSettings() {
        ModalContext context = captureModalContext();
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("sheetSettingsDialog"));
        dialog.setWindowTitle(QStringLiteral("Drawing sheets"));
        dialog.setModal(true);
        dialog.resize(700, 500);
        auto* root = new QVBoxLayout(&dialog);
        auto* sheet_row = new QHBoxLayout();
        auto* sheet_label = new QLabel(QStringLiteral("Output sheet"), &dialog);
        auto* selector = new QComboBox(&dialog);
        selector->setObjectName(QStringLiteral("sheetSelector"));
        selector->setMinimumWidth(200);
        selector->setToolTip(QStringLiteral("Select the sheet used by draft PDF, SVG, and print output"));
        auto* add = new QPushButton(QStringLiteral("Add sheet…"), &dialog);
        add->setObjectName(QStringLiteral("addSheet"));
        auto* remove = new QPushButton(QStringLiteral("Remove"), &dialog);
        remove->setObjectName(QStringLiteral("removeSheet"));
        sheet_row->addWidget(sheet_label);
        sheet_row->addWidget(selector, 1);
        sheet_row->addWidget(add);
        sheet_row->addWidget(remove);
        root->addLayout(sheet_row);

        auto* form = new QFormLayout();
        auto* number = new QLineEdit(&dialog);
        auto* project = new QLineEdit(&dialog);
        auto* title = new QLineEdit(&dialog);
        auto* author = new QLineEdit(&dialog);
        auto* issue_date = new QLineEdit(&dialog);
        number->setObjectName(QStringLiteral("sheetNumber"));
        project->setObjectName(QStringLiteral("sheetProject"));
        title->setObjectName(QStringLiteral("sheetTitle"));
        author->setObjectName(QStringLiteral("sheetAuthor"));
        issue_date->setObjectName(QStringLiteral("sheetIssueDate"));
        form->addRow(QStringLiteral("Sheet number"), number);
        form->addRow(QStringLiteral("Project"), project);
        form->addRow(QStringLiteral("Title"), title);
        form->addRow(QStringLiteral("Author"), author);
        form->addRow(QStringLiteral("Issue date"), issue_date);
        auto* apply = new QPushButton(QStringLiteral("Apply sheet metadata"), &dialog);
        apply->setObjectName(QStringLiteral("applySheetMetadata"));
        form->addRow(apply);
        root->addLayout(form);
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("sheetSettingsStatus"));
        status->setWordWrap(true);
        root->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        root->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        const auto selected_id = [&] {
            return selector->currentData().toString();
        };
        const auto fill_fields = [&] {
            try {
                const auto source = authoringSnapshot();
                const auto record = decode_sheet_model(source);
                if (!record) {
                    status->setText(QStringLiteral("No drawing sheets are defined."));
                    return;
                }
                const auto id = selected_id().toStdString();
                const auto found = std::find_if(record->model.sheets().begin(), record->model.sheets().end(),
                                                [&](const auto& sheet) { return sheet.id == id; });
                if (found == record->model.sheets().end()) {
                    status->setText(QStringLiteral("Choose a drawing sheet."));
                    return;
                }
                number->setText(QString::fromStdString(found->number));
                project->setText(QString::fromStdString(found->title_block.project));
                title->setText(QString::fromStdString(found->title_block.title));
                author->setText(QString::fromStdString(found->title_block.author));
                issue_date->setText(QString::fromStdString(found->title_block.issue_date));
                status->setText(QStringLiteral("%1 viewports • %2 callouts • %3 schedule placements • selected for output")
                                    .arg(static_cast<int>(found->viewports.size()))
                                    .arg(static_cast<int>(found->callouts.size()))
                                    .arg(static_cast<int>(found->schedules.size())));
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Sheet data is unavailable: %1")
                                    .arg(QString::fromUtf8(error.what())));
            }
        };
        const auto fill_selector = [&] {
            try {
                const auto source = authoringSnapshot();
                const auto record = decode_sheet_model(source);
                QSignalBlocker block(selector);
                selector->clear();
                if (!record) {
                    fill_fields();
                    return;
                }
                auto wanted = outputSheetId();
                int wanted_index = -1;
                for (int index = 0; index < static_cast<int>(record->model.sheets().size()); ++index) {
                    const auto& sheet = record->model.sheets()[static_cast<std::size_t>(index)];
                    selector->addItem(QStringLiteral("%1  ·  %2")
                                          .arg(QString::fromStdString(sheet.number),
                                               QString::fromStdString(sheet.title_block.title)),
                                      QString::fromStdString(sheet.id));
                    if (QString::fromStdString(sheet.id) == wanted) wanted_index = index;
                }
                if (wanted_index < 0 && selector->count() > 0) wanted_index = 0;
                if (wanted_index >= 0) selector->setCurrentIndex(wanted_index);
                fill_fields();
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Sheet list is unavailable: %1")
                                    .arg(QString::fromUtf8(error.what())));
            }
        };
        QObject::connect(selector, &QComboBox::currentIndexChanged, &dialog,
                         [&](int index) {
                             if (index < 0) return;
                             if (selectOutputSheet(selector->itemData(index).toString())) {
                                 fill_fields();
                             }
                         });
        QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
            if (selected_id().isEmpty()) return;
            if (!modalContextUnchanged(context)) {
                context = captureModalContext();
                fill_selector();
                return;
            }
            if (editSheetMetadata(selected_id(), number->text(), project->text(), title->text(),
                                  author->text(), issue_date->text())) {
                context = captureModalContext();
                fill_selector();
            }
        });
        QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
            bool accepted = false;
            const auto new_number = QInputDialog::getText(
                &dialog, QStringLiteral("Add drawing sheet"), QStringLiteral("Sheet number:"),
                QLineEdit::Normal, QStringLiteral("A-201"), &accepted).trimmed();
            if (!accepted || new_number.isEmpty()) return;
            const auto new_title = QInputDialog::getText(
                &dialog, QStringLiteral("Add drawing sheet"), QStringLiteral("Sheet title:"),
                QLineEdit::Normal, QStringLiteral("New sheet"), &accepted).trimmed();
            if (!accepted) return;
            if (!createDrawingSheet(new_number, QStringLiteral("420"), QStringLiteral("297"), new_title).isEmpty()) {
                context = captureModalContext();
                fill_selector();
            }
        });
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
            const auto id = selected_id();
            if (id.isEmpty()) return;
            if (QMessageBox::question(&dialog, QStringLiteral("Remove drawing sheet"),
                                      QStringLiteral("Remove the selected sheet? This can be undone.")) !=
                QMessageBox::Yes) return;
            if (removeDrawingSheet(id)) {
                context = captureModalContext();
                fill_selector();
            }
        });
        try {
            fill_selector();
            dialog.exec();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Sheet settings: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void showViewportSettings() {
        const auto context = captureModalContext();
        const auto source = authoringSnapshot();
        const auto sheet_entity = std::find_if(source.entities().begin(), source.entities().end(),
            [](const auto& entry) { return entry.second.type == kSheetViewEntityType; });
        if (sheet_entity == source.entities().end()) {
            setError(QStringLiteral("No typed drawing sheet is available."));
            return;
        }
        try {
            const auto model = decode_sheet_view_entity(sheet_entity->second);
            if (model.sheets().empty() || model.sheets().front().viewports.empty())
                throw std::invalid_argument("no sheet viewport is defined");
            const auto& sheet = model.sheets().front();
            const auto& viewport = sheet.viewports.front();
            const auto number = [](double value) {
                return QString::number(value, 'g', 12);
            };
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setWindowTitle(QStringLiteral("Viewport settings"));
            dialog.setModal(true);
            auto* form = new QFormLayout(&dialog);
            auto* x = new QLineEdit(number(viewport.bounds.x_mm), &dialog);
            auto* y = new QLineEdit(number(viewport.bounds.y_mm), &dialog);
            auto* width = new QLineEdit(number(viewport.bounds.width_mm), &dialog);
            auto* height = new QLineEdit(number(viewport.bounds.height_mm), &dialog);
            auto* scale = new QLineEdit(number(viewport.scale_denominator), &dialog);
            x->setObjectName(QStringLiteral("viewportX"));
            y->setObjectName(QStringLiteral("viewportY"));
            width->setObjectName(QStringLiteral("viewportWidth"));
            height->setObjectName(QStringLiteral("viewportHeight"));
            scale->setObjectName(QStringLiteral("viewportScale"));
            form->addRow(QStringLiteral("X (mm)"), x);
            form->addRow(QStringLiteral("Y (mm)"), y);
            form->addRow(QStringLiteral("Width (mm)"), width);
            form->addRow(QStringLiteral("Height (mm)"), height);
            form->addRow(QStringLiteral("Scale denominator"), scale);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted || !modalContextUnchanged(context)) return;
            (void)editSheetViewport(QString::fromStdString(sheet.id),
                                    QString::fromStdString(viewport.id), x->text(), y->text(),
                                    width->text(), height->text(), scale->text());
        } catch (const std::exception& error) {
            setError(QStringLiteral("Viewport settings: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void showSchedulePlacementSettings() {
        const auto context = captureModalContext();
        const auto source = authoringSnapshot();
        const auto sheet_entity = std::find_if(source.entities().begin(), source.entities().end(),
            [](const auto& entry) { return entry.second.type == kSheetViewEntityType; });
        if (sheet_entity == source.entities().end()) {
            setError(QStringLiteral("No typed drawing sheet is available."));
            return;
        }
        try {
            const auto model = decode_sheet_view_entity(sheet_entity->second);
            if (model.sheets().empty() || model.sheets().front().schedules.empty())
                throw std::invalid_argument("no schedule placement is defined");
            const auto& sheet = model.sheets().front();
            const auto& placement = sheet.schedules.front();
            const auto number = [](double value) { return QString::number(value, 'g', 12); };
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setWindowTitle(QStringLiteral("Schedule placement settings"));
            dialog.setModal(true);
            auto* form = new QFormLayout(&dialog);
            auto* schedule = new QLineEdit(QString::fromStdString(placement.schedule_id), &dialog);
            auto* x = new QLineEdit(number(placement.bounds.x_mm), &dialog);
            auto* y = new QLineEdit(number(placement.bounds.y_mm), &dialog);
            auto* width = new QLineEdit(number(placement.bounds.width_mm), &dialog);
            auto* height = new QLineEdit(number(placement.bounds.height_mm), &dialog);
            schedule->setReadOnly(true);
            schedule->setObjectName(QStringLiteral("schedulePlacementName"));
            x->setObjectName(QStringLiteral("schedulePlacementX"));
            y->setObjectName(QStringLiteral("schedulePlacementY"));
            width->setObjectName(QStringLiteral("schedulePlacementWidth"));
            height->setObjectName(QStringLiteral("schedulePlacementHeight"));
            form->addRow(QStringLiteral("Schedule"), schedule);
            form->addRow(QStringLiteral("X (mm)"), x);
            form->addRow(QStringLiteral("Y (mm)"), y);
            form->addRow(QStringLiteral("Width (mm)"), width);
            form->addRow(QStringLiteral("Height (mm)"), height);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted || !modalContextUnchanged(context)) return;
            (void)editSheetSchedulePlacement(QString::fromStdString(sheet.id),
                                              QString::fromStdString(placement.id), x->text(), y->text(),
                                              width->text(), height->text());
        } catch (const std::exception& error) {
            setError(QStringLiteral("Schedule placement settings: %1")
                         .arg(QString::fromUtf8(error.what())));
        }
    }

    void showArchitecturalViewSettings() {
        const auto context = captureModalContext();
        const auto source = authoringSnapshot();
        const auto sheet_entity = std::find_if(source.entities().begin(), source.entities().end(),
            [](const auto& entry) { return entry.second.type == kSheetViewEntityType; });
        if (sheet_entity == source.entities().end()) {
            setError(QStringLiteral("No typed architectural view is available."));
            return;
        }
        try {
            const auto model = decode_sheet_view_entity(sheet_entity->second);
            const auto expected_kind = m_architectural_view_kind == BuildingViewKind::plan
                ? CoordinatedViewKind::plan
                : m_architectural_view_kind == BuildingViewKind::elevation
                ? CoordinatedViewKind::elevation : CoordinatedViewKind::section;
            const auto found = std::find_if(model.views().begin(), model.views().end(),
                [&](const auto& view) { return view.kind == expected_kind; });
            if (found == model.views().end())
                throw std::invalid_argument("the selected architectural view is not defined");
            const auto number = [](double value) { return QString::number(value, 'g', 12); };
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setWindowTitle(QStringLiteral("Architectural view settings"));
            dialog.setModal(true);
            auto* form = new QFormLayout(&dialog);
            auto* cut = new QLineEdit(number(found->presentation.cut_depth_m), &dialog);
            auto* far = new QLineEdit(number(found->presentation.far_depth_m), &dialog);
            auto* cut_line = new QLineEdit(number(found->presentation.cut_line_mm), &dialog);
            auto* projection_line = new QLineEdit(number(found->presentation.projection_line_mm), &dialog);
            auto* pattern = new QLineEdit(QString::fromStdString(found->presentation.hatch_pattern), &dialog);
            auto* hatch_scale = new QLineEdit(number(found->presentation.hatch_scale), &dialog);
            auto* hatch = new QCheckBox(QStringLiteral("Enable material hatching"), &dialog);
            auto* detail = new QComboBox(&dialog);
            detail->addItems({QStringLiteral("Coarse"), QStringLiteral("Medium"), QStringLiteral("Fine")});
            detail->setCurrentIndex(static_cast<int>(found->presentation.detail));
            cut->setObjectName(QStringLiteral("viewCutDepth"));
            far->setObjectName(QStringLiteral("viewFarDepth"));
            cut_line->setObjectName(QStringLiteral("viewCutLine"));
            projection_line->setObjectName(QStringLiteral("viewProjectionLine"));
            pattern->setObjectName(QStringLiteral("viewHatchPattern"));
            hatch_scale->setObjectName(QStringLiteral("viewHatchScale"));
            hatch->setObjectName(QStringLiteral("viewHatchEnabled"));
            detail->setObjectName(QStringLiteral("viewDetail"));
            hatch->setChecked(found->presentation.hatch_enabled);
            form->addRow(QStringLiteral("Cut depth (m)"), cut);
            form->addRow(QStringLiteral("Far depth (m)"), far);
            form->addRow(QStringLiteral("Cut line width (mm)"), cut_line);
            form->addRow(QStringLiteral("Projection line width (mm)"), projection_line);
            form->addRow(QStringLiteral("Hatch pattern"), pattern);
            form->addRow(QStringLiteral("Hatch scale"), hatch_scale);
            form->addRow(hatch);
            form->addRow(QStringLiteral("Detail"), detail);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted || !modalContextUnchanged(context)) return;
            (void)editArchitecturalViewPresentation(
                QString::fromStdString(found->id), cut->text(), far->text(), cut_line->text(),
                projection_line->text(), hatch->isChecked(), pattern->text(), hatch_scale->text(),
                detail->currentText());
        } catch (const std::exception& error) {
            setError(QStringLiteral("Architectural view settings: %1")
                         .arg(QString::fromUtf8(error.what())));
        }
    }

    void showAnnotationEditor() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setWindowTitle(QStringLiteral("Add annotations"));
        dialog.setModal(true);
        dialog.resize(520, 300);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* label_template = new QComboBox(&dialog);
        label_template->setObjectName(QStringLiteral("annotationLabelTemplate"));
        for (const auto& definition : default_label_templates()) {
            label_template->addItem(
                QString::fromStdString(definition.content),
                QString::fromStdString(definition.id));
        }
        auto* label_content = new QLineEdit(&dialog);
        label_content->setObjectName(QStringLiteral("annotationLabelContent"));
        label_content->setPlaceholderText(QStringLiteral("Uses the template text when empty"));
        auto* label_x = new QLineEdit(QStringLiteral("0"), &dialog);
        auto* label_y = new QLineEdit(QStringLiteral("0"), &dialog);
        label_x->setObjectName(QStringLiteral("annotationLabelX"));
        label_y->setObjectName(QStringLiteral("annotationLabelY"));
        form->addRow(QStringLiteral("Label template"), label_template);
        form->addRow(QStringLiteral("Label text"), label_content);
        form->addRow(QStringLiteral("Label X (m)"), label_x);
        form->addRow(QStringLiteral("Label Y (m)"), label_y);

        auto* symbol = new QComboBox(&dialog);
        symbol->setObjectName(QStringLiteral("annotationSymbol"));
        for (const auto& definition : default_symbol_catalog()) {
            symbol->addItem(QStringLiteral("%1  (%2 × %3 m)")
                                .arg(QString::fromStdString(definition.family))
                                .arg(QString::number(definition.width_metres, 'g', 4))
                                .arg(QString::number(definition.depth_metres, 'g', 4)),
                            QString::fromStdString(definition.id));
        }
        auto* symbol_x = new QLineEdit(QStringLiteral("0"), &dialog);
        auto* symbol_y = new QLineEdit(QStringLiteral("0"), &dialog);
        symbol_x->setObjectName(QStringLiteral("annotationSymbolX"));
        symbol_y->setObjectName(QStringLiteral("annotationSymbolY"));
        form->addRow(QStringLiteral("Symbol"), symbol);
        form->addRow(QStringLiteral("Symbol X (m)"), symbol_x);
        form->addRow(QStringLiteral("Symbol Y (m)"), symbol_y);
        layout->addLayout(form);

        auto* actions = new QHBoxLayout;
        auto* add_label = new QPushButton(QStringLiteral("Add label"), &dialog);
        auto* add_symbol = new QPushButton(QStringLiteral("Add symbol"), &dialog);
        add_label->setObjectName(QStringLiteral("addAnnotationLabel"));
        add_symbol->setObjectName(QStringLiteral("addAnnotationSymbol"));
        actions->addWidget(add_label);
        actions->addWidget(add_symbol);
        actions->addStretch();
        layout->addLayout(actions);
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("annotationEditorStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        const auto parse_position = [](const QLineEdit* x, const QLineEdit* y) {
            bool x_ok = false;
            bool y_ok = false;
            const auto point = Vec2{x->text().trimmed().toDouble(&x_ok),
                                    y->text().trimmed().toDouble(&y_ok)};
            if (!x_ok || !y_ok || !std::isfinite(point.x) || !std::isfinite(point.y)) {
                throw std::invalid_argument("Annotation coordinates must be finite metres.");
            }
            return point;
        };
        QObject::connect(add_label, &QPushButton::clicked, &dialog, [this, &dialog, label_template,
                                                                      label_content, label_x, label_y,
                                                                      status, parse_position] {
            try {
                const auto position = parse_position(label_x, label_y);
                const auto id = createAnnotationLabel(label_template->currentData().toString(),
                                                       label_content->text(), position);
                if (id.isEmpty()) throw std::invalid_argument(lastError().toStdString());
                status->setText(QStringLiteral("Added label %1").arg(id));
                Q_UNUSED(dialog);
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Label: %1").arg(QString::fromUtf8(error.what())));
            }
        });
        QObject::connect(add_symbol, &QPushButton::clicked, &dialog, [this, &dialog, symbol,
                                                                       symbol_x, symbol_y, status,
                                                                       parse_position] {
            try {
                const auto position = parse_position(symbol_x, symbol_y);
                const auto id = createAnnotationSymbol(symbol->currentData().toString(), position);
                if (id.isEmpty()) throw std::invalid_argument(lastError().toStdString());
                status->setText(QStringLiteral("Added symbol %1").arg(id));
                Q_UNUSED(dialog);
            } catch (const std::exception& error) {
                status->setText(QStringLiteral("Symbol: %1").arg(QString::fromUtf8(error.what())));
            }
        });
        dialog.exec();
    }

    void showReferenceImport() {
        const auto selected = QFileDialog::getOpenFileName(
            owner, QStringLiteral("Import reference image"), {},
            QStringLiteral("Reference files (*.pdf *.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
        if (!selected.isEmpty()) {
            int page_index = 0;
            if (QFileInfo(selected).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
                QPdfDocument pdf;
                if (pdf.load(selected) == QPdfDocument::Error::None && pdf.pageCount() > 1) {
                    bool accepted = false;
                    const auto page = QInputDialog::getInt(owner, QStringLiteral("PDF reference page"),
                        QStringLiteral("Page to import as a raster underlay:"), 1, 1,
                        pdf.pageCount(), 1, &accepted);
                    if (!accepted) return;
                    page_index = page - 1;
                }
            }
            (void)importReferenceImage(selected, page_index);
        }
    }

    void showReferenceCalibration() {
        const auto snapshot = m_document->snapshot();
        const auto found = snapshot.entities().find(m_selected_id.toStdString());
        if (found == snapshot.entities().end() || found->second.type != "reference_asset") {
            setError(QStringLiteral("Select a reference image before calibrating it."));
            return;
        }
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setWindowTitle(QStringLiteral("Calibrate reference image"));
        auto* form = new QFormLayout(&dialog);
        auto* first_x = new QLineEdit(QStringLiteral("0"), &dialog);
        auto* first_y = new QLineEdit(QStringLiteral("0"), &dialog);
        auto* second_x = new QLineEdit(QStringLiteral("100"), &dialog);
        auto* second_y = new QLineEdit(QStringLiteral("0"), &dialog);
        auto* known_distance = new QLineEdit(QStringLiteral("1 m"), &dialog);
        first_x->setObjectName(QStringLiteral("referenceCalibrationFirstX"));
        first_y->setObjectName(QStringLiteral("referenceCalibrationFirstY"));
        second_x->setObjectName(QStringLiteral("referenceCalibrationSecondX"));
        second_y->setObjectName(QStringLiteral("referenceCalibrationSecondY"));
        known_distance->setObjectName(QStringLiteral("referenceCalibrationDistance"));
        form->addRow(QStringLiteral("First X (px)"), first_x);
        form->addRow(QStringLiteral("First Y (px)"), first_y);
        form->addRow(QStringLiteral("Second X (px)"), second_x);
        form->addRow(QStringLiteral("Second Y (px)"), second_y);
        form->addRow(QStringLiteral("Known distance"), known_distance);
        auto* status = new QLabel(&dialog);
        status->setWordWrap(true);
        form->addRow(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, status,
                                                                           first_x, first_y,
                                                                           second_x, second_y,
                                                                           known_distance] {
            if (calibrateReference(m_selected_id, first_x->text(), first_y->text(),
                                   second_x->text(), second_y->text(), known_distance->text())) {
                dialog.accept();
            } else {
                status->setText(lastError());
            }
        });
        dialog.exec();
    }

    struct ShortcutBinding {
        QString id;
        QAction* action{};
        QKeySequence standard;
        QKeySequence apex;
    };

    QString shortcutSettingsPath() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
               QStringLiteral("/keyboard-shortcuts.json");
    }

    QString validateShortcuts(const std::vector<QKeySequence>& keys) const {
        // Canvas editing and text controls retain their ordinary editing keys.
        const std::vector<QKeySequence> reserved{
            QKeySequence::Undo, QKeySequence::Redo, QKeySequence::Copy,
            QKeySequence::Cut, QKeySequence::Paste, QKeySequence::SelectAll,
            QKeySequence(QStringLiteral("Ctrl+Shift+Z"))};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto& sequence = keys[index];
            if (sequence.isEmpty()) continue;
            if (sequence.count() != 1) return QStringLiteral("Use one key combination per command.");
            const auto key = sequence[0].key();
            const auto modifiers = sequence[0].keyboardModifiers();
            if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift ||
                key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_Tab || key == Qt::Key_Backtab ||
                key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape ||
                key == Qt::Key_Backspace || key == Qt::Key_Delete || key == Qt::Key_Insert ||
                (key >= Qt::Key_Home && key <= Qt::Key_PageDown) ||
                (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                 !(key >= Qt::Key_F1 && key <= Qt::Key_F35)) ||
                std::find(reserved.begin(), reserved.end(), sequence) != reserved.end()) {
                return QStringLiteral("%1 is reserved for text entry or canvas editing.")
                    .arg(sequence.toString(QKeySequence::NativeText));
            }
            for (std::size_t other = 0; other < index; ++other) {
                if (keys[other] == sequence) {
                    return QStringLiteral("%1 is assigned more than once.")
                        .arg(sequence.toString(QKeySequence::NativeText));
                }
            }
        }
        return {};
    }

    void initializeShortcuts() {
        const auto add = [this](QString id, QAction* action, const char* standard, const char* apex) {
            m_shortcuts.push_back({std::move(id), action, QKeySequence(QString::fromLatin1(standard)),
                                  QKeySequence(QString::fromLatin1(apex))});
        };
        add(QStringLiteral("new"), m_new_action, "Ctrl+N", "Ctrl+N");
        add(QStringLiteral("open"), m_open_action, "Ctrl+O", "F3");
        add(QStringLiteral("save"), m_save_action, "Ctrl+S", "F2");
        add(QStringLiteral("save-as"), m_save_as_action, "Ctrl+Shift+S", "Ctrl+Shift+S");
        add(QStringLiteral("commands"), m_palette_action, "Ctrl+K", "Ctrl+K");
        add(QStringLiteral("measurement"), m_measurement_action, "Ctrl+1", "Ctrl+1");
        add(QStringLiteral("architectural"), m_architectural_action, "Ctrl+2", "Ctrl+2");
        add(QStringLiteral("annotations"), m_annotation_action, "Ctrl+Shift+A", "Ctrl+Shift+A");
        auto* define = new QAction(QStringLiteral("Define area before drawing"), owner);
        define->setObjectName(QStringLiteral("defineAreaShortcut"));
        owner->addAction(define);
        QObject::connect(define, &QAction::triggered, owner, [this] {
            (void)beginBoundaryDrawing(BoundaryAuthoringMode::define_first, {});
        });
        add(QStringLiteral("define-area"), define, "Ctrl+Shift+D", "F4");
        for (const auto& binding : m_shortcuts) binding.action->setShortcut(binding.standard);

        QFile file(shortcutSettingsPath());
        if (!file.exists()) return;
        try {
            if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024)
                throw std::runtime_error("Shortcut settings cannot be read.");
            const auto settings_document = json::parse(file.readAll().toStdString());
            if (!settings_document.is_object() || settings_document.size() != 2 ||
                !settings_document.at("version").is_number_integer() || settings_document.at("version") != 1 ||
                !settings_document.at("bindings").is_object() || settings_document.at("bindings").size() != m_shortcuts.size())
                throw std::runtime_error("Unsupported shortcut settings.");
            std::vector<QKeySequence> keys;
            for (const auto& binding : m_shortcuts) {
                const auto text = QString::fromStdString(
                    settings_document.at("bindings").at(binding.id.toStdString()).get<std::string>());
                const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
                if (sequence.toString(QKeySequence::PortableText) != text)
                    throw std::runtime_error("Invalid shortcut text.");
                keys.push_back(sequence);
            }
            if (!validateShortcuts(keys).isEmpty()) throw std::runtime_error("Conflicting or reserved shortcuts.");
            for (std::size_t index = 0; index < keys.size(); ++index)
                m_shortcuts[index].action->setShortcut(keys[index]);
        } catch (const std::exception&) {
            m_shortcut_load_error = QStringLiteral("Saved shortcuts could not be loaded. Default bindings are active.");
        }
    }

    void showShortcutSettings() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("keyboardShortcutDialog"));
        dialog.setWindowTitle(QStringLiteral("Keyboard shortcuts"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* presets = new QComboBox(&dialog);
        presets->setObjectName(QStringLiteral("keyboardShortcutPreset"));
        presets->addItems({QStringLiteral("Current bindings"), QStringLiteral("Property Studio defaults"),
                           QStringLiteral("Apex v7 compatible subset")});
        layout->addWidget(presets);
        auto* help = new QLabel(QStringLiteral(
            "Apex subset: F2 Save, F3 Open, F4 Define area. Other commands retain Studio defaults. "
            "Text editing and canvas keys remain reserved. Clear a binding to disable it."), &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* form = new QFormLayout;
        std::vector<QKeySequenceEdit*> editors;
        for (const auto& binding : m_shortcuts) {
            auto* edit = new QKeySequenceEdit(binding.action->shortcut(), &dialog);
            edit->setMaximumSequenceLength(1);
            edit->setClearButtonEnabled(true);
            edit->setObjectName(QStringLiteral("shortcut-") + binding.id);
            edit->setAccessibleName(binding.action->text());
            form->addRow(binding.action->text(), edit);
            editors.push_back(edit);
        }
        layout->addLayout(form);
        auto* status = new QLabel(m_shortcut_load_error, &dialog);
        status->setObjectName(QStringLiteral("keyboardShortcutStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        buttons->setObjectName(QStringLiteral("keyboardShortcutButtons"));
        layout->addWidget(buttons);
        QObject::connect(presets, &QComboBox::currentIndexChanged, &dialog, [&](int preset) {
            for (std::size_t index = 0; index < editors.size(); ++index) {
                const auto& binding = m_shortcuts[index];
                editors[index]->setKeySequence(preset == 1 ? binding.standard :
                                               preset == 2 ? binding.apex : binding.action->shortcut());
            }
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            std::vector<QKeySequence> keys;
            for (auto* editor : editors) keys.push_back(editor->keySequence());
            const auto error = validateShortcuts(keys);
            if (!error.isEmpty()) { status->setText(error); return; }
            json settings_document{{"version", 1}, {"bindings", json::object()}};
            for (std::size_t index = 0; index < keys.size(); ++index)
                settings_document["bindings"][m_shortcuts[index].id.toStdString()] =
                    keys[index].toString(QKeySequence::PortableText).toStdString();
            const auto bytes = QByteArray::fromStdString(settings_document.dump(2));
            QSaveFile file(shortcutSettingsPath());
            if (!QDir().mkpath(QFileInfo(file.fileName()).absolutePath()) ||
                !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                status->setText(QStringLiteral("Could not save shortcuts. Existing bindings remain active."));
                return;
            }
            for (std::size_t index = 0; index < keys.size(); ++index)
                m_shortcuts[index].action->setShortcut(keys[index]);
            m_shortcut_load_error.clear();
            dialog.accept();
        });
        dialog.exec();
    }

    void showMeasurementKeypad() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("measurementKeypadDialog"));
        dialog.setWindowTitle(QStringLiteral("Measurement keypad"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* target = new QComboBox(&dialog);
        target->setObjectName(QStringLiteral("measurementKeypadTarget"));
        target->setAccessibleName(QStringLiteral("Dimension to edit"));
        const std::array<QLineEdit*, 3> fields{m_length_edit, m_height_edit, m_thickness_edit};
        const std::array<QString, 3> names{QStringLiteral("Length"), QStringLiteral("Height"), QStringLiteral("Thickness")};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index]->isEnabled() && !fields[index]->isReadOnly() && !fields[index]->isHidden())
                target->addItem(names[index], static_cast<int>(index));
        }
        layout->addWidget(target);
        auto* input = new QLineEdit(&dialog);
        input->setObjectName(QStringLiteral("measurementKeypadValue"));
        input->setAccessibleName(QStringLiteral("Measurement expression"));
        layout->addWidget(input);
        auto* hint = new QLabel(m_metric_units ? QStringLiteral("Default unit: metres. Fractions and explicit units are accepted.") :
                                              QStringLiteral("Default unit: feet. Fractions and explicit units are accepted."), &dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto* grid = new QGridLayout;
        const QStringList tokens{QStringLiteral("7"), QStringLiteral("8"), QStringLiteral("9"), QStringLiteral("/"),
                                 QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("."),
                                 QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral(" "),
                                 QStringLiteral("0"), QStringLiteral("ft"), QStringLiteral("in"), QStringLiteral("m"),
                                 QStringLiteral("mm"), QStringLiteral("cm")};
        for (int index = 0; index < tokens.size(); ++index) {
            const auto token = tokens[index];
            auto* button = new QPushButton(token == QStringLiteral(" ") ? QStringLiteral("Space") : token, &dialog);
            button->setObjectName(QStringLiteral("measurementKeypadToken%1").arg(index));
            button->setMinimumSize(48, 40);
            button->setAutoDefault(false);
            grid->addWidget(button, index / 4, index % 4);
            QObject::connect(button, &QPushButton::clicked, &dialog, [input, token] {
                input->insert(token); input->setFocus();
            });
        }
        auto* erase = new QPushButton(QStringLiteral("Backspace"), &dialog);
        erase->setAutoDefault(false);
        grid->addWidget(erase, 4, 2, 1, 2);
        QObject::connect(erase, &QPushButton::clicked, input, [input] { input->backspace(); input->setFocus(); });
        layout->addLayout(grid);
        auto* status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("measurementKeypadStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
        buttons->setObjectName(QStringLiteral("measurementKeypadButtons"));
        layout->addWidget(buttons);
        const auto selected_id = m_selected_id;
        const auto revision = m_document->revision();
        const auto document_id = m_document->snapshot().document_id();
        const auto load = [&] {
            if (target->count()) input->setText(fields[static_cast<std::size_t>(target->currentData().toInt())]->text());
            input->selectAll();
            input->setFocus();
        };
        QObject::connect(target, &QComboBox::currentIndexChanged, &dialog, load);
        load();
        buttons->button(QDialogButtonBox::Apply)->setEnabled(target->count() > 0);
        if (!target->count()) status->setText(QStringLiteral("Select an editable dimensioned object first."));
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        bool wall_length = false;
        QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
            if (m_selected_id != selected_id || m_document->revision() != revision ||
                m_document->snapshot().document_id() != document_id) {
                status->setText(QStringLiteral("The selection or document changed. Reopen the keypad.")); return;
            }
            const auto index = target->currentData().toInt();
            if (!fields[static_cast<std::size_t>(index)]->isEnabled()) return;
            const auto entity = selectedEntity();
            wall_length = index == 0 && entity && entity->type == "wall";
            if (wall_length) {
                try {
                    if (!(parse_quantity(input->text().toStdString(), m_metric_units ? Unit::metre : Unit::foot).metres > 0.0))
                        throw std::invalid_argument("Length must be positive.");
                } catch (const std::exception& error) { status->setText(QString::fromUtf8(error.what())); return; }
                dialog.accept(); // Continue through the existing constraint preview.
            } else {
                const bool ok = index == 0 ? editSelectedLength(input->text()) :
                                index == 1 ? editSelectedHeight(input->text()) : editSelectedThickness(input->text());
                if (ok) dialog.accept(); else status->setText(lastError());
            }
        });
        if (dialog.exec() == QDialog::Accepted && wall_length) showConstraintEditor(input->text());
    }

    void showAssistance() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setObjectName(QStringLiteral("assistanceDialog"));
        dialog.setWindowTitle(QStringLiteral("Offline assistance"));
        dialog.setModal(true);
        dialog.resize(680, 500);

        auto* layout = new QVBoxLayout(&dialog);
        auto* enabled = new QCheckBox(QStringLiteral("Enable suggestions for this session"), &dialog);
        enabled->setChecked(m_assistance_session.enabled());
        enabled->setToolTip(QStringLiteral(
            "Assistance is optional and session-scoped. Every suggestion remains unverified until accepted."));
        layout->addWidget(enabled);

        auto* controls = new QHBoxLayout();
        auto* kind = new QComboBox(&dialog);
        kind->addItem(QStringLiteral("Trace selected reference"),
                      static_cast<int>(AssistanceKind::tracing));
        kind->addItem(QStringLiteral("Extract explicit dimensions"),
                      static_cast<int>(AssistanceKind::dimension_extraction));
        kind->addItem(QStringLiteral("Place labels on named objects"),
                      static_cast<int>(AssistanceKind::label_placement));
        kind->addItem(QStringLiteral("Parse a command"),
                      static_cast<int>(AssistanceKind::natural_language));
        controls->addWidget(kind);
        auto* command = new QLineEdit(&dialog);
        command->setPlaceholderText(QStringLiteral("label Entry at 1.25, 2.5"));
        command->setVisible(false);
        controls->addWidget(command, 1);
        auto* generate = new QPushButton(QStringLiteral("Generate suggestions"), &dialog);
        controls->addWidget(generate);
        layout->addLayout(controls);

        auto* list = new QListWidget(&dialog);
        list->setObjectName(QStringLiteral("assistanceProposalList"));
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(list, 1);
        auto* status = new QLabel(&dialog);
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        status->setObjectName(QStringLiteral("assistanceStatus"));
        layout->addWidget(status);

        auto* buttons = new QHBoxLayout();
        auto* accept = new QPushButton(QStringLiteral("Accept selected"), &dialog);
        auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
        close->setDefault(true);
        buttons->addStretch(1);
        buttons->addWidget(accept);
        buttons->addWidget(close);
        layout->addLayout(buttons);

        std::vector<AssistanceProposal> proposals;
        const auto kind_name = [](AssistanceKind value) {
            switch (value) {
            case AssistanceKind::tracing: return QStringLiteral("tracing");
            case AssistanceKind::dimension_extraction: return QStringLiteral("dimensions");
            case AssistanceKind::label_placement: return QStringLiteral("labels");
            case AssistanceKind::natural_language: return QStringLiteral("command");
            }
            return QStringLiteral("unknown");
        };
        const auto repopulate = [&] {
            list->clear();
            for (std::size_t index = 0; index < proposals.size(); ++index) {
                const auto& item = proposals[index];
                auto* row = new QListWidgetItem(
                    QStringLiteral("Unverified • %1 • %2 • confidence %3%")
                        .arg(kind_name(item.kind), QString::fromStdString(item.preview.command_type))
                        .arg(item.source.confidence * 100.0, 0, 'f', 0), list);
                row->setData(Qt::UserRole, static_cast<int>(index));
                row->setToolTip(QStringLiteral("Source: %1\nAffected: %2")
                    .arg(QString::fromStdString(item.source.original_text.empty()
                                                    ? item.source.reference_id
                                                    : item.source.original_text),
                         QString::fromStdString(item.preview.affected_entity_ids.empty()
                                                     ? std::string{} : item.preview.affected_entity_ids.front())));
            }
            if (list->count() > 0) list->setCurrentRow(0);
        };
        const auto selected_reference = [&]() -> QString {
            const auto snapshot = authoringSnapshot();
            const auto selected = snapshot.entities().find(m_selected_id.toStdString());
            if (selected != snapshot.entities().end() && selected->second.type == "reference_asset") {
                return m_selected_id;
            }
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type == "reference_asset") return id_from(id);
            }
            return {};
        };
        QObject::connect(enabled, &QCheckBox::toggled, &dialog,
                         [this](bool checked) { setAssistanceEnabled(checked); });
        QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, [kind, command](int index) {
            const auto value = static_cast<AssistanceKind>(kind->itemData(index).toInt());
            command->setVisible(value == AssistanceKind::natural_language);
        });
        QObject::connect(generate, &QPushButton::clicked, &dialog, [&] {
            proposals.clear();
            const auto value = static_cast<AssistanceKind>(kind->currentData().toInt());
            if (value == AssistanceKind::natural_language) {
                proposals = parseAssistanceCommand(command->text());
            } else if (value == AssistanceKind::label_placement) {
                proposals = suggestLabelAssistance();
            } else {
                const auto reference = selected_reference();
                if (reference.isEmpty()) {
                    status->setText(QStringLiteral("Select or import a reference image first."));
                    repopulate();
                    return;
                }
                proposals = suggestReferenceAssistance(reference, value);
            }
            repopulate();
            if (proposals.empty()) {
                const auto error = lastError();
                status->setText(error.isEmpty() ? QStringLiteral("No suggestions were produced.") : error);
            } else {
                status->setText(QStringLiteral(
                    "Suggestions are provisional. Review the source and preview before accepting."));
            }
        });
        QObject::connect(accept, &QPushButton::clicked, &dialog, [&] {
            const auto row = list->currentItem();
            if (!row) {
                status->setText(QStringLiteral("Choose a suggestion first."));
                return;
            }
            const auto index = row->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(proposals.size())) {
                status->setText(QStringLiteral("The suggestion list is stale. Generate it again."));
                return;
            }
            if (acceptAssistanceProposal(proposals[static_cast<std::size_t>(index)])) {
                proposals.erase(proposals.begin() + index);
                repopulate();
                status->setText(QStringLiteral("Accepted through the normal command history."));
            } else {
                status->setText(lastError());
            }
        });
        QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
        dialog.exec();
    }

    void showCommandPalette() {
        QDialog dialog(owner);
        styleDialog(dialog);
        dialog.setWindowTitle(QStringLiteral("Command search"));
        dialog.setModal(true);
        dialog.resize(560, 420);
        auto* layout = new QVBoxLayout(&dialog);
        auto* search = new QLineEdit(&dialog);
        search->setPlaceholderText(QStringLiteral("Search commands…"));
        auto* list = new QListWidget(&dialog);
        layout->addWidget(search);
        layout->addWidget(list, 1);

        struct Command {
            QString name;
            std::function<void()> execute;
        };
        std::vector<Command> commands{
            {QStringLiteral("Customize keyboard shortcuts"), [this] { showShortcutSettings(); }},
            {QStringLiteral("Measurement keypad"), [this] { showMeasurementKeypad(); }},
            {QStringLiteral("New project"), [this] { createNewProject(); }},
            {QStringLiteral("Open project"), [this] { openFromDialog(); }},
            {QStringLiteral("Save project"), [this] { saveProject(); }},
            {QStringLiteral("Save project as…"), [this] { saveAsFromDialog(); }},
            {QStringLiteral("Undo"), [this] { undoCommand(); }},
            {QStringLiteral("Redo"), [this] { redoCommand(); }},
            {QStringLiteral("Copy selection"), [this] { copySelection(); }},
            {QStringLiteral("Cut selection"), [this] { cutSelection(); }},
            {QStringLiteral("Paste selection"), [this] { pasteSelection(); }},
            {QStringLiteral("Delete selection"), [this] { deleteSelection(); }},
            {QStringLiteral("Insert boundary vertex"), [this] { showBoundaryVertexInsertion(); }},
            {QStringLiteral("Redefine boundary"), [this] { showBoundaryRedefinition(); }},
            {QStringLiteral("Detect closed areas from walls"), [this] { showAutomaticAreaDetection(); }},
            {QStringLiteral("Add building"), [this] { showOrganizationDialog("building"); }},
            {QStringLiteral("Add floor"), [this] { showOrganizationDialog("floor"); }},
            {QStringLiteral("Add drawing layer"), [this] { showOrganizationDialog("layer"); }},
            {QStringLiteral("Rename selected property, building, floor or layer"),
             [this] { showOrganizationDialog("rename"); }},
            {QStringLiteral("Edit project details"), [this] {
                 if (m_selected_id.isEmpty() || !selectedEntity().has_value() ||
                     selectedEntity()->type != "property") {
                     setError(QStringLiteral("Select the project property in the navigator first."));
                     return;
                 }
                 m_project_details_group->setVisible(true);
                 m_project_subject_name_edit->setFocus();
             }},
            {QStringLiteral("Edit selected area attributes"), [this] {
                 if (m_selected_id.isEmpty() || !selectedEntity().has_value() ||
                     !is_closed_boundary_entity(selectedEntity()->type)) {
                     setError(QStringLiteral("Select a closed boundary first."));
                     return;
                 }
                 m_area_attributes_group->setVisible(true);
                 m_area_attributes_edit->setFocus();
            }},
            {QStringLiteral("Manage workspace profiles"), [this] { showWorkspaceProfiles(); }},
            {QStringLiteral("Named revisions and comparison"), [this] { showRevisionHistory(); }},
            {QStringLiteral("Transform selection"), [this] { showBoundaryTransformEditor(); }},
            {QStringLiteral("Measurement workspace"), [this] { setWorkspace(Workspace::measurement); }},
            {QStringLiteral("Architectural workspace"), [this] { setWorkspace(Workspace::architectural); }},
            {QStringLiteral("Add labels and symbols"), [this] { showAnnotationEditor(); }},
            {QStringLiteral("Import reference image"), [this] { showReferenceImport(); }},
            {QStringLiteral("Calibrate selected reference image"),
             [this] { showReferenceCalibration(); }},
            {QStringLiteral("Trace selected reference"), [this] { beginReferenceTrace(); }},
            {QStringLiteral("Open schedules"), [this] { showSchedules(); }},
            {QStringLiteral("Edit drawing sheet settings"), [this] { showSheetSettings(); }},
            {QStringLiteral("Edit sheet viewport settings"), [this] { showViewportSettings(); }},
            {QStringLiteral("Edit schedule placement settings"),
             [this] { showSchedulePlacementSettings(); }},
            {QStringLiteral("Edit architectural view settings"),
             [this] { showArchitecturalViewSettings(); }},
            {QStringLiteral("Design phases and remodeling alternatives"),
             [this] { showRemodelingAlternatives(); }},
            {QStringLiteral("Room and boundary relationships"),
             [this] { showRoomRelationships(); }},
            {QStringLiteral("Edit levels and floor-to-floor links"),
             [this] { showVerticalLevels(); }},
            {QStringLiteral("Create room boundary from selected geometry"),
             [this] { createRoomBoundaryFromSelection(); }},
            {QStringLiteral("Edit reusable assemblies"),
             [this] { showAssemblies(); }},
            {QStringLiteral("Offline assistance"), [this] { showAssistance(); }},
            {QStringLiteral("Select tool"), [this] { setTool(CanvasTool::select); }},
            {QStringLiteral("Draw measurement boundary"), [this] { setTool(CanvasTool::boundary); }},
            {QStringLiteral("Define area before drawing"),
             [this] { (void)beginBoundaryDrawing(BoundaryAuthoringMode::define_first, {}); }},
            {QStringLiteral("Draw straight wall"), [this] { setTool(CanvasTool::wall); }},
            {QStringLiteral("Create door opening"),
             [this] { createOpeningFromDialog(QStringLiteral("door")); }},
            {QStringLiteral("Create window opening"),
             [this] { createOpeningFromDialog(QStringLiteral("window")); }},
            {QStringLiteral("Create slab from selected boundary"),
             [this] { createSlabFromDialog(); }},
            {QStringLiteral("Create column, beam, stair or roof"),
             [this] { showBuildingObjectDialog(false); }},
            {QStringLiteral("Edit selected building object"),
             [this] { showBuildingObjectDialog(true); }},
            {QStringLiteral("Wall dimensions and constraints"), [this] { showConstraintEditor(); }},
            {QStringLiteral("Toggle grid"), [this] { toggleGrid(); }},
            {QStringLiteral("Toggle snap"), [this] { toggleSnap(); }},
            {QStringLiteral("Toggle overview map"), [this] { toggleOverviewMap(); }},
            {QStringLiteral("Fit view"), [this] { fitView(); }},
            {QStringLiteral("Light theme"), [this] { applyTheme(WorkspaceTheme::light); }},
            {QStringLiteral("Dark theme"), [this] { applyTheme(WorkspaceTheme::dark); }},
            {QStringLiteral("High contrast theme"), [this] { applyTheme(WorkspaceTheme::high_contrast); }},
            {QStringLiteral("Export draft PDF"), [this] { exportFromDialog(); }},
            {QStringLiteral("Export draft SVG"), [this] {
                const auto selected = QFileDialog::getSaveFileName(
                    owner, QStringLiteral("Export draft SVG"), {}, QStringLiteral("SVG document (*.svg)"));
                if (!selected.isEmpty()) exportDraftSvg(selected);
            }},
            {QStringLiteral("Print preview (draft)"), [this] { showPrintPreview(); }},
            {QStringLiteral("About internal checkpoint"), [this] { showAbout(); }},
        };

        const auto repopulate = [&] {
            const auto query = search->text().trimmed();
            list->clear();
            for (std::size_t index = 0; index < commands.size(); ++index) {
                if (!query.isEmpty() &&
                    !commands[index].name.contains(query, Qt::CaseInsensitive)) {
                    continue;
                }
                auto* item = new QListWidgetItem(commands[index].name, list);
                item->setData(Qt::UserRole, static_cast<int>(index));
            }
            if (list->count() > 0) {
                list->setCurrentRow(0);
            }
        };
        QObject::connect(search, &QLineEdit::textChanged, &dialog, repopulate);
        QObject::connect(search, &QLineEdit::returnPressed, &dialog, [&] {
            if (list->currentItem()) {
                const auto index = list->currentItem()->data(Qt::UserRole).toInt();
                commands[static_cast<std::size_t>(index)].execute();
                dialog.accept();
            }
        });
        QObject::connect(list, &QListWidget::itemActivated, &dialog,
                         [&](QListWidgetItem* item) {
                             const auto index = item->data(Qt::UserRole).toInt();
                             commands[static_cast<std::size_t>(index)].execute();
                             dialog.accept();
                         });
        repopulate();
        search->setFocus();
        dialog.exec();
    }

    void fitView() {
        m_measurementCanvas->fitView();
        m_architecturalCanvas->fitView();
        if (m_nativeModelView) {
            m_nativeModelView->fitAll();
        }
    }

    [[nodiscard]] bool workspaceDocumentsShareDocument() const noexcept {
        return m_document != nullptr;
    }

    [[nodiscard]] QString lastError() const { return m_last_error; }

    void closeEvent(QCloseEvent* event) {
        event->setAccepted(confirmDirtyTransition(
            QStringLiteral("Unsaved project"),
            QStringLiteral("This project has unsaved changes. Save before closing?")));
    }

private:
    using EntityValue = std::decay_t<decltype(std::declval<DocumentSnapshot>().entities().begin()->second)>;

    std::optional<EntityValue> selectedEntity() const {
        if (m_selected_id.isEmpty()) {
            return std::nullopt;
        }
        const auto snapshot = m_document->snapshot();
        const auto& entities = snapshot.entities();
        const auto found = entities.find(m_selected_id.toStdString());
        if (found == entities.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    std::optional<EntityValue> propertyEntity() const {
        const auto snapshot = m_document->snapshot();
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "property") {
                return entity;
            }
        }
        return std::nullopt;
    }

    template <typename Edit>
    bool editSelected(Edit&& edit, const char* message) {
        const auto entity = selectedEntity();
        if (!entity.has_value()) {
            setError(QStringLiteral("Select an entity before editing it."));
            return false;
        }
        auto properties = entity->properties;
        edit(properties);
        return editSelectedProperties(std::move(properties), message);
    }

    bool editSelectedProperties(json properties, const char* message) {
        const auto entity = selectedEntity();
        if (!entity.has_value()) {
            setError(QStringLiteral("Select an entity before editing it."));
            return false;
        }
        if (is_architectural_entity(entity->type)) {
            try {
                const auto source = authoringSnapshot();
                std::map<std::string, std::string> encoded;
                for (const auto& [key, value] : properties.items()) {
                    encoded.emplace(key, value.dump());
                }
                const auto transaction = ArchitecturalTransaction::create(
                    new_id("architectural-tx"), std::to_string(source.revision()),
                    {entity->id},
                    {ArchitecturalOperation{ArchitecturalAction::property_edit,
                        entity->id, {}, {}, std::move(encoded), std::nullopt}},
                    message);
                const auto preview = preview_architectural_transaction(source, transaction);
                if (entity_map_digest(source.entities()) == entity_map_digest(preview.entities())) {
                    clearError();
                    return true;
                }
                const auto command = architectural_transaction_command(source, transaction,
                                                                        source.revision());
                applyDocumentCommand(Command{command});
                clearError();
                refresh();
                return true;
            } catch (const std::exception& error) {
                setError(QStringLiteral("%1: %2").arg(QString::fromUtf8(message),
                                                        QString::fromUtf8(error.what())));
                return false;
            }
        }
        auto updated = *entity;
        updated.properties = std::move(properties);
        if (!applyEntity(std::move(updated), message)) {
            return false;
        }
        refresh();
        return true;
    }

    bool previewWall(const Entity& wall_entity,
                     const std::vector<HostedOpening>& openings,
                     const QString& context) {
        const auto baseline = read_required_segment(wall_entity.properties, "baseline");
        if (!baseline.has_value()) {
            setError(QStringLiteral("%1: the wall has no valid baseline.").arg(context));
            return false;
        }
        const auto thickness = read_finite_number(wall_entity.properties, "thickness_m");
        const auto height = read_finite_number(wall_entity.properties, "height_m");
        const auto elevation = read_finite_number(wall_entity.properties, "elevation_m");
        if (!thickness.has_value() || !height.has_value() || !elevation.has_value()) {
            setError(QStringLiteral("%1: wall thickness, height, and elevation are required.")
                         .arg(context));
            return false;
        }
        try {
            (void)make_wall(
                Wall{wall_entity.id, *baseline, *thickness, *height, *elevation, openings});
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("%1 rejected: %2")
                         .arg(context, QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool previewOpening(const Entity& opening_entity, const json& candidate_properties) {
        const auto wall_id = read_string(candidate_properties, "wall_id");
        if (!wall_id.has_value() || wall_id->empty()) {
            setError(QStringLiteral("Opening preview rejected: wall_id is required."));
            return false;
        }
        const auto snapshot = m_document->snapshot();
        const auto& entities = snapshot.entities();
        const auto wall = entities.find(*wall_id);
        if (wall == entities.end() || wall->second.type != "wall") {
            setError(QStringLiteral("Opening preview rejected: host wall does not exist."));
            return false;
        }
        std::vector<HostedOpening> openings;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type != "opening") {
                continue;
            }
            const auto existing_wall_id = read_string(entity.properties, "wall_id");
            if (!existing_wall_id.has_value() || existing_wall_id.value() != *wall_id) {
                continue;
            }
            Entity candidate = entity;
            if (id == opening_entity.id) {
                candidate.properties = candidate_properties;
            }
            QString opening_error;
            const auto opening = read_hosted_opening(candidate, &opening_error);
            if (!opening.has_value()) {
                setError(QStringLiteral("Opening preview rejected: %1").arg(opening_error));
                return false;
            }
            openings.push_back(*opening);
        }
        if (std::none_of(openings.begin(), openings.end(), [&](const HostedOpening& opening) {
                return opening.id == opening_entity.id;
            })) {
            Entity candidate = opening_entity;
            candidate.properties = candidate_properties;
            QString opening_error;
            const auto opening = read_hosted_opening(candidate, &opening_error);
            if (!opening.has_value()) {
                setError(QStringLiteral("Opening preview rejected: %1").arg(opening_error));
                return false;
            }
            openings.push_back(*opening);
        }
        return previewWall(wall->second, openings, QStringLiteral("Opening preview"));
    }

    bool previewSlab(const Entity& slab_entity, const json& properties) {
        const auto boundary = read_required_boundary(properties, "boundary");
        if (!boundary.has_value()) {
            setError(QStringLiteral("Slab preview rejected: boundary is required."));
            return false;
        }
        const auto holes = read_required_holes(properties);
        if (!holes.has_value()) {
            setError(QStringLiteral("Slab preview rejected: holes must be a segment array list."));
            return false;
        }
        const auto diagnostics = validate_boundary(*boundary);
        if (!diagnostics.empty()) {
            setError(QStringLiteral("Slab preview rejected: %1")
                         .arg(QString::fromStdString(diagnostics.front().message)));
            return false;
        }
        for (const auto& hole : *holes) {
            const auto hole_diagnostics = validate_boundary(hole);
            if (!hole_diagnostics.empty()) {
                setError(QStringLiteral("Slab preview rejected: %1")
                             .arg(QString::fromStdString(hole_diagnostics.front().message)));
                return false;
            }
        }
        const auto thickness = read_finite_number(properties, "thickness_m");
        const auto elevation = read_finite_number(properties, "elevation_m");
        if (!thickness.has_value() || !elevation.has_value()) {
            setError(QStringLiteral("Slab preview rejected: thickness_m and elevation_m are required."));
            return false;
        }
        try {
            (void)make_slab(Slab{slab_entity.id, *boundary, *holes, *thickness, *elevation});
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Slab preview rejected: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool editSelectedQuantity(const QString& expression, std::string_view key,
                              QString label, const char* message) {
        const auto entity = selectedEntity();
        const auto key_is_height = key == "height_m";
        const auto key_is_thickness = key == "thickness_m";
        const auto allowed = entity.has_value() &&
                             ((entity->type == "wall") ||
                              (entity->type == "opening" && key_is_height) ||
                              (entity->type == "slab" && key_is_thickness));
        if (!allowed) {
            setError(QStringLiteral("Select a wall, opening, or slab to edit its %1.").arg(label));
            return false;
        }
        try {
            const auto quantity = parse_quantity(
                expression.toStdString(), m_metric_units ? Unit::metre : Unit::foot);
            if (!(quantity.metres > 1e-7)) {
                setError(QStringLiteral("%1 must be greater than zero.").arg(label));
                return false;
            }
            auto properties = entity->properties;
            properties[std::string(key)] = quantity.metres;
            if (entity->type == "opening") {
                if (!previewOpening(*entity, properties)) {
                    return false;
                }
            } else if (entity->type == "slab") {
                if (!previewSlab(*entity, properties)) {
                    return false;
                }
            }
            return editSelectedProperties(std::move(properties), message);
        } catch (const std::exception& error) {
            setError(QStringLiteral("%1: %2").arg(label, QString::fromUtf8(error.what())));
            return false;
        }
    }

    void requireWorkspaceDocument() const {
        if (document_authoring_source_digest_v1(m_document->snapshot()) !=
            document_authoring_source_digest_v1(m_project_workspace->snapshot()))
            throw std::runtime_error("The recovered document changed outside its workspace. The command is blocked to preserve recovery history.");
    }

    bool projectDirty() const {
        return m_document->dirty() || (!m_recovery_ledger.empty() &&
            m_project_workspace->epoch() != m_saved_workspace_epoch);
    }

    void commitWorkspaceEdit(PreparedWorkspaceEdit& edit) {
        // Allocate the compatibility view before committing. Preserve its address
        // for both canvases and callers holding the shared Document.
        auto candidate = Document::fork(edit.preview());
        if (const auto saved = m_document->snapshot().saved_revision_optional())
            candidate.mark_saved(*saved);
        (void)m_project_workspace->commit(edit);
        *m_document = std::move(candidate);
    }

    Command augmentAuthoredCommand(const Command& command) {
        // Register only newly authored geometry. Existing unregistered objects
        // retain their legacy visibility; editing them must not change ownership.
        auto authored_command = command;
        if (auto* changes = std::get_if<ApplyEntityChanges>(&authored_command)) {
            const auto source = authoringSnapshot();
            if (const auto record = decode_phase_model(source); record &&
                std::none_of(changes->entity_changes.begin(), changes->entity_changes.end(),
                    [&](const auto& change) {
                        return change.kind == EntityChangeKind::erase &&
                               change.entity_id == record->entity_id;
                    })) {
                auto registry = source.entities().at(record->entity_id);
                auto model = record->model;
                for (const auto& change : changes->entity_changes) {
                    if (change.entity_id == record->entity_id ||
                        change.entity.id == record->entity_id) {
                        registry = change.entity;
                        model = ModelPhases::from_json(registry.properties.at("model"));
                    }
                }
                auto ids = model.entity_ids();
                auto baseline = model.baseline_ids();
                auto alternatives = model.alternatives();
                bool changed = false;
                for (const auto& change : changes->entity_changes) {
                    if (change.kind != EntityChangeKind::erase) continue;
                    const auto removed = change.entity_id;
                    if (removed == record->entity_id) continue;
                    const auto entity_before = std::find(ids.begin(), ids.end(), removed);
                    if (entity_before == ids.end()) continue;
                    ids.erase(entity_before);
                    std::erase(baseline, removed);
                    for (auto& alternative : alternatives) {
                        std::erase(alternative.demolished_ids, removed);
                        std::erase(alternative.proposed_ids, removed);
                    }
                    changed = true;
                }
                for (const auto& change : changes->entity_changes) {
                    if (change.kind != EntityChangeKind::upsert ||
                        source.entities().contains(change.entity.id) ||
                        !is_phase_model_entity(change.entity.type) ||
                        change.entity.type == "building" || change.entity.type == "floor" ||
                        std::find(ids.begin(), ids.end(), change.entity.id) != ids.end()) continue;
                    ids.push_back(change.entity.id);
                    if (model.active_alternative()) {
                        for (auto& alternative : alternatives) {
                            if (alternative.id == *model.active_alternative())
                                alternative.proposed_ids.push_back(change.entity.id);
                        }
                    } else {
                        baseline.push_back(change.entity.id);
                    }
                    changed = true;
                }
                if (changed) {
                    registry.properties["model"] = ModelPhases::create(
                        std::move(ids), std::move(baseline), std::move(alternatives),
                        model.active_alternative()).to_json();
                    std::erase_if(changes->entity_changes, [&](const auto& change) {
                        return change.entity.id == record->entity_id;
                    });
                    changes->entity_changes.push_back(EntityChange::upsert(std::move(registry)));
                }
            }
        }
        return authored_command;
    }

    void applyAuthoredCommand(const Command& command) {
        if (m_recovery_ledger.empty()) {
            m_document->apply(command);
            return;
        }
        requireWorkspaceDocument();
        auto edit = m_project_workspace->prepare(command);
        commitWorkspaceEdit(edit);
    }

    void applyDocumentCommand(const Command& command) {
        applyAuthoredCommand(augmentAuthoredCommand(command));
    }

    DocumentSnapshot authoringSnapshot() const {
        if (m_recovery_ledger.empty()) return m_document->snapshot();
        requireWorkspaceDocument();
        // The workspace's saved marker can differ after explicit Save. Build
        // sealed previews from the exact snapshot their workspace adapter uses.
        return m_project_workspace->snapshot();
    }

    void applyConstraintPreview(const ConstraintAuthoringPreview& preview) {
        if (m_recovery_ledger.empty()) {
            apply_constraint_authoring(*m_document, preview);
            return;
        }
        requireWorkspaceDocument();
        auto edit = m_project_workspace->prepare_constraint_authoring(preview);
        commitWorkspaceEdit(edit);
    }

    RecoveryLedger currentRecoveryLedger(const ProjectWorkspaceSnapshot& snapshot) const {
        auto ledger = m_recovery_ledger;
        if (ledger.empty()) return ledger;
        const auto history = capture_workspace_history_record(snapshot);
        bool has_history = false;
        bool has_active = false;
        for (auto it = ledger.begin(); it != ledger.end();) {
            if (it->record_kind == "workspace_history") {
                it->envelope = encode_workspace_history_record(snapshot.document(), history,
                    snapshot.active_boundary());
                has_history = true;
            } else if (it->record_kind == "boundary_active") {
                if (!snapshot.active_boundary()) {
                    it = ledger.erase(it);
                    continue;
                }
                it->envelope = encode_boundary_active_recovery(*snapshot.active_boundary());
                has_active = true;
            }
            ++it;
        }
        if (!has_history) ledger.push_back({make_stable_id(), "workspace_history",
            encode_workspace_history_record(snapshot.document(), history, snapshot.active_boundary())});
        if (snapshot.active_boundary() && !has_active)
            ledger.push_back({make_stable_id(), "boundary_active",
                encode_boundary_active_recovery(*snapshot.active_boundary())});
        return ledger;
    }

    bool applyEntity(Entity entity, const char* message,
                     std::optional<Revision> expected_revision = std::nullopt) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only: %1")
                         .arg(QString::fromStdString(m_document->read_only_reason())));
            return false;
        }
        try {
            applyDocumentCommand(ApplyEntityChanges{
                .expected_revision = expected_revision.value_or(m_document->revision()),
                .entity_changes = {EntityChange::upsert(std::move(entity))},
                .message = message,
            });
            clearError();
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("%1: %2").arg(QString::fromUtf8(message),
                                                    QString::fromUtf8(error.what())));
            return false;
        }
    }

    void drainSaveCompletions() {
        for (auto& completion : m_save_queue.take_completed()) {
            if (completion.kind == WorkspaceSaveQueue::CompletionKind::barrier) {
                m_completed_barrier = completion.sequence;
            } else if (m_autosave_pending && completion.sequence == m_autosave_pending->sequence) {
                auto pending = std::move(*m_autosave_pending);
                m_autosave_pending.reset();
                bool accepted = false;
                if (completion.succeeded()) {
                    const auto result = WorkspaceSaveCoordinator::accept(*completion.ticket,
                        *completion.receipt, m_project_workspace->capture(), pending.binding,
                        document_authoring_source_digest_v1(m_document->snapshot()));
                    // A stale publication still owns these bytes. Retain its hash
                    // for the next guarded write, without acknowledging newer state.
                    if (result.publication_valid) m_autosave_sha256 = completion.receipt->file_sha256;
                    accepted = result.acknowledged();
                }
                m_autosave_scheduler.complete(pending.capture, accepted,
                    WorkspaceAutosaveScheduler::Clock::now());
                if (accepted) m_autosaved_checkpoint = pending.capture.checkpoint_generation;
                if (!completion.succeeded()) {
                    m_autosave_retry_after = WorkspaceAutosaveScheduler::Clock::now() + std::chrono::seconds(2);
                    // Avoid modal UI from a timer (including during shutdown).
                    m_last_error = QStringLiteral("Recovery copy failed; current changes remain unsaved.");
                    owner->statusBar()->showMessage(m_last_error, 6000);
                }
            } else {
                m_completed_saves.emplace(completion.sequence, std::move(completion));
            }
        }
    }

    void resetAutosaveSession() {
        // Called only after a project transition has drained the queue. The
        // document ID can remain stable across reopen, so session state must
        // be reset explicitly rather than inferred from identity alone.
        m_autosave_scheduler = WorkspaceAutosaveScheduler{};
        m_autosave_document_id.clear();
        m_autosave_archive_id.clear();
        m_autosave_path.clear();
        m_autosave_sha256.clear();
        m_autosaved_checkpoint = 0;
        m_autosave_retry_after = {};
    }

    [[nodiscard]] std::filesystem::path autosaveDirectoryForCurrentProject() const {
        auto directory = m_file_path.empty()
            ? filesystem_path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)) /
                "recovery"
            : m_file_path.parent_path();
        if (directory.empty()) directory = std::filesystem::current_path();
        return directory;
    }

    void rebaseAutosaveDestination() {
        if (m_autosave_archive_id.empty()) return;
        const auto next = autosaveDirectoryForCurrentProject() /
            ("recovery-" + m_autosave_archive_id + ".bldproj");
        if (!same_filesystem_path(next, m_autosave_path)) {
            m_autosave_path = next;
            // A path change invalidates the CAS fingerprint captured for the
            // previous destination. The next capture must create the new
            // recovery file rather than compare it with the old one.
            m_autosave_sha256.clear();
        }
    }

    // Remove only the exact recovery copy previously owned by this desktop
    // session. A changed, malformed, aliased, or foreign file is retained so
    // Save As can never delete user data merely because a generated filename
    // happens to match. This is deliberately a best-effort cleanup boundary:
    // the final fingerprint check narrows the replacement window, while the
    // ordinary recovery publication remains guarded by its own CAS write.
    [[nodiscard]] bool cleanupAutosaveCopy(const std::filesystem::path& path,
                                           const std::string& archive_id,
                                           const std::string& document_id,
                                           const std::string& owner_token,
                                           const std::string& expected_hash) const {
        if (path.empty()) return true;
        std::error_code status_error;
        if (!std::filesystem::exists(path, status_error)) return !status_error;
        if (status_error || !std::filesystem::is_regular_file(
                std::filesystem::symlink_status(path, status_error)) || status_error)
            return false;
        if (archive_id.empty() || document_id.empty() || owner_token.empty() || expected_hash.empty() ||
            path.filename() != std::filesystem::path("recovery-" + archive_id + ".bldproj"))
            return false;
        try {
            const auto loaded = ProjectStore::load_archive(path, ArchiveRole::recovery_copy);
            if (!loaded.supported() || !loaded.recovery.decoded ||
                !loaded.recovery.decoded->recovery_copy)
                return false;
            const auto& record = *loaded.recovery.decoded->recovery_copy;
            if (record.archive_id != archive_id || record.document_id != document_id ||
                record.owner_token != owner_token)
                return false;
            if (loaded.file_sha256 != expected_hash) return false;
            // Check the bytes again immediately before removal. This does not
            // claim an adversarial filesystem lock, but it avoids deleting a
            // replacement that arrived between archive load and cleanup.
            if (ProjectStore::file_sha256(path) != loaded.file_sha256) return false;
            std::error_code remove_error;
            if (!std::filesystem::remove(path, remove_error) || remove_error) return false;
            return true;
        } catch (...) {
            return false;
        }
    }

    void waitForSaveBarrier() {
        const auto barrier = m_save_queue.enqueue_barrier();
        while (m_completed_barrier < barrier) {
            drainSaveCompletions();
            if (m_completed_barrier < barrier) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void pollAutosave() {
        drainSaveCompletions();
        if (!m_document->is_editable()) return;
        try {
            const bool recovery_project = !m_recovery_ledger.empty();
            if (recovery_project) {
                requireWorkspaceDocument();
            } else if (document_authoring_source_digest_v1(m_document->snapshot()) !=
                       document_authoring_source_digest_v1(m_project_workspace->snapshot())) {
                // Legacy v1-v3 documents still expose the historical mutable
                // Document API. Rebase the detached workspace before taking a
                // recovery capture so the queue never sees stale geometry.
                m_project_workspace = std::make_unique<ProjectWorkspace>(m_document->snapshot());
            }
            const auto now = WorkspaceAutosaveScheduler::Clock::now();
            const auto document_id = m_document->snapshot().document_id();
            if (m_autosave_document_id != document_id) {
                // Project transitions drain first, so the scheduler has no old job.
                m_autosave_scheduler = WorkspaceAutosaveScheduler{};
                m_autosave_document_id = document_id;
                // Legacy projects use the document revision as their
                // monotonic recovery generation. A clean open starts at its
                // saved marker; an edit that happened before the first timer
                // tick therefore remains visible to the scheduler.
                m_autosaved_checkpoint = recovery_project ? 0
                    : m_document->snapshot().saved_revision_optional().value_or(0);
                m_autosave_sha256.clear();
                m_autosave_retry_after = {};
                m_autosave_archive_id = make_stable_id();
                m_autosave_path = autosaveDirectoryForCurrentProject() /
                    ("recovery-" + m_autosave_archive_id + ".bldproj");
            }
            const auto edited_generation = recovery_project
                ? m_project_workspace->edited_generation() : m_document->revision();
            const auto checkpoint_generation = recovery_project
                ? m_project_workspace->checkpoint_generation() : m_document->revision();
            m_autosave_scheduler.observe(now, edited_generation, checkpoint_generation,
                m_autosaved_checkpoint);
            if (now < m_autosave_retry_after) return;
            const auto capture = m_autosave_scheduler.capture(now);
            if (!capture) return;
            try {
                const auto snapshot = m_project_workspace->capture();
                const auto path = m_autosave_path;
                const auto utf8 = path.generic_u8string();
                SavePublicationBinding binding{m_save_owner_token, ArchiveRole::recovery_copy,
                    m_autosave_archive_id, std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size())};
                auto ticket = WorkspaceSaveCoordinator::capture(snapshot, binding,
                    document_authoring_source_digest_v1(m_document->snapshot()));
                auto ledger = currentRecoveryLedger(snapshot);
                if (ledger.empty()) {
                    // A legacy or untitled project has no persisted recovery
                    // ledger yet, but a recovery-copy role still requires the
                    // same history anchor as a restored v4 project.
                    const auto history = capture_workspace_history_record(snapshot);
                    ledger.push_back({make_stable_id(), "workspace_history",
                        encode_workspace_history_record(snapshot.document(), history,
                            snapshot.active_boundary())});
                }
                RecoveryCopyRecord copy;
                copy.archive_id = m_autosave_archive_id;
                copy.owner_token = m_save_owner_token;
                copy.document_id = snapshot.document().document_id();
                if (!m_file_path.empty()) {
                    const auto source_utf8 = m_file_path.generic_u8string();
                    copy.source_path = std::string(reinterpret_cast<const char*>(source_utf8.data()), source_utf8.size());
                }
                if (!m_file_sha256.empty()) copy.source_sha256 = m_file_sha256;
                copy.explicitly_saved_document_revision = m_document->snapshot().saved_revision_optional();
                copy.explicit_save_generation = m_saved_edited_generation;
                copy.saved_edited_generation = m_saved_edited_generation;
                copy.workspace_epoch = snapshot.epoch();
                copy.edited_generation = snapshot.edited_generation();
                copy.checkpoint_generation = snapshot.checkpoint_generation();
                copy.autosaved_checkpoint_generation = snapshot.checkpoint_generation();
                ledger.push_back({m_autosave_archive_id, "recovery_copy", encode_recovery_copy_record(copy)});
                auto archive = ProjectArchiveSnapshot(snapshot.document(), std::move(ledger), ArchiveRole::recovery_copy);
                SaveOptions options;
                if (!m_autosave_sha256.empty()) options.expected_destination_sha256 = m_autosave_sha256;
                const auto sequence = m_save_queue.enqueue(std::move(ticket),
                    [path, archive = std::move(archive), options] {
                        std::filesystem::create_directories(path.parent_path());
                        return ProjectStore::save_archive(path, archive, options);
                    });
                m_autosave_pending = PendingAutosave{sequence, *capture, std::move(binding)};
            } catch (...) {
                m_autosave_scheduler.complete(*capture, false, now);
                throw;
            }
        } catch (const std::exception& error) {
            m_autosave_retry_after = WorkspaceAutosaveScheduler::Clock::now() + std::chrono::seconds(2);
            m_last_error = QStringLiteral("Recovery copy paused: %1").arg(QString::fromUtf8(error.what()));
        }
    }

    bool saveTo(const std::filesystem::path& path, bool current_destination) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only and cannot be saved."));
            return false;
        }
        if (!m_autosave_path.empty() && same_filesystem_path(path, m_autosave_path)) {
            setError(QStringLiteral("The selected project path is reserved for recovery data."));
            return false;
        }
        try {
            const auto previous_autosave_path = m_autosave_path;
            const auto previous_autosave_archive_id = m_autosave_archive_id;
            const auto previous_autosave_document_id = m_autosave_document_id;
            const auto snapshot = m_document->snapshot();
            const auto source_digest = document_authoring_source_digest_v1(snapshot);
            if (source_digest != document_authoring_source_digest_v1(m_project_workspace->snapshot())) {
                // The public mutable Document API remains supported for legacy projects.
                // Never discard a restored ledger to accommodate an out-of-band edit.
                if (!m_recovery_ledger.empty())
                    throw std::runtime_error("The recovered document changed outside its workspace. Saving is blocked to preserve recovery history.");
                m_project_workspace = std::make_unique<ProjectWorkspace>(snapshot);
            }
            const auto workspace_snapshot = m_project_workspace->capture();
            auto recovery_ledger = currentRecoveryLedger(workspace_snapshot);
            const auto source_document = m_document;
            const auto path_utf8 = path.generic_u8string();
            const SavePublicationBinding binding{
                m_save_owner_token, ArchiveRole::ordinary, make_stable_id(),
                std::string(reinterpret_cast<const char*>(path_utf8.data()), path_utf8.size())};
            auto ticket = WorkspaceSaveCoordinator::capture(workspace_snapshot, binding, source_digest);
            SaveOptions options;
            if (current_destination && !m_file_sha256.empty()) {
                options.expected_destination_sha256 = m_file_sha256;
            } else if (current_destination && std::filesystem::exists(path)) {
                // Leave the expected fingerprint empty so ProjectStore fails
                // closed with destination_exists rather than overwriting.
            } else if (!current_destination && path == m_file_path && !m_file_sha256.empty()) {
                options.expected_destination_sha256 = m_file_sha256;
            }
            // Recovery archives preserve an optional saved marker. Prepare a
            // detached snapshot with the current revision marked saved so a
            // successful save remains clean after a close/reopen cycle. The
            // live marker is changed only after acknowledgement succeeds.
            std::optional<Document> persisted_document;
            DocumentSnapshot persisted_snapshot = snapshot;
            if (!m_recovery_ledger.empty()) {
                persisted_document.emplace(Document::fork(snapshot));
                persisted_document->mark_saved(snapshot.revision());
                persisted_snapshot = persisted_document->snapshot();
            }
            const bool legacy = m_recovery_ledger.empty();
            const auto sequence = m_save_queue.enqueue(std::move(ticket),
                [path, persisted_snapshot, recovery_ledger, options, legacy] {
                    return legacy ? ProjectStore::save(path, persisted_snapshot, options)
                        : ProjectStore::save_archive(path,
                            ProjectArchiveSnapshot(persisted_snapshot, recovery_ledger, ArchiveRole::ordinary), options);
                });
            // Synchronous API compatibility: storage runs on the worker, while
            // this explicit barrier deliberately does not pump reentrant UI events.
            waitForSaveBarrier();
            // A queued recovery completion may have supplied the only trusted
            // fingerprint for the old copy while the barrier was draining.
            const auto previous_autosave_hash = m_autosave_sha256;
            auto completion = std::move(m_completed_saves.at(sequence));
            m_completed_saves.erase(sequence);
            if (completion.error) std::rethrow_exception(completion.error);
            const auto& receipt = *completion.receipt;
            const auto acknowledgement = WorkspaceSaveCoordinator::accept(*completion.ticket, receipt,
                m_project_workspace->capture(), binding,
                document_authoring_source_digest_v1(m_document->snapshot()));
            if (!acknowledgement.acknowledged() || source_document != m_document ||
                document_snapshot_digest(snapshot) != document_snapshot_digest(m_document->snapshot())) {
                setError(QStringLiteral("The file was saved, but the current workspace changed and was not marked saved."));
                return false;
            }
            const bool refresh_draft_source = m_boundary_source && m_boundary_document == m_document &&
                document_snapshot_digest(*m_boundary_source) == document_snapshot_digest(snapshot);
            m_document->mark_saved(receipt.revision);
            m_recovery_ledger = std::move(recovery_ledger);
            m_saved_workspace_epoch = m_project_workspace->epoch();
            m_saved_edited_generation = m_project_workspace->edited_generation();
            // An explicit publication is also a durable recovery checkpoint.
            // Advance the local autosave watermark only after the queue has
            // acknowledged this exact destination and workspace state.
            m_autosaved_checkpoint = m_recovery_ledger.empty()
                ? m_document->revision() : m_project_workspace->checkpoint_generation();
            if (refresh_draft_source) m_boundary_source = m_document->snapshot();
            m_file_path = path;
            m_file_sha256 = receipt.file_sha256;
            const bool same_destination = !previous_autosave_path.empty() &&
                same_filesystem_path(previous_autosave_path, path);
            const bool cleanup_ok = previous_autosave_path.empty() || same_destination ||
                cleanupAutosaveCopy(previous_autosave_path, previous_autosave_archive_id,
                    previous_autosave_document_id, m_save_owner_token, previous_autosave_hash);
            if (!same_destination && cleanup_ok) m_autosave_sha256.clear();
            rebaseAutosaveDestination();
            clearError();
            refresh();
            auto status = hasBoundaryDraftChanges()
                ? QStringLiteral("Saved committed geometry. The unfinished boundary is still unsaved.")
                : QStringLiteral("Saved revision %1.").arg(receipt.revision);
            if (!cleanup_ok) {
                status += QStringLiteral(" Recovery copy retained; cleanup pending.");
            }
            owner->statusBar()->showMessage(status, 6000);
            return true;
        } catch (const std::exception& error) {
            setError(QStringLiteral("Save failed; the document remains open: %1")
                         .arg(QString::fromUtf8(error.what())));
            return false;
        }
    }

    bool confirmDirtyTransition(const QString& title, const QString& message) {
        waitForSaveBarrier();
        if (!confirmDiscardBoundaryDraft()) return false;
        // A new window contains only the generated scaffold. It has no user
        // edits, even though Document quite correctly reports an absent saved
        // marker as dirty. Let startup recovery replace that pristine shell
        // without presenting a misleading Save/Discard prompt.
        if (m_file_path.empty() && m_recovery_ledger.empty() && m_document->revision() == 1 &&
            !hasBoundaryDraftChanges()) {
            return true;
        }
        if (!projectDirty() || !m_document->is_editable()) {
            return true;
        }
        const auto context = captureModalContext();
        const auto state = m_boundary_session ? std::optional{m_boundary_session->view()} : std::nullopt;
        const auto unchanged = [&] {
            if (!modalContextUnchanged(context)) return false;
            if (state.has_value() != m_boundary_session.has_value() ||
                (state && !(m_boundary_session->view() == *state))) {
                setError(QStringLiteral("The unfinished boundary changed while the message was open. Review it before leaving the project."));
                return false;
            }
            return true;
        };
        const auto answer = QMessageBox::warning(
            owner, title, message,
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            return false;
        }
        if (!unchanged()) return false;
        if (answer == QMessageBox::Save && !saveProject()) return false;
        if (!unchanged()) return false;
        // The modal prompt runs an event loop and can submit another autosave.
        // Finish that publication before a successful close/project transition.
        waitForSaveBarrier();
        return true;
    }

    void buildUi() {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        owner->setObjectName(QStringLiteral("propertyStudioMainWindow"));
        owner->resize(1480, 900);
        owner->setMinimumSize(1080, 700);

        auto* toolbar = owner->addToolBar(QStringLiteral("Workspace"));
        toolbar->setObjectName(QStringLiteral("primaryToolbar"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        // Keep the command strip a single compact hit row. Labels remain on
        // the actions for menus, keyboard navigation, and screen readers;
        // the primary row presents the bundled glyphs and exposes the label
        // through the tooltip/status tip instead of spending vertical space
        // on clipped text beside every icon.
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setIconSize(QSize(16, 16));
        toolbar->setContentsMargins(0, 0, 0, 0);
        if (auto* toolbar_layout = toolbar->layout()) {
            toolbar_layout->setContentsMargins(0, 0, 0, 0);
            toolbar_layout->setSpacing(1);
        }
        // One compact row with readable icons and usable mouse targets.
        toolbar->setFixedHeight(32);
        const auto add_toolbar_action = [this, toolbar](const QString& label, const char* icon_paths) {
            auto* action = toolbar->addAction(modern_toolbar_icon(icon_paths), label);
            action->setToolTip(label);
            action->setStatusTip(label);
            return action;
        };
        m_new_action = add_toolbar_action(QStringLiteral("New"), "<path d='M6 3h9l3 3v15H6z'/><path d='M15 3v5h5'/><path d='M9 13h6M12 10v6'/>");
        m_open_action = add_toolbar_action(QStringLiteral("Open"), "<path d='M3 7h7l2 2h9v11H3z'/><path d='M3 7V5h7l2 2'/>");
        m_recover_action = add_toolbar_action(QStringLiteral("Recover"), "<path d='M5 8a8 8 0 1 1 0 8'/><path d='M5 4v4h4'/>");
        m_save_action = add_toolbar_action(QStringLiteral("Save"), "<path d='M5 3h12l3 3v15H5z'/><path d='M8 3v6h9V3M8 21v-7h9v7'/>");
        m_save_as_action = add_toolbar_action(QStringLiteral("Save as"), "<path d='M5 3h12l3 3v15H5z'/><path d='M8 3v6h9V3M8 21v-7h9v7'/><path d='M15 12h5M17.5 9.5v5'/>");
        toolbar->addSeparator();
        m_undo_action = add_toolbar_action(QStringLiteral("Undo"), "<path d='M9 7 4 12l5 5'/><path d='M4 12h9a7 7 0 0 1 7 7'/>");
        m_redo_action = add_toolbar_action(QStringLiteral("Redo"), "<path d='m15 7 5 5-5 5'/><path d='M20 12h-9a7 7 0 0 0-7 7'/>");
        toolbar->addSeparator();
        m_measurement_action = toolbar->addAction(
            modern_toolbar_icon("<path d='M4 5h16v14H4z'/><path d='M8 9h8M8 13h5'/><path d='M17 17l3 3'/><path d='m17 17 2-2'/>"),
            QStringLiteral("Measurement"));
        m_measurement_action->setToolTip(QStringLiteral("Measurement workspace (Ctrl+1)"));
        m_measurement_action->setStatusTip(QStringLiteral("Measurement workspace (Ctrl+1)"));
        m_architectural_action = toolbar->addAction(
            modern_toolbar_icon("<path d='M4 20V9l8-5 8 5v11'/><path d='M8 20v-6h8v6'/><path d='M10 10h4'/>"),
            QStringLiteral("Architectural"));
        m_architectural_action->setToolTip(QStringLiteral("Architectural workspace (Ctrl+2)"));
        m_architectural_action->setStatusTip(QStringLiteral("Architectural workspace (Ctrl+2)"));
        toolbar->addSeparator();
        m_palette_action = add_toolbar_action(QStringLiteral("Commands"), "<path d='M5 5h5v5H5zM14 5h5v5h-5zM5 14h5v5H5zM14 14h5v5h-5z'/>");
        auto* shortcut_settings = add_toolbar_action(QStringLiteral("Shortcuts"), "<rect x='3' y='6' width='18' height='12' rx='2'/><path d='M7 10h2M11 10h2M15 10h2M7 14h10'/>");
        shortcut_settings->setObjectName(QStringLiteral("keyboardShortcutSettings"));

        // Keep the canvas-facing toolbar focused. Secondary authoring and
        // presentation commands remain one click away in an overflow menu,
        // while their QAction identities and shortcuts stay stable.
        auto* more_menu = new QMenu(owner);
        auto* survey_action = more_menu->addAction(QStringLiteral("Survey traverse…"));
        survey_action->setObjectName(QStringLiteral("surveyTraverse"));
        QObject::connect(survey_action, &QAction::triggered, owner, [this] { showSurveyCalculator(); });
        m_copy_action = new QAction(QStringLiteral("Copy selection"), owner);
        m_copy_action->setObjectName(QStringLiteral("copySelection"));
        m_copy_action->setShortcut(QKeySequence::Copy);
        m_copy_action->setShortcutContext(Qt::WindowShortcut);
        m_cut_action = new QAction(QStringLiteral("Cut selection"), owner);
        m_cut_action->setObjectName(QStringLiteral("cutSelection"));
        m_cut_action->setShortcut(QKeySequence::Cut);
        m_cut_action->setShortcutContext(Qt::WindowShortcut);
        m_paste_action = new QAction(QStringLiteral("Paste selection"), owner);
        m_paste_action->setObjectName(QStringLiteral("pasteSelection"));
        m_paste_action->setShortcut(QKeySequence::Paste);
        m_paste_action->setShortcutContext(Qt::WindowShortcut);
        m_delete_action = new QAction(QStringLiteral("Delete selection"), owner);
        m_delete_action->setObjectName(QStringLiteral("deleteSelection"));
        m_delete_action->setShortcut(QKeySequence::Delete);
        m_delete_action->setShortcutContext(Qt::WindowShortcut);
        m_insert_vertex_action = new QAction(QStringLiteral("Insert boundary vertex…"), owner);
        m_insert_vertex_action->setObjectName(QStringLiteral("insertBoundaryVertex"));
        more_menu->addAction(m_copy_action);
        more_menu->addAction(m_cut_action);
        more_menu->addAction(m_paste_action);
        more_menu->addAction(m_delete_action);
        more_menu->addAction(m_insert_vertex_action);
        more_menu->addSeparator();
        m_annotation_action = new QAction(QStringLiteral("Annotations"), owner);
        m_reference_action = new QAction(QStringLiteral("Reference image"), owner);
        m_schedule_action = new QAction(QStringLiteral("Schedules"), owner);
        m_sheet_action = new QAction(QStringLiteral("Sheet settings"), owner);
        m_viewport_action = new QAction(QStringLiteral("Viewport settings"), owner);
        m_schedule_placement_action = new QAction(QStringLiteral("Schedule placement"), owner);
        m_view_action = new QAction(QStringLiteral("Architectural view settings"), owner);
        m_remodel_action = new QAction(QStringLiteral("Design phases and alternatives…"), owner);
        m_remodel_action->setObjectName(QStringLiteral("designPhaseSettings"));
        m_relationship_action = new QAction(QStringLiteral("Room relationships…"), owner);
        m_relationship_action->setObjectName(QStringLiteral("roomRelationships"));
        m_levels_action = new QAction(QStringLiteral("Levels…"), owner);
        m_levels_action->setObjectName(QStringLiteral("verticalLevels"));
        m_assembly_action = new QAction(QStringLiteral("Assemblies…"), owner);
        m_assembly_action->setObjectName(QStringLiteral("assemblyCatalog"));
        m_assistance_action = new QAction(QStringLiteral("Offline assistance…"), owner);
        m_workspace_profiles_action = new QAction(QStringLiteral("Workspace profiles…"), owner);
        m_workspace_profiles_action->setObjectName(QStringLiteral("workspaceProfiles"));
        m_revisions_action = new QAction(QStringLiteral("Named revisions…"), owner);
        m_revisions_action->setObjectName(QStringLiteral("revisionHistory"));
        m_transform_action = new QAction(QStringLiteral("Transform selection…"), owner);
        m_transform_action->setObjectName(QStringLiteral("boundaryTransform"));
        m_redefine_action = new QAction(QStringLiteral("Redefine boundary…"), owner);
        m_redefine_action->setObjectName(QStringLiteral("boundaryRedefinition"));
        m_detect_areas_action = new QAction(QStringLiteral("Detect closed areas…"), owner);
        m_detect_areas_action->setObjectName(QStringLiteral("detectClosedAreas"));
        m_about_action = new QAction(QStringLiteral("About Property Studio"), owner);
        const std::array<QAction*, 18> secondary_actions{
            m_annotation_action, m_reference_action, m_schedule_action, m_sheet_action,
            m_viewport_action, m_schedule_placement_action, m_view_action, m_remodel_action,
            m_relationship_action, m_levels_action, m_assembly_action, m_assistance_action,
            m_workspace_profiles_action, m_revisions_action, m_transform_action, m_redefine_action,
            m_detect_areas_action,
            m_about_action};
        for (auto* action : secondary_actions) {
            owner->addAction(action);
            more_menu->addAction(action);
        }
        more_menu->addSeparator();
        auto* more_action = more_menu->addAction(QStringLiteral("Keyboard shortcuts…"));
        QObject::connect(more_action, &QAction::triggered, owner, [this] { showShortcutSettings(); });
        const auto text_editor_focused = [] {
            auto* focused = QApplication::focusWidget();
            return qobject_cast<QLineEdit*>(focused) != nullptr ||
                   qobject_cast<QPlainTextEdit*>(focused) != nullptr;
        };
        QObject::connect(m_copy_action, &QAction::triggered, owner, [this, text_editor_focused] {
            if (!text_editor_focused()) (void)copySelection();
        });
        QObject::connect(m_cut_action, &QAction::triggered, owner, [this, text_editor_focused] {
            if (!text_editor_focused()) (void)cutSelection();
        });
        QObject::connect(m_paste_action, &QAction::triggered, owner, [this, text_editor_focused] {
            if (!text_editor_focused()) (void)pasteSelection();
        });
        QObject::connect(m_delete_action, &QAction::triggered, owner, [this, text_editor_focused] {
            if (!text_editor_focused()) (void)deleteSelection();
        });
        QObject::connect(m_insert_vertex_action, &QAction::triggered, owner,
                         [this] { showBoundaryVertexInsertion(); });
        auto* more_button = new QToolButton(toolbar);
        more_button->setObjectName(QStringLiteral("moreTools"));
        more_button->setIcon(modern_toolbar_icon("<path d='M5 7h14M5 12h14M5 17h14'/>"));
        more_button->setToolTip(QStringLiteral("Annotations, references, phases, sheets, and view settings"));
        more_button->setStatusTip(QStringLiteral("Annotations, references, phases, sheets, and view settings"));
        more_button->setAccessibleName(QStringLiteral("More tools"));
        more_button->setAccessibleDescription(QStringLiteral(
            "Open secondary authoring and presentation commands"));
        more_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        more_button->setMenu(more_menu);
        more_button->setPopupMode(QToolButton::InstantPopup);
        toolbar->addWidget(more_button);
        QObject::connect(shortcut_settings, &QAction::triggered, owner, [this] { showShortcutSettings(); });
        toolbar->addSeparator();
        auto* theme_menu = new QMenu(owner);
        const auto add_theme_action = [this, theme_menu](QString label, WorkspaceTheme theme) {
            auto* action = theme_menu->addAction(std::move(label));
            QObject::connect(action, &QAction::triggered, owner, [this, theme] { applyTheme(theme); });
        };
        add_theme_action(QStringLiteral("Light"), WorkspaceTheme::light);
        add_theme_action(QStringLiteral("Dark"), WorkspaceTheme::dark);
        add_theme_action(QStringLiteral("High contrast"), WorkspaceTheme::high_contrast);
        auto* theme_button = new QToolButton(toolbar);
        theme_button->setObjectName(QStringLiteral("themeMenu"));
        theme_button->setIcon(modern_toolbar_icon(
            "<circle cx='12' cy='12' r='4'/><path d='M12 2v3M12 19v3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M2 12h3M19 12h3M4.9 19.1 7 17M17 7l2.1-2.1'/>"));
        theme_button->setToolTip(QStringLiteral("Select light, dark, or high-contrast workspace theme"));
        theme_button->setStatusTip(QStringLiteral("Select light, dark, or high-contrast workspace theme"));
        theme_button->setAccessibleName(QStringLiteral("Theme"));
        theme_button->setAccessibleDescription(QStringLiteral("Select the workspace color theme"));
        theme_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        theme_button->setMenu(theme_menu);
        theme_button->setPopupMode(QToolButton::InstantPopup);
        toolbar->addWidget(theme_button);
        m_unitsCombo = new QComboBox(toolbar);
        m_unitsCombo->setObjectName(QStringLiteral("unitSystem"));
        m_unitsCombo->addItems({QStringLiteral("Imperial"), QStringLiteral("Metric")});
        m_unitsCombo->setToolTip(QStringLiteral("Units for dimensions and calculations"));
        toolbar->addWidget(m_unitsCombo);
        m_pageSizeCombo = new QComboBox(toolbar);
        m_pageSizeCombo->setObjectName(QStringLiteral("outputPageSize"));
        const std::vector<std::pair<QString, QPageSize::PageSizeId>> page_sizes{
            {QStringLiteral("Letter"), QPageSize::Letter},
            {QStringLiteral("Legal"), QPageSize::Legal},
            {QStringLiteral("Tabloid"), QPageSize::Tabloid},
            {QStringLiteral("A4"), QPageSize::A4},
            {QStringLiteral("A3"), QPageSize::A3},
        };
        for (const auto& [label, page_size] : page_sizes) {
            m_pageSizeCombo->addItem(label, static_cast<int>(page_size));
        }
        m_pageSizeCombo->setCurrentIndex(m_pageSizeCombo->findData(static_cast<int>(QPageSize::A4)));
        m_pageSizeCombo->setToolTip(QStringLiteral("Paper size for PDF, print preview, and sheets"));
        toolbar->addWidget(m_pageSizeCombo);
        m_architecturalViewCombo = new QComboBox(toolbar);
        m_architecturalViewCombo->setObjectName(QStringLiteral("architecturalView"));
        m_architecturalViewCombo->addItem(QStringLiteral("Plan"), static_cast<int>(BuildingViewKind::plan));
        m_architecturalViewCombo->addItem(QStringLiteral("Elevation"), static_cast<int>(BuildingViewKind::elevation));
        m_architecturalViewCombo->addItem(QStringLiteral("Section · 1.2 m"), static_cast<int>(BuildingViewKind::section));
        m_architecturalViewCombo->setToolTip(QStringLiteral(
            "Select the derived architectural plan, elevation, or horizontal section view"));
        toolbar->addWidget(m_architecturalViewCombo);

        auto* workspace_group = new QActionGroup(owner);
        workspace_group->setExclusive(true);
        m_measurement_action->setCheckable(true);
        m_architectural_action->setCheckable(true);
        workspace_group->addAction(m_measurement_action);
        workspace_group->addAction(m_architectural_action);
        m_measurement_action->setChecked(true);

        QObject::connect(m_new_action, &QAction::triggered, owner, [this] { createNewProject(); });
        QObject::connect(m_open_action, &QAction::triggered, owner, [this] { openFromDialog(); });
        QObject::connect(m_recover_action, &QAction::triggered, owner,
                         [this] { recoverFromDialog(); });
        QObject::connect(m_save_action, &QAction::triggered, owner, [this] { saveProject(); });
        QObject::connect(m_save_as_action, &QAction::triggered, owner,
                         [this] { saveAsFromDialog(); });
        QObject::connect(m_undo_action, &QAction::triggered, owner, [this] { undoCommand(); });
        QObject::connect(m_redo_action, &QAction::triggered, owner, [this] { redoCommand(); });
        QObject::connect(m_measurement_action, &QAction::triggered, owner,
                         [this] { setWorkspace(Workspace::measurement); });
        QObject::connect(m_architectural_action, &QAction::triggered, owner,
                         [this] { setWorkspace(Workspace::architectural); });
        QObject::connect(m_palette_action, &QAction::triggered, owner,
                         [this] { showCommandPalette(); });
        QObject::connect(m_annotation_action, &QAction::triggered, owner,
                         [this] { showAnnotationEditor(); });
        QObject::connect(m_reference_action, &QAction::triggered, owner,
                         [this] { showReferenceImport(); });
        QObject::connect(m_schedule_action, &QAction::triggered, owner,
                         [this] { showSchedules(); });
        QObject::connect(m_sheet_action, &QAction::triggered, owner,
                         [this] { showSheetSettings(); });
        QObject::connect(m_viewport_action, &QAction::triggered, owner,
                         [this] { showViewportSettings(); });
        QObject::connect(m_schedule_placement_action, &QAction::triggered, owner,
                         [this] { showSchedulePlacementSettings(); });
        QObject::connect(m_view_action, &QAction::triggered, owner,
                         [this] { showArchitecturalViewSettings(); });
        QObject::connect(m_remodel_action, &QAction::triggered, owner,
                         [this] { showRemodelingAlternatives(); });
        QObject::connect(m_relationship_action, &QAction::triggered, owner,
                         [this] { showRoomRelationships(); });
        QObject::connect(m_levels_action, &QAction::triggered, owner,
                         [this] { showVerticalLevels(); });
        QObject::connect(m_assembly_action, &QAction::triggered, owner,
                         [this] { showAssemblies(); });
        QObject::connect(m_assistance_action, &QAction::triggered, owner,
                         [this] { showAssistance(); });
        QObject::connect(m_workspace_profiles_action, &QAction::triggered, owner,
                         [this] { showWorkspaceProfiles(); });
        QObject::connect(m_revisions_action, &QAction::triggered, owner,
                         [this] { showRevisionHistory(); });
        QObject::connect(m_transform_action, &QAction::triggered, owner,
                         [this] { showBoundaryTransformEditor(); });
        QObject::connect(m_redefine_action, &QAction::triggered, owner,
                         [this] { showBoundaryRedefinition(); });
        QObject::connect(m_detect_areas_action, &QAction::triggered, owner,
                         [this] { showAutomaticAreaDetection(); });
        QObject::connect(m_about_action, &QAction::triggered, owner, [this] { showAbout(); });
        QObject::connect(m_unitsCombo, &QComboBox::currentIndexChanged, owner,
                         [this](int index) { setMetricUnits(index == 1); });
        QObject::connect(m_architecturalViewCombo, &QComboBox::currentIndexChanged, owner,
                         [this](int index) {
                             const auto value = m_architecturalViewCombo->itemData(index).toInt();
                             switch (static_cast<BuildingViewKind>(value)) {
                             case BuildingViewKind::plan:
                             case BuildingViewKind::elevation:
                             case BuildingViewKind::section:
                                 m_architectural_view_kind = static_cast<BuildingViewKind>(value);
                                 refreshCanvases();
                                 if (m_workspace == Workspace::architectural) m_architecturalCanvas->fitView();
                                 break;
                             }
                         });
        initializeShortcuts();
        applyTheme(WorkspaceTheme::light);

        auto* central = new QWidget(owner);
        central->setObjectName(QStringLiteral("workspaceRoot"));
        auto* root_layout = new QVBoxLayout(central);
        root_layout->setContentsMargins(0, 0, 0, 0);
        root_layout->setSpacing(0);

        m_plan_error_banner = new QLabel(central);
        m_plan_error_banner->setObjectName(QStringLiteral("planGeometryError"));
        m_plan_error_banner->setWordWrap(true);
        m_plan_error_banner->setStyleSheet(QStringLiteral(
            "color:#8b1a1a; background:#fff0ed; border:1px solid #d38d86; padding:6px 10px;"));
        root_layout->addWidget(m_plan_error_banner);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setObjectName(QStringLiteral("workspaceSplitter"));
        m_workspace_splitter = splitter;
        // Keep the canvas close to the compact command strip while retaining
        // a small breathing room around the side panels.
        splitter->setContentsMargins(16, 4, 16, 14);
        splitter->setChildrenCollapsible(true);
        root_layout->addWidget(splitter, 1);

        auto* navigator_panel = new QWidget(splitter);
        navigator_panel->setObjectName(QStringLiteral("navigatorPanel"));
        navigator_panel->setMinimumWidth(220);
        navigator_panel->setMaximumWidth(300);
        auto* navigator_layout = new QVBoxLayout(navigator_panel);
        navigator_layout->setContentsMargins(14, 15, 14, 14);
        navigator_layout->setSpacing(10);
        auto* drawing_layer_heading = new QLabel(QStringLiteral("DRAWING LAYER"), navigator_panel);
        drawing_layer_heading->setObjectName(QStringLiteral("panelHeading"));
        navigator_layout->addWidget(drawing_layer_heading);
        m_drawing_layer_combo = new QComboBox(navigator_panel);
        m_drawing_layer_combo->setObjectName(QStringLiteral("drawingLayer"));
        m_drawing_layer_combo->setMinimumWidth(0);
        m_drawing_layer_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_drawing_layer_combo->setToolTip(QStringLiteral(
            "Choose the destination for new geometry. View filters affect only plan and 3D "
            "visibility; drawing remains allowed on hidden layers. Floor association does not "
            "offset an object's world elevation."));
        navigator_layout->addWidget(m_drawing_layer_combo);
        m_drawing_context_label = new QLabel(navigator_panel);
        m_drawing_context_label->setObjectName(QStringLiteral("drawingContext"));
        m_drawing_context_label->setWordWrap(true);
        m_drawing_context_label->setTextFormat(Qt::PlainText);
        navigator_layout->addWidget(m_drawing_context_label);
        auto* phase_heading = new QLabel(QStringLiteral("DESIGN PHASE"), navigator_panel);
        phase_heading->setObjectName(QStringLiteral("phaseHeading"));
        phase_heading->setStyleSheet(QStringLiteral("font-weight:600;"));
        navigator_layout->addWidget(phase_heading);
        m_model_phase_combo = new QComboBox(navigator_panel);
        m_model_phase_combo->setObjectName(QStringLiteral("modelPhase"));
        m_model_phase_combo->setMinimumWidth(0);
        m_model_phase_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_model_phase_combo->setToolTip(QStringLiteral(
            "Choose the existing model or a remodeling alternative. The selection is saved in the project and filters all shared views."));
        navigator_layout->addWidget(m_model_phase_combo);
        auto* visibility_header = new QHBoxLayout();
        auto* visibility_heading = new QLabel(QStringLiteral("VISIBILITY"), navigator_panel);
        visibility_heading->setObjectName(QStringLiteral("panelHeading"));
        visibility_heading->setStyleSheet(QStringLiteral("font-weight:600;"));
        visibility_header->addWidget(visibility_heading);
        visibility_header->addStretch();
        m_show_all_button = new QPushButton(QStringLiteral("Show all"), navigator_panel);
        m_show_all_button->setObjectName(QStringLiteral("showAllContainers"));
        m_show_all_button->setToolTip(QStringLiteral(
            "Clear the view-only floor and drawing-layer filters."));
        m_show_all_button->setAutoDefault(false);
        visibility_header->addWidget(m_show_all_button);
        navigator_layout->addLayout(visibility_header);
        m_visibility_label = new QLabel(navigator_panel);
        m_visibility_label->setObjectName(QStringLiteral("visibilitySummary"));
        m_visibility_label->setWordWrap(true);
        m_visibility_label->setToolTip(QStringLiteral(
            "View filters hide floors and drawing layers in plan and 3D. They never change area totals."));
        navigator_layout->addWidget(m_visibility_label);
        QObject::connect(m_show_all_button, &QPushButton::clicked, owner,
                         [this] { showAllContainers(); });
        QObject::connect(m_drawing_layer_combo, &QComboBox::activated, owner, [this](int index) {
            setActiveLayer(m_drawing_layer_combo->itemData(index).toString());
        });
        QObject::connect(m_model_phase_combo, &QComboBox::activated, owner, [this](int index) {
            if (m_model_phase_combo->itemData(index).isValid()) {
                (void)selectRemodelingAlternative(m_model_phase_combo->itemData(index).toString());
            } else {
                showRemodelingAlternatives();
            }
        });
        m_navigator = new VisibilityTreeWidget(navigator_panel);
        navigator_layout->addWidget(m_navigator, 1);
        m_navigator->setObjectName(QStringLiteral("projectNavigator"));
        m_navigator->setHeaderHidden(true);
        m_navigator->setIndentation(14);
        m_navigator->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_navigator->setMinimumWidth(0);
        m_navigator->setMaximumWidth(280);
        QObject::connect(m_navigator, &QTreeWidget::currentItemChanged, owner,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (m_refreshing || !item) return;
                // A pointer press on the check indicator changes the row's
                // current item as part of the same event. Preserve the
                // selected semantic entity in that case; keyboard navigation
                // still selects floor/layer rows normally.
                if (m_navigator->checkboxInteraction()) {
                    return;
                }
                const auto id = item->data(0, Qt::UserRole).toString();
                const auto document = m_document;
                // Defer rebuilding the tree until Qt finishes its keyboard or
                // selection event; never retain a pointer to the old item.
                QTimer::singleShot(0, owner, [this, id, document] {
                    if (m_document == document && !id.isEmpty() && id != m_selected_id &&
                        has_entity(*m_document, id)) selectEntity(id);
                });
            });
        QObject::connect(m_navigator, &QTreeWidget::itemClicked, owner,
                         [this](QTreeWidgetItem* item, int column) {
                             onNavigatorClicked(item, column);
                         });
        QObject::connect(m_navigator, &QTreeWidget::itemChanged, owner,
                         [this](QTreeWidgetItem* item, int column) {
                             if (m_refreshing || column != 0 ||
                                 !is_visibility_container_item(item)) {
                                 return;
                             }
                             const auto id = item->data(0, Qt::UserRole).toString();
                             if (id.isEmpty()) return;
                             const auto document = m_document;
                             const auto visible = item->checkState(0) == Qt::Checked;
                             // QTreeWidget is in the middle of processing the
                             // keyboard/mouse event. Rebuild only after that
                             // event, and never apply it to a replacement
                             // document opened before the queued callback.
                             QTimer::singleShot(0, owner, [this, id, visible, document] {
                                 if (m_document != document || !has_entity(*m_document, id)) {
                                     return;
                                 }
                                 (void)setContainerVisible(id, visible);
                             });
                         });

        auto* tool_panel = new QWidget(splitter);
        tool_panel->setObjectName(QStringLiteral("toolPanel"));
        tool_panel->setFixedWidth(104);
        auto* tool_layout = new QVBoxLayout(tool_panel);
        tool_layout->setContentsMargins(7, 14, 7, 14);
        tool_layout->setSpacing(7);
        m_select_button = addToolButton(tool_layout, QStringLiteral("Select"), CanvasTool::select, true);
        m_boundary_button = addToolButton(tool_layout, QStringLiteral("Boundary"), CanvasTool::boundary);
        m_boundary_button->setText(QStringLiteral("Draw first"));
        m_boundary_button->setObjectName(QStringLiteral("drawFirstBoundary"));
        m_boundary_button->setToolTip(QStringLiteral("Draw measured linework, then choose its area classification"));
        m_define_boundary_button = new QToolButton(tool_panel);
        m_define_boundary_button->setText(QStringLiteral("Define first"));
        m_define_boundary_button->setIcon(modern_toolbar_icon(
            "<path d='M5 6h14M5 12h14M5 18h14'/><path d='M8 4v16M16 4v16'/>"));
        m_define_boundary_button->setIconSize(QSize(20, 20));
        m_define_boundary_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_define_boundary_button->setObjectName(QStringLiteral("defineFirstBoundary"));
        m_define_boundary_button->setToolTip(QStringLiteral("Choose an area classification before drawing and place each dimension"));
        m_define_boundary_button->setCheckable(true);
        m_define_boundary_button->setAutoRaise(true);
        m_define_boundary_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_define_boundary_button);
        QObject::connect(m_define_boundary_button, &QToolButton::clicked, owner,
                         [this] {
                             if (!beginBoundaryDrawing(BoundaryAuthoringMode::define_first, {})) syncToolControls();
                         });
        m_wall_button = addToolButton(tool_layout, QStringLiteral("Wall"), CanvasTool::wall);
        m_object_button = new QToolButton(tool_panel);
        m_object_button->setText(QStringLiteral("Object…"));
        m_object_button->setIcon(modern_toolbar_icon(
            "<path d='M4 10 12 4l8 6v10H4z'/><path d='M9 20v-6h6v6'/>"));
        m_object_button->setIconSize(QSize(20, 20));
        m_object_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_object_button->setToolTip(QStringLiteral("Create a column, beam, stair or roof"));
        m_object_button->setObjectName(QStringLiteral("createBuildingObject"));
        m_object_button->setAutoRaise(true);
        m_object_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_object_button);
        QObject::connect(m_object_button, &QToolButton::clicked, owner,
                         [this] { showBuildingObjectDialog(false); });
        tool_layout->addStretch();
        m_grid_button = new QToolButton(tool_panel);
        m_grid_button->setText(QStringLiteral("Grid"));
        m_grid_button->setIcon(modern_toolbar_icon(
            "<path d='M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z'/>"));
        m_grid_button->setIconSize(QSize(20, 20));
        m_grid_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_grid_button->setToolTip(QStringLiteral("Toggle measurement grid"));
        m_grid_button->setObjectName(QStringLiteral("gridTool"));
        m_grid_button->setCheckable(true);
        m_grid_button->setChecked(true);
        m_grid_button->setAutoRaise(true);
        m_grid_button->setMinimumWidth(0);
        m_grid_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_grid_button);
        m_snap_button = new QToolButton(tool_panel);
        m_snap_button->setText(QStringLiteral("Snap"));
        m_snap_button->setIcon(modern_toolbar_icon(
            "<path d='M6 5v6a6 6 0 0 0 12 0V5'/><path d='M6 5h4M14 5h4'/>"));
        m_snap_button->setIconSize(QSize(20, 20));
        m_snap_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_snap_button->setToolTip(QStringLiteral("Snap points to a 0.25 m grid"));
        m_snap_button->setObjectName(QStringLiteral("snapTool"));
        m_snap_button->setCheckable(true);
        m_snap_button->setChecked(true);
        m_snap_button->setAutoRaise(true);
        m_snap_button->setMinimumWidth(0);
        m_snap_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_snap_button);
        m_fit_button = new QToolButton(tool_panel);
        m_fit_button->setText(QStringLiteral("Fit"));
        m_fit_button->setIcon(modern_toolbar_icon(
            "<path d='M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5'/>"));
        m_fit_button->setIconSize(QSize(20, 20));
        m_fit_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_fit_button->setToolTip(QStringLiteral("Fit geometry in both workspace views"));
        m_fit_button->setAutoRaise(true);
        m_fit_button->setMinimumWidth(0);
        m_fit_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_fit_button);
        m_overview_button = new QToolButton(tool_panel);
        m_overview_button->setText(QStringLiteral("Map"));
        m_overview_button->setIcon(modern_toolbar_icon(
            "<rect x='4' y='5' width='16' height='14' rx='2'/><path d='m6 16 4-4 3 3 2-2 3 3'/><circle cx='9' cy='9' r='1'/>"));
        m_overview_button->setIconSize(QSize(20, 20));
        m_overview_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_overview_button->setToolTip(QStringLiteral("Show or hide the overview map"));
        m_overview_button->setObjectName(QStringLiteral("overviewMapTool"));
        m_overview_button->setCheckable(true);
        m_overview_button->setChecked(true);
        m_overview_button->setAutoRaise(true);
        m_overview_button->setMinimumWidth(0);
        m_overview_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_overview_button);
        QObject::connect(m_grid_button, &QToolButton::toggled, owner,
                         [this](bool enabled) { setGrid(enabled); });
        QObject::connect(m_snap_button, &QToolButton::toggled, owner,
                         [this](bool enabled) { setSnap(enabled); });
        QObject::connect(m_fit_button, &QToolButton::clicked, owner, [this] { fitView(); });
        QObject::connect(m_overview_button, &QToolButton::toggled, owner,
                         [this](bool enabled) { setOverviewMap(enabled); });

        m_workspaceTabs = new QTabWidget(splitter);
        m_workspaceTabs->setObjectName(QStringLiteral("workspaceTabs"));
        m_workspaceTabs->setDocumentMode(true);
        m_workspaceTabs->setTabsClosable(false);
        // Workspace switching is promoted to the primary toolbar. Keep the
        // tab container for the shared view lifecycle, but remove the
        // duplicate tab strip so the canvas begins at the content edge.
        m_workspaceTabs->tabBar()->setVisible(false);
        m_measurementCanvas = new PlanCanvas(m_workspaceTabs);
        m_measurementCanvas->setObjectName(QStringLiteral("measurementPlanCanvas"));
        auto* architectural_body = new QWidget(m_workspaceTabs);
        auto* architectural_layout = new QHBoxLayout(architectural_body);
        architectural_layout->setContentsMargins(0, 0, 0, 0);
        auto* architectural_splitter = new QSplitter(Qt::Horizontal, architectural_body);
        architectural_splitter->setObjectName(QStringLiteral("architecturalSplitter"));
        architectural_splitter->setChildrenCollapsible(true);
        m_architectural_splitter = architectural_splitter;
        m_architecturalCanvas = new PlanCanvas(architectural_splitter);
        m_architecturalCanvas->setObjectName(QStringLiteral("architecturalPlanCanvas"));
        architectural_splitter->addWidget(m_architecturalCanvas);
        const auto platform = QGuiApplication::platformName();
        const auto native_platform = platform != QStringLiteral("offscreen") &&
                                     platform != QStringLiteral("minimal");
        if (native_platform) {
            m_nativeModelView = new visualization::NativeModelView(architectural_splitter);
            m_nativeModelView->setEntitySelectedCallback(
                [this](QString id) { selectEntity(id); });
            m_nativeModelView->setErrorCallback([this](QString error) {
                setError(QStringLiteral("3D view: %1").arg(error));
            });
            architectural_splitter->addWidget(m_nativeModelView);
            architectural_splitter->setStretchFactor(0, 1);
            architectural_splitter->setStretchFactor(1, 1);
        } else {
            auto* unavailable = new QLabel(
                QStringLiteral("Native OCCT 3D is disabled on the offscreen smoke platform."),
                architectural_splitter);
            unavailable->setObjectName(QStringLiteral("modelViewUnavailable"));
            unavailable->setAlignment(Qt::AlignCenter);
            unavailable->setWordWrap(true);
            architectural_splitter->addWidget(unavailable);
        }
        architectural_layout->addWidget(architectural_splitter);
        m_workspaceTabs->addTab(m_measurementCanvas, QStringLiteral("Measurement"));
        m_workspaceTabs->addTab(architectural_body, QStringLiteral("Architectural"));
        QObject::connect(m_workspaceTabs, &QTabWidget::currentChanged, owner,
                         [this](int index) {
                             m_workspace = index == 0 ? Workspace::measurement
                                                       : Workspace::architectural;
                             refreshTitle();
                         });
        connectCanvas(m_measurementCanvas);
        connectCanvas(m_architecturalCanvas);

        m_inspector = new QScrollArea(splitter);
        m_inspector->setWidgetResizable(true);
        m_inspector->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_inspector->setMinimumWidth(320);
        m_inspector->setMaximumWidth(390);
        auto* inspector_body = new QWidget(m_inspector);
        inspector_body->setObjectName(QStringLiteral("inspectorBody"));
        inspector_body->setMinimumWidth(0);
        inspector_body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        auto* inspector_layout = new QVBoxLayout(inspector_body);
        inspector_layout->setContentsMargins(16, 15, 16, 16);
        inspector_layout->setSpacing(12);
        auto* heading = new QLabel(QStringLiteral("Inspector"), inspector_body);
        heading->setObjectName(QStringLiteral("inspectorHeading"));
        heading->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
        inspector_layout->addWidget(heading);
        m_inspector_context = new QLabel(inspector_body);
        m_inspector_context->setWordWrap(true);
        inspector_layout->addWidget(m_inspector_context);
        m_material_group = new QWidget(inspector_body);
        auto* material_layout = new QVBoxLayout(m_material_group);
        material_layout->setContentsMargins(0, 0, 0, 0);
        auto* material_row = new QHBoxLayout;
        material_row->addWidget(new QLabel("Material", m_material_group));
        m_material_combo = new QComboBox(m_material_group);
        m_material_combo->setObjectName("materialAssignment");
        m_material_combo->setAccessibleName("Assigned material");
        m_material_combo->setMinimumWidth(0);
        m_material_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_material_combo->setToolTip("Materials are managed in the Assembly catalog.");
        material_row->addWidget(m_material_combo, 1);
        auto* apply_material = new QPushButton("Apply", m_material_group);
        apply_material->setObjectName("assignMaterial");
        material_row->addWidget(apply_material);
        material_layout->addLayout(material_row);
        m_material_error = new QLabel(m_material_group);
        m_material_error->setObjectName("materialAssignmentError");
        m_material_error->setWordWrap(true);
        material_layout->addWidget(m_material_error);
        inspector_layout->addWidget(m_material_group);
        m_door_swing_button = new QPushButton("Door swing…", inspector_body);
        m_door_swing_button->setObjectName("editDoorSwing");
        inspector_layout->addWidget(m_door_swing_button);
        QObject::connect(m_door_swing_button, &QPushButton::clicked, owner, [this] { showDoorSwingEditor(); });
        QObject::connect(apply_material, &QPushButton::clicked, owner, [this] {
            try {
                if (!m_material_context || !modalContextUnchanged(*m_material_context))
                    throw std::invalid_argument("Material editing context changed. Reselect the object.");
                auto candidate = selectedEntity();
                if (!candidate) throw std::invalid_argument("Select an architectural object.");
                const auto prior = candidate->properties.value("material_assignment", json{});
                const auto encoded = m_material_combo->currentData().toString();
                if (encoded.isEmpty()) candidate->properties.erase("material_assignment");
                else candidate->properties["material_assignment"] = json::parse(encoded.toStdString());
                if (candidate->properties.value("material_assignment", json{}) == prior) { m_material_error->hide(); return; }
                applyDocumentCommand(ApplyEntityChanges{m_material_context->revision,
                    {EntityChange::upsert(*candidate)}, {}, "assign material"});
                clearError();
                refresh();
            } catch (const std::exception& error) {
                m_material_error->setText(QString::fromUtf8(error.what()));
                m_material_error->show();
            }
        });
        m_edit_object_button = new QPushButton(QStringLiteral("Edit object…"), inspector_body);
        m_edit_object_button->setObjectName(QStringLiteral("editBuildingObject"));
        inspector_layout->addWidget(m_edit_object_button);
        QObject::connect(m_edit_object_button, &QPushButton::clicked, owner,
                         [this] { showBuildingObjectDialog(true); });
        m_roof_properties_group = new QGroupBox(QStringLiteral("Roof dimensions"), inspector_body);
        m_roof_properties_group->setObjectName(QStringLiteral("roofProperties"));
        auto* roof_properties_form = new QFormLayout(m_roof_properties_group);
        m_roof_run_edit = new QLineEdit(m_roof_properties_group);
        m_roof_run_edit->setObjectName(QStringLiteral("roofRun"));
        m_roof_run_edit->setToolTip(QStringLiteral("Horizontal run of the selected sloped panel"));
        m_roof_run_label = new QLabel(QStringLiteral("Run"), m_roof_properties_group);
        m_roof_run_label->setBuddy(m_roof_run_edit);
        roof_properties_form->addRow(m_roof_run_label, m_roof_run_edit);
        m_roof_span_edit = new QLineEdit(m_roof_properties_group);
        m_roof_span_edit->setObjectName(QStringLiteral("roofSpan"));
        m_roof_span_edit->setToolTip(QStringLiteral("Full horizontal width across the roof"));
        roof_properties_form->addRow(QStringLiteral("Span"), m_roof_span_edit);
        m_roof_rise_edit = new QLineEdit(m_roof_properties_group);
        m_roof_rise_edit->setObjectName(QStringLiteral("roofRise"));
        m_roof_rise_edit->setToolTip(QStringLiteral("Rise; enter zero for a flat roof"));
        roof_properties_form->addRow(QStringLiteral("Rise"), m_roof_rise_edit);
        m_roof_overhang_edit = new QLineEdit(m_roof_properties_group);
        m_roof_overhang_edit->setObjectName(QStringLiteral("roofOverhang"));
        m_roof_overhang_edit->setToolTip(QStringLiteral("Horizontal extension beyond the roof footprint"));
        roof_properties_form->addRow(QStringLiteral("Overhang"), m_roof_overhang_edit);
        m_roof_thickness_edit = new QLineEdit(m_roof_properties_group);
        m_roof_thickness_edit->setObjectName(QStringLiteral("roofThickness"));
        m_roof_thickness_edit->setToolTip(QStringLiteral("Panel thickness measured normal to the roof"));
        roof_properties_form->addRow(QStringLiteral("Thickness"), m_roof_thickness_edit);
        m_roof_pitch_value = new QLabel(m_roof_properties_group);
        m_roof_pitch_value->setObjectName(QStringLiteral("roofPitch"));
        roof_properties_form->addRow(QStringLiteral("Derived pitch"), m_roof_pitch_value);
        m_roof_preview_error = new QLabel(m_roof_properties_group);
        m_roof_preview_error->setObjectName(QStringLiteral("roofPreviewError"));
        m_roof_preview_error->setWordWrap(true);
        m_roof_preview_error->hide();
        roof_properties_form->addRow(m_roof_preview_error);
        for (auto* field : {m_roof_run_edit, m_roof_span_edit, m_roof_rise_edit,
                            m_roof_overhang_edit, m_roof_thickness_edit}) {
            QObject::connect(field, &QLineEdit::textChanged, owner,
                             [this] { refreshRoofDimensionPreview(); });
        }
        m_apply_roof_properties_button = new QPushButton(QStringLiteral("Apply roof dimensions"),
                                                          m_roof_properties_group);
        m_apply_roof_properties_button->setObjectName(QStringLiteral("applyRoofProperties"));
        m_apply_roof_properties_button->setAccessibleName(QStringLiteral("Apply roof dimensions"));
        roof_properties_form->addRow(m_apply_roof_properties_button);
        auto* roof_openings = new QPushButton("Openings…", m_roof_properties_group);
        roof_openings->setObjectName("editRoofOpenings");
        roof_properties_form->addRow(roof_openings);
        QObject::connect(roof_openings, &QPushButton::clicked, owner, [this] { editRoofOpenings(); });
        m_building_properties_group = new QGroupBox(QStringLiteral("Object dimensions"), inspector_body);
        m_building_properties_group->setObjectName(QStringLiteral("buildingDimensions"));
        auto* building_form_layout = new QFormLayout(m_building_properties_group);
        for (const auto& spec : std::vector<std::pair<QString, const char*>>{
                 {QStringLiteral("Width"), "width_m"}, {QStringLiteral("Depth"), "depth_m"},
                 {QStringLiteral("Radius"), "radius_m"}, {QStringLiteral("Height"), "height_m"},
                 {QStringLiteral("TotalRise"), "total_rise_m"}, {QStringLiteral("Going"), "going_m"},
                 {QStringLiteral("RiserCount"), "riser_count"}}) {
            const auto label_text = spec.first == QStringLiteral("TotalRise") ? QStringLiteral("Total rise") :
                spec.first == QStringLiteral("RiserCount") ? QStringLiteral("Risers") : spec.first;
            auto* label = new QLabel(label_text, m_building_properties_group);
            auto* edit = new QLineEdit(m_building_properties_group);
            edit->setObjectName(QStringLiteral("contextBuilding") + spec.first);
            edit->setAccessibleName(label_text);
            label->setBuddy(edit);
            building_form_layout->addRow(label, edit);
            m_building_dimensions.push_back({spec.first, spec.second, label, edit, {}});
        }
        auto* placement_toggle = new QToolButton(m_building_properties_group);
        placement_toggle->setObjectName(QStringLiteral("buildingPlacementToggle"));
        placement_toggle->setText(QStringLiteral("Placement"));
        placement_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        placement_toggle->setArrowType(Qt::RightArrow);
        placement_toggle->setCheckable(true);
        building_form_layout->addRow(placement_toggle);
        auto* placement_body = new QWidget(m_building_properties_group);
        placement_body->setObjectName(QStringLiteral("buildingPlacement"));
        auto* placement_form = new QFormLayout(placement_body);
        placement_form->setContentsMargins(0, 0, 0, 0);
        placement_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        placement_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (const auto& suffix : {"BaseX", "BaseY", "BaseZ", "StartX", "StartY", "StartZ",
                                   "EndX", "EndY", "EndZ", "OrientationDegrees"}) {
            const auto name = QString::fromLatin1(suffix);
            const auto text = name == "OrientationDegrees" ? QStringLiteral("Orientation (degrees)")
                : name.left(name.size() - 1) + QStringLiteral(" ") + name.right(1);
            auto* label = new QLabel(text, placement_body);
            auto* edit = new QLineEdit(placement_body);
            edit->setMinimumWidth(0);
            edit->setObjectName(QStringLiteral("contextBuilding") + name);
            edit->setAccessibleName(text);
            label->setBuddy(edit);
            placement_form->addRow(label, edit);
            m_building_placement.push_back({name, nullptr, label, edit, {}});
        }
        building_form_layout->addRow(placement_body);
        placement_body->hide();
        QObject::connect(placement_toggle, &QToolButton::toggled, owner,
                         [placement_toggle, placement_body](bool expanded) {
            placement_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            placement_body->setVisible(expanded);
        });
        m_building_dimensions_error = new QLabel(m_building_properties_group);
        m_building_dimensions_error->setObjectName(QStringLiteral("buildingDimensionsError"));
        m_building_dimensions_error->setWordWrap(true);
        m_building_dimensions_error->hide();
        building_form_layout->addRow(m_building_dimensions_error);
        auto* apply_building = new QPushButton(QStringLiteral("Apply changes"), m_building_properties_group);
        apply_building->setObjectName(QStringLiteral("applyBuildingDimensions"));
        building_form_layout->addRow(apply_building);
        QObject::connect(apply_building, &QPushButton::clicked, owner, [this] { (void)editSelectedBuildingDimensions(); });
        m_building_properties_group->hide();
        inspector_layout->addWidget(m_building_properties_group);
        m_roof_properties_group->setVisible(false);
        inspector_layout->addWidget(m_roof_properties_group);
        QObject::connect(m_apply_roof_properties_button, &QPushButton::clicked, owner, [this] {
            (void)editSelectedRoofDimensions(m_roof_run_edit->text(),
                                                   m_roof_span_edit->text(),
                                                   m_roof_rise_edit->text(),
                                                   m_roof_overhang_edit->text(),
                                                   m_roof_thickness_edit->text());
        });
        m_delete_annotation_button = new QPushButton(QStringLiteral("Delete annotation"), inspector_body);
        m_delete_annotation_button->setObjectName(QStringLiteral("deleteAnnotation"));
        m_delete_annotation_button->setVisible(false);
        inspector_layout->addWidget(m_delete_annotation_button);
        QObject::connect(m_delete_annotation_button, &QPushButton::clicked, owner,
                         [this] { (void)deleteAnnotation(m_selected_id); });
        m_project_details_group = new QGroupBox(QStringLiteral("Project details"), inspector_body);
        m_project_details_group->setObjectName(QStringLiteral("projectDetails"));
        auto* project_details_form = new QFormLayout(m_project_details_group);
        m_project_subject_name_edit = new QLineEdit(m_project_details_group);
        m_project_subject_name_edit->setObjectName(QStringLiteral("projectSubjectName"));
        project_details_form->addRow(QStringLiteral("Name"), m_project_subject_name_edit);
        m_project_subject_address_edit = new QLineEdit(m_project_details_group);
        m_project_subject_address_edit->setObjectName(QStringLiteral("projectSubjectAddress"));
        project_details_form->addRow(QStringLiteral("Address"), m_project_subject_address_edit);
        m_project_subject_reference_edit = new QLineEdit(m_project_details_group);
        m_project_subject_reference_edit->setObjectName(QStringLiteral("projectSubjectReference"));
        project_details_form->addRow(QStringLiteral("Reference"), m_project_subject_reference_edit);
        m_project_subject_attributes_edit = new QPlainTextEdit(m_project_details_group);
        m_project_subject_attributes_edit->setObjectName(QStringLiteral("projectSubjectAttributes"));
        m_project_subject_attributes_edit->setPlaceholderText(QStringLiteral("{\"key\": \"value\"}"));
        m_project_subject_attributes_edit->setMaximumHeight(82);
        m_project_subject_attributes_edit->setTabChangesFocus(true);
        project_details_form->addRow(QStringLiteral("Attributes (JSON)"), m_project_subject_attributes_edit);
        m_apply_project_details_button = new QPushButton(QStringLiteral("Apply project details"),
                                                          m_project_details_group);
        m_apply_project_details_button->setObjectName(QStringLiteral("applyProjectDetails"));
        project_details_form->addRow(m_apply_project_details_button);
        m_project_details_group->setVisible(false);
        inspector_layout->addWidget(m_project_details_group);
        QObject::connect(m_apply_project_details_button, &QPushButton::clicked, owner, [this] {
            (void)editProjectSubject(m_project_subject_name_edit->text(),
                                     m_project_subject_address_edit->text(),
                                     m_project_subject_reference_edit->text(),
                                     m_project_subject_attributes_edit->toPlainText());
        });
        m_annotation_group = new QGroupBox(QStringLiteral("Annotation properties"), inspector_body);
        m_annotation_group->setObjectName(QStringLiteral("annotationProperties"));
        auto* annotation_layout = new QFormLayout(m_annotation_group);
        m_annotation_content_edit = new QLineEdit(m_annotation_group);
        m_annotation_content_edit->setObjectName(QStringLiteral("annotationContent"));
        annotation_layout->addRow(QStringLiteral("Text"), m_annotation_content_edit);
        m_annotation_x_edit = new QLineEdit(m_annotation_group);
        m_annotation_x_edit->setObjectName(QStringLiteral("annotationX"));
        annotation_layout->addRow(QStringLiteral("X (m)"), m_annotation_x_edit);
        m_annotation_y_edit = new QLineEdit(m_annotation_group);
        m_annotation_y_edit->setObjectName(QStringLiteral("annotationY"));
        annotation_layout->addRow(QStringLiteral("Y (m)"), m_annotation_y_edit);
        m_annotation_rotation_edit = new QLineEdit(m_annotation_group);
        m_annotation_rotation_edit->setObjectName(QStringLiteral("annotationRotation"));
        annotation_layout->addRow(QStringLiteral("Rotation (deg)"), m_annotation_rotation_edit);
        m_annotation_scale_edit = new QLineEdit(m_annotation_group);
        m_annotation_scale_edit->setObjectName(QStringLiteral("annotationScale"));
        annotation_layout->addRow(QStringLiteral("Scale"), m_annotation_scale_edit);
        m_annotation_visible_check = new QCheckBox(QStringLiteral("Visible"), m_annotation_group);
        m_annotation_visible_check->setObjectName(QStringLiteral("annotationVisible"));
        annotation_layout->addRow(m_annotation_visible_check);
        m_apply_annotation_button = new QPushButton(QStringLiteral("Apply annotation"), m_annotation_group);
        m_apply_annotation_button->setObjectName(QStringLiteral("applyAnnotation"));
        annotation_layout->addRow(m_apply_annotation_button);
        m_annotation_group->setVisible(false);
        inspector_layout->addWidget(m_annotation_group);
        QObject::connect(m_apply_annotation_button, &QPushButton::clicked, owner, [this] {
            (void)editAnnotation(m_selected_id, m_annotation_content_edit->text(),
                                 m_annotation_x_edit->text(), m_annotation_y_edit->text(),
                                 m_annotation_rotation_edit->text(), m_annotation_scale_edit->text(),
                                 m_annotation_visible_check->isChecked());
        });
        m_reference_group = new QGroupBox(QStringLiteral("Reference image"), inspector_body);
        m_reference_group->setObjectName(QStringLiteral("referenceProperties"));
        auto* reference_layout = new QFormLayout(m_reference_group);
        m_reference_x_edit = new QLineEdit(m_reference_group);
        m_reference_x_edit->setObjectName(QStringLiteral("referenceX"));
        reference_layout->addRow(QStringLiteral("X (m)"), m_reference_x_edit);
        m_reference_y_edit = new QLineEdit(m_reference_group);
        m_reference_y_edit->setObjectName(QStringLiteral("referenceY"));
        reference_layout->addRow(QStringLiteral("Y (m)"), m_reference_y_edit);
        m_reference_calibration_edit = new QLineEdit(m_reference_group);
        m_reference_calibration_edit->setObjectName(QStringLiteral("referenceCalibration"));
        m_reference_calibration_edit->setToolTip(QStringLiteral("Model metres per source pixel"));
        reference_layout->addRow(QStringLiteral("Metres / pixel"), m_reference_calibration_edit);
        m_reference_scale_edit = new QLineEdit(m_reference_group);
        m_reference_scale_edit->setObjectName(QStringLiteral("referenceScale"));
        reference_layout->addRow(QStringLiteral("Scale"), m_reference_scale_edit);
        m_reference_rotation_edit = new QLineEdit(m_reference_group);
        m_reference_rotation_edit->setObjectName(QStringLiteral("referenceRotation"));
        reference_layout->addRow(QStringLiteral("Rotation (deg)"), m_reference_rotation_edit);
        m_reference_intensity_edit = new QLineEdit(m_reference_group);
        m_reference_intensity_edit->setObjectName(QStringLiteral("referenceIntensity"));
        reference_layout->addRow(QStringLiteral("Intensity (0..1)"), m_reference_intensity_edit);
        m_reference_flip_horizontal_check = new QCheckBox(QStringLiteral("Flip horizontally"), m_reference_group);
        m_reference_flip_horizontal_check->setObjectName(QStringLiteral("referenceFlipHorizontal"));
        reference_layout->addRow(m_reference_flip_horizontal_check);
        m_reference_flip_vertical_check = new QCheckBox(QStringLiteral("Flip vertically"), m_reference_group);
        m_reference_flip_vertical_check->setObjectName(QStringLiteral("referenceFlipVertical"));
        reference_layout->addRow(m_reference_flip_vertical_check);
        m_reference_visible_check = new QCheckBox(QStringLiteral("Visible"), m_reference_group);
        m_reference_visible_check->setObjectName(QStringLiteral("referenceVisible"));
        reference_layout->addRow(m_reference_visible_check);
        m_apply_reference_button = new QPushButton(QStringLiteral("Apply reference"), m_reference_group);
        m_apply_reference_button->setObjectName(QStringLiteral("applyReference"));
        reference_layout->addRow(m_apply_reference_button);
        m_calibrate_reference_button = new QPushButton(QStringLiteral("Calibrate known distance…"), m_reference_group);
        m_calibrate_reference_button->setObjectName(QStringLiteral("calibrateReference"));
        reference_layout->addRow(m_calibrate_reference_button);
        m_reference_group->setVisible(false);
        inspector_layout->addWidget(m_reference_group);
        QObject::connect(m_apply_reference_button, &QPushButton::clicked, owner, [this] {
            (void)editReferenceTransform(
                m_selected_id, m_reference_x_edit->text(), m_reference_y_edit->text(),
                m_reference_calibration_edit->text(), m_reference_scale_edit->text(),
                m_reference_rotation_edit->text(), m_reference_intensity_edit->text(),
                m_reference_flip_horizontal_check->isChecked(),
                m_reference_flip_vertical_check->isChecked(),
                m_reference_visible_check->isChecked());
        });
        QObject::connect(m_calibrate_reference_button, &QPushButton::clicked, owner,
                         [this] { showReferenceCalibration(); });
        m_constraint_button = new QPushButton(QStringLiteral("Dimensions and constraints…"), inspector_body);
        m_constraint_button->setObjectName(QStringLiteral("editWallConstraints"));
        inspector_layout->addWidget(m_constraint_button);
        QObject::connect(m_constraint_button, &QPushButton::clicked, owner, [this] { showConstraintEditor(); });
        auto* form = new QFormLayout;
        m_geometry_form = form;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        m_length_edit = new QLineEdit(inspector_body);
        m_length_edit->setObjectName(QStringLiteral("inspectorLength"));
        m_length_edit->setMinimumWidth(0);
        form->addRow(QStringLiteral("Length"), m_length_edit);
        m_classification_combo = new QComboBox(inspector_body);
        m_classification_combo->setEditable(true);
        m_classification_combo->setMinimumWidth(0);
        m_classification_combo->addItems({QStringLiteral("measurement"), QStringLiteral("interior"),
                                          QStringLiteral("exterior"), QStringLiteral("party"),
                                          QStringLiteral("room"), QStringLiteral("door"),
                                          QStringLiteral("window"), QStringLiteral("slab")});
        form->addRow(QStringLiteral("Classification"), m_classification_combo);
        m_height_edit = new QLineEdit(inspector_body);
        m_height_edit->setObjectName(QStringLiteral("inspectorHeight"));
        m_height_edit->setMinimumWidth(0);
        form->addRow(QStringLiteral("Height"), m_height_edit);
        m_thickness_edit = new QLineEdit(inspector_body);
        m_thickness_edit->setObjectName(QStringLiteral("inspectorThickness"));
        m_thickness_edit->setMinimumWidth(0);
        form->addRow(QStringLiteral("Thickness"), m_thickness_edit);
        inspector_layout->addLayout(form);

        auto* keypad = new QPushButton(QStringLiteral("Measurement keypad…"), inspector_body);
        keypad->setObjectName(QStringLiteral("measurementKeypad"));
        keypad->setAccessibleName(QStringLiteral("Open measurement keypad"));
        inspector_layout->addWidget(keypad);
        QObject::connect(keypad, &QPushButton::clicked, owner, [this] { showMeasurementKeypad(); });

        auto* calculation_group = new QGroupBox(QStringLiteral("Area calculation"), inspector_body);
        m_calculation_group = calculation_group;
        calculation_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* calculation_layout = new QFormLayout(calculation_group);
        calculation_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
        const auto configure_value_label = [](QLabel* label) {
            label->setMinimumWidth(0);
            auto policy = label->sizePolicy();
            policy.setHorizontalPolicy(QSizePolicy::Preferred);
            policy.setVerticalPolicy(QSizePolicy::Preferred);
            policy.setHeightForWidth(label->wordWrap());
            label->setSizePolicy(policy);
            label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        };
        m_calculation_status = new QLabel(calculation_group);
        m_calculation_status->setObjectName(QStringLiteral("calculationStatus"));
        m_calculation_status->setWordWrap(true);
        configure_value_label(m_calculation_status);
        calculation_layout->addRow(m_calculation_status);
        m_calculation_base_value = new QLabel(calculation_group);
        m_calculation_base_value->setObjectName(QStringLiteral("calculationBaseArea"));
        configure_value_label(m_calculation_base_value);
        calculation_layout->addRow(QStringLiteral("Base area"), m_calculation_base_value);
        m_calculation_net_value = new QLabel(calculation_group);
        m_calculation_net_value->setObjectName(QStringLiteral("calculationNetArea"));
        configure_value_label(m_calculation_net_value);
        calculation_layout->addRow(QStringLiteral("Net area"), m_calculation_net_value);
        m_calculation_factored_value = new QLabel(calculation_group);
        m_calculation_factored_value->setObjectName(QStringLiteral("calculationFactoredArea"));
        configure_value_label(m_calculation_factored_value);
        calculation_layout->addRow(QStringLiteral("Factored area"), m_calculation_factored_value);
        m_calculation_perimeter_value = new QLabel(calculation_group);
        m_calculation_perimeter_value->setObjectName(QStringLiteral("calculationPerimeter"));
        configure_value_label(m_calculation_perimeter_value);
        calculation_layout->addRow(QStringLiteral("Perimeter"), m_calculation_perimeter_value);
        m_calculation_deductions_list = new QListWidget(calculation_group);
        m_calculation_deductions_list->setObjectName(QStringLiteral("calculationDeductions"));
        m_calculation_deductions_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_calculation_deductions_list->setMinimumHeight(0);
        m_calculation_deductions_list->setMaximumHeight(72);
        m_calculation_deductions_list->setAlternatingRowColors(false);
        calculation_layout->addRow(QStringLiteral("Deductions"), m_calculation_deductions_list);
        m_edit_deductions_button = new QPushButton(QStringLiteral("Edit deductions…"), calculation_group);
        m_edit_deductions_button->setObjectName(QStringLiteral("editDeductions"));
        m_edit_deductions_button->setAccessibleName(QStringLiteral("Edit area deductions"));
        calculation_layout->addRow(m_edit_deductions_button);
        QObject::connect(m_edit_deductions_button, &QPushButton::clicked, owner,
                         [this] { showDeductionEditor(); });
        m_calculation_rounding_value = new QLabel(calculation_group);
        m_calculation_rounding_value->setObjectName(QStringLiteral("calculationRounding"));
        m_calculation_rounding_value->setWordWrap(true);
        configure_value_label(m_calculation_rounding_value);
        calculation_layout->addRow(m_calculation_rounding_value);
        m_calculation_building_total_value = new QLabel(calculation_group);
        m_calculation_building_total_value->setObjectName(QStringLiteral("calculationBuildingTotal"));
        configure_value_label(m_calculation_building_total_value);
        calculation_layout->addRow(QStringLiteral("Building total"), m_calculation_building_total_value);
        m_calculation_living_total_value = new QLabel(calculation_group);
        m_calculation_living_total_value->setObjectName(QStringLiteral("calculationLivingTotal"));
        configure_value_label(m_calculation_living_total_value);
        calculation_layout->addRow(QStringLiteral("Living total"), m_calculation_living_total_value);
        m_factor_edit = new QLineEdit(calculation_group);
        m_factor_edit->setObjectName(QStringLiteral("inspectorFactor"));
        m_factor_edit->setPlaceholderText(QStringLiteral("1, 3/4, or 0.75"));
        m_factor_edit->setMinimumWidth(0);
        calculation_layout->addRow(QStringLiteral("Factor"), m_factor_edit);
        inspector_layout->addWidget(calculation_group);

        auto* profile_group = new QGroupBox(QStringLiteral("Calculation profile"), inspector_body);
        m_profile_group = profile_group;
        profile_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* profile_layout = new QVBoxLayout(profile_group);
        m_calculation_profile_context = new QLabel(profile_group);
        m_calculation_profile_context->setObjectName(QStringLiteral("calculationProfileContext"));
        m_calculation_profile_context->setWordWrap(true);
        configure_value_label(m_calculation_profile_context);
        profile_layout->addWidget(m_calculation_profile_context);
        m_calculation_profile_version = new QLabel(profile_group);
        m_calculation_profile_version->setObjectName(QStringLiteral("calculationProfileVersion"));
        configure_value_label(m_calculation_profile_version);
        profile_layout->addWidget(m_calculation_profile_version);
        m_include_building_check = new QCheckBox(QStringLiteral("Include in building total"),
                                                  profile_group);
        m_include_building_check->setObjectName(QStringLiteral("includeBuildingTotal"));
        profile_layout->addWidget(m_include_building_check);
        m_include_living_check = new QCheckBox(QStringLiteral("Include in living total"),
                                                profile_group);
        m_include_living_check->setObjectName(QStringLiteral("includeLivingTotal"));
        profile_layout->addWidget(m_include_living_check);
        inspector_layout->addWidget(profile_group);
        m_area_attributes_group = new QGroupBox(QStringLiteral("Area attributes"), inspector_body);
        m_area_attributes_group->setObjectName(QStringLiteral("areaAttributes"));
        auto* area_attributes_layout = new QFormLayout(m_area_attributes_group);
        m_area_attributes_edit = new QPlainTextEdit(m_area_attributes_group);
        m_area_attributes_edit->setObjectName(QStringLiteral("areaAttributesJson"));
        m_area_attributes_edit->setPlaceholderText(QStringLiteral("{\"key\": \"value\"}"));
        m_area_attributes_edit->setMaximumHeight(82);
        m_area_attributes_edit->setTabChangesFocus(true);
        area_attributes_layout->addRow(QStringLiteral("Attributes (JSON)"), m_area_attributes_edit);
        m_apply_area_attributes_button = new QPushButton(QStringLiteral("Apply area attributes"),
                                                          m_area_attributes_group);
        m_apply_area_attributes_button->setObjectName(QStringLiteral("applyAreaAttributes"));
        area_attributes_layout->addRow(m_apply_area_attributes_button);
        m_area_attributes_group->setVisible(false);
        inspector_layout->addWidget(m_area_attributes_group);
        QObject::connect(m_apply_area_attributes_button, &QPushButton::clicked, owner, [this] {
            (void)editSelectedAreaAttributes(m_area_attributes_edit->toPlainText());
        });
        m_read_only_label = new QLabel(inspector_body);
        m_read_only_label->setWordWrap(true);
        m_read_only_label->setStyleSheet(QStringLiteral("color:#d59564;"));
        inspector_layout->addWidget(m_read_only_label);
        inspector_layout->addStretch();
        m_inspector->setWidget(inspector_body);

        QObject::connect(m_length_edit, &QLineEdit::editingFinished, owner,
                         [this] {
                             if (m_refreshing || !m_length_edit->isModified()) return;
                             m_length_edit->setModified(false);
                             const auto entity = selectedEntity();
                             if (entity && entity->type == "wall") showConstraintEditor(m_length_edit->text());
                             else editSelectedLength(m_length_edit->text());
                         });
        QObject::connect(m_height_edit, &QLineEdit::editingFinished, owner,
                         [this] { editSelectedHeight(m_height_edit->text()); });
        QObject::connect(m_thickness_edit, &QLineEdit::editingFinished, owner,
                         [this] { editSelectedThickness(m_thickness_edit->text()); });
        QObject::connect(m_factor_edit, &QLineEdit::editingFinished, owner,
                         [this] { editSelectedFactor(m_factor_edit->text()); });
        QObject::connect(m_include_building_check, &QCheckBox::toggled, owner,
                         [this](bool) {
                             if (!m_refreshing) {
                                 setSelectedCalculationRule(m_include_building_check->isChecked(),
                                                            m_include_living_check->isChecked());
                             }
                         });
        QObject::connect(m_include_living_check, &QCheckBox::toggled, owner,
                         [this](bool) {
                             if (!m_refreshing) {
                                 setSelectedCalculationRule(m_include_building_check->isChecked(),
                                                            m_include_living_check->isChecked());
                             }
                         });
        QObject::connect(m_classification_combo, &QComboBox::currentTextChanged, owner,
                         [this](const QString& text) {
                             if (!m_refreshing) {
                                 editSelectedClassification(text);
                             }
                         });

        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 0);
        splitter->setStretchFactor(2, 1);
        splitter->setStretchFactor(3, 0);
        owner->setCentralWidget(central);
        owner->statusBar()->clearMessage();
    }

    QToolButton* addToolButton(QVBoxLayout* layout, const QString& label, CanvasTool tool,
                               bool checked = false) {
        auto* button = new QToolButton(layout->parentWidget());
        button->setText(label);
        switch (tool) {
        case CanvasTool::select:
            button->setIcon(modern_toolbar_icon(
                "<path d='M6 3l12 9-6 1-3 7L6 3z'/><path d='m11 16 3 3'/>"));
            button->setObjectName(QStringLiteral("selectTool"));
            break;
        case CanvasTool::boundary:
            button->setIcon(modern_toolbar_icon(
                "<path d='M5 5h14v14H5z'/><path d='M5 12h14M12 5v14'/>"));
            button->setObjectName(QStringLiteral("drawFirstTool"));
            break;
        case CanvasTool::wall:
            button->setIcon(modern_toolbar_icon(
                "<path d='M5 4v16M19 4v16M5 8h14M5 16h14'/>"));
            button->setObjectName(QStringLiteral("wallTool"));
            break;
        }
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setToolTip(tool_name(tool));
        button->setCheckable(true);
        button->setChecked(checked);
        button->setAutoRaise(true);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(button);
        QObject::connect(button, &QToolButton::clicked, owner, [this, tool] { setTool(tool); });
        return button;
    }

    void connectCanvas(PlanCanvas* canvas) {
        canvas->setPointClicked([this](Vec2 point) { onCanvasPoint(point); });
        canvas->setEntityClicked([this](QString id) { selectEntity(id); });
        canvas->setCursorMoved([this, canvas](Vec2 point) {
            // Snap toggles update both canvases; only the active workspace
            // owns the shared authoring pointer and cursor status.
            const auto* active = m_workspace == Workspace::measurement
                                     ? m_measurementCanvas : m_architecturalCanvas;
            if (canvas != active) return;
            m_last_cursor = point;
            refreshCursorLabel(point);
            if (m_boundary_session) {
                m_boundary_session->set_pointer(point);
                refreshBoundaryPreview();
            }
        });
        canvas->setFinishRequested([this] { finishTool(); });
        canvas->setCancelRequested([this] { cancelTool(); });
        canvas->setPreciseInputRequested([this] { preciseBoundaryInput(); });
        canvas->setDraftUndoRequested([this] { (void)undoCommand(); });
        canvas->setDraftRedoRequested([this] { (void)redoCommand(); });
    }

    void refresh() {
        m_refreshing = true;
        refreshCanvases();
        refreshNavigator();
        refreshInspector();
        refreshActions();
        refreshTitle();
        m_refreshing = false;
    }

    void refreshCanvases() {
        const auto snapshot = m_document->snapshot();
        m_plan_geometry_error.clear();
        std::erase_if(m_plan_projection_cache, [&](const auto& entry) {
            return !snapshot.entities().contains(entry.first);
        });
        std::erase_if(m_plan_slab_validation_cache, [&](const auto& entry) {
            return !snapshot.entities().contains(entry.first);
        });
        std::vector<CanvasEntity> all_geometry;
        std::vector<CanvasLabel> all_labels;
        std::vector<CanvasReference> reference_underlays;
        all_geometry.reserve(snapshot.entities().size());
        const auto append_geometry_error = [&](const QString& message) {
            if (!m_plan_geometry_error.isEmpty()) {
                m_plan_geometry_error += QLatin1Char('\n');
            }
            m_plan_geometry_error += message;
        };
        std::map<std::string, std::vector<HostedOpening>, std::less<>> openings_by_wall;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type == "reference_asset") {
                try {
                    const auto asset_id = read_string(entity.properties, "asset_id");
                    if (!asset_id.has_value()) throw std::invalid_argument("asset_id is required");
                    const auto render_asset_id = read_string(entity.properties, "render_asset_id")
                        .value_or(*asset_id);
                    const auto asset = snapshot.assets().find(render_asset_id);
                    if (asset == snapshot.assets().end()) {
                        throw std::invalid_argument("render asset is missing");
                    }
                    const QByteArray raw(reinterpret_cast<const char*>(asset->second.bytes.data()),
                                         static_cast<qsizetype>(asset->second.bytes.size()));
                    const auto image = QImage::fromData(raw);
                    if (image.isNull()) throw std::invalid_argument("raster asset could not be decoded");
                    const auto position = read_point(
                        entity.properties.contains("position_m")
                            ? entity.properties.at("position_m") : json{});
                    if (!position.has_value()) throw std::invalid_argument("position_m must be [x, y]");
                    const auto metres_per_source_unit =
                        read_number(entity.properties, "metres_per_source_unit", 0.01);
                    const auto transform_scale = read_number(entity.properties, "scale", 1.0);
                    const auto rotation = read_number(entity.properties, "rotation_degrees", 0.0);
                    const auto intensity = read_number(entity.properties, "intensity", 1.0);
                    if (!(metres_per_source_unit > 0.0) || !(transform_scale > 0.0) ||
                        !std::isfinite(metres_per_source_unit) || !std::isfinite(transform_scale) ||
                        !std::isfinite(rotation) || !std::isfinite(intensity) || intensity < 0.0 ||
                        intensity > 1.0) {
                        throw std::invalid_argument("reference transform is invalid");
                    }
                    const auto boolean_property = [&](std::string_view key, bool fallback) {
                        const auto value = entity.properties.value(std::string(key), json(fallback));
                        if (!value.is_boolean()) throw std::invalid_argument("reference boolean property is invalid");
                        return value.get<bool>();
                    };
                    reference_underlays.push_back(CanvasReference{
                        id_from(id), image, *position, metres_per_source_unit, transform_scale,
                        rotation, boolean_property("flip_horizontal", false),
                        boolean_property("flip_vertical", false), intensity,
                        boolean_property("visible", true), id_from(id) == m_selected_id});
                } catch (const std::exception& error) {
                    append_geometry_error(QStringLiteral("Reference %1: %2")
                                              .arg(id_from(id), QString::fromUtf8(error.what())));
                }
                continue;
            }
            if (entity.type != "opening") {
                continue;
            }
            const auto wall_id = read_string(entity.properties, "wall_id");
            if (!wall_id.has_value()) {
                append_geometry_error(QStringLiteral("Opening %1: wall_id is required").arg(id_from(id)));
                continue;
            }
            QString opening_error;
            const auto opening = read_hosted_opening(entity, &opening_error);
            if (!opening.has_value()) {
                append_geometry_error(QStringLiteral("Opening %1: %2")
                                          .arg(id_from(id), opening_error));
                continue;
            }
            const auto host = snapshot.entities().find(*wall_id);
            if (host == snapshot.entities().end() || host->second.type != "wall") {
                append_geometry_error(QStringLiteral("Opening %1: host wall %2 is missing")
                                          .arg(id_from(id), id_from(*wall_id)));
                continue;
            }
            openings_by_wall[*wall_id].push_back(*opening);
        }
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type == "opening" && entity.properties.value("opening_kind", std::string{}) == "door" &&
                entity.properties.contains("door_operation")) {
                try {
                    const auto host = snapshot.entities().find(entity.properties.at("wall_id").get<std::string>());
                    if(host == snapshot.entities().end()) throw std::invalid_argument("host wall is missing");
                    const auto baseline = read_required_segment(host->second.properties, "baseline");
                    const auto opening = read_hosted_opening(entity);
                    if(!baseline || !opening) throw std::invalid_argument("door geometry is incomplete");
                    all_geometry.push_back(CanvasEntity{id_from(id),"opening",
                        door_plan_symbol(*baseline,opening->offset,opening->width,
                            decode_door_operation(entity.properties.at("door_operation"))),0,id_from(id)==m_selected_id});
                } catch(const std::exception& error) {
                    append_geometry_error(QStringLiteral("Door %1: %2").arg(id_from(id),QString::fromUtf8(error.what())));
                }
                continue;
            }
            if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                try {
                    const auto decoded = decode_boundary_dimension_entity(entity);
                    if (!decoded.supported()) throw std::invalid_argument(decoded.unsupported_reason);
                    const auto& dimension = *decoded.dimension;
                    const auto source = snapshot.entities().find(dimension.boundary_id);
                    if (source == snapshot.entities().end())
                        throw std::invalid_argument("source boundary is missing");
                    const auto resolved = dimension.resolve(source->second);
                    all_labels.push_back({id_from(id), dimension.text_position,
                        format_length(resolved.segment_length_metres, m_metric_units),
                        id_from(id) == m_selected_id});
                } catch (const std::exception& error) {
                    append_geometry_error(QStringLiteral("Dimension %1: %2")
                        .arg(id_from(id), QString::fromUtf8(error.what())));
                }
                continue;
            }
            if (can_recognize_building_entity_type(entity.type)) {
                try {
                    const auto key = entity.type + '\n' + entity.properties.dump();
                    auto cached = m_plan_projection_cache.find(id);
                    if (cached == m_plan_projection_cache.end() || cached->second.first != key) {
                        auto projection = project_building_plan(decode_building_entity(entity));
                        cached = m_plan_projection_cache.insert_or_assign(
                            id, std::make_pair(key, std::move(projection))).first;
                    }
                    all_geometry.push_back(CanvasEntity{id_from(id), QString::fromStdString(entity.type),
                        cached->second.second, 0.0, id_from(id) == m_selected_id});
                } catch (const std::exception& error) {
                    m_plan_projection_cache.erase(id);
                    append_geometry_error(QStringLiteral("Object %1: %2")
                        .arg(id_from(id), QString::fromUtf8(error.what())));
                }
                continue;
            }
            if (entity.type != "wall" && !is_closed_boundary_entity(entity.type) &&
                entity.type != "slab") {
                continue;
            }
            Boundary segments;
            if (entity.type == "wall") {
                const auto baseline = read_required_segment(entity.properties, "baseline");
                if (!baseline.has_value()) {
                    append_geometry_error(QStringLiteral("Wall %1: baseline is invalid or missing")
                                              .arg(id_from(id)));
                    continue;
                }
                segments.push_back(*baseline);
                const auto thickness = read_finite_number(entity.properties, "thickness_m");
                const auto height = read_finite_number(entity.properties, "height_m");
                const auto elevation = read_finite_number(entity.properties, "elevation_m");
                if (!thickness.has_value() || !height.has_value() || !elevation.has_value()) {
                    append_geometry_error(QStringLiteral("Wall %1: thickness, height, and elevation are required")
                                              .arg(id_from(id)));
                    continue;
                }
                try {
                    validate_wall_semantics(Wall{id, segments.front(), *thickness, *height,
                                                 *elevation, openings_by_wall[id]});
                } catch (const std::exception& error) {
                    append_geometry_error(QStringLiteral("Wall %1: %2")
                                              .arg(id_from(id), QString::fromUtf8(error.what())));
                    continue;
                }
                segments = wall_segments_without_openings(segments.front(), openings_by_wall[id]);
            } else if (entity.type == "slab") {
                const auto boundary = read_required_boundary(entity.properties, "boundary");
                if (!boundary.has_value()) {
                    append_geometry_error(QStringLiteral("Slab %1: boundary is invalid or missing")
                                              .arg(id_from(id)));
                    continue;
                }
                segments = *boundary;
                const auto key = entity.type + '\n' + entity.properties.dump();
                auto cached = m_plan_slab_validation_cache.find(id);
                if (cached == m_plan_slab_validation_cache.end() || cached->second.first != key) {
                    QString validation_error;
                    const auto holes = read_required_holes(entity.properties);
                    if (!holes.has_value()) {
                        validation_error = QStringLiteral("holes must be an array");
                    } else {
                        const auto thickness = read_finite_number(entity.properties, "thickness_m");
                        const auto elevation = read_finite_number(entity.properties, "elevation_m");
                        if (!thickness.has_value() || !elevation.has_value()) {
                            validation_error = QStringLiteral("thickness and elevation are required");
                        } else {
                            try {
                                (void)make_slab(Slab{id, segments, *holes, *thickness, *elevation});
                            } catch (const std::exception& error) {
                                validation_error = QString::fromUtf8(error.what());
                            }
                        }
                    }
                    cached = m_plan_slab_validation_cache
                                 .insert_or_assign(id, std::make_pair(key, validation_error))
                                 .first;
                }
                if (!cached->second.second.isEmpty()) {
                    append_geometry_error(QStringLiteral("Slab %1: %2")
                                              .arg(id_from(id), cached->second.second));
                    continue;
                }
            } else {
                segments = read_boundary(entity.properties);
                if (segments.empty()) {
                    append_geometry_error(QStringLiteral("%1 %2: no valid boundary segments")
                                              .arg(QString::fromStdString(entity.type), id_from(id)));
                    continue;
                }
                const auto diagnostics = validate_boundary(segments);
                if (!diagnostics.empty()) {
                    append_geometry_error(QStringLiteral("Boundary %1: %2")
                                              .arg(id_from(id),
                                                   QString::fromStdString(diagnostics.front().message)));
                    continue;
                }
            }
            all_geometry.push_back(CanvasEntity{id_from(id),
                                                QString::fromStdString(entity.type),
                                                segments,
                                                read_number(entity.properties, "thickness_m", 0.08),
                                                id_from(id) == m_selected_id});
        }
        // Presentation annotations are kept in a typed entity, but their
        // child IDs are still rendered as ordinary retained canvas values so
        // both interactive and persisted output use the same vector path.
        std::vector<std::string> annotation_child_ids;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type != kAnnotationEntityType) continue;
            try {
                const auto state = decode_annotation_entity(entity);
                const auto catalog = default_symbol_catalog();
                for (const auto& label : state.labels) {
                    if (!label.visible) continue;
                    annotation_child_ids.push_back(label.id);
                    all_labels.push_back({id_from(label.id), label.placement.position,
                                          QString::fromStdString(label.content),
                                          id_from(label.id) == m_selected_id,
                                          label.placement.rotation_radians,
                                          label.placement.scale,
                                          label.style.text_height_metres});
                }
                for (const auto& symbol : state.symbols) {
                    if (!symbol.visible) continue;
                    const auto definition = std::find_if(
                        catalog.begin(), catalog.end(), [&](const auto& candidate) {
                            return candidate.id == symbol.symbol_id;
                        });
                    if (definition == catalog.end()) {
                        throw std::invalid_argument("annotation symbol definition is missing");
                    }
                    Boundary preview;
                    for (const auto& stroke : placed_symbol_preview(*definition, symbol.placement)) {
                        preview.push_back({stroke.start, stroke.end, 0.0});
                    }
                    annotation_child_ids.push_back(symbol.id);
                    all_geometry.push_back({id_from(symbol.id), QStringLiteral("symbol"),
                                            std::move(preview), 0.0,
                                            id_from(symbol.id) == m_selected_id});
                }
            } catch (const std::exception& error) {
                append_geometry_error(QStringLiteral("Annotations %1: %2")
                                           .arg(id_from(id), QString::fromUtf8(error.what())));
            }
        }
        // Measurement always retains its plan geometry. Build all three
        // architectural presentations from the same snapshot so persisted
        // sheet viewports can render independently of the active workspace.
        std::array<std::vector<CanvasEntity>, 3> view_geometry;
        view_geometry[architectural_view_index(BuildingViewKind::plan)] = all_geometry;
        const auto build_architectural_geometry = [&](BuildingViewKind kind) {
            if (kind == BuildingViewKind::plan) return all_geometry;
            std::vector<CanvasEntity> result;
            result.reserve(snapshot.entities().size());
            const auto frame = architectural_view_frame(snapshot, kind);
            for (const auto& [id, entity] : snapshot.entities()) {
                try {
                    if (can_recognize_building_entity_type(entity.type)) {
                        const auto key = "view:" + std::to_string(static_cast<int>(kind)) +
                                         '\n' + entity.type + '\n' + entity.properties.dump();
                        auto cached = m_plan_projection_cache.find(id);
                        if (cached == m_plan_projection_cache.end() || cached->second.first != key) {
                            auto projection = project_building_view(
                                decode_building_entity(entity), kind, frame);
                            cached = m_plan_projection_cache.insert_or_assign(
                                id, std::make_pair(key, std::move(projection))).first;
                        }
                        result.push_back(CanvasEntity{
                            id_from(id), QString::fromStdString(entity.type),
                            cached->second.second, 0.0, id_from(id) == m_selected_id});
                        continue;
                    }
                    if (entity.type == "wall") {
                        const auto baseline = read_required_segment(entity.properties, "baseline");
                        const auto thickness = read_finite_number(entity.properties, "thickness_m");
                        const auto height = read_finite_number(entity.properties, "height_m");
                        const auto elevation = read_finite_number(entity.properties, "elevation_m");
                        if (!baseline || !thickness || !height || !elevation) {
                            throw std::invalid_argument("wall projection requires baseline, thickness, height, and elevation");
                        }
                        const Wall wall{id, *baseline, *thickness, *height, *elevation,
                                        openings_by_wall[id]};
                        validate_wall_semantics(wall);
                        const auto projection = project_shape_view(
                            make_wall(wall), kind, frame);
                        result.push_back(CanvasEntity{
                            id_from(id), QStringLiteral("wall"), projection, *thickness,
                            id_from(id) == m_selected_id});
                        continue;
                    }
                    if (entity.type == "slab") {
                        const auto boundary = read_required_boundary(entity.properties, "boundary");
                        const auto holes = read_required_holes(entity.properties);
                        const auto thickness = read_finite_number(entity.properties, "thickness_m");
                        const auto elevation = read_finite_number(entity.properties, "elevation_m");
                        if (!boundary || !holes || !thickness || !elevation) {
                            throw std::invalid_argument("slab projection requires boundary, holes, thickness, and elevation");
                        }
                        const auto projection = project_shape_view(
                            make_slab(Slab{id, *boundary, *holes, *thickness, *elevation}),
                            kind, frame);
                        result.push_back(CanvasEntity{
                            id_from(id), QStringLiteral("slab"), projection, *thickness,
                            id_from(id) == m_selected_id});
                    }
                } catch (const std::exception& error) {
                    if (kind == m_architectural_view_kind) {
                        append_geometry_error(QStringLiteral("%1 view %2: %3")
                            .arg(kind == BuildingViewKind::elevation
                                     ? QStringLiteral("Elevation") : QStringLiteral("Section"),
                                 id_from(id), QString::fromUtf8(error.what())));
                    }
                }
            }
            return result;
        };
        for (const auto kind : {BuildingViewKind::elevation, BuildingViewKind::section}) {
            view_geometry[architectural_view_index(kind)] = build_architectural_geometry(kind);
        }
        // Every supported entity above has been parsed and validated before
        // the view mask is applied. Hidden invalid geometry therefore keeps
        // the output error visible and cannot become a way around validation.
        auto visible_ids = visible_project_entities(snapshot, m_view_filter);
        // Design-phase selection is a semantic view mask layered after
        // organization visibility. Geometry is still parsed and validated
        // above, so a demolished or alternate object can never hide an error
        // in the source document. Objects outside the phase registry remain
        // visible until an imported/project-owned phase model claims them.
        try {
            visible_ids = visible_project_entities_with_phase(snapshot, m_view_filter);
        } catch (const std::exception& error) {
            append_geometry_error(QStringLiteral("Design phase: %1")
                                      .arg(QString::fromUtf8(error.what())));
        }
        // Annotation children are presentation records nested under the
        // validated annotation entity rather than standalone Document
        // entities, so they inherit the parent's fail-open visibility.
        for (const auto& id : annotation_child_ids) visible_ids.insert(id);
        std::vector<CanvasEntity> geometry;
        std::array<std::vector<CanvasEntity>, 3> visible_view_geometry;
        geometry.reserve(all_geometry.size());
        for (auto& entity : all_geometry) {
            if (visible_ids.contains(entity.id.toStdString())) {
                geometry.push_back(std::move(entity));
            }
        }
        for (std::size_t index = 0; index < view_geometry.size(); ++index) {
            visible_view_geometry[index].reserve(view_geometry[index].size());
            for (auto& entity : view_geometry[index]) {
                if (visible_ids.contains(entity.id.toStdString())) {
                    visible_view_geometry[index].push_back(std::move(entity));
                }
            }
        }
        m_measurementCanvas->setEntities(geometry);
        m_architectural_view_entities = std::move(visible_view_geometry);
        m_architecturalCanvas->setEntities(
            m_architectural_view_entities[architectural_view_index(m_architectural_view_kind)]);
        std::vector<CanvasLabel> labels;
        for (auto& label : all_labels) {
            if (visible_ids.contains(label.id.toStdString())) labels.push_back(std::move(label));
        }
        m_measurementCanvas->setLabels(labels);
        m_architecturalCanvas->setLabels(std::move(labels));
        m_measurementCanvas->setReferences(reference_underlays);
        m_architecturalCanvas->setReferences(std::move(reference_underlays));
        m_plan_error_banner->setText(m_plan_geometry_error);
        m_plan_error_banner->setVisible(!m_plan_geometry_error.isEmpty());
        m_measurementCanvas->setSelectedId(m_selected_id);
        m_architecturalCanvas->setSelectedId(m_selected_id);
        m_measurementCanvas->setGridEnabled(m_grid_enabled);
        m_architecturalCanvas->setGridEnabled(m_grid_enabled);
        m_measurementCanvas->setSnapEnabled(m_snap_enabled);
        m_architecturalCanvas->setSnapEnabled(m_snap_enabled);
        m_measurementCanvas->setMetricUnits(m_metric_units);
        m_architecturalCanvas->setMetricUnits(m_metric_units);
        if (m_nativeModelView) {
            visualization::NativeModelView::VisibleEntityIds native_visible_ids;
            native_visible_ids.insert(visible_ids.begin(), visible_ids.end());
            m_nativeModelView->setSnapshot(snapshot, std::move(native_visible_ids));
        }
    }

    void showOrganizationDialog(const std::string& type) {
        const auto modal_context = captureModalContext();
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        const auto snapshot = m_document->snapshot();
        const auto selected = selectedEntity();
        QString parent_id;
        QString initial_name;
        if (type == "rename") {
            if (!selected || (selected->type != "property" && selected->type != "building" &&
                              selected->type != "floor" && selected->type != "layer")) {
                setError(QStringLiteral("Select a property, building, floor or layer to rename."));
                return;
            }
            parent_id = id_from(selected->id);
            initial_name = QString::fromStdString(read_string(selected->properties, "name").value_or(""));
        } else {
            const auto expected = type == "building" ? "property" : type == "floor" ? "building" : "floor";
            if (selected && selected->type == expected) parent_id = id_from(selected->id);
            else if (const auto context = organize_project(snapshot).drawing_context(m_active_layer_id.toStdString()))
                parent_id = id_from(type == "building" ? context->property_id :
                                    type == "floor" ? context->building_id : context->floor_id);
            if (parent_id.isEmpty()) {
                setError(QStringLiteral("Select the parent %1 in the navigator first.")
                             .arg(QString::fromUtf8(expected)));
                return;
            }
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(owner,
            type == "rename" ? QStringLiteral("Rename") : QStringLiteral("Add %1").arg(QString::fromStdString(type)),
            QStringLiteral("Name"), QLineEdit::Normal, initial_name, &accepted);
        if (!accepted) return;
        if (!modalContextUnchanged(modal_context)) return;
        if (type == "rename") renameOrganizationEntity(parent_id, name, modal_context.revision);
        else createOrganization(parent_id, name, type, modal_context.revision);
    }

    void refreshModelPhaseControl(const DocumentSnapshot& snapshot) {
        if (!m_model_phase_combo) return;
        const QSignalBlocker blocker(m_model_phase_combo);
        m_model_phase_combo->clear();
        try {
            const auto record = decode_phase_model(snapshot);
            if (!record) {
                m_model_phase_combo->addItem(QStringLiteral("Set up design phases…"));
                m_model_phase_combo->setEnabled(true);
                m_model_phase_combo->setToolTip(QStringLiteral(
                    "Create a persisted baseline and remodeling alternatives from the architectural model."));
                return;
            }
            m_model_phase_combo->addItem(QStringLiteral("Existing baseline"), QString{});
            for (const auto& alternative : record->model.alternatives()) {
                m_model_phase_combo->addItem(phase_alternative_label(alternative),
                                             id_from(alternative.id));
            }
            const auto active = record->model.active_alternative()
                ? id_from(*record->model.active_alternative()) : QString{};
            const auto index = m_model_phase_combo->findData(active);
            m_model_phase_combo->setCurrentIndex(index >= 0 ? index : 0);
            m_model_phase_combo->setToolTip(QStringLiteral(
                "Active design phase: %1. Use Design phases and alternatives to create or review options.")
                .arg(m_model_phase_combo->currentText()));
        } catch (const std::exception& error) {
            m_model_phase_combo->addItem(QStringLiteral("Invalid design phase record"));
            m_model_phase_combo->setEnabled(false);
            m_model_phase_combo->setToolTip(QString::fromUtf8(error.what()));
            return;
        }
        m_model_phase_combo->setEnabled(true);
    }

    void refreshNavigator() {
        std::map<QString, bool> expansion;
        for (QTreeWidgetItemIterator item(m_navigator); *item; ++item)
            expansion[(*item)->data(0, Qt::UserRole).toString()] = (*item)->isExpanded();
        const QSignalBlocker tree_blocker(m_navigator);
        m_navigator->clear();
        const auto snapshot = m_document->snapshot();
        const auto organization = organize_project(snapshot);
        std::set<std::string, std::less<>> visible_ids;
        try {
            visible_ids = visible_project_entities_with_phase(snapshot, m_view_filter);
        } catch (const std::exception&) {
            visible_ids = visible_project_entities(snapshot, m_view_filter);
        }
        std::map<std::string, QTreeWidgetItem*, std::less<>> items;
        for (const auto& [id, node] : organization.nodes) {
            const auto name = read_string(snapshot.entities().at(id).properties, "name");
            QString label = QString::fromStdString(name && !name->empty() ? *name : node.type);
            if (!name || name->empty()) {
                label.replace(QLatin1Char('_'), QLatin1Char(' '));
                if (!label.isEmpty()) label[0] = label[0].toUpper();
            }
            const bool visibility_container = is_visibility_container_type(node.type);
            const bool own_hidden = node.type == "floor"
                ? m_view_filter.hidden_floor_ids.contains(id)
                : node.type == "layer" && m_view_filter.hidden_layer_ids.contains(id);
            const bool hidden_by_floor = node.type == "layer" && !node.context.floor_id.empty() &&
                m_view_filter.hidden_floor_ids.contains(node.context.floor_id);
            const bool effective_visible = visible_ids.contains(id);
            if (visibility_container) {
                if (!effective_visible) {
                    if (hidden_by_floor && own_hidden) {
                        label += QStringLiteral(" (hidden by floor and layer)");
                    } else if (hidden_by_floor) {
                        label += QStringLiteral(" (hidden by floor)");
                    } else {
                        label += QStringLiteral(" (hidden)");
                    }
                } else if (own_hidden && !node.issues.empty()) {
                    label += QStringLiteral(" (visible: unresolved)");
                }
            }
            auto* item = new QTreeWidgetItem(QStringList{label});
            item->setData(0, Qt::UserRole, id_from(id));
            item->setData(0, visibility_type_role, QString::fromStdString(node.type));
            QString tooltip = id_from(id);
            for (const auto& issue : node.issues) tooltip += QLatin1Char('\n') + QString::fromStdString(issue);
            if (visibility_container) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                // The check state represents this container's own filter. A
                // checked layer therefore remains checked when its floor is
                // hidden, making the non-recursive behavior explicit.
                item->setCheckState(0, own_hidden ? Qt::Unchecked : Qt::Checked);
                tooltip += QStringLiteral("\nSpace toggles this container's view visibility only.");
                if (!effective_visible) {
                    tooltip += hidden_by_floor && own_hidden
                        ? QStringLiteral("\nEffective state: hidden by its floor and layer filters.")
                        : hidden_by_floor
                            ? QStringLiteral("\nEffective state: hidden by its floor filter.")
                            : QStringLiteral("\nEffective state: hidden by its layer filter.");
                } else if (own_hidden && !node.issues.empty()) {
                    tooltip += QStringLiteral("\nEffective state: visible because its organization is unresolved.");
                }
            }
            item->setToolTip(0, tooltip);
            if (!node.issues.empty()) item->setForeground(0, QColor(170, 35, 35));
            items.emplace(id, item);
        }
        QTreeWidgetItem* unassigned = nullptr;
        for (const auto& [id, node] : organization.nodes) {
            auto* item = items.at(id);
            if (!node.parent_id.empty()) {
                items.at(node.parent_id)->addChild(item);
            } else if (node.type == "property" && node.issues.empty()) {
                m_navigator->addTopLevelItem(item);
            } else {
                if (!unassigned) {
                    unassigned = new QTreeWidgetItem(m_navigator, {QStringLiteral("Unassigned / unresolved")});
                    unassigned->setExpanded(true);
                }
                unassigned->addChild(item);
            }
        }
        // Annotation labels and symbols live inside one validated typed
        // entity. Expose their stable child IDs in the navigator so they can
        // be selected, inspected, and removed without flattening the wire
        // format into renderer-only entities.
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type != kAnnotationEntityType) continue;
            const auto parent = items.find(id);
            if (parent == items.end()) continue;
            try {
                const auto state = decode_annotation_entity(entity);
                for (const auto& label : state.labels) {
                    auto* child = new QTreeWidgetItem(parent->second,
                        {QStringLiteral("Label  •  %1").arg(QString::fromStdString(label.content))});
                    child->setData(0, Qt::UserRole, id_from(label.id));
                    child->setToolTip(0, QString::fromStdString(label.id));
                    child->setSelected(id_from(label.id) == m_selected_id);
                    if (id_from(label.id) == m_selected_id) m_navigator->setCurrentItem(child);
                }
                for (const auto& symbol : state.symbols) {
                    auto* child = new QTreeWidgetItem(parent->second,
                        {QStringLiteral("Symbol  •  %1").arg(QString::fromStdString(symbol.symbol_id))});
                    child->setData(0, Qt::UserRole, id_from(symbol.id));
                    child->setToolTip(0, QString::fromStdString(symbol.id));
                    child->setSelected(id_from(symbol.id) == m_selected_id);
                    if (id_from(symbol.id) == m_selected_id) m_navigator->setCurrentItem(child);
                }
            } catch (const std::exception&) {
                // The entity validator reports malformed state in the main
                // canvas; do not make navigator refresh itself throw.
            }
        }
        for (const auto& [id, item] : items) {
            const auto previous = expansion.find(id_from(id));
            item->setExpanded(previous == expansion.end() || previous->second);
            item->setSelected(id_from(id) == m_selected_id);
            if (id_from(id) == m_selected_id) m_navigator->setCurrentItem(item);
        }
        const QSignalBlocker combo_blocker(m_drawing_layer_combo);
        refreshModelPhaseControl(snapshot);
        m_drawing_layer_combo->clear();
        for (const auto& [id, node] : organization.nodes) {
            if (node.type != "layer") continue;
            const auto context = organization.drawing_context(id);
            if (!context) continue;
            const auto display = [&](const std::string& entity_id) {
                const auto& value = organization.nodes.at(entity_id);
                return QString::fromStdString(value.name.empty() ? value.type : value.name);
            };
            const auto label = display(context->building_id) + QStringLiteral(" / ") +
                display(context->floor_id) + QStringLiteral(" / ") + display(id);
            m_drawing_layer_combo->addItem(label, id_from(id));
            m_drawing_layer_combo->setItemData(m_drawing_layer_combo->count() - 1, label, Qt::ToolTipRole);
        }
        const auto active_index = m_drawing_layer_combo->findData(m_active_layer_id);
        m_drawing_layer_combo->setCurrentIndex(active_index);
        m_drawing_layer_combo->setPlaceholderText(QStringLiteral("Choose a drawing layer"));
        auto context_label = active_index < 0
            ? QStringLiteral("Choose a drawing layer")
            : m_drawing_layer_combo->itemText(active_index);
        if (active_index >= 0 && !visible_ids.contains(m_active_layer_id.toStdString())) {
            context_label += QStringLiteral(
                "\nHidden by the view filter; drawing remains enabled on this layer.");
        }
        m_drawing_context_label->setText(context_label);
        m_visibility_label->setText(
            m_view_filter.hidden_floor_ids.empty() && m_view_filter.hidden_layer_ids.empty()
                ? QStringLiteral("All visible")
                : QStringLiteral("Filter active"));
        m_show_all_button->setEnabled(true);
    }

    void refreshCalculationInspector(const std::optional<EntityValue>& selected) {
        const bool is_area = selected.has_value() && is_closed_boundary_entity(selected->type);
        m_calculation_group->setVisible(is_area);
        m_profile_group->setVisible(is_area);
        const auto clear_values = [&] {
            m_calculation_base_value->setText(QStringLiteral("—"));
            m_calculation_net_value->setText(QStringLiteral("—"));
            m_calculation_factored_value->setText(QStringLiteral("—"));
            m_calculation_perimeter_value->setText(QStringLiteral("—"));
            m_calculation_rounding_value->setText(QStringLiteral("—"));
            m_calculation_building_total_value->setText(QStringLiteral("—"));
            m_calculation_living_total_value->setText(QStringLiteral("—"));
        };
        const auto set_status_style = [&](bool error) {
            m_calculation_status->setStyleSheet(
                error ? QStringLiteral("color:#d56b6b; font-weight:600;") : QString{});
        };
        const auto clear_controls = [&] {
            m_calculation_deductions_list->clear();
            m_edit_deductions_button->setEnabled(false);
            {
                QSignalBlocker blocker(m_factor_edit);
                m_factor_edit->clear();
            }
            {
                QSignalBlocker building_blocker(m_include_building_check);
                QSignalBlocker living_blocker(m_include_living_check);
                m_include_building_check->setChecked(false);
                m_include_living_check->setChecked(false);
            }
            m_factor_edit->setEnabled(false);
            m_include_building_check->setEnabled(false);
            m_include_living_check->setEnabled(false);
        };
        const auto set_calculation_error = [&](const QString& message) {
            clear_values();
            set_status_style(true);
            m_calculation_status->setText(QStringLiteral("Totals blocked: %1").arg(message));
            owner->statusBar()->showMessage(QStringLiteral("Area calculations blocked: %1").arg(message),
                                            8000);
        };
        clear_values();
        set_status_style(false);
        m_calculation_profile_context->setText(QStringLiteral("Select a closed boundary to edit its rule."));
        m_calculation_profile_version->setText(QStringLiteral("Profile —"));

        if (!selected.has_value() || !is_closed_boundary_entity(selected->type)) {
            clear_controls();
            m_calculation_status->setText(
                selected.has_value() ? QStringLiteral("Area calculations apply to closed boundaries.")
                                     : QStringLiteral("Select a closed boundary to calculate area totals."));
            return;
        }

        const auto editable = m_document->is_editable();
        const auto snapshot = m_document->snapshot();
        const auto property = propertyEntity();
        if (!property.has_value()) {
            clear_controls();
            set_calculation_error(QStringLiteral("no property entity is available"));
            return;
        }

        CalculationProfile persisted_profile;
        try {
            persisted_profile = read_calculation_profile(property->properties);
        } catch (const std::exception& error) {
            clear_controls();
            set_calculation_error(QStringLiteral("profile is invalid: %1")
                                      .arg(QString::fromUtf8(error.what())));
            return;
        }

        CalculationProfile display_profile = persisted_profile;
        display_profile.display_unit =
            m_metric_units ? AreaUnit::square_metre : AreaUnit::square_foot;
        const auto classification = read_string(selected->properties, "classification");
        const auto classification_name =
            classification.has_value() && !classification->empty()
                ? QString::fromStdString(*classification)
                : QStringLiteral("(unassigned)");
        std::optional<ClassificationRule> profile_rule;
        if (classification.has_value()) {
            const auto found = persisted_profile.classifications.find(*classification);
            if (found != persisted_profile.classifications.end()) {
                profile_rule = found->second;
            }
        }
        StoredFactor factor;
        try {
            factor = read_stored_factor(selected->properties);
        } catch (const std::exception& error) {
            clear_controls();
            set_calculation_error(QStringLiteral("factor is invalid: %1")
                                      .arg(QString::fromUtf8(error.what())));
            return;
        }
        const auto factor_value = static_cast<double>(factor.rational.numerator) /
                                  static_cast<double>(factor.rational.denominator);
        std::vector<std::string> selected_deduction_ids;
        try {
            selected_deduction_ids = read_deduction_ids(selected->properties);
        } catch (const std::exception& error) {
            clear_controls();
            set_calculation_error(QStringLiteral("deduction references are invalid: %1")
                                      .arg(QString::fromUtf8(error.what())));
            return;
        }
        m_calculation_deductions_list->clear();
        for (const auto& id : selected_deduction_ids) {
            auto* item = new QListWidgetItem(id_from(id), m_calculation_deductions_list);
            item->setData(Qt::UserRole, id_from(id));
        }
        if (selected_deduction_ids.empty()) {
            auto* item = new QListWidgetItem(QStringLiteral("None"), m_calculation_deductions_list);
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        }
        m_edit_deductions_button->setEnabled(editable);

        m_calculation_profile_version->setText(
            QStringLiteral("Profile version %1").arg(persisted_profile.version));
        m_calculation_profile_version->setToolTip(QString::fromStdString(persisted_profile.id));
        if (profile_rule.has_value()) {
            m_calculation_profile_context->setText(
                QStringLiteral("Rule for: %1").arg(classification_name));
        } else {
            m_calculation_profile_context->setText(
                QStringLiteral("Classification: %1 has no profile rule. Choose the totals below to assign it.")
                    .arg(classification_name));
        }
        {
            QSignalBlocker blocker(m_factor_edit);
            m_factor_edit->setText(factor.expression.isEmpty()
                                       ? QStringLiteral("%1/%2")
                                             .arg(factor.rational.numerator)
                                             .arg(factor.rational.denominator)
                                       : factor.expression);
        }
        {
            QSignalBlocker building_blocker(m_include_building_check);
            QSignalBlocker living_blocker(m_include_living_check);
            m_include_building_check->setChecked(
                profile_rule.has_value() && profile_rule->building_total);
            m_include_living_check->setChecked(
                profile_rule.has_value() && profile_rule->living_total);
        }
        m_factor_edit->setEnabled(editable);
        m_include_building_check->setEnabled(editable);
        m_include_living_check->setEnabled(editable);

        try {
            const auto& entities = snapshot.entities();
            // View filters are presentation-only and must never change area
            // totals.  The active design phase is semantic, however: a
            // demolished boundary cannot contribute to a selected total, and
            // a visible boundary with a hidden deduction is an explicit stale
            // relationship rather than a silently changed number.
            const auto phase_visible_ids = visible_project_entities_with_phase(
                snapshot, ProjectViewFilter{});
            if (!phase_visible_ids.contains(selected->id)) {
                throw std::invalid_argument(
                    "selected boundary is hidden by the active design phase");
            }
            std::set<std::string, std::less<>> referenced_deductions;
            for (const auto& [id, entity] : entities) {
                if (!is_closed_boundary_entity(entity.type)) continue;
                if (!phase_visible_ids.contains(id)) continue;
                for (const auto& deduction_id : read_deduction_ids(entity.properties)) {
                    if (deduction_id == id) {
                        throw std::invalid_argument("Boundary " + id + " cannot deduct itself");
                    }
                    if (!phase_visible_ids.contains(deduction_id)) {
                        throw std::invalid_argument("Boundary " + id +
                                                    " references deduction " + deduction_id +
                                                    " hidden by the active design phase");
                    }
                    referenced_deductions.insert(deduction_id);
                }
            }
            std::vector<MeasurementArea> areas;
            areas.reserve(entities.size());
            for (const auto& [id, entity] : entities) {
                if (!is_closed_boundary_entity(entity.type)) {
                    continue;
                }
                if (!phase_visible_ids.contains(id)) {
                    continue;
                }
                if (referenced_deductions.contains(id)) {
                    continue;
                }
                const auto floor_id = read_string(entity.properties, "floor_id");
                if (!floor_id.has_value() || floor_id->empty()) {
                    throw std::invalid_argument("Boundary " + id + " is missing floor_id");
                }
                const auto floor = entities.find(*floor_id);
                if (floor == entities.end() || floor->second.type != "floor") {
                    throw std::invalid_argument("Boundary " + id + " references an unknown floor");
                }
                auto building_id = read_string(entity.properties, "building_id");
                if (!building_id.has_value() || building_id->empty()) {
                    building_id = read_string(floor->second.properties, "building_id");
                }
                if (!building_id.has_value() || building_id->empty()) {
                    throw std::invalid_argument("Boundary " + id + " has no building reference");
                }
                const auto building = entities.find(*building_id);
                if (building == entities.end() || building->second.type != "building") {
                    throw std::invalid_argument("Boundary " + id + " references an unknown building");
                }
                const auto boundary = read_boundary(entity.properties);
                if (boundary.empty()) {
                    throw std::invalid_argument("Boundary " + id + " has no valid segments");
                }
                const auto entity_classification = read_string(entity.properties, "classification");
                if (!entity_classification.has_value() || entity_classification->empty()) {
                    throw std::invalid_argument("Boundary " + id +
                                                " has no classification rule; assign a classification");
                }
                if (!persisted_profile.classifications.contains(*entity_classification)) {
                    throw std::invalid_argument("Classification '" + *entity_classification +
                                                "' has no calculation profile rule; assign it before calculating totals");
                }
                const auto stored_factor = read_stored_factor(entity.properties);
                if (entity.properties.contains("calculation_scope") && !entity.properties.at("calculation_scope").is_string())
                    throw std::invalid_argument("Boundary " + id + " has an invalid calculation scope");
                const auto scope_name = read_string(entity.properties, "calculation_scope")
                    .value_or(*entity_classification == "survey" ? "site" : "building");
                if (scope_name != "site" && scope_name != "building")
                    throw std::invalid_argument("Boundary " + id + " has an unknown calculation scope");
                std::vector<AreaDeduction> deductions;
                for (const auto& deduction_id : read_deduction_ids(entity.properties)) {
                    const auto deduction = entities.find(deduction_id);
                    if (deduction == entities.end() ||
                        !is_closed_boundary_entity(deduction->second.type)) {
                        throw std::invalid_argument("Boundary " + id +
                                                    " references an unavailable deduction " + deduction_id);
                    }
                    const auto deduction_floor = read_string(deduction->second.properties, "floor_id");
                    if (!deduction_floor.has_value() || *deduction_floor != *floor_id) {
                        throw std::invalid_argument("Deduction " + deduction_id +
                                                    " must be on the same floor as boundary " + id);
                    }
                    const auto deduction_boundary = read_boundary(deduction->second.properties);
                    const auto deduction_diagnostics = validate_boundary(deduction_boundary);
                    if (!deduction_diagnostics.empty()) {
                        throw std::invalid_argument("Deduction " + deduction_id + " is invalid: " +
                                                    deduction_diagnostics.front().message);
                    }
                    if (!read_deduction_ids(deduction->second.properties).empty()) {
                        throw std::invalid_argument("Deduction " + deduction_id +
                                                    " cannot contain another deduction");
                    }
                    deductions.push_back({deduction_id, deduction_boundary});
                }
                areas.push_back(MeasurementArea{id,
                                                *building_id,
                                                *floor_id,
                                                *entity_classification,
                                                boundary,
                                                std::move(deductions),
                                                stored_factor.rational,
                                                scope_name == "site" ? AreaScope::site : AreaScope::building});
            }
            const auto report = calculate_areas(areas, display_profile);
            const auto selected_result = std::find_if(
                report.areas.begin(), report.areas.end(), [&](const AreaCalculation& result) {
                    return result.area_id == selected->id;
                });
            if (selected_result == report.areas.end()) {
                throw std::invalid_argument("selected boundary is not present in the calculation report");
            }
            const auto base = display_area(selected_result->base_square_metres, display_profile);
            const auto net = display_area(selected_result->net_square_metres, display_profile);
            const auto factored = selected_result->display;
            const auto suffix = area_unit_suffix(display_profile.display_unit);
            const auto format_area = [&](const DisplayArea& value) {
                return format_display_area(value);
            };
            m_calculation_base_value->setText(format_area(base));
            m_calculation_net_value->setText(format_area(net));
            m_calculation_factored_value->setText(format_area(factored));
            m_calculation_perimeter_value->setText(
                format_length(selected_result->perimeter_metres, m_metric_units));
            m_calculation_deductions_list->clear();
            if (selected_result->deductions.empty()) {
                auto* item = new QListWidgetItem(QStringLiteral("None"), m_calculation_deductions_list);
                item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            } else {
                for (const auto& trace : selected_result->deductions) {
                    const auto applied = format_area(
                        display_area(trace.applied_square_metres, display_profile));
                    const auto requested = format_area(
                        display_area(trace.requested_square_metres, display_profile));
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1  ·  applied %2 of %3")
                            .arg(id_from(trace.id), applied, requested),
                        m_calculation_deductions_list);
                    item->setData(Qt::UserRole, id_from(trace.id));
                }
            }
            m_calculation_rounding_value->setText(
                QStringLiteral("Rounded to %1 decimal places\nUnrounded: %2 %3\nDisplayed: %4\nDifference: %5 %3")
                    .arg(display_profile.decimal_places)
                    .arg(QString::number(factored.unrounded, 'f', 6))
                    .arg(suffix)
                    .arg(format_area(factored))
                    .arg(QString::number(factored.rounding_delta, 'f', 6)));
            m_calculation_building_total_value->setText(format_area(report.building.display));
            m_calculation_living_total_value->setText(format_area(report.living.display));
            m_calculation_status->setText(QStringLiteral("Calculated"));
            m_factor_edit->setToolTip(
                QStringLiteral("Exact factor %1 = %2")
                    .arg(factor.expression.isEmpty() ? QStringLiteral("exact") : factor.expression)
                    .arg(QString::number(factor_value, 'g', 8)));
        } catch (const std::exception& error) {
            set_calculation_error(QString::fromUtf8(error.what()));
        }
    }

    void refreshInspector() {
        const auto entity = selectedEntity();
        const auto editable = m_document->is_editable();
        const auto inspector_snapshot = m_document->snapshot();
        std::optional<QString> annotation_context;
        std::optional<LabelInstance> selected_annotation_label;
        std::optional<SymbolInstance> selected_annotation_symbol;
        if (!entity.has_value() && !m_selected_id.isEmpty()) {
            const auto wanted = m_selected_id.toStdString();
            for (const auto& [id, candidate] : inspector_snapshot.entities()) {
                (void)id;
                if (candidate.type != kAnnotationEntityType) continue;
                try {
                    const auto state = decode_annotation_entity(candidate);
                    const auto label = std::find_if(
                        state.labels.begin(), state.labels.end(),
                        [&](const auto& value) { return value.id == wanted; });
                    if (label != state.labels.end()) {
                        selected_annotation_label = *label;
                        annotation_context = QStringLiteral("Label\n%1")
                            .arg(QString::fromStdString(label->content));
                        break;
                    }
                    const auto symbol = std::find_if(
                        state.symbols.begin(), state.symbols.end(),
                        [&](const auto& value) { return value.id == wanted; });
                    if (symbol != state.symbols.end()) {
                        selected_annotation_symbol = *symbol;
                        annotation_context = QStringLiteral("Symbol\n%1")
                            .arg(QString::fromStdString(symbol->symbol_id));
                        break;
                    }
                } catch (const std::exception&) {
                    // Canvas validation reports malformed annotation state;
                    // inspector refresh remains safe while it is visible.
                }
            }
        }
        const bool wall = entity.has_value() && entity->type == "wall";
        m_constraint_button->setVisible(wall);
        m_constraint_button->setEnabled(wall && m_document->is_editable());
        const bool opening = entity.has_value() && entity->type == "opening";
        m_door_swing_button->setVisible(opening && entity->properties.value("opening_kind", std::string{}) == "door");
        m_door_swing_button->setEnabled(m_document->is_editable());
        const bool slab = entity.has_value() && entity->type == "slab";
        const bool reference_asset = entity.has_value() && entity->type == "reference_asset";
        const bool project_entity = entity.has_value() && entity->type == "property";
        const bool area_entity = entity.has_value() && is_closed_boundary_entity(entity->type);
        const bool building_object = entity && can_recognize_building_entity_type(entity->type);
        const bool material_object = wall || opening || slab || building_object ||
            (entity && (entity->type == "room" || entity->type == "room_boundary"));
        m_material_group->setVisible(material_object);
        m_material_group->setEnabled(material_object && m_document->is_editable());
        m_material_error->hide();
        m_material_context.reset();
        if (material_object) {
            m_material_context = captureModalContext();
            QSignalBlocker blocker(m_material_combo);
            m_material_combo->clear();
            m_material_combo->addItem("None", QString{});
            const auto assignment = entity->properties.value("material_assignment", json{});
            for (const auto& [catalog_id, catalog] : inspector_snapshot.entities()) {
                if (catalog.type != "assembly_model") continue;
                const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
                for (const auto& material : model.materials()) {
                    const json value{{"version", 1}, {"catalog_id", catalog_id}, {"material_id", material.id}};
                    m_material_combo->addItem(QString::fromStdString(material.name), QString::fromStdString(value.dump()));
                    const auto index = m_material_combo->count() - 1;
                    m_material_combo->setItemData(index,
                        QStringLiteral("%1 / %2").arg(QString::fromStdString(catalog_id),
                            QString::fromStdString(material.id)), Qt::ToolTipRole);
                    if (assignment.is_object() && assignment.value("catalog_id", std::string{}) == catalog_id &&
                        assignment.value("material_id", std::string{}) == material.id)
                        m_material_combo->setCurrentIndex(index);
                }
            }
        }
        const auto building_form = building_object
            ? read_string(entity->properties, "form") : std::optional<std::string>{};
        const bool sloped_roof_panel = building_object && entity->type == "roof" &&
            building_form.has_value() && *building_form == "sloped_roof_panel";
        const bool symmetric_roof_form = building_object && entity->type == "roof" &&
            building_form.has_value() && (*building_form == "gable_roof" || *building_form == "hip_roof");
        const bool editable_geometry = wall || opening || slab ||
            (entity && is_closed_boundary_entity(entity->type));
        m_edit_object_button->setVisible(building_object);
        m_edit_object_button->setEnabled(editable && building_object);
        m_roof_properties_group->setVisible(false);
        m_roof_properties_group->setEnabled(false);
        m_roof_edit_context.reset();
        m_building_edit_context.reset();
        const bool dimension_object = building_object &&
            (entity->type == "column" || entity->type == "beam" || entity->type == "stair");
        m_building_properties_group->setVisible(dimension_object);
        m_building_properties_group->setEnabled(dimension_object && editable);
        m_building_dimensions_error->hide();
        if (dimension_object) {
            m_building_edit_context = captureModalContext();
            m_building_properties_group->setTitle(QStringLiteral("%1")
                .arg(entity->type == "column" ? QStringLiteral("Column") :
                     entity->type == "beam" ? QStringLiteral("Beam") : QStringLiteral("Stair")));
            for (auto& dimension : m_building_dimensions) {
                const bool relevant = entity->type == "beam"
                    ? (dimension.suffix == "Width" || dimension.suffix == "Depth")
                    : entity->type == "stair"
                    ? (dimension.suffix == "Width" || dimension.suffix == "TotalRise" ||
                       dimension.suffix == "Going" || dimension.suffix == "RiserCount")
                    : building_form == std::optional<std::string>("circular_column")
                    ? (dimension.suffix == "Radius" || dimension.suffix == "Height")
                    : (dimension.suffix == "Width" || dimension.suffix == "Depth" || dimension.suffix == "Height");
                dimension.label->setVisible(relevant);
                dimension.edit->setVisible(relevant);
                if (!relevant) continue;
                QSignalBlocker blocker(dimension.edit);
                const auto value = read_number(entity->properties, dimension.property, 0.0);
                dimension.original_text = dimension.suffix == QStringLiteral("RiserCount")
                    ? QString::number(value, 'f', 0) : format_length(value, m_metric_units);
                dimension.edit->setText(dimension.original_text);
            }
            // Use the canonical dialog's formatting and field availability. Unchanged
            // inspector text never replaces its decoded values or quantity receipts.
            BuildingObjectDialog placement_dialog(*entity, m_metric_units, owner);
            for (auto& placement : m_building_placement) {
                auto* field = placement_dialog.findChild<QLineEdit*>(QStringLiteral("buildingObject") + placement.suffix);
                placement.label->setVisible(field != nullptr);
                placement.edit->setVisible(field != nullptr);
                if (!field) continue;
                if (placement.suffix == "OrientationDegrees") {
                    const auto label = entity->type == "column" ? QStringLiteral("Rotation (degrees)")
                                                               : QStringLiteral("Orientation (degrees)");
                    placement.label->setText(label);
                    placement.edit->setAccessibleName(label);
                }
                QSignalBlocker blocker(placement.edit);
                placement.original_text = field->text();
                placement.edit->setText(placement.original_text);
            }
        }
        m_delete_annotation_button->setVisible(annotation_context.has_value());
        m_delete_annotation_button->setEnabled(editable && annotation_context.has_value());
        m_annotation_group->setVisible(annotation_context.has_value());
        m_annotation_group->setEnabled(editable && annotation_context.has_value());
        m_project_details_group->setVisible(project_entity);
        m_project_details_group->setEnabled(editable && project_entity);
        m_area_attributes_group->setVisible(area_entity);
        m_area_attributes_group->setEnabled(editable && area_entity);
        m_reference_group->setVisible(reference_asset);
        m_reference_group->setEnabled(editable && reference_asset);
        if (reference_asset) {
            m_project_details_group->setVisible(false);
            m_area_attributes_group->setVisible(false);
            m_inspector_context->setText(
                QStringLiteral("Reference image\n%1")
                    .arg(QString::fromStdString(read_string(entity->properties, "source_path")
                                                    .value_or("embedded raster"))));
            const auto position = read_point(
                entity->properties.contains("position_m")
                    ? entity->properties.at("position_m") : json{});
            const auto set_value = [](QLineEdit* field, double value) {
                QSignalBlocker blocker(field);
                field->setText(QString::number(value, 'g', 12));
            };
            set_value(m_reference_x_edit, position ? position->x : 0.0);
            set_value(m_reference_y_edit, position ? position->y : 0.0);
            set_value(m_reference_calibration_edit,
                      read_number(entity->properties, "metres_per_source_unit", 0.01));
            set_value(m_reference_scale_edit, read_number(entity->properties, "scale", 1.0));
            set_value(m_reference_rotation_edit,
                      read_number(entity->properties, "rotation_degrees", 0.0));
            set_value(m_reference_intensity_edit,
                      read_number(entity->properties, "intensity", 1.0));
            const auto boolean_property = [&](std::string_view key, bool fallback) {
                const auto value = entity->properties.value(std::string(key), json(fallback));
                return value.is_boolean() ? value.get<bool>() : fallback;
            };
            {
                QSignalBlocker blocker(m_reference_flip_horizontal_check);
                m_reference_flip_horizontal_check->setChecked(
                    boolean_property("flip_horizontal", false));
            }
            {
                QSignalBlocker blocker(m_reference_flip_vertical_check);
                m_reference_flip_vertical_check->setChecked(boolean_property("flip_vertical", false));
            }
            {
                QSignalBlocker blocker(m_reference_visible_check);
                m_reference_visible_check->setChecked(boolean_property("visible", true));
            }
            m_geometry_form->setRowVisible(m_length_edit, false);
            m_geometry_form->setRowVisible(m_classification_combo, false);
            m_geometry_form->setRowVisible(m_height_edit, false);
            m_geometry_form->setRowVisible(m_thickness_edit, false);
            m_length_edit->setEnabled(false);
            m_height_edit->setEnabled(false);
            m_thickness_edit->setEnabled(false);
            m_classification_combo->setEnabled(false);
            m_read_only_label->setText(editable ? QString{} : QStringLiteral("Read-only: %1")
                                                                      .arg(QString::fromStdString(m_document->read_only_reason())));
            refreshCalculationInspector(std::nullopt);
            return;
        }
        if (annotation_context.has_value()) {
            m_project_details_group->setVisible(false);
            m_area_attributes_group->setVisible(false);
            m_inspector_context->setText(*annotation_context);
            {
                QSignalBlocker blocker(m_annotation_content_edit);
                m_annotation_content_edit->setText(selected_annotation_label.has_value()
                                                       ? QString::fromStdString(selected_annotation_label->content)
                                                       : QString{});
            }
            {
                QSignalBlocker blocker(m_annotation_x_edit);
                const auto position = selected_annotation_label.has_value()
                                          ? selected_annotation_label->placement.position
                                          : selected_annotation_symbol->placement.position;
                m_annotation_x_edit->setText(QString::number(position.x, 'g', 12));
            }
            {
                QSignalBlocker blocker(m_annotation_y_edit);
                const auto position = selected_annotation_label.has_value()
                                          ? selected_annotation_label->placement.position
                                          : selected_annotation_symbol->placement.position;
                m_annotation_y_edit->setText(QString::number(position.y, 'g', 12));
            }
            {
                QSignalBlocker blocker(m_annotation_rotation_edit);
                const auto radians = selected_annotation_label.has_value()
                                         ? selected_annotation_label->placement.rotation_radians
                                         : selected_annotation_symbol->placement.rotation_radians;
                m_annotation_rotation_edit->setText(
                    QString::number(radians * 180.0 / std::numbers::pi, 'g', 12));
            }
            {
                QSignalBlocker blocker(m_annotation_scale_edit);
                const auto instance_scale = selected_annotation_label.has_value()
                                                ? selected_annotation_label->placement.scale
                                                : selected_annotation_symbol->placement.scale;
                m_annotation_scale_edit->setText(QString::number(instance_scale, 'g', 12));
            }
            {
                QSignalBlocker blocker(m_annotation_visible_check);
                m_annotation_visible_check->setChecked(selected_annotation_label.has_value()
                                                           ? selected_annotation_label->visible
                                                           : selected_annotation_symbol->visible);
            }
            m_annotation_content_edit->setEnabled(editable && selected_annotation_label.has_value());
            m_geometry_form->setRowVisible(m_length_edit, false);
            m_geometry_form->setRowVisible(m_classification_combo, false);
            m_geometry_form->setRowVisible(m_height_edit, false);
            m_geometry_form->setRowVisible(m_thickness_edit, false);
            m_length_edit->setEnabled(false);
            m_height_edit->setEnabled(false);
            m_thickness_edit->setEnabled(false);
            m_classification_combo->setEnabled(false);
            m_read_only_label->setText(editable ? QString{} : QStringLiteral("Read-only: %1")
                                                                      .arg(QString::fromStdString(m_document->read_only_reason())));
            refreshCalculationInspector(std::nullopt);
            return;
        }
        m_reference_group->setVisible(false);
        if (project_entity) {
            const auto subject = entity->properties.value("subject", json::object());
            const auto read_subject_string = [&](std::string_view key, std::string fallback = {}) {
                if (subject.is_object()) {
                    if (const auto value = read_string(subject, key)) return *value;
                }
                return read_string(entity->properties, key).value_or(std::move(fallback));
            };
            {
                QSignalBlocker blocker(m_project_subject_name_edit);
                m_project_subject_name_edit->setText(QString::fromStdString(
                    read_subject_string("name", read_string(entity->properties, "name").value_or("Untitled property"))));
            }
            {
                QSignalBlocker blocker(m_project_subject_address_edit);
                m_project_subject_address_edit->setText(QString::fromStdString(
                    read_subject_string("address")));
            }
            {
                QSignalBlocker blocker(m_project_subject_reference_edit);
                m_project_subject_reference_edit->setText(QString::fromStdString(
                    read_subject_string("reference")));
            }
            {
                QSignalBlocker blocker(m_project_subject_attributes_edit);
                const auto attributes = subject.is_object()
                    ? subject.value("attributes", json::object()) : json::object();
                m_project_subject_attributes_edit->setPlainText(
                    attributes.is_object() ? QString::fromStdString(attributes.dump(2))
                                           : QStringLiteral("{}"));
            }
        } else {
            QSignalBlocker name_blocker(m_project_subject_name_edit);
            QSignalBlocker address_blocker(m_project_subject_address_edit);
            QSignalBlocker reference_blocker(m_project_subject_reference_edit);
            QSignalBlocker attributes_blocker(m_project_subject_attributes_edit);
            m_project_subject_name_edit->clear();
            m_project_subject_address_edit->clear();
            m_project_subject_reference_edit->clear();
            m_project_subject_attributes_edit->clear();
        }
        if (area_entity) {
            const auto attributes = entity->properties.value("area_attributes", json::object());
            QSignalBlocker blocker(m_area_attributes_edit);
            m_area_attributes_edit->setPlainText(
                attributes.is_object() ? QString::fromStdString(attributes.dump(2))
                                       : QStringLiteral("{}"));
        } else {
            QSignalBlocker blocker(m_area_attributes_edit);
            m_area_attributes_edit->clear();
        }
        m_geometry_form->setRowVisible(m_length_edit, editable_geometry);
        m_geometry_form->setRowVisible(m_classification_combo, editable_geometry);
        m_geometry_form->setRowVisible(m_height_edit, wall || opening);
        m_geometry_form->setRowVisible(m_thickness_edit, wall || slab);
        if (auto* label = qobject_cast<QLabel*>(m_geometry_form->labelForField(m_length_edit))) {
            label->setText(opening ? QStringLiteral("Width") :
                entity.has_value() && (slab || is_closed_boundary_entity(entity->type))
                    ? QStringLiteral("Perimeter") : QStringLiteral("Length"));
        }
        if (!entity.has_value()) {
            m_inspector_context->setText(QStringLiteral("No selection\nUse Select or choose an object in the navigator."));
            m_length_edit->clear();
            m_height_edit->clear();
            m_thickness_edit->clear();
            {
                QSignalBlocker blocker(m_classification_combo);
                m_classification_combo->setCurrentIndex(-1);
            }
            m_length_edit->setEnabled(false);
            m_height_edit->setEnabled(false);
            m_thickness_edit->setEnabled(false);
            m_classification_combo->setEnabled(false);
            m_read_only_label->setText(editable ? QString{} : QStringLiteral("Read-only: %1")
                                                                      .arg(QString::fromStdString(m_document->read_only_reason())));
            refreshCalculationInspector(entity);
            return;
        }
        const auto type = QString::fromStdString(entity->type);
        Boundary boundary;
        if (entity->type == "wall") {
            if (const auto baseline = read_required_segment(entity->properties, "baseline")) {
                boundary.push_back(*baseline);
            }
        } else if (entity->type == "slab") {
            if (const auto canonical = read_required_boundary(entity->properties, "boundary")) {
                boundary = *canonical;
            }
        } else {
            boundary = read_boundary(entity->properties);
        }
        double length = 0.0;
        if (is_closed_boundary_entity(entity->type) || entity->type == "slab") {
            length = perimeter(boundary);
        } else if (entity->type == "opening") {
            length = read_number(entity->properties, "width_m", 0.0);
        } else if (!boundary.empty()) {
            length = segment_length(boundary.front());
        }
        QString context = type;
        context.replace(QLatin1Char('_'), QLatin1Char(' '));
        if (!context.isEmpty()) context[0] = context[0].toUpper();
        if (entity->type == "opening") {
            const auto wall_id = read_string(entity->properties, "wall_id");
            const auto kind = read_string(entity->properties, "opening_kind");
            const auto kind_label = kind.has_value() && !kind->empty()
                                        ? QString::fromStdString(*kind).toLower()
                                        : QStringLiteral("door/window");
            context = QStringLiteral("%1 opening\nHosted by wall %2")
                          .arg(kind_label.left(1).toUpper() + kind_label.mid(1),
                               wall_id.has_value() ? QString::fromStdString(*wall_id)
                                                   : QStringLiteral("unknown"));
        } else if (entity->type == "slab") {
            context = QStringLiteral("Slab\nClosed profile");
        } else if (entity->type == "roof") {
            const auto roof_label = sloped_roof_panel
                ? (read_number(entity->properties, "rise_m", 0.0) == 0.0
                       ? QStringLiteral("Flat panel") : QStringLiteral("Sloped panel"))
                : building_form == std::optional<std::string>("hip_roof")
                    ? QStringLiteral("Hip roof") : QStringLiteral("Gable roof");
            context = QStringLiteral("Roof\n%1").arg(roof_label);
        }
        m_inspector_context->setText(context);
        if (sloped_roof_panel || symmetric_roof_form) {
            m_roof_edit_context = captureModalContext();
            m_roof_properties_group->setVisible(true);
            m_roof_properties_group->setEnabled(editable);
            m_roof_run_label->setText(symmetric_roof_form ? QStringLiteral("Length") : QStringLiteral("Run"));
            m_roof_run_edit->setAccessibleName(m_roof_run_label->text());
            m_roof_run_edit->setToolTip(symmetric_roof_form ? QStringLiteral("Length along the ridge")
                                                 : QStringLiteral("Horizontal run along the slope"));
            m_roof_rise_edit->setToolTip(symmetric_roof_form ? QStringLiteral("Positive ridge height above the eaves")
                                                  : QStringLiteral("Rise; enter zero for a flat roof"));
            const auto set_roof_value = [&](QLineEdit* field, QString& original_text,
                                            double value) {
                QSignalBlocker blocker(field);
                field->setText(format_length(value, m_metric_units));
                field->setModified(false);
                original_text = field->text();
            };
            set_roof_value(m_roof_run_edit, m_roof_run_original_text,
                           read_number(entity->properties, symmetric_roof_form ? "length_m" : "run_m", 0.0));
            set_roof_value(m_roof_span_edit, m_roof_span_original_text,
                           read_number(entity->properties, "span_m", 0.0));
            set_roof_value(m_roof_rise_edit, m_roof_rise_original_text,
                           read_number(entity->properties, "rise_m", 0.0));
            set_roof_value(m_roof_overhang_edit, m_roof_overhang_original_text,
                           read_number(entity->properties, "overhang_m", 0.0));
            set_roof_value(m_roof_thickness_edit, m_roof_thickness_original_text,
                           read_number(entity->properties, "thickness_m", 0.0));
            const auto pitch = read_number(entity->properties, "pitch_rad", 0.0);
            QSignalBlocker pitch_blocker(m_roof_pitch_value);
            m_roof_pitch_value->setText(QStringLiteral("%1°")
                                            .arg(pitch * 180.0 / std::numbers::pi, 0, 'f', 3));
            m_roof_preview_error->clear();
            m_roof_preview_error->hide();
        }
        {
            QSignalBlocker blocker(m_length_edit);
            m_length_edit->setText(format_length(length, m_metric_units));
        }
        {
            QSignalBlocker blocker(m_classification_combo);
            const auto key = entity->type == "opening" ? "opening_kind" : "classification";
            const auto value = read_string(entity->properties, key);
            const auto classification = value.has_value()
                                            ? QString::fromStdString(*value)
                                            : entity->type == "slab" ? QStringLiteral("slab")
                                                                      : QStringLiteral("measurement");
            if (m_classification_combo->findText(classification) < 0) {
                m_classification_combo->addItem(classification);
            }
            m_classification_combo->setCurrentText(classification);
        }
        {
            QSignalBlocker blocker(m_height_edit);
            m_height_edit->setText(format_length(
                read_number(entity->properties, "height_m", 0.0), m_metric_units));
        }
        {
            QSignalBlocker blocker(m_thickness_edit);
            m_thickness_edit->setText(format_length(read_number(entity->properties, "thickness_m", 0.0),
                                                    m_metric_units));
        }
        m_length_edit->setEnabled(editable && (wall || opening));
        m_height_edit->setEnabled(editable && (wall || opening));
        m_thickness_edit->setEnabled(editable && (wall || slab));
        m_classification_combo->setEnabled(editable &&
                                           (wall || opening || slab ||
                                            is_closed_boundary_entity(entity->type)));
        m_read_only_label->setText(editable ? QString{}
                                             : QStringLiteral("Read-only: %1")
                                                   .arg(QString::fromStdString(m_document->read_only_reason())));
        refreshCalculationInspector(entity);
    }

    void refreshActions() {
        m_undo_action->setEnabled(m_boundary_session ? m_boundary_session->can_undo() :
            m_recovery_ledger.empty() ? m_document->can_undo() : m_project_workspace->can_undo());
        m_redo_action->setEnabled(m_boundary_session ? m_boundary_session->can_redo() :
            m_recovery_ledger.empty() ? m_document->can_redo() : m_project_workspace->can_redo());
        m_save_action->setEnabled(m_document->is_editable() && projectDirty());
        m_save_as_action->setEnabled(m_document->is_editable());
        m_object_button->setEnabled(m_document->is_editable());
    }

    void refreshTitle() {
        auto title = m_file_path.empty()
            ? QStringLiteral("Untitled project")
            : QString::fromStdWString(m_file_path.filename().wstring());
        if (projectDirty() || hasBoundaryDraftChanges()) {
            title += QStringLiteral(" *");
        }
        owner->setWindowTitle(title);
        if (m_measurement_action) {
            QSignalBlocker first(m_measurement_action);
            QSignalBlocker second(m_architectural_action);
            m_measurement_action->setChecked(m_workspace == Workspace::measurement);
            m_architectural_action->setChecked(m_workspace == Workspace::architectural);
        }
    }

    void refreshCursorLabel(Vec2 point) {
        owner->statusBar()->showMessage(
            QStringLiteral("Cursor  X %1  Y %2  •  %3  •  %4")
                .arg(format_length(point.x, m_metric_units), format_length(point.y, m_metric_units),
                     workspace_name(m_workspace), tool_name(m_tool)));
    }

    void onNavigatorClicked(QTreeWidgetItem* item, int column) {
        if (item == nullptr) return;
        if (column == 0 && m_navigator->checkboxInteraction()) {
            return;
        }
        const auto value = item->data(0, Qt::UserRole);
        if (!value.isValid()) return;
        const auto id = value.toString();
        const auto document = m_document;
        // Selection can rebuild the tree. Defer it until QTreeWidget has
        // finished delivering the click, and bind the callback to the
        // document that supplied this item.
        QTimer::singleShot(0, owner, [this, id, document] {
            if (m_document == document && !id.isEmpty() && id != m_selected_id &&
                has_entity(*m_document, id)) {
                selectEntity(id);
            }
        });
    }

    bool hasBoundaryDraftChanges() const {
        if (!m_boundary_session) return false;
        const auto state = m_boundary_session->view();
        return state.active_chain.has_value() || !state.accepted_chains.empty();
    }

    bool confirmDiscardBoundaryDraft() {
        if (!hasBoundaryDraftChanges()) return true;
        const auto context = captureModalContext();
        const auto state = m_boundary_session->view();
        const auto choice = QMessageBox::warning(owner, QStringLiteral("Unfinished boundary"),
            QStringLiteral("This boundary has not been added to the project. Discard the unfinished drawing? Choose Cancel to keep drawing."),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice != QMessageBox::Discard) return false;
        if (!modalContextUnchanged(context)) return false;
        if (!m_boundary_session || !(m_boundary_session->view() == state)) {
            setError(QStringLiteral("The unfinished boundary changed while the message was open. Review it before discarding."));
            return false;
        }
        return true;
    }

    std::optional<QString> chooseBoundaryClassification(const DrawingContext& context) {
        const auto snapshot = m_document->snapshot();
        const auto property = snapshot.entities().find(context.property_id);
        if (property == snapshot.entities().end()) throw std::invalid_argument("drawing property is missing");
        const auto profile = read_calculation_profile(property->second.properties);
        QStringList choices;
        for (const auto& [name, rule] : profile.classifications) {
            (void)rule;
            choices.push_back(QString::fromStdString(name));
        }
        QInputDialog dialog(owner);
        dialog.setObjectName(QStringLiteral("boundaryClassificationDialog"));
        dialog.setWindowTitle(QStringLiteral("Define area"));
        dialog.setLabelText(QStringLiteral("Area classification (controls area totals):"));
        dialog.setComboBoxItems(choices);
        dialog.setComboBoxEditable(true);
        dialog.setTextValue(m_last_boundary_classification);
        if (dialog.exec() != QDialog::Accepted) return std::nullopt;
        const auto selected = dialog.textValue().trimmed();
        if (selected.isEmpty()) throw std::invalid_argument("choose a nonempty area classification");
        return selected;
    }

    void refreshBoundaryPreview() {
        if (!m_boundary_session) {
            m_measurementCanvas->setBoundaryDraftPreview(std::nullopt);
            m_architecturalCanvas->setBoundaryDraftPreview(std::nullopt);
            return;
        }
        const auto state = m_boundary_session->view();
        BoundaryDraftPreview preview;
        const auto append_chain = [&](const auto& edges, const auto& dimensions) {
            for (const auto& edge : edges) preview.segments.push_back(edge.segment);
            for (const auto& dimension : dimensions) {
                const auto edge = std::find_if(edges.begin(), edges.end(), [&](const auto& item) {
                    return item.segment_id == dimension.segment_id;
                });
                if (edge != edges.end()) preview.labels.push_back({dimension.text_position,
                    format_length(segment_length(edge->segment), m_metric_units)});
            }
        };
        for (const auto& chain : state.accepted_chains)
            append_chain(chain.boundary.segments, chain.dimensions);
        if (state.active_chain) {
            const auto& chain = *state.active_chain;
            append_chain(chain.segments, chain.dimensions);
            preview.anchor = chain.anchor;
            preview.pen_position = chain.segments.empty() ? chain.anchor : chain.segments.back().segment.end;
            if (state.phase == BoundaryAuthoringPhase::drawing && state.pen_state == BoundaryPenState::down &&
                state.pointer && (state.pointer->x != preview.pen_position->x || state.pointer->y != preview.pen_position->y))
                preview.rubber_band = Segment{*preview.pen_position, *state.pointer, 0};
            if (state.phase == BoundaryAuthoringPhase::awaiting_dimension && state.pointer && state.pending_dimension) {
                const auto edge = std::find_if(chain.segments.begin(), chain.segments.end(), [&](const auto& item) {
                    return item.segment_id == state.pending_dimension->segment_id;
                });
                if (edge != chain.segments.end()) preview.labels.push_back({*state.pointer,
                    format_length(segment_length(edge->segment), m_metric_units)});
            }
        }
        const auto mode = state.mode == BoundaryAuthoringMode::draw_first
            ? QStringLiteral("Draw First") : QStringLiteral("Define First");
        switch (state.phase) {
        case BoundaryAuthoringPhase::awaiting_classification:
            preview.instruction = mode + QStringLiteral("  •  Choose an area classification"); break;
        case BoundaryAuthoringPhase::awaiting_anchor:
            preview.instruction = mode + QStringLiteral("  •  Click to anchor the drawing  •  Esc cancels"); break;
        case BoundaryAuthoringPhase::awaiting_dimension:
            preview.instruction = mode + QStringLiteral("  •  Click to place this edge's dimension  •  Ctrl+Z undoes"); break;
        case BoundaryAuthoringPhase::drawing:
            preview.instruction = mode + QStringLiteral("  •  Click an endpoint  •  D precise line/curve  •  Enter closes"); break;
        case BoundaryAuthoringPhase::completed:
            preview.instruction = mode + QStringLiteral("  •  Enter defines and adds the area  •  Ctrl+Z revises it"); break;
        case BoundaryAuthoringPhase::cancelled: break;
        }
        m_measurementCanvas->setBoundaryDraftPreview(preview);
        m_architecturalCanvas->setBoundaryDraftPreview(std::move(preview));
    }

    void boundaryDraftChanged() {
        refreshBoundaryPreview();
        refreshActions();
        refreshTitle();
    }

    void onCanvasPoint(Vec2 point) {
        if (m_tool == CanvasTool::boundary) {
            if (!m_boundary_session) return;
            try {
                m_boundary_session->set_pointer(point);
                switch (m_boundary_session->phase()) {
                case BoundaryAuthoringPhase::awaiting_anchor:
                    (void)m_boundary_session->anchor(point); break;
                case BoundaryAuthoringPhase::drawing:
                    (void)m_boundary_session->add_line_to(point); break;
                case BoundaryAuthoringPhase::awaiting_dimension: {
                    (void)m_boundary_session->place_manual_dimension(point);
                    const auto chain = m_boundary_session->active_chain();
                    if (chain && !chain->segments.empty() &&
                        chain->segments.back().segment.end.x == chain->anchor.x &&
                        chain->segments.back().segment.end.y == chain->anchor.y) {
                        boundaryDraftChanged();
                        finishTool();
                        return;
                    }
                    break;
                }
                default:
                    setError(QStringLiteral("Press Enter to define the closed area, or undo to revise it."));
                    return;
                }
                clearError();
                boundaryDraftChanged();
            } catch (const std::exception& error) {
                setError(QStringLiteral("Boundary input: %1").arg(QString::fromUtf8(error.what())));
            }
            return;
        }
        if (m_tool == CanvasTool::wall) {
            if (!m_pending_wall_start.has_value()) {
                m_pending_wall_start = point;
                m_measurementCanvas->setWallPreview(std::make_pair(point, point));
                m_architecturalCanvas->setWallPreview(std::make_pair(point, point));
                owner->statusBar()->showMessage(QStringLiteral("Wall start recorded  •  click the end point"));
                return;
            }
            const auto id = createStraightWall(*m_pending_wall_start, point,
                                               QStringLiteral("interior"));
            if (!id.isEmpty()) {
                clearPreview();
                setTool(CanvasTool::select);
            }
        }
    }

    void finishTool() {
        if (m_tool != CanvasTool::boundary || !m_boundary_session) return;
        try {
            if (m_boundary_document != m_document || !m_boundary_source || !m_boundary_context)
                throw std::invalid_argument("the drawing no longer belongs to the open project");
            auto candidate = *m_boundary_session;
            if (candidate.phase() == BoundaryAuthoringPhase::awaiting_dimension)
                throw std::invalid_argument("place the pending edge dimension before closing the area");
            if (const auto chain = candidate.active_chain()) {
                if (chain->segments.empty()) throw std::invalid_argument("draw an edge before closing the area");
                const auto end = chain->segments.back().segment.end;
                if (end.x != chain->anchor.x || end.y != chain->anchor.y) {
                    (void)candidate.add_closing_segment();
                    Boundary geometry;
                    const auto closed_draft = candidate.active_chain();
                    for (const auto& edge : closed_draft->segments) geometry.push_back(edge.segment);
                    const auto diagnostics = validate_boundary(geometry);
                    if (!diagnostics.empty()) throw std::invalid_argument(diagnostics.front().message);
                    if (candidate.phase() == BoundaryAuthoringPhase::awaiting_dimension) {
                        m_boundary_session = std::move(candidate);
                        clearError(); boundaryDraftChanged();
                        return;
                    }
                }
                (void)candidate.close_chain();
            }
            if (candidate.accepted_chains().empty()) throw std::invalid_argument("draw a closed area first");
            m_boundary_session = std::move(candidate);
            boundaryDraftChanged();
            if (!m_boundary_session->accepted_chains().back().classified) {
                const auto modal_context = captureModalContext();
                const auto original_state = m_boundary_session->view();
                const auto classification = chooseBoundaryClassification(*m_boundary_context);
                if (!classification) return;
                if (!modalContextUnchanged(modal_context) || !m_boundary_session ||
                    !(m_boundary_session->view() == original_state)) return;
                m_boundary_session->classify_last_chain(classification->toStdString());
                m_last_boundary_classification = *classification;
                boundaryDraftChanged();
            }
            BoundaryCommitIntent intent{m_boundary_session->options(), m_boundary_session->accepted_chains(),
                *m_boundary_context, "Draw and define measured area"};
            if (document_snapshot_digest(*m_boundary_source) != document_snapshot_digest(m_document->snapshot()))
                throw std::invalid_argument("the document changed after boundary drawing started");
            const auto preview = preview_boundary_commit(authoringSnapshot(), intent);
            if (!preview.accepted()) throw std::invalid_argument(preview.diagnostics().empty()
                ? "boundary commit was rejected" : preview.diagnostics().front());
            QString committed_id;
            if (m_redefine_boundary_id.has_value()) {
                const auto created_id = preview.created_boundary_ids().front();
                const auto created = preview.candidate_entities().find(created_id);
                if (created == preview.candidate_entities().end()) {
                    throw std::invalid_argument("the replacement boundary was not present in the preview");
                }
                const auto replacement = decode_identified_boundary_entity(created->second);
                const auto source_id = *m_redefine_boundary_id;
                const auto classification = QString::fromStdString(
                    read_string(created->second.properties, "classification").value_or(""));
                if (!redefineSelectedBoundary(boundary_geometry(replacement), classification)) {
                    throw std::invalid_argument(lastError().toStdString());
                }
                // Redefinition updates the existing entity in place. Keep the
                // source selection instead of selecting the temporary preview
                // identity used to validate the replacement.
                committed_id = source_id;
                m_redefine_boundary_id.reset();
            } else if (m_recovery_ledger.empty()) {
                (void)apply_boundary_commit(*m_document, preview);
            } else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_boundary_commit(preview);
                commitWorkspaceEdit(edit);
            }
            if (committed_id.isEmpty()) {
                committed_id = QString::fromStdString(preview.created_boundary_ids().front());
            }
            m_selected_id = committed_id;
            clearPreview();
            m_tool = CanvasTool::select;
            syncToolControls();
            clearError();
            refresh();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Finish boundary: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void cancelTool() {
        clearPreview();
        m_pending_wall_start.reset();
        m_redefine_boundary_id.reset();
        setTool(CanvasTool::select);
    }

    void preciseBoundaryInput() {
        if (m_tool != CanvasTool::boundary || !m_boundary_session ||
            m_boundary_session->phase() != BoundaryAuthoringPhase::drawing) {
            setError(QStringLiteral("Anchor the drawing and place any pending dimension before entering a segment."));
            return;
        }
        const auto context = captureModalContext();
        const auto state = m_boundary_session->view();
        try {
            BoundaryInputDialog dialog(*m_boundary_session, m_metric_units, owner);
            if (dialog.exec() != QDialog::Accepted) return;
            if (!modalContextUnchanged(context) || !m_boundary_session ||
                !(m_boundary_session->view() == state)) return;
            const auto candidate = dialog.candidate();
            if (!candidate) return;
            m_boundary_session = *candidate;
            clearError();
            boundaryDraftChanged();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Precise segment: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void setTool(CanvasTool tool) {
        if (tool == CanvasTool::boundary) {
            if (m_boundary_session && m_tool == tool &&
                m_boundary_session->mode() == BoundaryAuthoringMode::draw_first) return;
            if (!beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, {})) syncToolControls();
            return;
        }
        if (!confirmDiscardBoundaryDraft()) { syncToolControls(); return; }
        m_tool = tool;
        clearPreview();
        syncToolControls();
        boundaryDraftChanged();
        owner->statusBar()->showMessage(tool_name(tool));
    }

    void syncToolControls() {
        m_measurementCanvas->setTool(m_tool);
        m_architecturalCanvas->setTool(m_tool);
        {
            QSignalBlocker first(m_select_button);
            QSignalBlocker second(m_boundary_button);
            QSignalBlocker third(m_wall_button);
            QSignalBlocker fourth(m_define_boundary_button);
            m_select_button->setChecked(m_tool == CanvasTool::select);
            m_boundary_button->setChecked(m_tool == CanvasTool::boundary && m_boundary_session &&
                m_boundary_session->mode() == BoundaryAuthoringMode::draw_first);
            m_define_boundary_button->setChecked(m_tool == CanvasTool::boundary && m_boundary_session &&
                m_boundary_session->mode() == BoundaryAuthoringMode::define_first);
            m_wall_button->setChecked(m_tool == CanvasTool::wall);
        }
    }

    void setGrid(bool enabled) {
        m_grid_enabled = enabled;
        m_measurementCanvas->setGridEnabled(enabled);
        m_architecturalCanvas->setGridEnabled(enabled);
    }

    void setSnap(bool enabled) {
        m_snap_enabled = enabled;
        m_measurementCanvas->setSnapEnabled(enabled);
        m_architecturalCanvas->setSnapEnabled(enabled);
    }

    void toggleGrid() { m_grid_button->setChecked(!m_grid_button->isChecked()); }
    void toggleSnap() { m_snap_button->setChecked(!m_snap_button->isChecked()); }
    void setOverviewMap(bool enabled) {
        m_overview_map_enabled = enabled;
        m_measurementCanvas->setOverviewMapEnabled(enabled);
        m_architecturalCanvas->setOverviewMapEnabled(enabled);
    }
    void toggleOverviewMap() { setOverviewMap(!m_overview_map_enabled); }

    void clearPreview() {
        m_measurementCanvas->clearPreview();
        m_architecturalCanvas->clearPreview();
        m_boundary_session.reset();
        m_boundary_source.reset();
        m_boundary_context.reset();
        m_boundary_document.reset();
        m_pending_wall_start.reset();
        m_redefine_boundary_id.reset();
    }

    void openFromDialog() {
        const auto selected = QFileDialog::getOpenFileName(
            owner, QStringLiteral("Open project"), {}, QStringLiteral("Property Studio project (*.bldproj)"));
        if (!selected.isEmpty()) {
            openProject(selected);
        }
    }

    bool recoverFromDialog(bool startup_only = false, bool inform_if_empty = true) {
        const auto directory = filesystem_path(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)) / "recovery";
        const auto discovered = discover_recovery_copies(std::nullopt, directory);
        if (!discovered.directory_diagnostic.empty()) {
            setError(QStringLiteral("Recovery discovery failed: %1")
                         .arg(QString::fromUtf8(discovered.directory_diagnostic.c_str())));
            return false;
        }
        QStringList choices;
        std::vector<QString> paths;
        for (const auto& candidate : discovered.candidates) {
            if (!candidate.loadable || candidate.duplicate_archive_id ||
                (startup_only && !recovery_candidate_has_unsaved_work(candidate))) continue;
            auto label = QString::fromStdWString(candidate.path.wstring());
            if (candidate.metadata) {
                label += QStringLiteral("  [session %1]")
                             .arg(QString::fromStdString(candidate.metadata->archive_id));
            }
            choices.push_back(label);
            paths.push_back(QString::fromStdWString(candidate.path.wstring()));
        }
        if (choices.isEmpty()) {
            if (inform_if_empty) {
                QMessageBox::information(owner, QStringLiteral("Recover project"),
                                         QStringLiteral("No valid local recovery copies were found."));
            }
            return false;
        }
        if (startup_only && choices.size() == 1) {
            const auto answer = QMessageBox::question(
                owner, QStringLiteral("Recover previous work"),
                QStringLiteral("An unsaved local recovery copy is available:\n%1\n\nRecover it now?")
                    .arg(choices.front()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            return answer == QMessageBox::Yes && openProject(paths.front());
        }
        bool accepted = false;
        const auto selected = QInputDialog::getItem(
            owner, QStringLiteral("Recover project"),
            QStringLiteral("Choose a local recovery copy:"), choices, 0, false, &accepted);
        if (!accepted || selected.isEmpty()) return false;
        const auto index = choices.indexOf(selected);
        return index >= 0 && index < static_cast<int>(paths.size()) && openProject(paths[index]);
    }

public:
    bool offerStartupRecovery() {
        // Startup is called with a fresh untitled document. Keep this guard so
        // future callers cannot replace a live project or draft unexpectedly.
        if (!m_file_path.empty() || hasBoundaryDraftChanges()) return false;
        // The default scaffold is the only revision in a new window. It is
        // intentionally unsaved, but it has no user edits to protect.
        if (m_recovery_ledger.empty()) {
            if (m_document->revision() > 1) return false;
        } else if (projectDirty()) {
            return false;
        }
        return recoverFromDialog(true, false);
    }

private:
    struct ModalContext {
        std::shared_ptr<Document> document;
        Revision revision;
        QString selected_id;
        QString layer_id;
        bool metric_units;
    };

    ModalContext captureModalContext() const {
        return {m_document, m_document->revision(), m_selected_id, m_active_layer_id, m_metric_units};
    }

    bool modalContextUnchanged(const ModalContext& context) {
        if (m_document != context.document || m_document->revision() != context.revision ||
            m_selected_id != context.selected_id || m_active_layer_id != context.layer_id ||
            m_metric_units != context.metric_units) {
            setError(QStringLiteral("The project, selection, drawing layer or units changed while the dialog was open. Reopen the tool to use the current context."));
            return false;
        }
        return true;
    }

public:
    void showDeductionEditor() {
        const auto selected = selectedEntity();
        if (!selected || !is_closed_boundary_entity(selected->type)) {
            setError(QStringLiteral("Select a closed boundary before editing deductions."));
            return;
        }
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        const auto context = captureModalContext();
        try {
            const auto snapshot = authoringSnapshot();
            const auto floor_id = read_string(selected->properties, "floor_id");
            if (!floor_id.has_value() || floor_id->empty()) {
                throw std::invalid_argument("The selected boundary has no floor reference.");
            }
            QDialog dialog(owner);
            styleDialog(dialog);
            dialog.setObjectName(QStringLiteral("calculationDeductionDialog"));
            dialog.setWindowTitle(QStringLiteral("Area deductions"));
            dialog.setModal(true);
            dialog.resize(560, 420);
            auto* layout = new QVBoxLayout(&dialog);

            auto* source = new QComboBox(&dialog);
            source->setObjectName(QStringLiteral("calculationDeductionSource"));
            source->setToolTip(QStringLiteral("Choose a closed boundary on the active floor to subtract."));
            layout->addWidget(new QLabel(QStringLiteral("Closed boundary to subtract"), &dialog));
            layout->addWidget(source);

            auto* list = new QListWidget(&dialog);
            list->setObjectName(QStringLiteral("calculationDeductionList"));
            list->setSelectionMode(QAbstractItemView::SingleSelection);
            layout->addWidget(new QLabel(QStringLiteral("Deductions applied to this area"), &dialog));
            layout->addWidget(list, 1);

            auto* list_buttons = new QHBoxLayout();
            auto* add = new QPushButton(QStringLiteral("Add deduction"), &dialog);
            add->setObjectName(QStringLiteral("addCalculationDeduction"));
            auto* remove = new QPushButton(QStringLiteral("Remove selected"), &dialog);
            remove->setObjectName(QStringLiteral("removeCalculationDeduction"));
            list_buttons->addStretch(1);
            list_buttons->addWidget(add);
            list_buttons->addWidget(remove);
            layout->addLayout(list_buttons);

            auto* status = new QLabel(&dialog);
            status->setObjectName(QStringLiteral("calculationDeductionStatus"));
            status->setWordWrap(true);
            status->setTextFormat(Qt::PlainText);
            layout->addWidget(status);

            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
                                                 &dialog);
            buttons->setObjectName(QStringLiteral("calculationDeductionButtons"));
            layout->addWidget(buttons);

            QStringList staged;
            for (const auto& id : read_deduction_ids(selected->properties))
                staged.push_back(id_from(id));
            const auto label_for = [&](const Entity& entity) {
                auto type = QString::fromStdString(entity.type);
                type.replace(QLatin1Char('_'), QLatin1Char(' '));
                if (!type.isEmpty()) type[0] = type[0].toUpper();
                const auto name = read_string(entity.properties, "name");
                return name.has_value() && !name->empty()
                    ? QStringLiteral("%1  ·  %2").arg(QString::fromStdString(*name), id_from(entity.id))
                    : QStringLiteral("%1  ·  %2").arg(type, id_from(entity.id));
            };
            for (const auto& [id, candidate] : snapshot.entities()) {
                if (id == selected->id || !is_closed_boundary_entity(candidate.type)) continue;
                const auto candidate_floor = read_string(candidate.properties, "floor_id");
                if (!candidate_floor.has_value() || *candidate_floor != *floor_id) continue;
                const auto boundary = read_boundary(candidate.properties);
                if (validate_boundary(boundary).empty()) {
                    source->addItem(label_for(candidate), id_from(id));
                }
            }

            const auto refresh_list = [&] {
                list->clear();
                for (const auto& id : staged) {
                    const auto found = snapshot.entities().find(id.toStdString());
                    const auto label = found == snapshot.entities().end()
                        ? QStringLiteral("Missing boundary  ·  %1").arg(id)
                        : label_for(found->second);
                    auto* item = new QListWidgetItem(label, list);
                    item->setData(Qt::UserRole, id);
                }
                remove->setEnabled(list->currentItem() != nullptr);
                if (staged.isEmpty()) status->setText(QStringLiteral("No deductions selected."));
            };
            refresh_list();
            add->setEnabled(source->count() > 0);

            QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
                const auto id = source->currentData().toString();
                if (id.isEmpty() || staged.contains(id)) {
                    status->setText(QStringLiteral("Choose a boundary that is not already selected."));
                    return;
                }
                staged.push_back(id);
                refresh_list();
                status->setText(QStringLiteral("Deduction staged. Apply to validate and save it."));
            });
            QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
                if (!list->currentItem()) return;
                staged.removeAll(list->currentItem()->data(Qt::UserRole).toString());
                refresh_list();
                status->setText(staged.isEmpty() ? QStringLiteral("No deductions selected.")
                                                 : QStringLiteral("Deduction removed from the pending edit."));
            });
            QObject::connect(list, &QListWidget::currentRowChanged, &dialog,
                             [remove](int row) { remove->setEnabled(row >= 0); });
            QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                             &dialog, [&] {
                                 if (!modalContextUnchanged(context)) {
                                     status->setText(lastError());
                                     return;
                                 }
                                 if (setSelectedDeductions(staged)) {
                                     dialog.accept();
                                 } else {
                                     status->setText(lastError());
                                 }
                             });
            QObject::connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked,
                             &dialog, &QDialog::reject);
            dialog.exec();
            refreshInspector();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Deductions: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void showConstraintEditor(const QString& initial_length = {}) {
        const auto entity = selectedEntity();
        if (!entity || entity->type != "wall" || !m_document->is_editable()) {
            setError(QStringLiteral("Select an editable straight wall to change dimensions or constraints."));
            return;
        }
        const auto context = captureModalContext();
        try {
            ConstraintDialog dialog(authoringSnapshot(), m_selected_id, m_metric_units, owner);
            if (!initial_length.isEmpty()) dialog.setLengthExpression(initial_length);
            if (dialog.exec() != QDialog::Accepted) { refreshInspector(); return; }
            if (!modalContextUnchanged(context)) return;
            const auto preview = dialog.acceptedPreview();
            if (!preview) return;
            applyConstraintPreview(*preview);
            clearError();
            refresh();
        } catch (const std::exception& error) {
            setError(QStringLiteral("Constraint edit: %1").arg(QString::fromUtf8(error.what())));
        }
    }

private:
    void showBuildingObjectDialog(bool editing) {
        const auto context = captureModalContext();
        const auto original = editing ? selectedEntity() : std::optional<Entity>{};
        if (editing && (!original || !can_recognize_building_entity_type(original->type))) {
            setError(QStringLiteral("Select a column, beam, stair or roof to edit."));
            return;
        }
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This document is read-only."));
            return;
        }
        BuildingObjectDialog dialog(original, m_metric_units, owner);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (!modalContextUnchanged(context)) return;
        if (const auto candidate = dialog.candidate()) {
            if (!commitBuildingObject(*candidate, context.revision, editing).isEmpty())
                setWorkspace(Workspace::architectural);
        }
    }

    void showDoorSwingEditor() {
        const auto context = captureModalContext();
        const auto source = selectedEntity();
        if(!source || source->type != "opening") return;
        try {
            const auto prior = source->properties.value("door_operation",json{});
            const auto operation = prior.is_null() ? DoorOperation{} : decode_door_operation(prior);
            QDialog dialog(owner);
            dialog.setObjectName("doorSwingDialog"); dialog.setWindowTitle("Door swing");
            auto* layout = new QVBoxLayout(&dialog);
            auto* form = new QFormLayout;
            auto* hinge = new QComboBox(&dialog); hinge->setObjectName("editDoorHinge");
            hinge->addItems({"None","Start jamb","End jamb"});
            hinge->setCurrentIndex(prior.is_null()?0:(operation.hinge_at_end?2:1));
            hinge->setToolTip("Jamb order follows the host wall's drawing direction.");
            auto* side = new QComboBox(&dialog); side->setObjectName("editDoorSide");
            side->addItems({"Left of wall","Right of wall"}); side->setCurrentIndex(operation.swing_left?0:1);
            auto* angle = new QDoubleSpinBox(&dialog); angle->setObjectName("editDoorAngle");
            angle->setRange(0.01,180); angle->setDecimals(2); angle->setSuffix("°"); angle->setValue(operation.angle_degrees);
            const auto displayed_angle = angle->value();
            form->addRow("Hinge",hinge); form->addRow("Swing",side); form->addRow("Angle",angle);
            layout->addLayout(form);
            auto sync=[&]{side->setEnabled(hinge->currentIndex()!=0);angle->setEnabled(hinge->currentIndex()!=0);};
            sync(); QObject::connect(hinge,&QComboBox::currentIndexChanged,&dialog,[&]{sync();});
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);
            layout->addWidget(buttons);
            QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
            QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
            if(dialog.exec()!=QDialog::Accepted || !modalContextUnchanged(context)) return;
            auto candidate = *source;
            if(hinge->currentIndex()==0) candidate.properties.erase("door_operation");
            else candidate.properties["door_operation"] = encode_door_operation(
                DoorOperation{hinge->currentIndex()==2,side->currentIndex()==0,
                    angle->value()==displayed_angle ? operation.angle_degrees : angle->value()});
            if(candidate.properties==source->properties) return;
            applyDocumentCommand(ApplyEntityChanges{context.revision,{EntityChange::upsert(candidate)},{},"edit door swing"});
            clearError(); refresh();
        } catch(const std::exception& error) { setError(QString::fromUtf8(error.what())); }
    }

    void createOpeningFromDialog(const QString& kind) {
        const auto context = captureModalContext();
        const auto wall = selectedEntity();
        if (!wall.has_value() || wall->type != "wall") {
            setError(QStringLiteral("Select a wall before creating a %1 opening.").arg(kind));
            return;
        }
        try {
            const auto snapshot = m_document->snapshot();
            std::vector<const Entity*> openings;
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type != "opening") continue;
                const auto host_id = read_string(entity.properties, "wall_id");
                if (host_id && *host_id == wall->id) openings.push_back(&entity);
            }
            Wall host;
            std::string error;
            if (!read_document_wall(*wall, openings, host, error)) throw std::invalid_argument(error);
            HostedOpeningDialog dialog(host, m_metric_units ? Unit::metre : Unit::foot,
                kind == QStringLiteral("window"), owner);
            if (dialog.exec() != QDialog::Accepted || !modalContextUnchanged(context)) return;
            (void)createHostedOpening(kind, dialog.offsetExpression(), dialog.widthExpression(),
                dialog.sillExpression(), dialog.heightExpression(), context.revision, dialog.doorOperation());
        } catch (const std::exception& error) {
            setError(QStringLiteral("Opening: %1").arg(QString::fromUtf8(error.what())));
        }
    }

    void createSlabFromDialog() {
        const auto context = captureModalContext();
        const auto boundary = selectedEntity();
        if (!boundary.has_value() || !is_closed_boundary_entity(boundary->type)) {
            setError(QStringLiteral("Select a closed boundary before creating a slab."));
            return;
        }
        bool accepted = false;
        const auto thickness = QInputDialog::getText(
            owner, QStringLiteral("Create slab"), QStringLiteral("Slab thickness:"),
            QLineEdit::Normal, m_metric_units ? QStringLiteral("0.15 m")
                                               : QStringLiteral("6 in"),
            &accepted);
        if (!accepted) {
            return;
        }
        const auto elevation = QInputDialog::getText(
            owner, QStringLiteral("Create slab"), QStringLiteral("Elevation:"), QLineEdit::Normal,
            m_metric_units ? QStringLiteral("0 m") : QStringLiteral("0 in"), &accepted);
        if (!accepted) {
            return;
        }
        if (!modalContextUnchanged(context)) return;
        (void)createSlabFromSelectedBoundary(thickness, elevation, context.revision);
    }

    void saveAsFromDialog() {
        const auto selected = QFileDialog::getSaveFileName(
            owner, QStringLiteral("Save project as"), {}, QStringLiteral("Property Studio project (*.bldproj)"));
        if (!selected.isEmpty()) {
            saveProjectAs(selected);
        }
    }

    void exportFromDialog() {
        const auto selected = QFileDialog::getSaveFileName(
            owner, QStringLiteral("Export draft PDF"), {}, QStringLiteral("PDF document (*.pdf)"));
        if (!selected.isEmpty()) {
            exportDraftPdf(selected);
        }
    }

    void showAbout() {
        QMessageBox::information(
            owner, QStringLiteral("About Property Studio"),
            QStringLiteral("Property Studio — Internal checkpoint\n\n"
                           "An offline Windows application for property measurement\n"
                           "and architectural design.\n\n"
                           "This development build is incomplete. Apex compatibility\n"
                           "and production acceptance have not been certified."));
    }

    void setError(const QString& error) {
        m_last_error = error;
        owner->statusBar()->showMessage(error, 8000);
    }

    void clearError() {
        m_last_error.clear();
    }

    MainWindow* owner{};
    std::shared_ptr<Document> m_document;
    std::unique_ptr<ProjectWorkspace> m_project_workspace;
    RecoveryLedger m_recovery_ledger;
    std::uint64_t m_saved_workspace_epoch{};
    std::uint64_t m_saved_edited_generation{};
    const std::string m_save_owner_token{make_stable_id()};
    WorkspaceSaveQueue m_save_queue;
    WorkspaceAutosaveScheduler m_autosave_scheduler;
    struct PendingAutosave {
        std::uint64_t sequence;
        WorkspaceAutosaveScheduler::Capture capture;
        SavePublicationBinding binding;
    };
    std::optional<PendingAutosave> m_autosave_pending;
    std::map<std::uint64_t, WorkspaceSaveQueue::Completion> m_completed_saves;
    std::uint64_t m_completed_barrier{};
    std::uint64_t m_autosaved_checkpoint{};
    std::string m_autosave_document_id;
    std::string m_autosave_archive_id;
    std::filesystem::path m_autosave_path;
    std::string m_autosave_sha256;
    WorkspaceAutosaveScheduler::TimePoint m_autosave_retry_after{};
    QTimer* m_save_timer{};
    std::filesystem::path m_file_path;
    std::string m_file_sha256;
    Workspace m_workspace{Workspace::measurement};
    WorkspaceTheme m_theme{WorkspaceTheme::light};
    CanvasTool m_tool{CanvasTool::select};
    bool m_metric_units{false};
    bool m_grid_enabled{true};
    bool m_snap_enabled{true};
    bool m_overview_map_enabled{true};
    bool m_refreshing{false};
    ProjectViewFilter m_view_filter;
    QString m_active_layer_id;
    QComboBox* m_drawing_layer_combo{};
    QComboBox* m_model_phase_combo{};
    QComboBox* m_pageSizeCombo{};
    QComboBox* m_architecturalViewCombo{};
    QString m_output_sheet_id;
    QLabel* m_drawing_context_label{};
    QLabel* m_visibility_label{};
    QPushButton* m_show_all_button{};
    QString m_selected_id;
    QString m_last_error;
    QString m_plan_geometry_error;
    std::map<std::string, std::pair<std::string, Boundary>> m_plan_projection_cache;
    std::map<std::string, std::pair<std::string, QString>, std::less<>>
        m_plan_slab_validation_cache;
    std::array<std::vector<CanvasEntity>, 3> m_architectural_view_entities;
    Vec2 m_last_cursor{};
    std::optional<BoundaryAuthoringSession> m_boundary_session;
    std::optional<DocumentSnapshot> m_boundary_source;
    std::optional<DrawingContext> m_boundary_context;
    std::shared_ptr<Document> m_boundary_document;
    std::optional<QString> m_redefine_boundary_id;
    AssistanceSession m_assistance_session;
    QString m_last_boundary_classification{QStringLiteral("measurement")};
    QToolButton* m_define_boundary_button{};
    std::optional<Vec2> m_pending_wall_start;
    BuildingViewKind m_architectural_view_kind{BuildingViewKind::plan};

    VisibilityTreeWidget* m_navigator{};
    QSplitter* m_workspace_splitter{};
    QSplitter* m_architectural_splitter{};
    QTabWidget* m_workspaceTabs{};
    PlanCanvas* m_measurementCanvas{};
    PlanCanvas* m_architecturalCanvas{};
    visualization::NativeModelView* m_nativeModelView{};
    QScrollArea* m_inspector{};
    QFormLayout* m_geometry_form{};
    QGroupBox* m_calculation_group{};
    QGroupBox* m_profile_group{};
    QLabel* m_inspector_context{};
    QLabel* m_plan_error_banner{};
    QGroupBox* m_project_details_group{};
    QLineEdit* m_project_subject_name_edit{};
    QLineEdit* m_project_subject_address_edit{};
    QLineEdit* m_project_subject_reference_edit{};
    QPlainTextEdit* m_project_subject_attributes_edit{};
    QPushButton* m_apply_project_details_button{};
    struct BuildingDimensionField {
        QString suffix;
        const char* property;
        QLabel* label{};
        QLineEdit* edit{};
        QString original_text;
    };
    QGroupBox* m_building_properties_group{};
    QLabel* m_building_dimensions_error{};
    std::vector<BuildingDimensionField> m_building_dimensions;
    std::vector<BuildingDimensionField> m_building_placement;
    std::optional<ModalContext> m_building_edit_context;
    QWidget* m_material_group{};
    QPushButton* m_door_swing_button{};
    QComboBox* m_material_combo{};
    QLabel* m_material_error{};
    std::optional<ModalContext> m_material_context;
    QGroupBox* m_roof_properties_group{};
    QLineEdit* m_roof_run_edit{};
    QLabel* m_roof_run_label{};
    QLineEdit* m_roof_span_edit{};
    QLineEdit* m_roof_overhang_edit{};
    QLineEdit* m_roof_rise_edit{};
    QLineEdit* m_roof_thickness_edit{};
    QLabel* m_roof_pitch_value{};
    QLabel* m_roof_preview_error{};
    QPushButton* m_apply_roof_properties_button{};
    QString m_roof_run_original_text;
    QString m_roof_span_original_text;
    QString m_roof_overhang_original_text;
    QString m_roof_rise_original_text;
    QString m_roof_thickness_original_text;
    std::optional<ModalContext> m_roof_edit_context;
    QGroupBox* m_area_attributes_group{};
    QPlainTextEdit* m_area_attributes_edit{};
    QPushButton* m_apply_area_attributes_button{};
    QLabel* m_calculation_status{};
    QLabel* m_calculation_base_value{};
    QLabel* m_calculation_net_value{};
    QLabel* m_calculation_factored_value{};
    QLabel* m_calculation_perimeter_value{};
    QListWidget* m_calculation_deductions_list{};
    QPushButton* m_edit_deductions_button{};
    QLabel* m_calculation_rounding_value{};
    QLabel* m_calculation_building_total_value{};
    QLabel* m_calculation_living_total_value{};
    QLabel* m_calculation_profile_context{};
    QLabel* m_calculation_profile_version{};
    QLabel* m_read_only_label{};
    QComboBox* m_classification_combo{};
    QComboBox* m_unitsCombo{};
    QLineEdit* m_length_edit{};
    QLineEdit* m_height_edit{};
    QLineEdit* m_thickness_edit{};
    QLineEdit* m_factor_edit{};
    QCheckBox* m_include_building_check{};
    QCheckBox* m_include_living_check{};
    QToolButton* m_select_button{};
    QToolButton* m_boundary_button{};
    QToolButton* m_wall_button{};
    QToolButton* m_object_button{};
    QPushButton* m_edit_object_button{};
    QPushButton* m_delete_annotation_button{};
    QGroupBox* m_annotation_group{};
    QLineEdit* m_annotation_content_edit{};
    QLineEdit* m_annotation_x_edit{};
    QLineEdit* m_annotation_y_edit{};
    QLineEdit* m_annotation_rotation_edit{};
    QLineEdit* m_annotation_scale_edit{};
    QCheckBox* m_annotation_visible_check{};
    QPushButton* m_apply_annotation_button{};
    QGroupBox* m_reference_group{};
    QLineEdit* m_reference_x_edit{};
    QLineEdit* m_reference_y_edit{};
    QLineEdit* m_reference_calibration_edit{};
    QLineEdit* m_reference_scale_edit{};
    QLineEdit* m_reference_rotation_edit{};
    QLineEdit* m_reference_intensity_edit{};
    QCheckBox* m_reference_flip_horizontal_check{};
    QCheckBox* m_reference_flip_vertical_check{};
    QCheckBox* m_reference_visible_check{};
    QPushButton* m_apply_reference_button{};
    QPushButton* m_calibrate_reference_button{};
    QPushButton* m_constraint_button{};
    QToolButton* m_grid_button{};
    QToolButton* m_snap_button{};
    QToolButton* m_fit_button{};
    QToolButton* m_overview_button{};
    QAction* m_new_action{};
    QAction* m_open_action{};
    QAction* m_recover_action{};
    QAction* m_save_action{};
    QAction* m_save_as_action{};
    QAction* m_undo_action{};
    QAction* m_redo_action{};
    QAction* m_measurement_action{};
    QAction* m_architectural_action{};
    QAction* m_palette_action{};
    QAction* m_copy_action{};
    QAction* m_cut_action{};
    QAction* m_paste_action{};
    QAction* m_delete_action{};
    QAction* m_insert_vertex_action{};
    QAction* m_redefine_action{};
    QAction* m_detect_areas_action{};
    std::vector<ShortcutBinding> m_shortcuts;
    QString m_shortcut_load_error;
    QAction* m_annotation_action{};
    QAction* m_reference_action{};
    QAction* m_schedule_action{};
    QAction* m_sheet_action{};
    QAction* m_viewport_action{};
    QAction* m_schedule_placement_action{};
    QAction* m_view_action{};
    QAction* m_remodel_action{};
    QAction* m_relationship_action{};
    QAction* m_levels_action{};
    QAction* m_assembly_action{};
    QAction* m_assistance_action{};
    QAction* m_workspace_profiles_action{};
    QAction* m_revisions_action{};
    QAction* m_transform_action{};
    QAction* m_about_action{};
};

MainWindow::MainWindow(std::shared_ptr<Document> document, QWidget* parent)
    : QMainWindow(parent), m_impl(std::make_unique<Impl>(this, std::move(document))) {}

MainWindow::~MainWindow() = default;

Document& MainWindow::document() noexcept {
    return m_impl->document();
}

const Document& MainWindow::document() const noexcept {
    return m_impl->document();
}

DocumentScheduleProjection MainWindow::scheduleSnapshot() const {
    return m_impl->scheduleSnapshot();
}

bool MainWindow::editProjectSubject(const QString& name, const QString& address,
                                    const QString& reference, const QString& attributes_json) {
    return m_impl->editProjectSubject(name, address, reference, attributes_json);
}

bool MainWindow::editSelectedAreaAttributes(const QString& attributes_json) {
    return m_impl->editSelectedAreaAttributes(attributes_json);
}

bool MainWindow::editScheduleCell(const QString& object_id, const QString& column,
                                   const QString& replacement) {
    return m_impl->editScheduleCell(object_id, column, replacement);
}

bool MainWindow::editSheetMetadata(const QString& sheet_id, const QString& number,
                                   const QString& project, const QString& title,
                                   const QString& author, const QString& issue_date) {
    return m_impl->editSheetMetadata(sheet_id, number, project, title, author, issue_date);
}

bool MainWindow::editSheetViewport(const QString& sheet_id, const QString& viewport_id,
                                   const QString& x_mm, const QString& y_mm,
                                   const QString& width_mm, const QString& height_mm,
                                   const QString& scale_denominator) {
    return m_impl->editSheetViewport(sheet_id, viewport_id, x_mm, y_mm, width_mm, height_mm,
                                     scale_denominator);
}

bool MainWindow::editSheetSchedulePlacement(const QString& sheet_id, const QString& placement_id,
                                            const QString& x_mm, const QString& y_mm,
                                            const QString& width_mm, const QString& height_mm) {
    return m_impl->editSheetSchedulePlacement(sheet_id, placement_id, x_mm, y_mm, width_mm,
                                              height_mm);
}

QString MainWindow::createDrawingSheet(const QString& number, const QString& width_mm,
                                       const QString& height_mm, const QString& title) {
    return m_impl->createDrawingSheet(number, width_mm, height_mm, title);
}

bool MainWindow::removeDrawingSheet(const QString& sheet_id) {
    return m_impl->removeDrawingSheet(sheet_id);
}

bool MainWindow::selectOutputSheet(const QString& sheet_id) {
    return m_impl->selectOutputSheet(sheet_id);
}

QString MainWindow::outputSheetId() const {
    return m_impl->outputSheetId();
}

bool MainWindow::editArchitecturalViewPresentation(
    const QString& view_id, const QString& cut_depth_m, const QString& far_depth_m,
    const QString& cut_line_mm, const QString& projection_line_mm, bool hatch_enabled,
    const QString& hatch_pattern, const QString& hatch_scale, const QString& detail) {
    return m_impl->editArchitecturalViewPresentation(
        view_id, cut_depth_m, far_depth_m, cut_line_mm, projection_line_mm, hatch_enabled,
        hatch_pattern, hatch_scale, detail);
}

Workspace MainWindow::workspace() const noexcept {
    return m_impl->workspace();
}

void MainWindow::setWorkspace(Workspace workspace) {
    m_impl->setWorkspace(workspace);
}

bool MainWindow::workspaceDocumentsShareDocument() const noexcept {
    return m_impl->workspaceDocumentsShareDocument();
}

bool MainWindow::metricUnits() const noexcept {
    return m_impl->metricUnits();
}

void MainWindow::setMetricUnits(bool metric) {
    m_impl->setMetricUnits(metric);
}

QString MainWindow::activeLayerId() const { return m_impl->activeLayerId(); }
bool MainWindow::setActiveLayer(const QString& id) { return m_impl->setActiveLayer(id); }
bool MainWindow::setContainerVisible(const QString& id, bool visible) {
    return m_impl->setContainerVisible(id, visible);
}
void MainWindow::showAllContainers() { m_impl->showAllContainers(); }
bool MainWindow::entityVisible(const QString& id) const { return m_impl->entityVisible(id); }
QString MainWindow::activeRemodelingAlternative() const {
    return m_impl->activeRemodelingAlternative();
}
bool MainWindow::selectRemodelingAlternative(const QString& alternative_id) {
    return m_impl->selectRemodelingAlternative(alternative_id);
}
QString MainWindow::createBuilding(const QString& parent, const QString& name, std::optional<Revision> revision) {
    return m_impl->createOrganization(parent, name, "building", revision);
}
QString MainWindow::createFloor(const QString& parent, const QString& name, std::optional<Revision> revision) {
    return m_impl->createOrganization(parent, name, "floor", revision);
}
QString MainWindow::createLayer(const QString& parent, const QString& name, std::optional<Revision> revision) {
    return m_impl->createOrganization(parent, name, "layer", revision);
}
bool MainWindow::renameOrganizationEntity(const QString& id, const QString& name,
                                          std::optional<Revision> revision) {
    return m_impl->renameOrganizationEntity(id, name, revision);
}

bool MainWindow::beginBoundaryDrawing(BoundaryAuthoringMode mode, QString classification) {
    return m_impl->beginBoundaryDrawing(mode, std::move(classification));
}

QString MainWindow::createBoundary(const Boundary& boundary, QString classification,
                                   std::optional<Revision> revision) {
    return m_impl->createBoundary(boundary, classification, revision);
}

QString MainWindow::createRoomBoundary(const Boundary& boundary, QString classification,
                                       std::optional<Revision> revision) {
    return m_impl->createRoomBoundary(boundary, classification, revision);
}

QString MainWindow::createRoomBoundaryFromExistingGeometry(QString classification,
                                                           std::optional<Revision> revision) {
    return m_impl->createRoomBoundaryFromExistingGeometry(classification, revision);
}

QStringList MainWindow::detectRoomBoundariesFromExistingWalls(
    QString classification, std::optional<Revision> revision) {
    return m_impl->detectRoomBoundariesFromExistingWalls(std::move(classification), revision);
}

QString MainWindow::createStraightWall(Vec2 start, Vec2 end, QString classification,
                                       std::optional<Revision> revision) {
    return m_impl->createStraightWall(start, end, classification, revision);
}

QString MainWindow::commitBuildingObject(Entity candidate, std::uint64_t expected_revision,
                                         bool replace_selected) {
    return m_impl->commitBuildingObject(std::move(candidate), expected_revision, replace_selected);
}

QString MainWindow::createHostedOpening(QString kind,
                                        QString offset,
                                        QString width,
                                        QString sill,
                                        QString height, std::optional<Revision> expected_revision) {
    return m_impl->createHostedOpening(kind, offset, width, sill, height, expected_revision);
}

QString MainWindow::createSlabFromSelectedBoundary(QString thickness, QString elevation,
                                                   std::optional<Revision> revision) {
    return m_impl->createSlabFromSelectedBoundary(thickness, elevation, revision);
}

QString MainWindow::createSlabFromBoundary(const Boundary& boundary,
                                           QString thickness,
                                           QString elevation,
                                           std::vector<Boundary> holes,
                                           std::optional<Revision> revision) {
    return m_impl->createSlabFromBoundary(boundary, thickness, elevation, std::move(holes), revision);
}

bool MainWindow::selectEntity(const QString& entity_id) {
    return m_impl->selectEntity(entity_id);
}

QString MainWindow::selectedEntityId() const {
    return m_impl->selectedEntityId();
}

bool MainWindow::copySelection() {
    return m_impl->copySelection();
}

bool MainWindow::cutSelection() {
    return m_impl->cutSelection();
}

bool MainWindow::pasteSelection() {
    return m_impl->pasteSelection();
}

bool MainWindow::deleteSelection() {
    return m_impl->deleteSelection();
}

bool MainWindow::insertSelectedBoundaryVertex(const QString& segment_id,
                                              const QString& fraction) {
    return m_impl->insertSelectedBoundaryVertex(segment_id, fraction);
}

bool MainWindow::redefineSelectedBoundary(const Boundary& boundary,
                                           const QString& classification) {
    return m_impl->redefineSelectedBoundary(boundary, classification);
}

bool MainWindow::editSelectedClassification(const QString& classification) {
    return m_impl->editSelectedClassification(classification);
}

bool MainWindow::editSelectedLength(const QString& expression) {
    return m_impl->editSelectedLength(expression);
}

bool MainWindow::editSelectedHeight(const QString& expression) {
    return m_impl->editSelectedHeight(expression);
}

bool MainWindow::editSelectedThickness(const QString& expression) {
    return m_impl->editSelectedThickness(expression);
}

bool MainWindow::editSelectedFactor(const QString& expression) {
    return m_impl->editSelectedFactor(expression);
}

bool MainWindow::setSelectedCalculationRule(bool include_in_building, bool include_in_living) {
    return m_impl->setSelectedCalculationRule(include_in_building, include_in_living);
}

bool MainWindow::transformSelectedBoundary(const QString& rotation_degrees,
                                           bool flip_horizontal,
                                           bool flip_vertical,
                                           const QString& offset_x,
                                           const QString& offset_y,
                                           bool clone) {
    return m_impl->transformSelectedBoundary(rotation_degrees, flip_horizontal, flip_vertical,
                                             offset_x, offset_y, clone);
}

QString MainWindow::createAnnotationLabel(const QString& template_id, const QString& content,
                                          Vec2 position) {
    return m_impl->createAnnotationLabel(template_id, content, position);
}

QString MainWindow::createAnnotationSymbol(const QString& symbol_id, Vec2 position) {
    return m_impl->createAnnotationSymbol(symbol_id, position);
}

bool MainWindow::editAnnotation(const QString& annotation_id, const QString& content,
                                const QString& x_metres, const QString& y_metres,
                                const QString& rotation_degrees, const QString& scale,
                                bool visible) {
    return m_impl->editAnnotation(annotation_id, content, x_metres, y_metres,
                                  rotation_degrees, scale, visible);
}

bool MainWindow::deleteAnnotation(const QString& annotation_id) {
    return m_impl->deleteAnnotation(annotation_id);
}

QString MainWindow::importReferenceImage(const QString& path) {
    return m_impl->importReferenceImage(path);
}

bool MainWindow::calibrateReference(const QString& reference_id, const QString& first_x,
                                    const QString& first_y, const QString& second_x,
                                    const QString& second_y, const QString& known_distance) {
    return m_impl->calibrateReference(reference_id, first_x, first_y, second_x, second_y,
                                      known_distance);
}

bool MainWindow::editReferenceTransform(const QString& reference_id, const QString& x_metres,
                                        const QString& y_metres,
                                        const QString& metres_per_source_unit,
                                        const QString& scale, const QString& rotation_degrees,
                                        const QString& intensity, bool flip_horizontal,
                                        bool flip_vertical, bool visible) {
    return m_impl->editReferenceTransform(reference_id, x_metres, y_metres,
                                          metres_per_source_unit, scale, rotation_degrees,
                                          intensity, flip_horizontal, flip_vertical, visible);
}

bool MainWindow::beginReferenceTrace() {
    return m_impl->beginReferenceTrace();
}

bool MainWindow::assistanceEnabled() const noexcept {
    return m_impl->assistanceEnabled();
}

void MainWindow::setAssistanceEnabled(bool enabled) {
    m_impl->setAssistanceEnabled(enabled);
}

std::vector<AssistanceProposal> MainWindow::suggestReferenceAssistance(
    const QString& reference_id, AssistanceKind kind) {
    return m_impl->suggestReferenceAssistance(reference_id, kind);
}

std::vector<AssistanceProposal> MainWindow::suggestLabelAssistance() {
    return m_impl->suggestLabelAssistance();
}

std::vector<AssistanceProposal> MainWindow::parseAssistanceCommand(const QString& command) {
    return m_impl->parseAssistanceCommand(command);
}

bool MainWindow::acceptAssistanceProposal(const AssistanceProposal& proposal) {
    return m_impl->acceptAssistanceProposal(proposal);
}

bool MainWindow::undoCommand() {
    return m_impl->undoCommand();
}

bool MainWindow::redoCommand() {
    return m_impl->redoCommand();
}

void MainWindow::showAnnotationEditor() {
    m_impl->showAnnotationEditor();
}

void MainWindow::showReferenceImport() {
    m_impl->showReferenceImport();
}

void MainWindow::showAssistance() {
    m_impl->showAssistance();
}

void MainWindow::showRemodelingAlternatives() {
    m_impl->showRemodelingAlternatives();
}

void MainWindow::showRoomRelationships() {
    m_impl->showRoomRelationships();
}

void MainWindow::showVerticalLevels() {
    m_impl->showVerticalLevels();
}

void MainWindow::showAssemblies() {
    m_impl->showAssemblies();
}

bool MainWindow::createNewProject() {
    return m_impl->createNewProject();
}

bool MainWindow::openProject(const QString& path) {
    return m_impl->openProject(path);
}

bool MainWindow::saveProject() {
    return m_impl->saveProject();
}

QString MainWindow::recoveryCopyPath() const {
    return m_impl->recoveryCopyPath();
}

bool MainWindow::offerStartupRecovery() {
    return m_impl->offerStartupRecovery();
}

bool MainWindow::saveProjectAs(const QString& path) {
    return m_impl->saveProjectAs(path);
}

bool MainWindow::exportDraftPdf(const QString& path) {
    return m_impl->exportDraftPdf(path);
}

bool MainWindow::exportDraftSvg(const QString& path) {
    return m_impl->exportDraftSvg(path);
}

bool MainWindow::exportNativeViewImage(const QString& path) {
    return m_impl->exportNativeViewImage(path);
}

bool MainWindow::showPrintPreview() {
    return m_impl->showPrintPreview();
}

void MainWindow::showCommandPalette() {
    m_impl->showCommandPalette();
}

void MainWindow::showConstraintEditor() {
    m_impl->showConstraintEditor();
}

void MainWindow::showWorkspaceProfiles() {
    m_impl->showWorkspaceProfiles();
}

void MainWindow::showRevisionHistory() {
    m_impl->showRevisionHistory();
}

bool MainWindow::restoreNamedRevision(const QString& name, const QString& path) {
    return m_impl->restoreNamedRevision(name, path);
}

void MainWindow::showBoundaryTransformEditor() {
    m_impl->showBoundaryTransformEditor();
}

void MainWindow::showBoundaryRedefinition() {
    m_impl->showBoundaryRedefinition();
}

void MainWindow::showAutomaticAreaDetection() {
    m_impl->showAutomaticAreaDetection();
}

void MainWindow::fitView() {
    m_impl->fitView();
}

QString MainWindow::lastError() const {
    return m_impl->lastError();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_impl->closeEvent(event);
}

}  // namespace sketch::desktop
