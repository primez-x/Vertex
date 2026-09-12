#include "sketch/desktop/building_object_dialog.hpp"

#include "sketch/quantity.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
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

namespace sketch::desktop {
namespace {

using json = nlohmann::json;

constexpr double geometry_tolerance = 1e-7;
constexpr std::size_t maximum_risers = 10'000;

struct FormInfo {
    std::string_view type;
    std::string_view form;
    std::string_view label;
};

constexpr std::array<FormInfo, 8> form_infos{{
    {"column", "rectangular_column", "Rectangular column"},
    {"column", "circular_column", "Circular column"},
    {"beam", "straight_beam", "Straight beam"},
    {"stair", "straight_stair_flight", "Straight stair flight"},
    {"railing", "straight_railing", "Straight railing"},
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
         bool metric_units)
        : owner(dialog), original_entity(std::move(original)), metric(metric_units) {
        setup();
    }

    [[nodiscard]] std::optional<Entity> candidate() const { return candidate_entity; }

    [[nodiscard]] QString last_error() const { return error; }

    bool submit() {
        candidate_entity.reset();
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
            if (original_entity.has_value()) {
                const auto canonical = encode_building_entity(*object,
                                                              original_entity->extensions);
                auto merged = *original_entity;
                merged.type = canonical.type;
                merged.properties.update(canonical.properties);
                if (merged.type == "roof" && !canonical.properties.contains("roof_openings"))
                    merged.properties.erase("roof_openings");
                apply_quantity_entries(merged.properties, canonical.properties);
                candidate_entity = std::move(merged);
            } else {
                candidate_entity = encode_building_entity(*object);
                apply_quantity_entries(candidate_entity->properties,
                                       candidate_entity->properties);
            }
            clear_error();
            owner->accept();
            return true;
        } catch (const std::exception& caught) {
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
                        return "straight_stair_flight";
                    } else if constexpr (std::is_same_v<Object, Railing>) {
                        return "straight_railing";
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
        fields.clear();
        quantity_pointers.clear();
        dirty.clear();
        parsed_quantities.clear();
        landing_check = nullptr;
        derived_pitch = nullptr;
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
        } else if (form == "straight_stair_flight") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
            add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
            add_integer_field(layout, "Risers", "buildingObjectRiserCount", 10);
            add_quantity_field(layout, "Total rise", "buildingObjectTotalRise", 2.5);
            add_quantity_field(layout, "Going", "buildingObjectGoing", 0.25);
            add_quantity_field(layout, "Width", "buildingObjectWidth", 1.2);
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
        } else if (form == "straight_railing") {
            add_coordinate_fields(layout, "Base", "buildingObjectBaseX",
                                  "buildingObjectBaseY", "buildingObjectBaseZ", {});
            add_angle_field(layout, "Orientation (degrees)",
                            "buildingObjectOrientationDegrees", 0.0);
            add_quantity_field(layout, "Length", "buildingObjectLength", 3.0);
            add_quantity_field(layout, "Height", "buildingObjectHeight", 1.1);
            add_quantity_field(layout, "Thickness", "buildingObjectThickness", 0.08);
            add_quantity_field(layout, "Post spacing", "buildingObjectPostSpacing", 0.9);
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
        } else if (form == "straight_stair_flight" || form == "straight_railing" ||
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
                } else if constexpr (std::is_same_v<Object, Railing>) {
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

    std::optional<BuildingObject> read_object() {
        const auto form = form_string(form_combo->currentData().toString());
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
                *base, *radius, *height};
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
        if (form == "straight_stair_flight") {
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
            if (!base.has_value() || !orientation.has_value() || !risers.has_value() ||
                !total_rise.has_value() || !going.has_value() || !width.has_value()) {
                return std::nullopt;
            }
            if (landing_check->isChecked() && !landing.has_value()) {
                return std::nullopt;
            }
            return StairFlight{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *orientation, *risers, *total_rise, *going, *width, landing};
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
                                       !dirty.contains("buildingObjectRun") &&
                                       !dirty.contains("buildingObjectRise")
                                   ? fallback->pitch_radians
                                   : std::atan(*rise / *run);
            return SlopedRoofPanel{
                original_entity.has_value() ? original_entity->id : std::string{},
                *base, *orientation, *run, *span, *rise, pitch, *overhang, *thickness,
                fallback ? fallback->openings : std::vector<RoofOpening>{}};
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
                                           !dirty.contains("buildingObjectSpan") &&
                                           !dirty.contains("buildingObjectRise")
                                       ? fallback->pitch_radians
                                       : std::atan(*rise / (*span * 0.5));
                return Roof{
                    original_entity.has_value() ? original_entity->id : std::string{},
                    *base, *orientation, *length, *span, *rise, pitch, *overhang, *thickness,
                    fallback ? fallback->openings : std::vector<RoofOpening>{}};
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
                if (canonical != nullptr && canonical->is_number()) {
                    try {
                        const auto authoritative = canonical->get<double>();
                        if (std::isfinite(authoritative) &&
                            parsed->second.metres == authoritative) {
                            entries[pointer] = quantity_receipt_json(parsed->second);
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
    std::optional<BuildingObject> original_object;
    std::optional<Entity> candidate_entity;
    bool metric{};
    bool loading{};
    bool original_invalid{};
    QString error;

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

std::optional<Entity> BuildingObjectDialog::candidate() const {
    return m_impl->candidate();
}

bool BuildingObjectDialog::submit() { return m_impl->submit(); }

QString BuildingObjectDialog::lastError() const { return m_impl->last_error(); }

}  // namespace sketch::desktop
