#include "sketch/desktop/building_object_dialog.hpp"

#include "sketch/quantity.hpp"
#include "sketch/building_plan_projection.hpp"
#include "sketch/phase_roof_opening_edit.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sketch::desktop {
namespace {

using json = nlohmann::json;

constexpr double geometry_tolerance = 1e-7;
constexpr std::size_t maximum_risers = 10'000;
constexpr std::array<const char*, 4> roof_opening_keys{"x_m", "y_m", "width_m", "depth_m"};
constexpr std::array<std::optional<RoofOpeningQuantityInput> RoofOpeningUpsertIntent::*, 4>
    roof_opening_inputs{&RoofOpeningUpsertIntent::x, &RoofOpeningUpsertIntent::y,
                        &RoofOpeningUpsertIntent::width, &RoofOpeningUpsertIntent::depth};

struct FormInfo {
    std::string_view type;
    std::string_view form;
    std::string_view label;
};

constexpr std::array<FormInfo, 11> form_infos{{
    {"column", "rectangular_column", "Rectangular column"},
    {"column", "circular_column", "Circular column"},
    {"beam", "straight_beam", "Straight beam"},
    {"stair", "straight_stair_flight", "Straight stair flight"},
    {"stair", "multi_flight_stair", "Multi-flight stair"},
    {"railing", "straight_railing", "Straight railing"},
    {"railing", "stair_flight_railing", "Stair flight railing"},
    {"railing", "stair_landing_railing", "Stair landing railing"},
    {"roof", "sloped_roof_panel", "Sloped roof panel"},
    {"roof", "gable_roof", "Gable roof"},
    {"roof", "hip_roof", "Hip roof"},
}};

QString qt_string(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString display_number(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    if (value == 0.0) {
        return QStringLiteral("0");
    }
    // Shortest fixed representation that round-trips to this exact double.
    // Fixed notation also stays within the quantity parser's input grammar.
    std::array<char, 768> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                         value, std::chars_format::fixed);
    if (converted.ec != std::errc{})
        throw std::runtime_error("Could not format the object dimension");
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(converted.ptr - buffer.data()));
}

QString display_scalar(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    return QString::number(value, 'g', 17);
}

QString display_length(double metres, bool metric) {
    const auto si_text = display_number(metres) + QStringLiteral(" m");
    try {
        // Route the unit conversion through the central exact quantity parser.
        // The original semantic value is retained separately for edits, so a
        // rounded display never rewrites an untouched field. Keep generated
        // defaults concise; an explicit SI suffix is preferable to a long
        // converted decimal or an unfamiliar generated fraction.
        const auto canonical = parse_quantity(si_text.toStdString(), Unit::metre);
        if (metric) {
            return si_text;
        }

        const auto displayed = quantity_value(canonical, Unit::foot);
        if (std::isfinite(displayed)) {
            const auto direct = display_number(displayed) + QStringLiteral(" ft");
            try {
                const auto round_trip = parse_quantity(direct.toStdString(), Unit::foot);
                if (direct.size() <= 14 && round_trip.metres == canonical.metres) {
                    return direct;
                }
            } catch (const std::exception&) {
                // Retain the explicit SI value below.
            }
        }

    } catch (const std::exception&) {
        // Fall through to a readable SI value. Geometry validation still owns
        // the supported numeric range at submission time.
    }
    return si_text;
}

QString display_angle(double radians) {
    if (!std::isfinite(radians)) {
        return {};
    }
    return display_number(radians * 180.0 / std::numbers::pi);
}

QString display_pitch(double radians) {
    if (!std::isfinite(radians)) {
        return QStringLiteral("—");
    }
    return QString::number(radians * 180.0 / std::numbers::pi, 'f', 3) +
           QStringLiteral("°");
}

const FormInfo* form_info(std::string_view form) {
    const auto found = std::find_if(form_infos.begin(), form_infos.end(),
                                    [form](const FormInfo& info) {
                                        return info.form == form;
                                    });
    return found == form_infos.end() ? nullptr : &*found;
}

std::string form_string(const QString& value) {
    return value.toStdString();
}

struct QuantityReceipt {
    Quantity quantity;
    json raw;
};

std::optional<std::int64_t> json_int64(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    try {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(unsigned_value);
        }
        return value.get<std::int64_t>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Unit> unit_from_receipt(std::string_view value) {
    if (value == "m") {
        return Unit::metre;
    }
    if (value == "mm") {
        return Unit::millimetre;
    }
    if (value == "cm") {
        return Unit::centimetre;
    }
    if (value == "ft") {
        return Unit::foot;
    }
    if (value == "in") {
        return Unit::inch;
    }
    return std::nullopt;
}

std::string receipt_unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    return {};
}

QString display_receipt_expression(const Quantity& quantity, bool metric) {
    auto expression = qt_string(quantity.original_expression);
    const auto default_unit = metric ? Unit::metre : Unit::foot;
    if (quantity.entered_unit == default_unit) {
        return expression;
    }

    try {
        // A suffixless expression is interpreted using the dialog's current
        // default. Add the recorded unit only in that ambiguous case; an
        // explicit suffix already tells the user how the value is interpreted.
        const auto interpreted = parse_quantity(quantity.original_expression, default_unit);
        if (interpreted.entered_unit == default_unit) {
            expression += QStringLiteral(" ") +
                          qt_string(receipt_unit_name(quantity.entered_unit));
        }
    } catch (const std::exception&) {
        // The receipt was already validated. If this defensive lexical check
        // cannot classify the expression, preserve the original text verbatim.
    }
    return expression;
}

json quantity_receipt_json(const Quantity& quantity) {
    return json{{"version", 1},
                {"original_expression", quantity.original_expression},
                {"entered_unit", receipt_unit_name(quantity.entered_unit)},
                {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                                   {"denominator", quantity.exact_metres.denominator}}}};
}

Quantity exact_roof_opening_default(double metres, Unit default_unit) {
    if (std::isfinite(metres)) {
        std::array<char, 768> buffer{};
        for (int precision = 0; precision <= 18; ++precision) {
            const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                 metres, std::chars_format::fixed, precision);
            if (converted.ec != std::errc{}) continue;
            try {
                auto quantity = parse_quantity(std::string(buffer.data(), converted.ptr) + " m", default_unit);
                if (quantity.metres == metres) return quantity;
            } catch (const std::exception&) {
                // Try the next bounded fixed decimal; never accept rounded geometry.
            }
        }
    }
    throw std::invalid_argument("The new roof opening dimension has no supported exact input.");
}

bool known_roof_opening_receipt(const json& value) {
    if (!value.is_object()) return false;
    const auto version = value.find("version");
    return version == value.end() ||
        (version->is_number_integer() && *version == 1);
}

