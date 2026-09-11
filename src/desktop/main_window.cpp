#include "sketch/desktop/main_window.hpp"

#include "plan_canvas.hpp"

#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/building_plan_projection.hpp"
#include "sketch/building_view_projection.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/boundary_commit.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/output_fingerprint.hpp"
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
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/visualization/native_model_view.hpp"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCloseEvent>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPageSize>
#include <QPalette>
#include <QPainter>
#include <QPdfWriter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QSaveFile>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSvgGenerator>
#include <QStyle>
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
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace sketch::desktop {
namespace {

using json = nlohmann::json;

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

bool is_closed_boundary_entity(std::string_view type) {
    return type == "boundary" || type == "measurement_boundary" || type == "room_boundary";
}

bool is_architectural_entity(std::string_view type) {
    return type == "wall" || type == "opening" || type == "room" || type == "slab" ||
           type == "roof" || type == "stair" || type == "column" || type == "beam";
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
            }
        }
        QTreeWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
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
        return build_document_schedules(m_document->snapshot());
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
        QPalette palette = owner->style()->standardPalette();
        if (theme == WorkspaceTheme::dark) {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#20252b")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#f1f5f9")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#15191e")));
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#252c34")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#f1f5f9")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#2b333d")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#f1f5f9")));
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2f80ed")));
            palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        } else if (theme == WorkspaceTheme::high_contrast) {
            palette.setColor(QPalette::Window, Qt::black);
            palette.setColor(QPalette::WindowText, Qt::white);
            palette.setColor(QPalette::Base, Qt::black);
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#202020")));
            palette.setColor(QPalette::Text, Qt::white);
            palette.setColor(QPalette::Button, Qt::black);
            palette.setColor(QPalette::ButtonText, Qt::white);
            palette.setColor(QPalette::Highlight, Qt::yellow);
            palette.setColor(QPalette::HighlightedText, Qt::black);
            palette.setColor(QPalette::Link, Qt::yellow);
        }
        owner->setPalette(palette);
        owner->setProperty("workspaceTheme", QString::fromStdString(workspace_theme_name(theme)));
        m_theme = theme;
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
        const auto visible = visible_project_entities(m_document->snapshot(), m_view_filter);
        return visible.contains(id.toStdString());
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

    bool beginBoundaryDrawing(BoundaryAuthoringMode mode, QString classification) {
        if (!m_document->is_editable()) {
            setError(QStringLiteral("This project is read-only."));
            return false;
        }
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
                           std::optional<Revision> expected_revision = std::nullopt) {
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
        const auto properties = json{{"floor_id", drawing_context->floor_id},
                                     {"layer_id", drawing_context->layer_id},
                                     {"segments", boundary_json(boundary)},
                                     {"classification", classification.toStdString()},
                                     {"factor", 1.0},
                                     {"factor_expression", "1"},
                                     {"factor_numerator", 1},
                                     {"factor_denominator", 1}};
        if (!applyEntity(Entity{entity_id,
                                "measurement_boundary",
                                properties,
                                false,
                                json::object()},
                         "create measurement boundary", revision)) {
            return {};
        }
        m_selected_id = id;
        refresh();
        return id;
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
                                std::optional<Revision> expected_revision = std::nullopt) {
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

            const auto properties = json{{"wall_id", wall_entity->id},
                                         {"offset_m", offset},
                                         {"width_m", width},
                                         {"sill_m", sill},
                                         {"height_m", height},
                                         {"opening_kind", normalized_kind.toStdString()},
                                         {"classification", normalized_kind.toStdString()}};
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
            if (m_recovery_ledger.empty()) m_document->undo(m_document->revision());
            else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_undo();
                commitWorkspaceEdit(edit);
            }
            m_selected_id.clear();
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
            if (m_recovery_ledger.empty()) m_document->redo(m_document->revision());
            else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_redo();
                commitWorkspaceEdit(edit);
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
            const auto& sheet = model.sheets().front();
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
        const DocumentSnapshot& snapshot) const {
        const auto build_digest = currentExecutableDigest();
        OutputFingerprintInputs inputs;
        inputs.profiles = fingerprint_not_applicable(
            "Calculation profiles are stored in the document head.");
        inputs.fonts = fingerprint_not_applicable(
            "Draft output uses the selected local Qt font without embedding font bytes.");
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

    bool writeOutputFingerprint(const QString& output_path, const DocumentSnapshot& snapshot,
                                QString output_kind) {
        const auto fingerprint = make_output_fingerprint(snapshot, outputFingerprintInputs(snapshot));
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
        if (!m_nativeModelView->exportViewImage(path)) {
            const auto reason = m_nativeModelView->lastError();
            setError(reason.isEmpty() ? QStringLiteral("Native OCCT 3D export failed.")
                                      : QStringLiteral("Native OCCT 3D export failed: %1")
                                            .arg(reason));
            return false;
        }
        if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0) {
            setError(QStringLiteral("Native OCCT 3D export did not produce an image."));
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
            if (model.sheets().empty()) throw std::invalid_argument("no drawing sheets are defined");
            const auto& sheet = model.sheets().front();
            QDialog dialog(owner);
            dialog.setWindowTitle(QStringLiteral("Sheet settings"));
            dialog.setModal(true);
            auto* form = new QFormLayout(&dialog);
            auto* number = new QLineEdit(QString::fromStdString(sheet.number), &dialog);
            auto* project = new QLineEdit(QString::fromStdString(sheet.title_block.project), &dialog);
            auto* title = new QLineEdit(QString::fromStdString(sheet.title_block.title), &dialog);
            auto* author = new QLineEdit(QString::fromStdString(sheet.title_block.author), &dialog);
            auto* issue_date = new QLineEdit(QString::fromStdString(sheet.title_block.issue_date), &dialog);
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
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted || !modalContextUnchanged(context)) return;
            (void)editSheetMetadata(QString::fromStdString(sheet.id), number->text(),
                                    project->text(), title->text(), author->text(), issue_date->text());
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

    void showCommandPalette() {
        QDialog dialog(owner);
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
            {QStringLiteral("New project"), [this] { createNewProject(); }},
            {QStringLiteral("Open project"), [this] { openFromDialog(); }},
            {QStringLiteral("Save project"), [this] { saveProject(); }},
            {QStringLiteral("Save project as…"), [this] { saveAsFromDialog(); }},
            {QStringLiteral("Undo"), [this] { undoCommand(); }},
            {QStringLiteral("Redo"), [this] { redoCommand(); }},
            {QStringLiteral("Add building"), [this] { showOrganizationDialog("building"); }},
            {QStringLiteral("Add floor"), [this] { showOrganizationDialog("floor"); }},
            {QStringLiteral("Add drawing layer"), [this] { showOrganizationDialog("layer"); }},
            {QStringLiteral("Rename selected property, building, floor or layer"),
             [this] { showOrganizationDialog("rename"); }},
            {QStringLiteral("Measurement workspace"), [this] { setWorkspace(Workspace::measurement); }},
            {QStringLiteral("Architectural workspace"), [this] { setWorkspace(Workspace::architectural); }},
            {QStringLiteral("Add labels and symbols"), [this] { showAnnotationEditor(); }},
            {QStringLiteral("Open schedules"), [this] { showSchedules(); }},
            {QStringLiteral("Edit drawing sheet settings"), [this] { showSheetSettings(); }},
            {QStringLiteral("Edit sheet viewport settings"), [this] { showViewportSettings(); }},
            {QStringLiteral("Edit schedule placement settings"),
             [this] { showSchedulePlacementSettings(); }},
            {QStringLiteral("Edit architectural view settings"),
             [this] { showArchitecturalViewSettings(); }},
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

    void applyDocumentCommand(const Command& command) {
        if (m_recovery_ledger.empty()) {
            m_document->apply(command);
            return;
        }
        requireWorkspaceDocument();
        auto edit = m_project_workspace->prepare(command);
        commitWorkspaceEdit(edit);
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
                auto directory = m_file_path.empty()
                    ? filesystem_path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)) / "recovery"
                    : m_file_path.parent_path();
                if (directory.empty()) directory = std::filesystem::current_path();
                m_autosave_path = directory / ("recovery-" + m_autosave_archive_id + ".bldproj");
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
        try {
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
            clearError();
            refresh();
            owner->statusBar()->showMessage(hasBoundaryDraftChanges()
                ? QStringLiteral("Saved committed geometry. The unfinished boundary is still unsaved.")
                : QStringLiteral("Saved revision %1.").arg(receipt.revision), 6000);
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
        owner->setObjectName(QStringLiteral("propertyStudioMainWindow"));
        owner->resize(1480, 900);
        owner->setMinimumSize(1080, 700);

        auto* toolbar = owner->addToolBar(QStringLiteral("Workspace"));
        toolbar->setMovable(false);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_new_action = toolbar->addAction(QStringLiteral("New"));
        m_open_action = toolbar->addAction(QStringLiteral("Open"));
        m_recover_action = toolbar->addAction(QStringLiteral("Recover…"));
        m_save_action = toolbar->addAction(QStringLiteral("Save"));
        m_save_as_action = toolbar->addAction(QStringLiteral("Save as…"));
        toolbar->addSeparator();
        m_undo_action = toolbar->addAction(QStringLiteral("Undo"));
        m_redo_action = toolbar->addAction(QStringLiteral("Redo"));
        toolbar->addSeparator();
        m_measurement_action = toolbar->addAction(QStringLiteral("Measurement"));
        m_architectural_action = toolbar->addAction(QStringLiteral("Architectural"));
        toolbar->addSeparator();
        m_palette_action = toolbar->addAction(QStringLiteral("Commands"));
        m_annotation_action = toolbar->addAction(QStringLiteral("Annotations"));
        m_schedule_action = toolbar->addAction(QStringLiteral("Schedules"));
        m_sheet_action = toolbar->addAction(QStringLiteral("Sheet settings"));
        m_viewport_action = toolbar->addAction(QStringLiteral("Viewport settings"));
        m_schedule_placement_action = toolbar->addAction(QStringLiteral("Schedule placement"));
        m_view_action = toolbar->addAction(QStringLiteral("View settings"));
        m_about_action = toolbar->addAction(QStringLiteral("About"));
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
        theme_button->setText(QStringLiteral("Theme"));
        theme_button->setToolTip(QStringLiteral("Select light, dark, or high-contrast workspace theme"));
        theme_button->setMenu(theme_menu);
        theme_button->setPopupMode(QToolButton::InstantPopup);
        toolbar->addWidget(theme_button);
         toolbar->addWidget(new QLabel(QStringLiteral("Units"), toolbar));
         m_unitsCombo = new QComboBox(toolbar);
         m_unitsCombo->addItems({QStringLiteral("Imperial (ft/in)"), QStringLiteral("Metric (m/mm)")});
         toolbar->addWidget(m_unitsCombo);
         toolbar->addWidget(new QLabel(QStringLiteral("Sheet"), toolbar));
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
         m_pageSizeCombo->setToolTip(QStringLiteral("Select the draft PDF and print sheet size"));
         toolbar->addWidget(m_pageSizeCombo);
         toolbar->addWidget(new QLabel(QStringLiteral("Architectural view"), toolbar));
         m_architecturalViewCombo = new QComboBox(toolbar);
         m_architecturalViewCombo->setObjectName(QStringLiteral("architecturalView"));
         m_architecturalViewCombo->addItem(QStringLiteral("Plan"), static_cast<int>(BuildingViewKind::plan));
         m_architecturalViewCombo->addItem(QStringLiteral("Elevation"), static_cast<int>(BuildingViewKind::elevation));
         m_architecturalViewCombo->addItem(QStringLiteral("Section @ 1.2 m"), static_cast<int>(BuildingViewKind::section));
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
        m_new_action->setShortcut(QKeySequence::New);
        m_open_action->setShortcut(QKeySequence::Open);
        m_save_action->setShortcut(QKeySequence::Save);
        m_palette_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
        applyTheme(WorkspaceTheme::light);

        auto* central = new QWidget(owner);
        auto* root_layout = new QVBoxLayout(central);
        root_layout->setContentsMargins(0, 0, 0, 0);
        auto* banner = new QLabel(
            QStringLiteral("Offline native workflow  •  Measurement + Architectural"),
            central);
        banner->setObjectName(QStringLiteral("checkpointBanner"));
        banner->setStyleSheet(QStringLiteral(
            "QLabel#checkpointBanner { background:#243142; color:#d8e7f7; padding:5px 10px; "
            "font-weight:600; letter-spacing:0.4px; }"));
        root_layout->addWidget(banner);
        m_plan_error_banner = new QLabel(central);
        m_plan_error_banner->setObjectName(QStringLiteral("planGeometryError"));
        m_plan_error_banner->setWordWrap(true);
        m_plan_error_banner->setStyleSheet(QStringLiteral(
            "color:#8b1a1a; background:#fff0ed; border:1px solid #d38d86; padding:6px 10px;"));
        root_layout->addWidget(m_plan_error_banner);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setChildrenCollapsible(true);
        root_layout->addWidget(splitter, 1);

        auto* navigator_panel = new QWidget(splitter);
        navigator_panel->setMinimumWidth(190);
        navigator_panel->setMaximumWidth(260);
        auto* navigator_layout = new QVBoxLayout(navigator_panel);
        navigator_layout->setContentsMargins(0, 0, 0, 0);
        navigator_layout->addWidget(new QLabel(QStringLiteral("Drawing layer"), navigator_panel));
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
        m_drawing_context_label->setStyleSheet(QStringLiteral("color:#555; font-size:11px;"));
        navigator_layout->addWidget(m_drawing_context_label);
        auto* scope_label = new QLabel(QStringLiteral("Geometry uses world elevations"), navigator_panel);
        scope_label->setStyleSheet(QStringLiteral("color:#666; font-size:11px;"));
        scope_label->setWordWrap(true);
        navigator_layout->addWidget(scope_label);
        auto* visibility_header = new QHBoxLayout();
        auto* visibility_heading = new QLabel(QStringLiteral("Visibility"), navigator_panel);
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
        m_visibility_label->setStyleSheet(QStringLiteral("color:#666; font-size:11px;"));
        navigator_layout->addWidget(m_visibility_label);
        QObject::connect(m_show_all_button, &QPushButton::clicked, owner,
                         [this] { showAllContainers(); });
        QObject::connect(m_drawing_layer_combo, &QComboBox::activated, owner, [this](int index) {
            setActiveLayer(m_drawing_layer_combo->itemData(index).toString());
        });
        m_navigator = new VisibilityTreeWidget(navigator_panel);
        navigator_layout->addWidget(m_navigator, 1);
        m_navigator->setObjectName(QStringLiteral("projectNavigator"));
        m_navigator->setHeaderHidden(true);
        m_navigator->setIndentation(14);
        m_navigator->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_navigator->setMinimumWidth(190);
        m_navigator->setMaximumWidth(230);
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
        tool_panel->setFixedWidth(82);
        auto* tool_layout = new QVBoxLayout(tool_panel);
        tool_layout->setContentsMargins(4, 8, 4, 8);
        tool_layout->setSpacing(4);
        m_select_button = addToolButton(tool_layout, QStringLiteral("Select"), CanvasTool::select, true);
        m_boundary_button = addToolButton(tool_layout, QStringLiteral("Boundary"), CanvasTool::boundary);
        m_boundary_button->setText(QStringLiteral("Draw first"));
        m_boundary_button->setObjectName(QStringLiteral("drawFirstBoundary"));
        m_boundary_button->setToolTip(QStringLiteral("Draw measured linework, then choose its area classification"));
        m_define_boundary_button = new QToolButton(tool_panel);
        m_define_boundary_button->setText(QStringLiteral("Define first"));
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
        m_grid_button->setToolTip(QStringLiteral("Toggle measurement grid"));
        m_grid_button->setCheckable(true);
        m_grid_button->setChecked(true);
        m_grid_button->setAutoRaise(true);
        m_grid_button->setMinimumWidth(0);
        m_grid_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_grid_button);
        m_snap_button = new QToolButton(tool_panel);
        m_snap_button->setText(QStringLiteral("Snap"));
        m_snap_button->setToolTip(QStringLiteral("Snap points to a 0.25 m grid"));
        m_snap_button->setCheckable(true);
        m_snap_button->setChecked(true);
        m_snap_button->setAutoRaise(true);
        m_snap_button->setMinimumWidth(0);
        m_snap_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_snap_button);
        m_fit_button = new QToolButton(tool_panel);
        m_fit_button->setText(QStringLiteral("Fit"));
        m_fit_button->setToolTip(QStringLiteral("Fit geometry in both workspace views"));
        m_fit_button->setAutoRaise(true);
        m_fit_button->setMinimumWidth(0);
        m_fit_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tool_layout->addWidget(m_fit_button);
        QObject::connect(m_grid_button, &QToolButton::toggled, owner,
                         [this](bool enabled) { setGrid(enabled); });
        QObject::connect(m_snap_button, &QToolButton::toggled, owner,
                         [this](bool enabled) { setSnap(enabled); });
        QObject::connect(m_fit_button, &QToolButton::clicked, owner, [this] { fitView(); });

        m_workspaceTabs = new QTabWidget(splitter);
        m_workspaceTabs->setDocumentMode(true);
        m_workspaceTabs->setTabsClosable(false);
        m_measurementCanvas = new PlanCanvas(m_workspaceTabs);
        m_measurementCanvas->setObjectName(QStringLiteral("measurementPlanCanvas"));
        auto* architectural_body = new QWidget(m_workspaceTabs);
        auto* architectural_layout = new QHBoxLayout(architectural_body);
        architectural_layout->setContentsMargins(0, 0, 0, 0);
        auto* architectural_splitter = new QSplitter(Qt::Horizontal, architectural_body);
        architectural_splitter->setChildrenCollapsible(true);
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
            unavailable->setAlignment(Qt::AlignCenter);
            unavailable->setWordWrap(true);
            unavailable->setStyleSheet(QStringLiteral("color:#9aa8ba; padding:24px;"));
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
        m_inspector->setMinimumWidth(290);
        m_inspector->setMaximumWidth(330);
        auto* inspector_body = new QWidget(m_inspector);
        inspector_body->setMinimumWidth(0);
        inspector_body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        auto* inspector_layout = new QVBoxLayout(inspector_body);
        inspector_layout->setContentsMargins(10, 10, 10, 10);
        auto* heading = new QLabel(QStringLiteral("Inspector"), inspector_body);
        heading->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
        inspector_layout->addWidget(heading);
        m_inspector_context = new QLabel(inspector_body);
        m_inspector_context->setWordWrap(true);
        inspector_layout->addWidget(m_inspector_context);
        m_edit_object_button = new QPushButton(QStringLiteral("Edit object…"), inspector_body);
        m_edit_object_button->setObjectName(QStringLiteral("editBuildingObject"));
        inspector_layout->addWidget(m_edit_object_button);
        QObject::connect(m_edit_object_button, &QPushButton::clicked, owner,
                         [this] { showBuildingObjectDialog(true); });
        m_delete_annotation_button = new QPushButton(QStringLiteral("Delete annotation"), inspector_body);
        m_delete_annotation_button->setObjectName(QStringLiteral("deleteAnnotation"));
        m_delete_annotation_button->setVisible(false);
        inspector_layout->addWidget(m_delete_annotation_button);
        QObject::connect(m_delete_annotation_button, &QPushButton::clicked, owner,
                         [this] { (void)deleteAnnotation(m_selected_id); });
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
        owner->statusBar()->showMessage(QStringLiteral("Ready  •  select a tool to begin"));
    }

    QToolButton* addToolButton(QVBoxLayout* layout, const QString& label, CanvasTool tool,
                               bool checked = false) {
        auto* button = new QToolButton(layout->parentWidget());
        button->setText(label);
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
        all_geometry.reserve(snapshot.entities().size());
        const auto append_geometry_error = [&](const QString& message) {
            if (!m_plan_geometry_error.isEmpty()) {
                m_plan_geometry_error += QLatin1Char('\n');
            }
            m_plan_geometry_error += message;
        };
        std::map<std::string, std::vector<HostedOpening>, std::less<>> openings_by_wall;
        for (const auto& [id, entity] : snapshot.entities()) {
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
                                          id_from(label.id) == m_selected_id});
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

    void refreshNavigator() {
        std::map<QString, bool> expansion;
        for (QTreeWidgetItemIterator item(m_navigator); *item; ++item)
            expansion[(*item)->data(0, Qt::UserRole).toString()] = (*item)->isExpanded();
        const QSignalBlocker tree_blocker(m_navigator);
        m_navigator->clear();
        const auto snapshot = m_document->snapshot();
        const auto organization = organize_project(snapshot);
        const auto visible_ids = visible_project_entities(snapshot, m_view_filter);
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
                ? QStringLiteral("View only: unchecked floors/layers hide plan and 3D; view filters do not change totals.")
                : QStringLiteral("View filter active: hidden floors/layers affect plan and 3D; view filters do not change totals."));
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
            std::vector<MeasurementArea> areas;
            areas.reserve(entities.size());
            for (const auto& [id, entity] : entities) {
                if (!is_closed_boundary_entity(entity.type)) {
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
                areas.push_back(MeasurementArea{id,
                                                *building_id,
                                                *floor_id,
                                                *entity_classification,
                                                boundary,
                                                {},
                                                stored_factor.rational});
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
                        annotation_context = QStringLiteral("Label\n%1")
                            .arg(QString::fromStdString(label->content));
                        break;
                    }
                    const auto symbol = std::find_if(
                        state.symbols.begin(), state.symbols.end(),
                        [&](const auto& value) { return value.id == wanted; });
                    if (symbol != state.symbols.end()) {
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
        const bool slab = entity.has_value() && entity->type == "slab";
        const bool building_object = entity && can_recognize_building_entity_type(entity->type);
        const bool editable_geometry = wall || opening || slab ||
            (entity && is_closed_boundary_entity(entity->type));
        m_edit_object_button->setVisible(building_object);
        m_edit_object_button->setEnabled(editable && building_object);
        m_delete_annotation_button->setVisible(annotation_context.has_value());
        m_delete_annotation_button->setEnabled(editable && annotation_context.has_value());
        if (annotation_context.has_value()) {
            m_inspector_context->setText(*annotation_context);
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
        }
        m_inspector_context->setText(context);
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
        auto title = QStringLiteral("Property Studio — Internal checkpoint");
        if (!m_file_path.empty()) {
            title += QStringLiteral("  •  ") +
                     QString::fromStdWString(m_file_path.filename().wstring());
        } else {
            title += QStringLiteral("  •  Untitled");
        }
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
            if (m_recovery_ledger.empty()) {
                (void)apply_boundary_commit(*m_document, preview);
            } else {
                requireWorkspaceDocument();
                auto edit = m_project_workspace->prepare_boundary_commit(preview);
                commitWorkspaceEdit(edit);
            }
            m_selected_id = QString::fromStdString(preview.created_boundary_ids().front());
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

    void clearPreview() {
        m_measurementCanvas->clearPreview();
        m_architecturalCanvas->clearPreview();
        m_boundary_session.reset();
        m_boundary_source.reset();
        m_boundary_context.reset();
        m_boundary_document.reset();
        m_pending_wall_start.reset();
    }

    void openFromDialog() {
        const auto selected = QFileDialog::getOpenFileName(
            owner, QStringLiteral("Open project"), {}, QStringLiteral("Property Studio project (*.bldproj)"));
        if (!selected.isEmpty()) {
            openProject(selected);
        }
    }

    void recoverFromDialog() {
        const auto directory = filesystem_path(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)) / "recovery";
        const auto discovered = discover_recovery_copies(std::nullopt, directory);
        if (!discovered.directory_diagnostic.empty()) {
            setError(QStringLiteral("Recovery discovery failed: %1")
                         .arg(QString::fromUtf8(discovered.directory_diagnostic.c_str())));
            return;
        }
        QStringList choices;
        std::vector<QString> paths;
        for (const auto& candidate : discovered.candidates) {
            if (!candidate.loadable || candidate.duplicate_archive_id) continue;
            auto label = QString::fromStdWString(candidate.path.wstring());
            if (candidate.metadata) {
                label += QStringLiteral("  [session %1]")
                             .arg(QString::fromStdString(candidate.metadata->archive_id));
            }
            choices.push_back(label);
            paths.push_back(QString::fromStdWString(candidate.path.wstring()));
        }
        if (choices.isEmpty()) {
            QMessageBox::information(owner, QStringLiteral("Recover project"),
                                     QStringLiteral("No valid local recovery copies were found."));
            return;
        }
        bool accepted = false;
        const auto selected = QInputDialog::getItem(
            owner, QStringLiteral("Recover project"),
            QStringLiteral("Choose a local recovery copy:"), choices, 0, false, &accepted);
        if (!accepted || selected.isEmpty()) return;
        const auto index = choices.indexOf(selected);
        if (index >= 0 && index < static_cast<int>(paths.size())) openProject(paths[index]);
    }

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

    void createOpeningFromDialog(const QString& kind) {
        const auto context = captureModalContext();
        const auto wall = selectedEntity();
        if (!wall.has_value() || wall->type != "wall") {
            setError(QStringLiteral("Select a wall before creating a %1 opening.").arg(kind));
            return;
        }
        bool accepted = false;
        const auto offset = QInputDialog::getText(
            owner, QStringLiteral("Create %1 opening").arg(kind),
            QStringLiteral("Offset along wall:"), QLineEdit::Normal,
            m_metric_units ? QStringLiteral("0.75 m") : QStringLiteral("2 ft"), &accepted);
        if (!accepted) {
            return;
        }
        const auto width = QInputDialog::getText(
            owner, QStringLiteral("Create %1 opening").arg(kind),
            QStringLiteral("Opening width:"), QLineEdit::Normal,
            m_metric_units ? QStringLiteral("0.9 m") : QStringLiteral("3 ft"), &accepted);
        if (!accepted) {
            return;
        }
        const auto sill = QInputDialog::getText(
            owner, QStringLiteral("Create %1 opening").arg(kind),
            QStringLiteral("Sill height:"), QLineEdit::Normal,
            m_metric_units ? QStringLiteral("0 m") : QStringLiteral("0 in"), &accepted);
        if (!accepted) {
            return;
        }
        const auto height = QInputDialog::getText(
            owner, QStringLiteral("Create %1 opening").arg(kind),
            QStringLiteral("Opening height:"), QLineEdit::Normal,
            m_metric_units ? QStringLiteral("2.1 m") : QStringLiteral("7 ft"), &accepted);
        if (!accepted) {
            return;
        }
        if (!modalContextUnchanged(context)) return;
        (void)createHostedOpening(kind, offset, width, sill, height, context.revision);
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
    bool m_refreshing{false};
    ProjectViewFilter m_view_filter;
    QString m_active_layer_id;
    QComboBox* m_drawing_layer_combo{};
    QComboBox* m_pageSizeCombo{};
    QComboBox* m_architecturalViewCombo{};
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
    QString m_last_boundary_classification{QStringLiteral("measurement")};
    QToolButton* m_define_boundary_button{};
    std::optional<Vec2> m_pending_wall_start;
    BuildingViewKind m_architectural_view_kind{BuildingViewKind::plan};

    VisibilityTreeWidget* m_navigator{};
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
    QLabel* m_calculation_status{};
    QLabel* m_calculation_base_value{};
    QLabel* m_calculation_net_value{};
    QLabel* m_calculation_factored_value{};
    QLabel* m_calculation_perimeter_value{};
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
    QPushButton* m_constraint_button{};
    QToolButton* m_grid_button{};
    QToolButton* m_snap_button{};
    QToolButton* m_fit_button{};
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
    QAction* m_annotation_action{};
    QAction* m_schedule_action{};
    QAction* m_sheet_action{};
    QAction* m_viewport_action{};
    QAction* m_schedule_placement_action{};
    QAction* m_view_action{};
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

QString MainWindow::createAnnotationLabel(const QString& template_id, const QString& content,
                                          Vec2 position) {
    return m_impl->createAnnotationLabel(template_id, content, position);
}

QString MainWindow::createAnnotationSymbol(const QString& symbol_id, Vec2 position) {
    return m_impl->createAnnotationSymbol(symbol_id, position);
}

bool MainWindow::deleteAnnotation(const QString& annotation_id) {
    return m_impl->deleteAnnotation(annotation_id);
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