std::optional<QuantityReceipt> decode_quantity_receipt(const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    try {
        const auto version = value.find("version");
        const auto expression = value.find("original_expression");
        const auto entered_unit = value.find("entered_unit");
        const auto exact_metres = value.find("exact_metres");
        if (version == value.end() || expression == value.end() ||
            entered_unit == value.end() || exact_metres == value.end() ||
            !expression->is_string() || !entered_unit->is_string() ||
            !exact_metres->is_object()) {
            return std::nullopt;
        }
        const auto version_number = json_int64(*version);
        if (!version_number.has_value() || *version_number != 1) {
            return std::nullopt;
        }
        const auto expression_text = expression->get<std::string>();
        const auto unit = unit_from_receipt(entered_unit->get<std::string>());
        if (!unit.has_value()) {
            return std::nullopt;
        }
        const auto numerator = exact_metres->find("numerator");
        const auto denominator = exact_metres->find("denominator");
        if (numerator == exact_metres->end() || denominator == exact_metres->end()) {
            return std::nullopt;
        }
        const auto numerator_value = json_int64(*numerator);
        const auto denominator_value = json_int64(*denominator);
        if (!numerator_value.has_value() || !denominator_value.has_value() ||
            *denominator_value <= 0) {
            return std::nullopt;
        }
        const ExactRational exact{*numerator_value, *denominator_value};
        const auto parsed = parse_quantity(expression_text, *unit);
        if (parsed.entered_unit != *unit || parsed.exact_metres != exact) {
            return std::nullopt;
        }
        return QuantityReceipt{parsed, value};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const json* json_at_pointer(const json& object, std::string_view pointer) {
    try {
        const auto path = json::json_pointer(std::string(pointer));
        return &object.at(path);
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::optional<QuantityReceipt> receipt_for_value(const json& value, double authoritative) {
    if (!std::isfinite(authoritative)) {
        return std::nullopt;
    }
    const auto receipt = decode_quantity_receipt(value);
    if (!receipt.has_value() || receipt->quantity.metres != authoritative) {
        return std::nullopt;
    }
    return receipt;
}

}  // namespace

class BuildingObjectDialog::Impl final {
public:
    Impl(BuildingObjectDialog* dialog, std::optional<Entity> original,
         bool metric_units, std::optional<DocumentSnapshot> source = std::nullopt)
        : owner(dialog), original_entity(std::move(original)), source_snapshot(std::move(source)), metric(metric_units) {
        setup();
    }

    [[nodiscard]] std::optional<Entity> candidate() const { return candidate_entity; }
    [[nodiscard]] std::vector<Entity> related_candidates() const { return related_entities; }

    [[nodiscard]] QString last_error() const { return error; }

    bool submit() {
        candidate_entity.reset();
        related_entities.clear();
        parsed_quantities.clear();
        if (original_invalid) {
            return false;
        }
        clear_error();

        try {
            const auto object = read_object();
            if (!object.has_value()) {
                return false;
            }
            std::vector<Entity> upgrades;
            if (const auto* rail = std::get_if<Railing>(&*object); rail && (rail->host || rail->landing_host)) {
                if (!source_snapshot) throw std::invalid_argument("A current document is required for hosted railings.");
                const auto& stair_id = rail->host ? rail->host->stair_id : rail->landing_host->stair_id;
                auto entities = source_snapshot->entities();
                if (const auto found = upgraded_hosts.find(stair_id); rail->host && found != upgraded_hosts.end()) {
                    entities.insert_or_assign(found->first, found->second);
                    upgrades.push_back(found->second);
                }
                const auto found = entities.find(stair_id);
                if (found == entities.end()) throw std::invalid_argument("The selected stair host is unavailable.");
                (void)make_building_shape(*object, entities);
            }
            const bool opening_only_roof = roof_geometry_unchanged(*object);
            if (opening_only_roof) {
                // Start from the actual source, including its numeric wire forms
                // and opaque metadata. The table's typed inputs own only the roster.
                candidate_entity = *original_entity;
                if (!roof_opening_intent.upserts.empty() || !roof_opening_intent.removed_opening_ids.empty())
                    candidate_entity = replay_roof_opening_entity(*original_entity, roof_opening_intent);
            } else if (original_entity.has_value()) {
                const auto canonical = encode_building_entity(*object,
                                                              original_entity->extensions);
                auto merged = *original_entity;
                merged.type = canonical.type;
                merged.properties.update(canonical.properties);
                if (merged.type == "stair" &&
                    canonical.properties.value("form", std::string{}) == "multi_flight_stair" &&
                    original_entity->properties.value("version", 0) == 4)
                    merged.properties["version"] = 4;
                preserve_stair_child_metadata(merged.properties, canonical.properties);
                for (const auto* key : {"top_landing", "level_connection", "host"}) {
                    if (canonical.properties.contains(key) && canonical.properties.at(key).is_object() &&
                        original_entity->properties.contains(key) && original_entity->properties.at(key).is_object()) {
                        auto retained = original_entity->properties.at(key);
                        retained.update(canonical.properties.at(key));
                        merged.properties[key] = std::move(retained);
                    }
                }
                if (merged.type == "stair" && !canonical.properties.contains("level_connection"))
                    merged.properties.erase("level_connection");
                if (merged.type == "railing" && canonical.properties.contains("host")) {
                    for (const auto* key : {"base_position_m", "orientation_rad", "length_m"}) merged.properties.erase(key);
                }
                if (merged.type == "roof" && canonical.properties.contains("roof_openings") &&
                    original_entity->properties.contains("roof_openings") &&
                    original_entity->properties.value("form", std::string{}) == canonical.properties.value("form", std::string{})) {
                    for (auto& row : merged.properties.at("roof_openings")) {
                        for (const auto& source : original_entity->properties.at("roof_openings")) {
                            if (row.at("id") != source.at("id")) continue;
                            const bool unchanged = std::all_of(roof_opening_keys.begin(), roof_opening_keys.end(),
                                [&](const auto* key) { return row.at(key) == source.at(key); });
                            if (unchanged) row = source;
                            break;
                        }
                    }
                }
                if (merged.type == "roof" && !canonical.properties.contains("roof_openings")) {
                    const auto& source = original_entity->properties;
                    if (source.value("form", std::string{}) == canonical.properties.value("form", std::string{}) &&
                        source.value("version", json{}) == 2 && source.contains("roof_openings") &&
                        source.at("roof_openings").is_array() && source.at("roof_openings").empty()) {
                        merged.properties["version"] = 2;
                        merged.properties["roof_openings"] = source.at("roof_openings");
                    } else {
                        merged.properties.erase("roof_openings");
                    }
                }
                apply_quantity_entries(merged.properties, canonical.properties);
                candidate_entity = std::move(merged);
            } else {
                candidate_entity = encode_building_entity(*object);
                apply_quantity_entries(candidate_entity->properties,
                                       candidate_entity->properties);
            }
            if (!opening_only_roof && candidate_entity->type == "roof" && roof_openings_changed) {
                auto envelope = json::object();
                if (original_entity && original_entity->extensions.contains("roof_opening_input")) {
                    envelope = original_entity->extensions.at("roof_opening_input");
                    if (!envelope.is_object() || envelope.value("version", json{}) != 1 ||
                        !envelope.contains("entries") || !envelope.at("entries").is_object())
                        throw std::invalid_argument("The roof opening input has an unsupported receipt version.");
                }
                envelope["version"] = 1;
                envelope["entries"] = roof_opening_receipts;
                candidate_entity->extensions["roof_opening_input"] = std::move(envelope);
            }
            clear_error();
            related_entities = std::move(upgrades);
            owner->accept();
            return true;
        } catch (const std::exception& caught) {
            candidate_entity.reset();
            related_entities.clear();
            fail(QStringLiteral("%1").arg(QString::fromUtf8(caught.what())));
            return false;
        }
    }

private:
    void setup() {
        owner->setObjectName(QStringLiteral("buildingObjectDialog"));
        owner->setModal(true);
        owner->setMinimumSize(360, 360);
        owner->resize(480, 560);
        owner->setWindowTitle(original_entity.has_value()
                                  ? QStringLiteral("Edit building object")
                                  : QStringLiteral("Create building object"));

        auto* root = new QVBoxLayout(owner);
        root->setContentsMargins(12, 10, 12, 10);
        root->setSpacing(8);

        auto* heading = new QLabel(
            QStringLiteral("Dimensions and placement"),
            owner);
        heading->setObjectName(QStringLiteral("buildingObjectHeading"));
        heading->setWordWrap(true);
        root->addWidget(heading);

        auto* selector = new QFormLayout;
        selector->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        selector->setRowWrapPolicy(QFormLayout::WrapLongRows);
        type_combo = new QComboBox(owner);
        type_combo->setObjectName(QStringLiteral("buildingObjectType"));
        type_combo->setMinimumWidth(0);
        type_combo->addItem(QStringLiteral("Column"), QStringLiteral("column"));
        type_combo->addItem(QStringLiteral("Beam"), QStringLiteral("beam"));
        type_combo->addItem(QStringLiteral("Stair"), QStringLiteral("stair"));
        type_combo->addItem(QStringLiteral("Railing"), QStringLiteral("railing"));
        type_combo->addItem(QStringLiteral("Roof"), QStringLiteral("roof"));
        selector->addRow(QStringLiteral("Type"), type_combo);

        form_combo = new QComboBox(owner);
        form_combo->setObjectName(QStringLiteral("buildingObjectForm"));
        form_combo->setMinimumWidth(0);
        selector->addRow(QStringLiteral("Form"), form_combo);
        root->addLayout(selector);

        error_label = new QLabel(owner);
        error_label->setObjectName(QStringLiteral("buildingObjectError"));
        error_label->setWordWrap(true);
        error_label->setStyleSheet(QStringLiteral("color:#b44b4b;"));
        error_label->setVisible(false);
        root->addWidget(error_label);

        scroll = new QScrollArea(owner);
        scroll->setObjectName(QStringLiteral("buildingObjectScrollArea"));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        form_body = new QWidget(scroll);
        form_body->setObjectName(QStringLiteral("buildingObjectFormBody"));
        auto* form_body_layout = new QVBoxLayout(form_body);
        form_body_layout->setContentsMargins(2, 2, 2, 2);
        form_body_layout->setSpacing(4);
        form_stack = new QStackedWidget(form_body);
        form_stack->setObjectName(QStringLiteral("buildingObjectFormStack"));
        form_body_layout->addWidget(form_stack);
        setup_roof_openings(form_body_layout);
        scroll->setWidget(form_body);
        root->addWidget(scroll, 1);

        buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                       owner);
        buttons->setObjectName(QStringLiteral("buildingObjectButtons"));
        submit_button = buttons->button(QDialogButtonBox::Ok);
        submit_button->setObjectName(QStringLiteral("buildingObjectSubmit"));
        submit_button->setText(original_entity.has_value() ? QStringLiteral("Apply")
                                                            : QStringLiteral("Create"));
        submit_button->setDefault(true);
        root->addWidget(buttons);

        QObject::connect(type_combo,
                         qOverload<int>(&QComboBox::currentIndexChanged), owner,
                         [this](int) {
                             if (!loading) {
                                 populate_forms();
                             }
                         });
        QObject::connect(form_combo,
                         qOverload<int>(&QComboBox::currentIndexChanged), owner,
                         [this](int) {
                             if (!loading) {
                                 rebuild_form();
                             }
                         });
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner,
                         [this] { submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner,
                         [this] { owner->reject(); });
        QObject::connect(owner, &QDialog::rejected, owner, [this] { invalidate_candidate(); });

        if (original_entity.has_value()) {
            initialize_edit();
        } else {
            loading = true;
            type_combo->setCurrentIndex(0);
            loading = false;
            populate_forms();
        }
    }

    void initialize_edit() {
        try {
            if (!original_entity->extensions.is_object()) {
                throw std::invalid_argument("Original building metadata must be a JSON object.");
            }
            load_original_quantity_entries();
            original_object = decode_building_entity(*original_entity);
            populate_roof_openings();
            const auto type = original_entity->type;
            const auto form = std::visit(
                [](const auto& value) -> std::string_view {
                    using Object = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Object, RectangularColumn>) {
                        return "rectangular_column";
                    } else if constexpr (std::is_same_v<Object, CircularColumn>) {
                        return "circular_column";
                    } else if constexpr (std::is_same_v<Object, Beam>) {
                        return "straight_beam";
                    } else if constexpr (std::is_same_v<Object, StairFlight>) {
                        return value.flights.empty() ? "straight_stair_flight" : "multi_flight_stair";
                    } else if constexpr (std::is_same_v<Object, Railing>) {
                        return value.landing_host ? "stair_landing_railing" : value.host ? "stair_flight_railing" : "straight_railing";
                    } else if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
                        return "sloped_roof_panel";
                    } else if constexpr (std::is_same_v<Object, GableRoof>) {
                        return "gable_roof";
                    } else {
                        return "hip_roof";
                    }
                },
                *original_object);
            if (form_info(form) == nullptr || form_info(form)->type != type) {
                throw std::invalid_argument("Original building form is not supported.");
            }

            loading = true;
            const auto type_index = type_combo->findData(qt_string(type));
            if (type_index < 0) {
                throw std::invalid_argument("Original building type is not supported.");
            }
            type_combo->setCurrentIndex(type_index);
            loading = false;
            populate_forms();
            const auto form_index = form_combo->findData(qt_string(form));
            if (form_index < 0) {
                throw std::invalid_argument("Original building form is not supported.");
            }
            loading = true;
            form_combo->setCurrentIndex(form_index);
            loading = false;
            rebuild_form();
            type_combo->setEnabled(false);
            form_combo->setEnabled(false);
        } catch (const std::exception& caught) {
            original_invalid = true;
            loading = false;
            disable_editor();
            fail(QStringLiteral("Cannot edit building object: %1")
                     .arg(QString::fromUtf8(caught.what())));
        }
    }

    void load_original_quantity_entries() {
        original_quantity_entries.clear();
        if (!original_entity.has_value() || !original_entity->properties.is_object()) {
            return;
        }
        const auto found = original_entity->properties.find("quantity_entries");
        if (found == original_entity->properties.end() || !found->is_object()) {
            return;
        }
        for (const auto& [pointer, receipt] : found->items()) {
            original_quantity_entries.emplace(pointer, receipt);
        }
    }

    void disable_editor() {
        type_combo->setEnabled(false);
        form_combo->setEnabled(false);
        scroll->setEnabled(false);
        if (submit_button != nullptr) {
            submit_button->setEnabled(false);
        }
    }

    void populate_forms() {
        const auto type = type_combo->currentData().toString().toStdString();
        {
            const QSignalBlocker blocker(*form_combo);
            form_combo->clear();
            for (const auto& info : form_infos) {
                if (info.type == type) {
                    form_combo->addItem(qt_string(info.label), qt_string(info.form));
                }
            }
        }
        rebuild_form();
    }

    void rebuild_form() {
        invalidate_candidate();
        fields.clear();
        quantity_pointers.clear();
        dirty.clear();
        parsed_quantities.clear();
        landing_check = nullptr;
        level_connection_check = nullptr;
        derived_pitch = nullptr;
        stair_flights = nullptr;
        stair_landings = nullptr;
        stair_summary = nullptr;
        host_combo = nullptr;
        host_flight_combo = nullptr;
        host_side_combo = nullptr;
        host_landing_combo = nullptr;
        host_edge_combo = nullptr;
        host_interval_combo = nullptr;
        landing_summary = nullptr;
        railing_preview = nullptr;
        railing_preview_status = nullptr;
        landing_choices.clear();
        landing_intervals.clear();
        if (form_page != nullptr) {
            form_stack->removeWidget(form_page);
            delete form_page;
            form_page = nullptr;
        }
        form_page = new QWidget(form_stack);
        form_page->setObjectName(QStringLiteral("buildingObjectFormPage"));
        auto* layout = new QFormLayout(form_page);
        layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
        layout->setHorizontalSpacing(8);
        layout->setVerticalSpacing(4);

        const auto form = form_string(form_combo->currentData().toString());
        if (form == "rectangular_column") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
            add_quantity_field(layout, "Width", "buildingObjectWidth", 0.4);
            add_quantity_field(layout, "Depth", "buildingObjectDepth", 0.4);
            add_quantity_field(layout, "Height", "buildingObjectHeight", 3.0);
            add_angle_field(layout, "Rotation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
        } else if (form == "circular_column") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
            add_quantity_field(layout, "Radius", "buildingObjectRadius", 0.2);
            add_quantity_field(layout, "Height", "buildingObjectHeight", 3.0);
        } else if (form == "straight_beam") {
            add_coordinate_fields(layout, "Start", "buildingObjectStartX",
                                  "buildingObjectStartY", "buildingObjectStartZ", {
                                      0.0, 0.0, 2.4});
            add_coordinate_fields(layout, "End", "buildingObjectEndX",
                                  "buildingObjectEndY", "buildingObjectEndZ", {
                                      3.0, 0.0, 2.4});
            add_scalar_fields(layout, "Up direction", "buildingObjectUpX",
                              "buildingObjectUpY", "buildingObjectUpZ", {0.0, 0.0, 1.0});
            add_quantity_field(layout, "Width", "buildingObjectWidth", 0.2);
            add_quantity_field(layout, "Depth", "buildingObjectDepth", 0.3);
        } else if (form == "straight_stair_flight" || form == "multi_flight_stair") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
            add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
            add_integer_field(layout, "Risers", "buildingObjectRiserCount", 10);
            if (form == "multi_flight_stair") {
                fields.at("buildingObjectRiserCount")->setReadOnly(true);
                setup_stair_topology(layout);
            }
            add_quantity_field(layout, "Total rise", "buildingObjectTotalRise", 2.5);
            add_quantity_field(layout, form == "multi_flight_stair" ? "Default going" : "Going", "buildingObjectGoing", 0.25);
            add_quantity_field(layout, form == "multi_flight_stair" ? "Default width" : "Width", "buildingObjectWidth", 1.2);
            landing_check = new QCheckBox(QStringLiteral("Add top landing"), form_page);
            landing_check->setObjectName(QStringLiteral("buildingObjectLandingEnabled"));
            layout->addRow(QString(), landing_check);
            add_quantity_field(layout, "Landing depth", "buildingObjectLandingDepth", 1.2);
            add_quantity_field(layout, "Landing thickness", "buildingObjectLandingThickness",
                               0.15);
            QObject::connect(landing_check, &QCheckBox::toggled, owner,
                             [this](bool enabled) {
                                 if (!loading) {
                                     dirty["buildingObjectLandingEnabled"] = true;
                                 }
                                 if (fields.contains("buildingObjectLandingDepth")) {
                                     fields.at("buildingObjectLandingDepth")
                                         ->setEnabled(enabled);
                                 }
                                 if (fields.contains("buildingObjectLandingThickness")) {
                                     fields.at("buildingObjectLandingThickness")
                                         ->setEnabled(enabled);
                                 }
                             });
            fields.at("buildingObjectLandingDepth")->setEnabled(false);
            fields.at("buildingObjectLandingThickness")->setEnabled(false);

            level_connection_check = new QCheckBox(
                QStringLiteral("Connect to floor-to-floor levels"), form_page);
            level_connection_check->setObjectName(
                QStringLiteral("buildingObjectLevelConnectionEnabled"));
            layout->addRow(QString(), level_connection_check);
            add_identifier_field(layout, QStringLiteral("Level graph"),
                                 "buildingObjectLevelGraph");
            add_identifier_field(layout, QStringLiteral("Floor-to-floor link"),
                                 "buildingObjectLevelLink");
            add_identifier_field(layout, QStringLiteral("Lower level"),
                                 "buildingObjectLowerLevel");
            add_identifier_field(layout, QStringLiteral("Upper level"),
                                 "buildingObjectUpperLevel");
            QObject::connect(level_connection_check, &QCheckBox::toggled, owner,
                             [this](bool enabled) {
                                 if (!loading) {
                                     dirty["buildingObjectLevelConnectionEnabled"] = true;
                                 }
                                 for (const auto* name : {"buildingObjectLevelGraph",
                                                          "buildingObjectLevelLink",
                                                          "buildingObjectLowerLevel",
                                                          "buildingObjectUpperLevel"}) {
                                     if (fields.contains(name)) {
                                         fields.at(name)->setEnabled(enabled);
                                     }
                                 }
                             });
            for (const auto* name : {"buildingObjectLevelGraph", "buildingObjectLevelLink",
                                     "buildingObjectLowerLevel", "buildingObjectUpperLevel"}) {
                fields.at(name)->setEnabled(false);
            }
            if (stair_summary) {
                for (const auto* name : {"buildingObjectTotalRise", "buildingObjectGoing", "buildingObjectWidth"})
                    QObject::connect(fields.at(name), &QLineEdit::textChanged, owner, [this] { refresh_stair_summary(); });
            }
        } else if (form == "straight_railing" || form == "stair_flight_railing" || form == "stair_landing_railing") {
            if (form == "straight_railing") {
                add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
                add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
                add_quantity_field(layout, "Length", "buildingObjectLength", 3.0);
            } else if (form == "stair_landing_railing") setup_landing_railing_host(layout);
            else setup_railing_host(layout);
            add_quantity_field(layout, "Height", "buildingObjectHeight", 1.1);
            add_quantity_field(layout, "Thickness", "buildingObjectThickness", 0.08);
            add_quantity_field(layout, "Maximum post spacing", "buildingObjectPostSpacing", 0.9);
            if (form == "stair_landing_railing") setup_railing_preview(layout);
        } else if (form == "sloped_roof_panel") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {
                                      0.0, 0.0, 3.0});
            add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
            add_quantity_field(layout, "Run", "buildingObjectRun", 4.0);
            add_quantity_field(layout, "Span", "buildingObjectSpan", 3.0);
            add_quantity_field(layout, "Rise", "buildingObjectRise", 1.0);
            derived_pitch = new QLabel(form_page);
            derived_pitch->setObjectName(QStringLiteral("buildingObjectDerivedPitch"));
            derived_pitch->setText(QStringLiteral("—"));
            layout->addRow(QStringLiteral("Pitch (derived)"), derived_pitch);
            add_quantity_field(layout, "Overhang", "buildingObjectOverhang", 0.2,
                               false);
            add_quantity_field(layout, "Thickness", "buildingObjectThickness", 0.1);
            connect_pitch_updates();
        } else if (form == "gable_roof" || form == "hip_roof") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {
                                      0.0, 0.0, 3.0});
            add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
            add_quantity_field(layout, "Length", "buildingObjectLength", 5.0);
            add_quantity_field(layout, "Span", "buildingObjectSpan", 4.0);
            add_quantity_field(layout, "Rise", "buildingObjectRise", 1.0);
            derived_pitch = new QLabel(form_page);
            derived_pitch->setObjectName(QStringLiteral("buildingObjectDerivedPitch"));
            derived_pitch->setText(QStringLiteral("—"));
            layout->addRow(QStringLiteral("Pitch (derived)"), derived_pitch);
            add_quantity_field(layout, "Overhang", "buildingObjectOverhang", 0.2,
                               false);
            add_quantity_field(layout, "Thickness", "buildingObjectThickness", 0.1);
            connect_pitch_updates();
        }

        form_stack->addWidget(form_page);
        form_stack->setCurrentWidget(form_page);
        if (original_object.has_value()) {
            populate_from_original();
        }
        refresh_pitch();
        refresh_stair_summary();
        // Changes to any form control invalidate a previously submitted candidate
        // and any geometry preview. The captured document remains immutable.
        for (auto* edit : form_page->findChildren<QLineEdit*>())
            QObject::connect(edit, &QLineEdit::textChanged, owner, [this] { if (!loading) invalidate_candidate(); });
        for (auto* box : form_page->findChildren<QComboBox*>())
            QObject::connect(box, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { if (!loading) invalidate_candidate(); });
        for (auto* check : form_page->findChildren<QCheckBox*>())
            QObject::connect(check, &QCheckBox::toggled, owner, [this] { if (!loading) invalidate_candidate(); });
        for (auto* table : form_page->findChildren<QTableWidget*>())
            QObject::connect(table, &QTableWidget::itemChanged, owner, [this] { if (!loading) invalidate_candidate(); });
        const bool roof = form_info(form) != nullptr && form_info(form)->type == "roof";
        roof_openings_group->setVisible(roof || roof_openings_table->rowCount() != 0);
        roof_openings_help->setText(roof
            ? (form == "sloped_roof_panel"
                ? QStringLiteral("Opening corner X/Y relative to the roof base. Width/depth are plan dimensions.")
                : QStringLiteral("Opening corner X/Y relative to the roof centre. Width/depth are plan dimensions."))
            : QStringLiteral("This form cannot contain roof openings. Remove them explicitly or choose a roof form."));
    }

    void preserve_stair_child_metadata(json& merged, const json& canonical) const {
        if (!original_entity || original_entity->type != "stair") return;
        for (const auto* key : {"flights", "landings"}) {
            if (!canonical.contains(key) || !original_entity->properties.contains(key)) continue;
            const auto& old = original_entity->properties.at(key);
            if (!old.is_array()) continue;
            for (auto& child : merged.at(key)) {
                for (const auto& previous : old) {
                    if (previous.is_object() && previous.value("id", std::string{}) == child.value("id", std::string{})) {
                        auto retained = previous;
                        if (std::string_view(key) == "flights") {
                            for (const auto* dimension : {"going_m", "width_m"})
                                if (!child.contains(dimension)) retained.erase(dimension);
                        }
                        if (std::string_view(key) == "landings" && !child.contains("straight_alignment"))
                            retained.erase("straight_alignment");
                        retained.update(child);
                        child = std::move(retained);
                        break;
                    }
                }
            }
        }
    }

    static std::string row_id(QTableWidget* table, int row) {
        return table->item(row, 0)->data(Qt::UserRole).toString().toStdString();
    }

    void add_stair_flight_row(std::string id, std::size_t risers,
                              std::optional<double> going = std::nullopt,
                              std::optional<double> width = std::nullopt) {
        const QSignalBlocker blocker(stair_flights);
        const auto row = stair_flights->rowCount();
        stair_flights->insertRow(row);
        auto* cell = new QTableWidgetItem(QString::number(static_cast<qulonglong>(risers)));
        cell->setData(Qt::UserRole, qt_string(id));
        stair_flights->setItem(row, 0, cell);
        const std::array<std::optional<double>, 2> values{going, width};
        const std::array<const char*, 2> keys{"going_m", "width_m"};
        for (std::size_t i = 0; i < values.size(); ++i) {
            QString text;
            if (values[i]) {
                text = display_length(*values[i], metric);
                const auto pointer = "/flights/" + std::to_string(row) + "/" + keys[i];
                if (const auto receipt = original_quantity_entries.find(pointer); receipt != original_quantity_entries.end()) {
                    if (const auto decoded = receipt_for_value(receipt->second, *values[i]); decoded)
                        text = display_receipt_expression(decoded->quantity, metric);
                }
            }
            auto* dimension = new QTableWidgetItem(text);
            dimension->setData(Qt::UserRole + 1, text);
            if (values[i]) dimension->setData(Qt::UserRole + 2, *values[i]);
            dimension->setToolTip(QStringLiteral("Leave blank to use the stair default. Enter a length or expression with units, such as 0.3 m or 11 in."));
            stair_flights->setItem(row, static_cast<int>(i) + 1, dimension);
        }
    }

    void add_stair_landing_row(const StairConnectingLanding& landing) {
        const QSignalBlocker blocker(stair_landings);
        const auto row = stair_landings->rowCount();
        stair_landings->insertRow(row);
        const std::array<double, 3> values{landing.depth, landing.thickness, landing.return_gap};
        const std::array<int, 3> columns{0, 1, 3};
        const std::array<const char*, 3> keys{"depth_m", "thickness_m", "return_gap_m"};
        for (std::size_t i = 0; i < columns.size(); ++i) {
            auto text = display_length(values[i], metric);
            const auto pointer = "/landings/" + std::to_string(row) + "/" + keys[i];
            if (const auto receipt = original_quantity_entries.find(pointer); receipt != original_quantity_entries.end()) {
                if (const auto decoded = receipt_for_value(receipt->second, values[i]); decoded)
                    text = display_receipt_expression(decoded->quantity, metric);
            }
            auto* cell = new QTableWidgetItem(text);
            cell->setData(Qt::UserRole, qt_string(landing.id));
            cell->setData(Qt::UserRole + 1, text);
            cell->setData(Qt::UserRole + 2, values[i]);
            stair_landings->setItem(row, columns[i], cell);
        }
        auto* turns = new QComboBox(stair_landings);
        turns->addItem(QStringLiteral("Straight"), static_cast<int>(StairTurn::straight));
        turns->addItem(QStringLiteral("Left 90°"), static_cast<int>(StairTurn::left_quarter));
        turns->addItem(QStringLiteral("Right 90°"), static_cast<int>(StairTurn::right_quarter));
        turns->addItem(QStringLiteral("Left 180°"), static_cast<int>(StairTurn::left_half));
        turns->addItem(QStringLiteral("Right 180°"), static_cast<int>(StairTurn::right_half));
        turns->setCurrentIndex(turns->findData(static_cast<int>(landing.turn)));
        stair_landings->setCellWidget(row, 2, turns);
        auto* alignment = new QComboBox(stair_landings);
        // Flight width grows toward physical left when looking up the stair,
        // matching the hosted-railing side convention. The persisted alignment
        // flag refers to the far edge of that positive local-width interval.
        alignment->addItem(QStringLiteral("Right"), false);
        alignment->addItem(QStringLiteral("Left"), true);
        alignment->setCurrentIndex(alignment->findData(landing.align_right));
        alignment->setEnabled(landing.turn == StairTurn::straight);
        alignment->setToolTip(QStringLiteral("Align consecutive straight flights by their left or right edges, looking up the stair."));
        stair_landings->setCellWidget(row, 4, alignment);
        QObject::connect(turns, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this, turns, alignment] {
            const bool straight = static_cast<StairTurn>(turns->currentData().toInt()) == StairTurn::straight;
            alignment->setEnabled(straight);
            if (!straight) alignment->setCurrentIndex(0);
            invalidate_candidate();
            refresh_stair_summary();
        });
        QObject::connect(alignment, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] {
            invalidate_candidate();
            refresh_stair_summary();
        });
    }

    void setup_stair_topology(QFormLayout* layout) {
        stair_flights = new QTableWidget(0, 3, form_page);
        stair_flights->setObjectName(QStringLiteral("buildingObjectStairFlights"));
        stair_flights->setHorizontalHeaderLabels({QStringLiteral("Risers in flight"), QStringLiteral("Going (optional)"), QStringLiteral("Width (optional)")});
        stair_flights->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        stair_flights->setSelectionBehavior(QAbstractItemView::SelectRows);
        stair_flights->setSelectionMode(QAbstractItemView::SingleSelection);
        stair_flights->setMinimumHeight(110);
        layout->addRow(QStringLiteral("Ordered flights"), stair_flights);
        auto* actions = new QWidget(form_page);
        auto* action_layout = new QHBoxLayout(actions);
        action_layout->setContentsMargins(0, 0, 0, 0);
        const auto button = [&](QString title, const char* name, auto action) {
            auto* result = new QPushButton(title, actions);
            result->setObjectName(QString::fromLatin1(name));
            result->setAutoDefault(false);
            action_layout->addWidget(result);
            QObject::connect(result, &QPushButton::clicked, owner, action);
        };
        button(QStringLiteral("Add"), "buildingObjectAddStairFlight", [this] {
            add_stair_flight_row(make_stable_id(), 10);
            add_stair_landing_row({make_stable_id(), 1.2, 0.15, StairTurn::straight, 0});
            stair_flights->selectRow(stair_flights->rowCount() - 1);
            refresh_stair_summary();
        });
        button(QStringLiteral("Remove"), "buildingObjectRemoveStairFlight", [this] {
            const auto row = stair_flights->currentRow();
            if (row < 0 || stair_flights->rowCount() <= 1) return;
            const QSignalBlocker flights_blocker(stair_flights), landings_blocker(stair_landings);
            stair_flights->removeRow(row);
            stair_landings->removeRow(row == 0 ? 0 : row - 1);
            refresh_stair_summary();
        });
        const auto move = [this](int offset) {
            const auto row = stair_flights->currentRow();
            const auto target = row + offset;
            if (row < 0 || target < 0 || target >= stair_flights->rowCount()) return;
            const QSignalBlocker blocker(stair_flights);
            for (int column = 0; column < stair_flights->columnCount(); ++column) {
                auto* a = stair_flights->takeItem(row, column);
                auto* b = stair_flights->takeItem(target, column);
                stair_flights->setItem(row, column, b);
                stair_flights->setItem(target, column, a);
            }
            stair_flights->selectRow(target);
            refresh_stair_summary();
        };
        button(QStringLiteral("Up"), "buildingObjectMoveStairFlightUp", [move] { move(-1); });
        button(QStringLiteral("Down"), "buildingObjectMoveStairFlightDown", [move] { move(1); });
        layout->addRow(QString(), actions);
        stair_landings = new QTableWidget(0, 5, form_page);
        stair_landings->setObjectName(QStringLiteral("buildingObjectStairLandings"));
        stair_landings->setHorizontalHeaderLabels({QStringLiteral("Depth"), QStringLiteral("Thickness"), QStringLiteral("Turn"), QStringLiteral("Return gap"), QStringLiteral("Alignment")});
        stair_landings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        stair_landings->setMinimumHeight(110);
        layout->addRow(QStringLiteral("Connecting landings"), stair_landings);
        auto* help = new QLabel(QStringLiteral("Each flight has its own riser count and optional going and width. Leave a dimension blank to use the stair default below; enter lengths or expressions with units to override it. Landing 1 connects flights 1 and 2. Reordering moves whole flights and retains landing order. Return gap applies to 180° turns; other turns require zero."), form_page);
        help->setWordWrap(true);
        layout->addRow(help);
        stair_summary = new QLabel(form_page);
        stair_summary->setObjectName(QStringLiteral("buildingObjectStairSummary"));
        stair_summary->setWordWrap(true);
        layout->addRow(QStringLiteral("Derived summary"), stair_summary);
        add_stair_flight_row(make_stable_id(), 10);
        QObject::connect(stair_flights, &QTableWidget::itemChanged, owner, [this] { refresh_stair_summary(); });
        QObject::connect(stair_landings, &QTableWidget::itemChanged, owner, [this] { refresh_stair_summary(); });
    }

    void populate_stair_topology(const StairFlight& stair) {
        const QSignalBlocker flight_blocker(stair_flights), landing_blocker(stair_landings);
        stair_flights->setRowCount(0);
        stair_landings->setRowCount(0);
        for (const auto& flight : stair.flights) add_stair_flight_row(flight.id, flight.riser_count, flight.going, flight.width);
        for (const auto& landing : stair.landings) add_stair_landing_row(landing);
    }

    void remap_stair_child_receipts() {
        if (!original_entity || !original_entity->properties.contains("quantity_entries")) return;
        for (auto it = original_quantity_entries.begin(); it != original_quantity_entries.end();) {
            if (it->first.starts_with("/landings/") || it->first.starts_with("/flights/")) it = original_quantity_entries.erase(it);
            else ++it;
        }
        const auto* stair = original_as<StairFlight>();
        if (!stair) return;
        const auto& receipts = original_entity->properties.at("quantity_entries");
        if (!receipts.is_object()) return;
        const auto remap = [&](QTableWidget* table, const auto& children, const char* key) {
            for (int row = 0; row < table->rowCount(); ++row) {
                const auto id = row_id(table, row);
                for (std::size_t old_row = 0; old_row < children.size(); ++old_row) {
                    if (children[old_row].id != id) continue;
                    const auto old_prefix = std::string("/") + key + "/" + std::to_string(old_row) + "/";
                    const auto new_prefix = std::string("/") + key + "/" + std::to_string(row) + "/";
                    for (const auto& [pointer, receipt] : receipts.items()) {
                        if (pointer.starts_with(old_prefix)) original_quantity_entries[new_prefix + pointer.substr(old_prefix.size())] = receipt;
                    }
                }
            }
        };
        remap(stair_flights, stair->flights, "flights");
        remap(stair_landings, stair->landings, "landings");
    }

    bool read_stair_topology(StairFlight& stair, bool record_receipts = true, bool validate = true) {
        stair.flights.clear(); stair.landings.clear(); stair.riser_count = 0;
        if (record_receipts) remap_stair_child_receipts();
        for (int row = 0; row < stair_flights->rowCount(); ++row) {
            bool ok{};
            const auto text = stair_flights->item(row, 0)->text().trimmed();
            const auto count = text.toULongLong(&ok);
            if (!ok || count == 0 || count > maximum_risers || text != QString::number(count))
                throw std::invalid_argument("Each flight must have an integer riser count from 1 to 10000.");
            const auto length = [&](int column, const char* key) -> std::optional<double> {
                const auto* cell = stair_flights->item(row, column);
                const auto pointer = "/flights/" + std::to_string(row) + "/" + key;
                if (record_receipts) quantity_pointers["stairFlight" + std::to_string(row) + key] = pointer;
                if (!cell || cell->text().trimmed().isEmpty()) return std::nullopt;
                if (original_entity && cell->text() == cell->data(Qt::UserRole + 1).toString())
                    return cell->data(Qt::UserRole + 2).toDouble();
                const auto parsed = parse_quantity(cell->text().toStdString(), metric ? Unit::metre : Unit::foot);
                if (record_receipts) parsed_quantities.insert_or_assign(pointer, parsed);
                return parsed.metres;
            };
            stair.flights.push_back({row_id(stair_flights, row), static_cast<std::size_t>(count),
                                     length(1, "going_m"), length(2, "width_m")});
            stair.riser_count += static_cast<std::size_t>(count);
        }
        for (int row = 0; row < stair_landings->rowCount(); ++row) {
            const auto length = [&](int column, const char* key) {
                const auto* cell = stair_landings->item(row, column);
                const auto pointer = "/landings/" + std::to_string(row) + "/" + key;
                if (record_receipts) quantity_pointers["stairLanding" + std::to_string(row) + key] = pointer;
                if (original_entity && cell->text() == cell->data(Qt::UserRole + 1).toString())
                    return cell->data(Qt::UserRole + 2).toDouble();
                const auto parsed = parse_quantity(cell->text().toStdString(), metric ? Unit::metre : Unit::foot);
                if (record_receipts) parsed_quantities.insert_or_assign(pointer, parsed);
                return parsed.metres;
            };
            const auto* turns = qobject_cast<QComboBox*>(stair_landings->cellWidget(row, 2));
            const auto id = row_id(stair_landings, row);
            const auto turn = static_cast<StairTurn>(turns->currentData().toInt());
            bool align_right = false;
            if (turn == StairTurn::straight) {
                if (const auto* original = original_as<StairFlight>())
                    for (const auto& landing : original->landings)
                        if (landing.id == id) { align_right = landing.align_right; break; }
                if (const auto* alignment = qobject_cast<QComboBox*>(stair_landings->cellWidget(row, 4)))
                    align_right = alignment->currentData().toBool();
            }
            stair.landings.push_back({id, length(0, "depth_m"), length(1, "thickness_m"),
                turn, length(3, "return_gap_m"), align_right});
        }
        if (validate) validate_stair(stair);
        return true;
    }

    void refresh_stair_summary() {
        if (!stair_summary || loading || !fields.contains("buildingObjectTotalRise")) return;
        // Count validity is independent of the dimension/shape preview. Keep the
        // total visible while another field is temporarily invalid.
        qulonglong total{};
        bool counts_valid = true;
        for (int row = 0; row < stair_flights->rowCount(); ++row) {
            bool ok{};
            const auto text = stair_flights->item(row, 0)->text().trimmed();
            const auto count = text.toULongLong(&ok);
            if (!ok || count == 0 || count > maximum_risers || text != QString::number(count)) counts_valid = false;
            else total += count;
        }
        fields.at("buildingObjectRiserCount")->setText(counts_valid ? QString::number(total) : QStringLiteral("—"));
        try {
            StairFlight draft;
            draft.id = "preview";
            draft.total_rise = parse_quantity(fields.at("buildingObjectTotalRise")->text().toStdString(), metric ? Unit::metre : Unit::foot).metres;
            draft.going = parse_quantity(fields.at("buildingObjectGoing")->text().toStdString(), metric ? Unit::metre : Unit::foot).metres;
            draft.width = parse_quantity(fields.at("buildingObjectWidth")->text().toStdString(), metric ? Unit::metre : Unit::foot).metres;
            read_stair_topology(draft, false, false);
            const auto derived = derive_stair_layout(draft);
            double run{};
            for (const auto& flight : derived.flights) run += flight.run;
            stair_summary->setText(QStringLiteral("%1 flights, %2 connecting landings; %3 risers. Total rise %4; uniform riser %5; flight runs total %6.")
                .arg(draft.flights.size()).arg(draft.landings.size()).arg(draft.riser_count)
                .arg(display_length(draft.total_rise, metric), display_length(draft.total_rise / static_cast<double>(draft.riser_count), metric), display_length(run, metric)));
        } catch (const std::exception& caught) {
            stair_summary->setText(QStringLiteral("Invalid stair: %1").arg(QString::fromUtf8(caught.what())));
        }
    }

    void invalidate_candidate() {
        candidate_entity.reset();
        related_entities.clear();
        if (railing_preview) railing_preview->clear();
        if (railing_preview_status) railing_preview_status->setText(QStringLiteral("Preview needs updating."));
    }

    StairFlight selected_landing_stair() const {
        if (!source_snapshot || !host_combo || host_combo->currentData().toString().isEmpty())
            throw std::invalid_argument("Choose a current multi-flight stair.");
        const auto id = host_combo->currentData().toString().toStdString();
        const auto& entity = source_snapshot->entities().at(id);
        const auto version = entity.properties.value("version", 0);
        if ((version != 2 && version != 3 && version != 4) ||
            entity.properties.value("form", std::string{}) != "multi_flight_stair")
            throw std::invalid_argument("Landing railings require a canonical multi-flight stair.");
        return decode_stair_properties(id, entity.properties);
    }

    void setup_landing_railing_host(QFormLayout* layout) {
        host_combo = new QComboBox(form_page);
        host_combo->setObjectName(QStringLiteral("buildingObjectStairHost"));
        host_combo->addItem(QStringLiteral("Choose a stair…"), QString());
        if (source_snapshot) {
            int number{};
            for (const auto& [id, entity] : source_snapshot->entities()) {
                if (entity.type != "stair") continue;
                try {
                    const auto stair = decode_stair_properties(id, entity.properties);
                    const auto version = entity.properties.value("version", 0);
                    if ((version != 2 && version != 3 && version != 4) || stair.flights.empty() ||
                        (stair.landings.empty() && !stair.top_landing)) continue;
                    auto name = QStringLiteral("Stair %1").arg(++number);
                    if (entity.extensions.contains("name") && entity.extensions.at("name").is_string())
                        name = qt_string(entity.extensions.at("name").get<std::string>());
                    host_combo->addItem(name, qt_string(id));
                } catch (const std::exception&) { /* Unsupported hosts cannot supply authority. */ }
            }
        }
        layout->addRow(QStringLiteral("Stair"), host_combo);
        host_landing_combo = new QComboBox(form_page);
        host_landing_combo->setObjectName(QStringLiteral("buildingObjectStairHostLanding"));
        layout->addRow(QStringLiteral("Landing"), host_landing_combo);
        host_edge_combo = new QComboBox(form_page);
        host_edge_combo->setObjectName(QStringLiteral("buildingObjectStairHostLandingEdge"));
        layout->addRow(QStringLiteral("Exposed edge"), host_edge_combo);
        host_interval_combo = new QComboBox(form_page);
        host_interval_combo->setObjectName(QStringLiteral("buildingObjectStairHostLandingInterval"));
        layout->addRow(QStringLiteral("Available coverage"), host_interval_combo);
        add_scalar_field(layout, QStringLiteral("Start coverage (%)"), "buildingObjectHostStartPercent", 0);
        add_scalar_field(layout, QStringLiteral("End coverage (%)"), "buildingObjectHostEndPercent", 100);
        auto* full = new QPushButton(QStringLiteral("Use full available coverage"), form_page);
        full->setObjectName(QStringLiteral("buildingObjectLandingFullCoverage"));
        layout->addRow(full);
        landing_summary = new QLabel(form_page);
        landing_summary->setObjectName(QStringLiteral("buildingObjectLandingRailingSummary"));
        landing_summary->setWordWrap(true);
        landing_summary->setText(QStringLiteral("Choose a stair to see its landing edges."));
        layout->addRow(landing_summary);
        auto* note = new QLabel(QStringLiteral("Coverage measures the outer post faces along the original landing edge. Each railing must fit one exposed interval. Flight access remains open."), form_page);
        note->setWordWrap(true); layout->addRow(note);
        QObject::connect(host_combo, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { populate_host_landings(); });
        QObject::connect(host_landing_combo, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { populate_landing_edges(); });
        QObject::connect(host_edge_combo, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { populate_landing_intervals(); });
        QObject::connect(host_interval_combo, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { use_landing_interval(); });
        QObject::connect(full, &QPushButton::clicked, owner, [this] { use_landing_interval(); });
    }

    void populate_host_landings() {
        const QSignalBlocker blocker(*host_landing_combo);
        host_landing_combo->clear(); landing_choices.clear();
        try {
            const auto stair = selected_landing_stair();
            for (std::size_t i = 0; i < stair.landings.size(); ++i) {
                landing_choices.push_back({stair.id, StairLandingRole::connecting, stair.landings[i].id,
                    stair.flights[i].id, stair.flights[i + 1].id});
                host_landing_combo->addItem(QStringLiteral("Connecting landing %1 (flights %2 → %3)").arg(i + 1).arg(i + 1).arg(i + 2),
                    static_cast<int>(landing_choices.size() - 1));
            }
            if (stair.top_landing) {
                landing_choices.push_back({stair.id, StairLandingRole::top, {}, stair.flights.back().id, {}});
                host_landing_combo->addItem(QStringLiteral("Top landing (after flight %1)").arg(stair.flights.size()),
                    static_cast<int>(landing_choices.size() - 1));
            }
        } catch (const std::exception&) { /* Empty choice is reported when submitted. */ }
        populate_landing_edges();
    }

    std::optional<StairLandingRailingHost> selected_landing_host() const {
        if (!host_landing_combo || host_landing_combo->currentIndex() < 0) return std::nullopt;
        const auto index = host_landing_combo->currentData().toInt();
        if (index < 0 || static_cast<std::size_t>(index) >= landing_choices.size()) return std::nullopt;
        return landing_choices[static_cast<std::size_t>(index)];
    }

    void populate_landing_edges() {
        const QSignalBlocker blocker(*host_edge_combo);
        host_edge_combo->clear();
        try {
            const auto stair = selected_landing_stair();
            auto host = selected_landing_host();
            if (host) for (std::size_t edge = 0; edge < 4; ++edge) {
                host->edge_index = edge;
                const auto derived = derive_stair_landing_edge(stair, *host);
                if (derived.exposed_intervals.empty()) continue;
                const auto dx = derived.edge_end.x - derived.edge_start.x;
                const auto dy = derived.edge_end.y - derived.edge_start.y;
                // Edges follow the landing's own travel frame, independent of
                // world rotation. The number retains the original perimeter ID.
                const std::array<const char*, 4> labels{"Right side", "Far side", "Left side", "Near side"};
                host_edge_combo->addItem(QStringLiteral("%1 — edge %2, %3").arg(QString::fromLatin1(labels[edge])).arg(edge + 1)
                    .arg(display_length(std::hypot(dx, dy), metric)), static_cast<int>(edge));
            }
        } catch (const std::exception& caught) { fail(QString::fromUtf8(caught.what())); }
        populate_landing_intervals();
    }

    void populate_landing_intervals() {
        const QSignalBlocker blocker(*host_interval_combo);
        host_interval_combo->clear(); landing_intervals.clear();
        try {
            const auto stair = selected_landing_stair();
            auto host = selected_landing_host();
            if (host && host_edge_combo->currentIndex() >= 0) {
                host->edge_index = static_cast<std::size_t>(host_edge_combo->currentData().toInt());
                const auto edge = derive_stair_landing_edge(stair, *host);
                landing_intervals = edge.exposed_intervals;
                const auto length = std::hypot(edge.edge_end.x - edge.edge_start.x, edge.edge_end.y - edge.edge_start.y);
                for (std::size_t i = 0; i < landing_intervals.size(); ++i) {
                    const auto& interval = landing_intervals[i];
                    host_interval_combo->addItem(QStringLiteral("%1–%2% of edge (%3)")
                        .arg(QString::number(interval.start_fraction * 100, 'g', 6), QString::number(interval.end_fraction * 100, 'g', 6),
                             display_length(length * (interval.end_fraction - interval.start_fraction), metric)), static_cast<int>(i));
                }
                landing_summary->setText(QStringLiteral("Original edge %1; landing span inward %2. Choose one available interval, then adjust its coverage if needed.")
                    .arg(display_length(length, metric), display_length(edge.normal_span, metric)));
            } else landing_summary->setText(QStringLiteral("No exposed landing edge is selected."));
        } catch (const std::exception& caught) { fail(QString::fromUtf8(caught.what())); }
        use_landing_interval();
    }

    void use_landing_interval() {
        if (!host_interval_combo || host_interval_combo->currentIndex() < 0) return;
        const auto index = static_cast<std::size_t>(host_interval_combo->currentData().toInt());
        if (index >= landing_intervals.size()) return;
        fields.at("buildingObjectHostStartPercent")->setText(display_scalar(landing_intervals[index].start_fraction * 100));
        fields.at("buildingObjectHostEndPercent")->setText(display_scalar(landing_intervals[index].end_fraction * 100));
        if (!loading) {
            dirty["buildingObjectHostStartPercent"] = true;
            dirty["buildingObjectHostEndPercent"] = true;
            invalidate_candidate();
        }
    }

    void setup_railing_preview(QFormLayout* layout) {
        auto* button = new QPushButton(QStringLiteral("Preview landing railing"), form_page);
        button->setObjectName(QStringLiteral("buildingObjectRailingPreview"));
        layout->addRow(button);
        railing_preview = new QLabel(form_page);
        railing_preview->setObjectName(QStringLiteral("buildingObjectRailingPreview" "Image"));
        railing_preview->setAlignment(Qt::AlignCenter);
        layout->addRow(railing_preview);
        railing_preview_status = new QLabel(QStringLiteral("Preview needs updating."), form_page);
        railing_preview_status->setObjectName(QStringLiteral("buildingObjectRailingPreviewStatus"));
        railing_preview_status->setWordWrap(true); layout->addRow(railing_preview_status);
        QObject::connect(button, &QPushButton::clicked, owner, [this] {
            invalidate_candidate(); clear_error(); parsed_quantities.clear();
            if (original_invalid) { fail(QStringLiteral("The original landing attachment is unavailable in this document.")); return; }
            try {
                const auto object = read_object();
                if (!object) return;
                if (!source_snapshot) throw std::invalid_argument("A current document is required for a landing preview.");
                const auto shape = make_building_shape(*object, source_snapshot->entities());
                if (shape.IsNull()) throw std::invalid_argument("Landing railing preview did not produce geometry.");
                const auto& rail = std::get<Railing>(*object);
                const auto edge = derive_stair_landing_edge(selected_landing_stair(), *rail.landing_host);
                const auto span = std::hypot(edge.edge_end.x - edge.edge_start.x, edge.edge_end.y - edge.edge_start.y) *
                    (rail.landing_host->end_fraction - rail.landing_host->start_fraction);
                const auto plan = project_building_plan(*object, source_snapshot->entities());
                const auto stair_layout = derive_stair_layout(selected_landing_stair());
                const auto landing_index = rail.landing_host->role == StairLandingRole::top
                    ? selected_landing_stair().landings.size()
                    : static_cast<std::size_t>(host_landing_combo->currentData().toInt());
                const auto& landing = stair_layout.landings.at(landing_index).footprint;
                double min_x = std::numeric_limits<double>::infinity(), min_y = min_x;
                double max_x = -min_x, max_y = -min_x;
                for (const auto& segment : plan) for (const auto& point : {segment.start, segment.end}) {
                    min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
                    min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
                }
                for (const auto& point : landing) {
                    min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
                    min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
                }
                if (plan.empty()) throw std::invalid_argument("Landing railing preview has no plan geometry.");
                QPixmap image(320, 140); image.fill(owner->palette().color(QPalette::Base));
                QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
                const auto scale = std::min(288.0 / std::max(max_x - min_x, 0.01), 108.0 / std::max(max_y - min_y, 0.01));
                const auto point = [&](const auto& p) { return QPointF(16 + (p.x - min_x) * scale, 124 - (p.y - min_y) * scale); };
                painter.setPen(QPen(owner->palette().color(QPalette::Mid), 1, Qt::DashLine));
                for (std::size_t i = 0; i < landing.size(); ++i) painter.drawLine(point(landing[i]), point(landing[(i + 1) % landing.size()]));
                painter.setPen(QPen(owner->palette().color(QPalette::Text), 1.5));
                for (const auto& segment : plan) painter.drawLine(point(segment.start), point(segment.end));
                painter.end(); railing_preview->setPixmap(image);
                railing_preview_status->setText(QStringLiteral("Native geometry validated against this stair. Plan preview; coverage %1, height %2, thickness %3, maximum post spacing %4.")
                    .arg(display_length(span, metric), display_length(rail.height, metric), display_length(rail.thickness, metric), display_length(rail.post_spacing, metric)));
            } catch (const std::exception& caught) { fail(QString::fromUtf8(caught.what())); }
            parsed_quantities.clear();
        });
    }

    void setup_railing_host(QFormLayout* layout) {
        host_combo = new QComboBox(form_page);
        host_combo->setObjectName(QStringLiteral("buildingObjectStairHost"));
        host_combo->addItem(QStringLiteral("Choose a stair…"), QString());
        if (source_snapshot) {
            int number{};
            for (const auto& [id, entity] : source_snapshot->entities()) {
                if (entity.type != "stair") continue;
                try {
                    (void)decode_stair_properties(id, entity.properties);
                    auto name = QStringLiteral("Stair %1").arg(++number);
                    if (entity.extensions.contains("name") && entity.extensions.at("name").is_string())
                        name = qt_string(entity.extensions.at("name").get<std::string>());
                    host_combo->addItem(name, qt_string(id));
                } catch (const std::exception&) { /* Unsupported hosts are not authorable. */ }
            }
        }
        layout->addRow(QStringLiteral("Stair host"), host_combo);
        host_flight_combo = new QComboBox(form_page);
        host_flight_combo->setObjectName(QStringLiteral("buildingObjectStairHostFlight"));
        layout->addRow(QStringLiteral("Flight"), host_flight_combo);
        host_side_combo = new QComboBox(form_page);
        host_side_combo->setObjectName(QStringLiteral("buildingObjectStairHostSide"));
        host_side_combo->addItems({QStringLiteral("Left (upward travel)"), QStringLiteral("Right (upward travel)")});
        layout->addRow(QStringLiteral("Side"), host_side_combo);
        add_scalar_field(layout, QStringLiteral("Start station (0–1)"), "buildingObjectHostStart", 0);
        add_scalar_field(layout, QStringLiteral("End station (0–1)"), "buildingObjectHostEnd", 1);
        auto* note = new QLabel(QStringLiteral("Stations follow the selected flight. Flight rails do not create landing guards. Choosing a straight stair upgrades it to one stable flight when you apply."), form_page);
        note->setWordWrap(true);
        layout->addRow(note);
        QObject::connect(host_combo, qOverload<int>(&QComboBox::currentIndexChanged), owner, [this] { populate_host_flights(); });
    }

    void populate_host_flights() {
        if (!host_combo || !host_flight_combo) return;
        host_flight_combo->clear();
        const auto id = host_combo->currentData().toString().toStdString();
        if (id.empty() || !source_snapshot) return;
        try {
            auto entity = source_snapshot->entities().at(id);
            auto stair = decode_stair_properties(id, entity.properties);
            if (stair.flights.empty()) {
                auto found = upgraded_hosts.find(id);
                if (found == upgraded_hosts.end()) {
                    stair.flights.push_back({make_stable_id(), stair.riser_count});
                    const auto topology = encode_stair_properties(stair);
                    // This implicit upgrade authors only the schema and one
                    // flight identity. Keep unchanged dimensions, nested opaque
                    // landing/connection content and quantity receipts verbatim.
                    for (const auto* key : {"version", "form", "flights", "landings"})
                        entity.properties[key] = topology.at(key);
                    found = upgraded_hosts.emplace(id, std::move(entity)).first;
                }
                stair = decode_stair_properties(id, found->second.properties);
            }
            for (std::size_t i = 0; i < stair.flights.size(); ++i)
                host_flight_combo->addItem(QStringLiteral("Flight %1 (%2 risers)").arg(i + 1).arg(stair.flights[i].riser_count), qt_string(stair.flights[i].id));
        } catch (const std::exception& caught) { fail(QString::fromUtf8(caught.what())); }
    }

    void setup_roof_openings(QVBoxLayout* layout) {
        // Keep the draft outside the rebuilt form page so changing roof forms
        // never silently deletes authored openings or their stable IDs.
        roof_openings_group = new QGroupBox(QStringLiteral("Roof openings"), form_body);
        roof_openings_group->setObjectName(QStringLiteral("buildingObjectRoofOpeningsGroup"));
        auto* group_layout = new QVBoxLayout(roof_openings_group);
        group_layout->setContentsMargins(6, 6, 6, 6);
        group_layout->setSpacing(4);
        roof_openings_help = new QLabel(roof_openings_group);
        roof_openings_help->setWordWrap(true);
        group_layout->addWidget(roof_openings_help);
        roof_openings_table = new QTableWidget(0, 4, roof_openings_group);
        roof_openings_table->setObjectName(QStringLiteral("buildingObjectRoofOpenings"));
        roof_openings_table->setAccessibleName(QStringLiteral("Roof opening measurements"));
        roof_openings_table->setHorizontalHeaderLabels({QStringLiteral("X"), QStringLiteral("Y"),
                                                        QStringLiteral("Width"), QStringLiteral("Depth")});
        roof_openings_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        roof_openings_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        roof_openings_table->setMinimumWidth(0);
        roof_openings_table->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        refresh_roof_opening_table_height();
        group_layout->addWidget(roof_openings_table);
        auto* actions = new QHBoxLayout;
        auto* add = new QPushButton(QStringLiteral("Add opening"), roof_openings_group);
        add->setObjectName(QStringLiteral("buildingObjectAddRoofOpening"));
        add->setAutoDefault(false);
        auto* remove = new QPushButton(QStringLiteral("Remove selected"), roof_openings_group);
        remove->setObjectName(QStringLiteral("buildingObjectRemoveRoofOpening"));
        remove->setAutoDefault(false);
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addStretch();
        group_layout->addLayout(actions);
        layout->addWidget(roof_openings_group);
        QObject::connect(add, &QPushButton::clicked, owner, [this] {
            if (roof_openings_table->rowCount() >= 256) {
                fail(QStringLiteral("A roof supports at most 256 openings."));
                return;
            }
            clear_error();
            append_roof_opening({QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                                 0.5, 0.5, 0.5, 0.5});
            roof_openings_table->setCurrentCell(roof_openings_table->rowCount() - 1, 0);
        });
        QObject::connect(remove, &QPushButton::clicked, owner, [this] {
            const auto rows = roof_openings_table->selectionModel()->selectedRows();
            std::vector<int> indices;
            for (const auto& row : rows) indices.push_back(row.row());
            std::sort(indices.rbegin(), indices.rend());
            for (const auto row : indices) roof_openings_table->removeRow(row);
            refresh_roof_opening_table_height();
            clear_error();
        });
    }

    void refresh_roof_opening_table_height() {
        const auto rows = std::clamp(roof_openings_table->rowCount(), 1, 4);
        roof_openings_table->setFixedHeight(roof_openings_table->horizontalHeader()->sizeHint().height() +
            rows * roof_openings_table->verticalHeader()->defaultSectionSize() +
            2 * roof_openings_table->frameWidth());
    }

    std::optional<Quantity> opening_receipt(const std::string& id, const char* key,
                                            double authoritative) const {
        try {
            const auto& receipt = original_roof_opening_receipts.at(id).at(key);
            const auto expression = receipt.at("original_expression").get<std::string>();
            const auto unit_text = receipt.at("default_unit").get<std::string>();
            if (expression.empty() || expression.size() > 4096 ||
                (unit_text != "m" && unit_text != "ft")) return std::nullopt;
            const auto quantity = parse_quantity(expression, unit_text == "m" ? Unit::metre : Unit::foot);
            const auto numerator = json_int64(receipt.at("exact_metres").at("numerator"));
            const auto denominator = json_int64(receipt.at("exact_metres").at("denominator"));
            if (!numerator || !denominator || *denominator <= 0 ||
                quantity.exact_metres != ExactRational{*numerator, *denominator} ||
                quantity.metres != authoritative) return std::nullopt;
            return quantity;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    void append_roof_opening(const RoofOpening& opening) {
        const auto row = roof_openings_table->rowCount();
        roof_openings_table->insertRow(row);
        const std::array<double, 4> values{opening.x, opening.y, opening.width, opening.depth};
        for (int column = 0; column < 4; ++column) {
            auto text = display_length(values[column], metric);
            if (const auto receipt = opening_receipt(opening.id, roof_opening_keys[column], values[column]); receipt)
                text = display_receipt_expression(*receipt, metric);
            auto* item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, qt_string(opening.id));
            item->setData(Qt::UserRole + 1, text);
            item->setData(Qt::UserRole + 2, values[column]);
            roof_openings_table->setItem(row, column, item);
        }
        refresh_roof_opening_table_height();
    }

    void populate_roof_openings() {
        const auto prior = original_entity->extensions.find("roof_opening_input");
        if (prior != original_entity->extensions.end() && prior->is_object() &&
            prior->value("version", json{}) == 1 && prior->contains("entries") && prior->at("entries").is_object())
            original_roof_opening_receipts = prior->at("entries");
        std::visit([this](const auto& object) {
            using Object = std::decay_t<decltype(object)>;
            if constexpr (std::is_same_v<Object, SlopedRoofPanel> || std::is_same_v<Object, GableRoof> ||
                          std::is_same_v<Object, HipRoof>) {
                for (const auto& opening : object.openings) append_roof_opening(opening);
            }
        }, *original_object);
    }

    bool roof_geometry_unchanged(const BuildingObject& candidate) const {
        if (!original_entity || original_entity->type != "roof") return false;
        return std::visit([this](const auto& roof) {
            using Roof = std::decay_t<decltype(roof)>;
            if constexpr (std::is_same_v<Roof, SlopedRoofPanel> || std::is_same_v<Roof, GableRoof> ||
                          std::is_same_v<Roof, HipRoof>) {
                const auto* source = original_as<Roof>();
                if (!source || roof.id != source->id ||
                    roof.base_position.x != source->base_position.x ||
                    roof.base_position.y != source->base_position.y ||
                    roof.base_position.z != source->base_position.z ||
                    roof.orientation_radians != source->orientation_radians ||
                    roof.span != source->span || roof.rise != source->rise ||
                    roof.pitch_radians != source->pitch_radians ||
                    roof.overhang != source->overhang || roof.thickness != source->thickness) return false;
                if constexpr (std::is_same_v<Roof, SlopedRoofPanel>) return roof.run == source->run;
                else return roof.length == source->length;
            } else {
                return false;
            }
        }, candidate);
    }

    std::optional<std::vector<RoofOpening>> read_roof_openings() {
        std::vector<RoofOpening> openings;
        auto entries = json::array();
        auto receipts = original_roof_opening_receipts;
        roof_opening_intent = {};
        if (original_entity) roof_opening_intent.roof_id = original_entity->id;
        // Bind each row to the actual original child identity, independently of
        // display roles and receipt JSON. An untouched new row still needs inputs.
        std::map<std::string, const json*, std::less<>> original_rows;
        if (original_entity && original_entity->properties.contains("roof_openings")) {
            for (const auto& source : original_entity->properties.at("roof_openings"))
                original_rows.emplace(source.at("id").get<std::string>(), &source);
        }
        const auto default_unit = metric ? Unit::metre : Unit::foot;
        for (int row = 0; row < roof_openings_table->rowCount(); ++row) {
            const auto id = roof_openings_table->item(row, 0)->data(Qt::UserRole).toString().toStdString();
            const auto original = original_rows.find(id);
            const bool fresh = original == original_rows.end();
            RoofOpeningUpsertIntent upsert;
            upsert.opening_id = id;
            std::array<double, 4> values{};
            json entry{{"id", id}};
            for (int column = 0; column < 4; ++column) {
                const auto* item = roof_openings_table->item(row, column);
                const auto text = item->text().trimmed();
                try {
                    std::optional<Quantity> quantity;
                    if (text == item->data(Qt::UserRole + 1).toString()) {
                        values[column] = item->data(Qt::UserRole + 2).toDouble();
                        if (fresh) quantity = exact_roof_opening_default(values[column], default_unit);
                    } else {
                        quantity = parse_quantity(text.toStdString(), default_unit);
                        values[column] = quantity->metres;
                    }
                    const bool changed = fresh ||
                        values[column] != original->second->at(roof_opening_keys[column]).get<double>();
                    if (changed) {
                        if (!quantity) throw std::invalid_argument("The edited roof opening lacks its exact input.");
                        upsert.*roof_opening_inputs[column] = RoofOpeningQuantityInput{*quantity, default_unit};
                        if (receipts.contains(id) && !known_roof_opening_receipt(receipts.at(id)))
                            throw std::invalid_argument("The roof opening has an unsupported child receipt version.");
                        if (!receipts.contains(id)) receipts[id] = json::object();
                        auto updated = json{{"original_expression", quantity->original_expression},
                             {"default_unit", metric ? "m" : "ft"},
                             {"exact_metres", {{"numerator", quantity->exact_metres.numerator},
                                               {"denominator", quantity->exact_metres.denominator}}}};
                        if (receipts[id].contains(roof_opening_keys[column]) &&
                            !known_roof_opening_receipt(receipts[id].at(roof_opening_keys[column])))
                            throw std::invalid_argument("The roof opening dimension has an unsupported receipt version.");
                        auto& receipt = receipts[id][roof_opening_keys[column]];
                        if (receipt.is_object()) {
                            auto exact = receipt.value("exact_metres", json::object());
                            if (!exact.is_object())
                                throw std::invalid_argument("The roof opening receipt rational must be an object.");
                            exact.update(updated.at("exact_metres"));
                            receipt.update(updated);
                            receipt["exact_metres"] = std::move(exact);
                        } else {
                            receipt = std::move(updated);
                        }
                    }
                } catch (const std::exception& caught) {
                    roof_openings_table->setCurrentCell(row, column);
                    roof_openings_table->setFocus();
                    fail(QStringLiteral("Opening %1, %2: %3").arg(row + 1)
                        .arg(roof_openings_table->horizontalHeaderItem(column)->text())
                        .arg(QString::fromUtf8(caught.what())));
                    return std::nullopt;
                }
                entry[roof_opening_keys[column]] = values[column];
            }
            if (std::any_of(roof_opening_inputs.begin(), roof_opening_inputs.end(),
                            [&](const auto member) { return (upsert.*member).has_value(); }))
                roof_opening_intent.upserts.push_back(std::move(upsert));
            openings.push_back({id, values[0], values[1], values[2], values[3]});
            entries.push_back(std::move(entry));
        }
        auto original_entries = json::array();
        if (original_entity && original_entity->properties.contains("roof_openings")) {
            for (const auto& source : original_entity->properties.at("roof_openings")) {
                json understood{{"id", source.at("id")}};
                for (const auto* key : roof_opening_keys) understood[key] = source.at(key);
                original_entries.push_back(std::move(understood));
                const auto id = source.at("id").get<std::string>();
                if (std::none_of(openings.begin(), openings.end(), [&](const auto& opening) { return opening.id == id; }))
                    roof_opening_intent.removed_opening_ids.push_back(id);
            }
        }
        for (auto receipt = receipts.begin(); receipt != receipts.end();) {
            const bool retained = std::any_of(openings.begin(), openings.end(), [&](const auto& opening) {
                return opening.id == receipt.key();
            });
            const bool original_child = std::any_of(original_entries.begin(), original_entries.end(), [&](const auto& opening) {
                return opening.at("id") == receipt.key();
            });
            if (!retained && original_child) {
                if (!known_roof_opening_receipt(*receipt))
                    throw std::invalid_argument("Roof opening removal cannot erase an unsupported child receipt.");
                for (const auto* key : roof_opening_keys)
                    if (receipt->contains(key) && !known_roof_opening_receipt(receipt->at(key)))
                        throw std::invalid_argument("Roof opening removal cannot erase an unsupported dimension receipt.");
                receipt = receipts.erase(receipt);
            } else ++receipt;
        }
        roof_openings_changed = entries != original_entries || receipts != original_roof_opening_receipts;
        roof_opening_receipts = std::move(receipts);
        return openings;
    }

    std::string quantity_pointer_for_field(const char* name) const {
        if (form_combo == nullptr) {
            return {};
        }
        const auto form = form_string(form_combo->currentData().toString());
        const std::string_view field_name{name};
        std::string vector_property;
        if (form == "rectangular_column" || form == "circular_column") {
            vector_property = "base_center_m";
        } else if (form == "straight_stair_flight" || form == "multi_flight_stair" || form == "straight_railing" ||
                   form == "sloped_roof_panel" ||
                   (form == "gable_roof" || form == "hip_roof")) {
            vector_property = "base_position_m";
        }
        if (!vector_property.empty()) {
            if (field_name == "buildingObjectBaseX") {
                return "/" + vector_property + "/0";
            }
            if (field_name == "buildingObjectBaseY") {
                return "/" + vector_property + "/1";
            }
            if (field_name == "buildingObjectBaseZ") {
                return "/" + vector_property + "/2";
            }
        }
        if (form == "straight_beam") {
            if (field_name == "buildingObjectStartX") {
                return "/start_m/0";
            }
            if (field_name == "buildingObjectStartY") {
                return "/start_m/1";
            }
            if (field_name == "buildingObjectStartZ") {
                return "/start_m/2";
            }
            if (field_name == "buildingObjectEndX") {
                return "/end_m/0";
            }
            if (field_name == "buildingObjectEndY") {
                return "/end_m/1";
            }
            if (field_name == "buildingObjectEndZ") {
                return "/end_m/2";
            }
        }
        if (field_name == "buildingObjectWidth") {
            return "/width_m";
        }
        if (field_name == "buildingObjectDepth") {
            return "/depth_m";
        }
        if (field_name == "buildingObjectHeight") {
            return "/height_m";
        }
        if (field_name == "buildingObjectRadius") {
            return "/radius_m";
        }
        if (field_name == "buildingObjectTotalRise") {
            return "/total_rise_m";
        }
        if (field_name == "buildingObjectGoing") {
            return "/going_m";
        }
        if (field_name == "buildingObjectLandingDepth") {
            return "/top_landing/depth_m";
        }
        if (field_name == "buildingObjectLandingThickness") {
            return "/top_landing/thickness_m";
        }
        if (field_name == "buildingObjectRun") {
            return "/run_m";
        }
        if (field_name == "buildingObjectSpan") {
            return "/span_m";
        }
        if (field_name == "buildingObjectRise") {
            return "/rise_m";
        }
        if (field_name == "buildingObjectLength") {
            return "/length_m";
        }
        if (field_name == "buildingObjectOverhang") {
            return "/overhang_m";
        }
        if (field_name == "buildingObjectThickness") {
            return "/thickness_m";
        }
        if (field_name == "buildingObjectPostSpacing") {
            return "/post_spacing_m";
        }
        return {};
    }

    void connect_pitch_updates() {
        for (const auto key : {"buildingObjectRun", "buildingObjectSpan",
                               "buildingObjectRise"}) {
            if (fields.contains(key)) {
                QObject::connect(fields.at(key), &QLineEdit::textChanged, owner,
                                 [this](const QString&) { refresh_pitch(); });
            }
        }
    }

    void refresh_pitch() {
        if (derived_pitch == nullptr) {
            return;
        }
        const auto form = form_string(form_combo->currentData().toString());
        if (original_object.has_value()) {
            if (form == "sloped_roof_panel") {
                if (const auto* original = original_as<SlopedRoofPanel>();
                    original != nullptr && !dirty.contains("buildingObjectRun") &&
                    !dirty.contains("buildingObjectRise")) {
                    derived_pitch->setText(display_pitch(original->pitch_radians));
                    return;
                }
            } else if (form == "gable_roof" || form == "hip_roof") {
                if (const auto* original = original_as<HipRoof>();
                    original != nullptr && !dirty.contains("buildingObjectSpan") &&
                    !dirty.contains("buildingObjectRise")) {
                    derived_pitch->setText(display_pitch(original->pitch_radians));
                    return;
                }
                if (const auto* original = original_as<GableRoof>();
                    original != nullptr && !dirty.contains("buildingObjectSpan") &&
                    !dirty.contains("buildingObjectRise")) {
                    derived_pitch->setText(display_pitch(original->pitch_radians));
                    return;
                }
            }
        }
        const auto run_text = fields.contains("buildingObjectRun")
                                  ? fields.at("buildingObjectRun")->text()
                                  : fields.at("buildingObjectSpan")->text();
        const auto rise_text = fields.contains("buildingObjectRise")
                                   ? fields.at("buildingObjectRise")->text()
                                   : QString();
        bool run_ok = false;
        bool rise_ok = false;
        double run = 0.0;
        double rise = 0.0;
        try {
            run = parse_quantity(run_text.toStdString(),
                                 metric ? Unit::metre : Unit::foot).metres;
            rise = parse_quantity(rise_text.toStdString(),
                                  metric ? Unit::metre : Unit::foot).metres;
        } catch (const std::exception&) {
            if (derived_pitch != nullptr) {
                derived_pitch->setText(QStringLiteral("—"));
            }
            return;
        }
        run_ok = std::isfinite(run) && run > geometry_tolerance;
        rise_ok = std::isfinite(rise) && rise >= 0.0;
        double denominator = run;
        if (form == "gable_roof" || form == "hip_roof") {
            denominator = run * 0.5;
            run_ok = run_ok && denominator > geometry_tolerance;
        }
        if (run_ok && rise_ok) {
            const auto pitch = std::atan(rise / denominator);
            derived_pitch->setText(display_pitch(pitch));
        } else {
            derived_pitch->setText(QStringLiteral("—"));
        }
    }

    void populate_from_original() {
        if (!original_object.has_value()) {
            return;
        }
        loading = true;
        std::visit(
            [this](const auto& value) {
                using Object = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Object, RectangularColumn>) {
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_center);
                    set_length("buildingObjectWidth", value.width);
                    set_length("buildingObjectDepth", value.depth);
                    set_length("buildingObjectHeight", value.height);
                    set_angle("buildingObjectOrientationDegrees", value.rotation_radians);
                } else if constexpr (std::is_same_v<Object, CircularColumn>) {
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_center);
                    set_length("buildingObjectRadius", value.radius);
                    set_length("buildingObjectHeight", value.height);
                } else if constexpr (std::is_same_v<Object, Beam>) {
                    set_coordinate("buildingObjectStartX", "buildingObjectStartY",
                                   "buildingObjectStartZ", value.start);
                    set_coordinate("buildingObjectEndX", "buildingObjectEndY",
                                   "buildingObjectEndZ", value.end);
                    set_scalar("buildingObjectUpX", value.up.x);
                    set_scalar("buildingObjectUpY", value.up.y);
                    set_scalar("buildingObjectUpZ", value.up.z);
                    set_length("buildingObjectWidth", value.width);
                    set_length("buildingObjectDepth", value.depth);
                } else if constexpr (std::is_same_v<Object, StairFlight>) {
                    if (stair_flights) populate_stair_topology(value);
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_position);
                    set_angle("buildingObjectOrientationDegrees", value.orientation_radians);
                    set_integer("buildingObjectRiserCount", value.riser_count);
                    set_length("buildingObjectTotalRise", value.total_rise);
                    set_length("buildingObjectGoing", value.going);
                    set_length("buildingObjectWidth", value.width);
                    const bool has_landing = value.top_landing.has_value();
                    landing_check->setChecked(has_landing);
                    set_length("buildingObjectLandingDepth",
                               has_landing ? value.top_landing->depth : 1.2);
                    set_length("buildingObjectLandingThickness",
                               has_landing ? value.top_landing->thickness : 0.15);
                    const bool has_level_connection = value.level_connection.has_value();
                    level_connection_check->setChecked(has_level_connection);
                    set_text("buildingObjectLevelGraph",
                             has_level_connection
                                 ? qt_string(value.level_connection->graph_entity_id)
                                 : QString());
                    set_text("buildingObjectLevelLink",
                             has_level_connection
                                 ? qt_string(value.level_connection->link_id)
                                 : QString());
                    set_text("buildingObjectLowerLevel",
                             has_level_connection
                                 ? qt_string(value.level_connection->lower_level_id)
                                 : QString());
                    set_text("buildingObjectUpperLevel",
                             has_level_connection
                                 ? qt_string(value.level_connection->upper_level_id)
                                 : QString());
                } else if constexpr (std::is_same_v<Object, Railing>) {
                    if (value.landing_host && host_combo) {
                        const auto& host = *value.landing_host;
                        host_combo->setCurrentIndex(host_combo->findData(qt_string(host.stair_id)));
                        populate_host_landings();
                        int landing_index = -1;
                        for (std::size_t i = 0; i < landing_choices.size(); ++i) {
                            const auto& choice = landing_choices[i];
                            if (choice.role == host.role && choice.landing_id == host.landing_id &&
                                choice.incoming_flight_id == host.incoming_flight_id && choice.outgoing_flight_id == host.outgoing_flight_id)
                                landing_index = static_cast<int>(i);
                        }
                        host_landing_combo->setCurrentIndex(landing_index);
                        populate_landing_edges();
                        host_edge_combo->setCurrentIndex(host_edge_combo->findData(static_cast<int>(host.edge_index)));
                        populate_landing_intervals();
                        int interval_index = -1;
                        for (std::size_t i = 0; i < landing_intervals.size(); ++i)
                            if (host.start_fraction >= landing_intervals[i].start_fraction - geometry_tolerance &&
                                host.end_fraction <= landing_intervals[i].end_fraction + geometry_tolerance)
                                interval_index = static_cast<int>(i);
                        host_interval_combo->setCurrentIndex(interval_index);
                        set_scalar("buildingObjectHostStartPercent", host.start_fraction * 100);
                        set_scalar("buildingObjectHostEndPercent", host.end_fraction * 100);
                        if (host_combo->currentIndex() < 1 || landing_index < 0 || host_edge_combo->currentIndex() < 0 || interval_index < 0) {
                            original_invalid = true;
                            fail(QStringLiteral("The original landing edge or its flight witnesses are unavailable in this document."));
                        }
                    } else if (value.host && host_combo) {
                        host_combo->setCurrentIndex(host_combo->findData(qt_string(value.host->stair_id)));
                        populate_host_flights();
                        host_flight_combo->setCurrentIndex(host_flight_combo->findData(qt_string(value.host->flight_id)));
                        host_side_combo->setCurrentIndex(value.host->side == StairRailingSide::left ? 0 : 1);
                        set_scalar("buildingObjectHostStart", value.host->start_fraction);
                        set_scalar("buildingObjectHostEnd", value.host->end_fraction);
                    }
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_position);
                    set_angle("buildingObjectOrientationDegrees", value.orientation_radians);
                    set_length("buildingObjectLength", value.length);
                    set_length("buildingObjectHeight", value.height);
                    set_length("buildingObjectThickness", value.thickness);
                    set_length("buildingObjectPostSpacing", value.post_spacing);
                } else if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_position);
                    set_angle("buildingObjectOrientationDegrees", value.orientation_radians);
                    set_length("buildingObjectRun", value.run);
                    set_length("buildingObjectSpan", value.span);
                    set_length("buildingObjectRise", value.rise);
                    set_length("buildingObjectOverhang", value.overhang);
                    set_length("buildingObjectThickness", value.thickness);
                } else if constexpr ((std::is_same_v<Object, GableRoof> || std::is_same_v<Object, HipRoof>)) {
                    set_coordinate("buildingObjectBaseX", "buildingObjectBaseY",
                                   "buildingObjectBaseZ", value.base_position);
                    set_angle("buildingObjectOrientationDegrees", value.orientation_radians);
                    set_length("buildingObjectLength", value.length);
                    set_length("buildingObjectSpan", value.span);
                    set_length("buildingObjectRise", value.rise);
                    set_length("buildingObjectOverhang", value.overhang);
                    set_length("buildingObjectThickness", value.thickness);
                }
            },
            *original_object);
        loading = false;
        dirty.clear();
        if (landing_check != nullptr) {
            const auto enabled = landing_check->isChecked();
            fields.at("buildingObjectLandingDepth")->setEnabled(enabled);
            fields.at("buildingObjectLandingThickness")->setEnabled(enabled);
        }
        if (level_connection_check != nullptr) {
            const auto enabled = level_connection_check->isChecked();
            for (const auto* name : {"buildingObjectLevelGraph", "buildingObjectLevelLink",
                                     "buildingObjectLowerLevel", "buildingObjectUpperLevel"}) {
                fields.at(name)->setEnabled(enabled);
            }
        }
    }

    void add_coordinate_fields(QFormLayout* layout, QString label,
                               const char* x_name, const char* y_name,
                               const char* z_name, Vec3 defaults) {
        add_quantity_field(layout, label + QStringLiteral(" X"), x_name, defaults.x,
                           false);
        add_quantity_field(layout, label + QStringLiteral(" Y"), y_name, defaults.y,
                           false);
        add_quantity_field(layout, label + QStringLiteral(" Z"), z_name, defaults.z,
                           false);
    }

    void add_scalar_fields(QFormLayout* layout, QString label, const char* x_name,
                           const char* y_name, const char* z_name, Vec3 defaults) {
        add_scalar_field(layout, label + QStringLiteral(" X"), x_name, defaults.x);
        add_scalar_field(layout, label + QStringLiteral(" Y"), y_name, defaults.y);
        add_scalar_field(layout, label + QStringLiteral(" Z"), z_name, defaults.z);
    }

    void add_quantity_field(QFormLayout* layout, QString label, const char* name,
                            double default_metres, bool positive = true) {
        (void)positive;
        auto* edit = new QLineEdit(form_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setMinimumWidth(0);
        edit->setPlaceholderText(metric ? QStringLiteral("e.g. 1.2 m")
                                         : QStringLiteral("e.g. 4 ft"));
        edit->setText(display_length(default_metres, metric));
        fields.emplace(name, edit);
        const auto pointer = quantity_pointer_for_field(name);
        if (!pointer.empty()) {
            quantity_pointers.emplace(name, pointer);
        }
        QObject::connect(edit, &QLineEdit::textChanged, owner,
                         [this, name](const QString&) {
                             if (!loading) {
                                 dirty[name] = true;
                             }
                         });
        layout->addRow(std::move(label), edit);
    }

    void add_scalar_field(QFormLayout* layout, QString label, const char* name,
                          double default_value) {
        auto* edit = new QLineEdit(form_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setMinimumWidth(0);
        edit->setText(display_scalar(default_value));
        fields.emplace(name, edit);
        QObject::connect(edit, &QLineEdit::textChanged, owner,
                         [this, name](const QString&) {
                             if (!loading) {
                                 dirty[name] = true;
                             }
                         });
        layout->addRow(std::move(label), edit);
    }

    void add_angle_field(QFormLayout* layout, QString label, const char* name,
                         double default_radians) {
        auto* edit = new QLineEdit(form_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setMinimumWidth(0);
        edit->setText(display_angle(default_radians));
        edit->setPlaceholderText(QStringLiteral("degrees"));
        fields.emplace(name, edit);
        QObject::connect(edit, &QLineEdit::textChanged, owner,
                         [this, name](const QString&) {
                             if (!loading) {
                                 dirty[name] = true;
                             }
                         });
        layout->addRow(std::move(label), edit);
    }

    void add_integer_field(QFormLayout* layout, QString label, const char* name,
                           std::size_t default_value) {
        auto* edit = new QLineEdit(form_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setMinimumWidth(0);
        edit->setText(QString::number(static_cast<qulonglong>(default_value)));
        edit->setPlaceholderText(QStringLiteral("1–10000"));
        fields.emplace(name, edit);
        QObject::connect(edit, &QLineEdit::textChanged, owner,
                         [this, name](const QString&) {
                             if (!loading) {
                                 dirty[name] = true;
                             }
                         });
        layout->addRow(std::move(label), edit);
    }

    void add_identifier_field(QFormLayout* layout, QString label, const char* name) {
        auto* edit = new QLineEdit(form_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setMinimumWidth(0);
        edit->setPlaceholderText(QStringLiteral("stable ID"));
        fields.emplace(name, edit);
        QObject::connect(edit, &QLineEdit::textChanged, owner,
                         [this, name](const QString&) {
                             if (!loading) {
                                 dirty[name] = true;
                             }
                         });
        layout->addRow(std::move(label), edit);
    }

    void set_length(const char* name, double value) {
        const auto pointer = quantity_pointers.find(name);
        if (pointer != quantity_pointers.end()) {
            const auto receipt = original_quantity_entries.find(pointer->second);
            if (receipt != original_quantity_entries.end()) {
                if (const auto decoded = receipt_for_value(receipt->second, value);
                    decoded.has_value()) {
                    set_text(name, display_receipt_expression(decoded->quantity, metric));
                    return;
                }
            }
        }
        set_text(name, display_length(value, metric));
    }

    void set_scalar(const char* name, double value) { set_text(name, display_scalar(value)); }

    void set_angle(const char* name, double value) { set_text(name, display_angle(value)); }

    void set_integer(const char* name, std::size_t value) {
        set_text(name, QString::number(static_cast<qulonglong>(value)));
    }

    void set_coordinate(const char* x_name, const char* y_name, const char* z_name,
                        Vec3 value) {
        set_length(x_name, value.x);
        set_length(y_name, value.y);
        set_length(z_name, value.z);
    }

    void set_text(const char* name, const QString& value) {
        const auto found = fields.find(name);
        if (found != fields.end()) {
            found->second->setText(value);
        }
    }

    template <typename Object>
    const Object* original_as() const {
        if (!original_object.has_value()) {
            return nullptr;
        }
        return std::get_if<Object>(&*original_object);
    }

    std::optional<double> read_length(const char* name, QString label, bool positive,
                                      double fallback) {
        if (original_entity.has_value() && !dirty.contains(name)) {
            return fallback;
        }
        const auto found = fields.find(name);
        if (found == fields.end()) {
            fail(QStringLiteral("%1 is unavailable.").arg(label));
            return std::nullopt;
        }
        try {
            const auto quantity = parse_quantity(
                found->second->text().toStdString(), metric ? Unit::metre : Unit::foot);
            if (!std::isfinite(quantity.metres) ||
                (positive && quantity.metres <= geometry_tolerance)) {
                fail(QStringLiteral("%1 must be greater than zero.").arg(label));
                return std::nullopt;
            }
            const auto pointer = quantity_pointers.find(name);
            if (pointer != quantity_pointers.end()) {
                parsed_quantities.insert_or_assign(pointer->second, quantity);
            }
            return quantity.metres;
        } catch (const std::exception& caught) {
            fail(QStringLiteral("%1: %2").arg(label, QString::fromUtf8(caught.what())));
            return std::nullopt;
        }
    }

    std::optional<double> read_scalar(const char* name, QString label, double fallback) {
        if (original_entity.has_value() && !dirty.contains(name)) {
            return fallback;
        }
        const auto found = fields.find(name);
        if (found == fields.end()) {
            fail(QStringLiteral("%1 is unavailable.").arg(label));
            return std::nullopt;
        }
        bool ok = false;
        const auto value = found->second->text().trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            fail(QStringLiteral("%1 must be a finite number.").arg(label));
            return std::nullopt;
        }
        return value;
    }

    std::optional<double> read_angle(const char* name, QString label, double fallback) {
        if (original_entity.has_value() && !dirty.contains(name)) {
            return fallback;
        }
        const auto degrees = read_scalar(name, label, fallback * 180.0 / std::numbers::pi);
        if (!degrees.has_value()) {
            return std::nullopt;
        }
        const auto radians = *degrees * std::numbers::pi / 180.0;
        if (!std::isfinite(radians) || std::abs(radians) > 1'000'000.0) {
            fail(QStringLiteral("%1 is outside the supported range.").arg(label));
            return std::nullopt;
        }
        return radians;
    }

    std::optional<std::size_t> read_integer(const char* name, QString label,
                                             std::size_t fallback) {
        if (original_entity.has_value() && !dirty.contains(name)) {
            return fallback;
        }
        const auto found = fields.find(name);
        if (found == fields.end()) {
            fail(QStringLiteral("%1 is unavailable.").arg(label));
            return std::nullopt;
        }
        const auto text = found->second->text().trimmed();
        bool ok = false;
        const auto value = text.toULongLong(&ok);
        if (!ok || value == 0 || value > maximum_risers ||
            text != QString::number(value)) {
            fail(QStringLiteral("%1 must be an integer from 1 to 10000.").arg(label));
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    }

    std::optional<Vec3> read_coordinate(const char* x_name, const char* y_name,
                                         const char* z_name, QString label,
                                         Vec3 fallback) {
        const auto x = read_length(x_name, label + QStringLiteral(" X"), false, fallback.x);
        const auto y = read_length(y_name, label + QStringLiteral(" Y"), false, fallback.y);
        const auto z = read_length(z_name, label + QStringLiteral(" Z"), false, fallback.z);
        if (!x.has_value() || !y.has_value() || !z.has_value()) {
            return std::nullopt;
        }
        return Vec3{*x, *y, *z};
    }

    std::optional<StairLanding> read_landing(const StairFlight* fallback) {
        if (landing_check == nullptr) {
            fail(QStringLiteral("Stair landing control is unavailable."));
            return std::nullopt;
        }
        if (!landing_check->isChecked()) {
            return std::nullopt;
        }
        const auto fallback_depth = fallback != nullptr && fallback->top_landing.has_value()
                                         ? fallback->top_landing->depth
                                         : 1.2;
        const auto fallback_thickness = fallback != nullptr && fallback->top_landing.has_value()
                                            ? fallback->top_landing->thickness
                                            : 0.15;
        const auto depth = read_length("buildingObjectLandingDepth", "Landing depth", true,
                                      fallback_depth);
        const auto thickness = read_length("buildingObjectLandingThickness",
                                           "Landing thickness", true, fallback_thickness);
        if (!depth.has_value() || !thickness.has_value()) {
            return std::nullopt;
        }
        return StairLanding{*depth, *thickness};
    }

    std::optional<std::string> read_identifier(const char* name, QString label,
                                               std::string fallback) {
        if (original_entity.has_value() && !dirty.contains(name)) {
            return fallback;
        }
        const auto found = fields.find(name);
        if (found == fields.end()) {
            fail(QStringLiteral("%1 is unavailable.").arg(label));
            return std::nullopt;
        }
        const auto value = found->second->text().trimmed().toUtf8().toStdString();
        if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos) {
            fail(QStringLiteral("%1 must be a non-empty stable ID.").arg(label));
            return std::nullopt;
        }
        return value;
    }

    std::optional<StairLevelConnection> read_level_connection(
        const StairFlight* fallback) {
        if (level_connection_check == nullptr) {
            fail(QStringLiteral("Stair level connection control is unavailable."));
            return std::nullopt;
        }
        if (!level_connection_check->isChecked()) {
            return std::nullopt;
        }
        if (original_entity.has_value() &&
            !dirty.contains("buildingObjectLevelConnectionEnabled") &&
            !dirty.contains("buildingObjectLevelGraph") &&
            !dirty.contains("buildingObjectLevelLink") &&
            !dirty.contains("buildingObjectLowerLevel") &&
            !dirty.contains("buildingObjectUpperLevel")) {
            if (fallback != nullptr && fallback->level_connection.has_value()) {
                return fallback->level_connection;
            }
            fail(QStringLiteral("Level connection IDs are required."));
            return std::nullopt;
        }
        const auto graph = read_identifier("buildingObjectLevelGraph",
                                           QStringLiteral("Level graph"),
                                           fallback != nullptr && fallback->level_connection
                                               ? fallback->level_connection->graph_entity_id
                                               : std::string());
        const auto link = read_identifier("buildingObjectLevelLink",
                                          QStringLiteral("Floor-to-floor link"),
                                          fallback != nullptr && fallback->level_connection
                                              ? fallback->level_connection->link_id
                                              : std::string());
        const auto lower = read_identifier("buildingObjectLowerLevel",
                                           QStringLiteral("Lower level"),
                                           fallback != nullptr && fallback->level_connection
                                               ? fallback->level_connection->lower_level_id
                                               : std::string());
        const auto upper = read_identifier("buildingObjectUpperLevel",
                                           QStringLiteral("Upper level"),
                                           fallback != nullptr && fallback->level_connection
                                               ? fallback->level_connection->upper_level_id
                                               : std::string());
        if (!graph.has_value() || !link.has_value() || !lower.has_value() ||
            !upper.has_value()) {
            return std::nullopt;
        }
        if (*lower == *upper) {
            fail(QStringLiteral("Lower and upper levels must differ."));
            return std::nullopt;
        }
        return StairLevelConnection{*graph, *link, *lower, *upper};
    }

    std::optional<BuildingObject> read_object() {
        const auto form = form_string(form_combo->currentData().toString());
        if (roof_openings_table->rowCount() != 0 &&
            (form_info(form) == nullptr || form_info(form)->type != "roof")) {
            fail(QStringLiteral("Remove the roof openings or choose a roof form before submitting."));
            return std::nullopt;
        }
        if (form == "rectangular_column") {
            const auto* fallback = original_as<RectangularColumn>();
            const auto base = read_coordinate(
                "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                QStringLiteral("Base"), fallback != nullptr ? fallback->base_center : Vec3{});
            const auto width = read_length("buildingObjectWidth", QStringLiteral("Width"),
                                           true, fallback != nullptr ? fallback->width : 0.4);
            const auto depth = read_length("buildingObjectDepth", QStringLiteral("Depth"),
                                           true, fallback != nullptr ? fallback->depth : 0.4);
            const auto height = read_length("buildingObjectHeight", QStringLiteral("Height"),
                                            true, fallback != nullptr ? fallback->height : 3.0);
            const auto rotation = read_angle(
                "buildingObjectOrientationDegrees", QStringLiteral("Rotation"),
                fallback != nullptr ? fallback->rotation_radians : 0.0);
            if (!base.has_value() || !width.has_value() || !depth.has_value() ||
                !height.has_value() || !rotation.has_value()) {
                return std::nullopt;
            }
            return RectangularColumn{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *width, *depth, *height, *rotation};
        }
        if (form == "circular_column") {
            const auto* fallback = original_as<CircularColumn>();
            const auto base = read_coordinate(
                "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                QStringLiteral("Base"), fallback != nullptr ? fallback->base_center : Vec3{});
            const auto radius = read_length("buildingObjectRadius", QStringLiteral("Radius"),
                                            true, fallback != nullptr ? fallback->radius : 0.2);
            const auto height = read_length("buildingObjectHeight", QStringLiteral("Height"),
                                            true, fallback != nullptr ? fallback->height : 3.0);
            if (!base.has_value() || !radius.has_value() || !height.has_value()) {
                return std::nullopt;
            }
            return CircularColumn{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *radius, *height, fallback != nullptr ? fallback->rotation_radians : 0.0};
        }
        if (form == "straight_beam") {
            const auto* fallback = original_as<Beam>();
            const auto start = read_coordinate(
                "buildingObjectStartX", "buildingObjectStartY", "buildingObjectStartZ",
                QStringLiteral("Start"), fallback != nullptr ? fallback->start : Vec3{0, 0, 2.4});
            const auto end = read_coordinate(
                "buildingObjectEndX", "buildingObjectEndY", "buildingObjectEndZ",
                QStringLiteral("End"), fallback != nullptr ? fallback->end : Vec3{3, 0, 2.4});
            const auto up_x = read_scalar("buildingObjectUpX", "Up X",
                                         fallback != nullptr ? fallback->up.x : 0.0);
            const auto up_y = read_scalar("buildingObjectUpY", "Up Y",
                                         fallback != nullptr ? fallback->up.y : 0.0);
            const auto up_z = read_scalar("buildingObjectUpZ", "Up Z",
                                         fallback != nullptr ? fallback->up.z : 1.0);
            const auto width = read_length("buildingObjectWidth", QStringLiteral("Width"),
                                           true, fallback != nullptr ? fallback->width : 0.2);
            const auto depth = read_length("buildingObjectDepth", QStringLiteral("Depth"),
                                           true, fallback != nullptr ? fallback->depth : 0.3);
            if (!start.has_value() || !end.has_value() || !up_x.has_value() ||
                !up_y.has_value() || !up_z.has_value() || !width.has_value() ||
                !depth.has_value()) {
                return std::nullopt;
            }
            return Beam{
                original_entity.has_value() ? original_entity->id : std::string{},
                *start, *end, {*up_x, *up_y, *up_z}, *width, *depth};
        }
        if (form == "straight_stair_flight" || form == "multi_flight_stair") {
            const auto* fallback = original_as<StairFlight>();
            const auto base = read_coordinate(
                "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                QStringLiteral("Base"), fallback != nullptr ? fallback->base_position : Vec3{});
            const auto orientation = read_angle(
                "buildingObjectOrientationDegrees", QStringLiteral("Orientation"),
                fallback != nullptr ? fallback->orientation_radians : 0.0);
            const auto risers = read_integer("buildingObjectRiserCount", QStringLiteral("Risers"),
                                             fallback != nullptr ? fallback->riser_count : 10);
            const auto total_rise = read_length(
                "buildingObjectTotalRise", QStringLiteral("Total rise"), true,
                fallback != nullptr ? fallback->total_rise : 2.5);
            const auto going = read_length("buildingObjectGoing", QStringLiteral("Going"), true,
                                          fallback != nullptr ? fallback->going : 0.25);
            const auto width = read_length("buildingObjectWidth", QStringLiteral("Width"), true,
                                           fallback != nullptr ? fallback->width : 1.2);
            const auto landing = read_landing(fallback);
            const auto level_connection = read_level_connection(fallback);
            if (!base.has_value() || !orientation.has_value() || !risers.has_value() ||
                !total_rise.has_value() || !going.has_value() || !width.has_value()) {
                return std::nullopt;
            }
            if (landing_check->isChecked() && !landing.has_value()) {
                return std::nullopt;
            }
            if (level_connection_check->isChecked() && !level_connection.has_value()) {
                return std::nullopt;
            }
            StairFlight result{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *orientation, *risers, *total_rise, *going, *width, landing,
                level_connection};
            if (stair_flights && !read_stair_topology(result)) return std::nullopt;
            return result;
        }
        if (form == "stair_landing_railing") {
            auto host = selected_landing_host();
            if (!host || !host_edge_combo || host_edge_combo->currentIndex() < 0 ||
                !host_interval_combo || host_interval_combo->currentIndex() < 0) {
                fail(QStringLiteral("Choose a current stair, landing, exposed edge and available coverage.")); return std::nullopt;
            }
            const auto* fallback = original_as<Railing>();
            const auto height = read_length("buildingObjectHeight", "Height", true, fallback ? fallback->height : 1.1);
            const auto thickness = read_length("buildingObjectThickness", "Thickness", true, fallback ? fallback->thickness : 0.08);
            const auto spacing = read_length("buildingObjectPostSpacing", "Maximum post spacing", true, fallback ? fallback->post_spacing : 0.9);
            const auto fraction = [&](const char* name, const char* label, bool end) -> std::optional<double> {
                if (fallback && fallback->landing_host && !dirty.contains(name))
                    return end ? fallback->landing_host->end_fraction : fallback->landing_host->start_fraction;
                const auto percent = read_scalar(name, QString::fromLatin1(label), end ? 100.0 : 0.0);
                if (!percent) return std::nullopt;
                return *percent / 100.0;
            };
            const auto start = fraction("buildingObjectHostStartPercent", "Start coverage (%)", false);
            const auto end = fraction("buildingObjectHostEndPercent", "End coverage (%)", true);
            if (!height || !thickness || !spacing || !start || !end) return std::nullopt;
            host->edge_index = static_cast<std::size_t>(host_edge_combo->currentData().toInt());
            host->start_fraction = *start; host->end_fraction = *end;
            // Re-read the actual source and witnesses before building. The helper
            // rejects blocked contact edges and changed ordered topology.
            (void)derive_stair_landing_edge(selected_landing_stair(), *host);
            Railing result{original_entity ? original_entity->id : std::string{}, {}, 0, 0, *height, *thickness, *spacing};
            result.landing_host = *host;
            validate_railing(result);
            return result;
        }
        if (form == "stair_flight_railing") {
            const auto* fallback = original_as<Railing>();
            if (!host_combo || host_combo->currentData().toString().isEmpty() ||
                host_flight_combo->currentData().toString().isEmpty()) {
                fail(QStringLiteral("Choose a stair and a flight.")); return std::nullopt;
            }
            const auto height = read_length("buildingObjectHeight", "Height", true, fallback ? fallback->height : 1.1);
            const auto thickness = read_length("buildingObjectThickness", "Thickness", true, fallback ? fallback->thickness : 0.08);
            const auto spacing = read_length("buildingObjectPostSpacing", "Maximum post spacing", true, fallback ? fallback->post_spacing : 0.9);
            const auto start = read_scalar("buildingObjectHostStart", "Start fraction", fallback && fallback->host ? fallback->host->start_fraction : 0.0);
            const auto end = read_scalar("buildingObjectHostEnd", "End fraction", fallback && fallback->host ? fallback->host->end_fraction : 1.0);
            if (!height || !thickness || !spacing || !start || !end) return std::nullopt;
            Railing result{original_entity ? original_entity->id : std::string{}, {}, 0, 0, *height, *thickness, *spacing};
            result.host = StairRailingHost{host_combo->currentData().toString().toStdString(),
                host_flight_combo->currentData().toString().toStdString(),
                host_side_combo->currentIndex() == 0 ? StairRailingSide::left : StairRailingSide::right, *start, *end};
            validate_railing(result);
            return result;
        }
        if (form == "straight_railing") {
            const auto* fallback = original_as<Railing>();
            const auto base = read_coordinate(
                "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                QStringLiteral("Base"), fallback != nullptr ? fallback->base_position : Vec3{});
            const auto orientation = read_angle(
                "buildingObjectOrientationDegrees", QStringLiteral("Orientation"),
                fallback != nullptr ? fallback->orientation_radians : 0.0);
            const auto length = read_length("buildingObjectLength", QStringLiteral("Length"), true,
                                           fallback != nullptr ? fallback->length : 3.0);
            const auto height = read_length("buildingObjectHeight", QStringLiteral("Height"), true,
                                           fallback != nullptr ? fallback->height : 1.1);
            const auto thickness = read_length("buildingObjectThickness", QStringLiteral("Thickness"), true,
                                               fallback != nullptr ? fallback->thickness : 0.08);
            const auto spacing = read_length("buildingObjectPostSpacing", QStringLiteral("Post spacing"), true,
                                             fallback != nullptr ? fallback->post_spacing : 0.9);
            if (!base.has_value() || !orientation.has_value() || !length.has_value() ||
                !height.has_value() || !thickness.has_value() || !spacing.has_value()) {
                return std::nullopt;
            }
            return Railing{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *orientation, *length, *height, *thickness, *spacing};
        }
        if (form == "sloped_roof_panel") {
            const auto* fallback = original_as<SlopedRoofPanel>();
            const auto base = read_coordinate(
                "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                QStringLiteral("Base"), fallback != nullptr ? fallback->base_position : Vec3{0, 0, 3});
            const auto orientation = read_angle(
                "buildingObjectOrientationDegrees", QStringLiteral("Orientation"),
                fallback != nullptr ? fallback->orientation_radians : 0.0);
            const auto run = read_length("buildingObjectRun", QStringLiteral("Run"), true,
                                         fallback != nullptr ? fallback->run : 4.0);
            const auto span = read_length("buildingObjectSpan", QStringLiteral("Span"), true,
                                          fallback != nullptr ? fallback->span : 3.0);
            const auto rise = read_length("buildingObjectRise", QStringLiteral("Rise"), false,
                                         fallback != nullptr ? fallback->rise : 1.0);
            const auto overhang = read_length("buildingObjectOverhang",
                                              QStringLiteral("Overhang"), false,
                                              fallback != nullptr ? fallback->overhang : 0.2);
            const auto thickness = read_length("buildingObjectThickness",
                                               QStringLiteral("Thickness"), true,
                                               fallback != nullptr ? fallback->thickness : 0.1);
            if (!base.has_value() || !orientation.has_value() || !run.has_value() ||
                !span.has_value() || !rise.has_value() || !overhang.has_value() ||
                !thickness.has_value()) {
                return std::nullopt;
            }
            if (*rise < 0.0) {
                fail(QStringLiteral("Rise must be zero or greater."));
                return std::nullopt;
            }
            const auto pitch = fallback != nullptr &&
                                       *run == fallback->run && *rise == fallback->rise
                                   ? fallback->pitch_radians
                                   : std::atan2(*rise, *run);
            const auto openings = read_roof_openings();
            if (!openings) return std::nullopt;
            return SlopedRoofPanel{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *orientation, *run, *span, *rise, pitch, *overhang, *thickness,
                *openings};
        }
        if (form == "gable_roof" || form == "hip_roof") {
            const auto read_roof = [&]<typename Roof>() -> std::optional<BuildingObject> {
                const auto* fallback = original_as<Roof>();
                const auto base = read_coordinate(
                    "buildingObjectBaseX", "buildingObjectBaseY", "buildingObjectBaseZ",
                    QStringLiteral("Base"), fallback != nullptr ? fallback->base_position : Vec3{0, 0, 3});
                const auto orientation = read_angle(
                    "buildingObjectOrientationDegrees", QStringLiteral("Orientation"),
                    fallback != nullptr ? fallback->orientation_radians : 0.0);
                const auto length = read_length("buildingObjectLength", QStringLiteral("Length"), true,
                                                fallback != nullptr ? fallback->length : 5.0);
                const auto span = read_length("buildingObjectSpan", QStringLiteral("Span"), true,
                                              fallback != nullptr ? fallback->span : 4.0);
                const auto rise = read_length("buildingObjectRise", QStringLiteral("Rise"), true,
                                              fallback != nullptr ? fallback->rise : 1.0);
                const auto overhang = read_length("buildingObjectOverhang",
                                                  QStringLiteral("Overhang"), false,
                                                  fallback != nullptr ? fallback->overhang : 0.2);
                const auto thickness = read_length("buildingObjectThickness",
                                                   QStringLiteral("Thickness"), true,
                                                   fallback != nullptr ? fallback->thickness : 0.1);
                if (!base.has_value() || !orientation.has_value() || !length.has_value() ||
                    !span.has_value() || !rise.has_value() || !overhang.has_value() ||
                    !thickness.has_value()) {
                    return std::nullopt;
                }
                const auto pitch = fallback != nullptr &&
                                           *span == fallback->span && *rise == fallback->rise
                                       ? fallback->pitch_radians
                                       : std::atan2(*rise, *span * 0.5);
                const auto openings = read_roof_openings();
                if (!openings) return std::nullopt;
                return Roof{
                    original_entity.has_value() ? original_entity->id : std::string{},
                    *base, *orientation, *length, *span, *rise, pitch, *overhang, *thickness,
                    *openings};
            };
            return form == "hip_roof" ? read_roof.template operator()<HipRoof>()
                                      : read_roof.template operator()<GableRoof>();
        }
        fail(QStringLiteral("Choose a supported building form."));
        return std::nullopt;
    }

    json build_quantity_entries(const json& canonical_properties) const {
        json entries = json::object();
        // Keep caller-provided records byte-for-byte unless their canonical
        // field was actually edited or removed. Unknown pointers and future
        // record versions remain opaque, non-authoritative metadata; display
        // code validates a record before allowing it to replace a field's
        // current canonical value.
        for (const auto& [pointer, receipt] : original_quantity_entries) {
            entries[pointer] = receipt;
        }

        for (const auto& [field, pointer] : quantity_pointers) {
            (void)field;
            const auto* canonical = json_at_pointer(canonical_properties, pointer);
            const auto parsed = parsed_quantities.find(pointer);
            if (parsed != parsed_quantities.end()) {
                const auto original = original_quantity_entries.find(pointer);
                const bool same_roof_form = original_entity && original_entity->type == "roof" &&
                    original_entity->properties.value("form", std::string{}) ==
                        canonical_properties.value("form", std::string{});
                if (same_roof_form && original != original_quantity_entries.end() &&
                    !decode_quantity_receipt(original->second)) {
                    throw std::invalid_argument("The edited roof quantity has an unsupported receipt.");
                }
                if (canonical != nullptr && canonical->is_number()) {
                    try {
                        const auto authoritative = canonical->get<double>();
                        if (std::isfinite(authoritative) &&
                            parsed->second.metres == authoritative) {
                            auto updated = quantity_receipt_json(parsed->second);
                            if (same_roof_form && original != original_quantity_entries.end()) {
                                auto retained = original->second;
                                auto exact = retained.at("exact_metres");
                                exact.update(updated.at("exact_metres"));
                                retained.update(updated);
                                retained["exact_metres"] = std::move(exact);
                                updated = std::move(retained);
                            }
                            entries[pointer] = std::move(updated);
                        } else {
                            entries.erase(pointer);
                        }
                    } catch (const std::exception&) {
                        entries.erase(pointer);
                    }
                } else {
                    entries.erase(pointer);
                }
                continue;
            }

            // A field with no parsed quantity was untouched in an existing
            // object. Preserve its original record, including stale or
            // unsupported records, as opaque metadata. If the canonical field
            // no longer exists (for example, a landing was removed), discard
            // its receipt because it no longer has a value to describe.
            if (canonical == nullptr || !canonical->is_number()) {
                entries.erase(pointer);
                continue;
            }
        }
        return entries;
    }

    void apply_quantity_entries(json& target, const json& canonical_properties) const {
        const auto entries = build_quantity_entries(canonical_properties);
        if (entries.empty()) {
            target.erase("quantity_entries");
        } else {
            target["quantity_entries"] = entries;
        }
    }

    void fail(const QString& message) {
        error = message;
        if (error_label != nullptr) {
            error_label->setText(message);
            error_label->setVisible(true);
        }
    }

    void clear_error() {
        error.clear();
        if (error_label != nullptr) {
            error_label->clear();
            error_label->setVisible(false);
        }
    }

    BuildingObjectDialog* owner{};
    std::optional<Entity> original_entity;
    std::optional<DocumentSnapshot> source_snapshot;
    std::optional<BuildingObject> original_object;
    std::optional<Entity> candidate_entity;
    std::vector<Entity> related_entities;
    std::map<std::string, Entity, std::less<>> upgraded_hosts;
    bool metric{};
    bool loading{};
    bool original_invalid{};
    bool roof_openings_changed{};
    QString error;
    json original_roof_opening_receipts = json::object();
    json roof_opening_receipts = json::object();
    RoofOpeningEditIntent roof_opening_intent;

    QComboBox* type_combo{};
    QComboBox* form_combo{};
    QLabel* error_label{};
    QLabel* derived_pitch{};
    QScrollArea* scroll{};
    QWidget* form_body{};
    QStackedWidget* form_stack{};
    QWidget* form_page{};
    QDialogButtonBox* buttons{};
    QPushButton* submit_button{};
    QCheckBox* landing_check{};
    QTableWidget* stair_flights{};
    QTableWidget* stair_landings{};
    QLabel* stair_summary{};
    QComboBox* host_combo{};
    QComboBox* host_flight_combo{};
    QComboBox* host_side_combo{};
    QComboBox* host_landing_combo{};
    QComboBox* host_edge_combo{};
    QComboBox* host_interval_combo{};
    QLabel* landing_summary{};
    QLabel* railing_preview{};
    QLabel* railing_preview_status{};
    std::vector<StairLandingRailingHost> landing_choices;
    std::vector<StairLandingEdgeInterval> landing_intervals;
    QCheckBox* level_connection_check{};
    QGroupBox* roof_openings_group{};
    QLabel* roof_openings_help{};
    QTableWidget* roof_openings_table{};
    std::map<std::string, QLineEdit*, std::less<>> fields;
    std::map<std::string, std::string, std::less<>> quantity_pointers;
    std::map<std::string, json, std::less<>> original_quantity_entries;
    std::map<std::string, Quantity, std::less<>> parsed_quantities;
    std::map<std::string, bool, std::less<>> dirty;
};

BuildingObjectDialog::BuildingObjectDialog(std::optional<Entity> original,
                                           bool metricUnits, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(original), metricUnits)) {}

BuildingObjectDialog::~BuildingObjectDialog() = default;

BuildingObjectDialog::BuildingObjectDialog(const DocumentSnapshot& source,
                                           std::optional<Entity> original,
                                           bool metricUnits, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(original), metricUnits, source)) {}

std::optional<Entity> BuildingObjectDialog::candidate() const {
    return m_impl->candidate();
}

std::vector<Entity> BuildingObjectDialog::relatedCandidates() const { return m_impl->related_candidates(); }

std::vector<Entity> BuildingObjectDialog::coordinatedCandidates() const {
    auto result = relatedCandidates();
    if (const auto primary = candidate()) result.push_back(*primary);
    return result;
}

bool BuildingObjectDialog::submit() { return m_impl->submit(); }

QString BuildingObjectDialog::lastError() const { return m_impl->last_error(); }

}  // namespace sketch::desktop
